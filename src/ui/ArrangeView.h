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
    static constexpr int kToolbarH     = 42;
    static constexpr int kRulerH       = 36;
    static constexpr int kScrollH      = 10;
    static constexpr int kScrollW      = 10;
        static constexpr int kMinClipPx    = 6;
    static constexpr int kResizeZone   = 10;
    static constexpr int kDefaultTrackH = 72;
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
          transport (seq, link),
          trackStrip (seq),
          inspector (seq)
    {
        transport.setDocked (true);
        addAndMakeVisible (transport);
        addAndMakeVisible (inspector);
        inspector.setVisible (false);
        addAndMakeVisible (trackStrip);

        transport.onFloatRequested = [this] { showFloatingTransport(); };
        transport.onDockRequested  = [this] { dockTransport(); };
        transport.onMixerRequested    = [this] { if (onMixerRequested)   onMixerRequested(); };
        transport.onArrangerRequested = [this] { if (onArrangerRequested) onArrangerRequested(); };
        transport.onGlobalEqRequested = [this] { if (onGlobalEqRequested) onGlobalEqRequested(); };

        // ── Quantize buttons — same six resolutions/ids as the transport's
        // GRID combo, same radio-group approach as FloatingTransportBar's
        // own gridButtons (radioGroupId + clickingTogglesState(true), so
        // JUCE itself enforces exactly one active at a time — the earlier
        // "flip the matching one on by hand" version never turned the old
        // selection off, which is why multiple buttons could stay lit at
        // once). Each click still just forwards to transport.setSnapItemId(),
        // which stays the single source of truth for the actual snap value.
        {
            static constexpr int kQuantizeButtonRadioGroup = 9101;
            static const char* const kQuantizeLabels[kNumQuantizeOptions] =
                { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32" };
            for (int i = 0; i < kNumQuantizeOptions; ++i)
            {
                auto& b = quantizeButtons[i];
                const int itemId = i + 1;
                b.setButtonText (kQuantizeLabels[i]);
                b.setTooltip ("Grid snap: " + juce::String (kQuantizeLabels[i]));
                b.setRadioGroupId (kQuantizeButtonRadioGroup, juce::dontSendNotification);
                b.setClickingTogglesState (true);
                b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff1c2028));
                b.setColour (juce::TextButton::buttonOnColourId, getTheme().accent);
                b.setColour (juce::TextButton::textColourOffId,  juce::Colours::white.withAlpha (0.55f));
                b.setColour (juce::TextButton::textColourOnId,   juce::Colours::black.withAlpha (0.85f));
                b.getProperties().set ("flatFill", true);
                b.onClick = [this, itemId] { transport.setSnapItemId (itemId); };
                addAndMakeVisible (b);
            }
            quantizeButtons[4].setToggleState (true, juce::dontSendNotification); // 1/16, matches transport's initial GRID selection
        }

        // ── Arranger command toolbar ───────────────────────────────────────
        // These buttons call the already-existing engine/tool operations.
        {
            static const char* const labels[kNumArrangeTools] =
                { "+ CLIP", "SELECT", "DRAW", "ERASE", "SPLIT", "GLUE", "LOCK", "INSPECTOR" };

            for (int i = 0; i < kNumArrangeTools; ++i)
            {
                auto& b = arrangerButtons[i];
                b.setButtonText (labels[i]);
                b.setTooltip (labels[i]);
                b.setColour (juce::TextButton::buttonColourId, juce::Colour (0xff11171c));
                b.setColour (juce::TextButton::buttonOnColourId, getTheme().accent);
                b.setColour (juce::TextButton::textColourOffId, juce::Colours::white.withAlpha (0.72f));
                b.setColour (juce::TextButton::textColourOnId, juce::Colours::black.withAlpha (0.9f));
                b.getProperties().set ("flatFill", true);
                b.getProperties().set ("transportFontSize", 15.0);
                addAndMakeVisible (b);
            }

            arrangerButtons[0].onClick = [this]
            {
                const int t = juce::isPositiveAndBelow (selectedTrack, engine.getNumTracks())
                            ? selectedTrack : 0;
                if (juce::isPositiveAndBelow (t, engine.getNumTracks()))
                {
                    const int64_t tick = snapTick (engine.getPlayheadTick());
                    const int idx = engine.addClip (t, tick, MidiClip::kPPQ * 4);
                    if (idx >= 0)
                        selectSingleClip (t, idx);
                }
                repaint();
            };

            arrangerButtons[1].onClick = [this] { setActiveTool (Tool::Select); };
            arrangerButtons[2].onClick = [this] { setActiveTool (Tool::Draw); };
            arrangerButtons[3].onClick = [this] { setActiveTool (Tool::Erase); };
            arrangerButtons[4].onClick = [this] { setActiveTool (Tool::Split); };
            arrangerButtons[5].onClick = [this] { setActiveTool (Tool::Glue); };
            arrangerButtons[6].onClick = [this]
            {
                editingLocked = ! editingLocked;
                arrangerButtons[6].setToggleState (editingLocked, juce::dontSendNotification);
                repaint();
            };

            arrangerButtons[7].onClick = [this]
            {
                inspectorVisible = ! inspectorVisible;
                inspector.setVisible (inspectorVisible);
                arrangerButtons[7].setToggleState (inspectorVisible, juce::dontSendNotification);
                resized();
                repaint();
            };

            arrangerButtons[1].setToggleState (true, juce::dontSendNotification);
            arrangerButtons[6].setToggleState (false, juce::dontSendNotification);
            arrangerButtons[7].setToggleState (false, juce::dontSendNotification);
        }

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

        // Quantize buttons fill the header to the right of the "QUANTIZE"
        // label — see paintArrangeHeader(). Laid out here (rather than
        // computed fresh in paint()) so hit-testing and painting always
        // agree on the same rectangle. Uses arrangeHeaderBounds() rather than
        // a fixed kTransportH + 3 offset so the row (and its buttons) slide
        // up to sit right under the title bar when the transport is
        // floating and no longer reserves kTransportH at the top — same
        // vertical shift the ruler/track rows already get from r above.
        {
            const auto header = arrangeHeaderBounds();
            auto content = header.withWidth (340).reduced (8, 4);
            content.removeFromLeft (100);   // reserve room for the larger "QUANTIZE" label
            quantizeButtonsBounds = content;

            const int gap = 4;
            const int bw  = (content.getWidth() - gap * (kNumQuantizeOptions - 1)) / kNumQuantizeOptions;
            auto row = content;
            for (int i = 0; i < kNumQuantizeOptions; ++i)
            {
                quantizeButtons[i].setBounds (row.removeFromLeft (bw));
                row.removeFromLeft (gap);
            }

            // Command toolbar occupies the timeline side of the same header
            // row, leaving QUANTIZE + its six resolution buttons untouched.
            auto tools = arrangeHeaderBounds().withTrimmedLeft (350).reduced (4, 3);
            const int toolGap = 4;
            const int widths[kNumArrangeTools] = { 66, 62, 52, 58, 56, 52, 52, 82 };
            int x = tools.getX();
            for (int i = 0; i < kNumArrangeTools; ++i)
            {
                const int w = juce::jmin (widths[i], tools.getRight() - x);
                arrangerButtons[i].setBounds (x, tools.getY(), juce::jmax (0, w), tools.getHeight());
                x += w + toolGap;
            }
        }

        // Corner square between the two scrollbars
        auto cornerR = r;
        cornerR = cornerR.removeFromBottom (kScrollH).removeFromRight (kScrollW);

        auto hScrollR = r.removeFromBottom (kScrollH).withTrimmedRight (kScrollW);
        auto vScrollR = r.removeFromRight  (kScrollW);

        hScroll.setBounds (hScrollR.withTrimmedLeft (leftPanelW()));
        vScroll.setBounds (vScrollR);

        if (inspectorVisible)
        {
            auto inspectorCol = r.removeFromLeft (kInspectorW);
            inspectorCol.removeFromTop (kToolbarH + kRulerH);
            inspector.setBounds (inspectorCol);
        }
        else
            inspector.setBounds ({});

        auto leftCol = r.removeFromLeft (kStripW);
        leftCol.removeFromTop (kToolbarH + kRulerH);
        trackStrip.setBounds (leftCol);
        trackStrip.setTrackHeight (trackH);

        gridArea      = r;
        gridArea.removeFromTop (kToolbarH);
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
        if (getWidth() > leftPanelW() + 8 && getHeight() > kTransportH + kScrollH + 8)
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
                engine.resetLoopRangeToDefault();
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

        // LOCK leaves selection/navigation available but blocks
        // destructive/drawing tool actions.
        if (editingLocked && currentTool != Tool::Select)
        {
            repaint();
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
            if (editingLocked) { repaint(); return; }
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
            if (editingLocked) { beginClipSelection (trackIdx, hitClip, e.mods.isShiftDown()); repaint(); return; }
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
     *  FloatingTransportBar instance throughout, just reparented; see
     *  that class's header comment. Wired to transport.onFloatRequested. */
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
    FloatingTransportBar  transport;
    TrackInspector        inspector;
    TrackHeaderStrip      trackStrip;
    ZoomableScrollBar     hScroll { false };
    ZoomableScrollBar     vScroll { true  };

    // Quick-access quantize/grid-snap buttons, drawn in the ARRANGE header
    // (between the "ARRANGE" and "TRACKS" labels — see paintArrangeHeader())
    // rather than only living in the transport's GRID combo/gridButtons.
    // Same six resolutions, same item ids, as FloatingTransportBar's
    // gridCombo, so a click here just forwards to transport.setSnapItemId()
    // and transport.getSnapTicks()/currentSnapTicks() stays the single source
    // of truth for what clip create/move/resize/split actually snap
    // to — these buttons are a second control surface for that one value,
    // not a second value. Mirrored back from transport in timerCallback(),
    // same "poll and mirror" approach FloatingTransportBar uses for its own
    // gridButtons vs gridCombo.
    static constexpr int kNumQuantizeOptions = 6;
    juce::TextButton      quantizeButtons[kNumQuantizeOptions];
    juce::Rectangle<int>  quantizeButtonsBounds;

    // Quick-access arranger commands. These are deliberately wired to the
    // same engine/tool handlers used by the existing right-click Tool menu,
    // so the toolbar is a second control surface, not a second editor.
    static constexpr int kNumArrangeTools = 8;
    juce::TextButton arrangerButtons[kNumArrangeTools];
    bool editingLocked = false;
    bool inspectorVisible = false;

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
        // transport's SET LEFT / SET RIGHT buttons or editable L/R fields,
        // which previously never reached this view at all. Skipped while
        // the user is actively dragging out a new region on the ruler
        // (mouseUp below commits that drag to the engine instead), so the
        // live drag preview isn't fought over every timer tick.
        if (rulerDrag != RulerDrag::LoopSet)
        {
            loopStart = engine.getLoopStartTick();
            loopEnd   = engine.getLoopEndTick();
        }

        // Mirror the shared grid-snap selection onto the quantize buttons —
        // keeps them in sync however the resolution last changed, whether
        // that was one of these buttons, the transport's own GRID combo/
        // gridButtons, or external code calling transport.setSnapItemId()
        // directly. Same "poll and mirror" approach FloatingTransportBar's
        // own timerCallback uses to keep its gridButtons in sync with
        // gridCombo — see that class's comment above gridButtons.
        {
            const int selectedId = transport.getSnapItemId();
            if (selectedId >= 1 && selectedId <= kNumQuantizeOptions)
            {
                auto& selectedButton = quantizeButtons[selectedId - 1];
                if (! selectedButton.getToggleState())
                    selectedButton.setToggleState (true, juce::dontSendNotification);
            }
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
     *  (rounds a tick to this resolution), paintRuler(), and
     *  paintGridLines() (draws sub-beat lines at this resolution) so the
     *  visible ruler and grid always match
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
     *  leaves the whole group selected (so it can be dragged together);
     *  a plain click on a clip that ISN'T already selected replaces the
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
                const auto r = clipRectForClip (ti, ci);
                if (r.intersects (rubberBandRect)
                    && ! isClipSelected (ti, ci))
                    selectedClips.emplace_back (ti, ci);
            }
        }

        if (! selectedClips.empty())
        {
            selectTrack (selectedClips.back().first);
            selectedClip = selectedClips.back().second;
        }
        else
        {
            selectedTrack = -1;
            selectedClip = 0;
            if (onTrackTypeSelected)
                onTrackTypeSelected (TrackType::MainSlice, false, false, -1, -1, -1);
        }
        trackStrip.setSelectedTrack (selectedTrack);
    }

    void setActiveTool (Tool t)
    {
        currentTool = t;
        for (int i = 0; i < kNumArrangeTools; ++i)
            if (i >= 1 && i <= 5)
                arrangerButtons[i].setToggleState (false, juce::dontSendNotification);

        int idx = 1;
        switch (t)
        {
            case Tool::Select: idx = 1; break;
            case Tool::Draw:   idx = 2; break;
            case Tool::Erase:  idx = 3; break;
            case Tool::Split:  idx = 4; break;
            case Tool::Glue:   idx = 5; break;
        }
        arrangerButtons[idx].setToggleState (true, juce::dontSendNotification);
        repaint();
    }

    //=========================================================================
    //  Context menu
    //=========================================================================
    void showContextMenu (int trackIdx, int clipIdx, const juce::MouseEvent& e)
    {
        juce::PopupMenu menu;

        if (juce::isPositiveAndBelow (trackIdx, engine.getNumTracks()))
        {
            juce::PopupMenu toolMenu;
            toolMenu.addItem (1, "Select", true, currentTool == Tool::Select);
            toolMenu.addItem (2, "Draw",   true, currentTool == Tool::Draw);
            toolMenu.addItem (3, "Erase",  true, currentTool == Tool::Erase);
            toolMenu.addItem (4, "Split",  true, currentTool == Tool::Split);
            toolMenu.addItem (5, "Glue",   true, currentTool == Tool::Glue);
            menu.addSubMenu ("Tool", toolMenu);
        }

        if (clipIdx >= 0)
        {
            menu.addSeparator();
            menu.addItem (20, "Clear clip", true);
            menu.addItem (21, "Delete clip", true);
        }

        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ e.getScreenX(), e.getScreenY(), 1, 1 }),
                            [this, trackIdx, clipIdx] (int result)
        {
            if (result >= 1 && result <= 5)
            {
                const Tool tools[] = { Tool::Select, Tool::Draw, Tool::Erase, Tool::Split, Tool::Glue };
                setActiveTool (tools[result - 1]);
                return;
            }
            if (result == 20 && trackIdx >= 0 && clipIdx >= 0)
            {
                if (MidiClip* c = engine.getClip (trackIdx, clipIdx)) c->clear();
                repaint();
                return;
            }
            if (result == 21 && trackIdx >= 0 && clipIdx >= 0)
            {
                engine.removeClip (trackIdx, clipIdx);
                selectedClips.clear();
                selectedTrack = -1;
                selectedClip = 0;
                trackStrip.setSelectedTrack (-1);
                if (onTrackTypeSelected)
                    onTrackTypeSelected (TrackType::MainSlice, false, false, -1, -1, -1);
                repaint();
            }
        });
    }

    //=========================================================================
    //  Tool handlers
    //=========================================================================
    void handleDrawClipDown (int trackIdx, const juce::MouseEvent& e)
    {
        drawStartTick = snapTick (xToTick (e.x));
        drawLenTicks  = MidiClip::kPPQ * 4;
        dragMode      = DragMode::DrawClip;
        dragTrack     = trackIdx;
        dragClip      = -1;
        repaint();
    }

    void handleEraseClipDown (int trackIdx, int clipIdx)
    {
        engine.removeClip (trackIdx, clipIdx);
        selectedClips.clear();
        selectedTrack = -1;
        selectedClip = 0;
        trackStrip.setSelectedTrack (-1);
        if (onTrackTypeSelected)
            onTrackTypeSelected (TrackType::MainSlice, false, false, -1, -1, -1);
    }

    /** SequencerEngine has no splitClip() API — a split is implemented here
     *  directly on top of the existing clip primitives (getClip/getClipInfo/
     *  addClip/setClipLengthTicks): the original clip is shortened in place
     *  and a new clip is created for the tail, each keeping only the notes
     *  (re-based to their own clip-local ticks) that fall on their side of
     *  the cut. */
    void handleSplitClipDown (int trackIdx, int clipIdx, const juce::MouseEvent& e)
    {
        MidiClip* clip = engine.getClip (trackIdx, clipIdx);
        if (! clip) return;

        const auto info = engine.getClipInfo (trackIdx, clipIdx);
        const int64_t splitTick = snapTick (xToTick (e.x));
        const int64_t cutOffsetInClip = splitTick - info.startTick;
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
        const int tailIdx = engine.addClip (trackIdx, splitTick, tailLen);
        if (MidiClip* tail = engine.getClip (trackIdx, tailIdx))
            tail->setNotes (tailNotes);

        engine.setClipLengthTicks (trackIdx, clipIdx, cutOffsetInClip);
        clip->setNotes (headNotes);

        selectSingleClip (trackIdx, clipIdx);
    }

    /** SequencerEngine has no glueClips() API either — merges the clicked
     *  clip with the following clip on the same track directly, extending
     *  the clicked clip to cover both and re-basing the merged-in clip's
     *  notes by its start offset relative to the clicked clip. */
    void handleGlueClipDown (int trackIdx, int clipIdx)
    {
        if (clipIdx < 0 || clipIdx + 1 >= engine.getNumClips (trackIdx)) return;

        MidiClip* clip = engine.getClip (trackIdx, clipIdx);
        if (! clip) return;
        const auto info = engine.getClipInfo (trackIdx, clipIdx);

        const int nextIdx = clipIdx + 1;
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

        selectSingleClip (trackIdx, clipIdx);
    }

    //=========================================================================
    //  Track selection
    //=========================================================================
    void selectTrack (int idx)
    {
        if (! juce::isPositiveAndBelow (idx, engine.getNumTracks()))
        {
            selectedTrack = -1;
            selectedClip  = 0;
            if (onTrackTypeSelected)
                onTrackTypeSelected (TrackType::MainSlice, false, false, -1, -1, -1);
            trackStrip.setSelectedTrack (-1);
            return;
        }

        selectedTrack = idx;
        if (selectedClip >= engine.getNumClips (idx))
            selectedClip = 0;

        trackStrip.setSelectedTrack (idx);
        const auto info = engine.getTrackInfo (idx);
        const bool isSf2Track = info.type == TrackType::SfPlayer && ! info.isSfzInstrument;
        const int ch1Based = (info.type == TrackType::SfPlayer
                              && info.midiChannel >= 0 && info.midiChannel < 16)
                            ? info.midiChannel + 1 : -1;
        if (onTrackTypeSelected)
            onTrackTypeSelected (info.type, true, info.isSfzInstrument, ch1Based,
                                 isSf2Track ? info.preset.bank   : -1,
                                 isSf2Track ? info.preset.preset : -1);
    }

    //=========================================================================
    //  Painting
    //=========================================================================
    void paintArrangeHeader (juce::Graphics& g)
    {
        const auto& theme = getTheme();
        const auto header = arrangeHeaderBounds();

        // ThemeData has no transportBg — theme.header is the "top bar" fill
        // used for this exact purpose elsewhere (TrackHeaderStrip, etc).
        g.setColour (theme.header);
        g.fillRect (header);

        g.setColour (theme.separator.withAlpha (0.75f));
        g.fillRect (header.getX(), header.getBottom() - 1, header.getWidth(), 1);

        // Header title is intentionally kept large enough to remain legible
        // beside the six quantize controls; the old 14 px caption was too small.
        g.setColour (theme.foreground.withAlpha (0.92f));
        g.setFont (juce::Font (17.0f, juce::Font::bold));
        auto quantizeLabel = header.withWidth (100).reduced (10, 0);
        g.drawText ("QUANTIZE", quantizeLabel, juce::Justification::centredLeft, false);

        // The actual command buttons are child components; this separator
        // visually divides quantize controls from editing commands.
        g.setColour (theme.separator.withAlpha (0.7f));
        g.fillRect (header.getX() + 340, header.getY(), 1, header.getHeight());

        // Quantize buttons (real child components, positioned in resized())
        // fill the rest of this header — nothing else to paint there.
        // The bottom border itself is NOT drawn here — see paintOverChildren()
        // below: trackStrip sits directly under this header, and drawing the
        // border in this paint() pass put it underneath trackStrip's own
        // background fill whenever their bounds touched or overlapped by
        // a rounding pixel, making the border invisible.
    }

    /** Draws the header/track-strip separator after every child (trackStrip,
     *  inspector, quantize buttons) has painted. Component::paintOverChildren()
     *  runs after every child paint (unlike the parent's own paint() pass, above),
     *  so this line is guaranteed to sit on top and stay visible no matter how
     *  trackStrip's bounds line up with the header. */
    void paintOverChildren (juce::Graphics& g) override
    {
        const auto& theme = getTheme();
        const auto header = arrangeHeaderBounds();
        g.setColour (theme.separator.withAlpha (0.85f));
        g.fillRect (header.getX(), header.getBottom() - 1, header.getWidth(), 1);
    }

    juce::Rectangle<int> arrangeHeaderBounds() const noexcept
    {
        const int top = transport.isFloating() ? 3 : kTransportH + 3;
        return { 3, top, juce::jmax (0, getWidth() - 6), kToolbarH };
    }

    void paintRuler (juce::Graphics& g)
    {
        const auto& theme = getTheme();
        g.setColour (theme.waveformBg);
        g.fillRect (rulerBounds);

        g.setColour (theme.separator.withAlpha (0.65f));
        g.fillRect (rulerBounds.getX(), rulerBounds.getBottom() - 1, rulerBounds.getWidth(), 1);

        const int64_t snap = currentSnapTicks();
        const int64_t beat = MidiClip::kPPQ;
        const int64_t bar  = beat * 4;

        const int64_t firstTick = xToTick (rulerBounds.getX());
        const int64_t lastTick  = xToTick (rulerBounds.getRight());
        const int64_t startBar  = firstTick / bar;
        const int64_t endBar    = lastTick / bar + 1;

        g.setFont (juce::Font (12.0f, juce::Font::plain));
        for (int64_t bi = startBar; bi <= endBar; ++bi)
        {
            const int64_t t = bi * bar;
            const int x = (int) tickToX (t);
            if (x < rulerBounds.getX() || x > rulerBounds.getRight()) continue;
            g.setColour (theme.foreground.withAlpha (0.8f));
            g.drawText (juce::String (bi + 1), x + 3, rulerBounds.getY() + 2, 40, 16,
                        juce::Justification::centredLeft, false);
            g.setColour (theme.separator.withAlpha (0.45f));
            g.drawVerticalLine (x, (float) rulerBounds.getY() + 20.0f,
                                (float) rulerBounds.getBottom());
        }

        if (snap > 0)
        {
            const int64_t step = juce::jmax<int64_t> (1, snap);
            const int64_t first = (firstTick / step) * step;
            for (int64_t t = first; t <= lastTick + step; t += step)
            {
                const int x = (int) tickToX (t);
                if (x < rulerBounds.getX() || x > rulerBounds.getRight()) continue;
                if (t % bar != 0)
                {
                    g.setColour (theme.separator.withAlpha (0.2f));
                    g.drawVerticalLine (x, (float) rulerBounds.getBottom() - 8.0f,
                                        (float) rulerBounds.getBottom());
                }
            }
        }
    }

    void paintTrackRows (juce::Graphics& g)
    {
        const auto& theme = getTheme();
        g.setColour (theme.waveformBg);
        g.fillRect (clipGridBounds);

        const int numTracks = engine.getNumTracks();
        for (int ti = 0; ti < numTracks; ++ti)
        {
            const int y = trackTopY (ti);
            if (y + trackH < clipGridBounds.getY() || y > clipGridBounds.getBottom()) continue;

            const bool selected = (ti == selectedTrack);
            if (selected)
            {
                g.setColour (theme.accent.withAlpha (0.07f));
                g.fillRect (clipGridBounds.getX(), y, clipGridBounds.getWidth(), trackH - 1);
            }

            g.setColour (theme.separator.withAlpha (0.35f));
            g.fillRect (clipGridBounds.getX(), y + trackH - 1, clipGridBounds.getWidth(), 1);

            paintGridLines (g, y);
            paintClipsForTrack (g, ti, y);
        }
    }

    void paintGridLines (juce::Graphics& g, int y)
    {
        const auto& theme = getTheme();
        const int64_t snap = currentSnapTicks();
        if (snap <= 0) return;

        const int64_t firstTick = xToTick (clipGridBounds.getX());
        const int64_t lastTick  = xToTick (clipGridBounds.getRight());
        const int64_t first = (firstTick / snap) * snap;

        for (int64_t t = first; t <= lastTick + snap; t += snap)
        {
            const int x = (int) tickToX (t);
            if (x < clipGridBounds.getX() || x > clipGridBounds.getRight()) continue;
            const bool isBar = (t % (MidiClip::kPPQ * 4) == 0);
            g.setColour (theme.separator.withAlpha (isBar ? 0.45f : 0.18f));
            g.drawVerticalLine (x, (float) y, (float) (y + trackH - 1));
        }
    }

    void paintClipsForTrack (juce::Graphics& g, int trackIdx, int y)
    {
        const auto& theme = getTheme();
        const int numClips = engine.getNumClips (trackIdx);
        for (int ci = 0; ci < numClips; ++ci)
        {
            const auto r = clipRectForClip (trackIdx, ci);
            if (! r.intersects (clipGridBounds)) continue;

            const bool selected = isClipSelected (trackIdx, ci)
                               || (trackIdx == selectedTrack && ci == selectedClip);
            // ThemeData has no clipFill — theme.button is the nearest
            // existing "neutral flat surface" colour (used the same way for
            // the scrollbar track in styleScrollBar()). Flag for review if
            // a dedicated clip-tile colour was actually intended.
            const auto fill = selected ? theme.accent : theme.button;
            g.setColour (fill.withAlpha (selected ? 0.78f : 0.62f));
            g.fillRoundedRectangle (r.toFloat(), 3.0f);

            g.setColour (selected ? theme.accent.brighter (0.2f)
                                  : theme.separator.withAlpha (0.75f));
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 1.0f);

            const auto info = engine.getClipInfo (trackIdx, ci);
            g.setColour (selected ? juce::Colours::black.withAlpha (0.85f)
                                  : theme.foreground.withAlpha (0.78f));
            g.setFont (juce::Font (11.0f, juce::Font::bold));
            g.drawText ("CLIP " + juce::String (ci + 1), r.reduced (6, 2),
                        juce::Justification::centredLeft, true);

            if (selected)
            {
                const int handleW = juce::jmin (kResizeZone, r.getWidth());
                g.setColour (theme.foreground.withAlpha (0.35f));
                g.fillRect (r.getRight() - handleW, r.getY() + 2,
                            handleW, juce::jmax (0, r.getHeight() - 4));
            }
        }
    }

    void paintLoopOverlay (juce::Graphics& g)
    {
        if (loopStart < 0 || loopEnd <= loopStart) return;
        const auto& theme = getTheme();
        const int x1 = (int) tickToX (loopStart);
        const int x2 = (int) tickToX (loopEnd);
        const int l = juce::jmax (clipGridBounds.getX(), x1);
        const int r = juce::jmin (clipGridBounds.getRight(), x2);
        if (r <= l) return;

        g.setColour (theme.accent.withAlpha (0.06f));
        g.fillRect (l, rulerBounds.getY(), r - l, clipGridBounds.getBottom() - rulerBounds.getY());
        g.setColour (theme.accent.withAlpha (0.9f));
        g.fillRect (x1, rulerBounds.getY(), 2, clipGridBounds.getBottom() - rulerBounds.getY());
        g.fillRect (x2 - 1, rulerBounds.getY(), 2, clipGridBounds.getBottom() - rulerBounds.getY());
    }

    void paintPlayhead (juce::Graphics& g)
    {
        const int x = (int) tickToX (engine.getPlayheadTick());
        if (x < 0 || x > getWidth()) return;
        const auto& theme = getTheme();
        g.setColour (theme.accent.withAlpha (0.95f));
        g.fillRect (x, rulerBounds.getY(), 2, clipGridBounds.getBottom() - rulerBounds.getY());
    }

    void paintRubberBand (juce::Graphics& g)
    {
        if (rubberBandRect.isEmpty()) return;
        const auto& theme = getTheme();
        g.setColour (theme.accent.withAlpha (0.12f));
        g.fillRect (rubberBandRect);
        g.setColour (theme.accent.withAlpha (0.65f));
        g.drawRect (rubberBandRect, 1);
    }

    void paintDrawClipPreview (juce::Graphics& g)
    {
        if (dragMode != DragMode::DrawClip || dragTrack < 0) return;
        const auto& theme = getTheme();
        const int x = (int) tickToX (drawStartTick);
        const int w = juce::jmax (kMinClipPx, (int)(drawLenTicks * pixelsPerTick));
        const int y = trackTopY (dragTrack);
        juce::Rectangle<int> r (x, y, w, trackH - 1);
        g.setColour (theme.accent.withAlpha (0.25f));
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
        g.setColour (theme.accent.withAlpha (0.65f));
        g.drawRoundedRectangle (r.toFloat(), 3.0f, 1.0f);
    }

    void updateCursor (const juce::MouseEvent& e)
    {
        if (rulerBounds.contains (e.getPosition()))
        {
            setMouseCursor (juce::MouseCursor::CrosshairCursor);
            return;
        }

        if (! clipGridBounds.contains (e.getPosition()))
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
            return;
        }

        const int ti = trackFromY (e.y);
        if (juce::isPositiveAndBelow (ti, engine.getNumTracks()))
        {
            for (int ci = 0; ci < engine.getNumClips (ti); ++ci)
            {
                const auto r = clipRectForClip (ti, ci);
                if (r.contains (e.getPosition()))
                {
                    if (e.x >= r.getRight() - kResizeZone)
                    {
                        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
                        return;
                    }
                    break;
                }
            }
        }

        setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void updateHoverHandle (juce::Point<int> p)
    {
        hoverTrack = -1;
        hoverClip = -1;
        if (! clipGridBounds.contains (p)) return;

        const int ti = trackFromY (p.y);
        if (! juce::isPositiveAndBelow (ti, engine.getNumTracks())) return;

        for (int ci = 0; ci < engine.getNumClips (ti); ++ci)
        {
            const auto r = clipRectForClip (ti, ci);
            if (r.contains (p) && p.x >= r.getRight() - kResizeZone)
            {
                hoverTrack = ti;
                hoverClip = ci;
                return;
            }
        }
    }

    /** Restored — was called from the constructor but had no definition.
     *  Styles a scrollbar to match the LCD-frame look used throughout this
     *  view (see paint()'s theme.waveformBg/separator frame). */
    static void styleScrollBar (juce::ScrollBar& sb)
    {
        const auto& theme = getTheme();
        sb.setColour (juce::ScrollBar::backgroundColourId, theme.waveformBg);
        sb.setColour (juce::ScrollBar::thumbColourId,      theme.foreground.withAlpha (0.28f));
        sb.setColour (juce::ScrollBar::trackColourId,      theme.button.withAlpha (0.45f));
    }

    /** Restored — was called from mouseExit() but had no definition.
     *  Thin setter over hoverTrack/hoverClip; updateHoverHandle() sets those
     *  fields directly during mouseMove, but mouseExit needs to both clear
     *  them and force the hover-handle fade-out to repaint immediately. */
    void setHoverHandle (int trackIdx, int clipIdx)
    {
        if (trackIdx == hoverTrack && clipIdx == hoverClip) return;
        hoverTrack = trackIdx;
        hoverClip  = clipIdx;
        repaint();
    }

    int leftPanelW() const noexcept
    {
        return inspectorVisible ? kLeftW : kStripW;
    }

    void updateScrollRanges()
    {
        // SequencerEngine has no getArrangementLengthTicks() — getLengthTicks()
        // is documented as exactly that ("global length = end of last clip
        // across all tracks"), so it's the correct existing call here.
        const int viewW = juce::jmax (1, clipGridBounds.getWidth());
        const int totalW = juce::jmax (viewW, (int) juce::roundToInt (engine.getLengthTicks() * pixelsPerTick));
        const double maxScrollX = juce::jmax (0.0, (double) totalW - viewW);
        scrollX = juce::jlimit (0.0, maxScrollX, scrollX);

        const int viewH = juce::jmax (1, clipGridBounds.getHeight());
        const int totalH = juce::jmax (viewH, engine.getNumTracks() * trackH);
        scrollY = juce::jlimit (0, juce::jmax (0, totalH - viewH), scrollY);

        if (maxScrollX > 0.0)
        {
            hScroll.setCurrentRange (scrollX / (double) totalW,
                                     viewW / (double) totalW);
        }
        else
        {
            hScroll.setCurrentRange (0.0, 1.0);
        }

        if (totalH > viewH)
        {
            vScroll.setCurrentRange (scrollY / (double) totalH,
                                     viewH / (double) totalH);
        }
        else
        {
            vScroll.setCurrentRange (0.0, 1.0);
        }
    }

    void scrollBarMoved (juce::ScrollBar* bar, double newRangeStart) override
    {
        if (bar == &hScroll)
        {
            const int viewW = juce::jmax (1, clipGridBounds.getWidth());
            const int totalW = juce::jmax (viewW, (int) juce::roundToInt (engine.getLengthTicks() * pixelsPerTick));
            scrollX = juce::jlimit (0.0, juce::jmax (0.0, (double) totalW - viewW), newRangeStart * totalW);
        }
        else if (bar == &vScroll)
        {
            const int viewH = juce::jmax (1, clipGridBounds.getHeight());
            const int totalH = juce::jmax (viewH, engine.getNumTracks() * trackH);
            scrollY = juce::jlimit (0, juce::jmax (0, totalH - viewH),
                                    (int) juce::roundToInt (newRangeStart * totalH));
        }
        repaint();
    }
};
