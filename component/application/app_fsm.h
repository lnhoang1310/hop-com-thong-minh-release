#pragma once
/*
 * app_fsm.h - Máy trạng thái chính của Hộp cơm thông minh
 *
 *   BOOT --> WARMUP (làm nóng MQ) --> SELECT_FOOD (menu chọn món) --> MONITOR <--> CALIBRATE
 *                                          ^-------- UP giữ 3s ---------|
 *
 * Nút bấm (3 nút):
 *   Menu chọn món      : UP / DOWN lên xuống, OK chọn
 *                        (menu lúc khởi động: chọn xong -> phiên mới, lấy nền;
 *                         menu mở lại bằng UP 3s: chỉ đổi món, giữ phiên + nền)
 *   UP / DOWN (nhấn)   : chuyển trang OLED
 *   OK (nhấn)          : về trang Tổng quan | bỏ qua màn hình làm nóng
 *   OK (giữ 3s)        : bật/tắt AP cấu hình WiFi
 *   UP (giữ 3s)        : mở lại menu chọn món
 *   DOWN (giữ 3s)      : "Hộp mới" - bắt đầu phiên đo mới, lấy lại giá trị nền
 *   UP + DOWN (giữ 5s) : hiệu chuẩn MQ (đặt hộp ở không khí sạch)
 * Không có LED/còi: khi mức tăng, OLED tự về trang Tổng quan và hiện thông báo.
 *
 * Thiết bị tự đánh giá (food_eval.h). Mỗi TELEMETRY_INTERVAL_S giây (và ngay khi mức đổi) gửi
 * số đo + kết quả lên server qua MQTT; server chỉ nhận/ghi dữ liệu và có thể gửi lệnh.
 */
#include "esp_err.h"
#include "food_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_STATE_BOOT = 0,
    APP_STATE_WARMUP,
    APP_STATE_MONITOR,
    APP_STATE_CALIBRATE,
    APP_STATE_SELECT_FOOD,
} app_state_t;

/* Khởi tạo hàng đợi sự kiện + task FSM. Gọi sau khi các module khác đã init. */
esp_err_t   app_fsm_start(void);
app_state_t app_fsm_get_state(void);
void        app_fsm_get_safety(food_safety_t *out);

#ifdef __cplusplus
}
#endif