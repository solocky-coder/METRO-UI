/*
    DYSEKT 2
    Metro UI

    MetroLookAndFeel.cpp
*/
#include "MetroLookAndFeel.h"

namespace dysekt::metro
{
MetroLookAndFeel::MetroLookAndFeel()
{
    setColour (juce::TextButton::buttonColourId,       MetroTheme::Colours::raisedPanel);
    setColour (juce::TextButton::textColourOffId,      MetroTheme::Colours::textPrimary);
    setColour (juce::TextButton::textColourOnId,       MetroTheme::Colours::textPrimary);
    setColour (juce::ToggleButton::textColourId,       MetroTheme::Colours::textPrimary);
    setColour (juce::Label::textColourId,              MetroTheme::Colours::textPrimary);
    setColour (juce::Label::backgroundColourId,        juce::Colours::transparentBlack);
    setColour (juce::TextEditor::textColourId,         MetroTheme::Colours::textPrimary);
    setColour (juce::TextEditor::backgroundColourId,   MetroTheme::Colours::raisedPanel);
    setColour (juce::TextEditor::highlightColourId,    MetroTheme::Colours::accentMuted);
    setColour (juce::TextEditor::highlightedTextColourId, MetroTheme::Colours::textPrimary);
    setColour (juce::TextEditor::outlineColourId,      MetroTheme::Colours::separator);
    setColour (juce::TextEditor::focusedOutlineColourId, MetroTheme::Colours::focusRing);
}

void MetroLookAndFeel::drawButtonBackground (juce::Graphics& graphics,
                                             juce::Button& button,
                                             const juce::Colour& backgroundColour,
                                             bool highlighted,
                                             bool down)
{
    auto colour = button.getToggleState() ? MetroTheme::Colours::accentMuted
                                          : backgroundColour;

    if (highlighted)
        colour = colour.interpolatedWith (juce::Colours::white, 0.08f);
    if (down)
        colour = colour.interpolatedWith (juce::Colours::black, 0.16f);

    const auto bounds = button.getLocalBounds().toFloat();
    graphics.setColour (colour);
    graphics.fillRoundedRectangle (bounds, MetroTheme::Metrics::controlCornerRadius);

    graphics.setColour (MetroTheme::Colours::separator);
    graphics.drawRoundedRectangle (bounds.reduced (0.5f), MetroTheme::Metrics::controlCornerRadius, 1.0f);
}

void MetroLookAndFeel::drawButtonText (juce::Graphics& graphics,
                                       juce::TextButton& button,
                                       bool, bool)
{
    graphics.setColour (button.isEnabled() ? MetroTheme::Colours::textPrimary
                                           : MetroTheme::Colours::textDisabled);
    graphics.setFont (MetroTheme::buttonFont());
    graphics.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (MetroTheme::Metrics::compactPadding, 0),
                             juce::Justification::centred, 1);
}

void MetroLookAndFeel::drawToggleButton (juce::Graphics& graphics,
                                         juce::ToggleButton& button,
                                         bool highlighted, bool down)
{
    const auto bounds = button.getLocalBounds();
    const auto boxSize = juce::jmin (bounds.getHeight() - MetroTheme::Metrics::quarterGrid,
                                     MetroTheme::Metrics::iconButtonSize - MetroTheme::Metrics::quarterGrid);
    auto box = bounds.withSizeKeepingCentre (boxSize, boxSize)
                     .withX (bounds.getX() + MetroTheme::Metrics::halfGrid)
                     .toFloat();

    auto fill = button.getToggleState() ? MetroTheme::Colours::accent : MetroTheme::Colours::raisedPanel;
    if (highlighted) fill = fill.brighter (0.08f);
    if (down) fill = fill.darker (0.12f);
    graphics.setColour (fill);
    graphics.fillRoundedRectangle (box, MetroTheme::Metrics::controlCornerRadius);
    graphics.setColour (MetroTheme::Colours::separator);
    graphics.drawRoundedRectangle (box.reduced (0.5f), MetroTheme::Metrics::controlCornerRadius, 1.0f);

    if (button.getToggleState())
    {
        graphics.setColour (MetroTheme::Colours::textPrimary);
        graphics.setFont (MetroTheme::buttonFont());
        graphics.drawText (juce::String (juce::CharPointer_UTF8 ("✓")), box.toNearestInt(), juce::Justification::centred);
    }

    graphics.setColour (button.isEnabled() ? MetroTheme::Colours::textPrimary : MetroTheme::Colours::textDisabled);
    graphics.setFont (MetroTheme::labelFont());
    graphics.drawFittedText (button.getButtonText(), bounds.withTrimmedLeft (box.getRight() + MetroTheme::Metrics::compactPadding),
                             juce::Justification::centredLeft, 1);
}

void MetroLookAndFeel::drawLabel (juce::Graphics& graphics, juce::Label& label)
{
    graphics.fillAll (label.findColour (juce::Label::backgroundColourId));
    graphics.setColour (label.findColour (juce::Label::textColourId));
    graphics.setFont (label.getFont().getHeight() > 0.0f ? label.getFont() : MetroTheme::labelFont());
    graphics.drawFittedText (label.getText(), label.getLocalBounds().reduced (MetroTheme::Metrics::halfGrid, 0),
                             label.getJustificationType(), 1);
}

void MetroLookAndFeel::drawTextEditorOutline (juce::Graphics& graphics, int width, int height,
                                              juce::TextEditor& editor)
{
    const auto colour = editor.hasKeyboardFocus (true) ? MetroTheme::Colours::focusRing
                                                        : MetroTheme::Colours::separator;
    graphics.setColour (colour);
    graphics.drawRoundedRectangle (juce::Rectangle<float> (static_cast<float> (width), static_cast<float> (height)).reduced (0.5f),
                                   MetroTheme::Metrics::controlCornerRadius, MetroTheme::Metrics::focusRingThickness);
}

} // namespace dysekt::metro
