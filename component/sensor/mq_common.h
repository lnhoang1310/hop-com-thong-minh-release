#pragma once
/*
 * mq_common.h - Phần dùng chung cho cảm biến khí dòng MQ (MQ136, MQ137)
 *
 *  Rs  = RL * (Vc - Vout) / Vout
 *  ppm = A * (Rs / R0) ^ B
 *  R0 được hiệu chuẩn trong không khí sạch: R0 = Rs_air / clean_air_ratio
 *  R0 lưu trong NVS (namespace "mq_cal").
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Cấu hình (điền trước khi init) */
    const char *name;
    const char *nvs_key;          /* khoá NVS lưu R0 (<= 15 ký tự) */
    int   gpio;
    float rl_kohm;
    float r0_kohm;                /* giá trị mặc định, bị ghi đè nếu NVS có */
    float clean_air_ratio;
    float curve_a;
    float curve_b;
    /* Trạng thái nội bộ */
    adc_channel_t channel;
    adc_cali_handle_t cali;
    bool  cali_ok;
    bool  ready;
} mq_sensor_t;

esp_err_t mq_sensor_init(mq_sensor_t *s);
esp_err_t mq_sensor_deinit(mq_sensor_t *s);

/* Điện áp tại chân OUT của module (đã bù cầu chia), trung bình `samples` mẫu */
esp_err_t mq_sensor_read_voltage(mq_sensor_t *s, int samples, float *v_out);
esp_err_t mq_sensor_read_rs(mq_sensor_t *s, int samples, float *rs_kohm);
esp_err_t mq_sensor_read_ppm(mq_sensor_t *s, float *ppm);
/* Một lần đọc: Rs/R0 (dùng cho đánh giá) và ppm gần đúng (chỉ để hiển thị) */
esp_err_t mq_sensor_read(mq_sensor_t *s, float *rs_r0, float *ppm);

/* Hiệu chuẩn R0 (đặt cảm biến trong không khí sạch, đã làm nóng). Lưu NVS. */
esp_err_t mq_sensor_calibrate(mq_sensor_t *s, int samples, float *r0_kohm);
esp_err_t mq_sensor_reset_calibration(mq_sensor_t *s, float default_r0);

#ifdef __cplusplus
}
#endif