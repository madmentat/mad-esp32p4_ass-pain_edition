#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STM32_SWD_STATUS_IDLE = 0,
    STM32_SWD_STATUS_BUSY,
    STM32_SWD_STATUS_OK,
    STM32_SWD_STATUS_FAILED,
} stm32_swd_status_t;

typedef struct {
    stm32_swd_status_t status;
    esp_err_t last_result;
    uint32_t dp_idcode;
    uint32_t cpuid;
    uint32_t dbgmcu_idcode;
    char last_message[96];
} stm32_swd_state_t;

/* Configure NRST/SWDIO/SWCLK GPIOs. Safe to call on every boot. */
esp_err_t stm32_swd_programmer_init(void);

/* Start a one-shot probe task. It performs SWD line reset, reads DP IDCODE,
 * powers up the debug port, then tries to read ARM CPUID and STM32 DBGMCU IDCODE.
 * This is the first hardware validation step before flash programming is added.
 */
esp_err_t stm32_swd_programmer_probe_async(void);

/* Synchronous probe, mostly useful from the async task and later tests. */
esp_err_t stm32_swd_programmer_probe(void);

/* Program a raw STM32F030 application image to internal flash through SWD.
 * image is written to flash_base, normally 0x08000000. The function performs:
 * connect -> halt core -> flash unlock -> page erase -> half-word program -> verify -> reset.
 */
esp_err_t stm32_swd_programmer_flash_image(const uint8_t *image, size_t image_size, uint32_t flash_base);

stm32_swd_status_t stm32_swd_programmer_get_status(void);
const stm32_swd_state_t *stm32_swd_programmer_get_state(void);
const char *stm32_swd_programmer_get_status_text(void);

#ifdef __cplusplus
}
#endif
