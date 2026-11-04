#include "ui/TimelineBar.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace gmod::ui {
namespace {

constexpr wchar_t ClassName[] = L"GmodReplayTimelineBar";
constexpr int Margin = 16;
constexpr int TrackTop = 22;
constexpr int TrackBottom = 44;
constexpr int ButtonTop = 48;
constexpr int ButtonHeight = 34;
constexpr int ButtonWidth = 46;
constexpr int ButtonGap = 6;
constexpr int ChipGap = 4;
constexpr int NoHit = -1;
constexpr int TrackHit = 10;
constexpr int ChipBase = 100;

enum Button { SkipBack = 0, Reverse = 1, PlayPause = 2, SkipForward = 3, ButtonCount = 4 };

constexpr COLORREF Background = RGB(16, 18, 24);
constexpr COLORREF Divider = RGB(48, 52, 64);
constexpr COLORREF TrackColor = RGB(78, 84, 98);
constexpr COLORREF PlayedColor = RGB(232, 62, 62);
constexpr COLORREF Hover = RGB(52, 57, 70);
constexpr COLORREF Text = RGB(236, 238, 244);
constexpr COLORREF MutedText = RGB(150, 156, 172);

std::wstring clock_text(float seconds) {
    const int total = static_cast<int>(std::max(seconds, 0.0f));
    wchar_t text[32];
    if (total >= 3600) {
        swprintf_s(text, L"%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    } else {
        swprintf_s(text, L"%d:%02d", total / 60, total % 60);
    }
    return text;
}

// Wall-clock time at a demo time, from the time of day the recording started.
std::wstring real_clock_text(int start, float seconds) {
    if (start < 0) return {};
    const int total = (start + static_cast<int>(std::max(seconds, 0.0f))) % 86400;
    wchar_t text[16];
    swprintf_s(text, L"%02d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
    return text;
}

void fill(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void rounded(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void disc(HDC dc, int cx, int cy, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    Ellipse(dc, cx - radius, cy - radius, cx + radius + 1, cy + radius + 1);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void triangle(HDC dc, int cx, int cy, int half, bool pointing_right, COLORREF color) {
    const int tip = pointing_right ? cx + half : cx - half;
    const int back = pointing_right ? cx - half : cx + half;
    const POINT points[3]{{tip, cy}, {back, cy - half - 1}, {back, cy + half + 1}};
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    Polygon(dc, points, 3);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void text_in(HDC dc, const std::wstring& text, RECT rect, UINT flags, COLORREF color) {
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
              flags | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

bool inside(const RECT& rect, int x, int y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

} // namespace

struct TimelineBar::Layout {
    RECT track{};
    RECT buttons[ButtonCount]{};
    RECT clock{};
    RECT follow{};
    std::vector<RECT> chips;
};

TimelineBar::Layout TimelineBar::layout(int width) const {
    Layout result;
    result.track = {Margin, TrackTop, std::max(width - Margin, Margin + 1), TrackBottom};
    int x = Margin;
    for (auto& button : result.buttons) {
        button = {x, ButtonTop, x + ButtonWidth, ButtonTop + ButtonHeight};
        x += ButtonWidth + ButtonGap;
    }
    result.clock = {x + 10, ButtonTop, x + (state_.real_start >= 0 ? 270 : 160), ButtonTop + ButtonHeight};

    const int count = static_cast<int>(speed_labels_.size());
    if (count > 0) {
        const int available = std::max<int>(
            width - Margin - static_cast<int>(result.clock.right) - 220, count * 30);
        const int chip_width = std::clamp((available - (count - 1) * ChipGap) / count, 30, 50);
        const int total = count * chip_width + (count - 1) * ChipGap;
        int chip_x = width - Margin - total;
        for (int index = 0; index < count; ++index) {
            result.chips.push_back(
                {chip_x, ButtonTop, chip_x + chip_width, ButtonTop + ButtonHeight});
            chip_x += chip_width + ChipGap;
        }
        result.follow = {result.clock.right + 8, ButtonTop,
                         std::max<int>(result.chips.front().left - 12, result.clock.right + 8),
                         ButtonTop + ButtonHeight};
    } else {
        result.follow = {result.clock.right + 8, ButtonTop, width - Margin, ButtonTop + ButtonHeight};
    }
    return result;
}

TimelineBar::TimelineBar(HWND owner, std::vector<std::wstring> speed_labels,
                         TimelineActions actions)
    : owner_(owner), speed_labels_(std::move(speed_labels)), actions_(std::move(actions)) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &TimelineBar::window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = ClassName;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::runtime_error("Could not register the timeline window class.");
    }
    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, ClassName, L"", WS_POPUP,
        0, 0, 100, Height, owner_, nullptr, instance, this);
    if (!window_) {
        throw std::runtime_error("Could not create the timeline window.");
    }
    SetLayeredWindowAttributes(window_, 0, 236, LWA_ALPHA);
    font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    small_font_ = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

TimelineBar::~TimelineBar() {
    if (window_ && IsWindow(window_)) {
        DestroyWindow(window_);
    }
    if (font_) DeleteObject(font_);
    if (small_font_) DeleteObject(small_font_);
}

void TimelineBar::set_visible(bool visible) {
    visible_ = visible;
    if (!visible_) {
        ShowWindow(window_, SW_HIDE);
        scrubbing_ = false;
        pressed_button_ = NoHit;
    } else {
        SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        placed_width_ = 0;
        dirty_ = true;
    }
}

void TimelineBar::update(const TimelineState& state) {
    const bool discrete_change = state.playing != state_.playing || state.reverse != state_.reverse
        || state.speed_index != state_.speed_index || state.follow != state_.follow
        || state.duration != state_.duration || state.real_start != state_.real_start;
    if (discrete_change || state.time != state_.time) dirty_ = true;
    state_ = state;
    if (!visible_) return;

    if (IsIconic(owner_) || !IsWindowVisible(owner_)) {
        ShowWindow(window_, SW_HIDE);
        return;
    }
    RECT client{};
    GetClientRect(owner_, &client);
    POINT origin{0, client.bottom - Height};
    ClientToScreen(owner_, &origin);
    const int width = client.right - client.left;
    if (origin.x != placed_x_ || origin.y != placed_y_ || width != placed_width_) {
        SetWindowPos(window_, nullptr, origin.x, origin.y, width, Height,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        placed_x_ = origin.x;
        placed_y_ = origin.y;
        placed_width_ = width;
        dirty_ = true;
    }
    if (!IsWindowVisible(window_)) {
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        dirty_ = true;
    }
    const DWORD now = GetTickCount();
    if (dirty_ && (discrete_change || now - last_paint_ >= 33)) {
        last_paint_ = now;
        dirty_ = false;
        InvalidateRect(window_, nullptr, FALSE);
    }
}

float TimelineBar::time_at(int x) const {
    RECT client{};
    GetClientRect(window_, &client);
    const auto lay = layout(client.right - client.left);
    const float span = static_cast<float>(std::max<LONG>(lay.track.right - lay.track.left, 1));
    const float fraction = std::clamp(static_cast<float>(x - lay.track.left) / span, 0.0f, 1.0f);
    return fraction * state_.duration;
}

void TimelineBar::mouse_move(int x, int y) {
    hover_x_ = x;
    hover_y_ = y;
    if (!tracking_leave_) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
        TrackMouseEvent(&track);
        tracking_leave_ = true;
    }
    if (scrubbing_ && actions_.seek) {
        actions_.seek(time_at(x));
    }
    dirty_ = true;
    InvalidateRect(window_, nullptr, FALSE);
}

namespace {

int hit_test(const RECT& track, const RECT* buttons, const std::vector<RECT>& chips, int x, int y) {
    if (y >= TrackTop - 2 && y < TrackBottom && x >= track.left - 8 && x < track.right + 8) {
        return TrackHit;
    }
    for (int index = 0; index < ButtonCount; ++index) {
        if (inside(buttons[index], x, y)) return index;
    }
    for (std::size_t index = 0; index < chips.size(); ++index) {
        if (inside(chips[index], x, y)) return ChipBase + static_cast<int>(index);
    }
    return NoHit;
}

} // namespace

void TimelineBar::mouse_down(int x, int y) {
    RECT client{};
    GetClientRect(window_, &client);
    const auto lay = layout(client.right - client.left);
    const int hit = hit_test(lay.track, lay.buttons, lay.chips, x, y);
    if (hit == TrackHit) {
        scrubbing_ = true;
        if (actions_.scrub) actions_.scrub(true);
        if (actions_.seek) actions_.seek(time_at(x));
    } else {
        pressed_button_ = hit;
    }
    dirty_ = true;
    InvalidateRect(window_, nullptr, FALSE);
}

void TimelineBar::mouse_up(int x, int y) {
    RECT client{};
    GetClientRect(window_, &client);
    const auto lay = layout(client.right - client.left);
    if (scrubbing_) {
        scrubbing_ = false;
        if (actions_.scrub) actions_.scrub(false);
    } else if (pressed_button_ != NoHit
               && pressed_button_ == hit_test(lay.track, lay.buttons, lay.chips, x, y)) {
        switch (pressed_button_) {
        case SkipBack:
            if (actions_.skip) actions_.skip(-10.0f);
            break;
        case Reverse:
            if (actions_.reverse) actions_.reverse();
            break;
        case PlayPause:
            // One toggle: playing forwards pauses, anything else plays forwards.
            if (state_.playing && !state_.reverse) {
                if (actions_.pause) actions_.pause();
            } else if (actions_.play) {
                actions_.play();
            }
            break;
        case SkipForward:
            if (actions_.skip) actions_.skip(10.0f);
            break;
        default:
            if (pressed_button_ >= ChipBase && actions_.set_speed) {
                actions_.set_speed(pressed_button_ - ChipBase);
            }
            break;
        }
    }
    pressed_button_ = NoHit;
    dirty_ = true;
    InvalidateRect(window_, nullptr, FALSE);
}

void TimelineBar::paint() {
    PAINTSTRUCT paint_struct{};
    HDC target = BeginPaint(window_, &paint_struct);
    RECT client{};
    GetClientRect(window_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, std::max(width, 1), std::max(height, 1));
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    draw(dc, width);
    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(window_, &paint_struct);
}

void TimelineBar::paint_preview(HDC target, int width, int height) {
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, std::max(width, 1), Height);
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    draw(dc, width);
    BitBlt(target, 0, height - Height, width, Height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

void TimelineBar::draw(HDC dc, int width) {
    const RECT client{0, 0, width, Height};
    SetBkMode(dc, TRANSPARENT);

    fill(dc, client, Background);
    RECT line{0, 0, width, 1};
    fill(dc, line, Divider);

    const auto lay = layout(width);
    const int hit = hover_x_ >= 0
        ? hit_test(lay.track, lay.buttons, lay.chips, hover_x_, hover_y_) : NoHit;
    const bool track_active = scrubbing_ || hit == TrackHit;

    // Seek track.
    const int center = (TrackTop + TrackBottom) / 2 + 2;
    const int thickness = track_active ? 6 : 4;
    RECT base{lay.track.left, center - thickness / 2, lay.track.right, center + (thickness + 1) / 2};
    rounded(dc, base, thickness, TrackColor);
    const float fraction = state_.duration > 0.0f
        ? std::clamp(state_.time / state_.duration, 0.0f, 1.0f) : 0.0f;
    const int thumb_x = lay.track.left + static_cast<int>(
        std::lround(fraction * static_cast<float>(lay.track.right - lay.track.left)));
    if (thumb_x > lay.track.left) {
        RECT played{lay.track.left, base.top, thumb_x, base.bottom};
        rounded(dc, played, thickness, PlayedColor);
    }
    disc(dc, thumb_x, center, track_active ? 8 : 6, PlayedColor);

    // Hover preview: the time under the cursor.
    if (track_active && state_.duration > 0.0f) {
        const int px = std::clamp(hover_x_, static_cast<int>(lay.track.left),
                                  static_cast<int>(lay.track.right));
        std::wstring label = clock_text(time_at(px));
        const std::wstring real = real_clock_text(state_.real_start, time_at(px));
        if (!real.empty()) label += L"  (" + real + L")";
        SelectObject(dc, small_font_);
        const int half = real.empty() ? 28 : 58;
        RECT box{px - half, 2, px + half, 20};
        const int shift = std::max<int>(0, Margin - box.left)
            - std::max<int>(0, box.right - (width - Margin));
        OffsetRect(&box, shift, 0);
        rounded(dc, box, 6, Hover);
        text_in(dc, label, box, DT_CENTER | DT_VCENTER, Text);
    }

    // Transport buttons.
    SelectObject(dc, font_);
    for (int index = 0; index < ButtonCount; ++index) {
        const RECT& rect = lay.buttons[index];
        bool active = false;
        const bool playing_forward = state_.playing && !state_.reverse;
        if (index == Reverse) active = state_.playing && state_.reverse;
        if (index == PlayPause) active = playing_forward;
        const bool hovered = hit == index || pressed_button_ == index;
        if (active) {
            rounded(dc, rect, 10, PlayedColor);
        } else if (hovered) {
            rounded(dc, rect, 10, Hover);
        }
        const COLORREF ink = active ? RGB(255, 255, 255) : Text;
        const int cx = (rect.left + rect.right) / 2;
        const int cy = (rect.top + rect.bottom) / 2;
        switch (index) {
        case SkipBack:
            text_in(dc, L"-10s", rect, DT_CENTER | DT_VCENTER, ink);
            break;
        case SkipForward:
            text_in(dc, L"+10s", rect, DT_CENTER | DT_VCENTER, ink);
            break;
        case Reverse:
            triangle(dc, cx, cy, 7, false, ink);
            break;
        case PlayPause:
            if (playing_forward) {
                RECT left_bar{cx - 6, cy - 7, cx - 2, cy + 8};
                RECT right_bar{cx + 3, cy - 7, cx + 7, cy + 8};
                fill(dc, left_bar, ink);
                fill(dc, right_bar, ink);
            } else {
                triangle(dc, cx, cy, 7, true, ink);
            }
            break;
        }
    }

    // Clock.
    SelectObject(dc, font_);
    std::wstring clock = clock_text(state_.time) + L" / " + clock_text(state_.duration);
    const std::wstring real = real_clock_text(state_.real_start, state_.time);
    if (!real.empty()) clock += L"   " + real;
    text_in(dc, clock, lay.clock, DT_LEFT | DT_VCENTER, Text);
    if (!state_.follow.empty() && lay.follow.right > lay.follow.left) {
        SelectObject(dc, small_font_);
        text_in(dc, L"following " + state_.follow, lay.follow, DT_LEFT | DT_VCENTER, MutedText);
    }

    // Speed presets.
    SelectObject(dc, small_font_);
    for (std::size_t index = 0; index < lay.chips.size(); ++index) {
        const RECT& rect = lay.chips[index];
        const bool selected = static_cast<int>(index) == state_.speed_index;
        const bool hovered = hit == ChipBase + static_cast<int>(index)
            || pressed_button_ == ChipBase + static_cast<int>(index);
        if (selected) {
            rounded(dc, rect, 10, PlayedColor);
        } else if (hovered) {
            rounded(dc, rect, 10, Hover);
        }
        text_in(dc, speed_labels_[index], rect, DT_CENTER | DT_VCENTER,
                selected ? RGB(255, 255, 255) : MutedText);
    }
}

LRESULT TimelineBar::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    const int x = static_cast<int>(static_cast<short>(LOWORD(lparam)));
    const int y = static_cast<int>(static_cast<short>(HIWORD(lparam)));
    switch (message) {
    case WM_PAINT:
        paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        mouse_move(x, y);
        return 0;
    case WM_MOUSELEAVE:
        tracking_leave_ = false;
        if (!scrubbing_) {
            hover_x_ = hover_y_ = -1;
            dirty_ = true;
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(window_);
        mouse_down(x, y);
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == window_) ReleaseCapture();
        mouse_up(x, y);
        return 0;
    case WM_CAPTURECHANGED:
        if (scrubbing_) {
            scrubbing_ = false;
            if (actions_.scrub) actions_.scrub(false);
        }
        return 0;
    default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
}

LRESULT CALLBACK TimelineBar::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    TimelineBar* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<TimelineBar*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TimelineBar*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (!self || !self->window_) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    return self->handle(message, wparam, lparam);
}

} // namespace gmod::ui
