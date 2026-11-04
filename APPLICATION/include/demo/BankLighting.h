#pragma once
#include "demo/DemoData.h"
#include <array>
namespace gmod::demo {
inline std::array<float,4> bank_lighting_at(const DemoData& demo,float time,
    const std::array<int,4>& styles,std::array<float,4> fallback) {
    for(const auto& key:demo.bank_lights) {
        if(key.time>time) break;
        for(int slot=0;slot<4;++slot) if(styles[slot]==key.style) fallback[slot]=key.value;
    }
    return fallback;
}
}
