#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

class SequencerEngine;

namespace dysekt::metro
{
/** Standalone transport controls bound to the shared sequencer engine. */
class MetroTransportBar final : public juce::Component,
                                private juce::Timer
{
public:
    explicit MetroTransportBar (SequencerEngine& sequencer);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void updateTempoFromEditor();

    SequencerEngine& engine;
    juce::TextButton rewind { "Rewind" };
    juce::TextButton play { "Play" };
    juce::TextButton stop { "Stop" };
    juce::TextButton record { "Record" };
    juce::Label tempo;
};
} // namespace dysekt::metro
