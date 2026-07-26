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
    const juce::Colour kSuccess       (0xFF16C60C);
    const juce::Colour kWarning       (0xFFFFB900);
    const juce::Colour kDanger        (0xFFE81123);
    const juce::Colour kSecondaryText (0xFFC8C8C8);   // Secondary (Engineering Manual v1)

    ThemeData build()
    {
        ThemeData t;
        t.name             = "metro";
        // Anchors, updated to match the arranger redesign brief: a blue-black
        // canvas (#070B0D) instead of true black so panels read as tinted
        // rather than neutral charcoal, and a cyan accent (#00D7E8) reserved
        // for the strongest active state — playhead, selected item edge,
        // active tool — per the brief's color rules. Everything below is
        // derived from this one constant, so a single-line change re-tints
        // the whole theme.
        static const juce::Colour kAccent (0xFF00D7E8);
        t.background       = juce::Colour (0xFF070B0D);   // blue-black canvas
        t.waveformBg       = juce::Colour (0xFF0C1418);    // panel surface
        t.darkBar          = juce::Colour (0xFF132027);    // raised bars/toolbar blocks
        t.foreground       = juce::Colour (0xFFD7EEF2);   // Text
        t.header           = juce::Colour (0xFF070B0D);   // top bar == Background
        t.waveform         = kAccent;                      // Accent waveform
        t.selectionOverlay = kAccent.withAlpha (0.22f);
        t.lockActive       = kAccent;                      // Accent
        t.lockInactive     = juce::Colour (0xFF78919A);   // muted secondary — --ui-muted
        t.gridLine         = juce::Colour (0xFF1A282E);    // blue-tinted grid, subtle
        t.accent           = kAccent;                      // Accent
        t.button           = juce::Colour (0xFF132027);    // --ui-raised
        t.buttonHover      = juce::Colour (0xFF1A2C34);    // slightly lifted from --ui-raised
        t.separator        = juce::Colour (0xFF29404A);    // --ui-rule

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
