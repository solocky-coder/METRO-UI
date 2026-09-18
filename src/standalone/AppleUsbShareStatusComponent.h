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
        title.setText ("Apple USB Direct Network", juce::dontSendNotification);
        title.setFont (juce::Font (28.0f, juce::Font::bold));
        subtitle.setText ("Direct isolated USB network for iPhone / iPad - no Wi-Fi, router, or Internet sharing", juce::dontSendNotification);
        subtitle.setColour (juce::Label::textColourId, juce::Colours::lightgrey);

        deviceCaption.setText ("Apple device", juce::dontSendNotification);
        deviceLabel.setText ("Looking for iPhone or iPad...", juce::dontSendNotification);
        deviceLabel.setFont (juce::Font (21.0f));
        adapterLabel.setText ("USB Ethernet: not connected", juce::dontSendNotification);
        for (auto* label : { &deviceCaption, &adapterLabel, &connectionCaption, &ipCaption, &rxCaption, &txCaption, &activityTitle })
            label->setColour (juce::Label::textColourId, juce::Colours::lightgrey);

        connectionCaption.setText ("Connection", juce::dontSendNotification);
        connectionLabel.setText ("Not sharing", juce::dontSendNotification);
        connectionLabel.setFont (juce::Font (22.0f, juce::Font::bold));
        ipCaption.setText ("Device IP", juce::dontSendNotification);
        rxCaption.setText ("Download", juce::dontSendNotification);
        txCaption.setText ("Upload", juce::dontSendNotification);
        ipValue.setText ("—", juce::dontSendNotification);
        rxValue.setText ("0 KB/s", juce::dontSendNotification);
        txValue.setText ("0 KB/s", juce::dontSendNotification);
        for (auto* value : { &ipValue, &rxValue, &txValue })
            value->setFont (juce::Font (20.0f, juce::Font::bold));
        for (auto* caption : { &ipCaption, &rxCaption, &txCaption, &connectionCaption, &deviceCaption })
            caption->setFont (juce::Font (15.0f, juce::Font::bold));

        activityTitle.setText ("Activity", juce::dontSendNotification);
        activityTitle.setFont (juce::Font (17.0f, juce::Font::bold));
        activityEditor.setMultiLine (true);
        activityEditor.setReadOnly (true);
        activityEditor.setScrollbarsShown (true);
        activityEditor.setFont (juce::Font (15.0f));
        activityEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101017));
        activityEditor.setColour (juce::TextEditor::textColourId, juce::Colours::white);
        statusDot.setText ("●", juce::dontSendNotification);
        statusDot.setFont (juce::Font (15.0f, juce::Font::bold));

        startButton.setButtonText ("Start sharing");
        stopButton.setButtonText ("Stop");
        diagnosticsButton.setButtonText ("Diagnostics");
        stopButton.setEnabled (false);

        // USB sharing runs via a separate helper that must run elevated
        // (see docs/APPLE_USB_SHARE_STANDALONE_MIGRATION.md); this is here
        // so the resulting UAC prompt reads as expected rather than as a
        // security scare.
        uacNoteLabel.setText ("Windows will ask for administrator permission the first time you start sharing.",
                               juce::dontSendNotification);
        uacNoteLabel.setFont (juce::Font (14.0f));
        uacNoteLabel.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        uacNoteLabel.setJustificationType (juce::Justification::centredLeft);

        startButton.onClick = [this]
        {
            startButton.setEnabled (false);
            stopButton.setEnabled (false);
            appendActivity ("Starting USB network path…");
            // start() is asynchronous now — a `true` return only means the
            // elevated helper launch is underway (a UAC prompt is likely
            // about to appear), not that sharing is live. poll(), called
            // from timerCallback() below, is what resolves Starting into
            // Connected/Error once the outcome is actually known.
            const bool launchStarted = usbTransport.start ({});
            if (launchStarted)
            {
                appendActivity ("Waiting for administrator permission…");
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

        // Use an explicitly typed array. MSVC cannot deduce a common pointer
        // type for this braced initializer because JUCE component subclasses
        // do not participate in the same deduction path as the first cast.
        juce::Component* components[] =
        {
            &title, &subtitle, &deviceCaption, &deviceLabel,
            &adapterLabel, &connectionCaption, &connectionLabel, &ipCaption, &ipValue,
            &rxCaption, &rxValue, &txCaption, &txValue, &activityTitle, &activityEditor,
            &statusDot, &startButton, &stopButton, &diagnosticsButton, &uacNoteLabel
        };

        for (auto* component : components)
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
        deviceCaption.setBounds (device.removeFromTop (24));
        deviceLabel.setBounds (device.removeFromTop (34));
        adapterLabel.setBounds (device.removeFromTop (28));
        statusDot.setBounds (getWidth() - 46, 24, 24, 24);

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
        uacNoteLabel.setBounds (area.removeFromBottom (18));
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

    void timerCallback() override { usbTransport.poll(); refresh(); }

    void refresh()
    {
        const auto state = usbTransport.state();
        const auto devices = usbTransport.enumerate();

        juce::String deviceText;
        juce::String firstConnectedAdapter;
        bool connected = false;
        int appleCount = 0;

        for (const auto& device : devices)
        {
            if (device.kind != DeviceNetworkTransport::Kind::AppleUsb)
                continue;

            ++appleCount;
            if (device.connected)
            {
                connected = true;
                if (firstConnectedAdapter.isEmpty())
                    firstConnectedAdapter = device.interfaceName.isNotEmpty() ? device.interfaceName : device.name;
            }

            if (deviceText.isNotEmpty())
                deviceText << "\n";
            deviceText << (device.connected ? "[USB] " : "[--] ")
                       << (device.name.isNotEmpty() ? device.name : "Apple USB device")
                       << "  |  " << (device.interfaceName.isNotEmpty() ? device.interfaceName : "USB Ethernet")
                       << "  |  " << (device.connected ? "CONNECTED" : "WAITING");
        }

        if (appleCount == 0)
            deviceText = "No Apple USB device detected";
        else if (appleCount == 1 && connected)
            deviceText = "iPhone / iPad detected\n" + deviceText;

        deviceLabel.setText (deviceText, juce::dontSendNotification);
        adapterLabel.setText (firstConnectedAdapter.isNotEmpty()
                                   ? "USB Ethernet: " + firstConnectedAdapter
                                   : "USB Ethernet: waiting for Apple NCM",
                               juce::dontSendNotification);

        connectionLabel.setText (
            state == DeviceNetworkTransport::State::Connected ? "USB network path is ON"
            : state == DeviceNetworkTransport::State::Starting ? "Detecting Apple USB device / starting service..."
            : state == DeviceNetworkTransport::State::Error ? "USB service error"
            : connected ? "Apple USB device detected - starting service..."
                        : "Waiting for iPhone or iPad over USB",
            juce::dontSendNotification);

        if (state == DeviceNetworkTransport::State::Error || state == DeviceNetworkTransport::State::Stopped)
        {
            if (state == DeviceNetworkTransport::State::Error)
            {
                if (! startButton.isEnabled()) startButton.setEnabled (true);
                if (stopButton.isEnabled()) stopButton.setEnabled (false);
            }
            else
            {
                if (! startButton.isEnabled()) startButton.setEnabled (true);
                if (stopButton.isEnabled()) stopButton.setEnabled (false);
            }
        }

#if JUCE_WINDOWS
        updateNetworkMetrics (firstConnectedAdapter);
#else
        ipValue.setText ("-", juce::dontSendNotification);
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
            const juce::String name = juce::String (a->FriendlyName != nullptr ? a->FriendlyName : L"");
            if (preferredAdapter.isNotEmpty() && name != preferredAdapter && ! name.containsIgnoreCase (preferredAdapter)) continue;
            if (preferredAdapter.isEmpty() && ! name.containsIgnoreCase ("UsbNcm") && ! name.containsIgnoreCase ("USB Ethernet")) continue;
            juce::String address;
            juce::String fallbackIpv6;
            for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next)
            {
                char host[NI_MAXHOST] = {};
                if (u->Address.lpSockaddr != nullptr && getnameinfo (u->Address.lpSockaddr, static_cast<socklen_t> (u->Address.iSockaddrLength), host, sizeof (host), nullptr, 0, NI_NUMERICHOST) == 0)
                {
                    const juce::String candidate (host);
                    if (! candidate.contains (":"))
                    {
                        address = candidate;
                        break;
                    }
                    if (fallbackIpv6.isEmpty() && candidate.startsWithIgnoreCase ("fe80"))
                        fallbackIpv6 = candidate;
                }
            }
            if (address.isEmpty())
                address = fallbackIpv6;
            ipValue.setText (address.isNotEmpty() ? address : "—", juce::dontSendNotification);
            MIB_IF_ROW2 row {};
            row.InterfaceIndex = a->IfIndex;
            if (GetIfEntry2 (&row) == NO_ERROR)
            {
                const auto now = juce::Time::getMillisecondCounter();
                const auto rx = row.InOctets;
                const auto tx = row.OutOctets;
                if (lastMetricTime != 0 && now > lastMetricTime)
                {
                    const double seconds = (now - lastMetricTime) / 1000.0;
                    rxValue.setText (juce::String (static_cast<int> ((rx - lastRx) / seconds / 1024.0)) + " KB/s", juce::dontSendNotification);
                    txValue.setText (juce::String (static_cast<int> ((tx - lastTx) / seconds / 1024.0)) + " KB/s", juce::dontSendNotification);
                }
                lastRx = rx; lastTx = tx; lastMetricTime = now;
            }
            return;
        }
        ipValue.setText ("—", juce::dontSendNotification);
        rxValue.setText ("0 KB/s", juce::dontSendNotification);
        txValue.setText ("0 KB/s", juce::dontSendNotification);
    }
#endif

    AppleUsbNetworkTransport& usbTransport;
    juce::Label title, subtitle, deviceCaption, deviceLabel, adapterLabel,
                connectionCaption, connectionLabel, ipCaption, ipValue,
                rxCaption, rxValue, txCaption, txValue, activityTitle, statusDot,
                uacNoteLabel;
    juce::TextEditor activityEditor;
    juce::TextButton startButton, stopButton, diagnosticsButton;
#if JUCE_WINDOWS
    ULONG64 lastRx = 0, lastTx = 0;
    juce::uint32 lastMetricTime = 0;
#endif
};
