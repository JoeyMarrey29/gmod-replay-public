#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Reads game content straight out of Garry's Mod's own archives. A mounted archive behaves like
// a folder: "...\content.gma\materials\x.vmt" is the file materials/x.vmt inside content.gma.
// Paths that are not inside a mounted archive are read from disk as usual.
namespace gmod::source::vfs {

// Mounts a Workshop .gma or a Valve *_dir.vpk. Re-mounting an unchanged archive is free;
// a changed one (Steam updated it) is indexed again. Returns false when it can't be read.
bool mount(const std::filesystem::path& archive);
// Mounts the zip stored at [offset, offset + size) of `container` (a disk file or a file inside
// another mounted archive, e.g. a map's pakfile lump) under `mount_point`.
bool mount_zip(const std::filesystem::path& mount_point, const std::filesystem::path& container,
               std::uint64_t offset, std::uint64_t size);
bool is_mounted(const std::filesystem::path& archive);
// True when the path names something inside a mounted archive (rather than on disk).
bool is_archived(const std::filesystem::path& path);

bool exists(const std::filesystem::path& path);
bool is_directory(const std::filesystem::path& path);
std::optional<std::uint64_t> file_size(const std::filesystem::path& path);
// The whole file (decompressed). Throws std::runtime_error when it is missing or unreadable.
std::vector<std::uint8_t> read(const std::filesystem::path& path);
// `length` bytes from `offset` of a file. Throws when the range is outside the file.
std::vector<std::uint8_t> read_range(const std::filesystem::path& path, std::uint64_t offset,
                                     std::size_t length);
// Files below `folder` as '/'-separated paths relative to it (all depths when recursive).
std::vector<std::string> list_files(const std::filesystem::path& folder, bool recursive);
// Names of the folders directly inside `folder`.
std::vector<std::string> list_folders(const std::filesystem::path& folder);

// Raw LZMA stream with its 5 property bytes.
std::vector<std::uint8_t> lzma_decode(const std::uint8_t* properties, const std::uint8_t* data,
                                      std::size_t size, std::size_t unpacked_size);
// Valve's "LZMA" block (id, actual size, lzma size, properties, data) used by BSP lumps.
bool is_valve_lzma(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> valve_lzma_decode(const std::uint8_t* data, std::size_t size);

} // namespace gmod::source::vfs
