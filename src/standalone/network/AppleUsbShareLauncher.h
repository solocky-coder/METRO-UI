#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <functional>
#include <memory>

#if JUCE_WINDOWS
 // winsock2.h must come before windows.h in every translation unit that
 // (transitively) includes this header — see AppleUsbNetworkTransport.cpp,
 // which pulls this header in before its own winsock2.h/windows.h pair.
 // Getting this order backwards anywhere in the chain makes windows.h drag
 // in the legacy winsock.h first, which then collides with winsock2.h.
 #include <winsock2.h>
 #include <windows.h>
#endif

// Replaces AppleUsbShareEngine (deleted). DYSEKT no longer reimplements the
// USB/PnP/WinUSB/NCM/ICS state machine in-process; that logic stays in the
// separate iPhoneUsbShare.exe helper, unmodified, running elevated. This
// class only launches and monitors that helper.
//
// See docs/APPLE_USB_SHARE_STANDALONE_MIGRATION.md for the full design.
//
// The helper is NOT embedded into DysektStandalone — a self-contained
// single-file .NET publish runs 150+ MB, which is both too large for a
// plain git push and a bad fit for JUCE's BinaryData (compiling that much
// data into a C-array is its own problem independent of git). Instead it
// ships as a loose file, copied next to DysektStandalone.exe by CMake's
// POST_BUILD step (see the WIN32 block in CMakeLists.txt), and located at
// runtime via helperExecutablePath() below.
//
// IMPORTANT — start() is asynchronous. The first time it runs, it registers
// a Task Scheduler task (\DYSEKT\iPhoneUsbShare) with TASK_RUNLEVEL_HIGHEST +
// TASK_LOGON_INTERACTIVE_TOKEN, then calls IRegisteredTask::Run() on it —
// this is the standard "elevate without a fresh UAC prompt every launch"
// pattern, and it only works silently for an account that's already a local
// Administrator (a split-token elevation Windows already trusts it to grant
// itself); for a genuinely standard user, Windows will still prompt or the
// registration will fail, and that failure is reported the normal way
// through the log callback. Either way, start() only kicks the registration
// + launch off on a background thread and returns once that thread has been
// started — it does NOT mean sharing is live. Callers must poll
// status()/isRunning() (AppleUsbShareStatusComponent already polls at 2Hz)
// rather than treat a `true` return as "connected".
class AppleUsbShareLauncher final
{
public:
    using LogCallback = std::function<void (const juce::String&)>;

    explicit AppleUsbShareLauncher (LogCallback log = {});
    ~AppleUsbShareLauncher();

    // Launches the helper elevated and hidden on a background thread, which
    // also tails its ActivityLog.txt. Returns false only for failures known
    // synchronously (e.g. the helper isn't present next to this exe) —
    // everything past that point is reported through the log callback and
    // isRunning()/status().
    bool start();

    // Signals the helper's named stop event and gives it a few seconds to
    // exit gracefully (so ShareEngine.StopAsync()'s own safe-mode config
    // cleanup runs), then terminates it if it hasn't.
    void stop();

    bool isRunning() const noexcept { return running.load (std::memory_order_acquire); }

    // True while the background launch/monitor thread is alive - including
    // the window after start() where the task is still being registered and
    // the helper has not been spawned yet (isRunning() is false then).
    bool isLaunching() const noexcept;

    // Link progress parsed from the helper's ActivityLog.txt for the CURRENT
    // session only (history from earlier sessions is skipped, and every flag
    // is cleared on start()/stop() and when the helper logs a new session).
    // "Windows sees a usbncm adapter" is NOT proof the phone is linked - a
    // stale adapter from an earlier session stays "up" - so the UI uses these
    // instead: the helper's isolated network is ready, the phone has sent a
    // DHCP request, and the helper has ACKed a lease to it.
    bool isUsbNetworkReady() const noexcept   { return networkReady.load (std::memory_order_acquire); }
    bool hasSeenDhcpRequest() const noexcept  { return dhcpRequestSeen.load (std::memory_order_acquire); }
    bool hasDhcpLease() const noexcept        { return dhcpLeased.load (std::memory_order_acquire); }
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

    std::atomic<bool> networkReady { false };
    std::atomic<bool> dhcpRequestSeen { false };
    std::atomic<bool> dhcpLeased { false };
    void resetLinkProgress() noexcept;
    void noteHelperLogLine (const juce::String& line); // thread-safe; called from the tail thread

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
    // No longer per-launch-unique: a Scheduled Task's action arguments are
    // fixed at registration time (Run()'s params VARIANT only reaches
    // COM-handler actions, not EXEC actions like this one), so the name has
    // to be a fixed constant — see kStopEventName in the .cpp. That's fine;
    // this feature is inherently single-instance (one user, one phone, one
    // DYSEKT).

    // juce::File::currentApplicationFile resolves to DysektStandalone.exe
    // itself only for the Standalone target — this deliberately doesn't
    // need to work (and isn't linked) for the VST3 target.
    static juce::File helperExecutablePath();
    static juce::File activityLogPath();
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppleUsbShareLauncher)
};
