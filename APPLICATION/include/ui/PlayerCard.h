#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace gmod::ui {

struct PlayerCardState {
    bool visible = false;
    std::wstring title;  // character name, or the vehicle's name
    COLORREF title_color = RGB(0, 255, 0);
    std::vector<std::pair<std::wstring, std::wstring>> rows;  // label, value
    bool vehicle = false;   // a vehicle's card: only the Close button applies
    bool followed = false;  // the card's player is already being followed
    bool has_voice = false;  // the demo holds voice clips for this player
    bool solo = false;       // only this player's voice is being heard
    bool muted = false;      // this player's voice is silenced
    std::wstring spectator;  // Steam name - character name when automatically following
};

// The box that opens when a player or vehicle is clicked: everything the demo knows about them,
// with buttons to follow them or close the box. Spectator cards dock top-left;
// clicked player/vehicle cards dock top-right.
// The title and values can be highlighted by dragging; the highlight is copied to the clipboard.
class PlayerCard {
public:
    PlayerCard(HWND owner, std::function<void()> follow, std::function<void()> solo,
               std::function<void()> mute, std::function<void()> close);
    ~PlayerCard();
    PlayerCard(const PlayerCard&) = delete;
    PlayerCard& operator=(const PlayerCard&) = delete;

    void update(const PlayerCardState& state);
    // Copies the highlighted text (Ctrl+C in the replay window). False when nothing is highlighted.
    bool copy_selection() const;
    bool paint_frame(HDC dc, int width, int height) const;

private:
    struct Caret {
        int segment = 0;
        int index = 0;
    };
    struct Segment {
        std::wstring label;  // empty for the title
        std::wstring text;
        HFONT font = nullptr;
        COLORREF color = 0;
    };
    struct Line {
        int segment = 0;
        int start = 0;
        int length = 0;
        int x = 0;
        int y = 0;
        int height = 0;
    };
    struct Layout {
        std::vector<Segment> segments;
        std::vector<Line> lines;
        int bottom = 0;
    };

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    Layout layout(HDC dc, const PlayerCardState& state) const;
    int measure(const PlayerCardState& state) const;
    int header_height(HDC dc, const PlayerCardState& state) const;
    bool header_at(POINT at) const;
    void paint(HDC dc, int width, int height) const;
    bool caret_at(POINT at, bool strict, Caret& caret) const;
    bool has_selection() const;
    std::wstring selected_text() const;
    int buttons_height() const;
    RECT follow_rect(int height) const;
    RECT close_rect(int height) const;
    RECT solo_rect(int height) const;
    RECT mute_rect(int height) const;
    RECT cross_rect() const;
    RECT collapse_rect() const;

    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    std::function<void()> follow_;
    std::function<void()> solo_;
    std::function<void()> mute_;
    std::function<void()> close_;
    PlayerCardState state_;
    bool shown_ = false;
    bool collapsed_ = false;
    int placed_x_ = -1;
    int placed_y_ = -1;
    int height_ = 0;
    Caret anchor_;
    Caret caret_;
    bool selecting_ = false;
    bool copied_ = false;
};

} // namespace gmod::ui
