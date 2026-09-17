#include "AppleUsbShareLauncher.h"
#include <juce_events/juce_events.h> // juce::MessageManager::callAsync

#if JUCE_WINDOWS
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

// BinaryData::iPhoneUsbShare_exe / _exeSize — embedded via the DysektFonts
// binary-data target in CMakeLists.txt. Included here, not in the header,
// so nothing outside this translation unit needs to know the resource name.
#include "BinaryData.h"

namespace
{
    // Bump this whenever iPhoneUsbShare.exe is re-embedded from an updated
    // upstream build, so extractHelperIfNeeded() knows to re-extract instead
    // of trusting a stale copy at the install path.
    constexpr int helperVersion = 1;
}

AppleUsbShareLauncher::AppleUsbShareLauncher (LogCallback log)
    : logCallback (std::move (log))
{
}

AppleUsbShareLauncher::~AppleUsbShareLauncher()
{
    // Flip the flag first so any log line already queued on the message
    // thread via postLog() becomes a no-op instead of touching a
    // half-destroyed object.
    aliveFlag->store (false, std::memory_order_release);
    stop();

#if JUCE_WINDOWS
    if (launchThread != nullptr)
    {
        // Same idiom as fileLoadPool.removeAllJobs(true, 5000) in
        // ~DysektProcessor(): wait, but don't forcibly kill the thread if
        // it's stuck inside a blocking Win32 call past the timeout. The
        // aliveFlag check above is what makes that survivable.
        launchThread = nullptr; // LaunchThread's destructor calls stopThread(5000)
    }
    if (stopEvent != nullptr) { CloseHandle (stopEvent); stopEvent = nullptr; }
    if (childProcess != nullptr) { CloseHandle (childProcess); childProcess = nullptr; }
#endif
}

void AppleUsbShareLauncher::setStatus (const juce::String& s)
{
    const juce::ScopedLock sl (statusLock);
    currentStatus = s;
}

juce::String AppleUsbShareLauncher::status() const
{
    const juce::ScopedLock sl (statusLock);
    return currentStatus;
}

void AppleUsbShareLauncher::log (const juce::String& message)
{
    setStatus (message);
    if (logCallback)
        logCallback (message);
}

void AppleUsbShareLauncher::postLog (const juce::String& message)
{
    auto flag = aliveFlag;
    juce::MessageManager::callAsync ([this, flag, message]
    {
        if (! flag->load (std::memory_order_acquire))
            return;
        log (message);
    });
}

#if JUCE_WINDOWS

juce::File AppleUsbShareLauncher::helperInstallDirectory()
{
    const auto localAppData = juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", {});
    return juce::File (localAppData).getChildFile ("DYSEKT").getChildFile ("iPhoneUsbShare");
}

juce::File AppleUsbShareLauncher::helperExecutablePath()
{
    return helperInstallDirectory().getChildFile ("iPhoneUsbShare.exe");
}

juce::File AppleUsbShareLauncher::activityLogPath()
{
    // Matches ShareEngine.cs: ActivityLogPath => Combine(AppDir, "ActivityLog.txt"),
    // where AppDir is AppContext.BaseDirectory of the (extracted, self-contained)
    // published exe — i.e. the same directory the exe runs from.
    return helperInstallDirectory().getChildFile ("ActivityLog.txt");
}

bool AppleUsbShareLauncher::extractHelperIfNeeded()
{
    const auto installDir = helperInstallDirectory();
    if (! installDir.exists() && ! installDir.createDirectory())
    {
        log ("ERROR: could not create " + installDir.getFullPathName());
        return false;
    }

    const auto exePath = helperExecutablePath();
    const auto versionFile = installDir.getChildFile ("helper_version.txt");
    const auto embeddedSize = (int64_t) BinaryData::iPhoneUsbShare_exeSize;

    const bool upToDate = exePath.existsAsFile()
                          && versionFile.existsAsFile()
                          && versionFile.loadFileAsString().trim().getIntValue() == helperVersion
                          && exePath.getSize() == embeddedSize;
    if (upToDate)
        return true;

    log (exePath.existsAsFile() ? "Updating bundled iPhoneUsbShare helper…"
                                 : "Extracting bundled iPhoneUsbShare helper…");

    if (exePath.existsAsFile() && ! exePath.deleteFile())
    {
        log ("ERROR: could not replace existing helper at " + exePath.getFullPathName());
        return false;
    }

    if (! exePath.replaceWithData (BinaryData::iPhoneUsbShare_exe, BinaryData::iPhoneUsbShare_exeSize))
    {
        log ("ERROR: could not write helper to " + exePath.getFullPathName());
        return false;
    }

    versionFile.replaceWithText (juce::String (helperVersion));
    return true;
}

// Owns the background thread that: launches the elevated helper, then tails
// its ActivityLog.txt and watches its process handle until stop() signals it
// to exit or the process dies on its own.
class AppleUsbShareLauncher::LaunchThread final : public juce::Thread
{
public:
    explicit LaunchThread (AppleUsbShareLauncher& ownerIn)
        : juce::Thread ("AppleUsbShareLauncher"), owner (ownerIn) {}

    ~LaunchThread() override { stopThread (5000); }

    void run() override
    {
        owner.postLog ("Requesting administrator permission to start USB sharing…");

        const auto exePath = AppleUsbShareLauncher::helperExecutablePath();
        const auto args = juce::String ("--hidden --stop-event=") + owner.stopEventName;

        SHELLEXECUTEINFOW info {};
        info.cbSize = sizeof (info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.lpVerb = L"runas";
        info.lpFile = exePath.getFullPathName().toWideCharPointer();
        info.lpParameters = args.toWideCharPointer();
        info.lpDirectory = exePath.getParentDirectory().getFullPathName().toWideCharPointer();
        info.nShow = SW_HIDE;

        if (! ShellExecuteExW (&info) || info.hProcess == nullptr)
        {
            const auto err = GetLastError();
            owner.running.store (false, std::memory_order_release);
            owner.postLog (err == ERROR_CANCELLED
                                ? "USB sharing needs administrator permission to continue."
                                : "ERROR: could not start iPhoneUsbShare helper (Win32 error " + juce::String ((int) err) + ")");
            return;
        }

        owner.childProcess = info.hProcess;
        owner.running.store (true, std::memory_order_release);
        owner.postLog ("iPhoneUsbShare helper started; waiting for USB handshake…");

        const auto logFile = AppleUsbShareLauncher::activityLogPath();
        juce::int64 byteOffset = 0;

        while (! threadShouldExit())
        {
            // Tail ActivityLog.txt by byte offset — this is the entire IPC
            // channel back from the helper; no pipes/sockets are needed
            // because ShareEngine.WriteLog() already writes every state
            // transition there (see ShareEngine.cs).
            if (logFile.existsAsFile())
            {
                const auto size = logFile.getSize();
                if (size > byteOffset)
                {
                    juce::FileInputStream in (logFile);
                    if (in.openedOk())
                    {
                        in.setPosition (byteOffset);
                        const auto chunk = in.readString();
                        byteOffset = in.getPosition();
                        for (const auto& line : juce::StringArray::fromLines (chunk))
                            if (line.isNotEmpty())
                                owner.postLog (line);
                    }
                }
                else if (size < byteOffset)
                {
                    // Helper's own log restarted (new session) — re-read from the top.
                    byteOffset = 0;
                }
            }

            // Zero-timeout poll: detect the helper exiting on its own
            // (crash, or it honoured --stop-event and shut down) without
            // blocking this thread's ability to keep tailing the log.
            if (owner.childProcess != nullptr &&
                WaitForSingleObject (owner.childProcess, 0) == WAIT_OBJECT_0)
            {
                DWORD exitCode = 0;
                GetExitCodeProcess (owner.childProcess, &exitCode);
                owner.running.store (false, std::memory_order_release);
                owner.postLog (exitCode == 0
                                    ? "iPhoneUsbShare helper exited."
                                    : "iPhoneUsbShare helper exited unexpectedly (code " + juce::String ((int) exitCode) + ").");
                return;
            }

            wait (500);
        }
    }

private:
    AppleUsbShareLauncher& owner;
};

#endif // JUCE_WINDOWS

bool AppleUsbShareLauncher::start()
{
#if JUCE_WINDOWS
    if (launchThread != nullptr && launchThread->isThreadRunning())
    {
        log ("USB sharing is already starting or running.");
        return true;
    }

    if (! extractHelperIfNeeded())
        return false;

    // DYSEKT creates the stop event unelevated and passes its name to the
    // elevated helper; an elevated process can open/signal an object a
    // lower-integrity process created (only the reverse is restricted by
    // Windows' integrity policy), so no custom ACLs are needed here.
    if (stopEvent != nullptr) { CloseHandle (stopEvent); stopEvent = nullptr; }
    stopEventName = "Local\\DysektAppleUsbShare_" + juce::String ((juce::uint64) juce::Time::getMillisecondCounterHiRes())
                    + "_" + juce::String (GetCurrentProcessId());
    stopEvent = CreateEventW (nullptr, TRUE, FALSE, stopEventName.toWideCharPointer());
    if (stopEvent == nullptr)
    {
        log ("ERROR: could not create stop event");
        return false;
    }

    running.store (false, std::memory_order_release); // set true by LaunchThread once the process actually starts
    launchThread = std::make_unique<LaunchThread> (*this);
    launchThread->startThread();
    return true;
#else
    juce::ignoreUnused (this);
    log ("Apple USB Share is only supported on Windows.");
    return false;
#endif
}

void AppleUsbShareLauncher::stop()
{
#if JUCE_WINDOWS
    if (stopEvent != nullptr)
        SetEvent (stopEvent);

    if (childProcess != nullptr)
    {
        // Give ShareEngine.StopAsync()'s safe-mode config cleanup a real
        // chance to run before falling back to a hard kill.
        const auto waitResult = WaitForSingleObject (childProcess, 5000);
        if (waitResult != WAIT_OBJECT_0)
        {
            log ("iPhoneUsbShare helper did not exit in time; terminating.");
            TerminateProcess (childProcess, 1);
        }
        CloseHandle (childProcess);
        childProcess = nullptr;
    }

    running.store (false, std::memory_order_release);
    setStatus ("Apple USB service stopped");
#else
    running.store (false, std::memory_order_release);
#endif
}
