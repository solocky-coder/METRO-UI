/*
    DYSEKT 2
    Metro UI

    MetroLookAndFeel.h

    JUCE LookAndFeel implementation for the flat, compact Metro interface.
*/
#pragma once

#include "MetroTheme.h"

namespace dysekt::metro
{
class MetroLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    MetroLookAndFeel();

    void drawButtonBackground (juce::Graphics&, juce::Button&,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&,
                           bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;

    void drawLabel (juce::Graphics&, juce::Label&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height,
                                juce::TextEditor&) override;
};

} // namespace dysekt::metro
