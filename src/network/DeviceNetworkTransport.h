#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

//==============================================================================
// DeviceNetworkTransport
//
// Generic abstraction for the path that makes a remote SonoBus device
// reachable from DYSEKT over IP.  This layer is deliberately independent of
// AOO/SonoBus and of any particular device vendor.
//
// A transport is responsible for establishing/maintaining connectivity to a
// device.  It does NOT carry audio itself and it must not know about AOO source
// IDs, groups, codecs, or the realtime audio callback.
//
// Current/future implementations:
//   - Apple USB: CDC-NCM network interface
//   - Android USB: platform-supported USB networking
//   - Wi-Fi/LAN: ordinary Windows network connectivity
//
// Once a transport is active, SonoBus continues to use its normal UDP/AOO
// packets. MetroNetworkAudio remains the receiver above this layer.
//==============================================================================
class DeviceNetworkTransport
{
public:
    enum class Kind
    {
        WifiLan,
        AppleUsb,
        AndroidUsb
    };

    enum class State
    {
        Stopped,
        Starting,
        Active,
        Error
    };

    struct DeviceInfo
    {
        std::string id;
        std::string displayName;
        Kind kind = Kind::WifiLan;
        State state = State::Stopped;
        bool networkAvailable = false;
    };

    using StateListener = std::function<void(const DeviceInfo&)>;

    virtual ~DeviceNetworkTransport() = default;

    DeviceNetworkTransport(const DeviceNetworkTransport&) = delete;
    DeviceNetworkTransport& operator=(const DeviceNetworkTransport&) = delete;

    virtual Kind kind() const noexcept = 0;
    virtual std::vector<DeviceInfo> enumerateDevices() = 0;
    virtual bool start(const std::string& deviceId) = 0;
    virtual void stop(const std::string& deviceId) = 0;
    virtual State state(const std::string& deviceId) const = 0;

    virtual void setStateListener(StateListener listener)
    {
        stateListener = std::move(listener);
    }

protected:
    DeviceNetworkTransport() = default;

    void notifyState(const DeviceInfo& info) const
    {
        if (stateListener)
            stateListener(info);
    }

private:
    StateListener stateListener;
};

// Human-readable names are kept here rather than in the individual platform
// implementations so the DYSEKT UI does not need Apple/Android-specific logic.
inline const char* deviceNetworkTransportName(DeviceNetworkTransport::Kind kind) noexcept
{
    switch (kind)
    {
        case DeviceNetworkTransport::Kind::WifiLan:   return "Wi-Fi / LAN";
        case DeviceNetworkTransport::Kind::AppleUsb:  return "USB device";
        case DeviceNetworkTransport::Kind::AndroidUsb:return "USB device";
    }

    return "Network";
}
