#pragma once
/*
 * oled_sh1106.h - Driver OLED 1.3" 128x64 (SH1106, tương thích SSD1306) qua I2C
 * Vẽ bằng oled_gfx_*, sau đó gọi oled_flush() để đẩy framebuffer ra màn.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "oled_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t oled_init(void);
bool      oled_is_ready(void);
esp_err_t oled_flush(void);
esp_err_t oled_set_contrast(uint8_t contrast);
esp_err_t oled_power(bool on);
esp_err_t oled_invert(bool invert);

#ifdef __cplusplus
}
#endif