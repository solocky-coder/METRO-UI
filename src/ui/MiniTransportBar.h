#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "../sequencer/SequencerEngine.h"
#include "../metro/MetroColours.h"
#include "TransportIconButton.h"
#include "DysektLookAndFeel.h"

//==============================================================================
//  MiniTransportBar
//
//  Compact four-button transport — STOP | PLAY | REC | LOOP, in that order —
//  that sits where the UNDO/REDO/PANIC/cog row used to live, beneath the
//  centre control frame. Drives the same SequencerEngine calls as
//  FloatingTransportBar (stop(), play(), setRecording(), setLooping()) and
//  mirrors the engine's state back on a timer so it stays in step with the
//  arranger's / floating transport's own buttons.
//==============================================================================
class MiniTransportBar final : public juce::Component,
                               private juce::Timer
{
public:
    explicit MiniTransportBar (SequencerEngine& seq) : engine (seq)
    {
        namespace M = dysekt::metro;

        stopButton  .configure (TransportIcons::Kind::Stop,   M::Accent::Orange,    "Stop");
        playButton  .configure (TransportIcons::Kind::Play,   M::Transport::Play,   "Play");
        recordButton.configure (TransportIcons::Kind::Record, M::Transport::Record, "Record");
        loopButton  .configure (TransportIcons::Kind::Loop,   M::Accent::Cyan,      "Toggle looping");

        // Same wiring as FloatingTransportBar: PLAY only requests playback (the
        // engine flips isPlaying() on the next audio block, and timerCallback()
        // confirms it); REC / LOOP are plain toggles.
        recordButton.setClickingTogglesState (true);
        loopButton  .setClickingTogglesState (true);

        stopButton  .onClick       = [this] { engine.stop(); };
        playButton  .onClick       = [this] { engine.play(); };
        recordButton.onStateChange = [this] { engine.setRecording (recordButton.getToggleState()); };
        loopButton  .onStateChange = [this] { engine.setLooping   (loopButton.getToggleState()); };

        for (auto* b : { &stopButton, &playButton, &recordButton, &loopButton })
        {
            b->setWantsKeyboardFocus (false);   // keep Space/keys going to the editor
            addAndMakeVisible (*b);
        }

        startTimerHz (20);
        timerCallback();
    }

    ~MiniTransportBar() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        const auto& theme = getTheme();
        g.setColour (theme.name == "metro" ? theme.waveformBg : theme.darkBar);
        g.fillRect (getLocalBounds());
        g.setColour (theme.separator);
        g.drawRect (getLocalBounds(), 1);
    }

    void resized() override
    {
        // Same inset/gap scaling HeaderBar used (design height kBtnBarH = 38px).
        const float sf  = (float) getHeight() / 38.0f;
        const int   pad = juce::roundToInt (3 * sf);
        const int   gap = juce::roundToInt (2 * sf);

        auto r = getLocalBounds().reduced (pad);
        const int btnW = r.getWidth() / 4;

        for (auto* b : { &stopButton, &playButton, &recordButton, &loopButton })
            b->setBounds (r.removeFromLeft (btnW).withTrimmedRight (gap));
        // (any 1-3 px of rounding remainder simply stays as right-hand padding)
    }

private:
    void timerCallback() override
    {
        const bool playing   = engine.isPlaying();
        const bool recording = engine.isRecording();
        const bool looping   = engine.isLooping();

        if (playButton.getToggleState()   != playing)   playButton  .setToggleState (playing,   juce::dontSendNotification);
        if (recordButton.getToggleState() != recording) recordButton.setToggleState (recording, juce::dontSendNotification);
        if (loopButton.getToggleState()   != looping)   loopButton  .setToggleState (looping,   juce::dontSendNotification);
    }

    SequencerEngine& engine;

    TransportIconButton stopButton, playButton, recordButton, loopButton;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MiniTransportBar)
};
