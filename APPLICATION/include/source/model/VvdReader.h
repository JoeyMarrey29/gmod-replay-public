#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace gmod::source::model {

struct ModelVertex {
    float position[3]{};
    float normal[3]{};
    float uv[2]{};
    // Skinning: up to three bones in this model's bone order.
    float bone_weight[3]{};
    std::uint8_t bone_id[3]{};
    std::uint8_t bone_count = 0;
};

struct VvdSummary {
    int version = 0;
    int checksum = 0;
    int lod0_vertex_count = 0;
    std::vector<ModelVertex> vertices;
};

class VvdReader {
public:
    explicit VvdReader(std::filesystem::path path);
    VvdSummary read() const;

private:
    std::filesystem::path path_;
};

} // namespace gmod::source::model
