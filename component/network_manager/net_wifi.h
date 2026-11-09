#pragma once
/*
 * net_wifi.h - Quản lý WiFi (STA + AP cấu hình)
 *
 *  - Khởi động: có mạng đã lưu -> tự kết nối; chưa có -> bật AP cấu hình (captive portal)
 *  - Kết nối mới từ web: thử tối đa WIFI_STA_MAX_RETRY lần; thành công mới lưu NVS,
 *    thất bại thì quay lại mạng đã lưu (nếu có)
 *  - Mất mạng: tự kết nối lại (giãn thời gian dần, tối đa 60s)
 *  - Ngắt kết nối từ web: dừng tự kết nối; tuỳ chọn xoá mạng đã lưu
 */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_WIFI_NOT_CONFIGURED = 0,   /* chưa có mạng nào được lưu */
    NET_WIFI_CONNECTING,
    NET_WIFI_CONNECTED,
    NET_WIFI_FAILED,               /* thử kết nối thất bại (vẫn sẽ thử lại nếu có mạng đã lưu) */
    NET_WIFI_DISCONNECTED,         /* người dùng chủ động ngắt */
} net_wifi_state_t;

typedef enum {
    NET_ATTEMPT_NONE = 0,
    NET_ATTEMPT_PENDING,
    NET_ATTEMPT_SUCCESS,
    NET_ATTEMPT_FAILED,
} net_attempt_result_t;

typedef struct {
    net_wifi_state_t state;
    char     ssid[33];             /* mạng đang kết nối / đang thử */
    char     saved_ssid[33];
    bool     has_saved;
    char     ip[16];
    char     gateway[16];
    int8_t   rssi;
    uint8_t  channel;
    char     mac[18];

    /* Kết quả lần kết nối gần nhất do người dùng yêu cầu (từ web) */
    uint32_t attempt_id;
    net_attempt_result_t attempt_result;
    char     attempt_ssid[33];
    char     attempt_error[64];

    bool     portal_active;
    char     ap_ssid[33];
    char     ap_ip[16];
    uint8_t  ap_clients;
} net_wifi_status_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t channel;
    bool    secure;
} net_wifi_ap_info_t;

typedef void (*net_wifi_cb_t)(void *arg);

esp_err_t net_wifi_init(void);
void      net_wifi_set_callback(net_wifi_cb_t cb, void *arg);   /* gọi khi trạng thái thay đổi */

/* Thử kết nối mạng mới. Trả về ngay; theo dõi kết quả qua attempt_id/attempt_result */
esp_err_t net_wifi_connect(const char *ssid, const char *password, uint32_t *attempt_id);
esp_err_t net_wifi_disconnect(bool forget);
esp_err_t net_wifi_reconnect(void);   /* kết nối lại mạng đã lưu sau khi người dùng ngắt */

/* Quét mạng (chặn ~2-4 s). Danh sách đã gộp SSID trùng, sắp xếp theo RSSI giảm dần */
esp_err_t net_wifi_scan(net_wifi_ap_info_t *list, uint16_t max, uint16_t *count);

void      net_wifi_get_status(net_wifi_status_t *out);
bool      net_wifi_is_connected(void);

/* AP cấu hình + captive portal */
esp_err_t net_wifi_portal_start(void);
esp_err_t net_wifi_portal_stop(void);
bool      net_wifi_portal_active(void);

const char *net_wifi_state_str(net_wifi_state_t st);

#ifdef __cplusplus
}
#endif