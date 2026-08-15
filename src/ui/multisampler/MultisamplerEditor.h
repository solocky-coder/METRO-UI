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
        tracks for the raw-SFZ zone builder, so an eventual "unsaved changes"
        prompt can treat both the same way. */
    bool isDirty() const noexcept { return dirty; }
    void clearDirtyFlag() noexcept { dirty = false; }

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
    void performEngineSync();        // export to cache SFZ + sfzPlayer2.loadFile()

    void importSfzClicked();
    void exportSfzClicked();
    void newInstrumentClicked();
    void addZoneClicked();   // pick a sample, then AddZoneOverlay for lo/hi/root — the
                              // native-model equivalent of PluginEditor's
                              // openZoneBuilderAddZone()/showZoneBuilderAddZoneOverlay()

    void refreshInspectorFromSelection();
    void applyInspectorFieldsToSelection();   // called on each field's onReturn/onFocusLost

    DysektProcessor& processor;
    MultisamplerInstrument instrument;
    bool dirty = false;

    ZoneMapView zoneMapView;

    // ── Header ───────────────────────────────────────────────────────────
    juce::Label  titleLabel;
    juce::TextButton addZoneButton { "ADD ZONE" };
    juce::TextButton importButton  { "IMPORT SFZ" };
    juce::TextButton exportButton  { "EXPORT SFZ" };
    juce::TextButton newButton     { "NEW" };
    std::unique_ptr<juce::FileChooser> fileChooser;
    std::unique_ptr<AddZoneOverlay>    zoneAddOverlay;   // modal popup; owned only while open

    // ── Compact inspector strip (single-zone editing) ──────────────────────
    // Shown only when exactly one zone is selected; hidden (and inert) for
    // zero or multi-selection, matching ZoneMapView's shift-click multi-select
    // — multi-field editing across a selection is a Phase-2 follow-up.
    juce::Label inspectorTitle;
    juce::Label lowKeyLabel, highKeyLabel, rootKeyLabel, lowVelLabel, highVelLabel, gainLabel, panLabel;
    juce::Label lowKeyField, highKeyField, rootKeyField, lowVelField, highVelField, gainField, panField;
    juce::Uuid  inspectedZoneId = juce::Uuid::null();   // juce::Uuid::null() when nothing/multiple selected

    static constexpr int kEngineSyncDebounceMs = 300;
    static constexpr int kHeaderH    = 32;
    static constexpr int kInspectorH = 30;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MultisamplerEditor)
};
