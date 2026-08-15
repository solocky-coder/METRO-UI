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
#include <functional>

struct MultisamplerInstrument;

class ZoneMapView : public juce::Component
{
public:
    ZoneMapView();

    /** Non-owning. Pass nullptr to show an empty grid. */
    void setInstrument (MultisamplerInstrument* instrumentToShow);

    /** Call after any external edit to the instrument (inspector field
        change, import, undo) so the view re-derives its cached layout. */
    void refresh();

    /** Currently selected zone ids, in no particular order. Empty means
        nothing selected. */
    const std::vector<juce::Uuid>& getSelectedZoneIds() const noexcept { return selectedIds; }
    void setSelectedZoneIds (std::vector<juce::Uuid> ids);

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

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

private:
    enum class DragMode { none, move, resizeLeft, resizeRight, resizeTop, resizeBottom };

    struct ZoneRect
    {
        juce::Uuid id;
        juce::Rectangle<float> bounds;   // in component-local pixels
        bool missingSample = false;
        bool overlapping   = false;
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

    MultisamplerInstrument* instrument = nullptr;
    std::vector<ZoneRect> cachedRects;   // one per instrument->zones entry, same order
    std::vector<juce::Uuid> selectedIds;

    // Live drag state
    DragMode   dragMode = DragMode::none;
    juce::Uuid dragZoneId = juce::Uuid::null();
    juce::Point<int> dragStartMouse;
    int dragStartLowKey = 0, dragStartHighKey = 0;
    int dragStartLowVel = 0, dragStartHighVel = 0;
    bool dragChangedAnything = false;

    juce::Uuid hoverZoneId = juce::Uuid::null();   // for cursor feedback only

    static constexpr int kEdgeGrabPx = 5;
    static constexpr int kKeyboardStripPx = 18;   // piano-key strip along the bottom
    static constexpr int kVelocityRulerPx = 28;   // velocity scale along the left

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ZoneMapView)
};
