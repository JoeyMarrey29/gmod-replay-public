#include "ui/Updates.h"

#include "app/Edition.h"

#include <shlobj.h>

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace gmod::ui {
namespace {

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::map<std::string, std::string> parse_update_status(const std::string& text) {
    std::map<std::string, std::string> result;
    std::istringstream input(text);
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto split = line.find('=');
        if (split == std::string::npos || split == 0) throw std::runtime_error("Invalid updater status field.");
        std::string value;
        for (std::size_t i = split + 1; i < line.size(); ++i) {
            if (line[i] == '%') {
                if (i + 2 >= line.size()) throw std::runtime_error("Incomplete updater status escape.");
                const int high = hex_digit(line[i + 1]), low = hex_digit(line[i + 2]);
                if (high < 0 || low < 0) throw std::runtime_error("Invalid updater status escape.");
                value.push_back(static_cast<char>((high << 4) | low));
                i += 2;
            } else {
                value.push_back(line[i]);
            }
        }
        if (!result.emplace(line.substr(0, split), std::move(value)).second) {
            throw std::runtime_error("Duplicate updater status field.");
        }
    }
    if (!result.contains("status") || !result.contains("message")) {
        throw std::runtime_error("Updater status is missing required fields.");
    }
    return result;
}

Updates::Updates(std::filesystem::path root) : root_(std::move(root)) {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        throw std::runtime_error("Could not locate the update settings folder.");
    }
    status_path_ = std::filesystem::path(local) / gmod::edition::state_folder / L"updates"
        / (L"ui-" + std::to_wstring(GetCurrentProcessId()) + L".status");
    CoTaskMemFree(local);
}

Updates::~Updates() {
    if (process_) CloseHandle(process_);
}

std::string Updates::value(const std::string& key) const {
    const auto found = status_.find(key);
    return found == status_.end() ? std::string() : found->second;
}

void Updates::fail(const std::string& message) {
    status_["status"] = "error";
    status_["message"] = message;
    std::error_code error;
    if (std::filesystem::is_directory(status_path_.parent_path(), error)) {
        std::ofstream log(status_path_.parent_path() / L"updates.log", std::ios::app);
        log << "Native update panel: " << message << '\n';
    }
}

void Updates::start(const std::string& operation) {
    if (busy()) return;
    try {
        const auto script = root_ / L"tools" / L"Update-GModReplay.ps1";
        if (!std::filesystem::is_regular_file(script)) {
            throw std::runtime_error("Update controls are available in installed release builds.");
        }
        std::filesystem::create_directories(status_path_.parent_path());
        std::filesystem::remove(status_path_);
        std::filesystem::remove(status_path_.wstring() + L".cancel");
        wchar_t system[MAX_PATH]{};
        if (!GetSystemDirectoryW(system, MAX_PATH)) throw std::runtime_error("Could not locate Windows PowerShell.");
        const auto executable = std::filesystem::path(system) / L"WindowsPowerShell\\v1.0\\powershell.exe";
        std::wstring command = L"\"" + executable.wstring()
            + L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + script.wstring()
            + L"\" -Operation " + std::wstring(operation.begin(), operation.end())
            + L" -StatusFile \"" + status_path_.wstring() + L"\" -ViewerPid " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, root_.c_str(), &startup, &process)) {
            throw std::runtime_error("Could not start the update service (Windows error "
                + std::to_string(GetLastError()) + ").");
        }
        CloseHandle(process.hThread);
        process_ = process.hProcess;
        last_status_.clear();
        status_["status"] = "working";
        status_["message"] = operation == "link" ? "Requesting a GitHub sign-in code..." :
            operation == "install" ? "Verifying update before installation..." : "Loading update status...";
        status_.erase("code");
    } catch (const std::exception& error) {
        fail(error.what());
    }
}

void Updates::cancel() {
    if (!busy()) return;
    std::ofstream output(status_path_.wstring() + L".cancel");
    output << "cancel\n";
    if (!output) {
        fail("Could not cancel this update task. Close the panel and try again.");
    }
}

bool Updates::poll() {
    bool changed = false;
    const auto read_status = [&] {
        const HANDLE input = CreateFileW(status_path_.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (input == INVALID_HANDLE_VALUE) return;
        const DWORD length = GetFileSize(input, nullptr);
        if (length > 131072) {
            CloseHandle(input);
            fail("Update service returned an oversized status message.");
            changed = true;
            return;
        }
        std::string text(length, '\0');
        DWORD read = 0;
        const bool ok = ReadFile(input, text.data(), length, &read, nullptr) != FALSE && read == length;
        CloseHandle(input);
        if (!ok) {
            fail("Could not read update status. Check permissions on the local update folder.");
            changed = true;
            return;
        }
        if (text != last_status_) {
            try {
                status_ = parse_update_status(text);
            } catch (const std::exception& error) {
                fail(error.what());
            }
            last_status_ = text;
            changed = true;
        }
    };
    read_status();
    if (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        CloseHandle(process_);
        process_ = nullptr;
        read_status();
        if (last_status_.empty() || (code != 0 && value("status") != "error")) {
            fail("The update service exited before completing (code "
                + std::to_string(code) + "). Try again or use the manual installer.");
        }
        changed = true;
    }
    return changed;
}

} // namespace gmod::ui
