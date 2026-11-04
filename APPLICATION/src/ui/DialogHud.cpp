#include "ui/DialogHud.h"
#include <algorithm>

namespace gmod::ui {
namespace {

constexpr wchar_t ClassName[] = L"GModReplayDialogHud";
// Not a colour any job uses: Medic names are magenta and would vanish against a magenta key.
constexpr COLORREF KeyColor = RGB(2, 1, 3);
constexpr int BoxWidth = 560;
constexpr int Pad = 12;
constexpr int TitleHeight = 24;

HFONT make_font(int height, int weight) {
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
}

} // namespace

DialogHud::DialogHud(HWND owner) : owner_(owner) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &DialogHud::window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = ClassName;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = CreateSolidBrush(KeyColor);
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    window_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, ClassName, L"",
        WS_POPUP, 0, 0, BoxWidth, 80, owner_, nullptr, window_class.hInstance, nullptr);
    if (window_) SetLayeredWindowAttributes(window_, KeyColor, 0, LWA_COLORKEY);
}

DialogHud::~DialogHud() {
    if (window_) DestroyWindow(window_);
}

void DialogHud::update(const DialogHudState& state) {
    if (!window_ || IsIconic(owner_)) return;
    if (!state.visible) {
        if (shown_) ShowWindow(window_, SW_HIDE);
        shown_ = false;
        return;
    }
    // Measure the wrapped text first so the box fits it.
    HDC measure = GetDC(window_);
    static const HFONT regular = make_font(14, FW_NORMAL);
    HGDIOBJ previous = SelectObject(measure, regular);
    int height = Pad + TitleHeight;
    for (const auto& line : state.lines) {
        RECT area{0, 0, BoxWidth - 2 * Pad, 0};
        DrawTextW(measure, line.c_str(), -1, &area, DT_CALCRECT | DT_WORDBREAK);
        height += area.bottom + 6;
    }
    height += static_cast<int>(std::min<std::size_t>(state.options.size(),10))*32;
    if (state.reconstructed) height += 18;
    height += Pad;
    SelectObject(measure, previous);
    ReleaseDC(window_, measure);

    RECT client{};
    GetClientRect(owner_, &client);
    height = std::min(height,std::max(80,static_cast<int>(client.bottom-client.top)-24));
    POINT origin{(client.right - client.left - BoxWidth) / 2,
                 std::max(8,std::min(static_cast<int>((client.bottom-client.top)*0.62f),
                     static_cast<int>(client.bottom-client.top)-height-12))};
    ClientToScreen(owner_, &origin);
    const bool moved = origin.x != placed_x_ || origin.y != placed_y_ || height != placed_height_;
    if (moved) {
        SetWindowPos(window_, HWND_TOP, origin.x, origin.y, BoxWidth, height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        placed_x_ = origin.x;
        placed_y_ = origin.y;
        placed_height_ = height;
    }
    if (!shown_) {
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        shown_ = true;
    }
    if (painted_ && !moved && last_.title == state.title && last_.lines == state.lines
        && last_.options == state.options && last_.reconstructed == state.reconstructed) return;
    last_ = state;
    paint(state, BoxWidth, height);
    painted_ = true;
}

void DialogHud::paint(const DialogHudState& state, int width, int height) {
    HDC target = GetDC(window_);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    HGDIOBJ previous = SelectObject(dc, bitmap);
    RECT all{0, 0, width, height};
    HBRUSH key = CreateSolidBrush(KeyColor);
    FillRect(dc, &all, key);
    DeleteObject(key);

    HBRUSH fill = CreateSolidBrush(RGB(24, 28, 34));
    HPEN border = CreatePen(PS_SOLID, 2, RGB(70, 140, 220));
    HGDIOBJ old_brush = SelectObject(dc, fill);
    HGDIOBJ old_pen = SelectObject(dc, border);
    RoundRect(dc, 1, 1, width - 1, height - 1, 10, 10);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(fill);
    DeleteObject(border);

    static const HFONT bold = make_font(16, FW_BOLD);
    static const HFONT regular = make_font(14, FW_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, bold);
    SetTextColor(dc, RGB(120, 190, 255));
    RECT title{Pad, Pad, width - Pad, Pad + TitleHeight};
    DrawTextW(dc, state.title.c_str(), -1, &title, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, regular);
    SetTextColor(dc, RGB(235, 235, 235));
    int y = Pad + TitleHeight;
    for (const auto& line : state.lines) {
        RECT area{Pad, y, width - Pad, height};
        RECT measured{0, 0, width - 2 * Pad, 0};
        DrawTextW(dc, line.c_str(), -1, &measured, DT_CALCRECT | DT_WORDBREAK);
        DrawTextW(dc, line.c_str(), -1, &area, DT_LEFT | DT_WORDBREAK);
        y += measured.bottom + 6;
    }
    for (std::size_t i=0;i<std::min<std::size_t>(state.options.size(),10);++i) {
        RECT button{Pad,y,width-Pad,y+27};
        HBRUSH background = CreateSolidBrush(RGB(43,49,58));
        FillRect(dc,&button,background);
        DeleteObject(background);
        button.left += 10; button.right -= 8;
        const auto label=std::to_wstring(i+1)+L". "+state.options[i];
        DrawTextW(dc,label.c_str(),-1,&button,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        y += 32;
    }
    if (state.reconstructed) {
        SetTextColor(dc,RGB(160,169,180));
        RECT note{Pad,y,width-Pad,y+18};
        DrawTextW(dc,L"Reconstructed menu",-1,&note,DT_LEFT|DT_SINGLELINE);
    }
    BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY);
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(window_, target);
}

LRESULT CALLBACK DialogHud::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace gmod::ui
