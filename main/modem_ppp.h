#pragma once
#include "esp_err.h"
#include "esp_modem_api.h"
#include <stddef.h>
#include <stdbool.h>

typedef struct {
    int tx_io, rx_io, rts_io, cts_io, dtr_io, rst_io, pwrkey_io, board_power_io;
    bool rst_active_low;
    int rst_pulse_ms;
    const char *apn;       // ej: "internet.itelcel.com"
    const char *sim_pin;   // opcional
    bool use_cmux;         // true para CMUX (no recomendado por ahora)
} modem_ppp_config_t;

// Arranca PPP y BLOQUEA hasta obtener IP (o timeout_ms)
esp_err_t modem_ppp_start_blocking(const modem_ppp_config_t *cfg,
                                   int timeout_ms,
                                   esp_modem_dce_t **out_dce);

// --- NUEVO: celda → info y geolocalización ---
bool modem_get_cell_info(esp_modem_dce_t *dce, int *mcc, int *mnc, int *tac, int *cid);

bool modem_geolocate_from_cell(esp_modem_dce_t *dce, const char *token,
                               char *city, size_t city_len,
                               char *state, size_t state_len,
                               char *date, size_t date_len,
                               char *time, size_t time_len);
