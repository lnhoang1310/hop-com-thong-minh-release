#pragma once
/*
 * sgp40.h - Driver cảm biến VOC Sensirion SGP40 (I2C)
 * Trả về giá trị thô SRAW_VOC (VOC càng cao -> SRAW càng THẤP).
 * VOC Index đầy đủ (thuật toán Sensirion Gas Index) để server tính.
 * Nên gọi sgp40_measure_raw() đều đặn mỗi 1 giây.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sgp40_init(i2c_master_bus_handle_t bus, uint8_t addr);
esp_err_t sgp40_deinit(void);
bool      sgp40_is_ready(void);

/* temperature_c / humidity_rh để bù; truyền NAN nếu không có -> dùng 25°C / 50% */
esp_err_t sgp40_measure_raw(float temperature_c, float humidity_rh, uint16_t *sraw);
esp_err_t sgp40_self_test(void);
esp_err_t sgp40_heater_off(void);
esp_err_t sgp40_get_serial(uint64_t *serial);

#ifdef __cplusplus
}
#endif