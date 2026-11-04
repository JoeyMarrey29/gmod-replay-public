#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace gmod::ui {

// The Garry's Mod style developer console: a translucent panel over the top of the view with
// the scrolling log and an input line. It never takes focus; the viewer's window forwards the
// keyboard to it while it is open.
class GameConsole {
public:
    using Executor = std::function<void(const std::wstring& command)>;
    using Completer = std::function<std::vector<std::wstring>(const std::wstring& prefix)>;

    GameConsole(HWND owner, Executor execute, Completer complete);
    ~GameConsole();
    GameConsole(const GameConsole&) = delete;
    GameConsole& operator=(const GameConsole&) = delete;

    void set_visible(bool visible);
    bool visible() const noexcept { return visible_; }
    void toggle() { set_visible(!visible_); }

    void print(const std::wstring& text, COLORREF color = RGB(255, 255, 255));
    void clear();

    // Keyboard input forwarded by the viewer window. Both return true when consumed.
    bool on_char(wchar_t ch);
    bool on_key(WPARAM key);
    void scroll_by(int wheel_delta);

    // Re-places the panel and repaints when something changed.
    void update();
    // Draws the console as it would look over a client area of this size (for screenshots).
    void paint_preview(HDC dc, int client_width, int client_height);

private:
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    void paint(HDC dc, int width, int height);
    void submit();
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    struct Line {
        std::wstring text;
        COLORREF color;
    };

    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    Executor execute_;
    Completer complete_;
    std::vector<Line> lines_;
    std::vector<std::wstring> history_;
    int history_at_ = -1;
    std::wstring input_;
    bool visible_ = false;
    bool dirty_ = true;
    int scroll_ = 0;  // lines scrolled up from the newest
    int placed_x_ = -1;
    int placed_y_ = -1;
    int placed_width_ = 0;
    int placed_height_ = 0;
    DWORD opened_at_ = 0;
};

} // namespace gmod::ui
