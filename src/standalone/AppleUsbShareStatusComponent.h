#pragma once

#include "network/AppleUsbNetworkTransport.h"

#include <juce_gui_basics/juce_gui_basics.h>

#if JUCE_WINDOWS
#include <windows.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

class AppleUsbShareStatusComponent final : public juce::Component,
                                           private juce::Timer
{
public:
    explicit AppleUsbShareStatusComponent (AppleUsbNetworkTransport& transport)
        : usbTransport (transport)
    {
        deviceLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        deviceLabel.setFont (juce::Font (18.0f, juce::Font::plain));
        adapterLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        connectionLabel.setColour (juce::Label::textColourId, juce::Colours::white);
        connectionLabel.setFont (juce::Font (20.0f, juce::Font::bold));
        ipValue.setColour (juce::Label::textColourId, juce::Colours::white);
        rxValue.setColour (juce::Label::textColourId, juce::Colours::white);
        txValue.setColour (juce::Label::textColourId, juce::Colours::white);
        for (auto* label : { &ipCaption, &rxCaption, &txCaption })
        {
            label->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
            addAndMakeVisible (*label);
        }
        ipCaption.setText ("Device IP", juce::dontSendNotification);
        rxCaption.setText ("Download", juce::dontSendNotification);
        txCaption.setText ("Upload", juce::dontSendNotification);

        activityTitle.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        activityTitle.setText ("Activity", juce::dontSendNotification);
        activityTitle.setFont (juce::Font (13.0f, juce::Font::bold));
        activityEditor.setMultiLine (true);
        activityEditor.setReadOnly (true);
        activityEditor.setScrollbarsShown (true);
        activityEditor.setFont (juce::Font (12.0f));
        activityEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xFF101017));
        activityEditor.setColour (juce::TextEditor::textColourId, juce::Colours::white);

        statusDot.setColour (juce::Label::textColourId, juce::Colours::orange);

        addAndMakeVisible (deviceLabel);
        addAndMakeVisible (adapterLabel);
        addAndMakeVisible (connectionLabel);
        addAndMakeVisible (ipValue);
        addAndMakeVisible (rxValue);
        addAndMakeVisible (txValue);
        addAndMakeVisible (activityTitle);
        addAndMakeVisible (activityEditor);
        addAndMakeVisible (statusDot);

        startTimerHz (4);
        refresh();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        auto top = area.removeFromTop (44);
        deviceLabel.setBounds (top.removeFromLeft (getWidth() - 80));
        statusDot.setBounds (area.getRight() - 18, 18, 12, 12);
        adapterLabel.setBounds (area.getX(), 50, area.getWidth(), 22);
        connectionLabel.setBounds (area.getX(), 76, area.getWidth(), 28);

        auto metrics = area.withY (112).withHeight (46);
        const int third = metrics.getWidth() / 3;
        auto cell = metrics.removeFromLeft (third);
        ipCaption.setBounds (cell.removeFromTop (18));
        ipValue.setBounds (cell);
        cell = metrics.removeFromLeft (third);
        rxCaption.setBounds (cell.removeFromTop (18));
        rxValue.setBounds (cell);
        txCaption.setBounds (metrics.removeFromTop (18));
        txValue.setBounds (metrics);

        activityTitle.setBounds (area.getX(), 168, area.getWidth(), 20);
        activityEditor.setBounds (area.getX(), 190, area.getWidth(), juce::jmax (70, area.getHeight() - 190));
    }

private:
    void timerCallback() override { refresh(); }

    void refresh()
    {
        const auto state = usbTransport.state();
        const auto devices = usbTransport.enumerate();
        juce::String adapter;
        bool connected = false;
        for (const auto& device : devices)
        {
            if (device.kind == DeviceNetworkTransport::Kind::AppleUsb)
            {
                adapter = device.name;
                connected = device.connected;
                break;
            }
        }

        deviceLabel.setText (connected ? "iPhone / iPad detected" : "Looking for iPhone or iPad…", juce::dontSendNotification);
        adapterLabel.setText (adapter.isNotEmpty() ? "USB Ethernet: " + adapter : "USB Ethernet: not connected", juce::dontSendNotification);

        const auto status = usbTransport.status();
        connectionLabel.setText (state == DeviceNetworkTransport::State::Connected
                                      ? "Connected"
                                      : state == DeviceNetworkTransport::State::Starting
                                          ? "Starting"
                                          : state == DeviceNetworkTransport::State::Error
                                              ? "Error"
                                              : "Not sharing",
                                  juce::dontSendNotification);
        activityEditor.setText (status.isNotEmpty() ? status : "Waiting for Apple USB transport…", false);

#if JUCE_WINDOWS
        updateNetworkMetrics (adapter);
#else
        ipValue.setText ("—", juce::dontSendNotification);
        rxValue.setText ("0 KB/s", juce::dontSendNotification);
        txValue.setText ("0 KB/s", juce::dontSendNotification);
#endif

        const auto dot = state == DeviceNetworkTransport::State::Connected ? juce::Colours::green
                       : state == DeviceNetworkTransport::State::Error ? juce::Colours::red
                       : state == DeviceNetworkTransport::State::Starting ? juce::Colours::orange
                       : juce::Colours::grey;
        statusDot.setColour (juce::Label::textColourId, dot);
    }

#if JUCE_WINDOWS
    void updateNetworkMetrics (const juce::String& preferredAdapter)
    {
        ULONG size = 0;
        if (GetAdaptersAddresses (AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW)
        {
            ipValue.setText ("—", juce::dontSendNotification);
            rxValue.setText ("0 KB/s", juce::dontSendNotification);
            txValue.setText ("0 KB/s", juce::dontSendNotification);
            return;
        }

        std::vector<unsigned char> buffer (size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*> (buffer.data());
        if (GetAdaptersAddresses (AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) != NO_ERROR)
            return;

        for (auto* a = adapters; a != nullptr; a = a->Next)
        {
            const auto friendly = a->FriendlyName ? juce::String (a->FriendlyName) : juce::String();
            const auto description = a->Description ? juce::String (a->Description) : juce::String();
            const auto combined = (friendly + " " + description).toLowerCase();
            if (! combined.contains ("usbncm") && ! combined.contains ("usb ncm") &&
                ! combined.contains ("iphone") && ! combined.contains ("ipad") &&
                ! combined.contains ("apple"))
                continue;
            if (preferredAdapter.isNotEmpty() && friendly != preferredAdapter && description != preferredAdapter)
                continue;

            juce::String ip = "—";
            for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next)
            {
                if (u->Address.lpSockaddr == nullptr || u->Address.lpSockaddr->sa_family != AF_INET)
                    continue;
                char host[NI_MAXHOST] = {};
                if (getnameinfo (u->Address.lpSockaddr, u->Address.iSockaddrLength, host, sizeof (host), nullptr, 0, NI_NUMERICHOST) == 0)
                {
                    ip = host;
                    break;
                }
            }
            ipValue.setText (ip, juce::dontSendNotification);

            MIB_IF_ROW2 row{};
            row.InterfaceLuid = a->Luid;
            if (GetIfEntry2 (&row) == NO_ERROR)
            {
                const auto now = juce::Time::getMillisecondCounter();
                if (lastSampleMs != 0)
                {
                    const auto dt = juce::jmax<uint32_t> (1, now - lastSampleMs);
                    const auto rx = static_cast<double> (row.InOctets - lastRx) / static_cast<double> (dt) * 1000.0 / 1024.0;
                    const auto tx = static_cast<double> (row.OutOctets - lastTx) / static_cast<double> (dt) * 1000.0 / 1024.0;
                    rxValue.setText (juce::String (juce::jmax (0.0, rx), 1) + " KB/s", juce::dontSendNotification);
                    txValue.setText (juce::String (juce::jmax (0.0, tx), 1) + " KB/s", juce::dontSendNotification);
                }
                lastRx = row.InOctets;
                lastTx = row.OutOctets;
                lastSampleMs = juce::Time::getMillisecondCounter();
            }
            return;
        }

        ipValue.setText ("—", juce::dontSendNotification);
    }
#endif

    AppleUsbNetworkTransport& usbTransport;
    juce::Label deviceLabel, adapterLabel, connectionLabel;
    juce::Label ipCaption, rxCaption, txCaption, ipValue, rxValue, txValue;
    juce::Label activityTitle, statusDot;
    juce::TextEditor activityEditor;
#if JUCE_WINDOWS
    uint64_t lastRx = 0;
    uint64_t lastTx = 0;
    uint32_t lastSampleMs = 0;
#endif
};
