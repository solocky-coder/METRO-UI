#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dysekt::metro
{
/** Bottom context area reserved for the selected Metro workspace item. */
class MetroInspector final : public juce::Component
{
public:
    MetroInspector();

    void setContextMessage (const juce::String& message);
    void paint (juce::Graphics&) override;

private:
    juce::String contextMessage;
};
} // namespace dysekt::metro
