if(CMAKE_SCRIPT_MODE_FILE OR CMAKE_FIND_PACKAGE_NAME)
    file(GLOB EEZ_UI_SOURCES "src/ui/*.c")
else()
    file(GLOB EEZ_UI_SOURCES CONFIGURE_DEPENDS "src/ui/*.c")
endif()

set(APP_SOURCES
    "jc4880p443c_demo.c"
    "display_direct_timing.c"
    "display_experiments.c"
    "eez_ui_port.c"
    "eez_ui_runtime.c"
    "ui_background_direct.c"
    "stm32_swd_programmer.c"
    "update/update_manager.c"
    ${EEZ_UI_SOURCES}
)

set(APP_REQUIRES
    jc4880p443c
    lvgl
    esp_lcd_st7701
    esp_lcd_touch
    esp_lcd_touch_gt911
    esp_timer
    esp_mm
    driver
    freertos
    log
    esp_wifi
    esp_event
    esp_netif
    nvs_flash
    app_update
    esp_http_client
    esp_partition
    esp_app_format
    esp-tls
    json
    mbedtls
)


if(CONFIG_MAD_OTA_ENABLE AND CONFIG_DASHBOARD_WIFI_BACKEND_DISABLED)
    message(FATAL_ERROR "MAD OTA requires ESP32-C6 Hosted Wi-Fi backend, but CONFIG_DASHBOARD_WIFI_BACKEND_DISABLED=y. Run prepare_p4_hosted_ota_build.ps1 or patch_sdkconfig_p4_hosted_ota.ps1, then idf.py reconfigure.")
endif()

if(CONFIG_DASHBOARD_WIFI_BACKEND_ESP32C6_AT)
    list(APPEND APP_SOURCES "wifi_manager_esp32c6_at.c")
elseif(CONFIG_DASHBOARD_WIFI_BACKEND_ESP32C6_SERVICE)
    list(APPEND APP_SOURCES "wifi_manager_esp32c6_service.c")
elseif(CONFIG_DASHBOARD_WIFI_BACKEND_ESP32C6_HOSTED)
    list(APPEND APP_SOURCES "wifi_manager_esp32c6_hosted.c")
else()
    list(APPEND APP_SOURCES "wifi_manager_stub.c")
endif()

idf_component_register(
    SRCS
        ${APP_SOURCES}
    INCLUDE_DIRS "." "src/ui"
    REQUIRES
        ${APP_REQUIRES}
)
