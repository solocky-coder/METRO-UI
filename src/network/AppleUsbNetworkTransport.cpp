#include "AppleUsbNetworkTransport.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#include <winusb.h>
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace dysekt::network
{
namespace
{
constexpr unsigned short kAppleVid = 0x05AC;
constexpr char kControlGuid[] = "{8D4D9C11-3B6B-4D3A-9B0B-7E8B2E2E0C51}";
constexpr DWORD kPresent = DIGCF_PRESENT;
constexpr DWORD kAllClasses = DIGCF_ALLCLASSES;

struct DeviceHandle
{
    HANDLE file = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE usb = nullptr;

    ~DeviceHandle()
    {
        if (usb) WinUsb_Free(usb);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }

    explicit operator bool() const noexcept { return usb != nullptr; }
};

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool isAppleInstance(const std::string& id)
{
    const auto v = lower(id);
    return v.rfind("usb\\vid_05ac&", 0) == 0;
}

std::string instanceId(HDEVINFO info, SP_DEVINFO_DATA& data)
{
    DWORD required = 0;
    SetupDiGetDeviceInstanceIdA(info, &data, nullptr, 0, &required);
    if (required == 0) return {};
    std::string result(required, '\0');
    if (!SetupDiGetDeviceInstanceIdA(info, &data, result.data(), required, nullptr)) return {};
    if (!result.empty() && result.back() == '\0') result.pop_back();
    return result;
}

std::string propertyString(HDEVINFO info, SP_DEVINFO_DATA& data, DWORD property)
{
    DWORD type = 0;
    DWORD bytes = 0;
    SetupDiGetDeviceRegistryPropertyA(info, &data, property, &type, nullptr, 0, &bytes, nullptr);
    if (bytes == 0) return {};
    std::vector<char> buffer(bytes + 1, 0);
    if (!SetupDiGetDeviceRegistryPropertyA(info, &data, property, &type,
                                           reinterpret_cast<PBYTE>(buffer.data()), bytes, nullptr, nullptr))
        return {};
    return std::string(buffer.data());
}

std::vector<std::pair<std::string, std::string>> enumerateApple()
{
    std::vector<std::pair<std::string, std::string>> result;
    GUID empty = GUID_NULL;
    const auto info = SetupDiGetClassDevsA(&empty, nullptr, nullptr, kPresent | kAllClasses);
    if (info == INVALID_HANDLE_VALUE) return result;

    for (DWORD i = 0; ; ++i)
    {
        SP_DEVINFO_DATA data{};
        data.cbSize = sizeof(data);
        if (!SetupDiEnumDeviceInfo(info, i, &data)) break;
        const auto id = instanceId(info, data);
        if (!isAppleInstance(id)) continue;
        auto name = propertyString(info, data, SPDRP_FRIENDLYNAME);
        if (name.empty()) name = propertyString(info, data, SPDRP_DEVICEDESC);
        result.emplace_back(id, name.empty() ? "Apple USB device" : name);
    }
    SetupDiDestroyDeviceInfoList(info);
    return result;
}

std::string findControlPath()
{
    GUID guid{};
    if (CLSIDFromString(CA2W(kControlGuid), &guid) != NOERROR) return {};
    return {};
}

// The custom control interface is deliberately opened through the device
// interface GUID installed by WinUsbControl.inf.  We use SetupDi directly so
// this transport does not depend on a separate iPhoneUsbShare process.
std::string controlPath()
{
    GUID guid{};
    if (IIDFromString(CA2W(kControlGuid), &guid) != NOERROR) return {};

    auto info = SetupDiGetClassDevsA(&guid, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) return {};
    std::string path;
    for (DWORD i = 0; ; ++i)
    {
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &guid, i, &iface)) break;
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailA(info, &iface, nullptr, 0, &required, nullptr);
        if (required == 0) continue;
        std::vector<std::byte> buffer(required);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (!SetupDiGetDeviceInterfaceDetailA(info, &iface, detail, required, nullptr, nullptr)) continue;
        path = detail->DevicePath;
        break;
    }
    SetupDiDestroyDeviceInfoList(info);
    return path;
}

std::unique_ptr<DeviceHandle> openControl()
{
    const auto path = controlPath();
    if (path.empty()) return {};
    auto result = std::make_unique<DeviceHandle>();
    result->file = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (result->file == INVALID_HANDLE_VALUE) return {};
    if (!WinUsb_Initialize(result->file, &result->usb)) return {};
    return result;
}

std::string getMode()
{
    auto dev = openControl();
    if (!dev) return {};
    std::array<UCHAR, 4> bytes{};
    WINUSB_SETUP_PACKET setup{};
    setup.RequestType = 0xC0;
    setup.Request = 0x45;
    setup.Value = 0;
    setup.Index = 0;
    setup.Length = static_cast<USHORT>(bytes.size());
    ULONG transferred = 0;
    if (!WinUsb_ControlTransfer(dev->usb, setup, bytes.data(), static_cast<ULONG>(bytes.size()), &transferred, nullptr))
        return {};
    if (transferred != 3 && transferred != 4) return {};
    std::string mode;
    for (ULONG i = 0; i < transferred; ++i)
    {
        if (i) mode += ':';
        mode += std::to_string(bytes[i]);
    }
    return mode;
}

bool controlTransfer(UCHAR requestType, UCHAR request, USHORT value, USHORT index)
{
    auto dev = openControl();
    if (!dev) return false;
    WINUSB_SETUP_PACKET setup{};
    setup.RequestType = requestType;
    setup.Request = request;
    setup.Value = value;
    setup.Index = index;
    setup.Length = 0;
    ULONG transferred = 0;
    return WinUsb_ControlTransfer(dev->usb, setup, nullptr, 0, &transferred, nullptr) != FALSE;
}

bool setConfiguration(int configuration)
{
    return controlTransfer(0x00, 0x09, static_cast<USHORT>(configuration), 0);
}

bool setMode(int mode)
{
    // macOS Internet Sharing mode request used by the proven iPhoneUsbShare
    // implementation: 0xC0/0x52 GET-like control with the mode in wIndex.
    return controlTransfer(0xC0, 0x52, 0, static_cast<USHORT>(mode));
}

bool restartDevice(const std::string& id)
{
    std::string command = "pnputil.exe /restart-device \"" + id + "\"";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');
    const auto ok = CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}

bool adapterUp()
{
    ULONG size = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, nullptr, &size);
    if (size == 0) return false;
    std::vector<std::byte> buffer(size);
    auto* first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                             nullptr, first, &size) != NO_ERROR)
        return false;
    for (auto* a = first; a; a = a->Next)
    {
        if (a->OperStatus != IfOperStatusUp) continue;
        const auto friendly = lower(a->FriendlyName ? a->FriendlyName : L"");
        const auto description = lower(a->Description ? a->Description : L"");
        if (friendly.find(L"apple") != std::wstring::npos ||
            friendly.find(L"iphone") != std::wstring::npos ||
            friendly.find(L"ipad") != std::wstring::npos ||
            friendly.find(L"ncm") != std::wstring::npos ||
            description.find(L"apple") != std::wstring::npos ||
            description.find(L"ncm") != std::wstring::npos)
            return true;
    }
    return false;
}

bool waitForAdapter(int seconds)
{
    for (int i = 0; i < seconds * 4; ++i)
    {
        if (adapterUp()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return false;
}

} // namespace

struct AppleUsbNetworkTransport::Impl
{
    std::string activeId;
    State current = State::Stopped;
};

AppleUsbNetworkTransport::AppleUsbNetworkTransport() : impl(std::make_unique<Impl>()) {}
AppleUsbNetworkTransport::~AppleUsbNetworkTransport() = default;

std::vector<DeviceNetworkTransport::DeviceInfo> AppleUsbNetworkTransport::enumerateDevices()
{
    std::vector<DeviceInfo> result;
    for (const auto& [id, name] : enumerateApple())
    {
        DeviceInfo info;
        info.id = id;
        info.displayName = name;
        info.kind = Kind::AppleUsb;
        info.state = (id == impl->activeId) ? impl->current : State::Stopped;
        info.networkAvailable = (id == impl->activeId) && adapterUp();
        result.push_back(std::move(info));
    }
    return result;
}

bool AppleUsbNetworkTransport::start(const std::string& deviceId)
{
    impl->activeId = deviceId;
    impl->current = State::Starting;
    DeviceInfo starting{ deviceId, "Apple USB device", Kind::AppleUsb, State::Starting, false };
    notifyState(starting);

    if (!isAppleInstance(deviceId) || controlPath().empty())
    {
        impl->current = State::Error;
        starting.state = State::Error;
        notifyState(starting);
        return false;
    }

    auto mode = getMode();
    if (mode.empty())
    {
        impl->current = State::Error;
        starting.state = State::Error;
        notifyState(starting);
        return false;
    }

    // Preserve the proven iPhoneUsbShare sequence: safe configuration 2,
    // re-enumerate, verify 3:3:3, select configuration 4, then SET_MODE(3).
    if (mode != "5:3:3" && mode != "5:3:3:0")
    {
        if (!setConfiguration(2) || !restartDevice(deviceId))
        {
            impl->current = State::Error;
            starting.state = State::Error;
            notifyState(starting);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
        mode = getMode();
        if (mode != "3:3:3" && mode != "3:3:3:0")
        {
            impl->current = State::Error;
            starting.state = State::Error;
            notifyState(starting);
            return false;
        }
        if (!setConfiguration(4) || !setMode(3))
        {
            setConfiguration(2);
            impl->current = State::Error;
            starting.state = State::Error;
            notifyState(starting);
            return false;
        }
    }

    if (!waitForAdapter(15))
    {
        // Do not install an audio driver, configure ICS, or wait for DHCP.
        // The transport only needs a Windows network interface; MetroNetworkAudio
        // receives UDP through INADDR_ANY once that interface exists.
        impl->current = State::Error;
        starting.state = State::Error;
        notifyState(starting);
        return false;
    }

    impl->current = State::Active;
    DeviceInfo active{ deviceId, "Apple USB device", Kind::AppleUsb, State::Active, true };
    notifyState(active);
    return true;
}

void AppleUsbNetworkTransport::stop(const std::string& deviceId)
{
    if (deviceId != impl->activeId) return;
    impl->current = State::Stopped;
    DeviceInfo stopped{ deviceId, "Apple USB device", Kind::AppleUsb, State::Stopped, false };
    notifyState(stopped);
}

DeviceNetworkTransport::State AppleUsbNetworkTransport::state(const std::string& deviceId) const
{
    return deviceId == impl->activeId ? impl->current : State::Stopped;
}

} // namespace dysekt::network

#else

namespace dysekt::network
{
struct AppleUsbNetworkTransport::Impl {};
AppleUsbNetworkTransport::AppleUsbNetworkTransport() : impl(std::make_unique<Impl>()) {}
AppleUsbNetworkTransport::~AppleUsbNetworkTransport() = default;
std::vector<DeviceNetworkTransport::DeviceInfo> AppleUsbNetworkTransport::enumerateDevices() { return {}; }
bool AppleUsbNetworkTransport::start(const std::string&) { return false; }
void AppleUsbNetworkTransport::stop(const std::string&) {}
DeviceNetworkTransport::State AppleUsbNetworkTransport::state(const std::string&) const { return State::Stopped; }
}

#endif
