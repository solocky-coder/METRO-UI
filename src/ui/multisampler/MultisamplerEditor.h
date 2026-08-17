#pragma once
// =============================================================================
//  MultisamplerEditor.h — Top-level multisampler panel
//  ─────────────────────────────────────────────────────────────────────────
//  Implementation-plan §6/§14 "first development slice": owns the native
//  MultisamplerInstrument, shows it on a ZoneMapView, lets a single selected
//  zone's key/velocity/gain/pan be edited via a compact inline inspector
//  strip, and keeps sfzPlayer2 in sync via the plan's §5 "Playback
//  synchronization" path — debounce edits, export the model to a cache-
//  directory SFZ on the UI thread, then hand that file to SfzPlayer::loadFile()
//  on the existing async fileLoadPool path (SfzPlayer itself performs the
//  safe swap at a block boundary; nothing here touches the audio thread).
//
//  A dedicated ZoneInspector.h component is the natural next split once this
//  strip grows past a handful of fields (per the plan's §6 file list) — kept
//  inline here for now to keep the first slice small.
//
//  Like zoneBuilderKeysPanel (the existing raw-SFZ zone builder this panel
//  is the native-model successor to — see PluginEditor.cpp), this component
//  draws its own complete frame and does not rely on PluginEditor's
//  paintOverChildren() bezel.
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "ZoneMapView.h"
#include "../AddZoneOverlay.h"
#include "../KeysPanel.h"
#include "../UIHelpers.h"
#include "../../audio/multisampler/MultisamplerInstrument.h"
#include "../../audio/multisampler/SfzImporter.h"

class DysektProcessor;

class MultisamplerEditor : public juce::Component,
                            private juce::Timer
{
public:
    explicit MultisamplerEditor (DysektProcessor& processorToUse);
    ~MultisamplerEditor() override;

    void paint   (juce::Graphics&) override;
    void resized() override;

    /** Replaces the instrument outright (e.g. loading a .metrokit — Phase 4).
        Triggers an immediate (non-debounced) engine resync by default. Pass
        syncEngine=false when the caller already knows the live engine is
        correctly pointed at the source (e.g. a background sync from a load
        path that already called sfzPlayer2.loadFile() on the original file
        directly) — resyncing in that case would just reload a redundant,
        lossily round-tripped copy for no benefit. */
    void setInstrument (MultisamplerInstrument newInstrument, bool syncEngine = true);

    const MultisamplerInstrument& getInstrument() const noexcept { return instrument; }

    /** Consolidation-plan step 3: the same keyboard-highlight data ZONES
        gets from SfzPlayerDropdownPanel::parseSfzZones(), derived instead
        from a MultisamplerInstrument — so sfzPlayerDropdown.keysPanel can
        be repointed at whichever .sfz is currently loaded via MULTISAMPLER
        without depending on ZONES' scratch-file text at all. Static/free of
        `this` so PluginEditor can call it on any instrument snapshot it
        already has (e.g. getInstrument()) without needing a live
        MultisamplerEditor edit in flight. Colour indexing matches
        parseSfzZones()/SfzZoneColours exactly (same palette, same
        top-to-bottom zones[] order) so a file's zone colours don't change
        depending on which editor most recently produced them. Disabled
        (muted) zones are skipped, matching what SfzExporter actually
        writes to the engine — a disabled zone was never in the exported
        SFZ ZONES would have parsed either. */
    static std::vector<KeysPanel::Keyzone> toKeyzones (const MultisamplerInstrument& instrument);

    /** Imports an .sfz directly, no file picker — the shared body behind
        importSfzClicked() (after the user picks a file) and also callable by
        whoever owns the app's other .sfz load paths (browser, drag-and-drop),
        so this panel's model stays in sync with whatever .sfz is loaded
        elsewhere instead of only ever changing via its own IMPORT SFZ button.
        Fires onImportWarnings on failure or partial success, same contract as
        clicking IMPORT SFZ; silent on a clean import. Returns true on success
        (even with warnings). Pass syncEngine=false when the caller already
        pointed the live engine at `file` directly — see setInstrument(). */
    bool importFromFile (const juce::File& file, bool syncEngine = true);

    /** True once at least one committed edit has happened since the last
        save/load — mirrors the zoneBuilderDirty flag PluginEditor already
        tracks for the raw-SFZ zone builder, so an "unsaved changes" prompt
        can treat both the same way (see toggleMultisamplerEditor). */
    bool isDirty() const noexcept { return dirty; }
    void clearDirtyFlag() noexcept { dirty = false; }

    /** SAVE half of the zoneBuilder-style save/discard pair. Writes the
        current instrument back to whichever file it was last imported from
        or exported to (silent overwrite, no picker) and clears the dirty
        flag on success. If this instrument has never been pointed at a
        file (e.g. built from scratch via NEW), falls back to the same
        Save-As file picker as clicking EXPORT SFZ — there's nothing to
        overwrite yet. */
    void saveInPlace();

    /** DISCARD half of the save/discard pair. Throws away edits since the
        last import/save by reloading the instrument from whichever file
        saveInPlace()/importFromFile() last pointed it at, or resetting to
        a blank instrument if there is no such file. Always leaves the
        dirty flag clear. */
    void discardPendingEdits();

    /** The file saveInPlace() would overwrite, or an invalid File() if
        this instrument has never been imported from / saved to one. Lets
        PluginEditor phrase its unsaved-changes prompt accurately (e.g.
        offering "Save" only when there's somewhere to save to). */
    const juce::File& getLastSavedFile() const noexcept { return lastSavedFile; }

    /** -1 if zero or multiple zones are selected in the zone map; otherwise
        the index into getInstrument().zones for the single selected zone.
        Lets PluginEditor drive the shared SliceControlBar (SCB) readout the
        exact same way it already does for ZONES
        (zoneBuilderKeysPanel::onRowClicked -> sliceControlBar.
        setSfzZoneSummary), keyed off this index instead of ZONES' row
        index — see onZoneSelectionOrEditChanged. */
    int getSelectedZoneIndex() const noexcept;

    /** Fired whenever the selected zone (per getSelectedZoneIndex()) or its
        data changes — a selection click, a live drag, a drag commit, or a
        SliceControlBar field edit applied via applySliceControlBarFieldEdit().
        This is MULTISAMPLER's exact counterpart of ZONES'
        zoneBuilderKeysPanel::onRowClicked/onZoneEdited pair; PluginEditor
        hooks this the same way to push sliceControlBar.setSfzZoneSummary()/
        clearSfzZoneSummary(), so the SCB behaves identically regardless of
        which editor is open. */
    std::function<void()> onZoneSelectionOrEditChanged;

    /** Applies one SliceControlBar field edit (see SliceControlBar::
        SfzZoneField) to the zone at `zoneIndex` (as returned by
        getSelectedZoneIndex()). PluginEditor's existing
        sliceControlBar.onSfzZoneParamEdited handler forwards here instead
        of its ZONES-matrix logic whenever MULTISAMPLER, not ZONES, is the
        active editor — same trigger, same field enum, different model
        underneath. No-op if zoneIndex is out of range. */
    void applySliceControlBarFieldEdit (int zoneIndex, int field, float value);

    /** Fired after every committed model edit (drag-commit, inspector apply,
        import, New) — after the debounced resync has been scheduled, not
        after it completes. Lets PluginEditor update window chrome / the
        SAVE-prompt-on-close logic without polling. */
    std::function<void()> onInstrumentChanged;

    /** Fired once after an importSfzClicked() attempt IF there's anything
        worth telling the user about: either a hard failure (importSucceeded
        == false, warnings holds a single synthetic entry carrying
        Result::errorMessage) or a successful import that still produced
        SfzImporter::Result::warnings (unsupported opcodes, missing samples,
        malformed lines, etc.). Not fired on a clean, warning-free import.
        `sourceFileName` is the imported SFZ's display name, for use in
        whatever dialog title the owner shows. MultisamplerEditor has no
        overlay/dialog machinery of its own (see the plan's §3 "let the UI
        edit that model" — this panel only owns the model and its own inline
        controls), so surfacing these to the user is left to whoever owns the
        surrounding chrome, same as PluginEditor's existing
        MessageOverlay/ConfirmOverlay pattern for zoneBuilder. */
    std::function<void (const juce::String& sourceFileName,
                         bool importSucceeded,
                         const std::vector<SfzImporter::Warning>& warnings)> onImportWarnings;

private:
    void timerCallback() override;   // fires once, kEngineSyncDebounceMs after the last edit

    void scheduleEngineSync();       // (re)start the debounce timer
    // isFreshLoad: true when called right after setInstrument() wholesale-swapped
    // the model (import/New/discard) — sliceManager2 should wipe every slice
    // clean, same as any other fresh file load. false (the debounced-timer
    // path) means this is a genuine in-place zone edit and per-slice
    // DYSEKT-only fields (custom ADSR/EQ/filter/mute group/etc.) should
    // survive the rebuild instead — see zoneBuilderReloadPending's
    // declaration in PluginProcessor.h and performEngineSync()'s own comment.
    void performEngineSync (bool isFreshLoad);

    void importSfzClicked();
    void exportSfzClicked();
    void newInstrumentClicked();
    void addZoneClicked();   // pick a sample, then AddZoneOverlay for lo/hi/root — the
                              // native-model equivalent of PluginEditor's
                              // openZoneBuilderAddZone()/showZoneBuilderAddZoneOverlay()

    void refreshInspectorFromSelection();

    DysektProcessor& processor;
    MultisamplerInstrument instrument;
    bool dirty = false;
    juce::File lastSavedFile;   // last file imported from or exported/saved to; File() if none yet

    ZoneMapView zoneMapView;

    // ── Header ───────────────────────────────────────────────────────────
    juce::Label  titleLabel;
    juce::TextButton addZoneButton { "ADD ZONE" };
    juce::TextButton importButton  { "IMPORT SFZ" };
    juce::TextButton exportButton  { "EXPORT SFZ" };
    juce::TextButton newButton     { "NEW" };
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<AddZoneOverlay>    zoneAddOverlay;   // modal popup; owned only while open

    // ── Header zone-summary readout ─────────────────────────────────────
    // Same loKey/hiKey/root/pitch/pan/volume/release/loop info the shared
    // SliceControlBar already shows for the selected zone (see
    // SliceControlBar::drawSfzZoneSummary), mirrored here inline in this
    // panel's own header so it's visible right where zones are being
    // clicked/played, without needing to look down at the SCB row at the
    // bottom of the whole plugin window. Replaces the old bottom "selection
    // status" strip (a single-line filename/placeholder readout) that used
    // to sit between the zone map and the piano-key ruler — that strip is
    // gone, and ZoneMapView's own keyboard strip (kKeyboardStripPx) grew to
    // absorb the vertical space it freed up instead. Read-only; per-zone
    // *editing* still lives entirely in the SCB (see
    // onZoneSelectionOrEditChanged/applySliceControlBarFieldEdit above),
    // this is purely a convenience duplicate of the same data.
    juce::Label headerZoneSummary;
    juce::Uuid  inspectedZoneId = juce::Uuid::null();   // juce::Uuid::null() when nothing/multiple selected

    static constexpr int kEngineSyncDebounceMs = 300;
    static constexpr int kHeaderH    = 32;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultisamplerEditor)
};
