/*
 * ui_display.c - Vẽ giao diện
 * Font chỉ có ASCII nên chữ tiếng Việt viết không dấu.
 */
#include "ui_display.h"
#include "oled_gfx.h"
#include "food_eval.h"
#include <stdio.h>
#include <string.h>

#define HEADER_H 10
#define DEG "\x7F"   /* ký tự dấu độ trong font */

static const char *const s_page_titles[UI_PAGE_COUNT] = {
    "TONG QUAN", "KHI SO VOI NEN", "MOI TRUONG", "KET NOI",
};

static int clamp_pct(float v, float lo, float hi)
{
    if (hi <= lo) return 0;
    float p = (v - lo) * 100.0f / (hi - lo);
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    return (int)(p + 0.5f);
}

static void draw_wifi_icon(int x, int y, const ui_model_t *m, oled_color_t c)
{
    /* 4 cột tín hiệu cao dần, rộng 9px, cao 8px */
    int bars = 0;
    if (m->wifi_state == UI_WIFI_CONNECTED) {
        bars = m->rssi >= -55 ? 4 : m->rssi >= -67 ? 3 : m->rssi >= -78 ? 2 : 1;
    } else if (m->wifi_state == UI_WIFI_CONNECTING) {
        bars = (int)(m->frame / 2 % 5);   /* hiệu ứng đang kết nối */
    }
    for (int i = 0; i < 4; i++) {
        int h = 2 + i * 2;          /* cột cao 2,4,6,8 px */
        int bx = x + i * 2;         /* rộng 1px, cách nhau 1px */
        if (i < bars) {
            oled_gfx_vline(bx, y + 8 - h, h, c);
        } else {
            oled_gfx_pixel(bx, y + 7, c);
        }
    }
    if (m->wifi_state == UI_WIFI_FAILED || m->wifi_state == UI_WIFI_NOT_CONFIGURED ||
        m->wifi_state == UI_WIFI_DISCONNECTED) {
        /* dấu x nhỏ */
        for (int i = 0; i < 4; i++) {
            oled_gfx_pixel(x + 4 + i, y + i, c);
            oled_gfx_pixel(x + 7 - i, y + i, c);
        }
    }
}

static void draw_header(const char *title, const ui_model_t *m)
{
    oled_gfx_fill_rect(0, 0, OLED_WIDTH, HEADER_H - 1, OLED_COLOR_WHITE);
    oled_gfx_text(2, 1, title, 1, OLED_COLOR_BLACK);
    int x = OLED_WIDTH - 9;
    draw_wifi_icon(x, 0, m, OLED_COLOR_BLACK);
    x -= 3;
    if (m->portal_active) {
        x -= oled_gfx_text_width("AP", 1);
        oled_gfx_text(x, 1, "AP", 1, OLED_COLOR_BLACK);
        x -= 3;
    }
}

static void draw_page_dots(const ui_model_t *m)
{
    int total_w = UI_PAGE_COUNT * 6 - 2;
    int x = (OLED_WIDTH - total_w) / 2;
    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        if (i == (int)m->page) {
            oled_gfx_fill_rect(x + i * 6, 61, 4, 3, OLED_COLOR_WHITE);
        } else {
            oled_gfx_pixel(x + i * 6 + 1, 62, OLED_COLOR_WHITE);
            oled_gfx_pixel(x + i * 6 + 2, 62, OLED_COLOR_WHITE);
        }
    }
}

/* Một hàng: nhãn | thanh loading có vạch ngưỡng | giá trị */
static void draw_bar_row(int y, const char *label, bool valid, bool waiting, int pct,
                         int mark_warn, int mark_danger, const char *value)
{
    const int bar_x = 25, bar_w = 64, bar_h = 9;
    oled_gfx_text(0, y + 1, label, 1, OLED_COLOR_WHITE);
    if (valid) {
        oled_gfx_progress_marks(bar_x, y, bar_w, bar_h, pct, mark_warn, mark_danger);
        oled_gfx_text_aligned(bar_x + bar_w + 2, y + 1, OLED_WIDTH - bar_x - bar_w - 2, value, 1,
                              OLED_ALIGN_RIGHT, OLED_COLOR_WHITE);
    } else {
        oled_gfx_round_rect(bar_x, y, bar_w, bar_h, OLED_COLOR_WHITE);
        for (int x = bar_x + 3; x < bar_x + bar_w - 2; x += 3) {
            oled_gfx_pixel(x, y + 4, OLED_COLOR_WHITE);   /* đường chấm: chưa có dữ liệu */
        }
        oled_gfx_text_aligned(bar_x + bar_w + 2, y + 1, OLED_WIDTH - bar_x - bar_w - 2,
                              waiting ? "..." : "--", 1, OLED_ALIGN_RIGHT, OLED_COLOR_WHITE);
    }
}


static void fmt_duration(char *buf, size_t n, uint32_t sec)
{
    if (sec >= 3600) snprintf(buf, n, "%luh%02lu", (unsigned long)(sec / 3600), (unsigned long)((sec / 60) % 60));
    else snprintf(buf, n, "%lum", (unsigned long)(sec / 60));
}

/* ---------------- Trang 0: Tổng quan ---------------- */
static void page_overview(const ui_model_t *m)
{
    const food_safety_t *s = &m->safety;
    const char *lv = food_level_to_str(s->level);
    char buf[28];

    /* Mức kết quả: chữ to nếu vừa (<= 10 ký tự), còn lại chữ thường; BIEN DOI MANH nhấp nháy */
    int scale = oled_gfx_text_width(lv, 2) <= OLED_WIDTH ? 2 : 1;
    int ty = scale == 2 ? 14 : 17;
    bool blink = (s->level == FOOD_LEVEL_DANGER) && ((m->frame / 3) % 2 == 0);
    if (blink) {
        oled_gfx_fill_rect(0, 12, OLED_WIDTH, 18, OLED_COLOR_WHITE);
        oled_gfx_text_aligned(0, ty, OLED_WIDTH, lv, scale, OLED_ALIGN_CENTER, OLED_COLOR_BLACK);
    } else {
        oled_gfx_rect(0, 12, OLED_WIDTH, 18, OLED_COLOR_WHITE);
        oled_gfx_text_aligned(0, ty, OLED_WIDTH, lv, scale, OLED_ALIGN_CENTER, OLED_COLOR_WHITE);
    }

    /* Thanh chỉ số khí S, vạch tại ngưỡng T1/T2 của nhóm món */
    oled_gfx_text(0, 34, "S", 1, OLED_COLOR_WHITE);
    if (s->gas_level == FOOD_LEVEL_UNKNOWN) {
        /* Thanh chạy qua lại CHỈ khi đang lấy mẫu nền; thiếu dữ liệu thì để trống */
        if (!m->session.base_ready) {
            oled_gfx_progress_indeterminate(8, 33, 94, 9, m->frame);
        } else {
            oled_gfx_round_rect(8, 33, 94, 9, OLED_COLOR_WHITE);
        }
        snprintf(buf, sizeof(buf), "--");
    } else {
        /* T1/T2 lấy theo nhóm món hiện tại (food_eval_profile) */
        const food_profile_t *p = food_eval_profile(m->session.food);
        oled_gfx_progress_marks(8, 33, 94, 9, s->score, p->t1, p->t2);
        snprintf(buf, sizeof(buf), "%u", s->score);
    }
    oled_gfx_text_aligned(103, 34, 25, buf, 1, OLED_ALIGN_RIGHT, OLED_COLOR_WHITE);

    oled_gfx_text_aligned(0, 44, OLED_WIDTH, s->message, 1, OLED_ALIGN_CENTER, OLED_COLOR_WHITE);

    if (m->data.valid & SENSOR_VALID_TEMP) {
        snprintf(buf, sizeof(buf), "%.1f" DEG "C %.0f%%", m->data.temperature_c, m->data.humidity_rh);
    } else {
        snprintf(buf, sizeof(buf), "--" DEG "C --%%");
    }
    oled_gfx_text(0, 53, buf, 1, OLED_COLOR_WHITE);
    oled_gfx_text_aligned(70, 53, 58, m->cloud_online ? "Online" : "Offline", 1, OLED_ALIGN_RIGHT, OLED_COLOR_WHITE);
}

/* ---------------- Trang 1: Khí (thay đổi so với nền) ---------------- */
static int pct_norm(float d, float lo, float hi)
{
    return clamp_pct(d, lo, hi);
}

static void page_gas(const ui_model_t *m)
{
    const food_sensor_data_t *d = &m->data;
    const food_session_t *se = &m->session;
    bool base = se->base_ready;
    bool mq_ok = d->mq_warmed_up;
    char v[12];

    bool ok = base && mq_ok && (d->valid & SENSOR_VALID_NH3) && se->base_nh3_rs_r0 > 0 && d->nh3_rs_r0 > 0;
    float dn = ok ? se->base_nh3_rs_r0 / d->nh3_rs_r0 : 0;
    snprintf(v, sizeof(v), "x%.2f", dn);
    draw_bar_row(13, "NH3", ok, !ok, pct_norm(dn, 1.0f, FOOD_NH3_HI),
                 pct_norm(FOOD_NH3_LO, 1.0f, FOOD_NH3_HI), -1, v);

    ok = base && mq_ok && (d->valid & SENSOR_VALID_H2S) && se->base_h2s_rs_r0 > 0 && d->h2s_rs_r0 > 0;
    float dh = ok ? se->base_h2s_rs_r0 / d->h2s_rs_r0 : 0;
    snprintf(v, sizeof(v), "x%.2f", dh);
    draw_bar_row(25, "H2S", ok, !ok, pct_norm(dh, 1.0f, FOOD_H2S_HI),
                 pct_norm(FOOD_H2S_LO, 1.0f, FOOD_H2S_HI), -1, v);

    ok = base && (d->valid & SENSOR_VALID_VOC) && se->base_voc_raw > 0;
    float dv = ok ? (float)se->base_voc_raw - d->voc_raw : 0;
    snprintf(v, sizeof(v), "%+d", (int)dv);
    draw_bar_row(37, "VOC", ok, !ok, pct_norm(dv, 0, FOOD_VOC_HI),
                 pct_norm(FOOD_VOC_LO, 0, FOOD_VOC_HI), -1, v);

    /* CO2: thanh theo độ tăng so với nền, số hiển thị là ppm tuyệt đối */
    ok = (d->valid & SENSOR_VALID_CO2) != 0;
    float dc = (ok && base && se->base_co2_ppm) ? (float)d->co2_ppm - se->base_co2_ppm : 0;
    snprintf(v, sizeof(v), "%u", d->co2_ppm);
    draw_bar_row(49, "CO2", ok, false, pct_norm(dc, 0, FOOD_CO2_HI),
                 pct_norm(FOOD_CO2_LO, 0, FOOD_CO2_HI), -1, v);
}

/* ---------------- Trang 2: Môi trường + quỹ thời gian ---------------- */
static void page_env(const ui_model_t *m)
{
    const food_sensor_data_t *d = &m->data;
    char v[16];
    bool has_t = d->valid & SENSOR_VALID_TEMP;

    snprintf(v, sizeof(v), "%.1f" DEG "C", d->temperature_c);
    draw_bar_row(13, "Temp", has_t, false,
                 clamp_pct(d->temperature_c, FOOD_TEMP_BAR_MIN, FOOD_TEMP_BAR_MAX),
                 clamp_pct(FOOD_TEMP_ZONE_LOW_C, FOOD_TEMP_BAR_MIN, FOOD_TEMP_BAR_MAX),
                 clamp_pct(FOOD_HOT_AMBIENT_C, FOOD_TEMP_BAR_MIN, FOOD_TEMP_BAR_MAX), v);

    snprintf(v, sizeof(v), "%.0f%%", d->humidity_rh);
    draw_bar_row(25, "Hum", d->valid & SENSOR_VALID_HUM, false,
                 clamp_pct(d->humidity_rh, 0, 100), -1, -1, v);

    /* Quỹ thời gian an toàn 120 phút (USDA): vạch ở 60 phút */
    uint32_t used_min = m->session.budget_s / 60;
    snprintf(v, sizeof(v), "%lu/120p", (unsigned long)(used_min > 999 ? 999 : used_min));
    oled_gfx_text(0, 38, "Quy TG", 1, OLED_COLOR_WHITE);
    oled_gfx_text_aligned(40, 38, 88, v, 1, OLED_ALIGN_RIGHT, OLED_COLOR_WHITE);
    oled_gfx_progress_marks(0, 48, OLED_WIDTH, 9,
                            clamp_pct((float)m->session.budget_s, 0, (float)FOOD_BUDGET_EXPIRED_S),
                            clamp_pct((float)FOOD_BUDGET_WARN_S, 0, (float)FOOD_BUDGET_EXPIRED_S), -1);
}

/* ---------------- Trang 3: Kết nối ---------------- */
static void page_network(const ui_model_t *m)
{
    char buf[40];
    const char *st;
    switch (m->wifi_state) {
    case UI_WIFI_CONNECTED:    st = "Da ket noi"; break;
    case UI_WIFI_CONNECTING:   st = "Dang ket noi..."; break;
    case UI_WIFI_FAILED:       st = "Loi ket noi"; break;
    case UI_WIFI_DISCONNECTED: st = "Da ngat ket noi"; break;
    default:                   st = "Chua cau hinh"; break;
    }
    snprintf(buf, sizeof(buf), "WiFi: %s", st);
    oled_gfx_text(0, 13, buf, 1, OLED_COLOR_WHITE);

    if (m->sta_ssid[0]) {
        snprintf(buf, sizeof(buf), "> %.19s", m->sta_ssid);
        oled_gfx_text(0, 22, buf, 1, OLED_COLOR_WHITE);
    }
    if (m->wifi_state == UI_WIFI_CONNECTED) {
        snprintf(buf, sizeof(buf), "IP %s", m->sta_ip);
        oled_gfx_text(0, 31, buf, 1, OLED_COLOR_WHITE);
    }
    if (m->portal_active) {
        snprintf(buf, sizeof(buf), "AP %.15s (%u)", m->ap_ssid, m->ap_clients);
    } else {
        snprintf(buf, sizeof(buf), "Bk %.18s", m->broker);   /* địa chỉ broker đang dùng */
    }
    oled_gfx_text(0, 40, buf, 1, OLED_COLOR_WHITE);
    snprintf(buf, sizeof(buf), "Srv: %.16s", m->cloud_status);
    oled_gfx_text(0, 49, buf, 1, OLED_COLOR_WHITE);
    oled_gfx_hline(0, 58, OLED_WIDTH, OLED_COLOR_WHITE);
}

/* ---------------- Màn hình đặc biệt ---------------- */
static void screen_boot(const ui_model_t *m)
{
    oled_gfx_text_aligned(0, 6, OLED_WIDTH, "HOP COM", 2, OLED_ALIGN_CENTER, OLED_COLOR_WHITE);
    oled_gfx_text_aligned(0, 26, OLED_WIDTH, "THONG MINH", 1, OLED_ALIGN_CENTER, OLED_COLOR_WHITE);
    oled_gfx_progress(14, 40, 100, 8, m->progress);
    oled_gfx_text_aligned(0, 53, OLED_WIDTH, "Dang khoi dong...", 1, OLED_ALIGN_CENTER, OLED_COLOR_WHITE);
}

/* Thanh lớn, chữ % đảo màu nằm giữa thanh */
static void big_progress(int y, uint8_t pct)
{
    char buf[8];
    oled_gfx_progress(4, y, 120, 13, pct);
    snprintf(buf, sizeof(buf), "%u%%", pct);
    oled_gfx_text_aligned(4, y + 3, 120, buf, 1, OLED_ALIGN_CENTER, OLED_COLOR_INVERT);
}

static void screen_warmup(const ui_model_t *m)
{
    char buf[24];
    draw_header("KHOI DONG", m);
    oled_gfx_text_aligned(0, 14, OLED_WIDTH, "Lam nong cam bien", 1, OLED_ALIGN_CENTER, OLED_COLOR_WHITE);
    big_progress(25, m->progress);
    snprintf(buf, sizeof(buf), "Con lai %lu:%02lu",
             (unsigned long)(m->remaining_s / 60), (unsigned long)(m->remaining_s % 60));
    oled_gfx_text_aligned(0, 42, OLED_WIDTH, buf, 1, OLED_ALIGN_CENTER, OLED_COLOR_WHITE);
    oled_gfx_text_aligned(0, 54, OLED_WIDTH, "OK: bo qua", 1, OLED_ALIGN_CENTER, OLED_COLOR_WHITE);
}

static void screen_calibrating(const ui_model_t *m)
{
    draw_header("HIEU CHUAN MQ", m);
    oled_gfx_text_aligned(0, 14, OLED_WIDTH, "Dat hop noi", 1, OLED_ALIGN_CENTER, OLED_COLOR_WHITE);
    oled_gfx_text_aligned(0, 23, OLED_WIDTH, "khong khi sach", 1, OLED_ALIGN_CENTER, OLED_COLOR_WHITE);
    big_progress(36, m->progress);
}

/* Menu chọn món: 5 dòng cao 10 px dưới thanh tiêu đề, dòng đang chọn đảo màu */
static void screen_food_menu(const ui_model_t *m)
{
    draw_header("CHON MON AN", m);
    for (int i = 0; i < FOOD_GROUP_COUNT; i++) {
        int y = HEADER_H + 1 + i * 10;
        bool sel = i == m->menu_index;
        if (sel) oled_gfx_fill_rect(0, y - 1, OLED_WIDTH, 10, OLED_COLOR_WHITE);
        oled_color_t c = sel ? OLED_COLOR_BLACK : OLED_COLOR_WHITE;
        oled_gfx_text(2, y, sel ? ">" : " ", 1, c);
        oled_gfx_text(10, y, food_group_to_name((food_group_t)i), 1, c);
        if (sel) oled_gfx_text_aligned(0, y, OLED_WIDTH - 2, "OK", 1, OLED_ALIGN_RIGHT, c);
    }
}

static void screen_ota(const ui_model_t *m)
{
    draw_header("CAP NHAT FIRMWARE", m);
    oled_gfx_text_aligned(0, 14, OLED_WIDTH, "Khong tat nguon!", 1, OLED_ALIGN_CENTER, OLED_COLOR_WHITE);
    big_progress(27, m->progress);
    oled_gfx_text_aligned(0, 46, OLED_WIDTH, m->ota_msg, 1, OLED_ALIGN_CENTER, OLED_COLOR_WHITE);
}

static void draw_toast(const char *msg)
{
    int w = oled_gfx_text_width(msg, 1) + 12;
    if (w > OLED_WIDTH - 4) w = OLED_WIDTH - 4;
    int x = (OLED_WIDTH - w) / 2;
    oled_gfx_fill_rect(x, 22, w, 20, OLED_COLOR_BLACK);
    oled_gfx_rect(x, 22, w, 20, OLED_COLOR_WHITE);
    oled_gfx_rect(x + 2, 24, w - 4, 16, OLED_COLOR_WHITE);
    oled_gfx_text_aligned(x, 28, w, msg, 1, OLED_ALIGN_CENTER, OLED_COLOR_WHITE);
}

void ui_display_render(const ui_model_t *m)
{
    oled_gfx_clear();
    if (!m) return;

    switch (m->screen) {
    case UI_SCREEN_BOOT:
        screen_boot(m);
        break;
    case UI_SCREEN_WARMUP:
        screen_warmup(m);
        break;
    case UI_SCREEN_CALIBRATING:
        screen_calibrating(m);
        break;
    case UI_SCREEN_OTA:
        screen_ota(m);
        break;
    case UI_SCREEN_FOOD_MENU:
        screen_food_menu(m);
        break;
    case UI_SCREEN_MAIN:
    default: {
        ui_page_t p = m->page < UI_PAGE_COUNT ? m->page : UI_PAGE_OVERVIEW;
        if (p == UI_PAGE_OVERVIEW) {
            char title[20], age[12];
            fmt_duration(age, sizeof(age), m->session.age_s);
            snprintf(title, sizeof(title), "%s  %s", food_group_to_label(m->session.food), age);
            draw_header(title, m);
        } else {
            draw_header(s_page_titles[p], m);
        }
        switch (p) {
        case UI_PAGE_GAS:     page_gas(m); break;
        case UI_PAGE_ENV:     page_env(m); break;
        case UI_PAGE_NETWORK: page_network(m); break;
        default:              page_overview(m); break;
        }
        draw_page_dots(m);
        break;
    }
    }

    if (m->toast[0]) {
        draw_toast(m->toast);
    }
}