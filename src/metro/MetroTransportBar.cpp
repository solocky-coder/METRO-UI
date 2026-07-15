#include "MetroTransportBar.h"
#include "MetroTheme.h"
#include "../sequencer/SequencerEngine.h"

namespace dysekt::metro
{
MetroTransportBar::MetroTransportBar (SequencerEngine& sequencer) : engine (sequencer)
{
    record.setClickingTogglesState (true);

    for (auto* button : { &rewind, &play, &stop, &record })
        addAndMakeVisible (*button);

    rewind.onClick = [this] { engine.rewind(); };
    play.onClick = [this] { engine.play(); };
    stop.onClick = [this] { engine.stop(); };
    record.onClick = [this] { engine.setRecording (record.getToggleState()); };

    tempo.setEditable (true, true, false);
    tempo.setJustificationType (juce::Justification::centred);
    tempo.setFont (MetroTheme::bodyFont());
    tempo.setTooltip ("Tempo in beats per minute (20–999)");
    tempo.onTextChange = [this] { updateTempoFromEditor(); };
    addAndMakeVisible (tempo);

    startTimerHz (20);
}

void MetroTransportBar::paint (juce::Graphics& graphics)
{
    graphics.fillAll (MetroTheme::Colours::panelBackground);
    graphics.setColour (MetroTheme::Colours::separator);
    graphics.fillRect (getLocalBounds().removeFromBottom (MetroTheme::Metrics::separatorThickness));
}

void MetroTransportBar::resized()
{
    auto area = getLocalBounds().reduced (MetroTheme::Metrics::panelPadding,
                                         MetroTheme::Metrics::grid);
    tempo.setBounds (area.removeFromRight (MetroTheme::Metrics::grid * 14));

    for (auto* button : { &rewind, &play, &stop, &record })
        button->setBounds (area.removeFromLeft (MetroTheme::Metrics::grid * 10)
                               .reduced (MetroTheme::Metrics::separatorThickness));
}

void MetroTransportBar::timerCallback()
{
    play.setToggleState (engine.isPlaying(), juce::dontSendNotification);
    record.setToggleState (engine.isRecording(), juce::dontSendNotification);

    if (! tempo.isBeingEdited())
        tempo.setText (juce::String (engine.getBpm(), 1) + " BPM", juce::dontSendNotification);
}

void MetroTransportBar::updateTempoFromEditor()
{
    const auto bpm = tempo.getText().upToFirstOccurrenceOf (" ", false, false).getFloatValue();
    if (bpm >= 20.0f && bpm <= 999.0f)
        engine.setBpm (bpm);
}
} // namespace dysekt::metro
