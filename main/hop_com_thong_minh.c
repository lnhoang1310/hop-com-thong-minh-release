/*
 * Hộp cơm thông minh - ESP32-S3 SuperMini
 *
 * Thứ tự khởi động:
 *   NVS -> bus I2C dùng chung -> OLED -> cảm biến -> WiFi (+ AP cấu hình nếu chưa có mạng) -> FSM
 */
#include <stdio.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "app_config.h"
#include "i2c_bus.h"
#include "oled_sh1106.h"
#include "sensor_manager.h"
#include "net_wifi.h"
#include "app_fsm.h"
#include "ota.h"

static const char *TAG = "MAIN";

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS lỗi/định dạng cũ -> xoá và khởi tạo lại");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== Hop com thong minh khoi dong ===");
    nvs_init();

    /* Các khối dưới đây lỗi thì chỉ log, thiết bị vẫn chạy phần còn lại */
    if (i2c_bus_init() != ESP_OK)     ESP_LOGE(TAG, "Bus I2C lỗi (kiểm tra SDA/SCL)");
    if (oled_init() != ESP_OK)        ESP_LOGE(TAG, "OLED lỗi");
    // sensor_manager_init();
    if (net_wifi_init() != ESP_OK)    ESP_LOGE(TAG, "WiFi lỗi");
    sensor_manager_init();

    ESP_ERROR_CHECK(app_fsm_start());
    ota_update_mark_valid();          /* chạy tới đây = firmware tốt, không cần quay về bản cũ */
    ESP_LOGI(TAG, "Khởi động xong");
}