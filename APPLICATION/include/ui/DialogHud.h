#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace gmod::ui {

struct DialogHudState {
    bool visible = false;
    std::wstring title;               // the NPC's name
    std::vector<std::wstring> lines;  // text the demo recorded for the dialogue
    std::vector<std::wstring> options;
    bool reconstructed = false;
};

// The NPC dialogue box shown while the recording player talks to an NPC. Colour-keyed and
// click-through, like the other overlays.
class DialogHud {
public:
    explicit DialogHud(HWND owner);
    ~DialogHud();
    DialogHud(const DialogHud&) = delete;
    DialogHud& operator=(const DialogHud&) = delete;

    void update(const DialogHudState& state);

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void paint(const DialogHudState& state, int width, int height);

    HWND owner_ = nullptr;
    HWND window_ = nullptr;
    DialogHudState last_;
    bool shown_ = false;
    bool painted_ = false;
    int placed_x_ = -1;
    int placed_y_ = -1;
    int placed_height_ = 0;
};

} // namespace gmod::ui
