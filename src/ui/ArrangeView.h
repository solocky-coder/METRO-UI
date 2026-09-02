#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "FloatingTransportBar.h"
#include "TrackHeaderStrip.h"
#include "TrackInspector.h"
#include "DysektLookAndFeel.h"
#include "../sequencer/SequencerEngine.h"
#include "../sequencer/MidiClip.h"
#include "ToolIcons.h"
#include "ZoomableScrollBar.h"
#include <limits>
#include <algorithm>
#include <vector>
#include <utility>

//==============================================================================
//  ArrangeView  —  Cubase-style arrange window
//
//  Ruler:
//   Left-click / drag         → seek + scrub playhead
//   Alt + drag                → set loop in/out (L/R markers)
//   Right-click               → clear loop markers
//
//  Clip body:
//   Left-click                → select track
//   Drag                      → move clip (ghost preview, committed on mouseUp)
//   Drag right edge (8 px)    → resize clip length
//   Double-click              → open piano roll
//   Right-click               → context menu
//
//  Empty space:
//   Left-click                → deselect
//   Right-click               → track context menu
//
//  Keyboard:
//   Space                     → play / stop toggle
//   Home / Numpad 0           → rewind to bar 1
//   +/-                       → increase / decrease track height
//   Delete                    → clear selected clip contents
//
//  Scroll / zoom:
//   Ctrl + scroll             → horizontal zoom (centred on mouse)
//   Shift + scroll            → fast horizontal scroll
//   Scroll                    → horizontal scroll
//   Vertical scrollbar        → track rows scroll (when > screen height)
//
//  Auto-scroll:
//   Playhead auto-follows during playback (Cubase-style page scroll)
//==============================================================================
class ArrangeView : public juce::Component,
                    private juce::Timer,
                    private juce::ScrollBar::Listener
{
public:
    /** Clip-grid tool, selected via the clip right-click "Tool" submenu or the
     *  S/D/E/K/G shortcuts — same names/keys as PianoRollComponent::Tool, but
     *  operating on whole clips rather than notes:
     *   Select — default: click to select/move a clip, drag right edge to resize,
     *            click empty space to create a clip (unchanged pre-existing behaviour)
     *   Draw   — click empty space creates a clip; click an existing clip just selects it
     *   Erase  — click a clip deletes it
     *   Split  — click a clip cuts it into two clips at the click point
     *   Glue   — click a clip merges it with the next clip on the same track
     */
    enum class Tool { Select, Draw, Erase, Split, Glue };

    static constexpr int kTransportH   = 88;   // title strip (24) + gap (16) +
                                                // content row (40) + bottom gutter (8).
                                                // The gutter keeps child controls and
                                                // their expanded frames inside the
                                                // transport surface instead of sitting
                                                // on the arranger boundary.
    static constexpr int kInspectorW   = 216;
    static constexpr int kStripW       = 196;
    static constexpr int kLeftW        = kInspectorW + kStripW;
    static constexpr int kRulerH       = 32;
    static constexpr int kScrollH      = 10;
    static constexpr int kScrollW      = 10;
        static constexpr int kMinClipPx    = 6;
    static constexpr int kResizeZone   = 10;
    static constexpr int kDefaultTrackH = 64;
    static constexpr int kMinTrackH    = 28;
    static constexpr int kMaxTrackH    = 140;

    /** Owner wires this to open the piano roll for the given track + clip. */
    std::function<void(int trackIndex, int clipIndex)> onClipDoubleClicked;

    /** Fired whenever the selected track changes.
     *  @param type          Track type of the newly selected track.
     *  @param hasSelection  false when nothing is selected (deselect / empty view).
     *  @param isSfzInstrument  Only meaningful when type == SfPlayer && hasSelection;
     *                          true if the track is a real .sfz-file instrument track
     *                          rather than an SF2 preset track — see
     *                          SequencerTrack::isSfzInstrument. Lets listeners tell
     *                          the SFZ-PLAYER and SF2-PLAYER tabs apart.
     *  @param midiChannel1Based  Only meaningful when type == SfPlayer && hasSelection
     *                          && !isSfzInstrument (a genuine SF2 preset track): the
     *                          track's assigned MIDI channel, 1-based. -1 otherwise.
     *  @param presetBank / @param presetProgram  Same guard as midiChannel1Based.
     *                          The track's own SequencerTrackInfo::preset (bank/
     *                          program) — the actual authoritative preset<->track
     *                          link, set wherever the track's preset was originally
     *                          assigned (e.g. TrackInspector's PART dropdown via
     *                          addOrUpdateSfTrackOnChannel()). NOT the same as
     *                          Sf2ProgramGrid::getPresetChannels(), which only tracks
     *                          assignments made through the SF2-PLAYER panel's own
     *                          right-click menu and is a completely separate map —
     *                          a track assigned via the Arranger never appears there.
     *                          -1/-1 when not applicable. */
    std::function<void(TrackType type, bool hasSelection, bool isSfzInstrument,
                        int midiChannel1Based, int presetBank, int presetProgram)> onTrackTypeSelected;

    /** Fired whenever a track's mute (enabled) state changes via the
     *  track-header strip's M button, so the owner can mirror it onto
     *  whatever else tracks mute state for that track — e.g. the SF2
     *  internal mixer's per-channel mute badge. muted == true means the
     *  track was just disabled (i.e. the new enabled state is false). */
    std::function<void(int trackIndex, bool muted)> onTrackMutedForSync;

    /** Selects the arranger track whose SfPlayer MIDI channel (0-based)
     *  matches, and scrolls/highlights it exactly as a direct click on the
     *  track header would. No-op if no such track exists. Called by the SF2
     *  internal mixer so clicking a channel strip there focuses the
     *  matching arranger track. */
    void selectTrackForSfChannel (int midiChannel0Based)
    {
        const int idx = engine.findSfTrackForChannel (midiChannel0Based);
        if (idx < 0) return;
        selectTrack (idx);
        trackStrip.setSelectedTrack (idx);
        repaint();
    }

    /** Re-fires onTrackTypeSelected for the currently selected track.
     *  Call this when opening the sequencer panel so the editor can
     *  apply the correct MIDI route mode for whatever track is already selected. */
    void notifyCurrentTrack()
    {
        if (onTrackTypeSelected)
        {
            if (juce::isPositiveAndBelow (selectedTrack, engine.getNumTracks()))
            {
                const auto info = engine.getTrackInfo (selectedTrack);
                const bool isSf2Track = info.type == TrackType::SfPlayer && ! info.isSfzInstrument;
                const int ch1Based = (info.type == TrackType::SfPlayer
                                       && info.midiChannel >= 0 && info.midiChannel < 16)
                                    ? info.midiChannel + 1 : -1;
                onTrackTypeSelected (info.type, true, info.isSfzInstrument, ch1Based,
                                      isSf2Track ? info.preset.bank   : -1,
                                      isSf2Track ? info.preset.preset : -1);
            }
            else
                onTrackTypeSelected (TrackType::MainSlice, false, false, -1, -1, -1);
        }
    }

    //==========================================================================
    ArrangeView (SequencerEngine& seq, AbletonLink* link = nullptr)
        : engine (seq),
          linkPtr (link),
          transport (seq, link),
          trackStrip (seq),
          inspector (seq)
    {
        transport.setDocked (true);
        addAndMakeVisible (transport);
        addAndMakeVisible (inspector);
        addAndMakeVisible (trackStrip);

        transport.onFloatRequested = [this] { showFloatingTransport(); };
        transport.onDockRequested  = [this] { dockTransport(); };
        transport.onMixerRequested    = [this] { if (onMixerRequested)   onMixerRequested(); };
        transport.onArrangerRequested = [this] { if (onArrangerRequested) onArrangerRequested(); };
        transport.onGlobalEqRequested = [this] { if (onGlobalEqRequested) onGlobalEqRequested(); };

        // ── Horizontal scrollbar ──────────────────────────────────────────────
        hScroll.setRangeLimits (0.0, 1.0);
        hScroll.setCurrentRange (0.0, 0.5);
        hScroll.setAutoHide (false);
        styleScrollBar (hScroll);
        hScroll.addListener (this);
        addAndMakeVisible (hScroll);

        // Drag either end of the thumb to zoom horizontally (pixelsPerTick),
        // anchored on the opposite edge of the currently-visible range —
        // replaces the old "ARRANGEMENT OVERVIEW" minimap strip.
        hScroll.minScale = 0.003;
        hScroll.maxScale = 0.4;
        hScroll.getScale = [this] { return pixelsPerTick; };
        hScroll.applyZoom = [this] (bool draggingStartEdge, double newScale)
        {
            const double viewW = (double) clipGridBounds.getWidth();
            if (draggingStartEdge)
            {
                // Left edge of the thumb dragged — keep the tick currently
                // at the RIGHT edge of the view fixed on screen.
                const double anchorTick = (scrollX + viewW) / pixelsPerTick;
                pixelsPerTick = newScale;
                scrollX = juce::jmax (0.0, anchorTick * pixelsPerTick - viewW);
            }
            else
            {
                // Right edge dragged — keep the LEFT edge fixed.
                const double anchorTick = scrollX / pixelsPerTick;
                pixelsPerTick = newScale;
                scrollX = juce::jmax (0.0, anchorTick * pixelsPerTick);
            }
            updateScrollRanges();
            repaint();
        };

        // ── Vertical scrollbar ────────────────────────────────────────────────
        vScroll.setRangeLimits (0.0, 1.0);
        vScroll.setCurrentRange (0.0, 1.0);
        vScroll.setAutoHide (false);
        styleScrollBar (vScroll);
        vScroll.addListener (this);
        addAndMakeVisible (vScroll);

        // Same idea vertically: drag either end of the thumb to zoom track
        // height (trackH), anchored on the opposite edge.
        vScroll.minScale = (double) kMinTrackH;
        vScroll.maxScale = (double) kMaxTrackH;
        vScroll.getScale = [this] { return (double) trackH; };
        vScroll.applyZoom = [this] (bool draggingStartEdge, double newScale)
        {
            const int viewH = clipGridBounds.getHeight();
            const int newTrackH = juce::jlimit (kMinTrackH, kMaxTrackH, (int) juce::roundToInt (newScale));
            if (draggingStartEdge)
            {
                // Top edge of the thumb dragged — keep the BOTTOM edge fixed.
                const double anchorTrackPos = (double) (scrollY + viewH) / (double) trackH;
                trackH = newTrackH;
                scrollY = juce::jmax (0, (int) (anchorTrackPos * trackH) - viewH);
            }
            else
            {
                // Bottom edge dragged — keep the TOP edge fixed.
                const double anchorTrackPos = (double) scrollY / (double) trackH;
                trackH = newTrackH;
                scrollY = juce::jmax (0, (int) (anchorTrackPos * trackH));
            }
            trackStrip.setTrackHeight (trackH);
            updateScrollRanges();
            trackStrip.repaint();
            repaint();
        };

        // ── Track-strip callbacks ─────────────────────────────────────────────
        trackStrip.onTrackSelected = [this] (int idx)
        {
            selectTrack (idx);
            repaint();
        };
        trackStrip.onTrackMuted = [this] (int idx, bool newEnabled)
        {
            // TrackHeaderStrip::onTrackMuted's bool is the track's *new*
            // enabled state, not "is now muted" — invert it here so
            // onTrackMutedForSync's contract (muted == true means the
            // track was just disabled) is unambiguous for listeners.
            if (onTrackMutedForSync) onTrackMutedForSync (idx, ! newEnabled);
            repaint();
        };

        setWantsKeyboardFocus (true);
        startTimerHz (30);
    }

    ~ArrangeView() override
    {
        hScroll.removeListener (this);
        vScroll.removeListener (this);
        stopTimer();
        if (transport.isFloating())
            transport.hide();
    }

    //==========================================================================
    void resized() override
    {
        auto r = getLocalBounds().reduced (3);
        if (! transport.isFloating())
            transport.setBounds (r.removeFromTop (kTransportH));

        // Corner square between the two scrollbars
        auto cornerR = r;
        cornerR = cornerR.removeFromBottom (kScrollH).removeFromRight (kScrollW);

        auto hScrollR = r.removeFromBottom (kScrollH).withTrimmedRight (kScrollW);
        auto vScrollR = r.removeFromRight  (kScrollW);

        hScroll.setBounds (hScrollR.withTrimmedLeft (kLeftW));
        vScroll.setBounds (vScrollR);

        auto inspectorCol = r.removeFromLeft (kInspectorW);
        inspectorCol.removeFromTop (kRulerH);
        inspector.setBounds (inspectorCol);

        auto leftCol = r.removeFromLeft (kStripW);
        leftCol.removeFromTop (kRulerH);
        trackStrip.setBounds (leftCol);
        trackStrip.setTrackHeight (trackH);

        gridArea      = r;
        rulerBounds   = gridArea.removeFromTop (kRulerH);
        clipGridBounds = gridArea;

        updateScrollRanges();
    }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = getTheme();
        auto b = getLocalBounds();

        // ── LCD-style frame — flat, square-cornered, no gradient, no glow ────
        g.setColour (theme.waveformBg);
        g.fillRoundedRectangle (b.toFloat(), 0.0f);
        g.setColour (theme.separator);
        g.drawRoundedRectangle (b.toFloat().reduced (0.5f), 0.0f, 1.0f);

        // Clip all track content to the inner screen rect
        g.saveState();
        g.reduceClipRegion (b.reduced (3));

        paintArrangeHeader (g);
        paintRuler (g);
        paintTrackRows (g);
        paintLoopOverlay (g);
        paintPlayhead (g);
        paintRubberBand (g);
        paintDrawClipPreview (g);

        // Corner fill between scrollbars
        if (getWidth() > kLeftW + 8 && getHeight() > kTransportH + kScrollH + 8)
        {
            g.setColour (theme.waveformBg);
            g.fillRect (getWidth() - kScrollW - 4,
                        getHeight() - kScrollH - 4,
                        kScrollW, kScrollH);
        }

        g.restoreState();
    }

    //==========================================================================
    //  Mouse
    //==========================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        updateCursor (e);
        updateHoverHandle (e.getPosition());
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        setHoverHandle (-1, -1);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();

        // ── Ruler ─────────────────────────────────────────────────────────────
        if (rulerBounds.contains (e.getPosition()))
        {
            if (e.mods.isRightButtonDown())
            {
                // Reset to the engine's implicit default (whole-arrangement)
                // range rather than just clearing the local overlay — with
                // timerCallback() now syncing loopStart/loopEnd from the
                // engine every tick, a purely local reset would get
                // immediately overwritten by the still-configured engine
                // range on the very next repaint.
                engine.setLoopRange (0, engine.getLengthTicks());
                loopStart = engine.getLoopStartTick();
                loopEnd   = engine.getLoopEndTick();
                repaint(); return;
            }

            // Grab an existing loop marker directly, so it can be dragged to
            // a new position instead of only being replaceable by drawing a
            // whole new region with Alt-drag.
            if (loopStart >= 0 && loopEnd > loopStart)
            {
                if (isNearLoopMarker (e.x, loopStart))
                { rulerDrag = RulerDrag::DragLoopStart; return; }
                if (isNearLoopMarker (e.x, loopEnd))
                { rulerDrag = RulerDrag::DragLoopEnd; return; }
            }

            const int64_t tick = xToTick (e.x);
            if (e.mods.isAltDown())
            {
                loopStart = tick; loopEnd = tick;
                rulerDrag = RulerDrag::LoopSet;
                loopDragAnchor = tick;
            }
            else
            {
                engine.seekToTick (juce::jmax ((int64_t)0, tick));
                rulerDrag = RulerDrag::Scrub;
            }
            repaint(); return;
        }

        // ── Clip grid ─────────────────────────────────────────────────────────
        if (! clipGridBounds.contains (e.getPosition())) return;

        const int trackIdx = trackFromY (e.y);
        const bool validTrack = juce::isPositiveAndBelow (trackIdx, engine.getNumTracks());

        // Hit test all clips on this track (none if the click is below the
        // last track row — that's still valid space to start a rubber-band
        // drag from, it just can't hit a clip or fall back to creating one).
        int hitClip = -1;
        juce::Rectangle<int> hitRect;
        if (validTrack)
        {
            const int numClips = engine.getNumClips (trackIdx);
            for (int ci = 0; ci < numClips; ++ci)
            {
                const auto r = clipRectForClip (trackIdx, ci);
                if (r.contains (e.getPosition())) { hitClip = ci; hitRect = r; break; }
            }
        }
        const bool onClip = (hitClip >= 0);

        if (e.mods.isRightButtonDown())
        {
            showContextMenu (trackIdx, hitClip, e);
            return;
        }

        // Non-Select tools act on a single left-click instead of the
        // Select tool's move/resize/rubber-band-select behaviour below.
        // They all need a real track under the cursor.
        if (currentTool != Tool::Select)
        {
            if (validTrack)
            {
                switch (currentTool)
                {
                    case Tool::Draw:
                        if (onClip) { selectSingleClip (trackIdx, hitClip); }
                        else        handleDrawClipDown (trackIdx, e);
                        break;
                    case Tool::Erase: if (onClip) handleEraseClipDown (trackIdx, hitClip); break;
                    case Tool::Split: if (onClip) handleSplitClipDown (trackIdx, hitClip, e); break;
                    case Tool::Glue:  if (onClip) handleGlueClipDown  (trackIdx, hitClip); break;
                    default: break;
                }
            }
            repaint(); trackStrip.repaint();
            return;
        }

        // Resize handle — right edge of a clip
        if (onClip && e.x >= hitRect.getRight() - kResizeZone)
        {
            beginClipSelection (trackIdx, hitClip, e.mods.isShiftDown());
            dragMode       = DragMode::ResizeRight;
            dragTrack      = trackIdx;
            dragClip       = hitClip;
            dragStartX     = e.x;
            dragStartTicks = engine.getClipInfo (trackIdx, hitClip).lengthTicks;
            dragResizeLen  = dragStartTicks;
            updateCursor (e);
            repaint(); return;
        }

        // Clip body — move (whole selection moves together if this clip is
        // part of a multi-clip selection; see clipRectForClip)
        if (onClip)
        {
            beginClipSelection (trackIdx, hitClip, e.mods.isShiftDown());
            dragMode       = DragMode::MoveClip;
            dragTrack      = trackIdx;
            dragClip       = hitClip;
            dragStartX     = e.x;
            dragStartTicks = engine.getClipInfo (trackIdx, hitClip).startTick;
            dragLiveOffset = dragStartTicks;
            updateCursor (e);
            repaint(); return;
        }

        // Empty space — starts a rubber-band selection drag. Clip creation
        // is opt-in only (double-click for a 1-bar clip, or the Draw tool
        // for a drawn length — see mouseDoubleClick / handleDrawClipDown),
        // so a plain click here just clears/starts the selection rect.
        // This also fires below the last track row (validTrack false,
        // dragTrack -1) so a rubber-band can be started anywhere in the
        // arranger.
        {
            rubberBandStart = e.getPosition();
            rubberBandRect  = juce::Rectangle<int> (rubberBandStart, rubberBandStart);
            rubberBandBaseSelection = e.mods.isShiftDown() ? selectedClips
                                                            : std::vector<std::pair<int,int>>{};
            dragMode  = DragMode::RubberBand;
            dragTrack = trackIdx;
            repaint(); return;
        }
    }   // end mouseDown

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // ── Ruler scrub / loop drag ────────────────────────────────────────────
        if (rulerDrag == RulerDrag::Scrub)
        {
            engine.seekToTick (juce::jmax ((int64_t)0, xToTick (e.x)));
            repaint(); return;
        }

        if (rulerDrag == RulerDrag::LoopSet)
        {
            const int64_t tick = xToTick (e.x);
            if (tick >= loopDragAnchor)
            {
                loopStart = loopDragAnchor;
                loopEnd   = tick;
            }
            else
            {
                loopStart = tick;
                loopEnd   = loopDragAnchor;
            }
            repaint(); return;
        }

        if (rulerDrag == RulerDrag::DragLoopStart)
        {
            const int64_t tick = snapTick (xToTick (e.x));
            const int64_t maxStart = juce::jmax<int64_t> (0, loopEnd - MidiClip::kPPQ);
            loopStart = juce::jlimit<int64_t> (0, maxStart, tick);
            repaint(); return;
        }

        if (rulerDrag == RulerDrag::DragLoopEnd)
        {
            const int64_t tick = snapTick (xToTick (e.x));
            loopEnd = juce::jmax (loopStart + MidiClip::kPPQ, tick);
            repaint(); return;
        }

        if (dragMode == DragMode::None) return;

        if (dragMode == DragMode::RubberBand)
        {
            rubberBandRect = juce::Rectangle<int>::leftTopRightBottom (
                juce::jmin (rubberBandStart.x, e.x), juce::jmin (rubberBandStart.y, e.y),
                juce::jmax (rubberBandStart.x, e.x), juce::jmax (rubberBandStart.y, e.y));
            updateRubberBandSelection();
            repaint(); return;
        }

        if (dragMode == DragMode::DrawClip)
        {
            const int64_t tick   = snapTick (xToTick (e.x));
            const int64_t minLen = MidiClip::kPPQ * 4;   // 1-bar floor
            drawLenTicks = juce::jmax (minLen, tick - drawStartTick);
            repaint(); return;
        }

        const int dx = e.x - dragStartX;

        if (dragMode == DragMode::ResizeRight)
        {
            const int64_t newLen = juce::jmax (
                MidiClip::kPPQ,
                dragStartTicks + (int64_t)(dx * ticksPerPixel()));
            dragResizeLen = snapTick (newLen);
            repaint(); return;
        }

        if (dragMode == DragMode::MoveClip)
        {
            const int64_t newOff = juce::jmax ((int64_t)0,
                dragStartTicks + (int64_t)(dx * ticksPerPixel()));
            dragLiveOffset = snapTick (newOff);

            // Auto-scroll: nudge scrollX if near edges
            const int margin = 40;
            if (e.x < clipGridBounds.getX() + margin)
                scrollX = juce::jmax (0.0, scrollX - 8.0);
            else if (e.x > clipGridBounds.getRight() - margin)
                scrollX = scrollX + 8.0;

            updateScrollRanges();
            repaint(); return;
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        // Commit a drawn clip — length is whatever drawLenTicks grew to
        // while dragging (floored to 1 bar in handleDrawClipDown/mouseDrag
        // above), so a plain click-with-no-drag still yields a usable
        // 1-bar clip rather than nothing.
        if (dragMode == DragMode::DrawClip && dragTrack >= 0)
        {
            const int newIdx = engine.addClip (dragTrack, drawStartTick, drawLenTicks);
            selectSingleClip (dragTrack, newIdx);
            drawLenTicks = 0;
        }

        // Commit clip resize
        if (dragMode == DragMode::ResizeRight && dragTrack >= 0)
        {
            engine.setClipLengthTicks (dragTrack, dragClip, dragResizeLen);
            dragResizeLen = 0;
        }

        // Commit clip move — every clip in selectedClips moves together by
        // the same delta the anchor (dragTrack/dragClip) moved by. Falls
        // back to just the anchor if nothing is in the multi-selection
        // (e.g. a plain, non-multi drag).
        if (dragMode == DragMode::MoveClip && dragTrack >= 0)
        {
            const int64_t delta = dragLiveOffset - dragStartTicks;
            if (delta != 0)
            {
                // Snapshot (track, oldStart, newStart) triples before touching
                // the engine — moving one clip can re-sort its track's clip
                // list and invalidate every other clip's index on that same
                // track mid-loop, so indices must be re-resolved by tick
                // rather than reused across calls.
                struct PendingMove { int track; int64_t oldStart; int64_t newStart; };
                std::vector<PendingMove> moves;

                if (selectedClips.empty())
                {
                    moves.push_back ({ dragTrack, dragStartTicks, dragLiveOffset });
                }
                else
                {
                    for (auto& sel : selectedClips)
                    {
                        const auto info = engine.getClipInfo (sel.first, sel.second);
                        const int64_t newStart = juce::jmax ((int64_t) 0, info.startTick + delta);
                        moves.push_back ({ sel.first, info.startTick, newStart });
                    }
                }

                auto findClipIndexAtStart = [this] (int track, int64_t startTick) -> int
                {
                    for (int ci = 0; ci < engine.getNumClips (track); ++ci)
                        if (engine.getClipInfo (track, ci).startTick == startTick)
                            return ci;
                    return -1;
                };

                for (auto& mv : moves)
                {
                    const int ci = findClipIndexAtStart (mv.track, mv.oldStart);
                    if (ci >= 0)
                        engine.setClipStartTick (mv.track, ci, mv.newStart);
                }

                // Re-sync selectedClips (and the primary selectedTrack/
                // selectedClip) to the clips' post-move, post-sort indices.
                std::vector<std::pair<int,int>> updated;
                for (auto& mv : moves)
                {
                    const int ci = findClipIndexAtStart (mv.track, mv.newStart);
                    if (ci >= 0) updated.emplace_back (mv.track, ci);
                }
                if (! selectedClips.empty())
                    selectedClips = updated;
                if (! updated.empty())
                {
                    selectTrack (updated.back().first);
                    selectedClip = updated.back().second;
                }
            }
            dragLiveOffset = 0;
        }

        // Finalize the rubber-band drag: a real drag (rect grew past a
        // couple of pixels) just leaves the live-updated selectedClips in
        // place. A plain click on empty space no longer creates a clip —
        // clip creation is opt-in only, via double-click (see
        // mouseDoubleClick, 1-bar clip) or the Draw tool (see
        // handleDrawClipDown/DragMode::DrawClip, drawn-length clip) — so a
        // non-drag click here just clears the selection.
        if (dragMode == DragMode::RubberBand)
        {
            rubberBandRect = {};
            rubberBandBaseSelection.clear();
        }

        // Commit a ruler-drawn loop region (Alt-drag), or a dragged existing
        // marker, to the engine — so it actually takes effect as the play
        // loop and so the transport's L/R locator fields (which read
        // straight from the engine) pick it up too, instead of only ever
        // existing as a local paint value.
        if ((rulerDrag == RulerDrag::LoopSet
             || rulerDrag == RulerDrag::DragLoopStart
             || rulerDrag == RulerDrag::DragLoopEnd)
            && loopEnd > loopStart)
            engine.setLoopRange (loopStart, loopEnd);

        rulerDrag  = RulerDrag::None;
        dragMode   = DragMode::None;
        dragTrack  = -1;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! clipGridBounds.contains (e.getPosition())) return;
        const int trackIdx = trackFromY (e.y);
        if (! juce::isPositiveAndBelow (trackIdx, engine.getNumTracks())) return;

        for (int ci = 0; ci < engine.getNumClips (trackIdx); ++ci)
        {
            if (clipRectForClip (trackIdx, ci).contains (e.getPosition()))
            {
                selectTrack (trackIdx);
                selectedClip  = ci;
                trackStrip.setSelectedTrack (trackIdx);
                repaint();
                if (onClipDoubleClicked) onClipDoubleClicked (trackIdx, ci);
                return;
            }
        }

        // Double-click on empty space — the only click-based way to create
        // a clip (see mouseUp's RubberBand handling: a plain single click
        // no longer creates one). Always exactly 1 bar; a longer clip is
        // made explicitly with the Draw tool (handleDrawClipDown), which
        // sizes to whatever length the user drags out.
        const int64_t clickTick = snapTick (xToTick (e.x));
        const int64_t oneBarLen = MidiClip::kPPQ * 4;
        const int newIdx = engine.addClip (trackIdx, clickTick, oneBarLen);
        selectSingleClip (trackIdx, newIdx);
        trackStrip.setSelectedTrack (trackIdx);
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
        {
            // Zoom around mouse position
            const double tickAtMouse = xToTick (e.x);
            const double factor      = w.deltaY > 0 ? 1.18 : (1.0 / 1.18);
            pixelsPerTick = juce::jlimit (0.003, 0.4, pixelsPerTick * factor);
            scrollX = juce::jmax (0.0,
                tickAtMouse * pixelsPerTick - (e.x - clipGridBounds.getX()));
        }
        else if (e.mods.isShiftDown())
        {
            scrollX = juce::jmax (0.0, scrollX - w.deltaY * 200.0);
        }
        else if (e.mods.isAltDown())
        {
            // Vertical scroll with Alt
            const int totalH = engine.getNumTracks() * trackH;
            const int viewH  = clipGridBounds.getHeight();
            scrollY = juce::jlimit (0, juce::jmax (0, totalH - viewH),
                                    scrollY - (int)(w.deltaY * 40.0));
        }
        else
        {
            scrollX = juce::jmax (0.0, scrollX - w.deltaY * 80.0);
        }
        updateScrollRanges();
        repaint();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::spaceKey)
        {
            if (engine.isPlaying()) engine.stop();
            else                    engine.play();
            return true;
        }

        if (k == juce::KeyPress::homeKey ||
            k.getKeyCode() == juce::KeyPress::numberPad0)
        {
            engine.rewind();
            scrollX = 0.0;
            updateScrollRanges();
            repaint();
            return true;
        }

        if (k.getKeyCode() == '+' || k.getKeyCode() == '=')
        {
            trackH = juce::jlimit (kMinTrackH, kMaxTrackH, trackH + 6);
            trackStrip.setTrackHeight (trackH);
            updateScrollRanges(); trackStrip.repaint(); repaint();
            return true;
        }
        if (k.getKeyCode() == '-')
        {
            trackH = juce::jlimit (kMinTrackH, kMaxTrackH, trackH - 6);
            trackStrip.setTrackHeight (trackH);
            updateScrollRanges(); trackStrip.repaint(); repaint();
            return true;
        }

        if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
            && selectedTrack >= 0)
        {
            if (MidiClip* c = engine.getClip (selectedTrack, selectedClip))
                c->clear();
            repaint();
            return true;
        }

        // Tool shortcuts — same letters/keys as PianoRollComponent's Tool.
        if (k.getKeyCode() == 'S') { setActiveTool (Tool::Select); return true; }
        if (k.getKeyCode() == 'D') { setActiveTool (Tool::Draw);   return true; }
        if (k.getKeyCode() == 'E') { setActiveTool (Tool::Erase);  return true; }
        if (k.getKeyCode() == 'K') { setActiveTool (Tool::Split);  return true; }
        if (k.getKeyCode() == 'G') { setActiveTool (Tool::Glue);   return true; }

        return false;
    }

    //==========================================================================
    //  Floating transport
    //==========================================================================
    /** Undocks the transport from this view (it was a normal child component,
     *  reserving kTransportH at the top — see resized()) and puts it on the
     *  desktop as its own floating window instead. It's the same
     *  FloatingTransportBar instance throughout, just reparented; see that
     *  class's header comment. Wired to transport.onFloatRequested. */
    void showFloatingTransport()
    {
        removeChildComponent (&transport);
        transport.setDocked (false);
        transport.show();
        resized();   // reclaim the space the docked bar was occupying
    }

    /** Removes the transport from the desktop and re-docks it as a normal
     *  child component at the top of this view. Wired to
     *  FloatingTransportBar::onDockRequested. */
    void dockTransport()
    {
        transport.hide();
        transport.setDocked (true);
        addAndMakeVisible (transport);
        resized();
    }

    /** Lets an owner (e.g. SlotWindowContent) dock extra controls — such as the
     *  Mixer / Arranger view switcher — into the far left of the transport row. */
    FloatingTransportBar& getTransportBar() noexcept { return transport; }

    /** Fired when the floating transport's MIXER / ARRANGER / GLOBAL EQ
     *  buttons are clicked — set by the owner (e.g. SlotWindowContent) to
     *  switch views, mirroring how it wires up its own switcher buttons. */
    std::function<void()> onMixerRequested;
    std::function<void()> onArrangerRequested;
    std::function<void()> onGlobalEqRequested;

private:
    //==========================================================================
    //  State
    //==========================================================================
    SequencerEngine&      engine;
    AbletonLink*          linkPtr = nullptr;
    FloatingTransportBar  transport;
    TrackInspector        inspector;
    TrackHeaderStrip      trackStrip;
    ZoomableScrollBar     hScroll { false };
    ZoomableScrollBar     vScroll { true  };

    juce::Rectangle<int>  gridArea, rulerBounds, clipGridBounds;
    
    int      selectedTrack  = -1;
    int      trackH         = kDefaultTrackH;

    // Horizontal scroll / zoom
    double   pixelsPerTick  = 0.08;
    double   scrollX        = 0.0;

    // Vertical scroll (pixels from top of track list)
    int      scrollY        = 0;

    // Per-track clip start offsets (arrange-view only, not serialised)
    // clip offsets are now stored per ClipSlot inside the engine

    // Loop markers (-1 = unset)
    int64_t  loopStart      = -1;
    int64_t  loopEnd        = -1;
    int64_t  loopDragAnchor = 0;

    // Playhead auto-scroll
    int64_t  lastAutoScrollTick = -1;

    // Drag state
    enum class DragMode  { None, MoveClip, ResizeRight, RubberBand, DrawClip };
    enum class RulerDrag { None, Scrub, LoopSet, DragLoopStart, DragLoopEnd };
    DragMode  dragMode       = DragMode::None;
    RulerDrag rulerDrag      = RulerDrag::None;

    // Resize-handle hover preview — set from mouseMove when the cursor is
    // within kResizeZone of an unselected clip's right edge, so paintClip
    // can fade the handle in at low opacity ahead of an actual click.
    // -1/-1 when nothing is hovered.
    int       hoverTrack     = -1;
    int       hoverClip      = -1;

    // Rubber-band drag-select (empty-space drag with the Select tool)
    juce::Point<int>      rubberBandStart;
    juce::Rectangle<int>  rubberBandRect;
    // Selection the rubber-band drag started from — non-empty only when the
    // drag began with Shift held, so the drag adds to it rather than
    // replacing it.
    std::vector<std::pair<int,int>> rubberBandBaseSelection;

    // Clip-grid tool — mirrors PianoRollComponent::Tool so the two right-click
    // "Tool" submenus (and their S/D/E/K/G shortcuts) match; the actions mean
    // clip-level things here rather than note-level things.
    Tool      currentTool    = Tool::Select;
    int       dragTrack      = -1;
    int       dragClip       = -1;   // which clip slot is being dragged/resized
    int       dragStartX     = 0;
    int64_t   dragStartTicks = 0;
    int64_t   dragLiveOffset = 0;
    int64_t   dragResizeLen  = 0;   // live preview length during ResizeRight
    int64_t   drawStartTick  = 0;   // Draw-tool clip start, set on mouseDown
    int64_t   drawLenTicks   = 0;   // Draw-tool live preview length while dragging

    int       selectedClip   = 0;   // which clip is selected on selectedTrack

    // Multi-clip selection — (trackIdx, clipIdx) pairs. selectedTrack/
    // selectedClip above still track the "primary" clip (whichever was
    // most recently clicked/toggled) so all pre-existing single-clip code
    // (paint's row highlight, the context menu, piano-roll open, Delete)
    // keeps working unchanged; selectedClips is only consulted by the new
    // group-move / rubber-band code.
    std::vector<std::pair<int,int>> selectedClips;

    //==========================================================================
    //  Timer — repaints + auto-scroll
    //==========================================================================
    void timerCallback() override
    {
        if (engine.isPlaying())
            autoScrollToPlayhead();

        // Applies any notes the audio thread queued while recording. Cheap
        // no-op when not recording; must run on the message thread, which
        // this Timer already is. Draining unconditionally (rather than only
        // while isRecording() is true) means a note-off that arrives right
        // as recording stops still gets its real duration instead of being
        // silently dropped.
        engine.drainRecordedEvents();

        // Keep the ruler/grid loop markers in sync with the engine's actual
        // loop range — e.g. locators set from the docked or floating
        // transport's SET LEFT/SET RIGHT buttons or editable L/R fields,
        // which previously never reached this view at all. Skipped while
        // the user is actively dragging out a new region on the ruler
        // (mouseUp below commits that drag to the engine instead), so the
        // live drag preview isn't fought over every timer tick.
        if (rulerDrag != RulerDrag::LoopSet)
        {
            loopStart = engine.getLoopStartTick();
            loopEnd   = engine.getLoopEndTick();
        }

        repaint();
    }

    void autoScrollToPlayhead()
    {
        const int64_t tick = engine.getPlayheadTick();
        if (tick == lastAutoScrollTick) return;
        lastAutoScrollTick = tick;

        const float px = tickToX (tick);
        const int   right = clipGridBounds.getRight();
        const int   left  = clipGridBounds.getX();

        // Page-scroll: if playhead goes past the right edge, jump one page
        if (px > right - 20)
        {
            scrollX += clipGridBounds.getWidth() * 0.85;
            updateScrollRanges();
        }
        // Snap back if rewound past left edge
        else if (px < left && scrollX > 0)
        {
            scrollX = juce::jmax (0.0, tick * pixelsPerTick - 40.0);
            updateScrollRanges();
        }
    }

    //==========================================================================
    //  Coordinate helpers
    //==========================================================================
    double ticksPerPixel() const noexcept
    {
        return pixelsPerTick > 0.0 ? 1.0 / pixelsPerTick : 1.0;
    }

    int64_t xToTick (int x) const noexcept
    {
        return (int64_t) juce::jmax (0.0,
            (x - clipGridBounds.getX() + scrollX) / pixelsPerTick);
    }

    float tickToX (int64_t t) const noexcept
    {
        return (float)(t * pixelsPerTick - scrollX + clipGridBounds.getX());
    }

    /** Grab tolerance (screen pixels) for clicking directly on an existing
     *  loop-marker line to drag it, both here and in updateCursor(). */
    static constexpr int kLoopMarkerGrabPx = 5;

    bool isNearLoopMarker (int x, int64_t markerTick) const noexcept
    {
        const int mx = (int) tickToX (markerTick);
        return (x > mx ? x - mx : mx - x) <= kLoopMarkerGrabPx;
    }

    int trackFromY (int y) const noexcept
    {
        return (y - clipGridBounds.getY() + scrollY) / trackH;
    }

    int trackTopY (int i) const noexcept
    {
        return clipGridBounds.getY() + i * trackH - scrollY;
    }

    juce::Rectangle<int> clipRectForClip (int trackIdx, int clipIdx) const
    {
        if (clipGridBounds.isEmpty()) return {};
        const auto info = engine.getClipInfo (trackIdx, clipIdx);

        int64_t startTick  = info.startTick;
        int64_t lengthTicks = info.lengthTicks;

        // Live overrides during drag — a multi-clip move previews every
        // selected clip sliding together by the same delta as the clip
        // actually under the mouse (dragTrack/dragClip), not just that anchor.
        if (dragMode == DragMode::MoveClip)
        {
            if (dragTrack == trackIdx && dragClip == clipIdx)
                startTick = dragLiveOffset;
            else if (isClipSelected (dragTrack, dragClip) && isClipSelected (trackIdx, clipIdx))
                startTick = info.startTick + (dragLiveOffset - dragStartTicks);
        }
        if (dragMode == DragMode::ResizeRight && dragTrack == trackIdx && dragClip == clipIdx)
            lengthTicks = dragResizeLen;

        const int w = juce::jmax (kMinClipPx, (int)(lengthTicks * pixelsPerTick));
        const int x = clipGridBounds.getX() + (int)(startTick * pixelsPerTick - scrollX);
        const int y = trackTopY (trackIdx);
        return { x, y, w, trackH - 1 };
    }

    /** Grid-quantize resolution currently selected in the GRID combo, read
     *  live from the transport bar's GRID combo, docked or floating — same
     *  instance either way now (see FloatingTransportBar's header comment),
     *  so there's only ever one snap value to read. Shared by snapTick()
     *  (rounds a tick to this resolution) and paintGridLines() (draws
     *  sub-beat lines at this resolution) so the visible grid always matches
     *  what clip create/move/resize/split actually snaps to. 0 means no
     *  snapping. */
    int64_t currentSnapTicks() const noexcept
    {
        return transport.getSnapTicks();
    }

    int64_t snapTick (int64_t t) const noexcept
    {
        const int64_t snap = currentSnapTicks();
        if (snap <= 0) return t;
        return ((t + snap / 2) / snap) * snap;
    }

    //==========================================================================
    //  Multi-selection helpers
    //==========================================================================
    /** True if (trackIdx, clipIdx) is part of the current multi-clip selection. */
    bool isClipSelected (int trackIdx, int clipIdx) const noexcept
    {
        for (auto& p : selectedClips)
            if (p.first == trackIdx && p.second == clipIdx)
                return true;
        return false;
    }

    /** Replaces the whole selection with a single clip, and updates the
     *  legacy selectedTrack/selectedClip "primary" pointer to match, so
     *  every pre-existing single-clip code path keeps working unchanged. */
    void selectSingleClip (int trackIdx, int clipIdx)
    {
        selectedClips.clear();
        selectedClips.emplace_back (trackIdx, clipIdx);
        selectTrack (trackIdx);
        selectedClip = clipIdx;
    }

    /** Click-to-select behaviour shared by the resize-handle and clip-body
     *  mouseDown branches: Shift adds the clicked clip to whatever's
     *  already selected without disturbing the rest of the group; a plain
     *  click on a clip that's already part of a multi-clip selection
     *  leaves the whole group selected (so it can be dragged together); a
     *  plain click on a clip that ISN'T already selected replaces the
     *  selection with just that clip. */
    void beginClipSelection (int trackIdx, int clipIdx, bool shiftDown)
    {
        if (shiftDown)
        {
            if (! isClipSelected (trackIdx, clipIdx))
                selectedClips.emplace_back (trackIdx, clipIdx);
            selectTrack (trackIdx);
            selectedClip = clipIdx;
        }
        else if (! isClipSelected (trackIdx, clipIdx))
        {
            selectSingleClip (trackIdx, clipIdx);
        }
        else
        {
            selectTrack (trackIdx);
            selectedClip = clipIdx;
        }
    }

    /** Recomputes selectedClips as rubberBandBaseSelection (empty unless the
     *  drag was Shift-started) plus every clip whose on-screen rect
     *  currently intersects rubberBandRect. Called live from mouseDrag so
     *  the selection updates as the rectangle grows. */
    void updateRubberBandSelection()
    {
        selectedClips = rubberBandBaseSelection;

        const int numTracks = engine.getNumTracks();
        for (int ti = 0; ti < numTracks; ++ti)
        {
            const int numClips = engine.getNumClips (ti);
            for (int ci = 0; ci < numClips; ++ci)
            {
                if (isClipSelected (ti, ci)) continue;
                if (clipRectForClip (ti, ci).intersects (rubberBandRect))
                    selectedClips.emplace_back (ti, ci);
            }
        }

        if (! selectedClips.empty())
        {
            const auto& primary = selectedClips.back();
            selectTrack (primary.first);
            selectedClip = primary.second;
        }
    }

    int64_t totalVisibleTicks() const noexcept
    {
        int64_t maxEnd = MidiClip::kPPQ * 4 * 4;
        for (int ti = 0; ti < engine.getNumTracks(); ++ti)
            for (int ci = 0; ci < engine.getNumClips (ti); ++ci)
            {
                const auto info = engine.getClipInfo (ti, ci);
                maxEnd = juce::jmax (maxEnd, info.endTick());
            }
        return juce::jmax (maxEnd * 2, MidiClip::kPPQ * 4 * 32);
    }

    //==========================================================================
    //  Scrollbars
    //==========================================================================
    /** Set the selected track index and update the SfzPlayer's live input channel mask.
     *  If the selected track is an SF2/SFZ track, its FluidSynth channel receives
     *  live controller (ch-1) input.  Any other track type clears the mask (silence). */
    void selectTrack (int idx)
    {
        selectedTrack = idx;
        inspector.setSelectedTrack (idx);

        uint16_t mask = 0;
        TrackType type = TrackType::MainSlice;
        bool isSfzInstrument = false;
        bool hasSelection = juce::isPositiveAndBelow (idx, engine.getNumTracks());

        int liveCh = 0;  // 0 = disabled (SfPlayer handles its own mask)
        int assignedChannel1Based = -1;
        int assignedPresetBank = -1, assignedPresetProgram = -1;

        if (hasSelection)
        {
            const auto info = engine.getTrackInfo (idx);
            type = info.type;
            isSfzInstrument = info.isSfzInstrument;
            switch (info.type)
            {
                case TrackType::MainSlice:
                    liveCh = 1;  // slicer always responds on ch 1
                    break;
                case TrackType::ChromaticSlice:
                    liveCh = info.midiChannel + 1;  // stored 0-based
                    break;
                case TrackType::SfPlayer:
                    liveCh = 0;  // SfPlayer uses liveInputChannelMask instead
                    if (info.midiChannel >= 0 && info.midiChannel < 16)
                    {
                        mask = (uint16_t)(1u << info.midiChannel);
                        assignedChannel1Based = info.midiChannel + 1;
                    }
                    if (! isSfzInstrument)
                    {
                        // The track's own preset link (set wherever it was
                        // assigned — e.g. TrackInspector's PART dropdown),
                        // not Sf2ProgramGrid's separate channel map.
                        assignedPresetBank    = info.preset.bank;
                        assignedPresetProgram = info.preset.preset;
                    }
                    break;
            }
        }

        engine.setSelectedLiveChannel (liveCh);
        engine.setSelectedSfLiveChannels (mask);
        engine.setRecordingTrack (hasSelection ? idx : -1);

        // Authoritative live-routing target for PluginProcessor::processBlock
        // (see SequencerEngine::getSelectedLiveTarget() / setMidiRouteMode()'s
        // comment on ArrangeView::selectTrack). The accessors above are
        // legacy/vestigial and do NOT feed getSelectedLiveTarget() — without
        // this call selectedLiveTarget never leaves its {none, 0} default, so
        // channel-1 live notes are never re-stamped to the selected track's
        // engine/channel no matter what's highlighted in the Arranger.
        engine.setSelectedTrack (hasSelection ? idx : -1);

        if (onTrackTypeSelected)
            onTrackTypeSelected (type, hasSelection, isSfzInstrument, assignedChannel1Based,
                                  assignedPresetBank, assignedPresetProgram);
    }

    static void styleScrollBar (juce::ScrollBar& sb)
    {
        const auto& theme = getTheme();
        sb.setColour (juce::ScrollBar::backgroundColourId, theme.waveformBg);
        sb.setColour (juce::ScrollBar::thumbColourId,      theme.foreground.withAlpha (0.28f));
        sb.setColour (juce::ScrollBar::trackColourId,      theme.button.withAlpha (0.45f));
    }

    void updateScrollRanges()
    {
        // Horizontal
        const double totalW = totalVisibleTicks() * pixelsPerTick;
        hScroll.setRangeLimits (0.0, totalW);
        hScroll.setCurrentRange (scrollX,
                                 scrollX + clipGridBounds.getWidth(),
                                 juce::dontSendNotification);

        // Vertical
        const int totalH = engine.getNumTracks() * trackH;
        const int viewH  = clipGridBounds.getHeight();
        scrollY = juce::jlimit (0, juce::jmax (0, totalH - viewH), scrollY);
        vScroll.setRangeLimits (0.0, (double)juce::jmax (viewH, totalH));
        vScroll.setCurrentRange ((double)scrollY, (double)(scrollY + viewH),
                                 juce::dontSendNotification);

        // Scrolling/zooming can slide a clip out from under (or into) the
        // cursor without the mouse itself moving, so re-run the hover hit
        // test against wherever the mouse actually is whenever the visible
        // range changes.
        updateHoverHandle (getMouseXYRelative());
    }

    void scrollBarMoved (juce::ScrollBar* sb, double newRangeStart) override
    {
        if (sb == &hScroll)
            scrollX = newRangeStart;
        else
            scrollY = (int) newRangeStart;
        updateHoverHandle (getMouseXYRelative());
        repaint();
    }

    //==========================================================================
    //  Tool
    //==========================================================================
    /** Switches the active clip-grid tool — from the right-click "Tool"
     *  submenu, a keyboard shortcut (S/D/E/K/G), or eventually a toolbar.
     *  Mirrors PianoRollComponent::setActiveTool()'s immediate-feedback
     *  pattern: apply the tool's cursor right away rather than waiting for
     *  the next mouseMove. */
    void setActiveTool (Tool t)
    {
        currentTool = t;
        setMouseCursor (toolCursorFor (t));
        repaint();
    }

    //==========================================================================
    //  Cursor
    //==========================================================================
    /** Builds an actual mouse cursor out of the same ToolIcons glyph shown on
     *  the right-click Tool submenu, so the OS cursor over the clip grid always
     *  shows which tool is active — mirrors PianoRollComponent::makeToolCursor(). */
    static juce::MouseCursor makeToolCursor (Tool tool)
    {
        constexpr int size = 32;
        juce::Image img (juce::Image::ARGB, size, size, true);
        juce::Graphics g (img);

        // Same fixed box for every stamp below (outline pass + fill pass) so
        // position-anchored glyphs (e.g. the Select arrow) don't desync
        // between passes and warp out of shape.
        const auto b = juce::Rectangle<float> (2.0f, 2.0f, (float) size - 4.0f, (float) size - 4.0f);
        const auto kind = static_cast<ToolIcons::Kind> (static_cast<int> (tool));

        static const int offs[][2] = { {-1,0}, {1,0}, {0,-1}, {0,1}, {-1,-1}, {1,-1}, {-1,1}, {1,1} };
        for (auto& o : offs)
        {
            juce::Graphics::ScopedSaveState save (g);
            g.addTransform (juce::AffineTransform::translation ((float) o[0], (float) o[1]));
            ToolIcons::draw (g, kind, b, juce::Colours::black);
        }
        ToolIcons::draw (g, kind, b, juce::Colours::white);

        // Hotspot: the "business end" of each glyph — Erase/Split/Glue have
        // no single sharp point, so their hotspot is just the icon's centre.
        int hx = size / 2, hy = size / 2;
        switch (tool)
        {
            case Tool::Select: hx = (int) (size * 0.22f); hy = (int) (size * 0.10f); break;
            case Tool::Draw:   hx = (int) (size * 0.25f); hy = (int) (size * 0.82f); break;
            case Tool::Split:  hy = (int) (size * 0.19f); break;
            default: break;
        }
        return juce::MouseCursor (img, hx, hy);
    }

    /** Cached per-tool cursors — built once, since makeToolCursor() rasterises
     *  an image and mouseMove fires far too often to redo that every call. */
    static const juce::MouseCursor& toolCursorFor (Tool t)
    {
        static const juce::MouseCursor cursors[] = {
            makeToolCursor (Tool::Select), makeToolCursor (Tool::Draw), makeToolCursor (Tool::Erase),
            makeToolCursor (Tool::Split),  makeToolCursor (Tool::Glue)
        };
        return cursors[(int) t];
    }

    void updateCursor (const juce::MouseEvent& e)
    {
        if (dragMode == DragMode::ResizeRight)
        { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); return; }
        if (dragMode == DragMode::MoveClip)
        { setMouseCursor (juce::MouseCursor::DraggingHandCursor); return; }
        if (rulerDrag == RulerDrag::DragLoopStart || rulerDrag == RulerDrag::DragLoopEnd)
        { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); return; }

        if (rulerBounds.contains (e.getPosition())
            && loopStart >= 0 && loopEnd > loopStart
            && (isNearLoopMarker (e.x, loopStart) || isNearLoopMarker (e.x, loopEnd)))
        { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); return; }

        if (clipGridBounds.contains (e.getPosition()))
        {
            const int trackIdx = trackFromY (e.y);
            if (juce::isPositiveAndBelow (trackIdx, engine.getNumTracks()))
            {
                // Non-Select tools act with a single click rather than
                // move/resize, so signal that with the tool's own cursor
                // for the whole grid instead of the Select-tool hover cues.
                if (currentTool != Tool::Select)
                {
                    setMouseCursor (toolCursorFor (currentTool));
                    return;
                }

                for (int ci = 0; ci < engine.getNumClips (trackIdx); ++ci)
                {
                    const auto clipR = clipRectForClip (trackIdx, ci);
                    if (clipR.contains (e.getPosition()))
                    {
                        if (e.x >= clipR.getRight() - kResizeZone)
                            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
                        else
                            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
                        return;
                    }
                }
                // Over empty track space — show pencil / crosshair to signal clip creation
                setMouseCursor (juce::MouseCursor::CrosshairCursor);
                return;
            }
        }
        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    /** Recomputes hoverTrack/hoverClip from the current mouse position and
     *  repaints if they changed. Only unselected clips get a hover entry —
     *  a selected clip's handle is already fully visible, so there's
     *  nothing for the hover preview to add there. */
    void updateHoverHandle (juce::Point<int> pos)
    {
        int newHoverTrack = -1, newHoverClip = -1;

        if (currentTool == Tool::Select && clipGridBounds.contains (pos))
        {
            const int trackIdx = trackFromY (pos.y);
            if (juce::isPositiveAndBelow (trackIdx, engine.getNumTracks()))
            {
                const int numClips = engine.getNumClips (trackIdx);
                for (int ci = 0; ci < numClips; ++ci)
                {
                    const auto clipR = clipRectForClip (trackIdx, ci);
                    if (clipR.contains (pos)
                        && pos.x >= clipR.getRight() - kResizeZone
                        && ! isClipSelected (trackIdx, ci))
                    {
                        newHoverTrack = trackIdx;
                        newHoverClip  = ci;
                        break;
                    }
                }
            }
        }

        setHoverHandle (newHoverTrack, newHoverClip);
    }

    void setHoverHandle (int trackIdx, int clipIdx)
    {
        if (trackIdx == hoverTrack && clipIdx == hoverClip) return;
        hoverTrack = trackIdx;
        hoverClip  = clipIdx;
        repaint();
    }

    //==========================================================================
    //  Context menus
    //==========================================================================
    void showContextMenu (int trackIdx, int clipIdx, const juce::MouseEvent& e)
    {
        const bool validTrack = juce::isPositiveAndBelow (trackIdx, engine.getNumTracks());
        const SequencerTrackInfo info = validTrack ? engine.getTrackInfo (trackIdx)
                                                    : SequencerTrackInfo{};
        const bool onClip = validTrack && (clipIdx >= 0);
        juce::PopupMenu m;

        // Tool submenu — same entry as PianoRollComponent's clip/note-grid
        // right-click menu, so switching tools works the same way in both views.
        // Icons come from the shared ToolIcons.h glyph set so the two menus
        // are visually identical, not just structurally the same.
        juce::PopupMenu toolMenu;
        const auto toolIconColour = findColour (juce::TextButton::textColourOffId);
        auto addToolItem = [&] (int itemId, const juce::String& text, Tool tool)
        {
            juce::PopupMenu::Item item;
            item.itemID   = itemId;
            item.text     = text;
            item.isTicked = (currentTool == tool);
            item.setImage (ToolIcons::makeMenuIcon (static_cast<ToolIcons::Kind> (static_cast<int> (tool)),
                                                     toolIconColour));
            toolMenu.addItem (item);
        };
        addToolItem (30, "Select (S)", Tool::Select);
        addToolItem (31, "Draw (D)",   Tool::Draw);
        addToolItem (32, "Erase (E)",  Tool::Erase);
        addToolItem (33, "Split (K)",  Tool::Split);
        addToolItem (34, "Glue (G)",   Tool::Glue);
        m.addSubMenu ("Tool", toolMenu);

        // Track/clip-specific items only make sense when the click landed
        // on an actual track row — right-clicking empty space below the
        // last track (or with no tracks at all) still opens the menu, just
        // scoped down to the Tool submenu above.
        if (validTrack)
        {
            m.addSeparator();

            if (onClip)
            {
                m.addItem (1, "Open in piano roll");
                m.addSeparator();
                m.addItem (8, "Repeat clip");
                m.addItem (4, "Duplicate to next track");
                m.addSeparator();
                m.addItem (2, info.enabled ? "Mute track" : "Unmute track");
                m.addItem (3, "Clear clip");
                m.addItem (6, "Delete clip");
                m.addSeparator();
                m.addItem (5, "Set loop to clip length");
            }
            else
            {
                m.addItem (2, info.enabled ? "Mute track" : "Unmute track");
            }
        }

        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (e.getScreenX(), e.getScreenY(), 1, 1)),
            [this, trackIdx, clipIdx, info, onClip] (int result)
            {
                switch (result)
                {
                    case 1:
                        selectTrack (trackIdx);
                        selectedClip  = clipIdx;
                        trackStrip.setSelectedTrack (trackIdx);
                        if (onClipDoubleClicked) onClipDoubleClicked (trackIdx, clipIdx);
                        break;
                    case 2:
                        engine.setTrackEnabled (trackIdx, ! info.enabled);
                        break;
                    case 3:
                        if (MidiClip* c = engine.getClip (trackIdx, clipIdx))
                            c->clear();
                        break;
                    case 4:
                        duplicateClipToNextTrack (trackIdx, clipIdx);
                        break;
                    case 5:
                    {
                        const auto ci = engine.getClipInfo (trackIdx, clipIdx);
                        loopStart = ci.startTick;
                        loopEnd   = ci.endTick();
                        break;
                    }
                    case 6:
                        engine.removeClip (trackIdx, clipIdx);
                        if (selectedTrack == trackIdx && selectedClip == clipIdx)
                            selectedClip = 0;
                        break;
                    case 8:  // Repeat clip
                    {
                        MidiClip* src = engine.getClip (trackIdx, clipIdx);
                        if (src)
                        {
                            const auto srcInfo = engine.getClipInfo (trackIdx, clipIdx);
                            juce::Array<MidiNote> notes;
                            { const juce::ScopedReadLock sl (src->getLock()); notes = src->getNotes(); }
                            for (int rep = 1; rep <= 1; ++rep)
                            {
                                const int64_t start = srcInfo.startTick + srcInfo.lengthTicks * rep;
                                const int newIdx = engine.addClip (trackIdx, start, srcInfo.lengthTicks);
                                if (MidiClip* dst = engine.getClip (trackIdx, newIdx))
                                    dst->setNotes (notes);
                            }
                        }
                        break;
                    }
                    case 30: setActiveTool (Tool::Select); break;
                    case 31: setActiveTool (Tool::Draw);   break;
                    case 32: setActiveTool (Tool::Erase);  break;
                    case 33: setActiveTool (Tool::Split);  break;
                    case 34: setActiveTool (Tool::Glue);   break;
                    default: break;
                }
                repaint(); trackStrip.repaint();
            });
    }

    void duplicateClipToNextTrack (int srcTrack, int srcClipIdx)
    {
        const int dstTrack = srcTrack + 1;
        MidiClip* src = engine.getClip (srcTrack, srcClipIdx);
        if (! src || dstTrack >= engine.getNumTracks()) return;
        const auto srcInfo = engine.getClipInfo (srcTrack, srcClipIdx);
        // Add a new clip on the destination track at the same start position
        const int newIdx = engine.addClip (dstTrack, srcInfo.startTick, srcInfo.lengthTicks);
        MidiClip* dst = engine.getClip (dstTrack, newIdx);
        if (! dst) return;
        const juce::ScopedReadLock sl (src->getLock());
        dst->setNotes (src->getNotes());
        repaint();
    }

    //==========================================================================
    //  Tool handlers — clip-grid equivalents of PianoRollComponent's
    //  Draw/Erase/Split/Glue handlers, operating on whole clips.
    //==========================================================================
    /** Starts a drawn clip: the clip isn't created yet — mouseDrag grows
     *  drawLenTicks live as the user drags right, and mouseUp commits it at
     *  whatever length that ended up at (floored to 1 bar for a plain
     *  click with no real drag, matching the double-click-to-create
     *  shortcut's clip length). */
    void handleDrawClipDown (int trackIdx, const juce::MouseEvent& e)
    {
        drawStartTick = snapTick (xToTick (e.x));
        drawLenTicks  = MidiClip::kPPQ * 4;   // 1-bar floor while not yet dragged
        dragMode  = DragMode::DrawClip;
        dragTrack = trackIdx;
    }

    void handleEraseClipDown (int trackIdx, int clipIdx)
    {
        engine.removeClip (trackIdx, clipIdx);
        if (selectedTrack == trackIdx && selectedClip == clipIdx)
            selectedClip = 0;
    }

    /** Cuts the clicked clip into two clips at the click point: the original
     *  clip is shortened in place, and a new clip is created for the tail,
     *  each keeping only the notes (re-based to their own clip-local ticks)
     *  that fall on their side of the cut. No-ops if the click lands too
     *  close to either end to leave two clips with positive length. */
    void handleSplitClipDown (int trackIdx, int clipIdx, const juce::MouseEvent& e)
    {
        MidiClip* clip = engine.getClip (trackIdx, clipIdx);
        if (! clip) return;

        const auto info = engine.getClipInfo (trackIdx, clipIdx);
        const int64_t cutTick = snapTick (xToTick (e.x));
        const int64_t cutOffsetInClip = cutTick - info.startTick;
        if (cutOffsetInClip <= 0 || cutOffsetInClip >= info.lengthTicks)
            return;   // click wasn't inside this clip's body

        juce::Array<MidiNote> headNotes, tailNotes;
        {
            const juce::ScopedReadLock sl (clip->getLock());
            for (const auto& n : clip->getNotes())
            {
                if (n.startTick < cutOffsetInClip)
                    headNotes.add (n);
                else
                {
                    MidiNote moved = n;
                    moved.startTick -= cutOffsetInClip;
                    tailNotes.add (moved);
                }
            }
        }

        const int64_t tailLen = info.lengthTicks - cutOffsetInClip;
        const int tailIdx = engine.addClip (trackIdx, cutTick, tailLen);
        if (MidiClip* tail = engine.getClip (trackIdx, tailIdx))
            tail->setNotes (tailNotes);

        engine.setClipLengthTicks (trackIdx, clipIdx, cutOffsetInClip);
        clip->setNotes (headNotes);

        selectTrack (trackIdx);
        selectedClip = clipIdx;
    }

    /** Merges the clicked clip with the next clip on the same track (the
     *  clip with the lowest startTick that is >= this clip's end), extending
     *  the clicked clip to cover both and re-basing the merged-in clip's
     *  notes by its start offset relative to the clicked clip. No-ops if
     *  there's no next clip on the track. */
    void handleGlueClipDown (int trackIdx, int clipIdx)
    {
        MidiClip* clip = engine.getClip (trackIdx, clipIdx);
        if (! clip) return;
        const auto info = engine.getClipInfo (trackIdx, clipIdx);

        int nextIdx = -1;
        int64_t nextStart = std::numeric_limits<int64_t>::max();
        const int numClips = engine.getNumClips (trackIdx);
        for (int ci = 0; ci < numClips; ++ci)
        {
            if (ci == clipIdx) continue;
            const auto ci_info = engine.getClipInfo (trackIdx, ci);
            if (ci_info.startTick >= info.endTick() && ci_info.startTick < nextStart)
            {
                nextStart = ci_info.startTick;
                nextIdx   = ci;
            }
        }
        if (nextIdx < 0) return;   // nothing to glue to

        MidiClip* next = engine.getClip (trackIdx, nextIdx);
        if (! next) return;
        const auto nextInfo = engine.getClipInfo (trackIdx, nextIdx);
        const int64_t offset = nextInfo.startTick - info.startTick;

        juce::Array<MidiNote> merged;
        {
            const juce::ScopedReadLock sl (clip->getLock());
            merged = clip->getNotes();
        }
        {
            const juce::ScopedReadLock sl (next->getLock());
            for (const auto& n : next->getNotes())
            {
                MidiNote moved = n;
                moved.startTick += offset;
                merged.add (moved);
            }
        }

        engine.setClipLengthTicks (trackIdx, clipIdx, offset + nextInfo.lengthTicks);
        clip->setNotes (merged);
        engine.removeClip (trackIdx, nextIdx);

        // If the removed clip's index was below ours, the clicked clip's own
        // index has now shifted down by one to fill the gap.
        selectTrack (trackIdx);
        selectedClip = (nextIdx < clipIdx) ? clipIdx - 1 : clipIdx;
    }

    //==========================================================================
    //  Painting
    //==========================================================================
    void paintArrangeHeader (juce::Graphics& g) const
    {
        const auto& theme = getTheme();
        const juce::Rectangle<int> header (3, kTransportH + 3, kLeftW, kRulerH);
        g.setColour (theme.header);
        g.fillRect (header);
        g.setColour (theme.accent.withAlpha (0.8f));
        g.fillRect (header.getX(), header.getY(), 3, header.getHeight());
        g.setColour (theme.foreground.withAlpha (0.92f));
        g.setFont (juce::Font (11.5f, juce::Font::bold));
        g.drawText ("ARRANGE", header.reduced (12, 0), juce::Justification::centredLeft, false);
        g.setColour (theme.foreground.withAlpha (0.45f));
        g.setFont (juce::Font (9.0f));
        g.drawText ("TRACKS", header.reduced (10, 0), juce::Justification::centredRight, false);
        g.setColour (theme.separator);
        g.fillRect (header.getX(), header.getBottom() - 1, header.getWidth(), 1);
    }

    void paintRuler (juce::Graphics& g) const
    {
        // Ruler content is horizontally scrolled; keep it out of the fixed
        // ARRANGE header when loop markers move beyond the visible timeline.
        g.saveState();
        g.reduceClipRegion (rulerBounds);

        const auto& theme = getTheme();

        // Background
        g.setColour (theme.header);
        g.fillRect (rulerBounds);

        // Bottom border
        g.setColour (theme.separator);
        g.fillRect (rulerBounds.getX(), rulerBounds.getBottom() - 1,
                    rulerBounds.getWidth(), 1);

        // Loop region shading inside ruler — a loop range can be set (e.g.
        // from the transport's L/R locators) without looping actually being
        // switched on, so this should only paint while the engine is
        // actually looping, not merely whenever a range happens to exist.
        if (engine.isLooping() && loopStart >= 0 && loopEnd > loopStart)
        {
            const float lx = tickToX (loopStart);
            const float rx = tickToX (loopEnd);
            g.setColour (theme.accent.withAlpha (0.22f));
            g.fillRect (lx, (float)rulerBounds.getY(), rx - lx,
                        (float)rulerBounds.getHeight());
        }

        const int64_t ppq    = MidiClip::kPPQ;
        const int64_t barLen = ppq * 4;
        const int     gx     = clipGridBounds.getX();
        const int     gw     = clipGridBounds.getWidth();
        const int64_t total  = totalVisibleTicks();

        // Decide beat/bar visibility based on zoom
        const double pxPerBar  = barLen * pixelsPerTick;
        const double pxPerBeat = ppq  * pixelsPerTick;
        const bool showBeats   = pxPerBeat >= 6.0;

        // Beat ticks — same opacity+width scheme as paintGridLines (bars:
        // 2px, theme.separator at 95% alpha; beats: 1px, theme.gridLine at
        // 28% alpha) rather than the old height-based bar/beat distinction,
        // so the ruler and the row grid read as one system.
        if (showBeats)
        {
            const int64_t firstBeat = (int64_t)(scrollX / (pixelsPerTick * ppq));
            const int64_t lastBeat  = firstBeat + (int64_t)(gw / (pixelsPerTick * ppq)) + 2;
            for (int64_t b = firstBeat; b <= lastBeat && b * ppq <= total; ++b)
            {
                const int x = gx + (int)((b * ppq) * pixelsPerTick - scrollX);
                if (x < gx || x > gx + gw) continue;
                const bool isBar = (b % 4 == 0);
                g.setColour (isBar ? theme.separator.withAlpha (0.95f)
                                    : theme.gridLine.withAlpha (0.28f));
                g.fillRect (x, rulerBounds.getY(), isBar ? 2 : 1, rulerBounds.getHeight());
            }
        }

        // Bar numbers
        g.setFont (juce::Font (11.f, juce::Font::bold));
        const int64_t firstBar = (int64_t)(scrollX / (pixelsPerTick * barLen));
        const int64_t lastBar  = firstBar + (int64_t)(gw / (pixelsPerTick * barLen)) + 2;
        for (int64_t bar = firstBar; bar <= lastBar && bar * barLen <= total; ++bar)
        {
            const int x = gx + (int)((bar * barLen) * pixelsPerTick - scrollX);
            if (x < gx || x > gx + gw) continue;
            g.setColour (theme.foreground.withAlpha (0.78f));
            g.drawText (juce::String (bar + 1),
                        x + 3, rulerBounds.getY(),
                        48, rulerBounds.getHeight(),
                        juce::Justification::centredLeft, false);
        }

        // Loop L / R labels
        if (loopStart >= 0 && loopEnd > loopStart)
        {
            const float lx = tickToX (loopStart);
            const float rx = tickToX (loopEnd);
            g.setFont (juce::Font (9.f, juce::Font::bold));
            g.setColour (getTheme().accent.brighter (0.2f));

            if (rulerBounds.contains ((int)lx, rulerBounds.getCentreY()))
            {
                g.drawText ("L", (int)lx + 2, rulerBounds.getY(),
                            14, rulerBounds.getHeight(), juce::Justification::centredLeft);
                g.drawVerticalLine ((int)lx, (float)rulerBounds.getY(),
                                    (float)rulerBounds.getBottom());
            }

            if (rulerBounds.contains ((int)rx, rulerBounds.getCentreY()))
            {
                g.drawText ("R", (int)rx - 16, rulerBounds.getY(),
                            14, rulerBounds.getHeight(), juce::Justification::centredRight);
                g.drawVerticalLine ((int)rx, (float)rulerBounds.getY(),
                                    (float)rulerBounds.getBottom());
            }
        }

        g.restoreState();
    }

    void paintTrackRows (juce::Graphics& g) const
    {
        const int n = engine.getNumTracks();
        for (int i = 0; i < n; ++i)
        {
            const int rowTop = trackTopY (i);
            if (rowTop + trackH < clipGridBounds.getY()) continue;  // above view
            if (rowTop > clipGridBounds.getBottom()) break;          // below view
            paintOneTrack (g, i);
        }

        // Empty space below the last track never got a row painted above, but
        // the grid still needs to continue through it so it doesn't look
        // "gone" once you scroll (or zoom) past the end of the track list.
        const int contentBottom = trackTopY (n);
        if (contentBottom < clipGridBounds.getBottom())
        {
            const juce::Rectangle<int> emptyR (
                clipGridBounds.getX(),
                contentBottom,
                clipGridBounds.getWidth(),
                clipGridBounds.getBottom() - contentBottom);

            g.saveState();
            g.reduceClipRegion (clipGridBounds);

            const auto& theme = getTheme();
            g.setColour (theme.waveformBg);
            g.fillRect (emptyR);

            paintGridLines (g, emptyR);

            g.restoreState();
        }
    }

    void paintLoopOverlay (juce::Graphics& g) const
    {
        // A loop range can be set (e.g. from the transport's L/R locators)
        // without looping actually being switched on — only paint this
        // overlay while the engine is actually looping.
        if (! engine.isLooping() || loopStart < 0 || loopEnd <= loopStart) return;
        const float lx = tickToX (loopStart);
        const float rx = tickToX (loopEnd);

        // The loop can extend beyond either side of the viewport. Clip all
        // overlay painting so it cannot bleed into the fixed track strip.
        g.saveState();
        g.reduceClipRegion (clipGridBounds);

        // DYSEKT-METRO pass: no translucent wash across the track rows — a
        // solid fill there would just hide every clip underneath it, and a
        // wash is the one thing this pass is meant to remove. The loop
        // range instead reads as full-opacity boundary lines at the loop's
        // start/end, rather than a colour tint over the whole timeline.
        const auto& theme = getTheme();
        g.setColour (theme.accent);
        g.drawVerticalLine ((int)lx,
                            (float)clipGridBounds.getY(),
                            (float)clipGridBounds.getBottom());
        g.drawVerticalLine ((int)rx,
                            (float)clipGridBounds.getY(),
                            (float)clipGridBounds.getBottom());

        g.restoreState();
    }

    /** Draws the live rubber-band selection rectangle while the user is
     *  dragging in empty space. Clipped to the clip grid so it can't paint
     *  over the ruler or track strip. */
    void paintRubberBand (juce::Graphics& g) const
    {
        if (dragMode != DragMode::RubberBand || rubberBandRect.isEmpty()) return;

        g.saveState();
        g.reduceClipRegion (clipGridBounds);

        // DYSEKT-METRO pass: solid outline only — a solid fill would hide
        // the clips being selected, so the rectangle reads via a
        // full-opacity border instead of a wash-plus-border combo.
        const auto& theme = getTheme();
        g.setColour (theme.accent);
        g.drawRect (rubberBandRect, 2);

        g.restoreState();
    }

    /** Live preview of the clip being drawn with the Draw tool — same
     *  visual language as the rubber-band rect, drawn in the target
     *  track's row so it reads as "this is where the clip will land". */
    void paintDrawClipPreview (juce::Graphics& g) const
    {
        if (dragMode != DragMode::DrawClip || ! juce::isPositiveAndBelow (dragTrack, engine.getNumTracks()))
            return;

        const int x = (int) tickToX (drawStartTick);
        const int w = juce::jmax (kMinClipPx, (int) (drawLenTicks * pixelsPerTick));
        const juce::Rectangle<int> r (x, trackTopY (dragTrack), w, trackH - 1);

        g.saveState();
        g.reduceClipRegion (clipGridBounds);

        // DYSEKT-METRO pass: solid accent-colour block rather than a
        // translucent fill + border — since it's landing in previously
        // empty space there's nothing underneath to preserve, and using
        // the accent colour (instead of the target track's own colour)
        // is what marks this as "still a preview", not translucency.
        const auto& theme = getTheme();
        g.setColour (theme.accent);
        g.fillRect (r);

        g.restoreState();
    }

    void paintOneTrack (juce::Graphics& g, int i) const
    {
        const auto info  = engine.getTrackInfo (i);
        const bool muted = ! info.enabled;

        const juce::Rectangle<int> rowR (
            clipGridBounds.getX(),
            trackTopY (i),
            clipGridBounds.getWidth(),
            trackH);

        // Clip to grid area
        g.saveState();
        g.reduceClipRegion (clipGridBounds);

        // DYSEKT-METRO pass: rows no longer lean on a barely-visible
        // brightness alternation — every row is one flat colour, and the
        // "seam" between tracks is a solid dark gap rather than a 1px
        // hairline, so the row boundaries read clearly even at a glance.
        // Row background stays neutral regardless of track selection — a
        // whole-row tint (even the old subtle alpha wash) reads as "this
        // whole row, including empty grid space, is highlighted", when
        // only the clip itself should carry that signal. See paintClip's
        // accent-line treatment for the actual selection indicator.
        const auto& theme = getTheme();
        g.setColour (theme.waveformBg);
        g.fillRect (rowR.withTrimmedBottom (2));
        g.setColour (juce::Colour (0xFF000000));
        g.fillRect (rowR.withTop (rowR.getBottom() - 2));

        // Vertical grid lines
        paintGridLines (g, rowR);

        // Clips — paint all slots on this track
        const int numClips = engine.getNumClips (i);
        for (int ci = 0; ci < numClips; ++ci)
        {
            const bool isSelClip = isClipSelected (i, ci);
            paintClip (g, i, ci, info, isSelClip, muted);
        }

        g.restoreState();
    }

    void paintGridLines (juce::Graphics& g, const juce::Rectangle<int>& rowR) const
    {
        const int64_t ppq    = MidiClip::kPPQ;
        const int64_t barLen = ppq * 4;
        const int64_t total  = totalVisibleTicks();
        const bool showBeats = (ppq * pixelsPerTick) >= 4.0;
        const auto& theme = getTheme();

        const int64_t firstBeat = (int64_t)(scrollX / (pixelsPerTick * ppq));
        const int64_t lastBeat  = firstBeat + (int64_t)(rowR.getWidth() / (pixelsPerTick * ppq)) + 2;

        for (int64_t b = firstBeat; b <= lastBeat && b * ppq <= total; ++b)
        {
            const int x = clipGridBounds.getX() + (int)((b * ppq) * pixelsPerTick - scrollX);
            if (x < rowR.getX() || x > rowR.getRight()) continue;
            const bool isBar = (b % 4 == 0);
            if (!showBeats && !isBar) continue;
            g.setColour (isBar ? theme.separator.withAlpha (0.95f) : theme.gridLine.withAlpha (0.28f));
            g.fillRect (x, rowR.getY(), isBar ? 2 : 1, rowR.getHeight() - 1);
        }

        // Sub-beat grid lines at the live GRID/quantize resolution — mirrors
        // PianoRollComponent's snap-grid pass so the arranger's grid visibly
        // reacts to the same GRID combo that drives snapTick(), instead of
        // always showing quarter-note/bar lines regardless of that setting.
        const int64_t snap = currentSnapTicks();
        if (snap > 0 && pixelsPerTick * (double) snap > 4.0)
        {
            g.setColour (theme.gridLine.withAlpha (0.24f));
            const int64_t startSnap = (int64_t)(scrollX / (pixelsPerTick * (double) snap)) * snap;
            for (int64_t t = startSnap; t <= total; t += snap)
            {
                if (t % ppq == 0) continue; // already drawn as a beat/bar line above
                const int x = clipGridBounds.getX() + (int)(t * pixelsPerTick - scrollX);
                if (x > rowR.getRight()) break;
                if (x < rowR.getX()) continue;
                g.fillRect (x, rowR.getY(), 1, rowR.getHeight() - 1);
            }
        }
    }

    void paintClip (juce::Graphics& g, int i, int ci,
                    const SequencerTrackInfo& info,
                    bool isSel, bool muted) const
    {
        const auto clipR = clipRectForClip (i, ci);
        if (! clipGridBounds.intersects (clipR)) return;

        // DYSEKT-METRO pass: clips are solid flat tiles now — full-opacity
        // track colour, square corners (fillRect, not a 0-radius rounded
        // rect), no border and no left accent bar. Muted gets its own
        // desaturated tile colour rather than a lowered-alpha version of
        // the same colour, since a translucent tile reads as "half a tile"
        // once every other clip on the canvas is solid.
        const juce::Colour tile = muted
            ? info.colour.withSaturation (0.10f).withBrightness (0.30f)
            : info.colour;

        g.setColour (tile);
        g.fillRect (clipR.reduced (1, 1));

        // Selection reads as a single accent-colour edge line rather than a
        // brightened outline — an outline implies "this shape has a
        // border", which doesn't fit tiles that otherwise have none.
        if (isSel)
        {
            g.setColour (getTheme().accent);
            g.fillRect (clipR.getX() + 1, clipR.getY() + 1, clipR.getWidth() - 2, 3);
        }

        // Track name
        if (trackH >= 20)
        {
            g.setFont (juce::Font (juce::jmin (11.f, (float)trackH * 0.22f), juce::Font::bold));
            g.setColour (muted ? juce::Colours::white.withAlpha (0.45f)
                               : juce::Colours::white.withAlpha (0.92f));
            g.drawText (info.name,
                        clipR.getX() + 6, clipR.getY() + 2,
                        juce::jmax (0, clipR.getWidth() - 26),
                        juce::jmax (0, (int)(trackH * 0.38f)),
                        juce::Justification::centredLeft, true);
        }

        // Track type badge
        if (clipR.getWidth() > 32 && trackH >= 20)
        {
            juce::String badge;
            switch (info.type)
            {
                case TrackType::MainSlice:      badge = "SL"; break;
                case TrackType::ChromaticSlice: badge = "CH"; break;
                case TrackType::SfPlayer:       badge = "SF"; break;
            }
            g.setFont (juce::Font (8.f));
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.drawText (badge,
                        clipR.getRight() - 22, clipR.getY() + 2,
                        20, 12,
                        juce::Justification::centredRight, false);
        }

        // Resize handle — solid, no alpha layering. Selected clips get a
        // full-strength handle; unselected clips get it only on hover, so
        // the cursor change and the visual affordance still arrive
        // together, just via a colour swap rather than a fade-in.
        const bool isHoverHandle = (! isSel && i == hoverTrack && ci == hoverClip);
        if (isSel || isHoverHandle)
        {
            const juce::Rectangle<int> handleR (
                clipR.reduced (1, 1).withLeft (clipR.getRight() - 8));
            g.setColour (isSel ? juce::Colours::black.withAlpha (0.35f)
                               : juce::Colours::black.withAlpha (0.18f));
            g.fillRect (handleR);
            g.setColour (juce::Colours::white.withAlpha (isSel ? 0.55f : 0.30f));
            const float cx = (float) handleR.getCentreX();
            for (int dot = 0; dot < 3; ++dot)
            {
                const float dy = handleR.getY() + handleR.getHeight() * (0.25f + dot * 0.25f);
                g.fillRect (juce::Rectangle<float> (2.f, 2.f).withCentre ({ cx, dy }));
            }
        }

        // Mini note preview
        paintNotePreview (g, i, ci, clipR, tile, muted);
    }

    void paintNotePreview (juce::Graphics& g, int trackIdx, int clipIdx,
                           juce::Rectangle<int> clipR,
                           juce::Colour base, bool muted) const
    {
        const MidiClip* clip = engine.getClip (trackIdx, clipIdx);
        if (! clip) return;
        const int64_t clipLen = clip->getLengthTicks();
        if (clipLen <= 0) return;

        const int headerH  = (int)(trackH * 0.38f);
        const int previewY = clipR.getY() + headerH;
        const int previewH = clipR.getHeight() - headerH - 3;
        if (previewH < 4) return;

        g.saveState();
        g.reduceClipRegion (clipR.withTrimmedTop (headerH).withTrimmedBottom (2));

        // Bar-boundary grid lines behind the notes
        {
            const int64_t ppq    = MidiClip::kPPQ;
            const int64_t barLen = ppq * 4;
            g.setColour (base.brighter (0.15f).withAlpha (muted ? 0.10f : 0.18f));
            for (int64_t t = barLen; t < clipLen; t += barLen)
            {
                const float gx = clipR.getX() + (float)t / (float)clipLen * clipR.getWidth();
                g.fillRect (juce::Rectangle<float> (gx, (float)previewY, 1.f, (float)previewH));
            }
        }

        {
            const juce::ScopedReadLock sl (clip->getLock());
            const auto& notes = clip->getNotes();
            if (notes.isEmpty()) { g.restoreState(); return; }

            int loNote = 127, hiNote = 0;
            for (const auto& n : notes)
            {
                loNote = juce::jmin (loNote, n.note);
                hiNote = juce::jmax (hiNote, n.note);
            }
            const int range = juce::jmax (12, hiNote - loNote + 2);

            for (const auto& n : notes)
            {
                if (n.startTick >= clipLen) continue;
                const float nx = clipR.getX()
                    + (float)(n.startTick) / (float)clipLen * clipR.getWidth();
                const float nw = juce::jmax (1.5f,
                    (float)(n.durationTick) / (float)clipLen * clipR.getWidth() - 0.5f);
                const float pitch = (float)(n.note - loNote) / (float)range;
                const float ny = (float)previewY + (1.f - pitch) * (float)(previewH - 3);
                const float nh = juce::jmax (1.5f, (float)previewH / (float)range);
                const float va = muted ? 0.22f : (0.4f + 0.5f * n.velocity / 127.f);
                g.setColour (base.brighter (0.2f).withAlpha (va));
                g.fillRoundedRectangle(nx, ny, nw, nh, 0.0f);
            }
        }

        g.restoreState();
    }

    void paintPlayhead (juce::Graphics& g) const
    {
        if (clipGridBounds.isEmpty()) return;
        const int64_t tick  = engine.getPlayheadTick();
        const int64_t total = totalVisibleTicks();
        if (tick < 0 || tick > total) return;

        const int x = (int)tickToX (tick);
        if (x < clipGridBounds.getX() || x > clipGridBounds.getRight()) return;

        const auto playheadColour = juce::Colours::white.withAlpha (0.96f);

        // Line through ruler + tracks
        g.setColour (playheadColour);
        g.fillRect (x - 1, rulerBounds.getY(),
                    2, rulerBounds.getHeight() + clipGridBounds.getHeight());

        // Small triangular cap in the ruler, apex pointing down at the line
        const float capW = 7.0f, capH = 6.0f;
        juce::Path cap;
        cap.addTriangle ((float) x - capW * 0.5f, (float) rulerBounds.getY(),
                          (float) x + capW * 0.5f, (float) rulerBounds.getY(),
                          (float) x,               (float) rulerBounds.getY() + capH);
        g.setColour (playheadColour);
        g.fillPath (cap);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrangeView)
};
