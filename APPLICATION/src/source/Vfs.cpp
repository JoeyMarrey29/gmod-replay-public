#include "source/Vfs.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#include "LzmaDec.h"

namespace gmod::source::vfs {
namespace {

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size,
                        nullptr, nullptr);
    return result;
}

std::string normalize_inner(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    std::string result;
    result.reserve(name.size());
    for (const char c : name) {
        if (c == '/' && (result.empty() || result.back() == '/')) continue;
        result.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    }
    while (!result.empty() && result.back() == '/') result.pop_back();
    return result;
}

std::string key_of(const std::filesystem::path& path) {
    return normalize_inner(utf8(path.wstring()));
}

// ---- Shared read-only handles with positional (thread-safe) reads --------------------------

std::mutex g_handle_mutex;
std::unordered_map<std::wstring, HANDLE> g_handles;

HANDLE open_shared(const std::wstring& path) {
    std::lock_guard lock(g_handle_mutex);
    const auto found = g_handles.find(path);
    if (found != g_handles.end()) return found->second;
    // Steam may replace the file while we have it open (Workshop updates).
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return handle;
    g_handles.emplace(path, handle);
    return handle;
}

void close_shared(const std::wstring& path) {
    std::lock_guard lock(g_handle_mutex);
    const auto found = g_handles.find(path);
    if (found == g_handles.end()) return;
    CloseHandle(found->second);
    g_handles.erase(found);
}

void pread(const std::wstring& path, std::uint64_t offset, void* out, std::size_t size) {
    const HANDLE handle = open_shared(path);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Could not open " + utf8(path));
    }
    auto* cursor = static_cast<std::uint8_t*>(out);
    while (size > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 64u << 20));
        OVERLAPPED overlapped{};
        overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD got = 0;
        if (!ReadFile(handle, cursor, chunk, &got, &overlapped) || got == 0) {
            throw std::runtime_error("Could not read " + utf8(path));
        }
        cursor += got;
        offset += got;
        size -= got;
    }
}

std::uint64_t disk_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

std::uint64_t disk_stamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32)
         | data.ftLastWriteTime.dwLowDateTime;
}

// Sequential reader over part of a file, for parsing archive directories.
class Cursor {
public:
    Cursor(std::wstring path, std::uint64_t start, std::uint64_t end)
        : path_(std::move(path)), position_(start), end_(end) {}

    std::uint64_t position() const { return position_; }

    void bytes(void* out, std::size_t size) {
        auto* cursor = static_cast<std::uint8_t*>(out);
        while (size > 0) {
            if (position_ < buffer_start_ || position_ >= buffer_start_ + buffer_.size()) fill();
            const std::size_t at = static_cast<std::size_t>(position_ - buffer_start_);
            const std::size_t take = std::min(size, buffer_.size() - at);
            std::memcpy(cursor, buffer_.data() + at, take);
            cursor += take;
            position_ += take;
            size -= take;
        }
    }

    template <typename T>
    T value() {
        T result{};
        bytes(&result, sizeof(T));
        return result;
    }

    std::string text() {
        std::string result;
        for (;;) {
            const char c = value<char>();
            if (c == '\0') return result;
            result.push_back(c);
            if (result.size() > 4096) throw std::runtime_error("Archive name is too long.");
        }
    }

private:
    void fill() {
        if (position_ >= end_) throw std::runtime_error("Archive directory is truncated.");
        buffer_start_ = position_;
        buffer_.resize(static_cast<std::size_t>(std::min<std::uint64_t>(end_ - position_, 1u << 20)));
        pread(path_, buffer_start_, buffer_.data(), buffer_.size());
    }

    std::wstring path_;
    std::uint64_t position_;
    std::uint64_t end_;
    std::uint64_t buffer_start_ = 0;
    std::vector<std::uint8_t> buffer_;
};

// ---- Archives --------------------------------------------------------------------------------

enum class Method : std::uint8_t { Stored, ZipStored, ZipLzma };

struct Entry {
    std::uint32_t source = 0;      // index into Archive::sources
    std::uint64_t offset = 0;      // absolute offset in that file (zip: of the local header)
    std::uint64_t size = 0;        // bytes stored there
    std::uint64_t unpacked = 0;    // file size
    std::uint32_t preload_at = 0;  // VPK bytes kept in the directory file
    std::uint32_t preload_size = 0;
    Method method = Method::Stored;
};

struct Archive {
    std::string prefix;  // normalized archive path
    std::wstring path;
    std::uint64_t stamp_size = 0;
    std::uint64_t stamp_time = 0;
    std::vector<std::wstring> sources;
    std::vector<std::uint8_t> preload;
    std::unordered_map<std::string, Entry> files;
    std::unordered_map<std::string, std::vector<std::string>> child_files;    // folder -> names
    std::unordered_map<std::string, std::vector<std::string>> child_folders;  // folder -> names
    std::unordered_set<std::string> folders;

    void add(std::string name, Entry entry) {
        name = normalize_inner(std::move(name));
        if (name.empty() || !files.emplace(name, entry).second) return;
        auto slash = name.find_last_of('/');
        std::string folder = slash == std::string::npos ? std::string() : name.substr(0, slash);
        child_files[folder].push_back(name.substr(slash == std::string::npos ? 0 : slash + 1));
        while (!folder.empty() && folders.insert(folder).second) {
            slash = folder.find_last_of('/');
            const std::string parent = slash == std::string::npos ? std::string() : folder.substr(0, slash);
            child_folders[parent].push_back(folder.substr(slash == std::string::npos ? 0 : slash + 1));
            folder = parent;
        }
    }

    bool has_folder(const std::string& inner) const {
        return inner.empty() || folders.contains(inner);
    }
};

std::shared_mutex g_mutex;
std::vector<std::shared_ptr<Archive>> g_archives;

std::shared_ptr<Archive> parse_gma(const std::wstring& path) {
    const auto size = disk_size(path);
    Cursor in(path, 0, size);
    char magic[4];
    in.bytes(magic, 4);
    if (std::memcmp(magic, "GMAD", 4) != 0) throw std::runtime_error("Not a GMA file.");
    const auto version = in.value<std::uint8_t>();
    in.value<std::uint64_t>();  // author SteamID
    in.value<std::uint64_t>();  // timestamp
    if (version > 1) {
        while (!in.text().empty()) {}  // required content
    }
    in.text();  // name
    in.text();  // description
    in.text();  // author
    in.value<std::int32_t>();
    struct Pending {
        std::string name;
        std::uint64_t size;
    };
    std::vector<Pending> pending;
    for (;;) {
        if (in.value<std::uint32_t>() == 0) break;
        Pending item;
        item.name = in.text();
        item.size = static_cast<std::uint64_t>(in.value<std::int64_t>());
        in.value<std::uint32_t>();  // CRC
        pending.push_back(std::move(item));
    }
    auto archive = std::make_shared<Archive>();
    archive->sources.push_back(path);
    std::uint64_t offset = in.position();
    for (auto& item : pending) {
        Entry entry;
        entry.offset = offset;
        entry.size = entry.unpacked = item.size;
        if (offset + item.size > size) throw std::runtime_error("GMA file data is truncated.");
        archive->add(std::move(item.name), entry);
        offset += item.size;
    }
    return archive;
}

std::shared_ptr<Archive> parse_vpk(const std::wstring& path) {
    const auto size = disk_size(path);
    Cursor in(path, 0, size);
    if (in.value<std::uint32_t>() != 0x55AA1234u) throw std::runtime_error("Not a VPK directory.");
    const auto version = in.value<std::uint32_t>();
    const auto tree_size = in.value<std::uint32_t>();
    std::uint64_t header = 12;
    if (version == 2) {
        for (int i = 0; i < 4; ++i) in.value<std::uint32_t>();
        header = 28;
    } else if (version != 1) {
        throw std::runtime_error("Unsupported VPK version.");
    }
    auto archive = std::make_shared<Archive>();
    archive->sources.push_back(path);
    std::wstring stem = path;
    if (stem.size() > 8 && _wcsicmp(stem.c_str() + stem.size() - 8, L"_dir.vpk") == 0) stem.resize(stem.size() - 8);
    const std::uint64_t data_start = header + tree_size;
    for (;;) {
        const auto extension = in.text();
        if (extension.empty()) break;
        for (;;) {
            const auto folder = in.text();
            if (folder.empty()) break;
            for (;;) {
                const auto name = in.text();
                if (name.empty()) break;
                in.value<std::uint32_t>();  // CRC
                const auto preload = in.value<std::uint16_t>();
                const auto index = in.value<std::uint16_t>();
                const auto offset = in.value<std::uint32_t>();
                const auto length = in.value<std::uint32_t>();
                in.value<std::uint16_t>();  // terminator
                Entry entry;
                entry.preload_at = static_cast<std::uint32_t>(archive->preload.size());
                entry.preload_size = preload;
                if (preload > 0) {
                    archive->preload.resize(archive->preload.size() + preload);
                    in.bytes(archive->preload.data() + entry.preload_at, preload);
                }
                if (index == 0x7FFF) {
                    entry.source = 0;
                    entry.offset = data_start + offset;
                } else {
                    entry.source = static_cast<std::uint32_t>(index) + 1;
                    if (archive->sources.size() <= entry.source) archive->sources.resize(entry.source + 1);
                    if (archive->sources[entry.source].empty()) {
                        wchar_t suffix[16];
                        swprintf_s(suffix, L"_%03u.vpk", static_cast<unsigned>(index));
                        archive->sources[entry.source] = stem + suffix;
                    }
                    entry.offset = offset;
                }
                entry.size = length;
                entry.unpacked = static_cast<std::uint64_t>(preload) + length;
                std::string full = folder == " " ? std::string() : folder + "/";
                full += name;
                if (extension != " ") full += "." + extension;
                archive->add(std::move(full), entry);
            }
        }
    }
    return archive;
}

std::shared_ptr<Archive> parse_zip(const std::wstring& path, std::uint64_t base, std::uint64_t size) {
    if (size < 22) throw std::runtime_error("Zip is too small.");
    const std::size_t tail_size = static_cast<std::size_t>(std::min<std::uint64_t>(size, 65557));
    std::vector<std::uint8_t> tail(tail_size);
    pread(path, base + size - tail_size, tail.data(), tail.size());
    std::size_t end = std::string::npos;
    for (std::size_t i = tail_size - 22 + 1; i-- > 0;) {
        if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6) {
            end = i;
            break;
        }
    }
    if (end == std::string::npos) throw std::runtime_error("Zip end record not found.");
    auto get16 = [](const std::uint8_t* p) { return static_cast<std::uint32_t>(p[0] | (p[1] << 8)); };
    auto get32 = [](const std::uint8_t* p) {
        return static_cast<std::uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24));
    };
    const auto count = get16(&tail[end + 10]);
    const auto directory_size = get32(&tail[end + 12]);
    const auto directory_offset = get32(&tail[end + 16]);
    if (static_cast<std::uint64_t>(directory_offset) + directory_size > size) {
        throw std::runtime_error("Zip directory is outside the zip.");
    }
    std::vector<std::uint8_t> directory(directory_size);
    pread(path, base + directory_offset, directory.data(), directory.size());
    auto archive = std::make_shared<Archive>();
    archive->sources.push_back(path);
    std::size_t at = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (at + 46 > directory.size() || get32(&directory[at]) != 0x02014B50u) {
            throw std::runtime_error("Zip directory entry is invalid.");
        }
        const auto* record = &directory[at];
        const auto method = get16(record + 10);
        const auto packed = get32(record + 20);
        const auto unpacked = get32(record + 24);
        const auto name_length = get16(record + 28);
        const auto extra_length = get16(record + 30);
        const auto comment_length = get16(record + 32);
        const auto local = get32(record + 42);
        if (at + 46 + name_length > directory.size()) throw std::runtime_error("Zip name is truncated.");
        std::string name(reinterpret_cast<const char*>(record + 46), name_length);
        at += 46 + name_length + extra_length + comment_length;
        if (name.empty() || name.back() == '/') continue;
        if (method != 0 && method != 14) continue;
        Entry entry;
        entry.offset = base + local;
        entry.size = packed;
        entry.unpacked = unpacked;
        entry.method = method == 0 ? Method::ZipStored : Method::ZipLzma;
        archive->add(std::move(name), entry);
    }
    return archive;
}

struct Located {
    std::shared_ptr<Archive> archive;
    std::string inner;
};

Located locate(const std::string& key) {
    std::shared_lock lock(g_mutex);
    // Longest prefix wins: a pakfile can be mounted at a path inside another archive.
    const std::shared_ptr<Archive>* best = nullptr;
    for (const auto& archive : g_archives) {
        const auto& prefix = archive->prefix;
        if (key.size() >= prefix.size() && key.compare(0, prefix.size(), prefix) == 0
            && (key.size() == prefix.size() || key[prefix.size()] == '/')
            && (!best || prefix.size() > (*best)->prefix.size())) {
            best = &archive;
        }
    }
    if (!best) return {};
    const auto& prefix = (*best)->prefix;
    return {*best, key.size() == prefix.size() ? std::string() : key.substr(prefix.size() + 1)};
}

const Entry* find_entry(const Located& located) {
    if (!located.archive) return nullptr;
    const auto found = located.archive->files.find(located.inner);
    return found == located.archive->files.end() ? nullptr : &found->second;
}

std::uint64_t zip_data_offset(const Archive& archive, const Entry& entry) {
    std::uint8_t header[30];
    pread(archive.sources[entry.source], entry.offset, header, sizeof(header));
    if (header[0] != 'P' || header[1] != 'K' || header[2] != 3 || header[3] != 4) {
        throw std::runtime_error("Zip local header is invalid.");
    }
    const auto name_length = static_cast<std::uint32_t>(header[26] | (header[27] << 8));
    const auto extra_length = static_cast<std::uint32_t>(header[28] | (header[29] << 8));
    return entry.offset + 30 + name_length + extra_length;
}

std::vector<std::uint8_t> read_entry(const Archive& archive, const Entry& entry) {
    if (entry.source >= archive.sources.size() || archive.sources[entry.source].empty()) {
        throw std::runtime_error("Archive part is missing.");
    }
    const auto& source = archive.sources[entry.source];
    switch (entry.method) {
    case Method::Stored: {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(entry.unpacked));
        if (entry.preload_size > 0) {
            std::memcpy(bytes.data(), archive.preload.data() + entry.preload_at, entry.preload_size);
        }
        if (entry.size > 0) pread(source, entry.offset, bytes.data() + entry.preload_size, static_cast<std::size_t>(entry.size));
        return bytes;
    }
    case Method::ZipStored: {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(entry.size));
        if (!bytes.empty()) pread(source, zip_data_offset(archive, entry), bytes.data(), bytes.size());
        return bytes;
    }
    case Method::ZipLzma: {
        // Zip LZMA (method 14): version (2), property size (2), properties, raw stream.
        std::vector<std::uint8_t> packed(static_cast<std::size_t>(entry.size));
        pread(source, zip_data_offset(archive, entry), packed.data(), packed.size());
        if (packed.size() < 4) throw std::runtime_error("Zip LZMA entry is truncated.");
        const std::size_t properties = packed[2] | (packed[3] << 8);
        if (properties != LZMA_PROPS_SIZE || packed.size() < 4 + properties) {
            throw std::runtime_error("Zip LZMA properties are invalid.");
        }
        return lzma_decode(packed.data() + 4, packed.data() + 4 + properties, packed.size() - 4 - properties,
                           static_cast<std::size_t>(entry.unpacked));
    }
    }
    return {};
}

void register_archive(std::shared_ptr<Archive> archive) {
    std::unique_lock lock(g_mutex);
    for (auto& existing : g_archives) {
        if (existing->prefix == archive->prefix) {
            existing = std::move(archive);
            return;
        }
    }
    g_archives.push_back(std::move(archive));
}

void* lzma_alloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
void lzma_free(ISzAllocPtr, void* address) { std::free(address); }
const ISzAlloc g_lzma_alloc{lzma_alloc, lzma_free};

std::vector<std::uint8_t> read_disk(const std::filesystem::path& path) {
    const std::wstring name = path.wstring();
    const HANDLE handle = CreateFileW(name.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not open " + path.string());
    LARGE_INTEGER size{};
    GetFileSizeEx(handle, &size);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
    std::size_t done = 0;
    while (done < bytes.size()) {
        DWORD got = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 64u << 20));
        if (!ReadFile(handle, bytes.data() + done, chunk, &got, nullptr) || got == 0) break;
        done += got;
    }
    CloseHandle(handle);
    if (done != bytes.size()) throw std::runtime_error("Could not read " + path.string());
    return bytes;
}

} // namespace

bool mount(const std::filesystem::path& archive_path) {
    const std::wstring path = archive_path.wstring();
    const auto prefix = key_of(archive_path);
    const auto size = disk_size(path);
    const auto stamp = disk_stamp(path);
    if (size == 0) return false;
    {
        std::shared_lock lock(g_mutex);
        for (const auto& existing : g_archives) {
            if (existing->prefix == prefix && existing->stamp_size == size && existing->stamp_time == stamp) return true;
        }
    }
    // The file changed or is new: forget any old handle so we read the current file.
    close_shared(path);
    try {
        const auto extension = archive_path.extension().wstring();
        std::shared_ptr<Archive> archive = _wcsicmp(extension.c_str(), L".vpk") == 0 ? parse_vpk(path) : parse_gma(path);
        archive->prefix = prefix;
        archive->path = path;
        archive->stamp_size = size;
        archive->stamp_time = stamp;
        register_archive(std::move(archive));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool mount_zip(const std::filesystem::path& mount_point, const std::filesystem::path& container,
               std::uint64_t offset, std::uint64_t size) {
    try {
        // Find the bytes on disk: the container may itself be a stored file inside an archive.
        std::wstring physical = container.wstring();
        std::uint64_t base = offset;
        const auto located = locate(key_of(container));
        if (located.archive) {
            const auto* entry = find_entry(located);
            if (!entry || entry->method != Method::Stored || entry->preload_size != 0) return false;
            physical = located.archive->sources[entry->source];
            base += entry->offset;
            if (offset + size > entry->size) return false;
        }
        auto archive = parse_zip(physical, base, size);
        archive->prefix = key_of(mount_point);
        archive->path = mount_point.wstring();
        register_archive(std::move(archive));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool is_mounted(const std::filesystem::path& archive) {
    const auto prefix = key_of(archive);
    std::shared_lock lock(g_mutex);
    return std::any_of(g_archives.begin(), g_archives.end(),
                       [&](const auto& existing) { return existing->prefix == prefix; });
}

bool is_archived(const std::filesystem::path& path) {
    return locate(key_of(path)).archive != nullptr;
}

bool exists(const std::filesystem::path& path) {
    const auto located = locate(key_of(path));
    if (located.archive) return find_entry(located) || located.archive->has_folder(located.inner);
    std::error_code error;
    return std::filesystem::exists(path, error);
}

bool is_directory(const std::filesystem::path& path) {
    const auto located = locate(key_of(path));
    if (located.archive) return located.archive->has_folder(located.inner) && !find_entry(located);
    std::error_code error;
    return std::filesystem::is_directory(path, error);
}

std::optional<std::uint64_t> file_size(const std::filesystem::path& path) {
    const auto located = locate(key_of(path));
    if (located.archive) {
        const auto* entry = find_entry(located);
        if (!entry) return std::nullopt;
        return entry->unpacked;
    }
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) return std::nullopt;
    return size;
}

std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    const auto located = locate(key_of(path));
    if (located.archive) {
        const auto* entry = find_entry(located);
        if (!entry) throw std::runtime_error("Could not open " + path.string());
        return read_entry(*located.archive, *entry);
    }
    return read_disk(path);
}

std::vector<std::uint8_t> read_range(const std::filesystem::path& path, std::uint64_t offset,
                                     std::size_t length) {
    const auto located = locate(key_of(path));
    std::vector<std::uint8_t> bytes(length);
    if (located.archive) {
        const auto* entry = find_entry(located);
        if (!entry) throw std::runtime_error("Could not open " + path.string());
        if (offset + length > entry->unpacked) throw std::runtime_error("Read past the end of " + path.string());
        if (entry->method != Method::Stored) {
            const auto whole = read_entry(*located.archive, *entry);
            std::memcpy(bytes.data(), whole.data() + offset, length);
            return bytes;
        }
        std::size_t done = 0;
        if (offset < entry->preload_size) {
            done = static_cast<std::size_t>(std::min<std::uint64_t>(length, entry->preload_size - offset));
            std::memcpy(bytes.data(), located.archive->preload.data() + entry->preload_at + offset, done);
        }
        if (done < length) {
            pread(located.archive->sources[entry->source], entry->offset + offset + done - entry->preload_size,
                  bytes.data() + done, length - done);
        }
        return bytes;
    }
    if (offset + length > disk_size(path.wstring())) throw std::runtime_error("Read past the end of " + path.string());
    const HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not open " + path.string());
    std::size_t done = 0;
    while (done < length) {
        OVERLAPPED overlapped{};
        const auto at = offset + done;
        overlapped.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
        overlapped.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(length - done, 64u << 20));
        if (!ReadFile(handle, bytes.data() + done, chunk, &got, &overlapped) || got == 0) break;
        done += got;
    }
    CloseHandle(handle);
    if (done != length) throw std::runtime_error("Could not read " + path.string());
    return bytes;
}

std::vector<std::string> list_files(const std::filesystem::path& folder, bool recursive) {
    std::vector<std::string> result;
    const auto located = locate(key_of(folder));
    if (located.archive) {
        const auto& archive = *located.archive;
        std::vector<std::pair<std::string, std::string>> pending{{located.inner, std::string()}};
        while (!pending.empty()) {
            const auto [inner, relative] = pending.back();
            pending.pop_back();
            const auto files = archive.child_files.find(inner);
            if (files != archive.child_files.end()) {
                for (const auto& name : files->second) result.push_back(relative + name);
            }
            if (!recursive) break;
            const auto folders = archive.child_folders.find(inner);
            if (folders == archive.child_folders.end()) continue;
            for (const auto& name : folders->second) {
                pending.emplace_back(inner.empty() ? name : inner + "/" + name, relative + name + "/");
            }
        }
        return result;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) return result;
    if (recursive) {
        for (std::filesystem::recursive_directory_iterator it(folder, error), end; !error && it != end;
             it.increment(error)) {
            if (it->is_regular_file(error)) {
                result.push_back(std::filesystem::relative(it->path(), folder, error).generic_string());
            }
        }
    } else {
        for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
            if (it->is_regular_file(error)) result.push_back(it->path().filename().generic_string());
        }
    }
    return result;
}

std::vector<std::string> list_folders(const std::filesystem::path& folder) {
    std::vector<std::string> result;
    const auto located = locate(key_of(folder));
    if (located.archive) {
        const auto found = located.archive->child_folders.find(located.inner);
        if (found != located.archive->child_folders.end()) result = found->second;
        return result;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) return result;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (it->is_directory(error)) result.push_back(it->path().filename().generic_string());
    }
    return result;
}

std::vector<std::uint8_t> lzma_decode(const std::uint8_t* properties, const std::uint8_t* data,
                                      std::size_t size, std::size_t unpacked_size) {
    std::vector<std::uint8_t> output(unpacked_size);
    SizeT output_size = unpacked_size;
    SizeT input_size = size;
    ELzmaStatus status{};
    const auto result = LzmaDecode(output.data(), &output_size, data, &input_size, properties, LZMA_PROPS_SIZE,
                                   LZMA_FINISH_ANY, &status, &g_lzma_alloc);
    if (result != SZ_OK || output_size != unpacked_size) {
        throw std::runtime_error("LZMA data could not be decoded.");
    }
    return output;
}

bool is_valve_lzma(const std::vector<std::uint8_t>& bytes) {
    return bytes.size() >= 17 && std::memcmp(bytes.data(), "LZMA", 4) == 0;
}

std::vector<std::uint8_t> valve_lzma_decode(const std::uint8_t* data, std::size_t size) {
    if (size < 17 || std::memcmp(data, "LZMA", 4) != 0) throw std::runtime_error("Not a Valve LZMA block.");
    std::uint32_t actual = 0, packed = 0;
    std::memcpy(&actual, data + 4, 4);
    std::memcpy(&packed, data + 8, 4);
    if (17 + static_cast<std::size_t>(packed) > size) throw std::runtime_error("Valve LZMA block is truncated.");
    return lzma_decode(data + 12, data + 17, packed, actual);
}

} // namespace gmod::source::vfs
