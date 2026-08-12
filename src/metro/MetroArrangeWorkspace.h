/*
    DYSEKT 2
    Metro UI

    MetroArrangeWorkspace.h

    Owns and lays out the arrangement workspace: MetroArrangementView on
    top, a draggable horizontal divider, and MetroStepSequencer below.
    Routes MetroArrangementView's selection callback into the step editor
    (see METRO_STEP_SEQUENCER_UI_INTEGRATION.md section 2) while still
    exposing that same selection outward, so callers that previously
    listened to MetroArrangementView directly (e.g. MetroStandaloneEditor's
    inspector) keep working unchanged.
*/
#pragma once

#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include "MetroArrangementView.h"
#include "MetroSelection.h"
#include "MetroStepSequencer.h"

class SequencerEngine;

namespace dysekt::metro
{
/** Split-pane container: arranger on top, step editor below, divider between. */
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

private:
    void onArrangementSelectionChanged (const MetroSelection& selection);
    juce::Rectangle<int> dividerBounds() const;
    bool isEditorCollapsed() const;

    MetroArrangementView arrangementView;
    MetroStepSequencer   stepSequencer;

    // Fraction of the available height (below the divider strip) given to
    // the arranger, per the spec's default ~35/65 split. Preserved for the
    // lifetime of the app; not yet persisted across sessions.
    float arrangerHeightFraction = 0.35f;
    bool draggingDivider = false;

    std::function<void (const MetroSelection&)> onSelectionChanged;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MetroArrangeWorkspace)
};
} // namespace dysekt::metro
