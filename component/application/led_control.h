#pragma once
/*
 * led_control.h - Điều khiển 3 đèn (xanh/vàng/đỏ) và còi cảnh báo
 *
 *   SAFE    : đèn xanh sáng, còi tắt
 *   WARNING : đèn vàng sáng, còi bíp ngắn mỗi 1 giây
 *   DANGER  : đèn đỏ sáng, còi kêu liên tục
 *   UNKNOWN : tắt hết (hoặc hiệu ứng chờ khi đang khởi động)
 */
#include <stdbool.h>
#include "esp_err.h"
#include "food_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kiểu còi khi DANGER: kêu liên tục (mặc định) */
#define BUZZER_WARN_ON_MS     150     /* WARNING: bíp 150ms ... */
#define BUZZER_WARN_PERIOD_MS 1000    /* ... mỗi 1000ms */

esp_err_t led_control_init(void);

void led_control_set_level(food_level_t level);
food_level_t led_control_get_level(void);

/* Tắt tiếng còi cho mức hiện tại; tự bật lại khi mức thay đổi */
void led_control_set_mute(bool mute);
bool led_control_is_muted(void);

/* Hiệu ứng chờ khi khởi động (3 đèn chạy lần lượt) */
void led_control_set_busy(bool busy);

/* Bíp ngắn xác nhận thao tác nút (không bị ảnh hưởng bởi mute) */
void led_control_beep(uint32_t ms);

#ifdef __cplusplus
}
#endif