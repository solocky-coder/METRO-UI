#pragma once
// =============================================================================
//  GlassStyle.h — shared drawing helpers for the "glass" theme
// =============================================================================
//  Everything here is a no-op or a cheap pass-through unless getTheme().glass is
//  set, so call sites can branch on GlassStyle::on() and keep their existing
//  painting untouched for every other theme.
//
//  The ambient glow is rendered ONCE into a cached image (rebuilt only when the
//  size or glow colours change) — never a live gradient/blur per paint().
// =============================================================================
#include <juce_graphics/juce_graphics.h>
#include "DysektLookAndFeel.h"

namespace GlassStyle
{
    inline bool  on()     { return getTheme().glass; }
    inline float radius() { return getTheme().glass ? getTheme().cornerRadius : 0.0f; }

    /** Soft purple/blue ambient glow behind the whole window (cached image). */
    inline void paintAmbient (juce::Graphics& g, juce::Rectangle<int> area)
    {
        const auto& t = getTheme();
        if (! t.glass || area.isEmpty()) return;

        static juce::Image cache;
        static juce::Rectangle<int> cachedArea;
        static juce::uint32 cachedA = 0, cachedB = 0, cachedBg = 0;

        if (! cache.isValid() || cachedArea != area
            || cachedA != t.glowA.getARGB() || cachedB != t.glowB.getARGB() || cachedBg != t.background.getARGB())
        {
            cache = juce::Image (juce::Image::ARGB, area.getWidth(), area.getHeight(), true);
            juce::Graphics ig (cache);
            const float w = (float) area.getWidth(), h = (float) area.getHeight();
            ig.fillAll (t.background);

            auto blob = [&] (juce::Colour c, float cx, float cy, float rad, float alpha)
            {
                juce::ColourGradient grad (c.withAlpha (alpha), cx, cy, c.withAlpha (0.0f), cx + rad, cy, true);
                ig.setGradientFill (grad);
                ig.fillRect (cache.getBounds());
            };
            blob (t.glowA, w * 0.92f, h * 0.05f, juce::jmax (w, h) * 0.55f, 0.20f);   // top-right
            blob (t.glowB, w * 0.00f, h * 0.55f, juce::jmax (w, h) * 0.45f, 0.18f);   // left edge
            blob (t.glowB, w * 0.35f, h * 1.05f, juce::jmax (w, h) * 0.50f, 0.10f);   // bottom

            cachedArea = area; cachedA = t.glowA.getARGB(); cachedB = t.glowB.getARGB(); cachedBg = t.background.getARGB();
        }
        g.drawImageAt (cache, area.getX(), area.getY());
    }

    /** Rounded translucent card with a 1px hairline — panels, toolbars, transport. */
    inline void fillCard (juce::Graphics& g, juce::Rectangle<float> r, float alpha = 0.92f)
    {
        const auto& t = getTheme();
        g.setColour (t.header.withAlpha (alpha));
        g.fillRoundedRectangle (r, t.cornerRadius);
        g.setColour (t.separator.withAlpha (0.9f));
        g.drawRoundedRectangle (r.reduced (0.5f), t.cornerRadius, 1.0f);
    }

    /** Translucent rounded clip: tinted body, soft top sheen, accent title-strip line. */
    inline void paintClip (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, bool selected, bool muted)
    {
        const float rad = getTheme().cornerRadius;
        const float a   = muted ? 0.10f : (selected ? 0.42f : 0.28f);
        g.setColour (c.withAlpha (a));
        g.fillRoundedRectangle (r, rad);
        juce::ColourGradient sheen (c.withAlpha (muted ? 0.05f : 0.20f), 0.f, r.getY(),
                                    c.withAlpha (0.0f), 0.f, r.getBottom(), false);
        g.setGradientFill (sheen);
        g.fillRoundedRectangle (r, rad);
        g.setColour (selected ? getTheme().accent : c.withAlpha (muted ? 0.35f : 0.85f));
        g.drawRoundedRectangle (r.reduced (0.5f), rad, selected ? 1.6f : 1.0f);
    }
}
