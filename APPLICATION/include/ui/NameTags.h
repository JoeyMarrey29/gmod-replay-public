#pragma once

#include <windows.h>

#include <string>
#include <vector>
#include "ui/ScopeProjection.h"

namespace gmod::ui {

// One player's tag, already projected into client pixels.
struct NameTag {
    float x = 0.0f;  // horizontal centre
    float y = 0.0f;  // bottom edge of the text block
    float depth = 0.0f;
    std::wstring name;
    std::wstring steam;  // Steam profile name, drawn small above the character name
    std::wstring job;  // empty for citizens
    std::wstring status;  // weapon state: "SCOPED IN", "AIMING", "RELOADING"; empty otherwise
    bool hovered = false;  // under the cursor (clickable) or open in the player card
    COLORREF color = RGB(0, 255, 0);
    bool scoped = false;
};

// Shared label rasterizer. Live playback paints into the DirectX frame texture;
// tests/previews can use a memory device context. No overlay window is created.
class NameTags {
public:
    NameTags() = delete;
    static void paint(HDC dc, int width, int height, const std::vector<NameTag>& tags,
                      const ScopeProjection& scope = {});
};

} // namespace gmod::ui
