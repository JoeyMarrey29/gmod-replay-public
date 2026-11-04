#pragma once
#include "demo/DemoData.h"
#include <algorithm>
#include <cmath>

namespace gmod::demo {
// Source fist TraceHull uses an axis-aligned (-6,-8,-5)..(6,8,5) hull.
// Expanding a player's bounds by that hull turns the sweep into a segment test.
inline float fist_player_fraction(const std::array<float,3>& start,const std::array<float,3>& end,
                                  const PlayerPose& target) {
    const float low[3]{target.x-22,target.y-24,target.z-5};
    const float high[3]{target.x+22,target.y+24,target.z+((target.flags&2)!=0?54.0f:72.0f)+5};
    float enter=0,leave=1;
    for(int axis=0;axis<3;++axis) {
        const float delta=end[axis]-start[axis];
        if(std::fabs(delta)<0.00001f) {
            if(start[axis]<low[axis] || start[axis]>high[axis]) return 1;
        } else {
            float a=(low[axis]-start[axis])/delta,b=(high[axis]-start[axis])/delta;
            if(a>b) std::swap(a,b);
            enter=std::max(enter,a);leave=std::min(leave,b);
            if(enter>leave) return 1;
        }
    }
    return enter;
}
inline bool view_model_restarted(const ViewModelKey& before,const ViewModelKey& key) {
    return before.model!=key.model || before.sequence!=key.sequence
        || before.new_sequence!=key.new_sequence || before.animation_parity!=key.animation_parity;
}
inline void merge_melee_attacks(DemoPlayer& player,const std::vector<TimedInt>& inferred) {
    for(const auto& key:inferred) {
        if(std::none_of(player.attacks.begin(),player.attacks.end(),[&](const auto& existing) {
            return existing.value==key.value && std::fabs(existing.time-key.time)<0.06f;
        })) player.attacks.push_back(key);
    }
    std::stable_sort(player.attacks.begin(),player.attacks.end(),[](const auto& a,const auto& b){return a.time<b.time;});
}
inline bool has_recorded_melee_sound(const DemoData& demo,int player,int weapon,float time,
                                      const std::vector<std::string>& files,const std::array<float,3>* point=nullptr) {
    const auto first=std::lower_bound(demo.sounds.begin(),demo.sounds.end(),time-0.15f,
        [](const auto& sound,float t){return sound.time<t;});
    for(auto sound=first;sound!=demo.sounds.end() && sound->time<=time+0.15f;++sound) {
        bool owner=sound->entity==player || sound->entity==weapon;
        if(!owner && sound->entity<=0 && point) {
            const float dx=sound->x-(*point)[0],dy=sound->y-(*point)[1],dz=sound->z-(*point)[2];
            owner=dx*dx+dy*dy+dz*dz<=64*64;
        }
        if((sound->flags&7)==0 && owner && std::find(files.begin(),files.end(),sound->name)!=files.end()) return true;
    }
    return false;
}
}
