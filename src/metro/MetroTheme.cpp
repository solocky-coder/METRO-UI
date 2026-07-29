#include "MetroTheme.h"
#include "../ui/ThemeData.h"

namespace MetroTheme
{
    // Semantic status colours from the v1 spec's COLOR PALETTE section.
    // ThemeData has no dedicated success/warning/danger/secondary-text slots
    // (no theme in this app has ever needed them as first-class fields —
    // adding them would mean touching all twelve other themes' structs for
    // a feature only Metro uses), so they live here as the canonical
    // reference values for any Metro-specific code that wants them —
    // MixerPanel's meter already lands close to these by convention.
    //
    // Values below were re-sampled directly from the reskin spec's palette
    // swatches (not guessed from generic "Windows system colour" names) —
    // they're softer/more muted than the stock Windows 8 tile colours a
    // previous pass had used here (e.g. kWarning was the saturated Windows
    // "Amber" #FFB900; the spec's actual Warning swatch is the paler #F9B04D).
    const juce::Colour kSuccess       (0xFF60C560);   // Success swatch
    const juce::Colour kWarning       (0xFFF9B04D);   // Warning swatch
    const juce::Colour kDanger        (0xFFEC6168);   // Error swatch
    const juce::Colour kSecondaryText (0xFF7E7E7E);   // Text Secondary swatch

    ThemeData build()
    {
        ThemeData t;
        t.name             = "metro";
        // Anchors — re-matched to the reskin spec image's actual COLOR
        // PALETTE panel: a neutral flat #1E1E1E canvas (NOT the blue-black
        // #070B0D a previous "arranger redesign brief" had swapped in here —
        // that brief's blue-tinted anchors never matched the spec image and
        // are what this fix corrects), with the spec's cyan-blue Accent
        // swatch (~#0EA7D6) reserved for the strongest active state —
        // playhead, selected item edge, active tool. Everything below is
        // derived from this one constant, so a single-line change re-tints
        // the whole theme.
        static const juce::Colour kAccent (0xFF0EA7D6);
        t.background       = juce::Colour (0xFF1E1E1E);   // Background swatch
        t.waveformBg       = juce::Colour (0xFF252526);   // Surface swatch
        t.darkBar          = juce::Colour (0xFF2D2D30);   // Surface Alt swatch
        t.foreground       = juce::Colour (0xFFFFFFFF);   // Text Primary swatch
        t.header           = juce::Colour (0xFF1E1E1E);   // top bar == Background
        t.waveform         = kAccent;                      // Accent waveform
        t.selectionOverlay = kAccent.withAlpha (0.22f);
        t.lockActive       = kAccent;                      // Accent
        t.lockInactive     = kSecondaryText;                // Text Secondary swatch
        t.gridLine         = juce::Colour (0xFF2A2A2A);    // neutral, subtle grid
        t.accent           = kAccent;                      // Accent
        t.button           = juce::Colour (0xFF2D2D30);    // Surface Alt swatch — raised chrome
        t.buttonHover       = juce::Colour (0xFF3A3A3A);   // one step up from Surface Alt
        t.separator        = juce::Colour (0xFF3A3A3A);    // Border / Line swatch

        // Windows/Metro tile colour set — flat, single-hue swatches, no neon —
        // used for slice colours across the pad grid / mixer / waveform.
        t.slicePalette[0 ] = juce::Colour (0xFFA4C400); // Lime
        t.slicePalette[1 ] = juce::Colour (0xFF60A917); // Green
        t.slicePalette[2 ] = juce::Colour (0xFF008A00); // Emerald
        t.slicePalette[3 ] = juce::Colour (0xFF00ABA9); // Teal
        t.slicePalette[4 ] = juce::Colour (0xFF1BA1E2); // Cyan
        t.slicePalette[5 ] = juce::Colour (0xFF0050EF); // Cobalt
        t.slicePalette[6 ] = juce::Colour (0xFF6A00FF); // Indigo
        t.slicePalette[7 ] = juce::Colour (0xFFAA00FF); // Violet
        t.slicePalette[8 ] = juce::Colour (0xFFF472D0); // Pink
        t.slicePalette[9 ] = juce::Colour (0xFFD80073); // Magenta
        t.slicePalette[10] = juce::Colour (0xFFA20025); // Crimson
        t.slicePalette[11] = juce::Colour (0xFFE51400); // Red
        t.slicePalette[12] = juce::Colour (0xFFFA6800); // Orange
        t.slicePalette[13] = juce::Colour (0xFFF0A30A); // Amber
        t.slicePalette[14] = juce::Colour (0xFF647687); // Steel
        t.slicePalette[15] = juce::Colour (0xFF76608A); // Mauve
        return t;
    }
}
