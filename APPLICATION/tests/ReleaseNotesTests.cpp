#include "ui/ReleaseNotes.h"
#include "ui/MainMenu.h"
#include "ui/NameTags.h"
#include "ui/Updates.h"
#include "ui/Settings.h"
#include "ui/EscMenu.h"
#include "demo/DemoCache.h"
#include "ReleaseNotesData.h"

#include <iostream>
#include <algorithm>
#include <fstream>
#include <winioctl.h>
#include <stdexcept>

void test_menu() {
    {
        const HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = 200;
        info.bmiHeader.biHeight = -200;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) throw std::runtime_error("Could not create scope label test bitmap.");
        const auto old = SelectObject(dc, bitmap);
        gmod::ui::ScopeProjection lens;
        lens.triangles = {{
            gmod::ui::ScopeVertex{{-0.5f,-0.5f,0.5f,1},{0,1}},
            gmod::ui::ScopeVertex{{0.5f,-0.5f,0.5f,1},{1,1}},
            gmod::ui::ScopeVertex{{-0.5f,0.5f,0.5f,1},{0,0}}},
            {gmod::ui::ScopeVertex{{0.5f,-0.5f,0.5f,1},{1,1}},
            gmod::ui::ScopeVertex{{0.5f,0.5f,0.5f,1},{1,0}},
            gmod::ui::ScopeVertex{{-0.5f,0.5f,0.5f,1},{0,0}}}};
        gmod::ui::NameTag tag;
        tag.x = tag.y = 100;
        tag.name = L"Player";
        tag.steam = L"Steam";
        tag.job = L"Police";
        tag.status = L"AIMING";
        const auto painted = [&](const gmod::ui::NameTag& value, const gmod::ui::ScopeProjection& scope) {
            PatBlt(dc, 0, 0, 200, 200, BLACKNESS);
            gmod::ui::NameTags::paint(dc,200,200,{value},scope);
            GdiFlush();
            const auto* pixels = static_cast<const std::uint32_t*>(bits);
            return std::any_of(pixels,pixels+200*200,[](auto pixel) { return (pixel & 0xffffffu) != 0; });
        };
        if (painted(tag,lens)) throw std::runtime_error("Unscoped name text leaked into the scope lens.");
        tag.scoped = true;
        if (!painted(tag,lens)) throw std::runtime_error("Scoped name text was not drawn in the lens.");
        tag.x = 20;
        if (painted(tag,lens)) throw std::runtime_error("Scoped name text leaked outside the scope lens.");
        tag.scoped = false;
        if (!painted(tag,lens)) throw std::runtime_error("Unscoped names outside the lens disappeared.");
        tag.x = 100;
        if (!painted(tag,{})) throw std::runtime_error("Unscoping did not restore normal name text.");
        SelectObject(dc,old);
        DeleteObject(bitmap);
        DeleteDC(dc);
    }
    const HWND window = CreateWindowExW(0, L"STATIC", L"Menu regression", WS_POPUP,
        0, 0, 1264, 681, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) throw std::runtime_error("Could not create UI test window");
    const HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = 1264;
    info.bmiHeader.biHeight = -681;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    const auto old = SelectObject(dc, bitmap);
    gmod::ui::MainMenu menu(REPLAY_SOURCE_ROOT);
    const auto snapshot = [&] {
        menu.draw(window, dc);
        GdiFlush();
        const auto pixels = static_cast<const std::uint32_t*>(bits);
        return std::vector<std::uint32_t>(pixels, pixels + 1264 * 681);
    };
    for (int i = 0; i < 3; ++i) menu.key_down(window, VK_DOWN);
    const auto home = snapshot();
    menu.key_down(window, VK_RETURN);
    const auto updates = snapshot();
    if (home == updates) throw std::runtime_error("Updates panel did not open");
    menu.key_down(window, VK_TAB);
    menu.key_down(window, VK_RETURN);
    const auto expanded = snapshot();
    if (updates == expanded) throw std::runtime_error("Release history did not open inside Updates");
    menu.key_down(window, VK_BACK);
    if (snapshot() != updates) throw std::runtime_error("Backspace did not restore update status");
    menu.mouse_down(window, 810, 75);
    if (snapshot() != expanded) throw std::runtime_error("Release history button did not restore history");
    menu.mouse_down(window, 900, 55);
    if (snapshot() != updates) throw std::runtime_error("Update status button did not restore status");
    menu.key_down(window, VK_TAB);
    menu.key_down(window, VK_RETURN);
    snapshot();
    BYTE keyboard[256]{};
    if (!GetKeyboardState(keyboard)) throw std::runtime_error("Could not read keyboard state");
    const BYTE control_state = keyboard[VK_CONTROL];
    keyboard[VK_CONTROL] |= 0x80;
    if (!SetKeyboardState(keyboard)) throw std::runtime_error("Could not enable Ctrl shortcut state");
    menu.key_down(window, 'A');
    const auto all_selected = snapshot();
    if (all_selected == expanded) throw std::runtime_error("Ctrl+A did not highlight selectable menu text");
    keyboard[VK_CONTROL] = control_state;
    SetKeyboardState(keyboard);
    if (!menu.mouse_down(window, 300, 200)) throw std::runtime_error("Could not begin text selection");
    if (!menu.mouse_move(window, 700, 200)) throw std::runtime_error("Dragging did not change the selection");
    if (!menu.mouse_up(window, 700, 200)) throw std::runtime_error("Mouse-up did not finish text selection");
    if (snapshot() == all_selected) throw std::runtime_error("Dragging did not highlight release-note text");
    menu.mouse_down(window, 400, 145);
    const auto collapsed_card = snapshot();
    if (collapsed_card == expanded) throw std::runtime_error("Mouse accordion did not collapse");
    menu.mouse_down(window, 400, 145);
    const auto expanded_card = snapshot();
    if (expanded_card == collapsed_card) throw std::runtime_error("Mouse accordion did not re-expand");
    menu.key_down(window, VK_RETURN);
    if (snapshot() == expanded_card) throw std::runtime_error("Keyboard accordion did not collapse");
    menu.key_down(window, VK_RETURN);
    if (snapshot() != expanded_card) throw std::runtime_error("Keyboard accordion did not re-expand identically");
    // The public edition's history may hold a single release, which has nothing to scroll to.
    if (gmod::ui::release_data::markdown.size() > 1) {
        menu.mouse_wheel(window, -WHEEL_DELTA * 3);
        const auto scrolled = snapshot();
        if (scrolled == expanded_card) throw std::runtime_error("History did not scroll");
        menu.key_down(window, VK_HOME);
        if (snapshot() != expanded_card) throw std::runtime_error("History Home did not restore scroll position");
        menu.key_down(window, VK_DOWN);
        snapshot();
        menu.key_down(window, VK_RETURN);
        const auto older = snapshot();
        if (older == expanded_card) throw std::runtime_error("Older release did not expand");
    }
    menu.key_down(window, VK_ESCAPE);
    if (menu.finished() || snapshot() != home) throw std::runtime_error("Escape did not return to unchanged home screen");
    const auto update_home = snapshot();
    menu.key_down(window, VK_RETURN);
    if (snapshot() == update_home) throw std::runtime_error("Updates panel did not open");
    menu.key_down(window, VK_RETURN);
    if (menu.finished() || snapshot() != update_home) throw std::runtime_error("In-app Close did not restore home screen");
    SetWindowPos(window, nullptr, 0, 0, 900, 600, SWP_NOMOVE | SWP_NOZORDER);
    const auto small_home = snapshot();
    menu.key_down(window, VK_RETURN);
    if (snapshot() == small_home) throw std::runtime_error("Resized Updates panel did not open");
    menu.mouse_down(window, 760, 55);
    if (snapshot() != small_home) throw std::runtime_error("Scaled in-app Close hit target is incorrect");
    menu.key_down(window, VK_UP);
    menu.key_down(window, VK_UP);
    menu.key_down(window, VK_RETURN);
    if (menu.result().action != gmod::ui::MenuResult::Action::FreeRoam)
        throw std::runtime_error("Free Roam must remain accessible on the home menu");
    const auto original_settings = gmod::ui::settings();
    wchar_t original_appdata[32768]{};
    const auto appdata_length = GetEnvironmentVariableW(L"APPDATA", original_appdata, 32768);
    wchar_t original_localappdata[32768]{};
    const auto localappdata_length = GetEnvironmentVariableW(L"LOCALAPPDATA", original_localappdata, 32768);
    const auto cache_test_root = std::filesystem::temp_directory_path()
        / ("gmod-replay-menu-cache-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(cache_test_root);
    if (!SetEnvironmentVariableW(L"APPDATA", cache_test_root.c_str()))
        throw std::runtime_error("Could not isolate cache menu settings.");
    if (!SetEnvironmentVariableW(L"LOCALAPPDATA", cache_test_root.c_str()))
        throw std::runtime_error("Could not isolate disk cache menu settings.");
    int clears = 0;
    gmod::ui::settings().cache_gpu_assets = false;
    gmod::ui::settings().cache_demos_on_disk = false;
    gmod::ui::settings().demo_cache_gb = 25;
    gmod::ui::MainMenu cache_menu(REPLAY_SOURCE_ROOT, [&] { ++clears; });
    for (int i = 0; i < 4; ++i) cache_menu.key_down(window, VK_DOWN);
    cache_menu.key_down(window, VK_RETURN);
    if (gmod::ui::settings().cache_gpu_assets || cache_menu.finished())
        throw std::runtime_error("Cache settings must open a popup, not toggle caching.");
    cache_menu.draw(window, dc);
    cache_menu.mouse_down(window, 20, 20);
    if (cache_menu.finished()) throw std::runtime_error("Cache popup must isolate background input.");
    cache_menu.key_down(window, VK_RETURN);
    if (!gmod::ui::settings().cache_gpu_assets || cache_menu.finished() || clears != 0)
        throw std::runtime_error("Cache toggle must enable caching without leaving home.");
    gmod::ui::ViewerSettings persisted;
    persisted.load();
    if (!persisted.cache_gpu_assets) throw std::runtime_error("Cache menu choice was not saved.");
    cache_menu.key_down(window, VK_RETURN);
    if (gmod::ui::settings().cache_gpu_assets || clears != 1 || cache_menu.finished())
        throw std::runtime_error("Disabling cache must clear retained GPU assets immediately.");
    cache_menu.key_down(window, VK_DOWN);
    cache_menu.key_down(window, VK_RETURN);
    if (!gmod::ui::settings().cache_demos_on_disk || gmod::ui::settings().cache_gpu_assets)
        throw std::runtime_error("Disk cache must be independently enabled.");
    cache_menu.draw(window, dc);
    cache_menu.mouse_down(window, 120, 225);
    if (gmod::ui::settings().cache_demos_on_disk)
        throw std::runtime_error("Cache popup mouse hit targets must follow the centered layout.");
    cache_menu.mouse_down(window, 120, 225);
    cache_menu.key_down(window, VK_DOWN);
    cache_menu.key_down(window, VK_HOME);
    cache_menu.key_down(window, VK_LEFT);
    if (gmod::ui::settings().demo_cache_gb != 1)
        throw std::runtime_error("Slider keyboard minimum must be 1 GiB.");
    cache_menu.key_down(window, VK_END);
    cache_menu.key_down(window, VK_RIGHT);
    if (gmod::ui::settings().demo_cache_gb != 100)
        throw std::runtime_error("Slider keyboard maximum must be 100 GiB.");
    cache_menu.key_down(window, VK_LEFT);
    cache_menu.key_down(window, VK_PRIOR);
    if (gmod::ui::settings().demo_cache_gb != 89)
        throw std::runtime_error("Slider arrow/page adjustments must use 1/10 GiB steps.");
    cache_menu.key_down(window, VK_NEXT);
    if (gmod::ui::settings().demo_cache_gb != 99)
        throw std::runtime_error("Slider Page Down must increase the limit by 10 GiB.");
    cache_menu.draw(window, dc);
    cache_menu.mouse_down(window, 100, 354);
    cache_menu.mouse_move(window, 1000, 354);
    if (gmod::ui::settings().demo_cache_gb != 99)
        throw std::runtime_error("Dragging must not save or evict cache entries before release.");
    cache_menu.mouse_up(window, 1000, 354);
    if (gmod::ui::settings().demo_cache_gb != 100 || GetCapture() == window)
        throw std::runtime_error("Slider dragging past the right edge must save 100 GiB and release capture.");
    cache_menu.mouse_down(window, 450, 354);
    cache_menu.mouse_up(window, 0, 354);
    if (gmod::ui::settings().demo_cache_gb != 1)
        throw std::runtime_error("Slider dragging past the left edge must save 1 GiB.");
    cache_menu.mouse_down(window, 450, 354);
    cache_menu.key_down(window, VK_ESCAPE);
    cache_menu.mouse_up(window, 450, 354);
    if (gmod::ui::settings().demo_cache_gb != 1 || GetCapture() == window)
        throw std::runtime_error("Escape must cancel a drag without saving or leaving mouse capture active.");
    cache_menu.mouse_down(window, 450, 354);
    cache_menu.cancel_mouse_selection();
    ReleaseCapture();
    cache_menu.mouse_up(window, 450, 354);
    if (gmod::ui::settings().demo_cache_gb != 1)
        throw std::runtime_error("Capture loss must discard an unfinished cache-limit drag.");
    cache_menu.mouse_down(window, 270, 354);
    cache_menu.mouse_up(window, 270, 354);
    persisted.load();
    if (!persisted.cache_demos_on_disk || persisted.demo_cache_gb != 25)
        throw std::runtime_error("User-defined disk cache limit did not persist.");
    SetWindowPos(window, nullptr, 0, 0, 600, 400, SWP_NOMOVE | SWP_NOZORDER);
    cache_menu.draw(window, dc);
    cache_menu.mouse_down(window, 400, 240);
    cache_menu.mouse_up(window, 1000, 240);
    if (gmod::ui::settings().demo_cache_gb != 100)
        throw std::runtime_error("Slider hit targets must follow scaled popup coordinates.");
    cache_menu.key_down(window, VK_HOME);
    for (int i = 0; i < 24; ++i) cache_menu.key_down(window, VK_RIGHT);
    if (gmod::ui::settings().demo_cache_gb != 25)
        throw std::runtime_error("Slider must allow exact whole-GiB keyboard selection.");
    cache_menu.key_down(window, VK_DOWN);
    const auto disk = gmod::demo::DemoCache::default_root();
    std::filesystem::create_directories(disk);
    const auto cached_file = disk / (std::string(64, 'a') + ".cache");
    { std::ofstream file(cached_file); file << "disposable"; }
    const HANDLE sparse = CreateFileW(cached_file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (sparse == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not open sparse cache test file.");
    DWORD returned = 0;
    const bool marked_sparse = DeviceIoControl(sparse, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) != FALSE;
    CloseHandle(sparse);
    if (!marked_sparse) throw std::runtime_error("Could not mark cache test file sparse.");
    std::filesystem::resize_file(cached_file, 1024ull * 1024 * 1024 + 1);
    cache_menu.key_down(window, VK_UP);
    cache_menu.key_down(window, VK_HOME);
    persisted.load();
    if (persisted.demo_cache_gb != 25 || !std::filesystem::exists(cached_file))
        throw std::runtime_error("A destructive limit change must wait for confirmation before saving or deleting.");
    cache_menu.key_down(window, VK_RETURN);
    persisted.load();
    if (persisted.demo_cache_gb != 25 || !std::filesystem::exists(cached_file))
        throw std::runtime_error("Cancel must be selected by default and preserve the previous limit and cache.");
    cache_menu.key_down(window, VK_HOME);
    cache_menu.key_down(window, VK_ESCAPE);
    if (cache_menu.finished() || gmod::ui::settings().demo_cache_gb != 25
        || !std::filesystem::exists(cached_file))
        throw std::runtime_error("Escape must cancel limit confirmation without closing the popup.");
    cache_menu.key_down(window, VK_HOME);
    cache_menu.key_down(window, VK_LEFT);
    cache_menu.key_down(window, VK_RETURN);
    persisted.load();
    if (persisted.demo_cache_gb != 1 || std::filesystem::exists(cached_file))
        throw std::runtime_error("Confirmed limit reduction must persist the new limit and evict excess cache.");
    cache_menu.key_down(window, VK_END);
    cache_menu.draw(window, dc);
    cache_menu.mouse_down(window, 400, 240);
    cache_menu.mouse_up(window, 0, 240);
    if (gmod::ui::settings().demo_cache_gb != 1)
        throw std::runtime_error("Non-destructive slider reductions should not require confirmation.");
    cache_menu.key_down(window, VK_DOWN);
    { std::ofstream file(cached_file); file << "disposable"; }
    cache_menu.key_down(window, VK_RETURN);
    if (!std::filesystem::exists(cached_file))
        throw std::runtime_error("Clearing disk cache must require confirmation.");
    cache_menu.key_down(window, VK_ESCAPE);
    if (!std::filesystem::exists(cached_file) || cache_menu.finished())
        throw std::runtime_error("Cancel must preserve disk cache and keep the popup open.");
    cache_menu.key_down(window, VK_RETURN);
    cache_menu.key_down(window, VK_RETURN);
    if (std::filesystem::exists(cached_file))
        throw std::runtime_error("Confirmed disk cache clear must remove cached entries.");
    cache_menu.key_down(window, VK_ESCAPE);
    if (cache_menu.finished()) throw std::runtime_error("Closing cache settings must return home, not quit.");
    cache_menu.key_down(window, VK_DOWN);
    cache_menu.key_down(window, VK_RETURN);
    if (cache_menu.result().action != gmod::ui::MenuResult::Action::Quit)
        throw std::runtime_error("Quit must remain accessible after the cache option.");
    gmod::ui::settings() = original_settings;
    SetEnvironmentVariableW(L"APPDATA", appdata_length ? original_appdata : nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", localappdata_length ? original_localappdata : nullptr);
    std::filesystem::remove(disk);
    std::filesystem::remove(cache_test_root / "GModReplay" / "settings.ini");
    std::filesystem::remove(cache_test_root / "GModReplay");
    std::filesystem::remove(cache_test_root);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    DestroyWindow(window);
}

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void test_unload_confirmation() {
    const HWND owner = CreateWindowExW(0, L"STATIC", L"Unload regression", WS_POPUP,
        0, 0, 1280, 900, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(owner != nullptr, "Could not create unload test owner.");
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    int exits = 0;
    {
        gmod::ui::EscMenu menu(owner, {{}, [&] { ++exits; }, {}, {}});
        menu.set_visible(true);
        const HWND popup = FindWindowW(L"GModReplayEscMenu", nullptr);
        require(popup != nullptr, "Could not find options popup.");
        const auto click_from_bottom = [&](int offset) {
            RECT client{};
            GetClientRect(popup, &client);
            SendMessageW(popup, WM_LBUTTONDOWN, MK_LBUTTON,
                         MAKELPARAM(220, client.bottom - offset));
        };
        click_from_bottom(79); // Back to main menu, above Quit.
        require(exits == 0, "Back to menu must ask before unloading.");
        click_from_bottom(79); // Cancel, above confirmation.
        require(exits == 0 && menu.visible(), "Cancel must retain the current scene and options.");
        click_from_bottom(79);
        menu.set_visible(false);
        menu.set_visible(true);
        click_from_bottom(79);
        require(exits == 0, "Closing/reopening options must discard pending confirmation.");
        click_from_bottom(39); // Confirm, the bottom button.
        require(exits == 1, "Confirm must unload exactly once.");
    }
    DestroyWindow(owner);
    std::cout << "PASS: unload requires confirmation, Cancel retains scene, closing options discards confirmation\n";
}

int main() {
    try {
        test_unload_confirmation();
        using Kind = gmod::ui::ReleaseNoteBlock::Kind;
        const auto note = gmod::ui::parse_release_note(
            "# Version 1\r\n\r\n## Changes\r\n- **First** item\r\n  continues with `code`.\r\n"
            "- Second item\r\n\r\nParagraph\r\nwrapped.\r\n");
        require(note.title == "Version 1", "Title parsing");
        require(note.blocks.size() == 4, "Block count");
        require(note.blocks[0].kind == Kind::Heading, "Section heading");
        require(note.blocks[1].kind == Kind::Bullet &&
            note.blocks[1].text == "First item continues with code.", "Wrapped bullet and inline formatting");
        require(note.blocks[3].text == "Paragraph wrapped.", "Wrapped paragraph");
        bool rejected = false;
        try { gmod::ui::parse_release_note("Missing title"); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Malformed notes must fail explicitly");
        require(!gmod::ui::release_data::markdown.empty(), "Bundled history must not be empty");
        for (const auto text : gmod::ui::release_data::markdown) {
            const auto bundled = gmod::ui::parse_release_note(text);
            for (const auto& block : bundled.blocks) {
                require(block.text.find('\r') == std::string::npos, "CRLF normalization");
                require(block.text.find('`') == std::string::npos, "No raw inline code markers");
            }
        }
        require(gmod::ui::parse_release_note(gmod::ui::release_data::markdown.front()).title.find(
            gmod::ui::release_data::version.substr(0, gmod::ui::release_data::version.find('-'))) !=
            std::string::npos, "Newest release must be first");
        const auto status = gmod::ui::parse_update_status(
            "status=linking\r\nmessage=Sign%20in%0Anow%21\r\nlogin=Joey%2Btest\r\ncode=ABCD-1234\r\n");
        require(status.at("message") == "Sign in\nnow!" && status.at("login") == "Joey+test", "Status decoding");
        for (const auto invalid : {"status=idle\n", "status=idle\nmessage=%", "status=idle\nmessage=%xy",
                                  "status=idle\nmessage=one\nmessage=two"}) {
            bool failed = false;
            try { gmod::ui::parse_update_status(invalid); }
            catch (const std::runtime_error&) { failed = true; }
            require(failed, "Malformed status must fail explicitly");
        }
        test_menu();
        std::cout << "PASS: release formatting/history, text selection, update IPC, accordion keyboard/mouse, scroll, modal isolation and close\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
