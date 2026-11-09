#pragma once
/*
 * i2c_bus.h - Bus I2C dùng chung (OLED + SCD40 + SGP40 trên cùng SDA/SCL)
 *
 * ESP-IDF chỉ cho tạo MỘT bus trên mỗi cổng I2C; mọi thiết bị phải gắn vào cùng handle này.
 * Driver i2c_master tự khoá bus, nên các task khác nhau (UI, cảm biến) gọi song song vẫn an toàn.
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tạo bus (gọi nhiều lần không sao). Trả lỗi nếu chân/cổng sai. */
esp_err_t i2c_bus_init(void);

/* Handle bus, NULL nếu khởi tạo thất bại */
i2c_master_bus_handle_t i2c_bus_get(void);

/* Kiểm tra có thiết bị trả lời ở địa chỉ addr không */
bool i2c_bus_probe(uint8_t addr);

/* Gỡ bus bị treo (thiết bị giữ SDA ở mức thấp sau sụt nguồn / lỏng dây): phát 9 xung SCL + STOP
 * rồi reset khối I2C. Tự giới hạn tối đa 1 lần/giây. Trả ESP_OK nếu đã thực hiện. */
esp_err_t i2c_bus_recover(void);

#ifdef __cplusplus
}
#endif