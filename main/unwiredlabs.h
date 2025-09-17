#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool unwiredlabs_geolocate(const char *token,
                           int mcc, int mnc, int tac, int cid,
                           char *out_city, size_t city_len,
                           char *out_state, size_t state_len,
                           char *out_date, size_t date_len,
                           char *out_time, size_t time_len);

#ifdef __cplusplus
}
#endif
