#include "AppleUsbShareLauncher.h"
#include <juce_events/juce_events.h> // juce::MessageManager::callAsync

#if JUCE_WINDOWS
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <winsock2.h>
#include <windows.h>
#include <oleauto.h>
#include <taskschd.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "taskschd.lib")
#endif

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

void AppleUsbShareLauncher::resetLinkProgress() noexcept
{
    networkReady.store (false, std::memory_order_release);
    dhcpRequestSeen.store (false, std::memory_order_release);
    dhcpLeased.store (false, std::memory_order_release);
}

void AppleUsbShareLauncher::noteHelperLogLine (const juce::String& line)
{
    // A new helper session (or the end of one) invalidates whatever the
    // previous session established.
    if (line.contains ("iPhoneUsbShare session started")
        || line.contains ("Stop signal received")
        || line.contains ("Sharing stopped"))
    {
        resetLinkProgress();
        return;
    }

    if (line.contains ("Isolated USB network ready"))
    {
        networkReady.store (true, std::memory_order_release);
    }
    else if (line.contains ("DHCP DISCOVER received")
             || line.contains ("DHCP REQUEST received")
             || line.contains ("DHCP OFFER sent"))
    {
        dhcpRequestSeen.store (true, std::memory_order_release);
    }
    else if (line.contains ("DHCP ACK sent"))
    {
        dhcpRequestSeen.store (true, std::memory_order_release);
        dhcpLeased.store (true, std::memory_order_release);
    }
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

juce::File AppleUsbShareLauncher::helperExecutablePath()
{
    // Copied here by CMake's POST_BUILD step (see the WIN32 block in
    // CMakeLists.txt) — not embedded, not extracted anywhere. This only
    // resolves correctly when running as DysektStandalone.exe itself.
    return juce::File::getSpecialLocation (juce::File::currentApplicationFile)
               .getSiblingFile ("AppleUsbShareDriver")
               .getChildFile ("iPhoneUsbShare.exe");
}

juce::File AppleUsbShareLauncher::activityLogPath()
{
    // Matches ShareEngine.cs: ActivityLogPath => Combine(AppDir, "ActivityLog.txt"),
    // where AppDir is AppContext.BaseDirectory of the running exe — i.e.
    // the same AppleUsbShareDriver folder helperExecutablePath() resolves to.
    return helperExecutablePath().getSiblingFile ("ActivityLog.txt");
}

namespace
{
    // Registered under its own subfolder purely so it's easy to find in
    // taskschd.msc — has no functional effect.
    const wchar_t* const kTaskFolder = L"\\DYSEKT";
    const wchar_t* const kTaskName   = L"iPhoneUsbShare";

    // Fixed rather than per-launch-unique — see the comment on this in
    // AppleUsbShareLauncher.h next to the (now-removed) stopEventName field.
    const wchar_t* const kStopEventName = L"Local\\DysektAppleUsbShare_StopEvent";

    struct ComInitGuard
    {
        HRESULT hr = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
        bool ok() const noexcept { return SUCCEEDED (hr); }
        ~ComInitGuard() { if (ok()) CoUninitialize(); }
    };

    // Minimal RAII wrapper for COM interface pointers — avoids pulling in
    // comdef.h/_com_ptr_t (and its comsuppw.lib/comsuppwd.lib linkage,
    // which varies by CRT-linkage mode and isn't worth the risk here for
    // something this small).
    template <typename T>
    struct ComPtr
    {
        T* p = nullptr;
        ComPtr() = default;
        ~ComPtr() { if (p != nullptr) p->Release(); }
        ComPtr (const ComPtr&) = delete;
        ComPtr& operator= (const ComPtr&) = delete;
        T** address() noexcept { return &p; }
        T* operator-> () const noexcept { return p; }
        explicit operator bool() const noexcept { return p != nullptr; }
    };

    // Same idea for BSTR — SysAllocString/SysFreeString instead of _bstr_t.
    struct Bstr
    {
        BSTR value = nullptr;
        explicit Bstr (const wchar_t* text) : value (SysAllocString (text)) {}
        explicit Bstr (const juce::String& text) : value (SysAllocString (text.toWideCharPointer())) {}
        ~Bstr() { if (value != nullptr) SysFreeString (value); }
        Bstr (const Bstr&) = delete;
        Bstr& operator= (const Bstr&) = delete;
        operator BSTR() const noexcept { return value; }
    };

    // Same idea for VARIANT — several ITaskService/IRegisteredTask methods
    // take a VARIANT purely to mean "use the default" when left VT_EMPTY.
    struct EmptyVariant
    {
        VARIANT value;
        EmptyVariant() { VariantInit (&value); }
        ~EmptyVariant() { VariantClear (&value); }
        EmptyVariant (const EmptyVariant&) = delete;
        EmptyVariant& operator= (const EmptyVariant&) = delete;
        operator VARIANT() const noexcept { return value; }
    };

    juce::String hrError (const juce::String& what, HRESULT hr)
    {
        return "ERROR: " + what + " (0x" + juce::String::toHexString ((int) hr) + ")";
    }

    // Registers (or, on later calls, silently updates — TASK_CREATE_OR_UPDATE)
    // the \DYSEKT\iPhoneUsbShare task: runs exePath at TASK_RUNLEVEL_HIGHEST
    // under the current interactive user, with no triggers — it only ever
    // runs because runRegisteredTask() below explicitly calls Run() on it.
    // Called on every start(): cheap, and keeps the registered path/arguments
    // in sync if the helper ever moves.
    juce::String registerTask (const juce::File& exePath)
    {
        ComPtr<ITaskService> service;
        HRESULT hr = CoCreateInstance (CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_ITaskService, (void**) service.address());
        if (FAILED (hr) || ! service)
            return hrError ("could not create Task Scheduler service", hr);

        {
            EmptyVariant server, user, domain, password;
            hr = service->Connect (server, user, domain, password);
        }
        if (FAILED (hr))
            return hrError ("could not connect to Task Scheduler", hr);

        ComPtr<ITaskFolder> rootFolder;
        hr = service->GetFolder (Bstr (L"\\"), rootFolder.address());
        if (FAILED (hr) || ! rootFolder)
            return hrError ("could not open Task Scheduler root folder", hr);

        ComPtr<ITaskFolder> dysektFolder;
        hr = rootFolder->GetFolder (Bstr (kTaskFolder), dysektFolder.address());
        if (FAILED (hr) || ! dysektFolder)
        {
            ComPtr<ITaskFolder> created;
            EmptyVariant sddl;
            hr = rootFolder->CreateFolder (Bstr (kTaskFolder), sddl, created.address());
            if (FAILED (hr) && hr != HRESULT_FROM_WIN32 (ERROR_ALREADY_EXISTS))
                return hrError ("could not create the DYSEKT task folder", hr);
            hr = rootFolder->GetFolder (Bstr (kTaskFolder), dysektFolder.address());
            if (FAILED (hr) || ! dysektFolder)
                return hrError ("could not open the DYSEKT task folder", hr);
        }

        ComPtr<ITaskDefinition> task;
        hr = service->NewTask (0, task.address());
        if (FAILED (hr) || ! task)
            return hrError ("could not create a task definition", hr);

        ComPtr<IRegistrationInfo> regInfo;
        if (SUCCEEDED (task->get_RegistrationInfo (regInfo.address())) && regInfo)
        {
            regInfo->put_Author (Bstr (L"DYSEKT"));
            regInfo->put_Description (Bstr (L"Runs iPhoneUsbShare.exe elevated for DYSEKT's "
                                             L"Apple USB Share feature, without a UAC prompt on every launch."));
        }

        ComPtr<IPrincipal> principal;
        hr = task->get_Principal (principal.address());
        if (FAILED (hr) || ! principal)
            return hrError ("could not get the task's principal", hr);
        principal->put_RunLevel (TASK_RUNLEVEL_HIGHEST);
        principal->put_LogonType (TASK_LOGON_INTERACTIVE_TOKEN);

        ComPtr<ITaskSettings> settings;
        if (SUCCEEDED (task->get_Settings (settings.address())) && settings)
        {
            settings->put_DisallowStartIfOnBatteries (VARIANT_FALSE);
            settings->put_StopIfGoingOnBatteries (VARIANT_FALSE);
            settings->put_StartWhenAvailable (VARIANT_FALSE);
            settings->put_ExecutionTimeLimit (Bstr (L"PT0S")); // "PT0S" == no limit, per ITaskSettings docs
            settings->put_MultipleInstances (TASK_INSTANCES_IGNORE_NEW);
        }

        ComPtr<IActionCollection> actions;
        hr = task->get_Actions (actions.address());
        if (FAILED (hr) || ! actions)
            return hrError ("could not get the task's action collection", hr);

        ComPtr<IAction> action;
        hr = actions->Create (TASK_ACTION_EXEC, action.address());
        if (FAILED (hr) || ! action)
            return hrError ("could not create the task's action", hr);

        ComPtr<IExecAction> execAction;
        hr = action->QueryInterface (IID_IExecAction, (void**) execAction.address());
        if (FAILED (hr) || ! execAction)
            return hrError ("could not query the exec action", hr);

        const auto args = juce::String ("--hidden --stop-event=") + juce::String (kStopEventName);
        execAction->put_Path (Bstr (exePath.getFullPathName()));
        execAction->put_Arguments (Bstr (args));
        execAction->put_WorkingDirectory (Bstr (exePath.getParentDirectory().getFullPathName()));

        ComPtr<IRegisteredTask> registered;
        EmptyVariant userId, password, sddl;
        hr = dysektFolder->RegisterTaskDefinition (Bstr (kTaskName), task.p, TASK_CREATE_OR_UPDATE,
                                                    userId, password, TASK_LOGON_INTERACTIVE_TOKEN, sddl,
                                                    registered.address());
        if (FAILED (hr) || ! registered)
            return hrError ("could not register the iPhoneUsbShare task", hr);

        return {};
    }

    // Runs the already-registered task and returns its PID, or 0 on failure
    // (with errorOut explaining why).
    DWORD runRegisteredTask (juce::String& errorOut)
    {
        ComPtr<ITaskService> service;
        HRESULT hr = CoCreateInstance (CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_ITaskService, (void**) service.address());
        if (FAILED (hr) || ! service) { errorOut = hrError ("could not create Task Scheduler service", hr); return 0; }

        {
            EmptyVariant server, user, domain, password;
            hr = service->Connect (server, user, domain, password);
        }
        if (FAILED (hr)) { errorOut = hrError ("could not connect to Task Scheduler", hr); return 0; }

        ComPtr<ITaskFolder> folder;
        hr = service->GetFolder (Bstr (kTaskFolder), folder.address());
        if (FAILED (hr) || ! folder) { errorOut = hrError ("could not open the DYSEKT task folder", hr); return 0; }

        ComPtr<IRegisteredTask> registeredTask;
        hr = folder->GetTask (Bstr (kTaskName), registeredTask.address());
        if (FAILED (hr) || ! registeredTask) { errorOut = hrError ("the iPhoneUsbShare task is not registered", hr); return 0; }

        ComPtr<IRunningTask> runningTask;
        EmptyVariant params;
        hr = registeredTask->Run (params, runningTask.address());
        if (FAILED (hr) || ! runningTask) { errorOut = hrError ("could not run the iPhoneUsbShare task", hr); return 0; }

        // EnginePID can read 0 for a brief moment right after Run() returns,
        // before the Task Scheduler engine has finished spinning the
        // process up — poll for up to ~2 seconds rather than failing
        // immediately on the first read.
        DWORD pid = 0;
        for (int attempt = 0; attempt < 20 && pid == 0; ++attempt)
        {
            runningTask->Refresh();
            runningTask->get_EnginePID (&pid);
            if (pid == 0)
                Sleep (100);
        }

        if (pid == 0)
            errorOut = "ERROR: the iPhoneUsbShare task started but its process ID never appeared";

        return pid;
    }
}

namespace
{
    // ActivityLog.txt is very chatty (hundreds of "SetupAPI candidate" lines,
    // multi-line pnputil dumps). Only forward the lines a user needs to follow
    // the bring-up, plus anything that looks like a failure.
    bool isHelperMilestoneLine (const juce::String& line)
    {
        static const char* const milestones[] =
        {
            "Share mode:", "Apple device found", "Selecting NCM configuration",
            "SET_CONFIGURATION", "SET_MODE", "UsbNcm produced a usable adapter",
            "Isolated DHCP server listening", "Isolated USB network ready",
            "DHCP ", "Stop signal received", "Sharing stopped",
            "iPhoneUsbShare session started"
        };

        for (auto* m : milestones)
            if (line.contains (m))
                return true;

        if (line.contains ("driver-state query failed") || line.contains ("SetupAPI candidate"))
            return false;

        return line.contains ("ERROR") || line.contains ("Exception") || line.containsIgnoreCase ("failed");
    }

    // Helper lines look like "[2026-09-21 12:12:33.441] message"; the UI adds
    // its own timestamp, so drop the helper's.
    juce::String stripHelperTimestamp (const juce::String& line)
    {
        if (line.startsWithChar ('['))
        {
            const auto rest = line.fromFirstOccurrenceOf ("] ", false, false);
            if (rest.isNotEmpty())
                return rest;
        }
        return line;
    }
}

// Owns the background thread that: registers + runs the elevated task via
// Task Scheduler, opens a normal process HANDLE for the PID it returns, then
// tails ActivityLog.txt and watches that handle until stop() signals it to
// exit or the process dies on its own. Everything past the launch step is
// unchanged from the old ShellExecuteExW-based version — a HANDLE obtained
// via OpenProcess behaves identically to one returned by ShellExecuteExW for
// WaitForSingleObject/TerminateProcess/CloseHandle purposes.
class AppleUsbShareLauncher::LaunchThread final : public juce::Thread
{
public:
    explicit LaunchThread (AppleUsbShareLauncher& ownerIn)
        : juce::Thread ("AppleUsbShareLauncher"), owner (ownerIn) {}

    ~LaunchThread() override { stopThread (5000); }

    void run() override
    {
        // COM apartment state is per-thread — this must run here regardless
        // of whatever JUCE has already set up for the message thread.
        ComInitGuard com;
        if (! com.ok())
        {
            owner.running.store (false, std::memory_order_release);
            owner.postLog (hrError ("could not initialize COM for Task Scheduler", com.hr));
            return;
        }

        // Start tailing from the END of the existing log. Previously this
        // began at byte 0 and replayed every earlier session, so a stale
        // "DHCP ACK" from an old run could have looked like a live link.
        // Captured before the helper is launched so nothing it writes is missed.
        const auto logFile = AppleUsbShareLauncher::activityLogPath();
        juce::int64 byteOffset = logFile.existsAsFile() ? logFile.getSize() : 0;
        juce::String pendingText; // trailing partial line carried to the next read

        owner.postLog ("Registering iPhoneUsbShare as a scheduled task (elevated, no repeat prompts)...");

        const auto exePath = AppleUsbShareLauncher::helperExecutablePath();
        if (const auto err = registerTask (exePath); err.isNotEmpty())
        {
            owner.running.store (false, std::memory_order_release);
            // Registering a TASK_RUNLEVEL_HIGHEST task only self-elevates
            // silently for an account that's already a local Administrator;
            // a genuinely standard-user account will see this fail (or
            // still get prompted), which is exactly what should happen.
            owner.postLog (err);
            return;
        }

        owner.postLog ("Starting iPhoneUsbShare via Task Scheduler...");
        juce::String runError;
        const DWORD pid = runRegisteredTask (runError);
        if (pid == 0)
        {
            owner.running.store (false, std::memory_order_release);
            owner.postLog (runError);
            return;
        }

        HANDLE process = OpenProcess (SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                                       FALSE, pid);
        if (process == nullptr)
        {
            owner.running.store (false, std::memory_order_release);
            owner.postLog ("ERROR: iPhoneUsbShare started (PID " + juce::String ((int) pid)
                            + ") but its process handle could not be opened");
            return;
        }

        owner.childProcess = process;
        owner.running.store (true, std::memory_order_release);
        owner.postLog ("iPhoneUsbShare helper started (PID " + juce::String ((int) pid) + "); waiting for USB handshake...");

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
                        auto chunk = pendingText + in.readString();
                        byteOffset = in.getPosition();
                        pendingText = {};

                        // Only act on complete lines: a marker split across two
                        // reads would otherwise be missed and the UI would sit
                        // on "waiting" forever.
                        if (! chunk.endsWithChar ('\n'))
                        {
                            const int lastNewline = chunk.lastIndexOfChar ('\n');
                            pendingText = lastNewline >= 0 ? chunk.substring (lastNewline + 1) : chunk;
                            chunk = lastNewline >= 0 ? chunk.substring (0, lastNewline + 1) : juce::String();
                        }

                        for (const auto& line : juce::StringArray::fromLines (chunk))
                        {
                            if (line.isEmpty())
                                continue;

                            owner.noteHelperLogLine (line);
                            if (isHelperMilestoneLine (line))
                                owner.postLog (stripHelperTimestamp (line));
                        }
                    }
                }
                else if (size < byteOffset)
                {
                    // Helper's own log restarted (new session) — re-read from the top.
                    byteOffset = 0;
                    pendingText = {};
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

bool AppleUsbShareLauncher::isLaunching() const noexcept
{
#if JUCE_WINDOWS
    return launchThread != nullptr && launchThread->isThreadRunning();
#else
    return false;
#endif
}

bool AppleUsbShareLauncher::start()
{
#if JUCE_WINDOWS
    if (launchThread != nullptr && launchThread->isThreadRunning())
    {
        if (! launchThread->threadShouldExit())
        {
            log ("USB sharing is already starting or running.");
            return true;
        }

        // A previous stop() asked this thread to finish, but it has not
        // returned yet. Wait for it (it is woken by stop() and normally exits
        // within milliseconds) instead of treating the Start as a no-op -
        // that used to leave the UI stuck on "Starting" with no helper.
        if (! launchThread->waitForThreadToExit (3000))
        {
            log ("ERROR: previous Direct USB session is still shutting down - try again in a few seconds.");
            return false;
        }
    }

    const auto exePath = helperExecutablePath();
    if (! exePath.existsAsFile())
    {
        log ("ERROR: iPhoneUsbShare.exe not found at " + exePath.getFullPathName()
             + " - it ships alongside DysektStandalone.exe and is not part of the installer zip yet.");
        return false;
    }

    if (stopEvent != nullptr) { CloseHandle (stopEvent); stopEvent = nullptr; }
    stopEvent = CreateEventW (nullptr, TRUE, FALSE, kStopEventName);
    if (stopEvent == nullptr)
    {
        log ("ERROR: could not create stop event");
        return false;
    }

    running.store (false, std::memory_order_release); // set true by LaunchThread once the process actually starts
    resetLinkProgress();
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
    resetLinkProgress();
#if JUCE_WINDOWS
    // The log-tailing thread only leaves its loop when it sees the helper
    // exit through owner.childProcess, and this function nulls that handle
    // below. Without an explicit exit request the thread would spin forever
    // after every Stop, and every later start() would be ignored as
    // "already starting or running".
    if (launchThread != nullptr)
    {
        launchThread->signalThreadShouldExit();
        launchThread->notify();
    }

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
