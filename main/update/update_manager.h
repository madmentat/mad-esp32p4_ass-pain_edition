#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MAD_OTA_STATUS_IDLE = 0,
    MAD_OTA_STATUS_WAITING_WIFI,
    MAD_OTA_STATUS_CHECKING,
    MAD_OTA_STATUS_UPDATE_AVAILABLE,
    MAD_OTA_STATUS_NO_UPDATE,
    MAD_OTA_STATUS_DOWNLOADING,
    MAD_OTA_STATUS_INSTALLING,
    MAD_OTA_STATUS_VERIFYING,
    MAD_OTA_STATUS_REBOOT_PENDING,
    MAD_OTA_STATUS_FAILED,
} mad_ota_status_t;

typedef struct {
    mad_ota_status_t status;
    int progress_percent;
    char current_version[32];
    char available_version[32];
    char last_error[96];
} mad_ota_state_t;

extern volatile mad_ota_state_t g_mad_ota_state;

/*
 * Mark pending OTA image valid if rollback is enabled and this boot is still
 * pending verification. Safe to call on every boot.
 */
esp_err_t mad_ota_confirm_running_app(void);

/*
 * Start the OTA manager task. It waits for Wi-Fi, checks update manifest, and
 * installs only when CONFIG_MAD_OTA_AUTO_INSTALL is enabled.
 */
esp_err_t mad_ota_start_async(void);

/*
 * Manual install trigger for UI button. If an update was already found,
 * installs it. If not, checks the manifest first and installs when available.
 */
esp_err_t mad_ota_install_available_async(void);

/* Manual STM32 component update: fetches target=STM32F030K6T6 from the same
 * update/check manifest, downloads it to RAM, verifies SHA-256, then flashes
 * STM32 internal flash through SWD.
 */
esp_err_t mad_ota_install_stm32_available_async(void);

mad_ota_status_t mad_ota_get_status(void);
int mad_ota_get_progress_percent(void);
const char *mad_ota_get_last_error(void);
const char *mad_ota_get_current_version(void);
const char *mad_ota_get_available_version(void);

/* True after the UI/manual update button has been pressed at least once. */
bool mad_ota_manual_install_was_requested(void);

/* True only during ESP32-P4 self-OTA display-hold mode.  Runtime UI can use
 * this to draw a small native HUD while normal LVGL panel flushes are frozen. */
bool mad_ota_p4_self_ota_display_hold_active(void);

#ifdef __cplusplus
}
#endif
