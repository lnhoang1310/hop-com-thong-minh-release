/*
 * cloud_manager.c - MQTT client (esp-mqtt), gói tin xem cloud_protocol.h
 */
#include "cloud_manager.h"
#include "cloud_protocol.h"
#include "net_wifi.h"
#include "app_config.h"
#include "mqtt_client.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>
#include "esp_tls_errors.h"
#include "esp_crt_bundle.h"
#include "mqtt_setting.h"

/* Dùng MQTT 5 nếu đã bật trong menuconfig (ESP-MQTT Configurations -> Enable MQTT protocol 5.0),
 * nếu không thì tự dùng MQTT 3.1.1. Gói tin giống hệt nhau ở cả hai phiên bản. */

static const char *TAG = "CLOUD";

#define TOPIC_LEN 80

/* Client chỉ được tạo / khởi động / huỷ trong task "cloud_tx" để không tranh chấp */
static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static bool s_started;
static mqtt_settings_t s_set;            /* cấu hình broker đang dùng (NVS / app_config.h) */
static char s_uri[96];
static char s_broker_text[72];           /* "host:port" cho OLED và web */
static bool s_sntp_started;
static uint32_t s_interval_s = TELEMETRY_INTERVAL_S;

static cloud_cmd_cb_t s_cmd_cb;
static void *s_cb_arg;

static char s_device_id[20];
static char t_status[TOPIC_LEN], t_telemetry[TOPIC_LEN], t_cmd[TOPIC_LEN];
static const char *s_will_msg = "{\"state\":\"offline\"}";
static char s_status[24] = "Cho WiFi";

static void set_status(const char *txt)
{
    strlcpy(s_status, txt, sizeof(s_status));
}

const char *cloud_manager_status_text(void)
{
    return s_connected ? "Da ket noi" : s_status;
}

/* Giải thích lỗi kết nối broker: in log chi tiết + đặt dòng ngắn cho OLED */
static void explain_error(const esp_mqtt_error_codes_t *e)
{
    if (!e) return;
    if (e->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        int en = e->esp_transport_sock_errno;
        esp_err_t tls = e->esp_tls_last_esp_err;
        if (e->esp_tls_cert_verify_flags) {
            set_status("Loi chung chi TLS");
            ESP_LOGE(TAG, "Chứng chỉ của %s không hợp lệ (flags 0x%x). Broker tự ký -> dán CA vào web cấu hình; "
                     "broker công cộng -> để trống ô CA", s_uri, e->esp_tls_cert_verify_flags);
        } else if (tls == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED || tls == ESP_ERR_MBEDTLS_SSL_SETUP_FAILED) {
            set_status("Loi TLS (sai cong?)");
            ESP_LOGE(TAG, "Bắt tay TLS với %s thất bại (%s). Kiểm tra cổng TLS (thường 8883) và broker có bật TLS",
                     s_uri, esp_err_to_name(tls));
        } else if (en == ECONNREFUSED) {
            set_status("Broker chua chay");
            ESP_LOGE(TAG, "Máy %s có trả lời nhưng cổng bị từ chối (ECONNREFUSED). Kiểm tra: broker đã chạy chưa; "
                     "Mosquitto 2.x mặc định CHỈ nghe localhost -> cần 'listener 1883' + 'allow_anonymous true'", s_uri);
        } else if (tls == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
            set_status("Sai ten may chu");
            ESP_LOGE(TAG, "Không phân giải được tên máy trong %s", s_uri);
        } else if (en == EHOSTUNREACH || en == ETIMEDOUT || en == ENETUNREACH ||
                   tls == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT || tls == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST) {
            set_status("IP sai/tuong lua");
            ESP_LOGE(TAG, "Không tới được %s (errno=%d, %s). Kiểm tra: đúng IP máy tính (ipconfig)? cùng mạng WiFi? "
                     "Windows Firewall đã mở cổng 1883? Router có bật 'AP isolation' không?",
                     s_uri, en, esp_err_to_name(tls));
        } else {
            char buf[24];
            snprintf(buf, sizeof(buf), "Loi mang %d", en);
            set_status(buf);
            ESP_LOGE(TAG, "Lỗi mạng tới %s: errno=%d, esp-tls=%s", s_uri, en, esp_err_to_name(tls));
        }
    } else if (e->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        int rc = e->connect_return_code;
        if (rc == 0x84 || rc == MQTT_CONNECTION_REFUSE_PROTOCOL) {
            set_status("Broker k co MQTT5");
            ESP_LOGE(TAG, "Broker không hỗ trợ MQTT 5 (reason 0x%02x) -> dùng Mosquitto >= 1.6", rc);
        } else if (rc == 0x86 || rc == 0x87 || rc == MQTT_CONNECTION_REFUSE_BAD_USERNAME ||
                   rc == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) {
            set_status("Sai user/mat khau");
            ESP_LOGE(TAG, "Broker từ chối đăng nhập (reason 0x%02x) -> kiểm tra tên/mật khẩu trên web cấu hình "
                     "hoặc bật allow_anonymous", rc);
        } else {
            char buf[24];
            snprintf(buf, sizeof(buf), "Broker tu choi %02x", rc);
            set_status(buf);
            ESP_LOGE(TAG, "Broker từ chối kết nối, reason 0x%02x", rc);
        }
    }
}

const char *cloud_manager_broker_text(void)
{
    return s_broker_text;
}

const char *cloud_manager_device_id(void)
{
    return s_device_id;
}

bool cloud_manager_is_online(void)
{
    return s_connected;
}

void cloud_manager_set_interval(uint32_t interval_s)
{
    s_interval_s = interval_s;
}

uint32_t cloud_manager_get_interval(void)
{
    return s_interval_s;
}

int64_t cloud_manager_unix_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < 1700000000) return 0;   /* chưa đồng bộ SNTP */
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/*
 * GỬI BẢN TIN
 * Mọi bản tin đi qua hàng đợi s_tx_queue và CHỈ task "cloud_tx" gọi esp-mqtt để gửi:
 * task MQTT gọi mqtt_event_handler trong lúc giữ khoá nội bộ của esp-mqtt, gửi từ nhiều
 * task cùng lúc dễ gây khoá chéo (deadlock).
 */
typedef enum {
    TX_STATUS_ONLINE = 0,   /* json do task tx tự tạo */
    TX_TELEMETRY,
    TX_START,               /* có IP -> khởi động client */
    TX_RECONFIG,            /* cấu hình broker vừa đổi -> huỷ client cũ, tạo lại */
} tx_kind_t;

typedef struct {
    tx_kind_t kind;
    char     *json;         /* cấp phát động, task tx free() */
} tx_item_t;

#define TX_QUEUE_LEN 8
static QueueHandle_t s_tx_queue;

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data);

static void build_topics(void)
{
#define MK(buf, suffix) snprintf(buf, sizeof(buf), "%s/%s", s_set.root, suffix)
    MK(t_status, "status");
    MK(t_telemetry, "telemetry");
    MK(t_cmd, "cmd");
#undef MK
}

static void client_destroy(void)
{
    if (!s_client) return;
    if (s_started) esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_started = false;
    s_connected = false;
}

static void client_create(void)
{
    mqtt_settings_free(&s_set);
    mqtt_settings_load(&s_set);
    mqtt_settings_uri(&s_set, s_uri, sizeof(s_uri));
    snprintf(s_broker_text, sizeof(s_broker_text), "%s:%u", s_set.host, s_set.port);
    build_topics();
    const char *bad = mqtt_settings_validate(&s_set);
    if (bad) {
        ESP_LOGE(TAG, "Cấu hình broker lỗi: %s", bad);
        set_status("Cau hinh broker loi");
        return;
    }

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_uri,
        .credentials.client_id = s_device_id,
        .credentials.username = s_set.user[0] ? s_set.user : NULL,
        .credentials.authentication.password = s_set.pass[0] ? s_set.pass : NULL,
#if CONFIG_MQTT_PROTOCOL_5
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
#else
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
#endif
        .session.keepalive = MQTT_KEEPALIVE_S,
        .session.last_will = {
            .topic = t_status,
            .msg = s_will_msg,
            .msg_len = (int)strlen(s_will_msg),
            .qos = 1,
            .retain = 1,
        },
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 10000,
        .buffer.size = 2048,
        .task.stack_size = 6144,
    };
    if (s_set.tls) {
        if (s_set.ca && s_set.ca[0]) {
            cfg.broker.verification.certificate = s_set.ca;           /* CA tự dán (broker tự ký) */
        } else {
            cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;   /* CA công cộng */
        }
    }
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Không tạo được MQTT client");
        set_status("Loi tao client");
        return;
    }
#if CONFIG_MQTT_PROTOCOL_5
    esp_mqtt5_connection_property_config_t conn = {
        .will_delay_interval = 10,   /* rớt mạng ngắn < 10 s thì chưa báo offline */
    };
    esp_mqtt5_client_set_connect_property(s_client, &conn);
#endif
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    ESP_LOGI(TAG, "MQTT: %s%s, client %s, topic %s/...", s_uri,
             s_set.tls ? ((s_set.ca && s_set.ca[0]) ? " (TLS, CA tự dán)" : " (TLS, CA có sẵn)") : "",
             s_device_id, s_set.root);
}

static void client_start(void)
{
    if (!s_client || s_started || !net_wifi_is_connected()) return;
    set_status("Dang ket noi...");
    ESP_LOGI(TAG, "Kết nối broker %s", s_uri);
    if (esp_mqtt_client_start(s_client) == ESP_OK) {
        s_started = true;   /* sau đó esp-mqtt tự kết nối lại */
    }
}

static void tx_task(void *arg)
{
    (void)arg;
    tx_item_t it;
    client_create();
    while (1) {
        if (xQueueReceive(s_tx_queue, &it, portMAX_DELAY) != pdTRUE) continue;
        if (it.kind == TX_START) {
            client_start();
            continue;
        }
        if (it.kind == TX_RECONFIG) {
            ESP_LOGI(TAG, "Áp dụng cấu hình broker mới");
            client_destroy();
            set_status("Cho WiFi");
            client_create();
            client_start();
            continue;
        }
        if (it.kind == TX_STATUS_ONLINE) {
            net_wifi_status_t ws;
            net_wifi_get_status(&ws);
            it.json = cloud_protocol_build_status(ws.ip, ws.rssi);
        }
        if (s_connected && s_client && it.json) {
            bool status = it.kind == TX_STATUS_ONLINE;
            int id = esp_mqtt_client_enqueue(s_client, status ? t_status : t_telemetry, it.json, 0, 1,
                                             status ? 1 : 0, true);
            if (id < 0) ESP_LOGW(TAG, "Không gửi được bản tin (%d)", id);
        }
        free(it.json);
    }
}

/* Đưa bản tin vào hàng đợi (không chặn). json được giao quyền sở hữu cho task tx. */
static esp_err_t tx_post(tx_kind_t kind, char *json)
{
    if (!s_tx_queue) {
        free(json);
        return ESP_ERR_INVALID_STATE;
    }
    tx_item_t it = { .kind = kind, .json = json };
    if (xQueueSend(s_tx_queue, &it, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Hàng đợi gửi đầy, bỏ bản tin");
        free(json);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t cloud_manager_apply_settings(void)
{
    return tx_post(TX_RECONFIG, NULL);
}

esp_err_t cloud_manager_publish_telemetry(const food_sensor_data_t *d, const food_session_t *s,
                                          const food_safety_t *r)
{
    if (!s_connected || !d || !s || !r) return ESP_ERR_INVALID_STATE;
    char *json = cloud_protocol_build_telemetry(d, s, r);
    if (!json) return ESP_ERR_NO_MEM;
    return tx_post(TX_TELEMETRY, json);
}

static bool topic_is(const esp_mqtt_event_handle_t ev, const char *topic)
{
    size_t n = strlen(topic);
    return ev->topic_len == (int)n && strncmp(ev->topic, topic, n) == 0;
}

static void handle_data(esp_mqtt_event_handle_t ev)
{
    if (ev->total_data_len != ev->data_len || ev->current_data_offset != 0) {
        ESP_LOGW(TAG, "Bỏ bản tin bị chia nhỏ (%d byte)", ev->total_data_len);
        return;
    }

    if (topic_is(ev, t_cmd)) {
        cloud_cmd_t cmd;
        esp_err_t err = cloud_protocol_parse_cmd(ev->data, ev->data_len, &cmd);
        if (err != ESP_OK || cmd.type == CLOUD_CMD_UNKNOWN) {
            ESP_LOGW(TAG, "Lệnh không hợp lệ, bỏ qua: %.*s", ev->data_len, ev->data);
            return;
        }
        ESP_LOGI(TAG, "Lệnh: %.*s", ev->data_len, ev->data);
        if (s_cmd_cb) s_cmd_cb(&cmd, s_cb_arg);
    }
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "Đã kết nối broker %s", s_uri);
        set_status("Da ket noi");
        esp_mqtt_client_subscribe_single(s_client, t_cmd, 1);
        tx_post(TX_STATUS_ONLINE, NULL);
        break;
    case MQTT_EVENT_DISCONNECTED:
        if (s_connected) {
            ESP_LOGW(TAG, "Mất kết nối broker");
            set_status("Mat ket noi");
        }
        s_connected = false;
        break;
    case MQTT_EVENT_DATA:
        handle_data(ev);
        break;
    case MQTT_EVENT_ERROR:
        explain_error(ev->error_handle);
        break;
    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    if (!s_sntp_started) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&cfg) == ESP_OK) s_sntp_started = true;
    }
    tx_post(TX_START, NULL);   /* task cloud_tx khởi động client */
}

esp_err_t cloud_manager_init(cloud_cmd_cb_t cmd_cb, void *arg)
{
    s_cmd_cb = cmd_cb;
    s_cb_arg = arg;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "hc-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    s_tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(tx_item_t));
    if (!s_tx_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(tx_task, "cloud_tx", 6144, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL);

    if (net_wifi_is_connected()) {
        on_got_ip(NULL, NULL, 0, NULL);
    }
    return ESP_OK;
}