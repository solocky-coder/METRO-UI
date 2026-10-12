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

    /** One soft radial blob of colour, used to build the cached ambient-glow image. */
    inline void glowBlob (juce::Graphics& ig, juce::Rectangle<int> area, juce::Colour c,
                          float cx, float cy, float rad, float alpha)
    {
        juce::ColourGradient grad (c.withAlpha (alpha), cx, cy, c.withAlpha (0.0f), cx + rad, cy, true);
        ig.setGradientFill (grad);
        ig.fillRect (area);
    }

    /** Soft purple/blue ambient glow behind the whole window (cached image).
        A few sizes are cached at once because several top-level surfaces paint it (the editor and
        the Arrange/Mixer/EQ window), and one shared cache would be rebuilt on every alternate paint. */
    inline void paintAmbient (juce::Graphics& g, juce::Rectangle<int> area)
    {
        const auto& t = getTheme();
        if (! t.glass || area.isEmpty()) return;

        struct Entry { int w = 0, h = 0; juce::uint32 a = 0, b = 0, bg = 0; juce::Image img; };
        static Entry cache[4];
        static int   nextSlot = 0;

        Entry* hit = nullptr;
        for (auto& e : cache)
            if (e.img.isValid() && e.w == area.getWidth() && e.h == area.getHeight()
                && e.a == t.glowA.getARGB() && e.b == t.glowB.getARGB() && e.bg == t.background.getARGB())
            { hit = &e; break; }

        if (hit == nullptr)
        {
            hit = &cache[nextSlot]; nextSlot = (nextSlot + 1) % 4;
            hit->w = area.getWidth(); hit->h = area.getHeight();
            hit->a = t.glowA.getARGB(); hit->b = t.glowB.getARGB(); hit->bg = t.background.getARGB();
            hit->img = juce::Image (juce::Image::ARGB, hit->w, hit->h, true);
            juce::Graphics ig (hit->img);
            const float w = (float) hit->w, h = (float) hit->h;
            ig.fillAll (t.background);
            glowBlob (ig, hit->img.getBounds(), t.glowA, w * 0.92f, h * 0.05f, juce::jmax (w, h) * 0.45f, 0.42f);   // top-right
            glowBlob (ig, hit->img.getBounds(), t.glowB, w * 0.00f, h * 0.55f, juce::jmax (w, h) * 0.38f, 0.36f);   // left edge
            glowBlob (ig, hit->img.getBounds(), t.glowB, w * 0.35f, h * 1.05f, juce::jmax (w, h) * 0.42f, 0.22f);   // bottom
        }
        g.drawImageAt (hit->img, area.getX(), area.getY());
    }

    /** Clip drawing to a rect — softly rounded in Glass so content never squares off a rounded frame. */
    inline void reduceClip (juce::Graphics& g, juce::Rectangle<int> r)
    {
        if (on()) { juce::Path p; p.addRoundedRectangle (r.toFloat(), radius() * 0.6f); g.reduceClipRegion (p); }
        else      { g.reduceClipRegion (r); }
    }

    /** Rounded translucent card with a 1px hairline — panels, toolbars, transport. */
    inline void fillCard (juce::Graphics& g, juce::Rectangle<float> r, float alpha = 0.66f)
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
