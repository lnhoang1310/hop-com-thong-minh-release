/*
 * food_eval.c - Phiên đo, giá trị nền, quỹ thời gian và thuật toán rule-0.1
 * Chỉ được gọi từ task FSM (không cần khoá).
 */
#include "food_eval.h"
#include "esp_random.h"
#include "esp_log.h"
#include "nvs.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "EVAL";

#define NVS_NS        "session"
#define NVS_KEY       "s1"
#define SAVE_EVERY_S  300

/* Trọng số NH3 / H2S / VOC / CO2 và ngưỡng T1 / T2 theo báo cáo */
static const food_profile_t s_profiles[FOOD_GROUP_COUNT] = {
    [FOOD_GROUP_RICE] = { 0.1f, 0.1f, 0.4f, 0.4f, 40, 70 },
    [FOOD_GROUP_MEAT] = { 0.4f, 0.3f, 0.2f, 0.1f, 40, 70 },
    [FOOD_GROUP_FISH] = { 0.6f, 0.2f, 0.1f, 0.1f, 35, 65 },
    [FOOD_GROUP_SOUP] = { 0.3f, 0.1f, 0.3f, 0.3f, 40, 70 },
    [FOOD_GROUP_EGG]  = { 0.1f, 0.7f, 0.1f, 0.1f, 30, 60 },
};

/* Bản lưu NVS */
typedef struct {
    uint32_t magic;
    food_session_t s;
    int64_t  start_unix;   /* 0 = chưa biết giờ thực */
} saved_t;
#define SAVED_MAGIC 0x48435332u   /* "HCS2" - đổi số này để bỏ phiên lưu từ firmware cũ */

static food_session_t s_sess;
static int64_t  s_start_unix;
static uint32_t s_last_ts;
static bool     s_has_last;
static uint32_t s_age_ms, s_zone_ms, s_budget_ms;   /* phần lẻ < 1000 ms chưa cộng vào giây */
static food_level_t s_prev_gas = FOOD_LEVEL_UNKNOWN; /* cho vùng trễ */
static uint32_t s_last_save_age;

/* tích luỹ nền */
static uint8_t  s_base_n;
static double   s_acc_nh3, s_acc_h2s, s_acc_voc, s_acc_co2;
static uint8_t  s_n_nh3, s_n_h2s, s_n_voc, s_n_co2;

const food_profile_t *food_eval_profile(food_group_t g)
{
    return &s_profiles[g < FOOD_GROUP_COUNT ? g : FOOD_GROUP_RICE];
}

static bool time_valid(void)
{
    return time(NULL) > 1700000000;   /* sau 11/2023 -> đã đồng bộ SNTP */
}

static void reset_baseline(void)
{
    s_base_n = 0;
    s_acc_nh3 = s_acc_h2s = s_acc_voc = s_acc_co2 = 0;
    s_n_nh3 = s_n_h2s = s_n_voc = s_n_co2 = 0;
    s_sess.base_ready = false;
}

void food_eval_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    saved_t sv = { .magic = SAVED_MAGIC, .s = s_sess, .start_unix = s_start_unix };
    nvs_set_blob(h, NVS_KEY, &sv, sizeof(sv));
    nvs_commit(h);
    nvs_close(h);
    s_last_save_age = s_sess.age_s;
}

void food_eval_init(void)
{
    memset(&s_sess, 0, sizeof(s_sess));
    nvs_handle_t h;
    saved_t sv;
    size_t len = sizeof(sv);
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, NVS_KEY, &sv, &len) == ESP_OK && len == sizeof(sv) && sv.magic == SAVED_MAGIC;
        nvs_close(h);
    }
    if (ok) {
        /* Khôi phục phiên sau khi mất điện. Khi có giờ thực (SNTP), khoảng tắt máy được
         * cộng vào tuổi phiên và quỹ thời gian (xem food_eval_update). */
        s_sess = sv.s;
        s_start_unix = sv.start_unix;
        ESP_LOGI(TAG, "Khôi phục phiên %08lx (%s), tuổi %lus", (unsigned long)s_sess.id,
                 food_group_to_code(s_sess.food), (unsigned long)s_sess.age_s);
    } else {
        food_eval_new_session(FOOD_GROUP_RICE);
    }
}

void food_eval_new_session(food_group_t food)
{
    memset(&s_sess, 0, sizeof(s_sess));
    s_sess.id = esp_random();
    s_sess.food = food < FOOD_GROUP_COUNT ? food : FOOD_GROUP_RICE;
    s_start_unix = time_valid() ? (int64_t)time(NULL) : 0;
    s_has_last = false;
    s_age_ms = s_zone_ms = s_budget_ms = 0;
    s_prev_gas = FOOD_LEVEL_UNKNOWN;
    reset_baseline();
    ESP_LOGI(TAG, "Phiên mới %08lx, món %s", (unsigned long)s_sess.id, food_group_to_code(s_sess.food));
    food_eval_save();
}

void food_eval_set_food(food_group_t food)
{
    if (food >= FOOD_GROUP_COUNT) return;
    s_sess.food = food;
    food_eval_save();
}

void food_eval_get_session(food_session_t *out)
{
    if (out) *out = s_sess;
}

/* Nền đã lấy khi thiếu một kênh (cảm biến lúc đó chưa chạy) mà giờ kênh đó có dữ liệu
 * -> nền không còn đúng, phải lấy lại; nếu không kênh đó bị bỏ qua mãi mãi */
static bool base_missing_channel(const food_sensor_data_t *d)
{
    if (!s_sess.base_ready) return false;
    if ((d->valid & SENSOR_VALID_CO2) && s_sess.base_co2_ppm == 0) return true;
    if ((d->valid & SENSOR_VALID_VOC) && s_sess.base_voc_raw == 0) return true;
    if (d->mq_warmed_up && (d->valid & SENSOR_VALID_NH3) && s_sess.base_nh3_rs_r0 <= 0) return true;
    if (d->mq_warmed_up && (d->valid & SENSOR_VALID_H2S) && s_sess.base_h2s_rs_r0 <= 0) return true;
    return false;
}

static void accumulate_baseline(const food_sensor_data_t *d)
{
    if (base_missing_channel(d)) {
        ESP_LOGW(TAG, "Có thêm kênh cảm biến so với lúc lấy nền -> lấy lại nền");
        reset_baseline();
    }
    if (s_sess.base_ready) return;
    /* Chờ MQ làm nóng xong rồi mới lấy nền cho các kênh khí */
    if (!d->mq_warmed_up && (d->valid & (SENSOR_VALID_NH3 | SENSOR_VALID_H2S))) {
        return;   /* có MQ nhưng chưa làm nóng xong -> chưa lấy nền kênh nào */
    }
    if (d->valid & SENSOR_VALID_NH3) { s_acc_nh3 += d->nh3_rs_r0; s_n_nh3++; }
    if (d->valid & SENSOR_VALID_H2S) { s_acc_h2s += d->h2s_rs_r0; s_n_h2s++; }
    if (d->valid & SENSOR_VALID_VOC) { s_acc_voc += d->voc_raw; s_n_voc++; }
    if (d->valid & SENSOR_VALID_CO2) { s_acc_co2 += d->co2_ppm; s_n_co2++; }
    if (++s_base_n < FOOD_BASELINE_SAMPLES) return;

    s_sess.base_nh3_rs_r0 = s_n_nh3 ? (float)(s_acc_nh3 / s_n_nh3) : 0;
    s_sess.base_h2s_rs_r0 = s_n_h2s ? (float)(s_acc_h2s / s_n_h2s) : 0;
    s_sess.base_voc_raw   = s_n_voc ? (uint16_t)(s_acc_voc / s_n_voc) : 0;
    s_sess.base_co2_ppm   = s_n_co2 ? (uint16_t)(s_acc_co2 / s_n_co2) : 0;
    s_sess.base_ready = true;
    ESP_LOGI(TAG, "Nền: NH3 %.3f H2S %.3f VOC %u CO2 %u", s_sess.base_nh3_rs_r0,
             s_sess.base_h2s_rs_r0, s_sess.base_voc_raw, s_sess.base_co2_ppm);
    food_eval_save();
}

void food_eval_update(const food_sensor_data_t *d)
{
    if (!d) return;
    if (s_has_last) {
        uint32_t dt_ms = d->timestamp_ms - s_last_ts;
        if (dt_ms < 60000) {
            s_age_ms += dt_ms;
            if (d->valid & SENSOR_VALID_TEMP) {
                float t = d->temperature_c;
                if (t >= FOOD_TEMP_ZONE_LOW_C && t <= FOOD_TEMP_ZONE_HIGH_C) {
                    s_zone_ms += dt_ms;
                    s_budget_ms += (t > FOOD_HOT_AMBIENT_C) ? 2 * dt_ms : dt_ms;
                }
            }
            /* chuyển phần đủ giây sang bộ đếm giây, giữ phần lẻ */
            s_sess.age_s += s_age_ms / 1000;       s_age_ms %= 1000;
            s_sess.zone_s += s_zone_ms / 1000;     s_zone_ms %= 1000;
            s_sess.budget_s += s_budget_ms / 1000; s_budget_ms %= 1000;
        }
    }
    s_last_ts = d->timestamp_ms;
    s_has_last = true;

    /* Đồng bộ tuổi phiên theo giờ thực khi có SNTP. Khoảng tắt máy không đo được nhiệt độ:
     * tính THẬN TRỌNG là nằm trong vùng nguy hiểm (cộng vào zone_s và budget_s, hệ số 1). */
    if (time_valid()) {
        int64_t now = time(NULL);
        if (s_start_unix == 0) {
            s_start_unix = now - s_sess.age_s;
        } else if (now - s_start_unix > (int64_t)s_sess.age_s + 5) {
            uint32_t gap = (uint32_t)(now - s_start_unix) - s_sess.age_s;
            s_sess.age_s += gap;
            s_sess.zone_s += gap;
            s_sess.budget_s += gap;
            ESP_LOGW(TAG, "Bù %lus thời gian tắt máy vào tuổi phiên và quỹ thời gian", (unsigned long)gap);
        }
    }

    accumulate_baseline(d);

    if (s_sess.age_s - s_last_save_age >= SAVE_EVERY_S) {
        food_eval_save();
    }
}

static float clip01(float v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

static float norm(float d, float lo, float hi)
{
    return clip01((d - lo) / (hi - lo));
}

void food_eval_local(const food_sensor_data_t *d, food_safety_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    strlcpy(out->model, FOOD_EVAL_MODEL, sizeof(out->model));
    out->from_cloud = false;
    out->timestamp_ms = d ? d->timestamp_ms : 0;
    out->seq = d ? d->seq : 0;
    out->valid_ms = 60000;

    /* --- Mức thời gian --- */
    if (s_sess.budget_s >= FOOD_BUDGET_EXPIRED_S || s_sess.age_s >= FOOD_AGE_EXPIRED_S) {
        out->time_level = FOOD_TIME_EXPIRED;
    } else if (s_sess.budget_s >= FOOD_BUDGET_WARN_S || s_sess.age_s >= FOOD_AGE_WARN_S) {
        out->time_level = FOOD_TIME_WARN;
    } else {
        out->time_level = FOOD_TIME_OK;
    }

    /* --- Mức khí --- */
    const food_profile_t *p = food_eval_profile(s_sess.food);
    float sum_w = 0, sum = 0;
    if (d && s_sess.base_ready) {
        if ((d->valid & SENSOR_VALID_NH3) && d->mq_warmed_up && s_sess.base_nh3_rs_r0 > 0 && d->nh3_rs_r0 > 0) {
            sum += p->w_nh3 * norm(s_sess.base_nh3_rs_r0 / d->nh3_rs_r0, FOOD_NH3_LO, FOOD_NH3_HI);
            sum_w += p->w_nh3;
        }
        if ((d->valid & SENSOR_VALID_H2S) && d->mq_warmed_up && s_sess.base_h2s_rs_r0 > 0 && d->h2s_rs_r0 > 0) {
            sum += p->w_h2s * norm(s_sess.base_h2s_rs_r0 / d->h2s_rs_r0, FOOD_H2S_LO, FOOD_H2S_HI);
            sum_w += p->w_h2s;
        }
        if ((d->valid & SENSOR_VALID_VOC) && s_sess.base_voc_raw > 0) {
            sum += p->w_voc * norm((float)s_sess.base_voc_raw - d->voc_raw, FOOD_VOC_LO, FOOD_VOC_HI);
            sum_w += p->w_voc;
        }
        if ((d->valid & SENSOR_VALID_CO2) && s_sess.base_co2_ppm > 0) {
            sum += p->w_co2 * norm((float)d->co2_ppm - s_sess.base_co2_ppm, FOOD_CO2_LO, FOOD_CO2_HI);
            sum_w += p->w_co2;
        }
    }

    if (sum_w < 0.5f) {
        /* < 50% trọng số có dữ liệu -> chưa đủ cơ sở */
        out->gas_level = FOOD_LEVEL_UNKNOWN;
    } else {
        int s = (int)lroundf(100.0f * sum / sum_w);
        out->score = (uint8_t)(s < 0 ? 0 : s > 100 ? 100 : s);
        /* Vùng trễ FOOD_HYSTERESIS điểm: chỉ hạ mức khi S xuống dưới ngưỡng - trễ,
         * tránh đèn/còi nhảy qua lại khi S dao động quanh ngưỡng */
        int t1 = p->t1, t2 = p->t2;
        if (s_prev_gas == FOOD_LEVEL_DANGER) t2 -= FOOD_HYSTERESIS;
        if (s_prev_gas >= FOOD_LEVEL_WARNING) t1 -= FOOD_HYSTERESIS;
        out->gas_level = out->score >= t2 ? FOOD_LEVEL_DANGER
                       : out->score >= t1 ? FOOD_LEVEL_WARNING : FOOD_LEVEL_SAFE;
    }
    if (d) s_prev_gas = out->gas_level;

    /* --- Kết hợp --- */
    food_level_t t = out->time_level == FOOD_TIME_EXPIRED ? FOOD_LEVEL_DANGER
                   : out->time_level == FOOD_TIME_WARN ? FOOD_LEVEL_WARNING : FOOD_LEVEL_SAFE;
    if (out->gas_level == FOOD_LEVEL_UNKNOWN) {
        out->level = (out->time_level == FOOD_TIME_OK) ? FOOD_LEVEL_UNKNOWN : t;
    } else {
        out->level = out->gas_level > t ? out->gas_level : t;
    }

    const char *msg;
    if (out->time_level == FOOD_TIME_EXPIRED)            msg = "Qua thoi gian an toan";
    else if (out->gas_level == FOOD_LEVEL_DANGER)       msg = "Khi bien doi manh";
    else if (out->gas_level == FOOD_LEVEL_WARNING)      msg = "Khi tang - kiem tra";
    else if (out->time_level == FOOD_TIME_WARN)         msg = "Sap het thoi gian";
    else if (out->gas_level == FOOD_LEVEL_SAFE)         msg = "Chua thay bat thuong";
    else if (d && !d->mq_warmed_up)                     msg = "MQ dang lam nong";
    else if (s_sess.base_ready)                         msg = "Thieu du lieu cam bien";
    else                                                msg = NULL;
    if (msg) {
        strlcpy(out->message, msg, sizeof(out->message));
    } else {
        snprintf(out->message, sizeof(out->message), "Lay mau nen %u/%u", s_base_n, FOOD_BASELINE_SAMPLES);
    }
}