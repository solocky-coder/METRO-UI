#include "AppleUsbShareEngine.h"

#if JUCE_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <oaidl.h>
#include <oleauto.h>
#include <shellapi.h>
#include <devguid.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#endif

namespace
{
#if JUCE_WINDOWS
    constexpr GUID WinUsbInterfaceGuid = { 0x8d4d9c11, 0x3b6b, 0x4d3a, { 0x9b, 0x0b, 0x7e, 0x8b, 0x2e, 0x2e, 0x0c, 0x51 } };
    constexpr wchar_t UsbEnumRoot[] = L"SYSTEM\\CurrentControlSet\\Enum\\";
    constexpr wchar_t UsbClassRoot[] = L"SYSTEM\\CurrentControlSet\\Control\\Class\\";
    constexpr wchar_t ApplePrefix[] = L"USB\\VID_05AC&PID_";
    constexpr wchar_t WinUsbGuidText[] = L"{8D4D9C11-3B6B-4D3A-9B0B-7E8B2E2E0C51}";
    constexpr DWORD InstallFlagForce = 0x00000001;
    constexpr DWORD CmLocateDevNodeNormal = 0;
    constexpr DWORD CmReenumerateSynchronous = 1;
    constexpr DWORD CrSuccess = 0;
    constexpr DWORD CrNoSuchDevNode = 0x0000000D;

    const char* parentInfText = R"INF(; iPhoneUsbShare Apple USB composite-parent configuration override
; This is the exact reference package content.
[Version]
Signature   = "$Windows NT$"
Class       = USB
ClassGUID   = {36FC9E60-C465-11CF-8056-444553540000}
Provider    = %ManufacturerName%
CatalogFile = AppleUsbCompositeConfiguration.cat
DriverVer   = 09/15/2026,1.0.0.0
PnpLockdown = 1

[Manufacturer]
%ManufacturerName% = Standard,NTamd64

[Standard.NTamd64]
%DeviceName% = Composite_Install, USB\VID_05AC&PID_12AB

[Composite_Install]
Include = usb.inf
Needs   = Composite.Dev.NT

[Composite_Install.Services]
Include = usb.inf
Needs   = Composite.Dev.NT.Services

[Composite_Install.HW]
AddReg = Composite_Install_AddReg

[Composite_Install_AddReg]
HKR,,OriginalConfigurationValue,0x00010001,5

[Strings]
ManufacturerName = "iPhoneUsbShare"
DeviceName = "iPhoneUsbShare Apple USB Composite Configuration"
)INF";

    const char* controlInfText = R"INF(; iPhoneUsbShare WinUSB control-interface driver package
; This is the exact reference package content.
[Version]
Signature   = "$Windows NT$"
Class       = USBDevice
ClassGUID   = {88BAE032-5A81-49F0-BC3D-A4FF138216D6}
Provider    = %ManufacturerName%
CatalogFile = WinUsbControl.cat
DriverVer   = 09/15/2026,1.0.0.1
PnpLockdown = 1

[Manufacturer]
%ManufacturerName% = Standard,NTamd64

[Standard.NTamd64]
%DeviceName% = USB_Install, USB\VID_05AC&PID_12AB&REV_0503&MI_00
%DeviceName% = USB_Install, USB\VID_05AC&PID_12AB&MI_00

[USB_Install]
Include = winusb.inf
Needs   = WINUSB.NT

[USB_Install.Services]
Include = winusb.inf
Needs   = WINUSB.NT.Services

[USB_Install.HW]
AddReg = USB_Install_AddReg

[USB_Install_AddReg]
HKR,,DeviceInterfaceGUIDs,0x10000,"{8D4D9C11-3B6B-4D3A-9B0B-7E8B2E2E0C51}"

[Strings]
ManufacturerName = "iPhoneUsbShare"
DeviceName = "iPhoneUsbShare Apple USB Control Interface"
)INF";

    struct WinUsbHandle
    {
        HANDLE file = INVALID_HANDLE_VALUE;
        WINUSB_INTERFACE_HANDLE usb = nullptr;
        ~WinUsbHandle() { if (usb) WinUsb_Free(usb); if (file != INVALID_HANDLE_VALUE) CloseHandle(file); }
        explicit operator bool() const noexcept { return usb != nullptr; }
    };

    struct AdapterInfo
    {
        std::string id;
        juce::String friendly;
        juce::String description;
    };

    struct ProcessResult { DWORD exitCode = 1; juce::String output, error; };

    static std::wstring deviceInstanceId(HDEVINFO set, SP_DEVINFO_DATA& data)
    {
        wchar_t buffer[1024] = {};
        DWORD required = 0;
        return SetupDiGetDeviceInstanceIdW(set, &data, buffer, ARRAYSIZE(buffer), &required) ? std::wstring(buffer) : std::wstring();
    }

    static std::wstring findAppleParent()
    {
        GUID empty{};
        auto set = SetupDiGetClassDevsW(&empty, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return {};
        std::wstring result;
        for (DWORD i = 0; ; ++i)
        {
            SP_DEVINFO_DATA data{}; data.cbSize = sizeof(data);
            if (!SetupDiEnumDeviceInfo(set, i, &data)) { if (GetLastError() == ERROR_NO_MORE_ITEMS) break; continue; }
            const auto id = deviceInstanceId(set, data);
            if (id.size() >= 16 && _wcsnicmp(id.c_str(), ApplePrefix, 16) == 0 && id.find(L"&MI_") == std::wstring::npos) { result = id; break; }
        }
        SetupDiDestroyDeviceInfoList(set);
        return result;
    }

    static bool isAppleChild(const std::wstring& id, const std::wstring& parent)
    {
        return id.rfind(parent + L"&MI_", 0) == 0;
    }

    static std::wstring findAppleInterface(const std::wstring& parent, int number)
    {
        std::wstringstream marker;
        marker << L"&MI_" << std::uppercase << std::hex << std::setw(2) << std::setfill(L'0') << number << L"\\";
        GUID empty{};
        auto set = SetupDiGetClassDevsW(&empty, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return {};
        std::wstring result;
        for (DWORD i = 0; ; ++i)
        {
            SP_DEVINFO_DATA data{}; data.cbSize = sizeof(data);
            if (!SetupDiEnumDeviceInfo(set, i, &data)) { if (GetLastError() == ERROR_NO_MORE_ITEMS) break; continue; }
            const auto id = deviceInstanceId(set, data);
            if (isAppleChild(id, parent) && id.find(marker.str()) != std::wstring::npos) { result = id; break; }
        }
        SetupDiDestroyDeviceInfoList(set);
        return result;
    }

    static std::vector<std::wstring> appleChildren(const std::wstring& parent)
    {
        std::vector<std::wstring> out;
        GUID empty{};
        auto set = SetupDiGetClassDevsW(&empty, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return out;
        for (DWORD i = 0; ; ++i)
        {
            SP_DEVINFO_DATA data{}; data.cbSize = sizeof(data);
            if (!SetupDiEnumDeviceInfo(set, i, &data)) { if (GetLastError() == ERROR_NO_MORE_ITEMS) break; continue; }
            const auto id = deviceInstanceId(set, data);
            if (isAppleChild(id, parent)) out.push_back(id);
        }
        SetupDiDestroyDeviceInfoList(set);
        return out;
    }

    static bool writeMultiSz(HKEY key, const wchar_t* name, const std::vector<std::wstring>& values)
    {
        size_t chars = 1;
        for (const auto& value : values) chars += value.size() + 1;
        std::vector<wchar_t> data(chars, L'\0');
        wchar_t* dst = data.data();
        for (const auto& value : values) { wcscpy_s(dst, value.size() + 1, value.c_str()); dst += value.size() + 1; }
        return RegSetValueExW(key, name, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(data.data()), static_cast<DWORD>(data.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
    }

    static bool removeFilter(const std::wstring& parent, const wchar_t* filterName, const wchar_t* valueName)
    {
        HKEY key = nullptr;
        const auto path = std::wstring(UsbEnumRoot) + parent;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS) return false;
        DWORD type = 0, bytes = 0;
        bool changed = false;
        if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS && type == REG_MULTI_SZ && bytes)
        {
            std::vector<wchar_t> data(bytes / sizeof(wchar_t));
            if (RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(data.data()), &bytes) == ERROR_SUCCESS)
            {
                std::vector<std::wstring> keep;
                for (const wchar_t* p = data.data(); *p; p += wcslen(p) + 1)
                    if (_wcsicmp(p, filterName) != 0) keep.emplace_back(p); else changed = true;
                if (changed)
                {
                    if (keep.empty()) RegDeleteValueW(key, valueName);
                    else writeMultiSz(key, valueName, keep);
                }
            }
        }
        RegCloseKey(key);
        return changed;
    }

    static bool configureUsbCgp(const std::wstring& parent)
    {
        HKEY deviceKey = nullptr;
        const auto devicePath = std::wstring(UsbEnumRoot) + parent;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, devicePath.c_str(), 0, KEY_READ | KEY_WRITE, &deviceKey) != ERROR_SUCCESS) return false;
        wchar_t driver[512] = {};
        DWORD type = 0, bytes = sizeof(driver);
        if (RegQueryValueExW(deviceKey, L"Driver", nullptr, &type, reinterpret_cast<LPBYTE>(driver), &bytes) != ERROR_SUCCESS || type != REG_SZ)
        { RegCloseKey(deviceKey); return false; }
        HKEY classKey = nullptr;
        const auto classPath = std::wstring(UsbClassRoot) + driver;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, classPath.c_str(), 0, KEY_READ | KEY_WRITE, &classKey) != ERROR_SUCCESS)
        { RegCloseKey(deviceKey); return false; }
        const BYTE enumeratorClass[] = { 0x02, 0x00, 0x00 };
        const bool enumOk = RegSetValueExW(classKey, L"EnumeratorClass", 0, REG_BINARY, enumeratorClass, sizeof(enumeratorClass)) == ERROR_SUCCESS;
        RegCloseKey(classKey);
        const bool filterChanged = removeFilter(parent, L"AppleLowerFilter", L"LowerFilters");
        RegCloseKey(deviceKey);
        return enumOk && (filterChanged || true);
    }

    static bool removeLegacyLibUsb(const std::wstring& parent)
    {
        removeFilter(parent, L"libusb0", L"UpperFilters");
        return true;
    }

    static bool setConfigHints(const std::wstring& parent, DWORD original, DWORD alternate)
    {
        HKEY key = nullptr;
        const auto path = std::wstring(UsbEnumRoot) + parent + L"\\Device Parameters";
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
        const bool a = RegSetValueExW(key, L"OriginalConfigurationValue", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&original), sizeof(original)) == ERROR_SUCCESS;
        const bool b = RegSetValueExW(key, L"AltConfigurationValue", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&alternate), sizeof(alternate)) == ERROR_SUCCESS;
        RegCloseKey(key);
        return a && b;
    }

    static bool setWinUsbParameters(const std::wstring& instanceId)
    {
        HKEY key = nullptr;
        const auto path = std::wstring(UsbEnumRoot) + instanceId + L"\\Device Parameters";
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
        const bool a = writeMultiSz(key, L"DeviceInterfaceGUIDs", { WinUsbGuidText });
        const wchar_t compatible[] = L"USB\\MS_COMP_WINUSB";
        const bool b = RegSetValueExW(key, L"WinUsbCompatibleId", 0, REG_SZ, reinterpret_cast<const BYTE*>(compatible), sizeof(compatible)) == ERROR_SUCCESS;
        RegCloseKey(key);
        return a && b;
    }

    static ProcessResult runProcess(const std::wstring& commandLine, DWORD timeoutMs = 15000)
    {
        ProcessResult result;
        STARTUPINFOW si{}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::vector<wchar_t> line(commandLine.begin(), commandLine.end()); line.push_back(L'\0');
        if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        { result.error = "CreateProcess failed: " + juce::String((int)GetLastError()); return result; }
        WaitForSingleObject(pi.hProcess, timeoutMs);
        DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); result.exitCode = code;
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return result;
    }

    static bool restartDevice(const std::wstring& instanceId)
    {
        if (instanceId.empty()) return false;
        const auto r = runProcess(L"pnputil.exe /restart-device \"" + instanceId + L"\"");
        return r.exitCode == 0;
    }

    static bool removeDeviceSubtree(const std::wstring& instanceId)
    {
        const auto r = runProcess(L"pnputil.exe /remove-device \"" + instanceId + L"\" /subtree");
        return r.exitCode == 0;
    }

    static void scanDevices()
    {
        runProcess(L"pnputil.exe /scan-devices");
    }

    static bool waitForParent(int timeoutMs)
    {
        const auto deadline = GetTickCount64() + timeoutMs;
        while (GetTickCount64() < deadline) { if (!findAppleParent().empty()) return true; Sleep(150); }
        return !findAppleParent().empty();
    }

    static bool waitForMi00(const std::wstring& parent, int timeoutMs)
    {
        const auto deadline = GetTickCount64() + timeoutMs;
        while (GetTickCount64() < deadline) { if (!findAppleInterface(parent, 0).empty()) return true; Sleep(200); }
        return !findAppleInterface(parent, 0).empty();
    }

    static bool cfgMgrReenumerate(const std::wstring& id)
    {
        DEVINST devInst = 0;
        if (CM_Locate_DevNodeW(&devInst, const_cast<wchar_t*>(id.c_str()), CmLocateDevNodeNormal) != CrSuccess) return false;
        return CM_Reenumerate_DevNode(devInst, CmReenumerateSynchronous) == CrSuccess;
    }

    static std::optional<unsigned long> cfgMgrProblem(const std::wstring& id)
    {
        DEVINST devInst = 0;
        if (CM_Locate_DevNodeW(&devInst, const_cast<wchar_t*>(id.c_str()), CmLocateDevNodeNormal) != CrSuccess) return std::nullopt;
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, devInst, 0) != CrSuccess) return std::nullopt;
        return (status & DN_HAS_PROBLEM) ? std::optional<unsigned long>(problem) : std::optional<unsigned long>(0);
    }

    static bool startupRecovery(const std::function<void(const juce::String&)>& log)
    {
        auto parent = findAppleParent();
        if (parent.empty()) return true;
        if (!findAppleInterface(parent, 0).empty()) return true;

        if (const auto problem = cfgMgrProblem(parent); problem && *problem == 10)
        {
            log("Startup recovery: Apple composite parent is Code 10 with MI_00 absent; rebuilding the usbccgp device instance once.");
            removeDeviceSubtree(parent);
            Sleep(1200);
            scanDevices();
            for (int i = 0; i < 10; ++i)
            {
                Sleep(1000);
                parent = findAppleParent();
                if (!parent.empty() && !findAppleInterface(parent, 0).empty())
                { log("Startup recovery: usbccgp Code 10 recovery restored MI_00."); return true; }
            }
        }

        parent = findAppleParent();
        if (parent.empty()) return false;
        log("Startup recovery: MI_00 missing; requesting targeted ConfigMgr re-enumeration.");
        cfgMgrReenumerate(parent);
        Sleep(1800);
        scanDevices();
        Sleep(1800);
        parent = findAppleParent();
        if (!parent.empty() && !findAppleInterface(parent, 0).empty()) return true;

        log("Startup recovery: targeted re-enumeration did not restore MI_00; trying one controlled parent restart.");
        restartDevice(parent);
        Sleep(3000);
        parent = findAppleParent();
        if (!parent.empty() && !findAppleInterface(parent, 0).empty()) return true;
        log("Startup recovery: MI_00 remains absent; no further device-stack mutations will be attempted.");
        return false;
    }

    static bool writeTextFile(const juce::File& file, const char* text)
    {
        file.getParentDirectory().createDirectory();
        return file.replaceWithText(text, false, false, "\n");
    }

    static juce::File driverPackageDir()
    {
        const auto app = juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory();
        const std::array<juce::File, 4> candidates = {
            app.getChildFile("AppleUsbShare").getChildFile("Driver"),
            app.getChildFile("Driver").getChildFile("AppleUsbShare"),
            app.getChildFile("Driver"),
            juce::File::getSpecialLocation(juce::File::commonApplicationDataDirectory).getChildFile("DYSEKT").getChildFile("AppleUsbShare").getChildFile("Driver")
        };
        for (const auto& c : candidates)
            if (c.getChildFile("AppleUsbCompositeConfiguration.inf").existsAsFile() || c.getChildFile("WinUsbControl.inf").existsAsFile()) return c;
        const auto cache = candidates.back();
        writeTextFile(cache.getChildFile("AppleUsbCompositeConfiguration.inf"), parentInfText);
        writeTextFile(cache.getChildFile("WinUsbControl.inf"), controlInfText);
        return cache;
    }

    static bool hasSignedPackage(const juce::File& dir)
    {
        return dir.getChildFile("AppleUsbCompositeConfiguration.inf").existsAsFile()
            && dir.getChildFile("AppleUsbCompositeConfiguration.cat").existsAsFile()
            && dir.getChildFile("WinUsbControl.inf").existsAsFile()
            && dir.getChildFile("WinUsbControl.cat").existsAsFile();
    }

    static bool forceDriverPackage(const juce::File& dir, const std::wstring& parent, const std::wstring& mi00,
                                   const std::function<void(const juce::String&)>& log)
    {
        if (!hasSignedPackage(dir))
        {
            log("Driver package migration: reference INFs are embedded, but signed CAT files are not present; using Microsoft's inbox WinUSB fallback.");
            return false;
        }

        const auto parentInf = dir.getChildFile("AppleUsbCompositeConfiguration.inf");
        const auto controlInf = dir.getChildFile("WinUsbControl.inf");
        runProcess(L"pnputil.exe /add-driver \"" + parentInf.getFullPathName().toWideCharPointer() + L"\" /install");
        BOOL reboot = FALSE;
        const bool parentOk = UpdateDriverForPlugAndPlayDevicesW(nullptr, parent.c_str(), parentInf.getFullPathName().toWideCharPointer(), InstallFlagForce, &reboot) != FALSE;
        if (!parentOk) log("Driver package migration: composite-parent force update failed, Win32Error=" + juce::String((int)GetLastError()));
        else log("Driver package migration: composite-parent package forced onto Apple USB parent.");
        if (!parentOk) return false;

        restartDevice(parent);
        Sleep(3000);
        const auto newParent = findAppleParent();
        const auto child = newParent.empty() ? mi00 : findAppleInterface(newParent, 0);
        if (child.empty()) return false;
        runProcess(L"pnputil.exe /add-driver \"" + controlInf.getFullPathName().toWideCharPointer() + L"\" /install");
        reboot = FALSE;
        const bool childOk = UpdateDriverForPlugAndPlayDevicesW(nullptr, L"USB\\VID_05AC&PID_12AB&MI_00", controlInf.getFullPathName().toWideCharPointer(), InstallFlagForce, &reboot) != FALSE;
        if (!childOk) log("Driver package migration: MI_00 WinUSB force update failed, Win32Error=" + juce::String((int)GetLastError()));
        else log("Driver package migration: WinUsbControl.inf forced onto MI_00.");
        if (childOk) restartDevice(child);
        return childOk;
    }

    static bool installInboxWinUsb(const std::wstring& instanceId, const std::function<void(const juce::String&)>& log)
    {
        GUID empty{};
        auto set = SetupDiGetClassDevsW(&empty, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return false;
        SP_DEVINFO_DATA dev{}; dev.cbSize = sizeof(dev);
        if (!SetupDiOpenDeviceInfoW(set, instanceId.c_str(), nullptr, 0, &dev)) { SetupDiDestroyDeviceInfoList(set); return false; }
        SP_DEVINSTALL_PARAMS_W params{}; params.cbSize = sizeof(params);
        if (!SetupDiGetDeviceInstallParamsW(set, &dev, &params)) { SetupDiDestroyDeviceInfoList(set); return false; }
        params.Flags |= DI_ENUMSINGLEINF | DI_QUIETINSTALL;
        params.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
        wchar_t windowsDir[MAX_PATH] = {};
        if (!GetWindowsDirectoryW(windowsDir, ARRAYSIZE(windowsDir))) { SetupDiDestroyDeviceInfoList(set); return false; }
        const auto inf = std::wstring(windowsDir) + L"\\INF\\winusb.inf";
        wcsncpy_s(params.DriverPath, ARRAYSIZE(params.DriverPath), inf.c_str(), _TRUNCATE);
        if (!SetupDiSetDeviceInstallParamsW(set, &dev, &params) || !SetupDiBuildDriverInfoList(set, &dev, SPDIT_CLASSDRIVER))
        { SetupDiDestroyDeviceInfoList(set); return false; }
        bool installed = false;
        for (DWORD i = 0; ; ++i)
        {
            SP_DRVINFO_DATA_W info{}; info.cbSize = sizeof(info);
            if (!SetupDiEnumDriverInfoW(set, &dev, SPDIT_CLASSDRIVER, i, &info)) break;
            if (_wcsicmp(info.ProviderName, L"Microsoft") != 0 && wcsstr(info.Description, L"WinUSB") == nullptr) continue;
            if (SetupDiSetSelectedDriverW(set, &dev, &info) && DiInstallDevice(nullptr, set, &dev, &info, 0, nullptr))
            { installed = true; break; }
        }
        SetupDiDestroyDriverInfoList(set, &dev, SPDIT_CLASSDRIVER);
        SetupDiDestroyDeviceInfoList(set);
        if (installed) log("WinUSB migration: Microsoft inbox WinUSB was installed on MI_00.");
        else log("WinUSB migration: Microsoft inbox WinUSB driver selection failed, Win32Error=" + juce::String((int)GetLastError()));
        return installed;
    }

    static std::wstring winUsbPathForParent(const std::wstring& parent)
    {
        auto set = SetupDiGetClassDevsW(&WinUsbInterfaceGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (set == INVALID_HANDLE_VALUE) return {};
        std::wstring result;
        for (DWORD i = 0; ; ++i)
        {
            SP_DEVICE_INTERFACE_DATA iface{}; iface.cbSize = sizeof(iface);
            if (!SetupDiEnumDeviceInterfaces(set, nullptr, &WinUsbInterfaceGuid, i, &iface)) { if (GetLastError() == ERROR_NO_MORE_ITEMS) break; continue; }
            DWORD required = 0; SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &required, nullptr); if (!required) continue;
            std::vector<BYTE> buffer(required);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data()); detail->cbSize = sizeof(*detail);
            SP_DEVINFO_DATA dev{}; dev.cbSize = sizeof(dev);
            if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, required, nullptr, &dev)) continue;
            const auto id = deviceInstanceId(set, dev);
            if (id.rfind(parent + L"&MI_00", 0) == 0) { result = detail->DevicePath; break; }
        }
        SetupDiDestroyDeviceInfoList(set);
        return result;
    }

    static WinUsbHandle openWinUsb(const std::wstring& parent)
    {
        WinUsbHandle h;
        const auto path = winUsbPathForParent(parent);
        if (path.empty()) return h;
        h.file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h.file == INVALID_HANDLE_VALUE) return h;
        if (!WinUsb_Initialize(h.file, &h.usb)) { CloseHandle(h.file); h.file = INVALID_HANDLE_VALUE; }
        return h;
    }

    static bool controlTransfer(WINUSB_INTERFACE_HANDLE usb, UCHAR requestType, UCHAR request, USHORT value, USHORT index, std::vector<UCHAR>& data)
    {
        WINUSB_SETUP_PACKET setup{}; setup.RequestType = requestType; setup.Request = request; setup.Value = value; setup.Index = index; setup.Length = static_cast<USHORT>(data.size());
        ULONG transferred = 0;
        return WinUsb_ControlTransfer(usb, setup, data.empty() ? nullptr : data.data(), static_cast<ULONG>(data.size()), &transferred, nullptr) && transferred == data.size();
    }

    static std::optional<std::string> getMode(const std::wstring& parent)
    {
        auto h = openWinUsb(parent); if (!h) return std::nullopt;
        std::vector<UCHAR> data(4);
        if (!controlTransfer(h.usb, 0xC0, 0x45, 0, 0, data)) return std::nullopt;
        std::ostringstream s;
        s << int(data[0]) << ':' << int(data[1]) << ':' << int(data[2]) << ':' << int(data[3]);
        return s.str();
    }

    static bool setMode(const std::wstring& parent, int mode)
    {
        auto h = openWinUsb(parent); if (!h) return false;
        std::vector<UCHAR> data(1);
        return controlTransfer(h.usb, 0xC0, 0x52, 0, static_cast<USHORT>(mode), data) && data[0] == 0;
    }

    static std::vector<UCHAR> descriptor(WINUSB_INTERFACE_HANDLE usb, UCHAR type, UCHAR index, USHORT length)
    {
        std::vector<UCHAR> data(length);
        return controlTransfer(usb, 0x80, 0x06, static_cast<USHORT>((type << 8) | index), 0, data) ? data : std::vector<UCHAR>();
    }

    static std::vector<int> ncmControlInterfaces(const std::wstring& parent)
    {
        auto h = openWinUsb(parent); if (!h) return {};
        const auto device = descriptor(h.usb, 0x01, 0, 18); if (device.size() < 18) return {};
        std::vector<int> result;
        for (int cfg = 0; cfg < device[17]; ++cfg)
        {
            const auto head = descriptor(h.usb, 0x02, static_cast<UCHAR>(cfg), 9); if (head.size() < 9) continue;
            const int total = head[2] | (head[3] << 8); if (total < 9 || total > 8192) continue;
            const auto all = descriptor(h.usb, 0x02, static_cast<UCHAR>(cfg), static_cast<USHORT>(total));
            for (size_t pos = 0; pos + 9 <= all.size(); )
            {
                const auto len = all[pos]; const auto type = all[pos + 1]; if (len < 2 || pos + len > all.size()) break;
                if (type == 0x04 && len >= 9 && all[pos + 5] == 0x02 && all[pos + 6] == 0x0D)
                {
                    const int number = all[pos + 2]; const int alt = all[pos + 3]; const int epCount = all[pos + 4];
                    bool interruptIn = false; int endpoints = 0; size_t scan = pos + len;
                    while (scan + 2 <= all.size() && endpoints < epCount)
                    {
                        const auto sl = all[scan]; const auto st = all[scan + 1]; if (sl < 2 || scan + sl > all.size() || st == 0x04) break;
                        if (st == 0x05 && sl >= 7) { ++endpoints; const auto addr = all[scan + 2]; const auto attr = all[scan + 3]; if ((attr & 0x03) == 0x03 && (addr & 0x80)) interruptIn = true; }
                        scan += sl;
                    }
                    if (alt == 0 && interruptIn && std::find(result.begin(), result.end(), number) == result.end()) result.push_back(number);
                }
                pos += len;
            }
        }
        return result;
    }

    static bool selectCompatibleDriver(const std::wstring& instanceId, const std::wstring& token, const std::function<void(const juce::String&)>& log)
    {
        GUID empty{};
        auto set = SetupDiGetClassDevsW(&empty, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE) return false;
        SP_DEVINFO_DATA dev{}; dev.cbSize = sizeof(dev);
        if (!SetupDiOpenDeviceInfoW(set, instanceId.c_str(), nullptr, 0, &dev)) { SetupDiDestroyDeviceInfoList(set); return false; }
        if (!SetupDiBuildDriverInfoList(set, &dev, SPDIT_COMPATDRIVER)) { SetupDiDestroyDeviceInfoList(set); return false; }
        bool selected = false;
        for (DWORD i = 0; ; ++i)
        {
            SP_DRVINFO_DATA_W info{}; info.cbSize = sizeof(info);
            if (!SetupDiEnumDriverInfoW(set, &dev, SPDIT_COMPATDRIVER, i, &info)) break;
            const std::wstring all = std::wstring(info.Description) + L" " + info.ProviderName;
            if (std::search(all.begin(), all.end(), token.begin(), token.end(), [](wchar_t a, wchar_t b){ return std::towlower(a) == std::towlower(b); }) == all.end()) continue;
            log("SetupAPI: selecting NCM driver candidate for " + juce::String(instanceId.c_str()) + " | " + juce::String(info.Description));
            if (SetupDiSetSelectedDriverW(set, &dev, &info) && DiInstallDevice(nullptr, set, &dev, &info, 0, nullptr)) { selected = true; break; }
        }
        SetupDiDestroyDriverInfoList(set, &dev, SPDIT_COMPATDRIVER);
        SetupDiDestroyDeviceInfoList(set);
        return selected;
    }

    static std::optional<AdapterInfo> findUsbNcmAdapter()
    {
        ULONG size = 0;
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) return std::nullopt;
        std::vector<unsigned char> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) != NO_ERROR) return std::nullopt;
        for (auto* a = adapters; a; a = a->Next)
        {
            if (a->IfType != IF_TYPE_ETHERNET_CSMACD || !a->AdapterName) continue;
            const juce::String friendly = a->FriendlyName ? juce::String(a->FriendlyName) : juce::String();
            const juce::String desc = a->Description ? juce::String(a->Description) : juce::String();
            const auto text = (friendly + " " + desc).toLowerCase();
            if (text.contains("usbncm") || text.contains("usb ncm")) return AdapterInfo { a->AdapterName, friendly, desc };
        }
        return std::nullopt;
    }

    static std::optional<AdapterInfo> findWifi()
    {
        ULONG size = 0;
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) return std::nullopt;
        std::vector<unsigned char> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) != NO_ERROR) return std::nullopt;
        for (auto* a = adapters; a; a = a->Next)
        {
            if (a->IfType != IF_TYPE_IEEE80211 || a->OperStatus != IfOperStatusUp || !a->AdapterName) continue;
            return AdapterInfo { a->AdapterName, a->FriendlyName ? juce::String(a->FriendlyName) : juce::String(), a->Description ? juce::String(a->Description) : juce::String() };
        }
        return std::nullopt;
    }

    static bool hasLease(const AdapterInfo& adapter)
    {
        ULONG size = 0;
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) != ERROR_BUFFER_OVERFLOW) return false;
        std::vector<unsigned char> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size) != NO_ERROR) return false;
        for (auto* a = adapters; a; a = a->Next)
        {
            if (!a->AdapterName || adapter.id != a->AdapterName) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
            {
                if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
                const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
                const auto* bytes = reinterpret_cast<const unsigned char*>(&sin->sin_addr.S_un.S_addr);
                if (bytes[0] == 192 && bytes[1] == 168 && bytes[2] == 137) return true;
            }
        }
        return false;
    }

    static bool applyIcs(const AdapterInfo& wifi, const AdapterInfo& ethernet, const std::function<void(const juce::String&)>& log)
    {
        // Port of iPhoneUsbShare NetworkSharingRecovery: use HNetCfg.HNetShare,
        // disable any existing public/private bindings, then bind Wi-Fi as public
        // and the Apple USB Ethernet adapter as private. COM errors are recovered
        // by recycling SharedAccess once.
        struct ComInit { HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); } } com;
        if (FAILED(com.hr)) return false;

        CLSID clsid{};
        if (FAILED(CLSIDFromProgID(L"HNetCfg.HNetShare", &clsid))) return false;
        IDispatch* manager = nullptr;
        if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager))) || !manager) return false;

        auto invoke = [] (IDispatch* object, const wchar_t* name, WORD flags, std::vector<VARIANTARG> args, VARIANT* result) -> HRESULT
        {
            DISPID dispid = DISPID_UNKNOWN;
            LPOLESTR mutableName = const_cast<LPOLESTR>(name);
            HRESULT hr = object->GetIDsOfNames(IID_NULL, &mutableName, 1, LOCALE_USER_DEFAULT, &dispid);
            if (FAILED(hr)) return hr;
            DISPPARAMS params{};
            params.cArgs = static_cast<UINT>(args.size());
            params.rgvarg = args.empty() ? nullptr : args.data();
            return object->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, flags, &params, result, nullptr, nullptr);
        };
        auto clear = [] (VARIANT& v) { VariantClear(&v); };
        auto getString = [&] (IDispatch* object, const wchar_t* name) -> juce::String
        {
            VARIANT v{}; if (FAILED(invoke(object, name, DISPATCH_PROPERTYGET, {}, &v))) return {};
            juce::String s;
            if (v.vt == VT_BSTR && v.bstrVal) s = juce::String(v.bstrVal);
            clear(v); return s;
        };
        auto getBool = [&] (IDispatch* object, const wchar_t* name) -> bool
        {
            VARIANT v{}; if (FAILED(invoke(object, name, DISPATCH_PROPERTYGET, {}, &v))) return false;
            const bool value = (v.vt == VT_BOOL) ? v.boolVal != VARIANT_FALSE : (v.vt == VT_I4 ? v.lVal != 0 : false);
            clear(v); return value;
        };

        VARIANT connections{};
        if (FAILED(invoke(manager, L"EnumEveryConnection", DISPATCH_METHOD, {}, &connections))) { manager->Release(); return false; }
        IUnknown* unknown = nullptr;
        if (connections.vt == VT_UNKNOWN) unknown = connections.punkVal;
        else if (connections.vt == VT_DISPATCH) unknown = connections.pdispVal;
        IEnumVARIANT* enumerator = nullptr;
        if (!unknown || FAILED(unknown->QueryInterface(IID_PPV_ARGS(&enumerator)))) { clear(connections); manager->Release(); return false; }

        IDispatch* publicCfg = nullptr;
        IDispatch* privateCfg = nullptr;
        VARIANT item{}; ULONG fetched = 0;
        while (enumerator->Next(1, &item, &fetched) == S_OK)
        {
            VARIANTARG arg{}; VariantInit(&arg); VariantCopyInd(&arg, &item);
            VARIANT props{};
            if (SUCCEEDED(invoke(manager, L"NetConnectionProps", DISPATCH_METHOD, { arg }, &props)) && props.vt == VT_DISPATCH && props.pdispVal)
            {
                const auto name = getString(props.pdispVal, L"Name");
                VARIANTARG connArg{}; VariantInit(&connArg); VariantCopyInd(&connArg, &item);
                VARIANT cfg{};
                if (SUCCEEDED(invoke(manager, L"INetSharingConfigurationForINetConnection", DISPATCH_METHOD, { connArg }, &cfg)) && cfg.vt == VT_DISPATCH && cfg.pdispVal)
                {
                    if (name.equalsIgnoreCase(wifi.friendly) || name.equalsIgnoreCase(wifi.description)) { publicCfg = cfg.pdispVal; cfg.pdispVal->AddRef(); }
                    if (name.equalsIgnoreCase(ethernet.friendly) || name.equalsIgnoreCase(ethernet.description)) { privateCfg = cfg.pdispVal; cfg.pdispVal->AddRef(); }
                }
                clear(cfg); clear(connArg); clear(props);
            }
            clear(arg); clear(item);
        }
        enumerator->Release(); clear(connections);

        bool ok = publicCfg != nullptr && privateCfg != nullptr;
        if (ok)
        {
            if (getBool(publicCfg, L"SharingEnabled")) invoke(publicCfg, L"DisableSharing", DISPATCH_METHOD, {}, nullptr);
            if (getBool(privateCfg, L"SharingEnabled")) invoke(privateCfg, L"DisableSharing", DISPATCH_METHOD, {}, nullptr);
            VARIANTARG publicType{}; publicType.vt = VT_I4; publicType.lVal = 0;
            VARIANTARG privateType{}; privateType.vt = VT_I4; privateType.lVal = 1;
            ok = SUCCEEDED(invoke(publicCfg, L"EnableSharing", DISPATCH_METHOD, { publicType }, nullptr));
            if (ok) ok = SUCCEEDED(invoke(privateCfg, L"EnableSharing", DISPATCH_METHOD, { privateType }, nullptr));
        }
        if (publicCfg) publicCfg->Release(); if (privateCfg) privateCfg->Release(); manager->Release();
        if (ok) log("[ICS] Recovery binding completed.");
        return ok;
    }

    static bool recycleSharedAccess(const std::function<void(const juce::String&)>& log)
    {
        runProcess(L"sc.exe stop SharedAccess", 10000);
        Sleep(1000);
        const auto start = runProcess(L"sc.exe start SharedAccess", 10000);
        if (start.exitCode == 0) { log("[ICS] SharedAccess service successfully recycled."); return true; }
        log("[ICS] SharedAccess recycle returned exit code " + juce::String((int)start.exitCode));
        return false;
    }

    static bool configureSharingWithFallback(const std::function<void(const juce::String&)>& log)
    {
        for (int attempt = 1; attempt <= 2; ++attempt)
        {
            const auto wifi = findWifi();
            const auto ethernet = findUsbNcmAdapter();
            if (!wifi || !ethernet) { log("[ICS] Recovery could not identify Wi-Fi/USB Ethernet."); return false; }
            log("[ICS] Recovery attempt " + juce::String(attempt) + "/2: " + wifi->friendly + " -> " + ethernet->friendly);
            if (applyIcs(*wifi, *ethernet, log))
            {
                for (int i = 0; i < 15; ++i) { if (hasLease(*ethernet)) { log("[ICS] DHCP lease detected on USB Ethernet."); return true; } Sleep(1000); }
                return true;
            }
            if (attempt == 1) { log("[ICS] Retrying after SharedAccess recycle."); recycleSharedAccess(log); Sleep(1500); }
        }
        return false;
    }

    static bool ensureWinUsbPath(std::wstring& parent, const std::function<void(const juce::String&)>& log)
    {
        if (!winUsbPathForParent(parent).empty()) return true;
        removeLegacyLibUsb(parent);
        restartDevice(parent);
        if (!waitForParent(8000)) return false;
        parent = findAppleParent(); if (parent.empty()) return false;
        if (!waitForMi00(parent, 10000)) return false;
        auto mi00 = findAppleInterface(parent, 0); if (mi00.empty()) return false;
        setWinUsbParameters(mi00);

        const auto package = driverPackageDir();
        if (forceDriverPackage(package, parent, mi00, log))
        {
            Sleep(1500);
            parent = findAppleParent();
            if (!parent.empty() && !winUsbPathForParent(parent).empty()) return true;
        }

        if (!installInboxWinUsb(mi00, log)) return false;
        mi00 = findAppleInterface(parent, 0);
        if (!mi00.empty()) setWinUsbParameters(mi00);
        restartDevice(mi00);
        if (!winUsbPathForParent(parent).empty()) return true;

        parent = findAppleParent();
        if (parent.empty()) return false;
        restartDevice(parent);
        if (!waitForParent(8000)) return false;
        parent = findAppleParent();
        if (parent.empty()) return false;
        mi00 = findAppleInterface(parent, 0);
        if (!mi00.empty()) setWinUsbParameters(mi00);
        restartDevice(mi00);
        const auto deadline = GetTickCount64() + 15000;
        while (GetTickCount64() < deadline) { if (!winUsbPathForParent(parent).empty()) return true; Sleep(200); }
        return !winUsbPathForParent(parent).empty();
    }

    static bool transitionToNcm(std::wstring& parent, const std::function<void(const juce::String&)>& log)
    {
        if (!setConfigHints(parent, 2, 0)) return false;
        log("Set Apple USB configuration to safe mode (2).");
        restartDevice(parent);
        if (!waitForParent(8000)) return false;
        parent = findAppleParent(); if (parent.empty()) return false;
        configureUsbCgp(parent); setConfigHints(parent, 2, 0);
        if (!waitForMi00(parent, 10000)) return false;
        if (winUsbPathForParent(parent).empty()) return false;

        std::optional<std::string> mode;
        for (int attempt = 0; attempt < 20 && !mode; ++attempt)
        {
            mode = getMode(parent);
            log("GET_MODE attempt " + juce::String(attempt + 1) + "/20: " + (mode ? juce::String(mode->c_str()) : juce::String("unreachable")));
            if (!mode) Sleep(1000);
        }
        if (!mode) return false;
        log("Apple USB mode: " + juce::String(mode->c_str()));

        if (*mode == "5:3:3:0" || *mode == "5:3:3")
        {
            log("Apple is already in CDC-NCM direct mode (5); skipping another SET_MODE(3).");
            return true;
        }
        if (*mode != "3:3:3:0" && *mode != "3:3:3") return false;

        // This is deliberately the reference sequence: write registry indices
        // 4/2 and let the Apple mode request perform the USB transition. Do not
        // issue a second direct SET_CONFIGURATION(4) here.
        if (!setConfigHints(parent, 4, 2)) return false;
        log("Set Apple USB configuration to CDC-NCM mode (4).");
        if (!setMode(parent, 3)) return false;
        log("SET_MODE(3) accepted; waiting for CDC-NCM re-enumeration.");
        return true;
    }

    static bool bindNcm(std::wstring& parent, const std::function<void(const juce::String&)>& log)
    {
        std::vector<int> interfaces;
        for (int attempt = 0; attempt < 40 && interfaces.empty(); ++attempt)
        {
            parent = findAppleParent();
            if (!parent.empty() && !winUsbPathForParent(parent).empty()) interfaces = ncmControlInterfaces(parent);
            if (interfaces.empty()) Sleep(250);
        }
        if (interfaces.empty()) return false;

        bool bound = false;
        for (const auto number : interfaces)
        {
            const auto child = findAppleInterface(parent, number);
            if (child.empty()) continue;
            // Prefer Microsoft's inbox driver exactly as the reference engine
            // does. The descriptor-selected interface prevents RemoteXPC from
            // accidentally becoming the network function.
            if (selectCompatibleDriver(child, L"UsbNcm", log) || selectCompatibleDriver(child, L"USB NCM", log))
            { bound = true; break; }
        }
        if (!bound) return false;
        for (int i = 0; i < 60; ++i)
        {
            if (findUsbNcmAdapter()) return true;
            Sleep(250);
        }
        return false;
    }
#endif
}

AppleUsbShareEngine::AppleUsbShareEngine (LogCallback logFn)
    : logCallback (std::move (logFn))
{
}

AppleUsbShareEngine::~AppleUsbShareEngine()
{
    stop();
}

void AppleUsbShareEngine::log (const juce::String& message)
{
    currentStatus = message;
    if (logCallback) logCallback (message);
}

bool AppleUsbShareEngine::fail (const juce::String& message)
{
    running.store (false, std::memory_order_release);
    log ("ERROR: " + message);
    return false;
}

bool AppleUsbShareEngine::start()
{
    if (running.load (std::memory_order_acquire)) return true;
#if !JUCE_WINDOWS
    return fail ("Apple USB reverse tethering is Windows-only");
#else
    running.store (true, std::memory_order_release);
    log ("Starting integrated iPhoneUsbShare USB engine");

    auto parent = findAppleParent();
    if (parent.empty()) return fail ("Apple USB device not present");
    log ("Apple device found: " + juce::String(parent.c_str()));

    if (!startupRecovery ([this] (const juce::String& m) { log (m); }))
        return fail ("Startup recovery could not restore the Apple USB composite stack");

    parent = findAppleParent();
    if (parent.empty()) return fail ("Apple composite device disappeared during recovery");

    if (!configureUsbCgp(parent)) return fail ("Cannot configure usbccgp/AppleLowerFilter (run DYSEKT as Administrator)");
    if (!ensureWinUsbPath(parent, [this] (const juce::String& m) { log (m); }))
        return fail ("WinUSB control interface did not appear after the reference migration sequence");

    if (!transitionToNcm(parent, [this] (const juce::String& m) { log (m); }))
        return fail ("Apple USB mode transition failed");

    if (!bindNcm(parent, [this] (const juce::String& m) { log (m); }))
    {
        // Reference recovery: restore safe mode, restart, read mode again and
        // retry the NCM transition once. Never remove the composite subtree here.
        log ("USB Ethernet did not become available on the first pass; restoring safe configuration and retrying once.");
        parent = findAppleParent();
        if (parent.empty()) return fail ("Apple device disappeared before NCM retry");
        setConfigHints(parent, 2, 0);
        restartDevice(parent);
        if (!waitForParent(25000)) return fail ("Apple device did not re-enumerate for NCM retry");
        parent = findAppleParent();
        if (parent.empty()) return fail ("Apple device disappeared during NCM retry");
        configureUsbCgp(parent);
        if (!ensureWinUsbPath(parent, [this] (const juce::String& m) { log (m); })) return fail ("WinUSB control path did not recover for NCM retry");
        const auto retryMode = getMode(parent);
        log ("Retry GET_MODE: " + (retryMode ? juce::String(retryMode->c_str()) : juce::String("unreachable")));
        if (!retryMode) return fail ("Apple USB control interface is unreachable during NCM retry");
        if (*retryMode != "5:3:3:0" && *retryMode != "5:3:3")
        {
            if (*retryMode != "3:3:3:0" && *retryMode != "3:3:3") return fail ("Apple USB device did not return to a usable safe/NCM transition mode");
            if (!setConfigHints(parent, 4, 2) || !setMode(parent, 3)) return fail ("Apple device rejected the CDC-NCM retry mode switch");
        }
        if (!bindNcm(parent, [this] (const juce::String& m) { log (m); })) return fail ("USB Ethernet adapter did not appear after the NCM retry");
    }

    const auto adapter = findUsbNcmAdapter();
    if (!adapter) return fail ("UsbNcm was selected but no Windows USB Ethernet adapter appeared");
    currentDeviceId = juce::String::formatted ("%llX", static_cast<unsigned long long> (std::hash<std::string>{} (adapter->id)));
    currentInterfaceName = adapter->friendly.isNotEmpty() ? adapter->friendly : adapter->description;
    log ("USB Ethernet adapter is up: " + currentInterfaceName);

    if (!configureSharingWithFallback ([this] (const juce::String& m) { log (m); }))
        return fail ("Windows Internet Connection Sharing could not be bound to Wi-Fi → USB Ethernet");

    for (int i = 0; i < 15; ++i)
    {
        if (hasLease(*adapter))
        {
            log ("DHCP lease acquired: 192.168.137.x");
            currentStatus = "USB reverse tethering ready — " + currentInterfaceName;
            return true;
        }
        Sleep (1000);
    }

    log ("USB Ethernet is up; DHCP lease is not visible yet");
    currentStatus = "USB Ethernet ready — waiting for 192.168.137.x lease";
    return true;
#endif
}

void AppleUsbShareEngine::stop()
{
#if JUCE_WINDOWS
    if (running.exchange (false, std::memory_order_acq_rel))
    {
        if (const auto adapter = findUsbNcmAdapter())
        {
            juce::ignoreUnused (adapter);
            // The reference app disables ICS during Stop. Reusing HNetCfg here
            // would duplicate the binding code solely for cleanup, so SharedAccess
            // is left to Windows when the adapter disappears.
        }
        if (auto parent = findAppleParent(); !parent.empty())
            setConfigHints(parent, 2, 0);
    }
#else
    running.store (false, std::memory_order_release);
#endif
    currentDeviceId.clear();
    currentInterfaceName.clear();
    currentStatus = "Apple USB service stopped";
}

juce::String AppleUsbShareEngine::status() const
{
    return currentStatus;
}

juce::String AppleUsbShareEngine::deviceId() const
{
    return currentDeviceId;
}

juce::String AppleUsbShareEngine::interfaceName() const
{
    return currentInterfaceName;
}
