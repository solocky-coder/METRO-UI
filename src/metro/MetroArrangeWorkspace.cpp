#include "MetroArrangeWorkspace.h"
#include "MetroColours.h"
#include "MetroMetrics.h"
#include "../sequencer/SequencerEngine.h"

namespace dysekt::metro
{
namespace
{
    constexpr int kDividerHeight = MetroMetrics::grid;         // 8px, aligned to the project's spacing grid
    constexpr int kMinArrangerHeight = MetroMetrics::grid * 10;
    constexpr int kMinEditorHeight   = MetroMetrics::grid * 21; // enough for toolbar + step header + a few rows + velocity lane
}

MetroArrangeWorkspace::MetroArrangeWorkspace (SequencerEngine& sequencer)
    : arrangementView (sequencer), stepSequencer (sequencer)
{
    arrangementView.setSelectionChangedCallback ([this] (const MetroSelection& selection)
                                                 { onArrangementSelectionChanged (selection); });
    stepSequencer.onCollapsedChanged = [this] (bool) { resized(); };

    addAndMakeVisible (arrangementView);
    addAndMakeVisible (stepSequencer);
}

MetroArrangeWorkspace::~MetroArrangeWorkspace() = default;

void MetroArrangeWorkspace::setSelectionChangedCallback (std::function<void (const MetroSelection&)> callback)
{
    onSelectionChanged = std::move (callback);
}

void MetroArrangeWorkspace::onArrangementSelectionChanged (const MetroSelection& selection)
{
    // Selecting a clip opens it in the step editor; selecting a track or
    // clearing the selection closes it — see integration spec section 2.
    if (selection.isClip())
        stepSequencer.setActiveClip (selection.trackIndex, selection.clipIndex);
    else
        stepSequencer.clearActiveClip();

    if (onSelectionChanged != nullptr)
        onSelectionChanged (selection);
}

bool MetroArrangeWorkspace::isEditorCollapsed() const
{
    return stepSequencer.isCollapsed();
}

void MetroArrangeWorkspace::resized()
{
    auto area = getLocalBounds();

    if (isEditorCollapsed())
    {
        stepSequencer.setBounds (area.removeFromBottom (MetroStepSequencer::collapsedHeight()));
        area.removeFromBottom (kDividerHeight);
        arrangementView.setBounds (area);
        return;
    }

    const int totalHeight = juce::jmax (0, area.getHeight() - kDividerHeight);
    int editorHeight = (int) ((float) totalHeight * (1.0f - arrangerHeightFraction));
    editorHeight = juce::jlimit (kMinEditorHeight, juce::jmax (kMinEditorHeight, totalHeight - kMinArrangerHeight),
                                editorHeight);

    stepSequencer.setBounds (area.removeFromBottom (editorHeight));
    area.removeFromBottom (kDividerHeight);
    arrangementView.setBounds (area);
}

void MetroArrangeWorkspace::paint (juce::Graphics& g)
{
    g.setColour (Base::Background);
    g.fillRect (dividerBounds());
    g.setColour (Base::Border);
    g.fillRect (dividerBounds().withHeight (1));
    g.fillRect (dividerBounds().withY (dividerBounds().getBottom() - 1).withHeight (1));
}

juce::Rectangle<int> MetroArrangeWorkspace::dividerBounds() const
{
    if (isEditorCollapsed())
        return { 0, stepSequencer.getY() - kDividerHeight, getWidth(), kDividerHeight };
    return { 0, arrangementView.getBottom(), getWidth(), kDividerHeight };
}

void MetroArrangeWorkspace::mouseDown (const juce::MouseEvent& e)
{
    draggingDivider = ! isEditorCollapsed() && dividerBounds().contains (e.getPosition());
}

void MetroArrangeWorkspace::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingDivider || isEditorCollapsed())
        return;

    const int totalHeight = juce::jmax (1, getHeight() - kDividerHeight);
    const int upperBound = juce::jmax (kMinArrangerHeight, totalHeight - kMinEditorHeight);
    const int arrangerHeight = juce::jlimit (kMinArrangerHeight, upperBound, e.y);
    arrangerHeightFraction = (float) arrangerHeight / (float) totalHeight;
    resized();
}

void MetroArrangeWorkspace::mouseMove (const juce::MouseEvent& e)
{
    const bool overDivider = ! isEditorCollapsed() && dividerBounds().contains (e.getPosition());
    setMouseCursor (overDivider ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
}

void MetroArrangeWorkspace::mouseUp (const juce::MouseEvent&)
{
    draggingDivider = false;
}

} // namespace dysekt::metro
