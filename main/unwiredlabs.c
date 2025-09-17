#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG_UL = "unwired";

static int month_to_num(const char *abbr)
{
    if (!abbr || strlen(abbr) < 3) return 0;
    const char *m = abbr;
    if (!strncmp(m, "Jan", 3)) return 1;
    if (!strncmp(m, "Feb", 3)) return 2;
    if (!strncmp(m, "Mar", 3)) return 3;
    if (!strncmp(m, "Apr", 3)) return 4;
    if (!strncmp(m, "May", 3)) return 5;
    if (!strncmp(m, "Jun", 3)) return 6;
    if (!strncmp(m, "Jul", 3)) return 7;
    if (!strncmp(m, "Aug", 3)) return 8;
    if (!strncmp(m, "Sep", 3)) return 9;
    if (!strncmp(m, "Oct", 3)) return 10;
    if (!strncmp(m, "Nov", 3)) return 11;
    if (!strncmp(m, "Dec", 3)) return 12;
    return 0;
}

static void format_local_datetime_from_date_header(const char *date_hdr, int tz_offset_hours,
                                                   char *out_date, size_t date_len,
                                                   char *out_time, size_t time_len)
{
    // Expects e.g. "Tue, 19 Aug 2025 17:09:41 GMT"
    if (!date_hdr) return;
    char buf[128];
    strlcpy(buf, date_hdr, sizeof(buf));
    // Skip weekday and comma
    char *p = strchr(buf, ',');
    if (p) p += 2; else p = buf;
    int day = atoi(p);
    // Find month abbr
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    char month_abbr[4] = {0};
    strncpy(month_abbr, p, 3);
    int month = month_to_num(month_abbr);
    // Year
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    int year = atoi(p);
    // Time
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    int hh = atoi(p);
    int mm = 0, ss = 0;
    char *colon = strchr(p, ':');
    if (colon) mm = atoi(colon + 1);
    char *colon2 = colon ? strchr(colon + 1, ':') : NULL;
    if (colon2) ss = atoi(colon2 + 1);

    // Apply tz offset (e.g., -6)
    hh += tz_offset_hours;
    while (hh < 0) { hh += 24; day -= 1; }
    while (hh >= 24) { hh -= 24; day += 1; }
    // Note: Simplified month/day adjust omitted for brevity

    if (out_date && date_len)
        snprintf(out_date, date_len, "%02d-%02d-%04d", day, month, year);
    if (out_time && time_len)
        snprintf(out_time, time_len, "%02d:%02d:%02d", hh, mm, ss);
}

bool unwiredlabs_geolocate(const char *token,
                           int mcc, int mnc, int tac, int cid,
                           char *out_city, size_t city_len,
                           char *out_state, size_t state_len,
                           char *out_date, size_t date_len,
                           char *out_time, size_t time_len)
{
    if (!token || !*token) {
        ESP_LOGE(TAG_UL, "Missing Unwired token");
        return false;
    }

    char json[256];
    snprintf(json, sizeof(json),
             "{\"token\":\"%s\",\"radio\":\"lte\",\"mcc\":%d,\"mnc\":%d,\"cells\":[{\"lac\":%d,\"cid\":%d}],\"address\":2}",
             token, mcc, mnc, tac, cid);

    esp_http_client_config_t cfg = {
        .url = "https://us1.unwiredlabs.com/v2/process.php",
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_UL, "HTTP perform failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int status = esp_http_client_get_status_code(client);
    int64_t cl = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG_UL, "HTTP %d, content length=%lld", status, (long long)cl);

    // Date header
    char *date_hdr = NULL;
    if (esp_http_client_get_header(client, "Date", &date_hdr) == ESP_OK && date_hdr) {
        format_local_datetime_from_date_header(date_hdr, -6, out_date, date_len, out_time, time_len);
    }

    // Read body
    char *body = NULL;
    int buf_sz = (cl > 0 && cl < 8192) ? (int)cl + 1 : 4096;
    body = (char *)calloc(1, buf_sz);
    if (!body) {
        esp_http_client_cleanup(client);
        return false;
    }
    int r = esp_http_client_read_response(client, body, buf_sz - 1);
    if (r < 0) {
        ESP_LOGE(TAG_UL, "read_response error");
        free(body);
        esp_http_client_cleanup(client);
        return false;
    }
    body[r] = 0;
    ESP_LOGI(TAG_UL, "Body: %.*s", r, body);

    // Parse JSON
    bool ok = false;
    cJSON *root = cJSON_ParseWithLength(body, r);
    if (root) {
        cJSON *addr = cJSON_GetObjectItemCaseSensitive(root, "address_detail");
        if (cJSON_IsObject(addr)) {
            cJSON *city = cJSON_GetObjectItemCaseSensitive(addr, "city");
            cJSON *state = cJSON_GetObjectItemCaseSensitive(addr, "state");
            if (cJSON_IsString(city) && cJSON_IsString(state)) {
                if (out_city && city_len) strlcpy(out_city, city->valuestring, city_len);
                if (out_state && state_len) strlcpy(out_state, state->valuestring, state_len);
                ok = true;
            }
        }
        cJSON_Delete(root);
    }

    free(body);
    esp_http_client_cleanup(client);
    return ok;
}








