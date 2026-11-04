#pragma once

#include "source/vtf/VtfReader.h"

#include <filesystem>

namespace gmod::source::png {

class PngReader {
public:
    explicit PngReader(std::filesystem::path path);
    vtf::Pixels read() const;

private:
    std::filesystem::path path_;
};

} // namespace gmod::source::png
