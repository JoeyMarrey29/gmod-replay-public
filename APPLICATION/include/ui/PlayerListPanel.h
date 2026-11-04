#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace gmod::ui {

struct PlayerListRow {
    int id = -1;
    std::wstring name;
    std::wstring detail;  // second line: Steam ID, team, weapon, health, ...
    std::wstring status;  // right-aligned badge text such as "In vehicle"
    bool present = false;
    bool followed = false;
    bool recorder = false;
    bool has_voice = false;  // the demo holds voice clips for this player
    bool isolated = false;   // only this player's voice is currently audible
};

struct PlayerListActions {
    std::function<void(int)> select;   // row id
    std::function<void(int)> isolate;  // row id: toggle hearing only this player's voice
};

// A scrolling scoreboard-style list of the demo's players. It is an owned,
// non-activating overlay so it takes mouse input without stealing keyboard focus.
class PlayerListPanel {
public:
    PlayerListPanel(HWND owner, PlayerListActions actions);
    ~PlayerListPanel();
    PlayerListPanel(const PlayerListPanel&) = delete;
    PlayerListPanel& operator=(const PlayerListPanel&) = delete;

    void set_visible(bool visible);
    bool visible() const noexcept { return visible_; }

    void set_rows(std::vector<PlayerListRow> rows, std::wstring title);
    // Keeps the panel centred over the owner's client area and repaints when needed.
    void update();
    void scroll_by(int wheel_delta);

private:
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    void paint();
    int content_height() const;
    int view_height() const;
    int max_scroll() const;
    int row_at(int y) const;
    void drag_scroll_to(int y);
    void center_on_followed();
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    HFONT title_font_ = nullptr;
    HFONT font_ = nullptr;
    HFONT small_font_ = nullptr;
    PlayerListActions actions_;
    std::vector<PlayerListRow> rows_;
    std::wstring title_;
    bool visible_ = false;
    bool dirty_ = true;
    bool tracking_leave_ = false;
    bool dragging_bar_ = false;
    bool center_pending_ = false;
    int scroll_ = 0;
    int hover_row_ = -1;
    int pressed_id_ = -1;
    bool pressed_isolate_ = false;
    int hover_isolate_ = -1;  // row index whose isolate button is under the cursor
    RECT isolate_rect(int row_top, int list_right) const;
    int placed_x_ = 0;
    int placed_y_ = 0;
    int placed_width_ = 0;
    int placed_height_ = 0;
    DWORD last_paint_ = 0;
};

} // namespace gmod::ui
