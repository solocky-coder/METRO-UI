#pragma once
#include "../ui/AvixLookAndFeel.h"   // getTheme()

// Shared painters for hand-painted components. Call these instead of
// g.fillRect / g.drawRect so every panel picks up radius, gradient and glow
// from the theme. On themes with cornerRadius == 0 and glow == 0 they
// degrade to flat squares, so other themes are unaffected.
namespace XivaPaint
{
    inline void glowStroke (juce::Graphics& g, const juce::Path& p, juce::Colour c,
                            float w, float glow)
    {
        using PST = juce::PathStrokeType;
        if (glow > 0.0f)
            for (int i = 3; i >= 1; --i)
            {
                g.setColour (c.withAlpha (0.05f * glow * (float) (4 - i)));
                g.strokePath (p, PST (w + (float) i * 2.0f, PST::curved, PST::rounded));
            }
        g.setColour (c);
        g.strokePath (p, PST (w, PST::curved, PST::rounded));
    }

    inline void panel (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const auto& t = getTheme();
        const auto top = t.panelTop.isTransparent()    ? t.waveformBg : t.panelTop;
        const auto bot = t.panelBottom.isTransparent() ? t.waveformBg : t.panelBottom;
        g.setGradientFill (juce::ColourGradient (top, r.getX(), r.getY(),
                                                 bot, r.getRight(), r.getBottom(), false));
        g.fillRoundedRectangle (r, t.cornerRadius);
        g.setColour (t.accent.withAlpha (0.16f));
        g.drawRoundedRectangle (r.reduced (0.5f), t.cornerRadius, 1.0f);
    }

    inline void panel (juce::Graphics& g, juce::Rectangle<int> r) { panel (g, r.toFloat()); }
}
