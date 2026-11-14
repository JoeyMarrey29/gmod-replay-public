#pragma once
#include "demo/DemoData.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gmod::demo {
inline const TimedFloat* flash_at(const DemoPlayer& player, float time) {
    const auto next = std::upper_bound(player.flashed.begin(), player.flashed.end(), time,
        [](float t, const TimedFloat& key) { return t < key.time; });
    if (next == player.flashed.begin() || (next - 1)->value <= time) return nullptr;
    return &*(next - 1);
}

inline float flash_opacity(const DemoPlayer& player, float time) {
    const auto* key = flash_at(player, time);
    // PerpHeads cl_playerhealth.lua: WhiteScreen alpha = (FLASHED - CurTime()) / 20.
    return key ? std::clamp((key->value - time) / 20.0f, 0.0f, 1.0f) : 0.0f;
}

inline bool flashbang_effect(const DemoEffect& effect) {
    return effect.kind == "dispatch" && effect.name == "flashbang_smoke";
}

inline constexpr float FlashbangBurstLifetime = 0.1f;
inline constexpr float FlashbangSmokeLifetime = 3.0f;
inline constexpr const char* FlashbangSound = "weapons/flashbang/flashbang_explode2.mp3";

inline int flashbang_smoke_count(const DemoEffect& effect) {
    return std::isfinite(effect.scale) && effect.scale > 0
        ? 2 + static_cast<int>(15 * std::min(effect.scale,8.0f)) + static_cast<int>(20 * std::min(effect.scale,8.0f)) : 0;
}

inline std::string flashbang_smoke_material(const DemoEffect& effect, int particle) {
    return particle <= static_cast<int>(15 * std::min(effect.scale,8.0f)) ? "particle/smokestack"
        : "particle/smokesprites_000" + std::to_string(1 + particle % 9);
}

inline SmokeParticle flashbang_smoke_particle(const DemoEffect& effect, int particle, float time) {
    SmokeParticle puff;
    const float age = time - effect.time;
    if (!std::isfinite(age) || age < 0 || age >= FlashbangSmokeLifetime
        || particle < 0 || particle >= flashbang_smoke_count(effect)) return puff;
    const float scale = std::min(effect.scale,8.0f);
    const bool stack = particle <= static_cast<int>(15 * scale);
    std::uint32_t seed = std::uint32_t(effect.entity)*9781u + std::uint32_t(particle+1)*1597334677u;
    const auto random = [&]() {
        seed ^= seed >> 16; seed *= 0x7feb352du; seed ^= seed >> 15; seed *= 0x846ca68bu; seed ^= seed >> 16;
        return float(seed & 0xffffffu)/float(0x1000000u);
    };
    const float lifetime = 2 + random();
    if (age >= lifetime) return puff;
    const float progress = age/lifetime;
    const float drag = stack ? 4.0f : 3.0f;
    const float travel = (1-std::exp(-drag*age))/drag;
    const float gravity_travel = (age-travel)/drag;
    // Original flashbang_smoke Lua ranges/materials; analytic drag keeps pause/rewind deterministic.
    for (int axis=0;axis<3;++axis) {
        const float factor = axis == 2 ? (stack ? 0.8f : 1.3f) : (stack ? 1.0f : 0.5f);
        const float velocity = (random()*2-1)*100*scale*factor;
        const float gravity = axis == 2 ? random()*100 : (random()*2-1)*100*scale;
        puff.position[axis] = effect.origin[axis] + velocity*travel + gravity*gravity_travel;
    }
    puff.radius = (10 + 10*random()*progress)*scale;
    puff.opacity = ((stack ? 30 : 40) + 20*random())/255.0f*(1-progress);
    const float brightness = (100 + 40*random())/255.0f;
    puff.color = {brightness,brightness,brightness};
    return puff;
}
}
