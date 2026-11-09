/*
 * sgp40.c - Driver SGP40
 */
#include "sgp40.h"
#include "sensirion_i2c.h"
#include "app_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>
#include <inttypes.h>

static const char *TAG = "SGP40";

#define SGP40_CMD_MEASURE_RAW   0x260F
#define SGP40_CMD_SELF_TEST     0x280E
#define SGP40_CMD_HEATER_OFF    0x3615
#define SGP40_CMD_GET_SERIAL    0x3682
#define SGP40_SELF_TEST_OK      0xD400

static i2c_master_dev_handle_t s_dev;

esp_err_t sgp40_init(i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }
    if (i2c_master_probe(bus, addr, SENSIRION_I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "Không tìm thấy SGP40 tại 0x%02X", addr);
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_MAIN_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }
    uint64_t serial = 0;
    if (sgp40_get_serial(&serial) == ESP_OK) {
        ESP_LOGI(TAG, "SGP40 OK, serial 0x%012" PRIX64, serial);
    }
    if (sgp40_self_test() != ESP_OK) {
        ESP_LOGW(TAG, "Self-test SGP40 thất bại");
    }
    return ESP_OK;
}

esp_err_t sgp40_deinit(void)
{
    if (!s_dev) {
        return ESP_OK;
    }
    sgp40_heater_off();
    esp_err_t err = i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return err;
}

bool sgp40_is_ready(void)
{
    return s_dev != NULL;
}

esp_err_t sgp40_measure_raw(float temperature_c, float humidity_rh, uint16_t *sraw)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t args[2] = { 0x8000, 0x6666 };   /* 50 %RH, 25 °C */
    if (!isnan(humidity_rh) && !isnan(temperature_c)) {
        if (humidity_rh < 0) humidity_rh = 0;
        if (humidity_rh > 100) humidity_rh = 100;
        if (temperature_c < -45) temperature_c = -45;
        if (temperature_c > 130) temperature_c = 130;
        args[0] = (uint16_t)(humidity_rh * 65535.0f / 100.0f);
        args[1] = (uint16_t)((temperature_c + 45.0f) * 65535.0f / 175.0f);
    }
    esp_err_t err = sensirion_send_cmd_args(s_dev, SGP40_CMD_MEASURE_RAW, args, 2);
    if (err != ESP_OK) {
        return err;
    }
    sensirion_delay_ms(30);
    uint16_t w;
    err = sensirion_read_words(s_dev, &w, 1);
    if (err == ESP_OK && sraw) {
        *sraw = w;
    }
    return err;
}

esp_err_t sgp40_self_test(void)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = sensirion_send_cmd(s_dev, SGP40_CMD_SELF_TEST);
    if (err != ESP_OK) {
        return err;
    }
    sensirion_delay_ms(320);
    uint16_t w;
    err = sensirion_read_words(s_dev, &w, 1);
    if (err != ESP_OK) {
        return err;
    }
    return (w == SGP40_SELF_TEST_OK) ? ESP_OK : ESP_FAIL;
}

esp_err_t sgp40_heater_off(void)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return sensirion_send_cmd(s_dev, SGP40_CMD_HEATER_OFF);
}

esp_err_t sgp40_get_serial(uint64_t *serial)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = sensirion_send_cmd(s_dev, SGP40_CMD_GET_SERIAL);
    if (err != ESP_OK) {
        return err;
    }
    sensirion_delay_ms(1);
    uint16_t w[3];
    err = sensirion_read_words(s_dev, w, 3);
    if (err == ESP_OK && serial) {
        *serial = ((uint64_t)w[0] << 32) | ((uint64_t)w[1] << 16) | w[2];
    }
    return err;
}