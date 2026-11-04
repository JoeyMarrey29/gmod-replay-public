#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Finds Garry's Mod and mounts the content a demo needs straight from the game's own files:
// the PERP Workshop addons, the map's addon (and the pakfile inside its BSP), then the base
// game archives in the order gameinfo.txt gives. Nothing is copied or extracted.
namespace gmod::source::content {

// Every Steam library on this computer (registry, libraryfolders.vdf, usual folders).
std::vector<std::filesystem::path> steam_libraries();
// Garry's Mod installs (the folder that holds garrysmod\ and sourceengine\), the library Steam
// installed it into first.
std::vector<std::filesystem::path> find_gmod_installs();
// The install a file or folder sits inside (e.g. a demo in garrysmod\data\phdemos), or empty.
std::filesystem::path install_containing(const std::filesystem::path& path);

struct ContentSet {
    std::filesystem::path gmod;               // empty when Garry's Mod wasn't found
    std::vector<std::filesystem::path> roots;  // searched in this order
    std::filesystem::path bsp;                 // the map file (may be inside an archive)
    bool bundled = false;                      // fell back to the app's own assets folder
    std::vector<std::string> log;              // what was mounted, for build\content.log
};

// Mounts what `map_name` needs. `hint` (e.g. the demo's path) picks the install when there are
// several. Bundled assets are a fallback only if their BSP has the requested name.
ContentSet mount_content(const std::filesystem::path& application_root, const std::string& map_name,
                         const std::filesystem::path& hint);

// Rescans the Workshop folders and rewrites the addon index. Returns a one-line summary.
std::wstring refresh_addon_index(const std::filesystem::path& application_root,
                                 const std::filesystem::path& hint);

} // namespace gmod::source::content
