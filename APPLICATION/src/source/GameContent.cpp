#include "source/GameContent.h"

#include "source/Vfs.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace gmod::source::content {
namespace {

std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), ::towlower);
    return text;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr,
                        nullptr);
    return result;
}

std::wstring from_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

bool is_folder(const std::filesystem::path& path) {
    std::error_code error;
    return !path.empty() && std::filesystem::is_directory(path, error);
}

bool is_file(const std::filesystem::path& path) {
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(path, error);
}

// One Workshop/legacy addon: its title and the maps it carries.
struct Addon {
    std::filesystem::path path;
    std::uint64_t size = 0;
    std::int64_t stamp = 0;
    std::string title;
    std::vector<std::string> maps;  // "rp_paralake_city_v6" for maps/rp_paralake_city_v6.bsp
};

std::int64_t file_stamp(const std::filesystem::path& path) {
    std::error_code error;
    const auto written = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<std::int64_t>(written.time_since_epoch().count());
}

// Reads a .gma's title and file table (not the file data).
bool read_addon(const std::filesystem::path& path, Addon& addon) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    const auto read_string = [&]() {
        std::string text;
        for (char c; file.get(c) && c != '\0';) text.push_back(c);
        return text;
    };
    char magic[4]{};
    file.read(magic, 4);
    if (std::memcmp(magic, "GMAD", 4) != 0) return false;
    const int version = file.get();
    file.ignore(16);  // steam id, timestamp
    if (version > 1) {
        while (!read_string().empty() && file) {}
    }
    addon.title = read_string();
    read_string();  // description
    read_string();  // author
    file.ignore(4);  // addon version
    for (;;) {
        std::uint32_t number = 0;
        file.read(reinterpret_cast<char*>(&number), 4);
        if (!file || number == 0) break;
        const auto name = lower(read_string());
        file.ignore(12);  // size, crc
        if (name.starts_with("maps/") && name.ends_with(".bsp") && name.find('/', 5) == std::string::npos) {
            addon.maps.push_back(name.substr(5, name.size() - 9));
        }
    }
    if (!file) return false;
    std::error_code error;
    addon.size = std::filesystem::file_size(path, error);
    addon.stamp = file_stamp(path);
    return true;
}

std::filesystem::path index_path(const std::filesystem::path& application_root) {
    return application_root / L"build" / L"addon_index.txt";
}

// Workshop items (steamapps\workshop\content\4000\<id>\*.gma) and legacy garrysmod\addons\*.gma.
std::vector<std::filesystem::path> addon_files(const std::filesystem::path& gmod) {
    std::vector<std::filesystem::path> files;
    std::error_code error;
    const auto workshop = gmod.parent_path().parent_path() / L"workshop" / L"content" / L"4000";
    for (std::filesystem::directory_iterator item(workshop, error), end; !error && item != end; item.increment(error)) {
        if (!item->is_directory(error)) continue;
        std::error_code inner;
        for (std::filesystem::directory_iterator it(item->path(), inner), stop; !inner && it != stop;
             it.increment(inner)) {
            if (_wcsicmp(it->path().extension().c_str(), L".gma") == 0) files.push_back(it->path());
        }
    }
    for (std::filesystem::directory_iterator it(gmod / L"garrysmod" / L"addons", error), end; !error && it != end;
         it.increment(error)) {
        if (_wcsicmp(it->path().extension().c_str(), L".gma") == 0) files.push_back(it->path());
    }
    return files;
}

std::vector<Addon> scan_addons(const std::filesystem::path& gmod) {
    std::vector<Addon> addons;
    for (const auto& path : addon_files(gmod)) {
        Addon addon;
        addon.path = path;
        if (read_addon(path, addon)) addons.push_back(std::move(addon));
    }
    return addons;
}

void save_index(const std::filesystem::path& application_root, const std::filesystem::path& gmod,
                const std::vector<Addon>& addons) {
    std::error_code error;
    std::filesystem::create_directories(index_path(application_root).parent_path(), error);
    std::ofstream out(index_path(application_root), std::ios::binary | std::ios::trunc);
    out << "gmod\t" << to_utf8(gmod.wstring()) << "\n";
    for (const auto& addon : addons) {
        out << "addon2\t" << to_utf8(addon.path.wstring()) << "\t" << addon.size << "\t" << addon.stamp << "\t"
            << addon.title << "\t";
        for (std::size_t i = 0; i < addon.maps.size(); ++i) out << (i ? ";" : "") << addon.maps[i];
        out << "\n";
    }
}

// Reads the saved index; entries keep the size and time stamp their header was read at.
bool load_index(const std::filesystem::path& application_root, const std::filesystem::path& gmod,
                std::vector<Addon>& addons) {
    std::ifstream in(index_path(application_root), std::ios::binary);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line) || line != "gmod\t" + to_utf8(gmod.wstring())) return false;
    while (std::getline(in, line)) {
        std::vector<std::string> fields;
        std::stringstream stream(line);
        for (std::string field; std::getline(stream, field, '\t');) fields.push_back(field);
        if (fields.size() < 5 || fields[0] != "addon2") continue;
        Addon addon;
        addon.path = from_utf8(fields[1]);
        addon.size = std::strtoull(fields[2].c_str(), nullptr, 10);
        addon.stamp = std::strtoll(fields[3].c_str(), nullptr, 10);
        addon.title = fields[4];
        if (fields.size() > 5) {
            std::stringstream maps(fields[5]);
            for (std::string map; std::getline(maps, map, ';');) {
                if (!map.empty()) addon.maps.push_back(map);
            }
        }
        addons.push_back(std::move(addon));
    }
    return true;
}

// Brings the saved index up to date with the addons on disk: only added or changed .gma files
// (by size and write time) have their header read. Returns true when anything changed.
bool update_index(const std::filesystem::path& application_root, const std::filesystem::path& gmod,
                  std::vector<Addon>& addons) {
    std::vector<Addon> saved;
    const bool had_index = load_index(application_root, gmod, saved);
    std::map<std::wstring, Addon*> by_path;
    for (auto& addon : saved) by_path[lower(addon.path.wstring())] = &addon;
    bool changed = !had_index;
    std::size_t reused = 0;
    addons.clear();
    for (const auto& path : addon_files(gmod)) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        const auto stamp = file_stamp(path);
        const auto found = by_path.find(lower(path.wstring()));
        if (!error && found != by_path.end() && found->second->size == size && found->second->stamp == stamp) {
            addons.push_back(std::move(*found->second));
            ++reused;
            continue;
        }
        changed = true;
        Addon addon;
        addon.path = path;
        if (read_addon(path, addon)) addons.push_back(std::move(addon));
    }
    if (reused != saved.size()) changed = true;  // some addon was removed
    if (changed) save_index(application_root, gmod, addons);
    return changed;
}

bool is_perp_addon(const Addon& addon) {
    return lower(addon.title).starts_with("perpheads");
}

std::filesystem::path pick_install(const std::filesystem::path& hint) {
    if (!hint.empty()) {
        const auto containing = install_containing(hint);
        if (!containing.empty()) return containing;
    }
    const auto installs = find_gmod_installs();
    return installs.empty() ? std::filesystem::path() : installs.front();
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, 4);
    return value;
}

} // namespace

std::vector<std::filesystem::path> steam_libraries() {
    std::vector<std::filesystem::path> roots;
    const auto read = [&](HKEY hive, const wchar_t* key, const wchar_t* value) {
        wchar_t buffer[1024]{};
        DWORD size = sizeof(buffer);
        if (RegGetValueW(hive, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS) {
            roots.emplace_back(buffer);
        }
    };
    read(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    read(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    read(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");

    std::vector<std::filesystem::path> libraries;
    std::set<std::wstring> seen;
    const auto add = [&](const std::filesystem::path& path) {
        if (path.empty()) return;
        const auto key = lower(path.lexically_normal().wstring());
        if (seen.insert(key).second) libraries.push_back(path.lexically_normal());
    };
    for (const auto& root : roots) {
        add(root);
        // "path"  "D:\\SteamLibrary"
        std::ifstream vdf(root / L"steamapps" / L"libraryfolders.vdf");
        static const std::regex path_line(R"vdf("path"\s*"([^"]+)")vdf");
        for (std::string line; std::getline(vdf, line);) {
            std::smatch match;
            if (!std::regex_search(line, match, path_line)) continue;
            std::string value = match[1].str();
            std::string unescaped;
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\') ++i;
                unescaped.push_back(value[i]);
            }
            add(std::filesystem::path(from_utf8(unescaped)));
        }
    }
    const DWORD drives = GetLogicalDrives();
    for (int letter = 0; letter < 26; ++letter) {
        if (!(drives & (1u << letter))) continue;
        const std::wstring drive = std::wstring(1, static_cast<wchar_t>(L'A' + letter)) + L":\\";
        if (GetDriveTypeW(drive.c_str()) != DRIVE_FIXED) continue;
        for (const wchar_t* folder : {L"Program Files (x86)\\Steam", L"Program Files\\Steam", L"Steam",
                                      L"SteamLibrary", L"Games\\Steam", L"Games\\SteamLibrary"}) {
            add(std::filesystem::path(drive) / folder);
        }
    }
    return libraries;
}

std::vector<std::filesystem::path> find_gmod_installs() {
    std::vector<std::filesystem::path> installs;
    std::vector<std::filesystem::path> unconfirmed;
    for (const auto& library : steam_libraries()) {
        const auto gmod = library / L"steamapps" / L"common" / L"GarrysMod";
        if (!is_folder(gmod / L"garrysmod")) continue;
        // appmanifest_4000.acf marks the library Steam actually installed Garry's Mod into.
        if (is_file(library / L"steamapps" / L"appmanifest_4000.acf")) {
            installs.push_back(gmod);
        } else {
            unconfirmed.push_back(gmod);
        }
    }
    installs.insert(installs.end(), unconfirmed.begin(), unconfirmed.end());
    return installs;
}

std::filesystem::path install_containing(const std::filesystem::path& path) {
    std::error_code error;
    auto at = std::filesystem::absolute(path, error);
    while (!at.empty()) {
        if (is_file(at / L"garrysmod" / L"gameinfo.txt")) return at;
        const auto parent = at.parent_path();
        if (parent == at) break;
        at = parent;
    }
    return {};
}

ContentSet mount_content(const std::filesystem::path& application_root, const std::string& map_name,
                         const std::filesystem::path& hint) {
    ContentSet set;
    const auto map = lower(map_name.empty() ? std::string("rp_paralake_city_v6") : map_name);
    const auto gmod = pick_install(hint);
    if (!gmod.empty()) {
        set.gmod = gmod;
        set.log.push_back("GMOD " + to_utf8(gmod.wstring()));
        std::vector<Addon> addons;
        const auto index_started = std::chrono::steady_clock::now();
        const bool index_changed = update_index(application_root, gmod, addons);
        set.log.push_back(std::string(index_changed ? "ADDON_INDEX updated (" : "ADDON_INDEX unchanged (")
                          + std::to_string(addons.size()) + " addons, "
                          + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now() - index_started).count())
                          + " ms)");
        std::vector<const Addon*> perp;
        const Addon* map_addon = nullptr;
        for (const auto& addon : addons) {
            if (is_perp_addon(addon)) perp.push_back(&addon);
            if (std::find(addon.maps.begin(), addon.maps.end(), map) != addon.maps.end()
                && (!map_addon || file_stamp(addon.path) > file_stamp(map_addon->path))) {
                map_addon = &addon;
            }
        }
        std::sort(perp.begin(), perp.end(), [](const Addon* a, const Addon* b) { return lower(a->title) < lower(b->title); });
        const auto mount_root = [&](const std::filesystem::path& archive) {
            if (!is_file(archive)) return;
            if (vfs::mount(archive)) {
                set.roots.push_back(archive);
                set.log.push_back("MOUNT " + to_utf8(archive.wstring()));
            } else {
                set.log.push_back("MOUNT_FAILED " + to_utf8(archive.wstring()));
            }
        };
        const auto loose_root = [&](const std::filesystem::path& folder) {
            if (!is_folder(folder)) return;
            set.roots.push_back(folder);
            set.log.push_back("FOLDER " + to_utf8(folder.wstring()));
        };
        for (const auto* addon : perp) mount_root(addon->path);
        std::size_t map_root = set.roots.size();
        if (map_addon && std::find(perp.begin(), perp.end(), map_addon) == perp.end()) mount_root(map_addon->path);
        const std::size_t pak_slot = set.roots.size();
        mount_root(gmod / L"garrysmod" / L"garrysmod_dir.vpk");
        for (const wchar_t* name : {L"hl2_textures_dir.vpk", L"hl2_sound_vo_english_dir.vpk",
                                    L"hl2_sound_misc_dir.vpk", L"hl2_misc_dir.vpk"}) {
            mount_root(gmod / L"sourceengine" / name);
        }
        mount_root(gmod / L"platform" / L"platform_misc_dir.vpk");
        loose_root(gmod / L"garrysmod");
        loose_root(gmod / L"sourceengine");
        loose_root(gmod / L"garrysmod" / L"download");
        // Mounted game depots (mountdepots.txt), then the fallbacks GMod ships for missing games.
        mount_root(gmod / L"sourceengine" / L"content_hl2_dir.vpk");
        mount_root(gmod / L"sourceengine" / L"content_cstrike_dir.vpk");
        mount_root(gmod / L"garrysmod" / L"fallbacks_dir.vpk");

        const auto relative = std::filesystem::path(L"maps") / (from_utf8(map) + L".bsp");
        for (std::size_t i = map_root; i < set.roots.size() && set.bsp.empty(); ++i) {
            if (vfs::exists(set.roots[i] / relative)) set.bsp = set.roots[i] / relative;
        }
        for (std::size_t i = 0; i < map_root && set.bsp.empty(); ++i) {
            if (vfs::exists(set.roots[i] / relative)) set.bsp = set.roots[i] / relative;
        }
        if (!set.bsp.empty()) {
            set.log.push_back("MAP " + to_utf8(set.bsp.wstring()));
            // The pakfile lump is a zip of the map's own materials, models and sounds.
            try {
                const auto header = vfs::read_range(set.bsp, 0, 8 + 64 * 16);
                const std::size_t at = 8 + 40 * 16;
                const auto offset = read_u32(header, at);
                const auto length = read_u32(header, at + 4);
                const auto packed_size = read_u32(header, at + 12);
                std::filesystem::path pak = set.bsp;
                pak += L".pak";
                if (packed_size != 0) {
                    set.log.push_back("PAKFILE_COMPRESSED (not mounted)");
                } else if (length > 0 && vfs::mount_zip(pak, set.bsp, offset, length)) {
                    set.roots.insert(set.roots.begin() + static_cast<std::ptrdiff_t>(pak_slot), pak);
                    set.log.push_back("PAKFILE " + std::to_string(length) + " bytes");
                } else {
                    set.log.push_back("PAKFILE_FAILED");
                }
            } catch (const std::exception& error) {
                set.log.push_back(std::string("PAKFILE_FAILED ") + error.what());
            }
        } else {
            set.log.push_back("MAP_NOT_FOUND " + map);
        }
        if (perp.empty()) set.log.push_back("NO_PERP_ADDONS");
    } else {
        set.log.push_back("GMOD_NOT_FOUND");
    }

    // Without the game (or its copy of the map), use the app's bundled content if it has any.
    if (set.bsp.empty()) {
        const auto extracted = application_root / L"assets" / L"extracted";
        std::vector<std::filesystem::path> bundled;
        for (const wchar_t* name : {L"Perpheads_Content_1", L"Perpheads_Content_2", L"Perpheads_Content_3",
                                    L"rp_paralake_city_v6", L"rp_paralake_city_v6_map_gma",
                                    L"rp_paralake_city_v6_bsp_pak", L"steam_source"}) {
            if (is_folder(extracted / name)) bundled.push_back(extracted / name);
        }
        const auto bundled_bsp = application_root / L"assets" / L"raw" / L"maps" / (from_utf8(map) + L".bsp");
        if (!bundled.empty() && is_file(bundled_bsp)) {
            set.roots = std::move(bundled);
            set.bsp = bundled_bsp;
            set.bundled = true;
            set.log.push_back("BUNDLED_ASSETS");
        }
    }
    return set;
}

std::wstring refresh_addon_index(const std::filesystem::path& application_root, const std::filesystem::path& hint) {
    const auto gmod = pick_install(hint);
    if (gmod.empty()) return L"Garry's Mod was not found on this computer.";
    const auto addons = scan_addons(gmod);
    save_index(application_root, gmod, addons);
    std::size_t perp = 0;
    std::size_t maps = 0;
    for (const auto& addon : addons) {
        if (is_perp_addon(addon)) ++perp;
        maps += addon.maps.size();
    }
    return L"Found " + std::to_wstring(perp) + L" PERP addon" + (perp == 1 ? L"" : L"s") + L" and "
         + std::to_wstring(maps) + L" map" + (maps == 1 ? L"" : L"s") + L" in Garry's Mod.";
}

} // namespace gmod::source::content
