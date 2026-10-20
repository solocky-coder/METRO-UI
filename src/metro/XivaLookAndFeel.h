#pragma once
#include "MetroLookAndFeel.h"

// Active only while theme == "xiva". Inherits Metro (and through it Avix),
// overriding just the stock-widget drawing that differs.
class XivaLookAndFeel : public MetroLookAndFeel
{
public:
    XivaLookAndFeel() = default;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool isHighlighted, bool isDown) override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize,
                        bool isMouseOver, bool isMouseDown) override;
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline    (juce::Graphics&, int width, int height, juce::TextEditor&) override;
};
