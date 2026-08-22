#include "MultisamplerWaveformLcd.h"
#include "DysektLookAndFeel.h"
#include "SliceControlBar.h"
#include "../PluginProcessor.h"
#include <cmath>

// ── Theme helpers ─────────────────────────────────────────────────────────────
static juce::Colour msLcdBg()       { return getTheme().darkBar.darker (0.55f); }
static juce::Colour msLcdPhosphor() { return getTheme().accent; }
static juce::Colour msLcdDim()      { return getTheme().accent.withAlpha (0.15f).overlaidWith (msLcdBg()); }

// Toxic-Candy node colours — match SliceWaveformLcd so the two panels read
// as the same instrument family when switching tabs.
static const juce::Colour kColAttack  { 0xFF00FF87 }; // Toxic Lime
static const juce::Colour kColDecay   { 0xFFFFE800 }; // Radioactive Yellow
static const juce::Colour kColSustain { 0xFF00C8FF }; // Ice Blue
static const juce::Colour kColRelease { 0xFFFF6B00 }; // Molten Orange

// ── Constructor ───────────────────────────────────────────────────────────────

MultisamplerWaveformLcd::MultisamplerWaveformLcd (DysektProcessor& p)
    : processor (p)
{
    setOpaque (false);
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void MultisamplerWaveformLcd::resized()
{
    screenArea = getLocalBounds().reduced (4).toFloat();
}

// ── Source ────────────────────────────────────────────────────────────────────

void MultisamplerWaveformLcd::setSource (const MultisamplerInstrument* instr, int selectedZoneIndex)
{
    instrument   = instr;
    lastSetIndex = selectedZoneIndex;

    if (instrument != nullptr
        && selectedZoneIndex >= 0
        && selectedZoneIndex < (int) instrument->zones.size())
    {
        selectedZoneId = instrument->zones[(size_t) selectedZoneIndex].id;
    }
    else
    {
        selectedZoneId = juce::Uuid::null();
    }

    forceRebuild = true;
    repaint();
}

void MultisamplerWaveformLcd::clearSource()
{
    setSource (nullptr, -1);
}

// ── Zone resolution ───────────────────────────────────────────────────────────

const SampleZone* MultisamplerWaveformLcd::resolveSelectedZone (int& outIndex) const
{
    outIndex = -1;
    if (instrument == nullptr || selectedZoneId == juce::Uuid::null())
        return nullptr;

    const auto& zones = instrument->zones;
    for (size_t i = 0; i < zones.size(); ++i)
    {
        if (zones[i].id == selectedZoneId)
        {
            outIndex = (int) i;
            return &zones[i];
        }
    }
    return nullptr; // zone was deleted elsewhere since setSource()
}

void MultisamplerWaveformLcd::decodeZoneFile (const juce::File& f)
{
    if (f == decodedFile)
        return; // already decoded this exact file

    decodedFile = f;
    zoneSampleData.clear();

    if (f.existsAsFile())
        zoneSampleData.loadFromFile (f, processor.getSampleRate() > 0.0
                                             ? processor.getSampleRate() : 44100.0);
}

// ── Data building ─────────────────────────────────────────────────────────────

void MultisamplerWaveformLcd::buildDisplayData()
{
    data = {};

    if (instrument == nullptr)
        return; // hasInstrument stays false — drawNoData() handles it

    data.hasInstrument = true;

    if (instrument->zones.empty())
        return; // hasZones stays false

    data.hasZones = true;

    int idx = -1;
    const SampleZone* z = resolveSelectedZone (idx);
    if (z == nullptr)
        return; // hasSelection stays false — "-- SELECT A ZONE --"

    data.hasSelection = true;
    data.zoneIndex     = idx;
    data.sampleName    = z->sampleFile.getFileName();
    data.sampleRate    = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 44100.0;

    decodeZoneFile (z->sampleFile);

    data.missingSample = z->hasMissingSample() || zoneSampleData.getNumFrames() <= 0;
    if (data.missingSample)
        return;

    data.totalFrames = zoneSampleData.getNumFrames();
    const auto resolved = resolveSampleRange (*z, data.totalFrames);
    if (resolved.length() <= 0)
    {
        data.missingSample = true;
        return;
    }
    data.startSample = (int) resolved.start;
    data.endSample   = (int) resolved.end;
}

// ── Timer-driven repaint ──────────────────────────────────────────────────────

void MultisamplerWaveformLcd::repaintLcd()
{
    // Cheap: resolves the selected zone and re-decodes only if its file
    // path actually changed. Runs before the rebuild checks below so a
    // zone/file/range change is seen on the same tick it happens.
    buildDisplayData();

    int idx = -1;
    const SampleZone* z = resolveSelectedZone (idx);
    const juce::Uuid curId = (z != nullptr) ? z->id : juce::Uuid();

    const bool zoneChanged  = (curId != lastBuiltZoneId);
    const bool fileChanged  = (z != nullptr) && (z->sampleFile != lastBuiltSampleFile);
    const bool rangeChanged = (z != nullptr) && (z->sampleStart != lastBuiltSampleStart
                                                   || z->sampleEnd != lastBuiltSampleEnd);
    const bool viewChanged  = (std::abs (zoom - lastBuiltZoom) > 0.0001f
                                || std::abs (scroll - lastBuiltScroll) > 0.0001f);
    const bool missingChanged = (data.missingSample != lastBuiltMissing);
    const bool needsRebuild = forceRebuild || zoneChanged || fileChanged
                               || rangeChanged || viewChanged || missingChanged;

    if (dragRole == NodeRole::None)
    {
        if (postCommitGuard > 0)
        {
            --postCommitGuard;
        }
        else if (zoneChanged || forceRebuild)
        {
            buildEnvelopeNodes();
        }

        if (needsRebuild)
        {
            buildWaveformPeaks();
            lastBuiltZoneId      = curId;
            lastBuiltSampleFile  = (z != nullptr) ? z->sampleFile   : juce::File();
            lastBuiltSampleStart = (z != nullptr) ? z->sampleStart  : -1;
            lastBuiltSampleEnd   = (z != nullptr) ? z->sampleEnd    : -1;
            lastBuiltZoom        = zoom;
            lastBuiltScroll      = scroll;
            lastBuiltMissing     = data.missingSample;
            forceRebuild         = false;
        }
    }

    repaint();
}

// ── Envelope: read selected zone's ADSR → normalised env + envNodes ───────────

float MultisamplerWaveformLcd::getSliceDurMs() const
{
    static constexpr float kDefaultMs = 1000.0f;
    if (! data.hasSelection || data.missingSample)
        return kDefaultMs;

    const int len = data.endSample - data.startSample;
    if (len <= 0 || data.sampleRate <= 0.0)
        return kDefaultMs;

    return (float) len / (float) data.sampleRate * 1000.0f;
}

void MultisamplerWaveformLcd::buildEnvelopeNodes()
{
    int idx = -1;
    const SampleZone* z = resolveSelectedZone (idx);

    float attackMs = 0.0f, decayMs = 0.0f, sustainPc = 100.0f, releaseMs = 0.0f;
    if (z != nullptr)
    {
        attackMs  = z->attackSeconds  * 1000.0f;
        decayMs   = z->decaySeconds   * 1000.0f;
        sustainPc = z->sustainLevel   * 100.0f;
        releaseMs = z->releaseSeconds * 1000.0f;
    }

    static constexpr float kMin = 0.01f, kMax = 0.99f, kGap = 0.01f;

    // Calculated against the selected playable range, not blindly against
    // the full file — see getSliceDurMs()/computePlayableRange().
    const float kViewMs = juce::jmax (1.0f, getSliceDurMs());

    const float attackNorm  = std::sqrt (juce::jmin (attackMs  / kViewMs, 1.0f));
    const float decayNorm   = std::sqrt (juce::jmin (decayMs   / kViewMs, 1.0f));
    const float releaseNorm = std::sqrt (juce::jmin (releaseMs / kViewMs, 1.0f));

    const float ax_raw = kMin + attackNorm  * (kMax - kMin);
    const float rx_raw = (releaseMs < 0.5f)
                         ? kMax
                         : juce::jlimit (kMin, kMax, kMax - releaseNorm * (kMax - kMin));

    env.ax = juce::jlimit (kMin, kMax - 2.0f * kGap, ax_raw);
    env.rx = juce::jlimit (env.ax + 2.0f * kGap, kMax, rx_raw);

    const float dSpan = env.rx - env.ax - 2.0f * kGap;
    env.dx = juce::jlimit (env.ax + kGap,
                           env.rx - kGap,
                           env.ax + kGap + decayNorm * dSpan);

    env.sy    = juce::jlimit (0.04f, 0.94f, 1.0f - (sustainPc / 100.0f));
    env.ay    = 0.04f;
    env.sxEnd = env.rx;

    envNodes.clear();

    EnvNode a; a.xn = env.ax; a.yn = env.ay; a.role = NodeRole::Attack;
    a.colour = kColAttack; a.label = "A"; envNodes.add (a);

    EnvNode d; d.xn = env.dx; d.yn = env.sy; d.role = NodeRole::Decay;
    d.colour = kColDecay;  d.label = "D"; envNodes.add (d);

    EnvNode s;
    s.xn = (env.dx + env.sxEnd) * 0.5f; s.yn = env.sy;
    s.role = NodeRole::Sustain; s.colour = kColSustain; s.label = "S";
    envNodes.add (s);

    EnvNode r; r.xn = env.rx; r.yn = env.sy; r.role = NodeRole::Release;
    r.colour = kColRelease; r.label = "R"; envNodes.add (r);
}

// ── Commit: inverse-map env → onZoneParamEdited ────────────────────────────────

void MultisamplerWaveformLcd::commitNodes()
{
    static constexpr float kMin = 0.01f, kMax = 0.99f, kGap = 0.01f;

    const float kViewMs = juce::jmax (1.0f, getSliceDurMs());

    const float aRatio = (env.ax - kMin) / juce::jmax (0.001f, kMax - kMin);
    const float rRatio = (kMax - env.rx) / juce::jmax (0.001f, kMax - kMin);
    const float dSpan  = env.rx - env.ax - 2.0f * kGap;
    const float dRatio = (env.dx - (env.ax + kGap)) / juce::jmax (0.001f, dSpan);

    const float attackMs  = juce::jlimit (0.0f, kViewMs, aRatio * aRatio * kViewMs);
    const float decayMs   = juce::jlimit (0.0f, kViewMs, dRatio * dRatio * kViewMs);
    const float sustainPc = juce::jlimit (0.0f, 100.0f, (1.0f - env.sy) * 100.0f);
    const float releaseMs = juce::jlimit (0.0f, kViewMs, rRatio * rRatio * kViewMs);

    int idx = -1;
    resolveSelectedZone (idx);

    if (onZoneParamEdited && idx >= 0)
    {
        switch (dragRole)
        {
            case NodeRole::Attack:
                onZoneParamEdited (idx, SliceControlBar::ZoneAttack,  attackMs  / 1000.0f);
                break;
            case NodeRole::Decay:
                onZoneParamEdited (idx, SliceControlBar::ZoneDecay,   decayMs   / 1000.0f);
                break;
            case NodeRole::Sustain:
                onZoneParamEdited (idx, SliceControlBar::ZoneSustain, sustainPc / 100.0f);
                break;
            case NodeRole::Release:
                onZoneParamEdited (idx, SliceControlBar::ZoneRelease, releaseMs / 1000.0f);
                break;
            default: break;
        }
    }

    // Give MultisamplerEditor::applySliceControlBarFieldEdit time to echo
    // the new value (it calls refreshInspectorFromSelection() synchronously,
    // which re-enters via PluginEditor's onZoneSelectionOrEditChanged back
    // into setSource() — harmless: that only touches instrument/
    // selectedZoneId/the rebuild guards, never dragRole/env/envNodes, so it
    // can't disturb the drag in progress) before rebuilding from the model.
    postCommitGuard = 6;
    forceRebuild     = true;
}

// ── Hit testing ───────────────────────────────────────────────────────────────

MultisamplerWaveformLcd::NodeRole MultisamplerWaveformLcd::hitTest (juce::Point<float> pos) const
{
    if (screenArea.isEmpty()) return NodeRole::None;

    const float W  = screenArea.getWidth();
    const float H  = screenArea.getHeight();
    const float ox = screenArea.getX();
    const float oy = screenArea.getY();

    NodeRole best  = NodeRole::None;
    float    bestD2 = kHitR * kHitR;

    for (const auto& n : envNodes)
    {
        const float nx = ox + n.xn * W;
        const float ny = oy + n.yn * H;
        const float dx = pos.x - nx;
        const float dy = pos.y - ny;
        const float d2 = dx * dx + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; best = n.role; }
    }
    return best;
}

// ── Mouse ─────────────────────────────────────────────────────────────────────

void MultisamplerWaveformLcd::mouseMove (const juce::MouseEvent& e)
{
    const NodeRole newHov = hitTest (e.position);
    if (newHov != hovRole)
    {
        hovRole = newHov;
        setMouseCursor (hovRole != NodeRole::None
                            ? juce::MouseCursor::PointingHandCursor
                            : juce::MouseCursor::NormalCursor);
        repaint();
    }
}

void MultisamplerWaveformLcd::mouseDown (const juce::MouseEvent& e)
{
    // No lockMask equivalent exists on SampleZone — nothing to toggle on
    // right-click, matching the retired SfzWaveformLcd's "no lock in
    // SF-player mode" precedent and SliceWaveformLcd's old MULTISAMPLER
    // branch (right-click was a no-op there too).
    if (e.mods.isRightButtonDown()) return;

    // Editing requires a resolved, playable zone.
    if (! data.hasSelection || data.missingSample) return;

    dragRole = hitTest (e.position);
}

void MultisamplerWaveformLcd::mouseDrag (const juce::MouseEvent& e)
{
    if (dragRole == NodeRole::None || screenArea.isEmpty()) return;
    if (! data.hasSelection || data.missingSample) { dragRole = NodeRole::None; return; }

    const float W  = screenArea.getWidth();
    const float H  = screenArea.getHeight();
    const float ox = screenArea.getX();
    const float oy = screenArea.getY();

    const float xn = juce::jlimit (0.01f, 0.99f, (e.position.x - ox) / W);
    const float yn = juce::jlimit (0.02f, 0.98f, (e.position.y - oy) / H);

    static constexpr float kMin = 0.01f, kMax = 0.99f, kGap = 0.01f;

    if      (dragRole == NodeRole::Attack)
        env.ax = juce::jlimit (kMin, env.dx - kGap, xn);
    else if (dragRole == NodeRole::Decay)
        env.dx = juce::jlimit (env.ax + kGap, env.rx - kGap, xn);
    else if (dragRole == NodeRole::Sustain)
        env.sy = juce::jlimit (0.04f, 0.94f, yn);
    else if (dragRole == NodeRole::Release)
        env.rx = juce::jlimit (env.dx + kGap, kMax, xn);

    env.sxEnd = env.rx;

    // Rebuild envNodes from updated env.* (no model read during drag)
    envNodes.clear();
    EnvNode a; a.xn = env.ax; a.yn = env.ay; a.role = NodeRole::Attack;
    a.colour = kColAttack; a.label = "A"; envNodes.add (a);
    EnvNode d; d.xn = env.dx; d.yn = env.sy; d.role = NodeRole::Decay;
    d.colour = kColDecay;  d.label = "D"; envNodes.add (d);
    EnvNode s; s.xn = (env.dx + env.sxEnd) * 0.5f; s.yn = env.sy;
    s.role = NodeRole::Sustain; s.colour = kColSustain; s.label = "S"; envNodes.add (s);
    EnvNode r; r.xn = env.rx; r.yn = env.sy; r.role = NodeRole::Release;
    r.colour = kColRelease; r.label = "R"; envNodes.add (r);

    commitNodes();
    repaint();
}

void MultisamplerWaveformLcd::mouseUp (const juce::MouseEvent&)
{
    dragRole        = NodeRole::None;
    postCommitGuard = 6;
    repaint();
}

// ── Waveform backdrop ─────────────────────────────────────────────────────────

void MultisamplerWaveformLcd::buildWaveformPeaks()
{
    peaks.clearQuick();
    peaks.insertMultiple (-1, 0.0f, kPeaks);

    if (! data.hasSelection || data.missingSample) return;

    const int sliceLen = data.endSample - data.startSample;
    if (sliceLen <= 0) return;

    const float z           = juce::jmax (1.0f, zoom);
    const float windowFrac  = 1.0f / z;
    const float maxScroll   = (float) sliceLen * (1.0f - windowFrac);
    const int   startF      = data.startSample
                               + (int) juce::jlimit (0.0f, (float) juce::jmax (0, sliceLen - 1),
                                                      scroll * maxScroll);
    const int   endF        = (int) juce::jlimit ((float) startF + 1.0f, (float) data.endSample,
                                                    (float) startF + windowFrac * (float) sliceLen);

    windowStartFrame = startF;
    windowEndFrame   = endF;

    for (int i = 0; i < kPeaks; ++i)
    {
        const float t   = (float) i / (float) kPeaks;
        const int   pos = startF + (int) (t * (float) (endF - startF));
        peaks.set (i, DysektProcessor::getWaveformPeakAtIn (zoneSampleData, pos));
    }
}

// Envelope Y at normalised X (linear interpolation between nodes) ─────────────

float MultisamplerWaveformLcd::envAt (float xn) const
{
    const float kSEnd = env.sxEnd;
    struct Pt { float x, y; };
    const Pt pts[] = {
        { 0.0f,    1.0f    },
        { env.ax,  env.ay  },
        { env.dx,  env.sy  },
        { kSEnd,   env.sy  },
        { env.rx,  env.sy  },
        { 1.0f,    1.0f    }
    };
    constexpr int N = 6;

    for (int i = 0; i < N - 1; ++i)
    {
        if (xn >= pts[i].x && xn <= pts[i + 1].x)
        {
            const float span = pts[i + 1].x - pts[i].x;
            const float t = (span > 0.0f) ? (xn - pts[i].x) / span : 0.0f;
            return pts[i].y + t * (pts[i + 1].y - pts[i].y);
        }
    }
    return 1.0f;
}

void MultisamplerWaveformLcd::drawWaveform (juce::Graphics& g, const juce::Rectangle<float>& area)
{
    if (peaks.isEmpty()) return;

    const float cy = area.getCentreY();
    const float W  = area.getWidth();
    const float H  = area.getHeight();
    const int   n  = peaks.size();

    g.setColour (msLcdPhosphor().withAlpha (0.20f));
    g.drawHorizontalLine (juce::roundToInt (cy), area.getX(), area.getRight());

    int zoneIdx = -1;
    const SampleZone* z = resolveSelectedZone (zoneIdx);
    juce::Colour col = (z != nullptr && z->hasCustomColour)
                            ? juce::Colour (z->customColourArgb)
                            : msLcdPhosphor();

    std::vector<float> xs ((size_t) n), amps ((size_t) n);
    for (int i = 0; i < n; i++)
    {
        const float xn = (float) i / (float) n;
        xs[(size_t) i] = area.getX() + xn * W;
        const float eGain = 1.0f - envAt (xn);
        amps[(size_t) i] = juce::jlimit (0.0f, 1.0f, peaks[i]) * (H * 0.45f) * eGain;
    }

    juce::Path lineTop, lineBot, fill;
    for (int i = 0; i < n; i++)
    {
        const float yT = cy - amps[(size_t) i];
        const float yB = cy + amps[(size_t) i];
        if (i == 0) { lineTop.startNewSubPath (xs[0], yT); lineBot.startNewSubPath (xs[0], yB); }
        else        { lineTop.lineTo (xs[(size_t) i], yT); lineBot.lineTo (xs[(size_t) i], yB); }
    }
    fill = lineTop;
    for (int i = n - 1; i >= 0; i--)
        fill.lineTo (xs[(size_t) i], cy + amps[(size_t) i]);
    fill.closeSubPath();

    switch (waveformMode)
    {
        default:
        case 0: // Hard
        {
            g.setColour (col.withAlpha (0.12f));
            g.fillPath (fill);
            juce::PathStrokeType sharp (1.1f);
            g.setColour (col.withAlpha (0.85f));
            g.strokePath (lineTop, sharp);
            g.strokePath (lineBot, sharp);
            break;
        }
        case 1: // Soft
        {
            juce::ColourGradient grad (col.withAlpha (0.02f), 0.0f, area.getY(),
                                        col.withAlpha (0.02f), 0.0f, area.getBottom(), false);
            grad.addColour (0.5, col.withAlpha (0.24f));
            g.setGradientFill (grad);
            g.fillPath (fill);
            juce::PathStrokeType soft (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
            g.setColour (col.withAlpha (0.85f));
            g.strokePath (lineTop, soft);
            g.strokePath (lineBot, soft);
            break;
        }
        case 2: // Outline
        {
            g.setColour (col.withAlpha (0.10f));
            g.fillPath (fill);
            juce::PathStrokeType outline (1.3f);
            g.setColour (col.withAlpha (0.85f));
            g.strokePath (lineTop, outline);
            g.strokePath (lineBot, outline);
            break;
        }
        case 3: // Rectified
        {
            juce::Path humpUpper, humpLower;
            for (int i = 0; i < n; i++)
            {
                const float yUp  = cy - amps[(size_t) i];
                const float yLow = cy + H * 0.28f - amps[(size_t) i] * 0.55f;
                if (i == 0) { humpUpper.startNewSubPath (xs[0], yUp); humpLower.startNewSubPath (xs[0], yLow); }
                else        { humpUpper.lineTo (xs[(size_t) i], yUp); humpLower.lineTo (xs[(size_t) i], yLow); }
            }
            juce::PathStrokeType stroke (1.4f);
            g.setColour (col.withAlpha (0.75f));
            g.strokePath (humpUpper, stroke);
            g.strokePath (humpLower, stroke);
            break;
        }
        case 4: // Mirrored
        {
            g.setColour (col.withAlpha (0.12f));
            g.fillPath (fill);
            juce::PathStrokeType stroke (1.2f);
            g.setColour (col.withAlpha (0.75f));
            g.strokePath (lineTop, stroke);
            g.strokePath (lineBot, stroke);

            auto flipped = fill;
            flipped.applyTransform (juce::AffineTransform::scale (-1.0f, 1.0f, area.getCentreX(), 0.0f));
            g.setColour (col.withAlpha (0.10f));
            g.fillPath (flipped);
            break;
        }
        case 5: // Bars
        {
            const int barCount = juce::jmax (1, juce::roundToInt (W / 4.0f));
            g.setColour (col.withAlpha (0.80f));
            for (int b = 0; b < barCount; ++b)
            {
                const int i  = juce::jlimit (0, n - 1, (int) ((float) b / (float) barCount * n));
                const float bx = area.getX() + ((float) b + 0.5f) / (float) barCount * W;
                const float a  = amps[(size_t) i];
                g.fillRect (juce::Rectangle<float> (bx - 1.2f, cy - a, 2.4f, a * 2.0f));
            }
            break;
        }
        case 6: // RMS
        {
            juce::Path smoothTop, smoothBot;
            const int win = juce::jmax (1, n / 40);
            for (int i = 0; i < n; i++)
            {
                int lo = juce::jmax (0, i - win), hi = juce::jmin (n - 1, i + win);
                float avg = 0.0f;
                for (int k = lo; k <= hi; ++k) avg += amps[(size_t) k];
                avg /= (float) (hi - lo + 1);
                const float yT = cy - avg, yB = cy + avg;
                if (i == 0) { smoothTop.startNewSubPath (xs[0], yT); smoothBot.startNewSubPath (xs[0], yB); }
                else        { smoothTop.lineTo (xs[(size_t) i], yT); smoothBot.lineTo (xs[(size_t) i], yB); }
            }
            for (float w = 6.0f; w >= 1.5f; w -= 1.5f)
            {
                g.setColour (col.withAlpha (0.10f));
                g.strokePath (smoothTop, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.strokePath (smoothBot, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            break;
        }
        case 7: // Stepped
        {
            const int steps = juce::jmax (1, juce::roundToInt (W / 6.0f));
            juce::Path stepTop, stepBot;
            float prevT = cy, prevB = cy;
            for (int s = 0; s < steps; ++s)
            {
                const int i = juce::jlimit (0, n - 1, (int) ((float) s / (float) steps * n));
                const float sx0 = area.getX() + (float) s / (float) steps * W;
                const float sx1 = area.getX() + (float) (s + 1) / (float) steps * W;
                const float yT = cy - amps[(size_t) i], yB = cy + amps[(size_t) i];
                if (s == 0) { stepTop.startNewSubPath (sx0, yT); stepBot.startNewSubPath (sx0, yB); }
                else        { stepTop.lineTo (sx0, prevT); stepBot.lineTo (sx0, prevB); }
                stepTop.lineTo (sx1, yT);
                stepBot.lineTo (sx1, yB);
                prevT = yT; prevB = yB;
            }
            juce::PathStrokeType stroke (1.3f);
            g.setColour (col.withAlpha (0.85f));
            g.strokePath (stepTop, stroke);
            g.strokePath (stepBot, stroke);
            break;
        }
    }
}

void MultisamplerWaveformLcd::drawLoopOverlay (juce::Graphics& g, const juce::Rectangle<float>& area)
{
    int zoneIdx = -1;
    const SampleZone* z = resolveSelectedZone (zoneIdx);
    if (z == nullptr || z->loopMode == LoopMode::noLoop) return;
    if (z->loopStart < 0 || z->loopEnd <= z->loopStart) return;
    if (windowEndFrame <= windowStartFrame) return;

    auto toX = [&] (int64_t sample) -> float
    {
        const float t = ((float) sample - (float) windowStartFrame)
                         / (float) (windowEndFrame - windowStartFrame);
        return area.getX() + t * area.getWidth();
    };

    const float x0 = juce::jlimit (area.getX(), area.getRight(), toX (z->loopStart));
    const float x1 = juce::jlimit (area.getX(), area.getRight(), toX (z->loopEnd));
    if (x1 <= x0 + 1.0f) return;

    const juce::Colour loopColour { 0xFFFFE800 };
    g.setColour (loopColour.withAlpha (0.06f));
    g.fillRect  (x0, area.getY(), x1 - x0, area.getHeight());

    g.setColour (loopColour.withAlpha (0.50f));
    g.drawVerticalLine (juce::roundToInt (x0), area.getY(), area.getBottom());
    g.drawVerticalLine (juce::roundToInt (x1), area.getY(), area.getBottom());

    const float labelX = (x0 + x1) * 0.5f - 18.0f;
    const float labelY = area.getY() + 3.0f;
    g.setFont (DysektLookAndFeel::makeFont (7.0f, true));
    g.setColour (loopColour.withAlpha (0.55f));
    g.drawText ("LOOP", juce::Rectangle<float> (labelX, labelY, 36.0f, 10.0f),
                juce::Justification::centred, false);
}

void MultisamplerWaveformLcd::mouseWheelMove (const juce::MouseEvent& e,
                                               const juce::MouseWheelDetails& w)
{
    if (! data.hasSelection || data.missingSample) return;

    const bool isZoom = e.mods.isCtrlDown() || e.mods.isCommandDown();
    const int sliceLen = data.endSample - data.startSample;
    if (sliceLen <= 0) return;

    if (isZoom)
    {
        const float anchorFrac = juce::jlimit (0.0f, 1.0f,
            (e.position.x - screenArea.getX()) / screenArea.getWidth());

        const float oldZoom   = juce::jmax (1.0f, zoom);
        const float newZoom   = juce::jlimit (1.0f, 32.0f,
                                              oldZoom * (w.deltaY > 0 ? 1.25f : 0.8f));
        const float oldScroll = scroll;

        const float oldWindowFrac = 1.0f / oldZoom;
        const float oldStart      = oldScroll * (float) sliceLen * (1.0f - oldWindowFrac);
        const float anchorSample  = oldStart + anchorFrac * oldWindowFrac * (float) sliceLen;

        const float newWindowFrac = 1.0f / newZoom;
        const float newStart      = anchorSample - anchorFrac * newWindowFrac * (float) sliceLen;
        const float newMaxStart   = (float) sliceLen * (1.0f - newWindowFrac);
        const float newScroll     = (newMaxStart > 0.0f)
                                    ? juce::jlimit (0.0f, 1.0f, newStart / newMaxStart)
                                    : 0.0f;

        zoom   = newZoom;
        scroll = newScroll;
    }
    else
    {
        const float delta = w.deltaX != 0.0f ? -w.deltaX : -w.deltaY;
        scroll = juce::jlimit (0.0f, 1.0f, scroll + delta * 0.08f);
    }

    buildWaveformPeaks();
    repaint();
}

// ── Draw helpers ──────────────────────────────────────────────────────────────

void MultisamplerWaveformLcd::drawBackground (juce::Graphics& g)
{
    const auto b = getLocalBounds();

    g.setColour (getTheme().waveformBg);
    g.fillRoundedRectangle (b.toFloat(), 0.0f);
    g.setColour (getTheme().separator);
    g.drawRoundedRectangle (b.toFloat().reduced (0.5f), 0.0f, 1.0f);

    const auto screen = b.reduced (4);
    g.setColour (msLcdBg());
    g.fillRoundedRectangle (screen.toFloat(), 0.0f);

    g.setColour (juce::Colour (0xFF000000).withAlpha ((uint8_t) kScanlineAlpha));
    for (int y = screen.getY(); y < screen.getBottom(); y += 2)
        g.drawHorizontalLine (y, (float) screen.getX(), (float) screen.getRight());
}

void MultisamplerWaveformLcd::drawSegmentLabel (juce::Graphics& g,
                                                 float x0, float y0,
                                                 float x1, float y1,
                                                 const char* text,
                                                 juce::Colour col,
                                                 const juce::Rectangle<float>& area)
{
    const float mx = area.getX() + ((x0 + x1) * 0.5f) * area.getWidth();
    const float my = area.getY() + ((y0 + y1) * 0.5f) * area.getHeight() - 9.0f;
    g.setFont (DysektLookAndFeel::makeFont (8.0f));
    g.setColour (col.withAlpha (0.40f));
    g.drawText (juce::String (text),
                juce::Rectangle<float> (mx - 30.0f, my - 6.0f, 60.0f, 12.0f),
                juce::Justification::centred, false);
}

void MultisamplerWaveformLcd::drawHeader (juce::Graphics& g,
                                           const juce::Rectangle<float>& area)
{
    const float headerH = 18.0f;
    const auto  headerR = area.withHeight (headerH);

    g.setFont (DysektLookAndFeel::makeFont (8.5f, true));
    g.setColour (msLcdPhosphor().withAlpha (0.55f));
    g.drawText ("MULTISAMPLER", headerR.withRight (headerR.getX() + 96.0f),
                juce::Justification::centredLeft, false);

    if (data.sampleName.isNotEmpty())
    {
        g.setFont (DysektLookAndFeel::makeFont (8.0f));
        g.setColour (msLcdPhosphor().withAlpha (0.38f));
        g.drawText (data.sampleName, headerR, juce::Justification::centredRight, true);
    }
}

void MultisamplerWaveformLcd::drawEnvelope (juce::Graphics& g,
                                             const juce::Rectangle<float>& area)
{
    const float W  = area.getWidth();
    const float H  = area.getHeight();
    const float ox = area.getX();
    const float oy = area.getY();

    auto px = [&] (float xn) { return ox + xn * W; };
    auto py = [&] (float yn) { return oy + yn * H; };

    juce::Path envFill;
    envFill.startNewSubPath (px (0.0f),        py (1.0f));
    envFill.lineTo           (px (env.ax),     py (env.ay));
    envFill.lineTo           (px (env.dx),     py (env.sy));
    envFill.lineTo           (px (env.sxEnd),  py (env.sy));
    envFill.lineTo           (px (env.rx),     py (env.sy));
    envFill.lineTo           (px (1.0f),       py (1.0f));
    envFill.closeSubPath();

    juce::ColourGradient fillGrad (kColDecay.withAlpha (0.07f), 0, oy,
                                    kColDecay.withAlpha (0.00f), 0, oy + H, false);
    g.setGradientFill (fillGrad);
    g.fillPath (envFill);

    juce::Path envLine;
    envLine.startNewSubPath (px (0.0f),        py (1.0f));
    envLine.lineTo           (px (env.ax),     py (env.ay));
    envLine.lineTo           (px (env.dx),     py (env.sy));
    envLine.lineTo           (px (env.sxEnd),  py (env.sy));
    envLine.lineTo           (px (env.rx),     py (env.sy));
    envLine.lineTo           (px (1.0f),       py (1.0f));

    g.setColour (juce::Colours::white.withAlpha (0.07f));
    g.strokePath (envLine, juce::PathStrokeType (2.5f));

    juce::Path dashedLine;
    {
        juce::PathStrokeType stroke (1.0f);
        float dashes[] = { 3.0f, 5.0f };
        stroke.createDashedStroke (dashedLine, envLine, dashes, 2);
    }
    g.setColour (juce::Colours::white.withAlpha (0.20f));
    g.fillPath (dashedLine);

    juce::Path susLine;
    susLine.startNewSubPath (px (env.dx), py (env.sy));
    susLine.lineTo           (px (env.sxEnd), py (env.sy));
    g.setColour (kColSustain.withAlpha (0.35f));
    g.strokePath (susLine, juce::PathStrokeType (1.0f));

    drawSegmentLabel (g, 0.0f, 1.0f, env.ax, env.ay, "ATTACK",  kColAttack,  area);
    drawSegmentLabel (g, env.ax, env.ay, env.dx, env.sy, "DECAY", kColDecay, area);
    drawSegmentLabel (g, env.rx, env.sy, 1.0f, 1.0f, "RELEASE", kColRelease, area);
}

void MultisamplerWaveformLcd::drawNodes (juce::Graphics& g,
                                          const juce::Rectangle<float>& area)
{
    const float W  = area.getWidth();
    const float H  = area.getHeight();
    const float ox = area.getX();
    const float oy = area.getY();

    for (const auto& node : envNodes)
    {
        const float cx  = ox + node.xn * W;
        const bool  hov = (node.role == hovRole || node.role == dragRole);
        const float r   = hov ? kNodeR + 2.5f : kNodeR;

        const float compH = (float) getHeight();
        const float cyRaw = oy + node.yn * H;
        const float cy    = juce::jmax (r + 2.0f, juce::jmin (compH - r - 2.0f, cyRaw));

        // No lockMask equivalent exists on SampleZone — always drawn unlocked.
        if (node.role != NodeRole::Sustain)
        {
            g.setColour (node.colour.withAlpha (0.18f));
            g.drawVerticalLine (juce::roundToInt (cx), cy + r, oy + H);
        }

        g.setColour (node.colour.withAlpha (hov ? 0.55f : 0.25f));
        g.drawEllipse (cx - r, cy - r, r * 2.0f, r * 2.0f, hov ? 1.5f : 1.0f);

        const float dr = hov ? 3.0f : 2.5f;
        g.setColour (node.colour.withAlpha (hov ? 1.0f : 0.80f));
        g.fillEllipse (cx - dr, cy - dr, dr * 2.0f, dr * 2.0f);

        g.setFont (DysektLookAndFeel::makeFont (hov ? 11.0f : 9.5f, true));
        g.setColour (node.colour.withAlpha (hov ? 1.0f : 0.70f));
        g.drawText (juce::String (node.label),
                    juce::Rectangle<float> (cx - 14.0f, cy + r + 2.0f, 28.0f, 12.0f),
                    juce::Justification::centred, false);
    }
}

void MultisamplerWaveformLcd::drawNoData (juce::Graphics& g)
{
    const auto b = getLocalBounds().reduced (4);

    if (! data.hasInstrument || ! data.hasZones)
    {
        g.setFont (DysektLookAndFeel::makeFont (18.0f, true));
        g.setColour (msLcdPhosphor().withAlpha (0.18f));
        g.drawText ("EMPTY", b, juce::Justification::centred);

        g.setFont (DysektLookAndFeel::makeFont (7.5f));
        g.setColour (msLcdDim().brighter (0.5f));
        auto b2 = b;
        g.drawText ("import an sfz or add a zone",
                    b2.removeFromBottom (18), juce::Justification::centred);
        return;
    }

    if (! data.hasSelection)
    {
        g.setFont (DysektLookAndFeel::makeFont (10.0f));
        g.setColour (msLcdDim().brighter (0.4f));
        g.drawText ("-- SELECT A ZONE --", b, juce::Justification::centred);
        return;
    }

    // hasSelection but missingSample (missing file / failed decode /
    // zero-length range) — show the missing/default treatment, no editing.
    g.setFont (DysektLookAndFeel::makeFont (18.0f, true));
    g.setColour (msLcdPhosphor().withAlpha (0.18f));
    g.drawText ("EMPTY", b, juce::Justification::centred);

    g.setFont (DysektLookAndFeel::makeFont (7.5f));
    g.setColour (msLcdDim().brighter (0.5f));
    auto b3 = b;
    g.drawText ("missing or invalid sample",
                b3.removeFromBottom (18), juce::Justification::centred);
}

// ── Playhead ──────────────────────────────────────────────────────────────────

void MultisamplerWaveformLcd::drawPlayhead (juce::Graphics& g, const juce::Rectangle<float>& area)
{
    if (! data.hasSelection || data.missingSample) return;

    int zoneIdx = -1;
    const SampleZone* z = resolveSelectedZone (zoneIdx);
    if (z == nullptr) return;

    // MULTISAMPLER zone indices don't correspond to sliceManager2/voicePool2
    // slice indices (loop-split zones can shift that numbering), so a
    // voice's sliceIdx can't be matched against the zone index directly.
    // Instead, match the active voice against the selected zone's own
    // key/velocity range — every Voice already carries the real midiNote/
    // velocity that triggered it, and SampleZone already carries lowKey/
    // highKey/lowVelocity/highVelocity.
    for (int i = 0; i < VoicePool::kMaxVoices; ++i)
    {
        const auto& v = processor.voicePool2.getVoice (i);
        if (! v.active) continue;

        // Voice::velocity is normalised 0..1 (see VoicePool::startVoice);
        // SampleZone's range is stored in the original 1..127 MIDI scale.
        const int vel127 = juce::roundToInt (v.velocity * 127.0f);
        if (v.midiNote < z->lowKey || v.midiNote > z->highKey
            || vel127 < z->lowVelocity || vel127 > z->highVelocity)
            continue;

        // rawPos is an absolute sample index into processor.sampleData2 —
        // the pre-rendered, per-note capture buffer SoundFontLoader.cpp
        // builds for MULTISAMPLER, which has NO relationship to
        // data.startSample/data.endSample (positions in the *original*
        // zone sample file, decoded separately into zoneSampleData purely
        // so this display has a waveform/peaks to draw). v.sliceIdx (set
        // from sliceManager2.midiNoteToSlice() at voice start) identifies
        // which rendered segment this voice is actually playing, in
        // sampleData2's own coordinate space — the same one rawPos is in —
        // even though that index has no relationship to the zone index
        // (hence the key/velocity matching above instead of index matching).
        auto& sm2 = processor.sliceManager2;
        if (v.sliceIdx < 0 || v.sliceIdx >= sm2.getNumSlices())
            continue;

        const int segStart = sm2.getSlice (v.sliceIdx).startSample;
        const int segEnd   = (v.sliceIdx + 1 < sm2.getNumSlices())
                                  ? sm2.getSlice (v.sliceIdx + 1).startSample
                                  : processor.sampleData2.getNumFrames();
        const int segRange = segEnd - segStart;
        if (segRange <= 0) continue;

        const float rawPos = processor.voicePool2.voicePositions[i].load (std::memory_order_relaxed);
        const float xn = juce::jlimit (0.0f, 1.0f, (rawPos - (float) segStart) / (float) segRange);
        const float x  = area.getX() + xn * area.getWidth();

        g.setColour (msLcdPhosphor().withAlpha (0.85f));
        g.drawLine (x, area.getY(), x, area.getBottom(), 1.5f);

        const float capH = 5.0f;
        juce::Path cap;
        cap.addTriangle (x - 3.5f, area.getY(),
                          x + 3.5f, area.getY(),
                          x, area.getY() + capH);
        g.fillPath (cap);

        break; // only draw the most-recently-hit voice
    }
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void MultisamplerWaveformLcd::paint (juce::Graphics& g)
{
    {
        juce::Path clipPath;
        clipPath.addRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        g.reduceClipRegion (clipPath);
    }

    buildDisplayData();
    drawBackground (g);

    const auto nodeArea = getLocalBounds().reduced (4).toFloat();
    screenArea = nodeArea;
    const auto lcdArea = nodeArea.reduced (2.0f);

    if (! data.hasInstrument || ! data.hasZones || ! data.hasSelection || data.missingSample)
    {
        drawNoData (g);
        return;
    }

    drawHeader        (g, lcdArea);
    drawWaveform       (g, lcdArea);
    drawLoopOverlay    (g, lcdArea);
    drawEnvelope       (g, lcdArea);
    drawNodes  (g, nodeArea);
    drawPlayhead (g, lcdArea);
}
