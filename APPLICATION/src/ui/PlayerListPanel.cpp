#include "ui/PlayerListPanel.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace gmod::ui {
namespace {

constexpr wchar_t ClassName[] = L"GmodReplayPlayerList";
constexpr int HeaderHeight = 58;
constexpr int RowHeight = 54;
constexpr int Padding = 16;
constexpr int BarWidth = 10;
constexpr int PanelWidth = 720;

constexpr COLORREF Background = RGB(16, 18, 24);
constexpr COLORREF HeaderColor = RGB(24, 27, 36);
constexpr COLORREF Divider = RGB(48, 52, 64);
constexpr COLORREF Hover = RGB(40, 45, 58);
constexpr COLORREF Followed = RGB(120, 34, 38);
constexpr COLORREF Accent = RGB(232, 62, 62);
constexpr COLORREF Text = RGB(236, 238, 244);
constexpr COLORREF MutedText = RGB(150, 156, 172);
constexpr COLORREF AbsentText = RGB(104, 109, 124);
constexpr COLORREF BarTrack = RGB(32, 35, 45);
constexpr COLORREF BarThumb = RGB(96, 102, 120);

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

void text_in(HDC dc, const std::wstring& text, RECT rect, UINT flags, COLORREF color) {
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
              flags | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

HFONT make_font(int height, int weight) {
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

} // namespace

PlayerListPanel::PlayerListPanel(HWND owner, PlayerListActions actions)
    : owner_(owner), actions_(std::move(actions)) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &PlayerListPanel::window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = ClassName;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        throw std::runtime_error("Could not register the player list window class.");
    }
    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, ClassName, L"", WS_POPUP,
        0, 0, PanelWidth, 400, owner_, nullptr, instance, this);
    if (!window_) {
        throw std::runtime_error("Could not create the player list window.");
    }
    SetLayeredWindowAttributes(window_, 0, 240, LWA_ALPHA);
    title_font_ = make_font(-20, FW_SEMIBOLD);
    font_ = make_font(-17, FW_SEMIBOLD);
    small_font_ = make_font(-13, FW_NORMAL);
}

PlayerListPanel::~PlayerListPanel() {
    if (window_ && IsWindow(window_)) DestroyWindow(window_);
    if (title_font_) DeleteObject(title_font_);
    if (font_) DeleteObject(font_);
    if (small_font_) DeleteObject(small_font_);
}

void PlayerListPanel::set_visible(bool visible) {
    if (visible == visible_) return;
    visible_ = visible;
    if (!visible_) {
        if (GetCapture() == window_) ReleaseCapture();
        ShowWindow(window_, SW_HIDE);
        dragging_bar_ = false;
        pressed_id_ = -1;
        hover_row_ = -1;
    } else {
        // Shown above the HUD (scope mask, feed) and the name tags.
        SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        placed_width_ = 0;
        center_pending_ = true;
        dirty_ = true;
    }
}

void PlayerListPanel::set_rows(std::vector<PlayerListRow> rows, std::wstring title) {
    auto same = [](const PlayerListRow& a, const PlayerListRow& b) {
        return a.id == b.id && a.present == b.present && a.followed == b.followed
            && a.recorder == b.recorder && a.name == b.name && a.detail == b.detail
            && a.status == b.status && a.has_voice == b.has_voice && a.isolated == b.isolated;
    };
    const bool unchanged = title == title_ && rows.size() == rows_.size()
        && std::equal(rows.begin(), rows.end(), rows_.begin(), same);
    if (unchanged) return;
    rows_ = std::move(rows);
    title_ = std::move(title);
    dirty_ = true;
    if (center_pending_) center_on_followed();
    scroll_ = std::clamp(scroll_, 0, max_scroll());
}

int PlayerListPanel::content_height() const {
    return static_cast<int>(rows_.size()) * RowHeight;
}

int PlayerListPanel::view_height() const {
    RECT client{};
    GetClientRect(window_, &client);
    return std::max<int>(client.bottom - HeaderHeight, 0);
}

int PlayerListPanel::max_scroll() const {
    return std::max(content_height() - view_height(), 0);
}

RECT PlayerListPanel::isolate_rect(int row_top, int list_right) const {
    return {list_right - Padding - 112, row_top + 30, list_right - Padding, row_top + 50};
}

int PlayerListPanel::row_at(int y) const {
    if (y < HeaderHeight) return -1;
    const int index = (y - HeaderHeight + scroll_) / RowHeight;
    return index >= 0 && index < static_cast<int>(rows_.size()) ? index : -1;
}

void PlayerListPanel::center_on_followed() {
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        if (!rows_[index].followed) continue;
        const int row_center = static_cast<int>(index) * RowHeight + RowHeight / 2;
        scroll_ = std::clamp(row_center - view_height() / 2, 0, max_scroll());
        break;
    }
    center_pending_ = false;
}

void PlayerListPanel::scroll_by(int wheel_delta) {
    // Three rows per wheel notch.
    const int step = wheel_delta * RowHeight * 3 / WHEEL_DELTA;
    const int next = std::clamp(scroll_ - step, 0, max_scroll());
    if (next != scroll_) {
        scroll_ = next;
        dirty_ = true;
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void PlayerListPanel::drag_scroll_to(int y) {
    const int track = std::max(view_height(), 1);
    const float fraction = std::clamp(static_cast<float>(y - HeaderHeight) / track, 0.0f, 1.0f);
    scroll_ = static_cast<int>(fraction * static_cast<float>(max_scroll()));
    dirty_ = true;
    InvalidateRect(window_, nullptr, FALSE);
}

void PlayerListPanel::update() {
    if (!visible_) return;
    if (IsIconic(owner_) || !IsWindowVisible(owner_)) {
        ShowWindow(window_, SW_HIDE);
        return;
    }
    RECT client{};
    GetClientRect(owner_, &client);
    const int client_width = client.right - client.left;
    const int client_height = client.bottom - client.top;
    const int width = std::min(PanelWidth, std::max(client_width - 40, 240));
    const int height = std::max(client_height - 160, 160);
    POINT origin{(client_width - width) / 2, (client_height - height) / 2 - 20};
    ClientToScreen(owner_, &origin);
    if (origin.x != placed_x_ || origin.y != placed_y_ || width != placed_width_
        || height != placed_height_) {
        SetWindowPos(window_, nullptr, origin.x, origin.y, width, height,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        placed_x_ = origin.x;
        placed_y_ = origin.y;
        placed_width_ = width;
        placed_height_ = height;
        scroll_ = std::clamp(scroll_, 0, max_scroll());
        dirty_ = true;
    }
    if (!IsWindowVisible(window_)) {
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        if (center_pending_) center_on_followed();
        dirty_ = true;
    }
    const DWORD now = GetTickCount();
    if (dirty_ && now - last_paint_ >= 33) {
        last_paint_ = now;
        dirty_ = false;
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void PlayerListPanel::paint() {
    PAINTSTRUCT paint_struct{};
    HDC target = BeginPaint(window_, &paint_struct);
    RECT client{};
    GetClientRect(window_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, std::max(width, 1), std::max(height, 1));
    HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
    SetBkMode(dc, TRANSPARENT);

    fill(dc, client, Background);

    const bool overflow = max_scroll() > 0;
    const int list_right = width - (overflow ? BarWidth + 6 : 0);
    const int first = std::max(scroll_ / RowHeight, 0);
    for (int index = first; index < static_cast<int>(rows_.size()); ++index) {
        const int top = HeaderHeight + index * RowHeight - scroll_;
        if (top >= height) break;
        const auto& row = rows_[static_cast<std::size_t>(index)];
        RECT area{0, top, list_right, top + RowHeight};
        if (row.followed) {
            fill(dc, area, Followed);
        } else if (index == hover_row_) {
            fill(dc, area, Hover);
        }
        RECT line{Padding, top + RowHeight - 1, list_right - Padding, top + RowHeight};
        fill(dc, line, Divider);

        const COLORREF name_color = row.present ? Text : AbsentText;
        SelectObject(dc, font_);
        RECT badge_rect{list_right - 190, top + 8, list_right - Padding, top + 30};
        std::wstring badge = row.status;
        if (row.recorder) badge = badge.empty() ? L"RECORDER" : L"RECORDER  " + badge;
        RECT name_rect{Padding, top + 6, badge_rect.left - 8, top + 30};
        text_in(dc, row.name.empty() ? L"(unnamed)" : row.name, name_rect,
                DT_LEFT | DT_VCENTER, name_color);
        SelectObject(dc, small_font_);
        text_in(dc, badge, badge_rect, DT_RIGHT | DT_VCENTER,
                row.recorder ? Accent : (row.present ? MutedText : AbsentText));
        RECT detail_rect{Padding, top + 30, list_right - Padding, top + RowHeight - 4};
        if (row.has_voice) {
            // Isolate-voice toggle: lit while only this player is audible.
            const RECT button = isolate_rect(top, list_right);
            detail_rect.right = button.left - 8;
            const bool hot = index == hover_isolate_;
            fill(dc, button, row.isolated ? Accent : (hot ? RGB(64, 70, 88) : RGB(36, 40, 52)));
            SelectObject(dc, small_font_);
            RECT label = button;
            text_in(dc, row.isolated ? L"Voice isolated" : L"Isolate voice", label,
                    DT_CENTER | DT_VCENTER, row.isolated ? RGB(255, 255, 255) : MutedText);
        }
        SelectObject(dc, small_font_);
        text_in(dc, row.detail, detail_rect, DT_LEFT | DT_VCENTER,
                row.present ? MutedText : AbsentText);
    }

    // Header drawn after the rows so scrolled rows slide beneath it.
    RECT header{0, 0, width, HeaderHeight};
    fill(dc, header, HeaderColor);
    RECT header_line{0, HeaderHeight - 1, width, HeaderHeight};
    fill(dc, header_line, Divider);
    SelectObject(dc, title_font_);
    RECT title_rect{Padding, 6, width - Padding, 34};
    text_in(dc, title_, title_rect, DT_LEFT | DT_VCENTER, Text);
    SelectObject(dc, small_font_);
    RECT hint_rect{Padding, 34, width - Padding, HeaderHeight - 4};
    text_in(dc, L"Click a player to follow them  |  \"Isolate voice\" to hear only them  |  Mouse wheel to scroll  |  Release P to close",
            hint_rect, DT_LEFT | DT_VCENTER, MutedText);

    if (overflow) {
        const int track_top = HeaderHeight;
        const int track_height = std::max(height - HeaderHeight, 1);
        RECT track{width - BarWidth - 4, track_top, width - 4, height};
        rounded(dc, track, BarWidth, BarTrack);
        const int thumb_height = std::max(
            track_height * track_height / std::max(content_height(), 1), 36);
        const int travel = std::max(track_height - thumb_height, 1);
        const int thumb_top = track_top + scroll_ * travel / std::max(max_scroll(), 1);
        RECT thumb{track.left, thumb_top, track.right, thumb_top + thumb_height};
        rounded(dc, thumb, BarWidth, dragging_bar_ ? Accent : BarThumb);
    }

    RECT border{0, 0, width, height};
    HBRUSH border_brush = CreateSolidBrush(Divider);
    FrameRect(dc, &border, border_brush);
    DeleteObject(border_brush);

    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(window_, &paint_struct);
}

LRESULT PlayerListPanel::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    const int x = static_cast<int>(static_cast<short>(LOWORD(lparam)));
    const int y = static_cast<int>(static_cast<short>(HIWORD(lparam)));
    RECT client{};
    GetClientRect(window_, &client);
    const bool in_bar = max_scroll() > 0 && x >= client.right - BarWidth - 10 && y >= HeaderHeight;
    switch (message) {
    case WM_PAINT:
        paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEWHEEL:
        scroll_by(GET_WHEEL_DELTA_WPARAM(wparam));
        return 0;
    case WM_MOUSEMOVE: {
        if (!tracking_leave_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
            TrackMouseEvent(&track);
            tracking_leave_ = true;
        }
        if (dragging_bar_) {
            drag_scroll_to(y);
            return 0;
        }
        const int row = in_bar ? -1 : row_at(y);
        int isolate_hover = -1;
        if (row >= 0 && rows_[static_cast<std::size_t>(row)].has_voice) {
            const int list_right = client.right - (max_scroll() > 0 ? BarWidth + 6 : 0);
            const RECT button = isolate_rect(HeaderHeight + row * RowHeight - scroll_, list_right);
            const POINT at{x, y};
            if (PtInRect(&button, at)) isolate_hover = row;
        }
        if (row != hover_row_ || isolate_hover != hover_isolate_) {
            hover_row_ = row;
            hover_isolate_ = isolate_hover;
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_leave_ = false;
        if (hover_row_ != -1 || hover_isolate_ != -1) {
            hover_row_ = -1;
            hover_isolate_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(window_);
        if (in_bar) {
            dragging_bar_ = true;
            drag_scroll_to(y);
        } else {
            const int row = row_at(y);
            pressed_id_ = row >= 0 ? rows_[static_cast<std::size_t>(row)].id : -1;
            pressed_isolate_ = row >= 0 && row == hover_isolate_;
        }
        return 0;
    case WM_LBUTTONUP: {
        if (GetCapture() == window_) ReleaseCapture();
        if (dragging_bar_) {
            dragging_bar_ = false;
            InvalidateRect(window_, nullptr, FALSE);
        } else if (pressed_id_ >= 0) {
            const int row = row_at(y);
            if (row >= 0 && rows_[static_cast<std::size_t>(row)].id == pressed_id_) {
                if (pressed_isolate_) {
                    if (row == hover_isolate_ && actions_.isolate) actions_.isolate(pressed_id_);
                } else if (actions_.select) {
                    actions_.select(pressed_id_);
                }
            }
        }
        pressed_id_ = -1;
        pressed_isolate_ = false;
        return 0;
    }
    case WM_CAPTURECHANGED:
        dragging_bar_ = false;
        return 0;
    default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
}

LRESULT CALLBACK PlayerListPanel::window_proc(HWND window, UINT message, WPARAM wparam,
                                              LPARAM lparam) {
    PlayerListPanel* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        self = static_cast<PlayerListPanel*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<PlayerListPanel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (!self || !self->window_) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    return self->handle(message, wparam, lparam);
}

} // namespace gmod::ui
