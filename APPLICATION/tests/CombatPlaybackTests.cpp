#include "demo/DemoData.h"
#include "demo/FireEffects.h"
#include "demo/BackWeapons.h"
#include "demo/MeleePlayback.h"
#include "demo/Perpheads.h"
#include "demo/ModelIdentifiers.h"
#include "source/WorldTrace.h"
#include "audio/SoundCatalog.h"
#include "audio/StoppedSoundEvents.h"
#include "ui/ScopeProjection.h"
#include "app/VehicleCamera.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void near(float actual, float expected) {
    require(std::fabs(actual-expected) < 0.001f,"Unexpected numeric test result.");
}
}

int main(int argc, char** argv) try {
    using namespace gmod::demo;
    {
        DemoProp vehicle;
        vehicle.vehicle_id = {{1, "b"}};
        vehicle.registration = {{2, "PD-1234"}, {4, "AB12"}, {6, ""}};
        vehicle.plate_skin = {{3, "blue"}, {5, ""}};
        require(vehicle_plate_at(vehicle, 0).id.empty(), "Vehicle ID leaked before recording");
        require(vehicle_plate_at(vehicle, 1).registration.empty(), "Plate leaked before recording");
        require(vehicle_plate_at(vehicle, 4.5f).registration == "AB12", "Plate change lost");
        require(vehicle_plate_at(vehicle, 2.5f).registration == "PD-1234", "Plate rewind failed");
        require(vehicle_plate_at(vehicle, 6).registration.empty(), "Removed plate remains visible");
        require(vehicle_plate_states(vehicle).size() == 4, "Plate variant timeline lost");
        PerpheadsRegistry registry;
        registry.plate_skins["blue"] = {"models/blue", {0.1f, 0.2f, 0.3f}};
        const auto odd = plate_appearance(registry, {"b", "PD-1234", "blue"});
        require(odd.model.ends_with("skinned"), "Odd plate uses wrong model");
        require(odd.materials[2].second.ends_with("/dash"), "Plate dash material lost");
        require(odd.materials.back().second == "models/blue", "Plate background skin lost");
        near(odd.color[1], 0.2f);
        const auto even = plate_appearance(registry, {"b", "AB12", ""});
        require(even.model.ends_with("_even") && even.materials.size() == 6, "Even plate layout incorrect");
        require(even.materials[0].second.ends_with("/space")
            && even.materials[1].second.ends_with("/a") && even.materials[5].second.ends_with("/space"),
            "Short plate is not centred");
        bool rejected = false;
        try { plate_appearance(registry, {"b", "BAD!", ""}); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Unsupported plate silently accepted");
        rejected = false;
        try { plate_appearance(registry, {"b", "AB12", "unknown-skin"}); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Unknown plate skin silently replaced with a default");
        PropPose pose; pose.x = 10; pose.y = 20; pose.z = 30; pose.yaw = 90;
        PlatePlacement placement; placement.pos = {100, 0, 5};
        const auto transform = plate_transform(pose, placement);
        near(transform[3], 10); near(transform[7], 120); near(transform[11], 35);
        ClothingType type; type.index = 11; type.model = "police.mdl"; type.government = true;
        type.data_number_bodygroup = "badge_num";
        type.data_number_material_base = "models/badge/n";
        registry.clothing[11] = type;
        ClothingRecord record;
        ClothingItem item; item.type = 11; item.data_number = 709;
        record.items = {item};
        auto parts = compose_outfit(registry, record);
        require(parts.size() == 1 && parts[0].data_number == 709, "Badge number lost");
        require(parts[0].sub_materials[0].second == "models/badge/n0"
            && parts[0].sub_materials[1].second == "models/badge/n7"
            && parts[0].sub_materials[2].second == "models/badge/n0"
            && parts[0].sub_materials[3].second == "models/badge/n9", "Uniform digits incorrect");
        record.items[0].data_number = 710;
        require(!(parts[0] == compose_outfit(registry, record)[0]), "Distinct badges share an outfit variant");
        record.items[0].data_number = 0;
        parts = compose_outfit(registry, record);
        require(parts[0].data_number == 0 && parts[0].sub_materials.empty()
            && parts[0].number_bodygroup == "badge_num", "Missing badge should hide its bodygroup");
        record.items[0].data_number = 10000;
        rejected = false;
        try { compose_outfit(registry, record); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Invalid uniform number silently accepted");
        std::cout << "PASS: plate timelines, odd/even centring, original skins, transforms and uniform badge digits\n";
    }
    {
        ViewModelKey before,key;before.model=key.model=1016;before.sequence=key.sequence=2;
        require(!view_model_restarted(before,key),"Unchanged view model restarts");
        key.animation_parity=1;require(view_model_restarted(before,key),"Animation-only fist restart lost");
        key.animation_parity=0;key.new_sequence=1;require(view_model_restarted(before,key),"Sequence parity restart lost");
        DemoPlayer player;player.attacks={{2,0}};
        merge_melee_attacks(player,{{1,0},{2.02f,0},{3,1}});
        require(player.attacks.size()==3 && player.attacks.front().time==1 && player.attacks.back().value==1,
            "Recorder melee reconstruction duplicates or loses attacks");
        DemoData data;DemoSound sound;sound.time=2;sound.entity=50;sound.name="npc/vort/claw_swing1.wav";
        data.sounds={sound};const std::vector<std::string> files{sound.name};
        require(has_recorded_melee_sound(data,50,70,2.1f,files),"Recorded melee sound not deduplicated");
        require(!has_recorded_melee_sound(data,51,71,2.1f,files),"Another player's sound suppresses this swing");
        require(!has_recorded_melee_sound(data,50,70,3,files),"Earlier sound suppresses future swing");
        data.sounds[0].entity=0;data.sounds[0].x=10;
        const std::array<float,3> origin{0,0,0},far{1000,0,0};
        require(has_recorded_melee_sound(data,50,70,2,files,&origin),"Unattached recorded hit sound duplicated");
        require(!has_recorded_melee_sound(data,50,70,2,files,&far),"Distant unattributed sound suppresses this hit");
        std::cout << "PASS: view-model restart counters, inferred melee attacks and sound deduplication\n";
        PlayerPose target;target.x=40;
        near(fist_player_fraction({0,0,60},{50,0,60},target),0.36f);
        target.y=40;near(fist_player_fraction({0,0,60},{50,0,60},target),1);
        target.y=0;target.x=-50;near(fist_player_fraction({0,0,60},{50,0,60},target),1);
        target.x=40;near(fist_player_fraction({0,0,80},{50,0,80},target),1);
        target.flags=2;near(fist_player_fraction({0,0,60},{50,0,60},target),1);
        target.flags=0;target.x=10;near(fist_player_fraction({0,0,60},{50,0,60},target),0);
        std::cout << "PASS: fist hull contacts, missed/behind targets, height and crouching\n";
    }
    {
        DemoData data;data.players.resize(1);auto& player=data.players[0];
        player.entity=22;player.info={{0,100,1,0,"keys"},{6,0,1,2,""}};
        player.active_weapon={{0,70,70,"keys"},{3,80,80,"rifle"},{4,70,70,"keys"}};
        DemoProp gun;gun.back_gun=true;gun.back_weapon={{0,80}};
        PropKey key;key.time=1;key.parent=22;gun.keys.push_back(key);
        key.time=2;key.present=false;gun.keys.push_back(key);
        key.time=2.5f;key.present=true;gun.keys.push_back(key);
        require(back_gun_player_at(data,gun,0)<0,"Back gun exists before spawn");
        require(back_gun_player_at(data,gun,1.5f)==0,"Holstered gun missing");
        require(back_gun_player_at(data,gun,2.2f)<0,"Dormant back gun remains");
        require(back_gun_player_at(data,gun,3.5f)<0,"Held gun duplicated on back");
        require(back_gun_player_at(data,gun,4.5f)==0,"Gun did not return after holstering");
        require(back_gun_player_at(data,gun,4.5f,0)<0,"Back gun visible in own first-person view");
        require(back_gun_player_at(data,gun,6.5f)<0,"Dead player's back gun remains");
        require(back_gun_player_at(data,gun,1.5f)==0,"Back gun rewind failed");
        const float origin[3]{10,20,30},angles[3]{0,0,0};
        auto matrix=back_gun_transform(gmod::source::model::matrix_from_source_angles(origin,angles));
        near(matrix[3],2);near(matrix[7],24);near(matrix[11],27);
        const float turned[3]{0,90,0};
        matrix=back_gun_transform(gmod::source::model::matrix_from_source_angles(origin,turned));
        near(matrix[3],6);near(matrix[7],12);near(matrix[11],27);
        std::cout << "PASS: recorded back guns, held/dead/first-person suppression, PVS, chest offsets and rewind\n";
    }
    {
        std::vector<FireKey> keys={{1,50,1,{0,0,10},1,true},{2,50,1,{1,2,10},2,true},
            {3,50,1,{},1,false},{4,50,2,{0,0,10},1,true},{5,50,1,{},1,false},{6,50,2,{},1,false}};
        FirePlayback playback;
        require(playback.sample(keys,0).empty(),"Fire exists before spawn");
        const auto first=playback.sample(keys,1.2f).at(50);
        const auto particle=fire_particle(first,2,false,1.2f);
        require(particle.opacity>0 && particle.radius>0,"No flame particles");
        require(fire_particle(first,3,false,1.1f).opacity==0,"Flame born in the future");
        require(fire_particle(first,0,false,4).opacity==0,"Expired particle survived");
        auto hidden=first;hidden.key.scale=0;
        require(fire_particle(hidden,2,false,1.2f).opacity==0,"Zero-sized fire remains visible");
        const auto changed=playback.sample(keys,2.2f).at(50);
        near(changed.born,1);near(changed.key.scale,2);near(changed.key.origin[0],1);
        require(playback.sample(keys,3.5f).empty(),"Extinguished fire remains");
        near(playback.sample(keys,4.2f).at(50).born,4);
        require(playback.sample(keys,5.5f).size()==1,"Stale serial removed a new fire");
        require(playback.sample(keys,6.5f).empty(),"Reused fire survived removal");
        const auto rewound=fire_particle(playback.sample(keys,1.2f).at(50),2,false,1.2f);
        require(particle.position==rewound.position && particle.color==rewound.color
            && particle.radius==rewound.radius && particle.opacity==rewound.opacity,"Fire rewind differs");
        const auto paused=fire_particle(playback.sample(keys,1.2f).at(50),2,false,1.2f);
        require(paused.position==rewound.position,"Paused fire moves");
        std::cout << "PASS: fire lifetime, scale, deletion, reused handles, pause and rewind\n";
    }
    {
        DemoProp vehicle; vehicle.vehicle=true;
        PropKey key; key.has_position=key.has_yaw=true;key.yaw=170;
        vehicle.keys.push_back(key);
        key.time=0.04f;key.x=4;vehicle.keys.push_back(key);
        key.has_position=false;key.yaw=-170;vehicle.keys.push_back(key);
        key.has_position=true;key.x=8;key.yaw=-160;vehicle.keys.push_back(key);
        key.time=0.08f;key.x=16;key.yaw=-150;vehicle.keys.push_back(key);
        vehicle.poses.push_back({0.04f,{}});
        vehicle.poses.push_back({0.04f,{1}});
        coalesce_vehicle_updates(vehicle);
        require(vehicle.keys.size()==3 && vehicle.poses.size()==1 && vehicle.poses[0].value[0]==1,
                "Vehicle updates did not resolve to their final per-tick state.");
        PropPose pose;
        require(sample_prop(vehicle,0.02f,pose),"Vehicle midpoint missing");
        near(pose.x,4);near(pose.yaw,185);
        PropPose before,at;
        require(sample_prop(vehicle,0.04f-0.00001f,before) && sample_prop(vehicle,0.04f,at),"Vehicle boundary missing");
        require(std::fabs(before.x-at.x)<0.01f && std::fabs(std::remainder(before.yaw-at.yaw,360.0f))<0.01f,
                "Vehicle jumped at a duplicate-update boundary.");
        require(sample_prop(vehicle,0.02f,pose),"Vehicle midpoint rewind missing");near(pose.x,4);
        key.time=0.12f;key.present=true;vehicle.keys.push_back(key);
        key.present=false;vehicle.keys.push_back(key);
        coalesce_vehicle_updates(vehicle);
        require(!sample_prop(vehicle,0.12f,pose),"Coalescing revived a removed vehicle.");
        DemoData demo;
        demo.recorder_camera={{0,0,0,0,0,170,false},{0.04f,4,0,0,0,175,false},
                              {0.04f,8,0,0,0,-160,false},{0.08f,16,0,0,0,-150,false}};
        CameraKey camera;
        require(sample_recorder_camera(demo,0.02f,camera),"Recorder camera midpoint missing");near(camera.yaw,185);
        require(sample_recorder_camera(demo,0.04f-0.00001f,camera),"Recorder camera boundary missing");
        require(std::fabs(std::remainder(camera.yaw+160,360.0f))<0.01f,"Recorder camera snapped to its final tick update.");
        std::cout << "PASS: same-tick vehicle/camera continuity, partial updates, wheel poses, deletion and rewind\n";
    }
    {
        gmod::app::VehicleCamera mode;
        bool first=true;
        mode.update(true,0,first);
        require(mode.active && !first && !mode.free_look,"Car entry must use recorded-look third person.");
        mode.toggle(first);
        require(mode.free_look && !first,"R must enable orbit without first person.");
        mode.update(true,0,first);
        require(mode.free_look && !first,"Vehicle update reset orbit mode.");
        mode.toggle(first);
        require(!mode.free_look && !first,"Second R must restore recorded-look third person.");
        mode.update(true,1,first);
        require(!mode.free_look && !first,"Following another occupant must remain third person.");
        mode.update(false,1,first);
        require(!mode.active && first,"Leaving a car must restore the previous on-foot view.");
        mode.toggle(first);
        require(!first,"On-foot R must still toggle first/third person.");
        mode.update(true,1,first); mode.toggle(first); mode.update(false,1,first);
        require(!first,"Car modes must preserve an on-foot third-person preference.");
        DemoData look;
        look.players.resize(2); look.recorder_player=0;
        look.recorder_camera={{1,0,0,0,10,170,false},{1.5f,0,0,0,20,-170,false}};
        look.players[0].look={{1,0,30}}; // Body/network yaw must not override recorder camera.
        look.players[1].look={{1,10,170},{1.5f,20,-170},{5,50,90}};
        float pitch=0,yaw=0;
        require(sample_player_look(look,0,1.25f,pitch,yaw),"Recorder look missing while car origin is invalid.");
        near(pitch,15);near(std::fabs(yaw),180);
        require(sample_player_look(look,1,1.25f,pitch,yaw),"Other occupant eye angles missing.");
        near(pitch,15);near(std::fabs(yaw),180);
        require(sample_player_look(look,1,4,pitch,yaw),"Seated sparse eye angles missing.");
        near(pitch,20);near(yaw,-170); // No interpolation across a teleport/cut.
        require(sample_player_look(look,1,1,pitch,yaw),"Look rewind failed.");near(yaw,170);
        require(!sample_player_look(look,1,0,pitch,yaw),"Future eye angles leaked into earlier playback.");
        DemoData parented;
        parented.recorder_player=0; parented.players.resize(2);
        parented.recorder_camera={{1,0,0,0,0,20,false},{2,0,0,0,0,20,false}};
        for (auto& player : parented.players) {
            PlayerSample seated; seated.time=1; seated.x=100; seated.flags=PlayerFlagSeated|PlayerFlagInCar;
            player.samples.push_back(seated);
            player.vehicle_links={{1,7,7}};
        }
        parented.players[1].look={{1,0,150}};
        DemoProp car; car.entity=7; car.vehicle=true;
        PropKey car_key;car_key.time=1;car_key.has_position=car_key.has_yaw=true;car_key.yaw=30;
        car.keys.push_back(car_key);car_key.time=2;car_key.yaw=90;car.keys.push_back(car_key);
        parented.props.push_back(car);
        require(sample_player_look(parented,0,1,pitch,yaw),"Parent-local recorder look missing"); near(yaw,50);
        require(sample_player_look(parented,0,1.5f,pitch,yaw),"Car-turn look missing"); near(yaw,80);
        require(sample_player_look(parented,0,2,pitch,yaw),"Car-turn endpoint missing"); near(yaw,110);
        require(sample_player_look(parented,0,1,pitch,yaw),"Car-turn rewind missing");near(yaw,50);
        DemoProp reused;reused.entity=7;
        PropKey obsolete=car.keys[0];obsolete.time=0;obsolete.present=false;reused.keys={obsolete};
        parented.props.insert(parented.props.begin(),reused);
        require(sample_player_look(parented,0,1.5f,pitch,yaw),"Reused parent entity missing");near(yaw,80);
        require(sample_player_look(parented,1,1.5f,pitch,yaw),"Other occupant look missing");near(yaw,150);
        // Passenger view inherits both its local seat rotation and the car rotation.
        DemoProp seat;seat.entity=8;seat.seat=true;
        car_key.time=1;car_key.yaw=-90;car_key.parent=7;seat.keys={car_key};
        parented.props.push_back(seat);parented.players[0].vehicle_links={{1,7,8}};
        require(sample_player_look(parented,0,1.5f,pitch,yaw),"Passenger parent chain missing");near(yaw,-10);
        parented.props.back().keys[0].parent=-1; // m_hVehicle supplies a missing seat parent.
        require(sample_player_look(parented,0,1.5f,pitch,yaw),"Passenger vehicle-link fallback missing");near(yaw,-10);
        parented.props.back().keys[0].parent=8; // Malformed cyclic parent chains must not hang or apply partial rotation.
        require(sample_player_look(parented,0,1.5f,pitch,yaw),"Recorder fallback missing");near(yaw,20);
        parented.players[0].samples[0].flags=0;
        require(sample_player_look(parented,0,1.5f,pitch,yaw),"On-foot recorder look missing");near(yaw,20);
        std::cout << "PASS: recorder vehicle-parent rotation, passenger chain, rewind and no double-rotation of other players\n";
        std::cout << "PASS: vehicle R modes, on-foot restoration, recorder/occupant look, angle wrap and rewind\n";
    }
    {
        gmod::audio::StoppedSoundEvents stopped;
        require(stopped.allows(5), "Sound starts must play before stopsound.");
        stopped.stop(100);
        require(!stopped.allows(80) && !stopped.allows(100), "Stopsound must cancel prior and current starts.");
        require(stopped.allows(101), "Stopsound must not mute future starts.");
        stopped.observe(200);
        require(!stopped.allows(80), "A forward seek must not resurrect an old alarm.");
        require(!stopped.allows_world_event(110, 1, false), "A volume update must not restart a stopped loop.");
        require(!stopped.allows_world_event(110, 2, false), "A pitch update must not restart a stopped loop.");
        require(stopped.allows_world_event(110, 1, true), "Newly started loops must still receive updates.");
        require(stopped.allows_world_event(80, 4, false), "Stop events must remain effective.");
        stopped.observe(90);
        require(!stopped.allows(80), "Rewinding after an alarm's start must leave it stopped.");
        stopped.observe(80);
        require(stopped.allows(80), "Seeking back to the alarm's start must replay it.");
        stopped.observe(95);
        require(stopped.allows_world_event(80, 0, false), "Replayed starts must remain eligible during restoration.");
        stopped.stop(95);
        require(!stopped.allows(80), "A second stopsound must cancel replayed starts.");
        stopped.observe(0);
        require(stopped.allows(0), "Rewinding to the recording start must permit initial events.");
        std::cout << "PASS: stopsound boundaries, future starts, parameter updates, forward seeks and rewind retriggers\n";
    }
    {
        gmod::ui::ScopeProjection lens;
        lens.triangles.push_back({gmod::ui::ScopeVertex{{-1,-1,0.5f,1},{0,1}},
            gmod::ui::ScopeVertex{{2,-2,1,2},{1,1}}, gmod::ui::ScopeVertex{{0,3,1.5f,3},{0.5f,0}}});
        for (const auto size : {std::array<int,2>{1280,720}, {1920,1080}, {900,900}, {640,360}}) {
            for (const auto uv : {std::array<float,2>{0.5f,0.5f}, {0.25f,0.8f}, {0.7f,0.9f}}) {
                const auto screen = lens.screen(uv[0],uv[1],size[0],size[1]);
                require(screen.has_value(),"Scope UV did not project through its actual lens mesh.");
                const auto restored = lens.texture((*screen)[0],(*screen)[1],size[0],size[1]);
                require(restored.has_value(),"Scope picking missed the projected lens.");
                near((*restored)[0],uv[0]);
                near((*restored)[1],uv[1]);
            }
            require(!lens.texture(-10,-10,size[0],size[1]),"Scope picking included pixels outside the lens.");
        }
        require(!lens.screen(-1,2,1280,720),"Scope labels included UVs outside the lens mesh.");
        require(!gmod::ui::ScopeProjection{}.screen(0.5f,0.5f,1280,720),
            "Inactive scope retained its lens projection.");
    }
    near(player_eye_height(0), 64.0f);
    near(player_eye_height(1), 64.0f);
    near(player_eye_height(263), 40.0f);
    near(player_eye_height(PlayerFlagSeated), 40.0f);
    near(player_eye_height(PlayerFlagSeated | 2), 40.0f);
    if (argc == 4 && std::string_view(argv[1]) == "--back-weapons") {
        const std::atomic<bool> cancel{false};
        const auto demo=load_demo(argv[2],argv[3],[](float,const std::string&){},cancel);
        const auto player=std::find_if(demo.players.begin(),demo.players.end(),[](const auto& p){return p.name=="sl_cx";});
        require(player!=demo.players.end(),"sl_cx fixture missing");
        const int index=static_cast<int>(player-demo.players.begin());
        const auto gun=std::find_if(demo.props.begin(),demo.props.end(),[&](const auto& p){return p.back_gun && back_gun_player_at(demo,p,2137)==index;});
        require(gun!=demo.props.end(),"Recorded sl_cx back gun missing before death");
        require(demo.models.at(gun->model)=="models/perpheads/weapons/w_870.mdl","Wrong back gun model");
        require(timed_int_at(gun->back_weapon,2137,-1)==1687,"Back gun's networked weapon link missing");
        require(back_gun_player_at(demo,*gun,2139.5f)<0,"Drawn shotgun remains on back");
        require(back_gun_player_at(demo,*gun,2140)<0,"Back gun remains after death");
        require(back_gun_player_at(demo,*gun,2137)==index,"Recorded back gun rewind failed");
        std::cout << "PASS: real sl_cx back-mounted Remington, draw, death and rewind entity=" << gun->entity << "\n";
        return 0;
    } else if (argc == 4 && std::string_view(argv[1]) == "--vehicle-look") {
        const std::atomic<bool> cancel{false};
        const auto demo=load_demo(argv[2],argv[3],[](float,const std::string&){},cancel);
        int recorder_samples=0,other_samples=0,different_from_body=0,recorder_rotated=0,flat_car_checks=0;
        for (std::size_t i=0;i<demo.players.size();++i) {
            const auto& player=demo.players[i];
            for (const auto& key : player.samples) {
                if ((key.flags & PlayerFlagInCar)==0) continue;
                // Exit/seat-rebind records can share a timestamp. Exercise the
                // effective playback pose, not a superseded sample at that tick.
                PlayerPose current;
                if (!sample_player(player,key.time,current) || !(current.flags & PlayerFlagInCar)) continue;
                float pitch=0,yaw=0;
                if (!sample_player_look(demo,static_cast<int>(i),key.time,pitch,yaw)) continue;
                if (static_cast<int>(i)==demo.recorder_player) {
                    CameraKey camera;
                    require(sample_recorder_camera(demo,key.time,camera),"Recorder camera missing");
                    if (std::fabs(std::remainder(yaw-camera.yaw,360.0f))>5) ++recorder_rotated;
                    const auto link=std::upper_bound(player.vehicle_links.begin(),player.vehicle_links.end(),key.time,
                        [](float time,const PlayerVehicleKey& value){return time<value.time;});
                    if (link!=player.vehicle_links.begin() && !camera.origin_valid) {
                        const int parent=std::prev(link)->parent;
                        const auto car=std::find_if(demo.props.begin(),demo.props.end(),[&](const auto& value){return value.vehicle && value.entity==parent;});
                        PropPose pose;
                        if (car!=demo.props.end() && sample_prop(*car,key.time,pose) && std::fabs(pose.pitch)<0.05f && std::fabs(pose.roll)<0.05f) {
                            if (std::fabs(std::remainder(yaw-camera.yaw-pose.yaw,360.0f))>=0.2f) {
                                const auto frame=std::upper_bound(car->keys.begin(),car->keys.end(),key.time,
                                    [](float t,const PropKey& value){return t<value.time;});
                                std::cerr << "RECORDER_PARENT_CHECK time=" << key.time << " local=" << camera.pitch << ',' << camera.yaw
                                    << " car=" << pose.pitch << ',' << pose.yaw << ',' << pose.roll << " world=" << pitch << ',' << yaw
                                    << " flags=" << current.flags << " link=" << std::prev(link)->vehicle << ',' << parent
                                    << " car_parent=" << (frame==car->keys.begin()?-1:std::prev(frame)->parent) << '\n';
                            }
                            require(std::fabs(std::remainder(yaw-camera.yaw-pose.yaw,360.0f))<0.2f,
                                "Recorder camera failed to inherit the car's turn");
                            ++flat_car_checks;
                        }
                    }
                    ++recorder_samples;
                } else {
                    ++other_samples;
                    if (std::fabs(std::remainder(yaw-key.yaw,360.0f))>5) ++different_from_body;
                }
            }
        }
        require(recorder_samples>0 && other_samples>0,"Captured vehicle look coverage missing");
        require(different_from_body>0,"Only synthesized body facing was available");
        require(recorder_rotated>0 && flat_car_checks>0,"Recorder vehicle-parent rotation was not exercised");
        std::cout << "PASS: captured vehicle look recorder=" << recorder_samples << " other=" << other_samples
                  << " differing_from_body=" << different_from_body << " recorder_rotated=" << recorder_rotated
                  << " flat_car_checks=" << flat_car_checks << '\n';
        int vehicle_steps=0,camera_steps=0,recorder_vehicle_steps=0;
        float worst_vehicle=0,worst_camera=0;
        for (const auto& prop : demo.props) {
            if (!prop.vehicle) continue;
            float last=-1;
            for (const auto& key : prop.keys) {
                if (key.time==last) continue;
                last=key.time;
                PropPose a,b,c;
                const float ta=key.time-0.002f,tb=key.time-0.001f;
                if (!sample_prop(prop,ta,a) || !sample_prop(prop,tb,b) || !sample_prop(prop,key.time,c)) continue;
                const float ratio=(key.time-tb)/(tb-ta); // Float timestamp spacing is uneven late in long demos.
                const float error=std::hypot((c.x-b.x)-(b.x-a.x)*ratio,(c.y-b.y)-(b.y-a.y)*ratio,(c.z-b.z)-(b.z-a.z)*ratio);
                if (error>0.1f && error<128) {
                    ++vehicle_steps;worst_vehicle=std::max(worst_vehicle,error);
                    if (demo.recorder_player>=0) {
                        const auto& player=demo.players[demo.recorder_player];
                        const auto link=std::upper_bound(player.vehicle_links.begin(),player.vehicle_links.end(),key.time,
                            [](float time,const PlayerVehicleKey& value){return time<value.time;});
                        if (link!=player.vehicle_links.begin() && std::prev(link)->vehicle==prop.entity) ++recorder_vehicle_steps;
                    }
                }
            }
        }
        float last=-1;
        for (const auto& key : demo.recorder_camera) {
            if (key.time==last) continue;
            last=key.time;
            CameraKey a,b,c;
            const float ta=key.time-0.002f,tb=key.time-0.001f;
            if (!sample_recorder_camera(demo,ta,a) || !sample_recorder_camera(demo,tb,b) || !sample_recorder_camera(demo,key.time,c)) continue;
            const float ratio=(key.time-tb)/(tb-ta);
            const float error=std::fabs(std::remainder(std::remainder(c.yaw-b.yaw,360.0f)-std::remainder(b.yaw-a.yaw,360.0f)*ratio,360.0f));
            if (error>0.1f) {++camera_steps;worst_camera=std::max(worst_camera,error);}
        }
        std::cout << "CONTINUITY vehicle_steps=" << vehicle_steps << " worst_units=" << worst_vehicle
                  << " recorder_vehicle_steps=" << recorder_vehicle_steps << " camera_steps=" << camera_steps << " worst_degrees=" << worst_camera << '\n';
        return 0;
    }
    if (argc == 5 && std::string_view(argv[1]) == "--model-identifiers") {
        const std::atomic<bool> cancel{false};
        const auto demo = load_demo(argv[2], argv[3], [](float,const std::string&) {}, cancel, argv[4]);
        const auto registry = load_perpheads_registry(demo.perp_data);
        require(registry.vehicle_plates.size() > 100 && registry.plate_skins.size() > 10,
            "Exact server plate registry missing");
        int vehicles = 0, badges = 0, two_plates = 0;
        for (const auto& prop : demo.props) {
            if (!prop.vehicle) continue;
            const auto states = vehicle_plate_states(prop);
            if (states.empty()) continue;
            ++vehicles;
            for (const auto& state : states) {
                require(registry.vehicle_plates.contains(state.id), "Recorded vehicle layout missing");
                const auto appearance = plate_appearance(registry, state);
                require(!appearance.materials.empty(), "Recorded plate has no materials");
                if (registry.vehicle_plates.at(state.id).placements.size() == 2) ++two_plates;
            }
            if (prop.entity == 3273)
                for (float time : {300.0f, 600.0f, 300.0f}) {
                    PropPose pose;
                    if (sample_prop(prop, time, pose))
                        std::cout << "PLATE_FIXTURE time=" << time << " position=" << pose.x << ","
                            << pose.y << "," << pose.z << " yaw=" << pose.yaw
                            << " registration=" << vehicle_plate_at(prop, time).registration << "\n";
                }
        }
        for (const auto& [id, record] : demo.clothing_records) {
            for (const auto& part : compose_outfit(registry, record))
                if (part.data_number > 0) {
                    ++badges;
                    require(part.sub_materials.size() >= 4 && !part.number_bodygroup.empty(),
                        "Recorded badge materials missing");
                }
        }
        require(vehicles > 30 && two_plates > 0 && badges > 10, "Real identifier fixture coverage missing");
        for (const auto& player : demo.players) {
            PlayerPose pose;
            if (sample_player(player, 300, pose)
                && timed_int_at(player.job_team, 300, registry.team_citizen) == registry.team_police)
                std::cout << "BADGE_FIXTURE player=" << player.name << " entity=" << player.entity
                    << " position=" << pose.x << "," << pose.y << "," << pose.z << " yaw=" << pose.yaw << "\n";
        }
        std::cout << "PASS: real recorded plate parsing, exact layouts, skins and uniform digits vehicles="
            << vehicles << " front/rear=" << two_plates << " badges=" << badges << "\n";
        return 0;
    }
    if (argc == 5 && std::string_view(argv[1]) == "--modern-clothing") {
        const std::atomic<bool> cancel{false};
        const auto demo = load_demo(argv[2], argv[3], [](float,const std::string&) {}, cancel, argv[4]);
        require(!demo.perp_data.empty(), "Exact modern registry was not exported.");
        const auto registry = load_perpheads_registry(demo.perp_data);
        require(!registry.required_slots.empty(), "Modern body slots are missing.");
        require(!registry.weapon_attachments.empty(), "Modern weapon attachment registry is empty.");
        std::size_t attachment_references = 0;
        for (const auto& [handle, weapon] : demo.weapons) {
            for (const auto& key : weapon.attachments) {
                std::size_t offset = 0;
                while (offset < key.text.size()) {
                    const auto end = key.text.find(',', offset);
                    const int id = std::stoi(key.text.substr(offset, end - offset));
                    require(registry.weapon_attachments.contains(id),
                        "Modern recorded attachment ID has no exact registry definition.");
                    ++attachment_references;
                    if (end == std::string::npos) break;
                    offset = end + 1;
                }
            }
        }
        require(attachment_references > 100, "Modern attachment fixture data is missing.");
        for (const auto [entity, time] : {std::pair{120,4.0f}, std::pair{78,730.0f}}) {
            const int index = player_index_at(demo, entity, time);
            require(index >= 0, "Modern scoped player is missing.");
            const auto held = held_weapon_at(demo, demo.players[static_cast<std::size_t>(index)], time);
            bool known = false;
            const auto ids = weapon_attachments_at(demo, held.handle, time, known);
            require(known, "Modern scoped weapon attachments are unknown.");
            const auto optic = std::find_if(ids.begin(), ids.end(), [&](int id) {
                const auto& definition = registry.weapon_attachments.at(id);
                return definition.magnification > 1.0f;
            });
            require(optic != ids.end(), "Modern scoped weapon has no magnifying optic.");
            const auto flags = weapon_flags_at(demo, held.handle, time);
            require(flags.aiming && !flags.passive && !flags.reloading,
                "Modern scoped fixture lost its active aiming state.");
            std::cout << "MODERN_SCOPE entity=" << entity << " time=" << time
                << " optic=" << registry.weapon_attachments.at(*optic).name << '\n';
        }
        for (const int entity : {8, 78, 120}) {
            for (const float time : {4.0f, 0.0f, 30.0f, 4.0f}) {
                const int index = player_index_at(demo, entity, time);
                require(index >= 0, "Warehouse clothing fixture player missing.");
                const auto& keys = demo.players[static_cast<std::size_t>(index)].outfit;
                require(!keys.empty(), "Modern player has no outfit timeline.");
                auto key = std::upper_bound(keys.begin(), keys.end(), time,
                    [](float t, const TimedInt& value) { return t < value.time; });
                if (key != keys.begin()) --key;
                const auto found = demo.clothing_records.find(key->value);
                require(found != demo.clothing_records.end(), "Modern outfit record is unresolved.");
                require(!found->second.explicit_parts, "Modern demo was decoded as legacy clothing.");
                const auto parts = compose_outfit(registry, found->second);
                require(parts.size() >= 4, "Modern outfit is missing clothing or bare body parts.");
                std::set<int> covered;
                for (const auto& part : parts) {
                    const auto type = registry.clothing.find(part.type);
                    require(type != registry.clothing.end() && !part.model.empty(),
                        "Modern outfit model or clothing type is missing.");
                    covered.insert(type->second.slots.begin(), type->second.slots.end());
                }
                for (const int slot : registry.required_slots)
                    require(covered.contains(slot), "Modern outfit leaves a required body slot empty.");
            }
        }
        std::cout << "PASS: warehouse modern clothing covers every body slot at start, 4s, 30s and rewind; "
            << attachment_references << " attachment references resolve, ACOG and PM2 aim states preserved\n";
    } else if (argc == 5 && std::string_view(argv[1]) == "--legacy-clothing") {
        const std::atomic<bool> cancel{false};
        const auto demo = load_demo(argv[2], argv[3], [](float,const std::string&) {}, cancel, argv[4]);
        require(!demo.perp_data.empty(), "Exact historical registry was not exported.");
        const auto registry = load_perpheads_registry(demo.perp_data);
        require(registry.weapon_attachments.size() == 99, "Historical attachment registry is incomplete.");
        std::size_t records = 0, outfits = 0, attachments = 0;
        for (const auto& [id, record] : demo.clothing_records) {
            if (!record.explicit_parts) continue;
            ++records;
            const auto parts = compose_outfit(registry, record);
            require(parts.size() == record.items.size(), "Recorded legacy body parts were dropped or invented.");
            for (std::size_t i = 0; i < parts.size(); ++i) {
                near(static_cast<float>(parts[i].skin), static_cast<float>(record.items[i].model_skin));
                require(record.items[i].model_path == parts[i].model + ".mdl",
                    "Legacy outfit did not use its recorded model.");
            }
        }
        for (const auto& player : demo.players) outfits += player.outfit.size();
        for (const auto& [entity, weapon] : demo.weapons) {
            for (const auto& key : weapon.attachments) {
                std::size_t offset = 0;
                while (offset < key.text.size()) {
                    const auto end = key.text.find(',', offset);
                    const int id = std::stoi(key.text.substr(offset, end - offset));
                    require(registry.weapon_attachments.contains(id), "Recorded attachment ID is unresolved.");
                    ++attachments;
                    if (end == std::string::npos) break;
                    offset = end + 1;
                }
            }
        }
        require(records > 100 && outfits > 100 && attachments > 10, "Legacy appearance fixtures are missing.");
        std::cout << "PASS: legacy outfits=" << outfits << " records=" << records
            << " attachment IDs=" << attachments << " exact historical registry"
            << " tick_rate=" << demo.tick_rate << "\n";
    } else if (argc == 4 && std::string_view(argv[1]) == "--command-car-passengers") {
        const std::atomic<bool> cancel{false};
        auto demo = load_demo(argv[2], argv[3], [](float,const std::string&) {}, cancel);
        const auto seat = std::find_if(demo.props.begin(), demo.props.end(),
            [](const auto& prop) { return prop.entity == 5580 && prop.seat; });
        require(seat != demo.props.end() && !seat->vehicle && seat->model == -1,
            "Baseline-restored passenger model was classified as a standalone car.");
        const auto car = std::find_if(demo.props.begin(), demo.props.end(),
            [](const auto& prop) { return prop.entity == 4194 && prop.vehicle; });
        require(car != demo.props.end() && car->skin == 11, "Command-car livery was lost.");
        const auto check = [&] {
            for (int entity : {7, 61, 78, 81}) {
                for (float time : {529.0f, 500.0f, 529.0f}) {
                    const int index = player_index_at(demo, entity, time);
                    require(index >= 0, "Car occupant fixture player missing.");
                    PlayerPose player;
                    PropPose vehicle;
                    require(sample_player(demo.players[static_cast<std::size_t>(index)], time, player)
                        && sample_prop(*car, time, vehicle), "Occupied car pose missing.");
                    require((player.flags & PlayerFlagSeated) != 0, "Occupant lost seated state.");
                    const float dx = player.x - vehicle.x, dy = player.y - vehicle.y;
                    require(dx*dx + dy*dy < 100*100 && std::fabs(player.z - vehicle.z) < 100,
                        "Occupant used local seat coordinates instead of following the moving car.");
                }
            }
        };
        check();
        rebind_vehicle_seats(demo);
        check();
        std::cout << "PASS: baseline-restored passenger seats follow command car, preserve livery, seek and rebind\n";
    } else if (argc == 4 && std::string_view(argv[1]) == "--crouch-eyes") {
        const std::atomic<bool> cancel{false};
        const auto demo = load_demo(argv[2], argv[3], [](float,const std::string&) {}, cancel);
        const int index = player_index_at(demo, 88, 266.0f);
        require(index >= 0, "SkinnyPx fixture player missing.");
        const auto& player = demo.players[static_cast<std::size_t>(index)];
        require(player.name == "SkinnyPxABQ", "Wrong crouch fixture identity.");
        PlayerPose pose;
        require(sample_player(player, 266.0f, pose) && (pose.flags & 2) != 0,
            "SkinnyPx is not crouched at 4:26.");
        near(pose.z + player_eye_height(pose.flags), pose.z + 40.0f);
        const auto standing = std::find_if(player.samples.begin(), player.samples.end(),
            [](const auto& sample) { return (sample.flags & (2 | PlayerFlagSeated)) == 0; });
        require(standing != player.samples.end(), "Standing fixture sample missing.");
        near(player_eye_height(standing->flags), 64.0f);
        require(sample_player(player, 266.0f, pose) && (pose.flags & 2) != 0,
            "Rewinding lost the recorded crouch.");
        near(player_eye_height(pose.flags), 40.0f);
        std::cout << "PASS: SkinnyPx at 4:26 uses PERP's 40-unit crouch eyes; standing, seats and rewind preserved\n";
    } else if (argc == 4 && std::string_view(argv[1]) == "--session-identity") {
        const std::atomic<bool> cancel{false};
        const auto demo = load_demo(argv[2], argv[3], [](float,const std::string&) {}, cancel);
        const auto at = [&](int tick) -> const DemoPlayer& {
            const int index = player_index_at(demo, 106, static_cast<float>(tick) / demo.tick_rate);
            require(index >= 0, "Expected connected player in reused slot.");
            return demo.players[static_cast<std::size_t>(index)];
        };
        const auto& billy = at(100);
        require(billy.name == "Billy Blowher" && billy.steam_id == "STEAM_0:0:900924143",
            "Initial slot identity changed.");
        near(billy.disconnected_at, 3052.0f / demo.tick_rate);
        require(std::all_of(billy.samples.begin(), billy.samples.end(), [&](const auto& sample) {
            return sample.time <= billy.disconnected_at;
        }), "Later occupant poses leaked into Billy's track.");
        require(at(10000).name == "hipopatumus", "Intermediate connection was lost.");
        require(player_index_at(demo,106,25000.0f / demo.tick_rate) < 0,
            "Vacant slot was assigned to a stale identity.");
        const auto& dekkrior = at(40000);
        require(dekkrior.name == "Dekkrior" && dekkrior.user_id == 521
            && dekkrior.steam_id == "STEAM_0:1:51937904"
            && dekkrior.steam_id64 == "76561198064141537",
            "Reconnected Dekkrior has the wrong Steam identity.");
        require(!dekkrior.first_name.empty()
            && std::all_of(dekkrior.first_name.begin(), dekkrior.first_name.end(), [&](const auto& key) {
                return key.time >= dekkrior.connected_at;
            }) && std::all_of(billy.first_name.begin(), billy.first_name.end(), [&](const auto& key) {
                return key.time < billy.disconnected_at;
            }),
            "Reconnected character metadata did not reach the new session.");
        PlayerPose pose;
        require(!sample_player(billy,40000.0f / demo.tick_rate,pose),
            "Disconnected Billy is still renderable.");
        require(sample_player(dekkrior,38540.0f / demo.tick_rate,pose),
            "Reconnected Dekkrior has no renderable pose.");
        std::cout << "PASS: real Billy / hipopatumus / Dekkrior session identities, metadata and poses\n";
    } else if (argc == 5 && std::string_view(argv[1]) == "--recovered-pair") {
        const std::atomic<bool> cancel{false};
        for (int file = 2; file <= 3; ++file) {
            const auto demo = load_demo(argv[file], argv[4], [](float,const std::string&) {}, cancel);
            near(demo.tick_rate, 25.0f);
            near(demo.duration, 62143.0f / 25.0f);
            for (const int entity : {2884,2886,2889}) {
                const auto door = std::find_if(demo.doors.begin(),demo.doors.end(),
                    [&](const auto& value) { return value.entity == entity; });
                require(door != demo.doors.end(),"Build demo hallway door missing.");
                std::array<float,3> angles{};
                require(door_angles_at(*door,1892.0f,angles),"Hallway door angles missing.");
                near(angles[1],180.0f);
            }
            for (const int entity : {1325,2841,3370,4428,4502,4544,5146,5399,5556,5684}) {
                require(std::any_of(demo.props.begin(), demo.props.end(), [&](const auto& prop) {
                    PropPose pose;
                    return prop.entity == entity && sample_prop(prop,1271.0f,pose);
                }), "Build demo base prop was absent at 21:11.");
            }
            for (const int entity : {32,63}) {
                const auto player = std::find_if(demo.players.begin(), demo.players.end(),
                    [&](const auto& value) { return value.entity == entity; });
                require(player != demo.players.end(), "Build demo fixture player missing.");
                const auto held = held_weapon_at(demo,*player,1271.0f);
                const auto expected = entity == 32
                    ? "weapon_perp_phys_l115a3" : "weapon_perp_phys_ots33";
                require(held.class_name == expected, "Build demo held gun was not resolved at 21:11.");
                if (entity == 32) {
                    bool known = false;
                    const auto attachments = weapon_attachments_at(demo,held.handle,1271.0f,known);
                    require(known && std::find(attachments.begin(),attachments.end(),56) != attachments.end(),
                        "Recorded L115A3 ACOG missing from the exact weapon handle.");
                    const auto ads = weapon_flags_at(demo,held.handle,1273.04f);
                    require(ads.aiming && !ads.passive && !ads.reloading,
                        "L115A3 active scope fixture lost recorded aim state.");
                    require(!weapon_flags_at(demo,held.handle,1271).aiming,
                        "Rewind retained future scope state.");
                }
                require(!player->samples.empty() && player->samples.back().time > 2400.0f,
                    "Build demo player state stopped before the end of the recording.");
                if (entity == 63) {
                    PlayerPose pose;
                    require(sample_player(*player,888.48f,pose) && (pose.flags & 2) != 0,
                        "Janis's explicitly recorded crouch at 14:48.48 was lost.");
                    require(sample_player(*player,900.0f,pose) && (pose.flags & 2) == 0,
                        "Janis retained crouch after his standing-baseline snapshot.");
                    require(sample_player(*player,1271.0f,pose) && (pose.flags & 2) == 0,
                        "Janis was stuck crouched at 21:11.");
                    require(sample_player(*player,1304.04f,pose) && (pose.flags & 2) != 0,
                        "Janis's separate recorded crouch at 21:44 was lost.");
                    require(sample_player(*player,1305.0f,pose) && (pose.flags & 2) == 0,
                        "Janis remained crouched after his recorded stand-up at 21:44.2.");
                    require(sample_player(*player,1271.0f,pose) && (pose.flags & 2) == 0,
                        "Rewinding restored stale crouch instead of Janis's standing state.");
                }
            }
            std::cout << "PASS: " << argv[file] << " duration=" << demo.duration
                << ", ten base props and both held guns at 21:11\n";
        }
    } else if (argc == 4 && std::string_view(argv[1]) == "--smoke") {
        const std::atomic<bool> cancel{false};
        const auto demo = load_demo(argv[2],argv[3],[](float,const std::string&) {},cancel);
        int impacts = 0;
        for (const auto& effect : demo.effects) {
            if (!surface_smoke_effect(effect)) continue;
            const auto puff = smoke_particle(effect,0,effect.time+0.1f,false);
            if (puff.opacity <= 0) continue;
            if (impacts < 8) std::cout << "SMOKE_FIXTURE time=" << effect.time
                << " material=" << effect.material << " scale=" << effect.scale
                << " origin=" << effect.origin[0] << "," << effect.origin[1] << "," << effect.origin[2]
                << " normal=" << effect.normal[0] << "," << effect.normal[1] << "," << effect.normal[2] << "\n";
            ++impacts;
        }
        require(impacts > 0,"Real recording exported no renderable surface smoke impacts.");
        std::cout << "PASS: real surface smoke impacts=" << impacts << "\n";
        if (demo.recorder_player >= 0) {
            const auto& player = demo.players[static_cast<std::size_t>(demo.recorder_player)];
            for (std::size_t i=0;i<std::min<std::size_t>(player.shots.size(),8);++i) {
                const float time = player.shots[i];
                const auto held = held_weapon_at(demo,player,time);
                std::cout << "RECORDER_SMOKE_FIXTURE player=" << demo.recorder_player << " time=" << time
                    << " weapon=" << held.class_name << "\n";
            }
        }
    } else if (argc == 5 && std::string_view(argv[1]) == "--suppressed-shots") {
        const std::atomic<bool> cancel{false};
        const auto demo = load_demo(argv[2],argv[3],[](float,const std::string&) {},cancel);
        const auto registry = load_perpheads_registry(argv[4]);
        int count = 0, recorder_count = 0;
        for (std::size_t index = 0; index < demo.players.size(); ++index) {
            const auto& player = demo.players[index];
            for (const float time : player.shots) {
                const auto held = held_weapon_at(demo,player,time);
                const auto name = weapon_sound_override_at(registry,demo,held.handle,time);
                if (name.empty()) continue;
                const auto sound = std::find_if(registry.gun_sounds.begin(),registry.gun_sounds.end(),
                    [&](const auto& entry) { return entry.second.name == name; });
                require(sound != registry.gun_sounds.end() && !sound->second.files.empty(),
                    "Historical suppressor has no registered firing sample.");
                if (count < 8 || static_cast<int>(index) == demo.recorder_player && recorder_count < 5)
                    std::cout << "SUPPRESSED_SHOT player=" << index << " time=" << time
                        << " recorder=" << (static_cast<int>(index) == demo.recorder_player)
                        << " sound=" << name << " file=" << sound->second.files.front()
                        << " level=" << sound->second.level << " volume=" << sound->second.volume << '\n';
                ++count;
                if (static_cast<int>(index) == demo.recorder_player) ++recorder_count;
            }
        }
        require(count > 0,"Fixture contains no recorded suppressed shots.");
        std::cout << "PASS: exact historical suppressor sound for " << count
            << " recorded shots, including " << recorder_count << " recorder shots\n";
    } else if (argc == 3 && std::string_view(argv[1]) == "--registry") {
        const auto registry = load_perpheads_registry(argv[2]);
        const auto acog = std::find_if(registry.weapon_attachments.begin(),registry.weapon_attachments.end(),
            [](const auto& entry) { return entry.second.name == "acog"; });
        require(acog != registry.weapon_attachments.end(),"Historical ACOG definition missing.");
        near(acog->second.magnification,3);
        near(90.0f/(3*acog->second.magnification),10);
        require(acog->second.iron_pos.has_value(),"ACOG sight offset missing.");
        near((*acog->second.iron_pos)[1],2.45f);
        require(registry.weapon_attachments.size() == 99,"Historical attachment registry is incomplete.");
        std::cout << "PASS: historical Lua registry, real ACOG definition, aim offset and 10-degree lens\n";
    } else if (argc == 4 && std::string_view(argv[1]) == "--cabin-door") {
        const std::atomic<bool> cancel{false};
        const auto demo = load_demo(argv[2],argv[3],[](float,const std::string&) {},cancel);
        const auto door = std::find_if(demo.doors.begin(),demo.doors.end(),
            [](const auto& value) { return value.entity == 280; });
        require(door != demo.doors.end(),"Cabin door fixture missing.");
        std::array<float,3> angles{};
        require(door_angles_at(*door,297,angles),"Cabin door rotation unavailable.");
        near(angles[1],0);
        near(door_openness(*door,297,0.9f),0);
        near(door_openness(*door,340,0.9f),1);
        near(door_openness(*door,297,0.9f),0);
        std::cout << "PASS: cabin door recorded closed at 4:57, later opening and rewind\n";
    } else if (argc == 4 && std::string_view(argv[1]) == "--revive-grenade") {
        const std::atomic<bool> cancel{false};
        const auto demo = load_demo(argv[2],argv[3],[](float,const std::string&) {},cancel);
        const auto player = std::find_if(demo.players.begin(),demo.players.end(),
            [](const auto& value) { return value.entity == 2; });
        require(player != demo.players.end(),"Fredy fixture player missing.");
        require(player_incapacitated(*player,1015),"Recorded death was lost.");
        require(!player_incapacitated(*player,1026),"Revived player retained dead baseline state.");
        require(player_incapacitated(*player,1015),"Rewind did not restore recorded death.");
        require(!player_incapacitated(*player,1026),"Seeking forward lost recorded revival.");
        const auto held = held_weapon_at(demo,*player,987.5f);
        require(held.class_name == "weapon_perp_phys_m67","Recorded grenade identity missing.");
        const auto check = [&](float time, int value, float start) {
            const auto* state = grenade_state_at(demo,held.handle,time);
            require(state && state->value == value,"Incorrect recorded grenade state.");
            near(state->time,start);
        };
        check(984.1f,2,984.04f);
        check(985.7f,3,985.56f);
        check(987.5f,4,987.44f);
        check(984.1f,2,984.04f);
        PlayerPose pose;
        require(sample_player(*player,1026,pose),"Revived player has no rendered pose.");
        std::cout << "REVIVED_POSITION " << pose.x << ',' << pose.y << ',' << pose.z << '\n';
        std::cout << "PASS: Fredy death/revival/rewind and exact grenade pin/spoon/throw timeline; player_index="
            << std::distance(demo.players.begin(),player) << '\n';
    } else if (argc == 4 && std::string_view(argv[1]) == "--randy-gun") {
        const std::atomic<bool> cancel{false};
        const auto demo = load_demo(argv[2],argv[3],[](float,const std::string&) {},cancel);
        const auto player = std::find_if(demo.players.begin(),demo.players.end(),
            [](const auto& value) { return value.entity == 47; });
        require(player != demo.players.end(),"Randy fixture player missing.");
        PlayerPose pose;
        require(sample_player(*player,556,pose),"Armed player has no rendered pose.");
        std::cout << "RANDY_POSITION " << pose.x << ',' << pose.y << ',' << pose.z << '\n';
        for (const float time : {556.0f,555.5f,556.0f}) {
            const auto held = held_weapon_at(demo,*player,time);
            require(held.handle == (5602 | (959 << 13)),"Wrong weapon entity serial restored.");
            require(held.class_name == "weapon_perp_phys_glock17","Firing Glock identity was lost.");
            const auto inventory = inventory_at(demo,*player,time);
            require(std::find(inventory.begin(),inventory.end(),held.class_name) != inventory.end(),
                "Held Glock missing from recorded inventory.");
            const auto view = demo.weapon_view_models.find(held.class_name);
            require(view != demo.weapon_view_models.end() && view->second.find("glock") != std::string::npos,
                "Held Glock has no recorded view model.");
        }
        std::cout << "PASS: Randy exact serial959 Glock, held model, inventory and rewind; player_index="
            << std::distance(demo.players.begin(),player) << '\n';
    } else if (argc == 4 && std::string_view(argv[1]) == "--ots-reload") {
        const std::atomic<bool> cancel{false};
        const auto demo = load_demo(argv[2],argv[3],[](float,const std::string&) {},cancel);
        const auto player = std::find_if(demo.players.begin(),demo.players.end(),
            [](const auto& value) { return value.entity == 63; });
        require(player != demo.players.end(),"OTS reload fixture player missing.");
        const auto held = held_weapon_at(demo,*player,1250);
        require(held.class_name == "weapon_perp_phys_ots33","OTS was not held at reload.");
        require(!weapon_flags_at(demo,held.handle,1250).reloading,"Fixture unexpectedly has a Lua reload flag.");
        const auto* reload = player_reload_at(*player,1250);
        require(reload != nullptr,"Recorded reload animation event was lost.");
        near(reload->time,1249.4f);
        require(player_reload_at(*player,1252) == reload,"Reload timing changed while seeking.");
        player_reload_at(*player,1249);
        require(player_reload_at(*player,1250) == reload,"Rewind lost recorded reload start.");
        std::cout << "PASS: Hix OTS reload uses recorded event at 20:49.4 despite absent Lua reload flag; player_index="
            << std::distance(demo.players.begin(),player) << '\n';
    } else if (argc == 4 && std::string_view(argv[1]) == "--plank") {
        const std::atomic<bool> cancel{false};
        const auto demo = load_demo(argv[2],argv[3],[](float,const std::string&) {},cancel);
        const auto plank = std::find_if(demo.props.begin(),demo.props.end(),
            [&](const auto& prop) {
                return prop.entity == 1108 && prop.model >= 0
                    && demo.models.at(prop.model) == "models/props_debris/wood_board06a.mdl";
            });
        require(plank != demo.props.end(),"Recorded dropped plank model was not imported.");
        PropPose pose;
        require(!sample_prop(*plank,1413.0f,pose),"Dropped plank appeared before its spawn.");
        require(sample_prop(*plank,1417.0f,pose),"Dropped plank missing at the reported time.");
        require(!sample_prop(*plank,1413.0f,pose),"Rewind retained the future dropped plank.");
        require(sample_prop(*plank,1417.0f,pose),"Seeking forward lost the dropped plank.");
        std::cout << "PASS: exact recorded dropped plank, spawn visibility and rewind; position="
            << pose.x << ',' << pose.y << ',' << pose.z << '\n';
    } else if (argc == 4 && std::string_view(argv[1]) == "--gungame") {
        const std::atomic<bool> cancel{false};
        const auto demo = load_demo(argv[2],argv[3],[](float,const std::string&) {},cancel);
        const auto found = std::find_if(demo.players.begin(),demo.players.end(),
            [](const auto& p) { return p.entity == 65; });
        require(found != demo.players.end(),"Gun Game fixture player missing.");
        const auto inventory = [&](float time, const char* item) {
            const auto items = inventory_at(demo,*found,time);
            return std::find(items.begin(),items.end(),item) != items.end();
        };
        require(inventory(90,"weapon_perp_phys_p99"),"Pre-event P99 missing.");
        require(!inventory(103,"weapon_perp_phys_p99"),"Pre-event P99 survived the event strip.");
        require(inventory(103,"weapon_perp_phys_ots33"),"Gun Game starting weapon missing.");
        require(!inventory(104.1f,"weapon_perp_phys_ots33"),"Old Gun Game weapon survived replacement.");
        require(inventory(104.1f,"weapon_perp_phys_p99"),"New Gun Game weapon missing.");
        require(held_weapon_at(demo,*found,104.1f).class_name == "weapon_perp_phys_p99",
            "Held weapon did not advance with Gun Game.");
        require(inventory(90,"weapon_perp_phys_p99"),"Rewind failed to restore pre-event inventory.");
        std::cout << "PASS: real Gun Game strip, OTS-33 to P99 replacement, held model and rewind\n";
    } else if (argc == 3) {
        const std::atomic<bool> cancel{false};
        const auto demo = load_demo(argv[1],argv[2],[](float,const std::string&) {},cancel);
        const auto player = [&](int entity) -> const DemoPlayer& {
            const auto found = std::find_if(demo.players.begin(),demo.players.end(),
                [&](const auto& value) { return value.entity == entity; });
            require(found != demo.players.end(),"October 8 fixture player missing.");
            return *found;
        };
        RagdollPose ragdoll;
        require(player_incapacitated(player(65),315.1f)
            && ragdoll_pose_at(demo,player(65),315.1f,ragdoll),"October 8 recorded corpse unavailable.");
        const auto held = held_weapon_at(demo,player(60),2184);
        require(held.class_name == "weapon_perp_bottle","October 8 bottle handle/model resolution failed.");
        const auto* attack = player_attack_at(player(89),395.56f);
        require(attack && attack->value == 0 && std::abs(attack->time-395.44f) < 0.001f,
            "October 8 fist attack missing from native playback.");
        for (const int entity : {555,970}) {
            const auto track = std::find_if(demo.props.begin(),demo.props.end(),
                [&](const auto& prop) { return prop.entity == entity && prop.brush; });
            require(track != demo.props.end(),"October 8 parented brush track missing.");
            PropPose pose;
            require(sample_prop(*track,2810.64f,pose),"October 8 brush re-entry pose missing.");
            const float expected_z = entity == 555 ? 50.0f : 1708.375f;
            near(pose.z,expected_z);
            if (entity == 555) {
                near(pose.y,30);
                require(sample_prop(*track,2855.8f,pose),"Closed shutter pose missing."); near(pose.y,30);
                require(sample_prop(*track,2856.4f,pose),"Open shutter pose missing."); near(pose.y,90);
            } else {
                require(sample_prop(*track,2855.8f,pose),"Elevator parent pose missing.");
            }
            const auto leaf = std::find_if(demo.props.begin(),demo.props.end(),
                [](const auto& prop) { return prop.entity == 800 && prop.map_attached; });
            require(leaf != demo.props.end() && leaf->map_created_id == 2764,
                "Visible elevator leaf lost its exact map identity.");
            PropPose leaf_pose;
            require(sample_prop(*leaf,2856.4f,leaf_pose),"Visible elevator leaf vanished with its parent.");
            near(leaf_pose.x,-1); near(leaf_pose.y,-14); near(leaf_pose.z,4);
            near(leaf_pose.yaw,90);
            require(!leaf->hidden.empty() && timed_int_at(leaf->hidden,2856.4f,1) == 0,
                "Server-enabled elevator leaf retained its authored disabled visibility.");
            std::cout << "PASS: recorded brush entity=" << entity << " position="
                << pose.x << "," << pose.y << "," << pose.z << "\n";
        }
        require(std::any_of(demo.sounds.begin(),demo.sounds.end(),[](const auto& sound) {
            return sound.name == "ambient/fire/mtov_flame2.wav" && std::abs(sound.time-19.04f) < 0.001f;
        }),"October 8 server sound did not reach native playback.");
        std::cout << "PASS: real October 8 corpse, bottle, fist, parented brush and custom sound integration\n";
    } else if (argc != 1) {
        std::cerr << "Usage: gmod-replay-combat-tests [<October8-demo.dem> <gmoddem.exe>]"
            " | --recovered-pair <Build.dem> <build-recovered.dem> <gmoddem.exe>"
            " | --modern-clothing <warehouse-demo.dem> <gmoddem.exe> <perp-data-root>\n";
        return 1;
    }

    DemoData grenade_demo;
    const int grenade_handle = weapon_handle(42,5);
    grenade_demo.weapons[grenade_handle].grenade = {{1,1},{2,2},{3,3},{4,4}};
    grenade_demo.weapons[grenade_handle].gone = 5;
    grenade_demo.weapons[weapon_handle(42,6)].grenade = {{6,1}};
    require(!grenade_state_at(grenade_demo,grenade_handle,0),"Future grenade state leaked backwards.");
    require(grenade_state_at(grenade_demo,grenade_handle,4.2f)->value == 4,"Grenade throw state missing.");
    require(grenade_state_at(grenade_demo,grenade_handle,2.2f)->value == 2,"Rewind retained thrown state.");
    require(!grenade_state_at(grenade_demo,grenade_handle,5),"Deleted grenade retained its animation.");
    require(!grenade_state_at(grenade_demo,weapon_handle(42,6),4.2f),"Grenade state leaked across entity serials.");
    std::cout << "PASS: grenade state transitions, deletion, serial isolation and rewind\n";
    PerpheadsRegistry sound_registry;
    WeaponAttachmentDef iron;
    iron.id = 1;
    iron.sequence = 2;
    iron.sight_sequence = 3;
    sound_registry.weapon_attachments.emplace(1, iron);
    WeaponAttachmentDef optic;
    optic.id = 2;
    optic.sight = true;
    sound_registry.weapon_attachments.emplace(2, optic);
    DemoData sight_demo;
    const int sight_handle = weapon_handle(30, 4);
    auto& sight_weapon = sight_demo.weapons[sight_handle];
    sight_weapon.attachments = {{1, "1"}, {2, "1,2"}, {3, "1"}};
    sight_weapon.gone = 4;
    require(weapon_attachment_sequence_at(sound_registry, sight_demo, 1, sight_handle, 1) == 2,
        "Iron sights must use their original upright sequence without an optic.");
    require(weapon_attachment_sequence_at(sound_registry, sight_demo, 1, sight_handle, 2) == 3,
        "Fitted optic must select the historical folded sight sequence.");
    require(weapon_attachment_sequence_at(sound_registry, sight_demo, 1, sight_handle, 3) == 2,
        "Removing optic must restore upright iron sights.");
    require(weapon_attachment_sequence_at(sound_registry, sight_demo, 1, sight_handle, 2) == 3,
        "Rewind must restore folded iron sights.");
    require(weapon_attachment_sequence_at(sound_registry, sight_demo, 1, weapon_handle(30, 5), 2) == 2,
        "Optic state must not leak across weapon entity serials.");
    require(weapon_attachment_sequence_at(sound_registry, sight_demo, 1, sight_handle, 4) == 2,
        "Deleted weapon must not retain folded sight state.");
    std::cout << "PASS: historical iron-sight sequences, optic removal, rewind and weapon serial isolation\n";
    WeaponAttachmentDef suppressor;
    suppressor.id = 23;
    suppressor.sound = "suppressor_919";
    sound_registry.weapon_attachments.emplace(23,suppressor);
    WeaponAttachmentDef compensator;
    compensator.id = 24;
    compensator.name = "rifle_suppressor_flash";
    sound_registry.weapon_attachments.emplace(24,compensator);
    DemoData suppressed_demo;
    const int suppressed_handle = weapon_handle(20,3);
    auto& suppressed_weapon = suppressed_demo.weapons[suppressed_handle];
    suppressed_weapon.attachments = {{1,"24"},{2,"23,24"},{3,"24"},{4,"23"}};
    suppressed_weapon.gone = 5;
    require(weapon_sound_override_at(sound_registry,suppressed_demo,suppressed_handle,1.5f).empty(),
        "Flash hider was mistaken for a sound suppressor.");
    require(weapon_sound_override_at(sound_registry,suppressed_demo,suppressed_handle,2.5f) == "suppressor_919",
        "Fitted suppressor did not select its historical sound.");
    require(weapon_sound_override_at(sound_registry,suppressed_demo,suppressed_handle,3.5f).empty(),
        "Removing suppressor retained suppressed sound.");
    require(weapon_sound_override_at(sound_registry,suppressed_demo,suppressed_handle,2.5f) == "suppressor_919",
        "Seeking back lost suppressed sound.");
    require(weapon_sound_override_at(sound_registry,suppressed_demo,weapon_handle(20,4),4.5f).empty(),
        "Suppressed sound leaked across reused entity serials.");
    require(weapon_sound_override_at(sound_registry,suppressed_demo,suppressed_handle,5).empty(),
        "Deleted weapon retained its suppressor sound.");
    std::cout << "PASS: attachment-defined suppressor sound, flash-hider exclusion, removal, deletion, serial isolation and rewind\n";
    DemoPlayer reload_player;
    reload_player.active_weapon = {{0,100,weapon_handle(100,1),"weapon_perp_phys_ots33"}};
    reload_player.reloads = {{2,3},{3,4},{4,5},{5,3}};
    require(!player_reload_at(reload_player,1),"Future reload leaked before start.");
    require(player_reload_at(reload_player,2.5f)->time == 2,"Reload start missing.");
    require(player_reload_at(reload_player,3.5f)->time == 2,"Reload loop restarted the whole animation.");
    require(!player_reload_at(reload_player,4.5f),"Reload-end event was ignored.");
    reload_player.attacks.push_back({5.5f,0});
    require(!player_reload_at(reload_player,6),"Firing did not interrupt the reload.");
    require(player_reload_at(reload_player,5.25f)->time == 5,"Rewind retained a future interruption.");
    reload_player.active_weapon.push_back({5.1f,100,weapon_handle(100,2),"weapon_perp_phys_ots33"});
    require(!player_reload_at(reload_player,5.25f),"Reload leaked into a replacement weapon handle.");
    std::cout << "PASS: recorded reload start/loop/end, firing interruption, weapon changes and rewind\n";
    DemoPlayer injured;
    injured.info = {{0,100,-1,0,{}},{2,-10,-1,2,{}},{5,100,-1,0,{}}};
    injured.ragdoll = {{2.1f,100},{5,0}};
    require(!player_incapacitated(injured,1),"Live player was treated as a corpse.");
    require(player_incapacitated(injured,2.05f),"Death before ragdoll link kept the standing body.");
    require(player_incapacitated(injured,3),"Recorded ragdoll state ignored.");
    require(!player_incapacitated(injured,5),"Respawn did not restore alive rendering.");
    require(player_incapacitated(injured,3),"Rewinding lost the incapacitated state.");
    DemoData ragdoll_demo;
    ragdoll_demo.ragdolls.push_back({100,1,7,{{2,{1,2,3},{0,0,0}}}});
    RagdollPose body_pose;
    require(!ragdoll_pose_at(ragdoll_demo,injured,2.05f,body_pose),"Future body link leaked backwards.");
    require(ragdoll_pose_at(ragdoll_demo,injured,3,body_pose) && body_pose.model == 7,
        "Recorded corpse pose missing.");
    require(!ragdoll_pose_at(ragdoll_demo,injured,5,body_pose),"Respawn retained old corpse pose.");
    ragdoll_demo.ragdolls[0].keys[0].ang.clear();
    require(!ragdoll_pose_at(ragdoll_demo,injured,3,body_pose),"Incomplete corpse pose accepted.");
    DemoPlayer holder;
    holder.active_weapon = {{0,10,weapon_handle(10,2),{}},{4,11,weapon_handle(11,3),{}}};
    ragdoll_demo.weapons[weapon_handle(10,2)].class_name = {{1,"weapon_bottle"}};
    ragdoll_demo.weapons[weapon_handle(10,3)].class_name = {{0,"weapon_unrelated"}};
    require(held_weapon_at(ragdoll_demo,holder,0.5f).class_name == "weapon_bottle",
        "Late immutable weapon class was lost or confused with a reused index.");
    holder.attacks = {{1,0},{2,1},{4.5f,0}};
    require(!player_attack_at(holder,0.5f),"Future melee attack leaked backwards.");
    require(player_attack_at(holder,1.5f)->value == 0,"Primary melee attack missing.");
    require(player_attack_at(holder,2.5f)->value == 1,"Secondary attack missing.");
    require(!player_attack_at(holder,4.25f),"Old attack leaked across weapon switch.");
    require(player_attack_at(holder,4.75f)->time == 4.5f,"New held-weapon attack missing.");
    holder.entity = 7;
    auto& bottle = ragdoll_demo.weapons[weapon_handle(10,2)];
    bottle.owner = {{0,7},{2,-1},{3,7}};
    bottle.gone = 3.5f;
    bottle.attachments = {{0,"56,12"},{2,"12"}};
    bool attachments_known = false;
    require(weapon_attachments_at(ragdoll_demo,weapon_handle(10,2),1,attachments_known)
        == std::vector<int>({56,12}) && attachments_known,"Fitted attachments missing.");
    require(weapon_attachments_at(ragdoll_demo,weapon_handle(10,2),2,attachments_known)
        == std::vector<int>({12}),"Removed optic remained attached.");
    require(weapon_attachments_at(ragdoll_demo,weapon_handle(10,3),2,attachments_known).empty()
        && !attachments_known,"Entity reuse borrowed another serial's attachments.");
    require(weapon_attachments_at(ragdoll_demo,weapon_handle(10,2),1,attachments_known).size() == 2,
        "Rewind failed to restore fitted optic.");
    require(inventory_at(ragdoll_demo,holder,1) == std::vector<std::string>{"weapon_bottle"},
        "Owned weapon missing from inventory.");
    require(inventory_at(ragdoll_demo,holder,2).empty()
        && held_weapon_at(ragdoll_demo,holder,2).handle < 0,"Dropped weapon remained owned or held.");
    require(inventory_at(ragdoll_demo,holder,3).size() == 1,"Pickup did not restore inventory.");
    require(inventory_at(ragdoll_demo,holder,3.5f).empty()
        && held_weapon_at(ragdoll_demo,holder,3.5f).handle < 0,"Deleted weapon remained owned or held.");
    require(held_weapon_at(ragdoll_demo,holder,1).class_name == "weapon_bottle",
        "Rewind did not restore weapon ownership.");
    std::cout << "PASS: incapacitation, missing corpse pose, respawn/rewind, serial-safe held model, melee events\n";
    DemoProp shutter;
    shutter.brush = true;
    PropKey closed;
    closed.time = 1; closed.has_position = true; closed.x = 10; closed.z = 20;
    closed.has_yaw = true; closed.yaw = 350;
    auto open = closed; open.time = 2; open.z = 120; open.yaw = 10;
    PropKey left; left.time = 3; left.present = false;
    auto returned = open; returned.time = 5; returned.z = 60;
    shutter.keys = {closed,open,left,returned};
    PropPose shutter_pose;
    require(!sample_prop(shutter,0,shutter_pose),"Future shutter position leaked before first record.");
    require(sample_prop(shutter,1.5f,shutter_pose),"Sliding shutter pose missing.");
    near(shutter_pose.z,70); near(shutter_pose.yaw,360);
    require(sample_prop(shutter,4,shutter_pose),"Shutter vanished when outside recorder visibility.");
    near(shutter_pose.z,120);
    require(sample_prop(shutter,5,shutter_pose),"Shutter re-entry missing."); near(shutter_pose.z,60);
    require(sample_prop(shutter,1,shutter_pose),"Rewind lost closed shutter."); near(shutter_pose.z,20);
    shutter.brush = false;
    require(!sample_prop(shutter,4,shutter_pose),"Ordinary prop visibility behavior changed.");
    std::cout << "PASS: sliding brush translation, rotation, PVS persistence, re-entry and rewind\n";
    DemoProp car;
    car.vehicle = true;
    car.entity = 100;
    PropKey moving;
    moving.present = moving.has_position = moving.has_yaw = true;
    car.keys.push_back(moving);
    moving.time = 0.02f;
    moving.has_position = false;
    moving.yaw = 10;
    car.keys.push_back(moving);
    moving.time = 0.04f;
    moving.has_position = true;
    moving.x = 4;
    car.keys.push_back(moving);
    PropPose car_pose;
    require(sample_prop(car,0.01f,car_pose),"Vehicle interpolation missing.");
    near(car_pose.x,1);
    require(sample_prop(car,0.03f,car_pose),"Vehicle angle-only key blocked translation.");
    near(car_pose.x,3);
    near(car_pose.yaw,10);
    require(sample_prop(car,0.01f,car_pose),"Vehicle rewind failed.");
    near(car_pose.x,1);
    DemoData seats;
    seats.duration = 5;
    DemoPlayer driver;
    driver.entity = 1;
    driver.seated = {{1,true},{4,false}};
      driver.look = {{1,20,45},{2,30,60}};
    seats.players.push_back(driver);
    car.keys.resize(1);
    car.keys[0].x = 100; car.keys[0].y = 200; car.keys[0].z = 300;
    car.keys[0].yaw = 90;
    car.owners = {{1,1}};
    car.driver_attachment = std::array<float,12>{0,-1,0,-20,1,0,0,10,0,0,1,20};
    seats.props.push_back(car);
    rebind_vehicle_seats(seats);
      float look_pitch=0,look_yaw=0;
      require(sample_player_look(seats,0,2,look_pitch,look_yaw),"Seat synthesis discarded eye angles.");
      near(look_pitch,30);near(look_yaw,60);
    PlayerPose driver_pose;
    require(sample_player(seats.players[0],2,driver_pose),"Model-specific driver seat missing.");
    near(driver_pose.x,90); near(driver_pose.y,180); near(driver_pose.z,320);
    near(std::remainder(driver_pose.yaw - 180,360),0);
    const auto count = seats.players[0].samples.size();
    seats.props[0].driver_attachment->at(3) = 20;
    rebind_vehicle_seats(seats);
    require(seats.players[0].samples.size() == count,"Seat rebinding duplicated synthesized poses.");
    require(sample_player(seats.players[0],2,driver_pose),"Right-hand-drive seat missing.");
    near(driver_pose.x,90); near(driver_pose.y,220); near(driver_pose.z,320);
    std::cout << "PASS: angle-only vehicle keys do not freeze translation; model-specific left/right driver seats and rebinding\n";
    DemoEffect impact;
    impact.time = 100;
    impact.origin = {120,-80,50};
    require(blood_mist_particle(impact,0,99).opacity == 0,"Future blood mist appeared.");
    require(blood_mist_particle(impact,0,100).opacity > 0,"Recorded blood mist missing.");
    const auto puff = blood_mist_particle(impact,3,100.25f);
    near(puff.radius,15);
    near(puff.opacity,0.45f * (1.0f - 0.25f / BloodMistLifetime));
    require(puff.radius > 12 && puff.opacity > 0.25f,"Blood mist is not visibly exaggerated.");
    require(blood_mist_particle(impact,3,101).opacity == 0,"Blood mist did not expire.");
    require(blood_mist_particle(impact,3,100 + BloodMistLifetime).opacity == 0,"Mist lifetime boundary failed.");
    const auto rewound = blood_mist_particle(impact,3,100.25f);
    require(rewound.position == puff.position && rewound.opacity == puff.opacity
        && rewound.radius == puff.radius,"Seeking changed blood mist randomness.");
    require(blood_mist_particle(impact,4,100.25f).position != puff.position,"Blood particles overlap exactly.");
    std::cout << "PASS: blood mist spawn, expiry, expansion, deterministic pause and rewind\n";
    DemoEffect smoke;
    smoke.time = 100;
    smoke.kind = "dispatch";
    smoke.name = "gdcw_universal_impact";
    smoke.origin = {10,20,30};
    smoke.normal = {0,1,0};
    smoke.scale = 0.5f;
    smoke.material = 67;
    require(surface_smoke_effect(smoke),"Concrete bullet impact smoke missing.");
    for (const int material : {45,65,66,70,72,83}) {
        smoke.material = static_cast<float>(material);
        require(!surface_smoke_effect(smoke),"Flesh impact incorrectly emitted grey dust.");
    }
    smoke.material = 67;
    smoke.name = "gdcw_universal_penetrate";
    require(surface_smoke_effect(smoke),"Recorded bullet exit smoke missing.");
    smoke.name = "bloodimpact";
    require(!surface_smoke_effect(smoke),"Blood event emitted grey smoke.");
    smoke.name = "gdcw_universal_impact";
    const auto dust = smoke_particle(smoke,2,100.2f,false);
    require(dust.opacity > 0 && dust.radius > 0 && dust.position[1] > smoke.origin[1],
        "Surface smoke did not travel out of the recorded surface.");
    require(smoke_particle(smoke,2,100.4f,false).radius > dust.radius,"Smoke did not expand.");
    require(smoke_particle(smoke,2,100.4f,false).opacity < dust.opacity,"Smoke did not fade.");
    const auto rewind_smoke = smoke_particle(smoke,2,100.2f,false);
    require(rewind_smoke.position == dust.position && rewind_smoke.opacity == dust.opacity,
        "Smoke changed after pause/seek/rewind.");
    require(smoke_particle(smoke,2,99,false).opacity == 0,"Future smoke appeared.");
    require(smoke_particle(smoke,2,101,false).opacity == 0,"Expired smoke remained.");
    require(smoke_particle(smoke,2,100+SmokeLifetime,false).opacity == 0,"Smoke expiry boundary failed.");
    const auto muzzle_smoke = smoke_particle(smoke,2,100.2f,true);
    require(muzzle_smoke.opacity > 0 && muzzle_smoke.position != dust.position,
        "Muzzle smoke did not use its own particle motion.");
    smoke.material = 87;
    require(smoke_particle(smoke,2,100.2f,false).color != dust.color,
        "Wood and concrete impacts used identical dust colors.");
    smoke.material = 67;
    smoke.normal = {0,4,0};
    require(smoke_particle(smoke,2,100.2f,false).position == dust.position,
        "Smoke motion depends on the encoded normal's length.");
    smoke.normal = {0,-1,0};
    require(smoke_particle(smoke,2,100.2f,false).position[1] < smoke.origin[1],
        "Smoke ignored a reversed impact normal.");
    smoke.normal = {0,0,0};
    require(smoke_particle(smoke,2,100.2f,false).opacity == 0,"Invalid normal invented smoke direction.");
    smoke.normal = {0,1,0};
    smoke.scale = 0;
    require(smoke_particle(smoke,2,100.2f,false).opacity == 0,"Zero-strength impact produced smoke.");
    DemoData shot_demo;
    shot_demo.players.resize(1);
    shot_demo.recorder_player = 0;
    shot_demo.players[0].shots = {1,1,1.04f,2,3};
    const auto shot_times = recorded_shot_times(shot_demo,0,{2.02f,5});
    require(shot_times == std::vector<float>({1,1.04f,2,3,5}),
        "Shot union doubled an event or lost rapid counter fire.");
    std::cout << "PASS: surface and muzzle smoke, material exclusion, expansion/fade, fixed emission, pause/rewind and deduplicated shot sources\n";
    near(decal_opacity(100,99),0);
    near(decal_opacity(100,100),1);
    near(decal_opacity(100,154.999f),1);
    near(decal_opacity(100,155),1);
    near(decal_opacity(100,157.5f),0.5f);
    require(decal_opacity(100,159.999f) > 0,"Decal expired before 60 seconds.");
    near(decal_opacity(100,160),0);
    near(decal_opacity(100,200),0);
    near(decal_opacity(100,157.5f),0.5f);
    near(decal_opacity(100,110),1);
    near(decal_opacity(100,110),1);
    std::cout << "PASS: 60-second decal lifetime, final 5-second fade, exact boundaries, rewind, pause\n";
    DemoDoor door;
    door.yaws = {{1,350,20,90},{1.04f,10,40,110},{1.08f,-40,0,90},{300,0,0,90}};
    std::array<float,3> angles{5,15,25};
    require(!door_angles_at(door,0,angles),"A future door rotation leaked into earlier playback.");
    near(angles[0],5); near(angles[1],15); near(angles[2],25);
    require(door_angles_at(door,1.02f,angles),"Door interpolation unavailable.");
    near(angles[0],30); near(angles[1],360); near(angles[2],100);
    require(door_angles_at(door,1.06f,angles),"Reverse-swing interpolation unavailable.");
    near(angles[0],20); near(angles[1],-15); near(angles[2],100);
    require(door_angles_at(door,4,angles),"Settled door rotation unavailable.");
    near(angles[0],0); near(angles[1],-40); near(angles[2],90);
    require(door_angles_at(door,299,angles),"Idle door orientation unavailable.");
    near(angles[1],-40);
    require(door_angles_at(door,300,angles),"Door re-entry orientation unavailable.");
    near(angles[1],0);
    require(door_angles_at(door,1,angles),"Seeking backwards lost the recorded door orientation.");
    near(angles[0],20); near(angles[1],350); near(angles[2],90);
    std::cout << "PASS: complete door axes, angle wrap, both swing directions, seek, no future rotation\n";

    const auto wave = [](const char* chunk, std::size_t size, std::size_t count_at, int count) {
        std::vector<std::uint8_t> bytes(20 + size);
        std::memcpy(bytes.data(),"RIFF",4);
        const auto length = static_cast<std::uint32_t>(bytes.size() - 8);
        std::memcpy(bytes.data() + 4,&length,4);
        std::memcpy(bytes.data() + 8,"WAVE",4);
        std::memcpy(bytes.data() + 12,chunk,4);
        const auto chunk_size = static_cast<std::uint32_t>(size);
        std::memcpy(bytes.data() + 16,&chunk_size,4);
        if (count_at < size) std::memcpy(bytes.data() + 20 + count_at,&count,4);
        return bytes;
    };
    auto bytes = wave("data",4,0,0);
    require(gmod::audio::wav_has_loop(bytes) == false,"A stoppable one-shot was classified as looping.");
    require(gmod::audio::wav_has_loop(wave("cue ",28,0,1)) == true,"Source WAV loop cue lost.");
    require(gmod::audio::wav_has_loop(wave("smpl",60,28,1)) == true,"WAV sampler loop lost.");
    require(gmod::audio::wav_has_loop(wave("cue ",4,0,0)) == false,"Empty cues became an infinite loop.");
    require(!gmod::audio::wav_has_loop(wave("cue ",4,0,1)),"Truncated cue record accepted.");
    bytes.pop_back();
    require(!gmod::audio::wav_has_loop(bytes),"Truncated WAV accepted.");
    require(gmod::audio::uses_wave_loop_metadata("plats/platstop1.wav"),
        "Bank shutter stop sounds must use WAV loop metadata, not later stop commands.");
    require(gmod::audio::uses_wave_loop_metadata("plats/elevator_move.wav")
        && gmod::audio::uses_wave_loop_metadata("doors/doormove1.wav")
        && gmod::audio::uses_wave_loop_metadata("buttons/button1.wav")
        && !gmod::audio::uses_wave_loop_metadata("vehicles/engine.wav"),
        "Mechanical sound loop classification changed unrelated sounds.");
    std::cout << "PASS: finite door/platform stops, Source loop cues, sampler loops, malformed WAV rejection\n";

    using namespace gmod::source;
    std::vector<bsp::Triangle> triangles(2);
    triangles[0].a={-10,-10,0}; triangles[0].b={10,-10,0}; triangles[0].c={10,10,0};
    triangles[1].a={-10,-10,0}; triangles[1].b={10,10,0}; triangles[1].c={-10,10,0};
    const WorldTrace trace(triangles);
    const float point[3]{9,0,0}, normal[3]{0,0,1};
    const auto decal=trace.project_decal(point,normal,8,8);
    require(!decal.empty() && decal.size()%3 == 0,"Clipped decal did not produce triangles.");
    for (const auto& vertex:decal) {
        require(vertex.position[0] <= 10.001f && vertex.position[0] >= 4.999f,
                "Decal extends outside its surface or projector.");
        require(vertex.position[1] >= -4.001f && vertex.position[1] <= 4.001f,"Decal height exceeds projector.");
        near(vertex.position[2],0.08f);
        for (const float uv:vertex.uv) require(uv >= -0.001f && uv <= 1.001f,"Decal UV out of range.");
    }
    const float nowhere[3]{0,0,20};
    require(trace.project_decal(nowhere,normal,8,8).empty(),"Decal floated away from its surface.");
    const float zero[3]{};
    require(trace.project_decal(point,zero,8,8).empty(),"Invalid projection normal accepted.");
    for (auto& triangle:triangles) triangle.material="glass/window";
    WorldTrace::Hit hit;
    const float origin[3]{0,0,1}, direction[3]{0,0,-1};
    require(!WorldTrace(triangles).trace(origin,direction,2,hit),"Glass blocks ordinary visibility.");
    require(WorldTrace(triangles,true).trace(origin,direction,2,hit),"Glass cannot receive recorded bullet decals.");
    near(hit.distance,1);
    std::cout << "PASS: surface clipping, negative grid cells, depth offset, UVs, invalid normals, glass decals\n";
    const auto sweep = [&](std::array<float,3> a, std::array<float,3> b, float radius) {
        return WorldTrace(triangles,true).sweep_sphere(a.data(),b.data(),radius);
    };
    near(sweep({0,0,20},{0,0,-20},2),0.45f);
    near(sweep({0,0,-20},{0,0,20},2),0.45f);
    near(sweep({0,0,20},{0,0,10},2),1);
    near(sweep({0,0,1},{0,0,20},2),0);
    near(sweep({0,0,20},{0,0,20},2),1);
    near(sweep({11,0,20},{11,0,-20},2),(20-std::sqrt(3.0f))/40);
    near(sweep({11,11,20},{11,11,-20},2),(20-std::sqrt(2.0f))/40);
    near(sweep({13,0,20},{13,0,-20},2),1);
    near(sweep({0,0,2},{20,0,2},2),0);
    const float edge_start[3]{11,0,20}, edge_direction[3]{0,0,-1};
    require(!WorldTrace(triangles,true).trace(edge_start,edge_direction,40,hit),
        "Edge fixture must miss with a point ray.");
    std::vector<bsp::Triangle> large(2);
    large[0].a={-20000,-20000,0}; large[0].b={20000,-20000,0}; large[0].c={20000,20000,0};
    large[1].a={-20000,-20000,0}; large[1].b={20000,20000,0}; large[1].c={-20000,20000,0};
    const float above[3]{-400,-400,20}, below[3]{-400,-400,-20};
    near(WorldTrace(large,true).sweep_sphere(above,below,2),0.45f);
    require(WorldTrace(large,true).trace(above,edge_direction,40,hit),"Oversized world face lost collision.");
    near(WorldTrace({},true).sweep_sphere(above,below,8),1);
    // Collision is a pure query: leaving a wall restores full distance, and
    // seeking back gives the same result without retained camera state.
    near(sweep({0,0,20},{0,0,-20},2),0.45f);
    near(sweep({50,0,20},{50,0,-20},2),1);
    near(sweep({0,0,20},{0,0,-20},2),0.45f);
    std::cout << "PASS: swept camera volume, two-sided walls/glass, edges/corners, overlap, empty/large surfaces, clearance and deterministic seek\n";
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << "\n";
    return 1;
}
