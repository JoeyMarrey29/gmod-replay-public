#pragma once
#include "demo/DemoData.h"
#include <algorithm>
#include <cmath>
#include <string_view>
namespace gmod::demo {
inline bool is_bank_shutter(std::string_view model) {
    return model=="models/perpheads/props/bankshutters.mdl"
        || model=="models/perpheads/props/bankshutters2.mdl";
}
inline bool bank_shutter_matches_origin(const DemoProp& prop,const float origin[3]) {
    const auto key=std::find_if(prop.keys.begin(),prop.keys.end(),[](const auto& k){return k.has_position;});
    if(key==prop.keys.end() || key->parent>0) return false;
    const float x=key->x-origin[0],y=key->y-origin[1],z=key->z-origin[2];
    return x*x+y*y+z*z<=1.0f;
}
inline const DemoProp::Animation* bank_shutter_animation_at(const DemoProp& prop,float time) {
    if(prop.animations.empty()) return nullptr;
    const auto end=std::upper_bound(prop.animations.begin(),prop.animations.end(),time,
        [](float t,const auto& key) { return t<key.time; });
    return end==prop.animations.begin() ? &prop.animations.front():&*std::prev(end);
}
inline void record_bank_shutter_animation(DemoProp& prop,DemoProp::Animation key,bool cycle_recorded) {
    // Re-entering PVS can resend the same sequence without its cycle. Preserve
    // its original clock so a finished closing sequence stays closed.
    if(!cycle_recorded && !prop.animations.empty()
        && prop.animations.back().sequence==key.sequence && prop.animations.back().rate==key.rate) return;
    prop.animations.push_back(key);
}
inline float bank_shutter_cycle(const DemoProp::Animation& key,float time,float duration,bool loop) {
    float cycle=key.cycle;
    if(duration>0) cycle+=std::max(0.0f,time-key.time)*key.rate/duration;
    return loop ? cycle-std::floor(cycle) : std::clamp(cycle,0.0f,1.0f);
}
}
