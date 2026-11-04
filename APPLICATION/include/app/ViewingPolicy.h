#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

// Who may view which recording.
//  - Private edition: only a GitHub collaborator whose access the updater verified within the
//    last 7 days (DPAPI record written by tools\Update-GModReplay.ps1).
//  - Public edition: anyone, but a recording only opens once it is 15 real-world minutes old,
//    measured from when it finished recording, against network time.
namespace gmod::app {

inline constexpr char unauthorised_message[] =
    "Error: Unauthorised Access, please log in to your github account or use the public version. "
    "If you are incorrectly being denied access, please reach out to Joey.";

inline constexpr std::int64_t recording_hold_seconds = 15 * 60;
inline constexpr std::int64_t access_grace_seconds = 7 * 24 * 60 * 60;

// True when this Windows user holds verified collaborator access (always false in the public edition).
bool collaborator_verified();

struct Clearance {
    bool allowed = false;
    // How far into the recording playback may go: the full length when allowed, otherwise 0.
    float playable = 0.0f;
    std::string message;
};

// Whether `demo` may be opened now. `duration_seconds` is 0 when the length is not known yet.
Clearance clear_recording(const std::filesystem::path& demo, float duration_seconds);
// Whether free roam (no recording) may be opened.
Clearance clear_free_roam();

// Helpers, exposed for tests. Times are Unix seconds (UTC).
std::optional<std::int64_t> recording_name_start(const std::filesystem::path& demo);
std::optional<std::int64_t> recording_last_write(const std::filesystem::path& demo);
std::int64_t recording_end(std::optional<std::int64_t> last_write, std::optional<std::int64_t> name_start,
                           float duration_seconds, std::int64_t now);
std::string too_new_message(std::int64_t seconds_left);
bool access_record_current(const std::string& record, std::int64_t now);

} // namespace gmod::app
