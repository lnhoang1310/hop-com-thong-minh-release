#pragma once
/*
 * food_eval.h - Phiên đo + đánh giá cục bộ (thuật toán "rule-0.1")
 *
 * Server dùng CÙNG công thức này (xem cloud_protocol.h). Thiết bị tự tính khi
 * chưa có / mất kết nối server, nên hai bên luôn cho kết quả giống nhau.
 *
 * 1) Độ thay đổi so với nền (nền = trung bình 10 mẫu đầu của phiên):
 *      dNH3 = nền(Rs/R0) / hiện tại(Rs/R0)     (>1 khi NH3 tăng)
 *      dH2S = nền(Rs/R0) / hiện tại(Rs/R0)
 *      dVOC = nền(SRAW) - hiện tại(SRAW)        (ticks, >0 khi VOC tăng)
 *      dCO2 = hiện tại - nền                    (ppm)
 * 2) Chuẩn hoá: x = clip((d - lo) / (hi - lo), 0, 1)
 * 3) Chỉ số khí: S = 100 * sum(w_i * x_i) / sum(w_i)   (chỉ cộng kênh có dữ liệu)
 * 4) Mức khí: S < T1 -> low, T1 <= S < T2 -> check, S >= T2 -> strong
 *    (vùng trễ 3 điểm: đang ở mức cao thì chỉ hạ khi S < ngưỡng - 3)
 * 5) Mức thời gian (USDA FSIS, FDA Food Code, FSS): quỹ 120 phút, x2 khi > 32 °C
 * 6) Mức cuối = max(mức khí, mức thời gian)
 * 7) Trạng thái nhiệt (nóng / đang nguội / đã nguội / lạnh) từ nhiệt độ trong hộp và độ dốc
 *    3 phút gần nhất. Mẫu nền chỉ được lấy khi đã nguội ổn định (hoặc lạnh) >= 2 phút, để hơi
 *    nóng bốc lên lúc đầu không bị tính là "khí nền" (tối đa chờ 60 phút).
 * Trọng số, T1, T2 lấy từ báo cáo (giả thuyết ban đầu, cần hiệu chỉnh bằng thực nghiệm).
 */
#include "food_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FOOD_EVAL_MODEL        "rule-0.1"

/* ---- Thời gian - nhiệt độ ---- */
#define FOOD_TEMP_ZONE_LOW_C   5.0f
#define FOOD_TEMP_ZONE_HIGH_C  60.0f
#define FOOD_HOT_AMBIENT_C     32.0f
#define FOOD_BUDGET_WARN_S     (60 * 60)       /* đã dùng 60 phút */
#define FOOD_BUDGET_EXPIRED_S  (120 * 60)      /* hết quỹ 120 phút */
#define FOOD_AGE_WARN_S        (20 * 3600)
#define FOOD_AGE_EXPIRED_S     (24 * 3600)

/* ---- Vùng trễ khi hạ mức (điểm S) ---- */
#define FOOD_HYSTERESIS        3

/* ---- Lấy mẫu nền ---- */
#define FOOD_BASELINE_SAMPLES  10
#define FOOD_BASE_STABLE_S     120      /* đã nguội ổn định bao lâu thì bắt đầu lấy nền */
#define FOOD_BASE_WAIT_MAX_S   3600     /* chờ nguội tối đa, quá thì lấy nền luôn */

/* ---- Trạng thái nhiệt (nhiệt độ KHÔNG KHÍ trong hộp, cần hiệu chỉnh bằng thực nghiệm) ---- */
#define FOOD_THERMAL_HOT_C          45.0f   /* >= : đang nóng */
#define FOOD_THERMAL_COLD_C         10.0f   /* <  : lạnh */
#define FOOD_THERMAL_COOLING_CPM   (-0.15f) /* độ dốc <= : đang nguội (°C/phút) */
#define FOOD_THERMAL_HEATING_CPM    0.30f   /* độ dốc >= : vừa cho đồ nóng vào, đang nóng lên */
#define FOOD_THERMAL_WINDOW_MIN     3       /* tính độ dốc trên 3 phút gần nhất */

/* ---- Khoảng chuẩn hoá [lo, hi] cho từng kênh ---- */
#define FOOD_NH3_LO  1.10f
#define FOOD_NH3_HI  2.00f
#define FOOD_H2S_LO  1.10f
#define FOOD_H2S_HI  2.00f
#define FOOD_VOC_LO  300.0f
#define FOOD_VOC_HI  3000.0f
#define FOOD_CO2_LO  100.0f
#define FOOD_CO2_HI  1200.0f   /* SCD40 chỉ đảm bảo sai số tới 2000 ppm */

/* ---- Thang thanh hiển thị OLED (chỉ để vẽ) ---- */
#define FOOD_NH3_BAR_MAX       50.0f
#define FOOD_H2S_BAR_MAX       10.0f
#define FOOD_NH3_WARN_PPM      10.0f
#define FOOD_NH3_DANGER_PPM    25.0f
#define FOOD_H2S_WARN_PPM      1.0f
#define FOOD_H2S_DANGER_PPM    5.0f
#define FOOD_CO2_BAR_MIN       400.0f
#define FOOD_CO2_BAR_MAX       2000.0f
#define FOOD_CO2_WARN_PPM      1000.0f
#define FOOD_CO2_DANGER_PPM    1500.0f
#define FOOD_VOC_BAR_MAX       100.0f
#define FOOD_VOC_WARN_REL      40.0f
#define FOOD_VOC_DANGER_REL    70.0f
#define FOOD_TEMP_BAR_MIN      0.0f
#define FOOD_TEMP_BAR_MAX      60.0f

typedef struct {
    float   w_nh3, w_h2s, w_voc, w_co2;
    uint8_t t1, t2;
} food_profile_t;

const food_profile_t *food_eval_profile(food_group_t g);

void food_eval_init(void);                         /* đọc phiên đã lưu trong NVS */
void food_eval_update(const food_sensor_data_t *d); /* gọi mỗi lần có mẫu mới */
void food_eval_new_session(food_group_t food);     /* "Hộp mới" */
void food_eval_set_food(food_group_t food);
void food_eval_get_session(food_session_t *out);
void food_eval_save(void);                         /* lưu NVS (tự gọi định kỳ) */

/* Đánh giá cục bộ theo rule-0.1 */
void food_eval_local(const food_sensor_data_t *d, food_safety_t *out);

#ifdef __cplusplus
}
#endif