#pragma once
/*
 * ui_display.h - Giao diện OLED 128x64 dạng thanh "loading"
 *
 * Màn hình:
 *   BOOT        : logo + thanh chạy
 *   WARMUP      : làm nóng cảm biến MQ (thanh % + thời gian còn lại)
 *   CALIBRATING : hiệu chuẩn MQ
 *   FOOD_MENU   : menu chọn loại thực phẩm (UP/DOWN chọn, OK xác nhận)
 *   OTA         : đang cập nhật firmware (thanh % + trạng thái)
 *   MAIN        : 4 trang, chuyển bằng nút UP/DOWN, OK về trang Tổng quan
 *       0 TONG QUAN : món + tuổi phiên, mức kết quả, thanh chỉ số khí S
 *       1 KHI       : mức thay đổi so với nền của NH3, H2S, VOC, CO2
 *       2 MOI TRUONG: nhiệt độ, độ ẩm, quỹ thời gian an toàn 120 phút
 *       3 MANG      : trạng thái WiFi / AP cấu hình
 * Hàm render chỉ vẽ vào framebuffer (oled_gfx), không đụng phần cứng.
 */
#include <stdbool.h>
#include <stdint.h>
#include "food_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_SCREEN_BOOT = 0,
    UI_SCREEN_WARMUP,
    UI_SCREEN_CALIBRATING,
    UI_SCREEN_MAIN,
    UI_SCREEN_FOOD_MENU,
    UI_SCREEN_OTA,
} ui_screen_t;

typedef enum {
    UI_PAGE_OVERVIEW = 0,
    UI_PAGE_GAS,
    UI_PAGE_ENV,
    UI_PAGE_NETWORK,
    UI_PAGE_COUNT,
} ui_page_t;

typedef enum {
    UI_WIFI_NOT_CONFIGURED = 0,
    UI_WIFI_CONNECTING,
    UI_WIFI_CONNECTED,
    UI_WIFI_FAILED,
    UI_WIFI_DISCONNECTED,
} ui_wifi_state_t;

typedef struct {
    ui_screen_t screen;
    ui_page_t   page;
    uint32_t    frame;            /* tăng mỗi lần vẽ, dùng cho hiệu ứng */

    uint8_t     progress;         /* % cho BOOT/WARMUP/CALIBRATING */
    uint32_t    remaining_s;
    uint8_t     menu_index;       /* dòng đang chọn trong menu món (food_group_t) */
    char        ota_msg[24];      /* trạng thái cập nhật firmware */

    food_sensor_data_t data;
    food_safety_t      safety;
    food_session_t     session;   /* phiên đo: món, tuổi, quỹ thời gian, giá trị nền */
    bool        cloud_online;     /* đang kết nối MQTT server */
    char        cloud_status[24]; /* lý do chưa kết nối server (cloud_manager_status_text) */
    char        broker[24];       /* host:port broker, hiện trên trang Kết nối */

    ui_wifi_state_t wifi_state;
    char        sta_ssid[33];
    char        sta_ip[16];
    int8_t      rssi;
    bool        portal_active;
    char        ap_ssid[33];
    uint8_t     ap_clients;

    char        toast[28];        /* chuỗi khác rỗng -> hiện hộp thông báo */
} ui_model_t;

/* Vẽ toàn bộ model vào framebuffer */
void ui_display_render(const ui_model_t *m);

#ifdef __cplusplus
}
#endif