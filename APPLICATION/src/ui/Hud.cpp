#include "ui/Hud.h"

#include <algorithm>
#include <cmath>

namespace gmod::ui {
namespace {

constexpr wchar_t ClassName[] = L"GModReplayHud";
constexpr COLORREF KeyColor = RGB(2, 1, 3);

HFONT make_font(int height, int weight) {
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
}

SIZE extent(HDC dc, const std::wstring& text) {
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    return size;
}

void outlined(HDC dc, const std::wstring& text, int left, int top, COLORREF color) {
    SetTextColor(dc, RGB(0, 0, 0));
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            if (dx != 0 || dy != 0) {
                TextOutW(dc, left + dx, top + dy, text.c_str(), static_cast<int>(text.size()));
            }
        }
    }
    SetTextColor(dc, color);
    TextOutW(dc, left, top, text.c_str(), static_cast<int>(text.size()));
}

void box(HDC dc, const RECT& rect, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
    HBRUSH frame = CreateSolidBrush(border);
    FrameRect(dc, &rect, frame);
    DeleteObject(frame);
}

} // namespace

bool HudState::operator==(const HudState& other) const {
    if (vehicle_camera != other.vehicle_camera || vehicle_free_look != other.vehicle_free_look
        || speed_visible != other.speed_visible || speed != other.speed || speed_unit != other.speed_unit
        || hint != other.hint
        || toast != other.toast || feed.size() != other.feed.size()) {
        return false;
    }
    for (std::size_t index = 0; index < feed.size(); ++index) {
        const auto& a = feed[index];
        const auto& b = other.feed[index];
        if (a.left != b.left || a.left_color != b.left_color || a.action != b.action
            || a.right != b.right || a.right_color != b.right_color) {
            return false;
        }
    }
    return true;
}

Hud::Hud(HWND owner) : owner_(owner) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &Hud::window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = ClassName;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = CreateSolidBrush(KeyColor);
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, ClassName, L"",
        WS_POPUP, 0, 0, 16, 16, owner_, nullptr, window_class.hInstance, nullptr);
    if (window_) SetLayeredWindowAttributes(window_, KeyColor, 0, LWA_COLORKEY);
}

Hud::~Hud() {
    if (window_) DestroyWindow(window_);
}

void Hud::set_visible(bool visible) {
    if (!window_ || visible == visible_) return;
    visible_ = visible;
    if (visible) {
        place(true);
        painted_ = false;
        ShowWindow(window_, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(window_, SW_HIDE);
    }
}

void Hud::place(bool raise) {
    RECT client{};
    GetClientRect(owner_, &client);
    POINT origin{0, 0};
    ClientToScreen(owner_, &origin);
    width_ = client.right - client.left;
    height_ = client.bottom - client.top;
    // Only raised when it first appears, leaving later player lists and panels above it.
    SetWindowPos(window_, raise ? HWND_TOP : nullptr, origin.x, origin.y, width_, height_,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | (raise ? 0 : SWP_NOZORDER));
}

void Hud::update(const HudState& state) {
    if (!window_ || !visible_ || IsIconic(owner_)) return;
    const int old_width = width_;
    const int old_height = height_;
    place();
    if (painted_ && old_width == width_ && old_height == height_ && state == last_) return;
    last_ = state;
    painted_ = true;

    HDC target = GetDC(window_);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width_, height_);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    RECT all{0, 0, width_, height_};
    HBRUSH key = CreateSolidBrush(KeyColor);
    FillRect(dc, &all, key);
    DeleteObject(key);
    paint(dc, width_, height_, last_);
    BitBlt(target, 0, 0, width_, height_, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(window_, target);
}

void Hud::paint(HDC dc, int width, int height, const HudState& state) {
    static const HFONT small_font = make_font(12, FW_NORMAL);
    static const HFONT feed_font = make_font(13, FW_BOLD);
    static const HFONT speed_font = make_font(40, FW_BOLD);
    static const HFONT unit_font = make_font(15, FW_BOLD);
    SetBkMode(dc, TRANSPARENT);


    // Kill and death feed, top right.
    SelectObject(dc, feed_font);
    int y = 14;
    for (const auto& line : state.feed) {
        const SIZE left = extent(dc, line.left);
        const SIZE action = extent(dc, line.action);
        const SIZE right = extent(dc, line.right);
        const int gap = 8;
        int total = left.cx + gap + action.cx;
        if (!line.right.empty()) total += gap + right.cx;
        const int pad = 8;
        const RECT rect{width - 16 - total - 2 * pad, y, width - 16, y + 24};
        box(dc, rect, RGB(18, 20, 26), RGB(90, 40, 40));
        int x = rect.left + pad;
        const int top = y + (24 - left.cy) / 2;
        SetTextColor(dc, line.left_color);
        TextOutW(dc, x, top, line.left.c_str(), static_cast<int>(line.left.size()));
        x += left.cx + gap;
        SetTextColor(dc, RGB(200, 200, 205));
        TextOutW(dc, x, top, line.action.c_str(), static_cast<int>(line.action.size()));
        x += action.cx + gap;
        if (!line.right.empty()) {
            SetTextColor(dc, line.right_color);
            TextOutW(dc, x, top, line.right.c_str(), static_cast<int>(line.right.size()));
        }
        y += 28;
    }

    // Speedometer, bottom right.
    if (state.speed_visible) {
        const RECT rect{width - 190, height - 150, width - 24, height - 60};
        box(dc, rect, RGB(18, 20, 26), RGB(70, 140, 220));
        SelectObject(dc, speed_font);
        const std::wstring digits = std::to_wstring(state.speed);
        const SIZE size = extent(dc, digits);
        SetTextColor(dc, RGB(240, 240, 240));
        TextOutW(dc, rect.left + (rect.right - rect.left - size.cx) / 2, rect.top + 8,
                 digits.c_str(), static_cast<int>(digits.size()));
        SelectObject(dc, unit_font);
        const SIZE unit = extent(dc, state.speed_unit);
        SetTextColor(dc, RGB(130, 180, 235));
        TextOutW(dc, rect.left + (rect.right - rect.left - unit.cx) / 2, rect.bottom - 26,
                 state.speed_unit.c_str(), static_cast<int>(state.speed_unit.size()));
    }

    if (state.vehicle_camera) {
        SelectObject(dc,small_font);
        const std::wstring label=state.vehicle_free_look ? L"FreeLook: ON" : L"FreeLook: OFF";
        outlined(dc,label,width-156,height-176,RGB(225,230,240));
    }

    if (!state.hint.empty()) {
        SelectObject(dc, small_font);
        outlined(dc, state.hint, 14, height - 28, RGB(220, 220, 225));
    }

    if (!state.toast.empty()) {
        SelectObject(dc, feed_font);
        const SIZE size = extent(dc, state.toast);
        const RECT rect{(width - size.cx) / 2 - 14, height / 4 - 4, (width + size.cx) / 2 + 14,
                        height / 4 + size.cy + 6};
        box(dc, rect, RGB(18, 20, 26), RGB(70, 140, 220));
        SetTextColor(dc, RGB(240, 240, 240));
        TextOutW(dc, rect.left + 14, rect.top + 5, state.toast.c_str(),
                 static_cast<int>(state.toast.size()));
    }
}

LRESULT CALLBACK Hud::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace gmod::ui
