#include "ZoneMapView.h"
#include "../../audio/multisampler/MultisamplerInstrument.h"
#include "../../audio/SfzZoneColours.h"
#include "../../PluginProcessor.h"
#include "../DysektLookAndFeel.h"
#include <algorithm>
#include <cmath>

ZoneMapView::ZoneMapView (DysektProcessor& processorToUse)
    : processor (processorToUse)
{
    setWantsKeyboardFocus (false);
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
        activeNotesSnap[0] = lo;
        activeNotesSnap[1] = hi;
        repaint();
    }
}

// ── Public API ───────────────────────────────────────────────────────────

void ZoneMapView::setInstrument (MultisamplerInstrument* instrumentToShow)
{
    instrument = instrumentToShow;
    selectedIds.clear();
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

        // Same palette AND same indexing rule (disabled zones don't consume
        // a colour slot) as MultisamplerEditor::toKeyzones() / SfzExporter,
        // so a zone's colour here always matches its colour on the live
        // keyboard highlight and in a re-imported ZONES view of the same
        // file — see toKeyzones()'s header comment.
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
}

// ── Hit testing ──────────────────────────────────────────────────────────

const ZoneMapView::ZoneRect* ZoneMapView::topmostZoneAt (juce::Point<float> p) const
{
    // Later zones are drawn on top (see MultisamplerInstrument::zonesMatching
    // doc comment), so search back-to-front.
    for (auto it = cachedRects.rbegin(); it != cachedRects.rend(); ++it)
        if (it->bounds.contains (p))
            return &(*it);
    return nullptr;
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
    for (int key = 0; key < 128; ++key)
    {
        const float x0 = xForKey (key);
        const float x1 = xForKey (key + 1);
        const bool active = isNoteActive (key);
        g.setColour (active
                        ? theme.accent
                        : (blackKey[key % 12] ? theme.darkBar : theme.foreground.withAlpha (0.15f)));
        g.fillRect (juce::Rectangle<float> (x0, (float) g_area.getBottom(), x1 - x0, (float) kKeyboardStripPx));
    }
    g.setColour (theme.separator);
    g.drawHorizontalLine (g_area.getBottom(), (float) g_area.getX(), (float) g_area.getRight());

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

        auto fill = r.missingSample ? juce::Colours::red.withAlpha (0.25f)
                                     : r.colour.withAlpha (sounding ? 0.80f : (selected ? 0.55f : (hovered ? 0.38f : 0.24f)));
        g.setColour (fill);
        g.fillRect (r.bounds);

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
