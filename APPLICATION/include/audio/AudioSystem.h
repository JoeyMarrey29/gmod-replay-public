#pragma once

#include "source/bsp/BspReader.h"
#include "audio/SoundCategory.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gmod::audio {

// Plays the map's ambient_generic emitters and trigger_soundscape soundscapes
// on a background thread (XAudio2 output, Media Foundation decoding).
// Positions are Source units; failures never stop the viewer, they only
// silence audio and are written to the log.
class AudioSystem {
public:
    static constexpr float MaxOneShotSourceSeconds = 120.0f;

    AudioSystem(
        std::vector<std::filesystem::path> content_roots,
        std::filesystem::path log_path,
        source::bsp::AudioScene scene);
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    // `right` is the listener's right vector; both are in Source space.
    void set_listener(const float position[3], const float right[3]);
    void toggle_mute();
    // The viewer's own volume setting, 0..1, applied on top of every sound.
    void set_master_volume(float volume);
    void set_category_volume(SoundCategory category, float volume);

    // Demo voice chat. Clips are identified by the caller; positions are Source units.
    // `rate` is the playback speed (it also shifts pitch). Commands are applied on the
    // audio thread; unknown or finished clips are ignored.
    // `flat` plays the clip at full level everywhere (government radio) instead of at the
    // speaker's position.
    void voice_start(int clip, const std::filesystem::path& wav, float offset_seconds,
                     float rate, const float position[3], bool flat = false);
    void voice_move(int clip, float rate, const float position[3], bool flat = false);
    void voice_stop(int clip);
    void voice_stop_all();
    // One-shot positional sound from the game's sound folders (`sound` is relative to
    // them, e.g. "player/footsteps/concrete1.wav"). Loudness falls off to silence at `radius`.
    // `offset_seconds` is source-file time, independent of playback rate; expired clips are skipped.
    void play_sound(const std::string& sound, const float position[3], float gain, float radius,
                    float rate = 1.0f, float offset_seconds = 0.0f);
    // One-shot heard the way Perpheads' gun sounds are (cl_gun_sounds.lua): volume
    // min(1, (level / distance)^0.75 * volume), with no other distance fade.
    void play_level_sound(const std::string& sound, const float position[3], float level, float volume,
                          float rate = 1.0f, float offset_seconds = 0.0f);
    // A looping world sound (an engine, a siren) under a caller-chosen id: started when the id
    // is new, otherwise moved and re-levelled (a different `sound` restarts it).
    void loop_set(int id, const std::string& sound, const float position[3], float gain,
                  float radius, float rate = 1.0f);
    void loop_stop(int id);
    void loop_stop_all();
    // Cancels demo effects (one-shots and loops), leaving voice chat and map ambience alone.
    void world_stop_all();
    // Console stopsound: cancel all current effects and map ambience, preserving
    // voice chat. The current soundscape stays stopped until a new trigger starts it.
    void stop_sounds();
    // Freezes/resumes demo effects and map ambience in place, including soundscape
    // scheduling. Voice chat is controlled independently by the replay.
    void world_set_paused(bool paused);
    std::wstring status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gmod::audio
