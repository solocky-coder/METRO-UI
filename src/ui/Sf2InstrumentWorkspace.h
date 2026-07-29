#pragma once
// =============================================================================
//  Sf2InstrumentWorkspace.h  —  3-column SF2 instrument workspace
// =============================================================================
//  Full rewrite, drafted literally against sf2-metro-reference-keyboard.svg
//  (the approved mockup), NOT against the previous "reflow the old panel into
//  three columns" build. Layout, per the mockup's 1370x414 reference panel:
//
//    [ Col 1 ~25.5% ] [ Col 2 ~34.8% ] [ Col 3 ~34.1% ]
//     Preset browser    Active preset      Performance & FX
//                        + Amp envelope
//
//  Docked top bar (36px): title, "PRESET BROWSER / PERFORMANCE / ENVELOPE"
//  caption, and a SOUNDFONT LOADED status pill (green dot when a file is
//  loaded, per Sf2ProgramGrid's existing load-state signal).
//
//  Column 1 — Preset browser (mockup: no more 8-col grid):
//    • Search box (juce::TextEditor, "⌕  Search presets" placeholder)
//    • Single-column preset list, selected row highlighted with the accent
//      left-bar treatment shown in the mockup (#22c3ee), bank/program badge
//      right-aligned per row
//    • "BANK 000  ·  GENERAL MIDI" footer + "BROWSE SF2" button (opens the
//      shared FileBrowserPanel, same as SfzDropdownPanel's old file-open path)
//    Left-click auditions a program change; right-click assigns a MIDI
//    channel — channel-assignment/collision rules ported unchanged from
//    SfzDropdownPanel (channels 1-2 reserved, chromatic-slice/SFZ-Player
//    channels blocked). List/search state is presentation-only; the
//    underlying assignment bookkeeping still goes through Sf2ProgramGrid's
//    existing data, so getProgramGrid() keeps returning real state even
//    though the grid itself is no longer painted.
//
//  Column 2 top — Active preset + 3 knobs (LEVEL / TRANSPOSE / PAN):
//    Wired straight to SfzPlayer::setVolume/setTranspose/setPan (no
//    pushCommand/MIDI-learn indirection for the value itself; right-click
//    still opens the MIDI-learn menu on the global SliceParamField IDs,
//    exactly as SfzDropdownPanel did).
//
//    ⚠ DEVIATION FROM THE LITERAL MOCKUP: the SVG's knob row has only 3
//    knobs and drops FineTune entirely — it is not present anywhere in the
//    mockup. Silently deleting fine-tune access would be a functionality
//    regression the SVG doesn't call out, so it is kept as a small secondary
//    stepper docked under the Transpose knob (fineZone), visually
//    subordinate to the 3 primary knobs so the mockup's proportions and
//    reading order are preserved. Flagging this here rather than baking it
//    in silently — worth confirming against the design before shipping.
//
//  Column 2 bottom — SF2 filter controls (permanent, no tabs):
//    Live CUTOFF (Hz/kHz) and RESONANCE (%) sliders backed by
//    SfzPlayer::getSf2FilterCutoff/Resonance and setSf2FilterCutoff/Resonance,
//    which drive FluidSynth's GEN_FILTERFC/GEN_FILTERQ generators (see
//    SfzPlayer::applyFluidFilterFromUi()). SF2/FluidSynth only.
//    ⚠ DEVIATION FROM THE LITERAL MOCKUP: the mockup shows a permanent AMP
//    ENVELOPE display here instead. That was dropped rather than tabbed
//    against Filter (an earlier iteration of this panel tried the tab
//    approach) because the same A/D/S/R state this graph showed is already
//    live in two other places on screen at once — the top-bar LCD text
//    readout AND the draggable envelope overlay on the waveform view
//    (Sf2WaveformLcd) — so a third, read-only copy added nothing. Filter is
//    the only thing that wasn't already live elsewhere, so it now owns this
//    space outright with no tab/mode switch needed.
//
//  Column 3 — Performance & FX:
//    • REVERB SEND slider (SfzPlayer::getReverbMix/setReverbMix, 0-100%)
//    • REVERB DAMP  slider (SfzPlayer::getReverbDamp/setReverbDamp, 0-100%)
//    • Decaying NOTE ACTIVITY meter (bars), driven by processor.sfzActiveNotes,
//      docked immediately above the compact keyboard.
//    • Compact keyboard, C3-C5 only, drawn by our own CompactKeyboard nested
//      class rather than the shared KeysPanel. KeysPanel is a full 128-key
//      component with an attached sample-zone matrix ("+ ZONE" editor, "No
//      zones loaded" placeholder, etc.) — it has no API to restrict its
//      visible range, so embedding it here at ~110px meant the zone-matrix
//      placeholder ate most of the height and the real keys got squeezed
//      into a sliver, plus 75 white keys crammed into column 3's width read
//      as "too narrow". A small dedicated component avoids all of that.
//
//  Column 3 ALSO carries the explicit mixer-toggle decided in the prior
//    review: a small tab pair — "Performance & FX" (this literal-mockup
//    state, shown whenever <=1 MIDI channel has an assigned preset) vs.
//    "Channel Mixer" (Sf2ChannelFxPanel, shown once more than one channel is
//    multitimbral-assigned). The mockup only depicts the single-preset
//    state; the toggle is new UI not present in the SVG, added deliberately
//    instead of silently keeping a 4th de-facto column.
//
//  Below ~760 px wide, columns stack top-to-bottom instead of side-by-side
//  (kNarrowThreshold), same responsive behaviour as before.
//
//  Public API mirrors SfzDropdownPanel so the PluginEditor swap stays
//  mechanical: onFileChosen(), panelDidShow(), onFileLoaded,
//  onPresetChannelAssigned, notifyPresetChannelChanged(), getProgramGrid().
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "Sf2ProgramGrid.h"
#include "Sf2ChannelFxPanel.h"
#include "../audio/SfzPlayer.h"

class DysektProcessor;

class Sf2InstrumentWorkspace : public juce::Component,
                                public juce::Timer,
                                public juce::FileDragAndDropTarget
{
public:
    explicit Sf2InstrumentWorkspace (DysektProcessor& p);
    ~Sf2InstrumentWorkspace() override;

    // ── Core overrides ────────────────────────────────────────────────────────
    void paint   (juce::Graphics&) override;
    void resized () override;
    void timerCallback() override;

    // ── FileDragAndDropTarget ─────────────────────────────────────────────────
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    // ── Public API (mirrors SfzDropdownPanel) ─────────────────────────────────
    void panelDidShow();

    /** Called after a new SF2 file has been accepted. */
    std::function<void (const juce::File&)> onFileLoaded;

    /** Accepts a newly-picked .sf2 file (routed here from PluginEditor's
     *  shared FileBrowserPanel::onLoadRequest when uiMode == SF2-PLAYER). */
    void onFileChosen (const juce::File& f);

    /** Fired when the user right-clicks a preset row and assigns a MIDI channel. */
    std::function<void (const Sf2PresetInfo&, int midiChannel1Based)> onPresetChannelAssigned;

    /** Called by PluginEditor whenever a preset<->channel mapping changes. */
    void notifyPresetChannelChanged (const juce::String& presetName, int midiCh1Based);

    /** Called by PluginEditor when the Arranger's selected track is a genuine
     *  SF2 preset track (see ArrangeView::onTrackTypeSelected). Looks up
     *  whichever preset row matches the track's own bank/program — the
     *  actual authoritative preset<->track link (SequencerTrackInfo::preset,
     *  set wherever the track was assigned, e.g. TrackInspector's PART
     *  dropdown) — NOT Sf2ProgramGrid::getPresetChannels(), which only knows
     *  about assignments made through this panel's own right-click menu and
     *  is a separate map entirely; a track assigned via the Arranger never
     *  appears there. If found, selects/loads it exactly as a left-click on
     *  that row would (updates the ACTIVE PRESET header, envelope, and list
     *  highlight). No-op, deliberately, if no matching preset is found in
     *  the currently-loaded preset list. */
    void selectPresetForTrack (int presetBank, int presetProgram);

    /** Direct access to the SF2 program grid (read-only) for PluginEditor.
     *  Still backs the preset list even though Sf2ProgramGrid's own grid
     *  paint route is no longer used in column 1. */
    const Sf2ProgramGrid& getProgramGrid() const noexcept { return programGrid; }

private:
    // Nested classes (not free classes in an anonymous namespace) so they
    // get automatic access to our private members per C++11 nested-class
    // access rules, without needing friend declarations.
    class PresetListModel;
    class CompactKeyboard;

    // ── Layout — proportions taken from the 1370x414 reference panel ─────────
    static constexpr float kColPresetsFrac = 350.0f / 1370.0f;   // ~0.2555
    static constexpr float kColVoiceFrac   = 477.0f / 1370.0f;   // ~0.3482
    // Column 3 (Performance & FX / Channel Mixer) gets the remainder (~0.396).
    static constexpr int   kNarrowThreshold = 760;   // px — below this, stack columns
    static constexpr int   kTopBarH         = 36;
    static constexpr int   kKnobW  = 64;
    static constexpr int   kKnobH  = 60;
    static constexpr int   kPad    = 8;

    void layoutWide   (juce::Rectangle<int> bounds);
    void layoutNarrow (juce::Rectangle<int> bounds);

    // ── Top bar ────────────────────────────────────────────────────────────
    juce::Rectangle<int> topBarZone, loadedPillZone;

    // ── Column 1 — preset browser (search + list, no grid) ────────────────
    Sf2ProgramGrid programGrid;   // data model retained; not painted directly
    std::vector<Sf2PresetInfo> presetList;
    juce::TextEditor searchBox;
    juce::ListBox    presetListBox;
    std::unique_ptr<juce::ListBoxModel> presetListModel;   // see .cpp: PresetListModel
    juce::TextButton browseButton { "BROWSE SF2" };
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::Rectangle<int> col1Zone, searchZone, listZone, bankFooterZone, browseButtonZone;
    juce::String currentSearchFilter;
    std::vector<int> filteredPresetIndices;   // indices into presetList, post-search-filter
    void restoreGridChannelAssignments();
    void rebuildFilteredPresetRows();
    void handlePresetLeftClicked  (int presetIdx);
    void handlePresetRightClicked (int presetIdx, juce::Point<int> screenPos);
    void handleChannelAssigned    (int presetIdx, int ch);

    /** SfzPlayer tracks two separate indices: getCurrentPresetIndex() (the
     *  actual engine-loaded preset) and getDisplayPresetIndex() (UI-only,
     *  set via setDisplayPresetIndex() whenever the user clicks a row here,
     *  defaults to -1). Using getCurrentPresetIndex() for UI highlighting
     *  was the bug behind "the first preset stays highlighted forever" —
     *  it only changes on a real engine load, not on browsing. This falls
     *  back to the real engine index only while nothing has been explicitly
     *  browsed yet (-1), so a freshly-loaded file still shows its actual
     *  active preset instead of "No preset selected". */
    int effectiveDisplayPresetIndex() const;

    struct AssignedPreset { juce::String name; int ch { 0 }; };
    std::vector<AssignedPreset> sf2Presets;

    // ── Column 2 top — active preset + 3 knobs ─────────────────────────────
    juce::Rectangle<int> col2Zone, activePresetHeaderZone;
    juce::Rectangle<int> levelZone, transZone, panZone;
    // Secondary fine-tune stepper — deliberate deviation from the literal
    // mockup (see header comment above); visually subordinate to the 3
    // primary knobs.
    juce::Rectangle<int> fineZone;

    // ── Column 2 bottom — filter + reverb controls (permanent, no tabs) ─────────
    //  Previously a switchable ENVELOPE/FILTER pair. Dropped the ENVELOPE
    //  side entirely: SfzPlayer's A/D/S/R is already shown live in two other
    //  places on screen at once — the top-bar LCD text readout AND the
    //  draggable envelope overlay on the waveform view (Sf2WaveformLcd) — so
    //  a third, read-only copy of the same four numbers here added nothing.
    //  Filter is the only thing that isn't already live elsewhere, so it now
    //  owns this space outright.
    juce::Rectangle<int> filterCutoffZone, filterResonanceZone;
    juce::Rectangle<int> reverbSendZone, reverbDampZone;

    // ── Column 3 — Performance & FX / Channel Mixer toggle ─────────────────
    enum class Col3Mode { PerformanceFx, ChannelMixer };
    Col3Mode col3Mode { Col3Mode::PerformanceFx };
    juce::Rectangle<int> col3Zone, col3TabZone;
    juce::Rectangle<int> perfFxTabZone, channelMixerTabZone;
    bool channelMixerTabEnabled() const noexcept { return countAssignedChannels() > 1; }

    juce::Rectangle<int> noteActivityLabelZone, noteMeterZone;
    juce::Rectangle<int> keyboardZone;

    std::unique_ptr<CompactKeyboard> compactKeyboard;
    Sf2ChannelFxPanel channelFxPanel;   // shown only in Col3Mode::ChannelMixer
    uint16_t assignedChannelMask { 0 }; ///< bit N set = MIDI channel N (0-based bit) has a preset
    int      countAssignedChannels() const noexcept;
    void     refreshChannelFxLabels();

    // Cached assignment range is retained for program-grid bookkeeping.
    int cachedChLow  { 0 };
    int cachedChHigh { 0 };

    void drawKnob      (juce::Graphics& g, juce::Rectangle<int> bounds,
                         float normalised, const juce::String& label,
                         const juce::String& valueStr) const;
    void drawSlider     (juce::Graphics& g, juce::Rectangle<int> bounds,
                         float normalised, const juce::String& label,
                         const juce::String& valueStr) const;
    void drawNoteMeter  (juce::Graphics& g, juce::Rectangle<int> bounds) const;

    // Decaying note-activity meter — derived from processor.sfzActiveNotes,
    // throttles its own repaint rate down when idle rather than repainting
    // the whole workspace at 30 Hz for no reason.
    float noteActivityLevel { 0.f };
    int   idleTicks         { 0 };

    enum class ActiveKnob { None, Level, Transpose, FineTune, Pan,
                             ReverbSend, ReverbDamp,
                             FilterCutoff, FilterResonance };
    ActiveKnob activeKnob   { ActiveKnob::None };
    int        dragStartY   { 0 };
    float      dragStartVal { 0.f };

    float volToNorm    (float linear) const;
    float normToVol    (float n)      const;
    float transToNorm  (int semi)     const;
    int   normToTrans  (float n)      const;
    float panToNorm    (float p)      const;
    float normToPan    (float n)      const;
    float fineToNorm   (float cents)  const;
    float normToFine   (float n)      const;

    // Filter cutoff uses a logarithmic UI mapping (appropriate for frequency);
    // resonance uses a linear mapping.
    float cutoffToNorm    (float hz)  const;
    float normToCutoff    (float n)   const;
    float resonanceToNorm (float pct) const;
    float normToResonance (float n)   const;

    void showMidiLearnMenu (int fieldId, juce::Point<int> screenPos);

    // ── Mouse events ──────────────────────────────────────────────────────────
    void mouseDown        (const juce::MouseEvent&) override;
    void mouseDrag        (const juce::MouseEvent&) override;
    void mouseUp          (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove   (const juce::MouseEvent&,
                           const juce::MouseWheelDetails&) override;

    DysektProcessor& processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sf2InstrumentWorkspace)
};
