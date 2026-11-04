#pragma once

#include <algorithm>
#include <cstdint>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gmod::source::bsp {

struct Vec3 {
    float x;
    float y;
    float z;
};

struct Triangle {
    Vec3 a;
    Vec3 b;
    Vec3 c;
    std::string material;
    // Normalised by the BSP texdata size; the renderer rescales to the
    // runtime base-texture size like the engine's GetMappingWidth().
    float uv[3][2]{};
    float texdata_size[2]{1.0f, 1.0f};
    // Displacement vertex alpha (0..255): WorldVertexTransition blend weight.
    float blend[3]{};
    // Normalised lightmap-atlas coordinates; negative u means no lightmap (fullbright).
    float lightmap_uv[3][2]{{-1.0f, -1.0f}, {-1.0f, -1.0f}, {-1.0f, -1.0f}};
    // Part of the miniature 3D skybox (visible from the sky_camera), not the playable map.
    bool sky_3d = false;
    // Authored brush geometry retained separately from demo-driven instances.
    int map_brush = -1;
};

// Garry's Mod env_skypaint: the procedural 2D sky behind the 3D skybox.
struct SkyPaint {
    float top[3]{0.35f, 0.55f, 1.0f};
    float bottom[3]{0.525f, 0.655f, 0.945f};
    float sun_normal[3]{0.0f, 0.0f, 1.0f};
    float sun_color[3]{1.0f, 0.95f, 0.8f};
    float sun_size = 1.6f;
    float fade_bias = 1.0f;
};

// The map's light_environment sun (worldlight emit_skylight), in Source axes.
struct SunLight {
    float direction[3]{};  // direction the light travels
    float color[3]{};      // linear, same units as the leaf ambient cubes
};

struct SkyCamera {
    // Fog the engine applies only while drawing the 3D skybox.
    bool fog_enabled = false;
    float fog_start = 0.0f;
    float fog_end = 0.0f;
    float fog_max_density = 1.0f;
    float fog_color[3]{};
    float fog_color2[3]{};
    float fog_direction[3]{1.0f, 0.0f, 0.0f};
    bool fog_blend = false;
    Vec3 origin{};
    float scale = 16.0f;
};

// Style-0 baked lightmaps of every world face packed into one atlas, stored as
// RGB9E5 (DXGI_FORMAT_R9G9B9E5_SHAREDEXP) texels of linear light.
using LightStyleWeights = std::array<float, 256>;
struct StyledLightmapBlock {
    int x = 0, y = 0, width = 0, height = 0;
    std::vector<int> styles;
    std::vector<std::vector<float>> samples; // linear RGB per style, including dark styles
};
using BankLightWeights = std::array<float,4>;
struct StyledLightmaps {
    std::array<int,4> bank_styles{{-1,-1,-1,-1}};
    LightStyleWeights initial{};
    std::vector<StyledLightmapBlock> blocks;
};
struct LightmapPatch {
    int x, y, width, height;
    std::vector<std::uint32_t> texels;
};
struct LightmapAtlas {
    int width = 0;
    int height = 0;
    std::vector<std::uint32_t> texels;
    std::shared_ptr<const StyledLightmaps> styled;
};
// Recompose only faces whose recorded brightness changed, including their filtering gutters.
std::vector<LightmapPatch> lightmap_patches(const StyledLightmaps& data,
    const LightStyleWeights& before, const LightStyleWeights& after);

// Baked leaf ambient light as six linear-RGB axis colours in Source space,
// ordered +X, -X, +Y, -Y, +Z, -Z.
struct AmbientCube {
    bool found = false;
    // The point lies inside the 3D skybox region.
    bool in_sky_3d = false;
    // The point's leaf can see the sky, so the sun's direct light reaches it.
    bool sun_visible = false;
    float color[6][3]{};
};

// A visible entity drawn with an .mdl (doors, prop_dynamic, physics props).
struct EntityModel {
    std::string classname;
    std::string model;
    Vec3 origin{};
    Vec3 angles{};
    bool hidden = false;
    int skin = 0;
    float fade_max = 0.0f;
    int body = 0;
    // prop_door_rotating `hardware`: 0 none, 1 lever, 2 push bar, 3 keypad.
    int hardware = 0;
    // prop_door_rotating swing: degrees, degrees per second, and direction
    // (0 both ways, 1 forward, 2 backward).
    float door_distance = 90.0f;
    float door_speed = 100.0f;
    int open_dir = 1;
    int map_created_id = -1;
    std::string parent_name;
    std::string parent_model;
    Vec3 parent_angles{};
    std::string targetname;
};

// MapCreationID is the one-based entity-lump ordinal plus GMod's 1234 offset.
inline int map_creation_id(std::size_t entity_ordinal) {
    return static_cast<int>(entity_ordinal) + 1235;
}

// PERP's map-authored scenesystemNN groups are randomly kept or killed at startup.
// A single recorded member confirms the whole group, even outside the recorder's PVS.
inline std::optional<std::set<std::string>> unrecorded_map_scenes(
    std::span<const EntityModel> entities, std::span<const int> recorded_ids) {
    if (recorded_ids.empty()) return std::nullopt;
    std::set<std::string> groups, observed;
    for (const auto& entity : entities) {
        std::string_view suffix = entity.targetname;
        if (!suffix.starts_with("scenesystem")) continue;
        suffix.remove_prefix(std::string_view("scenesystem").size());
        if (suffix.empty() || suffix.find_first_not_of("0123456789") != std::string_view::npos)
            continue;
        groups.insert(entity.targetname);
        if (std::find(recorded_ids.begin(), recorded_ids.end(), entity.map_created_id) != recorded_ids.end())
            observed.insert(entity.targetname);
    }
    for (const auto& group : observed) groups.erase(group);
    return groups;
}

// A brush entity's triangles in BSP model-local coordinates. Apply the Source
// AngleMatrix to each point, then add `origin`; playback may replace either pose.
// `model` and `model_index` identify the entity-lump "*N" model; `targetname`
// is useful for correlating map entities with recorded network entities.
struct BrushModel {
    std::string classname;
    std::string targetname;
    std::string model;
    std::size_t model_index = 0;
    Vec3 origin{};
    Vec3 angles{};
    std::vector<Triangle> triangles;
};

struct EntityStats {
    std::size_t model_entities = 0;
    std::size_t brush_entities = 0;
    std::size_t brush_faces = 0;
    std::size_t brush_triangles = 0;
    std::size_t skipped_hidden = 0;
    std::size_t scaled_models = 0;
};

// Entities that make sound. Positions are Source units.
struct AmbientEmitter {
    std::string sound;      // ambient_generic "message": a file or a sound-script name
    Vec3 origin{};
    float radius = 0.0f;    // audible distance; 0 = use `flags`
    int flags = 0;          // spawnflags: 1 everywhere, 16 start silent, 32 not looped
    float volume = 1.0f;    // "health" 0..10 mapped to 0..1
    float pitch = 100.0f;
};

struct SoundscapeEntity {
    std::string targetname;
    std::string soundscape;  // name in the map's soundscape script
    Vec3 origin{};
    std::vector<std::pair<int, Vec3>> positions;  // position0..N entities
};

// A trigger_soundscape brush: while the listener is inside, `target` is active.
struct SoundscapeVolume {
    std::string target;
    Vec3 mins{};
    Vec3 maxs{};
};

struct AudioScene {
    std::vector<AmbientEmitter> ambient;
    std::vector<SoundscapeEntity> soundscapes;
    std::vector<SoundscapeVolume> volumes;
};

struct PlayerSpawnPoint {
    Vec3 origin{};
    float yaw_degrees = 0.0f;
};

struct BspSummary {
    int32_t version = 0;
    std::size_t vertex_count = 0;
    std::size_t edge_count = 0;
    std::size_t surfedge_count = 0;
    std::size_t face_count = 0;
    std::size_t world_face_count = 0;
    std::size_t triangle_count = 0;
    std::size_t material_count = 0;
    std::size_t displacement_count = 0;
    std::size_t displacement_triangle_count = 0;
    std::size_t displacement_failures = 0;
    std::size_t lightmap_faces = 0;
    std::size_t lightmap_failures = 0;
    float lightmap_max = 0.0f;
    float lightmap_mean = 0.0f;
    std::size_t sky_3d_faces = 0;
    std::size_t hidden_faces = 0;
};

// In-memory lighting queries against a map: baked ambient light and solidity.
// Cheap enough to call per entity per frame; independent of the BspReader.
class BspProbe {
public:
    AmbientCube sample(const Vec3& point) const;
    AmbientCube sample(const Vec3& point, const BankLightWeights& weights) const;
    bool is_solid(const Vec3& point) const;
    bool is_water(const Vec3& point) const;

private:
    friend class BspReader;
    struct Impl;
    explicit BspProbe(std::shared_ptr<const Impl> impl);
    std::shared_ptr<const Impl> impl_;
};

class BspReader {
public:
    explicit BspReader(std::filesystem::path path);

    BspSummary read();
    std::optional<PlayerSpawnPoint> first_player_spawn() const;
    // Worldspawn geometry only; brush entity faces are available from brush_models().
    const std::vector<Triangle>& world_triangles() const noexcept;
    std::vector<Triangle> release_world_triangles() noexcept;
    const std::optional<SkyCamera>& sky_camera() const noexcept;
    const std::optional<SkyPaint>& sky_paint() const noexcept;
    const std::optional<SunLight>& sun_light() const noexcept;
    const std::vector<EntityModel>& entity_models() const noexcept;
    // Separate brush-entity geometry; triangles remain in model-local coordinates.
    const std::vector<BrushModel>& brush_models() const noexcept;
    // Reads ambient_generic, env_soundscape_triggerable and trigger_soundscape.
    AudioScene audio_scene() const;
    const EntityStats& entity_stats() const noexcept;
    const LightmapAtlas& lightmap_atlas() const noexcept;
    LightmapAtlas release_lightmap_atlas() noexcept;
    // Valid after read(): keeps the lighting data in memory for repeated queries.
    BspProbe make_probe(bool bake_bank_lighting = false) const;
    // Nearest baked leaf ambient sample for each Source-space point.
    std::vector<AmbientCube> ambient_cubes(const std::vector<Vec3>& points) const;

private:
    std::array<int,4> bank_styles_{{-1,-1,-1,-1}};
    std::filesystem::path path_;
    std::vector<Triangle> world_triangles_;
    LightmapAtlas lightmap_atlas_;
    std::optional<SkyCamera> sky_camera_;
    std::optional<SkyPaint> sky_paint_;
    std::optional<SunLight> sun_light_;
    std::vector<EntityModel> entity_models_;
    std::vector<BrushModel> brush_models_;
    EntityStats entity_stats_;
    // World faces lying entirely at or above this height belong to the 3D skybox.
    float sky_floor_z_ = std::numeric_limits<float>::max();
    // Visibility clusters seen from the sky_camera (the 3D skybox room).
    std::vector<bool> sky_clusters_;
};

} // namespace gmod::source::bsp
