#include "ui/ReleaseNotes.h"

#include <sstream>
#include <stdexcept>

namespace gmod::ui {
namespace {

std::string plain_text(std::string text) {
    for (const auto marker : {"**", "`"}) {
        for (auto pos = text.find(marker); pos != std::string::npos; pos = text.find(marker, pos)) {
            text.erase(pos, std::char_traits<char>::length(marker));
        }
    }
    return text;
}

} // namespace

ReleaseNote parse_release_note(std::string_view markdown) {
    ReleaseNote result;
    std::istringstream input{std::string(markdown)};
    bool continuation = false;
    for (std::string line; std::getline(input, line);) {
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) {
            continuation = false;
            continue;
        }
        line = line.substr(start);
        line.erase(line.find_last_not_of(" \t\r") + 1);
        if (line.starts_with("# ")) {
            if (!result.title.empty()) throw std::runtime_error("Release notes contain multiple titles.");
            result.title = plain_text(line.substr(2));
            continuation = false;
        } else if (line.starts_with("## ")) {
            result.blocks.push_back({ReleaseNoteBlock::Kind::Heading, plain_text(line.substr(3))});
            continuation = false;
        } else if (line.starts_with("- ")) {
            result.blocks.push_back({ReleaseNoteBlock::Kind::Bullet, plain_text(line.substr(2))});
            continuation = true;
        } else if (continuation) {
            result.blocks.back().text += " " + plain_text(line);
        } else {
            result.blocks.push_back({ReleaseNoteBlock::Kind::Paragraph, plain_text(line)});
            continuation = true;
        }
    }
    if (result.title.empty() || result.blocks.empty()) {
        throw std::runtime_error("Release notes need a title and body.");
    }
    return result;
}

} // namespace gmod::ui
