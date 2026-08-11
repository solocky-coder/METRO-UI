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
//    4. The panel remains visible when no SF2 file is loaded and shows its empty state.
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

    /** Set the Arranger-track colour for channel `ch` (0-15). An invalid
     *  colour restores the standard theme accent. */
    void setChannelColour (int ch, juce::Colour colour)
    {
        if (ch < 0 || ch >= 16) return;
        channelColours[ch] = colour;
        repaint();
    }

    /** Highlight and reveal a channel. Pass -1 to clear the focus. */
    void setSelectedChannel (int ch)
    {
        selectedChannel = (ch >= 0 && ch < 16 && (activeMask & (1u << ch))) ? ch : -1;
        if (selectedChannel >= 0) scrollToChannel (selectedChannel);
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

        // Columns were previously squeezed to fit the panel's fixed width
        // (colW = getWidth()/numActive) with no floor, so anything past
        // ~5-6 simultaneously-active channels shrank below a usable size —
        // knob circles and labels became illegible slivers, which reads as
        // "channels just aren't showing" even though they were technically
        // still being painted. Give every column a minimum width instead,
        // and let the panel scroll horizontally (mouse wheel / trackpad)
        // once more channels are active than fit at that minimum width.
        const float colW = columnWidth (numActive);
        clampScroll (colW * (float) numActive);

        int col = 0;
        for (int ch = 0; ch < 16; ++ch)
        {
            if (! (activeMask & (1u << ch))) continue;

            const float x = (float) col * colW - scrollX;
            if (x + colW >= 0.f && x <= (float) getWidth())   // skip fully off-screen columns
            {
                const juce::Rectangle<float> colRect (x, 0.f, colW, (float) getHeight());
                paintChannel (g, ch, colRect, theme);
            }
            ++col;
        }

        // Scroll hint: a thin track+thumb along the bottom edge, only shown
        // once content is actually wider than the visible panel.
        const float contentW = colW * (float) numActive;
        if (contentW > (float) getWidth())
        {
            const float trackY = (float) getHeight() - 3.f;
            g.setColour (theme.separator.withAlpha (0.5f));
            g.fillRect (0.f, trackY, (float) getWidth(), 3.f);

            const float thumbW = juce::jmax (24.f, (float) getWidth() * (float) getWidth() / contentW);
            const float maxScroll = contentW - (float) getWidth();
            const float thumbX = maxScroll > 0.f ? (scrollX / maxScroll) * ((float) getWidth() - thumbW) : 0.f;
            g.setColour (theme.accent.withAlpha (0.6f));
            g.fillRect (thumbX, trackY, thumbW, 3.f);
        }
    }

    void resized() override { clampScroll (columnWidth (countActiveBits()) * (float) countActiveBits()); }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const int numActive = countActiveBits();
        const float cw = columnWidth (numActive) * (float) numActive;
        if (cw <= (float) getWidth()) return;   // nothing to scroll

        // Prefer horizontal wheel/trackpad delta; fall back to vertical so a
        // plain mouse wheel still scrolls this horizontally-laid-out panel.
        const float delta = (std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY);
        scrollX -= delta * 240.f;
        clampScroll (cw);
        repaint();
    }

    void timerCallback() override
    {
        // The workspace keeps this mixer visible even before a SoundFont is
        // loaded, where paint() presents its empty-state message.
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
            const auto labelRect = col.withHeight (labelH());
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

    // kKnobH/kLabelH used to be hard pixel constants (72 / 20 — three knob
    // rows + the preset label add up to 236px of *required* content). That
    // was fine as long as whoever laid this panel out always handed it
    // ≥236px of height — but nothing enforced that, and Sf2InstrumentWorkspace
    // ended up doing exactly the opposite: after its own fixed-height
    // sections (knobs, filter/reverb, note meter, keyboard) ate the
    // available column height, this panel could be handed a rect only a
    // few px tall. Since paintChannel()/knobRect() drew at the fixed 72/20
    // sizes regardless, most (or all) of the three knob rows landed outside
    // the panel's actual bounds and were clipped away — a fully-populated
    // mixer with real assigned channels, rendering as nothing visible.
    // kKnobH/kLabelH are now *derived from the panel's own current height*
    // instead, clamped between a legible floor and the original comfortable
    // ceiling, so the strip always fits in whatever height it's actually
    // given — shrinking gracefully under pressure rather than clipping.
    static constexpr float kKnobHMax   = 72.f;
    static constexpr float kKnobHMin   = 40.f;   // still shows a full arc + label + value
    static constexpr float kLabelHMax  = 20.f;
    static constexpr float kLabelHMin  = 14.f;
    static constexpr float kPadding    =  4.f;
    static constexpr float kMinColW    = 96.f;   // floor width so knobs/labels stay legible —
                                                  // panel scrolls horizontally past this instead
                                                  // of shrinking columns further

    /** Preset-label row height for the current panel height. */
    float labelH() const noexcept
    {
        return juce::jlimit (kLabelHMin, kLabelHMax, (float) getHeight() * 0.14f);
    }

    /** Per-knob row height for the current panel height — whatever's left
     *  after the label row and inter-row padding, split three ways, clamped
     *  to a legible floor. This is what lets three knob rows actually fit
     *  (and stay visible) inside however much height the workspace hands
     *  this panel, instead of assuming a fixed 236px is always available. */
    float knobH() const noexcept
    {
        const float remaining = (float) getHeight() - labelH() - kPadding;
        return juce::jlimit (kKnobHMin, kKnobHMax, remaining / 3.f);
    }

    struct DragState { int ch { -1 }; Knob knob { Knob::None }; };

    // ── Helpers ───────────────────────────────────────────────────────────────

    int countActiveBits() const noexcept
    {
        int n = 0;
        for (int i = 0; i < 16; ++i) if (activeMask & (1u << i)) ++n;
        return n;
    }

    /** Column width: divide the panel evenly when few channels are active,
     *  but never shrink below kMinColW — once numActive would need less
     *  than that, the panel scrolls instead. */
    float columnWidth (int numActive) const noexcept
    {
        if (numActive <= 0) return kMinColW;
        return juce::jmax ((float) getWidth() / (float) numActive, kMinColW);
    }

    void clampScroll (float contentW) noexcept
    {
        const float maxScroll = juce::jmax (0.f, contentW - (float) getWidth());
        scrollX = juce::jlimit (0.f, maxScroll, scrollX);
    }

    void scrollToChannel (int ch)
    {
        if (! (activeMask & (1u << ch))) return;
        const int numActive = countActiveBits();
        const float colW = columnWidth (numActive);
        int ordinal = 0;
        for (int i = 0; i < ch; ++i) if (activeMask & (1u << i)) ++ordinal;
        const float left = ordinal * colW;
        const float right = left + colW;
        if (left < scrollX) scrollX = left;
        else if (right > scrollX + (float) getWidth()) scrollX = right - (float) getWidth();
        clampScroll (colW * (float) numActive);
    }

    juce::Colour accentFor (int ch, const ThemeData& theme) const
    {
        return channelColours[ch].isTransparent() ? theme.accent : channelColours[ch];
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
        const float colW = columnWidth (numActive);
        int col = 0;
        for (int i = 0; i < 16; ++i)
        {
            if (! (activeMask & (1u << i))) continue;
            if (i == ch) return { (float) col * colW - scrollX, 0.f, colW, (float) getHeight() };
            ++col;
        }
        return {};
    }

    juce::Rectangle<float> knobRect (const juce::Rectangle<float>& col, Knob k) const
    {
        const float y0 = labelH() + kPadding;
        const float kH = knobH();
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
        const auto accent = accentFor (ch, theme);

        // Track-colour tab makes the mixer column match its preset row/track.
        g.setColour (accent.withAlpha (0.85f));
        g.fillRect (col.getX(), col.getY(), 3.f, col.getHeight());

        // Separator and selected-channel focus outline.
        g.setColour (theme.separator);
        g.drawLine (col.getX(), col.getY(), col.getX(), col.getBottom(), 1.f);
        if (ch == selectedChannel)
        {
            g.setColour (accent.withAlpha (0.95f));
            g.drawRect (col.reduced (1.f), 2.f);
        }

        const auto strip = processor.sfzPlayer.getChannelStrip (ch);

        // Preset label — dimmed when muted, doubles as the mute-toggle target.
        // Shows the actual 1-based MIDI channel number alongside the preset
        // name (not just the name) so it's possible to confirm which channel
        // a controller needs to send on without switching back to the
        // Workspace MIDI INPUT readout.
        g.setColour (strip.muted ? accent.withAlpha (0.35f) : accent);
        g.setFont (DysektLookAndFeel::makeFont(13.f, true));
        const auto labelRect = col.withHeight (labelH()).reduced (kPadding, 1.f);
        juce::String label = "CH " + juce::String (ch + 1) + "  "
                            + (channelLabels[ch].isEmpty() ? juce::String ("(empty)") : channelLabels[ch]);
        if (strip.muted) label += " (muted)";
        g.drawText (label, labelRect.toNearestInt(), juce::Justification::centredLeft, true);

        // Draw each knob
        paintKnob (g, ch, Knob::Volume,     col, "VOL",  theme, accent, strip.muted);
        paintKnob (g, ch, Knob::Pan,        col, "PAN",  theme, accent, strip.muted);
        paintKnob (g, ch, Knob::ReverbSend, col, "REV",  theme, accent, strip.muted);
    }

    void paintKnob (juce::Graphics& g, int ch, Knob k,
                    const juce::Rectangle<float>& col,
                    const char* label,
                    const ThemeData& theme,
                    juce::Colour accent,
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
        // Text row heights scale down alongside the knob cell itself so the
        // circle always keeps a usable share of kr — at kKnobHMin (34px) a
        // fixed 14/16 label pair would leave only ~4px for the circle.
        const float textRowH = juce::jmax (9.f, kr.getHeight() * 0.19f);
        auto topLabel = kr.removeFromTop (textRowH);
        auto botLabel = kr.removeFromBottom (textRowH + 2.f);
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
        g.setColour (accent.withMultipliedAlpha (alpha));
        g.strokePath (fillArc, juce::PathStrokeType (3.f));

        // Pointer
        const float px = cx + radius * 0.6f * std::sin (fillAngle);
        const float py = cy - radius * 0.6f * std::cos (fillAngle);
        g.setColour (accent.withMultipliedAlpha (alpha));
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
    juce::Colour     channelColours[16];
    int              selectedChannel { -1 };
    float            scrollX     { 0.f };      // horizontal scroll offset, px

    DragState dragState;
    int       dragStartY   { 0 };
    float     dragStartVal { 0.f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sf2ChannelFxPanel)
};
