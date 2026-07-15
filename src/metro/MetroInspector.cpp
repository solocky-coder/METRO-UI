#include "MetroInspector.h"
#include "MetroTheme.h"

namespace dysekt::metro
{
MetroInspector::MetroInspector()
    : contextMessage ("Select a clip, pad, browser item, or mixer channel")
{
}

void MetroInspector::setContextMessage (const juce::String& message)
{
    selection = MetroSelection::none();
    contextMessage = message;
    repaint();
}

void MetroInspector::showTrack (const SequencerTrackInfo& track)
{
    selection = MetroSelection::forTrack (-1, track);
    repaint();
}

void MetroInspector::showClip (int clipIndex, const SequencerTrackInfo& parentTrack, const SequencerClipInfo& clip)
{
    selection = MetroSelection::forClip (-1, clipIndex, parentTrack, clip);
    repaint();
}

void MetroInspector::clearSelection()
{
    selection = MetroSelection::none();
    repaint();
}

void MetroInspector::paint (juce::Graphics& graphics)
{
    graphics.fillAll (MetroTheme::Colours::panelBackground);
    graphics.setColour (MetroTheme::Colours::separator);
    graphics.fillRect (getLocalBounds().removeFromTop (MetroTheme::Metrics::separatorThickness));

    auto content = getLocalBounds().reduced (MetroTheme::Metrics::panelPadding);
    graphics.setColour (MetroTheme::Colours::textDisabled);
    graphics.setFont (MetroTheme::captionFont());
    graphics.drawText ("INSPECTOR", content.removeFromTop (MetroTheme::Metrics::grid * 3),
                       juce::Justification::centredLeft, true);

    if (selection.isNone())
    {
        graphics.setColour (MetroTheme::Colours::textSecondary);
        graphics.setFont (MetroTheme::smallFont());
        graphics.drawText (contextMessage, content, juce::Justification::centredLeft, true);
        return;
    }

    paintSelectionDetails (graphics, content);
}

void MetroInspector::paintSelectionDetails (juce::Graphics& graphics, juce::Rectangle<int> content)
{
    graphics.setColour (MetroTheme::Colours::textPrimary);
    graphics.setFont (MetroTheme::bodyFont());

    if (selection.isTrack())
    {
        graphics.drawText ("Track: " + selection.track.name,
                           content.removeFromTop (MetroTheme::Metrics::grid * 3),
                           juce::Justification::centredLeft, true);
    }
    else if (selection.isClip())
    {
        graphics.drawText ("Clip " + juce::String (selection.clipIndex + 1) + " on " + selection.track.name,
                           content.removeFromTop (MetroTheme::Metrics::grid * 3),
                           juce::Justification::centredLeft, true);
    }

    graphics.setColour (MetroTheme::Colours::textSecondary);
    graphics.setFont (MetroTheme::smallFont());

    if (selection.isClip())
    {
        const auto startBeats = static_cast<double> (selection.clip.startTick) / 960.0;
        const auto lengthBeats = static_cast<double> (selection.clip.lengthTicks) / 960.0;
        graphics.drawText ("Start: beat " + juce::String (startBeats, 2)
                               + "   Length: " + juce::String (lengthBeats, 2) + " beats",
                           content.removeFromTop (MetroTheme::Metrics::grid * 3),
                           juce::Justification::centredLeft, true);
    }
    else if (selection.isTrack())
    {
        graphics.drawText (juce::String (selection.track.numClips) + " clip(s) on this track",
                           content.removeFromTop (MetroTheme::Metrics::grid * 3),
                           juce::Justification::centredLeft, true);
    }
}
} // namespace dysekt::metro
