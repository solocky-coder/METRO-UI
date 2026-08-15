#include "MultisamplerEditor.h"
#include "../../PluginProcessor.h"
#include "../DysektLookAndFeel.h"
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

    void configureEditableField (juce::Label& l)
    {
        l.setEditable (false, true, false);   // single click doesn't start editing; double-click does
        l.setJustificationType (juce::Justification::centred);
        l.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    }
}

MultisamplerEditor::MultisamplerEditor (DysektProcessor& processorToUse)
    : processor (processorToUse)
{
    instrument.name = "New Instrument";

    addAndMakeVisible (zoneMapView);
    zoneMapView.setInstrument (&instrument);
    zoneMapView.onSelectionChanged = [this] { refreshInspectorFromSelection(); };
    zoneMapView.onZoneEditing      = [this] { dirty = true; repaint(); };
    zoneMapView.onZoneEditCommitted = [this]
    {
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

    // ── Inspector strip ──────────────────────────────────────────────────
    configureStaticLabel (inspectorTitle, "NO ZONE SELECTED");
    inspectorTitle.setFont (juce::FontOptions (11.0f));
    addAndMakeVisible (inspectorTitle);

    struct FieldSpec { juce::Label* labelWidget; juce::Label* fieldWidget; const char* labelText; };
    const FieldSpec specs[] = {
        { &lowKeyLabel,  &lowKeyField,  "LO KEY" },
        { &highKeyLabel, &highKeyField, "HI KEY" },
        { &rootKeyLabel, &rootKeyField, "ROOT" },
        { &lowVelLabel,  &lowVelField,  "LO VEL" },
        { &highVelLabel, &highVelField, "HI VEL" },
        { &gainLabel,    &gainField,    "GAIN dB" },
        { &panLabel,     &panField,     "PAN" },
    };
    for (const auto& s : specs)
    {
        configureStaticLabel (*s.labelWidget, s.labelText);
        s.labelWidget->setFont (juce::FontOptions (9.0f));
        s.labelWidget->setJustificationType (juce::Justification::centred);
        addAndMakeVisible (*s.labelWidget);

        configureEditableField (*s.fieldWidget);
        s.fieldWidget->onTextChange = [this] { applyInspectorFieldsToSelection(); };
        addAndMakeVisible (*s.fieldWidget);
    }
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

    auto inspector = r.removeFromBottom (kInspectorH - 4);
    r.removeFromTop (6);

    zoneMapView.setBounds (r);

    inspectorTitle.setBounds (inspector.removeFromLeft (130));
    const int numFields = 7;
    const int fieldW = juce::jmax (36, inspector.getWidth() / numFields);
    juce::Label* labels[] = { &lowKeyLabel, &highKeyLabel, &rootKeyLabel, &lowVelLabel, &highVelLabel, &gainLabel, &panLabel };
    juce::Label* fields[] = { &lowKeyField, &highKeyField, &rootKeyField, &lowVelField, &highVelField, &gainField, &panField };
    for (int i = 0; i < numFields; ++i)
    {
        auto col = inspector.removeFromLeft (fieldW);
        labels[i]->setBounds (col.removeFromTop (12));
        fields[i]->setBounds (col);
    }
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

        // Same palette/indexing as parseSfzZones() so a file's zone colours
        // read identically regardless of which editor produced this list —
        // see toKeyzones()'s header comment.
        kz.colour = SfzZoneColours::zoneColour (colIdx);
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

    juce::Label* fields[] = { &lowKeyField, &highKeyField, &rootKeyField, &lowVelField, &highVelField, &gainField, &panField };
    for (auto* f : fields) f->setEnabled (zone != nullptr);

    if (zone == nullptr)
    {
        inspectorTitle.setText (selected.empty() ? "NO ZONE SELECTED" : "MULTIPLE ZONES SELECTED", juce::dontSendNotification);
        for (auto* f : fields) f->setText ({}, juce::dontSendNotification);
        return;
    }

    inspectorTitle.setText (zone->sampleFile.getFileName().isNotEmpty()
                                ? zone->sampleFile.getFileName() : juce::String ("(no sample)"),
                             juce::dontSendNotification);
    lowKeyField.setText  (juce::String (zone->lowKey),  juce::dontSendNotification);
    highKeyField.setText (juce::String (zone->highKey), juce::dontSendNotification);
    rootKeyField.setText (juce::String (zone->rootKey), juce::dontSendNotification);
    lowVelField.setText  (juce::String (zone->lowVelocity),  juce::dontSendNotification);
    highVelField.setText (juce::String (zone->highVelocity), juce::dontSendNotification);
    gainField.setText    (juce::String (zone->gainDb, 1), juce::dontSendNotification);
    panField.setText     (juce::String (zone->pan, 2),    juce::dontSendNotification);
}

void MultisamplerEditor::applyInspectorFieldsToSelection()
{
    if (inspectedZoneId == juce::Uuid::null()) return;
    auto* zone = instrument.findZone (inspectedZoneId);
    if (zone == nullptr) return;

    const int lo   = juce::jlimit (0, 127, lowKeyField.getText().getIntValue());
    const int hi    = juce::jlimit (lo, 127, highKeyField.getText().getIntValue());
    const int root  = juce::jlimit (0, 127, rootKeyField.getText().getIntValue());
    const int loVel = juce::jlimit (1, 127, lowVelField.getText().getIntValue());
    const int hiVel  = juce::jlimit (loVel, 127, highVelField.getText().getIntValue());

    zone->lowKey       = lo;
    zone->highKey      = hi;
    zone->rootKey       = root;
    zone->lowVelocity   = loVel;
    zone->highVelocity  = hiVel;
    zone->gainDb        = (float) gainField.getText().getDoubleValue();
    zone->pan           = juce::jlimit (-1.0f, 1.0f, (float) panField.getText().getDoubleValue());

    dirty = true;
    zoneMapView.refresh();
    scheduleEngineSync();
    if (onInstrumentChanged) onInstrumentChanged();
}
