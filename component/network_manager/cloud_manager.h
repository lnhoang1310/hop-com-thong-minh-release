#pragma once
/*
 * cloud_manager.h - Client MQTT kết nối server (gói tin: xem cloud_protocol.h)
 *
 *  - Tự kết nối broker khi WiFi có IP, tự kết nối lại khi rớt
 *  - Broker, cổng, TLS, tài khoản, topic gốc lấy từ mqtt_settings (sửa được trên web cấu hình)
 *  - Đồng bộ giờ SNTP (tính thời gian tắt máy vào quỹ thời gian)
 *  - Gửi telemetry (số đo + kết quả tự đánh giá); nhận cmd -> gọi cmd_cb (FSM xử lý, không phản hồi)
 * Các callback chạy trong task MQTT: chỉ nên đẩy sự kiện vào hàng đợi, không xử lý lâu.
 */
#include <stdbool.h>
#include "esp_err.h"
#include "food_types.h"
#include "cloud_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*cloud_cmd_cb_t)(const cloud_cmd_t *cmd, void *arg);

esp_err_t   cloud_manager_init(cloud_cmd_cb_t cmd_cb, void *arg);
bool        cloud_manager_is_online(void);
/* Trạng thái/lý do lỗi kết nối server, ASCII ngắn (<= 17 ký tự) để hiện trên OLED */
const char *cloud_manager_status_text(void);
const char *cloud_manager_device_id(void);
/* "host:port" của broker đang dùng */
const char *cloud_manager_broker_text(void);
/* Đọc lại cấu hình broker (mqtt_settings) và kết nối lại. Gọi sau khi lưu cấu hình trên web. */
esp_err_t   cloud_manager_apply_settings(void);

/* Chu kỳ telemetry hiện tại (giây) */
void        cloud_manager_set_interval(uint32_t interval_s);
uint32_t    cloud_manager_get_interval(void);

esp_err_t cloud_manager_publish_telemetry(const food_sensor_data_t *d, const food_session_t *s,
                                          const food_safety_t *r);

/* Giờ thực Unix (ms) nếu đã đồng bộ SNTP, ngược lại 0 */
int64_t   cloud_manager_unix_ms(void);

#ifdef __cplusplus
}
#endif