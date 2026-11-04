#include "demo/DemoCache.h"
#include "DemoCacheVersion.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace gmod::demo {
namespace {

// Bump whenever parser interpretation, serialized fields or native layout changes.
constexpr char kSchema[] = "GModReplay-DemoData-x64-v1";

void check_cancel(const std::atomic<bool>& cancel) {
    if (cancel.load()) throw std::runtime_error("Demo cache operation cancelled.");
}

class Hash {
public:
    Hash() {
        if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &handle_, nullptr, 0, nullptr, 0, 0) < 0)
            throw std::runtime_error("Could not create demo cache hash.");
    }
    ~Hash() { BCryptDestroyHash(handle_); }
    void add(const void* data, std::size_t size) {
        if (BCryptHashData(handle_, const_cast<PUCHAR>(static_cast<const unsigned char*>(data)),
                           static_cast<ULONG>(size), 0) < 0)
            throw std::runtime_error("Could not hash demo cache data.");
    }
    std::string finish() {
        std::array<unsigned char, 32> digest{};
        if (BCryptFinishHash(handle_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            throw std::runtime_error("Could not finish demo cache hash.");
        std::string text;
        for (auto byte : digest) {
            text += "0123456789abcdef"[byte >> 4];
            text += "0123456789abcdef"[byte & 15];
        }
        return text;
    }
private:
    BCRYPT_HASH_HANDLE handle_ = nullptr;
};

bool cache_name(const std::filesystem::path& path) {
    const auto name = path.stem().string();
    return path.extension() == L".cache" && name.size() == 64
        && name.find_first_not_of("0123456789abcdef") == std::string::npos;
}

void validate_key(const std::string& key) {
    if (key.size() != 64 || key.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("Invalid demo cache key.");
}

class Writer;
class Reader;
template<class A, class T> void fields(A& a, T& v);

class Measure {
public:
    static constexpr bool reading = false;
    std::uintmax_t size = 64; // checksum footer
    void bytes(void*, std::size_t count) {
        if (count > std::numeric_limits<std::uintmax_t>::max() - size)
            throw std::runtime_error("Parsed demo is too large to cache.");
        size += count;
    }
    template<class... T> void operator()(T&... v) { (fields(*this, v), ...); }
};

class Writer {
public:
    static constexpr bool reading = false;
    Writer(const std::filesystem::path& path, const std::atomic<bool>& cancel)
        : file_(path, std::ios::binary | std::ios::trunc), cancel_(cancel) {
        if (!file_) throw std::runtime_error("Could not create parsed demo cache.");
    }
    void bytes(void* data, std::size_t size) {
        auto* p = static_cast<char*>(data);
        while (size) {
            check_cancel(cancel_);
            const auto count = std::min<std::size_t>(size, 1024 * 1024);
            file_.write(p, count);
            if (!file_) throw std::runtime_error("Could not write parsed demo cache (disk full?).");
            hash_.add(p, count);
            p += count;
            size -= count;
        }
    }
    template<class... T> void operator()(T&... v) { (fields(*this, v), ...); }
    void finish() {
        const auto digest = hash_.finish();
        file_.write(digest.data(), digest.size());
        file_.flush();
        if (!file_) throw std::runtime_error("Could not finish parsed demo cache.");
        file_.close();
    }
private:
    std::ofstream file_;
    const std::atomic<bool>& cancel_;
    Hash hash_;
};

class Reader {
public:
    static constexpr bool reading = true;
    Reader(const std::filesystem::path& path, const std::atomic<bool>& cancel)
        : file_(path, std::ios::binary), remaining_(std::filesystem::file_size(path)), cancel_(cancel) {
        if (!file_ || remaining_ < 64) throw std::runtime_error("Incomplete parsed demo cache.");
        remaining_ -= 64;
        Hash hash;
        std::vector<char> buffer(1024 * 1024);
        auto size = remaining_;
        while (size) {
            check_cancel(cancel_);
            const auto count = std::min<std::uintmax_t>(size, buffer.size());
            file_.read(buffer.data(), static_cast<std::streamsize>(count));
            if (!file_) throw std::runtime_error("Could not read parsed demo cache.");
            hash.add(buffer.data(), static_cast<std::size_t>(count));
            size -= count;
        }
        std::array<char, 64> digest{};
        file_.read(digest.data(), digest.size());
        if (!file_ || hash.finish() != std::string(digest.data(), digest.size()))
            throw std::runtime_error("Parsed demo cache checksum mismatch.");
        file_.seekg(0);
    }
    void bytes(void* data, std::size_t size) {
        if (size > remaining_) throw std::runtime_error("Truncated parsed demo cache.");
        auto* p = static_cast<char*>(data);
        while (size) {
            check_cancel(cancel_);
            const auto count = std::min<std::size_t>(size, 1024 * 1024);
            file_.read(p, count);
            if (!file_) throw std::runtime_error("Could not read parsed demo cache.");
            p += count;
            size -= count;
            remaining_ -= count;
        }
    }
    void count(std::uint64_t count, std::size_t minimum) const {
        if (count > remaining_ / minimum)
            throw std::runtime_error("Invalid parsed demo cache length.");
    }
    void finish() const {
        if (remaining_) throw std::runtime_error("Unexpected data in parsed demo cache.");
    }
    template<class... T> void operator()(T&... v) { (fields(*this, v), ...); }
private:
    std::ifstream file_;
    std::uintmax_t remaining_;
    const std::atomic<bool>& cancel_;
};

template<class A, class T> void fields(A& a, T& v) {
    static_assert(std::is_trivially_copyable_v<T>);
    a.bytes(&v, sizeof(v));
}
template<class A, class T> void fields(A& a, std::vector<T>& v) {
    std::uint64_t size = v.size();
    a(size);
    if constexpr (A::reading) {
        a.count(size, std::is_trivially_copyable_v<T> ? sizeof(T) : 1);
        v.resize(static_cast<std::size_t>(size));
    }
    if constexpr (std::is_trivially_copyable_v<T>) a.bytes(v.data(), v.size() * sizeof(T));
    else for (auto& item : v) a(item);
}
template<class A, class C> void fields(A& a, std::basic_string<C>& v) {
    std::uint64_t size = v.size();
    a(size);
    if constexpr (A::reading) {
        a.count(size, sizeof(C));
        v.resize(static_cast<std::size_t>(size));
    }
    a.bytes(v.data(), v.size() * sizeof(C));
}
template<class A> void fields(A& a, std::filesystem::path& v) {
    auto text = v.wstring();
    a(text);
    if constexpr (A::reading) v = text;
}
template<class A, class K, class V> void fields(A& a, std::unordered_map<K, V>& v) {
    std::uint64_t size = v.size();
    a(size);
    if constexpr (A::reading) {
        a.count(size, 1);
        for (std::uint64_t i = 0; i < size; ++i) {
            K key{};
            V value{};
            a(key, value);
            if (!v.emplace(std::move(key), std::move(value)).second)
                throw std::runtime_error("Duplicate parsed demo cache entry.");
        }
    } else for (auto& [key, value] : v) {
        auto key_copy = key;
        a(key_copy, value);
    }
}

template<class A> void fields(A& a, TimedText& v) { a(v.time, v.text); }
template<class A> void fields(A& a, PlayerLookKey& v) { a(v.time,v.pitch,v.yaw); }
template<class A> void fields(A& a, PlayerInfoKey& v) { a(v.time, v.health, v.team, v.life_state, v.weapon); }
template<class A> void fields(A& a, ActiveWeaponKey& v) { a(v.time, v.weapon, v.handle, v.class_name); }
template<class A> void fields(A& a, DemoPlayer& v) {
    a(v.id, v.npc, v.entity, v.user_id, v.name, v.steam_id, v.steam_id64,
      v.connected_at, v.disconnected_at, v.voice_clips, v.samples, v.look, v.presence, v.seated,
      v.vehicle_links, v.info, v.first_name, v.last_name, v.job_team, v.police_rank,
      v.medic_rank, v.firefighter_rank, v.outfit, v.ragdoll, v.radio_channel,
      v.restrained, v.phone_open, v.phone_call, v.emotes, v.active_weapon, v.shots, v.attacks, v.reloads);
}
template<class A> void fields(A& a, DemoProp::Animation& v) { a(v.time,v.sequence,v.cycle,v.rate); }
template<class A> void fields(A& a, DemoProp& v) {
    a(v.entity, v.model, v.skin, v.body, v.vehicle, v.driver_attachment, v.brush,
      v.map_attached, v.map_rotation, v.map_created_id, v.seat, v.keys, v.hidden, v.owners, v.poses, v.colors, v.animations, v.back_gun, v.back_weapon,
      v.vehicle_id, v.registration, v.plate_skin, v.vehicle_owner);
}
template<class A> void fields(A& a, DemoDoor& v) { a(v.entity, v.has_origin, v.origin, v.keys, v.yaws, v.hidden); }
template<class A> void fields(A& a, DemoEffect& v) {
    a(v.time, v.kind, v.name, v.origin, v.start, v.normal, v.entity, v.scale, v.material);
}
template<class A> void fields(A& a, DemoClothingEvent& v) {
    a(v.time, v.name, v.entity, v.entry, v.model_index, v.model_path, v.skin, v.visible);
}
template<class A> void fields(A& a, DemoVoiceClip& v) { a(v.time, v.duration, v.player, v.proximity); }
template<class A> void fields(A& a, ClothingItem& v) {
    a(v.type, v.skin, v.data_number, v.color, v.model_path, v.model_skin);
}
template<class A> void fields(A& a, ClothingRecord& v) {
    a(v.explicit_parts, v.female, v.model, v.skin, v.face, v.model_override, v.items);
}
template<class A> void fields(A& a, WeaponEntityState& v) {
    a(v.attachments, v.class_name, v.hold_type, v.shots, v.aiming, v.passive, v.reloading,
      v.grenade, v.owner, v.gone);
}
template<class A> void fields(A& a, RagdollKey& v) { a(v.time, v.pos, v.ang); }
template<class A> void fields(A& a, DemoRagdoll& v) { a(v.entity, v.serial, v.model, v.keys); }
template<class A> void fields(A& a, RadioSync& v) { a(v.time, v.channels, v.muted); }
template<class A> void fields(A& a, NpcNotice& v) { a(v.time, v.text, v.exit_text); }
template<class A> void fields(A& a, NpcGuiPage& v) { a(v.time, v.visible, v.inferred, v.title, v.text, v.options); }
template<class A> void fields(A& a, FireKey& v) { a(v.time,v.entity,v.serial,v.origin,v.scale,v.active); }
template<class A> void fields(A& a, DemoData::BankLight& v) { a(v.time,v.style,v.value); }
template<class A> void fields(A& a, DemoLogLine& v) { a(v.time, v.kind, v.category, v.entity, v.text, v.sender); }
template<class A> void fields(A& a, DemoSound& v) {
    a(v.time, v.entity, v.channel, v.flags, v.volume, v.level, v.pitch,
      v.x, v.y, v.z, v.index, v.name, v.loop);
}
template<class A> void fields(A& a, DemoData& v) {
    // path comes from the opened recording, voice_store from the extracted voice payload.
    a(v.perp_data, v.perp_data_note, v.gun_sounds, v.map_name, v.client_name, v.recorder_player,
      v.recorder_camera, v.tick_rate, v.duration, v.models, v.model_paths_by_index, v.players,
      v.props, v.recorded_map_entities, v.doors, v.effects, v.clothing, v.weapon_view_models, v.voice, v.clothing_records,
      v.weapons, v.class_holds, v.ragdolls, v.radio_sync, v.radio_channel_names, v.npc_uses,
      v.npc_notices, v.npc_gui, v.log, v.sounds, v.view_model, v.bank_lights, v.fires);
}

struct RemoveTemporary {
    std::filesystem::path path;
    ~RemoveTemporary() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

} // namespace

DemoCache::DemoCache(std::filesystem::path root) : root_(std::move(root)) {
    if (root_.empty()) throw std::runtime_error("The demo cache directory is unavailable.");
}

std::filesystem::path DemoCache::default_root() {
    wchar_t path[32768]{};
    const auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", path, 32768);
    if (!size || size >= 32768) throw std::runtime_error("LOCALAPPDATA is unavailable for the demo cache.");
    return std::filesystem::path(path) / L"GModReplay" / L"DemoCache";
}

std::string DemoCache::fingerprint(const std::filesystem::path& path,
                                 const std::atomic<bool>& cancel, const ProgressFn& progress) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Could not hash file: " + path.string());
    const auto size = std::filesystem::file_size(path);
    const auto modified = std::filesystem::last_write_time(path);
    Hash hash;
    std::vector<char> buffer(1024 * 1024);
    std::uintmax_t read = 0;
    while (file) {
        check_cancel(cancel);
        file.read(buffer.data(), buffer.size());
        const auto count = file.gcount();
        hash.add(buffer.data(), static_cast<std::size_t>(count));
        read += count;
        if (progress) progress(size ? 100.0f * static_cast<float>(read) / static_cast<float>(size) : 100.0f,
                               "Identifying recording by contents");
    }
    if (!file.eof() || read != size || std::filesystem::file_size(path) != size
        || std::filesystem::last_write_time(path) != modified)
        throw std::runtime_error("File changed or could not be read while hashing: " + path.string());
    return hash.finish();
}

std::string DemoCache::key(const std::string& demo_hash, const std::string& parser_hash) {
    Hash hash;
    hash.add(kSchema, sizeof(kSchema));
    hash.add(demo_cache_revision, sizeof(demo_cache_revision));
    hash.add(demo_hash.data(), demo_hash.size());
    hash.add(parser_hash.data(), parser_hash.size());
    return hash.finish();
}

std::optional<DemoData> DemoCache::load(const std::string& key, const std::filesystem::path& demo,
                                      const std::atomic<bool>& cancel, const ProgressFn& progress) {
    validate_key(key);
    const auto path = root_ / (key + ".cache");
    if (!std::filesystem::exists(path)) return {};
    if (progress) progress(0, "Restoring parsed demo from disk");
    DemoData data;
    Reader reader(path, cancel);
    std::string schema;
    std::string stored_key;
    reader(schema, stored_key);
    if (schema != kSchema || stored_key != key) throw std::runtime_error("Incompatible parsed demo cache.");
    reader(data);
    if (!data.perp_data.empty() && !std::filesystem::is_directory(data.perp_data))
        throw std::runtime_error("Cached demo's PERP data is no longer installed.");
    if (!data.voice.empty()) {
        data.voice_store = std::make_shared<VoiceStore>();
        data.voice_store->directory = std::filesystem::temp_directory_path()
            / ("gmod-replay-voice-" + std::to_string(GetCurrentProcessId()) + "-"
               + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(data.voice_store->directory);
    }
    std::vector<char> buffer(1024 * 1024);
    for (std::size_t i = 0; i < data.voice.size(); ++i) {
        std::uint64_t size = 0;
        reader(size);
        reader.count(size, 1);
        auto& clip = data.voice[i];
        clip.file = data.voice_store->directory / (std::to_string(i) + ".wav");
        std::ofstream file(clip.file, std::ios::binary);
        while (size) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size, buffer.size()));
            reader.bytes(buffer.data(), count);
            file.write(buffer.data(), count);
            size -= count;
        }
        file.flush();
        if (!file) throw std::runtime_error("Could not restore cached voice files.");
    }
    reader.finish();
    data.path = demo;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now());
    if (progress) progress(100, "Parsed demo restored from disk");
    return data;
}

bool DemoCache::touch(const std::string& key) {
    validate_key(key);
    const auto path = root_ / (key + ".cache");
    if (!std::filesystem::exists(path)) return false;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now());
    return true;
}

bool DemoCache::store(const std::string& key, const DemoData& data, std::uintmax_t limit,
                     const std::atomic<bool>& cancel, const ProgressFn& progress) {
    validate_key(key);
    check_cancel(cancel);
    std::string schema = kSchema;
    auto stored_key = key;
    Measure measure;
    measure(schema, stored_key, const_cast<DemoData&>(data));
    for (const auto& clip : data.voice) {
        auto size = std::filesystem::file_size(clip.file);
        measure(size);
        if (size > std::numeric_limits<std::size_t>::max())
            throw std::runtime_error("Decoded voice is too large to cache.");
        measure.bytes(nullptr, static_cast<std::size_t>(size));
    }
    if (measure.size > limit) return false;
    std::filesystem::create_directories(root_);
    const auto path = root_ / (key + ".cache");
    const auto pending = root_ / (key + "." + std::to_string(GetCurrentProcessId()) + ".pending");
    RemoveTemporary cleanup{pending};
    if (progress) progress(0, "Saving parsed demo to disk cache");
    Writer writer(pending, cancel);
    writer(schema, stored_key);
    // Serialization is read-only on the writer side.
    writer(const_cast<DemoData&>(data));
    std::vector<char> buffer(1024 * 1024);
    for (const auto& clip : data.voice) {
        std::uint64_t size = std::filesystem::file_size(clip.file);
        writer(size);
        std::ifstream file(clip.file, std::ios::binary);
        while (size) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(size, buffer.size()));
            file.read(buffer.data(), count);
            if (!file) throw std::runtime_error("Could not cache decoded voice file: " + clip.file.string());
            writer.bytes(buffer.data(), count);
            size -= count;
        }
    }
    writer.finish();
    const auto bytes = std::filesystem::file_size(pending);
    if (bytes > limit) return false;
    trim(limit - bytes);
    if (!MoveFileExW(pending.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Could not publish parsed demo cache: " + std::to_string(GetLastError()));
    if (progress) progress(100, "Parsed demo saved to disk cache");
    return true;
}

DemoCache::Usage DemoCache::usage() const {
    Usage result;
    if (!std::filesystem::exists(root_)) return result;
    for (const auto& file : std::filesystem::directory_iterator(root_)) {
        if (file.is_regular_file() && cache_name(file.path())) {
            result.bytes += file.file_size();
            ++result.entries;
        }
    }
    return result;
}

void DemoCache::trim(std::uintmax_t limit) {
    if (!std::filesystem::exists(root_)) return;
    struct Entry { std::filesystem::path path; std::filesystem::file_time_type used; std::uintmax_t bytes; };
    std::vector<Entry> entries;
    std::uintmax_t total = 0;
    for (const auto& file : std::filesystem::directory_iterator(root_)) {
        if (!file.is_regular_file() || !cache_name(file.path())) continue;
        entries.push_back({file.path(), file.last_write_time(), file.file_size()});
        total += entries.back().bytes;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
    for (const auto& file : entries) {
        if (total <= limit) break;
        std::filesystem::remove(file.path);
        total -= file.bytes;
    }
}

void DemoCache::clear() { trim(0); }

} // namespace gmod::demo
