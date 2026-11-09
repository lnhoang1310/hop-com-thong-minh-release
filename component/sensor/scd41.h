#pragma once
/*
 * scd41.h - Driver cảm biến CO2 Sensirion SCD41 (I2C)
 * Chế độ đo định kỳ: mỗi 5 giây có 1 mẫu mới.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t co2_ppm;
    float    temperature_c;
    float    humidity_rh;
} scd41_measurement_t;

esp_err_t scd41_init(i2c_master_bus_handle_t bus, uint8_t addr);
esp_err_t scd41_deinit(void);
bool      scd41_is_ready(void);

esp_err_t scd41_start_periodic(void);
esp_err_t scd41_start_low_power_periodic(void);   /* 1 mẫu / 30 s */
esp_err_t scd41_stop_periodic(void);
esp_err_t scd41_data_ready(bool *ready);
esp_err_t scd41_read_measurement(scd41_measurement_t *out);

/* Các lệnh dưới đây chỉ dùng khi ĐÃ dừng đo định kỳ */
esp_err_t scd41_get_serial(uint64_t *serial);
esp_err_t scd41_set_temperature_offset(float offset_c);
esp_err_t scd41_set_altitude(uint16_t altitude_m);
esp_err_t scd41_set_asc_enabled(bool enable);
esp_err_t scd41_forced_recalibration(uint16_t target_ppm, int16_t *correction_ppm);

/* Được phép gọi khi đang đo định kỳ */
esp_err_t scd41_set_ambient_pressure(uint32_t pressure_pa);

#ifdef __cplusplus
}
#endif