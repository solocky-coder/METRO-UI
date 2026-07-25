#pragma once
// =============================================================================
//  Sf2InstrumentWorkspace.h  —  persistent 3-column SF2 instrument workspace
// =============================================================================
//  Rebuilt replacement for SfzDropdownPanel's strip+popup UI. Instead of a
//  thin header strip that pops a program grid overlay on top of everything
//  else, this is a permanently-docked 3-column layout:
//
//    [ 26% Presets ] [ 36% Voice / Reverb / Keyboard ] [ 38% Channel mixer ]
//
//  Below ~760 px wide the columns stack top-to-bottom instead (narrow-width
//  stacking) so the workspace stays usable in a squeezed VST3 host window.
//
//  Column 1 — Preset browser: Sf2ProgramGrid, always visible (no more
//  open/close popup state). Left-click auditions, right-click assigns a
//  MIDI channel. This channel-assignment/collision/audition logic is ported
//  directly from SfzDropdownPanel's constructor (same rules: channels 1-2
//  reserved, chromatic-slice/SFZ-Player channels blocked).
//
//  Column 2 — Voice controls: TRN / FINE / PAN / VOL knobs and REV MIX /
//  REV SIZE knobs, wired straight to the real SfzPlayer setters (no
//  pushCommand/MIDI-learn indirection needed for the values themselves —
//  right-click still opens the MIDI-learn menu on the global SliceParamField
//  IDs, exactly as SfzDropdownPanel did). Below the knobs: a MIDI channel
//  range spinner (same lo/hi sfPlayerChannelMask logic as before), a decaying
//  note-activity meter driven by processor.sfzActiveNotes, and an embedded
//  KeysPanel (EngineSource::SfPlayer) for on-screen play + live note
//  highlighting.
//
//  Column 3 — Sf2ChannelFxPanel, shown once more than one MIDI channel has
//  an assigned preset (i.e. true multitimbral use); otherwise a single-
//  channel hint is drawn instead, since the per-channel mixer has nothing
//  useful to show for exactly one channel.
//
//  Public API mirrors SfzDropdownPanel so the PluginEditor swap is
//  mechanical: onFileChosen(), panelDidShow(), onFileLoaded,
//  onPresetChannelAssigned, notifyPresetChannelChanged(), getProgramGrid().
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "KeysPanel.h"
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

    /** Fired when the user right-clicks a preset cell and assigns a MIDI channel. */
    std::function<void (const Sf2PresetInfo&, int midiChannel1Based)> onPresetChannelAssigned;

    /** Called by PluginEditor whenever a preset<->channel mapping changes. */
    void notifyPresetChannelChanged (const juce::String& presetName, int midiCh1Based);

    /** Direct access to the SF2 program grid (read-only) for PluginEditor. */
    const Sf2ProgramGrid& getProgramGrid() const noexcept { return programGrid; }

private:
    // ── Layout ────────────────────────────────────────────────────────────────
    static constexpr float kColPresetsFrac = 0.26f;
    static constexpr float kColVoiceFrac   = 0.36f;
    // Column 3 (mixer) gets whatever remains (~0.38).
    static constexpr int   kNarrowThreshold = 760;   // px — below this, stack columns
    static constexpr int   kKnobW  = 56;
    static constexpr int   kKnobH  = 56;
    static constexpr int   kPad    = 8;

    void layoutWide   (juce::Rectangle<int> bounds);
    void layoutNarrow (juce::Rectangle<int> bounds);

    // ── Column 1 — preset browser ─────────────────────────────────────────────
    Sf2ProgramGrid programGrid;
    std::vector<Sf2PresetInfo> presetList;
    void restoreGridChannelAssignments();

    struct AssignedPreset { juce::String name; int ch { 0 }; };
    std::vector<AssignedPreset> sf2Presets;

    // ── Column 2 — voice controls ─────────────────────────────────────────────
    juce::Rectangle<int> transZone, fineZone, panZone, volZone, rvMixZone, rvSizeZone;
    juce::Rectangle<int> chLowDec,  chLowLabel,  chLowInc;
    juce::Rectangle<int> chHighDec, chHighLabel, chHighInc;
    juce::Rectangle<int> chRangeLabelZone;
    juce::Rectangle<int> noteMeterZone;
    juce::Rectangle<int> keyboardZone;

    KeysPanel keysPanel;

    int cachedChLow  { 0 };
    int cachedChHigh { 0 };

    void drawKnob (juce::Graphics& g, juce::Rectangle<int> bounds,
                   float normalised, const juce::String& label,
                   const juce::String& valueStr) const;
    void drawNoteMeter (juce::Graphics& g, juce::Rectangle<int> bounds) const;

    // Decaying note-activity meter — derived from processor.sfzActiveNotes,
    // throttles its own repaint rate down when idle rather than repainting
    // the whole workspace at 30 Hz for no reason.
    float noteActivityLevel { 0.f };
    int   idleTicks         { 0 };

    enum class ActiveKnob { None, Transpose, FineTune, Pan, Volume, ReverbMix, ReverbSize };
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

    void showMidiLearnMenu (int fieldId, juce::Point<int> screenPos);

    // ── Column 3 — per-channel mixer ──────────────────────────────────────────
    Sf2ChannelFxPanel channelFxPanel;
    juce::Rectangle<int> singleChannelHintZone;
    uint16_t assignedChannelMask { 0 };   ///< bit N set = MIDI channel N (0-based bit) has a preset
    int      countAssignedChannels() const noexcept;
    void     refreshChannelFxLabels();

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
