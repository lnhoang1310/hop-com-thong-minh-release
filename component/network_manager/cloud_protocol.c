/*
 * cloud_protocol.c - Đóng gói / phân tích JSON theo cloud_protocol.h
 */
#include "cloud_protocol.h"
#include "cJSON.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static double round_to(double v, int decimals)
{
    double p = pow(10, decimals);
    return round(v * p) / p;
}

static char *finish(cJSON *root)
{
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return txt;
}

bool food_group_from_code(const char *code, food_group_t *out)
{
    if (!code) return false;
    for (int g = 0; g < FOOD_GROUP_COUNT; g++) {
        if (strcasecmp(code, food_group_to_code((food_group_t)g)) == 0) {
            if (out) *out = (food_group_t)g;
            return true;
        }
    }
    return false;
}

char *cloud_protocol_build_status(const char *ip, int8_t rssi)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "state", "online");
    if (ip && ip[0]) cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "rssi", rssi);
    return finish(root);
}

char *cloud_protocol_build_telemetry(const food_sensor_data_t *d, const food_session_t *s,
                                     const food_safety_t *r)
{
    if (!d || !s || !r) return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "food", food_group_to_code(s->food));
    cJSON_AddNumberToObject(root, "age_s", s->age_s);
    cJSON_AddNumberToObject(root, "budget_s", s->budget_s);

    if (d->valid & SENSOR_VALID_TEMP) cJSON_AddNumberToObject(root, "t", round_to(d->temperature_c, 1));
    if (d->valid & SENSOR_VALID_HUM)  cJSON_AddNumberToObject(root, "rh", round_to(d->humidity_rh, 1));
    if (d->valid & SENSOR_VALID_CO2)  cJSON_AddNumberToObject(root, "co2", d->co2_ppm);
    if (d->valid & SENSOR_VALID_VOC)  cJSON_AddNumberToObject(root, "voc", d->voc_raw);
    if (d->valid & SENSOR_VALID_NH3)  cJSON_AddNumberToObject(root, "nh3", round_to(d->nh3_rs_r0, 4));
    if (d->valid & SENSOR_VALID_H2S)  cJSON_AddNumberToObject(root, "h2s", round_to(d->h2s_rs_r0, 4));

    /* Kết quả tự đánh giá */
    cJSON_AddStringToObject(root, "level", food_level_to_code(r->level));
    cJSON_AddStringToObject(root, "gas_level", food_level_to_code(r->gas_level));
    cJSON_AddStringToObject(root, "time_level", food_time_to_code(r->time_level));
    cJSON_AddNumberToObject(root, "score", r->score);
    cJSON_AddStringToObject(root, "msg", r->message);
    return finish(root);
}

esp_err_t cloud_protocol_parse_cmd(const char *json, size_t len, cloud_cmd_t *out)
{
    if (!json || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return ESP_ERR_INVALID_RESPONSE;

    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(root, "cmd"));
    out->has_food = food_group_from_code(cJSON_GetStringValue(cJSON_GetObjectItem(root, "food")), &out->food);
    esp_err_t ret = ESP_OK;

    if (!cmd) {
        out->type = CLOUD_CMD_UNKNOWN;
    } else if (strcmp(cmd, "new_session") == 0) {
        out->type = CLOUD_CMD_NEW_SESSION;
    } else if (strcmp(cmd, "set_food") == 0) {
        out->type = CLOUD_CMD_SET_FOOD;
        if (!out->has_food) ret = ESP_ERR_INVALID_ARG;
    } else if (strcmp(cmd, "set_interval") == 0) {
        out->type = CLOUD_CMD_SET_INTERVAL;
        const cJSON *s = cJSON_GetObjectItem(root, "s");
        if (cJSON_IsNumber(s) && s->valuedouble >= 5 && s->valuedouble <= 300) {
            out->interval_s = (uint32_t)s->valuedouble;
        } else {
            ret = ESP_ERR_INVALID_ARG;
        }
    } else if (strcmp(cmd, "calibrate_mq") == 0) {
        out->type = CLOUD_CMD_CALIBRATE_MQ;
    } else if (strcmp(cmd, "reboot") == 0) {
        out->type = CLOUD_CMD_REBOOT;
    } else if (strcmp(cmd, "ota") == 0) {
        out->type = CLOUD_CMD_OTA;
        const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
        if (url && strlen(url) < sizeof(out->url)) {
            strlcpy(out->url, url, sizeof(out->url));
        } else {
            ret = ESP_ERR_INVALID_ARG;
        }
    } else {
        out->type = CLOUD_CMD_UNKNOWN;
    }
    cJSON_Delete(root);
    return ret;
}