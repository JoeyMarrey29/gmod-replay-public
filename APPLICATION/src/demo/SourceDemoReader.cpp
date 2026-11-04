#include "demo/SourceDemoReader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace gmod::demo {
namespace {

constexpr std::size_t HeaderSize = 1072;
constexpr std::size_t CommandInfoSize = 76;
constexpr std::int32_t MaxPayloadBytes = 16 * 1024 * 1024;

class TruncatedRecord final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void read_exact(
    std::istream& input,
    void* destination,
    std::size_t size,
    const std::filesystem::path& path,
    std::string_view context) {
    input.read(static_cast<char*>(destination), static_cast<std::streamsize>(size));
    if (static_cast<std::size_t>(input.gcount()) != size) {
        throw TruncatedRecord(
            "Truncated demo " + path.string() + " while reading " + std::string(context) + ".");
    }
}

std::int32_t read_i32(
    std::istream& input,
    const std::filesystem::path& path,
    std::string_view context) {
    std::array<std::uint8_t, 4> bytes{};
    read_exact(input, bytes.data(), bytes.size(), path, context);
    const std::uint32_t value = static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8)
        | (static_cast<std::uint32_t>(bytes[2]) << 16)
        | (static_cast<std::uint32_t>(bytes[3]) << 24);
    return std::bit_cast<std::int32_t>(value);
}

void skip_payload(
    std::istream& input,
    std::int32_t length,
    std::uintmax_t file_size,
    const std::filesystem::path& path) {
    if (length < 0 || length > MaxPayloadBytes) {
        throw std::runtime_error("Demo contains an invalid command payload length.");
    }
    const auto position = input.tellg();
    if (position < 0
        || static_cast<std::uintmax_t>(position) > file_size) {
        throw std::runtime_error("Demo command payload has an invalid file offset: " + path.string());
    }
    if (static_cast<std::uintmax_t>(length)
        > file_size - static_cast<std::uintmax_t>(position)) {
        throw TruncatedRecord(
            "Demo final command payload is incomplete; only "
            + std::to_string(file_size - static_cast<std::uintmax_t>(position))
            + " of " + std::to_string(length) + " bytes remain.");
    }
    if (length > 0) {
        input.seekg(length, std::ios::cur);
    }
    if (!input) {
        if (input.eof()) {
            throw TruncatedRecord("Demo ended while advancing through its final command payload.");
        }
        throw std::runtime_error("Could not advance through demo command payload: " + path.string());
    }
}

std::string fixed_string(
    const std::array<std::uint8_t, HeaderSize>& header,
    std::size_t offset) {
    const auto begin = header.begin() + static_cast<std::ptrdiff_t>(offset);
    const auto end = begin + 260;
    const auto terminator = std::find(begin, end, static_cast<std::uint8_t>(0));
    return std::string(begin, terminator);
}

SourceDemoHeader parse_header(
    const std::array<std::uint8_t, HeaderSize>& bytes,
    const std::filesystem::path& path) {
    constexpr std::array<std::uint8_t, 8> source_magic{
        'H', 'L', '2', 'D', 'E', 'M', 'O', 0};
    constexpr std::array<std::uint8_t, 8> gmod_magic{
        'G', 'M', 'O', 'D', 'E', 'M', 'O', 0};
    if (!std::equal(source_magic.begin(), source_magic.end(), bytes.begin())
        && !std::equal(gmod_magic.begin(), gmod_magic.end(), bytes.begin())) {
        throw std::runtime_error("Demo has an unsupported Source/GMod magic: " + path.string());
    }

    const auto scalar_i32 = [&](std::size_t offset) {
        const std::uint32_t value = static_cast<std::uint32_t>(bytes[offset])
            | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
            | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
            | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
        return std::bit_cast<std::int32_t>(value);
    };
    const auto scalar_f32 = [&](std::size_t offset) {
        const std::uint32_t value = static_cast<std::uint32_t>(bytes[offset])
            | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
            | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
            | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
        return std::bit_cast<float>(value);
    };

    SourceDemoHeader header;
    header.magic = bytes[0] == 'G' ? "GMODEMO" : "HL2DEMO";
    header.demo_protocol = scalar_i32(8);
    header.network_protocol = scalar_i32(12);
    if (header.demo_protocol != 3) {
        throw std::runtime_error(
            "Unsupported demo protocol " + std::to_string(header.demo_protocol)
            + "; the in-app reader currently supports protocol 3 only.");
    }
    header.server_name = fixed_string(bytes, 16);
    header.client_name = fixed_string(bytes, 276);
    header.map_name = fixed_string(bytes, 536);
    header.game_directory = fixed_string(bytes, 796);
    header.playback_time = scalar_f32(1056);
    header.playback_ticks = scalar_i32(1060);
    header.playback_frames = scalar_i32(1064);
    header.signon_length = scalar_i32(1068);
    if (header.playback_time < 0.0f || header.playback_ticks < 0
        || header.playback_frames < 0 || header.signon_length < 0) {
        throw std::runtime_error("Demo header contains invalid playback metadata: " + path.string());
    }
    return header;
}

// Little-endian bit stream, as used by Source net messages.
class BitReader {
public:
    explicit BitReader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    bool ok() const { return !failed_; }

    std::uint32_t read(int bits) {
        std::uint64_t value = 0;
        for (int index = 0; index < bits; ++index) {
            const std::size_t byte = position_ >> 3;
            if (byte >= bytes_.size()) {
                failed_ = true;
                return 0;
            }
            value |= static_cast<std::uint64_t>((bytes_[byte] >> (position_ & 7)) & 1) << index;
            ++position_;
        }
        return static_cast<std::uint32_t>(value);
    }

    void skip_string() {
        while (read(8) != 0 && !failed_) {}
    }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t position_ = 0;
    bool failed_ = false;
};

// The demo header's tick count is unreliable (the Stop record carries a bogus tick and the
// recording client assumed a default tick rate); the server's own interval in svc_ServerInfo
// is what entity ticks are measured in. Returns 0 when it cannot be found.
float find_tick_interval(const std::vector<std::uint8_t>& payload) {
    constexpr std::uint32_t SvcPrint = 7;
    constexpr std::uint32_t SvcServerInfo = 8;
    BitReader reader(payload);
    while (reader.ok()) {
        const auto type = reader.read(6);
        if (type == SvcPrint) {
            reader.skip_string();
            continue;
        }
        if (type != SvcServerInfo) return 0.0f;
        reader.read(16);  // protocol
        reader.read(32);  // server count
        reader.read(1);   // HLTV
        reader.read(1);   // dedicated
        reader.read(32);  // client CRC
        reader.read(16);  // max classes
        for (int index = 0; index < 4; ++index) reader.read(32);  // map MD5
        reader.read(8);   // player slot
        reader.read(8);   // max clients
        const auto bits = reader.read(32);
        if (!reader.ok()) return 0.0f;
        const float interval = std::bit_cast<float>(bits);
        return interval > 0.001f && interval < 0.5f ? interval : 0.0f;
    }
    return 0.0f;
}

} // namespace

SourceDemoScan scan_source_demo(
    const std::filesystem::path& path,
    const std::atomic<bool>& cancel) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not open demo for in-app parsing: " + path.string());
    }
    std::error_code size_error;
    const auto file_size = std::filesystem::file_size(path, size_error);
    if (size_error || file_size < HeaderSize) {
        throw std::runtime_error("Demo is too short to contain its Source header: " + path.string());
    }

    std::array<std::uint8_t, HeaderSize> header_bytes{};
    read_exact(input, header_bytes.data(), header_bytes.size(), path, "the Source header");

    SourceDemoScan scan;
    scan.header = parse_header(header_bytes, path);
    std::uint64_t command_count = 0;
    bool saw_stop = false;
    while (!saw_stop) {
        if (cancel) throw std::runtime_error("cancelled");
        try {
            const int raw_command = input.get();
            if (raw_command == std::char_traits<char>::eof()) {
                scan.tail_recovery_message =
                    "Demo command stream ended without dem_stop; complete commands before EOF were retained.";
                break;
            }
            if (raw_command < 1 || raw_command > 8) {
                throw std::runtime_error("Demo command stream contains an invalid command type.");
            }
            const int command = raw_command;
            const auto command_tick = read_i32(input, path, "a command tick");
            ++command_count;
            if (command_count > file_size) {
                throw std::runtime_error("Demo command stream did not make forward progress.");
            }

            switch (command) {
        case 1:
        case 2: {
            std::array<std::uint8_t, CommandInfoSize> command_info{};
            read_exact(input, command_info.data(), command_info.size(), path, "packet command info");
            (void)read_i32(input, path, "a packet sequence number");
            (void)read_i32(input, path, "a packet sequence number");
            const auto length = read_i32(input, path, "a packet payload length");
            if (length < 0 || length > MaxPayloadBytes) {
                throw std::runtime_error("Demo contains an invalid command payload length.");
            }
            if (command == 1 && scan.tick_interval == 0.0f && length > 0 && length <= 1 << 20) {
                std::vector<std::uint8_t> payload(static_cast<std::size_t>(length));
                read_exact(input, payload.data(), payload.size(), path, "a signon payload");
                scan.tick_interval = find_tick_interval(payload);
            } else {
                skip_payload(input, length, file_size, path);
            }
            // Keep camera samples transactional: an incomplete final packet must not contribute.
            if (command_tick > 0) {
                auto read_float = [&](std::size_t offset) {
                    std::uint32_t bits = static_cast<std::uint32_t>(command_info[offset])
                        | (static_cast<std::uint32_t>(command_info[offset + 1]) << 8)
                        | (static_cast<std::uint32_t>(command_info[offset + 2]) << 16)
                        | (static_cast<std::uint32_t>(command_info[offset + 3]) << 24);
                    return std::bit_cast<float>(bits);
                };
                scan.camera.push_back({command_tick, read_float(4), read_float(8), read_float(12),
                                       read_float(16), read_float(20)});
            }
            ++scan.packet_count;
            scan.packet_bytes += static_cast<std::uint64_t>(length);
            break;
        }
        case 3:
            break;
        case 4: {
            const auto length = read_i32(input, path, "a console-command payload length");
            skip_payload(input, length, file_size, path);
            break;
        }
        case 5: {
            (void)read_i32(input, path, "a user-command number");
            const auto length = read_i32(input, path, "a user-command payload length");
            skip_payload(input, length, file_size, path);
            break;
        }
        case 6:
        case 8: {
            const auto length = read_i32(input, path, "a table payload length");
            skip_payload(input, length, file_size, path);
            break;
        }
        case 7:
            saw_stop = true;
            break;
        default:
            throw std::runtime_error("Demo command stream contains an unsupported command type.");
        }
            if (command != 3 && command != 7) {
                scan.last_content_tick = std::max(scan.last_content_tick, command_tick);
            }
        } catch (const TruncatedRecord& error) {
            scan.tail_recovery_message =
                std::string(error.what()) + " The incomplete final command was discarded.";
            break;
        }
    }
    return scan;
}

float effective_demo_duration(const SourceDemoScan& scan, float tick_rate) {
    const bool has_observed_duration = scan.last_content_tick > 0
        && std::isfinite(tick_rate) && tick_rate > 0.0f;
    const float observed_duration = has_observed_duration
        ? static_cast<float>(scan.last_content_tick) / tick_rate
        : 0.0f;
    if (!std::isfinite(scan.header.playback_time) || scan.header.playback_time <= 0.0f) {
        return observed_duration;
    }

    if (!has_observed_duration) {
        return scan.header.playback_time;
    }

    constexpr float MaxClockDisagreement = 1.25f;
    float disagreement = scan.header.playback_time / observed_duration;
    if (scan.header.playback_ticks > 0) {
        disagreement = static_cast<float>(scan.header.playback_ticks)
            / static_cast<float>(scan.last_content_tick);
    }
    if (disagreement >= MaxClockDisagreement || disagreement <= 1.0f / MaxClockDisagreement) {
        return observed_duration;
    }

    return scan.header.playback_time;
}

} // namespace gmod::demo
