#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/SequencerEngine.h"
#include "DysektLookAndFeel.h"

//==============================================================================
//  TrackHeaderStrip — vertical list of track headers.
//  Shows: colour swatch | track name | MIDI-RX dot | mute button
//
//  MIDI receive indicator blinks green when the sequencer fires notes on that
//  track.  The 10-Hz timer polls SequencerEngine::getMidiActivityAndClear().
//==============================================================================
class TrackHeaderStrip : public juce::Component,
                         private juce::Timer
{
public:
    explicit TrackHeaderStrip (SequencerEngine& seq) : engine (seq) { startTimerHz (10); }
    ~TrackHeaderStrip() override { stopTimer(); }

    void setTrackHeight (int h) { trackH = juce::jmax (18, h); repaint(); }
    int  getTrackHeight()       const noexcept { return trackH; }
    int  getSelectedTrack()     const noexcept { return selectedTrack; }
    void setSelectedTrack (int i)              { selectedTrack = i; repaint(); }

    std::function<void(int)>            onTrackSelected;
    std::function<void(int, bool)>      onTrackMuted;
    std::function<void(int, int)>       onSfTrackChannelChanged;  // trackIdx, ch 1-based

    int getRequiredHeight() const { return engine.getNumTracks() * trackH; }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        const auto& theme = getTheme();
        g.fillAll (theme.waveformBg);
        const int n = engine.getNumTracks();
        for (int i = 0; i < n; ++i)
        {
            const auto info  = engine.getTrackInfo (i);
            const bool recordArmed = (i == engine.getRecordingTrackIndex());
            const auto rowR  = getRowBounds (i);
            const bool sel   = (i == selectedTrack);

            g.setColour (theme.header.withAlpha (0.92f));
            g.fillRect (rowR);

            // Track-colour wash across the whole row — was previously just
            // the 4px swatch below with a flat dark background otherwise,
            // so the header didn't read as "this track's colour" at a
            // glance the way the coloured clip in the timeline does.
            // Selected rows get a stronger wash so selection still reads
            // clearly, but in the track's own colour rather than the
            // generic app accent.
            //
            // Selected alpha raised 0.30 -> 0.62: composited over
            // theme.header's near-opaque near-black fill just above, 0.30
            // meant the final pixel was roughly 70% black / 30% track
            // colour — nowhere near what the identity swatch and the
            // TRACK COLOUR picker swatches show (both paint info.colour at
            // full opacity, no wash), so the selected row read as a
            // noticeably darker, desaturated version of the colour a user
            // had just picked. 0.62 gets close to a true-colour read while
            // still leaving enough black underneath for the name text (see
            // below, drawn in full-opacity info.colour on top) to sit
            // against without flattening into its own background.
            g.setColour (info.colour.withAlpha (sel ? 0.62f : 0.14f));
            g.fillRect (rowR);

            // Left accent bar on the selected row, now in the track's own
            // colour (was theme.accent) so it matches the wash above.
            if (sel)
            {
                g.setColour (info.colour);
                g.fillRect (rowR.getX(), rowR.getY(), 3, rowR.getHeight());
            }

            g.setColour (info.colour);
            g.fillRect (rowR.withTrimmedLeft (3).withTrimmedRight (rowR.getWidth() - 7).toFloat());

            // M / S / R button trio — mirrors TrackInspector's row, added here
            // so the timeline header carries the same controls per the
            // arranger redesign brief (was mute-only before). Mute now uses
            // the exact same accent (0xffc99140, lit when engaged) and
            // lit-when-active convention TrackInspector's own mute button
            // uses, replacing the previous green-when-unmuted/red-when-muted
            // scheme — that was a second, unrelated colour language for the
            // same control shown in two places. Solo/Record already agreed
            // with TrackInspector's colours exactly (0xFFD1B34C / 0xFFD95454)
            // and are unchanged.
            const int btnW  = juce::jlimit (18, 24, trackH - 10);
            const int btnH  = juce::jlimit (12, 18, trackH - 8);
            const int btnGap = 3;
            const auto recR  = rowR.withTrimmedLeft (rowR.getWidth() - btnW - 4)
                                   .withSizeKeepingCentre (btnW, btnH);
            const auto soloR = recR.translated (-(btnW + btnGap), 0);
            const auto muteR = soloR.translated (-(btnW + btnGap), 0);

            static const juce::Colour kMuteAccent  (0xffc99140);
            static const juce::Colour kSoloAccent  (0xFFD1B34C);
            static const juce::Colour kRecordAccent(0xFFD95454);
            // All three off-states now use the same .darker(0.55f)-of-own-
            // accent convention TrackInspector's configureButton() uses,
            // matching Mute (fixed above already). Solo/Record previously
            // fell back to theme.button (a generic UI grey) when off, which
            // is the far more common state for both — so despite Mute
            // already matching, Solo/Record still looked like a different
            // colour language most of the time. Now all three literally
            // share on/off colours with the inspector's copy.
            g.setColour (info.enabled  ? kMuteAccent.darker (0.55f)   : kMuteAccent);
            g.fillRoundedRectangle (muteR.toFloat(), 0.0f);
            g.setColour (info.solo     ? kSoloAccent                  : kSoloAccent.darker (0.55f));
            g.fillRoundedRectangle (soloR.toFloat(), 0.0f);
            g.setColour (recordArmed   ? kRecordAccent                : kRecordAccent.darker (0.55f));
            g.fillRoundedRectangle (recR.toFloat(), 0.0f);

            g.setColour (juce::Colours::white.withAlpha (0.7f));
            g.setFont (juce::Font (juce::jlimit (10.5f, 16.5f, (float)trackH * 0.22f), juce::Font::bold));
            g.drawText ("M", muteR, juce::Justification::centred, false);
            g.drawText ("S", soloR, juce::Justification::centred, false);
            g.drawText ("R", recR,  juce::Justification::centred, false);

            // Level meter — approximated from MIDI-activity hold counters
            // (no continuous per-track level is plumbed from SequencerEngine
            // yet; this is a discrete on/off proxy, not true peak metering).
            const auto meterR = muteR.withTrimmedLeft (-(btnGap + 5))
                                      .withWidth (3)
                                      .translated (-(muteR.getWidth() + btnGap + 5), 0);
            const bool rxActive  = (i < kMaxTracks && midiHoldCounters[i] > 0);
            g.setColour (theme.separator);
            g.fillRect (meterR);
            if (rxActive)
            {
                g.setColour (theme.accent);
                g.fillRect (meterR.withTrimmedTop (meterR.getHeight() * 3 / 5));
            }

            // Track name — width trimmed to clear the M/S/R + meter cluster
            // on the right (meterR is its leftmost extent, computed above).
            const int reservedRight = rowR.getRight() - meterR.getX() + 6;
            g.setFont (juce::Font (juce::jlimit (12.0f, 16.0f, (float)trackH * 0.25f), juce::Font::bold));
            g.setColour (sel ? info.colour : theme.foreground);
            g.drawText (info.name, rowR.getX() + 14, rowR.getY(),
                        rowR.getWidth() - reservedRight - 14,
                        trackH, juce::Justification::centredLeft, true);

            // Type + channel badge
            if (trackH >= 32)
            {
                juce::String badge;
                switch (info.type)
                {
                    case TrackType::MainSlice:      badge = "SL"; break;
                    case TrackType::ChromaticSlice: badge = "CH"; break;
                    case TrackType::SfPlayer:       badge = "SF"; break;
                }
                g.setFont (juce::Font (juce::jlimit (9.0f, 11.0f, (float)trackH * 0.17f)));
                g.setColour (info.colour.withAlpha (0.6f));
                g.drawText (badge, rowR.getX() + 14, rowR.getCentreY(), 26, trackH / 2,
                            juce::Justification::centredLeft, false);

                if (info.type == TrackType::SfPlayer || info.type == TrackType::ChromaticSlice)
                {
                    g.setColour (info.colour.withAlpha (0.85f));
                    g.drawText ("CH" + juce::String (info.midiChannel + 1),
                                rowR.getX() + 42, rowR.getCentreY(), 46, trackH / 2,
                                juce::Justification::centredLeft, false);
                }
            }

            g.setColour (theme.separator);
            g.fillRect (0, rowR.getBottom() - 1, getWidth(), 1);
        }
    }

    //==========================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int i = e.y / trackH;
        if (! juce::isPositiveAndBelow (i, engine.getNumTracks())) return;
        const auto info = engine.getTrackInfo (i);

        if (e.mods.isRightButtonDown())
        {
            showContextMenu (i, info, e.getScreenPosition());
            return;
        }

        const auto rowR  = getRowBounds (i);
        const int btnW   = juce::jlimit (18, 24, trackH - 10);
        const int btnH   = juce::jlimit (12, 18, trackH - 8);
        const int btnGap = 3;
        const auto recR  = rowR.withTrimmedLeft (rowR.getWidth() - btnW - 4)
                               .withSizeKeepingCentre (btnW, btnH);
        const auto soloR = recR.translated (-(btnW + btnGap), 0);
        const auto muteR = soloR.translated (-(btnW + btnGap), 0);

        if (muteR.contains (e.getPosition()))
        {
            engine.setTrackEnabled (i, ! info.enabled);
            if (onTrackMuted) onTrackMuted (i, ! info.enabled);
        }
        else if (soloR.contains (e.getPosition()))
        {
            engine.setTrackSolo (i, ! info.solo);
        }
        else if (recR.contains (e.getPosition()) && i < kMaxTracks)
        {
            // Toggles the real recording-target track (SequencerEngine's
            // recordingTrackIndex). Note this is also driven by track
            // *selection* elsewhere (see ArrangeView's selection handler),
            // so explicitly arming a track here and then selecting a
            // different one will silently re-arm that one — decoupling
            // "armed" from "selected" would need a change there too.
            engine.setRecordingTrack (i == engine.getRecordingTrackIndex() ? -1 : i);
        }
        else
        {
            selectedTrack = i;
            if (onTrackSelected) onTrackSelected (i);
        }
        repaint();
    }

private:
    void showContextMenu (int idx, const SequencerTrackInfo& info, juce::Point<int> pos)
    {
        if (info.type != TrackType::SfPlayer) return;

        // The real .sfz-instrument track (isSfzInstrument) is a singleton —
        // SequencerEngine::addSfzTrack finds-and-updates it in place rather
        // than ever creating a second one (it can still disappear and
        // reappear via removeSfzTrack()/addSfzTrack() as the MULTISAMPLER
        // instrument's zone count crosses 0, but there's still only ever
        // one at a time) — and sfzPlayer2's routing mask only ever gains
        // channels, never loses its channel-2 default (see
        // PluginProcessor.cpp's per-block sfzPlayer2ChannelMask OR-in). So
        // there both isn't a second track to disambiguate from and no way
        // to actually move sfzPlayer2 off channel 2 via this menu — it can
        // only look like it worked. Multitimbral SF2 preset tracks
        // (isSfzInstrument == false) are the real, multi-instance use case
        // this menu exists for; leave it live for those.
        if (info.isSfzInstrument) return;

        juce::PopupMenu menu;
        menu.addSectionHeader ("MIDI Channel – " + info.name);
        for (int ch = 1; ch <= 16; ++ch)
            menu.addItem (ch, "Channel " + juce::String (ch), true, ch == info.midiChannel + 1);

        const int ti = idx;
        menu.showMenuAsync (
            juce::PopupMenu::Options()
                .withTargetComponent (this)
                .withTargetScreenArea ({ pos.x, pos.y, 1, 1 }),
            [this, ti, info] (int result)
            {
                if (result >= 1 && result <= 16 && info.type == TrackType::SfPlayer)
                    if (onSfTrackChannelChanged) onSfTrackChannelChanged (ti, result);
            });
    }

    juce::Rectangle<int> getRowBounds (int i) const
    {
        return { 0, i * trackH, getWidth(), trackH };
    }

    void timerCallback() override
    {
        const int n = juce::jmin (engine.getNumTracks(), kMaxTracks);
        bool needsRepaint = false;
        for (int i = 0; i < n; ++i)
        {
            if (engine.getMidiActivityAndClear (i))
            {
                midiHoldCounters[i] = kHoldTicks;
                needsRepaint = true;
            }
            else if (midiHoldCounters[i] > 0)
            {
                --midiHoldCounters[i];
                needsRepaint = true;
            }
        }
        if (needsRepaint) repaint();
    }

    SequencerEngine& engine;
    int selectedTrack = 0;
    int trackH        = 64;

    static constexpr int kMaxTracks = SequencerEngine::kActivityFlagCount;
    static constexpr int kHoldTicks = 3;
    int midiHoldCounters[kMaxTracks] = {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeaderStrip)
};
