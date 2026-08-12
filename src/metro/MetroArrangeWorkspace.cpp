#include "MetroArrangeWorkspace.h"
#include "MetroColours.h"
#include "MetroMetrics.h"
#include "MetroTypography.h"
#include "../sequencer/SequencerEngine.h"

namespace dysekt::metro
{
namespace
{
    constexpr int kDividerHeight = MetroMetrics::grid;         // 8px, aligned to the project's spacing grid
    constexpr int kTabBarHeight  = MetroMetrics::grid * 3;     // 24px strip for the PIANO ROLL | STEPS tabs
    constexpr int kMinArrangerHeight = MetroMetrics::grid * 10;
    constexpr int kMinEditorHeight   = MetroMetrics::grid * 21; // enough for tab bar + toolbar + a few rows

    /** Same drum-vs-instrument test MetroStepSequencer::rebuildRows() uses
     *  to decide between its 16-pad drum mapping and a chromatic note
     *  range — reused here so "default intelligently" and the step
     *  editor's own row model never disagree about what counts as a
     *  drum track. */
    bool isDrumStyleTrack (const SequencerTrackInfo& info) noexcept
    {
        const bool isMainTrack = info.type == TrackType::MainSlice;
        // Bank 128 is the General MIDI convention for a percussion bank —
        // used here to tell a drum-kit SF preset apart from a melodic one.
        const bool isDrumKit = info.type == TrackType::SfPlayer
                                 && ! info.isSfzInstrument
                                 && info.preset.bank == 128;
        return isMainTrack || isDrumKit;
    }
}

MetroArrangeWorkspace::MetroArrangeWorkspace (SequencerEngine& sequencer)
    : engine (sequencer), arrangementView (sequencer), stepSequencer (sequencer), pianoRoll (sequencer)
{
    arrangementView.setSelectionChangedCallback ([this] (const MetroSelection& selection)
                                                 { onArrangementSelectionChanged (selection); });
    stepSequencer.onCollapsedChanged = [this] (bool c) { syncCollapsed (c); };
    pianoRoll.onCollapsedChanged     = [this] (bool c) { syncCollapsed (c); };

    addAndMakeVisible (arrangementView);
    addAndMakeVisible (stepSequencer);
    addAndMakeVisible (pianoRoll);

    auto setUpTab = [this] (juce::TextButton& b)
    {
        b.setClickingTogglesState (false);
        b.setColour (juce::TextButton::buttonOnColourId, Accent::Cyan);
        addAndMakeVisible (b);
    };
    setUpTab (pianoRollTabButton);
    setUpTab (stepsTabButton);
    pianoRollTabButton.onClick = [this] { setEditorMode (MetroEditorMode::pianoRoll); };
    stepsTabButton.onClick     = [this] { setEditorMode (MetroEditorMode::steps); };

    showEditorMode (currentEditorMode);
}

MetroArrangeWorkspace::~MetroArrangeWorkspace() = default;

void MetroArrangeWorkspace::setSelectionChangedCallback (std::function<void (const MetroSelection&)> callback)
{
    onSelectionChanged = std::move (callback);
}

MetroEditorMode MetroArrangeWorkspace::defaultModeForTrack (int trackIndex) const
{
    return isDrumStyleTrack (engine.getTrackInfo (trackIndex)) ? MetroEditorMode::steps
                                                                : MetroEditorMode::pianoRoll;
}

void MetroArrangeWorkspace::onArrangementSelectionChanged (const MetroSelection& selection)
{
    // Selecting a clip opens it in the lower editor (in whichever mode is
    // remembered/defaulted for that track) — selecting a track or clearing
    // the selection closes it. See integration spec section 2.
    if (selection.isClip())
    {
        activeTrackIndex = selection.trackIndex;

        auto it = viewModeByTrack.find (activeTrackIndex);
        const MetroEditorMode mode = (it != viewModeByTrack.end()) ? it->second
                                                                    : defaultModeForTrack (activeTrackIndex);
        // Remember the (possibly just-defaulted) mode so re-selecting this
        // track later — even before the user has manually switched
        // anything — stays consistent.
        viewModeByTrack[activeTrackIndex] = mode;
        showEditorMode (mode);

        stepSequencer.setActiveClip (selection.trackIndex, selection.clipIndex);
        pianoRoll.setActiveClip     (selection.trackIndex, selection.clipIndex);
    }
    else
    {
        activeTrackIndex = -1;
        stepSequencer.clearActiveClip();
        pianoRoll.clearActiveClip();
    }

    if (onSelectionChanged != nullptr)
        onSelectionChanged (selection);
}

void MetroArrangeWorkspace::setEditorMode (MetroEditorMode mode)
{
    if (activeTrackIndex >= 0)
        viewModeByTrack[activeTrackIndex] = mode;
    showEditorMode (mode);
}

void MetroArrangeWorkspace::showEditorMode (MetroEditorMode mode)
{
    currentEditorMode = mode;

    // Both editors always hold the same clip selection (set in lock-step
    // in onArrangementSelectionChanged above) — swapping which one is
    // visible never loses anything, since neither owns note data of its
    // own; both read/write straight through to the same MidiClip.
    stepSequencer.setVisible (mode == MetroEditorMode::steps);
    pianoRoll.setVisible     (mode == MetroEditorMode::pianoRoll);

    updateTabButtonStates();
    resized();
}

void MetroArrangeWorkspace::updateTabButtonStates()
{
    stepsTabButton.setToggleState     (currentEditorMode == MetroEditorMode::steps,     juce::dontSendNotification);
    pianoRollTabButton.setToggleState (currentEditorMode == MetroEditorMode::pianoRoll, juce::dontSendNotification);
}

void MetroArrangeWorkspace::syncCollapsed (bool collapsed)
{
    // Collapse state is shared between the two editor modes — collapsing
    // one and switching tabs shouldn't un-collapse the other.
    stepSequencer.setCollapsed (collapsed);
    pianoRoll.setCollapsed (collapsed);
    resized();
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
        auto editorArea = area.removeFromBottom (kTabBarHeight + MetroStepSequencer::collapsedHeight());
        auto tabArea = editorArea.removeFromTop (kTabBarHeight);
        pianoRollTabButton.setBounds (tabArea.removeFromLeft (juce::jmax (80, tabArea.getWidth() / 6)));
        stepsTabButton.setBounds     (tabArea.removeFromLeft (juce::jmax (60, tabArea.getWidth() / 5)));

        stepSequencer.setBounds (editorArea);
        pianoRoll.setBounds (editorArea);

        area.removeFromBottom (kDividerHeight);
        arrangementView.setBounds (area);
        return;
    }

    const int totalHeight = juce::jmax (0, area.getHeight() - kDividerHeight);
    int editorHeight = (int) ((float) totalHeight * (1.0f - arrangerHeightFraction));
    editorHeight = juce::jlimit (kMinEditorHeight, juce::jmax (kMinEditorHeight, totalHeight - kMinArrangerHeight),
                                editorHeight);

    auto editorArea = area.removeFromBottom (editorHeight);
    auto tabArea = editorArea.removeFromTop (kTabBarHeight);
    pianoRollTabButton.setBounds (tabArea.removeFromLeft (juce::jmax (80, tabArea.getWidth() / 6)));
    stepsTabButton.setBounds     (tabArea.removeFromLeft (juce::jmax (60, tabArea.getWidth() / 5)));

    stepSequencer.setBounds (editorArea);
    pianoRoll.setBounds (editorArea);

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

    auto tabArea = tabBarBounds();
    g.setColour (Base::SurfaceAlt);
    g.fillRect (tabArea);
    g.setColour (Base::Border);
    g.drawHorizontalLine (tabArea.getBottom() - 1, (float) tabArea.getX(), (float) tabArea.getRight());
}

juce::Rectangle<int> MetroArrangeWorkspace::dividerBounds() const
{
    if (isEditorCollapsed())
        return { 0, getHeight() - (kTabBarHeight + MetroStepSequencer::collapsedHeight()) - kDividerHeight,
                getWidth(), kDividerHeight };
    return { 0, arrangementView.getBottom(), getWidth(), kDividerHeight };
}

juce::Rectangle<int> MetroArrangeWorkspace::tabBarBounds() const
{
    return { 0, dividerBounds().getBottom(), getWidth(), kTabBarHeight };
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
