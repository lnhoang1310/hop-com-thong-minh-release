#pragma once
/*
 * ota_update.h - Cập nhật firmware qua mạng (OTA)
 *
 * Hai cách:
 *   1. Web cấu hình (bật AP bằng cách giữ OK 3s) -> mục "Cập nhật firmware" -> chọn file
 *      build/hop_com_thong_minh.bin -> trình duyệt gửi file lên /api/ota.
 *   2. Lệnh MQTT {"cmd":"ota","url":"http://192.168.1.10:8000/hop_com_thong_minh.bin"}
 *      -> thiết bị tự tải file (http hoặc https).
 * Thiết bị kiểm tra file đúng là firmware của dự án này (tên project + chip) trước khi ghi xong,
 * ghi vào phân vùng OTA còn trống, rồi khởi động lại vào bản mới. Bản đang chạy không bị xoá:
 * nếu bản mới lỗi khởi động (và bật rollback trong menuconfig) thì tự quay về bản cũ.
 *
 * Cần bảng phân vùng có 2 phân vùng OTA (menuconfig -> Partition Table ->
 * "Two large size OTA partitions", flash 4 MB).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_RUNNING,
    OTA_STATE_DONE,      /* ghi xong, sắp khởi động lại */
    OTA_STATE_FAILED,
} ota_state_t;

/* Ghi từng phần (dùng cho tải lên qua web). total_size = 0 nếu không biết trước. */
esp_err_t ota_update_begin(size_t total_size, const char *source);
esp_err_t ota_update_write(const void *data, size_t len);
/* Kiểm tra ảnh, đặt phân vùng khởi động, hẹn khởi động lại sau reboot_delay_ms */
esp_err_t ota_update_end(uint32_t reboot_delay_ms);
void      ota_update_abort(const char *reason);

/* Tải firmware từ URL trong task riêng (không chặn). */
esp_err_t ota_update_start_url(const char *url);

ota_state_t ota_update_state(void);
uint8_t     ota_update_progress(void);       /* 0..100 */
const char *ota_update_message(void);        /* ASCII không dấu, cho OLED */
bool        ota_update_supported(void);      /* có phân vùng OTA để ghi không */
const char *ota_update_running_partition(void);

/* Gọi khi khởi động xong: xác nhận bản đang chạy là tốt (huỷ quay về bản cũ nếu bật rollback) */
void ota_update_mark_valid(void);

#ifdef __cplusplus
}
#endif