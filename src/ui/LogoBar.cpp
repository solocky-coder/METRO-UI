#include "LogoBar.h"
#include <cmath>
#include "AvixLookAndFeel.h"
#include "../PluginProcessor.h"
#include <BinaryData.h>

LogoBar::LogoBar (AvixProcessor& p) : processor (p) {}

void LogoBar::paint (juce::Graphics& g)
{
    // No background fill: the logo is a transparent PNG and should float on
    // whatever the editor paints behind it, not sit in a header-coloured box.

    // ── AVIX logo ─────────────────────────────────────────────────────────
    // One transparent PNG for every theme (the brand colour is fixed), drawn
    // without a frame border so it sits directly on the header colour.
    const auto src = juce::ImageCache::getFromMemory (BinaryData::AVIX_logo_png,
                                                      BinaryData::AVIX_logo_pngSize);
    if (! src.isValid())
        return;

    const float physScale = g.getInternalContext().getPhysicalPixelScaleFactor();

    // Fit inside the bounds (small inset), keep aspect, never enlarge.
    // Generous inset so the XIVA wordmark never touches the frame border.
    const float padX = juce::jmax (6.0f, (float) getWidth()  * 0.04f);
    const float padY = juce::jmax (8.0f, (float) getHeight() * 0.09f);
    const auto avail = getLocalBounds().toFloat().reduced (padX, padY);
    const float fit  = juce::jmin (avail.getWidth()  * physScale / (float) src.getWidth(),
                                   avail.getHeight() * physScale / (float) src.getHeight(),
                                   1.0f);
    const int targetW = juce::jmax (1, juce::roundToInt ((float) src.getWidth()  * fit));
    const int targetH = juce::jmax (1, juce::roundToInt ((float) src.getHeight() * fit));

    if (! scaledLogo.isValid() || scaledForW != targetW || scaledForH != targetH)
    {
        // Halve repeatedly until within 2x of the target, then do the final
        // resize. A single large bilinear downscale skips source pixels and
        // makes the thin glowing strokes shimmer; stepwise halving averages
        // them instead.
        juce::Image img = src;
        while (img.getWidth() >= targetW * 2 && img.getHeight() >= targetH * 2)
            img = img.rescaled (img.getWidth() / 2, img.getHeight() / 2,
                                juce::Graphics::highResamplingQuality);

        scaledLogo = (img.getWidth() == targetW && img.getHeight() == targetH)
                         ? img
                         : img.rescaled (targetW, targetH, juce::Graphics::highResamplingQuality);
        scaledForW = targetW;
        scaledForH = targetH;
    }

    const float drawW = (float) targetW / physScale;
    const float drawH = (float) targetH / physScale;
    const juce::Rectangle<float> dest (avail.getCentreX() - drawW * 0.5f,
                                       avail.getCentreY() - drawH * 0.5f,
                                       drawW, drawH);

    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (scaledLogo, dest, juce::RectanglePlacement::stretchToFit);

    // ── Enlarged waveform ─────────────────────────────────────────────────
    // The waveform baked into the PNG is tiny and faint. Draw a larger,
    // brighter one across the full frame width, centred on the PNG's own
    // waveform row (76/205 of the image height), in front of the X.
    {
        const auto cyan   = juce::Colour (0xFF6FE3EC);
        const float cy    = dest.getY() + dest.getHeight() * (76.0f / 205.0f);
        const float x0    = (float) getLocalBounds().getX() + padX;
        const float x1    = (float) getLocalBounds().getRight() - padX;
        const float halfW = (x1 - x0) * 0.5f;
        const float cx    = (x0 + x1) * 0.5f;
        const float maxA  = dest.getHeight() * 0.27f;      // peak half-height

        g.setColour (cyan.withAlpha (0.55f));
        g.drawLine (x0, cy, x1, cy, 1.0f);

        const float step = 2.0f;
        for (float x = x0; x <= x1; x += step)
        {
            const float t    = (x - cx) / halfW;                       // -1..1
            const float env  = std::exp (-t * t * 3.2f);               // bell envelope
            const int   k    = (int) ((x - x0) / step);
            const juce::uint32 h = (juce::uint32) (k * 2654435761u) ^ 0x9E3779B9u;
            const float rnd  = 0.25f + 0.75f * (float) ((h >> 8) & 0xFF) / 255.0f;
            const float amp  = maxA * env * rnd;
            if (amp < 0.6f) continue;
            g.setColour (cyan.withAlpha (0.45f + 0.5f * env));
            g.drawLine (x, cy - amp, x, cy + amp, 1.2f);
        }
    }
}
