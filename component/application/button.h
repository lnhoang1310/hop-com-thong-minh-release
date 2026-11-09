#pragma once
/*
 * button.h - Đọc 3 nút (UP, DOWN, OK) có chống dội
 *
 *  BTN_EVT_SHORT : nhấn rồi nhả trước BUTTON_LONG_MS
 *  BTN_EVT_LONG  : giữ đủ BUTTON_LONG_MS (phát 1 lần khi đang giữ, nhả ra không phát SHORT nữa)
 *  BTN_EVT_COMBO : giữ ĐỒNG THỜI UP + DOWN đủ BUTTON_COMBO_MS (id = BTN_UP).
 *                  Khi đang nhấn cả hai, sự kiện riêng của UP/DOWN bị chặn để không kích hoạt nhầm.
 */
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_UP = 0,
    BTN_DOWN,
    BTN_OK,
    BTN_COUNT,
} button_id_t;

typedef enum {
    BTN_EVT_SHORT = 0,
    BTN_EVT_LONG,
    BTN_EVT_COMBO,
} button_event_t;

#define BUTTON_DEBOUNCE_MS   30
#define BUTTON_LONG_MS       3000
#define BUTTON_COMBO_MS      5000

typedef void (*button_cb_t)(button_id_t id, button_event_t evt, void *arg);

esp_err_t button_init(button_cb_t cb, void *arg);

#ifdef __cplusplus
}
#endif