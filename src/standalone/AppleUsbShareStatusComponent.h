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
        ipValue.setText ("-", juce::dontSendNotification);
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
        statusDot.setText (juce::String::fromUTF8 ("\xe2\x97\x8f"), juce::dontSendNotification);
        statusDot.setFont (juce::Font (15.0f, juce::Font::bold));

        startButton.setButtonText ("Start sharing");
        stopButton.setButtonText ("Stop");
        diagnosticsButton.setButtonText ("Diagnostics");
        stopButton.setEnabled (false);

        // DysektLookAndFeel draws a disabled button exactly like an enabled
        // one and ignores buttonColourId unless "flatFill" is set. Opt these
        // two in and colour them ourselves (see updateActionButtons()) so it
        // is obvious at a glance which of Start / Stop can be pressed.
        startButton.getProperties().set ("flatFill", true);
        stopButton.getProperties().set ("flatFill", true);

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
            appendActivity ("Starting Direct USB link...");
            // start() is asynchronous now — a `true` return only means the
            // elevated helper launch is underway (a UAC prompt is likely
            // about to appear), not that sharing is live. poll(), called
            // from timerCallback() below, is what resolves Starting into
            // Connected/Error once the outcome is actually known.
            if (usbTransport.start ({}))
                appendActivity ("Launching Direct USB helper (Windows may show an administrator prompt)...");
            else
                appendActivity ("ERROR: " + usbTransport.status());

            // Which of Start / Stop is available is derived from the
            // transport state in refresh(), so it is also correct when
            // sharing was started automatically rather than by this button.
            refresh();
        };

        stopButton.onClick = [this]
        {
            appendActivity ("Stopping Direct USB link...");
            usbTransport.stop();
            appendActivity ("Direct USB link stopped.");
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

        usbTransport.setActivityCallback ([this] (const juce::String& message) { appendActivity (message); });
        startTimerHz (2);
        refresh();
    }

    ~AppleUsbShareStatusComponent() override
    {
        usbTransport.setActivityCallback ({});
        stopTimer();
    }

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

    void timerCallback() override
    {
        usbTransport.poll();
        // Log each change of bring-up phase once (e.g. "Waiting for Ios Device
        // (no DHCP request yet)" -> "Direct USB link is up"). Helper milestones
        // and errors arrive separately through the transport's activity callback.
        const auto st = usbTransport.state();
        if (st == DeviceNetworkTransport::State::Starting || st == DeviceNetworkTransport::State::Connected)
        {
            const auto phase = usbTransport.linkStatus();
            if (phase != lastLoggedPhase)
            {
                lastLoggedPhase = phase;
                appendActivity (phase);
            }
        }
        else
        {
            lastLoggedPhase = {};
        }
        refresh();
    }

    // Start / Stop availability follows the transport state, whichever path
    // changed it (button, arrival auto-start, error, unplug):
    //   Stopped / Error      -> Start available, Stop not
    //   Starting / Connected -> Stop available, Start not
    // Available buttons are solid (green / red) with white text; unavailable
    // ones are flat dark with dim text, and the hint line says what to do.
    void updateActionButtons (DeviceNetworkTransport::State state)
    {
        using S = DeviceNetworkTransport::State;
        const bool canStart = (state == S::Stopped || state == S::Error);
        const bool canStop  = (state == S::Starting || state == S::Connected);

        if (startButton.isEnabled() != canStart) startButton.setEnabled (canStart);
        if (stopButton.isEnabled()  != canStop)  stopButton.setEnabled (canStop);

        styleActionButton (startButton, canStart, juce::Colour (0xff2a9d5c));
        styleActionButton (stopButton,  canStop,  juce::Colour (0xffc23b3b));

        juce::String hint, startTip, stopTip;
        switch (state)
        {
            case S::Starting:
                hint     = "Starting - please wait, this can take 30-40 seconds. Stop cancels it.";
                startTip = "Already starting - wait for the link to come up.";
                stopTip  = "Cancel and stop Direct USB sharing.";
                break;
            case S::Connected:
                hint     = "Sharing is active. Click Stop to end it.";
                startTip = "Sharing is already active.";
                stopTip  = "Stop Direct USB sharing.";
                break;
            case S::Error:
                hint     = "Sharing failed - see Activity, then click Start sharing to try again.";
                startTip = "Try starting Direct USB sharing again.";
                stopTip  = "Nothing to stop - sharing is not running.";
                break;
            default:
                hint     = "Sharing is stopped. Click Start sharing to begin "
                           "(Windows asks for administrator permission the first time).";
                startTip = "Start Direct USB sharing.";
                stopTip  = "Nothing to stop - sharing is not running.";
                break;
        }

        uacNoteLabel.setText (hint, juce::dontSendNotification);
        uacNoteLabel.setColour (juce::Label::textColourId,
                                state == S::Error ? juce::Colours::orange : juce::Colours::lightgrey);
        startButton.setTooltip (startTip);
        stopButton.setTooltip (stopTip);
    }

    static void styleActionButton (juce::TextButton& b, bool available, juce::Colour availableFill)
    {
        b.setColour (juce::TextButton::buttonColourId, available ? availableFill : juce::Colour (0xff17171d));
        b.setColour (juce::TextButton::textColourOffId, available ? juce::Colours::white : juce::Colour (0xff5c5c68));
    }

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
                       << "  |  " << (! device.connected ? "WAITING"
                                             : state == DeviceNetworkTransport::State::Connected ? "LINK UP"
                                                                                                  : "ADAPTER READY - LINK NOT UP YET");
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
            state == DeviceNetworkTransport::State::Connected ? "DIRECT USB is ON - isolated USB link active"
            : state == DeviceNetworkTransport::State::Starting ? usbTransport.linkStatus()
            : state == DeviceNetworkTransport::State::Error ? "Direct USB error - see Activity"
            : connected ? "Apple USB device detected - starting Direct USB..."
                        : "Waiting for iPhone or iPad over USB",
            juce::dontSendNotification);

        updateActionButtons (state);

#if JUCE_WINDOWS
        updateNetworkMetrics (firstConnectedAdapter);
#else
        ipValue.setText ("-", juce::dontSendNotification);
        rxValue.setText ("0 KB/s", juce::dontSendNotification);
        txValue.setText ("0 KB/s", juce::dontSendNotification);
#endif

        // Windows keeps the NCM adapter and its static 192.168.99.1 address
        // configured after a session ends, so an IP here does not prove the
        // link is live. Only show it once the helper has ACKed a DHCP lease.
        if (state != DeviceNetworkTransport::State::Connected)
            ipValue.setText ("- (not linked yet)", juce::dontSendNotification);

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
            ipValue.setText ("-", juce::dontSendNotification); rxValue.setText ("0 KB/s", juce::dontSendNotification); txValue.setText ("0 KB/s", juce::dontSendNotification); return;
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
            ipValue.setText (address.isNotEmpty() ? address : "-", juce::dontSendNotification);
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
        ipValue.setText ("-", juce::dontSendNotification);
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
    juce::String lastLoggedPhase;
    juce::TextButton startButton, stopButton, diagnosticsButton;
#if JUCE_WINDOWS
    ULONG64 lastRx = 0, lastTx = 0;
    juce::uint32 lastMetricTime = 0;
#endif
};
