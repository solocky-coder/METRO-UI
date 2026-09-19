#include "AppleUsbNetworkTransport.h"

#if JUCE_WINDOWS
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "setupapi.lib")
#endif


static bool appleUsbDevicePresent()
{
#if JUCE_WINDOWS
    const auto info = SetupDiGetClassDevsA (nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (info == INVALID_HANDLE_VALUE)
        return false;

    bool found = false;
    for (DWORD i = 0; ; ++i)
    {
        SP_DEVINFO_DATA data {};
        data.cbSize = sizeof (data);
        if (! SetupDiEnumDeviceInfo (info, i, &data))
            break;

        char id[512] {};
        if (SetupDiGetDeviceInstanceIdA (info, &data, id, static_cast<DWORD> (sizeof (id)), nullptr))
        {
            std::string value = id;
            std::transform (value.begin(), value.end(), value.begin(),
                            [] (unsigned char ch) { return static_cast<char> (std::tolower (ch)); });
            if (value.rfind ("usb\\vid_05ac&", 0) == 0)
            {
                found = true;
                break;
            }
        }
    }

    SetupDiDestroyDeviceInfoList (info);
    return found;
#else
    return false;
#endif
}

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
    const bool applePresent = appleUsbDevicePresent();
    const auto state = currentState.load (std::memory_order_acquire);

    if (state == State::Stopped)
    {
        if (applePresent)
        {
            currentStatus = "Apple USB device detected - starting USB sharing...";
            if (! start (""))
                currentStatus = "Apple USB sharing could not be started";
        }
        return;
    }

    if (state == State::Error)
    {
        // Avoid repeated UAC/helper launches after a failure. Physical
        // removal resets the state so the next USB arrival gets a fresh try.
        if (! applePresent)
        {
            currentState.store (State::Stopped, std::memory_order_release);
            currentStatus = "Waiting for iPhone or iPad over USB";
        }
        return;
    }

    if (launcher != nullptr && ! launcher->isRunning())
    {
        currentStatus = launcher->status();
        currentState.store (State::Error, std::memory_order_release);
        return;
    }

    bool connected = false;
    for (const auto& device : enumerate())
        if (device.kind == Kind::AppleUsb && device.connected)
        {
            connected = true;
            break;
        }

    currentState.store (connected ? State::Connected : State::Starting, std::memory_order_release);
    if (connected)
        currentStatus = "USB reverse tethering ready";
}

juce::String AppleUsbNetworkTransport::status() const
{
    return currentStatus;
}
