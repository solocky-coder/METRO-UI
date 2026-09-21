#include "AppleUsbNetworkTransport.h"

#if JUCE_WINDOWS
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cctype>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "setupapi.lib")
#endif


static bool appleUsbDevicePresent()
{
#if JUCE_WINDOWS
    const auto info = SetupDiGetClassDevsA (nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (info == INVALID_HANDLE_VALUE)
        return false;

    bool found = false;
    for (DWORD i = 0; ; ++i)
    {
        SP_DEVINFO_DATA data {};
        data.cbSize = sizeof (data);
        if (! SetupDiEnumDeviceInfo (info, i, &data))
            break;

        char id[512] {};
        if (SetupDiGetDeviceInstanceIdA (info, &data, id, static_cast<DWORD> (sizeof (id)), nullptr))
        {
            std::string value = id;
            std::transform (value.begin(), value.end(), value.begin(),
                            [] (unsigned char ch) { return static_cast<char> (std::tolower (ch)); });
            if (value.rfind ("usb\\vid_05ac&", 0) == 0)
            {
                found = true;
                break;
            }
        }
    }

    SetupDiDestroyDeviceInfoList (info);
    return found;
#else
    return false;
#endif
}

AppleUsbNetworkTransport::AppleUsbNetworkTransport()
{
    launcher = std::make_unique<AppleUsbShareLauncher> ([this] (const juce::String& message)
    {
        currentStatus = message;
        if (activityCallback)
            activityCallback (message);
    });
}

AppleUsbNetworkTransport::~AppleUsbNetworkTransport()
{
    stop();
}

std::vector<DeviceNetworkTransport::Device> AppleUsbNetworkTransport::enumerate()
{
    std::vector<DeviceNetworkTransport::Device> devices;
#if JUCE_WINDOWS
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) == ERROR_BUFFER_OVERFLOW)
    {
        std::vector<unsigned char> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) == NO_ERROR)
        {
            for (auto* adapter = adapters; adapter; adapter = adapter->Next)
            {
                if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD || ! adapter->AdapterName)
                    continue;
                const auto friendly = adapter->FriendlyName ? juce::String(adapter->FriendlyName) : juce::String();
                const auto description = adapter->Description ? juce::String(adapter->Description) : juce::String();
                const auto text = (friendly + " " + description).toLowerCase();
                if (! text.contains ("usbncm") && ! text.contains ("usb ncm"))
                    continue;

                Device device;
                device.kind = Kind::AppleUsb;
                device.id = juce::String::formatted ("%llX", static_cast<unsigned long long> (adapter->Luid.Value));
                device.name = friendly.isNotEmpty() ? friendly : description;
                device.interfaceName = device.name;
                device.connected = adapter->OperStatus == IfOperStatusUp;
                devices.push_back (device);
            }
        }
    }
#endif
    return devices;
}

bool AppleUsbNetworkTransport::start (const juce::String& deviceId)
{
    juce::ignoreUnused (deviceId);
    manualStop.store (false, std::memory_order_release);
    currentState.store (State::Starting, std::memory_order_release);
    currentStatus = "Starting USB sharing helper...";

    const bool launchStarted = launcher != nullptr && launcher->start();
    if (! launchStarted)
    {
        currentStatus = launcher != nullptr ? launcher->status() : juce::String ("Apple USB helper unavailable");
        currentState.store (State::Error, std::memory_order_release);
    }
    // On success we deliberately leave state() at Starting — poll() (driven
    // by the UI's existing 2Hz timer) is what promotes it to Connected once
    // a usbncm adapter actually comes up, or to Error if the helper exits
    // without one appearing. See AppleUsbShareLauncher::start()'s comment on
    // why this can't be resolved synchronously anymore (UAC can block
    // indefinitely; the USB mode-switch handshake takes several seconds).
    return launchStarted;
}

void AppleUsbNetworkTransport::stop()
{
    manualStop.store (true, std::memory_order_release);
    if (launcher != nullptr)
        launcher->stop();
    currentStatus = "USB network transport stopped";
    currentState.store (State::Stopped, std::memory_order_release);
}

void AppleUsbNetworkTransport::poll()
{
    const bool applePresent = appleUsbDevicePresent();
    const auto state = currentState.load (std::memory_order_acquire);

    if (applePresent)
        absentPolls.store (0, std::memory_order_relaxed);

    // Physical removal is the reset point for auto-start suppression. This
    // also tears down a live helper if the USB device disappears without the
    // normal stop path running first.
    if (! applePresent)
    {
        // ~4 s at the UI's 2 Hz poll: longer than a USB re-enumeration, far
        // shorter than a real unplug that the user would notice.
        constexpr int kAbsentGracePolls = 8;
        if (absentPolls.fetch_add (1, std::memory_order_relaxed) + 1 < kAbsentGracePolls)
            return;

        manualStop.store (false, std::memory_order_release);
        if (state != State::Stopped)
        {
            if (activityCallback)
                activityCallback ("Apple USB device gone for ~4 s; stopping helper.");
            if (launcher != nullptr)
                launcher->stop();
            currentStatus = "Waiting for iPhone or iPad over USB";
            currentState.store (State::Stopped, std::memory_order_release);
        }
        return;
    }

    if (state == State::Stopped)
    {
        if (! manualStop.load (std::memory_order_acquire))
        {
            currentStatus = "Apple USB device detected - starting USB sharing...";
            if (activityCallback)
                activityCallback ("Auto-start: Apple USB device present and no manual Stop.");
            if (! start (""))
                currentStatus = "Apple USB sharing could not be started";
        }
        return;
    }

    if (state == State::Error)
    {
        // Avoid repeated UAC/helper launches after a failure. Physical
        // removal resets the state so the next USB arrival gets a fresh try.
        if (! applePresent)
        {
            currentState.store (State::Stopped, std::memory_order_release);
            currentStatus = "Waiting for iPhone or iPad over USB";
        }
        return;
    }

    // isRunning() only becomes true once the background thread has registered
    // and started the helper, which takes longer than one 2 Hz poll tick.
    // Treating "not running yet" as failure flipped the state to Error
    // before the launch had even finished; only fail once the launch thread
    // is done AND the helper is not running.
    if (launcher != nullptr && ! launcher->isRunning() && ! launcher->isLaunching())
    {
        currentStatus = launcher->status();
        currentState.store (State::Error, std::memory_order_release);
        return;
    }

    // Connected used to mean "some usbncm adapter reports up". That stays true
    // after a session ends (Windows keeps the adapter), so a later run showed
    // "ready" while the helper had not even started and the phone never linked.
    // Now it requires the helper to have ACKed a DHCP lease to the phone in
    // THIS session (parsed from its ActivityLog.txt).
    const bool linkUp = launcher != nullptr && launcher->hasDhcpLease();

    currentState.store (linkUp ? State::Connected : State::Starting, std::memory_order_release);
    if (linkUp)
        currentStatus = "Direct USB link is up (isolated USB network, no Internet sharing)";
}

juce::String AppleUsbNetworkTransport::linkStatus() const
{
    const auto st = currentState.load (std::memory_order_acquire);
    if (st != State::Starting && st != State::Connected)
        return currentStatus;

    if (launcher != nullptr)
    {
        if (launcher->hasDhcpLease())
            return "Direct USB link is up (isolated USB network, no Internet sharing)";
        if (launcher->hasSeenDhcpRequest())
            return "DHCP request received - completing handshake...";
        if (launcher->isUsbNetworkReady())
            return "Waiting for Ios Device (no DHCP request yet)";
    }

    return "Starting Direct USB link...";
}

juce::String AppleUsbNetworkTransport::status() const
{
    return currentStatus;
}
