/*
    DYSEKT 2
    Metro UI

    MetroTypography.h

    Shared type scale and font factories for the Metro standalone UI.
*/
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dysekt::metro
{
/**
    The complete Metro type scale.

    Metro uses a compact, highly legible hierarchy. Use these factories rather
    than constructing fonts at call sites, ensuring labels and panels stay
    visually consistent throughout the standalone application.
*/
struct MetroTypography final
{
    // Preferred Windows UI family. JUCE selects a suitable system fallback
    // when Segoe UI is unavailable on the current platform.
    static constexpr const char* fontFamily = "Segoe UI";

    static juce::Font display()
    {
        return makeFont (24.0f, juce::Font::bold);
    }

    static juce::Font title()
    {
        return makeFont (18.0f, juce::Font::bold);
    }

    static juce::Font sectionTitle()
    {
        return makeFont (14.0f, juce::Font::bold);
    }

    static juce::Font body()
    {
        return makeFont (14.0f, juce::Font::plain);
    }

    static juce::Font label()
    {
        return makeFont (12.0f, juce::Font::plain);
    }

    static juce::Font small()
    {
        return makeFont (11.0f, juce::Font::plain);
    }

    static juce::Font caption()
    {
        return makeFont (10.0f, juce::Font::plain);
    }

    static juce::Font button()
    {
        return makeFont (12.0f, juce::Font::bold);
    }

    static juce::Font numeric()
    {
        return makeFont (14.0f, juce::Font::plain);
    }

private:
    static juce::Font makeFont (float height, int styleFlags)
    {
        return juce::Font (juce::FontOptions (fontFamily, height, styleFlags));
    }
};

} // namespace dysekt::metro
