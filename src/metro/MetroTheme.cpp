/*
    DYSEKT 2
    Metro UI

    MetroTheme.cpp
*/
#include "MetroTheme.h"
#include "MetroColours.h"

namespace dysekt::metro
{
const juce::Colour MetroTheme::Colours::windowBackground { Base::Background };
const juce::Colour MetroTheme::Colours::panelBackground  { Base::Surface };
const juce::Colour MetroTheme::Colours::raisedPanel      { Base::Elevated };
const juce::Colour MetroTheme::Colours::separator        { Base::Border };
const juce::Colour MetroTheme::Colours::accent           { Accent::Blue };
const juce::Colour MetroTheme::Colours::accentMuted      { Accent::Blue.withAlpha (0.42f) };
const juce::Colour MetroTheme::Colours::textPrimary      { Text::Primary };
const juce::Colour MetroTheme::Colours::textSecondary    { Text::Secondary };
const juce::Colour MetroTheme::Colours::textDisabled     { Text::Disabled };
const juce::Colour MetroTheme::Colours::hoverOverlay     { State::Hover };
const juce::Colour MetroTheme::Colours::pressedOverlay   { State::Pressed };
const juce::Colour MetroTheme::Colours::focusRing        { State::Focus };

juce::Font MetroTheme::displayFont()      { return MetroTypography::display(); }
juce::Font MetroTheme::titleFont()        { return MetroTypography::title(); }
juce::Font MetroTheme::sectionTitleFont() { return MetroTypography::sectionTitle(); }
juce::Font MetroTheme::bodyFont()         { return MetroTypography::body(); }
juce::Font MetroTheme::labelFont()        { return MetroTypography::label(); }
juce::Font MetroTheme::smallFont()        { return MetroTypography::small(); }
juce::Font MetroTheme::captionFont()      { return MetroTypography::caption(); }
juce::Font MetroTheme::buttonFont()       { return MetroTypography::button(); }
juce::Font MetroTheme::numericFont()      { return MetroTypography::numeric(); }

} // namespace dysekt::metro
