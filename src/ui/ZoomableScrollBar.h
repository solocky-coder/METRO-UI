/*
    DYSEKT 2

    ZoomableScrollBar.h

    A juce::ScrollBar that can also zoom: dragging within a few pixels of
    either end of the thumb changes a caller-supplied "scale" value instead
    of panning, anchored on the *opposite* edge of the currently-visible
    range — the same feel as dragging a DAW timeline's scrollbar handles to
    zoom in/out around a fixed point. Click-and-drag anywhere else on the
    thumb, or on the track, behaves exactly like a normal juce::ScrollBar
    (unchanged — falls straight through to the base class) since only the
    code inside the handle's edge zone is new.

    Replaces ArrangeView's old "ARRANGEMENT OVERVIEW" minimap strip, which
    only offered click-to-seek and took up a dedicated row; zoom now lives
    directly on the scrollbars that were already there.

    Usage — give it callbacks rather than owning any zoom state itself,
    since the actual "scale" (pixelsPerTick for the horizontal bar, trackH
    for the vertical one) and the recompute-and-clamp/re-anchor logic belong
    to the owner:

        hScroll.getScale = [this] { return pixelsPerTick; };
        hScroll.minScale = 0.003; hScroll.maxScale = 6.0;
        hScroll.applyZoom = [this] (bool draggingStartEdge, double newScale)
        {
            ... set pixelsPerTick = newScale, reposition scrollX so the
                fixed edge stays put, updateScrollRanges(), repaint() ...
        };

    See ArrangeView's constructor for the concrete wiring used there.
*/
#pragma once

#include <cmath>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>


class ZoomableScrollBar : public juce::ScrollBar
{
public:
    explicit ZoomableScrollBar (bool isVerticalBar) : juce::ScrollBar (isVerticalBar) {}

    /** Returns the current "scale" value (pixelsPerTick / trackH / whatever
     *  the owner's zoom unit is) to zoom from. Must be set for edge-zoom to
     *  be active at all — if either this or applyZoom is unset, the bar
     *  behaves as a plain juce::ScrollBar. */
    std::function<double()> getScale;

    /** Called continuously during an edge-drag with the new scale (already
     *  clamped to [minScale, maxScale]) and which edge is being dragged
     *  (true = the START edge — the low-value end of the thumb; false = the
     *  END edge). The callback owns applying the new scale AND repositioning
     *  the pan offset so the *opposite* edge of the visible range stays
     *  fixed on screen, then repainting. */
    std::function<void (bool draggingStartEdge, double newScale)> applyZoom;

    double minScale = 0.01, maxScale = 10.0;

    /** Sensitivity: scrollbar-local pixels of drag per octave (doubling or
     *  halving) of scale. Smaller = touchier. */
    double dragPixelsPerOctave = 120.0;

private:
    static constexpr int kEdgeGrabPx = 6;

    enum class Edge { None, Start, End };

    Edge edgeAt (juce::Point<int> p) const
    {
        if (! (getScale && applyZoom))
            return Edge::None;

        const int full = isVertical() ? getHeight() : getWidth();
        const auto limit = getRangeLimit();
        if (limit.getLength() <= 0.0 || full <= 0)
            return Edge::None;

        const auto cur = getCurrentRange();
        const auto toPx = [&] (double v) { return (int) ((v - limit.getStart()) / limit.getLength() * (double) full); };
        const int thumbStart = toPx (cur.getStart());
        const int thumbEnd   = toPx (cur.getEnd());
        const int axis = isVertical() ? p.y : p.x;

        if (std::abs (axis - thumbStart) <= kEdgeGrabPx) return Edge::Start;
        if (std::abs (axis - thumbEnd)   <= kEdgeGrabPx) return Edge::End;
        return Edge::None;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto edge = edgeAt (e.getPosition());
        setMouseCursor (edge == Edge::None
                             ? juce::MouseCursor::NormalCursor
                             : (isVertical() ? juce::MouseCursor::UpDownResizeCursor
                                              : juce::MouseCursor::LeftRightResizeCursor));
        juce::ScrollBar::mouseMove (e);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragEdge = edgeAt (e.getPosition());
        if (dragEdge == Edge::None) { juce::ScrollBar::mouseDown (e); return; }

        dragStartAxis  = isVertical() ? e.y : e.x;
        dragStartScale = getScale();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragEdge == Edge::None) { juce::ScrollBar::mouseDrag (e); return; }

        const int axis = isVertical() ? e.y : e.x;
        const double deltaAxis = (double) (axis - dragStartAxis);

        // Dragging the END edge toward Start shrinks the visible range (zoom
        // in); dragging the START edge toward End also zooms in — so the
        // exponent's sign flips between the two edges.
        const double sign  = (dragEdge == Edge::End) ? -1.0 : 1.0;
        const double factor = std::pow (2.0, sign * deltaAxis / dragPixelsPerOctave);
        const double newScale = juce::jlimit (minScale, maxScale, dragStartScale * factor);

        applyZoom (dragEdge == Edge::Start, newScale);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragEdge == Edge::None) { juce::ScrollBar::mouseUp (e); return; }
        dragEdge = Edge::None;
    }

    Edge   dragEdge       = Edge::None;
    int    dragStartAxis  = 0;
    double dragStartScale = 1.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ZoomableScrollBar)
};
