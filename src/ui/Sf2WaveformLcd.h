#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

class DysektProcessor;

/**
 * Right LCD panel shown when uiMode == 2 (SF2-Player / FluidSynth).
 *
 * ADSR UI ownership:
 *   - This component is the SF2-Player's ADSR editor.
 *   - It edits the SF2 player's envelope through processor.sfzPlayer and
 *     synchronizes the JUCE ADSR values consumed by the FluidSynth path.
 *   - The setSfzAttack/Decay/Sustain/Release method names are legacy naming;
 *     in this component they refer to the SF2-Player's stored envelope values,
 *     not the separate SFZ-Player (processor.sfzPlayer2).
 *   - The UI here is a draggable envelope graph, not four rotary knobs.
 *
 * No APVTS writes: these envelope values are atomics, not registered
 * parameters.
 *
 * Uses processor.sampleData3 for the waveform backdrop — its own independent
 * render via SoundFontLoadTarget::SfPlayer, decoupled from the Slicer's
 * sampleData and from the SFZ-PLAYER tab's sampleData2 (sliceManager2 /
 * voicePool2). The SFZ-PLAYER tab uses SliceLcdDisplay/SliceWaveformLcd in
 * mode-aware fashion instead of this dedicated SF2 component.
 *
 * Call repaintLcd() from the editor's timerCallback() at ~30 Hz.
 */
class Sf2WaveformLcd : public juce::Component
{
public:
    explicit Sf2WaveformLcd (DysektProcessor& p);

    void paint     (juce::Graphics& g) override;
    void resized   () override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp        (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

    void repaintLcd();

    void setWaveformMode (int mode) { waveformMode = juce::jlimit (0, 7, mode); repaint(); }
    int  getWaveformMode () const   { return waveformMode; }

    static constexpr int kPreferredHeight = 136;

private:
    // ── Node roles ────────────────────────────────────────────────────────────
    enum class NodeRole { None, Attack, Decay, Sustain, Release };

    struct EnvNode
    {
        float      xn    { 0.0f };
        float      yn    { 0.0f };
        NodeRole   role  { NodeRole::None };
        juce::Colour colour;
        const char* label { nullptr };
    };

    // ── Normalised envelope state ─────────────────────────────────────────────
    struct
    {
        float ax    { 0.07f };
        float ay    { 0.04f };
        float dx    { 0.25f };
        float sy    { 0.30f };
        float rx    { 0.99f };
        float sxEnd { 0.99f };
    } env;

    juce::Array<EnvNode> envNodes;

    void buildEnvelopeNodes();
    void commitNodes();

    // ── Draw helpers ──────────────────────────────────────────────────────────
    void drawBackground    (juce::Graphics& g);
    void drawEnvelope      (juce::Graphics& g, const juce::Rectangle<float>& area);
    void drawNodes         (juce::Graphics& g, const juce::Rectangle<float>& area);
    void drawHeader        (juce::Graphics& g, const juce::Rectangle<float>& area);
    void drawNoInstrument  (juce::Graphics& g);
    void drawPlayhead      (juce::Graphics& g, const juce::Rectangle<float>& area);
    void drawSegmentLabel  (juce::Graphics& g,
                            float x0, float y0, float x1, float y1,
                            const char* text, juce::Colour col,
                            const juce::Rectangle<float>& area);

    NodeRole hitTest (juce::Point<float> pos) const;

    DysektProcessor& processor;

    // ── Waveform backdrop ────────────────────────────────────────────────────
    static constexpr int kPeaks = 512;
    juce::Array<float> peaks;
    int   cachedTotalFrames { 0 };
    float cachedZoom   { 1.0f };
    float cachedScroll { 0.0f };
    void buildWaveformPeaks();
    void drawWaveformBackdrop (juce::Graphics& g, const juce::Rectangle<float>& area);
    void drawLoopOverlay      (juce::Graphics& g, const juce::Rectangle<float>& area);

    int waveformMode { 0 };

    NodeRole dragRole { NodeRole::None };
    NodeRole hovRole  { NodeRole::None };
    int      postCommitGuard { 0 };

    juce::Rectangle<float> screenArea;

    static constexpr int   kScanlineAlpha = 18;
    static constexpr float kNodeR         = 14.0f;
    static constexpr float kHitR          = 26.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sf2WaveformLcd)
};
