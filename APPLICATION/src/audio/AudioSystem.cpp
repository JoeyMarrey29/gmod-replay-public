#include "audio/AudioSystem.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <xaudio2.h>
#include <shlwapi.h>
#include "source/Vfs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace gmod::audio {
namespace {

using Microsoft::WRL::ComPtr;

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// ---- Valve KeyValues (soundscape scripts) ---------------------------------

struct Kv {
    std::string key;
    std::string value;
    std::vector<Kv> kids;
    bool block = false;
};

struct Token {
    std::string text;
    bool quoted = false;
};

std::vector<Token> tokenize(const std::string& text) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
        } else if (c == '"') {
            const auto close = text.find('"', i + 1);
            const auto end = close == std::string::npos ? text.size() : close;
            tokens.push_back({text.substr(i + 1, end - i - 1), true});
            i = end + 1;
        } else if (c == '{' || c == '}') {
            tokens.push_back({std::string(1, c), false});
            ++i;
        } else {
            std::size_t end = i;
            while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))
                   && text[end] != '{' && text[end] != '}' && text[end] != '"') ++end;
            tokens.push_back({text.substr(i, end - i), false});
            i = end;
        }
    }
    return tokens;
}

std::vector<Kv> parse_block(const std::vector<Token>& tokens, std::size_t& at) {
    std::vector<Kv> result;
    while (at < tokens.size()) {
        const auto& token = tokens[at];
        if (!token.quoted && token.text == "}") {
            ++at;
            return result;
        }
        Kv node;
        node.key = lower(token.text);
        ++at;
        if (at < tokens.size() && !tokens[at].quoted && tokens[at].text == "{") {
            ++at;
            node.block = true;
            node.kids = parse_block(tokens, at);
        } else if (at < tokens.size()) {
            node.value = tokens[at].text;
            ++at;
        }
        result.push_back(std::move(node));
    }
    return result;
}

std::pair<float, float> parse_range(const std::string& text, std::pair<float, float> fallback) {
    std::vector<float> values;
    std::stringstream stream(text);
    for (std::string part; std::getline(stream, part, ',');) {
        try {
            values.push_back(std::stof(part));
        } catch (...) {
        }
    }
    if (values.empty()) return fallback;
    return {values.front(), values.size() > 1 ? values[1] : values.front()};
}

// ---- Soundscape definitions ------------------------------------------------

struct Play {
    bool random = false;
    bool nested = false;
    std::string nested_name;
    float nested_volume = 1.0f;
    std::pair<float, float> time{10.0f, 30.0f};
    std::pair<float, float> volume{1.0f, 1.0f};
    std::pair<float, float> pitch{100.0f, 100.0f};
    std::vector<std::string> waves;
    int position = -1;  // -1 none (2D), -2 random, >=0 position entity index
    float sound_level = 75.0f;
};

Play parse_play(const Kv& node, bool random) {
    Play play;
    play.random = random;
    for (const auto& kid : node.kids) {
        if (kid.key == "volume") play.volume = parse_range(kid.value, play.volume);
        else if (kid.key == "pitch") play.pitch = parse_range(kid.value, play.pitch);
        else if (kid.key == "time") play.time = parse_range(kid.value, play.time);
        else if (kid.key == "wave") play.waves.push_back(kid.value);
        else if (kid.key == "rndwave") {
            for (const auto& wave : kid.kids) {
                if (wave.key == "wave") play.waves.push_back(wave.value);
            }
        } else if (kid.key == "position") {
            const auto value = lower(kid.value);
            play.position = value == "random" ? -2 : (value.empty() || !std::isdigit(static_cast<unsigned char>(value[0])) ? -1 : std::atoi(value.c_str()));
        } else if (kid.key == "attenuation") {
            // A soundscape can give its falloff as an attenuation instead of a level:
            // level = 50 + 20 / attenuation (ATTN_TO_SNDLVL).
            const float attenuation = static_cast<float>(std::atof(kid.value.c_str()));
            if (attenuation > 0.0f) play.sound_level = 50.0f + 20.0f / attenuation;
        } else if (kid.key == "soundlevel") {
            const auto digits = kid.value.find_first_of("0123456789");
            if (digits != std::string::npos) play.sound_level = std::stof(kid.value.substr(digits));
        }
    }
    return play;
}

using Scape = std::vector<Play>;

// ---- Sample decoding --------------------------------------------------------

struct Sample {
    std::vector<std::int16_t> pcm;  // mono, 44.1 kHz
};

ComPtr<IMFSourceReader> open_reader(const std::filesystem::path& path) {
    ComPtr<IMFSourceReader> reader;
    if (!gmod::source::vfs::is_archived(path)) {
        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) return nullptr;
        return reader;
    }
    // Inside a game archive: decode from memory, telling MF what container it is.
    std::vector<std::uint8_t> bytes;
    try {
        bytes = gmod::source::vfs::read(path);
    } catch (const std::exception&) {
        return nullptr;
    }
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
    if (!stream) return nullptr;
    ComPtr<IMFByteStream> byte_stream;
    if (FAILED(MFCreateMFByteStreamOnStream(stream.Get(), &byte_stream))) return nullptr;
    auto extension = path.extension().wstring();
    for (auto& c : extension) c = static_cast<wchar_t>(towlower(c));
    ComPtr<IMFAttributes> stream_attributes;
    if (SUCCEEDED(byte_stream.As(&stream_attributes))) {
        stream_attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE,
                                     extension == L".mp3" ? L"audio/mpeg" : extension == L".ogg" ? L"audio/ogg" : L"audio/wav");
    }
    if (FAILED(MFCreateSourceReaderFromByteStream(byte_stream.Get(), nullptr, &reader))) return nullptr;
    return reader;
}

bool decode_file(const std::filesystem::path& path, std::vector<std::int16_t>& output) {
    ComPtr<IMFSourceReader> reader = open_reader(path);
    if (!reader) {
        return false;
    }
    UINT32 channels = 0;
    for (const UINT32 attempt : {1u, 2u}) {
        ComPtr<IMFMediaType> type;
        if (FAILED(MFCreateMediaType(&type))) return false;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, attempt);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 44100);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2 * attempt);
        type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 44100 * 2 * attempt);
        if (SUCCEEDED(reader->SetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, type.Get()))) {
            channels = attempt;
            break;
        }
    }
    if (channels == 0) return false;
    constexpr std::size_t MaxSamples = 44100u * static_cast<std::size_t>(AudioSystem::MaxOneShotSourceSeconds);
    std::vector<std::int16_t> raw;
    while (raw.size() < MaxSamples * channels) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
                                      nullptr, &flags, nullptr, &sample))) {
            break;
        }
        if ((flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) != 0) break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) break;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) break;
        const auto* samples = reinterpret_cast<const std::int16_t*>(data);
        raw.insert(raw.end(), samples, samples + length / 2);
        buffer->Unlock();
    }
    if (raw.empty()) return false;
    if (channels == 1) {
        output = std::move(raw);
    } else {
        output.resize(raw.size() / 2);
        for (std::size_t i = 0; i < output.size(); ++i) {
            output[i] = static_cast<std::int16_t>((raw[i * 2] + raw[i * 2 + 1]) / 2);
        }
    }
    return true;
}

float saturate(float value) { return std::clamp(value, 0.0f, 1.0f); }

} // namespace

struct AudioSystem::Impl {
    using Vec3 = source::bsp::Vec3;

    enum class Mode { Flat, Point, FixedPan, Level };

    struct Voice {
        IXAudio2SourceVoice* voice = nullptr;
        std::shared_ptr<Sample> sample;
        SoundCategory category = SoundCategory::Ambient;
        float base = 1.0f;
        Mode mode = Mode::Flat;
        Vec3 origin{};
        float radius = 1000.0f;
        float pan = 0.0f;
        std::uint64_t owner = 0;  // soundscape instance id, 0 = none
        bool loop = false;
        int clip = -1;            // demo voice clip id, -1 for ambient sounds
        std::string key;          // looping world sounds: the file being looped
    };

    struct VoiceCommand {
        enum class Type { Start, Move, Stop, StopAll, OneShot, LoopSet, LoopStop, LoopStopAll, WorldStopAll, WorldPause, StopSounds } type = Type::Stop;
        int clip = -1;
        std::filesystem::path file;
        float offset = 0.0f;  // seconds into the clip
        float rate = 1.0f;
        Vec3 position{};
        std::string key;      // one-shot: sound-relative path ("player/footsteps/concrete1.wav")
        float gain = 1.0f;
        float radius = 1000.0f;
        float level = 0.0f;   // one-shot: > 0 for the Perpheads gun-sound falloff (Mode::Level)
        bool flat = false;    // voice clips: heard everywhere at full level (radio)
        bool paused = false;
    };

    struct PlayState {
        Play play;
        float mult = 1.0f;
        double next = 0.0;
        bool started = false;
    };

    struct Instance {
        std::uint64_t id = 0;
        std::string name;
        const source::bsp::SoundscapeEntity* entity = nullptr;
        float fade = 0.0f;
        bool wanted = true;
        bool stopped = false;
        std::vector<PlayState> plays;
    };

    std::vector<std::filesystem::path> roots;
    std::ofstream log;
    source::bsp::AudioScene scene;

    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> muted{false};
    std::atomic<float> volume{1.0f};
    std::atomic<float> category_volumes[static_cast<int>(SoundCategory::Count)]{
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    int loop_decodes_this_tick = 0;
    mutable std::mutex mutex;
    std::vector<VoiceCommand> commands;  // guarded by `mutex`
    bool has_listener = false;
    float listener_position[3]{};
    float listener_right[3]{1, 0, 0};
    std::wstring status_text = L"snd starting";

    // Audio-thread state.
    ComPtr<IXAudio2> xaudio;
    IXAudio2MasteringVoice* master = nullptr;
    UINT32 master_channels = 2;
    std::unordered_map<std::string, std::filesystem::path> index;
    std::unordered_map<std::string, Scape> scapes;
    std::unordered_map<std::string, const source::bsp::SoundscapeEntity*> entities;
    std::unordered_map<std::string, std::shared_ptr<Sample>> cache;
    std::unordered_map<std::string, double> durations;
    std::map<std::string, int> missing;
    std::vector<Voice> voices;
    std::vector<Instance> instances;
    std::uint64_t next_id = 1;
    bool world_paused = false;
    double ambience_time = 0.0;
    std::string current_target;
    std::mt19937 rng{std::random_device{}()};
    static constexpr std::size_t MaxVoices = 128;
    static constexpr float MasterGain = 0.7f;
    static constexpr float FadeSeconds = 1.5f;

    float uniform(std::pair<float, float> range) {
        if (range.second <= range.first) return range.first;
        return std::uniform_real_distribution<float>(range.first, range.second)(rng);
    }

    static std::string clean_wave(std::string wave) {
        std::size_t skip = 0;
        while (skip < wave.size() && std::string("*#@<>^)(}$!?").find(wave[skip]) != std::string::npos) ++skip;
        wave.erase(0, skip);
        std::replace(wave.begin(), wave.end(), '\\', '/');
        return lower(wave);
    }

    std::shared_ptr<Sample> load_sample(const std::string& wave, float offset = 0.0f) {
        const auto key = clean_wave(wave);
        const auto cached = cache.find(key);
        if (cached != cache.end()) return cached->second;
        std::shared_ptr<Sample> result;
        const auto found = index.find(key);
        if (found == index.end()) {
            ++missing[key];
            log << "MISSING " << key << "\n";
        } else {
            if (offset > 0.0f) {
                auto duration = durations.find(key);
                if (duration == durations.end()) {
                    auto reader = open_reader(found->second);
                    PROPVARIANT value;
                    PropVariantInit(&value);
                    const HRESULT result = reader
                        ? reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &value)
                        : E_FAIL;
                    if (SUCCEEDED(result) && value.vt == VT_UI8) {
                        duration = durations.emplace(key, static_cast<double>(value.uhVal.QuadPart) / 10000000.0).first;
                    } else {
                        log << "DURATION_UNAVAILABLE " << key << " hr=" << result << "; decoding sample\n";
                    }
                    PropVariantClear(&value);
                }
                if (duration != durations.end() && offset >= duration->second) return nullptr;
            }
            auto sample = std::make_shared<Sample>();
            if (decode_file(found->second, sample->pcm)) {
                int peak = 0;
                for (const auto value : sample->pcm) peak = std::max(peak, std::abs(static_cast<int>(value)));
                log << "LOADED " << key << " samples=" << sample->pcm.size() << " peak=" << peak << "\n";
                result = std::move(sample);
            } else {
                log << "DECODE_FAILED " << found->second.string() << "\n";
            }
        }
        cache[key] = result;
        log.flush();
        return result;
    }

    bool start_voice(const std::shared_ptr<Sample>& sample, float base, float pitch, bool loop,
                     Mode mode, Vec3 origin, float radius, float pan, std::uint64_t owner,
                     float offset = 0.0f, bool paused = false) {
        if (!sample || voices.size() >= MaxVoices || !xaudio) return false;
        const auto begin = static_cast<std::size_t>(std::max(offset, 0.0f) * 44100.0f);
        if (begin >= sample->pcm.size()) return false;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = 44100;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 2;
        format.nAvgBytesPerSec = 88200;
        IXAudio2SourceVoice* voice = nullptr;
        if (FAILED(xaudio->CreateSourceVoice(&voice, &format, 0, 4.0f))) return false;
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(sample->pcm.size() * sizeof(std::int16_t));
        buffer.pAudioData = reinterpret_cast<const BYTE*>(sample->pcm.data());
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.LoopCount = loop ? XAUDIO2_LOOP_INFINITE : 0;
        buffer.PlayBegin = static_cast<UINT32>(begin);
        buffer.PlayLength = static_cast<UINT32>(sample->pcm.size() - begin);
        if (FAILED(voice->SubmitSourceBuffer(&buffer))) {
            voice->DestroyVoice();
            return false;
        }
        voice->SetFrequencyRatio(std::clamp(pitch / 100.0f, 0.25f, 4.0f));
        voice->SetVolume(0.0f);
        if (!paused) voice->Start();
        Voice entry;
        entry.voice = voice;
        entry.sample = sample;
        entry.base = base;
        entry.mode = mode;
        entry.origin = origin;
        entry.radius = std::max(radius, 1.0f);
        entry.pan = pan;
        entry.owner = owner;
        entry.loop = loop;
        voices.push_back(std::move(entry));
        return true;
    }

    // Demo voice chat: positional, started part-way through when playback joins a clip.
    static constexpr float VoiceRadius = 2400.0f;
    static constexpr float VoiceGain = 1.6f;
    static constexpr float RadioGain = 1.1f;

    // Frees a voice for something more important: a looping world sound first and, when
    // drop_one_shots, otherwise the oldest one-shot.
    void make_room(bool drop_one_shots) {
        for (int pass = 0; pass < (drop_one_shots ? 2 : 1); ++pass) {
            for (std::size_t i = 0; i < voices.size(); ++i) {
                const bool match = pass == 0 ? voices[i].clip >= LoopBase : voices[i].clip == OneShotClip;
                if (!match) continue;
                voices[i].voice->DestroyVoice();
                voices.erase(voices.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        }
    }

    void start_clip(const VoiceCommand& command) {
        for (const auto& voice : voices) {
            if (voice.clip == command.clip) return;
        }
        const auto key = command.file.string();
        std::shared_ptr<Sample> sample;
        const auto cached = cache.find(key);
        if (cached != cache.end()) {
            sample = cached->second;
        } else {
            auto decoded = std::make_shared<Sample>();
            if (decode_file(command.file, decoded->pcm)) {
                sample = std::move(decoded);
            } else {
                log << "VOICE_DECODE_FAILED " << key << "\n";
            }
            cache[key] = sample;
        }
        if (!sample || !xaudio) return;
        // Voice chat is never crowded out by sound effects.
        if (voices.size() >= MaxVoices) make_room(true);
        if (voices.size() >= MaxVoices) return;
        const auto begin = static_cast<std::size_t>(std::max(command.offset, 0.0f) * 44100.0f);
        if (begin + 441 >= sample->pcm.size()) return;
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = 44100;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 2;
        format.nAvgBytesPerSec = 88200;
        IXAudio2SourceVoice* source_voice = nullptr;
        if (FAILED(xaudio->CreateSourceVoice(&source_voice, &format, 0, 4.0f))) return;
        XAUDIO2_BUFFER buffer{};
        buffer.AudioBytes = static_cast<UINT32>(sample->pcm.size() * sizeof(std::int16_t));
        buffer.pAudioData = reinterpret_cast<const BYTE*>(sample->pcm.data());
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        buffer.PlayBegin = static_cast<UINT32>(begin);
        buffer.PlayLength = static_cast<UINT32>(sample->pcm.size() - begin);
        if (FAILED(source_voice->SubmitSourceBuffer(&buffer))) {
            source_voice->DestroyVoice();
            return;
        }
        source_voice->SetFrequencyRatio(std::clamp(command.rate, 0.25f, 4.0f));
        source_voice->SetVolume(0.0f);
        source_voice->Start();
        Voice entry;
        entry.voice = source_voice;
        entry.sample = sample;
        entry.base = command.flat ? RadioGain : VoiceGain;
        entry.mode = command.flat ? Mode::Flat : Mode::Point;
        entry.origin = command.position;
        entry.radius = VoiceRadius;
        entry.clip = command.clip;
        entry.category = SoundCategory::Voices;
        voices.push_back(std::move(entry));
    }

    // Demo one-shots (footsteps, gunfire): positional sounds from the game's sound folders.
    static constexpr int OneShotClip = -2;
    // Looping world sounds (engines, sirens) are identified by the caller; ids are offset so
    // they never collide with voice clip ids.
    static constexpr int LoopBase = 1'000'000;

    Voice* find_loop(int id) {
        for (auto& voice : voices) {
            if (voice.clip == LoopBase + id) return &voice;
        }
        return nullptr;
    }

    void set_loop(const VoiceCommand& command) {
        Voice* existing = find_loop(command.clip);
        if (existing && existing->key != command.key) {
            existing->voice->DestroyVoice();
            voices.erase(voices.begin() + (existing - voices.data()));
            existing = nullptr;
        }
        if (existing) {
            existing->origin = command.position;
            existing->base = command.gain;
            existing->radius = std::max(command.radius, 1.0f);
            existing->voice->SetFrequencyRatio(std::clamp(command.rate, 0.25f, 4.0f));
            return;
        }
        // Decoding a new file takes a while and happens on this thread; one per tick keeps
        // voices and gunfire from stalling behind a burst of new engine sounds.
        if (cache.find(clean_wave(command.key)) == cache.end()) {
            if (loop_decodes_this_tick >= 1) return;
            ++loop_decodes_this_tick;
        }
        const auto sample = load_sample(command.key);
        if (!sample) return;
        if (!start_voice(sample, command.gain, std::clamp(command.rate, 0.25f, 4.0f) * 100.0f, true,
                         Mode::Point, command.position, command.radius, 0.0f, 0, 0.0f, world_paused)) {
            return;
        }
        voices.back().clip = LoopBase + command.clip;
        voices.back().key = command.key;
        voices.back().category = sound_category(command.key);
    }

    void stop_loop(int id) {
        for (std::size_t i = 0; i < voices.size(); ++i) {
            if (voices[i].clip != LoopBase + id) continue;
            voices[i].voice->DestroyVoice();
            voices.erase(voices.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }

    void start_one_shot(const VoiceCommand& command) {
        if (command.offset >= MaxOneShotSourceSeconds) return;
        const auto sample = load_sample(command.key, command.offset);
        if (!sample) return;
        if (command.offset * 44100.0f >= static_cast<float>(sample->pcm.size())) return;
        // One-shots (gunfire, footsteps) matter more than a distant engine: when every voice
        // is taken, a looping world sound gives its place up.
        if (voices.size() >= MaxVoices) make_room(false);
        const bool level = command.level > 0.0f;
        if (!start_voice(sample, command.gain, std::clamp(command.rate, 0.25f, 4.0f) * 100.0f,
                         false, level ? Mode::Level : Mode::Point, command.position,
                         level ? command.level : command.radius, 0.0f, 0, command.offset, world_paused)) {
            return;
        }
        voices.back().clip = OneShotClip;
        voices.back().category = sound_category(command.key, false, level);
        if (command.offset > 0.0f) {
            log << "ONE_SHOT_RESTORED " << command.key << " offset=" << command.offset << "\n";
            log.flush();
        }
    }

    void set_world_voice_paused(Voice& voice, bool paused) {
        const HRESULT result = paused ? voice.voice->Stop() : voice.voice->Start();
        if (FAILED(result)) {
            log << "WORLD_PAUSE_FAILED paused=" << paused << " hr=" << result << "\n";
            log.flush();
        }
    }

    void run_commands(std::vector<VoiceCommand>& pending) {
        for (const auto& command : pending) {
            switch (command.type) {
            case VoiceCommand::Type::StopSounds:
                for (std::size_t i = 0; i < voices.size();) {
                    if (voices[i].clip >= 0 && voices[i].clip < LoopBase) {
                        ++i;
                        continue;
                    }
                    voices[i].voice->DestroyVoice();
                    voices.erase(voices.begin() + static_cast<std::ptrdiff_t>(i));
                }
                for (auto& instance : instances) instance.stopped = true;
                log << "STOP_SOUND effects_and_ambience_stopped voices_preserved\n";
                log.flush();
                break;
            case VoiceCommand::Type::WorldPause:
                if (world_paused != command.paused) {
                    world_paused = command.paused;
                    for (auto& voice : voices) {
                        if (voice.clip < 0 || voice.clip >= LoopBase)
                            set_world_voice_paused(voice, world_paused);
                    }
                }
                break;
            case VoiceCommand::Type::Start:
                start_clip(command);
                break;
            case VoiceCommand::Type::OneShot:
                start_one_shot(command);
                break;
            case VoiceCommand::Type::LoopSet:
                set_loop(command);
                break;
            case VoiceCommand::Type::LoopStop:
                stop_loop(command.clip);
                break;
            case VoiceCommand::Type::LoopStopAll:
            case VoiceCommand::Type::WorldStopAll: {
                std::size_t one_shots = 0, loops = 0;
                for (std::size_t i = 0; i < voices.size();) {
                    const bool one_shot = voices[i].clip == OneShotClip;
                    const bool loop = voices[i].clip >= LoopBase;
                    if (!loop && !(one_shot && command.type == VoiceCommand::Type::WorldStopAll)) {
                        ++i;
                        continue;
                    }
                    if (one_shot) ++one_shots;
                    if (loop) ++loops;
                    voices[i].voice->DestroyVoice();
                    voices.erase(voices.begin() + static_cast<std::ptrdiff_t>(i));
                }
                if (command.type == VoiceCommand::Type::WorldStopAll) {
                    log << "WORLD_SOUNDS_STOPPED one_shots=" << one_shots << " loops=" << loops << "\n";
                    log.flush();
                }
                break;
            }
            case VoiceCommand::Type::Move:
                for (auto& voice : voices) {
                    if (voice.clip != command.clip) continue;
                    voice.origin = command.position;
                    voice.mode = command.flat ? Mode::Flat : Mode::Point;
                    voice.base = command.flat ? RadioGain : VoiceGain;
                    voice.voice->SetFrequencyRatio(std::clamp(command.rate, 0.25f, 4.0f));
                }
                break;
            case VoiceCommand::Type::Stop:
                for (std::size_t i = 0; i < voices.size(); ++i) {
                    if (voices[i].clip != command.clip) continue;
                    voices[i].voice->DestroyVoice();
                    voices.erase(voices.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
                break;
            case VoiceCommand::Type::StopAll:
                for (std::size_t i = 0; i < voices.size();) {
                    if (voices[i].clip < 0 || voices[i].clip >= LoopBase) {
                        ++i;
                        continue;
                    }
                    voices[i].voice->DestroyVoice();
                    voices.erase(voices.begin() + static_cast<std::ptrdiff_t>(i));
                }
                break;
            }
        }
        pending.clear();
    }

    void apply_levels(Voice& entry, float gain, float pan) {
        entry.voice->SetVolume(gain);
        const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.7853981634f;
        float matrix[8]{};
        if (master_channels == 1) {
            matrix[0] = 1.0f;
        } else {
            matrix[0] = std::cos(angle);
            matrix[1] = std::sin(angle);
        }
        entry.voice->SetOutputMatrix(nullptr, 1, std::min<UINT32>(master_channels, 8), matrix);
    }

    void build_index() {
        for (const auto& root : roots) {
            const auto base = root / "sound";
            for (const auto& relative : gmod::source::vfs::list_files(base, true)) {
                index.emplace(lower(relative), base / std::filesystem::path(relative));
            }
        }
        log << "SOUND_FILES " << index.size() << "\n";
    }

    void load_scripts() {
        std::filesystem::path script;
        for (const auto& root : roots) {
            const auto candidate = root / "scripts" / "soundscapes_rp_paralake_city_v6.txt";
            if (gmod::source::vfs::exists(candidate)) {
                script = candidate;
                break;
            }
        }
        if (script.empty()) {
            log << "NO_SOUNDSCAPE_SCRIPT\n";
            return;
        }
        const auto script_bytes = gmod::source::vfs::read(script);
        const auto tokens = tokenize(std::string(script_bytes.begin(), script_bytes.end()));
        std::size_t at = 0;
        for (const auto& node : parse_block(tokens, at)) {
            if (!node.block) continue;
            Scape scape;
            for (const auto& kid : node.kids) {
                if (kid.key == "playlooping") scape.push_back(parse_play(kid, false));
                else if (kid.key == "playrandom") scape.push_back(parse_play(kid, true));
                else if (kid.key == "playsoundscape") {
                    Play nested;
                    nested.nested = true;
                    for (const auto& part : kid.kids) {
                        if (part.key == "name") nested.nested_name = lower(part.value);
                        else if (part.key == "volume") nested.nested_volume = std::stof(part.value);
                    }
                    scape.push_back(std::move(nested));
                }
            }
            scapes[node.key] = std::move(scape);
        }
        log << "SOUNDSCAPES_DEFINED " << scapes.size() << " from " << script.string() << "\n";
    }

    void flatten(Instance& instance, const std::string& name, float mult, int depth) {
        const auto found = scapes.find(lower(name));
        if (found == scapes.end() || depth > 4) {
            if (found == scapes.end()) log << "SOUNDSCAPE_UNDEFINED " << name << "\n";
            return;
        }
        for (const auto& play : found->second) {
            if (play.nested) {
                flatten(instance, play.nested_name, mult * play.nested_volume, depth + 1);
            } else if (!play.waves.empty()) {
                instance.plays.push_back({play, mult, 0.0, false});
            }
        }
    }

    static float level_radius(float sound_level) {
        // Source attenuates as 1 / (distance x dist_mult); this is where that falls to a tenth.
        return std::clamp(360.0f * std::pow(10.0f, sound_level / 20.0f - 3.0f), 300.0f, 6000.0f);
    }

    void start_play(Instance& instance, PlayState& state, const std::string& wave, bool loop) {
        const auto sample = load_sample(wave);
        if (!sample) return;
        const auto& play = state.play;
        Mode mode = Mode::Flat;
        Vec3 origin{};
        float radius = 1000.0f;
        float pan = 0.0f;
        if (play.position == -2) {
            mode = Mode::FixedPan;
            pan = std::uniform_real_distribution<float>(-1.0f, 1.0f)(rng);
        } else if (play.position >= 0 && instance.entity) {
            for (const auto& [position_index, position] : instance.entity->positions) {
                if (position_index == play.position) {
                    mode = Mode::Point;
                    origin = position;
                    radius = level_radius(play.sound_level);
                }
            }
        }
        if (start_voice(sample, uniform(play.volume) * state.mult, uniform(play.pitch), loop,
                        mode, origin, radius, pan, instance.id, 0.0f, world_paused)) {
            voices.back().category = sound_category(wave, true);
        }
    }

    void select_soundscape(const float position[3]) {
        const source::bsp::SoundscapeVolume* best = nullptr;
        float best_size = 0.0f;
        for (const auto& volume : scene.volumes) {
            if (position[0] < volume.mins.x || position[0] > volume.maxs.x
                || position[1] < volume.mins.y || position[1] > volume.maxs.y
                || position[2] < volume.mins.z || position[2] > volume.maxs.z
                || !entities.contains(volume.target)) {
                continue;
            }
            const float size = (volume.maxs.x - volume.mins.x) * (volume.maxs.y - volume.mins.y)
                * (volume.maxs.z - volume.mins.z);
            if (!best || size < best_size) {
                best = &volume;
                best_size = size;
            }
        }
        // Like the engine, the previous soundscape stays when no trigger contains the listener.
        if (!best || best->target == current_target) return;
        current_target = best->target;
        const auto* entity = entities[current_target];
        for (auto& instance : instances) {
            if (instance.wanted) instance.wanted = false;
        }
        for (auto& instance : instances) {
            if (instance.name == entity->soundscape && !instance.wanted) {
                instance.wanted = true;
                instance.entity = entity;
                if (instance.stopped) {
                    instance.stopped = false;
                    for (auto& state : instance.plays) {
                        state.started = false;
                        state.next = 0.0;
                    }
                }
                return;
            }
        }
        Instance instance;
        instance.id = next_id++;
        instance.name = entity->soundscape;
        instance.entity = entity;
        flatten(instance, entity->soundscape, 1.0f, 0);
        log << "SOUNDSCAPE " << instance.name << " plays=" << instance.plays.size() << "\n";
        log.flush();
        instances.push_back(std::move(instance));
    }

    void start_ambient() {
        std::size_t started = 0, silent = 0, unavailable = 0;
        for (const auto& emitter : scene.ambient) {
            if ((emitter.flags & 16) != 0) {
                ++silent;
                continue;
            }
            const auto key = clean_wave(emitter.sound);
            const bool file_name = key.ends_with(".wav") || key.ends_with(".mp3");
            if (!file_name) {
                ++unavailable;
                log << "AMBIENT_SOUND_SCRIPT_UNAVAILABLE " << emitter.sound << "\n";
                continue;
            }
            const auto sample = load_sample(key);
            if (!sample) {
                ++unavailable;
                continue;
            }
            float radius = emitter.radius;
            if (radius <= 0.0f) {
                radius = (emitter.flags & 8) ? 1250.0f : (emitter.flags & 4) ? 750.0f
                    : (emitter.flags & 2) ? 250.0f : 1250.0f;
            }
            const bool everywhere = (emitter.flags & 1) != 0;
            if (start_voice(sample, emitter.volume, emitter.pitch, (emitter.flags & 32) == 0,
                            everywhere ? Mode::Flat : Mode::Point, emitter.origin, radius, 0.0f, 0,
                            0.0f, world_paused)) {
                voices.back().category = sound_category(key, true);
                ++started;
            }
        }
        log << "AMBIENT emitters=" << scene.ambient.size() << " playing=" << started
            << " start_silent=" << silent << " unavailable=" << unavailable << "\n";
        log.flush();
    }

    void tick(double delta) {
        float position[3];
        float right[3];
        std::vector<VoiceCommand> pending;
        bool listening = false;
        {
            std::lock_guard lock(mutex);
            pending.swap(commands);
            listening = has_listener;
            if (listening) {
                std::copy(listener_position, listener_position + 3, position);
                std::copy(listener_right, listener_right + 3, right);
            }
        }
        loop_decodes_this_tick = 0;
        const bool was_paused = world_paused;
        run_commands(pending);
        if (!listening) return;
        const double ambience_delta = world_paused || was_paused ? 0.0 : delta;
        ambience_time += ambience_delta;
        if (!world_paused) select_soundscape(position);

        for (auto& instance : instances) {
            if (world_paused) continue;
            const float step = static_cast<float>(ambience_delta) / FadeSeconds;
            instance.fade = std::clamp(instance.fade + (instance.wanted ? step : -step), 0.0f, 1.0f);
            if (instance.fade <= 0.0f && instance.wanted) instance.fade = 0.001f;
            if (!instance.wanted && instance.fade <= 0.0f) {
                // Fully faded out: restart cleanly if the listener comes back.
                for (auto& state : instance.plays) {
                    state.started = false;
                    state.next = 0.0;
                }
                continue;
            }
            if (instance.stopped) continue;
            for (auto& state : instance.plays) {
                if (!state.play.random) {
                    if (!state.started && instance.wanted) {
                        state.started = true;
                        start_play(instance, state, state.play.waves[rng() % state.play.waves.size()], true);
                    }
                } else if (instance.wanted) {
                    if (state.next == 0.0) {
                        state.next = ambience_time + uniform({0.0f, state.play.time.second});
                    } else if (ambience_time >= state.next) {
                        state.next = ambience_time + std::max(0.5f, uniform(state.play.time));
                        start_play(instance, state, state.play.waves[rng() % state.play.waves.size()], false);
                    }
                }
            }
        }

        const bool mute = muted.load();
        const float user_volume = volume.load();
        for (std::size_t i = 0; i < voices.size();) {
            auto& voice = voices[i];
            float fade = 1.0f;
            bool drop = false;
            if (voice.owner != 0) {
                const auto owner = std::find_if(instances.begin(), instances.end(),
                    [&](const Instance& instance) { return instance.id == voice.owner; });
                if (owner == instances.end() || (!owner->wanted && owner->fade <= 0.0f)) drop = true;
                else fade = owner->fade;
            }
            if (!drop && !voice.loop) {
                XAUDIO2_VOICE_STATE state{};
                voice.voice->GetState(&state);
                drop = state.BuffersQueued == 0;
            }
            if (drop) {
                voice.voice->DestroyVoice();
                voices.erase(voices.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            float attenuation = 1.0f;
            float pan = voice.pan;
            if (voice.mode == Mode::Point || voice.mode == Mode::Level) {
                const float dx = voice.origin.x - position[0];
                const float dy = voice.origin.y - position[1];
                const float dz = voice.origin.z - position[2];
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (voice.mode == Mode::Level) {
                    // `radius` holds the sound level here; the game clamps level gain x volume to 1.
                    attenuation = std::pow(voice.radius / std::max(distance, 1.0f), 0.75f);
                    attenuation = std::min(attenuation, 1.0f / std::max(voice.base, 1e-3f));
                } else {
                    attenuation = saturate(1.0f - distance / voice.radius);
                    attenuation *= attenuation;
                }
                const float along = (dx * right[0] + dy * right[1] + dz * right[2]) / std::max(distance, 1.0f);
                pan = along * std::min(distance / 64.0f, 1.0f);
            } else if (voice.mode == Mode::FixedPan) {
                attenuation = 0.7f;
            }
            apply_levels(voice, mute ? 0.0f : voice.base * fade * attenuation * MasterGain * user_volume
                * category_volumes[static_cast<int>(voice.category)].load(), pan);
            ++i;
        }

        std::wstringstream text;
        text << L"snd " << (mute ? L"MUTED " : L"") << std::wstring(current_target.begin(), current_target.end())
             << L" voices " << voices.size();
        std::lock_guard lock(mutex);
        status_text = text.str();
    }

    void run() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool media = SUCCEEDED(MFStartup(MF_VERSION));
        try {
            if (!media || FAILED(XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR))
                || FAILED(xaudio->CreateMasteringVoice(&master))) {
                log << "AUDIO_DEVICE_UNAVAILABLE\n";
                std::lock_guard lock(mutex);
                status_text = L"snd unavailable";
            } else {
                XAUDIO2_VOICE_DETAILS details{};
                master->GetVoiceDetails(&details);
                master_channels = std::max<UINT32>(details.InputChannels, 1);
                log << "AUDIO_DEVICE ok channels=" << master_channels << "\n";
                build_index();
                load_scripts();
                for (const auto& entity : scene.soundscapes) entities.emplace(entity.targetname, &entity);
                log << "SCENE ambient=" << scene.ambient.size() << " soundscape_entities="
                    << scene.soundscapes.size() << " trigger_volumes=" << scene.volumes.size() << "\n";
                start_ambient();
                auto last = std::chrono::steady_clock::now();
                while (!stop) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    const auto now = std::chrono::steady_clock::now();
                    tick(std::min(std::chrono::duration<double>(now - last).count(), 0.25));
                    last = now;
                }
            }
        } catch (const std::exception& error) {
            log << "AUDIO_ERROR " << error.what() << "\n";
            std::lock_guard lock(mutex);
            status_text = L"snd error";
        }
        for (auto& voice : voices) voice.voice->DestroyVoice();
        voices.clear();
        if (master) master->DestroyVoice();
        xaudio.Reset();
        log << "UNIQUE_MISSING_SOUNDS " << missing.size() << "\n";
        for (const auto& [name, count] : missing) log << "  " << name << " x" << count << "\n";
        log.flush();
        if (media) MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
    }
};

AudioSystem::AudioSystem(
    std::vector<std::filesystem::path> content_roots,
    std::filesystem::path log_path,
    source::bsp::AudioScene scene)
    : impl_(std::make_unique<Impl>()) {
    impl_->roots = std::move(content_roots);
    impl_->log.open(log_path);
    impl_->scene = std::move(scene);
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
}

AudioSystem::~AudioSystem() {
    impl_->stop = true;
    if (impl_->thread.joinable()) impl_->thread.join();
}

void AudioSystem::set_listener(const float position[3], const float right[3]) {
    std::lock_guard lock(impl_->mutex);
    std::copy(position, position + 3, impl_->listener_position);
    std::copy(right, right + 3, impl_->listener_right);
    impl_->has_listener = true;
}

void AudioSystem::toggle_mute() { impl_->muted = !impl_->muted; }

void AudioSystem::set_master_volume(float volume) {
    impl_->volume = std::clamp(volume, 0.0f, 1.0f);
}

void AudioSystem::set_category_volume(SoundCategory category, float volume) {
    const auto index = static_cast<int>(category);
    if (index < 0 || index >= static_cast<int>(SoundCategory::Count)) return;
    impl_->category_volumes[index] = std::clamp(volume, 0.0f, 1.0f);
}

void AudioSystem::voice_start(int clip, const std::filesystem::path& wav, float offset_seconds,
                              float rate, const float position[3], bool flat) {
    Impl::VoiceCommand command;
    command.type = Impl::VoiceCommand::Type::Start;
    command.clip = clip;
    command.file = wav;
    command.offset = offset_seconds;
    command.rate = rate;
    command.flat = flat;
    command.position = {position[0], position[1], position[2]};
    std::lock_guard lock(impl_->mutex);
    impl_->commands.push_back(std::move(command));
}

void AudioSystem::voice_move(int clip, float rate, const float position[3], bool flat) {
    Impl::VoiceCommand command;
    command.type = Impl::VoiceCommand::Type::Move;
    command.clip = clip;
    command.rate = rate;
    command.flat = flat;
    command.position = {position[0], position[1], position[2]};
    std::lock_guard lock(impl_->mutex);
    impl_->commands.push_back(std::move(command));
}

void AudioSystem::voice_stop(int clip) {
    Impl::VoiceCommand command;
    command.type = Impl::VoiceCommand::Type::Stop;
    command.clip = clip;
    std::lock_guard lock(impl_->mutex);
    impl_->commands.push_back(std::move(command));
}

void AudioSystem::voice_stop_all() {
    Impl::VoiceCommand command;
    command.type = Impl::VoiceCommand::Type::StopAll;
    std::lock_guard lock(impl_->mutex);
    impl_->commands.push_back(std::move(command));
}

void AudioSystem::play_sound(const std::string& sound, const float position[3], float gain,
                             float radius, float rate, float offset_seconds) {
    Impl::VoiceCommand command;
    command.type = Impl::VoiceCommand::Type::OneShot;
    command.key = sound;
    command.gain = gain;
    command.radius = radius;
    command.rate = rate;
    command.offset = offset_seconds;
    command.position = {position[0], position[1], position[2]};
    std::lock_guard lock(impl_->mutex);
    // A burst of shots must not pile up while the audio thread is busy.
    if (offset_seconds > 0.0f || impl_->commands.size() < 256) impl_->commands.push_back(std::move(command));
}

void AudioSystem::play_level_sound(const std::string& sound, const float position[3], float level,
                                   float volume, float rate, float offset_seconds) {
    Impl::VoiceCommand command;
    command.type = Impl::VoiceCommand::Type::OneShot;
    command.key = sound;
    command.gain = volume;
    command.level = std::max(level, 1.0f);
    command.rate = rate;
    command.offset = offset_seconds;
    command.position = {position[0], position[1], position[2]};
    std::lock_guard lock(impl_->mutex);
    if (offset_seconds > 0.0f || impl_->commands.size() < 256) impl_->commands.push_back(std::move(command));
}

void AudioSystem::loop_set(int id, const std::string& sound, const float position[3], float gain,
                          float radius, float rate) {
    Impl::VoiceCommand command;
    command.type = Impl::VoiceCommand::Type::LoopSet;
    command.clip = id;
    command.key = sound;
    command.gain = gain;
    command.radius = radius;
    command.rate = rate;
    command.position = {position[0], position[1], position[2]};
    std::lock_guard lock(impl_->mutex);
    if (impl_->commands.size() < 512) impl_->commands.push_back(std::move(command));
}

void AudioSystem::loop_stop_all() {
    Impl::VoiceCommand command;
    command.type = Impl::VoiceCommand::Type::LoopStopAll;
    std::lock_guard lock(impl_->mutex);
    impl_->commands.push_back(std::move(command));
}

void AudioSystem::world_stop_all() {
    Impl::VoiceCommand command;
    command.type = Impl::VoiceCommand::Type::WorldStopAll;
    std::lock_guard lock(impl_->mutex);
    impl_->commands.push_back(std::move(command));
}

void AudioSystem::stop_sounds() {
    Impl::VoiceCommand command;
    command.type = Impl::VoiceCommand::Type::StopSounds;
    std::lock_guard lock(impl_->mutex);
    impl_->commands.push_back(std::move(command));
}

void AudioSystem::world_set_paused(bool paused) {
    Impl::VoiceCommand command;
    command.type = Impl::VoiceCommand::Type::WorldPause;
    command.paused = paused;
    std::lock_guard lock(impl_->mutex);
    impl_->commands.push_back(std::move(command));
}

void AudioSystem::loop_stop(int id) {
    Impl::VoiceCommand command;
    command.type = Impl::VoiceCommand::Type::LoopStop;
    command.clip = id;
    std::lock_guard lock(impl_->mutex);
    impl_->commands.push_back(std::move(command));
}

std::wstring AudioSystem::status() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->status_text;
}

} // namespace gmod::audio
