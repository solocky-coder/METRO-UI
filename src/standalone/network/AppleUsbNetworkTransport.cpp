#include "AppleUsbNetworkTransport.h"

#if JUCE_WINDOWS
 #include <winsock2.h>
 #include <windows.h>
 #include <iphlpapi.h>
 #include <vector>
 #pragma comment(lib, "iphlpapi.lib")
#endif

AppleUsbNetworkTransport::AppleUsbNetworkTransport() = default;
AppleUsbNetworkTransport::~AppleUsbNetworkTransport() { stop(); }

std::vector<DeviceNetworkTransport::Device> AppleUsbNetworkTransport::enumerate()
{
    std::vector<Device> devices;

#if JUCE_WINDOWS
    ULONG size = 0;
    if (GetAdaptersAddresses (AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW)
        return devices;

    std::vector<unsigned char> buffer (size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*> (buffer.data());
    if (GetAdaptersAddresses (AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) != NO_ERROR)
        return devices;

    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next)
    {
        if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD)
            continue;

        const auto friendlyName = adapter->FriendlyName != nullptr
                                    ? juce::String (adapter->FriendlyName)
                                    : juce::String();
        const auto description = adapter->Description != nullptr
                                    ? juce::String (adapter->Description)
                                    : juce::String();

        // Apple NCM can appear to Windows as an ordinary Ethernet adapter.
        // This discovery layer is intentionally conservative; the actual
        // Apple USB/NCM bring-up belongs behind this standalone-only boundary.
        const auto text = (friendlyName + " " + description).toLowerCase();
        if (!text.contains ("apple") && !text.contains ("iphone") && !text.contains ("ipad"))
            continue;

        Device device;
        device.kind = Kind::AppleUsb;
        device.id = juce::String::toHexString ((juce::int64) adapter->Luid.Value);
        device.name = friendlyName.isNotEmpty() ? friendlyName : description;
        device.interfaceName = device.name;
        device.connected = true;
        devices.push_back (device);
    }
#else
    // Apple USB/NCM is currently a Windows standalone transport. Keeping the
    // non-Windows implementation empty avoids pretending that a platform-
    // specific USB path exists on other hosts.
#endif

    return devices;
}

bool AppleUsbNetworkTransport::start (const juce::String& deviceId)
{
    currentState.store (State::Starting, std::memory_order_release);
    const auto devices = enumerate();

    for (const auto& device : devices)
    {
        if (deviceId.isEmpty() || device.id == deviceId)
        {
            currentDeviceId = device.id;
            currentInterfaceName = device.interfaceName;
            currentStatus = "USB network interface ready — " + device.interfaceName;
            currentState.store (State::Connected, std::memory_order_release);
            return true;
        }
    }

    currentDeviceId.clear();
    currentInterfaceName.clear();
    currentStatus = "Apple USB network interface not present";
    currentState.store (State::Error, std::memory_order_release);
    return false;
}

void AppleUsbNetworkTransport::stop()
{
    currentDeviceId.clear();
    currentInterfaceName.clear();
    currentStatus = "USB network transport stopped";
    currentState.store (State::Stopped, std::memory_order_release);
}

juce::String AppleUsbNetworkTransport::status() const
{
    return currentStatus;
}
