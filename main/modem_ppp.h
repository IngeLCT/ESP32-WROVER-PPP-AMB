#pragma once

#include "esp_modem_api.h"
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int tx_io;
    int rx_io;
    int rts_io;
    int cts_io;
    int pwrkey_io;
    int rst_io;
    int board_power_io;
    bool rst_active_low;
    int rst_pulse_ms;
    const char *apn;
    bool use_cmux;
} modem_ppp_config_t;

/**
 * Inicia PPP y espera IP
 */
esp_err_t modem_ppp_start_blocking(const modem_ppp_config_t *cfg,
                                   int timeout_ms,
                                   esp_modem_dce_t **out_dce);

/**
 * Ejecuta AT+CPSI? y devuelve MCC, MNC, TAC, CID
 */
bool modem_geolocate_from_cell(esp_modem_dce_t *dce,
                               int *mcc, int *mnc,
                               int *tac, int *cid);

#ifdef __cplusplus
}
#endif
