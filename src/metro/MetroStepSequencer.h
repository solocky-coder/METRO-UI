/*
    DYSEKT 2
    Metro UI

    MetroStepSequencer.h

    A step-grid clip editor for the Metro standalone. Displays and edits
    exactly one MidiClip at a time — the clip currently selected in the
    arranger (see MetroArrangeWorkspace) — through SequencerEngine's
    existing message-thread MidiClip editing API. Owns no note data and no
    playback engine of its own; see METRO_STEP_SEQUENCER_UI_INTEGRATION.md
    for the design this implements.

    Like MetroArrangementView, this is a single hand-painted component
    rather than one child component per grid cell — the toolbar's controls
    are the only real juce::Component children; the row header, step
    header, grid and velocity lane are all drawn directly in paint() so a
    64-step x many-row pattern never has to construct hundreds of Buttons.
*/
#pragma once

#include <functional>
#include <set>
#include <utility>
#include <vector>
#include <juce_data_structures/juce_data_structures.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/SequencerEngine.h"

namespace dysekt::metro
{
/** One row of the step grid. Rows store their musical identity (a MIDI
    note number) independently of their visual position, per the
    integration spec — row 0 is never assumed to mean note 36. */
struct StepRow
{
    juce::String name;
    int          midiNote = 60;
    juce::Colour colour;
};

/** Grid resolution, expressed as steps per quarter note. */
enum class StepResolution
{
    eighth       = 2,
    sixteenth    = 4,
    thirtySecond = 8
};

/** Metro-skinned step-grid clip editor. */
class MetroStepSequencer final : public juce::Component,
                                  private juce::Timer
{
public:
    explicit MetroStepSequencer (SequencerEngine& sequencer);
    ~MetroStepSequencer() override;

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
     *  the integration spec. */
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
    //  Row model
    //==========================================================================
    void rebuildRows();
    void updateToolbarTitle();
    void setResolution (StepResolution);
    void setNumSteps (int newNumSteps);
    void updateToolbarToggleStates();

    //==========================================================================
    //  Grid geometry
    //==========================================================================
    void updateStepTicks();
    int  rowHeaderWidth() const;
    int  stepCellWidth()  const;
    int  gridContentWidth()  const;
    int  gridContentHeight() const;
    void clampScroll();

    juce::Rectangle<int> toolbarBounds()     const;
    juce::Rectangle<int> stepHeaderBounds()  const;
    juce::Rectangle<int> rowHeaderBounds()   const;
    juce::Rectangle<int> gridBounds()        const;
    juce::Rectangle<int> velocityLaneBounds() const;

    /** Bounds of one cell in component-local coordinates, after scroll. */
    juce::Rectangle<int> cellBounds (int rowIndex, int stepIndex) const;

    /** -1 if the point isn't over a row / step. */
    int rowAtY  (int y) const;
    int stepAtX (int x) const;

    /** When the grid/row content is smaller than the visible area (e.g. a
     *  short clip's note range, or few steps, in a wide docked/floating
     *  panel), centre it instead of pinning it to the top-left and leaving
     *  a dead, undecorated block of flat background filling the rest of
     *  the panel. Both are 0 once content needs to scroll (content >=
     *  visible), which is exactly when clampScroll() already pins
     *  stepScrollPx/rowScrollPx to 0 too, so this never fights scrolling. */
    int gridOffsetX() const;
    int gridOffsetY() const;

    //==========================================================================
    //  Editing
    //==========================================================================
    MidiClip* resolveClip() const;

    /** Applies (or skips, if already touched) one step toggle during an
     *  in-progress click/drag gesture. `adding` fixes the gesture's
     *  direction, established by the first cell touched. */
    void applyStepDuringGesture (int rowIndex, int stepIndex, bool adding);

    void toggleFocusedStep();
    void deleteSelectedSteps();
    void selectAllActiveSteps();
    void copySelectedSteps();
    void pasteClipboard();
    void moveFocus (int rowDelta, int stepDelta, bool extendSelection);

    void showStepContextMenu (int rowIndex, int stepIndex, juce::Point<int> screenPos);
    void beginVelocityDrag (int rowIndex, int stepIndex, int startY);
    void continueVelocityDrag (int y);

    //==========================================================================
    //  Playback feedback
    //==========================================================================
    void timerCallback() override;
    /** Clip-relative step under the playhead, or -1 if playback is outside
     *  the selected clip (or nothing is selected/playing). */
    int  currentPlayheadStep() const;
    void updatePlayheadAndFollow();

    //==========================================================================
    //  Painting
    //==========================================================================
    void drawEmptyState     (juce::Graphics&, const juce::String& message);
    void drawToolbarBackdrop(juce::Graphics&);
    void drawStepHeader     (juce::Graphics&);
    void drawRowHeaders     (juce::Graphics&);
    void drawGrid           (juce::Graphics&);
    void drawVelocityLane   (juce::Graphics&);

    //==========================================================================
    SequencerEngine& engine;

    // Active clip — resolved through stable track/clip indices, matching
    // the rest of the arranger's selection model (see MetroSelection).
    int activeTrackIndex = -1;
    int activeClipIndex  = -1;
    SequencerTrackInfo activeTrackInfo;
    SequencerClipInfo  activeClipInfo;

    std::vector<StepRow> rows;

    StepResolution resolution = StepResolution::sixteenth;
    int     numSteps  = 16;
    int64_t stepTicks = MidiClip::kPPQ / 4;

    int rowScrollPx  = 0;
    int stepScrollPx = 0;

    // Click/drag-paint gesture state.
    bool isDraggingSteps = false;
    bool dragIsAdding = true;
    std::set<std::pair<int, int>> touchedThisGesture;

    // Velocity-lane drag gesture state.
    bool velocityDragActive = false;
    int  velocityDragStep = -1;

    int hoverRow = -1, hoverStep = -1;
    int focusedRow = 0, focusedStep = 0;
    // Active (row, step) cells the user has selected for velocity editing,
    // deletion, or copy — always a subset of currently-active steps.
    std::set<std::pair<int, int>> selectedSteps;

    std::vector<MidiNote> clipboard;

    juce::UndoManager undoManager;

    // Follow-mode playhead tracking.
    int lastPlayheadStep = -1;
    bool followEnabled = true;

    bool collapsed = false;

    // Toolbar children.
    juce::Label      titleLabel;
    juce::Label      clipNameLabel;
    juce::TextButton res8Button   { "1/8" };
    juce::TextButton res16Button  { "1/16" };
    juce::TextButton res32Button  { "1/32" };
    juce::TextButton len8Button   { "8" };
    juce::TextButton len16Button  { "16" };
    juce::TextButton len32Button  { "32" };
    juce::TextButton len64Button  { "64" };
    juce::TextButton followButton   { "FOLLOW" };
    juce::TextButton collapseButton { juce::CharPointer_UTF8 ("\xE2\x96\xBE") }; // ▾

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MetroStepSequencer)
};
} // namespace dysekt::metro
