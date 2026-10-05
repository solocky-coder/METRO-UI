#pragma once
#include <algorithm>
#include <set>
#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/SequencerEngine.h"
#include "DysektLookAndFeel.h"
#include "HorizontalLevelMeter.h"
#include "../network/NetworkTrackMeters.h"

//==============================================================================
//  TrackHeaderStrip — vertical list of track headers.
//  Shows: colour swatch | track name | MIDI-RX dot | mute button
//
//  MIDI receive indicator blinks green when the sequencer fires notes on that
//  track.  The timer polls SequencerEngine::getMidiActivityAndClear() at ~10 Hz and
//  Network Audio track peak meters (NetworkTrackMeters) at ~30 Hz.
//==============================================================================
class TrackHeaderStrip : public juce::Component,
                         private juce::Timer
{
public:
    explicit TrackHeaderStrip (SequencerEngine& seq) : engine (seq) { startTimerHz (HorizontalLevelMeter::kUpdateHz); }
    ~TrackHeaderStrip() override { stopTimer(); }

    void setTrackHeight (int h) { trackH = juce::jmax (18, h); repaint(); }
    int  getTrackHeight()       const noexcept { return trackH; }
    int  getSelectedTrack()     const noexcept { return selectedTrack; }
    void setSelectedTrack (int i)
    {
        selectedTrack = i;
        // A selected child must never stay hidden inside a collapsed parent.
        if (i >= 0)
        {
            const auto nest = engine.getTrackNesting();
            if (i < (int) nest.size() && nest[(size_t) i].parentIndex >= 0
                && collapsed.erase (nest[(size_t) i].deviceId) > 0 && onLayoutChanged)
                onLayoutChanged();
        }
        repaint();
    }

    /** Called when rows are shown/hidden, so the timeline can re-layout. */
    std::function<void()> onLayoutChanged;

    // Visible rows. MIDI children of a network audio track can be collapsed under it,
    // so row N is no longer track N. Everything that maps y <-> track goes through these.
    std::vector<int> getVisibleTracks() const { return visibleFrom (engine.getTrackNesting()); }
    int getVisibleTrackCount() const          { return (int) getVisibleTracks().size(); }
    int rowOfTrack (int i) const
    {
        const auto v = getVisibleTracks();
        const auto it = std::find (v.begin(), v.end(), i);
        return it == v.end() ? -1 : (int) (it - v.begin());
    }
    int trackAtRow (int row) const
    {
        const auto v = getVisibleTracks();
        return juce::isPositiveAndBelow (row, (int) v.size()) ? v[(size_t) row] : -1;
    }

    std::function<void(int)>            onTrackSelected;
    std::function<void(int, bool)>      onTrackMuted;
    std::function<void(int, int)>       onSfTrackChannelChanged;  // trackIdx, ch 1-based

    int getRequiredHeight() const { return getVisibleTrackCount() * trackH; }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        const auto& theme = getTheme();
        g.fillAll (theme.waveformBg);
        const auto nest    = engine.getTrackNesting();
        const auto visible = visibleFrom (nest);
        for (int row = 0; row < (int) visible.size(); ++row)
        {
            const int i = visible[(size_t) row];
            const auto info  = engine.getTrackInfo (i);
            const bool isChildRow  = nest[(size_t) i].parentIndex >= 0;
            const bool isParentRow = nest[(size_t) i].childCount > 0;
            const bool collapsedRow = isParentRow && collapsed.count (nest[(size_t) i].deviceId) > 0;
            const int  textShift   = (isChildRow ? kChildIndent : 0) + (isParentRow ? kChevronW : 0);
            // Audio tracks show their own (multi-arm) audio record arm; every other
            // track type shows whether it is the single MIDI recording target.
            const bool recordArmed = (info.type == TrackType::Audio) ? info.audioRecordArm
                                                                     : (i == engine.getRecordingTrackIndex());
            const juce::Rectangle<int> rowR (0, row * trackH, getWidth(), trackH);
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

            // Nesting: MIDI children hang off their audio track with an elbow connector.
            if (isChildRow)
            {
                const bool lastChild = (i + 1 >= (int) nest.size())
                                    || nest[(size_t) i + 1].parentIndex != nest[(size_t) i].parentIndex;
                const int cx = rowR.getX() + 17;
                g.setColour (theme.foreground.withAlpha (0.35f));
                g.fillRect (cx, rowR.getY(), 1, lastChild ? rowR.getHeight() / 2 : rowR.getHeight());
                g.fillRect (cx, rowR.getCentreY(), 12, 1);
            }

            // Collapse arrow on an audio track that has MIDI children.
            if (isParentRow)
            {
                const auto cr = chevronRect (rowR).toFloat().reduced (3.0f);
                juce::Path tri;
                if (collapsedRow)
                    tri.addTriangle (cr.getX(), cr.getY(), cr.getX(), cr.getBottom(), cr.getRight(), cr.getCentreY());
                else
                    tri.addTriangle (cr.getX(), cr.getY(), cr.getRight(), cr.getY(), cr.getCentreX(), cr.getBottom());
                g.setColour (theme.accent);
                g.fillPath (tri);
            }

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
            //
            // Network Audio tracks get a real horizontal peak meter instead (drawn
            // below, under the name), so this MIDI proxy is skipped for them.
            if (info.type != TrackType::Audio)
            {
                const bool rxActive  = (i < kMaxTracks && midiHoldCounters[i] > 0);
                g.setColour (theme.separator);
                g.fillRect (meterR);
                if (rxActive)
                {
                    g.setColour (theme.accent);
                    g.fillRect (meterR.withTrimmedTop (meterR.getHeight() * 3 / 5));
                }
            }

            // Track name — width trimmed to clear the M/S/R + meter cluster
            // on the right (meterR is its leftmost extent, computed above).
            //
            // Always theme.foreground now, not info.colour when selected.
            // Text colour tied to the track's own colour meant certain
            // colours (dark/muted ones, or anything close in luminance to
            // the wash it sits on above) could render close to unreadable —
            // exactly the failure mode this exists to avoid. theme.foreground
            // is guaranteed legible against this UI's dark chrome regardless
            // of which of the 8 track colours is picked; matches
            // TrackInspector's own identity-name text, which never tied its
            // colour to info.colour for the same reason. The wash, left
            // accent bar, and swatch strip above already carry the "this is
            // the track's colour" identity signal — text doesn't need to
            // duplicate that at the cost of readability.
            //
            // Layout: the name sits in its own band at the TOP of the row, centred, in a
            // larger font; the M/S/R + meter cluster keeps the row's middle line below it.
            const int nameTop  = rowR.getY() + 3;
            const int nameBand = juce::jmax (12, (rowR.getCentreY() - btnH / 2) - nameTop - 1);
            const float nameFontH = (float) juce::jmin (nameBand, juce::jlimit (14, 22, (int) ((float) trackH * 0.32f)));
            g.setFont (juce::Font (nameFontH, juce::Font::bold));
            g.setColour (theme.foreground);

            // Network Audio rows put a horizontal stereo/mono peak meter on the middle line
            // (level with M/S/R), under the centred name. Rows too short for it (< 34 px)
            // get a slim 3 px meter along the bottom edge instead.
            const bool audioRow      = (info.type == TrackType::Audio);
            const bool audioMeterRow = audioRow && trackH >= 34;
            constexpr int kAudioMeterH = 8;

            // Children drop the repeated audio-track name; a collapsed parent shows how many it hides.
            juce::String shownName = info.name;
            if (isChildRow)
            {
                const auto parentName = engine.getTrackInfo (nest[(size_t) i].parentIndex).name;
                if (shownName.startsWith (parentName + " "))
                    shownName = shownName.substring (parentName.length() + 1);
            }
            else if (collapsedRow)
                shownName << "  [" << nest[(size_t) i].childCount << "]";

            g.drawText (shownName, rowR.getX() + 14 + textShift, nameTop,
                        rowR.getWidth() - 2 * (14 + textShift), nameBand,
                        juce::Justification::centred, true);

            if (audioRow && i < kMaxTracks)
            {
                int meterX = rowR.getX() + 14 + textShift;
                const int meterRight = muteR.getX() - 10;

                if (audioMeterRow)
                {
                    const int meterY = rowR.getCentreY() - kAudioMeterH / 2;

                    // Small "NET" tag left of the meter when there is room for both.
                    if (meterRight - (meterX + 28) >= 48)
                    {
                        g.setFont (juce::Font (9.5f));
                        g.setColour (theme.foreground.withAlpha (0.55f));
                        g.drawText ("NET", meterX, meterY - 2, 26, kAudioMeterH + 4,
                                    juce::Justification::centredLeft, false);
                        meterX += 28;
                    }
                    netMeters[i].paint (g, { meterX, meterY, meterRight - meterX, kAudioMeterH },
                                        theme.separator);
                }
                else
                {
                    netMeters[i].paint (g, { meterX, rowR.getBottom() - 5, meterRight - meterX, 3 },
                                        theme.separator);
                }
            }

            // Type + channel badge — same readability fix as the name above,
            // theme.foreground at reduced alpha instead of info.colour at
            // reduced alpha, preserving the existing dimmer-than-the-name
            // hierarchy without the colour-contrast risk.
            if (trackH >= 32 && info.type != TrackType::Audio)
            {
                juce::String badge;
                switch (info.type)
                {
                    case TrackType::MainSlice:      badge = "SL"; break;
                    case TrackType::ChromaticSlice: badge = "CH"; break;
                    case TrackType::SfPlayer:       badge = "SF"; break;
                    case TrackType::Audio:          badge = "AU"; break;
                    case TrackType::NetworkMidi:    badge = "NET"; break;
                }
                g.setFont (juce::Font (juce::jlimit (9.0f, 11.0f, (float)trackH * 0.17f)));
                g.setColour (theme.foreground.withAlpha (0.55f));
                g.drawText (badge, rowR.getX() + 14 + textShift, rowR.getCentreY(), 26, trackH / 2,
                            juce::Justification::centredLeft, false);

                if (info.type == TrackType::SfPlayer || info.type == TrackType::ChromaticSlice
                    || info.type == TrackType::NetworkMidi)
                {
                    g.setColour (theme.foreground.withAlpha (0.75f));
                    g.drawText ("CH" + juce::String (info.midiChannel + 1),
                                rowR.getX() + 42 + textShift, rowR.getCentreY(), 46, trackH / 2,
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
        const auto nest    = engine.getTrackNesting();
        const auto visible = visibleFrom (nest);
        const int rowIdx = e.y / trackH;
        if (e.y < 0 || ! juce::isPositiveAndBelow (rowIdx, (int) visible.size())) return;
        const int i = visible[(size_t) rowIdx];
        const auto info = engine.getTrackInfo (i);

        if (e.mods.isRightButtonDown())
        {
            showContextMenu (i, info, e.getScreenPosition());
            return;
        }

        const juce::Rectangle<int> rowR (0, rowIdx * trackH, getWidth(), trackH);

        if (nest[(size_t) i].childCount > 0 && chevronRect (rowR).expanded (4).contains (e.getPosition()))
        {
            toggleCollapsed (nest, i);
            return;
        }

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
            // recordingTrackIndex). This is independent of track *selection*
            // — selecting a different track to inspect it does NOT move the
            // arm (see ArrangeView::selectTrack()), so arming track 3 here
            // and then clicking over to track 1 leaves track 3 armed.
            if (info.type == TrackType::Audio)
                engine.setAudioRecordArm (i, ! info.audioRecordArm);   // multi-arm: other tracks keep their state
            else
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
        const int row = rowOfTrack (i);
        if (row < 0) return {};
        return { 0, row * trackH, getWidth(), trackH };
    }

    static constexpr int kChildIndent = 22;   // extra left inset for nested MIDI children
    static constexpr int kChevronW    = 18;   // room for the collapse arrow on a parent

    static juce::Rectangle<int> chevronRect (juce::Rectangle<int> rowR)
    {
        return { rowR.getX() + 12, rowR.getCentreY() - 8, 16, 16 };
    }

    std::vector<int> visibleFrom (const std::vector<SequencerEngine::TrackNest>& nest) const
    {
        std::vector<int> v;
        v.reserve (nest.size());
        for (int i = 0; i < (int) nest.size(); ++i)
            if (! (nest[(size_t) i].parentIndex >= 0 && collapsed.count (nest[(size_t) i].deviceId) > 0))
                v.push_back (i);
        return v;
    }

    void toggleCollapsed (const std::vector<SequencerEngine::TrackNest>& nest, int parentTrack)
    {
        const int dev = nest[(size_t) parentTrack].deviceId;
        if (collapsed.erase (dev) == 0)
        {
            collapsed.insert (dev);
            // Selection must not stay on a track that just disappeared: move it to the parent.
            if (selectedTrack >= 0 && selectedTrack < (int) nest.size()
                && nest[(size_t) selectedTrack].parentIndex >= 0 && nest[(size_t) selectedTrack].deviceId == dev)
            {
                selectedTrack = parentTrack;
                if (onTrackSelected) onTrackSelected (parentTrack);
            }
        }
        repaint();
        if (onLayoutChanged) onLayoutChanged();
    }

    void timerCallback() override
    {
        const int n = juce::jmin (engine.getNumTracks(), kMaxTracks);
        bool needsRepaint = false;

        // Network Audio level meters: every tick (~30 Hz). Only rows whose meter is
        // (or just stopped being) active are repainted.
        for (int i = 0; i < n; ++i)
            if (netMeters[i].update (NetworkTrackMeters::consume (i)))
                repaint (getRowBounds (i));

        // MIDI-activity proxy meter: kept at its original ~10 Hz cadence.
        if (++midiTickDivider < kMidiTickDivisor)
            return;
        midiTickDivider = 0;

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
    std::set<int> collapsed;   // linked device ids of audio parents whose children are hidden (not saved)

    static constexpr int kMaxTracks = SequencerEngine::kActivityFlagCount;
    static constexpr int kHoldTicks = 3;
    int midiHoldCounters[kMaxTracks] = {};

    // The timer now runs at the meter rate; the MIDI proxy still updates every 3rd tick.
    static constexpr int kMidiTickDivisor = HorizontalLevelMeter::kUpdateHz / 10;
    int midiTickDivider = 0;
    HorizontalLevelMeter netMeters[kMaxTracks];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TrackHeaderStrip)
};
