#pragma once

namespace gmod::demo {

// Chat categories of the Perpheads chat system: the command each is typed with
// (GM.chatPrefixes in chatsystem/cl_chat.lua) and its colour (ColorIDs in sh_chat.lua).
// The server's display names are translation keys that exist in no installed file, so the
// command stands in for the name.
struct ChatStyle {
    const char* name;
    unsigned char r, g, b;
};

inline const ChatStyle& chat_style(int category) {
    static constexpr ChatStyle styles[] = {
        {"CHAT", 50, 50, 150},      // 0
        {"LOCAL", 120, 247, 210},   // 1
        {"LOOC", 150, 150, 150},    // 2
        {"OOC", 255, 255, 255},     // 3
        {"WHISPER", 119, 192, 212}, // 4
        {"YELL", 218, 121, 9},      // 5
        {"PM", 66, 212, 30},        // 6
        {"ME", 220, 72, 47},        // 7
        {"BROADCAST", 229, 14, 70}, // 8
        {"911", 245, 15, 12},       // 9
        {"RADIO", 15, 161, 174},    // 10
        {"ORG", 218, 11, 174},      // 11
        {"ADVERT", 202, 208, 184},  // 12
        {"ADMIN", 24, 231, 122},    // 13
        {"REPORT", 190, 55, 45},    // 14
        {"ROLL", 25, 209, 65},      // 15
        {"TAXI", 223, 208, 14},     // 16
        {"ROADCREW", 234, 185, 120},// 17
        {"HELP", 233, 223, 76},     // 18
        {"DESC", 236, 122, 31},     // 19
        {"REPLY", 66, 212, 30},     // 20
        {"PANIC", 245, 15, 12},     // 21
        {"ACT", 25, 15, 12},        // 22
        {"COURIER", 20, 222, 155},  // 23
        {"PBC", 56, 62, 242},       // 24
        {"CALL", 66, 212, 30},      // 25
        {"FLIP", 25, 209, 65},      // 26
        {"DEV", 255, 0, 0},         // 27
        {"STAFF", 213, 97, 59},     // 28
        {"EVENT", 255, 128, 0},     // 29
        {"FBC", 237, 166, 33},      // 30
        {"CAMPAIGN", 229, 14, 70},  // 31
        {"UNIT", 15, 121, 174},     // 32
        {"INC", 15, 81, 174},       // 33
        {"DRAW", 25, 209, 65},      // 34
        {"DICE", 25, 209, 65},      // 35
        {"EMSBC", 168, 0, 168},     // 36
        {"TPBC", 255, 174, 249},    // 37
        {"SMT", 17, 174, 127},      // 38
        {"TS", 17, 174, 127},       // 39
    };
    constexpr int count = static_cast<int>(sizeof(styles) / sizeof(styles[0]));
    return styles[category >= 0 && category < count ? category : 0];
}

} // namespace gmod::demo
