#pragma once
// =============================================================================
//  MultisamplerEditor.h — Top-level multisampler panel
//  ─────────────────────────────────────────────────────────────────────────
//  Implementation-plan §6/§14 "first development slice": owns the native
//  MultisamplerInstrument, shows it via either a ZoneMapView (2D key/
//  velocity grid) or a KeysPanel-based table — see ViewMode — lets a single
//  selected zone's key/velocity/gain/pan be edited via a compact inline
//  inspector strip, and keeps sfzPlayer2 in sync via the plan's §5
//  "Playback synchronization" path — debounce edits, export the model to a
//  cache-directory SFZ on the UI thread, then hand that file to
//  SfzPlayer::loadFile() on the existing async fileLoadPool path (SfzPlayer
//  itself performs the safe swap at a block boundary; nothing here touches
//  the audio thread).
//
//  A dedicated ZoneInspector.h component is the natural next split once this
//  strip grows past a handful of fields (per the plan's §6 file list) — kept
//  inline here for now to keep the first slice small.
//
//  This component draws its own complete frame in both view modes and does
//  not rely on PluginEditor's paintOverChildren() bezel.
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "ZoneMapView.h"
#include "../AddZoneOverlay.h"
#include "../KeysPanel.h"
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

    /** Two presentations of the same MultisamplerInstrument — a 2D key/
        velocity map (zoneMapView) or a scannable table of zones
        (zoneTableView, a KeysPanel in SFZ-editable mode). This is what ZONES
        actually is now: not a separate editor with its own raw-SFZ scratch
        file, just this panel showing Table instead of Map. Both views read
        and write the same `instrument`, so switching between them mid-edit
        never loses anything and never needs its own save/discard prompt —
        only closing the panel entirely does (see PluginEditor::
        showMultisamplerPanel). */
    enum class ViewMode { Map, Table };
    void setViewMode (ViewMode mode);
    ViewMode getViewMode() const noexcept { return viewMode; }

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

    /** Rebuilds zoneTableView's rows from `instrument`, keeping
        tableRowZoneIds parallel to it (row i's zone id at index i) so table
        callbacks (row click/edit/right-click) can resolve back to a real
        zone. Duplicates toKeyzones()'s field mapping and disabled-zone skip
        rather than reusing it directly, since toKeyzones() is a static,
        id-free helper other callers (PluginEditor's read-only keyboard-
        highlight bridge) depend on keeping that exact signature. Called
        after every model mutation regardless of which view is currently
        visible (from refreshInspectorFromSelection(), see its trailing
        call), so the table is never stale when the user switches to it. */
    void refreshZoneTable();

    /** KeysPanel row click -> resolves to a zone id via tableRowZoneIds and
        selects it the same way clicking a zone in the map does (drives
        zoneMapView's own selection state, which stays the single source of
        truth for "what's selected" in both view modes — see
        selectZoneAtTableRow()'s .cpp comment for why). */
    void selectZoneAtTableRow (int row);

    /** KeysPanel field-edit (drag or type a cell) -> resolves to a zone via
        tableRowZoneIds and writes the edited fields straight onto that
        SampleZone, same set of fields applySliceControlBarFieldEdit()
        writes. */
    void applyTableRowEdit (int row, const KeysPanel::Keyzone& edited);

    DysektProcessor& processor;
    MultisamplerInstrument instrument;
    bool dirty = false;
    juce::File lastSavedFile;   // last file imported from or exported/saved to; File() if none yet

    ViewMode viewMode = ViewMode::Map;
    ZoneMapView zoneMapView;
    KeysPanel   zoneTableView { processor };   // ViewMode::Table — hidden until that mode is active
    std::vector<juce::Uuid> tableRowZoneIds;   // row i in zoneTableView <-> instrument.zones entry with this id

    // ── Header ───────────────────────────────────────────────────────────
    juce::Label  titleLabel;
    juce::TextButton addZoneButton { "ADD ZONE" };
    juce::TextButton importButton  { "IMPORT SFZ" };
    juce::TextButton exportButton  { "EXPORT SFZ" };
    juce::TextButton newButton     { "NEW" };
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<AddZoneOverlay>    zoneAddOverlay;   // modal popup; owned only while open

    // ── Selection status strip ──────────────────────────────────────────
    // Just a slim "N zones · (selection state)" readout now — actual
    // per-zone editing lives entirely in the shared SliceControlBar (see
    // onZoneSelectionOrEditChanged/applySliceControlBarFieldEdit above),
    // the exact same component ZONES edits through, rather than this panel
    // maintaining its own separate lo/hi/root/vel/gain/pan field row that
    // could drift out of sync with SCB's idea of the same zone.
    juce::Label inspectorTitle;
    juce::Uuid  inspectedZoneId = juce::Uuid::null();   // juce::Uuid::null() when nothing/multiple selected

    static constexpr int kEngineSyncDebounceMs = 300;
    static constexpr int kHeaderH    = 32;
    static constexpr int kInspectorH = 20;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultisamplerEditor)
};
