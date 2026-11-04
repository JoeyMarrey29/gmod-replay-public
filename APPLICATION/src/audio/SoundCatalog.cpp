#include "audio/SoundCatalog.h"

#include "source/Vfs.h"
#include <algorithm>
#include <cctype>
#include <set>
#include <iostream>

namespace gmod::audio {
namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string stem_of(const std::string& key) {
    const auto slash = key.find_last_of('/');
    auto name = slash == std::string::npos ? key : key.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

std::string without_digits(std::string text) {
    while (!text.empty() && std::isdigit(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}

bool contains(const std::string& text, const char* part) {
    return text.find(part) != std::string::npos;
}

} // namespace

SoundCatalog::SoundCatalog(const std::vector<std::filesystem::path>& content_roots)
    : roots_(content_roots) {
    std::set<std::string> seen;
    const auto scan = [&](const std::filesystem::path& base, const std::string& folder) {
        for (const auto& file : source::vfs::list_files(base / std::filesystem::path(folder), false)) {
            const auto key = lower(folder + "/" + file);
            if (seen.insert(key).second) folders_[lower(folder)].push_back(key);
        }
    };
    for (const auto& root : content_roots) {
        const auto sound = root / "sound";
        scan(sound, "player/footsteps");
        for (const auto& folder : source::vfs::list_folders(sound / "weapons")) scan(sound, "weapons/" + folder);
    }
    for (auto& [folder, files] : folders_) std::sort(files.begin(), files.end());
}

bool SoundCatalog::wave_loops(const std::string& file) const {
    const auto cached = loop_cache_.find(file);
    if (cached != loop_cache_.end()) return cached->second;
    for (const auto& root : roots_) {
        const auto path = root / "sound" / std::filesystem::path(file);
        if (!source::vfs::exists(path)) continue;
        const auto looping = wav_has_loop(source::vfs::read(path));
        if (!looping) std::clog << "SOUND_LOOP_METADATA_INVALID " << path.string() << "\n";
        return loop_cache_.emplace(file, looping.value_or(false)).first->second;
    }
    std::clog << "SOUND_LOOP_METADATA_MISSING " << file << "\n";
    return loop_cache_.emplace(file, false).first->second;
}

std::vector<std::string> SoundCatalog::fire_sounds(const std::string& sound_name) const {
    std::string name = lower(sound_name);
    if (name.ends_with("_shoot")) name.resize(name.size() - 6);
    if (name.empty()) return {};

    // Folders to try, best match first: the exact name, the "rem"-prefixed form (870 is
    // rem870), then folders sharing a prefix with it (g36 / g36c, ak74u / ak74).
    std::vector<std::string> candidates{name, "rem" + name};
    std::vector<std::pair<std::size_t, std::string>> partial;
    for (const auto& [folder, files] : folders_) {
        if (!folder.starts_with("weapons/")) continue;
        const auto leaf = folder.substr(8);
        if (leaf == name || leaf == "rem" + name) continue;
        if (leaf.starts_with(name) || name.starts_with(leaf)) {
            partial.emplace_back(std::min(leaf.size(), name.size()), leaf);
        }
    }
    std::sort(partial.begin(), partial.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [length, leaf] : partial) candidates.push_back(leaf);
    candidates.push_back("revolvers");

    for (const auto& leaf : candidates) {
        const auto folder = folders_.find("weapons/" + leaf);
        if (folder == folders_.end()) continue;
        std::vector<std::string> fire;
        std::vector<std::string> named;
        std::vector<std::string> loose;  // the folder's only file that is not a reload/handling sound
        for (const auto& key : folder->second) {
            const auto stem = stem_of(key);
            if (contains(stem, "silenc")) continue;
            if (contains(stem, "fire")) {
                fire.push_back(key);
            } else if (stem == leaf || stem == name || without_digits(stem) == leaf
                       || without_digits(stem) == name) {
                // Some folders name the shot after the weapon alone (usp.mp3, l115a3.mp3, 357.mp3).
                named.push_back(key);
            } else {
                bool handling = false;
                for (const char* part : {"mag", "slide", "bolt", "cock", "charge", "load", "pump", "insert",
                                         "deploy", "safety", "cyl", "casing", "barrel", "release", "catch",
                                         "check", "stock", "selector", "forward", "back", "draw", "start",
                                         "end", "dust", "butt", "unlock", "attach", "detach"}) {
                    if (contains(stem, part)) {
                        handling = true;
                        break;
                    }
                }
                if (!handling) loose.push_back(key);
            }
        }
        if (!fire.empty()) return fire;
        if (!named.empty()) return named;
        if (loose.size() == 1) return loose;
    }
    return {};
}

std::vector<std::string> SoundCatalog::footsteps(const std::string& step_set) const {
    const auto folder = folders_.find("player/footsteps");
    if (folder == folders_.end()) return {};
    // Step sound names that share another set's files.
    static const std::map<std::string, std::string> shared{
        {"default", "concrete"},   {"solidmetal", "metal"},   {"metal_box", "metal"},
        {"metalvent", "duct"},     {"wood_panel", "woodpanel"}, {"wood_box", "woodpanel"},
        {"wood_crate", "woodpanel"}, {"water", "wade"},       {"slipperyslime", "slosh"}};
    std::string set = lower(step_set);
    const auto alias = shared.find(set);
    if (alias != shared.end()) set = alias->second;
    for (const auto& wanted : {set, std::string("concrete")}) {
        std::vector<std::string> files;
        for (const auto& key : folder->second) {
            if (without_digits(stem_of(key)) == wanted) files.push_back(key);
        }
        if (!files.empty()) return files;
    }
    return {};
}

} // namespace gmod::audio
