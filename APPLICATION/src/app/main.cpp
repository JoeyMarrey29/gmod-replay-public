#include <windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <shellapi.h>

#include "resource.h"
#include "dx12/DescriptorHeap.h"
#include "dx12/ResourceHelpers.h"
#include "dx12/GpuAssetCache.h"
#include "dx12/FrameTextOverlay.h"
#include "app/CpuAssetCache.h"
#include "app/ParsedDemoCache.h"
#include "app/VehicleCamera.h"
#include "app/Edition.h"
#include "app/ViewingPolicy.h"
#include "demo/DemoCache.h"
#include "audio/AudioSystem.h"
#include "audio/SoundCatalog.h"
#include "audio/StoppedSoundEvents.h"
#include "demo/ChatStyle.h"
#include "demo/DemoData.h"
#include "demo/BankLighting.h"
#include "demo/BankShutters.h"
#include "demo/BackWeapons.h"
#include "demo/MeleePlayback.h"
#include "demo/FireEffects.h"
#include "demo/Perpheads.h"
#include "demo/ModelIdentifiers.h"
#include "ui/MainMenu.h"
#include "ui/EscMenu.h"
#include "ui/GameConsole.h"
#include "ui/Hud.h"
#include "ui/Settings.h"
#include "ui/NameTags.h"
#include "ui/PlayerListPanel.h"
#include "ui/RadioHud.h"
#include "ui/DialogHud.h"
#include "ui/PlayerCard.h"
#include "ui/TimelineBar.h"
#include "source/bsp/BspReader.h"
#include "source/bsp/StaticPropsReader.h"
#include "source/AssetResolver.h"
#include "source/GameContent.h"
#include "source/Vfs.h"
#include "source/Ground.h"
#include "source/WorldTrace.h"
#include "source/model/VvdReader.h"
#include "source/model/StaticPropMeshImporter.h"
#include "source/model/PlayerAnimator.h"
#include "source/vmt/VmtReader.h"
#include "source/vtf/VtfReader.h"
#include "source/png/PngReader.h"

#include <array>
#include <atomic>
#include <cfloat>
#include <charconv>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT FrameCount = 2;
constexpr UINT WindowWidth = 1280;
constexpr UINT WindowHeight = 720;
bool g_mouse_captured = false;
// While the player list is open the cursor must stay free, so clicks on the
// scene must not capture it; the wheel is forwarded to the list instead.
bool g_mouse_locked_out = false;
// Outside free view the cursor stays free (so the timeline and list are always
// clickable) and the scene is orbited by dragging; free view captures the mouse.
bool g_free_view = true;
// The playback bar is open: the cursor stays free for it, so free view looks around by
// dragging instead of capturing the mouse.
bool g_drag_look = false;
bool g_dragging = false;
POINT g_drag_last{};
float g_drag_dx = 0.0f;
float g_drag_dy = 0.0f;
gmod::ui::PlayerListPanel* g_player_list = nullptr;
// The developer console (opened with the key left of 1). While it is open it receives the
// keyboard, and the viewer's own key shortcuts stand down.
gmod::ui::GameConsole* g_console = nullptr;
bool g_text_input = false;
bool g_console_key_pending = false;
bool g_esc_consumed = false;
// Returning home was confirmed in the Esc menu.
bool g_menu_requested = false;
// The 3D view is drawn into this child window, which fills the main window's client area and
// is destroyed with the scene. The main window itself is only ever painted with GDI (menu,
// loading screen), which a flip-model swap chain on the same window would get in the way of.
HWND g_surface = nullptr;
// The playing scene's audio, so closing the window can silence it at once.
gmod::audio::AudioSystem* g_audio = nullptr;

// Closing the window must end the process: whatever is still tearing down after a few
// seconds is not worth keeping the (possibly still audible) process alive for.
void start_exit_watchdog() {
    static std::atomic<bool> started{false};
    if (started.exchange(true)) return;
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        TerminateProcess(GetCurrentProcess(), 0);
    }).detach();
}

void apply_audio_settings(gmod::audio::AudioSystem* audio) {
    const auto& levels = gmod::ui::settings();
    audio->set_master_volume(static_cast<float>(levels.volume) / 100.0f);
    audio->set_category_volume(gmod::audio::SoundCategory::Cars, static_cast<float>(levels.cars_volume) / 100.0f);
    audio->set_category_volume(gmod::audio::SoundCategory::Voices, static_cast<float>(levels.voices_volume) / 100.0f);
    audio->set_category_volume(gmod::audio::SoundCategory::Gunshots, static_cast<float>(levels.gunshots_volume) / 100.0f);
    audio->set_category_volume(gmod::audio::SoundCategory::Doors, static_cast<float>(levels.doors_volume) / 100.0f);
    audio->set_category_volume(gmod::audio::SoundCategory::Alarms, static_cast<float>(levels.alarms_volume) / 100.0f);
    audio->set_category_volume(gmod::audio::SoundCategory::Ambient, static_cast<float>(levels.ambient_volume) / 100.0f);
    audio->set_category_volume(gmod::audio::SoundCategory::Other, static_cast<float>(levels.other_volume) / 100.0f);
}

// Mouse input falls through the surface to the main window, which handles it all.
LRESULT CALLBACK surface_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wparam, lparam);
}

// Everything the program prints (load progress, warnings) goes to the tilde console instead
// of a separate command prompt window, and to build\console.log. Lines from every thread are
// collected here; the console picks up the new ones as it draws.
std::mutex g_log_mutex;
std::vector<std::wstring> g_log_lines;
std::ofstream g_log_file;

void add_log_line(std::wstring line) {
    // Caller holds g_log_mutex.
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    if (g_log_file.is_open()) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()),
                                             nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<std::size_t>(std::max(size, 0)), '\0');
        if (size > 0) {
            WideCharToMultiByte(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(),
                                size, nullptr, nullptr);
        }
        g_log_file << utf8 << "\n";
        g_log_file.flush();
    }
    g_log_lines.push_back(std::move(line));
}

std::size_t log_line_count() {
    std::lock_guard lock(g_log_mutex);
    return g_log_lines.size();
}

std::vector<std::wstring> log_lines(std::size_t from, std::size_t to) {
    std::lock_guard lock(g_log_mutex);
    to = std::min(to, g_log_lines.size());
    if (from >= to) return {};
    return {g_log_lines.begin() + static_cast<std::ptrdiff_t>(from),
            g_log_lines.begin() + static_cast<std::ptrdiff_t>(to)};
}

template <typename Char>
class LogBuffer : public std::basic_streambuf<Char> {
    using Traits = std::char_traits<Char>;
    using IntType = typename Traits::int_type;

protected:
    IntType overflow(IntType ch) override {
        if (Traits::eq_int_type(ch, Traits::eof())) return Traits::not_eof(ch);
        const Char value = Traits::to_char_type(ch);
        put(&value, 1);
        return ch;
    }
    std::streamsize xsputn(const Char* text, std::streamsize count) override {
        put(text, count);
        return count;
    }

private:
    void put(const Char* text, std::streamsize count) {
        std::lock_guard lock(g_log_mutex);
        for (std::streamsize index = 0; index < count; ++index) {
            if (text[index] == Char('\n')) {
                add_log_line(widen(pending_));
                pending_.clear();
            } else {
                pending_.push_back(text[index]);
            }
        }
    }
    static std::wstring widen(const std::basic_string<Char>& text) {
        if constexpr (std::is_same_v<Char, wchar_t>) {
            return text;
        } else {
            if (text.empty()) return {};
            const int size = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(std::max(size, 0)), L'\0');
            if (size > 0) MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
            return wide;
        }
    }
    std::basic_string<Char> pending_;
};

using gmod::app::CpuAssetCache;

LogBuffer<char> g_log_narrow;
LogBuffer<wchar_t> g_log_wide;

void capture_program_output(const std::filesystem::path& log_path) {
    std::error_code ignored;
    std::filesystem::create_directories(log_path.parent_path(), ignored);
    g_log_file.open(log_path, std::ios::trunc);
    std::cout.rdbuf(&g_log_narrow);
    std::cerr.rdbuf(&g_log_narrow);
    std::clog.rdbuf(&g_log_narrow);
    std::wcout.rdbuf(&g_log_wide);
    std::wcerr.rdbuf(&g_log_wide);
    std::wclog.rdbuf(&g_log_wide);
}
// Screenshot runs for automated checks: no console, no window and no overlay is ever shown,
// so they never appear over whatever is on screen.
bool g_headless = false;
bool g_disable_blood_mist = false;
bool g_disable_smoke = false;
// The window's client size changed; the renderer rebuilds its swap chain before the next frame.
bool g_resize_pending = false;
UINT g_resize_width = 0;
UINT g_resize_height = 0;
// A press that did not move is a click on the scene (it selects the player under it).
float g_press_moved = 0.0f;
bool g_click_pending = false;
bool g_follow_click_pending = false;
bool g_press_double_click = false;
POINT g_click_at{};
// Right-click anywhere clears the selection.
bool g_deselect_pending = false;
// The cursor is over a player that a click would select.
bool g_hover_player = false;

void capture_mouse(HWND window) {
    if (g_mouse_captured || g_headless) return;
    SetCapture(window);
    ShowCursor(FALSE);
    g_mouse_captured = true;
}

void release_mouse() {
    if (!g_mouse_captured) return;
    ReleaseCapture();
    ClipCursor(nullptr);
    ShowCursor(TRUE);
    g_mouse_captured = false;
}

// Thrown when the user cancels a load. Deliberately not a
// std::exception so per-item `catch (const std::exception&)` blocks don't
// swallow it.
struct LoadCancelled {};
struct ViewingDenied : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Progress shared between the loader thread and the UI thread.
struct LoadingState {
    struct Snapshot {
        int stage = 0;
        std::wstring stage_name;
        std::wstring detail;
        std::size_t current = 0;
        std::size_t total = 0;
    };

    void begin_stage(int stage, std::wstring name, std::size_t total) {
        std::lock_guard lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        if (!state_.stage_name.empty())
            std::wcout << L"LOAD_STAGE " << state_.stage_name << L" seconds="
                << std::chrono::duration<double>(now - stage_started_).count() << L"\n";
        stage_started_ = now;
        state_.stage = stage + stage_offset;
        state_.stage_name = std::move(name);
        state_.detail.clear();
        state_.current = 0;
        state_.total = total;
    }

    // Advances the current stage by one item and checks for cancellation.
    void step(const std::string& item) {
        check_cancel();
        std::lock_guard lock(mutex_);
        ++state_.current;
        state_.detail.assign(item.begin(), item.end());
    }

    void check_cancel() const {
        if (cancel) {
            throw LoadCancelled{};
        }
    }

    Snapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return state_;
    }

    // Reports progress inside the current stage (e.g. a percentage).
    void progress(std::size_t current, const std::string& text) {
        std::lock_guard lock(mutex_);
        state_.current = current;
        state_.detail.assign(text.begin(), text.end());
    }

    // Demo loads add a leading parse the demo stage.
    int stage_count = 5;
    int stage_offset = 0;
    std::wstring title = L"Loading rp_paralake_city_v6.3";
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    bool cancellable = true;
    const std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();

private:
    mutable std::mutex mutex_;
    Snapshot state_;
    std::chrono::steady_clock::time_point stage_started_ = std::chrono::steady_clock::now();
};

LoadingState* g_loading = nullptr;
gmod::ui::MainMenu* g_menu = nullptr;
void paint_loading_screen(HWND window, const LoadingState& loading);

RECT loading_cancel_rect(HWND window) {
    RECT client{};
    GetClientRect(window, &client);
    const int right = client.right - client.right / 8;
    const int bottom = std::min<LONG>(client.bottom - 12, client.bottom / 2 + 146);
    return {std::max(12, right - 160), bottom - 38, right, bottom};
}

struct WorldVertex {
    float position[3];
    float uv[2];
    // WorldVertexTransition weight (0 = $basetexture, 1 = $basetexture2).
    float blend = 0.0f;
    // World: lightmap-atlas UV (negative u = unlit). Props: model-space normal.
    float extra[3]{};
};

struct AmbientGpu {
    // rgb per ambient-cube face; color[0].w is 1 when the sun reaches the prop.
    float color[6][4]{};
};

struct WorldMaterialTexture {
    std::string material;
    std::filesystem::path vtf;
    std::filesystem::path png;
    bool decodable = false;
    // Fallback texture for water materials without a decodable normal map.
    bool water_placeholder = false;
    bool neutral_placeholder = false;
    bool prop_material = false;
    // 0 opaque, 1 alpha-test, 2 translucent blend, 3 additive (from the VMT).
    UINT alpha_mode = 0;
    float alpha_ref = 0.5f;
    std::filesystem::path vtf2;
    std::filesystem::path modulate;
    // Runtime base-texture size (engine GetMappingWidth/Height); 0 if unknown.
    float mapping_size[2]{};
    float decal_scale = 1.0f;
    // Bit 0: blend with $basetexture2; bit 1: $blendmodulatetexture present.
    UINT material_flags = 0;
    int format = -1;
    bool operator==(const WorldMaterialTexture&) const = default;
};

struct WorldBatch {
    std::string material;
    UINT vertex_offset = 0;
    UINT vertex_count = 0;
    UINT texture_index = 0;
    UINT alpha_mode = 0;
    float alpha_ref = 0.5f;
    UINT material_flags = 0;
    // Drawn in the 3D-skybox pass instead of the main pass.
    bool sky = false;
    int map_brush = -1;
    bool active = true;
};

struct Matrix4 {
    float values[16]{};
};

struct Float3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct DrawConstants {
    UINT instance_base = 0;
    UINT use_instances = 0;
    UINT alpha_mode = 0;
    float alpha_ref = 0.5f;
    UINT material_flags = 0;
    // SV_VertexID excludes the draw's BaseVertexLocation, so shaders that index
    // per-vertex side buffers add it themselves.
    UINT base_vertex = 0;
};
constexpr UINT DrawConstantCount = sizeof(DrawConstants) / sizeof(UINT);

struct PropDraw {
    std::string material;
    UINT index_offset = 0;
    UINT index_count = 0;
    INT base_vertex = 0;
    UINT variant = 0;
    UINT texture_index = 0;
    UINT alpha_mode = 0;
    float alpha_ref = 0.5f;
    UINT material_flags = 0;
    bool map_geometry = false;
    UINT skin_base_vertex = 0;
};

// One model/skin combination; its instances are contiguous in `instances`.
struct PropVariant {
    UINT first_instance = 0;
    UINT instance_count = 0;
    bool first_person = false;
    std::shared_ptr<gmod::source::WorldTrace> camera_trace;
};

// Instance transform indices with this bit set refer to the per-frame dynamic
// buffers (demo-driven actors) instead of the static tables.
constexpr UINT DynamicBit = 0x80000000u;
// Dynamic instance ids pack the actor slot (low 20 bits) and, for skinned
// actors, the player's palette index + 1 (next 11 bits).
constexpr UINT DynamicSlotMask = 0x000FFFFFu;
constexpr UINT PaletteShift = 20;
constexpr UINT PaletteMask = 0x7FFu;
constexpr UINT MaxPaletteBones = 128;

struct PropInstance {
    float center[3]{};
    float radius = 0.0f;
    float origin[3]{};
    float fade_max = 0.0f;
    UINT transform = 0;
    bool sky = false;
    // Demo playback sets the pose of dynamic instances every frame.
    bool dynamic = false;
    bool active = true;
    float local_center[3]{};
    int map_entity = -1;
};

// What each character wears over time, composed from the demo's clothing data.
struct OutfitData {
    struct Key {
        float time = 0.0f;
        std::vector<int> parts;  // indices into `parts` worn from `time`
    };
    gmod::demo::PerpheadsRegistry registry;
    std::vector<gmod::demo::OutfitPart> parts;
    std::vector<int> owner;  // player index each part belongs to
    std::vector<std::vector<Key>> by_player;
    // Material replacement tables; a clothing variant's skin carries (index + 1) << 8.
    std::vector<std::vector<std::pair<std::string, std::string>>> remaps;
    // Ragdoll model index (demo model table) -> skeleton bone driven by each physics solid.
    std::unordered_map<int, std::vector<int>> ragdoll_bones;
    // First-person view models (c_ weapon models) with their own skeleton and sequences.
    struct ViewModelAsset {
        std::string path;  // normalized
        std::shared_ptr<gmod::source::model::AnimationModel> animation;
        std::shared_ptr<gmod::source::model::SkeletonPoser> poser;
        std::vector<std::string> names;
        std::vector<std::array<float, 12>> bind;  // bone -> model space (bind pose)
        // Player skeleton bone -> view model bone it is bonemerged to; -1 = hidden.
        std::vector<int> arm_bone;
        int idle = -1;
        int fire = -1;        // firing sequence (hip)
        int fire_secondary = -1;
        int fire_aimed = -1;  // firing sequence while aiming down sights
        int draw = -1;        // pulling the weapon out
        int reload = -1;      // reloading (single-sequence reloads)
        int pin = -1;
        int spoon = -1;
        int toss = -1;
        int idle_nopin = -1;
        int idle_nospoon = -1;
        // Playback rate the server used for a sequence of this model (from the recorder's own
        // view model data); applied to other players, whose view models are not recorded.
        std::unordered_map<int, float> sequence_rate;
        std::vector<int> reload_chain;  // start, loading steps, end for per-round reloads
    };
    std::vector<ViewModelAsset> view_models;
    std::unordered_map<std::string, int> view_model_ids;  // normalized path -> view_models
    // Vehicle models, each skinned to its own skeleton and animated from the pose parameters
    // the server networks (same structure as a view model asset).
    struct ChestAttachment { int bone=-1;std::array<float,12> bind{}; };
    std::unordered_map<int,ChestAttachment> chest_attachments;
    std::vector<ViewModelAsset> vehicle_assets;
    std::unordered_map<std::string, int> vehicle_ids;  // normalized path -> vehicle_assets
    struct Plate {
        gmod::demo::VehiclePlateState state;
        gmod::demo::PlatePlacement placement;
        std::array<float, 3> color{};
    };
    std::vector<Plate> plates;
    std::unordered_map<int, gmod::source::model::StaticPropMeshVertex> badge_probes;
};

// What a dynamic slot represents in the demo.
struct ActorBinding {
    int kind = 0;   // 1 player, 2 physics prop or vehicle, 3 door, 4 clothing, 5 held weapon,
                    // 6 view model, 7 arm clothing, 8 brush, 9 parented map model,
                    // 10 world weapon attachment, 11 view model attachment, 12 back gun, 13 licence plate
    int index = -1; // into the demo's players / props / doors
    int model = -1; // players: model index in the demo's table
    int entry = -1; // clothing: index into OutfitData::parts
    int skin = 0;
    bool female = false;
    std::string weapon;  // held weapons: the class name the player must be holding
    int attachment = -1;
    int attachment_sequence = 0;
    // Players and clothing: skinned with this player's bone palette.
    int palette = -1;
    std::string model_path;
    bool initial_hidden = false;
    float origin[3]{};
    float angles[3]{};
    float door_distance = 90.0f;
    float door_speed = 100.0f;
    int open_dir = 1;
    int muzzle_bone = -1;
    std::array<float,3> muzzle_point{};
};

struct PropRenderData {
    std::vector<WorldVertex> vertices;
    std::vector<std::uint32_t> indices;
    // transforms[0] is identity and is used by world geometry.
    std::vector<Matrix4> transforms;
    std::vector<PropInstance> instances;
    std::vector<PropVariant> variants;
    std::vector<PropDraw> batches;
    // Baked leaf ambient cube per transform index (entry 0 is unused).
    std::vector<AmbientGpu> ambient;
    // One entry per dynamic slot (demo actors).
    std::vector<ActorBinding> actors;
    // Packed bone ids/weights per entry of `vertices` (see StaticPropMeshVertex).
    std::vector<std::array<std::uint32_t, 2>> skin;
    // Player bone palettes: this many players, MaxPaletteBones matrices each.
    std::size_t palette_players = 0;
    std::size_t map_vertices = 0;
    std::size_t map_indices = 0;
};

struct DynamicPose {
    bool active = false;
    Matrix4 transform{};
    AmbientGpu ambient{};
};

struct EffectVertex {
    float position[3];
    float uv[2];
    float opacity = 1.0f;
    float radius = 0.0f;
    float color[3]{1,1,1};
};

struct EffectDraw {
    UINT first = 0;
    UINT count = 0;
    std::string material;
    bool additive = false;
    bool mist = false;
    bool smoke = false;
};

// What the demo's sound effects need: the floor under a foot, which step sounds a surface
// uses, and the files of every weapon and step sound.
struct DemoAudioAssets {
    gmod::source::GroundMap ground;
    gmod::source::SurfaceSounds surfaces;
    gmod::audio::SoundCatalog catalog;
    gmod::source::WorldTrace trace;  // line of sight and bullet impacts
    gmod::source::WorldTrace decal_trace;
    std::unordered_map<std::string,std::shared_ptr<gmod::source::WorldTrace>> model_traces;
};

// Fire samples of a SWEP (by normalized first-person model). When its own sample is not in
// any installed content (the Glock 17's "Glock17_Shoot" script and file exist nowhere), the
// sibling of the same SWEP base with the most similar model name stands in; `via` names it.
std::vector<std::string> resolve_fire_sounds(const gmod::demo::PerpheadsRegistry& registry,
                                             const gmod::audio::SoundCatalog& catalog,
                                             const std::string& view_key, std::string* via = nullptr) {
    const auto own = registry.swep_fire_sounds.find(view_key);
    if (own != registry.swep_fire_sounds.end()) {
        for (const auto& [index, entry] : registry.gun_sounds) {
            std::string name = entry.name;
            std::transform(name.begin(), name.end(), name.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name == own->second && !entry.files.empty()) return entry.files;
        }
        auto files = catalog.fire_sounds(own->second);
        if (!files.empty()) return files;
    }
    const auto info = registry.swep_info.find(view_key);
    if (info == registry.swep_info.end() || !info->second.gun || info->second.base.empty()) return {};
    std::vector<std::string> best;
    std::size_t best_shared = 0;
    for (const auto& [key, sound] : registry.swep_fire_sounds) {
        if (key == view_key) continue;
        const auto other = registry.swep_info.find(key);
        if (other == registry.swep_info.end() || other->second.base != info->second.base) continue;
        std::size_t shared = 0;
        while (shared < key.size() && shared < view_key.size() && key[shared] == view_key[shared]) ++shared;
        if (shared <= best_shared) continue;
        auto files = catalog.fire_sounds(sound);
        if (files.empty()) continue;
        best = std::move(files);
        best_shared = shared;
        if (via) *via = key;
    }
    return best;
}

struct LoadedScene {
    std::vector<gmod::source::bsp::Triangle> triangles;
    std::optional<gmod::source::bsp::PlayerSpawnPoint> player_spawn;
    std::vector<WorldMaterialTexture> material_textures;
    PropRenderData prop_data;
    gmod::source::bsp::LightmapAtlas lightmap;
    std::optional<gmod::source::bsp::SkyCamera> sky_camera;
    std::optional<gmod::source::bsp::SkyPaint> sky_paint;
    std::optional<gmod::source::bsp::SunLight> sun;
    gmod::source::bsp::AudioScene audio;
    std::optional<gmod::source::bsp::BspProbe> probe;
    std::optional<gmod::demo::DemoData> demo;
    std::shared_ptr<gmod::source::model::PlayerAnimator> animator;
    std::shared_ptr<OutfitData> outfit;
    std::shared_ptr<DemoAudioAssets> demo_audio;
    std::vector<std::filesystem::path> content_roots;  // where sounds and models are looked up
    std::unordered_map<std::string, std::string> material_surfaces;
    std::set<int> moving_map_brushes;
};

std::vector<gmod::source::bsp::Triangle> collision_triangles(
    const std::vector<gmod::source::bsp::Triangle>& triangles, const std::set<int>& moving_brushes) {
    std::vector<gmod::source::bsp::Triangle> result;
    result.reserve(triangles.size());
    for (const auto& triangle : triangles)
        if (!moving_brushes.contains(triangle.map_brush)) result.push_back(triangle);
    return result;
}

struct RetainedMap {
    std::string key;
    std::optional<LoadedScene> scene;
    std::size_t reuses = 0;
    std::size_t revision = 0;
};

void transform_point(const Matrix4& matrix, const float point[3], float output[3]) {
    for (int axis = 0; axis < 3; ++axis) {
        output[axis] = point[0] * matrix.values[axis]
            + point[1] * matrix.values[4 + axis]
            + point[2] * matrix.values[8 + axis]
            + matrix.values[12 + axis];
    }
}

// Planes for a row-vector view-projection (clip = position * matrix, D3D depth 0..1).
std::array<std::array<float, 4>, 6> frustum_planes(const Matrix4& matrix) {
    auto column = [&](int index) {
        return std::array<float, 4>{
            matrix.values[index], matrix.values[4 + index],
            matrix.values[8 + index], matrix.values[12 + index]};
    };
    const auto c0 = column(0);
    const auto c1 = column(1);
    const auto c2 = column(2);
    const auto c3 = column(3);
    std::array<std::array<float, 4>, 6> planes{};
    for (int k = 0; k < 4; ++k) {
        planes[0][k] = c3[k] + c0[k];
        planes[1][k] = c3[k] - c0[k];
        planes[2][k] = c3[k] + c1[k];
        planes[3][k] = c3[k] - c1[k];
        planes[4][k] = c2[k];
        planes[5][k] = c3[k] - c2[k];
    }
    for (auto& plane : planes) {
        const float length = std::sqrt(
            plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
        if (length > 0.0f) {
            for (auto& value : plane) {
                value /= length;
            }
        }
    }
    return planes;
}

Matrix4 prop_transform(const float origin[3], const float angles[3]) {
    constexpr float degrees_to_radians = 0.01745329251994329577f;
    const float pitch = angles[0] * degrees_to_radians;
    const float yaw = angles[1] * degrees_to_radians;
    const float roll = angles[2] * degrees_to_radians;
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cr = std::cos(roll);
    const float sr = std::sin(roll);
    const float rotation[3][3] = {
        {cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr},
        {sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr},
        {-sp, cp * sr, cp * cr}};
    Matrix4 transform{};
    for (int source_axis = 0; source_axis < 3; ++source_axis) {
        transform.values[source_axis * 4] = -rotation[0][source_axis];
        transform.values[source_axis * 4 + 1] = rotation[2][source_axis];
        transform.values[source_axis * 4 + 2] = -rotation[1][source_axis];
    }
    transform.values[12] = -origin[0];
    transform.values[13] = origin[2];
    transform.values[14] = -origin[1];
    transform.values[15] = 1.0f;
    return transform;
}

Matrix4 prop_transform(const gmod::source::bsp::StaticProp& prop) {
    return prop_transform(prop.origin, prop.angles);
}

// Players whose model is not networked are drawn with the demo's most used one.
int fallback_player_model(const gmod::demo::DemoData& demo) {
    std::map<int, std::size_t> counts;
    for (const auto& player : demo.players) {
        if (player.npc) continue;
        for (const auto& sample : player.samples) {
            if (sample.model >= 0) {
                ++counts[sample.model];
            }
        }
    }
    int best = -1;
    std::size_t best_count = 0;
    for (const auto& [model, count] : counts) {
        if (count > best_count) {
            best = model;
            best_count = count;
        }
    }
    return best;
}

std::string normalize_clothing_model_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (path.ends_with(".mdl")) path.resize(path.size() - 4);
    return path;
}

bool is_female_player_model(std::string_view model_path) {
    return normalize_clothing_model_path(std::string(model_path)).find("female_")
        != std::string::npos;
}

// Third-person weapon models are named w_*, but the demo only reveals each weapon's
// first-person model (c_*). Returns an empty string when no usable model exists.
std::string find_weapon_world_model(
    const gmod::source::AssetResolver& assets,
    const gmod::demo::DemoData& demo,
    const gmod::demo::PerpheadsRegistry& registry,
    const std::string& weapon) {
    std::vector<std::string> candidates;
    const auto view = demo.weapon_view_models.find(weapon);
    if (view != demo.weapon_view_models.end()) {
        // The weapon's own script names the model it shows in other players' hands.
        const auto world = registry.swep_world_models.find(normalize_clothing_model_path(view->second));
        if (world != registry.swep_world_models.end()) {
            if (world->second.empty()) return {};
            candidates.push_back(world->second);
        }
    }
    if (view != demo.weapon_view_models.end()) {
        std::string path = view->second;
        std::replace(path.begin(), path.end(), '\\', '/');
        const auto slash = path.find_last_of('/');
        const std::string directory = slash == std::string::npos ? "" : path.substr(0, slash + 1);
        const std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
        if (file.size() > 2 && (file[0] == 'c' || file[0] == 'v') && file[1] == '_') {
            const std::string world = "w_" + file.substr(2);
            candidates.push_back(directory + world);
            // Melee weapons keep their first-person models in a C_Weapons subfolder.
            std::string lower = directory;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const auto sub = lower.find("c_weapons/");
            if (sub != std::string::npos) {
                candidates.push_back(directory.substr(0, sub) + world);
            }
        }
    }
    std::string suffix = weapon;
    for (const char* prefix : {"weapon_perp_phys_", "weapon_perp_", "weapon_", "roleplay_"}) {
        if (suffix.rfind(prefix, 0) == 0) {
            suffix.erase(0, std::strlen(prefix));
            break;
        }
    }
    candidates.push_back("models/perpheads/weapons/w_" + suffix + ".mdl");
    if (weapon == "weapon_physgun") candidates.push_back("models/weapons/w_physics.mdl");
    for (const auto& candidate : candidates) {
        const auto status = assets.resolve_model(candidate);
        if (status.has_mdl && status.has_vvd && status.has_vtx) return candidate;
    }
    return {};
}

// Rigid row-major 3x4 helpers (out = R * v + t).
std::array<float, 12> rigid_inverse(const float m[12]) {
    std::array<float, 12> out{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) out[row * 4 + column] = m[column * 4 + row];
    }
    for (int row = 0; row < 3; ++row) {
        out[row * 4 + 3] = -(out[row * 4] * m[3] + out[row * 4 + 1] * m[7] + out[row * 4 + 2] * m[11]);
    }
    return out;
}

// Applies `b` first, then `a`.
std::array<float, 12> rigid_compose(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> out{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            out[row * 4 + column] = a[row * 4] * b[column] + a[row * 4 + 1] * b[4 + column]
                + a[row * 4 + 2] * b[8 + column];
        }
        out[row * 4 + 3] = a[row * 4] * b[3] + a[row * 4 + 1] * b[7] + a[row * 4 + 2] * b[11]
            + a[row * 4 + 3];
    }
    return out;
}

// A bonemerged world model keeps its own hand bind pose, but it is skinned with the
// player's palette. Pre-transforming its vertices by (player bind * weapon inverse bind)
// puts the weapon where the engine's bonemerge would.
std::optional<std::array<float, 12>> weapon_hand_correction(
    const gmod::source::model::PlayerAnimator& animator, const std::filesystem::path& weapon_mdl,
    const std::optional<std::array<float, 12>>& hand_attachment = std::nullopt) {
    constexpr const char* HandBone = "valvebiped.bip01_r_hand";
    const auto find = [](const std::vector<gmod::source::model::Bone>& bones)
        -> const gmod::source::model::Bone* {
        for (const auto& bone : bones) {
            if (bone.name == HandBone) return &bone;
        }
        return nullptr;
    };
    const auto weapon_bones = gmod::source::model::read_bones(weapon_mdl);
    const auto* weapon_hand = find(weapon_bones);
    const auto* player_hand = find(animator.skeleton());
    if (!player_hand) return std::nullopt;
    const auto player_bind = rigid_inverse(player_hand->pose_to_bone);
    // A model with no hand bone of its own is held at the player model's right-hand
    // attachment (or the hand itself when the model has none).
    if (!weapon_hand) return hand_attachment ? *hand_attachment : player_bind;
    std::array<float, 12> weapon_inverse_bind{};
    std::copy(weapon_hand->pose_to_bone, weapon_hand->pose_to_bone + 12, weapon_inverse_bind.begin());
    return rigid_compose(player_bind, weapon_inverse_bind);
}

std::optional<std::array<float,12>> recorded_prop_matrix(
    const gmod::demo::DemoData& demo, std::size_t index, float time, int depth = 0) {
    if (depth >= 16 || index >= demo.props.size()) return std::nullopt;
    const auto& prop = demo.props[index];
    gmod::demo::PropPose pose;
    if (!gmod::demo::sample_prop(prop,time,pose)) return std::nullopt;
    const float origin[3]{pose.x,pose.y,pose.z};
    const float angles[3]{pose.pitch,pose.yaw,pose.roll};
    auto matrix = gmod::source::model::matrix_from_source_angles(origin,angles);
    const auto after = std::upper_bound(prop.keys.begin(),prop.keys.end(),time,
        [](float t,const auto& key) { return t < key.time; });
    int parent = -1;
    for (auto key = after; key != prop.keys.begin();) {
        --key;
        if (key->has_position) {
            parent = key->parent;
            // Missing network angles are unknown, not an instruction to erase the map rotation.
            if (!key->has_yaw && prop.map_rotation) {
                for (int row = 0; row < 3; ++row)
                    for (int column = 0; column < 3; ++column)
                        matrix[row*4+column] = (*prop.map_rotation)[row*4+column];
            }
            break;
        }
    }
    if (parent <= 0) return matrix;
    std::size_t parent_index = demo.props.size();
    for (std::size_t candidate = 0; candidate < demo.props.size(); ++candidate) {
        const auto& track = demo.props[candidate];
        if (track.entity != parent || track.keys.empty() || track.keys.front().time > time) continue;
        if (parent_index == demo.props.size()
            || track.keys.front().time > demo.props[parent_index].keys.front().time) parent_index = candidate;
    }
    const auto parent_matrix = recorded_prop_matrix(demo,parent_index,time,depth + 1);
    if (!parent_matrix) return std::nullopt;
    return rigid_compose(*parent_matrix,matrix);
}

Matrix4 prop_transform(const std::array<float,12>& source) {
    Matrix4 result{};
    for (int axis = 0; axis < 3; ++axis) {
        result.values[axis*4] = -source[axis];
        result.values[axis*4+1] = source[8+axis];
        result.values[axis*4+2] = -source[4+axis];
    }
    result.values[12] = -source[3];
    result.values[13] = source[11];
    result.values[14] = -source[7];
    result.values[15] = 1;
    return result;
}

Matrix4 matrix_multiply(const Matrix4& left, const Matrix4& right) {
    Matrix4 result{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            for (int index = 0; index < 4; ++index) {
                result.values[row * 4 + column] +=
                    left.values[row * 4 + index] * right.values[index * 4 + column];
            }
        }
    }
    return result;
}

Matrix4 perspective_matrix(float fov, float aspect, float near_plane, float far_plane) {
    Matrix4 result{};
    const float scale = 1.0f / std::tan(fov * 0.5f);
    result.values[0] = scale / aspect;
    result.values[5] = scale;
    result.values[10] = far_plane / (far_plane - near_plane);
    result.values[11] = 1.0f;
    result.values[14] = -near_plane * far_plane / (far_plane - near_plane);
    return result;
}

Matrix4 camera_view(float x, float y, float z, float yaw, float pitch) {
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float fx = cp * sy;
    const float fy = sp;
    const float fz = cp * cy;
    const float rx = cy;
    const float rz = -sy;
    const float ux = -sp * sy;
    const float uy = cp;
    const float uz = -sp * cy;
    Matrix4 result{};
    result.values[0] = rx;
    result.values[1] = ux;
    result.values[2] = fx;
    result.values[4] = 0.0f;
    result.values[5] = uy;
    result.values[6] = fy;
    result.values[8] = rz;
    result.values[9] = uz;
    result.values[10] = fz;
    result.values[12] = -(rx * x + rz * z);
    result.values[13] = -(ux * x + uy * y + uz * z);
    result.values[14] = -(fx * x + fy * y + fz * z);
    result.values[15] = 1.0f;
    return result;
}

void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::runtime_error(
            std::string(operation) + " failed: 0x" +
            std::to_string(static_cast<unsigned long>(result)));
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    }
    const bool double_click = message == WM_LBUTTONDBLCLK;
    if (double_click) message = WM_LBUTTONDOWN;
    // F10 (and a lone Alt) would otherwise put the window into Windows' modal menu-bar mode,
    // which stalls the frame loop and swallows keys until another key is pressed.
    if ((message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) && wparam == VK_F10) {
        return 0;
    }
    if (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_KEYMENU && lparam == 0) {
        return 0;
    }
    if (message == WM_SIZE && g_surface && wparam != SIZE_MINIMIZED) {
        MoveWindow(g_surface, 0, 0, LOWORD(lparam), HIWORD(lparam), FALSE);
    }
    if (g_menu) {
        const int x = static_cast<int>(static_cast<short>(LOWORD(lparam)));
        const int y = static_cast<int>(static_cast<short>(HIWORD(lparam)));
        bool repaint = false;
        switch (message) {
        case WM_PAINT:
            g_menu->paint(window);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE:
            repaint = g_menu->mouse_move(window, x, y);
            break;
        case WM_LBUTTONDOWN:
            repaint = g_menu->mouse_down(window, x, y);
            break;
        case WM_LBUTTONUP:
            repaint = g_menu->mouse_up(window, x, y);
            break;
        case WM_CAPTURECHANGED:
            repaint = g_menu->cancel_mouse_selection();
            break;
        case WM_MOUSEWHEEL: {
            // Wheel coordinates are in screen space; only the delta matters here.
            repaint = g_menu->mouse_wheel(window, GET_WHEEL_DELTA_WPARAM(wparam));
            break;
        }
        case WM_KEYDOWN:
            repaint = g_menu->key_down(window, wparam);
            break;
        case WM_TIMER:
            if (wparam == 74) {
                if (g_menu->poll_updates()) InvalidateRect(window, nullptr, FALSE);
                if (g_menu->finished()) PostMessageW(window, WM_NULL, 0, 0);
                return 0;
            }
            break;
        default:
            break;
        }
        if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP
            || message == WM_CAPTURECHANGED
            || message == WM_MOUSEWHEEL
            || message == WM_KEYDOWN) {
            if (repaint) {
                InvalidateRect(window, nullptr, FALSE);
            }
            if (g_menu->finished()) {
                PostMessageW(window, WM_NULL, 0, 0);
            }
            return 0;
        }
    }
    if (message == WM_PAINT && g_loading) {
        paint_loading_screen(window, *g_loading);
        return 0;
    }
    if (g_loading && (message == WM_LBUTTONDOWN || message == WM_KEYDOWN)) {
        const RECT button = loading_cancel_rect(window);
        const POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        if (g_loading->cancellable && ((message == WM_KEYDOWN && (wparam == VK_ESCAPE || wparam == VK_RETURN || wparam == VK_SPACE))
            || (message == WM_LBUTTONDOWN && PtInRect(&button, point)))) {
            g_loading->cancel = true;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    }
    if (message == WM_SIZE && wparam != SIZE_MINIMIZED && !g_loading && !g_menu) {
        g_resize_width = LOWORD(lparam);
        g_resize_height = HIWORD(lparam);
        g_resize_pending = g_resize_width > 0 && g_resize_height > 0;
    }
    if (message == WM_ERASEBKGND && g_loading) {
        return 1;
    }
    if (message == WM_DESTROY) {
        if (g_audio) g_audio->set_master_volume(0.0f);
        start_exit_watchdog();
        release_mouse();
        PostQuitMessage(0);
        return 0;
    }
    // Left click selects the player under the cursor (or under the crosshair while looking
    // around in free view); a click on empty space clears the selection. A press that
    // drags turns the camera instead, so the click is taken on release.
    if (message == WM_LBUTTONDOWN && !g_loading) {
        if (g_mouse_locked_out) return 0;
        g_press_double_click = double_click;
        if (g_free_view && !g_drag_look) {
            if (!g_mouse_captured) {
                capture_mouse(window);
            } else {
                RECT client{};
                GetClientRect(window, &client);
                g_click_at = {(client.right - client.left) / 2, (client.bottom - client.top) / 2};
                g_click_pending = true;
                g_follow_click_pending = double_click;
            }
        } else {
            SetCapture(window);
            g_dragging = true;
            g_press_moved = 0.0f;
            g_drag_last = {static_cast<int>(static_cast<short>(LOWORD(lparam))),
                           static_cast<int>(static_cast<short>(HIWORD(lparam)))};
        }
        return 0;
    }
    if (message == WM_RBUTTONDOWN && !g_loading && !g_menu) {
        if (g_mouse_locked_out) return 0;
        g_deselect_pending = true;
        return 0;
    }
    if (message == WM_MOUSEMOVE && g_dragging) {
        const POINT now{static_cast<int>(static_cast<short>(LOWORD(lparam))),
                        static_cast<int>(static_cast<short>(HIWORD(lparam)))};
        g_drag_dx += static_cast<float>(now.x - g_drag_last.x);
        g_drag_dy += static_cast<float>(now.y - g_drag_last.y);
        g_press_moved += static_cast<float>(std::abs(now.x - g_drag_last.x) + std::abs(now.y - g_drag_last.y));
        g_drag_last = now;
        return 0;
    }
    if ((message == WM_LBUTTONUP || message == WM_CAPTURECHANGED) && g_dragging) {
        g_dragging = false;
        if (message == WM_LBUTTONUP) {
            ReleaseCapture();
            if (g_press_moved < 6.0f) {
                g_click_at = {static_cast<int>(static_cast<short>(LOWORD(lparam))),
                              static_cast<int>(static_cast<short>(HIWORD(lparam)))};
                g_click_pending = true;
                g_follow_click_pending = g_press_double_click;
            }
        }
        return 0;
    }
    if (message == WM_KEYDOWN || message == WM_CHAR) {
        // The console key is the one left of "1" on every layout (scan code 0x29).
        const bool console_key = message == WM_KEYDOWN && ((lparam >> 16) & 0xFF) == 0x29;
        if (console_key && !(lparam & (1 << 30))) {
            g_console_key_pending = true;
            return 0;
        }
        if (g_console && g_console->visible()) {
            if (message == WM_CHAR) {
                g_console->on_char(static_cast<wchar_t>(wparam));
            } else {
                if (wparam == VK_ESCAPE) g_esc_consumed = true;
                g_console->on_key(wparam);
            }
            return 0;
        }
        // The character the console key types is never meant as text.
        if (message == WM_CHAR) return 0;
    }
    if (message == WM_MOUSEWHEEL && g_console && g_console->visible()) {
        g_console->scroll_by(GET_WHEEL_DELTA_WPARAM(wparam));
        return 0;
    }
    if (message == WM_MOUSEWHEEL && g_player_list && g_player_list->visible()) {
        g_player_list->scroll_by(GET_WHEEL_DELTA_WPARAM(wparam));
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
        release_mouse();
        return 0;
    }
    if (message == WM_ACTIVATEAPP && wparam == FALSE) {
        release_mouse();
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// The local time of day (seconds) a demo began. Perpheads names recordings after their
// start ("..._2026-10-17 22-02-06.dem"); other files fall back to their creation time.
int demo_start_time_of_day(const std::filesystem::path& path) {
    const std::wstring name = path.stem().wstring();
    const auto space = name.rfind(L' ');
    if (space != std::wstring::npos) {
        int hour = -1;
        int minute = -1;
        int second = -1;
        if (swscanf_s(name.c_str() + space + 1, L"%d-%d-%d", &hour, &minute, &second) == 3
            && hour >= 0 && hour < 24 && minute >= 0 && minute < 60 && second >= 0 && second < 60) {
            return hour * 3600 + minute * 60 + second;
        }
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return -1;
    FILETIME local{};
    SYSTEMTIME time{};
    if (!FileTimeToLocalFileTime(&data.ftCreationTime, &local) || !FileTimeToSystemTime(&local, &time)) {
        return -1;
    }
    return time.wHour * 3600 + time.wMinute * 60 + time.wSecond;
}

class Dx12Foundation {
public:
    explicit Dx12Foundation(
        HWND window,
        const std::vector<gmod::source::bsp::Triangle>& triangles,
        const std::vector<WorldMaterialTexture>& materials,
        const PropRenderData& prop_data,
        const gmod::source::bsp::LightmapAtlas& lightmap,
        const std::optional<gmod::source::bsp::SkyCamera>& sky_camera,
        const std::optional<gmod::source::bsp::SkyPaint>& sky_paint,
        const std::optional<gmod::source::bsp::SunLight>& sun,
        const std::optional<gmod::source::bsp::PlayerSpawnPoint>& spawn,
        bool vsync, gmod::dx12::GpuAssetCache* asset_cache = nullptr) {
        asset_cache_ = asset_cache;
        window_ = window;
        camera_world_ = std::make_unique<gmod::source::WorldTrace>(triangles,true);
        camera_transforms_ = prop_data.transforms;
        sun_ = sun;
        if (sky_camera) {
            sky_camera_ = *sky_camera;
        }
        if (sky_camera) {
            // Renderer axes are (-x, z, -y) of Source space.
            sky_origin_ = {-sky_camera->origin.x, sky_camera->origin.z, -sky_camera->origin.y};
            sky_scale_ = sky_camera->scale;
            has_sky_camera_ = true;
        }
        sky_paint_ = sky_paint;
        vsync_ = vsync;
        create_device();
        create_swap_chain(g_surface ? g_surface : window);
        create_render_targets();
        create_frame_resources();
        create_world_textures(materials, prop_data);
        create_lightmap_texture(lightmap);
        create_world_pipeline(triangles, materials, prop_data, spawn);
        map_camera_ = {initial_camera_x_, initial_camera_y_, initial_camera_z_,
            initial_camera_yaw_, initial_camera_pitch_};
        // Started last: a throwing constructor must not leave joinable threads.
        start_texture_decoders();
    }

    ~Dx12Foundation() {
        stop_texture_decoders();
        if (fence_event_) {
            wait_for_gpu();
            CloseHandle(fence_event_);
        }
    }

    void suspend_demo() {
        set_bank_lighting(initial_bank_weights());
        frame_text_painter_ = {};
        detach_audio();
        stop_texture_decoders();
        wait_for_gpu();
        effect_vertices_.clear();
        effect_draws_.clear();
        follow_.reset();
        follow_look_.reset();
        first_person_.reset();
        scope_fov_ = 0;
        outline_active_ = false;
        outline_through_walls_ = false;
        demo_status_.clear();
        water_demo_time_.reset();
        initial_camera_x_ = map_camera_[0];
        initial_camera_y_ = map_camera_[1];
        initial_camera_z_ = map_camera_[2];
        initial_camera_yaw_ = map_camera_[3];
        initial_camera_pitch_ = map_camera_[4];
        std::fill(dynamic_poses_.begin(), dynamic_poses_.end(), DynamicPose{});
        std::fill(palette_cpu_.begin(), palette_cpu_.end(), 0.0f);
        camera_initialized_ = mouse_initialized_ = was_mouse_captured_ = false;
        last_camera_time_ = {};
    }

    void bind_demo(const LoadedScene& scene) {
        suspend_demo();
        const bool keep_textures = world_materials_ == scene.material_textures
            && completed_requests_ == texture_requests_.size();
        dynamic_instances_.clear();
        prop_batches_.clear();
        outline_variants_.clear();
        camera_transforms_ = scene.prop_data.transforms;
        if (keep_textures) {
            prop_instances_ = scene.prop_data.instances;
            prop_variants_ = scene.prop_data.variants;
            for (std::size_t index = 0; index < prop_instances_.size(); ++index)
                if (prop_instances_[index].dynamic) dynamic_instances_.push_back(index);
        } else {
            texture_requests_.clear();
            decoded_.clear();
            decoded_bytes_ = completed_requests_ = 0;
            next_request_ = 0;
            stop_decoding_ = false;
            failed_world_materials_ = 0;
            scope_lens_material_ = UINT_MAX;
            create_world_textures(scene.material_textures, scene.prop_data);
        }
        for (auto& batch : world_batches_) {
            const auto material = std::find_if(scene.material_textures.begin(), scene.material_textures.end(),
                [&](const auto& item) { return item.material == batch.material; });
            if (material == scene.material_textures.end())
                throw std::runtime_error("Retained world material is missing from the new scene.");
            batch.texture_index = material_slot(material - scene.material_textures.begin());
        }
        create_prop_buffers(scene.material_textures, scene.prop_data);
        write_outline_view();
        write_scope_view();
        write_water_views();
        if (!keep_textures) start_texture_decoders();
        std::cout << "RETAINED_RENDERER_REUSE: world geometry, lightmaps, pipelines and swap chain unchanged\n";
    }

    void bind_map_brushes(const LoadedScene& scene) {
        for (auto& batch : world_batches_)
            batch.active = !scene.moving_map_brushes.contains(batch.map_brush);
        if (std::any_of(scene.triangles.begin(), scene.triangles.end(),
            [](const auto& triangle) { return triangle.map_brush >= 0; }))
            camera_world_ = std::make_unique<gmod::source::WorldTrace>(
                collision_triangles(scene.triangles, scene.moving_map_brushes), true);
        std::cout << "MAP_BRUSH_BIND recorded=" << scene.moving_map_brushes.size() << "\n";
    }

    ID3D12Resource* world_resource() const { return world_vertex_buffer_.Get(); }
    ID3D12Resource* map_prop_resource() const { return map_prop_vertex_buffer_.Get(); }
    bool demo_state_clean() const {
        return !audio_ && !follow_ && !follow_look_ && !frame_text_painter_ && !first_person_ && !water_demo_time_ && scope_fov_ == 0 && !outline_active_
            && effect_vertices_.empty() && effect_draws_.empty()
            && std::none_of(dynamic_poses_.begin(), dynamic_poses_.end(),
                [](const auto& pose) { return pose.active; })
            && std::all_of(palette_cpu_.begin(), palette_cpu_.end(), [](float value) { return value == 0; });
    }

    void render() {
        const UINT frame = swap_chain_->GetCurrentBackBufferIndex();
        wait_for_fence(frame_fence_values_[frame]);
        upload_effect_vertices(frame);
        stream_world_materials();
        update_camera(frame);
        cull_props(frame);
        last_rendered_frame_ = frame;
        command_allocators_[frame]->Reset();
        command_list_->Reset(command_allocators_[frame].Get(), nullptr);
        record_lightmap_updates(frame);

        D3D12_RESOURCE_BARRIER begin{};
        begin.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        begin.Transition.pResource = render_targets_[frame].Get();
        begin.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        begin.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        begin.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list_->ResourceBarrier(1, &begin);

        auto rtv = rtv_heap_.cpu(frame);
        const auto dsv = dsv_heap_.cpu();
        command_list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        constexpr float clear[] = {0.04f, 0.07f, 0.12f, 1.0f};
        command_list_->ClearRenderTargetView(rtv, clear, 0, nullptr);
        command_list_->ClearDepthStencilView(
            dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        command_list_->SetGraphicsRootSignature(root_signature_.Get());
        ID3D12DescriptorHeap* descriptor_heaps[] = {texture_heap_.get()};
        command_list_->SetDescriptorHeaps(1, descriptor_heaps);
        auto set_camera = [&](UINT pass) {
            command_list_->SetGraphicsRootConstantBufferView(
                0, camera_buffer_->GetGPUVirtualAddress() + camera_slot_offset(frame, pass));
        };
        set_camera(0);
        command_list_->SetGraphicsRootDescriptorTable(4,texture_heap_.gpu(water_table_slot()));
        command_list_->SetGraphicsRootDescriptorTable(
            2, texture_heap_.gpu(instance_table_slot(frame)));
        D3D12_VIEWPORT viewport{
            0.0f, 0.0f, static_cast<float>(width_),
            static_cast<float>(height_), 0.0f, 1.0f};
        D3D12_RECT scissor{
            0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
        command_list_->RSSetViewports(1, &viewport);
        command_list_->RSSetScissorRects(1, &scissor);
        command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        std::size_t draws = 0;
        std::uint64_t triangles = 0;
        bool rendering_scope = false;
        bool rendering_reflection = false;
        int water_filter = 0; // 0 all, 1 skip water, 2 water only
        const auto water_matches = [&](UINT flags) {
            return water_filter==0 || ((flags & 4u)!=0)==(water_filter==2);
        };
        bool rendering_view_model = false;
        auto draw_pass = [&](ID3D12PipelineState* pipeline, UINT first_mode, UINT last_mode,
                             bool sky) {
            command_list_->SetPipelineState(pipeline);
            command_list_->IASetVertexBuffers(0, 1, &world_vertex_view_);
            for (const auto& batch : world_batches_) {
                if (rendering_view_model) continue;
                if (!water_matches(batch.material_flags)) continue;
                if (!batch.active || batch.sky != sky || batch.alpha_mode < first_mode
                    || batch.alpha_mode > last_mode) {
                    continue;
                }
                const DrawConstants constants{
                    0, 0, batch.alpha_mode, batch.alpha_ref, batch.material_flags};
                command_list_->SetGraphicsRoot32BitConstants(
                    3, DrawConstantCount, &constants, 0);
                command_list_->SetGraphicsRootDescriptorTable(
                    1, texture_heap_.gpu(batch.texture_index));
                command_list_->DrawInstanced(
                    batch.vertex_count, 1, batch.vertex_offset, 0);
                ++draws;
                triangles += batch.vertex_count / 3;
            }
            if (prop_batches_.empty()) {
                return;
            }
            command_list_->IASetVertexBuffers(0, 1, &prop_vertex_view_);
            command_list_->IASetIndexBuffer(&prop_index_view_);
            for (const auto& batch : prop_batches_) {
                if (!water_matches(batch.material_flags)) continue;
                if (batch.alpha_mode < first_mode || batch.alpha_mode > last_mode) {
                    continue;
                }
                if (prop_variants_[batch.variant].first_person != rendering_view_model) continue;
                const auto [start, count] = variant_visible_[(rendering_reflection ? 4 : rendering_scope ? 2 : 0)+(sky ? 1 : 0)][batch.variant];
                if (count == 0) {
                    continue;
                }
                const bool active_lens = !rendering_scope && scope_fov_ > 0
                    && prop_variants_[batch.variant].first_person
                    && batch.texture_index == scope_lens_material_;
                command_list_->IASetVertexBuffers(0, 1,
                    batch.map_geometry ? &map_prop_vertex_view_ : &prop_vertex_view_);
                command_list_->IASetIndexBuffer(batch.map_geometry ? &map_prop_index_view_ : &prop_index_view_);
                const DrawConstants constants{
                    start, 1, batch.alpha_mode, batch.alpha_ref,
                    batch.material_flags | (active_lens ? 64u : 0u),
                    batch.skin_base_vertex};
                command_list_->SetGraphicsRoot32BitConstants(
                    3, DrawConstantCount, &constants, 0);
                command_list_->SetGraphicsRootDescriptorTable(
                    1, texture_heap_.gpu(active_lens ? scope_table_slot() : batch.texture_index));
                command_list_->DrawIndexedInstanced(
                    batch.index_count, count, batch.index_offset, batch.base_vertex, 0);
                ++draws;
                triangles += static_cast<std::uint64_t>(batch.index_count / 3) * count;
            }
        };
        auto draw_scene = [&](bool sky) {
            draw_pass(pipeline_state_.Get(), 0, 1, sky);
            draw_pass(blend_pipeline_state_.Get(), 2, 2, sky);
            draw_pass(additive_pipeline_state_.Get(), 3, 3, sky);
        };
        if (reflection_active_) {
            rendering_reflection = true;
            water_filter = 1;
            // No recursive reflection, and no reflection SRV bound while writing its RTV.
            command_list_->SetGraphicsRootDescriptorTable(4,texture_heap_.gpu(0));
            auto barrier = begin;
            barrier.Transition.pResource = water_reflection_.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            command_list_->ResourceBarrier(1,&barrier);
            const auto reflection_rtv = rtv_heap_.cpu(FrameCount+3);
            const auto reflection_dsv = dsv_heap_.cpu(2);
            command_list_->OMSetRenderTargets(1,&reflection_rtv,FALSE,&reflection_dsv);
            command_list_->ClearRenderTargetView(reflection_rtv,clear,0,nullptr);
            command_list_->ClearDepthStencilView(reflection_dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
            const D3D12_VIEWPORT reflection_viewport{0,0,static_cast<float>(std::max<UINT>(1,width_/2)),
                static_cast<float>(std::max<UINT>(1,height_/2)),0,1};
            const D3D12_RECT reflection_scissor{0,0,static_cast<LONG>(std::max<UINT>(1,width_/2)),
                static_cast<LONG>(std::max<UINT>(1,height_/2))};
            command_list_->RSSetViewports(1,&reflection_viewport);
            command_list_->RSSetScissorRects(1,&reflection_scissor);
            set_camera(4);
            if (sky_pipeline_state_) {
                command_list_->SetPipelineState(sky_pipeline_state_.Get());
                command_list_->DrawInstanced(3,1,0,0);
            }
            if (has_sky_camera_) {
                set_camera(5);
                draw_scene(true);
                command_list_->ClearDepthStencilView(reflection_dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
                set_camera(4);
            }
            draw_scene(false);
            std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
            command_list_->ResourceBarrier(1,&barrier);
            rendering_reflection = false;
            water_filter = 0;
            command_list_->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
            command_list_->RSSetViewports(1,&viewport);
            command_list_->RSSetScissorRects(1,&scissor);
            set_camera(0);
            command_list_->SetGraphicsRootDescriptorTable(4,texture_heap_.gpu(water_table_slot()));
        }
        if (scope_fov_ > 0 && scope_color_) {
            rendering_scope = true;
            auto barrier = begin;
            barrier.Transition.pResource = scope_color_.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            command_list_->ResourceBarrier(1,&barrier);
            const auto scope_rtv = rtv_heap_.cpu(FrameCount+1);
            const auto scope_dsv = dsv_heap_.cpu(1);
            command_list_->OMSetRenderTargets(1,&scope_rtv,FALSE,&scope_dsv);
            command_list_->ClearRenderTargetView(scope_rtv,clear,0,nullptr);
            command_list_->ClearDepthStencilView(scope_dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
            const D3D12_VIEWPORT scope_viewport{0,0,512,512,0,1};
            const D3D12_RECT scope_scissor{0,0,512,512};
            command_list_->RSSetViewports(1,&scope_viewport);
            command_list_->RSSetScissorRects(1,&scope_scissor);
            set_camera(2);
            if (sky_pipeline_state_) {
                command_list_->SetPipelineState(sky_pipeline_state_.Get());
                command_list_->DrawInstanced(3,1,0,0);
            }
            if (has_sky_camera_) {
                set_camera(3);
                draw_scene(true);
                command_list_->ClearDepthStencilView(scope_dsv,D3D12_CLEAR_FLAG_DEPTH,1,0,0,nullptr);
                set_camera(2);
            }
            draw_scene(false);
            draw_combat_effects(frame);
            if (outline_active_ && outline_mask_pipeline_ && !prop_batches_.empty())
                draw_outlines(scope_rtv, scope_dsv, draws, triangles, true);
            std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
            command_list_->ResourceBarrier(1,&barrier);
            rendering_scope = false;
            command_list_->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
            command_list_->RSSetViewports(1,&viewport);
            command_list_->RSSetScissorRects(1,&scissor);
            set_camera(0);
        }
        if (sky_pipeline_state_) {
            // Procedural env_skypaint background behind everything.
            command_list_->SetPipelineState(sky_pipeline_state_.Get());
            command_list_->DrawInstanced(3, 1, 0, 0);
        }
        if (has_sky_camera_) {
            // The 3D skybox is drawn first with its own camera, then depth is
            // cleared so the real map always covers it.
            set_camera(1);
            draw_scene(true);
            command_list_->ClearDepthStencilView(
                dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            set_camera(0);
        }
        water_filter = 1;
        draw_scene(false);
        if (!water_planes_.empty()) {
            copy_water_background(frame);
            water_filter = 2;
            draw_pass(water_pipeline_state_.Get(),2,2,false);
        }
        water_filter = 0;
        draw_combat_effects(frame);
        if (outline_active_ && outline_mask_ && outline_mask_pipeline_ && !prop_batches_.empty()) {
            draw_outlines(rtv, dsv, draws, triangles);
        }
        rendering_view_model = true;
        draw_scene(false);

        // Labels belong to the swap-chain image so game capture sees them with
        // the scene/halos. Paint after updating this frame's camera projection.
        if (frame_text_painter_ && frame_text_ && frame_text_->draw(command_list_.Get(),frame,rtv,frame_text_painter_)) {
            ++draws; ++triangles;
        }

        auto end = begin;
        end.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        end.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        command_list_->ResourceBarrier(1, &end);
        command_list_->Close();

        ID3D12CommandList* lists[] = {command_list_.Get()};
        command_queue_->ExecuteCommandLists(1, lists);
        const HRESULT present = swap_chain_->Present(vsync_ ? 1 : 0, 0);
        if (present == DXGI_ERROR_DEVICE_REMOVED || present == DXGI_ERROR_DEVICE_RESET) {
            const HRESULT reason = device_->GetDeviceRemovedReason();
            throw std::runtime_error(
                "The GPU device was lost (driver reset or out of video memory). Reason: 0x"
                + std::to_string(static_cast<unsigned long>(reason)));
        }
        check_hresult(present, "Present");
        ++fence_value_;
        check_hresult(command_queue_->Signal(fence_.Get(), fence_value_), "Signal frame fence");
        frame_fence_values_[frame] = fence_value_;
        update_stats(draws, triangles);
    }

    void set_dynamic_pose(std::size_t slot, const DynamicPose& pose) {
        if (slot < dynamic_poses_.size()) {
            dynamic_poses_[slot] = pose;
        }
    }

    const WorldMaterialTexture* effect_material(const std::string& name) const {
        const auto found = std::find_if(world_materials_.begin(), world_materials_.end(),
            [&](const auto& material) { return material.material == name; });
        return found == world_materials_.end() ? nullptr : &*found;
    }

    void set_combat_effects(std::vector<EffectVertex> vertices, std::vector<EffectDraw> draws) {
        effect_vertices_ = std::move(vertices);
        effect_draws_ = std::move(draws);
    }

    // Called once per recorded fist attack while building its sound timeline.
    // Static prop meshes are needed here: tree trunks are not BSP world faces.
    float fist_static_fraction(const std::array<float,3>& start,const std::array<float,3>& end,bool& prop_hit) const {
        constexpr float radius=6.0f;
        float fraction=camera_world_->sweep_sphere(start.data(),end.data(),radius);
        prop_hit=false;
        const std::array<float,3> render_start{-start[0],start[2],-start[1]},render_end{-end[0],end[2],-end[1]};
        for(const auto& variant:prop_variants_) {
            if(!variant.camera_trace) continue;
            for(UINT offset=0;offset<variant.instance_count;++offset) {
                const auto& instance=prop_instances_[variant.first_instance+offset];
                if(instance.sky || instance.dynamic || !instance.active) continue;
                const auto& matrix=camera_transforms_[instance.transform];
                float center[3];transform_point(matrix,instance.local_center,center);
                float distance2=0;
                for(int axis=0;axis<3;++axis) distance2+=(center[axis]-render_start[axis])*(center[axis]-render_start[axis]);
                if(distance2>(instance.radius+56)*(instance.radius+56)) continue;
                const auto local=[&](const std::array<float,3>& point) {
                    std::array<float,3> result{};
                    for(int axis=0;axis<3;++axis) for(int row=0;row<3;++row)
                        result[axis]+=matrix.values[axis*4+row]*(point[row]-matrix.values[12+row]);
                    return result;
                };
                const auto a=local(render_start),b=local(render_end);
                const float hit=variant.camera_trace->sweep_sphere(a.data(),b.data(),radius);
                if(hit<fraction) {fraction=hit;prop_hit=true;}
            }
        }
        return fraction;
    }

    std::array<float,3> skinned_point(std::size_t palette, int bone,
                                     const std::array<float,3>& point) const {
        const auto offset = (palette*MaxPaletteBones + static_cast<std::size_t>(bone))*12;
        if (bone < 0 || offset+12 > palette_cpu_.size()) throw std::runtime_error("Invalid muzzle bone palette.");
        const float* m = palette_cpu_.data()+offset;
        return {m[0]*point[0]+m[1]*point[1]+m[2]*point[2]+m[3],
                m[4]*point[0]+m[5]*point[1]+m[6]*point[2]+m[7],
                m[8]*point[0]+m[9]*point[1]+m[10]*point[2]+m[11]};
    }
    std::array<float,12> skinned_transform(std::size_t palette,int bone,const std::array<float,12>& bind) const {
        const auto offset=(palette*MaxPaletteBones+static_cast<std::size_t>(bone))*12;
        if(bone<0 || offset+12>palette_cpu_.size()) throw std::runtime_error("Invalid chest palette.");
        std::array<float,12> skin{};std::copy_n(palette_cpu_.data()+offset,12,skin.begin());
        return rigid_compose(skin,bind);
    }
    // Outlines are drawn for dynamic poses carrying an outline colour (ambient face 5 alpha).
    void set_outline(bool active, bool through_walls) {
        outline_active_ = active;
        outline_through_walls_ = through_walls;
    }

    void draw_outlines(D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                       D3D12_CPU_DESCRIPTOR_HANDLE dsv, std::size_t& draws,
                       std::uint64_t& triangles, bool scope = false) {
        if (outline_variants_.size() != prop_variants_.size()) {
            outline_variants_.assign(prop_variants_.size(), 0);
            for (std::size_t index = 0; index < prop_variants_.size(); ++index) {
                const auto& variant = prop_variants_[index];
                for (UINT offset = 0; offset < variant.instance_count; ++offset) {
                    if (prop_instances_[variant.first_instance + offset].dynamic) {
                        outline_variants_[index] = 1;
                        break;
                    }
                }
            }
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = scope ? scope_outline_mask_.Get() : outline_mask_.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list_->ResourceBarrier(1, &barrier);
        const auto mask_rtv = rtv_heap_.cpu(scope ? FrameCount + 2 : FrameCount);
        constexpr float none[] = {0.0f, 0.0f, 0.0f, 0.0f};
        command_list_->ClearRenderTargetView(mask_rtv, none, 0, nullptr);
        command_list_->OMSetRenderTargets(1, &mask_rtv, FALSE, &dsv);
        command_list_->SetPipelineState(outline_through_walls_ ? outline_xray_pipeline_.Get()
                                                               : outline_mask_pipeline_.Get());
        command_list_->IASetVertexBuffers(0, 1, &prop_vertex_view_);
        command_list_->IASetIndexBuffer(&prop_index_view_);
        for (const auto& batch : prop_batches_) {
            if (batch.variant >= outline_variants_.size() || !outline_variants_[batch.variant]) {
                continue;
            }
            if (prop_variants_[batch.variant].first_person) continue;
            const auto [start, count] = variant_visible_[scope ? 2 : 0][batch.variant];
            if (count == 0) {
                continue;
            }
            const DrawConstants constants{
                start, 1, batch.alpha_mode, batch.alpha_ref,
                batch.material_flags | 0x80000000u, batch.skin_base_vertex};
            command_list_->IASetVertexBuffers(0, 1,
                batch.map_geometry ? &map_prop_vertex_view_ : &prop_vertex_view_);
            command_list_->IASetIndexBuffer(batch.map_geometry ? &map_prop_index_view_ : &prop_index_view_);
            command_list_->SetGraphicsRoot32BitConstants(3, DrawConstantCount, &constants, 0);
            command_list_->SetGraphicsRootDescriptorTable(1, texture_heap_.gpu(batch.texture_index));
            command_list_->DrawIndexedInstanced(
                batch.index_count, count, batch.index_offset, batch.base_vertex, 0);
            ++draws;
            triangles += static_cast<std::uint64_t>(batch.index_count / 3) * count;
        }
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        command_list_->ResourceBarrier(1, &barrier);
        command_list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        command_list_->SetPipelineState(outline_composite_pipeline_.Get());
        command_list_->SetGraphicsRootDescriptorTable(1, texture_heap_.gpu(
            scope ? scope_outline_table_slot() : outline_table_slot()));
        command_list_->DrawInstanced(3, 1, 0, 0);
        ++draws;
    }

    // Skinning matrices (3x4, row-major) for one player; at most MaxPaletteBones.
    void set_bone_palette(std::size_t player, const std::vector<std::array<float, 12>>& matrices) {
        const auto first = player * MaxPaletteBones * 12;
        if (matrices.size() > MaxPaletteBones || first + matrices.size() * 12 > palette_cpu_.size()) {
            return;
        }
        for (std::size_t bone = 0; bone < matrices.size(); ++bone) {
            std::copy(matrices[bone].begin(), matrices[bone].end(),
                      palette_cpu_.begin() + static_cast<std::ptrdiff_t>(first + bone * 12));
        }
    }

    // Text shown in the window title next to the frame statistics.
    void set_demo_status(std::wstring text) { demo_status_ = std::move(text); }
    void set_water_time(float time) { water_demo_time_ = time; }
    void set_water_probe(const std::optional<gmod::source::bsp::BspProbe>& probe) { water_probe_ = probe; }

    // Source-space point the camera should chase; none = free camera.
    void set_follow(std::optional<std::array<float, 3>> source_position, float height = 60.0f,
                    std::optional<std::array<float,2>> look = std::nullopt) {
        follow_ = source_position;
        follow_height_ = height;
        follow_look_ = look;
    }
    bool following() const { return follow_.has_value(); }
    const Matrix4& view_projection() const noexcept { return view_projection_; }
    const Matrix4& scope_view_projection() const noexcept { return scope_view_projection_; }
    gmod::ui::ScopeProjection scope_overlay() const {
        gmod::ui::ScopeProjection result;
        if (scope_fov_ <= 0) return result;
        for (const auto& mesh : scope_meshes_) {
            const auto slot = mesh.transform & DynamicSlotMask;
            if (slot >= dynamic_poses_.size() || !dynamic_poses_[slot].active) continue;
            const auto& transform = dynamic_poses_[slot].transform;
            const auto palette = (mesh.transform >> PaletteShift) & PaletteMask;
            for (std::size_t first = 0; first + 2 < mesh.vertices.size(); first += 3) {
                std::array<gmod::ui::ScopeVertex, 3> triangle;
                bool valid = true;
                for (int corner = 0; corner < 3; ++corner) {
                    const auto& vertex = mesh.vertices[first + corner];
                    float local[3];
                    std::copy_n(vertex.vertex.position, 3, local);
                    const auto count = vertex.skin[0] >> 24;
                    if (palette > 0 && count > 0) {
                        std::fill_n(local, 3, 0.0f);
                        const float weights[3]{(vertex.skin[1] & 0xffffu)/65535.0f,
                            (vertex.skin[1] >> 16)/65535.0f,
                            std::max(0.0f, 1.0f-(vertex.skin[1] & 0xffffu)/65535.0f-(vertex.skin[1] >> 16)/65535.0f)};
                        for (UINT i = 0; i < std::min(count, 3u); ++i) {
                            const auto bone = (vertex.skin[0] >> (8*i)) & 0xffu;
                            const auto offset = ((palette-1)*MaxPaletteBones + bone)*12;
                            if (offset + 12 > palette_cpu_.size()) { valid = false; break; }
                            for (int axis = 0; axis < 3; ++axis) {
                                const auto* row = palette_cpu_.data() + offset + axis*4;
                                local[axis] += weights[i]*(row[0]*vertex.vertex.position[0]
                                    + row[1]*vertex.vertex.position[1] + row[2]*vertex.vertex.position[2] + row[3]);
                            }
                        }
                    }
                    float world[3];
                    transform_point(transform, local, world);
                    for (int axis = 0; axis < 4; ++axis)
                        triangle[corner].clip[axis] = world[0]*view_projection_.values[axis]
                            + world[1]*view_projection_.values[4+axis]
                            + world[2]*view_projection_.values[8+axis] + view_projection_.values[12+axis];
                    triangle[corner].uv = {vertex.vertex.uv[0], vertex.vertex.uv[1]};
                    valid &= triangle[corner].clip[3] > 0.001f;
                }
                if (valid) result.triangles.push_back(triangle);
            }
        }
        return result;
    }
    // Camera position in Source space: where the viewer hears from.
    std::array<float, 3> listener_source() const noexcept {
        if (first_person_) return {first_person_->x, first_person_->y, first_person_->z};
        return {-camera_x_, -camera_z_, camera_y_};
    }

    // A recorded eye position and view angles (Source space, degrees); when set it
    // replaces the free or chase camera so the scene is seen through the player's eyes.
    struct FirstPersonView {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float yaw_degrees = 0.0f;
        float pitch_degrees = 0.0f;
    };
    void set_first_person(std::optional<FirstPersonView> view) { first_person_ = view; }
    void attach_audio(std::unique_ptr<gmod::audio::AudioSystem> audio) {
        audio_ = std::move(audio);
    }
    gmod::audio::AudioSystem* audio() { return audio_.get(); }
    void detach_audio() { audio_.reset(); }

    void set_scope_magnification(float magnification) {
        const float fov = magnification > 0 ? (90.0f/(3.0f*magnification))*0.01745329251994329577f : 0;
        if (scope_fov_ != fov && fov > 0)
            std::cout << "SCOPE_LENS magnification=" << magnification << " fov_degrees="
                << 90.0f/(3.0f*magnification) << " material_slot=" << scope_lens_material_ << "\n";
        scope_fov_ = fov;
    }
    float field_of_view() const {
        return 1.05f;
    }

    void finish_world_material_stream() {
        while (completed_requests_ < texture_requests_.size()) {
            const auto before = completed_requests_;
            stream_world_materials(-1.0);
            if (completed_requests_ == before) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    }

    // Source-space position and angles (degrees; positive pitch looks down).
    void set_camera(float x, float y, float z, float yaw_degrees, float pitch_degrees) {
        constexpr float degrees_to_radians = 0.01745329251994329577f;
        initial_camera_x_ = -x;
        initial_camera_y_ = z;
        initial_camera_z_ = -y;
        initial_camera_yaw_ = std::atan2(
            -std::cos(yaw_degrees * degrees_to_radians),
            -std::sin(yaw_degrees * degrees_to_radians));
        initial_camera_pitch_ = -pitch_degrees * degrees_to_radians;
        camera_initialized_ = false;
    }

    std::array<int,4> bank_styles() const {
        return styled_lightmaps_ ? styled_lightmaps_->bank_styles : std::array<int,4>{-1,-1,-1,-1};
    }
    gmod::source::bsp::BankLightWeights initial_bank_weights() const {
        gmod::source::bsp::BankLightWeights weights{};
        if(styled_lightmaps_) for(int slot=0;slot<4;++slot) {
            const int style=styled_lightmaps_->bank_styles[slot];
            if(style>=0) weights[slot]=styled_lightmaps_->initial[style];
        }
        return weights;
    }
    void set_bank_lighting(const gmod::source::bsp::BankLightWeights& weights) {
        if(!styled_lightmaps_ || desired_bank_weights_==weights) return;
        desired_bank_weights_=weights;
        desired_light_styles_=styled_lightmaps_->initial;
        for(int slot=0;slot<4;++slot) if(styled_lightmaps_->bank_styles[slot]>=0)
            desired_light_styles_[styled_lightmaps_->bank_styles[slot]]=weights[slot];
    }
    std::size_t bank_lightmap_uploads() const { return bank_lightmap_uploads_; }
    bool water_reflections_active() const noexcept { return reflection_active_; }
    UINT width() const noexcept { return width_; }
    UINT height() const noexcept { return height_; }
    void set_frame_text_painter(gmod::dx12::FrameTextOverlay::Painter painter) {
        if (painter && !frame_text_) frame_text_=std::make_unique<gmod::dx12::FrameTextOverlay>(device_.Get(),width_,height_,FrameCount);
        frame_text_painter_=std::move(painter);
    }

    // Rebuilds the swap chain and depth buffer for a new client size. The aspect ratio of the
    // projection follows, so the picture is re-framed rather than stretched.
    void resize(UINT width, UINT height) {
        if (!swap_chain_ || width == 0 || height == 0 || (width == width_ && height == height_)) return;
        wait_for_gpu();
        for (auto& target : render_targets_) target.Reset();
        depth_buffer_.Reset();
        outline_mask_.Reset();
        check_hresult(swap_chain_->ResizeBuffers(FrameCount, width, height,
                                                 DXGI_FORMAT_R8G8B8A8_UNORM, 0),
                      "Resize swap chain");
        width_ = width;
        height_ = height;
        create_size_dependent_targets();
        if (frame_text_) frame_text_->resize(width_,height_);
    }

    void test_water_resize() {
        const UINT original_width = width_, original_height = height_;
        resize(original_width+63,original_height+37);
        render();
        if (!reflection_active_) throw std::runtime_error("Water render test needs visible water above its surface.");
        resize(original_width,original_height);
        render();
        wait_for_gpu();
        for (UINT pass : {4u,5u}) {
            const float* constants = reinterpret_cast<const float*>(camera_mapped_ +
                camera_slot_offset(last_rendered_frame_,pass) + sizeof(Matrix4));
            const float scale = pass==5 ? sky_scale_ : 1.0f;
            const float plane = pass==5 ? sky_origin_.y+water_height_/scale : water_height_;
            if (constants[61]*(plane-1.0f/scale)+constants[63]>=0 ||
                constants[61]*(plane+1.0f/scale)+constants[63]<=0)
                throw std::runtime_error("Reflected world/skybox must clip terrain below the waterline.");
        }
        std::cout << "PASS: water reflection/refraction targets survived odd-size resize and restoration, plane="
                  << water_height_ << "\n";
    }

    void capture_bmp(const std::wstring& path) {
        capture_resource_bmp(render_targets_[last_rendered_frame_].Get(),D3D12_RESOURCE_STATE_PRESENT,path);
    }

    void capture_water_debug(const std::wstring& path) {
        const std::filesystem::path output(path);
        const auto stem = output.parent_path()/output.stem();
        capture_resource_bmp(water_reflection_.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            stem.wstring()+L"_reflection.bmp");
        capture_resource_bmp(water_scene_.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            stem.wstring()+L"_refraction.bmp");
    }

    void capture_resource_bmp(ID3D12Resource* resource, D3D12_RESOURCE_STATES state, const std::wstring& path) {
        wait_for_gpu();
        const UINT frame = last_rendered_frame_;
        const auto description = resource->GetDesc();
        UINT64 upload_size = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        UINT64 row_size = 0;
        device_->GetCopyableFootprints(
            &description, 0, 1, 0, &footprint, &rows, &row_size, &upload_size);

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = upload_size;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        check_hresult(device_->CreateCommittedResource(
                          &heap, D3D12_HEAP_FLAG_NONE, &buffer,
                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                          IID_PPV_ARGS(&readback)),
                      "Create screenshot readback");

        command_allocators_[frame]->Reset();
        command_list_->Reset(command_allocators_[frame].Get(), nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = state;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list_->ResourceBarrier(1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = resource;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        command_list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = state;
        command_list_->ResourceBarrier(1, &barrier);
        command_list_->Close();
        ID3D12CommandList* lists[] = {command_list_.Get()};
        command_queue_->ExecuteCommandLists(1, lists);
        wait_for_gpu();

        void* mapped = nullptr;
        D3D12_RANGE range{0, static_cast<SIZE_T>(upload_size)};
        check_hresult(readback->Map(0, &range, &mapped), "Map screenshot readback");
        write_bmp(path, description.Width, description.Height, mapped,
                  footprint.Footprint.RowPitch);
        readback->Unmap(0, nullptr);
    }

private:
    static constexpr UINT64 CameraSlotSize = 512;
    // Main/skybox, scope-lens, and reflected main/skybox camera pairs.
    static constexpr UINT CameraSlotsPerFrame = 6;
    // Per material: base, $basetexture2, $blendmodulatetexture (slots 0..2 = fallback).
    static constexpr std::size_t MaterialSlotCount = 3;

    static UINT material_slot(std::size_t material_index) {
        return static_cast<UINT>(MaterialSlotCount * (material_index + 1));
    }

    // Descriptor slots: material triples, then per frame one table of
    // {static transforms t1, visible instance indices t2, prop ambient cubes t6,
    // lightmap atlas t7, dynamic transforms t8, dynamic ambient cubes t9,
    // per-vertex skin data t10, player bone palettes t11}.
    static constexpr std::size_t InstanceTableSize = 8;
    UINT instance_table_slot(UINT frame) const {
        return static_cast<UINT>(
            MaterialSlotCount * (world_materials_.size() + 1) + frame * InstanceTableSize);
    }
    // After the per-frame tables: the outline mask (one material-sized table).
    UINT outline_table_slot() const { return instance_table_slot(FrameCount); }
    UINT scope_table_slot() const { return outline_table_slot()+MaterialSlotCount; }
    UINT scope_outline_table_slot() const { return scope_table_slot()+MaterialSlotCount; }
    UINT water_table_slot() const { return scope_outline_table_slot()+MaterialSlotCount; }

    void wait_for_fence(UINT64 value) {
        if (value != 0 && fence_->GetCompletedValue() < value) {
            check_hresult(fence_->SetEventOnCompletion(value, fence_event_),
                          "Set frame fence event");
            WaitForSingleObject(fence_event_, INFINITE);
        }
    }

    // Copies static data into a GPU-local (DEFAULT heap) buffer.
    ComPtr<ID3D12Resource> create_static_buffer(
        const void* data, UINT64 size, D3D12_RESOURCE_STATES final_state, bool reusable = false) {
        std::string cache_key;
        if (asset_cache_ && reusable) {
            cache_key = gmod::dx12::GpuAssetCache::content_key(data, static_cast<std::size_t>(size),
                "buffer:" + std::to_string(size) + ":" + std::to_string(final_state));
            if (auto cached = asset_cache_->find(cache_key)) return cached;
        }
        auto buffer = gmod::dx12::create_buffer(
            device_.Get(), size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        auto upload = gmod::dx12::create_buffer(
            device_.Get(), size, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        D3D12_RANGE range{0, 0};
        check_hresult(upload->Map(0, &range, &mapped), "Map static buffer upload");
        std::memcpy(mapped, data, static_cast<std::size_t>(size));
        upload->Unmap(0, nullptr);
        upload_allocator_->Reset();
        command_list_->Reset(upload_allocator_.Get(), nullptr);
        command_list_->CopyBufferRegion(buffer.Get(), 0, upload.Get(), 0, size);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = buffer.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = final_state;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list_->ResourceBarrier(1, &barrier);
        command_list_->Close();
        ID3D12CommandList* lists[] = {command_list_.Get()};
        command_queue_->ExecuteCommandLists(1, lists);
        wait_for_gpu();
        if (!cache_key.empty()) asset_cache_->store(cache_key, buffer);
        return buffer;
    }

    void cull_props(UINT frame) {
        for (auto& pass : variant_visible_) {
            pass.assign(prop_variants_.size(), {0u, 0u});
        }
        stats_visible_instances_ = 0;
        if (prop_instances_.empty()) {
            return;
        }
        apply_dynamic_poses(frame);
        UINT cursor = 0;
        cull_pass(frame, false, view_projection_,
                  {camera_x_, camera_y_, camera_z_}, cursor);
        if (has_sky_camera_) {
            cull_pass(frame, true, sky_view_projection_, sky_view_position_, cursor);
        }
        stats_visible_instances_ = cursor;
        if (reflection_active_) {
            const Float3 eye{camera_x_,2*water_height_-camera_y_,camera_z_};
            cull_pass(frame,false,reflected_view_projection_,eye,cursor,4);
            if (has_sky_camera_) {
                const Float3 sky_eye{sky_view_position_.x,2*(sky_origin_.y+water_height_/sky_scale_)-sky_view_position_.y,sky_view_position_.z};
                cull_pass(frame,true,reflected_sky_projection_,sky_eye,cursor,4);
            }
        }
        if (scope_fov_ > 0) {
            cull_pass(frame,false,scope_view_projection_,{camera_x_,camera_y_,camera_z_},cursor,2);
            if (has_sky_camera_)
                cull_pass(frame,true,scope_sky_projection_,sky_view_position_,cursor,2);
        }
    }

    // Copies this frame's demo-actor poses into the GPU-visible tables and
    // refreshes each instance's culling sphere.
    void apply_dynamic_poses(UINT frame) {
        if (palette_mapped_[frame] && !palette_cpu_.empty()) {
            std::memcpy(palette_mapped_[frame], palette_cpu_.data(),
                        palette_cpu_.size() * sizeof(float));
        }
        for (const auto index : dynamic_instances_) {
            auto& instance = prop_instances_[index];
            const auto slot = static_cast<std::size_t>(instance.transform & DynamicSlotMask);
            if (slot >= dynamic_poses_.size()) {
                instance.active = false;
                continue;
            }
            const auto& pose = dynamic_poses_[slot];
            instance.active = pose.active;
            if (!pose.active) {
                continue;
            }
            dynamic_transform_mapped_[frame][slot] = pose.transform;
            dynamic_ambient_mapped_[frame][slot] = pose.ambient;
            transform_point(pose.transform, instance.local_center, instance.center);
            instance.origin[0] = pose.transform.values[12];
            instance.origin[1] = pose.transform.values[13];
            instance.origin[2] = pose.transform.values[14];
        }
    }

    // Main, lens and reflection cameras each append at most one entry per instance.
    void cull_pass(UINT frame, bool sky, const Matrix4& view_projection,
                   const Float3& eye, UINT& cursor, UINT camera = 0) {
        const auto planes = frustum_planes(view_projection);
        auto* output = visible_mapped_[frame];
        for (std::size_t variant_index = 0; variant_index < prop_variants_.size();
             ++variant_index) {
            const auto& variant = prop_variants_[variant_index];
            const UINT start = cursor;
            if (camera != 0 && variant.first_person) continue;
            for (UINT offset = 0; offset < variant.instance_count; ++offset) {
                const auto& instance = prop_instances_[variant.first_instance + offset];
                if (instance.sky != sky || !instance.active) {
                    continue;
                }
                if (instance.fade_max > 0.0f) {
                    const float dx = instance.origin[0] - eye.x;
                    const float dy = instance.origin[1] - eye.y;
                    const float dz = instance.origin[2] - eye.z;
                    if (dx * dx + dy * dy + dz * dz > instance.fade_max * instance.fade_max) {
                        continue;
                    }
                }
                bool inside = true;
                for (const auto& plane : planes) {
                    if (plane[0] * instance.center[0] + plane[1] * instance.center[1]
                        + plane[2] * instance.center[2] + plane[3] < -instance.radius) {
                        inside = false;
                        break;
                    }
                }
                if (inside) {
                    output[cursor++] = instance.transform;
                }
            }
            variant_visible_[camera+(sky ? 1 : 0)][variant_index] = {start, cursor - start};
        }
    }
    void update_stats(std::size_t draws, std::uint64_t triangles) {
        ++stats_frames_;
        const auto now = std::chrono::steady_clock::now();
        if (stats_time_.time_since_epoch().count() == 0) {
            stats_time_ = now;
            return;
        }
        const float elapsed = std::chrono::duration<float>(now - stats_time_).count();
        if (elapsed < 0.5f) {
            return;
        }
        const float fps = stats_frames_ / elapsed;
        stats_frames_ = 0;
        stats_time_ = now;
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        if (adapter_) {
            adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory);
        }
        if constexpr (gmod::edition::is_public) {
            SetWindowTextW(window_, L"GMod Replay");
            return;
        }
        wchar_t textures[96]{};
        if (completed_requests_ < texture_requests_.size()) {
            swprintf_s(textures, L"Textures %zu/%zu",
                       completed_requests_, texture_requests_.size());
        } else {
            swprintf_s(textures, L"Textures Ready (fallbacks: %zu)",
                       failed_world_materials_);
        }
        // Source-space position and view angles, in the format --camera accepts.
        constexpr float radians_to_degrees = 57.29577951308232f;
        const float source_yaw = std::atan2(-std::cos(camera_yaw_), -std::sin(camera_yaw_))
            * radians_to_degrees;
        wchar_t position[96]{};
        swprintf_s(position, L"POS %.0f %.0f %.0f  yaw %.0f pitch %.0f",
                   -camera_x_, -camera_z_, camera_y_, source_yaw,
                   -camera_pitch_ * radians_to_degrees);
        const std::wstring audio_status = audio_ ? audio_->status() + L" (M mutes)" : L"snd off";
        const std::wstring demo_part = demo_status_.empty() ? std::wstring() : demo_status_ + L" | ";
        wchar_t title[640]{};
        swprintf_s(
            title,
            L"GMod Replay | %s%s (O copies) | %.0f FPS (%.1f ms) | props %zu/%zu | draws %zu | %.2fM tris"
            L" | %s | VRAM %.2f/%.2f GB | %s",
            demo_part.c_str(), position,
            fps, fps > 0.0f ? 1000.0f / fps : 0.0f,
            stats_visible_instances_, prop_instances_.size(), draws,
            triangles / 1000000.0,
            audio_status.c_str(),
            memory.CurrentUsage / 1073741824.0, memory.Budget / 1073741824.0,
            textures);
        SetWindowTextW(window_, title);
    }

    void create_world_textures(
        const std::vector<WorldMaterialTexture>& materials,
        const PropRenderData& prop_data) {
        texture_heap_.create(
            device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            static_cast<UINT>(MaterialSlotCount * (materials.size() + 5) + FrameCount * InstanceTableSize),
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);
        prop_instances_ = prop_data.instances;
        for (std::size_t index = 0; index < prop_instances_.size(); ++index) {
            if (prop_instances_[index].dynamic) {
                dynamic_instances_.push_back(index);
            }
        }
        prop_variants_ = prop_data.variants;
        world_materials_ = materials;
        for (std::size_t i = 0; i < materials.size(); ++i)
            if (materials[i].material == "models/jarheads/weapons/attachments/rifle_sights/lens/rt")
                scope_lens_material_ = material_slot(i);
        world_textures_.resize(MaterialSlotCount * (materials.size() + 1));
        // Reserved slots: 0 neutral grey, 1 missing-texture magenta, 2 water.
        upload_texture(FallbackSlot, {1, 1, {200, 200, 200, 255}});
        upload_texture(MissingSlot, {1, 1, {220, 20, 220, 255}});
        upload_texture(WaterSlot, {1, 1, {40, 90, 120, 255}});
        for (std::size_t slot = MaterialSlotCount; slot < world_textures_.size(); ++slot) {
            bind_texture(slot, FallbackSlot);
        }

        // One decode request per unique file; every slot that shares the file
        // is bound to the same GPU texture once it arrives.
        std::map<std::filesystem::path, std::size_t> request_by_path;
        auto request = [&](const std::filesystem::path& path, bool png,
                           std::size_t slot, std::size_t failure_slot) {
            if (asset_cache_) {
                try {
                    if (auto cached = asset_cache_->find(gmod::dx12::GpuAssetCache::texture_source_key(path, png))) {
                        world_textures_[slot] = std::move(cached);
                        bind_texture(slot, slot);
                        return;
                    }
                } catch (const std::exception& error) {
                    std::cerr << "Texture cache lookup failed " << path.string() << ": " << error.what() << "\n";
                }
            }
            auto [found, inserted] =
                request_by_path.emplace(path, texture_requests_.size());
            if (inserted) {
                texture_requests_.push_back({path, png, {}});
            }
            texture_requests_[found->second].targets.push_back({slot, failure_slot});
        };
        for (std::size_t index = 0; index < materials.size(); ++index) {
            const auto& material = materials[index];
            const auto slot = material_slot(index);
            if (!material.vtf2.empty()) {
                request(material.vtf2, false, slot + 1, FallbackSlot);
            }
            if (!material.modulate.empty()) {
                request(material.modulate, false, slot + 2, FallbackSlot);
            }
            const std::size_t failure_slot = material.water_placeholder ? WaterSlot
                : (material.neutral_placeholder || material.prop_material) ? FallbackSlot
                : MissingSlot;
            if (!material.vtf.empty()) {
                request(material.vtf, false, slot, failure_slot);
            } else if (!material.png.empty()) {
                request(material.png, true, slot, failure_slot);
            } else {
                bind_texture(slot, failure_slot);
                if (failure_slot == MissingSlot) {
                    ++failed_world_materials_;
                    std::cerr << "World material fallback (no VTF): "
                              << material.material << "\n";
                }
            }
        }
    }

    void start_texture_decoders() {
        const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
        // Leave cores for the render thread and the OS.
        const unsigned workers = std::clamp(hardware > 2 ? hardware - 2 : 1u, 1u, 8u);
        for (unsigned index = 0; index < workers; ++index) {
            decode_workers_.emplace_back([this] { decode_worker(); });
        }
    }

    void stop_texture_decoders() {
        {
            // Under the lock, so a worker about to wait can't miss the wakeup.
            std::lock_guard lock(decode_mutex_);
            stop_decoding_ = true;
        }
        decode_space_.notify_all();
        for (auto& worker : decode_workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        decode_workers_.clear();
    }

    // Runs on worker threads: CPU-only file reads and DXT/PNG decoding.
    void decode_worker() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stop_decoding_) {
            const std::size_t index = next_request_++;
            if (index >= texture_requests_.size()) {
                break;
            }
            DecodedTexture result{index};
            try {
                const auto& item = texture_requests_[index];
                result.pixels = item.png
                    ? gmod::source::png::PngReader(item.path).read()
                    : gmod::source::vtf::VtfReader(item.path).read_native();
            } catch (const std::exception& error) {
                result.error = error.what();
            }
            const std::size_t bytes = result.pixels ? result.pixels->rgba.size() : 0;
            std::unique_lock lock(decode_mutex_);
            // Bounds decoded-but-not-uploaded memory so a slow GPU can't let
            // gigabytes of pixels pile up in RAM.
            decode_space_.wait(lock, [&] {
                return stop_decoding_ || decoded_bytes_ == 0
                    || decoded_bytes_ + bytes <= MaxPendingDecodeBytes;
            });
            if (stop_decoding_) {
                break;
            }
            decoded_bytes_ += bytes;
            decoded_.push_back(std::move(result));
        }
        if (SUCCEEDED(com)) {
            CoUninitialize();
        }
    }

    // Records the copy for one texture without waiting; the caller submits the
    // batch. Returns the upload buffer, which must outlive the GPU copy.
    static DXGI_FORMAT texture_dxgi_format(gmod::source::vtf::PixelFormat format) {
        switch (format) {
        case gmod::source::vtf::PixelFormat::Bc1: return DXGI_FORMAT_BC1_UNORM;
        case gmod::source::vtf::PixelFormat::Bc2: return DXGI_FORMAT_BC2_UNORM;
        case gmod::source::vtf::PixelFormat::Bc3: return DXGI_FORMAT_BC3_UNORM;
        case gmod::source::vtf::PixelFormat::Rgb9E5: return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        default: return DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }

    ComPtr<ID3D12Resource> record_texture_upload(
        const gmod::source::vtf::Pixels& texture_pixels,
        ComPtr<ID3D12Resource>& destination_texture, bool cache = true) {
        const auto format = texture_dxgi_format(texture_pixels.format);
        const bool block_compressed =
            texture_pixels.format == gmod::source::vtf::PixelFormat::Bc1
            || texture_pixels.format == gmod::source::vtf::PixelFormat::Bc2
            || texture_pixels.format == gmod::source::vtf::PixelFormat::Bc3;
        const UINT mips = static_cast<UINT>(std::max(1, texture_pixels.mip_levels));
        if (texture_pixels.width <= 0 || texture_pixels.height <= 0
            || (!block_compressed && texture_pixels.rgba.size()
                < static_cast<std::size_t>(texture_pixels.width) * texture_pixels.height * 4)) {
            throw std::runtime_error("Decoded texture has invalid dimensions.");
        }
        std::string cache_key;
        if (cache && asset_cache_) {
            cache_key = gmod::dx12::GpuAssetCache::content_key(texture_pixels.rgba.data(), texture_pixels.rgba.size(),
                "texture:" + std::to_string(format) + ":" + std::to_string(texture_pixels.width)
                + ":" + std::to_string(texture_pixels.height) + ":" + std::to_string(mips));
            if (auto cached = asset_cache_->find(cache_key)) {
                destination_texture = std::move(cached);
                return {};
            }
        }
        D3D12_RESOURCE_DESC description{};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = static_cast<UINT64>(texture_pixels.width);
        description.Height = static_cast<UINT>(texture_pixels.height);
        description.DepthOrArraySize = 1;
        description.MipLevels = static_cast<UINT16>(mips);
        // DXT textures keep the VTF's stored blocks (BC1/2/3) and mip chain.
        description.Format = format;
        description.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES default_heap{};
        default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> texture;
        check_hresult(device_->CreateCommittedResource(
                          &default_heap, D3D12_HEAP_FLAG_NONE, &description,
                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                          IID_PPV_ARGS(&texture)),
                      "Create world texture");
        UINT64 upload_size = 0;
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(mips);
        std::vector<UINT> rows(mips);
        std::vector<UINT64> row_sizes(mips);
        device_->GetCopyableFootprints(
            &description, 0, mips, 0, footprints.data(), rows.data(),
            row_sizes.data(), &upload_size);
        std::size_t source_size = 0;
        for (UINT mip = 0; mip < mips; ++mip) {
            source_size += static_cast<std::size_t>(row_sizes[mip]) * rows[mip];
        }
        if (texture_pixels.rgba.size() < source_size) {
            throw std::runtime_error("Decoded texture data is smaller than its mip chain.");
        }
        auto upload = gmod::dx12::create_buffer(
            device_.Get(), upload_size, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        D3D12_RANGE range{0, 0};
        check_hresult(upload->Map(0, &range, &mapped), "Map world texture upload");
        std::size_t source_offset = 0;
        for (UINT mip = 0; mip < mips; ++mip) {
            for (UINT row_index = 0; row_index < rows[mip]; ++row_index) {
                std::memcpy(
                    static_cast<std::uint8_t*>(mapped) + footprints[mip].Offset
                        + static_cast<std::size_t>(row_index) * footprints[mip].Footprint.RowPitch,
                    texture_pixels.rgba.data() + source_offset
                        + static_cast<std::size_t>(row_index) * row_sizes[mip],
                    static_cast<std::size_t>(row_sizes[mip]));
            }
            source_offset += static_cast<std::size_t>(row_sizes[mip]) * rows[mip];
        }
        upload->Unmap(0, nullptr);

        for (UINT mip = 0; mip < mips; ++mip) {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = texture.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = mip;
            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = upload.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = footprints[mip];
            command_list_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list_->ResourceBarrier(1, &barrier);
        destination_texture = std::move(texture);
        if (!cache_key.empty()) pending_cached_textures_.emplace_back(cache_key, destination_texture);
        return upload;
    }

    // Uploads the baked lightmap atlas; an empty atlas becomes one white luxel.
    void create_lightmap_texture(const gmod::source::bsp::LightmapAtlas& atlas) {
        styled_lightmaps_=atlas.styled;
        uploaded_light_styles_=desired_light_styles_=styled_lightmaps_ ? styled_lightmaps_->initial
            : gmod::source::bsp::LightStyleWeights{};
        desired_bank_weights_=initial_bank_weights();
        gmod::source::vtf::Pixels pixels;
        pixels.format = gmod::source::vtf::PixelFormat::Rgb9E5;
        if (atlas.texels.empty()) {
            pixels.width = 1;
            pixels.height = 1;
            const std::uint32_t white = (16u << 27) | (256u << 18) | (256u << 9) | 256u;
            pixels.rgba.resize(4);
            std::memcpy(pixels.rgba.data(), &white, sizeof(white));
        } else {
            pixels.width = atlas.width;
            pixels.height = atlas.height;
            pixels.rgba.resize(atlas.texels.size() * sizeof(std::uint32_t));
            std::memcpy(pixels.rgba.data(), atlas.texels.data(), pixels.rgba.size());
        }
        upload_allocator_->Reset();
        command_list_->Reset(upload_allocator_.Get(), nullptr);
        auto upload = record_texture_upload(pixels, lightmap_texture_, false);
        command_list_->Close();
        ID3D12CommandList* lists[] = {command_list_.Get()};
        command_queue_->ExecuteCommandLists(1, lists);
        wait_for_gpu();
    }

    void record_lightmap_updates(UINT frame) {
        if (!gmod::ui::settings().lighting) return;
        if (!styled_lightmaps_ || uploaded_light_styles_==desired_light_styles_) return;
        auto patches=gmod::source::bsp::lightmap_patches(*styled_lightmaps_,uploaded_light_styles_,desired_light_styles_);
        if (patches.empty()) { uploaded_light_styles_=desired_light_styles_; return; }
        UINT64 size=0;
        for (const auto& p:patches) size=((size+511)&~UINT64(511))+UINT64((p.width*4+255)&~255)*p.height;
        // This frame slot's fence has completed before we map or replace its upload.
        auto& upload=lightmap_uploads_[frame];
        if (!upload || upload->GetDesc().Width<size) upload=gmod::dx12::create_buffer(device_.Get(),size,
            D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped=nullptr; D3D12_RANGE empty{0,0};
        check_hresult(upload->Map(0,&empty,&mapped),"Map styled lightmaps");
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={lightmap_texture_.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST};
        command_list_->ResourceBarrier(1,&barrier);
        UINT64 offset=0;
        for (const auto& p:patches) {
            offset=(offset+511)&~UINT64(511);
            const UINT pitch=(p.width*4+255)&~255;
            for (int y=0;y<p.height;++y) std::memcpy(static_cast<std::uint8_t*>(mapped)+offset+UINT64(y)*pitch,
                p.texels.data()+std::size_t(y)*p.width,std::size_t(p.width)*4);
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=upload.Get();
            src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint={offset,{DXGI_FORMAT_R9G9B9E5_SHAREDEXP,UINT(p.width),UINT(p.height),1,pitch}};
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=lightmap_texture_.Get();
            dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            command_list_->CopyTextureRegion(&dst,p.x,p.y,0,&src,nullptr);
            offset+=UINT64(pitch)*p.height;
        }
        upload->Unmap(0,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
        command_list_->ResourceBarrier(1,&barrier);
        uploaded_light_styles_=desired_light_styles_;
        ++bank_lightmap_uploads_;
    }

    // Immediate upload used for the reserved fallback textures at start-up.
    void upload_texture(
        std::size_t index, const gmod::source::vtf::Pixels& texture_pixels) {
        upload_allocator_->Reset();
        command_list_->Reset(upload_allocator_.Get(), nullptr);
        auto upload = record_texture_upload(texture_pixels, world_textures_[index]);
        command_list_->Close();
        ID3D12CommandList* lists[] = {command_list_.Get()};
        command_queue_->ExecuteCommandLists(1, lists);
        wait_for_gpu();
        bind_texture(index, index);
    }

    // Uploads decoded textures within a per-frame time budget (negative budget
    // means "until nothing is ready"). Only GPU work happens on this thread.
    void stream_world_materials(double budget_ms = StreamBudgetMs) {
        if (completed_requests_ >= texture_requests_.size()) {
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        std::vector<ComPtr<ID3D12Resource>> uploads;
        std::vector<std::pair<std::size_t, std::size_t>> binds;
        bool recording = false;
        while (completed_requests_ < texture_requests_.size()) {
            if (budget_ms >= 0.0 && std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count() > budget_ms) {
                break;
            }
            DecodedTexture item;
            {
                std::lock_guard lock(decode_mutex_);
                if (decoded_.empty()) {
                    break;
                }
                item = std::move(decoded_.front());
                decoded_.pop_front();
                decoded_bytes_ -= item.pixels ? item.pixels->rgba.size() : 0;
            }
            decode_space_.notify_all();
            ++completed_requests_;
            const auto& request = texture_requests_[item.request];
            bool uploaded = false;
            if (item.pixels) {
                try {
                    if (!recording) {
                        upload_allocator_->Reset();
                        command_list_->Reset(upload_allocator_.Get(), nullptr);
                        recording = true;
                    }
                    uploads.push_back(
                        record_texture_upload(
                            *item.pixels, world_textures_[request.targets.front().slot]));
                    uploaded = true;
                } catch (const std::exception& error) {
                    item.error = error.what();
                }
            }
            if (uploaded) {
                if (asset_cache_) {
                    try {
                        pending_cached_textures_.emplace_back(
                            gmod::dx12::GpuAssetCache::texture_source_key(request.path, request.png),
                            world_textures_[request.targets.front().slot]);
                    } catch (const std::exception& error) {
                        std::cerr << "Texture cache store failed " << request.path.string() << ": " << error.what() << "\n";
                    }
                }
                for (const auto& target : request.targets) {
                    binds.emplace_back(target.slot, request.targets.front().slot);
                }
                continue;
            }
            std::cerr << "Texture unavailable " << request.path.string()
                      << ": " << item.error << "\n";
            for (const auto& target : request.targets) {
                binds.emplace_back(target.slot, target.failure_slot);
                if (target.failure_slot == MissingSlot) {
                    ++failed_world_materials_;
                }
            }
        }
        if (recording) {
            command_list_->Close();
            ID3D12CommandList* lists[] = {command_list_.Get()};
            command_queue_->ExecuteCommandLists(1, lists);
        }
        if (!binds.empty()) {
            // Also drains in-flight frames, so rewriting their descriptors is safe.
            wait_for_gpu();
            for (const auto& [slot, source] : binds) {
                bind_texture(slot, source);
            }
        }
    }

    void bind_texture(std::size_t destination, std::size_t source) {
        world_textures_[destination] = world_textures_[source];
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        const auto texture_description = world_textures_[destination]->GetDesc();
        view.Format = texture_description.Format;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = texture_description.MipLevels;
        device_->CreateShaderResourceView(
            world_textures_[destination].Get(), &view,
            texture_heap_.cpu(static_cast<UINT>(destination)));
    }

    bool copy_was_down_ = false;
    bool mute_was_down_ = false;
    std::optional<std::array<float, 3>> follow_;
    std::unique_ptr<gmod::dx12::FrameTextOverlay> frame_text_;
    gmod::dx12::FrameTextOverlay::Painter frame_text_painter_;
    std::optional<std::array<float, 2>> follow_look_; // Source pitch/yaw for a locked chase camera
    float follow_height_ = 60.0f;
    std::unique_ptr<gmod::source::WorldTrace> camera_world_;
    std::vector<Matrix4> camera_transforms_;

    float chase_fraction(const std::array<float,3>& start, const std::array<float,3>& end) const {
        constexpr float radius = 8.0f;
        const float source_start[3]{-start[0],-start[2],start[1]};
        const float source_end[3]{-end[0],-end[2],end[1]};
        float fraction = camera_world_->sweep_sphere(source_start,source_end,radius);
        for (const auto& variant : prop_variants_) {
            if (!variant.camera_trace) continue;
            for (UINT offset = 0; offset < variant.instance_count; ++offset) {
                const auto& instance = prop_instances_[variant.first_instance+offset];
                if (instance.sky) continue;
                const Matrix4* matrix = nullptr;
                if (instance.dynamic) {
                    const auto& pose = dynamic_poses_[instance.transform & DynamicSlotMask];
                    if (!pose.active) continue;
                    matrix = &pose.transform;
                } else {
                    matrix = &camera_transforms_[instance.transform];
                }
                float center[3];
                transform_point(*matrix,instance.local_center,center);
                float distance2 = 0.0f;
                for (int axis = 0; axis < 3; ++axis)
                    distance2 += (center[axis]-start[axis])*(center[axis]-start[axis]);
                if (distance2 > std::pow(instance.radius+220.0f+radius,2.0f)) continue;
                const auto local = [&](const std::array<float,3>& point) {
                    std::array<float,3> result{};
                    for (int axis = 0; axis < 3; ++axis)
                        for (int row = 0; row < 3; ++row)
                            result[axis] += matrix->values[axis*4+row]
                                * (point[row]-matrix->values[12+row]);
                    return result;
                };
                const auto a = local(start), b = local(end);
                const float contact = variant.camera_trace->sweep_sphere(a.data(),b.data(),radius);
                if (g_headless && contact < fraction) std::cout << "CHASE_OBSTACLE dynamic="
                    << instance.dynamic << " fraction=" << contact << "\n";
                fraction = std::min(fraction,contact);
            }
        }
        // Stay a little off the contact plane to avoid floating-point flicker.
        return fraction < 1.0f ? std::max(0.0f,fraction-0.5f/220.0f) : 1.0f;
    }
    std::optional<FirstPersonView> first_person_;
    std::unique_ptr<gmod::audio::AudioSystem> audio_;

    // Puts "x y z yaw pitch" (Source units/degrees) on the clipboard.
    void copy_position_to_clipboard() {
        constexpr float radians_to_degrees = 57.29577951308232f;
        wchar_t text[128]{};
        swprintf_s(text, L"%.1f %.1f %.1f %.1f %.1f", -camera_x_, -camera_z_, camera_y_,
                   std::atan2(-std::cos(camera_yaw_), -std::sin(camera_yaw_)) * radians_to_degrees,
                   -camera_pitch_ * radians_to_degrees);
        const SIZE_T bytes = (wcslen(text) + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!memory) {
            return;
        }
        std::memcpy(GlobalLock(memory), text, bytes);
        GlobalUnlock(memory);
        if (OpenClipboard(window_)) {
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, memory)) {
                GlobalFree(memory);
            }
            CloseClipboard();
        } else {
            GlobalFree(memory);
        }
    }

    void update_camera(UINT frame) {
        if (!camera_initialized_) {
            camera_x_ = initial_camera_x_;
            camera_y_ = initial_camera_y_;
            camera_z_ = initial_camera_z_;
            camera_yaw_ = initial_camera_yaw_;
            camera_pitch_ = initial_camera_pitch_;
            camera_initialized_ = true;
        }
        const bool focused = GetForegroundWindow() == window_ && !g_text_input;
        // Hold Shift to fly faster across the map.
        const float speed = focused && (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 2100.0f : 700.0f;
        const auto now = std::chrono::steady_clock::now();
        const float delta = last_camera_time_.time_since_epoch().count() == 0
            ? 1.0f / 60.0f
            : std::clamp(std::chrono::duration<float>(
                now - last_camera_time_).count(), 0.0f, 0.1f);
        last_camera_time_ = now;
        const float forward_x = std::sin(camera_yaw_);
        const float forward_z = std::cos(camera_yaw_);
        const float right_x = std::cos(camera_yaw_);
        const float right_z = -std::sin(camera_yaw_);
        if (focused && (GetAsyncKeyState('W') & 0x8000)) {
            camera_x_ += forward_x * speed * delta;
            camera_z_ += forward_z * speed * delta;
        }
        if (focused && (GetAsyncKeyState('S') & 0x8000)) {
            camera_x_ -= forward_x * speed * delta;
            camera_z_ -= forward_z * speed * delta;
        }
        if (focused && (GetAsyncKeyState('A') & 0x8000)) {
            camera_x_ -= right_x * speed * delta;
            camera_z_ -= right_z * speed * delta;
        }
        if (focused && (GetAsyncKeyState('D') & 0x8000)) {
            camera_x_ += right_x * speed * delta;
            camera_z_ += right_z * speed * delta;
        }
        if (focused && (GetAsyncKeyState(VK_SPACE) & 0x8000)) {
            camera_y_ += speed * delta;
        }
        if (focused && (GetAsyncKeyState('C') & 0x8000)) {
            camera_y_ -= speed * delta;
        }
        const bool copy_down = focused && (GetAsyncKeyState('O') & 0x8000);
        if (copy_down && !copy_was_down_) {
            copy_position_to_clipboard();
        }
        copy_was_down_ = copy_down;
        const bool mute_down = focused && (GetAsyncKeyState('M') & 0x8000);
        if (mute_down && !mute_was_down_ && audio_) {
            audio_->toggle_mute();
        }
        mute_was_down_ = mute_down;

        POINT cursor{};
        GetCursorPos(&cursor);
        if (g_mouse_captured && !was_mouse_captured_) {
            mouse_initialized_ = false;
        }
        was_mouse_captured_ = g_mouse_captured;
        if (g_mouse_captured && GetForegroundWindow() == window_) {
            if (!mouse_initialized_) {
                RECT client{};
                GetClientRect(window_, &client);
                POINT center{
                    (client.right - client.left) / 2,
                    (client.bottom - client.top) / 2
                };
                ClientToScreen(window_, &center);
                SetCursorPos(center.x, center.y);
                last_cursor_ = center;
                mouse_initialized_ = true;
            } else {
                camera_yaw_ += (cursor.x - last_cursor_.x) * 0.0025f;
                camera_pitch_ -= (cursor.y - last_cursor_.y) * 0.0025f;
                camera_pitch_ = std::clamp(camera_pitch_, -1.45f, 1.45f);
                RECT client{};
                GetClientRect(window_, &client);
                POINT center{
                    (client.right - client.left) / 2,
                    (client.bottom - client.top) / 2
                };
                ClientToScreen(window_, &center);
                SetCursorPos(center.x, center.y);
                last_cursor_ = center;
            }
        }
        if (g_drag_dx != 0.0f || g_drag_dy != 0.0f) {
            camera_yaw_ += g_drag_dx * 0.0025f;
            camera_pitch_ = std::clamp(camera_pitch_ - g_drag_dy * 0.0025f, -1.45f, 1.45f);
            g_drag_dx = g_drag_dy = 0.0f;
        }
        if (first_person_) {
            constexpr float degrees_to_radians = 0.01745329251994329577f;
            const float yaw = first_person_->yaw_degrees * degrees_to_radians;
            float pitch = std::fmod(first_person_->pitch_degrees, 360.0f);
            if (pitch > 180.0f) pitch -= 360.0f;
            if (pitch < -180.0f) pitch += 360.0f;
            camera_x_ = -first_person_->x;
            camera_y_ = first_person_->z;
            camera_z_ = -first_person_->y;
            camera_yaw_ = std::atan2(-std::cos(yaw), -std::sin(yaw));
            camera_pitch_ = std::clamp(-pitch * degrees_to_radians, -1.45f, 1.45f);
        } else if (follow_) {
            if (follow_look_) {
                constexpr float radians = 0.01745329251994329577f;
                const float yaw = (*follow_look_)[1]*radians;
                float pitch = std::remainder((*follow_look_)[0],360.0f);
                camera_yaw_ = std::atan2(-std::cos(yaw),-std::sin(yaw));
                camera_pitch_ = std::clamp(-pitch*radians,-1.45f,1.45f);
            }
            // Chase camera: orbit behind the followed point using the mouse look.
            constexpr float follow_distance = 220.0f;
            const float cp = std::cos(camera_pitch_);
            const float fx = cp * std::sin(camera_yaw_);
            const float fy = std::sin(camera_pitch_);
            const float fz = cp * std::cos(camera_yaw_);
            const std::array<float,3> pivot{-(*follow_)[0],(*follow_)[2]+follow_height_,-(*follow_)[1]};
            const std::array<float,3> desired{pivot[0]-fx*follow_distance,
                pivot[1]-fy*follow_distance,pivot[2]-fz*follow_distance};
            const float fraction = chase_fraction(pivot,desired);
            camera_x_ = pivot[0]+(desired[0]-pivot[0])*fraction;
            camera_y_ = pivot[1]+(desired[1]-pivot[1])*fraction;
            camera_z_ = pivot[2]+(desired[2]-pivot[2])*fraction;
            if (g_headless) std::cout << "CHASE_CAMERA distance=" << fraction*follow_distance
                << " pivot=" << -pivot[0] << "," << -pivot[2] << "," << pivot[1]
                << " camera=" << -camera_x_ << "," << -camera_z_ << "," << camera_y_ << "\n";
        }
        const auto view = camera_view(
            camera_x_, camera_y_, camera_z_, camera_yaw_, camera_pitch_);
        const auto projection = perspective_matrix(
            field_of_view(), static_cast<float>(width_) / static_cast<float>(height_),
            1.0f, initial_camera_far_);
        view_projection_ = matrix_multiply(view, projection);
        reflection_active_ = false;
        float best_water = FLT_MAX;
        const auto water_frustum = frustum_planes(view_projection_);
        for (const auto& plane : water_planes_) {
            if (!gmod::ui::settings().reflections) break;
            if (camera_y_ <= plane.height+0.5f) continue;
            const float x = std::clamp(camera_x_,plane.min_x,plane.max_x);
            const float z = std::clamp(camera_z_,plane.min_z,plane.max_z);
            const float distance = (x-camera_x_)*(x-camera_x_)+(z-camera_z_)*(z-camera_z_)
                +0.25f*(camera_y_-plane.height)*(camera_y_-plane.height);
            bool visible = true;
            for (const auto& face : water_frustum) {
                const float px = face[0]>=0 ? plane.max_x : plane.min_x;
                const float pz = face[2]>=0 ? plane.max_z : plane.min_z;
                if (face[0]*px+face[1]*plane.height+face[2]*pz+face[3]<0) { visible=false; break; }
            }
            if (visible && distance<best_water) {
                best_water = distance;
                water_height_ = plane.height;
                reflection_active_ = true;
            }
        }
        if (audio_) {
            // Renderer (x, y, z) is Source (-x, -z, y); the camera's right vector
            // is (cos yaw, 0, -sin yaw) in renderer space.
            const float listener[3]{-camera_x_, -camera_z_, camera_y_};
            const float right[3]{-std::cos(camera_yaw_), std::sin(camera_yaw_), 0.0f};
            audio_->set_listener(listener, right);
        }
        auto* main_slot = camera_mapped_ + camera_slot_offset(frame, 0);
        std::memcpy(main_slot, view_projection_.values, sizeof(view_projection_.values));
        write_frame_constants(main_slot, false);
        if (has_sky_camera_) {
            // The 3D skybox is a 1:scale miniature: the eye moves 1/scale as far.
            sky_view_position_ = {
                sky_origin_.x + camera_x_ / sky_scale_,
                sky_origin_.y + camera_y_ / sky_scale_,
                sky_origin_.z + camera_z_ / sky_scale_};
            const auto sky_view = camera_view(
                sky_view_position_.x, sky_view_position_.y, sky_view_position_.z,
                camera_yaw_, camera_pitch_);
            sky_view_projection_ = matrix_multiply(sky_view, projection);
            auto* sky_slot = camera_mapped_ + camera_slot_offset(frame, 1);
            std::memcpy(sky_slot, sky_view_projection_.values,
                        sizeof(sky_view_projection_.values));
            write_frame_constants(sky_slot, true);
        }
        if (scope_fov_ > 0) {
            const auto scope_projection = perspective_matrix(scope_fov_,1,1,initial_camera_far_);
            auto* slot = camera_mapped_+camera_slot_offset(frame,2);
            const auto scope_matrix = matrix_multiply(view,scope_projection);
            scope_view_projection_ = scope_matrix;
            std::memcpy(slot,scope_matrix.values,sizeof(scope_matrix.values));
            write_frame_constants(slot,false,scope_fov_,1);
            if (has_sky_camera_) {
                const auto sky_view = camera_view(sky_view_position_.x,sky_view_position_.y,
                    sky_view_position_.z,camera_yaw_,camera_pitch_);
                const auto matrix = matrix_multiply(sky_view,scope_projection);
                scope_sky_projection_ = matrix;
                slot = camera_mapped_+camera_slot_offset(frame,3);
                std::memcpy(slot,matrix.values,sizeof(matrix.values));
                write_frame_constants(slot,true,scope_fov_,1);
            }
        }
        if (reflection_active_) {
            const auto mirror = [](float height) {
                Matrix4 matrix{};
                matrix.values[0]=matrix.values[10]=matrix.values[15]=1;
                matrix.values[5]=-1;
                matrix.values[13]=2*height;
                return matrix;
            };
            reflected_view_projection_ = matrix_multiply(mirror(water_height_),view_projection_);
            auto* slot = camera_mapped_+camera_slot_offset(frame,4);
            std::memcpy(slot,reflected_view_projection_.values,sizeof(Matrix4));
            write_frame_constants(slot,false,0,1,true);
            if (has_sky_camera_) {
                reflected_sky_projection_ = matrix_multiply(mirror(sky_origin_.y+water_height_/sky_scale_),sky_view_projection_);
                slot = camera_mapped_+camera_slot_offset(frame,5);
                std::memcpy(slot,reflected_sky_projection_.values,sizeof(Matrix4));
                write_frame_constants(slot,true,0,1,true);
            }
        }
    }

    static UINT64 camera_slot_offset(UINT frame, UINT pass) {
        return (static_cast<UINT64>(frame) * CameraSlotsPerFrame + pass) * CameraSlotSize;
    }

    // Everything after the view-projection matrix in the camera constants:
    // sky-ray basis and env_skypaint colours, the sun, and the skybox fog.
    void write_frame_constants(std::uint8_t* slot, bool sky_pass,
                               float lens_fov = 0, float lens_aspect = 1, bool reflected = false) const {
        float constants[64]{};
        const float fov = lens_fov > 0 ? lens_fov : field_of_view();
        const float tan_half = std::tan(fov * 0.5f);
        const float aspect = lens_fov > 0 ? lens_aspect : static_cast<float>(width_) / static_cast<float>(height_);
        const float cp = std::cos(camera_pitch_);
        const float sp = std::sin(camera_pitch_);
        const float cy = std::cos(camera_yaw_);
        const float sy = std::sin(camera_yaw_);
        const float basis[12]{
            cp * sy, sp, cp * cy, 0.0f,
            cy * tan_half * aspect, 0.0f, -sy * tan_half * aspect, 0.0f,
            -sp * sy * tan_half, cp * tan_half, -sp * cy * tan_half, 0.0f};
        std::copy(basis, basis + 12, constants);
        // Source (x, y, z) -> renderer (-x, z, -y).
        auto renderer_vector = [](const float source[3], float output[3]) {
            output[0] = -source[0];
            output[1] = source[2];
            output[2] = -source[1];
            const float length = std::sqrt(
                output[0] * output[0] + output[1] * output[1] + output[2] * output[2]);
            for (int axis = 0; axis < 3; ++axis) {
                output[axis] = length > 0.0f ? output[axis] / length : 0.0f;
            }
        };
        if (sky_paint_) {
            const auto& paint = *sky_paint_;
            constexpr float degrees_to_radians = 0.01745329251994329577f;
            const float sun_radius = 0.6f * paint.sun_size * degrees_to_radians;
            float sun[3];
            renderer_vector(paint.sun_normal, sun);
            const float paint_constants[16]{
                paint.top[0], paint.top[1], paint.top[2], 0.0f,
                paint.bottom[0], paint.bottom[1], paint.bottom[2], 0.0f,
                sun[0], sun[1], sun[2], std::cos(sun_radius),
                paint.sun_color[0], paint.sun_color[1], paint.sun_color[2], paint.fade_bias};
            std::copy(paint_constants + 0, paint_constants + 16, constants + 12);
        }
        if (sun_) {
            float direction[3];
            renderer_vector(sun_->direction, direction);
            const float light[8]{
                direction[0], direction[1], direction[2], 0.0f,
                sun_->color[0], sun_->color[1], sun_->color[2], 0.0f};
            std::copy(light, light + 8, constants + 28);
        }
        constants[35] = float((gmod::ui::settings().lighting ? 0u : 1u)
            | (gmod::ui::settings().reflections ? 0u : 2u));
        if (sky_pass && sky_camera_.fog_enabled && sky_camera_.fog_end > sky_camera_.fog_start) {
            float fog_direction[3];
            renderer_vector(sky_camera_.fog_direction, fog_direction);
            const float fog[16]{
                sky_camera_.fog_color[0], sky_camera_.fog_color[1], sky_camera_.fog_color[2], 1.0f,
                sky_camera_.fog_color2[0], sky_camera_.fog_color2[1], sky_camera_.fog_color2[2],
                sky_camera_.fog_blend ? 1.0f : 0.0f,
                fog_direction[0], fog_direction[1], fog_direction[2], sky_camera_.fog_max_density,
                sky_camera_.fog_start, sky_camera_.fog_end, 0.0f, 0.0f};
            std::copy(fog, fog + 16, constants + 36);
        }
        const Float3 eye = sky_pass
            ? sky_view_position_ : Float3{camera_x_, camera_y_, camera_z_};
        constants[52] = eye.x;
        constants[53] = eye.y;
        constants[54] = eye.z;
        // Being below a distant pond's elevation does not put the eye underwater.
        constants[55] = water_probe_ && water_probe_->is_water({-eye.x,-eye.z,eye.y}) ? 1.0f : 0.0f;
        constants[56] = water_height_;
        constants[57] = water_demo_time_.value_or(g_headless ? 0.0f : std::chrono::duration<float>(
            std::chrono::steady_clock::now()-water_clock_).count());
        // 1: background/depth snapshot available; 2: planar reflection is available too.
        constants[58] = !sky_pass && !reflected && lens_fov==0 && !water_planes_.empty()
            ? (reflection_active_ ? 2.0f : 1.0f) : 0.0f;
        constants[59] = initial_camera_far_;
        constants[63] = 1;
        if (reflected) {
            constants[1] *= -1;
            constants[5] *= -1;
            constants[9] *= -1;
            constants[53] = 2*(sky_pass ? sky_origin_.y+water_height_/sky_scale_ : water_height_)-eye.y;
            // The miniature skybox must use the same waterline in its own coordinates.
            // Otherwise its seabed is reflected as a floating sheet above the real water.
            const float scale = sky_pass ? sky_scale_ : 1.0f;
            const float plane = sky_pass ? sky_origin_.y+water_height_/scale : water_height_;
            constants[61] = 1;
            constants[63] = -plane+0.2f/scale;
        }
        std::memcpy(slot + sizeof(Matrix4), constants, sizeof(constants));
    }
    void create_world_pipeline(
        const std::vector<gmod::source::bsp::Triangle>& triangles,
        const std::vector<WorldMaterialTexture>& materials,
        const PropRenderData& prop_data,
        const std::optional<gmod::source::bsp::PlayerSpawnPoint>& spawn) {
        if (triangles.empty()) {
            throw std::runtime_error("BSP contained no world triangles.");
        }
        gmod::source::bsp::Vec3 min{
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()};
        gmod::source::bsp::Vec3 max{
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest()};
        auto include = [&min, &max](const gmod::source::bsp::Vec3& point) {
            min.x = std::min(min.x, point.x);
            min.y = std::min(min.y, point.y);
            min.z = std::min(min.z, point.z);
            max.x = std::max(max.x, point.x);
            max.y = std::max(max.y, point.y);
            max.z = std::max(max.z, point.z);
        };
        std::size_t camera_bounds_triangles = 0;
        for (const auto& triangle : triangles) {
            auto material = triangle.material;
            std::transform(material.begin(), material.end(), material.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (triangle.sky_3d || material.find("skybox") != std::string::npos
                || material.find("toolssky") != std::string::npos) {
                continue;
            }
            include(triangle.a);
            include(triangle.b);
            include(triangle.c);
            ++camera_bounds_triangles;
        }
        if (camera_bounds_triangles == 0) {
            for (const auto& triangle : triangles) {
                include(triangle.a);
                include(triangle.b);
                include(triangle.c);
            }
        }
        const float width = std::max(max.x - min.x, 1.0f);
        const float depth = std::max(max.y - min.y, 1.0f);
        const float height = std::max(max.z - min.z, 1.0f);
        const float center_x = (min.x + max.x) * 0.5f;
        const float center_y = (min.y + max.y) * 0.5f;
        const float center_z = (min.z + max.z) * 0.5f;
        initial_camera_x_ = -center_x;
        initial_camera_y_ = center_z + 200.0f;
        initial_camera_z_ = -center_y
            - std::min(std::max(width, depth) * 0.05f, 2500.0f);
        initial_camera_yaw_ = 0.0f;
        initial_camera_pitch_ = -0.15f;
        if (spawn) {
            initial_camera_x_ = -spawn->origin.x;
            initial_camera_y_ = spawn->origin.z + 2000.0f;
            initial_camera_z_ = -spawn->origin.y;
            constexpr float degrees_to_radians = 0.01745329251994329577f;
            initial_camera_yaw_ = std::atan2(
                -std::cos(spawn->yaw_degrees * degrees_to_radians),
                -std::sin(spawn->yaw_degrees * degrees_to_radians));
            initial_camera_pitch_ = -0.25f;
        }
        initial_camera_far_ = std::max({width, depth, height}) * 3.0f;
        std::map<std::tuple<std::string, bool, int>, std::vector<WorldVertex>> grouped;
        std::map<std::string, const WorldMaterialTexture*> material_lookup;
        for (const auto& material : materials) {

            material_lookup.emplace(material.material, &material);
        }
        // Group horizontal water surfaces by elevation. Rivers and the ocean can
        // use different planes; select the nearby visible body for the reflection.
        std::map<int, WaterPlane> planes;
        for (const auto& triangle : triangles) {
            const auto material = material_lookup.find(triangle.material);
            if (triangle.sky_3d || material == material_lookup.end()
                || (material->second->material_flags & (4u | 128u)) != 4u) continue;
            if (std::abs(triangle.a.z - triangle.b.z) > 0.5f
                || std::abs(triangle.a.z - triangle.c.z) > 0.5f) continue;
            const float area = std::abs((triangle.b.x-triangle.a.x)*(triangle.c.y-triangle.a.y)
                -(triangle.b.y-triangle.a.y)*(triangle.c.x-triangle.a.x))*0.5f;
            auto& plane = planes[static_cast<int>(std::lround(triangle.a.z*2))];
            plane.height = triangle.a.z;
            plane.area += area;
            for (const auto point : {triangle.a, triangle.b, triangle.c}) {
                plane.min_x = std::min(plane.min_x, -point.x);
                plane.max_x = std::max(plane.max_x, -point.x);
                plane.min_z = std::min(plane.min_z, -point.y);
                plane.max_z = std::max(plane.max_z, -point.y);
            }
        }
        for (const auto& [height_key, plane] : planes) {
            if (plane.area >= 4096) water_planes_.push_back(plane);
        }
        std::cout << "WATER_PLANES count=" << water_planes_.size() << "\n";
        std::size_t rescaled_triangles = 0;
        for (const auto& triangle : triangles) {
            // BSP UVs are normalised by texdata size; the engine divides by the
            // runtime base texture's mapping size instead.
            float scale_u = 1.0f;
            float scale_v = 1.0f;
            const auto found = material_lookup.find(triangle.material);
            if (found != material_lookup.end()
                && found->second->mapping_size[0] > 0.0f
                && found->second->mapping_size[1] > 0.0f) {
                scale_u = triangle.texdata_size[0] / found->second->mapping_size[0];
                scale_v = triangle.texdata_size[1] / found->second->mapping_size[1];
                if (scale_u != 1.0f || scale_v != 1.0f) {
                    ++rescaled_triangles;
                }
            }
            auto convert = [&](const gmod::source::bsp::Vec3& point, int corner) {
                return WorldVertex{
                    {-point.x, point.z, -point.y},
                    {triangle.uv[corner][0] * scale_u,
                     triangle.uv[corner][1] * scale_v},
                    triangle.blend[corner] / 255.0f,
                    {triangle.lightmap_uv[corner][0], triangle.lightmap_uv[corner][1], 0.0f}};
            };
            auto& vertices = grouped[{triangle.material, triangle.sky_3d, triangle.map_brush}];
            vertices.push_back(convert(triangle.a, 0));
            vertices.push_back(convert(triangle.b, 1));
            vertices.push_back(convert(triangle.c, 2));
        }
        std::wcout << L"World triangles rescaled to runtime texture size: "
                   << rescaled_triangles << L"\n";
        std::vector<WorldVertex> vertices;
        for (const auto& [group, grouped_vertices] : grouped) {
            const auto& [material_name, sky, brush] = group;
            const auto found_material = std::find_if(materials.begin(), materials.end(),
                [&](const auto& material) { return material.material == material_name; });
            if (found_material == materials.end() || grouped_vertices.empty()) continue;
            const auto& material = *found_material;
            WorldBatch batch;
            batch.material = material.material;
            batch.sky = sky;
            batch.map_brush = brush;
            batch.vertex_offset = static_cast<UINT>(vertices.size());
            batch.vertex_count = static_cast<UINT>(grouped_vertices.size());
            const auto material_index = static_cast<std::size_t>(
                &material - materials.data());
            batch.texture_index = material_slot(material_index);
            batch.alpha_mode = material.alpha_mode;
            batch.alpha_ref = material.alpha_ref;
            batch.material_flags = material.material_flags;
            vertices.insert(vertices.end(), grouped_vertices.begin(), grouped_vertices.end());
            world_batches_.push_back(std::move(batch));
        }
        if (vertices.empty()) {
            throw std::runtime_error("No world material batches contained vertices.");
        }

        world_vertex_buffer_ = create_static_buffer(
            vertices.data(), vertices.size() * sizeof(WorldVertex),
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, true);
        world_vertex_view_ = {
            world_vertex_buffer_->GetGPUVirtualAddress(),
            static_cast<UINT>(vertices.size() * sizeof(WorldVertex)),
            sizeof(WorldVertex)};
        world_vertex_count_ = static_cast<UINT>(vertices.size());

        const char* vertex_source = R"(
            cbuffer Camera : register(b0) {
                row_major float4x4 viewProjection;
                float4 forward;
                float4 right;
                float4 up;
                float4 topColor;
                float4 bottomColor;
                float4 skySunDirection;
                float4 skySunColor;
                float4 lightDirection;
                float4 lightColor;
                float4 fogColor;
                float4 fogColor2;
                float4 fogDirection;
                float4 fogParams;
                float4 eyePosition;
                float4 waterParameters;
                float4 waterClipPlane;
            };            cbuffer Draw : register(b1) {
                uint instanceBase;
                uint useInstances;
                uint alphaMode;
                float alphaRef;
                uint materialFlags;
                uint baseVertex;
            };
            struct PropTransform {
                float4 row0;
                float4 row1;
                float4 row2;
                float4 row3;
            };
            StructuredBuffer<PropTransform> propTransforms : register(t1);
            StructuredBuffer<uint> visibleInstances : register(t2);
            struct AmbientCube {
                float4 colors[6];
            };
            StructuredBuffer<AmbientCube> propAmbient : register(t6);
            StructuredBuffer<PropTransform> dynamicTransforms : register(t8);
            StructuredBuffer<AmbientCube> dynamicAmbient : register(t9);
            // Packed bone ids (3 x 8 bits + count) and weights (2 x 16 bits) per
            // prop vertex, and 3 float4 rows per palette bone.
            StructuredBuffer<uint2> skinVertices : register(t10);
            StructuredBuffer<float4> bonePalette : register(t11);
            struct Input {
                float3 position : POSITION;
                float2 uv : TEXCOORD0;
                float blend : TEXCOORD1;
                float3 extra : TEXCOORD2;
            };
            struct Output {
                float4 position : SV_POSITION;
                float2 uv : TEXCOORD0;
                float blend : TEXCOORD1;
                float3 extra : TEXCOORD2;
                float3 vertexLight : TEXCOORD3;
                float3 eyeDelta : TEXCOORD4;
                float3 tint : TEXCOORD5;
                nointerpolation float4 outline : TEXCOORD6;
                float clipDistance : SV_ClipDistance0;
            };
            Output main(Input input, uint instanceId : SV_InstanceID,
                        uint vertexId : SV_VertexID) {
                Output output;
                float3 localPosition = input.position;
                float3 localNormal = input.extra;
                float4 worldPosition = float4(input.position, 1.0);
                output.vertexLight = float3(1.0, 1.0, 1.0);
                output.tint = float3(1.0, 1.0, 1.0);
                output.outline = float4(0.0, 0.0, 0.0, 0.0);
                if (useInstances != 0) {
                    // SV_InstanceID always starts at 0, so the per-draw base
                    // selects this batch's range of visible instances.
                    uint transformIndex = visibleInstances[instanceBase + instanceId];
                    // The top bit selects the per-frame demo-actor tables.
                    bool dynamicInstance = (transformIndex & 0x80000000u) != 0;
                    uint slot = dynamicInstance ? (transformIndex & 0xFFFFFu)
                                                : (transformIndex & 0x7FFFFFFFu);
                    uint paletteIndex = dynamicInstance ? ((transformIndex >> 20) & 0x7FFu) : 0u;
                    PropTransform transform;
                    AmbientCube cube;
                    if (dynamicInstance) {
                        transform = dynamicTransforms[slot];
                        cube = dynamicAmbient[slot];
                    } else {
                        transform = propTransforms[slot];
                        cube = propAmbient[slot];
                    }
                    if (paletteIndex != 0u) {
                        uint2 skin = skinVertices[vertexId + baseVertex];
                        uint boneCount = skin.x >> 24;
                        if (boneCount > 0u) {
                            float w0 = float(skin.y & 0xFFFFu) / 65535.0;
                            float w1 = float(skin.y >> 16) / 65535.0;
                            float w2 = max(0.0, 1.0 - w0 - w1);
                            uint paletteBase = (paletteIndex - 1u) * 128u * 3u;
                            float3 skinnedPosition = float3(0.0, 0.0, 0.0);
                            float3 skinnedNormal = float3(0.0, 0.0, 0.0);
                            [unroll] for (uint i = 0u; i < 3u; ++i) {
                                if (i >= boneCount) break;
                                float weight = i == 0u ? w0 : (i == 1u ? w1 : w2);
                                uint bone = (skin.x >> (8u * i)) & 0xFFu;
                                float4 r0 = bonePalette[paletteBase + bone * 3u];
                                float4 r1 = bonePalette[paletteBase + bone * 3u + 1u];
                                float4 r2 = bonePalette[paletteBase + bone * 3u + 2u];
                                skinnedPosition += weight * float3(
                                    dot(r0.xyz, input.position) + r0.w,
                                    dot(r1.xyz, input.position) + r1.w,
                                    dot(r2.xyz, input.position) + r2.w);
                                skinnedNormal += weight * float3(
                                    dot(r0.xyz, input.extra),
                                    dot(r1.xyz, input.extra),
                                    dot(r2.xyz, input.extra));
                            }
                            localPosition = skinnedPosition;
                            localNormal = normalize(skinnedNormal);
                        }
                    }
                    worldPosition = localPosition.x * transform.row0
                        + localPosition.y * transform.row1
                        + localPosition.z * transform.row2
                        + transform.row3;
                    [branch] if ((uint(lightColor.w) & 1u) == 0u) {
                    // Source-style ambient cube; the cube is indexed in Source
                    // axes while the renderer uses (-x, z, -y).
                    float3 normal = normalize(
                        localNormal.x * transform.row0.xyz
                        + localNormal.y * transform.row1.xyz
                        + localNormal.z * transform.row2.xyz);
                    float3 sourceNormal = float3(-normal.x, -normal.z, normal.y);
                    float3 weights = sourceNormal * sourceNormal;
                    float3 ambient =
                        weights.x * cube.colors[sourceNormal.x < 0.0 ? 1 : 0].rgb
                        + weights.y * cube.colors[sourceNormal.y < 0.0 ? 3 : 2].rgb
                        + weights.z * cube.colors[sourceNormal.z < 0.0 ? 5 : 4].rgb;
                    // Direct sun reaches a prop only where its leaf sees the sky.
                    float3 sun = lightColor.rgb * saturate(dot(normal, -lightDirection.xyz))
                        * cube.colors[0].w;
                    output.vertexLight = ambient + sun;
                    } else {
                        output.vertexLight = float3(1.0,1.0,1.0);
                    }
                    // Per-instance colour rides in the otherwise unused alpha of the
                    // ambient cube; faces 1-3 hold r, g, b and face 4 flags it as set.
                    if (dynamicInstance && cube.colors[4].w > 0.5) {
                        output.tint = float3(cube.colors[1].w, cube.colors[2].w, cube.colors[3].w);
                    }
                    // Outline colour of a highlighted player, packed as 0xRRGGBB + 1 in face 5.
                    if (dynamicInstance && cube.colors[5].w > 0.5) {
                        uint packed = uint(cube.colors[5].w + 0.5) - 1u;
                        output.outline = float4(float((packed >> 16) & 255u),
                            float((packed >> 8) & 255u), float(packed & 255u), 255.0) / 255.0;
                    }
                }
                output.position = mul(worldPosition, viewProjection);
                // The outline mask pass skips everything that is not highlighted.
                if ((materialFlags & 0x80000000u) != 0u && output.outline.a <= 0.0) {
                    output.position = float4(2.0, 2.0, 2.0, 1.0);
                }
                output.uv = input.uv;
                output.blend = input.blend;
                output.extra = input.extra;
                output.eyeDelta = worldPosition.xyz - eyePosition.xyz;
                output.clipDistance = dot(worldPosition, waterClipPlane);
                return output;
            }
        )";
        const char* pixel_source = R"(
            cbuffer Camera : register(b0) {
                row_major float4x4 viewProjection;
                float4 forward;
                float4 right;
                float4 up;
                float4 topColor;
                float4 bottomColor;
                float4 skySunDirection;
                float4 skySunColor;
                float4 lightDirection;
                float4 lightColor;
                float4 fogColor;
                float4 fogColor2;
                float4 fogDirection;
                float4 fogParams;
                float4 eyePosition;
                float4 waterParameters;
                float4 waterClipPlane;
            };            cbuffer Draw : register(b1) {
                uint instanceBase;
                uint useInstances;
                uint alphaMode;
                float alphaRef;
                uint materialFlags;
                uint baseVertex;
            };
            Texture2D diffuseTexture : register(t3);
            Texture2D diffuseTexture2 : register(t4);
            Texture2D blendModulate : register(t5);
            Texture2D lightmapTexture : register(t7);
            Texture2D waterReflection : register(t12);
            Texture2D waterScene : register(t13);
            Texture2D<float> waterDepth : register(t14);
            SamplerState diffuseSampler : register(s0);
            SamplerState lightmapSampler : register(s1);
            float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0,
                        float blend : TEXCOORD1, float3 extra : TEXCOORD2,
                        float3 vertexLight : TEXCOORD3,
                        float3 eyeDelta : TEXCOORD4,
                        float3 tint : TEXCOORD5) : SV_TARGET {
                float4 color = diffuseTexture.Sample(diffuseSampler, uv);
                if ((materialFlags & 16u) != 0) {
                    // $blendtintbybasealpha: the base alpha says how much the colour tints.
                    color.rgb *= lerp(float3(1.0, 1.0, 1.0), tint, color.a);
                }
                if ((materialFlags & 32u) != 0) {
                    // Vehicle paint: the shared white body texture takes the car's colour.
                    color.rgb *= tint;
                }
                if ((materialFlags & 1) != 0) {
                    // WorldVertexTransition, as in Source's lightmappedgeneric:
                    // the modulate texture's G is the centre, R the half-width.
                    float factor = saturate(blend);
                    if ((materialFlags & 2) != 0) {
                        float4 modulate = blendModulate.Sample(diffuseSampler, uv);
                        float low = saturate(modulate.g - modulate.r);
                        float high = saturate(modulate.g + modulate.r);
                        factor = high - low > 0.0001
                            ? smoothstep(low, high, factor)
                            : step(low, factor);
                    }
                    color = lerp(color, diffuseTexture2.Sample(diffuseSampler, uv), factor);
                }
                // Light is linear and textures are gamma encoded, so lighting in
                // linear space and re-encoding scales the texture by light^(1/2.2).
                float3 light = float3(1.0,1.0,1.0);
                [branch] if ((uint(lightColor.w) & 1u) == 0u) {
                    light = vertexLight;
                    if (useInstances == 0) {
                        light = extra.x < 0.0 ? float3(1.0,1.0,1.0)
                            : lightmapTexture.Sample(lightmapSampler, extra.xy).rgb;
                    }
                    // The scope render target already contains the scene's lighting.
                    if ((materialFlags & 64u) == 0)
                        color.rgb *= pow(max(light, 0.0), 1.0 / 2.2);
                }
                if ((materialFlags & 128u) != 0u && eyePosition.w < 0.5) clip(-1);
                if ((materialFlags & 4) != 0) {
                    float3 world = eyeDelta + eyePosition.xyz;
                    bool beneath = (materialFlags & 128u) != 0u;
                    clip(beneath ? world.y - eyePosition.y - 0.05 : eyePosition.y - world.y + 0.05);
                    float time = waterParameters.y;
                    float2 p = world.xz;
                    float2 slope = float2(
                        0.030*cos(dot(p,float2(0.047,0.031))+time*0.9),
                        0.025*sin(dot(p,float2(-0.029,0.061))-time*0.7));
                    if ((materialFlags & 256u) != 0u) {
                        float2 n0 = diffuseTexture.Sample(diffuseSampler,p*0.0032+float2(time*0.012,time*0.004)).xy*2-1;
                        float2 n1 = diffuseTexture.Sample(diffuseSampler,p.yx*0.0051+float2(-time*0.008,time*0.007)).xy*2-1;
                        slope += (n0+n1)*0.13;
                    } else {
                        slope += float2(sin(dot(p,float2(0.19,0.11))-time*1.4),
                                        cos(dot(p,float2(-0.13,0.21))+time*1.1))*0.025;
                    }
                    float3 normal = normalize(float3(slope.x,1,slope.y));
                    float3 toEye = normalize(-eyeDelta);
                    float cosine = saturate(abs(dot(toEye,normal)));
                    float fresnel = 0.025 + 0.975*pow(1-cosine,5);
                    if ((uint(lightColor.w) & 2u) != 0u) fresnel = 0;
                    float3 reflectedRay = reflect(-toEye,normal);
                    float3 reflection = lerp(bottomColor.rgb,topColor.rgb,
                        pow(saturate(reflectedRay.y),max(skySunColor.w,0.01)));
                    float3 transmission = float3(0.055,0.11,0.14);
                    float shoreline = 1;
                    if (waterParameters.z > 0.5 && !beneath) {
                        uint width,height;
                        waterScene.GetDimensions(width,height);
                        float2 screenUV = position.xy/float2(width,height);
                        float viewZ = max(dot(eyeDelta,forward.xyz),1);
                        float farZ = waterParameters.w;
                        float background = waterDepth.SampleLevel(lightmapSampler,screenUV,0);
                        float backgroundZ = farZ/max(farZ-background*(farZ-1),0.001);
                        float thickness = max(backgroundZ-viewZ,0)*length(eyeDelta)/viewZ;
                        float depthBelow = thickness*abs(toEye.y);
                        shoreline = smoothstep(0,8,depthBelow);
                        // Distort only underwater pixels; foreground geometry must not smear into the water.
                        float2 refractUV = saturate(screenUV+slope*0.009*saturate(depthBelow/24));
                        float shiftedDepth = waterDepth.SampleLevel(lightmapSampler,refractUV,0);
                        float shiftedZ = farZ/max(farZ-shiftedDepth*(farZ-1),0.001);
                        if (shiftedZ < viewZ) refractUV = screenUV;
                        float3 bed = waterScene.SampleLevel(lightmapSampler,refractUV,0).rgb;
                        // Use the water column's depth for colour absorption. A
                        // long, shallow viewing ray must not turn thin water
                        // into an opaque fog sheet above the visible bed.
                        float3 absorption = exp(-depthBelow*float3(0.0020,0.0013,0.0010));
                        transmission = bed*absorption+float3(0.055,0.105,0.13)*(1-absorption);
                        float2 reflectUV = saturate(screenUV+slope*0.025*saturate(1800/viewZ));
                        if (waterParameters.z > 1.5 && abs(world.y-waterParameters.x) <= 2.0)
                            reflection = waterReflection.SampleLevel(lightmapSampler,reflectUV,0).rgb;
                        color = float4(lerp(waterScene.SampleLevel(lightmapSampler,screenUV,0).rgb,
                            lerp(transmission,reflection,fresnel),shoreline),1);
                    } else {
                        color = float4(lerp(transmission,reflection,fresnel),0.82+0.18*fresnel);
                    }
                    float3 sunlight = normalize(-lightDirection.xyz+float3(0,0.00001,0));
                    float glint = pow(saturate(dot(normal,normalize(toEye+sunlight))),160);
                    if ((uint(lightColor.w) & 1u) == 0u)
                        color.rgb += lightColor.rgb*glint*0.12*shoreline;
                } else if ((materialFlags & 8) != 0) {
                    // Privacy/frosted glass: the engine blurs what is behind it.
                    color = float4(float3(0.82, 0.88, 0.92) * pow(max(light, 0.0), 1.0 / 2.2), 0.78);
                }
                if (fogColor.w > 0.5) {
                    // env sky_camera fog: linear in skybox units, capped at fogmaxdensity.
                    float distance = length(eyeDelta);
                    float amount = saturate((distance - fogParams.x) / (fogParams.y - fogParams.x))
                        * fogDirection.w;
                    float3 tint = fogColor.rgb;
                    if (fogColor2.w > 0.5) {
                        tint = lerp(fogColor.rgb, fogColor2.rgb,
                                    saturate(dot(eyeDelta / max(distance, 0.001), fogDirection.xyz)));
                    }
                    color.rgb = lerp(color.rgb, tint, amount);
                }
                if (alphaMode == 0) {
                    return float4(color.rgb, 1.0);
                }
                if (alphaMode == 1) {
                    clip(color.a - alphaRef);
                    return float4(color.rgb, 1.0);
                }
                return color;
            }
        )";
        ComPtr<ID3DBlob> vertex_shader;
        ComPtr<ID3DBlob> pixel_shader;
        ComPtr<ID3DBlob> errors;
        check_hresult(D3DCompile(
                          vertex_source, std::strlen(vertex_source), "world_vs",
                          nullptr, nullptr, "main", "vs_5_0", 0, 0,
                          &vertex_shader, &errors),
                      "Compile world vertex shader");
        check_hresult(D3DCompile(
                          pixel_source, std::strlen(pixel_source), "world_ps",
                          nullptr, nullptr, "main", "ps_5_0", 0, 0,
                          &pixel_shader, &errors),
                      "Compile world pixel shader");

        D3D12_ROOT_SIGNATURE_DESC root_description{};
        D3D12_ROOT_PARAMETER camera_parameter{};
        camera_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        camera_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        camera_parameter.Descriptor.ShaderRegister = 0;
        D3D12_DESCRIPTOR_RANGE texture_range{};
        texture_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        texture_range.NumDescriptors = static_cast<UINT>(MaterialSlotCount);
        texture_range.BaseShaderRegister = 3;
        D3D12_DESCRIPTOR_RANGE instance_ranges[3]{};
        instance_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        instance_ranges[0].NumDescriptors = 2;
        instance_ranges[0].BaseShaderRegister = 1;
        instance_ranges[0].OffsetInDescriptorsFromTableStart = 0;
        instance_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        instance_ranges[1].NumDescriptors = 4;
        instance_ranges[1].BaseShaderRegister = 6;
        instance_ranges[1].OffsetInDescriptorsFromTableStart = 2;
        instance_ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        instance_ranges[2].NumDescriptors = 2;
        instance_ranges[2].BaseShaderRegister = 10;
        instance_ranges[2].OffsetInDescriptorsFromTableStart = 6;
        D3D12_ROOT_PARAMETER texture_parameter{};
        texture_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        texture_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        texture_parameter.DescriptorTable.NumDescriptorRanges = 1;
        texture_parameter.DescriptorTable.pDescriptorRanges = &texture_range;
        D3D12_ROOT_PARAMETER transform_parameter{};
        transform_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        transform_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        transform_parameter.DescriptorTable.NumDescriptorRanges = 3;
        transform_parameter.DescriptorTable.pDescriptorRanges = instance_ranges;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_ANISOTROPIC;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.ShaderRegister = 0;
        sampler.MaxAnisotropy = 16;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC lightmap_sampler{};
        lightmap_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        lightmap_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lightmap_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lightmap_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lightmap_sampler.ShaderRegister = 1;
        lightmap_sampler.MaxLOD = D3D12_FLOAT32_MAX;
        lightmap_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC samplers[2]{sampler, lightmap_sampler};
        D3D12_ROOT_PARAMETER draw_parameter{};
        draw_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        draw_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        draw_parameter.Constants.ShaderRegister = 1;
        draw_parameter.Constants.Num32BitValues = DrawConstantCount;
        D3D12_DESCRIPTOR_RANGE water_range{};
        water_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        water_range.NumDescriptors = 3;
        water_range.BaseShaderRegister = 12;
        D3D12_ROOT_PARAMETER water_parameter{};
        water_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        water_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        water_parameter.DescriptorTable.NumDescriptorRanges = 1;
        water_parameter.DescriptorTable.pDescriptorRanges = &water_range;
        D3D12_ROOT_PARAMETER parameters[5]{
            camera_parameter, texture_parameter, transform_parameter, draw_parameter, water_parameter};
        root_description.NumParameters = 5;
        root_description.pParameters = parameters;
        root_description.NumStaticSamplers = 2;
        root_description.pStaticSamplers = samplers;
        root_description.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized;
        check_hresult(D3D12SerializeRootSignature(
                          &root_description, D3D_ROOT_SIGNATURE_VERSION_1,
                          &serialized, &errors),
                      "Serialize world root signature");
        check_hresult(device_->CreateRootSignature(
                          0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                          IID_PPV_ARGS(&root_signature_)),
                      "Create world root signature");

        const D3D12_INPUT_ELEMENT_DESC input[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 20,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24,
             D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.InputLayout = {input, 4};
        pipeline.pRootSignature = root_signature_.Get();
        pipeline.VS = {vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize()};
        pipeline.PS = {pixel_shader->GetBufferPointer(), pixel_shader->GetBufferSize()};
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.DepthStencilState.DepthEnable = TRUE;
        pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.SampleMask = UINT_MAX;
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipeline.SampleDesc.Count = 1;
        check_hresult(device_->CreateGraphicsPipelineState(
                          &pipeline, IID_PPV_ARGS(&pipeline_state_)),
                      "Create world pipeline state");
        // Translucent and additive passes: depth-tested, no depth writes.
        pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        auto& blend = pipeline.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        check_hresult(device_->CreateGraphicsPipelineState(
                          &pipeline, IID_PPV_ARGS(&blend_pipeline_state_)),
                      "Create translucent pipeline state");
        // Refraction already contains the scene behind the surface. Keep the
        // nearest water interface in depth so overlapping BSP faces cannot tint
        // it again as a second transparent sheet, regardless of draw order.
        pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        check_hresult(device_->CreateGraphicsPipelineState(
                          &pipeline, IID_PPV_ARGS(&water_pipeline_state_)),
                      "Create water pipeline state");
        pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        blend.DestBlend = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        check_hresult(device_->CreateGraphicsPipelineState(
                          &pipeline, IID_PPV_ARGS(&additive_pipeline_state_)),
                      "Create additive pipeline state");
        create_outline_pipelines(pipeline);
        create_effect_pipelines(pipeline);
        if (sky_paint_) {
            create_sky_pipeline();
        }
        create_prop_buffers(materials, prop_data);
        write_outline_view();
        write_scope_view();
        write_water_views();
    }

    void create_effect_pipelines(D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline) {
        const char* source = R"(
            cbuffer Camera : register(b0) {
                row_major float4x4 viewProjection;
                float4 forward; float4 right; float4 up;
            };
            Texture2D baseTexture : register(t3);
            SamplerState baseSampler : register(s0);
            struct Vertex { float3 position : POSITION; float2 uv : TEXCOORD0; float opacity : TEXCOORD1; float radius : TEXCOORD2; float3 color : COLOR0; };
            struct Pixel { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float opacity : TEXCOORD1; float3 color : COLOR0; };
            Pixel vs(Vertex v) {
                Pixel p;
                float3 position = v.position + v.radius * (normalize(right.xyz)*(v.uv.x*2-1)
                    + normalize(up.xyz)*(1-v.uv.y*2));
                p.position = mul(float4(position,1),viewProjection);
                p.uv = v.uv; p.opacity = v.opacity; p.color = v.color;
                return p;
            }
            float4 decal(Pixel p) : SV_TARGET {
                float4 color = baseTexture.Sample(baseSampler,clamp(p.uv,0.001,0.999));
                return float4(lerp(float3(0.5,0.5,0.5),color.rgb,saturate(p.opacity)),1);
            }
            float4 flash(Pixel p) : SV_TARGET {
                float4 color = baseTexture.Sample(baseSampler,clamp(p.uv,0.001,0.999));
                return float4(color.rgb*p.color,color.a*saturate(p.opacity));
            }
            float4 mist(Pixel p) : SV_TARGET {
                float4 color = baseTexture.Sample(baseSampler,clamp(p.uv,0.001,0.999));
                return float4(color.rgb * float3(0.24,0.008,0.004),color.a*saturate(p.opacity));
            }
            float4 smoke(Pixel p) : SV_TARGET {
                float4 color = baseTexture.Sample(baseSampler,clamp(p.uv,0.001,0.999));
                return float4(color.rgb*p.color,color.a*saturate(p.opacity));
            }
        )";
        auto compile = [&](const char* entry, const char* target) {
            ComPtr<ID3DBlob> shader, errors;
            const auto result = D3DCompile(source, std::strlen(source), "combat_effects", nullptr,
                nullptr, entry, target, 0, 0, &shader, &errors);
            if (FAILED(result) && errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer()) << "\n";
            check_hresult(result, "Compile combat effect shader");
            return shader;
        };
        const auto vs = compile("vs","vs_5_0");
        const auto ps = compile("decal","ps_5_0");
        const auto flash = compile("flash","ps_5_0");
        const auto mist = compile("mist","ps_5_0");
        const auto smoke = compile("smoke","ps_5_0");
        const D3D12_INPUT_ELEMENT_DESC input[]{
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32_FLOAT,0,20,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"TEXCOORD",2,DXGI_FORMAT_R32_FLOAT,0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,28,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
        pipeline.InputLayout = {input,5};
        pipeline.VS = {vs->GetBufferPointer(),vs->GetBufferSize()};
        pipeline.PS = {ps->GetBufferPointer(),ps->GetBufferSize()};
        pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        auto& blend = pipeline.BlendState.RenderTarget[0];
        blend.SrcBlend = D3D12_BLEND_DEST_COLOR;
        blend.DestBlend = D3D12_BLEND_SRC_COLOR;
        check_hresult(device_->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&decal_pipeline_)),
                      "Create decal modulation pipeline");
        pipeline.PS = {flash->GetBufferPointer(),flash->GetBufferSize()};
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_ONE;
        check_hresult(device_->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&flash_pipeline_)),
                      "Create muzzle flash pipeline");
        pipeline.PS = {mist->GetBufferPointer(),mist->GetBufferSize()};
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        check_hresult(device_->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&mist_pipeline_)),
                      "Create blood mist pipeline");
        pipeline.PS = {smoke->GetBufferPointer(),smoke->GetBufferSize()};
        check_hresult(device_->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&smoke_pipeline_)),
                      "Create smoke puff pipeline");
    }

    void upload_effect_vertices(UINT frame) {
        if (effect_vertices_.empty()) return;
        const auto bytes = effect_vertices_.size()*sizeof(EffectVertex);
        if (effect_capacity_[frame] < bytes) {
            effect_buffers_[frame] = gmod::dx12::create_buffer(device_.Get(),bytes,
                D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
            effect_capacity_[frame] = bytes;
        }
        void* mapped = nullptr;
        D3D12_RANGE range{0,0};
        check_hresult(effect_buffers_[frame]->Map(0,&range,&mapped),"Map combat effect vertices");
        std::memcpy(mapped,effect_vertices_.data(),bytes);
        effect_buffers_[frame]->Unmap(0,nullptr);
    }

    void draw_combat_effects(UINT frame) {
        if (effect_vertices_.empty()) return;
        const D3D12_VERTEX_BUFFER_VIEW view{effect_buffers_[frame]->GetGPUVirtualAddress(),
            static_cast<UINT>(effect_vertices_.size()*sizeof(EffectVertex)),sizeof(EffectVertex)};
        command_list_->IASetVertexBuffers(0,1,&view);
        for (const auto& draw : effect_draws_) {
            const auto* material = effect_material(draw.material);
            if (!material || material->vtf.empty()) continue;
            const auto index = static_cast<std::size_t>(material-world_materials_.data());
            command_list_->SetPipelineState(draw.smoke ? smoke_pipeline_.Get() : draw.mist ? mist_pipeline_.Get()
                : draw.additive ? flash_pipeline_.Get() : decal_pipeline_.Get());
            command_list_->SetGraphicsRootDescriptorTable(1,texture_heap_.gpu(material_slot(index)));
            command_list_->DrawInstanced(draw.count,1,draw.first,0);
        }
    }

    // Player outlines, like GMod's halos: highlighted actors are drawn as flat colour into
    // a mask (depth-tested, or over everything when seen through walls), then a fullscreen
    // pass draws a band of that colour just outside each shape.
    void create_outline_pipelines(D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline) {
        const char* mask_source = R"(
            cbuffer Draw : register(b1) {
                uint instanceBase;
                uint useInstances;
                uint alphaMode;
                float alphaRef;
                uint materialFlags;
                uint baseVertex;
            };
            Texture2D diffuseTexture : register(t3);
            SamplerState diffuseSampler : register(s0);
            float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0,
                        float blend : TEXCOORD1, float3 extra : TEXCOORD2,
                        float3 vertexLight : TEXCOORD3, float3 eyeDelta : TEXCOORD4,
                        float3 tint : TEXCOORD5,
                        nointerpolation float4 outline : TEXCOORD6) : SV_TARGET {
                if (outline.a <= 0.0) discard;
                if (alphaMode != 0u) {
                    float alpha = diffuseTexture.Sample(diffuseSampler, uv).a;
                    clip(alpha - (alphaMode == 1u ? alphaRef : 0.3));
                }
                return float4(outline.rgb, 1.0);
            }
        )";
        const char* composite_vertex = R"(
            float4 main(uint id : SV_VertexID) : SV_POSITION {
                float2 corner = float2((id << 1) & 2, id & 2);
                return float4(corner * 2.0 - 1.0, 0.0, 1.0);
            }
        )";
        const char* composite_pixel = R"(
            Texture2D outlineMask : register(t3);
            float4 main(float4 position : SV_POSITION) : SV_TARGET {
                int2 pixel = int2(position.xy);
                if (outlineMask.Load(int3(pixel, 0)).a > 0.5) discard;
                const int Radius = 3;
                float best = 1000.0;
                float3 color = float3(0.0, 0.0, 0.0);
                [loop] for (int dy = -Radius; dy <= Radius; ++dy) {
                    [loop] for (int dx = -Radius; dx <= Radius; ++dx) {
                        float distance = float(dx * dx + dy * dy);
                        if (distance > Radius * Radius + 1 || distance >= best) continue;
                        float4 sample = outlineMask.Load(int3(pixel + int2(dx, dy), 0));
                        if (sample.a > 0.5) {
                            best = distance;
                            color = sample.rgb;
                        }
                    }
                }
                if (best >= 1000.0) discard;
                return float4(color, 1.0);
            }
        )";
        ComPtr<ID3DBlob> mask_shader;
        ComPtr<ID3DBlob> composite_vs;
        ComPtr<ID3DBlob> composite_ps;
        ComPtr<ID3DBlob> errors;
        check_hresult(D3DCompile(mask_source, std::strlen(mask_source), "outline_mask_ps",
                                 nullptr, nullptr, "main", "ps_5_0", 0, 0, &mask_shader, &errors),
                      "Compile outline mask shader");
        check_hresult(D3DCompile(composite_vertex, std::strlen(composite_vertex), "outline_vs",
                                 nullptr, nullptr, "main", "vs_5_0", 0, 0, &composite_vs, &errors),
                      "Compile outline vertex shader");
        check_hresult(D3DCompile(composite_pixel, std::strlen(composite_pixel), "outline_ps",
                                 nullptr, nullptr, "main", "ps_5_0", 0, 0, &composite_ps, &errors),
                      "Compile outline pixel shader");
        pipeline.PS = {mask_shader->GetBufferPointer(), mask_shader->GetBufferSize()};
        pipeline.BlendState.RenderTarget[0].BlendEnable = FALSE;
        pipeline.DepthStencilState.DepthEnable = TRUE;
        pipeline.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pipeline.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        check_hresult(device_->CreateGraphicsPipelineState(
                          &pipeline, IID_PPV_ARGS(&outline_mask_pipeline_)),
                      "Create outline mask pipeline");
        pipeline.DepthStencilState.DepthEnable = FALSE;
        check_hresult(device_->CreateGraphicsPipelineState(
                          &pipeline, IID_PPV_ARGS(&outline_xray_pipeline_)),
                      "Create through-walls outline pipeline");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC composite{};
        composite.pRootSignature = root_signature_.Get();
        composite.VS = {composite_vs->GetBufferPointer(), composite_vs->GetBufferSize()};
        composite.PS = {composite_ps->GetBufferPointer(), composite_ps->GetBufferSize()};
        composite.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        composite.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        composite.RasterizerState.DepthClipEnable = TRUE;
        composite.DepthStencilState.DepthEnable = FALSE;
        composite.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        composite.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        composite.SampleMask = UINT_MAX;
        composite.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        composite.NumRenderTargets = 1;
        composite.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        composite.SampleDesc.Count = 1;
        check_hresult(device_->CreateGraphicsPipelineState(
                          &composite, IID_PPV_ARGS(&outline_composite_pipeline_)),
                      "Create outline composite pipeline");
    }

    // The mask as t3 of a material-style table (all three slots, so the table is complete).
    void write_outline_view() {
        if (!outline_mask_ || !texture_heap_.get()) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        for (UINT index = 0; index < MaterialSlotCount; ++index) {
            device_->CreateShaderResourceView(outline_mask_.Get(), &view,
                                              texture_heap_.cpu(outline_table_slot() + index));
            if (scope_outline_mask_)
                device_->CreateShaderResourceView(scope_outline_mask_.Get(), &view,
                    texture_heap_.cpu(scope_outline_table_slot() + index));
        }
    }

    // Fullscreen gradient and sun from the map's env_skypaint.
    void create_sky_pipeline() {
        const char* vertex_source = R"(
            cbuffer Camera : register(b0) {
                row_major float4x4 viewProjection;
                float4 forward;
                float4 right;
                float4 up;
                float4 topColor;
                float4 bottomColor;
                float4 sunDirection;
                float4 sunColor;
            };
            struct Output {
                float4 position : SV_POSITION;
                float3 direction : TEXCOORD0;
            };
            Output main(uint id : SV_VertexID) {
                float2 corner = float2((id << 1) & 2, id & 2);
                float2 ndc = corner * 2.0 - 1.0;
                Output output;
                output.position = float4(ndc, 1.0, 1.0);
                output.direction = forward.xyz + right.xyz * ndc.x + up.xyz * ndc.y;
                return output;
            }
        )";
        const char* pixel_source = R"(
            cbuffer Camera : register(b0) {
                row_major float4x4 viewProjection;
                float4 forward;
                float4 right;
                float4 up;
                float4 topColor;
                float4 bottomColor;
                float4 sunDirection;
                float4 sunColor;
            };
            float4 main(float4 position : SV_POSITION,
                        float3 direction : TEXCOORD0) : SV_TARGET {
                float3 ray = normalize(direction);
                float blend = pow(saturate(ray.y), max(sunColor.w, 0.01));
                float3 sky = lerp(bottomColor.rgb, topColor.rgb, blend);
                float alignment = dot(ray, sunDirection.xyz);
                float disc = smoothstep(sunDirection.w - 0.0004, sunDirection.w + 0.0004, alignment);
                float glow = pow(saturate(alignment), 200.0) * 0.35;
                return float4(saturate(sky + sunColor.rgb * (disc + glow)), 1.0);
            }
        )";
        ComPtr<ID3DBlob> vertex_shader;
        ComPtr<ID3DBlob> pixel_shader;
        ComPtr<ID3DBlob> errors;
        check_hresult(D3DCompile(
                          vertex_source, std::strlen(vertex_source), "sky_vs",
                          nullptr, nullptr, "main", "vs_5_0", 0, 0,
                          &vertex_shader, &errors),
                      "Compile sky vertex shader");
        check_hresult(D3DCompile(
                          pixel_source, std::strlen(pixel_source), "sky_ps",
                          nullptr, nullptr, "main", "ps_5_0", 0, 0,
                          &pixel_shader, &errors),
                      "Compile sky pixel shader");
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root_signature_.Get();
        pipeline.VS = {vertex_shader->GetBufferPointer(), vertex_shader->GetBufferSize()};
        pipeline.PS = {pixel_shader->GetBufferPointer(), pixel_shader->GetBufferSize()};
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.DepthStencilState.DepthEnable = FALSE;
        pipeline.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.SampleMask = UINT_MAX;
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipeline.SampleDesc.Count = 1;
        check_hresult(device_->CreateGraphicsPipelineState(
                          &pipeline, IID_PPV_ARGS(&sky_pipeline_state_)),
                      "Create sky pipeline state");
    }

    void create_prop_buffers(
        const std::vector<WorldMaterialTexture>& materials,
        const PropRenderData& prop_data) {
        scope_meshes_.clear();
        for (const auto& batch : prop_data.batches) {
            const auto& variant = prop_data.variants[batch.variant];
            if (!variant.first_person) continue;
            const auto material = std::find_if(materials.begin(), materials.end(),
                [&](const auto& entry) { return entry.material == batch.material; });
            if (material == materials.end()
                || material_slot(static_cast<std::size_t>(material-materials.begin())) != scope_lens_material_) continue;
            for (UINT i = 0; i < variant.instance_count; ++i) {
                const auto& instance = prop_data.instances[variant.first_instance+i];
                if (!instance.dynamic) continue;
                ScopeMesh mesh;
                mesh.transform = instance.transform;
                for (UINT index = 0; index < batch.index_count; ++index) {
                    const auto vertex = batch.base_vertex + prop_data.indices[batch.index_offset+index];
                    mesh.vertices.push_back({prop_data.vertices.at(vertex),
                        prop_data.skin.empty() ? std::array<std::uint32_t, 2>{} : prop_data.skin.at(vertex)});
                }
                scope_meshes_.push_back(std::move(mesh));
            }
        }
        if (g_headless) std::cout << "SCOPE_MESHES count=" << scope_meshes_.size() << "\n";
        if (prop_data.transforms.empty()) {
            throw std::runtime_error("Static-prop transform table is missing its identity entry.");
        }
        const auto vertex_bytes = prop_data.vertices.size() * sizeof(WorldVertex);
        const auto index_bytes = prop_data.indices.size() * sizeof(std::uint32_t);
        const auto transform_bytes = prop_data.transforms.size() * sizeof(Matrix4);
        if (vertex_bytes > UINT_MAX || index_bytes > UINT_MAX
            || prop_data.transforms.size() > UINT_MAX) {
            throw std::runtime_error("Static-prop GPU buffers exceed Direct3D 12 limits.");
        }

        if (prop_data.map_vertices > prop_data.vertices.size() || prop_data.map_indices > prop_data.indices.size())
            throw std::runtime_error("Retained prop geometry exceeds scene buffers.");
        if (prop_data.map_vertices && !map_prop_vertex_buffer_) {
            map_prop_vertex_buffer_ = create_static_buffer(prop_data.vertices.data(),
                prop_data.map_vertices * sizeof(WorldVertex), D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            map_prop_index_buffer_ = create_static_buffer(prop_data.indices.data(),
                prop_data.map_indices * sizeof(std::uint32_t), D3D12_RESOURCE_STATE_INDEX_BUFFER);
            map_prop_vertex_view_ = {map_prop_vertex_buffer_->GetGPUVirtualAddress(),
                static_cast<UINT>(prop_data.map_vertices * sizeof(WorldVertex)), sizeof(WorldVertex)};
            map_prop_index_view_ = {map_prop_index_buffer_->GetGPUVirtualAddress(),
                static_cast<UINT>(prop_data.map_indices * sizeof(std::uint32_t)), DXGI_FORMAT_R32_UINT};
        }
        const auto actor_vertex_bytes = vertex_bytes - prop_data.map_vertices * sizeof(WorldVertex);
        const auto actor_index_bytes = index_bytes - prop_data.map_indices * sizeof(std::uint32_t);
        if (actor_vertex_bytes && actor_index_bytes) {
            prop_vertex_buffer_ = create_static_buffer(
                prop_data.vertices.data() + prop_data.map_vertices, actor_vertex_bytes,
                D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, true);
            prop_vertex_view_ = {
                prop_vertex_buffer_->GetGPUVirtualAddress(),
                static_cast<UINT>(actor_vertex_bytes), sizeof(WorldVertex)};
            prop_index_buffer_ = create_static_buffer(
                prop_data.indices.data() + prop_data.map_indices, actor_index_bytes,
                D3D12_RESOURCE_STATE_INDEX_BUFFER, true);
            prop_index_view_ = {
                prop_index_buffer_->GetGPUVirtualAddress(),
                static_cast<UINT>(actor_index_bytes), DXGI_FORMAT_R32_UINT};
        } else {
            prop_vertex_buffer_.Reset();
            prop_index_buffer_.Reset();
            prop_vertex_view_ = {};
            prop_index_view_ = {};
        }
        if (!map_prop_vertex_buffer_ || !prop_transform_buffer_) prop_transform_buffer_ = create_static_buffer(
            prop_data.transforms.data(), transform_bytes,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (prop_data.ambient.size() != prop_data.transforms.size()) {
            throw std::runtime_error("Static-prop ambient table does not match its transforms.");
        }
        if (!map_prop_vertex_buffer_ || !prop_ambient_buffer_) prop_ambient_buffer_ = create_static_buffer(
            prop_data.ambient.data(), prop_data.ambient.size() * sizeof(AmbientGpu),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        // Per-vertex skin data (t10). Scenes without skinned actors bind a
        // one-element placeholder the shader never reads.
        if (!prop_data.skin.empty() && prop_data.skin.size() != prop_data.vertices.size()) {
            throw std::runtime_error("Static-prop skin data does not match its vertices.");
        }
        const std::array<std::uint32_t, 2> empty_skin{};
        const bool has_skin = !prop_data.skin.empty();
        prop_skin_buffer_ = create_static_buffer(
            has_skin ? prop_data.skin.data() : &empty_skin,
            (has_skin ? prop_data.skin.size() : 1) * sizeof(empty_skin),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true);
        D3D12_SHADER_RESOURCE_VIEW_DESC skin_view{};
        skin_view.Format = DXGI_FORMAT_UNKNOWN;
        skin_view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        skin_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        skin_view.Buffer.NumElements = static_cast<UINT>(has_skin ? prop_data.skin.size() : 1);
        skin_view.Buffer.StructureByteStride = sizeof(empty_skin);
        // Player bone palettes (t11): MaxPaletteBones 3x4 matrices per player,
        // rewritten every frame.
        palette_float_count_ = std::max<std::size_t>(prop_data.palette_players, 1)
            * MaxPaletteBones * 12;
        palette_cpu_.assign(palette_float_count_, 0.0f);
        D3D12_SHADER_RESOURCE_VIEW_DESC palette_view = skin_view;
        palette_view.Buffer.NumElements = static_cast<UINT>(palette_float_count_ / 4);
        palette_view.Buffer.StructureByteStride = 4 * sizeof(float);

        visible_capacity_ = static_cast<UINT>(
            std::max<std::size_t>(prop_data.instances.size()*3, 1));
        D3D12_SHADER_RESOURCE_VIEW_DESC transform_view{};
        transform_view.Format = DXGI_FORMAT_UNKNOWN;
        transform_view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        transform_view.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        transform_view.Buffer.NumElements =
            static_cast<UINT>(prop_data.transforms.size());
        transform_view.Buffer.StructureByteStride = sizeof(Matrix4);
        D3D12_SHADER_RESOURCE_VIEW_DESC visible_view = transform_view;
        visible_view.Buffer.NumElements = visible_capacity_;
        visible_view.Buffer.StructureByteStride = sizeof(std::uint32_t);
        D3D12_SHADER_RESOURCE_VIEW_DESC ambient_view = transform_view;
        ambient_view.Buffer.NumElements = static_cast<UINT>(prop_data.ambient.size());
        ambient_view.Buffer.StructureByteStride = sizeof(AmbientGpu);
        // Demo actors: CPU-written every frame, one copy per frame in flight.
        dynamic_capacity_ = static_cast<UINT>(std::max<std::size_t>(prop_data.actors.size(), 1));
        dynamic_poses_.assign(dynamic_capacity_, DynamicPose{});
        D3D12_SHADER_RESOURCE_VIEW_DESC dynamic_transform_view = transform_view;
        dynamic_transform_view.Buffer.NumElements = dynamic_capacity_;
        D3D12_SHADER_RESOURCE_VIEW_DESC dynamic_ambient_view = ambient_view;
        dynamic_ambient_view.Buffer.NumElements = dynamic_capacity_;
        D3D12_SHADER_RESOURCE_VIEW_DESC lightmap_view{};
        lightmap_view.Format = lightmap_texture_->GetDesc().Format;
        lightmap_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        lightmap_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        lightmap_view.Texture2D.MipLevels = 1;
        for (UINT frame = 0; frame < FrameCount; ++frame) {
            visible_buffers_[frame] = gmod::dx12::create_buffer(
                device_.Get(), visible_capacity_ * sizeof(std::uint32_t),
                D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            D3D12_RANGE range{0, 0};
            check_hresult(visible_buffers_[frame]->Map(
                              0, &range, reinterpret_cast<void**>(&visible_mapped_[frame])),
                          "Map visible prop instances");
            std::memset(visible_mapped_[frame], 0, visible_capacity_ * sizeof(std::uint32_t));
            device_->CreateShaderResourceView(
                prop_transform_buffer_.Get(), &transform_view,
                texture_heap_.cpu(instance_table_slot(frame)));
            device_->CreateShaderResourceView(
                visible_buffers_[frame].Get(), &visible_view,
                texture_heap_.cpu(instance_table_slot(frame) + 1));
            device_->CreateShaderResourceView(
                prop_ambient_buffer_.Get(), &ambient_view,
                texture_heap_.cpu(instance_table_slot(frame) + 2));
            device_->CreateShaderResourceView(
                lightmap_texture_.Get(), &lightmap_view,
                texture_heap_.cpu(instance_table_slot(frame) + 3));
            dynamic_transform_buffers_[frame] = gmod::dx12::create_buffer(
                device_.Get(), dynamic_capacity_ * sizeof(Matrix4),
                D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            dynamic_ambient_buffers_[frame] = gmod::dx12::create_buffer(
                device_.Get(), dynamic_capacity_ * sizeof(AmbientGpu),
                D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            check_hresult(dynamic_transform_buffers_[frame]->Map(
                              0, &range, reinterpret_cast<void**>(&dynamic_transform_mapped_[frame])),
                          "Map dynamic transforms");
            check_hresult(dynamic_ambient_buffers_[frame]->Map(
                              0, &range, reinterpret_cast<void**>(&dynamic_ambient_mapped_[frame])),
                          "Map dynamic ambient");
            std::memset(dynamic_transform_mapped_[frame], 0, dynamic_capacity_ * sizeof(Matrix4));
            std::memset(dynamic_ambient_mapped_[frame], 0, dynamic_capacity_ * sizeof(AmbientGpu));
            device_->CreateShaderResourceView(
                dynamic_transform_buffers_[frame].Get(), &dynamic_transform_view,
                texture_heap_.cpu(instance_table_slot(frame) + 4));
            device_->CreateShaderResourceView(
                dynamic_ambient_buffers_[frame].Get(), &dynamic_ambient_view,
                texture_heap_.cpu(instance_table_slot(frame) + 5));
            palette_buffers_[frame] = gmod::dx12::create_buffer(
                device_.Get(), palette_float_count_ * sizeof(float),
                D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
            check_hresult(palette_buffers_[frame]->Map(
                              0, &range, reinterpret_cast<void**>(&palette_mapped_[frame])),
                          "Map bone palettes");
            std::memset(palette_mapped_[frame], 0, palette_float_count_ * sizeof(float));
            device_->CreateShaderResourceView(
                prop_skin_buffer_.Get(), &skin_view,
                texture_heap_.cpu(instance_table_slot(frame) + 6));
            device_->CreateShaderResourceView(
                palette_buffers_[frame].Get(), &palette_view,
                texture_heap_.cpu(instance_table_slot(frame) + 7));
        }

        std::map<std::string, UINT> material_indices;
        for (std::size_t index = 0; index < materials.size(); ++index) {
            material_indices.emplace(
                materials[index].material, static_cast<UINT>(index));
        }
        prop_batches_.reserve(prop_data.batches.size());
        for (const auto& source : prop_data.batches) {
            if (source.index_count == 0) {
                continue;
            }
            PropDraw batch = source;
            batch.skin_base_vertex = static_cast<UINT>(source.base_vertex);
            batch.map_geometry = source.index_offset < prop_data.map_indices;
            if (!batch.map_geometry) {
                batch.base_vertex -= static_cast<INT>(prop_data.map_vertices);
                batch.index_offset -= static_cast<UINT>(prop_data.map_indices);
            }
            const auto material = material_indices.find(source.material);
            if (material != material_indices.end()) {
                batch.texture_index = material_slot(material->second);
                batch.alpha_mode = materials[material->second].alpha_mode;
                batch.alpha_ref = materials[material->second].alpha_ref;
                batch.material_flags = materials[material->second].material_flags;
            }
            prop_batches_.push_back(std::move(batch));
        }
    }

    void create_device() {
        if (asset_cache_ && asset_cache_->device) {
            device_ = asset_cache_->device;
            adapter_ = asset_cache_->adapter;
            check_hresult(device_->GetDeviceRemovedReason(), "Reuse cached GPU device");
            create_command_queue();
            return;
        }
        ComPtr<IDXGIFactory6> factory;
        check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        ComPtr<IDXGIAdapter1> selected;
        DXGI_ADAPTER_DESC1 selected_desc{};
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapterByGpuPreference(
                    index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 description{};
            candidate->GetDesc1(&description);
            if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                continue;
            }
            if (SUCCEEDED(D3D12CreateDevice(
                    candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                    __uuidof(ID3D12Device), nullptr))) {
                selected = candidate;
                selected_desc = description;
                break;
            }
        }
        if (!selected) {
            throw std::runtime_error("No hardware DirectX 12 adapter found.");
        }
        check_hresult(D3D12CreateDevice(
                          selected.Get(), D3D_FEATURE_LEVEL_11_0,
                          IID_PPV_ARGS(&device_)),
                      "D3D12CreateDevice");
        std::wcout << L"GPU: " << selected_desc.Description << L"\n";
        selected.As(&adapter_);
        if (asset_cache_) {
            asset_cache_->device = device_;
            asset_cache_->adapter = adapter_;
        }
        create_command_queue();
    }

    void create_command_queue() {
        D3D12_COMMAND_QUEUE_DESC queue_description{};
        queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        check_hresult(device_->CreateCommandQueue(
                          &queue_description, IID_PPV_ARGS(&command_queue_)),
                      "CreateCommandQueue");
    }

    void create_swap_chain(HWND window) {
        ComPtr<IDXGIFactory4> factory;
        check_hresult(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width_;
        description.Height = height_;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = FrameCount;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> temporary;
        check_hresult(factory->CreateSwapChainForHwnd(
                          command_queue_.Get(), window, &description,
                          nullptr, nullptr, &temporary),
                      "CreateSwapChainForHwnd");
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        check_hresult(temporary.As(&swap_chain_), "Query IDXGISwapChain3");
    }

    void create_render_targets() {
        rtv_heap_.create(
            device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FrameCount + 4,
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
        dsv_heap_.create(
            device_.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 3,
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE);
        create_size_dependent_targets();
    }

    // Back buffers and the depth buffer, at the current width_ x height_.
    void create_size_dependent_targets() {
        for (UINT index = 0; index < FrameCount; ++index) {
            check_hresult(swap_chain_->GetBuffer(
                              index, IID_PPV_ARGS(&render_targets_[index])),
                          "Get swap-chain buffer");
            device_->CreateRenderTargetView(
                render_targets_[index].Get(), nullptr, rtv_heap_.cpu(index));
        }

        D3D12_HEAP_PROPERTIES depth_heap{};
        depth_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC depth_description{};
        depth_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth_description.Width = width_;
        depth_description.Height = height_;
        depth_description.DepthOrArraySize = 1;
        depth_description.MipLevels = 1;
        depth_description.Format = DXGI_FORMAT_D32_FLOAT;
        depth_description.SampleDesc.Count = 1;
        depth_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 1.0f;
        check_hresult(device_->CreateCommittedResource(
                          &depth_heap, D3D12_HEAP_FLAG_NONE, &depth_description,
                          D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                          IID_PPV_ARGS(&depth_buffer_)),
                      "Create depth buffer");
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_description{};
        dsv_description.Format = DXGI_FORMAT_D32_FLOAT;
        dsv_description.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device_->CreateDepthStencilView(
            depth_buffer_.Get(), &dsv_description, dsv_heap_.cpu());

        D3D12_RESOURCE_DESC mask_description = depth_description;
        mask_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        mask_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE mask_clear{};
        mask_clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        check_hresult(device_->CreateCommittedResource(
                          &depth_heap, D3D12_HEAP_FLAG_NONE, &mask_description,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &mask_clear,
                          IID_PPV_ARGS(&outline_mask_)),
                      "Create outline mask");
        device_->CreateRenderTargetView(outline_mask_.Get(), nullptr, rtv_heap_.cpu(FrameCount));
        write_outline_view();
        auto lens_depth = depth_description;
        lens_depth.Width = lens_depth.Height = 512;
        check_hresult(device_->CreateCommittedResource(
            &depth_heap,D3D12_HEAP_FLAG_NONE,&lens_depth,D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clear,IID_PPV_ARGS(&scope_depth_)),"Create scope depth");
        device_->CreateDepthStencilView(scope_depth_.Get(),&dsv_description,dsv_heap_.cpu(1));
        auto lens = mask_description;
        lens.Width = lens.Height = 512;
        check_hresult(device_->CreateCommittedResource(
            &depth_heap,D3D12_HEAP_FLAG_NONE,&lens,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &mask_clear,IID_PPV_ARGS(&scope_color_)),"Create scope lens");
        device_->CreateRenderTargetView(scope_color_.Get(),nullptr,rtv_heap_.cpu(FrameCount+1));
        check_hresult(device_->CreateCommittedResource(
            &depth_heap,D3D12_HEAP_FLAG_NONE,&lens,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &mask_clear,IID_PPV_ARGS(&scope_outline_mask_)),"Create scope outline mask");
        device_->CreateRenderTargetView(scope_outline_mask_.Get(),nullptr,rtv_heap_.cpu(FrameCount+2));
        write_outline_view();
        if (texture_heap_.get()) write_scope_view();
        create_water_targets();
    }

    void create_water_targets() {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC color{};
        color.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        color.Width = std::max<UINT>(1, width_/2);
        color.Height = std::max<UINT>(1, height_/2);
        color.DepthOrArraySize = color.MipLevels = 1;
        color.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        color.SampleDesc.Count = 1;
        color.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = color.Format;
        check_hresult(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&color,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,&clear,IID_PPV_ARGS(&water_reflection_)),
            "Create water reflection");
        device_->CreateRenderTargetView(water_reflection_.Get(),nullptr,rtv_heap_.cpu(FrameCount+3));
        auto depth = color;
        depth.Format = DXGI_FORMAT_D32_FLOAT;
        depth.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        clear.Format = depth.Format;
        clear.DepthStencil.Depth = 1;
        check_hresult(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&depth,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,&clear,IID_PPV_ARGS(&water_reflection_depth_)),
            "Create water reflection depth");
        device_->CreateDepthStencilView(water_reflection_depth_.Get(),nullptr,dsv_heap_.cpu(2));
        color.Width = depth.Width = width_;
        color.Height = depth.Height = height_;
        color.Flags = depth.Flags = D3D12_RESOURCE_FLAG_NONE;
        depth.Format = DXGI_FORMAT_R32_TYPELESS;
        check_hresult(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&color,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&water_scene_)),
            "Create water refraction snapshot");
        check_hresult(device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&depth,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&water_depth_)),
            "Create water depth snapshot");
        write_water_views();
    }

    void write_water_views() {
        if (!texture_heap_.get() || !water_reflection_) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(water_reflection_.Get(),&view,texture_heap_.cpu(water_table_slot()));
        device_->CreateShaderResourceView(water_scene_.Get(),&view,texture_heap_.cpu(water_table_slot()+1));
        view.Format = DXGI_FORMAT_R32_FLOAT;
        device_->CreateShaderResourceView(water_depth_.Get(),&view,texture_heap_.cpu(water_table_slot()+2));
    }

    void copy_water_background(UINT frame) {
        command_list_->OMSetRenderTargets(0,nullptr,FALSE,nullptr);
        // Unbind the water SRVs before the copies, and restore them after transition.
        command_list_->SetGraphicsRootDescriptorTable(4,texture_heap_.gpu(0));
        D3D12_RESOURCE_BARRIER barriers[4]{};
        ID3D12Resource* resources[]{render_targets_[frame].Get(),depth_buffer_.Get(),water_scene_.Get(),water_depth_.Get()};
        const D3D12_RESOURCE_STATES before[]{D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
        for (int i=0; i<4; ++i) {
            barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            auto& transition = barriers[i].Transition;
            transition.pResource = resources[i];
            transition.StateBefore = before[i];
            transition.StateAfter = i<2 ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
            transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        command_list_->ResourceBarrier(4,barriers);
        command_list_->CopyResource(water_scene_.Get(),render_targets_[frame].Get());
        command_list_->CopyResource(water_depth_.Get(),depth_buffer_.Get());
        for (auto& barrier : barriers) std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
        command_list_->ResourceBarrier(4,barriers);
        command_list_->SetGraphicsRootDescriptorTable(4,texture_heap_.gpu(water_table_slot()));
        const auto rtv = rtv_heap_.cpu(frame);
        const auto dsv = dsv_heap_.cpu();
        command_list_->OMSetRenderTargets(1,&rtv,FALSE,&dsv);
    }

    void write_scope_view() {
        if (!scope_color_) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        for (UINT i = 0; i < MaterialSlotCount; ++i)
            device_->CreateShaderResourceView(scope_color_.Get(),&view,texture_heap_.cpu(scope_table_slot()+i));
    }

    void create_frame_resources() {
        for (auto& allocator : command_allocators_) {
            check_hresult(device_->CreateCommandAllocator(
                              D3D12_COMMAND_LIST_TYPE_DIRECT,
                              IID_PPV_ARGS(&allocator)),
                          "Create command allocator");
        }
        check_hresult(device_->CreateCommandList(
                          0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                          command_allocators_[0].Get(), nullptr,
                          IID_PPV_ARGS(&command_list_)),
                      "Create command list");
        command_list_->Close();
        check_hresult(device_->CreateCommandAllocator(
                          D3D12_COMMAND_LIST_TYPE_DIRECT,
                          IID_PPV_ARGS(&upload_allocator_)),
                      "Create upload command allocator");
        check_hresult(device_->CreateFence(
                          0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)),
                      "Create fence");
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fence_event_) {
            throw std::runtime_error("CreateEventW failed.");
        }
        camera_buffer_ = gmod::dx12::create_buffer(
            device_.Get(), CameraSlotSize * CameraSlotsPerFrame * FrameCount, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        D3D12_RANGE range{0, 0};
        check_hresult(camera_buffer_->Map(
                          0, &range,
                          reinterpret_cast<void**>(&camera_mapped_)),
                      "Map camera constants");
    }

    void wait_for_gpu() {
        ++fence_value_;
        check_hresult(command_queue_->Signal(fence_.Get(), fence_value_), "Signal fence");
        if (fence_->GetCompletedValue() < fence_value_) {
            check_hresult(fence_->SetEventOnCompletion(fence_value_, fence_event_),
                          "Set fence event");
            WaitForSingleObject(fence_event_, INFINITE);
        }
        if (asset_cache_) {
            for (const auto& [key, texture] : pending_cached_textures_) asset_cache_->store(key, texture);
            pending_cached_textures_.clear();
        }
    }

    static void write_bmp(
        const std::wstring& path, UINT width, UINT height,
        const void* pixels, UINT row_pitch) {
#pragma pack(push, 1)
        struct FileHeader {
            std::uint16_t type = 0x4D42;
            std::uint32_t size = 0;
            std::uint16_t reserved1 = 0;
            std::uint16_t reserved2 = 0;
            std::uint32_t offset = 54;
        };
        struct InfoHeader {
            std::uint32_t size = 40;
            std::int32_t width = 0;
            std::int32_t height = 0;
            std::uint16_t planes = 1;
            std::uint16_t bits = 32;
            std::uint32_t compression = 0;
            std::uint32_t image_size = 0;
            std::int32_t xppm = 0;
            std::int32_t yppm = 0;
            std::uint32_t colors = 0;
            std::uint32_t important = 0;
        };
#pragma pack(pop)
        const auto* source = static_cast<const std::uint8_t*>(pixels);
        const auto image_size = width * height * 4;
        FileHeader file_header;
        file_header.size = sizeof(FileHeader) + sizeof(InfoHeader) + image_size;
        InfoHeader info_header;
        info_header.width = static_cast<std::int32_t>(width);
        info_header.height = static_cast<std::int32_t>(height);
        info_header.image_size = image_size;
        std::ofstream output(path, std::ios::binary);
        if (!output) {
            throw std::runtime_error("Could not open screenshot path.");
        }
        output.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
        output.write(reinterpret_cast<const char*>(&info_header), sizeof(info_header));
        std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 4);
        for (UINT y = 0; y < height; ++y) {
            const auto* input = source + static_cast<size_t>(height - y - 1) * row_pitch;
            for (UINT x = 0; x < width; ++x) {
                row[x * 4 + 0] = input[x * 4 + 2];
                row[x * 4 + 1] = input[x * 4 + 1];
                row[x * 4 + 2] = input[x * 4 + 0];
                row[x * 4 + 3] = input[x * 4 + 3];
            }
            output.write(reinterpret_cast<const char*>(row.data()), width * 4);
        }
    }

    gmod::dx12::GpuAssetCache* asset_cache_ = nullptr;
    std::vector<std::pair<std::string, ComPtr<ID3D12Resource>>> pending_cached_textures_;
    ComPtr<ID3D12Device> device_;
    UINT width_ = WindowWidth;
    UINT height_ = WindowHeight;
    ComPtr<ID3D12CommandQueue> command_queue_;
    ComPtr<IDXGISwapChain3> swap_chain_;
    gmod::dx12::DescriptorHeap rtv_heap_;
    gmod::dx12::DescriptorHeap dsv_heap_;
    std::array<ComPtr<ID3D12Resource>, FrameCount> render_targets_;
    ComPtr<ID3D12PipelineState> decal_pipeline_;
    ComPtr<ID3D12PipelineState> flash_pipeline_;
    ComPtr<ID3D12PipelineState> mist_pipeline_;
    ComPtr<ID3D12PipelineState> smoke_pipeline_;
    std::array<ComPtr<ID3D12Resource>,FrameCount> effect_buffers_;
    std::array<std::size_t,FrameCount> effect_capacity_{};
    std::vector<EffectVertex> effect_vertices_;
    std::vector<EffectDraw> effect_draws_;
    ComPtr<ID3D12Resource> depth_buffer_;
    std::array<ComPtr<ID3D12CommandAllocator>, FrameCount> command_allocators_;
    ComPtr<ID3D12GraphicsCommandList> command_list_;
    ComPtr<ID3D12Fence> fence_;
    struct WaterPlane {
        float height = 0, area = 0;
        float min_x = FLT_MAX, min_z = FLT_MAX;
        float max_x = -FLT_MAX, max_z = -FLT_MAX;
    };
    std::vector<WaterPlane> water_planes_;
    float water_height_ = 0;
    bool reflection_active_ = false;
    std::optional<float> water_demo_time_;
    std::optional<gmod::source::bsp::BspProbe> water_probe_;
    Matrix4 reflected_view_projection_, reflected_sky_projection_;
    ComPtr<ID3D12Resource> water_reflection_, water_reflection_depth_, water_scene_, water_depth_;
    const std::chrono::steady_clock::time_point water_clock_ = std::chrono::steady_clock::now();
    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12PipelineState> pipeline_state_;
    ComPtr<ID3D12PipelineState> water_pipeline_state_;
    gmod::dx12::DescriptorHeap texture_heap_;
    std::vector<ComPtr<ID3D12Resource>> world_textures_;
    ComPtr<ID3D12Resource> lightmap_texture_;
    std::shared_ptr<const gmod::source::bsp::StyledLightmaps> styled_lightmaps_;
    gmod::source::bsp::LightStyleWeights uploaded_light_styles_{}, desired_light_styles_{};
    gmod::source::bsp::BankLightWeights desired_bank_weights_{};
    std::array<ComPtr<ID3D12Resource>,FrameCount> lightmap_uploads_;
    std::size_t bank_lightmap_uploads_=0;
    ComPtr<ID3D12Resource> prop_ambient_buffer_;
    std::array<ComPtr<ID3D12Resource>, FrameCount> dynamic_transform_buffers_;
    std::array<ComPtr<ID3D12Resource>, FrameCount> dynamic_ambient_buffers_;
    ComPtr<ID3D12Resource> prop_skin_buffer_;
    std::array<ComPtr<ID3D12Resource>, FrameCount> palette_buffers_;
    std::array<float*, FrameCount> palette_mapped_{};
    std::vector<float> palette_cpu_;
    std::size_t palette_float_count_ = 0;
    bool palette_dirty_ = false;
    std::array<Matrix4*, FrameCount> dynamic_transform_mapped_{};
    std::array<AmbientGpu*, FrameCount> dynamic_ambient_mapped_{};
    UINT dynamic_capacity_ = 1;
    std::vector<DynamicPose> dynamic_poses_;
    std::vector<std::size_t> dynamic_instances_;
    std::wstring demo_status_;
    std::vector<WorldMaterialTexture> world_materials_;
    std::size_t failed_world_materials_ = 0;

    static constexpr std::size_t FallbackSlot = 0;
    static constexpr std::size_t MissingSlot = 1;
    static constexpr std::size_t WaterSlot = 2;
    static constexpr double StreamBudgetMs = 4.0;
    static constexpr std::size_t MaxPendingDecodeBytes = 512ull * 1024 * 1024;
    struct TextureTarget {
        std::size_t slot = 0;
        std::size_t failure_slot = FallbackSlot;
    };
    struct TextureRequest {
        std::filesystem::path path;
        bool png = false;
        std::vector<TextureTarget> targets;
    };
    struct DecodedTexture {
        std::size_t request = 0;
        std::optional<gmod::source::vtf::Pixels> pixels;
        std::string error;
    };
    // Written only before the decode workers start; read-only afterwards.
    std::vector<TextureRequest> texture_requests_;
    std::vector<std::thread> decode_workers_;
    std::atomic<std::size_t> next_request_{0};
    std::atomic<bool> stop_decoding_{false};
    std::mutex decode_mutex_;
    std::condition_variable decode_space_;
    std::deque<DecodedTexture> decoded_;
    std::size_t decoded_bytes_ = 0;
    std::size_t completed_requests_ = 0;
    ComPtr<ID3D12Resource> camera_buffer_;
    std::uint8_t* camera_mapped_ = nullptr;
    Matrix4 view_projection_{};
    ComPtr<IDXGIAdapter3> adapter_;
    ComPtr<ID3D12CommandAllocator> upload_allocator_;
    ComPtr<ID3D12PipelineState> blend_pipeline_state_;
    ComPtr<ID3D12PipelineState> additive_pipeline_state_;
    ComPtr<ID3D12PipelineState> outline_mask_pipeline_;
    ComPtr<ID3D12PipelineState> outline_xray_pipeline_;
    ComPtr<ID3D12PipelineState> outline_composite_pipeline_;
    ComPtr<ID3D12Resource> outline_mask_;
    ComPtr<ID3D12Resource> scope_color_;
    struct ScopeMesh {
        struct Vertex { WorldVertex vertex; std::array<std::uint32_t, 2> skin; };
        UINT transform = 0;
        std::vector<Vertex> vertices;
    };
    std::vector<ScopeMesh> scope_meshes_;
    ComPtr<ID3D12Resource> scope_outline_mask_;
    ComPtr<ID3D12Resource> scope_depth_;
    float scope_fov_ = 0;
    Matrix4 scope_view_projection_{};
    Matrix4 scope_sky_projection_{};
    UINT scope_lens_material_ = UINT_MAX;
    std::vector<char> outline_variants_;
    bool outline_active_ = false;
    bool outline_through_walls_ = false;
    std::array<UINT64, FrameCount> frame_fence_values_{};
    std::array<ComPtr<ID3D12Resource>, FrameCount> visible_buffers_;
    std::array<std::uint32_t*, FrameCount> visible_mapped_{};
    UINT visible_capacity_ = 1;
    std::vector<PropInstance> prop_instances_;
    std::vector<PropVariant> prop_variants_;
    // [0] main pass, [1] 3D-skybox pass: (start, count) of visible instances per variant.
    std::array<std::vector<std::pair<UINT, UINT>>, 6> variant_visible_;
    bool has_sky_camera_ = false;
    Float3 sky_origin_{};
    float sky_scale_ = 16.0f;
    Float3 sky_view_position_{};
    Matrix4 sky_view_projection_{};
    std::optional<gmod::source::bsp::SkyPaint> sky_paint_;
    std::optional<gmod::source::bsp::SunLight> sun_;
    gmod::source::bsp::SkyCamera sky_camera_{};
    ComPtr<ID3D12PipelineState> sky_pipeline_state_;
    bool vsync_ = true;
    std::chrono::steady_clock::time_point stats_time_{};
    UINT stats_frames_ = 0;
    std::size_t stats_visible_instances_ = 0;
    ComPtr<ID3D12Resource> world_vertex_buffer_;
    D3D12_VERTEX_BUFFER_VIEW world_vertex_view_{};
    UINT world_vertex_count_ = 0;
    std::vector<WorldBatch> world_batches_;
    ComPtr<ID3D12Resource> prop_vertex_buffer_;
    ComPtr<ID3D12Resource> map_prop_vertex_buffer_;
    ComPtr<ID3D12Resource> map_prop_index_buffer_;
    D3D12_VERTEX_BUFFER_VIEW map_prop_vertex_view_{};
    D3D12_INDEX_BUFFER_VIEW map_prop_index_view_{};
    D3D12_VERTEX_BUFFER_VIEW prop_vertex_view_{};
    ComPtr<ID3D12Resource> prop_index_buffer_;
    D3D12_INDEX_BUFFER_VIEW prop_index_view_{};
    ComPtr<ID3D12Resource> prop_transform_buffer_;
    std::vector<PropDraw> prop_batches_;
    UINT64 fence_value_ = 0;
    UINT last_rendered_frame_ = 0;
    HWND window_ = nullptr;
    bool camera_initialized_ = false;
    bool mouse_initialized_ = false;
    bool was_mouse_captured_ = false;
    POINT last_cursor_{};
    float camera_x_ = 0.0f;
    float camera_y_ = 1.2f;
    float camera_z_ = -2.4f;
    float camera_yaw_ = 0.0f;
    float camera_pitch_ = -0.25f;
    float initial_camera_x_ = 0.0f;
    float initial_camera_y_ = 0.0f;
    float initial_camera_z_ = 0.0f;
    float initial_camera_yaw_ = 0.0f;
    float initial_camera_pitch_ = -0.45f;
    float initial_camera_far_ = 100000.0f;
    std::array<float, 5> map_camera_{};
    std::chrono::steady_clock::time_point last_camera_time_{};
    HANDLE fence_event_ = nullptr;
};

// Drives dynamic instances from a demo: advances the playback clock, samples
// every actor at that time and hands the poses to the renderer.
class ReplayDriver {
public:
    ReplayDriver(gmod::demo::DemoData demo, std::vector<ActorBinding> actors,
                 gmod::source::bsp::BspProbe probe,
                 std::shared_ptr<gmod::source::model::PlayerAnimator> animator,
                 std::shared_ptr<OutfitData> outfit,
                 std::shared_ptr<DemoAudioAssets> audio_assets,
                 HWND window, bool show_timeline, bool follow_recorder)
        : demo_(std::move(demo)), actors_(std::move(actors)), probe_(std::move(probe)),
          animator_(std::move(animator)), outfit_(std::move(outfit)),
          audio_assets_(std::move(audio_assets)),
          fallback_model_(fallback_player_model(demo_)),
          door_ambient_(actors_.size()),
          prop_ambient_(actors_.size()),
          animation_states_(demo_.players.size()),
          animation_time_(demo_.players.size(), -1.0f),
          next_step_(demo_.players.size(), -1.0f),
          ragdolled_(demo_.players.size(), 0),
          ragdoll_origin_(demo_.players.size()),
          ragdoll_model_(demo_.players.size()) {
        for (const auto& clip : demo_.voice) {
            max_voice_duration_ = std::max(max_voice_duration_, clip.duration);
        }
        if (follow_recorder) {
            int recorder = demo_.recorder_player;
            if (recorder < 0 && !demo_.client_name.empty()) {
                for (std::size_t index = 0; index < demo_.players.size(); ++index) {
                    if (demo_.players[index].name == demo_.client_name
                        && !demo_.players[index].samples.empty()) {
                        recorder = static_cast<int>(index);
                        break;
                    }
                }
            }
            if (recorder >= 0) {
                follow_ = recorder;
                first_person_ = !demo_.recorder_camera.empty();
            }
        }
        window_ = window;
        free_view_ = follow_ < 0;
        create_timeline(window, !g_headless && show_timeline);
        preview_timeline_ = g_headless && show_timeline;
        create_player_list(window);
        radio_hud_ = std::make_unique<gmod::ui::RadioHud>(window, [this] { radio_muted_ = !radio_muted_; });
        dialog_hud_ = std::make_unique<gmod::ui::DialogHud>(window);
        hud_ = std::make_unique<gmod::ui::Hud>(window);
        if (!g_headless) hud_->set_visible(true);
        esc_menu_ = std::make_unique<gmod::ui::EscMenu>(window, gmod::ui::EscMenuActions{
            [this] { close_esc_menu(); },
            [this] { g_menu_requested = true; },
            [this] { PostMessageW(window_, WM_CLOSE, 0, 0); },
            [this] { settings_changed_ = true; }});
        console_ = std::make_unique<gmod::ui::GameConsole>(
            window, [this](const std::wstring& command) { run_console_command(command); },
            [this](const std::wstring& prefix) { return complete_console(prefix); });
        g_console = console_.get();
        for (std::size_t index = 0; index < demo_.props.size(); ++index) {
            if (demo_.props[index].entity > 0) prop_by_entity_[demo_.props[index].entity].push_back(static_cast<int>(index));
        }
        reconstruct_melee();
        reconstruct_doll_sounds();
        classify_world_sounds();
        build_feed_events();
        build_console_entries();
        started_ticks_ = GetTickCount();
        player_card_ = std::make_unique<gmod::ui::PlayerCard>(
            window,
            [this] {
                if (card_player_ >= 0) follow(card_player_);
                last_card_refresh_ = 0;
            },
            [this] {
                // Hear only this player (a second press goes back to everyone).
                if (card_player_ >= 0) {
                    if (effective_isolation() == card_player_) {
                        isolated_voice_ = -1;
                    } else {
                        isolated_voice_ = card_player_;
                    }
                    last_card_refresh_ = 0;
                }
            },
            [this] {
                if (card_player_ >= 0 && !muted_voices_.erase(card_player_)) muted_voices_.insert(card_player_);
                last_card_refresh_ = 0;
            },
            [this] {
                card_player_ = -1;
                card_vehicle_ = -1;
            });
        g_free_view = free_view_;
    }

    ~ReplayDriver() {
        g_player_list = nullptr;
        g_console = nullptr;
        g_text_input = false;
        g_free_view = true;
        g_dragging = false;
    }

    void seek(float time) {
        time_ = std::clamp(time, 0.0f, std::min(demo_.duration, viewable_));
        world_sound_seek_ = true;
        effect_sound_seek_ = true;
    }

    // How far into the recording the viewing policy lets playback go (nothing until cleared).
    void set_viewable(float seconds) {
        viewable_ = std::max(seconds, 0.0f);
        seek(time_);
    }

    // Chase the player with this index in the demo's player list.
    void follow(int player) {
        if (player >= 0 && static_cast<std::size_t>(player) < demo_.players.size()) {
            follow_ = player;
            set_free_view(false);
        }
    }

    // Follows the first player seen riding in a car, a few seconds into the ride (for
    // automated checks).
    void follow_first_driver() {
        for (std::size_t index = 0; index < demo_.players.size(); ++index) {
            const auto& samples = demo_.players[index].samples;
            for (std::size_t i = 0; i < samples.size(); ++i) {
                if ((samples[i].flags & gmod::demo::PlayerFlagInCar) == 0) continue;
                std::size_t last = i;
                while (last + 1 < samples.size() && samples[last + 1].segment == samples[i].segment) ++last;
                seek(std::min(samples[i].time + 3.0f, (samples[i].time + samples[last].time) * 0.5f));
                follow(static_cast<int>(index));
                std::cout << "follow-car: player " << index << " at " << time_ << "s\n";
                return;
            }
        }
        std::cout << "follow-car: nobody rides a car in this demo\n";
    }

    // Enables the recorded point of view for the followed player (the recorder's own
    // camera when it is the recorder).
    void set_first_person(bool enabled) { first_person_ = enabled; }

    // Keeps the player list open without Tab (for automated checks).
    void pin_player_list() { player_list_pinned_ = true; }
    void set_real_start(int seconds_of_day) { real_start_ = seconds_of_day; }
    // Opens the info card of a player (for automated checks).
    void open_card(int player) {
        if (player >= 0 && static_cast<std::size_t>(player) < demo_.players.size()) {
            card_player_ = player;
            card_vehicle_ = -1;
        }
    }
    // Simulates a click at a client point on the next frame and records what it selected
    // (for automated checks).
    void test_click(int x, int y, bool double_click = false) {
        g_click_pending = true;
        g_follow_click_pending = double_click;
        g_click_at = {x, y};
        click_test_ = true;
    }
    // Debug aid: draw players without the vehicles around them.
    void hide_vehicles() { hide_vehicles_ = true; }
    // Screenshot runs have no audio; this logs the world sounds that would have played.
    void log_world_sounds(std::filesystem::path path) { world_sound_log_path_ = std::move(path); }
    void reconstruct_fist_impacts(Dx12Foundation& renderer) {
        std::vector<std::string> sounds;
        for(int i=1;i<=5;++i) sounds.push_back("physics/flesh/flesh_impact_bullet"+std::to_string(i)+".wav");
        // A recorded glass/flesh hit takes precedence over any inferred contact.
        auto recorded=sounds;
        for(int i=1;i<=2;++i) recorded.push_back("physics/glass/glass_cup_break"+std::to_string(i)+".wav");
        recorded.push_back("physics/flesh/flesh_bloody_break.wav");
        std::vector<gmod::demo::DemoSound> inferred;
        for(std::size_t index=0;index<demo_.players.size();++index) {
            const auto& player=demo_.players[index];
            for(const auto& attack:player.attacks) {
                if(attack.value!=0) continue; // shove/grab are not punches
                const auto held=gmod::demo::held_weapon_at(demo_,player,attack.time);
                if(held.class_name!="roleplay_fists") continue;
                gmod::demo::PlayerPose pose;
                if(gmod::demo::player_incapacitated(player,attack.time) || !gmod::demo::sample_player(player,attack.time,pose)
                    || (pose.flags&gmod::demo::PlayerFlagSeated)!=0) continue;
                float pitch=pose.pitch,yaw=pose.yaw;
                gmod::demo::sample_player_look(demo_,static_cast<int>(index),attack.time,pitch,yaw);
                std::array<float,3> eye{pose.x,pose.y,pose.z+gmod::demo::player_eye_height(pose.flags)};
                gmod::demo::CameraKey camera;
                // Demo camera origins are feet positions, matching first_person_view().
                if(static_cast<int>(index)==demo_.recorder_player && gmod::demo::sample_recorder_camera(demo_,attack.time,camera)
                    && camera.origin_valid)
                    eye={camera.x,camera.y,camera.z+gmod::demo::player_eye_height(pose.flags)};
                if(gmod::demo::has_recorded_melee_sound(demo_,player.entity,held.entity,attack.time,recorded,&eye)) continue;
                constexpr float Radians=0.017453292519943295f;
                pitch*=Radians;yaw*=Radians;
                const std::array<float,3> forward{std::cos(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw),-std::sin(pitch)};
                const std::array<float,3> up{std::sin(pitch)*std::cos(yaw),std::sin(pitch)*std::sin(yaw),std::cos(pitch)};
                std::array<float,3> start{},end{};
                for(int axis=0;axis<3;++axis) {start[axis]=eye[axis]-3*up[axis];end[axis]=start[axis]+50*forward[axis];}
                bool prop=false;float fraction=renderer.fist_static_fraction(start,end,prop);
                const char* kind=prop?"map-prop":"world";
                for(std::size_t other=0;other<demo_.players.size();++other) {
                    if(other==index) continue;
                    gmod::demo::PlayerPose target;
                    if(!gmod::demo::sample_player(demo_.players[other],attack.time,target)
                        || gmod::demo::player_incapacitated(demo_.players[other],attack.time)) continue;
                    const float hit=gmod::demo::fist_player_fraction(start,end,target);
                    if(hit<fraction) {fraction=hit;kind=demo_.players[other].npc?"npc":"player";}
                }
                float distance2=0;
                for(int axis=0;axis<3;++axis) {const float d=start[axis]+(end[axis]-start[axis])*fraction-eye[axis];distance2+=d*d;}
                const bool hit=fraction<1 && distance2<=45*45 && std::string_view(kind)!="npc";
                if(g_headless) std::cout << "FIST_CONTACT time=" << attack.time << " player=" << player.name
                    << " kind=" << (hit?kind:"miss") << " distance=" << std::sqrt(distance2) << "\n";
                if(!hit) continue;
                gmod::demo::DemoSound sound;sound.time=attack.time;sound.entity=player.entity;
                sound.channel=1;sound.index=-1;
                sound.name=sounds[static_cast<std::size_t>(std::lround(attack.time*demo_.tick_rate))%sounds.size()];
                sound.volume=1;sound.level=75;sound.x=eye[0];sound.y=eye[1];sound.z=eye[2];
                inferred.push_back(sound);
                if(g_headless) std::cout << "FIST_IMPACT_RECONSTRUCTED time=" << sound.time << " kind=" << kind << " file=" << sound.name << "\n";
            }
        }
        demo_.sounds.insert(demo_.sounds.end(),inferred.begin(),inferred.end());
        std::stable_sort(demo_.sounds.begin(),demo_.sounds.end(),[](const auto& a,const auto& b){return a.time<b.time;});
    }

    void test_fist_impacts(Dx12Foundation& renderer,const std::filesystem::path& folder) {
        if(demo_.recorder_player<0) throw std::runtime_error("Fist impact test requires the latest recorder fixture.");
        const int recorder=demo_.players[demo_.recorder_player].entity;
        const auto find=[&](float time) {
            return std::find_if(demo_.sounds.begin(),demo_.sounds.end(),[&](const auto& sound) {
                return sound.entity==recorder && sound.index==-1 && sound.name.starts_with("physics/flesh/flesh_impact_bullet")
                    && std::fabs(sound.time-time)<0.04f;
            });
        };
        if(find(3530.3143f)!=demo_.sounds.end()) throw std::runtime_error("Missed punch incorrectly makes impact sound.");
        const auto tree=find(3541.3429f),person=find(3546.4571f);
        if(tree==demo_.sounds.end() || person==demo_.sounds.end()) throw std::runtime_error("Recorded tree/player fist contact has no sound.");
        const auto count=demo_.sounds.size();const float original_time=time_;
        const int original_follow=follow_;const bool original_free=free_view_,original_first=first_person_;
        follow_=demo_.recorder_player;free_view_=false;first_person_=true;
        if(world_sound_log_path_.empty()) world_sound_log_path_=folder/L"fist-impact-audio.txt";
        const auto capture=[&](const gmod::demo::DemoSound& sound,const wchar_t* name) {
            seek(sound.time+0.02f);update(renderer,0,false);renderer.render();
            camera_position_=renderer.listener_source();
            apply_world_sound(renderer,sound,true);
            renderer.capture_bmp((folder/name).wstring());
        };
        capture(*tree,L"fist-tree.bmp");capture(*person,L"fist-player.bmp");capture(*tree,L"fist-tree-rewound.bmp");
        if(demo_.sounds.size()!=count) throw std::runtime_error("Rewinding duplicated reconstructed fist sounds.");
        if(world_sound_log_.is_open()) world_sound_log_.flush();
        follow_=original_follow;free_view_=original_free;first_person_=original_first;seek(original_time);
        update(renderer,0,false);renderer.render();
        std::ofstream(folder/L"fist-impact-test.txt") << "PASS actual recorder: silent misses, tree impact, player impact, audio playback and rewind without duplicate events\n";
    }

    void test_melee_render(Dx12Foundation& renderer,const std::filesystem::path& folder) {
        const int original_follow=follow_;const bool original_free=free_view_,original_first=first_person_;
        const float original_time=time_;
        follow_=demo_.recorder_player;free_view_=false;first_person_=true;
        if(follow_<0) throw std::runtime_error("Melee render test needs a recorder.");
        const auto capture=[&](float time,const wchar_t* name) {
            seek(time);update(renderer,0,false);renderer.render();renderer.capture_bmp((folder/name).wstring());
            if(vm_id_<0 || rendered_arm_parts_==0) throw std::runtime_error("First-person arm clothing missing.");
        };
        capture(3530.1f,L"melee-idle.bmp");const auto idle=arm_palette_;
        capture(3530.45f,L"melee-swing.bmp");const auto swing=arm_palette_;
        capture(3530.65f,L"melee-follow-through.bmp");
        capture(3531.15f,L"melee-recovery.bmp");capture(3530.45f,L"melee-rewound.bmp");
        if(idle==swing || swing!=arm_palette_) throw std::runtime_error("Fist swing or rewind bone palettes differ.");
        const auto& player=demo_.players[follow_];
        const auto attack=std::find_if(player.attacks.begin(),player.attacks.end(),[](const auto& key){return key.value==0 && std::fabs(key.time-3530.3143f)<0.05f;});
        if(attack==player.attacks.end()) throw std::runtime_error("Recorder's fist swing missing from third-person timeline.");
        first_person_=false;seek(3530.45f);update(renderer,0,false);renderer.render();
        renderer.capture_bmp((folder/L"melee-third-person.bmp").wstring());
        const auto sound=std::find_if(demo_.sounds.begin(),demo_.sounds.end(),[](const auto& event) {
            return event.index==-1 && event.name.starts_with("npc/vort/claw_swing");
        });
        if(sound==demo_.sounds.end()) throw std::runtime_error("Missing client melee sound reconstruction.");
        seek(sound->time);camera_position_={sound->x,sound->y,sound->z};
        apply_world_sound(renderer,*sound,true);
        if(world_sound_log_.is_open()) world_sound_log_.flush();
        follow_=original_follow;free_view_=original_free;first_person_=original_first;seek(original_time);
        update(renderer,0,false);renderer.render();
        std::ofstream(folder/L"melee-render-test.txt") << "PASS recorded fist swing, clothing, restart counters, third-person attacks, missing melee audio and rewind\n";
    }

    void test_back_weapons(Dx12Foundation& renderer,const std::filesystem::path& folder) {
        const auto player=std::find_if(demo_.players.begin(),demo_.players.end(),[](const auto& p){return p.name=="sl_cx";});
        if(player==demo_.players.end()) throw std::runtime_error("Back gun render test requires the sl_cx recording.");
        const int index=static_cast<int>(player-demo_.players.begin());
        const auto gun=std::find_if(demo_.props.begin(),demo_.props.end(),[&](const auto& p){return p.back_gun && gmod::demo::back_gun_player_at(demo_,p,2137)==index;});
        if(gun==demo_.props.end()) throw std::runtime_error("Missing sl_cx recorded back gun.");
        const auto original_time=time_;
        const bool original_free=free_view_,original_first=first_person_;
        const int original_follow=follow_;
        const auto check=[&](float time,bool visible,const wchar_t* name) {
            seek(time);update(renderer,0,false);renderer.render();
            const bool drawn=std::find(rendered_back_guns_.begin(),rendered_back_guns_.end(),gun->entity)!=rendered_back_guns_.end();
            if(drawn!=visible) throw std::runtime_error("Back gun visibility regression at "+std::to_string(time));
            renderer.capture_bmp((folder/name).wstring());
        };
        check(2137,true,L"back-gun-holstered.bmp");check(2139.5f,false,L"back-gun-drawn.bmp");
        check(2140,false,L"back-gun-dead.bmp");check(2137,true,L"back-gun-rewound.bmp");
        free_view_=false;first_person_=true;follow_=index;
        check(2137,false,L"back-gun-first-person.bmp");
        free_view_=original_free;first_person_=original_first;follow_=original_follow;seek(original_time);
        update(renderer,0,false);renderer.render();
        std::ofstream(folder/L"back-gun-render-test.txt") << "PASS real sl_cx shotgun: holstered, drawn, death, rewind and first-person suppression\n";
    }

    void test_model_identifiers(Dx12Foundation& renderer, const std::filesystem::path& folder) {
        if (!outfit_) throw std::runtime_error("Identifier render test needs recorded outfits.");
        const auto original_time = time_;
        const auto original_follow = follow_;
        const auto original_free = free_view_, original_first = first_person_;
        free_view_ = true;
        first_person_ = false;
        renderer.set_frame_text_painter({});
        const auto capture = [&](float time, const std::array<float,3>& point,
                                 const std::array<float,3>& normal, const wchar_t* name, float distance = 45) {
            seek(time);
            constexpr float Degrees = 57.29577951308232f;
            renderer.set_camera(point[0] + normal[0] * distance, point[1] + normal[1] * distance,
                point[2] + normal[2] * distance,
                std::atan2(-normal[1], -normal[0]) * Degrees,
                std::atan2(normal[2], std::hypot(normal[0], normal[1])) * Degrees);
            update(renderer, 0, false);
            renderer.render();
            renderer.capture_bmp((folder / name).wstring());
        };
        bool tested_plate = false;
        for (const auto& vehicle : demo_.props) {
            if (!vehicle.vehicle || tested_plate) continue;
            for (const auto& key : vehicle.keys) {
                if (!key.present || !key.has_position) continue;
                const float time = key.time + 0.01f;
                const auto state = gmod::demo::vehicle_plate_at(vehicle, time);
                const auto def = outfit_->registry.vehicle_plates.find(state.id);
                gmod::demo::PropPose pose;
                if (state.registration.empty() || def == outfit_->registry.vehicle_plates.end()
                    || def->second.placements.size() != 2 || !gmod::demo::sample_prop(vehicle,time,pose)) continue;
                const auto plate = gmod::demo::plate_transform(pose, def->second.placements.front());
                const std::array<float,3> point{plate[3],plate[7],plate[11]}, normal{plate[0],plate[4],plate[8]};
                capture(time,point,normal,L"plate-rear.bmp");
                const auto expected = rendered_plate_poses_;
                const auto vehicle_index = static_cast<int>(&vehicle - demo_.props.data());
                if (std::count_if(expected.begin(),expected.end(),[&](const auto& entry) {
                    return actors_.at(entry.first).index == vehicle_index;
                }) != 2) throw std::runtime_error("Front/rear plates not submitted to renderer.");
                seek(time + 5); update(renderer,0,false); renderer.render();
                capture(time,point,normal,L"plate-rear-rewound.bmp");
                if (expected != rendered_plate_poses_) throw std::runtime_error("Plate transforms changed on rewind.");
                const auto front = gmod::demo::plate_transform(pose, def->second.placements.back());
                capture(time,{front[3],front[7],front[11]},{front[0],front[4],front[8]},L"plate-front.bmp");
                tested_plate = true;
                break;
            }
        }
        bool tested_badge = false;
        for (const float badge_time : {300.0f,301.0f,305.0f}) {
            seek(badge_time);
            update(renderer,0,false);
            for (std::size_t part = 0; part < outfit_->parts.size(); ++part) {
                if (outfit_->parts[part].data_number <= 0) continue;
                const auto player = static_cast<std::size_t>(outfit_->owner[part]);
                gmod::demo::PlayerPose pose;
                if (!outfit_part_worn(player,static_cast<int>(part))
                    || !gmod::demo::sample_player(demo_.players[player],time_,pose)) continue;
                const auto probe = outfit_->badge_probes.find(static_cast<int>(part));
                if (probe == outfit_->badge_probes.end())
                    throw std::runtime_error("Shoulder badge digit geometry is missing.");
                const auto& vertex = probe->second;
                const std::array<float,3> vertex_point{vertex.position[0],vertex.position[1],vertex.position[2]};
                const std::array<float,3> vertex_tip{vertex.position[0]+vertex.normal[0],
                    vertex.position[1]+vertex.normal[1],vertex.position[2]+vertex.normal[2]};
                std::array<float,3> local{}, local_tip{};
                const float weights[]{static_cast<float>(vertex.skin_weights & 0xFFFFu)/65535,
                    static_cast<float>(vertex.skin_weights >> 16)/65535,0};
                for (unsigned i = 0; i < (vertex.skin_bones >> 24); ++i) {
                    const int bone = (vertex.skin_bones >> (8*i)) & 0xFF;
                    const float weight = i == 2 ? std::max(0.0f,1-weights[0]-weights[1]) : weights[i];
                    const auto p = renderer.skinned_point(player,bone,vertex_point);
                    const auto tip = renderer.skinned_point(player,bone,vertex_tip);
                    for (int axis = 0; axis < 3; ++axis) {
                        local[axis] += weight*p[axis];
                        local_tip[axis] += weight*tip[axis];
                    }
                }
                const float origin[]{pose.x,pose.y,pose.z}, angles[]{0,pose.yaw,0};
                const auto body = gmod::source::model::matrix_from_source_angles(origin,angles);
                std::array<float,3> point{},normal{};
                for (int row = 0; row < 3; ++row) {
                    point[row] = body[row*4+3];
                    for (int col = 0; col < 3; ++col) {
                        point[row] += body[row*4+col]*local[col];
                        normal[row] += body[row*4+col]*(local_tip[col]-local[col]);
                    }
                }
                const float length = std::hypot(normal[0],normal[1],normal[2]);
                if (length < 0.5f) throw std::runtime_error("Invalid animated shoulder badge normal.");
                for (auto& axis : normal) axis /= length;
                const auto name = L"officer-badge-" + std::to_wstring(outfit_->parts[part].data_number)
                    + (badge_time == 300 ? L"" : L"-time-" + std::to_wstring(static_cast<int>(badge_time)));
                capture(badge_time,point,normal,(name+L".bmp").c_str(),10);
                if (!rendered_badge_parts_.contains(static_cast<int>(part)))
                    throw std::runtime_error("Recorded uniform badge not submitted to renderer.");
                seek(badge_time+5); update(renderer,0,false); renderer.render();
                capture(badge_time,point,normal,(name+L"-rewound.bmp").c_str(),10);
                tested_badge = true;
            }
        }
        if (!tested_plate || !tested_badge) throw std::runtime_error("Identifier render fixture missing.");
        free_view_ = original_free; first_person_ = original_first; follow_ = original_follow;
        seek(original_time); update(renderer,0,false); renderer.render();
        std::ofstream(folder / L"identifiers-render-test.txt")
            << "PASS original front/rear plate meshes, recorded uniform badge, seek and exact plate-transform rewind\n";
    }

    void test_fire_render(Dx12Foundation& renderer,const std::filesystem::path& folder) {
        const auto* material=renderer.effect_material("effects/perp2_flame");
        if(!material || material->vtf.empty()) throw std::runtime_error("Missing Perpheads flame texture.");
        const auto original=demo_.fires;
        const float original_time=time_;
        // Name tags have their own timing; compare the fire pixels without HUD text.
        renderer.set_frame_text_painter({});
        // Place controlled fires on the pavement in front of the bank; no recorded fire is required.
        demo_.fires={{1,8100,1,{-9000,2700,1326},0.6f,true},
                     {1,8101,1,{-9150,2700,1326},1.0f,true},
                     {1,8102,1,{-9300,2700,1326},1.5f,true},
                     {3,8100,1,{-9000,2700,1326},0.6f,false},
                     {3,8101,1,{-9150,2700,1326},1.0f,false},
                     {3,8102,1,{-9300,2700,1326},1.5f,false}};
        const auto test_keys=demo_.fires;
        demo_.fires.clear();fire_playback_.reset();
        const auto capture=[&](float time,const wchar_t* name) {
            seek(time);update(renderer,0,false);renderer.render();renderer.capture_bmp((folder/name).wstring());
        };
        // These fires occupy the left three quarters of this camera view. Distant
        // actors on the right have independent animation state; compare the fire region.
        const auto bytes=[&](const wchar_t* name) {
            std::ifstream file(folder/name,std::ios::binary);
            const std::vector<char> data(std::istreambuf_iterator<char>(file),{});
            if(data.size()<sizeof(BITMAPFILEHEADER)+sizeof(BITMAPINFOHEADER)) return std::vector<char>{};
            BITMAPFILEHEADER header{};BITMAPINFOHEADER info{};
            std::memcpy(&header,data.data(),sizeof(header));
            std::memcpy(&info,data.data()+sizeof(header),sizeof(info));
            if(info.biWidth<=0 || info.biHeight<=0 || (info.biBitCount!=24 && info.biBitCount!=32)) return std::vector<char>{};
            const auto pixel_bytes=info.biBitCount/8;
            const auto stride=(std::size_t(info.biWidth)*pixel_bytes+3)&~std::size_t(3);
            if(header.bfOffBits+stride*info.biHeight>data.size()) return std::vector<char>{};
            std::vector<char> foreground;
            for(int y=0;y<info.biHeight;++y) {
                const auto first=data.begin()+header.bfOffBits+stride*y;
                foreground.insert(foreground.end(),first,first+std::size_t(info.biWidth)*pixel_bytes*3/4);
            }
            return foreground;
        };
        capture(2.0f,L"fire-unlit.bmp");
        demo_.fires=test_keys;fire_playback_.reset();
        capture(0.5f,L"fire-before.bmp");capture(2.0f,L"fire-lit.bmp");
        capture(2.0f,L"fire-paused.bmp");capture(2.15f,L"fire-moving.bmp");
        capture(3.5f,L"fire-extinguished.bmp");capture(2.0f,L"fire-rewound.bmp");
        const auto lit=bytes(L"fire-lit.bmp");
        if(lit.empty() || lit==bytes(L"fire-unlit.bmp") || lit==bytes(L"fire-before.bmp") || lit==bytes(L"fire-extinguished.bmp")
            || lit==bytes(L"fire-moving.bmp") || lit!=bytes(L"fire-paused.bmp") || lit!=bytes(L"fire-rewound.bmp"))
            throw std::runtime_error("Fire rendering, pause, extinguish or rewind failed.");
        demo_.fires=original;fire_playback_.reset();seek(original_time);update(renderer,0,false);renderer.render();
        renderer.set_frame_text_painter([this,&renderer](HDC dc,int width,int height) {
            return paint_frame_labels(dc,width,height,renderer);
        });
        std::ofstream(folder/L"fire-render-test.txt") << "PASS controlled fire sizes, animation, pause, extinguish and exact rewind\n";
    }

    void test_bank_shutters(Dx12Foundation& renderer,const std::filesystem::path& folder) {
        struct Shutter { std::size_t index;int open,close,moving;std::vector<gmod::demo::DemoProp::Animation> original; };
        std::vector<Shutter> shutters;
        for(const auto& actor:actors_) if(actor.kind==9 && actor.palette>=0 && gmod::demo::is_bank_shutter(actor.model_path)) {
            const auto& animation=*outfit_->vehicle_assets.at(actor.model).animation;
            int open=animation.find_sequence("idleup"),close=animation.find_sequence("idledown");
            const int moving=animation.find_sequence("shutterdown");
            if(open<0) open=animation.find_sequence("shutterupidle");
            if(close<0) close=animation.find_sequence("shutterdownidle");
            if(open<0 || close<0 || moving<0) throw std::runtime_error("Bank shutter is missing authored open/close sequences.");
            const auto index=std::size_t(actor.index);
            shutters.push_back({index,open,close,moving,demo_.props[index].animations});
        }
        if(shutters.size()!=2) throw std::runtime_error("Expected exactly two animated bank shutters.");
        const auto capture=[&](int mode,const wchar_t* name) {
            for(const auto& shutter:shutters)
                demo_.props[shutter.index].animations={{0,mode==0?shutter.open:mode==1?shutter.moving:shutter.close,mode==1?0.5f:0.0f,0}};
            update(renderer,0,false);renderer.render();renderer.capture_bmp((folder/name).wstring());
        };
        capture(0,L"shutters-open.bmp");capture(1,L"shutters-moving.bmp");capture(2,L"shutters-closed.bmp");
        const auto count=bank_shutter_palette_updates_;
        for(int frame=0;frame<10;++frame) { update(renderer,0,false);renderer.render(); }
        if(count!=bank_shutter_palette_updates_) throw std::runtime_error("Stopped bank shutters rebuild their skeleton every frame.");
        capture(0,L"shutters-rewound.bmp");
        const auto bytes=[&](const wchar_t* name) {
            std::ifstream file(folder/name,std::ios::binary);
            return std::vector<char>(std::istreambuf_iterator<char>(file),{});
        };
        const auto open=bytes(L"shutters-open.bmp"),closed=bytes(L"shutters-closed.bmp"),moving=bytes(L"shutters-moving.bmp");
        if(open.empty() || open==closed || moving==open || moving==closed || open!=bytes(L"shutters-rewound.bmp"))
            throw std::runtime_error("Shutter transition or rewind image test failed.");
        for(auto& shutter:shutters) demo_.props[shutter.index].animations=std::move(shutter.original);
        update(renderer,0,false);renderer.render();
        std::ofstream(folder/L"bank-shutters-test.txt") << "PASS two bank shutters: open, closing, closed, exact rewind; zero idle palette updates\n";
    }

    void test_baked_bank_lighting(Dx12Foundation& renderer,const std::filesystem::path& folder) {
        const auto original_keys=demo_.bank_lights;
        auto& options=gmod::ui::settings();
        const bool original_bank=options.bank_raid_lighting,original_lighting=options.lighting;
        options.bank_raid_lighting=options.lighting=true;
        const auto ids=renderer.bank_styles();
        if(std::any_of(ids.begin(),ids.end(),[](int id){return id<0;}))
            throw std::runtime_error("Bank test requires the bank map's lightstyles.");
        const auto set=[&](const gmod::source::bsp::BankLightWeights& weights) {
            demo_.bank_lights.clear();
            for(int i=0;i<4;++i) demo_.bank_lights.push_back({0,ids[i],weights[i]});
            update(renderer,0,false);renderer.render();
        };
        set({1,1,0,0});renderer.capture_bmp((folder/L"bank-baked-normal.bmp").wstring());
        auto uploads=renderer.bank_lightmap_uploads();
        for(int i=0;i<20;++i) { update(renderer,0,false);renderer.render(); }
        if(renderer.bank_lightmap_uploads()!=uploads) throw std::runtime_error("Steady normal bank lighting uploaded every frame.");
        set({0,0,1,1});renderer.capture_bmp((folder/L"bank-baked-raid.bmp").wstring());
        if(renderer.bank_lightmap_uploads()!=uploads+1) throw std::runtime_error("Raid bank lighting did not update exactly once.");
        uploads=renderer.bank_lightmap_uploads();
        for(int i=0;i<20;++i) { update(renderer,0,false);renderer.render(); }
        if(renderer.bank_lightmap_uploads()!=uploads) throw std::runtime_error("Steady raid bank lighting uploaded every frame.");
        set({1,1,0,0});renderer.capture_bmp((folder/L"bank-baked-restored.bmp").wstring());
        const auto bytes=[&](const wchar_t* name) {
            std::ifstream file(folder/name,std::ios::binary);
            return std::vector<char>(std::istreambuf_iterator<char>(file),{});
        };
        const auto normal=bytes(L"bank-baked-normal.bmp");
        if(normal.empty() || normal!=bytes(L"bank-baked-restored.bmp") || normal==bytes(L"bank-baked-raid.bmp"))
            throw std::runtime_error("Baked bank rendering/rewind did not restore exactly.");
        double baseline_ms=0,baked_ms=0;
        for(int round=0;round<4;++round) for(int pass=0;pass<2;++pass) {
            const bool enabled=(round+pass)%2;
            options.bank_raid_lighting=enabled;
            for(int warm=0;warm<5;++warm) update(renderer,0,false);
            const auto start=std::chrono::steady_clock::now();
            for(int frame=0;frame<100;++frame) update(renderer,0,false);
            const auto elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/100;
            (enabled ? baked_ms:baseline_ms)+=elapsed;
        }
        demo_.bank_lights=original_keys;
        options.bank_raid_lighting=original_bank;options.lighting=original_lighting;
        update(renderer,0,false);renderer.render();
        std::ofstream report(folder/L"bank-baked-test.txt");
        report << "PASS normal/raid differ; normal restores exactly; 40 steady frames upload no lightmaps\n";
        report << "REPLAY_UPDATE baseline_ms=" << baseline_ms/4 << " baked_ms=" << baked_ms/4 << "\n";
    }

    void test_world_sound_resets(Dx12Foundation& renderer) {
        playing_ = false;
        update(renderer, 0.0f, false);
        update(renderer, 0.0f, false);
        playing_ = true;
        update(renderer, 0.0f, false);
        seek(time_ + 0.25f);
        update(renderer, 0.0f, false);
        seek(time_ - 1.0f);
        update(renderer, 0.0f, false);
    }
    // Screenshot aids: show the Esc menu or the console in the composited picture.
    void preview_menu(bool help = false) {
        preview_menu_ = true;
        if (help) esc_menu_->preview_help();
    }
    void preview_console(const std::wstring& command) {
        preview_console_ = true;
        preview_command_ = command;
    }

    bool paint_frame_labels(HDC dc,int width,int height,const Dx12Foundation& renderer) {
        if (!gmod::ui::settings().names) return false;
        camera_position_=renderer.listener_source();
        const auto tags=build_view_tags(renderer,width,height);
        if (tags.empty()) return false;
        if (g_headless) std::cout << "FRAME_ESP labels=" << tags.size() << '\n';
        gmod::ui::NameTags::paint(dc,width,height,tags,renderer.scope_overlay());
        return true;
    }

    // ESP labels are already in the captured swap-chain image. Remaining
    // window-based controls/HUD are composited here for screenshot previews.
    void paint_overlays(HDC dc, int width, int height, const Dx12Foundation& renderer,
                        const std::array<float, 3>& camera) {
        camera_position_ = camera;
        const auto& options = gmod::ui::settings();
        if (options.names) {
            if (g_headless) {
                const auto lens = renderer.scope_overlay();
                const auto tags = build_view_tags(renderer, width, height);
                std::cout << "SCOPE_OVERLAY triangles=" << lens.triangles.size()
                    << " scoped_tags=" << std::count_if(tags.begin(), tags.end(), [](const auto& tag) { return tag.scoped; }) << "\n";
            }
        }
        gmod::ui::Hud::paint(dc, width, height, build_hud_state());
        if (preview_console_) {
            if (!preview_ran_) {
                preview_ran_ = true;
                sync_console(true);
                if (!preview_command_.empty()) {
                    console_->print(L"] " + preview_command_, RGB(255, 235, 120));
                    run_console_command(preview_command_);
                }
            }
            console_->paint_preview(dc, width, height);
        }
        if (preview_menu_) esc_menu_->paint_preview(dc, width, height);
        if (preview_timeline_ && timeline_) {
            timeline_->update({time_, demo_.duration, playing_, reverse_, speed_index_,
                               free_view_ ? std::wstring() : follow_name(), real_start_});
            timeline_->paint_preview(dc, width, height);
        }
    }

    // `focused` gates keyboard input; `delta` is the real elapsed time in seconds.
    void update(Dx12Foundation& renderer, float delta, bool focused) {
        if (player_list_pinned_ && !player_list_open_ && !g_headless) open_player_list();
        camera_position_ = renderer.listener_source();
        if (g_console_key_pending) {
            g_console_key_pending = false;
            if (focused && console_) console_->toggle();
        }
        reconcile_console();
        if (focused) {
            handle_keys();
        } else {
            if (player_list_open_ && !player_list_pinned_) close_player_list(false);
            close_controls(false);
        }
        g_esc_consumed = false;
        if (playing_) {
            time_ += delta * kSpeeds[speed_index_] * (reverse_ ? -1.0f : 1.0f);
            if (time_ >= std::min(demo_.duration, viewable_)) {
                time_ = std::min(demo_.duration, viewable_);
                playing_ = false;
            } else if (time_ <= 0.0f) {
                time_ = 0.0f;
                playing_ = false;
            }
        }
        apply(renderer);
        camera_position_ = renderer.listener_source();
        update_voices(renderer);
        update_world_sounds(renderer);
        update_effects(renderer);
        if (auto* audio = renderer.audio()) {
            apply_audio_settings(audio);
        }
        renderer.set_demo_status(status());
        renderer.set_water_time(time_);
        update_picking(renderer);
        if (g_headless) {
            // Nothing is drawn as a window; the card and dialog state are still computed by
            // the test hooks that need them.
            return;
        }
        const auto& options = gmod::ui::settings();
        if (player_card_) {
            const DWORD now = GetTickCount();
            if (last_card_refresh_ == 0 || now - last_card_refresh_ >= 200) {
                player_card_->update(build_card_state());
                last_card_refresh_ = now;
            }
        }
        if (radio_hud_) radio_hud_->update(options.radio_hud ? build_radio_state() : gmod::ui::RadioHudState{});
        if (dialog_hud_) dialog_hud_->update(options.dialogs ? build_dialog_state() : gmod::ui::DialogHudState{});
        if (hud_) hud_->update(build_hud_state());
        if (esc_menu_) esc_menu_->update();
        if (console_) {
            if (console_->visible()) sync_console(false);
            console_->update();
        }
        if (timeline_) {
            timeline_->update({time_, demo_.duration, playing_, reverse_, speed_index_,
                               free_view_ ? std::wstring() : follow_name(), real_start_});
        }
        if (player_list_open_) {
            const DWORD now = GetTickCount();
            if (now - last_list_refresh_ >= 250) refresh_player_list();
            player_list_->update();
        }
    }

    void set_door_states(bool enabled) { use_doors_ = enabled; }

private:
    static constexpr float kSpeeds[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f};
    // XAudio's minimum source rate divided by the fastest audible demo speed.
    static constexpr float OneShotLookback = gmod::audio::AudioSystem::MaxOneShotSourceSeconds * 4.0f / 0.25f;

    void play_forward() {
        reverse_ = false;
        if (time_ >= demo_.duration) seek(0.0f);
        playing_ = true;
    }

    // True when the player is transmitting on a radio channel the recording player is in.
    bool transmitting_on_recorder_channel(const gmod::demo::DemoPlayer& player, float time) const {
        const int channel = gmod::demo::timed_int_at(player.radio_channel, time, 0);
        if (channel <= 0) return false;
        const auto* sync = gmod::demo::radio_sync_at(demo_, time);
        return sync && std::find(sync->channels.begin(), sync->channels.end(), channel)
            != sync->channels.end();
    }

    // The NPC dialogue box: opened by perpheads_npc_used (the NPC's name comes from its Lua
    // definition) or by a perpheads_npc_notify message, which carries its own text.
    gmod::ui::DialogHudState build_dialog_state() const {
        if (!first_person_ || free_view_ || follow_ < 0) return {};
        if (!demo_.npc_gui.empty()) {
            gmod::ui::DialogHudState state;
            const auto* current = gmod::demo::npc_gui_at(demo_,time_);
            if (!current) return state;
            const auto& page = *current;
            state.visible = page.visible;
            state.title = to_wide(page.title);
            if (!page.text.empty()) state.lines.push_back(to_wide(page.text));
            for (const auto& option : page.options) state.options.push_back(to_wide(option));
            state.reconstructed = page.inferred;
            return state;
        }
        constexpr float DialogSeconds = 8.0f;
        gmod::ui::DialogHudState state;
        const gmod::demo::NpcUse* use = nullptr;
        for (const auto& candidate : demo_.npc_uses) {
            if (candidate.time <= time_ && time_ < candidate.time + DialogSeconds) use = &candidate;
        }
        const gmod::demo::NpcNotice* notice = nullptr;
        for (const auto& candidate : demo_.npc_notices) {
            if (candidate.time <= time_ && time_ < candidate.time + DialogSeconds) notice = &candidate;
        }
        if (!use && !notice) return state;
        state.visible = true;
        if (notice && (!use || notice->time >= use->time)) {
            state.title = L"Notice";
            state.lines.push_back(to_wide(notice->text));
            if (!notice->exit_text.empty()) state.lines.push_back(to_wide(notice->exit_text));
            return state;
        }
        std::string name = "NPC " + std::to_string(use->npc_id);
        if (outfit_) {
            const auto known = outfit_->registry.npc_names.find(use->npc_id);
            if (known != outfit_->registry.npc_names.end()) name = known->second;
        }
        state.title = to_wide(name);
        state.lines.push_back(L"Talking to this NPC.");
        return state;
    }

    // The radio HUD: for each channel the recorder is in, who is transmitting right now.
    gmod::ui::RadioHudState build_radio_state() const {
        gmod::ui::RadioHudState state;
        const auto* sync = gmod::demo::radio_sync_at(demo_, time_);
        if (!sync || sync->channels.empty()) return state;
        state.available = true;
        state.muted = radio_muted_;
        for (const int id : sync->channels) {
            gmod::ui::RadioHudChannel channel;
            std::string name;
            if (outfit_) {
                const auto fixed = outfit_->registry.radio_channels.find(id);
                if (fixed != outfit_->registry.radio_channels.end()) name = fixed->second;
            }
            if (name.empty()) {
                const auto named = demo_.radio_channel_names.find(id);
                name = named != demo_.radio_channel_names.end() ? named->second
                                                                : "Radio channel " + std::to_string(id);
            }
            channel.name = to_wide(name);
            for (std::size_t index = 0; index < demo_.players.size(); ++index) {
                const auto& player = demo_.players[index];
                if (gmod::demo::timed_int_at(player.radio_channel, time_, 0) != id) continue;
                std::string text;
                std::string title;
                unsigned char rgb[3]{0, 255, 0};
                if (outfit_) {
                    title = gmod::demo::job_title_at(outfit_->registry, player, time_);
                    gmod::demo::job_color_at(outfit_->registry, player, time_, rgb);
                }
                std::string who = gmod::demo::character_name_at(player, time_);
                if (who.empty()) who = display_name(index);
                // The server's HUD shows rank and name for emergency services.
                const bool service = outfit_
                    && gmod::demo::timed_int_at(player.job_team, time_, outfit_->registry.team_citizen)
                        != outfit_->registry.team_citizen;
                text = (service && !title.empty() ? title + " | " : std::string()) + who;
                channel.talking.push_back({to_wide(text), RGB(rgb[0], rgb[1], rgb[2])});
            }
            if (channel.talking.empty()) continue;
            if (std::find(sync->muted.begin(), sync->muted.end(), id) != sync->muted.end()) {
                channel.name += L" (muted)";
            }
            state.channels.push_back(std::move(channel));
        }
        return state;
    }

    // Starts, moves and stops the decoded voice clips so they follow the demo clock.
    void update_voices(Dx12Foundation& renderer) {
        auto* audio = renderer.audio();
        if (!audio || demo_.voice.empty()) return;
        const float rate = kSpeeds[speed_index_];
        const bool audible = playing_ && !reverse_ && rate <= 4.0f;
        const bool jumped = std::fabs(time_ - last_voice_time_) > std::max(rate, 1.0f);
        last_voice_time_ = time_;
        // Changing who is isolated restarts the voices so the change is heard at once.
        const int only = effective_isolation();
        if (applied_isolation_ != only || applied_muted_ != muted_voices_.size()) {
            applied_isolation_ = only;
            applied_muted_ = muted_voices_.size();
            audio->voice_stop_all();
            voice_active_.clear();
        }
        if ((!audible || jumped) && !voice_active_.empty()) {
            audio->voice_stop_all();
            voice_active_.clear();
        }
        if (!audible) return;

        const auto position_of = [&](const gmod::demo::DemoVoiceClip& clip, float out[3]) {
            const auto& player = demo_.players[static_cast<std::size_t>(clip.player)];
            gmod::demo::PlayerPose pose;
            if (!gmod::demo::sample_player(player, time_, pose)) {
                if (player.samples.empty()) return false;
                const auto after = std::upper_bound(
                    player.samples.begin(), player.samples.end(), time_,
                    [](float value, const gmod::demo::PlayerSample& sample) {
                        return value < sample.time;
                    });
                const auto& last = after == player.samples.begin() ? player.samples.front()
                                                                  : *(after - 1);
                pose.x = last.x;
                pose.y = last.y;
                pose.z = last.z;
            }
            out[0] = pose.x;
            out[1] = pose.y;
            out[2] = pose.z + 64.0f;
            return true;
        };

        const auto first = std::lower_bound(
            demo_.voice.begin(), demo_.voice.end(), time_ - max_voice_duration_,
            [](const gmod::demo::DemoVoiceClip& clip, float value) { return clip.time < value; });
        for (auto it = first; it != demo_.voice.end() && it->time <= time_; ++it) {
            if (time_ >= it->time + it->duration) continue;
            // Isolation silences every other player's voice, radio included.
            if (only >= 0 && it->player != only) continue;
            if (muted_voices_.count(it->player) != 0) continue;
            const int id = static_cast<int>(it - demo_.voice.begin());
            const auto& player = demo_.players[static_cast<std::size_t>(it->player)];
            const bool radio = !it->proximity && transmitting_on_recorder_channel(player, it->time + 0.2f);
            if (radio && radio_muted_) {
                if (voice_active_.erase(id) != 0) audio->voice_stop(id);
                continue;
            }
            float position[3];
            if (!position_of(*it, position)) continue;
            // Someone transmitting on a channel the recorder is in is heard everywhere,
            // unless the radio is muted (then only proximity voice remains). An isolated
            // voice is played flat too, so distance never hides it.
            const bool flat = only >= 0 || radio;
            if (voice_active_.insert(id).second) {
                audio->voice_start(id, it->file, time_ - it->time, rate, position, flat);
            } else {
                audio->voice_move(id, rate, position, flat);
            }
        }
        for (auto it = voice_active_.begin(); it != voice_active_.end();) {
            const auto& clip = demo_.voice[static_cast<std::size_t>(*it)];
            if (time_ >= clip.time + clip.duration || time_ < clip.time) {
                audio->voice_stop(*it);
                it = voice_active_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::optional<std::array<float,12>> effect_target(const ActorBinding& actor, float time) const {
        float origin[3]{actor.origin[0],actor.origin[1],actor.origin[2]};
        std::array<float,3> angles{actor.angles[0],actor.angles[1],actor.angles[2]};
        if (actor.kind == 3) {
            const auto& door = demo_.doors[static_cast<std::size_t>(actor.index)];
            if (gmod::demo::door_hidden_at(door,time)) return std::nullopt;
            angles[1] += (actor.open_dir == 2 ? -1.0f : 1.0f) * actor.door_distance
                * gmod::demo::door_openness(door,time,actor.door_distance/std::max(actor.door_speed,1.0f));
            gmod::demo::door_angles_at(door,time,angles);
        } else if (actor.kind == 8 || actor.kind == 9) {
            if (actor.index < 0) return std::nullopt;
            if (gmod::demo::timed_int_at(demo_.props[static_cast<std::size_t>(actor.index)].hidden,
                time,actor.initial_hidden ? 1 : 0) != 0) return std::nullopt;
            const auto recorded = recorded_prop_matrix(demo_,static_cast<std::size_t>(actor.index),time);
            return recorded.value_or(gmod::source::model::matrix_from_source_angles(origin,angles.data()));
        } else if (actor.kind == 2) {
            gmod::demo::PropPose pose;
            if (!gmod::demo::sample_prop(demo_.props[static_cast<std::size_t>(actor.index)],time,pose)) return std::nullopt;
            origin[0]=pose.x; origin[1]=pose.y; origin[2]=pose.z;
            angles={pose.pitch,pose.yaw,pose.roll};
        } else return std::nullopt;
        return gmod::source::model::matrix_from_source_angles(origin,angles.data());
    }

    void build_muzzle_smoke() {
        if (smoke_built_) return;
        smoke_built_ = true;
        if (!animator_) {
            std::cerr << "MUZZLE_SMOKE_UNAVAILABLE: player animation skeleton is unavailable.\n";
            return;
        }
        std::vector<float> view_shots;
        if (outfit_) {
            for (std::size_t i=1;i<demo_.view_model.size();++i) {
                const auto& key = demo_.view_model[i];
                const auto& before = demo_.view_model[i-1];
                if (key.sequence == before.sequence && key.new_sequence == before.new_sequence) continue;
                const auto path = demo_.model_paths_by_index.find(key.model);
                if (path == demo_.model_paths_by_index.end()) continue;
                const auto known = outfit_->view_model_ids.find(normalize_clothing_model_path(path->second));
                if (known == outfit_->view_model_ids.end()) continue;
                const auto& asset = outfit_->view_models[static_cast<std::size_t>(known->second)];
                if (key.sequence == asset.fire || key.sequence == asset.fire_aimed)
                    view_shots.push_back(key.time);
            }
        }
        std::size_t unavailable = 0;
        for (std::size_t index=0;index<demo_.players.size();++index) {
            const auto& player = demo_.players[index];
            for (const float shot : gmod::demo::recorded_shot_times(demo_,static_cast<int>(index),view_shots)) {
                const auto held = gmod::demo::held_weapon_at(demo_,player,shot);
                const auto actor = std::find_if(actors_.begin(),actors_.end(),[&](const ActorBinding& value) {
                    return value.kind == 5 && value.index == static_cast<int>(index)
                        && value.weapon == held.class_name && value.muzzle_bone >= 0;
                });
                gmod::demo::PlayerPose pose;
                if (actor == actors_.end() || !gmod::demo::sample_player(player,shot,pose)
                    || gmod::demo::player_incapacitated(player,shot)) {
                    ++unavailable;
                    continue;
                }
                constexpr float Radians = 0.01745329251994329577f;
                const float yaw = pose.yaw*Radians, pitch = pose.pitch*Radians;
                gmod::source::model::PlayerAnimator::Input input;
                input.forward = pose.vx*std::cos(yaw)+pose.vy*std::sin(yaw);
                input.right = pose.vx*std::sin(yaw)-pose.vy*std::cos(yaw);
                input.crouching = (pose.flags & 2) != 0;
                input.grounded = (pose.flags & 1) != 0 || std::fabs(pose.vz) < 60;
                input.seated = (pose.flags & gmod::demo::PlayerFlagSeated) != 0;
                input.driver = (pose.flags & gmod::demo::PlayerFlagDriver) != 0;
                input.hold = held.hold_type;
                input.pitch = pose.pitch;
                input.fire_elapsed = 0;
                gmod::source::model::PlayerAnimator::State state;
                std::vector<gmod::source::model::Matrix3x4> palette;
                animator_->update(state,input,0,palette);
                if (static_cast<std::size_t>(actor->muzzle_bone) >= palette.size())
                    throw std::runtime_error("Muzzle smoke attachment is outside the player skeleton.");
                const auto& bone = palette[static_cast<std::size_t>(actor->muzzle_bone)];
                float local[3]{};
                for (int row=0;row<3;++row)
                    local[row] = bone[row*4]*actor->muzzle_point[0]+bone[row*4+1]*actor->muzzle_point[1]
                        +bone[row*4+2]*actor->muzzle_point[2]+bone[row*4+3];
                const float origin[3]{pose.x,pose.y,pose.z}, angles[3]{0,pose.yaw,0};
                float rendered[3];
                transform_point(prop_transform(origin,angles),local,rendered);
                gmod::demo::DemoEffect effect;
                effect.time = shot;
                effect.origin = {-rendered[0],-rendered[2],rendered[1]};
                effect.normal = {std::cos(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw),-std::sin(pitch)};
                effect.scale = 1;
                muzzle_smoke_.push_back(std::move(effect));
            }
        }
        std::sort(muzzle_smoke_.begin(),muzzle_smoke_.end(),[](const auto& a,const auto& b) {
            return a.time < b.time;
        });
        std::cout << "MUZZLE_SMOKE recorded=" << muzzle_smoke_.size() << " unavailable_poses=" << unavailable << "\n";
    }

    void update_combat_visuals(Dx12Foundation& renderer) {
        using V = std::array<float,3>;
        const auto move = [](const std::array<float,12>& m,const V& p) {
            return V{m[0]*p[0]+m[1]*p[1]+m[2]*p[2]+m[3],
                     m[4]*p[0]+m[5]*p[1]+m[6]*p[2]+m[7],
                     m[8]*p[0]+m[9]*p[1]+m[10]*p[2]+m[11]};
        };
        if (!decals_built_ && audio_assets_) {
            std::size_t unavailable = 0, blood = 0;
            for (const auto& effect : demo_.effects) {
                if (effect.kind != "decal") continue;
                const auto* material = renderer.effect_material(effect.name);
                if (!material || material->vtf.empty()) { ++unavailable; continue; }
                V point=effect.origin, start=effect.start;
                const auto* trace = &audio_assets_->decal_trace;
                int target = -1;
                if (effect.entity > 0) {
                    for (std::size_t i=0; i<actors_.size(); ++i) {
                        const auto& actor=actors_[i];
                        if (actor.index < 0) continue;
                        const int entity = actor.kind == 3 ? demo_.doors[static_cast<std::size_t>(actor.index)].entity
                            : actor.kind == 2 || actor.kind == 8 || actor.kind == 9
                                ? demo_.props[static_cast<std::size_t>(actor.index)].entity : -1;
                        if (entity != effect.entity) continue;
                        const auto matrix=effect_target(actor,effect.time);
                        const auto mesh=audio_assets_->model_traces.find(actor.model_path);
                        if (!matrix || mesh==audio_assets_->model_traces.end()) continue;
                        const auto inverse=rigid_inverse(matrix->data());
                        point=move(inverse,point); start=move(inverse,start);
                        trace=mesh->second.get();
                        target=static_cast<int>(i);
                        break;
                    }
                    if (target < 0) { ++unavailable; continue; }
                }
                V direction{point[0]-start[0],point[1]-start[1],point[2]-start[2]};
                const float length=std::sqrt(direction[0]*direction[0]+direction[1]*direction[1]+direction[2]*direction[2]);
                if (length < 0.001f) { ++unavailable; continue; }
                V ray_start{};
                for (int axis=0;axis<3;++axis) ray_start[axis]=point[axis]-direction[axis]/length*2.0f;
                gmod::source::WorldTrace::Hit hit;
                if (!trace->trace(ray_start.data(),direction.data(),4.0f,hit)) { ++unavailable; continue; }
                auto vertices=trace->project_decal(hit.point,hit.normal,
                    material->mapping_size[0]*material->decal_scale,
                    material->mapping_size[1]*material->decal_scale);
                if (vertices.empty()) { ++unavailable; continue; }
                decals_.push_back({effect.time,effect.name,target,std::move(vertices)});
                if (effect.name.find("blood") != std::string::npos) ++blood;
            }
            decals_built_=true;
            std::cout << "COMBAT_DECALS recorded=" << decals_.size() << " blood=" << blood << " unavailable_surfaces=" << unavailable
                      << " (unavailable entity meshes are not projected onto the map)\n";
        }
        std::vector<EffectVertex> vertices;
        std::vector<EffectDraw> draws;
        const auto& fires=fire_playback_.sample(demo_.fires,time_);
        if(!fires.empty()) {
            const auto listener=renderer.listener_source();
            std::vector<std::pair<float,const gmod::demo::FireEmitter*>> nearby;
            for(const auto& [entity,fire]:fires) {
                float distance=0;
                for(int i=0;i<3;++i) distance+=(fire.key.origin[i]-listener[i])*(fire.key.origin[i]-listener[i]);
                if(distance<8192.0f*8192.0f) nearby.emplace_back(distance,&fire);
            }
            std::sort(nearby.begin(),nearby.end(),[](const auto& a,const auto& b){return a.first<b.first;});
            EffectDraw draw{static_cast<UINT>(vertices.size()),0,"effects/perp2_flame",true};
            for(std::size_t i=0;i<std::min<std::size_t>(nearby.size(),128);++i) {
                const auto& fire=*nearby[i].second;
                const int latest=static_cast<int>(std::floor((time_-fire.born)/0.06f));
                for(int emission=std::max(0,latest-34);emission<=latest;++emission) for(bool blue:{false,true}) {
                    const auto puff=gmod::demo::fire_particle(fire,emission,blue,time_);
                    if(puff.opacity<=0) continue;
                    for(const int corner:{0,1,2,0,2,3}) {
                        const float u=corner==0||corner==3?0.0f:1.0f,v=corner<2?0.0f:1.0f;
                        vertices.push_back({{-puff.position[0],puff.position[2],-puff.position[1]},
                            {u,v},puff.opacity,puff.radius,{puff.color[0],puff.color[1],puff.color[2]}});
                    }
                }
            }
            draw.count=static_cast<UINT>(vertices.size())-draw.first;
            if(draw.count) draws.push_back(std::move(draw));
        }
        if (gmod::ui::settings().decals) {
            const auto first = std::upper_bound(decals_.begin(),decals_.end(),
                time_ - gmod::demo::DecalLifetime,
                [](float time,const RecordedDecal& decal) { return time < decal.time; });
            for (auto it = first; it != decals_.end(); ++it) {
                const auto& decal = *it;
                if (decal.time > time_) break;
                const float opacity = gmod::demo::decal_opacity(decal.time,time_);
                if (opacity <= 0.0f) continue;
                std::optional<std::array<float,12>> matrix;
                if (decal.target >= 0) {
                    const auto& actor = actors_[static_cast<std::size_t>(decal.target)];
                    if (actor.kind == 3 && !use_doors_) {
                        matrix = gmod::source::model::matrix_from_source_angles(actor.origin,actor.angles);
                    } else matrix=effect_target(actor,time_);
                    if (!matrix) continue;
                }
                EffectDraw draw{static_cast<UINT>(vertices.size()),0,decal.material,false};
                for (const auto& vertex : decal.vertices) {
                    const auto p=matrix ? move(*matrix,vertex.position) : vertex.position;
                    vertices.push_back({{-p[0],p[2],-p[1]},{vertex.uv[0],vertex.uv[1]},opacity});
                }
                draw.count=static_cast<UINT>(vertices.size())-draw.first;
                draws.push_back(std::move(draw));
            }
        }
        if (gmod::ui::settings().muzzle_flash) {
            for (const auto& muzzle : active_muzzles_) {
                const auto& player=demo_.players[static_cast<std::size_t>(muzzle.player)];
                float shot=-1.0f;
                const auto next=std::upper_bound(player.shots.begin(),player.shots.end(),time_);
                if (next!=player.shots.begin()) shot=*(next-1);
                const auto first=std::lower_bound(demo_.gun_sounds.begin(),demo_.gun_sounds.end(),time_-0.06f,
                    [](const auto& sound,float t){return sound.time<t;});
                for (auto sound=first;sound!=demo_.gun_sounds.end() && sound->time<=time_;++sound) {
                    if (!sound->impact && sound->player==muzzle.player) shot=std::max(shot,sound->time);
                }
                const float age=time_-shot;
                if (shot < 0 || age < 0 || age >= 0.06f) continue;
                if (g_headless && (!last_flash_log_.contains(muzzle.player) || last_flash_log_[muzzle.player] != shot)) {
                    last_flash_log_[muzzle.player]=shot;
                    std::cout << "COMBAT_FLASH player=" << muzzle.player << " shot=" << shot
                              << " time=" << time_ << " render_point=" << muzzle.point[0] << ","
                              << muzzle.point[1] << "," << muzzle.point[2] << "\n";
                }
                const float radius=5.0f;
                EffectDraw draw{static_cast<UINT>(vertices.size()),6,"effects/muzzleflash1",true};
                for (const int corner : {0,1,2,0,2,3}) {
                    const float x=corner==0 || corner==3 ? -1.0f : 1.0f;
                    const float y=corner<2 ? -1.0f : 1.0f;
                    vertices.push_back({{muzzle.point[0],muzzle.point[1],muzzle.point[2]},
                                        {(x+1)*0.5f,(y+1)*0.5f},1.0f-age/0.06f,radius});
                }
                draws.push_back(std::move(draw));
            }
        }
        if (gmod::ui::settings().decals && !g_disable_blood_mist) {
            const auto first = std::lower_bound(demo_.effects.begin(),demo_.effects.end(),
                time_ - gmod::demo::BloodMistLifetime,
                [](const auto& effect,float time) { return effect.time < time; });
            for (auto effect = first; effect != demo_.effects.end() && effect->time <= time_; ++effect) {
                if (effect->kind != "dispatch" || effect->name != "bloodimpact") continue;
                EffectDraw draw{static_cast<UINT>(vertices.size()),0,"effects/blood_core",false,true};
                for (int particle = 0; particle < 8; ++particle) {
                    const auto puff = gmod::demo::blood_mist_particle(*effect,particle,time_);
                    if (puff.opacity <= 0.0f) continue;
                    for (const int corner : {0,1,2,0,2,3}) {
                        const float u = corner == 0 || corner == 3 ? 0.0f : 1.0f;
                        const float v = corner < 2 ? 0.0f : 1.0f;
                        vertices.push_back({{-puff.position[0],puff.position[2],-puff.position[1]},
                            {u,v},puff.opacity,puff.radius});
                    }
                }
                draw.count = static_cast<UINT>(vertices.size()) - draw.first;
                if (draw.count > 0) {
                    if (g_headless) std::cout << "COMBAT_MIST event=" << effect->time << " time=" << time_
                        << " origin=" << effect->origin[0] << "," << effect->origin[1] << "," << effect->origin[2] << "\n";
                    draws.push_back(std::move(draw));
                }
            }
        }
        if (!g_disable_smoke) {
            if (gmod::ui::settings().muzzle_flash) build_muzzle_smoke();
            struct Puff {
                gmod::demo::SmokeParticle particle;
                float distance;
            };
            std::vector<Puff> smoke;
            const auto listener = renderer.listener_source();
            const auto append_smoke = [&](const gmod::demo::DemoEffect& effect,bool muzzle) {
                bool emitted = false;
                for (int particle=0;particle<4;++particle) {
                    const auto puff = gmod::demo::smoke_particle(effect,particle,time_,muzzle);
                    if (puff.opacity <= 0) continue;
                    float distance = 0;
                    for (int i=0;i<3;++i) distance += (puff.position[i]-listener[i])*(puff.position[i]-listener[i]);
                    smoke.push_back({puff,distance});
                    emitted = true;
                }
                if (emitted) {
                    if (g_headless) std::cout << "COMBAT_SMOKE kind=" << (muzzle ? "muzzle" : "impact")
                        << " event=" << effect.time << " time=" << time_ << " material=" << effect.material
                        << " origin=" << effect.origin[0] << "," << effect.origin[1] << "," << effect.origin[2] << "\n";
                }
            };
            if (gmod::ui::settings().muzzle_flash) {
                const auto first = std::lower_bound(muzzle_smoke_.begin(),muzzle_smoke_.end(),
                    time_-gmod::demo::SmokeLifetime,[](const auto& effect,float time) { return effect.time < time; });
                for (auto it=first;it!=muzzle_smoke_.end() && it->time<=time_;++it) append_smoke(*it,true);
            }
            if (gmod::ui::settings().decals) {
                const auto first = std::lower_bound(demo_.effects.begin(),demo_.effects.end(),
                    time_-gmod::demo::SmokeLifetime,[](const auto& effect,float time) { return effect.time < time; });
                for (auto it=first;it!=demo_.effects.end() && it->time<=time_;++it)
                    if (gmod::demo::surface_smoke_effect(*it)) append_smoke(*it,false);
            }
            std::stable_sort(smoke.begin(),smoke.end(),[](const Puff& a,const Puff& b) {
                return a.distance > b.distance;
            });
            EffectDraw draw{static_cast<UINT>(vertices.size()),0,"particle/particle_smokegrenade",false,false,true};
            for (const auto& item : smoke) {
                const auto& puff = item.particle;
                for (const int corner : {0,1,2,0,2,3}) {
                    const float u = corner == 0 || corner == 3 ? 0.0f : 1.0f;
                    const float v = corner < 2 ? 0.0f : 1.0f;
                    vertices.push_back({{-puff.position[0],puff.position[2],-puff.position[1]},
                        {u,v},puff.opacity,puff.radius,{puff.color[0],puff.color[1],puff.color[2]}});
                }
            }
            draw.count = static_cast<UINT>(vertices.size())-draw.first;
            if (draw.count) draws.push_back(std::move(draw));
        }
        renderer.set_combat_effects(std::move(vertices),std::move(draws));
    }

    // Gunfire at every networked shot and footsteps for players on the move. Both follow the
    // demo clock and only sound while it plays forwards at a pace the sounds can keep up with.
    void update_effects(Dx12Foundation& renderer) {
        auto audio = sound_sink(renderer);
        const float rate = kSpeeds[speed_index_];
        const bool restoring = effect_sound_seek_ || time_ < last_effect_time_ || time_ - last_effect_time_ > 1.0f;
        const float previous = restoring ? time_ - OneShotLookback : last_effect_time_;
        last_effect_time_ = time_;
        if (!audio || !audio_assets_) return;
        const bool audible = playing_ && !reverse_ && rate <= 4.0f;
        // A seek, rewind or pause restarts the step timers instead of replaying the gap.
        if (!audible) {
            std::fill(next_step_.begin(), next_step_.end(), -1.0f);
            return;
        }
        effect_sound_seek_ = false;
        if (restoring) std::fill(next_step_.begin(), next_step_.end(), -1.0f);
        const auto listener = renderer.listener_source();
        const auto pick = [&](const std::vector<std::string>& files) -> const std::string& {
            return files[effect_rng_() % files.size()];
        };
        const auto jitter = [&] {
            return rate * (0.95f + 0.1f * static_cast<float>(effect_rng_() % 1000) / 1000.0f);
        };
        const auto within = [&](float x, float y, float z, float radius) {
            const float dx = x - listener[0], dy = y - listener[1], dz = z - listener[2];
            return dx * dx + dy * dy + dz * dz < radius * radius;
        };

        constexpr float GunRadius = 6000.0f;
        constexpr float StepRadius = 1400.0f;
        const auto weapon_shot = [&](const gmod::demo::HeldWeapon& held, float time, const float* position) {
            // Melee attacks use their recorded/reconstructed sounds, never firearm fallbacks.
            const auto* info=swep_info_for(held.class_name);
            if(held.class_name=="roleplay_fists" || (info && !info->gun)) return;
            audio.elapsed = restoring ? std::max(0.0f, time_ - time) : 0.0f;
            const auto override = outfit_
                ? gmod::demo::weapon_sound_override_at(outfit_->registry,demo_,held.handle,time) : std::string();
            if (!override.empty()) {
                const auto entry = std::find_if(outfit_->registry.gun_sounds.begin(),outfit_->registry.gun_sounds.end(),
                    [&](const auto& item) { return _stricmp(item.second.name.c_str(),override.c_str()) == 0; });
                if (entry == outfit_->registry.gun_sounds.end() || entry->second.files.empty()) {
                    std::cerr << "WEAPON_SOUND_OVERRIDE_UNRESOLVED time=" << time
                        << " weapon=" << held.class_name << " sound=" << override << "\n";
                    return;
                }
                if (within(position[0],position[1],position[2],outfit_->registry.gun_sound_distance)) {
                    const auto& file = pick(entry->second.files);
                    audio.level_sound(file,position,entry->second.level,entry->second.volume,jitter());
                    std::cout << "WEAPON_SHOT time=" << time << " weapon=" << held.class_name
                        << " override=" << override << " file=" << file << "\n";
                }
                return;
            }
            const auto& files = fire_sounds_for(held.class_name);
            if (files.empty()) {
                std::cerr << "WEAPON_SHOT_UNRESOLVED time=" << time << " weapon=" << held.class_name << "\n";
                return;
            }
            audio.one_shot(pick(files),position,1.5f,GunRadius,jitter());
        };
        // Demos with Perpheads' own gun-sound messages: each one plays the server's sound at
        // the server's position, heard as cl_gun_sounds.lua hears it.
        const bool server_gun_sounds = outfit_ && std::any_of(demo_.gun_sounds.begin(), demo_.gun_sounds.end(),
            [](const auto& sound) { return !sound.impact; });
        if (outfit_ && !demo_.gun_sounds.empty()) {
            const auto first = std::upper_bound(demo_.gun_sounds.begin(), demo_.gun_sounds.end(), previous,
                [](float value, const gmod::demo::DemoData::GunSound& sound) { return value < sound.time; });
            for (auto sound = first; sound != demo_.gun_sounds.end() && sound->time <= time_; ++sound) {
                const auto entry = outfit_->registry.gun_sounds.find(sound->index);
                if (entry == outfit_->registry.gun_sounds.end()) continue;
                const float range = sound->impact ? outfit_->registry.impact_sound_distance
                                                  : outfit_->registry.gun_sound_distance;
                if (!within(sound->x, sound->y, sound->z, range)) continue;
                const float position[3]{sound->x, sound->y, sound->z};
                audio.elapsed = restoring ? std::max(0.0f, time_ - sound->time) : 0.0f;
                audio.level_sound(pick(entry->second.files), position, entry->second.level,
                                        entry->second.volume, rate * (0.9f + 0.2f * static_cast<float>(effect_rng_() % 1000) / 1000.0f));
            }
        }
        for (std::size_t index = 0; index < demo_.players.size(); ++index) {
            // The recorder hears locally predicted fire, which need not have a server sound
            // message. Its muzzle counter still records those shots in older demos.
            const bool recorder = static_cast<int>(index) == demo_.recorder_player;
            if (server_gun_sounds && !recorder) continue;
            const auto& player = demo_.players[index];
            const auto first = std::upper_bound(player.shots.begin(), player.shots.end(), previous);
            for (auto shot = first; shot != player.shots.end() && *shot <= time_; ++shot) {
                if (server_gun_sounds) {
                    const auto nearby = std::lower_bound(demo_.gun_sounds.begin(), demo_.gun_sounds.end(), *shot - 0.05f,
                        [](const auto& sound, float value) { return sound.time < value; });
                    const auto after = std::upper_bound(nearby, demo_.gun_sounds.end(), *shot + 0.05f,
                        [](float value, const auto& sound) { return value < sound.time; });
                    const bool covered = std::any_of(nearby, after, [&](const auto& sound) {
                        return !sound.impact && sound.player == static_cast<int>(index);
                    });
                    if (covered) continue;
                }
                gmod::demo::PlayerPose pose;
                if (!gmod::demo::sample_player(player, *shot, pose)) continue;
                if (!within(pose.x, pose.y, pose.z + 56.0f, GunRadius)) continue;
                const auto held = gmod::demo::held_weapon_at(demo_, player, *shot);
                const float position[3]{pose.x, pose.y, pose.z + 56.0f};
                weapon_shot(held,*shot,position);
            }
        }

        // The recording player's own shots: some weapons (the sniper rifles) never flip the
        // muzzle-flash parity the shots above are read from, but the view model always starts
        // its fire sequence.
        if (demo_.recorder_player >= 0 && !demo_.view_model.empty() && outfit_) {
            const auto& recorder = demo_.players[static_cast<std::size_t>(demo_.recorder_player)];
            const auto first_key = std::upper_bound(
                demo_.view_model.begin(), demo_.view_model.end(), previous,
                [](float value, const gmod::demo::ViewModelKey& key) { return value < key.time; });
            for (auto key = first_key; key != demo_.view_model.end() && key->time <= time_; ++key) {
                if (key == demo_.view_model.begin() || key->model <= 0) continue;
                const auto& before = *(key - 1);
                if (!gmod::demo::view_model_restarted(before,*key)) continue;
                const auto path = demo_.model_paths_by_index.find(key->model);
                if (path == demo_.model_paths_by_index.end()) continue;
                const auto known = outfit_->view_model_ids.find(normalize_clothing_model_path(path->second));
                if (known == outfit_->view_model_ids.end()) continue;
                const auto& asset = outfit_->view_models[static_cast<std::size_t>(known->second)];
                if (key->sequence != asset.fire && key->sequence != asset.fire_aimed) continue;
                const bool counted = std::any_of(recorder.shots.begin(), recorder.shots.end(),
                    [&](float shot) { return std::fabs(shot - key->time) < 0.3f; });
                if (counted) continue;
                gmod::demo::PlayerPose pose;
                if (!gmod::demo::sample_player(recorder, key->time, pose)) continue;
                const auto held = gmod::demo::held_weapon_at(demo_, recorder, key->time);
                const float position[3]{pose.x, pose.y, pose.z + 56.0f};
                weapon_shot(held,key->time,position);
            }
        }

        // Footsteps: Source steps every 0.4 s walking and 0.3 s running; the server mutes
        // them while crouching. Skipped at fast playback, where they would only be noise.
        audio.elapsed = 0.0f;
        if (rate > 2.0f) return;
        for (std::size_t index = 0; index < demo_.players.size(); ++index) {
            gmod::demo::PlayerPose pose;
            const auto& player = demo_.players[index];
            const bool stepping = !ragdolled_[index]
                && gmod::demo::sample_player(player, time_, pose)
                && (pose.flags & gmod::demo::PlayerFlagSeated) == 0
                && (pose.flags & 2) == 0
                && ((pose.flags & 1) != 0 || std::fabs(pose.vz) < 60.0f)
                && std::hypot(pose.vx, pose.vy) >= 70.0f;
            if (!stepping) {
                next_step_[index] = -1.0f;
                continue;
            }
            if (next_step_[index] < 0.0f) next_step_[index] = time_ + 0.05f;
            if (time_ < next_step_[index]) continue;
            const float speed = std::hypot(pose.vx, pose.vy);
            const bool running = speed >= 190.0f;
            next_step_[index] = time_ + (running ? 0.3f : 0.4f);
            if (!within(pose.x, pose.y, pose.z, StepRadius)) continue;
            const auto& surface = audio_assets_->ground.surface_at(pose.x, pose.y, pose.z);
            const auto& files = step_sounds_for(surface);
            if (files.empty()) continue;
            const float position[3]{pose.x, pose.y, pose.z + 8.0f};
            audio.one_shot(pick(files), position, running ? 0.9f : 0.55f, StepRadius, jitter());
        }
    }

    // ---- Sounds the server itself played (svc_Sounds): doors, locks, horns, sirens, engines ----

    void reconstruct_melee() {
        if(!outfit_) return;
        std::vector<gmod::demo::TimedInt> recorder_attacks;
        for(std::size_t i=1;i<demo_.view_model.size();++i) {
            const auto& key=demo_.view_model[i];
            if(!gmod::demo::view_model_restarted(demo_.view_model[i-1],key)) continue;
            const auto path=demo_.model_paths_by_index.find(key.model);
            if(path==demo_.model_paths_by_index.end()) continue;
            const auto model=normalize_clothing_model_path(path->second);
            const auto asset=outfit_->view_model_ids.find(model);
            if(asset==outfit_->view_model_ids.end()) continue;
            const auto info=outfit_->registry.swep_info.find(model);
            const bool fists=model.find("c_fisticuffs")!=std::string::npos;
            if(!fists && (info==outfit_->registry.swep_info.end() || !info->second.melee)) continue;
            const auto& animation=*outfit_->view_models[asset->second].animation;
            if(animation.sequence_has_activity(key.sequence,"ACT_VM_PRIMARYATTACK")) recorder_attacks.push_back({key.time,0});
            else if(animation.sequence_has_activity(key.sequence,"ACT_VM_SECONDARYATTACK")) recorder_attacks.push_back({key.time,1});
        }
        if(demo_.recorder_player>=0) {
            gmod::demo::merge_melee_attacks(demo_.players[demo_.recorder_player],recorder_attacks);
            std::cout << "RECORDER_MELEE_ATTACKS count=" << recorder_attacks.size() << "\n";
        }
        std::vector<gmod::demo::DemoSound> reconstructed;
        for(const auto& player:demo_.players) for(const auto& attack:player.attacks) {
            const auto held=gmod::demo::held_weapon_at(demo_,player,attack.time);
            const auto* info=swep_info_for(held.class_name);
            if(!info || !info->melee || info->melee_swing.files.empty()) continue;
            // Remote gesture events occur at the hit/swing time; recorder view-model events
            // occur at the beginning of a possibly delayed attack.
            const bool inferred=demo_.recorder_player>=0 && &player==&demo_.players[demo_.recorder_player]
                && std::any_of(recorder_attacks.begin(),recorder_attacks.end(),[&](const auto& key){return key.time==attack.time && key.value==attack.value;});
            const float time=attack.time+(inferred?(attack.value==1?info->melee_secondary_delay:info->melee_primary_delay):0);
            if(gmod::demo::has_recorded_melee_sound(demo_,player.entity,held.entity,time,info->melee_swing.files)) continue;
            gmod::demo::PlayerPose pose;if(!gmod::demo::sample_player(player,time,pose)) continue;
            gmod::demo::DemoSound sound; sound.time=time;sound.entity=player.entity;sound.channel=1;sound.index=-1;
            sound.name=info->melee_swing.files[static_cast<std::size_t>(std::lround(time*demo_.tick_rate))%info->melee_swing.files.size()];
            sound.volume=info->melee_swing.volume;sound.level=static_cast<int>(info->melee_swing.level);
            sound.x=pose.x;sound.y=pose.y;sound.z=pose.z+56;
            reconstructed.push_back(sound);
            if(g_headless) std::cout << "MELEE_SOUND_RECONSTRUCTED time=" << sound.time << " player=" << player.name
                << " file=" << sound.name << "\n";
        }
        demo_.sounds.insert(demo_.sounds.end(),reconstructed.begin(),reconstructed.end());
        std::stable_sort(demo_.sounds.begin(),demo_.sounds.end(),[](const auto& a,const auto& b){return a.time<b.time;});
        std::cout << "MELEE_SOUNDS_RECONSTRUCTED count=" << reconstructed.size() << "\n";
    }

    void reconstruct_doll_sounds() {
        if (!outfit_ || demo_.recorder_player < 0) return;
        const auto& recorder = demo_.players[static_cast<std::size_t>(demo_.recorder_player)];
        std::vector<gmod::demo::DemoSound> reconstructed;
        for (std::size_t index = 1; index < demo_.view_model.size(); ++index) {
            const auto& key = demo_.view_model[index];
            const auto& before = demo_.view_model[index - 1];
            if (!gmod::demo::view_model_restarted(before,key)) continue;
            const auto path = demo_.model_paths_by_index.find(key.model);
            if (path == demo_.model_paths_by_index.end()) continue;
            const auto model = normalize_clothing_model_path(path->second);
            const auto asset_id = outfit_->view_model_ids.find(model);
            const auto info = outfit_->registry.swep_info.find(model);
            if (asset_id == outfit_->view_model_ids.end() || info == outfit_->registry.swep_info.end()) continue;
            const auto& asset = outfit_->view_models[static_cast<std::size_t>(asset_id->second)];
            const auto* sound = asset.animation->sequence_has_activity(key.sequence, "ACT_VM_PRIMARYATTACK")
                ? &info->second.doll_shake
                : asset.animation->sequence_has_activity(key.sequence, "ACT_VM_FIDGET")
                    ? &info->second.doll_fidget : nullptr;
            if (!sound || sound->files.empty()) continue;
            const bool recorded = std::any_of(demo_.sounds.begin(), demo_.sounds.end(), [&](const auto& event) {
                return std::fabs(event.time - key.time) < 0.12f
                    && (event.entity == recorder.entity || event.entity == (key.weapon & 8191))
                    && std::find(sound->files.begin(), sound->files.end(), event.name) != sound->files.end();
            });
            if (recorded) continue;
            gmod::demo::PlayerPose pose;
            if (!gmod::demo::sample_player(recorder, key.time, pose)) continue;
            const auto tick = static_cast<std::size_t>(std::lround(key.time * demo_.tick_rate));
            gmod::demo::DemoSound event;
            event.time = key.time;
            event.entity = recorder.entity;
            event.channel = 3;
            event.index = -1;
            event.name = sound->files[tick % sound->files.size()];
            event.volume = sound->volume;
            event.level = static_cast<int>(sound->level);
            event.x = pose.x;
            event.y = pose.y;
            event.z = pose.z + 56.0f;
            std::clog << "DOLL_SOUND_RECONSTRUCTED time=" << event.time << " file=" << event.name << "\n";
            reconstructed.push_back(std::move(event));
        }
        demo_.sounds.insert(demo_.sounds.end(), reconstructed.begin(), reconstructed.end());
        std::stable_sort(demo_.sounds.begin(), demo_.sounds.end(),
            [](const auto& left, const auto& right) { return left.time < right.time; });
    }

    static std::uint64_t sound_key(const gmod::demo::DemoSound& sound) {
        return (static_cast<std::uint64_t>(sound.entity) << 20) | (static_cast<std::uint64_t>(sound.channel) << 17)
            | static_cast<std::uint64_t>(sound.index & 0x1FFFF);
    }

    // Mechanical stop commands can arrive long after a finite sample finished.
    void classify_world_sounds() {
        std::unordered_map<std::uint64_t, std::size_t> started;
        for (std::size_t index = 0; index < demo_.sounds.size(); ++index) {
            auto& sound = demo_.sounds[index];
            const auto key = sound_key(sound);
            // Engine and tyre sounds are loops whether or not the updates that prove it
            // survived decoding; played as one-shots they would restart over and over.
            if (sound.name.find("vehicles/") != std::string::npos
                && (sound.flags & 4) == 0) {
                std::string lower = sound.name;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                for (const char* part : {"idle", "first", "second", "third", "fourth", "fifth", "cruise", "rev.",
                                         "throttle", "full", "midthrottle", "tires/", "skid"}) {
                    if (lower.find(part) != std::string::npos) sound.loop = true;
                }
            }
            if ((sound.flags & 4) == 0 && sound.name.find("physics/") != std::string::npos
                && sound.name.find("loop") != std::string::npos) {
                sound.loop = true;
            }
            if ((sound.flags & 4) != 0) {
                const auto found = started.find(key);
                if (found != started.end()) demo_.sounds[found->second].loop = true;
                started.erase(key);
            } else if ((sound.flags & 3) != 0) {
                // A change with no start seen: the sound began before the demo did.
                sound.loop = true;
                const auto found = started.find(key);
                if (found != started.end()) demo_.sounds[found->second].loop = true;
            } else {
                started[key] = index;
            }
        }
        // A stop can interrupt a one-shot too. Doors, platforms and crosswalk buttons without
        // WAV loop cues must finish naturally, not repeat until their next stop.
        for (auto& sound : demo_.sounds) {
            const auto file = world_sound_file(sound.name);
            if ((sound.flags & 4) == 0 && gmod::audio::uses_wave_loop_metadata(file)
                && audio_assets_)
                sound.loop = audio_assets_->catalog.wave_loops(file);
        }
    }

    // The map's own ambient emitters are played by the audio system already.
    static bool world_sound_skipped(const std::string& name) {
        return name.rfind("ambience/", 0) == 0 || name.rfind("ambient/", 0) == 0
            || name.rfind("common/", 0) == 0 || name.find('.') == std::string::npos;
    }

    static std::string world_sound_file(std::string name) {
        // Leading characters are spatialisation hints, not part of the file name.
        std::replace(name.begin(), name.end(), '\\', '/');
        const auto first = name.find_first_not_of("*#@<>^()}$!?");
        return first == std::string::npos ? std::string() : name.substr(first);
    }

    bool entity_position(int entity, float out[3]) const {
        const int player = gmod::demo::player_index_at(demo_, entity, time_);
        if (player >= 0) {
            gmod::demo::PlayerPose pose;
            if (gmod::demo::sample_player(demo_.players[static_cast<std::size_t>(player)], time_, pose)) {
                out[0] = pose.x;
                out[1] = pose.y;
                out[2] = pose.z + 40.0f;
                return true;
            }
        }
        const auto props = prop_by_entity_.find(entity);
        if (props != prop_by_entity_.end()) {
            for (const int index : props->second) {
                gmod::demo::PropPose pose;
                if (gmod::demo::sample_prop(demo_.props[static_cast<std::size_t>(index)], time_, pose)) {
                    out[0] = pose.x;
                    out[1] = pose.y;
                    out[2] = pose.z + 30.0f;
                    return true;
                }
            }
        }
        return false;
    }

    // Distance at which Source's falloff (1 / (distance x dist_mult)) reaches a tenth, capped:
    // some servers' sounds carry absurd levels that would otherwise be heard across the map.
    static float sound_radius(int level, float cap) {
        return std::clamp(360.0f * std::pow(10.0f, static_cast<float>(level) / 20.0f - 3.0f), 300.0f, cap);
    }

    struct WorldLoop {
        int id = 0;
        int entity = 0;
        int channel = 0;
        std::string file;
        float gain = 1.0f;
        float radius = 1000.0f;
        float rate = 1.0f;
        float origin[3]{};
        bool playing = false;  // currently handed to the audio system (near enough to hear)
        bool vehicle = false;  // an engine or tyre sound: only meaningful while its car is around
        bool contact = false;  // a physics scrape or roll: sounds only while something is moving
        float last_event = 0.0f;  // demo time of the server's latest word on it
    };

    // Sound output for the world sounds: the audio system, or (in screenshot runs, which have
    // none) a log of what would have played.
    struct SoundSink {
        gmod::audio::AudioSystem* audio = nullptr;
        std::ofstream* log = nullptr;
        float time = 0.0f;
        float elapsed = 0.0f;
        float demo_rate = 1.0f;
        const gmod::audio::StoppedSoundEvents* stopped = nullptr;
        explicit operator bool() const { return audio || log; }
        void loop_set(int id, const std::string& file, const float* at, float gain, float radius, float rate) const {
            if (audio) audio->loop_set(id, file, at, gain, radius, rate);
            else if (log) *log << time << " loop " << id << " " << file << " gain=" << gain << " radius=" << radius << " rate=" << rate << " at=" << at[0] << "," << at[1] << "," << at[2] << "\n";
        }
        void loop_stop(int id) const {
            if (audio) audio->loop_stop(id);
            else if (log) *log << time << " stop " << id << "\n";
        }
        void loop_stop_all() const {
            if (audio) audio->loop_stop_all();
            else if (log) *log << time << " stop-all\n";
        }
        void world_stop_all() const {
            if (audio) audio->world_stop_all();
            else if (log) *log << time << " world-stop-all\n";
        }
        void world_set_paused(bool paused) const {
            if (audio) audio->world_set_paused(paused);
            else if (log) *log << time << (paused ? " world-pause\n" : " world-resume\n");
        }
        void one_shot(const std::string& file, const float* at, float gain, float radius, float rate) const {
            if (stopped && !stopped->allows(time - elapsed)) return;
            const float offset = elapsed * std::clamp(rate, 0.25f, 4.0f) / demo_rate;
            if (offset >= gmod::audio::AudioSystem::MaxOneShotSourceSeconds) return;
            if (audio) audio->play_sound(file, at, gain, radius, rate, offset);
            else if (log) *log << time << " once " << file << " gain=" << gain << " radius=" << radius << " rate=" << rate << " offset=" << offset << " at=" << at[0] << "," << at[1] << "," << at[2] << "\n";
        }
        void level_sound(const std::string& file, const float* at, float level, float volume, float rate) const {
            if (stopped && !stopped->allows(time - elapsed)) return;
            const float offset = elapsed * std::clamp(rate, 0.25f, 4.0f) / demo_rate;
            if (offset >= gmod::audio::AudioSystem::MaxOneShotSourceSeconds) return;
            if (audio) audio->play_level_sound(file,at,level,volume,rate,offset);
            else if (log) *log << time << " level " << file << " level=" << level << " volume=" << volume
                << " rate=" << rate << " offset=" << offset << " at=" << at[0] << "," << at[1] << "," << at[2] << "\n";
        }
    };

    SoundSink sound_sink(Dx12Foundation& renderer) {
        stopped_sound_events_.observe(time_);
        SoundSink sink;
        sink.stopped = &stopped_sound_events_;
        sink.audio = renderer.audio();
        if (!sink.audio && g_headless && !world_sound_log_path_.empty()) {
            if (!world_sound_log_.is_open()) world_sound_log_.open(world_sound_log_path_, std::ios::trunc);
            sink.log = &world_sound_log_;
        }
        sink.time = time_;
        sink.demo_rate = kSpeeds[speed_index_];
        return sink;
    }

    void apply_world_sound(Dx12Foundation& renderer, const gmod::demo::DemoSound& sound, bool one_shots,
                           bool restoring = false) {
        auto audio = sound_sink(renderer);
        if (restoring) audio.elapsed = std::max(0.0f, time_ - sound.time);
        if (!audio) return;
        const auto key = sound_key(sound);
        if (!stopped_sound_events_.allows_world_event(sound.time, sound.flags, active_loops_.contains(key))) return;
        if ((sound.flags & 4) != 0) {
            const auto found = active_loops_.find(key);
            if (found != active_loops_.end()) {
                if (found->second.playing) audio.loop_stop(found->second.id);
                active_loops_.erase(found);
            }
            return;
        }
        if ((sound.flags & 3) == 0 && sound.channel != 0) {
            for (auto it = active_loops_.begin(); it != active_loops_.end();) {
                if (it->second.entity == sound.entity && it->second.channel == sound.channel) {
                    if (it->second.playing) audio.loop_stop(it->second.id);
                    it = active_loops_.erase(it);
                } else ++it;
            }
        }
        if (world_sound_skipped(sound.name)) return;
        const std::string file = world_sound_file(sound.name);
        if (file.empty()) return;
        float origin[3]{sound.x, sound.y, sound.z};
        entity_position(sound.entity, origin);
        const float rate = kSpeeds[speed_index_];
        if (sound.loop) {
            auto& loop = active_loops_[key];
            if (loop.id == 0) loop.id = next_loop_id_++;
            loop.entity = sound.entity;
            loop.channel = sound.channel;
            loop.file = file;
            loop.vehicle = sound.name.find("vehicles/") != std::string::npos
                || sound.name.find("Vehicles/") != std::string::npos;
            loop.last_event = sound.time;
            // A ragdoll sliding or rolling keeps its scrape going only while the server keeps
            // updating it; the stop message is often one that cannot be read.
            loop.contact = sound.name.find("physics/") != std::string::npos
                || sound.name.find("scrape") != std::string::npos
                || sound.name.find("_roll") != std::string::npos;
            loop.gain = sound.volume * 0.4f;
            loop.radius = sound_radius(sound.level, loop.vehicle ? 900.0f : 1500.0f);
            loop.rate = static_cast<float>(sound.pitch) / 100.0f;
            std::copy(origin, origin + 3, loop.origin);
        } else if (one_shots && (sound.flags & 3) == 0) {
            const auto listener = camera_position_;
            const float dx = origin[0] - listener[0], dy = origin[1] - listener[1], dz = origin[2] - listener[2];
            bool ram = false;
            if (file == "physics/wood/wood_panel_break1.wav"
                || file == "physics/wood/wood_panel_impact_hard1.wav") {
                const int player = gmod::demo::player_index_at(demo_, sound.entity, sound.time);
                ram = player >= 0
                    && gmod::demo::held_weapon_at(demo_, demo_.players[static_cast<std::size_t>(player)], sound.time).class_name
                        == "weapon_perp_battering_ram";
            }
            const float radius = ram ? std::max(1200.0f, sound_radius(sound.level, 2500.0f))
                                     : sound_radius(sound.level, 2500.0f);
            if (dx * dx + dy * dy + dz * dz > radius * radius) return;
            const bool walled = audio_assets_ && audio_assets_->trace.blocked(camera_position_.data(), origin);
            audio.one_shot(file, origin, sound.volume * (walled ? 0.3f : 0.8f), walled && !ram ? radius * 0.6f : radius,
                           static_cast<float>(sound.pitch) / 100.0f * rate);
        }
    }

    void update_world_sounds(Dx12Foundation& renderer) {
        const auto audio = sound_sink(renderer);
        if (!audio) return;
        const float rate = kSpeeds[speed_index_];
        const bool supported = !reverse_ && rate <= 4.0f;
        const float previous = world_sound_time_;
        const bool jumped = world_sound_seek_ || time_ < previous - 0.001f || time_ - previous > 1.0f;
        world_sound_seek_ = false;
        world_sound_time_ = time_;
        if (!supported || jumped) {
            if (world_sound_ready_ || !active_loops_.empty()) {
                audio.world_stop_all();
                active_loops_.clear();
            }
            world_sound_ready_ = false;
        }
        const bool paused = !playing_ || !supported;
        if (world_sound_paused_ != paused) {
            audio.world_set_paused(paused);
            world_sound_paused_ = paused;
        }
        if (paused) return;
        const auto after = [](float value, const gmod::demo::DemoSound& sound) { return value < sound.time; };
        if (jumped || !world_sound_ready_) {
            // The decoder retains at most 120 source seconds. At the minimum supported
            // source/demo rate (0.25 / 4), no earlier one-shot can still be active.
            world_sound_ready_ = true;
            auto begin = std::upper_bound(demo_.sounds.begin(), demo_.sounds.end(), time_ - OneShotLookback, after);
            const auto end = std::upper_bound(demo_.sounds.begin(), demo_.sounds.end(), time_, after);
            std::unordered_map<std::uint64_t, const gmod::demo::DemoSound*> channel_starts, stops;
            for (auto it = begin; it != end; ++it) {
                if ((it->flags & 4) != 0) stops[sound_key(*it)] = &*it;
                else if (it->channel != 0 && (it->flags & 3) == 0)
                    channel_starts[sound_key(*it) >> 17] = &*it;
            }
            for (auto it = begin; it != end; ++it) {
                if (it->loop && it->time < time_ - 90.0f) continue;
                if (!it->loop) {
                    const auto stopped = stops.find(sound_key(*it));
                    if (stopped != stops.end() && stopped->second > &*it) continue;
                    const auto replaced = channel_starts.find(sound_key(*it) >> 17);
                    if (it->channel != 0 && replaced != channel_starts.end() && replaced->second > &*it) continue;
                }
                apply_world_sound(renderer, *it, true, true);
            }
            return;
        }
        auto it = std::upper_bound(demo_.sounds.begin(), demo_.sounds.end(), previous, after);
        for (; it != demo_.sounds.end() && it->time <= time_; ++it) apply_world_sound(renderer, *it, true);

        // Loops follow the entity that carries them.
        const DWORD now = GetTickCount();
        if (now - last_loop_move_ >= 100) {
            last_loop_move_ = now;
            // Only the nearest loops within earshot play, so distant traffic neither costs voices
            // nor drowns out gunfire and voices.
            constexpr std::size_t MaxLoops = 14;
            std::vector<std::pair<float, WorldLoop*>> candidates;
            for (auto it = active_loops_.begin(); it != active_loops_.end();) {
                auto& loop = it->second;
                // Some of the server's updates cannot be read, so a car sound nobody has
                // mentioned for a while is taken as stopped rather than left droning on.
                const bool stale = (loop.vehicle && time_ - loop.last_event > 6.0f)
                    || (loop.contact && time_ - loop.last_event > 1.5f);
                const bool car_gone = loop.vehicle && loop.entity > 0 && !entity_position(loop.entity, loop.origin);
                if (stale || car_gone) {
                    if (loop.playing) audio.loop_stop(loop.id);
                    it = active_loops_.erase(it);
                    continue;
                }
                ++it;
                if (loop.entity > 0 && !loop.vehicle) entity_position(loop.entity, loop.origin);
                const float dx = loop.origin[0] - camera_position_[0];
                const float dy = loop.origin[1] - camera_position_[1];
                const float dz = loop.origin[2] - camera_position_[2];
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance < loop.radius) candidates.push_back({distance, &loop});
                else if (loop.playing) {
                    audio.loop_stop(loop.id);
                    loop.playing = false;
                }
            }
            std::sort(candidates.begin(), candidates.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
            for (std::size_t index = 0; index < candidates.size(); ++index) {
                auto& loop = *candidates[index].second;
                if (index < MaxLoops) {
                    // Walls muffle an engine: behind world geometry it is quieter and carries less far.
                    const bool walled = audio_assets_ && audio_assets_->trace.blocked(camera_position_.data(), loop.origin);
                    audio.loop_set(loop.id, loop.file, loop.origin, walled ? loop.gain * 0.25f : loop.gain,
                                   walled ? loop.radius * 0.5f : loop.radius, loop.rate * rate);
                    loop.playing = true;
                } else if (loop.playing) {
                    audio.loop_stop(loop.id);
                    loop.playing = false;
                }
            }
        }
    }

    // Fire samples of a weapon class: its first-person model -> SWEP script -> sound name.
    const std::vector<std::string>& fire_sounds_for(const std::string& weapon_class) {
        auto found = fire_sound_cache_.find(weapon_class);
        if (found != fire_sound_cache_.end()) return found->second;
        std::vector<std::string> files;
        const auto view = demo_.weapon_view_models.find(weapon_class);
        if (view != demo_.weapon_view_models.end() && outfit_) {
            files = resolve_fire_sounds(outfit_->registry, audio_assets_->catalog,
                                        normalize_clothing_model_path(view->second));
        }
        return fire_sound_cache_.emplace(weapon_class, std::move(files)).first->second;
    }

    const std::vector<std::string>& step_sounds_for(const std::string& surface) {
        auto found = step_sound_cache_.find(surface);
        if (found != step_sound_cache_.end()) return found->second;
        auto files = audio_assets_->catalog.footsteps(audio_assets_->surfaces.step_set(surface));
        return step_sound_cache_.emplace(surface, std::move(files)).first->second;
    }

    void play_reverse() {
        reverse_ = true;
        if (time_ <= 0.0f) seek(demo_.duration);
        playing_ = true;
    }

    static std::wstring to_wide(const std::string& text) {
        const int size = MultiByteToWideChar(
            CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(std::max(size, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            wide.data(), size);
        return wide;
    }

    // The parser garbles some names; the demo header holds the recorder's real one.
    std::string display_name(std::size_t index) const {
        if (static_cast<int>(index) == demo_.recorder_player && !demo_.client_name.empty()) {
            return demo_.client_name;
        }
        return demo_.players[index].name;
    }

    std::wstring follow_name() const {
        if (follow_ < 0) return {};
        return to_wide(display_name(static_cast<std::size_t>(follow_)));
    }

public:
    // Name tags for the players in view: the roleplay name and, for anyone on a job, the
    // job's short title, in the job's colour (all from the demo's networked data).
    std::vector<gmod::ui::NameTag> build_view_tags(
        const Dx12Foundation& renderer, int width = 0, int height = 0) const {
        if (width <= 0 || height <= 0) {
            RECT client{};
            GetClientRect(window_, &client);
            width = client.right;
            height = client.bottom;
        }
        auto tags = build_tags(renderer.view_projection(), width, height);
        const auto lens = renderer.scope_overlay();
        if (lens.triangles.empty() || width <= 0 || height <= 0) return tags;
        for (auto tag : build_tags(renderer.scope_view_projection(), width, height)) {
            const auto point = lens.screen(tag.x/width, tag.y/height, width, height);
            if (!point) continue;
            tag.x = (*point)[0];
            tag.y = (*point)[1];
            tag.scoped = true;
            tags.push_back(std::move(tag));
        }
        return tags;
    }

    std::vector<gmod::ui::NameTag> build_tags(const Matrix4& view_projection, int width = 0,
                                              int height = 0) const {
        std::vector<gmod::ui::NameTag> tags;
        if (width <= 0 || height <= 0) {
            RECT client{};
            GetClientRect(window_, &client);
            width = client.right - client.left;
            height = client.bottom - client.top;
        }
        if (width <= 0 || height <= 0) return tags;
        const float* m = view_projection.values;
        const bool hide_followed = first_person_ && follow_ >= 0 && !free_view_;
        const auto& options = gmod::ui::settings();
        for (const auto& candidate : tag_candidates_) {
            if (hide_followed && candidate.player == follow_) continue;
            // Renderer space is (-x, z, -y); the tag floats above the head.
            const float world[3]{-candidate.x, candidate.z + candidate.head, -candidate.y};
            const float cx = world[0] * m[0] + world[1] * m[4] + world[2] * m[8] + m[12];
            const float cy = world[0] * m[1] + world[1] * m[5] + world[2] * m[9] + m[13];
            const float cw = world[0] * m[3] + world[1] * m[7] + world[2] * m[11] + m[15];
            if (cw <= 1.0f || cw > TagRange) continue;
            const float nx = cx / cw;
            const float ny = cy / cw;
            if (nx < -1.1f || nx > 1.1f || ny < -1.1f || ny > 1.1f) continue;
            if (!options.esp_through_walls() && audio_assets_) {
                const float head[3]{candidate.x, candidate.y, candidate.z + candidate.head};
                if (audio_assets_->trace.blocked(camera_position_.data(), head)) continue;
            }
            const auto& player = demo_.players[static_cast<std::size_t>(candidate.player)];
            std::string name = gmod::demo::character_name_at(player, time_);
            const std::string steam = display_name(static_cast<std::size_t>(candidate.player));
            if (name.empty()) name = steam;
            gmod::ui::NameTag tag;
            tag.x = (nx * 0.5f + 0.5f) * static_cast<float>(width);
            tag.y = (1.0f - (ny * 0.5f + 0.5f)) * static_cast<float>(height);
            tag.depth = cw;
            tag.name = to_wide(name);
            // The parser garbles some Steam names; showing mojibake is worse than nothing.
            const auto steam_wide = to_wide(steam);
            const bool garbled = std::any_of(steam_wide.begin(), steam_wide.end(),
                [](wchar_t ch) { return ch == L'\xFFFD' || ch < L' '; });
            if (!garbled && steam != name && options.steam_names) tag.steam = steam_wide;
            if (outfit_) {
                tag.job = to_wide(gmod::demo::job_title_at(outfit_->registry, player, time_));
                unsigned char rgb[3];
                gmod::demo::job_color_at(outfit_->registry, player, time_, rgb);
                tag.color = RGB(rgb[0], rgb[1], rgb[2]);
            }
            tag.status = condition_status(player);
            if (tag.status.empty()) tag.status = weapon_status(player);
            if (tag.status.empty() && options.car_speed) {
                if (const auto speed = vehicle_speed(static_cast<std::size_t>(candidate.player))) {
                    tag.status = std::to_wstring(display_speed(*speed)) + L' ' + speed_unit();
                }
            }
            tag.hovered = candidate.player == hovered_ || candidate.player == card_player_;
            tags.push_back(std::move(tag));
        }
        return tags;
    }

private:
    // Firearm state shown under the name: the gun base's networked IronSights, Passive and
    // Reloading. Sniper-base rifles aim through their scope.
    std::wstring weapon_status(const gmod::demo::DemoPlayer& player) const {
        const auto held = gmod::demo::held_weapon_at(demo_, player, time_);
        if (held.handle < 0) return {};
        const auto* info = swep_info_for(held.class_name);
        if (!info || !info->gun) return {};
        const auto flags = gmod::demo::weapon_flags_at(demo_, held.handle, time_);
        if (flags.reloading) return L"RELOADING";
        if (flags.aiming && !flags.passive) {
            return info->base == "perpheads_sniper_base" ? L"SCOPED IN" : L"AIMING DOWN SIGHTS";
        }
        return {};
    }

    std::wstring weapon_name(const std::string& weapon_class) const {
        const auto* info = swep_info_for(weapon_class);
        if (info && !info->name.empty()) return to_wide(info->name);
        return weapon_label(weapon_class);
    }

    // Client pixels and depth of a Source-space point; false when it is behind the camera.
    bool project(const Matrix4& view_projection, float x, float y, float z, int width, int height,
                 float& screen_x, float& screen_y, float& depth) const {
        const float* m = view_projection.values;
        const float world[3]{-x, z, -y};
        const float cx = world[0] * m[0] + world[1] * m[4] + world[2] * m[8] + m[12];
        const float cy = world[0] * m[1] + world[1] * m[5] + world[2] * m[9] + m[13];
        const float cw = world[0] * m[3] + world[1] * m[7] + world[2] * m[11] + m[15];
        if (cw <= 1.0f) return false;
        screen_x = (cx / cw * 0.5f + 0.5f) * static_cast<float>(width);
        screen_y = (1.0f - (cy / cw * 0.5f + 0.5f)) * static_cast<float>(height);
        depth = cw;
        return true;
    }

    // The nearest player whose body (or name tag) is under the client point, -1 for none.
    int pick_player(const Matrix4& view_projection, int x, int y, int width = 0, int height = 0) const {
        if (width <= 0 || height <= 0) {
            RECT client{};
            GetClientRect(window_, &client);
            width = client.right - client.left;
            height = client.bottom - client.top;
        }
        if (width <= 0 || height <= 0) return -1;
        const bool hide_followed = first_person_ && follow_ >= 0 && !free_view_;
        int best = -1;
        float best_depth = FLT_MAX;
        for (const auto& candidate : tag_candidates_) {
            if (hide_followed && candidate.player == follow_) continue;
            float feet_x, feet_y, feet_depth, head_x, head_y, head_depth;
            if (!project(view_projection, candidate.x, candidate.y, candidate.z, width, height,
                         feet_x, feet_y, feet_depth)
                || !project(view_projection, candidate.x, candidate.y, candidate.z + candidate.head + 4.0f,
                            width, height, head_x, head_y, head_depth)
                || feet_depth > TagRange) {
                continue;
            }
            const float tall = std::max(feet_y - head_y, 6.0f);
            const float half = std::max(tall * 0.22f, 7.0f);
            const float px = static_cast<float>(x), py = static_cast<float>(y);
            // The box spans the body plus the name tag drawn above the head.
            if (px < feet_x - half || px > feet_x + half || py < head_y - 34.0f || py > feet_y + 2.0f) continue;
            if (feet_depth < best_depth) {
                best = candidate.player;
                best_depth = feet_depth;
            }
        }
        return best;
    }

    // Hover highlight and click selection. Outside free view the cursor is free and picks;
    // in free view the crosshair at the centre of the view picks.
    int pick_view_player(const Dx12Foundation& renderer, int x, int y, int width = 0, int height = 0) const {
        if (width <= 0 || height <= 0) {
            RECT client{};
            GetClientRect(window_, &client);
            width = client.right;
            height = client.bottom;
        }
        if (const auto uv = renderer.scope_overlay().texture(static_cast<float>(x), static_cast<float>(y), width, height))
            return pick_player(renderer.scope_view_projection(),
                static_cast<int>((*uv)[0]*width), static_cast<int>((*uv)[1]*height), width, height);
        return pick_player(renderer.view_projection(), x, y, width, height);
    }

    void update_picking(Dx12Foundation& renderer) {
        hovered_ = -1;
        if (free_view_ && g_mouse_captured) {
            if (!player_list_open_) {
                RECT client{};
                GetClientRect(window_, &client);
                hovered_ = pick_view_player(renderer, (client.right - client.left) / 2,
                                       (client.bottom - client.top) / 2);
            }
        } else if (!player_list_open_ && !g_mouse_locked_out) {
            POINT cursor{};
            GetCursorPos(&cursor);
            const HWND under = WindowFromPoint(cursor);
            if (under == window_ || (under && under == g_surface)) {
                ScreenToClient(window_, &cursor);
                hovered_ = pick_view_player(renderer, cursor.x, cursor.y);
            }
        }
        g_hover_player = hovered_ >= 0 && !g_mouse_captured;
        if (g_deselect_pending) {
            g_deselect_pending = false;
            g_click_pending = false;
            g_follow_click_pending = false;
            if (card_player_ >= 0 || card_vehicle_ >= 0) {
                card_player_ = -1;
                card_vehicle_ = -1;
                last_card_refresh_ = 0;
            }
        }
        if (g_click_pending && !click_test_) {
            g_click_pending = false;
            const int hit = pick_view_player(renderer, g_click_at.x, g_click_at.y);
            apply_player_click(hit, hit < 0 ? pick_view_vehicle(renderer, g_click_at.x, g_click_at.y) : -1);
        }
    }

    // The nearest drawn vehicle under the client point (index into demo_.props), -1 for none.
    int pick_vehicle(const Matrix4& view_projection, int x, int y, int width, int height) const {
        if (hide_vehicles_ || width <= 0 || height <= 0) return -1;
        constexpr float Radius = 95.0f;
        constexpr float Height = 80.0f;
        constexpr float Range = 6000.0f;
        int best = -1;
        float best_depth = FLT_MAX;
        for (std::size_t index = 0; index < demo_.props.size(); ++index) {
            const auto& prop = demo_.props[index];
            if (!prop.vehicle || prop.seat || prop.model < 0) continue;
            if (gmod::demo::timed_int_at(prop.hidden, time_, 0) != 0) continue;
            gmod::demo::PropPose pose;
            if (!gmod::demo::sample_prop(prop, time_, pose)) continue;
            // A cylinder around the vehicle, projected to its screen bounds.
            float left = FLT_MAX, right = -FLT_MAX, top = FLT_MAX, bottom = -FLT_MAX, nearest = FLT_MAX;
            bool behind = false;
            for (int corner = 0; corner < 16 && !behind; ++corner) {
                const float angle = static_cast<float>(corner % 8) * 0.785398f;
                float sx, sy, depth;
                if (!project(view_projection, pose.x + Radius * std::cos(angle), pose.y + Radius * std::sin(angle),
                             pose.z + (corner < 8 ? 0.0f : Height), width, height, sx, sy, depth)) {
                    behind = true;
                    break;
                }
                left = std::min(left, sx);
                right = std::max(right, sx);
                top = std::min(top, sy);
                bottom = std::max(bottom, sy);
                nearest = std::min(nearest, depth);
            }
            if (behind || nearest > Range) continue;
            const float px = static_cast<float>(x), py = static_cast<float>(y);
            if (px < left || px > right || py < top || py > bottom) continue;
            if (nearest < best_depth) {
                best = static_cast<int>(index);
                best_depth = nearest;
            }
        }
        return best;
    }

    int pick_view_vehicle(const Dx12Foundation& renderer, int x, int y, int width = 0, int height = 0) const {
        if (width <= 0 || height <= 0) {
            RECT client{};
            GetClientRect(window_, &client);
            width = client.right;
            height = client.bottom;
        }
        if (const auto uv = renderer.scope_overlay().texture(static_cast<float>(x), static_cast<float>(y), width, height))
            return pick_vehicle(renderer.scope_view_projection(),
                static_cast<int>((*uv)[0]*width), static_cast<int>((*uv)[1]*height), width, height);
        return pick_vehicle(renderer.view_projection(), x, y, width, height);
    }

    void apply_player_click(int hit, int vehicle) {
        const bool follow_click = std::exchange(g_follow_click_pending, false);
        card_player_ = hit;
        card_vehicle_ = hit < 0 ? vehicle : -1;
        last_card_refresh_ = 0;
        if (hit >= 0 && follow_click) follow(hit);
    }

public:
    // Resolves a --click test against the frame just rendered and records what it selected.
    bool resolve_test_click(const Dx12Foundation& renderer, int width, int height) {
        if (!click_test_ || !g_click_pending) return false;
        g_click_pending = false;
        const int hit = pick_view_player(renderer, g_click_at.x, g_click_at.y, width, height);
        const int vehicle = hit < 0 ? pick_view_vehicle(renderer, g_click_at.x, g_click_at.y, width, height) : -1;
        apply_player_click(hit, vehicle);
        std::ofstream log("build\\click_test.log", std::ios::app);
        log << "click " << g_click_at.x << "," << g_click_at.y << " -> " << hit;
        if (vehicle >= 0) log << " vehicle=" << vehicle;
        log << " followed=" << follow_ << " free_view=" << free_view_;
        if (hit >= 0 || vehicle >= 0) {
            const auto card = build_card_state();
            log << " | " << std::string(card.title.begin(), card.title.end());
            for (const auto& [label, value] : card.rows) {
                log << " | " << std::string(label.begin(), label.end()) << ": "
                    << std::string(value.begin(), value.end());
            }
        }
        log << "\n";
        return true;
    }

private:
    // The player using an entity index at the current time, -1 when none is known.
    int player_for_entity(int entity) const {
        if (entity <= 0) return -1;
        int found = -1;
        for (std::size_t index = 0; index < demo_.players.size(); ++index) {
            const auto& player = demo_.players[index];
            if (player.npc || player.entity != entity) continue;
            const bool connected = player.connected_at <= time_
                && (player.disconnected_at < 0.0f || time_ < player.disconnected_at);
            if (connected) return static_cast<int>(index);
            found = static_cast<int>(index);
        }
        return found;
    }

    std::wstring describe_player(int index) const {
        if (index < 0) return {};
        const auto& player = demo_.players[static_cast<std::size_t>(index)];
        const std::string steam = display_name(static_cast<std::size_t>(index));
        const std::string character = gmod::demo::character_name_at(player, time_);
        if (character.empty() || character == steam) return to_wide(steam);
        return to_wide(character + " (" + steam + ")");
    }

    // What the demo knows about the vehicle in the info card, most importantly who owns it.
    gmod::ui::PlayerCardState build_vehicle_card_state() const {
        gmod::ui::PlayerCardState state;
        if (card_vehicle_ < 0 || static_cast<std::size_t>(card_vehicle_) >= demo_.props.size()) return state;
        const auto& vehicle = demo_.props[static_cast<std::size_t>(card_vehicle_)];
        state.visible = true;
        state.vehicle = true;
        state.title_color = RGB(255, 210, 90);
        std::string model = vehicle.model >= 0 && static_cast<std::size_t>(vehicle.model) < demo_.models.size()
            ? demo_.models[static_cast<std::size_t>(vehicle.model)] : std::string();
        std::replace(model.begin(), model.end(), '\\', '/');
        const auto slash = model.find_last_of('/');
        std::string name = slash == std::string::npos ? model : model.substr(slash + 1);
        if (name.size() > 4 && name.ends_with(".mdl")) name.resize(name.size() - 4);
        state.title = name.empty() ? L"Vehicle" : L"Vehicle - " + to_wide(name);
        const auto add = [&](const wchar_t* label, std::wstring value) {
            state.rows.emplace_back(label, std::move(value));
        };
        const int owner_entity = gmod::demo::timed_int_at(vehicle.vehicle_owner, time_, -1);
        const int owner = player_for_entity(owner_entity);
        std::wstring owner_text = owner >= 0 ? describe_player(owner)
            : owner_entity > 0 ? L"Unknown player (entity #" + std::to_wstring(owner_entity) + L")"
            : L"Not recorded";
        add(L"Owner", owner_text);
        if (owner >= 0) {
            const auto& player = demo_.players[static_cast<std::size_t>(owner)];
            std::string ids = player.steam_id.empty() ? std::string("unknown") : player.steam_id;
            if (!player.steam_id64.empty()) ids += "  (" + player.steam_id64 + ")";
            add(L"Owner SteamID", to_wide(ids));
        }
        const auto plate = gmod::demo::timed_text_at(vehicle.registration, time_);
        add(L"Registration", plate.empty() ? L"None" : to_wide(plate));
        int driver = -1;
        if (!vehicle.owners.empty()) {
            const auto after = std::upper_bound(vehicle.owners.begin(), vehicle.owners.end(), time_,
                [](float time, const auto& key) { return time < key.time; });
            if (after != vehicle.owners.begin()) driver = player_for_entity(std::prev(after)->owner);
        }
        add(L"Driver", driver >= 0 ? describe_player(driver) : L"Nobody");
        add(L"Entity", L"#" + std::to_wstring(vehicle.entity));
        return state;
    }

    // Everything the demo knows about the player in the info card.
    gmod::ui::PlayerCardState build_card_state() const {
        if (card_vehicle_ >= 0) return build_vehicle_card_state();
        gmod::ui::PlayerCardState state;
        if (card_player_ < 0 || static_cast<std::size_t>(card_player_) >= demo_.players.size()) return state;
        const auto index = static_cast<std::size_t>(card_player_);
        const auto& player = demo_.players[index];
        state.visible = true;
        const std::string steam = display_name(index);
        const std::string character = gmod::demo::character_name_at(player, time_);
        state.title = to_wide(character.empty() ? steam : character);
        state.followed = follow_ == card_player_ && !free_view_;
        state.has_voice = player.voice_clips > 0;
        state.solo = effective_isolation() == card_player_;
        state.muted = muted_voices_.count(card_player_) != 0;
        const auto add = [&](const wchar_t* label, std::wstring value) {
            state.rows.emplace_back(label, std::move(value));
        };
        add(L"Steam name", to_wide(steam));
        std::string ids = player.steam_id.empty() ? std::string("unknown") : player.steam_id;
        if (!player.steam_id64.empty()) ids += "  (" + player.steam_id64 + ")";
        add(L"SteamID", to_wide(ids));
        if (outfit_) {
            const auto& registry = outfit_->registry;
            unsigned char rgb[3];
            gmod::demo::job_color_at(registry, player, time_, rgb);
            state.title_color = RGB(rgb[0], rgb[1], rgb[2]);
            const int team = gmod::demo::timed_int_at(player.job_team, time_, registry.team_citizen);
            const auto found = registry.teams.find(team);
            std::string role = found != registry.teams.end() ? found->second.name : "Team " + std::to_string(team);
            const auto title = gmod::demo::job_title_at(registry, player, time_);
            if (!title.empty() && title != role) role += " - " + title;
            add(L"Role", to_wide(role));
        }
        gmod::demo::PlayerPose pose;
        const bool present = gmod::demo::sample_player(player, time_, pose);
        const auto* info = gmod::demo::player_info_at(player, time_);
        std::wstring status;
        if (ragdolled_[index]) status = L"Ragdolled (knocked out or dead)";
        else if (!present) status = L"Not in the recorder's view";
        else if (info && info->life_state > 0) status = L"Dead";
        else if ((pose.flags & gmod::demo::PlayerFlagSeated) != 0) {
            status = (pose.flags & gmod::demo::PlayerFlagDriver) != 0 ? L"Driving a vehicle" : L"In a vehicle";
        } else {
            const float speed = std::hypot(pose.vx, pose.vy);
            status = (pose.flags & 2) != 0 ? L"Crouching" : speed >= 190.0f ? L"Running"
                : speed >= 30.0f ? L"Walking" : L"Standing";
        }
        add(L"Status", status);
        if (info && info->health >= 0) add(L"Health", std::to_wstring(info->health));
        const auto held = gmod::demo::held_weapon_at(demo_, player, time_);
        std::wstring holding = L"Nothing";
        if (!held.class_name.empty()) {
            holding = weapon_name(held.class_name);
            if (held.hold_type == "normal") holding += L" (unarmed)";
        }
        add(L"Holding", holding);
        if (const auto* swep = swep_info_for(held.class_name); swep && swep->gun && held.handle >= 0) {
            const auto flags = gmod::demo::weapon_flags_at(demo_, held.handle, time_);
            std::wstring weapon_state;
            const auto append = [&](const wchar_t* text) {
                if (!weapon_state.empty()) weapon_state += L", ";
                weapon_state += text;
            };
            if (flags.aiming && !flags.passive) {
                append(swep->base == "perpheads_sniper_base" ? L"Scoped in" : L"Aiming down sights");
            }
            if (flags.passive) append(L"Lowered (passive)");
            if (flags.reloading) append(L"Reloading");
            add(L"Weapon", weapon_state.empty() ? L"Ready" : weapon_state);
        }
        std::wstring inventory;
        std::vector<std::wstring> listed;
        for (const auto& weapon : gmod::demo::inventory_at(demo_, player, time_)) {
            auto name = weapon_name(weapon);
            if (std::find(listed.begin(), listed.end(), name) != listed.end()) continue;
            if (!inventory.empty()) inventory += L", ";
            inventory += name;
            listed.push_back(std::move(name));
        }
        add(L"Inventory", inventory.empty() ? L"Nothing seen" : inventory);
        const int channel = gmod::demo::timed_int_at(player.radio_channel, time_, 0);
        if (channel > 0) {
            std::string name;
            if (outfit_) {
                const auto fixed = outfit_->registry.radio_channels.find(channel);
                if (fixed != outfit_->registry.radio_channels.end()) name = fixed->second;
            }
            if (name.empty()) {
                const auto named = demo_.radio_channel_names.find(channel);
                name = named != demo_.radio_channel_names.end() ? named->second
                                                               : "channel " + std::to_string(channel);
            }
            add(L"Radio", L"Transmitting on " + to_wide(name));
        }
        bool talking = false;
        for (const auto& clip : demo_.voice) {
            if (clip.player == card_player_ && clip.time <= time_ && time_ < clip.time + clip.duration) {
                talking = true;
                break;
            }
        }
        add(L"Voice", (talking ? std::wstring(L"Talking now, ") : std::wstring())
                          + std::to_wstring(player.voice_clips) + L" clips in the demo");
        add(L"Entity", L"#" + std::to_wstring(player.entity) + L"  (user id " + std::to_wstring(player.user_id) + L")");
        return state;
    }

public:

private:
    static constexpr float TagRange = 2500.0f;

    static std::wstring weapon_label(const std::string& weapon) {
        std::string label = weapon;
        for (const char* prefix : {"weapon_perp_phys_", "weapon_perp_", "weapon_", "roleplay_"}) {
            if (label.rfind(prefix, 0) == 0) {
                label.erase(0, std::strlen(prefix));
                break;
            }
        }
        std::replace(label.begin(), label.end(), '_', ' ');
        return to_wide(label);
    }

    void create_player_list(HWND window) {
        gmod::ui::PlayerListActions actions;
        actions.select = [this](int player) {
            follow_ = player;
            set_free_view(false);
            refresh_player_list();
        };
        actions.isolate = [this](int player) {
            // Toggle: isolating the same player again returns everyone's voice.
            isolated_voice_ = isolated_voice_ == player ? -1 : player;
            refresh_player_list();
        };
        player_list_ = std::make_unique<gmod::ui::PlayerListPanel>(window, std::move(actions));
        g_player_list = player_list_.get();
    }

    void refresh_player_list() {
        last_list_refresh_ = GetTickCount();
        std::vector<gmod::ui::PlayerListRow> rows;
        int in_world = 0;
        for (std::size_t index = 0; index < demo_.players.size(); ++index) {
            const auto& player = demo_.players[index];
            if (player.samples.empty() || player.npc) continue;
            gmod::demo::PlayerPose pose;
            gmod::ui::PlayerListRow row;
            row.id = static_cast<int>(index);
            row.name = to_wide(display_name(index));
            row.present = gmod::demo::sample_player(player, time_, pose);
            row.followed = row.id == follow_;
            row.recorder = row.id == demo_.recorder_player;
            row.has_voice = player.voice_clips > 0;
            row.isolated = row.id == isolated_voice_;
            const auto* info = gmod::demo::player_info_at(player, time_);
            std::wstring detail = player.steam_id.empty() ? L"no Steam ID" : to_wide(player.steam_id);
            if (info && info->team >= 0) detail += L"  |  Team " + std::to_wstring(info->team);
            if (info && !info->weapon.empty()) {
                detail += L"  |  " + weapon_label(info->weapon);
            }
            if (row.present && info && info->health >= 0) {
                detail += L"  |  HP " + std::to_wstring(info->health);
            }
            if (player.voice_clips > 0) {
                detail += L"  |  " + std::to_wstring(player.voice_clips) + L" voice clips";
            }
            detail += L"  |  ent " + std::to_wstring(player.entity);
            row.detail = std::move(detail);
            if (!row.present) {
                row.status = L"Not in world";
            } else {
                ++in_world;
                if ((pose.flags & gmod::demo::PlayerFlagSeated) != 0) row.status = L"In vehicle";
                else if (info && info->life_state > 0) row.status = L"Dead";
                else if ((pose.flags & 2) != 0) row.status = L"Crouching";
                else row.status = L"In world";
            }
            rows.push_back(std::move(row));
        }
        const auto lower = [](std::wstring text) {
            std::transform(text.begin(), text.end(), text.begin(), ::towlower);
            return text;
        };
        const auto garbled = [](const std::wstring& name) {
            return std::any_of(name.begin(), name.end(),
                               [](wchar_t ch) { return ch == L'\xFFFD' || ch < L' '; });
        };
        std::stable_sort(rows.begin(), rows.end(), [&](const auto& a, const auto& b) {
            if (a.present != b.present) return a.present;
            if (a.recorder != b.recorder) return a.recorder;
            const bool a_garbled = garbled(a.name);
            const bool b_garbled = garbled(b.name);
            if (a_garbled != b_garbled) return b_garbled;
            return lower(a.name) < lower(b.name);
        });
        std::wstring title = L"Players  -  " + std::to_wstring(in_world) + L" in world, "
            + std::to_wstring(rows.size()) + L" in demo";
        player_list_->set_rows(std::move(rows), std::move(title));
    }

    // Free view detaches the camera from the followed player and captures the mouse
    // for look-around. Everywhere else the cursor stays free for the on-screen UI.
    void set_free_view(bool enabled) {
        if (enabled == free_view_) return;
        if (!enabled && follow_ < 0) {
            // Nobody is followed yet: fall back to the recorder, then anyone in the world.
            if (demo_.recorder_player >= 0) follow_ = demo_.recorder_player;
            else if (!visible_players_.empty()) follow_ = visible_players_.front();
            else return;
        }
        free_view_ = enabled;
        g_free_view = enabled;
        if (enabled) {
            if (!g_mouse_locked_out && !controls_open_) capture_mouse(window_);
        } else {
            release_mouse();
            mouse_was_captured_ = false;
        }
    }

    // While any panel that needs the cursor is open (player list, Esc menu, console) the
    // mouse is released and scene clicks do nothing; the last one to close restores mouse
    // look if free view was active.
    void begin_modal() {
        if (modal_depth_++ == 0) {
            mouse_was_captured_ = g_mouse_captured;
            release_mouse();
            g_mouse_locked_out = true;
        }
    }

    void end_modal(bool restore_mouse) {
        if (modal_depth_ == 0 || --modal_depth_ > 0) return;
        g_mouse_locked_out = false;
        if (restore_mouse && mouse_was_captured_ && free_view_) capture_mouse(window_);
        mouse_was_captured_ = false;
    }

    // Holding P shows the player list and frees the cursor;
    // releasing it hides them and restores mouse look if free view was active.
    void open_player_list() {
        player_list_open_ = true;
        begin_modal();
        refresh_player_list();
        player_list_->set_visible(true);
    }

    void close_player_list(bool restore_mouse) {
        player_list_open_ = false;
        player_list_->set_visible(false);
        end_modal(restore_mouse);
    }

    // Tab shows the playback bar and frees the cursor to use it. Unlike the menus it is not
    // modal: the scene still turns by dragging and a click still selects players.
    void open_controls() {
        if (controls_open_ || !timeline_) return;
        controls_open_ = true;
        controls_recapture_ = g_mouse_captured;
        release_mouse();
        g_drag_look = true;
        timeline_->set_visible(true);
    }

    void close_controls(bool restore_mouse) {
        if (!controls_open_) return;
        controls_open_ = false;
        g_drag_look = false;
        if (timeline_) timeline_->set_visible(false);
        if (restore_mouse && controls_recapture_ && free_view_ && !g_mouse_locked_out) {
            capture_mouse(window_);
        }
        controls_recapture_ = false;
    }

    void create_timeline(HWND window, bool show) {
        std::vector<std::wstring> labels;
        for (const float speed : kSpeeds) {
            wchar_t text[16];
            swprintf_s(text, L"%gx", speed);
            labels.emplace_back(text);
        }
        gmod::ui::TimelineActions actions;
        actions.play = [this] { play_forward(); };
        actions.reverse = [this] { play_reverse(); };
        actions.pause = [this] { playing_ = false; };
        actions.set_speed = [this](int index) {
            speed_index_ = std::clamp(index, 0, static_cast<int>(std::size(kSpeeds)) - 1);
        };
        actions.skip = [this](float seconds) { seek(time_ + seconds); };
        actions.seek = [this](float seconds) { seek(seconds); };
        // Dragging the thumb holds playback and resumes it afterwards.
        actions.scrub = [this](bool begin) {
            if (begin) {
                resume_after_scrub_ = playing_;
                playing_ = false;
            } else {
                playing_ = resume_after_scrub_ && time_ > 0.0f && time_ < demo_.duration;
                resume_after_scrub_ = false;
            }
        };
        timeline_ = std::make_unique<gmod::ui::TimelineBar>(
            window, std::move(labels), std::move(actions));
        timeline_->set_visible(show);
    }

    // ---- Text log, kill feed and HUD ------------------------------------------------------

    struct FeedEvent {
        float time = 0.0f;
        int player = -1;
        int kind = 0;  // 0 died, 1 knocked out, 2 got back up
    };
    struct ConsoleEntry {
        float time = 0.0f;
        std::wstring text;
        COLORREF color = RGB(255, 255, 255);
    };

    COLORREF job_colour(const gmod::demo::DemoPlayer& player, float time) const {
        unsigned char rgb[3]{255, 255, 255};
        if (outfit_) gmod::demo::job_color_at(outfit_->registry, player, time, rgb);
        return RGB(rgb[0], rgb[1], rgb[2]);
    }

    std::string person_name(std::size_t index, float time) const {
        std::string name = gmod::demo::character_name_at(demo_.players[index], time);
        return name.empty() ? display_name(index) : name;
    }

    // Deaths come from the networked life state; knockouts and getting back up from a player
    // turning into (and leaving) a ragdoll while alive. The demo does not say who dealt the
    // damage, so the feed names only the victim.
    void build_feed_events() {
        for (std::size_t index = 0; index < demo_.players.size(); ++index) {
            const auto& player = demo_.players[index];
            if (player.npc) continue;
            std::vector<float> deaths;
            int previous = -1;
            for (const auto& key : player.info) {
                if (key.life_state < 0) continue;
                if (previous == 0 && key.life_state != 0) deaths.push_back(key.time);
                previous = key.life_state;
            }
            const auto near_death = [&](float time) {
                return std::any_of(deaths.begin(), deaths.end(),
                                   [&](float death) { return std::fabs(death - time) < 4.0f; });
            };
            for (const float time : deaths) feed_events_.push_back({time, static_cast<int>(index), 0});
            int ragdoll = 0;
            for (const auto& key : player.ragdoll) {
                if (key.value != 0 && ragdoll == 0 && !near_death(key.time)) {
                    feed_events_.push_back({key.time, static_cast<int>(index), 1});
                } else if (key.value == 0 && ragdoll != 0 && !near_death(key.time)) {
                    feed_events_.push_back({key.time, static_cast<int>(index), 2});
                }
                ragdoll = key.value;
            }
        }
        std::stable_sort(feed_events_.begin(), feed_events_.end(),
                         [](const FeedEvent& a, const FeedEvent& b) { return a.time < b.time; });
    }

    std::wstring feed_action(int kind) const {
        return kind == 0 ? L"died" : kind == 1 ? L"was knocked out" : L"got back up";
    }

    // Everything the console prints as the demo plays: chat in the server's own colours,
    // notices, joins and leaves, and the feed's deaths.
    void build_console_entries() {
        for (const auto& line : demo_.log) {
            ConsoleEntry entry;
            entry.time = line.time;
            const auto& style = gmod::demo::chat_style(line.category);
            const COLORREF category_colour = RGB(style.r, style.g, style.b);
            const std::wstring tag = L"[" + to_wide(style.name) + L"] ";
            const int speaker = gmod::demo::player_index_at(demo_, line.entity, line.time);
            switch (line.kind) {
            case gmod::demo::LogKind::Chat:
            case gmod::demo::LogKind::Ooc: {
                std::string who = "?";
                if (speaker >= 0) {
                    const auto index = static_cast<std::size_t>(speaker);
                    // Out-of-character channels show the Steam name, in-character the roleplay name.
                    who = line.kind == gmod::demo::LogKind::Ooc ? display_name(index)
                                                                : person_name(index, line.time);
                    // Players whose names the demo's truncated tables lost are known by entity only.
                    if (who.empty()) who = person_name(index, line.time);
                    if (who.empty()) who = "Player #" + std::to_string(line.entity);
                }
                entry.text = tag + to_wide(who) + L": " + to_wide(line.text);
                entry.color = category_colour;
                break;
            }
            case gmod::demo::LogKind::Narration:
                entry.text = to_wide(line.text);
                entry.color = category_colour;
                break;
            case gmod::demo::LogKind::Npc:
                entry.text = tag + to_wide(line.sender) + L": " + to_wide(line.text);
                entry.color = category_colour;
                break;
            case gmod::demo::LogKind::Announcement:
                entry.text = L"[OOC] Announcement: " + to_wide(line.text);
                entry.color = RGB(255, 255, 255);
                break;
            case gmod::demo::LogKind::Notify:
                entry.text = to_wide(line.text);
                entry.color = line.category == 1 ? RGB(255, 110, 110) : RGB(255, 235, 120);
                break;
            case gmod::demo::LogKind::Connection:
                entry.text = to_wide(line.text);
                entry.color = line.text.find("connected") != std::string::npos ? RGB(150, 255, 150)
                                                                               : RGB(255, 170, 170);
                break;
            }
            console_entries_.push_back(std::move(entry));
        }
        for (const auto& event : feed_events_) {
            ConsoleEntry entry;
            entry.time = event.time;
            entry.text = to_wide(person_name(static_cast<std::size_t>(event.player), event.time))
                + L" " + feed_action(event.kind);
            entry.color = RGB(255, 140, 140);
            console_entries_.push_back(std::move(entry));
        }
        std::stable_sort(console_entries_.begin(), console_entries_.end(),
                         [](const ConsoleEntry& a, const ConsoleEntry& b) { return a.time < b.time; });
    }

    void console_banner() {
        console_->print(L"GMod Replay console", RGB(255, 255, 255));
        console_->print(L"Map: " + to_wide(demo_.map_name) + L"   Recorded by: " + to_wide(demo_.client_name)
                            + L"   Length: " + clock(demo_.duration),
                        RGB(200, 200, 200));
        console_->print(L"Type \"help\" for the list of commands.", RGB(200, 200, 200));
        console_->print(L"", RGB(255, 255, 255));
        // The program's own output (what used to go to the command prompt window).
        constexpr std::size_t ProgramLines = 200;
        const std::size_t count = log_line_count();
        const std::size_t first = count > ProgramLines ? count - ProgramLines : 0;
        for (const auto& line : log_lines(first, count)) print_program_line(line);
        log_shown_ = count;
        if (count > 0) console_->print(L"", RGB(255, 255, 255));
    }

    void print_program_line(const std::wstring& line) {
        std::wstring lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        const bool problem = lower.find(L"unavailable") != std::wstring::npos
            || lower.find(L"error") != std::wstring::npos || lower.find(L"could not") != std::wstring::npos
            || lower.find(L"fallback") != std::wstring::npos;
        console_->print(line, problem ? RGB(255, 170, 90) : RGB(160, 170, 185));
    }

    // Keeps the console's log in step with the demo clock: new lines are appended as time
    // passes and the log is rebuilt after a seek.
    void sync_console(bool rebuild) {
        if (!console_) return;
        constexpr std::size_t HistoryLines = 300;
        if (!rebuild && (time_ < console_time_ - 0.001f || time_ - console_time_ > 5.0f)) rebuild = true;
        if (rebuild) {
            console_->clear();
            console_banner();
            const auto end = std::upper_bound(
                console_entries_.begin(), console_entries_.end(), time_,
                [](float value, const ConsoleEntry& entry) { return value < entry.time; });
            console_next_ = static_cast<std::size_t>(end - console_entries_.begin());
            const std::size_t first = console_next_ > HistoryLines ? console_next_ - HistoryLines : 0;
            for (std::size_t index = first; index < console_next_; ++index) {
                console_->print(console_entries_[index].text, console_entries_[index].color);
            }
        } else {
            const std::size_t count = log_line_count();
            if (count > log_shown_) {
                for (const auto& line : log_lines(log_shown_, count)) print_program_line(line);
                log_shown_ = count;
            }
            while (console_next_ < console_entries_.size() && console_entries_[console_next_].time <= time_) {
                console_->print(console_entries_[console_next_].text, console_entries_[console_next_].color);
                ++console_next_;
            }
        }
        console_time_ = time_;
    }

    struct SettingName {
        const wchar_t* name;
        bool gmod::ui::ViewerSettings::*member;
    };
    static const std::vector<SettingName>& setting_names() {
        static const std::vector<SettingName> names{
            {L"names", &gmod::ui::ViewerSettings::names},
            {L"steam_names", &gmod::ui::ViewerSettings::steam_names},
            {L"names_through_walls", &gmod::ui::ViewerSettings::names_through_walls},
            {L"outline_all_players", &gmod::ui::ViewerSettings::outline_all_players},
            {L"car_speed", &gmod::ui::ViewerSettings::car_speed},
            {L"speed_mph", &gmod::ui::ViewerSettings::speed_mph},
            {L"kill_feed", &gmod::ui::ViewerSettings::kill_feed},
            {L"radio_hud", &gmod::ui::ViewerSettings::radio_hud},
            {L"dialogs", &gmod::ui::ViewerSettings::dialogs},
            {L"decals", &gmod::ui::ViewerSettings::decals},
            {L"muzzle_flash", &gmod::ui::ViewerSettings::muzzle_flash},
        };
        return names;
    }

    static const std::vector<std::wstring>& console_commands() {
        static const std::vector<std::wstring> commands{
            L"help", L"clear", L"echo", L"status", L"players", L"timescale", L"seek", L"pause", L"play",
            L"follow", L"free", L"firstperson", L"volume", L"stopsound", L"quit"};
        return commands;
    }

    std::vector<std::wstring> complete_console(const std::wstring& prefix) const {
        std::wstring lower = prefix;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        std::vector<std::wstring> matches;
        for (const auto& command : console_commands()) {
            if (command.rfind(lower, 0) == 0) matches.push_back(command);
        }
        for (const auto& setting : setting_names()) {
            if (std::wstring(setting.name).rfind(lower, 0) == 0) matches.push_back(setting.name);
        }
        return matches;
    }

    // Seconds from "90", "1:30" or "+10" / "-10" relative to now.
    std::optional<float> parse_time(const std::wstring& text) const {
        if (text.empty()) return std::nullopt;
        try {
            const bool relative = text[0] == L'+' || text[0] == L'-';
            const auto colon = text.find(L':');
            float seconds = 0.0f;
            if (colon != std::wstring::npos) {
                seconds = std::stof(text.substr(0, colon)) * 60.0f + std::stof(text.substr(colon + 1));
            } else {
                seconds = std::stof(text);
            }
            return relative ? time_ + seconds : seconds;
        } catch (...) {
            return std::nullopt;
        }
    }

    void run_console_command(const std::wstring& line) {
        std::wistringstream stream(line);
        std::wstring command;
        stream >> command;
        std::transform(command.begin(), command.end(), command.begin(), ::towlower);
        std::wstring rest;
        std::getline(stream, rest);
        const auto first = rest.find_first_not_of(L' ');
        rest = first == std::wstring::npos ? std::wstring() : rest.substr(first);
        const auto say = [&](const std::wstring& text, COLORREF colour = RGB(255, 255, 255)) {
            console_->print(text, colour);
        };
        auto& options = gmod::ui::settings();
        if (command == L"help") {
            say(L"Commands:");
            say(L"  help, clear, echo <text>, status, players, quit");
            say(L"  timescale <x>        playback speed (0.25 - 32)");
            say(L"  seek <time>          90, 1:30, +10 or -10");
            say(L"  play, pause");
            say(L"  follow <name>        chase a player (name, character or Steam name)");
            say(L"  free                 free camera");
            say(L"  firstperson [0|1]    view through the followed player's eyes");
            say(L"  volume [0-100]");
            say(L"  stopsound            stop current effects and ambience; keep voices");
            say(L"Options (give 0 or 1, or nothing to read them):");
            std::wstring names;
            for (const auto& setting : setting_names()) names += std::wstring(L"  ") + setting.name;
            say(names, RGB(200, 200, 200));
        } else if (command == L"clear") {
            console_->clear();
        } else if (command == L"echo") {
            say(rest);
        } else if (command == L"status") {
            say(L"map      : " + to_wide(demo_.map_name));
            say(L"recorder : " + to_wide(demo_.client_name));
            say(L"time     : " + clock(time_) + L" / " + clock(demo_.duration));
            say(L"players  : " + std::to_wstring(visible_players_.size()) + L" in the world, "
                + std::to_wstring(demo_.players.size()) + L" in the demo");
        } else if (command == L"players") {
            say(L"# userid name                          steamid");
            for (const int index : visible_players_) {
                const auto& player = demo_.players[static_cast<std::size_t>(index)];
                std::wstring where;
                if (const auto speed = vehicle_speed(static_cast<std::size_t>(index))) {
                    where = L"  [vehicle, " + std::to_wstring(display_speed(*speed)) + L" " + speed_unit() + L"]";
                }
                say(L"# " + std::to_wstring(player.user_id) + L" \"" + to_wide(person_name(static_cast<std::size_t>(index), time_))
                    + L"\"  " + to_wide(player.steam_id) + L"  #" + std::to_wstring(index) + where);
            }
        } else if (command == L"timescale") {
            const auto value = parse_time(rest);
            if (!value) {
                say(L"\"timescale\" = \"" + number_text(kSpeeds[speed_index_]) + L"\"");
            } else {
                int best = 0;
                for (int index = 0; index < static_cast<int>(std::size(kSpeeds)); ++index) {
                    if (std::fabs(kSpeeds[index] - *value) < std::fabs(kSpeeds[best] - *value)) best = index;
                }
                speed_index_ = best;
                say(L"timescale set to " + number_text(kSpeeds[best]));
            }
        } else if (command == L"seek") {
            const auto value = parse_time(rest);
            if (!value) {
                say(L"Usage: seek <seconds | m:ss | +n | -n>", RGB(255, 140, 140));
            } else {
                seek(*value);
                say(L"Seeked to " + clock(time_));
            }
        } else if (command == L"pause") {
            playing_ = false;
        } else if (command == L"play") {
            play_forward();
        } else if (command == L"free") {
            set_free_view(true);
        } else if (command == L"firstperson") {
            first_person_ = rest.empty() ? !first_person_ : rest != L"0";
        } else if (command == L"stopsound") {
            stopped_sound_events_.stop(time_);
            active_loops_.clear();
            if (g_audio) g_audio->stop_sounds();
            say(L"Stopped current effects and ambience. Voices and new sound starts are unaffected.");
        } else if (command == L"volume") {
            if (rest.empty()) {
                say(L"\"volume\" = \"" + std::to_wstring(options.volume) + L"\"");
            } else {
                options.volume = std::clamp(_wtoi(rest.c_str()), 0, 100);
                options.save();
            }
        } else if (command == L"follow") {
            std::wstring needle = rest;
            std::transform(needle.begin(), needle.end(), needle.begin(), ::towlower);
            int found = -1;
            for (const int index : visible_players_) {
                std::wstring name = to_wide(person_name(static_cast<std::size_t>(index), time_)) + L" "
                    + to_wide(display_name(static_cast<std::size_t>(index)));
                std::transform(name.begin(), name.end(), name.begin(), ::towlower);
                if (!needle.empty() && name.find(needle) != std::wstring::npos) {
                    found = index;
                    break;
                }
            }
            if (found < 0) {
                say(L"No player in the world matches \"" + rest + L"\".", RGB(255, 140, 140));
            } else {
                follow(found);
                say(L"Following " + to_wide(person_name(static_cast<std::size_t>(found), time_)));
            }
        } else if (command == L"quit" || command == L"exit") {
            PostMessageW(window_, WM_CLOSE, 0, 0);
        } else {
            bool handled = false;
            for (const auto& setting : setting_names()) {
                if (command != setting.name) continue;
                handled = true;
                if (!rest.empty()) {
                    options.*setting.member = rest != L"0";
                    options.save();
                }
                say(L"\"" + std::wstring(setting.name) + L"\" = \"" + (options.*setting.member ? L"1" : L"0") + L"\"");
            }
            if (!handled) say(L"Unknown command: " + command, RGB(255, 140, 140));
        }
    }

    // Both vehicle modes are chase cameras. Preserve the on-foot view across entry,
    // exit, seeks and follow changes; R switches only the car's look behaviour.
    void update_vehicle_view() {
        bool seated = false;
        if (follow_ >= 0 && !free_view_) {
            gmod::demo::PlayerPose pose;
            seated = gmod::demo::sample_player(demo_.players[static_cast<std::size_t>(follow_)], time_, pose)
                && (pose.flags & gmod::demo::PlayerFlagInCar) != 0;
        }
        vehicle_camera_.update(seated,follow_,first_person_);
    }

    // Speed in Source units per second of a player riding a vehicle; none when on foot.
    std::optional<float> vehicle_speed(std::size_t index) const {
        gmod::demo::PlayerPose pose;
        if (!gmod::demo::sample_player(demo_.players[index], time_, pose)
            || (pose.flags & gmod::demo::PlayerFlagSeated) == 0) {
            return std::nullopt;
        }
        return std::sqrt(pose.vx * pose.vx + pose.vy * pose.vy + pose.vz * pose.vz);
    }

    // The server's own conversion: units per second / 17.6 gives mph, times 1.609344 km/h.
    int display_speed(float units_per_second) const {
        const float mph = std::round(units_per_second / 17.6f);
        return static_cast<int>(gmod::ui::settings().speed_mph ? mph : std::round(mph * 1.609344f));
    }

    const wchar_t* speed_unit() const { return gmod::ui::settings().speed_mph ? L"MPH" : L"KPH"; }

    static std::wstring number_text(float value) {
        wchar_t text[32];
        swprintf_s(text, L"%g", value);
        return text;
    }

public:
    gmod::ui::HudState build_hud_state() const {
        gmod::ui::HudState state;
        const auto& options = gmod::ui::settings();
        state.vehicle_camera = vehicle_camera_.active && !free_view_;
        state.vehicle_free_look = vehicle_camera_.free_look;
        if (GetTickCount() - started_ticks_ < 15000) {
            state.hint = L"F1: all controls    Hold P: players    TAB: playback    Tap SHIFT: play/pause    Click: select    Double-click: follow    ESC: options    `: console    F: free view    R: third person";
        }
        if (options.car_speed && follow_ >= 0 && !free_view_) {
            if (const auto speed = vehicle_speed(static_cast<std::size_t>(follow_))) {
                state.speed_visible = true;
                state.speed = display_speed(*speed);
                state.speed_unit = speed_unit();
            }
        }
        if (options.kill_feed) {
            for (auto it = feed_events_.rbegin(); it != feed_events_.rend() && state.feed.size() < 6; ++it) {
                if (it->time > time_) continue;
                if (time_ - it->time > 10.0f) break;
                const auto index = static_cast<std::size_t>(it->player);
                gmod::ui::FeedLine line;
                line.left = to_wide(person_name(index, it->time));
                line.left_color = job_colour(demo_.players[index], it->time);
                line.action = feed_action(it->kind);
                state.feed.push_back(std::move(line));
            }
        }
        return state;
    }

private:
    const gmod::demo::WeaponAttachmentDef* fitted_sight(int handle) const {
        bool known = false;
        const gmod::demo::WeaponAttachmentDef* fallback = nullptr;
        for (int id : gmod::demo::weapon_attachments_at(demo_,handle,time_,known)) {
            const auto found = outfit_->registry.weapon_attachments.find(id);
            if (found != outfit_->registry.weapon_attachments.end() && found->second.iron_pos) {
                if (found->second.sight || found->second.magnification > 0) return &found->second;
                fallback = &found->second;
            }
        }
        return fallback;
    }

    void update_scope(Dx12Foundation& renderer) {
        float magnification = 0;
        if (first_person_ && !free_view_ && follow_ >= 0 && !ragdolled_[static_cast<std::size_t>(follow_)]) {
            const auto& player = demo_.players[static_cast<std::size_t>(follow_)];
            const auto held = gmod::demo::held_weapon_at(demo_, player, time_);
            if (const auto* sight = fitted_sight(held.handle)) {
                const auto flags = gmod::demo::weapon_flags_at(demo_,held.handle,time_);
                if (flags.aiming && !flags.passive && !flags.reloading
                    && time_-flags.aiming_since > 0.15f)
                    magnification = sight->magnification;
            }
        }
        renderer.set_scope_magnification(magnification);
    }

    bool edge(int key) {
        const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        bool& was = key_was_down_[key & 0xFF];
        const bool pressed = down && !was;
        was = down;
        return pressed;
    }

    // Playback is mouse-only (the timeline bar, toggled with Tab). Keys switch views;
    // Esc opens the options menu.
    void handle_keys() {
        const bool escape = edge(VK_ESCAPE);
        if (g_text_input) {
            // The console owns the keyboard.
            return;
        }
        if (escape && !g_esc_consumed) {
            if (esc_open_) close_esc_menu();
            else open_esc_menu();
        }
        if (edge(VK_F1)) {
            open_esc_menu();
            if (esc_open_) esc_menu_->show_help();
        }
        if (esc_open_) return;
        if (edge('C') && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && player_card_) player_card_->copy_selection();
        if (edge('F')) set_free_view(!free_view_);
        if (edge('R')) {
            update_vehicle_view();
            vehicle_camera_.toggle(first_person_);
        }
        // Hold P for the player list; Tab shows or hides the playback bar.
        const bool list_down = (GetAsyncKeyState('P') & 0x8000) != 0 || player_list_pinned_;
        if (list_down && !player_list_open_) {
            open_player_list();
        } else if (!list_down && player_list_open_) {
            close_player_list(true);
        }
        if (edge(VK_TAB)) {
            if (controls_open_) close_controls(true);
            else open_controls();
        }
        // A quick tap of Shift plays or pauses; holding it (the free-view speed boost) or
        // using it with another key does not.
        const bool shift_down = GetForegroundWindow() == window_ && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        if (shift_down) {
            if (shift_down_at_ == 0) {
                shift_down_at_ = GetTickCount();
                shift_spoiled_ = false;
            }
            for (int key = 0x08; key <= 0xFE && !shift_spoiled_; ++key) {
                if (key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT) continue;
                if (key >= VK_LBUTTON && key <= VK_XBUTTON2) continue;
                if (GetAsyncKeyState(key) & 0x8000) shift_spoiled_ = true;
            }
        } else if (shift_down_at_ != 0) {
            const bool tap = !shift_spoiled_ && GetTickCount() - shift_down_at_ < 300;
            shift_down_at_ = 0;
            if (tap) {
                if (playing_) playing_ = false;
                else play_forward();
            }
        }
    }

    void open_esc_menu() {
        if (esc_open_ || !esc_menu_) return;
        if (player_list_open_ && !player_list_pinned_) close_player_list(false);
        close_controls(false);
        esc_open_ = true;
        begin_modal();
        esc_menu_->set_visible(true);
    }

    void close_esc_menu() {
        if (!esc_open_) return;
        esc_open_ = false;
        esc_menu_->set_visible(false);
        end_modal(true);
    }

    // The console can be closed from the window procedure (Esc); this notices.
    void reconcile_console() {
        if (!console_) return;
        const bool open = console_->visible();
        if (open == console_modal_) return;
        console_modal_ = open;
        g_text_input = open;
        if (open) {
            if (esc_open_) close_esc_menu();
            if (player_list_open_ && !player_list_pinned_) close_player_list(false);
            close_controls(false);
            begin_modal();
            sync_console(true);
        } else {
            end_modal(true);
        }
    }

    AmbientGpu ambient_at(const float position[3], float height) const {
        if (!gmod::ui::settings().lighting) return {};
        const gmod::source::bsp::Vec3 point{position[0],position[1],position[2]+height};
        const auto cube = gmod::ui::settings().bank_raid_lighting ? probe_.sample(point,bank_weights_) : probe_.sample(point);
        AmbientGpu gpu;
        gpu.color[0][3] = cube.sun_visible ? 1.0f : 0.0f;
        for (int face = 0; face < 6; ++face) {
            for (int channel = 0; channel < 3; ++channel) {
                gpu.color[face][channel] = cube.found ? cube.color[face][channel] : 0.6f;
            }
        }
        return gpu;
    }

    // Advances every present player's animation and uploads its bone palette.
    void update_animation(Dx12Foundation& renderer) {
        if (!animator_) return;
        for (std::size_t index = 0; index < demo_.players.size(); ++index) {
            // A knocked-out player or corpse is a ragdoll: the server networks every physics
            // bone, so the body is posed exactly as it lay.
            ragdolled_[index] = 0;
            gmod::demo::RagdollPose ragdoll;
            if (outfit_ && gmod::demo::ragdoll_pose_at(demo_, demo_.players[index], time_, ragdoll)) {
                const auto bones = outfit_->ragdoll_bones.find(ragdoll.model);
                const auto path = demo_.model_paths_by_index.find(ragdoll.model);
                if (bones != outfit_->ragdoll_bones.end()
                    && std::any_of(bones->second.begin(),bones->second.end(),[](int bone) { return bone >= 0; })
                    && path != demo_.model_paths_by_index.end()
                    && ragdoll.key->pos.size() >= 3) {
                    if (anim_debug_) *anim_debug_ << "t=" << time_ << " player=" << index << " RAGDOLL model=" << path->second << "\n";
                    const float origin[3]{ragdoll.key->pos[0], ragdoll.key->pos[1], ragdoll.key->pos[2]};
                    animator_->ragdoll_palette(bones->second, ragdoll.key->pos, ragdoll.key->ang,
                                               origin, palette_scratch_);
                    renderer.set_bone_palette(index, palette_scratch_);
                    ragdolled_[index] = 1;
                    ragdoll_origin_[index] = {origin[0], origin[1], origin[2]};
                    ragdoll_model_[index] = normalize_clothing_model_path(path->second);
                    animation_time_[index] = -1.0f;
                    continue;
                }
            }
            if (gmod::demo::player_incapacitated(demo_.players[index],time_)) {
                const int entity = gmod::demo::timed_int_at(demo_.players[index].ragdoll,time_,0);
                if (missing_ragdolls_logged_.emplace(index,entity).second)
                    std::cout << "RAGDOLL_POSE_UNAVAILABLE player=" << demo_.players[index].entity
                        << " ragdoll=" << entity << " time=" << time_
                        << " (standing body hidden until a recorded pose and mapped model are available)\n";
                animation_time_[index] = -1.0f;
                continue;
            }
            gmod::demo::PlayerPose player;
            if (!gmod::demo::sample_player(demo_.players[index], time_, player)) {
                animation_time_[index] = -1.0f;
                continue;
            }
            auto& state = animation_states_[index];
            float dt = animation_time_[index] >= 0.0f ? time_ - animation_time_[index] : 0.0f;
            // Seeks and respawns restart the smoothing instead of lerping across the
            // jump. Small negative steps are normal while playing in reverse.
            if (std::fabs(dt) > 0.5f) {
                state.valid = false;
                dt = 0.0f;
            }
            animation_time_[index] = time_;
            const float yaw = player.yaw * 0.01745329251994329577f;
            gmod::source::model::PlayerAnimator::Input input;
            input.forward = player.vx * std::cos(yaw) + player.vy * std::sin(yaw);
            input.right = player.vx * std::sin(yaw) - player.vy * std::cos(yaw);
            input.crouching = (player.flags & 2) != 0;
            // The grounded flag is only networked once it changes, so a player who is not
            // moving vertically counts as standing.
            input.grounded = (player.flags & 1) != 0 || std::fabs(player.vz) < 60.0f;
            input.seated = (player.flags & gmod::demo::PlayerFlagSeated) != 0;
            input.driver = (player.flags & gmod::demo::PlayerFlagDriver) != 0;
            // The stance comes from the weapon's networked hold type; "normal" (keys,
            // hands) and weapons that never networked one are unarmed.
            const auto held = gmod::demo::held_weapon_at(demo_, demo_.players[index], time_);
            std::string hold = held.hold_type;
            if (hold.empty() && held.class_name == "weapon_physgun") hold = "physgun";
            if (hold != "normal") input.hold = std::move(hold);
            const auto* melee_attack=gmod::demo::player_attack_at(demo_.players[index],time_);
            if(held.class_name=="roleplay_fists" && melee_attack && time_-melee_attack->time<1.0f) input.hold="fist";
            // Firing gesture after each networked shot.
            if (held.handle >= 0) {
                const auto flags = gmod::demo::weapon_flags_at(demo_, held.handle, time_);
                if (flags.reloading && flags.reloading_since >= 0.0f) {
                    input.reload_elapsed = time_ - flags.reloading_since;
                } else if (const auto* reload = gmod::demo::player_reload_at(demo_.players[index], time_)) {
                    input.reload_elapsed = time_ - reload->time;
                    input.hold_reload = false;
                }
            }
            {
                const auto& shots = demo_.players[index].shots;
                const auto after = std::upper_bound(shots.begin(), shots.end(), time_);
                if (after != shots.begin() && time_ - *(after - 1) < 2.0f) {
                    input.fire_elapsed = time_ - *(after - 1);
                }
                const auto* attack = gmod::demo::player_attack_at(demo_.players[index],time_);
                if (attack && attack->value == 0 && time_ - attack->time < 2.0f
                    && (input.fire_elapsed < 0 || time_ - attack->time < input.fire_elapsed))
                    input.fire_elapsed = time_ - attack->time;
            }
            if (const auto* grenade = grenade_state_for(held); grenade && grenade->value == 4) {
                input.hold = "grenade";
                input.fire_elapsed = time_ - grenade->time;
                input.reload_elapsed = -1.0f;
            }
            input.pitch = player.pitch;
            apply_gestures(demo_.players[index], input);
            if (anim_debug_) {
                *anim_debug_ << "t=" << time_ << " player=" << index << " flags=" << player.flags
                             << " hold=" << input.hold << " act=" << input.act << " seated=" << input.seated
                             << " fwd=" << input.forward << " right=" << input.right
                             << " hold_gesture=" << input.hold_gesture << "\n";
            }
            animator_->update(state, input, dt, palette_scratch_);
            if (anim_debug_ && !palette_scratch_.empty()) {
                for (const std::size_t bone : {std::size_t(0), std::size_t(10), palette_scratch_.size() - 1}) {
                    *anim_debug_ << "  bone " << bone << ":";
                    for (const float value : palette_scratch_[bone]) *anim_debug_ << " " << value;
                    *anim_debug_ << "\n";
                }
            }
            renderer.set_bone_palette(index, palette_scratch_);
        }
    }

    // The act the server last started on the player (an emote), as a sequence of the
    // animation model and the seconds it has been running; false when none is playing.
    // The server's sequence numbers count the player model's own sequence ("idle") first.
    bool current_act(const gmod::demo::DemoPlayer& player, int& sequence, float& elapsed,
                     bool& loop) const {
        constexpr int PlayerModelSequences = 1;
        const auto* emote = gmod::demo::emote_at(player, time_);
        if (!emote || !animator_) return false;
        sequence = emote->sequence - PlayerModelSequences;
        if (sequence < 0 || static_cast<std::size_t>(sequence) >= animator_->sequence_count()) return false;
        elapsed = time_ - emote->time;
        loop = !emote->once;
        return loop || elapsed < animator_->sequence_seconds(sequence);
    }

    // Seconds the player's current non-zero value of a networked state has held, <0 when it is 0.
    float state_held(const std::vector<gmod::demo::TimedInt>& keys) const {
        const auto it = std::upper_bound(keys.begin(), keys.end(), time_,
            [](float value, const gmod::demo::TimedInt& key) { return value < key.time; });
        if (it == keys.begin() || (it - 1)->value == 0) return -1.0f;
        return time_ - (it - 1)->time;
    }

    // The custom gestures sh_animations.lua plays: acts started by the server, and the ones
    // held while a state is true (handcuffed, phone call, phone open - first match wins).
    void apply_gestures(const gmod::demo::DemoPlayer& player,
                        gmod::source::model::PlayerAnimator::Input& input) {
        if (deploy_cuffed_ == -2) {
            deploy_cuffed_ = animator_->activity_sequence("ACT_VM_DEPLOY_7");
            deploy_phone_call_ = animator_->activity_sequence("ACT_VM_DEPLOY_4");
            deploy_phone_open_ = animator_->activity_sequence("ACT_VM_DEPLOY_5");
        }
        float elapsed = 0.0f;
        bool loop = false;
        int sequence = -1;
        if (current_act(player, sequence, elapsed, loop)) {
            input.act = sequence;
            input.act_elapsed = elapsed;
            input.act_loop = loop;
        }
        struct Held { const std::vector<gmod::demo::TimedInt>* keys; int sequence; };
        for (const Held held : {Held{&player.restrained, deploy_cuffed_},
                                Held{&player.phone_call, deploy_phone_call_},
                                Held{&player.phone_open, deploy_phone_open_}}) {
            const float since = state_held(*held.keys);
            if (since < 0.0f || held.sequence < 0) continue;
            input.hold_gesture = held.sequence;
            input.hold_weight = std::clamp(since * 6.0f, 0.0f, 1.0f);
            break;
        }
    }

    // A condition shown under the name: restrained or surrendering.
    std::wstring condition_status(const gmod::demo::DemoPlayer& player) const {
        switch (gmod::demo::timed_int_at(player.restrained, time_, 0)) {
        case 1: return L"HANDCUFFED";
        case 2: return L"ZIP-TIED";
        default: break;
        }
        float elapsed = 0.0f;
        bool loop = false;
        int sequence = -1;
        if (current_act(player, sequence, elapsed, loop)
            && animator_->sequence_name(sequence) == "ph_act_surrender") {
            return L"SURRENDERING";
        }
        return {};
    }

    // Script facts of a weapon class (by its first-person model), null when unknown.
    const gmod::demo::SwepInfo* swep_info_for(const std::string& weapon_class) const {
        if (!outfit_ || weapon_class.empty()) return nullptr;
        const auto view = demo_.weapon_view_models.find(weapon_class);
        if (view == demo_.weapon_view_models.end()) return nullptr;
        const auto info = outfit_->registry.swep_info.find(normalize_clothing_model_path(view->second));
        return info == outfit_->registry.swep_info.end() ? nullptr : &info->second;
    }

    const gmod::demo::TimedInt* grenade_state_for(const gmod::demo::HeldWeapon& held) const {
        if (!outfit_) return nullptr;
        const auto path = demo_.weapon_view_models.find(held.class_name);
        if (path == demo_.weapon_view_models.end()) return nullptr;
        const auto id = outfit_->view_model_ids.find(normalize_clothing_model_path(path->second));
        if (id == outfit_->view_model_ids.end()) return nullptr;
        const auto& asset = outfit_->view_models[static_cast<std::size_t>(id->second)];
        if (asset.pin < 0 || asset.toss < 0) return nullptr;
        return gmod::demo::grenade_state_at(demo_, held.handle, time_);
    }

    // The first-person view model of the followed player. For the recorder it plays the
    // networked view model sequence; other players' view models are not in the demo, so
    // their held weapon's model idles. The arms are the player's own forearm, hand and
    // finger clothing bonemerged onto it (cl_viewmodel.lua).
    void update_view_model(Dx12Foundation& renderer,
                           const std::optional<Dx12Foundation::FirstPersonView>& view) {
        vm_id_ = -1;
        vm_player_ = -1;
        if (!outfit_ || !animator_ || outfit_->view_models.empty() || !view || !first_person_
            || free_view_ || follow_ < 0) {
            return;
        }
        const auto player_index = static_cast<std::size_t>(follow_);
        const auto& player = demo_.players[player_index];
        gmod::demo::PlayerPose body;
        if (gmod::demo::player_incapacitated(player,time_)
            || !gmod::demo::sample_player(player, time_, body)
            || (body.flags & gmod::demo::PlayerFlagSeated) != 0) {
            return;
        }
        const auto held = gmod::demo::held_weapon_at(demo_, player, time_);
        int id = -1;
        int sequence = -1;
        float cycle = 0.0f;
        const auto lookup = [&](const std::string& path) {
            const auto found = outfit_->view_model_ids.find(normalize_clothing_model_path(path));
            return found == outfit_->view_model_ids.end() ? -1 : found->second;
        };
        if (follow_ == demo_.recorder_player && !demo_.view_model.empty()) {
            const auto* key = gmod::demo::view_model_at(demo_, time_);
            if (!key || key->model <= 0 || (key->effects & 32) != 0) return;
            const auto path = demo_.model_paths_by_index.find(key->model);
            if (path == demo_.model_paths_by_index.end()) return;
            id = lookup(path->second);
            if (id < 0) return;
            const auto& animation = *outfit_->view_models[static_cast<std::size_t>(id)].animation;
            sequence = key->sequence;
            if (sequence >= 0 && sequence < static_cast<int>(animation.sequence_count())) {
                const std::vector<float> parameters(animation.pose_parameter_count(), 0.0f);
                const float length = std::max(animation.duration(sequence, parameters), 0.01f);
                if (animation.is_looping(sequence)) {
                    // The parity counter ticks constantly while idling; it is not a restart.
                    const float played = time_ * key->rate / length;
                    cycle = played - std::floor(played);
                } else {
                    // A one-shot (fire, draw, reload) restarts whenever its parity flips.
                    const auto* start = key;
                    while (start != demo_.view_model.data()) {
                        const auto* before = start - 1;
                        if (before->sequence != key->sequence || before->new_sequence != key->new_sequence
                            || before->model != key->model || before->animation_parity != key->animation_parity) {
                            break;
                        }
                        start = before;
                    }
                    cycle = std::clamp((time_ - start->time) * key->rate / length, 0.0f, 1.0f);
                }
            }
        } else {
            // Other players' view models are not in the demo: their weapon plays what its
            // own networked events imply - the draw after it was pulled out, the firing
            // sequence after each shot (the aimed one while the sights are up), else idle.
            if (held.class_name.empty()) return;
            const auto view_path = demo_.weapon_view_models.find(held.class_name);
            if (view_path == demo_.weapon_view_models.end()) return;
            id = lookup(view_path->second);
            if (id < 0) return;
            const auto& asset_ref = outfit_->view_models[static_cast<std::size_t>(id)];
            const auto& animation = *asset_ref.animation;
            const std::vector<float> parameters(animation.pose_parameter_count(), 0.0f);
            const auto length_of = [&](int candidate) {
                // Real playback length: the sequence's own length at the rate the server
                // played it (the fists' draw runs at 5x).
                const auto rate = asset_ref.sequence_rate.find(candidate);
                return std::max(animation.duration(candidate, parameters)
                                    / (rate == asset_ref.sequence_rate.end() ? 1.0f : rate->second), 0.01f);
            };
            const auto flags = held.handle >= 0 ? gmod::demo::weapon_flags_at(demo_, held.handle, time_)
                                                : gmod::demo::WeaponFlags{};
            // Time since this weapon was last shot, and since it became the active weapon.
            float since_shot = -1.0f;
            const auto& shots = player.shots;
            const auto after = std::upper_bound(shots.begin(), shots.end(), time_);
            if (after != shots.begin()) since_shot = time_ - *(after - 1);
            const auto* attack = gmod::demo::player_attack_at(player,time_);
            const bool attack_drives = attack && (since_shot < 0 || time_ - attack->time <= since_shot);
            if (attack_drives)
                since_shot = time_ - attack->time;
            float since_draw = -1.0f;
            const auto& actives = player.active_weapon;
            const auto current = std::upper_bound(actives.begin(), actives.end(), time_,
                [](float value, const gmod::demo::ActiveWeaponKey& key) { return value < key.time; });
            if (current != actives.begin()) since_draw = time_ - (current - 1)->time;
            const int fire = attack_drives && attack->value == 1 && asset_ref.fire_secondary >= 0
                ? asset_ref.fire_secondary : flags.aiming ? asset_ref.fire_aimed : asset_ref.fire;
            const auto* reload = gmod::demo::player_reload_at(player, time_);
            const float reload_since = flags.reloading ? flags.reloading_since : reload ? reload->time : -1.0f;
            float reload_length = asset_ref.reload >= 0 ? length_of(asset_ref.reload) : 0.0f;
            for (const auto step : asset_ref.reload_chain) reload_length += length_of(step);
            const auto* grenade = grenade_state_for(held);
            if (grenade && grenade->value >= 2 && grenade->value <= 4) {
                const int action = grenade->value == 2 ? asset_ref.pin
                    : grenade->value == 3 ? asset_ref.spoon : asset_ref.toss;
                const int idle = grenade->value == 2 ? asset_ref.idle_nopin : asset_ref.idle_nospoon;
                const float elapsed = time_ - grenade->time;
                if (action >= 0 && (elapsed < length_of(action) || idle < 0 || grenade->value == 4)) {
                    sequence = action;
                    cycle = std::clamp(elapsed / length_of(action), 0.0f, 1.0f);
                } else {
                    sequence = idle >= 0 ? idle : asset_ref.idle;
                    if (sequence >= 0) cycle = std::fmod(elapsed / length_of(sequence), 1.0f);
                }
                if (g_headless) std::cout << "GRENADE_ANIMATION player=" << follow_
                    << " state=" << grenade->value << " since=" << grenade->time
                    << " sequence=" << sequence << " cycle=" << cycle << "\n";
            } else if (reload_since >= 0.0f
                && (asset_ref.reload >= 0 || !asset_ref.reload_chain.empty())
                && (flags.reloading || time_ - reload_since < reload_length)
                && (since_shot < 0.0f || since_shot > time_ - reload_since)) {
                // Reloading is a networked state: play the reload from when it began (a
                // multi-part reload steps through start, each round, end), holding the last
                // frame until the state ends.
                float remaining = time_ - reload_since;
                if (asset_ref.reload >= 0) {
                    sequence = asset_ref.reload;
                    cycle = std::clamp(remaining / length_of(sequence), 0.0f, 1.0f);
                } else {
                    sequence = asset_ref.reload_chain.back();
                    cycle = 1.0f;
                    for (const int step : asset_ref.reload_chain) {
                        const float length = length_of(step);
                        if (remaining < length) {
                            sequence = step;
                            cycle = remaining / length;
                            break;
                        }
                        remaining -= length;
                    }
                }
            } else if (fire >= 0 && since_shot >= 0.0f && since_shot < length_of(fire)) {
                sequence = fire;
                cycle = since_shot / length_of(fire);
            } else if (asset_ref.draw >= 0 && since_draw >= 0.0f && since_draw < length_of(asset_ref.draw)) {
                sequence = asset_ref.draw;
                cycle = since_draw / length_of(asset_ref.draw);
            } else {
                sequence = asset_ref.idle;
                if (sequence >= 0) {
                    const float length = length_of(sequence);
                    cycle = time_ / length - std::floor(time_ / length);
                }
            }
        }
        const auto& asset = outfit_->view_models[static_cast<std::size_t>(id)];
        if (sequence < 0 || sequence >= static_cast<int>(asset.animation->sequence_count())) sequence = 0;
        if (asset.animation->sequence_count() == 0) return;
        std::vector<gmod::source::model::BonePose> pose;
        try {
            const std::vector<float> parameters(asset.animation->pose_parameter_count(), 0.0f);
            asset.animation->sample(sequence, parameters, cycle, pose);
        } catch (const std::exception&) {
            return;
        }
        asset.poser->palette(pose, vm_palette_);
        renderer.set_bone_palette(demo_.players.size(), vm_palette_);
        // Arms: bones the view model has follow it exactly (bonemerge); the rest are
        // collapsed behind the eye like cl_viewmodel.lua's zero-scaled bones.
        const auto& skeleton = animator_->skeleton();
        const std::array<float, 12> collapsed{0, 0, 0, -60.0f, 0, 0, 0, 0.0f, 0, 0, 0, -60.0f};
        arm_palette_.assign(skeleton.size(), collapsed);
        for (std::size_t bone = 0; bone < skeleton.size() && bone < asset.arm_bone.size(); ++bone) {
            const int source = asset.arm_bone[bone];
            if (source < 0 || static_cast<std::size_t>(source) >= vm_palette_.size()) continue;
            const auto world = rigid_compose(vm_palette_[static_cast<std::size_t>(source)],
                                             asset.bind[static_cast<std::size_t>(source)]);
            std::array<float, 12> to_bone{};
            std::copy(skeleton[bone].pose_to_bone, skeleton[bone].pose_to_bone + 12, to_bone.begin());
            arm_palette_[bone] = rigid_compose(world, to_bone);
        }
        renderer.set_bone_palette(demo_.players.size() + 1, arm_palette_);

        // Placement: the eye, turned and shifted by SWEP:GetViewModelPosition (iron sights or
        // the passive pose, both blending in over 0.3 s).
        constexpr float ToRadians = 0.01745329251994329577f;
        const float pitch = view->pitch_degrees * ToRadians;
        const float yaw = view->yaw_degrees * ToRadians;
        std::array<float, 3> forward{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw),
                                     -std::sin(pitch)};
        std::array<float, 3> right{std::sin(yaw), -std::cos(yaw), 0.0f};
        std::array<float, 3> up{std::sin(pitch) * std::cos(yaw), std::sin(pitch) * std::sin(yaw),
                                std::cos(pitch)};
        std::array<float, 3> origin{view->x, view->y, view->z};
        // The camera's own axes, kept before the sights turn the model (the field-of-view
        // scaling below happens in the camera's frame).
        const std::array<float, 3> camera_forward = forward;
        const std::array<float, 3> camera_right = right;
        const std::array<float, 3> camera_up = up;
        const auto* info = swep_info_for(held.class_name);
        if (info && info->gun && held.handle >= 0) {
            const auto flags = gmod::demo::weapon_flags_at(demo_, held.handle, time_);
            static constexpr float RunPos[3]{4.0f, 2.0f, 0.0f};
            static constexpr float RunAng[3]{-35.0f, 50.0f, -35.0f};
            float amount = 0.0f;
            float optic_pos[3]{};
            float optic_ang[3]{};
            const float* offset_pos = nullptr;
            const float* offset_ang = nullptr;
            if (flags.passive) {
                amount = 0.5f;
                offset_pos = RunPos;
                offset_ang = RunAng;
            } else if (flags.aiming) {
                amount = std::clamp((time_ - flags.aiming_since) / 0.3f, 0.0f, 1.0f);
                offset_pos = info->iron_pos;
                offset_ang = info->iron_ang;
                if (const auto* sight = fitted_sight(held.handle)) {
                    for (int axis = 0; axis < 3; ++axis) {
                        optic_pos[axis] = (info->rail_pos ? (*info->rail_pos)[axis] : info->iron_pos[axis])
                            -(*sight->iron_pos)[axis];
                        optic_ang[axis] = (info->rail_pos ? info->rail_ang[axis] : info->iron_ang[axis])
                            -sight->iron_ang[axis];
                    }
                    offset_pos = optic_pos;
                    offset_ang = optic_ang;
                }
            }
            if (amount > 0.0f) {
                // Angle:RotateAroundAxis, each step about the already-turned axes.
                const auto rotate = [](std::array<float, 3>& v, const std::array<float, 3>& axis, float degrees) {
                    const float a = degrees * 0.01745329251994329577f;
                    const float c = std::cos(a), s = std::sin(a);
                    const float d = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
                    const std::array<float, 3> cross{axis[1] * v[2] - axis[2] * v[1],
                                                     axis[2] * v[0] - axis[0] * v[2],
                                                     axis[0] * v[1] - axis[1] * v[0]};
                    for (int i = 0; i < 3; ++i) v[i] = v[i] * c + cross[i] * s + axis[i] * d * (1.0f - c);
                };
                const auto turn = [&](const std::array<float, 3> axis, float degrees) {
                    rotate(forward, axis, degrees);
                    rotate(right, axis, degrees);
                    rotate(up, axis, degrees);
                };
                turn(right, offset_ang[0] * amount);
                turn(up, offset_ang[1] * amount);
                turn(forward, offset_ang[2] * amount);
                for (int i = 0; i < 3; ++i) {
                    origin[i] += right[i] * offset_pos[0] * amount + forward[i] * offset_pos[1] * amount
                        + up[i] * offset_pos[2] * amount;
                }
            }
        }
        // The view model is drawn with its own field of view (SWEP.ViewModelFOV, a 4:3
        // horizontal angle); scaling it across the view axis gives the same projection.
        const float view_model_fov = (info ? info->fov : 62.0f) * ToRadians;
        const float scale = std::tan(1.05f * 0.5f) / (std::tan(view_model_fov * 0.5f) * 0.75f);
        // A different field of view is a scaling of everything across the view axis, about the
        // eye. It has to act on where the model sits relative to the eye as well as on the
        // model itself, or the sights (placed on the view axis) drift off centre.
        const auto dot3 = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        };
        const std::array<float, 3> camera_left{-camera_right[0], -camera_right[1], -camera_right[2]};
        const auto squeeze = [&](const std::array<float, 3>& v) {
            const float along = dot3(camera_forward, v);
            const float across_left = dot3(camera_left, v) * scale;
            const float across_up = dot3(camera_up, v) * scale;
            std::array<float, 3> out;
            for (int i = 0; i < 3; ++i) {
                out[i] = camera_forward[i] * along + camera_left[i] * across_left + camera_up[i] * across_up;
            }
            return out;
        };
        const std::array<float, 3> left{-right[0], -right[1], -right[2]};
        const std::array<float, 3> from_eye{origin[0] - view->x, origin[1] - view->y, origin[2] - view->z};
        const auto placed = squeeze(from_eye);
        const std::array<float, 3> shifted{view->x + placed[0], view->y + placed[1], view->z + placed[2]};
        Matrix4 transform{};
        const auto put = [&](int row, const std::array<float, 3>& v) {
            transform.values[row * 4] = -v[0];
            transform.values[row * 4 + 1] = v[2];
            transform.values[row * 4 + 2] = -v[1];
        };
        put(0, squeeze(forward));
        put(1, squeeze(left));
        put(2, squeeze(up));
        transform.values[12] = -shifted[0];
        transform.values[13] = shifted[2];
        transform.values[14] = -shifted[1];
        transform.values[15] = 1.0f;
        vm_transform_ = transform;
        const float eye[3]{view->x, view->y, view->z};
        vm_ambient_ = ambient_at(eye, 0.0f);
        vm_id_ = id;
        vm_player_ = follow_;
        if(g_headless) std::cout << "VIEWMODEL_POSE player=" << follow_ << " model=" << asset.path
            << " sequence=" << sequence << " name=" << asset.animation->sequence_name(sequence) << " cycle=" << cycle << "\n";
    }

    // A vehicle's skinning palette from the pose parameters the server networks: steering,
    // wheel spin and suspension height. Values are interpolated between network updates;
    // wheel spin wraps, so it blends the short way round.
    void update_vehicle_palette(Dx12Foundation& renderer, const ActorBinding& actor,
                                const gmod::demo::DemoProp& prop) {
        if (!outfit_ || static_cast<std::size_t>(actor.model) >= outfit_->vehicle_assets.size()) return;
        const auto& asset = outfit_->vehicle_assets[static_cast<std::size_t>(actor.model)];
        const auto& animation = *asset.animation;
        std::vector<float> parameters(animation.pose_parameter_count(), 0.0f);
        std::array<float, gmod::demo::VehiclePoseCount> before{}, after{};
        float fraction = 0.0f;
        const bool networked = gmod::demo::sample_vehicle_params(prop, time_, before, after, fraction);
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            float value = 0.5f;  // wheels at mid suspension travel when nothing was networked
            if (networked && index < before.size()) {
                value = before[index];
                float delta = after[index] - before[index];
                if (animation.pose_parameter_loops(index)) {
                    if (delta > 0.5f) delta -= 1.0f;
                    if (delta < -0.5f) delta += 1.0f;
                }
                value += delta * fraction;
                if (animation.pose_parameter_loops(index)) value -= std::floor(value);
            }
            parameters[index] = animation.pose_parameter_raw(index, value);
        }
        std::vector<gmod::source::model::BonePose> pose;
        try {
            animation.sample_with_layers(0, parameters, 0.0f, pose);
        } catch (const std::exception&) {
            return;
        }
        asset.poser->palette(pose, vehicle_palette_);
        renderer.set_bone_palette(static_cast<std::size_t>(actor.palette), vehicle_palette_);
    }

    void update_bank_shutter_palette(Dx12Foundation& renderer, const ActorBinding& actor,
                            const gmod::demo::DemoProp& prop) {
        if (!outfit_ || actor.model<0 || actor.palette<0
            || static_cast<std::size_t>(actor.model)>=outfit_->vehicle_assets.size()) return;
        const auto& asset=outfit_->vehicle_assets[actor.model];
        const auto& animation=*asset.animation;
        const auto* recorded=gmod::demo::bank_shutter_animation_at(prop,time_);
        if (!recorded) return;
        const auto& key=*recorded;
        if (key.sequence<0 || static_cast<std::size_t>(key.sequence)>=animation.sequence_count()) return;
        std::vector<float> parameters(animation.pose_parameter_count(),0.0f);
        float cycle=gmod::demo::bank_shutter_cycle(key,time_,animation.duration(key.sequence,parameters),
            animation.is_looping(key.sequence));
        const auto& sequence_name=animation.sequence_name(key.sequence);
        if(sequence_name.starts_with("idle") || sequence_name.ends_with("idle")) cycle=0;
        const std::pair<int,float> state{key.sequence,cycle};
        if(const auto cached=bank_shutter_palettes_.find(actor.palette);
            cached!=bank_shutter_palettes_.end() && cached->second==state) return;
        std::vector<gmod::source::model::BonePose> pose;
        try { animation.sample_with_layers(key.sequence,parameters,cycle,pose); }
        catch (const std::exception&) { return; }
        asset.poser->palette(pose,vehicle_palette_);
        renderer.set_bone_palette(static_cast<std::size_t>(actor.palette),vehicle_palette_);
        bank_shutter_palettes_[actor.palette]=state;
        ++bank_shutter_palette_updates_;
    }

    // The server's clothing data for the player at the current demo time; before the
    // first record the earliest known outfit applies.
    bool outfit_part_worn(std::size_t player, int part) const {
        if (!outfit_ || player >= outfit_->by_player.size()) return false;
        const auto& keys = outfit_->by_player[player];
        if (keys.empty()) return false;
        auto it = std::upper_bound(
            keys.begin(), keys.end(), time_,
            [](float time, const OutfitData::Key& key) { return time < key.time; });
        if (it != keys.begin()) --it;
        return std::find(it->parts.begin(), it->parts.end(), part) != it->parts.end();
    }

    void apply(Dx12Foundation& renderer) {
        const auto& options=gmod::ui::settings();
        const auto weights=options.bank_raid_lighting
            ? gmod::demo::bank_lighting_at(demo_,time_,renderer.bank_styles(),renderer.initial_bank_weights())
            : renderer.initial_bank_weights();
        if (options.lighting!=lighting_enabled_ || options.bank_raid_lighting!=bank_lighting_enabled_
            || weights!=bank_weights_) {
            lighting_enabled_=options.lighting;
            bank_lighting_enabled_=options.bank_raid_lighting;
            bank_weights_=weights;
            for (auto& cached:prop_ambient_) cached.reset();
            for (auto& cached:door_ambient_) cached.reset();
        }
        renderer.set_bank_lighting(weights);
        rendered_arm_parts_=0;
        rendered_plate_poses_.clear();
        rendered_badge_parts_.clear();
        rendered_back_guns_.clear();
        active_muzzles_.clear();
        update_vehicle_view();
        update_animation(renderer);
        update_scope(renderer);
        const std::optional<Dx12Foundation::FirstPersonView> view =
            free_view_ ? std::nullopt : first_person_view();
        update_view_model(renderer, view);
        const bool hide_followed = first_person_ && follow_ >= 0 && !free_view_;
        std::vector<int> visible;
        std::optional<std::array<float, 3>> follow_position;
        tag_candidates_.clear();
        // Outline colour per player, packed as 0xRRGGBB + 1 (0 = not outlined): health from green
        // (full) through yellow to red. Health the demo does not carry counts as full, or as
        // none while the player is a ragdoll (knocked out or dead).
        const bool outline_all = gmod::ui::settings().outline_all_players;
        std::vector<int> outline_cache(demo_.players.size(), -1);
        const auto outline_color = [&](std::size_t player) -> float {
            if (player >= demo_.players.size()) return 0.0f;
            if (outline_cache[player] >= 0) return static_cast<float>(outline_cache[player]);
            int packed = 0;
            const auto& data = demo_.players[player];
            if (!data.npc && outline_all) {
                const auto* info = gmod::demo::player_info_at(data, time_);
                const bool known = info && info->health >= 0;
                const float health = known ? std::clamp(info->health / 100.0f, 0.0f, 1.0f)
                    : (player < ragdolled_.size() && ragdolled_[player]) ? 0.0f : 1.0f;
                const int red = static_cast<int>(std::lround(255.0f * std::min(1.0f, 2.0f - 2.0f * health)));
                const int green = static_cast<int>(std::lround(255.0f * std::min(1.0f, 2.0f * health)));
                packed = (red << 16) + (green << 8) + 1;
            }
            outline_cache[player] = packed;
            return static_cast<float>(packed);
        };
        bool any_outline = false;
        for (std::size_t slot = 0; slot < actors_.size(); ++slot) {
            const auto& actor = actors_[slot];
            DynamicPose pose;
            if (actor.kind == 1) {
                gmod::demo::PlayerPose player;
                const auto player_index = static_cast<std::size_t>(actor.index);
                if (ragdolled_[player_index]) {
                    // The ragdoll's bones carry absolute positions; the body sits at its pelvis.
                    if (actor.model >= 0 && static_cast<std::size_t>(actor.model) < demo_.models.size()
                        && normalize_clothing_model_path(
                               demo_.models[static_cast<std::size_t>(actor.model)])
                            == ragdoll_model_[player_index]) {
                        const auto& at = ragdoll_origin_[player_index];
                        const float origin[3]{at[0], at[1], at[2]};
                        const float angles[3]{0.0f, 0.0f, 0.0f};
                        pose.active = !(hide_followed && actor.index == follow_);
                        pose.transform = prop_transform(origin, angles);
                        pose.ambient = ambient_at(origin, 20.0f);
                        visible.push_back(actor.index);
                        TagCandidate candidate;
                        candidate.player = actor.index;
                        candidate.x = origin[0];
                        candidate.y = origin[1];
                        candidate.z = origin[2];
                        candidate.head = 40.0f;
                        tag_candidates_.push_back(candidate);
                        if (actor.index == follow_) {
                            follow_position = std::array<float, 3>{origin[0], origin[1], origin[2]};
                        }
                    }
                } else if (!gmod::demo::player_incapacitated(demo_.players[player_index],time_)
                    && gmod::demo::sample_player(demo_.players[player_index], time_, player)
                    && (player.model >= 0 ? player.model : fallback_model_) == actor.model) {
                    const float origin[3]{player.x, player.y, player.z};
                    const float angles[3]{0.0f, player.yaw, 0.0f};
                    // Seen through their own eyes, the followed body would fill the view.
                    pose.active = !(hide_followed && actor.index == follow_);
                    pose.transform = prop_transform(origin, angles);
                    pose.ambient = ambient_at(origin, 36.0f);
                    visible.push_back(actor.index);
                    TagCandidate candidate;
                    candidate.player = actor.index;
                    candidate.x = player.x;
                    candidate.y = player.y;
                    candidate.z = player.z;
                    candidate.head = (player.flags & gmod::demo::PlayerFlagSeated) != 0 ? 56.0f
                        : (player.flags & 2) != 0 ? 54.0f : 78.0f;
                    tag_candidates_.push_back(candidate);
                    if (actor.index == follow_) {
                        follow_position = std::array<float, 3>{origin[0], origin[1], origin[2]};
                    }
                }
            } else if (actor.kind == 12) {
                const auto& gun=demo_.props.at(static_cast<std::size_t>(actor.index));
                const int player_index=gmod::demo::back_gun_player_at(demo_,gun,time_,hide_followed?follow_:-1);
                gmod::demo::PlayerPose player;
                if(player_index>=0 && outfit_ && gmod::demo::sample_player(demo_.players[player_index],time_,player)) {
                    const int model=player.model>=0?player.model:fallback_model_;
                    const auto chest=outfit_->chest_attachments.find(model);
                    if(chest!=outfit_->chest_attachments.end()) {
                        const float origin[3]{player.x,player.y,player.z},angles[3]{0,player.yaw,0};
                        const auto body=gmod::source::model::matrix_from_source_angles(origin,angles);
                        const auto animated=renderer.skinned_transform(static_cast<std::size_t>(player_index),chest->second.bone,chest->second.bind);
                        const auto world=gmod::demo::back_gun_transform(rigid_compose(body,animated));
                        pose.active=true;pose.transform=prop_transform(world);pose.ambient=ambient_at(origin,36);
                        rendered_back_guns_.push_back(gun.entity);
                        pose.ambient.color[5][3]=outline_color(static_cast<std::size_t>(player_index));
                        any_outline=any_outline || pose.ambient.color[5][3]>0.5f;
                        if(g_headless) std::cout << "BACK_GUN entity=" << gun.entity << " player=" << demo_.players[player_index].name
                            << " time=" << time_ << " position=" << world[3] << "," << world[7] << "," << world[11] << "\n";
                    }
                }
            } else if (actor.kind == 13) {
                const auto& vehicle = demo_.props.at(static_cast<std::size_t>(actor.index));
                const auto& plate = outfit_->plates.at(static_cast<std::size_t>(actor.entry));
                gmod::demo::PropPose prop;
                if (!hide_vehicles_ && gmod::demo::vehicle_plate_at(vehicle, time_) == plate.state
                    && gmod::demo::timed_int_at(vehicle.hidden, time_, 0) == 0
                    && gmod::demo::sample_prop(vehicle, time_, prop)) {
                    pose.active = true;
                    const auto world = gmod::demo::plate_transform(prop, plate.placement);
                    pose.transform = prop_transform(world);
                    if (g_headless) rendered_plate_poses_.emplace(static_cast<int>(slot), world);
                    const float origin[]{world[3], world[7], world[11]};
                    pose.ambient = ambient_at(origin, 0);
                    for (int channel = 0; channel < 3; ++channel)
                        pose.ambient.color[channel + 1][3] = plate.color[channel];
                    pose.ambient.color[4][3] = 1;
                    if (g_headless)
                        std::cout << "LICENSE_PLATE entity=" << vehicle.entity
                            << " registration=" << plate.state.registration << " time=" << time_
                            << " position=" << world[3] << "," << world[7] << "," << world[11] << "\n";
                }
            } else if (actor.kind == 2) {
                gmod::demo::PropPose prop;
                const auto& demo_prop = demo_.props[static_cast<std::size_t>(actor.index)];
                if (!(hide_vehicles_ && demo_prop.vehicle)
                    && gmod::demo::sample_prop(demo_prop, time_, prop)) {
                    const float origin[3]{prop.x, prop.y, prop.z};
                    const float angles[3]{prop.pitch, prop.yaw, prop.roll};
                    pose.active = true;
                    pose.transform = prop_transform(origin, angles);
                    if (demo_prop.vehicle) {
                        pose.ambient = ambient_at(origin, 24.0f);
                        if (actor.palette >= 0 && actor.model >= 0) update_vehicle_palette(renderer, actor, demo_prop);
                        // Only shared paint materials use this tint; livery materials are unflagged.
                        float rgb[3];
                        gmod::demo::sample_vehicle_color(demo_prop, time_, rgb);
                        for (int channel = 0; channel < 3; ++channel) {
                            pose.ambient.color[channel + 1][3] = rgb[channel];
                        }
                        pose.ambient.color[4][3] = 1.0f;
                    } else {
                        // Placed props mostly sit still: their light is only looked up again
                        // when one has been moved.
                        auto& cached = prop_ambient_[slot];
                        if (!cached || std::fabs(cached->origin[0] - origin[0]) > 8.0f
                            || std::fabs(cached->origin[1] - origin[1]) > 8.0f
                            || std::fabs(cached->origin[2] - origin[2]) > 8.0f) {
                            cached = CachedAmbient{{origin[0], origin[1], origin[2]},
                                                   ambient_at(origin, 24.0f)};
                        }
                        pose.ambient = cached->ambient;
                    }
                }
            } else if (actor.kind == 8 || actor.kind == 9) {
                if (actor.index < 0) {
                    renderer.set_dynamic_pose(slot, pose);
                    continue;
                }
                const auto index = static_cast<std::size_t>(actor.index);
                const auto& prop = demo_.props[index];
                pose.active = gmod::demo::timed_int_at(prop.hidden,time_,actor.initial_hidden ? 1 : 0) == 0;
                const auto recorded = recorded_prop_matrix(demo_,index,time_);
                if (!recorded && pose.active && !prop.keys.empty() && time_ >= prop.keys.front().time
                    && missing_brush_poses_logged_.insert(index).second)
                    std::cout << "MAP_POSE_UNAVAILABLE entity=" << prop.entity << " time=" << time_
                        << " (retaining authored map placement; parent pose unavailable)\n";
                const auto matrix = recorded.value_or(
                    gmod::source::model::matrix_from_source_angles(actor.origin,actor.angles));
                pose.transform = prop_transform(matrix);
                const float origin[3]{matrix[3],matrix[7],matrix[11]};
                pose.ambient = ambient_at(origin,0.0f);
                if(actor.palette>=0 && !prop.animations.empty()) update_bank_shutter_palette(renderer,actor,prop);
            } else if (actor.kind == 5 || actor.kind == 10) {
                gmod::demo::PlayerPose player;
                const auto player_index = static_cast<std::size_t>(actor.index);
                // A ragdoll has dropped whatever it held.
                if (!gmod::demo::player_incapacitated(demo_.players[player_index],time_)
                    && gmod::demo::sample_player(demo_.players[player_index], time_, player)
                    && (player.flags & gmod::demo::PlayerFlagSeated) == 0
                    && !(hide_followed && actor.index == follow_)) {
                    const auto held = gmod::demo::held_weapon_at(
                        demo_, demo_.players[player_index], time_);
                    if (held.class_name == actor.weapon
                        && !(held.class_name == "roleplay_keys" && held.hold_type == "normal")) {
                        bool known = false;
                        const auto attachments = actor.kind == 10
                            ? gmod::demo::weapon_attachments_at(demo_,held.handle,time_,known) : std::vector<int>{};
                        if (actor.kind == 10 && std::find(attachments.begin(),attachments.end(),actor.attachment)
                            == attachments.end()) {
                            renderer.set_dynamic_pose(slot,pose);
                            continue;
                        }
                        if (actor.kind == 10 && actor.attachment_sequence !=
                            gmod::demo::weapon_attachment_sequence_at(
                                outfit_->registry, demo_, actor.attachment, held.handle, time_)) {
                            renderer.set_dynamic_pose(slot,pose);
                            continue;
                        }
                        const float origin[3]{player.x, player.y, player.z};
                        const float angles[3]{0.0f, player.yaw, 0.0f};
                        pose.active = true;
                        pose.transform = prop_transform(origin, angles);
                        pose.ambient = ambient_at(origin, 36.0f);
                    }
                }
            } else if (actor.kind == 3 && gmod::demo::door_hidden_at(
                           demo_.doors[static_cast<std::size_t>(actor.index)], time_)) {
                // Blown off its frame (C2): the server stops drawing it.
                pose.active = false;
            } else if (actor.kind == 3) {
                const float openness = use_doors_
                    ? gmod::demo::door_openness(
                          demo_.doors[static_cast<std::size_t>(actor.index)], time_,
                          actor.door_distance / std::max(actor.door_speed, 1.0f))
                    : 0.0f;
                // The server networks the door's real rotation, which says which way it swung
                // (it depends on who opened it). Without one, "open forward" swings the leaf in
                // the positive yaw direction.
                const float sign = actor.open_dir == 2 ? -1.0f : 1.0f;
                float yaw = actor.angles[1] + sign * actor.door_distance * openness;
                std::array<float, 3> angles{actor.angles[0], yaw, actor.angles[2]};
                if (use_doors_) {
                    gmod::demo::door_angles_at(demo_.doors[static_cast<std::size_t>(actor.index)], time_, angles);
                }
                pose.active = true;
                pose.transform = prop_transform(actor.origin, angles.data());
                if (!door_ambient_[slot]) {
                    door_ambient_[slot] = ambient_at(actor.origin, 50.0f);
                }
                pose.ambient = *door_ambient_[slot];
            } else if (actor.kind == 6 || actor.kind == 11) {
                if (vm_id_ >= 0 && actor.entry == vm_id_) {
                    if (actor.kind == 11) {
                        const auto held = gmod::demo::held_weapon_at(demo_,demo_.players[follow_],time_);
                        bool known = false;
                        const auto attachments = gmod::demo::weapon_attachments_at(demo_,held.handle,time_,known);
                        if (std::find(attachments.begin(),attachments.end(),actor.attachment) == attachments.end()) {
                            renderer.set_dynamic_pose(slot,pose);
                            continue;
                        }
                        if (actor.attachment_sequence != gmod::demo::weapon_attachment_sequence_at(
                                outfit_->registry, demo_, actor.attachment, held.handle, time_)) {
                            renderer.set_dynamic_pose(slot,pose);
                            continue;
                        }
                    }
                    pose.active = true;
                    pose.transform = vm_transform_;
                    pose.ambient = vm_ambient_;
                }
            } else if (actor.kind == 7) {
                const auto player_index = static_cast<std::size_t>(actor.index);
                if (vm_id_ >= 0 && actor.index == vm_player_ && outfit_part_worn(player_index, actor.entry)) {
                    ++rendered_arm_parts_;
                    pose.active = true;
                    pose.transform = vm_transform_;
                    pose.ambient = vm_ambient_;
                    const auto& part = outfit_->parts[static_cast<std::size_t>(actor.entry)];
                    if (part.tint) {
                        for (int channel = 0; channel < 3; ++channel) {
                            pose.ambient.color[channel + 1][3] = part.color[channel] / 255.0f;
                        }
                        pose.ambient.color[4][3] = 1.0f;
                    }
                }
            } else if (actor.kind == 4) {
                gmod::demo::PlayerPose player;
                const auto player_index = static_cast<std::size_t>(actor.index);
                if (player_index < demo_.players.size() && ragdolled_[player_index]
                    && outfit_part_worn(player_index, actor.entry)) {
                    const auto& at = ragdoll_origin_[player_index];
                    const float origin[3]{at[0], at[1], at[2]};
                    const float angles[3]{0.0f, 0.0f, 0.0f};
                    pose.active = !(hide_followed && player_index == static_cast<std::size_t>(follow_));
                    pose.transform = prop_transform(origin, angles);
                    pose.ambient = ambient_at(origin, 20.0f);
                    const auto& part = outfit_->parts[static_cast<std::size_t>(actor.entry)];
                    if (part.tint) {
                        for (int channel = 0; channel < 3; ++channel) {
                            pose.ambient.color[channel + 1][3] = part.color[channel] / 255.0f;
                        }
                        pose.ambient.color[4][3] = 1.0f;
                    }
                } else if (player_index < demo_.players.size()
                    && !gmod::demo::player_incapacitated(demo_.players[player_index],time_)
                    && gmod::demo::sample_player(demo_.players[player_index], time_, player)
                    && outfit_part_worn(player_index, actor.entry)) {
                    const float origin[3]{player.x, player.y, player.z};
                    const float angles[3]{0.0f, player.yaw, 0.0f};
                    pose.active = !(hide_followed && player_index == static_cast<std::size_t>(follow_));
                    pose.transform = prop_transform(origin, angles);
                    pose.ambient = ambient_at(origin, 36.0f);
                    const auto& part = outfit_->parts[static_cast<std::size_t>(actor.entry)];
                    if (part.tint) {
                        for (int channel = 0; channel < 3; ++channel) {
                            pose.ambient.color[channel + 1][3] = part.color[channel] / 255.0f;
                        }
                        pose.ambient.color[4][3] = 1.0f;
                    }
                }
            }
            if (g_headless && pose.active && actor.kind == 4
                && outfit_->parts.at(actor.entry).data_number > 0)
                rendered_badge_parts_.insert(actor.entry);
            if (pose.active && (actor.kind == 1 || actor.kind == 4 || actor.kind == 5)
                && actor.index >= 0) {
                pose.ambient.color[5][3] = outline_color(static_cast<std::size_t>(actor.index));
                any_outline = any_outline || pose.ambient.color[5][3] > 0.5f;
            }
            if (anim_debug_ && pose.active && (actor.kind == 1 || actor.kind == 4 || actor.kind == 5)) {
                *anim_debug_ << "ACTOR slot=" << slot << " kind=" << actor.kind << " player=" << actor.index
                             << " palette=" << actor.palette << " model=" << actor.model_path << "\n";
            }
            if (pose.active && actor.muzzle_bone >= 0 && actor.palette >= 0
                && (actor.kind == 5 || actor.kind == 6)) {
                const int player=actor.kind == 6 ? vm_player_ : actor.index;
                if (player >= 0) {
                    const auto local=renderer.skinned_point(static_cast<std::size_t>(actor.palette),
                                                           actor.muzzle_bone,actor.muzzle_point);
                    ActiveMuzzle muzzle{player,{}};
                    transform_point(pose.transform,local.data(),muzzle.point.data());
                    active_muzzles_.push_back(muzzle);
                }
            }
            renderer.set_dynamic_pose(slot, pose);
        }
        renderer.set_outline(any_outline, gmod::ui::settings().esp_through_walls());
        std::sort(visible.begin(), visible.end());
        visible.erase(std::unique(visible.begin(), visible.end()), visible.end());
        // NPCs share the player pipeline but are not players: no list entry, tag or follow.
        const auto is_npc = [&](int index) {
            return index >= 0 && demo_.players[static_cast<std::size_t>(index)].npc;
        };
        visible.erase(std::remove_if(visible.begin(), visible.end(), is_npc), visible.end());
        tag_candidates_.erase(
            std::remove_if(tag_candidates_.begin(), tag_candidates_.end(),
                           [&](const TagCandidate& candidate) { return is_npc(candidate.player); }),
            tag_candidates_.end());
        visible_players_ = std::move(visible);
        if (follow_position) {
            last_follow_ = follow_position;
        }
        // A followed player who is not in the world keeps the camera where it was.
        float follow_height = 60.0f;
        if (follow_ >= 0) {
            const auto& player = demo_.players[static_cast<std::size_t>(follow_)];
            gmod::demo::PlayerPose pose;
            if (gmod::demo::player_incapacitated(player,time_)) follow_height = 20.0f;
            else if (gmod::demo::sample_player(player,time_,pose) && (pose.flags & 2) != 0)
                follow_height = 28.0f;
        }
        std::optional<std::array<float,2>> vehicle_look;
        if (vehicle_camera_.active && !vehicle_camera_.free_look && !free_view_) {
            float pitch=0,yaw=0;
            if (gmod::demo::sample_player_look(demo_,follow_,time_,pitch,yaw))
                vehicle_look=std::array<float,2>{pitch,yaw};
        }
        renderer.set_follow(follow_ >= 0 && !free_view_ ? last_follow_ : std::nullopt,follow_height,vehicle_look);
        renderer.set_first_person(view);
        update_combat_visuals(renderer);
    }

    // The followed player's point of view: the recorder's own recorded camera when
    // following the recorder, otherwise the player's eyes and recorded angles.
    std::optional<Dx12Foundation::FirstPersonView> first_person_view() {
        if (!first_person_ || follow_ < 0) return std::nullopt;
        std::optional<Dx12Foundation::FirstPersonView> view;
        gmod::demo::CameraKey camera;
        gmod::demo::PlayerPose body;
        const bool have_body = gmod::demo::sample_player(
            demo_.players[static_cast<std::size_t>(follow_)], time_, body);
        const bool seated = have_body && (body.flags & gmod::demo::PlayerFlagSeated) != 0;
        const float eye = gmod::demo::player_eye_height(have_body ? body.flags : 0);
        if (follow_ == demo_.recorder_player
            && gmod::demo::sample_recorder_camera(demo_, time_, camera)) {
            if (camera.origin_valid && !seated) {
                // The recorded view origin is the player's feet, not the eyes.
                view = Dx12Foundation::FirstPersonView{
                    camera.x, camera.y, camera.z + eye, camera.yaw, camera.pitch};
            } else if (have_body) {
                // In a vehicle the engine sends no world origin, so the seat position
                // (which follows the vehicle) carries the recorded view angles.
                view = Dx12Foundation::FirstPersonView{
                    body.x, body.y, body.z + eye, camera.yaw, camera.pitch};
            }
        } else if (have_body) {
            view = Dx12Foundation::FirstPersonView{body.x, body.y, body.z + eye, body.yaw, body.pitch};
        }
        // A player who is not in the world keeps the camera where it was.
        if (view) last_view_ = view;
        return last_view_;
    }

    static std::wstring clock(float seconds) {
        const int total = static_cast<int>(std::max(seconds, 0.0f));
        wchar_t text[32];
        if (total >= 3600) {
            swprintf_s(text, L"%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
        } else {
            swprintf_s(text, L"%d:%02d", total / 60, total % 60);
        }
        return text;
    }

    std::wstring status() const {
        std::wstring text = !playing_ ? L"PAUSED " : reverse_ ? L"REVERSE " : L"PLAY ";
        text += clock(time_) + L" / " + clock(demo_.duration);
        wchar_t speed[24];
        swprintf_s(speed, L" x%g", kSpeeds[speed_index_]);
        text += speed;
        if (free_view_) {
            text += L" | FREE VIEW";
        } else if (follow_ >= 0) {
            text += (first_person_ ? L" | 1st person " : L" | 3rd person ") + follow_name();
            if (vehicle_camera_.active) text += vehicle_camera_.free_look ? L" | FreeLook: ON" : L" | FreeLook: OFF";
        }
        if (!use_doors_) text += L" | doors closed";
        text += vehicle_camera_.active ? L" | F free view, R FreeLook, hold P players"
                                       : L" | F free view, R 1st/3rd person, hold P players";
        return text;
    }

    gmod::demo::DemoData demo_;
    std::vector<ActorBinding> actors_;
    gmod::source::bsp::BspProbe probe_;
    std::shared_ptr<gmod::source::model::PlayerAnimator> animator_;
    std::shared_ptr<OutfitData> outfit_;
    std::shared_ptr<DemoAudioAssets> audio_assets_;
    int rendered_arm_parts_=0;
    std::vector<int> rendered_back_guns_;
    gmod::demo::FirePlayback fire_playback_;
    float last_effect_time_ = 0.0f;
    std::vector<float> next_step_;  // demo time of each player's next footstep, -1 = none
    std::vector<char> ragdolled_;   // the player is a ragdoll at the current time
    std::vector<std::array<float, 3>> ragdoll_origin_;
    std::vector<std::string> ragdoll_model_;  // normalized path of the ragdoll's model
    std::unordered_map<std::string, std::vector<std::string>> fire_sound_cache_;
    // GMODREPLAY_ANIM_DEBUG=<file>: each player's animation inputs, every frame (research aid).
    std::unique_ptr<std::ofstream> anim_debug_ = [] {
        wchar_t path[MAX_PATH]{};
        return GetEnvironmentVariableW(L"GMODREPLAY_ANIM_DEBUG", path, MAX_PATH) > 0
            ? std::make_unique<std::ofstream>(path) : nullptr;
    }();
    std::unordered_map<std::string, std::vector<std::string>> step_sound_cache_;
    std::mt19937 effect_rng_{20260416};
    std::vector<gmod::source::model::PlayerAnimator::State> animation_states_;
    std::vector<float> animation_time_;  // demo time of each player's last update, -1 = idle
    std::vector<gmod::source::model::Matrix3x4> palette_scratch_;
    // First-person view model this frame (vm_id_ < 0 when none is shown).
    int vm_id_ = -1;
    int vm_player_ = -1;
    Matrix4 vm_transform_{};
    AmbientGpu vm_ambient_{};
    std::vector<gmod::source::model::Matrix3x4> vm_palette_;
    std::vector<gmod::source::model::Matrix3x4> vehicle_palette_;
    std::vector<gmod::source::model::Matrix3x4> arm_palette_;
    int fallback_model_ = -1;
    std::map<int,std::pair<int,float>> bank_shutter_palettes_;
    std::size_t bank_shutter_palette_updates_=0;
    bool lighting_enabled_ = true;
    bool bank_lighting_enabled_ = true;
    gmod::source::bsp::BankLightWeights bank_weights_{};
    std::vector<std::optional<AmbientGpu>> door_ambient_;
    struct CachedAmbient {
        float origin[3];
        AmbientGpu ambient;
    };
    std::vector<std::optional<CachedAmbient>> prop_ambient_;
    float time_ = 0.0f;
    float viewable_ = 0.0f;
    bool playing_ = true;
    bool reverse_ = false;
    bool resume_after_scrub_ = false;
    std::unique_ptr<gmod::ui::TimelineBar> timeline_;
    bool use_doors_ = true;
    struct ActiveMuzzle { int player; std::array<float,3> point; };
    struct RecordedDecal {
        float time;
        std::string material;
        int target;
        std::vector<gmod::source::WorldTrace::DecalVertex> vertices;
    };
    std::vector<ActiveMuzzle> active_muzzles_;
    bool smoke_built_ = false;
    std::vector<gmod::demo::DemoEffect> muzzle_smoke_;
    std::vector<RecordedDecal> decals_;
    std::set<std::pair<std::size_t,int>> missing_ragdolls_logged_;
    std::set<std::size_t> missing_brush_poses_logged_;
    bool decals_built_ = false;
    std::unordered_map<int,float> last_flash_log_;
    int speed_index_ = 2;
    int follow_ = -1;
    bool first_person_ = false;
    gmod::app::VehicleCamera vehicle_camera_;
    bool free_view_ = true;
    std::optional<Dx12Foundation::FirstPersonView> last_view_;
    HWND window_ = nullptr;
    std::unique_ptr<gmod::ui::PlayerListPanel> player_list_;
    bool player_list_open_ = false;
    bool controls_open_ = false;
    bool controls_recapture_ = false;  // free-view mouse look to restore when the bar closes
    DWORD shift_down_at_ = 0;          // when Shift went down (0 = up)
    bool shift_spoiled_ = false;       // Shift was used with another key, so it is not a tap
    bool player_list_pinned_ = false;
    bool hide_vehicles_ = false;
    bool mouse_was_captured_ = false;
    DWORD last_list_refresh_ = 0;
    std::optional<std::array<float, 3>> last_follow_;
    std::vector<int> visible_players_;
    struct TagCandidate {
        int player = -1;
        float x = 0.0f, y = 0.0f, z = 0.0f, head = 84.0f;
    };
    std::vector<TagCandidate> tag_candidates_;
    std::unique_ptr<gmod::ui::RadioHud> radio_hud_;
    std::unique_ptr<gmod::ui::DialogHud> dialog_hud_;
    std::unique_ptr<gmod::ui::PlayerCard> player_card_;
    std::unique_ptr<gmod::ui::Hud> hud_;
    std::unique_ptr<gmod::ui::EscMenu> esc_menu_;
    std::unique_ptr<gmod::ui::GameConsole> console_;
    bool esc_open_ = false;
    bool console_modal_ = false;
    bool settings_changed_ = false;
    int modal_depth_ = 0;
    std::vector<FeedEvent> feed_events_;
    std::vector<ConsoleEntry> console_entries_;
    std::size_t console_next_ = 0;
    std::size_t log_shown_ = 0;  // program output lines already in the console
    float console_time_ = 0.0f;
    DWORD started_ticks_ = 0;
    std::array<float, 3> camera_position_{};
    std::unordered_map<std::uint64_t, WorldLoop> active_loops_;
    gmod::audio::StoppedSoundEvents stopped_sound_events_;
    std::ofstream world_sound_log_;
    std::filesystem::path world_sound_log_path_;
    std::unordered_map<int, std::vector<int>> prop_by_entity_;  // entity index -> demo_.props indexes
    int next_loop_id_ = 1;
    float world_sound_time_ = 0.0f;
    bool world_sound_ready_ = false;
    bool world_sound_seek_ = false;
    bool effect_sound_seek_ = false;
    bool world_sound_paused_ = false;
    DWORD last_loop_move_ = 0;
    int deploy_cuffed_ = -2;            // animation sequences for the held gestures (-2: not looked up)
    int deploy_phone_call_ = -2;
    int deploy_phone_open_ = -2;
    bool preview_menu_ = false;
    bool preview_console_ = false;
    bool preview_ran_ = false;
    std::wstring preview_command_;
    int card_player_ = -1;  // player shown in the info card, -1 = closed
    int card_vehicle_ = -1; // vehicle (index into demo_.props) shown in the info card instead
    int real_start_ = -1;   // local time of day (seconds) the recording began, -1 = unknown
    std::map<int, std::array<float,12>> rendered_plate_poses_;
    std::set<int> rendered_badge_parts_;
    bool preview_timeline_ = false;  // headless screenshot: draw the timeline into the image
    int isolated_voice_ = -1;  // player whose voice is the only one heard, -1 = everyone
    int applied_isolation_ = -1;
    std::set<int> muted_voices_;  // players whose voice is silenced
    std::size_t applied_muted_ = 0;
    // Isolation is enabled explicitly from the player card or Tab list.
    int effective_isolation() const { return isolated_voice_; }
    int hovered_ = -1;      // player under the cursor
    bool click_test_ = false;
    DWORD last_card_refresh_ = 0;
    bool radio_muted_ = false;
    std::set<int> voice_active_;
    float last_voice_time_ = 0.0f;
    float max_voice_duration_ = 0.0f;
    std::array<bool, 256> key_was_down_{};
};

// GMod's own SWEPs name themselves with a "#token" (SWEP.PrintName = "#weapon_fists"); the game
// shows the English text from resource/localization/en/*.properties. Earlier roots win.
void localize_swep_names(gmod::demo::PerpheadsRegistry& registry,
                         const std::vector<std::filesystem::path>& roots) {
    namespace vfs = gmod::source::vfs;
    const bool any = std::any_of(registry.swep_info.begin(), registry.swep_info.end(),
        [](const auto& entry) { return !entry.second.name.empty() && entry.second.name.front() == '#'; });
    if (!any) return;
    std::map<std::string, std::string> text;
    for (const auto& root : roots) {
        const auto folder = root / L"resource" / L"localization" / L"en";
        if (!vfs::is_directory(folder)) continue;
        for (const auto& name : vfs::list_files(folder, false)) {
            if (name.size() < 11 || name.compare(name.size() - 11, 11, ".properties") != 0) continue;
            std::vector<std::uint8_t> bytes;
            try {
                bytes = vfs::read(folder / name);
            } catch (const std::exception&) {
                continue;
            }
            std::istringstream lines(std::string(bytes.begin(), bytes.end()));
            std::string line;
            while (std::getline(lines, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const auto equals = line.find('=');
                if (equals == std::string::npos || equals == 0 || line[0] == '#') continue;
                text.emplace(line.substr(0, equals), line.substr(equals + 1));
            }
        }
    }
    for (auto& [_, info] : registry.swep_info) {
        if (info.name.empty() || info.name.front() != '#') continue;
        const auto found = text.find(info.name.substr(1));
        info.name = found != text.end() ? found->second : std::string();
    }
}

LoadedScene load_scene(
    const std::filesystem::path& application_root, LoadingState& loading,
    gmod::demo::DemoData* demo = nullptr, const std::filesystem::path& demo_file = {},
    CpuAssetCache* cache = nullptr, RetainedMap* retained = nullptr, bool preparing_map = false) {
    // Game content is read in place from Garry's Mod (Workshop addons and the base archives).
    loading.begin_stage(1, L"Finding Garry's Mod content", 0);
    const auto content = gmod::source::content::mount_content(
        application_root, demo ? demo->map_name : std::string(), demo_file);
    {
        std::ofstream content_log(application_root / L"build\\content.log", std::ios::trunc);
        for (const auto& line : content.log) content_log << line << "\n";
    }
    const auto bsp_path = content.bsp;
    if (bsp_path.empty()) {
        throw std::runtime_error("Map " + (demo ? demo->map_name : std::string("rp_paralake_city_v6"))
                                 + " was not found in Garry's Mod. Is the PERP Workshop content downloaded?");
    }
    if (cache) cache->begin_load();
    std::string retained_key;
    if (retained) {
        retained_key = cache->source_key(bsp_path);
        for (const auto& root : content.roots) retained_key += "|" + root.lexically_normal().string();
        if (!retained->scene || retained->key != retained_key || !cache->sources_current()) {
            std::cout << "RETAINED_MAP_REBUILD reason="
                << (!retained->scene ? "first-map" : retained->key != retained_key ? "map-or-mounts-changed" : "source-changed")
                << "\n";
            const auto revision = retained->revision + 1;
            *retained = {};
            retained->revision = revision;
            retained->scene = load_scene(application_root, loading, demo, demo_file, cache, nullptr, true);
            retained->key = retained_key;
        } else {
            ++retained->reuses;
            std::cout << "RETAINED_MAP_REUSE: skipping map preparation\n";
        }
    }
    const LoadedScene* map_scene = retained ? &*retained->scene : nullptr;
    auto* brush_demo = demo;
    if (preparing_map) demo = nullptr;
    loading.begin_stage(1, map_scene ? L"Binding demo to the open map" : L"Reading BSP world geometry and displacements", 0);
    gmod::source::bsp::BspReader local_bsp(bsp_path);
    gmod::source::bsp::StaticPropsReader local_props(bsp_path);
    gmod::source::bsp::BspSummary summary;
    gmod::source::bsp::StaticPropsSummary prop_summary;
    const auto map_key = cache ? cache->source_key(bsp_path) : std::string();
    const auto map = cache ? cache->maps.find(map_key) : std::map<std::string, CpuAssetCache::Map>::iterator{};
    auto& bsp = map_scene ? cache->maps.at(map_key).bsp : local_bsp;
    auto& static_props = map_scene ? cache->maps.at(map_key).props : local_props;
    if (cache && map != cache->maps.end()) {
        if (!map_scene) bsp = map->second.bsp;
        summary = map->second.summary;
        if (!map_scene) static_props = map->second.props;
        prop_summary = map->second.prop_summary;
        ++cache->map_hits;
    } else {
        summary = bsp.read();
        loading.check_cancel();
        loading.begin_stage(2, L"Reading static-prop placements", 0);
        prop_summary = static_props.read();
        if (cache) cache->maps.emplace(map_key, CpuAssetCache::Map{bsp, summary, static_props, prop_summary});
    }
    const auto player_spawn = map_scene ? map_scene->player_spawn : bsp.first_player_spawn();
    loading.check_cancel();
    gmod::source::AssetResolver assets(content.roots);
    if (demo) {
        std::map<int,std::optional<std::array<float,12>>> attachments;
        std::ofstream log(application_root / L"build\\vehicle_seats.log");
        for (auto& prop : demo->props) {
            if (!prop.vehicle || prop.model < 0 || prop.owners.empty()) continue;
            auto [cached,inserted] = attachments.try_emplace(prop.model);
            if (inserted) {
                const auto& model = demo->models[static_cast<std::size_t>(prop.model)];
                const auto status = assets.resolve_model(model);
                if (status.has_mdl) {
                    const auto bones = gmod::source::model::read_bones(status.mdl);
                    const auto entries = gmod::source::model::read_attachments(status.mdl);
                    const auto entry = std::find_if(entries.begin(),entries.end(),[](const auto& value) {
                        return value.name == "vehicle_feet_passenger0";
                    });
                    if (entry != entries.end() && entry->bone >= 0
                        && static_cast<std::size_t>(entry->bone) < bones.size()) {
                        cached->second = rigid_compose(
                            rigid_inverse(bones[static_cast<std::size_t>(entry->bone)].pose_to_bone),
                            entry->local);
                        const auto& matrix = *cached->second;
                        log << "DRIVER_ATTACHMENT model=" << model << " local="
                            << matrix[3] << "," << matrix[7] << "," << matrix[11] << "\n";
                    }
                }
                if (!cached->second)
                    log << "DRIVER_ATTACHMENT_UNAVAILABLE model=" << model << " (retaining legacy offset)\n";
            }
            prop.driver_attachment = cached->second;
        }
        gmod::demo::rebind_vehicle_seats(*demo);
    }
    auto world_triangles = map_scene ? map_scene->triangles : bsp.release_world_triangles();
    std::vector<std::pair<const gmod::source::bsp::BrushModel*,std::size_t>> moving_brushes;
    for (const auto& brush : bsp.brush_models()) {
        std::size_t recorded = brush_demo ? brush_demo->props.size() : 0;
        if (brush_demo) {
            for (std::size_t index = 0; index < brush_demo->props.size(); ++index) {
                const auto& prop = brush_demo->props[index];
                if (prop.brush && prop.model >= 0
                    && brush_demo->models[static_cast<std::size_t>(prop.model)] == brush.model) {
                    recorded = index;
                    break;
                }
            }
        }
        if (preparing_map || (brush_demo && recorded < brush_demo->props.size())) {
            moving_brushes.emplace_back(&brush,recorded);
            if (!preparing_map) continue;
        }
        if (map_scene) continue;
        const float origin[3]{brush.origin.x,brush.origin.y,brush.origin.z};
        const float angles[3]{brush.angles.x,brush.angles.y,brush.angles.z};
        const auto matrix = gmod::source::model::matrix_from_source_angles(origin,angles);
        for (auto triangle : brush.triangles) {
            if (preparing_map) triangle.map_brush = std::stoi(brush.model.substr(1));
            for (auto* point : {&triangle.a,&triangle.b,&triangle.c}) {
                const auto p = *point;
                *point = {matrix[0]*p.x+matrix[1]*p.y+matrix[2]*p.z+matrix[3],
                    matrix[4]*p.x+matrix[5]*p.y+matrix[6]*p.z+matrix[7],
                    matrix[8]*p.x+matrix[9]*p.y+matrix[10]*p.z+matrix[11]};
            }
            world_triangles.push_back(std::move(triangle));
        }
    }
    std::size_t resolved_mdl = 0;
    std::size_t resolved_complete = 0;
    std::size_t readable_vvd = 0;
    std::size_t rendered_prop_instances = 0;
    std::size_t rendered_clothing_instances = 0;
    std::size_t failed_prop_models = 0;
    PropRenderData prop_data = map_scene ? map_scene->prop_data : PropRenderData{};
    if (map_scene) {
        prop_data.map_vertices = prop_data.vertices.size();
        prop_data.map_indices = prop_data.indices.size();
    }
    std::set<std::string> prop_material_names;
    std::vector<gmod::source::bsp::Vec3> ambient_points{{0.0f, 0.0f, 0.0f}};
    if (!map_scene) prop_data.transforms.push_back(Matrix4{{1.0f, 0.0f, 0.0f, 0.0f,
                                             0.0f, 1.0f, 0.0f, 0.0f,
                                             0.0f, 0.0f, 1.0f, 0.0f,
                                             0.0f, 0.0f, 0.0f, 1.0f}});
    std::set<std::string> world_materials;
    std::size_t resolved_vmts = 0;
    std::size_t resolved_vtfs = 0;
    std::vector<WorldMaterialTexture> material_textures = map_scene
        ? map_scene->material_textures : std::vector<WorldMaterialTexture>{};
    std::unordered_map<std::string,std::shared_ptr<gmod::source::WorldTrace>> model_traces;
    if (!map_scene) for (const auto& triangle : world_triangles) {
        if (!triangle.material.empty()) {
            world_materials.insert(triangle.material);
        }
    }
    for (const auto& [brush,index] : moving_brushes)
        for (const auto& triangle : brush->triangles) world_materials.insert(triangle.material);
    const std::vector<std::filesystem::path>& content_roots = content.roots;
    // Looks a Source-relative path ("materials/x.vmt") up in every content root.
    auto find_source_file = [&](std::string relative) -> std::filesystem::path {
        std::replace(relative.begin(), relative.end(), '\\', '/');
        for (const auto& root : content_roots) {
            const auto candidate = root / std::filesystem::path(relative);
            if (gmod::source::vfs::exists(candidate)) {
                return candidate;
            }
        }
        return {};
    };
    // Player animation: sequences come from a model the players' MDL includes.
    std::shared_ptr<gmod::source::model::PlayerAnimator> animator;
    std::string animator_log = "none";
    if (demo) {
        const int reference_model = fallback_player_model(*demo);
        if (reference_model >= 0) {
            const auto player_status = assets.resolve_model(
                demo->models[static_cast<std::size_t>(reference_model)]);
            if (player_status.has_mdl) {
                try {
                    for (const auto& include : gmod::source::model::read_include_models(player_status.mdl)) {
                        const auto animation_mdl = find_source_file(include);
                        if (animation_mdl.empty()) continue;
                        auto animation_ani = animation_mdl;
                        animation_ani.replace_extension(".ani");
                        try {
                            animator = std::make_shared<gmod::source::model::PlayerAnimator>(
                                animation_mdl, animation_ani);
                            animator_log = include;
                            break;
                        } catch (const std::exception& error) {
                            animator_log = include + " rejected: " + error.what();
                        }
                    }
                } catch (const std::exception& error) {
                    animator_log = std::string("include scan failed: ") + error.what();
                }
            }
        }
        // Two extra palettes after the players: the view model's own skeleton, and the
        // player skeleton bonemerged onto it for the arms' clothing.
        prop_data.palette_players = demo->players.size() + 2;
        if (prop_data.palette_players >= PaletteMask) {
            throw std::runtime_error("The demo has more players than the bone palette supports.");
        }
    }
    if (map_scene && animator) prop_data.skin.resize(prop_data.vertices.size());
    // Reads a VMT and, for patch/include materials, inherits $basetexture
    // from the included parent (searched across all roots, depth bounded).
    std::function<gmod::source::vmt::Material(const std::string&, int)> load_vmt =
        [&](const std::string& relative_vmt, int depth)
        -> gmod::source::vmt::Material {
        const auto path = find_source_file(relative_vmt);
        if (path.empty()) {
            return {};
        }
        gmod::source::vmt::Material parsed;
        if (cache) {
            const auto key = cache->source_key(path);
            const auto found = cache->materials.find(key);
            if (found != cache->materials.end()) {
                parsed = found->second;
                ++cache->material_hits;
            } else {
                parsed = gmod::source::vmt::VmtReader(path).read();
                cache->materials.emplace(key, parsed);
            }
        } else {
            parsed = gmod::source::vmt::VmtReader(path).read();
        }
        // Cache the file itself, not inherited results: parents depend on the current mount order.
        if (!parsed.include_path.empty() && depth < 8) {
            auto parent = load_vmt(parsed.include_path, depth + 1);
            if (parsed.shader.empty() || parsed.shader == "patch") {
                parsed.shader = parent.shader;
            }
            if (parsed.base_texture.empty()) {
                parsed.base_texture = parent.base_texture;
            }
            if (parsed.base_texture2.empty()) {
                parsed.base_texture2 = parent.base_texture2;
            }
            if (parsed.blend_modulate_texture.empty()) {
                parsed.blend_modulate_texture = parent.blend_modulate_texture;
            }
            if (parsed.normal_map.empty()) {
                parsed.normal_map = parent.normal_map;
            }
            if (parsed.dudv_map.empty()) {
                parsed.dudv_map = parent.dudv_map;
            }
            parsed.water_above = parsed.water_above && parent.water_above;
            parsed.translucent = parsed.translucent || parent.translucent;
            parsed.alpha_test = parsed.alpha_test || parent.alpha_test;
            parsed.additive = parsed.additive || parent.additive;
            parsed.no_cull = parsed.no_cull || parent.no_cull;
            if (parsed.surface_prop.empty()) parsed.surface_prop = parent.surface_prop;
            parsed.blend_tint_by_base_alpha = parsed.blend_tint_by_base_alpha
                || parent.blend_tint_by_base_alpha;
            if (parsed.alpha_test_reference <= 0.0f) {
                parsed.alpha_test_reference = parent.alpha_test_reference;
            }
        }
        // Some materials name their texture with backslashes, a leading "materials/" or a
        // file extension; the engine accepts all of them.
        for (std::string* texture : {&parsed.base_texture, &parsed.base_texture2,
                                     &parsed.blend_modulate_texture, &parsed.normal_map, &parsed.dudv_map}) {
            std::replace(texture->begin(), texture->end(), '\\', '/');
            while (texture->find("//") != std::string::npos) texture->erase(texture->find("//"), 1);
            if (texture->rfind("materials/", 0) == 0) texture->erase(0, 10);
            for (const char* extension : {".vtf", ".vmt"}) {
                const std::size_t length = std::strlen(extension);
                if (texture->size() > length
                    && _stricmp(texture->c_str() + texture->size() - length, extension) == 0) {
                    texture->resize(texture->size() - length);
                }
            }
        }
        return parsed;
    };
    // Source names env_cubemap-patched materials "maps/<map>/<name>_<x>_<y>_<z>";
    // when no such VMT exists the engine uses the original "<name>" VMT.
    auto unpatched_name = [](const std::string& name) -> std::string {
        std::string lowered = name;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowered.rfind("maps/", 0) != 0) {
            return "";
        }
        const auto map_end = lowered.find('/', 5);
        if (map_end == std::string::npos) {
            return "";
        }
        auto base = lowered.substr(map_end + 1);
        for (int part = 0; part < 3; ++part) {
            const auto underscore = base.rfind('_');
            if (underscore == std::string::npos || underscore + 1 >= base.size()) {
                return "";
            }
            for (std::size_t i = underscore + 1; i < base.size(); ++i) {
                if (!(std::isdigit(static_cast<unsigned char>(base[i]))
                      || (i == underscore + 1 && base[i] == '-'))) {
                    return "";
                }
            }
            base.resize(underscore);
        }
        return base;
    };
    // Variant key: model, skin, and packed bodygroup state (body * 16 + hardware).
    std::map<std::tuple<std::string, int, int, std::string, int>,
             std::vector<const gmod::source::bsp::StaticProp*>> prop_variants;
    if (!map_scene) for (const auto& prop : static_props.props()) {
        if (!prop.model.empty()) {
            prop_variants[{prop.model, prop.skin, 0, {}, 0}].push_back(&prop);
        }
    }
    // Doors, prop_dynamic and physics props come from the entity lump and are
    // drawn exactly like static props, at their spawn position.
    std::vector<gmod::source::bsp::StaticProp> entity_props;
    entity_props.reserve(bsp.entity_models().size());
    std::map<const gmod::source::bsp::StaticProp*, int> map_entity_ids;
    std::set<std::string> unrecorded_scenes;
    if (demo) {
        const auto scenes = gmod::source::bsp::unrecorded_map_scenes(
            bsp.entity_models(), demo->recorded_map_entities);
        if (scenes) {
            unrecorded_scenes = *scenes;
            std::cout << "MAP_SCENES recorded_map_entities=" << demo->recorded_map_entities.size()
                << " omitted_groups=" << unrecorded_scenes.size() << "\n";
            for (const auto& group : unrecorded_scenes)
                std::cout << "MAP_SCENE_UNRECORDED " << group << "\n";
        } else {
            std::cout << "MAP_SCENES_UNAVAILABLE no recorded map IDs (retaining authored layout)\n";
        }
    }
    const auto recorded_parent_model = [&](const gmod::demo::DemoProp& prop) -> std::string {
        if (!demo) return {};
        const auto key = std::find_if(prop.keys.begin(),prop.keys.end(),
            [](const auto& value) { return value.parent > 0; });
        if (key == prop.keys.end()) return {};
        for (const auto& parent : demo->props)
            if (parent.entity == key->parent && (parent.brush || parent.map_attached) && parent.model >= 0)
                return demo->models[static_cast<std::size_t>(parent.model)];
        return {};
    };
    for (std::size_t entity_index = 0; entity_index < bsp.entity_models().size(); ++entity_index) {
        const auto& entity = bsp.entity_models()[entity_index];
        if (unrecorded_scenes.contains(entity.targetname)) {
            if (map_scene) {
                for (auto& instance : prop_data.instances)
                    if (instance.map_entity == static_cast<int>(entity_index)) instance.active = false;
            }
            continue;
        }
        gmod::source::bsp::StaticProp prop;
        prop.model = entity.model;
        prop.origin[0] = entity.origin.x;
        prop.origin[1] = entity.origin.y;
        prop.origin[2] = entity.origin.z;
        prop.angles[0] = entity.angles.x;
        prop.angles[1] = entity.angles.y;
        prop.angles[2] = entity.angles.z;
        prop.skin = entity.skin;
        prop.body = entity.body;
        prop.hardware = entity.hardware;
        prop.is_door = entity.classname == "prop_door_rotating";
        prop.door_distance = entity.door_distance;
        prop.door_speed = entity.door_speed;
        prop.open_dir = entity.open_dir;
        prop.fade_max = entity.fade_max;
        prop.start_hidden = entity.hidden;
        if (demo) {
            std::vector<std::size_t> hierarchy_matches, id_matches, shutter_origin_matches;
            for (std::size_t index = 0; index < demo->props.size(); ++index) {
                const auto& recorded = demo->props[index];
                if (!recorded.map_attached || recorded.model < 0
                    || demo->models[static_cast<std::size_t>(recorded.model)] != entity.model)
                    continue;
                const auto parent_model = recorded_parent_model(recorded);
                if (!parent_model.empty() && !entity.parent_model.empty()) {
                    if (parent_model != entity.parent_model) continue;
                    hierarchy_matches.push_back(index);
                } else if (recorded.map_created_id == entity.map_created_id) {
                    id_matches.push_back(index);
                } else if(gmod::demo::is_bank_shutter(entity.model)
                    && gmod::demo::bank_shutter_matches_origin(recorded,prop.origin)) {
                    shutter_origin_matches.push_back(index);
                }
            }
            const auto& matches = !hierarchy_matches.empty() ? hierarchy_matches
                : !id_matches.empty() ? id_matches : shutter_origin_matches;
            if (matches.size() == 1) {
                const auto index = matches.front();
                auto& recorded = demo->props[index];
                prop.actor_kind = 9;
                prop.actor_index = static_cast<int>(index);
                const float zero[3]{};
                const float parent_angles[3]{
                    entity.parent_angles.x,entity.parent_angles.y,entity.parent_angles.z};
                recorded.map_rotation = rigid_compose(rigid_inverse(
                    gmod::source::model::matrix_from_source_angles(zero,parent_angles).data()),
                    gmod::source::model::matrix_from_source_angles(zero,prop.angles));
                std::cout << "MAP_ATTACHMENT entity=" << recorded.entity << " map_id="
                    << entity.map_created_id << " model=" << entity.model
                    << " match=" << (!hierarchy_matches.empty() ? "parent-model" : !id_matches.empty() ? "map-id" : "shutter-origin") << "\n";
            } else if (matches.size() > 1) {
                std::cout << "MAP_ATTACHMENT_AMBIGUOUS map_id=" << entity.map_created_id
                    << " model=" << entity.model << " candidates=" << matches.size()
                    << " (retaining authored placement)\n";
            }
        }
        if (entity.hidden && prop.actor_kind != 9) continue;
        entity_props.push_back(std::move(prop));
        map_entity_ids.emplace(&entity_props.back(), static_cast<int>(entity_index));
    }

    // Demo playback: players and player-placed props become extra instances, and
    // each networked door takes over the map door at the same origin.
    std::vector<gmod::source::bsp::StaticProp> actor_props;
    std::size_t matched_doors = 0;
    std::string weapon_log;
    auto outfit = std::make_shared<OutfitData>();
    if (demo) {
        const int fallback = fallback_player_model(*demo);
        // What every character wears is composed from the server's clothing-data store
        // with the server's own attachment rules.
        try {
            // The data matching the server Lua the demo was recorded with; the shared folder
            // (last refreshed from the newest demo) when that could not be built.
            const auto shared = application_root / L"assets\\data\\perpheads";
            std::ofstream(application_root / L"build\\outfit.log", std::ios::app)
                << "PERP_DATA " << (demo->perp_data.empty() ? "shared" : "demo") << " ("
                << demo->perp_data_note << ")\n";
            try {
                outfit->registry = gmod::demo::load_perpheads_registry(
                    demo->perp_data.empty() ? shared : demo->perp_data);
            } catch (const std::exception&) {
                if (demo->perp_data.empty()) throw;
                outfit->registry = gmod::demo::load_perpheads_registry(shared);
            }
            localize_swep_names(outfit->registry, content_roots);
        } catch (const std::exception& error) {
            std::ofstream(application_root / L"build\\outfit.log", std::ios::app)
                << "REGISTRY_FAILED " << error.what() << "\n";
        }
        outfit->by_player.resize(demo->players.size());
        if(animator) {
            std::set<int> models;
            for(const auto& player:demo->players) for(const auto& key:player.samples) if(key.model>=0) models.insert(key.model);
            const auto& names=animator->skeleton_names();
            for(int model:models) {
                const auto status=assets.resolve_model(demo->models.at(model));
                if(!status.has_mdl) continue;
                const auto bones=gmod::source::model::read_bones(status.mdl);
                const auto attachments=gmod::source::model::read_attachments(status.mdl);
                const auto chest=std::find_if(attachments.begin(),attachments.end(),[](const auto& a){return a.name=="chest";});
                if(chest==attachments.end() || chest->bone<0 || static_cast<std::size_t>(chest->bone)>=bones.size()) continue;
                int bone=chest->bone;
                while(bone>=0 && static_cast<std::size_t>(bone)<bones.size()) {
                    const auto found=std::find(names.begin(),names.end(),bones[bone].name);
                    if(found!=names.end()) {
                        outfit->chest_attachments[model]={static_cast<int>(found-names.begin()),
                            rigid_compose(rigid_inverse(bones[chest->bone].pose_to_bone),chest->local)};
                        break;
                    }
                    bone=bones[bone].parent;
                }
            }
        }
        // Ragdolls: which skeleton bone each physics solid of the ragdoll's model drives.
        if (animator) {
            const auto& names = animator->skeleton_names();
            for (const auto& ragdoll : demo->ragdolls) {
                if (ragdoll.model <= 0 || outfit->ragdoll_bones.count(ragdoll.model) != 0) continue;
                const auto path = demo->model_paths_by_index.find(ragdoll.model);
                if (path == demo->model_paths_by_index.end()) continue;
                const auto status = assets.resolve_model(path->second);
                if (!status.has_mdl) continue;
                auto phy = status.mdl;
                phy.replace_extension(".phy");
                std::vector<int> mapping;
                for (const auto& name : gmod::source::model::read_phy_solid_names(phy)) {
                    const auto found = std::find(names.begin(), names.end(), name);
                    mapping.push_back(found == names.end() ? -1 : static_cast<int>(found - names.begin()));
                }
                std::ofstream(application_root / L"build\\outfit.log",std::ios::app)
                    << "RAGDOLL_MAPPING model=" << path->second << " solids=" << mapping.size()
                    << " mapped=" << std::count_if(mapping.begin(),mapping.end(),[](int bone) { return bone >= 0; })
                    << "\n";
                outfit->ragdoll_bones.emplace(ragdoll.model, std::move(mapping));
            }
        }
        for (std::size_t index = 0; index < demo->players.size(); ++index) {
            std::vector<int> player_parts;  // global part ids already created for this player
            for (const auto& key : demo->players[index].outfit) {
                const auto record = demo->clothing_records.find(key.value);
                if (record == demo->clothing_records.end()) continue;
                OutfitData::Key outfit_key;
                outfit_key.time = key.time;
                for (const auto& part : gmod::demo::compose_outfit(outfit->registry, record->second)) {
                    int found = -1;
                    for (const int existing : player_parts) {
                        if (outfit->parts[static_cast<std::size_t>(existing)] == part) {
                            found = existing;
                            break;
                        }
                    }
                    if (found < 0) {
                        found = static_cast<int>(outfit->parts.size());
                        outfit->parts.push_back(part);
                        player_parts.push_back(found);
                        outfit->owner.push_back(static_cast<int>(index));
                    }
                    outfit_key.parts.push_back(found);
                }
                outfit->by_player[index].push_back(std::move(outfit_key));
            }
        }
        {
            std::ofstream log(application_root / L"build\\outfit.log", std::ios::app);
            for (std::size_t index = 0; index < demo->players.size(); ++index) {
                std::set<int> parts;
                for (const auto& key : outfit->by_player[index]) parts.insert(key.parts.begin(), key.parts.end());
                log << "PLAYER " << index << " entity=" << demo->players[index].entity << " parts=" << parts.size();
                for (const int part : parts) log << " " << outfit->parts[static_cast<std::size_t>(part)].model;
                log << "\n";
            }
        }
        // Material replacements differ per clothing skin; each distinct table is a variant.
        std::map<std::vector<std::pair<std::string, std::string>>, int> remap_ids;
        for (std::size_t index = 0; index < demo->players.size(); ++index) {
            std::set<int> seen;
            for (const auto& key : outfit->by_player[index]) {
                for (const int part_id : key.parts) {
                    if (!seen.insert(part_id).second) continue;
                    const auto& part = outfit->parts[static_cast<std::size_t>(part_id)];
                    int remap = 0;
                    if (!part.sub_materials.empty()) {
                        const auto inserted = remap_ids.emplace(
                            part.sub_materials, static_cast<int>(remap_ids.size()) + 1);
                        if (inserted.second) outfit->remaps.push_back(part.sub_materials);
                        remap = inserted.first->second;
                    }
                    gmod::source::bsp::StaticProp prop;
                    prop.model = part.model;
                    prop.skin = part.skin | (remap << 8);
                    prop.actor_kind = 4;
                    prop.actor_index = static_cast<int>(index);
                    prop.actor_entry = part_id;
                    actor_props.push_back(prop);
                    // cl_viewmodel.lua draws the forearm, hand, wrist and finger clothing of
                    // the local player bonemerged onto the first-person view model.
                    const auto type = outfit->registry.clothing.find(part.type);
                    const bool arm = type != outfit->registry.clothing.end()
                        && std::any_of(type->second.slots.begin(), type->second.slots.end(),
                                       [](int slot) { return slot == 2 || slot == 3 || slot == 9
                                                          || slot == 10 || (slot >= 14 && slot <= 23); });
                    if (arm && animator && !demo->players[index].npc) {
                        prop.actor_kind = 7;
                        prop.actor_palette = static_cast<int>(demo->players.size()) + 1;
                        actor_props.push_back(std::move(prop));
                    }
                }
            }
        }
        for (std::size_t index = 0; index < demo->players.size(); ++index) {
            std::set<int> seen;
            for (const auto& sample : demo->players[index].samples) {
                const int model = sample.model >= 0 ? sample.model : fallback;
                if (model >= 0) seen.insert(model);
            }
            for (const auto& link : demo->players[index].ragdoll) {
                if (link.value <= 0) continue;
                for (const auto& ragdoll : demo->ragdolls) {
                    if (ragdoll.entity != link.value) continue;
                    const auto path = demo->model_paths_by_index.find(ragdoll.model);
                    if (path == demo->model_paths_by_index.end()) continue;
                    const auto model = std::find(demo->models.begin(),demo->models.end(),path->second);
                    if (model != demo->models.end()) seen.insert(static_cast<int>(model-demo->models.begin()));
                }
            }
            for (const int model : seen) {
                const auto& model_path = demo->models[static_cast<std::size_t>(model)];
                gmod::source::bsp::StaticProp prop;
                prop.model = model_path;
                prop.actor_kind = 1;
                prop.actor_index = static_cast<int>(index);
                prop.actor_model = model;
                actor_props.push_back(std::move(prop));
            }
        }
        // Vehicles are skinned to their own skeleton (palettes after the players and view
        // models) and animated every frame from the pose parameters the server networks;
        // placed props are plain rigid models.
        std::size_t vehicle_palettes = 0;
        for (std::size_t index = 0; index < demo->props.size(); ++index) {
            const auto& demo_prop = demo->props[index];
            if (demo_prop.model < 0 || demo_prop.brush || demo_prop.map_attached) continue;
            gmod::source::bsp::StaticProp prop;
            prop.model = demo->models[static_cast<std::size_t>(demo_prop.model)];
            prop.actor_kind = demo_prop.back_gun ? 12 : 2;
            prop.actor_index = static_cast<int>(index);
            prop.actor_entry = demo_prop.vehicle ? 1 : -1;
            prop.skin = demo_prop.skin;
            prop.body = demo_prop.body;
            if (demo_prop.vehicle && animator) {
                const auto key = normalize_clothing_model_path(prop.model);
                auto known = outfit->vehicle_ids.find(key);
                if (known == outfit->vehicle_ids.end()) {
                    int id = -1;
                    try {
                        const auto status = assets.resolve_model(prop.model);
                        if (status.has_mdl && status.has_vvd && status.has_vtx) {
                            OutfitData::ViewModelAsset asset;
                            auto ani = status.mdl;
                            ani.replace_extension(".ani");
                            asset.animation = std::make_shared<gmod::source::model::AnimationModel>(status.mdl, ani);
                            const auto& bones = asset.animation->bones();
                            if (!bones.empty() && bones.size() <= MaxPaletteBones) {
                                asset.poser = std::make_shared<gmod::source::model::SkeletonPoser>(
                                    bones, *asset.animation);
                                for (const auto& bone : bones) asset.names.push_back(bone.name);
                                asset.path = key;
                                id = static_cast<int>(outfit->vehicle_assets.size());
                                outfit->vehicle_assets.push_back(std::move(asset));
                            }
                        }
                    } catch (const std::exception& error) {
                        weapon_log += "VEHICLE_ASSET_FAILED " + prop.model + " " + error.what() + "\n";
                    }
                    known = outfit->vehicle_ids.emplace(key, id).first;
                }
                if (known->second >= 0) {
                    prop.actor_model = known->second;
                    prop.actor_palette = static_cast<int>(demo->players.size()) + 2
                        + static_cast<int>(vehicle_palettes++);
                }
            }
            actor_props.push_back(std::move(prop));
        }
        prop_data.palette_players = demo->players.size() + 2 + vehicle_palettes;
        if (prop_data.palette_players >= PaletteMask) {
            throw std::runtime_error("The demo has more vehicles than the bone palette supports.");
        }
        for (std::size_t index = 0; index < demo->props.size(); ++index) {
            const auto& vehicle = demo->props[index];
            if (!vehicle.vehicle || vehicle.model < 0) continue;
            for (const auto& state : gmod::demo::vehicle_plate_states(vehicle)) {
                const auto definition = outfit->registry.vehicle_plates.find(state.id);
                if (definition == outfit->registry.vehicle_plates.end()) {
                    weapon_log += "PLATE_LAYOUT_UNAVAILABLE entity=" + std::to_string(vehicle.entity)
                        + " vehicle=" + state.id + "\n";
                    continue;
                }
                auto placements = definition->second.placements;
                if (!definition->second.kits.empty()) {
                    const auto status = assets.resolve_model(demo->models.at(vehicle.model));
                    if (status.has_mdl) {
                        const auto groups = gmod::source::model::read_bodygroups(status.mdl);
                        std::vector<int> choices;
                        for (const auto& group : groups)
                            if (group.models > 1)
                                choices.push_back(std::max(vehicle.body, 0) / group.base % group.models);
                        for (const auto& kit : definition->second.kits)
                            if (kit.groups == choices) {
                                if (kit.plate) placements.front() = *kit.plate;
                                break;
                            }
                    }
                }
                const auto appearance = gmod::demo::plate_appearance(outfit->registry, state);
                const auto inserted = remap_ids.emplace(appearance.materials, static_cast<int>(remap_ids.size()) + 1);
                if (inserted.second) outfit->remaps.push_back(appearance.materials);
                for (const auto& placement : placements) {
                    gmod::source::bsp::StaticProp plate;
                    plate.model = appearance.model;
                    plate.skin = inserted.first->second << 8;
                    plate.actor_kind = 13;
                    plate.actor_index = static_cast<int>(index);
                    plate.actor_entry = static_cast<int>(outfit->plates.size());
                    outfit->plates.push_back({state, placement, appearance.color});
                    actor_props.push_back(std::move(plate));
                }
            }
        }
        // Held weapons are bonemerged onto the player's hand like clothing.
        std::map<std::string, std::string> weapon_models;
        std::map<std::string,std::set<int>> observed_attachments;
        for (const auto& [handle,state] : demo->weapons) {
            for (const auto& key : state.attachments) {
                const auto weapon = gmod::demo::timed_text_at(state.class_name,key.time);
                bool known = false;
                for (int id : gmod::demo::weapon_attachments_at(*demo,handle,key.time,known))
                    if (outfit->registry.weapon_attachments.count(id))
                        observed_attachments[weapon].insert(id);
            }
        }
        for (std::size_t index = 0; index < demo->players.size(); ++index) {
            std::set<std::string> held;
            for (const auto& key : demo->players[index].active_weapon) {
                if (key.handle < 0) continue;
                const auto name = gmod::demo::held_weapon_at(*demo,demo->players[index],key.time).class_name;
                if (!name.empty()) held.insert(name);
            }
            for (const auto& weapon : held) {
                auto found = weapon_models.find(weapon);
                if (found == weapon_models.end()) {
                    found = weapon_models.emplace(
                        weapon, find_weapon_world_model(assets, *demo, outfit->registry, weapon)).first;
                }
                if (found->second.empty()) {
                    weapon_log += "WEAPON_NO_WORLD_MODEL class=" + weapon + "\n";
                    continue;
                }
                weapon_log += "WEAPON_MODEL class=" + weapon + " model=" + found->second + "\n";
                gmod::source::bsp::StaticProp prop;
                prop.model = found->second;
                prop.actor_kind = 5;
                prop.actor_index = static_cast<int>(index);
                prop.actor_weapon = weapon;
                actor_props.push_back(prop);
                for (int id : observed_attachments[weapon]) {
                    auto mounted = prop;
                    mounted.model = outfit->registry.weapon_attachments.at(id).model;
                    mounted.actor_kind = 10;
                    mounted.actor_attachment = id;
                    mounted.actor_parent_model = prop.model;
                    mounted.actor_sequence = outfit->registry.weapon_attachments.at(id).sequence;
                    if (const int sight_sequence = outfit->registry.weapon_attachments.at(id).sight_sequence;
                        sight_sequence != mounted.actor_sequence) {
                        auto alternate = mounted;
                        alternate.actor_sequence = sight_sequence;
                        actor_props.push_back(std::move(alternate));
                    }
                    actor_props.push_back(std::move(mounted));
                }
            }
        }
        // First-person view models: the recorder's networked ones, and the model of every
        // weapon class seen in the demo (for following other players in first person).
        if (animator) {
            std::set<std::string> paths;
            for (const auto& key : demo->view_model) {
                const auto found = demo->model_paths_by_index.find(key.model);
                if (key.model > 0 && found != demo->model_paths_by_index.end()) paths.insert(found->second);
            }
            for (const auto& [weapon, view] : demo->weapon_view_models) paths.insert(view);
            const auto& player_names = animator->skeleton_names();
            // Bones cl_viewmodel.lua scales away so only the arms show.
            static const std::set<std::string> hidden{
                "valvebiped.bip01_pelvis", "valvebiped.bip01_head1", "valvebiped.bip01_neck1",
                "valvebiped.bip01_spine1", "valvebiped.bip01_spine2", "valvebiped.bip01_spine3",
                "valvebiped.bip01_spine4", "valvebiped.bip01_l_clavicle", "valvebiped.bip01_r_clavicle"};
            for (const auto& path : paths) {
                const auto key = normalize_clothing_model_path(path);
                if (outfit->view_model_ids.count(key) != 0) continue;
                const auto status = assets.resolve_model(path);
                if (!status.has_mdl || !status.has_vvd || !status.has_vtx) {
                    weapon_log += "VIEWMODEL_MISSING " + path + "\n";
                    continue;
                }
                try {
                    OutfitData::ViewModelAsset asset;
                    auto ani = status.mdl;
                    ani.replace_extension(".ani");
                    asset.animation = std::make_shared<gmod::source::model::AnimationModel>(status.mdl, ani);
                    const auto& bones = asset.animation->bones();
                    if (bones.empty() || bones.size() > MaxPaletteBones) {
                        weapon_log += "VIEWMODEL_TOO_MANY_BONES " + path + "\n";
                        continue;
                    }
                    asset.poser = std::make_shared<gmod::source::model::SkeletonPoser>(bones, *asset.animation);
                    for (const auto& bone : bones) {
                        asset.names.push_back(bone.name);
                        asset.bind.push_back(rigid_inverse(bone.pose_to_bone));
                    }
                    asset.arm_bone.assign(player_names.size(), -1);
                    for (std::size_t bone = 0; bone < player_names.size(); ++bone) {
                        if (hidden.count(player_names[bone]) != 0) continue;
                        const auto match = std::find(asset.names.begin(), asset.names.end(), player_names[bone]);
                        if (match != asset.names.end()) {
                            asset.arm_bone[bone] = static_cast<int>(match - asset.names.begin());
                        }
                    }
                    std::string names_log;
                    asset.fire = asset.animation->find_activity("ACT_VM_PRIMARYATTACK");
                    asset.fire_secondary = asset.animation->find_activity("ACT_VM_SECONDARYATTACK");
                    asset.draw = asset.animation->find_activity("ACT_VM_DRAW");
                    asset.idle = asset.animation->find_activity("ACT_VM_IDLE");
                    asset.reload = asset.animation->find_activity("ACT_VM_RELOAD");
                    asset.pin = asset.animation->find_sequence("pin");
                    asset.spoon = asset.animation->find_sequence("spoon");
                    asset.toss = asset.animation->find_sequence("toss_nospoon");
                    if (asset.toss < 0) asset.toss = asset.animation->find_sequence("toss");
                    asset.idle_nopin = asset.animation->find_sequence("idle_nopin");
                    asset.idle_nospoon = asset.animation->find_sequence("idle_nospoon");
                    for (int sequence = 0; sequence < static_cast<int>(asset.animation->sequence_count()); ++sequence) {
                        auto name = asset.animation->sequence_name(sequence);
                        std::transform(name.begin(), name.end(), name.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        names_log += " " + std::to_string(sequence) + "=" + name;
                        const auto has = [&](const char* word) { return name.find(word) != std::string::npos; };
                        if (asset.idle < 0 && has("idle")) asset.idle = sequence;
                        const bool aimed = has("iron") || has("sight") || has("ads");
                        const bool shot = has("fire") || has("shoot") || has("attack") || has("recoil")
                            || has("shot");
                        if (shot && !has("empty") && !has("dry") && !has("alt")) {
                            if (aimed) { if (asset.fire_aimed < 0) asset.fire_aimed = sequence; }
                            else if (asset.fire < 0) asset.fire = sequence;
                        }
                        if (asset.draw < 0 && (has("draw") || has("deploy") || has("pullout") || has("equip"))) {
                            asset.draw = sequence;
                        }
                        // A plain "reload" is the whole reload; revolvers and shotguns instead
                        // chain start, one step per round, and end (handled below).
                        if (name == "reload") asset.reload = sequence;
                    }
                    if (asset.fire_aimed < 0) asset.fire_aimed = asset.fire;
                    if (asset.reload < 0) {
                        int start = -1, end = -1, first_feed = -1;
                        std::vector<int> steps;
                        for (int sequence = 0; sequence < static_cast<int>(asset.animation->sequence_count()); ++sequence) {
                            auto name = asset.animation->sequence_name(sequence);
                            std::transform(name.begin(), name.end(), name.begin(),
                                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                            if (name.rfind("reload_", 0) != 0) continue;
                            const auto part = name.substr(7);
                            if (part == "start") start = sequence;
                            else if (part == "end" && end < 0) end = sequence;
                            else if (part == "feed_first") first_feed = sequence;
                            else if (part == "feed" || (!part.empty() && std::isdigit(static_cast<unsigned char>(part[0])))) {
                                steps.push_back(sequence);
                            }
                        }
                        if (start >= 0) asset.reload_chain.push_back(start);
                        if (first_feed >= 0) asset.reload_chain.push_back(first_feed);
                        for (const int step : steps) asset.reload_chain.push_back(step);
                        if (end >= 0) asset.reload_chain.push_back(end);
                    }
                    asset.path = key;
                    {
                        // Most common recorded rate per sequence of this model.
                        std::map<int, std::map<float, int>> votes;
                        for (const auto& recorded : demo->view_model) {
                            const auto recorded_path = demo->model_paths_by_index.find(recorded.model);
                            if (recorded_path == demo->model_paths_by_index.end()
                                || normalize_clothing_model_path(recorded_path->second) != key
                                || recorded.rate <= 0.0f) {
                                continue;
                            }
                            ++votes[recorded.sequence][recorded.rate];
                        }
                        for (const auto& [sequence, rates] : votes) {
                            const auto best = std::max_element(rates.begin(), rates.end(),
                                [](const auto& a, const auto& b) { return a.second < b.second; });
                            if (best->first != 1.0f) asset.sequence_rate[sequence] = best->first;
                        }
                    }
                    const int id = static_cast<int>(outfit->view_models.size());
                    weapon_log += "VIEWMODEL " + path + " bones=" + std::to_string(bones.size())
                        + " idle=" + std::to_string(asset.idle) + " fire=" + std::to_string(asset.fire)
                        + " fire_aimed=" + std::to_string(asset.fire_aimed)
                        + " reload=" + std::to_string(asset.reload)
                        + " draw=" + std::to_string(asset.draw) + " :" + names_log + "\n";
                    outfit->view_model_ids.emplace(key, id);
                    outfit->view_models.push_back(std::move(asset));
                    gmod::source::bsp::StaticProp prop;
                    prop.model = path;
                    prop.actor_kind = 6;
                    prop.actor_index = id;
                    prop.actor_entry = id;
                    prop.actor_palette = static_cast<int>(demo->players.size());
                    actor_props.push_back(std::move(prop));
                } catch (const std::exception& error) {
                    weapon_log += "VIEWMODEL_FAILED " + path + " " + error.what() + "\n";
                }
            }
        }
        const auto parent_count = actor_props.size();
        for (std::size_t i = 0; i < parent_count; ++i) {
            if (actor_props[i].actor_kind != 6) continue;
            const auto parent = actor_props[i];
            std::set<int> ids;
            for (const auto& [weapon,view] : demo->weapon_view_models) {
                if (normalize_clothing_model_path(view) != normalize_clothing_model_path(parent.model)) continue;
                const auto found = observed_attachments.find(weapon);
                if (found != observed_attachments.end()) ids.insert(found->second.begin(),found->second.end());
            }
            for (int id : ids) {
                auto mounted = parent;
                mounted.model = outfit->registry.weapon_attachments.at(id).model;
                mounted.actor_kind = 11;
                mounted.actor_attachment = id;
                mounted.actor_parent_model = parent.model;
                mounted.actor_sequence = outfit->registry.weapon_attachments.at(id).sequence;
                if (const int sight_sequence = outfit->registry.weapon_attachments.at(id).sight_sequence;
                    sight_sequence != mounted.actor_sequence) {
                    auto alternate = mounted;
                    alternate.actor_sequence = sight_sequence;
                    actor_props.push_back(std::move(alternate));
                }
                actor_props.push_back(std::move(mounted));
            }
        }
        std::vector<char> door_taken(entity_props.size(), 0);
        for (std::size_t index = 0; index < demo->doors.size(); ++index) {
            const auto& door = demo->doors[index];
            for (std::size_t candidate = 0; candidate < entity_props.size(); ++candidate) {
                auto& prop = entity_props[candidate];
                if (!prop.is_door || door_taken[candidate]) {
                    continue;
                }
                const float dx = prop.origin[0] - door.origin[0];
                const float dy = prop.origin[1] - door.origin[1];
                const float dz = prop.origin[2] - door.origin[2];
                if (dx * dx + dy * dy + dz * dz <= 1.0f) {
                    door_taken[candidate] = 1;
                    prop.actor_kind = 3;
                    prop.actor_index = static_cast<int>(index);
                    ++matched_doors;
                    break;
                }
            }
        }
    }
    // Only the two bank shutter models use this recorded animation path.
    // Reuse the vehicle asset/palette storage; each instance still owns its palette.
    for (auto& prop : entity_props) {
        if (demo && prop.actor_kind==9 && animator && gmod::demo::is_bank_shutter(prop.model)) {
            const auto& track=demo->props[static_cast<std::size_t>(prop.actor_index)];
            if (!track.animations.empty()) {
                const auto key=normalize_clothing_model_path(prop.model);
                auto known=outfit->vehicle_ids.find(key);
                if (known==outfit->vehicle_ids.end()) {
                    int id=-1;
                    try {
                        const auto status=assets.resolve_model(prop.model);
                        if (status.has_mdl && status.has_vvd && status.has_vtx) {
                            OutfitData::ViewModelAsset asset;
                            auto ani=status.mdl; ani.replace_extension(".ani");
                            asset.animation=std::make_shared<gmod::source::model::AnimationModel>(status.mdl,ani);
                            const auto& bones=asset.animation->bones();
                            if (!bones.empty() && bones.size()<=MaxPaletteBones) {
                                asset.poser=std::make_shared<gmod::source::model::SkeletonPoser>(bones,*asset.animation);
                                for (const auto& bone:bones) asset.names.push_back(bone.name);
                                weapon_log += "BANK_SHUTTER_SEQUENCES model="+prop.model;
                                for(int seq=0;seq<int(asset.animation->sequence_count());++seq)
                                    weapon_log += " "+std::to_string(seq)+"="+asset.animation->sequence_name(seq);
                                weapon_log += "\n";
                                asset.path=key; id=static_cast<int>(outfit->vehicle_assets.size());
                                outfit->vehicle_assets.push_back(std::move(asset));
                            }
                        }
                    } catch (const std::exception& error) {
                        weapon_log += "MAP_ANIMATION_FAILED "+prop.model+" "+error.what()+"\n";
                    }
                    known=outfit->vehicle_ids.emplace(key,id).first;
                }
                if (known->second>=0) {
                    if (prop_data.palette_players>=PaletteMask) throw std::runtime_error("Too many animated map props.");
                    prop.actor_palette=static_cast<int>(prop_data.palette_players++);
                    prop.actor_model=known->second;
                    std::cout << "MAP_ANIMATION model=" << prop.model << " keys=" << track.animations.size() << "\n";
                }
            }
        }
        if (map_scene) {
            if (prop.actor_kind == 0) continue;
            for (auto& instance : prop_data.instances)
                if (instance.map_entity == map_entity_ids.at(&prop)) instance.active = false;
        }
        prop_variants[{prop.model, prop.skin,
                       ((prop.body * 16 + prop.hardware) * 2 + (prop.is_door ? 1 : 0))
                           | (prop.actor_palette>=0 ? (1<<24):0), {}, 0}]
            .push_back(&prop);
    }
    constexpr int VehiclePoseFlag = 1 << 26;
    constexpr int ViewModelFlag = 1 << 25;
    constexpr int VehicleSkinFlag = 1 << 24;
    constexpr int ArmClothingFlag = 1 << 22;
    constexpr int BackWeaponFlag = 1 << 23;
    constexpr int HiddenBadgeFlag = 1 << 21;
    constexpr int VariantFlags = VehiclePoseFlag | ViewModelFlag | VehicleSkinFlag | BackWeaponFlag | ArmClothingFlag | HiddenBadgeFlag;
    for (const auto& prop : actor_props) {
        // The player model keeps only its head visible; the server's clothing data
        // provides every body part (see compose_outfit).
        int body_code = 0;
        if(prop.actor_kind==12) body_code|=BackWeaponFlag;
        if(prop.actor_kind==7) body_code|=ArmClothingFlag;
        if ((prop.actor_kind == 4 || prop.actor_kind == 7)
            && !outfit->parts.at(prop.actor_entry).number_bodygroup.empty()
            && outfit->parts.at(prop.actor_entry).data_number == 0)
            body_code |= HiddenBadgeFlag;
        if (prop.actor_kind == 2 && prop.actor_entry == 1) {
            // A vehicle with its own skeleton is animated live; one without is baked at rest.
            body_code |= prop.actor_palette >= 0 ? VehicleSkinFlag : VehiclePoseFlag;
        } else if (prop.actor_kind == 2 && prop.body > 0) {
            body_code |= prop.body * 16 * 2;  // bodygroup of a placed prop
        }
        // View models are skinned to their own skeleton, never shared with other actors.
        if (prop.actor_kind == 6 || prop.actor_kind == 11) body_code |= ViewModelFlag;
        prop_variants[{prop.model, prop.skin, body_code, prop.actor_parent_model, prop.actor_sequence}].push_back(&prop);
    }
    std::ofstream prop_log(application_root / L"build\\static_prop_import.log");
    prop_log << weapon_log;
    prop_log << "DEMO_CLOTHING_EVENTS=" << (demo ? demo->clothing.size() : 0) << "\n";
    const gmod::source::model::StaticPropMeshImporter prop_importer;
    // The player model has bones the animation skeleton lacks (hat, shades, ...); hats and
    // glasses are bonemerged to them, so they follow the nearest animated ancestor.
    std::vector<std::pair<std::string, std::string>> bone_aliases;
    // Bind pose (model space) of each alias bone on the player model: an accessory authored
    // around its own origin must be moved onto that bone, exactly as bonemerging does.
    std::map<std::string, std::array<float, 12>> alias_binds;
    std::optional<std::array<float, 12>> hand_attachment;
    if (animator && demo) {
        try {
            const int reference_model = fallback_player_model(*demo);
            const auto player_status = reference_model >= 0
                ? assets.resolve_model(demo->models[static_cast<std::size_t>(reference_model)])
                : gmod::source::ModelDependencyStatus{};
            if (player_status.has_mdl) {
                const auto& names = animator->skeleton_names();
                const auto bones = gmod::source::model::read_bones(player_status.mdl);
                for (const auto& bone : bones) {
                    if (bone.name == "valvebiped.anim_attachment_rh") {
                        hand_attachment = rigid_inverse(bone.pose_to_bone);
                    }
                    if (std::find(names.begin(), names.end(), bone.name) != names.end()) continue;
                    int parent = bone.parent;
                    while (parent >= 0) {
                        const auto& candidate = bones[static_cast<std::size_t>(parent)].name;
                        if (std::find(names.begin(), names.end(), candidate) != names.end()) {
                            bone_aliases.emplace_back(bone.name, candidate);
                            alias_binds[bone.name] = rigid_inverse(bone.pose_to_bone);
                            break;
                        }
                        parent = bones[static_cast<std::size_t>(parent)].parent;
                    }
                }
            }
        } catch (const std::exception& error) {
            prop_log << "BONE_ALIASES_FAILED " << error.what() << "\n";
        }
        for (const auto& [from, to] : bone_aliases) prop_log << "BONE_ALIAS " << from << " -> " << to << "\n";
    }
    loading.begin_stage(3, L"Importing static-prop models", prop_variants.size());
    for (const auto& [variant, instances] : prop_variants) {
        const auto& [model_path, variant_skin, variant_code, parent_model_path, attachment_sequence] = variant;
        const int skin = variant_skin & 0xFF;
        const int remap_id = variant_skin >> 8;
        const bool vehicle_pose = (variant_code & VehiclePoseFlag) != 0;
        const int body_code = variant_code & ~VariantFlags;
        const auto clothing_instances = static_cast<std::size_t>(std::count_if(
            instances.begin(), instances.end(),
            [](const auto* prop) { return prop->actor_kind == 4 || prop->actor_kind == 7; }));
        if (clothing_instances > 0) {
            prop_log << "CLOTHING_VARIANT model=" << model_path
                     << " skin=" << skin << " remap=" << remap_id
                     << " instances=" << clothing_instances << "\n";
        }
        gmod::source::model::BodySelection selection;
        if (clothing_instances > 0) {
            const auto& part = outfit->parts.at(instances.front()->actor_entry);
            if (!part.number_bodygroup.empty()) {
                selection.named_overrides.emplace_back(part.number_bodygroup,
                    (variant_code & HiddenBadgeFlag) != 0 ? 1 : 0);
                // Rigid upper-arm badges need clearance over the deforming sleeve.
                selection.bodygroup_normal_offsets.emplace_back(part.number_bodygroup,0.25f);
                prop_log << "UNIFORM_BADGE model=" << model_path << " number=" << part.data_number << "\n";
            }
        }
        if (remap_id > 0) {
            selection.material_remap = &outfit->remaps[static_cast<std::size_t>(remap_id) - 1];
        }
        const bool skinned_variant = animator && std::any_of(
            instances.begin(), instances.end(),
            [](const auto* prop) {
                return prop->actor_kind == 1 || prop->actor_kind == 4 || prop->actor_kind == 5
                    || prop->actor_kind == 7 || prop->actor_kind == 10;
            });
        const bool view_model_variant = (variant_code & ViewModelFlag) != 0;
        const bool vehicle_skinned = (variant_code & VehicleSkinFlag) != 0;
        if (view_model_variant && !instances.empty()) {
            const auto& asset = outfit->view_models[static_cast<std::size_t>(instances.front()->actor_entry)];
            selection.skeleton = &asset.names;
            selection.fallback_bone = asset.names.front();
        } else if (vehicle_skinned && !instances.empty()) {
            const auto& asset = outfit->vehicle_assets[static_cast<std::size_t>(instances.front()->actor_model)];
            selection.skeleton = &asset.names;
            selection.fallback_bone = asset.names.front();
        } else if (skinned_variant) {
            selection.skeleton = &animator->skeleton_names();
            selection.bone_aliases = &bone_aliases;
            selection.fallback_bone = "valvebiped.bip01_r_hand";
        }
        selection.apply_idle_pose = body_code % 2 != 0;
        const int packed_body = body_code / 2;
        // Perpheads civilian models keep the renderable body in Skeleton choice 1;
        // choice 0 is empty, leaving only the head visible.
        selection.body = packed_body / 16;
        // The door hardware lives in bodypart 1 ("handle01") of door models.
        if (packed_body % 16 != 0) {
            selection.overrides.emplace_back(1, packed_body % 16);
        }
        loading.step(model_path);
        const auto status = assets.resolve_model(model_path);
        if (status.has_mdl) {
            ++resolved_mdl;
        }
        if (!status.has_mdl || !status.has_vvd || !status.has_vtx) {
            ++failed_prop_models;
            prop_log << "MISSING_DEPENDENCY model=" << model_path
                     << " mdl=" << status.has_mdl
                     << " vvd=" << status.has_vvd
                     << " vtx=" << status.has_vtx << "\n";
            continue;
        }
        ++resolved_complete;
        std::vector<gmod::source::model::Matrix3x4> baked_pose;
        if (!parent_model_path.empty() && selection.skeleton) {
            try {
                const auto parent = assets.resolve_model(parent_model_path);
                if (!parent.has_mdl) throw std::runtime_error("Attachment parent model is unavailable: "+parent_model_path);
                const auto parent_bones = gmod::source::model::read_bones(parent.mdl);
                const auto child_bones = gmod::source::model::read_bones(status.mdl);
                std::vector<gmod::source::model::Matrix3x4> attachment_pose;
                if (attachment_sequence != 0) {
                    const gmod::source::model::AnimationModel animation(status.mdl, {});
                    if (attachment_sequence >= static_cast<int>(animation.sequence_count()))
                        throw std::runtime_error("Attachment sequence is outside its model.");
                    std::vector<gmod::source::model::BonePose> local_pose;
                    animation.sample(attachment_sequence,
                        std::vector<float>(animation.pose_parameter_count(), 0.0f), 0.0f, local_pose);
                    gmod::source::model::SkeletonPoser(animation.bones(), animation).palette(local_pose, attachment_pose);
                }
                const auto hand = view_model_variant ? std::optional<std::array<float,12>>{}
                    : weapon_hand_correction(*animator,parent.mdl,hand_attachment);
                baked_pose = gmod::source::model::bonemerged_attachment_palette(
                    parent_bones, child_bones, attachment_pose);
                if (hand) {
                    for (auto& transform : baked_pose)
                        transform = rigid_compose(*hand, transform);
                }
                selection.bone_matrices = &baked_pose;
                prop_log << "WEAPON_ATTACHMENT model=" << model_path << " parent=" << parent_model_path
                    << " bones=" << child_bones.size() << " first_person=" << view_model_variant
                    << " sequence=" << attachment_sequence << "\n";
            } catch (const std::exception& error) {
                ++failed_prop_models;
                prop_log << "WEAPON_ATTACHMENT_FAILED model=" << model_path
                    << " parent=" << parent_model_path << " " << error.what() << "\n";
                continue;
            }
        }
        const bool weapon_variant = animator && std::any_of(
            instances.begin(), instances.end(),
            [](const auto* prop) { return prop->actor_kind == 5; });
        if (weapon_variant) {
            try {
                if (const auto correction = weapon_hand_correction(*animator, status.mdl, hand_attachment)) {
                    const auto bone_total = gmod::source::model::read_bones(status.mdl).size();
                    baked_pose.assign(bone_total, *correction);
                    selection.bone_matrices = &baked_pose;
                }
            } catch (const std::exception& error) {
                prop_log << "WEAPON_ATTACH_FAILED model=" << model_path << " " << error.what() << "\n";
                baked_pose.clear();
                selection.bone_matrices = nullptr;
            }
        }
        int muzzle_bone = -1;
        std::array<float,3> muzzle_point{};
        if ((weapon_variant || view_model_variant) && selection.skeleton) {
            const auto attachments = gmod::source::model::read_attachments(status.mdl);
            const auto bones = gmod::source::model::read_bones(status.mdl);
            auto muzzle = std::find_if(attachments.begin(),attachments.end(),
                [](const auto& attachment) { return attachment.name.find("muzzle") != std::string::npos; });
            if (muzzle == attachments.end()) {
                muzzle = std::find_if(attachments.begin(),attachments.end(),
                    [](const auto& attachment) { return attachment.name == "suppressor"; });
            }
            if (muzzle != attachments.end() && muzzle->bone >= 0
                && static_cast<std::size_t>(muzzle->bone) < bones.size()) {
                const auto& bone = bones[static_cast<std::size_t>(muzzle->bone)];
                const auto& names = *selection.skeleton;
                auto target = std::find(names.begin(),names.end(),bone.name);
                if (target == names.end()) target = std::find(names.begin(),names.end(),selection.fallback_bone);
                if (target != names.end()) {
                    muzzle_bone = static_cast<int>(target-names.begin());
                    auto bind = rigid_inverse(bone.pose_to_bone);
                    auto transform = rigid_compose(bind,muzzle->local);
                    if (!baked_pose.empty()) transform = rigid_compose(baked_pose.front(),transform);
                    muzzle_point = {transform[3],transform[7],transform[11]};
                    prop_log << "MUZZLE_ATTACHMENT model=" << model_path << " bone=" << muzzle_bone
                             << " point=" << muzzle_point[0] << "," << muzzle_point[1] << "," << muzzle_point[2] << "\n";
                }
            } else {
                prop_log << "MUZZLE_ATTACHMENT_UNAVAILABLE model=" << model_path << "\n";
            }
        }
        if (clothing_instances > 0 && animator) {
            // How much of the clothing's skeleton the animation drives; a model whose bones
            // match none is drawn in its reference pose.
            try {
                const auto& names = animator->skeleton_names();
                const auto bones = gmod::source::model::read_bones(status.mdl);
                std::size_t matched = 0;
                for (const auto& bone : bones) {
                    if (std::find(names.begin(), names.end(), bone.name) != names.end()) ++matched;
                }
                prop_log << "CLOTHING_BONES model=" << model_path << " matched=" << matched << "/" << bones.size()
                         << (bones.empty() ? "" : " first=" + bones.front().name) << "\n";
            } catch (const std::exception& error) {
                prop_log << "CLOTHING_BONES_FAILED model=" << model_path << " " << error.what() << "\n";
            }
        }
        const bool clothing_variant = animator && !alias_binds.empty() && std::any_of(
            instances.begin(), instances.end(),
            [](const auto* prop) { return prop->actor_kind == 4; });
        if (clothing_variant) {
            try {
                const auto bones = gmod::source::model::read_bones(status.mdl);
                std::vector<gmod::source::model::Matrix3x4> corrections(
                    bones.size(), rigid_compose({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0},
                                                {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}));
                std::optional<gmod::source::model::Matrix3x4> root_correction;
                std::string root_target;
                std::size_t root_anchor = 0;
                for (const auto& bone : bones) {
                    const auto bind = alias_binds.find(bone.name);
                    const auto alias = std::find_if(bone_aliases.begin(),bone_aliases.end(),
                        [&](const auto& value) { return value.first == bone.name; });
                    if (bind == alias_binds.end() || alias == bone_aliases.end()) continue;
                    std::array<float,12> inverse_bind{};
                    std::copy(bone.pose_to_bone,bone.pose_to_bone + 12,inverse_bind.begin());
                    root_correction = rigid_compose(bind->second,inverse_bind);
                    root_target = alias->second;
                    root_anchor = static_cast<std::size_t>(&bone - bones.data());
                    break;
                }
                std::vector<gmod::source::model::Matrix3x4> idle_skin;
                if (root_correction && std::any_of(bones.begin(), bones.end(),
                        [&](const auto& bone) {
                            return bone.parent < 0 && !alias_binds.contains(bone.name)
                                && std::find(selection.skeleton->begin(), selection.skeleton->end(),
                                    bone.name) == selection.skeleton->end();
                        })) {
                    const gmod::source::model::AnimationModel animation(status.mdl, {});
                    if (animation.sequence_count() > 0) {
                        std::vector<gmod::source::model::BonePose> pose;
                        animation.sample(0, std::vector<float>(animation.pose_parameter_count(), 0.0f),
                                         0.0f, pose);
                        const gmod::source::model::SkeletonPoser poser(bones, animation);
                        poser.palette(pose, idle_skin);
                        // Source aligns the first merged bone in the accessory's current
                        // animation, which need not match its authored bind pose.
                        root_correction = rigid_compose(*root_correction,
                            rigid_inverse(idle_skin.at(root_anchor).data()));
                    } else {
                        prop_log << "ACCESSORY_NO_IDLE_SEQUENCE model=" << model_path
                                 << " using bind pose\n";
                    }
                }
                bool any = false;
                for (std::size_t bone = 0; bone < bones.size(); ++bone) {
                    const auto bind = alias_binds.find(bones[bone].name);
                    if (bind == alias_binds.end()) {
                        int parent = static_cast<int>(bone);
                        const auto& names = animator->skeleton_names();
                        while (parent >= 0) {
                            const auto& name = bones[static_cast<std::size_t>(parent)].name;
                            if (alias_binds.contains(name)
                                || std::find(names.begin(),names.end(),name) != names.end()) break;
                            parent = bones[static_cast<std::size_t>(parent)].parent;
                        }
                        // Source aligns unmatched roots through the first merged bone;
                        // an accessory's independent mesh root must not fall back to a hand.
                        if (parent < 0 && root_correction) {
                            corrections[bone] = idle_skin.empty() ? *root_correction
                                : rigid_compose(*root_correction, idle_skin.at(bone));
                            selection.fallback_bone = root_target;
                            any = true;
                            prop_log << "ACCESSORY_ROOT_MERGED model=" << model_path
                                << " bone=" << bones[bone].name << " target=" << root_target << "\n";
                        }
                        continue;
                    }
                    std::array<float, 12> inverse_bind{};
                    std::copy(bones[bone].pose_to_bone, bones[bone].pose_to_bone + 12,
                              inverse_bind.begin());
                    corrections[bone] = rigid_compose(bind->second, inverse_bind);
                    any = true;
                }
                if (any) {
                    baked_pose = std::move(corrections);
                    selection.bone_matrices = &baked_pose;
                }
            } catch (const std::exception& error) {
                prop_log << "ACCESSORY_ATTACH_FAILED model=" << model_path << " " << error.what() << "\n";
                baked_pose.clear();
                selection.bone_matrices = nullptr;
            }
        }
        // A body model authored in its own rest pose (a costume: a stock or Halloween player model
        // set as the clothing data's model override) is re-based onto the animation skeleton's
        // rest pose, so the shared palette (animated pose * skeleton pose_to_bone) skins it the way
        // the engine skins it against its own bones.
        const bool body_variant = animator && !selection.bone_matrices && std::any_of(
            instances.begin(), instances.end(), [](const auto* prop) { return prop->actor_kind == 1; });
        if (body_variant) {
            try {
                const auto bones = gmod::source::model::read_bones(status.mdl);
                const auto& names = animator->skeleton_names();
                const auto& skeleton = animator->skeleton();
                std::vector<gmod::source::model::Matrix3x4> corrections(
                    bones.size(), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});
                bool differs = false;
                for (std::size_t bone = 0; bone < bones.size(); ++bone) {
                    int current = static_cast<int>(bone);
                    int target = -1;
                    while (current >= 0) {
                        const auto found = std::find(names.begin(), names.end(),
                                                     bones[static_cast<std::size_t>(current)].name);
                        if (found != names.end()) {
                            target = static_cast<int>(found - names.begin());
                            break;
                        }
                        current = bones[static_cast<std::size_t>(current)].parent;
                    }
                    if (target < 0) continue;
                    std::array<float, 12> to_bone{};
                    std::copy(bones[static_cast<std::size_t>(current)].pose_to_bone,
                              bones[static_cast<std::size_t>(current)].pose_to_bone + 12, to_bone.begin());
                    corrections[bone] = rigid_compose(
                        rigid_inverse(skeleton[static_cast<std::size_t>(target)].pose_to_bone), to_bone);
                    for (int cell = 0; cell < 12; ++cell) {
                        const float identity = (cell == 0 || cell == 5 || cell == 10) ? 1.0f : 0.0f;
                        if (std::fabs(corrections[bone][cell] - identity) > ((cell % 4 == 3) ? 0.05f : 1e-3f)) differs = true;
                    }
                }
                if (differs) {
                    baked_pose = std::move(corrections);
                    selection.bone_matrices = &baked_pose;
                    prop_log << "BODY_REST_POSE_REBASED model=" << model_path << "\n";
                }
            } catch (const std::exception& error) {
                prop_log << "BODY_REBASE_FAILED model=" << model_path << " " << error.what() << "\n";
                baked_pose.clear();
                selection.bone_matrices = nullptr;
            }
        }
        if (vehicle_pose) {
            try {
                auto animation_ani = status.mdl;
                animation_ani.replace_extension(".ani");
                const gmod::source::model::AnimationModel animation(status.mdl, animation_ani);
                std::vector<float> parameters(animation.pose_parameter_count(), 0.0f);
                for (const char* name : {"vehicle_wheel_fl_height", "vehicle_wheel_fr_height",
                                         "vehicle_wheel_rl_height", "vehicle_wheel_rr_height"}) {
                    const int index = animation.find_pose_parameter(name);
                    if (index >= 0) parameters[static_cast<std::size_t>(index)] = 0.5f;
                }
                std::vector<gmod::source::model::BonePose> pose;
                animation.sample_with_layers(0, parameters, 0.0f, pose);
                const gmod::source::model::SkeletonPoser poser(animation.bones(), animation);
                poser.palette(pose, baked_pose);
                selection.bone_matrices = &baked_pose;
            } catch (const std::exception& error) {
                prop_log << "VEHICLE_POSE_FAILED model=" << model_path << " " << error.what() << "\n";
                baked_pose.clear();
            }
        }
        try {
            gmod::source::model::StaticPropMeshResult imported;
            const gmod::source::model::StaticPropMeshResult* mesh_result = nullptr;
            std::string mesh_cache_key;
            if (cache) {
                mesh_cache_key = CpuAssetCache::mesh_key(model_path, status.mdl, status.vvd, status.vtx, skin, selection, cache);
                const auto found = cache->meshes.find(mesh_cache_key);
                if (found != cache->meshes.end()) {
                    mesh_result = &found->second;
                    ++cache->mesh_hits;
                } else {
                    auto result = prop_importer.read(model_path, status.mdl, status.vvd, status.vtx, skin, selection);
                    mesh_result = &cache->meshes.emplace(mesh_cache_key, std::move(result)).first->second;
                }
            } else {
                imported = prop_importer.read(model_path, status.mdl, status.vvd, status.vtx, skin, selection);
                mesh_result = &imported;
            }
            const auto& mesh = *mesh_result;
            for (const auto* instance : instances) {
                if (instance->actor_kind != 4) continue;
                const auto& part = outfit->parts.at(instance->actor_entry);
                if (part.data_number <= 0) continue;
                const auto& base = outfit->registry.clothing.at(part.type).data_number_material_base;
                const auto digit = std::find_if(mesh.batches.begin(),mesh.batches.end(),[&](const auto& batch) {
                    return batch.vertex_count > 0 && batch.material.starts_with(base);
                });
                if (digit != mesh.batches.end())
                    outfit->badge_probes.emplace(instance->actor_entry,mesh.vertices.at(digit->vertex_offset));
            }
            if (!mesh.vertices.empty()) {
                ++readable_vvd;
            }
            if (mesh.vertices.empty() || mesh.indices.empty()) {
                prop_log << "NO_RENDERABLE_MESH model=" << model_path
                         << " skin=" << skin << "\n";
                continue;
            }
            if (mesh.vertices.size() > static_cast<std::size_t>(INT_MAX)
                    - prop_data.vertices.size()
                || mesh.indices.size() > UINT_MAX - prop_data.indices.size()
                || instances.size() > UINT_MAX - prop_data.transforms.size()) {
                throw std::runtime_error("Static-prop render data exceeds Direct3D limits.");
            }
            for (const auto& batch : mesh.batches) {
                if (batch.vertex_offset > mesh.vertices.size()
                    || batch.vertex_count > mesh.vertices.size() - batch.vertex_offset
                    || batch.index_offset > mesh.indices.size()
                    || batch.index_count > mesh.indices.size() - batch.index_offset) {
                    throw std::runtime_error("Imported prop mesh batch exceeds its buffers.");
                }
            }
            if (!skinned_variant && !view_model_variant && !vehicle_skinned
                && std::any_of(instances.begin(),instances.end(),
                    [](const auto* prop) {
                        return prop->actor_kind == 0 || prop->actor_kind == 2
                            || prop->actor_kind == 3 || prop->actor_kind == 8 || prop->actor_kind == 9;
                    })) {
                const auto cached_trace = cache ? cache->model_traces.find(mesh_cache_key)
                    : std::map<std::string, std::shared_ptr<gmod::source::WorldTrace>>::iterator{};
                if (cache && cached_trace != cache->model_traces.end()) {
                    model_traces[model_path] = cached_trace->second;
                } else {
                    std::vector<gmod::source::bsp::Triangle> surfaces;
                    for (std::size_t i = 0; i+2 < mesh.indices.size(); i+=3) {
                        gmod::source::bsp::Triangle triangle;
                        auto point = [&](std::uint32_t index) {
                            const auto& p = mesh.vertices.at(index).position;
                            return gmod::source::bsp::Vec3{p[0],p[1],p[2]};
                        };
                        triangle.a = point(mesh.indices[i]);
                        triangle.b = point(mesh.indices[i+1]);
                        triangle.c = point(mesh.indices[i+2]);
                        surfaces.push_back(std::move(triangle));
                    }
                    model_traces[model_path] = std::make_shared<gmod::source::WorldTrace>(surfaces,true);
                    if (cache) cache->model_traces.emplace(mesh_cache_key, model_traces.at(model_path));
                }
            }
            // Model-space bounding sphere used for per-instance frustum culling.
            float low[3]{FLT_MAX, FLT_MAX, FLT_MAX};
            float high[3]{-FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (const auto& vertex : mesh.vertices) {
                for (int axis = 0; axis < 3; ++axis) {
                    low[axis] = std::min(low[axis], vertex.position[axis]);
                    high[axis] = std::max(high[axis], vertex.position[axis]);
                }
            }
            const float center[3]{
                (low[0] + high[0]) * 0.5f,
                (low[1] + high[1]) * 0.5f,
                (low[2] + high[2]) * 0.5f};
            float radius_squared = 0.0f;
            for (const auto& vertex : mesh.vertices) {
                const float dx = vertex.position[0] - center[0];
                const float dy = vertex.position[1] - center[1];
                const float dz = vertex.position[2] - center[2];
                radius_squared = std::max(radius_squared, dx * dx + dy * dy + dz * dz);
            }
            const auto radius = std::sqrt(radius_squared)
                + (weapon_variant || view_model_variant ? 130.0f : skinned_variant ? 24.0f : 0.0f);
            const auto base_vertex = static_cast<INT>(prop_data.vertices.size());
            if (skinned_variant && std::any_of(instances.begin(), instances.end(),
                                               [](const auto* prop) { return prop->actor_kind == 1; })) {
                const auto unskinned = std::count_if(mesh.vertices.begin(), mesh.vertices.end(),
                    [](const auto& vertex) { return (vertex.skin_bones >> 24) == 0; });
                prop_log << "PLAYER_MODEL_MESH model=" << model_path << " vertices=" << mesh.vertices.size()
                         << " unskinned=" << unskinned << " body=" << selection.body << "\n";
            }
            const auto base_index = static_cast<UINT>(prop_data.indices.size());
            const auto variant_index = static_cast<UINT>(prop_data.variants.size());
            for (const auto& vertex : mesh.vertices) {
                prop_data.vertices.push_back({
                    {vertex.position[0], vertex.position[1], vertex.position[2]},
                    {vertex.uv[0], vertex.uv[1]}, 0.0f,
                    {vertex.normal[0], vertex.normal[1], vertex.normal[2]}});
                if (animator) {
                    prop_data.skin.push_back({vertex.skin_bones, vertex.skin_weights});
                }
            }
            prop_data.indices.insert(
                prop_data.indices.end(), mesh.indices.begin(), mesh.indices.end());
            prop_data.variants.push_back({
                static_cast<UINT>(prop_data.instances.size()),
                static_cast<UINT>(instances.size()),
                instances.front()->actor_kind == 6 || instances.front()->actor_kind == 7
                    || instances.front()->actor_kind == 11,
                model_traces.contains(model_path) ? model_traces.at(model_path) : nullptr});
            for (const auto* prop : instances) {
                if (prop->actor_kind != 0) {
                    // Pose comes from the demo every frame; nothing static to store.
                    const bool skinned_actor = animator
                        && (prop->actor_kind == 1 || prop->actor_kind == 4
                            || prop->actor_kind == 5 || prop->actor_kind == 6
                            || prop->actor_kind == 7
                            || prop->actor_kind == 10 || prop->actor_kind == 11
                            || ((prop->actor_kind == 2 || prop->actor_kind == 9) && prop->actor_palette >= 0));
                    const int palette = prop->actor_palette >= 0 ? prop->actor_palette : prop->actor_index;
                    if (prop_data.actors.size() > DynamicSlotMask) {
                        throw std::runtime_error("Too many demo actors for the instance id layout.");
                    }
                    PropInstance instance{};
                    instance.dynamic = true;
                    instance.active = false;
                    instance.transform = DynamicBit
                        | (skinned_actor
                               ? (static_cast<UINT>(palette) + 1u) << PaletteShift
                               : 0u)
                        | static_cast<UINT>(prop_data.actors.size());
                    std::copy(center, center + 3, instance.local_center);
                    instance.radius = radius;
                    ActorBinding binding;
                    binding.kind = prop->actor_kind;
                    binding.index = prop->actor_index;
                    binding.palette = skinned_actor ? palette : -1;
                    binding.model = prop->actor_model;
                    binding.entry = prop->actor_entry;
                    binding.skin = skin;
                    binding.female = prop->actor_female;
                    binding.weapon = prop->actor_weapon;
                    binding.attachment = prop->actor_attachment;
                    binding.attachment_sequence = prop->actor_sequence;
                    binding.model_path = prop->model;
                    binding.initial_hidden = prop->start_hidden;
                    std::copy(prop->origin, prop->origin + 3, binding.origin);
                    std::copy(prop->angles, prop->angles + 3, binding.angles);
                    binding.door_distance = prop->door_distance;
                    binding.door_speed = prop->door_speed;
                    binding.open_dir = prop->open_dir;
                    binding.muzzle_bone = muzzle_bone;
                    binding.muzzle_point = muzzle_point;
                    prop_data.actors.push_back(binding);
                    prop_data.instances.push_back(instance);
                    continue;
                }
                const auto transform = prop_transform(*prop);
                PropInstance instance{};
                const auto entity_id = map_entity_ids.find(prop);
                if (entity_id != map_entity_ids.end()) instance.map_entity = entity_id->second;
                instance.transform = static_cast<UINT>(prop_data.transforms.size());
                transform_point(transform, center, instance.center);
                instance.radius = radius;
                std::copy(center,center+3,instance.local_center);
                instance.origin[0] = transform.values[12];
                instance.origin[1] = transform.values[13];
                instance.origin[2] = transform.values[14];
                instance.fade_max = prop->fade_max;
                prop_data.transforms.push_back(transform);
                prop_data.instances.push_back(instance);
                ambient_points.push_back({prop->origin[0], prop->origin[1], prop->origin[2]});
            }
            for (const auto& batch : mesh.batches) {
                auto material_name = batch.material;
                bool material_vmt_found = false;
                for (const auto& candidate : batch.material_candidates) {
                    if (!find_source_file(
                            "materials/" + candidate + ".vmt").empty()) {
                        material_name = candidate;
                        material_vmt_found = true;
                        break;
                    }
                }
                if (!material_vmt_found) {
                    prop_log << "UNRESOLVED_MATERIAL model=" << model_path
                             << " skin=" << skin << " name=" << batch.material
                             << " candidates=";
                    for (std::size_t index = 0;
                         index < batch.material_candidates.size(); ++index) {
                        if (index > 0) {
                            prop_log << "|";
                        }
                        prop_log << batch.material_candidates[index];
                    }
                    prop_log << "\n";
                }
                if (!material_name.empty()) {
                    world_materials.insert(material_name);
                    prop_material_names.insert(material_name);
                } else {
                    prop_log << "MISSING_MATERIAL_NAME model=" << model_path
                             << " skin=" << skin << "\n";
                }
                if (batch.index_count == 0) {
                    continue;
                }
                PropDraw draw{};
                draw.material = material_name;
                draw.index_offset = base_index + static_cast<UINT>(batch.index_offset);
                draw.index_count = static_cast<UINT>(batch.index_count);
                draw.base_vertex = base_vertex;
                draw.variant = variant_index;
                prop_data.batches.push_back(std::move(draw));
            }
            rendered_prop_instances += instances.size();
            rendered_clothing_instances += clothing_instances;
        } catch (const std::exception& error) {
            ++failed_prop_models;
            prop_log << "IMPORT_ERROR model=" << model_path
                     << " skin=" << skin << ": " << error.what() << "\n";
        }
    }
    prop_log << "SUMMARY instances=" << static_props.props().size()
             << " model_skin_variants=" << prop_variants.size()
             << " mdl=" << resolved_mdl
             << " complete=" << resolved_complete
             << " readable=" << readable_vvd
             << " clothing_instances=" << rendered_clothing_instances
             << " rendered_instances=" << rendered_prop_instances
             << " failed_variants=" << failed_prop_models
             << " unique_vertices=" << prop_data.vertices.size()
             << " indices=" << prop_data.indices.size()
             << " instance_transforms=" << prop_data.transforms.size() - 1
             << " fading_instances=" << std::count_if(
                    prop_data.instances.begin(), prop_data.instances.end(),
                    [](const PropInstance& instance) { return instance.fade_max > 0.0f; })
             << " draw_batches=" << prop_data.batches.size() << "\n";
    prop_log.close();
    // Static props are lit from the baked leaf ambient cube at their origin.
    if (!map_scene) {
        prop_data.ambient.resize(ambient_points.size());
        const auto cubes = bsp.ambient_cubes(ambient_points);
        std::size_t missing_ambient = 0;
        for (std::size_t index = 1; index < cubes.size(); ++index) {
            auto& gpu = prop_data.ambient[index];
            gpu.color[0][3] = cubes[index].sun_visible ? 1.0f : 0.0f;
            if (!cubes[index].found) {
                ++missing_ambient;
            }
            for (int face = 0; face < 6; ++face) {
                for (int channel = 0; channel < 3; ++channel) {
                    // Props outside any sampled leaf stay unlit rather than black.
                    gpu.color[face][channel] = cubes[index].found
                        ? cubes[index].color[face][channel] : 1.0f;
                }
            }
        }
        for (auto& instance : prop_data.instances) {
            if (!instance.dynamic) {
                instance.sky = cubes[instance.transform].in_sky_3d;
            }
        }
        std::wcout << L"Static-prop ambient cubes: found="
                   << cubes.size() - 1 - missing_ambient
                   << L" missing=" << missing_ambient << L"\n";
        std::ofstream lighting_log(application_root / L"build\\lighting.log");
        if (bsp.sun_light()) {
            const auto& sun = *bsp.sun_light();
            lighting_log << "sun color=(" << sun.color[0] << "," << sun.color[1] << ","
                         << sun.color[2] << ") dir=(" << sun.direction[0] << ","
                         << sun.direction[1] << "," << sun.direction[2] << ")\n";
        }
        lighting_log << "lightmap_faces=" << summary.lightmap_faces
                     << " failures=" << summary.lightmap_failures
                     << " mean=" << summary.lightmap_mean
                     << " max=" << summary.lightmap_max
                     << " ambient_found=" << cubes.size() - 1 - missing_ambient
                     << " ambient_missing=" << missing_ambient
                     << " sun_visible=" << std::count_if(cubes.begin() + 1, cubes.end(),
                            [](const auto& cube) { return cube.sun_visible; })
                     << " dark_with_sun=" << std::count_if(cubes.begin() + 1, cubes.end(),
                            [](const auto& cube) {
                                float peak = 0.0f;
                                for (const auto& face : cube.color) {
                                    peak = std::max({peak, face[0], face[1], face[2]});
                                }
                                return cube.found && cube.sun_visible && peak < 0.05f;
                            })
                     << " dark_ambient=" << std::count_if(cubes.begin() + 1, cubes.end(),
                            [](const auto& cube) {
                                float peak = 0.0f;
                                for (const auto& face : cube.color) {
                                    peak = std::max({peak, face[0], face[1], face[2]});
                                }
                                return cube.found && peak < 0.05f;
                            })
                     << "\n";
        std::size_t dark_samples_logged = 0;
        for (std::size_t index = 1; index < cubes.size() && dark_samples_logged < 8; ++index) {
            float peak = 0.0f;
            for (const auto& face : cubes[index].color) {
                peak = std::max({peak, face[0], face[1], face[2]});
            }
            if (cubes[index].found && cubes[index].sun_visible && peak < 0.05f) {
                ++dark_samples_logged;
                lighting_log << "dark prop origin=(" << ambient_points[index].x << ","
                             << ambient_points[index].y << "," << ambient_points[index].z
                             << ") peak=" << peak << "\n";
            }
        }
        for (std::size_t index = 1; index < cubes.size() && index <= 3; ++index) {
            lighting_log << "cube " << index << ":";
            for (int face = 0; face < 6; ++face) {
                lighting_log << " (" << cubes[index].color[face][0] << ","
                             << cubes[index].color[face][1] << ","
                             << cubes[index].color[face][2] << ")";
            }
            lighting_log << "\n";
        }
    }
    std::size_t missing_vmt = 0;
    std::size_t no_base_texture = 0;
    std::size_t png_fallbacks = 0;
    auto material_surfaces = map_scene ? map_scene->material_surfaces
        : std::unordered_map<std::string, std::string>{};
    std::ofstream resolution_log(application_root / L"build\\material_resolution.log");
    if (demo) {
        world_materials.insert("effects/perp2_flame");
        world_materials.insert("effects/muzzleflash1");
        world_materials.insert("effects/blood_core");
        world_materials.insert("particle/particle_smokegrenade");
        for (const auto& effect : demo->effects) {
            if (effect.kind == "decal") world_materials.insert(effect.name);
        }
    }
    loading.begin_stage(4, L"Resolving materials (VMT/VTF)", world_materials.size());
    for (const auto& material : world_materials) {
        loading.step(material);
        if (map_scene && std::any_of(material_textures.begin(), material_textures.end(),
            [&](const auto& item) { return item.material == material; })) continue;
        WorldMaterialTexture resolved{material};
        resolved.prop_material = prop_material_names.contains(material);
        gmod::source::vmt::Material parsed;
        std::string vmt_name = material;
        if (find_source_file("materials/" + vmt_name + ".vmt").empty()) {
            const auto original = unpatched_name(material);
            if (!original.empty()) {
                vmt_name = original;
            }
        }
        try {
            parsed = load_vmt("materials/" + vmt_name + ".vmt", 0);
            if (parsed.shader == "subrect" && !parsed.model_material.empty()) {
                const float scale = parsed.decal_scale;
                parsed = load_vmt("materials/" + parsed.model_material + ".vmt", 0);
                parsed.decal_scale = scale;
            }
        } catch (const std::exception& error) {
            resolution_log << "PARSE_ERROR " << material << ": " << error.what() << "\n";
        }
        material_surfaces[material] = parsed.surface_prop;
        resolved.decal_scale = parsed.decal_scale;
        resolved.alpha_mode = parsed.additive ? 3u
            : parsed.translucent ? 2u
            : parsed.alpha_test ? 1u
            : 0u;
        if (parsed.blend_tint_by_base_alpha) resolved.material_flags |= 16u;
        {
            // TDM cars share one white paint texture; the server tints it with the car's colour.
            std::string lowered = parsed.base_texture;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::replace(lowered.begin(), lowered.end(), '\\', '/');
            if (lowered.find("tdmcars/shared/skin") != std::string::npos
                && lowered.find("skin_") == std::string::npos) {
                resolved.material_flags |= 32u;
            }
            const std::string plate_glyph = "models/jarheads/vehicles/licenseplatesskinned/";
            if (material.starts_with(plate_glyph)
                && material.find('/', plate_glyph.size()) == std::string::npos
                && material.substr(plate_glyph.size()) != "plate")
                resolved.material_flags |= 32u;
        }
        if (parsed.alpha_test_reference > 0.0f) {
            resolved.alpha_ref = parsed.alpha_test_reference;
        }
        const bool water_surface = parsed.shader == "water" && !resolved.prop_material;
        if (!parsed.water_above) resolved.material_flags |= 128u;
        if (water_surface) {
            resolved.material_flags |= 4u;
            resolved.alpha_mode = 2u;
        }
        if (water_surface && !parsed.normal_map.empty()) {
            const auto normal = find_source_file("materials/" + parsed.normal_map + ".vtf");
            if (!normal.empty()) {
                parsed.base_texture = parsed.normal_map;
                resolved.material_flags |= 256u;
            }
        }
        if (find_source_file("materials/" + vmt_name + ".vmt").empty()) {
            ++missing_vmt;
            resolved.neutral_placeholder = resolved.prop_material;
            resolution_log << "MISSING_VMT " << material << "\n";
        } else if (parsed.base_texture.empty()) {
            ++no_base_texture;
            const bool is_water = parsed.shader == "water";
            const bool is_refract = parsed.shader.rfind("refract", 0) == 0;
            resolved.water_placeholder = !resolved.prop_material && is_water;
            resolved.neutral_placeholder =
                resolved.prop_material || !resolved.water_placeholder;
            // Water uses a normal-map fallback when needed; frosted glass still
            // uses its procedural translucent approximation.
            if (is_water) {
                resolved.material_flags |= 4u;
                resolved.alpha_mode = 2u;
            } else if (is_refract) {
                resolved.material_flags |= 8u;
                resolved.alpha_mode = 2u;
            }
            resolution_log << "NO_BASETEXTURE shader=" << parsed.shader
                           << " include=" << parsed.include_path
                           << " material=" << material << "\n";
        } else {
            ++resolved_vmts;
            const auto vtf = find_source_file(
                "materials/" + parsed.base_texture + ".vtf");
            if (!vtf.empty()) {
                ++resolved_vtfs;
                resolution_log << "VTF_PATH " << vtf.string() << "\n";
                // Decode is deliberately deferred until the render loop.
                resolved.vtf = vtf;
                resolved.decodable = true;
                try {
                    gmod::source::vtf::Texture header;
                    if (cache) {
                        const auto key = cache->source_key(vtf);
                        const auto found = cache->texture_headers.find(key);
                        if (found != cache->texture_headers.end()) {
                            header = found->second;
                            ++cache->texture_header_hits;
                        } else {
                            header = gmod::source::vtf::VtfReader(vtf).read();
                            cache->texture_headers.emplace(key, header);
                        }
                    } else {
                        header = gmod::source::vtf::VtfReader(vtf).read();
                    }
                    resolved.mapping_size[0] = static_cast<float>(header.width);
                    resolved.mapping_size[1] = static_cast<float>(header.height);
                } catch (const std::exception& error) {
                    resolution_log << "VTF_HEADER_ERROR " << vtf.string()
                                   << ": " << error.what() << "\n";
                }
                if (parsed.shader == "worldvertextransition"
                    && !parsed.base_texture2.empty()) {
                    resolved.vtf2 = find_source_file(
                        "materials/" + parsed.base_texture2 + ".vtf");
                    if (!resolved.vtf2.empty()) {
                        resolved.material_flags |= 1u;
                        if (!parsed.blend_modulate_texture.empty()) {
                            resolved.modulate = find_source_file(
                                "materials/" + parsed.blend_modulate_texture + ".vtf");
                            if (!resolved.modulate.empty()) {
                                resolved.material_flags |= 2u;
                            } else {
                                resolution_log << "NO_VTF blendmodulatetexture="
                                               << parsed.blend_modulate_texture
                                               << " material=" << material << "\n";
                            }
                        }
                    } else {
                        resolution_log << "NO_VTF basetexture2=" << parsed.base_texture2
                                       << " material=" << material << "\n";
                    }
                }
            } else {
                auto png_name = parsed.base_texture;
                std::replace(png_name.begin(), png_name.end(), '\\', '/');
                const auto png = application_root.parent_path()
                    / L"gmod-replay\\assets\\source-cache\\rp_paralake_city_v6.3"
                    / "materials" / std::filesystem::path(png_name + ".png");
                if (std::filesystem::exists(png)) {
                    resolved.png = png;
                    ++png_fallbacks;
                } else {
                    resolved.neutral_placeholder = resolved.prop_material;
                    resolution_log << "NO_VTF basetexture=" << parsed.base_texture
                                   << " material=" << material << "\n";
                }
            }
        }
        // Keep one descriptor and one batch slot for every BSP material,
        // including materials with missing or malformed source assets.
        material_textures.push_back(std::move(resolved));
    }
    resolution_log << "DISPLACEMENTS faces=" << summary.displacement_count
                   << " triangles=" << summary.displacement_triangle_count
                   << " failures=" << summary.displacement_failures
                   << " blend_materials=" << std::count_if(
                          material_textures.begin(), material_textures.end(),
                          [](const WorldMaterialTexture& item) {
                              return (item.material_flags & 1u) != 0;
                          })
                   << "\n";
    resolution_log << "SUMMARY total=" << world_materials.size()
                   << " missing_vmt=" << missing_vmt
                   << " no_basetexture=" << no_base_texture
                   << " raw_vtf=" << resolved_vtfs
                   << " png_fallback=" << png_fallbacks << "\n";
    resolution_log.close();
    std::wcout << L"BSP v" << summary.version
               << L": vertices=" << summary.vertex_count
               << L" edges=" << summary.edge_count
               << L" surfedges=" << summary.surfedge_count
               << L" faces=" << summary.face_count
               << L" world_faces=" << summary.world_face_count
               << L" triangles=" << summary.triangle_count
               << L" displacements=" << summary.displacement_count
               << L" displacement_triangles=" << summary.displacement_triangle_count
               << L" displacement_failures=" << summary.displacement_failures << L"\n";
    std::wcout << L"Lightmaps: faces=" << summary.lightmap_faces
               << L" failures=" << summary.lightmap_failures
               << L" mean=" << summary.lightmap_mean
               << L" max=" << summary.lightmap_max << L"\n";
    std::wcout << L"Entities: model_entities=" << bsp.entity_stats().model_entities
               << L" brush_entities=" << bsp.entity_stats().brush_entities
               << L" brush_faces=" << bsp.entity_stats().brush_faces
               << L" hidden_by_flags=" << bsp.entity_stats().skipped_hidden
               << L" scaled_models_unsupported=" << bsp.entity_stats().scaled_models << L"\n";
    std::wcout << L"3D skybox: faces=" << summary.sky_3d_faces
               << L" hidden_faces=" << summary.hidden_faces
               << L" camera=" << (bsp.sky_camera() ? L"yes" : L"no") << L"\n";
    std::wcout << L"World materials: " << summary.material_count
               << L" VMTs with base textures: " << resolved_vmts
               << L" VTFs found: " << resolved_vtfs
               << L" (VTF decode streamed during rendering)\n";
    std::wcout << L"Static props: models=" << prop_summary.model_count
               << L" instances=" << prop_summary.prop_count
               << L" invalid_model_refs=" << prop_summary.invalid_model_references
               << L"\n";
    std::wcout << L"Static-prop dependencies: mdl=" << resolved_mdl
               << L" complete_mdl_vvd_vtx=" << resolved_complete
               << L" readable_vvd=" << readable_vvd
               << L" rendered_instances=" << rendered_prop_instances
               << L"\n";
    std::wcout << L"Static-prop variants failed: " << failed_prop_models
               << L" shared_vertices=" << prop_data.vertices.size()
               << L" instances=" << rendered_prop_instances << L"\n";
    if (!map_scene) for (const auto& [brush,index] : moving_brushes) {
        if (brush->triangles.empty()) continue;
        if (prop_data.actors.size() > DynamicSlotMask)
            throw std::runtime_error("Too many demo brush actors.");
        const auto variant = static_cast<UINT>(prop_data.variants.size());
        prop_data.variants.push_back({static_cast<UINT>(prop_data.instances.size()),1});
        PropInstance instance{};
        instance.dynamic = true;
        instance.active = false;
        instance.transform = DynamicBit | static_cast<UINT>(prop_data.actors.size());
        ActorBinding actor;
        actor.kind = 8;
        actor.index = preparing_map ? -1 : static_cast<int>(index);
        actor.model_path = brush->model;
        actor.origin[0] = brush->origin.x; actor.origin[1] = brush->origin.y; actor.origin[2] = brush->origin.z;
        actor.angles[0] = brush->angles.x; actor.angles[1] = brush->angles.y; actor.angles[2] = brush->angles.z;
        prop_data.actors.push_back(actor);
        std::map<std::string,std::vector<const gmod::source::bsp::Triangle*>> materials;
        for (const auto& triangle : brush->triangles) {
            materials[triangle.material].push_back(&triangle);
            for (const auto& point : {triangle.a,triangle.b,triangle.c})
                instance.radius = std::max(instance.radius,std::sqrt(point.x*point.x+point.y*point.y+point.z*point.z));
        }
        prop_data.instances.push_back(instance);
        model_traces[brush->model] = std::make_shared<gmod::source::WorldTrace>(brush->triangles,true);
        prop_data.variants[variant].camera_trace = model_traces[brush->model];
        for (const auto& [material,triangles] : materials) {
            const auto texture = std::find_if(material_textures.begin(),material_textures.end(),
                [&](const auto& value) { return value.material == material; });
            PropDraw draw{};
            draw.material = material;
            draw.variant = variant;
            draw.base_vertex = static_cast<INT>(prop_data.vertices.size());
            draw.index_offset = static_cast<UINT>(prop_data.indices.size());
            UINT local_index = 0;
            for (const auto* triangle : triangles) {
                const float ab[3]{triangle->b.x-triangle->a.x,triangle->b.y-triangle->a.y,triangle->b.z-triangle->a.z};
                const float ac[3]{triangle->c.x-triangle->a.x,triangle->c.y-triangle->a.y,triangle->c.z-triangle->a.z};
                float normal[3]{ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0]};
                const float length = std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
                if (length <= 0.00001f) continue;
                for (float& value : normal) value /= length;
                float uv_scale[2]{1,1};
                if (texture != material_textures.end()) {
                    for (int axis = 0; axis < 2; ++axis)
                        if (texture->mapping_size[axis] > 0)
                            uv_scale[axis] = triangle->texdata_size[axis] / texture->mapping_size[axis];
                }
                const gmod::source::bsp::Vec3 points[]{triangle->a,triangle->b,triangle->c};
                for (int corner = 0; corner < 3; ++corner) {
                    const auto& point = points[corner];
                    prop_data.vertices.push_back({{point.x,point.y,point.z},
                        {triangle->uv[corner][0]*uv_scale[0],triangle->uv[corner][1]*uv_scale[1]},
                        triangle->blend[corner]/255.0f,{normal[0],normal[1],normal[2]}});
                    prop_data.indices.push_back(local_index++);
                    if (animator) prop_data.skin.push_back({});
                }
            }
            draw.index_count = local_index;
            if (draw.index_count > 0) prop_data.batches.push_back(std::move(draw));
        }
        std::cout << "BRUSH_ACTOR model=" << brush->model << " target=" << brush->targetname
            << " entity=" << (preparing_map ? -1 : brush_demo->props[index].entity)
            << " triangles=" << brush->triangles.size() << "\n";
    }
    LoadedScene scene;
    if (map_scene && demo) {
        for (auto& actor : prop_data.actors) {
            if (actor.kind != 8) continue;
            const auto found = std::find_if(demo->props.begin(), demo->props.end(),
                [&](const auto& prop) {
                    return prop.brush && prop.model >= 0
                        && demo->models.at(prop.model) == actor.model_path;
                });
            actor.index = found == demo->props.end() ? -1 : static_cast<int>(found - demo->props.begin());
            if (actor.index >= 0) scene.moving_map_brushes.insert(std::stoi(actor.model_path.substr(1)));
        }
    }
    scene.content_roots = content.roots;
    if (demo || preparing_map) {
        // Footsteps and gunfire: the floor under a foot (built before the triangles move
        // out), the step sound set of each surface, and the available sound files.
        const auto audio_triangles = map_scene
            ? collision_triangles(world_triangles, scene.moving_map_brushes) : std::vector<gmod::source::bsp::Triangle>{};
        scene.demo_audio = map_scene ? map_scene->demo_audio : std::make_shared<DemoAudioAssets>(DemoAudioAssets{
            gmod::source::GroundMap(
                world_triangles,
                [&](const std::string& material) {
                    const auto found = material_surfaces.find(material);
                    return found == material_surfaces.end() ? std::string() : found->second;
                }),
            gmod::source::SurfaceSounds(find_source_file("scripts/surfaceproperties.txt")),
            gmod::audio::SoundCatalog(content_roots),
            gmod::source::WorldTrace(world_triangles),
            gmod::source::WorldTrace(world_triangles,true),std::move(model_traces)});
        if (demo) {
            // Which of the sounds the server played exist in the installed content.
            std::map<std::string, int> counts;
            for (const auto& sound : demo->sounds) ++counts[sound.name];
            std::ofstream world_log(application_root / L"build\\world_sounds.log");
            std::size_t found_events = 0, missing_events = 0;
            for (const auto& [name, count] : counts) {
                const auto first = name.find_first_not_of("*#@<>^()}$!?");
                const std::string file = first == std::string::npos ? name : name.substr(first);
                bool found = false;
                for (const auto& root : content_roots) {
                    if (gmod::source::vfs::exists(root / L"sound" / std::filesystem::path(file))) {
                        found = true;
                        break;
                    }
                }
                (found ? found_events : missing_events) += static_cast<std::size_t>(count);
                world_log << (found ? "OK      " : "MISSING ") << count << " " << name << "\n";
            }
            world_log << "TOTAL ok=" << found_events << " missing=" << missing_events << "\n";
        }
        if (map_scene) {
            scene.demo_audio = std::make_shared<DemoAudioAssets>(*map_scene->demo_audio);
            scene.demo_audio->ground = gmod::source::GroundMap(audio_triangles,
                [&](const std::string& material) {
                    const auto found = material_surfaces.find(material);
                    return found == material_surfaces.end() ? std::string() : found->second;
                });
            scene.demo_audio->trace = gmod::source::WorldTrace(audio_triangles);
            scene.demo_audio->decal_trace = gmod::source::WorldTrace(audio_triangles, true);
            for (auto& [model, trace] : model_traces) scene.demo_audio->model_traces[model] = std::move(trace);
        }
        std::ofstream sound_log(application_root / L"build\\sound_resolution.log");
        if (demo) for (const auto& [weapon_class, view_model] : demo->weapon_view_models) {
            const auto key = normalize_clothing_model_path(view_model);
            const auto sound = outfit->registry.swep_fire_sounds.find(key);
            if (sound == outfit->registry.swep_fire_sounds.end()) {
                sound_log << "NO_SWEP_SOUND class=" << weapon_class << "\n";
                continue;
            }
            std::string via;
            const auto files = resolve_fire_sounds(outfit->registry, scene.demo_audio->catalog, key, &via);
            sound_log << (files.empty() ? "NO_FIRE_FILES" : via.empty() ? "FIRE_OK" : "FIRE_FALLBACK")
                      << " class=" << weapon_class << " sound=" << sound->second
                      << (via.empty() ? std::string() : " via=" + via)
                      << (files.empty() ? std::string() : " file=" + files.front()) << "\n";
        }
    }
    scene.triangles = std::move(world_triangles);
    scene.lightmap = map_scene ? map_scene->lightmap : bsp.release_lightmap_atlas();
    scene.probe = map_scene ? map_scene->probe : std::optional(bsp.make_probe(true));
    if (demo || preparing_map) {
        if (demo)
        std::wcout << L"Demo actors: " << prop_data.actors.size() << L" instances, "
                   << matched_doors << L" of " << demo->doors.size()
                   << L" demo doors matched to map doors\n";
    }
    scene.sky_camera = bsp.sky_camera();
    scene.sky_paint = bsp.sky_paint();
    scene.sun = bsp.sun_light();
    try {
        scene.audio = map_scene ? map_scene->audio : bsp.audio_scene();
    } catch (const std::exception& error) {
        std::wcout << L"Audio entities unavailable: " << error.what() << L"\n";
    }
    scene.player_spawn = player_spawn;
    scene.material_textures = std::move(material_textures);
    scene.prop_data = std::move(prop_data);
    scene.animator = animator;
    scene.outfit = outfit;
    scene.material_surfaces = std::move(material_surfaces);
    {
        std::ofstream animation_log(application_root / L"build\\animation.log");
        animation_log << "animator=" << animator_log << "\n";
    }
    return scene;
}

} // namespace

// Saves the window's client area as a BMP (used to check the GDI screens).
void save_window_bmp(HWND window, const std::filesystem::path& path,
                     const std::function<void(HDC)>& draw = {}) {
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HDC source = GetDC(window);
    HDC dc = CreateCompatibleDC(source);
    HBITMAP bitmap = CreateCompatibleBitmap(source, width, height);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    // A hidden window has nothing on screen to copy, so GDI content is drawn straight in.
    if (draw) draw(dc);
    else BitBlt(dc, 0, 0, width, height, source, 0, 0, SRCCOPY);
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = width;
    header.biHeight = height;
    header.biPlanes = 1;
    header.biBitCount = 24;
    header.biCompression = BI_RGB;
    const int stride = (width * 3 + 3) & ~3;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height);
    GetDIBits(dc, bitmap, 0, height, pixels.data(), reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(header);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(window, source);
}

// Draws name tags onto a screenshot written by the renderer (the overlay window is not
// part of the swap chain).
void overlay_on_bmp(const std::filesystem::path& path,
                    const std::function<void(HDC, int, int)>& draw) {
    HBITMAP bitmap = static_cast<HBITMAP>(LoadImageW(
        nullptr, path.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION));
    if (!bitmap) return;
    BITMAP info{};
    GetObjectW(bitmap, sizeof(info), &info);
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    draw(dc, info.bmWidth, info.bmHeight);
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = info.bmWidth;
    header.biHeight = info.bmHeight;
    header.biPlanes = 1;
    header.biBitCount = 24;
    header.biCompression = BI_RGB;
    const int stride = (info.bmWidth * 3 + 3) & ~3;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * info.bmHeight);
    GetDIBits(dc, bitmap, 0, info.bmHeight, pixels.data(),
              reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS);
    BITMAPFILEHEADER file{};
    file.bfType = 0x4D42;
    file.bfOffBits = sizeof(file) + sizeof(header);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    SelectObject(dc, previous);
    DeleteDC(dc);
    DeleteObject(bitmap);
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    g_headless = wcsstr(GetCommandLineW(), L"--screenshot") != nullptr
        || wcsstr(GetCommandLineW(), L"--menu-screenshot") != nullptr
        || wcsstr(GetCommandLineW(), L"--menu-return-test") != nullptr;
    if (g_headless) SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    if (!g_headless) {
        // Inno Setup checks this mutex before replacing a running viewer.
        if (!CreateMutexW(nullptr, FALSE, gmod::edition::app_mutex))
            return static_cast<int>(GetLastError());
    }
    {
        wchar_t module_path[MAX_PATH]{};
        const auto length = GetModuleFileNameW(nullptr, module_path, static_cast<DWORD>(std::size(module_path)));
        if (length > 0 && length < std::size(module_path)) {
            capture_program_output(std::filesystem::path(module_path).parent_path() / L"build\\console.log");
        }
    }
    const wchar_t class_name[] = L"GmodReplayApplication";
    const auto app_icon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_GMOD_REPLAY), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR | LR_SHARED));
    const auto app_small_icon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_GMOD_REPLAY), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED));
    if (!app_icon || !app_small_icon) {
        return static_cast<int>(GetLastError());
    }
    WNDCLASSW window_class{};
    window_class.hInstance = instance;
    window_class.hIcon = app_icon;
    window_class.lpfnWndProc = window_proc;
    window_class.lpszClassName = class_name;
    window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&window_class)) {
        return static_cast<int>(GetLastError());
    }
    WNDCLASSW surface_class{};
    surface_class.hInstance = instance;
    surface_class.lpfnWndProc = surface_proc;
    surface_class.lpszClassName = L"GmodReplaySurface";
    surface_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&surface_class)) {
        return static_cast<int>(GetLastError());
    }
    const bool hidden_window = g_headless;
    HWND window = CreateWindowExW(
        0, class_name, L"GMod Replay Native Application",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | (hidden_window ? 0 : WS_VISIBLE),
        CW_USEDEFAULT, CW_USEDEFAULT, WindowWidth, WindowHeight,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        return static_cast<int>(GetLastError());
    }
    SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(app_icon));
    SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(app_small_icon));

    try {
        wchar_t executable_path[MAX_PATH]{};
        const auto executable_length = GetModuleFileNameW(
            nullptr, executable_path, static_cast<DWORD>(std::size(executable_path)));
        if (executable_length == 0 || executable_length >= std::size(executable_path)) {
            throw std::runtime_error("Could not determine the application directory.");
        }
        const auto application_root =
            std::filesystem::path(executable_path).parent_path();
        int argument_count = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
        std::wstring screenshot_path;
        bool vsync = true;
        bool reload_test = false;
        bool gpu_cache_test = false;
        bool water_render_test = false;
        bool graphics_render_test = false;
        bool bank_baked_test = false;
        bool fist_impact_test = false;
        bool melee_render_test = false;
        bool back_weapons_test = false;
        bool identifiers_render_test = false;
        bool fire_render_test = false;
        bool bank_shutters_test = false;
        bool session_cache_test = false;
        int session_cache_phase = 0;
        std::optional<std::filesystem::path> initial_test_demo;
        bool retained_state_test = false;
        bool retained_replace_test = false;
        std::optional<std::filesystem::path> reload_demo;
        // Test: open the scene interactively (muted), go back to the menu after this many
        // seconds, then log that the menu came up and quit.
        float menu_return_after = 0.0f;
        float close_after = 0.0f;
        std::optional<std::array<float, 5>> camera_override;
        std::optional<std::filesystem::path> demo_path;
        float start_time = 0.0f;
        int follow_index = -1;
        bool follow_car = false;
        int card_index = -1;
        int click_x = -1;
        bool double_click = false;
        bool preview_menu = false;
        bool preview_help = false;
        std::filesystem::path sound_log_path;
        bool world_sound_reset_test = false;
        std::wstring preview_console;
        bool preview_console_on = false;
        int click_y = -1;
        float advance_seconds = 0.0f;
        bool show_timeline = false;
        bool free_roam = false;
        bool recorder_pov = false;
        bool pin_player_list = false;
        bool hide_vehicles = false;
        std::wstring menu_screenshot;
        std::wstring menu_keys;
        std::wstring load_cancel_test;
        int load_cancel_test_stage = 1;
        bool load_cancel_test_completed = false;
        for (int index = 1; arguments && index < argument_count; ++index) {
            const std::wstring argument = arguments[index];
            if (argument == L"--screenshot" && index + 1 < argument_count) {
                screenshot_path = arguments[++index];
            } else if (argument == L"--demo" && index + 1 < argument_count) {
                demo_path = std::filesystem::path(arguments[++index]);
            } else if (argument == L"--time" && index + 1 < argument_count) {
                start_time = std::stof(arguments[++index]);
            } else if (argument == L"--freeroam") {
                free_roam = true;
            } else if (argument == L"--follow" && index + 1 < argument_count) {
                follow_index = std::stoi(arguments[++index]);
            } else if (argument == L"--card" && index + 1 < argument_count) {
                card_index = std::stoi(arguments[++index]);
            } else if ((argument == L"--click" || argument == L"--double-click") && index + 2 < argument_count) {
                double_click = argument == L"--double-click";
                click_x = std::stoi(arguments[++index]);
                click_y = std::stoi(arguments[++index]);
            } else if (argument == L"--advance" && index + 1 < argument_count) {
                advance_seconds = std::stof(arguments[++index]);
            } else if (argument == L"--timeline") {
                show_timeline = true;
            } else if (argument == L"--hide-vehicles") {
                hide_vehicles = true;
            } else if (argument == L"--no-reflections") {
                gmod::ui::settings().reflections = false;
            } else if (argument == L"--unlit") {
                gmod::ui::settings().lighting = false;
            } else if (argument == L"--no-decals") {
                gmod::ui::settings().decals = false;
            } else if (argument == L"--no-blood-mist") {
                g_disable_blood_mist = true;
            } else if (argument == L"--no-smoke") {
                g_disable_smoke = true;
            } else if (argument == L"--no-muzzle-flash") {
                gmod::ui::settings().muzzle_flash = false;
            } else if (argument == L"--player-list") {
                pin_player_list = true;
            } else if (argument == L"--hide-walled-names") {
                gmod::ui::settings().names_through_walls = false;
            } else if (argument == L"--show-walled-names") {
                gmod::ui::settings().names_through_walls = true;
            } else if (argument == L"--outline-all") {
                gmod::ui::settings().outline_all_players = true;
            } else if (argument == L"--follow-car") {
                follow_car = true;
            } else if (argument == L"--sound-log" && index + 1 < argument_count) {
                sound_log_path = arguments[++index];
            } else if (argument == L"--world-sound-reset-test") {
                world_sound_reset_test = true;
            } else if (argument == L"--menu") {
                preview_menu = true;
            } else if (argument == L"--menu-help") {
                preview_menu = preview_help = true;
            } else if (argument == L"--console") {
                preview_console_on = true;
                if (index + 1 < argument_count && arguments[index + 1][0] != L'-') preview_console = arguments[++index];
            } else if (argument == L"--pov") {
                recorder_pov = true;
            } else if (argument == L"--menu-screenshot" && index + 1 < argument_count) {
                menu_screenshot = arguments[++index];
            } else if (argument == L"--menu-keys" && index + 1 < argument_count) {
                // Test only: T = Tab, U/D = Up/Down, L/R = Left/Right, E = Enter.
                menu_keys = arguments[++index];
            } else if (argument == L"--load-cancel-test" && index + 1 < argument_count) {
                load_cancel_test = arguments[++index];
                if (load_cancel_test != L"click" && load_cancel_test != L"escape") {
                    throw std::runtime_error("Load cancellation test requires click or escape.");
                }
            } else if (argument == L"--load-cancel-test-stage" && index + 1 < argument_count) {
                load_cancel_test_stage = std::stoi(arguments[++index]);
            } else if (argument == L"--demo-folder" && index + 1 < argument_count) {
                // Browses this folder for this run only (not saved to the settings).
                gmod::ui::settings().demo_folder = arguments[++index];
            } else if (argument == L"--novsync") {
                vsync = false;
            } else if (argument == L"--reload-test") {
                reload_test = true;
            } else if (argument == L"--fist-impact-test") {
                fist_impact_test = true;
            } else if (argument == L"--melee-render-test") {
                melee_render_test = true;
            } else if (argument == L"--back-weapons-render-test") {
                back_weapons_test = true;
            } else if (argument == L"--identifiers-render-test") {
                identifiers_render_test = true;
            } else if (argument == L"--fire-render-test") {
                fire_render_test = true;
            } else if (argument == L"--bank-shutters-test") {
                bank_shutters_test = true;
            } else if (argument == L"--bank-baked-test") {
                bank_baked_test = true;
            } else if (argument == L"--graphics-render-test") {
                graphics_render_test = true;
            } else if (argument == L"--water-render-test") {
                water_render_test = true;
            } else if (argument == L"--gpu-cache-test") {
                gpu_cache_test = true;
                reload_test = true;
                gmod::ui::settings().cache_gpu_assets = true;
            } else if (argument == L"--reload-demo" && index + 1 < argument_count) {
                reload_demo = arguments[++index];
            } else if (argument == L"--session-cache-test") {
                session_cache_test = gpu_cache_test = reload_test = true;
                gmod::ui::settings().cache_gpu_assets = true;
            } else if (argument == L"--retained-state-test") {
                retained_state_test = true;
            } else if (argument == L"--retained-replace-test") {
                retained_replace_test = true;
            } else if (argument == L"--menu-return-test" && index + 1 < argument_count) {
                menu_return_after = std::stof(arguments[++index]);
            } else if (argument == L"--close-test" && index + 1 < argument_count) {
                // Test only: plays muted, then closes the window like the X button.
                close_after = std::stof(arguments[++index]);
            } else if (argument == L"--camera" && index + 5 < argument_count) {
                std::array<float, 5> values{};
                for (auto& value : values) {
                    value = std::stof(arguments[++index]);
                }
                camera_override = values;
            } else {
                const auto candidate = std::filesystem::path(argument);
                const auto extension = candidate.extension().wstring();
                if (CompareStringOrdinal(extension.c_str(), -1, L".dem", -1, TRUE) == CSTR_EQUAL) {
                    demo_path = candidate;
                }
            }
        }
        if (arguments) {
            LocalFree(arguments);
        }
        if (session_cache_test) {
            if (!demo_path || !reload_demo)
                throw std::runtime_error("Session cache test requires --demo and --reload-demo.");
            initial_test_demo = demo_path;
        }

        // Main menu: choose a demo, free roam, or quit. Skipped on the first pass when the
        // command line already says what to do (screenshots, --demo, --freeroam). "Back to main
        // menu" tears the scene down and comes back here, in the same window.
        bool first_session = true;
        bool keep_options = false;  // --reload-test: load the same scene again in this window
        gmod::dx12::GpuAssetCache gpu_asset_cache;
        CpuAssetCache cpu_asset_cache;
        gmod::app::ParsedDemoCache parsed_demos;
        RetainedMap retained_map;
        std::map<std::string, RetainedMap> retained_maps;
        std::string active_map_name;
        std::unique_ptr<Dx12Foundation> retained_renderer;
        struct ParkedRenderer {
            std::unique_ptr<Dx12Foundation> renderer;
            HWND surface = nullptr;
            std::size_t revision = 0;
        };
        std::map<std::string, ParkedRenderer> retained_renderers;
        std::string renderer_map_key;
        std::size_t renderer_map_revision = 0;
        ID3D12Resource* first_world_resource = nullptr;
        ID3D12Resource* first_map_prop_resource = nullptr;
        std::size_t first_cache_entries = 0;
        std::size_t first_cache_buffer_hits = 0;
        double first_cache_load_seconds = 0;
        const auto clear_gpu_cache = [&] {
            std::cout << "GPU_CACHE_CLEAR resources=" << gpu_asset_cache.size() << "\n";
            retained_renderer.reset();
            for (auto& [key, parked] : retained_renderers) {
                parked.renderer.reset();
                if (parked.surface) DestroyWindow(parked.surface);
            }
            retained_renderers.clear();
            retained_map = {};
            retained_maps.clear();
            active_map_name.clear();
            parsed_demos.clear();
            renderer_map_key.clear();
            if (g_surface) {
                DestroyWindow(g_surface);
                g_surface = nullptr;
            }
            gpu_asset_cache.clear();
            cpu_asset_cache = {};
        };
        for (;;) {
        if (!gmod::ui::settings().cache_gpu_assets && gpu_asset_cache.size() != 0) clear_gpu_cache();
        const bool skip_menu = (first_session || keep_options) && (!screenshot_path.empty() || demo_path || free_roam
            || camera_override);
        if (!first_session && !keep_options) {
            demo_path.reset();
            camera_override.reset();
            start_time = 0.0f;
            follow_index = card_index = click_x = click_y = -1;
            follow_car = false;
            advance_seconds = 0.0f;
            show_timeline = free_roam = recorder_pov = pin_player_list = hide_vehicles = false;
            preview_menu = preview_help = preview_console_on = false;
            sound_log_path.clear();
        }
        first_session = false;
        keep_options = false;
        if (!skip_menu || !menu_screenshot.empty()) {
            static bool updates_started = false;
            gmod::ui::MainMenu menu(application_root, clear_gpu_cache);
            g_menu = &menu;
            if (load_cancel_test_completed) {
                for (const auto cursor : {IDC_IBEAM, IDC_WAIT}) {
                    SetCursor(LoadCursorW(nullptr, cursor));
                    SendMessageW(window, WM_SETCURSOR, reinterpret_cast<WPARAM>(window), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
                    if (GetCursor() != LoadCursorW(nullptr, IDC_ARROW)) {
                        throw std::runtime_error("Default cursor was not restored.");
                    }
                }
                std::cout << "PASS: load cancellation returned to the home menu; arrow cursor restored\n";
            }
            if (!g_headless) {
                SetTimer(window, 74, 200, nullptr);
                if (!updates_started) {
                    updates_started = true;
                    menu.start_updates();
                }
            }
            SetWindowTextW(window, L"GMod Replay");
            InvalidateRect(window, nullptr, FALSE);
            if (menu_return_after > 0.0f) {
                UpdateWindow(window);
                std::cout << "MENU_RETURN_OK surface=" << (g_surface ? "alive" : "gone") << std::endl;
                g_menu = nullptr;
                return 0;
            }
            if (!menu_screenshot.empty()) {
                const auto draw_menu_test = [&] {
                    const HDC dc = GetDC(window);
                    menu.draw(window, dc);
                    ReleaseDC(window, dc);
                };
                draw_menu_test();
                for (const wchar_t key : menu_keys) {
                    if (key == L'W') {
                        const auto deadline = GetTickCount64() + 30000;
                        while (menu.updates_busy() && GetTickCount64() < deadline) {
                            menu.poll_updates();
                            Sleep(50);
                        }
                        if (menu.updates_busy()) throw std::runtime_error("Menu update service test timed out.");
                        draw_menu_test();
                        continue;
                    }
                    const WPARAM code = key == L'T' ? VK_TAB : key == L'U' ? VK_UP : key == L'D' ? VK_DOWN
                        : key == L'L' ? VK_LEFT : key == L'R' ? VK_RIGHT : key == L'E' ? VK_RETURN
                        : key == L'N' ? VK_NEXT : key == L'H' ? VK_HOME : key == L'X' ? VK_ESCAPE : 0;
                    if (code) {
                        menu.key_down(window, code);
                        draw_menu_test();
                    }
                }
                UpdateWindow(window);
                save_window_bmp(window, menu_screenshot, [&](HDC dc) { menu.draw(window, dc); });
                g_menu = nullptr;
                return 0;
            }
            MSG pending{};
            while (!menu.finished() && GetMessageW(&pending, nullptr, 0, 0) > 0) {
                TranslateMessage(&pending);
                DispatchMessageW(&pending);
            }
            KillTimer(window, 74);
            g_menu = nullptr;
            if (!menu.finished()
                || menu.result().action == gmod::ui::MenuResult::Action::Quit) {
                return 0;
            }
            if (menu.result().action == gmod::ui::MenuResult::Action::OpenDemo) {
                demo_path = menu.result().demo;
                gmod::ui::MainMenu::remember(application_root, *demo_path);
            }
        }

        // Viewing policy (ViewingPolicy.h): the private edition needs a verified collaborator,
        // the public edition keeps recordings locked for their first 15 minutes.
        {
            const auto clearance = demo_path ? gmod::app::clear_recording(*demo_path, 0.0f)
                                             : gmod::app::clear_free_roam();
            if (!clearance.allowed) {
                std::cerr << clearance.message << "\n";
                if (g_headless) return 1;
                const std::wstring text(clearance.message.begin(), clearance.message.end());
                MessageBoxW(window, text.c_str(), L"GMod Replay", MB_OK | MB_ICONERROR);
                continue;
            }
        }

        // Scene parsing runs on a worker so this thread keeps pumping window
        // messages: the window stays responsive and can cancel back to the menu.
        {
        LoadingState loading;
        LoadedScene scene;
        double demo_parse_seconds = 0;
        bool reused_demo_parse = false;
        bool reused_disk_demo = false;
        std::exception_ptr load_error;
        gmod::app::Clearance clearance;
        bool cancelled = false;
        std::optional<gmod::demo::DemoData> demo_data;
        if (demo_path) {
            loading.stage_count = 6;
            loading.stage_offset = 1;
            loading.title = L"Loading demo  " + demo_path->filename().wstring();
        }
        g_loading = &loading;
        std::thread loader([&] {
            try {
                if (demo_path) {
                    const auto parse_started = std::chrono::steady_clock::now();
                    loading.begin_stage(0, L"Parsing the demo (gmoddem)", 100);
                    const bool cache_enabled = gmod::ui::settings().cache_gpu_assets;
                    const auto* cached = cache_enabled ? parsed_demos.find(*demo_path) : nullptr;
                    if (cached) {
                        loading.begin_stage(0, L"Restoring parsed demo from memory", 0);
                        demo_data = *cached;
                        reused_demo_parse = true;
                        std::cout << "PARSED_DEMO_REUSE: skipping parser\n";
                    }
                    const auto version = gmod::dx12::GpuAssetCache::texture_source_key(*demo_path, false);
                    const bool disk_enabled = gmod::ui::settings().cache_demos_on_disk;
                    if (!cached || disk_enabled) {
                        std::filesystem::path parser;
                        if (!cached) parser = gmod::demo::find_parser(application_root);
                        const auto progress = [&](float percent, const std::string& text) {
                            loading.progress(static_cast<std::size_t>(percent), text);
                        };
                        std::optional<gmod::demo::DemoCache> disk_cache;
                        std::string disk_key;
                        bool disk_present = false;
                        const auto disk_limit = static_cast<std::uintmax_t>(gmod::ui::settings().demo_cache_gb)
                            * 1024 * 1024 * 1024;
                        if (disk_enabled) {
                            try {
                                if (parser.empty()) parser = gmod::demo::find_parser(application_root);
                                disk_cache.emplace();
                                disk_cache->trim(disk_limit);
                                const auto hash = gmod::demo::DemoCache::fingerprint(*demo_path, loading.cancel, progress);
                                disk_key = gmod::demo::DemoCache::key(hash,
                                    gmod::demo::DemoCache::fingerprint(parser, loading.cancel));
                                if (cached) disk_present = disk_cache->touch(disk_key);
                                else {
                                    demo_data = disk_cache->load(disk_key, *demo_path, loading.cancel, progress);
                                    reused_disk_demo = demo_data.has_value();
                                    disk_present = reused_disk_demo;
                                }
                                std::cout << "DISK_DEMO_CACHE " << (disk_present ? "hit" : "miss")
                                    << " key=" << disk_key << "\n";
                            } catch (const std::runtime_error& error) {
                                loading.check_cancel();
                                std::cerr << "Disk demo cache could not be read: " << error.what() << "\n";
                                progress(0, cached ? "Disk cache unavailable; using the in-memory recording"
                                                  : "Disk cache unavailable; parsing the original recording");
                            }
                        }
                        if (!demo_data) {
                            demo_data = gmod::demo::load_demo(*demo_path, parser, progress,
                                loading.cancel, application_root / L"assets\\data\\perpheads\\packs");
                            loading.check_cancel();
                        }
                        if (disk_cache && !disk_key.empty() && !disk_present) {
                            try {
                                if (version != gmod::dx12::GpuAssetCache::texture_source_key(*demo_path, false))
                                    throw std::runtime_error("Recording changed during parsing; disk cache not saved.");
                                if (!disk_cache->store(disk_key, *demo_data, disk_limit, loading.cancel, progress)) {
                                    std::cerr << "Parsed recording exceeds the disk cache limit; not cached.\n";
                                    progress(100, "Recording exceeds disk cache limit; playing without saving");
                                }
                            } catch (const std::runtime_error& error) {
                                loading.check_cancel();
                                std::cerr << "Disk demo cache could not be saved: " << error.what() << "\n";
                                progress(100, "Could not save disk cache; see application log");
                            }
                        }
                    }
                    loading.check_cancel();
                    if (version != gmod::dx12::GpuAssetCache::texture_source_key(*demo_path, false))
                        throw std::runtime_error("Recording changed while loading: " + demo_path->string());
                    if (cache_enabled && !cached) parsed_demos.store(*demo_path, version, *demo_data);
                    demo_parse_seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - parse_started).count();
                    // Checked again with the real length: a recording only unlocks 15 minutes after it ended.
                    clearance = gmod::app::clear_recording(*demo_path, demo_data->duration);
                    if (!clearance.allowed) throw ViewingDenied(clearance.message);
                }
                if (retained_state_test && first_cache_entries != 0 && demo_data) {
                    std::reverse(demo_data->props.begin(), demo_data->props.end());
                    std::reverse(demo_data->doors.begin(), demo_data->doors.end());
                    std::cout << "RETAINED_STATE_TEST: reordered props and doors before fresh binding\n";
                }
                loading.check_cancel();
                if (gmod::ui::settings().cache_gpu_assets) {
                    const auto map_name = demo_data ? demo_data->map_name : std::string("rp_paralake_city_v6");
                    if (active_map_name != map_name) {
                        if (!active_map_name.empty())
                            retained_maps.insert_or_assign(active_map_name, std::move(retained_map));
                        retained_map = {};
                        const auto parked = retained_maps.find(map_name);
                        if (parked != retained_maps.end()) {
                            retained_map = std::move(parked->second);
                            retained_maps.erase(parked);
                        }
                        active_map_name = map_name;
                    }
                }
                scene = load_scene(application_root, loading, demo_data ? &*demo_data : nullptr,
                                   demo_path ? *demo_path : std::filesystem::path(),
                                   gmod::ui::settings().cache_gpu_assets ? &cpu_asset_cache : nullptr,
                                   gmod::ui::settings().cache_gpu_assets ? &retained_map : nullptr);
                scene.demo = std::move(demo_data);
            } catch (const LoadCancelled&) {
                cancelled = true;
            } catch (...) {
                if (loading.cancel) cancelled = true;
                else load_error = std::current_exception();
            }
            loading.finished = true;
            PostMessageW(window, WM_NULL, 0, 0);
        });
        bool quit_requested = false;
        auto last_paint = std::chrono::steady_clock::time_point{};
        bool test_cancel_sent = false;
        while (!loading.finished) {
            const auto test_progress = loading.snapshot();
            if (!load_cancel_test.empty() && !test_cancel_sent
                && test_progress.stage >= load_cancel_test_stage
                && (test_progress.stage > 1 || test_progress.current > 1)) {
                if (load_cancel_test == L"escape") {
                    SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
                } else {
                    const RECT button = loading_cancel_rect(window);
                    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON,
                        MAKELPARAM((button.left + button.right) / 2, (button.top + button.bottom) / 2));
                }
                test_cancel_sent = true;
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    quit_requested = true;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (quit_requested) {
                loading.cancel = true;
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last_paint > std::chrono::milliseconds(100)) {
                InvalidateRect(window, nullptr, FALSE);
                last_paint = now;
            }
        }
        loader.join();
        if (quit_requested) {
            g_loading = nullptr;
            return 0;
        }
        if (cancelled || loading.cancel) {
            g_loading = nullptr;
            if (!load_cancel_test.empty()) {
                if (!test_cancel_sent) throw std::runtime_error("Load finished before the cancellation test ran.");
                load_cancel_test_completed = true;
                menu_screenshot = screenshot_path;
                if (menu_screenshot.empty()) throw std::runtime_error("Load cancellation test requires --screenshot.");
                load_cancel_test.clear();
            }
            InvalidateRect(window, nullptr, FALSE);
            continue;
        }
        if (!load_cancel_test.empty()) throw std::runtime_error("Load cancellation test did not cancel the loader.");
        if (load_error) {
            g_loading = nullptr;
            try {
                std::rethrow_exception(load_error);
            } catch (const ViewingDenied& denied) {
                std::cerr << denied.what() << "\n";
                if (g_headless) return 1;
                const std::string message = denied.what();
                InvalidateRect(window, nullptr, FALSE);
                MessageBoxW(window, std::wstring(message.begin(), message.end()).c_str(), L"GMod Replay",
                            MB_OK | MB_ICONERROR);
                continue;
            }
        }

        loading.begin_stage(5, L"Uploading geometry to the GPU", 0);
        loading.cancellable = false;
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        // The swap chain takes over presentation from here; stop GDI painting.
        g_loading = nullptr;
        const auto gpu_setup_start = std::chrono::steady_clock::now();
        if (retained_renderer && renderer_map_key != retained_map.key
            && gmod::ui::settings().cache_gpu_assets) {
            ShowWindow(g_surface, SW_HIDE);
            auto& parked = retained_renderers[renderer_map_key];
            parked.renderer.reset();
            if (parked.surface) DestroyWindow(parked.surface);
            parked = {std::move(retained_renderer), g_surface, renderer_map_revision};
            g_surface = nullptr;
        }
        if (!retained_renderer && gmod::ui::settings().cache_gpu_assets) {
            const auto parked = retained_renderers.find(retained_map.key);
            if (parked != retained_renderers.end()) {
                retained_renderer = std::move(parked->second.renderer);
                g_surface = parked->second.surface;
                renderer_map_key = retained_map.key;
                renderer_map_revision = parked->second.revision;
                retained_renderers.erase(parked);
                RECT client{};
                GetClientRect(window, &client);
                MoveWindow(g_surface, 0, 0, client.right, client.bottom, FALSE);
            }
        }
        const bool reuse_renderer = retained_renderer && retained_map.scene
            && renderer_map_key == retained_map.key && renderer_map_revision == retained_map.revision;
        if (!reuse_renderer) {
            retained_renderer.reset();
            if (g_surface) DestroyWindow(g_surface);
            RECT client{};
            GetClientRect(window, &client);
            g_surface = CreateWindowExW(0, L"GmodReplaySurface", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                        0, 0, std::max<LONG>(client.right - client.left, 1),
                                        std::max<LONG>(client.bottom - client.top, 1), window, nullptr, instance,
                                        nullptr);
            retained_renderer = std::make_unique<Dx12Foundation>(
            window, scene.triangles, scene.material_textures, scene.prop_data,
            scene.lightmap, scene.sky_camera, scene.sky_paint, scene.sun,
            scene.player_spawn, vsync, gmod::ui::settings().cache_gpu_assets ? &gpu_asset_cache : nullptr);
            renderer_map_key = retained_map.key;
            renderer_map_revision = retained_map.revision;
        } else {
            retained_renderer->bind_demo(scene);
            ShowWindow(g_surface, SW_SHOW);
        }
        auto& renderer = *retained_renderer;
        renderer.set_water_probe(scene.probe);
        renderer.bind_map_brushes(scene);
        if (gpu_cache_test && !renderer.demo_state_clean())
            throw std::runtime_error("Previous demo camera, effects, audio, poses or palettes leaked into the new scene.");
        auto audio_scene = std::move(scene.audio);
        auto scene_content_roots = std::move(scene.content_roots);
        std::unique_ptr<ReplayDriver> replay;
        if (scene.demo && scene.probe) {
            replay = std::make_unique<ReplayDriver>(
                std::move(*scene.demo), scene.prop_data.actors, *scene.probe,
                scene.animator, scene.outfit, scene.demo_audio, window, show_timeline,
                (!camera_override && screenshot_path.empty()) || recorder_pov);
            replay->reconstruct_fist_impacts(renderer);
            replay->set_viewable(clearance.playable);
            replay->seek(start_time);
            if (follow_index >= 0) replay->follow(follow_index);
            if (follow_car) replay->follow_first_driver();
            if (card_index >= 0) replay->open_card(card_index);
            if (click_x >= 0) replay->test_click(click_x, click_y, double_click);
            if (pin_player_list) replay->pin_player_list();
            if (hide_vehicles) replay->hide_vehicles();
            if (preview_menu) replay->preview_menu(preview_help);
            if (!sound_log_path.empty()) replay->log_world_sounds(sound_log_path);
            if (preview_console_on) replay->preview_console(preview_console);
            if (demo_path) replay->set_real_start(demo_start_time_of_day(*demo_path));
            renderer.set_frame_text_painter([driver=replay.get(),&renderer](HDC dc,int width,int height) {
                return driver->paint_frame_labels(dc,width,height,renderer);
            });
        }
        scene = LoadedScene{};
        const auto scene_load_seconds = std::chrono::duration<double>(gpu_setup_start - loading.started).count();
        std::cout << "SCENE_LOAD seconds=" << scene_load_seconds << "\n";
        std::cout << "LOAD_BREAKDOWN demo_parse_seconds=" << demo_parse_seconds
            << " map_and_binding_seconds=" << scene_load_seconds - demo_parse_seconds
            << " retained_map=" << reuse_renderer << " parsed_demo_cached=" << reused_demo_parse
            << " disk_demo_cached=" << reused_disk_demo << "\n";
        if (gmod::ui::settings().cache_gpu_assets)
            std::cout << "SESSION_CACHE parsed_demos=" << parsed_demos.size()
                << " prepared_maps=" << retained_maps.size() + (retained_map.scene ? 1 : 0)
                << " renderers=" << retained_renderers.size() + (retained_renderer ? 1 : 0) << "\n";
        {
            const auto now = std::chrono::steady_clock::now();
            std::ofstream timing(application_root / L"build\\startup_timing.log", std::ios::trunc);
            timing << "scene_load_seconds="
                   << std::chrono::duration<double>(gpu_setup_start - loading.started).count()
                   << " gpu_setup_seconds="
                   << std::chrono::duration<double>(now - gpu_setup_start).count()
                   << " demo_parse_seconds=" << demo_parse_seconds
                   << " map_and_binding_seconds=" << scene_load_seconds - demo_parse_seconds
                   << " retained_map=" << reuse_renderer
                   << " parsed_demo_cached=" << reused_demo_parse
                   << " disk_demo_cached=" << reused_disk_demo << "\n";
        }
        if (camera_override) {
            const auto& c = *camera_override;
            renderer.set_camera(c[0], c[1], c[2], c[3], c[4]);
        }
        if (!replay && g_headless) renderer.set_water_time(start_time);
        if (screenshot_path.empty()) {
            auto sound_roots = std::move(scene_content_roots);
            renderer.attach_audio(std::make_unique<gmod::audio::AudioSystem>(
                std::move(sound_roots), application_root / L"build\\audio.log",
                std::move(audio_scene)));
            g_audio = renderer.audio();
        }
        std::unique_ptr<gmod::ui::EscMenu> free_roam_menu;
        bool free_roam_escape_down = false;
        bool free_roam_mouse_was_captured = false;
        const auto show_free_roam_menu = [&](bool visible) {
            if (visible) {
                free_roam_mouse_was_captured = g_mouse_captured;
                release_mouse();
            }
            free_roam_menu->set_visible(visible);
            g_mouse_locked_out = visible;
            if (!visible && free_roam_mouse_was_captured) capture_mouse(window);
        };
        if (!replay && !g_headless) {
            free_roam_menu = std::make_unique<gmod::ui::EscMenu>(window, gmod::ui::EscMenuActions{
                [&] { show_free_roam_menu(false); },
                [&] { g_menu_requested = true; },
                [&] { PostMessageW(window, WM_CLOSE, 0, 0); },
                {}});
        }
        if (!screenshot_path.empty()) {
            if (!sound_log_path.empty()) renderer.render();
            if (replay) {
                // Optionally let the demo play for a while first so animation
                // cycles advance (fixed 30 Hz steps; deterministic).
                constexpr float Step = 1.0f / 30.0f;
                replay->update(renderer, 0.0f, false);
                if (!sound_log_path.empty()) renderer.render();
                for (float played = 0.0f; played < advance_seconds; played += Step) {
                    replay->update(renderer, Step, false);
                    if (!sound_log_path.empty()) renderer.render();
                }
                if (world_sound_reset_test) replay->test_world_sound_resets(renderer);
            }
            renderer.finish_world_material_stream();
            renderer.render();
            if (water_render_test) renderer.test_water_resize();
            if (replay && replay->resolve_test_click(renderer,static_cast<int>(renderer.width()),static_cast<int>(renderer.height()))) {
                replay->update(renderer,0.0f,false);
                renderer.render();
            }
            if (fist_impact_test && replay) replay->test_fist_impacts(renderer,std::filesystem::path(screenshot_path).parent_path());
            if (melee_render_test && replay) replay->test_melee_render(renderer,std::filesystem::path(screenshot_path).parent_path());
            if (back_weapons_test && replay) replay->test_back_weapons(renderer,std::filesystem::path(screenshot_path).parent_path());
            if (identifiers_render_test && replay) replay->test_model_identifiers(renderer,std::filesystem::path(screenshot_path).parent_path());
            if (fire_render_test && replay) replay->test_fire_render(renderer,std::filesystem::path(screenshot_path).parent_path());
            if (bank_shutters_test && replay) replay->test_bank_shutters(renderer,std::filesystem::path(screenshot_path).parent_path());
            if (bank_baked_test && replay) replay->test_baked_bank_lighting(renderer,std::filesystem::path(screenshot_path).parent_path());
            if (graphics_render_test) {
                auto& options=gmod::ui::settings();
                const auto saved_reflections=options.reflections, saved_lighting=options.lighting;
                const auto folder=std::filesystem::path(screenshot_path).parent_path();
                const auto capture=[&](const wchar_t* name,bool reflections,bool lighting) {
                    options.reflections=reflections; options.lighting=lighting;
                    if (replay) replay->update(renderer,0,false);
                    renderer.render();
                    if (!reflections && renderer.water_reflections_active())
                        throw std::runtime_error("Disabled water reflections still render the scene.");
                    renderer.capture_bmp((folder/name).wstring());
                };
                capture(L"graphics-lit.bmp",true,true);
                capture(L"graphics-no-reflections.bmp",false,true);
                capture(L"graphics-unlit.bmp",false,false);
                capture(L"graphics-restored.bmp",true,true);
                const auto bytes=[&](const wchar_t* name) {
                    std::ifstream file(folder/name,std::ios::binary);
                    return std::vector<char>(std::istreambuf_iterator<char>(file),{});
                };
                const auto lit=bytes(L"graphics-lit.bmp");
                if (lit.empty() || lit!=bytes(L"graphics-restored.bmp")
                    || lit==bytes(L"graphics-no-reflections.bmp")
                    || bytes(L"graphics-no-reflections.bmp")==bytes(L"graphics-unlit.bmp"))
                    throw std::runtime_error("Graphics settings rendering/restore regression.");
                options.reflections=saved_reflections; options.lighting=saved_lighting;
                if (replay) replay->update(renderer,0,false);
                renderer.render();
                std::ofstream(folder/L"graphics-test.txt") << "PASS reflection pass disabled, unlit image differs, settings restore exactly\n";
            }
            renderer.capture_bmp(screenshot_path);
            if (water_render_test) renderer.capture_water_debug(screenshot_path);
            if (replay) {
                overlay_on_bmp(screenshot_path, [&](HDC dc, int width, int height) {
                    replay->paint_overlays(dc, width, height, renderer,
                                          renderer.listener_source());
                });
            }
            if (reload_test) {
                if (gpu_cache_test && session_cache_phase == 0) {
                    SendMessageW(window, WM_SYSKEYDOWN, VK_F10, 0);
                    SendMessageW(window, WM_SYSKEYUP, VK_F10, 1LL << 30);
                    if (g_menu_requested) throw std::runtime_error("F10 must not unload the current scene.");
                    first_cache_entries = gpu_asset_cache.size();
                    first_cache_buffer_hits = gpu_asset_cache.buffer_hits;
                    first_cache_load_seconds = scene_load_seconds;
                    first_world_resource = renderer.world_resource();
                    first_map_prop_resource = renderer.map_prop_resource();
                    if (first_cache_entries == 0) throw std::runtime_error("First scene did not retain GPU assets.");
                    std::cout << "GPU_CACHE_FIRST entries=" << first_cache_entries << "\n";
                }
                reload_test = false;
                if (session_cache_test) {
                    ++session_cache_phase;
                    demo_path = session_cache_phase == 1 ? reload_demo : initial_test_demo;
                    reload_test = session_cache_phase == 1;
                } else if (reload_demo) demo_path = *reload_demo;
                keep_options = true;
                const std::filesystem::path first(screenshot_path);
                screenshot_path = (first.parent_path() / (L"reload_" + first.filename().wstring())).wstring();
                renderer.suspend_demo();
                replay.reset();
                if (!renderer.demo_state_clean())
                    throw std::runtime_error("Unloading did not reset demo state.");
                if (g_surface) ShowWindow(g_surface, SW_HIDE);
                if (retained_replace_test) retained_map.key += ":replacement-test";
                continue;
            }
            if (gpu_cache_test) {
                if (((!reload_demo && !retained_replace_test) || session_cache_test) && !reused_demo_parse)
                    throw std::runtime_error("Reload ran the demo parser instead of reusing parsed data.");
                if (session_cache_test && parsed_demos.size() != 2)
                    throw std::runtime_error("Switching recordings did not retain both parsed demos.");
                if (retained_replace_test) {
                    if (reuse_renderer || retained_map.reuses != 0 || cpu_asset_cache.maps.size() != 1)
                        throw std::runtime_error("Replacing the retained map did not discard the previous scene.");
                    std::cout << "PASS: retained map replacement discarded the old scene and renderer\n";
                } else if (!reuse_renderer || retained_map.reuses == 0
                    || renderer.world_resource() != first_world_resource
                    || renderer.map_prop_resource() != first_map_prop_resource)
                    throw std::runtime_error("Warm load rebuilt the retained map or renderer.");
                if (!retained_replace_test && (gpu_asset_cache.size() < first_cache_entries
                    || (!reload_demo && gpu_asset_cache.buffer_hits <= first_cache_buffer_hits)))
                    throw std::runtime_error("Reload did not reuse retained textures and geometry.");
                std::cout << "PASS: GPU cache reload entries=" << gpu_asset_cache.size()
                    << " hits=" << gpu_asset_cache.hits << " buffer_hits=" << gpu_asset_cache.buffer_hits << "\n";
                if (cpu_asset_cache.map_hits == 0 || (!reload_demo && cpu_asset_cache.mesh_hits == 0))
                    throw std::runtime_error("Reload did not reuse decoded map and model data.");
                std::cout << "CPU_CACHE map_hits=" << cpu_asset_cache.map_hits
                    << " mesh_hits=" << cpu_asset_cache.mesh_hits
                    << " material_hits=" << cpu_asset_cache.material_hits
                    << " vtf_header_hits=" << cpu_asset_cache.texture_header_hits << "\n";
                std::cout << "CACHE_TIMING cold_seconds=" << first_cache_load_seconds
                    << " warm_seconds=" << scene_load_seconds << "\n";
                clear_gpu_cache();
                if (gpu_asset_cache.size() != 0 || gpu_asset_cache.device
                    || !cpu_asset_cache.maps.empty() || !cpu_asset_cache.meshes.empty()
                    || !cpu_asset_cache.model_traces.empty() || !cpu_asset_cache.materials.empty()
                    || !cpu_asset_cache.texture_headers.empty() || parsed_demos.size() != 0
                    || !retained_maps.empty() || !retained_renderers.empty())
                    throw std::runtime_error("Disabling GPU cache did not clear retained ownership.");
                std::cout << "PASS: GPU cache clear released retained assets and device\n";
            }
            return 0;
        }

        if (!hidden_window) ShowWindow(window, show_command);
        {
            // Frame the picture at the real client size from the first frame.
            RECT client{};
            GetClientRect(window, &client);
            g_resize_width = static_cast<UINT>(client.right - client.left);
            g_resize_height = static_cast<UINT>(client.bottom - client.top);
            g_resize_pending = g_resize_width > 0 && g_resize_height > 0;
        }
        MSG message{};
        auto last_frame = std::chrono::steady_clock::now();
        const auto session_start = last_frame;
        if ((menu_return_after > 0.0f || close_after > 0.0f) && renderer.audio()) renderer.audio()->toggle_mute();
        bool back_to_menu = false;
        while (message.message != WM_QUIT) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (message.message == WM_QUIT) {
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            const float delta = std::min(
                std::chrono::duration<float>(now - last_frame).count(), 0.25f);
            last_frame = now;
            const bool focused = GetForegroundWindow() == window;
            if (g_resize_pending) {
                g_resize_pending = false;
                renderer.resize(g_resize_width, g_resize_height);
            }
            if (replay) {
                replay->update(renderer, delta, focused);
            } else if (free_roam_menu) {
                const bool escape_down = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
                if (focused && escape_down && !free_roam_escape_down)
                    show_free_roam_menu(!free_roam_menu->visible());
                free_roam_escape_down = escape_down;
                free_roam_menu->update();
                if (auto* audio = renderer.audio())
                    apply_audio_settings(audio);
            }
            // The Esc menu returns home after confirmation; tests can request it directly.
            if (menu_return_after > 0.0f && std::chrono::duration<float>(now - session_start).count() > menu_return_after) {
                g_menu_requested = true;
            }
            if (close_after > 0.0f && std::chrono::duration<float>(now - session_start).count() > close_after) {
                close_after = 0.0f;
                PostMessageW(window, WM_CLOSE, 0, 0);
            }
            if (g_menu_requested) {
                back_to_menu = true;
                break;
            }
            renderer.render();
        }
        // Sound stops first, before any GPU or thread teardown that could take a while.
        g_audio = nullptr;
        renderer.suspend_demo();
        if (gmod::ui::settings().cache_gpu_assets) {
            std::cout << "GPU_CACHE resources=" << gpu_asset_cache.size()
                << " hits=" << gpu_asset_cache.hits << " misses=" << gpu_asset_cache.misses << "\n";
        }
        if (!back_to_menu) return 0;
        // Playback ends here; the optional retained map owns the scene and renderer.
        replay.reset();
        free_roam_menu.reset();
        if (!gmod::ui::settings().cache_gpu_assets) clear_gpu_cache();
        }
        if (g_surface) {
            ShowWindow(g_surface, SW_HIDE);
        }
        release_mouse();
        g_menu_requested = false;
        g_mouse_locked_out = false;
        g_drag_look = false;
        g_dragging = false;
        g_click_pending = false;
        g_follow_click_pending = false;
        g_press_double_click = false;
        g_deselect_pending = false;
        g_hover_player = false;
        g_esc_consumed = false;
        g_console_key_pending = false;
        g_text_input = false;
        g_resize_pending = false;
        InvalidateRect(window, nullptr, TRUE);
        }
    } catch (const std::exception& error) {
        wchar_t executable_path[MAX_PATH]{};
        const auto executable_length = GetModuleFileNameW(
            nullptr, executable_path, static_cast<DWORD>(std::size(executable_path)));
        if (executable_length > 0 && executable_length < std::size(executable_path)) {
            const auto error_log =
                std::filesystem::path(executable_path).parent_path()
                / L"build\\application_error.log";
            std::ofstream output(error_log, std::ios::trunc);
            output << error.what() << "\n";
        }
        if(!g_headless) MessageBoxA(window, error.what(), "GMod Replay error", MB_OK | MB_ICONERROR);
        return 1;
    }
}

namespace {

// Loading screen, double-buffered GDI; only used before the swap chain exists.
void paint_loading_screen(HWND window, const LoadingState& loading) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, std::max(width, 1), std::max(height, 1));
    HGDIOBJ previous_bitmap = SelectObject(dc, bitmap);

    HBRUSH background = CreateSolidBrush(RGB(12, 18, 30));
    FillRect(dc, &client, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);

    const auto snapshot = loading.snapshot();
    const double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - loading.started).count();

    HFONT title_font = CreateFontW(34, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    HFONT body_font = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    const int left = width / 8;
    const int right = width - width / 8;
    int y = height / 2 - 110;

    HGDIOBJ previous_font = SelectObject(dc, title_font);
    SetTextColor(dc, RGB(235, 240, 248));
    RECT line{left, y, right, y + 44};
    DrawTextW(dc, loading.title.c_str(), -1, &line, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    y += 56;

    SelectObject(dc, body_font);
    wchar_t stage_text[256]{};
    swprintf_s(stage_text, L"Step %d of %d: %s", snapshot.stage, loading.stage_count,
               snapshot.stage_name.c_str());
    line = {left, y, right, y + 26};
    DrawTextW(dc, stage_text, -1, &line, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    y += 36;

    // Overall bar weights each finished stage equally, plus the current
    // stage's fraction when it reports item counts.
    const double stage_fraction = snapshot.total > 0
        ? static_cast<double>(snapshot.current) / snapshot.total : 0.0;
    const double overall = std::clamp(
        (std::max(snapshot.stage, 1) - 1 + stage_fraction) / loading.stage_count, 0.0, 1.0);
    RECT bar{left, y, right, y + 18};
    HBRUSH track = CreateSolidBrush(RGB(36, 46, 64));
    FillRect(dc, &bar, track);
    DeleteObject(track);
    RECT fill = bar;
    fill.right = bar.left + static_cast<LONG>((bar.right - bar.left) * overall);
    HBRUSH accent = CreateSolidBrush(RGB(64, 156, 255));
    FillRect(dc, &fill, accent);
    DeleteObject(accent);
    y += 30;

    SetTextColor(dc, RGB(150, 162, 182));
    wchar_t detail[512]{};
    if (snapshot.total > 0) {
        swprintf_s(detail, L"%zu / %zu   %s", snapshot.current, snapshot.total,
                   snapshot.detail.c_str());
    } else {
        swprintf_s(detail, L"%s", snapshot.detail.c_str());
    }
    line = {left, y, right, y + 26};
    DrawTextW(dc, detail, -1, &line, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    y += 30;
    wchar_t footer[160]{};
    swprintf_s(footer, L"%.0f s elapsed  -  %s", elapsed,
        loading.cancel ? L"Cancelling... returning to the home screen" :
        loading.cancellable ? L"Escape to cancel and return home" : L"Finishing GPU setup");
    line = {left, y, right, y + 26};
    DrawTextW(dc, footer, -1, &line, DT_LEFT | DT_SINGLELINE);

    const RECT button = loading_cancel_rect(window);
    HBRUSH button_background = CreateSolidBrush(loading.cancel || !loading.cancellable ? RGB(36, 46, 64) : RGB(40, 78, 122));
    FillRect(dc, &button, button_background);
    DeleteObject(button_background);
    SetTextColor(dc, loading.cancel || !loading.cancellable ? RGB(150, 162, 182) : RGB(235, 240, 248));
    RECT button_text = button;
    DrawTextW(dc, loading.cancel ? L"Cancelling..." : loading.cancellable ? L"Cancel" : L"Finishing...", -1, &button_text,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous_font);
    SelectObject(dc, previous_bitmap);
    DeleteObject(title_font);
    DeleteObject(body_font);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(window, &paint);
}

} // namespace
