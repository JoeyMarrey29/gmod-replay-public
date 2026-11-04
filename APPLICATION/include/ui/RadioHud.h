#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace gmod::ui {

struct RadioHudLine {
    std::wstring text;  // "SGT | Zovetta Aren"
    COLORREF color = RGB(255, 255, 255);
};

struct RadioHudChannel {
    std::wstring name;
    std::vector<RadioHudLine> talking;
};

struct RadioHudState {
    bool available = false;  // the recording player has a government radio
    bool muted = false;
    std::vector<RadioHudChannel> channels;  // only channels with someone transmitting
};

// The server's radio HUD at the top of the screen: who is transmitting on each channel,
// with a button that mutes or unmutes the radio. Colour-keyed overlay: the empty parts
// are click-through, so only the button takes mouse input.
class RadioHud {
public:
    RadioHud(HWND owner, std::function<void()> toggle_mute);
    ~RadioHud();
    RadioHud(const RadioHud&) = delete;
    RadioHud& operator=(const RadioHud&) = delete;

    void update(const RadioHudState& state);

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    void paint(const RadioHudState& state, int width, int height);
    static RECT button_rect();

    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    std::function<void()> toggle_mute_;
    RadioHudState last_;
    bool shown_ = false;
    int placed_x_ = -1;
    int placed_y_ = -1;
    int placed_width_ = 0;
    int placed_height_ = 0;
    bool painted_ = false;
};

} // namespace gmod::ui
