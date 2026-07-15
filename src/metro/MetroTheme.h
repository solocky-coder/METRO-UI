/*
    DYSEKT 2
    Metro UI

    MetroTheme.h

    Central Metro theme facade. Components should obtain shared palette and
    typography through this header instead of declaring local design values.
*/
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "MetroMetrics.h"
#include "MetroTypography.h"

namespace dysekt::metro
{
struct MetroTheme final
{
    /** Semantic colours used by the Metro shell and its components. */
    struct Colours final
    {
        static const juce::Colour windowBackground;
        static const juce::Colour panelBackground;
        static const juce::Colour raisedPanel;
        static const juce::Colour separator;
        static const juce::Colour accent;
        static const juce::Colour accentMuted;
        static const juce::Colour textPrimary;
        static const juce::Colour textSecondary;
        static const juce::Colour textDisabled;
        static const juce::Colour hoverOverlay;
        static const juce::Colour pressedOverlay;
        static const juce::Colour focusRing;
    };

    using Metrics = MetroMetrics;

    static juce::Font displayFont();
    static juce::Font titleFont();
    static juce::Font sectionTitleFont();
    static juce::Font bodyFont();
    static juce::Font labelFont();
    static juce::Font smallFont();
    static juce::Font captionFont();
    static juce::Font buttonFont();
    static juce::Font numericFont();
};

} // namespace dysekt::metro
