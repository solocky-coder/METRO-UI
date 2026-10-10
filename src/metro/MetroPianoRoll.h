/*
    DYSEKT 2
    Metro UI

    MetroPianoRoll.h

    A continuous-pitch clip editor for the Metro standalone. Displays and
    edits exactly one MidiClip at a time — the clip currently selected in
    the arranger (see MetroArrangeWorkspace) — through SequencerEngine's
    existing message-thread MidiClip editing API.

    Deliberately mirrors MetroStepSequencer's architecture and public
    contract (setActiveClip / clearActiveClip / collapse handling / toolbar
    shape) so MetroArrangeWorkspace can hold both as interchangeable "editor
    mode" views over the same underlying clip: switching between PIANO ROLL
    and STEPS never loses data, because neither view owns note data of its
    own — both read/write straight through to the MidiClip resolved from
    SequencerEngine each time.

    Like MetroStepSequencer, this is a single hand-painted component rather
    than one child component per note — the toolbar's controls are the only
    real juce::Component children; the piano-key gutter, time ruler and note
    grid are all drawn directly in paint().
*/
#pragma once

#include <functional>
#include <set>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/SequencerEngine.h"
#include "MetroStepSequencer.h" // shares StepResolution — piano roll's snap grid uses the same resolutions as the step grid

namespace dysekt::metro
{
/** Metro-skinned continuous-pitch clip editor. */
class MetroPianoRoll final : public juce::Component,
                              private juce::Timer
{
public:
    explicit MetroPianoRoll (SequencerEngine& sequencer);
    ~MetroPianoRoll() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseDown  (const juce::MouseEvent&) override;
    void mouseDrag  (const juce::MouseEvent&) override;
    void mouseUp    (const juce::MouseEvent&) override;
    void mouseMove  (const juce::MouseEvent&) override;
    void mouseExit  (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusGained (juce::Component::FocusChangeType) override;
    void focusLost   (juce::Component::FocusChangeType) override;

    /** Points the editor at a specific arranger clip. Matches the
     *  MetroArrangeWorkspace::onArrangementSelectionChanged() contract in
     *  the integration spec — same contract MetroStepSequencer implements,
     *  so the workspace can call both views identically. */
    void setActiveClip (int trackIndex, int clipIndex);

    /** Shows the "no clip selected" empty state. */
    void clearActiveClip();

    bool isCollapsed() const noexcept { return collapsed; }
    void setCollapsed (bool shouldBeCollapsed);

    /** Fired whenever the collapse state changes, so the owning workspace
     *  can re-run its split layout. */
    std::function<void (bool)> onCollapsedChanged;

    /** Height to reserve when collapsed — just the toolbar strip. */
    static int collapsedHeight();

private:
    //==========================================================================
    //  Toolbar / state
    //==========================================================================
    void updateToolbarTitle();
    void setResolution (StepResolution);
    void updateToolbarToggleStates();

    //==========================================================================
    //  Grid geometry
    //==========================================================================
    juce::Rectangle<int> toolbarBounds()   const;
    juce::Rectangle<int> rulerBounds()     const;
    juce::Rectangle<int> keyGutterBounds() const;
    juce::Rectangle<int> gridBounds()      const;

    int   rowHeight() const noexcept { return kRowHeight; }
    float pixelsPerTick() const noexcept;
    int   gridContentWidth()  const;
    int   gridContentHeight() const;
    void  clampScroll();

    /** X position (component-local, after scroll) of a tick. */
    int   xForTick (int64_t tick) const;
    /** Tick at an X position (component-local), snapped to the current
     *  resolution unless snap is disabled by the caller. */
    int64_t tickForX (int x, bool snap) const;
    /** MIDI note number for a Y position (component-local, after scroll). -1 if out of range. */
    int   noteForY (int y) const;
    /** Y position (component-local, after scroll) of the top of a note's row. */
    int   yForNote (int note) const;

    bool isBlackKey (int note) const noexcept;

    /** Pixel bounds of a note event, clipped to nothing (caller intersects
     *  against gridBounds()). */
    juce::Rectangle<int> boundsForNote (const MidiNote&) const;

    //==========================================================================
    //  Editing
    //==========================================================================
    MidiClip* resolveClip() const;

    enum class DragMode { none, moveNote, resizeNote, createNote, marquee };

    /** -1 if the point isn't over a note. */
    int noteIndexAt (juce::Point<int> localPos) const;

    void deleteSelectedNotes();
    void selectAllNotes();
    void copySelectedNotes();
    void pasteClipboard();
    void commitDragIfAny();

    void showNoteContextMenu (int noteIndex, juce::Point<int> screenPos);

    //==========================================================================
    //  Playback feedback
    //==========================================================================
    void timerCallback() override;
    void updatePlayheadAndFollow();

    //==========================================================================
    //  Painting
    //==========================================================================
    void drawEmptyState      (juce::Graphics&, const juce::String& message);
    void drawToolbarBackdrop (juce::Graphics&);
    void drawRuler            (juce::Graphics&);
    void drawKeyGutter        (juce::Graphics&);
    void drawGrid              (juce::Graphics&);
    void drawNotes              (juce::Graphics&);

    //==========================================================================
    SequencerEngine& engine;

    // Active clip — resolved through stable track/clip indices, matching
    // the rest of the arranger's selection model (see MetroSelection). Kept
    // in lock-step with MetroStepSequencer's own activeTrackIndex/
    // activeClipIndex, since MetroArrangeWorkspace drives both views
    // together whenever the arranger selection changes.
    int activeTrackIndex = -1;
    int activeClipIndex  = -1;
    SequencerTrackInfo activeTrackInfo;
    SequencerClipInfo  activeClipInfo;

    StepResolution resolution = StepResolution::sixteenth;
    int64_t snapTicks = MidiClip::kPPQ / 4;

    int rowScrollPx  = 0; // vertical scroll, in pixels (pitch axis)
    int tickScrollPx = 0; // horizontal scroll, in pixels (time axis)

    static constexpr int kRowHeight = 12;
    static constexpr int kLowestNote  = 0;
    static constexpr int kHighestNote = 127;

    // Drag-gesture state.
    DragMode dragMode = DragMode::none;
    int   dragNoteIndex = -1;
    juce::Point<int> dragStartPos;
    MidiNote dragNoteOriginal; // snapshot at gesture start, for undo + delta math
    juce::Rectangle<int> marqueeRect;

    int hoverNoteIndex = -1;
    std::set<int> selectedNoteIndices;

    std::vector<MidiNote> clipboard;

    juce::UndoManager undoManager;

    // Follow-mode playhead tracking.
    bool followEnabled = true;

    bool collapsed = false;

    // Toolbar children.
    juce::Label      titleLabel;
    juce::Label      clipNameLabel;
    juce::TextButton res8Button   { "1/8" };
    juce::TextButton res16Button  { "1/16" };
    juce::TextButton res32Button  { "1/32" };
    juce::TextButton followButton   { "FOLLOW" };
    juce::TextButton collapseButton { juce::CharPointer_UTF8 ("\xE2\x96\xBE") }; // ▾

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MetroPianoRoll)
};
} // namespace dysekt::metro
