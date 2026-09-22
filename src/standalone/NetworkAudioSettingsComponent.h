#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "../network/MetroNetworkAudio.h"
#include "../network/NetworkAudioChannelState.h"
#include "../metro/MetroLookAndFeel.h"
#include "NetworkAudioProcessor.h"
#include "Utf8Text.h"

class NetworkAudioSettingsComponent : public juce::Component,
                                       private juce::Timer
{
public:
    static constexpr int kDirectUsbPort = 9000;

    explicit NetworkAudioSettingsComponent (juce::AudioDeviceManager& deviceManager,
                                             MetroNetworkAudio* networkAudioIn,
                                             bool showDeviceSelectorIn = false)
        : audioSelector (deviceManager, 0, 0, 1, 2, false, false, false, false),
          networkAudio (networkAudioIn),
          sourceModel (networkAudioIn)
    {
        showDeviceSelector = showDeviceSelectorIn;
        NetworkAudioProcessor::setActiveNetworkAudio (networkAudioIn);
        setLookAndFeel (&settingsLookAndFeel);

        constexpr int kDeviceCardHeight = 2 * 10 + 260;
        constexpr int kSectionGapOuter = 14;
        setSize (980, showDeviceSelector ? 980 : 980 - kDeviceCardHeight - kSectionGapOuter);

        if (showDeviceSelector)
            addAndMakeVisible (audioSelector);
        else
            audioSelector.setVisible (false);

        networkTitle.setText ("NETWORK AUDIO", juce::dontSendNotification);
        networkTitle.setFont (juce::Font (18.0f, juce::Font::bold));
        networkTitle.setColour (juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible (networkTitle);

        transportLabel.setText (utf8 ("AOO • Direct isolated USB or SonoBus / LAN"), juce::dontSendNotification);
        transportLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        addAndMakeVisible (transportLabel);
        directUsbButton.setClickingTogglesState (true);
        directUsbButton.setTooltip ("Starts/stops the AOO audio stream over the USB link. "
                                    "The link itself is set up on the Apple USB Share tab.");
        // Direct USB is an optional transport mode. Normal AOO/SonoBus networking
        // remains the default when Network Audio is enabled.
        directUsbButton.setToggleState (false, juce::dontSendNotification);
        updateDirectUsbButtonText();
        directUsbButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        directUsbButton.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        addAndMakeVisible (directUsbButton);

        // This is deliberately a TextButton rather than JUCE's ToggleButton.
        // It is the service master switch: ON starts the network backend and
        // OFF disconnects and stops it. The button text also makes the state
        // unambiguous without relying on a checkbox glyph.
        enableButton.setClickingTogglesState (true);
        enableButton.setToggleState (getNetworkAudioChannelState().enabled.load (std::memory_order_relaxed),
                                     juce::dontSendNotification);
        updateEnableButtonText();
        enableButton.onClick = [this]
        {
            setNetworkAudioEnabled (enableButton.getToggleState());
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
        setNetworkControlEnabled (networkEnabled);
        directUsbButton.setVisible (true);
        directUsbButton.onClick = [this]
        {
            if (networkAudio == nullptr || ! enableButton.getToggleState())
            {
                directUsbButton.setToggleState (false, juce::dontSendNotification);
                updateDirectUsbButtonText();
                updateDirectUsbInstructions (false);
                updateStatus ("Enable Network audio before using Direct USB");
                return;
            }

            updateDirectUsbButtonText();

#if DYSEKT_HAS_AOO
            networkAudio->disconnect();
            networkAudio->disconnectDirectPeers();
            networkAudio->stop();
            if (directUsbButton.getToggleState())
            {
                if (! networkAudio->startDirect (kDirectUsbPort))
                {
                    directUsbButton.setToggleState (false, juce::dontSendNotification);
                    updateDirectUsbButtonText();
                    updateDirectUsbInstructions (false);
                    updateStatus ("Could not start direct USB AOO backend");
                    return;
                }
                // Prime the reserved Apple USB peer addresses at the same UDP
                // port shown in the Direct USB instructions. Each isolated NCM
                // adapter uses its own /24 (.99 through .102). This is important
                // for AUv3-hosted SonoBus, which may wait for an incoming AOO
                // invite before emitting its first packet. Runtimes remain
                // invisible until a peer responds, so unused slots are harmless.
                const char* directPeers[] =
                {
                    "192.168.99.2",
                    "192.168.100.2",
                    "192.168.101.2",
                    "192.168.102.2"
                };
                int configuredPeers = 0;
                for (const auto* peer : directPeers)
                    if (networkAudio->connectDirectPeer (peer, kDirectUsbPort, 0))
                        ++configuredPeers;

                updateStatus (utf8 ("Direct USB — waiting for Apple device"));
                updateDirectUsbInstructions (true, configuredPeers);
            }
            else
            {
                updateDirectUsbInstructions (false);
                if (! networkAudio->start())
                    updateStatus ("Could not start AOO network backend");
                else
                    updateStatus (directUsbButton.getToggleState()
                              ? utf8 ("Ready — Direct USB audio")
                              : utf8 ("Ready — local Wi-Fi/LAN audio"));
            }
#endif
        };

        connectButton.onClick = [this] { connectClicked(); };
        addAndMakeVisible (connectButton);

        disconnectButton.onClick = [this] { disconnectClicked(); };
        addAndMakeVisible (disconnectButton);

        addAndMakeVisible (statusLabel);

        directUsbInfoButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xFF7FE0EC));
        directUsbInfoButton.onClick = [this] { showDirectUsbInfoPopup(); };
        directUsbInfoButton.setVisible (false);
        addAndMakeVisible (directUsbInfoButton);

        updateStatus (networkEnabled
                          ? (networkAudio != nullptr && networkAudio->isRunning()
                                 ? utf8 ("Connected — waiting for network sources")
                                 : utf8 ("Ready — local Wi-Fi/LAN audio"))
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

    // AUDIT: none of this panel's toggle-style controls (the two TextButton
    // toggles, plus the ToggleButton checkboxes) previously had any border —
    // MetroLookAndFeel::drawButtonBackground only ever fills a flat rectangle
    // (theme.button when off, theme.accent when on), with no outline drawn at
    // any point. That made an OFF toggle pixel-identical to a plain one-shot
    // action button (Connect / Join, Disconnect) — see the "Network audio:
    // OFF" confusion this session started from. Framing every toggle here,
    // drawn after children so it's never occluded, fixes that without
    // touching the shared LookAndFeel (and therefore without changing how
    // one-shot buttons look anywhere else in the app).
    void paintOverChildren (juce::Graphics& g) override
    {
        const auto& theme = getTheme();

        auto frame = [&] (const juce::Component& c, bool on)
        {
            if (! c.isVisible())
                return;
            g.setColour (on ? theme.accent : theme.separator);
            g.drawRoundedRectangle (c.getBounds().toFloat().expanded (3.0f), 3.0f, on ? 1.6f : 1.0f);
        };

        frame (enableButton,      enableButton.getToggleState());
        frame (directUsbButton,   directUsbButton.getToggleState());
        frame (muteButton,        muteButton.getToggleState());
        frame (soloButton,        soloButton.getToggleState());
        frame (recordArmButton,   recordArmButton.getToggleState());
        frame (monitorButton,     monitorButton.getToggleState());
        frame (publicGroupButton, publicGroupButton.getToggleState());
    }

    juce::TextButton& getDirectUsbButton() noexcept { return directUsbButton; }

    void resized() override
    {
        constexpr int kPad = 16;
        constexpr int kLabelColW = 100;
        constexpr int kValueColW = 70;
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

        if (showDeviceSelector)
        {
            auto deviceCardArea = area.removeFromTop (2 * kCardPad + 260);
            devicePanelBounds = deviceCardArea;
            audioSelector.setBounds (deviceCardArea.reduced (kCardPad));
            area.removeFromTop (kSectionGap);
        }
        else
        {
            devicePanelBounds = {};
        }

        auto channelCardArea = area.removeFromTop (2 * kCardPad + 18 + kRowGap + 4 * kRowH + 3 * kRowGap);
        channelPanelBounds = channelCardArea;
        auto channelInner = channelCardArea.reduced (kCardPad);
        channelTitle.setBounds (channelInner.removeFromTop (18));
        channelInner.removeFromTop (kRowGap);

        auto channelHeaderRow = channelInner.removeFromTop (kRowH);
        enableButton.setBounds (channelHeaderRow.removeFromLeft (220));
        channelHeaderRow.removeFromLeft (10);
        directUsbButton.setBounds (channelHeaderRow.removeFromLeft (140));
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

        constexpr int kToggleGap = 8; // clearance so each toggle's frame stays visually separate
        auto toggleRow = channelInner.removeFromTop (kRowH);
        toggleRow.removeFromLeft (kLabelColW);
        muteButton.setBounds (toggleRow.removeFromLeft (70));
        toggleRow.removeFromLeft (kToggleGap);
        soloButton.setBounds (toggleRow.removeFromLeft (70));
        toggleRow.removeFromLeft (kToggleGap);
        recordArmButton.setBounds (toggleRow.removeFromLeft (100));
        toggleRow.removeFromLeft (kToggleGap);
        monitorButton.setBounds (toggleRow.removeFromLeft (90));
        area.removeFromTop (kSectionGap);

        constexpr int kInstructionsH = kRowH; // single-line clickable prompt, not an inline text block

        auto connectionCardArea = area.removeFromTop (2 * kCardPad + 3 * kRowH + 2 * kRowGap
                                                        + (kRowGap + 6) + 30 + 8 + kRowH
                                                        + kRowGap + kInstructionsH);
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
        connectionInner.removeFromTop (kRowGap);

        auto instructionsRow = connectionInner.removeFromTop (kInstructionsH);
        instructionsRow.removeFromLeft (kLabelColW);
        directUsbInfoButton.setBounds (instructionsRow.removeFromLeft (280));
        area.removeFromTop (kSectionGap);

        sourcesPanelBounds = area;
        auto sourcesInner = area.reduced (kCardPad);
        sourcesLabel.setBounds (sourcesInner.removeFromTop (22));
        sourcesInner.removeFromTop (6);
        sourceList.setBounds (sourcesInner);
    }

private:
    // getAllAddresses() returns every interface JUCE can see, including
    // junk that's never a usable connection target: link-local autoconfig
    // (169.254.0.0/16 — what Windows assigns an adapter that got no DHCP
    // reply), multicast (224.0.0.0-239.255.255.255), and loopback. Left
    // unfiltered, the instructions turned into an 11-address wall of text
    // — that was the actual unreadability problem, not just the label's size.
    static bool isUsableDirectUsbAddress (const juce::IPAddress& addr)
    {
        if (addr.isNull() || addr.isIPv6)
            return false;
        if (addr == juce::IPAddress::local())
            return false;
        if (addr.address[0] == 169 && addr.address[1] == 254)
            return false;
        if (addr.address[0] >= 224)
            return false;
        return true;
    }

    // Builds the connect-info text and drives the small clickable prompt;
    // the actual readable display is the popup in showDirectUsbInfoPopup().
    //
    // configuredPeers is how many of the hardcoded candidate addresses
    // connectDirectPeer() accepted locally (valid IP string, backend
    // running). AOO direct mode is UDP and connectDirectPeer() performs no
    // handshake, so this count says nothing about whether a device is
    // actually plugged in or listening on the other end — don't word the
    // popup as if a peer has "responded".
    void updateDirectUsbInstructions (bool visible, int configuredPeers = 0)
    {
        juce::ignoreUnused (configuredPeers);

        if (! visible)
        {
            directUsbInfoButton.setVisible (false);
            directUsbInfoText = {};
            return;
        }

        // Direct USB uses one isolated subnet per Apple NCM adapter. The
        // Windows-side addresses are .1 and the Apple peers are .2 on the
        // reserved .99-.102 subnets.
        constexpr int directUsbPort = kDirectUsbPort;

        directUsbInfoText =
            "USB Direct peers:\n"
            "  192.168.99.2:" + juce::String (directUsbPort) + "\n"
            "  192.168.100.2:" + juce::String (directUsbPort) + "\n"
            "  192.168.101.2:" + juce::String (directUsbPort) + "\n"
            "  192.168.102.2:" + juce::String (directUsbPort)
            + "\n\nUse the matching peer address in SonoBus on each iPad.";

        directUsbInfoButton.setButtonText ("SonoBus connect info (4 USB peers)");
        directUsbInfoButton.setVisible (true);
    }

    void showDirectUsbInfoPopup()
    {
        if (directUsbInfoText.isEmpty())
            return;

        juce::AlertWindow::showMessageBoxAsync (
            juce::AlertWindow::InfoIcon,
            "Connect from SonoBus",
            "On the other device, open SonoBus -> Connect Direct and enter:\n\n" + directUsbInfoText,
            "OK",
            this);
    }

    void updateDirectUsbButtonText()
    {
        directUsbButton.setButtonText (directUsbButton.getToggleState() ? "USB AUDIO: ON" : "USB AUDIO: OFF");
        directUsbButton.setColour (juce::TextButton::buttonColourId,
                                   directUsbButton.getToggleState() ? juce::Colour (0xff168c9e)
                                                                    : juce::Colour (0xff2a2a30));
    }

    void updateEnableButtonText()
    {
        const bool enabled = enableButton.getToggleState();
        enableButton.setButtonText (enabled ? "Network audio: ON" : "Network audio: OFF");
    }

    void setNetworkControlEnabled (bool enabled)
    {
        getNetworkAudioChannelState().enabled.store (enabled, std::memory_order_relaxed);
        connectButton.setEnabled (enabled);
        directUsbButton.setEnabled (enabled);
        updateDirectUsbButtonText();
        disconnectButton.setEnabled (enabled);
        serverEditor.setEnabled (enabled);
        portEditor.setEnabled (enabled);
        userEditor.setEnabled (enabled);
        groupEditor.setEnabled (enabled);
        passwordEditor.setEnabled (enabled);
        publicGroupButton.setEnabled (enabled);
        monitorButton.setEnabled (enabled);
        gainSlider.setEnabled (enabled);
        panSlider.setEnabled (enabled);
        muteButton.setEnabled (enabled);
        soloButton.setEnabled (enabled);
        recordArmButton.setEnabled (enabled);
    }

    void setNetworkAudioEnabled (bool enabled)
    {
#if DYSEKT_HAS_AOO
        if (enabled)
        {
            if (networkAudio == nullptr)
            {
                enableButton.setToggleState (false, juce::dontSendNotification);
                updateEnableButtonText();
                setNetworkControlEnabled (false);
                updateStatus ("AOO network audio is unavailable");
                return;
            }

            const bool started = networkAudio->isRunning()
                               || (directUsbButton.getToggleState()
                                       ? networkAudio->startDirect (kDirectUsbPort)
                                       : networkAudio->start());
            if (! started)
            {
                enableButton.setToggleState (false, juce::dontSendNotification);
                updateEnableButtonText();
                setNetworkControlEnabled (false);
                updateDirectUsbInstructions (false);
                updateStatus ("Could not start AOO network backend");
                return;
            }

            setNetworkControlEnabled (true);
            updateEnableButtonText();
            if (directUsbButton.getToggleState())
                updateDirectUsbInstructions (true);
            updateStatus (utf8 ("Ready — local Wi-Fi/LAN audio"));
        }
        else
        {
            if (networkAudio != nullptr)
            {
                networkAudio->disconnect();
                networkAudio->disconnectDirectPeers();
                networkAudio->stop();
            }
            setNetworkControlEnabled (false);
            updateEnableButtonText();
            updateDirectUsbInstructions (false);
            updateStatus ("Disabled");
            refreshSources();
        }
#else
        juce::ignoreUnused (enabled);
        enableButton.setToggleState (false, juce::dontSendNotification);
        updateEnableButtonText();
        setNetworkControlEnabled (false);
        updateStatus ("AOO backend is not enabled in this build");
#endif
    }

    void timerCallback() override
    {
        refreshSources();
    }

    void connectClicked()
    {
#if DYSEKT_HAS_AOO
        if (networkAudio == nullptr)
        {
            updateStatus ("AOO network audio is unavailable");
            return;
        }

        if (directUsbButton.getToggleState())
        {
            if (networkAudio->isRunning()) networkAudio->disconnectDirectPeers();
            if (! networkAudio->isRunning() && ! networkAudio->startDirect (kDirectUsbPort))
            {
                updateDirectUsbInstructions (false);
                updateStatus ("Could not start direct USB AOO backend");
                return;
            }

            // Bootstrap all four reserved Apple USB peers immediately. These
            // runtimes are internal AOO handshake state only: they are NOT
            // published as sources until an iPad actually responds. This is
            // important when hosted SonoBus/AUv3 waits for the host invite and
            // does not emit the first packet itself.
            const char* directPeers[] =
            {
                "192.168.99.2",
                "192.168.100.2",
                "192.168.101.2",
                "192.168.102.2"
            };

            int configuredPeers = 0;
            for (const auto* peer : directPeers)
                if (networkAudio->connectDirectPeer (peer, kDirectUsbPort, 0))
                    ++configuredPeers;

            updateStatus (utf8 ("Direct USB backend started; bootstrapping Apple peers"));
            updateDirectUsbInstructions (true, configuredPeers);
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
                updateStatus (utf8 ("Connected — waiting for network sources"));
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
        {
            networkAudio->disconnect();
            networkAudio->disconnectDirectPeers();
        }
#endif
        updateDirectUsbInstructions (false);
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
        auto stateColour = juce::Colours::grey;
        if (text.startsWithIgnoreCase ("Connected"))
            stateColour = juce::Colour (0xFF4CAF50);
        else if (text.startsWithIgnoreCase ("Connecting") || text.startsWithIgnoreCase ("Ready"))
            stateColour = juce::Colour (0xFFE0A83D);
        else if (text.containsIgnoreCase ("could not") || text.containsIgnoreCase ("rejected")
                  || text.containsIgnoreCase ("unavailable"))
            stateColour = juce::Colour (0xFFE05A4C);

        statusLabel.setColour (juce::Label::textColourId, stateColour);
        statusLabel.setText ("STATUS  |  " + text, juce::dontSendNotification);
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
                if (source.online)
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
                if (! candidate.online)
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
            const auto details = s.group + "  |  "
                               + (s.channels > 0 && s.sampleRate > 0.0
                                      ? juce::String (s.channels) + " ch  |  " + juce::String (s.sampleRate, 0) + " Hz"
                                      : juce::String ("waiting for source format"))
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
    bool showDeviceSelector = false;

protected:
    // Exposed so a derived settings view (e.g. MetroNetworkAudioSettingsSelector)
    // can lay out an alternate full-page view using the same content region,
    // without duplicating this class's layout constants.
    juce::Rectangle<int> devicePanelBounds, channelPanelBounds, connectionPanelBounds, sourcesPanelBounds;

private:
    juce::AudioDeviceSelectorComponent audioSelector;
    MetroNetworkAudio* networkAudio = nullptr;

    juce::Label networkTitle;
    juce::Label transportLabel;
    juce::TextButton directUsbButton;
    juce::TextButton enableButton { "Network audio: OFF" };
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
    juce::TextButton directUsbInfoButton { "SonoBus connect info" };
    juce::String directUsbInfoText;
    juce::Label sourcesLabel;
    juce::ListBox sourceList;
    SourceListModel sourceModel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkAudioSettingsComponent)
};