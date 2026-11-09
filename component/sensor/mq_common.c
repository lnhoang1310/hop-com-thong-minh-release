/*
 * mq_common.c - ADC + tính toán cho cảm biến MQ
 */
#include "mq_common.h"
#include "app_config.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>

static const char *TAG = "MQ";

#define MQ_NVS_NAMESPACE   "mq_cal"
#define MQ_ADC_ATTEN       ADC_ATTEN_DB_12
#define MQ_ADC_BITWIDTH    ADC_BITWIDTH_DEFAULT
#define MQ_DEFAULT_SAMPLES 16
#define MQ_ADC_VREF_MV     3100.0f   /* dùng khi không có hiệu chuẩn eFuse */

/* ADC1 dùng chung cho mọi cảm biến MQ */
static adc_oneshot_unit_handle_t s_adc1;
static int s_adc1_users;
static SemaphoreHandle_t s_adc_lock;

static esp_err_t adc1_acquire(void)
{
    if (!s_adc_lock) {
        s_adc_lock = xSemaphoreCreateMutex();
        if (!s_adc_lock) return ESP_ERR_NO_MEM;
    }
    if (!s_adc1) {
        adc_oneshot_unit_init_cfg_t cfg = {
            .unit_id = ADC_UNIT_1,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        esp_err_t err = adc_oneshot_new_unit(&cfg, &s_adc1);
        if (err != ESP_OK) {
            return err;
        }
    }
    s_adc1_users++;
    return ESP_OK;
}

static void adc1_release(void)
{
    if (s_adc1_users > 0 && --s_adc1_users == 0 && s_adc1) {
        adc_oneshot_del_unit(s_adc1);
        s_adc1 = NULL;
    }
}

static void nvs_load_r0(mq_sensor_t *s)
{
    nvs_handle_t h;
    if (nvs_open(MQ_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    float r0 = 0;
    size_t len = sizeof(r0);
    if (nvs_get_blob(h, s->nvs_key, &r0, &len) == ESP_OK && len == sizeof(r0) && r0 > 0.01f) {
        s->r0_kohm = r0;
        ESP_LOGI(TAG, "%s: R0 từ NVS = %.2f kOhm", s->name, r0);
    }
    nvs_close(h);
}

static esp_err_t nvs_save_r0(const mq_sensor_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MQ_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, s->nvs_key, &s->r0_kohm, sizeof(s->r0_kohm));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t mq_sensor_init(mq_sensor_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    s->ready = false;
    s->cali_ok = false;
    s->cali = NULL;

    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(s->gpio, &unit, &s->channel);
    if (err != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "%s: GPIO %d không phải chân ADC1", s->name, s->gpio);
        return ESP_ERR_INVALID_ARG;
    }
    err = adc1_acquire();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Không khởi tạo được ADC1: %s", esp_err_to_name(err));
        return err;
    }
    adc_oneshot_chan_cfg_t ch_cfg = {
        .atten = MQ_ADC_ATTEN,
        .bitwidth = MQ_ADC_BITWIDTH,
    };
    err = adc_oneshot_config_channel(s_adc1, s->channel, &ch_cfg);
    if (err != ESP_OK) {
        adc1_release();
        return err;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = s->channel,
        .atten = MQ_ADC_ATTEN,
        .bitwidth = MQ_ADC_BITWIDTH,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s->cali) == ESP_OK) {
        s->cali_ok = true;
    }
#endif
    if (!s->cali_ok) {
        ESP_LOGW(TAG, "%s: không có hiệu chuẩn ADC, dùng công thức tuyến tính", s->name);
    }

    nvs_load_r0(s);
    s->ready = true;
    ESP_LOGI(TAG, "%s sẵn sàng (GPIO %d, kênh %d, R0=%.2f kOhm)", s->name, s->gpio, s->channel, s->r0_kohm);
    return ESP_OK;
}

esp_err_t mq_sensor_deinit(mq_sensor_t *s)
{
    if (!s || !s->ready) return ESP_OK;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if (s->cali_ok) {
        adc_cali_delete_scheme_curve_fitting(s->cali);
    }
#endif
    s->cali_ok = false;
    s->ready = false;
    adc1_release();
    return ESP_OK;
}

esp_err_t mq_sensor_read_voltage(mq_sensor_t *s, int samples, float *v_out)
{
    if (!s || !s->ready) return ESP_ERR_INVALID_STATE;
    if (samples <= 0) samples = MQ_DEFAULT_SAMPLES;

    int64_t sum_mv = 0;
    int ok = 0;
    xSemaphoreTake(s_adc_lock, portMAX_DELAY);
    for (int i = 0; i < samples; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc1, s->channel, &raw) != ESP_OK) {
            continue;
        }
        int mv = 0;
        if (s->cali_ok) {
            adc_cali_raw_to_voltage(s->cali, raw, &mv);
        } else {
            mv = (int)((float)raw * MQ_ADC_VREF_MV / 4095.0f);
        }
        sum_mv += mv;
        ok++;
    }
    xSemaphoreGive(s_adc_lock);

    if (ok == 0) return ESP_FAIL;
    float v_adc = (float)sum_mv / ok / 1000.0f;
    if (v_out) *v_out = v_adc * MQ_DIVIDER_RATIO;
    return ESP_OK;
}

esp_err_t mq_sensor_read_rs(mq_sensor_t *s, int samples, float *rs_kohm)
{
    float v;
    esp_err_t err = mq_sensor_read_voltage(s, samples, &v);
    if (err != ESP_OK) return err;
    if (v < 0.01f) v = 0.01f;                          /* tránh chia 0 */
    if (v > MQ_SUPPLY_VOLTAGE - 0.01f) v = MQ_SUPPLY_VOLTAGE - 0.01f;
    if (rs_kohm) *rs_kohm = s->rl_kohm * (MQ_SUPPLY_VOLTAGE - v) / v;
    return ESP_OK;
}

esp_err_t mq_sensor_read(mq_sensor_t *s, float *rs_r0, float *ppm)
{
    float rs;
    esp_err_t err = mq_sensor_read_rs(s, MQ_DEFAULT_SAMPLES, &rs);
    if (err != ESP_OK) return err;
    float ratio = rs / s->r0_kohm;
    if (rs_r0) *rs_r0 = ratio;
    if (ppm) {
        float value = s->curve_a * powf(ratio, s->curve_b);
        if (!isfinite(value) || value < 0) value = 0;
        if (value > 10000.0f) value = 10000.0f;
        *ppm = value;
    }
    return ESP_OK;
}

esp_err_t mq_sensor_read_ppm(mq_sensor_t *s, float *ppm)
{
    return mq_sensor_read(s, NULL, ppm);
}

esp_err_t mq_sensor_calibrate(mq_sensor_t *s, int samples, float *r0_kohm)
{
    if (!s || !s->ready) return ESP_ERR_INVALID_STATE;
    if (samples <= 0) samples = 50;
    float sum = 0;
    int ok = 0;
    for (int i = 0; i < samples; i++) {
        float rs;
        if (mq_sensor_read_rs(s, MQ_DEFAULT_SAMPLES, &rs) == ESP_OK) {
            sum += rs;
            ok++;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (ok == 0) return ESP_FAIL;
    s->r0_kohm = (sum / ok) / s->clean_air_ratio;
    ESP_LOGI(TAG, "%s: hiệu chuẩn R0 = %.2f kOhm", s->name, s->r0_kohm);
    if (r0_kohm) *r0_kohm = s->r0_kohm;
    return nvs_save_r0(s);
}

esp_err_t mq_sensor_reset_calibration(mq_sensor_t *s, float default_r0)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    s->r0_kohm = default_r0;
    nvs_handle_t h;
    if (nvs_open(MQ_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, s->nvs_key);
        nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
}