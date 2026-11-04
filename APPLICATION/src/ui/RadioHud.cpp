#include "ui/RadioHud.h"

#include <algorithm>

namespace gmod::ui {
namespace {

constexpr wchar_t ClassName[] = L"GModReplayRadioHud";
// Not a colour any job uses: Medic names are magenta and would vanish against a magenta key.
constexpr COLORREF KeyColor = RGB(2, 1, 3);
constexpr int HudWidth = 420;
constexpr int ButtonWidth = 150;
constexpr int ButtonHeight = 26;
constexpr int Margin = 8;
constexpr int LineHeight = 17;

HFONT make_font(int height, int weight) {
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
}

void outlined(HDC dc, const std::wstring& text, int center_x, int top, COLORREF color) {
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    const int left = center_x - size.cx / 2;
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

} // namespace

RadioHud::RadioHud(HWND owner, std::function<void()> toggle_mute)
    : owner_(owner), toggle_mute_(std::move(toggle_mute)) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &RadioHud::window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = ClassName;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = CreateSolidBrush(KeyColor);
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, ClassName, L"", WS_POPUP,
        0, 0, HudWidth, 40, owner_, nullptr, window_class.hInstance, this);
    if (window_) SetLayeredWindowAttributes(window_, KeyColor, 0, LWA_COLORKEY);
}

RadioHud::~RadioHud() {
    if (window_) DestroyWindow(window_);
}

RECT RadioHud::button_rect() {
    return {(HudWidth - ButtonWidth) / 2, Margin, (HudWidth + ButtonWidth) / 2, Margin + ButtonHeight};
}

void RadioHud::update(const RadioHudState& state) {
    if (!window_ || IsIconic(owner_)) return;
    if (!state.available) {
        if (shown_) ShowWindow(window_, SW_HIDE);
        shown_ = false;
        return;
    }
    int height = Margin + ButtonHeight + Margin;
    for (const auto& channel : state.channels) {
        height += LineHeight + 2 + static_cast<int>(channel.talking.size()) * LineHeight + 4;
    }
    RECT client{};
    GetClientRect(owner_, &client);
    POINT origin{(client.right - client.left - HudWidth) / 2, 0};
    ClientToScreen(owner_, &origin);

    const bool same_state = painted_ && last_.available == state.available && last_.muted == state.muted
        && last_.channels.size() == state.channels.size()
        && std::equal(state.channels.begin(), state.channels.end(), last_.channels.begin(),
                      [](const RadioHudChannel& a, const RadioHudChannel& b) {
                          return a.name == b.name && a.talking.size() == b.talking.size()
                              && std::equal(a.talking.begin(), a.talking.end(), b.talking.begin(),
                                            [](const RadioHudLine& x, const RadioHudLine& y) {
                                                return x.text == y.text && x.color == y.color;
                                            });
                      });
    const bool moved = origin.x != placed_x_ || origin.y != placed_y_ || height != placed_height_;
    if (moved) {
        SetWindowPos(window_, HWND_TOP, origin.x, origin.y, HudWidth, height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        placed_x_ = origin.x;
        placed_y_ = origin.y;
        placed_height_ = height;
    }
    if (!shown_) {
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        shown_ = true;
    }
    if (same_state && !moved) return;
    last_ = state;
    paint(state, HudWidth, height);
    painted_ = true;
}

void RadioHud::paint(const RadioHudState& state, int width, int height) {
    HDC target = GetDC(window_);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    RECT all{0, 0, width, height};
    HBRUSH key = CreateSolidBrush(KeyColor);
    FillRect(dc, &all, key);
    DeleteObject(key);

    static const HFONT bold = make_font(13, FW_BOLD);
    static const HFONT regular = make_font(13, FW_NORMAL);
    SetBkMode(dc, TRANSPARENT);

    // The mute button: green while the radio is audible, red while muted.
    const RECT button = button_rect();
    HBRUSH fill = CreateSolidBrush(state.muted ? RGB(150, 40, 40) : RGB(40, 120, 60));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HGDIOBJ old_brush = SelectObject(dc, fill);
    HGDIOBJ old_pen = SelectObject(dc, border);
    RoundRect(dc, button.left, button.top, button.right, button.bottom, 8, 8);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(fill);
    DeleteObject(border);
    SelectObject(dc, bold);
    const std::wstring label = state.muted ? L"RADIO MUTED - click to unmute" : L"RADIO ON - click to mute";
    SetTextColor(dc, RGB(255, 255, 255));
    RECT text_area = button;
    DrawTextW(dc, label.c_str(), -1, &text_area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);

    int y = button.bottom + Margin;
    for (const auto& channel : state.channels) {
        SelectObject(dc, bold);
        outlined(dc, channel.name, width / 2, y, RGB(255, 255, 255));
        y += LineHeight + 2;
        SelectObject(dc, regular);
        for (const auto& line : channel.talking) {
            outlined(dc, line.text, width / 2, y, line.color);
            y += LineHeight;
        }
        y += 4;
    }
    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(window_, target);
}

LRESULT RadioHud::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN: {
        const POINT at{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        const RECT button = button_rect();
        if (PtInRect(&button, at) && toggle_mute_) toggle_mute_();
        return 0;
    }
    default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
    (void)wparam;
}

LRESULT CALLBACK RadioHud::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    RadioHud* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<RadioHud*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->window_ = window;
    } else {
        self = reinterpret_cast<RadioHud*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (self) return self->handle(message, wparam, lparam);
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace gmod::ui
