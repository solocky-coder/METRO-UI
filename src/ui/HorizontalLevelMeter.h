#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../network/NetworkTrackMeters.h"
#include <algorithm>
#include <cmath>

//==============================================================================
//  HorizontalLevelMeter
//
//  Slim horizontal peak meter for a Network Audio track header.
//    - Stereo route: two stacked bars (L over R). Mono route: one bar.
//    - Scale -60 dB .. 0 dB. Flat zones: green to -18 dB, amber to -6 dB, red above.
//    - Peak-hold tick (holds ~1.1 s, then falls), clip LED (latches ~1.5 s when a
//      block peaks at or above 0 dBFS).
//    - "Offline" look (empty, dimmed bars) when the track stops rendering a source.
//
//  Usage: call update() at kUpdateHz with the latest NetworkTrackMeters::Reading
//  (it returns true while the meter still has something to draw, so the caller can
//  skip repaints for idle tracks), and paint() from the owner's paint().
//==============================================================================
class HorizontalLevelMeter
{
public:
    static constexpr int   kUpdateHz = 30;
    static constexpr float kFloorDb  = -60.0f;

    // Returns true if the meter is (or has just stopped being) visibly active.
    bool update (const NetworkTrackMeters::Reading& r) noexcept
    {
        const bool wasActive = active;

        if (r.blocks != lastBlocks)
        {
            lastBlocks = r.blocks;
            liveHold   = kLiveHoldTicks;
            stereo     = r.stereo;
        }
        else if (liveHold > 0)
        {
            --liveHold;
        }

        const float raw[2] = { r.left, r.right };
        for (int c = 0; c < 2; ++c)
        {
            const float p = gainToPos (raw[c]);

            if (p >= level[c]) level[c] = p;                                   // instant attack
            else               level[c] = std::max (0.0f, level[c] - kFallPerTick);

            if (p >= peak[c])            { peak[c] = p; peakHold[c] = kPeakHoldTicks; }
            else if (peakHold[c] > 0)    { --peakHold[c]; }
            else                         { peak[c] = std::max (0.0f, peak[c] - kPeakFallPerTick); }
        }

        if (raw[0] >= 0.999f || raw[1] >= 0.999f) clipHold = kClipHoldTicks;
        else if (clipHold > 0)                    --clipHold;

        active = liveHold > 0 || clipHold > 0
                 || level[0] > 0.0f || level[1] > 0.0f
                 || peak[0]  > 0.0f || peak[1]  > 0.0f;

        return active || wasActive;
    }

    bool isLive() const noexcept { return liveHold > 0; }

    // `area` is the full meter rectangle (bars + clip LED). Stereo bars are used
    // when the route is stereo and the area is tall enough for two readable bars.
    void paint (juce::Graphics& g, juce::Rectangle<int> area, juce::Colour barBackground) const
    {
        if (area.getWidth() < 12 || area.getHeight() < 2)
            return;

        constexpr int kClipW = 4, kClipGap = 3;
        const int clipX = area.getRight() - kClipW;
        const int barW  = area.getWidth() - kClipW - kClipGap;

        const float dim = isLive() ? 1.0f : 0.55f;
        const auto bg   = barBackground.withMultipliedAlpha (dim);

        // Clip LED
        g.setColour (clipHold > 0 ? kRed : bg);
        g.fillRect (clipX, area.getY(), kClipW, area.getHeight());

        const bool twoBars = stereo && area.getHeight() >= 7;
        const int  gap     = twoBars ? 1 : 0;
        const int  barH    = twoBars ? (area.getHeight() - gap) / 2 : area.getHeight();

        if (twoBars)
        {
            drawBar (g, area.getX(), area.getY(),                barW, barH, level[0], peak[0], bg);
            drawBar (g, area.getX(), area.getY() + barH + gap,   barW, barH, level[1], peak[1], bg);
        }
        else
        {
            drawBar (g, area.getX(), area.getY(), barW, area.getHeight(),
                     std::max (level[0], level[1]), std::max (peak[0], peak[1]), bg);
        }
    }

private:
    // Zone boundaries as fractions of the -60..0 dB scale.
    static constexpr float kAmberStart = (-18.0f - kFloorDb) / -kFloorDb;   // 0.70
    static constexpr float kRedStart   = ( -6.0f - kFloorDb) / -kFloorDb;   // 0.90

    static constexpr float kFallPerTick     = 0.60f / (float) kUpdateHz;    // ~36 dB/s
    static constexpr float kPeakFallPerTick = 0.25f / (float) kUpdateHz;    // ~15 dB/s
    static constexpr int   kPeakHoldTicks   = (int) (1.1 * kUpdateHz);
    static constexpr int   kClipHoldTicks   = (int) (1.5 * kUpdateHz);
    static constexpr int   kLiveHoldTicks   = kUpdateHz / 2;

    inline static const juce::Colour kGreen { 0xff2fbf71 };
    inline static const juce::Colour kAmber { 0xffe0a93b };
    inline static const juce::Colour kRed   { 0xffe0504f };

    static float gainToPos (float gain) noexcept
    {
        if (gain <= 0.001f) return 0.0f;                       // <= -60 dB
        const float db = 20.0f * std::log10 (gain);
        return std::min (1.0f, std::max (0.0f, (db - kFloorDb) / -kFloorDb));
    }

    static void drawBar (juce::Graphics& g, int x, int y, int w, int h,
                         float lvl, float pk, juce::Colour bg)
    {
        g.setColour (bg);
        g.fillRect (x, y, w, h);

        const float fillW = lvl * (float) w;
        auto zone = [&] (float from, float to, juce::Colour c)
        {
            const float x0 = from * (float) w;
            const float x1 = std::min (to * (float) w, fillW);
            if (x1 > x0)
            {
                g.setColour (c);
                g.fillRect (juce::Rectangle<float> ((float) x + x0, (float) y, x1 - x0, (float) h));
            }
        };
        zone (0.0f,        kAmberStart, kGreen);
        zone (kAmberStart, kRedStart,   kAmber);
        zone (kRedStart,   1.0f,        kRed);

        // Scale notches at -18 dB and -6 dB (cut into the bar in the background colour).
        if (h >= 3)
        {
            g.setColour (bg);
            g.fillRect (x + (int) (kAmberStart * (float) w), y, 1, h);
            g.fillRect (x + (int) (kRedStart   * (float) w), y, 1, h);
        }

        // Peak-hold tick
        if (pk > 0.0f)
        {
            const int px = x + std::min (w - 2, (int) (pk * (float) w));
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.fillRect (px, y, 2, h);
        }
    }

    float    level[2]    = { 0.0f, 0.0f };
    float    peak[2]     = { 0.0f, 0.0f };
    int      peakHold[2] = { 0, 0 };
    int      clipHold    = 0;
    int      liveHold    = 0;
    uint32_t lastBlocks  = 0;
    bool     stereo      = false;
    bool     active      = false;
};
