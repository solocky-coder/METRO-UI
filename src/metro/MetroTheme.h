#pragma once
#include <juce_graphics/juce_graphics.h>

// Forward-declared only — this header must not pull in ThemeData.h, since
// ThemeData.h itself calls MetroTheme::build() from ThemeData::metroTheme()
// to register Metro with the app's existing theme system (ThemeData is the
// single data model every panel already reads via getTheme(); Metro needs to
// go through that same channel to reach every hand-painted component, not
// just the standard JUCE widgets a LookAndFeel subclass can reach).
struct ThemeData;

// Windows Metro / Fluent palette — DYSEKT-SF Metro UI Developer Specification
// (Technical) v2, THEME section, plus the fuller palette from v1's COLOR
// PALETTE section (Success/Warning/Danger, secondary text, slice colours).
namespace MetroTheme
{
    ThemeData build();

    // Semantic status colours from the spec's COLOR PALETTE section — not
    // part of ThemeData's shared schema (see MetroTheme.cpp), but declared
    // here as the canonical values for any Metro-specific code that needs
    // success/warning/danger/secondary-text colours (e.g. a future meter
    // or status readout done in the Metro idiom).
    extern const juce::Colour kSuccess;
    extern const juce::Colour kWarning;
    extern const juce::Colour kDanger;
    extern const juce::Colour kSecondaryText;
}
