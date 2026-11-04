#include "dx12/GpuAssetCache.h"
#include "app/CpuAssetCache.h"
#include "app/ParsedDemoCache.h"
#include "demo/DemoCache.h"
#include "dx12/ResourceHelpers.h"
#include "ui/Settings.h"

#include <fstream>
#include <iostream>
#include <cstring>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void test_disk_cache(const std::filesystem::path& root) {
    using namespace gmod::demo;
    const std::atomic<bool> cancel{false};
    const auto input = root / L"original.dem";
    const auto moved = root / L"renamed.dem";
    { std::ofstream file(input, std::ios::binary); file << "abc"; }
    const auto hash = DemoCache::fingerprint(input, cancel);
    require(hash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "Disk cache must identify the complete content using SHA-256.");
    std::filesystem::copy_file(input, moved);
    require(hash == DemoCache::fingerprint(moved, cancel), "Renamed demos must share disk cache identity.");
    const auto time = std::filesystem::last_write_time(moved);
    { std::ofstream file(moved, std::ios::binary); file << "abd"; }
    std::filesystem::last_write_time(moved, time);
    require(hash != DemoCache::fingerprint(moved, cancel), "Equal-sized changed demos must not reuse cached parsing.");
    const auto key = DemoCache::key(hash, "parser-one");
    const auto other_key = DemoCache::key(hash, "parser-two");
    require(key != other_key, "Parser updates must invalidate disk cache identity.");
    const auto folder = root / L"disk-cache";
    DemoCache cache(folder);
    require(!cache.load(key, input, cancel, {}), "Unknown demos must miss the disk cache.");
    require(!cache.touch(key), "An in-memory demo without a disk snapshot must be saved when disk caching is enabled.");
    DemoData original;
    original.path = input;
    original.map_name = "test-map";
    original.client_name = "Recorder";
    original.tick_rate = 25;
    original.duration = 123;
    original.recorder_player = 0;
    original.recorder_camera.push_back({2, 1, 2, 3, 4, 5, true});
    original.models = {"models/player.mdl"};
    original.recorded_map_entities = {1235, 1398, 1400};
    original.model_paths_by_index[3] = "models/car.mdl";
    auto& player = original.players.emplace_back();
    player.name = "Player";
    player.samples.emplace_back().x = 42;
    player.first_name.push_back({1, "Character"});
    player.info.push_back({2, 50, 3, 0, "weapon_test"});
    player.active_weapon.push_back({3, 5, 8197, "weapon_test"});
    player.attacks.push_back({4, 0});
    auto& prop = original.props.emplace_back();
    prop.vehicle = true;
    prop.driver_attachment = std::array<float, 12>{1, 2, 3};
    prop.keys.emplace_back().x = 99;
    prop.vehicle_id = {{1, "b"}};
    prop.registration = {{2, "PD-1234"}, {4, "AB12"}};
    prop.plate_skin = {{3, "default_blue"}};
    original.doors.emplace_back().yaws.push_back({1, 90, 0, 0});
    original.effects.emplace_back().name = "blood";
    original.clothing.emplace_back().skin = "1";
    original.clothing_records[1].items.emplace_back().skin = 2;
    original.clothing_records[1].model_override = "costume";
    original.weapon_view_models["weapon_test"] = "view.mdl";
    original.clothing_records.at(1).explicit_parts = true;
    original.clothing_records.at(1).items[0].model_path = "models/recorded/jeans.mdl";
    original.clothing_records.at(1).items[0].model_skin = 7;
    original.weapons[8197].class_name.push_back({1, "weapon_test"});
    original.weapons[8197].attachments.push_back({2, "7,3"});
    original.class_holds["weapon_test"] = "pistol";
    original.ragdolls.emplace_back().keys.push_back({1, {2, 3}, {4, 5}});
    original.radio_sync.push_back({1, {2, 3}, {3}});
    original.radio_channel_names[2] = "Police";
    original.npc_uses.push_back({1, 2, 3});
    original.npc_notices.push_back({1, "Hello", "Bye"});
    original.players[0].look.push_back({2,10,125});
    original.npc_gui.push_back({2,true,true,"Garage","Select a vehicle",{"Vehicles","Close"}});
    original.npc_gui.push_back({3,false,false,"","",{}});
    original.log.push_back({1, LogKind::Chat, 0, 1, "Message", "Sender"});
    original.sounds.emplace_back().name = "alarm.wav";
    original.gun_sounds.emplace_back().impact = true;
    original.view_model.emplace_back().weapon = 8197;
    const auto voice = root / L"voice.wav";
    { std::ofstream file(voice, std::ios::binary); file << "voice data"; }
    original.voice.push_back({1, 2, 0, voice, false});
    require(cache.store(key, original, 1024 * 1024, cancel, {}), "Could not store native parsed demo.");
    const auto bytes = cache.usage().bytes;
    require(cache.usage().entries == 1 && bytes > 0, "Disk cache usage is incorrect.");
    require(cache.touch(key), "An in-memory cache hit must update disk LRU without decoding it again.");
    auto restored = DemoCache(folder).load(key, moved, cancel, {});
    require(restored && restored->path == moved && restored->duration == 123 && restored->recorder_player == 0
        && restored->recorder_camera[0].yaw == 5 && restored->map_name == original.map_name
        && restored->players[0].samples[0].x == 42 && restored->players[0].first_name[0].text == "Character"
        && restored->players[0].info[0].weapon == "weapon_test"
        && restored->players[0].active_weapon[0].handle == 8197
        && restored->props[0].driver_attachment->at(2) == 3 && restored->props[0].keys[0].x == 99
        && restored->props[0].vehicle_id[0].text == "b"
        && restored->props[0].registration[1].text == "AB12"
        && restored->props[0].plate_skin[0].text == "default_blue"
        && restored->recorded_map_entities == original.recorded_map_entities
        && restored->doors[0].yaws[0].yaw == 90 && restored->effects[0].name == "blood"
        && restored->clothing[0].skin == "1" && restored->clothing_records.at(1).items[0].skin == 2
        && restored->clothing_records.at(1).explicit_parts
        && restored->clothing_records.at(1).items[0].model_path == "models/recorded/jeans.mdl"
        && restored->clothing_records.at(1).items[0].model_skin == 7
        && restored->weapon_view_models.at("weapon_test") == "view.mdl"
        && restored->weapons.at(8197).attachments[0].text == "7,3"
        && restored->class_holds.at("weapon_test") == "pistol"
        && restored->ragdolls[0].keys[0].ang[1] == 5 && restored->radio_sync[0].muted[0] == 3
        && restored->radio_channel_names.at(2) == "Police" && restored->npc_uses[0].entity == 3
        && restored->npc_notices[0].exit_text == "Bye" && restored->log[0].text == "Message"
        && restored->players[0].look[0].time == 2 && restored->players[0].look[0].yaw == 125
        && restored->npc_gui.size() == 2 && restored->npc_gui[0].visible
        && restored->npc_gui[0].options[0] == "Vehicles" && restored->npc_gui[0].inferred
        && !restored->npc_gui[1].visible && restored->npc_gui[1].time == 3
        && restored->sounds[0].name == "alarm.wav" && restored->gun_sounds[0].impact
        && restored->view_model[0].weapon == 8197,
        "Disk snapshots must restore all playback data, not a subset.");
    std::ifstream clip(restored->voice[0].file, std::ios::binary);
    const std::string clip_text{std::istreambuf_iterator<char>(clip), std::istreambuf_iterator<char>()};
    clip.close();
    require(clip_text == "voice data" && !restored->voice[0].proximity, "Cached voice data did not survive restoration.");
    require(!cache.load(other_key, input, cancel, {}), "Changed parser must not use the old entry.");
    require(!cache.store(other_key, original, bytes - 1, cancel, {}) && cache.usage().entries == 1,
        "Oversized entries must not evict existing cache or exceed the budget.");
    require(cache.store(other_key, original, 2 * bytes, cancel, {}), "Cache must fit exactly at its limit.");
    std::filesystem::last_write_time(folder / (key + ".cache"), time - std::chrono::hours(2));
    std::filesystem::last_write_time(folder / (other_key + ".cache"), time - std::chrono::hours(1));
    restored = cache.load(key, moved, cancel, {});
    cache.trim(bytes);
    require(cache.usage().entries == 1 && cache.load(key, input, cancel, {})
        && !cache.load(other_key, input, cancel, {}), "Eviction must use last access, not creation order.");
    {
        std::fstream file(folder / (key + ".cache"), std::ios::binary | std::ios::in | std::ios::out);
        file.put('X');
    }
    bool corrupt_rejected = false;
    try { cache.load(key, input, cancel, {}); }
    catch (const std::runtime_error&) { corrupt_rejected = true; }
    require(corrupt_rejected, "Corrupt disk snapshots must not reach playback.");
    const std::atomic<bool> cancelled{true};
    bool stopped = false;
    try { cache.store(other_key, original, 2 * bytes, cancelled, {}); }
    catch (const std::runtime_error&) { stopped = true; }
    require(stopped, "Cache writing must respect cancellation.");
    { std::ofstream file(folder / "unrelated.txt"); file << "keep"; }
    cache.clear();
    require(cache.usage().entries == 0 && std::filesystem::exists(restored->voice[0].file)
        && std::filesystem::exists(folder / "unrelated.txt") && std::filesystem::exists(input),
        "Clearing disk cache must preserve active voice files, original demos and unrelated files.");
    for (const auto& file : std::filesystem::directory_iterator(folder))
        require(file.path().extension() != L".pending", "Cancelled writes must not leave partial cache files.");
    const auto extracted = restored->voice_store->directory;
    restored.reset();
    require(!std::filesystem::exists(extracted), "Restored voice files must follow playback ownership.");
    std::filesystem::remove(folder / "unrelated.txt");
    std::filesystem::remove(folder);
    std::filesystem::remove(voice);
    std::filesystem::remove(input);
    std::filesystem::remove(moved);
    std::cout << "PASS: persistent content identity, complete snapshots, voices, LRU, exact budget, corruption and cancellation\n";
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path()
        / ("gmod-replay-gpu-cache-" + std::to_string(GetCurrentProcessId()));
    wchar_t appdata[32768]{};
    const auto length = GetEnvironmentVariableW(L"APPDATA", appdata, 32768);
    try {
        std::filesystem::create_directories(root);
        require(SetEnvironmentVariableW(L"APPDATA", root.c_str()) != FALSE, "Cannot isolate test settings.");
        gmod::ui::ViewerSettings defaults;
        require(!defaults.cache_gpu_assets, "GPU caching must default to off.");
        require(!defaults.cache_demos_on_disk, "Persistent caching must default to off.");
        require(defaults.demo_cache_gb == 25, "Disk cache budget must default to 25 GiB.");
        defaults.cache_gpu_assets = true;
        defaults.cache_demos_on_disk = true;
        defaults.demo_cache_gb = 37;
        require(defaults.save(), "Could not save enabled cache choice.");
        gmod::ui::ViewerSettings loaded;
        loaded.load();
        require(loaded.cache_gpu_assets, "Enabled cache setting did not persist.");
        require(loaded.cache_demos_on_disk && loaded.demo_cache_gb == 37, "Disk cache settings did not persist.");
        loaded.cache_gpu_assets = false;
        require(loaded.save(), "Could not save disabled cache choice.");
        loaded.cache_gpu_assets = true;
        loaded.load();
        require(!loaded.cache_gpu_assets, "Disabled cache setting did not persist.");
        const auto blocked = root / "blocked";
        { std::ofstream file(blocked); file << "not a directory"; }
        require(SetEnvironmentVariableW(L"APPDATA", blocked.c_str()) != FALSE, "Cannot isolate failed settings save.");
        require(!loaded.save(), "Settings save failure must be reported.");
        SetEnvironmentVariableW(L"APPDATA", length ? appdata : nullptr);
        test_disk_cache(root);

        const auto key = gmod::dx12::GpuAssetCache::content_key("abc", 3, "buffer:3:1");
        const unsigned char prefix[]{0xba, 0x78, 0x16, 0xbf};
        const auto digest = key.substr(std::string("buffer:3:1:").size());
        require(digest.size() == 32 && std::memcmp(digest.data(), prefix, sizeof(prefix)) == 0,
                "GPU cache must use the SHA256 content digest.");
        require(key == gmod::dx12::GpuAssetCache::content_key("abc", 3, "buffer:3:1"),
                "Identical buffers must share a cache key.");
        require(key != gmod::dx12::GpuAssetCache::content_key("abd", 3, "buffer:3:1"),
                "Changed content must not reuse a GPU buffer.");
        require(key != gmod::dx12::GpuAssetCache::content_key("abc", 3, "buffer:3:2"),
                "Different resource states must not share buffers.");

        const auto container = root / "content.gma";
        { std::ofstream file(container); file << "first"; }
        {
            gmod::app::ParsedDemoCache demos;
            gmod::demo::DemoData original;
            original.map_name = "first-map";
            original.players.emplace_back().name = "First player";
            const auto voices = root / "voice";
            std::filesystem::create_directory(voices);
            { std::ofstream file(voices / "clip.wav"); file << "retained voice"; }
            original.voice_store = std::make_shared<gmod::demo::VoiceStore>();
            original.voice_store->directory = voices;
            const auto version = gmod::dx12::GpuAssetCache::texture_source_key(container, false);
            demos.store(container, version, original);
            original.voice_store.reset();
            require(std::filesystem::exists(voices / "clip.wav"),
                    "Cached recordings must keep their decoded voice files alive.");
            auto playback = *demos.find(container);
            playback.players.front().name = "Changed by playback";
            require(demos.find(container)->players.front().name == "First player",
                    "Playback must not mutate cached parser output.");
            const auto second = root / "second.dem";
            { std::ofstream file(second); file << "second recording"; }
            original.map_name = "second-map";
            demos.store(second, gmod::dx12::GpuAssetCache::texture_source_key(second, false), original);
            require(demos.size() == 2 && demos.find(container)->map_name == "first-map",
                    "Opening another demo must retain the first parsed recording.");
            { std::ofstream file(second); file << "changed recording data"; }
            require(!demos.find(second) && demos.size() == 1,
                    "Changed recordings must invalidate parsed data.");
            bool rejected = false;
            try { demos.store(second, version, original); }
            catch (const std::runtime_error&) { rejected = true; }
            require(rejected, "A recording changed during parsing must not be cached.");
            demos.clear();
            require(demos.size() == 0, "Disabling cache must release all parsed recordings.");
            require(std::filesystem::exists(voices / "clip.wav"),
                    "Clearing cache must not remove voice files still owned by playback.");
            playback.voice_store.reset();
            require(!std::filesystem::exists(voices),
                    "Voice files must be removed after both cache and playback release them.");
            std::filesystem::remove(second);
        }
        const auto texture = container / "materials" / "test.vtf";
        const auto first_key = gmod::dx12::GpuAssetCache::texture_source_key(texture, false);
        { std::ofstream file(container); file << "second version"; }
        require(first_key != gmod::dx12::GpuAssetCache::texture_source_key(texture, false),
                "Changed archive must invalidate cached textures.");
        require(first_key != gmod::dx12::GpuAssetCache::texture_source_key(texture, true),
                "Different texture decoders must not share aliases.");

        using gmod::app::CpuAssetCache;
        using gmod::source::model::BodySelection;
        BodySelection selection;
        const auto mesh_key = [&](const BodySelection& value, int skin = 0) {
            return CpuAssetCache::mesh_key("models/test.mdl", texture, texture, texture, skin, value);
        };
        const auto base_mesh_key = mesh_key(selection);
        const auto different = [&](const BodySelection& value) {
            require(base_mesh_key != mesh_key(value), "Changed mesh selection reused the base mesh.");
        };
        require(base_mesh_key == mesh_key(selection), "Identical mesh selections must share keys.");
        require(base_mesh_key != mesh_key(selection, 1), "Different skins must not reuse meshes.");
        selection.body = 1; different(selection); selection = {};
        selection.apply_idle_pose = true; different(selection); selection = {};
        selection.normal_offset = 0.1f; different(selection); selection = {};
        selection.bodygroup_normal_offsets = {{"badge_num",0.25f}}; different(selection); selection = {};
        selection.overrides.emplace_back(1, 2); different(selection); selection = {};
        selection.named_overrides.emplace_back("badge_num", 1); different(selection); selection = {};
        selection.fallback_bone = "hand"; different(selection); selection = {};
        std::vector<std::string> skeleton{"root", "hand"};
        selection.skeleton = &skeleton; different(selection);
        auto copied_skeleton = skeleton;
        const auto skeleton_key = mesh_key(selection);
        selection.skeleton = &copied_skeleton;
        require(skeleton_key == mesh_key(selection), "Skeleton identity must use values, not addresses.");
        copied_skeleton[1] = "head";
        require(skeleton_key != mesh_key(selection), "Changed skeleton reused the previous mesh.");
        selection = {};
        std::vector<std::array<float, 12>> matrices(1);
        selection.bone_matrices = &matrices; different(selection);
        const auto pose_key = mesh_key(selection);
        matrices[0][3] = 5.0f;
        require(pose_key != mesh_key(selection), "Changed baked pose reused the previous mesh.");
        selection = {};
        std::vector<std::pair<std::string, std::string>> remaps{{"original", "replacement"}};
        selection.material_remap = &remaps; different(selection);
        const auto remap_key = mesh_key(selection);
        remaps[0].second = "another";
        require(remap_key != mesh_key(selection), "Changed material remap reused the previous mesh.");
        selection = {};
        selection.bone_aliases = &remaps; different(selection);
        const auto alias_key = mesh_key(selection);
        remaps[0].second = "new_bone";
        require(alias_key != mesh_key(selection), "Changed bone aliases reused the previous mesh.");
        selection = {};
        { std::ofstream file(container); file << "third version of the model archive"; }
        require(base_mesh_key != mesh_key(selection), "Changed model archive must invalidate cached meshes.");
        CpuAssetCache cpu_cache;
        const auto source_version = cpu_cache.source_key(texture);
        require(source_version == cpu_cache.source_key(texture), "Source identity changed within a load.");
        require(cpu_cache.sources_current(), "Unchanged source archive should remain reusable.");
        { std::ofstream file(container); file << "fourth model archive revision with different size"; }
        require(!cpu_cache.sources_current(), "Changed archive must invalidate a prepared map.");
        cpu_cache.begin_load();
        require(source_version != cpu_cache.source_key(texture), "New load must recheck archive versions.");
        cpu_cache.meshes[base_mesh_key].vertices.resize(1);
        cpu_cache.meshes["other-map-model"].vertices.resize(2);
        cpu_cache.mesh_hits = 2;
        cpu_cache.materials[base_mesh_key].base_texture = "test/texture";
        cpu_cache.texture_headers[base_mesh_key].width = 512;
        cpu_cache.material_hits = cpu_cache.texture_header_hits = 1;
        require(cpu_cache.meshes.size() == 2, "CPU cache must retain different model variants.");
        cpu_cache = {};
        require(cpu_cache.meshes.empty() && cpu_cache.maps.empty() && cpu_cache.mesh_hits == 0
                    && cpu_cache.materials.empty() && cpu_cache.texture_headers.empty()
                    && cpu_cache.material_hits == 0 && cpu_cache.texture_header_hits == 0,
                "Disabling cache must release decoded assets and reset counters.");

        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        require(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))), "Cannot create DXGI factory.");
        Microsoft::WRL::ComPtr<IDXGIAdapter> warp;
        require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))), "Cannot create test adapter.");
        gmod::dx12::GpuAssetCache cache;
        require(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&cache.device))),
                "Cannot create test GPU device.");
        {
            auto first = gmod::dx12::create_buffer(cache.device.Get(), 4096, D3D12_HEAP_TYPE_DEFAULT,
                                                 D3D12_RESOURCE_STATE_COMMON);
            auto second = gmod::dx12::create_buffer(cache.device.Get(), 8192, D3D12_HEAP_TYPE_DEFAULT,
                                                  D3D12_RESOURCE_STATE_COMMON);
            cache.store(key, first);
            cache.store("second-asset", second);
            require(cache.find(key).Get() == first.Get(), "Cache did not return the same GPU allocation.");
        }
        require(cache.size() == 2 && cache.find(key) && cache.find("second-asset"),
                "Retaining another asset must not drop existing assets.");
        require(!cache.find("missing"), "Missing cache entry returned a resource.");
        cache.clear();
        require(cache.size() == 0 && !cache.device && !cache.adapter && cache.hits == 0,
                "Disabling cache must release resources and device ownership.");
        std::filesystem::remove(container);
        std::filesystem::remove(blocked);
        std::filesystem::remove(root / "GModReplay" / "settings.ini");
        std::filesystem::remove(root / "GModReplay");
        std::filesystem::remove(root);
        std::cout << "PASS: CPU/GPU asset identity, mesh selections, multi-scene retention, invalidation, clear and settings persistence\n";
        return 0;
    } catch (const std::exception& error) {
        SetEnvironmentVariableW(L"APPDATA", length ? appdata : nullptr);
        std::cerr << "FAIL: " << error.what() << "; diagnostics: " << root.string() << "\n";
        return 1;
    }
}
