#include "ui/NameTags.h"

#include <algorithm>
#include <cmath>

namespace gmod::ui {
namespace {

HFONT make_font(int height, int weight) {
    return CreateFontW(-height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Tahoma");
}

void outlined_text(HDC dc, const std::wstring& text, int center_x, int bottom, COLORREF color) {
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    const int left = center_x - size.cx / 2;
    const int top = bottom - size.cy;
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

void NameTags::paint(HDC dc, int width, int height, const std::vector<NameTag>& tags,
                     const ScopeProjection& scope) {
    static const HFONT name_font = make_font(13, FW_BOLD);
    static const HFONT job_font = make_font(11, FW_NORMAL);
    HGDIOBJ previous = SelectObject(dc, name_font);
    SetBkMode(dc, TRANSPARENT);
    // Far tags first so near ones draw over them.
    std::vector<const NameTag*> ordered;
    ordered.reserve(tags.size());
    for (const auto& tag : tags) ordered.push_back(&tag);
    std::sort(ordered.begin(), ordered.end(),
              [](const NameTag* a, const NameTag* b) { return a->depth > b->depth; });
    const int saved = SaveDC(dc);
    const HRGN lens = CreateRectRgn(0, 0, 0, 0);
    for (const auto& triangle : scope.triangles) {
        POINT points[3];
        for (int i = 0; i < 3; ++i) {
            points[i] = {static_cast<LONG>(std::lround((triangle[i].clip[0]/triangle[i].clip[3]+1)*0.5f*width)),
                         static_cast<LONG>(std::lround((1-triangle[i].clip[1]/triangle[i].clip[3])*0.5f*height))};
        }
        const HRGN part = CreatePolygonRgn(points, 3, WINDING);
        CombineRgn(lens, lens, part, RGN_OR);
        DeleteObject(part);
    }
    const HRGN outside = CreateRectRgn(0, 0, width, height);
    CombineRgn(outside, outside, lens, RGN_DIFF);
    for (const auto* tag : ordered) {
        SelectClipRgn(dc, tag->scoped ? lens : outside);
        const int center = static_cast<int>(std::lround(tag->x));
        int bottom = static_cast<int>(std::lround(tag->y));
        const int block_bottom = bottom;
        int block_width = 0;
        const auto widen = [&](const std::wstring& text) {
            SIZE size{};
            GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
            block_width = std::max<int>(block_width, size.cx);
        };
        if (!tag->status.empty()) {
            SelectObject(dc, job_font);
            outlined_text(dc, tag->status, center, bottom, RGB(255, 170, 40));
            widen(tag->status);
            TEXTMETRICW metrics{};
            GetTextMetricsW(dc, &metrics);
            bottom -= metrics.tmHeight - 1;
        }
        if (!tag->job.empty()) {
            SelectObject(dc, job_font);
            outlined_text(dc, tag->job, center, bottom, tag->color);
            widen(tag->job);
            TEXTMETRICW metrics{};
            GetTextMetricsW(dc, &metrics);
            bottom -= metrics.tmHeight - 1;
        }
        SelectObject(dc, name_font);
        outlined_text(dc, tag->name, center, bottom, tag->color);
        widen(tag->name);
        {
            TEXTMETRICW metrics{};
            GetTextMetricsW(dc, &metrics);
            bottom -= metrics.tmHeight - 1;
        }
        if (!tag->steam.empty()) {
            SelectObject(dc, job_font);
            outlined_text(dc, tag->steam, center, bottom, RGB(190, 190, 190));
            widen(tag->steam);
            TEXTMETRICW metrics{};
            GetTextMetricsW(dc, &metrics);
            bottom -= metrics.tmHeight - 1;
        }
        if (tag->hovered) {
            // A frame around the tag marks the player the cursor would select.
            const RECT frame{center - block_width / 2 - 4, bottom - 2, center + block_width / 2 + 5,
                             block_bottom + 3};
            HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
            FrameRect(dc, &frame, white);
            DeleteObject(white);
        }
    }
    RestoreDC(dc, saved);
    DeleteObject(outside);
    DeleteObject(lens);
    SelectObject(dc, previous);
}

} // namespace gmod::ui
