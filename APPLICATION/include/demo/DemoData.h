#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace gmod::demo {

// Door states as networked by the engine (m_eDoorState).
enum class DoorState : int { Closed = 0, Opening = 1, Open = 2, Closing = 3, Ajar = 4 };

struct PlayerSample {
    float time = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    int model = -1;     // index into DemoData::models
    int segment = 0;    // samples in different segments are never interpolated
    int flags = 0;      // networked m_fFlags (FL_ONGROUND = 1, FL_DUCKING = 2) | PlayerFlag*
};

// Not engine flags: set on samples generated while the player rides a vehicle.
constexpr int PlayerFlagSeated = 1 << 20;
constexpr int PlayerFlagDriver = 1 << 21;
constexpr int PlayerFlagInCar = 1 << 22;  // the seat belongs to a vehicle (not a free-standing chair)

constexpr float player_eye_height(int flags) {
    // PERP's CrouchHeight hook sets SetViewOffsetDucked(Vector(0, 0, 40)).
    return (flags & (PlayerFlagSeated | 2)) != 0 ? 40.0f : 64.0f;
}

struct PresenceEvent {
    float time = 0.0f;
    bool present = false;
};

// Sparse per-player state; a key is stored whenever any field changes.
struct PlayerInfoKey {
    float time = 0.0f;
    int health = -1;
    int team = -1;
    int life_state = -1;
    std::string weapon;
};

struct TimedInt {
    float time = 0.0f;
    int value = 0;
};

struct PlayerLookKey {
    float time = 0, pitch = 0, yaw = 0;
};

struct PlayerVehicleKey {
    float time = 0.0f;
    int vehicle = -1;
    int parent = -1;
};

struct TimedText {
    float time = 0.0f;
    std::string text;
};

// A networked weapon swap: the entity index of the active weapon, -1 for none.
struct ActiveWeaponKey {
    float time = 0.0f;
    int weapon = -1;         // entity index
    int handle = -1;         // index + (serial << 13): unique even when an index is reused
    std::string class_name;  // class of exactly that handle, empty when never learned
};

// Engine entity handle as the demo records it (13-bit index, 10-bit serial).
constexpr int weapon_handle(int index, int serial) {
    return serial < 0 ? index : index | (serial << 13);
}

// ph_playanim: the server starts an act (emote) on a player; ph_stopanim ends it.
struct EmoteKey {
    float time = 0.0f;
    int sequence = -1;  // the server's sequence number of the player model; -1 = stopped
    bool once = true;   // plays through once (dontloop); otherwise loops until stopped
};

struct DemoPlayer {
    int id = -1;
    // A server NPC (vendor, dealer...) shown like a standing player; not a real player.
    bool npc = false;
    int entity = -1;
    int user_id = -1;
    std::string name;
    std::string steam_id;
    std::string steam_id64;
    float connected_at = 0.0f;
    float disconnected_at = -1.0f;
    int voice_clips = 0;
    std::vector<PlayerSample> samples;
    std::vector<PlayerLookKey> look; // networked eye angles, independent of reconstructed seat/body facing
    std::vector<PresenceEvent> presence;
    // Intervals where the networked origin is (0,0,0): the player is parented to a vehicle
    // and the engine stops sending a world position.
    std::vector<PresenceEvent> seated;
    std::vector<PlayerVehicleKey> vehicle_links;
    std::vector<PlayerInfoKey> info;
    // Roleplay identity and job, all networked by the server (NWVars and the player resource).
    std::vector<TimedText> first_name;  // rp_fname
    std::vector<TimedText> last_name;   // rp_lname
    std::vector<TimedInt> job_team;     // m_iTeam of the player resource
    std::vector<TimedInt> police_rank;
    std::vector<TimedInt> medic_rank;
    std::vector<TimedInt> firefighter_rank;
    std::vector<TimedInt> outfit;       // clothing-data store index worn from `time`
    std::vector<TimedInt> ragdoll;      // entity index of the ragdoll the player is, 0 = none
    std::vector<TimedInt> radio_channel;  // government radio channel being transmitted on, 0 = none
    // NWVars behind the server's own custom gestures (sh_animations.lua).
    std::vector<TimedInt> restrained;  // 0 free, 1 handcuffs, 2 zip ties
    std::vector<TimedInt> phone_open;
    std::vector<TimedInt> phone_call;
    std::vector<EmoteKey> emotes;      // ph_playanim / ph_stopanim broadcasts, in time order
    std::vector<ActiveWeaponKey> active_weapon;
    std::vector<float> shots;           // muzzle-flash parity changes of the active weapon
    std::vector<TimedInt> attacks;      // recorded player animation events: primary=0, secondary=1
    std::vector<TimedInt> reloads;      // recorded reload animation events: start=3, loop=4, end=5
};

// The latest key at or before `time`, or nullptr when the demo has none yet.
const PlayerInfoKey* player_info_at(const DemoPlayer& player, float time);
bool player_incapacitated(const DemoPlayer& player, float time);
const TimedInt* player_attack_at(const DemoPlayer& player, float time);
const TimedInt* player_reload_at(const DemoPlayer& player, float time);

struct PropKey {
    float time = 0.0f;
    bool present = true;
    bool has_position = false;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    // Entity angles in degrees: yaw, pitch and roll. Vehicles and props carry the entity's
    // networked rotation; `has_yaw` is false only when none was ever networked.
    bool has_yaw = false;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
    int parent = -1;  // seats: entity index of the vehicle they are attached to
};

// Networked pose parameters of a vehicle (steering, wheel spin and suspension height of each
// wheel), normalized 0..1 and indexed like the model's pose parameter table.
constexpr std::size_t VehiclePoseCount = 24;
struct PoseKey {
    float time = 0.0f;
    std::array<float, VehiclePoseCount> value{};
};

// A vehicle's paint colour (the `color` NWVar, 0..1 per channel).
struct ColorKey {
    float time = 0.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f;
};

struct OwnerKey {
    float time = 0.0f;
    int owner = -1;  // entity index of the occupant, -1 when empty
};

// A player-placed physics prop, a scripted prop (furniture, items, lamps...), a vehicle, or an
// invisible vehicle seat.
struct DemoProp {
    struct Animation { float time=0; int sequence=0; float cycle=0; float rate=1; };
    std::vector<Animation> animations; // recorded bank shutter sequences only
    int entity = -1;
    int model = -1;  // -1 for seats, which are never drawn
    int skin = 0;    // m_nSkin: paint finish or livery of a vehicle
    int body = 0;    // m_nBody
    bool vehicle = false;
    std::optional<std::array<float,12>> driver_attachment;
    bool brush = false;
    bool map_attached = false;
    std::optional<std::array<float,12>> map_rotation;
    int map_created_id = -1;
    bool back_gun = false; // recorded prop_back_gun attached to a player
    std::vector<TimedInt> back_weapon; // networked swep entity used to suppress the held gun
    bool seat = false;      // prop_vehicle_prisoner_pod: occupant bookkeeping only
    std::vector<PropKey> keys;
    std::vector<TimedInt> hidden;
    std::vector<OwnerKey> owners;
    std::vector<PoseKey> poses;    // vehicles: networked pose parameters over time
    std::vector<ColorKey> colors;  // vehicles: paint colour over time
    std::vector<TimedText> vehicle_id;
    std::vector<TimedText> registration;
    std::vector<TimedText> plate_skin;
    std::vector<TimedInt> vehicle_owner; // vehicles: entity index of the owning player, -1 when unknown
};

struct DoorKey {
    float time = 0.0f;
    DoorState state = DoorState::Closed;
};

// The door's networked rotation (yaw, degrees): which way it really swung and how far.
struct DoorYawKey {
    float time = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
};

// A prop_door_rotating networked in the demo; `origin` equals the origin of the
// map entity it belongs to.
struct DoorHiddenKey {
    float time = 0.0f;
    bool hidden = false;  // EF_NODRAW: a door blown off its frame is no longer drawn
};

struct DemoDoor {
    int entity = -1;
    bool has_origin = false;
    float origin[3]{};
    std::vector<DoorKey> keys;
    std::vector<DoorYawKey> yaws;  // empty when the server never networked a rotation
    std::vector<DoorHiddenKey> hidden;
};

struct DemoEffect {
    float time = 0.0f;
    std::string kind;
    std::string name;
    std::array<float, 3> origin{};
    std::array<float, 3> start{};
    std::array<float, 3> normal{};
    int entity = 0;
    float scale = 0.0f;
    float material = 0.0f;
};

constexpr float DecalLifetime = 60.0f;
constexpr float DecalFadeDuration = 5.0f;
float decal_opacity(float event_time, float playback_time);

constexpr float BloodMistLifetime = 0.65f;
struct BloodMistParticle {
    std::array<float, 3> position{};
    float radius = 0.0f;
    float opacity = 0.0f;
};
BloodMistParticle blood_mist_particle(const DemoEffect& effect, int particle, float playback_time);

constexpr float SmokeLifetime = 0.8f;
struct SmokeParticle {
    std::array<float,3> position{};
    std::array<float,3> color{};
    float radius = 0.0f;
    float opacity = 0.0f;
};
bool surface_smoke_effect(const DemoEffect& effect);
SmokeParticle smoke_particle(const DemoEffect& effect, int particle, float playback_time, bool muzzle);

struct DemoClothingEvent {
    float time = 0.0f;
    std::string name;
    int entity = -1;
    int entry = -1;
    int model_index = -1;
    std::string model_path;
    std::string skin;
    std::string visible;
};

// The recording client's own camera (Source space, degrees).
struct CameraKey {
    float time = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
    // False while the client is parented to a vehicle: the engine reports no world origin
    // then; view angles remain available but are relative to the vehicle/seat parent.
    bool origin_valid = true;
};

// A voice transmission decoded to a WAV file by the reference parser.
struct DemoVoiceClip {
    float time = 0.0f;
    float duration = 0.0f;
    int player = -1;  // index into DemoData::players
    std::filesystem::path file;
    bool proximity = true;
};

// Owns the temporary directory holding the decoded voice clips.
struct VoiceStore {
    std::filesystem::path directory;
    VoiceStore() = default;
    VoiceStore(const VoiceStore&) = delete;
    VoiceStore& operator=(const VoiceStore&) = delete;
    ~VoiceStore() {
        if (directory.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

// One worn item of a character, as stored by the server's clothing-data store.
struct ClothingItem {
    int type = 0;         // clothing type registry index
    int skin = 1;         // 1-based skin of that type
    int data_number = 0;
    unsigned char color[4]{255, 255, 255, 255};
    std::string model_path;
    int model_skin = 0;
};

struct ClothingRecord {
    bool female = false;
    int model = 0;        // character model number; also the skin of the bare body parts
    int skin = 0;
    int face = 0;
    // A costume model (ClothingData.ModelOverride): the entity takes this model instead of a
    // character body, so no bare body parts are attached (cl_clothing.lua AttachClothing).
    std::string model_override;
    std::vector<ClothingItem> items;
    bool explicit_parts = false;  // legacy bonemerge entries already include the bare body parts
};

struct WeaponEntityState {
    std::vector<TimedText> attachments;  // networked AttachmentListString ("87,54,7,44")
    std::vector<TimedText> class_name;  // weapon class from the spawn rows
    std::vector<TimedText> hold_type;   // networked m_strHoldType
    std::vector<float> shots;
    // Firearm-base Lua NetworkVars (only meaningful for firearms): sights up (aiming down
    // sights or scoped), passive stance, reloading. 1 = on.
    std::vector<TimedInt> aiming;
    std::vector<TimedInt> passive;
    std::vector<TimedInt> reloading;
    std::vector<TimedInt> grenade;      // grenade-base int slot 1: normal/pin/spoon/thrown
    std::vector<TimedInt> owner;        // carrying player's entity index, -1 = nobody
    float gone = -1.0f;                 // time the weapon entity was deleted, -1 = never
};

// The recording player's first-person view model (view model slot 0).
struct ViewModelKey {
    float time = 0.0f;
    int owner = -1;     // entity index of the player it belongs to
    int model = -1;     // demo model table index (0 = none)
    int sequence = 0;
    float rate = 1.0f;
    int new_sequence = 0;  // parity that flips when the same sequence restarts
    int weapon = -1;       // weapon handle
    int effects = 0;       // EF_NODRAW (32) hides it
    int animation_parity = 0; // independent animation restart counter
};

// One networked ragdoll pose: world position and Source angles of every physics object
// (three floats each, one object per ragdoll solid).
struct RagdollKey {
    float time = 0.0f;
    std::vector<float> pos;
    std::vector<float> ang;
};

struct DemoRagdoll {
    int entity = -1;
    int serial = -1;
    int model = -1;  // index into DemoData::model_paths_by_index
    std::vector<RagdollKey> keys;
};

struct RagdollPose {
    int model = -1;
    const RagdollKey* key = nullptr;
};

// The recording player's government radio, as the server synced it.
struct RadioSync {
    float time = 0.0f;
    std::vector<int> channels;  // channel ids joined
    std::vector<int> muted;     // channel ids muted
};

struct NpcUse {
    float time = 0.0f;
    int npc_id = 0;   // NPC_DATABASE id
    int entity = 0;   // the NPC entity talked to
};

struct NpcNotice {
    float time = 0.0f;
    std::string text;
    std::string exit_text;
};

struct NpcGuiPage {
    float time = 0;
    bool visible = false;
    bool inferred = false;
    std::string title, text;
    std::vector<std::string> options;
};

// A line of the in-game text log: chat, OOC, narration, announcements, notices.
enum class LogKind : int { Chat = 0, Ooc = 1, Narration = 2, Npc = 3, Announcement = 4, Notify = 5, Connection = 6 };

struct DemoLogLine {
    float time = 0.0f;
    LogKind kind = LogKind::Chat;
    int category = 0;  // the chat prefix id (local, OOC, radio...), or notice type
    int entity = 0;    // speaker entity; 0 when there is none
    std::string text;
    std::string sender;  // NPC name
};

// One sound the server started, changed or stopped (svc_Sounds). The name is the file the
// server's sound-precache table lists for the sound's index.
struct DemoSound {
    float time = 0.0f;
    int entity = 0;
    int channel = 0;
    int flags = 0;  // SND_* bits: 1 change volume, 2 change pitch, 4 stop, 8 spawning
    float volume = 1.0f;
    int level = 75;
    int pitch = 100;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int index = 0;      // sound precache index
    std::string name;   // empty when the index is not in the table
    bool loop = false;  // later changed or stopped by the server, so it is a loop (engine, siren)
};

struct FireKey {
    float time=0; int entity=-1, serial=0;
    std::array<float,3> origin{}; float scale=1; bool active=true;
};

struct DemoData {
    std::vector<FireKey> fires;
    struct BankLight { float time=0; int style=0; float value=1; };
    std::vector<BankLight> bank_lights;
    std::filesystem::path path;
    // PERP data (jobs, weapons, clothing) built from the server Lua this demo was recorded
    // with; empty when it could not be (then the shared data folder is used).
    std::filesystem::path perp_data;
    std::string perp_data_note;
    // Perpheads gun sounds the server sent (ph_bullet_sound / ph_bullet_impact_sound): the
    // GunSounds registration index and where it played. Sorted by time.
    struct GunSound {
        float time = 0.0f;
        int index = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        bool impact = false;
        int player = -1;  // the shooter (the player whose eyes were at the position), -1 if none
    };
    std::vector<GunSound> gun_sounds;
    std::string map_name;
    std::string client_name;  // the player who recorded the demo
    int recorder_player = -1; // index into `players` whose view the recording shows, if found
    std::vector<CameraKey> recorder_camera;  // sorted by time
    // Server ticks per second. Entity ticks use the server's interval; the demo header's
    // own tick count is not reliable.
    float tick_rate = 25.0f;
    float duration = 0.0f;
    std::vector<std::string> models;
    std::unordered_map<int, std::string> model_paths_by_index;
    std::vector<DemoPlayer> players;
    std::vector<DemoProp> props;
    // MapCreationIDs observed anywhere in this recording, including non-animated map props.
    // Presence confirms startup scene membership; leaving PVS does not disable a scene.
    std::vector<int> recorded_map_entities;
    std::vector<DemoDoor> doors;
    std::vector<DemoEffect> effects;
    std::vector<DemoClothingEvent> clothing;
    // Weapon class name -> first-person model path seen on its entity.
    std::unordered_map<std::string, std::string> weapon_view_models;
    std::vector<DemoVoiceClip> voice;  // sorted by time
    std::shared_ptr<VoiceStore> voice_store;
    // Clothing-data store index -> what the server recorded for it.
    std::unordered_map<int, ClothingRecord> clothing_records;
    // Weapon handle -> class name and networked hold type over time.
    std::unordered_map<int, WeaponEntityState> weapons;
    // Weapon class -> the hold type nearly every entity of that class reported; used when
    // one entity never networked its own (its value equalled the class default).
    std::unordered_map<std::string, std::string> class_holds;
    std::vector<DemoRagdoll> ragdolls;
    std::vector<RadioSync> radio_sync;                       // sorted by time
    std::unordered_map<int, std::string> radio_channel_names;  // channels the demo created
    std::vector<NpcUse> npc_uses;
    std::vector<NpcNotice> npc_notices;
    std::vector<NpcGuiPage> npc_gui; // complete page snapshots, including explicit closes
    std::vector<DemoLogLine> log;     // chat and notices, sorted by time
    std::vector<DemoSound> sounds;    // sorted by time
    std::vector<ViewModelKey> view_model;  // sorted by time
};

int player_index_at(const DemoData& demo, int entity, float time);

struct HeldWeapon {
    int entity = -1;
    int handle = -1;
    std::string class_name;
    std::string hold_type;  // empty when never networked and the class has none either
    bool hold_from_class = false;
};

std::vector<float> recorded_shot_times(const DemoData& demo, int player,
                                      const std::vector<float>& view_model_shots = {});

// Firearm state of a weapon handle at `time`.
struct WeaponFlags {
    bool aiming = false;
    bool passive = false;
    bool reloading = false;
    float aiming_since = -1.0f;  // time the sights went up (aiming only)
    float reloading_since = -1.0f;  // time the reload began (reloading only)
};
WeaponFlags weapon_flags_at(const DemoData& demo, int handle, float time);
const TimedInt* grenade_state_at(const DemoData& demo, int handle, float time);
// Gun attachment ids fitted to the weapon at `time`; `known` is false when the demo never
// networked a list for it.
std::vector<int> weapon_attachments_at(const DemoData& demo, int handle, float time, bool& known);
// Distinct weapon classes the player carries at `time` (by networked weapon owner).
std::vector<std::string> inventory_at(const DemoData& demo, const DemoPlayer& player, float time);
// The view model key in effect at `time`, null before the first.
const ViewModelKey* view_model_at(const DemoData& demo, float time);

// The weapon the player has out at `time` (entity -1 when none).
HeldWeapon held_weapon_at(const DemoData& demo, const DemoPlayer& player, float time);
// Latest value at or before `time`; `fallback` when the timeline has not started.
int timed_int_at(const std::vector<TimedInt>& keys, float time, int fallback);
// The act the server last started on the player and not yet stopped; null when none.
const EmoteKey* emote_at(const DemoPlayer& player, float time);
const std::string& timed_text_at(const std::vector<TimedText>& keys, float time);
// "First Last" of the character the player is roleplaying at `time`, empty when unknown.
std::string character_name_at(const DemoPlayer& player, float time);
// True when a shot was fired by the player within [time - window, time].
bool fired_recently(const DemoPlayer& player, float time, float window);
// The ragdoll the player is at `time` (a knockout or corpse), its latest pose at or before
// `time`. False when the player is on their feet.
bool ragdoll_pose_at(const DemoData& demo, const DemoPlayer& player, float time, RagdollPose& pose);
// The recorder's radio state at `time`; null before the server first synced it.
const RadioSync* radio_sync_at(const DemoData& demo, float time);

struct PlayerPose {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    int model = -1;
    // Ground-plane velocity in units/second, from the recorded positions.
    float vx = 0.0f;
    float vy = 0.0f;
    float vz = 0.0f;
    int flags = 0;
};

struct PropPose {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float roll = 0.0f;
};

// Return false when the entity is not in the world at `time`.
bool sample_player(const DemoPlayer& player, float time, PlayerPose& pose);
// Interpolated recording camera; false when the demo has no camera data.
bool sample_recorder_camera(const DemoData& demo, float time, CameraKey& camera);
bool sample_player_look(const DemoData& demo, int player, float time, float& pitch, float& yaw);
bool sample_prop(const DemoProp& prop, float time, PropPose& pose);
// The two networked pose-parameter snapshots around `time` and how far between them it is
// (0..1); false when the vehicle never networked any.
bool sample_vehicle_params(const DemoProp& prop, float time, std::array<float, VehiclePoseCount>& before,
                           std::array<float, VehiclePoseCount>& after, float& fraction);
// Paint colour at `time`; white when none was networked.
void sample_vehicle_color(const DemoProp& prop, float time, float rgb[3]);
// The door's recorded yaw at `time` (interpolated the short way round); false when none.
bool door_yaw_at(const DemoDoor& door, float time, float& yaw);
bool door_angles_at(const DemoDoor& door, float time, std::array<float, 3>& angles);
// True while the door is not drawn (blown off its frame).
bool door_hidden_at(const DemoDoor& door, float time);
// 0 = closed, 1 = fully open. `seconds_to_open` is how long the swing takes.
float door_openness(const DemoDoor& door, float time, float seconds_to_open);

using ProgressFn = std::function<void(float percent, const std::string& text)>;

// Scans the Source demo container in-process, then temporarily uses gmoddem for
// entity state and Perpheads events. Intermediate files are removed afterwards.
// Throws std::runtime_error on failure or "cancelled" when `cancel` becomes true.
// With `perp_data_cache`, the parser also finds (building once per server Lua version) the
// PERP data matching the demo and reports it in DemoData::perp_data.
DemoData load_demo(
    const std::filesystem::path& demo_path,
    const std::filesystem::path& parser_exe,
    const ProgressFn& progress,
    const std::atomic<bool>& cancel,
    const std::filesystem::path& perp_data_cache = {});

const NpcGuiPage* npc_gui_at(const DemoData& data, float time);
// Merge partial updates at the same tick before interpolating vehicles/seats.
void coalesce_vehicle_updates(DemoProp& prop);
void rebind_vehicle_seats(DemoData& data);

// Finds gmoddem.exe in `application_root\tools`, then in the sibling parser project.
std::filesystem::path find_parser(const std::filesystem::path& application_root);

} // namespace gmod::demo
