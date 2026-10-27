#include "fastboot_client.h"

#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kSparseMagic = 0xED26FF3A;
constexpr std::uint64_t kDefaultMaxDownload = 256ull * 1024 * 1024;
constexpr DWORD kCommandTimeoutMs = 20000;
constexpr DWORD kFlashTimeoutMs = 180000;

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

std::uint64_t FileSize(const std::wstring& path, std::string* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = "Cannot open image: " + FormatWinError(GetLastError());
        return 0;
    }
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) != FALSE;
    DWORD code = ok ? 0 : GetLastError();
    CloseHandle(file);
    if (!ok) {
        *error = "Cannot get image size: " + FormatWinError(code);
        return 0;
    }
    return static_cast<std::uint64_t>(size.QuadPart);
}

bool IsSparseImage(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::uint32_t magic = 0;
    DWORD read = 0;
    bool ok = ReadFile(file, &magic, sizeof(magic), &read, nullptr) != FALSE &&
              read == sizeof(magic);
    CloseHandle(file);
    return ok && magic == kSparseMagic;
}

std::string TrimPacket(const char* data, std::size_t length) {
    std::string text(data, length);
    while (!text.empty() && text.back() == '\0') text.pop_back();
    return text;
}

bool ParseU64(const std::string& text, std::uint64_t* value) {
    if (text.empty()) return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long parsed = std::strtoull(text.c_str(), &end, 0);
    if (errno != 0 || end == text.c_str()) return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

}  // namespace

bool FastbootClient::ReadResponse(DWORD timeout_ms, Response* response,
                                  std::string* error) {
    ULONGLONG deadline = GetTickCount() + timeout_ms;
    response->status.clear();
    response->value.clear();
    response->infos.clear();

    while (true) {
        if (GetTickCount() >= deadline) {
            *error = "Timed out waiting for the device response";
            return false;
        }
        char buffer[64] = {0};
        std::size_t read = 0;
        std::string read_error;
        if (!device_->Read(buffer, sizeof(buffer), timeout_ms, &read,
                           &read_error)) {
            *error = read_error;
            return false;
        }
        std::string text = TrimPacket(buffer, read);
        if (text.size() < 4) {
            *error = "Invalid fastboot response: " + text;
            return false;
        }
        std::string status = text.substr(0, 4);
        std::string value = text.substr(4);
        if (status == "INFO" || status == "TEXT") {
            if (!value.empty()) response->infos.push_back(value);
            deadline = GetTickCount() + timeout_ms;
            continue;
        }
        if (status == "OKAY") {
            response->status = status;
            response->value = value;
            return true;
        }
        if (status == "FAIL") {
            *error = value.empty() ? "Device returned FAIL" : value;
            return false;
        }
        if (status == "DATA") {
            response->status = status;
            response->value = value;
            return true;
        }
        *error = "Unknown fastboot response: " + text;
        return false;
    }
}

bool FastbootClient::Command(const std::string& command, DWORD timeout_ms,
                             Response* response, std::string* error) {
    if (command.size() > 64) {
        *error = "Fastboot command is longer than 64 bytes";
        return false;
    }
    std::string write_error;
    if (!device_->Write(command.data(), command.size(), 10000, &write_error)) {
        *error = write_error;
        return false;
    }
    return ReadResponse(timeout_ms, response, error);
}

std::uint64_t FastbootClient::MaxDownloadSize(std::string* error) {
    (void)error;
    std::string value;
    std::string var_error;
    if (!GetVar("max-download-size", &value, &var_error)) {
        return kDefaultMaxDownload;
    }
    std::uint64_t parsed = 0;
    if (!ParseU64(value, &parsed) || parsed == 0) {
        return kDefaultMaxDownload;
    }
    return std::min<std::uint64_t>(parsed, 0xFFFFFFFFull);
}

std::uint64_t FastbootClient::PartitionSize(const std::string& partition) {
    std::string value;
    std::string error;
    if (!GetVar("partition-size:" + partition, &value, &error)) return 0;
    std::uint64_t parsed = 0;
    if (!ParseU64(value, &parsed)) return 0;
    return parsed;
}

bool FastbootClient::IsLogical(const std::string& partition) {
    std::string value;
    std::string error;
    if (!GetVar("is-logical:" + partition, &value, &error)) return false;
    return value == "yes";
}

bool FastbootClient::GetVar(const std::string& name, std::string* value,
                            std::string* error) {
    Response response;
    if (!Command("getvar:" + name, kCommandTimeoutMs, &response, error)) {
        return false;
    }
    *value = response.value;
    return true;
}

bool FastbootClient::GetVarAll(std::vector<std::string>* lines,
                               std::string* error) {
    Response response;
    if (!Command("getvar:all", kFlashTimeoutMs, &response, error)) return false;
    *lines = response.infos;
    if (!response.value.empty()) lines->push_back(response.value);
    return true;
}

bool FastbootClient::DownloadBuffer(const std::vector<std::uint8_t>& data,
                                    std::vector<std::string>* log,
                                    std::string* error) {
    if (data.empty()) {
        *error = "Image is empty";
        return false;
    }
    char command[32];
    std::snprintf(command, sizeof(command), "download:%08x",
                  static_cast<unsigned>(data.size()));
    Response response;
    if (!Command(command, kCommandTimeoutMs, &response, error)) return false;
    if (response.status != "DATA") {
        *error = "download did not return DATA";
        return false;
    }
    std::uint64_t expected = 0;
    if (!ParseU64(response.value, &expected) || expected != data.size()) {
        *error = "Device reported a different download size";
        return false;
    }
    if (!device_->Write(data.data(), data.size(), kFlashTimeoutMs, error)) {
        return false;
    }
    Response final_response;
    if (!ReadResponse(kFlashTimeoutMs, &final_response, error)) return false;
    if (final_response.status != "OKAY") {
        *error = "download did not finish with OKAY";
        return false;
    }
    if (log != nullptr) {
        log->push_back("Sent " + std::to_string(data.size()) + " bytes");
    }
    return true;
}

bool FastbootClient::DownloadFile(const std::wstring& path, std::uint64_t size,
                                  std::vector<std::string>* log,
                                  std::string* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = "Cannot open image: " + FormatWinError(GetLastError());
        return false;
    }

    char command[32];
    std::snprintf(command, sizeof(command), "download:%08x",
                  static_cast<unsigned>(size));
    Response response;
    if (!Command(command, kCommandTimeoutMs, &response, error)) {
        CloseHandle(file);
        return false;
    }
    if (response.status != "DATA") {
        CloseHandle(file);
        *error = "download did not return DATA";
        return false;
    }
    std::uint64_t expected = 0;
    if (!ParseU64(response.value, &expected) || expected != size) {
        CloseHandle(file);
        *error = "Device reported a different download size";
        return false;
    }

    std::vector<std::uint8_t> buffer(1024 * 1024);
    std::uint64_t sent = 0;
    while (sent < size) {
        DWORD request = static_cast<DWORD>(
            std::min<std::uint64_t>(buffer.size(), size - sent));
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), request, &read, nullptr) || read == 0) {
            CloseHandle(file);
            *error = "Cannot read image";
            return false;
        }
        if (!device_->Write(buffer.data(), read, kFlashTimeoutMs, error)) {
            CloseHandle(file);
            return false;
        }
        sent += read;
    }
    CloseHandle(file);

    Response final_response;
    if (!ReadResponse(kFlashTimeoutMs, &final_response, error)) return false;
    if (final_response.status != "OKAY") {
        *error = "download did not finish with OKAY";
        return false;
    }
    if (log != nullptr) {
        log->push_back("Sent " + std::to_string(size) + " bytes");
    }
    return true;
}

std::wstring FastbootClient::PrepareAvbImage(
    const std::wstring& path, std::uint64_t size, const std::string& partition,
    std::uint64_t max_download, std::string* error) {
    if (size < 64) return path;
    HANDLE source = CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source == INVALID_HANDLE_VALUE) return path;

    LARGE_INTEGER offset{};
    offset.QuadPart = static_cast<LONGLONG>(size - 64);
    if (!SetFilePointerEx(source, offset, nullptr, FILE_BEGIN)) {
        CloseHandle(source);
        return path;
    }
    char footer[64] = {0};
    DWORD read = 0;
    bool footer_ok = ReadFile(source, footer, sizeof(footer), &read, nullptr) &&
                     read == sizeof(footer) && std::memcmp(footer, "AVBf", 4) == 0;
    CloseHandle(source);
    if (!footer_ok) return path;

    std::uint64_t partition_size = PartitionSize(partition);
    if (partition_size <= size || partition_size > max_download) return path;
    if (IsLogical(partition)) return path;

    wchar_t temp_path[MAX_PATH] = {0};
    wchar_t temp_name[MAX_PATH] = {0};
    if (GetTempPathW(MAX_PATH, temp_path) == 0 ||
        GetTempFileNameW(temp_path, L"fbh", 0, temp_name) == 0) {
        return path;
    }

    HANDLE temp = CreateFileW(temp_name, GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (temp == INVALID_HANDLE_VALUE) return path;

    HANDLE input = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        CloseHandle(temp);
        DeleteFileW(temp_name);
        return path;
    }

    std::vector<std::uint8_t> buffer(1024 * 1024);
    std::uint64_t copied = 0;
    while (copied < size) {
        DWORD request = static_cast<DWORD>(
            std::min<std::uint64_t>(buffer.size(), size - copied));
        DWORD got = 0;
        if (!ReadFile(input, buffer.data(), request, &got, nullptr) || got == 0) {
            break;
        }
        DWORD written = 0;
        if (!WriteFile(temp, buffer.data(), got, &written, nullptr) ||
            written != got) {
            break;
        }
        copied += got;
    }
    CloseHandle(input);

    bool ok = copied == size;
    if (ok) {
        LARGE_INTEGER target{};
        target.QuadPart = static_cast<LONGLONG>(partition_size - 64);
        ok = SetFilePointerEx(temp, target, nullptr, FILE_BEGIN) != FALSE;
        if (ok) ok = SetEndOfFile(temp) != FALSE;
        if (ok) {
            DWORD written = 0;
            ok = WriteFile(temp, footer, sizeof(footer), &written, nullptr) &&
                 written == sizeof(footer);
        }
    }
    CloseHandle(temp);

    if (!ok) {
        DeleteFileW(temp_name);
        *error = "Failed to prepare AVB image";
        return path;
    }
    return temp_name;
}

bool FastbootClient::FlashFile(const std::string& partition,
                               const std::wstring& image,
                               std::vector<std::string>* log,
                               std::string* error) {
    std::uint64_t size = FileSize(image, error);
    if (size == 0) return false;
    bool sparse = IsSparseImage(image);
    std::uint64_t max_download = MaxDownloadSize(error);

    std::wstring prepared = image;
    std::uint64_t prepared_size = size;
    if (!sparse) {
        prepared = PrepareAvbImage(image, size, partition, max_download, error);
        if (prepared != image) {
            prepared_size = PartitionSize(partition);
            if (prepared_size == 0) {
                DeleteFileW(prepared.c_str());
                *error = "Failed to read partition size for AVB padding";
                return false;
            }
        }
    }

    if (prepared_size > max_download) {
        if (prepared != image) DeleteFileW(prepared.c_str());
        *error = "Image is larger than max-download-size (" +
                 std::to_string(max_download) +
                 "); this build supports raw boot images only";
        return false;
    }

    bool ok = DownloadFile(prepared, prepared_size, log, error);
    if (ok) {
        Response response;
        ok = Command("flash:" + partition, kFlashTimeoutMs, &response, error) &&
             response.status == "OKAY";
        if (ok && log != nullptr) log->push_back("Flashed " + partition);
    }
    if (prepared != image) DeleteFileW(prepared.c_str());
    return ok;
}

bool FastbootClient::BootFile(const std::wstring& image,
                              std::vector<std::string>* log,
                              std::string* error) {
    std::uint64_t size = FileSize(image, error);
    if (size == 0) return false;
    if (!DownloadFile(image, size, log, error)) return false;
    Response response;
    return Command("boot", kFlashTimeoutMs, &response, error) &&
           response.status == "OKAY";
}

bool FastbootClient::Erase(const std::string& partition, std::string* error) {
    Response response;
    return Command("erase:" + partition, kFlashTimeoutMs, &response, error) &&
           response.status == "OKAY";
}

bool FastbootClient::Reboot(const std::string& target, std::string* error) {
    std::string name = target;
    std::string command;
    if (name.empty() || name == "system") {
        command = "reboot";
    } else if (name == "bootloader") {
        command = "reboot-bootloader";
    } else if (name == "fastboot" || name == "fastbootd") {
        command = "reboot-fastboot";
    } else if (name == "recovery") {
        command = "reboot-recovery";
    } else if (name == "edl") {
        command = "reboot-edl";
    } else {
        command = "reboot-" + name;
    }
    if (!device_->Write(command.data(), command.size(), 2000, error)) {
        return false;
    }
    Sleep(200);
    return true;
}

bool FastbootClient::Oem(const std::string& args, std::string* error) {
    Response response;
    std::string command = args.empty() ? "oem" : "oem " + args;
    return Command(command, kFlashTimeoutMs, &response, error) &&
           response.status == "OKAY";
}

bool FastbootClient::Flashing(const std::string& action, std::string* error) {
    Response response;
    return Command("flashing " + action, kFlashTimeoutMs, &response, error) &&
           response.status == "OKAY";
}

bool FastbootClient::SetActive(const std::string& slot, std::string* error) {
    Response response;
    return Command("set_active:" + slot, kCommandTimeoutMs, &response, error) &&
           response.status == "OKAY";
}

bool FastbootClient::Continue(std::string* error) {
    Response response;
    return Command("continue", kCommandTimeoutMs, &response, error) &&
           response.status == "OKAY";
}

bool FastbootClient::Raw(const std::string& command, std::string* error) {
    Response response;
    return Command(command, kFlashTimeoutMs, &response, error) &&
           response.status == "OKAY";
}
