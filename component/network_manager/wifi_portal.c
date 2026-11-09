/*
 * wifi_portal.c - HTTP server + DNS cho trang cấu hình WiFi
 */
#include "wifi_portal.h"
#include "net_wifi.h"
#include "dns_server.h"
#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include "cJSON.h"
#include "cloud_manager.h"
#include "mqtt_setting.h"
#include "ota.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "PORTAL";

#define MAX_BODY_LEN      512
#define MQTT_BODY_LEN     6144     /* có thể kèm chứng chỉ CA dạng PEM */
#define OTA_CHUNK         4096
#define SCAN_MAX_RESULTS  20

/* File web nhúng (main/CMakeLists.txt -> EMBED_TXTFILES, có thêm '\0' ở cuối) */
extern const char html_start[] asm("_binary_wifi_setting_html_start");
extern const char html_end[]   asm("_binary_wifi_setting_html_end");
extern const char css_start[]  asm("_binary_wifi_setting_css_start");
extern const char css_end[]    asm("_binary_wifi_setting_css_end");
extern const char js_start[]   asm("_binary_wifi_setting_js_start");
extern const char js_end[]     asm("_binary_wifi_setting_js_end");

static httpd_handle_t s_server;
static char s_portal_url[40];

/* ------------------------- tiện ích ------------------------- */
static esp_err_t send_embedded(httpd_req_t *req, const char *type, const char *start, const char *end)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    size_t len = (size_t)(end - start);
    if (len > 0 && start[len - 1] == '\0') len--;   /* bỏ '\0' do EMBED_TXTFILES thêm vào */
    return httpd_resp_send(req, start, len);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, txt);
    cJSON_free(txt);
    return err;
}

static esp_err_t send_error_json(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", msg);
    httpd_resp_set_status(req, status);
    return send_json(req, root);
}

/* Đọc body JSON; trả NULL nếu lỗi (đã gửi phản hồi lỗi) */
static cJSON *read_json_body_max(httpd_req_t *req, size_t max_len)
{
    if (req->content_len == 0) {
        return cJSON_CreateObject();
    }
    if (req->content_len > max_len) {
        send_error_json(req, "413 Payload Too Large", "Dữ liệu quá dài");
        return NULL;
    }
    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        send_error_json(req, "500 Internal Server Error", "Hết bộ nhớ");
        return NULL;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (r <= 0) {
            free(buf);
            send_error_json(req, "400 Bad Request", "Không đọc được dữ liệu");
            return NULL;
        }
        got += (size_t)r;
    }
    buf[got] = '\0';
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        send_error_json(req, "400 Bad Request", "JSON không hợp lệ");
    }
    return root;
}

static cJSON *read_json_body(httpd_req_t *req)
{
    return read_json_body_max(req, MAX_BODY_LEN);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return send_json(req, ok);
}

static const char *attempt_str(net_attempt_result_t r)
{
    switch (r) {
    case NET_ATTEMPT_PENDING: return "pending";
    case NET_ATTEMPT_SUCCESS: return "success";
    case NET_ATTEMPT_FAILED:  return "failed";
    default:                  return "none";
    }
}

/* ------------------------- handler ------------------------- */
static esp_err_t h_index(httpd_req_t *req)
{
    return send_embedded(req, "text/html; charset=utf-8", html_start, html_end);
}

static esp_err_t h_css(httpd_req_t *req)
{
    return send_embedded(req, "text/css; charset=utf-8", css_start, css_end);
}

static esp_err_t h_js(httpd_req_t *req)
{
    return send_embedded(req, "application/javascript; charset=utf-8", js_start, js_end);
}

static esp_err_t h_status(httpd_req_t *req)
{
    net_wifi_status_t st;
    net_wifi_get_status(&st);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state", net_wifi_state_str(st.state));
    cJSON_AddStringToObject(root, "ssid", st.ssid);
    cJSON_AddBoolToObject(root, "has_saved", st.has_saved);
    cJSON_AddStringToObject(root, "saved_ssid", st.saved_ssid);
    cJSON_AddStringToObject(root, "ip", st.ip);
    cJSON_AddStringToObject(root, "gateway", st.gateway);
    cJSON_AddNumberToObject(root, "rssi", st.rssi);
    cJSON_AddNumberToObject(root, "channel", st.channel);
    cJSON_AddStringToObject(root, "mac", st.mac);

    cJSON *att = cJSON_AddObjectToObject(root, "attempt");
    cJSON_AddNumberToObject(att, "id", st.attempt_id);
    cJSON_AddStringToObject(att, "result", attempt_str(st.attempt_result));
    cJSON_AddStringToObject(att, "ssid", st.attempt_ssid);
    cJSON_AddStringToObject(att, "error", st.attempt_error);

    cJSON *ap = cJSON_AddObjectToObject(root, "ap");
    cJSON_AddStringToObject(ap, "ssid", st.ap_ssid);
    cJSON_AddStringToObject(ap, "ip", st.ap_ip);
    cJSON_AddNumberToObject(ap, "clients", st.ap_clients);

    cJSON *cl = cJSON_AddObjectToObject(root, "cloud");
    cJSON_AddBoolToObject(cl, "online", cloud_manager_is_online());
    cJSON_AddStringToObject(cl, "status", cloud_manager_status_text());
    cJSON_AddStringToObject(cl, "broker", cloud_manager_broker_text());

    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(dev, "name", "Hộp cơm thông minh");
    cJSON_AddStringToObject(dev, "fw", esp_app_get_description()->version);
    cJSON_AddNumberToObject(dev, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(dev, "heap", esp_get_free_heap_size());
    cJSON_AddStringToObject(dev, "partition", ota_update_running_partition());
    cJSON_AddBoolToObject(dev, "ota", ota_update_supported());
    return send_json(req, root);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    static net_wifi_ap_info_t list[SCAN_MAX_RESULTS];   /* httpd chỉ chạy 1 task -> dùng static an toàn */
    uint16_t n = 0;
    esp_err_t err = net_wifi_scan(list, SCAN_MAX_RESULTS, &n);
    if (err != ESP_OK) {
        return send_error_json(req, "503 Service Unavailable",
                               "Thiết bị đang bận kết nối, vui lòng quét lại sau vài giây");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (uint16_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", list[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", list[i].rssi);
        cJSON_AddNumberToObject(o, "channel", list[i].channel);
        cJSON_AddBoolToObject(o, "secure", list[i].secure);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, root);
}

static esp_err_t h_connect(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) return ESP_OK;

    /* {"reconnect":true} -> kết nối lại mạng đã lưu */
    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "reconnect"))) {
        cJSON_Delete(body);
        if (net_wifi_reconnect() != ESP_OK) {
            return send_error_json(req, "400 Bad Request", "Chưa có mạng nào được lưu");
        }
        cJSON *ok = cJSON_CreateObject();
        cJSON_AddBoolToObject(ok, "ok", true);
        return send_json(req, ok);
    }

    const cJSON *j_ssid = cJSON_GetObjectItem(body, "ssid");
    const cJSON *j_pass = cJSON_GetObjectItem(body, "password");
    const char *ssid = cJSON_IsString(j_ssid) ? j_ssid->valuestring : NULL;
    const char *pass = cJSON_IsString(j_pass) ? j_pass->valuestring : "";

    esp_err_t err;
    uint32_t attempt = 0;
    if (!ssid || !ssid[0] || strlen(ssid) > 32) {
        cJSON_Delete(body);
        return send_error_json(req, "400 Bad Request", "Tên WiFi không hợp lệ (1-32 ký tự)");
    }
    if (strlen(pass) > 64 || (pass[0] && strlen(pass) < 8)) {
        cJSON_Delete(body);
        return send_error_json(req, "400 Bad Request", "Mật khẩu phải từ 8 đến 64 ký tự (để trống nếu mạng mở)");
    }
    err = net_wifi_connect(ssid, pass, &attempt);
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return send_error_json(req, "500 Internal Server Error", "Không thể bắt đầu kết nối");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "attempt_id", attempt);
    return send_json(req, root);
}

static esp_err_t h_disconnect(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) return ESP_OK;
    bool forget = cJSON_IsTrue(cJSON_GetObjectItem(body, "forget"));
    cJSON_Delete(body);
    net_wifi_disconnect(forget);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return send_json(req, root);
}

/* ------------------------- cấu hình broker MQTT ------------------------- */
static esp_err_t h_mqtt_get(httpd_req_t *req)
{
    mqtt_settings_t m;
    mqtt_settings_load(&m);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "host", m.host);
    cJSON_AddNumberToObject(root, "port", m.port);
    cJSON_AddBoolToObject(root, "tls", m.tls);
    cJSON_AddStringToObject(root, "user", m.user);
    cJSON_AddBoolToObject(root, "has_pass", m.pass[0] != '\0');   /* không gửi mật khẩu ra trình duyệt */
    cJSON_AddStringToObject(root, "root", m.root);
    cJSON_AddStringToObject(root, "ca", m.ca ? m.ca : "");
    mqtt_settings_free(&m);
    cJSON_AddBoolToObject(root, "online", cloud_manager_is_online());
    cJSON_AddStringToObject(root, "status", cloud_manager_status_text());
    return send_json(req, root);
}

static void copy_str(const cJSON *body, const char *key, char *dst, size_t len)
{
    const cJSON *j = cJSON_GetObjectItem(body, key);
    if (cJSON_IsString(j)) strlcpy(dst, j->valuestring, len);
}

/*
 * POST /api/mqtt  {"host","port","tls","user","pass"?,"root","ca"}  -> lưu + kết nối lại
 *   bỏ trường "pass" = giữ mật khẩu cũ;  {"reset":true} = về mặc định app_config.h
 */
static esp_err_t h_mqtt_post(httpd_req_t *req)
{
    cJSON *body = read_json_body_max(req, MQTT_BODY_LEN);
    if (!body) return ESP_OK;

    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "reset"))) {
        cJSON_Delete(body);
        mqtt_settings_reset();
        cloud_manager_apply_settings();
        return send_ok(req);
    }

    mqtt_settings_t m;
    mqtt_settings_load(&m);                  /* giữ giá trị cũ cho trường không gửi */
    const cJSON *j;
    copy_str(body, "host", m.host, sizeof(m.host));
    copy_str(body, "user", m.user, sizeof(m.user));
    copy_str(body, "pass", m.pass, sizeof(m.pass));
    copy_str(body, "root", m.root, sizeof(m.root));
    if (cJSON_IsNumber(j = cJSON_GetObjectItem(body, "port"))) {
        m.port = (j->valuedouble >= 1 && j->valuedouble <= 65535) ? (uint16_t)j->valuedouble : 0;
    }
    if (cJSON_IsBool(j = cJSON_GetObjectItem(body, "tls"))) m.tls = cJSON_IsTrue(j);
    if (cJSON_IsString(j = cJSON_GetObjectItem(body, "ca"))) {
        mqtt_settings_free(&m);
        m.ca = j->valuestring[0] ? strdup(j->valuestring) : NULL;
    }
    cJSON_Delete(body);

    const char *bad = mqtt_settings_validate(&m);
    esp_err_t err = bad ? ESP_ERR_INVALID_ARG : mqtt_settings_save(&m);
    mqtt_settings_free(&m);
    if (bad) return send_error_json(req, "400 Bad Request", bad);
    if (err != ESP_OK) return send_error_json(req, "500 Internal Server Error", "Không lưu được cấu hình");
    cloud_manager_apply_settings();
    return send_ok(req);
}

/* ------------------------- cập nhật firmware ------------------------- */
/* POST /api/ota, body = nội dung file .bin (application/octet-stream) */
static esp_err_t h_ota(httpd_req_t *req)
{
    if (!ota_update_supported()) {
        return send_error_json(req, "400 Bad Request",
                               "Thiết bị chưa có phân vùng OTA. Nạp lại qua cáp USB với bảng phân vùng "
                               "\"Two large size OTA partitions\" trước.");
    }
    if (req->content_len < 1024) {
        return send_error_json(req, "400 Bad Request", "File quá nhỏ, hãy chọn file .bin của dự án");
    }
    if (ota_update_begin(req->content_len, "web") != ESP_OK) {
        return send_error_json(req, "400 Bad Request", ota_update_state() == OTA_STATE_RUNNING
                               ? "Đang có một bản cập nhật chạy" : "Không có chỗ cho firmware mới");
    }
    char *buf = malloc(OTA_CHUNK);
    if (!buf) {
        ota_update_abort("hết bộ nhớ");
        return send_error_json(req, "500 Internal Server Error", "Hết bộ nhớ");
    }
    size_t left = req->content_len;
    int timeouts = 0;
    esp_err_t err = ESP_OK;
    while (left > 0) {
        int r = httpd_req_recv(req, buf, left < OTA_CHUNK ? left : OTA_CHUNK);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 6) continue;
        if (r <= 0) {
            ota_update_abort("mất kết nối khi nhận file");
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
        err = ota_update_write(buf, (size_t)r);
        if (err != ESP_OK) break;
        left -= (size_t)r;
    }
    free(buf);
    if (err == ESP_OK) err = ota_update_end(2500);   /* chờ 2,5 s cho trình duyệt nhận phản hồi */
    if (err != ESP_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Cập nhật thất bại: %s", ota_update_message());
        return send_error_json(req, "400 Bad Request", msg);
    }
    cJSON *ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    cJSON_AddStringToObject(ok, "message", "Đã cập nhật, thiết bị đang khởi động lại");
    return send_json(req, ok);
}

/* Mọi URL lạ (generate_204, hotspot-detect.html, connecttest.txt, ...) -> trang cấu hình */
static esp_err_t h_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_portal_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
}

/* ------------------------- start / stop ------------------------- */
esp_err_t wifi_portal_start(const char *ap_ip)
{
    if (s_server) return ESP_OK;
    snprintf(s_portal_url, sizeof(s_portal_url), "http://%s/", ap_ip && ap_ip[0] ? ap_ip : "192.168.4.1");

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.max_open_sockets = 5;           /* 5 + 3 nội bộ + DNS + MQTT <= LWIP_MAX_SOCKETS */
    cfg.lru_purge_enable = true;        /* điện thoại mở nhiều kết nối -> tự đóng cái cũ */
    cfg.stack_size = 6144;
    cfg.recv_wait_timeout = 10;         /* tải firmware ~1 MB qua WiFi của hộp */
    cfg.send_wait_timeout = 5;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",                  .method = HTTP_GET,  .handler = h_index },
        { .uri = "/index.html",        .method = HTTP_GET,  .handler = h_index },
        { .uri = "/wifi_setting.css",  .method = HTTP_GET,  .handler = h_css },
        { .uri = "/wifi_setting.js",   .method = HTTP_GET,  .handler = h_js },
        { .uri = "/api/status",        .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/scan",          .method = HTTP_GET,  .handler = h_scan },
        { .uri = "/api/connect",       .method = HTTP_POST, .handler = h_connect },
        { .uri = "/api/disconnect",    .method = HTTP_POST, .handler = h_disconnect },
        { .uri = "/api/mqtt",          .method = HTTP_GET,  .handler = h_mqtt_get },
        { .uri = "/api/mqtt",          .method = HTTP_POST, .handler = h_mqtt_post },
        { .uri = "/api/ota",           .method = HTTP_POST, .handler = h_ota },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, h_redirect);

    dns_server_start(ap_ip && ap_ip[0] ? ap_ip : "192.168.4.1");
    ESP_LOGI(TAG, "Web cấu hình chạy tại %s", s_portal_url);
    return ESP_OK;
}

void wifi_portal_stop(void)
{
    dns_server_stop();
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}

bool wifi_portal_is_running(void)
{
    return s_server != NULL;
}