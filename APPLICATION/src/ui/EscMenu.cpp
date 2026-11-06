#include "ui/EscMenu.h"

#include "ui/Settings.h"

#include <algorithm>
#include <cwchar>

namespace gmod::ui {
namespace {

constexpr wchar_t ClassName[] = L"GModReplayEscMenu";
constexpr int MenuWidth = 440;
constexpr int HelpWidth = 660;
constexpr int Pad = 22;
constexpr int RowHeight = 25;
constexpr int HeadingHeight = 28;
constexpr int ButtonHeight = 34;
constexpr COLORREF Background = RGB(24, 28, 34);
constexpr COLORREF Border = RGB(70, 140, 220);
constexpr COLORREF HoverFill = RGB(44, 54, 68);

HFONT make_font(int height, int weight) {
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
}

} // namespace

enum class ItemKind { Heading, Note, Control, Page, Back, Esp, Toggle, VolumeDown, VolumeUp, VolumeTrack, Resume, MainMenu, ConfirmUnload, CancelUnload, Quit };

constexpr int HelpPage = 6;

static int menu_width(int page) { return page == HelpPage ? HelpWidth : MenuWidth; }

// Every control of the replay, shown on the Help page: {heading or key, what it does}.
// An empty description starts a new heading.
struct ControlLine {
    const wchar_t* key;
    const wchar_t* action;
};
constexpr ControlLine Controls[]{
    {L"Camera", nullptr},
    {L"F", L"Switch between following a player and free view"},
    {L"R", L"Switch first / third person (free look in vehicles)"},
    {L"W A S D", L"Move the free view camera"},
    {L"Space / C", L"Move the free view camera up / down"},
    {L"Hold Shift", L"Fly faster in free view"},
    {L"Mouse", L"In free view, click once to capture the mouse and look around"},
    {L"Click + drag", L"Turn the camera in third person, or in free view while\nthe Tab bar is open (the cursor is free there)"},
    {L"Playback", nullptr},
    {L"Tap Shift", L"Play / pause"},
    {L"Tab", L"Show / hide the playback bar: seek, skip, speed, ESP"},
    {L"Players and cars", nullptr},
    {L"Left click", L"Select the player or car under the cursor (the\ncrosshair while the mouse is captured)"},
    {L"Double click", L"Follow the player"},
    {L"Right click", L"Clear the selection"},
    {L"Drag on card", L"Highlight details in the info card; they are copied\nautomatically (double-click copies one value)"},
    {L"Ctrl + C", L"Copy the highlighted card text again"},
    {L"Hold P", L"Player list (it can be pinned open)"},
    {L"Other", nullptr},
    {L"M", L"Mute / unmute all sound"},
    {L"O", L"Copy the camera position"},
    {L"Key left of 1", L"Open / close the console"},
    {L"F1", L"This help"},
    {L"Esc", L"Options menu (also closes the console)"},
};

struct EscMenu::Item {
    ItemKind kind = ItemKind::Heading;
    std::wstring label;
    bool* flag = nullptr;
    RECT rect{};
    int* level = nullptr;
    int page = 0;
    int esp_mode = 0;
    std::wstring key;  // Control rows: the key or mouse action
};

EscMenu::EscMenu(HWND owner, EscMenuActions actions) : owner_(owner), actions_(std::move(actions)) {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &EscMenu::window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = ClassName;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    window_ = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, ClassName, L"", WS_POPUP, 0, 0,
                              MenuWidth, 400, owner_, nullptr, window_class.hInstance, this);
}

EscMenu::~EscMenu() {
    if (window_) DestroyWindow(window_);
}

std::vector<EscMenu::Item> EscMenu::layout(int width, int& height) const {
    auto& options = settings();
    std::vector<Item> items;
    int y = Pad + 44;
    const auto add = [&](ItemKind kind, std::wstring label, bool* flag, int item_height) {
        Item item;
        item.kind = kind;
        item.label = std::move(label);
        item.flag = flag;
        item.rect = {Pad, y, width - Pad, y + item_height};
        items.push_back(std::move(item));
        y += item_height;
    };
    if (confirm_unload_) {
        add(ItemKind::Heading, L"Are you sure you want to unload this scene?", nullptr, HeadingHeight);
        y += 10;
        add(ItemKind::CancelUnload, L"Cancel", nullptr, ButtonHeight);
        y += 6;
        add(ItemKind::ConfirmUnload, L"Yes, return to main menu", nullptr, ButtonHeight);
        height = y + Pad;
        return items;
    }
    const auto note = [&](const wchar_t* text) {
        add(ItemKind::Note, text, nullptr, 36);
    };
    const auto volume = [&](const wchar_t* label, int& value) {
        add(ItemKind::Note, std::wstring(label) + L"  " + std::to_wstring(value) + L"%", nullptr, 20);
        Item down;
        down.kind = ItemKind::VolumeDown;
        down.level = &value;
        down.rect = {Pad, y, Pad + 42, y + 26};
        items.push_back(down);
        Item up = down;
        up.kind = ItemKind::VolumeUp;
        up.rect = {width - Pad - 42, y, width - Pad, y + 26};
        items.push_back(up);
        // The centre is a volume track; its fill also makes muted groups obvious.
        Item track = down;
        track.kind = ItemKind::VolumeTrack;
        track.rect = {Pad + 52, y, width - Pad - 52, y + 26};
        items.push_back(track);
        y += 27;
    };
    if (page_ == 0) {
        add(ItemKind::Heading, L"Settings", nullptr, HeadingHeight);
        const wchar_t* labels[]{L"Display", L"HUD", L"Effects", L"Audio", L"Graphics"};
        for (int page = 1; page <= 5; ++page) {
            add(ItemKind::Page, labels[page - 1], nullptr, ButtonHeight);
            items.back().page = page;
            y += 6;
        }
        add(ItemKind::Heading, L"Help", nullptr, HeadingHeight);
        add(ItemKind::Page, L"Controls  (F1)", nullptr, ButtonHeight);
        items.back().page = HelpPage;
        y += 6;
    } else if (page_ == HelpPage) {
        for (const auto& line : Controls) {
            if (!line.action) {
                add(ItemKind::Heading, line.key, nullptr, HeadingHeight - 4);
                continue;
            }
            const bool two_lines = std::wcschr(line.action, L'\n') != nullptr;
            add(ItemKind::Control, line.action, nullptr, two_lines ? 32 : 18);
            items.back().key = line.key;
        }
        y += 8;
        add(ItemKind::Back, L"Back to options", nullptr, ButtonHeight);
        y += 6;
    } else {
        const wchar_t* headings[]{L"", L"Display", L"HUD", L"Effects", L"Audio", L"Graphics"};
        add(ItemKind::Heading, headings[page_], nullptr, HeadingHeight);
        if (page_ == 1) {
            add(ItemKind::Toggle, L"Names above players", &options.names, RowHeight);
            add(ItemKind::Toggle, L"Steam names on name tags", &options.steam_names, RowHeight);
            add(ItemKind::Heading, L"ESP", nullptr, HeadingHeight);
            const wchar_t* modes[]{L"None", L"Outline", L"Outline through walls"};
            for (int mode = 0; mode < 3; ++mode) {
                add(ItemKind::Esp, modes[mode], nullptr, RowHeight);
                items.back().esp_mode = mode;
            }
            note(L"Names show through walls only with\nOutline through walls enabled.");
        } else if (page_ == 2) {
            add(ItemKind::Toggle, L"Car speed", &options.car_speed, RowHeight);
            add(ItemKind::Toggle, L"Speed in mph (off: km/h)", &options.speed_mph, RowHeight);
            add(ItemKind::Toggle, L"Kill and death feed", &options.kill_feed, RowHeight);
            add(ItemKind::Toggle, L"Radio display", &options.radio_hud, RowHeight);
            add(ItemKind::Toggle, L"NPC dialogue boxes", &options.dialogs, RowHeight);
        } else if (page_ == 3) {
            add(ItemKind::Toggle, L"Bullet holes and decals", &options.decals, RowHeight);
            add(ItemKind::Toggle, L"Muzzle flash", &options.muzzle_flash, RowHeight);
        } else if (page_ == 4) {
            volume(L"Master volume", options.volume);
            volume(L"Cars", options.cars_volume);
            volume(L"Voices", options.voices_volume);
            volume(L"Gunshots", options.gunshots_volume);
            volume(L"Doors", options.doors_volume);
            volume(L"Alarms", options.alarms_volume);
            volume(L"Ambient sounds", options.ambient_volume);
            volume(L"Other", options.other_volume);
            note(L"Other: footsteps, impacts, UI sounds\nand uncategorized effects.");
        } else if (page_ == 5) {
            add(ItemKind::Toggle, L"Water reflections", &options.reflections, RowHeight);
            note(L"Turn off to skip rendering the reflected scene.");
            add(ItemKind::Toggle, L"Lighting", &options.lighting, RowHeight);
            note(L"Off: unlit world and players.\nSkips shading for better FPS.");
            add(ItemKind::Toggle, L"Bank raid lighting", &options.bank_raid_lighting, RowHeight);
            note(L"Requires Lighting. Updates when the bank\nswitches between normal and raid states.");
        }
        y += 6;
        add(ItemKind::Back, L"Back to settings", nullptr, ButtonHeight);
        y += 6;
    }
    y += 6;
    add(ItemKind::Resume, L"Resume", nullptr, ButtonHeight);
    if (page_ == 0) {
        y += 6;
        add(ItemKind::MainMenu, L"Back to main menu", nullptr, ButtonHeight);
        y += 6;
        add(ItemKind::Quit, L"Quit", nullptr, ButtonHeight);
    }
    height = y + Pad;
    return items;
}

void EscMenu::set_visible(bool visible) {
    if (!window_ || visible == visible_) return;
    visible_ = visible;
    if (!visible) {
        confirm_unload_ = false;
        page_ = 0;
        if (drag_level_) {
            drag_level_ = nullptr;
            ReleaseCapture();
            settings().save();
        }
        ShowWindow(window_, SW_HIDE);
        hover_ = -1;
    } else {
        placed_x_ = -1;
        update();
    }
}

void EscMenu::show_help() {
    if (!window_) return;
    confirm_unload_ = false;
    page_ = HelpPage;
    hover_ = -1;
    if (!visible_) set_visible(true);
    else update();
    InvalidateRect(window_, nullptr, FALSE);
}

void EscMenu::update() {
    if (!window_ || !visible_) return;
    if (IsIconic(owner_)) {
        ShowWindow(window_, SW_HIDE);
        return;
    }
    int height = 0;
    const int width = menu_width(page_);
    layout(width, height);
    RECT client{};
    GetClientRect(owner_, &client);
    POINT origin{(client.right - width) / 2, std::max<int>(8, (client.bottom - height) / 2)};
    ClientToScreen(owner_, &origin);
    if (origin.x != placed_x_ || origin.y != placed_y_ || height != placed_height_) {
        SetWindowPos(window_, HWND_TOP, origin.x, origin.y, width, height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        placed_x_ = origin.x;
        placed_y_ = origin.y;
        placed_height_ = height;
        InvalidateRect(window_, nullptr, FALSE);
    }
    if (!IsWindowVisible(window_)) {
        ShowWindow(window_, SW_SHOWNOACTIVATE);
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void EscMenu::paint_preview(HDC dc, int client_width, int client_height) {
    int height = 0;
    const int width = menu_width(page_);
    layout(width, height);
    SetViewportOrgEx(dc, (client_width - width) / 2, std::max<int>(8, (client_height - height) / 2), nullptr);
    paint(dc, width, height);
    SetViewportOrgEx(dc, 0, 0, nullptr);
}

void EscMenu::preview_help() {
    page_ = HelpPage;
}

void EscMenu::activate(const Item& item) {
    auto& options = settings();
    switch (item.kind) {
    case ItemKind::Page:
        page_ = item.page;
        hover_ = -1;
        update();
        break;
    case ItemKind::Back:
        page_ = 0;
        hover_ = -1;
        update();
        break;
    case ItemKind::Esp:
        options.outline_all_players = item.esp_mode != 0;
        options.names_through_walls = item.esp_mode == 2;
        options.save();
        if (actions_.changed) actions_.changed();
        break;
    case ItemKind::Toggle:
        if (item.flag) *item.flag = !*item.flag;
        options.save();
        if (actions_.changed) actions_.changed();
        break;
    case ItemKind::VolumeDown:
        if (item.level) *item.level = std::max(0, *item.level - 10);
        options.save();
        if (actions_.changed) actions_.changed();
        break;
    case ItemKind::VolumeUp:
        if (item.level) *item.level = std::min(100, *item.level + 10);
        options.save();
        if (actions_.changed) actions_.changed();
        break;
    case ItemKind::Resume:
        if (actions_.resume) actions_.resume();
        break;
    case ItemKind::MainMenu:
        confirm_unload_ = true;
        hover_ = -1;
        update();
        break;
    case ItemKind::CancelUnload:
        confirm_unload_ = false;
        hover_ = -1;
        update();
        break;
    case ItemKind::ConfirmUnload:
        confirm_unload_ = false;
        if (actions_.main_menu) actions_.main_menu();
        break;
    case ItemKind::Quit:
        if (actions_.quit) actions_.quit();
        break;
    default:
        break;
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void EscMenu::paint(HDC dc, int width, int height) {
    static const HFONT title_font = make_font(22, FW_BOLD);
    static const HFONT heading_font = make_font(14, FW_BOLD);
    static const HFONT body_font = make_font(14, FW_NORMAL);
    RECT all{0, 0, width, height};
    HBRUSH fill = CreateSolidBrush(Background);
    FillRect(dc, &all, fill);
    DeleteObject(fill);
    HBRUSH frame = CreateSolidBrush(Border);
    FrameRect(dc, &all, frame);
    DeleteObject(frame);
    SetBkMode(dc, TRANSPARENT);

    SelectObject(dc, title_font);
    SetTextColor(dc, RGB(240, 240, 240));
    RECT title{Pad, Pad - 4, width - Pad, Pad + 30};
    DrawTextW(dc, confirm_unload_ ? L"GMod Replay  -  Unload scene?" : page_ == HelpPage ? L"GMod Replay  -  Controls" : L"GMod Replay  -  Options",
              -1, &title, DT_LEFT | DT_SINGLELINE);

    int unused = 0;
    const auto items = layout(width, unused);
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        const bool hot = static_cast<int>(index) == hover_;
        switch (item.kind) {
        case ItemKind::VolumeTrack:
        case ItemKind::Note: {
            if (item.level) {
                RECT track = item.rect;
                track.top += 8;
                track.bottom -= 8;
                HBRUSH empty = CreateSolidBrush(RGB(50, 60, 74));
                FillRect(dc, &track, empty);
                DeleteObject(empty);
                track.right = track.left + (track.right - track.left) * *item.level / 100;
                HBRUSH full = CreateSolidBrush(Border);
                FillRect(dc, &track, full);
                DeleteObject(full);
            } else {
                SelectObject(dc, body_font);
                SetTextColor(dc, RGB(180, 190, 205));
                RECT text = item.rect;
                DrawTextW(dc, item.label.c_str(), -1, &text, DT_LEFT | DT_WORDBREAK);
            }
            break;
        }
        case ItemKind::Control: {
            SelectObject(dc, heading_font);
            SetTextColor(dc, RGB(235, 235, 235));
            RECT key{item.rect.left + 6, item.rect.top, item.rect.left + 136, item.rect.bottom};
            DrawTextW(dc, item.key.c_str(), -1, &key, DT_LEFT | DT_TOP | DT_SINGLELINE);
            SelectObject(dc, body_font);
            SetTextColor(dc, RGB(180, 190, 205));
            RECT text{item.rect.left + 140, item.rect.top, item.rect.right, item.rect.bottom};
            DrawTextW(dc, item.label.c_str(), -1, &text, DT_LEFT | DT_TOP);
            break;
        }
        case ItemKind::Heading: {
            const bool is_volume = item.label.rfind(L"Volume", 0) == 0;
            SelectObject(dc, heading_font);
            SetTextColor(dc, is_volume ? RGB(235, 235, 235) : RGB(130, 180, 235));
            RECT text = item.rect;
            DrawTextW(dc, item.label.c_str(), -1, &text,
                      (is_volume ? DT_CENTER : DT_LEFT) | DT_VCENTER | DT_SINGLELINE);
            if (!is_volume) {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(50, 70, 95));
                HGDIOBJ old = SelectObject(dc, pen);
                MoveToEx(dc, item.rect.left, item.rect.bottom - 3, nullptr);
                LineTo(dc, item.rect.right, item.rect.bottom - 3);
                SelectObject(dc, old);
                DeleteObject(pen);
            }
            break;
        }
        case ItemKind::Esp:
        case ItemKind::Toggle: {
            if (hot) {
                HBRUSH hover = CreateSolidBrush(HoverFill);
                FillRect(dc, &item.rect, hover);
                DeleteObject(hover);
            }
            const int box = 14;
            const int top = item.rect.top + (RowHeight - box) / 2;
            RECT check{item.rect.left + 6, top, item.rect.left + 6 + box, top + box};
            const bool radio = item.kind == ItemKind::Esp;
            const auto& options = settings();
            const int esp_mode = !options.outline_all_players ? 0 : options.names_through_walls ? 2 : 1;
            const bool selected = radio ? item.esp_mode == esp_mode : item.flag && *item.flag;
            if (radio) {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(150, 160, 175));
                auto old_pen = SelectObject(dc, pen);
                auto old_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
                Ellipse(dc, check.left, check.top, check.right, check.bottom);
                if (selected) {
                    HBRUSH on = CreateSolidBrush(RGB(70, 200, 110));
                    SelectObject(dc, on);
                    SelectObject(dc, GetStockObject(NULL_PEN));
                    Ellipse(dc, check.left + 3, check.top + 3, check.right - 3, check.bottom - 3);
                    SelectObject(dc, old_brush);
                    DeleteObject(on);
                }
                SelectObject(dc, old_pen);
                SelectObject(dc, old_brush);
                DeleteObject(pen);
            } else {
                HBRUSH border = CreateSolidBrush(RGB(150, 160, 175));
                FrameRect(dc, &check, border);
                DeleteObject(border);
            }
            if (!radio && selected) {
                RECT inner{check.left + 3, check.top + 3, check.right - 3, check.bottom - 3};
                HBRUSH on = CreateSolidBrush(RGB(70, 200, 110));
                FillRect(dc, &inner, on);
                DeleteObject(on);
            }
            SelectObject(dc, body_font);
            SetTextColor(dc, RGB(230, 230, 230));
            RECT text{item.rect.left + 30, item.rect.top, item.rect.right, item.rect.bottom};
            DrawTextW(dc, item.label.c_str(), -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        case ItemKind::Page:
        case ItemKind::Back:
        case ItemKind::VolumeDown:
        case ItemKind::VolumeUp:
        case ItemKind::Resume:
        case ItemKind::MainMenu:
        case ItemKind::ConfirmUnload:
        case ItemKind::CancelUnload:
        case ItemKind::Quit: {
            const bool primary = item.kind == ItemKind::Resume;
            const bool danger = item.kind == ItemKind::Quit;
            COLORREF color = primary ? RGB(40, 120, 60) : danger ? RGB(120, 45, 45) : RGB(60, 66, 80);
            if (hot) color = RGB(GetRValue(color) + 25, GetGValue(color) + 25, GetBValue(color) + 25);
            HBRUSH button = CreateSolidBrush(color);
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
            HGDIOBJ old_brush = SelectObject(dc, button);
            HGDIOBJ old_pen = SelectObject(dc, pen);
            RoundRect(dc, item.rect.left, item.rect.top, item.rect.right, item.rect.bottom, 8, 8);
            SelectObject(dc, old_brush);
            SelectObject(dc, old_pen);
            DeleteObject(button);
            DeleteObject(pen);
            SelectObject(dc, heading_font);
            SetTextColor(dc, RGB(255, 255, 255));
            const wchar_t* label = item.kind == ItemKind::VolumeDown ? L"-"
                : item.kind == ItemKind::VolumeUp ? L"+" : item.label.c_str();
            RECT text = item.rect;
            DrawTextW(dc, label, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        }
    }
}

void EscMenu::set_drag_level(int x) {
    if (!drag_level_) return;
    const int value = std::clamp<int>((x - drag_rect_.left) * 100 / (drag_rect_.right - drag_rect_.left), 0, 100);
    if (*drag_level_ == value) return;
    *drag_level_ = value;
    if (actions_.changed) actions_.changed();
    InvalidateRect(window_, nullptr, FALSE);
}

LRESULT EscMenu::handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
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
    case WM_MOUSEMOVE: {
        if (drag_level_) {
            set_drag_level(static_cast<short>(LOWORD(lparam)));
            return 0;
        }
        if (!tracking_leave_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
            TrackMouseEvent(&track);
            tracking_leave_ = true;
        }
        const POINT at{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        RECT client{};
        GetClientRect(window_, &client);
        int unused = 0;
        const auto items = layout(client.right, unused);
        int hit = -1;
        for (std::size_t index = 0; index < items.size(); ++index) {
            if (items[index].kind != ItemKind::Heading && items[index].kind != ItemKind::Note && items[index].kind != ItemKind::Control && PtInRect(&items[index].rect, at)) {
                hit = static_cast<int>(index);
            }
        }
        if (hit != hover_) {
            hover_ = hit;
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_leave_ = false;
        hover_ = -1;
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (drag_level_) {
            set_drag_level(static_cast<short>(LOWORD(lparam)));
            drag_level_ = nullptr;
            ReleaseCapture();
            settings().save();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (drag_level_) {
            drag_level_ = nullptr;
            settings().save();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const POINT at{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        RECT client{};
        GetClientRect(window_, &client);
        int unused = 0;
        const auto items = layout(client.right, unused);
        for (const auto& item : items) {
            if (item.kind != ItemKind::Heading && item.kind != ItemKind::Note && item.kind != ItemKind::Control && PtInRect(&item.rect, at)) {
                if (item.kind == ItemKind::VolumeTrack) {
                    drag_level_ = item.level;
                    drag_rect_ = item.rect;
                    SetCapture(window_);
                    set_drag_level(at.x);
                } else {
                    activate(item);
                }
                break;
            }
        }
        return 0;
    }
    default:
        return DefWindowProcW(window_, message, wparam, lparam);
    }
}

LRESULT CALLBACK EscMenu::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    EscMenu* self = nullptr;
    if (message == WM_NCCREATE) {
        self = static_cast<EscMenu*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->window_ = window;
    } else {
        self = reinterpret_cast<EscMenu*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    if (self) return self->handle(message, wparam, lparam);
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace gmod::ui
