#include "source/AssetResolver.h"

#include "source/Vfs.h"

#include <algorithm>
#include <cctype>

namespace gmod::source {
namespace {

std::string normalize(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    while (!value.empty() && value.front() == '/') {
        value.erase(value.begin());
    }
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::filesystem::path with_extension(
    const std::filesystem::path& path, const char* extension) {
    auto result = path;
    result.replace_extension(extension);
    return result;
}

} // namespace

AssetResolver::AssetResolver(std::vector<std::filesystem::path> roots)
    : roots_(std::move(roots)) {}

ModelDependencyStatus AssetResolver::resolve_model(
    const std::string& model_path) const {
    const auto normalized = normalize(model_path);
    auto relative = std::filesystem::path(normalized);
    if (relative.extension() != ".mdl") {
        relative += ".mdl";
    }

    ModelDependencyStatus status{};
    status.model_path = normalized;
    for (const auto& root : roots_) {
        const auto mdl = root / relative;
        if (!vfs::exists(mdl)) {
            continue;
        }
        status.mdl = mdl;
        status.vvd = with_extension(mdl, ".vvd");
        status.vtx = with_extension(mdl, ".dx90.vtx");
        status.has_mdl = vfs::exists(status.mdl);
        status.has_vvd = vfs::exists(status.vvd);
        status.has_vtx = vfs::exists(status.vtx);
        return status;
    }
    return status;
}

} // namespace gmod::source
