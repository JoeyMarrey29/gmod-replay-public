#include "audio/AudioSystem.h"

#include <windows.h>

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void wait_for_voices(const gmod::audio::AudioSystem& audio, int count,
                     std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto expected = L" voices " + std::to_wstring(count);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = audio.status();
        if (status.ends_with(expected)) return;
        if (status == L"snd unavailable" || status == L"snd error")
            throw std::runtime_error("Audio device or decoding failed during playback regression.");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    throw std::runtime_error("Audio voice count did not reach " + std::to_string(count));
}

void wait_for_minimum_voices(const gmod::audio::AudioSystem& audio, int count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = audio.status();
        const auto at = status.find(L" voices ");
        if (at != std::wstring::npos && std::stoi(status.substr(at + 8)) >= count) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    throw std::runtime_error("Soundscape did not start its looping and random sounds.");
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path()
        / ("gmod-replay-audio-" + std::to_string(GetCurrentProcessId()));
    const auto sound = root / "sound" / "alarm.wav";
    const auto short_sound = root / "sound" / "effect.wav";
    const auto expired_sound = root / "sound" / "expired.wav";
    const auto log = root / "audio.log";
    try {
        std::filesystem::create_directories(sound.parent_path());
        const auto write_fixture = [](const std::filesystem::path& path, std::uint32_t seconds) {
            std::ofstream file(path, std::ios::binary);
            const auto u16 = [&](std::uint16_t value) {
                const char bytes[]{static_cast<char>(value), static_cast<char>(value >> 8)};
                file.write(bytes, sizeof(bytes));
            };
            const auto u32 = [&](std::uint32_t value) {
                u16(static_cast<std::uint16_t>(value));
                u16(static_cast<std::uint16_t>(value >> 16));
            };
            const std::uint32_t samples = 44100 * seconds;
            file.write("RIFF", 4); u32(36 + samples * 2); file.write("WAVEfmt ", 8);
            u32(16); u16(1); u16(1); u32(44100); u32(88200); u16(2); u16(16);
            file.write("data", 4); u32(samples * 2);
            for (std::uint32_t i = 0; i < samples; ++i) u16(0);
            if (!file) throw std::runtime_error("Could not write finite alarm fixture.");
        };
        write_fixture(sound, 32);
        write_fixture(short_sound, 2);
        write_fixture(expired_sound, 2);
        {
            gmod::source::bsp::AudioScene scene;
            gmod::source::bsp::AmbientEmitter ambient;
            ambient.sound = "alarm.wav";
            scene.ambient.push_back(ambient);
            gmod::audio::AudioSystem audio({root}, log, std::move(scene));
            audio.set_master_volume(0);
            const float position[3]{}, right[3]{1, 0, 0};
            audio.set_listener(position, right);
            wait_for_voices(audio, 1);
            audio.voice_start(0, sound, 0, 1, position);
            audio.play_sound("alarm.wav", position, 1, 1000);
            audio.loop_set(1, "alarm.wav", position, 1, 1000);
            wait_for_voices(audio, 4);
            // Category mutes/relevels must preserve ongoing loops and voice positions.
            audio.set_category_volume(gmod::audio::SoundCategory::Voices, 0);
            audio.set_category_volume(gmod::audio::SoundCategory::Alarms, 0);
            wait_for_voices(audio, 4);
            audio.set_category_volume(gmod::audio::SoundCategory::Voices, 1);
            audio.set_category_volume(gmod::audio::SoundCategory::Alarms, 0.5f);
            audio.voice_stop_all();
            wait_for_voices(audio, 3);
            audio.voice_start(0, sound, 0, 1, position);
            wait_for_voices(audio, 4);
            audio.world_stop_all();
            wait_for_voices(audio, 2);
            audio.world_stop_all();
            wait_for_voices(audio, 2);
            audio.voice_stop_all();
            wait_for_voices(audio, 1);
            audio.play_sound("alarm.wav", position, 1, 1000);
            wait_for_voices(audio, 2);
            audio.world_stop_all();
            wait_for_voices(audio, 1);
            audio.play_sound("alarm.wav", position, 1, 1000);
            audio.world_stop_all();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            wait_for_voices(audio, 1);

            audio.voice_start(0, sound, 0, 1, position);
            audio.play_sound("effect.wav", position, 1, 1000);
            audio.play_level_sound("effect.wav", position, 1000, 1);
            audio.loop_set(1, "alarm.wav", position, 1, 1000);
            wait_for_voices(audio, 5);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            audio.world_set_paused(true);
            audio.world_set_paused(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(2200));
            wait_for_voices(audio, 5);
            audio.world_set_paused(false);
            audio.world_set_paused(false);
            // Both two-second effects must finish their remaining second, not restart.
            wait_for_voices(audio, 3, std::chrono::milliseconds(1500));

            audio.world_set_paused(true);
            audio.play_sound("effect.wav", position, 1, 1000);
            std::this_thread::sleep_for(std::chrono::milliseconds(2200));
            wait_for_voices(audio, 4);
            // Seeking while paused discards retained buffers; resume cannot resurrect them.
            audio.world_stop_all();
            wait_for_voices(audio, 2);
            audio.world_set_paused(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            wait_for_voices(audio, 2);
            audio.voice_stop_all();
            wait_for_voices(audio, 1);

            audio.play_sound("alarm.wav", position, 1, 1000, 1, 31);
            audio.play_level_sound("alarm.wav", position, 1000, 1, 1, 31);
            wait_for_voices(audio, 3);
            wait_for_voices(audio, 1, std::chrono::milliseconds(1500));
            audio.play_sound("alarm.wav", position, 1, 1000, 1, 32);
            audio.play_level_sound("alarm.wav", position, 1000, 1, 1, 40);
            audio.play_sound("expired.wav", position, 1, 1000, 1, 3);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            wait_for_voices(audio, 1);

            audio.world_set_paused(true);
            audio.play_sound("alarm.wav", position, 1, 1000, 0.5f, 31);
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            wait_for_voices(audio, 2);
            audio.world_set_paused(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            wait_for_voices(audio, 2);
            wait_for_voices(audio, 1, std::chrono::milliseconds(1500));
        }
        {
            std::ifstream file(log);
            const std::string text((std::istreambuf_iterator<char>(file)), {});
            if (text.find("LOADED expired.wav") != std::string::npos)
                throw std::runtime_error("Seeking decoded an already-expired uncached effect.");
            if (text.find("ONE_SHOT_RESTORED alarm.wav offset=31") == std::string::npos)
                throw std::runtime_error("Missing source-offset playback diagnostic.");
        }
        const auto pause_log = root / "ambient-pause.log";
        {
            gmod::source::bsp::AudioScene scene;
            gmod::source::bsp::AmbientEmitter ambient;
            ambient.sound = "effect.wav";
            ambient.flags = 32; // Finite ambient_generic, not a looping emitter.
            scene.ambient.push_back(ambient);
            gmod::audio::AudioSystem audio({root}, pause_log, std::move(scene));
            audio.set_master_volume(0);
            const float position[3]{}, right[3]{1, 0, 0};
            audio.set_listener(position, right);
            wait_for_voices(audio, 1);
            audio.voice_start(0, short_sound, 0, 1, position);
            wait_for_voices(audio, 2);
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
            audio.world_set_paused(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(2200));
            wait_for_voices(audio, 1); // Voice chat finishes independently; ambience retains its buffer.
            audio.world_set_paused(false);
            wait_for_voices(audio, 0, std::chrono::milliseconds(1700));
        }
        const auto scripts = root / "scripts";
        const auto soundscape_script = scripts / "soundscapes_rp_paralake_city_v6.txt";
        const auto stop_log = root / "stopsound.log";
        std::filesystem::create_directories(scripts);
        {
            std::ofstream script(soundscape_script);
            script << R"("test" {
                "playlooping" { "wave" "alarm.wav" "volume" "1" }
                "playrandom" { "wave" "effect.wav" "time" "0.5" "volume" "1" }
            }
            "quiet" { }
            "random_only" {
                "playrandom" { "wave" "effect.wav" "time" "0.5" "volume" "1" }
            }
            )";
        }
        const auto random_pause_log = root / "random-pause.log";
        {
            gmod::source::bsp::AudioScene scene;
            scene.soundscapes.push_back({"inside", "random_only"});
            scene.soundscapes.push_back({"outside", "quiet"});
            scene.volumes.push_back({"inside", {-10,-10,-10}, {10,10,10}});
            scene.volumes.push_back({"outside", {100,-10,-10}, {120,10,10}});
            gmod::audio::AudioSystem audio({root}, random_pause_log, std::move(scene));
            audio.set_master_volume(0);
            const float position[3]{}, right[3]{1,0,0}, outside[3]{110,0,0};
            audio.set_listener(position, right);
            wait_for_voices(audio, 1);
            audio.world_set_paused(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            audio.set_listener(outside, right);
            std::this_thread::sleep_for(std::chrono::milliseconds(2200));
            wait_for_voices(audio, 1);
            if (audio.status().find(L"inside") == std::wstring::npos)
                throw std::runtime_error("Paused ambience changed soundscape or advanced its fades.");
            audio.set_listener(position, right);
            audio.world_set_paused(false);
            wait_for_minimum_voices(audio, 2); // Random scheduling resumes on its retained clock.
            audio.stop_sounds();
            wait_for_voices(audio, 0);
        }
        {
            gmod::source::bsp::AudioScene scene;
            gmod::source::bsp::AmbientEmitter ambient;
            ambient.sound = "alarm.wav";
            scene.ambient.push_back(ambient);
            scene.soundscapes.push_back({"inside", "test"});
            scene.soundscapes.push_back({"outside", "quiet"});
            scene.volumes.push_back({"inside", {-10, -10, -10}, {10, 10, 10}});
            scene.volumes.push_back({"outside", {100, -10, -10}, {120, 10, 10}});
            gmod::audio::AudioSystem audio({root}, stop_log, std::move(scene));
            audio.set_master_volume(0);
            const float position[3]{}, right[3]{1, 0, 0}, outside[3]{110, 0, 0};
            audio.set_listener(position, right);
            audio.voice_start(0, sound, 0, 1, position);
            audio.voice_start(1, sound, 0, 1, position, true); // Radio voice also survives.
            wait_for_minimum_voices(audio, 5);
            audio.play_sound("effect.wav", position, 1, 1000);
            audio.play_level_sound("effect.wav", position, 1000, 1);
            audio.loop_set(1, "alarm.wav", position, 1, 1000);
            audio.stop_sounds();
            wait_for_voices(audio, 2);
            audio.voice_move(0, 1, position);
            audio.voice_move(1, 1, position, true);
            audio.world_set_paused(true);
            audio.world_set_paused(false);
            audio.set_master_volume(0);
            audio.set_category_volume(gmod::audio::SoundCategory::Alarms, 1);
            // Stay in the same trigger beyond multiple random scheduling intervals.
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            wait_for_voices(audio, 2);
            audio.play_sound("effect.wav", position, 1, 1000);
            audio.loop_set(1, "alarm.wav", position, 1, 1000);
            wait_for_voices(audio, 4); // Explicit new starts still work.
            audio.stop_sounds();
            wait_for_voices(audio, 2);
            audio.set_listener(outside, right);
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            audio.set_listener(position, right);
            wait_for_minimum_voices(audio, 4); // A new trigger can start the soundscape again.
            audio.stop_sounds();
            audio.voice_stop_all();
            wait_for_voices(audio, 0);
        }
        std::filesystem::remove(soundscape_script);
        std::filesystem::remove(scripts);
        std::filesystem::remove(stop_log);
        std::filesystem::remove(pause_log);
        std::filesystem::remove(random_pause_log);
        std::filesystem::remove(sound);
        std::filesystem::remove(short_sound);
        std::filesystem::remove(expired_sound);
        std::filesystem::remove(log);
        std::filesystem::remove(root / "sound");
        std::filesystem::remove(root);
        std::cout << "PASS: pause preserves one-shot positions, positional/level effects resume without restarting, "
                     "paused queued starts, seek offsets, expiry, pitch, ambient buffers/fades/random scheduling; "
                     "stopsound preserves local/radio voices, cancels ambience and random scheduling, permits new starts\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "; diagnostics: " << log << '\n';
        return 1;
    }
}
