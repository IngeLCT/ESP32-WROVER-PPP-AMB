#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ppp_gsm_start(void);

// Wait until PPP is connected or timeout (ms). Returns true if connected.
bool ppp_gsm_wait_connected(uint32_t timeout_ms);

// Cell info captured from AT+CPSI? before entering PPP data mode
typedef struct {
    int mcc;
    int mnc;
    uint32_t tac;     // Tracking Area Code (decimal)
    uint32_t cell_id; // Cell ID (decimal)
} ppp_cell_info_t;

// Returns true if cell info is available and writes to out
bool ppp_gsm_get_cell_info(ppp_cell_info_t *out);

#ifdef __cplusplus
}
#endif
