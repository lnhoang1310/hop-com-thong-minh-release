#pragma once
/*
 * dns_server.h - DNS "bắt tất cả" cho captive portal
 * Mọi truy vấn tên miền (bản ghi A) đều trả về IP của AP -> điện thoại tự mở trang cấu hình.
 */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t dns_server_start(const char *ip);
void      dns_server_stop(void);

#ifdef __cplusplus
}
#endif