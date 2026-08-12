#pragma once
// =============================================================================
//  Sf2ChannelFxPanel.h  —  Per-channel (per-preset) SF2 mixer strip
// =============================================================================
//  Displays one ROW per active FluidSynth channel. Each row shows:
//    • Preset name (set via setChannelLabel(), from the SF2 program grid)
//    • Volume slider     (0–100 %,  SfzPlayer::ChannelStrip::volume, 0..1)
//    • Pan slider        (-100..+100 %, ChannelStrip::pan, -1..+1)
//    • Reverb-send slider (0–100 %,  ChannelStrip::reverbSend, 0..1)
//    • Select            (click the row's label to select that channel —
//                          mirrors clicking a track in the Arranger: it
//                          highlights the row and, via onChannelSelected,
//                          tells the owning workspace to show that
//                          channel's preset as the active preset. It does
//                          NOT mute/unmute — mute has its own dedicated
//                          box, see below.)
//    • Mute toggle       (click the small mute box at the row's right edge;
//                          muted rows dim)
//
//  REDESIGN NOTE — this used to be a column-per-channel grid (one column per
//  channel, VOL/PAN/REV stacked as knobs inside it). That tied the panel's
//  vertical AND horizontal footprint to how many channels happened to be
//  active at once, which kept producing dead space in one axis or the other
//  no matter how the numbers were tuned:
//    • Few channels + a tall panel  → knobs capped at a max size, leaving a
//      dead gap below them before the next section.
//    • Few channels + a wide panel  → each column stretched to fill/half-
//      fill the panel, with the (fixed-size) knobs just floating centred in
//      a mostly-empty column.
//    • Many channels                → columns had to shrink below a legible
//      size, or the panel had to scroll sideways past a wall of thin strips.
//  A row-per-channel list sidesteps all three: row height is fixed and
//  height simply scales with channel COUNT (2 channels = 2 short rows, not
//  two overstretched columns), width is filled by horizontal sliders that
//  have no awkward "too wide" failure mode, and overflow is a plain
//  vertical scroll — the one list behaviour every user already knows.
//
//  Thread safety:
//    This is the real, already audio-thread-wired per-channel API. All
//    slider mutations call SfzPlayer::setChannelVolume/setChannelPan/
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

    /** Fired when the user clicks a row's label to select that channel
     *  (channel index 0-15) — mirrors clicking a track in the Arranger.
     *  The panel already highlights the row itself (setSelectedChannel());
     *  this callback is how the owning workspace finds out, so it can show
     *  that channel's assigned preset as the active preset the same way
     *  clicking the preset in the list on the left already does. */
    std::function<void (int channel)> onChannelSelected;

    /** Number of currently-active channels — lets the layout that owns this
     *  panel (Sf2InstrumentWorkspace) size its allotted height to match the
     *  actual row count (rowHeight() * this) instead of guessing. */
    int getActiveChannelCount() const noexcept { return countActiveBits(); }

    /** Fixed per-channel row height — see getActiveChannelCount(). */
    static constexpr float rowHeight() noexcept { return kRowH; }

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

        const float contentH = kRowH * (float) numActive;
        clampScroll (contentH);

        g.saveState();
        g.reduceClipRegion (getLocalBounds());

        int row = 0;
        for (int ch = 0; ch < 16; ++ch)
        {
            if (! (activeMask & (1u << ch))) continue;

            const float y = (float) row * kRowH - scrollY;
            if (y + kRowH >= 0.f && y <= (float) getHeight())   // skip fully off-screen rows
            {
                const juce::Rectangle<float> rowRect (0.f, y, (float) getWidth(), kRowH);
                paintChannelRow (g, ch, rowRect, theme);
            }
            ++row;
        }

        g.restoreState();

        // Scroll hint: a thin track+thumb along the right edge, only shown
        // once content is actually taller than the visible panel.
        if (contentH > (float) getHeight())
        {
            const float trackX = (float) getWidth() - 3.f;
            g.setColour (theme.separator.withAlpha (0.5f));
            g.fillRect (trackX, 0.f, 3.f, (float) getHeight());

            const float thumbH = juce::jmax (24.f, (float) getHeight() * (float) getHeight() / contentH);
            const float maxScroll = contentH - (float) getHeight();
            const float thumbY = maxScroll > 0.f ? (scrollY / maxScroll) * ((float) getHeight() - thumbH) : 0.f;
            g.setColour (theme.accent.withAlpha (0.6f));
            g.fillRect (trackX, thumbY, 3.f, thumbH);
        }
    }

    void resized() override { clampScroll (kRowH * (float) countActiveBits()); }

    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        const float contentH = kRowH * (float) countActiveBits();
        if (contentH <= (float) getHeight()) return;   // nothing to scroll

        // Prefer vertical wheel/trackpad delta; fall back to horizontal so a
        // sideways trackpad swipe still scrolls this vertically-laid-out list.
        const float delta = (std::abs (wheel.deltaY) > std::abs (wheel.deltaX) ? wheel.deltaY : wheel.deltaX);
        scrollY -= delta * 240.f;
        clampScroll (contentH);
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
        const auto fpt = e.position;

        // Click on the mute box toggles mute — kept as its own dedicated
        // target so it never fires from a plain row-select click.
        for (int ch = 0; ch < 16; ++ch)
        {
            if (! (activeMask & (1u << ch))) continue;
            const auto row = rowRectFor (ch);
            if (muteZone (row).contains (fpt))
            {
                const auto strip = processor.sfzPlayer.getChannelStrip (ch);
                processor.sfzPlayer.setChannelMuted (ch, ! strip.muted);
                repaint();
                return;
            }
        }

        // Click on a channel's label selects that channel — mirrors
        // clicking a track in the Arranger (selects it, doesn't mute it).
        for (int ch = 0; ch < 16; ++ch)
        {
            if (! (activeMask & (1u << ch))) continue;
            const auto row = rowRectFor (ch);
            if (labelZone (row).contains (fpt))
            {
                setSelectedChannel (ch);
                if (onChannelSelected) onChannelSelected (ch);
                return;
            }
        }

        // Click on a slider track jumps the value there immediately (typical
        // horizontal-slider behaviour) and starts a drag for follow-through.
        dragState = findSliderAt (e.getPosition());
        if (dragState.ch < 0) return;
        applyNorm (dragState.ch, dragState.knob, normForMouseX (dragState.ch, dragState.knob, e.position.x));
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragState.ch < 0) return;
        applyNorm (dragState.ch, dragState.knob, normForMouseX (dragState.ch, dragState.knob, e.position.x));
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragState = {};
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const auto hit = findSliderAt (e.getPosition());
        if (hit.ch < 0) return;
        resetToDefault (hit.ch, hit.knob);
        repaint();
    }

private:
    // ── Slider IDs — mirrors the real SfzPlayer::ChannelStrip fields ────────
    enum class Knob { None, Volume, Pan, ReverbSend };

    static constexpr float kRowH        = 36.f;   // fixed row height — this is what lets the
                                                    // panel's footprint scale with channel COUNT
                                                    // instead of with leftover space
    static constexpr float kAccentW     =  3.f;
    static constexpr float kPadding     =  8.f;
    static constexpr float kMuteW       = 16.f;
    static constexpr float kSliderGap   = 14.f;    // gap between the VOL/PAN/REV blocks
    static constexpr float kSliderLabelW = 28.f;   // "VOL"/"PAN"/"REV" text
    static constexpr float kSliderValueW = 34.f;   // value text, right-aligned
    static constexpr float kTrackH      =  5.f;

    /** Label column width — proportional so it doesn't dominate a narrow
     *  panel, but capped so it doesn't sprawl on a very wide one. */
    float labelW() const noexcept
    {
        return juce::jlimit (80.f, 170.f, (float) getWidth() * 0.20f);
    }

    struct DragState { int ch { -1 }; Knob knob { Knob::None }; };

    // ── Helpers ───────────────────────────────────────────────────────────────

    int countActiveBits() const noexcept
    {
        int n = 0;
        for (int i = 0; i < 16; ++i) if (activeMask & (1u << i)) ++n;
        return n;
    }

    void clampScroll (float contentH) noexcept
    {
        const float maxScroll = juce::jmax (0.f, contentH - (float) getHeight());
        scrollY = juce::jlimit (0.f, maxScroll, scrollY);
    }

    void scrollToChannel (int ch)
    {
        if (! (activeMask & (1u << ch))) return;
        int ordinal = 0;
        for (int i = 0; i < ch; ++i) if (activeMask & (1u << i)) ++ordinal;
        const float top = ordinal * kRowH;
        const float bottom = top + kRowH;
        if (top < scrollY) scrollY = top;
        else if (bottom > scrollY + (float) getHeight()) scrollY = bottom - (float) getHeight();
        clampScroll (kRowH * (float) countActiveBits());
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

    /** Returns the row rect for channel `ch` (0-15) given activeMask. Not
     *  clipped to the visible panel — callers check/clip as needed. */
    juce::Rectangle<float> rowRectFor (int ch) const
    {
        int row = 0;
        for (int i = 0; i < 16; ++i)
        {
            if (! (activeMask & (1u << i))) continue;
            if (i == ch) return { 0.f, (float) row * kRowH - scrollY, (float) getWidth(), kRowH };
            ++row;
        }
        return {};
    }

    juce::Rectangle<float> labelZone (const juce::Rectangle<float>& row) const
    {
        return { row.getX() + kAccentW + kPadding, row.getY(), labelW(), row.getHeight() };
    }

    juce::Rectangle<float> muteZone (const juce::Rectangle<float>& row) const
    {
        return { row.getRight() - kMuteW - kPadding, row.getY() + (row.getHeight() - kMuteW) * 0.5f,
                 kMuteW, kMuteW };
    }

    /** The three VOL/PAN/REV slider blocks fill whatever's left between the
     *  label and the mute box, split evenly — this is what lets the row use
     *  the panel's full width with no "too wide" failure mode. */
    juce::Rectangle<float> sliderBlockRect (const juce::Rectangle<float>& row, Knob k) const
    {
        auto area = row;
        area.removeFromLeft (kAccentW + kPadding + labelW() + kPadding);
        area.removeFromRight (kMuteW + kPadding * 2.f);
        const float blockW = (area.getWidth() - kSliderGap * 2.f) / 3.f;
        switch (k)
        {
            case Knob::Volume:     return area.removeFromLeft (blockW);
            case Knob::Pan:        area.removeFromLeft (blockW + kSliderGap); return area.removeFromLeft (blockW);
            case Knob::ReverbSend: area.removeFromRight (blockW);             return area.removeFromRight (blockW);
            default:                return {};
        }
    }

    /** The draggable track within a slider block (excludes the label/value
     *  text either side). */
    juce::Rectangle<float> trackRect (const juce::Rectangle<float>& row, Knob k) const
    {
        auto block = sliderBlockRect (row, k);
        block.removeFromLeft (kSliderLabelW);
        block.removeFromRight (kSliderValueW);
        return block.withSizeKeepingCentre (block.getWidth(), kTrackH);
    }

    float normForMouseX (int ch, Knob k, float mouseX) const noexcept
    {
        const auto track = trackRect (rowRectFor (ch), k);
        if (track.getWidth() <= 0.f) return 0.f;
        return juce::jlimit (0.f, 1.f, (mouseX - track.getX()) / track.getWidth());
    }

    DragState findSliderAt (juce::Point<int> pt) const
    {
        const juce::Point<float> fpt = pt.toFloat();
        for (int ch = 0; ch < 16; ++ch)
        {
            if (! (activeMask & (1u << ch))) continue;
            const auto row = rowRectFor (ch);
            if (! row.contains (fpt)) continue;
            for (auto k : { Knob::Volume, Knob::Pan, Knob::ReverbSend })
            {
                // Generous vertical hit-box (full row height) around the
                // thin track so it's easy to grab, not just the 5px line.
                auto hitBox = sliderBlockRect (row, k);
                if (hitBox.contains (fpt))
                    return { ch, k };
            }
        }
        return {};
    }

    // ── Painting ─────────────────────────────────────────────────────────────

    void paintChannelRow (juce::Graphics& g,
                           int ch,
                           const juce::Rectangle<float>& row,
                           const ThemeData& theme)
    {
        const auto accent = accentFor (ch, theme);
        const auto strip = processor.sfzPlayer.getChannelStrip (ch);
        const float alpha = strip.muted ? 0.4f : 1.0f;

        // Track-colour tab makes the mixer row match its preset row/track.
        g.setColour (accent.withAlpha (0.85f));
        g.fillRect (row.getX(), row.getY(), kAccentW, row.getHeight());

        // Row separator and selected-channel focus outline.
        g.setColour (theme.separator.withAlpha (0.6f));
        g.drawLine (row.getX(), row.getBottom(), row.getRight(), row.getBottom(), 1.f);
        if (ch == selectedChannel)
        {
            g.setColour (accent.withAlpha (0.95f));
            g.drawRect (row.reduced (1.f), 1.5f);
        }

        // Preset label — dimmed when muted, doubles as the row-select
        // target (see mouseDown()). Shows the actual 1-based MIDI channel
        // number alongside the preset name (not just the name) so it's
        // possible to confirm which channel a controller needs to send on
        // without switching back to the Workspace MIDI INPUT readout.
        g.setColour (strip.muted ? accent.withAlpha (0.35f) : accent);
        g.setFont (DysektLookAndFeel::makeFont(12.f, true));
        juce::String label = "CH " + juce::String (ch + 1) + "  "
                            + (channelLabels[ch].isEmpty() ? juce::String ("(empty)") : channelLabels[ch]);
        g.drawText (label, labelZone (row).reduced (0.f, 1.f).toNearestInt(),
                    juce::Justification::centredLeft, true);

        // Sliders
        paintSlider (g, ch, Knob::Volume,     row, "VOL", theme, accent, strip.muted);
        paintSlider (g, ch, Knob::Pan,        row, "PAN", theme, accent, strip.muted);
        paintSlider (g, ch, Knob::ReverbSend, row, "REV", theme, accent, strip.muted);

        // Mute box — small square indicator, also a click target (see
        // muteZone() / mouseDown()). Filled when muted, outline otherwise.
        const auto mute = muteZone (row);
        if (strip.muted)
        {
            g.setColour (accent.withAlpha (0.7f));
            g.fillRoundedRectangle (mute, 3.f);
        }
        else
        {
            g.setColour (theme.button.withAlpha (0.8f));
            g.drawRoundedRectangle (mute, 3.f, 1.f);
        }
    }

    void paintSlider (juce::Graphics& g, int ch, Knob k,
                       const juce::Rectangle<float>& row,
                       const char* label,
                       const ThemeData& theme,
                       juce::Colour accent,
                       bool dimmed)
    {
        const float norm = getCurrentNorm (ch, k);
        const float alpha = dimmed ? 0.4f : 1.0f;

        auto block = sliderBlockRect (row, k);
        auto labelRect = block.removeFromLeft (kSliderLabelW);
        auto valueRect = block.removeFromRight (kSliderValueW);
        const auto track = block.withSizeKeepingCentre (block.getWidth(), kTrackH);

        // Label
        g.setFont (DysektLookAndFeel::makeFont(9.f));
        g.setColour (theme.foreground.withAlpha (0.55f * alpha));
        g.drawText (label, labelRect.toNearestInt(), juce::Justification::centredLeft);

        // Track background
        g.setColour (theme.button.withMultipliedAlpha (alpha));
        g.fillRoundedRectangle (track, kTrackH * 0.5f);

        // Fill — Pan fills from centre (bipolar), Volume/Reverb fill from
        // the left (unipolar), matching the semantics of each parameter.
        g.setColour (accent.withMultipliedAlpha (alpha));
        if (k == Knob::Pan)
        {
            const float centreX = track.getCentreX();
            const float handleX = track.getX() + norm * track.getWidth();
            g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (centreX, handleX), track.getY(),
                                                              std::abs (handleX - centreX), track.getHeight()),
                                     kTrackH * 0.5f);
        }
        else
        {
            g.fillRoundedRectangle (track.withWidth (track.getWidth() * norm), kTrackH * 0.5f);
        }

        // Handle
        const float handleX = track.getX() + norm * track.getWidth();
        g.setColour (theme.foreground.withMultipliedAlpha (alpha));
        g.fillEllipse (handleX - 3.5f, track.getCentreY() - 3.5f, 7.f, 7.f);

        // Value text
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
        g.setFont (DysektLookAndFeel::makeFont(10.f, true));
        g.setColour (theme.foreground.withMultipliedAlpha (alpha));
        g.drawText (valStr, valueRect.toNearestInt(), juce::Justification::centredRight);
    }

    // ── State ─────────────────────────────────────────────────────────────────

    DysektProcessor& processor;
    uint16_t         activeMask  { 0x0000 };   // nothing shown until told otherwise
    juce::String     channelLabels[16];
    juce::Colour     channelColours[16];
    int              selectedChannel { -1 };
    float            scrollY     { 0.f };      // vertical scroll offset, px

    DragState dragState;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Sf2ChannelFxPanel)
};
