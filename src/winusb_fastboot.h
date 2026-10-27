#pragma once

#include <windows.h>
#include <winusb.h>

#include <cstdint>
#include <string>
#include <vector>

struct FastbootDeviceInfo {
    std::string location;      // usb:1D00:0-1.3
    std::string interface_path;  // \\?\usb#...
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
    std::string serial;
    std::string product;
};

class FastbootUsbDevice {
  public:
    FastbootUsbDevice();
    ~FastbootUsbDevice();

    FastbootUsbDevice(const FastbootUsbDevice&) = delete;
    FastbootUsbDevice& operator=(const FastbootUsbDevice&) = delete;

    bool OpenPath(const std::string& interface_path, std::string* error);
    void Close();

    bool IsFastboot() const { return is_fastboot_; }
    bool IsOpen() const { return winusb_ != nullptr; }

    const FastbootDeviceInfo& info() const { return info_; }

    bool Read(void* data, std::size_t length, DWORD timeout_ms,
              std::size_t* bytes_read, std::string* error);
    bool Write(const void* data, std::size_t length, DWORD timeout_ms,
               std::string* error);

  private:
    bool QueryDescriptors(std::string* error);
    bool SetPipeTimeout(UCHAR pipe, DWORD timeout_ms);
    std::string ReadStringDescriptor(UCHAR index);

    HANDLE file_ = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE winusb_ = nullptr;
    UCHAR ep_in_ = 0;
    UCHAR ep_out_ = 0;
    bool is_fastboot_ = false;
    FastbootDeviceInfo info_;
};

// Enumerate all USB interfaces that expose the fastboot class/subclass/protocol.
bool EnumerateFastbootDevices(std::vector<FastbootDeviceInfo>* devices,
                              std::string* error);
