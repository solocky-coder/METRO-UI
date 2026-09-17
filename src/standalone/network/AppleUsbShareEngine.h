#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>

// Native C++ port of the iPhoneUsbShare orchestration engine.
//
// This class intentionally contains the USB/PnP/WinUSB/NCM/ICS state machine;
// the Network Audio panel only hosts the UI. No second iPhoneUsbShare process
// is launched.
class AppleUsbShareEngine final
{
public:
    using LogCallback = std::function<void (const juce::String&)>;

    explicit AppleUsbShareEngine (LogCallback log = {});
    ~AppleUsbShareEngine();

    bool start();
    void stop();

    juce::String status() const;
    juce::String deviceId() const;
    juce::String interfaceName() const;

private:
    LogCallback logCallback;
    std::atomic<bool> running { false };
    juce::String currentStatus { "Apple USB service stopped" };
    juce::String currentDeviceId;
    juce::String currentInterfaceName;

    void log (const juce::String& message);
    bool fail (const juce::String& message);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppleUsbShareEngine)
};
