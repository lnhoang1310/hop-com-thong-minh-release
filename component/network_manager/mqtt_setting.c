/*
 * mqtt_settings.c - Đọc / ghi cấu hình broker trong NVS (namespace "mqtt")
 */
#include "mqtt_setting.h"
#include "app_config.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char *TAG = "MQTTCFG";
#define NS "mqtt"

/* Tách "mqtt://host:port" / "mqtts://host" trong app_config.h làm giá trị mặc định */
static void load_defaults(mqtt_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    const char *u = MQTT_BROKER_URI;
    s->tls = MQTT_USE_TLS || strncmp(u, "mqtts://", 8) == 0 || strncmp(u, "ssl://", 6) == 0;
    const char *p = strstr(u, "://");
    p = p ? p + 3 : u;
    const char *colon = strchr(p, ':');
    size_t hl = colon ? (size_t)(colon - p) : strlen(p);
    if (hl >= sizeof(s->host)) hl = sizeof(s->host) - 1;
    memcpy(s->host, p, hl);
    s->host[hl] = '\0';
    s->port = colon ? (uint16_t)atoi(colon + 1) : (s->tls ? 8883 : 1883);
    strlcpy(s->user, MQTT_USERNAME, sizeof(s->user));
    strlcpy(s->pass, MQTT_PASSWORD, sizeof(s->pass));
    strlcpy(s->root, MQTT_TOPIC_ROOT, sizeof(s->root));
}

esp_err_t mqtt_settings_load(mqtt_settings_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    load_defaults(out);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;   /* chưa lưu -> mặc định */

    char host[MQTT_SET_HOST_LEN];
    size_t len = sizeof(host);
    if (nvs_get_str(h, "host", host, &len) == ESP_OK && host[0]) {
        strlcpy(out->host, host, sizeof(out->host));
        uint16_t port;
        uint8_t tls;
        if (nvs_get_u16(h, "port", &port) == ESP_OK) out->port = port;
        if (nvs_get_u8(h, "tls", &tls) == ESP_OK) out->tls = tls != 0;
        len = sizeof(out->user); if (nvs_get_str(h, "user", out->user, &len) != ESP_OK) out->user[0] = '\0';
        len = sizeof(out->pass); if (nvs_get_str(h, "pass", out->pass, &len) != ESP_OK) out->pass[0] = '\0';
        len = sizeof(out->root); nvs_get_str(h, "root", out->root, &len);
        len = 0;
        if (nvs_get_str(h, "ca", NULL, &len) == ESP_OK && len > 1) {
            out->ca = malloc(len);
            if (out->ca && nvs_get_str(h, "ca", out->ca, &len) != ESP_OK) {
                free(out->ca);
                out->ca = NULL;
            }
        }
    }
    nvs_close(h);
    return ESP_OK;
}

void mqtt_settings_free(mqtt_settings_t *s)
{
    if (s && s->ca) {
        free(s->ca);
        s->ca = NULL;
    }
}

const char *mqtt_settings_validate(const mqtt_settings_t *s)
{
    if (!s->host[0]) return "Chưa nhập địa chỉ broker";
    for (const char *p = s->host; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '.' || *p == '-' || *p == '_')) {
            return "Địa chỉ broker chỉ gồm chữ, số, dấu chấm, gạch ngang (không có mqtt:// hay cổng)";
        }
    }
    if (s->port == 0) return "Cổng phải từ 1 đến 65535";
    if (!s->root[0]) return "Chưa nhập topic gốc";
    for (const char *p = s->root; *p; p++) {
        if (*p == '+' || *p == '#' || *p == ' ') return "Topic gốc không được chứa dấu +, # hoặc khoảng trắng";
    }
    if (s->root[0] == '/' || s->root[strlen(s->root) - 1] == '/') return "Topic gốc không bắt đầu/kết thúc bằng '/'";
    if (s->ca && s->ca[0]) {
        if (!s->tls) return "Chỉ dùng chứng chỉ CA khi bật TLS";
        if (!strstr(s->ca, "-----BEGIN CERTIFICATE-----") || !strstr(s->ca, "-----END CERTIFICATE-----")) {
            return "Chứng chỉ CA phải ở dạng PEM (-----BEGIN CERTIFICATE----- ...)";
        }
        if (strlen(s->ca) >= MQTT_SET_CA_MAX) return "Chứng chỉ CA quá dài (tối đa khoảng 4000 ký tự)";
    }
    return NULL;
}

esp_err_t mqtt_settings_save(const mqtt_settings_t *s)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "host", s->host);
    if (err == ESP_OK) err = nvs_set_u16(h, "port", s->port);
    if (err == ESP_OK) err = nvs_set_u8(h, "tls", s->tls ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, "user", s->user);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", s->pass);
    if (err == ESP_OK) err = nvs_set_str(h, "root", s->root);
    if (err == ESP_OK) {
        if (s->ca && s->ca[0]) {
            err = nvs_set_str(h, "ca", s->ca);
        } else {
            esp_err_t e = nvs_erase_key(h, "ca");
            if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) err = e;
        }
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Đã lưu broker %s://%s:%u, topic '%s'%s", s->tls ? "mqtts" : "mqtt", s->host,
                 s->port, s->root, (s->ca && s->ca[0]) ? ", CA tự dán" : "");
    } else {
        ESP_LOGE(TAG, "Lưu cấu hình broker lỗi: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t mqtt_settings_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Đã xoá cấu hình broker, dùng mặc định trong app_config.h");
    return err;
}

void mqtt_settings_uri(const mqtt_settings_t *s, char *buf, size_t len)
{
    snprintf(buf, len, "%s://%s:%u", s->tls ? "mqtts" : "mqtt", s->host, s->port);
}