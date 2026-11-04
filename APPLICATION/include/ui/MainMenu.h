#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "ui/ReleaseNotes.h"
#include "ui/Updates.h"

namespace gmod::ui {

struct MenuResult {
    enum class Action { None, OpenDemo, FreeRoam, Quit };
    Action action = Action::None;
    std::filesystem::path demo;
};

// One row of the demo browser: a folder (which can be opened to show what is inside) or a
// demo file, like the explorer tree in VS Code.
struct DemoTreeNode {
    std::filesystem::path path;
    std::wstring label;
    std::wstring subtitle;
    std::wstring detail;
    bool folder = false;
    bool expanded = false;
    bool scanned = false;
    int depth = 0;
    std::int64_t modified = 0;  // FILETIME ticks, newest demos first
    std::vector<DemoTreeNode> children;
};

// GMod-style main menu drawn with GDI before the Direct3D swap chain exists.
class MainMenu {
public:
    explicit MainMenu(std::filesystem::path application_root, std::function<void()> clear_gpu_cache = {});

    void paint(HWND window);
    // Draws the menu into any device context (the window's own, or a bitmap for tests).
    void draw(HWND window, HDC target);
    // Each input handler returns true when the menu needs repainting.
    bool mouse_move(HWND window, int x, int y);
    bool mouse_down(HWND window, int x, int y);
    bool mouse_up(HWND window, int x, int y);
    bool cancel_mouse_selection();
    bool mouse_wheel(HWND window, int delta);
    bool key_down(HWND window, WPARAM key);
    void start_updates();
    bool poll_updates();
    bool updates_busy() const { return updates_.busy(); }

    bool finished() const { return result_.action != MenuResult::Action::None; }
    const MenuResult& result() const { return result_; }

    // Remembers a demo so it shows up in the list next time.
    static void remember(const std::filesystem::path& application_root,
                         const std::filesystem::path& demo);
    // Garry's Mod's Perpheads demo folder on this computer (empty when not found).
    static std::filesystem::path find_gmod_demo_folder();

private:
    struct Layout;
    struct SelectableText {
        RECT rect{};
        std::wstring text;
        std::size_t begin = 0;
        int font_height = 0;
        int font_weight = FW_NORMAL;
        bool single_line = false;
    };
    Layout layout(HWND window) const;
    void load_tree();
    void scan(DemoTreeNode& node);
    void rebuild_rows();
    void toggle(DemoTreeNode& node);
    void reveal(int row, const Layout& layout);
    void choose_file(HWND window);
    void change_folder(HWND window);
    void detect_folder();
    // Rescans Garry's Mod for PERP addons and maps and refreshes the PERP data files.
    void refresh_assets(HWND window);
    void activate_item(HWND window, int item);
    void activate_row(int row);
    void clamp_scroll(const Layout& layout);
    void load_background();
    void draw_update_log(HDC dc, int width, int height);
    void draw_selectable_text(HDC dc, RECT rect, const std::wstring& value,
                              COLORREF color, int font_height, int font_weight,
                              bool single_line = false, bool device_coordinates = false);
    std::size_t selection_position(HDC dc, int x, int y) const;
    bool update_selection(int x, int y);
    bool copy_selection() const;
    bool update_log_key(WPARAM key);
    void draw_updates(HDC dc, int width, int height);
    void update_action(const std::string& action);
    void draw_cache(HDC dc, int width, int height);
    void cache_action(int item);
    bool save_cache_limit(bool confirmed = false);
    bool move_cache_slider(int x);
    void refresh_cache_usage();

    std::filesystem::path root_;
    std::function<void()> clear_gpu_cache_;
    std::filesystem::path folder_;  // the folder the tree shows
    bool folder_detected_ = true;   // found automatically rather than picked
    std::vector<DemoTreeNode> tree_;
    std::vector<DemoTreeNode*> rows_;  // the tree's visible rows, top to bottom
    std::vector<std::uint32_t> background_;  // BGRA, darkened
    int background_width_ = 0;
    int background_height_ = 0;
    int hover_item_ = -1;       // menu items
    int hover_row_ = -1;        // index in rows_
    int hover_button_ = -1;     // 0 = find GMod, 1 = change folder
    int selected_item_ = 0;
    int selected_row_ = 0;
    bool focus_list_ = false;
    bool busy_ = false;         // the folder picker or an asset refresh is running
    bool cache_open_ = false;
    bool cache_clear_confirm_ = false;
    bool cache_limit_confirm_ = false;
    bool cache_slider_dragging_ = false;
    int cache_item_ = 0;
    int cache_limit_ = 25;
    RECT cache_slider_{};
    std::wstring cache_usage_;
    std::wstring cache_message_;
    RECT cache_controls_[5]{};
    int scroll_ = 0;            // first visible row
    MenuResult result_;
    std::wstring message_;
    std::vector<ReleaseNote> releases_;
    std::vector<bool> release_expanded_;
    std::vector<RECT> release_headers_;
    RECT release_viewport_{};
    RECT release_close_{};
    RECT release_status_{};
    std::vector<SelectableText> selectable_runs_;
    std::wstring selectable_document_;
    std::size_t selection_start_ = 0;
    std::size_t selection_end_ = 0;
    bool selection_dragging_ = false;
    bool selection_active_ = false;
    bool update_log_open_ = false;
    int release_selected_ = 0;
    int release_scroll_ = 0;
    int release_scroll_max_ = 0;
    Updates& updates_;
    bool updates_open_ = false;
    bool close_when_install_ready_ = false;
    bool update_prompt_dismissed_ = false;
    bool update_prompt_deferred_ = false;
    std::vector<std::pair<RECT, std::string>> update_buttons_;
    int update_button_ = 0;
};

} // namespace gmod::ui
