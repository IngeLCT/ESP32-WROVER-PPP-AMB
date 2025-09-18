#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool unwiredlabs_geolocate(const char *token,
                           int mcc, int mnc, int tac_lac, int cid,
                           char *city, size_t city_len,
                           char *state, size_t state_len,
                           char *date, size_t date_len,
                           char *time, size_t time_len);

#ifdef __cplusplus
}
#endif
