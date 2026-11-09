#pragma once
/*
 * cloud_protocol.h - Gói tin MQTT giữa Hộp cơm (ESP32-S3) và server (bản rút gọn, 1 thiết bị)
 * Mọi payload là JSON UTF-8. <root> = MQTT_TOPIC_ROOT (app_config.h), vd "hopcom-khkt2026".
 *
 *  Hướng   Topic             QoS Retain  Nội dung
 *  D -> S  <root>/status      1   có     {"state":"online","ip":"192.168.43.37","rssi":-58}
 *                                        {"state":"offline"}   <- Last Will (broker tự gửi khi mất kết nối)
 *  D -> S  <root>/telemetry   1   không  số đo + kết quả tự đánh giá, 10 s/lần và ngay khi mức đổi
 *  S -> D  <root>/cmd         1   không  lệnh điều khiển (không có phản hồi)
 *
 *  telemetry:
 *   {"food":"com","age_s":720,"budget_s":300,
 *    "t":28.5,"rh":67.1,"co2":830,"voc":29876,"nh3":0.912,"h2s":0.975,
 *    "level":"low","gas_level":"low","time_level":"ok","score":18,"msg":"Chua thay bat thuong"}
 *   - food: "com" | "thit" | "ca" | "canh" | "trung"
 *   - age_s: tuổi phiên (s); budget_s: quỹ thời gian đã dùng (s, x2 khi > 32 °C), 3600 cảnh báo, 7200 hết
 *   - t (°C), rh (%), co2 (ppm) từ SCD40; voc = SRAW của SGP40 (VOC càng nhiều số càng NHỎ)
 *   - nh3 / h2s = Rs/R0 của MQ137 / MQ136 (khí càng nhiều số càng NHỎ)
 *   - Cảm biến nào không đo được thì BỎ trường đó
 *   - level, gas_level: "invalid" | "low" | "check" | "strong";  time_level: "ok" | "warn" | "expired"
 *   - score: chỉ số khí S 0..100;  msg: ASCII không dấu (dòng thông báo trên OLED)
 *   - Thuật toán đánh giá (rule-0.1): food_eval.h / food_eval.c
 *
 *  cmd:
 *   {"cmd":"set_food","food":"thit"}      đổi món, giữ phiên + nền
 *   {"cmd":"new_session","food":"ca"}     phiên mới (food tuỳ chọn), lấy lại nền
 *   {"cmd":"set_interval","s":30}         chu kỳ telemetry 5..300 s
 *   {"cmd":"calibrate_mq"}                hiệu chuẩn MQ (không khí sạch)
 *   {"cmd":"reboot"}
 *   {"cmd":"ota","url":"http://192.168.1.10:8000/hop_com_thong_minh.bin"}   cập nhật firmware
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "food_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLOUD_CMD_UNKNOWN = 0,
    CLOUD_CMD_NEW_SESSION,
    CLOUD_CMD_SET_FOOD,
    CLOUD_CMD_SET_INTERVAL,
    CLOUD_CMD_CALIBRATE_MQ,
    CLOUD_CMD_REBOOT,
    CLOUD_CMD_OTA,
} cloud_cmd_type_t;

typedef struct {
    cloud_cmd_type_t type;
    bool             has_food;
    food_group_t     food;
    uint32_t         interval_s;
    char             url[200];        /* lệnh ota */
} cloud_cmd_t;

/* Các hàm build_* trả về chuỗi JSON cấp phát động, caller phải free() */
char *cloud_protocol_build_status(const char *ip, int8_t rssi);
char *cloud_protocol_build_telemetry(const food_sensor_data_t *d, const food_session_t *s,
                                     const food_safety_t *r);

/* ESP_ERR_INVALID_RESPONSE: không phải JSON; ESP_ERR_INVALID_ARG: thiếu/sai tham số */
esp_err_t cloud_protocol_parse_cmd(const char *json, size_t len, cloud_cmd_t *out);

bool food_group_from_code(const char *code, food_group_t *out);

#ifdef __cplusplus
}
#endif