#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <iphlpapi.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

#include "AppleUsbNetworkTransport.h"

#if JUCE_WINDOWS
#include <vector>
#include <set>
#include <algorithm>
#include <optional>
#include <sstream>
#include <iomanip>
#endif

namespace
{
#if JUCE_WINDOWS
    constexpr GUID WinUsbInterfaceGuid = { 0x8d4d9c11, 0x3b6b, 0x4d3a, { 0x9b, 0x0b, 0x7e, 0x8b, 0x2e, 0x2e, 0x0c, 0x51 } };

    struct WinUsbHandle
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        WINUSB_INTERFACE_HANDLE usb = nullptr;
        ~WinUsbHandle() { if (usb != nullptr) WinUsb_Free(usb); if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
        explicit operator bool() const noexcept { return usb != nullptr; }
    };

    static std::wstring getDeviceInstanceId(HDEVINFO set, SP_DEVINFO_DATA& data)
    {
        wchar_t buffer[1024] = {};
        DWORD required = 0;
        if (!SetupDiGetDeviceInstanceIdW(set, &data, buffer, ARRAYSIZE(buffer), &required)) return {};
        return buffer;
    }

    static std::wstring findAppleParent()
    {
        GUID empty = {};
        auto set = SetupDiGetClassDevsW(&empty, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return {};
        std::wstring result;
        for (DWORD i = 0; ; ++i)
        {
            SP_DEVINFO_DATA data {};
            data.cbSize = sizeof(data);
            if (!SetupDiEnumDeviceInfo(set, i, &data)) { if (GetLastError() == ERROR_NO_MORE_ITEMS) break; continue; }
            auto id = getDeviceInstanceId(set, data);
            if (id.size() >= 16 && _wcsnicmp(id.c_str(), L"USB\\VID_05AC&PID_", 16) == 0 && id.find(L"&MI_") == std::wstring::npos) { result = id; break; }
        }
        SetupDiDestroyDeviceInfoList(set);
        return result;
    }

    static bool hasAppleInterface(const std::wstring& interfaceId, int number)
    {
        std::wstringstream marker;
        marker << L"&MI_" << std::uppercase << std::hex << std::setw(2) << std::setfill(L'0') << number << L"\\";
        return interfaceId.find(marker.str()) != std::wstring::npos;
    }

    static std::wstring findAppleInterface(int number)
    {
        GUID empty = {};
        auto set = SetupDiGetClassDevsW(&empty, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return {};
        std::wstring result;
        for (DWORD i = 0; ; ++i)
        {
            SP_DEVINFO_DATA data {};
            data.cbSize = sizeof(data);
            if (!SetupDiEnumDeviceInfo(set, i, &data)) { if (GetLastError() == ERROR_NO_MORE_ITEMS) break; continue; }
            auto id = getDeviceInstanceId(set, data);
            if (id.size() >= 16 && _wcsnicmp(id.c_str(), L"USB\\VID_05AC&PID_", 16) == 0 && hasAppleInterface(id, number)) { result = id; break; }
        }
        SetupDiDestroyDeviceInfoList(set);
        return result;
    }

    static std::wstring findWinUsbPath()
    {
        auto set = SetupDiGetClassDevsW(&WinUsbInterfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) return {};
        std::wstring result;
        for (DWORD i = 0; ; ++i)
        {
            SP_DEVICE_INTERFACE_DATA data {};
            data.cbSize = sizeof(data);
            if (!SetupDiEnumDeviceInterfaces(set, nullptr, &WinUsbInterfaceGuid, i, &data)) { if (GetLastError() == ERROR_NO_MORE_ITEMS) break; continue; }
            DWORD required = 0;
            SetupDiGetDeviceInterfaceDetailW(set, &data, nullptr, 0, &required, nullptr);
            if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
            std::vector<BYTE> buffer(required);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (SetupDiGetDeviceInterfaceDetailW(set, &data, detail, required, nullptr, nullptr)) { result = detail->DevicePath; break; }
        }
        SetupDiDestroyDeviceInfoList(set);
        return result;
    }

    static WinUsbHandle openWinUsb()
    {
        WinUsbHandle result;
        const auto path = findWinUsbPath();
        if (path.empty()) return result;
        result.file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (result.file == INVALID_HANDLE_VALUE) return result;
        if (!WinUsb_Initialize(result.file, &result.usb)) { CloseHandle(result.file); result.file = INVALID_HANDLE_VALUE; }
        return result;
    }

    static bool controlTransfer(WINUSB_INTERFACE_HANDLE usb, UCHAR requestType, UCHAR request, USHORT value, USHORT index, std::vector<UCHAR>& buffer)
    {
        WINUSB_SETUP_PACKET setup {};
        setup.RequestType = requestType; setup.Request = request; setup.Value = value; setup.Index = index; setup.Length = static_cast<USHORT>(buffer.size());
        ULONG transferred = 0;
        auto* data = buffer.empty() ? nullptr : buffer.data();
        return WinUsb_ControlTransfer(usb, setup, data, static_cast<ULONG>(buffer.size()), &transferred, nullptr) && transferred == buffer.size();
    }

    static std::optional<std::string> getMode()
    {
        auto handle = openWinUsb(); if (!handle) return std::nullopt;
        std::vector<UCHAR> buffer(4);
        if (!controlTransfer(handle.usb, 0xC0, 0x45, 0, 0, buffer)) return std::nullopt;
        std::ostringstream mode;
        mode << static_cast<int>(buffer[0]) << ':' << static_cast<int>(buffer[1]) << ':' << static_cast<int>(buffer[2]) << ':' << static_cast<int>(buffer[3]);
        return mode.str();
    }

    static bool setConfiguration(int configuration)
    {
        auto handle = openWinUsb(); if (!handle) return false;
        std::vector<UCHAR> empty;
        return controlTransfer(handle.usb, 0x00, 0x09, static_cast<USHORT>(configuration), 0, empty);
    }

    static bool setMode(int mode)
    {
        auto handle = openWinUsb(); if (!handle) return false;
        std::vector<UCHAR> result(1);
        return controlTransfer(handle.usb, 0xC0, 0x52, 0, static_cast<USHORT>(mode), result) && result[0] == 0;
    }

    static std::vector<UCHAR> getDescriptor(WINUSB_INTERFACE_HANDLE usb, UCHAR type, UCHAR index, USHORT length)
    {
        std::vector<UCHAR> buffer(length);
        if (!controlTransfer(usb, 0x80, 0x06, static_cast<USHORT>((type << 8) | index), 0, buffer)) return {};
        return buffer;
    }

    static std::vector<int> findInterruptBackedNcmControlInterfaces()
    {
        auto handle = openWinUsb(); if (!handle) return {};
        auto device = getDescriptor(handle.usb, 0x01, 0, 18); if (device.size() < 18) return {};
        std::vector<int> result;
        const int configurations = device[17];
        for (int configuration = 0; configuration < configurations; ++configuration)
        {
            auto head = getDescriptor(handle.usb, 0x02, static_cast<UCHAR>(configuration), 9); if (head.size() < 9) continue;
            const int total = head[2] | (head[3] << 8); if (total < 9 || total > 8192) continue;
            auto cfg = getDescriptor(handle.usb, 0x02, static_cast<UCHAR>(configuration), static_cast<USHORT>(total));
            for (size_t pos = 0; pos + 9 <= cfg.size(); )
            {
                const auto length = cfg[pos]; const auto type = cfg[pos + 1];
                if (length < 2 || pos + length > cfg.size()) break;
                if (type == 0x04 && length >= 9 && cfg[pos + 5] == 0x02 && cfg[pos + 6] == 0x0D)
                {
                    const int number = cfg[pos + 2], alternate = cfg[pos + 3], endpointCount = cfg[pos + 4];
                    bool interruptIn = false; size_t scan = pos + length; int endpoints = 0;
                    while (scan + 2 <= cfg.size() && endpoints < endpointCount)
                    {
                        const auto subLength = cfg[scan], subType = cfg[scan + 1];
                        if (subLength < 2 || scan + subLength > cfg.size() || subType == 0x04) break;
                        if (subType == 0x05 && subLength >= 7)
                        {
                            ++endpoints; const auto address = cfg[scan + 2], attributes = cfg[scan + 3];
                            if ((attributes & 0x03) == 0x03 && (address & 0x80) != 0) interruptIn = true;
                        }
                        scan += subLength;
                    }
                    if (alternate == 0 && interruptIn && std::find(result.begin(), result.end(), number) == result.end()) result.push_back(number);
                }
                pos += length;
            }
        }
        return result;
    }

    // Include all Ethernet PDOs while the Apple transition is in progress. A
    // newly-created NCM adapter can initially report Dormant/Down before the
    // network stack finishes binding it, so filtering on OperStatus here can
    // miss the adapter entirely.
    static std::set<std::string> ethernetAdapters()
    {
        std::set<std::string> result;
        ULONG size = 0;
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) return result;
        std::vector<unsigned char> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) != NO_ERROR) return result;
        for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next)
            if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD && adapter->AdapterName != nullptr)
                result.emplace(adapter->AdapterName);
        return result;
    }

    static juce::String adapterNameForId(const std::string& id)
    {
        ULONG size = 0;
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) return {};
        std::vector<unsigned char> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) != NO_ERROR) return {};
        for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next)
            if (adapter->AdapterName != nullptr && id == adapter->AdapterName)
            {
                if (adapter->FriendlyName != nullptr) return juce::String(adapter->FriendlyName);
                if (adapter->Description != nullptr) return juce::String(adapter->Description);
            }
        return juce::String(id);
    }

    static bool restartDevice(const std::wstring& instanceId)
    {
        if (instanceId.empty()) return false;
        std::wstring command = L"pnputil.exe /restart-device \"" + instanceId + L"\"";
        STARTUPINFOW si {}; si.cb = sizeof(si); PROCESS_INFORMATION pi {};
        std::vector<wchar_t> commandLine(command.begin(), command.end()); commandLine.push_back(L'\0');
        if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
        WaitForSingleObject(pi.hProcess, 10000); DWORD exitCode = 1; GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return exitCode == 0;
    }

    static bool waitForWinUsb(int timeoutMs)
    {
        const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
        while (GetTickCount64() < deadline) { if (!findWinUsbPath().empty()) return true; Sleep(100); }
        return false;
    }

    static std::optional<std::string> waitForNewEthernet(const std::set<std::string>& before, int timeoutMs)
    {
        const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
        while (GetTickCount64() < deadline)
        {
            auto current = ethernetAdapters();
            for (const auto& id : current) if (!before.contains(id)) return id;
            Sleep(250);
        }
        return std::nullopt;
    }
#endif
}

AppleUsbNetworkTransport::AppleUsbNetworkTransport() = default;
AppleUsbNetworkTransport::~AppleUsbNetworkTransport() { stop(); }

std::vector<DeviceNetworkTransport::Device> AppleUsbNetworkTransport::enumerate()
{
    std::vector<DeviceNetworkTransport::Device> devices;
#if JUCE_WINDOWS
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) return devices;
    std::vector<unsigned char> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) != NO_ERROR) return devices;
    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next)
    {
        if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD || adapter->OperStatus == IfOperStatusDown) continue;
        const auto friendlyName = adapter->FriendlyName != nullptr ? juce::String(adapter->FriendlyName) : juce::String();
        const auto description = adapter->Description != nullptr ? juce::String(adapter->Description) : juce::String();
        const auto text = (friendlyName + " " + description).toLowerCase();
        // Windows binds Apple's CDC-NCM function to its in-box UsbNcm driver.
        // The resulting adapter is commonly named "Ethernet N" while the
        // driver description is "UsbNcm Host Device #N", so looking only for
        // Apple/iPhone/iPad in the friendly name misses the working adapter.
        const bool isAppleNamed = text.contains("apple") || text.contains("iphone") || text.contains("ipad");
        const bool isUsbNcm = text.contains("usbncm") || text.contains("usb ncm");
        if (!isAppleNamed && !isUsbNcm) continue;
        Device device; device.kind = Kind::AppleUsb;
        device.id = juce::String::formatted("%llX", static_cast<unsigned long long>(adapter->Luid.Value));
        device.name = friendlyName.isNotEmpty() ? friendlyName : description; device.interfaceName = device.name; device.connected = true;
        devices.push_back(device);
    }
#endif
    return devices;
}

bool AppleUsbNetworkTransport::start(const juce::String& deviceId)
{
    currentState.store(State::Starting, std::memory_order_release);
#if !JUCE_WINDOWS
    juce::ignoreUnused(deviceId); currentStatus = "Apple USB/NCM transport is Windows-only"; currentState.store(State::Error, std::memory_order_release); return false;
#else
    juce::ignoreUnused(deviceId);
    const auto parent = findAppleParent();
    if (parent.empty()) { currentStatus = "Apple USB device not present"; currentState.store(State::Error, std::memory_order_release); return false; }
    if (findWinUsbPath().empty()) { currentStatus = "Apple WinUSB control interface not present — install WinUsbControl first"; currentState.store(State::Error, std::memory_order_release); return false; }
    if (!setConfiguration(2)) { currentStatus = "Apple USB: SET_CONFIGURATION(2) failed"; currentState.store(State::Error, std::memory_order_release); return false; }
    if (!restartDevice(parent)) { currentStatus = "Apple USB: device restart after safe configuration failed"; currentState.store(State::Error, std::memory_order_release); return false; }
    if (!waitForWinUsb(6000)) { currentStatus = "Apple USB: WinUSB control interface did not return after re-enumeration"; currentState.store(State::Error, std::memory_order_release); return false; }
    const auto mode = getMode();
    if (!mode.has_value()) { currentStatus = "Apple USB: GET_MODE failed"; currentState.store(State::Error, std::memory_order_release); return false; }
    if (*mode != "5:3:3:0" && *mode != "5:3:3") { currentStatus = "Apple USB: unexpected mode " + juce::String(*mode); currentState.store(State::Error, std::memory_order_release); return false; }
    const auto adaptersBeforeNcm = ethernetAdapters();
    if (!setConfiguration(4)) { currentStatus = "Apple USB: SET_CONFIGURATION(4) failed"; currentState.store(State::Error, std::memory_order_release); return false; }
    if (!setMode(3)) { currentStatus = "Apple USB: SET_MODE(3) failed"; currentState.store(State::Error, std::memory_order_release); return false; }
    const auto ncmInterfaces = findInterruptBackedNcmControlInterfaces();
    if (ncmInterfaces.empty())
    {
        bool foundMi02 = false;
        for (int attempt = 0; attempt < 20; ++attempt) { if (!findAppleInterface(2).empty()) { foundMi02 = true; break; } Sleep(250); }
        if (!foundMi02) { currentStatus = "Apple USB: CDC-NCM MI_02 was not published"; currentState.store(State::Error, std::memory_order_release); return false; }
    }
    const auto adapter = waitForNewEthernet(adaptersBeforeNcm, 10000);
    if (!adapter.has_value())
    {
        currentStatus = "Apple USB: NCM selected but no new Windows Ethernet adapter appeared";
        currentState.store(State::Error, std::memory_order_release); return false;
    }
    currentDeviceId = juce::String::formatted("%llX", static_cast<unsigned long long>(std::hash<std::string>{}(*adapter)));
    currentInterfaceName = adapterNameForId(*adapter);
    currentStatus = "USB network interface ready — " + currentInterfaceName;
    currentState.store(State::Connected, std::memory_order_release); return true;
#endif
}

void AppleUsbNetworkTransport::stop()
{
    currentDeviceId.clear(); currentInterfaceName.clear(); currentStatus = "USB network transport stopped"; currentState.store(State::Stopped, std::memory_order_release);
}
juce::String AppleUsbNetworkTransport::status() const
{
    return currentStatus;
}
