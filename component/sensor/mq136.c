/*
 * mq136.c - MQ136 (H2S)
 */
#include "mq136.h"
#include "mq_common.h"
#include "app_config.h"

static mq_sensor_t s_mq = {
    .name = "MQ136",
    .nvs_key = "r0_mq136",
    .gpio = MQ136_ADC_GPIO,
    .rl_kohm = MQ136_RL_KOHM,
    .r0_kohm = MQ136_R0_DEFAULT_KOHM,
    .clean_air_ratio = MQ136_CLEAN_AIR_RATIO,
    .curve_a = MQ136_CURVE_A,
    .curve_b = MQ136_CURVE_B,
};

esp_err_t mq136_init(void)
{
    return mq_sensor_init(&s_mq);
}

esp_err_t mq136_deinit(void)
{
    return mq_sensor_deinit(&s_mq);
}

bool mq136_is_ready(void)
{
    return s_mq.ready;
}

esp_err_t mq136_read_ppm(float *ppm)
{
    return mq_sensor_read_ppm(&s_mq, ppm);
}

esp_err_t mq136_read(float *rs_r0, float *ppm)
{
    return mq_sensor_read(&s_mq, rs_r0, ppm);
}

esp_err_t mq136_read_raw(float *v_out, float *rs_kohm)
{
    esp_err_t err = ESP_OK;
    if (v_out) {
        err = mq_sensor_read_voltage(&s_mq, 16, v_out);
    }
    if (err == ESP_OK && rs_kohm) {
        err = mq_sensor_read_rs(&s_mq, 16, rs_kohm);
    }
    return err;
}

esp_err_t mq136_calibrate(float *r0_kohm)
{
    return mq_sensor_calibrate(&s_mq, 50, r0_kohm);
}

esp_err_t mq136_reset_calibration(void)
{
    return mq_sensor_reset_calibration(&s_mq, MQ136_R0_DEFAULT_KOHM);
}

float mq136_get_r0(void)
{
    return s_mq.r0_kohm;
}