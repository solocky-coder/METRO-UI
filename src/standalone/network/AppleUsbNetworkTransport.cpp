#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <iphlpapi.h>
#include <devguid.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

#include "AppleUsbNetworkTransport.h"

#if JUCE_WINDOWS
#include <vector>
#include <algorithm>
#include <optional>
#include <sstream>
#include <iomanip>
#include <string>
#include <cwctype>
#include <functional>
#endif

namespace
{
#if JUCE_WINDOWS
    constexpr GUID WinUsbInterfaceGuid = { 0x8d4d9c11, 0x3b6b, 0x4d3a, { 0x9b, 0x0b, 0x7e, 0x8b, 0x2e, 0x2e, 0x0c, 0x51 } };
    constexpr wchar_t UsbEnumRoot[] = L"SYSTEM\\CurrentControlSet\\Enum\\";
    constexpr wchar_t UsbClassRoot[] = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";

    struct WinUsbHandle
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        WINUSB_INTERFACE_HANDLE usb = nullptr;
        ~WinUsbHandle() { if (usb) WinUsb_Free(usb); if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
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
            SP_DEVINFO_DATA data{}; data.cbSize = sizeof(data);
            if (!SetupDiEnumDeviceInfo(set, i, &data)) { if (GetLastError() == ERROR_NO_MORE_ITEMS) break; continue; }
            const auto id = getDeviceInstanceId(set, data);
            if (id.size() >= 16 && _wcsnicmp(id.c_str(), L"USB\\VID_05AC&PID_", 16) == 0 && id.find(L"&MI_") == std::wstring::npos) { result = id; break; }
        }
        SetupDiDestroyDeviceInfoList(set);
        return result;
    }

    static bool hasAppleInterface(const std::wstring& id, int number)
    {
        std::wstringstream marker;
        marker << L"&MI_" << std::uppercase << std::hex << std::setw(2) << std::setfill(L'0') << number << L"\\";
        return id.find(marker.str()) != std::wstring::npos;
    }

    static std::wstring findAppleInterface(int number)
    {
        GUID empty = {};
        auto set = SetupDiGetClassDevsW(&empty, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return {};
        std::wstring result;
        for (DWORD i = 0; ; ++i)
        {
            SP_DEVINFO_DATA data{}; data.cbSize = sizeof(data);
            if (!SetupDiEnumDeviceInfo(set, i, &data)) { if (GetLastError() == ERROR_NO_MORE_ITEMS) break; continue; }
            const auto id = getDeviceInstanceId(set, data);
            if (id.size() >= 16 && _wcsnicmp(id.c_str(), L"USB\\VID_05AC&PID_", 16) == 0 && hasAppleInterface(id, number)) { result = id; break; }
        }
        SetupDiDestroyDeviceInfoList(set);
        return result;
    }

    static bool writeUsbCgpConfiguration(const std::wstring& parent)
    {
        HKEY deviceKey = nullptr;
        const std::wstring devicePath = std::wstring(UsbEnumRoot) + parent;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, devicePath.c_str(), 0, KEY_READ | KEY_WRITE, &deviceKey) != ERROR_SUCCESS) return false;
        DWORD type = 0, bytes = 0;
        wchar_t driver[512] = {};
        bytes = sizeof(driver);
        if (RegQueryValueExW(deviceKey, L"Driver", nullptr, &type, reinterpret_cast<LPBYTE>(driver), &bytes) != ERROR_SUCCESS || type != REG_SZ) { RegCloseKey(deviceKey); return false; }
        HKEY classKey = nullptr;
        const std::wstring classPath = std::wstring(UsbClassRoot) + driver;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, classPath.c_str(), 0, KEY_READ | KEY_WRITE, &classKey) != ERROR_SUCCESS) { RegCloseKey(deviceKey); return false; }
        const BYTE enumeratorClass[] = { 0x02, 0x00, 0x00 };
        const auto enumRc = RegSetValueExW(classKey, L"EnumeratorClass", 0, REG_BINARY, enumeratorClass, sizeof(enumeratorClass));
        RegCloseKey(classKey);
        if (enumRc != ERROR_SUCCESS) { RegCloseKey(deviceKey); return false; }
        DWORD lowerType = 0, lowerBytes = 0;
        if (RegQueryValueExW(deviceKey, L"LowerFilters", nullptr, &lowerType, nullptr, &lowerBytes) == ERROR_SUCCESS && lowerType == REG_MULTI_SZ && lowerBytes > 0)
        {
            std::vector<wchar_t> values(lowerBytes / sizeof(wchar_t));
            if (RegQueryValueExW(deviceKey, L"LowerFilters", nullptr, &lowerType, reinterpret_cast<LPBYTE>(values.data()), &lowerBytes) == ERROR_SUCCESS)
            {
                std::vector<std::wstring> keep;
                for (const wchar_t* p = values.data(); *p; p += wcslen(p) + 1) if (_wcsicmp(p, L"AppleLowerFilter") != 0) keep.emplace_back(p);
                if (keep.empty()) RegDeleteValueW(deviceKey, L"LowerFilters");
                else
                {
                    size_t chars = 1; for (const auto& s : keep) chars += s.size() + 1;
                    std::vector<wchar_t> out(chars, L'\0'); wchar_t* dst = out.data();
                    for (const auto& s : keep) { wcscpy_s(dst, s.size() + 1, s.c_str()); dst += s.size() + 1; }
                    RegSetValueExW(deviceKey, L"LowerFilters", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(out.data()), static_cast<DWORD>(out.size() * sizeof(wchar_t)));
                }
            }
        }
        RegCloseKey(deviceKey);
        return true;
    }

    static bool setUsbConfigurationHints(const std::wstring& parent, DWORD original, DWORD alternate)
    {
        HKEY key = nullptr;
        const auto path = std::wstring(UsbEnumRoot) + parent + L"\\Device Parameters";
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
        const auto a = RegSetValueExW(key, L"OriginalConfigurationValue", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&original), sizeof(original));
        const auto b = RegSetValueExW(key, L"AltConfigurationValue", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&alternate), sizeof(alternate));
        RegCloseKey(key);
        return a == ERROR_SUCCESS && b == ERROR_SUCCESS;
    }

    static bool setWinUsbParameters(const std::wstring& instanceId)
    {
        HKEY key = nullptr;
        const auto path = std::wstring(UsbEnumRoot) + instanceId + L"\\Device Parameters";
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
        const wchar_t* guids[] = { L"{8D4D9C11-3B6B-4D3A-9B0B-7E8B2E2E0C51}", nullptr };
        const auto a = RegSetValueExW(key, L"DeviceInterfaceGUIDs", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(guids), static_cast<DWORD>((wcslen(guids[0]) + 2) * sizeof(wchar_t)));
        const wchar_t compatible[] = L"USB\\MS_COMP_WINUSB";
        const auto b = RegSetValueExW(key, L"WinUsbCompatibleId", 0, REG_SZ, reinterpret_cast<const BYTE*>(compatible), static_cast<DWORD>(sizeof(compatible)));
        RegCloseKey(key);
        return a == ERROR_SUCCESS && b == ERROR_SUCCESS;
    }

    static bool installInboxWinUsb(const std::wstring& instanceId)
    {
        GUID empty = {};
        auto set = SetupDiGetClassDevsW(&empty, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return false;
        SP_DEVINFO_DATA dev{}; dev.cbSize = sizeof(dev);
        if (!SetupDiOpenDeviceInfoW(set, instanceId.c_str(), nullptr, 0, &dev)) { SetupDiDestroyDeviceInfoList(set); return false; }
        SP_DEVINSTALL_PARAMS_W params{}; params.cbSize = sizeof(params);
        if (!SetupDiGetDeviceInstallParamsW(set, &dev, &params)) { SetupDiDestroyDeviceInfoList(set); return false; }
        params.Flags |= DI_ENUMSINGLEINF | DI_QUIETINSTALL;
        params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
        const auto inf = std::wstring(L"C:\\Windows\\INF\\winusb.inf");
        wcsncpy_s(params.DriverPath, ARRAYSIZE(params.DriverPath), inf.c_str(), _TRUNCATE);
        if (!SetupDiSetDeviceInstallParamsW(set, &dev, &params) || !SetupDiBuildDriverInfoList(set, &dev, SPDIT_CLASSDRIVER)) { SetupDiDestroyDeviceInfoList(set); return false; }
        bool installed = false;
        SP_DRVINFO_DATA_W info{}; info.cbSize = sizeof(info);
        if (SetupDiEnumDriverInfoW(set, &dev, SPDIT_CLASSDRIVER, 0, &info))
        {
            if (SetupDiSetSelectedDriverW(set, &dev, &info)) installed = SetupDiCallClassInstaller(DIF_INSTALLDEVICE, set, &dev) != FALSE;
        }
        SetupDiDestroyDriverInfoList(set, &dev, SPDIT_CLASSDRIVER);
        SetupDiDestroyDeviceInfoList(set);
        return installed;
    }

    static std::wstring findWinUsbPathForParent(const std::wstring& parent)
    {
        auto set = SetupDiGetClassDevsW(&WinUsbInterfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) return {};
        std::wstring result;
        for (DWORD i = 0; ; ++i)
        {
            SP_DEVICE_INTERFACE_DATA iface{}; iface.cbSize = sizeof(iface);
            if (!SetupDiEnumDeviceInterfaces(set, nullptr, &WinUsbInterfaceGuid, i, &iface)) { if (GetLastError() == ERROR_NO_MORE_ITEMS) break; continue; }
            DWORD required = 0; SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &required, nullptr); if (!required) continue;
            std::vector<BYTE> buf(required);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data()); detail->cbSize = sizeof(*detail);
            SP_DEVINFO_DATA dev{}; dev.cbSize = sizeof(dev);
            if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, required, nullptr, &dev)) continue;
            const auto id = getDeviceInstanceId(set, dev);
            if (id.rfind(parent + L"&MI_00", 0) == 0 || id.rfind(parent + L"\\", 0) == 0) { result = detail->DevicePath; break; }
        }
        SetupDiDestroyDeviceInfoList(set);
        return result;
    }

    static bool ensureWinUsbControl(const std::wstring& parent)
    {
        if (!findWinUsbPathForParent(parent).empty()) return true;
        const auto mi00 = findAppleInterface(0);
        if (mi00.empty()) return false;
        if (!setWinUsbParameters(mi00)) return false;
        if (!installInboxWinUsb(mi00)) return false;
        if (!CreateProcessW(nullptr, const_cast<wchar_t*>(std::wstring(L"pnputil.exe /restart-device \"") .append(mi00).append(L"\"").c_str()), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, nullptr, nullptr))
        {
            // Fall back to the normal device restart helper below.
        }
        Sleep(1500);
        return !findWinUsbPathForParent(parent).empty();
    }

    static WinUsbHandle openWinUsb(const std::wstring& parent)
    {
        WinUsbHandle result;
        const auto path = findWinUsbPathForParent(parent);
        if (path.empty()) return result;
        result.file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (result.file == INVALID_HANDLE_VALUE) return result;
        if (!WinUsb_Initialize(result.file, &result.usb)) { CloseHandle(result.file); result.file = INVALID_HANDLE_VALUE; }
        return result;
    }

    static bool controlTransfer(WINUSB_INTERFACE_HANDLE usb, UCHAR requestType, UCHAR request, USHORT value, USHORT index, std::vector<UCHAR>& buffer)
    {
        WINUSB_SETUP_PACKET setup{}; setup.RequestType = requestType; setup.Request = request; setup.Value = value; setup.Index = index; setup.Length = static_cast<USHORT>(buffer.size());
        ULONG transferred = 0; auto* data = buffer.empty() ? nullptr : buffer.data();
        return WinUsb_ControlTransfer(usb, setup, data, static_cast<ULONG>(buffer.size()), &transferred, nullptr) && transferred == buffer.size();
    }

    static std::optional<std::string> getMode(const std::wstring& parent)
    {
        auto h = openWinUsb(parent); if (!h) return std::nullopt;
        std::vector<UCHAR> b(4); if (!controlTransfer(h.usb, 0xC0, 0x45, 0, 0, b)) return std::nullopt;
        std::ostringstream s; s << int(b[0]) << ':' << int(b[1]) << ':' << int(b[2]) << ':' << int(b[3]); return s.str();
    }

    static bool setConfiguration(const std::wstring& parent, int configuration)
    {
        auto h = openWinUsb(parent); if (!h) return false; std::vector<UCHAR> b; return controlTransfer(h.usb, 0x00, 0x09, static_cast<USHORT>(configuration), 0, b);
    }

    static bool setMode(const std::wstring& parent, int mode)
    {
        auto h = openWinUsb(parent); if (!h) return false; std::vector<UCHAR> b(1); return controlTransfer(h.usb, 0xC0, 0x52, 0, static_cast<USHORT>(mode), b) && b[0] == 0;
    }

    static std::vector<UCHAR> getDescriptor(WINUSB_INTERFACE_HANDLE usb, UCHAR type, UCHAR index, USHORT length)
    {
        std::vector<UCHAR> b(length); if (!controlTransfer(usb, 0x80, 0x06, static_cast<USHORT>((type << 8) | index), 0, b)) return {}; return b;
    }

    static std::vector<int> findInterruptBackedNcmControlInterfaces(const std::wstring& parent)
    {
        auto h = openWinUsb(parent); if (!h) return {};
        auto device = getDescriptor(h.usb, 0x01, 0, 18); if (device.size() < 18) return {};
        std::vector<int> result;
        for (int cfgIndex = 0; cfgIndex < device[17]; ++cfgIndex)
        {
            auto head = getDescriptor(h.usb, 0x02, static_cast<UCHAR>(cfgIndex), 9); if (head.size() < 9) continue;
            const int total = head[2] | (head[3] << 8); if (total < 9 || total > 8192) continue;
            auto cfg = getDescriptor(h.usb, 0x02, static_cast<UCHAR>(cfgIndex), static_cast<USHORT>(total));
            for (size_t pos = 0; pos + 9 <= cfg.size(); )
            {
                const auto len = cfg[pos], type = cfg[pos + 1]; if (len < 2 || pos + len > cfg.size()) break;
                if (type == 0x04 && len >= 9 && cfg[pos + 5] == 0x02 && cfg[pos + 6] == 0x0D)
                {
                    const int number = cfg[pos + 2], alternate = cfg[pos + 3], endpointCount = cfg[pos + 4];
                    bool interruptIn = false; size_t scan = pos + len; int endpoints = 0;
                    while (scan + 2 <= cfg.size() && endpoints < endpointCount)
                    {
                        const auto sl = cfg[scan], st = cfg[scan + 1]; if (sl < 2 || scan + sl > cfg.size() || st == 0x04) break;
                        if (st == 0x05 && sl >= 7) { ++endpoints; const auto addr = cfg[scan + 2], attr = cfg[scan + 3]; if ((attr & 0x03) == 0x03 && (addr & 0x80)) interruptIn = true; }
                        scan += sl;
                    }
                    if (alternate == 0 && interruptIn && std::find(result.begin(), result.end(), number) == result.end()) result.push_back(number);
                }
                pos += len;
            }
        }
        return result;
    }

    static bool selectCompatibleDriver(const std::wstring& instanceId, const std::wstring& token)
    {
        GUID empty = {};
        auto set = SetupDiGetClassDevsW(&empty, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return false;
        SP_DEVINFO_DATA dev{}; dev.cbSize = sizeof(dev);
        if (!SetupDiOpenDeviceInfoW(set, instanceId.c_str(), nullptr, 0, &dev)) { SetupDiDestroyDeviceInfoList(set); return false; }
        if (!SetupDiBuildDriverInfoList(set, &dev, SPDIT_COMPATDRIVER)) { SetupDiDestroyDeviceInfoList(set); return false; }
        SP_DRVINFO_DATA_W info{}; info.cbSize = sizeof(info); bool selected = false;
        for (DWORD i = 0; SetupDiEnumDriverInfoW(set, &dev, SPDIT_COMPATDRIVER, i, &info); ++i)
        {
            const std::wstring all = std::wstring(info.Description) + L" " + info.ProviderName;
            if (std::search(all.begin(), all.end(), token.begin(), token.end(), [](wchar_t a, wchar_t b){ return towlower(a) == towlower(b); }) != all.end())
            {
                if (SetupDiSetSelectedDriverW(set, &dev, &info) && SetupDiCallClassInstaller(DIF_INSTALLDEVICE, set, &dev)) { selected = true; break; }
            }
        }
        SetupDiDestroyDriverInfoList(set, &dev, SPDIT_COMPATDRIVER); SetupDiDestroyDeviceInfoList(set); return selected;
    }

    static std::optional<std::string> findUsbNcmEthernet()
    {
        ULONG size = 0;
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) return std::nullopt;
        std::vector<unsigned char> buffer(size); auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) != NO_ERROR) return std::nullopt;
        for (auto* a = adapters; a; a = a->Next)
        {
            if (a->IfType != IF_TYPE_ETHERNET_CSMACD || !a->AdapterName) continue;
            const auto friendly = a->FriendlyName ? juce::String(a->FriendlyName) : juce::String();
            const auto desc = a->Description ? juce::String(a->Description) : juce::String();
            const auto text = (friendly + " " + desc).toLowerCase();
            if (text.contains("usbncm") || text.contains("usb ncm")) return std::string(a->AdapterName);
        }
        return std::nullopt;
    }

    static juce::String adapterNameForId(const std::string& id)
    {
        ULONG size = 0; if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) return {};
        std::vector<unsigned char> buffer(size); auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, a, &size) != NO_ERROR) return {};
        for (auto* p = a; p; p = p->Next) if (p->AdapterName && id == p->AdapterName) { if (p->FriendlyName) return juce::String(p->FriendlyName); if (p->Description) return juce::String(p->Description); }
        return juce::String(id);
    }

    static bool restartDevice(const std::wstring& instanceId)
    {
        if (instanceId.empty()) return false;
        std::wstring cmd = L"pnputil.exe /restart-device \"" + instanceId + L"\"";
        STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
        std::vector<wchar_t> line(cmd.begin(), cmd.end()); line.push_back(L'\0');
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
        WaitForSingleObject(pi.hProcess, 10000); DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return code == 0;
    }

    static bool waitForAppleParent(int timeoutMs)
    {
        const auto deadline = GetTickCount64() + timeoutMs; while (GetTickCount64() < deadline) { if (!findAppleParent().empty()) return true; Sleep(150); } return false;
    }

    static bool waitForWinUsb(const std::wstring& parent, int timeoutMs)
    {
        const auto deadline = GetTickCount64() + timeoutMs; while (GetTickCount64() < deadline) { if (!findWinUsbPathForParent(parent).empty()) return true; Sleep(150); } return false;
    }

    static std::optional<std::string> waitForUsbNcmEthernet(int timeoutMs)
    {
        const auto deadline = GetTickCount64() + timeoutMs; while (GetTickCount64() < deadline) { if (const auto ncm = findUsbNcmEthernet()) return ncm; Sleep(250); } return std::nullopt;
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
    std::vector<unsigned char> buffer(size); auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) != NO_ERROR) return devices;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next)
    {
        if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD || adapter->OperStatus == IfOperStatusDown) continue;
        const auto friendly = adapter->FriendlyName ? juce::String(adapter->FriendlyName) : juce::String();
        const auto description = adapter->Description ? juce::String(adapter->Description) : juce::String();
        const auto text = (friendly + " " + description).toLowerCase();
        if (!text.contains("apple") && !text.contains("iphone") && !text.contains("ipad") && !text.contains("usbncm") && !text.contains("usb ncm")) continue;
        Device d; d.kind = Kind::AppleUsb; d.id = juce::String::formatted("%llX", static_cast<unsigned long long>(adapter->Luid.Value)); d.name = friendly.isNotEmpty() ? friendly : description; d.interfaceName = d.name; d.connected = true; devices.push_back(d);
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
    auto parent = findAppleParent();
    if (parent.empty()) { currentStatus = "Apple USB device not present"; currentState.store(State::Error, std::memory_order_release); return false; }
    if (!writeUsbCgpConfiguration(parent)) { currentStatus = "Apple USB: cannot configure usbccgp/AppleLowerFilter (run DYSEKT as Administrator)"; currentState.store(State::Error, std::memory_order_release); return false; }
    if (!setUsbConfigurationHints(parent, 2, 0)) { currentStatus = "Apple USB: cannot write safe usbccgp configuration hints"; currentState.store(State::Error, std::memory_order_release); return false; }

    // iPhoneUsbShare does not require a pre-installed third-party WinUSB package.
    // It binds Microsoft's inbox winusb.inf to the existing Apple MI_00 PDO,
    // publishes the same application interface GUID, then restarts MI_00.
    if (findWinUsbPathForParent(parent).empty())
    {
        const auto mi00 = findAppleInterface(0);
        if (mi00.empty() || !setWinUsbParameters(mi00) || !installInboxWinUsb(mi00) || !restartDevice(mi00))
        {
            currentStatus = "Apple USB: could not bind Microsoft's inbox WinUSB driver to MI_00 (run DYSEKT as Administrator)";
            currentState.store(State::Error, std::memory_order_release); return false;
        }
        if (!waitForWinUsb(parent, 10000))
        {
            currentStatus = "Apple USB: WinUSB control interface did not appear after MI_00 installation";
            currentState.store(State::Error, std::memory_order_release); return false;
        }
    }

    if (!setConfiguration(parent, 2)) { currentStatus = "Apple USB: SET_CONFIGURATION(2) failed"; currentState.store(State::Error, std::memory_order_release); return false; }
    if (!restartDevice(parent) || !waitForAppleParent(8000)) { currentStatus = "Apple USB: device restart after safe configuration failed"; currentState.store(State::Error, std::memory_order_release); return false; }
    parent = findAppleParent();
    writeUsbCgpConfiguration(parent); setUsbConfigurationHints(parent, 2, 0);
    if (!waitForWinUsb(parent, 10000)) { currentStatus = "Apple USB: WinUSB control interface did not return after re-enumeration"; currentState.store(State::Error, std::memory_order_release); return false; }

    auto mode = getMode(parent);
    if (!mode) { currentStatus = "Apple USB: GET_MODE failed"; currentState.store(State::Error, std::memory_order_release); return false; }
    if (*mode != "5:3:3:0" && *mode != "5:3:3")
    {
        if (*mode != "3:3:3:0" && *mode != "3:3:3") { currentStatus = "Apple USB: unexpected mode " + juce::String(*mode); currentState.store(State::Error, std::memory_order_release); return false; }
        if (!setUsbConfigurationHints(parent, 4, 2) || !setConfiguration(parent, 4)) { currentStatus = "Apple USB: CDC-NCM configuration 4 failed"; currentState.store(State::Error, std::memory_order_release); return false; }
        if (!setMode(parent, 3)) { currentStatus = "Apple USB: SET_MODE(3) failed"; currentState.store(State::Error, std::memory_order_release); return false; }
    }

    std::vector<int> ncmInterfaces;
    for (int attempt = 0; attempt < 40 && ncmInterfaces.empty(); ++attempt)
    {
        parent = findAppleParent();
        if (!parent.empty() && waitForWinUsb(parent, 500)) ncmInterfaces = findInterruptBackedNcmControlInterfaces(parent);
        if (ncmInterfaces.empty()) Sleep(250);
    }
    if (ncmInterfaces.empty()) { currentStatus = "Apple USB: CDC-NCM control interface was not published"; currentState.store(State::Error, std::memory_order_release); return false; }

    bool bound = false;
    for (const auto number : ncmInterfaces)
    {
        const auto child = findAppleInterface(number);
        if (child.empty()) continue;
        if (selectCompatibleDriver(child, L"UsbNcm") || selectCompatibleDriver(child, L"USB NCM")) { bound = true; break; }
    }
    if (!bound) { currentStatus = "Apple USB: CDC-NCM was detected, but Windows did not select the UsbNcm driver"; currentState.store(State::Error, std::memory_order_release); return false; }

    const auto adapter = waitForUsbNcmEthernet(15000);
    if (!adapter) { currentStatus = "Apple USB: UsbNcm was selected but no Windows USB Ethernet adapter appeared"; currentState.store(State::Error, std::memory_order_release); return false; }
    currentDeviceId = juce::String::formatted("%llX", static_cast<unsigned long long>(std::hash<std::string>{}(*adapter)));
    currentInterfaceName = adapterNameForId(*adapter);
    currentStatus = "USB network interface ready — " + currentInterfaceName;
    currentState.store(State::Connected, std::memory_order_release);
    return true;
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
