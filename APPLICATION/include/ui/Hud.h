#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace gmod::ui {

// One line of the kill and death feed: "victim  (what happened)  other party".
struct FeedLine {
    std::wstring left;
    COLORREF left_color = RGB(255, 255, 255);
    std::wstring action;
    std::wstring right;
    COLORREF right_color = RGB(255, 255, 255);
};

struct HudState {
    bool vehicle_camera = false;
    bool vehicle_free_look = false;
    bool speed_visible = false;
    int speed = 0;
    std::wstring speed_unit = L"km/h";
    std::vector<FeedLine> feed;  // newest first
    std::wstring hint;           // bottom-left tip, empty to hide
    std::wstring toast;          // short centred message ("Radio muted"); empty to hide
    std::wstring spectating;     // Steam name - character name; empty in free view

    bool operator==(const HudState& other) const;
};

// Game-style heads-up display drawn over the scene: speedometer, kill feed,
// control hints. A click-through, non-activating, colour-keyed overlay.
class Hud {
public:
    explicit Hud(HWND owner);
    ~Hud();
    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;

    void set_visible(bool visible);
    void update(const HudState& state);

    // Draws the HUD onto a device context (also used when compositing screenshots).
    static void paint(HDC dc, int width, int height, const HudState& state);

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void place(bool raise = false);

    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    bool visible_ = false;
    int width_ = 0;
    int height_ = 0;
    HudState last_;
    bool painted_ = false;
};

} // namespace gmod::ui
