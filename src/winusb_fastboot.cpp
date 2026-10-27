#include "winusb_fastboot.h"

#include "usb_path_windows.h"

#include <setupapi.h>
#include <usb100.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const GUID kUsbDeviceInterface = {
    0xA5DCBF10, 0x6530, 0x11D2,
    {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};

const GUID kAndroidUsbInterface = {
    0xF72FE0D4, 0xCBCB, 0x407D,
    {0x88, 0x14, 0x9E, 0xD6, 0x73, 0xD0, 0xDD, 0x6B}};

std::string FormatWinError(DWORD code) {
    char* message = nullptr;
    DWORD length = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<char*>(&message), 0, nullptr);
    std::string result;
    if (length != 0 && message != nullptr) {
        result.assign(message, length);
        while (!result.empty() &&
               (result.back() == '\r' || result.back() == '\n' ||
                result.back() == ' ')) {
            result.pop_back();
        }
    } else {
        result = "WinError " + std::to_string(code);
    }
    if (message != nullptr) LocalFree(message);
    return result;
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return std::string();
    int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0,
                                     nullptr, nullptr);
    if (length <= 0) return std::string();
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), &result[0], length,
                        nullptr, nullptr);
    return result;
}

std::wstring Wide(const std::string& value) {
    if (value.empty()) return std::wstring();
    int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return std::wstring();
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), &result[0], length);
    return result;
}

struct InterfaceRef {
    std::string path;
    std::string location;
};

bool EnumerateGuid(const GUID& guid, std::vector<InterfaceRef>* out) {
    HDEVINFO handles = SetupDiGetClassDevsW(
        &guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (handles == INVALID_HANDLE_VALUE) return false;

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(
                handles, nullptr, &guid, index, &interface_data)) {
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
                handles, &interface_data, detail, required, nullptr,
                &device_info)) {
            continue;
        }

        InterfaceRef ref;
        ref.path = Utf8(detail->DevicePath);
        char location[256] = {0};
        if (fastboot::UsbPathFromInterfaceName(detail->DevicePath, location,
                                                sizeof(location))) {
            ref.location = location;
        }
        out->push_back(std::move(ref));
    }
    SetupDiDestroyDeviceInfoList(handles);
    return true;
}

bool GetDescriptor(WINUSB_INTERFACE_HANDLE winusb,
                   UCHAR type, UCHAR index, USHORT language,
                   std::vector<BYTE>* data) {
    data->assign(256, 0);
    ULONG transferred = 0;
    if (!WinUsb_GetDescriptor(winusb, type, index, language, data->data(),
                              static_cast<ULONG>(data->size()), &transferred)) {
        return false;
    }
    data->resize(transferred);
    return true;
}

std::string DecodeStringDescriptor(const std::vector<BYTE>& data) {
    if (data.size() < 4 || data[0] < 4) return std::string();
    int length = std::min<size_t>(data[0], data.size());
    std::wstring wide;
    for (int i = 2; i + 1 < length; i += 2) {
        wide.push_back(static_cast<wchar_t>(data[i] | (data[i + 1] << 8)));
    }
    return Utf8(wide);
}

}  // namespace

FastbootUsbDevice::FastbootUsbDevice() = default;

FastbootUsbDevice::~FastbootUsbDevice() {
    Close();
}

void FastbootUsbDevice::Close() {
    if (winusb_ != nullptr) {
        WinUsb_Free(winusb_);
        winusb_ = nullptr;
    }
    if (file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
    ep_in_ = ep_out_ = 0;
    is_fastboot_ = false;
}

bool FastbootUsbDevice::OpenPath(const std::string& interface_path,
                                 std::string* error) {
    Close();
    std::wstring path = Wide(interface_path);
    if (path.empty()) {
        *error = "Invalid USB interface path";
        return false;
    }

    file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        *error = "Open USB device failed: " + FormatWinError(GetLastError());
        return false;
    }

    if (!WinUsb_Initialize(file_, &winusb_)) {
        *error = "WinUsb_Initialize failed: " + FormatWinError(GetLastError()) +
                 " (device may not use the WinUSB driver)";
        Close();
        return false;
    }

    info_.interface_path = interface_path;
    if (!QueryDescriptors(error)) {
        Close();
        return false;
    }
    return true;
}

bool FastbootUsbDevice::QueryDescriptors(std::string* error) {
    USB_INTERFACE_DESCRIPTOR interface_descriptor{};
    if (!WinUsb_QueryInterfaceSettings(winusb_, 0, &interface_descriptor)) {
        *error = "WinUsb_QueryInterfaceSettings failed: " + FormatWinError(GetLastError());
                 FormatWinError(GetLastError());
        return false;
    }

    is_fastboot_ = interface_descriptor.bInterfaceClass == 0xFF &&
                   interface_descriptor.bInterfaceSubClass == 0x42 &&
                   interface_descriptor.bInterfaceProtocol == 0x03;

    for (UCHAR index = 1; index <= interface_descriptor.bNumEndpoints; ++index) {
        WINUSB_PIPE_INFORMATION pipe{};
        if (!WinUsb_QueryPipe(winusb_, 0, index, &pipe)) continue;
        if (pipe.PipeType != UsbdPipeTypeBulk) continue;
        if ((pipe.PipeId & 0x80) != 0) {
            if (ep_in_ == 0) ep_in_ = pipe.PipeId;
        } else {
            if (ep_out_ == 0) ep_out_ = pipe.PipeId;
        }
    }

    std::vector<BYTE> descriptor;
    if (GetDescriptor(winusb_, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0,
                      &descriptor)) {
        if (descriptor.size() >= sizeof(USB_DEVICE_DESCRIPTOR)) {
            auto* desc = reinterpret_cast<USB_DEVICE_DESCRIPTOR*>(
                descriptor.data());
            info_.vendor_id = desc->idVendor;
            info_.product_id = desc->idProduct;

            std::vector<BYTE> languages;
            USHORT language = 0x0409;
            if (GetDescriptor(winusb_, USB_STRING_DESCRIPTOR_TYPE, 0, 0,
                              &languages) &&
                languages.size() >= 4) {
                language = static_cast<USHORT>(languages[2] |
                                               (languages[3] << 8));
            }

            auto read_string = [&](UCHAR index) {
                if (index == 0) return std::string();
                std::vector<BYTE> value;
                if (!GetDescriptor(winusb_, USB_STRING_DESCRIPTOR_TYPE,
                                   index, language, &value)) {
                    return std::string();
                }
                return DecodeStringDescriptor(value);
            };

            info_.serial = read_string(desc->iSerialNumber);
            info_.product = read_string(desc->iProduct);
        }
    }

    if (!is_fastboot_) {
        *error = "This interface is not a fastboot interface";
        return false;
    }
    if (ep_in_ == 0 || ep_out_ == 0) {
        *error = "The fastboot interface has no bulk endpoints";
        return false;
    }
    return true;
}

bool FastbootUsbDevice::SetPipeTimeout(UCHAR pipe, DWORD timeout_ms) {
    ULONG value = timeout_ms;
    return WinUsb_SetPipePolicy(winusb_, pipe, PIPE_TRANSFER_TIMEOUT,
                                sizeof(value), &value) != FALSE;
}

bool FastbootUsbDevice::Read(void* data, std::size_t length, DWORD timeout_ms,
                             std::size_t* bytes_read, std::string* error) {
    if (!IsOpen() || ep_in_ == 0) {
        *error = "Device is not open";
        return false;
    }
    SetPipeTimeout(ep_in_, timeout_ms);
    ULONG transferred = 0;
    if (!WinUsb_ReadPipe(winusb_, ep_in_, static_cast<PUCHAR>(data),
                         static_cast<ULONG>(length), &transferred, nullptr)) {
        DWORD code = GetLastError();
        WinUsb_ResetPipe(winusb_, ep_in_);
        *error = "Read USB response failed: " + FormatWinError(code);
        return false;
    }
    *bytes_read = transferred;
    return true;
}

bool FastbootUsbDevice::Write(const void* data, std::size_t length,
                              DWORD timeout_ms, std::string* error) {
    if (!IsOpen() || ep_out_ == 0) {
        *error = "Device is not open";
        return false;
    }
    SetPipeTimeout(ep_out_, timeout_ms);
    const BYTE* cursor = static_cast<const BYTE*>(data);
    std::size_t remaining = length;
    while (remaining > 0) {
        ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(remaining, 1024 * 1024));
        ULONG written = 0;
        if (!WinUsb_WritePipe(winusb_, ep_out_, const_cast<PUCHAR>(cursor),
                              chunk, &written, nullptr)) {
            DWORD code = GetLastError();
            WinUsb_ResetPipe(winusb_, ep_out_);
            *error = "Write USB data failed: " + FormatWinError(code);
            return false;
        }
        if (written == 0) {
            *error = "Write USB data failed: zero bytes written";
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

bool EnumerateFastbootDevices(std::vector<FastbootDeviceInfo>* devices,
                              std::string* error) {
    std::vector<InterfaceRef> interfaces;
    EnumerateGuid(kUsbDeviceInterface, &interfaces);
    EnumerateGuid(kAndroidUsbInterface, &interfaces);

    std::sort(interfaces.begin(), interfaces.end(),
              [](const InterfaceRef& left, const InterfaceRef& right) {
                  return left.path < right.path;
              });
    interfaces.erase(
        std::unique(interfaces.begin(), interfaces.end(),
                    [](const InterfaceRef& left, const InterfaceRef& right) {
                        return left.path == right.path;
                    }),
        interfaces.end());

    devices->clear();
    for (const auto& iface : interfaces) {
        FastbootUsbDevice device;
        std::string open_error;
        if (!device.OpenPath(iface.path, &open_error)) continue;
        if (!device.IsFastboot()) {
            device.Close();
            continue;
        }
        FastbootDeviceInfo info = device.info();
        info.location = iface.location;
        if (info.location.empty()) {
            info.location = "usb:unknown";
        }
        devices->push_back(std::move(info));
        device.Close();
    }

    std::sort(devices->begin(), devices->end(),
              [](const FastbootDeviceInfo& left,
                 const FastbootDeviceInfo& right) {
                  return left.location < right.location;
              });
    if (devices->empty()) {
        *error = "No fastboot device found (fastboot mode + WinUSB driver required)";
        return false;
    }
    return true;
}



