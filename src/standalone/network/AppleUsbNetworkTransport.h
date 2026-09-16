#pragma once

#include "DeviceNetworkTransport.h"

// Apple USB/NCM transport used only by DYSEKT standalone.
//
// The audio engine remains transport-agnostic: once Windows exposes the Apple
// CDC-NCM function as a network adapter, SonoBus/AOO continues to use ordinary
// UDP. No audio protocol is implemented here.
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
};
