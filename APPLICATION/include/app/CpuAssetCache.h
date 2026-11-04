#pragma once

#include "dx12/GpuAssetCache.h"
#include "source/bsp/BspReader.h"
#include "source/bsp/StaticPropsReader.h"
#include "source/model/StaticPropMeshImporter.h"
#include "source/WorldTrace.h"
#include "source/vmt/VmtReader.h"
#include "source/vtf/VtfReader.h"

#include <map>
#include <sstream>

namespace gmod::app {

// Loader-thread ownership; snapshots are taken before demo-specific bindings are applied.
struct CpuAssetCache {
    struct Map {
        source::bsp::BspReader bsp;
        source::bsp::BspSummary summary;
        source::bsp::StaticPropsReader props;
        source::bsp::StaticPropsSummary prop_summary;
    };
    std::map<std::string, Map> maps;
    std::map<std::string, source::model::StaticPropMeshResult> meshes;
    std::map<std::string, std::shared_ptr<source::WorldTrace>> model_traces;
    std::map<std::string, source::vmt::Material> materials;
    std::map<std::string, source::vtf::Texture> texture_headers;
    std::size_t map_hits = 0;
    std::size_t mesh_hits = 0;
    std::size_t material_hits = 0;
    std::size_t texture_header_hits = 0;

    void begin_load() { source_versions_.clear(); }

    bool sources_current() const {
        for (const auto& [path, version] : physical_versions_) {
            if (!std::filesystem::is_regular_file(path)
                || dx12::GpuAssetCache::texture_source_key(path, false) != version) return false;
        }
        return true;
    }

    std::string source_key(const std::filesystem::path& path) {
        const auto normalized = path.lexically_normal();
        const auto found = source_versions_.find(normalized);
        if (found != source_versions_.end()) return found->second;
        std::error_code error;
        std::string version;
        if (std::filesystem::is_regular_file(normalized, error)) {
            version = dx12::GpuAssetCache::texture_source_key(normalized, false);
            physical_versions_.insert_or_assign(normalized, version);
        } else {
            const auto parent = normalized.parent_path();
            if (parent.empty() || parent == normalized)
                throw std::runtime_error("Asset cache source is unavailable: " + path.string());
            version = normalized.string() + ":" + source_key(parent);
        }
        source_versions_.emplace(normalized, version);
        return version;
    }

    static std::string mesh_key(const std::string& model, const std::filesystem::path& mdl,
        const std::filesystem::path& vvd, const std::filesystem::path& vtx, int skin,
        const source::model::BodySelection& selection, CpuAssetCache* cache = nullptr) {
        std::ostringstream key;
        const auto text = [&](const std::string& value) { key << value.size() << ':' << value; };
        const auto bytes = [&](const auto& values) {
            key << values.size() << ':';
            if (!values.empty()) key.write(reinterpret_cast<const char*>(values.data()),
                                          values.size() * sizeof(values.front()));
        };
        text(model);
        for (const auto& file : {mdl, vvd, vtx})
            text(cache ? cache->source_key(file) : dx12::GpuAssetCache::texture_source_key(file, false));
        key << skin << ':' << selection.body << ':' << selection.apply_idle_pose << ':';
        key.write(reinterpret_cast<const char*>(&selection.normal_offset), sizeof(selection.normal_offset));
        key << selection.bodygroup_normal_offsets.size() << ':';
        for (const auto& [name, offset] : selection.bodygroup_normal_offsets) {
            text(name);
            key.write(reinterpret_cast<const char*>(&offset), sizeof(offset));
        }
        key << selection.overrides.size() << ':';
        for (const auto& [part, value] : selection.overrides) key << part << ':' << value << ':';
        key << selection.named_overrides.size() << ':';
        for (const auto& [name, value] : selection.named_overrides) { text(name); key << value << ':'; }
        text(selection.fallback_bone);
        key << (selection.skeleton != nullptr) << ':';
        if (selection.skeleton) {
            key << selection.skeleton->size() << ':';
            for (const auto& name : *selection.skeleton) text(name);
        }
        key << (selection.bone_matrices != nullptr) << ':';
        if (selection.bone_matrices) bytes(*selection.bone_matrices);
        const auto pairs = [&](const auto* entries) {
            key << (entries != nullptr) << ':';
            if (entries) {
                key << entries->size() << ':';
                for (const auto& [from, to] : *entries) { text(from); text(to); }
            }
        };
        pairs(selection.material_remap);
        pairs(selection.bone_aliases);
        const auto serialized = key.str();
        return dx12::GpuAssetCache::content_key(serialized.data(), serialized.size(), "mesh");
    }

private:
    std::map<std::filesystem::path, std::string> source_versions_;
    std::map<std::filesystem::path, std::string> physical_versions_;
};

} // namespace gmod::app
