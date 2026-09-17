#pragma once

#include "DeviceNetworkTransport.h"
#include "AppleUsbShareEngine.h"

// Thin DYSEKT transport adapter around the native iPhoneUsbShare engine.
// The complete USB/PnP/NCM/ICS state machine lives in AppleUsbShareEngine;
// Network Audio only consumes the resulting Windows Ethernet interface.
class AppleUsbNetworkTransport final : public DeviceNetworkTransport
{
public:
    AppleUsbNetworkTransport();
    ~AppleUsbNetworkTransport() override;

    Kind kind() const noexcept override { return Kind::AppleUsb; }
    std::vector<Device> enumerate() override;
    bool start(const juce::String& deviceId) override;
    void stop() override;
    State state() const noexcept override { return currentState.load (std::memory_order_acquire); }
    juce::String status() const override;

private:
    std::atomic<State> currentState { State::Stopped };
    juce::String currentDeviceId;
    juce::String currentInterfaceName;
    juce::String currentStatus { "No USB device network interface detected" };
    std::unique_ptr<AppleUsbShareEngine> engine;
};
