#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

class DysektProcessor;

class LogoBar : public juce::Component
{
public:
    explicit LogoBar (DysektProcessor& p);
    void paint (juce::Graphics& g) override;
private:
    DysektProcessor& processor;
};
