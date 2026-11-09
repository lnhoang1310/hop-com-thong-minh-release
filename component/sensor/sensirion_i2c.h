#pragma once
/*
 * sensirion_i2c.h - Hàm dùng chung cho cảm biến Sensirion (SCD40/SCD41, SGP40)
 * CRC-8: đa thức 0x31, khởi tạo 0xFF
 */
#include <stdint.h>
#include <stddef.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SENSIRION_I2C_TIMEOUT_MS 100

/*
 * Chờ ÍT NHẤT `ms` mili-giây, đúng với mọi CONFIG_FREERTOS_HZ.
 * Với tick 10 ms (HZ=100), pdMS_TO_TICKS(1) = 0 và vTaskDelay(n) có thể ngắn hơn n tick,
 * nên: thời gian ngắn -> chờ bận chính xác; thời gian dài -> cộng thêm 1 tick.
 */
static inline void sensirion_delay_ms(uint32_t ms)
{
    if (ms < 20) {
        esp_rom_delay_us(ms * 1000);
    } else {
        vTaskDelay(pdMS_TO_TICKS(ms) + 1);
    }
}

static inline uint8_t sensirion_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* Gửi lệnh 16 bit (MSB trước) */
static inline esp_err_t sensirion_send_cmd(i2c_master_dev_handle_t dev, uint16_t cmd)
{
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    return i2c_master_transmit(dev, buf, sizeof(buf), SENSIRION_I2C_TIMEOUT_MS);
}

/* Gửi lệnh 16 bit kèm n word tham số (mỗi word tự thêm CRC) */
static inline esp_err_t sensirion_send_cmd_args(i2c_master_dev_handle_t dev, uint16_t cmd,
                                                const uint16_t *args, size_t n_args)
{
    uint8_t buf[2 + 3 * 4];
    if (n_args > 4) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t idx = 0;
    buf[idx++] = (uint8_t)(cmd >> 8);
    buf[idx++] = (uint8_t)(cmd & 0xFF);
    for (size_t i = 0; i < n_args; i++) {
        buf[idx++] = (uint8_t)(args[i] >> 8);
        buf[idx++] = (uint8_t)(args[i] & 0xFF);
        uint8_t crc = sensirion_crc8(&buf[idx - 2], 2);
        buf[idx++] = crc;
    }
    return i2c_master_transmit(dev, buf, idx, SENSIRION_I2C_TIMEOUT_MS);
}

/* Đọc n word (mỗi word 2 byte + 1 CRC), kiểm tra CRC */
static inline esp_err_t sensirion_read_words(i2c_master_dev_handle_t dev, uint16_t *words, size_t n_words)
{
    uint8_t buf[3 * 9];
    if (n_words > 9) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = i2c_master_receive(dev, buf, n_words * 3, SENSIRION_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < n_words; i++) {
        const uint8_t *p = &buf[i * 3];
        if (sensirion_crc8(p, 2) != p[2]) {
            return ESP_ERR_INVALID_CRC;
        }
        words[i] = (uint16_t)((p[0] << 8) | p[1]);
    }
    return ESP_OK;
}