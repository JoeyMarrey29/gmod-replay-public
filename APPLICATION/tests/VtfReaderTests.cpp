#include "source/vtf/VtfReader.h"
#include "source/vmt/VmtReader.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
void put(std::vector<std::uint8_t>& bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

std::vector<std::uint8_t> header(int format, int mips = 1, int frames = 1) {
    std::vector<std::uint8_t> bytes(80);
    std::memcpy(bytes.data(), "VTF\0", 4);
    put<std::int32_t>(bytes, 4, 7);
    put<std::int32_t>(bytes, 8, 2);
    put<std::int32_t>(bytes, 12, 80);
    put<std::uint16_t>(bytes, 16, 2);
    put<std::uint16_t>(bytes, 18, 2);
    put<std::uint16_t>(bytes, 24, static_cast<std::uint16_t>(frames));
    put<std::int32_t>(bytes, 52, format);
    bytes[56] = static_cast<std::uint8_t>(mips);
    put<std::int32_t>(bytes, 57, -1);
    return bytes;
}

struct Fixture {
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("gmod-replay-vtf-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".vtf");
    ~Fixture() { std::filesystem::remove(path); }

    void write(const std::vector<std::uint8_t>& bytes) const {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        require(static_cast<bool>(file), "Could not write VTF fixture");
    }
};

} // namespace

int main() {
    try {
        Fixture fixture;
        {
            Fixture water;
            water.path.replace_extension(L".vmt");
            {
                std::ofstream file(water.path);
                file << R"("Water" { "$basetexture" "nature/foam" "$normalmap" "liquids/water_normal" "$abovewater" "1" })";
            }
            const auto surface = gmod::source::vmt::VmtReader(water.path).read();
            require(surface.shader == "water" && surface.water_above
                && surface.base_texture == "nature/foam" && surface.normal_map == "liquids/water_normal",
                "Flow water must retain its shader, surface side and normal map even with a foam base texture.");
            {
                std::ofstream file(water.path);
                file << R"("Water" { "$abovewater" "0" "$normalmap" "water_normal" })";
            }
            require(!gmod::source::vmt::VmtReader(water.path).read().water_above,
                "The underwater material must not obscure the surface or the refracted seabed.");
            {
                std::ofstream file(water.path);
                file << R"("Water" { "$normalmap" "water_normal" })";
            }
            require(gmod::source::vmt::VmtReader(water.path).read().water_above,
                "Water without an explicit side must default to the above-water surface.");
            std::cout << "PASS: flow-water normal maps, foam base textures and above/below-water materials\n";
        }
        auto bytes = header(1, 2, 2);
        bytes.insert(bytes.end(), {99, 98, 97, 96, 95, 94, 93, 92});
        const std::vector<std::uint8_t> abgr{
            0, 30, 20, 10, 64, 60, 50, 40, 128, 90, 80, 70, 255, 120, 110, 100};
        bytes.insert(bytes.end(), abgr.begin(), abgr.end());
        bytes.insert(bytes.end(), 16, 222);
        fixture.write(bytes);
        const gmod::source::vtf::VtfReader reader(fixture.path);
        require(reader.read().supports_direct_rgba_decode(), "ABGR8888 must advertise support");
        const std::vector<std::uint8_t> expected{
            10, 20, 30, 0, 40, 50, 60, 64, 70, 80, 90, 128, 100, 110, 120, 255};
        const auto pixels = reader.read_top_mip();
        require(pixels.width == 2 && pixels.height == 2 && pixels.rgba == expected,
                "ABGR8888 must preserve channels, alpha, rows and frame-zero mip offsets");
        const auto native = reader.read_native();
        require(native.format == gmod::source::vtf::PixelFormat::Rgba8
                    && native.mip_levels == 1 && native.rgba == expected,
                "Native ABGR8888 upload must receive decoded RGBA8");

        bytes.resize(80 + 8 + 15);
        fixture.write(bytes);
        bool rejected = false;
        try {
            reader.read_top_mip();
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()) == "VTF uncompressed pixels are truncated.";
        }
        require(rejected, "Truncated ABGR8888 must report an explicit decoding error");

        for (const int format : {0, 12}) {
            bytes = header(format);
            for (int i = 0; i < 4; ++i) {
                if (format == 12) bytes.insert(bytes.end(), {30, 20, 10, 64});
                else bytes.insert(bytes.end(), {10, 20, 30, 64});
            }
            fixture.write(bytes);
            const auto previous = reader.read_top_mip();
            require(previous.rgba[0] == 10 && previous.rgba[1] == 20
                        && previous.rgba[2] == 30 && previous.rgba[3] == 64,
                    "Existing RGBA8888 and BGRA8888 decoding must remain unchanged");
        }
        std::cout << "PASS: ABGR8888 channels, alpha, mip/frame offsets, native upload, "
                     "truncation and existing uncompressed formats\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
