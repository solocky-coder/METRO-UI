#include "AppleUsbNetworkTransport.h"

#if JUCE_WINDOWS
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <vector>
#pragma comment(lib, "iphlpapi.lib")
#endif

AppleUsbNetworkTransport::AppleUsbNetworkTransport()
{
    launcher = std::make_unique<AppleUsbShareLauncher> ([this] (const juce::String& message)
    {
        currentStatus = message;
    });
}

AppleUsbNetworkTransport::~AppleUsbNetworkTransport()
{
    stop();
}

std::vector<DeviceNetworkTransport::Device> AppleUsbNetworkTransport::enumerate()
{
    std::vector<DeviceNetworkTransport::Device> devices;
#if JUCE_WINDOWS
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) == ERROR_BUFFER_OVERFLOW)
    {
        std::vector<unsigned char> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) == NO_ERROR)
        {
            for (auto* adapter = adapters; adapter; adapter = adapter->Next)
            {
                if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD || ! adapter->AdapterName)
                    continue;
                const auto friendly = adapter->FriendlyName ? juce::String(adapter->FriendlyName) : juce::String();
                const auto description = adapter->Description ? juce::String(adapter->Description) : juce::String();
                const auto text = (friendly + " " + description).toLowerCase();
                if (! text.contains ("usbncm") && ! text.contains ("usb ncm"))
                    continue;

                Device device;
                device.kind = Kind::AppleUsb;
                device.id = juce::String::formatted ("%llX", static_cast<unsigned long long> (adapter->Luid.Value));
                device.name = friendly.isNotEmpty() ? friendly : description;
                device.interfaceName = device.name;
                device.connected = adapter->OperStatus == IfOperStatusUp;
                devices.push_back (device);
            }
        }
    }
#endif
    return devices;
}

bool AppleUsbNetworkTransport::start (const juce::String& deviceId)
{
    juce::ignoreUnused (deviceId);
    currentState.store (State::Starting, std::memory_order_release);
    currentStatus = "Requesting administrator permission to start USB sharing…";

    const bool launchStarted = launcher != nullptr && launcher->start();
    if (! launchStarted)
    {
        currentStatus = launcher != nullptr ? launcher->status() : juce::String ("Apple USB helper unavailable");
        currentState.store (State::Error, std::memory_order_release);
    }
    // On success we deliberately leave state() at Starting — poll() (driven
    // by the UI's existing 2Hz timer) is what promotes it to Connected once
    // a usbncm adapter actually comes up, or to Error if the helper exits
    // without one appearing. See AppleUsbShareLauncher::start()'s comment on
    // why this can't be resolved synchronously anymore (UAC can block
    // indefinitely; the USB mode-switch handshake takes several seconds).
    return launchStarted;
}

void AppleUsbNetworkTransport::stop()
{
    if (launcher != nullptr)
        launcher->stop();
    currentStatus = "USB network transport stopped";
    currentState.store (State::Stopped, std::memory_order_release);
}

void AppleUsbNetworkTransport::poll()
{
    const auto state = currentState.load (std::memory_order_acquire);
    if (state != State::Starting && state != State::Connected)
        return; // Stopped/Error only change via start()/stop() themselves.

    if (launcher != nullptr && ! launcher->isRunning())
    {
        // Helper process exited (crash, UAC denial, or it honoured a stop
        // signal we didn't send) without us having called stop() ourselves.
        currentStatus = launcher->status();
        currentState.store (State::Error, std::memory_order_release);
        return;
    }

    bool connected = false;
    for (const auto& device : enumerate())
        if (device.kind == Kind::AppleUsb && device.connected) { connected = true; break; }

    currentState.store (connected ? State::Connected : State::Starting, std::memory_order_release);
    if (connected)
        currentStatus = "USB reverse tethering ready";
}

juce::String AppleUsbNetworkTransport::status() const
{
    return currentStatus;
}
