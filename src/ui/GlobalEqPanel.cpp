#include "GlobalEqPanel.h"
#include "../PluginProcessor.h"
#include "../params/ParamIds.h"
#include "ThemeData.h"
#include "DysektLookAndFeel.h"
#include <cmath>

// ── Constructor ───────────────────────────────────────────────────────────────

GlobalEqPanel::GlobalEqPanel (DysektProcessor& p)
    : processor (p)
{
    //            band       gain  freq     q    defaultFreq  draggable
    nodes[BLow]     = { BLow,     0.f,  80.f,   1.f,  80.f,   true,  {} };
    nodes[BLowMid]  = { BLowMid,  0.f,  250.f,  1.f,  250.f,  true,  {} };
    nodes[BMid]     = { BMid,     0.f,  1000.f, 1.f,  1000.f, true,  {} };
    nodes[BHighMid] = { BHighMid, 0.f,  4000.f, 1.f,  4000.f, true,  {} };
    nodes[BHigh]    = { BHigh,    0.f,  12000.f,1.f,  12000.f,true,  {} };

    // Allocate spectrum buffer — size matches SpectrumAnalyser::numBins (1024)
    spectrumSmoothed.assign (SpectrumAnalyser::numBins, 0.f);

    setOpaque (false);

    startTimerHz (30);   // 30 fps repaint for spectrum
}

GlobalEqPanel::~GlobalEqPanel()
{
    stopTimer();
}

// ── Timer callback — copies + smooths FFT data, triggers repaint ──────────────

void GlobalEqPanel::timerCallback()
{
    spectrumSampleRate = processor.getSampleRate();
    if (spectrumSampleRate <= 0.0) spectrumSampleRate = 44100.0;

    const auto& fresh = processor.spectrumAnalyser.getReadBuffer();

    // Exponential decay smoothing: 80% old + 20% new — gives the silky Pro-Q look.
    // Increase kDecay toward 0.95 for slower movement, lower toward 0.60 for snappier.
    static constexpr float kDecay = 0.80f;
    for (size_t i = 0; i < spectrumSmoothed.size(); ++i)
        spectrumSmoothed[i] = spectrumSmoothed[i] * kDecay + fresh[i] * (1.f - kDecay);

    repaint();
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void GlobalEqPanel::paint (juce::Graphics& g)
{
    auto& theme  = getTheme();
    auto  bounds = getLocalBounds().toFloat();
    auto  plot   = plotArea();

    // ── Theme-aware frame: gradient background + accent border ────────────────
    {
        auto bgTop = theme.darkBar.darker (0.45f);
        auto bgBot = theme.darkBar.darker (0.65f);
        juce::ColourGradient grad (bgTop, 0, 0, bgBot, 0, bounds.getHeight(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (bounds, 4.f);

        // Outer glow ring
        g.setColour (theme.accent.withAlpha (0.18f));
        g.drawRoundedRectangle (bounds.expanded (1.0f), 5.f, 1.0f);
        // Main accent border
        g.setColour (theme.accent.withAlpha (0.60f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 4.f, 1.5f);
    }

    // ── Title ─────────────────────────────────────────────────────────────────
    g.setColour (theme.foreground.withAlpha (0.55f));
    g.setFont (DysektLookAndFeel::makeFont (10.f, true));
    g.drawText ("GLOBAL EQ",
                juce::Rectangle<float> (bounds.getX() + 8.f, bounds.getY() + 3.f,
                                        80.f, 14.f),
                juce::Justification::centredLeft, false);

    // ── Grid ─────────────────────────────────────────────────────────────────

    // Minor horizontal dB lines (±3, ±9, ±15, ±18) — drawn first, very dim
    g.setColour (theme.gridLine.withAlpha (0.22f));
    for (int db : { -18, -15, -9, -3, 3, 9, 15, 18 })
        g.drawHorizontalLine (juce::roundToInt (gainToY ((float) db)), plot.getX(), plot.getRight());

    // Major horizontal dB lines (±6, ±12) and zero line
    for (int db : { -12, -6, 0, 6, 12 })
    {
        float y = gainToY ((float) db);
        if (db == 0)
        {
            g.setColour (theme.separator.withAlpha (0.70f));
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        }
        else
        {
            g.setColour (theme.gridLine.withAlpha (0.60f));
            g.drawHorizontalLine (juce::roundToInt (y), plot.getX(), plot.getRight());
        }
    }

    // Minor vertical frequency lines — per-decade subdivisions, drawn first, very dim
    g.setColour (theme.gridLine.withAlpha (0.20f));
    for (float hz : { 20.f, 30.f, 40.f, 60.f, 70.f, 80.f, 90.f,
                      150.f, 200.f, 300.f, 400.f, 600.f, 700.f, 800.f, 900.f,
                      1500.f, 2000.f, 3000.f, 4000.f, 6000.f, 7000.f, 8000.f, 9000.f,
                      15000.f, 20000.f })
        g.drawVerticalLine (juce::roundToInt (freqToX (hz)), plot.getY(), plot.getBottom());

    // Major vertical frequency lines — decade markers
    g.setColour (theme.gridLine.withAlpha (0.55f));
    for (float hz : { 50.f, 100.f, 500.f, 1000.f, 5000.f, 10000.f })
        g.drawVerticalLine (juce::roundToInt (freqToX (hz)), plot.getY(), plot.getBottom());

    // ── dB labels (right edge) ───────────────────────────────────────────────
    g.setColour (theme.foreground.withAlpha (0.35f));
    g.setFont (DysektLookAndFeel::makeFont (8.f));
    for (int db : { -12, -6, 6, 12 })
    {
        float y = gainToY ((float) db);
        juce::String lbl = (db > 0 ? "+" : "") + juce::String (db);
        g.drawText (lbl,
                    juce::Rectangle<float> (plot.getRight() + 3.f, y - 6.f, 22.f, 12.f),
                    juce::Justification::centredLeft, false);
    }

    // ── Frequency labels (bottom edge) ───────────────────────────────────────
    g.setColour (theme.foreground.withAlpha (0.35f));
    g.setFont (DysektLookAndFeel::makeFont (8.f));
    for (auto [hz, lbl] : std::initializer_list<std::pair<float, const char*>>{
            { 100.f, "100" }, { 200.f, "200" }, { 500.f, "500" }, { 1000.f, "1k" },
            { 2000.f, "2k" }, { 5000.f, "5k" }, { 10000.f, "10k" } })
    {
        float x = freqToX (hz);
        g.drawText (lbl,
                    juce::Rectangle<float> (x - 14.f, plot.getBottom() + 2.f, 28.f, 10.f),
                    juce::Justification::centred, false);
    }

    // ── Spectrum analyser (drawn BEFORE EQ curve so curve sits on top) ────────
    {
        auto specPath = buildSpectrum();
        if (! specPath.isEmpty())
        {
            // Filled area under the spectrum
            auto filled = specPath;
            filled.lineTo (plot.getRight(), plot.getBottom());
            filled.lineTo (plot.getX(),     plot.getBottom());
            filled.closeSubPath();
            g.setColour (theme.accent.withAlpha (0.07f));
            g.fillPath (filled);

            // Spectrum line — noticeably dimmer than the EQ curve
            g.setColour (theme.accent.withAlpha (0.28f));
            g.strokePath (specPath, juce::PathStrokeType (1.1f,
                          juce::PathStrokeType::curved,
                          juce::PathStrokeType::rounded));
        }
    }

    // ── EQ curve ─────────────────────────────────────────────────────────────
    auto curve = buildCurve();

    // Filled area
    {
        auto filled = curve;
        filled.lineTo (plot.getRight(), gainToY (0.f));
        filled.lineTo (plot.getX(),     gainToY (0.f));
        filled.closeSubPath();
        g.setColour (theme.accent.withAlpha (0.08f));
        g.fillPath (filled);
    }

    // Curve stroke
    g.setColour (theme.accent.withAlpha (0.85f));
    g.strokePath (curve, juce::PathStrokeType (1.8f,
                          juce::PathStrokeType::curved,
                          juce::PathStrokeType::rounded));

    // ── Node handles ─────────────────────────────────────────────────────────
    const char* bandLabels[kNumBands] = { "L", "LM", "M", "HM", "H" };

    // Band colours: shelves slightly dimmer, peaks full accent
    const float bandAlpha[kNumBands] = { 0.75f, 0.90f, 1.00f, 0.90f, 0.75f };

    for (int i = 0; i < kNumBands; ++i)
    {
        auto& n   = nodes[i];
        float x   = n.pos.x;
        float y   = n.pos.y;
        auto  col = theme.accent.withAlpha (bandAlpha[i]);

        // Shadow
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (x - kNodeRadius + 1.f, y - kNodeRadius + 1.f,
                       kNodeRadius * 2.f, kNodeRadius * 2.f);

        // Fill
        g.setColour (col);
        g.fillEllipse (x - kNodeRadius, y - kNodeRadius,
                       kNodeRadius * 2.f, kNodeRadius * 2.f);

        // Border
        g.setColour (theme.foreground.withAlpha (0.55f));
        g.drawEllipse (x - kNodeRadius, y - kNodeRadius,
                       kNodeRadius * 2.f, kNodeRadius * 2.f, 1.f);

        // Label inside node
        g.setColour (theme.background.withAlpha (0.90f));
        g.setFont (DysektLookAndFeel::makeFont (i == BLowMid || i == BHighMid ? 6.f : 7.5f, true));
        g.drawText (bandLabels[i],
                    juce::Rectangle<float> (x - kNodeRadius, y - kNodeRadius,
                                            kNodeRadius * 2.f, kNodeRadius * 2.f),
                    juce::Justification::centred, false);
    }

    // ── Readout: show freq + gain for hovered / dragged node ─────────────────
    int infoband = (dragBand != NoBand) ? dragBand : NoBand;
    if (infoband != NoBand)
    {
        auto& n = nodes[infoband];
        juce::String tip = juce::String ((int) n.freqHz) + " Hz  "
                         + (n.gainDb >= 0.f ? "+" : "")
                         + juce::String (n.gainDb, 1) + " dB";
        g.setColour (theme.foreground.withAlpha (0.55f));
        g.setFont (DysektLookAndFeel::makeFont (9.f));
        g.drawText (tip,
                    plot.withTrimmedBottom (2.f),
                    juce::Justification::bottomRight, false);
    }
}

// ── Resized ───────────────────────────────────────────────────────────────────

void GlobalEqPanel::resized()
{
    layout();
}

// ── Mouse ─────────────────────────────────────────────────────────────────────

void GlobalEqPanel::mouseDown (const juce::MouseEvent& e)
{
    int hit = hitTest (e.position);
    if (hit == NoBand) return;

    dragBand      = hit;
    dragStartGain = nodes[hit].gainDb;
    dragStartFreq = nodes[hit].freqHz;
}

void GlobalEqPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragBand == NoBand) return;

    auto& n = nodes[dragBand];

    // Y → gain
    n.gainDb = juce::jlimit (-kGainMax, kGainMax, yToGain (e.position.y));

    // X → freq (all bands now draggable)
    if (n.freqDraggable)
    {
        float rawFreq = xToFreq (e.position.x);
        float loLim = kPlotFreqLo, hiLim = kPlotFreqHi;
        if (dragBand == BLow)     { loLim = 20.f;           hiLim = 500.f;          }
        if (dragBand == BLowMid)  { loLim = kLowMidFreqLo;  hiLim = kLowMidFreqHi;  }
        if (dragBand == BMid)     { loLim = kMidFreqLo;      hiLim = kMidFreqHi;     }
        if (dragBand == BHighMid) { loLim = kHighMidFreqLo;  hiLim = kHighMidFreqHi; }
        if (dragBand == BHigh)    { loLim = 2000.f;          hiLim = 20000.f;        }
        n.freqHz = juce::jlimit (loLim, hiLim, rawFreq);
    }

    // Write to APVTS
    auto setP = [&] (const juce::String& id, float val)
    {
        if (auto* p = processor.apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (val));
    };

    switch (dragBand)
    {
        case BLow:     setP (ParamIds::globalEqLowGain,     n.gainDb);
                       setP (ParamIds::globalEqLowFreq,     n.freqHz); break;
        case BLowMid:  setP (ParamIds::globalEqLowMidGain,  n.gainDb);
                       setP (ParamIds::globalEqLowMidFreq,  n.freqHz); break;
        case BMid:     setP (ParamIds::globalEqMidGain,     n.gainDb);
                       setP (ParamIds::globalEqMidFreq,     n.freqHz); break;
        case BHighMid: setP (ParamIds::globalEqHighMidGain, n.gainDb);
                       setP (ParamIds::globalEqHighMidFreq, n.freqHz); break;
        case BHigh:    setP (ParamIds::globalEqHighGain,    n.gainDb);
                       setP (ParamIds::globalEqHighFreq,    n.freqHz); break;
        default: break;
    }

    layout();
    repaint();
}

void GlobalEqPanel::mouseUp (const juce::MouseEvent&)
{
    dragBand = NoBand;
    repaint();
}

void GlobalEqPanel::mouseMove (const juce::MouseEvent& e)
{
    int hit = hitTest (e.position);
    setMouseCursor (hit != NoBand ? juce::MouseCursor::PointingHandCursor
                                  : juce::MouseCursor::NormalCursor);
}

void GlobalEqPanel::mouseDoubleClick (const juce::MouseEvent& e)
{
    int hit = hitTest (e.position);
    if (hit == NoBand) return;
    resetBandToDefault (hit);
}

// ── Private helpers ───────────────────────────────────────────────────────────

void GlobalEqPanel::resetBandToDefault (int band)
{
    auto& n = nodes[band];
    n.gainDb = 0.f;
    n.freqHz = n.defaultFreqHz;

    auto setP = [&] (const juce::String& id, float val)
    {
        if (auto* p = processor.apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (val));
    };

    switch (band)
    {
        case BLow:
            setP (ParamIds::globalEqLowGain,     0.f);
            break;
        case BLowMid:
            setP (ParamIds::globalEqLowMidGain,  0.f);
            setP (ParamIds::globalEqLowMidFreq,  n.defaultFreqHz);
            break;
        case BMid:
            setP (ParamIds::globalEqMidGain,     0.f);
            setP (ParamIds::globalEqMidFreq,     n.defaultFreqHz);
            break;
        case BHighMid:
            setP (ParamIds::globalEqHighMidGain, 0.f);
            setP (ParamIds::globalEqHighMidFreq, n.defaultFreqHz);
            break;
        case BHigh:
            setP (ParamIds::globalEqHighGain,    0.f);
            break;
        default: break;
    }

    layout();
    repaint();
}

void GlobalEqPanel::layout()
{
    auto getP = [&] (const juce::String& id, float fallback = 0.f) -> float
    {
        if (auto* p = processor.apvts.getRawParameterValue (id))
            return p->load();
        return fallback;
    };

    nodes[BLow].gainDb     = getP (ParamIds::globalEqLowGain);
    nodes[BLow].freqHz     = getP (ParamIds::globalEqLowFreq, 80.f);

    nodes[BLowMid].gainDb  = getP (ParamIds::globalEqLowMidGain);
    nodes[BLowMid].freqHz  = getP (ParamIds::globalEqLowMidFreq,  250.f);
    nodes[BLowMid].q       = getP (ParamIds::globalEqLowMidQ,     1.f);

    nodes[BMid].gainDb     = getP (ParamIds::globalEqMidGain);
    nodes[BMid].freqHz     = getP (ParamIds::globalEqMidFreq,    1000.f);
    nodes[BMid].q          = getP (ParamIds::globalEqMidQ,       1.f);

    nodes[BHighMid].gainDb = getP (ParamIds::globalEqHighMidGain);
    nodes[BHighMid].freqHz = getP (ParamIds::globalEqHighMidFreq, 4000.f);
    nodes[BHighMid].q      = getP (ParamIds::globalEqHighMidQ,    1.f);

    nodes[BHigh].gainDb    = getP (ParamIds::globalEqHighGain);
    nodes[BHigh].freqHz    = getP (ParamIds::globalEqHighFreq, 12000.f);

    for (auto& n : nodes)
        n.pos = { freqToX (n.freqHz), gainToY (n.gainDb) };
}

juce::Rectangle<float> GlobalEqPanel::plotArea() const
{
    return getLocalBounds().toFloat()
           .reduced (8.f)
           .withTrimmedTop (18.f)
           .withTrimmedBottom (14.f)
           .withTrimmedRight (28.f);
}

float GlobalEqPanel::gainToY (float dB) const
{
    auto r = plotArea();
    float t = (dB + kGainMax) / (kGainMax * 2.f);
    return r.getBottom() - t * r.getHeight();
}

float GlobalEqPanel::yToGain (float y) const
{
    auto r = plotArea();
    float t = (r.getBottom() - y) / r.getHeight();
    return t * (kGainMax * 2.f) - kGainMax;
}

float GlobalEqPanel::freqToX (float hz) const
{
    auto  r     = plotArea();
    float logLo = std::log10 (kPlotFreqLo);
    float logHi = std::log10 (kPlotFreqHi);
    float t     = (std::log10 (juce::jlimit (kPlotFreqLo, kPlotFreqHi, hz)) - logLo)
                  / (logHi - logLo);
    return r.getX() + t * r.getWidth();
}

float GlobalEqPanel::xToFreq (float x) const
{
    auto  r     = plotArea();
    float t     = juce::jlimit (0.f, 1.f, (x - r.getX()) / r.getWidth());
    float logLo = std::log10 (kPlotFreqLo);
    float logHi = std::log10 (kPlotFreqHi);
    return std::pow (10.f, logLo + t * (logHi - logLo));
}

// ── Spectrum analyser path ────────────────────────────────────────────────────
// Maps FFT bins → screen coordinates.
// Bin i = frequency (i * sampleRate / fftSize).
//
// SpectrumAnalyser normalises values as: 0.0 = –80 dBFS, 1.0 = 0 dBFS.
// We map that 0..1 range directly across the full plot height so the spectrum
// fills the display at all volume levels — the EQ ±dB gridlines are a separate
// overlay and don't constrain how tall the spectrum can be.

juce::Path GlobalEqPanel::buildSpectrum() const
{
    const int   numBins  = (int) spectrumSmoothed.size();
    if (numBins == 0) return {};

    const int   fftSize  = numBins * 2;
    const float sr       = (float) spectrumSampleRate;
    const float binHz    = sr / (float) fftSize;
    auto        plot     = plotArea();

    juce::Path path;
    bool started = false;

    for (int i = 1; i < numBins; ++i)
    {
        float hz = (float) i * binHz;
        if (hz < kPlotFreqLo) continue;
        if (hz > kPlotFreqHi) break;

        float x = freqToX (hz);

        // norm is 0..1 where 1.0 = 0 dBFS, 0.0 = –80 dBFS.
        // Map linearly to the full plot height: norm=1 → top, norm=0 → bottom.
        float norm = juce::jlimit (0.f, 1.f, spectrumSmoothed[i]);
        float y    = plot.getBottom() - norm * plot.getHeight();

        if (! started) { path.startNewSubPath (x, y); started = true; }
        else             path.lineTo (x, y);
    }

    return path;
}

// ── EQ curve ─────────────────────────────────────────────────────────────────

// Biquad magnitude response for one band.
// type: 0 = lowShelf, 1 = peakFilter, 2 = highShelf
float GlobalEqPanel::filterMagnitudeAt (int type, float evalHz, float filterHz,
                                         float gainDb, float q, float sampleRate)
{
    const float A  = std::pow (10.f, gainDb / 40.f);
    const float Ap = std::pow (10.f, gainDb / 20.f);

    const float w0  = juce::MathConstants<float>::twoPi * filterHz / sampleRate;
    const float w   = juce::MathConstants<float>::twoPi * evalHz   / sampleRate;
    const float cw  = std::cos (w);
    const float cw0 = std::cos (w0);
    const float sw0 = std::sin (w0);

    float b0, b1, b2, a0, a1, a2;

    if (type == 0) // low shelf
    {
        float alpha = sw0 / 2.f * std::sqrt ((A + 1.f / A) * (1.f / 1.f - 1.f) + 2.f);
        b0 =  A * ((A+1) - (A-1)*cw0 + 2*std::sqrt(A)*alpha);
        b1 =  2*A * ((A-1) - (A+1)*cw0);
        b2 =  A * ((A+1) - (A-1)*cw0 - 2*std::sqrt(A)*alpha);
        a0 =  (A+1) + (A-1)*cw0 + 2*std::sqrt(A)*alpha;
        a1 = -2 * ((A-1) + (A+1)*cw0);
        a2 =  (A+1) + (A-1)*cw0 - 2*std::sqrt(A)*alpha;
    }
    else if (type == 2) // high shelf
    {
        float alpha = sw0 / 2.f * std::sqrt ((A + 1.f / A) * (1.f / 1.f - 1.f) + 2.f);
        b0 =  A * ((A+1) + (A-1)*cw0 + 2*std::sqrt(A)*alpha);
        b1 = -2*A * ((A-1) + (A+1)*cw0);
        b2 =  A * ((A+1) + (A-1)*cw0 - 2*std::sqrt(A)*alpha);
        a0 =  (A+1) - (A-1)*cw0 + 2*std::sqrt(A)*alpha;
        a1 =  2 * ((A-1) - (A+1)*cw0);
        a2 =  (A+1) - (A-1)*cw0 - 2*std::sqrt(A)*alpha;
    }
    else // peak
    {
        float alpha = sw0 / (2.f * q);
        b0 = 1 + alpha * Ap;
        b1 = -2 * cw0;
        b2 = 1 - alpha * Ap;
        a0 = 1 + alpha / Ap;
        a1 = -2 * cw0;
        a2 = 1 - alpha / Ap;
    }

    float cos2W = std::cos (2.f * w);
    float sinW  = std::sin (w);
    float sin2W = std::sin (2.f * w);

    float bRe = b0 + b1*cw  + b2*cos2W;
    float bIm =      b1*sinW + b2*sin2W;
    float aRe = a0 + a1*cw  + a2*cos2W;
    float aIm =      a1*sinW + a2*sin2W;

    float num = bRe*bRe + bIm*bIm;
    float den = aRe*aRe + aIm*aIm;
    if (den < 1e-10f) return 0.f;
    return std::sqrt (num / den);
}

juce::Path GlobalEqPanel::buildCurve() const
{
    auto  plot = plotArea();
    int   nPts = juce::jmax (2, (int) plot.getWidth());
    juce::Path path;

    for (int i = 0; i < nPts; ++i)
    {
        float x  = plot.getX() + (float) i;
        float hz = xToFreq (x);

        float magLin =
            filterMagnitudeAt (0, hz, nodes[BLow].freqHz,     nodes[BLow].gainDb,     1.f,                  kSampleRate)
          * filterMagnitudeAt (1, hz, nodes[BLowMid].freqHz,  nodes[BLowMid].gainDb,  nodes[BLowMid].q,     kSampleRate)
          * filterMagnitudeAt (1, hz, nodes[BMid].freqHz,     nodes[BMid].gainDb,     nodes[BMid].q,        kSampleRate)
          * filterMagnitudeAt (1, hz, nodes[BHighMid].freqHz, nodes[BHighMid].gainDb, nodes[BHighMid].q,    kSampleRate)
          * filterMagnitudeAt (2, hz, nodes[BHigh].freqHz,    nodes[BHigh].gainDb,    1.f,                  kSampleRate);

        float dB = 20.f * std::log10 (juce::jmax (1e-6f, magLin));
        float y  = gainToY (juce::jlimit (-kGainMax, kGainMax, dB));

        if (i == 0) path.startNewSubPath (x, y);
        else        path.lineTo (x, y);
    }

    return path;
}

int GlobalEqPanel::hitTest (juce::Point<float> p) const
{
    float bestDist = kNodeRadius * 2.f;
    int   best     = NoBand;

    for (int i = 0; i < kNumBands; ++i)
    {
        float d = nodes[i].pos.getDistanceFrom (p);
        if (d < bestDist)
        {
            bestDist = d;
            best     = i;
        }
    }
    return best;
}
