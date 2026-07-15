#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "MetroContent.h"

namespace dysekt::metro
{
/** Primary standalone navigation. Owns selection state and emits content changes. */
class MetroSidebar final : public juce::Component
{
public:
    MetroSidebar();

    std::function<void (MetroContent)> onSelectionChanged;
    void setSelectedContent (MetroContent content);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void selectContent (MetroContent content);

    juce::TextButton arrange { "Arrange" };
    juce::TextButton pads { "Pads" };
    juce::TextButton browser { "Browser" };
    juce::TextButton mixer { "Mixer" };
};
} // namespace dysekt::metro
