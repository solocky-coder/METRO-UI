#include "MultisamplerEditor.h"
#include "../../PluginProcessor.h"
#include "../DysektLookAndFeel.h"
#include "../SliceControlBar.h"
#include "../../audio/multisampler/SfzImporter.h"
#include "../../audio/multisampler/SfzExporter.h"
#include "../../audio/SfzZoneColours.h"

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

    // ── Table view (ViewMode::Table — what ZONES actually is now) ────────
    // A KeysPanel in SFZ-editable mode, fed from `instrument` via
    // refreshZoneTable()/tableRowZoneIds instead of parsed .sfz text. Its
    // own right-click menu is skipped entirely in favour of reusing
    // zoneMapView.showZoneContextMenu() (same delete/recolour code either
    // view's context menu ends up calling), so there's exactly one
    // implementation of "what happens when you delete/recolour a zone".
    addChildComponent (zoneTableView);   // hidden until setViewMode(Table)
    zoneTableView.setEngineSource (KeysPanel::EngineSource::SfzPlayer2);
    zoneTableView.setSfzEditable (true);
    zoneTableView.setSlicerHighlightEnabled (false);   // this instance only ever shows SFZ/SF2 zones, never the Slicer
    zoneTableView.setAddZoneButtonVisible (false);     // the header's own ADD ZONE button covers both view modes
    zoneTableView.onRowClicked = [this] (int row) { selectZoneAtTableRow (row); };
    zoneTableView.onZoneEdited = [this] (int row, const KeysPanel::Keyzone& kz) { applyTableRowEdit (row, kz); };
    zoneTableView.onRowRightClicked = [this] (int row, juce::Point<int> screenPos)
    {
        if (row < 0 || row >= (int) tableRowZoneIds.size())
            return;
        selectZoneAtTableRow (row);
        zoneMapView.showZoneContextMenu (tableRowZoneIds[(size_t) row], screenPos);
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

    // ── Selection status strip ──────────────────────────────────────────
    configureStaticLabel (inspectorTitle, "NO ZONE SELECTED");
    inspectorTitle.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (inspectorTitle);

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
    g.drawHorizontalLine (getHeight() - kInspectorH, 4.0f, bounds.getWidth() - 4.0f);

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

    inspectorTitle.setBounds (r.removeFromBottom (kInspectorH - 4));
    r.removeFromTop (6);

    zoneMapView.setBounds (r);
    zoneTableView.setBounds (r);   // only one of the two is ever visible — see setViewMode()
}

void MultisamplerEditor::setViewMode (ViewMode mode)
{
    if (viewMode == mode)
        return;
    viewMode = mode;

    if (viewMode == ViewMode::Table)
        refreshZoneTable();   // may already be current (refreshInspectorFromSelection keeps it live) but cheap, and guards a stale table on first switch

    zoneMapView.setVisible   (viewMode == ViewMode::Map);
    zoneTableView.setVisible (viewMode == ViewMode::Table);
    repaint();
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
    setInstrument (MultisamplerInstrument{});
    instrument.name = "New Instrument";
}

void MultisamplerEditor::addZoneClicked()
{
    // Same convention as PluginEditor's raw-SFZ zone builder
    // (openZoneBuilderAddZone): pick a sample first, then confirm the key
    // range in AddZoneOverlay before committing the zone.
    fileChooser = std::make_unique<juce::FileChooser> ("Add sample as new zone…", juce::File(),
                                                         "*.wav;*.aif;*.aiff;*.flac;*.ogg");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            const auto chosen = fc.getResult();
            if (! chosen.existsAsFile()) return; // cancelled

            // Default: start just above the highest key currently mapped, so a
            // second Add Zone doesn't silently overlap the first — mirrors
            // openZoneBuilderAddZone()'s prevHiKey logic exactly.
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
            importFromFile (file);
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
        clearDirtyFlag();
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
    if (! SfzExporter::exportToFile (instrument, cacheFile, opts))
        return;

    // UI thread only; SfzPlayer posts the load atomically and swaps at the
    // next audio block boundary (see SfzPlayer.h threading comment) — no
    // audio-thread file access happens here or inside loadFile() itself.
    processor.sfzPlayer2.loadFile (cacheFile, processor.fileLoadPool);

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
        processor.zoneBuilderReloadPending.store (true, std::memory_order_release);
    processor.loadSoundFontAsync (cacheFile, SoundFontLoadTarget::SfzPlayer2);
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
        inspectorTitle.setText (selected.empty() ? "NO ZONE SELECTED" : "MULTIPLE ZONES SELECTED", juce::dontSendNotification);
    }
    else
    {
        inspectorTitle.setText (zone->sampleFile.getFileName().isNotEmpty()
                                    ? zone->sampleFile.getFileName() : juce::String ("(no sample)"),
                                 juce::dontSendNotification);
    }

    // Drives the shared SliceControlBar — see this method's declaration
    // comment. PluginEditor reads getSelectedZoneIndex()/getInstrument()
    // right after this fires to build the SCB readout, exactly mirroring
    // ZONES' equivalent push into sliceControlBar.setSfzZoneSummary().
    if (onZoneSelectionOrEditChanged) onZoneSelectionOrEditChanged();

    // Runs after every model mutation (this method is called at the end of
    // every one of them — drag edit/commit, delete, Add Zone, SCB field
    // edit, import/New/discard), regardless of which view is currently
    // visible, so the table is never showing stale data by the time the
    // user switches to it.
    refreshZoneTable();
}

void MultisamplerEditor::refreshZoneTable()
{
    std::vector<KeysPanel::Keyzone> zones;
    tableRowZoneIds.clear();
    zones.reserve (instrument.zones.size());
    tableRowZoneIds.reserve (instrument.zones.size());

    // Mirrors toKeyzones()'s field mapping and disabled-zone skip exactly
    // (see refreshZoneTable()'s declaration comment for why this isn't
    // just a call to toKeyzones() with ids bolted on) — same colours, same
    // names, same row order a keyboard-highlight consumer would see, just
    // with a parallel id vector so table edits can find their way back to
    // a real zone.
    int colIdx = 0;
    for (const auto& z : instrument.zones)
    {
        if (! z.enabled)
            continue;

        KeysPanel::Keyzone kz;
        kz.loKey     = z.lowKey;
        kz.hiKey     = z.highKey;
        kz.loVel     = z.lowVelocity;
        kz.hiVel     = z.highVelocity;
        kz.rootPitch = z.rootKey;
        kz.isLooped  = (z.loopMode != LoopMode::noLoop);
        kz.isSfz     = true;

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
        tableRowZoneIds.push_back (z.id);
    }

    zoneTableView.setKeyzones (std::move (zones));
}

void MultisamplerEditor::selectZoneAtTableRow (int row)
{
    if (row < 0 || row >= (int) tableRowZoneIds.size())
        return;

    // zoneMapView's selection state stays the single source of truth for
    // "what's selected" in both view modes (rather than tracking a second,
    // parallel selection here) — refreshInspectorFromSelection() already
    // only ever reads zoneMapView.getSelectedZoneIds(), so routing table
    // clicks through the same setSelectedZoneIds() means that logic, and
    // getSelectedZoneIndex()'s inspectedZoneId, don't need to know or care
    // which view the click actually came from.
    zoneMapView.setSelectedZoneIds ({ tableRowZoneIds[(size_t) row] });
    refreshInspectorFromSelection();   // setSelectedZoneIds() doesn't fire onSelectionChanged itself — see its doc comment
}

void MultisamplerEditor::applyTableRowEdit (int row, const KeysPanel::Keyzone& edited)
{
    if (row < 0 || row >= (int) tableRowZoneIds.size())
        return;

    const auto targetId = tableRowZoneIds[(size_t) row];
    SampleZone* zone = nullptr;
    for (auto& z : instrument.zones)
        if (z.id == targetId) { zone = &z; break; }
    if (zone == nullptr)
        return;   // stale row (instrument changed elsewhere since the table was last refreshed)

    // Same fields applySliceControlBarFieldEdit() writes, just all at once
    // from one table-row edit instead of one SCB field at a time.
    zone->lowKey        = edited.loKey;
    zone->highKey        = edited.hiKey;
    zone->lowVelocity    = edited.loVel;
    zone->highVelocity   = edited.hiVel;
    zone->rootKey        = edited.rootPitch;
    zone->gainDb         = edited.volDb;
    zone->pan            = edited.pan;
    zone->tuneCents      = edited.tuneCents;
    zone->releaseSeconds = edited.releaseSec;

    dirty = true;
    zoneMapView.refresh();
    scheduleEngineSync();
    if (onInstrumentChanged) onInstrumentChanged();
    refreshInspectorFromSelection();   // also refreshes the table itself, see its trailing call
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
