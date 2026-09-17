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
    engine = std::make_unique<AppleUsbShareEngine> ([this] (const juce::String& message)
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
    currentStatus = "Starting integrated iPhoneUsbShare USB engine";

    const bool ok = engine != nullptr && engine->start();
    currentDeviceId = engine != nullptr ? engine->deviceId() : juce::String();
    currentInterfaceName = engine != nullptr ? engine->interfaceName() : juce::String();
    currentStatus = engine != nullptr ? engine->status() : juce::String ("Apple USB engine unavailable");
    currentState.store (ok ? State::Connected : State::Error, std::memory_order_release);
    return ok;
}

void AppleUsbNetworkTransport::stop()
{
    if (engine != nullptr)
        engine->stop();
    currentDeviceId.clear();
    currentInterfaceName.clear();
    currentStatus = "USB network transport stopped";
    currentState.store (State::Stopped, std::memory_order_release);
}

juce::String AppleUsbNetworkTransport::status() const
{
    return currentStatus;
}
