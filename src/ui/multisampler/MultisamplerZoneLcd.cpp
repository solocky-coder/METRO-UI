#include "MultisamplerZoneLcd.h"
#include "../DysektLookAndFeel.h"
#include "../UIHelpers.h"
#include <cmath>

namespace
{
    // Same "hold Shift to fine-tune" convention SliceControlBar's drag cells
    // already use elsewhere in the plugin — kept identical here rather than
    // inventing a second modifier for the same idea.
    constexpr float kFineModeScale = 0.15f;
}

MultisamplerZoneLcd::MultisamplerZoneLcd()
{
    setInterceptsMouseClicks (true, true);
}

// ── Display ──────────────────────────────────────────────────────────────

void MultisamplerZoneLcd::setZoneForDisplay (const SampleZone* zone, int displayIndex,
                                              bool isPreview, bool isAuditioning)
{
    multipleSelected = false;

    if (zone == nullptr)
    {
        clearZone();
        return;
    }

    // Required snapshot copy — see this method's doc comment in the header.
    // Every field read here happens synchronously within this call; nothing
    // below retains `zone` itself.
    snapshot.valid          = true;
    snapshot.name           = zone->sampleFile.getFileName().isNotEmpty()
                                   ? zone->sampleFile.getFileNameWithoutExtension()
                                   : juce::String ("(no sample)");
    snapshot.displayIndex   = displayIndex;
    snapshot.lowKey         = zone->lowKey;
    snapshot.highKey        = zone->highKey;
    snapshot.rootKey        = zone->rootKey;
    snapshot.group          = zone->group;
    snapshot.tuneCents      = zone->tuneCents;
    snapshot.pan            = zone->pan;
    snapshot.gainDb         = zone->gainDb;
    snapshot.attackSeconds  = zone->attackSeconds;
    snapshot.decaySeconds   = zone->decaySeconds;
    snapshot.sustainLevel   = zone->sustainLevel;
    snapshot.releaseSeconds = zone->releaseSeconds;
    snapshot.loopOn         = zone->loopMode != LoopMode::noLoop;
    snapshot.filterCutoffHz = zone->filterCutoffHz;
    snapshot.filterResonance = zone->filterResonance;
    snapshot.isPreview      = isPreview;
    snapshot.isAuditioning  = isAuditioning;

    repaint();
}

void MultisamplerZoneLcd::clearZone()
{
    snapshot = Snapshot{};
    multipleSelected = false;
    repaint();
}

void MultisamplerZoneLcd::setMultipleSelection (bool multiple)
{
    multipleSelected = multiple;
    if (multiple)
        snapshot.valid = false;
    repaint();
}

void MultisamplerZoneLcd::setEditable (bool shouldEdit)
{
    if (editable == shouldEdit) return;
    editable = shouldEdit;
    repaint();
}

// ── Field metadata ───────────────────────────────────────────────────────

juce::String MultisamplerZoneLcd::labelFor (MultisamplerZoneField field) const
{
    switch (field)
    {
        case MultisamplerZoneField::lowKey:      return "LO";
        case MultisamplerZoneField::highKey:     return "HI";
        case MultisamplerZoneField::rootKey:     return "ROOT";
        case MultisamplerZoneField::group:       return "GROUP";
        case MultisamplerZoneField::tune:        return "TUNE";
        case MultisamplerZoneField::pan:         return "PAN";
        case MultisamplerZoneField::gain:        return "GAIN";
        case MultisamplerZoneField::loopEnabled: return "LOOP";
        case MultisamplerZoneField::attack:      return "ATTACK";
        case MultisamplerZoneField::decay:       return "DECAY";
        case MultisamplerZoneField::sustain:     return "SUSTAIN";
        case MultisamplerZoneField::release:     return "RELEASE";
        case MultisamplerZoneField::cutoff:      return "FILTER";
        case MultisamplerZoneField::resonance:   return "RES";
    }
    return {};
}

float MultisamplerZoneLcd::getFieldValue (MultisamplerZoneField field) const
{
    switch (field)
    {
        case MultisamplerZoneField::lowKey:      return (float) snapshot.lowKey;
        case MultisamplerZoneField::highKey:     return (float) snapshot.highKey;
        case MultisamplerZoneField::rootKey:     return (float) snapshot.rootKey;
        case MultisamplerZoneField::group:       return (float) snapshot.group;
        case MultisamplerZoneField::tune:        return snapshot.tuneCents;
        case MultisamplerZoneField::pan:         return snapshot.pan;
        case MultisamplerZoneField::gain:        return snapshot.gainDb;
        case MultisamplerZoneField::loopEnabled: return snapshot.loopOn ? 1.0f : 0.0f;
        case MultisamplerZoneField::attack:      return snapshot.attackSeconds;
        case MultisamplerZoneField::decay:       return snapshot.decaySeconds;
        case MultisamplerZoneField::sustain:     return snapshot.sustainLevel;
        case MultisamplerZoneField::release:     return snapshot.releaseSeconds;
        case MultisamplerZoneField::cutoff:      return snapshot.filterCutoffHz;
        case MultisamplerZoneField::resonance:   return snapshot.filterResonance;
    }
    return 0.0f;
}

// Double-click reset targets — matches the model's own struct defaults in
// SampleZone.h so "reset" means "back to what a freshly added zone would
// have", not an arbitrary editor-chosen number.
float MultisamplerZoneLcd::defaultValueFor (MultisamplerZoneField field) const
{
    switch (field)
    {
        case MultisamplerZoneField::lowKey:      return 0.0f;
        case MultisamplerZoneField::highKey:     return 127.0f;
        case MultisamplerZoneField::rootKey:     return 60.0f;
        case MultisamplerZoneField::group:       return 0.0f;
        case MultisamplerZoneField::tune:        return 0.0f;
        case MultisamplerZoneField::pan:         return 0.0f;
        case MultisamplerZoneField::gain:        return 0.0f;
        case MultisamplerZoneField::loopEnabled: return 0.0f;
        case MultisamplerZoneField::attack:      return 0.005f;
        case MultisamplerZoneField::decay:       return 0.1f;
        case MultisamplerZoneField::sustain:     return 1.0f;
        case MultisamplerZoneField::release:     return 0.1f;
        case MultisamplerZoneField::cutoff:      return 20000.0f;
        case MultisamplerZoneField::resonance:   return 0.0f;
    }
    return 0.0f;
}

// Native units moved per pixel of vertical drag — same scale-per-field idea
// SliceControlBar::mouseDrag already used for the old SCB zone cells (see
// its ZonePan/ZoneVolume/etc. branches), carried over so drag feel doesn't
// change for anyone used to the old bar.
float MultisamplerZoneLcd::dragScaleFor (MultisamplerZoneField field, bool fineMode) const
{
    float scale = 1.0f;
    switch (field)
    {
        case MultisamplerZoneField::lowKey:
        case MultisamplerZoneField::highKey:
        case MultisamplerZoneField::rootKey:     scale = 1.0f;   break;
        case MultisamplerZoneField::group:       scale = 1.0f;   break;
        case MultisamplerZoneField::tune:        scale = 1.0f;   break;
        case MultisamplerZoneField::pan:         scale = 0.01f;  break;
        case MultisamplerZoneField::gain:        scale = 0.5f;   break;
        case MultisamplerZoneField::attack:      scale = 0.01f;  break;
        case MultisamplerZoneField::decay:       scale = 0.05f;  break;
        case MultisamplerZoneField::sustain:     scale = 0.01f;  break;
        case MultisamplerZoneField::release:     scale = 0.01f;  break;
        case MultisamplerZoneField::cutoff:      scale = 50.0f;  break;
        case MultisamplerZoneField::resonance:   scale = 0.01f;  break;
        case MultisamplerZoneField::loopEnabled: scale = 0.0f;   break; // toggle, not a drag
    }
    return fineMode ? scale * kFineModeScale : scale;
}

juce::String MultisamplerZoneLcd::formatFieldValue (MultisamplerZoneField field) const
{
    const auto note = [] (int n) { return UIHelpers::midiNoteToName (juce::jlimit (0, 127, n)); };
    switch (field)
    {
        case MultisamplerZoneField::lowKey:    return note (snapshot.lowKey);
        case MultisamplerZoneField::highKey:   return note (snapshot.highKey);
        case MultisamplerZoneField::rootKey:   return note (snapshot.rootKey);
        case MultisamplerZoneField::group:     return juce::String (snapshot.group);
        case MultisamplerZoneField::tune:
        {
            const int cents = juce::roundToInt (snapshot.tuneCents);
            return (cents >= 0 ? "+" : "") + juce::String (cents) + "ct";
        }
        case MultisamplerZoneField::pan:
            if (snapshot.pan == 0.0f) return "C";
            return (snapshot.pan < 0.0f ? "L" : "R") + juce::String (juce::roundToInt (std::abs (snapshot.pan) * 100.0f));
        case MultisamplerZoneField::gain:      return juce::String (snapshot.gainDb, 1) + "dB";
        case MultisamplerZoneField::loopEnabled: return snapshot.loopOn ? "ON" : "OFF";
        case MultisamplerZoneField::attack:    return juce::String (snapshot.attackSeconds, 3) + "s";
        case MultisamplerZoneField::decay:     return juce::String (snapshot.decaySeconds, 3) + "s";
        case MultisamplerZoneField::sustain:   return juce::String (juce::roundToInt (snapshot.sustainLevel * 100.0f)) + "%";
        case MultisamplerZoneField::release:   return juce::String (snapshot.releaseSeconds, 3) + "s";
        case MultisamplerZoneField::cutoff:
            return snapshot.filterCutoffHz >= 1000.0f
                       ? juce::String (snapshot.filterCutoffHz / 1000.0f, 1) + "kHz"
                       : juce::String (snapshot.filterCutoffHz, 0) + "Hz";
        case MultisamplerZoneField::resonance: return juce::String (snapshot.filterResonance, 2);
    }
    return {};
}

// ── Layout / paint ───────────────────────────────────────────────────────

void MultisamplerZoneLcd::resized()
{
    // Cell rects are (re)built in paint() against the current bounds, same
    // pattern SliceControlBar uses for its own ParamCell/SfzZoneCell hit
    // areas — resized() only needs to trigger a repaint.
    repaint();
}

void MultisamplerZoneLcd::paint (juce::Graphics& g)
{
    const auto& theme = getTheme();
    auto bounds = getLocalBounds();

    g.setColour (theme.darkBar.darker (0.15f));
    g.fillRoundedRectangle (bounds.toFloat(), 4.0f);
    g.setColour (theme.separator);
    g.drawRoundedRectangle (bounds.toFloat().reduced (0.5f), 4.0f, 1.0f);

    cells.clear();

    if (multipleSelected)
    {
        g.setColour (theme.foreground.withAlpha (0.55f));
        g.setFont (DysektLookAndFeel::makeFont (12.0f, false));
        g.drawText ("MULTIPLE ZONES SELECTED", bounds, juce::Justification::centred);
        return;
    }

    if (! snapshot.valid)
    {
        g.setColour (theme.foreground.withAlpha (0.55f));
        g.setFont (DysektLookAndFeel::makeFont (12.0f, false));
        g.drawText ("NO ZONE SELECTED", bounds, juce::Justification::centred);
        return;
    }

    auto content = bounds.reduced (8, 4);

    // Title row — name, index, preview/auditioning badges.
    auto titleRow = content.removeFromTop (18);
    g.setColour (theme.foreground);
    g.setFont (DysektLookAndFeel::makeFont (11.5f, true));
    juce::String title = "ZONE " + juce::String (snapshot.displayIndex + 1) + "   " + snapshot.name;
    g.drawText (title, titleRow, juce::Justification::centredLeft);

    if (snapshot.isPreview)
    {
        g.setColour (theme.accent.withAlpha (0.8f));
        g.setFont (DysektLookAndFeel::makeFont (10.0f, true));
        g.drawText ("PREVIEW", titleRow, juce::Justification::centredRight);
    }
    else if (snapshot.isAuditioning)
    {
        g.setColour (theme.accent);
        g.setFont (DysektLookAndFeel::makeFont (10.0f, true));
        g.drawText ("AUDITIONING", titleRow, juce::Justification::centredRight);
    }

    content.removeFromTop (2);

    // Three field rows, matching the plan's §5 suggested layout.
    const int rowH = content.getHeight() / 3;

    auto layoutRow = [&] (juce::Rectangle<int> row, std::initializer_list<MultisamplerZoneField> fields)
    {
        const int n = (int) fields.size();
        const int cellW = row.getWidth() / juce::jmax (1, n);
        int x = row.getX();
        for (auto f : fields)
        {
            juce::Rectangle<int> cellBounds (x, row.getY(), cellW, row.getHeight());
            drawCell (g, cellBounds, f);
            cells.push_back ({ cellBounds, f });
            x += cellW;
        }
    };

    layoutRow (content.removeFromTop (rowH),
               { MultisamplerZoneField::lowKey, MultisamplerZoneField::highKey,
                 MultisamplerZoneField::rootKey, MultisamplerZoneField::group });
    layoutRow (content.removeFromTop (rowH),
               { MultisamplerZoneField::tune, MultisamplerZoneField::pan,
                 MultisamplerZoneField::gain, MultisamplerZoneField::loopEnabled });
    layoutRow (content,
               { MultisamplerZoneField::attack, MultisamplerZoneField::decay,
                 MultisamplerZoneField::sustain, MultisamplerZoneField::release,
                 MultisamplerZoneField::cutoff, MultisamplerZoneField::resonance });
}

void MultisamplerZoneLcd::drawCell (juce::Graphics& g, juce::Rectangle<int> bounds, MultisamplerZoneField field)
{
    const auto& theme = getTheme();
    auto r = bounds.reduced (2, 1);

    // cells.size() at the moment this is called is exactly this cell's
    // eventual index in cells[], since drawCell() runs immediately before
    // its own push_back() in paint()'s layoutRow() — used to compare
    // against hoveredCellIdx below.
    const int idx = (int) cells.size();
    const bool hoveredNow = editable && (idx == hoveredCellIdx);
    const bool draggingNow = haveActiveDrag && activeField == field;

    if (draggingNow || hoveredNow)
    {
        g.setColour (draggingNow ? theme.accent.withAlpha (0.25f) : theme.accent.withAlpha (0.12f));
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
    }

    g.setColour (theme.foreground.withAlpha (editable ? 0.55f : 0.32f));
    g.setFont (DysektLookAndFeel::makeFont (8.5f, false));
    auto labelRow = r.removeFromTop (r.getHeight() / 2);
    g.drawText (labelFor (field), labelRow, juce::Justification::centredLeft);

    g.setColour (editable ? theme.foreground : theme.foreground.withAlpha (0.6f));
    g.setFont (DysektLookAndFeel::makeFont (11.0f, true));
    g.drawText (formatFieldValue (field), r, juce::Justification::centredLeft);
}

// ── Mouse / editing ──────────────────────────────────────────────────────

void MultisamplerZoneLcd::mouseMove (const juce::MouseEvent& e)
{
    int newHover = -1;
    for (int i = 0; i < (int) cells.size(); ++i)
        if (cells[(size_t) i].bounds.contains (e.getPosition())) { newHover = i; break; }

    if (newHover != hoveredCellIdx)
    {
        hoveredCellIdx = newHover;
        repaint();
    }
}

void MultisamplerZoneLcd::mouseExit (const juce::MouseEvent&)
{
    if (hoveredCellIdx != -1) { hoveredCellIdx = -1; repaint(); }
}

void MultisamplerZoneLcd::mouseDown (const juce::MouseEvent& e)
{
    if (! editable || ! snapshot.valid) return;

    for (const auto& cell : cells)
    {
        if (! cell.bounds.contains (e.getPosition())) continue;

        if (cell.field == MultisamplerZoneField::loopEnabled)
        {
            // Click toggles immediately — no drag gesture for a boolean.
            applyDrag (cell.field, snapshot.loopOn ? 0.0f : 1.0f, /*commit=*/true);
            return;
        }

        haveActiveDrag = true;
        dragMoved      = false;
        activeField    = cell.field;
        dragStartValue = getFieldValue (cell.field);
        dragStartY     = e.getScreenY();
        return;
    }
}

void MultisamplerZoneLcd::mouseDrag (const juce::MouseEvent& e)
{
    if (! haveActiveDrag || ! editable) return;

    const int deltaPixels = dragStartY - e.getScreenY();   // up = increase, matches SliceControlBar convention
    if (deltaPixels != 0) dragMoved = true;

    const bool fineMode = e.mods.isShiftDown();
    const float scale = dragScaleFor (activeField, fineMode);
    const float newValue = dragStartValue + (float) deltaPixels * scale;

    applyDrag (activeField, newValue, /*commit=*/false);
}

void MultisamplerZoneLcd::mouseUp (const juce::MouseEvent&)
{
    if (! haveActiveDrag) return;
    haveActiveDrag = false;

    // Commit once at gesture end — matches plan §6: dirty/undo/history
    // should land once per gesture, not once per mouse pixel. Only commit
    // if the value actually moved; a plain click with no drag shouldn't
    // register as an edit.
    if (dragMoved)
        applyDrag (activeField, getFieldValue (activeField), /*commit=*/true);

    repaint();
}

void MultisamplerZoneLcd::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! editable || ! snapshot.valid) return;

    for (const auto& cell : cells)
    {
        if (! cell.bounds.contains (e.getPosition())) continue;
        applyDrag (cell.field, defaultValueFor (cell.field), /*commit=*/true);
        return;
    }
}

void MultisamplerZoneLcd::applyDrag (MultisamplerZoneField field, float rawValue, bool commit)
{
    // Update the local snapshot immediately so the LCD repaints live during
    // a drag, exactly as plan §6 asks — but this is a *display* update
    // only. MultisamplerEditor::applyZoneFieldEdit is still the sole place
    // that clamps and writes the authoritative model value; if this
    // component's clamp-free display value differs from what gets clamped
    // upstream, the next setZoneForDisplay() call (fired via
    // onZoneSelectionOrEditChanged after the model write) overwrites it
    // with the true value, so there's no lasting drift.
    switch (field)
    {
        case MultisamplerZoneField::lowKey:      snapshot.lowKey  = juce::roundToInt (rawValue); break;
        case MultisamplerZoneField::highKey:     snapshot.highKey = juce::roundToInt (rawValue); break;
        case MultisamplerZoneField::rootKey:     snapshot.rootKey = juce::roundToInt (rawValue); break;
        case MultisamplerZoneField::group:       snapshot.group   = juce::roundToInt (rawValue); break;
        case MultisamplerZoneField::tune:        snapshot.tuneCents = rawValue; break;
        case MultisamplerZoneField::pan:         snapshot.pan = rawValue; break;
        case MultisamplerZoneField::gain:        snapshot.gainDb = rawValue; break;
        case MultisamplerZoneField::loopEnabled: snapshot.loopOn = rawValue > 0.5f; break;
        case MultisamplerZoneField::attack:      snapshot.attackSeconds = rawValue; break;
        case MultisamplerZoneField::decay:       snapshot.decaySeconds = rawValue; break;
        case MultisamplerZoneField::sustain:     snapshot.sustainLevel = rawValue; break;
        case MultisamplerZoneField::release:     snapshot.releaseSeconds = rawValue; break;
        case MultisamplerZoneField::cutoff:      snapshot.filterCutoffHz = rawValue; break;
        case MultisamplerZoneField::resonance:   snapshot.filterResonance = rawValue; break;
    }

    repaint();
    if (onFieldEdited) onFieldEdited (field, rawValue, commit);
}
