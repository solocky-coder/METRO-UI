#include "MetroTransportBar.h"
#include "MetroColours.h"
#include "MetroMetrics.h"
#include "MetroTypography.h"
#include "../sequencer/SequencerEngine.h"

namespace dysekt::metro
{
MetroTransportBar::MetroTransportBar (SequencerEngine& sequencer) : engine (sequencer)
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

    for (auto* button : { &rewind, &play, &stop, &record })
        button->setBounds (area.removeFromLeft (MetroMetrics::grid * 10)
                               .reduced (MetroMetrics::separatorThickness));
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
