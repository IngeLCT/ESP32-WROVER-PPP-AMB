#include "modem_ppp.h"

#include <string.h>         // memcpy
#include <inttypes.h>       // PRIu32
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
        gpio_set_level(c->rst_io,0); // inactivo
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(c->rst_io, 1 ); // activo
        vTaskDelay(pdMS_TO_TICKS(2600));
        gpio_set_level(c->rst_io,0); // inactivo
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
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(c->pwrkey_io, 1);
    }

    // Espera a que arranque el módem (banner + “Call Ready”)
    vTaskDelay(pdMS_TO_TICKS(3000));
}

/* Pequeño acumulador para respuestas AT (por si quieres loguearlas) */
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

static bool cpsi_se_ve_valido(const char *s) {
    if (!s || !*s) return false;
    // Casos a descartar (lo que viste cuando aún no está listo):
    if (strstr(s, "NO SERVICE")) return false;
    if (strstr(s, "000-00"))     return false;  // MCC-MNC nulos
    if (strstr(s, "BAND0"))      return false;  // banda nula/transitoria
    if (strstr(s, ",0,0,") || strstr(s, ",00000000,")) return false; // CellID/TAC nulos
    return true;
}

static bool cereg_registrado(const char *rsp) {
    // Formato típico: +CEREG: <n>,<stat>[,...] ; stat=1 (home) o 5 (roaming)
    return (rsp && (strstr(rsp, ",1") || strstr(rsp, ",5")));
}

static esp_err_t esperar_cereg(esp_modem_dce_t *dce, int timeout_ms, int poll_ms) {
    int elapsed = 0;
    char out[128] = {0};
    while (elapsed < timeout_ms) {
        // Asegura CR al final:
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

static esp_err_t cpsi_con_reintentos(esp_modem_dce_t *dce,
                                     char *out_final, size_t out_len,
                                     int intentos, int delay_ms)
{
    char out[256] = {0};
    esp_err_t last = ESP_FAIL;

    for (int i = 0; i < intentos; ++i) {
        memset(out, 0, sizeof(out));
        last = esp_modem_at(dce, "AT+CPSI?\r", out, 12000); // 12s > 9000ms (manual)
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

esp_err_t modem_send_at_and_log(esp_modem_dce_t *dce,
                                const char *cmd,
                                int timeout_ms)
{
    char out[256] = {0};

    char cmd_cr[64];
    size_t n = strlen(cmd);
    if (n > 0 && cmd[n-1] == '\r') {
        strlcpy(cmd_cr, cmd, sizeof(cmd_cr));
    } else {
        snprintf(cmd_cr, sizeof(cmd_cr), "%s\r", cmd);
    }

    // Por si acaso, garantiza modo COMANDO
    esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);

    esp_err_t err = esp_modem_at(dce, cmd_cr, out, timeout_ms);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "AT '%s' OK. Respuesta: %s", cmd, out);
    } else {
        ESP_LOGE(TAG, "AT '%s' FAIL (%s). Última línea: %s", cmd, esp_err_to_name(err), out);
    }
    return err;
}

static esp_err_t set_mode_if_needed(esp_modem_dce_t *dce, esp_modem_dce_mode_t target)
{
    esp_modem_dce_mode_t cur = esp_modem_get_mode(dce);   // devuelve el modo actual
    if (cur == target) return ESP_OK;
    return esp_modem_set_mode(dce, target);
}

esp_err_t modem_ppp_start_blocking(const modem_ppp_config_t *cfg,
                                   int timeout_ms,
                                   esp_modem_dce_t **out_dce)
{
    if (!cfg || !cfg->apn) return ESP_ERR_INVALID_ARG;

    // Requiere: esp_netif_init() y esp_event_loop_create_default() ya llamados por la app
    esp_netif_config_t nc = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *ppp = esp_netif_new(&nc);
    if (!ppp) return ESP_FAIL;

    // Hacer PPP la interfaz por defecto (ruta y DNS preferidos)
    esp_netif_set_default_netif(ppp);

    if (!s_ppp_eg) s_ppp_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));

    // Encendido HW del módem (alineado a A7670/SIM7600)
    hw_boot(cfg);

    // DTE (UART del módem)
    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.port_num   = UART_NUM_1;
    dte_cfg.uart_config.baud_rate  = 115200;     // explícito (ajusta si tu módem quedó en otro baud)
    dte_cfg.uart_config.tx_io_num  = cfg->tx_io;
    dte_cfg.uart_config.rx_io_num  = cfg->rx_io;
    dte_cfg.uart_config.rts_io_num = cfg->rts_io;
    dte_cfg.uart_config.cts_io_num = cfg->cts_io;
    dte_cfg.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE; // sin RTS/CTS por ahora

    // DCE SIM7600 (A7670 compatible)
    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(cfg->apn);
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_cfg, &dce_cfg, ppp);
    if (!dce) return ESP_FAIL;
    if (out_dce) *out_dce = dce;

    // APN explícito
    ESP_ERROR_CHECK(esp_modem_set_apn(dce, cfg->apn));

    // ====== CAMBIO 1: Asegurar COMMAND antes de cualquier AT ======
    ESP_ERROR_CHECK(set_mode_if_needed(dce, ESP_MODEM_MODE_COMMAND));

    // ====== AT de verificación (con \r) ======
    modem_send_at_and_log(dce, "AT",        3000);
    modem_send_at_and_log(dce, "ATE0",      3000);
    modem_send_at_and_log(dce, "AT+CMEE=2", 3000);

    // Espera opcional a registro (reduce CPSI "vacío")
    (void)esperar_cereg(dce, /*timeout_ms=*/15000, /*poll_ms=*/500);

    // CPSI con reintentos
    char cpsi[128] = {0};
    esp_err_t cpsi_ok = cpsi_con_reintentos(dce, cpsi, sizeof(cpsi),
                                            /*intentos=*/3, /*delay_ms=*/700);
    if (cpsi_ok == ESP_OK) {
        ESP_LOGI(TAG, "CPSI final: %s", cpsi);
    } else {
        ESP_LOGW(TAG, "CPSI no confiable tras reintentos: %s", cpsi);
    }

    // ====== CAMBIO 2: Handshake "AT" usando esp_modem_command y sin re-entrar COMMAND ======
    char at_rsp[64] = {0};
    s_acc = (at_acc_t){ .buf = at_rsp, .size = sizeof(at_rsp), .used = 0 };
    esp_err_t at_ok = esp_modem_command(dce, "AT\r", at_acc_cb, 1000);
    if (at_ok != ESP_OK) {
        ESP_LOGE(TAG, "El módem no responde a AT. rsp='%s' (verifica PWRKEY/baud/TX-RX/GND)", at_rsp);
        return at_ok;
    }
    (void)esp_modem_command(dce, "ATE0\r", at_acc_cb, 1000);
    vTaskDelay(pdMS_TO_TICKS(500));

    // ====== CAMBIO 3: Entrar a DATA o CMUX usando el guard ======
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

    // Verificar DNS y forzar si viene vacío
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