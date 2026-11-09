#pragma once
/*
 * wifi_portal.h - Web cấu hình WiFi (captive portal)
 *
 *  GET  /                   trang cấu hình (wifi_setting.html)
 *  GET  /wifi_setting.css
 *  GET  /wifi_setting.js
 *  GET  /api/status         trạng thái WiFi (JSON)
 *  GET  /api/scan           quét mạng xung quanh (JSON)
 *  POST /api/connect        {"ssid":"..","password":".."}  hoặc  {"reconnect":true}
 *  POST /api/disconnect     {"forget":false}
 *  Mọi URL khác -> chuyển hướng 302 về trang cấu hình (để điện thoại tự bật trang).
 */
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_portal_start(const char *ap_ip);
void      wifi_portal_stop(void);
bool      wifi_portal_is_running(void);

#ifdef __cplusplus
}
#endif