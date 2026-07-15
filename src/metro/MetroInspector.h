#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "MetroSelection.h"

namespace dysekt::metro
{
/** Bottom context area reserved for the selected Metro workspace item. */
class MetroInspector final : public juce::Component
{
public:
    MetroInspector();

    /** Generic placeholder text, still used by tabs without a selection model yet
        (Pads, Browser, Mixer). Arrangement selection should go through
        showTrack()/showClip()/clearSelection() instead. */
    void setContextMessage (const juce::String& message);

    /** Displays a selected track's real metadata rather than an opaque string. */
    void showTrack (const SequencerTrackInfo& track);

    /** Displays a selected clip's real metadata rather than an opaque string. */
    void showClip (int clipIndex, const SequencerTrackInfo& parentTrack, const SequencerClipInfo& clip);

    /** Clears any track/clip selection display, falling back to the context message. */
    void clearSelection();

    void paint (juce::Graphics&) override;

private:
    void paintSelectionDetails (juce::Graphics&, juce::Rectangle<int> content);

    juce::String contextMessage;
    MetroSelection selection;
};
} // namespace dysekt::metro
