#pragma once
/*
 * oled_gfx.h - Thư viện vẽ đơn sắc 128x64 trên framebuffer (không phụ thuộc phần cứng)
 * Bố cục framebuffer giống SH1106/SSD1306: buf[page * 128 + x], bit (y % 8).
 */
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_WIDTH   128
#define OLED_HEIGHT  64
#define OLED_PAGES   (OLED_HEIGHT / 8)
#define OLED_FB_SIZE (OLED_WIDTH * OLED_PAGES)

typedef enum {
    OLED_COLOR_BLACK = 0,
    OLED_COLOR_WHITE = 1,
    OLED_COLOR_INVERT = 2,
} oled_color_t;

typedef enum {
    OLED_ALIGN_LEFT = 0,
    OLED_ALIGN_CENTER,
    OLED_ALIGN_RIGHT,
} oled_align_t;

uint8_t *oled_gfx_buffer(void);
void oled_gfx_clear(void);
void oled_gfx_fill(oled_color_t color);

void oled_gfx_pixel(int x, int y, oled_color_t color);
void oled_gfx_hline(int x, int y, int w, oled_color_t color);
void oled_gfx_vline(int x, int y, int h, oled_color_t color);
void oled_gfx_rect(int x, int y, int w, int h, oled_color_t color);
void oled_gfx_fill_rect(int x, int y, int w, int h, oled_color_t color);
void oled_gfx_round_rect(int x, int y, int w, int h, oled_color_t color);

/* Văn bản: font 5x7, scale = 1 (6x8 px/ký tự), 2 (12x16)... Trả về độ rộng đã vẽ */
int  oled_gfx_char(int x, int y, char c, int scale, oled_color_t color);
int  oled_gfx_text(int x, int y, const char *s, int scale, oled_color_t color);
int  oled_gfx_text_aligned(int x, int y, int w, const char *s, int scale, oled_align_t align, oled_color_t color);
int  oled_gfx_text_width(const char *s, int scale);

/* Thanh tiến trình kiểu "loading": khung + phần lấp đầy theo phần trăm (0..100) */
void oled_gfx_progress(int x, int y, int w, int h, int percent);
/* Thanh tiến trình có vạch đánh dấu ngưỡng (phần trăm), mark < 0 thì bỏ qua */
void oled_gfx_progress_marks(int x, int y, int w, int h, int percent, int mark1, int mark2);
/* Thanh chạy qua lại (không xác định thời gian), frame tăng dần */
void oled_gfx_progress_indeterminate(int x, int y, int w, int h, uint32_t frame);

/* Bitmap 1bpp theo cột (giống font), height <= 8 */
void oled_gfx_bitmap_col8(int x, int y, const uint8_t *cols, int w, oled_color_t color);

#ifdef __cplusplus
}
#endif