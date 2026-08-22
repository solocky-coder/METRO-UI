#include "MultisamplerEditor.h"
#include "../../PluginProcessor.h"
#include "../DysektLookAndFeel.h"
#include "../SliceControlBar.h"
#include "../../audio/multisampler/SfzImporter.h"
#include "../../audio/multisampler/SfzExporter.h"
#include "../../audio/SfzZoneColours.h"
#include <cmath>

namespace
{
    // juce::Label (like all juce::Component subclasses) has a deleted copy
    // constructor/assignment and no move operations, so it can't be built
    // and returned by value into an existing member. Configure in place
    // via a reference instead.
    void configureStaticLabel (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setJustificationType (juce::Justification::centredLeft);
        l.setInterceptsMouseClicks (false, false);
    }
}

MultisamplerEditor::MultisamplerEditor (DysektProcessor& processorToUse)
    : processor (processorToUse), zoneMapView (processorToUse)
{
    instrument.name = "New Instrument";

    addAndMakeVisible (zoneMapView);
    zoneMapView.setInstrument (&instrument);
    zoneMapView.onSelectionChanged = [this] { refreshInspectorFromSelection(); };
    zoneMapView.onZoneHovered = [this] (juce::Uuid hoveredId)
    {
        // Display only — mirrors refreshInspectorFromSelection()'s id
        // bookkeeping but never touches inspectedZoneId/selection state,
        // matching ZoneMapView::onZoneHovered's own contract.
        hoveredZoneId = hoveredId;
        refreshZoneLcdDisplay();   // hover takes priority in zoneLcd too
        if (onZoneHoverChanged) onZoneHoverChanged();
    };
    zoneMapView.onZoneEditing      = [this]
    {
        dirty = true;
        // Live-drag readout — matches ZONES' onZoneEdited pushing into SCB
        // on every drag frame, not just on commit, via the same
        // refreshInspectorFromSelection() -> onZoneSelectionOrEditChanged path.
        refreshInspectorFromSelection();
        repaint();
    };
    zoneMapView.onZoneEditCommitted = [this]
    {
        dirty = true;
        refreshInspectorFromSelection();
        scheduleEngineSync();
        if (onInstrumentChanged) onInstrumentChanged();
    };
    zoneMapView.onZoneDeleted = [this]
    {
        // Same downstream effects as a committed drag edit (dirty flag,
        // debounced resync, chrome notification) — a deletion is just
        // another committed model edit, it just didn't come from a drag.
        dirty = true;
        refreshInspectorFromSelection();
        scheduleEngineSync();
        if (onInstrumentChanged) onInstrumentChanged();
    };

    configureStaticLabel (titleLabel, "MULTISAMPLER");
    titleLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    addAndMakeVisible (titleLabel);

    addAndMakeVisible (addZoneButton);
    addZoneButton.onClick = [this] { addZoneClicked(); };

    addAndMakeVisible (importButton);
    importButton.onClick = [this] { importSfzClicked(); };

    addAndMakeVisible (exportButton);
    exportButton.onClick = [this] { exportSfzClicked(); };

    addAndMakeVisible (newButton);
    newButton.onClick = [this] { newInstrumentClicked(); };

    // ── Zone LCD ─────────────────────────────────────────────────────────
    addAndMakeVisible (zoneLcd);
    zoneLcd.onFieldEdited = [this] (MultisamplerZoneField field, float value, bool isCommit)
    {
        applyZoneFieldEdit (field, value, isCommit);
    };
    refreshZoneLcdDisplay();   // starts empty — nothing selected/hovered yet

    refreshInspectorFromSelection();   // starts disabled — nothing selected yet
}

MultisamplerEditor::~MultisamplerEditor()
{
    stopTimer();
}

// ── Layout / paint ───────────────────────────────────────────────────────

void MultisamplerEditor::paint (juce::Graphics& g)
{
    const auto& theme = getTheme();
    auto bounds = getLocalBounds().toFloat();

    g.setColour (theme.darkBar);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (theme.accent.withAlpha (0.5f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

    g.setColour (theme.separator);
    g.drawHorizontalLine (kHeaderH, 4.0f, bounds.getWidth() - 4.0f);

    if (dirty)
    {
        g.setColour (theme.accent);
        g.fillEllipse ((float) getWidth() - 14.0f, 10.0f, 6.0f, 6.0f);
    }
}

void MultisamplerEditor::resized()
{
    auto r = getLocalBounds().reduced (6);

    auto header = r.removeFromTop (kHeaderH - 6);
    titleLabel.setBounds (header.removeFromLeft (140));
    header.removeFromRight (4);
    newButton.setBounds    (header.removeFromRight (60));
    header.removeFromRight (4);
    exportButton.setBounds (header.removeFromRight (90));
    header.removeFromRight (4);
    importButton.setBounds (header.removeFromRight (90));
    header.removeFromRight (4);
    addZoneButton.setBounds (header.removeFromRight (84));

    r.removeFromTop (6);
    zoneLcd.setBounds (r.removeFromTop (MultisamplerZoneLcd::kPreferredHeight));

    r.removeFromTop (6);
    zoneMapView.setBounds (r);
}

// ── Instrument lifecycle ────────────────────────────────────────────────

std::vector<KeysPanel::Keyzone> MultisamplerEditor::toKeyzones (const MultisamplerInstrument& instrument)
{
    std::vector<KeysPanel::Keyzone> zones;
    zones.reserve (instrument.zones.size());

    int colIdx = 0;
    for (const auto& z : instrument.zones)
    {
        if (! z.enabled)
            continue;   // matches SfzExporter — a muted zone was never in the file ZONES would parse either

        KeysPanel::Keyzone kz;
        kz.loKey     = z.lowKey;
        kz.hiKey     = z.highKey;
        kz.loVel     = z.lowVelocity;
        kz.hiVel     = z.highVelocity;
        kz.rootPitch = z.rootKey;
        kz.isLooped  = (z.loopMode != LoopMode::noLoop);
        kz.isSfz     = true;   // editable, same convention as parseSfzZones()

        // A user-picked colour (see ZoneMapView::showZoneContextMenu) always
        // wins over the palette-index default — same preference rule
        // ZoneMapView::rebuildLayout() applies, so the keyboard highlight
        // never disagrees with the zone map about a manually recoloured
        // zone's colour. Otherwise: same palette/indexing as
        // parseSfzZones() so a file's zone colours read identically
        // regardless of which editor produced this list — see
        // toKeyzones()'s header comment.
        kz.colour = z.hasCustomColour ? juce::Colour (z.customColourArgb)
                                       : SfzZoneColours::zoneColour (colIdx);
        ++colIdx;

        kz.name = z.sampleFile != juce::File()
                      ? z.sampleFile.getFileNameWithoutExtension()
                      : ("Zone " + juce::String (colIdx));

        kz.volDb      = z.gainDb;
        kz.pan        = z.pan;
        kz.tuneCents  = z.tuneCents;
        kz.releaseSec = z.releaseSeconds;

        zones.push_back (kz);
    }

    return zones;
}

void MultisamplerEditor::setInstrument (MultisamplerInstrument newInstrument, bool syncEngine)
{
    // A wholesale swap must not let a still-pending debounced sync from
    // *this* (about-to-be-replaced) instrument's edits land afterward and
    // misread as an in-place edit against the new one — see
    // cancelPendingEngineSync()'s declaration comment, which already
    // documented this call site as the intended caller. (The background
    // export job dispatched by an already-fired performEngineSync() is a
    // separate race, handled instead by engineSyncGeneration — see that
    // method.)
    cancelPendingEngineSync();

    instrument = std::move (newInstrument);
    dirty = false;
    zoneMapView.setInstrument (&instrument);
    refreshInspectorFromSelection();
    if (syncEngine)
        performEngineSync (true);   // not debounced — a wholesale swap should reflect immediately
    if (onInstrumentChanged) onInstrumentChanged();
    repaint();
}

void MultisamplerEditor::newInstrumentClicked()
{
    auto doNew = [this]
    {
        setInstrument (MultisamplerInstrument{});
        instrument.name = "New Instrument";
    };

    // Plan §5.6: NEW is an instrument-replacing action and must not
    // silently discard unsaved edits. See onConfirmDiscardIfDirty's
    // declaration comment — a dirty instrument with no handler wired
    // blocks the action rather than discarding un-prompted.
    if (! dirty)
        doNew();
    else if (onConfirmDiscardIfDirty)
        onConfirmDiscardIfDirty (doNew);
}

void MultisamplerEditor::addZoneClicked()
{
    // Stage 1 of 3: pick a sample. beginAddZoneTrim() opens the trim step,
    // which hands off to beginAddZoneKeyMapping() (the original
    // AddZoneOverlay step), which hands off to commitAddedZone(). See the
    // three-stage split's declaration comment in MultisamplerEditor.h.
    fileChooser = std::make_unique<juce::FileChooser> ("Add sample as new zone…", juce::File(),
                                                         "*.wav;*.aif;*.aiff;*.flac;*.ogg");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto chosen = fc.getResult();
            if (! chosen.existsAsFile()) return; // cancelled

            beginAddZoneTrim (chosen);
        });
}

void MultisamplerEditor::beginAddZoneTrim (const juce::File& sampleFile)
{
    zoneTrimOverlay = std::make_unique<AddZoneTrimOverlay> (sampleFile, processor.fileLoadPool);
    zoneTrimOverlay->onResult = [this, sampleFile] (AddZoneTrimOverlay::Result result, bool confirmed)
    {
        // Same deferred-reset use-after-free fix the key-mapping step below
        // uses: let onResult unwind before the overlay (currently on the
        // call stack) is destroyed.
        juce::MessageManager::callAsync ([this]
        {
            if (zoneTrimOverlay)
            {
                removeChildComponent (zoneTrimOverlay.get());
                zoneTrimOverlay.reset();
            }
        });

        if (! confirmed) return;   // cancelled or decode failed — instrument untouched

        beginAddZoneKeyMapping (sampleFile, result.start, result.end, result.totalFrames);
    };

    addAndMakeVisible (*zoneTrimOverlay);
    zoneTrimOverlay->setBounds (getLocalBounds());
    zoneTrimOverlay->toFront (true);
}

void MultisamplerEditor::beginAddZoneKeyMapping (const juce::File& sampleFile,
                                                  int64_t trimStart, int64_t trimEnd, int64_t totalFrames)
{
    // Default: start just above the highest key currently mapped, so a
    // second Add Zone doesn't silently overlap the first.
    int prevHiKey = -1;
    for (const auto& z : instrument.zones)
        prevHiKey = juce::jmax (prevHiKey, z.highKey);
    const int defaultLo = (prevHiKey < 0) ? 0 : juce::jmin (prevHiKey + 1, 127);

    zoneAddOverlay = std::make_unique<AddZoneOverlay> (sampleFile.getFileNameWithoutExtension(), defaultLo);
    zoneAddOverlay->onResult = [this, sampleFile, trimStart, trimEnd, totalFrames]
                               (int lo, int hi, int root, bool confirmed)
    {
        // Defer the reset so it runs after onResult has returned and
        // AddZoneOverlay is no longer on the call stack (use-after-free
        // fix — same pattern PluginEditor's equivalent flow uses).
        juce::MessageManager::callAsync ([this]
        {
            if (zoneAddOverlay)
            {
                removeChildComponent (zoneAddOverlay.get());
                zoneAddOverlay.reset();
            }
        });

        if (! confirmed) return;   // cancelled — instrument untouched, even though trim was confirmed

        commitAddedZone (sampleFile, trimStart, trimEnd, totalFrames, lo, hi, root);
    };

    addAndMakeVisible (*zoneAddOverlay);
    zoneAddOverlay->setBounds (getLocalBounds());
    zoneAddOverlay->toFront (true);
}

void MultisamplerEditor::commitAddedZone (const juce::File& sampleFile,
                                           int64_t trimStart, int64_t trimEnd, int64_t totalFrames,
                                           int lo, int hi, int root)
{
    SampleZone zone;
    zone.sampleFile = sampleFile;
    zone.lowKey     = lo;
    zone.highKey    = hi;
    zone.rootKey    = root;

    // Preserve the model convention for an untrimmed selection (see
    // SampleZone.h: "-1 == full sample length") rather than writing the
    // file's current length as a literal sampleEnd — that would freeze a
    // "whole file" selection to whatever length the file happened to be
    // at Add Zone time, instead of tracking the file the way an untouched
    // zone always has.
    const bool spansWholeFile = trimStart == 0 && trimEnd >= totalFrames;
    zone.sampleStart = trimStart;
    zone.sampleEnd   = spansWholeFile ? -1 : trimEnd;

    const auto& added = instrument.addZone (std::move (zone));

    dirty = true;
    zoneMapView.setSelectedZoneIds ({ added.id });
    zoneMapView.refresh();
    refreshInspectorFromSelection();
    scheduleEngineSync();
    if (onInstrumentChanged) onInstrumentChanged();
    repaint();
}

// ── SFZ import / export ─────────────────────────────────────────────────

void MultisamplerEditor::importSfzClicked()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Import SFZ…", juce::File(), "*.sfz");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (! file.existsAsFile()) return;

            // Plan §5.7: IMPORT SFZ replaces the whole instrument the same
            // way NEW does — same dirty guard, see newInstrumentClicked()
            // and onConfirmDiscardIfDirty's declaration comment.
            auto doImport = [this, file] { importFromFile (file); };
            if (! dirty)
                doImport();
            else if (onConfirmDiscardIfDirty)
                onConfirmDiscardIfDirty (doImport);
        });
}

bool MultisamplerEditor::importFromFile (const juce::File& file, bool syncEngine)
{
    auto result = SfzImporter::importFile (file);
    if (! result.success)
    {
        if (onImportWarnings)
        {
            SfzImporter::Warning fatal;
            fatal.kind   = SfzImporter::Warning::Kind::malformedOpcode;
            fatal.detail = result.errorMessage.isNotEmpty()
                               ? result.errorMessage : "Import failed.";
            onImportWarnings (file.getFileName(), false, { fatal });
        }
        return false;
    }

    setInstrument (std::move (result.instrument), syncEngine);
    lastSavedFile = file;   // setInstrument() already cleared dirty — this is now the save target
    if (! result.warnings.empty() && onImportWarnings)
        onImportWarnings (file.getFileName(), true, result.warnings);
    return true;
}

void MultisamplerEditor::exportSfzClicked()
{
    fileChooser = std::make_unique<juce::FileChooser> ("Export SFZ…", juce::File(), "*.sfz");
    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File()) return;
            if (! file.hasFileExtension ("sfz")) file = file.withFileExtension ("sfz");

            SfzExporter::Options opts;
            opts.useRelativeSamplePaths = true;
            if (SfzExporter::exportToFile (instrument, file, opts))
            {
                // The file just picked is now the thing SAVE / discard-revert
                // operate against, same as an IMPORT SFZ would set it.
                lastSavedFile = file;
                clearDirtyFlag();

                // clearDirtyFlag() alone never reaches the SCB — its SAVE
                // button reads a *cached copy* of isDirty() that only gets
                // refreshed inside onInstrumentChanged (see PluginEditor's
                // sliceControlBar.setInstrumentDirty(...) callback). Without
                // firing it here, the SAVE button silently stays lit and
                // clickable after a successful export, with zero visible
                // sign that anything happened — indistinguishable from the
                // button doing nothing at all.
                if (onInstrumentChanged) onInstrumentChanged();
            }
        });
}

void MultisamplerEditor::saveInPlace()
{
    if (lastSavedFile == juce::File())
    {
        // Never imported from or saved to anything yet (e.g. built from
        // scratch via NEW) — there's nothing to silently overwrite, so
        // fall back to the same Save-As picker EXPORT SFZ uses.
        exportSfzClicked();
        return;
    }

    SfzExporter::Options opts;
    opts.useRelativeSamplePaths = true;
    if (SfzExporter::exportToFile (instrument, lastSavedFile, opts))
    {
        clearDirtyFlag();

        // Same reason as exportSfzClicked()'s success path: without this,
        // the SCB's cached instrumentDirty mirror never learns the save
        // happened, so its SAVE button stays lit/clickable with no visible
        // change — the save silently succeeds on disk but looks like the
        // button did nothing.
        if (onInstrumentChanged) onInstrumentChanged();
    }
}

void MultisamplerEditor::discardPendingEdits()
{
    if (lastSavedFile.existsAsFile())
    {
        // Reload from disk — this both undoes the in-memory edits and
        // resyncs the live engine back to what's actually on disk (it may
        // currently be pointed at a debounced preview of the discarded
        // edits, per performEngineSync()).
        importFromFile (lastSavedFile, true);
    }
    else
    {
        // Never saved anywhere — "discard" just means back to blank.
        setInstrument (MultisamplerInstrument{});
    }
    clearDirtyFlag();   // belt-and-braces: both paths above already do this
}

// ── Engine sync (plan §5 "Playback synchronization") ────────────────────

void MultisamplerEditor::scheduleEngineSync()
{
    // Restarting the timer on every call is the debounce: only the last
    // edit in a burst (e.g. a drag) actually fires performEngineSync().
    startTimer (kEngineSyncDebounceMs);
}

void MultisamplerEditor::timerCallback()
{
    stopTimer();
    performEngineSync (false);   // debounced timer only ever fires after an in-place edit
}

void MultisamplerEditor::performEngineSync (bool isFreshLoad)
{
    // Plan §5.12: SfzExporter::exportToFile() re-renders every zone to SFZ
    // text and writes it to disk, synchronously — for a large instrument
    // that's real work, and this used to all run on the message thread.
    // The render+write now happens on processor.fileLoadPool (the same
    // single-thread pool every other background file load in this plugin
    // already shares) instead. SfzExporter::render()/exportToFile() are
    // pure functions of their arguments, so a snapshot copy of `instrument`
    // taken here (still on the UI thread) is safe for the background job
    // to read — the live `instrument` member keeps changing underneath it
    // on the UI thread, but the copy doesn't.
    //
    // engineSyncGeneration (see its declaration) guards against a stale
    // background export finishing after a newer one was already
    // dispatched — e.g. two edits close enough together to both survive
    // the debounce, or a fresh setInstrument() landing while an older
    // edit's export is still in flight — and overwriting sfzPlayer2 with
    // outdated data. Only the *disk I/O* crosses threads; the counter
    // itself is written/read only on the UI thread (the background job's
    // completion touches it only after hopping back via
    // MessageManager::callAsync below).
    const int myGen = ++engineSyncGeneration;

    // Cache-directory file, not a user-visible path — same convention as
    // other generated-preview files elsewhere in the codebase. Fixed name is
    // fine: only one multisampler instrument is being edited at a time, and
    // SfzPlayer::loadFile()'s pending-load handoff makes each call replace
    // whatever the previous one queued.
    const auto cacheFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("DYSEKT")
                                .getChildFile ("multisampler_preview.sfz");
    cacheFile.getParentDirectory().createDirectory();

    SfzExporter::Options opts;   // absolute sample paths — this file's location is an implementation detail

    // SafePointer, not a raw `this` — the sfzPlayer2/loadSoundFontAsync
    // calls below only happen after a genuine thread hop (fileLoadPool ->
    // MessageManager::callAsync), and this panel can be destroyed in the
    // meantime (tab switch, plugin teardown). The background lambda itself
    // never touches `this`/`processor` — exportToFile() is static and
    // takes everything it needs by value through its own arguments — so
    // only the message-thread continuation needs the guard.
    juce::Component::SafePointer<MultisamplerEditor> safeThis (this);

    processor.fileLoadPool.addJob (
        [safeThis, myGen, cacheFile, opts, isFreshLoad, snapshot = instrument]
        {
            if (! SfzExporter::exportToFile (snapshot, cacheFile, opts))
                return;   // nothing to load — same as the old synchronous early-return

            juce::MessageManager::callAsync ([safeThis, myGen, cacheFile, isFreshLoad]
            {
                if (safeThis == nullptr)
                    return;   // panel destroyed while the export was in flight
                if (myGen != safeThis->engineSyncGeneration)
                    return;   // superseded by a newer edit/load before this one finished

                // UI thread only; SfzPlayer posts the load atomically and swaps at
                // the next audio block boundary (see SfzPlayer.h threading comment) —
                // no audio-thread file access happens here or inside loadFile() itself.
                safeThis->processor.sfzPlayer2.loadFile (cacheFile, safeThis->processor.fileLoadPool);

                // The line above only updates sfzPlayer2 — the live sfizz/FluidSynth
                // engine — but sfzPlayer2.process() is never actually called; what
                // voicePool2 plays back is the pre-rendered slice data in
                // sliceManager2/sampleData2 (and, in turn, whatever the SFZ-PLAYER's
                // Slice/waveform view is showing), populated only by SoundFontLoader
                // via loadSoundFontAsync(..., SoundFontLoadTarget::SfzPlayer2). Without
                // re-running that here, every MULTISAMPLER edit reaches playback but
                // never the Slice view or the samples voicePool2 actually renders —
                // same gap SfzPlayerDropdownPanel::writeSfzZoneChange's identical
                // comment describes for the ZONES editor, fixed there the same way.
                //
                // Only mark this a "zone edit" rebuild (preserve each slice's
                // DYSEKT-only fields — custom ADSR, per-slice EQ/filter, chromatic
                // channel, mute group, etc. — across the rebuild) when it genuinely is
                // one, i.e. the debounced-timer path following a real in-place edit.
                // setInstrument()'s immediate isFreshLoad=true call is a wholesale
                // model swap (import/New/discard) and must behave like any other fresh
                // file load: wipe every slice clean rather than carrying over
                // per-slice customisation that belonged to whatever was loaded before.
                // See zoneBuilderReloadPending's declaration in PluginProcessor.h and
                // writeSfzZoneChange's identical distinction for the ZONES editor.
                if (! isFreshLoad)
                    safeThis->processor.zoneBuilderReloadPending.store (true, std::memory_order_release);
                safeThis->processor.loadSoundFontAsync (cacheFile, SoundFontLoadTarget::SfzPlayer2);
            });
        });
}

// ── Inspector ────────────────────────────────────────────────────────────

int MultisamplerEditor::getSelectedZoneIndex() const noexcept
{
    if (inspectedZoneId == juce::Uuid::null())
        return -1;
    for (size_t i = 0; i < instrument.zones.size(); ++i)
        if (instrument.zones[i].id == inspectedZoneId)
            return (int) i;
    return -1;
}

int MultisamplerEditor::getHoveredZoneIndex() const noexcept
{
    if (hoveredZoneId == juce::Uuid::null())
        return -1;
    for (size_t i = 0; i < instrument.zones.size(); ++i)
        if (instrument.zones[i].id == hoveredZoneId)
            return (int) i;
    return -1;
}

int MultisamplerEditor::getSelectedZoneCount() const noexcept
{
    return (int) zoneMapView.getSelectedZoneIds().size();
}

void MultisamplerEditor::refreshInspectorFromSelection()
{
    const auto& selected = zoneMapView.getSelectedZoneIds();
    const bool single = selected.size() == 1;

    const SampleZone* zone = nullptr;
    if (single)
    {
        for (auto& z : instrument.zones)
            if (z.id == selected.front()) { zone = &z; break; }
    }

    inspectedZoneId = (zone != nullptr) ? zone->id : juce::Uuid::null();

    // zoneLcd is now the sole display AND the sole edit surface for this
    // data (see MultisamplerZoneLcd.h) — it renders its own "NO ZONE
    // SELECTED" / "MULTIPLE ZONES SELECTED" / full-field-readout states
    // directly from setZoneForDisplay()/clearZone()/setMultipleSelection(),
    // so there is no separate text to build here any more.
    refreshZoneLcdDisplay();

    // PluginEditor reads getSelectedZoneIndex()/getSelectedZoneCount()/
    // getInstrument() right after this fires to keep sliceLcd's and
    // multisamplerWaveformLcd's waveform displays in sync — see this
    // method's declaration comment and syncMultisamplerDisplay().
    if (onZoneSelectionOrEditChanged) onZoneSelectionOrEditChanged();
}

void MultisamplerEditor::refreshZoneLcdDisplay()
{
    const auto& selectedIds = zoneMapView.getSelectedZoneIds();

    // Hover takes priority over selection — same convention this used to
    // follow indirectly via PluginEditor::syncMultisamplerDisplay's
    // getHoveredZoneIndex()-before-getSelectedZoneIndex() fallback chain,
    // now resolved directly here since this panel already owns both
    // hoveredZoneId and the selection (zoneMapView.getSelectedZoneIds()).
    const bool isPreview = hoveredZoneId != juce::Uuid::null();
    juce::Uuid showId = hoveredZoneId;

    if (! isPreview)
    {
        if (selectedIds.size() == 1)
        {
            showId = selectedIds.front();
        }
        else if (selectedIds.size() >= 2)
        {
            // Multiple zones selected, nothing hovered — show the
            // "Multiple zones selected" state rather than freezing on
            // whatever single zone was selected right before (the root
            // cause of the old LCD/SCB desync: getSelectedZoneIndex()
            // collapsed "0 selected" and "2+ selected" to the same -1
            // sentinel, so a sticky-last-zone fallback couldn't tell them
            // apart). getSelectedZoneIds().size() can't make that mistake.
            zoneLcd.setMultipleSelection (true);
            zoneLcd.setEditable (false);
            return;
        }
        // else: selectedIds.empty() — showId stays juce::Uuid::null(),
        // falls through to the clearZone() branch below.
    }

    const SampleZone* zone = nullptr;
    int displayIndex = -1;
    if (showId != juce::Uuid::null())
    {
        for (size_t i = 0; i < instrument.zones.size(); ++i)
            if (instrument.zones[i].id == showId) { zone = &instrument.zones[i]; displayIndex = (int) i; break; }
    }

    if (zone == nullptr)
    {
        zoneLcd.clearZone();
        zoneLcd.setEditable (false);
        return;
    }

    zoneLcd.setZoneForDisplay (zone, displayIndex, isPreview, /*isAuditioning*/ false);
    // Editable only when the zone being shown IS the true single
    // selection — never a hover preview, never part of a multi-selection
    // (matches MultisamplerZoneLcd::setEditable's doc comment contract).
    zoneLcd.setEditable (! isPreview && selectedIds.size() == 1);
}

void MultisamplerEditor::applySliceControlBarFieldEdit (int zoneIndex, int field, float value)
{
    if (zoneIndex < 0 || zoneIndex >= (int) instrument.zones.size())
        return;
    auto& z = instrument.zones[(size_t) zoneIndex];

    switch (field)
    {
        case SliceControlBar::ZoneLoKey:   z.lowKey       = juce::jmin (z.highKey, juce::roundToInt (value)); break;
        case SliceControlBar::ZoneHiKey:   z.highKey      = juce::jmax (z.lowKey,  juce::roundToInt (value)); break;
        case SliceControlBar::ZoneRoot:    z.rootKey      = juce::roundToInt (value); break;
        case SliceControlBar::ZonePitch:   z.tuneCents    = value; break;
        case SliceControlBar::ZonePan:     z.pan          = value; break;
        case SliceControlBar::ZoneVolume:  z.gainDb       = value; break;
        case SliceControlBar::ZoneRelease: z.releaseSeconds = value; break;
        case SliceControlBar::ZoneLoop:
            z.loopMode = (value > 0.5f) ? LoopMode::loopContinuous : LoopMode::noLoop;

            // Root cause 2 fix (MULTISAMPLER): SfzExporter only writes
            // loop_start/loop_end when z.loopStart/z.loopEnd are already
            // set (see SfzExporter.cpp's appendRegion), so toggling LOOP on
            // with none set exported loop_mode=loop_continuous with no
            // points — unactionable by SoundFontLoader on reload. There's
            // no loop-point editor here for the user to set explicit
            // points, so auto-fill the whole sample the first time LOOP is
            // turned on. Never overwrites an already-set range.
            if (z.loopMode == LoopMode::loopContinuous && (z.loopStart < 0 || z.loopEnd < 0))
            {
                juce::AudioFormatManager fm;
                fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (z.sampleFile));
                if (reader != nullptr && reader->lengthInSamples > 1)
                {
                    // Native loopEnd is exclusive (one-past-the-last frame,
                    // matching sampleEnd's [start, end) convention — see
                    // SampleZone.h) — the whole-file loop point is therefore
                    // lengthInSamples, not lengthInSamples - 1.
                    z.loopStart = 0;
                    z.loopEnd   = reader->lengthInSamples;
                }
            }
            break;
        default: return;
    }

    dirty = true;
    zoneMapView.refresh();
    scheduleEngineSync();
    if (onInstrumentChanged) onInstrumentChanged();
    refreshInspectorFromSelection();   // pushes the just-applied value back into SCB's own readout
}

void MultisamplerEditor::applyZoneFieldEdit (MultisamplerZoneField field, float value, bool isCommit)
{
    // Resolved by id, not index — see this method's declaration comment.
    // The only zone this can ever be editing is the true single selection
    // (refreshZoneLcdDisplay() only ever calls zoneLcd.setEditable(true)
    // for that case), and inspectedZoneId tracks exactly that zone.
    if (inspectedZoneId == juce::Uuid::null())
        return;

    SampleZone* zonePtr = nullptr;
    for (auto& z : instrument.zones)
        if (z.id == inspectedZoneId) { zonePtr = &z; break; }
    if (zonePtr == nullptr)
        return;
    auto& z = *zonePtr;

    switch (field)
    {
        case MultisamplerZoneField::lowKey:
            z.lowKey  = juce::jmin (z.highKey, juce::jlimit (0, 127, juce::roundToInt (value)));
            break;
        case MultisamplerZoneField::highKey:
            z.highKey = juce::jmax (z.lowKey,  juce::jlimit (0, 127, juce::roundToInt (value)));
            break;
        case MultisamplerZoneField::rootKey:
            z.rootKey = juce::jlimit (0, 127, juce::roundToInt (value));
            break;
        case MultisamplerZoneField::group:
            z.group = juce::jmax (0, juce::roundToInt (value));
            break;
        case MultisamplerZoneField::tune:
            // Matches SampleZone::tuneCents' own documented range.
            z.tuneCents = juce::jlimit (-1200.0f, 1200.0f, value);
            break;
        case MultisamplerZoneField::pan:
            // Matches SampleZone::pan's own documented range.
            z.pan = juce::jlimit (-1.0f, 1.0f, value);
            break;
        case MultisamplerZoneField::gain:
            z.gainDb = value;
            break;
        case MultisamplerZoneField::attack:
            z.attackSeconds = juce::jmax (0.0f, value);
            break;
        case MultisamplerZoneField::decay:
            z.decaySeconds = juce::jmax (0.0f, value);
            break;
        case MultisamplerZoneField::sustain:
            // Matches SampleZone::sustainLevel's own documented 0..1 range.
            z.sustainLevel = juce::jlimit (0.0f, 1.0f, value);
            break;
        case MultisamplerZoneField::release:
            z.releaseSeconds = juce::jmax (0.0f, value);
            break;
        case MultisamplerZoneField::cutoff:
            z.filterCutoffHz = juce::jlimit (20.0f, 20000.0f, value);
            break;
        case MultisamplerZoneField::resonance:
            // Matches SampleZone::filterResonance's own documented 0..1 range.
            z.filterResonance = juce::jlimit (0.0f, 1.0f, value);
            break;
        case MultisamplerZoneField::loopEnabled:
            z.loopMode = (value > 0.5f) ? LoopMode::loopContinuous : LoopMode::noLoop;

            // Root cause 2 fix (MULTISAMPLER), same as
            // applySliceControlBarFieldEdit's identical branch above — see
            // that comment for the full explanation. Kept in both places
            // rather than factored out since the two callers resolve the
            // target zone through different keys (id here, index there).
            if (z.loopMode == LoopMode::loopContinuous && (z.loopStart < 0 || z.loopEnd < 0))
            {
                juce::AudioFormatManager fm;
                fm.registerBasicFormats();
                std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (z.sampleFile));
                if (reader != nullptr && reader->lengthInSamples > 1)
                {
                    z.loopStart = 0;
                    z.loopEnd   = reader->lengthInSamples;
                }
            }
            break;
    }

    // Live-drag frames (isCommit == false) update the model so the zone map
    // and LCD repaint with the live value, matching MultisamplerZoneLcd::
    // applyDrag's own doc comment ("the next setZoneForDisplay() call...
    // overwrites [the live snapshot] with the true value, so there's no
    // lasting drift") — but must NOT mark a dirty engine-sync cycle for
    // every intermediate frame. Same live-vs-commit split ZoneMapView's own
    // onZoneEditing/onZoneEditCommitted pair already uses.
    dirty = true;
    zoneMapView.refresh();
    if (isCommit)
    {
        scheduleEngineSync();
        if (onInstrumentChanged) onInstrumentChanged();
    }

    refreshZoneLcdDisplay();
    if (onZoneSelectionOrEditChanged) onZoneSelectionOrEditChanged();
}
