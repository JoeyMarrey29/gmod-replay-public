#include "source/vmt/VmtReader.h"

#include "source/Vfs.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <stdexcept>
#include <vector>

namespace gmod::source::vmt {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

struct Token {
    std::string text;
    bool quoted = false;
};

// Splits KeyValues text into tokens: quoted strings, bare words and braces.
// Handles // comments and keeps quoted braces as plain strings.
std::vector<Token> tokenize(const std::string& text) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') {
                ++i;
            }
        } else if (c == '"') {
            const auto end = text.find('"', i + 1);
            const auto stop = end == std::string::npos ? text.size() : end;
            tokens.push_back({text.substr(i + 1, stop - i - 1), true});
            i = stop + 1;
        } else if (c == '{' || c == '}') {
            tokens.push_back({std::string(1, c), false});
            ++i;
        } else {
            const auto start = i;
            while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))
                && text[i] != '{' && text[i] != '}' && text[i] != '"') {
                ++i;
            }
            tokens.push_back({text.substr(start, i - start), false});
        }
    }
    return tokens;
}

struct Block {
    std::map<std::string, std::string> values;
    std::map<std::string, Block> children;
};

bool is_open(const Token& token) { return !token.quoted && token.text == "{"; }
bool is_close(const Token& token) { return !token.quoted && token.text == "}"; }

// Parses key/value pairs and nested blocks until the matching '}' or end.
Block parse_block(const std::vector<Token>& tokens, std::size_t& pos) {
    Block block;
    while (pos < tokens.size() && !is_close(tokens[pos])) {
        const auto key = lower(tokens[pos].text);
        ++pos;
        if (pos >= tokens.size()) {
            break;
        }
        if (is_open(tokens[pos])) {
            ++pos;
            block.children[key] = parse_block(tokens, pos);
            if (pos < tokens.size() && is_close(tokens[pos])) {
                ++pos;
            }
        } else if (!is_close(tokens[pos])) {
            block.values[key] = tokens[pos].text;
            ++pos;
        }
    }
    return block;
}

std::string value_of(const Block& block, const char* key) {
    const auto found = block.values.find(key);
    return found == block.values.end() ? "" : lower(found->second);
}

bool flag_of(const Block& block, const char* key) {
    const auto value = value_of(block, key);
    return value == "1" || value == "true";
}

// Patch "insert"/"replace" blocks override values from the included VMT.
void overlay(Block& target, const Block& source) {
    for (const auto& [key, value] : source.values) {
        target.values[key] = value;
    }
}

} // namespace

VmtReader::VmtReader(std::filesystem::path path)
    : path_(std::move(path)) {}

Material VmtReader::read() const {
    if (!vfs::exists(path_)) {
        throw std::runtime_error("Could not open VMT: " + path_.string());
    }
    const auto bytes = vfs::read(path_);
    const std::string text(bytes.begin(), bytes.end());
    const auto tokens = tokenize(text);
    Material material{};
    if (tokens.size() < 2) {
        return material;
    }
    material.shader = lower(tokens[0].text);
    std::size_t pos = 1;
    if (!is_open(tokens[pos])) {
        return material;
    }
    ++pos;
    auto body = parse_block(tokens, pos);
    if (material.shader == "patch") {
        material.include_path = value_of(body, "include");
        for (const char* section : {"insert", "replace"}) {
            const auto found = body.children.find(section);
            if (found != body.children.end()) {
                overlay(body, found->second);
            }
        }
    }
    material.base_texture = value_of(body, "$basetexture");
    material.model_material = value_of(body, "$modelmaterial");
    const auto decal_scale = value_of(body, "$decalscale");
    if (!decal_scale.empty()) material.decal_scale = std::stof(decal_scale);
    material.base_texture2 = value_of(body, "$basetexture2");
    material.blend_modulate_texture = value_of(body, "$blendmodulatetexture");
    material.normal_map = value_of(body, "$normalmap");
    material.dudv_map = value_of(body, "$dudvmap");
    material.water_above = value_of(body, "$abovewater") != "0";
    material.translucent = flag_of(body, "$translucent");
    material.alpha_test = flag_of(body, "$alphatest");
    const auto reference = value_of(body, "$alphatestreference");
    if (!reference.empty()) {
        try {
            material.alpha_test_reference = std::stof(reference);
        } catch (const std::exception&) {
            material.alpha_test_reference = 0.0f;
        }
    }
    material.additive = flag_of(body, "$additive");
    material.no_cull = flag_of(body, "$nocull");
    material.surface_prop = value_of(body, "$surfaceprop");
    material.blend_tint_by_base_alpha = flag_of(body, "$blendtintbybasealpha");
    return material;
}

} // namespace gmod::source::vmt
