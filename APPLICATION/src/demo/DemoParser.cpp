#include "demo/DemoData.h"
#include "demo/BankShutters.h"
#include "source/model/Animation.h"
#include "demo/Perpheads.h"
#include "demo/SourceDemoReader.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace gmod::demo {
namespace {

// ---- Minimal streaming JSON reader --------------------------------------------
// The parser's output reaches hundreds of megabytes, so values the viewer does
// not need are skipped without being materialised.

class Json {
public:
    Json(const char* begin, const char* end) : begin_(begin), p_(begin), end_(end) {}

    char peek() {
        skip_space();
        return p_ < end_ ? *p_ : '\0';
    }

    bool eat(char c) {
        skip_space();
        if (p_ < end_ && *p_ == c) {
            ++p_;
            return true;
        }
        return false;
    }

    void expect(char c) {
        if (!eat(c)) {
            fail(std::string("expected '") + c + "'");
        }
    }

    // Object keys never contain escapes in the parser's output.
    std::string_view key() {
        skip_space();
        if (p_ >= end_ || *p_ != '"') fail("expected a string key");
        const char* start = ++p_;
        while (p_ < end_ && *p_ != '"') {
            if (*p_ == '\\') ++p_;
            ++p_;
        }
        if (p_ >= end_) fail("unterminated string");
        std::string_view view(start, static_cast<std::size_t>(p_ - start));
        ++p_;
        expect(':');
        return view;
    }

    // A string value; plain strings are returned as views into the buffer.
    std::string_view string_view_value(std::string& scratch) {
        skip_space();
        if (end_ - p_ >= 4 && std::string_view(p_, 4) == "null") {
            p_ += 4;
            return {};
        }
        if (p_ >= end_ || *p_ != '"') fail("expected a string");
        const char* start = ++p_;
        bool escaped = false;
        while (p_ < end_ && *p_ != '"') {
            if (*p_ == '\\') {
                escaped = true;
                ++p_;
            }
            ++p_;
        }
        if (p_ >= end_) fail("unterminated string");
        std::string_view raw(start, static_cast<std::size_t>(p_ - start));
        ++p_;
        if (!escaped) return raw;
        scratch = unescape(raw);
        return scratch;
    }

    std::string string_value() {
        std::string scratch;
        return std::string(string_view_value(scratch));
    }

    double number() {
        skip_space();
        double value = 0.0;
        const auto result = std::from_chars(p_, end_, value);
        if (result.ec != std::errc{}) fail("expected a number");
        p_ = result.ptr;
        return value;
    }

    bool boolean() {
        skip_space();
        if (end_ - p_ >= 4 && std::string_view(p_, 4) == "true") {
            p_ += 4;
            return true;
        }
        if (end_ - p_ >= 5 && std::string_view(p_, 5) == "false") {
            p_ += 5;
            return false;
        }
        fail("expected a boolean");
    }

    void skip() {
        skip_space();
        if (p_ >= end_) fail("unexpected end of JSON");
        const char c = *p_;
        if (c == '"') {
            std::string scratch;
            string_view_value(scratch);
        } else if (c == '{' || c == '[') {
            int depth = 0;
            while (p_ < end_) {
                const char d = *p_++;
                if (d == '"') {
                    while (p_ < end_ && *p_ != '"') {
                        if (*p_ == '\\') ++p_;
                        ++p_;
                    }
                    ++p_;
                } else if (d == '{' || d == '[') {
                    ++depth;
                } else if (d == '}' || d == ']') {
                    if (--depth == 0) return;
                }
            }
            fail("unterminated value");
        } else {
            while (p_ < end_ && *p_ != ',' && *p_ != '}' && *p_ != ']'
                   && *p_ > ' ') ++p_;
        }
    }

    template <typename F>
    void object(F&& member) {
        expect('{');
        if (eat('}')) return;
        do {
            member(key());
        } while (eat(','));
        expect('}');
    }

    template <typename F>
    void array(F&& element) {
        expect('[');
        if (eat(']')) return;
        do {
            element();
        } while (eat(','));
        expect(']');
    }

    std::size_t offset() const { return static_cast<std::size_t>(p_ - begin_); }
    std::size_t size() const { return static_cast<std::size_t>(end_ - begin_); }

private:
    void skip_space() {
        while (p_ < end_ && static_cast<unsigned char>(*p_) <= ' ') ++p_;
    }

    [[noreturn]] void fail(const std::string& what) const {
        throw std::runtime_error("Demo JSON is malformed at byte "
                                 + std::to_string(p_ - begin_) + ": " + what);
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    static std::string unescape(std::string_view raw) {
        std::string out;
        out.reserve(raw.size());
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] != '\\' || i + 1 >= raw.size()) {
                out += raw[i];
                continue;
            }
            const char c = raw[++i];
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 >= raw.size()) break;
                unsigned code = static_cast<unsigned>(
                    std::strtoul(std::string(raw.substr(i + 1, 4)).c_str(), nullptr, 16));
                i += 4;
                // Surrogate pair.
                if (code >= 0xD800 && code < 0xDC00 && i + 6 < raw.size()
                    && raw[i + 1] == '\\' && raw[i + 2] == 'u') {
                    const unsigned low = static_cast<unsigned>(std::strtoul(
                        std::string(raw.substr(i + 3, 4)).c_str(), nullptr, 16));
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    i += 6;
                }
                append_utf8(out, code);
                break;
            }
            default: out += c; break;
            }
        }
        return out;
    }

    const char* begin_;
    const char* p_;
    const char* end_;
};

// ---- Demo JSON -> DemoData ------------------------------------------------------

float lerp(float a, float b, float t) { return a + (b - a) * t; }

float lerp_angle(float a, float b, float t) {
    float delta = std::fmod(b - a, 360.0f);
    if (delta > 180.0f) delta -= 360.0f;
    if (delta < -180.0f) delta += 360.0f;
    return a + delta * t;
}

class Builder {
public:
    explicit Builder(DemoData& data) : data_(data) {}

    int intern_model(std::string_view name) {
        const auto found = model_ids_.find(std::string(name));
        if (found != model_ids_.end()) return found->second;
        const int id = static_cast<int>(data_.models.size());
        data_.models.emplace_back(name);
        model_ids_.emplace(std::string(name), id);
        return id;
    }

    void parse(Json& json, const ProgressFn& progress, const std::atomic<bool>& cancel) {
        json.object([&](std::string_view key) {
            if (key == "header") {
                parse_header(json);
            } else if (key == "players") {
                json.array([&] {
                    parse_player(json);
                    check(json, progress, cancel);
                });
            } else if (key == "objects") {
                json.array([&] {
                    parse_object(json);
                    check(json, progress, cancel);
                });
            } else if (key == "models") {
                parse_models(json);
            } else if (key == "clothing") {
                json.array([&] {
                    parse_clothing(json);
                    check(json, progress, cancel);
                });
            } else if (key == "nwvars") {
                json.array([&] {
                    parse_nwvar(json);
                    check(json, progress, cancel);
                });
            } else if (key == "clothingData") {
                json.array([&] { parse_clothing_data(json); });
            } else if (key == "teams") {
                json.array([&] { parse_team(json); });
            } else if (key == "weaponState") {
                json.array([&] {
                    parse_weapon_state(json);
                    check(json, progress, cancel);
                });
            } else if (key == "ragdolls") {
                json.array([&] {
                    parse_ragdoll(json);
                    check(json, progress, cancel);
                });
            } else if (key == "uiEvents") {
                json.array([&] { parse_ui_event(json); });
            } else if (key == "lightStyles") {
                json.array([&] {
                    DemoData::BankLight light;
                    std::string pattern;
                    json.object([&](std::string_view field) {
                        if (field == "time") light.time=static_cast<float>(json.number());
                        else if (field == "style") light.style=static_cast<int>(json.number());
                        else if (field == "pattern") pattern=json.string_value();
                        else json.skip();
                    });
                    if (light.style>=32 && light.style<64 && !pattern.empty()
                        && pattern.find_first_not_of("abcdefghijklmnopqrstuvwxyz")==std::string::npos) {
                        float total=0;
                        for(char c:pattern) total+=float(c-'a')/12.0f;
                        light.value=total/float(pattern.size());
                        data_.bank_lights.push_back(light);
                    }
                });
            } else if (key == "npcGui") {
                json.array([&] {
                    NpcGuiPage page;
                    json.object([&](std::string_view field) {
                        if (field == "time") page.time = static_cast<float>(json.number());
                        else if (field == "visible") page.visible = json.boolean();
                        else if (field == "inferred") page.inferred = json.boolean();
                        else if (field == "title") page.title = json.string_value();
                        else if (field == "text") page.text = json.string_value();
                        else if (field == "options") json.array([&] { page.options.push_back(json.string_value()); });
                        else json.skip();
                    });
                    data_.npc_gui.push_back(std::move(page));
                });
            } else if (key == "playerAnimations") {
                json.array([&] {
                    float time = 0;
                    int entity = -1, event = -1;
                    json.object([&](std::string_view field) {
                        if (field == "time") time = static_cast<float>(json.number());
                        else if (field == "entity") entity = static_cast<int>(json.number());
                        else if (field == "event") event = static_cast<int>(json.number());
                        else json.skip();
                    });
                    const auto player = player_for_entity(entity, time);
                    if (player != NoPlayer && (event == 0 || event == 1))
                        data_.players[player].attacks.push_back({time,event});
                    else if (player != NoPlayer && event >= 3 && event <= 5)
                        data_.players[player].reloads.push_back({time,event});
                });
            } else if (key == "viewModels") {
                json.array([&] {
                    parse_view_model(json);
                    check(json, progress, cancel);
                });
            } else if (key == "poses") {
                json.array([&] {
                    parse_pose(json);
                    check(json, progress, cancel);
                });
            } else if (key == "fires") {
                json.array([&] { parse_fire(json); });
            } else if (key == "effects") {
                json.array([&] { parse_effect(json); });
            } else if (key == "sounds") {
                json.array([&] {
                    parse_sound(json);
                    check(json, progress, cancel);
                });
            } else {
                json.skip();
            }
        });
        finish();
    }

private:
    struct Object {
        int type = 0;  // 1 spawn, 2 move, 3 remove, 4 door, 5 owner
        std::string_view cls;
        std::string_view model;
        std::string_view state;
        std::string_view name;
        std::string_view lua_class;
        int entity = -1;
        float time = 0.0f;
        bool has_time = false;
        int tick = -1;
        bool has_position = false;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        bool has_yaw = false;
        bool hidden = false;
        float yaw = 0.0f;
        float pitch = 0.0f;
        float roll = 0.0f;
        int skin = 0;
        int body = 0;
        bool map_entity = false;
        int map_created_id = -1;
        bool vehicle = false;
        bool weapon = false;
        int sequence=0;
        float cycle=0,rate=1;
        bool cycle_recorded=false;
        int owner = -1;
        int parent = -1;
        long long creation = 0;
    };

    void check(const Json& json, const ProgressFn& progress, const std::atomic<bool>& cancel) {
        if ((++counter_ & 0x3FF) != 0) return;
        if (cancel) throw std::runtime_error("cancelled");
        progress(85.0f + 15.0f * static_cast<float>(json.offset())
                     / static_cast<float>(std::max<std::size_t>(json.size(), 1)),
                 "Reading demo data");
    }

    void parse_header(Json& json) {
        json.object([&](std::string_view key) {
            if (key == "mapName") {
                data_.map_name = json.string_value();
            } else if (key == "clientName") {
                data_.client_name = json.string_value();
            } else if (key == "tickRate") {
                if (json.peek() == 'n') {
                    json.skip();
                    // JsonExporter uses this rate for its timestamps when the header is unset.
                    data_.tick_rate = 66.6666666667f;
                } else {
                    data_.tick_rate = static_cast<float>(json.number());
                }
            } else if (key == "playbackTime") {
                data_.duration = static_cast<float>(json.number());
            } else {
                json.skip();
            }
        });
    }

    void parse_player(Json& json) {
        DemoPlayer player;
        // Carry-forward state, exactly like the reference replay builder.
        bool has_x = false, has_y = false, has_z = false;
        float cx = 0, cy = 0, cz = 0;
        float pitch = 0, yaw = 0, roll = 0;
        int model = -1;
        int vehicle = -1, parent = -1;
        int segment = 0;
        int flags = 1;  // networked flags are only sent once they differ; start on the ground
        bool invalid = false;
        bool seated = false;
        PlayerInfoKey info_state;
        json.object([&](std::string_view key) {
            if (key == "id") player.id = static_cast<int>(json.number());
            else if (key == "entity") player.entity = static_cast<int>(json.number());
            else if (key == "userId") player.user_id = static_cast<int>(json.number());
            else if (key == "name") player.name = json.string_value();
            else if (key == "steamId") player.steam_id = json.string_value();
            else if (key == "steamId64") player.steam_id64 = json.string_value();
            else if (key == "connectedAt") player.connected_at = static_cast<float>(json.number());
            else if (key == "disconnectedAt") player.disconnected_at = static_cast<float>(json.number());
            else if (key == "voice") {
                const int player_index = static_cast<int>(data_.players.size());
                json.array([&] {
                    DemoVoiceClip clip;
                    clip.player = player_index;
                    json.object([&](std::string_view clip_key) {
                        if (clip_key == "time") clip.time = static_cast<float>(json.number());
                        else if (clip_key == "duration") clip.duration = static_cast<float>(json.number());
                        else if (clip_key == "file") clip.file = std::filesystem::u8path(json.string_value());
                        else if (clip_key == "proximity") clip.proximity = json.boolean();
                        else json.skip();
                    });
                    ++player.voice_clips;
                    if (!clip.file.empty()) data_.voice.push_back(std::move(clip));
                });
            }
            else if (key == "ticks") {
                json.array([&] {
                    bool tx = false, ty = false, tz = false;
                    float x = 0, y = 0, z = 0, time = 0;
                    bool has_time = false;
                    bool spawn = false, gone = false;
                    bool info_changed = false;
                    bool vehicle_changed = false;
                    bool look_changed = false;
                    json.object([&](std::string_view tick_key) {
                        if (tick_key == "time") { time = static_cast<float>(json.number()); has_time = true; }
                        else if (tick_key == "x") { x = static_cast<float>(json.number()); tx = true; }
                        else if (tick_key == "y") { y = static_cast<float>(json.number()); ty = true; }
                        else if (tick_key == "z") { z = static_cast<float>(json.number()); tz = true; }
                        else if (tick_key == "pitch") { pitch = static_cast<float>(json.number()); look_changed = true; }
                        else if (tick_key == "yaw") { yaw = static_cast<float>(json.number()); look_changed = true; }
                        else if (tick_key == "roll") roll = static_cast<float>(json.number());
                        else if (tick_key == "flags") flags = static_cast<int>(json.number());
                        else if (tick_key == "vehicle") {
                            vehicle = static_cast<int>(json.number());
                            vehicle_changed = true;
                        }
                        else if (tick_key == "parent") {
                            parent = static_cast<int>(json.number());
                            vehicle_changed = true;
                        }
                        else if (tick_key == "health") { info_state.health = static_cast<int>(json.number()); info_changed = true; }
                        else if (tick_key == "team") { info_state.team = static_cast<int>(json.number()); info_changed = true; }
                        else if (tick_key == "lifeState") { info_state.life_state = static_cast<int>(json.number()); info_changed = true; }
                        else if (tick_key == "weapon") {
                            // A vehicle name here is a stale handle, not something held.
                            info_state.weapon = json.string_value();
                            if (info_state.weapon.rfind("prop_vehicle", 0) == 0) {
                                info_state.weapon.clear();
                            }
                            info_changed = true;
                        } else if (tick_key == "model") {
                            std::string scratch;
                            model = intern_model(json.string_view_value(scratch));
                        } else if (tick_key == "spawn") spawn = json.boolean();
                        else if (tick_key == "gone") gone = json.boolean();
                        else json.skip();
                    });
                    if (!has_time) return;
                    if (look_changed) player.look.push_back({time,pitch,yaw});
                    if (info_changed) {
                        info_state.time = time;
                        player.info.push_back(info_state);
                    }
                    if (vehicle_changed) {
                        player.vehicle_links.push_back({time, vehicle, parent});
                    }
                    if (spawn) player.presence.push_back({time, true});
                    if (gone) player.presence.push_back({time, false});
                    if (!(tx || ty || tz)) return;
                    const bool ok_x = tx || has_x, ok_y = ty || has_y, ok_z = tz || has_z;
                    if (!(ok_x && ok_y && ok_z)) return;
                    const float px = tx ? x : cx, py = ty ? y : cy, pz = tz ? z : cz;
                    // (0, 0, 0) is the engine's "unknown" and starts a new segment. For a
                    // player it is also what a parented (vehicle) occupant reports.
                    if (px == 0.0f && py == 0.0f && pz == 0.0f) {
                        has_x = has_y = has_z = false;
                        invalid = true;
                        if (!seated) {
                            seated = true;
                            player.seated.push_back({time, true});
                        }
                        return;
                    }
                    if (seated) {
                        seated = false;
                        player.seated.push_back({time, false});
                    }
                    if (invalid) {
                        ++segment;
                        invalid = false;
                    }
                    cx = px; cy = py; cz = pz;
                    has_x = has_y = has_z = true;
                    player.samples.push_back({time, px, py, pz, pitch, yaw, roll, model, segment, flags});
                });
            } else json.skip();
        });
        data_.players.push_back(std::move(player));
    }

    void parse_models(Json& json) {
        json.object([&](std::string_view key) {
            int index = -1;
            const auto parsed = std::from_chars(key.data(), key.data() + key.size(), index);
            if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || index < 0) {
                throw std::runtime_error("Demo model table contains a non-numeric index.");
            }
            data_.model_paths_by_index[index] = json.string_value();
        });
    }

    static std::string scalar_string(Json& json) {
        switch (json.peek()) {
        case '"': return json.string_value();
        case 'n':
            json.skip();
            return {};
        case 't': return json.boolean() ? "true" : "false";
        case 'f': return json.boolean() ? "true" : "false";
        default: {
            const auto value = json.number();
            if (std::floor(value) == value) {
                return std::to_string(static_cast<long long>(value));
            }
            return std::to_string(value);
        }
        }
    }

    static constexpr std::size_t NoPlayer = static_cast<std::size_t>(-1);

    std::size_t player_for_entity(int entity, float time) {
        const int index = player_index_at(data_, entity, time);
        return index < 0 ? NoPlayer : static_cast<std::size_t>(index);
    }

    template <typename T>
    static void push_changed(std::vector<T>& keys, T key) {
        if (!keys.empty()) {
            const bool same = [&] {
                if constexpr (std::is_same_v<T, TimedInt>) return keys.back().value == key.value;
                else return keys.back().text == key.text;
            }();
            if (same) return;
        }
        keys.push_back(std::move(key));
    }

    // A vehicle's networked pose parameters. Each entry lists the parameters that changed; the
    // track keeps the running values so every key is a full snapshot.
    void parse_pose(Json& json) {
        int entity = -1, tick = 0;
        std::vector<int> indices;
        std::vector<float> values;
        json.object([&](std::string_view name) {
            if (name == "e") entity = static_cast<int>(json.number());
            else if (name == "t") tick = static_cast<int>(json.number());
            else if (name == "i") json.array([&] { indices.push_back(static_cast<int>(json.number())); });
            else if (name == "v") json.array([&] { values.push_back(static_cast<float>(json.number())); });
            else json.skip();
        });
        const float time = static_cast<float>(tick) / std::max(data_.tick_rate, 1.0f);
        auto* prop = track_at(entity, time);
        if (!prop || !prop->vehicle) return;
        auto& running = pose_state_[static_cast<std::size_t>(prop - data_.props.data())];
        const std::size_t count = std::min(indices.size(), values.size());
        for (std::size_t n = 0; n < count; ++n) {
            if (indices[n] >= 0 && static_cast<std::size_t>(indices[n]) < VehiclePoseCount) {
                running[static_cast<std::size_t>(indices[n])] = values[n];
            }
        }
        if (!prop->poses.empty() && prop->poses.back().time == time) {
            prop->poses.back().value = running;
        } else {
            prop->poses.push_back({time, running});
        }
    }

    // Networked variables the viewer reads: the character name (rp_fname / rp_lname,
    // exactly what the server's own ESP shows) and the job ranks behind the short title.
    void parse_nwvar(Json& json) {
        int entity = -1;
        float time = 0.0f;
        bool unset = false;
        std::string op, key_name, text;
        double number = 0.0;
        bool has_number = false;
        float vector_value[3]{1.0f, 1.0f, 1.0f};
        bool has_vector = false;
        json.object([&](std::string_view key) {
            if (key == "e") entity = static_cast<int>(json.number());
            else if (key == "time") time = static_cast<float>(json.number());
            else if (key == "op") op = json.string_value();
            else if (key == "key") key_name = json.string_value();
            else if (key == "value") {
                const char next = json.peek();
                if (next == '"') {
                    text = json.string_value();
                } else if (next == '-' || (next >= '0' && next <= '9')) {
                    number = json.number();
                    has_number = true;
                } else if (next == 't' || next == 'f') {
                    number = json.boolean() ? 1.0 : 0.0;
                    has_number = true;
                } else if (next == '[') {
                    int axis = 0;
                    json.array([&] {
                        const auto value = static_cast<float>(json.number());
                        if (axis < 3) vector_value[axis] = value;
                        ++axis;
                    });
                    has_vector = axis >= 3;
                } else {
                    json.skip();
                }
            } else {
                json.skip();
            }
        });
        unset = op == "unset";
        if (op != "set" && !unset) return;
        if (key_name == "registration" || key_name == "licenseplateskin" || key_name == "id") {
            if (auto* prop = track_at(entity, time); prop && prop->vehicle) {
                auto& keys = key_name == "id" ? prop->vehicle_id
                    : key_name == "registration" ? prop->registration : prop->plate_skin;
                push_changed(keys, TimedText{time, unset ? std::string() : text});
            }
            return;
        }
        // The player who owns a vehicle (an Entity NWVar holding their entity index).
        if (key_name == "owner") {
            if (auto* prop = track_at(entity, time); prop && prop->vehicle)
                push_changed(prop->vehicle_owner,
                             TimedInt{time, unset || !has_number || number <= 0 ? -1 : static_cast<int>(number)});
            return;
        }
        // A vehicle's paint colour (the server tints the body material with it).
        if (key_name == "color" && has_vector) {
            if (auto* prop = track_at(entity, time); prop && prop->vehicle) {
                prop->colors.push_back({time, vector_value[0], vector_value[1], vector_value[2]});
            }
            return;
        }
        if(key_name=="swep") {
            if(auto* gun=track_at(entity,time);gun && gun->back_gun)
                push_changed(gun->back_weapon,TimedInt{time,unset || !has_number ? -1 : static_cast<int>(number)});
            return;
        }
        const bool name_key = key_name == "rp_fname" || key_name == "rp_lname";
        const bool rank_key = key_name == "policerank" || key_name == "medicrank"
            || key_name == "firefighterrank";
        if (key_name == "ragdoll" || key_name == "governmentradiotransmittingchannel"
            || key_name == "restrained" || key_name == "phoneopen" || key_name == "phonecall") {
            const auto player = player_for_entity(entity, time);
            if (player != NoPlayer) {
                auto& target = data_.players[player];
                auto& keys = key_name == "ragdoll" ? target.ragdoll
                    : key_name == "restrained" ? target.restrained
                    : key_name == "phoneopen" ? target.phone_open
                    : key_name == "phonecall" ? target.phone_call
                                              : target.radio_channel;
                push_changed(keys, TimedInt{time, unset || !has_number ? 0 : static_cast<int>(number)});
            }
            return;
        }
        if (!name_key && !rank_key) return;
        const auto player = player_for_entity(entity, time);
        if (player == NoPlayer) return;
        auto& target = data_.players[player];
        if (name_key) {
            push_changed(key_name == "rp_fname" ? target.first_name : target.last_name,
                         TimedText{time, unset ? std::string() : text});
            return;
        }
        int rank = 0;
        if (!unset) {
            rank = has_number ? static_cast<int>(number) : std::atoi(text.c_str());
        }
        auto& keys = key_name == "policerank" ? target.police_rank
            : key_name == "medicrank" ? target.medic_rank : target.firefighter_rank;
        push_changed(keys, TimedInt{time, rank});
    }

    struct ClothingEntry {
        int index = -1;
        int entity = -1;
        float time = 0.0f;
        std::string kind;
        std::vector<int> entities;
    };

    void parse_clothing_data(Json& json) {
        ClothingEntry entry;
        ClothingRecord record;
        json.object([&](std::string_view key) {
            if (key == "kind") entry.kind = json.string_value();
            else if (key == "time") entry.time = static_cast<float>(json.number());
            else if (key == "index") entry.index = static_cast<int>(json.number());
            else if (key == "e") entry.entity = static_cast<int>(json.number());
            else if (key == "entities") {
                json.array([&] { entry.entities.push_back(static_cast<int>(json.number())); });
            } else if (key == "explicitParts") record.explicit_parts = json.boolean();
            else if (key == "sex") record.female = json.number() == 0.0;
            else if (key == "model") record.model = static_cast<int>(json.number());
            else if (key == "modelOverride") record.model_override = json.string_value();
            else if (key == "skin") record.skin = static_cast<int>(json.number());
            else if (key == "face") record.face = static_cast<int>(json.number());
            else if (key == "clothing") {
                json.array([&] {
                    ClothingItem item;
                    json.object([&](std::string_view item_key) {
                        if (item_key == "type") item.type = static_cast<int>(json.number());
                        else if (item_key == "skin") item.skin = static_cast<int>(json.number());
                        else if (item_key == "dataNumber") item.data_number = static_cast<int>(json.number());
                        else if (item_key == "modelPath") item.model_path = json.string_value();
                        else if (item_key == "modelSkin") item.model_skin = static_cast<int>(json.number());
                        else if (item_key == "color") {
                            int channel = 0;
                            json.array([&] {
                                const auto value = static_cast<int>(json.number());
                                if (channel < 4) item.color[channel] = static_cast<unsigned char>(value);
                                ++channel;
                            });
                        } else {
                            json.skip();
                        }
                    });
                    record.items.push_back(item);
                });
            } else {
                json.skip();
            }
        });
        if (entry.index < 0) return;
        if (entry.kind == "store") {
            data_.clothing_records[entry.index] = std::move(record);
            for (const int entity : entry.entities) add_outfit(entity, entry.time, entry.index);
        } else if (entry.kind == "reference") {
            add_outfit(entry.entity, entry.time, entry.index);
        }
    }

    void add_outfit(int entity, float time, int index) {
        const auto player = player_for_entity(entity, time);
        if (player == NoPlayer) return;
        push_changed(data_.players[player].outfit, TimedInt{time, index});
    }

    void parse_team(Json& json) {
        int entity = -1;
        int team = 0;
        float time = 0.0f;
        json.object([&](std::string_view key) {
            if (key == "e") entity = static_cast<int>(json.number());
            else if (key == "team") team = static_cast<int>(json.number());
            else if (key == "time") time = static_cast<float>(json.number());
            else json.skip();
        });
        const auto player = player_for_entity(entity, time);
        if (player == NoPlayer) return;
        push_changed(data_.players[player].job_team, TimedInt{time, team});
    }

    void parse_ui_event(Json& json) {
        std::string kind, text, text2;
        int id = 0, entity = 0;
        float time = 0.0f;
        std::vector<int> channels, muted;
        json.object([&](std::string_view key) {
            if (key == "kind") kind = json.string_value();
            else if (key == "time") time = static_cast<float>(json.number());
            else if (key == "id") id = static_cast<int>(json.number());
            else if (key == "e") entity = static_cast<int>(json.number());
            else if (key == "text") text = json.string_value();
            else if (key == "text2") text2 = json.string_value();
            else if (key == "channels") json.array([&] { channels.push_back(static_cast<int>(json.number())); });
            else if (key == "muted") json.array([&] { muted.push_back(static_cast<int>(json.number())); });
            else json.skip();
        });
        if (kind == "radioSync") data_.radio_sync.push_back({time, std::move(channels), std::move(muted)});
        else if (kind == "radioChannel") data_.radio_channel_names[id] = text;
        else if (kind == "npcUsed") data_.npc_uses.push_back({time, id, entity});
        else if (kind == "npcNotify") data_.npc_notices.push_back({time, text, text2});
        else if (kind == "bulletSound" || kind == "impactSound") {
            DemoData::GunSound sound;
            sound.time = time;
            sound.index = id;
            sound.impact = kind == "impactSound";
            std::sscanf(text.c_str(), "%f %f %f", &sound.x, &sound.y, &sound.z);
            data_.gun_sounds.push_back(sound);
        }
        else if (kind == "playAnim" || kind == "stopAnim") {
            const auto player = player_for_entity(entity, time);
            if (player == NoPlayer) return;
            auto& emotes = data_.players[player].emotes;
            EmoteKey key;
            key.time = time;
            key.sequence = kind == "playAnim" ? id : -1;
            key.once = text2 != "0";
            // The server sends each act once per gesture slot; one start is enough.
            if (!emotes.empty() && emotes.back().sequence == key.sequence
                && std::fabs(emotes.back().time - time) < 0.05f) {
                return;
            }
            emotes.push_back(key);
        }
        else if (kind == "chat" || kind == "chatOoc" || kind == "chatNarration" || kind == "chatNpc"
                 || kind == "chatAnnouncement" || kind == "notify") {
            DemoLogLine line;
            line.time = time;
            line.category = id;
            line.entity = entity;
            if (kind == "chat") line.kind = LogKind::Chat;
            else if (kind == "chatOoc") line.kind = LogKind::Ooc;
            else if (kind == "chatNarration") line.kind = LogKind::Narration;
            else if (kind == "chatNpc") line.kind = LogKind::Npc;
            else if (kind == "chatAnnouncement") line.kind = LogKind::Announcement;
            else line.kind = LogKind::Notify;
            if (kind == "chatNpc") {
                line.sender = std::move(text);
                line.text = std::move(text2);
            } else {
                line.text = std::move(text);
            }
            data_.log.push_back(std::move(line));
        }
    }

    // svc_Sounds entries: footsteps are synthesised from movement, so the server's own are
    // left out, as are the engine's silent placeholder sounds.
    void parse_fire(Json& json) {
        FireKey fire;
        json.object([&](std::string_view key) {
            if(key=="time") fire.time=static_cast<float>(json.number());
            else if(key=="entity") fire.entity=static_cast<int>(json.number());
            else if(key=="serial") fire.serial=static_cast<int>(json.number());
            else if(key=="scale") fire.scale=static_cast<float>(json.number());
            else if(key=="active") fire.active=json.boolean();
            else if(key=="origin") { std::size_t i=0;json.array([&] { const float v=static_cast<float>(json.number());if(i<3)fire.origin[i]=v;++i; }); }
            else json.skip();
        });
        if(fire.entity>=0 && std::isfinite(fire.time) && std::isfinite(fire.scale)
            && fire.scale>=0 && std::all_of(fire.origin.begin(),fire.origin.end(),[](float v){return std::isfinite(v);})) {
            fire.scale=std::clamp(fire.scale,0.0f,8.0f);data_.fires.push_back(fire);
        }
    }

    void parse_effect(Json& json) {
        DemoEffect effect;
        json.object([&](std::string_view key) {
            if (key == "time") effect.time = static_cast<float>(json.number());
            else if (key == "kind") effect.kind = json.string_value();
            else if (key == "name") effect.name = json.string_value();
            else if (key == "entity") effect.entity = static_cast<int>(json.number());
            else if (key == "scale") effect.scale = static_cast<float>(json.number());
            else if (key == "material") effect.material = static_cast<float>(json.number());
            else if (key == "origin" || key == "start" || key == "normal") {
                auto& target = key == "origin" ? effect.origin : key == "start" ? effect.start : effect.normal;
                std::size_t axis = 0;
                json.array([&] {
                    const float value = static_cast<float>(json.number());
                    if (axis < target.size()) target[axis] = value;
                    ++axis;
                });
            } else json.skip();
        });
        data_.effects.push_back(std::move(effect));
    }

    void parse_sound(Json& json) {
        DemoSound sound;
        int tick = 0;
        json.object([&](std::string_view key) {
            if (key == "t") tick = static_cast<int>(json.number());
            else if (key == "e") sound.entity = static_cast<int>(json.number());
            else if (key == "n") sound.index = static_cast<int>(json.number());
            else if (key == "name") sound.name = json.string_value();
            else if (key == "f") sound.flags = static_cast<int>(json.number());
            else if (key == "c") sound.channel = static_cast<int>(json.number());
            else if (key == "v") sound.volume = static_cast<float>(json.number());
            else if (key == "lvl") sound.level = static_cast<int>(json.number());
            else if (key == "p") sound.pitch = static_cast<int>(json.number());
            else if (key == "pos") {
                float* axes[3]{&sound.x, &sound.y, &sound.z};
                int axis = 0;
                json.array([&] {
                    const float value = static_cast<float>(json.number());
                    if (axis < 3) *axes[axis] = value;
                    ++axis;
                });
            } else json.skip();
        });
        if ((sound.flags & 7) == 0 && sound.name != "common/null.wav"
            && (sound.name.empty() || sound.name.rfind("player/footsteps/", 0) == 0)) {
            return;
        }
        sound.time = static_cast<float>(tick) / std::max(data_.tick_rate, 1.0f);
        data_.sounds.push_back(std::move(sound));
    }

    void parse_ragdoll(Json& json) {
        int entity = -1, serial = -1, model = -1, tick = 0;
        RagdollKey key;
        json.object([&](std::string_view name) {
            if (name == "e") entity = static_cast<int>(json.number());
            else if (name == "serial") serial = static_cast<int>(json.number());
            else if (name == "model") model = static_cast<int>(json.number());
            else if (name == "t") tick = static_cast<int>(json.number());
            else if (name == "pos") json.array([&] { key.pos.push_back(static_cast<float>(json.number())); });
            else if (name == "ang") json.array([&] { key.ang.push_back(static_cast<float>(json.number())); });
            else json.skip();
        });
        key.time = static_cast<float>(tick) / std::max(data_.tick_rate, 1.0f);
        // One ragdoll per (entity, serial); a reused entity index starts a new one.
        auto& list = data_.ragdolls;
        DemoRagdoll* target = nullptr;
        for (auto it = list.rbegin(); it != list.rend(); ++it) {
            if (it->entity == entity) {
                if (it->serial == serial) target = &*it;
                break;
            }
        }
        if (!target) {
            list.push_back({entity, serial, model, {}});
            target = &list.back();
        }
        if (model > 0) target->model = model;
        target->keys.push_back(std::move(key));
    }

    void parse_weapon_state(Json& json) {
        std::string kind, value, owned_class;
        int entity = -1;
        int weapon = -1;
        int serial = -1;
        float time = 0.0f;
        float tick_time = -1.0f;
        json.object([&](std::string_view key) {
            if (key == "kind") kind = json.string_value();
            else if (key == "e") entity = static_cast<int>(json.number());
            else if (key == "weapon") weapon = static_cast<int>(json.number());
            else if (key == "serial") serial = static_cast<int>(json.number());
            else if (key == "value") value = json.string_value();
            else if (key == "cls") owned_class = json.string_value();
            else if (key == "time") time = static_cast<float>(json.number());
            else if (key == "t") {
                tick_time = static_cast<float>(json.number()) / std::max(data_.tick_rate, 1.0f);
            } else json.skip();
        });
        if (tick_time >= 0.0f && time == 0.0f) time = tick_time;
        if (kind == "active") {
            const auto player = player_for_entity(entity, time);
            if (player == NoPlayer) return;
            const int handle = weapon < 0 ? -1 : weapon_handle(weapon, serial);
            auto& keys = data_.players[player].active_weapon;
            if (keys.empty() || keys.back().handle != handle) keys.push_back({time, weapon, handle, value});
            if (handle >= 0 && !value.empty()) {
                auto& names = data_.weapons[handle].class_name;
                if (names.empty() || names.back().text != value) names.push_back({time, value});
            }
        } else if (kind == "hold") {
            data_.weapons[weapon_handle(entity, serial)].hold_type.push_back({time, value});
        } else if (kind == "attach") {
            auto& keys = data_.weapons[weapon_handle(entity, serial)].attachments;
            if (keys.empty() || keys.back().text != value) keys.push_back({time, value});
        } else if (kind == "shot") {
            data_.weapons[weapon_handle(entity, serial)].shots.push_back(time);
        } else if (kind == "grenade") {
            push_changed(data_.weapons[weapon_handle(entity, serial)].grenade,
                TimedInt{time, std::atoi(value.c_str())});
        } else if (kind == "aim" || kind == "passive" || kind == "reload") {
            auto& state = data_.weapons[weapon_handle(entity, serial)];
            auto& keys = kind == "aim" ? state.aiming : kind == "passive" ? state.passive : state.reloading;
            push_changed(keys, TimedInt{time, std::atof(value.c_str()) > 0.0 ? 1 : 0});
        } else if (kind == "owner") {
            auto& state = data_.weapons[weapon_handle(entity, serial)];
            push_changed(state.owner, TimedInt{time, std::atoi(value.c_str())});
            if (!owned_class.empty()
                && (state.class_name.empty() || state.class_name.back().text != owned_class)) {
                state.class_name.push_back({time, owned_class});
            }
        } else if (kind == "gone") {
            auto& state = data_.weapons[weapon_handle(entity, serial)];
            if (state.gone < 0.0f) state.gone = time;
        }
    }

    void parse_view_model(Json& json) {
        ViewModelKey key;
        int slot = -1, tick = 0, owner = -1;
        json.object([&](std::string_view name) {
            if (name == "t") tick = static_cast<int>(json.number());
            else if (name == "vm") slot = static_cast<int>(json.number());
            else if (name == "owner") owner = static_cast<int>(json.number());
            else if (name == "model") key.model = static_cast<int>(json.number());
            else if (name == "seq") key.sequence = static_cast<int>(json.number());
            else if (name == "rate") key.rate = static_cast<float>(json.number());
            else if (name == "animParity") key.animation_parity = static_cast<int>(json.number());
            else if (name == "newSeq") key.new_sequence = static_cast<int>(json.number());
            else if (name == "weapon") key.weapon = static_cast<int>(json.number());
            else if (name == "effects") key.effects = static_cast<int>(json.number());
            else json.skip();
        });
        if (slot != 0) return;  // slots 1 and 2 are unused by GMod weapons
        key.time = static_cast<float>(tick) / std::max(data_.tick_rate, 1.0f);
        key.owner = owner < 0 || owner == 8388607 ? -1 : owner & ((1 << 13) - 1);
        data_.view_model.push_back(key);
    }

    void parse_clothing(Json& json) {
        DemoClothingEvent event;
        int tick = -1;
        json.object([&](std::string_view key) {
            if (key == "t") {
                tick = static_cast<int>(json.number());
            } else if (key == "name") {
                event.name = json.string_value();
            } else if (key == "keys") {
                json.object([&](std::string_view key_name) {
                    if (key_name == "entity") {
                        event.entity = static_cast<int>(json.number());
                    } else if (key_name == "entry") {
                        event.entry = static_cast<int>(json.number());
                    } else if (key_name == "modelIndex") {
                        event.model_index = static_cast<int>(json.number());
                    } else if (key_name == "skin") {
                        event.skin = scalar_string(json);
                    } else if (key_name == "visible") {
                        event.visible = scalar_string(json);
                    } else {
                        json.skip();
                    }
                });
            } else {
                json.skip();
            }
        });

        const bool known_event = event.name == "perpheads_bonemerge"
            || event.name == "perpheads_bonemerge_entry_visible"
            || event.name == "perpheads_bonemerge_entry_skin"
            || event.name == "perpheads_bonemerge_entry_removed";
        if (!known_event) return;
        if (tick < 0 || event.entity < 0 || event.entry < 0) {
            throw std::runtime_error("Perpheads clothing event is missing tick, entity, or entry.");
        }
        event.time = static_cast<float>(tick) / std::max(data_.tick_rate, 1.0f);
        data_.clothing.push_back(std::move(event));
    }

    static DoorState state_from_name(std::string_view name, bool& known) {
        known = true;
        if (name == "closed") return DoorState::Closed;
        if (name == "opening") return DoorState::Opening;
        if (name == "open") return DoorState::Open;
        if (name == "closing") return DoorState::Closing;
        if (name == "ajar") return DoorState::Ajar;
        known = false;
        return DoorState::Closed;
    }

    void parse_object(Json& json) {
        Object object;
        std::string scratch_class, scratch_model, scratch_state, scratch_type, scratch_kind;
        std::string scratch_name, scratch_lua;
        std::string game_event, game_name, game_steam, game_reason;
        json.object([&](std::string_view key) {
            if (key == "event") {
                game_event = json.string_value();
                return;
            }
            if (key == "keys") {
                json.object([&](std::string_view field) {
                    if (field == "name") game_name = json.string_value();
                    else if (field == "networkid") game_steam = json.string_value();
                    else if (field == "reason") game_reason = json.string_value();
                    else json.skip();
                });
                return;
            }
            switch (key.size()) {
            case 1:
                if (key == "e") { object.entity = static_cast<int>(json.number()); return; }
                if (key == "t") { object.tick = static_cast<int>(json.number()); return; }
                if (key == "x") { object.x = static_cast<float>(json.number()); object.has_position = true; return; }
                if (key == "y") { object.y = static_cast<float>(json.number()); return; }
                if (key == "z") { object.z = static_cast<float>(json.number()); return; }
                break;
            case 3:
                if (key == "yaw") { object.yaw = static_cast<float>(json.number()); object.has_yaw = true; return; }
                break;
            case 4:
                if (key == "time") { object.time = static_cast<float>(json.number()); object.has_time = true; return; }
                if (key == "type") {
                    const auto type = json.string_view_value(scratch_type);
                    object.type = type == "spawn" ? 1 : type == "move" ? 2
                        : type == "remove" ? 3 : type == "door" ? 4
                        : type == "owner" ? 5 : type == "turn" ? 6 : type == "visibility" ? 7 : type == "animation" ? 8 : 0;
                    return;
                }
                if (key == "roll") { object.roll = static_cast<float>(json.number()); return; }
                if (key == "skin") { object.skin = static_cast<int>(json.number()); return; }
                if (key == "body") { object.body = static_cast<int>(json.number()); return; }
                if (key == "kind") {
                    const auto kind = json.string_view_value(scratch_kind);
                    object.vehicle = kind == "vehicle";
                    object.weapon = kind == "weapon";
                    return;
                }
                if (key == "name") { object.name = json.string_view_value(scratch_name); return; }
                break;
            case 5:
                if (key == "cycle") { object.cycle=static_cast<float>(json.number());object.cycle_recorded=true;return; }
                if (key == "pitch") { object.pitch = static_cast<float>(json.number()); return; }
                if (key == "class") { object.cls = json.string_view_value(scratch_class); return; }
                if (key == "model") { object.model = json.string_view_value(scratch_model); return; }
                if (key == "state") { object.state = json.string_view_value(scratch_state); return; }
                if (key == "owner") { object.owner = static_cast<int>(json.number()); return; }
                break;
            case 6:
                if (key == "parent") { object.parent = static_cast<int>(json.number()); return; }
                if (key == "hidden") { object.hidden = json.boolean(); return; }
                break;
            case 8:
                if (key == "sequence") { object.sequence=static_cast<int>(json.number());return; }
                if (key == "luaClass") { object.lua_class = json.string_view_value(scratch_lua); return; }
                break;
            case 9:
                if (key == "mapEntity") { object.map_entity = json.boolean(); return; }
                break;
            case 10:
                if (key == "creationId") { object.creation = static_cast<long long>(json.number()); return; }
                break;
            case 12:
                if (key == "playbackRate") { object.rate=static_cast<float>(json.number());return; }
                if (key == "mapCreatedId") { object.map_created_id = static_cast<int>(json.number()); return; }
                break;
            default: break;
            }
            json.skip();
        });
        if (!game_event.empty() && object.has_time) {
            // Join and leave announcements, as the console prints them.
            DemoLogLine line;
            line.time = object.time;
            line.kind = LogKind::Connection;
            if (game_event == "player_connect_client") {
                line.text = game_name + " (" + game_steam + ") connected";
            } else if (game_event == "player_disconnect") {
                line.text = game_name + " left the game" + (game_reason.empty() ? "" : " (" + game_reason + ")");
            }
            if (!line.text.empty()) data_.log.push_back(std::move(line));
            return;
        }
        handle(object);
    }

    // Tracked entities are kept after removal so a re-entering entity (the engine drops
    // entities leaving the player's view) continues the same track.
    struct TrackMeta {
        long long creation = 0;
        int kind = 0;  // 1 physics or scripted prop, 2 vehicle, 3 seat
        std::string cls;
    };

    // Which track an entity index belonged to over time (indexes are reused), so data keyed
    // by entity (pose parameters, NWVars) lands on the right vehicle.
    struct EntityTrack {
        float time = 0.0f;
        std::size_t track = 0;
    };

    void bind_entity_track(int entity, float time, std::size_t track) {
        auto& list = entity_tracks_[entity];
        if (list.empty() || list.back().track != track) list.push_back({time, track});
    }

    // The prop track for `entity` at `time`; before its first spawn the first track applies.
    DemoProp* track_at(int entity, float time) {
        const auto found = entity_tracks_.find(entity);
        if (found == entity_tracks_.end() || found->second.empty()) return nullptr;
        const EntityTrack* chosen = &found->second.front();
        for (const auto& candidate : found->second) {
            if (candidate.time <= time) chosen = &candidate;
        }
        return &data_.props[chosen->track];
    }

    // Player-placed models and map mover hierarchies; player attachments are separate.
    static bool is_placeable(const Object& object) {
        if (!object.model.empty() && object.model.front() == '*') return true;
        if (object.cls == "CDynamicProp" && object.map_entity && (object.parent > 0 || is_bank_shutter(object.model))
            && object.map_created_id >= 1234 && !object.model.empty()) return true;
        if (object.lua_class=="prop_back_gun" && object.cls=="CSENT_anim" && !object.model.empty()) return true;
        if (object.vehicle || object.map_entity || object.model.empty() || object.parent >= 0) return false;
        if (object.cls != "CPhysicsProp" && object.cls != "CSENT_anim") return false;
        if (object.model.front() == '*' || object.model.ends_with("error.mdl")) return false;
        return object.lua_class != "ent_billboard_proxy";
    }

    void close_track(std::size_t index, float time) {
        auto& keys = data_.props[index].keys;
        if (!keys.empty() && keys.back().present) {
            PropKey key;
            key.time = time;
            key.present = false;
            keys.push_back(key);
        }
    }

    void push_owner(DemoProp& prop, float time, int owner) {
        if (!prop.owners.empty() && prop.owners.back().owner == owner) return;
        prop.owners.push_back({time, owner});
    }

    // Server NPCs (vendors, dealers) are CSENT_anim entities whose Lua class names them.
    // Humans become stationary DemoPlayers (so they get the player body, clothing and idle
    // pose); machines (ATMs, pumps) become static props.
    struct NpcRef {
        bool human = false;
        std::size_t index = 0;
        std::string model;
        float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;
    };

    static bool is_npc_class(std::string_view lua_class) {
        return lua_class.starts_with("npc_") || lua_class.ends_with("_dealer");
    }

    bool handle_npc_spawn(const Object& object, float time) {
        const auto known = npc_by_entity_.find(object.entity);
        const bool declares = !object.model.empty() && is_npc_class(object.lua_class)
            && object.cls == "CSENT_anim";
        if (known != npc_by_entity_.end()
            && (declares ? known->second.model == object.model : object.model.empty())) {
            auto& npc = known->second;
            if (npc.human) {
                auto& presence = data_.players[npc.index].presence;
                if (presence.empty() || !presence.back().present) presence.push_back({time, true});
            } else if (prop_track_.find(object.entity) == prop_track_.end()) {
                PropKey key;
                key.time = time;
                key.has_position = true;
                key.x = npc.x;
                key.y = npc.y;
                key.z = npc.z;
                key.has_yaw = true;
                key.yaw = npc.yaw;
                data_.props[npc.index].keys.push_back(key);
                prop_track_[object.entity] = npc.index;
                dead_tracks_.erase(object.entity);
            }
            return true;
        }
        if (!declares) {
            npc_by_entity_.erase(object.entity);
            return false;
        }
        if (!object.has_position) return false;
        NpcRef npc;
        npc.model = std::string(object.model);
        npc.x = object.x;
        npc.y = object.y;
        npc.z = object.z;
        npc.yaw = object.has_yaw ? object.yaw : 0.0f;
        std::string lowered(object.model);
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        npc.human = lowered.find("/civilian/") != std::string::npos;
        if (npc.human) {
            DemoPlayer player;
            player.npc = true;
            player.entity = object.entity;
            player.name = "NPC";
            player.presence.push_back({time, true});
            player.samples.push_back({time, npc.x, npc.y, npc.z, 0.0f, npc.yaw, 0.0f,
                                      intern_model(object.model), 0, 1});
            npc.index = data_.players.size();
            data_.players.push_back(std::move(player));
        } else {
            DemoProp prop;
            prop.entity = object.entity;
            prop.model = intern_model(object.model);
            PropKey key;
            key.time = time;
            key.has_position = true;
            key.x = npc.x;
            key.y = npc.y;
            key.z = npc.z;
            key.has_yaw = true;
            key.yaw = npc.yaw;
            prop.keys.push_back(key);
            npc.index = data_.props.size();
            data_.props.push_back(std::move(prop));
            track_meta_.push_back({object.creation, 1});
            prop_track_[object.entity] = npc.index;
            dead_tracks_.erase(object.entity);
        }
        npc_by_entity_[object.entity] = std::move(npc);
        return true;
    }

    void handle(const Object& object) {
        if (object.entity < 0 || object.type == 0) return;
        const float time = object.has_time ? object.time
            : object.tick >= 0 ? static_cast<float>(object.tick) / data_.tick_rate : 0.0f;
        const auto prop_it = prop_track_.find(object.entity);
        switch (object.type) {
        case 8:
            if(prop_it!=prop_track_.end()) {
                auto& prop=data_.props[prop_it->second];
                if(prop.model>=0 && is_bank_shutter(data_.models[prop.model]))
                    record_bank_shutter_animation(prop,{time,object.sequence,object.cycle,object.rate},object.cycle_recorded);
            }
            break;
        case 1: {  // spawn
            if (object.map_entity && object.map_created_id >= 1234)
                data_.recorded_map_entities.push_back(object.map_created_id);
            if (handle_npc_spawn(object, time)) break;
            std::size_t parked = static_cast<std::size_t>(-1);
            if (prop_it != prop_track_.end()) {
                parked = prop_it->second;
                close_track(parked, time);
                prop_track_.erase(prop_it);
            } else if (const auto dead = dead_tracks_.find(object.entity); dead != dead_tracks_.end()) {
                parked = dead->second;
            }
            door_track_.erase(object.entity);

            if (object.weapon && object.model.size() > 0) {
                const auto name = !object.name.empty() ? object.name : object.lua_class;
                if (!name.empty()) {
                    data_.weapon_view_models.emplace(std::string(name), std::string(object.model));
                }
            }


            const bool seat = object.vehicle && object.cls == "CPropVehiclePrisonerPod";
            const bool vehicle = object.vehicle && !seat
                && (!object.model.empty() || parked != static_cast<std::size_t>(-1));
            const bool physics = is_placeable(object);
            int kind = seat ? 3 : vehicle ? 2 : physics ? 1 : 0;
            // A prop re-entering view only says its class; the parked track knows the rest.
            if (kind == 0 && !object.vehicle && parked != static_cast<std::size_t>(-1)
                && track_meta_[parked].kind == 1 && track_meta_[parked].cls == object.cls
                && object.model.empty()) {
                kind = 1;
            }
            if (kind != 0) {
                const bool revive = parked != static_cast<std::size_t>(-1)
                    && track_meta_[parked].kind == kind
                    && (object.creation == 0 || track_meta_[parked].creation == 0
                        || object.creation == track_meta_[parked].creation);
                PropKey key;
                key.time = time;
                key.has_position = object.has_position;
                key.x = object.x;
                key.y = object.y;
                key.z = object.z;
                key.parent = object.parent;
                key.has_yaw = object.has_yaw;
                key.yaw = object.yaw;
                key.pitch = object.pitch;
                key.roll = object.roll;
                if (revive) {
                    auto& prop = data_.props[parked];
                    if (!key.has_yaw && !prop.keys.empty()) {
                        // No angles in this packet: they are unchanged from before the entity left.
                        for (auto it = prop.keys.rbegin(); it != prop.keys.rend(); ++it) {
                            if (!it->has_yaw) continue;
                            key.has_yaw = true;
                            key.yaw = it->yaw;
                            key.pitch = it->pitch;
                            key.roll = it->roll;
                            break;
                        }
                    }
                    if(prop.back_gun && key.parent<0 && !prop.keys.empty()) key.parent=prop.keys.back().parent;
                    // A static prop re-entering view often resends no position: it is where it was.
                    if (!key.has_position && kind == 1) {
                        for (auto it = prop.keys.rbegin(); it != prop.keys.rend(); ++it) {
                            if (!it->has_position) continue;
                            key.has_position = true;
                            key.x = it->x;
                            key.y = it->y;
                            key.z = it->z;
                            break;
                        }
                    }
                    if (object.has_position || kind == 1) prop.keys.push_back(key);
                    if (object.owner >= 0) push_owner(prop, time, object.owner);
                    prop_track_[object.entity] = parked;
                    dead_tracks_.erase(object.entity);
                    bind_entity_track(object.entity, time, parked);
                } else if (kind == 3 || !object.model.empty()) {
                    DemoProp prop;
                    prop.entity = object.entity;
                    prop.model = kind == 3 ? -1 : intern_model(object.model);
                    prop.skin = object.skin;
                    prop.body = object.body;
                    prop.vehicle = kind == 2;
                    prop.brush = !object.model.empty() && object.model.front() == '*';
                    prop.map_attached = object.cls == "CDynamicProp" && object.map_entity
                        && (object.parent > 0 || is_bank_shutter(object.model));
                    prop.map_created_id = object.map_created_id;
                    prop.back_gun = object.lua_class=="prop_back_gun";
                    prop.seat = kind == 3;
                    prop.keys.push_back(key);
                    if (object.owner >= 0) prop.owners.push_back({time, object.owner});
                    data_.props.push_back(std::move(prop));
                    track_meta_.push_back({object.creation, kind, std::string(object.cls)});
                    prop_track_[object.entity] = data_.props.size() - 1;
                    dead_tracks_.erase(object.entity);
                    bind_entity_track(object.entity, time, data_.props.size() - 1);
                }
            } else if (object.cls == "CPropDoorRotating") {
                // A door re-entering view continues its old track (same entity, same map entity).
                std::size_t existing = static_cast<std::size_t>(-1);
                for (std::size_t i = data_.doors.size(); i-- > 0;) {
                    const auto& old = data_.doors[i];
                    if (old.entity != object.entity) continue;
                    const bool same_place = !object.has_position || !old.has_origin
                        || (std::fabs(old.origin[0] - object.x) < 1.0f
                            && std::fabs(old.origin[1] - object.y) < 1.0f
                            && std::fabs(old.origin[2] - object.z) < 1.0f);
                    if (same_place && (object.creation == 0 || door_creation_[i] == 0
                                       || door_creation_[i] == object.creation)) {
                        existing = i;
                    }
                    break;
                }
                if (existing != static_cast<std::size_t>(-1)) {
                    door_track_[object.entity] = existing;
                    if (object.has_yaw) {
                        auto& yaws = data_.doors[existing].yaws;
                        const DoorYawKey key{time, object.yaw, object.pitch, object.roll};
                        if (!yaws.empty() && yaws.back().time == time) yaws.back() = key;
                        else yaws.push_back(key);
                    }
                    break;
                }
                DemoDoor door;
                door.entity = object.entity;
                if (object.has_position) {
                    door.has_origin = true;
                    door.origin[0] = object.x;
                    door.origin[1] = object.y;
                    door.origin[2] = object.z;
                }
                if (object.has_yaw) door.yaws.push_back({time, object.yaw, object.pitch, object.roll});
                data_.doors.push_back(std::move(door));
                door_creation_.push_back(object.creation);
                door_track_[object.entity] = data_.doors.size() - 1;
            }
            break;
        }
        case 2:  // move
            if (const auto npc = npc_by_entity_.find(object.entity);
                npc != npc_by_entity_.end() && object.has_position) {
                npc->second.x = object.x;
                npc->second.y = object.y;
                npc->second.z = object.z;
                if (npc->second.human) {
                    data_.players[npc->second.index].samples.push_back(
                        {time, object.x, object.y, object.z, 0.0f, npc->second.yaw, 0.0f,
                         data_.players[npc->second.index].samples.back().model, 0, 1});
                    break;
                }
            }
            if (prop_it != prop_track_.end() && object.has_position) {
                PropKey key;
                key.time = time;
                key.has_position = true;
                key.x = object.x;
                key.y = object.y;
                key.z = object.z;
                const auto& previous = data_.props[prop_it->second].keys;
                key.parent = previous.empty() ? -1 : previous.back().parent;
                if (!previous.empty()) {
                    key.has_yaw = previous.back().has_yaw;
                    key.yaw = previous.back().yaw;
                    key.pitch = previous.back().pitch;
                    key.roll = previous.back().roll;
                }
                data_.props[prop_it->second].keys.push_back(key);
            }
            break;
        case 6:  // turn: new rotation of a vehicle, prop or door
            if (const auto door_it = door_track_.find(object.entity); door_it != door_track_.end()) {
                auto& yaws = data_.doors[door_it->second].yaws;
                const DoorYawKey key{time, object.yaw, object.pitch, object.roll};
                if (!yaws.empty() && yaws.back().time == time) yaws.back() = key;
                else yaws.push_back(key);
                break;
            }
            if (prop_it != prop_track_.end()) {
                auto& keys = data_.props[prop_it->second].keys;
                if (keys.empty() || !keys.back().present) break;
                if (keys.back().time != time) {
                    PropKey copy = keys.back();
                    copy.time = time;
                    copy.has_position = false;
                    keys.push_back(copy);
                }
                auto& key = keys.back();
                key.has_yaw = true;
                key.yaw = object.yaw;
                key.pitch = object.pitch;
                key.roll = object.roll;
            }
            break;
        case 3:  // remove
            if (const auto npc = npc_by_entity_.find(object.entity);
                npc != npc_by_entity_.end() && npc->second.human) {
                auto& presence = data_.players[npc->second.index].presence;
                if (presence.empty() || presence.back().present) presence.push_back({time, false});
                break;
            }
            if (prop_it != prop_track_.end()) {
                close_track(prop_it->second, time);
                dead_tracks_[object.entity] = prop_it->second;
                prop_track_.erase(prop_it);
            }
            door_track_.erase(object.entity);
            break;
        case 5:  // owner / occupant changed
            if (prop_it != prop_track_.end()) {
                push_owner(data_.props[prop_it->second], time, object.owner);
            }
            break;
        case 7:  // recorded model visibility
            if (prop_it != prop_track_.end())
                data_.props[prop_it->second].hidden.push_back({time,object.hidden ? 1 : 0});
            break;
        case 4: {  // door
            if (prop_it != prop_track_.end() && data_.props[prop_it->second].brush)
                data_.props[prop_it->second].hidden.push_back({time,object.hidden ? 1 : 0});
            if (prop_it != prop_track_.end() && data_.props[prop_it->second].brush
                && object.has_position) {
                auto& keys = data_.props[prop_it->second].keys;
                PropKey key;
                for (auto previous = keys.rbegin(); previous != keys.rend(); ++previous)
                    if (previous->has_position) { key = *previous; break; }
                key.time = time;
                key.present = true;
                key.has_position = true;
                key.x = object.x; key.y = object.y; key.z = object.z;
                if (!keys.empty() && keys.back().time == time) keys.back() = key;
                else keys.push_back(key);
                break;
            }
            const auto door_it = door_track_.find(object.entity);
            if (door_it == door_track_.end()) break;
            auto& door = data_.doors[door_it->second];
            if (object.has_position && !door.has_origin) {
                door.has_origin = true;
                door.origin[0] = object.x;
                door.origin[1] = object.y;
                door.origin[2] = object.z;
            }
            bool known = false;
            const auto state = state_from_name(object.state, known);
            if (known && (door.keys.empty() || door.keys.back().state != state)) {
                door.keys.push_back({time, state});
            }
            // Only doors that were ever hidden carry the flag; a shown door has none.
            if (object.hidden != (!door.hidden.empty() && door.hidden.back().hidden)) {
                door.hidden.push_back({time, object.hidden});
            }
            break;
        }
        default: break;
        }
    }

    // The reference parser's older exports carried no vehicle rotation, so orientation then
    // comes from the direction of travel. Source vehicle models face along local +Y, hence
    // the 90 degrees.
    static void derive_heading(DemoProp& prop) {
        constexpr float MinStep = 30.0f;
        constexpr float RadiansToDegrees = 57.29577951308232f;
        float anchor_x = 0.0f, anchor_y = 0.0f;
        bool have_anchor = false;
        bool have_heading = false;
        float heading = 0.0f;
        float first_yaw = 0.0f;
        std::size_t first_with_yaw = prop.keys.size();
        for (std::size_t index = 0; index < prop.keys.size(); ++index) {
            auto& key = prop.keys[index];
            if (!key.present || !key.has_position) {
                have_anchor = false;
                continue;
            }
            if (!have_anchor) {
                anchor_x = key.x;
                anchor_y = key.y;
                have_anchor = true;
            } else {
                const float dx = key.x - anchor_x;
                const float dy = key.y - anchor_y;
                if (dx * dx + dy * dy >= MinStep * MinStep) {
                    heading = std::atan2(dy, dx) * RadiansToDegrees;
                    have_heading = true;
                    anchor_x = key.x;
                    anchor_y = key.y;
                }
            }
            if (have_heading) {
                key.has_yaw = true;
                key.yaw = heading - 90.0f;
                if (first_with_yaw == prop.keys.size()) {
                    first_with_yaw = index;
                    first_yaw = key.yaw;
                }
            }
        }
        for (std::size_t index = 0; index < first_with_yaw; ++index) {
            prop.keys[index].has_yaw = true;
            prop.keys[index].yaw = first_yaw;
        }
    }

    void finish() {
        std::sort(data_.recorded_map_entities.begin(), data_.recorded_map_entities.end());
        data_.recorded_map_entities.erase(
            std::unique(data_.recorded_map_entities.begin(), data_.recorded_map_entities.end()),
            data_.recorded_map_entities.end());
        std::stable_sort(data_.bank_lights.begin(),data_.bank_lights.end(),
            [](const auto& a,const auto& b) { return a.time<b.time; });
        for (auto& event : data_.clothing) {
            if (event.model_index < 0) continue;
            const auto found = data_.model_paths_by_index.find(event.model_index);
            if (found != data_.model_paths_by_index.end()
                && !found->second.starts_with('*')) {
                event.model_path = found->second;
            }
        }
        std::stable_sort(data_.clothing.begin(), data_.clothing.end(),
            [](const DemoClothingEvent& a, const DemoClothingEvent& b) {
                return a.time < b.time;
            });
        for (auto& prop : data_.props) {
            std::stable_sort(prop.back_weapon.begin(),prop.back_weapon.end(),[](const auto& a,const auto& b){return a.time<b.time;});
            std::stable_sort(prop.vehicle_owner.begin(),prop.vehicle_owner.end(),[](const auto& a,const auto& b){return a.time<b.time;});
            std::stable_sort(prop.animations.begin(),prop.animations.end(),[](const auto& a,const auto& b) { return a.time<b.time; });
            std::stable_sort(prop.keys.begin(), prop.keys.end(),
                [](const PropKey& a, const PropKey& b) { return a.time < b.time; });
            std::stable_sort(prop.owners.begin(), prop.owners.end(),
                [](const OwnerKey& a, const OwnerKey& b) { return a.time < b.time; });
            std::stable_sort(prop.poses.begin(), prop.poses.end(),
                [](const PoseKey& a, const PoseKey& b) { return a.time < b.time; });
            if (prop.vehicle || prop.seat) coalesce_vehicle_updates(prop);
            std::stable_sort(prop.colors.begin(), prop.colors.end(),
                [](const ColorKey& a, const ColorKey& b) { return a.time < b.time; });
            std::stable_sort(prop.hidden.begin(), prop.hidden.end(),
                [](const TimedInt& a, const TimedInt& b) { return a.time < b.time; });
            // Vehicles that never networked their rotation fall back to the direction of travel.
            if (prop.vehicle && std::none_of(prop.keys.begin(), prop.keys.end(),
                                             [](const PropKey& key) { return key.has_yaw; })) {
                derive_heading(prop);
            }
        }
        for (auto& door : data_.doors) {
            std::stable_sort(door.keys.begin(), door.keys.end(),
                [](const DoorKey& a, const DoorKey& b) { return a.time < b.time; });
            std::stable_sort(door.yaws.begin(), door.yaws.end(),
                [](const DoorYawKey& a, const DoorYawKey& b) { return a.time < b.time; });
        }
        std::stable_sort(data_.fires.begin(),data_.fires.end(),[](const auto& a,const auto& b){return a.time<b.time;});
        std::stable_sort(data_.effects.begin(), data_.effects.end(),
            [](const DemoEffect& a, const DemoEffect& b) { return a.time < b.time; });
        for (auto& player : data_.players) {
            std::stable_sort(player.attacks.begin(),player.attacks.end(),
                [](const TimedInt& a,const TimedInt& b) { return a.time < b.time; });
            std::stable_sort(player.reloads.begin(),player.reloads.end(),
                [](const TimedInt& a,const TimedInt& b) { return a.time < b.time; });
            std::stable_sort(player.vehicle_links.begin(), player.vehicle_links.end(),
                [](const PlayerVehicleKey& a, const PlayerVehicleKey& b) { return a.time < b.time; });
        }
        for (const auto& ragdoll : data_.ragdolls) {
            const auto path = data_.model_paths_by_index.find(ragdoll.model);
            if (path != data_.model_paths_by_index.end()) intern_model(path->second);
        }
        // Doors whose position was never networked cannot be matched to the map.
        data_.doors.erase(
            std::remove_if(data_.doors.begin(), data_.doors.end(),
                [](const DemoDoor& door) { return !door.has_origin; }),
            data_.doors.end());
        if (data_.duration <= 0.0f) {
            for (const auto& player : data_.players) {
                if (!player.samples.empty()) {
                    data_.duration = std::max(data_.duration, player.samples.back().time);
                }
            }
        }
        // A shot belongs to whoever had that weapon out at the time.
        std::unordered_map<std::string, std::unordered_map<std::string, int>> votes;
        for (auto& [weapon, state] : data_.weapons) {
            const auto by_time = [](const TimedText& a, const TimedText& b) { return a.time < b.time; };
            std::stable_sort(state.class_name.begin(), state.class_name.end(), by_time);
            std::stable_sort(state.hold_type.begin(), state.hold_type.end(), by_time);
            std::stable_sort(state.attachments.begin(), state.attachments.end(), by_time);
            for (auto* keys : {&state.aiming, &state.passive, &state.reloading, &state.grenade, &state.owner}) {
                std::stable_sort(keys->begin(), keys->end(),
                    [](const TimedInt& a, const TimedInt& b) { return a.time < b.time; });
            }
            // The first hold type an entity reports is its class default.
            for (const auto& hold : state.hold_type) {
                if (hold.text.empty()) continue;
                const auto& name = timed_text_at(state.class_name, hold.time);
                if (!name.empty()) ++votes[name][hold.text];
                break;
            }
        }
        for (const auto& [name, counts] : votes) {
            int total = 0;
            for (const auto& [hold, count] : counts) total += count;
            for (const auto& [hold, count] : counts) {
                if (total >= 2 && count * 10 >= total * 8) data_.class_holds[name] = hold;
            }
        }
        std::unordered_map<int, std::vector<std::pair<float, std::size_t>>> holders;
        for (std::size_t index = 0; index < data_.players.size(); ++index) {
            for (const auto& key : data_.players[index].active_weapon) {
                if (key.handle >= 0) holders[key.handle].emplace_back(key.time, index);
            }
        }
        for (auto& [weapon, held_by] : holders) {
            std::stable_sort(held_by.begin(), held_by.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
        }
        for (const auto& [weapon, state] : data_.weapons) {
            const auto found = holders.find(weapon);
            if (found == holders.end()) continue;
            for (const float shot : state.shots) {
                const auto after = std::upper_bound(
                    found->second.begin(), found->second.end(), shot,
                    [](float time, const auto& holder) { return time < holder.first; });
                if (after == found->second.begin()) continue;
                data_.players[std::prev(after)->second].shots.push_back(shot);
            }
        }
        // Perpheads' own gun-sound messages are sent from the shooter's eye position, so the
        // player whose eyes were there fired; demos after the weapon rewrite have only these.
        std::stable_sort(data_.gun_sounds.begin(), data_.gun_sounds.end(),
            [](const DemoData::GunSound& a, const DemoData::GunSound& b) { return a.time < b.time; });
        for (auto& sound : data_.gun_sounds) {
            if (sound.impact) continue;
            float best = 48.0f * 48.0f;
            for (std::size_t index = 0; index < data_.players.size(); ++index) {
                PlayerPose pose;
                if (!sample_player(data_.players[index], sound.time, pose)) continue;
                const float dx = sound.x - pose.x;
                const float dy = sound.y - pose.y;
                const float height = sound.z - pose.z;
                const float flat = dx * dx + dy * dy;
                if (height < 0.0f || height > 96.0f || flat >= best) continue;
                best = flat;
                sound.player = static_cast<int>(index);
            }
            if (sound.player < 0) continue;
            auto& shots = data_.players[static_cast<std::size_t>(sound.player)].shots;
            const bool known = std::any_of(shots.begin(), shots.end(),
                [&](float shot) { return std::fabs(shot - sound.time) < 0.05f; });
            if (!known) shots.push_back(sound.time);
        }
        for (auto& player : data_.players) std::sort(player.shots.begin(), player.shots.end());
    }

    DemoData& data_;
    std::unordered_map<std::string, int> model_ids_;
    std::unordered_map<int, NpcRef> npc_by_entity_;
    std::unordered_map<int, std::size_t> prop_track_;
    std::unordered_map<int, std::size_t> dead_tracks_;
    std::unordered_map<int, std::vector<EntityTrack>> entity_tracks_;
    std::unordered_map<std::size_t, std::array<float, VehiclePoseCount>> pose_state_;
    std::vector<TrackMeta> track_meta_;
    std::unordered_map<int, std::size_t> door_track_;
    std::vector<long long> door_creation_;  // creation id of each entry of data_.doors
    std::uint64_t counter_ = 0;
};

// ---- gmoddem process --------------------------------------------------------------

std::wstring quote(const std::wstring& value) { return L"\"" + value + L"\""; }

// Finds the last "NN%" in a chunk of parser output.
bool find_percent(const std::string& text, float& percent) {
    for (std::size_t i = text.size(); i-- > 0;) {
        if (text[i] != '%') continue;
        std::size_t end = i;
        std::size_t start = end;
        while (start > 0 && (std::isdigit(static_cast<unsigned char>(text[start - 1])) || text[start - 1] == '.')) {
            --start;
        }
        if (start < end) {
            percent = std::strtof(text.substr(start, end - start).c_str(), nullptr);
            return true;
        }
    }
    return false;
}

void run_parser(
    const std::filesystem::path& parser_exe,
    const std::filesystem::path& demo,
    const std::filesystem::path& output,
    const ProgressFn& progress,
    const std::atomic<bool>& cancel,
    const std::filesystem::path& perp_data_cache) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &security, 0)) {
        throw std::runtime_error("Could not create a pipe for the demo parser.");
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    std::wstring command = quote(parser_exe.wstring()) + L" json " + quote(demo.wstring())
        + L" -o " + quote(output.wstring()) + L" --no-gluapack --compact";
    if (!perp_data_cache.empty()) command += L" --perp-data " + quote(perp_data_cache.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = write_end;
    startup.hStdInput = nullptr;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(
        nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &process);
    CloseHandle(write_end);
    if (!started) {
        CloseHandle(read_end);
        throw std::runtime_error("Could not start gmoddem.exe (" + parser_exe.string() + ").");
    }

    std::string tail;
    char buffer[4096];
    bool exited = false;
    while (true) {
        if (cancel) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 2000);
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
            CloseHandle(read_end);
            throw std::runtime_error("cancelled");
        }
        DWORD available = 0;
        if (PeekNamedPipe(read_end, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD got = 0;
            if (ReadFile(read_end, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) && got > 0) {
                std::string chunk(buffer, got);
                float percent = 0.0f;
                if (find_percent(chunk, percent)) {
                    progress(percent * 0.85f, "Parsing demo (gmoddem)");
                }
                tail += chunk;
                if (tail.size() > 2000) tail.erase(0, tail.size() - 2000);
            }
            continue;
        }
        if (exited) break;
        exited = WaitForSingleObject(process.hProcess, 40) == WAIT_OBJECT_0;
    }
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(read_end);
    if (exit_code != 0 || !std::filesystem::exists(output)) {
        std::string message = "gmoddem failed (exit code " + std::to_string(exit_code) + ").";
        std::string cleaned;
        for (const char c : tail) cleaned += (c == '\r' ? '\n' : c);
        const auto last = cleaned.find_last_not_of(" \r\n");
        if (last != std::string::npos) {
            const auto line_start = cleaned.find_last_of('\n', last);
            message += " " + cleaned.substr(line_start == std::string::npos ? 0 : line_start + 1,
                                              last - (line_start == std::string::npos ? 0 : line_start));
        }
        throw std::runtime_error(message);
    }
    constexpr auto recovery_marker = "DEMO_TAIL_RECOVERED:";
    if (const auto recovery = tail.find(recovery_marker); recovery != std::string::npos) {
        auto message = tail.substr(recovery + sizeof(recovery_marker) - 1);
        const auto end = message.find_first_of("\r\n");
        if (end != std::string::npos) message.resize(end);
        progress(100.0f, "Recovered incomplete demo tail: " + message);
    }
}

// The recording client's camera sits at the eyes of the player it shows, so the
// player whose position keeps matching the recorded camera is the recorder.
int find_recorder(const DemoData& data) {
    if (data.recorder_camera.empty() || data.tick_rate <= 0.0f) return -1;
    std::vector<int> matches(data.players.size(), 0);
    for (std::size_t sample = 0; sample < data.recorder_camera.size(); sample += 8) {
        const auto& view = data.recorder_camera[sample];
        if (!view.origin_valid) continue;
        for (std::size_t index = 0; index < data.players.size(); ++index) {
            if (data.players[index].npc) continue;
            PlayerPose pose;
            if (!sample_player(data.players[index], view.time, pose)) continue;
            const float dx = pose.x - view.x;
            const float dy = pose.y - view.y;
            const float dz = pose.z + 64.0f - view.z;
            if (dx * dx + dy * dy + dz * dz < 90.0f * 90.0f) ++matches[index];
        }
    }
    const auto best = std::max_element(matches.begin(), matches.end());
    if (best == matches.end() || *best < 10) return -1;
    return static_cast<int>(best - matches.begin());
}

// Entity ticks are server ticks, but the reference parser converts them with the (wrong)
// rate implied by the demo header. Multiplying every time it produced by `scale` puts them
// on the server clock.
void rescale_times(DemoData& data, float scale) {
    for (auto& light:data.bank_lights) light.time*=scale;
    if (scale == 1.0f) return;
    for (auto& fire : data.fires) fire.time *= scale;
    for (auto& effect : data.effects) effect.time *= scale;
    for (auto& player : data.players) {
        player.connected_at *= scale;
        if (player.disconnected_at >= 0.0f) player.disconnected_at *= scale;
        for (auto& sample : player.samples) sample.time *= scale;
        for (auto& look : player.look) look.time *= scale;
        for (auto& event : player.presence) event.time *= scale;
        for (auto& event : player.seated) event.time *= scale;
        for (auto& key : player.info) key.time *= scale;
        for (auto& key : player.vehicle_links) key.time *= scale;
    }
    for (auto& prop : data.props) {
        for (auto& key : prop.keys) key.time *= scale;
        for (auto& key : prop.owners) key.time *= scale;
        for (auto& key : prop.poses) key.time *= scale;
        for (auto& key : prop.colors) key.time *= scale;
        for (auto& key : prop.animations) key.time *= scale;
        for (auto& key : prop.back_weapon) key.time *= scale;
        for (auto& key : prop.vehicle_owner) key.time *= scale;
        for (auto& key : prop.hidden) key.time *= scale;
        for (auto* keys : {&prop.vehicle_id, &prop.registration, &prop.plate_skin})
            for (auto& key : *keys) key.time *= scale;
    }
    for (auto& door : data.doors) {
        for (auto& key : door.keys) key.time *= scale;
        for (auto& key : door.yaws) key.time *= scale;
        for (auto& key : door.hidden) key.time *= scale;
    }
    for (auto& event : data.clothing) event.time *= scale;
    for (auto& clip : data.voice) clip.time *= scale;
    for (auto& player : data.players) {
        for (auto* keys : {&player.first_name, &player.last_name}) {
            for (auto& key : *keys) key.time *= scale;
        }
        for (auto* keys : {&player.job_team, &player.police_rank, &player.medic_rank,
                           &player.firefighter_rank, &player.outfit, &player.ragdoll,
                           &player.radio_channel, &player.restrained, &player.phone_open,
                           &player.phone_call}) {
            for (auto& key : *keys) key.time *= scale;
        }
        for (auto& key : player.emotes) key.time *= scale;
        for (auto& key : player.active_weapon) key.time *= scale;
        for (auto& shot : player.shots) shot *= scale;
        for (auto& attack : player.attacks) attack.time *= scale;
        for (auto& reload : player.reloads) reload.time *= scale;
    }
    for (auto& [weapon, state] : data.weapons) {
        for (auto& key : state.class_name) key.time *= scale;
        for (auto& key : state.hold_type) key.time *= scale;
        for (auto& key : state.attachments) key.time *= scale;
        for (auto& shot : state.shots) shot *= scale;
        for (auto* keys : {&state.aiming, &state.passive, &state.reloading, &state.grenade, &state.owner}) {
            for (auto& key : *keys) key.time *= scale;
        }
        if (state.gone >= 0.0f) state.gone *= scale;
    }
    for (auto& key : data.view_model) key.time *= scale;
    for (auto& ragdoll : data.ragdolls) {
        for (auto& key : ragdoll.keys) key.time *= scale;
    }
    for (auto& sync : data.radio_sync) sync.time *= scale;
    for (auto& use : data.npc_uses) use.time *= scale;
    for (auto& notice : data.npc_notices) notice.time *= scale;
    for (auto& page : data.npc_gui) page.time *= scale;
    for (auto& line : data.log) line.time *= scale;
    for (auto& sound : data.sounds) sound.time *= scale;
    for (auto& sound : data.gun_sounds) sound.time *= scale;
}

const OwnerKey* occupant_at(const DemoProp& prop, float time) {
    const auto it = std::upper_bound(
        prop.owners.begin(), prop.owners.end(), time,
        [](float value, const OwnerKey& key) { return value < key.time; });
    return it == prop.owners.begin() ? nullptr : &*(it - 1);
}

const PlayerVehicleKey* player_vehicle_at(const DemoPlayer& player, float time) {
    const auto it = std::upper_bound(
        player.vehicle_links.begin(), player.vehicle_links.end(), time,
        [](float value, const PlayerVehicleKey& key) { return value < key.time; });
    return it == player.vehicle_links.begin() ? nullptr : &*(it - 1);
}

// While a player is parented to a vehicle the engine reports their origin as (0, 0, 0).
// Their real position is the vehicle's, so seated intervals are filled in with samples
// that follow the vehicle (or the seat's parent vehicle for passengers).
void bind_seats(DemoData& data) {
    constexpr float Slack = 0.5f;
    // Source vehicle models face along local +Y, so local x is to the right of travel.
    constexpr float DriverRight = -14.0f;
    constexpr float DriverForward = 6.0f;
    constexpr float SeatHeight = 28.0f;
    int next_segment = 1'000'000;

    for (auto& player : data.players) {
        if (player.entity < 0 || player.seated.empty()) continue;
        std::vector<PlayerSample> added;
        for (std::size_t event = 0; event < player.seated.size(); ++event) {
            if (!player.seated[event].present) continue;
            const float begin = player.seated[event].time;
            float end = event + 1 < player.seated.size()
                ? player.seated[event + 1].time : data.duration;
            for (const auto& link : player.vehicle_links) {
                if (link.time > begin && link.vehicle < 0 && link.parent < 0) {
                    end = std::min(end, link.time);
                    break;
                }
            }
            if (end <= begin) continue;

            const PlayerVehicleKey* link = player_vehicle_at(player, begin + Slack);
            if (!link || (link->vehicle < 0 && link->parent < 0)) {
                const auto next_link = std::find_if(
                    player.vehicle_links.begin(), player.vehicle_links.end(),
                    [&](const PlayerVehicleKey& key) {
                        return key.time >= begin && key.time < end
                            && (key.vehicle >= 0 || key.parent >= 0);
                    });
                link = next_link == player.vehicle_links.end() ? nullptr : &*next_link;
            }
            const float binding_begin = link ? std::max(begin, link->time) : begin;
            if (end <= binding_begin) continue;

            // Preserve seat-owner records when available; direct links fill missing metadata.
            const DemoProp* seat = nullptr;
            float seat_age = 1e30f;
            for (const auto& prop : data.props) {
                if (!prop.vehicle && !prop.seat) continue;
                const auto* key = occupant_at(prop, begin + Slack);
                if (!key || key->owner != player.entity) continue;
                const float age = std::fabs(begin - key->time);
                if (age < seat_age) {
                    seat = &prop;
                    seat_age = age;
                }
            }
            const DemoProp* linked_seat = nullptr;
            const DemoProp* linked_car = nullptr;
            if (link) {
                for (const auto& prop : data.props) {
                    if (prop.entity == link->parent && (prop.seat || prop.vehicle)) {
                        linked_seat = &prop;
                    }
                    if (prop.entity == link->vehicle && prop.vehicle) {
                        linked_car = &prop;
                    }
                }
                if (!seat) seat = linked_seat ? linked_seat : linked_car;
            }
            if (!seat) continue;

            const DemoProp* car = seat->vehicle ? seat : linked_car;
            float right = DriverRight, forward = DriverForward;
            bool driver = seat->vehicle;
            if (seat->seat) {
                // A passenger seat is parented to its vehicle; its keys hold local offsets.
                int parent = -1;
                const PropKey* local = nullptr;
                for (const auto& key : seat->keys) {
                    if (key.time > binding_begin + Slack) break;
                    if (key.present && key.has_position) local = &key;
                    if (key.present) parent = key.parent;
                }
                if (!local) continue;
                if (parent >= 0) {
                    for (const auto& prop : data.props) {
                        PropPose pose;
                        if (prop.vehicle && prop.entity == parent && sample_prop(prop, begin, pose)) {
                            car = &prop;
                            break;
                        }
                    }
                    if (!car) car = linked_car;
                    if (!car) continue;
                    right = local->x;
                    forward = local->y;
                }
            }

            const int model = [&] {
                int found = -1;
                for (const auto& sample : player.samples) {
                    if (sample.time > binding_begin) break;
                    found = sample.model;
                }
                return found;
            }();
            // A free-standing seat has no rotation data; keep the way the player was facing.
            const float facing = [&] {
                float found = 0.0f;
                for (const auto& sample : player.samples) {
                    if (sample.time > binding_begin) break;
                    found = sample.yaw;
                }
                return found;
            }();
            const int segment = next_segment++;
            auto add_sample = [&](float time) {
                PropPose pose;
                constexpr float DegreesToRadians = 0.017453292519943295f;
                PlayerSample sample;
                sample.time = time;
                sample.model = model;
                sample.segment = segment;
                sample.flags = 1 | PlayerFlagSeated | (driver ? PlayerFlagDriver : 0)
                    | (car ? PlayerFlagInCar : 0);
                if (car) {
                    if (!sample_prop(*car, time, pose)) return;
                    if (driver && car->driver_attachment) {
                        const auto& attachment = *car->driver_attachment;
                        const float origin[3]{pose.x,pose.y,pose.z};
                        const float angles[3]{pose.pitch,pose.yaw,pose.roll};
                        const auto matrix = source::model::matrix_from_source_angles(origin,angles);
                        sample.x = matrix[0]*attachment[3] + matrix[1]*attachment[7]
                            + matrix[2]*attachment[11] + matrix[3];
                        sample.y = matrix[4]*attachment[3] + matrix[5]*attachment[7]
                            + matrix[6]*attachment[11] + matrix[7];
                        sample.z = matrix[8]*attachment[3] + matrix[9]*attachment[7]
                            + matrix[10]*attachment[11] + matrix[11];
                        const float fx = matrix[0]*attachment[0] + matrix[1]*attachment[4]
                            + matrix[2]*attachment[8];
                        const float fy = matrix[4]*attachment[0] + matrix[5]*attachment[4]
                            + matrix[6]*attachment[8];
                        sample.yaw = std::atan2(fy,fx) / DegreesToRadians;
                        added.push_back(sample);
                        return;
                    }
                    const float yaw = pose.yaw * DegreesToRadians;
                    const float rx = std::cos(yaw), ry = std::sin(yaw);
                    const float fx = -ry, fy = rx;
                    sample.x = pose.x + right * rx + forward * fx;
                    sample.y = pose.y + right * ry + forward * fy;
                    sample.yaw = pose.yaw + 90.0f;
                } else {
                    if (!sample_prop(*seat, time, pose)) return;
                    sample.x = pose.x;
                    sample.y = pose.y;
                    sample.yaw = facing;
                }
                sample.z = pose.z + SeatHeight;
                added.push_back(sample);
            };
            const DemoProp& moving = car ? *car : *seat;
            add_sample(binding_begin);
            for (const auto& key : moving.keys) {
                if (key.time > binding_begin && key.time < end && key.present && (key.has_position || key.has_yaw)) {
                    add_sample(key.time);
                }
            }
            add_sample(end);
        }
        if (added.empty()) continue;
        player.samples.insert(player.samples.end(), added.begin(), added.end());
        std::stable_sort(player.samples.begin(), player.samples.end(),
            [](const PlayerSample& a, const PlayerSample& b) { return a.time < b.time; });
    }
}

// The rest of the demo's `demo.audio` sidecars are decoded voice clips; keep them for
// playback and drop stale directories left behind by earlier sessions.
void keep_voice_clips(DemoData& data, const std::filesystem::path& source_directory) {
    const auto temp = std::filesystem::temp_directory_path();
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(temp, error)) {
        const auto name = entry.path().filename().string();
        if (name.rfind("gmod-replay-voice-", 0) != 0) continue;
        // Cached recordings in a long-running session still own these directories.
        if (name.starts_with("gmod-replay-voice-" + std::to_string(GetCurrentProcessId()) + "-")) continue;
        std::error_code time_error;
        const auto age = std::filesystem::file_time_type::clock::now()
            - entry.last_write_time(time_error);
        if (!time_error && age > std::chrono::hours(24)) {
            std::filesystem::remove_all(entry.path(), time_error);
        }
    }
    if (data.voice.empty() || !std::filesystem::is_directory(source_directory)) {
        data.voice.clear();
        return;
    }
    const auto target = temp / ("gmod-replay-voice-" + std::to_string(GetCurrentProcessId()) + "-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::rename(source_directory, target, error);
    if (error) {
        data.voice.clear();
        return;
    }
    data.voice_store = std::make_shared<VoiceStore>();
    data.voice_store->directory = target;
    for (auto& clip : data.voice) clip.file = target / clip.file.filename();
    std::stable_sort(data.voice.begin(), data.voice.end(),
        [](const DemoVoiceClip& a, const DemoVoiceClip& b) { return a.time < b.time; });
}

} // namespaceconst std::filesystem::path& application_root) {
std::filesystem::path find_parser(const std::filesystem::path& application_root) {
    const std::filesystem::path candidates[] = {
        application_root / L"tools" / L"gmoddem.exe",
        application_root.parent_path() / L"gmod-demo-parser" / L"dist" / L"gmoddem.exe"};
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate)) return candidate;
    }
    throw std::runtime_error(
        "gmoddem.exe was not found. Place it in " + (application_root / L"tools").string() + ".");
}

void coalesce_vehicle_updates(DemoProp& prop) {
    std::size_t count=0;
    for (const auto key : prop.keys) {
        if (count && prop.keys[count-1].time==key.time) {
            auto& frame=prop.keys[count-1];
            frame.present=key.present;
            frame.parent=key.parent;
            if (key.has_position) {
                frame.has_position=true;
                frame.x=key.x; frame.y=key.y; frame.z=key.z;
            }
            if (key.has_yaw) {
                frame.has_yaw=true;
                frame.pitch=key.pitch; frame.yaw=key.yaw; frame.roll=key.roll;
            }
        } else prop.keys[count++]=key;
    }
    prop.keys.resize(count);
    count=0;
    for (const auto key : prop.poses) {
        if (count && prop.poses[count-1].time==key.time) prop.poses[count-1]=key;
        else prop.poses[count++]=key;
    }
    prop.poses.resize(count);
}

const NpcGuiPage* npc_gui_at(const DemoData& data, float time) {
    const auto end=std::upper_bound(data.npc_gui.begin(),data.npc_gui.end(),time,
        [](float value,const NpcGuiPage& page) { return value<page.time; });
    return end==data.npc_gui.begin() ? nullptr : &*std::prev(end);
}

DemoData load_demo(
    const std::filesystem::path& demo_path,
    const std::filesystem::path& parser_exe,
    const ProgressFn& progress,
    const std::atomic<bool>& cancel,
    const std::filesystem::path& perp_data_cache) {
    if (!std::filesystem::exists(demo_path)) {
        throw std::runtime_error("Demo file not found: " + demo_path.string());
    }
    const auto work = std::filesystem::temp_directory_path()
        / ("gmod-replay-" + std::to_string(GetCurrentProcessId()) + "-"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(work);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{work};

    const auto json_path = work / L"demo.json";
    progress(0.0f, "Reading the demo container in-app");
    const auto source_scan = scan_source_demo(demo_path, cancel);
    if (source_scan.packet_count == 0) {
        throw std::runtime_error("The demo container has no signon or packet records.");
    }
    progress(1.0f, source_scan.tail_recovery_message.empty()
        ? "In-app demo container scan complete"
        : "Recovered incomplete demo tail: " + source_scan.tail_recovery_message);

    progress(1.0f, "Parsing entity state (gmoddem reference)");
    run_parser(parser_exe, demo_path, json_path, progress, cancel, perp_data_cache);

    progress(85.0f, "Reading demo data");
    std::string text;
    {
        std::ifstream file(json_path, std::ios::binary | std::ios::ate);
        if (!file) throw std::runtime_error("The parser output could not be opened.");
        const auto length = static_cast<std::size_t>(file.tellg());
        text.resize(length);
        file.seekg(0);
        file.read(text.data(), static_cast<std::streamsize>(length));
    }

    DemoData data;
    data.path = demo_path;
    if (!perp_data_cache.empty()) {
        // Line 1: the matching data folder (empty when none); line 2: why.
        std::ifstream pointer(work / L"demo.perp.txt", std::ios::binary);
        std::string folder;
        if (pointer && std::getline(pointer, folder)) {
            std::getline(pointer, data.perp_data_note);
            if (!folder.empty() && folder.back() == '\r') folder.pop_back();
            if (!folder.empty()) {
                std::wstring wide(folder.size(), L'\0');
                wide.resize(static_cast<std::size_t>(MultiByteToWideChar(
                    CP_UTF8, 0, folder.data(), static_cast<int>(folder.size()), wide.data(),
                    static_cast<int>(wide.size()))));
                data.perp_data = wide;
            }
        } else {
            data.perp_data_note = "the parser did not report PERP data";
        }
    }
    Builder builder(data);
    Json json(text.data(), text.data() + text.size());
    builder.parse(json, progress, cancel);
    if (!source_scan.header.map_name.empty()) {
        data.map_name = source_scan.header.map_name;
    }
    if (!source_scan.header.client_name.empty()) {
        data.client_name = source_scan.header.client_name;
    }
    // The reference parser timed entity ticks with the rate implied by the demo header
    // (typically 66.67/s). The server's real interval (svc_ServerInfo, typically 0.04 s) is
    // what those ticks count in, so every time is stretched onto the server clock.
    if (source_scan.tick_interval > 0.0f) {
        const float parser_rate = data.tick_rate;
        data.tick_rate = 1.0f / source_scan.tick_interval;
        rescale_times(data, parser_rate / data.tick_rate);
    } else if (source_scan.header.playback_ticks > 0 && source_scan.header.playback_time > 0.0f) {
        data.tick_rate = static_cast<float>(source_scan.header.playback_ticks)
            / source_scan.header.playback_time;
    }
    data.duration = effective_demo_duration(source_scan, data.tick_rate);
    if (source_scan.header.playback_time > 0.0f
        && std::fabs(data.duration - source_scan.header.playback_time) > 1.0f) {
        progress(92.0f, "Corrected demo duration from recorded content: "
            + std::to_string(data.duration) + " seconds");
    }
    progress(93.0f, "Placing vehicle passengers");
    bind_seats(data);
    keep_voice_clips(data, work / L"demo.audio");
    progress(95.0f, "Finding the recording player");
    data.recorder_camera.reserve(source_scan.camera.size());
    for (const auto& view : source_scan.camera) {
        // Packets before the first real frame carry an all-zero view; later zero origins
        // (riding a vehicle) keep their angles but have no position.
        const bool zero_origin = view.x == 0.0f && view.y == 0.0f && view.z == 0.0f;
        if (zero_origin && data.recorder_camera.empty()) continue;
        data.recorder_camera.push_back(
            {static_cast<float>(view.tick) / data.tick_rate, view.x, view.y, view.z,
             view.pitch, view.yaw, !zero_origin});
    }
    data.recorder_player = find_recorder(data);
    progress(100.0f, "Demo ready");
    return data;
}

void rebind_vehicle_seats(DemoData& data) {
    for (auto& player : data.players) {
        std::erase_if(player.samples,[](const PlayerSample& sample) {
            return (sample.flags & PlayerFlagSeated) != 0;
        });
    }
    bind_seats(data);
}

// ---- Sampling ----------------------------------------------------------------------

float decal_opacity(float event_time, float playback_time) {
    const float age = playback_time - event_time;
    if (age < 0.0f || age >= DecalLifetime) return 0.0f;
    return std::clamp((DecalLifetime - age) / DecalFadeDuration, 0.0f, 1.0f);
}

BloodMistParticle blood_mist_particle(const DemoEffect& effect, int particle, float playback_time) {
    BloodMistParticle result;
    const float age = playback_time - effect.time;
    if (age < 0.0f || age >= BloodMistLifetime) return result;
    const float phase = effect.time * 1.37f + static_cast<float>(particle) * 2.399963f;
    const float speed = 12.0f + static_cast<float>(particle % 4) * 6.0f;
    const float scale = effect.scale > 0 ? std::clamp(effect.scale,0.25f,2.0f) : 1.0f;
    result.position = {effect.origin[0] + (std::cos(phase) * speed + effect.normal[0] * 30.0f) * age * scale,
        effect.origin[1] + (std::sin(phase) * speed + effect.normal[1] * 30.0f) * age * scale,
        effect.origin[2] + (12.0f + static_cast<float>(particle % 3) * 5.0f + effect.normal[2] * 30.0f) * age * scale
            - 25.0f * age * age};
    result.radius = 2.5f * (3.0f + static_cast<float>(particle % 3) + 12.0f * age) * scale;
    result.opacity = 0.45f * (1.0f - age / BloodMistLifetime);
    return result;
}

std::vector<float> recorded_shot_times(const DemoData& demo, int player,
                                      const std::vector<float>& view_model_shots) {
    const auto& primary = demo.players.at(static_cast<std::size_t>(player)).shots;
    auto result = primary;
    result.erase(std::unique(result.begin(),result.end()),result.end());
    const auto add = [&](float time) {
        const auto next = std::lower_bound(result.begin(),result.end(),time-0.05f);
        if (next == result.end() || *next > time+0.05f)
            result.insert(std::upper_bound(result.begin(),result.end(),time),time);
    };
    // Player shots already include matched server gun sounds from the builder.
    if (player == demo.recorder_player)
        for (const float time : view_model_shots) add(time);
    return result;
}

bool surface_smoke_effect(const DemoEffect& effect) {
    if (effect.kind != "dispatch"
        || (effect.name != "gdcw_universal_impact" && effect.name != "gdcw_universal_penetrate"))
        return false;
    const float material = std::ceil(effect.material);
    // Flesh/alien blood and water impacts are not grey surface dust.
    return std::isfinite(material) && material != 45 && material != 65 && material != 66
        && material != 70 && material != 72 && material != 83;
}

SmokeParticle smoke_particle(const DemoEffect& effect, int particle, float playback_time, bool muzzle) {
    SmokeParticle result;
    const float age = playback_time - effect.time;
    if (!std::isfinite(age) || age < 0 || age >= SmokeLifetime || effect.scale <= 0
        || !std::isfinite(effect.scale)) return result;
    const float scale = std::clamp(effect.scale,0.1f,2.0f);
    const float phase = effect.time*1.37f + static_cast<float>(particle)*2.399963f;
    const auto& n = effect.normal;
    const float length = std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
    if (!std::isfinite(length) || length < 0.001f) return result;
    const std::array<float,3> normal{n[0]/length,n[1]/length,n[2]/length};
    const std::array<float,3> axis = std::fabs(normal[2]) < 0.9f
        ? std::array<float,3>{0,0,1} : std::array<float,3>{1,0,0};
    std::array<float,3> side{normal[1]*axis[2]-normal[2]*axis[1],
        normal[2]*axis[0]-normal[0]*axis[2],normal[0]*axis[1]-normal[1]*axis[0]};
    const float side_length = std::sqrt(side[0]*side[0]+side[1]*side[1]+side[2]*side[2]);
    for (auto& value : side) value /= side_length;
    const std::array<float,3> up{normal[1]*side[2]-normal[2]*side[1],
        normal[2]*side[0]-normal[0]*side[2],normal[0]*side[1]-normal[1]*side[0]};
    // Integrate drag analytically so a seek produces the same puff as continuous playback.
    const float travel = (1.0f-std::exp(-5.0f*age))/5.0f;
    for (int i=0;i<3;++i) {
        if (!std::isfinite(effect.origin[i])) return {};
        const float spread = (side[i]*std::cos(phase)+up[i]*std::sin(phase))*(muzzle ? 8.0f : 18.0f);
        const float speed = muzzle ? 55.0f+particle*28.0f : 35.0f+particle*12.0f;
        result.position[i] = effect.origin[i]+normal[i]*0.6f
            +(normal[i]*speed+spread)*travel*scale;
    }
    result.position[2] += (muzzle ? 12.0f : 5.0f)*age*age;
    result.radius = ((muzzle ? 2.0f : 3.0f)+particle*0.6f+age*(muzzle ? 13.0f : 20.0f))*scale;
    result.opacity = (muzzle ? 0.2f : 0.4f)*(1.0f-age/SmokeLifetime);
    result.color = muzzle ? std::array<float,3>{0.55f,0.55f,0.55f}
        : effect.material == 68 || effect.material == 78 || effect.material == 87
            ? std::array<float,3>{0.47f,0.42f,0.34f} : std::array<float,3>{0.55f,0.53f,0.50f};
    return result;
}

const PlayerInfoKey* player_info_at(const DemoPlayer& player, float time) {
    const auto it = std::upper_bound(
        player.info.begin(), player.info.end(), time,
        [](float value, const PlayerInfoKey& key) { return value < key.time; });
    return it == player.info.begin() ? nullptr : &*(it - 1);
}

template <typename Key>
static const Key* key_at(const std::vector<Key>& keys, float time) {
    const auto it = std::upper_bound(
        keys.begin(), keys.end(), time,
        [](float value, const Key& key) { return value < key.time; });
    return it == keys.begin() ? nullptr : &*(it - 1);
}

const EmoteKey* emote_at(const DemoPlayer& player, float time) {
    const auto& keys = player.emotes;
    const auto it = std::upper_bound(keys.begin(), keys.end(), time,
        [](float value, const EmoteKey& key) { return value < key.time; });
    if (it == keys.begin() || (it - 1)->sequence < 0) return nullptr;
    return &*(it - 1);
}

int timed_int_at(const std::vector<TimedInt>& keys, float time, int fallback) {
    const auto* key = key_at(keys, time);
    return key ? key->value : fallback;
}

const std::string& timed_text_at(const std::vector<TimedText>& keys, float time) {
    static const std::string empty;
    const auto* key = key_at(keys, time);
    return key ? key->text : empty;
}

std::string character_name_at(const DemoPlayer& player, float time) {
    const auto& first = timed_text_at(player.first_name, time);
    const auto& last = timed_text_at(player.last_name, time);
    if (first.empty()) return last;
    return last.empty() ? first : first + " " + last;
}

HeldWeapon held_weapon_at(const DemoData& demo, const DemoPlayer& player, float time) {
    HeldWeapon held;
    const auto* active = key_at(player.active_weapon, time);
    if (!active || active->handle < 0) return held;
    held.entity = active->weapon;
    held.handle = active->handle;
    held.class_name = active->class_name;
    const auto found = demo.weapons.find(active->handle);
    if (found != demo.weapons.end()) {
        if (found->second.gone >= 0.0f && found->second.gone <= time) return {};
        const auto* owner = key_at(found->second.owner,time);
        if (owner && owner->value != player.entity) return {};
        if (held.class_name.empty()) {
            held.class_name = timed_text_at(found->second.class_name, time);
            // A handle includes the serial: learning its immutable class later is safe,
            // unlike guessing from an entity index that can be reused.
            if (held.class_name.empty() && !found->second.class_name.empty())
                held.class_name = found->second.class_name.front().text;
        }
        held.hold_type = timed_text_at(found->second.hold_type, time);
    }
    // No class name fallback from the player's own weapon field: it resolves through the
    // entity index alone and names stale weapons when an index is reused.
    if (held.hold_type.empty() && !held.class_name.empty()) {
        const auto holds = demo.class_holds.find(held.class_name);
        if (holds != demo.class_holds.end()) {
            held.hold_type = holds->second;
            held.hold_from_class = true;
        }
    }
    return held;
}

bool fired_recently(const DemoPlayer& player, float time, float window) {
    const auto it = std::upper_bound(player.shots.begin(), player.shots.end(), time);
    return it != player.shots.begin() && time - *(it - 1) <= window;
}

const TimedInt* grenade_state_at(const DemoData& demo, int handle, float time) {
    const auto found = demo.weapons.find(handle);
    if (found == demo.weapons.end() || (found->second.gone >= 0.0f && found->second.gone <= time))
        return nullptr;
    return key_at(found->second.grenade, time);
}

WeaponFlags weapon_flags_at(const DemoData& demo, int handle, float time) {
    WeaponFlags flags;
    const auto found = demo.weapons.find(handle);
    if (found == demo.weapons.end()) return flags;
    const auto& state = found->second;
    const auto latest = [time](const std::vector<TimedInt>& keys) -> const TimedInt* {
        const auto it = std::upper_bound(keys.begin(), keys.end(), time,
            [](float value, const TimedInt& key) { return value < key.time; });
        return it == keys.begin() ? nullptr : &*(it - 1);
    };
    if (const auto* key = latest(state.aiming); key && key->value != 0) {
        flags.aiming = true;
        flags.aiming_since = key->time;
    }
    if (const auto* key = latest(state.passive)) flags.passive = key->value != 0;
    if (const auto* key = latest(state.reloading); key && key->value != 0) {
        flags.reloading = true;
        flags.reloading_since = key->time;
    }
    return flags;
}

std::vector<int> weapon_attachments_at(const DemoData& demo, int handle, float time, bool& known) {
    std::vector<int> ids;
    known = false;
    const auto found = demo.weapons.find(handle);
    if (found == demo.weapons.end()) return ids;
    const auto& keys = found->second.attachments;
    const auto it = std::upper_bound(keys.begin(), keys.end(), time,
        [](float value, const TimedText& key) { return value < key.time; });
    if (it == keys.begin()) return ids;
    known = true;
    const auto& text = (it - 1)->text;
    for (std::size_t start = 0; start < text.size();) {
        auto end = text.find(',', start);
        if (end == std::string::npos) end = text.size();
        if (end > start) ids.push_back(std::atoi(text.substr(start, end - start).c_str()));
        start = end + 1;
    }
    return ids;
}

std::vector<std::string> inventory_at(const DemoData& demo, const DemoPlayer& player, float time) {
    std::vector<std::string> classes;
    if (player.entity < 0) return classes;
    for (const auto& [handle, state] : demo.weapons) {
        if (state.gone >= 0.0f && state.gone <= time) continue;
        if (timed_int_at(state.owner, time, -1) != player.entity) continue;
        const auto& name = timed_text_at(state.class_name, time);
        const auto& any = name.empty() && !state.class_name.empty() ? state.class_name.front().text : name;
        if (!any.empty() && std::find(classes.begin(), classes.end(), any) == classes.end()) {
            classes.push_back(any);
        }
    }
    std::sort(classes.begin(), classes.end());
    return classes;
}

const ViewModelKey* view_model_at(const DemoData& demo, float time) {
    const auto it = std::upper_bound(demo.view_model.begin(), demo.view_model.end(), time,
        [](float value, const ViewModelKey& key) { return value < key.time; });
    return it == demo.view_model.begin() ? nullptr : &*(it - 1);
}

const RadioSync* radio_sync_at(const DemoData& demo, float time) {
    const auto it = std::upper_bound(
        demo.radio_sync.begin(), demo.radio_sync.end(), time,
        [](float value, const RadioSync& sync) { return value < sync.time; });
    return it == demo.radio_sync.begin() ? nullptr : &*(it - 1);
}

bool player_incapacitated(const DemoPlayer& player, float time) {
    const int ragdoll = timed_int_at(player.ragdoll,time,0);
    const auto* info = player_info_at(player,time);
    return (ragdoll > 0 && ragdoll < 8191) || (info && info->life_state > 0);
}

const TimedInt* player_attack_at(const DemoPlayer& player, float time) {
    const auto* attack = key_at(player.attacks,time);
    const auto* active = key_at(player.active_weapon,time);
    if (!attack || !active || active->handle < 0 || attack->time < active->time) return nullptr;
    return attack;
}

const TimedInt* player_reload_at(const DemoPlayer& player, float time) {
    auto after = std::upper_bound(player.reloads.begin(), player.reloads.end(), time,
        [](float value, const TimedInt& key) { return value < key.time; });
    while (after != player.reloads.begin()) {
        const auto* reload = &*--after;
        if (reload->value == 5) return nullptr;
        if (reload->value != 3) continue;
        const auto* active = key_at(player.active_weapon, time);
        const auto* attack = player_attack_at(player, time);
        if (!active || active->handle < 0 || reload->time < active->time
            || (attack && attack->time >= reload->time)) return nullptr;
        return reload;
    }
    return nullptr;
}

bool ragdoll_pose_at(const DemoData& demo, const DemoPlayer& player, float time, RagdollPose& pose) {
    const int entity = timed_int_at(player.ragdoll, time, 0);
    if (entity <= 0 || entity >= 8191) return false;
    // The newest ragdoll on that entity index that exists by `time` (indexes are reused).
    const DemoRagdoll* best = nullptr;
    for (const auto& ragdoll : demo.ragdolls) {
        if (ragdoll.entity != entity || ragdoll.keys.empty() || ragdoll.keys.front().time > time) continue;
        if (!best || ragdoll.keys.front().time > best->keys.front().time) best = &ragdoll;
    }
    if (!best) return false;
    const auto after = std::upper_bound(
        best->keys.begin(), best->keys.end(), time,
        [](float value, const RagdollKey& key) { return value < key.time; });
    pose.key = &*(after - 1);
    pose.model = best->model;
    return pose.model > 0 && pose.key->pos.size() >= 3
        && pose.key->pos.size() == pose.key->ang.size();
}

namespace {

std::string lower_text(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string read_text_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Could not open " + path.string());
    std::string text(static_cast<std::size_t>(file.tellg()), '\0');
    file.seekg(0);
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    return text;
}

void read_int_list(Json& json, std::vector<int>& out) {
    if (json.peek() != '[') {  // an empty Lua table is exported as {}
        json.skip();
        return;
    }
    json.array([&] { out.push_back(static_cast<int>(json.number())); });
}

void read_rank_table(Json& json, RankTable& table) {
    json.object([&](std::string_view group) {
        auto* target = group == "names" ? &table.names : group == "short" ? &table.short_names : nullptr;
        if (!target) {
            json.skip();
            return;
        }
        json.object([&](std::string_view rank) {
            (*target)[std::atoi(std::string(rank).c_str())] = json.string_value();
        });
    });
}

} // namespace

PerpheadsRegistry load_perpheads_registry(const std::filesystem::path& directory) {
    PerpheadsRegistry registry;
    {
        const auto text = read_text_file(directory / L"clothing_types.json");
        Json json(text.data(), text.data() + text.size());
        json.object([&](std::string_view section) {
            if (section == "requiredSlots") {
                read_int_list(json, registry.required_slots);
            } else if (section == "types") {
                json.array([&] {
                    ClothingType type;
                    json.object([&](std::string_view key) {
                        if (key == "index") type.index = static_cast<int>(json.number());
                        else if (key == "identifier") type.identifier = json.string_value();
                        else if (key == "model") type.model = lower_text(json.string_value());
                        else if (key == "accessory") type.accessory = json.boolean();
                        else if (key == "government") type.government = json.boolean();
                        else if (key == "bodyPart") type.body_part = json.boolean();
                        else if (key == "dataNumberBodyGroup") type.data_number_bodygroup = lower_text(json.string_value());
                        else if (key == "dataNumberMaterialBase") type.data_number_material_base = lower_text(json.string_value());
                        else if (key == "attachBodyPartsForSlots") type.attach_body_parts_for_slots = json.boolean();
                        else if (key == "slots") read_int_list(json, type.slots);
                        else if (key == "optionalBodyPartSlots") {
                            if (json.peek() == '[') read_int_list(json, type.optional_slots);
                            else json.skip();
                        } else if (key == "excludedBodyParts") {
                            if (json.peek() == '[') {
                                json.array([&] { type.excluded_body_parts.push_back(json.string_value()); });
                            } else {
                                json.skip();
                            }
                        } else if (key == "skins") {
                            if (json.peek() != '[') {
                                json.skip();
                                return;
                            }
                            json.array([&] {
                                ClothingSkinDef skin;
                                json.object([&](std::string_view skin_key) {
                                    if (skin_key == "canTint") {
                                        skin.can_tint = json.boolean();
                                    } else if (skin_key == "subMaterials" && json.peek() == '[') {
                                        json.array([&] {
                                            std::vector<std::string> pair;
                                            json.array([&] { pair.push_back(lower_text(json.string_value())); });
                                            if (pair.size() == 2) {
                                                skin.sub_materials.emplace_back(pair[0], pair[1]);
                                            }
                                        });
                                    } else {
                                        json.skip();
                                    }
                                });
                                type.skins.push_back(std::move(skin));
                            });
                        } else if (key == "modelVariants") {
                            if (json.peek() == '[') {
                                json.array([&] {
                                    ClothingModelVariant variant;
                                    json.object([&](std::string_view variant_key) {
                                        if (variant_key == "feet") variant.missing_feet = json.boolean();
                                        else if (variant_key == "thighs") variant.missing_thighs = json.boolean();
                                        else if (variant_key == "model") variant.model = lower_text(json.string_value());
                                        else json.skip();
                                    });
                                    type.variants.push_back(std::move(variant));
                                });
                            } else {
                                json.skip();
                            }
                        } else {
                            json.skip();
                        }
                    });
                    registry.clothing[type.index] = std::move(type);
                });
            } else {
                json.skip();
            }
        });
    }
    {
        const auto text = read_text_file(directory / L"perpheads_jobs.json");
        Json json(text.data(), text.data() + text.size());
        json.object([&](std::string_view section) {
            if (section == "teams") {
                json.object([&](std::string_view) {
                    JobTeam team;
                    json.object([&](std::string_view key) {
                        if (key == "id") {
                            team.id = static_cast<int>(json.number());
                        } else if (key == "name") {
                            team.name = json.string_value();
                        } else if (key == "color") {
                            int channel = 0;
                            json.array([&] {
                                const auto value = static_cast<int>(json.number());
                                if (channel < 3) team.color[channel] = static_cast<unsigned char>(value);
                                ++channel;
                            });
                        } else {
                            json.skip();
                        }
                    });
                    registry.teams[team.id] = std::move(team);
                });
            } else if (section == "ranks") {
                json.object([&](std::string_view group) {
                    auto* table = group == "police" ? &registry.police
                        : group == "medic" ? &registry.medic
                        : group == "firefighter" ? &registry.firefighter : nullptr;
                    if (table) read_rank_table(json, *table);
                    else json.skip();
                });
            } else if (section == "dispatcherTeam") {
                registry.team_dispatcher = static_cast<int>(json.number());
            } else if (section == "radioChannels") {
                json.object([&](std::string_view id) {
                    registry.radio_channels[std::atoi(std::string(id).c_str())] = json.string_value();
                });
            } else if (section == "npcs") {
                json.object([&](std::string_view id) {
                    registry.npc_names[std::atoi(std::string(id).c_str())] = json.string_value();
                });
            } else if (section == "citizenTeam") {
                registry.team_citizen = static_cast<int>(json.number());
            } else {
                json.skip();
            }
        });
    }
    {
        const auto weapons_path = directory / L"swep_models.json";
        if (std::filesystem::exists(weapons_path)) {
            const auto text = read_text_file(weapons_path);
            Json json(text.data(), text.data() + text.size());
            json.object([&](std::string_view view) {
                registry.swep_world_models[std::string(view)] = json.string_value();
            });
        }
    }
    {
        const auto sounds_path = directory / L"swep_sounds.json";
        if (std::filesystem::exists(sounds_path)) {
            const auto text = read_text_file(sounds_path);
            Json json(text.data(), text.data() + text.size());
            json.object([&](std::string_view view) {
                registry.swep_fire_sounds[std::string(view)] = json.string_value();
            });
        }
    }
    {
        const auto info_path = directory / L"swep_info.json";
        if (std::filesystem::exists(info_path)) {
            const auto text = read_text_file(info_path);
            Json json(text.data(), text.data() + text.size());
            json.object([&](std::string_view view) {
                SwepInfo info;
                json.object([&](std::string_view key) {
                    const auto vector = [&](float out[3]) {
                        int axis = 0;
                        json.array([&] {
                            const auto value = static_cast<float>(json.number());
                            if (axis < 3) out[axis] = value;
                            ++axis;
                        });
                    };
                    if (key == "name") info.name = json.string_value();
                    else if (key == "base") info.base = json.string_value();
                    else if (key == "gun") info.gun = json.boolean();
                    else if (key == "melee") info.melee = json.boolean();
                    else if (key == "meleePrimaryDelay") info.melee_primary_delay=static_cast<float>(json.number());
                    else if (key == "meleeSecondaryDelay") info.melee_secondary_delay=static_cast<float>(json.number());
                    else if (key == "ironPos") vector(info.iron_pos);
                    else if (key == "ironAng") vector(info.iron_ang);
                    else if (key == "railPos") {
                        info.rail_pos.emplace();
                        vector(info.rail_pos->data());
                    }
                    else if (key == "railAng") vector(info.rail_ang);
                    else if (key == "fov") info.fov = static_cast<float>(json.number());
                    else if (key == "flip") info.flip = json.boolean();
                    else if (key == "dollShake" || key == "dollFidget" || key == "meleeSwing") {
                        auto& sound = key == "meleeSwing" ? info.melee_swing : key == "dollShake" ? info.doll_shake : info.doll_fidget;
                        json.object([&](std::string_view field) {
                            if (field == "files") json.array([&] { sound.files.push_back(json.string_value()); });
                            else if (field == "level") sound.level = static_cast<float>(json.number());
                            else if (field == "volume") sound.volume = static_cast<float>(json.number());
                            else json.skip();
                        });
                    }
                    else json.skip();
                });
                registry.swep_info[std::string(view)] = std::move(info);
            });
        }
    }
    {
        const auto gun_path = directory / L"gun_sounds.json";
        if (std::filesystem::exists(gun_path)) {
            const auto text = read_text_file(gun_path);
            Json json(text.data(), text.data() + text.size());
            json.object([&](std::string_view section) {
                if (section == "maxBulletDistance") registry.gun_sound_distance = static_cast<float>(json.number());
                else if (section == "maxImpactDistance") registry.impact_sound_distance = static_cast<float>(json.number());
                else if (section == "sounds") {
                    json.array([&] {
                        int index = 0;
                        GunSoundEntry entry;
                        json.object([&](std::string_view key) {
                            if (key == "index") index = static_cast<int>(json.number());
                            else if (key == "name") entry.name = json.string_value();
                            else if (key == "files") json.array([&] { entry.files.push_back(lower_text(json.string_value())); });
                            else if (key == "level") entry.level = static_cast<float>(json.number());
                            else if (key == "volume") entry.volume = static_cast<float>(json.number());
                            else json.skip();
                        });
                        if (index > 0 && !entry.files.empty()) registry.gun_sounds[index] = std::move(entry);
                    });
                } else {
                    json.skip();
                }
            });
        }
    }
    const auto attachment_path = directory / L"weapon_attachments.json";
    if (std::filesystem::exists(attachment_path)) {
        const auto text = read_text_file(attachment_path);
        Json json(text.data(),text.data()+text.size());
        json.object([&](std::string_view section) {
            if (section != "attachments") { json.skip(); return; }
            json.array([&] {
                WeaponAttachmentDef entry;
                json.object([&](std::string_view key) {
                    const auto vector = [&] {
                        std::array<float,3> result{};
                        int axis = 0;
                        json.array([&] {
                            const float value = static_cast<float>(json.number());
                            if (axis < 3) result[axis] = value;
                            ++axis;
                        });
                        if (axis != 3) throw std::runtime_error("Invalid weapon attachment aim vector.");
                        return result;
                    };
                    if (key == "id") entry.id = static_cast<int>(json.number());
                    else if (key == "slot") entry.slot = static_cast<int>(json.number());
                    else if (key == "sight") entry.sight = json.boolean();
                    else if (key == "sequence") entry.sequence = static_cast<int>(json.number());
                    else if (key == "sightSequence") entry.sight_sequence = static_cast<int>(json.number());
                    else if (key == "name") entry.name = json.string_value();
                    else if (key == "model") entry.model = lower_text(json.string_value());
                    else if (key == "sound") entry.sound = lower_text(json.string_value());
                    else if (key == "magnification") entry.magnification = static_cast<float>(json.number());
                    else if (key == "ironPos") entry.iron_pos = vector();
                    else if (key == "ironAng") entry.iron_ang = vector();
                    else json.skip();
                });
                if (entry.id <= 0 || entry.model.empty() || !std::isfinite(entry.magnification)
                    || entry.magnification < 0 || entry.sequence < 0 || entry.sight_sequence < 0)
                    throw std::runtime_error("Invalid weapon attachment definition.");
                registry.weapon_attachments.emplace(entry.id,std::move(entry));
            });
        });
    }
    const auto plates_path = directory / L"vehicle_plates.json";
    if (std::filesystem::exists(plates_path)) {
        const auto text = read_text_file(plates_path);
        Json json(text.data(), text.data() + text.size());
        const auto vector = [&] {
            std::array<float, 3> result{};
            int axis = 0;
            json.array([&] {
                const float value = static_cast<float>(json.number());
                if (!std::isfinite(value) || axis >= 3)
                    throw std::runtime_error("Invalid vehicle plate vector.");
                result[axis++] = value;
            });
            if (axis != 3) throw std::runtime_error("Invalid vehicle plate vector size.");
            return result;
        };
        const auto placement = [&] {
            PlatePlacement result;
            bool pos = false, ang = false;
            json.object([&](std::string_view key) {
                if (key == "pos") { result.pos = vector(); pos = true; }
                else if (key == "ang") { result.ang = vector(); ang = true; }
                else json.skip();
            });
            if (!pos || !ang) throw std::runtime_error("Incomplete vehicle plate placement.");
            return result;
        };
        json.object([&](std::string_view section) {
            if (section == "vehicles") {
                json.array([&] {
                    std::string id;
                    VehiclePlateDef def;
                    json.object([&](std::string_view key) {
                        if (key == "id") id = json.string_value();
                        else if (key == "placements") json.array([&] { def.placements.push_back(placement()); });
                        else if (key == "kits") {
                            if (json.peek() != '[') { json.skip(); return; }
                            json.array([&] {
                                VehiclePlateDef::Kit kit;
                                json.object([&](std::string_view field) {
                                    if (field == "groups") read_int_list(json, kit.groups);
                                    else if (field == "plate") kit.plate = placement();
                                    else json.skip();
                                });
                                def.kits.push_back(std::move(kit));
                            });
                        } else json.skip();
                    });
                    if (id.empty() || def.placements.empty())
                        throw std::runtime_error("Invalid vehicle plate definition.");
                    registry.vehicle_plates.emplace(id, std::move(def));
                });
            } else if (section == "skins") {
                json.array([&] {
                    std::string id;
                    PlateSkinDef def;
                    json.object([&](std::string_view key) {
                        if (key == "id") id = json.string_value();
                        else if (key == "material") def.material = lower_text(json.string_value());
                        else if (key == "color") {
                            int channel = 0;
                            json.array([&] {
                                const float value = static_cast<float>(json.number());
                                if (channel < 3) def.color[channel] = value / 255.0f;
                                ++channel;
                            });
                            if (channel != 3 && channel != 4)
                                throw std::runtime_error("Invalid vehicle plate text color.");
                        } else json.skip();
                    });
                    if (id.empty() || def.material.empty())
                        throw std::runtime_error("Invalid vehicle plate skin.");
                    registry.plate_skins.emplace(id, std::move(def));
                });
            } else json.skip();
        });
    }
    return registry;
}

int weapon_attachment_sequence_at(const PerpheadsRegistry& registry, const DemoData& demo,
                                  int attachment, int handle, float time) {
    const auto definition = registry.weapon_attachments.find(attachment);
    if (definition == registry.weapon_attachments.end())
        throw std::runtime_error("Unknown weapon attachment sequence definition.");
    bool known = false;
    for (int id : weapon_attachments_at(demo, handle, time, known)) {
        const auto fitted = registry.weapon_attachments.find(id);
        if (fitted != registry.weapon_attachments.end() && fitted->second.sight)
            return definition->second.sight_sequence;
    }
    return definition->second.sequence;
}

std::string weapon_sound_override_at(const PerpheadsRegistry& registry, const DemoData& demo,
                                     int handle, float time) {
    const auto weapon = demo.weapons.find(handle);
    if (weapon == demo.weapons.end() || (weapon->second.gone >= 0 && weapon->second.gone <= time))
        return {};
    bool known = false;
    for (const int id : weapon_attachments_at(demo,handle,time,known)) {
        const auto found = registry.weapon_attachments.find(id);
        if (found != registry.weapon_attachments.end() && !found->second.sound.empty())
            return found->second.sound;
    }
    return {};
}

std::string job_title_at(const PerpheadsRegistry& registry, const DemoPlayer& player, float time) {
    const int team = timed_int_at(player.job_team, time, registry.team_citizen);
    if (team == registry.team_citizen) return {};
    const auto short_rank = [&](const RankTable& table, const std::vector<TimedInt>& keys) {
        const auto found = table.short_names.find(timed_int_at(keys, time, 0));
        return found == table.short_names.end() ? std::string() : found->second;
    };
    if (team == registry.team_police) return short_rank(registry.police, player.police_rank);
    if (team == registry.team_medic) return short_rank(registry.medic, player.medic_rank);
    if (team == registry.team_firefighter) {
        return short_rank(registry.firefighter, player.firefighter_rank);
    }
    if (team == registry.team_dispatcher) return "Dispatcher";
    const auto found = registry.teams.find(team);
    return found == registry.teams.end() ? std::string("Unknown") : found->second.name;
}

void job_color_at(const PerpheadsRegistry& registry, const DemoPlayer& player, float time,
                  unsigned char rgb[3]) {
    const int team = timed_int_at(player.job_team, time, registry.team_citizen);
    const auto found = registry.teams.find(team);
    if (found == registry.teams.end()) {
        rgb[0] = 0;
        rgb[1] = 255;
        rgb[2] = 0;
        return;
    }
    std::copy(found->second.color, found->second.color + 3, rgb);
}

static bool sample_core(const DemoPlayer& player, float time, PlayerPose& pose, int& segment) {
    const auto& samples = player.samples;
    if (samples.empty()) return false;
    const auto next_it = std::lower_bound(
        samples.begin(), samples.end(), time,
        [](const PlayerSample& sample, float value) { return sample.time < value; });
    const auto previous_it = std::upper_bound(
        samples.begin(), samples.end(), time,
        [](float value, const PlayerSample& sample) { return value < sample.time; });
    const bool has_previous = previous_it != samples.begin();
    const bool has_next = next_it != samples.end();
    if (!has_previous) return false;
    const auto& previous = *(previous_it - 1);
    segment = previous.segment;
    auto set = [&](const PlayerSample& from) {
        pose = {from.x, from.y, from.z, from.pitch, from.yaw, from.roll, from.model};
        pose.flags = from.flags;
    };
    if (has_next) {
        const auto& next = *next_it;
        if (previous.time == next.time) {
            set(previous);
            return true;
        }
        const float gap = next.time - previous.time;
        if (previous.segment == next.segment && gap > 0.0f && gap <= 2.5f) {
            const float t = std::clamp((time - previous.time) / gap, 0.0f, 1.0f);
            pose = {lerp(previous.x, next.x, t), lerp(previous.y, next.y, t),
                    lerp(previous.z, next.z, t), lerp_angle(previous.pitch, next.pitch, t),
                    lerp_angle(previous.yaw, next.yaw, t), lerp_angle(previous.roll, next.roll, t),
                    previous.model};
            pose.flags = previous.flags;
            return true;
        }
    }
    set(previous);
    return true;
}

int player_index_at(const DemoData& demo, int entity, float time) {
    if (entity <= 0) return -1;
    for (std::size_t index = 0; index < demo.players.size(); ++index) {
        const auto& player = demo.players[index];
        if (player.entity == entity && time >= player.connected_at
            && (player.disconnected_at < 0.0f || time < player.disconnected_at))
            return static_cast<int>(index);
    }
    return -1;
}

bool sample_player(const DemoPlayer& player, float time, PlayerPose& pose) {
    if (time < player.connected_at
        || (player.disconnected_at >= 0.0f && time >= player.disconnected_at)) return false;
    // Presence: the last spawn/gone event at or before `time`; fail open if none.
    if (!player.presence.empty()) {
        const auto after = std::upper_bound(
            player.presence.begin(), player.presence.end(), time,
            [](float value, const PresenceEvent& event) { return value < event.time; });
        if (after != player.presence.begin() && !(after - 1)->present) return false;
    }
    int segment = 0;
    if (!sample_core(player, time, pose, segment)) return false;
    // Velocity: central difference over a short window, only inside one
    // continuous segment so teleports never register as speed.
    constexpr float Half = 0.19f;
    PlayerPose before;
    PlayerPose after;
    int before_segment = 0;
    int after_segment = 0;
    const bool have_before = sample_core(player, time - Half, before, before_segment)
        && before_segment == segment;
    const bool have_after = sample_core(player, time + Half, after, after_segment)
        && after_segment == segment;
    // At the edge of a segment (just spawned, about to leave) one side of the window is
    // missing; a one-sided difference keeps the walk cycle from idling there.
    if (!have_before && have_after) before = pose;
    if (!have_after && have_before) after = pose;
    if (have_before || have_after) {
        const float span = (have_before && have_after ? 2.0f : 1.0f) * Half;
        pose.vx = (after.x - before.x) / span;
        pose.vy = (after.y - before.y) / span;
        pose.vz = (after.z - before.z) / span;
    }
    return true;
}

bool sample_recorder_camera(const DemoData& demo, float time, CameraKey& camera) {
    const auto& keys = demo.recorder_camera;
    if (keys.empty()) return false;
    const auto after = std::upper_bound(
        keys.begin(), keys.end(), time,
        [](float value, const CameraKey& key) { return value < key.time; });
    if (after == keys.begin()) {
        camera = keys.front();
        return true;
    }
    const auto& previous = *(after - 1);
    camera = previous;
    if (after != keys.end()) {
        // Several packets can describe the same simulation tick. Interpolate to
        // its final camera state, which is also the state selected at that tick.
        const auto next_end=std::upper_bound(after,keys.end(),after->time,
            [](float value,const CameraKey& key) { return value<key.time; });
        const auto& next = *std::prev(next_end);
        const float gap = next.time - previous.time;
        // Larger gaps are cuts (respawn, teleport) and must not be interpolated.
        if (gap > 0.0f && gap <= 0.65f) {
            const float t = std::clamp((time - previous.time) / gap, 0.0f, 1.0f);
            camera.time = time;
            camera.pitch = lerp_angle(previous.pitch, next.pitch, t);
            camera.yaw = lerp_angle(previous.yaw, next.yaw, t);
            camera.origin_valid = previous.origin_valid && next.origin_valid;
            if (camera.origin_valid) {
                camera.x = lerp(previous.x, next.x, t);
                camera.y = lerp(previous.y, next.y, t);
                camera.z = lerp(previous.z, next.z, t);
            }
        }
    }
    return true;
}

// democmdinfo view angles are parent-local while the recorder is in a car.
// Networked eye angles for other players are world-space and must not use this.
static bool recorder_vehicle_look(const DemoData& demo, int player, float time, float& pitch, float& yaw) {
    PlayerPose occupant;
    if (!sample_player(demo.players[player],time,occupant) || !(occupant.flags & PlayerFlagInCar)) return false;
    const auto* link=player_vehicle_at(demo.players[player],time);
    int parent=link ? (link->parent>=0 ? link->parent : link->vehicle) : -1;
    if (parent<0) {
        for (const auto& prop : demo.props) {
            const auto* owner=occupant_at(prop,time);
            if ((prop.vehicle || prop.seat) && owner && owner->owner==demo.players[player].entity) {
                parent=prop.entity; break;
            }
        }
    }
    constexpr float radians=0.01745329251994329577f;
    const float cp=std::cos(pitch*radians);
    float direction[3]{cp*std::cos(yaw*radians),cp*std::sin(yaw*radians),-std::sin(pitch*radians)};
    std::array<int,8> visited{};
    int count=0;
    while (parent>=0) {
        if (count==static_cast<int>(visited.size()) || std::find(visited.begin(),visited.begin()+count,parent)!=visited.begin()+count)
            return false;
        visited[count++]=parent;
        PropPose pose;
        // Entity indices are reused by cars, props and weapons over a recording.
        // Resolve the active vehicle/seat lifetime, not the first matching index.
        const auto prop=std::find_if(demo.props.begin(),demo.props.end(),[&](const auto& value){
            return value.entity==parent && (value.vehicle || value.seat) && sample_prop(value,time,pose);
        });
        if (prop==demo.props.end()) return false;
        const float origin[3]{},angles[3]{pose.pitch,pose.yaw,pose.roll};
        const auto rotation=source::model::matrix_from_source_angles(origin,angles);
        const float x=rotation[0]*direction[0]+rotation[1]*direction[1]+rotation[2]*direction[2];
        const float y=rotation[4]*direction[0]+rotation[5]*direction[1]+rotation[6]*direction[2];
        const float z=rotation[8]*direction[0]+rotation[9]*direction[1]+rotation[10]*direction[2];
        direction[0]=x; direction[1]=y; direction[2]=z;
        const auto key=std::upper_bound(prop->keys.begin(),prop->keys.end(),time,
            [](float value,const PropKey& frame){return value<frame.time;});
        int next=key==prop->keys.begin() ? -1 : std::prev(key)->parent;
        // Some demos omit the seat's parent field but carry its car in m_hVehicle.
        if (next<0 && prop->seat && link && link->vehicle>=0 && link->vehicle!=parent) next=link->vehicle;
        parent=next;
    }
    if (!count) return false;
    pitch=std::atan2(-direction[2],std::hypot(direction[0],direction[1]))/radians;
    yaw=std::atan2(direction[1],direction[0])/radians;
    return true;
}

bool sample_player_look(const DemoData& demo, int player, float time, float& pitch, float& yaw) {
    if (player < 0 || static_cast<std::size_t>(player) >= demo.players.size()) return false;
    if (player == demo.recorder_player) {
        CameraKey camera;
        if (sample_recorder_camera(demo,time,camera)) {
            pitch = camera.pitch;
            yaw = camera.yaw;
            if (!camera.origin_valid) recorder_vehicle_look(demo,player,time,pitch,yaw);
            return true;
        }
    }
    const auto& keys = demo.players[player].look;
    const auto after = std::upper_bound(keys.begin(),keys.end(),time,
        [](float value,const PlayerLookKey& key) { return value < key.time; });
    if (after == keys.begin()) return false;
    const auto& previous = *std::prev(after);
    pitch = previous.pitch; yaw = previous.yaw;
    if (after != keys.end()) {
        const float gap = after->time-previous.time;
        if (gap > 0 && gap <= 0.65f) {
            const float fraction = std::clamp((time-previous.time)/gap,0.0f,1.0f);
            pitch = lerp_angle(previous.pitch,after->pitch,fraction);
            yaw = lerp_angle(previous.yaw,after->yaw,fraction);
        }
    }
    return true;
}

bool sample_prop(const DemoProp& prop, float time, PropPose& pose) {
    const auto& keys = prop.keys;
    const auto after = std::upper_bound(
        keys.begin(), keys.end(), time,
        [](float value, const PropKey& key) { return value < key.time; });
    if (after == keys.begin()) return false;
    const bool persistent = prop.brush || prop.map_attached;
    if (!(after - 1)->present && !persistent) return false;
    // Last known position at or before `time`, and the next one before any removal.
    const PropKey* previous = nullptr;
    for (auto it = after; it != keys.begin();) {
        --it;
        if (!it->present && !persistent) return false;
        if (it->has_position) {
            previous = &*it;
            break;
        }
    }
    if (!previous) return false;
    const PropKey* next = nullptr;
    for (auto it = after; (after - 1)->present && it != keys.end(); ++it) {
        if (!it->present) break;
        if (it->has_position) {
            next = &*it;
            break;
        }
    }
    pose = {previous->x, previous->y, previous->z, previous->has_yaw ? previous->yaw : 0.0f,
            previous->has_yaw ? previous->pitch : 0.0f, previous->has_yaw ? previous->roll : 0.0f};
    // Updates arrive every tick while something moves; a long gap means it jumped (or sat
    // still), so it is held rather than slid across.
    if (next && next->time > previous->time && next->time - previous->time <= 1.0f) {
        const float t = std::clamp((time - previous->time) / (next->time - previous->time), 0.0f, 1.0f);
        pose.x = lerp(previous->x, next->x, t);
        pose.y = lerp(previous->y, next->y, t);
        pose.z = lerp(previous->z, next->z, t);
    }
    const auto& rotation = *(after - 1);
    if (rotation.has_yaw) {
        pose.yaw = rotation.yaw;
        pose.pitch = rotation.pitch;
        pose.roll = rotation.roll;
        if (rotation.present && after != keys.end() && after->present && after->has_yaw
            && after->time > rotation.time && after->time - rotation.time <= 1.0f) {
            const float t = (time - rotation.time) / (after->time - rotation.time);
            pose.yaw = lerp_angle(rotation.yaw,after->yaw,t);
            pose.pitch = lerp_angle(rotation.pitch,after->pitch,t);
            pose.roll = lerp_angle(rotation.roll,after->roll,t);
        }
    }
    return true;
}

bool sample_vehicle_params(const DemoProp& prop, float time, std::array<float, VehiclePoseCount>& before,
                           std::array<float, VehiclePoseCount>& after, float& fraction) {
    const auto& keys = prop.poses;
    if (keys.empty()) return false;
    const auto upper = std::upper_bound(
        keys.begin(), keys.end(), time,
        [](float value, const PoseKey& key) { return value < key.time; });
    if (upper == keys.begin()) {
        before = after = keys.front().value;
        fraction = 0.0f;
        return true;
    }
    before = (upper - 1)->value;
    after = before;
    fraction = 0.0f;
    if (upper != keys.end() && upper->time > (upper - 1)->time && upper->time - (upper - 1)->time <= 0.5f) {
        after = upper->value;
        fraction = std::clamp((time - (upper - 1)->time) / (upper->time - (upper - 1)->time), 0.0f, 1.0f);
    }
    return true;
}

void sample_vehicle_color(const DemoProp& prop, float time, float rgb[3]) {
    rgb[0] = rgb[1] = rgb[2] = 1.0f;
    const auto& keys = prop.colors;
    if (keys.empty()) return;
    const auto upper = std::upper_bound(
        keys.begin(), keys.end(), time,
        [](float value, const ColorKey& key) { return value < key.time; });
    const auto& key = upper == keys.begin() ? keys.front() : *(upper - 1);
    rgb[0] = key.r;
    rgb[1] = key.g;
    rgb[2] = key.b;
}

bool door_hidden_at(const DemoDoor& door, float time) {
    const auto& keys = door.hidden;
    const auto upper = std::upper_bound(
        keys.begin(), keys.end(), time,
        [](float value, const DoorHiddenKey& key) { return value < key.time; });
    return upper != keys.begin() && (upper - 1)->hidden;
}

bool door_yaw_at(const DemoDoor& door, float time, float& yaw) {
    std::array<float, 3> angles{};
    if (!door_angles_at(door, time, angles)) return false;
    yaw = angles[1];
    return true;
}

bool door_angles_at(const DemoDoor& door, float time, std::array<float, 3>& angles) {
    const auto& keys = door.yaws;
    if (keys.empty()) return false;
    const auto upper = std::upper_bound(
        keys.begin(), keys.end(), time,
        [](float value, const DoorYawKey& key) { return value < key.time; });
    if (upper == keys.begin()) {
        return false;
    }
    const auto& before = *(upper - 1);
    if (upper == keys.end()) {
        angles = {before.pitch, before.yaw, before.roll};
        return true;
    }
    // Unchanged angles are omitted. A later opening must not rotate a settled
    // door throughout the intervening idle period.
    if (upper->time - before.time > 0.2f) {
        angles = {before.pitch, before.yaw, before.roll};
        return true;
    }
    const float span = std::max(upper->time - before.time, 1e-4f);
    const float fraction = std::clamp((time - before.time) / span, 0.0f, 1.0f);
    const float a[3]{before.pitch, before.yaw, before.roll};
    const float b[3]{upper->pitch, upper->yaw, upper->roll};
    for (int axis = 0; axis < 3; ++axis) {
        float delta = std::remainder(b[axis] - a[axis], 360.0f);
        angles[axis] = a[axis] + delta * fraction;
    }
    return true;
}

float door_openness(const DemoDoor& door, float time, float seconds_to_open) {
    const auto& keys = door.keys;
    if (keys.empty()) return 0.0f;
    const float duration = std::max(seconds_to_open, 0.05f);
    auto settled = [](DoorState state) {
        switch (state) {
        case DoorState::Open: return 1.0f;
        case DoorState::Ajar: return 0.25f;
        default: return 0.0f;
        }
    };
    // Before the first event the door was already in the state that led to it.
    float value = 0.0f;
    switch (keys.front().state) {
    case DoorState::Open: value = 1.0f; break;
    case DoorState::Closing: value = 1.0f; break;
    case DoorState::Ajar: value = 0.25f; break;
    default: value = 0.0f; break;
    }
    if (time < keys.front().time) return value;
    DoorState moving = DoorState::Closed;
    float previous_time = keys.front().time;
    auto advance = [&](float until) {
        const float step = (until - previous_time) / duration;
        if (moving == DoorState::Opening) value = std::min(1.0f, value + step);
        else if (moving == DoorState::Closing) value = std::max(0.0f, value - step);
        previous_time = until;
    };
    for (const auto& key : keys) {
        if (key.time > time) break;
        advance(key.time);
        if (key.state == DoorState::Opening || key.state == DoorState::Closing) {
            moving = key.state;
        } else {
            moving = DoorState::Closed;
            value = settled(key.state);
        }
    }
    advance(time);
    return value;
}

} // namespace gmod::demo
