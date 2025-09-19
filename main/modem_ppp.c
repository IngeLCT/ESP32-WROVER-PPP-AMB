#include "modem_ppp.h"

#include <string.h>         // memcpy, strncpy
#include <ctype.h>          // isdigit
#include <stdlib.h>         // strtol, atoi
#include <inttypes.h>       // PRIu32
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "lwip/inet.h"      // ipaddr_addr
#include "esp_modem_api.h"
#include "unwiredlabs.h"    // <- tu helper de geolocalización

static const char *TAG = "modem_ppp";
static EventGroupHandle_t s_ppp_eg;
#define PPP_UP_BIT  BIT0

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "PPP UP  ip=" IPSTR " gw=" IPSTR, IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        xEventGroupSetBits(s_ppp_eg, PPP_UP_BIT);
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP DOWN");
    }
}

/* Encendido estable para A7670/SIM7600:
 * - No pulses RST al arranque; déjalo inactivo
 * - PWRKEY activo LOW ~1.2 s para encender
 * - Espera 3 s a que el módem suba
 */
static void hw_boot(const modem_ppp_config_t *c) {
    if (c->board_power_io >= 0) {               // habilita rail 4G
        gpio_set_direction(c->board_power_io, GPIO_MODE_OUTPUT);
        gpio_set_level(c->board_power_io, 1);
    }

    if (c->rst_io >= 0) {                       // NO reset al inicio
        gpio_set_direction(c->rst_io, GPIO_MODE_OUTPUT);
        gpio_set_level(c->rst_io, c->rst_active_low ? 1 : 0); // inactivo
    }

    if (c->dtr_io >= 0) {                       // DTR inactivo
        gpio_set_direction(c->dtr_io, GPIO_MODE_OUTPUT);
        gpio_set_level(c->dtr_io, 0);
    }

    if (c->pwrkey_io >= 0) {                    // PWRKEY LOW ~1.2 s
        gpio_set_direction(c->pwrkey_io, GPIO_MODE_OUTPUT);
        gpio_set_level(c->pwrkey_io, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(c->pwrkey_io, 0);
        vTaskDelay(pdMS_TO_TICKS(1200));
        gpio_set_level(c->pwrkey_io, 1);
    }

    // Espera a que arranque el módem (banner + “Call Ready”)
    vTaskDelay(pdMS_TO_TICKS(3000));
}

/* Acumulador para respuestas AT */
typedef struct { char *buf; size_t size; size_t used; } at_acc_t;
static at_acc_t s_acc;

static esp_err_t at_acc_cb(uint8_t *data, size_t len) {
    if (!s_acc.buf || s_acc.size == 0) return ESP_OK;
    // Calcula restante sin underflow
    size_t remaining = 0;
    if (s_acc.used < s_acc.size) {
        remaining = s_acc.size - 1 - s_acc.used;
    }
    if (remaining == 0 || len == 0) return ESP_OK;
    size_t n = (len > remaining) ? remaining : len;
    memcpy(s_acc.buf + s_acc.used, data, n);
    s_acc.used += n;
    s_acc.buf[s_acc.used] = '\0';
    return ESP_OK;
}

static bool at_cmd(esp_modem_dce_t *dce, const char *cmd,
                   char *out, size_t outlen, uint32_t to_ms)
{
    if (!out || outlen < CONFIG_ESP_MODEM_C_API_STR_MAX) return false;
    memset(out, 0, outlen);
    command_result r = esp_modem_at(dce, cmd, out, (int)to_ms);
    return (r == OK);                 // <-- ¡no boolean-cast directo!
}

/* Parsers de celda */
// +CPSI: LTE,Online,334-020,...,<TAC>,<CID>,...
static bool parse_cpsi_line(const char *line, int *mcc, int *mnc, int *tac, int *cid) {
    if (!line || !strstr(line, "+CPSI")) return false;

    // Extrae MCC-MNC (formato "334-020")
    const char *dash = strstr(line, "-");
    if (!dash) return false;

    const char *p = dash;
    while (p > line && isdigit((unsigned char)p[-1])) p--;

    char mccs[4] = {0}, mncs[4] = {0};
    int n1 = (int)(dash - p);
    if (n1 < 2 || n1 > 3) return false;
    strncpy(mccs, p, n1);

    const char *r = dash + 1;
    int n2 = 0; 
    while (isdigit((unsigned char)r[n2]) && n2 < 3) n2++;
    if (n2 < 2 || n2 > 3) return false;
    strncpy(mncs, r, n2);

    int _mcc = atoi(mccs);
    int _mnc = atoi(mncs);

    // Busca los dos siguientes números grandes como TAC y CID (acepta dec o 0xHEX)
    int got = 0; 
    long vals[2] = {0, 0};
    for (const char *s = r + n2; *s && got < 2; ++s) {
        if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) {
            char *endptr;
            long v = strtol(s, &endptr, 16);
            if (endptr != s) {
                vals[got++] = v;
                s = endptr - 1;
            }
        } else if (isdigit((unsigned char)*s)) {
            char *endptr;
            long v = strtol(s, &endptr, 10);
            vals[got++] = v;
            s = endptr - 1;
        }
    }
    if (got < 2) return false;

    if (mcc) { *mcc = _mcc; }
    if (mnc) { *mnc = _mnc; }
    if (tac) { *tac = (int)vals[0]; }
    if (cid) { *cid = (int)vals[1]; }

    return true;
}

// +COPS?: +COPS: 0,2,"334020",7  (tras AT+COPS=3,2)
static bool parse_cops_numeric(const char *line, int *mcc, int *mnc) {
    if (!line) return false;
    const char *q = strchr(line, '"'); if (!q) return false;
    const char *q2 = strchr(q+1, '"'); if (!q2 || q2-q-1 < 5) return false;
    char op[8]={0}; strncpy(op, q+1, q2-q-1);
    char mccs[4]={0}, mncs[4]={0};
    strncpy(mccs, op, 3);
    strncpy(mncs, op+3, (int)strlen(op)-3);
    if (mcc) *mcc = atoi(mccs);
    if (mnc) *mnc = atoi(mncs);
    return true;
}

// +CEREG?: +CEREG: n,stat,"TAC","CI",...
static bool parse_cereg_line(const char *line, int *tac, int *cid) {
    if (!line) return false;
    const char *q=strchr(line,'"'); if(!q) return false;
    const char *q2=strchr(q+1,'"'); if(!q2) return false;
    const char *q3=strchr(q2+1,'"'); if(!q3) return false;
    const char *q4=strchr(q3+1,'"'); if(!q4) return false;
    char tac_hex[8]={0}, ci_hex[16]={0};
    strncpy(tac_hex, q+1,  q2-q-1);
    strncpy(ci_hex,  q3+1, q4-q3-1);
    if (tac) *tac = (int)strtol(tac_hex, NULL, 16);
    if (cid) *cid = (int)strtol(ci_hex,  NULL, 16);
    return true;
}

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

    // DTE (UART del módem)
    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.port_num   = UART_NUM_1;
    dte_cfg.uart_config.baud_rate  = 115200;     // ajusta si tu módem quedó en otro baud
    dte_cfg.uart_config.tx_io_num  = cfg->tx_io;
    dte_cfg.uart_config.rx_io_num  = cfg->rx_io;
    dte_cfg.uart_config.rts_io_num = cfg->rts_io;
    dte_cfg.uart_config.cts_io_num = cfg->cts_io;
    dte_cfg.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;

    // DCE SIM7600 (A7670 compatible)
    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(cfg->apn);
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_cfg, &dce_cfg, ppp);
    if (!dce) return ESP_FAIL;
    if (out_dce) *out_dce = dce;

    ESP_ERROR_CHECK(esp_modem_set_apn(dce, cfg->apn));

    // COMMAND antes del handshake
    ESP_ERROR_CHECK( esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND) );

    at_cmd(dce, "AT",        buf, sizeof(buf), 5000);
    at_cmd(dce, "ATE0",      buf, sizeof(buf), 5000);
    at_cmd(dce, "AT+CPSI?",  buf, sizeof(buf), 5000);
    at_cmd(dce, "AT+COPS=3,2", buf, sizeof(buf), 2000);
    at_cmd(dce, "AT+COPS?",    buf, sizeof(buf), 3000);
    at_cmd(dce, "AT+CEREG?",   buf, sizeof(buf), 3000);

    // Pasa a DATA (PPP). Con fallback si venía CMUX
    esp_err_t mode_err = esp_modem_set_mode(dce, cfg->use_cmux ? ESP_MODEM_MODE_CMUX
                                                               : ESP_MODEM_MODE_DATA);
    if (mode_err != ESP_OK && cfg->use_cmux) {
        ESP_LOGW(TAG, "set_mode(CMUX) falló: %s; reintentando DATA…", esp_err_to_name(mode_err));
        mode_err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
    }
    ESP_ERROR_CHECK(mode_err);

    ESP_LOGI(TAG, "Esperando IP PPP (%d ms)…", timeout_ms);
    EventBits_t b = xEventGroupWaitBits(s_ppp_eg, PPP_UP_BIT, pdFALSE, pdTRUE,
                                        timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY);
    if (!(b & PPP_UP_BIT)) {
        ESP_LOGE(TAG, "Timeout esperando IP PPP");
        return ESP_ERR_TIMEOUT;
    }

    // DNS fallback si el APN no entregó
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

/* --------- NUEVO: Obtener celda y geolocalizar --------- */

bool modem_get_cell_info(esp_modem_dce_t *dce, int *mcc, int *mnc, int *tac, int *cid) {
    if (!dce) return false;
    char buf[1024];

    // Pausa tráfico PPP para hablar AT sin pelearnos con DATA
    esp_modem_pause_net(dce, true);

    // 1) Intento con CPSI
    if (at_cmd(dce, "AT+CPSI?", buf, sizeof(buf), 5000)) {
        const char *line = strstr(buf, "+CPSI:");
        if (line && parse_cpsi_line(line, mcc, mnc, tac, cid)) {
            esp_modem_pause_net(dce, false);
            ESP_LOGI(TAG, "CPSI: MCC=%d MNC=%d TAC=%d CID=%d", mcc?*mcc:-1, mnc?*mnc:-1, tac?*tac:-1, cid?*cid:-1);
            ESP_LOGI(TAG, "Respuesta: %s", buf);
            ESP_LOGI(TAG, "Respuesta: %s", line);
            return true;
        }
    }

    // 2) Respaldo: COPS numeric (MCC/MNC)
    (void)at_cmd(dce, "AT+COPS=3,2\r", buf, sizeof(buf), 2000);
    if (at_cmd(dce, "AT+COPS?\r", buf, sizeof(buf), 3000)) {
        const char *line = strstr(buf, "+COPS:");
        if (line) parse_cops_numeric(line, mcc, mnc);
    }

    // 3) Respaldo: CEREG? (TAC/CID en hex)
    if (at_cmd(dce, "AT+CEREG?\r", buf, sizeof(buf), 3000)) {
        const char *line = strstr(buf, "+CEREG:");
        if (line) parse_cereg_line(line, tac, cid);
    }

    esp_modem_pause_net(dce, false);
    bool ok = (mcc && mnc && tac && cid && *mcc>0 && *mnc>=0 && *tac>0 && *cid>0);
    if (!ok) ESP_LOGW(TAG, "Cell info incompleta: mcc=%d mnc=%d tac=%d cid=%d",
                      mcc?*mcc:-1, mnc?*mnc:-1, tac?*tac:-1, cid?*cid:-1);
    return ok;
}

bool modem_geolocate_from_cell(esp_modem_dce_t *dce, const char *token,
                               char *city, size_t city_len,
                               char *state, size_t state_len,
                               char *date, size_t date_len,
                               char *time, size_t time_len)
{
    int mcc=0,mnc=0,tac=0,cid=0;
    if (!modem_get_cell_info(dce, &mcc, &mnc, &tac, &cid)) return false;
    bool r = unwiredlabs_geolocate(token, mcc, mnc, tac, cid,
                                   city, city_len, state, state_len, date, date_len, time, time_len);
    if (r) ESP_LOGI(TAG, "Geo (UL): %s, %s (%s %s)", city, state, date, time);
    return r;
}
