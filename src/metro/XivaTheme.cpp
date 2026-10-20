#include "XivaTheme.h"
#include "../ui/ThemeData.h"

namespace XivaTheme
{
    ThemeData build()
    {
        // Start from Metro so the 16 slice colours and any field not set
        // below stay valid, then override the look.
        ThemeData t = MetroTheme::build();
        static const juce::Colour kAccent (0xFF7EE6E6);

        t.name             = "xiva";
        t.background       = juce::Colour (0xFF06090D);
        t.waveformBg       = juce::Colour (0xFF0A0F15);
        t.darkBar          = juce::Colour (0xFF0E151C);
        t.foreground       = juce::Colour (0xFFC3DDE4);
        t.header           = juce::Colour (0xFF05080B);
        t.waveform         = kAccent;
        t.selectionOverlay = kAccent.withAlpha (0.22f);
        t.lockActive       = kAccent;
        t.lockInactive     = juce::Colour (0xFF7D95A1);
        t.gridLine         = juce::Colour (0xFF121B23);
        t.accent           = kAccent;
        t.button           = juce::Colour (0xFF0C131A);
        t.buttonHover      = juce::Colour (0xFF16212B);
        t.separator        = juce::Colour (0xFF2A3A46);

        t.cornerRadius     = 10.0f;
        t.glow             = 1.0f;
        t.panelTop         = juce::Colour (0xFF111A23);
        t.panelBottom      = juce::Colour (0xFF080C11);
        return t;
    }
}
