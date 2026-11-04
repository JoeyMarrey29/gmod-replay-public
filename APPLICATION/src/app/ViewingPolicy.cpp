#include "app/ViewingPolicy.h"

#include "app/Edition.h"

#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <mutex>
#include <vector>

namespace gmod::app {
namespace {

constexpr std::int64_t FileTimeUnixOffset = 116444736000000000LL;
constexpr char AccessRepositoryId[] = "1428373505";
constexpr char AccessEntropy[] = "GModReplay.Access.v1";
// Offline, the local clock is only trusted for recordings that are clearly old.
constexpr std::int64_t OfflineTrustSeconds = 2 * 60 * 60;

std::int64_t unix_from_filetime(const FILETIME& time) {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return (static_cast<std::int64_t>(value.QuadPart) - FileTimeUnixOffset) / 10000000LL;
}

std::optional<std::int64_t> unix_from_utc(const SYSTEMTIME& time) {
    FILETIME file{};
    if (!SystemTimeToFileTime(&time, &file)) return std::nullopt;
    return unix_from_filetime(file);
}

std::int64_t local_clock() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

// The Date header GitHub's API returns, so a changed Windows clock cannot unlock a recording.
std::optional<std::int64_t> query_network_time() {
    std::optional<std::int64_t> result;
    HINTERNET session = WinHttpOpen(L"GModReplay/time", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return result;
    WinHttpSetTimeouts(session, 3000, 3000, 3000, 3000);
    HINTERNET connection = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection
        ? WinHttpOpenRequest(connection, L"HEAD", L"/", nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
        : nullptr;
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr)) {
        SYSTEMTIME date{};
        DWORD size = sizeof(date);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_DATE | WINHTTP_QUERY_FLAG_SYSTEMTIME,
                                WINHTTP_HEADER_NAME_BY_INDEX, &date, &size, WINHTTP_NO_HEADER_INDEX)) {
            result = unix_from_utc(date);
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return result;
}

// Network time, fetched once per run and then advanced by the monotonic tick counter.
std::optional<std::int64_t> network_now() {
    static std::mutex lock;
    static std::optional<std::int64_t> anchor;
    static ULONGLONG anchor_tick = 0;
    std::lock_guard guard(lock);
    if (!anchor) {
        anchor = query_network_time();
        anchor_tick = GetTickCount64();
        if (!anchor) return std::nullopt;
    }
    return *anchor + static_cast<std::int64_t>((GetTickCount64() - anchor_tick) / 1000);
}

std::optional<std::filesystem::path> access_record_path() {
    PWSTR local = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return std::nullopt;
    std::filesystem::path path = std::filesystem::path(local) / L"GModReplay" / L"updates" / L"access.dpapi";
    CoTaskMemFree(local);
    return path;
}

std::optional<std::string> read_access_record() {
    const auto path = access_record_path();
    if (!path) return std::nullopt;
    std::ifstream input(*path, std::ios::binary);
    if (!input) return std::nullopt;
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() > 65536) return std::nullopt;
    DATA_BLOB sealed{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(AccessEntropy) - 1),
                      reinterpret_cast<BYTE*>(const_cast<char*>(AccessEntropy))};
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&sealed, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
        return std::nullopt;
    }
    std::string text(reinterpret_cast<const char*>(plain.pbData), plain.cbData);
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    return text;
}

std::optional<std::int64_t> json_integer(const std::string& text, const std::string& key) {
    const auto at = text.find("\"" + key + "\"");
    if (at == std::string::npos) return std::nullopt;
    auto position = text.find(':', at);
    if (position == std::string::npos) return std::nullopt;
    ++position;
    while (position < text.size() && (text[position] == ' ' || text[position] == '"')) ++position;
    std::int64_t value = 0;
    bool digits = false;
    while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
        value = value * 10 + (text[position++] - '0');
        digits = true;
    }
    if (!digits) return std::nullopt;
    return value;
}

} // namespace

bool access_record_current(const std::string& record, std::int64_t now) {
    const auto repository = json_integer(record, "repositoryId");
    const auto verified = json_integer(record, "verifiedAt");
    if (!repository || std::to_string(*repository) != AccessRepositoryId || !verified) return false;
    // A record from the future means the clock was wound back after verifying.
    return *verified <= now + 24 * 60 * 60 && now - *verified <= access_grace_seconds;
}

bool collaborator_verified() {
    if constexpr (edition::is_public) {
        return false;
    } else {
        const auto record = read_access_record();
        return record && access_record_current(*record, local_clock());
    }
}

std::optional<std::int64_t> recording_name_start(const std::filesystem::path& demo) {
    // perpheads_demo_2026-11-4 19-02-33.dem: local start date and time.
    const std::wstring name = demo.stem().wstring();
    const auto space = name.rfind(L' ');
    const auto underscore = name.rfind(L'_', space);
    if (space == std::wstring::npos || underscore == std::wstring::npos) return std::nullopt;
    int year = 0, month = 0, day = 0, hour = -1, minute = -1, second = -1;
    if (swscanf_s(name.c_str() + underscore + 1, L"%d-%d-%d", &year, &month, &day) != 3
        || swscanf_s(name.c_str() + space + 1, L"%d-%d-%d", &hour, &minute, &second) != 3) {
        return std::nullopt;
    }
    if (year < 2000 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23
        || minute < 0 || minute > 59 || second < 0 || second > 59) {
        return std::nullopt;
    }
    SYSTEMTIME local{};
    local.wYear = static_cast<WORD>(year);
    local.wMonth = static_cast<WORD>(month);
    local.wDay = static_cast<WORD>(day);
    local.wHour = static_cast<WORD>(hour);
    local.wMinute = static_cast<WORD>(minute);
    local.wSecond = static_cast<WORD>(second);
    SYSTEMTIME utc{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc)) return std::nullopt;
    return unix_from_utc(utc);
}

std::optional<std::int64_t> recording_last_write(const std::filesystem::path& demo) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(demo.c_str(), GetFileExInfoStandard, &data)) return std::nullopt;
    return unix_from_filetime(data.ftLastWriteTime);
}

std::int64_t recording_end(std::optional<std::int64_t> last_write, std::optional<std::int64_t> name_start,
                           float duration_seconds, std::int64_t now) {
    // Copying, moving or renaming keeps the last-write time, so the hold does not restart.
    // Creation time is never used: copies get a new one.
    std::int64_t end = last_write.value_or(now);
    if (name_start) {
        const auto named_end = *name_start + static_cast<std::int64_t>(std::max(duration_seconds, 0.0f));
        // A name from another time zone can claim a future start; only trust it when plausible.
        if (named_end <= now + 5 * 60) end = std::max(end, named_end);
    }
    return end;
}

std::string too_new_message(std::int64_t seconds_left) {
    const auto minutes = std::max<std::int64_t>(1, (seconds_left + 59) / 60);
    return "Error: demo is too new to view, please wait " + std::to_string(minutes)
        + (minutes == 1 ? " minute" : " minutes");
}

Clearance clear_free_roam() {
    Clearance clearance;
    if (edition::is_public || collaborator_verified()) {
        clearance.allowed = true;
    } else {
        clearance.message = unauthorised_message;
    }
    return clearance;
}

Clearance clear_recording(const std::filesystem::path& demo, float duration_seconds) {
    Clearance clearance;
    const float playable = std::max(duration_seconds, 0.0f);
    if (collaborator_verified()) {
        clearance.allowed = true;
        clearance.playable = playable;
        return clearance;
    }
    if (!edition::is_public) {
        clearance.message = unauthorised_message;
        return clearance;
    }
    const auto last_write = recording_last_write(demo);
    const auto name_start = recording_name_start(demo);
    if (const auto now = network_now()) {
        const auto left = recording_end(last_write, name_start, duration_seconds, *now)
            + recording_hold_seconds - *now;
        if (left > 0) {
            clearance.message = too_new_message(left);
            return clearance;
        }
    } else {
        const auto local = local_clock();
        if (local - recording_end(last_write, name_start, duration_seconds, local) < OfflineTrustSeconds) {
            clearance.message = "Error: could not check the current time. Connect to the internet to open "
                                "demos recorded in the last 2 hours.";
            return clearance;
        }
    }
    clearance.allowed = true;
    clearance.playable = playable;
    return clearance;
}

} // namespace gmod::app
