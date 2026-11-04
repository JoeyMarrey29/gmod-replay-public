#include "ui/MainMenu.h"
#include "app/Edition.h"

#include "source/GameContent.h"
#include "source/png/PngReader.h"
#include "ui/Settings.h"
#include "demo/DemoCache.h"
#include "ReleaseNotesData.h"

#include <commdlg.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <thread>

namespace gmod::ui {
namespace {

constexpr int kItemCount = 6;
const wchar_t* const kItemNames[kItemCount] = {
    L"Open Demo...", L"Free Roam", L"Refresh PERP Assets", L"Updates...", L"Cache Settings...", L"Quit"};
constexpr COLORREF kAccent = RGB(64, 156, 255);

Updates& application_updates(const std::filesystem::path& root) {
    // Keep pending sign-in/download/install tasks alive when playback replaces the menu.
    static Updates updates(root);
    return updates;
}

std::string to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                        size, nullptr, nullptr);
    return result;
}

std::wstring from_utf8(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), ::towlower);
    return text;
}

bool is_folder(const std::filesystem::path& path) {
    std::error_code error;
    return !path.empty() && std::filesystem::is_directory(path, error);
}

// "perpheads_demo_2026-10-14 17-16-05" -> "14 Oct 2026  17:16:05".
std::wstring pretty_title(const std::filesystem::path& path) {
    auto stem = path.stem().wstring();
    if (stem == L"demo" && path.has_parent_path()) {
        stem = path.parent_path().filename().wstring();
    }
    static const std::wregex pattern(LR"((\d{4})-(\d{1,2})-(\d{1,2})[ _](\d{2})-(\d{2})-(\d{2}))");
    std::wsmatch match;
    static const wchar_t* const months[] = {
        L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    if (std::regex_search(stem, match, pattern)) {
        const int month = std::clamp(std::stoi(match[2].str()), 1, 12);
        wchar_t text[96];
        swprintf_s(text, L"%d %s %s  %s:%s:%s", std::stoi(match[3].str()), months[month - 1],
                   match[1].str().c_str(), match[4].str().c_str(), match[5].str().c_str(),
                   match[6].str().c_str());
        return text;
    }
    return path.stem().wstring();
}

void fill_alpha(std::uint32_t* pixels, int width, int height, RECT rect,
                int red, int green, int blue, int alpha) {
    rect.left = std::clamp<LONG>(rect.left, 0, width);
    rect.right = std::clamp<LONG>(rect.right, 0, width);
    rect.top = std::clamp<LONG>(rect.top, 0, height);
    rect.bottom = std::clamp<LONG>(rect.bottom, 0, height);
    for (LONG y = rect.top; y < rect.bottom; ++y) {
        auto* row = pixels + static_cast<std::size_t>(y) * width;
        for (LONG x = rect.left; x < rect.right; ++x) {
            const std::uint32_t p = row[x];
            const int b = (p & 0xFF), g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
            row[x] = (static_cast<std::uint32_t>((r * (255 - alpha) + red * alpha) / 255) << 16)
                   | (static_cast<std::uint32_t>((g * (255 - alpha) + green * alpha) / 255) << 8)
                   | static_cast<std::uint32_t>((b * (255 - alpha) + blue * alpha) / 255);
        }
    }
}

HFONT make_font(int height, int weight, const wchar_t* face = L"Segoe UI") {
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, face);
}

std::wstring size_text(std::uintmax_t bytes) {
    wchar_t text[48];
    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    if (mb >= 1024.0) swprintf_s(text, L"%.2f GB", mb / 1024.0);
    else if (mb >= 1.0) swprintf_s(text, L"%.1f MB", mb);
    else swprintf_s(text, L"%.0f KB", std::max(1.0, mb * 1024.0));
    return text;
}

std::int64_t written_ticks(const std::filesystem::path& path) {
    std::error_code error;
    const auto written = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<std::int64_t>(written.time_since_epoch().count());
}

DemoTreeNode file_node(const std::filesystem::path& path, int depth) {
    DemoTreeNode node;
    node.path = path;
    node.label = path.filename().wstring();
    const auto title = pretty_title(path);
    if (title != path.stem().wstring()) node.subtitle = title;
    else {
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        SYSTEMTIME utc{}, local{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)
            && FileTimeToSystemTime(&attributes.ftLastWriteTime, &utc)
            && SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
            static const wchar_t* const months[] = {
                L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
            wchar_t text[96];
            swprintf_s(text, L"Modified %d %s %04d  %02d:%02d:%02d", local.wDay,
                months[local.wMonth - 1], local.wYear, local.wHour, local.wMinute, local.wSecond);
            node.subtitle = text;
        } else {
            node.subtitle = L"File date unavailable";
        }
    }
    node.depth = depth;
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (!error) node.detail = size_text(bytes);
    node.modified = written_ticks(path);
    return node;
}

void draw_chevron(HDC dc, int x, int y, bool open, COLORREF colour) {
    POINT points[3];
    if (open) {
        points[0] = {x - 5, y - 2};
        points[1] = {x + 5, y - 2};
        points[2] = {x, y + 4};
    } else {
        points[0] = {x - 2, y - 5};
        points[1] = {x - 2, y + 5};
        points[2] = {x + 4, y};
    }
    HBRUSH brush = CreateSolidBrush(colour);
    HPEN pen = CreatePen(PS_SOLID, 1, colour);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    Polygon(dc, points, 3);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

struct WrappedLine {
    std::size_t begin = 0;
    std::size_t end = 0;
};

std::vector<WrappedLine> wrap_text(HDC dc, const std::wstring& text, int width, bool single_line) {
    std::vector<WrappedLine> lines;
    if (text.empty()) {
        lines.push_back({});
        return lines;
    }
    std::size_t begin = 0;
    while (begin < text.size()) {
        if (single_line) {
            lines.push_back({begin, text.size()});
            break;
        }
        while (begin < text.size() && (text[begin] == L' ' || text[begin] == L'\t')) ++begin;
        if (begin == text.size()) break;
        const auto newline = text.find(L'\n', begin);
        const auto limit = newline == std::wstring::npos ? text.size() : newline;
        if (limit == begin) {
            lines.push_back({begin, begin});
            begin = limit + 1;
            continue;
        }
        int fit = 0;
        SIZE measured{};
        GetTextExtentExPointW(dc, text.data() + begin, static_cast<int>(limit - begin),
                              std::max(1, width), &fit, nullptr, &measured);
        fit = std::clamp(fit, 1, static_cast<int>(limit - begin));
        std::size_t end = begin + static_cast<std::size_t>(fit);
        if (end < limit) {
            const auto space = text.find_last_of(L" \t", end - 1);
            if (space != std::wstring::npos && space >= begin) end = space;
        }
        if (end == begin) end = std::min(begin + 1, limit);
        lines.push_back({begin, end});
        begin = end;
        while (begin < limit && (text[begin] == L' ' || text[begin] == L'\t')) ++begin;
        if (begin == limit && newline != std::wstring::npos) ++begin;
    }
    return lines;
}

// Keeps the window painted while `worker` runs; a quit request is passed on afterwards.
void wait_pumping(std::thread& worker) {
    HANDLE handle = worker.native_handle();
    bool quit = false;
    int quit_code = 0;
    while (MsgWaitForMultipleObjects(1, &handle, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit = true;
                quit_code = static_cast<int>(message.wParam);
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    worker.join();
    if (quit) PostQuitMessage(quit_code);
}

// Demos below `folder`, newest first.
std::vector<std::filesystem::path> newest_demos(const std::filesystem::path& folder, std::size_t limit) {
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> found;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(
             folder, std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        std::error_code ignored;
        if (!it->is_regular_file(ignored)) continue;
        auto extension = it->path().extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
        if (extension != L".dem") continue;
        found.emplace_back(it->last_write_time(ignored), it->path());
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::filesystem::path> result;
    for (std::size_t i = 0; i < found.size() && i < limit; ++i) result.push_back(found[i].second);
    return result;
}

// Runs a console tool without a window; its exit code, or -1 when it could not start.
long run_hidden(std::wstring command) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        return -1;
    }
    WaitForSingleObject(process.hProcess, 5 * 60 * 1000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return static_cast<long>(code);
}

// Rebuilds assets\data\perpheads (jobs, weapons, clothing) from the server Lua of the newest demo
// whose gluapack Garry's Mod has downloaded.
std::wstring refresh_perp_data(const std::filesystem::path& root, const std::filesystem::path& demos) {
    const auto parser = root / L"tools" / L"gmoddem.exe";
    if (!std::filesystem::exists(parser)) return L"Data not updated: tools\\gmoddem.exe is missing.";
    const auto candidates = newest_demos(demos, 5);
    if (candidates.empty()) return L"Data not updated: no demos found to read the server's scripts from.";
    const auto out = root / L"assets" / L"data" / L"perpheads";
    for (const auto& demo : candidates) {
        const long code = run_hidden(L"\"" + parser.wstring() + L"\" perp-data \"" + demo.wstring()
                                     + L"\" -o \"" + out.wstring() + L"\"");
        if (code == 0) return L"Jobs, weapons and clothing updated from " + demo.filename().wstring() + L".";
        if (code == -1) return L"Data not updated: could not start gmoddem.exe.";
        if (code != 4) {
            return L"Data not updated: reading " + demo.filename().wstring() + L" failed (code "
                + std::to_wstring(code) + L"; see assets\\data\\perpheads\\perp-data.log).";
        }
    }
    return L"Data not updated: the newest demos' script packs are not downloaded. Join the PERP "
           L"server once, then refresh again.";
}

} // namespace

std::filesystem::path MainMenu::find_gmod_demo_folder() {
    std::vector<std::filesystem::path> installs;
    std::vector<std::filesystem::path> unconfirmed;
    for (const auto& library : source::content::steam_libraries()) {
        const auto gmod = library / L"steamapps" / L"common" / L"GarrysMod";
        if (!is_folder(gmod / L"garrysmod")) continue;
        std::error_code error;
        // appmanifest_4000.acf marks the library Steam actually installed Garry's Mod into.
        if (std::filesystem::exists(library / L"steamapps" / L"appmanifest_4000.acf", error)) {
            installs.push_back(gmod);
        } else {
            unconfirmed.push_back(gmod);
        }
    }
    installs.insert(installs.end(), unconfirmed.begin(), unconfirmed.end());
    for (const auto& gmod : installs) {
        if (is_folder(gmod / L"garrysmod" / L"data" / L"phdemos")) {
            return gmod / L"garrysmod" / L"data" / L"phdemos";
        }
    }
    for (const auto& gmod : installs) {
        if (is_folder(gmod / L"garrysmod" / L"data")) return gmod / L"garrysmod" / L"data";
    }
    return installs.empty() ? std::filesystem::path() : installs.front() / L"garrysmod";
}

struct MainMenu::Layout {
    int width = 0;
    int height = 0;
    RECT items[kItemCount]{};
    RECT list{};
    RECT panel{};
    RECT heading{};
    RECT path_line{};
    RECT buttons[2]{};  // 0 = find GMod, 1 = change folder
    int row_height = 48;
    int indent = 18;
    int visible_rows = 0;
    int left = 72;
};

MainMenu::MainMenu(std::filesystem::path application_root, std::function<void()> clear_gpu_cache)
    : root_(std::move(application_root)), clear_gpu_cache_(std::move(clear_gpu_cache)),
      updates_(application_updates(root_)) {
    const std::filesystem::path chosen = settings().demo_folder;
    if (is_folder(chosen)) {
        folder_ = chosen;
        folder_detected_ = false;
    } else {
        folder_ = find_gmod_demo_folder();
    }
    load_tree();
    load_background();
    for (const auto markdown : release_data::markdown) {
        releases_.push_back(parse_release_note(markdown));
    }
    release_expanded_.resize(releases_.size(), false);
    if (!release_expanded_.empty()) release_expanded_[0] = true;
}

void MainMenu::load_background() {
    const auto path = root_ / L"assets" / L"ui" / L"menu_background.png";
    if (!std::filesystem::exists(path)) {
        return;
    }
    try {
        const auto image = source::png::PngReader(path).read();
        background_width_ = image.width;
        background_height_ = image.height;
        background_.resize(static_cast<std::size_t>(image.width) * image.height);
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                const auto* source = &image.rgba[(static_cast<std::size_t>(y) * image.width + x) * 4];
                // Darken toward the left where the menu sits, and at the edges.
                const float u = static_cast<float>(x) / image.width;
                const float v = static_cast<float>(y) / image.height;
                const float side = 0.30f + 0.55f * std::min(1.0f, u / 0.7f) * std::min(1.0f, u / 0.7f);
                const float edge = 0.75f + 0.25f * std::sin(v * 3.14159265f);
                const float k = side * edge;
                background_[static_cast<std::size_t>(y) * image.width + x] =
                    (static_cast<std::uint32_t>(source[0] * k) << 16)
                    | (static_cast<std::uint32_t>(source[1] * k) << 8)
                    | static_cast<std::uint32_t>(source[2] * k);
            }
        }
    } catch (...) {
        background_.clear();
    }
}

void MainMenu::scan(DemoTreeNode& node) {
    node.scanned = true;
    node.children.clear();
    std::vector<DemoTreeNode> folders;
    std::vector<DemoTreeNode> files;
    std::error_code error;
    for (std::filesystem::directory_iterator it(node.path, error), end; !error && it != end;
         it.increment(error)) {
        std::error_code entry_error;
        const auto& path = it->path();
        if (it->is_directory(entry_error)) {
            if (folders.size() >= 2000) continue;
            DemoTreeNode folder;
            folder.path = path;
            folder.label = path.filename().wstring();
            folder.folder = true;
            folder.depth = node.depth + 1;
            folders.push_back(std::move(folder));
        } else if (lower(path.extension().wstring()) == L".dem" && files.size() < 4000) {
            files.push_back(file_node(path, node.depth + 1));
        }
    }
    std::sort(folders.begin(), folders.end(), [](const DemoTreeNode& a, const DemoTreeNode& b) {
        return lower(a.label) < lower(b.label);
    });
    std::sort(files.begin(), files.end(), [](const DemoTreeNode& a, const DemoTreeNode& b) {
        return a.modified > b.modified;
    });
    node.children = std::move(folders);
    for (auto& file : files) node.children.push_back(std::move(file));
}

void MainMenu::load_tree() {
    tree_.clear();
    rows_.clear();
    message_.clear();
    DemoTreeNode recent;
    recent.label = L"Recent";
    recent.folder = true;
    recent.expanded = true;
    recent.scanned = true;
    std::ifstream list(root_ / L"build" / L"recent_demos.txt");
    for (std::string line; std::getline(list, line) && recent.children.size() < 8;) {
        const std::filesystem::path path = from_utf8(line);
        std::error_code error;
        if (!line.empty() && std::filesystem::is_regular_file(path, error)) {
            recent.children.push_back(file_node(path, 1));
        }
    }
    if (!recent.children.empty()) {
        recent.detail = std::to_wstring(recent.children.size());
        tree_.push_back(std::move(recent));
    }
    if (is_folder(folder_)) {
        DemoTreeNode root;
        root.path = folder_;
        root.depth = -1;
        scan(root);
        for (auto& child : root.children) tree_.push_back(std::move(child));
        if (root.children.empty()) message_ = L"No demos in this folder yet.";
    } else if (folder_.empty()) {
        message_ = L"Couldn't find Garry's Mod on this computer. Use Change folder... to pick "
                   L"where your demos are.";
    } else {
        message_ = L"The demo folder no longer exists. Use Change folder... or Find GMod.";
    }
    scroll_ = 0;
    selected_row_ = 0;
    rebuild_rows();
}

void MainMenu::rebuild_rows() {
    rows_.clear();
    const auto add = [&](auto& self, std::vector<DemoTreeNode>& nodes) -> void {
        for (auto& node : nodes) {
            rows_.push_back(&node);
            if (node.folder && node.expanded) self(self, node.children);
        }
    };
    add(add, tree_);
    selected_row_ = std::clamp(selected_row_, 0, std::max(0, static_cast<int>(rows_.size()) - 1));
    hover_row_ = -1;
}

void MainMenu::toggle(DemoTreeNode& node) {
    if (!node.folder) return;
    if (!node.scanned) scan(node);
    node.expanded = !node.expanded;
    rebuild_rows();
}

void MainMenu::remember(const std::filesystem::path& application_root,
                        const std::filesystem::path& demo) {
    const auto file = application_root / L"build" / L"recent_demos.txt";
    std::vector<std::string> lines{to_utf8(demo.wstring())};
    {
        std::ifstream input(file);
        for (std::string line; std::getline(input, line);) {
            if (!line.empty() && line != lines.front()) lines.push_back(line);
        }
    }
    if (lines.size() > 20) lines.resize(20);
    std::ofstream output(file, std::ios::trunc);
    for (const auto& line : lines) output << line << "\n";
}

MainMenu::Layout MainMenu::layout(HWND window) const {
    RECT client{};
    GetClientRect(window, &client);
    Layout result;
    result.width = std::max<int>(client.right - client.left, 1);
    result.height = std::max<int>(client.bottom - client.top, 1);
    const int item_step = std::min(56, std::max(32, (result.height - 180) / (kItemCount + 1)));
    const int item_top = std::min(result.height * 36 / 100, result.height - 76 - kItemCount * item_step);
    for (int i = 0; i < kItemCount; ++i) {
        result.items[i] = {result.left - 16, item_top + i * item_step, result.left + 330,
                          item_top + i * item_step + item_step - 6};
    }
    const int panel_left = result.width * 53 / 100;
    result.list = {panel_left, 168, result.width - 40, result.height - 56};
    result.panel = {result.list.left - 18, result.list.top - 132, result.list.right + 14,
                    result.list.bottom + 14};
    result.heading = {result.list.left, result.list.top - 116, result.list.right, result.list.top - 88};
    result.buttons[1] = {result.list.right - 150, result.list.top - 120, result.list.right,
                         result.list.top - 90};
    result.buttons[0] = {result.buttons[1].left - 122, result.list.top - 120,
                         result.buttons[1].left - 10, result.list.top - 90};
    result.path_line = {result.list.left, result.list.top - 76, result.list.right, result.list.top - 52};
    result.visible_rows = std::max<int>((result.list.bottom - result.list.top) / result.row_height, 1);
    return result;
}

void MainMenu::clamp_scroll(const Layout& layout) {
    const int rows = static_cast<int>(rows_.size());
    scroll_ = std::clamp(scroll_, 0, std::max(0, rows - layout.visible_rows));
}

void MainMenu::reveal(int row, const Layout& layout) {
    if (row < scroll_) scroll_ = row;
    if (row >= scroll_ + layout.visible_rows) scroll_ = row - layout.visible_rows + 1;
    clamp_scroll(layout);
}

void MainMenu::paint(HWND window) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(window, &paint);
    draw(window, target);
    EndPaint(window, &paint);
}

void MainMenu::draw(HWND window, HDC target) {
    const auto lay = layout(window);
    const int width = lay.width;
    const int height = lay.height;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ previous_bitmap = SelectObject(dc, bitmap);
    auto* pixels = static_cast<std::uint32_t*>(bits);

    if (!background_.empty()) {
        BITMAPINFO source{};
        source.bmiHeader.biSize = sizeof(source.bmiHeader);
        source.bmiHeader.biWidth = background_width_;
        source.bmiHeader.biHeight = -background_height_;
        source.bmiHeader.biPlanes = 1;
        source.bmiHeader.biBitCount = 32;
        source.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, 0, 0, width, height, 0, 0, background_width_, background_height_,
                      background_.data(), &source, DIB_RGB_COLORS, SRCCOPY);
        GdiFlush();
    } else {
        for (int y = 0; y < height; ++y) {
            const float v = static_cast<float>(y) / height;
            const auto r = static_cast<std::uint32_t>(20 - 12 * v);
            const auto g = static_cast<std::uint32_t>(30 - 16 * v);
            const auto b = static_cast<std::uint32_t>(48 - 24 * v);
            for (int x = 0; x < width; ++x) {
                pixels[static_cast<std::size_t>(y) * width + x] = (r << 16) | (g << 8) | b;
            }
        }
    }
    GdiFlush();

    // Everything drawn straight into the pixels goes first, then the GDI text on top.
    fill_alpha(pixels, width, height, lay.panel, 8, 12, 20, 190);
    RECT header_line{lay.panel.left, lay.list.top - 14, lay.panel.right, lay.list.top - 13};
    fill_alpha(pixels, width, height, header_line, 64, 156, 255, 160);
    for (int i = 0; i < 2; ++i) {
        const bool hot = i == hover_button_;
        fill_alpha(pixels, width, height, lay.buttons[i], 64, 156, 255, hot ? 120 : 50);
        const RECT b = lay.buttons[i];
        fill_alpha(pixels, width, height, {b.left, b.bottom - 2, b.right, b.bottom}, 64, 156, 255,
                   hot ? 255 : 150);
    }
    const int row_count = static_cast<int>(rows_.size());
    for (int row = 0; row < lay.visible_rows; ++row) {
        const int index = scroll_ + row;
        if (index >= row_count) break;
        const DemoTreeNode& node = *rows_[static_cast<std::size_t>(index)];
        const int top = lay.list.top + row * lay.row_height;
        RECT r{lay.list.left - 10, top, lay.list.right + 6, top + lay.row_height - 2};
        const bool hot = index == hover_row_ || (focus_list_ && index == selected_row_);
        if (hot) fill_alpha(pixels, width, height, r, 64, 156, 255, 70);
        if (focus_list_ && index == selected_row_) {
            fill_alpha(pixels, width, height, {r.left, r.top, r.left + 3, r.bottom}, 64, 156, 255, 255);
        }
        for (int depth = 0; depth < node.depth; ++depth) {
            const int x = lay.list.left + depth * lay.indent + 7;
            fill_alpha(pixels, width, height, {x, top, x + 1, top + lay.row_height}, 150, 170, 200, 45);
        }
        if (!node.folder) {
            const int x = lay.list.left + node.depth * lay.indent + 3;
            const int y = top + lay.row_height / 2 - 6;
            fill_alpha(pixels, width, height, {x, y, x + 9, y + 11}, 64, 156, 255, 200);
        }
    }
    if (row_count > lay.visible_rows) {
        const int track = lay.list.bottom - lay.list.top;
        const int thumb = std::max(24, track * lay.visible_rows / row_count);
        const int offset = (track - thumb) * scroll_ / std::max(1, row_count - lay.visible_rows);
        RECT bar{lay.list.right + 8, lay.list.top + offset, lay.list.right + 11, lay.list.top + offset + thumb};
        fill_alpha(pixels, width, height, bar, 150, 170, 200, 200);
    }
    for (int i = 0; i < kItemCount; ++i) {
        const bool hot = i == hover_item_ || (!focus_list_ && i == selected_item_);
        if (hot) {
            fill_alpha(pixels, width, height, lay.items[i], 64, 156, 255, 46);
            fill_alpha(pixels, width, height,
                       {lay.items[i].left, lay.items[i].top, lay.items[i].left + 4, lay.items[i].bottom},
                       64, 156, 255, 255);
        }
    }

    SetBkMode(dc, TRANSPARENT);
    HFONT title_font = make_font(64, FW_LIGHT);
    HFONT subtitle_font = make_font(20, FW_NORMAL);
    HFONT item_font = make_font(32, FW_LIGHT);
    HFONT folder_font = make_font(19, FW_SEMIBOLD);
    HFONT file_font = make_font(19, FW_NORMAL);
    HFONT small_font = make_font(16, FW_NORMAL);
    HFONT heading_font = make_font(17, FW_BOLD);
    HGDIOBJ previous_font = SelectObject(dc, title_font);

    SetTextColor(dc, RGB(240, 244, 250));
    RECT line{lay.left - 4, height * 10 / 100, width / 2, height * 10 / 100 + 80};
    DrawTextW(dc, L"GMOD REPLAY", -1, &line, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, subtitle_font);
    SetTextColor(dc, RGB(150, 170, 200));
    line = {lay.left, height * 10 / 100 + 78, width / 2, height * 10 / 100 + 108};
    DrawTextW(dc, L"Demo viewer  -  rp_paralake_city_v6.3", -1, &line, DT_LEFT | DT_SINGLELINE);

    SelectObject(dc, item_font);
    for (int i = 0; i < kItemCount; ++i) {
        const bool hot = i == hover_item_ || (!focus_list_ && i == selected_item_);
        SetTextColor(dc, hot ? RGB(255, 255, 255) : RGB(196, 206, 222));
        RECT text = lay.items[i];
        text.left += 22;
        DrawTextW(dc, kItemNames[i], -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(dc, heading_font);
    SetTextColor(dc, RGB(220, 230, 245));
    RECT heading = lay.heading;
    DrawTextW(dc, L"DEMOS", -1, &heading, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, small_font);
    const wchar_t* const button_names[2] = {L"Find GMod", L"Change folder..."};
    for (int i = 0; i < 2; ++i) {
        SetTextColor(dc, i == hover_button_ ? RGB(255, 255, 255) : RGB(200, 214, 236));
        RECT b = lay.buttons[i];
        DrawTextW(dc, button_names[i], -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SetTextColor(dc, RGB(140, 156, 184));
    std::wstring where = folder_.empty() ? std::wstring(L"Garry's Mod not found")
                                         : folder_.wstring();
    RECT path_rect = lay.path_line;
    RECT source_rect = path_rect;
    const wchar_t* source = folder_detected_ ? L"found automatically" : L"chosen folder";
    DrawTextW(dc, source, -1, &source_rect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    path_rect.right -= 150;
    DrawTextW(dc, where.data(), -1, &path_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_MODIFYSTRING);

    for (int row = 0; row < lay.visible_rows; ++row) {
        const int index = scroll_ + row;
        if (index >= row_count) break;
        const DemoTreeNode& node = *rows_[static_cast<std::size_t>(index)];
        const int top = lay.list.top + row * lay.row_height;
        const int x = lay.list.left + node.depth * lay.indent;
        const int middle = top + (lay.row_height - 2) / 2;
        if (node.folder) {
            draw_chevron(dc, x + 6, middle, node.expanded, RGB(170, 186, 212));
        }
        SelectObject(dc, node.folder ? folder_font : file_font);
        SetTextColor(dc, node.folder ? RGB(214, 224, 240) : RGB(240, 244, 250));
        RECT label{x + 20, top, lay.list.right - 86, top + lay.row_height - 2};
        if (!node.folder) label.bottom = top + 24;
        DrawTextW(dc, node.label.c_str(), -1, &label,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (!node.folder && !node.subtitle.empty()) {
            SelectObject(dc, small_font);
            SetTextColor(dc, RGB(150, 166, 192));
            RECT subtitle{x + 20, top + 24, lay.list.right - 86, top + lay.row_height - 2};
            DrawTextW(dc, node.subtitle.c_str(), -1, &subtitle,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        if (!node.detail.empty()) {
            SelectObject(dc, small_font);
            SetTextColor(dc, RGB(130, 146, 174));
            RECT detail{lay.list.right - 84, top, lay.list.right - 4, top + lay.row_height - 2};
            DrawTextW(dc, node.detail.c_str(), -1, &detail, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }
    if (!message_.empty()) {
        SelectObject(dc, subtitle_font);
        SetTextColor(dc, RGB(170, 184, 208));
        const int top = lay.list.top + std::min(row_count, lay.visible_rows) * lay.row_height + 12;
        RECT note{lay.list.left, top, lay.list.right, top + 80};
        DrawTextW(dc, message_.c_str(), -1, &note, DT_LEFT | DT_WORDBREAK);
    }
    SelectObject(dc, small_font);
    SetTextColor(dc, settings().cache_gpu_assets ? RGB(220, 190, 130) : RGB(150, 170, 200));
    RECT cache_hint{lay.left - 4, height - 80, width / 2 - 12, height - 54};
    DrawTextW(dc, settings().cache_gpu_assets
        ? L"All opened maps and parsed demos retained. RAM/VRAM use can grow."
        : settings().cache_demos_on_disk ? L"Disk demo cache is on. Manage its limit in Cache Settings."
        : L"Cache options are off. Open Cache Settings for faster demo reopening.",
        -1, &cache_hint, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    SetTextColor(dc, RGB(120, 136, 164));
    RECT hint{lay.left - 4, height - 52, width / 2, height - 20};
    DrawTextW(dc, L"Enter open   Up/Down move   Left/Right fold   Tab switch list   Esc quit", -1,
              &hint, DT_LEFT | DT_SINGLELINE);
    RECT credit{width / 2, height - 52, width - 28, height - 20};
    const auto version_credit = L"v" + from_utf8(std::string(release_data::version)) + L"  |  by joeymarrey";
    DrawTextW(dc, version_credit.c_str(), -1, &credit, DT_RIGHT | DT_SINGLELINE | DT_NOPREFIX);

    if (update_log_open_ || updates_open_ || cache_open_) {
        GdiFlush();
        fill_alpha(pixels, width, height, {0, 0, width, height}, 3, 7, 14, 185);
        if (cache_open_) draw_cache(dc, width, height);
        else if (update_log_open_) draw_update_log(dc, width, height);
        else draw_updates(dc, width, height);
    }
    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous_font);
    SelectObject(dc, previous_bitmap);
    DeleteObject(title_font);
    DeleteObject(subtitle_font);
    DeleteObject(item_font);
    DeleteObject(folder_font);
    DeleteObject(file_font);
    DeleteObject(small_font);
    DeleteObject(heading_font);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

void MainMenu::draw_selectable_text(HDC dc, RECT rect, const std::wstring& value,
                                    COLORREF color, int font_height, int font_weight,
                                    bool single_line, bool device_coordinates) {
    SelectableText run;
    run.rect = rect;
    if (device_coordinates) {
        POINT corners[2]{{rect.left, rect.top}, {rect.right, rect.bottom}};
        LPtoDP(dc, corners, 2);
        run.rect = {corners[0].x, corners[0].y, corners[1].x, corners[1].y};
    }
    run.text = value;
    run.begin = selectable_document_.size();
    run.font_height = font_height;
    run.font_weight = font_weight;
    run.single_line = single_line;
    selectable_runs_.push_back(run);
    selectable_document_ += value;
    selectable_document_ += L'\n';

    if (value.empty()) return;
    const HFONT font = make_font(font_height, font_weight);
    const auto previous = SelectObject(dc, font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int line_height = std::max(1, static_cast<int>(metrics.tmHeight));
    const auto lines = wrap_text(dc, value, rect.right - rect.left, single_line);
    const auto selection_first = selection_active_
        ? std::min(selection_start_, selection_end_) : selectable_document_.size();
    const auto selection_last = selection_active_
        ? std::max(selection_start_, selection_end_) : selectable_document_.size();
    SetBkMode(dc, TRANSPARENT);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto line = lines[i];
        const int y = rect.top + static_cast<int>(i) * line_height;
        if (y >= rect.bottom) break;
        const auto line_first = run.begin + line.begin;
        const auto line_last = run.begin + line.end;
        const auto selected_first = std::max(selection_first, line_first);
        const auto selected_last = std::min(selection_last, line_last);
        if (selected_first < selected_last) {
            SIZE before{}, selected{};
            if (selected_first > line_first) {
                GetTextExtentPoint32W(dc, value.data() + line.begin,
                    static_cast<int>(selected_first - line_first), &before);
            }
            GetTextExtentPoint32W(dc, value.data() + (selected_first - run.begin),
                static_cast<int>(selected_last - selected_first), &selected);
            RECT highlight{rect.left + before.cx, y, rect.left + before.cx + selected.cx,
                           std::min(y + line_height, static_cast<int>(rect.bottom))};
            const HBRUSH brush = CreateSolidBrush(RGB(54, 111, 171));
            FillRect(dc, &highlight, brush);
            DeleteObject(brush);
        }
        SetTextColor(dc, color);
        RECT line_rect{rect.left, y, rect.right,
                       std::min(y + line_height, static_cast<int>(rect.bottom))};
        DrawTextW(dc, value.data() + line.begin, static_cast<int>(line.end - line.begin),
                  &line_rect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, previous);
    DeleteObject(font);
}

std::size_t MainMenu::selection_position(HDC dc, int x, int y) const {
    const POINT point{x, y};
    for (auto run = selectable_runs_.rbegin(); run != selectable_runs_.rend(); ++run) {
        if (!PtInRect(&run->rect, point)) continue;
        if (run->single_line) {
            const int midpoint = run->rect.left + (run->rect.right - run->rect.left) / 2;
            return run->begin + (x < midpoint ? 0 : run->text.size());
        }
        const HFONT font = make_font(run->font_height, run->font_weight);
        const auto previous = SelectObject(dc, font);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        const int line_height = std::max(1, static_cast<int>(metrics.tmHeight));
        const auto lines = wrap_text(dc, run->text, run->rect.right - run->rect.left, false);
        const int row = std::clamp((y - static_cast<int>(run->rect.top)) / line_height, 0,
                                   static_cast<int>(lines.size()) - 1);
        const auto line = lines[static_cast<std::size_t>(row)];
        const int relative_x = std::max(0, x - static_cast<int>(run->rect.left));
        std::size_t character = line.begin;
        int previous_width = 0;
        while (character < line.end) {
            SIZE extent{};
            GetTextExtentPoint32W(dc, run->text.data() + line.begin,
                static_cast<int>(character - line.begin + 1), &extent);
            if (relative_x < (previous_width + extent.cx) / 2) break;
            previous_width = extent.cx;
            ++character;
        }
        SelectObject(dc, previous);
        DeleteObject(font);
        return run->begin + character;
    }
    return selectable_document_.size();
}

bool MainMenu::update_selection(int x, int y) {
    if (!selection_dragging_) return false;
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    const auto position = selection_position(dc, x, y);
    ReleaseDC(nullptr, dc);
    if (position == selection_end_) return false;
    selection_end_ = position;
    return true;
}

bool MainMenu::copy_selection() const {
    if (!selection_active_ || selection_start_ == selection_end_) return false;
    const auto first = std::min(selection_start_, selection_end_);
    const auto last = std::min(std::max(selection_start_, selection_end_), selectable_document_.size());
    if (first >= last) return false;
    const auto clipboard_error = [] {
        MessageBeep(MB_ICONWARNING);
        return false;
    };
    if (!OpenClipboard(nullptr)) return clipboard_error();
    const SIZE_T bytes = (last - first) * sizeof(wchar_t) + sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return clipboard_error();
    }
    auto* text = static_cast<wchar_t*>(GlobalLock(memory));
    if (!text) {
        GlobalFree(memory);
        CloseClipboard();
        return clipboard_error();
    }
    std::copy(selectable_document_.begin() + static_cast<std::ptrdiff_t>(first),
              selectable_document_.begin() + static_cast<std::ptrdiff_t>(last), text);
    text[last - first] = L'\0';
    GlobalUnlock(memory);
    if (!EmptyClipboard()) {
        GlobalFree(memory);
        CloseClipboard();
        return clipboard_error();
    }
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return clipboard_error();
    }
    CloseClipboard();
    return true;
}

void MainMenu::draw_update_log(HDC dc, int width, int height) {
    selectable_runs_.clear();
    selectable_document_.clear();
    const int panel_width = std::min(860, width - 40);
    const int panel_height = std::min(700, height - 40);
    const int left = (width - panel_width) / 2;
    const int top = (height - panel_height) / 2;
    const HGDIOBJ previous_font = GetCurrentObject(dc, OBJ_FONT);
    const RECT panel{left, top, left + panel_width, top + panel_height};
    const auto fill = [&](RECT rect, COLORREF color) {
        const HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    };
    const auto rounded = [&](RECT rect, COLORREF color, COLORREF border) {
        const HBRUSH brush = CreateSolidBrush(color);
        const HPEN pen = CreatePen(PS_SOLID, 1, border);
        const auto old_brush = SelectObject(dc, brush);
        const auto old_pen = SelectObject(dc, pen);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 16, 16);
        SelectObject(dc, old_brush);
        SelectObject(dc, old_pen);
        DeleteObject(brush);
        DeleteObject(pen);
    };
    rounded({panel.left + 7, panel.top + 9, panel.right + 7, panel.bottom + 9},
            RGB(4, 8, 14), RGB(4, 8, 14));
    rounded(panel, RGB(19, 28, 43), RGB(64, 84, 114));
    fill({panel.left + 24, panel.top + 19, panel.left + 28, panel.top + 57}, kAccent);
    const HFONT header = make_font(20, FW_SEMIBOLD);
    const HFONT body = make_font(18, FW_NORMAL);
    const HFONT caption_font = make_font(15, FW_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    RECT text{left + 42, top + 17, panel.right - 260, top + 56};
    draw_selectable_text(dc, text, L"Release history", RGB(242, 247, 255), 30,
                         FW_SEMIBOLD, true);
    const auto subtitle = L"Installed " + from_utf8(std::string(release_data::version)) +
        L"   |   " + std::to_wstring(releases_.size()) + L" releases   |   Available offline";
    text = {left + 26, top + 61, panel.right - 26, top + 85};
    draw_selectable_text(dc, text, subtitle, RGB(156, 176, 204), 15,
                         FW_NORMAL, true);
    release_close_ = {panel.right - 91, top + 24, panel.right - 24, top + 54};
    rounded(release_close_, RGB(39, 57, 80), RGB(64, 84, 114));
    SetTextColor(dc, RGB(225, 235, 249));
    text = release_close_;
    DrawTextW(dc, L"Close", -1, &text, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    release_status_ = {panel.right - 245, top + 24, panel.right - 103, top + 54};
    rounded(release_status_, RGB(39, 57, 80), RGB(64, 84, 114));
    text = release_status_;
    SelectObject(dc, caption_font);
    DrawTextW(dc, L"Update status", -1, &text, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    release_viewport_ = {left + 24, top + 101, panel.right - 24, panel.bottom - 48};
    fill({left + 24, top + 92, panel.right - 24, top + 93}, RGB(48, 67, 94));

    std::vector<std::vector<int>> block_heights(releases_.size());
    std::vector<int> heights(releases_.size(), 56);
    const int right = release_viewport_.right - 16;
    for (std::size_t i = 0; i < releases_.size(); ++i) {
        if (!release_expanded_[i]) continue;
        heights[i] += 12;
        for (const auto& block : releases_[i].blocks) {
            const bool heading = block.kind == ReleaseNoteBlock::Kind::Heading;
            const bool bullet = block.kind == ReleaseNoteBlock::Kind::Bullet;
            SelectObject(dc, heading ? header : body);
            const auto content = from_utf8(block.text);
            RECT measure{release_viewport_.left + (bullet ? 40 : 22), 0, right - 22, 0};
            DrawTextW(dc, content.c_str(), -1, &measure, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
            const int block_height = measure.bottom + (heading ? 16 : 10);
            block_heights[i].push_back(block_height);
            heights[i] += block_height;
        }
        heights[i] += 10;
    }
    int total = 0;
    for (const int value : heights) total += value + 10;
    const int viewport_height = release_viewport_.bottom - release_viewport_.top;
    release_scroll_max_ = std::max(0, total - 10 - viewport_height);
    release_scroll_ = std::clamp(release_scroll_, 0, release_scroll_max_);
    release_headers_.resize(releases_.size());
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, release_viewport_.left, release_viewport_.top,
                      release_viewport_.right, release_viewport_.bottom);
    int y = release_viewport_.top - release_scroll_;
    for (std::size_t i = 0; i < releases_.size(); ++i) {
        RECT card{release_viewport_.left, y, right, y + heights[i]};
        rounded(card, RGB(24, 36, 54), i == static_cast<std::size_t>(release_selected_)
            ? kAccent : RGB(43, 61, 85));
        const RECT heading{card.left + 1, y + 1, card.right - 1, y + 55};
        fill(heading, release_expanded_[i] ? RGB(35, 57, 84) : RGB(29, 43, 63));
        release_headers_[i] = heading;
        draw_chevron(dc, card.left + 21, y + 28, release_expanded_[i], kAccent);
        SelectObject(dc, header);
            text = {card.left + 42, y + 15, card.right - 18, y + 40};
            const auto label = from_utf8(releases_[i].title);
            draw_selectable_text(dc, text, label, RGB(229, 239, 255), 20,
                                 FW_SEMIBOLD, true);
        int block_y = y + 68;
        if (release_expanded_[i]) {
            for (std::size_t j = 0; j < releases_[i].blocks.size(); ++j) {
                const auto& block = releases_[i].blocks[j];
                const bool section = block.kind == ReleaseNoteBlock::Kind::Heading;
                const bool bullet = block.kind == ReleaseNoteBlock::Kind::Bullet;
                if (bullet) {
                    fill({card.left + 24, block_y + 8, card.left + 28, block_y + 12}, kAccent);
                }
                const auto content = from_utf8(block.text);
                text = {card.left + (bullet ? 40 : 22), block_y, card.right - 22,
                        block_y + block_heights[i][j]};
                draw_selectable_text(dc, text, content,
                    section ? RGB(116, 189, 255) : RGB(201, 215, 235),
                    section ? 20 : 18, section ? FW_SEMIBOLD : FW_NORMAL);
                block_y += block_heights[i][j];
            }
        }
        y += heights[i] + 10;
    }
    if (release_scroll_max_ > 0) {
        const int thumb = std::max(24, viewport_height * viewport_height / (viewport_height + release_scroll_max_));
        const int offset = (viewport_height - thumb) * release_scroll_ / release_scroll_max_;
        fill({right + 8, release_viewport_.top, right + 11, release_viewport_.bottom}, RGB(34, 49, 70));
        fill({right + 8, release_viewport_.top + offset, right + 11,
              release_viewport_.top + offset + thumb}, RGB(94, 132, 177));
    }
    RestoreDC(dc, saved);
    SelectObject(dc, caption_font);
    SetTextColor(dc, RGB(143, 163, 191));
    text = {left + 26, panel.bottom - 34, panel.right - 24, panel.bottom - 12};
    DrawTextW(dc, L"Drag to select; Ctrl+C copies; Ctrl+A selects all  |  Backspace: update status  |  Esc closes",
              -1, &text, DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, previous_font);
    DeleteObject(header);
    DeleteObject(body);
    DeleteObject(caption_font);
}

bool MainMenu::update_log_key(WPARAM key) {
    const int count = static_cast<int>(releases_.size());
    if (key == VK_BACK) {
        update_action("status-view");
    } else if (key == VK_ESCAPE) {
        update_log_open_ = false;
        selection_active_ = false;
    } else if (key == VK_PRIOR || key == VK_NEXT) {
        const int page = std::max(48L, release_viewport_.bottom - release_viewport_.top - 48);
        release_scroll_ = std::clamp(release_scroll_ + (key == VK_NEXT ? page : -page), 0, release_scroll_max_);
    } else if (key == VK_HOME || key == VK_END) {
        release_scroll_ = key == VK_HOME ? 0 : release_scroll_max_;
    } else if (count > 0 && (key == VK_UP || key == VK_DOWN || key == VK_TAB)) {
        const int step = key == VK_UP || (key == VK_TAB && GetKeyState(VK_SHIFT) < 0) ? -1 : 1;
        release_selected_ = (release_selected_ + step + count) % count;
        if (release_headers_.size() == releases_.size()) {
            const RECT header = release_headers_[static_cast<std::size_t>(release_selected_)];
            if (header.top < release_viewport_.top) release_scroll_ += header.top - release_viewport_.top;
            if (header.bottom > release_viewport_.bottom) release_scroll_ += header.bottom - release_viewport_.bottom;
            release_scroll_ = std::clamp(release_scroll_, 0, release_scroll_max_);
        }
    } else if (count > 0 && (key == VK_RETURN || key == VK_SPACE || key == VK_LEFT || key == VK_RIGHT)) {
        bool expanded = release_expanded_[static_cast<std::size_t>(release_selected_)];
        expanded = key == VK_RIGHT || (key != VK_LEFT && !expanded);
        release_expanded_[static_cast<std::size_t>(release_selected_)] = expanded;
    } else {
        return false;
    }
    return true;
}

bool MainMenu::mouse_move(HWND window, int x, int y) {
    if (busy_) return false;
    if (cache_open_) return cache_slider_dragging_ && move_cache_slider(x);
    if (update_log_open_ || updates_open_) return update_selection(x, y);
    const auto lay = layout(window);
    int item = -1;
    int row = -1;
    int button = -1;
    const POINT point{x, y};
    for (int i = 0; i < kItemCount; ++i) {
        if (PtInRect(&lay.items[i], point)) item = i;
    }
    for (int i = 0; i < 2; ++i) {
        if (PtInRect(&lay.buttons[i], point)) button = i;
    }
    RECT list{lay.list.left - 10, lay.list.top, lay.list.right + 6, lay.list.bottom};
    if (PtInRect(&list, point)) {
        const int visible = (y - lay.list.top) / lay.row_height;
        const int index = scroll_ + visible;
        if (visible < lay.visible_rows && index < static_cast<int>(rows_.size())) row = index;
    }
    if (item == hover_item_ && row == hover_row_ && button == hover_button_) return false;
    hover_item_ = item;
    hover_row_ = row;
    hover_button_ = button;
    return true;
}

bool MainMenu::mouse_down(HWND window, int x, int y) {
    if (busy_) return false;
    if (cache_open_) {
        const POINT point{x, y};
        for (int i = 0; i < 5; ++i) {
            if (!PtInRect(&cache_controls_[i], point)) continue;
            if (cache_limit_confirm_ && i != 3 && i != 4) return false;
            cache_item_ = i;
            if (i == 2) {
                cache_clear_confirm_ = false;
                cache_slider_dragging_ = true;
                SetCapture(window);
                move_cache_slider(x);
                return true;
            }
            cache_action(i);
            return true;
        }
        return false;
    }
    if (updates_open_) {
        const POINT point{x, y};
        for (std::size_t i = 0; i < update_buttons_.size(); ++i) {
            if (PtInRect(&update_buttons_[i].first, point)) {
                update_button_ = static_cast<int>(i);
                update_action(update_buttons_[i].second);
                return true;
            }
        }
        HDC dc = GetDC(nullptr);
        if (dc) {
            const auto position = selection_position(dc, x, y);
            ReleaseDC(nullptr, dc);
            if (position < selectable_document_.size()) {
                selection_start_ = selection_end_ = position;
                selection_dragging_ = selection_active_ = true;
                SetCapture(window);
                return true;
            }
        }
        return false;
    }
    if (update_log_open_) {
        const POINT point{x, y};
        if (PtInRect(&release_status_, point)) {
            update_action("status-view");
            return true;
        }
        if (PtInRect(&release_close_, point)) {
            update_log_open_ = false;
            selection_active_ = false;
            return true;
        }
        if (PtInRect(&release_viewport_, point)) {
            for (std::size_t i = 0; i < release_headers_.size(); ++i) {
                if (PtInRect(&release_headers_[i], point)) {
                    selection_active_ = false;
                    release_selected_ = static_cast<int>(i);
                    release_expanded_[i] = !release_expanded_[i];
                    return true;
                }
            }
            HDC dc = GetDC(nullptr);
            if (dc) {
                const auto position = selection_position(dc, x, y);
                ReleaseDC(nullptr, dc);
                if (position < selectable_document_.size()) {
                    selection_start_ = selection_end_ = position;
                    selection_dragging_ = selection_active_ = true;
                    SetCapture(window);
                    return true;
                }
            }
        }
        return false;
    }
    mouse_move(window, x, y);
    if (hover_item_ >= 0) {
        activate_item(window, hover_item_);
        return true;
    }
    if (hover_button_ == 0) {
        detect_folder();
        return true;
    }
    if (hover_button_ == 1) {
        change_folder(window);
        return true;
    }
    if (hover_row_ >= 0) {
        focus_list_ = true;
        selected_row_ = hover_row_;
        activate_row(hover_row_);
        clamp_scroll(layout(window));
        return true;
    }
    return false;
}

bool MainMenu::mouse_up(HWND window, int x, int y) {
    if (cache_slider_dragging_) {
        move_cache_slider(x);
        cache_slider_dragging_ = false;
        if (GetCapture() == window) ReleaseCapture();
        save_cache_limit();
        return true;
    }
    if (!selection_dragging_) return false;
    update_selection(x, y);
    selection_dragging_ = false;
    if (GetCapture() == window) ReleaseCapture();
    return true;
}

bool MainMenu::cancel_mouse_selection() {
    if (cache_slider_dragging_) {
        cache_slider_dragging_ = false;
        cache_limit_ = settings().demo_cache_gb;
        return true;
    }
    if (!selection_dragging_) return false;
    selection_dragging_ = false;
    return true;
}

bool MainMenu::mouse_wheel(HWND window, int delta) {
    if (busy_) return false;
    if (cache_open_) return false;
    if (updates_open_) return false;
    if (update_log_open_) {
        const int before = release_scroll_;
        release_scroll_ = std::clamp(release_scroll_ - delta * 72 / WHEEL_DELTA, 0, release_scroll_max_);
        return before != release_scroll_;
    }
    const auto lay = layout(window);
    const int before = scroll_;
    scroll_ -= (delta / WHEEL_DELTA) * 3;
    clamp_scroll(lay);
    hover_row_ = -1;
    return scroll_ != before;
}

bool MainMenu::key_down(HWND window, WPARAM key) {
    if (busy_) return false;
    if (cache_open_) {
        if (cache_limit_confirm_) {
            if (key == VK_ESCAPE) cache_action(4);
            else if (key == VK_TAB || key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN)
                cache_item_ = cache_item_ == 3 ? 4 : 3;
            else if (key == VK_RETURN || key == VK_SPACE) cache_action(cache_item_);
            return true;
        }
        if (cache_slider_dragging_) {
            if (key == VK_ESCAPE) {
                cache_slider_dragging_ = false;
                cache_limit_ = settings().demo_cache_gb;
                if (GetCapture() == window) ReleaseCapture();
            }
            return true;
        }
        if (key == VK_ESCAPE) {
            if (cache_clear_confirm_) cache_clear_confirm_ = false;
            else cache_open_ = false;
            return true;
        }
        if (cache_item_ == 2) {
            if (key == VK_LEFT || key == VK_RIGHT || key == VK_HOME || key == VK_END
                || key == VK_PRIOR || key == VK_NEXT) {
                const int step = key == VK_LEFT ? -1 : key == VK_RIGHT ? 1 : key == VK_PRIOR ? -10 : 10;
                cache_limit_ = key == VK_HOME ? ViewerSettings::min_demo_cache_gb
                    : key == VK_END ? ViewerSettings::max_demo_cache_gb
                    : std::clamp(cache_limit_ + step, ViewerSettings::min_demo_cache_gb,
                                 ViewerSettings::max_demo_cache_gb);
                save_cache_limit();
                return true;
            }
        }
        if (key == VK_TAB || key == VK_UP || key == VK_DOWN) {
            const int step = key == VK_UP || (key == VK_TAB && GetKeyState(VK_SHIFT) < 0) ? -1 : 1;
            cache_item_ = (cache_item_ + step + 5) % 5;
            cache_clear_confirm_ = false;
            return true;
        }
        if (key == VK_RETURN || key == VK_SPACE) {
            cache_action(cache_item_);
            return true;
        }
        return false;
    }
    if ((updates_open_ || update_log_open_) && GetKeyState(VK_CONTROL) < 0) {
        if (key == 'A' && !selectable_document_.empty()) {
            selection_start_ = 0;
            selection_end_ = selectable_document_.size();
            selection_active_ = true;
            return true;
        }
        if (key == 'C') return copy_selection();
    }
    if (updates_open_) {
        if (key == VK_ESCAPE) {
            updates_open_ = false;
            return true;
        }
        const int count = static_cast<int>(update_buttons_.size());
        if (count > 0 && (key == VK_TAB || key == VK_DOWN || key == VK_UP)) {
            const int step = key == VK_UP || (key == VK_TAB && GetKeyState(VK_SHIFT) < 0) ? -1 : 1;
            update_button_ = (update_button_ + step + count) % count;
            return true;
        }
        if (count > 0 && (key == VK_RETURN || key == VK_SPACE)) {
            update_action(update_buttons_[static_cast<std::size_t>(update_button_)].second);
            return true;
        }
        return false;
    }
    if (update_log_open_) return update_log_key(key);
    const auto lay = layout(window);
    const int count = static_cast<int>(rows_.size());
    DemoTreeNode* current = focus_list_ && selected_row_ < count
        ? rows_[static_cast<std::size_t>(selected_row_)] : nullptr;
    switch (key) {
    case VK_ESCAPE:
        result_.action = MenuResult::Action::Quit;
        return true;
    case VK_TAB:
        if (count > 0) focus_list_ = !focus_list_;
        if (focus_list_) reveal(selected_row_, lay);
        return true;
    case VK_RIGHT:
        if (!current) {
            if (count > 0) focus_list_ = true;
            reveal(selected_row_, lay);
        } else if (current->folder && !current->expanded) {
            toggle(*current);
        } else if (current->folder && selected_row_ + 1 < count
                   && rows_[static_cast<std::size_t>(selected_row_ + 1)]->depth > current->depth) {
            reveal(++selected_row_, lay);
        }
        return true;
    case VK_LEFT:
        if (!current) return false;
        if (current->folder && current->expanded) {
            toggle(*current);
        } else if (current->depth > 0) {
            int parent = selected_row_ - 1;
            while (parent > 0 && rows_[static_cast<std::size_t>(parent)]->depth >= current->depth) --parent;
            selected_row_ = std::max(parent, 0);
            reveal(selected_row_, lay);
        } else {
            focus_list_ = false;
        }
        return true;
    case VK_UP:
    case VK_DOWN: {
        const int step = key == VK_UP ? -1 : 1;
        if (focus_list_ && count > 0) {
            selected_row_ = std::clamp(selected_row_ + step, 0, count - 1);
            reveal(selected_row_, lay);
        } else {
            selected_item_ = (selected_item_ + step + kItemCount) % kItemCount;
        }
        return true;
    }
    case VK_RETURN:
        if (current) {
            activate_row(selected_row_);
            clamp_scroll(lay);
        } else {
            activate_item(window, selected_item_);
        }
        return true;
    default:
        return false;
    }
}

void MainMenu::activate_item(HWND window, int item) {
    if (item == 0) {
        choose_file(window);
    } else if (item == 1) {
        result_.action = MenuResult::Action::FreeRoam;
    } else if (item == 2) {
        refresh_assets(window);
    } else if (item == 3) {
        updates_open_ = true;
        update_button_ = 0;
        hover_item_ = hover_row_ = hover_button_ = -1;
        if (!updates_.busy()) updates_.start("status");
    } else if (item == 4) {
        cache_open_ = true;
        cache_clear_confirm_ = false;
        cache_limit_confirm_ = false;
        cache_item_ = 0;
        cache_limit_ = settings().demo_cache_gb;
        cache_message_.clear();
        refresh_cache_usage();
    } else {
        result_.action = MenuResult::Action::Quit;
    }
}

void MainMenu::refresh_cache_usage() {
    try {
        const auto usage = demo::DemoCache().usage();
        cache_usage_ = std::to_wstring(usage.entries) + L" recordings  |  "
            + (usage.bytes ? size_text(usage.bytes) : L"0 B")
            + L" used  |  limit " + std::to_wstring(settings().demo_cache_gb) + L" GiB";
    } catch (const std::runtime_error& error) {
        std::cerr << "Could not inspect disk demo cache: " << error.what() << "\n";
        cache_usage_ = L"Disk usage unavailable. Check the application log.";
    }
}

bool MainMenu::move_cache_slider(int x) {
    const int width = cache_slider_.right - cache_slider_.left;
    if (width <= 0) return false;
    const auto fraction = static_cast<double>(std::clamp<LONG>(x, cache_slider_.left, cache_slider_.right)
                                              - cache_slider_.left) / width;
    const auto value = ViewerSettings::min_demo_cache_gb + static_cast<int>(std::lround(fraction
        * (ViewerSettings::max_demo_cache_gb - ViewerSettings::min_demo_cache_gb)));
    if (value == cache_limit_) return false;
    cache_limit_ = value;
    return true;
}

bool MainMenu::save_cache_limit(bool confirmed) {
    const auto old = settings().demo_cache_gb;
    if (cache_limit_ == old) return true;
    const auto limit = static_cast<std::uintmax_t>(cache_limit_) * 1024 * 1024 * 1024;
    if (!confirmed && cache_limit_ < old) {
        try {
            if (demo::DemoCache().usage().bytes > limit) {
                cache_limit_confirm_ = true;
                cache_clear_confirm_ = false;
                cache_item_ = 4;
                cache_message_ = L"Reducing the limit to " + std::to_wstring(cache_limit_)
                    + L" GiB will remove least-recently-used cached recordings. They must be parsed again when reopened. Original demos are safe. Continue?";
                return false;
            }
        } catch (const std::runtime_error& error) {
            std::cerr << "Could not check disk cache before reducing its limit: " << error.what() << "\n";
            cache_limit_ = old;
            cache_message_ = L"Could not check cache usage. Limit unchanged; check the application log.";
            return false;
        }
    }
    settings().demo_cache_gb = cache_limit_;
    if (!settings().save()) {
        settings().demo_cache_gb = old;
        cache_limit_ = old;
        cache_message_ = L"Could not save the disk limit. Check the application log.";
        return false;
    }
    try {
        demo::DemoCache().trim(static_cast<std::uintmax_t>(settings().demo_cache_gb) * 1024 * 1024 * 1024);
        cache_message_ = L"Disk limit saved. Oldest-used recordings are removed first.";
    } catch (const std::runtime_error& error) {
        std::cerr << "Could not enforce disk demo cache limit: " << error.what() << "\n";
        cache_message_ = L"Limit saved, but cleanup failed. Check the application log.";
    }
    refresh_cache_usage();
    return true;
}

void MainMenu::cache_action(int item) {
    if (cache_limit_confirm_) {
        if (item != 3 && item != 4) return;
        cache_limit_confirm_ = false;
        if (item == 3) save_cache_limit(true);
        else {
            cache_limit_ = settings().demo_cache_gb;
            cache_message_ = L"Limit change cancelled. No cached recordings were removed.";
        }
        cache_item_ = 2;
        return;
    }
    if (item == 2) {
        save_cache_limit();
        return;
    }
    if (item == 4) {
        cache_open_ = false;
        return;
    }
    if (item == 3) {
        if (!cache_clear_confirm_) {
            cache_clear_confirm_ = true;
            cache_message_ = L"Clear all disk-cached demos? Original recordings and active playback are not deleted.";
            return;
        }
        cache_clear_confirm_ = false;
        try {
            demo::DemoCache().clear();
            cache_message_ = L"Disk demo cache cleared. Original recordings are unchanged.";
        } catch (const std::runtime_error& error) {
            std::cerr << "Could not clear disk demo cache: " << error.what() << "\n";
            cache_message_ = L"Could not clear the disk cache. Check the application log.";
        }
        refresh_cache_usage();
        return;
    }
    auto& enabled = item == 0 ? settings().cache_gpu_assets : settings().cache_demos_on_disk;
    enabled = !enabled;
    if (!settings().save()) {
        enabled = !enabled;
        cache_message_ = L"Could not save cache settings. Check the application log.";
        return;
    }
    cache_clear_confirm_ = false;
    if (item == 0) {
        if (!enabled && clear_gpu_cache_) clear_gpu_cache_();
        cache_message_ = enabled ? L"Session cache enabled; RAM/VRAM usage can grow until exit."
                                 : L"Session cache disabled; retained maps and parsed data released.";
    } else {
        cache_message_ = enabled ? L"Disk demo cache enabled for future opens."
                                 : L"Disk demo cache disabled. Existing files remain until cleared.";
    }
}

void MainMenu::draw_cache(HDC dc, int width, int height) {
    const int saved = SaveDC(dc);
    const double scale = std::min({1.0, (width - 24) / 780.0, (height - 24) / 510.0});
    const int w = std::max(1, static_cast<int>(780 * scale));
    const int h = std::max(1, static_cast<int>(510 * scale));
    SetMapMode(dc, MM_ANISOTROPIC);
    SetWindowExtEx(dc, 780, 510, nullptr);
    SetViewportExtEx(dc, w, h, nullptr);
    SetViewportOrgEx(dc, (width - w) / 2, (height - h) / 2, nullptr);
    const auto fill = [&](RECT r, COLORREF color) {
        const auto brush = CreateSolidBrush(color);
        FillRect(dc, &r, brush);
        DeleteObject(brush);
    };
    const auto text = [&](RECT r, const std::wstring& value, COLORREF color, int size, int weight = FW_NORMAL) {
        const auto font = make_font(size, weight);
        const auto old = SelectObject(dc, font);
        SetTextColor(dc, color);
        DrawTextW(dc, value.c_str(), -1, &r, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        DeleteObject(font);
    };
    SetBkMode(dc, TRANSPARENT);
    fill({0, 0, 780, 510}, RGB(17, 26, 40));
    fill({0, 0, 780, 3}, kAccent);
    text({28, 20, 750, 62}, L"Cache Settings", RGB(240, 246, 255), 30, FW_SEMIBOLD);
    const RECT rows[5] = {{28, 78, 752, 112}, {28, 167, 752, 201}, {28, 265, 752, 323},
                          {28, 365, 535, 402}, {558, 365, 752, 402}};
    for (int i = 0; i < 5; ++i) {
        fill(rows[i], i == cache_item_ ? RGB(39, 72, 109) : RGB(29, 43, 63));
        POINT points[2]{{rows[i].left, rows[i].top}, {rows[i].right, rows[i].bottom}};
        LPtoDP(dc, points, 2);
        cache_controls_[i] = {points[0].x, points[0].y, points[1].x, points[1].y};
    }
    const auto label = [&](int i, const std::wstring& value) {
        RECT r = rows[i];
        r.left += 12;
        r.top += 5;
        text(r, value, RGB(234, 242, 255), 21, FW_SEMIBOLD);
    };
    label(0, settings().cache_gpu_assets ? L"Session Cache: On" : L"Session Cache: Off");
    text({40, 118, 740, 161}, L"Keep all visited maps, models, textures and parsed demos in RAM/VRAM until exit. Memory use is uncapped.",
         RGB(173, 190, 212), 18);
    label(1, settings().cache_demos_on_disk ? L"Disk Demo Cache: On" : L"Disk Demo Cache: Off");
    text({40, 207, 740, 256}, L"Keep parsed demos and voice on disk across restarts. Content-based matching works after renaming or moving a demo; maps still need to load.",
         RGB(173, 190, 212), 18);
    label(2, L"Disk limit: " + std::to_wstring(cache_limit_) + L" GiB");
    text({345, 272, 740, 300}, L"1 - 100 GiB", RGB(173, 190, 212), 17);
    fill({40, 306, 740, 312}, RGB(72, 91, 115));
    const int thumb = 40 + static_cast<int>(std::lround(700.0
        * (cache_limit_ - ViewerSettings::min_demo_cache_gb)
        / (ViewerSettings::max_demo_cache_gb - ViewerSettings::min_demo_cache_gb)));
    fill({40, 306, thumb, 312}, kAccent);
    fill({thumb - 5, 300, thumb + 6, 319}, RGB(222, 238, 255));
    POINT slider_points[2]{{40, 300}, {740, 319}};
    LPtoDP(dc, slider_points, 2);
    cache_slider_ = {slider_points[0].x, slider_points[0].y, slider_points[1].x, slider_points[1].y};
    text({40, 331, 740, 358}, cache_usage_, RGB(157, 183, 211), 17);
    label(3, cache_limit_confirm_ ? L"Confirm: Reduce Limit" :
        cache_clear_confirm_ ? L"Confirm: Clear Disk Demo Cache" : L"Clear Disk Demo Cache...");
    label(4, cache_limit_confirm_ ? L"Cancel" : L"Close");
    text({28, 415, 752, 494}, cache_message_, RGB(225, 198, 142), 18);
    RestoreDC(dc, saved);
}

void MainMenu::start_updates() {
    if (!std::filesystem::exists(root_ / L"release.json") &&
        !std::filesystem::exists(root_ / L"tools" / L"Update-GModReplay.ps1")) return;
    updates_.start("automatic");
}

bool MainMenu::poll_updates() {
    const bool changed = updates_.poll();
    const bool deferred = update_prompt_deferred_ && !cache_open_;
    if (changed || deferred) {
        const auto status = updates_.value("status");
        if (status == "error" || (status == "ready" && !update_prompt_dismissed_)) {
            update_prompt_deferred_ = cache_open_;
            if (!cache_open_) {
                updates_open_ = true;
                update_log_open_ = false;
                selection_active_ = false;
            }
        } else {
            update_prompt_deferred_ = false;
        }
        if (close_when_install_ready_) {
            if (status == "waiting") {
                close_when_install_ready_ = false;
                result_.action = MenuResult::Action::Quit;
            } else if (status == "error" || status == "ready") {
                close_when_install_ready_ = false;
            }
        }
    }
    return changed || deferred;
}

void MainMenu::update_action(const std::string& action) {
    if (action == "history" || action == "status-view") {
        updates_open_ = action == "status-view";
        update_log_open_ = action == "history";
        selection_active_ = false;
        update_button_ = 0;
    } else if (action == "close") {
        updates_open_ = false;
        if (updates_.value("status") == "ready") update_prompt_dismissed_ = true;
    } else if (action == "install-now") {
        updates_.start("install");
        close_when_install_ready_ = updates_.busy();
    } else if (action == "install-later") {
        updates_.start("install");
    } else if (action == "not-now") {
        updates_open_ = false;
        update_prompt_dismissed_ = true;
    } else if (action == "browser") {
        const auto result = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", L"https://github.com/login/device", nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) updates_.fail("Could not open the browser. Visit https://github.com/login/device manually.");
    } else if (action == "cancel") {
        updates_.cancel();
    } else {
        if (action == "check") update_prompt_dismissed_ = false;
        updates_.start(action);
    }
}

void MainMenu::draw_updates(HDC dc, int width, int height) {
    selectable_runs_.clear();
    selectable_document_.clear();
    const int saved = SaveDC(dc);
    const float scale = std::min({1.0f, width / 820.0f, height / 650.0f});
    SetGraphicsMode(dc, GM_ADVANCED);
    const XFORM transform{scale, 0, 0, scale, 0, 0};
    SetWorldTransform(dc, &transform);
    width = static_cast<int>(width / scale);
    height = static_cast<int>(height / scale);
    const int panel_width = std::min(780, width - 40);
    const int panel_height = std::min(610, height - 40);
    const int x = (width - panel_width) / 2, y = (height - panel_height) / 2;
    const int right = x + panel_width, bottom = y + panel_height;
    const auto fill = [&](RECT rect, COLORREF color) {
        const HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    };
    fill({x + 7, y + 9, right + 7, bottom + 9}, RGB(4, 8, 14));
    fill({x, y, right, bottom}, RGB(19, 28, 43));
    fill({x, y, right, y + 3}, kAccent);
    const HFONT title = make_font(30, FW_SEMIBOLD);
    const HFONT header = make_font(20, FW_SEMIBOLD);
    const HFONT body = make_font(18, FW_NORMAL);
    const HFONT caption_font = make_font(15, FW_NORMAL);
    const auto previous_font = SelectObject(dc, title);
    const auto text = [&](RECT rect, const std::wstring& value, COLORREF color, UINT flags) {
        SetTextColor(dc, color);
        DrawTextW(dc, value.c_str(), -1, &rect, flags | DT_NOPREFIX);
    };
    text({x + 26, y + 19, right - 100, y + 58}, L"Updates", RGB(242, 247, 255), DT_SINGLELINE);
    SelectObject(dc, caption_font);
    text({x + 26, y + 62, right - 26, y + 86},
         L"Installed " + from_utf8(std::string(release_data::version)) + L"   |   "
             + (gmod::edition::is_public ? L"Public releases" : L"Private GitHub releases"),
         RGB(156, 176, 204), DT_SINGLELINE | DT_END_ELLIPSIS);
    update_buttons_.clear();
    const auto button = [&](RECT rect, const wchar_t* label, const std::string& action, bool enabled) {
        const bool selected = static_cast<int>(update_buttons_.size()) == update_button_;
        fill(rect, enabled ? (selected ? RGB(44, 87, 133) : RGB(35, 58, 85)) : RGB(28, 38, 53));
        if (enabled && selected) {
            const HBRUSH border = CreateSolidBrush(kAccent);
            FrameRect(dc, &rect, border);
            DeleteObject(border);
        }
        text(rect, label, enabled ? RGB(228, 239, 255) : RGB(107, 125, 148),
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (enabled) {
            POINT corners[2]{{rect.left, rect.top}, {rect.right, rect.bottom}};
            LPtoDP(dc, corners, 2);
            update_buttons_.push_back({{corners[0].x, corners[0].y, corners[1].x, corners[1].y}, action});
        }
    };
    button({right - 91, y + 25, right - 24, y + 55}, L"Close", "close", true);
    button({right - 255, y + 25, right - 103, y + 55}, L"Release history", "history", true);
    const auto login = updates_.value("login");
    const bool linked = updates_.value("linked") == "True";
    const bool working = updates_.busy();
    const auto status = updates_.value("status");
    fill({x + 24, y + 100, right - 24, y + 166}, RGB(25, 39, 58));
    SelectObject(dc, caption_font);
    if constexpr (gmod::edition::is_public) {
        text({x + 40, y + 109, right - 40, y + 129}, L"AUTOMATIC UPDATES", RGB(142, 166, 200), DT_SINGLELINE);
        SelectObject(dc, header);
        text({x + 40, y + 134, right - 40, y + 161}, L"Enabled by default",
             RGB(230, 239, 254), DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, body);
    } else {
        text({x + 40, y + 109, right - 190, y + 129}, L"GITHUB ACCOUNT", RGB(142, 166, 200), DT_SINGLELINE);
        SelectObject(dc, header);
        text({x + 40, y + 134, right - 190, y + 161},
             !linked ? L"Not linked" : login.empty() ? L"Linked (identity unavailable)" : L"@" + from_utf8(login),
             RGB(230, 239, 254), DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, body);
        button({right - 178, y + 116, right - 40, y + 151},
               linked ? L"Unlink account" : L"Link account", linked ? "unlink" : "link", !working);
    }
    fill({x + 24, y + 177, right - 24, y + 235}, RGB(25, 39, 58));
    text({x + 40, y + 192, right - 40, y + 222},
         gmod::edition::is_public ? L"Checks and downloads on launch; asks before installing" : L"Checks run on launch for linked accounts",
         RGB(221, 231, 247), DT_SINGLELINE | DT_VCENTER);
    if (status == "ready") {
        button({x + 24, y + 250, x + 206, y + 287}, L"Update now", "install-now", !working);
        button({x + 220, y + 250, right - 146, y + 287}, L"Update on app close",
               "install-later", !working);
        button({right - 130, y + 250, right - 24, y + 287}, L"Not now", "not-now", true);
    } else {
        button({x + 24, y + 250, x + 206, y + 287}, L"Check for updates", "check",
               !working && (gmod::edition::is_public || linked));
        text({x + 225, y + 250, right - 24, y + 287},
             working ? L"Working in the background..." : L"Updates install only with your approval.",
             RGB(143, 165, 195), DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
    const bool linking = !gmod::edition::is_public &&
        (status == "linking" || (working && !updates_.value("code").empty()));
    fill({x + 24, y + 304, right - 24, bottom - 72}, RGB(13, 22, 36));
    SelectObject(dc, header);
    const wchar_t* status_title = status == "error" ? L"Update needs attention" :
        status == "ready" ? L"Update ready" : status == "uptodate" ? L"You're up to date" :
        status == "waiting" ? L"Waiting for you to close the viewer" :
        linking ? L"Link your GitHub account" : working ? L"Update service" : L"Update status";
    text({x + 42, y + 318, right - 42, y + 346}, status_title,
         status == "error" ? RGB(255, 163, 145) : RGB(116, 189, 255), DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, body);
    text({x + 42, y + 354, right - 42, linking ? y + 407 : bottom - 90},
         from_utf8(updates_.value("message")), RGB(201, 215, 235), DT_WORDBREAK);
    if (linking) {
        SelectObject(dc, title);
        draw_selectable_text(dc, {x + 42, y + 418, x + 270, y + 454},
             from_utf8(updates_.value("code")), RGB(245, 249, 255), 30,
             FW_SEMIBOLD, true, true);
        SelectObject(dc, body);
        button({x + 280, y + 417, x + 448, y + 453}, L"Open GitHub", "browser", true);
        button({x + 462, y + 417, right - 42, y + 453}, L"Cancel sign-in", "cancel", true);
        SelectObject(dc, caption_font);
        text({x + 42, y + 464, right - 42, bottom - 82},
             L"GitHub limits this sign-in to the GMod Replay repository. It cannot access your other private repositories. "
             L"You must already have access to GMod Replay releases.",
             RGB(155, 176, 204), DT_WORDBREAK);
    } else if (status == "waiting") {
        button({x + 42, bottom - 119, x + 222, bottom - 84}, L"Cancel installation", "cancel", true);
    }
    if (update_button_ >= static_cast<int>(update_buttons_.size())) update_button_ = 0;
    SelectObject(dc, previous_font);
    DeleteObject(title);
    DeleteObject(header);
    DeleteObject(body);
    DeleteObject(caption_font);
    RestoreDC(dc, saved);
}

void MainMenu::activate_row(int row) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    DemoTreeNode& node = *rows_[static_cast<std::size_t>(row)];
    if (node.folder) {
        toggle(node);
        return;
    }
    result_.action = MenuResult::Action::OpenDemo;
    result_.demo = node.path;
}

void MainMenu::detect_folder() {
    settings().demo_folder.clear();
    settings().save();
    folder_ = find_gmod_demo_folder();
    folder_detected_ = true;
    load_tree();
}

void MainMenu::change_folder(HWND window) {
    // The folder picker needs a single-threaded COM apartment, and this thread is
    // multithreaded, so it runs on its own thread while this one keeps the window painted.
    std::wstring chosen;
    const std::wstring start = folder_.wstring();
    busy_ = true;
    std::thread picker([&chosen, start, window] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return;
        IFileOpenDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&dialog)))) {
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            dialog->SetTitle(L"Choose the folder your demos are in");
            IShellItem* initial = nullptr;
            if (!start.empty()
                && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&initial)))) {
                dialog->SetFolder(initial);
                initial->Release();
            }
            if (SUCCEEDED(dialog->Show(window))) {
                IShellItem* result = nullptr;
                if (SUCCEEDED(dialog->GetResult(&result))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        chosen = path;
                        CoTaskMemFree(path);
                    }
                    result->Release();
                }
            }
            dialog->Release();
        }
        CoUninitialize();
    });
    wait_pumping(picker);
    busy_ = false;
    if (chosen.empty()) return;
    folder_ = chosen;
    folder_detected_ = false;
    settings().demo_folder = chosen;
    settings().save();
    load_tree();
    focus_list_ = !rows_.empty();
}

void MainMenu::choose_file(HWND window) {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window;
    dialog.lpstrFilter = L"Garry's Mod demos (*.dem)\0*.dem\0All files\0*.*\0";
    dialog.lpstrFile = file;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrTitle = L"Open demo";
    const auto initial_text = folder_.wstring();
    if (is_folder(folder_)) dialog.lpstrInitialDir = initial_text.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&dialog)) {
        result_.action = MenuResult::Action::OpenDemo;
        result_.demo = file;
    }
}

void MainMenu::refresh_assets(HWND window) {
    if (busy_) return;
    if (clear_gpu_cache_) clear_gpu_cache_();
    try {
        demo::DemoCache().clear();
    } catch (const std::runtime_error& error) {
        std::cerr << "Could not invalidate disk demo cache before asset refresh: " << error.what() << "\n";
        MessageBoxW(window, L"Could not clear the disk demo cache. Asset refresh cancelled; check the application log.",
                    L"GMod Replay", MB_OK | MB_ICONERROR);
        return;
    }
    busy_ = true;
    message_ = L"Refreshing PERP assets from Garry's Mod...";
    InvalidateRect(window, nullptr, FALSE);
    UpdateWindow(window);
    std::wstring summary;
    const auto root = root_;
    const auto folder = folder_;
    std::thread worker([&summary, root, folder] {
        try {
            summary = source::content::refresh_addon_index(root, folder);
        } catch (const std::exception& error) {
            summary = L"Refresh failed: " + from_utf8(error.what());
        }
        try {
            summary += L" " + refresh_perp_data(root, folder);
        } catch (const std::exception& error) {
            summary += L" Data not updated: " + from_utf8(error.what());
        }
    });
    wait_pumping(worker);
    busy_ = false;
    message_ = summary;
    InvalidateRect(window, nullptr, FALSE);
}

} // namespace gmod::ui
