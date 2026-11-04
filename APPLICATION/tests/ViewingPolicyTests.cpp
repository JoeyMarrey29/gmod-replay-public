#include "app/ViewingPolicy.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void message_tests() {
    require(gmod::app::too_new_message(1) == "Error: demo is too new to view, please wait 1 minute",
            "one second rounds up to one minute");
    require(gmod::app::too_new_message(60) == "Error: demo is too new to view, please wait 1 minute",
            "exactly one minute");
    require(gmod::app::too_new_message(61) == "Error: demo is too new to view, please wait 2 minutes",
            "partial minutes round up");
    require(gmod::app::too_new_message(900) == "Error: demo is too new to view, please wait 15 minutes",
            "full hold");
    require(std::string(gmod::app::unauthorised_message)
                == "Error: Unauthorised Access, please log in to your github account or use the public version. "
                   "If you are incorrectly being denied access, please reach out to Joey.",
            "unauthorised wording is exact");
}

void recording_end_tests() {
    const std::int64_t now = 1'800'000'000;
    // Last write wins when it is later than the name.
    require(gmod::app::recording_end(now - 100, now - 4000, 600.0f, now) == now - 100, "last write later");
    // Name start + length wins when a copy has an older last-write time.
    require(gmod::app::recording_end(now - 9000, now - 4000, 600.0f, now) == now - 3400, "named end later");
    // A name that claims the future is ignored.
    require(gmod::app::recording_end(now - 9000, now + 3600, 0.0f, now) == now - 9000, "future name ignored");
    // No timestamps at all means the recording counts as brand new.
    require(gmod::app::recording_end(std::nullopt, std::nullopt, 0.0f, now) == now, "unknown is new");
}

void name_tests() {
    require(gmod::app::recording_name_start(L"perpheads_demo_2026-11-4 19-02-33.dem").has_value(),
            "perpheads name parses");
    require(!gmod::app::recording_name_start(L"something.dem").has_value(), "plain name has no start");
    require(!gmod::app::recording_name_start(L"perpheads_demo_2026-13-4 19-02-33.dem").has_value(),
            "bad month rejected");
}

void access_tests() {
    const std::int64_t now = 1'800'000'000;
    const auto record = [](std::int64_t verified, const char* repository = "1428373505") {
        return std::string("{\"login\":\"joey\",\"repositoryId\":\"") + repository + "\",\"verifiedAt\":"
            + std::to_string(verified) + "}";
    };
    require(gmod::app::access_record_current(record(now - 60), now), "fresh record");
    require(gmod::app::access_record_current(record(now - 6 * 86400), now), "inside grace");
    require(!gmod::app::access_record_current(record(now - 8 * 86400), now), "grace expired");
    require(!gmod::app::access_record_current(record(now + 3 * 86400), now), "future record rejected");
    require(!gmod::app::access_record_current(record(now, "1"), now), "other repository rejected");
    require(!gmod::app::access_record_current("{}", now), "empty record rejected");
}

} // namespace

int main() {
    try {
        message_tests();
        recording_end_tests();
        name_tests();
        access_tests();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Viewing policy tests passed\n";
    return 0;
}
