// (sloperinaRecomp) sloperinaGZ's panels over the picture, drawn with Dear ImGui: for runtimes whose menu is drawn with it
// (Longshot: vk_menu.cpp's menu_record, ui::begin / ui::record). The panels are the module's (its gfx.h gfx_screen_t), handed
// over by the host (host/sloperina.c's hos_sloperina_screen, found by name); they are drawn in gz's colours into ImGui's
// foreground list each frame, under the runtime's own menu.
//
// Included by vk_menu.cpp before menu_tick (patch_tools.py), which also makes menu_record draw a frame while panels show.
#pragma once

#include "vk_sloperina_common.h"

SgScreen sg_screen;
std::mutex sg_mu;

// (menu_record, each present) the latest panels from the host; true when one of them shows
bool sloperina_imgui_any() {
    const SgScreenFn fn = sg_screen_fn();
    if (!fn) return false;
    static SgScreen next;
    static uint32_t serial = ~0u;
    static bool any = false;
    uint32_t now = 0;
    const int n = fn(&next, sizeof next, &now);
    if (now == serial) return any;
    serial = now;
    std::lock_guard lk(sg_mu);
    if (n != (int)sizeof next) {
        static bool told;
        if (n && !told) told = true, std::fprintf(stderr, "[vk] sloperina: panels of %d bytes, %d expected: not drawn\n", n, (int)sizeof next);
        any = false;
        return false;
    }
    sg_screen = next;
    any = false;
    for (const SgPanel& p : sg_screen.panel) any |= p.rows > 0 && p.cols > 0;
    return any;
}

ImU32 sg_col(const uint8_t c[3], int alpha = 255) { return IM_COL32(c[0], c[1], c[2], alpha); }

// (menu_record, inside the ImGui frame) every panel, w x h the window's image
void sloperina_imgui_draw(uint32_t w, uint32_t h) {
    std::lock_guard lk(sg_mu);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ui::fonts().body ? ui::fonts().body : ImGui::GetFont();
    const int scale = std::max(1, (int)(h + 360) / 720);
    for (int i = 0; i < kSgPanels; i++) {
        const SgPanel& p = sg_screen.panel[i];
        if (p.rows <= 0 || p.cols <= 0) continue;
        const int pct = p.scale_pct ? p.scale_pct : 100;
        const float ch = std::max(8.0f, 16.0f * h / 720.0f * pct / 100.0f), cw = ch * 9 / 16, pad = ch * 3 / 8;
        const int bw = (int)(std::min(p.cols, kSgCols) * cw + 2 * pad);
        const int bh = (int)(std::min(p.rows, kSgRows) * ch + 2 * pad + std::abs(p.yoff) * scale);
        int x0 = 0, y0 = 0;
        sg_place(p, i, (int)w, (int)h, bw, bh, scale, x0, y0);
        if (!p.clear) dl->AddRectFilled(ImVec2((float)x0, (float)y0), ImVec2((float)(x0 + bw), (float)(y0 + bh)), sg_col(sg_bg_rgb, 0xE0));
        for (int r = 0; r < p.rows && r < kSgRows; r++) {
            const int alpha = i == SG_PANEL_LOG || p.clear ? (p.alpha[r] ? p.alpha[r] : 255) : 255;
            const float y = y0 + pad + r * ch + p.yoff * scale;
            for (int col = 0; col < p.cols && col < kSgCols;) {
                const unsigned char c = (unsigned char)p.ch[r][col];
                const int st = p.style[r][col] < 7 ? p.style[r][col] : 0;
                const float x = x0 + pad + col * cw;
                ImU32 text = sg_col(sg_style_rgb[st], alpha);
                if (!c || c == ' ') { col++; continue; }
                if (c == 0x01 || c == 0x02) {   // (two cells: gz's checkbox, with its green tick)
                    const float s = std::min(2 * cw, ch) - 4, bx = x + (2 * cw - s) / 2, by = y + (ch - s) / 2, t = std::max(1.0f, s / 8);
                    dl->AddRect(ImVec2(bx, by), ImVec2(bx + s, by + s), text, 0, 0, t);
                    if (c == 0x02) dl->AddRectFilled(ImVec2(bx + 3 * t, by + 3 * t), ImVec2(bx + s - 3 * t, by + s - 3 * t), IM_COL32(0, 255, 0, alpha));
                    col += 2;
                    continue;
                }
                const char* stand_in = nullptr;
                switch (c) {
                case 0x03: case 0x7F: col++; continue;            // (an icon's second cell; invisible sizing)
                case 0x0E: col += 2; continue;                    // (the game's picture of an item: not drawn here)
                case 0x04: stand_in = "x", text = IM_COL32(255, 0, 0, alpha); break;
                case 0x05: stand_in = ">", text = IM_COL32(247, 209, 57, alpha); break;
                case 0x06: stand_in = "*"; break;
                case 0x07: stand_in = "^"; break;
                case 0x08: stand_in = "v"; break;
                case 0x09: stand_in = "+", text = IM_COL32(0, 255, 0, alpha); break;
                case 0x0B: stand_in = ">"; break;
                case 0x0C: stand_in = "<"; break;
                case 0x80: stand_in = "#"; break;
                default: break;
                }
                if (stand_in) {
                    dl->AddText(font, ch, ImVec2(x, y), text, stand_in);
                    col += c == 0x05 ? 2 : 1;
                    continue;
                }
                if (c >= 0x20 && c < 0x7F) {   // (a cell each: the grid stays a grid whatever the font's advance)
                    const char one[2] = {(char)c, 0};
                    const float adv = font->CalcTextSizeA(ch, FLT_MAX, 0, one).x;
                    dl->AddText(font, ch, ImVec2(x + (cw - adv) / 2, y), text, one);
                }
                col++;
            }
        }
    }
}
