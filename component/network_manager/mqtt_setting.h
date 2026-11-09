#pragma once
/*
 * mqtt_settings.h - Cấu hình broker MQTT lưu trong NVS (sửa được trên web cấu hình)
 *
 * Lần đầu (NVS trống) lấy mặc định từ app_config.h: MQTT_BROKER_URI, MQTT_USERNAME,
 * MQTT_PASSWORD, MQTT_TOPIC_ROOT. Bấm "Khôi phục mặc định" trên web để quay về các giá trị này.
 *
 * TLS: bật "tls" -> kết nối mqtts://. Chứng chỉ máy chủ được kiểm tra bằng:
 *   - CA tự dán (PEM) nếu có  -> dùng cho broker tự dựng, chứng chỉ tự ký (vd Mosquitto trên laptop)
 *   - bộ CA gốc có sẵn của ESP-IDF nếu để trống -> dùng cho HiveMQ Cloud, EMQX Cloud, ...
 */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_SET_HOST_LEN  64
#define MQTT_SET_USER_LEN  64
#define MQTT_SET_PASS_LEN  64
#define MQTT_SET_ROOT_LEN  48
#define MQTT_SET_CA_MAX    4000    /* giới hạn chuỗi NVS ~4000 byte: đủ cho 1 chứng chỉ PEM */

typedef struct {
    char     host[MQTT_SET_HOST_LEN];
    uint16_t port;
    bool     tls;
    char     user[MQTT_SET_USER_LEN];
    char     pass[MQTT_SET_PASS_LEN];
    char     root[MQTT_SET_ROOT_LEN];
    char    *ca;                    /* PEM, cấp phát động, NULL nếu không có */
} mqtt_settings_t;

/* Đọc cấu hình (NVS, hoặc mặc định nếu chưa lưu). Gọi mqtt_settings_free() khi xong. */
esp_err_t mqtt_settings_load(mqtt_settings_t *out);
void      mqtt_settings_free(mqtt_settings_t *s);

/* Kiểm tra hợp lệ; trả về NULL nếu hợp lệ, ngược lại là câu báo lỗi (tiếng Việt) */
const char *mqtt_settings_validate(const mqtt_settings_t *s);

/* Lưu vào NVS. ca = NULL hoặc "" -> xoá CA tự dán */
esp_err_t mqtt_settings_save(const mqtt_settings_t *s);

/* Xoá cấu hình trong NVS -> lần sau dùng mặc định của app_config.h */
esp_err_t mqtt_settings_reset(void);

/* "mqtt(s)://host:port" */
void mqtt_settings_uri(const mqtt_settings_t *s, char *buf, size_t len);

#ifdef __cplusplus
}
#endif