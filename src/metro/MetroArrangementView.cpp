#include "MetroArrangementView.h"
#include "MetroColours.h"
#include "MetroMetrics.h"
#include "MetroTypography.h"
#include "../sequencer/SequencerEngine.h"

namespace dysekt::metro
{
MetroArrangementView::MetroArrangementView (SequencerEngine& sequencer)
    : engine (sequencer)
{
    startTimerHz (15);
}

MetroArrangementView::~MetroArrangementView() = default;

void MetroArrangementView::setZoom (float newZoom)
{
    zoom = juce::jlimit (0.25f, 4.0f, newZoom);
    repaint();
}

void MetroArrangementView::setScrollPosition (float newScrollPixels)
{
    scrollPixels = juce::jmax (0.0f, newScrollPixels);
    repaint();
}

void MetroArrangementView::setSelectionChangedCallback (std::function<void (const MetroSelection&)> callback)
{
    onSelectionChanged = std::move (callback);
}

void MetroArrangementView::clearSelection()
{
    setSelection (MetroSelection::none());
}

void MetroArrangementView::setSelection (MetroSelection newSelection)
{
    selection = std::move (newSelection);
    if (onSelectionChanged != nullptr)
        onSelectionChanged (selection);
    repaint();
}

int MetroArrangementView::beatWidthPx() const noexcept
{
    return juce::jmax (4, static_cast<int> (MetroMetrics::timelineBeatWidth * zoom));
}

juce::Rectangle<int> MetroArrangementView::trackRowBounds (int trackIndex) const
{
    const auto y = MetroMetrics::grid * 3 + trackIndex * MetroMetrics::trackHeight;
    return { 0, y, getWidth(), MetroMetrics::trackHeight };
}

juce::Rectangle<int> MetroArrangementView::clipBounds (int trackIndex, int clipIndex, int rowY) const
{
    const auto clip = engine.getClipInfo (trackIndex, clipIndex);
    const auto beatWidth = beatWidthPx();
    const auto clipX = static_cast<int> (clip.startTick / 960) * beatWidth - static_cast<int> (scrollPixels);
    const auto clipWidth = juce::jmax (beatWidth, static_cast<int> (clip.lengthTicks / 960) * beatWidth);
    return { clipX, rowY + MetroMetrics::grid, clipWidth, MetroMetrics::trackHeight - MetroMetrics::grid * 2 };
}

void MetroArrangementView::paint (juce::Graphics& graphics)
{
    // The arrangement is deliberately a quiet, high-contrast editing surface:
    // charcoal grid, fine subdivision lines, and no coloured panels competing
    // with clips.  The ruler is the sole place time is labelled.
    const auto bounds = getLocalBounds();
    const int rulerH = MetroMetrics::timelineHeight;
    const int beatWidth = beatWidthPx();
    const int scrollOffset = static_cast<int> (scrollPixels);
    const int firstBeat = scrollOffset / beatWidth;
    const int startX = firstBeat * beatWidth - scrollOffset;
    const auto ruler = bounds.removeFromTop (rulerH).reduced (4, 0);
    const auto grid = bounds.reduced (4, 0);

    graphics.fillAll (juce::Colour (0xFF151617));

    // Timeline ruler — a restrained, flat strip with bar and beat positions.
    graphics.setColour (juce::Colour (0xFF101112));
    graphics.fillRect (ruler);
    graphics.setColour (juce::Colour (0xFF9CA1A5));
    graphics.drawRect (ruler, 1);
    graphics.setFont (MetroTypography::small());

    for (int x = startX, beat = firstBeat; x < grid.getRight(); x += beatWidth, ++beat)
    {
        if (x < grid.getX()) continue;
        const bool isBar = (beat % 4 == 0);
        graphics.setColour (isBar ? juce::Colour (0xFF575A5D)
                                  : juce::Colour (0xFF303235));
        graphics.drawVerticalLine (x, (float) ruler.getY(), (float) grid.getBottom());

        const int bar = beat / 4 + 1;
        const int beatInBar = beat % 4 + 1;
        const auto label = beatInBar == 1 ? juce::String (bar)
                                          : juce::String (bar) + "." + juce::String (beatInBar);
        graphics.setColour (juce::Colour (0xFFCACCCE));
        graphics.drawText (label, x + 4, ruler.getY() + 1, beatWidth - 6,
                           ruler.getHeight() - 3, juce::Justification::centredLeft, false);
    }

    // Fine horizontal rows give an empty arrangement the same useful visual
    // cadence as a DAW grid, before any clips or additional tracks exist.
    constexpr int horizontalStep = 9;
    graphics.setColour (juce::Colour (0xFF252729));
    for (int y = grid.getY(); y < grid.getBottom(); y += horizontalStep)
        graphics.drawHorizontalLine (y, (float) grid.getX(), (float) grid.getRight());

    // A stronger divider retains the real track boundaries beneath the fine
    // grid, which becomes important once several tracks have been added.
    for (int index = 0; index < engine.getNumTracks(); ++index)
    {
        const auto row = trackRowBounds (index);
        if (! row.intersects (grid)) continue;
        const bool selected = selection.isTrack() && selection.trackIndex == index;
        if (selected)
        {
            graphics.setColour (juce::Colour (0xFF252729));
            graphics.fillRect (row.getIntersection (grid));
        }
        graphics.setColour (juce::Colour (0xFF3A3C3F));
        graphics.drawHorizontalLine (row.getBottom() - 1, (float) grid.getX(), (float) grid.getRight());

        for (int clipIndex = 0; clipIndex < engine.getNumClips (index); ++clipIndex)
        {
            const auto clipRect = clipBounds (index, clipIndex, row.getY()).getIntersection (grid);
            const bool clipSelected = selection.isClip() && selection.trackIndex == index
                                                        && selection.clipIndex == clipIndex;
            const auto track = engine.getTrackInfo (index);
            graphics.setColour (track.colour.withAlpha (clipSelected ? 0.88f : 0.68f));
            graphics.fillRect (clipRect);
            graphics.setColour (juce::Colour (0xFFE4E6E7).withAlpha (clipSelected ? 0.8f : 0.36f));
            graphics.drawRect (clipRect, 1);
        }
    }

    // The playhead is intentionally neutral; colour is reserved for actual clips.
    const int playheadX = static_cast<int> (engine.getPlayheadBeats()) * beatWidth - scrollOffset;
    if (playheadX >= grid.getX() && playheadX <= grid.getRight())
    {
        graphics.setColour (juce::Colour (0xFFC3C6C8).withAlpha (0.75f));
        graphics.drawVerticalLine (playheadX, (float) ruler.getY(), (float) grid.getBottom());
    }
}
int MetroArrangementView::hitTestTrack (juce::Point<int> position, int& clipIndexOut) const
{
    clipIndexOut = -1;

    for (int index = 0; index < engine.getNumTracks(); ++index)
    {
        const auto row = trackRowBounds (index);
        if (! row.contains (position))
            continue;

        for (int clipIndex = 0; clipIndex < engine.getNumClips (index); ++clipIndex)
        {
            if (clipBounds (index, clipIndex, row.getY()).contains (position))
            {
                clipIndexOut = clipIndex;
                break;
            }
        }

        return index;
    }

    return -1;
}

void MetroArrangementView::mouseDown (const juce::MouseEvent& event)
{
    int clipIndex = -1;
    const auto trackIndex = hitTestTrack (event.getPosition(), clipIndex);

    if (trackIndex < 0)
    {
        clearSelection();
        return;
    }

    if (clipIndex >= 0)
        setSelection (MetroSelection::forClip (trackIndex, clipIndex,
                                               engine.getTrackInfo (trackIndex),
                                               engine.getClipInfo (trackIndex, clipIndex)));
    else
        setSelection (MetroSelection::forTrack (trackIndex, engine.getTrackInfo (trackIndex)));
}

void MetroArrangementView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (wheel.deltaY == 0.0f)
        return;

    // Zoom is not currently mapped to a modifier key check here (JUCE gives us the
    // event separately from mods on some platforms); callers can still drive
    // setZoom() directly (e.g. from a future zoom control). Wheel motion scrolls.
    setScrollPosition (scrollPixels - wheel.deltaY * MetroMetrics::timelineBeatWidth * 4.0f);
}

void MetroArrangementView::timerCallback()
{
    repaint();
}

} // namespace dysekt::metro
