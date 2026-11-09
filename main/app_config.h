#pragma once
/* ============================== GPIO ============================== */
#define BUTTON_UP   7
#define BUTTON_DOWN 6
#define BUTTON_OK   5
#define BUTTON_ACTIVE_LEVEL   0   /* nút nối GND, dùng pull-up nội */

/* ============================== I2C ============================== */
#define I2C_MAIN_PORT I2C_NUM_1
#define I2C_MAIN_SDA 9
#define I2C_MAIN_SCL 8
#define I2C_MAIN_FREQ_HZ 100000

#define SGP40_I2C_ADDRESS 0x59
#define SGP40_I2C_PORT I2C_MAIN_PORT
#define SGP40_I2C_SDA I2C_MAIN_SDA
#define SGP40_I2C_SCL I2C_MAIN_SCL

/* SCD40 (driver scd41.c dùng chung cho SCD40/SCD41): CO2 + nhiệt độ + độ ẩm */
#define SCD41_I2C_ADDRESS 0x62
#define SCD41_I2C_PORT I2C_MAIN_PORT
#define SCD41_I2C_SDA I2C_MAIN_SDA
#define SCD41_I2C_SCL I2C_MAIN_SCL

#define OLED_I2C_ADDRESS 0x3C
#define OLED_I2C_PORT I2C_MAIN_PORT
#define OLED_I2C_SDA I2C_MAIN_SDA
#define OLED_I2C_SCL I2C_MAIN_SCL
#define OLED_I2C_FREQ_HZ I2C_MAIN_FREQ_HZ   /* có thể tăng 400000 nếu dây ngắn, có điện trở kéo lên 4.7k */

/* Màn 1.3" thường là SH1106 (lệch cột 2). Nếu là SSD1306 thì đặt 0. */
#define OLED_CONTROLLER_SH1106 1
#define OLED_FLIP_180          0

/* ============================== MQ (analog) ============================== */
#define MQ136_ADC_GPIO 2          /* H2S  - ADC1 */
#define MQ137_ADC_GPIO 1          /* NH3  - ADC1 */

/* Module MQ thường cấp 5V; đầu ra phải qua cầu chia áp trước khi vào ESP32.
 * MQ_DIVIDER_RATIO = Vout_cảm_biến / V_vào_ADC (VD: 10k/20k => 1.5). Không chia => 1.0 */
#define MQ_SUPPLY_VOLTAGE   5.0f
#define MQ_DIVIDER_RATIO    1.5f
#define MQ136_RL_KOHM       10.0f   /* điện trở tải trên module */
#define MQ137_RL_KOHM       10.0f

/* R0 mặc định (kΩ) khi chưa hiệu chuẩn. Hiệu chuẩn: giữ đồng thời UP + DOWN 5 giây trong không khí sạch */
#define MQ136_R0_DEFAULT_KOHM 20.0f
#define MQ137_R0_DEFAULT_KOHM 20.0f
/* Tỉ lệ Rs/R0 trong không khí sạch (theo datasheet) */
#define MQ136_CLEAN_AIR_RATIO 3.6f
#define MQ137_CLEAN_AIR_RATIO 3.6f
/* Đường cong ppm = A * (Rs/R0)^B (xấp xỉ từ datasheet) */
#define MQ136_CURVE_A 36.737f
#define MQ136_CURVE_B (-3.536f)
#define MQ137_CURVE_A 102.2f
#define MQ137_CURVE_B (-2.473f)

#define MQ_WARMUP_TIME_S   200     /* thời gian làm nóng cảm biến MQ khi bật nguồn */

/* ============================== Thời gian ============================== */
#define SENSOR_READ_PERIOD_MS  1000
#define UI_REFRESH_PERIOD_MS   200
#define CLOUD_RESULT_TIMEOUT_MS 60000  /* kết quả server quá cũ -> dùng đánh giá cục bộ */

/* ============================== WiFi ============================== */
#define WIFI_AP_SSID_PREFIX   "HopComThongMinh"   /* SSID: HopCom-XXXX (4 hex cuối MAC) */
#define WIFI_AP_PASSWORD      ""         /* "" = mạng mở; >= 8 ký tự = WPA2 */
#define WIFI_AP_CHANNEL       6
#define WIFI_AP_MAX_CONN      4
#define WIFI_STA_MAX_RETRY    5          /* số lần thử khi kết nối mạng mới từ web */
#define WIFI_PORTAL_AUTO_STOP_S 0        /* tắt AP sau N giây khi đã có mạng và không ai kết nối AP (0 = không tắt) */

/* ============================== MQTT5 / Server ============================== */
#define MQTT_BROKER_URI        "mqtt://broker.hivemq.com:1883"
#define MQTT_USE_TLS           0                            /* 1: kiểm tra chứng chỉ bằng bundle CA của ESP-IDF */
#define MQTT_USERNAME          ""                           /* "" = không đăng nhập */
#define MQTT_PASSWORD          ""
/* Broker công cộng dùng chung với mọi người -> đặt tên riêng để không lẫn với người khác.
 * PHẢI giống tham số --root khi chạy server_mau.py */
#define MQTT_TOPIC_ROOT        "hopcom-khkt2026"            /* topic: <root>/status|telemetry|eval|cmd */
#define MQTT_KEEPALIVE_S       5
#define TELEMETRY_INTERVAL_S   1                          /* chu kỳ gửi số đo (đổi được bằng lệnh set_interval) */
#define FW_VERSION             "1.2.2"