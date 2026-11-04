#include "ui/GameConsole.h"

#include <algorithm>

namespace gmod::ui {
namespace {

constexpr wchar_t ClassName[] = L"GModReplayConsole";
constexpr int Pad = 8;
constexpr int MaxLines = 4000;
constexpr COLORREF Background = RGB(28, 28, 28);
constexpr COLORREF InputBackground = RGB(14, 14, 14);

HFONT console_font() {
    static const HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                          CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
                                          L"Lucida Console");
    return font;
}

} // namespace

GameConsole::GameConsole(HWND owner, Executor execute, Completer complete)
    : owner_(owner), execute_(std::move(execute)), complete_(std::move(complete)) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &GameConsole::window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = ClassName;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, ClassName, L"",
                              WS_POPUP, 0, 0, 400, 300, owner_, nullptr, window_class.hInstance,
                              this);
    if (window_) SetLayeredWindowAttributes(window_, 0, 238, LWA_ALPHA);
}

GameConsole::~GameConsole() {
    if (window_) DestroyWindow(window_);
}

void GameConsole::set_visible(bool visible) {
    if (!window_ || visible == visible_) return;
    visible_ = visible;
    if (visible) {
        placed_x_ = -1;
        scroll_ = 0;
        opened_at_ = GetTickCount();
        update();
    } else {
        ShowWindow(window_, SW_HIDE);
    }
}

void GameConsole::print(const std::wstring& text, COLORREF color) {
    // One logical line per newline.
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = text.find(L'\n', start);
        const std::wstring piece = text.substr(start, end == std::wstring::npos ? end : end - start);
        lines_.push_back({piece, color});
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (lines_.size() > MaxLines) {
        lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(lines_.size() - MaxLines));
    }
    dirty_ = true;
}

void GameConsole::clear() {
    lines_.clear();
    scroll_ = 0;
    dirty_ = true;
}

void GameConsole::submit() {
    const std::wstring command = input_;
    input_.clear();
    history_at_ = -1;
    scroll_ = 0;
    if (command.empty()) return;
    if (history_.empty() || history_.back() != command) history_.push_back(command);
    print(L"] " + command, RGB(255, 235, 120));
    if (execute_) execute_(command);
}

bool GameConsole::on_char(wchar_t ch) {
    if (!visible_) return false;
    // The key that opened the console also sends a character; it is never typed.
    if (ch == L'`' || ch == L'\xAC' || ch == L'~') return true;
    if (ch >= L' ' && ch != 127) {
        input_.push_back(ch);
        dirty_ = true;
    }
    return true;
}

bool GameConsole::on_key(WPARAM key) {
    if (!visible_) return false;
    switch (key) {
    case VK_RETURN:
        submit();
        break;
    case VK_BACK:
        if (!input_.empty()) input_.pop_back();
        break;
    case VK_ESCAPE:
        set_visible(false);
        return true;
    case VK_UP:
        if (!history_.empty()) {
            history_at_ = history_at_ < 0 ? static_cast<int>(history_.size()) - 1
                                          : std::max(0, history_at_ - 1);
            input_ = history_[static_cast<std::size_t>(history_at_)];
        }
        break;
    case VK_DOWN:
        if (history_at_ >= 0) {
            history_at_ += 1;
            input_ = history_at_ >= static_cast<int>(history_.size())
                ? std::wstring() : history_[static_cast<std::size_t>(history_at_)];
            if (history_at_ >= static_cast<int>(history_.size())) history_at_ = -1;
        }
        break;
    case VK_PRIOR:
        scroll_ += 10;
        break;
    case VK_NEXT:
        scroll_ = std::max(0, scroll_ - 10);
        break;
    case VK_TAB:
        if (complete_ && !input_.empty()) {
            auto matches = complete_(input_);
            if (matches.size() == 1) {
                input_ = matches.front() + L" ";
            } else if (matches.size() > 1) {
                std::wstring common = matches.front();
                for (const auto& match : matches) {
                    std::size_t same = 0;
                    while (same < common.size() && same < match.size() && common[same] == match[same]) ++same;
                    common.resize(same);
                }
                if (common.size() > input_.size()) input_ = common;
                std::wstring list;
                for (const auto& match : matches) list += match + L"  ";
                print(list, RGB(180, 180, 180));
            }
        }
        break;
    default:
        return true;  // keys are swallowed while the console is open
    }
    dirty_ = true;
    return true;
}

void GameConsole::scroll_by(int wheel_delta) {
    if (!visible_) return;
    scroll_ = std::max(0, scroll_ + (wheel_delta > 0 ? 3 : -3));
    dirty_ = true;
}

void GameConsole::update() {
    if (!window_ || !visible_) return;
    if (IsIconic(owner_)) {
        ShowWindow(window_, SW_HIDE);
        return;
    }
    RECT client{};
    GetClientRect(owner_, &client);
    const int width = client.right - client.left;
    const int height = std::max<int>(260, static_cast<int>((client.bottom - client.top) * 0.46f));
    POINT origin{0, 0};
    ClientToScreen(owner_, &origin);
    if (origin.x != placed_x_ || origin.y != placed_y_ || width != placed_width_
        || height != placed_height_) {
        SetWindowPos(window_, HWND_TOP, origin.x, origin.y, width, height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        placed_x_ = origin.x;
        placed_y_ = origin.y;
        placed_width_ = width;
        placed_height_ = height;
        dirty_ = true;
    }
    if (!IsWindowVisible(window_)) {
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        dirty_ = true;
    }
    // The caret blinks, so the panel repaints twice a second even when idle.
    const bool caret = ((GetTickCount() - opened_at_) / 500) % 2 == 0;
    static bool last_caret = false;
    if (caret != last_caret) {
        last_caret = caret;
        dirty_ = true;
    }
    if (dirty_) {
        dirty_ = false;
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void GameConsole::paint_preview(HDC dc, int client_width, int client_height) {
    paint(dc, client_width, std::max<int>(260, static_cast<int>(client_height * 0.46f)));
}

void GameConsole::paint(HDC dc, int width, int height) {
    RECT all{0, 0, width, height};
    HBRUSH fill = CreateSolidBrush(Background);
    FillRect(dc, &all, fill);
    DeleteObject(fill);
    SelectObject(dc, console_font());
    SetBkMode(dc, TRANSPARENT);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int line_height = metrics.tmHeight + 2;
    const int char_width = std::max<int>(metrics.tmAveCharWidth, 1);
    const int columns = std::max(10, (width - 2 * Pad) / char_width);

    const int input_top = height - line_height - 8;
    RECT input_rect{0, input_top - 4, width, height};
    HBRUSH input_fill = CreateSolidBrush(InputBackground);
    FillRect(dc, &input_rect, input_fill);
    DeleteObject(input_fill);
    HPEN rule = CreatePen(PS_SOLID, 1, RGB(90, 90, 90));
    HGDIOBJ old_pen = SelectObject(dc, rule);
    MoveToEx(dc, 0, input_top - 4, nullptr);
    LineTo(dc, width, input_top - 4);
    SelectObject(dc, old_pen);
    DeleteObject(rule);

    // Newest line at the bottom; wrapped continuation rows count towards the scroll.
    int y = input_top - 8 - line_height;
    int skip = scroll_;
    for (std::size_t index = lines_.size(); index-- > 0 && y > -line_height;) {
        const auto& line = lines_[index];
        const int rows = std::max<int>(1, static_cast<int>((line.text.size() + columns - 1) / columns));
        for (int row = rows - 1; row >= 0; --row) {
            if (skip > 0) {
                --skip;
                continue;
            }
            const std::wstring piece = line.text.substr(
                static_cast<std::size_t>(row) * columns, static_cast<std::size_t>(columns));
            SetTextColor(dc, line.color);
            TextOutW(dc, Pad, y, piece.c_str(), static_cast<int>(piece.size()));
            y -= line_height;
            if (y < -line_height) break;
        }
    }

    SetTextColor(dc, RGB(255, 235, 120));
    const std::wstring prompt = L"] " + input_;
    TextOutW(dc, Pad, input_top, prompt.c_str(), static_cast<int>(prompt.size()));
    if (((GetTickCount() - opened_at_) / 500) % 2 == 0) {
        SIZE size{};
        GetTextExtentPoint32W(dc, prompt.c_str(), static_cast<int>(prompt.size()), &size);
        RECT caret{Pad + size.cx + 1, input_top + 2, Pad + size.cx + 1 + char_width / 2 + 2, input_top + line_height - 2};
        HBRUSH caret_fill = CreateSolidBrush(RGB(255, 235, 120));
        FillRect(dc, &caret, caret_fill);
        DeleteObject(caret_fill);
    }
    if (scroll_ > 0) {
        SetTextColor(dc, RGB(150, 150, 150));
        const std::wstring more = L"-- scrolled (PgDn) --";
        TextOutW(dc, width - Pad - static_cast<int>(more.size()) * char_width, input_top - line_height - 6,
                 more.c_str(), static_cast<int>(more.size()));
    }
}

LRESULT GameConsole::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEWHEEL:
        scroll_by(GET_WHEEL_DELTA_WPARAM(wparam));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint_info{};
        HDC target = BeginPaint(window_, &paint_info);
        RECT client{};
        GetClientRect(window_, &client);
        HDC dc = CreateCompatibleDC(target);
        HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
        HGDIOBJ previous = SelectObject(dc, bitmap);
        paint(dc, client.right, client.bottom);
        BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, previous);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(window_, &paint_info);
        return 0;
    }
    default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
}

LRESULT CALLBACK GameConsole::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    GameConsole* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<GameConsole*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->window_ = window;
    } else {
        self = reinterpret_cast<GameConsole*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (self) return self->handle(message, wparam, lparam);
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace gmod::ui
