#include "demo/BankLighting.h"
#include "demo/BankShutters.h"
#include "source/bsp/BspReader.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
void require(bool value,const char* text) { if(!value) throw std::runtime_error(text); }
int wmain(int argc,wchar_t** argv) try {
    using namespace gmod::demo;
    using namespace gmod::source::bsp;
    DemoData demo;
    demo.bank_lights={{0,35,1},{0,36,1},{0,37,0},{0,38,0},{2,35,0},{2,37,1},{8,35,1},{8,37,0}};
    const std::array<int,4> ids{35,36,37,38};
    const BankLightWeights normal{1,1,0,0};
    DemoProp shutter;shutter.animations={{0,3,0,1},{2,2,0,1},{8,0,0,0}};
    require(bank_shutter_animation_at(shutter,3)->sequence==2,"Recorded shutter closing sequence lost");
    require(bank_shutter_cycle(*bank_shutter_animation_at(shutter,3),20,1,false)==1,"Closed shutter restarted");
    require(bank_shutter_animation_at(shutter,1)->sequence==3,"Shutter rewind failed");
    DemoProp reentry;
    record_bank_shutter_animation(reentry,{2,2,0,1},false);
    record_bank_shutter_animation(reentry,{8,2,0,1},false);
    require(reentry.animations.size()==1 && bank_shutter_cycle(reentry.animations.back(),8,2,false)==1,
        "PVS reentry restarted a shutter that had already closed");
    record_bank_shutter_animation(reentry,{9,2,0,1},true);
    require(reentry.animations.size()==2,"Explicit cycle reset could not restart shutter animation");
    PropKey authored;authored.has_position=true;authored.x=-9038;authored.y=3775;authored.z=1501;
    reentry.keys={authored};
    const float map_origin[]{-9038,3775,1501},wrong_origin[]{-9040,3775,1501};
    require(bank_shutter_matches_origin(reentry,map_origin),"Shutter could not match a map with shifted entity IDs");
    require(!bank_shutter_matches_origin(reentry,wrong_origin),"Shutter matched the wrong authored position");
    require(bank_lighting_at(demo,1,ids,normal)==normal,"Initial bank lighting missing");
    require(bank_lighting_at(demo,2,ids,normal)[2]==1,"Raid lighting switch missing");
    require(bank_lighting_at(demo,8,ids,normal)[0]==1,"Normal lighting did not return");
    require(bank_lighting_at(demo,3,ids,normal)[0]==0,"Backward raid seek failed");
    require(bank_lighting_at(demo,0,ids,normal)==normal,"Rewind to normal failed");
    if(argc>=2) {
        BspReader bsp(argv[1]);bsp.read();
        const auto& atlas=bsp.lightmap_atlas();
        require(atlas.styled && atlas.styled->bank_styles==ids,"Bank lights were not resolved from map target names");
        require(atlas.styled->initial[35]==1 && atlas.styled->initial[37]==0,"Startup bank light state incorrect");
        auto alert=atlas.styled->initial;alert[35]=alert[36]=0;alert[37]=alert[38]=1;
        const auto changed=lightmap_patches(*atlas.styled,atlas.styled->initial,alert);
        require(!changed.empty(),"Raid switch changed no baked surfaces");
        const auto restored=lightmap_patches(*atlas.styled,alert,atlas.styled->initial);
        for(const auto& patch:restored) for(int y=0;y<patch.height;++y) for(int x=0;x<patch.width;++x)
            require(patch.texels[y*patch.width+x]==atlas.texels[(patch.y+y)*atlas.width+patch.x+x],"Normal lightmap restoration differs");
        const auto bake_start=std::chrono::steady_clock::now();
        const auto probe=bsp.make_probe(true);
        const auto bake_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-bake_start).count();
        float delta=0;
        for(const Vec3 p:{Vec3{-9450,3650,1400},Vec3{-9800,3600,1400},Vec3{-10300,3500,1080}}) {
            const auto n=probe.sample(p,normal),r=probe.sample(p,BankLightWeights{0,0,1,1});
            for(int face=0;face<6;++face) for(int c=0;c<3;++c) delta+=std::fabs(n.color[face][c]-r.color[face][c]);
        }
        require(delta>0.05f,"Baked model lighting did not change for raid state");
        const auto outside=probe.sample({-5000,-900,1000});
        const auto outside_raid=probe.sample({-5000,-900,1000},BankLightWeights{0,0,1,1});
        for(int face=0;face<6;++face) for(int c=0;c<3;++c)
            require(outside.color[face][c]==outside_raid.color[face][c],"Bank lighting affected unrelated area");
        double baseline_ms=0,baked_ms=0;float checksum=0;
        for(int round=0;round<4;++round) for(int pass=0;pass<2;++pass) {
            const bool enabled=(pass+round)%2;
            const auto start=std::chrono::steady_clock::now();
            for(int i=0;i<20000;++i) {
                const Vec3 p{-10000.0f+(i%10)*110.0f+float((i/100)%32),3300.0f+((i/10)%10)*30.0f,1400};
                const auto cube=enabled ? probe.sample(p,normal) : probe.sample(p);
                checksum+=cube.color[0][0];
            }
            const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            (enabled ? baked_ms : baseline_ms)+=ms;
        }
        std::cout << "BANK_BAKED patches=" << changed.size() << " one_time_probe_bake_ms=" << bake_ms << " model_light_delta=" << delta << '\n';
        std::cout << "PROBE_BENCHMARK queries=20000 baseline_ms=" << baseline_ms/4 << " baked_ms=" << baked_ms/4 << " checksum=" << checksum << '\n';
        if(argc==4) {
            const std::atomic<bool> cancel{false};
            const auto captured=load_demo(argv[2],argv[3],[](float,const std::string&){},cancel);
            require(!captured.bank_lights.empty(),"Captured demo lightstyles missing");
            int shutters=0;
            for(const auto& prop:captured.props) if(!prop.animations.empty()) {
                require(prop.model>=0 && is_bank_shutter(captured.models[prop.model]),"Unrelated map model acquired animation overhead");
                require(prop.map_attached,"Bank shutter was not attached to its authored map model");
                require(prop.animations.front().time>20 && prop.animations.front().time<30,"Shutter timestamps were not rescaled");
                ++shutters;
            }
            require(shutters==2,"Missing captured bank shutter models");
            require(bank_lighting_at(captured,0,ids,normal)==normal,"Captured bank lightstyles decoded incorrectly");
            std::cout << "PASS captured bank style changes=" << captured.bank_lights.size() << '\n';
        }
    }
    std::cout << "PASS baked bank state changes and rewind\n";
    return 0;
} catch(const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n';return 1; }
