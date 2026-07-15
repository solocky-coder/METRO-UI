#include "MetroSidebar.h"
#include "MetroTheme.h"

namespace dysekt::metro
{
MetroSidebar::MetroSidebar()
{
    const auto configure = [this] (juce::TextButton& button, MetroContent content)
    {
        button.setClickingTogglesState (true);
        button.setRadioGroupId (1);
        button.onClick = [this, content] { selectContent (content); };
        addAndMakeVisible (button);
    };

    configure (arrange, MetroContent::arrange);
    configure (pads, MetroContent::pads);
    configure (browser, MetroContent::browser);
    configure (mixer, MetroContent::mixer);
    setSelectedContent (MetroContent::arrange);
}

void MetroSidebar::setSelectedContent (MetroContent content)
{
    arrange.setToggleState (content == MetroContent::arrange, juce::dontSendNotification);
    pads.setToggleState (content == MetroContent::pads, juce::dontSendNotification);
    browser.setToggleState (content == MetroContent::browser, juce::dontSendNotification);
    mixer.setToggleState (content == MetroContent::mixer, juce::dontSendNotification);
}

void MetroSidebar::paint (juce::Graphics& graphics)
{
    graphics.fillAll (MetroTheme::Colours::panelBackground);
    graphics.setColour (MetroTheme::Colours::separator);
    graphics.fillRect (getLocalBounds().removeFromRight (MetroTheme::Metrics::separatorThickness));
}

void MetroSidebar::resized()
{
    auto area = getLocalBounds().reduced (MetroTheme::Metrics::panelPadding);
    for (auto* button : { &arrange, &pads, &browser, &mixer })
        button->setBounds (area.removeFromTop (MetroTheme::Metrics::controlHeight));
}

void MetroSidebar::selectContent (MetroContent content)
{
    setSelectedContent (content);
    if (onSelectionChanged)
        onSelectionChanged (content);
}
} // namespace dysekt::metro
