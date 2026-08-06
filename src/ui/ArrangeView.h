#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "TransportBar.h"
#include "FloatingTransportBar.h"
#include "TrackHeaderStrip.h"
#include "TrackInspector.h"
#include "DysektLookAndFeel.h"
#include "../sequencer/SequencerEngine.h"
#include "../sequencer/MidiClip.h"
#include "ToolIcons.h"
#include <limits>

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

    static constexpr int kTransportH   = 32;
    static constexpr int kInspectorW   = 216;
    static constexpr int kStripW       = 196;
    static constexpr int kLeftW        = kInspectorW + kStripW;
    static constexpr int kRulerH       = 32;
    static constexpr int kScrollH      = 10;
    static constexpr int kScrollW      = 10;
    static constexpr int kOverviewH    = 54;   // "ARRANGEMENT OVERVIEW" caption + minimap strip
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
        addAndMakeVisible (transport);
        addAndMakeVisible (inspector);
        addAndMakeVisible (trackStrip);

        transport.onFloatRequested = [this] { showFloatingTransport(); };

        // ── Horizontal scrollbar ──────────────────────────────────────────────
        hScroll.setRangeLimits (0.0, 1.0);
        hScroll.setCurrentRange (0.0, 0.5);
        hScroll.setAutoHide (false);
        styleScrollBar (hScroll);
        hScroll.addListener (this);
        addAndMakeVisible (hScroll);

        // ── Vertical scrollbar ────────────────────────────────────────────────
        vScroll.setRangeLimits (0.0, 1.0);
        vScroll.setCurrentRange (0.0, 1.0);
        vScroll.setAutoHide (false);
        styleScrollBar (vScroll);
        vScroll.addListener (this);
        addAndMakeVisible (vScroll);

        // ── Track-strip callbacks ─────────────────────────────────────────────
        trackStrip.onTrackSelected = [this] (int idx)
        {
            selectTrack (idx);
            repaint();
        };
        trackStrip.onTrackMuted = [this] (int, bool) { repaint(); };

        setWantsKeyboardFocus (true);
        startTimerHz (30);
    }

    ~ArrangeView() override
    {
        hScroll.removeListener (this);
        vScroll.removeListener (this);
        stopTimer();
        if (floatingTransport != nullptr)
            floatingTransport->hide();
    }

    //==========================================================================
    void resized() override
    {
        auto r = getLocalBounds().reduced (3);
        transport.setBounds (r.removeFromTop (kTransportH));

        // Corner square between the two scrollbars
        auto cornerR = r;
        cornerR = cornerR.removeFromBottom (kScrollH).removeFromRight (kScrollW);

        auto hScrollR = r.removeFromBottom (kScrollH).withTrimmedRight (kScrollW);
        auto vScrollR = r.removeFromRight  (kScrollW);

        // Arrangement-overview minimap, docked above the horizontal scrollbar,
        // spanning the same width as the ruler/clip grid (left of it are the
        // inspector + track-header columns, which the mockup's overview
        // strip does not run under).
        auto overviewR = r.removeFromBottom (kOverviewH);

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

        overviewBounds = overviewR.withTrimmedLeft (kLeftW);
        auto overviewBar = overviewBounds;
        overviewBar.removeFromTop (16 + 6); // caption line + gap
        overviewBarBounds = overviewBar.reduced (0, 2);

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
        paintArrangementOverview (g);

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
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();

        // ── Arrangement-overview minimap ─────────────────────────────────────
        if (overviewBarBounds.contains (e.getPosition()))
        {
            const auto totalTicks = arrangementContentTicks();
            if (totalTicks > 0 && overviewBarBounds.getWidth() > 0)
            {
                const double ratio = juce::jlimit (0.0, 1.0,
                    (double) (e.x - overviewBarBounds.getX()) / (double) overviewBarBounds.getWidth());
                engine.seekToTick ((int64_t) (ratio * (double) totalTicks));
            }
            return;
        }

        // ── Ruler ─────────────────────────────────────────────────────────────
        if (rulerBounds.contains (e.getPosition()))
        {
            if (e.mods.isRightButtonDown())
            {
                loopStart = -1; loopEnd = -1;
                repaint(); return;
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
        if (! juce::isPositiveAndBelow (trackIdx, engine.getNumTracks())) return;

        // Hit test all clips on this track
        const int numClips = engine.getNumClips (trackIdx);
        int hitClip = -1;
        juce::Rectangle<int> hitRect;
        for (int ci = 0; ci < numClips; ++ci)
        {
            const auto r = clipRectForClip (trackIdx, ci);
            if (r.contains (e.getPosition())) { hitClip = ci; hitRect = r; break; }
        }
        const bool onClip = (hitClip >= 0);

        if (e.mods.isRightButtonDown())
        {
            showContextMenu (trackIdx, hitClip, e);
            return;
        }

        // Non-Select tools act on a single left-click instead of the
        // Select tool's move/resize/create-on-empty-space behaviour below.
        if (currentTool != Tool::Select)
        {
            switch (currentTool)
            {
                case Tool::Draw:
                    if (onClip) { selectTrack (trackIdx); selectedClip = hitClip; }
                    else        handleDrawClipDown (trackIdx, e);
                    break;
                case Tool::Erase: if (onClip) handleEraseClipDown (trackIdx, hitClip); break;
                case Tool::Split: if (onClip) handleSplitClipDown (trackIdx, hitClip, e); break;
                case Tool::Glue:  if (onClip) handleGlueClipDown  (trackIdx, hitClip); break;
                default: break;
            }
            repaint(); trackStrip.repaint();
            return;
        }

        // Resize handle — right edge of a clip
        if (onClip && e.x >= hitRect.getRight() - kResizeZone)
        {
            dragMode       = DragMode::ResizeRight;
            dragTrack      = trackIdx;
            dragClip       = hitClip;
            dragStartX     = e.x;
            dragStartTicks = engine.getClipInfo (trackIdx, hitClip).lengthTicks;
            dragResizeLen  = dragStartTicks;
            selectTrack (trackIdx);
            selectedClip   = hitClip;
            updateCursor (e);
            repaint(); return;
        }

        // Clip body — move
        if (onClip)
        {
            dragMode       = DragMode::MoveClip;
            dragTrack      = trackIdx;
            dragClip       = hitClip;
            dragStartX     = e.x;
            dragStartTicks = engine.getClipInfo (trackIdx, hitClip).startTick;
            dragLiveOffset = dragStartTicks;
            selectTrack (trackIdx);
            selectedClip   = hitClip;
            updateCursor (e);
            repaint(); return;
        }

        // Empty track space — create new clip at click position
        {
            const int64_t clickTick = snapTick (xToTick (e.x));
            const int64_t defLen    = MidiClip::kPPQ * 4 * 4;  // 4 bars default
            const int newIdx = engine.addClip (trackIdx, clickTick, defLen);
            selectTrack (trackIdx);
            selectedClip  = newIdx;
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

        if (dragMode == DragMode::None) return;

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
        // Commit clip resize
        if (dragMode == DragMode::ResizeRight && dragTrack >= 0)
        {
            engine.setClipLengthTicks (dragTrack, dragClip, dragResizeLen);
            dragResizeLen = 0;
        }

        // Commit clip move
        if (dragMode == DragMode::MoveClip && dragTrack >= 0)
        {
            if (dragLiveOffset != engine.getClipInfo (dragTrack, dragClip).startTick)
            {
                // Shift all notes within the clip are clip-relative, so just move the slot
                engine.setClipStartTick (dragTrack, dragClip, dragLiveOffset);
                // Re-find clip index after sort (sortClips may have changed order)
                // Use dragLiveOffset to locate it
                for (int ci = 0; ci < engine.getNumClips (dragTrack); ++ci)
                    if (engine.getClipInfo (dragTrack, ci).startTick == dragLiveOffset)
                        { selectedClip = ci; break; }
            }
            dragLiveOffset = 0;
        }

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
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
        {
            // Zoom around mouse position
            const double tickAtMouse = xToTick (e.x);
            const double factor      = w.deltaY > 0 ? 1.18 : (1.0 / 1.18);
            pixelsPerTick = juce::jlimit (0.003, 6.0, pixelsPerTick * factor);
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
    /** Lazily creates (on first use) and shows the floating transport panel,
     *  hiding the docked TransportBar while it's up. Wired to
     *  transport.onFloatRequested. */
    void showFloatingTransport()
    {
        if (floatingTransport == nullptr)
        {
            floatingTransport = std::make_unique<FloatingTransportBar> (engine, linkPtr);
            floatingTransport->onDockRequested = [this] { dockTransport(); };
        }

        transport.setVisible (false);
        floatingTransport->show();
    }

    /** Hides the floating panel and restores the docked transport bar.
     *  Wired to FloatingTransportBar::onDockRequested. */
    void dockTransport()
    {
        if (floatingTransport != nullptr)
            floatingTransport->hide();

        transport.setVisible (true);
    }

    /** Lets an owner (e.g. SlotWindowContent) dock extra controls — such as the
     *  Mixer / Arranger view switcher — into the far left of the transport row. */
    TransportBar& getTransportBar() noexcept { return transport; }

private:
    //==========================================================================
    //  State
    //==========================================================================
    SequencerEngine&      engine;
    AbletonLink*          linkPtr = nullptr;
    TransportBar          transport;
    std::unique_ptr<FloatingTransportBar> floatingTransport;
    TrackInspector        inspector;
    TrackHeaderStrip      trackStrip;
    juce::ScrollBar       hScroll { false };
    juce::ScrollBar       vScroll { true  };

    juce::Rectangle<int>  gridArea, rulerBounds, clipGridBounds;
    juce::Rectangle<int>  overviewBounds, overviewBarBounds;

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
    enum class DragMode  { None, MoveClip, ResizeRight };
    enum class RulerDrag { None, Scrub, LoopSet };
    DragMode  dragMode       = DragMode::None;
    RulerDrag rulerDrag      = RulerDrag::None;

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

    int       selectedClip   = 0;   // which clip is selected on selectedTrack

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

        // Live overrides during drag
        if (dragMode == DragMode::MoveClip && dragTrack == trackIdx && dragClip == clipIdx)
            startTick = dragLiveOffset;
        if (dragMode == DragMode::ResizeRight && dragTrack == trackIdx && dragClip == clipIdx)
            lengthTicks = dragResizeLen;

        const int w = juce::jmax (kMinClipPx, (int)(lengthTicks * pixelsPerTick));
        const int x = clipGridBounds.getX() + (int)(startTick * pixelsPerTick - scrollX);
        const int y = trackTopY (trackIdx);
        return { x, y, w, trackH - 1 };
    }

    int64_t snapTick (int64_t t) const noexcept
    {
        const int64_t snap = MidiClip::kPPQ;  // quarter-note snap
        return ((t + snap / 2) / snap) * snap;
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

    /** Actual extent of the arrangement's content (unlike totalVisibleTicks(),
        which pads out to a minimum scrollable range) — used by the overview
        minimap so clips fill the whole strip rather than being squeezed into
        a sliver at the left. */
    int64_t arrangementContentTicks() const noexcept
    {
        int64_t maxEnd = MidiClip::kPPQ * 4; // at least one bar
        for (int ti = 0; ti < engine.getNumTracks(); ++ti)
            for (int ci = 0; ci < engine.getNumClips (ti); ++ci)
                maxEnd = juce::jmax (maxEnd, engine.getClipInfo (ti, ci).endTick());
        return maxEnd;
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
    }

    void scrollBarMoved (juce::ScrollBar* sb, double newRangeStart) override
    {
        if (sb == &hScroll)
            scrollX = newRangeStart;
        else
            scrollY = (int) newRangeStart;
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
    static juce::MouseCursor toolCursorFor (Tool t)
    {
        switch (t)
        {
            case Tool::Erase: return juce::MouseCursor::CrosshairCursor;
            case Tool::Split: return juce::MouseCursor::CrosshairCursor;
            case Tool::Glue:  return juce::MouseCursor::CrosshairCursor;
            case Tool::Draw:  return juce::MouseCursor::CrosshairCursor;
            case Tool::Select:
            default:          return juce::MouseCursor::NormalCursor;
        }
    }

    void updateCursor (const juce::MouseEvent& e)
    {
        if (dragMode == DragMode::ResizeRight)
        { setMouseCursor (juce::MouseCursor::LeftRightResizeCursor); return; }
        if (dragMode == DragMode::MoveClip)
        { setMouseCursor (juce::MouseCursor::DraggingHandCursor); return; }

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

    //==========================================================================
    //  Context menus
    //==========================================================================
    void showContextMenu (int trackIdx, int clipIdx, const juce::MouseEvent& e)
    {
        const auto info  = engine.getTrackInfo (trackIdx);
        const bool onClip = (clipIdx >= 0);
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
    void handleDrawClipDown (int trackIdx, const juce::MouseEvent& e)
    {
        const int64_t clickTick = snapTick (xToTick (e.x));
        const int64_t defLen    = MidiClip::kPPQ * 4 * 4;  // 4 bars default
        const int newIdx = engine.addClip (trackIdx, clickTick, defLen);
        selectTrack (trackIdx);
        selectedClip = newIdx;
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
        const auto& theme = getTheme();

        // Background
        g.setColour (theme.header);
        g.fillRect (rulerBounds);

        // Bottom border
        g.setColour (theme.separator);
        g.fillRect (rulerBounds.getX(), rulerBounds.getBottom() - 1,
                    rulerBounds.getWidth(), 1);

        // Loop region shading inside ruler
        if (loopStart >= 0 && loopEnd > loopStart)
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

        // Beat ticks
        if (showBeats)
        {
            const int64_t firstBeat = (int64_t)(scrollX / (pixelsPerTick * ppq));
            const int64_t lastBeat  = firstBeat + (int64_t)(gw / (pixelsPerTick * ppq)) + 2;
            for (int64_t b = firstBeat; b <= lastBeat && b * ppq <= total; ++b)
            {
                const int x = gx + (int)((b * ppq) * pixelsPerTick - scrollX);
                if (x < gx || x > gx + gw) continue;
                const bool isBar = (b % 4 == 0);
                g.setColour (isBar ? theme.separator : theme.gridLine);
                g.fillRect (x, rulerBounds.getY(),
                            1, isBar ? rulerBounds.getHeight()
                                     : rulerBounds.getHeight() / 2);
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
            g.drawText ("L", (int)lx + 2, rulerBounds.getY(),
                        14, rulerBounds.getHeight(), juce::Justification::centredLeft);
            g.drawText ("R", (int)rx - 16, rulerBounds.getY(),
                        14, rulerBounds.getHeight(), juce::Justification::centredRight);
            // Bracket lines
            g.drawVerticalLine ((int)lx, (float)rulerBounds.getY(), (float)rulerBounds.getBottom());
            g.drawVerticalLine ((int)rx, (float)rulerBounds.getY(), (float)rulerBounds.getBottom());
        }
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
    }

    void paintLoopOverlay (juce::Graphics& g) const
    {
        if (loopStart < 0 || loopEnd <= loopStart) return;
        const float lx = tickToX (loopStart);
        const float rx = tickToX (loopEnd);
        // Tinted band across all track rows
        g.setColour (getTheme().accent.withAlpha (0.06f));
        g.fillRect (lx, (float)clipGridBounds.getY(),
                    rx - lx, (float)clipGridBounds.getHeight());
        // Side lines
        g.setColour (getTheme().accent.withAlpha (0.45f));
        g.drawVerticalLine ((int)lx,
                            (float)clipGridBounds.getY(),
                            (float)clipGridBounds.getBottom());
        g.drawVerticalLine ((int)rx,
                            (float)clipGridBounds.getY(),
                            (float)clipGridBounds.getBottom());
    }

    void paintOneTrack (juce::Graphics& g, int i) const
    {
        const auto info  = engine.getTrackInfo (i);
        const bool isSel = (i == selectedTrack);
        const bool muted = ! info.enabled;

        const juce::Rectangle<int> rowR (
            clipGridBounds.getX(),
            trackTopY (i),
            clipGridBounds.getWidth(),
            trackH);

        // Clip to grid area
        g.saveState();
        g.reduceClipRegion (clipGridBounds);

        // Row background — a quiet layered surface keeps empty timeline space readable.
        const auto& theme = getTheme();
        g.setColour (isSel ? theme.accent.withAlpha (0.10f)
                           : (i % 2 == 0 ? theme.waveformBg.brighter (0.012f)
                                         : theme.waveformBg));
        g.fillRect (rowR);

        // Separator
        g.setColour (theme.separator);
        g.fillRect (rowR.getX(), rowR.getBottom() - 1, rowR.getWidth(), 1);

        // Vertical grid lines
        paintGridLines (g, rowR);

        // Clips — paint all slots on this track
        const int numClips = engine.getNumClips (i);
        for (int ci = 0; ci < numClips; ++ci)
        {
            const bool isSelClip = isSel && (ci == selectedClip);
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

        const int64_t firstBeat = (int64_t)(scrollX / (pixelsPerTick * ppq));
        const int64_t lastBeat  = firstBeat + (int64_t)(rowR.getWidth() / (pixelsPerTick * ppq)) + 2;

        for (int64_t b = firstBeat; b <= lastBeat && b * ppq <= total; ++b)
        {
            const int x = clipGridBounds.getX() + (int)((b * ppq) * pixelsPerTick - scrollX);
            if (x < rowR.getX() || x > rowR.getRight()) continue;
            const bool isBar = (b % 4 == 0);
            if (!showBeats && !isBar) continue;
            const auto& theme = getTheme();
            g.setColour (isBar ? theme.separator.withAlpha (0.82f) : theme.gridLine.withAlpha (0.48f));
            g.fillRect (x, rowR.getY(), 1, rowR.getHeight() - 1);
        }
    }

    void paintClip (juce::Graphics& g, int i, int ci,
                    const SequencerTrackInfo& info,
                    bool isSel, bool muted) const
    {
        const auto clipR = clipRectForClip (i, ci);
        if (! clipGridBounds.intersects (clipR)) return;

        const juce::Colour base = muted
            ? info.colour.withSaturation (0.08f).withBrightness (0.22f)
            : info.colour;

        // Flat fill — no gradient, square corners
        g.setColour (base.withAlpha (muted ? 0.16f : 0.42f));
        g.fillRoundedRectangle (clipR.toFloat().reduced (1.f, 1.f), 0.0f);

        // Border — brighter when selected
        if (isSel)
        {
            g.setColour (base.brighter (0.7f).withAlpha (0.95f));
            g.drawRoundedRectangle (clipR.toFloat().reduced (1.f, 1.f), 0.0f, 1.5f);
        }
        else
        {
            g.setColour (base.withAlpha (muted ? 0.28f : 0.6f));
            g.drawRoundedRectangle (clipR.toFloat().reduced (1.f, 1.f), 0.0f, 1.f);
        }

        // Mute hatch
        if (muted)
        {
            g.setColour (juce::Colours::black.withAlpha (0.38f));
            g.fillRoundedRectangle (clipR.toFloat().reduced (1.f, 1.f), 0.0f);
        }

        // Left accent bar (flat, no rounding — replaces the old top strip)
        const float accentW = 3.f;
        g.setColour (base.withAlpha (muted ? 0.24f : 0.90f));
        g.fillRect (clipR.getX() + 1.f, clipR.getY() + 1.f,
                    accentW, (float)clipR.getHeight() - 2.f);

        // Track name
        if (trackH >= 20)
        {
            g.setFont (juce::Font (juce::jmin (11.f, (float)trackH * 0.22f), juce::Font::bold));
            g.setColour (muted ? getTheme().foreground.withAlpha (0.35f)
                               : juce::Colours::white.withAlpha (0.88f));
            g.drawText (info.name,
                        clipR.getX() + 5 + (int)accentW, clipR.getY() + 2,
                        juce::jmax (0, clipR.getWidth() - 24 - (int)accentW),
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
            g.setColour (base.withAlpha (0.55f));
            g.drawText (badge,
                        clipR.getRight() - 22, clipR.getY() + 2,
                        20, 12,
                        juce::Justification::centredRight, false);
        }

        // Resize handle
        {
            const juce::Rectangle<float> handleR (
                clipR.toFloat().reduced (1.f, 1.f)
                    .withLeft ((float)(clipR.getRight() - 8)));
            g.setColour (base.withAlpha (0.4f));
            g.fillRoundedRectangle(handleR, 0.0f);
            g.setColour (base.brighter (0.5f).withAlpha (0.45f));
            const float cx = handleR.getCentreX();
            for (int dot = 0; dot < 3; ++dot)
            {
                const float dy = handleR.getY() + handleR.getHeight() * (0.25f + dot * 0.25f);
                g.fillEllipse (cx - 1.f, dy - 1.f, 2.f, 2.f);
            }
        }

        // Mini note preview
        paintNotePreview (g, i, ci, clipR, base, muted);
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

        const auto playheadColour = getTheme().accent.brighter (0.38f).withAlpha (0.96f);

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

    //==========================================================================
    //  Arrangement-overview minimap — a single flattened strip summarising
    //  every track's clips across the whole session, with its own playhead
    //  tick and click-to-seek (see mouseDown()).
    //==========================================================================
    void paintArrangementOverview (juce::Graphics& g) const
    {
        if (overviewBounds.getWidth() < 10 || overviewBounds.getHeight() < 10)
            return;

        const auto& theme = getTheme();

        auto caption = overviewBounds.withHeight (16);
        g.setColour (theme.foreground.withAlpha (0.48f));
        g.setFont (DysektLookAndFeel::makeFont (9.0f, true));
        g.drawText ("ARRANGEMENT OVERVIEW", caption, juce::Justification::centredLeft, false);

        if (overviewBarBounds.getWidth() < 4 || overviewBarBounds.getHeight() < 4)
            return;

        const auto barR = overviewBarBounds.toFloat();
        g.setColour (theme.waveformBg.darker (0.15f));
        g.fillRoundedRectangle (barR, 3.0f);

        const auto totalTicks = arrangementContentTicks();
        if (totalTicks > 0)
        {
            g.saveState();
            g.reduceClipRegion (overviewBarBounds);

            const float pxPerTick = (float) overviewBarBounds.getWidth() / (float) totalTicks;

            // Flatten every track's clips into one row. Later tracks are
            // painted first so earlier (higher, more prominent) tracks in
            // the list win where clips overlap in time — matching the
            // vertical stacking order of the main grid above.
            for (int ti = engine.getNumTracks() - 1; ti >= 0; --ti)
            {
                const auto info = engine.getTrackInfo (ti);
                for (int ci = 0; ci < engine.getNumClips (ti); ++ci)
                {
                    const auto clip = engine.getClipInfo (ti, ci);
                    const int x = overviewBarBounds.getX() + (int) ((float) clip.startTick * pxPerTick);
                    const int w = juce::jmax (1, (int) ((float) clip.lengthTicks * pxPerTick));
                    g.setColour (info.colour);
                    g.fillRect (x, overviewBarBounds.getY(), w, overviewBarBounds.getHeight());
                }
            }

            g.restoreState();

            const int64_t playTick = engine.getPlayheadTick();
            if (playTick >= 0 && playTick <= totalTicks)
            {
                const float px = overviewBarBounds.getX() + (float) playTick * pxPerTick;
                g.setColour (getTheme().accent.brighter (0.38f).withAlpha (0.96f));
                g.fillRect (px - 1.0f, (float) overviewBarBounds.getY(),
                            2.0f, (float) overviewBarBounds.getHeight());
            }
        }

        g.setColour (theme.separator);
        g.drawRoundedRectangle (barR.reduced (0.5f), 3.0f, 1.0f);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ArrangeView)
};
