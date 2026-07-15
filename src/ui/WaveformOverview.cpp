#include "WaveformOverview.h"
#include "../PluginProcessor.h"
#include "DysektLookAndFeel.h"   // getTheme()
#include "../PluginEditor.h"

//==============================================================================
WaveformOverview::WaveformOverview (DysektProcessor& p)
    : processor (p)
{
    setOpaque (false);
}

void WaveformOverview::repaintOverview()
{
    repaint();
}

//==============================================================================
// Viewport rect — mirrors WaveformView::buildViewState():
//   visibleLen   = numFrames / zoom
//   maxStart     = numFrames - visibleLen
//   visibleStart = scroll * maxStart
// Maps to pixel fractions of the *screen* rectangle (not the whole component).
//==============================================================================
juce::Rectangle<float> WaveformOverview::viewportRect (const juce::Rectangle<int>& screen) const
{
    auto snap = processor.sampleData.getSnapshot();
    if (! snap || snap->buffer.getNumSamples() == 0)
        return screen.toFloat();

    const float W   = (float) screen.getWidth();
    const float H   = (float) screen.getHeight();
    const int   nf  = snap->buffer.getNumSamples();
    const float z   = juce::jmax (1.0f, processor.zoom.load());
    const float sc  = processor.scroll.load();

    const int visLen   = juce::jlimit (1, nf, (int) ((float) nf / z));
    const int maxStart = juce::jmax (0, nf - visLen);
    const int visStart = juce::jlimit (0, maxStart, (int) (sc * (float) maxStart));

    const float x0 = (float) screen.getX() + (float) visStart        / (float) nf * W;
    const float x1 = (float) screen.getX() + (float) (visStart + visLen) / (float) nf * W;

    return { x0, (float) screen.getY(), juce::jmax (1.0f, x1 - x0), H };
}

//==============================================================================
// Peak cache — one max-abs value per pixel column of the screen area
//==============================================================================
void WaveformOverview::rebuildPeaks()
{
    auto snap = processor.sampleData.getSnapshot();
    if (! snap || snap->buffer.getNumSamples() == 0 || getWidth() <= 0)
    {
        peaks.clear();
        peakNumFrames = 0;
        peakWidth     = 0;
        return;
    }

    // Use the full component width; we offset rendering by the screen inset in paint()
    const int W  = getWidth();
    const int nf = snap->buffer.getNumSamples();
    const int ch = snap->buffer.getNumChannels();

    peaks.assign ((size_t) W, 0.0f);

    for (int px = 0; px < W; ++px)
    {
        int s0 = (int) ((float) px       / (float) W * (float) nf);
        int s1 = (int) ((float) (px + 1) / (float) W * (float) nf);
        s1 = juce::jmax (s0 + 1, s1);
        s1 = juce::jmin (s1, nf);

        float mx = 0.0f;
        for (int c = 0; c < ch; ++c)
        {
            const float* buf = snap->buffer.getReadPointer (c);
            for (int s = s0; s < s1; ++s)
                mx = juce::jmax (mx, std::abs (buf[s]));
        }
        peaks[(size_t) px] = mx;
    }

    peakNumFrames = nf;
    peakWidth     = W;
}

//==============================================================================
// Mini waveform — renders all 8 modes inside the screen rect
//==============================================================================
void WaveformOverview::drawMiniWaveform (juce::Graphics& g,
                                          const juce::Rectangle<int>& screen,
                                          const juce::Rectangle<float>& vr) const
{
    if (peaks.empty()) return;

    const int W   = screen.getWidth();
    const int H   = screen.getHeight();
    const int ox  = screen.getX();
    const int oy  = screen.getY();
    const int numPeaks = juce::jmin ((int) peaks.size(), W);
    if (numPeaks <= 0) return;

    const float cy    = (float) oy + (float) H * 0.5f;
    const float scale = (float) H * 0.42f;   // slightly tighter than the main view

    const juce::Colour waveCol = getTheme().waveform;

    // Helper: pixel index inside 'screen' -> component-space x
    auto px2x = [&] (int px) -> float { return (float) (ox + px); };

    switch (waveformMode)
    {
        // ── Mode 0 : Hard ──────────────────────────────────────────────────
        default:
        case 0:
        {
            juce::Path fillPath;
            fillPath.startNewSubPath (px2x (0), cy - peaks[0] * scale);
            for (int px = 1; px < numPeaks; ++px)
                fillPath.lineTo (px2x (px), cy - peaks[(size_t) px] * scale);
            for (int px = numPeaks - 1; px >= 0; --px)
                fillPath.lineTo (px2x (px), cy + peaks[(size_t) px] * scale);
            fillPath.closeSubPath();

            g.setColour (waveCol);
            g.fillPath (fillPath);
            break;
        }

        // ── Mode 1 : Soft ──────────────────────────────────────────────────
        case 1:
        {
            juce::Path fillPath;
            fillPath.startNewSubPath (px2x (0), cy - peaks[0] * scale);
            for (int px = 1; px < numPeaks; ++px)
                fillPath.lineTo (px2x (px), cy - peaks[(size_t) px] * scale);
            for (int px = numPeaks - 1; px >= 0; --px)
                fillPath.lineTo (px2x (px), cy + peaks[(size_t) px] * scale);
            fillPath.closeSubPath();

            g.setColour (waveCol.withAlpha (0.70f));
            g.fillPath (fillPath);

            juce::Path topLine;
            topLine.startNewSubPath (px2x (0), cy - peaks[0] * scale);
            for (int px = 1; px < numPeaks; ++px)
                topLine.lineTo (px2x (px), cy - peaks[(size_t) px] * scale);
            g.setColour (waveCol.withAlpha (0.95f));
            g.strokePath (topLine, juce::PathStrokeType (1.2f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            break;
        }

        // ── Mode 2 : Outline ───────────────────────────────────────────────
        case 2:
        {
            juce::Path topPath, botPath;
            topPath.startNewSubPath (px2x (0), cy - peaks[0] * scale);
            botPath.startNewSubPath (px2x (0), cy + peaks[0] * scale);
            for (int px = 1; px < numPeaks; ++px)
            {
                topPath.lineTo (px2x (px), cy - peaks[(size_t) px] * scale);
                botPath.lineTo (px2x (px), cy + peaks[(size_t) px] * scale);
            }
            g.setColour (waveCol.withAlpha (0.25f));
            g.strokePath (topPath, juce::PathStrokeType (2.5f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.strokePath (botPath, juce::PathStrokeType (2.5f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (waveCol.withAlpha (0.90f));
            g.strokePath (topPath, juce::PathStrokeType (1.0f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.strokePath (botPath, juce::PathStrokeType (1.0f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            break;
        }

        // ── Mode 3 : Rectified ─────────────────────────────────────────────
        case 3:
        {
            const float baseline  = (float) oy + (float) H * 0.85f;
            const float rectScale = scale * 1.6f;

            juce::Path rectPath;
            rectPath.startNewSubPath (px2x (0), baseline - peaks[0] * rectScale);
            for (int px = 1; px < numPeaks; ++px)
                rectPath.lineTo (px2x (px), baseline - peaks[(size_t) px] * rectScale);
            rectPath.lineTo (px2x (numPeaks - 1), baseline);
            rectPath.lineTo (px2x (0), baseline);
            rectPath.closeSubPath();

            juce::ColourGradient grad (waveCol.withAlpha (0.60f), 0.0f, (float) oy,
                                       waveCol.withAlpha (0.05f), 0.0f, (float) (oy + H), false);
            g.setGradientFill (grad);
            g.fillPath (rectPath);

            juce::Path topLine;
            topLine.startNewSubPath (px2x (0), baseline - peaks[0] * rectScale);
            for (int px = 1; px < numPeaks; ++px)
                topLine.lineTo (px2x (px), baseline - peaks[(size_t) px] * rectScale);
            g.setColour (waveCol.withAlpha (0.90f));
            g.strokePath (topLine, juce::PathStrokeType (1.1f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            break;
        }

        // ── Mode 4 : Mirrored ──────────────────────────────────────────────
        case 4:
        {
            juce::Path upperPath, lowerPath;
            upperPath.startNewSubPath (px2x (0), cy);
            lowerPath.startNewSubPath (px2x (0), cy);
            for (int px = 0; px < numPeaks; ++px)
            {
                upperPath.lineTo (px2x (px), cy - peaks[(size_t) px] * scale);
                lowerPath.lineTo (px2x (px), cy + peaks[(size_t) px] * scale);
            }
            upperPath.lineTo (px2x (numPeaks - 1), cy);
            upperPath.closeSubPath();
            lowerPath.lineTo (px2x (numPeaks - 1), cy);
            lowerPath.closeSubPath();

            g.setColour (waveCol.withAlpha (0.75f));
            g.fillPath (upperPath);
            g.setColour (waveCol.withAlpha (0.35f));
            g.fillPath (lowerPath);

            juce::Path edgePath;
            edgePath.startNewSubPath (px2x (0), cy - peaks[0] * scale);
            for (int px = 1; px < numPeaks; ++px)
                edgePath.lineTo (px2x (px), cy - peaks[(size_t) px] * scale);
            g.setColour (waveCol.withAlpha (0.90f));
            g.strokePath (edgePath, juce::PathStrokeType (1.0f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            break;
        }

        // ── Mode 5 : Bars ──────────────────────────────────────────────────
        case 5:
        {
            for (int px = 0; px < numPeaks; ++px)
            {
                float topY = cy - peaks[(size_t) px] * scale;
                float botY = cy + peaks[(size_t) px] * scale;
                float barH = juce::jmax (1.0f, botY - topY);
                float alpha = 0.4f + peaks[(size_t) px] * 0.55f;
                g.setColour (waveCol.withAlpha (alpha));
                g.fillRect (px2x (px), topY, 1.0f, barH);
            }
            break;
        }

        // ── Mode 6 : RMS ───────────────────────────────────────────────────
        case 6:
        {
            juce::Path rmsPath;
            rmsPath.startNewSubPath (px2x (0), cy - peaks[0] * scale);
            for (int px = 1; px < numPeaks; ++px)
                rmsPath.lineTo (px2x (px), cy - peaks[(size_t) px] * scale);
            for (int px = numPeaks - 1; px >= 0; --px)
                rmsPath.lineTo (px2x (px), cy + peaks[(size_t) px] * scale);
            rmsPath.closeSubPath();

            juce::ColourGradient grad (
                waveCol.withAlpha (0.0f), 0.0f, 0.0f,
                waveCol.withAlpha (0.0f), 0.0f, (float) (oy + H), false);
            grad.addColour (0.35, waveCol.withAlpha (0.22f));
            grad.addColour (0.5,  waveCol.withAlpha (0.36f));
            grad.addColour (0.65, waveCol.withAlpha (0.22f));
            g.setGradientFill (grad);
            g.fillPath (rmsPath);

            juce::Path rmsLine;
            rmsLine.startNewSubPath (px2x (0), cy - peaks[0] * scale);
            for (int px = 1; px < numPeaks; ++px)
                rmsLine.lineTo (px2x (px), cy - peaks[(size_t) px] * scale);
            g.setColour (waveCol.withAlpha (0.28f));
            g.strokePath (rmsLine, juce::PathStrokeType (3.0f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (waveCol.withAlpha (0.95f));
            g.strokePath (rmsLine, juce::PathStrokeType (1.1f,
                juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            break;
        }

        // ── Mode 7 : Stepped ───────────────────────────────────────────────
        case 7:
        {
            const int stepW = juce::jmax (2, W / juce::jmax (1, numPeaks / 4));
            juce::Path stepFill;
            bool started = false;
            float lastY = cy;
            for (int px = 0; px < numPeaks; px += stepW)
            {
                float y = cy - peaks[(size_t) px] * scale;
                if (! started) { stepFill.startNewSubPath (px2x (px), y); started = true; }
                else { stepFill.lineTo (px2x (px), lastY); stepFill.lineTo (px2x (px), y); }
                stepFill.lineTo (px2x (juce::jmin (px + stepW, numPeaks - 1)), y);
                lastY = y;
            }
            juce::Path mirrorFill = stepFill;
            // Close upper half
            juce::Path upper = stepFill;
            upper.lineTo (px2x (numPeaks - 1), cy);
            upper.lineTo (px2x (0), cy);
            upper.closeSubPath();

            // Build lower mirror
            juce::Path lower;
            bool startedLow = false;
            float lastYL = cy;
            for (int px = 0; px < numPeaks; px += stepW)
            {
                float y = cy + peaks[(size_t) px] * scale;
                if (! startedLow) { lower.startNewSubPath (px2x (px), y); startedLow = true; }
                else { lower.lineTo (px2x (px), lastYL); lower.lineTo (px2x (px), y); }
                lower.lineTo (px2x (juce::jmin (px + stepW, numPeaks - 1)), y);
                lastYL = y;
            }
            lower.lineTo (px2x (numPeaks - 1), cy);
            lower.lineTo (px2x (0), cy);
            lower.closeSubPath();

            g.setColour (waveCol.withAlpha (0.70f));
            g.fillPath (upper);
            g.setColour (waveCol.withAlpha (0.30f));
            g.fillPath (lower);

            // Top edge outline
            g.setColour (waveCol.withAlpha (0.95f));
            g.strokePath (stepFill, juce::PathStrokeType (1.0f));
            break;
        }
    }
}

//==============================================================================
// Paint
//==============================================================================
void WaveformOverview::paint (juce::Graphics& g)
{
    auto snap = processor.sampleData.getSnapshot();
    const bool hasSample = snap && snap->buffer.getNumSamples() > 0;

    if (hasSample)
    {
        const int nf = snap->buffer.getNumSamples();
        if (nf != peakNumFrames || getWidth() != peakWidth)
            rebuildPeaks();
    }

    // ── LCD-style frame — identical to SliceControlBar::paint() ─────────────
    const auto ac = getTheme().accent;
    auto b = getLocalBounds();

    juce::ColourGradient outerGrad (juce::Colour (0xFF131313), 0, 0,
                                    juce::Colour (0xFF0E0E0E), 0, (float) b.getHeight(), false);
    g.setGradientFill (outerGrad);
    g.fillRoundedRectangle (b.toFloat(), 4.0f);

    // Outer glow halo
    g.setColour (ac.withAlpha (0.18f));
    g.drawRoundedRectangle (b.toFloat().expanded (1.0f), 5.0f, 1.0f);
    // Border
    g.setColour (ac.withAlpha (0.65f));
    g.drawRoundedRectangle (b.toFloat().reduced (0.5f), 4.0f, 1.0f);

    auto screen = b.reduced (4);
    g.setColour (getTheme().darkBar.darker (0.55f));
    g.fillRoundedRectangle (screen.toFloat(), 2.0f);

    // Scanline texture
    g.setColour (juce::Colours::black.withAlpha (0.18f));
    for (int y = screen.getY(); y < screen.getBottom(); y += 2)
        g.drawHorizontalLine (y, (float) screen.getX(), (float) screen.getRight());

    // Top glow
    juce::ColourGradient glow (ac.withAlpha (0.06f), 0.f, (float) screen.getY(),
                                juce::Colours::transparentBlack, 0.f, (float) (screen.getY() + 18), false);
    g.setGradientFill (glow);
    g.fillRoundedRectangle (screen.toFloat(), 2.0f);

    // Screen border
    g.setColour (ac.withAlpha (0.12f));
    g.drawRoundedRectangle (screen.toFloat().expanded (0.5f), 2.0f, 1.0f);
    // ── End frame ─────────────────────────────────────────────────────────────

    const float W = (float) screen.getWidth();
    const float H = (float) screen.getHeight();
    const auto  vr = viewportRect (screen);

    if (hasSample && ! peaks.empty())
    {
        // Clip drawing to screen interior
        g.reduceClipRegion (screen);

        // ── Waveform — rendered in theme waveform colour, mode-aware ──────
        drawMiniWaveform (g, screen, vr);

        // Dim outside viewport
        g.setColour (juce::Colours::black.withAlpha (0.38f));
        if (vr.getX() > (float) screen.getX())
            g.fillRect ((float) screen.getX(), (float) screen.getY(),
                        vr.getX() - (float) screen.getX(), H);
        if (vr.getRight() < (float) screen.getRight())
            g.fillRect (vr.getRight(), (float) screen.getY(),
                        (float) screen.getRight() - vr.getRight(), H);

        // Viewport tint fill
        g.setColour (ac.withAlpha (0.10f));
        g.fillRect (vr);

        // Viewport border
        g.setColour (ac.withAlpha (0.85f));
        g.drawRect (vr, 1.0f);

        // Left handle
        const juce::Rectangle<float> lh (vr.getX(), (float) screen.getY(),
                                          (float) kHandleW, H);
        g.setColour (ac.withAlpha (0.90f));
        g.fillRect (lh);
        const float mid = (float) screen.getCentreY();
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        for (int i = -1; i <= 1; ++i)
            g.drawHorizontalLine ((int) (mid + (float) i * 3.0f),
                                  lh.getX() + 1.5f, lh.getRight() - 1.5f);

        // Right handle
        const juce::Rectangle<float> rh (vr.getRight() - (float) kHandleW,
                                          (float) screen.getY(), (float) kHandleW, H);
        g.setColour (ac.withAlpha (0.90f));
        g.fillRect (rh);
        for (int i = -1; i <= 1; ++i)
            g.drawHorizontalLine ((int) (mid + (float) i * 3.0f),
                                  rh.getX() + 1.5f, rh.getRight() - 1.5f);
    }
    else
    {
        // No sample — dim centre line
        g.setColour (getTheme().foreground.withAlpha (0.10f));
        g.drawHorizontalLine (screen.getCentreY(),
                              (float) screen.getX(), (float) screen.getRight());
    }
}

//==============================================================================
// Mouse — all hit-testing now in screen-space
//==============================================================================
void WaveformOverview::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isRightButtonDown())
    {
        using F = DysektProcessor;
        const auto screenPos = e.getScreenPosition();
        juce::PopupMenu menu;
        menu.addSectionHeader ("MIDI Learn");

        // Zoom
        const bool zoomMapped = processor.midiLearn.isMapped (F::FieldZoom);
        menu.addItem (1, "Learn Zoom CC");
        if (zoomMapped)
            menu.addItem (2, "Clear Zoom (" + processor.midiLearn.getLabelText (F::FieldZoom) + ")");

        menu.addSeparator();

        // Scroll
        const bool scrollMapped = processor.midiLearn.isMapped (F::FieldScroll);
        menu.addItem (3, "Learn Scroll CC");
        if (scrollMapped)
            menu.addItem (4, "Clear Scroll (" + processor.midiLearn.getLabelText (F::FieldScroll) + ")");

        menu.addSeparator();
        menu.addItem (1000, "Open MIDI Learn Dialog...");

        auto* topLvl = getTopLevelComponent();
        float ms = DysektLookAndFeel::getMenuScale();
        menu.showMenuAsync (
            juce::PopupMenu::Options()
                .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1))
                .withParentComponent (topLvl)
                .withStandardItemHeight ((int)(24 * ms)),
            [this] (int result) {
                using F = DysektProcessor;
                if      (result == 1) { processor.midiLearn.armLearn (F::FieldZoom);   repaint(); }
                else if (result == 2) { processor.midiLearn.clearMapping (F::FieldZoom);   repaint(); }
                else if (result == 3) { processor.midiLearn.armLearn (F::FieldScroll); repaint(); }
                else if (result == 4) { processor.midiLearn.clearMapping (F::FieldScroll); repaint(); }
                else if (result == 1000)
                {
                    if (auto* editor = findParentComponentOfClass<DysektEditor>())
                        editor->keyPressed (juce::KeyPress ('M', juce::ModifierKeys::commandModifier, 0));
                }
            }
        );
        return;
    }

    auto snap = processor.sampleData.getSnapshot();
    if (! snap || snap->buffer.getNumSamples() == 0) return;

    auto screen = getLocalBounds().reduced (4);

    isDragging      = true;
    dragStartScroll = processor.scroll.load();
    dragStartZoom   = juce::jmax (1.0f, processor.zoom.load());
    dragStartX      = e.x;

    const float W  = (float) screen.getWidth();
    const float mx = (float) e.x;
    const auto  vr = viewportRect (screen);
    const int   nf = snap->buffer.getNumSamples();

    const juce::Rectangle<float> lh (vr.getX(), (float) screen.getY(),
                                      (float) kHandleW, (float) screen.getHeight());
    const juce::Rectangle<float> rh (vr.getRight() - (float) kHandleW,
                                      (float) screen.getY(), (float) kHandleW, (float) screen.getHeight());

    if (lh.contains (mx, (float) e.y))
    {
        dragMode       = DragMode::ResizeLeft;
        dragFixedFrac  = (vr.getRight() - (float) screen.getX()) / W;
        dragMovingFrac = (vr.getX()     - (float) screen.getX()) / W;
    }
    else if (rh.contains (mx, (float) e.y))
    {
        dragMode       = DragMode::ResizeRight;
        dragFixedFrac  = (vr.getX()     - (float) screen.getX()) / W;
        dragMovingFrac = (vr.getRight() - (float) screen.getX()) / W;
    }
    else if (vr.contains (mx, (float) e.y))
    {
        dragMode      = DragMode::Scroll;
        dragFixedFrac = (mx - vr.getX()) / W;   // offset within viewport
    }
    else
    {
        // Click outside: jump-scroll so viewport centres on click
        dragMode = DragMode::Scroll;

        const int   visLen    = juce::jlimit (1, nf, (int) ((float) nf / dragStartZoom));
        const int   maxStart  = juce::jmax (1, nf - visLen);
        const float clickFrac = (mx - (float) screen.getX()) / W;
        const float clickSample = clickFrac * (float) nf;
        const float newStart    = juce::jlimit (0.0f, (float) maxStart,
                                                 clickSample - (float) visLen * 0.5f);
        processor.scroll.store (newStart / (float) maxStart);
        dragStartScroll = processor.scroll.load();
        dragFixedFrac   = (float) visLen * 0.5f / W;
    }

    repaint();
}

void WaveformOverview::mouseDrag (const juce::MouseEvent& e)
{
    if (! isDragging) return;

    auto snap = processor.sampleData.getSnapshot();
    if (! snap || snap->buffer.getNumSamples() == 0) return;

    auto screen = getLocalBounds().reduced (4);
    const float W   = (float) screen.getWidth();
    const int   nf  = snap->buffer.getNumSamples();
    const float dxF = (float) (e.x - dragStartX) / W;

    if (dragMode == DragMode::Scroll)
    {
        const int   visLen   = juce::jlimit (1, nf, (int) ((float) nf / dragStartZoom));
        const int   maxStart = juce::jmax (1, nf - visLen);
        const float dScroll  = dxF * (float) nf / (float) maxStart;
        processor.scroll.store (juce::jlimit (0.0f, 1.0f, dragStartScroll + dScroll));
    }
    else if (dragMode == DragMode::ResizeLeft)
    {
        const float rightFrac = dragFixedFrac;
        const float leftFrac  = juce::jlimit (0.0f,
                                               rightFrac - kMinViewFrac,
                                               dragMovingFrac + dxF);
        const float viewFrac  = rightFrac - leftFrac;
        const float newZoom   = juce::jmax (1.0f, 1.0f / viewFrac);

        const int visLen   = juce::jlimit (1, nf, (int) ((float) nf / newZoom));
        const int maxStart = juce::jmax (1, nf - visLen);
        const float newStart = leftFrac * (float) nf;
        processor.zoom.store   (newZoom);
        processor.scroll.store (juce::jlimit (0.0f, 1.0f, newStart / (float) maxStart));
    }
    else if (dragMode == DragMode::ResizeRight)
    {
        const float leftFrac  = dragFixedFrac;
        const float rightFrac = juce::jlimit (leftFrac + kMinViewFrac,
                                               1.0f,
                                               dragMovingFrac + dxF);
        const float viewFrac  = rightFrac - leftFrac;
        const float newZoom   = juce::jmax (1.0f, 1.0f / viewFrac);

        const int visLen   = juce::jlimit (1, nf, (int) ((float) nf / newZoom));
        const int maxStart = juce::jmax (1, nf - visLen);
        const float newStart = leftFrac * (float) nf;
        processor.zoom.store   (newZoom);
        processor.scroll.store (juce::jlimit (0.0f, 1.0f, newStart / (float) maxStart));
    }

    repaint();
}

void WaveformOverview::mouseUp (const juce::MouseEvent&)
{
    isDragging = false;
    dragMode   = DragMode::None;
    repaint();
}

void WaveformOverview::mouseWheelMove (const juce::MouseEvent& e,
                                        const juce::MouseWheelDetails& w)
{
    auto snap = processor.sampleData.getSnapshot();
    if (! snap || snap->buffer.getNumSamples() == 0) return;

    auto screen = getLocalBounds().reduced (4);
    const float W        = (float) screen.getWidth();
    const int   nf       = snap->buffer.getNumSamples();
    const float oldZoom  = juce::jmax (1.0f, processor.zoom.load());
    const float oldSc    = processor.scroll.load();

    const float newZoom  = juce::jmax (1.0f, oldZoom * (1.0f + w.deltaY * kZoomWheelSens));

    // Anchor to cursor — keep the sample under the mouse stationary
    const float anchorFrac   = ((float) e.x - (float) screen.getX()) / W;
    const float anchorSample = anchorFrac * (float) nf;

    const float newVisLen   = (float) nf / newZoom;
    const float newMaxStart = juce::jmax (1.0f, (float) nf - newVisLen);
    const float newStart    = anchorSample - anchorFrac * newVisLen;

    processor.zoom.store   (newZoom);
    processor.scroll.store (juce::jlimit (0.0f, 1.0f, newStart / newMaxStart));

    repaint();
}
