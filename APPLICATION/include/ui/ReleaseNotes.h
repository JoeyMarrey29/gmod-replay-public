#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace gmod::ui {

struct ReleaseNoteBlock {
    enum class Kind { Heading, Paragraph, Bullet };
    Kind kind = Kind::Paragraph;
    std::string text;
};

struct ReleaseNote {
    std::string title;
    std::vector<ReleaseNoteBlock> blocks;
};

ReleaseNote parse_release_note(std::string_view markdown);

} // namespace gmod::ui
