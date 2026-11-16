// (sloperinaRecomp) what both drawings of sloperinaGZ's panels share (vk_sloperina.h: GDI bitmaps, Hookshot's menu;
// vk_sloperina_imgui.h: Dear ImGui, Longshot's menu): the panels' layout, gz's colours, where a panel goes, and the exe's
// hos_sloperina_screen. Included by them, inside vk_menu.cpp (patch_tools.py).
#pragma once

// gfx.h's gfx_screen_t, field for field (LP64 both sides: the same layout on AArch64 and x86-64). The module sends its
// size; a different one (a module from another version) is not drawn.
constexpr int kSgCols = 64, kSgRows = 26, kSgPanels = 4, kSgHint = 256;
struct SgPanel {
    char ch[kSgRows][kSgCols];
    uint8_t style[kSgRows][kSgCols];
    int rows, cols;
    uint8_t corner;
    uint32_t hash;
    uint8_t alpha[kSgRows];
    int16_t x, y;
    bool clear;
    uint8_t pad;
    int8_t sticks[4];
    uint8_t clicks, device;
    int8_t yoff;
    uint8_t scale_pct;
    uint8_t kind[kSgRows];
    char hint[kSgHint];
    uint8_t scroll_row, scroll_rows;
    uint16_t scroll_items, scroll_top;
};
struct SgScreen { SgPanel panel[kSgPanels]; };

enum { SG_TOP_LEFT, SG_TOP_RIGHT, SG_BOTTOM_LEFT, SG_BOTTOM_RIGHT, SG_TOP_CENTRE, SG_BOTTOM_CENTRE, SG_CUSTOM };
enum { SG_PANEL_LOG = 2 };

// gz's colours (the module's nvn.c style_rgb, bg_rgb): normal, selected, title, dim, notice, log, faint
const uint8_t sg_style_rgb[7][3] = {{0xFF, 0xFF, 0xFF}, {0x80, 0xFF, 0x80}, {0xA0, 0xC8, 0xFF}, {0x90, 0x90, 0x90},
                                    {0xFF, 0xE0, 0x60}, {0xC0, 0xC0, 0xC0}, {0x50, 0x50, 0x58}};
const uint8_t sg_bg_rgb[3] = {0x18, 0x18, 0x20};

// The panel's top-left corner in the window (w x h), for a bitmap bw x bh
void sg_place(const SgPanel& p, int index, int w, int h, int bw, int bh, int scale, int& x, int& y) {
    const int m = 16 * scale;
    switch (index == 0 ? SG_TOP_LEFT : index == SG_PANEL_LOG ? SG_BOTTOM_RIGHT : p.corner) {
    case SG_TOP_RIGHT: x = w - bw - m, y = m; break;
    case SG_BOTTOM_LEFT: x = m, y = h - bh - m; break;
    case SG_BOTTOM_RIGHT: x = w - bw - m, y = h - bh - m; break;
    case SG_TOP_CENTRE: x = (w - bw) / 2, y = m; break;
    case SG_BOTTOM_CENTRE: x = (w - bw) / 2, y = h - bh - m; break;
    case SG_CUSTOM: x = p.x * w / 1000, y = p.y * h / 1000; break;
    default: x = m, y = m; break;
    }
    x = std::clamp(x, 0, std::max(0, w - bw));   // (a keyed bitmap is copied whole or not at all)
    y = std::clamp(y, 0, std::max(0, h - bh));
}

// the exe's hos_sloperina_screen (host/sloperina.c), or null in an exe without it
using SgScreenFn = int (*)(void*, uint32_t, uint32_t*);
SgScreenFn sg_screen_fn() {
    static SgScreenFn fn = [] {
#ifdef _WIN32
        return (SgScreenFn)(void*)GetProcAddress(GetModuleHandleA(nullptr), "hos_sloperina_screen");
#else
        return (SgScreenFn)dlsym(RTLD_DEFAULT, "hos_sloperina_screen");
#endif
    }();
    return fn;
}

