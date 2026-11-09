/*
 * oled_sh1106.c - Driver SH1106/SSD1306 dùng i2c_master (ESP-IDF 5.x)
 * OLED gắn vào bus I2C dùng chung với cảm biến (i2c_bus.c).
 * Chỉ gửi những trang (8 hàng) có thay đổi: ở 100 kHz, gửi cả màn mất ~90 ms
 * và chiếm bus của cảm biến, nên giảm lượng dữ liệu là cần thiết.
 */
#include "oled_sh1106.h"
#include "app_config.h"
#include "driver/i2c_master.h"
#include "i2c_bus.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "OLED";

#define OLED_CTRL_CMD   0x00
#define OLED_CTRL_DATA  0x40
#define OLED_TIMEOUT_MS 100
#define OLED_RETRY_MS   1000   /* lỗi bus -> chờ 1 s rồi mới thử lại, không chiếm CPU/bus */

#if OLED_CONTROLLER_SH1106
#define OLED_COL_OFFSET 2     /* SH1106 có RAM 132 cột, màn 128 cột nằm giữa */
#else
#define OLED_COL_OFFSET 0
#endif

static i2c_master_dev_handle_t s_dev;
static uint8_t s_shadow[OLED_FB_SIZE];   /* nội dung đang hiển thị trên màn */
static bool s_force_full = true;
static uint32_t s_fail_count;            /* số lần flush lỗi liên tiếp */
static int64_t s_retry_after_us;

static esp_err_t oled_cmds(const uint8_t *cmds, size_t n)
{
    uint8_t buf[1 + 16];
    if (n > 16) return ESP_ERR_INVALID_SIZE;
    buf[0] = OLED_CTRL_CMD;
    memcpy(&buf[1], cmds, n);
    return i2c_master_transmit(s_dev, buf, n + 1, OLED_TIMEOUT_MS);
}

static esp_err_t oled_cmd1(uint8_t c)
{
    return oled_cmds(&c, 1);
}

static esp_err_t oled_cmd2(uint8_t c, uint8_t arg)
{
    uint8_t b[2] = { c, arg };
    return oled_cmds(b, 2);
}

static esp_err_t oled_send_init(void);

esp_err_t oled_init(void)
{
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }
    if (i2c_master_probe(bus, OLED_I2C_ADDRESS, OLED_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "Không tìm thấy OLED tại 0x%02X", OLED_I2C_ADDRESS);
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDRESS,
        .scl_speed_hz = OLED_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }
    err = oled_send_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Lỗi gửi lệnh khởi tạo OLED");
        return ESP_FAIL;
    }
    oled_gfx_clear();
    s_force_full = true;
    oled_flush();
    oled_cmd1(0xAF);                        /* display on */
    ESP_LOGI(TAG, "OLED %s sẵn sàng", OLED_CONTROLLER_SH1106 ? "SH1106" : "SSD1306");
    return ESP_OK;
}

/* Chuỗi lệnh khởi tạo (dùng lại khi OLED bị reset do sụt nguồn) */
static esp_err_t oled_send_init(void)
{
    esp_err_t err;
    const uint8_t seg_remap = OLED_FLIP_180 ? 0xA0 : 0xA1;
    const uint8_t com_scan  = OLED_FLIP_180 ? 0xC0 : 0xC8;

    err  = oled_cmd1(0xAE);                 /* display off */
    err |= oled_cmd2(0xD5, 0x80);           /* clock divide */
    err |= oled_cmd2(0xA8, 0x3F);           /* multiplex 1/64 */
    err |= oled_cmd2(0xD3, 0x00);           /* display offset */
    err |= oled_cmd1(0x40);                 /* start line 0 */
#if OLED_CONTROLLER_SH1106
    err |= oled_cmd2(0xAD, 0x8B);           /* DC-DC on (SH1106) */
#else
    err |= oled_cmd2(0x8D, 0x14);           /* charge pump on (SSD1306) */
    err |= oled_cmd2(0x20, 0x02);           /* page addressing mode */
#endif
    err |= oled_cmd1(seg_remap);
    err |= oled_cmd1(com_scan);
    err |= oled_cmd2(0xDA, 0x12);           /* COM pins */
    err |= oled_cmd2(0x81, 0xCF);           /* contrast */
    err |= oled_cmd2(0xD9, 0xF1);           /* pre-charge */
    err |= oled_cmd2(0xDB, 0x40);           /* VCOMH */
    err |= oled_cmd1(0xA4);                 /* hiển thị theo RAM */
    err |= oled_cmd1(0xA6);                 /* normal (không đảo) */
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

bool oled_is_ready(void)
{
    return s_dev != NULL;
}

static esp_err_t oled_flush_pages(void);

/*
 * Đẩy framebuffer ra màn. Khi bus lỗi (lỏng dây, sụt nguồn làm OLED/cảm biến treo SDA):
 *  - không thử lại ngay mà chờ OLED_RETRY_MS, để task giao diện không bị kẹt liên tục
 *  - gỡ bus (i2c_bus_recover) rồi gửi lại lệnh khởi tạo, vì OLED có thể đã tự reset
 */
esp_err_t oled_flush(void)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    int64_t now = esp_timer_get_time();
    if (s_fail_count && now < s_retry_after_us) return ESP_ERR_INVALID_STATE;

    esp_err_t err = ESP_OK;
    if (s_fail_count) {
        i2c_bus_recover();
        err = oled_send_init();
        if (err == ESP_OK) err = oled_cmd1(0xAF);
        s_force_full = true;
    }
    if (err == ESP_OK) err = oled_flush_pages();

    if (err != ESP_OK) {
        if (s_fail_count == 0) {
            ESP_LOGE(TAG, "OLED mất kết nối (%s) - kiểm tra dây SDA/SCL, nguồn 3V3/GND", esp_err_to_name(err));
        }
        s_fail_count++;
        s_retry_after_us = now + (int64_t)OLED_RETRY_MS * 1000;
        s_force_full = true;
    } else if (s_fail_count) {
        ESP_LOGI(TAG, "OLED hoạt động lại sau %lu lần lỗi", (unsigned long)s_fail_count);
        s_fail_count = 0;
    }
    return err;
}

static esp_err_t oled_flush_pages(void)
{
    const uint8_t *fb = oled_gfx_buffer();
    uint8_t line[1 + OLED_WIDTH];
    line[0] = OLED_CTRL_DATA;
    for (int page = 0; page < OLED_PAGES; page++) {
        const uint8_t *src = &fb[page * OLED_WIDTH];
        if (!s_force_full && memcmp(src, &s_shadow[page * OLED_WIDTH], OLED_WIDTH) == 0) {
            continue;   /* trang không đổi */
        }
        uint8_t addr[3] = {
            (uint8_t)(0xB0 | page),
            (uint8_t)(0x00 | (OLED_COL_OFFSET & 0x0F)),
            (uint8_t)(0x10 | (OLED_COL_OFFSET >> 4)),
        };
        esp_err_t err = oled_cmds(addr, sizeof(addr));
        if (err != ESP_OK) return err;
        memcpy(&line[1], src, OLED_WIDTH);
        err = i2c_master_transmit(s_dev, line, sizeof(line), OLED_TIMEOUT_MS);
        if (err != ESP_OK) {
            s_force_full = true;   /* lỗi bus -> lần sau gửi lại toàn bộ */
            return err;
        }
        memcpy(&s_shadow[page * OLED_WIDTH], src, OLED_WIDTH);
    }
    s_force_full = false;
    return ESP_OK;
}

esp_err_t oled_set_contrast(uint8_t contrast)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return oled_cmd2(0x81, contrast);
}

esp_err_t oled_power(bool on)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return oled_cmd1(on ? 0xAF : 0xAE);
}

esp_err_t oled_invert(bool invert)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return oled_cmd1(invert ? 0xA7 : 0xA6);
}