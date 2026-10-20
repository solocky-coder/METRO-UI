#pragma once
#include <juce_graphics/juce_graphics.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <BinaryData.h>

/**  Returns the AVIX app icon (ring + waveform mark on a dark rounded tile)
     as a square image.
     @param size  Pixel dimensions of the square image (256 recommended). */
inline juce::Image makeLogoImage (int size)
{
    const auto src = juce::ImageCache::getFromMemory (BinaryData::AVIX_icon_png,
                                                      BinaryData::AVIX_icon_pngSize);
    if (! src.isValid() || size <= 0)
        return {};

    if (src.getWidth() == size && src.getHeight() == size)
        return src;

    return src.rescaled (size, size, juce::Graphics::highResamplingQuality);
}

/**  Sets the AVIX icon as the window peer icon (Windows / Linux title bar +
     taskbar) and the macOS dock icon.
     Safe to call any time after the component has a peer. */
inline void applyWindowIcon (juce::Component* comp)
{
    const auto img = makeLogoImage (256);

    if (! img.isValid())
        return;

   #if JUCE_MAC
    juce::Process::setDockIconImage (img);
   #endif

    if (comp != nullptr)
        if (auto* peer = comp->getPeer())
            peer->setIcon (img);
}
