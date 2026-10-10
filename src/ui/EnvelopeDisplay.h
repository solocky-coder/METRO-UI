#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "DysektLookAndFeel.h"
#include "ZoneEnvelopeRanges.h"
#include <cmath>

//==============================================================================
//  EnvelopeDisplay — shared envelope (ADSR) display helpers used by BOTH
//  SliceControlBar (Slicer) and MultisamplerZoneLcd (Multisampler):
//
//    * formatTime()  — seconds -> human string: "5.0 ms", "100 ms", "1.25 s"
//    * parseTime()   — text-entry counterpart ("20ms", "0.5s", bare number)
//    * drawCurve()   — small live ADSR curve, painted straight into the
//                      caller's Graphics (not a Component, so it can sit
//                      inside the parents' own paint() and never steals a
//                      mouse click from the knob cells around it)
//
//  Segment colours are the same fixed lime / yellow / ice-blue / orange the
//  Slicer's ADSR knobs and waveform nodes already use.
//==============================================================================
namespace EnvDisplay
{
    inline const juce::Colour kAttackColour  { 0xFF00FF87 };   // Toxic Lime
    inline const juce::Colour kDecayColour   { 0xFFFFE800 };   // Radioactive Yellow
    inline const juce::Colour kSustainColour { 0xFF00C8FF };   // Ice Blue
    inline const juce::Colour kReleaseColour { 0xFFFF6B00 };   // Molten Orange

    enum Segment { SegNone = -1, SegAttack = 0, SegDecay, SegSustain, SegRelease };

    inline juce::Colour colourForSegment (int seg)
    {
        switch (seg)
        {
            case SegAttack:  return kAttackColour;
            case SegDecay:   return kDecayColour;
            case SegSustain: return kSustainColour;
            case SegRelease: return kReleaseColour;
            default:         break;
        }
        return juce::Colours::white;
    }

    // < 10 ms -> one decimal ("5.0 ms"); < 1 s -> whole ms ("100 ms");
    // otherwise seconds with two decimals ("1.25 s").
    inline juce::String formatTime (float sec)
    {
        sec = juce::jmax (0.0f, sec);
        if (sec >= 0.9995f)
            return juce::String (sec, 2) + " s";

        const float ms = sec * 1000.0f;
        if (ms < 9.995f)
            return juce::String (ms, 1) + " ms";
        return juce::String (juce::roundToInt (ms)) + " ms";
    }

    // "20ms" / "20 ms" -> 0.02, "0.5s" -> 0.5. A bare number has no unit, so
    // it is read in whatever unit the field was showing when editing started:
    // pass bareIsMs = true while the value on screen was in ms.
    inline float parseTime (const juce::String& text, bool bareIsMs)
    {
        const auto t = text.trim().toLowerCase();
        const bool hasMs = t.endsWith ("ms");
        const bool hasS  = ! hasMs && t.endsWithChar ('s');
        const float v    = t.retainCharacters ("0123456789.-").getFloatValue();

        if (hasMs) return v / 1000.0f;
        if (hasS)  return v;
        return bareIsMs ? v / 1000.0f : v;
    }

    struct CurveParams
    {
        float attackSec, decaySec, sustain, releaseSec;
        float maxAttackSec, maxDecaySec, maxReleaseSec;
    };

    // highlight: Segment to brighten (the knob being dragged), or SegNone.
    // detailed:  false = compact thumbnail (Slicer bar); true = adds node dots
    //            and A/D/S/R letters under the baseline (Multisampler card).
    // scale:     UI scale factor of the caller.
    inline void drawCurve (juce::Graphics& g, juce::Rectangle<float> area, const CurveParams& p,
                           int highlight, bool detailed, float scale,
                           juce::Colour panel, juce::Colour line)
    {
        g.setColour (panel);
        g.fillRoundedRectangle (area, 2.0f * scale);
        g.setColour (line.withAlpha (0.6f));
        g.drawRoundedRectangle (area.reduced (0.5f), 2.0f * scale, 1.0f);

        const float padX   = 5.0f * scale;
        const float padTop = 4.0f * scale;
        const float padBot = (detailed ? 14.0f : 4.0f) * scale;
        const auto plot = area.reduced (padX, 0.0f).withTrimmedTop (padTop).withTrimmedBottom (padBot);
        if (plot.getWidth() < 8.0f || plot.getHeight() < 6.0f)
            return;

        // Segment widths follow the same cube taper as the knobs, so a 5 ms
        // attack is still visibly a (short) attack and the curve moves in step
        // with the knob under the user's hand.
        auto k = [] (float sec, float mx) { return ZoneEnv::timeToKnob (sec, mx); };
        const float wA = 8.0f + 50.0f * k (p.attackSec,  p.maxAttackSec);
        const float wD = 8.0f + 55.0f * k (p.decaySec,   p.maxDecaySec);
        const float wS = 34.0f;
        const float wR = 8.0f + 55.0f * k (p.releaseSec, p.maxReleaseSec);
        const float sc = plot.getWidth() / (wA + wD + wS + wR);

        const float x0 = plot.getX();
        const float x1 = x0 + wA * sc;
        const float x2 = x1 + wD * sc;
        const float x3 = x2 + wS * sc;
        const float x4 = x3 + wR * sc;
        const float yt = plot.getY();
        const float yb = plot.getBottom();
        const float ys = yt + (1.0f - juce::jlimit (0.0f, 1.0f, p.sustain)) * plot.getHeight();

        g.setColour (line);
        g.drawLine (x0, yb, x4, yb, 1.0f);

        juce::Path fill;
        fill.startNewSubPath (x0, yb);
        fill.lineTo (x1, yt);
        fill.lineTo (x2, ys);
        fill.lineTo (x3, ys);
        fill.lineTo (x4, yb);
        fill.closeSubPath();
        g.setColour (kSustainColour.withAlpha (0.09f));
        g.fillPath (fill);

        const float baseW = (detailed ? 2.0f : 1.5f) * scale;
        auto seg = [&] (int idx, float xa, float ya, float xb, float yb2)
        {
            const bool hl = (highlight == idx);
            g.setColour (hl ? juce::Colours::white : colourForSegment (idx).withAlpha (0.95f));
            g.drawLine (xa, ya, xb, yb2, baseW + (hl ? 1.2f * scale : 0.0f));
        };
        seg (SegAttack,  x0, yb, x1, yt);
        seg (SegDecay,   x1, yt, x2, ys);
        seg (SegSustain, x2, ys, x3, ys);
        seg (SegRelease, x3, ys, x4, yb);

        if (! detailed)
            return;

        const float r = 2.5f * scale;
        for (auto pt : { juce::Point<float> (x1, yt), juce::Point<float> (x2, ys), juce::Point<float> (x3, ys) })
        {
            g.setColour (panel);
            g.fillEllipse (pt.x - r, pt.y - r, 2.0f * r, 2.0f * r);
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.drawEllipse (pt.x - r, pt.y - r, 2.0f * r, 2.0f * r, 1.0f);
        }

        g.setFont (DysektLookAndFeel::makeFont (10.5f * scale, true));
        auto letter = [&] (int idx, const char* txt, float xa, float xb)
        {
            g.setColour (highlight == idx ? juce::Colours::white : colourForSegment (idx));
            g.drawText (txt, juce::Rectangle<float> (xa, yb + 1.0f * scale, xb - xa, padBot - 1.0f * scale),
                        juce::Justification::centred, false);
        };
        letter (SegAttack,  "A", x0, x1);
        letter (SegDecay,   "D", x1, x2);
        letter (SegSustain, "S", x2, x3);
        letter (SegRelease, "R", x3, x4);
    }
}
