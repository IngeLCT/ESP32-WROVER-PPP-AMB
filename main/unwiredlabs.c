#include "unwiredlabs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"

static const char *TAG = "unwired";

// Cambia a EU si tu cuenta está ahí.
#define UNWIRED_URL "https://us1.unwiredlabs.com/v2/process.php"

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} resp_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (rb && evt->data_len > 0 && rb->buf && rb->cap > rb->len) {
                size_t n = evt->data_len;
                if (n > rb->cap - rb->len - 1) n = rb->cap - rb->len - 1;
                if (n > 0) {
                    memcpy(rb->buf + rb->len, evt->data, n);
                    rb->len += n;
                    rb->buf[rb->len] = '\0';
                }
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

static bool json_get_string(const char *json, const char *key, char *out, size_t outlen) {
    if (!json || !key || !out || outlen == 0) return false;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return false;
    p += strlen(pat);
    const char *q = strchr(p, '"');
    if (!q || q <= p) return false;
    size_t n = (size_t)(q - p);
    if (n >= outlen) n = outlen - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return true;
}

bool unwiredlabs_geolocate(const char *token,
                           int mcc, int mnc, int tac_lac, int cid,
                           char *city, size_t city_len,
                           char *state, size_t state_len,
                           char *date, size_t date_len,
                           char *time, size_t time_len)
{
    if (city && city_len) city[0] = '\0';
    if (state && state_len) state[0] = '\0';
    if (date && date_len) date[0] = '\0';
    if (time && time_len) time[0] = '\0';

    if (!token || token[0] == '\0') {
        ESP_LOGE(TAG, "Token vacío");
        return false;
    }

    char body[256];
    int blen = snprintf(body, sizeof(body),
        "{\"token\":\"%s\",\"radio\":\"lte\",\"mcc\":%d,\"mnc\":%d,"
        "\"cells\":[{\"lac\":%d,\"cid\":%d,\"psc\":0}],\"address\":2}",
        token, mcc, mnc, tac_lac, cid);
    if (blen <= 0 || blen >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "Payload JSON demasiado grande");
        return false;
    }

    char resp[2048] = {0};
    resp_buf_t rb = { .buf = resp, .cap = sizeof(resp), .len = 0 };

    esp_http_client_config_t cfg = {
        .url = UNWIRED_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .event_handler = http_event_handler,
        .user_data = &rb,
        .disable_auto_redirect = false,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 1024,     // RX interno del cliente
        .buffer_size_tx = 512,   // TX interno del cliente
    };

    heap_caps_check_integrity_all(true);  // <-- pega esto
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG, "No se pudo crear http client");
        return false;
    }

    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, body, blen);

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    int64_t clen = esp_http_client_get_content_length(cli);

    ESP_LOGI(TAG, "HTTP %d, content length=%lld", status, (long long)clen);
    ESP_LOGI(TAG, "Body: %.*s", (int)rb.len, resp);

    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status != 200 || rb.len == 0) {
        ESP_LOGW(TAG, "Fallo HTTP o respuesta vacía (err=%s, status=%d, len=%u)",
                 esp_err_to_name(err), status, (unsigned)rb.len);
        return false;
    }

    char api_status[16] = "";
    if (json_get_string(resp, "status", api_status, sizeof(api_status))) {
        if (strcmp(api_status, "ok") != 0 && strcmp(api_status, "OK") != 0) {
            ESP_LOGW(TAG, "API status='%s'", api_status);
            return false;
        }
    }

    const char *addr = strstr(resp, "\"address\":");
    const char *start = addr ? strchr(addr, '{') : NULL;
    const char *end   = start ? strchr(start, '}') : NULL;

    if (start && end && end > start) {
        char tmp[512] = {0};
        size_t n = (size_t)(end - start + 1);
        if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
        memcpy(tmp, start, n);
        tmp[n] = 0;

        (void)json_get_string(tmp, "city",  city,  city_len);
        (void)json_get_string(tmp, "state", state, state_len);
    } else {
        (void)json_get_string(resp, "city",  city,  city_len);
        (void)json_get_string(resp, "state", state, state_len);
    }

    (void)json_get_string(resp, "date", date, date_len);
    (void)json_get_string(resp, "time", time, time_len);

    bool ok = (city && city[0]) || (state && state[0]);
    if (!ok) ESP_LOGW(TAG, "Sin address.city/state en respuesta");
    return ok;
}
