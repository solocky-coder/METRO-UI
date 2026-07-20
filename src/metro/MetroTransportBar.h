#pragma once

#include <functional>
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

    /** Fired when the user clicks the pop-out button — the host (e.g.
        MetroStandaloneEditor) owns what "floating" means: constructing a
        FloatingTransportBar, showing it, and hiding this docked bar while
        it's on screen. This class never creates a FloatingTransportBar
        itself, so MetroTransportBar.h/.cpp stay free of that dependency. */
    std::function<void()> onFloatRequested;

private:
    void timerCallback() override;
    void updateTempoFromEditor();

    SequencerEngine& engine;
    juce::TextButton rewind { "Rewind" };
    juce::TextButton play { "Play" };
    juce::TextButton stop { "Stop" };
    juce::TextButton record { "Record" };
    juce::TextButton floatButton { "Float" };
    juce::Label tempo;
};
} // namespace dysekt::metro
