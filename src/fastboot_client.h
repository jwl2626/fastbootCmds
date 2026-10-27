#pragma once

#include "winusb_fastboot.h"

#include <cstdint>
#include <string>
#include <vector>

class FastbootClient {
  public:
    explicit FastbootClient(FastbootUsbDevice* device) : device_(device) {}

    bool GetVar(const std::string& name, std::string* value, std::string* error);
    bool GetVarAll(std::vector<std::string>* lines, std::string* error);
    bool Reboot(const std::string& target, std::string* error);
    bool Erase(const std::string& partition, std::string* error);
    bool FlashFile(const std::string& partition, const std::wstring& image,
                   std::vector<std::string>* log, std::string* error);
    bool BootFile(const std::wstring& image, std::vector<std::string>* log,
                  std::string* error);
    bool Oem(const std::string& args, std::string* error);
    bool Flashing(const std::string& action, std::string* error);
    bool SetActive(const std::string& slot, std::string* error);
    bool Continue(std::string* error);
    bool Raw(const std::string& command, std::string* error);

  private:
    struct Response {
        std::string status;
        std::string value;
        std::vector<std::string> infos;
    };

    bool ReadResponse(DWORD timeout_ms, Response* response, std::string* error);
    bool Command(const std::string& command, DWORD timeout_ms,
                 Response* response, std::string* error);
    bool DownloadFile(const std::wstring& path, std::uint64_t size,
                      std::vector<std::string>* log, std::string* error);
    bool DownloadBuffer(const std::vector<std::uint8_t>& data,
                        std::vector<std::string>* log, std::string* error);
    std::uint64_t MaxDownloadSize(std::string* error);
    std::uint64_t PartitionSize(const std::string& partition);
    bool IsLogical(const std::string& partition);
    std::wstring PrepareAvbImage(const std::wstring& path, std::uint64_t size,
                                 const std::string& partition,
                                 std::uint64_t max_download, std::string* error);

    FastbootUsbDevice* device_;
};
