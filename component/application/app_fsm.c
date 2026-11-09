/*
 * app_fsm.c - Máy trạng thái chính
 * Một task duy nhất sở hữu màn hình OLED và phiên đo; mọi module khác gửi sự kiện qua hàng đợi.
 */
#include "app_fsm.h"
#include "app_config.h"
#include "food_eval.h"
#include "button.h"
#include "sensor_manager.h"
#include "net_wifi.h"
#include "cloud_manager.h"
#include "ota.h"
#include "oled_sh1106.h"
#include "ui_display.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "FSM";

#define FSM_QUEUE_LEN        16
#define BOOT_SCREEN_MS       1500
#define TOAST_MS             2000
#define LOG_SUMMARY_MS       5000

typedef enum {
    EVT_SENSOR = 0,
    EVT_BUTTON,
    EVT_WIFI,
    EVT_CMD,
} evt_type_t;

typedef struct {
    evt_type_t type;
    union {
        struct {
            button_id_t id;
            button_event_t kind;
        } btn;
        cloud_cmd_t cmd;
    };
} app_event_t;

static QueueHandle_t s_queue;
static app_state_t s_state = APP_STATE_BOOT;
static uint32_t s_state_enter_ms;
static ui_model_t s_ui;
static food_safety_t s_safety;
static uint32_t s_toast_until;
static uint32_t s_last_publish;
static uint32_t s_last_log;
static uint32_t s_interval_ms = TELEMETRY_INTERVAL_S * 1000;
static uint8_t s_menu_index;             /* dòng đang chọn trong menu món */
static bool s_menu_first;                /* menu lúc khởi động: chọn xong thì bắt đầu phiên mới */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const char *state_name(app_state_t s)
{
    switch (s) {
    case APP_STATE_BOOT:      return "BOOT";
    case APP_STATE_WARMUP:    return "WARMUP";
    case APP_STATE_MONITOR:   return "MONITOR";
    case APP_STATE_CALIBRATE: return "CALIBRATE";
    case APP_STATE_SELECT_FOOD: return "SELECT_FOOD";
    default:                  return "?";
    }
}

static void enter_state(app_state_t st)
{
    if (st == s_state) return;
    ESP_LOGI(TAG, "%s -> %s", state_name(s_state), state_name(st));
    s_state = st;
    s_state_enter_ms = now_ms();
}

static void toast(const char *msg)
{
    strlcpy(s_ui.toast, msg, sizeof(s_ui.toast));
    s_toast_until = now_ms() + TOAST_MS;
}

/* ------------------- callback từ các module (chạy ở task khác) ------------------- */
static void on_sensor_update(const food_sensor_data_t *d, void *arg)
{
    (void)d;
    (void)arg;
    app_event_t e = { .type = EVT_SENSOR };
    xQueueSend(s_queue, &e, 0);
}

static void on_button(button_id_t id, button_event_t kind, void *arg)
{
    (void)arg;
    static const char *const names[] = { "UP", "DOWN", "OK" };
    static const char *const kinds[] = { "nhan", "giu 3s", "UP+DOWN 5s" };
    ESP_LOGI(TAG, "Nút %s: %s", names[id], kinds[kind]);
    app_event_t e = { .type = EVT_BUTTON, .btn = { .id = id, .kind = kind } };
    xQueueSend(s_queue, &e, 0);
}

static void on_wifi_change(void *arg)
{
    (void)arg;
    app_event_t e = { .type = EVT_WIFI };
    xQueueSend(s_queue, &e, 0);
}

static void on_cloud_cmd(const cloud_cmd_t *cmd, void *arg)
{
    (void)arg;
    app_event_t e = { .type = EVT_CMD };
    e.cmd = *cmd;
    if (xQueueSend(s_queue, &e, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Hàng đợi đầy, bỏ lệnh từ server");
    }
}

/* ------------------- đánh giá + cập nhật kết quả ------------------- */
/* Trả về true nếu mức kết luận vừa đổi */
static bool apply_result(const food_safety_t *r)
{
    food_level_t prev = s_safety.level;
    s_safety = *r;
    if (s_state != APP_STATE_MONITOR) return false;
    if (prev != s_safety.level) {
        /* Không có LED/còi: khi mức tăng thì nhảy về trang Tổng quan và báo trên OLED */
        if (s_safety.level > prev && s_safety.level >= FOOD_LEVEL_WARNING) {
            s_ui.page = UI_PAGE_OVERVIEW;
            toast(s_safety.level == FOOD_LEVEL_DANGER ? "!! BIEN DOI MANH !!" : "! CAN KIEM TRA !");
        }
        ESP_LOGI(TAG, "Mức: %s -> %s", food_level_to_code(prev), food_level_to_code(s_safety.level));
        return true;
    }
    return false;
}

static void evaluate(void)
{
    food_sensor_data_t d;
    sensor_manager_get_data(&d);
    food_eval_update(&d);

    uint32_t now = now_ms();
    food_safety_t r;
    food_eval_local(&d, &r);                 /* thiết bị tự đánh giá (rule-0.1, food_eval.c) */
    bool changed = apply_result(&r);

    /* Gửi số đo + kết quả lên server theo chu kỳ, và gửi ngay khi mức kết luận đổi */
    if (s_state == APP_STATE_MONITOR && (changed || now - s_last_publish >= s_interval_ms)) {
        s_last_publish = now;
        food_session_t s;
        food_eval_get_session(&s);
        cloud_manager_publish_telemetry(&d, &s, &s_safety);
    }

    if (now - s_last_log >= LOG_SUMMARY_MS) {
        s_last_log = now;
        ESP_LOGI(TAG, "T=%.1fC RH=%.0f%% CO2=%u NH3=%.3f H2S=%.3f VOC=%u -> %s S=%u %s",
                 d.temperature_c, d.humidity_rh, d.co2_ppm, d.nh3_rs_r0, d.h2s_rs_r0, d.voc_raw,
                 food_level_to_code(s_safety.level), s_safety.score, cloud_manager_is_online() ? "[online]" : "[offline]");
    }
}

static void start_new_session(food_group_t food)
{
    food_eval_new_session(food);
}

static void change_food(food_group_t food)
{
    food_eval_set_food(food);            /* giữ phiên + nền, chỉ đổi trọng số/ngưỡng */
}

/* Mở menu chọn món. first = true: menu lúc khởi động (chọn xong -> phiên mới, lấy nền) */
static void open_food_menu(bool first)
{
    food_session_t s;
    food_eval_get_session(&s);
    s_menu_index = (uint8_t)s.food;
    s_menu_first = first;
    enter_state(APP_STATE_SELECT_FOOD);
}

/* OK trong menu */
static void confirm_food_menu(void)
{
    food_group_t food = (food_group_t)s_menu_index;
    food_session_t s;
    food_eval_get_session(&s);
    char buf[28];
    if (s_menu_first) {
        start_new_session(food);
        snprintf(buf, sizeof(buf), "%s - lay mau nen", food_group_to_label(food));
    } else if (food != s.food) {
        change_food(food);
        snprintf(buf, sizeof(buf), "Doi mon: %s", food_group_to_label(food));
    } else {
        snprintf(buf, sizeof(buf), "Giu mon: %s", food_group_to_label(food));
    }
    ESP_LOGI(TAG, "Chọn món: %s (%s)", food_group_to_code(food), s_menu_first ? "phiên mới" : "đổi món");
    s_ui.page = UI_PAGE_OVERVIEW;
    enter_state(APP_STATE_MONITOR);
    toast(buf);
}

static bool start_calibration(void)
{
    if (sensor_manager_warmup_remaining_s() > 0) return false;
    if (sensor_manager_request_calibration() != ESP_OK) return false;
    enter_state(APP_STATE_CALIBRATE);
    return true;
}

/* ------------------- lệnh từ server ------------------- */
static void handle_cmd(const cloud_cmd_t *c)
{
    food_session_t s;
    food_eval_get_session(&s);
    switch (c->type) {
    case CLOUD_CMD_NEW_SESSION:
        start_new_session(c->has_food ? c->food : s.food);
        toast("Phien moi (server)");
        if (s_state == APP_STATE_SELECT_FOOD) enter_state(APP_STATE_MONITOR);
        break;
    case CLOUD_CMD_SET_FOOD:
        if (s_state == APP_STATE_SELECT_FOOD && s_menu_first) {
            start_new_session(c->food);          /* đang ở menu khởi động: coi như chọn món */
        } else {
            change_food(c->food);
        }
        if (s_state == APP_STATE_SELECT_FOOD) enter_state(APP_STATE_MONITOR);
        toast("Doi mon (server)");
        break;
    case CLOUD_CMD_SET_INTERVAL:
        s_interval_ms = c->interval_s * 1000;
        cloud_manager_set_interval(c->interval_s);
        break;
    case CLOUD_CMD_CALIBRATE_MQ:
        if (s_state != APP_STATE_MONITOR || !start_calibration()) {
            ESP_LOGW(TAG, "Chưa hiệu chuẩn được lúc này");
        }
        break;
    case CLOUD_CMD_REBOOT:
        food_eval_save();
        esp_restart();
        break;
    case CLOUD_CMD_OTA:
        food_eval_save();
        if (ota_update_start_url(c->url) != ESP_OK) {
            ESP_LOGW(TAG, "Không bắt đầu được OTA (đang cập nhật hoặc URL sai)");
        }
        break;
    default:
        break;
    }
}

/* ------------------- xử lý nút ------------------- */
/*
 * Menu chọn món:  UP / DOWN (nhấn) lên / xuống, OK (nhấn) chọn
 * Màn hình chính:
 *   UP / DOWN (nhấn)     : chuyển trang
 *   OK (nhấn)            : về trang Tổng quan
 *   UP (giữ 3s)          : mở lại menu chọn món (đổi món, giữ phiên + nền)
 *   DOWN (giữ 3s)        : "Hộp mới" - phiên đo mới, lấy lại nền
 *   UP + DOWN (giữ 5s)   : hiệu chuẩn MQ (không khí sạch)
 * Làm nóng: OK (nhấn) bỏ qua -> vào menu chọn món
 * Mọi lúc:  OK (giữ 3s) bật/tắt AP cấu hình WiFi
 */
static void handle_button(button_id_t id, button_event_t kind)
{
    if (kind == BTN_EVT_COMBO) {
        if (s_state != APP_STATE_MONITOR) {
            toast("Chua the hieu chuan");
        } else if (sensor_manager_warmup_remaining_s() > 0) {
            toast("Cho lam nong xong");
        } else if (!start_calibration()) {
            toast("Khong co cam bien MQ");
        }
        return;
    }

    if (kind == BTN_EVT_LONG) {
        switch (id) {
        case BTN_OK:
            if (net_wifi_portal_active()) {
                net_wifi_portal_stop();
                toast("Da tat AP cau hinh");
            } else {
                net_wifi_portal_start();
                toast("Da bat AP cau hinh");
                s_ui.page = UI_PAGE_NETWORK;
            }
            break;
        case BTN_UP:
            if (s_state == APP_STATE_MONITOR) open_food_menu(false);
            break;
        case BTN_DOWN: {
            if (s_state != APP_STATE_MONITOR) break;
            food_session_t s;
            food_eval_get_session(&s);
            start_new_session(s.food);
            toast("Hop moi - lay nen");
            break;
        }
        default:
            break;
        }
        return;
    }

    /* BTN_EVT_SHORT */
    switch (s_state) {
    case APP_STATE_WARMUP:
        if (id == BTN_OK) {
            ESP_LOGW(TAG, "Bỏ qua màn hình làm nóng (kênh MQ vẫn chờ đủ thời gian)");
            open_food_menu(true);
        }
        break;
    case APP_STATE_SELECT_FOOD:
        if (id == BTN_UP) {
            s_menu_index = (uint8_t)((s_menu_index + FOOD_GROUP_COUNT - 1) % FOOD_GROUP_COUNT);
        } else if (id == BTN_DOWN) {
            s_menu_index = (uint8_t)((s_menu_index + 1) % FOOD_GROUP_COUNT);
        } else if (id == BTN_OK) {
            confirm_food_menu();
        }
        break;
    case APP_STATE_MONITOR:
        if (id == BTN_UP) {
            s_ui.page = (ui_page_t)((s_ui.page + UI_PAGE_COUNT - 1) % UI_PAGE_COUNT);
        } else if (id == BTN_DOWN) {
            s_ui.page = (ui_page_t)((s_ui.page + 1) % UI_PAGE_COUNT);
        } else if (id == BTN_OK) {
            s_ui.page = UI_PAGE_OVERVIEW;
        }
        break;
    default:
        break;
    }
}

/* ------------------- vẽ OLED ------------------- */
static ui_wifi_state_t map_wifi(net_wifi_state_t st)
{
    switch (st) {
    case NET_WIFI_CONNECTING:   return UI_WIFI_CONNECTING;
    case NET_WIFI_CONNECTED:    return UI_WIFI_CONNECTED;
    case NET_WIFI_FAILED:       return UI_WIFI_FAILED;
    case NET_WIFI_DISCONNECTED: return UI_WIFI_DISCONNECTED;
    default:                    return UI_WIFI_NOT_CONFIGURED;
    }
}

static void render(void)
{
    uint32_t now = now_ms();
    s_ui.frame++;

    switch (s_state) {
    case APP_STATE_BOOT: {
        uint32_t el = now - s_state_enter_ms;
        s_ui.screen = UI_SCREEN_BOOT;
        s_ui.progress = (uint8_t)(el >= BOOT_SCREEN_MS ? 100 : el * 100 / BOOT_SCREEN_MS);
        break;
    }
    case APP_STATE_WARMUP:
        s_ui.screen = UI_SCREEN_WARMUP;
        s_ui.progress = sensor_manager_warmup_percent();
        s_ui.remaining_s = sensor_manager_warmup_remaining_s();
        break;
    case APP_STATE_CALIBRATE:
        s_ui.screen = UI_SCREEN_CALIBRATING;
        s_ui.progress = sensor_manager_calibration_percent();
        break;
    case APP_STATE_SELECT_FOOD:
        s_ui.screen = UI_SCREEN_FOOD_MENU;
        s_ui.menu_index = s_menu_index;
        break;
    default:
        s_ui.screen = UI_SCREEN_MAIN;
        break;
    }

    /* Đang cập nhật firmware: che mọi màn hình khác; lỗi -> báo một lần rồi quay lại */
    static ota_state_t s_ota_prev;
    ota_state_t ota = ota_update_state();
    if (ota == OTA_STATE_RUNNING || ota == OTA_STATE_DONE) {
        s_ui.screen = UI_SCREEN_OTA;
        s_ui.progress = ota_update_progress();
        strlcpy(s_ui.ota_msg, ota_update_message(), sizeof(s_ui.ota_msg));
    } else if (ota == OTA_STATE_FAILED && s_ota_prev == OTA_STATE_RUNNING) {
        char buf[28];
        snprintf(buf, sizeof(buf), "OTA: %.20s", ota_update_message());
        toast(buf);
    }
    s_ota_prev = ota;

    sensor_manager_get_data(&s_ui.data);
    s_ui.safety = s_safety;
    food_eval_get_session(&s_ui.session);
    s_ui.cloud_online = cloud_manager_is_online();
    strlcpy(s_ui.cloud_status, cloud_manager_status_text(), sizeof(s_ui.cloud_status));
    strlcpy(s_ui.broker, cloud_manager_broker_text(), sizeof(s_ui.broker));

    net_wifi_status_t ws;
    net_wifi_get_status(&ws);
    s_ui.wifi_state = map_wifi(ws.state);
    strlcpy(s_ui.sta_ssid, ws.ssid, sizeof(s_ui.sta_ssid));
    strlcpy(s_ui.sta_ip, ws.ip, sizeof(s_ui.sta_ip));
    s_ui.rssi = ws.rssi;
    s_ui.portal_active = ws.portal_active;
    strlcpy(s_ui.ap_ssid, ws.ap_ssid, sizeof(s_ui.ap_ssid));
    s_ui.ap_clients = ws.ap_clients;

    if (s_ui.toast[0] && (int32_t)(now - s_toast_until) >= 0) {
        s_ui.toast[0] = '\0';
    }

    ui_display_render(&s_ui);
    if (oled_is_ready()) {
        oled_flush();
    }
}

/* ------------------- chuyển trạng thái theo thời gian ------------------- */
static void state_tick(void)
{
    uint32_t now = now_ms();
    switch (s_state) {
    case APP_STATE_BOOT:
        if (now - s_state_enter_ms >= BOOT_SCREEN_MS) {
            uint32_t mq = sensor_manager_present_mask() & (SENSOR_PRESENT_MQ136 | SENSOR_PRESENT_MQ137);
            if (mq && sensor_manager_warmup_remaining_s() > 0) {
                enter_state(APP_STATE_WARMUP);
            } else {
                open_food_menu(true);
            }
        }
        break;
    case APP_STATE_WARMUP:
        if (sensor_manager_warmup_remaining_s() == 0) {
            open_food_menu(true);       /* làm nóng xong -> chọn món */
        }
        break;
    case APP_STATE_CALIBRATE:
        if (!sensor_manager_is_calibrating()) {
            enter_state(APP_STATE_MONITOR);
            toast("Hieu chuan xong");
            /* R0 mới làm thay đổi Rs/R0 -> lấy lại nền */
            food_session_t s;
            food_eval_get_session(&s);
            start_new_session(s.food);
        }
        break;
    default:
        break;
    }
}

static void fsm_task(void *arg)
{
    (void)arg;
    app_event_t e;
    TickType_t last_render = 0;
    while (1) {
        bool urgent = false;
        if (xQueueReceive(s_queue, &e, pdMS_TO_TICKS(UI_REFRESH_PERIOD_MS)) == pdTRUE) {
            urgent = e.type == EVT_BUTTON || e.type == EVT_CMD;
            switch (e.type) {
            case EVT_SENSOR:
                evaluate();
                break;
            case EVT_BUTTON:
                handle_button(e.btn.id, e.btn.kind);
                break;
            case EVT_CMD:
                handle_cmd(&e.cmd);
                break;
            case EVT_WIFI:
            default:
                break;
            }
        }
        state_tick();
        TickType_t now = xTaskGetTickCount();
        if ((now - last_render) >= pdMS_TO_TICKS(UI_REFRESH_PERIOD_MS) || urgent) {
            last_render = now;
            render();
        }
    }
}

esp_err_t app_fsm_start(void)
{
    s_queue = xQueueCreate(FSM_QUEUE_LEN, sizeof(app_event_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    memset(&s_ui, 0, sizeof(s_ui));
    food_eval_init();
    food_eval_local(NULL, &s_safety);
    s_state = APP_STATE_BOOT;
    s_state_enter_ms = now_ms();

    button_init(on_button, NULL);
    net_wifi_set_callback(on_wifi_change, NULL);
    cloud_manager_set_interval(TELEMETRY_INTERVAL_S);
    cloud_manager_init(on_cloud_cmd, NULL);
    sensor_manager_start(on_sensor_update, NULL);

    if (xTaskCreate(fsm_task, "app_fsm", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

app_state_t app_fsm_get_state(void)
{
    return s_state;
}

void app_fsm_get_safety(food_safety_t *out)
{
    if (out) *out = s_safety;
}