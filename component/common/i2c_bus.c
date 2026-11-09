/*
 * i2c_bus.c - Bus I2C dùng chung
 */
#include "i2c_bus.h"
#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "I2C";

static i2c_master_bus_handle_t s_bus;
static bool s_tried;

esp_err_t i2c_bus_init(void)
{
    if (s_bus) return ESP_OK;
    if (s_tried) return ESP_FAIL;      /* đã thử và lỗi, không thử lại */
    s_tried = true;

    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_MAIN_PORT,
        .sda_io_num = I2C_MAIN_SDA,
        .scl_io_num = I2C_MAIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   /* yếu (~45k); nên có điện trở 4.7k trên module */
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Không tạo được bus I2C (SDA=%d SCL=%d): %s", I2C_MAIN_SDA, I2C_MAIN_SCL,
                 esp_err_to_name(err));
        s_bus = NULL;
        return err;
    }

    /* Liệt kê thiết bị để dễ kiểm tra dây nối */
    static const struct { uint8_t addr; const char *name; } known[] = {
        { OLED_I2C_ADDRESS, "OLED" },
        { SCD41_I2C_ADDRESS, "SCD40" },
        { SGP40_I2C_ADDRESS, "SGP40" },
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        bool ok = i2c_master_probe(s_bus, known[i].addr, 50) == ESP_OK;
        ESP_LOGI(TAG, "  0x%02X %-6s %s", known[i].addr, known[i].name, ok ? "OK" : "KHONG THAY");
    }
    return ESP_OK;
}

i2c_master_bus_handle_t i2c_bus_get(void)
{
    if (!s_bus) i2c_bus_init();
    return s_bus;
}

esp_err_t i2c_bus_recover(void)
{
    static int64_t s_last_us;
    if (!s_bus) return ESP_ERR_INVALID_STATE;
    int64_t now = esp_timer_get_time();
    if (s_last_us && now - s_last_us < 1000000) return ESP_ERR_TIMEOUT;
    s_last_us = now;
    esp_err_t err = i2c_master_bus_reset(s_bus);
    ESP_LOGW(TAG, "Gỡ bus I2C bị treo: %s", esp_err_to_name(err));
    return err;
}

bool i2c_bus_probe(uint8_t addr)
{
    i2c_master_bus_handle_t bus = i2c_bus_get();
    return bus && i2c_master_probe(bus, addr, 50) == ESP_OK;
}