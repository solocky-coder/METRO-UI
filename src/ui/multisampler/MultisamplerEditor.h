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
//  This is the sole SFZ-PLAYER zone editor (the older raw-SFZ ZONES
//  workflow has been fully retired). Like that former component, this one
//  draws its own complete frame and does not rely on PluginEditor's
//  paintOverChildren() bezel.
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "ZoneMapView.h"
#include "MultisamplerZoneField.h"
#include "MultisamplerZoneLcd.h"
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

    /** The same keyboard-highlight data SfzPlayerDropdownPanel::
        parseSfzZones() derives from raw SFZ text, derived instead from a
        MultisamplerInstrument — so sfzPlayerDropdown.keysPanel can be
        repointed at whichever .sfz is currently loaded via MULTISAMPLER.
        Static/free of
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
        save/load — drives the SCB's SAVE button and any future
        unsaved-changes prompts around instrument-replacing actions (New,
        Import, browser/drop loads — see Phase 2 of the METRO-UI
        Multisampler Implementation Plan). */
    bool isDirty() const noexcept { return dirty; }
    void clearDirtyFlag() { dirty = false; saveButton.setVisible (false); repaint(); }

    /** SAVE half of the save/discard pair. Writes the
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

    /** Cancels any pending debounced performEngineSync() (see
        scheduleEngineSync()/timerCallback()) without firing it. Available
        for instrument-replacing operations (New/Import/discard) that need
        to stop a stale in-flight sync from landing after a model swap —
        see setInstrument()'s isFreshLoad path and discardPendingEdits().
        Safe to call even when no sync is pending (stopTimer() on an
        inactive Timer is a no-op). */
    void cancelPendingEngineSync() noexcept { stopTimer(); }

    /** -1 if zero or multiple zones are selected in the zone map; otherwise
        the index into getInstrument().zones for the single selected zone.
        MULTISAMPLER no longer drives an external readout off this (the
        dedicated zoneLcd member is refreshed internally instead — see
        refreshInspectorFromSelection()); kept public since PluginEditor
        still uses it for its own keyboard-highlight/dirty bookkeeping. */
    int getSelectedZoneIndex() const noexcept;

    /** Fired whenever the selected zone (per getSelectedZoneIndex()) or its
        data changes — a selection click, a live drag, a drag commit, or a
        zoneLcd field edit applied via applyZoneFieldEdit(). */
    std::function<void()> onZoneSelectionOrEditChanged;

    /** -1 if nothing is currently hovered in the zone map (cursor off the
        map, or over empty grid space); otherwise the index into
        getInstrument().zones for whichever zone the cursor is currently
        showing — see ZoneMapView::onZoneHovered's doc comment. This is a
        display-priority index, not a separate edit target: this class's own
        refreshZoneLcd() lets it drive zoneLcd's preview state while
        non-null, which is safe because zoneLcd only accepts edits when
        setEditable(true) is passed alongside — and that's only ever true
        for the actually-selected zone (see §4 of the implementation plan). */
    int getHoveredZoneIndex() const noexcept;

    /** Fired whenever the hovered zone (per getHoveredZoneIndex()) changes —
        cursor moved to a different zone, off the map, or the wheel was used
        to cycle a stacked overlap. */
    std::function<void()> onZoneHoverChanged;

    /** Applies one zoneLcd field edit to the zone identified by `zoneId` —
        NOT a vector index, so a hover transition, insertion, deletion, or
        reorder happening between the drag starting and this being called
        can never redirect the edit to the wrong zone (see implementation
        plan §4/§6). No-op if zoneId doesn't resolve to a zone still present
        in getInstrument().zones. Owns all 14 fields' clamping (matching the
        ranges the UI presents and import/export accept), dirty-state,
        zone-map/LCD refresh, and debounced engine sync — see the plan's §6
        table for the full field/clamp list. */
    void applyZoneFieldEdit (const juce::Uuid& zoneId, MultisamplerZoneField field,
                              float value, bool isCommit);

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
        MessageOverlay/ConfirmOverlay pattern elsewhere in the plugin. */
    std::function<void (const juce::String& sourceFileName,
                         bool importSucceeded,
                         const std::vector<SfzImporter::Warning>& warnings)> onImportWarnings;

    /** Fired when the NEW or IMPORT SFZ button is clicked (a file already
        chosen, in IMPORT's case) while isDirty() is true — about to discard
        unsaved edits. MultisamplerEditor has no overlay/dialog machinery of
        its own (see onImportWarnings' comment above) so, same as that
        callback, confirming is left to whoever owns the surrounding chrome.
        The receiver must call `proceed()` to actually go ahead with the
        replacement — synchronously, or later once an async ConfirmOverlay
        resolves — and must not call it more than once per firing; declining
        simply means never calling it. Only fires when isDirty() is true: a
        clean NEW/IMPORT proceeds immediately without going through this
        callback at all, so a disconnected handler only blocks the dirty
        case (see METRO-UI Multisampler Implementation Plan §5.6–5.7) rather
        than breaking the feature outright — same fallback shape as
        onImportWarnings being optional. Not used for browser/drag-drop
        loads, which PluginEditor already knows about directly and guards
        with its own ConfirmOverlay before ever calling importFromFile() —
        see loadSfzIntoMultisampler() in PluginEditor.cpp (plan §5.5/§5.8). */
    std::function<void (std::function<void()> proceed)> onConfirmDiscardIfDirty;

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

    // Bumped once per performEngineSync() call, UI-thread-only (written and
    // read only from the UI thread — the background export job that reads
    // it does so only after hopping back via MessageManager::callAsync, see
    // performEngineSync()'s definition). Lets a stale background export
    // that finishes after a newer one was already dispatched discard
    // itself instead of overwriting sfzPlayer2 with outdated data — same
    // defensive shape as PluginProcessor.h's nextPreviewToken2/
    // latestPreviewToken2 pair, scoped to this class (plan §5.12).
    int engineSyncGeneration = 0;

    void importSfzClicked();
    void exportSfzClicked();
    void newInstrumentClicked();
    void addZoneClicked();   // pick a sample, then AddZoneOverlay for lo/hi/root

    void refreshInspectorFromSelection();

    /** Pushes the current selection/hover state into zoneLcd — a
        setZoneForDisplay()/clearZone()/setMultipleSelection() +
        setEditable() call, per the resolution rules in implementation plan
        §4 (hover takes preview priority over selection for *display*, but
        editability always follows selection only). Called from
        refreshInspectorFromSelection() and from the hover-changed handler
        wired up in the constructor. */
    void refreshZoneLcd();

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
    // SAVE moved here from the shared SliceControlBar per implementation
    // plan §7/Phase 4 — MULTISAMPLER's save status and action are now
    // entirely local to this panel's own header. Visible only while dirty
    // (see resized()/paint() — mirrors the old SCB SAVE button's
    // isInstrumentDirty()-gated visibility).
    juce::TextButton saveButton    { "SAVE" };
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<AddZoneOverlay>    zoneAddOverlay;   // modal popup; owned only while open

    // ── Header zone-summary readout ─────────────────────────────────────
    // Compact one-line convenience duplicate of the same fields zoneLcd
    // shows in full — kept because it's visible right where zones are being
    // clicked/played without looking down at zoneLcd's row. Read-only; all
    // per-zone *editing* lives in zoneLcd now (see
    // onZoneSelectionOrEditChanged/applyZoneFieldEdit above and
    // refreshZoneLcd()). No longer has any relationship to SliceControlBar
    // — see implementation plan §8 step 7 for what happened to that class's
    // former zone-summary code.
    juce::Label headerZoneSummary;
    juce::Uuid  inspectedZoneId = juce::Uuid::null();   // juce::Uuid::null() when nothing/multiple selected
    juce::Uuid  hoveredZoneId   = juce::Uuid::null();   // display-only, see getHoveredZoneIndex()

    // ── Dedicated zone LCD ───────────────────────────────────────────────
    // Owns and displays all 14 MULTISAMPLER zone fields directly — see
    // implementation plan §3/§7. An independent juce::Component holding no
    // long-lived SampleZone pointer of its own (see MultisamplerZoneLcd.h).
    MultisamplerZoneLcd zoneLcd;

    static constexpr int kEngineSyncDebounceMs = 300;
    static constexpr int kHeaderH    = 32;
    static constexpr int kZoneLcdGap = 6;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultisamplerEditor)
};
