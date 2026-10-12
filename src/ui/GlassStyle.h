#pragma once
// Shared painting helpers for the opt-in Glass theme. All call sites guard
// these helpers with on(), so other themes retain their existing appearance.
#include <juce_graphics/juce_graphics.h>
#include "DysektLookAndFeel.h"

namespace GlassStyle
{
    inline bool on() { return getTheme().glass; }
    inline float radius() { return on() ? getTheme().cornerRadius : 0.0f; }

    // Cache the ambient background so repaints don't rebuild it each frame.
    inline void paintAmbient (juce::Graphics& g, juce::Rectangle<int> area)
    {
        const auto& t = getTheme();
        if (! t.glass || area.isEmpty()) return;
        static juce::Image cache;
        static juce::Rectangle<int> cachedArea;
        static juce::uint32 cachedA = 0, cachedB = 0, cachedBg = 0;
        if (! cache.isValid() || cachedArea != area || cachedA != t.glowA.getARGB()
            || cachedB != t.glowB.getARGB() || cachedBg != t.background.getARGB())
        {
            cache = juce::Image (juce::Image::ARGB, area.getWidth(), area.getHeight(), true);
            juce::Graphics ig (cache);
            const float w = (float) area.getWidth(), h = (float) area.getHeight();
            ig.fillAll (t.background);
            auto blob = [&] (juce::Colour c, float cx, float cy, float rad, float alpha)
            {
                juce::ColourGradient grad (c.withAlpha (alpha), cx, cy,
                                           c.withAlpha (0.0f), cx + rad, cy, true);
                ig.setGradientFill (grad);
                ig.fillRect (cache.getBounds());
            };
            blob (t.glowA, w * 0.92f, h * 0.05f, juce::jmax (w, h) * 0.55f, 0.20f);
            blob (t.glowB, w * 0.00f, h * 0.55f, juce::jmax (w, h) * 0.45f, 0.18f);
            blob (t.glowB, w * 0.35f, h * 1.05f, juce::jmax (w, h) * 0.50f, 0.10f);
            cachedArea = area; cachedA = t.glowA.getARGB(); cachedB = t.glowB.getARGB();
            cachedBg = t.background.getARGB();
        }
        g.drawImageAt (cache, area.getX(), area.getY());
    }

    inline void fillCard (juce::Graphics& g, juce::Rectangle<float> r, float alpha = 0.92f)
    {
        const auto& t = getTheme();
        g.setColour (t.header.withAlpha (alpha));
        g.fillRoundedRectangle (r, t.cornerRadius);
        g.setColour (t.separator.withAlpha (0.9f));
        g.drawRoundedRectangle (r.reduced (0.5f), t.cornerRadius, 1.0f);
    }

    inline void paintClip (juce::Graphics& g, juce::Rectangle<float> r,
                           juce::Colour c, bool selected, bool muted)
    {
        const auto& t = getTheme();
        const float a = muted ? 0.10f : (selected ? 0.42f : 0.28f);
        g.setColour (c.withAlpha (a));
        g.fillRoundedRectangle (r, t.cornerRadius);
        juce::ColourGradient sheen (c.withAlpha (muted ? 0.05f : 0.20f), 0.0f, r.getY(),
                                    c.withAlpha (0.0f), 0.0f, r.getBottom(), false);
        g.setGradientFill (sheen);
        g.fillRoundedRectangle (r, t.cornerRadius);
        g.setColour (selected ? t.accent : c.withAlpha (muted ? 0.35f : 0.85f));
        g.drawRoundedRectangle (r.reduced (0.5f), t.cornerRadius, selected ? 1.6f : 1.0f);
    }
}
