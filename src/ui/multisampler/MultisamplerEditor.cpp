#include "MultisamplerEditor.h"
#include "../../PluginProcessor.h"
#include "../DysektLookAndFeel.h"
#include "../../audio/multisampler/SfzImporter.h"
#include "../../audio/multisampler/SfzExporter.h"

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

void MultisamplerEditor::setInstrument (MultisamplerInstrument newInstrument)
{
    instrument = std::move (newInstrument);
    dirty = false;
    zoneMapView.setInstrument (&instrument);
    refreshInspectorFromSelection();
    performEngineSync();   // not debounced — a wholesale swap should reflect immediately
    if (onInstrumentChanged) onInstrumentChanged();
    repaint();
}

void MultisamplerEditor::newInstrumentClicked()
{
    setInstrument (MultisamplerInstrument{});
    instrument.name = "New Instrument";
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
                return;
            }

            setInstrument (std::move (result.instrument));
            if (! result.warnings.empty() && onImportWarnings)
                onImportWarnings (file.getFileName(), true, result.warnings);
        });
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
            SfzExporter::exportToFile (instrument, file, opts);
        });
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
    performEngineSync();
}

void MultisamplerEditor::performEngineSync()
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
