#pragma once
/*
 * sensor_manager.h - Quản lý toàn bộ cảm biến
 *  - Khởi tạo SCD40 (CO2 + nhiệt độ + độ ẩm), SGP40 trên bus I2C dùng chung, MQ136, MQ137
 *  - Task đọc định kỳ (SENSOR_READ_PERIOD_MS), lưu bản sao dữ liệu mới nhất
 *  - Theo dõi thời gian làm nóng MQ, hiệu chuẩn R0
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "food_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bit trạng thái cảm biến có mặt (sensor_manager_present_mask) */
#define SENSOR_PRESENT_SCD41  (1u << 1)
#define SENSOR_PRESENT_SGP40  (1u << 2)
#define SENSOR_PRESENT_MQ136  (1u << 3)
#define SENSOR_PRESENT_MQ137  (1u << 4)

typedef void (*sensor_update_cb_t)(const food_sensor_data_t *data, void *arg);

esp_err_t sensor_manager_init(void);
esp_err_t sensor_manager_start(sensor_update_cb_t cb, void *arg);

void      sensor_manager_get_data(food_sensor_data_t *out);
uint32_t  sensor_manager_present_mask(void);

uint8_t   sensor_manager_warmup_percent(void);       /* 0..100 */
uint32_t  sensor_manager_warmup_remaining_s(void);

/* Hiệu chuẩn R0 cho MQ (chạy bất đồng bộ trong task cảm biến) */
esp_err_t sensor_manager_request_calibration(void);
bool      sensor_manager_is_calibrating(void);
uint8_t   sensor_manager_calibration_percent(void);

#ifdef __cplusplus
}
#endif