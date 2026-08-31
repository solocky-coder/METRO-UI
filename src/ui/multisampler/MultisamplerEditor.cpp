#include "MultisamplerEditor.h"
#include "../../PluginProcessor.h"
#include "../DysektLookAndFeel.h"
#include "../SliceControlBar.h"
#include "../../audio/multisampler/SfzImporter.h"
#include "../../audio/multisampler/SfzExporter.h"
#include "../../audio/SfzZoneColours.h"
#include <algorithm>
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
    zoneMapView.onZoneAdded = [this]
    {
        // "Repeat Zone" / "Paste Zone" (right-click menu) — same downstream
        // effects as onZoneEditCommitted/onZoneDeleted above; adding a zone
        // is just another committed model edit.
        dirty = true;
        refreshInspectorFromSelection();
        scheduleEngineSync();
        if (onInstrumentChanged) onInstrumentChanged();
    };
    zoneMapView.onTrimZoneRequested = [this] (juce::Uuid zoneIdToTrim)
    {
        beginTrimExistingZone (zoneIdToTrim);
    };

    configureStaticLabel (titleLabel, "MULTISAMPLER");
    // Bumped 13->17 per feedback on the annotated screenshot — the section
    // title read too small next to the rest of the (now taller, kHeaderH
    // 32->40) header row.
    titleLabel.setFont (juce::FontOptions (17.0f, juce::Font::bold));
    addAndMakeVisible (titleLabel);

    // zoneTagLabel/zoneBadgeLabel start blank — refreshZoneLcdDisplay()
    // (called at the end of this constructor) populates them from
    // zoneLcd.getZoneTitleText()/isShowingPreview()/isShowingAuditioning()
    // the same way it already does for zoneLcd itself.
    configureStaticLabel (zoneTagLabel, {});
    // Centred within its slot (set in resized()), which now floats in the
    // middle of the header's blank gap rather than hugging the toolbar —
    // per feedback on the annotated screenshot, sitting flush against the
    // toolbar read as disconnected from the title on the opposite side.
    zoneTagLabel.setJustificationType (juce::Justification::centred);
    // Bumped 12.5->15 alongside the reposition in resized() — floating
    // alone in the header now instead of tucked beside the toolbar, it
    // needed to read as prominently as the buttons across from it.
    zoneTagLabel.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    addAndMakeVisible (zoneTagLabel);

    configureStaticLabel (zoneBadgeLabel, {});
    zoneBadgeLabel.setFont (juce::FontOptions (10.0f, juce::Font::bold));
    addAndMakeVisible (zoneBadgeLabel);

    addAndMakeVisible (editLayerCombo);
    editLayerCombo.setTextWhenNothingSelected ("EDIT LAYER");
    editLayerCombo.setTextWhenNoChoicesAvailable ("EDIT LAYER");
    editLayerCombo.setEnabled (false);
    editLayerCombo.onChange = [this]
    {
        const int idx = editLayerCombo.getSelectedId() - 1;
        if (idx < 0 || idx >= (int) editLayerStackIds.size())
            return;
        // Fires ZoneMapView::onSelectionChanged -> refreshInspectorFromSelection()
        // -> refreshEditLayerCombo(), which re-selects this same entry — see
        // that method's doc comment.
        zoneMapView.bringZoneToFrontForEditing (editLayerStackIds[(size_t) idx]);
    };

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
    zoneLcd.onFieldLearnMenuRequested = [this] (MultisamplerZoneField field, juce::Point<int> screenPos)
    {
        showMidiLearnMenu (field, screenPos);
    };
    zoneLcd.setMidiLearnManager (&processor.midiLearn);

    // OUT (output bus) only makes sense to show where something can act on
    // an AUX 1–15 choice. In a real DAW host, any bus can be routed
    // freely, but the standalone build hard-caps its audio device to 2
    // output channels and only ever pulls the processor's Main bus into
    // the callback (see standalone/MainWindow.h) — so AUX 1–15 there is a
    // dead end: audio renders internally but never reaches a speaker. Set
    // once at construction (wrapperType doesn't change for the lifetime of
    // an editor instance) via JUCE's own wrapperType, the same mechanism
    // used elsewhere in this app to distinguish standalone from a hosted
    // plugin. See MultisamplerZoneLcd::setOutputBusVisible()'s doc comment
    // for why this removes the field from the layout rather than just
    // disabling it.
    zoneLcd.setOutputBusVisible (processor.wrapperType != juce::AudioProcessor::wrapperType_Standalone);

    refreshZoneLcdDisplay();   // starts empty — nothing selected/hovered yet

    refreshInspectorFromSelection();   // starts disabled — nothing selected yet

    // Steady 30 Hz poll for CC values processMidi() stages into
    // processor.msMidiLearn* — see pollMidiLearnCc()'s doc comment in the
    // header for why the audio thread can't just apply them itself.
    midiLearnPoller.start();
}

MultisamplerEditor::~MultisamplerEditor()
{
    midiLearnPoller.stop();
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
    const auto headerFull = header;   // full-width row, before left/right consumption below
    titleLabel.setBounds (header.removeFromLeft (140));
    header.removeFromRight (4);
    newButton.setBounds    (header.removeFromRight (60));
    header.removeFromRight (4);
    exportButton.setBounds (header.removeFromRight (90));
    header.removeFromRight (4);
    importButton.setBounds (header.removeFromRight (90));
    header.removeFromRight (4);
    addZoneButton.setBounds (header.removeFromRight (84));
    header.removeFromRight (4);
    editLayerCombo.setBounds (header.removeFromRight (150));

    // zoneBadgeLabel (the PREVIEW/AUDITIONING badge) keeps a small fixed
    // slot right against the combo, since it's contextual to editing.
    header.removeFromRight (8);
    zoneBadgeLabel.setBounds (header.removeFromRight (90));
    header.removeFromRight (6);

    // zoneTagLabel ("ZONE NN   name") centres itself on controlFrameCentreX
    // — the x-centre of the DualLcdControlFrame one row up, supplied by
    // PluginEditor every layout pass via setControlFrameCentreX() — so it
    // reads as tied to that frame rather than to this header's own,
    // asymmetric left/right toolbar clusters. Clamped against the FULL
    // header row (headerFull), not the leftover gap between clusters,
    // since the leftover gap is exactly the wrong-answer this replaces —
    // clamping to it would just reproduce the old off-centre behaviour on
    // any window narrow enough for the target point to fall near a
    // button. Falls back to centring in the full header only until
    // PluginEditor has laid out at least once (controlFrameCentreX == -1).
    constexpr int kZoneTagW = 240;
    const int fallbackCentreX = headerFull.getCentreX();
    int tagCentreX = (controlFrameCentreX >= 0) ? controlFrameCentreX : fallbackCentreX;
    tagCentreX = juce::jlimit (headerFull.getX() + kZoneTagW / 2,
                                headerFull.getRight() - kZoneTagW / 2,
                                tagCentreX);
    zoneTagLabel.setBounds (juce::Rectangle<int> (kZoneTagW, headerFull.getHeight())
                                 .withCentre ({ tagCentreX, headerFull.getCentreY() }));

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
    wireZoneTrimOverlayAudition();
    zoneTrimOverlay->onResult = [this, sampleFile] (AddZoneTrimOverlay::Result result, bool confirmed)
    {
        // Stop chromatic audition immediately — the overlay (and whatever
        // was auditioning through it) is closing either way. See
        // wireZoneTrimOverlayAudition()'s declaration comment.
        processor.resetTrimAudition();

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

void MultisamplerEditor::wireZoneTrimOverlayAudition()
{
    // Wires zoneTrimOverlay's chromatic-audition hooks (onSampleDecoded/
    // onTrimChanged/onAuditionNote) to PluginProcessor's trim-audition voice
    // pool — shared by both call sites that open the overlay
    // (beginAddZoneTrim() for a brand-new sample, beginTrimExistingZone()
    // for "Trim Sample" on an existing zone). See PluginProcessor.h's
    // trimAuditionActive doc comment for why this stays independent of the
    // live instrument.
    jassert (zoneTrimOverlay != nullptr);

    processor.trimAuditionActive.store (true, std::memory_order_relaxed);

    zoneTrimOverlay->onSampleDecoded = [this] (SampleData::SnapshotPtr decoded, double sourceSampleRate)
    {
        processor.trimAuditionSample.applyDecodedSample (std::move (decoded));
        processor.trimAuditionSampleRate.store (sourceSampleRate, std::memory_order_relaxed);
    };
    zoneTrimOverlay->onTrimChanged = [this] (int64_t start, int64_t end)
    {
        processor.trimAuditionRegionStart.store (start, std::memory_order_relaxed);
        processor.trimAuditionRegionEnd.store   (end,   std::memory_order_relaxed);
    };
    zoneTrimOverlay->onAuditionNote = [this] (int note, bool isOn)
    {
        processor.triggerTrimAuditionNote (note, isOn);
    };
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

void MultisamplerEditor::beginTrimExistingZone (const juce::Uuid& zoneId)
{
    // "Trim Sample" (right-click menu) — reopens the same overlay used to
    // trim a brand new sample in the Add Zone flow, but seeded with an
    // existing zone's current sampleFile/sampleStart/sampleEnd instead. See
    // this method's declaration comment for why it writes straight back to
    // the zone rather than routing through beginAddZoneKeyMapping()/
    // commitAddedZone().
    auto* zone = instrument.findZone (zoneId);
    if (zone == nullptr) return;

    // Nothing to decode/trim if the sample doesn't currently resolve —
    // same guard AddZoneTrimOverlay's own decode-failure path would hit
    // anyway, just without the wasted round trip through the file system.
    if (zone->hasMissingSample()) return;

    zoneTrimOverlay = std::make_unique<AddZoneTrimOverlay> (zone->sampleFile, processor.fileLoadPool,
                                                             zone->sampleStart, zone->sampleEnd);
    wireZoneTrimOverlayAudition();
    zoneTrimOverlay->onResult = [this, zoneId] (AddZoneTrimOverlay::Result result, bool confirmed)
    {
        // Stop chromatic audition immediately — same reasoning as
        // beginAddZoneTrim()'s onResult.
        processor.resetTrimAudition();

        // Same deferred-reset use-after-free fix beginAddZoneTrim() uses:
        // let onResult unwind before the overlay (currently on the call
        // stack) is destroyed.
        juce::MessageManager::callAsync ([this]
        {
            if (zoneTrimOverlay)
            {
                removeChildComponent (zoneTrimOverlay.get());
                zoneTrimOverlay.reset();
            }
        });

        if (! confirmed) return;   // cancelled or decode failed — instrument untouched

        auto* z = instrument.findZone (zoneId);
        if (z == nullptr) return;   // zone was deleted while the overlay was open

        // Same "-1 == full sample length" convention commitAddedZone()
        // writes for a brand new zone — see its comment — so re-trimming
        // back out to the full file round-trips to the same sampleEnd a
        // never-trimmed zone would have, not a literal frame count frozen
        // to this file's length right now.
        const bool spansWholeFile = result.start == 0 && result.end >= result.totalFrames;
        z->sampleStart = result.start;
        z->sampleEnd   = spansWholeFile ? -1 : result.end;

        dirty = true;
        zoneMapView.refresh();
        refreshInspectorFromSelection();
        scheduleEngineSync();
        if (onInstrumentChanged) onInstrumentChanged();
        repaint();
    };

    addAndMakeVisible (*zoneTrimOverlay);
    zoneTrimOverlay->setBounds (getLocalBounds());
    zoneTrimOverlay->toFront (true);
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

    // A lone selected zone is trivially its own top layer. With 2+
    // selected, auto-elect the one sitting highest in z-order (drawn on
    // top) instead of leaving inspectedZoneId null and falling back to a
    // dead-end "multiple selected" state — see
    // ZoneMapView::topmostZoneAmong()'s doc comment.
    juce::Uuid showId;
    if (selected.size() == 1)
        showId = selected.front();
    else if (selected.size() >= 2)
        showId = zoneMapView.topmostZoneAmong (selected);

    const SampleZone* zone = (showId != juce::Uuid::null()) ? instrument.findZone (showId) : nullptr;

    inspectedZoneId = (zone != nullptr) ? zone->id : juce::Uuid::null();

    refreshEditLayerCombo();

    // zoneLcd is now the sole display AND the sole edit surface for this
    // data (see MultisamplerZoneLcd.h) — it renders its own "NO ZONE
    // SELECTED" / full-field-readout states directly from
    // setZoneForDisplay()/clearZone(), so there is no separate text to
    // build here any more.
    refreshZoneLcdDisplay();

    // PluginEditor reads getSelectedZoneIndex()/getSelectedZoneCount()/
    // getInstrument() right after this fires to keep sliceLcd's and
    // multisamplerWaveformLcd's waveform displays in sync — see this
    // method's declaration comment and syncMultisamplerDisplay().
    if (onZoneSelectionOrEditChanged) onZoneSelectionOrEditChanged();
}

void MultisamplerEditor::refreshEditLayerCombo()
{
    editLayerStackIds.clear();
    editLayerCombo.clear (juce::dontSendNotification);

    int inspectedIdx = -1;
    if (inspectedZoneId != juce::Uuid::null())
        for (size_t i = 0; i < instrument.zones.size(); ++i)
            if (instrument.zones[i].id == inspectedZoneId) { inspectedIdx = (int) i; break; }

    if (inspectedIdx < 0)
    {
        editLayerCombo.setEnabled (false);
        return;
    }

    // Same pairwise key/velocity-overlap definition ZoneMapView's overlap
    // hatching and showZoneContextMenu()'s "Edit Layer" submenu are built
    // on, scoped here to just the currently inspected zone's stack (this
    // control has no click position of its own to test against).
    std::vector<size_t> stackIdx { (size_t) inspectedIdx };
    for (const auto& pair : instrument.findOverlappingPairs())
    {
        if (pair.first == (size_t) inspectedIdx)       stackIdx.push_back (pair.second);
        else if (pair.second == (size_t) inspectedIdx) stackIdx.push_back (pair.first);
    }

    if (stackIdx.size() < 2)
    {
        editLayerCombo.setEnabled (false);
        return;
    }

    std::sort (stackIdx.begin(), stackIdx.end());
    stackIdx.erase (std::unique (stackIdx.begin(), stackIdx.end()), stackIdx.end());

    editLayerCombo.setEnabled (true);
    int itemId = 1;
    int selectId = 1;
    for (auto idx : stackIdx)
    {
        const auto& z = instrument.zones[idx];
        const juce::String label = juce::String (itemId) + ". "
            + (z.sampleFile != juce::File() ? z.sampleFile.getFileNameWithoutExtension()
                                             : juce::String ("(no sample)"))
            + "  [" + UIHelpers::midiNoteToName (z.lowKey) + "-" + UIHelpers::midiNoteToName (z.highKey)
            + ", v" + juce::String (z.lowVelocity) + "-" + juce::String (z.highVelocity) + "]";
        editLayerCombo.addItem (label, itemId);
        editLayerStackIds.push_back (z.id);
        if (z.id == inspectedZoneId) selectId = itemId;
        ++itemId;
    }
    editLayerCombo.setSelectedId (selectId, juce::dontSendNotification);
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
            // Auto-elect the top-most (highest z-order, i.e. drawn-on-top)
            // zone among the selection for editing, rather than the old
            // "MULTIPLE ZONES SELECTED" dead end — matches ZoneMapView's
            // own topmost-wins hit-testing convention (topmostZoneAt()/
            // zonesAt()), so the zone shown here is the same one a click
            // at the stack's location would have hit.
            showId = zoneMapView.topmostZoneAmong (selectedIds);
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
        zoneTagLabel.setText ({}, juce::dontSendNotification);
        zoneBadgeLabel.setText ({}, juce::dontSendNotification);
        return;
    }

    zoneLcd.setZoneForDisplay (zone, displayIndex, isPreview, /*isAuditioning*/ false);
    // Editable whenever the zone being shown is a genuine selection target
    // — the true single selection, or the auto-elected top layer of a
    // multi-selection — but never a hover preview (matches
    // MultisamplerZoneLcd::setEditable's doc comment contract).
    zoneLcd.setEditable (! isPreview && ! selectedIds.empty());

    // zoneTagLabel/zoneBadgeLabel mirror exactly what zoneLcd's own title
    // row used to draw internally — sourced from zoneLcd itself (rather
    // than re-deriving displayIndex/isPreview here) so there's a single
    // place (setZoneForDisplay's snapshot) that owns that formatting.
    zoneTagLabel.setText (zoneLcd.getZoneTitleText(), juce::dontSendNotification);

    const auto& theme = getTheme();
    if (zoneLcd.isShowingPreview())
    {
        zoneBadgeLabel.setText ("PREVIEW", juce::dontSendNotification);
        zoneBadgeLabel.setColour (juce::Label::textColourId, theme.accent.withAlpha (0.8f));
    }
    else if (zoneLcd.isShowingAuditioning())
    {
        zoneBadgeLabel.setText ("AUDITIONING", juce::dontSendNotification);
        zoneBadgeLabel.setColour (juce::Label::textColourId, theme.accent);
    }
    else
    {
        zoneBadgeLabel.setText ({}, juce::dontSendNotification);
    }
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
        // Attack/Decay/Sustain node drags on MultisamplerWaveformLcd's
        // envelope graph route here (see PluginEditor's onZoneParamEdited
        // wiring) but were previously missing from this switch, so they
        // fell through to `default: return;` and were silently discarded —
        // dragging those three nodes did nothing to the zone even though
        // dragging the Release node (below) worked, and the separate
        // MultisamplerZoneLcd numeric fields (applyZoneFieldEdit) already
        // handled all four correctly. This is the "ADSR nodes and knobs
        // don't sync" bug: only Release ever reached the model.
        case SliceControlBar::ZoneAttack:  z.attackSeconds  = juce::jmax (0.0f, value); break;
        case SliceControlBar::ZoneDecay:   z.decaySeconds   = juce::jmax (0.0f, value); break;
        case SliceControlBar::ZoneSustain: z.sustainLevel   = juce::jlimit (0.0f, 1.0f, value); break;
        case SliceControlBar::ZoneRelease: z.releaseSeconds = juce::jmax (0.0f, value); break;
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
        case SliceControlBar::ZoneOneShot:
            // LoopMode is a single enum value, so this and ZoneLoop above
            // are mutually exclusive by construction — turning one on
            // silently supersedes the other already being on, with no
            // separate "clear the other flag" step needed (contrast
            // SliceLcdDisplay::mouseDown's Slicer-engine LOOP/1SH branches,
            // which really do need that extra step since Slice keeps LOOP
            // and one-shot as two independent bools).
            z.loopMode = (value > 0.5f) ? LoopMode::oneShot : LoopMode::noLoop;
            break;
        case SliceControlBar::ZoneReverse:
            z.reverse = value > 0.5f;
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
    // The only zone this can ever be editing is whatever refreshZoneLcdDisplay()
    // most recently called zoneLcd.setEditable(true) for — the true single
    // selection, or the auto-elected top layer when 2+ zones are selected
    // (see topmostZoneAmong()) — and inspectedZoneId tracks exactly that zone.
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
            // GROUP/OFF BY controls removed from the UI entirely — see
            // MultisamplerZoneLcd.cpp's layoutRow comment. group/offBy were
            // never wired to real choke behaviour in VoicePool (confirmed:
            // SoundFontLoader never reads them when building playback
            // slices), so this was a knob that could drift to arbitrary
            // values (no upper clamp existed) and audibly do nothing.
            // The enum slot is deliberately left in place, not deleted --
            // removing it would renumber every later field's MIDI Learn
            // slot index (kMidiLearnNumSlots is derived from this enum's
            // integer values) and silently remap any user's existing
            // learned CCs to the wrong control. Left as a no-op instead:
            // MIDI Learn can still "arm" this slot, but nothing happens on
            // incoming CC, matching there being no visible control to
            // reflect a change anyway. z.group itself is untouched here so
            // any value read from an externally-authored SFZ file (via
            // SfzImporter) still round-trips correctly on export.
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
        case MultisamplerZoneField::eq1Freq:
            // Matches SampleZone::eq1Freq's own documented range.
            z.eq1Freq = juce::jlimit (20.0f, 1000.0f, value);
            break;
        case MultisamplerZoneField::eq1Gain:
            z.eq1Gain = juce::jlimit (-24.0f, 24.0f, value);
            break;
        case MultisamplerZoneField::eq1Bw:
            z.eq1Bw = juce::jlimit (0.1f, 4.0f, value);
            break;
        case MultisamplerZoneField::eq2Freq:
            // Matches SampleZone::eq2Freq's own documented range.
            z.eq2Freq = juce::jlimit (100.0f, 10000.0f, value);
            break;
        case MultisamplerZoneField::eq2Gain:
            z.eq2Gain = juce::jlimit (-24.0f, 24.0f, value);
            break;
        case MultisamplerZoneField::eq2Bw:
            z.eq2Bw = juce::jlimit (0.1f, 4.0f, value);
            break;
        case MultisamplerZoneField::eq3Freq:
            // Matches SampleZone::eq3Freq's own documented range.
            z.eq3Freq = juce::jlimit (1000.0f, 20000.0f, value);
            break;
        case MultisamplerZoneField::eq3Gain:
            z.eq3Gain = juce::jlimit (-24.0f, 24.0f, value);
            break;
        case MultisamplerZoneField::eq3Bw:
            z.eq3Bw = juce::jlimit (0.1f, 4.0f, value);
            break;
        case MultisamplerZoneField::outputBus:
            // 0 = Main, 1-15 = Aux — matches SampleZone::outputBus's own
            // documented range and SfzDrumKitBusApplier/
            // autoAssignOutputBuses()'s clamping below.
            z.outputBus = juce::jlimit (0, 15, juce::roundToInt (value));
            break;
        case MultisamplerZoneField::showInMixer:
            // Manual mixer-pin override — see SampleZone::showInMixer's doc
            // comment. Independent of outputBus: turning this off never
            // hides a zone that's routed off Main (performEngineSync()'s
            // export/reimport ORs it back in via PluginProcessor's
            // PendingZonePin handling).
            z.showInMixer = value > 0.5f;
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
        case MultisamplerZoneField::kCount:
            break;   // sentinel, never a real field — see its doc comment
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

// =============================================================================
// MIDI Learn (Multisampler) — showMidiLearnMenu / pollMidiLearnCc /
// applyMidiLearnCc / getLiveFieldValue
// =============================================================================

void MultisamplerEditor::showMidiLearnMenu (MultisamplerZoneField field, juce::Point<int> screenPos)
{
    const int slot = midiLearnSlotFor (field);
    const bool mapped = processor.midiLearn.isMapped (slot);

    juce::PopupMenu menu;
    menu.addItem (1, "Learn MIDI CC");
    if (mapped)
        menu.addItem (2, "Clear (" + processor.midiLearn.getLabelText (slot) + ")");

    // No "Open MIDI Learn Dialog..." item here, unlike SliceControlBar's
    // version of this menu — that dialog already lists every mapped field
    // plugin-wide, including these (see gSlotParamNames in
    // MidiLearnDialog.cpp), so it isn't specific to this one field's menu.

    auto* topLvl = getTopLevelComponent();
    const float ms = DysektLookAndFeel::getMenuScale();
    menu.showMenuAsync (
        juce::PopupMenu::Options()
            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1))
            .withParentComponent (topLvl)
            .withStandardItemHeight ((int) (24 * ms)),
        [this, slot] (int result)
        {
            if (result == 1)      { processor.midiLearn.armLearn (slot);      zoneLcd.repaint(); }
            else if (result == 2) { processor.midiLearn.clearMapping (slot);  zoneLcd.repaint(); }
        });
}

void MultisamplerEditor::pollMidiLearnCc()
{
    // Cheap and unconditional so an armed->mapped transition or a manual
    // Clear (from showMidiLearnMenu above) shows up promptly even on a
    // tick where nothing else here changed — same reasoning SliceControlBar's
    // own always-on 30 Hz timer uses for its CC-label repaints.
    zoneLcd.repaint();

    for (int i = 0; i < (int) MultisamplerZoneField::kCount; ++i)
    {
        const auto field = static_cast<MultisamplerZoneField> (i);

        // Absolute: test-and-clear the dirty flag so a value that hasn't
        // changed since the last poll isn't reapplied every tick.
        const bool absDirty = processor.msMidiLearnAbsDirty[(size_t) i]
                                   .exchange (false, std::memory_order_relaxed);
        if (absDirty)
        {
            const float norm = processor.msMidiLearnAbsValue[(size_t) i]
                                    .load (std::memory_order_relaxed);
            applyMidiLearnCc (field, norm, /*isRelative=*/false);
        }

        // Relative: drain (exchange-to-zero) whatever delta accumulated
        // across every CC message since the last poll, so rapid encoder
        // turns between two 30 Hz ticks sum correctly instead of only the
        // last one landing.
        const float relDelta = processor.msMidiLearnRelDelta[(size_t) i]
                                    .exchange (0.0f, std::memory_order_relaxed);
        if (relDelta != 0.0f)
            applyMidiLearnCc (field, relDelta, /*isRelative=*/true);
    }
}

void MultisamplerEditor::applyMidiLearnCc (MultisamplerZoneField field, float ccValue, bool isRelative)
{
    if (inspectedZoneId == juce::Uuid::null())
        return;   // nothing selected/editable — mirrors applyZoneFieldEdit's own guard

    if (field == MultisamplerZoneField::loopEnabled || field == MultisamplerZoneField::showInMixer)
    {
        // Toggle fields: absolute CC >= 0.5 sets on, < 0.5 sets off; a
        // relative encoder click of either polarity just flips the current
        // state — same "click = toggle" semantics zoneLcd::mouseDown uses
        // for a direct click on these two cells.
        const bool newState = isRelative ? ! (getLiveFieldValue (field) > 0.5f)
                                          : (ccValue > 0.5f);
        applyZoneFieldEdit (field, newState ? 1.0f : 0.0f, /*isCommit=*/true);
        return;
    }

    float nativeVal;
    if (isRelative)
    {
        // ccValue is a raw signed step count here (same convention
        // PluginProcessor::processMidi()'s other relative-encoder branches
        // decode via MidiLearnManager::processCc). Scale it by the same
        // per-field increment zoneLcd's own mouse-drag path uses per pixel,
        // off the single shared table (MultisamplerZoneLcd::
        // relCcSensitivityFor), so a knob feels the same size turning a
        // physical encoder as dragging it one pixel with the mouse.
        const float sens = MultisamplerZoneLcd::relCcSensitivityFor (field);
        nativeVal = getLiveFieldValue (field) + ccValue * sens;
    }
    else
    {
        nativeVal = MultisamplerZoneLcd::nativeFromNorm (field, ccValue);
    }

    // isCommit = true on every applied CC tick, not just gesture-end: a
    // continuous mouse drag has a clear mouseUp to distinguish live frames
    // from the final commit, but successive ticks from a physical encoder
    // have no equivalent "gesture end" signal. scheduleEngineSync()'s own
    // debounce (kEngineSyncDebounceMs) already collapses a rapid run of
    // these into a single actual engine sync once the encoder goes quiet —
    // same pattern PluginProcessor's relative-encoder path already relies
    // on for the Slicer/SFZ-Player side (see FieldSliceStart's relative
    // branch, which commits via handleCommand every buffer rather than
    // batching to a single deferred commit).
    applyZoneFieldEdit (field, nativeVal, /*isCommit=*/true);
}

float MultisamplerEditor::getLiveFieldValue (MultisamplerZoneField field) const
{
    if (inspectedZoneId == juce::Uuid::null())
        return 0.0f;

    const SampleZone* zonePtr = nullptr;
    for (auto& z : instrument.zones)
        if (z.id == inspectedZoneId) { zonePtr = &z; break; }
    if (zonePtr == nullptr)
        return 0.0f;

    const auto& z = *zonePtr;
    switch (field)
    {
        case MultisamplerZoneField::lowKey:      return (float) z.lowKey;
        case MultisamplerZoneField::highKey:     return (float) z.highKey;
        case MultisamplerZoneField::rootKey:     return (float) z.rootKey;
        case MultisamplerZoneField::tune:        return z.tuneCents;
        case MultisamplerZoneField::pan:         return z.pan;
        case MultisamplerZoneField::gain:        return z.gainDb;
        case MultisamplerZoneField::attack:      return z.attackSeconds;
        case MultisamplerZoneField::decay:       return z.decaySeconds;
        case MultisamplerZoneField::sustain:     return z.sustainLevel;
        case MultisamplerZoneField::release:     return z.releaseSeconds;
        case MultisamplerZoneField::loopEnabled: return z.loopMode != LoopMode::noLoop ? 1.0f : 0.0f;
        case MultisamplerZoneField::cutoff:      return z.filterCutoffHz;
        case MultisamplerZoneField::resonance:   return z.filterResonance;
        case MultisamplerZoneField::group:       return (float) z.group;
        case MultisamplerZoneField::outputBus:   return (float) z.outputBus;
        case MultisamplerZoneField::showInMixer: return z.showInMixer ? 1.0f : 0.0f;
        case MultisamplerZoneField::eq1Freq:     return z.eq1Freq;
        case MultisamplerZoneField::eq1Gain:     return z.eq1Gain;
        case MultisamplerZoneField::eq1Bw:       return z.eq1Bw;
        case MultisamplerZoneField::eq2Freq:     return z.eq2Freq;
        case MultisamplerZoneField::eq2Gain:     return z.eq2Gain;
        case MultisamplerZoneField::eq2Bw:       return z.eq2Bw;
        case MultisamplerZoneField::eq3Freq:     return z.eq3Freq;
        case MultisamplerZoneField::eq3Gain:     return z.eq3Gain;
        case MultisamplerZoneField::eq3Bw:       return z.eq3Bw;
        case MultisamplerZoneField::kCount:      break;   // sentinel, never a real field
    }
    return 0.0f;
}

// Drum-kit auto-routing (PluginEditor::offerDrumKitAutoRouting). Assigns
// buses 1..15 round-robin, in top-to-bottom zones[] order, to the first
// `numZones` zones — writing straight to SampleZone::outputBus so the
// assignment round-trips through save/reload and performEngineSync() the
// same way a manual OUT-cell edit does.
//
// This used to be SfzDrumKitBusApplier, a self-deleting juce::Timer that
// polled processor.getUiSliceSnapshot2() and pushed CmdSetSliceParam
// commands once the async loadSoundFontAsync() load caught up — necessary
// back when the drum-kit prompt targeted sliceManager2 directly, since that
// engine only populates asynchronously. It's no longer needed: the prompt
// now fires from loadSfzIntoMultisampler()'s doImport lambda, strictly
// after multisamplerEditor.importFromFile() has already run SfzImporter
// synchronously and populated `instrument.zones` — so there's nothing left
// to wait on, and the old polling/command-queue indirection was itself the
// bug (it wrote the derived sliceManager2 copy, not SampleZone, so
// performEngineSync()'s next export/reimport silently wiped it — see this
// method's call site for the fuller history).
//
// `numZones` comes from SfzLayoutClassifier's independent parse of the same
// file (see offerDrumKitAutoRouting) rather than instrument.zones.size()
// directly, so this clamps defensively in case the two ever disagree.
void MultisamplerEditor::autoAssignOutputBuses (int numZones)
{
    const int n = juce::jmin (numZones, (int) instrument.zones.size());
    if (n <= 0)
        return;

    for (int i = 0; i < n; ++i)
    {
        // Bus 0 = Main is left alone; 1-15 round-robin. Wraps if there are
        // more than 15 zones — no UI currently exists to pick specific
        // buses per zone, so simple round-robin distribution is the
        // reasonable default (matches the old SfzDrumKitBusApplier's
        // identical choice).
        instrument.zones[(size_t) i].outputBus = 1 + (i % 15);
    }

    dirty = true;
    zoneMapView.refresh();
    scheduleEngineSync();
    if (onInstrumentChanged) onInstrumentChanged();
    refreshZoneLcdDisplay();
}
