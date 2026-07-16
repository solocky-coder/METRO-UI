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
    const juce::Colour kSecondaryText (0xFFB0B0B0);

    ThemeData build()
    {
        ThemeData t;
        t.name             = "metro";
        t.background       = juce::Colour (0xFF202124);   // Background
        t.waveformBg       = juce::Colour (0xFF1A1B1E);   // dark card behind waveform
        t.darkBar          = juce::Colour (0xFF1A1B1E);   // header / bar strip
        t.foreground       = juce::Colour (0xFFFFFFFF);   // Text
        t.header           = juce::Colour (0xFF202124);   // top bar
        t.waveform         = juce::Colour (0xFF0078D7);   // Accent waveform
        t.selectionOverlay = juce::Colour (0xFF0078D7).withAlpha (0.22f);
        t.lockActive       = juce::Colour (0xFF0078D7);   // Accent
        t.lockInactive     = juce::Colour (0xFF4A4A4A);   // muted secondary
        t.gridLine         = juce::Colour (0xFF333333);   // Raised — subtle grid
        t.accent           = juce::Colour (0xFF0078D7);   // Accent
        t.button           = juce::Colour (0xFF2B2B2B);   // Panel / Surface
        t.buttonHover      = juce::Colour (0xFF333333);   // Raised
        t.separator        = juce::Colour (0xFF3A3A3A);   // thin separator line

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
