#include "demo/Perpheads.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>

namespace gmod::demo {
namespace {

std::string clothing_model_path(const ClothingType& type, bool female, const std::string& model) {
    std::string path;
    if (type.accessory) {
        path = "models/perpheads/shared/accessories/";
    } else {
        path = type.government ? "models/perpheads/government/" : "models/perpheads/civilian/";
        path += female ? "female/" : "male/";
    }
    path += model;
    std::transform(path.begin(), path.end(), path.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (path.ends_with(".mdl")) path.resize(path.size() - 4);
    return path;
}

} // namespace

std::vector<OutfitPart> compose_outfit(
    const PerpheadsRegistry& registry, const ClothingRecord& record) {
    std::vector<OutfitPart> parts;
    std::set<int> missing(registry.required_slots.begin(), registry.required_slots.end());
    std::vector<const ClothingType*> worn;
    for (const auto& item : record.items) {
        const auto found = registry.clothing.find(item.type);
        if (found == registry.clothing.end()) continue;
        worn.push_back(&found->second);
        for (const int slot : found->second.slots) missing.erase(slot);
    }

    for (const auto& item : record.items) {
        const auto found = registry.clothing.find(item.type);
        if (found == registry.clothing.end()) continue;
        const auto& type = found->second;
        std::string model = type.model;
        for (const auto& variant : type.variants) {
            if (variant.missing_feet == (missing.count(6) != 0)
                && variant.missing_thighs == (missing.count(4) != 0)) {
                model = variant.model;
                break;
            }
        }
        OutfitPart part;
        part.model = item.model_path.empty() ? clothing_model_path(type, record.female, model) : item.model_path;
        if (part.model.ends_with(".mdl")) part.model.resize(part.model.size() - 4);
        if (record.explicit_parts) part.skin = item.model_skin;
        part.type = type.index;
        part.item_skin = item.skin;
        if (item.skin >= 1 && static_cast<std::size_t>(item.skin) <= type.skins.size()) {
            const auto& skin = type.skins[static_cast<std::size_t>(item.skin) - 1];
            part.sub_materials = skin.sub_materials;
            part.tint = skin.can_tint;
        }
        std::copy(item.color, item.color + 4, part.color);
        part.number_bodygroup = type.data_number_bodygroup;
        if (!type.data_number_bodygroup.empty() && (item.data_number < 0 || item.data_number > 9999))
            throw std::runtime_error("Invalid recorded uniform badge number: " + std::to_string(item.data_number));
        if (!type.data_number_bodygroup.empty() && !type.data_number_material_base.empty()
            && item.data_number > 0 && item.data_number <= 9999) {
            part.data_number = item.data_number;
            int divisor = 1000;
            for (int digit = 1; digit <= 4; ++digit, divisor /= 10)
                part.sub_materials.emplace_back(type.data_number_material_base + std::to_string(digit),
                    type.data_number_material_base + std::to_string(item.data_number / divisor % 10));
        }
        parts.push_back(std::move(part));
    }

    // A costume (model override) replaces the body: only the worn items are attached.
    if (record.explicit_parts || !record.model_override.empty()) return parts;

    // Bare body parts fill every required slot that no worn item covers.
    std::set<int> left(registry.required_slots.begin(), registry.required_slots.end());
    std::set<std::string> excluded;
    for (const auto* type : worn) {
        for (const int slot : type->slots) {
            for (const auto& name : type->excluded_body_parts) excluded.insert(name);
            if (!type->attach_body_parts_for_slots) left.erase(slot);
        }
        for (const int slot : type->optional_slots) left.erase(slot);
    }
    std::vector<const ClothingType*> bodies;
    for (const auto& [index, type] : registry.clothing) {
        if (type.body_part) bodies.push_back(&type);
    }
    std::sort(bodies.begin(), bodies.end(), [](const ClothingType* a, const ClothingType* b) {
        if (a->slots.size() != b->slots.size()) return a->slots.size() > b->slots.size();
        return a->index < b->index;
    });
    for (const auto* type : bodies) {
        if (excluded.count(type->identifier) != 0) continue;
        const bool usable = std::all_of(type->slots.begin(), type->slots.end(),
                                        [&](int slot) { return left.count(slot) != 0; });
        if (!usable) continue;
        for (const int slot : type->slots) left.erase(slot);
        OutfitPart part;
        part.model = clothing_model_path(*type, record.female, type->model);
        part.skin = record.model;
        part.type = type->index;
        parts.push_back(std::move(part));
    }
    return parts;
}

} // namespace gmod::demo
