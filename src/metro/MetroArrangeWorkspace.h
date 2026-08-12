/*
    DYSEKT 2
    Metro UI

    MetroArrangeWorkspace.h

    Owns and lays out the arrangement workspace: MetroArrangementView on
    top (the song-level view, always visible — SEQ opens this workspace,
    it doesn't mean any one particular editor), a draggable horizontal
    divider, and the lower clip editor below.

    The lower editor has two interchangeable *modes* over the same
    selected clip — MetroStepSequencer (STEPS) and MetroPianoRoll
    (PIANO ROLL) — switched via a small tab strip. Both modes read/write
    the same MidiClip through SequencerEngine directly, so switching
    modes never loses anything. The workspace remembers which mode was
    last chosen per track and, the first time a track is selected,
    defaults intelligently (drum-style tracks -> Steps, pitched
    instrument tracks -> Piano Roll) rather than hard-coding one editor.

    Routes MetroArrangementView's selection callback into whichever
    editor mode is (or becomes) active (see
    METRO_STEP_SEQUENCER_UI_INTEGRATION.md section 2) while still
    exposing that same selection outward, so callers that previously
    listened to MetroArrangementView directly (e.g. MetroStandaloneEditor's
    inspector) keep working unchanged.
*/
#pragma once

#include <functional>
#include <map>
#include <juce_gui_basics/juce_gui_basics.h>
#include "MetroArrangementView.h"
#include "MetroPianoRoll.h"
#include "MetroSelection.h"
#include "MetroStepSequencer.h"

class SequencerEngine;

namespace dysekt::metro
{
/** Which clip-editor mode is showing in the lower half of the workspace. */
enum class MetroEditorMode { steps, pianoRoll };

/** Split-pane container: arranger on top, PIANO ROLL / STEPS tabbed clip
    editor below, divider between. */
class MetroArrangeWorkspace final : public juce::Component
{
public:
    explicit MetroArrangeWorkspace (SequencerEngine& sequencer);
    ~MetroArrangeWorkspace() override;

    void resized() override;
    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;

    /** Forwards MetroArrangementView::getSelection(). Kept so
     *  MetroStandaloneEditor can keep driving its inspector exactly as it
     *  did before this workspace existed. */
    const MetroSelection& getSelection() const noexcept { return arrangementView.getSelection(); }

    /** Fired whenever the arranger's selection changes — same contract as
     *  MetroArrangementView::setSelectionChangedCallback(). */
    void setSelectionChangedCallback (std::function<void (const MetroSelection&)> callback);

    MetroArrangementView& getArrangementView() noexcept { return arrangementView; }
    MetroStepSequencer&   getStepSequencer()   noexcept { return stepSequencer; }
    MetroPianoRoll&        getPianoRoll()       noexcept { return pianoRoll; }

    /** Currently-showing editor mode for the selected track. */
    MetroEditorMode getEditorMode() const noexcept { return currentEditorMode; }

    /** Manually switches editor mode for the currently-selected track, and
     *  remembers the choice for that track (spec: "allow manual switching
     *  at any time" / "remember the selected view per track"). No-op if no
     *  track is selected. */
    void setEditorMode (MetroEditorMode mode);

private:
    void onArrangementSelectionChanged (const MetroSelection& selection);
    void showEditorMode (MetroEditorMode mode);
    MetroEditorMode defaultModeForTrack (int trackIndex) const;
    void syncCollapsed (bool collapsed);
    void updateTabButtonStates();

    juce::Rectangle<int> dividerBounds() const;
    juce::Rectangle<int> tabBarBounds() const;
    bool isEditorCollapsed() const;

    SequencerEngine& engine;

    MetroArrangementView arrangementView;
    MetroStepSequencer   stepSequencer;
    MetroPianoRoll        pianoRoll;

    juce::TextButton pianoRollTabButton { "PIANO ROLL" };
    juce::TextButton stepsTabButton     { "STEPS" };

    MetroEditorMode currentEditorMode = MetroEditorMode::steps;
    int activeTrackIndex = -1;

    // Per-track editor-mode memory (spec: "remember the selected view per
    // track"). Only tracks the user has explicitly picked a view for, or
    // that have already been defaulted once, appear here.
    std::map<int, MetroEditorMode> viewModeByTrack;

    // Fraction of the available height (below the divider strip) given to
    // the arranger, per the spec's default ~35/65 split. Preserved for the
    // lifetime of the app; not yet persisted across sessions.
    float arrangerHeightFraction = 0.35f;
    bool draggingDivider = false;

    std::function<void (const MetroSelection&)> onSelectionChanged;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MetroArrangeWorkspace)
};
} // namespace dysekt::metro
