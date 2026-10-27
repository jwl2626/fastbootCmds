#include "usb_path_windows.h"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpropdef.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

// MinGW 的 cfgmgr32.h 在旧版本（如 GCC 8）里缺少 Vista+ 的 property API 声明，
// 新版本（如 GCC 16）已经自带。重复声明会触发 -Wattributes，
// 这里仅在编译期屏蔽这一条警告，调用与链接行为完全不变。
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif
extern "C" CONFIGRET WINAPI CM_Get_DevNode_PropertyW(DEVINST dnDevInst,
                                                     const DEVPROPKEY* property_key,
                                                     DEVPROPTYPE* property_type,
                                                     PBYTE property_buffer,
                                                     PULONG property_buffer_size,
                                                     ULONG flags);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fastboot {
namespace {

const GUID kUsbDeviceInterface = {
    0xA5DCBF10, 0x6530, 0x11D2,
    {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};

const DEVPROPKEY kLocationPaths = {
    {0xA45C254E, 0xDF1C, 0x4EFD,
     {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}},
    37};

const DEVPROPKEY kLocationInfo = {
    {0xA45C254E, 0xDF1C, 0x4EFD,
     {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}},
    15};

bool IEquals(const std::wstring& value, const wchar_t* text) {
    return _wcsicmp(value.c_str(), text) == 0;
}

bool ParseNumberAfter(const std::wstring& text, const wchar_t* marker, int* value) {
    size_t pos = text.find(marker);
    if (pos == std::wstring::npos) return false;
    pos += wcslen(marker);
    if (pos >= text.size() || text[pos] != L'(') return false;
    ++pos;
    int result = 0;
    bool any = false;
    while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9') {
        any = true;
        result = result * 10 + (text[pos] - L'0');
        ++pos;
    }
    if (!any || pos >= text.size() || text[pos] != L')') return false;
    *value = result;
    return true;
}

bool GetStringList(DEVINST devinst, const DEVPROPKEY& key,
                   std::vector<std::wstring>* out) {
    ULONG size = 4096;
    std::vector<BYTE> buffer(size);
    DEVPROPTYPE type = 0;
    CONFIGRET result = CM_Get_DevNode_PropertyW(
        devinst, &key, &type, buffer.data(), &size, 0);
    if (result == CR_BUFFER_SMALL) {
        buffer.resize(size);
        result = CM_Get_DevNode_PropertyW(
            devinst, &key, &type, buffer.data(), &size, 0);
    }
    if (result != CR_SUCCESS || type != DEVPROP_TYPE_STRING_LIST) return false;

    const wchar_t* cursor = reinterpret_cast<const wchar_t*>(buffer.data());
    size_t remaining = size / sizeof(wchar_t);
    while (remaining > 0 && *cursor != L'\0') {
        std::wstring value(cursor);
        if (!value.empty()) out->push_back(value);
        size_t length = value.size() + 1;
        if (length >= remaining) break;
        cursor += length;
        remaining -= length;
    }
    return !out->empty();
}

bool ParseLocationPath(const std::wstring& path, char* out, size_t out_size) {
    int root_hub = 0;
    if (!ParseNumberAfter(path, L"USBROOT", &root_hub)) return false;

    std::vector<int> ports;
    size_t pos = 0;
    while ((pos = path.find(L"USB(", pos)) != std::wstring::npos) {
        pos += 4;
        int port = 0;
        bool any = false;
        while (pos < path.size() && path[pos] >= L'0' && path[pos] <= L'9') {
            any = true;
            port = port * 10 + (path[pos] - L'0');
            ++pos;
        }
        if (any) ports.push_back(port);
    }
    if (ports.empty()) return false;

    // PCI slot of the host controller, if Windows reports one.
    std::wstring pci;
    pos = 0;
    while ((pos = path.find(L"PCI(", pos)) != std::wstring::npos) {
        pos += 4;
        std::wstring value;
        while (pos < path.size() && iswxdigit(path[pos])) {
            value.push_back(path[pos]);
            ++pos;
        }
        if (!value.empty()) pci = value;
    }

    std::string text;
    if (!pci.empty()) {
        char pci_narrow[64] = {0};
        WideCharToMultiByte(CP_ACP, 0, pci.c_str(), -1,
                            pci_narrow, sizeof(pci_narrow) - 1, nullptr, nullptr);
        text = "usb:";
        text += pci_narrow;
        text += ":";
    } else {
        text = "usb:";
    }
    text += std::to_string(root_hub);
    text += "-";
    for (size_t i = 0; i < ports.size(); ++i) {
        if (i) text += ".";
        text += std::to_string(ports[i]);
    }
    if (text.size() + 1 > out_size) return false;
    std::snprintf(out, out_size, "%s", text.c_str());
    return true;
}

DEVINST FindByInterfacePath(const wchar_t* interface_name) {
    HDEVINFO handles = SetupDiGetClassDevsW(
        &kUsbDeviceInterface, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (handles == INVALID_HANDLE_VALUE) return 0;

    DEVINST found = 0;
    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(
                handles, nullptr, &kUsbDeviceInterface, index, &interface_data)) {
            break;
        }

        DWORD required = 0;
        SP_DEVINFO_DATA device_info{};
        device_info.cbSize = sizeof(device_info);
        SetupDiGetDeviceInterfaceDetailW(
            handles, &interface_data, nullptr, 0, &required, &device_info);
        if (required == 0) continue;

        std::vector<BYTE> buffer(required);
        auto detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
            buffer.data());
#ifdef _WIN64
        detail->cbSize = 8;
#else
        detail->cbSize = 6;
#endif
        if (!SetupDiGetDeviceInterfaceDetailW(
                handles, &interface_data, detail, required, nullptr, &device_info)) {
            continue;
        }
        if (_wcsicmp(detail->DevicePath, interface_name) == 0) {
            found = device_info.DevInst;
            break;
        }
    }
    SetupDiDestroyDeviceInfoList(handles);
    return found;
}

std::wstring InstanceIdFromInterfaceName(const std::wstring& name) {
    std::wstring text = name;
    if (text.rfind(L"\\\\?\\", 0) == 0) text.erase(0, 4);
    if (_wcsnicmp(text.c_str(), L"USB\\", 4) == 0) return text;

    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'#', start);
        if (end == std::wstring::npos) end = text.size();
        parts.push_back(text.substr(start, end - start));
        if (end == text.size()) break;
        start = end + 1;
    }
    if (parts.size() < 3 || !IEquals(parts[0], L"usb")) return L"";
    return L"USB\\" + parts[1] + L"\\" + parts[2];
}

bool QueryLocation(DEVINST devinst, const DEVPROPKEY& key,
                   char* out, size_t out_size) {
    std::vector<std::wstring> values;
    if (!GetStringList(devinst, key, &values)) return false;
    for (const auto& value : values) {
        if (ParseLocationPath(value, out, out_size)) return true;
    }
    return false;
}

bool ParseLocationInfo(const std::wstring& info, char* out, size_t out_size) {
    int port = 0;
    int hub = 0;
    // "Port_#0003.Hub_#0004"
    size_t port_pos = info.find(L"Port_#");
    size_t hub_pos = info.find(L"Hub_#");
    if (port_pos == std::wstring::npos || hub_pos == std::wstring::npos) return false;
    port_pos += 6;
    hub_pos += 5;
    port = _wtoi(info.c_str() + port_pos);
    hub = _wtoi(info.c_str() + hub_pos);
    if (port <= 0 || hub <= 0) return false;
    std::snprintf(out, out_size, "usb:h%dp%d", hub, port);
    return true;
}

}  // namespace

bool UsbPathFromInterfaceName(const wchar_t* interface_name,
                              char* out, size_t out_size) {
    if (interface_name == nullptr || out == nullptr || out_size == 0) return false;
    out[0] = '\0';

    DEVINST devinst = 0;
    std::wstring instance_id = InstanceIdFromInterfaceName(interface_name);
    if (!instance_id.empty()) {
        std::wstring copy = instance_id;
        if (CM_Locate_DevNodeW(&devinst, &copy[0], CM_LOCATE_DEVNODE_NORMAL) !=
            CR_SUCCESS) {
            devinst = 0;
        }
    }
    if (devinst == 0) devinst = FindByInterfacePath(interface_name);
    if (devinst == 0) return false;

    if (QueryLocation(devinst, kLocationPaths, out, out_size)) return true;

    std::vector<std::wstring> info;
    if (GetStringList(devinst, kLocationInfo, &info) && !info.empty()) {
        return ParseLocationInfo(info[0], out, out_size);
    }
    return false;
}

bool UsbPathMatches(const char* path, int hub, int index) {
    if (path == nullptr || (hub < 0 && index < 0)) return true;
    std::string text(path);
    if (text.rfind("usb:", 0) == 0) text.erase(0, 4);

    std::vector<int> numbers;
    for (size_t i = 0; i < text.size();) {
        if (text[i] >= '0' && text[i] <= '9') {
            int value = 0;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
                value = value * 10 + (text[i] - '0');
                ++i;
            }
            numbers.push_back(value);
        } else {
            ++i;
        }
    }
    if (numbers.empty()) return false;
    int actual_index = numbers.back();
    if (index >= 0 && actual_index != index) return false;
    if (hub >= 0) {
        if (numbers.size() < 2) return false;
        if (numbers[numbers.size() - 2] != hub) return false;
    }
    return true;
}

}  // namespace fastboot
