/*
 * ota_update.c - Ghi firmware mới vào phân vùng OTA còn trống
 */
#include "ota.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "OTA";

/* Phần đầu file .bin: header ảnh + header segment đầu + mô tả ứng dụng (tên project, phiên bản) */
#define HDR_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static volatile ota_state_t s_state;
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static size_t s_total, s_written;
static uint8_t s_hdr[HDR_LEN];
static bool s_hdr_checked;
static char s_msg[24] = "";
static char s_url[256];   /* URL ban đầu; khi chuyển hướng, esp_http_client tự giữ URL mới */

static void set_msg(const char *m)
{
    strlcpy(s_msg, m, sizeof(s_msg));
}

ota_state_t ota_update_state(void)    { return s_state; }
const char *ota_update_message(void)  { return s_msg; }

uint8_t ota_update_progress(void)
{
    if (s_state == OTA_STATE_DONE) return 100;
    if (!s_total) return 0;
    size_t p = s_written * 100 / s_total;
    return (uint8_t)(p > 99 ? 99 : p);
}

bool ota_update_supported(void)
{
    return esp_ota_get_next_update_partition(NULL) != NULL;
}

const char *ota_update_running_partition(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}

void ota_update_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "Firmware mới khởi động tốt -> giữ lại (huỷ quay về bản cũ)");
    }
    const esp_app_desc_t *d = esp_app_get_description();
    ESP_LOGI(TAG, "Đang chạy %s v%s trên phân vùng %s%s", d->project_name, d->version,
             run ? run->label : "?", ota_update_supported() ? "" : " (KHÔNG có phân vùng OTA)");
}

static void fail(const char *log_reason, const char *oled_msg)
{
    ESP_LOGE(TAG, "Cập nhật thất bại: %s", log_reason);
    if (s_handle) {
        esp_ota_abort(s_handle);
        s_handle = 0;
    }
    set_msg(oled_msg);
    s_state = OTA_STATE_FAILED;
}

esp_err_t ota_update_begin(size_t total_size, const char *source)
{
    if (s_state == OTA_STATE_RUNNING || s_state == OTA_STATE_DONE) return ESP_ERR_INVALID_STATE;
    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part) {
        fail("không có phân vùng OTA (đổi Partition Table trong menuconfig)", "Khong co phan vung");
        return ESP_ERR_NOT_FOUND;
    }
    if (total_size > s_part->size) {
        fail("file lớn hơn phân vùng OTA", "File qua lon");
        return ESP_ERR_INVALID_SIZE;
    }
    s_total = total_size;
    s_written = 0;
    s_hdr_checked = false;
    s_handle = 0;
    s_state = OTA_STATE_RUNNING;
    set_msg("Dang nhan file");
    esp_err_t err = esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        fail(esp_err_to_name(err), "Loi bat dau OTA");
        return err;
    }
    ESP_LOGI(TAG, "Bắt đầu cập nhật (%s), %u byte -> phân vùng %s", source ? source : "?",
             (unsigned)total_size, s_part->label);
    return ESP_OK;
}

/* Kiểm tra phần đầu file: đúng định dạng ảnh ESP, đúng chip, đúng tên project */
static esp_err_t check_header(void)
{
    const esp_image_header_t *ih = (const esp_image_header_t *)s_hdr;
    const esp_app_desc_t *nd = (const esp_app_desc_t *)(s_hdr + sizeof(esp_image_header_t) +
                                                       sizeof(esp_image_segment_header_t));
    const esp_app_desc_t *cur = esp_app_get_description();
    if (ih->magic != ESP_IMAGE_HEADER_MAGIC) {
        fail("không phải file firmware ESP (.bin)", "Sai dinh dang file");
        return ESP_ERR_INVALID_ARG;
    }
    if (ih->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        fail("firmware build cho chip khác", "Sai loai chip");
        return ESP_ERR_INVALID_ARG;
    }
    if (nd->magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strncmp(nd->project_name, cur->project_name, sizeof(nd->project_name)) != 0) {
        ESP_LOGE(TAG, "File là project '%.32s', thiết bị là '%s'", nd->project_name, cur->project_name);
        fail("sai project", "Sai firmware");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "Firmware mới: %.32s v%.32s (đang chạy v%s)", nd->project_name, nd->version, cur->version);
    return ESP_OK;
}

esp_err_t ota_update_write(const void *data, size_t len)
{
    if (s_state != OTA_STATE_RUNNING) return ESP_ERR_INVALID_STATE;
    const uint8_t *p = data;
    if (!s_hdr_checked) {
        size_t need = HDR_LEN - s_written;
        size_t n = len < need ? len : need;
        memcpy(s_hdr + s_written, p, n);
        if (s_written + n >= HDR_LEN) {
            if (check_header() != ESP_OK) return ESP_ERR_INVALID_ARG;
            s_hdr_checked = true;
        }
    }
    if (s_part && s_written + len > s_part->size) {
        fail("vượt kích thước phân vùng", "File qua lon");
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = esp_ota_write(s_handle, p, len);
    if (err != ESP_OK) {
        fail(esp_err_to_name(err), "Loi ghi flash");
        return err;
    }
    s_written += len;
    set_msg("Dang ghi firmware");
    return ESP_OK;
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    ESP_LOGW(TAG, "Khởi động lại vào firmware mới");
    esp_restart();
}

esp_err_t ota_update_end(uint32_t reboot_delay_ms)
{
    if (s_state != OTA_STATE_RUNNING) return ESP_ERR_INVALID_STATE;
    if (!s_hdr_checked) {
        fail("file quá ngắn", "File qua ngan");
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_total && s_written != s_total) {
        fail("nhận thiếu dữ liệu", "Thieu du lieu");
        return ESP_ERR_INVALID_SIZE;
    }
    set_msg("Dang kiem tra");
    esp_err_t err = esp_ota_end(s_handle);     /* kiểm tra checksum / SHA256 của ảnh */
    s_handle = 0;
    if (err != ESP_OK) {
        fail(err == ESP_ERR_OTA_VALIDATE_FAILED ? "ảnh firmware hỏng" : esp_err_to_name(err), "File hong");
        return err;
    }
    err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        fail(esp_err_to_name(err), "Loi dat phan vung");
        return err;
    }
    s_state = OTA_STATE_DONE;
    set_msg("Xong! Khoi dong lai");
    ESP_LOGI(TAG, "Ghi xong %u byte vào %s", (unsigned)s_written, s_part->label);
    xTaskCreate(reboot_task, "ota_reboot", 2048, (void *)(uintptr_t)reboot_delay_ms, 5, NULL);
    return ESP_OK;
}

void ota_update_abort(const char *reason)
{
    if (s_state == OTA_STATE_RUNNING) fail(reason ? reason : "huỷ", "Da huy cap nhat");
}

/* ------------------------- tải từ URL ------------------------- */
static bool is_redirect(int code)
{
    return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
}

static void url_task(void *arg)
{
    (void)arg;
    esp_http_client_config_t cfg = {
        .url = s_url,
        .timeout_ms = 20000,
        .buffer_size = 4096,          /* GitHub trả header dài (Location ~700 ký tự) */
        .buffer_size_tx = 1024,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* chỉ dùng khi URL là https */
        .max_redirection_count = 5,
    };
    char *buf = malloc(2048);
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!buf || !c) {
        fail("hết bộ nhớ", "Het bo nho");
        goto out;
    }

    /* Mở URL; gặp chuyển hướng (GitHub Release -> máy chủ chứa file) thì đi theo, tối đa 5 lần */
    int64_t len = -1;
    int code = 0;
    for (int hop = 0; hop <= 5; hop++) {
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Không mở được URL: %s", esp_err_to_name(err));
            fail("không mở được URL (sai địa chỉ, không có Internet hoặc lỗi TLS)", "Khong tai duoc file");
            goto out;
        }
        len = esp_http_client_fetch_headers(c);
        code = esp_http_client_get_status_code(c);
        if (!is_redirect(code)) break;
        if (hop == 5) break;
        ESP_LOGI(TAG, "Chuyển hướng (%d) -> đi theo", code);
        esp_http_client_flush_response(c, NULL);
        esp_http_client_set_redirection(c);
        esp_http_client_close(c);
    }
    if (code != 200) {
        char m[112];
        snprintf(m, sizeof(m), "máy chủ trả mã HTTP %d%s", code,
                 code == 404 ? " (sai đường dẫn hoặc repo đang để private)" : "");
        fail(m, code == 404 ? "Khong thay file (404)" : "Loi tai file");
        goto out;
    }
    s_state = OTA_STATE_IDLE;     /* cho phép begin */
    if (ota_update_begin(len > 0 ? (size_t)len : 0, "url") != ESP_OK) goto out;
    while (1) {
        int r = esp_http_client_read(c, buf, 2048);
        if (r < 0) {
            fail("mất kết nối khi tải", "Mat ket noi");
            goto out;
        }
        if (r == 0) {
            if (esp_http_client_is_complete_data_received(c)) break;
            if (len <= 0) break;        /* không biết độ dài -> coi như hết */
            fail("tải không hết file", "Thieu du lieu");
            goto out;
        }
        if (ota_update_write(buf, (size_t)r) != ESP_OK) goto out;
    }
    ota_update_end(1500);
out:
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(buf);
    vTaskDelete(NULL);
}

esp_err_t ota_update_start_url(const char *url)
{
    if (s_state == OTA_STATE_RUNNING || s_state == OTA_STATE_DONE) return ESP_ERR_INVALID_STATE;
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) ||
        strlen(url) >= sizeof(s_url)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_url, url, sizeof(s_url));
    s_state = OTA_STATE_RUNNING;     /* giữ chỗ, task sẽ begin lại */
    s_total = s_written = 0;
    set_msg("Dang tai file");
    ESP_LOGI(TAG, "Tải firmware từ %s", s_url);
    if (xTaskCreate(url_task, "ota_url", 8192, NULL, 4, NULL) != pdPASS) {
        s_state = OTA_STATE_FAILED;
        set_msg("Het bo nho");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}