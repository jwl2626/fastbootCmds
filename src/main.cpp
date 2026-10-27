#include "fastboot_client.h"
#include "usb_path_windows.h"
#include "winusb_fastboot.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Job {
    FastbootDeviceInfo device;
    std::vector<std::wstring> command;
    bool ok = false;
    std::string output;
};

std::string WideToUtf8(const std::wstring& value) {
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

std::wstring Lower(const std::wstring& value) {
    std::wstring result = value;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return result;
}

bool IsInteger(const std::wstring& value) {
    if (value.empty()) return false;
    for (wchar_t ch : value) {
        if (ch < L'0' || ch > L'9') return false;
    }
    return true;
}

void PrintUsage() {
    std::printf(
        "fastbootCmds - 多设备 fastboot 命令工具\n"
        "\n"
        "用法:\n"
        "  fastbootCmds\n"
        "  fastbootCmds devices\n"
        "  fastbootCmds <命令> [hub index]...\n"
        "  fastbootCmds FASTBOOTCMD <命令> [hub index]...\n"
        "  fastbootCmds FASTBOOTFLASH flash <分区> <镜像> [hub index]...\n"
        "  fastbootCmds ... all\n"
        "\n"
        "说明:\n"
        "  hub/index 对应 USB 物理路径的最后两级端口，例如 1 3 对应 usb:1D00:0-1.3。\n"
        "  不带 hub/index 时，默认使用第一台 fastboot 设备。\n"
        "  可以重复写多组 hub index，例如 1 3 1 4，表示同时操作两台设备。\n"
        "  all 表示操作当前所有 fastboot 设备。\n"
        "\n"
        "示例:\n"
        "  fastbootCmds reboot 1 3\n"
        "  fastbootCmds FASTBOOTCMD getvar 1 3\n"
        "  fastbootCmds FASTBOOTFLASH flash boot boot.img 1 3\n"
        "  fastbootCmds FASTBOOTCMD erase misc\n"
        "  fastbootCmds FASTBOOTCMD reboot bootloader 1 3\n"
        "  fastbootCmds FASTBOOTCMD oem recovery lowpoer 1 3\n"
        "  fastbootCmds FASTBOOTCMD getvar all\n"
        "\n"
        "fastbootCmds 与 fastboot.exe 使用相同的 fastboot 协议；\n"
        "不需要 fastboot.exe，也不依赖设备序列号。\n");
}

bool StripPrefix(std::vector<std::wstring>* args, bool* is_flash) {
    if (args->empty()) return false;
    std::wstring first = Lower(args->front());
    if (first == L"fastbootcmd" || first == L"fastbootflash") {
        *is_flash = first == L"fastbootflash";
        args->erase(args->begin());
    }
    return true;
}

bool ParseCommand(const std::vector<std::wstring>& raw,
                  std::vector<std::wstring>* command,
                  std::vector<std::pair<int, int>>* targets, bool* use_all,
                  std::string* error) {
    *command = raw;
    *use_all = false;
    bool is_flash = false;
    StripPrefix(command, &is_flash);
    if (command->empty()) {
        *error = "missing command";
        return false;
    }

    size_t end = command->size();
    while (end > 0 && IsInteger(command->at(end - 1))) --end;
    size_t numeric_count = command->size() - end;
    if (numeric_count >= 2 && numeric_count % 2 == 0) {
        for (size_t i = end; i < command->size(); i += 2) {
            int hub = _wtoi(command->at(i).c_str());
            int index = _wtoi(command->at(i + 1).c_str());
            targets->emplace_back(hub, index);
        }
        command->resize(end);
    } else if (!command->empty() && Lower(command->back()) == L"all") {
        *use_all = true;
        command->pop_back();
    }

    if (command->empty()) {
        *error = "missing command";
        return false;
    }
    if (is_flash && Lower(command->front()) != L"flash") {
        *error = "FASTBOOTFLASH requires: flash <partition> <image>";
        return false;
    }
    return true;
}

bool SelectDevices(const std::vector<FastbootDeviceInfo>& available,
                   const std::vector<std::pair<int, int>>& targets,
                   bool use_all, std::vector<FastbootDeviceInfo>* selected,
                   std::string* error) {
    selected->clear();
    if (use_all) {
        *selected = available;
        return !selected->empty();
    }
    if (targets.empty()) {
        selected->push_back(available.front());
        return true;
    }
    for (const auto& target : targets) {
        bool found = false;
        for (const auto& device : available) {
            if (fastboot::UsbPathMatches(device.location.c_str(), target.first,
                                         target.second)) {
                if (std::find_if(selected->begin(), selected->end(),
                                 [&](const FastbootDeviceInfo& item) {
                                     return item.interface_path ==
                                            device.interface_path;
                                 }) == selected->end()) {
                    selected->push_back(device);
                }
                found = true;
                break;
            }
        }
        if (!found) {
            *error = "No device matches hub " + std::to_string(target.first) +
                     " index " + std::to_string(target.second);
            return false;
        }
    }
    return true;
}

std::string Join(const std::vector<std::wstring>& values, size_t start) {
    std::string result;
    for (size_t i = start; i < values.size(); ++i) {
        if (!result.empty()) result.push_back(' ');
        result += WideToUtf8(values[i]);
    }
    return result;
}

bool RunJob(Job* job) {
    FastbootUsbDevice device;
    std::string error;
    if (!device.OpenPath(job->device.interface_path, &error)) {
        job->output += error + "\n";
        return false;
    }

    FastbootClient client(&device);
    const auto& args = job->command;
    std::wstring command = Lower(args[0]);

    if (command == L"reboot") {
        std::string target = args.size() > 1 ? WideToUtf8(args[1]) : "";
        if (!client.Reboot(target, &error)) {
            job->output += error + "\n";
            return false;
        }
        job->output += "reboot command sent\n";
    } else if (command == L"getvar") {
        if (args.size() > 1) {
            std::string value;
            std::string name = WideToUtf8(args[1]);
            if (!client.GetVar(name, &value, &error)) {
                job->output += error + "\n";
                return false;
            }
            job->output += name + ": " + value + "\n";
        } else {
            std::vector<std::string> lines;
            if (!client.GetVarAll(&lines, &error)) {
                job->output += error + "\n";
                return false;
            }
            for (const auto& line : lines) job->output += line + "\n";
        }
    } else if (command == L"erase") {
        if (args.size() < 2) {
            job->output += "erase requires a partition\n";
            return false;
        }
        if (!client.Erase(WideToUtf8(args[1]), &error)) {
            job->output += error + "\n";
            return false;
        }
        job->output += "erased " + WideToUtf8(args[1]) + "\n";
    } else if (command == L"flash") {
        if (args.size() < 3) {
            job->output += "flash requires: flash <partition> <image>\n";
            return false;
        }
        std::vector<std::string> log;
        if (!client.FlashFile(WideToUtf8(args[1]), args[2], &log, &error)) {
            for (const auto& line : log) job->output += line + "\n";
            job->output += error + "\n";
            return false;
        }
        for (const auto& line : log) job->output += line + "\n";
    } else if (command == L"oem") {
        if (!client.Oem(Join(args, 1), &error)) {
            job->output += error + "\n";
            return false;
        }
        job->output += "oem OK\n";
    } else if (command == L"flashing") {
        if (args.size() < 2) {
            job->output += "flashing requires an action\n";
            return false;
        }
        if (!client.Flashing(WideToUtf8(args[1]), &error)) {
            job->output += error + "\n";
            return false;
        }
        job->output += "flashing OK\n";
    } else if (command == L"set_active") {
        if (args.size() < 2) {
            job->output += "set_active requires a slot\n";
            return false;
        }
        if (!client.SetActive(WideToUtf8(args[1]), &error)) {
            job->output += error + "\n";
            return false;
        }
        job->output += "set_active OK\n";
    } else if (command == L"boot") {
        if (args.size() < 2) {
            job->output += "boot requires an image\n";
            return false;
        }
        std::vector<std::string> log;
        if (!client.BootFile(args[1], &log, &error)) {
            for (const auto& line : log) job->output += line + "\n";
            job->output += error + "\n";
            return false;
        }
        for (const auto& line : log) job->output += line + "\n";
    } else if (command == L"continue") {
        if (!client.Continue(&error)) {
            job->output += error + "\n";
            return false;
        }
        job->output += "continue OK\n";
    } else {
        std::string raw = Join(args, 0);
        if (!client.Raw(raw, &error)) {
            job->output += error + "\n";
            return false;
        }
        job->output += "OK\n";
    }

    device.Close();
    return true;
}

DWORD WINAPI JobThread(LPVOID parameter) {
    auto* job = static_cast<Job*>(parameter);
    job->ok = RunJob(job);
    return 0;
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == nullptr) {
        std::printf("无法读取命令行参数\n");
        return 1;
    }
    std::vector<std::wstring> raw;
    for (int i = 1; i < argc; ++i) raw.emplace_back(argv[i]);
    LocalFree(argv);

    if (raw.empty()) {
        PrintUsage();
        return 0;
    }

    std::wstring first = Lower(raw.front());
    if (first == L"help" || first == L"--help" || first == L"-h") {
        PrintUsage();
        return 0;
    }
    if (first == L"devices" || first == L"list") {
        std::vector<FastbootDeviceInfo> devices;
        std::string error;
        if (!EnumerateFastbootDevices(&devices, &error)) {
            std::printf("%s\n", error.c_str());
            return 1;
        }
        for (const auto& device : devices) {
            std::printf("%s  SN=%s  %04x:%04x\n", device.location.c_str(),
                        device.serial.empty() ? "-" : device.serial.c_str(),
                        device.vendor_id, device.product_id);
        }
        return 0;
    }

    std::vector<std::wstring> command;
    std::vector<std::pair<int, int>> targets;
    bool use_all = false;
    std::string error;
    if (!ParseCommand(raw, &command, &targets, &use_all, &error)) {
        std::printf("参数错误: %s\n\n", error.c_str());
        PrintUsage();
        return 1;
    }

    std::vector<FastbootDeviceInfo> available;
    if (!EnumerateFastbootDevices(&available, &error)) {
        std::printf("%s\n", error.c_str());
        return 1;
    }

    std::vector<FastbootDeviceInfo> selected;
    if (!SelectDevices(available, targets, use_all, &selected, &error)) {
        std::printf("%s\n", error.c_str());
        std::printf("当前设备:\n");
        for (const auto& device : available) {
            std::printf("  %s  SN=%s\n", device.location.c_str(),
                        device.serial.empty() ? "-" : device.serial.c_str());
        }
        return 1;
    }

    std::vector<Job> jobs(selected.size());
    for (size_t i = 0; i < selected.size(); ++i) {
        jobs[i].device = selected[i];
        jobs[i].command = command;
    }

    if (jobs.size() == 1) {
        jobs[0].ok = RunJob(&jobs[0]);
    } else {
        std::vector<HANDLE> threads;
        threads.reserve(jobs.size());
        for (auto& job : jobs) {
            HANDLE thread = CreateThread(nullptr, 0, JobThread, &job, 0, nullptr);
            if (thread != nullptr) {
                threads.push_back(thread);
            } else {
                job.output += "CreateThread failed\n";
            }
        }
        for (HANDLE thread : threads) {
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
    }

    bool all_ok = true;
    for (const auto& job : jobs) {
        std::printf("==== %s ====\n", job.device.location.c_str());
        std::printf("%s", job.output.c_str());
        if (!job.ok) all_ok = false;
    }
    return all_ok ? 0 : 1;
}
