#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

// Standalone-only network transport layer.
//
// This is deliberately outside src/network so the DYSEKT plugin target cannot
// acquire USB device networking or SonoBus transport responsibilities.
class DeviceNetworkTransport
{
public:
    enum class Kind
    {
        WifiLan,
        AppleUsb
    };

    enum class State
    {
        Stopped,
        Starting,
        Connected,
        Error
    };

    struct Device
    {
        Kind kind = Kind::WifiLan;
        juce::String id;
        juce::String name;
        juce::String interfaceName;
        bool connected = false;
    };

    virtual ~DeviceNetworkTransport() = default;

    virtual Kind kind() const noexcept = 0;
    virtual std::vector<Device> enumerate() = 0;
    virtual bool start(const juce::String& deviceId) = 0;
    virtual void stop() = 0;
    virtual State state() const noexcept = 0;
    virtual juce::String status() const = 0;

    static juce::String kindName(Kind kind);
};

using DeviceNetworkTransportPtr = std::unique_ptr<DeviceNetworkTransport>;
