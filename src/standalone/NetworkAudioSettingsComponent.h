#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../network/MetroNetworkAudio.h"
#include "../network/NetworkAudioChannelState.h"
#include "../metro/MetroLookAndFeel.h"
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

        // This dialog is launched as its own top-level DialogWindow (see
        // MainWindow::showAudioSettings()), so it is NOT a child of
        // MetroStandaloneEditor and never inherits the app's MetroLookAndFeel
        // that gets set there. Without this call every ComboBox, ToggleButton,
        // Slider, TextEditor and the built-in AudioDeviceSelectorComponent all
        // fall back to JUCE's stock LookAndFeel_V4 — which is why this panel
        // has looked visually inconsistent with the rest of METRO.
        setLookAndFeel (&settingsLookAndFeel);

        // The channel strip needs enough horizontal room for every toggle and the level meter.
        // Height is the sum of the card layout computed in resized() below, plus a settings
        // dialog isn't resizable (see MainWindow::showAudioSettings()), so this needs to be right.
        setSize (980, 980);

        addAndMakeVisible (audioSelector);

        networkTitle.setText ("NETWORK AUDIO", juce::dontSendNotification);
        networkTitle.setFont (juce::Font (18.0f, juce::Font::bold));
        networkTitle.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (networkTitle);

        transportLabel.setText ("SonoBus / AOO", juce::dontSendNotification);
        transportLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible (transportLabel);

        enableButton.setButtonText ("Enable network audio");
        enableButton.setToggleState (getNetworkAudioChannelState().enabled.load (std::memory_order_relaxed),
                                     juce::dontSendNotification);
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
        channelTitle.setFont (juce::Font (17.0f, juce::Font::bold));
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
        // MetroLookAndFeel::drawLinearSlider centers its track on the slider's full local
        // bounds and ignores the textbox offset JUCE normally reserves space for, so a
        // slider with a built-in textbox here would render with the track drawn straight
        // through it. Disable the native textbox and show the value with our own label.
        gainSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        addAndMakeVisible (gainSlider);

        panSlider.setRange (-1.0, 1.0, 0.01);
        panSlider.setValue (getNetworkAudioChannelState().pan.load(), juce::dontSendNotification);
        panSlider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        addAndMakeVisible (panSlider);

        for (auto* label : { &gainValueLabel, &panValueLabel })
        {
            label->setColour (juce::Label::textColourId, juce::Colours::white);
            label->setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (*label);
        }

        gainSlider.onValueChange = [this]
        {
            getNetworkAudioChannelState().setGainDb ((float) gainSlider.getValue());
            gainValueLabel.setText (juce::String (gainSlider.getValue(), 1) + " dB", juce::dontSendNotification);
        };
        panSlider.onValueChange = [this]
        {
            getNetworkAudioChannelState().setPan ((float) panSlider.getValue());
            panValueLabel.setText (juce::String (panSlider.getValue(), 2) + "  L/R", juce::dontSendNotification);
        };
        gainValueLabel.setText (juce::String (gainSlider.getValue(), 1) + " dB", juce::dontSendNotification);
        panValueLabel.setText (juce::String (panSlider.getValue(), 2) + "  L/R", juce::dontSendNotification);

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

        const bool networkEnabled = enableButton.getToggleState();
        for (auto* editor : { &serverEditor, &portEditor, &userEditor, &groupEditor, &passwordEditor })
            editor->setEnabled (networkEnabled);
        publicGroupButton.setEnabled (networkEnabled);
        monitorButton.setEnabled (networkEnabled);

        connectButton.setEnabled (networkEnabled);
        connectButton.onClick = [this] { connectClicked(); };
        addAndMakeVisible (connectButton);

        disconnectButton.setEnabled (networkEnabled);
        disconnectButton.onClick = [this] { disconnectClicked(); };
        addAndMakeVisible (disconnectButton);

        addAndMakeVisible (statusLabel);
        updateStatus (networkEnabled
                          ? (networkAudio != nullptr && networkAudio->isRunning()
                                 ? "Connected — waiting for network sources"
                                 : "Ready — local Wi-Fi/LAN audio")
                          : "Disabled");

        sourcesLabel.setText ("Sources", juce::dontSendNotification);
        sourcesLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        sourcesLabel.setFont (juce::Font (15.0f, juce::Font::bold));
        addAndMakeVisible (sourcesLabel);

        sourceList.setModel (&sourceModel);
        sourceList.setColour (juce::ListBox::backgroundColourId, juce::Colour (0xFF111118));
        sourceList.setColour (juce::ListBox::outlineColourId, juce::Colour (0xFF30303A));
        sourceList.setOutlineThickness (1);
        sourceList.setRowHeight (64);
        addAndMakeVisible (sourceList);

        startTimerHz (10);
    }

    ~NetworkAudioSettingsComponent() override
    {
        stopTimer();
        sourceList.setModel (nullptr);
        setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xFF0D0D14));

        // Grouped cards, cached by resized(), so related controls read as one
        // unit instead of the whole dialog being one flat field of widgets.
        for (auto* panel : { &devicePanelBounds, &channelPanelBounds, &connectionPanelBounds, &sourcesPanelBounds })
        {
            if (panel->isEmpty())
                continue;
            g.setColour (juce::Colour (0xFF15151C));
            g.fillRoundedRectangle (panel->toFloat(), 8.0f);
            g.setColour (juce::Colour (0xFF2A2A34));
            g.drawRoundedRectangle (panel->toFloat(), 8.0f, 1.0f);
        }
    }

    void resized() override
    {
        constexpr int kPad = 16;
        constexpr int kLabelColW = 100;   // one shared label column for every row in this dialog
        constexpr int kValueColW = 70;    // gain/pan readout column
        constexpr int kRowH = 24;
        constexpr int kRowGap = 8;
        constexpr int kSectionGap = 14;
        constexpr int kCardPad = 10;

        auto area = getLocalBounds().reduced (kPad);

        auto headerRow = area.removeFromTop (26);
        networkTitle.setBounds (headerRow.removeFromLeft (300));
        area.removeFromTop (2);
        transportLabel.setBounds (area.removeFromTop (18));
        area.removeFromTop (kSectionGap);

        // --- Device card: audioSelector is a built-in JUCE component that lays
        // out its own rows (device type / output / channel list / sample rate /
        // buffer size). It needs ~260px to lay all of those rows out without its
        // last row (buffer size) crowding the bottom edge of the card.
        auto deviceCardArea = area.removeFromTop (2 * kCardPad + 260);
        devicePanelBounds = deviceCardArea;
        audioSelector.setBounds (deviceCardArea.reduced (kCardPad));
        area.removeFromTop (kSectionGap);

        // --- Network audio channel card ----------------------------------
        auto channelCardArea = area.removeFromTop (2 * kCardPad + 18 + kRowGap + 4 * kRowH + 3 * kRowGap);
        channelPanelBounds = channelCardArea;
        auto channelInner = channelCardArea.reduced (kCardPad);

        channelTitle.setBounds (channelInner.removeFromTop (18));
        channelInner.removeFromTop (kRowGap);

        auto channelHeaderRow = channelInner.removeFromTop (kRowH);
        enableButton.setBounds (channelHeaderRow.removeFromLeft (220));
        meterLabel.setBounds (channelHeaderRow);
        channelInner.removeFromTop (kRowGap);

        auto gainRow = channelInner.removeFromTop (kRowH);
        gainLabel.setBounds (gainRow.removeFromLeft (kLabelColW));
        gainValueLabel.setBounds (gainRow.removeFromRight (kValueColW));
        gainRow.removeFromRight (10);
        gainSlider.setBounds (gainRow);
        channelInner.removeFromTop (kRowGap);

        auto panRow = channelInner.removeFromTop (kRowH);
        panLabel.setBounds (panRow.removeFromLeft (kLabelColW));
        panValueLabel.setBounds (panRow.removeFromRight (kValueColW));
        panRow.removeFromRight (10);
        panSlider.setBounds (panRow);
        channelInner.removeFromTop (kRowGap);

        auto toggleRow = channelInner.removeFromTop (kRowH);
        toggleRow.removeFromLeft (kLabelColW);
        muteButton.setBounds (toggleRow.removeFromLeft (70));
        soloButton.setBounds (toggleRow.removeFromLeft (70));
        recordArmButton.setBounds (toggleRow.removeFromLeft (100));
        monitorButton.setBounds (toggleRow.removeFromLeft (90));
        area.removeFromTop (kSectionGap);

        // --- Connection card ----------------------------------------------
        auto connectionCardArea = area.removeFromTop (2 * kCardPad + 3 * kRowH + 2 * kRowGap
                                                        + (kRowGap + 6) + 30 + 8 + kRowH);
        connectionPanelBounds = connectionCardArea;
        auto connectionInner = connectionCardArea.reduced (kCardPad);

        auto row = connectionInner.removeFromTop (kRowH);
        serverLabel.setBounds (row.removeFromLeft (kLabelColW));
        portEditor.setBounds (row.removeFromRight (90));
        row.removeFromRight (8);
        portLabel.setBounds (row.removeFromRight (40));
        row.removeFromRight (8);
        serverEditor.setBounds (row);
        connectionInner.removeFromTop (kRowGap);

        row = connectionInner.removeFromTop (kRowH);
        userLabel.setBounds (row.removeFromLeft (kLabelColW));
        auto userField = row.removeFromLeft ((row.getWidth() - 70) / 2);
        userEditor.setBounds (userField);
        row.removeFromLeft (8);
        groupLabel.setBounds (row.removeFromLeft (54));
        groupEditor.setBounds (row);
        connectionInner.removeFromTop (kRowGap);

        row = connectionInner.removeFromTop (kRowH);
        passwordLabel.setBounds (row.removeFromLeft (kLabelColW));
        passwordEditor.setBounds (row.removeFromLeft (200));
        row.removeFromLeft (12);
        publicGroupButton.setBounds (row.removeFromLeft (120));
        connectionInner.removeFromTop (kRowGap + 6);

        auto buttonRow = connectionInner.removeFromTop (30);
        buttonRow.removeFromLeft (kLabelColW);
        connectButton.setBounds (buttonRow.removeFromLeft (150));
        buttonRow.removeFromLeft (10);
        disconnectButton.setBounds (buttonRow.removeFromLeft (110));
        connectionInner.removeFromTop (8);

        auto statusRow = connectionInner.removeFromTop (kRowH);
        statusRow.removeFromLeft (kLabelColW);
        statusLabel.setBounds (statusRow);
        area.removeFromTop (kSectionGap);

        // --- Sources card: takes whatever is left ---------------------
        sourcesPanelBounds = area;
        auto sourcesInner = area.reduced (kCardPad);
        sourcesLabel.setBounds (sourcesInner.removeFromTop (22));
        sourcesInner.removeFromTop (6);
        sourceList.setBounds (sourcesInner);
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
        // juce::Label only supports one text colour, so the state colour applies to the
        // whole "<dot>  message" string rather than just the dot.
        auto stateColour = juce::Colours::grey;
        if (text.startsWithIgnoreCase ("Connected"))
            stateColour = juce::Colour (0xFF4CAF50);
        else if (text.startsWithIgnoreCase ("Connecting") || text.startsWithIgnoreCase ("Ready"))
            stateColour = juce::Colour (0xFFE0A83D);
        else if (text.containsIgnoreCase ("could not") || text.containsIgnoreCase ("rejected")
                  || text.containsIgnoreCase ("unavailable"))
            stateColour = juce::Colour (0xFFE05A4C);

        statusLabel.setColour (juce::Label::textColourId, stateColour);
        statusLabel.setText (juce::String (juce::CharPointer_UTF8 ("\xE2\x97\x8F")) + "  " + text,
                              juce::dontSendNotification);
    }

    class SourceListModel : public juce::ListBoxModel
    {
    public:
        explicit SourceListModel (MetroNetworkAudio* owner) : owner (owner) {}

        int getNumRows() override
        {
#if DYSEKT_HAS_AOO
            if (owner == nullptr)
                return 0;

            const auto sources = owner->getSources();
            int count = 0;
            for (const auto& source : sources)
                if (source.online && source.channels > 0 && source.sampleRate > 0.0)
                    ++count;
            return count;
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
            const MetroNetworkAudio::SourceInfo* source = nullptr;
            int visibleRow = 0;
            for (const auto& candidate : sources)
            {
                if (! candidate.online || candidate.channels <= 0 || candidate.sampleRate <= 0.0)
                    continue;
                if (visibleRow++ == rowNumber)
                {
                    source = &candidate;
                    break;
                }
            }
            if (source == nullptr)
                return;

            if (rowIsSelected)
                g.fillAll (juce::Colour (0xFF242430));

            const auto& s = *source;
            const auto name = s.user.isNotEmpty() ? s.user : "Unknown source";
            const auto details = s.group + "  |  " + juce::String (s.channels)
                               + " ch  |  " + juce::String (s.sampleRate, 0) + " Hz"
                               + "  |  loss " + juce::String (s.packetLoss * 100.0f, 1) + "%";

            g.setColour (s.online ? juce::Colours::white : juce::Colours::grey);
            g.setFont (juce::Font (22.0f, juce::Font::bold));
            g.drawText (name, 10, 3, width - 20, height / 2, juce::Justification::centredLeft);
            g.setColour (juce::Colours::lightgrey);
            g.setFont (juce::Font (16.0f));
            g.drawText (details, 10, height / 2, width - 20, height / 2 - 2,
                        juce::Justification::centredLeft);
#else
            juce::ignoreUnused (rowNumber, g, width, height, rowIsSelected);
#endif
        }

    private:
        MetroNetworkAudio* owner = nullptr;
    };

    MetroLookAndFeel settingsLookAndFeel;

    // Cached by resized(), drawn by paint() as the grouped card backgrounds.
    juce::Rectangle<int> devicePanelBounds, channelPanelBounds, connectionPanelBounds, sourcesPanelBounds;

    juce::AudioDeviceSelectorComponent audioSelector;
    MetroNetworkAudio* networkAudio = nullptr;

    juce::Label networkTitle;
    juce::Label transportLabel;
    juce::ToggleButton enableButton;
    juce::Label channelTitle;
    juce::Label gainLabel;
    juce::Slider gainSlider;
    juce::Label gainValueLabel;
    juce::Label panLabel;
    juce::Slider panSlider;
    juce::Label panValueLabel;
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkAudioSettingsComponent)
};