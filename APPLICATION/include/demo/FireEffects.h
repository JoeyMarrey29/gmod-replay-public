#pragma once
#include "demo/DemoData.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <limits>

namespace gmod::demo {
struct FireEmitter { FireKey key; float born=0; };

// Only consume changed entities during forward playback. A seek rebuilds the small live set.
class FirePlayback {
public:
    void reset() { cursor_=0; time_=-std::numeric_limits<float>::infinity(); live_.clear(); }
    const std::map<int,FireEmitter>& sample(const std::vector<FireKey>& keys,float time) {
        if(time<time_) reset();
        while(cursor_<keys.size() && keys[cursor_].time<=time) {
            const auto& key=keys[cursor_++];
            const auto it=live_.find(key.entity);
            if(!key.active) {
                if(it!=live_.end() && it->second.key.serial==key.serial) live_.erase(it);
            } else {
                const float born=it!=live_.end() && it->second.key.serial==key.serial ? it->second.born : key.time;
                live_[key.entity]={key,born};
            }
        }
        time_=time;return live_;
    }
private:
    std::size_t cursor_=0;
    float time_=-std::numeric_limits<float>::infinity();
    std::map<int,FireEmitter> live_;
};

struct FlameParticle {
    std::array<float,3> position{},color{};
    float radius=0,opacity=0;
};

// Reconstruct the server's ent_fire Lua emission interval, size, rise and blue base.
// Hashing the entity and emission number makes pause, frame rate and rewind agree.
inline FlameParticle fire_particle(const FireEmitter& fire,int emission,bool blue,float time) {
    FlameParticle result;
    if (fire.key.scale<=0) return result;
    const float age=time-(fire.born+emission*0.06f);
    if(age<0 || emission<0) return result;
    std::uint32_t seed=std::uint32_t(fire.key.entity)*9781u+std::uint32_t(fire.key.serial)*6271u
        +std::uint32_t(emission)*1597334677u;
    const auto random=[&]() {
        seed^=seed>>16;seed*=0x7feb352du;seed^=seed>>15;seed*=0x846ca68bu;seed^=seed>>16;
        return float(seed&0xffffffu)/float(0x1000000u);
    };
    // Particle size in Lua is a radius; cap lifetime/size to bound large or corrupt recordings.
    const float size=(50+30*random())*std::clamp(fire.key.scale,0.0f,8.0f);
    const float lifetime=std::min(2.0f,size*0.006f)*(blue?0.5f:1.0f);
    if(age>=lifetime) return result;
    const float progress=age/lifetime;
    const float drag=(1-std::exp(-0.8f*age))/0.8f;
    result.position={fire.key.origin[0]+(random()-0.5f)*size*0.5f+(random()-0.5f)*100*drag,
        fire.key.origin[1]+(random()-0.5f)*size*0.5f+(random()-0.5f)*100*drag,
        fire.key.origin[2]-10+size*(2.75f+0.75f*random())*drag};
    result.radius=((blue?size*0.75f:size)*(1-progress)+4*progress);
    result.opacity=(1-progress*0.61f)*std::min(1.0f,(1-progress)*6);
    const float brightness=(180+50*random())/255.0f;
    result.color=blue?std::array<float,3>{brightness*0.25f,brightness*0.25f,brightness}
        :std::array<float,3>{brightness,brightness,brightness};
    return result;
}
}
