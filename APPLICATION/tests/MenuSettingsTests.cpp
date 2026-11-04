#include "ui/EscMenu.h"
#include "ui/Settings.h"
#include "audio/SoundCategory.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void click(HWND window, int x, int y) {
    SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
}

// Optional review artifacts use the same GDI painter as the live menu.
void preview(gmod::ui::EscMenu& menu, const std::filesystem::path& path) {
    constexpr int width = 640, height = 660;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    require(bitmap != nullptr, "Cannot create menu preview.");
    auto old = SelectObject(dc, bitmap);
    menu.paint_preview(dc, width, height);
    GdiFlush();
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + width * height * 4;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
    out.write(static_cast<const char*>(pixels), width * height * 4);
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    require(out.good(), "Cannot write preview.");
}
}

int main(int argc, char** argv) {
    const auto root = std::filesystem::temp_directory_path()
        / ("gmod-menu-settings-" + std::to_string(GetCurrentProcessId()));
    try {
        // The process exits with its own environment; the user's settings are never touched.
        require(SetEnvironmentVariableW(L"APPDATA", root.c_str()) != FALSE, "Cannot isolate settings.");
        using namespace gmod::audio;
        require(sound_category("VEHICLES\\tdmcars\\bmw1m\\idle.wav") == SoundCategory::Cars, "Engine category.");
        require(sound_category("vehicles/car_siren.wav") == SoundCategory::Alarms, "Vehicle siren priority.");
        require(sound_category("perp2.5/house_alarm_long.mp3", true) == SoundCategory::Alarms, "Map alarm category.");
        require(sound_category("doors/door_open_001.wav") == SoundCategory::Doors, "Door category.");
        require(sound_category("custom/rifle.mp3", false, true) == SoundCategory::Gunshots, "Explicit gunfire category.");
        require(sound_category("ambience/exterior/wind.mp3") == SoundCategory::Ambient, "Recorded ambience category.");
        require(sound_category("custom/wind.mp3", true) == SoundCategory::Ambient, "Soundscape category.");
        require(sound_category("player/footsteps/concrete1.wav") == SoundCategory::Other, "Footstep category.");
        auto& settings = gmod::ui::settings();
        require(!settings.reflections && settings.lighting && !settings.bank_raid_lighting,"Graphics defaults are incorrect.");
        settings.names_through_walls = true; // Legacy independent name setting.
        require(!settings.esp_through_walls(), "Legacy names must respect ESP None.");
        HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1200, 680,
                                     nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(owner != nullptr, "Cannot create menu owner.");
        int changes = 0, resumes = 0;
        {
            gmod::ui::EscMenu menu(owner, {[&] { ++resumes; }, {}, {}, [&] { ++changes; }});
            menu.set_visible(true);
            HWND window = nullptr;
            do {
                window = FindWindowExW(nullptr, window, L"GModReplayEscMenu", nullptr);
            } while (window && GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId());
            require(window != nullptr, "Cannot create menu.");
            const auto capture = [&](const char* name) {
                if (argc > 1) {
                    std::filesystem::create_directories(argv[1]);
                    preview(menu, std::filesystem::path(argv[1]) / name);
                }
            };
            capture("settings.bmp");
            click(window, 200, 110); // Display submenu.
            capture("display-none.bmp");
            click(window, 200, 209);
            require(settings.outline_all_players && !settings.esp_through_walls(), "ESP Outline selection.");
            click(window, 200, 209);
            require(settings.outline_all_players && !settings.esp_through_walls(), "Selected ESP option must remain selected.");
            capture("display-outline.bmp");
            click(window, 200, 234);
            require(settings.esp_through_walls(), "ESP through-walls selection.");
            capture("display-through-walls.bmp");
            click(window, 200, 184);
            require(!settings.outline_all_players && !settings.esp_through_walls(), "ESP None selection.");
            click(window, 200, 234);
            require(settings.esp_through_walls(), "ESP mode must support direct selection from None.");
            click(window, 200, 184);
            click(window, 200, 300); // Back.
            click(window, 200, 230); // Audio submenu.
            RECT rect{};
            GetClientRect(window, &rect);
            require(rect.bottom <= 644, "Audio menu exceeds a 660px client area.");
            for (int row = 0; row < 8; ++row) click(window, 40, 126 + row * 47);
            int* levels[]{&settings.volume, &settings.cars_volume, &settings.voices_volume,
                &settings.gunshots_volume, &settings.doors_volume, &settings.alarms_volume,
                &settings.ambient_volume, &settings.other_volume};
            for (const auto level : levels) require(*level == 90, "Audio row edited the wrong category.");
            for (int i = 0; i < 12; ++i) click(window, 40, 173);
            require(settings.cars_volume == 0 && settings.volume == 90, "Category mute or lower bound failed.");
            for (int i = 0; i < 12; ++i) click(window, 400, 173);
            require(settings.cars_volume == 100, "Category upper bound failed.");
            click(window, 220, 173);
            require(settings.cars_volume == 50, "Clicking the volume track must set its position.");
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(220, 173));
            SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(500, 173));
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(500, 173));
            require(settings.cars_volume == 100, "Dragging beyond the track must clamp to full volume.");
            capture("audio.bmp");
            gmod::ui::ViewerSettings restored;
            restored.load();
            require(restored.volume == 90 && restored.cars_volume == 100 && restored.voices_volume == 90
                && restored.gunshots_volume == 90 && restored.doors_volume == 90 && restored.alarms_volume == 90
                && restored.ambient_volume == 90 && restored.other_volume == 90, "Audio levels did not persist.");
            require(!restored.outline_all_players && !restored.esp_through_walls(), "ESP mode did not persist.");
            click(window, 200, 576); // Resume from Audio.
            require(resumes == 1 && changes > 0, "Menu callbacks failed.");
            menu.set_visible(false);
            menu.set_visible(true);
            click(window, 200, 150); // HUD submenu.
            click(window, 200, 106);
            require(!settings.car_speed, "HUD toggle failed.");
            click(window, 200, 241); // Back.
            click(window, 200, 190); // Effects submenu.
            click(window, 200, 106);
            require(!settings.decals, "Effects toggle failed.");
            menu.set_visible(false);
            menu.set_visible(true);
            click(window,200,270); // Graphics submenu.
            click(window,200,106);
            click(window,200,167);
            click(window,200,228);
            require(settings.reflections && !settings.lighting,"Graphics toggles failed.");
            require(settings.bank_raid_lighting,"Bank lighting toggle failed.");
            capture("graphics.bmp");
            restored.load();
            require(restored.reflections && !restored.lighting,"Graphics settings did not persist.");
            require(restored.bank_raid_lighting,"Bank lighting setting did not persist.");
            click(window,200,295); // Back to settings.
            click(window,200,270);
            click(window,200,106);
            click(window,200,167);
            click(window,200,228);
            require(!settings.reflections && settings.lighting && !settings.bank_raid_lighting,"Graphics toggles could not be restored.");
            menu.set_visible(false);
        }
        DestroyWindow(owner);
        std::filesystem::remove(root / "GModReplay" / "settings.ini");
        std::filesystem::remove(root / "GModReplay");
        std::filesystem::remove(root);
        std::cout << "PASS: submenu navigation, exclusive ESP selection, audio category controls, bounds and persistence\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << " (settings in " << root << ")\n";
        return 1;
    }
}
