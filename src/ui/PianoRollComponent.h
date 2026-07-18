#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/SequencerEngine.h"
#include "DysektLookAndFeel.h"
#include "VelocityLane.h"

//==============================================================================
//  PianoRollComponent  —  Cubase-style MIDI editor
//
//  Tools:
//   Select (S)  — click selects, rubber-band multi-select, drag-move, resize
//   Draw   (D)  — click/drag creates notes
//   Erase  (E)  — click removes notes
//   Split  (K)  — click splits a note at cursor
//   Glue   (G)  — click merges adjacent notes on same pitch
//
//  Selection:
//   Shift+click  — add/remove note from selection
//   Ctrl+A       — select all
//   Delete/Bksp  — delete selected
//   Ctrl+C/V     — copy/paste
//   Ctrl+D       — duplicate (paste immediately after selection)
//   Ctrl+Z/Y     — undo/redo (local snapshot per edit action)
//
//  Right-click context menu:
//   Delete, Select All, Quantise, Set Velocity…, Note Properties…
//
//  Zoom:
//   Ctrl+scroll  — horizontal zoom (around mouse)
//   Shift+scroll — horizontal scroll
//   Scroll       — vertical scroll
//   Ctrl+0       — zoom to fit
//   +/- keys     — row height (vertical zoom)
//
//  Loop region:
//   Drag ruler (left)  → loop start
//   Drag ruler (right) → loop end
//   Right-click ruler  → clear loop
//==============================================================================
class PianoRollComponent : public juce::Component,
                           private juce::Timer,
                           private juce::ScrollBar::Listener
{
public:
    //==========================================================================
    //  Constants
    //==========================================================================
    static constexpr int kKeysW     = 68;  // wide enough for readable octave labels
    static constexpr int kRulerH    = 28;
    static constexpr int kVelocityH = 82;  // a real editable lane, not a footer
    static constexpr int kScrollH   = 10;
    static constexpr int kToolbarH  = 34;
    static constexpr int kNumNotes  = 128;
    static constexpr int kResizeZone = 7;   // px from note right edge = resize handle

    //==========================================================================
    //  Edit tool
    //==========================================================================
    enum class Tool { Select, Draw, Erase, Split, Glue };

    //==========================================================================
    explicit PianoRollComponent (SequencerEngine& seq)
        : engine (seq)
    {
        velocityLane.setClip (engine.getClip (0, 0));
        addAndMakeVisible (velocityLane);

        hScroll.setRangeLimits (0.0, 1.0);
        hScroll.setCurrentRange (0.0, 0.3);
        hScroll.setAutoHide (false);
        hScroll.setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xFF263342));
        hScroll.addListener (this);
        addAndMakeVisible (hScroll);

        vScroll.setRangeLimits (0.0, 1.0);
        vScroll.setCurrentRange (0.3, 0.7);
        vScroll.setAutoHide (false);
        vScroll.setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xFF263342));
        vScroll.addListener (this);
        addAndMakeVisible (vScroll);

        buildToolbar();
        setWantsKeyboardFocus (true);
        startTimerHz (30);
    }

    ~PianoRollComponent() override
    {
        hScroll.removeListener (this);
        vScroll.removeListener (this);
        stopTimer();
    }

    //==========================================================================
    void setActiveTrack (int trackIndex, int clipIndex = 0)
    {
        activeTrack = trackIndex;
        activeClip  = clipIndex;
        velocityLane.setClip (engine.getClip (trackIndex, clipIndex));
        velocityLane.setSelectedNote (-1);
        selectedNotes.clear();
        undoStack.clear();
        redoStack.clear();
        repaint();
        velocityLane.repaint();
    }

    void setSnapTicks  (int64_t ticks) { snapTicks = ticks; }
    void setActiveTool (Tool t)
    {
        currentTool = t;
        updateToolbarButtons();
        // Immediate feedback on tool switch (menu pick, toolbar click, shortcut
        // key) rather than waiting for the next mouseMove; Select-tool's
        // resize/drag-hand hover refinements still apply on the next mouseMove.
        setMouseCursor (toolCursorFor (t));
        repaint();
    }

    //==========================================================================
    void resized() override
    {
        auto r = getLocalBounds();
        auto vScrollR  = r.removeFromRight (kScrollH);
        auto hScrollR  = r.removeFromBottom (kScrollH);
        auto velR      = r.removeFromBottom (kVelocityH);
        toolbarBounds  = r.removeFromTop (kToolbarH);
        rulerBounds    = r.removeFromTop (kRulerH);
        keysBounds     = r.removeFromLeft (kKeysW);
        gridBounds     = r;

        layoutToolbar();

        velocityLane.setBounds (velR.withTrimmedLeft (kKeysW));
        hScroll.setBounds      (hScrollR.withTrimmedLeft (kKeysW));
        vScroll.setBounds      (vScrollR.withTrimmedTop (kToolbarH + kRulerH));

        updateScrollRanges();
    }

    //==========================================================================
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFF060608));
        drawToolbar  (g);
        drawRuler    (g);
        drawKeyboard (g);
        drawGrid     (g);
        drawLoopRegion (g);
        drawNotes    (g);
        drawPlayhead (g);
        drawRubberBand (g);
    }

    //==========================================================================
    //  Mouse interaction
    //==========================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();

        // ── Ruler interaction (loop points + playhead seek) ──────────────────
        if (rulerBounds.contains (e.getPosition()) && e.x >= kKeysW)
        {
            if (e.mods.isRightButtonDown())
            {
                loopStartTick = -1; loopEndTick = -1;
                repaint(); return;
            }
            const int64_t tick = xToTick (e.x);
            if (e.mods.isAltDown())
            {
                // Alt+drag ruler = set loop region
                loopDragMode = (e.x < tickToX (loopStartTick) + 6 && loopStartTick >= 0)
                               ? 1 : 2;   // 1=start, 2=end
                loopStartTick = tick;
                loopEndTick   = tick;
            }
            else
            {
                engine.seekToTick (tick);
            }
            repaint(); return;
        }

        // ── Grid interaction ─────────────────────────────────────────────────
        if (! gridBounds.contains (e.getPosition())) return;

        MidiClip* clip = engine.getClip (activeTrack, activeClip);
        if (clip == nullptr) return;

        const int64_t tick    = xToTick (e.x);
        const int     noteNum = yToNote (e.y);

        if (e.mods.isRightButtonDown())
        {
            mouseRightClick (e);
            repaint(); velocityLane.repaint();
            return;
        }

        switch (currentTool)
        {
            case Tool::Draw:   handleDrawDown   (e, clip, tick, noteNum); break;
            case Tool::Erase:  handleEraseDown  (e, clip, tick, noteNum); break;
            case Tool::Split:  handleSplitDown  (e, clip, tick, noteNum); break;
            case Tool::Glue:   handleGlueDown   (e, clip, tick, noteNum); break;
            case Tool::Select: handleSelectDown (e, clip, tick, noteNum); break;
        }

        repaint(); velocityLane.repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        // Loop drag on ruler
        if (loopDragMode != 0 && rulerBounds.contains (e.getPosition()))
        {
            const int64_t tick = xToTick (e.x);
            if (loopDragMode == 1) loopStartTick = tick;
            else                   loopEndTick   = tick;
            if (loopStartTick > loopEndTick) std::swap (loopStartTick, loopEndTick);
            repaint(); return;
        }

        MidiClip* clip = engine.getClip (activeTrack, activeClip);
        if (clip == nullptr) return;

        const int64_t tick    = xToTick (e.x);
        const int     noteNum = yToNote (e.y);

        switch (dragMode)
        {
            case DragMode::Draw:
            {
                const int64_t newEnd = snap (juce::jmax (tick, dragStartTick + 1));
                clip->resizeNote (dragNoteIdx, juce::jmax ((int64_t)1, newEnd - dragStartTick));
                break;
            }
            case DragMode::Move:
            {
                int64_t newStart = snap (juce::jmax ((int64_t)0, dragOrigStart + (tick - dragStartTick)));
                int     newNote  = juce::jlimit (0, 127, dragOrigNote + (noteNum - dragStartNote));

                // Move all selected notes together
                const int64_t deltaTick = newStart - dragOrigStart;
                const int     deltaPitch = newNote  - dragOrigNote;

                for (int i = 0; i < selectedNotes.size(); ++i)
                {
                    int idx = selectedNotes[i];
                    if (idx == dragNoteIdx) continue;
                    const juce::ScopedReadLock sl (clip->getLock());
                    if (! juce::isPositiveAndBelow (idx, clip->getNotes().size())) continue;
                    const auto& n = clip->getNotes().getReference (idx);
                    clip->moveNote (idx,
                        juce::jmax ((int64_t)0, n.startTick + deltaTick - dragGroupDeltaTick),
                        juce::jlimit (0, 127, n.note + deltaPitch - dragGroupDeltaPitch));
                }
                clip->moveNote (dragNoteIdx, newStart, newNote);
                dragGroupDeltaTick  = deltaTick;
                dragGroupDeltaPitch = deltaPitch;
                break;
            }
            case DragMode::Resize:
            {
                const int64_t newDur = snap (juce::jmax ((int64_t)1, dragOrigDuration + (tick - dragStartTick)));
                // Resize all selected notes proportionally
                for (int i = 0; i < selectedNotes.size(); ++i)
                {
                    int idx = selectedNotes[i];
                    if (idx == dragNoteIdx) continue;
                    const juce::ScopedReadLock sl (clip->getLock());
                    if (! juce::isPositiveAndBelow (idx, clip->getNotes().size())) continue;
                    clip->resizeNote (idx, juce::jmax ((int64_t)1,
                        clip->getNotes().getReference(idx).durationTick + (newDur - dragOrigDuration)));
                }
                clip->resizeNote (dragNoteIdx, newDur);
                break;
            }
            case DragMode::RubberBand:
            {
                rubberBandEnd = e.getPosition();
                updateRubberBandSelection (clip);
                break;
            }
            default: break;
        }

        repaint(); velocityLane.repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        loopDragMode = 0;

        MidiClip* clip = engine.getClip (activeTrack, activeClip);

        // NOTE: no pushUndo() here. The undo checkpoint for this whole drag
        // gesture was already captured *before* the edit began, in
        // handleDrawDown()/handleSelectDown(). Pushing another snapshot here
        // (of the *post*-drag state) meant Ctrl+Z had to be pressed twice to
        // undo a single drag: once to "undo" to the already-current
        // post-drag state (a no-op), and again to reach the real pre-drag
        // state.
        if (dragMode == DragMode::Move && clip)
        {
            // moveNote() intentionally leaves the note array unsorted during
            // the drag (see MidiClip::moveNote) so that indices held here
            // (selectedNotes, dragNoteIdx) stay valid for the whole gesture.
            // Now that the gesture is over and those indices are about to be
            // discarded, restore sorted order once.
            clip->resortNotes();
        }

        dragMode    = DragMode::None;
        dragNoteIdx = -1;
        dragGroupDeltaTick = 0;
        dragGroupDeltaPitch = 0;
        rubberBandStart = rubberBandEnd = {};
        repaint();
        juce::ignoreUnused (e);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        // Double-click a note → open properties
        if (! gridBounds.contains (e.getPosition())) return;
        MidiClip* clip = engine.getClip (activeTrack, activeClip);
        if (! clip) return;
        const int64_t tick    = xToTick (e.x);
        const int     noteNum = yToNote (e.y);
        const int idx = hitTestAny (clip, tick, noteNum);
        if (idx >= 0) showNotePropertiesDialog (clip, idx);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
        {
            const double zf = w.deltaY > 0 ? 1.15 : (1.0 / 1.15);
            const double tickAtMouse = xToTick (e.x);
            pixelsPerTick = juce::jlimit (0.03, 12.0, pixelsPerTick * zf);
            scrollX = juce::jmax (0.0, tickAtMouse * pixelsPerTick - (e.x - kKeysW));
        }
        else if (e.mods.isShiftDown())
        {
            scrollX = juce::jmax (0.0, scrollX - w.deltaY * 80.0);
        }
        else
        {
            noteRowOffset = juce::jlimit (0, kNumNotes - visibleRows(),
                                         noteRowOffset + (w.deltaY < 0 ? 3 : -3));
        }
        syncScroll(); updateScrollRanges(); repaint();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        updateCursorForPosition (e);
    }

    //==========================================================================
    //  Keyboard shortcuts
    //==========================================================================
    bool keyPressed (const juce::KeyPress& k) override
    {
        MidiClip* clip = engine.getClip (activeTrack, activeClip);

        // Tool shortcuts
        if (k.getKeyCode() == 'S') { setActiveTool (Tool::Select); return true; }
        if (k.getKeyCode() == 'D') { setActiveTool (Tool::Draw);   return true; }
        if (k.getKeyCode() == 'E') { setActiveTool (Tool::Erase);  return true; }
        if (k.getKeyCode() == 'K') { setActiveTool (Tool::Split);  return true; }
        if (k.getKeyCode() == 'G') { setActiveTool (Tool::Glue);   return true; }

        // Delete
        if (k.getKeyCode() == juce::KeyPress::deleteKey ||
            k.getKeyCode() == juce::KeyPress::backspaceKey)
        {
            if (clip) deleteSelected (clip);
            return true;
        }

        // Ctrl+A — select all
        if (k.isKeyCode ('A') && k.getModifiers().isCommandDown())
        {
            if (clip) selectAll (clip);
            return true;
        }

        // Ctrl+C — copy
        if (k.isKeyCode ('C') && k.getModifiers().isCommandDown())
        {
            if (clip) copySelected (clip);
            return true;
        }

        // Ctrl+V — paste
        if (k.isKeyCode ('V') && k.getModifiers().isCommandDown())
        {
            if (clip) { pushUndo (clip); pasteClipboard (clip); }
            return true;
        }

        // Ctrl+D — duplicate
        if (k.isKeyCode ('D') && k.getModifiers().isCommandDown())
        {
            if (clip) duplicateSelected (clip);
            return true;
        }

        // Ctrl+Z — undo
        if (k.isKeyCode ('Z') && k.getModifiers().isCommandDown())
        {
            if (clip) undo (clip);
            return true;
        }

        // Ctrl+Y / Ctrl+Shift+Z — redo
        if ((k.isKeyCode ('Y') && k.getModifiers().isCommandDown()) ||
            (k.isKeyCode ('Z') && k.getModifiers().isCommandDown() && k.getModifiers().isShiftDown()))
        {
            if (clip) redo (clip);
            return true;
        }

        // Ctrl+Q — quantise
        if (k.isKeyCode ('Q') && k.getModifiers().isCommandDown())
        {
            if (clip) quantiseSelected (clip);
            return true;
        }

        // Ctrl+0 — zoom to fit
        if (k.isKeyCode ('0') && k.getModifiers().isCommandDown())
        {
            zoomToFit (clip);
            return true;
        }

        // + / - — vertical zoom (note row height)
        if (k.getKeyCode() == '+' || k.getKeyCode() == '=')
        {
            noteRowH = juce::jlimit (4, 24, noteRowH + 1);
            repaint(); return true;
        }
        if (k.getKeyCode() == '-')
        {
            noteRowH = juce::jlimit (4, 24, noteRowH - 1);
            repaint(); return true;
        }

        return false;
    }

    //==========================================================================
    //  Context menu
    //==========================================================================
    void mouseRightClick (const juce::MouseEvent& e)
    {
        MidiClip* clip = engine.getClip (activeTrack, activeClip);
        if (! clip) return;

        const int64_t tick    = xToTick (e.x);
        const int     noteNum = yToNote (e.y);
        const int     noteIdx = gridBounds.contains (e.getPosition())
                              ? hitTestAny (clip, tick, noteNum) : -1;

        juce::PopupMenu m;

        juce::PopupMenu toolMenu;
        const auto toolIconColour = findColour (juce::TextButton::textColourOffId);
        auto addToolItem = [&] (int itemId, const juce::String& text, Tool tool)
        {
            juce::PopupMenu::Item item;
            item.itemID  = itemId;
            item.text    = text;
            item.isTicked = (currentTool == tool);
            item.setImage (makeToolMenuIcon (tool, toolIconColour));
            toolMenu.addItem (item);
        };
        addToolItem (20, "Select (S)", Tool::Select);
        addToolItem (21, "Draw (D)",   Tool::Draw);
        addToolItem (22, "Erase (E)",  Tool::Erase);
        addToolItem (23, "Split (K)",  Tool::Split);
        addToolItem (24, "Glue (G)",   Tool::Glue);
        m.addSubMenu ("Tool", toolMenu);
        m.addSeparator();

        if (noteIdx >= 0)
        {
            m.addItem (1, "Delete note");
            m.addItem (2, "Split here");
            m.addSeparator();
            m.addItem (3, "Note properties...");
        }
        else
        {
            m.addItem (4,  "Select all",           ! clip->getNotes().isEmpty());
            m.addItem (5,  "Deselect all",         ! selectedNotes.isEmpty());
            m.addSeparator();
            m.addItem (6,  "Quantise selection",   ! selectedNotes.isEmpty());
            m.addItem (7,  "Set velocity...",      ! selectedNotes.isEmpty());
            m.addSeparator();
            m.addItem (8,  "Delete selected",      ! selectedNotes.isEmpty());
            m.addSeparator();
            m.addItem (9,  "Zoom to fit");
        }

        m.showMenuAsync (juce::PopupMenu::Options()
                             .withTargetScreenArea (juce::Rectangle<int> (e.getScreenX(), e.getScreenY(), 1, 1)),
            [this, clip, noteIdx, tick, noteNum](int result)
            {
                switch (result)
                {
                    case 1:  pushUndo(clip); clip->removeNote(noteIdx); selectedNotes.clear(); break;
                    case 2:  pushUndo(clip); splitNote(clip, noteIdx, tick); break;
                    case 3:  showNotePropertiesDialog(clip, noteIdx); break;
                    case 4:  selectAll(clip); break;
                    case 5:  selectedNotes.clear(); velocityLane.setSelectedNote(-1); break;
                    case 6:  pushUndo(clip); quantiseSelected(clip); break;
                    case 7:  showSetVelocityDialog(clip); break;
                    case 8:  deleteSelected(clip); break;
                    case 9:  zoomToFit(clip); break;
                    case 20: setActiveTool (Tool::Select); break;
                    case 21: setActiveTool (Tool::Draw);   break;
                    case 22: setActiveTool (Tool::Erase);  break;
                    case 23: setActiveTool (Tool::Split);  break;
                    case 24: setActiveTool (Tool::Glue);   break;
                    default: break;
                }
                repaint(); velocityLane.repaint();
            });
    }

private:
    //==========================================================================
    //  References + state
    //==========================================================================
    SequencerEngine& engine;
    int activeTrack = 0;
    int activeClip  = 0;

    juce::Rectangle<int> toolbarBounds, rulerBounds, keysBounds, gridBounds;
    juce::ScrollBar hScroll { false }, vScroll { true };

    double  pixelsPerTick = 0.2;
    double  scrollX       = 0.0;
    int     noteRowOffset = 48;
    int     noteRowH      = 10;
    int64_t snapTicks     = MidiClip::kPPQ / 2;

    Tool    currentTool = Tool::Draw;

    // Loop region (-1 = not set)
    int64_t loopStartTick = -1;
    int64_t loopEndTick   = -1;
    int     loopDragMode  = 0;   // 0=none, 1=start, 2=end

    //── Drag state ─────────────────────────────────────────────────────────────
    enum class DragMode { None, Draw, Move, Resize, RubberBand };
    DragMode dragMode    = DragMode::None;
    int      dragNoteIdx = -1;
    int64_t  dragStartTick = 0, dragOrigStart = 0, dragOrigDuration = 0;
    int      dragStartNote = 0, dragOrigNote = 0;
    int64_t  dragGroupDeltaTick  = 0;
    int      dragGroupDeltaPitch = 0;

    juce::Point<int> rubberBandStart, rubberBandEnd;

    //── Selection ───────────────────────────────────────────────────────────────
    juce::Array<int> selectedNotes;
    VelocityLane     velocityLane;

    //── Clipboard ───────────────────────────────────────────────────────────────
    juce::Array<MidiNote> clipboard;

    //── Undo/Redo ───────────────────────────────────────────────────────────────
    static constexpr int kMaxUndo = 64;
    std::vector<juce::Array<MidiNote>> undoStack;
    std::vector<juce::Array<MidiNote>> redoStack;

    //── Toolbar buttons ─────────────────────────────────────────────────────────
    //  Tool buttons render a procedural vector glyph instead of a letter — see
    //  drawToolIcon()/ToolIconButton below (same "no external assets" approach
    //  LogoBar.cpp uses for its waveform icon).
    struct ToolIconButton : public juce::TextButton
    {
        Tool tool = Tool::Select;

        void paintButton (juce::Graphics& g, bool isHighlighted, bool isDown) override
        {
            getLookAndFeel().drawButtonBackground (g, *this,
                findColour (juce::TextButton::buttonColourId), isHighlighted, isDown);

            const auto colourId = getToggleState() ? juce::TextButton::textColourOnId
                                                     : juce::TextButton::textColourOffId;
            drawToolIcon (g, tool, getLocalBounds().toFloat().reduced (6.0f), findColour (colourId));
        }
    };

    ToolIconButton   btnSelect, btnDraw, btnErase, btnSplit, btnGlue;
    juce::TextButton btnUndo, btnRedo, btnZoomFit;

    /** Procedural glyph for each note-editing tool — arrow cursor (Select),
     *  pencil (Draw), eraser block (Erase), scissors (Split), glue drop (Glue).
     *  Shared by the toolbar buttons and the right-click tool submenu so both
     *  stay visually identical without needing bundled icon assets. */
    static void drawToolIcon (juce::Graphics& g, Tool tool, juce::Rectangle<float> b, juce::Colour colour)
    {
        const float cx = b.getCentreX();
        const float cy = b.getCentreY();
        const float s  = juce::jmin (b.getWidth(), b.getHeight());
        g.setColour (colour);

        switch (tool)
        {
            case Tool::Select:
            {
                // Classic arrow-cursor pointer.
                const float x = b.getX() + s * 0.20f;
                const float y = b.getY() + s * 0.08f;
                juce::Path p;
                p.startNewSubPath (x, y);
                p.lineTo (x, y + s * 0.66f);
                p.lineTo (x + s * 0.17f, y + s * 0.50f);
                p.lineTo (x + s * 0.29f, y + s * 0.76f);
                p.lineTo (x + s * 0.40f, y + s * 0.70f);
                p.lineTo (x + s * 0.28f, y + s * 0.44f);
                p.lineTo (x + s * 0.47f, y + s * 0.42f);
                p.closeSubPath();
                g.fillPath (p);
                break;
            }
            case Tool::Draw:
            {
                // Pencil, tilted, tip pointing down-left.
                juce::Path p;
                p.addRoundedRectangle (-s * 0.09f, -s * 0.40f, s * 0.18f, s * 0.60f, s * 0.03f);
                p.addTriangle (-s * 0.09f, s * 0.20f, s * 0.09f, s * 0.20f, 0.0f, s * 0.40f);
                p.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::pi * 0.78f)
                                       .translated (cx, cy));
                g.fillPath (p);
                break;
            }
            case Tool::Erase:
            {
                // Rotated eraser block with a two-tone divider line.
                const auto t = juce::AffineTransform::rotation (-juce::MathConstants<float>::pi * 0.2f)
                                    .translated (cx, cy);
                juce::Path body;
                body.addRoundedRectangle (-s * 0.28f, -s * 0.17f, s * 0.56f, s * 0.34f, s * 0.06f);
                body.applyTransform (t);
                g.fillPath (body);

                juce::Path divider;
                divider.addLineSegment ({ -s * 0.05f, -s * 0.17f, -s * 0.05f, s * 0.17f }, s * 0.025f);
                divider.applyTransform (t);
                g.setColour (colour.contrasting (0.5f).withAlpha (0.6f));
                g.fillPath (divider);
                break;
            }
            case Tool::Split:
            {
                // Scissors — crossing blades over two ring handles.
                juce::Path blades;
                blades.addLineSegment ({ cx - s * 0.16f, cy - s * 0.32f, cx + s * 0.16f, cy + s * 0.20f }, s * 0.05f);
                blades.addLineSegment ({ cx + s * 0.16f, cy - s * 0.32f, cx - s * 0.16f, cy + s * 0.20f }, s * 0.05f);
                g.fillPath (blades);
                g.drawEllipse (cx - s * 0.22f, cy + s * 0.14f, s * 0.14f, s * 0.14f, s * 0.035f);
                g.drawEllipse (cx + s * 0.08f, cy + s * 0.14f, s * 0.14f, s * 0.14f, s * 0.035f);
                break;
            }
            case Tool::Glue:
            {
                // Glue drop.
                juce::Path p;
                p.startNewSubPath (cx, cy - s * 0.34f);
                p.cubicTo (cx + s * 0.27f, cy - s * 0.02f, cx + s * 0.20f, cy + s * 0.34f, cx, cy + s * 0.34f);
                p.cubicTo (cx - s * 0.20f, cy + s * 0.34f, cx - s * 0.27f, cy - s * 0.02f, cx, cy - s * 0.34f);
                p.closeSubPath();
                g.fillPath (p);
                break;
            }
        }
    }

    /** Rasterises drawToolIcon() into a small Drawable, for use as the leading
     *  icon on the right-click tool submenu items (same glyphs as the toolbar
     *  buttons, so the two stay visually consistent). */
    static std::unique_ptr<juce::Drawable> makeToolMenuIcon (Tool tool, juce::Colour colour)
    {
        constexpr int size = 18;
        juce::Image img (juce::Image::ARGB, size, size, true);
        juce::Graphics g (img);
        drawToolIcon (g, tool, juce::Rectangle<float> (0.0f, 0.0f, (float) size, (float) size).reduced (1.0f), colour);

        auto d = std::make_unique<juce::DrawableImage>();
        d->setImage (img);
        return d;
    }

    /** Builds an actual mouse cursor out of the same glyph used on the toolbar
     *  button/menu item for `tool`, so the OS cursor over the grid always
     *  shows which tool is active rather than a generic system arrow/crosshair. */
    static juce::MouseCursor makeToolCursor (Tool tool)
    {
        constexpr int size = 24;
        juce::Image img (juce::Image::ARGB, size, size, true);
        juce::Graphics g (img);

        // One fixed bounding box for every stamp below — using two differently
        // sized boxes (expanding for an outline pass, shrinking for a fill
        // pass) desyncs position-anchored glyphs like the Select arrow (built
        // from b.getX()/b.getY(), not centred) between the two passes, warping
        // it into a shape that no longer matches the toolbar/menu icon.
        const auto b = juce::Rectangle<float> (2.0f, 2.0f, (float) size - 4.0f, (float) size - 4.0f);

        // Black outline: stamp the exact same glyph at 1px offsets in every
        // direction (translation preserves the shape exactly, unlike
        // rescaling), then a white fill dead-centre on top — like an OS
        // cursor's outline, but guaranteed to be the identical silhouette as
        // the toolbar/menu icon, just outlined for legibility over arbitrary
        // grid content.
        static const int offs[][2] = { {-1,0}, {1,0}, {0,-1}, {0,1}, {-1,-1}, {1,-1}, {-1,1}, {1,1} };
        for (auto& o : offs)
        {
            juce::Graphics::ScopedSaveState save (g);
            g.addTransform (juce::AffineTransform::translation ((float) o[0], (float) o[1]));
            drawToolIcon (g, tool, b, juce::Colours::black);
        }
        // White fill on top — kept theme-independent (a static function can't
        // call findColour() anyway) since these cursors are built once and
        // cached forever in toolCursorFor()'s static array.
        drawToolIcon (g, tool, b, juce::Colours::white);

        // Hotspot: the "business end" of each glyph (the arrow tip, pencil
        // point, glue-drop apex) — Erase/Split have no single sharp point, so
        // their hotspot is just the icon's centre.
        int hx = size / 2, hy = size / 2;
        switch (tool)
        {
            case Tool::Select: hx = (int) (size * 0.22f); hy = (int) (size * 0.10f); break;
            case Tool::Draw:   hx = (int) (size * 0.25f); hy = (int) (size * 0.82f); break;
            case Tool::Glue:   hy = (int) (size * 0.18f); break;
            default: break;
        }
        return juce::MouseCursor (img, hx, hy);
    }

    /** Cached per-tool cursors — built once, since makeToolCursor() rasterises
     *  an image and mouseMove fires far too often to redo that every call. */
    static const juce::MouseCursor& toolCursorFor (Tool tool)
    {
        static const juce::MouseCursor cursors[] = {
            makeToolCursor (Tool::Select), makeToolCursor (Tool::Draw), makeToolCursor (Tool::Erase),
            makeToolCursor (Tool::Split),  makeToolCursor (Tool::Glue)
        };
        return cursors[(int) tool];
    }

    //── Overlays ────────────────────────────────────────────────────────────────
    std::unique_ptr<juce::Component> velOverlay;

    //==========================================================================
    //  Helpers: coordinate mapping
    //==========================================================================
    int  visibleRows() const { return noteRowH > 0 ? gridBounds.getHeight() / noteRowH : 1; }
    int64_t xToTick (int x) const noexcept { return (int64_t)((x - kKeysW + scrollX) / pixelsPerTick); }
    float   tickToX (int64_t t) const noexcept { return (float)(t * pixelsPerTick - scrollX + kKeysW); }
    int     yToNote (int y)  const noexcept { return juce::jlimit (0, 127, (kNumNotes-1) - ((y - kRulerH - kToolbarH) / noteRowH + noteRowOffset)); }
    float   noteToY (int n)  const noexcept { return (float)(kRulerH + kToolbarH + ((kNumNotes-1-n) - noteRowOffset) * noteRowH); }
    int64_t snap    (int64_t t) const noexcept { return snapTicks > 0 ? ((t + snapTicks/2) / snapTicks) * snapTicks : t; }

    //==========================================================================
    //  Hit testing
    //==========================================================================
    int hitTestAny (MidiClip* clip, int64_t tick, int note) const
    {
        if (! clip) return -1;
        const juce::ScopedReadLock sl (clip->getLock());
        for (int i = 0; i < clip->getNotes().size(); ++i)
        {
            const auto& n = clip->getNotes().getReference (i);
            if (n.note == note && tick >= n.startTick && tick < n.endTick()) return i;
        }
        return -1;
    }

    bool isNearRightEdge (MidiClip* clip, int mouseX, int idx) const
    {
        if (! clip) return false;
        const juce::ScopedReadLock sl (clip->getLock());
        if (! juce::isPositiveAndBelow (idx, clip->getNotes().size())) return false;
        return std::abs (mouseX - (int) tickToX (clip->getNotes()[idx].endTick())) < kResizeZone;
    }

    //==========================================================================
    //  Undo/Redo
    //==========================================================================
    void pushUndo (MidiClip* clip)
    {
        if (! clip) return;
        const juce::ScopedReadLock sl (clip->getLock());
        undoStack.push_back (clip->getNotes());
        if ((int)undoStack.size() > kMaxUndo)
            undoStack.erase (undoStack.begin());
        redoStack.clear();
        updateUndoRedoButtons();
    }

    void undo (MidiClip* clip)
    {
        if (! clip || undoStack.empty()) return;
        const juce::ScopedReadLock sl (clip->getLock());
        redoStack.push_back (clip->getNotes());
        auto prev = undoStack.back(); undoStack.pop_back();
        clip->setNotes (prev);
        selectedNotes.clear();
        updateUndoRedoButtons();
        repaint(); velocityLane.repaint();
    }

    void redo (MidiClip* clip)
    {
        if (! clip || redoStack.empty()) return;
        const juce::ScopedReadLock sl (clip->getLock());
        undoStack.push_back (clip->getNotes());
        auto next = redoStack.back(); redoStack.pop_back();
        clip->setNotes (next);
        selectedNotes.clear();
        updateUndoRedoButtons();
        repaint(); velocityLane.repaint();
    }

    void updateUndoRedoButtons()
    {
        btnUndo.setEnabled (! undoStack.empty());
        btnRedo.setEnabled (! redoStack.empty());
    }

    //==========================================================================
    //  Edit operations
    //==========================================================================
    void selectAll (MidiClip* clip)
    {
        if (! clip) return;
        selectedNotes.clear();
        const juce::ScopedReadLock sl (clip->getLock());
        for (int i = 0; i < clip->getNotes().size(); ++i)
            selectedNotes.add (i);
        repaint();
    }

    void deleteSelected (MidiClip* clip)
    {
        if (! clip || selectedNotes.isEmpty()) return;
        pushUndo (clip);
        juce::Array<int> sorted = selectedNotes;
        sorted.sort();
        for (int i = sorted.size() - 1; i >= 0; --i)
            clip->removeNote (sorted[i]);
        selectedNotes.clear();
        velocityLane.setSelectedNote (-1);
        repaint(); velocityLane.repaint();
    }

    void copySelected (MidiClip* clip)
    {
        if (! clip || selectedNotes.isEmpty()) return;
        clipboard.clear();
        const juce::ScopedReadLock sl (clip->getLock());
        // Find earliest start to normalise offsets
        int64_t earliest = std::numeric_limits<int64_t>::max();
        for (int i : selectedNotes)
            if (juce::isPositiveAndBelow (i, clip->getNotes().size()))
                earliest = juce::jmin (earliest, clip->getNotes()[i].startTick);
        for (int i : selectedNotes)
        {
            if (juce::isPositiveAndBelow (i, clip->getNotes().size()))
            {
                auto n = clip->getNotes().getReference (i);
                n.startTick -= earliest;
                clipboard.add (n);
            }
        }
    }

    void pasteClipboard (MidiClip* clip)
    {
        if (! clip || clipboard.isEmpty()) return;

        // Paste at current playhead position, snapped
        const int64_t pasteStart = snap (engine.getPlayheadTick());
        selectedNotes.clear();

        for (const auto& n : clipboard)
        {
            MidiNote pasted = n;
            pasted.startTick += pasteStart;
            int idx = clip->addNote (pasted);
            selectedNotes.add (idx);
        }
        repaint(); velocityLane.repaint();
    }

    void duplicateSelected (MidiClip* clip)
    {
        if (! clip || selectedNotes.isEmpty()) return;
        copySelected (clip);

        // Find end of selection
        int64_t selEnd = 0;
        {
            const juce::ScopedReadLock sl (clip->getLock());
            for (int i : selectedNotes)
                if (juce::isPositiveAndBelow (i, clip->getNotes().size()))
                    selEnd = juce::jmax (selEnd, clip->getNotes()[i].endTick());
        }

        pushUndo (clip);
        // Temporarily move playhead to selEnd for paste
        const int64_t savedPH = engine.getPlayheadTick();
        engine.seekToTick (snap (selEnd));
        pasteClipboard (clip);
        engine.seekToTick (savedPH);
        repaint(); velocityLane.repaint();
    }

    void quantiseSelected (MidiClip* clip)
    {
        if (! clip || snapTicks <= 0 || selectedNotes.isEmpty()) return;
        pushUndo (clip);
        for (int idx : selectedNotes)
        {
            const juce::ScopedReadLock sl (clip->getLock());
            if (! juce::isPositiveAndBelow (idx, clip->getNotes().size())) continue;
            const int64_t oldStart = clip->getNotes()[idx].startTick;
            const int64_t snapped  = snap (oldStart);
            clip->moveNote (idx, snapped);
        }
        repaint();
    }

    void splitNote (MidiClip* clip, int idx, int64_t atTick)
    {
        if (! clip || ! juce::isPositiveAndBelow (idx, clip->getNotes().size())) return;
        MidiNote n;
        {
            const juce::ScopedReadLock sl (clip->getLock());
            n = clip->getNotes().getReference (idx);
        }
        if (atTick <= n.startTick || atTick >= n.endTick()) return;   // no-op: don't push undo for nothing

        pushUndo (clip);
        const int64_t origEnd  = n.endTick();
        clip->resizeNote (idx, atTick - n.startTick);

        MidiNote right = n;
        right.startTick    = atTick;
        right.durationTick = origEnd - atTick;
        clip->addNote (right);
        repaint();
    }

    void glueNotes (MidiClip* clip, int64_t tick, int noteNum)
    {
        // Find note at tick/noteNum and the next note on same pitch
        if (! clip) return;
        const juce::ScopedReadLock sl (clip->getLock());
        int idxA = -1;
        for (int i = 0; i < clip->getNotes().size(); ++i)
        {
            const auto& n = clip->getNotes().getReference (i);
            if (n.note == noteNum && tick >= n.startTick && tick < n.endTick())
                { idxA = i; break; }
        }
        if (idxA < 0) return;

        const int64_t aEnd = clip->getNotes()[idxA].endTick();
        int idxB = -1;
        for (int i = 0; i < clip->getNotes().size(); ++i)
        {
            if (i == idxA) continue;
            const auto& n = clip->getNotes().getReference (i);
            if (n.note == noteNum && n.startTick == aEnd)   // must be truly adjacent, not just "later"
                { idxB = i; break; }
        }
        if (idxB < 0) return;

        pushUndo (clip);
        const int64_t newDur = clip->getNotes()[idxB].endTick() - clip->getNotes()[idxA].startTick;
        clip->resizeNote (idxA, newDur);
        // remove higher index first
        if (idxB > idxA) clip->removeNote (idxB);
        else { clip->removeNote (idxA); clip->removeNote (idxB); }
        repaint();
    }

    void updateRubberBandSelection (MidiClip* clip)
    {
        if (! clip) return;
        auto rb = juce::Rectangle<int>::leftTopRightBottom (
            juce::jmin (rubberBandStart.x, rubberBandEnd.x),
            juce::jmin (rubberBandStart.y, rubberBandEnd.y),
            juce::jmax (rubberBandStart.x, rubberBandEnd.x),
            juce::jmax (rubberBandStart.y, rubberBandEnd.y));

        const int64_t tickL = xToTick (rb.getX());
        const int64_t tickR = xToTick (rb.getRight());
        const int noteTop   = yToNote (rb.getY());
        const int noteBot   = yToNote (rb.getBottom());

        selectedNotes.clear();
        const juce::ScopedReadLock sl (clip->getLock());
        for (int i = 0; i < clip->getNotes().size(); ++i)
        {
            const auto& n = clip->getNotes().getReference (i);
            if (n.note >= noteBot && n.note <= noteTop &&
                n.startTick < tickR && n.endTick() > tickL)
                selectedNotes.add (i);
        }
        velocityLane.setSelectedNote (selectedNotes.size() == 1 ? selectedNotes[0] : -1);
    }

    void zoomToFit (MidiClip* clip)
    {
        if (! clip) return;
        int64_t earliest = std::numeric_limits<int64_t>::max();
        int64_t latest   = 0;
        int     loNote   = 127, hiNote = 0;
        {
            const juce::ScopedReadLock sl (clip->getLock());
            for (const auto& n : clip->getNotes())
            {
                earliest = juce::jmin (earliest, n.startTick);
                latest   = juce::jmax (latest,   n.endTick());
                loNote   = juce::jmin (loNote,   n.note);
                hiNote   = juce::jmax (hiNote,   n.note);
            }
        }
        if (latest <= 0) return;
        if (earliest == std::numeric_limits<int64_t>::max()) earliest = 0;

        const int64_t span = juce::jmax ((int64_t)1, latest - earliest);
        pixelsPerTick = (double) gridBounds.getWidth() / (double) span * 0.9;
        scrollX = juce::jmax (0.0, earliest * pixelsPerTick - gridBounds.getWidth() * 0.05);

        const int noteSpan = juce::jmax (1, hiNote - loNote + 4);
        noteRowH = juce::jlimit (4, 24, gridBounds.getHeight() / noteSpan);
        noteRowOffset = juce::jmax (0, loNote - 2);
        syncScroll(); updateScrollRanges(); repaint();
    }

    //==========================================================================
    //  Tool handlers
    //==========================================================================
    void handleDrawDown (const juce::MouseEvent& e, MidiClip* clip, int64_t tick, int noteNum)
    {
        int idx = hitTestAny (clip, tick, noteNum);
        if (idx >= 0 && isNearRightEdge (clip, e.x, idx))
        {
            pushUndo (clip);
            dragMode = DragMode::Resize;
            dragNoteIdx = idx;
            dragStartTick = tick;
            const juce::ScopedReadLock sl (clip->getLock());
            dragOrigDuration = clip->getNotes()[idx].durationTick;
            selectedNotes.clear(); selectedNotes.add(idx);
        }
        else if (idx >= 0)
        {
            pushUndo (clip);
            dragMode = DragMode::Move;
            dragNoteIdx = idx;
            dragStartTick = tick;
            dragStartNote = noteNum;
            const juce::ScopedReadLock sl (clip->getLock());
            dragOrigStart = clip->getNotes()[idx].startTick;
            dragOrigNote  = clip->getNotes()[idx].note;
            selectedNotes.clear(); selectedNotes.add(idx);
            velocityLane.setSelectedNote(idx);
        }
        else
        {
            pushUndo (clip);
            dragMode = DragMode::Draw;
            dragStartTick = snap (tick);
            MidiNote n;
            n.note         = noteNum;
            n.velocity     = lastVelocity;
            n.startTick    = dragStartTick;
            n.durationTick = juce::jmax ((int64_t)1, snapTicks > 0 ? snapTicks : MidiClip::kPPQ / 4);
            dragNoteIdx    = clip->addNote (n);
            selectedNotes.clear(); selectedNotes.add (dragNoteIdx);
            velocityLane.setSelectedNote (dragNoteIdx);
        }
    }

    void handleSelectDown (const juce::MouseEvent& e, MidiClip* clip, int64_t tick, int noteNum)
    {
        int idx = hitTestAny (clip, tick, noteNum);

        if (idx >= 0)
        {
            if (isNearRightEdge (clip, e.x, idx))
            {
                pushUndo (clip);
                dragMode = DragMode::Resize;
                dragNoteIdx = idx;
                dragStartTick = tick;
                const juce::ScopedReadLock sl (clip->getLock());
                dragOrigDuration = clip->getNotes()[idx].durationTick;
                if (! selectedNotes.contains (idx)) { selectedNotes.clear(); selectedNotes.add(idx); }
            }
            else
            {
                if (! e.mods.isShiftDown() && ! selectedNotes.contains(idx))
                    selectedNotes.clear();
                if (e.mods.isShiftDown() && selectedNotes.contains(idx))
                    selectedNotes.removeFirstMatchingValue(idx);
                else
                    selectedNotes.addIfNotAlreadyThere(idx);
                velocityLane.setSelectedNote(idx);
                pushUndo (clip);
                dragMode = DragMode::Move;
                dragNoteIdx = idx;
                dragStartTick = tick;
                dragStartNote = noteNum;
                const juce::ScopedReadLock sl (clip->getLock());
                dragOrigStart = clip->getNotes()[idx].startTick;
                dragOrigNote  = clip->getNotes()[idx].note;
                dragGroupDeltaTick = 0; dragGroupDeltaPitch = 0;
            }
        }
        else
        {
            if (! e.mods.isShiftDown()) selectedNotes.clear();
            dragMode       = DragMode::RubberBand;
            rubberBandStart = e.getPosition();
            rubberBandEnd   = e.getPosition();
            velocityLane.setSelectedNote(-1);
        }
    }

    void handleEraseDown (const juce::MouseEvent&, MidiClip* clip, int64_t tick, int noteNum)
    {
        int idx = hitTestAny (clip, tick, noteNum);
        if (idx >= 0)
        {
            pushUndo (clip);
            clip->removeNote (idx);
            selectedNotes.removeFirstMatchingValue (idx);
            // Shift indices above
            for (int& s : selectedNotes) if (s > idx) --s;
        }
    }

    void handleSplitDown (const juce::MouseEvent&, MidiClip* clip, int64_t tick, int noteNum)
    {
        int idx = hitTestAny (clip, tick, noteNum);
        if (idx >= 0) splitNote (clip, idx, tick);
    }

    void handleGlueDown (const juce::MouseEvent&, MidiClip* clip, int64_t tick, int noteNum)
    {
        glueNotes (clip, tick, noteNum);
    }

    //==========================================================================
    //  Scroll sync
    //==========================================================================
    void syncScroll()
    {
        velocityLane.pixelsPerTick = pixelsPerTick;
        velocityLane.scrollOffsetX = scrollX;
        velocityLane.repaint();
    }

    void updateScrollRanges()
    {
        const double totalW = engine.getLengthTicks() * pixelsPerTick + gridBounds.getWidth();
        hScroll.setRangeLimits (0.0, totalW);
        hScroll.setCurrentRange (scrollX, scrollX + gridBounds.getWidth(), juce::dontSendNotification);
        vScroll.setRangeLimits (0.0, (double) kNumNotes);
        vScroll.setCurrentRange ((double) noteRowOffset, (double)(noteRowOffset + visibleRows()), juce::dontSendNotification);
        syncScroll();
    }

    //==========================================================================
    //  Timer
    //==========================================================================
    void timerCallback() override { repaint (gridBounds); }

    void scrollBarMoved (juce::ScrollBar* bar, double newRangeStart) override
    {
        if (bar == &hScroll)
        {
            scrollX = newRangeStart;
            syncScroll(); repaint();
        }
        else if (bar == &vScroll)
        {
            noteRowOffset = juce::jlimit (0, kNumNotes - visibleRows(), (int) newRangeStart);
            repaint();
        }
    }

    //==========================================================================
    //  Cursor
    //==========================================================================
    void updateCursorForPosition (const juce::MouseEvent& e)
    {
        if (! gridBounds.contains (e.getPosition())) { setMouseCursor (juce::MouseCursor::NormalCursor); return; }

        // Select tool: hovering an actual note still shows a resize/drag hint
        // (a more useful, specific affordance than the plain tool glyph), but
        // empty grid space falls back to the Select tool's own cursor glyph —
        // same idea as Draw/Erase/Split/Glue below, which always show their
        // tool's cursor since there's no equivalent per-note hint for them.
        if (currentTool == Tool::Select)
        {
            MidiClip* clip = engine.getClip (activeTrack, activeClip);
            const int64_t tick = xToTick (e.x);
            const int note = yToNote (e.y);
            const int idx = hitTestAny (clip, tick, note);
            if (idx >= 0 && isNearRightEdge (clip, e.x, idx))
                setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
            else if (idx >= 0)
                setMouseCursor (juce::MouseCursor::DraggingHandCursor);
            else
                setMouseCursor (toolCursorFor (Tool::Select));
            return;
        }

        setMouseCursor (toolCursorFor (currentTool));
    }

    //==========================================================================
    //  Toolbar
    //==========================================================================
    void buildToolbar()
    {
        auto setupBtn = [this](juce::TextButton& b, const juce::String& label, const juce::String& tip)
        {
            b.setButtonText (label);
            b.setTooltip (tip);
            b.setClickingTogglesState (false);
            b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xFF141820));
            b.setColour (juce::TextButton::buttonOnColourId, juce::Colour::fromFloatRGBA (0.25f,0.85f,0.85f,0.9f));
            b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xFF9AAABB));
            b.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xFF000000));
            addAndMakeVisible (b);
        };

        setupBtn (btnSelect,  "",        "Select tool (S)\nClick/rubber-band to select notes. Drag to move.\nRight-click for tool + edit menu.");
        setupBtn (btnDraw,    "",        "Draw tool (D)\nClick/drag to create notes.\nRight-click for tool + edit menu.");
        setupBtn (btnErase,   "",        "Erase tool (E)\nClick notes to delete.\nRight-click for tool + edit menu.");
        setupBtn (btnSplit,   "",        "Split tool (K)\nClick a note to split at cursor.\nRight-click for tool + edit menu.");
        setupBtn (btnGlue,    "",        "Glue tool (G)\nClick a note to merge with the next note on same pitch.\nRight-click for tool + edit menu.");
        setupBtn (btnUndo,    "↩",        "Undo (Ctrl+Z)");
        setupBtn (btnRedo,    "↪",        "Redo (Ctrl+Y)");
        setupBtn (btnZoomFit, "⊡",        "Zoom to fit (Ctrl+0)");

        btnSelect.tool = Tool::Select;
        btnDraw.tool   = Tool::Draw;
        btnErase.tool  = Tool::Erase;
        btnSplit.tool  = Tool::Split;
        btnGlue.tool   = Tool::Glue;

        btnSelect.onClick  = [this]{ setActiveTool(Tool::Select);  };
        btnDraw.onClick    = [this]{ setActiveTool(Tool::Draw);    };
        btnErase.onClick   = [this]{ setActiveTool(Tool::Erase);   };
        btnSplit.onClick   = [this]{ setActiveTool(Tool::Split);   };
        btnGlue.onClick    = [this]{ setActiveTool(Tool::Glue);    };
        btnUndo.onClick    = [this]{ MidiClip* c=engine.getClip(activeTrack); if(c) undo(c); };
        btnRedo.onClick    = [this]{ MidiClip* c=engine.getClip(activeTrack); if(c) redo(c); };
        btnZoomFit.onClick = [this]{ zoomToFit(engine.getClip(activeTrack)); };

        btnUndo.setEnabled (false);
        btnRedo.setEnabled (false);

        updateToolbarButtons();
    }

    void layoutToolbar()
    {
        auto r = toolbarBounds.reduced (4, 4).withTrimmedLeft (104);
        const int bw = 26, gap = 2;

        btnSelect.setBounds  (r.removeFromLeft (bw)); r.removeFromLeft(gap);
        btnDraw.setBounds    (r.removeFromLeft (bw)); r.removeFromLeft(gap);
        btnErase.setBounds   (r.removeFromLeft (bw)); r.removeFromLeft(gap);
        btnSplit.setBounds   (r.removeFromLeft (bw)); r.removeFromLeft(gap);
        btnGlue.setBounds    (r.removeFromLeft (bw)); r.removeFromLeft(gap*4);

        btnUndo.setBounds    (r.removeFromLeft (bw)); r.removeFromLeft(gap);
        btnRedo.setBounds    (r.removeFromLeft (bw)); r.removeFromLeft(gap);
        btnZoomFit.setBounds (r.removeFromLeft (bw));
    }

    void updateToolbarButtons()
    {
        struct { juce::TextButton* btn; Tool tool; } pairs[] = {
            {&btnSelect, Tool::Select},
            {&btnDraw,   Tool::Draw},
            {&btnErase,  Tool::Erase},
            {&btnSplit,  Tool::Split},
            {&btnGlue,   Tool::Glue},
        };
        for (auto& p : pairs)
            p.btn->setColour (juce::TextButton::buttonColourId,
                currentTool == p.tool
                    ? juce::Colour::fromFloatRGBA (0.25f, 0.85f, 0.85f, 0.25f)
                    : juce::Colour (0xFF141820));
    }

    //==========================================================================
    //  Dialogs
    //==========================================================================
    int lastVelocity = 100;

    void showNotePropertiesDialog (MidiClip* clip, int idx)
    {
        if (! juce::isPositiveAndBelow (idx, clip->getNotes().size())) return;
        const juce::ScopedReadLock sl (clip->getLock());
        const auto& n = clip->getNotes().getReference (idx);

        juce::MessageBoxOptions opts = juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::InfoIcon)
            .withTitle ("Note Properties")
            .withMessage ("Pitch: " + juce::MidiMessage::getMidiNoteName(n.note, true, true, 4) +
                          "\nStart: " + juce::String(n.startTick) + " ticks" +
                          "\nLength: " + juce::String(n.durationTick) + " ticks" +
                          "\nVelocity: " + juce::String(n.velocity))
            .withButton ("OK");
        juce::AlertWindow::showAsync (opts, nullptr);
    }

    void showSetVelocityDialog (MidiClip* clip)
    {
        // runModalLoop() removed in JUCE 8 — use an inline overlay instead.
        class SetVelocityOverlay : public juce::Component
        {
        public:
            std::function<void(bool, int)> onResult;

            SetVelocityOverlay (int currentVel)
            {
                editor.setText (juce::String (currentVel), false);
                editor.setInputRestrictions (3, "0123456789");
                editor.setSelectAllWhenFocused (true);
                editor.onReturnKey  = [this] { commit(); };
                editor.onEscapeKey  = [this] { dismiss(); };
                addAndMakeVisible (editor);

                okBtn.setButtonText ("OK");
                okBtn.onClick = [this] { commit(); };
                addAndMakeVisible (okBtn);

                cancelBtn.setButtonText ("Cancel");
                cancelBtn.onClick = [this] { dismiss(); };
                addAndMakeVisible (cancelBtn);

                setInterceptsMouseClicks (true, true);
            }

            void visibilityChanged() override
            {
                if (isVisible()) editor.grabKeyboardFocus();
            }

            void paint (juce::Graphics& g) override
            {
                g.setColour (juce::Colours::black.withAlpha (0.55f));
                g.fillRect (getLocalBounds());
                const auto box = dialogBox();
                g.setColour (juce::Colour (0xFF1A2030));
                g.fillRoundedRectangle (box.toFloat(), 0.0f);
                g.setColour (juce::Colour (0xFF3A8FCC).withAlpha (0.7f));
                g.drawRoundedRectangle (box.toFloat().reduced (0.5f), 0.0f, 1.5f);
                g.setFont (14.0f);
                g.setColour (juce::Colours::white);
                g.drawText ("Set Velocity", box.getX() + 16, box.getY() + 12,
                            box.getWidth() - 32, 20, juce::Justification::centredLeft);
                g.setFont (11.0f);
                g.setColour (juce::Colours::white.withAlpha (0.6f));
                g.drawText ("Enter velocity (1-127):", box.getX() + 16, box.getY() + 38,
                            box.getWidth() - 32, 16, juce::Justification::centredLeft);
            }

            void resized() override
            {
                const auto box = dialogBox();
                editor.setBounds (box.getX() + 16, box.getY() + 58,
                                  box.getWidth() - 32, 26);
                const int btnY = box.getBottom() - 38;
                const int btnW = (box.getWidth() - 48) / 2;
                okBtn    .setBounds (box.getX() + 16,           btnY, btnW, 26);
                cancelBtn.setBounds (box.getX() + 16 + btnW + 8, btnY, btnW, 26);
            }

            void mouseDown (const juce::MouseEvent& e) override
            {
                if (! dialogBox().contains (e.getPosition())) dismiss();
            }

        private:
            juce::TextEditor editor;
            juce::TextButton okBtn, cancelBtn;

            juce::Rectangle<int> dialogBox() const
            {
                const int w = juce::jmin (300, getWidth() - 40);
                const int h = 140;
                return { (getWidth() - w) / 2, (getHeight() - h) / 2, w, h };
            }

            void commit()
            {
                const int v = juce::jlimit (1, 127, editor.getText().getIntValue());
                if (onResult) onResult (true, v);
            }

            void dismiss()
            {
                if (onResult) onResult (false, 0);
            }
        };

        auto overlay = std::make_unique<SetVelocityOverlay> (lastVelocity);
        overlay->setBounds (getLocalBounds());

        auto* overlayPtr = overlay.get();
        overlayPtr->onResult = [this, clip, overlayPtr] (bool ok, int v)
        {
            if (ok)
            {
                lastVelocity = v;
                pushUndo (clip);
                for (int i : selectedNotes)
                    clip->setNoteVelocity (i, v);
                repaint();
                velocityLane.repaint();
            }
            removeChildComponent (overlayPtr);
            velOverlay.reset();
        };

        velOverlay = std::move (overlay);
        addAndMakeVisible (*velOverlay);
        velOverlay->setBounds (getLocalBounds());
        velOverlay->grabKeyboardFocus();
    }

    //==========================================================================
    //  Drawing
    //==========================================================================
    void drawToolbar (juce::Graphics& g)
    {
        g.setColour (juce::Colour (0xFF10131A));
        g.fillRect (toolbarBounds);
        g.setColour (juce::Colour (0xFF26303B));
        g.fillRect (toolbarBounds.withTrimmedTop (toolbarBounds.getHeight() - 1));

        // Editor identity stays visible even when this is detached from Arrange.
        g.setFont (DysektLookAndFeel::makeMonoFont (10.f));
        g.setColour (juce::Colour (0xFF75D7D1));
        g.drawText ("PIANO ROLL", toolbarBounds.getX() + 9, toolbarBounds.getY(),
                    84, toolbarBounds.getHeight(), juce::Justification::centredLeft, false);
        g.setColour (juce::Colour (0xFF2A3440));
        g.drawVerticalLine (96, (float) toolbarBounds.getY() + 7.f,
                            (float) toolbarBounds.getBottom() - 7.f);
    }

    void drawRuler (juce::Graphics& g)
    {
        g.setColour (juce::Colour (0xFF0D0D14));
        g.fillRect (rulerBounds);
        g.setColour (juce::Colour (0xFF1C2028));
        g.fillRect (rulerBounds.withTrimmedLeft (kKeysW));

        // Loop region tint
        if (loopStartTick >= 0 && loopEndTick > loopStartTick)
        {
            const float lx = tickToX (loopStartTick);
            const float rx = tickToX (loopEndTick);
            g.setColour (juce::Colour::fromFloatRGBA (0.25f, 0.85f, 0.85f, 0.15f));
            g.fillRect (lx, (float)rulerBounds.getY(), rx - lx, (float)rulerBounds.getHeight());
            g.setColour (juce::Colour::fromFloatRGBA (0.25f, 0.85f, 0.85f, 0.6f));
            g.drawVerticalLine ((int)lx, (float)rulerBounds.getY(), (float)rulerBounds.getBottom());
            g.drawVerticalLine ((int)rx, (float)rulerBounds.getY(), (float)rulerBounds.getBottom());
        }

        const double ticksPerBar = MidiClip::kPPQ * 4;
        const int64_t firstBar = (int64_t)(scrollX / (ticksPerBar * pixelsPerTick));
        const int64_t lastBar  = firstBar + (int64_t)(gridBounds.getWidth() / (ticksPerBar * pixelsPerTick)) + 2;

        // Sub-beat ticks
        const int64_t subSnap = snapTicks > 0 ? snapTicks : MidiClip::kPPQ / 4;
        for (int64_t t = firstBar * (int64_t)ticksPerBar; t < lastBar * (int64_t)ticksPerBar; t += subSnap)
        {
            const float x = tickToX (t);
            if (x < kKeysW) continue;
            const bool isBeat = (t % (int64_t)MidiClip::kPPQ == 0);
            const bool isBar  = (t % (int64_t)ticksPerBar == 0);
            if (! isBeat && ! isBar) continue;
            g.setColour (isBar ? juce::Colour (0xFF2A3040) : juce::Colour (0xFF1C2230));
            g.drawVerticalLine ((int)x, (float)rulerBounds.getY(), (float)rulerBounds.getBottom());
        }

        g.setFont (DysektLookAndFeel::makeMonoFont (10.f));
        for (int64_t bar = firstBar; bar <= lastBar; ++bar)
        {
            const float x = tickToX (bar * (int64_t) ticksPerBar);
            if (x < kKeysW) continue;
            g.setColour (juce::Colour (0xFF8090A0));
            g.drawText (juce::String (bar + 1), (int)x + 2, rulerBounds.getY(),
                        40, kRulerH, juce::Justification::centredLeft, false);
        }
    }

    void drawKeyboard (juce::Graphics& g)
    {
        // White-key base — the whole gutter starts white; black keys are
        // painted on top as shorter, true-black bars (standard DAW piano-roll
        // convention: one row per semitone, black keys drawn narrower).
        g.setColour (juce::Colour (0xFFF0F0EC));
        g.fillRect (keysBounds);

        const int top = yToNote (kRulerH + kToolbarH);
        const int bot = yToNote (gridBounds.getBottom());
        const float blackKeyW = (float) keysBounds.getWidth() * 0.62f;

        // Hairline separators between adjacent white keys.
        for (int note = bot; note <= top; ++note)
        {
            if (isBlackKey (note)) continue;
            const float y = noteToY (note);
            g.setColour (juce::Colour (0xFFC8C8C4));
            g.fillRect ((float) keysBounds.getX(), y, (float) keysBounds.getWidth(), 0.75f);
        }

        // Black keys — solid black, shorter than the white keys so the
        // white keys they sit between still read as continuous strips.
        for (int note = bot; note <= top; ++note)
        {
            if (! isBlackKey (note)) continue;
            const float y = noteToY (note);
            g.setColour (juce::Colours::black);
            g.fillRect ((float) keysBounds.getX(), y, blackKeyW, (float) noteRowH - 0.5f);
        }

        // Octave (C) markers + labels, coloured for contrast against the
        // white-key background.
        for (int note = bot; note <= top; ++note)
        {
            if ((note % 12) != 0) continue;
            const float y = noteToY (note);
            g.setColour (juce::Colour (0xFF8A8A86));
            g.fillRect ((float) keysBounds.getX(), y, (float) keysBounds.getWidth(), 1.0f);
            if (noteRowH >= 8)
            {
                g.setFont (DysektLookAndFeel::makeMonoFont (8.f));
                g.setColour (juce::Colour (0xFF404040));
                g.drawText ("C" + juce::String (note / 12 - 1),
                            keysBounds.getX(), (int) y,
                            keysBounds.getWidth() - 4, noteRowH,
                            juce::Justification::centredRight, false);
            }
        }

        // Separator line
        g.setColour (juce::Colour (0xFF2A3040));
        g.drawVerticalLine (keysBounds.getRight() - 1,
                            (float)keysBounds.getY(), (float)keysBounds.getBottom());
    }

    void drawGrid (juce::Graphics& g)
    {
        g.setColour (juce::Colour (0xFF0B0E14));
        g.fillRect (gridBounds);

        const int top = yToNote (kRulerH + kToolbarH);
        const int bot = yToNote (gridBounds.getBottom());

        // Horizontal note rows
        for (int note = bot; note <= top; ++note)
        {
            const float y = noteToY (note);
            if (isBlackKey (note))
            {
                g.setColour (juce::Colour (0xFF0E121A));
                g.fillRect ((float)gridBounds.getX(), y, (float)gridBounds.getWidth(), (float)noteRowH);
            }
            if ((note % 12) == 0)
            {
                g.setColour (juce::Colour::fromFloatRGBA (0.22f, 0.28f, 0.36f, 0.42f));
                g.fillRect ((float)gridBounds.getX(), y, (float)gridBounds.getWidth(), 0.75f);
            }
        }

        // Vertical beat + bar grid lines
        const double tpb = MidiClip::kPPQ;
        const int64_t fb = (int64_t)(scrollX / (tpb * pixelsPerTick));
        const int64_t lb = fb + (int64_t)(gridBounds.getWidth() / (tpb * pixelsPerTick)) + 2;
        for (int64_t beat = fb; beat <= lb; ++beat)
        {
            const float x = tickToX (beat * (int64_t) tpb);
            if (x < gridBounds.getX()) continue;
            const bool isBar = (beat % 4 == 0);
            g.setColour (isBar ? juce::Colour (0xFF354252) : juce::Colour (0xFF1B2330));
            g.drawVerticalLine ((int)x, (float)gridBounds.getY(), (float)gridBounds.getBottom());
        }

        // Sub-beat grid (snap grid)
        if (snapTicks > 0 && pixelsPerTick * snapTicks > 4.0)
        {
            g.setColour (juce::Colour::fromFloatRGBA (0.20f, 0.25f, 0.34f, 0.28f));
            const int64_t startSnap = (int64_t)(scrollX / (pixelsPerTick * snapTicks)) * snapTicks;
            for (int64_t t = startSnap; tickToX(t) < gridBounds.getRight(); t += snapTicks)
            {
                if (t % MidiClip::kPPQ == 0) continue;  // already drawn above
                const float x = tickToX (t);
                if (x < gridBounds.getX()) continue;
                g.drawVerticalLine ((int)x, (float)gridBounds.getY(), (float)gridBounds.getBottom());
            }
        }

        // Track colour tint
        const auto info = engine.getTrackInfo (activeTrack);
        g.setColour (info.colour.withAlpha (0.04f));
        g.fillRect (gridBounds);

        // Clip end boundary
        const auto clipInfo = engine.getClipInfo (activeTrack, activeClip);
        const float clipEndX = tickToX (clipInfo.lengthTicks);
        if (clipEndX >= gridBounds.getX() && clipEndX <= gridBounds.getRight())
        {
            g.setColour (juce::Colour::fromFloatRGBA (0.8f, 0.3f, 0.1f, 0.5f));
            g.drawVerticalLine ((int)clipEndX, (float)gridBounds.getY(), (float)gridBounds.getBottom());
        }
    }

    void drawLoopRegion (juce::Graphics& g)
    {
        if (loopStartTick < 0 || loopEndTick <= loopStartTick) return;
        const float lx = tickToX (loopStartTick);
        const float rx = tickToX (loopEndTick);
        g.setColour (juce::Colour::fromFloatRGBA (0.25f, 0.85f, 0.85f, 0.06f));
        g.fillRect (juce::Rectangle<float>(lx, (float)gridBounds.getY(), rx - lx, (float)gridBounds.getHeight()));
        g.setColour (juce::Colour::fromFloatRGBA (0.25f, 0.85f, 0.85f, 0.35f));
        g.drawVerticalLine ((int)lx, (float)gridBounds.getY(), (float)gridBounds.getBottom());
        g.drawVerticalLine ((int)rx, (float)gridBounds.getY(), (float)gridBounds.getBottom());
    }

    void drawNotes (juce::Graphics& g)
    {
        MidiClip* clip = engine.getClip (activeTrack, activeClip);
        if (! clip) return;

        const auto trackInfo = engine.getTrackInfo (activeTrack);
        const juce::ScopedReadLock sl (clip->getLock());

        for (int i = 0; i < clip->getNotes().size(); ++i)
        {
            const auto& n = clip->getNotes().getReference (i);
            const float x = tickToX (n.startTick);
            const float y = noteToY (n.note);
            const float w = juce::jmax (2.f, (float)(n.durationTick * pixelsPerTick) - 1.f);
            const float h = (float) noteRowH - 1.f;

            if (x + w < kKeysW || x > gridBounds.getRight()) continue;

            const bool sel = selectedNotes.contains (i);
            const auto baseCol = sel
                ? juce::Colour::fromFloatRGBA (0.25f, 0.85f, 0.85f, 1.0f)
                : trackInfo.colour.withAlpha (0.88f);

            // Note body
            g.setColour (baseCol);
            g.fillRoundedRectangle (x, y, w, h, 0.0f);

            // Velocity shading overlay
            const float velAlpha = (1.f - n.velocity / 127.f) * 0.38f;
            g.setColour (juce::Colour::fromFloatRGBA (0.f, 0.f, 0.f, velAlpha));
            g.fillRoundedRectangle (x, y, w, h, 0.0f);

            // Border
            g.setColour (sel ? baseCol.brighter (0.4f) : baseCol.brighter (0.2f).withAlpha (0.7f));
            g.drawRoundedRectangle (x + 0.5f, y + 0.5f, w - 1.f, h - 1.f, 0.0f, 0.75f);

            // Resize handle on right edge
            const float handleW = juce::jmin (4.f, w * 0.2f);
            g.setColour (baseCol.brighter (0.6f).withAlpha (0.7f));
            g.fillRoundedRectangle (x + w - handleW - 0.5f, y + 1.f,
                                    handleW, h - 2.f, 0.0f);

            // Note label (when rows are tall enough)
            if (noteRowH >= 11 && w > 18)
            {
                g.setFont (DysektLookAndFeel::makeMonoFont (juce::jmin (9.f, (float)noteRowH - 2.f)));
                g.setColour (sel ? juce::Colour (0xFF000000) : juce::Colours::white.withAlpha(0.7f));
                g.drawText (juce::MidiMessage::getMidiNoteName(n.note, true, false, 4),
                            (int)(x + 2.f), (int)y, (int)(w - 6.f), noteRowH,
                            juce::Justification::centredLeft, true);
            }
        }
    }

    void drawPlayhead (juce::Graphics& g)
    {
        const float x = tickToX (engine.getPlayheadTick());
        if (x < kKeysW || x > gridBounds.getRight()) return;

        // Line
        g.setColour (juce::Colour::fromFloatRGBA (0.9f, 0.5f, 0.15f, 0.85f));
        g.drawVerticalLine ((int)x, gridBounds.getY(), gridBounds.getBottom());

        // Triangle head in ruler
        juce::Path tri;
        tri.addTriangle (x - 5.f, (float)rulerBounds.getY(),
                         x + 5.f, (float)rulerBounds.getY(),
                         x,       (float)rulerBounds.getBottom());
        g.setColour (juce::Colour::fromFloatRGBA (0.9f, 0.5f, 0.15f, 0.9f));
        g.fillPath (tri);
    }

    void drawRubberBand (juce::Graphics& g)
    {
        if (dragMode != DragMode::RubberBand) return;
        auto rb = juce::Rectangle<int>::leftTopRightBottom (
            juce::jmin (rubberBandStart.x, rubberBandEnd.x),
            juce::jmin (rubberBandStart.y, rubberBandEnd.y),
            juce::jmax (rubberBandStart.x, rubberBandEnd.x),
            juce::jmax (rubberBandStart.y, rubberBandEnd.y));
        g.setColour (juce::Colour::fromFloatRGBA (0.25f, 0.85f, 0.85f, 0.12f));
        g.fillRect (rb);
        g.setColour (juce::Colour::fromFloatRGBA (0.25f, 0.85f, 0.85f, 0.7f));
        g.drawRect (rb, 1);
    }

    //==========================================================================
    static bool isBlackKey (int note) noexcept
    {
        const int p = note % 12;
        return p==1||p==3||p==6||p==8||p==10;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollComponent)
};
