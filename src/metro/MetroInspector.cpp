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
    contextMessage = message;
    repaint();
}

void MetroInspector::paint (juce::Graphics& graphics)
{
    graphics.fillAll (MetroTheme::Colours::panelBackground);
    graphics.setColour (MetroTheme::Colours::separator);
    graphics.fillRect (getLocalBounds().removeFromTop (MetroTheme::Metrics::separatorThickness));

    const auto content = getLocalBounds().reduced (MetroTheme::Metrics::panelPadding);
    graphics.setColour (MetroTheme::Colours::textDisabled);
    graphics.setFont (MetroTheme::captionFont());
    graphics.drawText ("INSPECTOR", content.removeFromTop (MetroTheme::Metrics::grid * 3),
                       juce::Justification::centredLeft, true);
    graphics.setColour (MetroTheme::Colours::textSecondary);
    graphics.setFont (MetroTheme::smallFont());
    graphics.drawText (contextMessage, content, juce::Justification::centredLeft, true);
}
} // namespace dysekt::metro
