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
#include "MultisamplerZoneLcd.h"
#include "MultisamplerZoneField.h"
#include "../AddZoneOverlay.h"
#include "AddZoneTrimOverlay.h"
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

    /** Forwards the app's current host-window scale factor (DysektEditor::
        resized()'s `sf`) down to zoneLcd and zoneMapView, so their text
        keeps pace with the rest of the UI instead of staying pinned at
        design-time pixel sizes — see ZoneMapView::setUiScale() and
        MultisamplerZoneLcd::setUiScale() for why they needed this. Cheap
        to call every resized() — only repaints, no layout change. */
    void setUiScale (float newScale)
    {
        zoneMapView.setUiScale (newScale);
        zoneLcd.setUiScale (newScale);
    }

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
    void clearDirtyFlag() noexcept { dirty = false; }

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
        Lets PluginEditor drive sliceLcd's and multisamplerWaveformLcd's
        waveform displays, keyed off this index — see
        onZoneSelectionOrEditChanged. The zone LCD's own readout (zoneLcd)
        no longer goes through this index at all — see
        getSelectedZoneCount()'s doc comment for why. */
    int getSelectedZoneIndex() const noexcept;

    /** Number of zones currently selected in the zone map — 0, 1, or 2+.
        Exists specifically so callers with their own "nothing selected" vs.
        "multiple selected" fallback logic (PluginEditor::
        syncMultisamplerDisplay, for its sliceLcd/multisamplerWaveformLcd
        waveform displays) can tell those two states apart, since
        getSelectedZoneIndex() collapses both to -1 by design — this was
        the root cause of the old LCD/SCB desync during a multi-select. */
    int getSelectedZoneCount() const noexcept;

    /** Fired whenever the selected zone (per getSelectedZoneIndex()) or its
        data changes — a selection click, a live drag, a drag commit, or a
        zoneLcd field edit applied via applyZoneFieldEdit(). PluginEditor
        hooks this to keep sliceLcd's and multisamplerWaveformLcd's waveform
        displays in sync (via getSelectedZoneIndex()/getSelectedZoneCount()/
        getInstrument()); zoneLcd's own readout refreshes itself internally
        via refreshZoneLcdDisplay() and doesn't need this callback at all. */
    std::function<void()> onZoneSelectionOrEditChanged;

    /** -1 if the cursor isn't currently over a zone in the zone map (or has
        left the map entirely); otherwise the index into getInstrument().zones
        for the zone currently shown for read-only hover/inspection purposes.
        Mirrors getSelectedZoneIndex()'s id-to-index resolution, but sourced
        from ZoneMapView::onZoneHovered rather than the click-selection —
        see onZoneHoverChanged. */
    int getHoveredZoneIndex() const noexcept;

    /** Fired whenever getHoveredZoneIndex() changes — either a new zone is
        hovered, the cursor leaves the map (back to -1), or the wheel is used
        to cycle a stack under a stationary cursor. Read-only preview: takes
        priority over the selection-driven display while non-null, matching
        ZoneMapView::onZoneHovered's own contract. PluginEditor hooks this to
        re-run syncMultisamplerDisplay(). */
    std::function<void()> onZoneHoverChanged;

    /** Applies one SliceControlBar field edit (see SliceControlBar::
        SfzZoneField) to the zone at `zoneIndex` (as returned by
        getSelectedZoneIndex()). The SCB itself no longer has any
        MULTISAMPLER-facing role (zoneLcd replaced it — see
        applyZoneFieldEdit() below), but multisamplerWaveformLcd's envelope-
        node dragging still speaks this same SliceControlBar::SfzZoneField
        vocabulary (Attack/Decay/Sustain/Release only) and still forwards
        here unconditionally — see its onZoneParamEdited wiring in
        PluginEditor.cpp. No-op if zoneIndex is out of range. */
    void applySliceControlBarFieldEdit (int zoneIndex, int field, float value);

    /** Applies one zoneLcd field edit (see MultisamplerZoneField) to the
        currently-editable zone (resolved by inspectedZoneId, not index —
        see MultisamplerZoneLcd.h's onFieldEdited doc comment for why: the
        LCD reports edits by field identity, not a vector index, so this is
        the one place both the id resolution and the value clamping happen).
        `isCommit` is false on every intermediate drag frame and true
        exactly once at gesture end; only a commit marks dirty, schedules
        the debounced engine sync, and fires onInstrumentChanged — matching
        ZoneMapView's own onZoneEditing (live) vs. onZoneEditCommitted
        (commit) split. No-op if nothing is currently selected/editable. */
    void applyZoneFieldEdit (MultisamplerZoneField field, float value, bool isCommit);

    /** Auto-assigns output buses 1-15 (round-robin, top-to-bottom zones[]
        order) to the first `numZones` zones — used by the drum-kit
        auto-routing confirm prompt (PluginEditor::offerDrumKitAutoRouting)
        right after import. Writes SampleZone::outputBus directly so the
        assignment round-trips through save/reload and performEngineSync(),
        unlike the old SfzDrumKitBusApplier path it replaces (see .cpp for
        the full history). No-op if numZones <= 0. */
    void autoAssignOutputBuses (int numZones);

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
    void addZoneClicked();   // pick a sample, then trim, then AddZoneOverlay for lo/hi/root

    // ── Add Zone stages ──────────────────────────────────────────────────
    // Split out of what used to be addZoneClicked()'s file-picker callback
    // body so the new trim step sits cleanly between picking the file and
    // collecting the key range — see the METRO-UI Multisampler Add Zone
    // Trim implementation notes' "Refactor MultisamplerEditor::
    // addZoneClicked()" section. Only commitAddedZone() actually touches
    // `instrument`; the first two stages just juggle overlays and pass
    // trim/key data forward, so cancelling either one leaves the
    // instrument untouched.
    void beginAddZoneTrim (const juce::File& sampleFile);
    void beginAddZoneKeyMapping (const juce::File& sampleFile,
                                  int64_t trimStart, int64_t trimEnd, int64_t totalFrames);
    void commitAddedZone (const juce::File& sampleFile,
                           int64_t trimStart, int64_t trimEnd, int64_t totalFrames,
                           int lo, int hi, int root);

    void refreshInspectorFromSelection();

    /** Resolves what zoneLcd should currently show — hover preview, the
        single selection, the auto-elected top layer of a multi-selection
        (see ZoneMapView::topmostZoneAmong()), or empty — directly from
        zoneMapView.getSelectedZoneIds() and hoveredZoneId. This is the
        replacement for the old headerZoneSummary text-building logic, and
        the reason zoneLcd never has the -1-ambiguity bug the SCB fallback
        used to have: getSelectedZoneIds().size() can distinguish "0
        selected" from "2+ selected" directly, with no sentinel collapsing
        them together. Called from refreshInspectorFromSelection() (selection
        changed), the onZoneHovered lambda (hover changed), and
        applyZoneFieldEdit() (the displayed zone's own data changed). */
    void refreshZoneLcdDisplay();

    /** Rebuilds editLayerCombo's items from whatever key/velocity-overlaps
        inspectedZoneId right now (MultisamplerInstrument::
        findOverlappingPairs(), the same pairwise overlap definition the
        zone-map's overlap hatching and the old right-click "Edit Layer"
        submenu use), and re-selects the entry matching inspectedZoneId.
        Disables the combo when nothing is selected or the selected zone
        doesn't overlap anything. Called from refreshInspectorFromSelection()
        so it always tracks the current selection, the same way zoneLcd does. */
    void refreshEditLayerCombo();

    DysektProcessor& processor;
    MultisamplerInstrument instrument;
    bool dirty = false;
    juce::File lastSavedFile;   // last file imported from or exported/saved to; File() if none yet

    ZoneMapView zoneMapView;

    // ── Header ───────────────────────────────────────────────────────────
    juce::Label  titleLabel;

    // Toolbar promotion of the right-click zone-map "Edit Layer" submenu
    // (see ZoneMapView::showZoneContextMenu) — lets the user reach a buried
    // layer without needing to right-click the exact stack of tiles first.
    // Scoped to whichever zone is currently selected/inspected rather than a
    // click point (see refreshEditLayerCombo()'s doc comment); disabled
    // when the current selection has nothing overlapping it. Placed before
    // (i.e. to the left of, in this right-to-left header layout) addZoneButton.
    juce::ComboBox editLayerCombo;
    std::vector<juce::Uuid> editLayerStackIds;   // combo item id N -> editLayerStackIds[N-1]

    juce::TextButton addZoneButton { "ADD ZONE" };
    juce::TextButton importButton  { "IMPORT SFZ" };
    juce::TextButton exportButton  { "EXPORT SFZ" };
    juce::TextButton newButton     { "NEW" };
    std::unique_ptr<juce::FileChooser>      fileChooser;
    std::unique_ptr<AddZoneTrimOverlay>     zoneTrimOverlay;   // Add Zone step 1; owned only while open
    std::unique_ptr<AddZoneOverlay>         zoneAddOverlay;    // Add Zone step 2; owned only while open

    // ── Zone LCD ─────────────────────────────────────────────────────────
    // Replaces the old route where MULTISAMPLER zone data was mirrored
    // read-only into the shared SliceControlBar (SCB) and edited through
    // SliceControlBar::SfzZoneField. zoneLcd is both the display AND the
    // edit surface for the selected zone's fields — live and editable in
    // its own strip above the zone map — so the SCB has no MULTISAMPLER-
    // facing role left at all (see METRO-UI Multisampler Implementation
    // Plan §8 step 7). See MultisamplerZoneLcd.h for why it never retains
    // a raw SampleZone pointer, and applyZoneFieldEdit()/
    // refreshZoneLcdDisplay() above for how edits and display resolution
    // are handled on this side.
    MultisamplerZoneLcd zoneLcd;
    juce::Uuid  inspectedZoneId = juce::Uuid::null();   // juce::Uuid::null() when nothing selected; otherwise the single
                                                         // selected zone, or the auto-elected top layer of a multi-selection
    juce::Uuid  hoveredZoneId = juce::Uuid::null();     // mirrors ZoneMapView::hoverZoneId; juce::Uuid::null() when the cursor isn't over a zone

    static constexpr int kEngineSyncDebounceMs = 300;
    static constexpr int kHeaderH    = 32;

    // ── Add Zone trim audition (plan §5-style preview, scoped to the trim
    // step only) ────────────────────────────────────────────────────────
    // While AddZoneTrimOverlay (beginAddZoneTrim()) is open, the user can
    // play notes on any channel routed to sfzPlayer2 to audition the
    // in-progress [start, end) trim region before a zone is ever added to
    // `instrument`. This reuses the exact export -> loadFile() ->
    // loadSoundFontAsync() pipeline performEngineSync() uses for real
    // edits, but renders a throwaway single-region MultisamplerInstrument
    // (spanning the full key/velocity range, root C3) instead of the real
    // `instrument`, and writes it to a separate cache file so it can never
    // collide with — or be overwritten by — a real edit's debounced sync
    // landing at the same time.
    //
    // Trade-off, by design: sfzPlayer2 can only have one file loaded at
    // once, so while this preview is active the real `instrument`'s
    // committed zones are NOT audible — the user is inside a modal
    // "add zone" overlay at this point anyway, not looking at the zone map.
    // restoreEngineToRealInstrument() puts the real instrument back the
    // moment the trim step closes (confirmed or cancelled), via the
    // existing performEngineSync() path, so nothing needs to change there.
    //
    // Needs its own timer rather than reusing the engineSyncGeneration/
    // Timer pair above: that timer's timerCallback() is hardwired to
    // performEngineSync(), and a trim-handle drag happening concurrently
    // with an unrelated real edit must not have either debounce starve or
    // pre-empt the other.
    class TrimPreviewTimer : public juce::Timer
    {
    public:
        explicit TrimPreviewTimer (std::function<void()> cb) : onTick (std::move (cb)) {}
        void timerCallback() override { stopTimer(); if (onTick) onTick(); }
    private:
        std::function<void()> onTick;
    };
    std::unique_ptr<TrimPreviewTimer> trimPreviewTimer;

    // Latest pending trim-preview request; read only by performTrimPreviewSync()
    // after the debounce fires, both on the UI thread — mirrors the
    // snapshot-by-value pattern performEngineSync() uses for `instrument`.
    juce::File addZoneTrimPreviewFile;
    int64_t    addZoneTrimPreviewStart = 0;
    int64_t    addZoneTrimPreviewEnd   = 0;

    // Same stale-completion guard as engineSyncGeneration, scoped separately
    // so a real edit's sync and a trim-preview sync can never invalidate
    // each other.
    int trimPreviewGeneration = 0;

    /** Debounces AddZoneTrimOverlay::onTrimChanged into performTrimPreviewSync(). */
    void scheduleTrimPreviewSync (const juce::File& sampleFile, int64_t start, int64_t end);
    void performTrimPreviewSync();
    /** Points sfzPlayer2 back at the real `instrument` — call once when the
     *  trim step closes, however it closes. Safe to call even if no trim
     *  preview was ever scheduled (e.g. the user cancelled before decode
     *  finished). */
    void restoreEngineToRealInstrument();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultisamplerEditor)
};
