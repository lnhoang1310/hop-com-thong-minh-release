#pragma once
/*
 * food_types.h - Kiểu dữ liệu dùng chung giữa cảm biến, FSM, hiển thị và server
 */
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cờ hợp lệ cho từng đại lượng trong food_sensor_data_t.valid */
#define SENSOR_VALID_TEMP   (1u << 0)
#define SENSOR_VALID_HUM    (1u << 1)
#define SENSOR_VALID_CO2    (1u << 2)
#define SENSOR_VALID_VOC    (1u << 3)
#define SENSOR_VALID_NH3    (1u << 4)
#define SENSOR_VALID_H2S    (1u << 5)

typedef struct {
    uint32_t valid;            /* tổ hợp SENSOR_VALID_xxx */
    float    temperature_c;    /* cảm biến nhiệt/ẩm tích hợp trong SCD40 */
    float    humidity_rh;      /* % */
    uint16_t co2_ppm;          /* SCD40 */
    uint16_t voc_raw;          /* SGP40 SRAW (VOC càng cao -> SRAW càng thấp) */
    uint8_t  voc_rel;          /* 0..100 ước lượng tương đối cho hiển thị */
    float    nh3_rs_r0;        /* MQ137: Rs/R0 (khí tăng -> tỉ số giảm) */
    float    h2s_rs_r0;        /* MQ136: Rs/R0 */
    float    nh3_ppm;          /* quy đổi gần đúng từ đường cong datasheet, chỉ để hiển thị */
    float    h2s_ppm;
    bool     mq_warmed_up;     /* MQ đã làm nóng xong */
    uint32_t seq;              /* tăng mỗi lần cập nhật */
    uint32_t timestamp_ms;     /* thời điểm đo (ms từ lúc khởi động) */
} food_sensor_data_t;

/* Nhóm món: mỗi nhóm có bộ trọng số / ngưỡng riêng */
typedef enum {
    FOOD_GROUP_RICE = 0,       /* "com"   */
    FOOD_GROUP_MEAT,           /* "thit"  */
    FOOD_GROUP_FISH,           /* "ca"    */
    FOOD_GROUP_SOUP,           /* "canh"  */
    FOOD_GROUP_EGG,            /* "trung" */
    FOOD_GROUP_COUNT,
} food_group_t;

/* Mức kết quả (theo cách gọi trong báo cáo) */
typedef enum {
    FOOD_LEVEL_UNKNOWN = 0,    /* "invalid": chưa đủ cơ sở đánh giá */
    FOOD_LEVEL_SAFE,           /* "low":     tín hiệu thấp */
    FOOD_LEVEL_WARNING,        /* "check":   cần kiểm tra */
    FOOD_LEVEL_DANGER,         /* "strong":  biến đổi mạnh / quá thời gian an toàn */
} food_level_t;

typedef enum {
    FOOD_TIME_OK = 0,          /* trong quỹ thời gian an toàn */
    FOOD_TIME_WARN,            /* đã dùng >= 50% quỹ, hoặc tuổi >= 20 giờ */
    FOOD_TIME_EXPIRED,         /* hết quỹ 120 phút, hoặc tuổi >= 24 giờ */
} food_time_level_t;

/* Trạng thái nhiệt của thức ăn, suy ra từ nhiệt độ không khí trong hộp và tốc độ thay đổi */
typedef enum {
    FOOD_THERMAL_UNKNOWN = 0,  /* chưa đủ 3 phút số liệu nhiệt độ */
    FOOD_THERMAL_HOT,          /* đang nóng: >= 45 °C hoặc đang nóng lên nhanh (vừa cho đồ nóng vào) */
    FOOD_THERMAL_COOLING,      /* đang nguội: nhiệt độ giảm >= 0,15 °C/phút */
    FOOD_THERMAL_COOLED,       /* đã nguội: nhiệt độ gần như không đổi */
    FOOD_THERMAL_COLD,         /* lạnh: < 10 °C (tủ lạnh) */
} food_thermal_t;

/* Kết quả đánh giá (từ server hoặc tính cục bộ) */
typedef struct {
    food_level_t      level;       /* mức cuối = max(mức khí, mức thời gian) */
    food_level_t      gas_level;
    food_time_level_t time_level;
    uint8_t  score;            /* chỉ số biến đổi khí S: 0..100, càng cao càng biến đổi nhiều */
    bool     from_cloud;       /* true: kết quả từ server, false: tính cục bộ */
    uint32_t seq;              /* seq của bản tin telemetry được đánh giá */
    uint32_t timestamp_ms;     /* thời điểm nhận/tính */
    uint32_t valid_ms;         /* kết quả hết hạn sau ngần này ms */
    char     message[24];      /* mô tả ngắn, ASCII không dấu để hiện trên OLED */
    char     model[16];        /* phiên bản thuật toán, vd "rule-0.1" */
    food_thermal_t thermal;    /* nóng / đang nguội / đã nguội / lạnh */
    float    temp_slope;       /* tốc độ đổi nhiệt độ, °C/phút (NAN nếu chưa đủ số liệu) */
} food_safety_t;

/* Phiên đo = một lần cho thức ăn vào hộp */
typedef struct {
    uint32_t     id;           /* ngẫu nhiên, gửi lên server dạng hex */
    food_group_t food;
    uint32_t     age_s;        /* thời gian kể từ khi bắt đầu phiên */
    uint32_t     zone_s;       /* thời gian thật trong vùng 5..60 °C */
    uint32_t     budget_s;     /* quỹ thời gian đã dùng (x2 khi > 32 °C) */
    bool         base_ready;   /* đã lấy xong giá trị nền */
    float        base_nh3_rs_r0;
    float        base_h2s_rs_r0;
    uint16_t     base_voc_raw;
    uint16_t     base_co2_ppm;
} food_session_t;

static inline const char *food_level_to_str(food_level_t lv)
{
    switch (lv) {
    case FOOD_LEVEL_SAFE:    return "TIN HIEU THAP";
    case FOOD_LEVEL_WARNING: return "CAN KIEM TRA";
    case FOOD_LEVEL_DANGER:  return "BIEN DOI MANH";
    default:                 return "CHUA DU CO SO";
    }
}

/* Mã dùng trong gói tin MQTT */
static inline const char *food_level_to_code(food_level_t lv)
{
    switch (lv) {
    case FOOD_LEVEL_SAFE:    return "low";
    case FOOD_LEVEL_WARNING: return "check";
    case FOOD_LEVEL_DANGER:  return "strong";
    default:                 return "invalid";
    }
}

static inline const char *food_time_to_code(food_time_level_t t)
{
    switch (t) {
    case FOOD_TIME_WARN:    return "warn";
    case FOOD_TIME_EXPIRED: return "expired";
    default:                return "ok";
    }
}

/* Chữ ngắn cho OLED (<= 9 ký tự) */
static inline const char *food_thermal_to_str(food_thermal_t t)
{
    switch (t) {
    case FOOD_THERMAL_HOT:     return "NONG";
    case FOOD_THERMAL_COOLING: return "NGUOI DAN";
    case FOOD_THERMAL_COOLED:  return "DA NGUOI";
    case FOOD_THERMAL_COLD:    return "LANH";
    default:                   return "";
    }
}

/* Mã dùng trong gói tin MQTT */
static inline const char *food_thermal_to_code(food_thermal_t t)
{
    switch (t) {
    case FOOD_THERMAL_HOT:     return "hot";
    case FOOD_THERMAL_COOLING: return "cooling";
    case FOOD_THERMAL_COOLED:  return "cooled";
    case FOOD_THERMAL_COLD:    return "cold";
    default:                   return "unknown";
    }
}

static inline const char *food_group_to_code(food_group_t g)
{
    static const char *const codes[FOOD_GROUP_COUNT] = { "com", "thit", "ca", "canh", "trung" };
    return (g < FOOD_GROUP_COUNT) ? codes[g] : "com";
}

static inline const char *food_group_to_label(food_group_t g)
{
    static const char *const names[FOOD_GROUP_COUNT] = { "COM", "THIT", "CA", "CANH", "TRUNG" };
    return (g < FOOD_GROUP_COUNT) ? names[g] : "COM";
}

/* Tên đầy đủ (không dấu) cho menu chọn món */
static inline const char *food_group_to_name(food_group_t g)
{
    static const char *const names[FOOD_GROUP_COUNT] = {
        "Com / tinh bot", "Thit", "Ca / hai san", "Canh / mon nuoc", "Trung",
    };
    return (g < FOOD_GROUP_COUNT) ? names[g] : names[0];
}

#ifdef __cplusplus
}
#endif