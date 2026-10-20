#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

class AvixProcessor;

/**  Draws the AVIX brand logo (ring + waveform mark with the AVIX wordmark)
     centred in its bounds on the current theme's header colour.

     The logo is a single transparent PNG shared by every theme, so it keeps
     its neon-cyan brand colour regardless of which theme is active. */
class LogoBar : public juce::Component
{
public:
    explicit LogoBar (AvixProcessor& p);
    void paint (juce::Graphics& g) override;
private:
    AvixProcessor& processor;

    // Logo pre-scaled to the exact on-screen pixel size. Rebuilt only when
    // the size changes, so paint() is a single image blit.
    juce::Image scaledLogo;
    int         scaledForW = 0;
    int         scaledForH = 0;
};
