/*
 * scd41.c - Driver SCD41
 */
#include "scd41.h"
#include "sensirion_i2c.h"
#include "app_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <inttypes.h>

static const char *TAG = "SCD41";

#define SCD41_CMD_START_PERIODIC        0x21B1
#define SCD41_CMD_START_LP_PERIODIC     0x21AC
#define SCD41_CMD_READ_MEASUREMENT      0xEC05
#define SCD41_CMD_STOP_PERIODIC         0x3F86
#define SCD41_CMD_GET_DATA_READY        0xE4B8
#define SCD41_CMD_SET_TEMP_OFFSET       0x241D
#define SCD41_CMD_SET_ALTITUDE          0x2427
#define SCD41_CMD_SET_AMBIENT_PRESSURE  0xE000
#define SCD41_CMD_FORCED_RECAL          0x362F
#define SCD41_CMD_SET_ASC               0x2416
#define SCD41_CMD_GET_SERIAL            0x3682
#define SCD41_CMD_REINIT                0x3646
#define SCD41_CMD_WAKE_UP               0x36F6

static i2c_master_dev_handle_t s_dev;
static bool s_periodic;

#define CHECK_DEV() do { if (!s_dev) return ESP_ERR_INVALID_STATE; } while (0)

static esp_err_t cmd_read(uint16_t cmd, uint32_t wait_ms, uint16_t *words, size_t n)
{
    esp_err_t err = sensirion_send_cmd(s_dev, cmd);
    if (err != ESP_OK) {
        return err;
    }
    sensirion_delay_ms(wait_ms);
    return sensirion_read_words(s_dev, words, n);
}

esp_err_t scd41_init(i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
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

    /* Cảm biến có thể đang đo định kỳ (sau soft reset ESP) hoặc đang ngủ */
    sensirion_send_cmd(s_dev, SCD41_CMD_WAKE_UP);       /* lệnh này không ACK -> bỏ qua lỗi */
    sensirion_delay_ms(30);
    sensirion_send_cmd(s_dev, SCD41_CMD_STOP_PERIODIC);
    sensirion_delay_ms(500);

    uint64_t serial = 0;
    err = scd41_get_serial(&serial);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Không đọc được SCD41 (%s)", esp_err_to_name(err));
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "SCD41 OK, serial 0x%012" PRIX64, serial);
    s_periodic = false;
    return ESP_OK;
}

esp_err_t scd41_deinit(void)
{
    if (!s_dev) {
        return ESP_OK;
    }
    scd41_stop_periodic();
    esp_err_t err = i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    return err;
}

bool scd41_is_ready(void)
{
    return s_dev != NULL;
}

esp_err_t scd41_start_periodic(void)
{
    CHECK_DEV();
    esp_err_t err = sensirion_send_cmd(s_dev, SCD41_CMD_START_PERIODIC);
    if (err == ESP_OK) {
        s_periodic = true;
    }
    return err;
}

esp_err_t scd41_start_low_power_periodic(void)
{
    CHECK_DEV();
    esp_err_t err = sensirion_send_cmd(s_dev, SCD41_CMD_START_LP_PERIODIC);
    if (err == ESP_OK) {
        s_periodic = true;
    }
    return err;
}

esp_err_t scd41_stop_periodic(void)
{
    CHECK_DEV();
    esp_err_t err = sensirion_send_cmd(s_dev, SCD41_CMD_STOP_PERIODIC);
    sensirion_delay_ms(500);
    s_periodic = false;
    return err;
}

esp_err_t scd41_data_ready(bool *ready)
{
    CHECK_DEV();
    uint16_t w;
    esp_err_t err = cmd_read(SCD41_CMD_GET_DATA_READY, 1, &w, 1);
    if (err == ESP_OK && ready) {
        *ready = (w & 0x07FF) != 0;
    }
    return err;
}

esp_err_t scd41_read_measurement(scd41_measurement_t *out)
{
    CHECK_DEV();
    uint16_t w[3];
    esp_err_t err = cmd_read(SCD41_CMD_READ_MEASUREMENT, 1, w, 3);
    if (err != ESP_OK) {
        return err;
    }
    if (w[0] == 0) {
        return ESP_ERR_INVALID_RESPONSE;   /* mẫu không hợp lệ */
    }
    if (out) {
        out->co2_ppm = w[0];
        out->temperature_c = -45.0f + 175.0f * (float)w[1] / 65535.0f;
        out->humidity_rh = 100.0f * (float)w[2] / 65535.0f;
    }
    return ESP_OK;
}

esp_err_t scd41_get_serial(uint64_t *serial)
{
    CHECK_DEV();
    uint16_t w[3];
    esp_err_t err = cmd_read(SCD41_CMD_GET_SERIAL, 1, w, 3);
    if (err == ESP_OK && serial) {
        *serial = ((uint64_t)w[0] << 32) | ((uint64_t)w[1] << 16) | w[2];
    }
    return err;
}

esp_err_t scd41_set_temperature_offset(float offset_c)
{
    CHECK_DEV();
    if (s_periodic) return ESP_ERR_INVALID_STATE;
    if (offset_c < 0) offset_c = 0;
    uint16_t arg = (uint16_t)(offset_c * 65535.0f / 175.0f + 0.5f);
    esp_err_t err = sensirion_send_cmd_args(s_dev, SCD41_CMD_SET_TEMP_OFFSET, &arg, 1);
    sensirion_delay_ms(1);
    return err;
}

esp_err_t scd41_set_altitude(uint16_t altitude_m)
{
    CHECK_DEV();
    if (s_periodic) return ESP_ERR_INVALID_STATE;
    esp_err_t err = sensirion_send_cmd_args(s_dev, SCD41_CMD_SET_ALTITUDE, &altitude_m, 1);
    sensirion_delay_ms(1);
    return err;
}

esp_err_t scd41_set_ambient_pressure(uint32_t pressure_pa)
{
    CHECK_DEV();
    uint16_t arg = (uint16_t)(pressure_pa / 100);
    esp_err_t err = sensirion_send_cmd_args(s_dev, SCD41_CMD_SET_AMBIENT_PRESSURE, &arg, 1);
    sensirion_delay_ms(1);
    return err;
}

esp_err_t scd41_set_asc_enabled(bool enable)
{
    CHECK_DEV();
    if (s_periodic) return ESP_ERR_INVALID_STATE;
    uint16_t arg = enable ? 1 : 0;
    esp_err_t err = sensirion_send_cmd_args(s_dev, SCD41_CMD_SET_ASC, &arg, 1);
    sensirion_delay_ms(1);
    return err;
}

esp_err_t scd41_forced_recalibration(uint16_t target_ppm, int16_t *correction_ppm)
{
    CHECK_DEV();
    if (s_periodic) return ESP_ERR_INVALID_STATE;
    esp_err_t err = sensirion_send_cmd_args(s_dev, SCD41_CMD_FORCED_RECAL, &target_ppm, 1);
    if (err != ESP_OK) {
        return err;
    }
    sensirion_delay_ms(400);
    uint16_t w;
    err = sensirion_read_words(s_dev, &w, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (w == 0xFFFF) {
        return ESP_FAIL;
    }
    if (correction_ppm) {
        *correction_ppm = (int16_t)((int32_t)w - 0x8000);
    }
    return ESP_OK;
}