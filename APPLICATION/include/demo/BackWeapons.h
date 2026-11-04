#pragma once
#include "demo/DemoData.h"
#include "source/model/Animation.h"
#include <algorithm>
#include <cmath>

namespace gmod::demo {
inline int back_gun_parent_at(const DemoProp& gun,float time) {
    if(!gun.back_gun || timed_int_at(gun.hidden,time,0)!=0) return -1;
    const auto next=std::upper_bound(gun.keys.begin(),gun.keys.end(),time,
        [](float t,const PropKey& key){return t<key.time;});
    if(next==gun.keys.begin() || !(next-1)->present) return -1;
    return (next-1)->parent;
}

inline int back_gun_player_at(const DemoData& demo,const DemoProp& gun,float time,int hidden_player=-1) {
    const int parent=back_gun_parent_at(gun,time);
    if(parent<0) return -1;
    const int player=player_index_at(demo,parent,time);
    if(player<0 || player==hidden_player || player_incapacitated(demo.players[player],time)) return -1;
    const int weapon=timed_int_at(gun.back_weapon,time,-1);
    if(weapon>=0 && held_weapon_at(demo,demo.players[player],time).entity==weapon) return -1;
    return player;
}

// prop_back_gun/cl_init.lua: use the animated chest, offset along its axes,
// then add (0,5,90) to the chest's world Euler angles (not a local rotation).
inline source::model::Matrix3x4 back_gun_transform(const source::model::Matrix3x4& chest) {
    constexpr float Degrees=57.29577951308232f;
    const float horizontal=std::hypot(chest[0],chest[4]);
    const float angles[3]{std::atan2(-chest[8],horizontal)*Degrees,
        (horizontal>0.0001f?std::atan2(chest[4],chest[0]):std::atan2(-chest[1],chest[5]))*Degrees+5,
        (horizontal>0.0001f?std::atan2(chest[9],chest[10]):0)*Degrees+90};
    const float origin[3]{chest[3]-8*chest[0]+4*chest[1]-3*chest[2],
        chest[7]-8*chest[4]+4*chest[5]-3*chest[6],
        chest[11]-8*chest[8]+4*chest[9]-3*chest[10]};
    return source::model::matrix_from_source_angles(origin,angles);
}
}
