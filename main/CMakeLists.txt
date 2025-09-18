idf_component_register(SRCS "unwiredlabs.c" "sensors.c" "main.c"
                    INCLUDE_DIRS "." REQUIRES driver esp_timer esp_http_client esp-tls esp_netif nvs_flash json esp_firebase lwip)

