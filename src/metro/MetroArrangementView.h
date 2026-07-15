/*
    DYSEKT 2
    Metro UI

    MetroArrangementView.h

    The Metro standalone's arrangement/timeline workspace: draws the timeline
    ruler, track lanes and clips, owns the playhead redraw timer, and handles
    click-to-select for tracks and clips.

    This was previously a private nested class inside MetroStandaloneEditor.
    It is extracted here so it can be reused, tested, and own its own
    selection/interaction logic independently of the editor shell.
*/
#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "MetroSelection.h"

class SequencerEngine;

namespace dysekt::metro
{
/** Draws and drives interaction for the arrangement (track/clip) workspace. */
class MetroArrangementView final : public juce::Component,
                                    private juce::Timer
{
public:
    explicit MetroArrangementView (SequencerEngine& sequencer);
    ~MetroArrangementView() override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    /** Horizontal zoom multiplier applied to the base beat width. Clamped to a sane range. */
    void setZoom (float newZoom);
    float getZoom() const noexcept { return zoom; }

    /** Horizontal scroll offset in pixels (pre-zoom timeline space). Clamped to >= 0. */
    void setScrollPosition (float newScrollPixels);
    float getScrollPosition() const noexcept { return scrollPixels; }

    /** Called whenever the user selects (or deselects) a track or clip. */
    void setSelectionChangedCallback (std::function<void (const MetroSelection&)> callback);

    /** Clears the current selection and notifies the callback, if any. */
    void clearSelection();

    const MetroSelection& getSelection() const noexcept { return selection; }

private:
    void timerCallback() override;

    int beatWidthPx() const noexcept;
    juce::Rectangle<int> trackRowBounds (int trackIndex) const;
    juce::Rectangle<int> clipBounds (int trackIndex, int clipIndex, int rowY) const;

    /** Returns trackIndex >= 0 if the point falls on a track row, else -1. clipIndexOut is
        set if the point also falls on a clip within that row, else left at -1. */
    int hitTestTrack (juce::Point<int> position, int& clipIndexOut) const;

    void setSelection (MetroSelection newSelection);

    SequencerEngine& engine;

    float zoom = 1.0f;
    float scrollPixels = 0.0f;

    MetroSelection selection;
    std::function<void (const MetroSelection&)> onSelectionChanged;
};
} // namespace dysekt::metro
