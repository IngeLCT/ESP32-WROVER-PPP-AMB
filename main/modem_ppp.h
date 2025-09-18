#pragma once
#include "esp_err.h"
#include "esp_modem_api.h"

typedef struct {
    int tx_io, rx_io, rts_io, cts_io, dtr_io, rst_io, pwrkey_io, board_power_io;
    bool rst_active_low;
    int rst_pulse_ms;
    const char *apn;       // ej: "internet.itelcel.com"
    const char *sim_pin;   // opcional
    bool use_cmux;         // true para modo mixto (AT + datos)
} modem_ppp_config_t;

// Arranca PPP y BLOQUEA hasta obtener IP (o timeout_ms)
esp_err_t modem_ppp_start_blocking(const modem_ppp_config_t *cfg,
                                   int timeout_ms,
                                   esp_modem_dce_t **out_dce);