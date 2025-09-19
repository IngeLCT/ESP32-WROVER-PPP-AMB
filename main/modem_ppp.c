#include "modem_ppp.h"
#include <string.h>
#include <inttypes.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "lwip/inet.h"      // ipaddr_addr
#include "esp_modem_api.h"

/* ==== HTTP (UnwiredLabs) ==== */
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "Privado.h"  // define UNWIREDLABS_TOKEN

#ifndef UNWIREDLABS_TOKEN
# define UNWIREDLABS_TOKEN ""
#endif

#ifndef UNWIRED_URL
# define UNWIRED_URL "https://us1.unwiredlabs.com/v2/process.php"
#endif
/* ============================ */

static const char *TAG = "modem_ppp";
static EventGroupHandle_t s_ppp_eg;
#define PPP_UP_BIT  BIT0

/* ===== UE info global ===== */
static modem_ue_info_t s_ue_info;   // última UE info válida (CPSI)
static bool            s_ue_valid;  // hay UE info válida

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "PPP UP  ip=" IPSTR " gw=" IPSTR, IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        xEventGroupSetBits(s_ppp_eg, PPP_UP_BIT);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP DOWN");
    }
}

/* Encendido estable para A7670/SIM7600 */
static void hw_boot(const modem_ppp_config_t *c) {
    if (c->board_power_io >= 0) {
        gpio_set_direction(c->board_power_io, GPIO_MODE_OUTPUT);
        gpio_set_level(c->board_power_io, 1);
    }
    if (c->rst_io >= 0) {
        gpio_set_direction(c->rst_io, GPIO_MODE_OUTPUT);
        gpio_set_level(c->rst_io, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(c->rst_io, 1);
        vTaskDelay(pdMS_TO_TICKS(2600));
        gpio_set_level(c->rst_io, 0);
    }
    if (c->dtr_io >= 0) {
        gpio_set_direction(c->dtr_io, GPIO_MODE_OUTPUT);
        gpio_set_level(c->dtr_io, 0);
    }
    if (c->pwrkey_io >= 0) {
        gpio_set_direction(c->pwrkey_io, GPIO_MODE_OUTPUT);
        gpio_set_level(c->pwrkey_io, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(c->pwrkey_io, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(c->pwrkey_io, 1);
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
}

/* Acumulador para esp_modem_command() */
typedef struct { char *buf; size_t size; size_t used; } at_acc_t;
static at_acc_t s_acc;

static esp_err_t at_acc_cb(uint8_t *data, size_t len) {
    size_t n = (len < (s_acc.size - s_acc.used - 1)) ? len : (s_acc.size - s_acc.used - 1);
    if (n > 0 && s_acc.buf) {
        memcpy(s_acc.buf + s_acc.used, data, n);
        s_acc.used += n;
        s_acc.buf[s_acc.used] = '\0';
    }
    return ESP_OK;
}

/* Validación de CPSI “completa” (descarta NO SERVICE/valores nulos) */
static bool cpsi_se_ve_valido(const char *s) {
    if (!s || !*s) return false;
    if (strstr(s, "NO SERVICE")) return false;
    if (strstr(s, "000-00"))     return false;  // MCC-MNC nulos
    if (strstr(s, "BAND0"))      return false;  // banda nula/transitoria
    if (strstr(s, ",0,0,") || strstr(s, ",00000000,")) return false; // CellID/TAC nulos
    return true;
}

/* +CEREG: <n>,<stat>[,...] ; stat 1(home) / 5(roaming) */
static bool cereg_registrado(const char *rsp) {
    return (rsp && (strstr(rsp, ",1") || strstr(rsp, ",5")));
}

static esp_err_t esperar_cereg(esp_modem_dce_t *dce, int timeout_ms, int poll_ms) {
    int elapsed = 0;
    char out[128] = {0};
    while (elapsed < timeout_ms) {
        esp_err_t err = esp_modem_at(dce, "AT+CEREG?\r", out, 2000);
        if (err == ESP_OK && cereg_registrado(out)) {
            ESP_LOGI(TAG, "CEREG OK: %s", out);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(poll_ms));
        elapsed += poll_ms;
    }
    ESP_LOGW(TAG, "CEREG no llegó a registrado en %d ms (último: %s)", timeout_ms, out);
    return ESP_ERR_TIMEOUT;
}

/* AT+CPSI? con reintentos/backoff (máx ~9s por manual; damos 12s) */
static esp_err_t cpsi_con_reintentos(esp_modem_dce_t *dce,
                                     char *out_final, size_t out_len,
                                     int intentos, int delay_ms)
{
    char out[256] = {0};
    esp_err_t last = ESP_FAIL;

    for (int i = 0; i < intentos; ++i) {
        memset(out, 0, sizeof(out));
        last = esp_modem_at(dce, "AT+CPSI?\r", out, 12000);
        if (last == ESP_OK && cpsi_se_ve_valido(out)) {
            if (out_final && out_len) strlcpy(out_final, out, out_len);
            ESP_LOGI(TAG, "CPSI intento %d/%d OK: %s", i+1, intentos, out);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "CPSI intento %d/%d %s: %s",
                 i+1, intentos, (last==ESP_OK ? "inválido" : esp_err_to_name(last)), out);
        vTaskDelay(pdMS_TO_TICKS(delay_ms + i*delay_ms)); // backoff lineal
    }
    if (out_final && out_len) strlcpy(out_final, out, out_len);
    return last == ESP_OK ? ESP_FAIL : last;
}

/* Enviar AT y loguear (agrega \r, asegura COMMAND) */
esp_err_t modem_send_at_and_log(esp_modem_dce_t *dce, const char *cmd, int timeout_ms)
{
    char out[256] = {0};
    char cmd_cr[64];
    size_t n = strlen(cmd);
    if (n > 0 && cmd[n-1] == '\r') strlcpy(cmd_cr, cmd, sizeof(cmd_cr));
    else                           snprintf(cmd_cr, sizeof(cmd_cr), "%s\r", cmd);

    (void)esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);

    esp_err_t err = esp_modem_at(dce, cmd_cr, out, timeout_ms);
    if (err == ESP_OK) ESP_LOGI(TAG, "AT '%s' OK. Respuesta: %s", cmd, out);
    else               ESP_LOGE(TAG, "AT '%s' FAIL (%s). Última línea: %s", cmd, esp_err_to_name(err), out);
    return err;
}

/* Cambia de modo sólo si es necesario */
static esp_err_t set_mode_if_needed(esp_modem_dce_t *dce, esp_modem_dce_mode_t target)
{
    esp_modem_dce_mode_t cur = esp_modem_get_mode(dce);
    if (cur == target) return ESP_OK;
    return esp_modem_set_mode(dce, target);
}

/* ===== Parser C de +CPSI (LTE): extrae MCC, MNC, TAC, ECI =====
 * Ejemplo real:
 * +CPSI: LTE,Online,334-20,0x232,43790378,55,EUTRAN-BAND5,2560,...
 *       idx:  0    1     2      3       4
 */
static char *trim_ws(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n-1])) s[--n] = '\0';
    return s;
}

static bool parse_cpsi_line(const char *line_in, modem_ue_info_t *out)
{
    if (!line_in || !out) return false;
    memset(out, 0, sizeof(*out));
    out->valid = false;

    char buf[256];
    strlcpy(buf, line_in, sizeof(buf));

    char *s = buf;
    char *pfx = strstr(buf, "+CPSI:");
    if (pfx) s = pfx + 6; // salta el prefijo

    int tokenId = 0;
    char *save = NULL;
    for (char *tok = strtok_r(s, ",", &save); tok; tok = strtok_r(NULL, ",", &save), tokenId++) {
        tok = trim_ws(tok);
        if (tokenId == 2) {  // "<MCC>-<MNC>"
            char *dash = strchr(tok, '-');
            if (!dash) return false;
            *dash = '\0';
            out->mcc = atoi(trim_ws(tok));
            out->mnc = atoi(trim_ws(dash + 1));
        } else if (tokenId == 3) { // TAC (hex o dec)
            out->tac = (uint32_t)strtoul(tok, NULL, 0);
        } else if (tokenId == 4) { // SCellID (ECI)
            out->cell_id = (uint32_t)strtoul(tok, NULL, 0);
            break; // ya tenemos lo que queremos
        }
    }

    if (out->mcc < 100 || out->mcc > 999) return false;
    if (out->mnc < 0   || out->mnc > 999) return false;
    out->valid = true;
    return true;
}

/* ===== API pública ===== */
bool modem_get_ue_info(modem_ue_info_t *out)
{
    if (!out) return false;
    *out = s_ue_info;
    return s_ue_valid;
}

/* ===================== UnwiredLabs (HTTP/S) ===================== */
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} resp_buf_t;

static esp_err_t http_evt(esp_http_client_event_t *evt) {
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb && evt->data && evt->data_len && rb->buf) {
        size_t n = evt->data_len;
        if (n > rb->cap - rb->len - 1) n = rb->cap - rb->len - 1;
        if (n > 0) {
            memcpy(rb->buf + rb->len, evt->data, n);
            rb->len += n;
            rb->buf[rb->len] = '\0';
        }
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

/* POST a UL usando s_ue_info. Solo city/state, sin fecha/hora. */
esp_err_t modem_unwiredlabs_city_state(char *city, size_t city_len,
                                       char *state, size_t state_len)
{
    if (city && city_len)  city[0]  = '\0';
    if (state && state_len) state[0] = '\0';

    if (!s_ue_valid) {
        ESP_LOGW(TAG, "UE info inválida; ejecuta CPSI primero");
        return ESP_FAIL;
    }
    if (UNWIREDLABS_TOKEN[0] == '\0') {
        ESP_LOGE(TAG, "UNWIREDLABS_TOKEN vacío (defínelo en Privado.h)");
        return ESP_ERR_INVALID_ARG;
    }

    /* UL usa 'lac' para TAC y 'cid' para ECI en LTE. address=2 pide address_detail. */
    char body[256];
    int blen = snprintf(body, sizeof(body),
        "{\"token\":\"%s\",\"radio\":\"lte\",\"mcc\":%d,\"mnc\":%d,"
        "\"cells\":[{\"lac\":%u,\"cid\":%u}],\"address\":2}",
        UNWIREDLABS_TOKEN, s_ue_info.mcc, s_ue_info.mnc,
        (unsigned)s_ue_info.tac, (unsigned)s_ue_info.cell_id);
    if (blen <= 0 || blen >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "Payload JSON demasiado grande");
        return ESP_FAIL;
    }

    char resp[2048] = {0};
    resp_buf_t rb = { .buf = resp, .cap = sizeof(resp), .len = 0 };

    esp_http_client_config_t cfg = {
        .url = UNWIRED_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .event_handler = http_evt,
        .user_data = &rb,
        .disable_auto_redirect = false,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,  // bundle de CAs de IDF
        .buffer_size = 1024,
        .buffer_size_tx = 512,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG, "No se pudo crear http client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, body, blen);

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    int64_t clen = esp_http_client_get_content_length(cli);
    ESP_LOGI(TAG, "UL HTTP %d, len=%lld", status, (long long)clen);
    ESP_LOGI(TAG, "UL Body: %.*s", (int)rb.len, resp);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status != 200 || rb.len == 0) {
        ESP_LOGW(TAG, "Fallo HTTP o respuesta vacía (err=%s, status=%d, len=%u)",
                 esp_err_to_name(err), status, (unsigned)rb.len);
        return (err != ESP_OK) ? err : ESP_FAIL;
    }

    /* Verifica status JSON */
    char api_status[16] = "";
    if (json_get_string(resp, "status", api_status, sizeof(api_status))) {
        if (strcmp(api_status, "ok") != 0 && strcmp(api_status, "OK") != 0) {
            ESP_LOGW(TAG, "API status='%s'", api_status);
            return ESP_FAIL;
        }
    }

    /* city/state: a veces vienen dentro de "address" o "address_detail" */
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

    if ((city && city[0]) || (state && state[0])) return ESP_OK;
    ESP_LOGW(TAG, "Sin address.city/state en respuesta");
    return ESP_FAIL;
}
/* =================== /UnwiredLabs (HTTP/S) ===================== */

/* ===== Arranque PPP bloqueante ===== */
esp_err_t modem_ppp_start_blocking(const modem_ppp_config_t *cfg,
                                   int timeout_ms,
                                   esp_modem_dce_t **out_dce)
{
    if (!cfg || !cfg->apn) return ESP_ERR_INVALID_ARG;

    esp_netif_config_t nc = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *ppp = esp_netif_new(&nc);
    if (!ppp) return ESP_FAIL;

    esp_netif_set_default_netif(ppp);

    if (!s_ppp_eg) s_ppp_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));

    hw_boot(cfg);

    /* DTE */
    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.port_num   = UART_NUM_1;
    dte_cfg.uart_config.baud_rate  = 115200;
    dte_cfg.uart_config.tx_io_num  = cfg->tx_io;
    dte_cfg.uart_config.rx_io_num  = cfg->rx_io;
    dte_cfg.uart_config.rts_io_num = cfg->rts_io;
    dte_cfg.uart_config.cts_io_num = cfg->cts_io;
    dte_cfg.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;

    /* DCE SIM7600 (A7670 compatible) */
    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(cfg->apn);
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_cfg, &dce_cfg, ppp);
    if (!dce) return ESP_FAIL;
    if (out_dce) *out_dce = dce;

    ESP_ERROR_CHECK(esp_modem_set_apn(dce, cfg->apn));

    /* 1) Asegurar COMMAND, diagnóstico y registro */
    ESP_ERROR_CHECK(set_mode_if_needed(dce, ESP_MODEM_MODE_COMMAND));
    modem_send_at_and_log(dce, "AT",        3000);
    modem_send_at_and_log(dce, "ATE0",      3000);
    modem_send_at_and_log(dce, "AT+CMEE=2", 3000);
    (void)esperar_cereg(dce, 15000, 500);   // opcional pero recomendado en LTE

    /* 2) Obtener CPSI con reintentos y parsearlo */
    char cpsi[128] = {0};
    esp_err_t cpsi_ok = cpsi_con_reintentos(dce, cpsi, sizeof(cpsi), 3, 700);
    if (cpsi_ok == ESP_OK) {
        modem_ue_info_t info;
        if (parse_cpsi_line(cpsi, &info) && cpsi_se_ve_valido(cpsi)) {
            s_ue_info   = info;
            s_ue_valid  = true;
            ESP_LOGI(TAG, "UE: MCC=%d MNC=%d TAC=%" PRIu32 " ECI=%" PRIu32,
                     info.mcc, info.mnc, info.tac, info.cell_id);
        } else {
            s_ue_valid = false;
            ESP_LOGW(TAG, "CPSI válido pero parseo incompleto: %s", cpsi);
        }
    } else {
        s_ue_valid = false;
        ESP_LOGW(TAG, "CPSI no confiable tras reintentos: %s", cpsi);
    }

    /* 3) Handshake corto por esp_modem_command (sin re-entrar COMMAND) */
    char at_rsp[64] = {0};
    s_acc = (at_acc_t){ .buf = at_rsp, .size = sizeof(at_rsp), .used = 0 };
    esp_err_t at_ok = esp_modem_command(dce, "AT\r", at_acc_cb, 1000);
    if (at_ok != ESP_OK) {
        ESP_LOGE(TAG, "El módem no responde a AT. rsp='%s' (verifica PWRKEY/baud/TX-RX/GND)", at_rsp);
        return at_ok;
    }
    (void)esp_modem_command(dce, "ATE0\r", at_acc_cb, 1000);
    vTaskDelay(pdMS_TO_TICKS(500));

    /* 4) DATA/PPP (o CMUX si lo pides) */
    esp_err_t mode_err = set_mode_if_needed(dce, cfg->use_cmux ? ESP_MODEM_MODE_CMUX
                                                               : ESP_MODEM_MODE_DATA);
    if (mode_err != ESP_OK && cfg->use_cmux) {
        ESP_LOGW(TAG, "set_mode(CMUX) falló: %s; reintentando DATA…", esp_err_to_name(mode_err));
        mode_err = set_mode_if_needed(dce, ESP_MODEM_MODE_DATA);
    }
    ESP_ERROR_CHECK(mode_err);

    ESP_LOGI(TAG, "Esperando IP PPP (%d ms)…", timeout_ms);
    EventBits_t b = xEventGroupWaitBits(s_ppp_eg, PPP_UP_BIT, pdFALSE, pdTRUE,
                                        timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY);
    if (!(b & PPP_UP_BIT)) {
        ESP_LOGE(TAG, "Timeout esperando IP PPP");
        return ESP_ERR_TIMEOUT;
    }

    /* DNS fallback si viene vacío */
    esp_netif_dns_info_t dmain = {0}, dbackup = {0};
    esp_netif_get_dns_info(ppp, ESP_NETIF_DNS_MAIN, &dmain);
    esp_netif_get_dns_info(ppp, ESP_NETIF_DNS_BACKUP, &dbackup);
    bool dns_ok = (dmain.ip.type == ESP_IPADDR_TYPE_V4 && dmain.ip.u_addr.ip4.addr != 0) ||
                  (dbackup.ip.type == ESP_IPADDR_TYPE_V4 && dbackup.ip.u_addr.ip4.addr != 0);
    if (!dns_ok) {
        ESP_LOGW(TAG, "DNS del PPP vacío; fijando 1.1.1.1 y 8.8.8.8");
        esp_netif_dns_info_t dns1 = { .ip.type = ESP_IPADDR_TYPE_V4 };
        dns1.ip.u_addr.ip4.addr = ipaddr_addr("1.1.1.1");
        esp_netif_set_dns_info(ppp, ESP_NETIF_DNS_MAIN, &dns1);
        esp_netif_dns_info_t dns2 = { .ip.type = ESP_IPADDR_TYPE_V4 };
        dns2.ip.u_addr.ip4.addr = ipaddr_addr("8.8.8.8");
        esp_netif_set_dns_info(ppp, ESP_NETIF_DNS_BACKUP, &dns2);
    } else {
        ESP_LOGI(TAG, "DNS PPP: main=%" PRIu32 " backup=%" PRIu32,
                 dmain.ip.u_addr.ip4.addr, dbackup.ip.u_addr.ip4.addr);
    }

    return ESP_OK;
}
