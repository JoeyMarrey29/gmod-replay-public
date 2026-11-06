#include "ui/PlayerCard.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <tuple>

namespace gmod::ui {
namespace {

constexpr wchar_t ClassName[] = L"GModReplayPlayerCard";
constexpr int CardWidth = 360;
constexpr int Pad = 12;
constexpr int HeaderHeight = 42;
constexpr int TitleHeight = 26;
constexpr int LabelWidth = 96;
constexpr int RowGap = 4;
constexpr int ButtonHeight = 28;
constexpr int ButtonGap = 10;
constexpr COLORREF Background = RGB(22, 26, 32);
constexpr COLORREF Border = RGB(70, 140, 220);

HFONT make_font(int height, int weight) {
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
}

HFONT title_font() {
    static const HFONT font = make_font(17, FW_BOLD);
    return font;
}

HFONT body_font() {
    static const HFONT font = make_font(13, FW_NORMAL);
    return font;
}

HFONT label_font() {
    static const HFONT font = make_font(13, FW_BOLD);
    return font;
}

void button(HDC dc, const RECT& area, const wchar_t* text, COLORREF fill, COLORREF ink) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, area.left, area.top, area.right, area.bottom, 8, 8);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
    SetTextColor(dc, ink);
    RECT text_area = area;
    DrawTextW(dc, text, -1, &text_area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

} // namespace

PlayerCard::PlayerCard(HWND owner, std::function<void()> follow, std::function<void()> solo,
                       std::function<void()> mute, std::function<void()> close)
    : owner_(owner), follow_(std::move(follow)), solo_(std::move(solo)), mute_(std::move(mute)),
      close_(std::move(close)) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &PlayerCard::window_proc;
    window_class.style = CS_DBLCLKS;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = ClassName;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, ClassName, L"", WS_POPUP,
                              0, 0, CardWidth, 100, owner_, nullptr, window_class.hInstance, this);
}

PlayerCard::~PlayerCard() {
    if (window_) DestroyWindow(window_);
}

int PlayerCard::buttons_height() const {
    return state_.vehicle ? ButtonHeight : 2 * ButtonHeight + ButtonGap;
}

RECT PlayerCard::follow_rect(int height) const {
    if (state_.vehicle) return {};
    const int width = (CardWidth - 2 * Pad - ButtonGap) / 2;
    return {Pad, height - Pad - ButtonHeight, Pad + width, height - Pad};
}

RECT PlayerCard::close_rect(int height) const {
    const int width = state_.vehicle ? CardWidth - 2 * Pad : (CardWidth - 2 * Pad - ButtonGap) / 2;
    return {CardWidth - Pad - width, height - Pad - ButtonHeight, CardWidth - Pad, height - Pad};
}

RECT PlayerCard::solo_rect(int height) const {
    if (state_.vehicle) return {};
    const int width = (CardWidth - 2 * Pad - ButtonGap) / 2;
    const int bottom = height - Pad - ButtonHeight - ButtonGap;
    return {Pad, bottom - ButtonHeight, Pad + width, bottom};
}

RECT PlayerCard::mute_rect(int height) const {
    if (state_.vehicle) return {};
    const int width = (CardWidth - 2 * Pad - ButtonGap) / 2;
    const int bottom = height - Pad - ButtonHeight - ButtonGap;
    return {CardWidth - Pad - width, bottom - ButtonHeight, CardWidth - Pad, bottom};
}

RECT PlayerCard::cross_rect() const {
    return {CardWidth - Pad - 18, Pad, CardWidth - Pad, Pad + 18};
}

RECT PlayerCard::collapse_rect() const {
    return {CardWidth - Pad - 44, Pad, CardWidth - Pad - 26, Pad + 18};
}

// Word-wraps the title and every value into lines, so drawing, hit-testing and highlighting
// all agree on where each character is.
int PlayerCard::header_height(HDC dc, const PlayerCardState& state) const {
    if (state.spectator.empty()) return HeaderHeight;
    const auto previous = SelectObject(dc, label_font());
    RECT header{Pad, Pad, collapse_rect().left - 8, Pad};
    const std::wstring text = L"Spectating: " + state.spectator;
    DrawTextW(dc, text.c_str(), -1, &header, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, previous);
    return std::max<int>(HeaderHeight, header.bottom + Pad);
}

bool PlayerCard::header_at(POINT at) const {
    HDC dc = GetDC(window_);
    const int height = header_height(dc, state_);
    ReleaseDC(window_, dc);
    const RECT header{0, 0, CardWidth, height};
    const RECT cross = cross_rect();
    return PtInRect(&header, at)
        && !(state_.vehicle && state_.spectator.empty() && PtInRect(&cross, at));
}

PlayerCard::Layout PlayerCard::layout(HDC dc, const PlayerCardState& state) const {
    Layout result;
    result.segments.push_back({L"", state.title, title_font(), state.title_color});
    for (const auto& [label, value] : state.rows) {
        result.segments.push_back({label, value.empty() ? L"-" : value, body_font(), RGB(235, 235, 235)});
    }
    int y = state.spectator.empty() ? Pad : header_height(dc, state);
    for (int segment = 0; segment < static_cast<int>(result.segments.size()); ++segment) {
        const auto& item = result.segments[static_cast<std::size_t>(segment)];
        const bool title = segment == 0;
        const int x = title ? Pad : Pad + LabelWidth;
        const int width = title ? collapse_rect().left - 8 - Pad : CardWidth - 2 * Pad - LabelWidth;
        HGDIOBJ previous = SelectObject(dc, item.font);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        const int line_height = title ? std::max<int>(metrics.tmHeight, 20) : std::max<int>(metrics.tmHeight, 15);
        const auto& text = item.text;
        const int length = static_cast<int>(text.size());
        int start = 0;
        int first_y = y;
        do {
            int end = start;
            while (end < length && text[static_cast<std::size_t>(end)] != L'\n') ++end;
            int fit = end - start;
            SIZE size{};
            if (fit > 0) {
                GetTextExtentExPointW(dc, text.c_str() + start, end - start, width, &fit, nullptr, &size);
            }
            int next = start + fit;
            if (next < end) {
                int space = next;
                while (space > start && text[static_cast<std::size_t>(space)] != L' ') --space;
                if (space > start) next = space;
                if (next == start) next = start + 1;
            }
            result.lines.push_back({segment, start, next - start, x, y, line_height});
            y += line_height;
            start = next;
            while (start < length && (text[static_cast<std::size_t>(start)] == L' '
                                      || text[static_cast<std::size_t>(start)] == L'\n')) {
                const bool newline = text[static_cast<std::size_t>(start)] == L'\n';
                ++start;
                if (newline) break;
            }
        } while (start < length);
        if (title) y = std::max(y, first_y + TitleHeight) + 6;
        else y = std::max(y, first_y + 15) + RowGap;
        SelectObject(dc, previous);
    }
    result.bottom = y;
    return result;
}

int PlayerCard::measure(const PlayerCardState& state) const {
    HDC dc = GetDC(window_);
    const int bottom = collapsed_ ? header_height(dc, state) : layout(dc, state).bottom;
    ReleaseDC(window_, dc);
    if (collapsed_) return bottom;
    const int buttons = state.vehicle ? ButtonHeight : 2 * ButtonHeight + ButtonGap;
    return bottom + 8 + buttons + Pad;
}

void PlayerCard::update(const PlayerCardState& state) {
    if (!window_ || IsIconic(owner_)) return;
    if (!state.visible) {
        if (shown_) ShowWindow(window_, SW_HIDE);
        shown_ = false;
        selecting_ = false;
        anchor_ = caret_ = {};
        collapsed_ = false;
        return;
    }
    if (!shown_) collapsed_ = !state.spectator.empty();
    const int height = measure(state);
    RECT client{};
    GetClientRect(owner_, &client);
    POINT origin = state.spectator.empty() ? POINT{client.right - CardWidth, 0} : POINT{0, 0};
    ClientToScreen(owner_, &origin);
    const bool moved = origin.x != placed_x_ || origin.y != placed_y_ || height != height_;
    if (moved) {
        SetWindowPos(window_, HWND_TOP, origin.x, origin.y, CardWidth, height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        placed_x_ = origin.x;
        placed_y_ = origin.y;
        height_ = height;
    }
    if (!shown_) {
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        shown_ = true;
    }
    const bool same = state.title == state_.title && state.title_color == state_.title_color
        && state.rows == state_.rows && state.followed == state_.followed && state.vehicle == state_.vehicle
        && state.solo == state_.solo && state.muted == state_.muted && state.has_voice == state_.has_voice
        && state.spectator == state_.spectator;
    if (same && !moved) return;
    // A different player or vehicle drops the highlight; live values updating keep it.
    if (state.title != state_.title || state.vehicle != state_.vehicle
        || state.rows.size() != state_.rows.size()) {
        selecting_ = false;
        copied_ = false;
        anchor_ = caret_ = {};
    }
    state_ = state;
    InvalidateRect(window_, nullptr, FALSE);
}

bool PlayerCard::has_selection() const {
    return anchor_.segment != caret_.segment || anchor_.index != caret_.index;
}

std::wstring PlayerCard::selected_text() const {
    if (!has_selection()) return {};
    HDC dc = GetDC(window_);
    const auto view = layout(dc, state_);
    ReleaseDC(window_, dc);
    auto first = anchor_;
    auto last = caret_;
    if (std::tie(last.segment, last.index) < std::tie(first.segment, first.index)) std::swap(first, last);
    std::wstring result;
    const int count = static_cast<int>(view.segments.size());
    for (int segment = std::max(first.segment, 0); segment <= last.segment && segment < count; ++segment) {
        const auto& item = view.segments[static_cast<std::size_t>(segment)];
        const int size = static_cast<int>(item.text.size());
        const int from = segment == first.segment ? std::clamp(first.index, 0, size) : 0;
        const int to = segment == last.segment ? std::clamp(last.index, 0, size) : size;
        const auto part = item.text.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(std::max(to - from, 0)));
        if (first.segment == last.segment) return part;
        if (!result.empty()) result += L"\r\n";
        result += item.label.empty() ? part : item.label + L": " + part;
    }
    return result;
}

bool PlayerCard::copy_selection() const {
    const auto text = selected_text();
    if (text.empty() || !OpenClipboard(window_)) return false;
    EmptyClipboard();
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    bool copied = false;
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* target = GlobalLock(memory)) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            copied = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!copied) GlobalFree(memory);
    }
    CloseClipboard();
    return copied;
}

// The character position under `at`. `strict` requires the point to be on the text itself,
// which is how a drag starts; while dragging, the nearest position is used.
bool PlayerCard::caret_at(POINT at, bool strict, Caret& caret) const {
    if (collapsed_) return false;
    HDC dc = GetDC(window_);
    if (strict && !state_.spectator.empty() && at.y < header_height(dc, state_)) {
        ReleaseDC(window_, dc);
        return false;
    }
    const auto view = layout(dc, state_);
    const Line* best = nullptr;
    int best_distance = 0;
    for (const auto& line : view.lines) {
        const int distance = at.y < line.y ? line.y - at.y : at.y >= line.y + line.height ? at.y - (line.y + line.height) + 1 : 0;
        if (!best || distance < best_distance) {
            best = &line;
            best_distance = distance;
        }
    }
    bool found = false;
    if (best && (!strict || (best_distance == 0 && at.x >= best->x - 4 && at.x < CardWidth - Pad))) {
        const auto& item = view.segments[static_cast<std::size_t>(best->segment)];
        HGDIOBJ previous = SelectObject(dc, item.font);
        int index = best->start;
        if (best->length > 0) {
            std::vector<int> extents(static_cast<std::size_t>(best->length));
            int fit = 0;
            SIZE size{};
            GetTextExtentExPointW(dc, item.text.c_str() + best->start, best->length, INT_MAX, &fit, extents.data(), &size);
            int left = 0;
            const int x = at.x - best->x;
            int offset = 0;
            while (offset < best->length) {
                const int right = extents[static_cast<std::size_t>(offset)];
                if (x < (left + right) / 2) break;
                left = right;
                ++offset;
            }
            index += offset;
        }
        SelectObject(dc, previous);
        caret = {best->segment, index};
        found = true;
    }
    ReleaseDC(window_, dc);
    return found;
}

bool PlayerCard::paint_frame(HDC dc, int width, int height) const {
    if (!shown_) return false;
    const int saved = SaveDC(dc);
    const int x = state_.spectator.empty() ? width - CardWidth : 0;
    const int y = 0;
    IntersectClipRect(dc, x, y, std::min(x + CardWidth, width), std::min(y + height_, height));
    SetViewportOrgEx(dc, x, y, nullptr);
    paint(dc, CardWidth, height_);
    RestoreDC(dc, saved);
    return true;
}

void PlayerCard::paint(HDC dc, int width, int height) const {
    RECT all{0, 0, width, height};
    HBRUSH fill = CreateSolidBrush(Background);
    FillRect(dc, &all, fill);
    DeleteObject(fill);
    HBRUSH frame = CreateSolidBrush(Border);
    FrameRect(dc, &all, frame);
    DeleteObject(frame);
    SetBkMode(dc, TRANSPARENT);

    SelectObject(dc, label_font());
    SetTextColor(dc, RGB(200, 200, 200));
    RECT cross = cross_rect();
    if (state_.vehicle && state_.spectator.empty())
        DrawTextW(dc, L"X", -1, &cross, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT collapse = collapse_rect();
    DrawTextW(dc, collapsed_ ? L"+" : L"-", -1, &collapse,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, state_.spectator.empty() ? title_font() : label_font());
    RECT header{Pad, Pad, collapse.left - 8, header_height(dc, state_) - Pad};
    if (!state_.spectator.empty() || collapsed_) {
        const std::wstring heading = !state_.spectator.empty() ? L"Spectating: " + state_.spectator : state_.title;
        if (state_.spectator.empty()) SetTextColor(dc, state_.title_color);
        DrawTextW(dc, heading.c_str(), -1, &header,
                  DT_LEFT | DT_NOPREFIX | (state_.spectator.empty() ? DT_SINGLELINE | DT_END_ELLIPSIS : DT_WORDBREAK));
    }
    if (collapsed_) return;

    auto first = anchor_;
    auto last = caret_;
    if (std::tie(last.segment, last.index) < std::tie(first.segment, first.index)) std::swap(first, last);
    const bool highlight = has_selection();
    HBRUSH selection = CreateSolidBrush(RGB(54, 111, 171));
    const auto view = layout(dc, state_);
    int labelled = -1;
    for (const auto& line : view.lines) {
        const auto& item = view.segments[static_cast<std::size_t>(line.segment)];
        if (!item.label.empty() && labelled != line.segment) {
            labelled = line.segment;
            SelectObject(dc, label_font());
            SetTextColor(dc, RGB(130, 180, 235));
            RECT label_area{Pad, line.y, Pad + LabelWidth - 6, line.y + 16};
            DrawTextW(dc, item.label.c_str(), -1, &label_area, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        SelectObject(dc, item.font);
        if (highlight && line.segment >= first.segment && line.segment <= last.segment) {
            const int line_end = line.start + line.length;
            const int from = line.segment == first.segment ? std::max(line.start, first.index) : line.start;
            const int to = line.segment == last.segment ? std::min(line_end, last.index) : line_end;
            const bool continues = line.segment != last.segment || last.index > line_end;
            if (to > from || (continues && from <= line_end && (line.segment != first.segment || first.index <= line_end))) {
                SIZE before{};
                SIZE upto{};
                GetTextExtentPoint32W(dc, item.text.c_str() + line.start, from - line.start, &before);
                GetTextExtentPoint32W(dc, item.text.c_str() + line.start, std::max(to, from) - line.start, &upto);
                RECT band{line.x + before.cx, line.y, line.x + upto.cx + (continues ? 4 : 0), line.y + line.height};
                FillRect(dc, &band, selection);
            }
        }
        SetTextColor(dc, item.color);
        TextOutW(dc, line.x, line.y, item.text.c_str() + line.start, line.length);
    }
    DeleteObject(selection);

    SelectObject(dc, label_font());
    if (copied_ && highlight) {
        SetTextColor(dc, RGB(120, 200, 140));
        RECT note{Pad, height - Pad - buttons_height() - 22, width - Pad, height - Pad - buttons_height() - 4};
        DrawTextW(dc, L"Copied to clipboard", -1, &note, DT_RIGHT | DT_SINGLELINE);
    }
    if (state_.spectator.empty())
        button(dc, close_rect(height), L"Close", RGB(70, 70, 80), RGB(255, 255, 255));
    if (state_.vehicle) return;
    button(dc, follow_rect(height), state_.followed ? L"Following" : L"Follow",
           state_.followed ? RGB(45, 70, 50) : RGB(40, 120, 60), RGB(255, 255, 255));
    const COLORREF idle = state_.has_voice ? RGB(60, 66, 90) : RGB(45, 47, 55);
    const COLORREF ink = state_.has_voice ? RGB(255, 255, 255) : RGB(130, 130, 140);
    button(dc, solo_rect(height), state_.solo ? L"Hearing only them" : L"Hear only them",
           state_.solo ? RGB(40, 100, 160) : idle, ink);
    button(dc, mute_rect(height), state_.muted ? L"Muted" : L"Mute",
           state_.muted ? RGB(150, 60, 60) : idle, ink);
}

LRESULT PlayerCard::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
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
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR: {
        POINT at{};
        GetCursorPos(&at);
        ScreenToClient(window_, &at);
        const RECT cross = cross_rect();
        Caret ignored;
        const bool header = header_at(at);
        const bool text = !header && !PtInRect(&cross, at) && caret_at(at, true, ignored);
        SetCursor(LoadCursorW(nullptr, selecting_ || text ? IDC_IBEAM : header ? IDC_HAND : IDC_ARROW));
        return TRUE;
    }
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONDOWN: {
        const POINT at{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        RECT client{};
        GetClientRect(window_, &client);
        const RECT follow = follow_rect(client.bottom);
        const RECT close = close_rect(client.bottom);
        const RECT solo = solo_rect(client.bottom);
        const RECT mute = mute_rect(client.bottom);
        const RECT cross = cross_rect();
        Caret hit;
        if (header_at(at)) {
            if (message == WM_LBUTTONDBLCLK) return 0;
            collapsed_ = !collapsed_;
            selecting_ = false;
            if (GetCapture() == window_) ReleaseCapture();
            anchor_ = caret_ = {};
            copied_ = false;
            update(state_);
            InvalidateRect(window_, nullptr, FALSE);
        }
        else if (state_.vehicle && state_.spectator.empty() && PtInRect(&cross, at) && close_) close_();
        else if (!collapsed_ && PtInRect(&follow, at) && follow_) follow_();
        else if (!collapsed_ && PtInRect(&solo, at) && solo_ && state_.has_voice) solo_();
        else if (!collapsed_ && PtInRect(&mute, at) && mute_ && state_.has_voice) mute_();
        else if (!collapsed_ && state_.spectator.empty() && PtInRect(&close, at) && close_) close_();
        else if (caret_at(at, true, hit)) {
            copied_ = false;
            if (message == WM_LBUTTONDBLCLK) {
                // Double-click highlights the whole value and copies it.
                HDC dc = GetDC(window_);
                const auto view = layout(dc, state_);
                ReleaseDC(window_, dc);
                anchor_ = {hit.segment, 0};
                caret_ = {hit.segment, static_cast<int>(view.segments[static_cast<std::size_t>(hit.segment)].text.size())};
                copied_ = copy_selection();
            } else {
                anchor_ = caret_ = hit;
                selecting_ = true;
                SetCapture(window_);
            }
            InvalidateRect(window_, nullptr, FALSE);
        } else if (has_selection()) {
            anchor_ = caret_ = {};
            copied_ = false;
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (selecting_) {
            const POINT at{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
            Caret hit;
            if (caret_at(at, false, hit) && (hit.segment != caret_.segment || hit.index != caret_.index)) {
                caret_ = hit;
                InvalidateRect(window_, nullptr, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (selecting_) {
            selecting_ = false;
            ReleaseCapture();
            copied_ = copy_selection();
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        selecting_ = false;
        return 0;
    default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
}

LRESULT CALLBACK PlayerCard::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    PlayerCard* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<PlayerCard*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->window_ = window;
    } else {
        self = reinterpret_cast<PlayerCard*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (self) return self->handle(message, wparam, lparam);
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace gmod::ui
