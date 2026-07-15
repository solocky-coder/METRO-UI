#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../MidiLearnManager.h"

class DysektProcessor;

/**  HALion-style full-sample minimap overview strip.
 *
 *   Draws in the same LCD-frame style as SliceControlBar — outer gradient
 *   shell, accent-coloured border, inner darkBar screen, scanline texture,
 *   top glow — using getTheme() colours so it follows every theme change.
 *
 *   The waveform inside the screen mirrors the current waveformMode (Hard /
 *   Soft / Outline / Rectified / Mirrored / Bars / RMS / Stepped).
 *
 *   HIDDEN IN TRIM MODE — the editor's resized() already gives this component
 *   zero bounds when trimDialog != nullptr, so no extra logic is needed here.
 *
 *   Interactions (HALion-style):
 *     - Drag left handle         -> resize left edge (zoom), right edge fixed
 *     - Drag right handle        -> resize right edge (zoom), left edge fixed
 *     - Drag inside viewport box -> scroll
 *     - Click outside viewport   -> jump-scroll so viewport centres on click
 *     - Mouse wheel              -> zoom anchored to cursor
 */
class WaveformOverview : public juce::Component
{
public:
    explicit WaveformOverview (DysektProcessor& p);

    void paint           (juce::Graphics& g) override;
    void mouseDown       (const juce::MouseEvent& e) override;
    void mouseDrag       (const juce::MouseEvent& e) override;
    void mouseUp         (const juce::MouseEvent& e) override;
    void mouseWheelMove  (const juce::MouseEvent& e,
                          const juce::MouseWheelDetails& w) override;

    bool isDraggingNow() const noexcept { return isDragging; }

    /** Mirror the editor's current waveformMode (0-7). */
    void setWaveformMode (int mode) noexcept { waveformMode = mode; repaint(); }

    /** Call from the editor's timerCallback to keep the display current. */
    void repaintOverview();

private:
    DysektProcessor& processor;

    int waveformMode { 0 };   // 0=Hard 1=Soft 2=Outline 3=Rect 4=Mirrored 5=Bars 6=RMS 7=Stepped

    // Peak cache — one max-abs value per pixel column, rebuilt on sample/size change
    std::vector<float> peaks;
    int peakNumFrames { 0 };
    int peakWidth     { 0 };

    void rebuildPeaks();
    juce::Rectangle<float> viewportRect (const juce::Rectangle<int>& screen) const;
    void drawMiniWaveform (juce::Graphics& g,
                           const juce::Rectangle<int>& screen,
                           const juce::Rectangle<float>& vr) const;

    enum class DragMode { None, Scroll, ResizeLeft, ResizeRight };

    bool     isDragging     { false };
    DragMode dragMode       { DragMode::None };

    // State captured at mouseDown
    float dragStartScroll   { 0.0f };
    float dragStartZoom     { 1.0f };
    float dragFixedFrac     { 0.0f };
    float dragMovingFrac    { 0.0f };
    int   dragStartX        { 0 };

    static constexpr int   kHandleW       = 7;
    static constexpr float kMinViewFrac   = 0.01f;
    static constexpr float kZoomWheelSens = 0.18f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformOverview)
};
