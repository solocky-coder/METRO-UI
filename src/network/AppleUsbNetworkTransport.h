#pragma once

#include "DeviceNetworkTransport.h"

#include <memory>

namespace dysekt::network
{

class AppleUsbNetworkTransport final : public DeviceNetworkTransport
{
public:
    AppleUsbNetworkTransport();
    ~AppleUsbNetworkTransport() override;

    Kind kind() const noexcept override { return Kind::AppleUsb; }
    std::vector<DeviceInfo> enumerateDevices() override;
    bool start(const std::string& deviceId) override;
    void stop(const std::string& deviceId) override;
    State state(const std::string& deviceId) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace dysekt::network
