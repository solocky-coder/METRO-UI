#pragma once
// =============================================================================
//  ZoneMapView.h — 2D key/velocity map for MultisamplerInstrument
//  ─────────────────────────────────────────────────────────────────────────
//  Plan §6 "Mapping Editor / Zone map":
//    - Horizontal axis: MIDI key 0-127
//    - Vertical axis:   velocity 1-127 (127 at top, 1 at bottom — matches
//      how velocity is usually drawn in DAW piano rolls)
//    - One rectangle per zone, drag to move, drag an edge to resize
//    - Shift-click for multi-selection
//    - Visual indication of overlaps and missing samples
//
//  This view is UI-thread-only and never touches audio. It edits the
//  MultisamplerInstrument it's pointed at directly (matching the plan's
//  "native model is the source of truth" decision in §3) and reports edits
//  back via callbacks so the owning panel can debounce a playback-engine
//  resync (see MultisamplerEditor / the plan's §5 "Playback synchronization").
//
//  Does NOT own the instrument — call setInstrument() whenever the owning
//  panel swaps in a different one (new instrument, import, etc.) and
//  refresh() whenever the model changes from elsewhere (e.g. the inspector).
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include "../../audio/multisampler/SampleZone.h"
#include <functional>
#include <optional>

struct MultisamplerInstrument;
class DysektProcessor;

class ZoneMapView : public juce::Component,
                     private juce::Timer
{
public:
    explicit ZoneMapView (DysektProcessor& processorToUse);
    ~ZoneMapView() override;

    /** Non-owning. Pass nullptr to show an empty grid. */
    void setInstrument (MultisamplerInstrument* instrumentToShow);

    /** Call after any external edit to the instrument (inspector field
        change, import, undo) so the view re-derives its cached layout. */
    void refresh();

    /** Currently selected zone ids, in no particular order. Empty means
        nothing selected. */
    const std::vector<juce::Uuid>& getSelectedZoneIds() const noexcept { return selectedIds; }
    void setSelectedZoneIds (std::vector<juce::Uuid> ids);

    /** Among `ids`, returns whichever one sits highest in this view's
        z-order (i.e. drawn last/on top — same convention topmostZoneAt()/
        zonesAt() already use for hit-testing, including any frontZoneId
        promotion from "Edit Layer"). Returns juce::Uuid::null() if `ids`
        is empty or none of them resolve to a rect currently in
        cachedRects. Used by MultisamplerEditor::refreshInspectorFromSelection()
        to auto-elect a zone to show/edit when 2+ zones are selected at
        once, instead of a dead-end "multiple selected" placeholder. */
    juce::Uuid topmostZoneAmong (const std::vector<juce::Uuid>& ids) const;

    /** Makes a visually-obscured layer the topmost map tile and selects it
        for the shared zone inspector. This changes display/editing order only;
        the instrument's SFZ region order and playback behaviour stay intact.
        Public so a toolbar-level "Edit Layer" control (MultisamplerEditor)
        can promote a layer directly, without needing a click position —
        see showZoneContextMenu()'s "Edit Layer" submenu for the original,
        click-driven route into this same call. No-op if `zoneId` doesn't
        resolve to a zone in the current instrument. */
    void bringZoneToFrontForEditing (const juce::Uuid& zoneId);

    /** Fired whenever the selection set changes as a result of a click
        (not when set programmatically via setSelectedZoneIds). */
    std::function<void()> onSelectionChanged;

    /** Fired continuously while a drag is in progress (move or resize), once
        per mouseDrag callback, so a live audition/preview can follow along.
        The model is already updated by the time this fires. */
    std::function<void()> onZoneEditing;

    /** Fired once on mouseUp after a drag that actually changed something —
        this is the "commit" signal the debounced engine resync should key
        off, as opposed to onZoneEditing's every-frame updates. */
    std::function<void()> onZoneEditCommitted;

    /** Fired after a zone (or the last of several selected zones) is deleted
        via the Delete-key/right-click-menu path below — same "commit"
        contract as onZoneEditCommitted (model is already updated by the
        time this fires), but kept separate so the owning panel can tell a
        deletion apart from a drag when it matters (e.g. status text). */
    std::function<void()> onZoneDeleted;

    /** Fired after "Repeat Zone" or "Paste Zone" (right-click menu below)
        adds a brand new zone to the instrument — same "commit" contract as
        onZoneEditCommitted (model already updated, selection already moved
        to the new zone) but kept separate so the owning panel can tell a
        zone being added apart from an in-place edit, the same way
        onZoneDeleted is kept separate for removals. */
    std::function<void()> onZoneAdded;

    /** Fired when "Trim Sample" (right-click menu below) is chosen for a
        zone. This view never opens the trim overlay itself — that's
        MultisamplerEditor's AddZoneTrimOverlay, which this class doesn't
        know about — so it just reports which zone the user asked to
        re-trim and lets the owning panel drive the rest (open the overlay
        seeded with that zone's current sampleFile/sampleStart/sampleEnd,
        then write the result back via refresh()). No-op if the owning
        panel doesn't wire this up. */
    std::function<void (juce::Uuid)> onTrimZoneRequested;

    /** Fired whenever the zone being shown for read-only hover/inspection
        purposes changes — either because the cursor moved to a new zone (or
        off the map entirely, passing juce::Uuid::null()) or because the
        wheel was used to cycle through a stack of overlapping zones under a
        stationary cursor (see mouseWheelMove()). Display/inspection only:
        never changes selectedIds, never touches playback. The owning panel
        should treat this as taking priority over the click-selection display
        while non-null, and fall back to its normal selection-based display
        when it goes back to null (cursor left the map). */
    std::function<void (juce::Uuid)> onZoneHovered;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;

    /** Scales every font size this component draws with, so zone labels and
        the velocity ruler keep pace with the rest of the app's host-window-
        driven scaling (see DysektEditor::resized()'s `sf`/`si()`) instead of
        staying pinned at design-time pixel sizes on a much larger window.
        1.0 = the design-time sizes this file was authored with.
        MultisamplerEditor forwards its own scale here — see
        MultisamplerEditor::setUiScale(). Geometry (grid/keyboard layout) is
        untouched — that already stretches proportionally via gridArea(); only
        text sizing needed this. */
    void setUiScale (float newScale) { uiScale = newScale; repaint(); }

private:
    float uiScale = 1.0f;

    enum class DragMode { none, move, resizeLeft, resizeRight, resizeTop, resizeBottom };

    struct ZoneRect
    {
        juce::Uuid id;
        juce::Rectangle<float> bounds;   // in component-local pixels
        juce::Colour colour;             // SfzZoneColours::zoneColour(), same index MultisamplerEditor::toKeyzones() uses
        bool missingSample = false;
        bool overlapping   = false;

        // Cached at rebuildLayout() time (from the matching SampleZone) so
        // paint() can draw an on-tile label without re-resolving the zone
        // by id every frame — same "cache the derived layout, don't re-walk
        // the model in paint" approach the rest of this class already uses.
        juce::String label;      // sample file name (sans extension), or "(no sample)"
        int lowKey  = 0, highKey  = 0;
        int lowVel  = 1, highVel  = 127;

        // 0 = Main (no badge drawn); 1-15 = Aux N — see SampleZone::outputBus.
        // Cached here for the same reason label/lowKey/etc. are: paint()
        // shouldn't re-resolve the zone by id every frame.
        int outputBus = 0;
    };

    // Grid geometry -----------------------------------------------------
    juce::Rectangle<int> gridArea() const;
    float xForKey (int key) const;
    float yForVelocity (int velocity) const;
    int   keyForX (float x) const;
    int   velocityForY (float y) const;

    void rebuildLayout();          // recomputes cachedRects from *instrument
    DragMode hitTestEdges (const ZoneRect&, juce::Point<float>) const;
    const ZoneRect* topmostZoneAt (juce::Point<float>) const;
    std::vector<const ZoneRect*> zonesAt (juce::Point<float>) const;

    /** Deletes every currently-selected zone (or, if `rightClickedId` isn't
        part of the current selection, just that one — matches how ZONES'
        right-click menu operates on "the row you clicked", not whatever was
        selected before). Clears selection, rebuilds layout, and fires
        onZoneDeleted. No-op if the instrument is null or nothing resolves. */
    void deleteZones (const juce::Uuid& rightClickedId);

    /** Right-click context menu. When several zones overlap at the click,
        an "Edit Layer" submenu lets the user bring any one to the visual
        front and select it for editing. Also includes Trim Sample, Repeat
        Zone, Copy Zone, Paste Zone, Delete Zone, and the standard 16-colour
        zone palette. */
    void showZoneContextMenu (const juce::Uuid& zoneId,
                              juce::Point<float> localPos,
                              juce::Point<int> screenPos);

    /** "Repeat Zone": thin wrapper around MultisamplerInstrument::
        duplicateZone() (adjacent copy, same mapping) that also moves
        selection to the new zone and fires onZoneAdded — the menu-driven
        counterpart to that model method, which existed but had no UI path
        to it before this menu item. */
    void repeatZone (const juce::Uuid& zoneId);

    /** "Paste Zone": inserts a new zone carrying every field of
        zoneClipboard except id (fresh) and mapping, which is instead
        recentred on `localPos` at the copied zone's own key/velocity span
        — pasting somewhere else on the grid is more useful than an exact
        overlapping duplicate, which "Repeat Zone" above already covers.
        No-op if zoneClipboard is empty (menu item is disabled in that
        case; this is just belt-and-suspenders). */
    void pasteZoneAt (juce::Point<float> localPos);

    // Set by "Copy Zone", read by "Paste Zone" — see showZoneContextMenu().
    // Deliberately a plain optional<SampleZone> rather than just a
    // juce::Uuid: the source zone can be deleted (or this view repointed
    // at a different instrument entirely) between copy and paste, and the
    // clipboard should still work, the same way a text clipboard doesn't
    // die when its source document closes.
    std::optional<SampleZone> zoneClipboard;

    MultisamplerInstrument* instrument = nullptr;
    std::vector<ZoneRect> cachedRects;   // instrument order, except the promoted edit layer is last/front
    std::vector<juce::Uuid> selectedIds;
    juce::Uuid frontZoneId = juce::Uuid::null();   // visual z-order override only; never reorders instrument->zones

    // ── Live MIDI highlighting ──────────────────────────────────────────
    // MULTISAMPLER always drives sfzPlayer2 (see MultisamplerEditor's engine
    // -sync comment), so the piano-key strip and currently-sounding zones
    // read processor.sfz2ActiveNotes the same way KeysPanel does when bound
    // to EngineSource::SfzPlayer2 — same atomics, same 30Hz poll, so a note
    // played through MULTISAMPLER lights up identically to how it would in
    // ZONES' keyboard.
    DysektProcessor& processor;
    void timerCallback() override;
    uint64_t activeNotesSnap[2] = { 0, 0 };
    bool isNoteActive (int note) const noexcept;
    void selectZonesForNewNotes (uint64_t newLo, uint64_t newHi);

    // Live drag state
    DragMode   dragMode = DragMode::none;
    juce::Uuid dragZoneId = juce::Uuid::null();
    juce::Point<int> dragStartMouse;
    int dragStartLowKey = 0, dragStartHighKey = 0;
    int dragStartLowVel = 0, dragStartHighVel = 0;
    bool dragChangedAnything = false;

    // The zone currently shown for hover/inspection purposes (drives the
    // paint() highlight, cursor shape, and onZoneHovered). Usually the
    // topmost zone under the cursor, but can differ from that when the user
    // has scrolled to cycle deeper into a stack — see mouseMove()'s
    // "stillInsideDisplayed" check and mouseWheelMove(). Display only; never
    // affects selectedIds or playback.
    juce::Uuid hoverZoneId = juce::Uuid::null();

    static constexpr int kEdgeGrabPx = 5;
    static constexpr int kKeyCellPx = 34;         // the black/white note cells themselves —
                                                   // was 18; grown by 16px to absorb the space
                                                   // freed when MultisamplerEditor's bottom
                                                   // status strip was removed in favour of the
                                                   // header zone-summary readout (see
                                                   // MultisamplerEditor::resized()'s doc comment)
    static constexpr int kOctaveLabelPx = 15;     // "C1"/"C2"... row underneath the cells
    static constexpr int kKeyboardStripPx = kKeyCellPx + kOctaveLabelPx;
    static constexpr int kVelocityRulerPx = 28;   // velocity scale along the left

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ZoneMapView)
};
