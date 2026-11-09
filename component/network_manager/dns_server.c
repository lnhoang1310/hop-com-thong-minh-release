/*
 * dns_server.c - DNS server tối giản trên UDP 53
 */
#include "dns_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "DNS";

#define DNS_PORT       53
#define DNS_MAX_LEN    512
#define DNS_TYPE_A     1
#define DNS_CLASS_IN   1

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

static TaskHandle_t s_task;
static volatile bool s_running;
static uint32_t s_ip;   /* network byte order */

/* Bỏ qua tên miền trong phần question, trả về con trỏ sau tên (NULL nếu lỗi) */
static const uint8_t *skip_name(const uint8_t *p, const uint8_t *end)
{
    while (p < end) {
        uint8_t len = *p;
        if (len == 0) return p + 1;
        if ((len & 0xC0) == 0xC0) return p + 2;   /* con trỏ nén */
        p += len + 1;
    }
    return NULL;
}

static int build_response(uint8_t *buf, int len)
{
    if (len < (int)sizeof(dns_header_t)) return -1;
    dns_header_t *h = (dns_header_t *)buf;
    if (ntohs(h->flags) & 0x8000) return -1;           /* đây là response, bỏ qua */
    if (ntohs(h->qdcount) != 1) return -1;

    const uint8_t *end = buf + len;
    const uint8_t *q = skip_name(buf + sizeof(dns_header_t), end);
    if (!q || q + 4 > end) return -1;
    uint16_t qtype = (uint16_t)((q[0] << 8) | q[1]);
    uint16_t qclass = (uint16_t)((q[2] << 8) | q[3]);
    int qend = (int)(q + 4 - buf);

    h->flags = htons(0x8180);        /* response, recursion desired+available, NOERROR */
    h->nscount = 0;
    h->arcount = 0;

    if (qtype != DNS_TYPE_A || qclass != DNS_CLASS_IN) {
        h->ancount = 0;              /* AAAA,... -> không có bản ghi */
        return qend;
    }
    if (qend + 16 > DNS_MAX_LEN) return -1;
    h->ancount = htons(1);
    uint8_t *a = buf + qend;
    a[0] = 0xC0; a[1] = 0x0C;        /* tên: trỏ tới question */
    a[2] = 0x00; a[3] = DNS_TYPE_A;
    a[4] = 0x00; a[5] = DNS_CLASS_IN;
    a[6] = 0x00; a[7] = 0x00; a[8] = 0x00; a[9] = 0x3C;   /* TTL 60s */
    a[10] = 0x00; a[11] = 0x04;
    memcpy(&a[12], &s_ip, 4);
    return qend + 16;
}

static void dns_task(void *arg)
{
    (void)arg;
    uint8_t buf[DNS_MAX_LEN];
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Không tạo được socket");
        goto done;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Không bind được cổng 53");
        close(sock);
        goto done;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(TAG, "DNS captive đang chạy");

    while (s_running) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len <= 0) continue;   /* timeout -> kiểm tra s_running */
        int out = build_response(buf, len);
        if (out > 0) {
            sendto(sock, buf, out, 0, (struct sockaddr *)&from, from_len);
        }
    }
    close(sock);
done:
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dns_server_start(const char *ip)
{
    if (s_task) return ESP_OK;
    struct in_addr a;
    if (!ip || inet_aton(ip, &a) == 0) return ESP_ERR_INVALID_ARG;
    s_ip = a.s_addr;
    s_running = true;
    if (xTaskCreate(dns_task, "dns", 3072, NULL, 4, &s_task) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dns_server_stop(void)
{
    s_running = false;
    for (int i = 0; i < 30 && s_task; i++) {   /* chờ task thoát (tối đa ~1.5s) */
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}