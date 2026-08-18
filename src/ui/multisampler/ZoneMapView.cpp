#include "ZoneMapView.h"
#include "../../audio/multisampler/MultisamplerInstrument.h"
#include "../../audio/SfzZoneColours.h"
#include "../../PluginProcessor.h"
#include "../DysektLookAndFeel.h"
#include "../UIHelpers.h"
#include <algorithm>
#include <cmath>
#include <iterator>

ZoneMapView::ZoneMapView (DysektProcessor& processorToUse)
    : processor (processorToUse)
{
    // Needs focus now so Delete/Backspace (see keyPressed) can reach this
    // component — previously false because nothing here consumed key
    // events at all.
    setWantsKeyboardFocus (true);
    startTimerHz (30);   // matches KeysPanel's poll rate for the same atomics
}

ZoneMapView::~ZoneMapView()
{
    stopTimer();
}

bool ZoneMapView::isNoteActive (int note) const noexcept
{
    if (note < 0 || note > 127) return false;
    const uint64_t word = (note < 64) ? activeNotesSnap[0] : activeNotesSnap[1];
    const int      bit  = (note < 64) ? note : (note - 64);
    return ((word >> bit) & 1) != 0;
}

void ZoneMapView::timerCallback()
{
    // Same bitmask MultisamplerEditor's engine-sync path plays through
    // (sfzPlayer2) — see this class's header comment. Torn reads are fine,
    // same as KeysPanel's identical use of these atomics: display-only.
    const uint64_t lo = processor.sfz2ActiveNotes[0].load (std::memory_order_relaxed);
    const uint64_t hi = processor.sfz2ActiveNotes[1].load (std::memory_order_relaxed);
    if (lo != activeNotesSnap[0] || hi != activeNotesSnap[1])
    {
        // Newly-triggered notes only (rising edges), so a note-off in this
        // same poll window doesn't get treated as "new". Playing a note
        // should select its zone the way a click does — MIDI input into
        // the multisampler is auditioning a mapping, and the inspector
        // (SCB) should follow along without requiring a mouse click too.
        const uint64_t newLo = lo & ~activeNotesSnap[0];
        const uint64_t newHi = hi & ~activeNotesSnap[1];

        activeNotesSnap[0] = lo;
        activeNotesSnap[1] = hi;

        if ((newLo != 0 || newHi != 0) && instrument != nullptr)
            selectZonesForNewNotes (newLo, newHi);

        repaint();
    }
}

void ZoneMapView::selectZonesForNewNotes (uint64_t newLo, uint64_t newHi)
{
    // Mirrors the click-selection path in mouseDown(): replace the
    // selection with whatever's newly playing, then fire the same
    // onSelectionChanged callback MultisamplerEditor already wires up to
    // refreshInspectorFromSelection() — MIDI-driven selection should look
    // identical downstream to a click, not need a separate code path in
    // the editor.
    //
    // Matching on key range alone (as an earlier version of this did) was
    // wrong for any velocity-layered instrument — several zones commonly
    // share the same key range, split only by velocity, so a single note
    // would hit every layer at once, land on "MULTIPLE ZONES SELECTED",
    // and never populate the SCB readout (only a click, which picks one
    // rect via topmostZoneAt, ever did). Use SampleZone::matches(), the
    // same key+velocity test voicePool2's real playback effectively
    // resolves, so a played note selects the one zone that actually
    // sounded for it. A chord across genuinely different key/velocity
    // zones still multi-selects, same as shift-clicking each would.
    std::vector<juce::Uuid> hitIds;
    for (int note = 0; note < 128; ++note)
    {
        const uint64_t word = (note < 64) ? newLo : newHi;
        const int      bit  = (note < 64) ? note : (note - 64);
        if (((word >> bit) & 1) == 0) continue;

        const int velocity = (int) processor.sfz2LastNoteOnVelocity[note].load (std::memory_order_relaxed);

        for (const auto& z : instrument->zones)
            if (z.matches (note, velocity)
                && std::find (hitIds.begin(), hitIds.end(), z.id) == hitIds.end())
                hitIds.push_back (z.id);
    }

    if (hitIds.empty()) return;

    selectedIds = std::move (hitIds);
    if (onSelectionChanged) onSelectionChanged();
}

// ── Public API ───────────────────────────────────────────────────────────

void ZoneMapView::setInstrument (MultisamplerInstrument* instrumentToShow)
{
    instrument = instrumentToShow;
    selectedIds.clear();
    frontZoneId = juce::Uuid::null();
    dragMode = DragMode::none;
    rebuildLayout();
    repaint();
}

void ZoneMapView::refresh()
{
    rebuildLayout();
    repaint();
}

void ZoneMapView::setSelectedZoneIds (std::vector<juce::Uuid> ids)
{
    selectedIds = std::move (ids);
    repaint();
}

// ── Grid geometry ────────────────────────────────────────────────────────

juce::Rectangle<int> ZoneMapView::gridArea() const
{
    return getLocalBounds().withTrimmedLeft (kVelocityRulerPx)
                            .withTrimmedBottom (kKeyboardStripPx);
}

float ZoneMapView::xForKey (int key) const
{
    const auto g = gridArea();
    const float t = juce::jlimit (0.0f, 1.0f, (float) key / 127.0f);
    return (float) g.getX() + t * (float) g.getWidth();
}

float ZoneMapView::yForVelocity (int velocity) const
{
    const auto g = gridArea();
    // velocity 1..127, 127 drawn at the top (y == g.getY())
    const float t = juce::jlimit (0.0f, 1.0f, ((float) velocity - 1.0f) / 126.0f);
    return (float) g.getY() + (1.0f - t) * (float) g.getHeight();
}

int ZoneMapView::keyForX (float x) const
{
    const auto g = gridArea();
    if (g.getWidth() <= 0) return 0;
    const float t = juce::jlimit (0.0f, 1.0f, (x - (float) g.getX()) / (float) g.getWidth());
    return juce::jlimit (0, 127, (int) std::round (t * 127.0f));
}

int ZoneMapView::velocityForY (float y) const
{
    const auto g = gridArea();
    if (g.getHeight() <= 0) return 127;
    const float t = juce::jlimit (0.0f, 1.0f, (y - (float) g.getY()) / (float) g.getHeight());
    return juce::jlimit (1, 127, (int) std::round ((1.0f - t) * 126.0f + 1.0f));
}

void ZoneMapView::rebuildLayout()
{
    cachedRects.clear();
    if (instrument == nullptr) return;

    // Half a cell's width/height in each axis, derived from xForKey/
    // yForVelocity themselves rather than a separate denominator, so a
    // single-key or single-velocity zone still renders with visible
    // thickness and the top/bottom-most values (127 and 1) don't collapse
    // to a zero-size sliver at the grid edge.
    const float halfKeyW = std::abs (xForKey (1) - xForKey (0)) * 0.5f;
    const float halfVelH = std::abs (yForVelocity (1) - yForVelocity (2)) * 0.5f;

    const auto g = gridArea().toFloat();

    cachedRects.reserve (instrument->zones.size());
    int colIdx = 0;
    for (const auto& z : instrument->zones)
    {
        ZoneRect r;
        r.id = z.id;
        r.missingSample = z.hasMissingSample();
        r.label = z.sampleFile != juce::File() ? z.sampleFile.getFileNameWithoutExtension()
                                                 : juce::String ("(no sample)");
        r.lowKey = z.lowKey;  r.highKey = z.highKey;
        r.lowVel = z.lowVelocity; r.highVel = z.highVelocity;

        // A user-picked colour (see showZoneContextMenu) always wins over
        // the palette-index default, matching MultisamplerEditor::
        // toKeyzones()'s identical preference — otherwise a manually
        // recoloured zone would look "reset" the moment another zone above
        // it shifts every subsequent palette index.
        if (z.hasCustomColour)
            r.colour = juce::Colour (z.customColourArgb);
        else
            r.colour = z.enabled ? SfzZoneColours::zoneColour (colIdx) : getTheme().foreground.withAlpha (0.25f);
        if (z.enabled) ++colIdx;

        const float x0 = xForKey (z.lowKey)  - halfKeyW;
        const float x1 = xForKey (z.highKey) + halfKeyW;
        const float y0 = yForVelocity (z.highVelocity) - halfVelH;   // top (smaller y — higher velocity)
        const float y1 = yForVelocity (z.lowVelocity)  + halfVelH;   // bottom

        juce::Rectangle<float> bounds (x0, y0, juce::jmax (2.0f, x1 - x0), juce::jmax (2.0f, y1 - y0));
        r.bounds = bounds.getIntersection (g);
        if (r.bounds.isEmpty()) r.bounds = bounds;   // fully off-grid zone (shouldn't happen) — keep visible rather than vanish

        cachedRects.push_back (r);
    }

    for (const auto& pair : instrument->findOverlappingPairs())
    {
        if (pair.first  < cachedRects.size()) cachedRects[pair.first].overlapping  = true;
        if (pair.second < cachedRects.size()) cachedRects[pair.second].overlapping = true;
    }

    // Keep an explicitly chosen layer visually on top without changing the
    // instrument's region order (which could alter SFZ playback semantics).
    // paint() draws front-to-back and hit testing searches back-to-front, so
    // moving only this cached rectangle to the end handles both consistently.
    if (frontZoneId != juce::Uuid::null())
    {
        const auto promoted = std::find_if (cachedRects.begin(), cachedRects.end(),
                                            [this] (const ZoneRect& r) { return r.id == frontZoneId; });
        if (promoted != cachedRects.end())
            std::rotate (promoted, std::next (promoted), cachedRects.end());
        else
            frontZoneId = juce::Uuid::null();
    }
}

// ── Hit testing ──────────────────────────────────────────────────────────

std::vector<const ZoneMapView::ZoneRect*> ZoneMapView::zonesAt (juce::Point<float> p) const
{
    std::vector<const ZoneRect*> hits;
    hits.reserve (cachedRects.size());

    // Return visual front-to-back order. The promoted edit layer, if any, is
    // already last in cachedRects (see rebuildLayout()).
    for (auto it = cachedRects.rbegin(); it != cachedRects.rend(); ++it)
        if (it->bounds.contains (p))
            hits.push_back (&(*it));

    return hits;
}

const ZoneMapView::ZoneRect* ZoneMapView::topmostZoneAt (juce::Point<float> p) const
{
    // This is called for every mouse move, so keep the common path allocation-free.
    for (auto it = cachedRects.rbegin(); it != cachedRects.rend(); ++it)
        if (it->bounds.contains (p))
            return &(*it);
    return nullptr;
}

void ZoneMapView::bringZoneToFrontForEditing (const juce::Uuid& zoneId)
{
    if (instrument == nullptr || instrument->findZone (zoneId) == nullptr)
        return;

    frontZoneId = zoneId;
    selectedIds = { zoneId };
    dragMode = DragMode::none;
    rebuildLayout();
    repaint();

    // This is a selection/display-order change, not an instrument edit, so it
    // intentionally does not dirty or resync the playback engine.
    if (onSelectionChanged) onSelectionChanged();
}

ZoneMapView::DragMode ZoneMapView::hitTestEdges (const ZoneRect& r, juce::Point<float> p) const
{
    const bool nearLeft   = std::abs (p.x - r.bounds.getX())      <= (float) kEdgeGrabPx;
    const bool nearRight  = std::abs (p.x - r.bounds.getRight())  <= (float) kEdgeGrabPx;
    const bool nearTop    = std::abs (p.y - r.bounds.getY())      <= (float) kEdgeGrabPx;
    const bool nearBottom = std::abs (p.y - r.bounds.getBottom()) <= (float) kEdgeGrabPx;

    // Corners aren't independently draggable in this first pass — key range
    // and velocity range resize one axis at a time, which is enough for the
    // plan's Phase 2 acceptance criteria ("resize key or velocity boundaries").
    if (nearLeft)   return DragMode::resizeLeft;
    if (nearRight)  return DragMode::resizeRight;
    if (nearTop)    return DragMode::resizeTop;
    if (nearBottom) return DragMode::resizeBottom;
    return DragMode::move;
}

// ── Mouse handling ───────────────────────────────────────────────────────

void ZoneMapView::mouseDown (const juce::MouseEvent& e)
{
    if (instrument == nullptr) return;

    grabKeyboardFocus();   // so a subsequent Delete/Backspace (see keyPressed) reaches this component

    const auto p = e.position;
    const auto* hit = topmostZoneAt (p);

    if (hit == nullptr)
    {
        if (! e.mods.isShiftDown() && ! selectedIds.empty())
        {
            selectedIds.clear();
            if (onSelectionChanged) onSelectionChanged();
            repaint();
        }
        dragMode = DragMode::none;
        return;
    }

    if (e.mods.isRightButtonDown())
    {
        // Right-click always at least targets the clicked zone, same as
        // ZONES' onRowRightClicked — select it first (unless it's already
        // part of a multi-selection the user is about to bulk-delete) so
        // "Delete Zone" has an unambiguous target.
        if (std::find (selectedIds.begin(), selectedIds.end(), hit->id) == selectedIds.end())
        {
            selectedIds = { hit->id };
            if (onSelectionChanged) onSelectionChanged();
            repaint();
        }
        showZoneContextMenu (hit->id, e.position, e.getScreenPosition());
        dragMode = DragMode::none;
        return;
    }

    if (e.mods.isShiftDown())
    {
        const auto already = std::find (selectedIds.begin(), selectedIds.end(), hit->id);
        if (already != selectedIds.end()) selectedIds.erase (already);
        else                              selectedIds.push_back (hit->id);
    }
    else if (std::find (selectedIds.begin(), selectedIds.end(), hit->id) == selectedIds.end())
    {
        selectedIds = { hit->id };
    }
    if (onSelectionChanged) onSelectionChanged();

    auto* zone = instrument->findZone (hit->id);
    if (zone == nullptr) { repaint(); return; }

    dragMode          = hitTestEdges (*hit, p);
    dragZoneId         = hit->id;
    dragStartMouse      = e.getPosition();
    dragStartLowKey    = zone->lowKey;
    dragStartHighKey   = zone->highKey;
    dragStartLowVel    = zone->lowVelocity;
    dragStartHighVel   = zone->highVelocity;
    dragChangedAnything = false;

    repaint();
}

void ZoneMapView::mouseDrag (const juce::MouseEvent& e)
{
    if (instrument == nullptr || dragMode == DragMode::none) return;
    auto* zone = instrument->findZone (dragZoneId);
    if (zone == nullptr) return;

    const int dxKeys = keyForX ((float) e.getPosition().x) - keyForX ((float) dragStartMouse.x);
    const int dyVel  = velocityForY ((float) e.getPosition().y) - velocityForY ((float) dragStartMouse.y);

    switch (dragMode)
    {
        case DragMode::move:
        {
            const int span = dragStartHighKey - dragStartLowKey;
            int newLow  = juce::jlimit (0, 127 - span, dragStartLowKey + dxKeys);
            zone->lowKey  = newLow;
            zone->highKey = newLow + span;

            const int vspan = dragStartHighVel - dragStartLowVel;
            int newLowVel = juce::jlimit (1, 127 - vspan, dragStartLowVel + dyVel);
            zone->lowVelocity  = newLowVel;
            zone->highVelocity = newLowVel + vspan;
            break;
        }
        case DragMode::resizeLeft:
            zone->lowKey = juce::jlimit (0, zone->highKey, dragStartLowKey + dxKeys);
            break;
        case DragMode::resizeRight:
            zone->highKey = juce::jlimit (zone->lowKey, 127, dragStartHighKey + dxKeys);
            break;
        case DragMode::resizeTop:
            zone->highVelocity = juce::jlimit (zone->lowVelocity, 127, dragStartHighVel + dyVel);
            break;
        case DragMode::resizeBottom:
            zone->lowVelocity = juce::jlimit (1, zone->highVelocity, dragStartLowVel + dyVel);
            break;
        case DragMode::none:
        default:
            return;
    }

    dragChangedAnything = true;
    rebuildLayout();
    repaint();
    if (onZoneEditing) onZoneEditing();
}

void ZoneMapView::mouseUp (const juce::MouseEvent&)
{
    const bool shouldNotify = dragChangedAnything && dragMode != DragMode::none;
    dragMode = DragMode::none;
    dragChangedAnything = false;
    if (shouldNotify && onZoneEditCommitted) onZoneEditCommitted();
}

void ZoneMapView::mouseMove (const juce::MouseEvent& e)
{
    const auto* hit = topmostZoneAt (e.position);
    const auto newHover = hit != nullptr ? hit->id : juce::Uuid::null();
    if (newHover != hoverZoneId)
    {
        hoverZoneId = newHover;
        repaint();
    }

    if (hit != nullptr)
    {
        switch (hitTestEdges (*hit, e.position))
        {
            case DragMode::resizeLeft:
            case DragMode::resizeRight:  setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); break;
            case DragMode::resizeTop:
            case DragMode::resizeBottom: setMouseCursor (juce::MouseCursor::UpDownResizeCursor);    break;
            default:                     setMouseCursor (juce::MouseCursor::DraggingHandCursor);    break;
        }
    }
    else
    {
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }
}

void ZoneMapView::mouseExit (const juce::MouseEvent&)
{
    if (hoverZoneId != juce::Uuid::null())
    {
        hoverZoneId = juce::Uuid::null();
        repaint();
    }
    setMouseCursor (juce::MouseCursor::NormalCursor);
}

void ZoneMapView::resized()
{
    rebuildLayout();
}

// ── Deletion ─────────────────────────────────────────────────────────────

bool ZoneMapView::keyPressed (const juce::KeyPress& key)
{
    if (instrument == nullptr || selectedIds.empty())
        return false;

    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
    {
        // Delete-key path always targets the current selection as a whole
        // (there's no single "clicked" zone here), unlike the right-click
        // menu's single-target default — matches ZONES having no keyboard
        // shortcut for this at all today, so this is pure upside rather
        // than a behaviour change to match.
        deleteZones (selectedIds.front());
        return true;
    }
    return false;
}

void ZoneMapView::deleteZones (const juce::Uuid& rightClickedId)
{
    if (instrument == nullptr) return;

    // If the right-clicked/keyed zone is part of the current multi-
    // selection, delete the whole selection; otherwise it's a lone
    // right-click on a zone that isn't selected, so only that one goes.
    std::vector<juce::Uuid> toDelete;
    if (std::find (selectedIds.begin(), selectedIds.end(), rightClickedId) != selectedIds.end())
        toDelete = selectedIds;
    else
        toDelete = { rightClickedId };

    bool anyRemoved = false;
    for (const auto& id : toDelete)
        anyRemoved |= instrument->removeZone (id);

    if (! anyRemoved) return;

    selectedIds.clear();
    dragMode = DragMode::none;
    rebuildLayout();
    repaint();

    if (onSelectionChanged)  onSelectionChanged();
    if (onZoneDeleted)       onZoneDeleted();
}

// ── Context menu ─────────────────────────────────────────────────────────

void ZoneMapView::showZoneContextMenu (const juce::Uuid& zoneId,
                                       juce::Point<float> localPos,
                                       juce::Point<int> screenPos)
{
    if (instrument == nullptr) return;
    auto* zone = instrument->findZone (zoneId);
    if (zone == nullptr) return;

    // Copy ids and labels now: PopupMenu is asynchronous, while cachedRects
    // can be rebuilt by resize, MIDI selection, or another edit before its
    // callback runs. zonesAt() is front-to-back, matching what the user sees.
    std::vector<juce::Uuid> layerIds;
    juce::PopupMenu layerSub;
    const auto layerHits = zonesAt (localPos);
    layerIds.reserve (layerHits.size());
    for (size_t i = 0; i < layerHits.size(); ++i)
    {
        const auto& r = *layerHits[i];
        layerIds.push_back (r.id);
        const auto label = juce::String ((int) i + 1) + ". " + r.label
                         + "  [" + UIHelpers::midiNoteToName (r.lowKey)
                         + "-" + UIHelpers::midiNoteToName (r.highKey)
                         + ", v" + juce::String (r.lowVel)
                         + "-" + juce::String (r.highVel) + "]";
        layerSub.addItem (1000 + (int) i, label, true,
                          std::find (selectedIds.begin(), selectedIds.end(), r.id) != selectedIds.end());
    }

    // Same 16-colour named palette as ZONES' onRowRightClicked (see
    // PluginEditor.cpp) and the Slicer's "Slice Color" picker (SliceLane.cpp)
    // — kept identical so the colour-picker UX matches everywhere in the
    // app a zone/slice can be recoloured.
    static const struct { const char* name; juce::uint32 argb; } kPal[] = {
        { "Cyan",    0xFF00C8FF }, { "Green",   0xFF00FF87 },
        { "Yellow",  0xFFFFE800 }, { "Orange",  0xFFFF6B00 },
        { "Red",     0xFFFF2D55 }, { "Pink",    0xFFFF2D9A },
        { "Violet",  0xFFB44FFF }, { "Blue",    0xFF4A80FF },
        { "Sky",     0xFF00BFFF }, { "Mint",    0xFF00FFD0 },
        { "Lime",    0xFFA8FF3E }, { "Gold",    0xFFFFD700 },
        { "Coral",   0xFFFF7F50 }, { "Magenta", 0xFFFF00FF },
        { "White",   0xFFE8E8E8 }, { "Silver",  0xFF888888 },
    };

    const juce::Colour curCol = zone->hasCustomColour
                                     ? juce::Colour (zone->customColourArgb)
                                     : juce::Colours::transparentBlack;
    juce::PopupMenu colourSub;
    for (int ci = 0; ci < 16; ++ci)
    {
        juce::Colour c ((juce::uint32) kPal[ci].argb);
        colourSub.addColouredItem (20 + ci, kPal[ci].name, c,
                                   true, zone->hasCustomColour && c.toDisplayString (false) == curCol.toDisplayString (false));
    }

    auto* topLvl = getTopLevelComponent();
    float ms = DysektLookAndFeel::getMenuScale();
    juce::PopupMenu menu;
    const bool multi = std::find (selectedIds.begin(), selectedIds.end(), zoneId) != selectedIds.end()
                            && selectedIds.size() > 1;
    if (layerIds.size() > 1)
    {
        menu.addSubMenu ("Edit Layer (" + juce::String ((int) layerIds.size()) + ")", layerSub);
        menu.addSeparator();
    }
    menu.addItem (1, multi ? "Delete " + juce::String ((int) selectedIds.size()) + " Zones" : "Delete Zone");
    menu.addSeparator();
    menu.addSubMenu ("Zone Color", colourSub);
    menu.showMenuAsync (
        juce::PopupMenu::Options()
            .withTargetScreenArea (juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1))
            .withParentComponent (topLvl)
            .withStandardItemHeight ((int) (24 * ms)),
        [this, zoneId, layerIds] (int result)
        {
            if (instrument == nullptr) return;   // view may have been repointed while the menu was open

            if (result >= 1000 && result < 1000 + (int) layerIds.size())
            {
                bringZoneToFrontForEditing (layerIds[(size_t) (result - 1000)]);
            }
            else if (result == 1)
            {
                deleteZones (zoneId);
            }
            else if (result >= 20 && result < 36)
            {
                static const juce::uint32 kPalARGB[] = {
                    0xFF00C8FF, 0xFF00FF87, 0xFFFFE800, 0xFFFF6B00,
                    0xFFFF2D55, 0xFFFF2D9A, 0xFFB44FFF, 0xFF4A80FF,
                    0xFF00BFFF, 0xFF00FFD0, 0xFFA8FF3E, 0xFFFFD700,
                    0xFFFF7F50, 0xFFFF00FF, 0xFFE8E8E8, 0xFF888888,
                };
                auto* z = instrument->findZone (zoneId);
                if (z == nullptr) return;
                z->hasCustomColour  = true;
                z->customColourArgb = kPalARGB[result - 20];
                rebuildLayout();
                repaint();
                if (onZoneEditCommitted) onZoneEditCommitted();   // persists via the normal debounced-resync/dirty path
            }
        });
}

// ── Painting ─────────────────────────────────────────────────────────────

void ZoneMapView::paint (juce::Graphics& g)
{
    const auto& theme = getTheme();
    const auto g_area = gridArea();

    g.fillAll (theme.waveformBg);

    // Octave gridlines (C notes every 12 semitones) + black-key shading.
    g.setColour (theme.gridLine.withAlpha (0.35f));
    for (int key = 0; key <= 127; key += 12)
    {
        const float x = xForKey (key);
        g.drawVerticalLine ((int) x, (float) g_area.getY(), (float) g_area.getBottom());
    }

    // Velocity gridlines every 32.
    for (int v = 1; v <= 127; v += 32)
    {
        const float y = yForVelocity (v);
        g.drawHorizontalLine ((int) y, (float) g_area.getX(), (float) g_area.getRight());
    }

    // Velocity ruler.
    g.setColour (theme.foreground.withAlpha (0.5f));
    g.setFont (juce::FontOptions (10.0f));
    for (int v = 127; v >= 1; v -= 32)
        g.drawText (juce::String (v), 0, (int) yForVelocity (v) - 7, kVelocityRulerPx - 3, 14,
                    juce::Justification::centredRight);

    // Piano-key strip along the bottom (just black/white shading, no labels
    // beyond octave C markers — this is a mapping aid, not a keyboard widget).
    // Notes currently sounding through sfzPlayer2 (real MIDI input, on-screen
    // keyboard clicks elsewhere, or MULTISAMPLER's own preview — anything
    // that lights up processor.sfz2ActiveNotes) light up in the theme accent
    // colour, same as KeysPanel's keyboard does for ZONES.
    static const bool blackKey[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
    // Cells are centred on xForKey(key) using the same half-cell width the
    // zone rects and octave gridlines are built around (see rebuildLayout's
    // halfKeyW), not spanned across [xForKey(key), xForKey(key+1)] — the
    // latter silently shifts every highlighted key half a cell to the right
    // of the zone column it's supposed to sit under (and collapses key 127
    // to zero width, since xForKey(128) clamps to the same x as xForKey(127)).
    //
    // Clamped to g_area's left/right edges, same as rebuildLayout() clips
    // zone bounds to gridArea() via getIntersection() — without this, key 0
    // and key 127's half-cell overflow spills past the grid on both sides,
    // making the keyboard visibly wider than the zone tiles above it.
    const float halfKeyW = std::abs (xForKey (1) - xForKey (0)) * 0.5f;
    const float gridLeft  = (float) g_area.getX();
    const float gridRight = (float) g_area.getRight();
    for (int key = 0; key < 128; ++key)
    {
        const float x0 = juce::jmax (gridLeft,  xForKey (key) - halfKeyW);
        const float x1 = juce::jmin (gridRight, xForKey (key) + halfKeyW);
        const bool active = isNoteActive (key);
        // Explicit flat black/white rather than theme-derived tones — the
        // previous theme.darkBar / foreground-at-15%-alpha pairing reads as
        // two shades of the same dark grey against this app's dark theme,
        // not a recognisable black/white key strip.
        static const juce::Colour whiteKey (0xffe8e8e6);
        static const juce::Colour blackKeyColour (0xff1c1c1f);
        g.setColour (active ? theme.accent : (blackKey[key % 12] ? blackKeyColour : whiteKey));
        g.fillRect (juce::Rectangle<float> (x0, (float) g_area.getBottom(), x1 - x0, (float) kKeyCellPx));
    }
    g.setColour (theme.separator);
    g.drawHorizontalLine (g_area.getBottom(), (float) g_area.getX(), (float) g_area.getRight());

    // Octave labels ("C1", "C2"...) under the strip, at the same x as the
    // octave gridlines above — one glance ties a column all the way from
    // the zone grid down through the keyboard to a note name, instead of
    // counting keys from the nearest C. Larger and bolder than the first
    // pass, which was unreadable at 9.5px; and each label rectangle is
    // clamped inside g_area the same way the key cells are above, so the
    // row never spills past the grid's left/right edges either.
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    for (int key = 0; key <= 127; key += 12)
    {
        const float cx = xForKey (key);
        const bool active = isNoteActive (key);
        g.setColour (active ? theme.accent : theme.foreground.withAlpha (0.7f));
        const float labelX = juce::jlimit (gridLeft, gridRight - 30.0f, cx - 15.0f);
        g.drawText (UIHelpers::midiNoteToName (key),
                    juce::Rectangle<float> (labelX, (float) g_area.getBottom() + (float) kKeyCellPx,
                                             30.0f, (float) kOctaveLabelPx),
                    juce::Justification::centred);
    }

    // Zones.
    for (const auto& r : cachedRects)
    {
        const bool selected = std::find (selectedIds.begin(), selectedIds.end(), r.id) != selectedIds.end();
        const bool hovered   = r.id == hoverZoneId;

        const bool sounding = [&]
        {
            const auto* zone = instrument != nullptr ? instrument->findZone (r.id) : nullptr;
            if (zone == nullptr) return false;
            for (int n = 0; n < 128; ++n)
                if (isNoteActive (n) && zone->keyInRange (n)) return true;
            return false;
        }();

        // Flat metro tile: solid colour, not the previous washed-out alpha
        // wash — a resting zone should read as a real block, not a faint
        // tint over the grid. Selection/hover/sounding still lift it, but
        // off the same higher floor rather than starting near-transparent.
        auto fill = r.missingSample ? juce::Colours::red.withAlpha (0.25f)
                                     : r.colour.withAlpha (sounding ? 1.00f : (selected ? 0.85f : (hovered ? 0.70f : 0.55f)));
        g.setColour (fill);
        g.fillRect (r.bounds);

        // On-tile label: sample name + key/velocity range, same info the
        // ZONES list row shows, so a glance at the grid tells you what's
        // mapped without opening the inspector. Fixed light colour rather
        // than a darkened version of the zone's own hue — the tile fill is
        // that hue alpha-blended over a near-black background, so a
        // same-hue-darkened label ends up close to the same luminance as
        // the tile itself and disappears. White reads against every
        // palette colour regardless of fill alpha.
        if (r.bounds.getWidth() > 24.0f && r.bounds.getHeight() > 16.0f)
        {
            auto textArea = r.bounds.toNearestInt().reduced (4, 2);

            g.setColour (juce::Colours::white.withAlpha (0.92f));
            g.setFont (juce::FontOptions (11.0f, juce::Font::plain));
            g.drawText (r.label, textArea.removeFromTop (14), juce::Justification::topLeft, true);

            if (textArea.getHeight() > 10)
            {
                g.setColour (juce::Colours::white.withAlpha (0.68f));
                g.setFont (juce::FontOptions (9.5f));
                const auto rangeText = UIHelpers::midiNoteToName (r.lowKey) + "-" + UIHelpers::midiNoteToName (r.highKey)
                                        + "  v" + juce::String (r.lowVel) + "-" + juce::String (r.highVel);
                g.drawText (rangeText, textArea, juce::Justification::topLeft, true);
            }
        }

        if (r.overlapping)
        {
            // Diagonal hatch to flag overlapping key/velocity ranges (plan §6,
            // "visual indication of overlaps").
            g.saveState();
            g.reduceClipRegion (r.bounds.toNearestInt());
            g.setColour (theme.foreground.withAlpha (0.20f));
            for (float x = r.bounds.getX() - r.bounds.getHeight(); x < r.bounds.getRight(); x += 6.0f)
                g.drawLine (x, r.bounds.getBottom(), x + r.bounds.getHeight(), r.bounds.getY(), 1.0f);
            g.restoreState();
        }

        g.setColour (sounding ? juce::Colours::white : (selected ? theme.accent : theme.separator.withAlpha (0.8f)));
        g.drawRect (r.bounds, (sounding || selected) ? 2.0f : 1.0f);

        if (r.missingSample)
        {
            g.setColour (juce::Colours::red.withAlpha (0.85f));
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText ("!", r.bounds.toNearestInt(), juce::Justification::topLeft);
        }
    }

    if (instrument == nullptr || instrument->zones.empty())
    {
        g.setColour (theme.foreground.withAlpha (0.4f));
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Drop samples here or import an SFZ to start mapping",
                    g_area, juce::Justification::centred);
    }
}
