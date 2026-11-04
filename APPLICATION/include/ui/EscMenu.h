#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace gmod::ui {

struct EscMenuActions {
    std::function<void()> resume;
    std::function<void()> main_menu;
    std::function<void()> quit;
    std::function<void()> changed;  // a setting was edited
};

// The pause-style options menu (Esc): category submenus, audio levels and ways
// out of the viewer. An owned, non-activating window, so keyboard focus
// stays on the viewer.
class EscMenu {
public:
    EscMenu(HWND owner, EscMenuActions actions);
    ~EscMenu();
    EscMenu(const EscMenu&) = delete;
    EscMenu& operator=(const EscMenu&) = delete;

    void set_visible(bool visible);
    bool visible() const noexcept { return visible_; }
    // Opens straight onto the list of controls (F1).
    void show_help();
    // Keeps the menu centred over the owner's client area.
    void update();
    // Draws the menu centred in a client area of the given size (for screenshots).
    void paint_preview(HDC dc, int client_width, int client_height);
    // Makes paint_preview draw the controls page.
    void preview_help();

private:
    struct Item;
    std::vector<Item> layout(int width, int& height) const;
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    void paint(HDC dc, int width, int height);
    void activate(const Item& item);
    void set_drag_level(int x);
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    EscMenuActions actions_;
    bool visible_ = false;
    bool confirm_unload_ = false;
    int page_ = 0; // 0: categories, 1: Display, 2: HUD, 3: Effects, 4: Audio, 5: Graphics, 6: Controls
    int* drag_level_ = nullptr;
    RECT drag_rect_{};
    bool tracking_leave_ = false;
    int hover_ = -1;
    int placed_x_ = -1;
    int placed_y_ = -1;
    int placed_height_ = 0;
};

} // namespace gmod::ui
