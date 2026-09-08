#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../network/MetroNetworkAudio.h"
#include "../network/NetworkAudioChannelState.h"
#include "NetworkAudioProcessor.h"

class NetworkAudioSettingsComponent : public juce::Component,
                                       private juce::Timer
{
public:
    explicit NetworkAudioSettingsComponent (juce::AudioDeviceManager& deviceManager,
                                             MetroNetworkAudio* networkAudio)
        : audioSelector (deviceManager, 0, 0, 1, 2, false, false, false, false),
          networkAudio (networkAudio),
          sourceModel (networkAudio)
    {
        NetworkAudioProcessor::setActiveNetworkAudio (networkAudio);
        setSize (680, 760);

        addAndMakeVisible (audioSelector);

        networkTitle.setText ("NETWORK AUDIO", juce::dontSendNotification);
        networkTitle.setFont (juce::Font (18.0f, juce::Font::bold));
        networkTitle.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (networkTitle);

        transportLabel.setText ("SonoBus / AOO", juce::dontSendNotification);
        transportLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible (transportLabel);

        enableButton.setButtonText ("Enable network audio");
        enableButton.onClick = [this, networkAudio]
        {
            const bool enabled = enableButton.getToggleState();
            getNetworkAudioChannelState().enabled.store (enabled, std::memory_order_relaxed);
            connectButton.setEnabled (enabled);
            disconnectButton.setEnabled (enabled);
            serverEditor.setEnabled (enabled);
            portEditor.setEnabled (enabled);
            userEditor.setEnabled (enabled);
            groupEditor.setEnabled (enabled);
            passwordEditor.setEnabled (enabled);
            publicGroupButton.setEnabled (enabled);
            monitorButton.setEnabled (enabled);

            if (! enabled)
            {
#if DYSEKT_HAS_AOO
                if (networkAudio != nullptr)
                    networkAudio->disconnect();
#endif
                updateStatus ("Disabled");
            }
            else
            {
#if DYSEKT_HAS_AOO
                updateStatus ("Ready — local Wi-Fi/LAN audio");
#else
                updateStatus ("AOO backend is not enabled in this build");
#endif
            }
        };
        addAndMakeVisible (enableButton);

        channelTitle.setText ("NETWORK AUDIO CHANNEL", juce::dontSendNotification);
        channelTitle.setFont (juce::Font (14.0f, juce::Font::bold));
        channelTitle.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (channelTitle);

        gainLabel.setText ("Gain", juce::dontSendNotification);
        panLabel.setText ("Pan", juce::dontSendNotification);
        for (auto* label : { &gainLabel, &panLabel })
        {
            label->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
            addAndMakeVisible (*label);
        }

        gainSlider.setRange (-100.0, 24.0, 0.1);
        gainSlider.setValue (getNetworkAudioChannelState().gainDb.load(), juce::dontSendNotification);
        gainSlider.setTextValueSuffix (" dB");
        gainSlider.onValueChange = []
        {
            getNetworkAudioChannelState().setGainDb ((float) gainSliderStaticValue());
        };
        addAndMakeVisible (gainSlider);

        panSlider.setRange (-1.0, 1.0, 0.01);
        panSlider.setValue (getNetworkAudioChannelState().pan.load(), juce::dontSendNotification);
        panSlider.setTextValueSuffix ("  L/R");
        panSlider.onValueChange = []
        {
            getNetworkAudioChannelState().setPan ((float) panSliderStaticValue());
        };
        addAndMakeVisible (panSlider);

        // Use local callbacks rather than a global routing object.  These
        // values are atomics, so the audio thread can read them safely.
        gainSlider.onValueChange = [this]
        {
            getNetworkAudioChannelState().setGainDb ((float) gainSlider.getValue());
        };
        panSlider.onValueChange = [this]
        {
            getNetworkAudioChannelState().setPan ((float) panSlider.getValue());
        };

        muteButton.setButtonText ("Mute");
        muteButton.setToggleState (getNetworkAudioChannelState().muted.load(), juce::dontSendNotification);
        muteButton.onClick = [this]
        {
            getNetworkAudioChannelState().muted.store (muteButton.getToggleState(), std::memory_order_relaxed);
        };
        addAndMakeVisible (muteButton);

        soloButton.setButtonText ("Solo");
        soloButton.setToggleState (getNetworkAudioChannelState().solo.load(), juce::dontSendNotification);
        soloButton.onClick = [this]
        {
            getNetworkAudioChannelState().solo.store (soloButton.getToggleState(), std::memory_order_relaxed);
        };
        addAndMakeVisible (soloButton);

        recordArmButton.setButtonText ("Record Arm");
        recordArmButton.setToggleState (getNetworkAudioChannelState().recordArm.load(), juce::dontSendNotification);
        recordArmButton.onClick = [this]
        {
            getNetworkAudioChannelState().recordArm.store (recordArmButton.getToggleState(), std::memory_order_relaxed);
        };
        addAndMakeVisible (recordArmButton);

        monitorButton.setButtonText ("Monitor");
        monitorButton.setToggleState (getNetworkAudioChannelState().monitor.load(), juce::dontSendNotification);
        monitorButton.onClick = [this]
        {
            getNetworkAudioChannelState().monitor.store (monitorButton.getToggleState(), std::memory_order_relaxed);
        };
        addAndMakeVisible (monitorButton);

        meterLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible (meterLabel);

        serverLabel.setText ("Server", juce::dontSendNotification);
        portLabel.setText ("Port", juce::dontSendNotification);
        userLabel.setText ("User", juce::dontSendNotification);
        groupLabel.setText ("Group", juce::dontSendNotification);
        passwordLabel.setText ("Password", juce::dontSendNotification);
        for (auto* label : { &serverLabel, &portLabel, &userLabel, &groupLabel, &passwordLabel })
        {
            label->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
            addAndMakeVisible (*label);
        }

        serverEditor.setText (MetroNetworkAudio::defaultServer, false);
        portEditor.setText (juce::String (MetroNetworkAudio::defaultServerPort), false);
        userEditor.setText ("METRO", false);
        groupEditor.setText ("studio", false);
        passwordEditor.setPasswordCharacter ('*');

        for (auto* editor : { &serverEditor, &portEditor, &userEditor, &groupEditor, &passwordEditor })
        {
            editor->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xFF191922));
            editor->setColour (juce::TextEditor::textColourId, juce::Colours::white);
            editor->setEnabled (false);
            addAndMakeVisible (*editor);
        }

        publicGroupButton.setButtonText ("Public group");
        publicGroupButton.setEnabled (false);
        addAndMakeVisible (publicGroupButton);

        connectButton.setEnabled (false);
        connectButton.onClick = [this] { connectClicked(); };
        addAndMakeVisible (connectButton);

        disconnectButton.setEnabled (false);
        disconnectButton.onClick = [this] { disconnectClicked(); };
        addAndMakeVisible (disconnectButton);

        statusLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        statusLabel.setText ("Disabled", juce::dontSendNotification);
        addAndMakeVisible (statusLabel);

        sourcesLabel.setText ("Sources", juce::dontSendNotification);
        sourcesLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        sourcesLabel.setFont (juce::Font (15.0f, juce::Font::bold));
        addAndMakeVisible (sourcesLabel);

        sourceList.setModel (&sourceModel);
        sourceList.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xFF111118));
        sourceList.setColour (juce::ListBox::outlineColourId, juce::Colour (0xFF30303A));
        sourceList.setOutlineThickness (1);
        addAndMakeVisible (sourceList);

        startTimerHz (10);
    }

    ~NetworkAudioSettingsComponent() override
    {
        stopTimer();
        sourceList.setModel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFF0D0D14));
        g.setColour (juce::Colour (0xFF2A2A34));
        g.drawHorizontalLine (106, 16.0f, (float) getWidth() - 16.0f);
        g.drawHorizontalLine (438, 16.0f, (float) getWidth() - 16.0f);
        g.drawHorizontalLine (570, 16.0f, (float) getWidth() - 16.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (16);
        networkTitle.setBounds (area.removeFromTop (28));
        transportLabel.setBounds (area.removeFromTop (22));
        area.removeFromTop (8);

        audioSelector.setBounds (area.removeFromTop (250));
        area.removeFromTop (12);

        enableButton.setBounds (area.removeFromTop (28));
        area.removeFromTop (6);

        channelTitle.setBounds (area.removeFromTop (24));
        auto channelRow = area.removeFromTop (28);
        gainLabel.setBounds (channelRow.removeFromLeft (40));
        gainSlider.setBounds (channelRow.removeFromLeft (230));
        panLabel.setBounds (channelRow.removeFromLeft (34));
        panSlider.setBounds (channelRow.removeFromLeft (180));
        muteButton.setBounds (channelRow.removeFromLeft (72));
        soloButton.setBounds (channelRow.removeFromLeft (72));
        recordArmButton.setBounds (channelRow.removeFromLeft (100));
        monitorButton.setBounds (channelRow.removeFromLeft (82));
        meterLabel.setBounds (channelRow);
        area.removeFromTop (8);

        auto row = area.removeFromTop (28);
        serverLabel.setBounds (row.removeFromLeft (62));
        serverEditor.setBounds (row.removeFromLeft (250));
        portLabel.setBounds (row.removeFromLeft (42));
        portEditor.setBounds (row.removeFromLeft (80));

        row = area.removeFromTop (28);
        userLabel.setBounds (row.removeFromLeft (62));
        userEditor.setBounds (row.removeFromLeft (150));
        groupLabel.setBounds (row.removeFromLeft (58));
        groupEditor.setBounds (row.removeFromLeft (150));

        row = area.removeFromTop (28);
        passwordLabel.setBounds (row.removeFromLeft (62));
        passwordEditor.setBounds (row.removeFromLeft (150));
        publicGroupButton.setBounds (row.removeFromLeft (130));
        connectButton.setBounds (row.removeFromLeft (150));
        disconnectButton.setBounds (row.removeFromLeft (110));

        statusLabel.setBounds (area.removeFromTop (30));
        area.removeFromTop (8);
        sourcesLabel.setBounds (area.removeFromTop (26));
        sourceList.setBounds (area);
    }

private:
    void timerCallback() override
    {
        refreshSources();
        const auto& state = getNetworkAudioChannelState();
        const float left = state.peakL.load (std::memory_order_relaxed);
        const float right = state.peakR.load (std::memory_order_relaxed);
        meterLabel.setText ("L " + juce::String (juce::Decibels::gainToDecibels (juce::jmax (left, 0.00001f)), 1)
                            + " dB   R " + juce::String (juce::Decibels::gainToDecibels (juce::jmax (right, 0.00001f)), 1) + " dB",
                            juce::dontSendNotification);
    }

    void connectClicked()
    {
#if DYSEKT_HAS_AOO
        if (networkAudio == nullptr)
        {
            updateStatus ("AOO network audio is unavailable");
            return;
        }

        if (! networkAudio->isRunning())
        {
            if (! networkAudio->start())
            {
                updateStatus ("Could not start AOO network backend");
                return;
            }
        }

        const int port = juce::jmax (1, portEditor.getText().getIntValue());
        if (! networkAudio->connectToServer (serverEditor.getText(), port,
                                             userEditor.getText(), passwordEditor.getText()))
        {
            updateStatus ("AOO connection request was rejected");
            return;
        }

        updateStatus ("Connecting to AOO server...");

        const auto group = groupEditor.getText();
        const auto password = passwordEditor.getText();
        const bool isPublic = publicGroupButton.getToggleState();
        juce::Timer::callAfterDelay (1500, [this, group, password, isPublic]
        {
            if (networkAudio == nullptr || ! networkAudio->isRunning())
                return;

            if (networkAudio->joinGroup (group, password, isPublic))
                updateStatus ("Connected — waiting for network sources");
            else
                updateStatus ("Connected, but group join could not be queued");
        });
#else
        juce::ignoreUnused (networkAudio);
        updateStatus ("AOO backend is not enabled in this build");
#endif
    }

    void disconnectClicked()
    {
#if DYSEKT_HAS_AOO
        if (networkAudio != nullptr)
            networkAudio->disconnect();
#endif
        updateStatus ("Disconnected");
        refreshSources();
    }

    void refreshSources()
    {
        sourceList.updateContent();
        sourceList.repaint();
    }

    void updateStatus (const juce::String& text)
    {
        statusLabel.setText (text, juce::dontSendNotification);
    }

    class SourceListModel : public juce::ListBoxModel
    {
    public:
        explicit SourceListModel (MetroNetworkAudio* owner) : owner (owner) {}

        int getNumRows() override
        {
#if DYSEKT_HAS_AOO
            return owner != nullptr ? (int) owner->getSources().size() : 0;
#else
            return 0;
#endif
        }

        void paintListBoxItem (int rowNumber, juce::Graphics& g,
                               int width, int height, bool rowIsSelected) override
        {
#if DYSEKT_HAS_AOO
            if (owner == nullptr)
                return;
            const auto sources = owner->getSources();
            if (! juce::isPositiveAndBelow (rowNumber, (int) sources.size()))
                return;

            if (rowIsSelected)
                g.fillAll (juce::Colour (0xFF242430));

            const auto& s = sources[(size_t) rowNumber];
            const auto name = s.user.isNotEmpty() ? s.user : "Unknown source";
            const auto details = s.group + "  •  " + juce::String (s.channels)
                               + " ch  •  " + juce::String (s.sampleRate, 0) + " Hz"
                               + "  •  loss " + juce::String (s.packetLoss * 100.0f, 1) + "%";

            g.setColour (s.online ? juce::Colours::white : juce::Colours::grey);
            g.setFont (juce::Font (14.0f, juce::Font::bold));
            g.drawText (name, 10, 3, width - 20, height / 2, juce::Justification::centredLeft);
            g.setColour (juce::Colours::grey);
            g.setFont (juce::Font (11.0f));
            g.drawText (details, 10, height / 2, width - 20, height / 2 - 2,
                        juce::Justification::centredLeft);
#else
            juce::ignoreUnused (rowNumber, g, width, height, rowIsSelected);
#endif
        }

    private:
        MetroNetworkAudio* owner = nullptr;
    };

    juce::AudioDeviceSelectorComponent audioSelector;
    MetroNetworkAudio* networkAudio = nullptr;

    juce::Label networkTitle;
    juce::Label transportLabel;
    juce::ToggleButton enableButton;
    juce::Label channelTitle;
    juce::Label gainLabel;
    juce::Slider gainSlider;
    juce::Label panLabel;
    juce::Slider panSlider;
    juce::ToggleButton muteButton;
    juce::ToggleButton soloButton;
    juce::ToggleButton recordArmButton;
    juce::ToggleButton monitorButton;
    juce::Label meterLabel;
    juce::Label serverLabel;
    juce::TextEditor serverEditor;
    juce::Label portLabel;
    juce::TextEditor portEditor;
    juce::Label userLabel;
    juce::TextEditor userEditor;
    juce::Label groupLabel;
    juce::TextEditor groupEditor;
    juce::Label passwordLabel;
    juce::TextEditor passwordEditor;
    juce::ToggleButton publicGroupButton;
    juce::TextButton connectButton { "Connect / Join" };
    juce::TextButton disconnectButton { "Disconnect" };
    juce::Label statusLabel;
    juce::Label sourcesLabel;
    juce::ListBox sourceList;
    SourceListModel sourceModel;

    static double gainSliderStaticValue() { return 0.0; }
    static double panSliderStaticValue() { return 0.0; }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkAudioSettingsComponent)
};