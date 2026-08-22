#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../audio/multisampler/MultisamplerInstrument.h"
#include "../audio/SampleData.h"

class DysektProcessor;

/**  Dedicated right LCD panel for the MULTISAMPLER tab (uiMode == 1).
 *
 *   Repurposes the presentation/interaction shell that used to live in the
 *   now-retired SfzWaveformLcd (LCD styling, waveform backdrop, ADSR nodes,
 *   hit testing, dragging, labels, wheel handling) but reads its data from
 *   a MultisamplerInstrument's selected SampleZone instead of the obsolete
 *   sfzPlayer2/sampleData2 SFZ-Player-2 pipeline. This is the same data
 *   path SliceWaveformLcd's MULTISAMPLER branch used to provide before the
 *   two engines shared one component (see METRO-UI Multisampler Waveform
 *   LCD Implementation Plan) — moved here so the Slicer/SFZ-PLAYER LCD can
 *   go back to being sliceManager/sliceManager2-only.
 *
 *   The component keeps a non-owning instrument pointer (the editor owns
 *   the model) but tracks the selected zone by its stable juce::Uuid
 *   internally, re-resolving that id against the current zone list on
 *   every rebuild — this keeps the display (and any edit in flight) from
 *   silently pointing at the wrong zone after insertion, deletion, or
 *   reordering elsewhere in MULTISAMPLER's UI.
 *
 *   Owns independent zoom/scroll state — never writes into
 *   processor.zoom/processor.scroll, which belong to the Slicer view.
 *
 *   Dimensions match SliceWaveformLcd::kPreferredHeight so the two panels
 *   swap into the same right-panel bounds.
 *   Call repaintLcd() from the editor's timerCallback() at ~30 Hz.
 */
class MultisamplerWaveformLcd : public juce::Component
{
public:
    explicit MultisamplerWaveformLcd (DysektProcessor& p);

    void paint     (juce::Graphics& g) override;
    void resized   () override;
    void mouseMove (const juce::MouseEvent& e) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp        (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override;

    /** Called by PluginEditor whenever MULTISAMPLER's selected/hovered zone
        changes — mirrors SliceLcdDisplay::setMultisamplerSource and the
        retired SliceWaveformLcd::setMultisamplerSource. `instrument` may be
        nullptr (nothing loaded); `selectedZoneIndex` may be -1 (nothing
        selected) or out of range (stale index mid-edit) — both handled
        safely, falling through to the appropriate empty state. */
    void setSource (const MultisamplerInstrument* instrument, int selectedZoneIndex);

    /** Equivalent to setSource (nullptr, -1) — used when the MULTISAMPLER
        tab is not active, so the component holds no stale instrument/zone
        pointers while hidden. */
    void clearSource();

    void repaintLcd();

    /** Set the current waveform display style (0-7) and repaint. Mirrors
        SliceWaveformLcd::setWaveformMode / WaveformView::setWaveformMode so
        the WAVE icon toggle drives this display too.
        0=Hard 1=Soft 2=Outline 3=Rectified 4=Mirrored 5=Bars 6=RMS 7=Stepped */
    void setWaveformMode (int mode) { waveformMode = juce::jlimit (0, 7, mode); repaint(); }
    int  getWaveformMode() const noexcept { return waveformMode; }

    /** Fired on every ADSR node drag frame. `zoneIndex` is the selected
        zone's *current* index (re-resolved from its stable id, so it stays
        correct even if the list was reordered since setSource() was last
        called). `field` is one of SliceControlBar::ZoneAttack/ZoneDecay/
        ZoneSustain/ZoneRelease (SfzZoneField); `value` is in that field's
        native units (seconds for Attack/Decay/Release, 0..1 for Sustain) —
        the same convention MultisamplerEditor::applySliceControlBarFieldEdit
        already expects from the SCB's own numeric ADSR cells. */
    std::function<void (int zoneIndex, int field, float value)> onZoneParamEdited;

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
        float ax    { 0.07f };   // attack peak X
        float ay    { 0.04f };   // attack peak Y (near 0 = loud)
        float dx    { 0.25f };   // decay end X
        float sy    { 0.30f };   // sustain Y level
        float rx    { 0.99f };   // release start X
        float sxEnd { 0.99f };   // sustain plateau end X (tracks rx)
    } env;

    juce::Array<EnvNode> envNodes;

    // ── Selected-zone resolution ──────────────────────────────────────────────
    const MultisamplerInstrument* instrument { nullptr };
    juce::Uuid selectedZoneId;          // stable identity of the shown zone
    int        lastSetIndex { -1 };     // index most recently passed to setSource()

    /** Re-resolves selectedZoneId against the current instrument->zones list
        and returns the matching zone, or nullptr if the instrument is gone,
        empty, or the id is no longer present (deleted). Also writes the
        zone's current index to outIndex when found (-1 otherwise). */
    const SampleZone* resolveSelectedZone (int& outIndex) const;

    void buildEnvelopeNodes();   // read selected zone's ADSR → env / envNodes
    void commitNodes();          // env → onZoneParamEdited

    // ── Display data for the selected zone ────────────────────────────────────
    struct DisplayData
    {
        bool   hasInstrument { false };   // instrument loaded, may have zero zones
        bool   hasZones      { false };
        bool   hasSelection  { false };   // a resolved, in-range zone
        bool   missingSample { false };
        int    zoneIndex     { -1 };
        int    startSample   { 0 };
        int    endSample     { 0 };
        int    totalFrames   { 0 };
        double sampleRate    { 44100.0 };
        juce::String sampleName;
    } data;

    void buildDisplayData();

    // ── Draw helpers ──────────────────────────────────────────────────────────
    void drawBackground    (juce::Graphics& g);
    void drawWaveform       (juce::Graphics& g, const juce::Rectangle<float>& area);
    void drawEnvelope      (juce::Graphics& g, const juce::Rectangle<float>& area);
    void drawNodes         (juce::Graphics& g, const juce::Rectangle<float>& area);
    void drawHeader        (juce::Graphics& g, const juce::Rectangle<float>& area);
    void drawNoData        (juce::Graphics& g);
    void drawPlayhead      (juce::Graphics& g, const juce::Rectangle<float>& area);
    void drawLoopOverlay   (juce::Graphics& g, const juce::Rectangle<float>& area);
    void drawSegmentLabel  (juce::Graphics& g,
                            float x0, float y0, float x1, float y1,
                            const char* text, juce::Colour col,
                            const juce::Rectangle<float>& area);

    float getSliceDurMs() const;  // duration of the selected zone's playable range, in ms
    float envAt (float xn) const; // interpolated envelope Y at position xn

    NodeRole hitTest (juce::Point<float> pos) const;

    DysektProcessor& processor;

    // ── Decoded sample for the selected zone ──────────────────────────────────
    // A SampleZone points at a file on disk, not a pre-decoded buffer — decode
    // it into a real SampleData instance so the existing
    // DysektProcessor::getWaveformPeakAtIn() peak read works unmodified. Only
    // re-decoded when the shown zone's file path actually changes.
    SampleData  zoneSampleData;
    juce::File  decodedFile;
    void        decodeZoneFile (const juce::File& f);

    // ── Waveform backdrop (independent zoom/scroll — never touches
    //    processor.zoom/processor.scroll, which belong to the Slicer view) ────
    static constexpr int kPeaks = 256;
    juce::Array<float> peaks;
    float zoom   { 1.0f };
    float scroll { 0.0f };
    // Visible window in zoneSampleData's own frame space — set by
    // buildWaveformPeaks(), read by drawLoopOverlay() so the loop markers
    // line up with the same zoom/scroll window the peaks were sampled from.
    int windowStartFrame { 0 };
    int windowEndFrame   { 0 };
    void buildWaveformPeaks();

    // ── Rebuild/version guards ─────────────────────────────────────────────────
    juce::Uuid lastBuiltZoneId;          // zone id peaks/envelope were last built for
    juce::File lastBuiltSampleFile;      // sampleFile path last built against
    int64_t    lastBuiltSampleStart { -1 };
    int64_t    lastBuiltSampleEnd   { -1 };
    float      lastBuiltZoom     { -1.0f };
    float      lastBuiltScroll   { -1.0f };
    bool       lastBuiltMissing  { false };
    bool       forceRebuild { true };

    int waveformMode { 0 };   // 0=Hard 1=Soft 2=Outline 3=Rectified 4=Mirrored 5=Bars 6=RMS 7=Stepped

    NodeRole dragRole { NodeRole::None };
    NodeRole hovRole  { NodeRole::None };
    int      postCommitGuard { 0 };

    juce::Rectangle<float> screenArea;

    static constexpr int   kScanlineAlpha = 18;
    static constexpr float kNodeR         = 14.0f;
    static constexpr float kHitR          = 26.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultisamplerWaveformLcd)
};
