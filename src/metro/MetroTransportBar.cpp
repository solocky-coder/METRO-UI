#include "MetroTransportBar.h"
#include "MetroColours.h"
#include "MetroMetrics.h"
#include "MetroTypography.h"
#include "../sequencer/SequencerEngine.h"
#include "../sequencer/AbletonLink.h"

namespace dysekt::metro
{
MetroTransportBar::MetroTransportBar (SequencerEngine& sequencer, AbletonLink* link)
    : engine (sequencer), linkPtr (link)
{
    record.setClickingTogglesState (true);
    floatButton.setTooltip ("Detach the transport into a floating panel");

    for (auto* button : { &rewind, &play, &stop, &record, &floatButton })
        addAndMakeVisible (*button);

    rewind.onClick = [this] { engine.rewind(); };
    play.onClick = [this] { engine.play(); };
    stop.onClick = [this] { engine.stop(); };
    record.onClick = [this] { engine.setRecording (record.getToggleState()); };
    floatButton.onClick = [this] { if (onFloatRequested) onFloatRequested(); };

    if (linkPtr != nullptr)
    {
        linkButton.setClickingTogglesState (true);
        linkButton.setTooltip ("Toggle Ableton Link (right-click for options)");
        linkButton.onStateChange = [this] { if (linkPtr) linkPtr->setEnabled (linkButton.getToggleState()); };
        linkButton.onRightClick = [this] (const juce::MouseEvent&) { showLinkContextMenu(); };
        addAndMakeVisible (linkButton);
    }

    tempo.setEditable (true, true, false);
    tempo.setJustificationType (juce::Justification::centred);
    tempo.setFont (MetroTypography::body());
    tempo.setTooltip ("Tempo in beats per minute (20–999)");
    tempo.onTextChange = [this] { updateTempoFromEditor(); };
    addAndMakeVisible (tempo);

    startTimerHz (20);
}

void MetroTransportBar::paint (juce::Graphics& graphics)
{
    graphics.fillAll (Base::Surface);
    graphics.setColour (Base::Border);
    graphics.fillRect (getLocalBounds().removeFromBottom (MetroMetrics::separatorThickness));
}

void MetroTransportBar::resized()
{
    auto area = getLocalBounds().reduced (MetroMetrics::panelPadding,
                                         MetroMetrics::grid);
    tempo.setBounds (area.removeFromRight (MetroMetrics::grid * 14));

    floatButton.setBounds (area.removeFromRight (MetroMetrics::grid * 8)
                               .reduced (MetroMetrics::separatorThickness));

    if (linkPtr != nullptr)
        linkButton.setBounds (area.removeFromRight (MetroMetrics::grid * 8)
                                  .reduced (MetroMetrics::separatorThickness));

    for (auto* button : { &rewind, &play, &stop, &record })
        button->setBounds (area.removeFromLeft (MetroMetrics::grid * 10)
                               .reduced (MetroMetrics::separatorThickness));
}

void MetroTransportBar::timerCallback()
{
    play.setToggleState (engine.isPlaying(), juce::dontSendNotification);
    record.setToggleState (engine.isRecording(), juce::dontSendNotification);

    if (! tempo.isBeingEdited())
    {
        // Mirror Link's tempo while enabled — playback follows abletonLink's
        // BPM directly (see SequencerEngine::processBlock()), so the label
        // should too, instead of showing the stale local engine.getBpm().
        const float displayBpm = (linkPtr != nullptr && linkPtr->isEnabled())
                                    ? linkPtr->getBpm (engine.getBpm())
                                    : engine.getBpm();
        tempo.setText (juce::String (displayBpm, 1) + " BPM", juce::dontSendNotification);
    }

    if (linkPtr != nullptr)
    {
        const int peers = linkPtr->getPeerCount();
        linkButton.setButtonText (peers > 0 ? ("LINK " + juce::String (peers)) : "LINK");
        linkButton.setToggleState (linkPtr->isEnabled(), juce::dontSendNotification);
    }
}

void MetroTransportBar::updateTempoFromEditor()
{
    const auto bpm = tempo.getText().upToFirstOccurrenceOf (" ", false, false).getFloatValue();
    if (bpm >= 20.0f && bpm <= 999.0f)
        engine.setBpm (bpm);
}

void MetroTransportBar::showLinkContextMenu()
{
    // The one Link setting that isn't tempo sync
    // (SequencerEngine::setLinkFollowsTransport()) — a right-click menu on
    // LINK rather than a second cramped button. LINK's own left-click
    // toggle keeps meaning "tempo sync only".
    juce::PopupMenu m;
    m.addItem (1, "Follow Remote Start/Stop", true, engine.getLinkFollowsTransport());
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (linkButton),
        [this] (int result)
        {
            if (result == 1)
                engine.setLinkFollowsTransport (! engine.getLinkFollowsTransport());
        });
}
} // namespace dysekt::metro
