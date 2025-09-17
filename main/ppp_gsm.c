#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_check.h"

#include "driver/uart.h"
#include "driver/gpio.h"

#include "netif/ppp/pppapi.h"
#include "netif/ppp/ppp.h"
#include "netif/ppp/pppos.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

#include "ppp_gsm_config.h"
#include "ppp_gsm.h"

static const char *TAG = "ppp_gsm";

// Link state bits
#define PPP_CONNECTED_BIT    BIT0
#define PPP_DISCONNECTED_BIT BIT1

static EventGroupHandle_t s_ppp_events;

static const char *ppp_err_to_str(int err)
{
    switch (err) {
    case PPPERR_NONE: return "PPPERR_NONE";
    case PPPERR_PARAM: return "PPPERR_PARAM";
    case PPPERR_OPEN: return "PPPERR_OPEN";
    case PPPERR_DEVICE: return "PPPERR_DEVICE";
    case PPPERR_ALLOC: return "PPPERR_ALLOC";
    case PPPERR_USER: return "PPPERR_USER";
    case PPPERR_CONNECT: return "PPPERR_CONNECT";
    case PPPERR_AUTHFAIL: return "PPPERR_AUTHFAIL";
    case PPPERR_PROTOCOL: return "PPPERR_PROTOCOL";
    case PPPERR_PEERDEAD: return "PPPERR_PEERDEAD";
    case PPPERR_IDLETIMEOUT: return "PPPERR_IDLETIMEOUT";
    case PPPERR_CONNECTTIME: return "PPPERR_CONNECTTIME";
    case PPPERR_LOOPBACK: return "PPPERR_LOOPBACK";
    default: return "PPPERR_UNKNOWN";
    }
}

static ppp_pcb *s_ppp = NULL;
static struct netif s_ppp_netif;
static int s_mcc = -1, s_mnc = -1;
static uint32_t s_tac = 0, s_cid = 0;
static bool s_have_cell = false;

#ifdef PPP_NOTIFY_PHASE
static const char *ppp_phase_to_str(u8_t phase)
{
#if defined(PPP_PHASE_DEAD)
    switch (phase) {
    case PPP_PHASE_DEAD: return "DEAD";
    case PPP_PHASE_INITIALIZE: return "INITIALIZE";
    case PPP_PHASE_ESTABLISH: return "ESTABLISH";
    case PPP_PHASE_AUTHENTICATE: return "AUTHENTICATE";
    case PPP_PHASE_NETWORK: return "NETWORK";
    case PPP_PHASE_RUNNING: return "RUNNING";
    case PPP_PHASE_TERMINATE: return "TERMINATE";
    default: return "UNKNOWN";
    }
#else
    (void)phase;
    return "UNKNOWN";
#endif
}

static void ppp_phase_cb(ppp_pcb *pcb, u8_t phase, void *ctx)
{
    (void)pcb;
    (void)ctx;
    ESP_LOGI(TAG, "PPP phase -> %s (%u)", ppp_phase_to_str(phase), (unsigned)phase);
}
#endif
// ============ GPIO helpers ============
static void gpio_out_init_num(int pin, int level)
{
    if (pin < 0) return;
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level((gpio_num_t)pin, level);
}

static void modem_power_sequence(void)
{
    // Board power enable
    gpio_out_init_num(BOARD_POWERON_GPIO, 1);

    // Reset sequence (active HIGH per Arduino code)
    gpio_out_init_num(PPP_MODEM_RST_GPIO, !MODEM_RESET_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PPP_MODEM_RST_GPIO, MODEM_RESET_LEVEL);
    vTaskDelay(pdMS_TO_TICKS(2600));
    gpio_set_level(PPP_MODEM_RST_GPIO, !MODEM_RESET_LEVEL);

    // DTR low to prevent sleep
    gpio_out_init_num(MODEM_DTR_GPIO, 0);

    // PWRKEY pulse: LOW -> HIGH -> LOW as in Arduino sequence
    gpio_out_init_num(BOARD_PWRKEY_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_PWRKEY_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BOARD_PWRKEY_GPIO, 0);

    // Let modem boot
    vTaskDelay(pdMS_TO_TICKS(PPP_MODEM_BOOT_MS));
}

// ============ UART helpers ============
static esp_err_t uart_setup(void)
{
    uart_config_t cfg = {
        .baud_rate = PPP_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(PPP_UART_NUM, 4 * 1024, 4 * 1024, 0, NULL, 0), TAG, "uart_driver_install");
    ESP_RETURN_ON_ERROR(uart_param_config(PPP_UART_NUM, &cfg), TAG, "uart_param_config");
    ESP_RETURN_ON_ERROR(uart_set_pin(PPP_UART_NUM,
                                     PPP_UART_TX_GPIO, PPP_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart_set_pin");
    return ESP_OK;
}

static void uart_flush_rx(void)
{
    size_t pending = 0;
    uart_get_buffered_data_len(PPP_UART_NUM, &pending);
    if (pending) {
        uint8_t dump[256];
        while (pending) {
            int to_read = pending > sizeof(dump) ? sizeof(dump) : (int)pending;
            int r = uart_read_bytes(PPP_UART_NUM, dump, to_read, pdMS_TO_TICKS(20));
            if (r <= 0) break;
            pending -= r;
        }
    }
}

static bool at_send_and_expect(const char *cmd, const char *expect, uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "> %s", cmd);
    uart_flush_rx();
    if (cmd && *cmd) {
        uart_write_bytes(PPP_UART_NUM, cmd, strlen(cmd));
        uart_write_bytes(PPP_UART_NUM, "\r\n", 2);
    }
    const int AT_LINE_MAX1 = 160;
    char line[161] = {0};
    uint32_t t0 = (uint32_t)xTaskGetTickCount();
    uint32_t to_ticks = pdMS_TO_TICKS(timeout_ms);
    int idx = 0;
    while ((xTaskGetTickCount() - t0) < to_ticks) {
        uint8_t ch;
        int r = uart_read_bytes(PPP_UART_NUM, &ch, 1, pdMS_TO_TICKS(50));
        if (r == 1) {
            if (ch == '\n' || idx >= AT_LINE_MAX1) {
                line[idx] = 0;
                if (idx) {
                    ESP_LOGI(TAG, "< %s", line);
                    if (expect && strstr(line, expect)) {
                        return true;
                    }
                }
                idx = 0;
            } else if (ch != '\r') {
                line[idx++] = (char)ch;
            }
        }
    }
    return false;
}


static bool at_cmd_capture_line(const char *cmd, const char *token, char *out, size_t outlen, uint32_t timeout_ms)
{
    if (!token || !out || outlen == 0) return false;
    uart_flush_rx();
    if (cmd && *cmd) {
        uart_write_bytes(PPP_UART_NUM, cmd, strlen(cmd));
        uart_write_bytes(PPP_UART_NUM, "\r\n", 2);
    }
    const int AT_LINE_MAX2 = 256;
    char line[AT_LINE_MAX2+1];
    int idx = 0;
    uint32_t t0 = (uint32_t)xTaskGetTickCount();
    uint32_t to_ticks = pdMS_TO_TICKS(timeout_ms);
    while ((xTaskGetTickCount() - t0) < to_ticks) {
        uint8_t ch;
        int r = uart_read_bytes(PPP_UART_NUM, &ch, 1, pdMS_TO_TICKS(50));
        if (r == 1) {
            if (ch == '\n' || idx >= AT_LINE_MAX2) {
                line[idx] = 0;
                if (idx) {
                    if (strstr(line, token)) {
                        strncpy(out, line, outlen - 1);
                        out[outlen - 1] = 0;
                        ESP_LOGI(TAG, "< %s", out);
                        return true;
                    }
                }
                idx = 0;
            } else if (ch != '\r') {
                line[idx++] = (char)ch;
            }
        }
    }
    return false;
}

static bool modem_wait_attach(uint32_t timeout_ms)
{
    uint32_t t0 = (uint32_t)xTaskGetTickCount();
    uint32_t to_ticks = pdMS_TO_TICKS(timeout_ms);
    while ((xTaskGetTickCount() - t0) < to_ticks) {
        if (at_send_and_expect("AT+CGATT?", "+CGATT: 1", 1000)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}


static void parse_cpsi_and_store(const char *cpsi)
{
    if (!cpsi) return;
    char buf[256];
    strlcpy(buf, cpsi, sizeof(buf));
    char *save = NULL;
    int tokenId = 0;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save), ++tokenId) {
        while (*tok == ' ') tok++;
        if (tokenId == 2) {
            char *dash = strchr(tok, '-');
            if (dash) {
                *dash = 0;
                s_mcc = atoi(tok);
                s_mnc = atoi(dash + 1);
            }
        } else if (tokenId == 3) {
            s_tac = (uint32_t)strtoul(tok, NULL, 0);
        } else if (tokenId == 4) {
            s_cid = (uint32_t)strtoul(tok, NULL, 10);
        }
    }
    if (s_mcc >= 0 && s_mnc >= 0 && s_cid > 0) {
        s_have_cell = true;
        ESP_LOGI(TAG, "Parsed MCC=%d MNC=%d TAC=%lu CID=%lu", s_mcc, s_mnc, (unsigned long)s_tac, (unsigned long)s_cid);
    } else {
        ESP_LOGW(TAG, "CPSI parse incomplete: %s", cpsi);
    }
}

static void at_log_basic_info(void)
{
    at_send_and_expect("AT+CGMI", "OK", PPP_AT_TIMEOUT_MS); // Manufacturer
    at_send_and_expect("AT+CGMM", "OK", PPP_AT_TIMEOUT_MS); // Model
    at_send_and_expect("AT+CGSN", "OK", PPP_AT_TIMEOUT_MS); // IMEI
    at_send_and_expect("AT+CPSI?", "OK", PPP_AT_TIMEOUT_MS); // Service info
}

// PPP output callback: send data to UART
static u32_t ppp_output_cb(ppp_pcb *pcb, const void *data, u32_t len, void *ctx)
{
    (void)pcb;
    (void)ctx;
    int w = uart_write_bytes(PPP_UART_NUM, (const char *)data, (size_t)len);
    return w > 0 ? (u32_t)w : 0;
}

// PPP link status callback
static void ppp_status_cb(ppp_pcb *pcb, int err_code, void *ctx)
{
    (void)ctx;
    struct netif *pppif = ppp_netif(pcb);
    switch (err_code) {
    case PPPERR_NONE: {
        xEventGroupSetBits(s_ppp_events, PPP_CONNECTED_BIT);
        const ip4_addr_t *ip = netif_ip4_addr(pppif); const ip4_addr_t *gw = netif_ip4_gw(pppif); const ip4_addr_t *nm = netif_ip4_netmask(pppif); ESP_LOGI(TAG, "PPPoS connected: IP=%s GW=%s MASK=%s", ip ? ip4addr_ntoa(ip) : "-", gw ? ip4addr_ntoa(gw) : "-", nm ? ip4addr_ntoa(nm) : "-");
        break;
    }
    default:
        ESP_LOGW(TAG, "PPPoS disconnected: %s (%d)", ppp_err_to_str(err_code), err_code);
        xEventGroupSetBits(s_ppp_events, PPP_DISCONNECTED_BIT);
        break;
    }
}


static void ppp_feed_loop(void *arg)
{
    (void)arg;
    uint8_t *buf = (uint8_t *)malloc(2048);
    if (!buf) {
        ESP_LOGE(TAG, "No mem for PPP RX buf");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Feeding PPP data from UART...");
    for (;;) {
        int r = uart_read_bytes(PPP_UART_NUM, buf, 2048, portMAX_DELAY);
        if (r > 0) {
            pppos_input_tcpip(s_ppp, buf, r);
        }
    }
}

static void ppp_gsm_task(void *arg)
{
    (void)arg;
    ESP_ERROR_CHECK(uart_setup());

    // Apply board-specific power/reset sequence (exactly as Arduino code)
    modem_power_sequence();

    // Basic AT init and echo off
    for (int i = 0; i < 10; ++i) {
        if (at_send_and_expect("AT", "OK", 1000)) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    at_send_and_expect("ATE0", "OK", PPP_AT_TIMEOUT_MS);
    at_send_and_expect("AT+CMEE=2", "OK", PPP_AT_TIMEOUT_MS);
    // SIM PIN (optional)
    if (strlen(PPP_SIM_PIN) > 0) {
        char cpin[64];
        snprintf(cpin, sizeof(cpin), "AT+CPIN=\"%s\"", PPP_SIM_PIN);
        at_send_and_expect(cpin, "OK", PPP_AT_TIMEOUT_MS);
        at_send_and_expect("AT+CPIN?", "READY", 10000);
    }

    // Configure APN
    char cgdcont[128];
    snprintf(cgdcont, sizeof(cgdcont), "AT+CGDCONT=1,\"IP\",\"%s\"", PPP_APN);
    at_send_and_expect(cgdcont, "OK", PPP_AT_TIMEOUT_MS);

    // Align SIMCom data profile with Telcel APN/auth
    char cncfg[160];
    bool cncfg_ok = false;
    snprintf(cncfg, sizeof(cncfg), "AT+CNCFG=0,1,\"%s\",\"%s\",\"%s\"", PPP_APN, PPP_USER, PPP_PASS);
    cncfg_ok = at_send_and_expect(cncfg, "OK", PPP_AT_TIMEOUT_MS);
    if (!cncfg_ok) {
        snprintf(cncfg, sizeof(cncfg), "AT+CNCFG=0,1,\"%s\"", PPP_APN);
        if (!at_send_and_expect(cncfg, "OK", PPP_AT_TIMEOUT_MS)) {
            ESP_LOGW(TAG, "AT+CNCFG failed, continuing without profile");
        }
    }
    if (!at_send_and_expect("AT+CSOCKSETPN=1", "OK", PPP_AT_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "AT+CSOCKSETPN failed");
    }

    // Query and log info
    at_log_basic_info();
    {
        char cpsi_line[256];
        if (at_cmd_capture_line("AT+CPSI?", "+CPSI:", cpsi_line, sizeof(cpsi_line), PPP_AT_TIMEOUT_MS)) {
            parse_cpsi_and_store(cpsi_line);
        }
    }

    // Wait for network attachment like Arduino's PPP.attached()
    bool attached = modem_wait_attach(60000);
    ESP_LOGI(TAG, "Attached: %s", attached ? "yes" : "no");    
    if (!attached) {        
        ESP_LOGE(TAG, "Network attach failed");        
        vTaskDelete(NULL);        
        return;    
    }    
    // Ensure PDP context 1 is active on SIMCom    
    at_send_and_expect("AT+CGACT=1,1", "OK", 10000);    
    ESP_LOGI(TAG, "Switching to data mode...");
    bool entered = false;
    const char *cmd1 = PPP_DIAL;
    const char *cmd2 = (strstr(PPP_DIAL, "CGDATA") != NULL) ? "ATD*99***1#" : "AT+CGDATA=\"PPP\",1";
    ESP_LOGI(TAG, "Dial attempt 1: %s", cmd1);
    if (at_send_and_expect(cmd1, "CONNECT", PPP_CONNECT_TIMEOUT_MS)) {
        entered = true;
    } else {
        ESP_LOGW(TAG, "Dial 1 failed, trying fallback: %s", cmd2);
        if (at_send_and_expect(cmd2, "CONNECT", PPP_CONNECT_TIMEOUT_MS)) {
            entered = true;
        }
    }
    if (!entered) {
        ESP_LOGE(TAG, "No CONNECT from modem. Check SIM/APN/dial.");
        vTaskDelete(NULL);
        return;
    }

    // Create PPP instance and connect
    s_ppp = pppapi_pppos_create(&s_ppp_netif, ppp_output_cb, ppp_status_cb, NULL);
#ifdef PPP_NOTIFY_PHASE
#endif
    if (!s_ppp) {
        ESP_LOGE(TAG, "Failed to create PPP PCB");
        vTaskDelete(NULL);
        return;
    }
    ppp_set_default(s_ppp);
    ppp_set_usepeerdns(s_ppp, 1);
#ifdef PPP_NOTIFY_PHASE
    ppp_set_notify_phase_callback(s_ppp, ppp_phase_cb);
#endif
    ppp_set_auth(s_ppp, PPPAUTHTYPE_PAP, PPP_USER, PPP_PASS);

    // Start feeding task first so PPP has data
    xTaskCreate(ppp_feed_loop, "ppp_feed", 4096, NULL, 20, NULL);
    pppapi_connect(s_ppp, 0);

    // Wait for connected or failure
    EventBits_t bits = xEventGroupWaitBits(s_ppp_events, PPP_CONNECTED_BIT | PPP_DISCONNECTED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(70000));
    if (bits & PPP_CONNECTED_BIT) {
        ESP_LOGI(TAG, "PPP connected.");
    } else {
        ESP_LOGW(TAG, "PPP did not connect in time (bits=%02x)", (unsigned)bits);
    }

    vTaskDelete(NULL);
}

void ppp_gsm_start(void)
{
    if (!s_ppp_events) s_ppp_events = xEventGroupCreate();
    xTaskCreate(ppp_gsm_task, "ppp_gsm", 6144, NULL, 10, NULL);
}

bool ppp_gsm_wait_connected(uint32_t timeout_ms)
{
    if (!s_ppp_events) return false;
    EventBits_t bits = xEventGroupWaitBits(s_ppp_events, PPP_CONNECTED_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & PPP_CONNECTED_BIT) != 0;
}

bool ppp_gsm_get_cell_info(ppp_cell_info_t *out)
{
    if (!out || !s_have_cell) return false;
    out->mcc = s_mcc;
    out->mnc = s_mnc;
    out->tac = s_tac;
    out->cell_id = s_cid;
    return true;
}







