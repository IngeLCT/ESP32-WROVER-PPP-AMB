// Core
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

// ESP-IDF
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"

// Project
#include "sensors.h"
#include "firebase.h"
#include "Privado.h"

// PPP
#include "modem_ppp.h"
#include "esp_modem_api.h"

static const char *TAG_APP = "app";
static esp_modem_dce_t *g_dce = NULL;

// ---- Ciudad global para el JSON ----
static char g_city[64]  = "----";
static bool g_city_sent = false;   // <-- NUEVO: solo mandar "ciudad" la primera vez

#ifndef UNWIREDLABS_TOKEN
#define UNWIREDLABS_TOKEN "" // define en Privado.h
#endif
#ifndef DEVICE_ID
#define DEVICE_ID "ESP32-WROVER-PPP"
#endif

static void init_sntp_and_time(void) {
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_init();
    setenv("TZ", "UTC6", 1); // GMT-6
    tzset();
    for (int i = 0; i < 100; ++i) {
        time_t now = 0;
        time(&now);
        if (now > 1609459200) break; // ~2021-01-01
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* Construye "Ciudad-Estado" sin comas (para CSV), con saneo básico */
static void build_city_hyphen(char *dst, size_t dstlen, const char *city, const char *state) {
    if (!dst || dstlen == 0) return;
    const char *c = (city && city[0]) ? city : "----";
    if (state && state[0]) {
        snprintf(dst, dstlen, "%s-%s", c, state);
    } else {
        snprintf(dst, dstlen, "%s", c);
    }
    // Reemplaza cualquier coma accidental por '-'
    for (size_t i = 0; dst[i]; ++i) {
        if (dst[i] == ',' || dst[i] == ';' || dst[i] == '|') dst[i] = '-';
    }
}

#define SENSOR_TASK_STACK 10240

static void sensor_task(void *pv) {
    SensorData data;

    // Hora de arranque (inicio)
    time_t start_epoch;
    struct tm start_tm_info;
    char inicio_str[20] = "00:00:00";
    time(&start_epoch);
    localtime_r(&start_epoch, &start_tm_info);
    strftime(inicio_str, sizeof(inicio_str), "%H:%M:%S", &start_tm_info);

    const uint32_t TOKEN_REFRESH_INTERVAL_SEC = 50 * 60; // 50 minutos
    time_t last_token_refresh = time(NULL);

    if (firebase_init() != 0) {
        ESP_LOGE(TAG_APP, "Error inicializando Firebase");
        vTaskDelete(NULL);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    firebase_delete("/historial_mediciones");

    // 1 muestra/minuto, envío cada 5 min
    const int SAMPLE_EVERY_MIN = 1;
    const int SAMPLES_PER_BATCH = 5;
    const TickType_t SAMPLE_DELAY_TICKS = pdMS_TO_TICKS(SAMPLE_EVERY_MIN * 60000);
    int sample_count = 0;

    double sum_pm1p0=0, sum_pm2p5=0, sum_pm4p0=0, sum_pm10p0=0, sum_voc=0, sum_nox=0, sum_avg_temp=0, sum_avg_hum=0;
    uint32_t sum_co2 = 0;

    while (1) {
        if (sensors_read(&data) == ESP_OK) {
            sample_count++;
            sum_pm1p0 += data.pm1p0;
            sum_pm2p5 += data.pm2p5;
            sum_pm4p0 += data.pm4p0;
            sum_pm10p0 += data.pm10p0;
            sum_voc += data.voc;
            sum_nox += data.nox;
            sum_avg_temp += data.avg_temp;
            sum_avg_hum += data.avg_hum;
            sum_co2 += data.co2;
            ESP_LOGI(TAG_APP,
                "Muestra %d/%d: PM1.0=%.2f PM2.5=%.2f PM4.0=%.2f PM10=%.2f VOC=%.1f NOx=%.1f CO2=%u Temp=%.2fC Hum=%.2f%%",
                sample_count, SAMPLES_PER_BATCH, data.pm1p0, data.pm2p5, data.pm4p0, data.pm10p0,
                data.voc, data.nox, data.co2, data.avg_temp, data.avg_hum);
        } else {
            ESP_LOGW(TAG_APP, "Error leyendo sensores (batch %d)", sample_count);
        }

        // Refresh del token cada 50 min aprox
        time_t now_epoch_check = time(NULL);
        if ((now_epoch_check - last_token_refresh) >= TOKEN_REFRESH_INTERVAL_SEC) {
            ESP_LOGI(TAG_APP, "Refrescando token (50m)...");
            int r = firebase_refresh_token();
            if (r == 0) ESP_LOGI(TAG_APP, "Token refresh OK"); else ESP_LOGW(TAG_APP, "Fallo refresh token (%d)", r);
            last_token_refresh = now_epoch_check;
        }

        if (sample_count >= SAMPLES_PER_BATCH) {
            // Promedio del batch
            SensorData avg = {0};
            avg.pm1p0 = sum_pm1p0 / sample_count;
            avg.pm2p5 = sum_pm2p5 / sample_count;
            avg.pm4p0 = sum_pm4p0 / sample_count;
            avg.pm10p0 = sum_pm10p0 / sample_count;
            avg.voc = sum_voc / sample_count;
            avg.nox = sum_nox / sample_count;
            avg.avg_temp = sum_avg_temp / sample_count;
            avg.avg_hum = sum_avg_hum / sample_count;
            avg.co2 = (uint16_t)(sum_co2 / sample_count);
            avg.scd_temp = avg.avg_temp;
            avg.scd_hum = avg.avg_hum;
            avg.sen_temp = avg.avg_temp;
            avg.sen_hum = avg.avg_hum;

            time_t now_epoch;
            struct tm tm_info;
            time(&now_epoch);
            localtime_r(&now_epoch, &tm_info);
            char hora_envio[16];
            strftime(hora_envio, sizeof(hora_envio), "%H:%M:%S", &tm_info);
            char fecha_actual[20];
            strftime(fecha_actual, sizeof(fecha_actual), "%d-%m-%Y", &tm_info);

            char json[512];

            if (!g_city_sent) {
                // Primera vez: incluye "ciudad" (sin comas) y márcala como enviada
                snprintf(json, sizeof(json),
                    "{\"pm1p0\":%.2f,\"pm2p5\":%.2f,\"pm4p0\":%.2f,\"pm10p0\":%.2f,"
                     "\"voc\":%.1f,\"nox\":%.1f,\"cTe\":%.2f,\"cHu\":%.2f,\"co2\":%u,"
                     "\"fecha\":\"%s\",\"inicio\":\"%s\",\"ciudad\":\"%s\",\"hora\":\"%s\",\"id\":\"%s\"}",
                    avg.pm1p0, avg.pm2p5, avg.pm4p0, avg.pm10p0,
                    avg.voc, avg.nox, avg.avg_temp, avg.avg_hum,
                    avg.co2, fecha_actual, inicio_str, g_city, hora_envio, DEVICE_ID);
                g_city_sent = true;  // <-- a partir de aquí ya no se manda "ciudad"
            } else {
                // Envíos subsecuentes: SIN "ciudad"
                snprintf(json, sizeof(json),
                    "{\"pm1p0\":%.2f,\"pm2p5\":%.2f,\"pm4p0\":%.2f,\"pm10p0\":%.2f,"
                     "\"voc\":%.1f,\"nox\":%.1f,\"cTe\":%.2f,\"cHu\":%.2f,\"co2\":%u,"
                     "\"fecha\":\"%s\",\"inicio\":\"%s\",\"hora\":\"%s\",\"id\":\"%s\"}",
                    avg.pm1p0, avg.pm2p5, avg.pm4p0, avg.pm10p0,
                    avg.voc, avg.nox, avg.avg_temp, avg.avg_hum,
                    avg.co2, fecha_actual, inicio_str, hora_envio, DEVICE_ID);
            }

            ESP_LOGI(TAG_APP, "JSON promedio %dm: %s", SAMPLES_PER_BATCH * SAMPLE_EVERY_MIN, json);
            firebase_push("/historial_mediciones", json);

            // Retención aproximada por tamaño total (~10 MB)
            const size_t MAX_BYTES = 10 * 1024 * 1024;
            static double avg_size = 256.0;
            static uint32_t approx_count = 0;
            size_t item_len = strlen(json);
            avg_size = (avg_size * 0.9) + (0.1 * (double)item_len);
            approx_count++;
            uint32_t max_items = (uint32_t)(MAX_BYTES / (avg_size > 1.0 ? avg_size : 1.0));
            uint32_t high_water = max_items + 50;
            if (approx_count > high_water) {
                int deleted = firebase_trim_oldest_batch("/historial_mediciones", 50);
                if (deleted > 0) {
                    approx_count = (approx_count > (uint32_t)deleted) ? (approx_count - (uint32_t)deleted) : 0;
                    ESP_LOGI(TAG_APP, "Retención: borrados %d antiguos. approx_count=%u max_items=%u avg=%.1fB",
                             deleted, approx_count, max_items, avg_size);
                }
            }

            // Reset de acumuladores
            sample_count = 0;
            sum_pm1p0=sum_pm2p5=sum_pm4p0=sum_pm10p0=sum_voc=sum_nox=sum_avg_temp=sum_avg_hum=0;
            sum_co2 = 0;
        }

        vTaskDelay(SAMPLE_DELAY_TICKS);
    }
}

void app_main(void)
{
    // esp_log_level_set("esp_modem", ESP_LOG_VERBOSE);
    // esp_log_level_set("command_lib", ESP_LOG_VERBOSE);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // === 1) Arranca PPP ===
    modem_ppp_config_t cfg = {
        .tx_io = 26, .rx_io = 27,
        .rts_io = -1, .cts_io = -1,     // sin flow control por ahora
        .dtr_io = 25,
        .rst_io = 5, .pwrkey_io = 4, .board_power_io = 12,
        .rst_active_low = true, .rst_pulse_ms = 200,
        .apn = "internet.itelcel.com",
        .sim_pin = "",
        .use_cmux = false               // ¡dejar en false!
    };
    esp_err_t mret = modem_ppp_start_blocking(&cfg, 120000 /* 120s timeout */, &g_dce);
    if (mret != ESP_OK) {
        ESP_LOGE(TAG_APP, "No se pudo levantar PPP (%s). Reiniciando...", esp_err_to_name(mret));
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    // === 2) SNTP con PPP activo ===
    init_sntp_and_time();

    // === 3) Geolocalización por celda (AT + UnwiredLabs) => g_city sin comas ===
    if (UNWIREDLABS_TOKEN[0]) {
        char city[64] = "", state[64] = "";
        if (modem_unwiredlabs_city_state(city, sizeof(city), state, sizeof(state)) == ESP_OK) {
            build_city_hyphen(g_city, sizeof(g_city), city, state); // "Ciudad-Estado" sin comas
            ESP_LOGI(TAG_APP, "Ciudad para JSON (1a vez): %s", g_city);
        } else {
            ESP_LOGW(TAG_APP, "No se pudo geolocalizar por celda. Ciudad='----'");
            strncpy(g_city, "----", sizeof(g_city));
            g_city[sizeof(g_city)-1] = '\0';
        }
    } else {
            ESP_LOGW(TAG_APP, "UNWIREDLABS_TOKEN vacío. Ciudad quedará '----'");
    }

    // === 4) Sensores y task de envío a Firebase ===
    esp_err_t sret = sensors_init_all();
    if (sret != ESP_OK) {
        ESP_LOGE(TAG_APP, "Fallo al inicializar sensores: %s", esp_err_to_name(sret));
    } else {
        xTaskCreate(sensor_task, "sensor_task", SENSOR_TASK_STACK, NULL, 5, NULL);
    }
}
