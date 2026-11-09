/*
 * led_control.c - Task 10ms điều khiển đèn + còi theo mức an toàn
 */
#include "led_control.h"
#include "app_config.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#if BUZZER_IS_PASSIVE
#include "driver/ledc.h"
#endif

static const char *TAG = "LED";

#define LED_TASK_PERIOD_MS 10

static volatile food_level_t s_level = FOOD_LEVEL_UNKNOWN;
static volatile bool s_muted;
static volatile bool s_busy;
static volatile uint32_t s_beep_ms;
static bool s_inited;

static void led_write(int gpio, bool on)
{
    gpio_set_level(gpio, on ? LED_ACTIVE_LEVEL : !LED_ACTIVE_LEVEL);
}

static void buzzer_write(bool on)
{
    static int last = -1;
    if (last == (int)on) return;
    last = on;
#if BUZZER_IS_PASSIVE
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? 512 : 0);   /* 50% của 10 bit */
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#else
    gpio_set_level(BUZZER, on ? BUZZER_ACTIVE_LEVEL : !BUZZER_ACTIVE_LEVEL);
#endif
}

static void led_task(void *arg)
{
    (void)arg;
    uint32_t t = 0;           /* ms */
    food_level_t prev = FOOD_LEVEL_UNKNOWN;
    uint32_t level_start = 0;

    while (1) {
        food_level_t lv = s_level;
        if (lv != prev) {
            prev = lv;
            level_start = t;  /* bắt đầu chu kỳ bíp ngay khi đổi mức */
        }
        uint32_t phase = t - level_start;
        bool r = false, y = false, g = false, buzz = false;

        if (s_busy) {
            int step = (t / 250) % 3;    /* đèn chạy: xanh -> vàng -> đỏ */
            g = step == 0;
            y = step == 1;
            r = step == 2;
        } else {
            switch (lv) {
            case FOOD_LEVEL_SAFE:
                g = true;
                break;
            case FOOD_LEVEL_WARNING:
                y = true;
                buzz = (phase % BUZZER_WARN_PERIOD_MS) < BUZZER_WARN_ON_MS;
                break;
            case FOOD_LEVEL_DANGER:
                r = true;
                buzz = true;
                break;
            default:
                break;
            }
            if (s_muted) buzz = false;
        }

        if (s_beep_ms > 0) {
            buzz = true;
            s_beep_ms = s_beep_ms > LED_TASK_PERIOD_MS ? s_beep_ms - LED_TASK_PERIOD_MS : 0;
        }

        led_write(LED_RED, r);
        led_write(LED_YELLOW, y);
        led_write(LED_GREEN, g);
        buzzer_write(buzz);

        vTaskDelay(pdMS_TO_TICKS(LED_TASK_PERIOD_MS));
        t += LED_TASK_PERIOD_MS;
    }
}

esp_err_t led_control_init(void)
{
    if (s_inited) return ESP_OK;
    uint64_t mask = (1ULL << LED_RED) | (1ULL << LED_GREEN) | (1ULL << LED_YELLOW);
#if !BUZZER_IS_PASSIVE
    mask |= (1ULL << BUZZER);
#endif
    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config lỗi: %s", esp_err_to_name(err));
        return err;
    }

#if BUZZER_IS_PASSIVE
    ledc_timer_config_t tcfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = BUZZER_PASSIVE_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&tcfg));
    ledc_channel_config_t ccfg = {
        .gpio_num = BUZZER,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ccfg));
#endif

    led_write(LED_RED, false);
    led_write(LED_YELLOW, false);
    led_write(LED_GREEN, false);
    buzzer_write(false);

    if (xTaskCreate(led_task, "led", 2560, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    return ESP_OK;
}

void led_control_set_level(food_level_t level)
{
    if (level != s_level) {
        if (level > s_level) {
            s_muted = false;  /* mức nặng hơn -> bật lại còi */
        }
        s_level = level;
    }
}

food_level_t led_control_get_level(void)
{
    return s_level;
}

void led_control_set_mute(bool mute)
{
    s_muted = mute;
}

bool led_control_is_muted(void)
{
    return s_muted;
}

void led_control_set_busy(bool busy)
{
    s_busy = busy;
}

void led_control_beep(uint32_t ms)
{
    s_beep_ms = ms;
}