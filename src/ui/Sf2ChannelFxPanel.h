#pragma once
// =============================================================================
//  Sf2ChannelFxPanel.h  —  Per-channel (per-preset) SF2 mixer strip
// =============================================================================
//  Displays one column per active FluidSynth channel.  Each column shows:
//    • Preset name (set via setChannelLabel(), from the SF2 program grid)
//    • Volume knob      (0–100 %,  SfzPlayer::ChannelStrip::volume, 0..1)
//    • Pan knob         (-100..+100 %, ChannelStrip::pan, -1..+1)
//    • Reverb-send knob (0–100 %,  ChannelStrip::reverbSend, 0..1)
//    • Mute toggle       (click the label to mute/unmute; muted columns dim)
//
//  Thread safety:
//    This is the real, already audio-thread-wired per-channel API. All knob
//    mutations call SfzPlayer::setChannelVolume/setChannelPan/
//    setChannelReverbSend/setChannelMuted() directly — these write to
//    lock-free atomics (SfzPlayer::ChannelStripAtomics) that are read on the
//    audio thread in SfzPlayer::process(). No pushCommand()/MIDI-learn path
//    exists for per-channel SF2 mixer fields (that path is reserved for the
//    global SliceParamField knobs), so none is invoked here. The panel polls
//    on its Timer (30 Hz) to refresh the display from getChannelStrip().
//
//  Integration:
//    1. Instantiate in PluginEditor (or Sf2InstrumentWorkspace), add as
//       child, lay out below the preset grid / voice controls.
//    2. Call setActiveChannelMask() whenever the set of assigned SF2
//       channels changes (e.g. from Sf2ProgramGrid::onChannelChanged).
//    3. Call setChannelLabel() with the preset name whenever a channel's
//       assignment changes.
//    4. The panel auto-hides when no SF2 file is loaded (isVisible() = false).
// =============================================================================

#include <juce_gui_basics/juce_gui_basics.h>
#include "../PluginProcessor.h"
#include "../audio/SfzPlayer.h"
#include "ThemeData.h"
#include "DysektLookAndFeel.h"
#include "UIHelpers.h"

class Sf2ChannelFxPanel  : public juce::Component,
                            public juce::Timer
{
public:
    explicit Sf2ChannelFxPanel (DysektProcessor& p)
        : processor (p)
    {
        startTimerHz (30);
    }

    ~Sf2ChannelFxPanel() override { stopTimer(); }

    // ── Called from editor / program-grid callbacks ───────────────────────────

    /** Set which FluidSynth channels (0-15) to display.  Pass the bitmask
     *  that matches the currently-assigned SF2 preset channels. */
    void setActiveChannelMask (uint16_t mask)
    {
        if (mask == activeMask) return;
        activeMask = mask;
        repaint();
    }

    /** Update the label shown above channel `ch` (0-15).  Typically the SF2
     *  preset name.  Call from the message thread whenever a new preset is
     *  assigned to a channel. */
    void setChannelLabel (int ch, const juce::String& label)
    {
        if (ch < 0 || ch >= 16) return;
        channelLabels[ch] = label;
        repaint();
    }

    // ── Component ─────────────────────────────────────────────────────────────

    void paint (juce::Graphics& g) override
    {
        // Was UIHelpers::drawTexturedPanel(..., PanelZone::Chassis) — the old
        // wood-grain/gradient chassis look. Sf2ChannelFxPanel only ever
        // appears docked inside Sf2InstrumentWorkspace's flat #080e12 "metro"
        // panel (no other call site — checked), so a textured background
        // here reads as a visual seam between two different UI languages
        // rather than one continuous column 3. Flat fill to match.
        const auto theme = ThemeData::darkTheme();
        g.fillAll (juce::Colour::fromString ("FF080e12"));

        const int numActive = countActiveBits();
        if (numActive == 0)
        {
            g.setColour (theme.foreground.withAlpha (0.4f));
            g.setFont (DysektLookAndFeel::makeFont(14.f));
            g.drawText ("No SF2 channels active", getLocalBounds(), juce::Justification::centred);
            return;
        }

        const float colW = (float) getWidth() / (float) numActive;
        int col = 0;

        for (int ch = 0; ch < 16; ++ch)
        {
            if (! (activeMask & (1u << ch))) continue;

            const float x = (float) col * colW;
            const juce::Rectangle<float> colRect (x, 0.f, colW, (float) getHeight());
            paintChannel (g, ch, colRect, theme);
            ++col;
        }
    }

    void resized() override {}

    void timerCallback() override
    {
        // Show/hide based on whether SF2 is loaded
        const bool shouldShow = processor.sfzPlayer.isLoaded();
        if (shouldShow != isVisible()) setVisible (shouldShow);
        if (! shouldShow) return;
        repaint();
    }

    // ── Mouse interaction ─────────────────────────────────────────────────────

    void mouseDown (const juce::MouseEvent& e) override
    {
        // Click on a channel's label toggles mute.
        for (int ch = 0; ch < 16; ++ch)
        {
            if (! (activeMask & (1u << ch))) continue;
            const auto col = colRectFor (ch);
            const auto labelRect = col.withHeight (kLabelH);
            if (labelRect.contains (e.position))
            {
                const auto strip = processor.sfzPlayer.getChannelStrip (ch);
                processor.sfzPlayer.setChannelMuted (ch, ! strip.muted);
                repaint();
                return;
            }
        }

        dragState = findKnobAt (e.getPosition());
        if (dragState.ch < 0) return;
        dragStartY   = e.getScreenY();
        dragStartVal = getCurrentNorm (dragState.ch, dragState.knob);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragState.ch < 0) return;
        const float delta = (float)(dragStartY - e.getScreenY()) / 120.f;  // px → 0-1
        const float newNorm = juce::jlimit (0.0f, 1.0f, dragStartVal + delta);
        applyNorm (dragState.ch, dragState.knob, newNorm);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragState = {};
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const auto hit = findKnobAt (e.getPosition());
        if (hit.ch < 0) return;
        resetToDefault (hit.ch, hit.knob);
        repaint();
    }

private:
    // ── Knob IDs — mirrors the real SfzPlayer::ChannelStrip fields ────────────
    enum class Knob { None, Volume, Pan, ReverbSend };

    static constexpr float kKnobH    = 72.f;   // px tall per knob row — was 44, too small to
                                                // fit a label + circle + value stack without
                                                // overlap (see paintKnob() fix below)
    static constexpr float kLabelH   = 20.f;   // preset name label
    static constexpr float kPadding  =  4.f;

    struct DragState { int ch { -1 }; Knob knob { Knob::None }; };

    // ── Helpers ───────────────────────────────────────────────────────────────

    int countActiveBits() const noexcept
    {
        int n = 0;
        for (int i = 0; i < 16; ++i) if (activeMask & (1u << i)) ++n;
        return n;
    }

    /** Returns the knob's normalised 0..1 display value from the live
     *  ChannelStrip snapshot.  Pan is remapped from -1..+1 to 0..1. */
    float getCurrentNorm (int ch, Knob k) const noexcept
    {
        const auto strip = processor.sfzPlayer.getChannelStrip (ch);
        switch (k)
        {
            case Knob::Volume:     return juce::jlimit (0.f, 1.f, strip.volume);
            case Knob::Pan:        return juce::jlimit (0.f, 1.f, (strip.pan + 1.0f) * 0.5f);
            case Knob::ReverbSend: return juce::jlimit (0.f, 1.f, strip.reverbSend);
            default:               return 0.f;
        }
    }

    void applyNorm (int ch, Knob k, float norm)
    {
        switch (k)
        {
            case Knob::Volume:     processor.sfzPlayer.setChannelVolume     (ch, norm);                break;
            case Knob::Pan:        processor.sfzPlayer.setChannelPan        (ch, norm * 2.0f - 1.0f);   break;
            case Knob::ReverbSend: processor.sfzPlayer.setChannelReverbSend (ch, norm);                 break;
            default: break;
        }
    }

    void resetToDefault (int ch, Knob k)
    {
        switch (k)
        {
            case Knob::Volume:     applyNorm (ch, k, 1.0f); break;   // unity
            case Knob::Pan:        applyNorm (ch, k, 0.5f); break;   // centre
            case Knob::ReverbSend: applyNorm (ch, k, 0.0f); break;   // dry
            default: break;
        }
    }

    // ── Layout & hit testing ─────────────────────────────────────────────────

    /** Returns the column rect for channel `ch` (0-15) given activeMask. */
    juce::Rectangle<float> colRectFor (int ch) const
    {
        const int numActive = countActiveBits();
        if (numActive == 0) return {};
        const float colW = (float) getWidth() / (float) numActive;
        int col = 0;
        for (int i = 0; i < 16; ++i)
        {
            if (! (activeMask & (1u << i))) continue;
            if (i == ch) return { (float) col * colW, 0.f, colW, (float) getHeight() };
            ++col;
        }
        return {};
    }

    juce::Rectangle<float> knobRect (const juce::Rectangle<float>& col, Knob k) const
    {
        const float y0 = kLabelH + kPadding;
        const float kH = kKnobH;
        const float x  = col.getX() + kPadding;
        const float w  = col.getWidth() - kPadding * 2.f;

        switch (k)
        {
            case Knob::Volume:     return { x, y0,            w, kH };
            case Knob::Pan:        return { x, y0 + kH,       w, kH };
            case Knob::ReverbSend: return { x, y0 + kH * 2.f, w, kH };
            default:               return {};
        }
    }

    DragState findKnobAt (juce::Point<int> pt) const
    {
        const juce::Point<float> fpt = pt.toFloat();
        for (int ch = 0; ch < 16; ++ch)
        {
            if (! (activeMask & (1u << ch))) continue;
            const auto col = colRectFor (ch);
            if (! col.contains (fpt)) continue;
            for (auto k : { Knob::Volume, Knob::Pan, Knob::ReverbSend })
                if (knobRect (col, k).contains (fpt))
                    return { ch, k };
        }
        return {};
    }

    // ── Painting ─────────────────────────────────────────────────────────────

    void paintChannel (juce::Graphics& g,
                       int ch,
                       const juce::Rectangle<float>& col,
                       const ThemeData& theme)
    {
        // Separator
        g.setColour (theme.separator);
        g.drawLine (col.getX(), col.getY(), col.getX(), col.getBottom(), 1.f);

        const auto strip = processor.sfzPlayer.getChannelStrip (ch);

        // Preset label — dimmed when muted, doubles as the mute-toggle target.
        // Shows the actual 1-based MIDI channel number alongside the preset
        // name (not just the name) so it's possible to confirm which channel
        // a controller needs to send on without switching back to the
        // Performance & FX tab's MIDI INPUT readout.
        g.setColour (strip.muted ? theme.foreground.withAlpha (0.35f) : theme.accent);
        g.setFont (DysektLookAndFeel::makeFont(13.f, true));
        const auto labelRect = col.withHeight (kLabelH).reduced (kPadding, 1.f);
        juce::String label = "CH " + juce::String (ch + 1) + "  "
                            + (channelLabels[ch].isEmpty() ? juce::String ("(empty)") : channelLabels[ch]);
        if (strip.muted) label += " (muted)";
        g.drawText (label, labelRect.toNearestInt(), juce::Justification::centredLeft, true);

        // Draw each knob
        paintKnob (g, ch, Knob::Volume,     col, "VOL",  theme, strip.muted);
        paintKnob (g, ch, Knob::Pan,        col, "PAN",  theme, strip.muted);
        paintKnob (g, ch, Knob::ReverbSend, col, "REV",  theme, strip.muted);
    }

    void paintKnob (juce::Graphics& g, int ch, Knob k,
                    const juce::Rectangle<float>& col,
                    const char* label,
                    const ThemeData& theme,
                    bool dimmed)
    {
        const float norm = getCurrentNorm (ch, k);

        // IMPORTANT: kr here is the FULL per-knob cell (label + circle +
        // value), not just the circle. Carving three non-overlapping
        // sub-rects out of it — rather than computing the top label and
        // bottom value positions independently from kr while also sizing
        // the circle from kr's full height — is what guarantees they can
        // never visually collide. (Previously the label/value text and the
        // knob circle all overlapped: kKnobH=44 only left room for either
        // the label+circle+value stack OR the circle alone, not both. See
        // Sf2InstrumentWorkspace::drawKnob for the same fix applied there.)
        auto kr = knobRect (col, k);
        auto topLabel = kr.removeFromTop (14.f);
        auto botLabel = kr.removeFromBottom (16.f);
        const auto& circleZone = kr;   // whatever's left in the middle

        const float cx     = circleZone.getCentreX();
        const float cy     = circleZone.getCentreY();
        const float radius = juce::jmin (circleZone.getWidth(), circleZone.getHeight()) * 0.4f;

        // Track arc
        constexpr float startAngle = juce::MathConstants<float>::pi * 1.2f;
        constexpr float endAngle   = juce::MathConstants<float>::pi * 2.8f;
        const float fillAngle = startAngle + norm * (endAngle - startAngle);

        const float alpha = dimmed ? 0.4f : 1.0f;

        juce::Path trackArc;
        trackArc.addCentredArc (cx, cy, radius, radius, 0.f, startAngle, endAngle, true);
        g.setColour (theme.button.withMultipliedAlpha (alpha));
        g.strokePath (trackArc, juce::PathStrokeType (3.f));

        juce::Path fillArc;
        fillArc.addCentredArc (cx, cy, radius, radius, 0.f, startAngle, fillAngle, true);
        g.setColour (theme.accent.withMultipliedAlpha (alpha));
        g.strokePath (fillArc, juce::PathStrokeType (3.f));

        // Pointer
        const float px = cx + radius * 0.6f * std::sin (fillAngle);
        const float py = cy - radius * 0.6f * std::cos (fillAngle);
        g.setColour (theme.accent.withMultipliedAlpha (alpha));
        g.drawLine (cx, cy, px, py, 1.5f);

        // Label and value — each in its own carved-out row, never the circleZone.
        g.setFont (DysektLookAndFeel::makeFont(11.f));
        g.setColour (theme.foreground.withAlpha (0.6f * alpha));
        g.drawText (label, topLabel.toNearestInt(), juce::Justification::centred);

        juce::String valStr;
        switch (k)
        {
            case Knob::Pan:
            {
                const int pct = juce::roundToInt ((norm * 2.0f - 1.0f) * 100.f);
                valStr = pct == 0 ? juce::String ("C")
                                  : (pct < 0 ? (juce::String (-pct) + "L") : (juce::String (pct) + "R"));
                break;
            }
            default: valStr = juce::String (juce::roundToInt (norm * 100.f)) + "%"; break;
        }

        g.setColour (theme.foreground.withMultipliedAlpha (alpha));
        g.setFont (DysektLookAndFeel::makeFont(13.f, true));
        g.drawText (valStr, botLabel.toNearestInt(), juce::Justification::centred);
    }

    // ── State ─────────────────────────────────────────────────────────────────

    DysektProcessor& processor;
    uint16_t         activeMask  { 0x0000 };   // nothing shown until told otherwise
    juce::String     channelLabels[16];

    DragState dragState;
    int       dragStartY   { 0 };
    float     dragStartVal { 0.f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sf2ChannelFxPanel)
};
