idf_component_register(
    SRCS "app_main.cpp" "tuya_client.cpp"
    PRIV_REQUIRES spi_flash esp_wifi esp_event esp_netif nvs_flash mbedtls driver esp_timer
    INCLUDE_DIRS ".")

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
