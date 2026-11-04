#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace gmod::source {

struct ModelDependencyStatus {
    std::string model_path;
    std::filesystem::path mdl;
    std::filesystem::path vvd;
    std::filesystem::path vtx;
    bool has_mdl = false;
    bool has_vvd = false;
    bool has_vtx = false;
};

class AssetResolver {
public:
    explicit AssetResolver(std::vector<std::filesystem::path> roots);

    ModelDependencyStatus resolve_model(const std::string& model_path) const;

private:
    std::vector<std::filesystem::path> roots_;
};

} // namespace gmod::source
