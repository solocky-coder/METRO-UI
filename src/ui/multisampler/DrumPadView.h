#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include "../../audio/multisampler/MultisamplerInstrument.h"

class DysektProcessor;

/** Read-only 128-note drum-kit performance surface.  Zone mutations remain
    owned by MultisamplerEditor; this component only selects and auditions. */
class DrumPadView : public juce::Component, private juce::Timer
{
public:
    DrumPadView (DysektProcessor&, MultisamplerInstrument&);
    void rebuild();
    void setUiScale (float s) { uiScale = juce::jlimit (0.65f, 2.0f, s); resized(); }
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    std::function<void(int)> onPadSelected;
    std::function<void(int, juce::Point<int>)> onPadContextMenu;
private:
    struct PadInfo { int zoneIndex = -1; int layers = 0; bool root = false; juce::Colour colour; int bus = 0; juce::String name; };
    void timerCallback() override;
    int noteAt (juce::Point<int>) const;
    juce::String noteName (int) const;
    DysektProcessor& processor;
    MultisamplerInstrument& instrument;
    std::array<PadInfo,128> pads{};
    std::array<juce::Rectangle<int>,8> bankBounds{};
    std::array<juce::Rectangle<int>,16> padBounds{};
    std::array<bool,128> active{};
    int bank = 0;
    int heldNote = -1;
    int selectedNote = -1;
    float uiScale = 1.0f;
};
