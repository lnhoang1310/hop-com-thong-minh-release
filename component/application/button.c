/*
 * button.c - Quét 3 nút mỗi 10 ms (tick FreeRTOS), chống dội, nhấn ngắn / giữ lâu / tổ hợp
 * Thời gian tính theo đồng hồ esp_timer nên đúng với mọi CONFIG_FREERTOS_HZ.
 */
#include "button.h"
#include "app_config.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "BTN";

#define SCAN_MS 10

static const int s_pins[BTN_COUNT] = { BUTTON_UP, BUTTON_DOWN, BUTTON_OK };

typedef struct {
    bool     stable;        /* trạng thái đã chống dội (true = đang nhấn) */
    bool     raw_last;
    uint32_t raw_since;     /* thời điểm raw đổi lần cuối (ms) */
    uint32_t press_at;      /* thời điểm bắt đầu nhấn (ms) */
    bool     long_sent;
    bool     suppressed;    /* bị chặn vì đang dùng tổ hợp */
} btn_state_t;

static button_cb_t s_cb;
static void *s_arg;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void emit(button_id_t id, button_event_t evt)
{
    if (s_cb) s_cb(id, evt, s_arg);
}

static const char *const s_names[BTN_COUNT] = { "UP", "DOWN", "OK" };

static void button_task(void *arg)
{
    (void)arg;
    btn_state_t st[BTN_COUNT] = {0};
    bool combo_active = false, combo_sent = false;
    uint32_t combo_at = 0;

    /* Nút đang "được nhấn" ngay lúc khởi động: thường do nối sai (lên 3.3V thay vì GND)
     * hoặc kẹt. Bỏ qua nút đó cho tới khi nó được nhả, tránh tự kích hoạt giữ lâu / tổ hợp. */
    vTaskDelay(pdMS_TO_TICKS(50) ? pdMS_TO_TICKS(50) : 1);
    for (int i = 0; i < BTN_COUNT; i++) {
        bool raw = gpio_get_level(s_pins[i]) == BUTTON_ACTIVE_LEVEL;
        st[i].raw_last = st[i].stable = raw;
        st[i].raw_since = now_ms();
        if (raw) {
            st[i].long_sent = st[i].suppressed = true;
            ESP_LOGW(TAG, "Nút %s (GPIO %d) đang ở trạng thái NHẤN lúc khởi động -> kiểm tra dây: "
                     "nút phải nối giữa chân GPIO và GND", s_names[i], s_pins[i]);
        }
    }

    while (1) {
        uint32_t now = now_ms();
        for (int i = 0; i < BTN_COUNT; i++) {
            btn_state_t *b = &st[i];
            bool raw = gpio_get_level(s_pins[i]) == BUTTON_ACTIVE_LEVEL;
            if (raw != b->raw_last) {
                b->raw_last = raw;
                b->raw_since = now;
            }
            if (now - b->raw_since < BUTTON_DEBOUNCE_MS || raw == b->stable) continue;

            b->stable = raw;
            ESP_LOGD(TAG, "%s %s", s_names[i], raw ? "nhấn" : "nhả");
            if (raw) {                                   /* vừa nhấn */
                b->press_at = now;
                b->long_sent = false;
                b->suppressed = false;
            } else if (!b->long_sent && !b->suppressed) { /* vừa nhả */
                emit((button_id_t)i, BTN_EVT_SHORT);
            }
        }

        /* --- Tổ hợp UP + DOWN --- */
        bool both = st[BTN_UP].stable && st[BTN_DOWN].stable;
        if (both && !combo_active) {
            combo_active = true;
            combo_sent = false;
            combo_at = now;
        }
        if (combo_active) {
            st[BTN_UP].suppressed = st[BTN_DOWN].suppressed = true;
            if (both && !combo_sent && now - combo_at >= BUTTON_COMBO_MS) {
                combo_sent = true;
                emit(BTN_UP, BTN_EVT_COMBO);
            }
            if (!st[BTN_UP].stable && !st[BTN_DOWN].stable) {
                combo_active = false;                    /* nhả cả hai mới kết thúc tổ hợp */
            }
        }

        /* --- Giữ lâu từng nút --- */
        for (int i = 0; i < BTN_COUNT; i++) {
            btn_state_t *b = &st[i];
            if (b->stable && !b->long_sent && !b->suppressed && now - b->press_at >= BUTTON_LONG_MS) {
                b->long_sent = true;
                emit((button_id_t)i, BTN_EVT_LONG);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SCAN_MS) ? pdMS_TO_TICKS(SCAN_MS) : 1);
    }
}

esp_err_t button_init(button_cb_t cb, void *arg)
{
    s_cb = cb;
    s_arg = arg;
    uint64_t mask = 0;
    for (int i = 0; i < BTN_COUNT; i++) mask |= 1ULL << s_pins[i];
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = BUTTON_ACTIVE_LEVEL == 0 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = BUTTON_ACTIVE_LEVEL == 1 ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config lỗi: %s", esp_err_to_name(err));
        return err;
    }
    return xTaskCreate(button_task, "button", 2560, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}