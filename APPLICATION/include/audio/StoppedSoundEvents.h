#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace gmod::audio {

// Stopsound cancels past starts, rather than muting future events. Retain the
// boundary across forward seeks; rewinding lets starts after the new position
// play again when playback reaches them.
class StoppedSoundEvents {
public:
    void stop(float time) { boundary_ = time; last_time_ = time; }
    void observe(float time) {
        if (boundary_ && time < last_time_) {
            // Seeking exactly to a start event should replay that event as well.
            boundary_ = std::min(*boundary_, std::nextafter(time, -std::numeric_limits<float>::infinity()));
        }
        last_time_ = time;
    }
    bool allows(float start) const { return !boundary_ || start > *boundary_; }
    bool allows_world_event(float start, int flags, bool has_loop) const {
        if (flags & 4) return true; // A stop remains effective even for a cancelled start.
        if (flags & 3) return !boundary_ || has_loop; // Volume/pitch updates cannot restart a cancelled loop.
        return allows(start);
    }

private:
    std::optional<float> boundary_;
    float last_time_ = 0.0f;
};

} // namespace gmod::audio
