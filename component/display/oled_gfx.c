/*
 * oled_gfx.c - Vẽ lên framebuffer 128x64
 */
#include "oled_gfx.h"
#include "font5x7.h"
#include <string.h>

static uint8_t s_fb[OLED_FB_SIZE];

uint8_t *oled_gfx_buffer(void)
{
    return s_fb;
}

void oled_gfx_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void oled_gfx_fill(oled_color_t color)
{
    if (color == OLED_COLOR_INVERT) {
        for (int i = 0; i < OLED_FB_SIZE; i++) s_fb[i] ^= 0xFF;
    } else {
        memset(s_fb, color ? 0xFF : 0x00, sizeof(s_fb));
    }
}

void oled_gfx_pixel(int x, int y, oled_color_t color)
{
    if (x < 0 || y < 0 || x >= OLED_WIDTH || y >= OLED_HEIGHT) return;
    uint8_t *p = &s_fb[(y >> 3) * OLED_WIDTH + x];
    uint8_t m = (uint8_t)(1u << (y & 7));
    switch (color) {
    case OLED_COLOR_WHITE:  *p |= m;  break;
    case OLED_COLOR_BLACK:  *p &= (uint8_t)~m; break;
    case OLED_COLOR_INVERT: *p ^= m;  break;
    }
}

void oled_gfx_hline(int x, int y, int w, oled_color_t color)
{
    for (int i = 0; i < w; i++) oled_gfx_pixel(x + i, y, color);
}

void oled_gfx_vline(int x, int y, int h, oled_color_t color)
{
    for (int i = 0; i < h; i++) oled_gfx_pixel(x, y + i, color);
}

void oled_gfx_rect(int x, int y, int w, int h, oled_color_t color)
{
    if (w <= 0 || h <= 0) return;
    oled_gfx_hline(x, y, w, color);
    if (h > 1) oled_gfx_hline(x, y + h - 1, w, color);
    if (h > 2) {
        oled_gfx_vline(x, y + 1, h - 2, color);
        if (w > 1) oled_gfx_vline(x + w - 1, y + 1, h - 2, color);
    }
}

void oled_gfx_fill_rect(int x, int y, int w, int h, oled_color_t color)
{
    for (int j = 0; j < h; j++) oled_gfx_hline(x, y + j, w, color);
}

void oled_gfx_round_rect(int x, int y, int w, int h, oled_color_t color)
{
    if (w < 3 || h < 3) {
        oled_gfx_rect(x, y, w, h, color);
        return;
    }
    oled_gfx_hline(x + 1, y, w - 2, color);
    oled_gfx_hline(x + 1, y + h - 1, w - 2, color);
    oled_gfx_vline(x, y + 1, h - 2, color);
    oled_gfx_vline(x + w - 1, y + 1, h - 2, color);
}

int oled_gfx_char(int x, int y, char c, int scale, oled_color_t color)
{
    unsigned char uc = (unsigned char)c;
    if (uc < FONT5X7_FIRST || uc > FONT5X7_LAST) uc = '?';
    if (scale < 1) scale = 1;
    const uint8_t *g = font5x7[uc - FONT5X7_FIRST];
    for (int col = 0; col < FONT5X7_WIDTH; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < FONT5X7_HEIGHT; row++) {
            if (bits & (1u << row)) {
                if (scale == 1) {
                    oled_gfx_pixel(x + col, y + row, color);
                } else {
                    oled_gfx_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
                }
            }
        }
    }
    return (FONT5X7_WIDTH + 1) * scale;
}

int oled_gfx_text_width(const char *s, int scale)
{
    if (!s || !*s) return 0;
    if (scale < 1) scale = 1;
    return (int)strlen(s) * (FONT5X7_WIDTH + 1) * scale - scale;   /* bỏ khoảng trắng cuối */
}

int oled_gfx_text(int x, int y, const char *s, int scale, oled_color_t color)
{
    int x0 = x;
    if (!s) return 0;
    while (*s) {
        x += oled_gfx_char(x, y, *s++, scale, color);
    }
    return x - x0;
}

int oled_gfx_text_aligned(int x, int y, int w, const char *s, int scale, oled_align_t align, oled_color_t color)
{
    int tw = oled_gfx_text_width(s, scale);
    int ox = x;
    if (align == OLED_ALIGN_CENTER) ox = x + (w - tw) / 2;
    else if (align == OLED_ALIGN_RIGHT) ox = x + w - tw;
    return oled_gfx_text(ox, y, s, scale, color);
}

void oled_gfx_progress(int x, int y, int w, int h, int percent)
{
    oled_gfx_progress_marks(x, y, w, h, percent, -1, -1);
}

void oled_gfx_progress_marks(int x, int y, int w, int h, int percent, int mark1, int mark2)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    oled_gfx_round_rect(x, y, w, h, OLED_COLOR_WHITE);
    int inner_w = w - 4;
    int inner_h = h - 4;
    if (inner_w <= 0 || inner_h <= 0) return;
    int fill = (inner_w * percent + 50) / 100;
    oled_gfx_fill_rect(x + 2, y + 2, fill, inner_h, OLED_COLOR_WHITE);

    /* vạch ngưỡng: chấm ở mép trên/dưới, đảo màu bên trong để luôn thấy */
    int marks[2] = { mark1, mark2 };
    for (int i = 0; i < 2; i++) {
        if (marks[i] <= 0 || marks[i] >= 100) continue;
        int mx = x + 2 + (inner_w * marks[i] + 50) / 100;
        oled_gfx_pixel(mx, y - 1, OLED_COLOR_WHITE);
        oled_gfx_pixel(mx, y + h, OLED_COLOR_WHITE);
        oled_gfx_vline(mx, y + 2, inner_h, OLED_COLOR_INVERT);
    }
}

void oled_gfx_progress_indeterminate(int x, int y, int w, int h, uint32_t frame)
{
    oled_gfx_round_rect(x, y, w, h, OLED_COLOR_WHITE);
    int inner_w = w - 4;
    int seg = inner_w / 4;
    if (seg < 2) return;
    int span = inner_w - seg;
    int pos = (int)(frame * 3 % (uint32_t)(span * 2));
    if (pos > span) pos = span * 2 - pos;   /* chạy qua rồi chạy lại */
    oled_gfx_fill_rect(x + 2 + pos, y + 2, seg, h - 4, OLED_COLOR_WHITE);
}

void oled_gfx_bitmap_col8(int x, int y, const uint8_t *cols, int w, oled_color_t color)
{
    for (int c = 0; c < w; c++) {
        for (int r = 0; r < 8; r++) {
            if (cols[c] & (1u << r)) oled_gfx_pixel(x + c, y + r, color);
        }
    }
}