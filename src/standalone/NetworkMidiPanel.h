#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <cmath>
#include <initializer_list>
#include "NetworkMidiSettings.h"
#include "../network/RtpMidiSession.h"
#include "../network/NetworkMidiManager.h"

// "USB MIDI" page of the Network Audio panel: sends DYSEKT's live incoming MIDI to
// an iPhone / iPad over the direct Apple USB link (wired RTP-MIDI, no Wi-Fi).
//
// The panel owns no session - it controls and reflects manager.getSession (deviceId), so
// closing and reopening the Network Audio dialog never interrupts a running session.
class NetworkMidiPanel final : public juce::Component,
                               private juce::Timer
{
public:
    explicit NetworkMidiPanel (NetworkMidiManager& managerToUse, int deviceIdToUse = 1)
        : manager (managerToUse), deviceId (deviceIdToUse)
    {
        auto values = NetworkMidiSettings::load();

        // A session that is already running (started at app launch or from an earlier
        // visit to this page) is the source of truth for what the page shows.
        auto& session = RtpMidiSession::shared();
        if (session.isRunning())
        {
            values.peer = session.getPeerAddress();
            values.port = session.getPeerPort();
            values.channelMask = session.getChannelMask();
            values.forwardRealtime = session.getForwardRealtime();
            values.enabled = true;
        }

        title.setText ("USB Network MIDI", juce::dontSendNotification);
        title.setFont (juce::Font (28.0f, juce::Font::bold));
        subtitle.setText ("Send DYSEKT's live incoming MIDI to an iPhone / iPad over the direct USB link - "
                          "wired network MIDI (RTP-MIDI), no Wi-Fi or router",
                          juce::dontSendNotification);
        subtitle.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

        for (auto* label : { &connectionCaption, &filterCaption, &helpCaption })
        {
            label->setFont (juce::Font (17.0f, juce::Font::bold));
            label->setColour (juce::Label::textColourId, juce::Colours::white);
        }
        connectionCaption.setText ("CONNECTION", juce::dontSendNotification);
        filterCaption.setText ("WHAT TO SEND", juce::dontSendNotification);
        helpCaption.setText ("HOW TO CONNECT", juce::dontSendNotification);

        enableButton.setClickingTogglesState (true);
        enableButton.setToggleState (values.enabled, juce::dontSendNotification);
        enableButton.setTooltip ("Send every live MIDI message DYSEKT receives to the device over the USB link");
        enableButton.onClick = [this] { onEnableClicked(); };
        updateEnableButtonText();

        deviceCaption.setText ("Device IP", juce::dontSendNotification);
        portCaption.setText ("Port", juce::dontSendNotification);
        for (auto* label : { &deviceCaption, &portCaption, &statusLabel, &infoLabel, &channelsCaption })
            label->setColour (juce::Label::textColourId, juce::Colours::lightgrey);

        deviceCombo.setEditableText (true);
        deviceCombo.addItem ("192.168.99.2", 1);
        deviceCombo.addItem ("192.168.100.2", 2);
        deviceCombo.addItem ("192.168.101.2", 3);
        deviceCombo.addItem ("192.168.102.2", 4);
        deviceCombo.setText (values.peer, juce::dontSendNotification);
        deviceCombo.setTooltip ("The iPhone / iPad's address on the USB link (USB slot 1 is 192.168.99.2)");
        deviceCombo.onChange = [this] { onTargetEdited(); };

        portEditor.setInputRestrictions (5, "0123456789");
        portEditor.setText (juce::String (values.port), false);
        portEditor.setTooltip ("Network MIDI control port of the iPad app's session (default 5004)");
        portEditor.onReturnKey = [this] { onTargetEdited(); };
        portEditor.onFocusLost = [this] { onTargetEdited(); };

        channelsCaption.setText ("Channels", juce::dontSendNotification);

        for (int ch = 0; ch < 16; ++ch)
        {
            auto& button = channelButtons[(size_t) ch];
            button.setButtonText (juce::String (ch + 1));
            button.setClickingTogglesState (true);
            button.setToggleState ((values.channelMask & (1u << ch)) != 0, juce::dontSendNotification);
            button.setTooltip ("Forward MIDI channel " + juce::String (ch + 1));
            button.onClick = [this] { onFilterEdited(); };
            addAndMakeVisible (button);
        }

        allButton.setButtonText ("All");
        allButton.onClick = [this] { setAllChannels (true); };
        noneButton.setButtonText ("None");
        noneButton.onClick = [this] { setAllChannels (false); };

        realtimeToggle.setButtonText ("Also send MIDI clock, start, continue and stop");
        realtimeToggle.setToggleState (values.forwardRealtime, juce::dontSendNotification);
        realtimeToggle.setTooltip ("Off by default: a hardware clock can flood the link. Turn on to let iPad apps follow it.");
        realtimeToggle.onClick = [this] { onFilterEdited(); };

        helpText.setText ("1.  Plug the iPad in and start Apple USB Share (the link shows 192.168.99.1 -> 192.168.99.2).\n"
                          "2.  In the iPad app, turn on a network MIDI session and allow connections (\"Anyone\" if it asks).\n"
                          "3.  Switch \"Send live MIDI to device\" on. DYSEKT connects to the device at the address above.\n\n"
                          "Everything arriving on DYSEKT's enabled MIDI inputs is forwarded, filtered by the channels you pick. "
                          "SysEx is not forwarded. Turn the iPad's Wi-Fi off while testing so the cable is the only path.",
                          juce::dontSendNotification);
        helpText.setJustificationType (juce::Justification::topLeft);
        helpText.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        helpText.setFont (juce::Font (15.0f));

        statusLabel.setFont (juce::Font (18.0f, juce::Font::bold));
        statusLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        infoLabel.setFont (juce::Font (15.0f));

        for (auto* c : std::initializer_list<juce::Component*> {
                 &title, &subtitle, &connectionCaption, &filterCaption, &helpCaption, &enableButton,
                 &deviceCaption, &deviceCombo, &portCaption, &portEditor, &statusLabel, &infoLabel,
                 &channelsCaption, &allButton, &noneButton, &realtimeToggle, &helpText })
            addAndMakeVisible (c);

        // Apply the filters to the live session (does not start it).
        applyFiltersToManager (currentValues());

        lastSent = RtpMidiSession::shared().getMessagesSent();
        refresh();
        startTimerHz (10);
    }

    ~NetworkMidiPanel() override = default;

    void paint (juce::Graphics& g) override
    {
        for (const auto& card : { connectionCard, filterCard, helpCard })
        {
            g.setColour (juce::Colour (0xff1c1c25));
            g.fillRoundedRectangle (card.toFloat(), 8.0f);
            g.setColour (juce::Colour (0xff2a2a34));
            g.drawRoundedRectangle (card.toFloat(), 8.0f, 1.0f);
        }

        // Status dot
        g.setColour (statusColour);
        g.fillEllipse (dotBounds.toFloat());

        // Activity LED: lights on every tick in which messages were sent
        g.setColour (juce::Colour (0xff3a3a46));
        g.fillEllipse (ledBounds.toFloat());
        if (ledBrightness > 0.01f)
        {
            g.setColour (juce::Colour (0xff35e06f).withAlpha (juce::jlimit (0.0f, 1.0f, ledBrightness)));
            g.fillEllipse (ledBounds.toFloat());
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (24);

        title.setBounds (area.removeFromTop (38));
        subtitle.setBounds (area.removeFromTop (24));
        area.removeFromTop (14);

        // ---- Connection card ------------------------------------------------
        connectionCard = area.removeFromTop (178);
        {
            auto inner = connectionCard.reduced (18, 14);
            connectionCaption.setBounds (inner.removeFromTop (24));
            inner.removeFromTop (6);

            auto row1 = inner.removeFromTop (36);
            enableButton.setBounds (row1.removeFromLeft (270));
            row1.removeFromLeft (16);
            dotBounds = row1.removeFromLeft (16).withSizeKeepingCentre (14, 14);
            row1.removeFromLeft (8);
            ledBounds = row1.removeFromRight (16).withSizeKeepingCentre (14, 14);
            row1.removeFromRight (8);
            statusLabel.setBounds (row1);

            inner.removeFromTop (10);
            auto row2 = inner.removeFromTop (32);
            deviceCaption.setBounds (row2.removeFromLeft (84));
            deviceCombo.setBounds (row2.removeFromLeft (190));
            row2.removeFromLeft (20);
            portCaption.setBounds (row2.removeFromLeft (44));
            portEditor.setBounds (row2.removeFromLeft (84));

            inner.removeFromTop (8);
            infoLabel.setBounds (inner.removeFromTop (26));
        }

        area.removeFromTop (14);

        // ---- Filter card ----------------------------------------------------
        filterCard = area.removeFromTop (204);
        {
            auto inner = filterCard.reduced (18, 14);
            filterCaption.setBounds (inner.removeFromTop (24));
            inner.removeFromTop (6);
            channelsCaption.setBounds (inner.removeFromTop (22));
            inner.removeFromTop (4);

            const int gap = 6;
            const int buttonW = juce::jlimit (34, 56, (inner.getWidth() - 7 * gap) / 8);

            for (int row = 0; row < 2; ++row)
            {
                auto line = inner.removeFromTop (32);
                for (int col = 0; col < 8; ++col)
                {
                    channelButtons[(size_t) (row * 8 + col)].setBounds (line.removeFromLeft (buttonW));
                    line.removeFromLeft (gap);
                }
                inner.removeFromTop (gap);
            }

            inner.removeFromTop (4);
            auto line = inner.removeFromTop (32);
            allButton.setBounds (line.removeFromLeft (70));
            line.removeFromLeft (gap);
            noneButton.setBounds (line.removeFromLeft (70));
            line.removeFromLeft (20);
            realtimeToggle.setBounds (line);
        }

        area.removeFromTop (14);

        // ---- Help card (takes the remaining height) -------------------------
        helpCard = area;
        {
            auto inner = helpCard.reduced (18, 14);
            helpCaption.setBounds (inner.removeFromTop (24));
            inner.removeFromTop (6);
            helpText.setBounds (inner);
        }
    }

private:
    //==========================================================================
    NetworkMidiSettings::Values currentValues() const
    {
        NetworkMidiSettings::Values v;
        v.peer = deviceCombo.getText().trim();
        v.port = juce::jlimit (1, 65534, portEditor.getText().getIntValue() > 0 ? portEditor.getText().getIntValue() : 5004);
        v.enabled = enableButton.getToggleState();
        v.forwardRealtime = realtimeToggle.getToggleState();

        uint32_t mask = 0;
        for (int ch = 0; ch < 16; ++ch)
            if (channelButtons[(size_t) ch].getToggleState())
                mask |= (1u << ch);
        v.channelMask = mask;
        return v;
    }

    void applyFiltersToManager (const NetworkMidiSettings::Values& v)
    {
        manager.configureDevice (deviceId, v.peer, "Network MIDI " + juce::String (deviceId));
        manager.setChannelMask (deviceId, v.channelMask);
        manager.setForwardRealtime (deviceId, v.forwardRealtime);
    }

    void persist() const
    {
        NetworkMidiSettings::save (currentValues());
    }

    void updateEnableButtonText()
    {
        enableButton.setButtonText (enableButton.getToggleState() ? "Send live MIDI to device: ON"
                                                                  : "Send live MIDI to device: OFF");
    }

    void startSessionFromFields()
    {
        const auto v = currentValues();

        if (! RtpMidiSession::isValidIPv4 (v.peer))
        {
            RtpMidiSession::shared().stop();
            enableButton.setToggleState (false, juce::dontSendNotification);
            updateEnableButtonText();
            persist();
            invalidAddressNote = true;
            refresh();
            return;
        }

        invalidAddressNote = false;
        applyFiltersToManager (v);
        RtpMidiSession::shared().start (v.peer, v.port);
    }

    void onEnableClicked()
    {
        updateEnableButtonText();

        if (enableButton.getToggleState())
            startSessionFromFields();
        else
            RtpMidiSession::shared().stop();

        persist();
        refresh();
    }

    // Device address or port edited: restart a running session toward the new target.
    void onTargetEdited()
    {
        persist();

        if (! enableButton.getToggleState())
            return;

        const auto v = currentValues();
        auto& session = RtpMidiSession::shared();

        if (! session.isRunning() || session.getPeerAddress() != v.peer || session.getPeerPort() != v.port)
            startSessionFromFields();
    }

    void onFilterEdited()
    {
        const auto v = currentValues();
        NetworkMidiSettings::applyFilters (v);
        persist();
    }

    void setAllChannels (bool on)
    {
        for (auto& button : channelButtons)
            button.setToggleState (on, juce::dontSendNotification);

        onFilterEdited();
    }

    //==========================================================================
    void timerCallback() override
    {
        refresh();
    }

    void refresh()
    {
        auto& session = RtpMidiSession::shared();
        const auto state = session.getState();

        // Mirror the real session state in the switch (e.g. started at app launch).
        if (session.isRunning() != enableButton.getToggleState() && ! invalidAddressNote)
        {
            enableButton.setToggleState (session.isRunning(), juce::dontSendNotification);
            updateEnableButtonText();
        }

        juce::Colour colour = juce::Colour (0xff6a6a76);
        juce::String text = "Off";
        juce::String info = "Not sending";

        if (invalidAddressNote && ! session.isRunning())
        {
            colour = juce::Colour (0xffe0a030);
            text = "Enter a valid IPv4 address (e.g. 192.168.99.2)";
        }
        else if (state != RtpMidiSession::State::Off)
        {
            text = session.getStatusText();
            colour = state == RtpMidiSession::State::Connected ? juce::Colour (0xff35e06f)
                                                               : juce::Colour (0xffe0a030);

            if (state == RtpMidiSession::State::Connected)
            {
                info = "USB link " + session.getLocalAddress() + "  ->  " + session.getPeerAddress()
                     + "      Messages sent: " + juce::String ((int) session.getMessagesSent());
            }
            else
            {
                info = "Live MIDI is sent as soon as the connection is up";
            }
        }

        statusLabel.setText (text, juce::dontSendNotification);
        infoLabel.setText (info, juce::dontSendNotification);

        // Activity LED
        const auto sentNow = session.getMessagesSent();
        const bool active = sentNow != lastSent;
        lastSent = sentNow;
        const float newBrightness = active ? 1.0f : ledBrightness * 0.55f;

        if (colour != statusColour || std::abs (newBrightness - ledBrightness) > 0.01f)
        {
            statusColour = colour;
            ledBrightness = newBrightness;
            repaint();
        }
    }

    //==========================================================================
    juce::Label title, subtitle;
    juce::Label connectionCaption, filterCaption, helpCaption;
    juce::TextButton enableButton;
    juce::Label deviceCaption, portCaption, statusLabel, infoLabel, channelsCaption;
    juce::ComboBox deviceCombo;
    juce::TextEditor portEditor;
    std::array<juce::TextButton, 16> channelButtons;
    juce::TextButton allButton, noneButton;
    juce::ToggleButton realtimeToggle;
    juce::Label helpText;

    juce::Rectangle<int> connectionCard, filterCard, helpCard, dotBounds, ledBounds;
    juce::Colour statusColour { 0xff6a6a76 };
    float ledBrightness = 0.0f;
    uint32_t lastSent = 0;
    bool invalidAddressNote = false;
    NetworkMidiManager& manager;
    int deviceId = 1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NetworkMidiPanel)
};
