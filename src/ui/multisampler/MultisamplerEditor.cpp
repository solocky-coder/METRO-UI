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

    // ── Header zone-summary readout ─────────────────────────────────────
    configureStaticLabel (headerZoneSummary, "Select a zone to edit");
    headerZoneSummary.setFont (juce::FontOptions (11.5f));
    headerZoneSummary.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (headerZoneSummary);

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
    header.removeFromLeft (10);   // gap after titleLabel
    headerZoneSummary.setBounds (header);   // whatever's left between title and buttons

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
    // Pick a sample first, then confirm the key range in AddZoneOverlay
    // before committing the zone.
    fileChooser = std::make_unique<juce::FileChooser> ("Add sample as new zone…", juce::File(),
                                                         "*.wav;*.aif;*.aiff;*.flac;*.ogg");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto chosen = fc.getResult();
            if (! chosen.existsAsFile()) return; // cancelled

            // Default: start just above the highest key currently mapped, so a
            // second Add Zone doesn't silently overlap the first.
            int prevHiKey = -1;
            for (const auto& z : instrument.zones)
                prevHiKey = juce::jmax (prevHiKey, z.highKey);
            const int defaultLo = (prevHiKey < 0) ? 0 : juce::jmin (prevHiKey + 1, 127);

            zoneAddOverlay = std::make_unique<AddZoneOverlay> (chosen.getFileNameWithoutExtension(), defaultLo);
            zoneAddOverlay->onResult = [this, chosen] (int lo, int hi, int root, bool confirmed)
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

                if (! confirmed) return;

                SampleZone zone;
                zone.sampleFile = chosen;
                zone.lowKey     = lo;
                zone.highKey    = hi;
                zone.rootKey    = root;
                const auto& added = instrument.addZone (std::move (zone));

                dirty = true;
                zoneMapView.setSelectedZoneIds ({ added.id });
                zoneMapView.refresh();
                refreshInspectorFromSelection();
                scheduleEngineSync();
                if (onInstrumentChanged) onInstrumentChanged();
                repaint();
            };

            addAndMakeVisible (*zoneAddOverlay);
            zoneAddOverlay->setBounds (getLocalBounds());
            zoneAddOverlay->toFront (true);
        });
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

    if (zone == nullptr)
    {
        // Same wording as SliceControlBar::drawSfzZoneSummary's own empty
        // state, so this header readout and the SCB's own row agree
        // exactly when nothing (or more than one zone) is selected.
        headerZoneSummary.setText (selected.empty() ? "Select a zone to edit" : "Multiple zones selected",
                                    juce::dontSendNotification);
    }
    else
    {
        // Mirrors SliceControlBar::drawSfzZoneSummary()'s field set and
        // formatting exactly (loKey/hiKey/ROOT/PITCH/PAN/VOLUME/RELEASE/
        // LOOP) so this reads as the same information, not a shorthand
        // version of it — see this label's declaration comment.
        const auto note = [] (int n) { return UIHelpers::midiNoteToName (juce::jlimit (0, 127, n)); };
        const juce::String panStr = zone->pan == 0.0f
            ? "C" : (zone->pan < 0.0f ? "L" : "R") + juce::String (juce::roundToInt (std::abs (zone->pan) * 100.0f));

        juce::String text = zone->sampleFile.getFileName().isNotEmpty()
                                ? zone->sampleFile.getFileName() : juce::String ("(no sample)");
        text << "   loKey " << note (zone->lowKey)
             << "   hiKey " << note (zone->highKey)
             << "   ROOT "  << (zone->rootKey >= 0 ? note (zone->rootKey) : juce::String ("--"))
             << "   PITCH " << juce::roundToInt (zone->tuneCents) << "ct"
             << "   PAN "   << panStr
             << "   VOLUME " << juce::String (zone->gainDb, 1) << "dB"
             << "   RELEASE " << juce::String (zone->releaseSeconds, 3) << "s"
             << "   LOOP "  << (zone->loopMode != LoopMode::noLoop ? "ON" : "OFF");
        headerZoneSummary.setText (text, juce::dontSendNotification);
    }

    // Drives the shared SliceControlBar — see this method's declaration
    // comment. PluginEditor reads getSelectedZoneIndex()/getInstrument()
    // right after this fires to build the SCB readout via
    // sliceControlBar.setSfzZoneSummary().
    if (onZoneSelectionOrEditChanged) onZoneSelectionOrEditChanged();
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
                    z.loopStart = 0;
                    z.loopEnd   = reader->lengthInSamples - 1;
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
