#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace gmod::ui {

// What the bar shows; pushed by the owner every frame.
struct TimelineState {
    float time = 0.0f;
    float duration = 0.0f;
    bool playing = false;
    bool reverse = false;
    int speed_index = 0;
    std::wstring follow;  // name of the followed player, empty when free-roaming
    int real_start = -1;  // local time of day the recording began, in seconds; -1 = unknown
};

// What the bar can ask the owner to do. All callbacks run on the UI thread.
struct TimelineActions {
    std::function<void()> play;
    std::function<void()> pause;
    std::function<void()> reverse;
    std::function<void(int)> set_speed;
    std::function<void(float)> skip;   // relative seconds
    std::function<void(float)> seek;   // absolute seconds
    std::function<void(bool)> scrub;   // true when a drag starts, false when it ends
};

// A YouTube-style control bar: a seek track above transport buttons, the clock
// and speed presets. It is an owned, non-activating overlay window so it takes
// mouse input without stealing keyboard focus from the viewer.
class TimelineBar {
public:
    TimelineBar(HWND owner, std::vector<std::wstring> speed_labels, TimelineActions actions);
    ~TimelineBar();
    TimelineBar(const TimelineBar&) = delete;
    TimelineBar& operator=(const TimelineBar&) = delete;

    void set_visible(bool visible);
    bool visible() const noexcept { return visible_; }

    // Call every frame: keeps the bar glued to the owner's client area and
    // repaints (rate-limited) when the state changed.
    void update(const TimelineState& state);
    // Draws the bar along the bottom of `dc` (headless screenshots).
    void paint_preview(HDC dc, int width, int height);

    static constexpr int Height = 88;

private:
    struct Layout;
    Layout layout(int width) const;
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    void paint();
    void draw(HDC dc, int width);
    void mouse_move(int x, int y);
    void mouse_down(int x, int y);
    void mouse_up(int x, int y);
    float time_at(int x) const;
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    HFONT small_font_ = nullptr;
    std::vector<std::wstring> speed_labels_;
    TimelineActions actions_;
    TimelineState state_;
    bool visible_ = false;
    bool dirty_ = true;
    bool tracking_leave_ = false;
    bool scrubbing_ = false;
    int hover_x_ = -1;
    int hover_y_ = -1;
    int pressed_button_ = -1;
    DWORD last_paint_ = 0;
    int placed_x_ = 0;
    int placed_y_ = 0;
    int placed_width_ = 0;
};

} // namespace gmod::ui
