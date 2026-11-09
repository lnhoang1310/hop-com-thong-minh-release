#pragma once
/*
 * mq136.h - Cảm biến khí H2S (MQ136), đọc analog qua ADC1
 * Cảm biến cần làm nóng (MQ_WARMUP_TIME_S) trước khi số liệu ổn định.
 */
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mq136_init(void);
esp_err_t mq136_deinit(void);
bool      mq136_is_ready(void);

esp_err_t mq136_read_ppm(float *ppm);
esp_err_t mq136_read(float *rs_r0, float *ppm);   /* Rs/R0 + ppm gần đúng */
esp_err_t mq136_read_raw(float *v_out, float *rs_kohm);  /* điện áp OUT & Rs để debug */

esp_err_t mq136_calibrate(float *r0_kohm);   /* gọi trong không khí sạch, lưu NVS */
esp_err_t mq136_reset_calibration(void);
float     mq136_get_r0(void);

#ifdef __cplusplus
}
#endif