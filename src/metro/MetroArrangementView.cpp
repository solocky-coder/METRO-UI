#include "MetroArrangementView.h"
#include "MetroTheme.h"
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
    return juce::jmax (4, static_cast<int> (MetroTheme::Metrics::timelineBeatWidth * zoom));
}

juce::Rectangle<int> MetroArrangementView::trackRowBounds (int trackIndex) const
{
    const auto y = MetroTheme::Metrics::grid * 3 + trackIndex * MetroTheme::Metrics::trackHeight;
    return { 0, y, getWidth(), MetroTheme::Metrics::trackHeight };
}

juce::Rectangle<int> MetroArrangementView::clipBounds (int trackIndex, int clipIndex, int rowY) const
{
    const auto clip = engine.getClipInfo (trackIndex, clipIndex);
    const auto beatWidth = beatWidthPx();
    const auto clipX = static_cast<int> (clip.startTick / 960) * beatWidth - static_cast<int> (scrollPixels);
    const auto clipWidth = juce::jmax (beatWidth, static_cast<int> (clip.lengthTicks / 960) * beatWidth);
    return { clipX, rowY + MetroTheme::Metrics::grid, clipWidth, MetroTheme::Metrics::trackHeight - MetroTheme::Metrics::grid * 2 };
}

void MetroArrangementView::paint (juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds();
    const auto beatWidth = beatWidthPx();
    graphics.fillAll (MetroTheme::Colours::windowBackground);
    graphics.setFont (MetroTheme::smallFont());

    const auto scrollOffset = static_cast<int> (scrollPixels);
    const auto firstBeat = scrollOffset / beatWidth;
    const auto startX = firstBeat * beatWidth - scrollOffset;

    for (int x = startX, beat = firstBeat; x < bounds.getWidth(); x += beatWidth, ++beat)
    {
        graphics.setColour (MetroTheme::Colours::separator.withAlpha (0.55f));
        graphics.drawVerticalLine (x, 0.0f, static_cast<float> (bounds.getHeight()));
        graphics.setColour (MetroTheme::Colours::textDisabled);
        graphics.drawText (juce::String (beat + 1), x + MetroTheme::Metrics::grid, 0,
                           beatWidth, MetroTheme::Metrics::grid * 3,
                           juce::Justification::centredLeft);
    }

    for (int index = 0; index < engine.getNumTracks(); ++index)
    {
        auto row = trackRowBounds (index);
        const auto track = engine.getTrackInfo (index);
        const bool trackSelected = selection.isTrack() && selection.trackIndex == index;

        graphics.setColour (trackSelected ? MetroTheme::Colours::raisedPanel
                                           : MetroTheme::Colours::panelBackground.withAlpha (0.75f));
        graphics.fillRect (row);
        graphics.setColour (MetroTheme::Colours::separator);
        graphics.drawHorizontalLine (row.getBottom() - 1, 0.0f, static_cast<float> (bounds.getWidth()));
        graphics.setColour (track.colour.brighter (0.2f));
        graphics.fillRect (row.removeFromLeft (MetroTheme::Metrics::grid / 2));
        if (trackSelected)
        {
            graphics.setColour (MetroTheme::Colours::focusRing);
            graphics.drawRect (trackRowBounds (index), 1);
        }
        graphics.setColour (MetroTheme::Colours::textPrimary);
        graphics.drawText (track.name, MetroTheme::Metrics::grid * 2, row.getY(),
                           MetroTheme::Metrics::grid * 18, MetroTheme::Metrics::trackHeight,
                           juce::Justification::centredLeft);

        for (int clipIndex = 0; clipIndex < engine.getNumClips (index); ++clipIndex)
        {
            const auto clipRect = clipBounds (index, clipIndex, row.getY());
            const bool clipSelected = selection.isClip() && selection.trackIndex == index && selection.clipIndex == clipIndex;

            graphics.setColour (track.colour.withAlpha (clipSelected ? 0.95f : 0.78f));
            graphics.fillRect (clipRect);
            if (clipSelected)
            {
                graphics.setColour (MetroTheme::Colours::focusRing);
                graphics.drawRect (clipRect, 2);
            }
            graphics.setColour (MetroTheme::Colours::textPrimary);
            graphics.drawText ("Clip " + juce::String (clipIndex + 1),
                               clipRect.reduced (MetroTheme::Metrics::grid),
                               juce::Justification::centredLeft, true);
        }
    }

    graphics.setColour (MetroTheme::Colours::accent);
    graphics.drawVerticalLine (static_cast<int> (engine.getPlayheadBeats()) * beatWidth - scrollOffset,
                               0.0f, static_cast<float> (bounds.getHeight()));
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
    setScrollPosition (scrollPixels - wheel.deltaY * MetroTheme::Metrics::timelineBeatWidth * 4.0f);
}

void MetroArrangementView::timerCallback()
{
    repaint();
}

} // namespace dysekt::metro
