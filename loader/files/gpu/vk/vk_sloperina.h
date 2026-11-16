// (sloperinaRecomp) sloperinaGZ's panels over the picture: the menu (top left), the watches, gz's log (bottom right)
// and the input display, as the module publishes them each frame (its gfx.h gfx_screen_t: grids of characters, a style
// a cell). On a Switch the module draws them itself through the game's NVN; in Hookshot the host hands them over
// (host/sloperina.c's hos_sloperina_screen, which the exe exports: found by name, so the back end's interface with the
// exe is unchanged) and they are painted here in gz's colours, each panel its own keyed
// bitmap (black: see-through) copied over the window's image after the game's picture, under the Insert menu.
//
// Included by vk_menu.cpp inside its anonymous namespace, after copy_over (patch_tools.py): it uses Bitmap, gdi_draw, bmp_mu,
// scale_of, Upload, copy_over and dev, which patch_tools.py checks are there.
// Only the GDI calls host/posix/gdi_posix.c has (fonts, FillRect, TextOutA), so Linux draws them too.
#pragma once
// (Linux: patch_tools.py includes <dlfcn.h> at the top of vk_menu.cpp, outside the namespace)

#include "vk_sloperina_common.h"

SgScreen sg_screen;                 // (the latest from the host)
uint32_t sg_serial = ~0u;
uint32_t sg_drawn_hash[kSgPanels];  // (what each bitmap shows: a hash of its panel's bytes, and the scale)
Bitmap sg_bmp[kSgPanels];
Upload sg_ups[kSgPanels][3];
const SgPanel* sg_cur = nullptr;    // (the panel gdi_draw's paint callback draws)
int sg_cell_w = 9, sg_cell_h = 16, sg_pad = 6;

uint32_t sg_hash(const SgPanel& p, int scale) {
    uint32_t h = 2166136261u ^ (uint32_t)scale;
    const uint8_t* b = (const uint8_t*)&p;
    for (size_t i = 0; i < sizeof p; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

COLORREF sg_rgb(const uint8_t c[3], int alpha = 255) {
    return RGB(c[0] * alpha / 255, c[1] * alpha / 255, c[2] * alpha / 255);
}

HFONT sg_font(int px) {
    return CreateFontA(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       NONANTIALIASED_QUALITY, DEFAULT_PITCH, "Consolas");
}

void sg_fill(HDC dc, int x, int y, int w, int h, COLORREF c) {
    RECT r{x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

// One cell's icon (gfx.h GFX_CHECK_OFF ...): drawn with rectangles, or as a stand-in character. Returns the cells it took.
int sg_icon(HDC dc, unsigned char c, int x, int y, COLORREF text, const uint8_t* bg) {
    const int w = sg_cell_w, h = sg_cell_h;
    auto box = [&](bool on) {   // (two cells: gz's checkbox, with its green tick)
        const int s = std::min(2 * w, h) - 4, bx = x + (2 * w - s) / 2, by = y + (h - s) / 2, t = std::max(1, s / 8);
        sg_fill(dc, bx, by, s, s, text);
        sg_fill(dc, bx + t, by + t, s - 2 * t, s - 2 * t, RGB(bg[0] | 1, bg[1], bg[2]));
        if (on) sg_fill(dc, bx + 3 * t, by + 3 * t, s - 6 * t, s - 6 * t, RGB(0, 0xFF, 0));
    };
    const char* stand_in = nullptr;
    COLORREF col = text;
    switch (c) {
    case 0x01: box(false); return 2;
    case 0x02: box(true); return 2;
    case 0x03: return 1;                                          // (the second cell of a two-cell icon)
    case 0x04: stand_in = "x", col = RGB(0xFF, 0, 0); break;      // remove
    case 0x05: stand_in = "> ", col = RGB(247, 209, 57); break;   // folder (two cells)
    case 0x06: stand_in = "*"; break;                             // wrench: edit in memory
    case 0x07: stand_in = "^"; break;
    case 0x08: stand_in = "v"; break;
    case 0x09: stand_in = "+", col = RGB(0, 0xFF, 0); break;
    case 0x0B: stand_in = ">"; break;
    case 0x0C: stand_in = "<"; break;
    case 0x0E: return 2;                                          // the game's picture of an item: not drawn here
    case 0x7F: return 1;                                          // (invisible panel sizing)
    case 0x80: stand_in = "#"; break;                             // lock
    default: return 0;
    }
    SetTextColor(dc, col);
    TextOutA(dc, x, y, stand_in, (int)strlen(stand_in));
    return (int)strlen(stand_in);
}

void sg_paint(HDC dc, uint32_t w, uint32_t h, int scale) {
    const SgPanel& p = *sg_cur;
    if (!p.clear) {   // (the background, a hair off black so the keyed copy keeps it)
        sg_fill(dc, 0, 0, (int)w, (int)h, RGB(sg_bg_rgb[0], sg_bg_rgb[1], sg_bg_rgb[2]));
    }
    const uint8_t black[3] = {0, 0, 0};
    const uint8_t* bg = p.clear ? black : sg_bg_rgb;
    HFONT f = sg_font(sg_cell_h);
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextAlign(dc, TA_TOP | TA_LEFT);
    const int yoff = p.yoff * scale;
    for (int r = 0; r < p.rows && r < kSgRows; r++) {
        const int alpha = &p == &sg_screen.panel[SG_PANEL_LOG] || p.clear ? (p.alpha[r] ? p.alpha[r] : 255) : 255;
        const int y = sg_pad + r * sg_cell_h + yoff;
        for (int col = 0; col < p.cols && col < kSgCols;) {
            const unsigned char c = (unsigned char)p.ch[r][col];
            const int st = p.style[r][col] < 7 ? p.style[r][col] : 0;
            const int x = sg_pad + col * sg_cell_w;
            if (!c || c == ' ') { col++; continue; }
            COLORREF text = sg_rgb(sg_style_rgb[st], alpha);
            if (int n = sg_icon(dc, c, x, y, text, bg)) { col += n; continue; }
            if (c < 0x20 || c >= 0x7F) { col++; continue; }
            // (a run of plain characters in one style, one TextOutA)
            int e = col + 1;
            while (e < p.cols && e < kSgCols && (unsigned char)p.ch[r][e] >= 0x20 && (unsigned char)p.ch[r][e] < 0x7F &&
                   p.style[r][e] == p.style[r][col])
                e++;
            SetTextColor(dc, text);
            for (int k = col; k < e; k++)   // (a cell each: the grid stays a grid whatever the font's advance)
                TextOutA(dc, sg_pad + k * sg_cell_w, y, &p.ch[r][k], 1);
            col = e;
        }
    }
    SelectObject(dc, old);
    DeleteObject(f);
}

// (menu_tick) the panels from the host, each drawn again when it changed
void sloperina_tick() {
    const SgScreenFn fn = sg_screen_fn();
    if (!fn) return;
    static SgScreen next;
    uint32_t serial = 0;
    const int n = fn(&next, sizeof next, &serial);
    if (serial == sg_serial) return;
    sg_serial = serial;
    if (n != (int)sizeof next) {
        if (n) {
            static bool told;
            if (!told) told = true, std::fprintf(stderr, "[vk] sloperina: panels of %d bytes, %d expected: not drawn\n", n, (int)sizeof next);
        }
        std::lock_guard lk(bmp_mu);
        for (Bitmap& b : sg_bmp) b.w = 0;
        std::memset(sg_drawn_hash, 0, sizeof sg_drawn_hash);
        return;
    }
    sg_screen = next;
    const int scale = scale_of(dev.swap_extent.height);
    for (int i = 0; i < kSgPanels; i++) {
        const SgPanel& p = sg_screen.panel[i];
        if (p.rows <= 0 || p.cols <= 0) {
            std::lock_guard lk(bmp_mu);
            sg_bmp[i].w = 0, sg_drawn_hash[i] = 0;
            continue;
        }
        const uint32_t hsh = sg_hash(p, scale);
        if (hsh == sg_drawn_hash[i]) continue;
        sg_drawn_hash[i] = hsh;
        const int pct = p.scale_pct ? p.scale_pct : 100;
        sg_cell_h = std::max(8, 16 * scale * pct / 100);
        sg_cell_w = std::max(5, sg_cell_h * 9 / 16);
        sg_pad = std::max(2, sg_cell_h * 3 / 8);
        sg_cur = &p;
        sg_bmp[i].keyed = true;
        gdi_draw(sg_bmp[i], (uint32_t)(p.cols * sg_cell_w + 2 * sg_pad), (uint32_t)(p.rows * sg_cell_h + 2 * sg_pad + std::abs(p.yoff) * scale),
                 sg_paint, scale);
        sg_cur = nullptr;
    }
}

// (menu_record) the panels over the window's image, w x h
void sloperina_record(VkCommandBuffer cmd, VkImage dst, int slot, uint32_t w, uint32_t h) {
    const int scale = scale_of(dev.swap_extent.height);
    for (int i = 0; i < kSgPanels; i++) {
        uint32_t bw, bh;
        {
            std::lock_guard lk(bmp_mu);
            bw = sg_bmp[i].w, bh = sg_bmp[i].h;
        }
        if (!bw) continue;
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = 1, di.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &di);
        int x = 0, y = 0;
        sg_place(sg_screen.panel[i], i, (int)w, (int)h, (int)bw, (int)bh, scale, x, y);
        copy_over(cmd, dst, sg_bmp[i], sg_ups[i][slot], x, y, w, h);
    }
}
