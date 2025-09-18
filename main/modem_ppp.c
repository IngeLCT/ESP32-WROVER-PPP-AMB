#include "modem_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
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

static void hw_boot(const modem_ppp_config_t *c) {
    // Secuencia igual a tu sketch Arduino
    if (c->board_power_io >= 0) {
        gpio_set_direction(c->board_power_io, GPIO_MODE_OUTPUT);
        gpio_set_level(c->board_power_io, 1);
    }
    if (c->rst_io >= 0) {
        gpio_set_direction(c->rst_io, GPIO_MODE_OUTPUT);
        // pulso de reset
        gpio_set_level(c->rst_io, c->rst_active_low ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(c->rst_io, c->rst_active_low ? 0 : 1);
        vTaskDelay(pdMS_TO_TICKS(c->rst_pulse_ms > 0 ? c->rst_pulse_ms : 200));
        gpio_set_level(c->rst_io, c->rst_active_low ? 1 : 0);
    }
    if (c->dtr_io >= 0) {
        gpio_set_direction(c->dtr_io, GPIO_MODE_OUTPUT);
        gpio_set_level(c->dtr_io, 0);
    }
    if (c->pwrkey_io >= 0) {
        gpio_set_direction(c->pwrkey_io, GPIO_MODE_OUTPUT);
        gpio_set_level(c->pwrkey_io, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(c->pwrkey_io, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(c->pwrkey_io, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t modem_ppp_start_blocking(const modem_ppp_config_t *cfg,
                                   int timeout_ms,
                                   esp_modem_dce_t **out_dce)
{
    if (!cfg || !cfg->apn) return ESP_ERR_INVALID_ARG;

    // Requiere que esp_netif_init() y esp_event_loop_create_default() estén ya llamados por la app
    esp_netif_config_t nc = ESP_NETIF_DEFAULT_PPP();
    esp_netif_t *ppp = esp_netif_new(&nc);
    // Hacer PPP la interfaz por defecto (ruta y DNS preferidos)
    esp_netif_set_default_netif(ppp);
    if (!ppp) return ESP_FAIL;

    if (!s_ppp_eg) s_ppp_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));

    hw_boot(cfg);

    // DTE UART
    esp_modem_dte_config_t dte_cfg = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_cfg.uart_config.port_num   = UART_NUM_1;
    dte_cfg.uart_config.tx_io_num  = cfg->tx_io;
    dte_cfg.uart_config.rx_io_num  = cfg->rx_io;
    dte_cfg.uart_config.rts_io_num = cfg->rts_io;
    dte_cfg.uart_config.cts_io_num = cfg->cts_io;
    dte_cfg.uart_config.flow_control = (cfg->rts_io >= 0 && cfg->cts_io >= 0)
                                       ? ESP_MODEM_FLOW_CONTROL_HW
                                       : ESP_MODEM_FLOW_CONTROL_NONE;

    // DCE SIM7600 (A7670 es compatible)
    esp_modem_dce_config_t dce_cfg = ESP_MODEM_DCE_DEFAULT_CONFIG(cfg->apn);
    esp_modem_dce_t *dce = esp_modem_new_dev(ESP_MODEM_DCE_SIM7600, &dte_cfg, &dce_cfg, ppp);
    if (!dce) return ESP_FAIL;
    if (out_dce) *out_dce = dce;

    // Asocia netif + APN explícito
    ESP_ERROR_CHECK(esp_modem_set_apn(dce, cfg->apn));

    // Modo (CMUX como en tu Arduino, o DATA normal)
    ESP_ERROR_CHECK(esp_modem_set_mode(dce, cfg->use_cmux ? ESP_MODEM_MODE_CMUX : ESP_MODEM_MODE_DATA));
    ESP_LOGI(TAG, "Esperando IP PPP (%d ms)...", timeout_ms);
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
