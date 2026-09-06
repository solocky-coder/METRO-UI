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
    const auto y = kToolbarHeight + kRulerHeight + trackIndex * kTrackRowHeight;
    return { 0, y, getWidth(), kTrackRowHeight };
}

juce::Rectangle<int> MetroArrangementView::clipBounds (int trackIndex, int clipIndex, int rowY) const
{
    const auto clip = engine.getClipInfo (trackIndex, clipIndex);
    const auto beatWidth = beatWidthPx();
    const auto clipX = kTrackHeaderWidth + static_cast<int> (clip.startTick / 960.0) * beatWidth - static_cast<int> (scrollPixels);
    const auto clipWidth = juce::jmax (beatWidth, static_cast<int> (std::ceil ((double) clip.lengthTicks / 960.0 * beatWidth)));
    return { clipX, rowY + kClipInset, clipWidth, kTrackRowHeight - kClipInset * 2 };
}

void MetroArrangementView::drawToolbar (juce::Graphics& g, juce::Rectangle<int> area) const
{
    g.setColour (juce::Colour (0xFF070B0E));
    g.fillRect (area);
    g.setColour (juce::Colour (0xFF24323A));
    g.drawHorizontalLine (area.getBottom() - 1, (float) area.getX(), (float) area.getRight());

    auto button = [&g] (juce::Rectangle<int> r, const juce::String& label, bool active)
    {
        g.setColour (active ? juce::Colour (0xFF0EA7D6).withAlpha (0.20f)
                             : juce::Colour (0xFF101820));
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
        g.setColour (active ? juce::Colour (0xFF35D7FF)
                             : juce::Colour (0xFF33454F));
        g.drawRoundedRectangle (r.toFloat(), 3.0f, 1.0f);
        g.setColour (active ? juce::Colours::white : juce::Colour (0xFFB5C0C5));
        g.setFont (juce::Font (12.0f, juce::Font::bold));
        g.drawText (label, r.reduced (7, 0), juce::Justification::centred, false);
    };

    button ({ area.getX() + 8, area.getY() + 7, 88, 26 }, "+ TRACK", false);
    button ({ area.getX() + 102, area.getY() + 7, 78, 26 }, "+ CLIP", false);
    button ({ area.getX() + 192, area.getY() + 7, 90, 26 }, "SNAP  1/16", false);
    button ({ area.getX() + 290, area.getY() + 7, 58, 26 }, "GRID", true);
    button ({ area.getX() + 356, area.getY() + 7, 78, 26 }, "MAGNET", false);

    const char* tools[] = { "↖", "✎", "⌫", "✂", "▣" };
    int x = area.getX() + 446;
    for (int i = 0; i < 5; ++i)
    {
        button ({ x, area.getY() + 7, 38, 26 }, tools[i], activeTool == i);
        x += 44;
    }

    button ({ x + 4, area.getY() + 7, 58, 26 }, "LOCK", false);
    g.setColour (juce::Colour (0xFF6C7A82));
    g.setFont (juce::Font (11.0f));
    g.drawText ("ARRANGER", area.getRight() - 92, area.getY() + 1, 82, 16,
                juce::Justification::centredRight, false);
}

void MetroArrangementView::drawRuler (juce::Graphics& g, juce::Rectangle<int> area) const
{
    g.setColour (juce::Colour (0xFF0A1115));
    g.fillRect (area);
    g.setColour (juce::Colour (0xFF1D313A));
    g.drawHorizontalLine (area.getBottom() - 1, (float) area.getX(), (float) area.getRight());

    const int beatWidth = beatWidthPx();
    const int firstBeat = juce::jmax (0, (int) std::floor (scrollPixels / beatWidth));
    const int startX = kTrackHeaderWidth + firstBeat * beatWidth - (int) scrollPixels;

    g.setColour (juce::Colour (0xFF0C151A));
    g.fillRect (area.getX(), area.getY(), kTrackHeaderWidth, area.getHeight());
    g.setColour (juce::Colour (0xFF52636B));
    g.drawText ("TRACK", area.getX() + 12, area.getY() + 5, kTrackHeaderWidth - 24, 18,
                juce::Justification::centredLeft, false);

    for (int beat = firstBeat, x = startX; x < area.getRight(); ++beat, x += beatWidth)
    {
        if (x < kTrackHeaderWidth) continue;
        const bool bar = (beat % 4 == 0);
        g.setColour (bar ? juce::Colour (0xFF3B5863) : juce::Colour (0xFF1C3038));
        g.drawVerticalLine (x, (float) area.getY(), (float) getHeight());

        const int barNo = beat / 4 + 1;
        const int beatNo = beat % 4 + 1;
        g.setColour (bar ? juce::Colour (0xFFE3F1F5) : juce::Colour (0xFF71858E));
        g.setFont (juce::Font (bar ? 14.0f : 10.0f, bar ? juce::Font::bold : juce::Font::plain));
        const auto label = beatNo == 1 ? juce::String (barNo)
                                       : juce::String (barNo) + "." + juce::String (beatNo);
        g.drawText (label, x + 5, area.getY() + 5, beatWidth - 8, 18,
                    juce::Justification::centredLeft, false);
    }

    const int64_t loopL = engine.getLoopStartTick();
    const int64_t loopR = engine.getLoopEndTick();
    const int lx = kTrackHeaderWidth + (int) std::llround ((double) loopL / MidiClip::kPPQ * beatWidth) - (int) scrollPixels;
    const int rx = kTrackHeaderWidth + (int) std::llround ((double) loopR / MidiClip::kPPQ * beatWidth) - (int) scrollPixels;
    g.setColour (juce::Colour (0xFF48D7FF).withAlpha (0.16f));
    g.fillRect (juce::jmax (kTrackHeaderWidth, lx), area.getBottom() - 7,
                juce::jmax (0, rx - lx), 6);
    g.setColour (juce::Colour (0xFF48D7FF));
    g.fillRect (lx, area.getBottom() - 10, 10, 10);
    g.fillRect (rx - 10, area.getBottom() - 10, 10, 10);
}

void MetroArrangementView::drawTrackHeader (juce::Graphics& g, juce::Rectangle<int> area,
                                            int trackIndex, const SequencerTrackInfo& track) const
{
    const bool selected = selection.isTrack() && selection.trackIndex == trackIndex;
    g.setColour (selected ? juce::Colour (0xFF101D22) : juce::Colour (0xFF090C0F));
    g.fillRect (area);
    g.setColour (track.colour);
    g.fillRect (area.getX(), area.getY(), 4, area.getHeight());
    g.setColour (juce::Colour (0xFF26363D));
    g.drawHorizontalLine (area.getBottom() - 1, (float) area.getX(), (float) area.getRight());

    g.setColour (juce::Colour (0xFF0EA7D6));
    g.fillRoundedRectangle ((float) area.getX() + 12, (float) area.getY() + 15, 30, 30, 3.0f);
    g.setColour (juce::Colours::black);
    g.setFont (juce::Font (13.0f, juce::Font::bold));
    g.drawText ("M", area.getX() + 12, area.getY() + 15, 30, 30, juce::Justification::centred, false);

    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (14.0f, juce::Font::bold));
    g.drawText (track.name.isNotEmpty() ? track.name : ("TRACK " + juce::String (trackIndex + 1)),
                area.getX() + 52, area.getY() + 11, 92, 18, juce::Justification::centredLeft, false);
    g.setColour (juce::Colour (0xFF72838B));
    g.setFont (juce::Font (10.0f));
    g.drawText ("CH " + juce::String (track.midiChannel > 0 ? track.midiChannel : 1),
                area.getX() + 52, area.getY() + 30, 55, 14, juce::Justification::centredLeft, false);

    auto smallButton = [&g] (int x, int y, const char* s, juce::Colour col)
    {
        g.setColour (juce::Colour (0xFF162027));
        g.fillRoundedRectangle ((float)x, (float)y, 26.0f, 20.0f, 3.0f);
        g.setColour (col);
        g.drawText (s, x, y + 1, 26, 18, juce::Justification::centred, false);
    };
    smallButton (area.getX() + 52, area.getY() + 48, "M", juce::Colour (0xFF93A0A5));
    smallButton (area.getX() + 82, area.getY() + 48, "S", juce::Colour (0xFFF1C84B));
    smallButton (area.getX() + 112, area.getY() + 48, "R", juce::Colour (0xFFEC6168));
    g.setColour (juce::Colour (0xFF7A8A91));
    g.drawText ("⋮", area.getRight() - 22, area.getY() + 14, 14, 28,
                juce::Justification::centred, false);
}

void MetroArrangementView::drawClip (juce::Graphics& g, juce::Rectangle<int> area,
                                     int trackIndex, int clipIndex, const SequencerTrackInfo& track) const
{
    const bool selected = selection.isClip() && selection.trackIndex == trackIndex
                                      && selection.clipIndex == clipIndex;
    auto r = area.reduced (0, 0);
    g.setColour (track.colour.withAlpha (selected ? 0.90f : 0.72f));
    g.fillRoundedRectangle (r.toFloat(), 5.0f);

    g.setColour (juce::Colour (0xFFBDF4FF).withAlpha (selected ? 0.95f : 0.42f));
    g.drawRoundedRectangle (r.toFloat(), 5.0f, selected ? 2.0f : 1.0f);

    const auto info = engine.getClipInfo (trackIndex, clipIndex);
    auto* clip = engine.getClip (trackIndex, clipIndex);
    g.setColour (juce::Colours::white);
    g.setFont (juce::Font (12.0f, juce::Font::bold));
    g.drawText (track.name.isNotEmpty() ? track.name : "CLIP",
                r.getX() + 10, r.getY() + 6, juce::jmax (40, r.getWidth() - 42), 17,
                juce::Justification::centredLeft, false);
    g.setColour (juce::Colour (0xFFD9E9EE));
    g.setFont (juce::Font (10.0f));
    g.drawText (juce::String ((double) info.lengthTicks / (double)(MidiClip::kPPQ * 4), 1) + " bars",
                r.getX() + 10, r.getY() + 24, 55, 14, juce::Justification::centredLeft, false);

    if (clip != nullptr)
    {
        const juce::ScopedReadLock lock (clip->getLock());
        const auto& notes = clip->getNotes();
        const double clipBeats = juce::jmax (1.0, (double) info.lengthTicks / MidiClip::kPPQ);
        const int noteTop = r.getY() + 40;
        const int noteBottom = r.getBottom() - 7;
        for (const auto& n : notes)
        {
            const float x1 = (float) r.getX() + 4.0f
                           + (float) n.startTick / (float) info.lengthTicks * (float) juce::jmax (1, r.getWidth() - 8);
            const float x2 = (float) r.getX() + 4.0f
                           + (float) (n.startTick + n.durationTick) / (float) info.lengthTicks * (float) juce::jmax (1, r.getWidth() - 8);
            const float y = (float) noteBottom
                          - (float) juce::jlimit (0, 127, n.note) / 127.0f
                            * (float) juce::jmax (8, noteBottom - noteTop);
            g.setColour (juce::Colour (0xFFD9FBFF).withAlpha (0.88f));
            g.fillRoundedRectangle (juce::Rectangle<float> (x1, y, juce::jmax (2.0f, x2 - x1), 3.0f), 1.5f);
        }
    }

    g.setColour (juce::Colour (0xFFE9FAFF));
    g.fillRect (r.getX(), r.getY(), 3, r.getHeight());
    g.fillRect (r.getRight() - 3, r.getY(), 3, r.getHeight());
}

void MetroArrangementView::paint (juce::Graphics& graphics)
{
    const auto bounds = getLocalBounds();
    graphics.fillAll (juce::Colour (0xFF05080A));

    auto toolbar = bounds.removeFromTop (kToolbarHeight);
    auto ruler = bounds.removeFromTop (kRulerHeight);
    const auto grid = bounds;

    drawToolbar (graphics, toolbar);
    drawRuler (graphics, ruler);

    // Arrangement grid.
    graphics.setColour (juce::Colour (0xFF05090C));
    graphics.fillRect (grid);
    const int beatWidth = beatWidthPx();
    const int firstBeat = juce::jmax (0, (int) std::floor (scrollPixels / beatWidth));
    const int startX = kTrackHeaderWidth + firstBeat * beatWidth - (int) scrollPixels;

    for (int beat = firstBeat, x = startX; x < grid.getRight(); ++beat, x += beatWidth)
    {
        if (x < kTrackHeaderWidth) continue;
        graphics.setColour (beat % 4 == 0 ? juce::Colour (0xFF253B44) : juce::Colour (0xFF12252C));
        graphics.drawVerticalLine (x, (float) grid.getY(), (float) grid.getBottom());
        const int sub = juce::jmax (1, beatWidth / 4);
        for (int sx = x + sub; sx < x + beatWidth; sx += sub)
        {
            graphics.setColour (juce::Colour (0xFF0B181E));
            graphics.drawVerticalLine (sx, (float) grid.getY(), (float) grid.getBottom());
        }
    }

    for (int index = 0; index < engine.getNumTracks(); ++index)
    {
        const auto row = trackRowBounds (index);
        if (! row.intersects (grid)) continue;

        drawTrackHeader (graphics, { 0, row.getY(), kTrackHeaderWidth, row.getHeight() },
                         index, engine.getTrackInfo (index));

        const auto track = engine.getTrackInfo (index);
        for (int clipIndex = 0; clipIndex < engine.getNumClips (index); ++clipIndex)
        {
            const auto clip = clipBounds (index, clipIndex, row.getY());
            if (clip.intersects ({ kTrackHeaderWidth, grid.getY(), grid.getWidth() - kTrackHeaderWidth, grid.getHeight() }))
                drawClip (graphics, clip, index, clipIndex, track);
        }
    }

    // Loop shading.
    const int lx = kTrackHeaderWidth + (int) std::llround ((double) engine.getLoopStartTick() / MidiClip::kPPQ * beatWidth) - (int) scrollPixels;
    const int rx = kTrackHeaderWidth + (int) std::llround ((double) engine.getLoopEndTick() / MidiClip::kPPQ * beatWidth) - (int) scrollPixels;
    graphics.setColour (juce::Colour (0xFF48D7FF).withAlpha (0.035f));
    if (rx > lx)
        graphics.fillRect (juce::jmax (kTrackHeaderWidth, lx), grid.getY(),
                           juce::jmin (rx, grid.getRight()) - juce::jmax (kTrackHeaderWidth, lx),
                           grid.getHeight());

    // Playhead.
    const int playheadX = kTrackHeaderWidth
                        + (int) std::llround (engine.getPlayheadBeats() * beatWidth)
                        - (int) scrollPixels;
    if (playheadX >= kTrackHeaderWidth && playheadX <= grid.getRight())
    {
        graphics.setColour (juce::Colours::white.withAlpha (0.92f));
        graphics.fillRect (playheadX - 1, ruler.getY(), 2, grid.getBottom() - ruler.getY());
        juce::Path triangle;
        triangle.addTriangle ((float) playheadX - 7.0f, (float) ruler.getY(),
                              (float) playheadX + 7.0f, (float) ruler.getY(),
                              (float) playheadX, (float) ruler.getY() + 10.0f);
        graphics.fillPath (triangle);
    }

    // Footer/status line.
    graphics.setColour (juce::Colour (0xFF081116));
    graphics.fillRect (0, getBottom() - 26, getWidth(), 26);
    graphics.setColour (juce::Colour (0xFF50636C));
    graphics.setFont (juce::Font (10.0f));
    graphics.drawText ("ARRANGER   |   " + juce::String (engine.getLoopEndTick() / (MidiClip::kPPQ * 4)) + " BARS   |   1/16",
                       12, getBottom() - 21, 220, 16, juce::Justification::left, false);
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

    if (event.y < kToolbarHeight)
    {
        const int toolX = 446;
        if (event.x >= toolX && event.x < toolX + 5 * 44)
        {
            activeTool = juce::jlimit (0, 4, (event.x - toolX) / 44);
            repaint();
        }
        return;
    }

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
