/*
 * sensor_manager.c
 */
#include "sensor_manager.h"
#include "app_config.h"
#include "scd41.h"
#include "sgp40.h"
#include "mq136.h"
#include "mq137.h"
#include "i2c_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "SENSOR";

#define SENSOR_TASK_STACK 4096
#define SENSOR_TASK_PRIO  5

/* Ước lượng VOC tương đối từ SRAW (thay thế tạm cho VOC Index của Sensirion) */
#define VOC_BASELINE_LEARN_S   60        /* giây đầu: học baseline */
#define VOC_BASELINE_TAU_S     (6 * 3600)
#define VOC_FULL_SCALE_DROP    3000.0f   /* SRAW giảm bao nhiêu thì coi là 100% */

/* SCD40 cho mẫu mới mỗi 5 s; giữ giá trị cũ tối đa ngần này trước khi coi là mất dữ liệu */
#define SCD_HOLD_MS            30000
#define SCD_RESTART_MS         15000

static SemaphoreHandle_t s_lock;
static food_sensor_data_t s_data;
static uint32_t s_present;
static int64_t  s_start_us;
static volatile bool s_cal_request;
static volatile bool s_calibrating;
static volatile uint8_t s_cal_percent;
static sensor_update_cb_t s_cb;
static void *s_cb_arg;

/* trạng thái ước lượng VOC */
static float s_voc_baseline;
static uint32_t s_scd_last_ms;
static uint32_t s_voc_samples;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

esp_err_t sensor_manager_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    memset(&s_data, 0, sizeof(s_data));
    s_start_us = esp_timer_get_time();

    i2c_master_bus_handle_t bus = i2c_bus_get();   /* bus dùng chung với OLED */
    if (!bus) {
        ESP_LOGE(TAG, "Không có bus I2C -> bỏ qua SCD40, SGP40");
    } else {
        if (scd41_init(bus, SCD41_I2C_ADDRESS) == ESP_OK) {
            if (scd41_start_periodic() == ESP_OK) s_present |= SENSOR_PRESENT_SCD41;
        }
        if (sgp40_init(bus, SGP40_I2C_ADDRESS) == ESP_OK) s_present |= SENSOR_PRESENT_SGP40;
    }

    if (mq136_init() == ESP_OK) s_present |= SENSOR_PRESENT_MQ136;
    if (mq137_init() == ESP_OK) s_present |= SENSOR_PRESENT_MQ137;

    ESP_LOGI(TAG, "Cảm biến: SCD40(CO2+T+RH)=%d SGP40=%d MQ136=%d MQ137=%d",
             !!(s_present & SENSOR_PRESENT_SCD41),
             !!(s_present & SENSOR_PRESENT_SGP40), !!(s_present & SENSOR_PRESENT_MQ136),
             !!(s_present & SENSOR_PRESENT_MQ137));
    return ESP_OK;
}

uint32_t sensor_manager_present_mask(void)
{
    return s_present;
}

uint32_t sensor_manager_warmup_remaining_s(void)
{
    int64_t elapsed_s = (esp_timer_get_time() - s_start_us) / 1000000;
    if (elapsed_s >= MQ_WARMUP_TIME_S) return 0;
    return (uint32_t)(MQ_WARMUP_TIME_S - elapsed_s);
}

uint8_t sensor_manager_warmup_percent(void)
{
    if (MQ_WARMUP_TIME_S <= 0) return 100;
    uint32_t remain = sensor_manager_warmup_remaining_s();
    return (uint8_t)(100 - (remain * 100) / MQ_WARMUP_TIME_S);
}

esp_err_t sensor_manager_request_calibration(void)
{
    if (!(s_present & (SENSOR_PRESENT_MQ136 | SENSOR_PRESENT_MQ137))) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_calibrating) return ESP_ERR_INVALID_STATE;
    s_cal_percent = 0;
    s_cal_request = true;
    return ESP_OK;
}

bool sensor_manager_is_calibrating(void)
{
    return s_cal_request || s_calibrating;
}

uint8_t sensor_manager_calibration_percent(void)
{
    return s_cal_percent;
}

void sensor_manager_get_data(food_sensor_data_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_data;
    xSemaphoreGive(s_lock);
}

static uint8_t voc_estimate(uint16_t sraw)
{
    float raw = (float)sraw;
    s_voc_samples++;
    if (s_voc_samples <= VOC_BASELINE_LEARN_S) {
        /* trung bình cộng trong giai đoạn học */
        s_voc_baseline += (raw - s_voc_baseline) / (float)s_voc_samples;
        return 0;
    }
    if (raw > s_voc_baseline) {
        s_voc_baseline = raw;   /* không khí sạch hơn -> nâng baseline ngay */
    } else {
        float dt = SENSOR_READ_PERIOD_MS / 1000.0f;
        s_voc_baseline += (raw - s_voc_baseline) * dt / VOC_BASELINE_TAU_S;
    }
    float rel = (s_voc_baseline - raw) / VOC_FULL_SCALE_DROP * 100.0f;
    if (rel < 0) rel = 0;
    if (rel > 100) rel = 100;
    return (uint8_t)rel;
}

static void do_calibration(void)
{
    s_calibrating = true;
    s_cal_request = false;
    s_cal_percent = 5;
    float r0;
    if (s_present & SENSOR_PRESENT_MQ136) {
        if (mq136_calibrate(&r0) == ESP_OK) ESP_LOGI(TAG, "MQ136 R0 = %.2f", r0);
    }
    s_cal_percent = 50;
    if (s_present & SENSOR_PRESENT_MQ137) {
        if (mq137_calibrate(&r0) == ESP_OK) ESP_LOGI(TAG, "MQ137 R0 = %.2f", r0);
    }
    s_cal_percent = 100;
    vTaskDelay(pdMS_TO_TICKS(500));   /* để UI kịp hiện 100% */
    s_calibrating = false;
}

static void sensor_task(void *arg)
{
    (void)arg;
    food_sensor_data_t d = {0};
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        if (s_cal_request) {
            do_calibration();
        }

        food_sensor_data_t prev = d;
        d.valid = 0;

        /* --- SCD40: CO2 + nhiệt độ + độ ẩm (mẫu mới mỗi 5 s) ---
         * Giữa hai mẫu dùng lại giá trị trước, tối đa SCD_HOLD_MS. */
        if (s_present & SENSOR_PRESENT_SCD41) {
            bool ready = false;
            scd41_measurement_t m;
            if (scd41_data_ready(&ready) == ESP_OK && ready && scd41_read_measurement(&m) == ESP_OK) {
                d.co2_ppm = m.co2_ppm;
                d.temperature_c = m.temperature_c;
                d.humidity_rh = m.humidity_rh;
                d.valid |= SENSOR_VALID_CO2 | SENSOR_VALID_TEMP | SENSOR_VALID_HUM;
                s_scd_last_ms = now_ms();
            } else if ((prev.valid & SENSOR_VALID_CO2) && now_ms() - s_scd_last_ms < SCD_HOLD_MS) {
                d.co2_ppm = prev.co2_ppm;
                d.temperature_c = prev.temperature_c;
                d.humidity_rh = prev.humidity_rh;
                d.valid |= prev.valid & (SENSOR_VALID_CO2 | SENSOR_VALID_TEMP | SENSOR_VALID_HUM);
            }
            /* Quá 15 s không có mẫu: SCD40 có thể đã reset do sụt nguồn (dừng đo) hoặc bus treo
             * -> gỡ bus và ra lệnh đo lại (nếu nó vẫn đang đo thì lệnh bị từ chối, không sao). */
            static uint32_t s_scd_restart_ms;
            uint32_t since = now_ms() - (s_scd_last_ms > s_scd_restart_ms ? s_scd_last_ms : s_scd_restart_ms);
            if (since > SCD_RESTART_MS) {
                s_scd_restart_ms = now_ms();
                ESP_LOGW(TAG, "SCD40 không có mẫu mới %lu s -> khởi động đo lại", (unsigned long)(since / 1000));
                i2c_bus_recover();
                scd41_start_periodic();
            }
        }
        /* Nhiệt/ẩm để bù cho SGP40 (NAN -> SGP40 dùng mặc định 25 °C / 50 %) */
        float t = (d.valid & SENSOR_VALID_TEMP) ? d.temperature_c : NAN;
        float rh = (d.valid & SENSOR_VALID_HUM) ? d.humidity_rh : NAN;

        /* --- SGP40: VOC (bù nhiệt/ẩm) --- */
        if (s_present & SENSOR_PRESENT_SGP40) {
            uint16_t sraw;
            if (sgp40_measure_raw(t, rh, &sraw) == ESP_OK) {
                d.voc_raw = sraw;
                d.voc_rel = voc_estimate(sraw);
                d.valid |= SENSOR_VALID_VOC;
            }
        }

        /* --- MQ136 / MQ137 --- */
        d.mq_warmed_up = sensor_manager_warmup_remaining_s() == 0;
        if (s_present & SENSOR_PRESENT_MQ136) {
            float ratio, ppm;
            if (mq136_read(&ratio, &ppm) == ESP_OK) {
                d.h2s_rs_r0 = ratio;
                d.h2s_ppm = ppm;
                d.valid |= SENSOR_VALID_H2S;
            }
        }
        if (s_present & SENSOR_PRESENT_MQ137) {
            float ratio, ppm;
            if (mq137_read(&ratio, &ppm) == ESP_OK) {
                d.nh3_rs_r0 = ratio;
                d.nh3_ppm = ppm;
                d.valid |= SENSOR_VALID_NH3;
            }
        }

        d.seq++;
        d.timestamp_ms = now_ms();

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_data = d;
        xSemaphoreGive(s_lock);

        ESP_LOGD(TAG, "T=%.1f RH=%.1f CO2=%u VOC=%u(%u%%) NH3 Rs/R0=%.3f H2S Rs/R0=%.3f valid=0x%02lx",
                 d.temperature_c, d.humidity_rh, d.co2_ppm, d.voc_raw, d.voc_rel,
                 d.nh3_rs_r0, d.h2s_rs_r0, (unsigned long)d.valid);

        if (s_cb) {
            s_cb(&d, s_cb_arg);
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SENSOR_READ_PERIOD_MS));
    }
}

esp_err_t sensor_manager_start(sensor_update_cb_t cb, void *arg)
{
    s_cb = cb;
    s_cb_arg = arg;
    BaseType_t ok = xTaskCreate(sensor_task, "sensor", SENSOR_TASK_STACK, NULL, SENSOR_TASK_PRIO, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}