#include "AppleUsbNetworkTransport.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>
#include <iphlpapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cwctype>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace
{
std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::wstring lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
    return value;
}

bool isAppleInstance(const std::string& id)
{
    return lower(id).rfind("usb\\vid_05ac&", 0) == 0;
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
    DWORD type = 0, bytes = 0;
    SetupDiGetDeviceRegistryPropertyA(info, &data, property, &type, nullptr, 0, &bytes);
    if (bytes == 0) return {};
    std::vector<BYTE> buffer(bytes + 1, 0);
    if (!SetupDiGetDeviceRegistryPropertyA(info, &data, property, &type, buffer.data(), bytes, nullptr)) return {};
    return reinterpret_cast<const char*>(buffer.data());
}

std::vector<std::pair<std::string, std::string>> enumerateApple()
{
    std::vector<std::pair<std::string, std::string>> result;
    const auto info = SetupDiGetClassDevsA(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
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

std::string controlPath()
{
    GUID guid{};
    if (IIDFromString(L"{8D4D9C11-3B6B-4D3A-9B0B-7E8B2E2E0C51}", &guid) != S_OK) return {};
    const auto info = SetupDiGetClassDevsA(&guid, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) return {};
    std::string path;
    for (DWORD i = 0; ; ++i)
    {
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &guid, i, &iface)) break;
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailA(info, &iface, nullptr, 0, &required, nullptr);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A)) continue;
        std::vector<BYTE> buffer(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        if (SetupDiGetDeviceInterfaceDetailA(info, &iface, detail, required, nullptr, nullptr))
        {
            path = detail->DevicePath;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(info);
    return path;
}

struct DeviceHandle
{
    HANDLE file = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    ~DeviceHandle() { if (usb) WinUsb_Free(usb); if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
};

std::unique_ptr<DeviceHandle> openControl()
{
    const auto path = controlPath();
    if (path.empty()) return {};
    auto result = std::make_unique<DeviceHandle>();
    result->file = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (result->file == INVALID_HANDLE_VALUE || !WinUsb_Initialize(result->file, &result->usb)) return {};
    return result;
}

std::string getMode()
{
    auto dev = openControl();
    if (!dev) return {};
    std::array<UCHAR, 4> data{};
    WINUSB_SETUP_PACKET setup{ 0xC0, 0x45, 0, 0, 4 };
    ULONG transferred = 0;
    if (!WinUsb_ControlTransfer(dev->usb, setup, data.data(), 4, &transferred, nullptr)) return {};
    if (transferred != 3 && transferred != 4) return {};
    std::string mode;
    for (ULONG i = 0; i < transferred; ++i) { if (i) mode += ':'; mode += std::to_string(data[i]); }
    return mode;
}

bool controlTransfer(UCHAR requestType, UCHAR request, USHORT value, USHORT index)
{
    auto dev = openControl();
    if (!dev) return false;
    WINUSB_SETUP_PACKET setup{ requestType, request, value, index, 0 };
    ULONG transferred = 0;
    return WinUsb_ControlTransfer(dev->usb, setup, nullptr, 0, &transferred, nullptr) != FALSE;
}

bool setConfiguration(int configuration)
{
    return controlTransfer(0x00, 0x09, static_cast<USHORT>(configuration), 0);
}

bool setMode(int mode)
{
    return controlTransfer(0xC0, 0x52, 0, static_cast<USHORT>(mode));
}

bool restartDevice(const std::string& id)
{
    std::string command = "pnputil.exe /restart-device \"" + id + "\"";
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<char> cmd(command.begin(), command.end()); cmd.push_back('\0');
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD exitCode = 1; GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return exitCode == 0;
}

bool adapterUp()
{
    ULONG size = 0;
    GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, nullptr, &size);
    if (size == 0) return false;
    std::vector<BYTE> buffer(size);
    auto* first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, nullptr, first, &size) != NO_ERROR) return false;
    for (auto* a = first; a; a = a->Next)
    {
        if (a->OperStatus != IfOperStatusUp) continue;
        const auto friendly = lower(a->FriendlyName ? a->FriendlyName : L"");
        const auto description = lower(a->Description ? a->Description : L"");
        const auto match = [](const std::wstring& s)
        {
            return s.find(L"apple") != std::wstring::npos || s.find(L"iphone") != std::wstring::npos ||
                   s.find(L"ipad") != std::wstring::npos || s.find(L"ncm") != std::wstring::npos;
        };
        if (match(friendly) || match(description)) return true;
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
        DeviceInfo info{ id, name, Kind::AppleUsb, State::Stopped, false };
        if (id == impl->activeId) { info.state = impl->current; info.networkAvailable = adapterUp(); }
        result.push_back(std::move(info));
    }
    return result;
}

bool AppleUsbNetworkTransport::start(const std::string& deviceId)
{
    impl->activeId = deviceId;
    impl->current = State::Starting;
    notifyState(DeviceInfo{ deviceId, "Apple USB device", Kind::AppleUsb, State::Starting, false });

    if (!isAppleInstance(deviceId) || controlPath().empty()) goto fail;

    auto mode = getMode();
    if (mode.empty()) goto fail;

    // Proven transition: safe configuration 2 -> restart -> 3:3:3 ->
    // configuration 4 -> SET_MODE(3).  DYSEKT deliberately does not start
    // Internet Connection Sharing and does not wait for DHCP.
    if (mode != "5:3:3" && mode != "5:3:3:0")
    {
        if (!setConfiguration(2) || !restartDevice(deviceId)) goto fail;
        std::this_thread::sleep_for(std::chrono::seconds(2));
        mode = getMode();
        if (mode != "3:3:3" && mode != "3:3:3:0") goto fail;
        if (!setConfiguration(4) || !setMode(3)) { setConfiguration(2); goto fail; }
    }

    if (!waitForAdapter(15)) goto fail;

    impl->current = State::Active;
    notifyState(DeviceInfo{ deviceId, "Apple USB device", Kind::AppleUsb, State::Active, true });
    return true;

fail:
    impl->current = State::Error;
    notifyState(DeviceInfo{ deviceId, "Apple USB device", Kind::AppleUsb, State::Error, false });
    return false;
}

void AppleUsbNetworkTransport::stop(const std::string& deviceId)
{
    if (deviceId != impl->activeId) return;
    impl->current = State::Stopped;
    notifyState(DeviceInfo{ deviceId, "Apple USB device", Kind::AppleUsb, State::Stopped, false });
}

DeviceNetworkTransport::State AppleUsbNetworkTransport::state(const std::string& deviceId) const
{
    return deviceId == impl->activeId ? impl->current : State::Stopped;
}

#else

struct AppleUsbNetworkTransport::Impl {};
AppleUsbNetworkTransport::AppleUsbNetworkTransport() : impl(std::make_unique<Impl>()) {}
AppleUsbNetworkTransport::~AppleUsbNetworkTransport() = default;
std::vector<DeviceNetworkTransport::DeviceInfo> AppleUsbNetworkTransport::enumerateDevices() { return {}; }
bool AppleUsbNetworkTransport::start(const std::string&) { return false; }
void AppleUsbNetworkTransport::stop(const std::string&) {}
DeviceNetworkTransport::State AppleUsbNetworkTransport::state(const std::string&) const { return State::Stopped; }

#endif
