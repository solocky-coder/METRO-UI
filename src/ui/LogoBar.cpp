#include "LogoBar.h"
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
    const auto avail = getLocalBounds().toFloat().reduced (2.0f);
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
}
