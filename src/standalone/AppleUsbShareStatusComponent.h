#pragma once

// Match the Windows target before any JUCE header can pull in windows.h.
#if defined(_WIN32)
 #ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0A00
 #endif
 #ifndef WINVER
  #define WINVER _WIN32_WINNT
 #endif
 #ifndef NTDDI_VERSION
  #define NTDDI_VERSION 0x0A000000
 #endif
 #include <winsock2.h>
 #include <ws2tcpip.h>
 #include <windows.h>
 #include <iphlpapi.h>
 #pragma comment(lib, "ws2_32.lib")
 #pragma comment(lib, "iphlpapi.lib")
#endif

#include "network/AppleUsbNetworkTransport.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

// Embedded iPhoneUsbShare application surface. This is a child of the DYSEKT
// Network Audio panel: no second process and no second top-level window.
class AppleUsbShareStatusComponent final : public juce::Component,
                                           private juce::Timer
{
public:
    explicit AppleUsbShareStatusComponent (AppleUsbNetworkTransport& transport)
        : usbTransport (transport)
    {
        title.setText ("Apple USB Internet Share", juce::dontSendNotification);
        title.setFont (juce::Font (24.0f, juce::Font::bold));
        subtitle.setText ("Share this PC's internet with your iPhone or iPad over USB", juce::dontSendNotification);
        subtitle.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

        deviceCaption.setText ("Apple device", juce::dontSendNotification);
        deviceLabel.setText ("Looking for iPhone or iPad…", juce::dontSendNotification);
        deviceLabel.setFont (juce::Font (17.0f));
        adapterLabel.setText ("USB Ethernet: not connected", juce::dontSendNotification);
        for (auto* label : { &deviceCaption, &adapterLabel, &connectionCaption, &ipCaption, &rxCaption, &txCaption, &activityTitle })
            label->setColour (juce::Label::textColourId, juce::Colours::lightgrey);

        connectionCaption.setText ("Connection", juce::dontSendNotification);
        connectionLabel.setText ("Not sharing", juce::dontSendNotification);
        connectionLabel.setFont (juce::Font (19.0f, juce::Font::bold));
        ipCaption.setText ("Device IP", juce::dontSendNotification);
        rxCaption.setText ("Download", juce::dontSendNotification);
        txCaption.setText ("Upload", juce::dontSendNotification);
        ipValue.setText ("—", juce::dontSendNotification);
        rxValue.setText ("0 KB/s", juce::dontSendNotification);
        txValue.setText ("0 KB/s", juce::dontSendNotification);

        activityTitle.setText ("Activity", juce::dontSendNotification);
        activityTitle.setFont (juce::Font (13.0f, juce::Font::bold));
        activityEditor.setMultiLine (true);
        activityEditor.setReadOnly (true);
        activityEditor.setScrollbarsShown (true);
        activityEditor.setFont (juce::Font (12.0f));
        activityEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101017));
        activityEditor.setColour (juce::TextEditor::textColourId, juce::Colours::white);
        statusDot.setText ("●", juce::dontSendNotification);
        statusDot.setFont (juce::Font (15.0f, juce::Font::bold));

        startButton.setButtonText ("Start sharing");
        stopButton.setButtonText ("Stop");
        diagnosticsButton.setButtonText ("Diagnostics");
        stopButton.setEnabled (false);

        startButton.onClick = [this]
        {
            startButton.setEnabled (false);
            stopButton.setEnabled (false);
            appendActivity ("Starting USB network path…");
            const bool ok = usbTransport.start ({});
            if (ok)
            {
                appendActivity ("USB network path is ON.");
                stopButton.setEnabled (true);
            }
            else
            {
                appendActivity ("ERROR: " + usbTransport.status());
                startButton.setEnabled (true);
            }
            refresh();
        };

        stopButton.onClick = [this]
        {
            stopButton.setEnabled (false);
            appendActivity ("Stopping USB network path…");
            usbTransport.stop();
            startButton.setEnabled (true);
            appendActivity ("USB network path stopped.");
            refresh();
        };

        diagnosticsButton.onClick = [this]
        {
            const auto devices = usbTransport.enumerate();
            juce::String report;
            report << "Apple USB Internet Share diagnostics\n\n";
            report << "Transport state: " << stateName (usbTransport.state()) << "\n";
            report << "Transport status: " << usbTransport.status() << "\n\n";
            report << "Detected network adapters:\n";
            if (devices.empty()) report << "  (none)\n";
            for (const auto& device : devices)
                report << "  " << device.name << " | connected=" << (device.connected ? "yes" : "no") << "\n";
            juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon, "Diagnostics", report, "OK");
            appendActivity ("Diagnostics collected.");
        };

        for (auto* component : { static_cast<juce::Component*> (&title), &subtitle, &deviceCaption, &deviceLabel,
                                 &adapterLabel, &connectionCaption, &connectionLabel, &ipCaption, &ipValue,
                                 &rxCaption, &rxValue, &txCaption, &txValue, &activityTitle, &activityEditor,
                                 &statusDot, &startButton, &stopButton, &diagnosticsButton })
            addAndMakeVisible (*component);

        startTimerHz (2);
        refresh();
    }

    ~AppleUsbShareStatusComponent() override { stopTimer(); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (18);
        title.setBounds (area.removeFromTop (32));
        subtitle.setBounds (area.removeFromTop (25));

        auto device = area.removeFromTop (78).reduced (10);
        deviceCaption.setBounds (device.removeFromTop (18));
        deviceLabel.setBounds (device.removeFromTop (24));
        adapterLabel.setBounds (device);
        statusDot.setBounds (getWidth() - 40, 58, 18, 18);

        auto connection = area.removeFromTop (112).reduced (10);
        connectionCaption.setBounds (connection.removeFromTop (20));
        connectionLabel.setBounds (connection.removeFromTop (28));
        const int third = connection.getWidth() / 3;
        auto cell = connection.removeFromLeft (third);
        ipCaption.setBounds (cell.removeFromTop (18)); ipValue.setBounds (cell);
        cell = connection.removeFromLeft (third);
        rxCaption.setBounds (cell.removeFromTop (18)); rxValue.setBounds (cell);
        txCaption.setBounds (connection.removeFromTop (18)); txValue.setBounds (connection);

        activityTitle.setBounds (area.removeFromTop (20));
        auto buttons = area.removeFromBottom (32);
        activityEditor.setBounds (area);
        diagnosticsButton.setBounds (buttons.removeFromRight (105));
        stopButton.setBounds (buttons.removeFromRight (72).reduced (2, 0));
        startButton.setBounds (buttons.removeFromRight (118).reduced (2, 0));
    }

private:
    static juce::String stateName (DeviceNetworkTransport::State state)
    {
        switch (state)
        {
            case DeviceNetworkTransport::State::Stopped: return "Stopped";
            case DeviceNetworkTransport::State::Starting: return "Starting";
            case DeviceNetworkTransport::State::Connected: return "Connected";
            case DeviceNetworkTransport::State::Error: return "Error";
            default: return "Unknown";
        }
    }

    void appendActivity (const juce::String& message)
    {
        const auto line = juce::Time::getCurrentTime().formatted ("[%H:%M:%S] ") + message + "\n";
        activityEditor.setText (line + activityEditor.getText().substring (0, 14000), false);
    }

    void timerCallback() override { refresh(); }

    void refresh()
    {
        const auto state = usbTransport.state();
        const auto devices = usbTransport.enumerate();
        juce::String adapter;
        bool connected = false;
        for (const auto& device : devices)
            if (device.kind == DeviceNetworkTransport::Kind::AppleUsb) { adapter = device.name; connected = device.connected; break; }

        deviceLabel.setText (connected ? "iPhone / iPad detected" : "Connect your iPhone or iPad by USB", juce::dontSendNotification);
        adapterLabel.setText (adapter.isNotEmpty() ? "USB Ethernet: " + adapter : "USB Ethernet: not connected", juce::dontSendNotification);
        connectionLabel.setText (state == DeviceNetworkTransport::State::Connected ? "USB network path is ON"
                                  : state == DeviceNetworkTransport::State::Starting ? "Starting"
                                  : state == DeviceNetworkTransport::State::Error ? "Error" : "Ready", juce::dontSendNotification);

#if JUCE_WINDOWS
        updateNetworkMetrics (adapter);
#else
        ipValue.setText ("—", juce::dontSendNotification);
        rxValue.setText ("0 KB/s", juce::dontSendNotification);
        txValue.setText ("0 KB/s", juce::dontSendNotification);
#endif

        const auto dot = connected && state == DeviceNetworkTransport::State::Connected ? juce::Colours::green
                       : connected ? juce::Colours::orange
                       : state == DeviceNetworkTransport::State::Error ? juce::Colours::red : juce::Colours::grey;
        statusDot.setColour (juce::Label::textColourId, dot);
    }

#if JUCE_WINDOWS
    void updateNetworkMetrics (const juce::String& preferredAdapter)
    {
        ULONG size = 0;
        if (GetAdaptersAddresses (AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW)
        {
            ipValue.setText ("—", juce::dontSendNotification); rxValue.setText ("0 KB/s", juce::dontSendNotification); txValue.setText ("0 KB/s", juce::dontSendNotification); return;
        }
        std::vector<unsigned char> buffer (size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*> (buffer.data());
        if (GetAdaptersAddresses (AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) != NO_ERROR) return;
        for (auto* a = adapters; a != nullptr; a = a->Next)
        {
            const auto friendly = a->FriendlyName ? juce::String (a->FriendlyName) : juce::String();
            const auto description = a->Description ? juce::String (a->Description) : juce::String();
            const auto combined = (friendly + " " + description).toLowerCase();
            if (! combined.contains ("usbncm") && ! combined.contains ("usb ncm") && ! combined.contains ("iphone") && ! combined.contains ("ipad") && ! combined.contains ("apple")) continue;
            if (preferredAdapter.isNotEmpty() && friendly != preferredAdapter && description != preferredAdapter) continue;

            juce::String ip = "—";
            for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next)
            {
                if (u->Address.lpSockaddr == nullptr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
                char host[NI_MAXHOST] = {};
                if (getnameinfo (u->Address.lpSockaddr, u->Address.iSockaddrLength, host, sizeof (host), nullptr, 0, NI_NUMERICHOST) == 0) { ip = host; break; }
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
                lastRx = row.InOctets; lastTx = row.OutOctets; lastSampleMs = juce::Time::getMillisecondCounter();
            }
            return;
        }
        ipValue.setText ("—", juce::dontSendNotification);
        rxValue.setText ("0 KB/s", juce::dontSendNotification);
        txValue.setText ("0 KB/s", juce::dontSendNotification);
    }
#endif

    AppleUsbNetworkTransport& usbTransport;
    juce::Label title, subtitle;
    juce::Label deviceCaption, deviceLabel, adapterLabel, statusDot;
    juce::Label connectionCaption, connectionLabel;
    juce::Label ipCaption, ipValue, rxCaption, rxValue, txCaption, txValue;
    juce::Label activityTitle;
    juce::TextEditor activityEditor;
    juce::TextButton startButton, stopButton, diagnosticsButton;
#if JUCE_WINDOWS
    uint64_t lastRx = 0, lastTx = 0;
    uint32_t lastSampleMs = 0;
#endif
};
