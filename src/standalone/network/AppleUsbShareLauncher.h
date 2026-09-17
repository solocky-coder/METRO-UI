#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <memory>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

// Replaces AppleUsbShareEngine (deleted). DYSEKT no longer reimplements the
// USB/PnP/WinUSB/NCM/ICS state machine in-process; that logic stays in the
// separate iPhoneUsbShare.exe helper, unmodified, running elevated. This
// class only extracts, launches, and monitors that helper.
//
// See docs/APPLE_USB_SHARE_STANDALONE_MIGRATION.md for the full design.
//
// IMPORTANT — start() is asynchronous. The helper must run elevated
// (requireAdministrator in its manifest), so launching it triggers a UAC
// consent prompt that can block indefinitely on user response, followed by
// the USB mode-switch handshake, which can itself take several seconds.
// start() only kicks the launch off on a background thread and returns
// once that thread has been started — it does NOT mean sharing is live.
// Callers must poll status()/isRunning() (AppleUsbShareStatusComponent
// already polls at 2Hz) rather than treat a `true` return as "connected".
class AppleUsbShareLauncher final
{
public:
    using LogCallback = std::function<void (const juce::String&)>;

    explicit AppleUsbShareLauncher (LogCallback log = {});
    ~AppleUsbShareLauncher();

    // Extracts the embedded helper if needed, then launches it elevated and
    // hidden on a background thread which also tails its ActivityLog.txt.
    // Returns false only for failures known synchronously (e.g. the
    // embedded resource is missing, or the target directory couldn't be
    // created) — everything past that point is reported through the log
    // callback and isRunning()/status().
    bool start();

    // Signals the helper's named stop event and gives it a few seconds to
    // exit gracefully (so ShareEngine.StopAsync()'s own safe-mode config
    // cleanup runs), then terminates it if it hasn't.
    void stop();

    bool isRunning() const noexcept { return running.load (std::memory_order_acquire); }
    juce::String status() const;

    // The old engine derived these from its own in-process adapter scan.
    // That scan now lives solely in AppleUsbNetworkTransport::enumerate();
    // kept here only so AppleUsbShareLauncher is a drop-in replacement for
    // AppleUsbShareEngine at every call site.
    juce::String deviceId() const { return {}; }
    juce::String interfaceName() const { return {}; }

private:
    LogCallback logCallback;
    std::atomic<bool> running { false };

    juce::CriticalSection statusLock;
    juce::String currentStatus { "Apple USB service stopped" };
    void setStatus (const juce::String& s);

    // Marshals a log line to the message thread and drops it if this
    // object has since been destroyed. Mirrors DysektProcessor's
    // loaderAliveFlag idiom (see PluginProcessor.h) for the same reason:
    // the tail thread can outlive the request to stop it by up to the
    // stopThread() timeout below.
    std::shared_ptr<std::atomic<bool>> aliveFlag = std::make_shared<std::atomic<bool>> (true);
    void postLog (const juce::String& message);
    void log (const juce::String& message);

#if JUCE_WINDOWS
    class LaunchThread;
    std::unique_ptr<LaunchThread> launchThread;

    HANDLE childProcess = nullptr;
    HANDLE stopEvent = nullptr;
    juce::String stopEventName;

    static juce::File helperInstallDirectory();
    static juce::File helperExecutablePath();
    static juce::File activityLogPath();
    bool extractHelperIfNeeded();
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppleUsbShareLauncher)
};
