/*
 * net_wifi.c - Quản lý WiFi STA + AP cấu hình
 */
#include "net_wifi.h"
#include "wifi_portal.h"
#include "app_config.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "WIFI";

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_KEY_SSID  "ssid"
#define WIFI_NVS_KEY_PASS  "pass"
#define SCAN_MAX_RECORDS   30
#define RETRY_MAX_DELAY_MS 60000
#define PORTAL_RETRY_MS    30000   /* khi AP đang bật: thử lại mạng đã lưu tối đa 30 s/lần */
#define PORTAL_PAUSE_MS    10000   /* có điện thoại đang nối AP: tạm dừng tự thử lại */

/* Công suất phát tối đa (dBm). Nhiều board SuperMini có anten gốm chưa phối hợp trở kháng tốt:
 * phát công suất cao (20 dBm mặc định) bị méo, điện thoại không thấy AP, đồng thời dòng đỉnh lớn
 * làm sụt áp 3V3 (gây lỗi I2C). 8.5 dBm thường ổn định hơn mà vẫn đủ trong phòng.
 * Đặt trong app_config.h nếu muốn đổi: 8 (yếu) .. 20 (mạnh nhất). */
#ifndef WIFI_MAX_TX_POWER_DBM
#define WIFI_MAX_TX_POWER_DBM 8.5
#endif

static SemaphoreHandle_t s_lock;
static net_wifi_status_t s_st;
static net_wifi_cb_t s_cb;
static void *s_cb_arg;

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_tick_timer;

static char s_try_ssid[33], s_try_pass[65];     /* mạng đang thử (từ web) */
static char s_saved_pass[65];                   /* mật khẩu mạng đã lưu */
static bool s_trying_new;
static bool s_user_disconnect;
static bool s_expect_leave;
static bool s_started;
static int  s_retry;
static volatile bool s_force_retry;   /* "Kết nối lại" từ web: bỏ qua việc tạm dừng khi có người dùng AP */
static volatile bool s_scanning;
static uint32_t s_portal_idle_s;
static char s_captive_uri[32];

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void notify(void)
{
    if (s_cb) s_cb(s_cb_arg);
}

const char *net_wifi_state_str(net_wifi_state_t st)
{
    switch (st) {
    case NET_WIFI_CONNECTING:   return "connecting";
    case NET_WIFI_CONNECTED:    return "connected";
    case NET_WIFI_FAILED:       return "failed";
    case NET_WIFI_DISCONNECTED: return "disconnected";
    default:                    return "not_configured";
    }
}

static const char *reason_to_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_EXPIRE:
        return "Sai mật khẩu WiFi";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "Không tìm thấy mạng (sai tên hoặc ở quá xa)";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "Kiểu bảo mật của mạng không được hỗ trợ";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
        return "Router từ chối kết nối";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "Mất tín hiệu router";
    default:
        return "Không kết nối được";
    }
}

static bool reason_is_auth(uint8_t reason)
{
    return reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_AUTH_EXPIRE;
}

/* ------------------------- NVS ------------------------- */
static bool nvs_load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_str(h, WIFI_NVS_KEY_SSID, ssid, &ssid_len) == ESP_OK && ssid[0];
    if (ok && nvs_get_str(h, WIFI_NVS_KEY_PASS, pass, &pass_len) != ESP_OK) {
        pass[0] = '\0';
    }
    nvs_close(h);
    return ok;
}

static esp_err_t nvs_save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, WIFI_NVS_KEY_PASS, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void nvs_erase_creds(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ------------------------- STA ------------------------- */
static esp_err_t apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass ? pass : "", sizeof(cfg.sta.password));
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* chấp nhận mọi kiểu bảo mật router hỗ trợ */
    cfg.sta.pmf_cfg.capable = true;
    cfg.sta.pmf_cfg.required = false;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    return esp_wifi_set_config(WIFI_IF_STA, &cfg);
}

static void schedule_retry(uint32_t delay_ms)
{
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
}

static uint32_t backoff_ms(int retry)
{
    uint32_t d = 2000;
    for (int i = 1; i < retry && d < RETRY_MAX_DELAY_MS; i++) d *= 2;
    if (d > RETRY_MAX_DELAY_MS) d = RETRY_MAX_DELAY_MS;
    /* Đang bật AP: mỗi lần thử kết nối, ESP32 quét qua mọi kênh WiFi (2-3 s) và rời kênh của AP
     * -> AP "lúc thấy lúc không". Thử thưa hơn để AP ổn định. */
    if (s_st.portal_active && d < PORTAL_RETRY_MS) d = PORTAL_RETRY_MS;
    return d;
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (s_user_disconnect) return;
    if (s_scanning) {
        schedule_retry(2000);
        return;
    }
    /* Có điện thoại đang vào trang cấu hình: không tự thử lại mạng cũ (việc quét kênh làm điện thoại
     * rớt khỏi AP). Người dùng tự bấm Kết nối / Kết nối lại trên web thì vẫn chạy (s_trying_new). */
    if (s_st.portal_active && s_st.ap_clients > 0 && !s_trying_new && !s_force_retry) {
        schedule_retry(PORTAL_PAUSE_MS);
        return;
    }
    s_force_retry = false;
    LOCK();
    bool has_target = s_trying_new || s_st.has_saved;
    if (has_target) {
        s_st.state = NET_WIFI_CONNECTING;
        strlcpy(s_st.ssid, s_trying_new ? s_try_ssid : s_st.saved_ssid, sizeof(s_st.ssid));
    }
    UNLOCK();
    if (has_target) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect lỗi: %s", esp_err_to_name(err));
            schedule_retry(3000);
        }
        notify();
    }
}

static void update_ap_clients(void)
{
    wifi_sta_list_t list;
    uint8_t n = 0;
    if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) n = (uint8_t)list.num;
    LOCK();
    s_st.ap_clients = n;
    UNLOCK();
}

/* Tick 1 giây: cập nhật RSSI, tự tắt AP khi không dùng */
static void tick_timer_cb(void *arg)
{
    (void)arg;
    static uint32_t sec;
    sec++;
    if (s_st.state == NET_WIFI_CONNECTED && (sec % 10 == 0)) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            LOCK();
            s_st.rssi = ap.rssi;
            UNLOCK();
        }
    }
#if WIFI_PORTAL_AUTO_STOP_S > 0
    if (s_st.portal_active && s_st.state == NET_WIFI_CONNECTED && s_st.ap_clients == 0) {
        if (++s_portal_idle_s >= WIFI_PORTAL_AUTO_STOP_S) {
            ESP_LOGI(TAG, "Đã có mạng và không ai dùng AP -> tắt AP cấu hình");
            net_wifi_portal_stop();
        }
    } else {
        s_portal_idle_s = 0;
    }
#endif
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        s_started = true;
        if (s_trying_new || s_st.has_saved) {
            retry_timer_cb(NULL);
        }
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        uint8_t reason = ev->reason;
        if (s_expect_leave && reason == WIFI_REASON_ASSOC_LEAVE) {
            s_expect_leave = false;   /* do chính ta ngắt để đổi mạng */
            break;
        }
        ESP_LOGW(TAG, "Mất kết nối '%s', lý do %u", s_st.ssid, reason);
        LOCK();
        s_st.ip[0] = '\0';
        s_st.gateway[0] = '\0';
        if (s_user_disconnect) {
            s_st.state = s_st.has_saved ? NET_WIFI_DISCONNECTED : NET_WIFI_NOT_CONFIGURED;
            UNLOCK();
            break;
        }
        bool start_portal = false;
        if (s_trying_new) {
            s_retry++;
            bool give_up = s_retry >= WIFI_STA_MAX_RETRY || (reason_is_auth(reason) && s_retry >= 2);
            if (!give_up) {
                s_st.state = NET_WIFI_CONNECTING;
                UNLOCK();
                schedule_retry(1000);
                break;
            }
            ESP_LOGW(TAG, "Kết nối '%s' thất bại: %s", s_try_ssid, reason_to_str(reason));
            s_trying_new = false;
            s_st.attempt_result = NET_ATTEMPT_FAILED;
            strlcpy(s_st.attempt_error, reason_to_str(reason), sizeof(s_st.attempt_error));
            s_st.state = NET_WIFI_FAILED;
            s_retry = 0;
            if (s_st.has_saved) {
                /* quay lại mạng cũ đã lưu */
                apply_sta_config(s_st.saved_ssid, s_saved_pass);
                strlcpy(s_st.ssid, s_st.saved_ssid, sizeof(s_st.ssid));
                UNLOCK();
                schedule_retry(5000);
            } else {
                UNLOCK();
            }
        } else if (s_st.has_saved) {
            s_retry++;
            s_st.state = s_retry >= WIFI_STA_MAX_RETRY ? NET_WIFI_FAILED : NET_WIFI_CONNECTING;
            start_portal = (s_retry == WIFI_STA_MAX_RETRY) && !s_st.portal_active;
            uint32_t d = backoff_ms(s_retry);
            UNLOCK();
            schedule_retry(d);
        } else {
            s_st.state = NET_WIFI_NOT_CONFIGURED;
            UNLOCK();
        }
        if (start_portal) {
            ESP_LOGW(TAG, "Không kết nối được mạng đã lưu -> bật AP cấu hình");
            net_wifi_portal_start();
        }
        break;
    }

    case WIFI_EVENT_AP_STACONNECTED:
    case WIFI_EVENT_AP_STADISCONNECTED:
        update_ap_clients();
        ESP_LOGI(TAG, "AP: %u thiết bị đang kết nối", s_st.ap_clients);
        break;

    default:
        return;
    }
    notify();
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    wifi_ap_record_t ap = {0};
    esp_wifi_sta_get_ap_info(&ap);

    LOCK();
    s_expect_leave = false;
    s_retry = 0;
    s_st.state = NET_WIFI_CONNECTED;
    snprintf(s_st.ip, sizeof(s_st.ip), IPSTR, IP2STR(&ev->ip_info.ip));
    snprintf(s_st.gateway, sizeof(s_st.gateway), IPSTR, IP2STR(&ev->ip_info.gw));
    s_st.rssi = ap.rssi;
    s_st.channel = ap.primary;
    if (s_trying_new) {
        s_trying_new = false;
        strlcpy(s_st.saved_ssid, s_try_ssid, sizeof(s_st.saved_ssid));
        strlcpy(s_saved_pass, s_try_pass, sizeof(s_saved_pass));
        s_st.has_saved = true;
        s_st.attempt_result = NET_ATTEMPT_SUCCESS;
        s_st.attempt_error[0] = '\0';
        if (nvs_save_creds(s_try_ssid, s_try_pass) != ESP_OK) {
            ESP_LOGE(TAG, "Không lưu được cấu hình WiFi vào NVS");
        }
    }
    strlcpy(s_st.ssid, s_st.saved_ssid, sizeof(s_st.ssid));
    UNLOCK();

    ESP_LOGI(TAG, "Đã kết nối '%s', IP %s, RSSI %d", s_st.ssid, s_st.ip, s_st.rssi);
    notify();
}

/* ------------------------- AP / portal ------------------------- */
static esp_err_t configure_ap(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_st.ap_ssid, sizeof(s_st.ap_ssid), "%s-%02X%02X", WIFI_AP_SSID_PREFIX, mac[4], mac[5]);

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.ap.ssid, s_st.ap_ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(s_st.ap_ssid);
    cfg.ap.channel = WIFI_AP_CHANNEL;
    cfg.ap.max_connection = WIFI_AP_MAX_CONN;
    if (strlen(WIFI_AP_PASSWORD) >= 8) {
        strlcpy((char *)cfg.ap.password, WIFI_AP_PASSWORD, sizeof(cfg.ap.password));
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    cfg.ap.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_AP, &cfg);
}

static void set_dhcp_captive_uri(void)
{
    /* DHCP option 114: Android 11+/iOS 14+ tự mở trang cấu hình */
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ip) != ESP_OK) return;
    snprintf(s_captive_uri, sizeof(s_captive_uri), "http://" IPSTR "/", IP2STR(&ip.ip));
    snprintf(s_st.ap_ip, sizeof(s_st.ap_ip), IPSTR, IP2STR(&ip.ip));
    esp_netif_dhcps_stop(s_ap_netif);
    esp_err_t err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                           s_captive_uri, strlen(s_captive_uri));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Không đặt được DHCP captive URI: %s", esp_err_to_name(err));
    }
    esp_netif_dhcps_start(s_ap_netif);
}

esp_err_t net_wifi_portal_start(void)
{
    if (s_st.portal_active) return ESP_OK;
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Không bật được AP: %s", esp_err_to_name(err));
        return err;
    }
    configure_ap();
    esp_wifi_set_ps(WIFI_PS_NONE);          /* AP cần phát beacon đều: tắt tiết kiệm năng lượng */
    set_dhcp_captive_uri();
    err = wifi_portal_start(s_st.ap_ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Không chạy được web cấu hình: %s", esp_err_to_name(err));
    }
    LOCK();
    s_st.portal_active = true;
    s_st.ap_clients = 0;
    UNLOCK();
    s_portal_idle_s = 0;
    ESP_LOGI(TAG, "AP cấu hình '%s' đã bật, truy cập http://%s", s_st.ap_ssid, s_st.ap_ip);
    notify();
    return ESP_OK;
}

esp_err_t net_wifi_portal_stop(void)
{
    if (!s_st.portal_active) return ESP_OK;
    wifi_portal_stop();
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    LOCK();
    s_st.portal_active = false;
    s_st.ap_clients = 0;
    UNLOCK();
    ESP_LOGI(TAG, "Đã tắt AP cấu hình");
    notify();
    return err;
}

bool net_wifi_portal_active(void)
{
    return s_st.portal_active;
}

/* ------------------------- API ------------------------- */
esp_err_t net_wifi_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    memset(&s_st, 0, sizeof(s_st));

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));   /* tự quản lý lưu NVS */

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL));

    const esp_timer_create_args_t retry_args = { .callback = retry_timer_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&retry_args, &s_retry_timer));
    const esp_timer_create_args_t tick_args = { .callback = tick_timer_cb, .name = "wifi_tick" };
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &s_tick_timer));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_st.mac, sizeof(s_st.mac), MACSTR, MAC2STR(mac));

    s_st.has_saved = nvs_load_creds(s_st.saved_ssid, sizeof(s_st.saved_ssid), s_saved_pass, sizeof(s_saved_pass));
    if (s_st.has_saved) {
        ESP_LOGI(TAG, "Mạng đã lưu: '%s'", s_st.saved_ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        apply_sta_config(s_st.saved_ssid, s_saved_pass);
        strlcpy(s_st.ssid, s_st.saved_ssid, sizeof(s_st.ssid));
        s_st.state = NET_WIFI_CONNECTING;
    } else {
        ESP_LOGI(TAG, "Chưa có mạng WiFi -> bật AP cấu hình");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        s_st.state = NET_WIFI_NOT_CONFIGURED;
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_err_t pe = esp_wifi_set_max_tx_power((int8_t)(WIFI_MAX_TX_POWER_DBM * 4));   /* đơn vị 0.25 dBm */
    int8_t q = 0;
    esp_wifi_get_max_tx_power(&q);
    ESP_LOGI(TAG, "Công suất phát WiFi tối đa %.2f dBm%s", q / 4.0, pe == ESP_OK ? "" : " (không đặt được)");
    esp_timer_start_periodic(s_tick_timer, 1000 * 1000);

    if (!s_st.has_saved) {
        net_wifi_portal_start();
    }
    return ESP_OK;
}

void net_wifi_set_callback(net_wifi_cb_t cb, void *arg)
{
    s_cb = cb;
    s_cb_arg = arg;
}

esp_err_t net_wifi_connect(const char *ssid, const char *password, uint32_t *attempt_id)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32) return ESP_ERR_INVALID_ARG;
    if (password && strlen(password) > 64) return ESP_ERR_INVALID_ARG;
    if (password && password[0] && strlen(password) < 8) return ESP_ERR_INVALID_SIZE;

    esp_timer_stop(s_retry_timer);
    LOCK();
    bool was_active = s_st.state == NET_WIFI_CONNECTED || s_st.state == NET_WIFI_CONNECTING ||
                      (s_st.state == NET_WIFI_FAILED && s_st.has_saved);
    strlcpy(s_try_ssid, ssid, sizeof(s_try_ssid));
    strlcpy(s_try_pass, password ? password : "", sizeof(s_try_pass));
    s_trying_new = true;
    s_user_disconnect = false;
    s_retry = 0;
    s_st.attempt_id++;
    s_st.attempt_result = NET_ATTEMPT_PENDING;
    strlcpy(s_st.attempt_ssid, ssid, sizeof(s_st.attempt_ssid));
    s_st.attempt_error[0] = '\0';
    s_st.state = NET_WIFI_CONNECTING;
    strlcpy(s_st.ssid, ssid, sizeof(s_st.ssid));
    s_st.ip[0] = '\0';
    if (attempt_id) *attempt_id = s_st.attempt_id;
    UNLOCK();

    ESP_LOGI(TAG, "Thử kết nối mạng mới '%s'", ssid);
    if (was_active) {
        s_expect_leave = true;
        esp_wifi_disconnect();
    }
    esp_err_t err = apply_sta_config(s_try_ssid, s_try_pass);
    if (err != ESP_OK) return err;
    schedule_retry(was_active ? 500 : 50);   /* kết nối trong timer, không chặn web */
    notify();
    return ESP_OK;
}

esp_err_t net_wifi_disconnect(bool forget)
{
    esp_timer_stop(s_retry_timer);
    LOCK();
    s_user_disconnect = true;
    s_trying_new = false;
    s_retry = 0;
    if (forget) {
        s_st.has_saved = false;
        s_st.saved_ssid[0] = '\0';
        s_saved_pass[0] = '\0';
    }
    s_st.state = s_st.has_saved ? NET_WIFI_DISCONNECTED : NET_WIFI_NOT_CONFIGURED;
    s_st.ip[0] = '\0';
    s_st.gateway[0] = '\0';
    if (forget) s_st.ssid[0] = '\0';
    UNLOCK();

    esp_wifi_disconnect();
    if (forget) {
        nvs_erase_creds();
        wifi_config_t empty = {0};
        esp_wifi_set_config(WIFI_IF_STA, &empty);
    }
    ESP_LOGI(TAG, "Đã ngắt kết nối%s", forget ? " và xoá mạng đã lưu" : "");
    notify();
    return ESP_OK;
}

esp_err_t net_wifi_reconnect(void)
{
    LOCK();
    if (!s_st.has_saved) {
        UNLOCK();
        return ESP_ERR_NOT_FOUND;
    }
    s_user_disconnect = false;
    s_trying_new = false;
    s_retry = 0;
    s_st.state = NET_WIFI_CONNECTING;
    strlcpy(s_st.ssid, s_st.saved_ssid, sizeof(s_st.ssid));
    UNLOCK();
    apply_sta_config(s_st.saved_ssid, s_saved_pass);
    s_force_retry = true;
    schedule_retry(50);
    ESP_LOGI(TAG, "Kết nối lại mạng đã lưu '%s'", s_st.saved_ssid);
    notify();
    return ESP_OK;
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const net_wifi_ap_info_t *)b)->rssi - ((const net_wifi_ap_info_t *)a)->rssi;
}

esp_err_t net_wifi_scan(net_wifi_ap_info_t *list, uint16_t max, uint16_t *count)
{
    if (!list || !count) return ESP_ERR_INVALID_ARG;
    *count = 0;
    if (!s_started) return ESP_ERR_INVALID_STATE;

    wifi_scan_config_t sc = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = { .min = 80, .max = 150 },
    };
    s_scanning = true;
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    s_scanning = false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Quét WiFi lỗi: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t n = SCAN_MAX_RECORDS;
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&n, recs);
    if (err != ESP_OK) {
        free(recs);
        return err;
    }

    uint16_t out = 0;
    for (uint16_t i = 0; i < n; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0]) continue;
        int found = -1;
        for (uint16_t j = 0; j < out; j++) {
            if (strcmp(list[j].ssid, ssid) == 0) { found = j; break; }
        }
        if (found >= 0) {
            if (recs[i].rssi > list[found].rssi) {
                list[found].rssi = recs[i].rssi;
                list[found].channel = recs[i].primary;
            }
            continue;
        }
        if (out >= max) continue;
        strlcpy(list[out].ssid, ssid, sizeof(list[out].ssid));
        list[out].rssi = recs[i].rssi;
        list[out].channel = recs[i].primary;
        list[out].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        out++;
    }
    free(recs);
    qsort(list, out, sizeof(list[0]), cmp_rssi);
    *count = out;
    return ESP_OK;
}

void net_wifi_get_status(net_wifi_status_t *out)
{
    if (!out) return;
    LOCK();
    *out = s_st;
    UNLOCK();
}

bool net_wifi_is_connected(void)
{
    return s_st.state == NET_WIFI_CONNECTED;
}