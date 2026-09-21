#pragma once

#include "DeviceNetworkTransport.h"
#include "AppleUsbShareLauncher.h"

// Thin DYSEKT transport adapter around the separate, elevated iPhoneUsbShare
// helper process. AppleUsbShareLauncher only launches/monitors that helper;
// Network Audio consumes the resulting Windows Ethernet interface the same
// way it always did, via enumerate() below.
class AppleUsbNetworkTransport final : public DeviceNetworkTransport
{
public:
    AppleUsbNetworkTransport();
    ~AppleUsbNetworkTransport() override;

    Kind kind() const noexcept override { return Kind::AppleUsb; }
    std::vector<Device> enumerate() override;

    // Asynchronous: only starts the elevated helper launch (see
    // AppleUsbShareLauncher::start()) and returns once that's underway.
    // A `true` return means "launch attempt in progress", not "connected" —
    // callers must keep polling state()/status() (via poll(), below).
    bool start(const juce::String& deviceId) override;
    void stop() override;
    State state() const noexcept override { return currentState.load (std::memory_order_acquire); }
    juce::String status() const override;

    // Call periodically (AppleUsbShareStatusComponent's existing 2Hz timer
    // does) to resolve Starting -> Connected/Error now that start() no
    // longer blocks until either outcome is known. Not part of the
    // DeviceNetworkTransport interface: transports that start synchronously
    // don't need it.
    void poll();

    // Text describing how far the Direct USB bring-up has got, for the UI
    // while state() is Starting/Connected (otherwise the last status()).
    // Connected is only reported once the helper has ACKed a DHCP lease to the
    // phone in the current session; until the phone asks for an address this
    // reads "Waiting for Ios Device (no DHCP request yet)".
    juce::String linkStatus() const;

    // Receives helper milestones/errors (already filtered) on the message
    // thread so the UI can show them in its Activity box. Pass {} to clear.
    void setActivityCallback (std::function<void (const juce::String&)> callback)
    {
        activityCallback = std::move (callback);
    }

private:
    std::atomic<State> currentState { State::Stopped };
    // A manual Stop suppresses the arrival-triggered auto-start until the
    // physical Apple USB device is removed. Removal clears this latch so
    // the next insertion starts automatically again.
    std::atomic<bool> manualStop { false };
    juce::String currentStatus { "No USB device network interface detected" };
    std::unique_ptr<AppleUsbShareLauncher> launcher;
    std::function<void (const juce::String&)> activityCallback;
};
