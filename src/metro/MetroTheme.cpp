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
        // Anchors are the Engineering Manual v1's authoritative values:
        // Background #1E1E1E, Surface #252526, Text #FFFFFF, Accent #0078D7,
        // Secondary #C8C8C8. The manual has no "Raised" tier, so hover/grid/
        // separator/waveformBg are re-derived here from those anchors using
        // the same relative-brightness ratios as the previous (pre-manual)
        // palette: hover/grid ≈ Surface brightened ~4%, separator ≈ Surface
        // brightened ~7%, waveformBg/darkBar ≈ Background darkened ~19%.
        t.background       = juce::Colour (0xFF1E1E1E);   // Background
        t.waveformBg       = juce::Colour (0xFF181818);   // Background, darkened ~19%
        t.darkBar          = juce::Colour (0xFF181818);   // Background, darkened ~19%
        t.foreground       = juce::Colour (0xFFFFFFFF);   // Text
        t.header           = juce::Colour (0xFF1E1E1E);   // top bar == Background
        t.waveform         = juce::Colour (0xFF0078D7);   // Accent waveform
        t.selectionOverlay = juce::Colour (0xFF0078D7).withAlpha (0.22f);
        t.lockActive       = juce::Colour (0xFF0078D7);   // Accent
        t.lockInactive     = juce::Colour (0xFF4A4A4A);   // muted secondary — not manual-anchored
        t.gridLine         = juce::Colour (0xFF2E2E2F);   // Surface, brightened ~4%
        t.accent           = juce::Colour (0xFF0078D7);   // Accent
        t.button           = juce::Colour (0xFF252526);   // Surface
        t.buttonHover      = juce::Colour (0xFF2E2E2F);   // Surface, brightened ~4%
        t.separator        = juce::Colour (0xFF343435);   // Surface, brightened ~7%

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
