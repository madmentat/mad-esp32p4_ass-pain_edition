/*
 * stm32_swd_programmer.c
 *
 * ESP32-P4 -> STM32F030 software SWD master.
 *
 * fix25_stm32_swd_ack5_no_extra_turnaround:
 *   - keeps CMSIS-style SWD transfers;
 *   - uses short MEM-AP programming chunks instead of long 1 KiB streams;
 *   - restores PG/program session after SWD desync and retries the current halfword;
 *   - avoids FLASH_SR polling during the tight halfword program loop;
 *   - clears debug halt/vector-catch and requests a CMSIS-style SYSRESETREQ
 *     before the final NRST pulse so the STM32 really starts the new app;
 *   - handles marginal write ACK=0x05 as ACK OK with NO extra turnaround,
 *     because the sampled third ACK bit is very likely already the release/turnaround
 *     cycle on this bit-banged P4 wiring;
 *   - uses a release-reset-then-immediate-DAP fallback instead of trying to
 *     fully initialise DAP while NRST is held low.
 *
 * Current JC4880P443C bench pin assignment:
 *   NRST  -> GPIO29
 *   SWDIO -> GPIO33
 *   SWCLK -> GPIO31
 */

#include "stm32_swd_programmer.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#ifndef CONFIG_STM32_SWD_ENABLE
#define CONFIG_STM32_SWD_ENABLE 1
#endif

#ifndef CONFIG_STM32_SWD_NRST_GPIO
#define CONFIG_STM32_SWD_NRST_GPIO 29
#endif

#ifndef CONFIG_STM32_SWD_SWDIO_GPIO
#define CONFIG_STM32_SWD_SWDIO_GPIO 33
#endif

#ifndef CONFIG_STM32_SWD_SWCLK_GPIO
#define CONFIG_STM32_SWD_SWCLK_GPIO 31
#endif

#ifndef CONFIG_STM32_SWD_BIT_DELAY_US
#define CONFIG_STM32_SWD_BIT_DELAY_US 2
#endif

#ifndef CONFIG_STM32_SWD_TASK_STACK_SIZE
#define CONFIG_STM32_SWD_TASK_STACK_SIZE 4096
#endif

#ifndef CONFIG_STM32_SWD_TASK_PRIORITY
#define CONFIG_STM32_SWD_TASK_PRIORITY 3
#endif

#ifndef CONFIG_STM32_SWD_TASK_CORE
#define CONFIG_STM32_SWD_TASK_CORE 0
#endif

#ifndef CONFIG_STM32_SWD_SCOPE_TEST_ENABLE
#define CONFIG_STM32_SWD_SCOPE_TEST_ENABLE 0
#endif

#ifndef CONFIG_STM32_SWD_SCOPE_TEST_PERIOD_MS
#define CONFIG_STM32_SWD_SCOPE_TEST_PERIOD_MS 100
#endif

#define SWD_PIN_NRST      ((gpio_num_t)CONFIG_STM32_SWD_NRST_GPIO)
#define SWD_PIN_SWDIO     ((gpio_num_t)CONFIG_STM32_SWD_SWDIO_GPIO)
#define SWD_PIN_SWCLK     ((gpio_num_t)CONFIG_STM32_SWD_SWCLK_GPIO)
#define SWD_DELAY_US_CFG      CONFIG_STM32_SWD_BIT_DELAY_US
#define SWD_DELAY_US_DEFAULT  ((SWD_DELAY_US_CFG < 2) ? 2 : SWD_DELAY_US_CFG)
#define SWD_DELAY_US_SLOW     10U

static uint32_t s_swd_delay_us = SWD_DELAY_US_DEFAULT;

/* CMSIS-DAP-style SWD request bits: APnDP, RnW, A2, A3. */
#define SWD_REQ_APnDP     (1U << 0)
#define SWD_REQ_RnW       (1U << 1)
#define SWD_REQ_A2        (1U << 2)
#define SWD_REQ_A3        (1U << 3)

#define SWD_ACK_OK        0x1U
#define SWD_ACK_WAIT      0x2U
#define SWD_ACK_FAULT     0x4U

#define SWD_TURNAROUND_CYCLES 1U
#define SWD_IDLE_CYCLES       8U
#define SWD_RETRY_COUNT       8

#define DP_ADDR_IDCODE    0x00U  /* DP read  addr 0x0 */
#define DP_ADDR_ABORT     0x00U  /* DP write addr 0x0 */
#define DP_ADDR_CTRL_STAT 0x04U
#define DP_ADDR_SELECT    0x08U
#define DP_ADDR_RDBUFF    0x0CU

#define AP_ADDR_CSW       0x00U
#define AP_ADDR_TAR       0x04U
#define AP_ADDR_DRW       0x0CU
#define AP_ADDR_IDR       0x0CU  /* AP bank 0xF, addr 0xFC */

#define DP_ABORT_ALL      0x0000001EU
#define DP_CTRL_PWRUP_REQ 0x50000000U
#define DP_CTRL_PWRUP_ACK 0xA0000000U

/* Cortex-M AHB-AP CSW: 32-bit access, auto-increment single, common defaults. */
#define AHB_AP_CSW_BASE   0x23000050U
#define AHB_AP_CSW_8BIT   (AHB_AP_CSW_BASE | 0U)
#define AHB_AP_CSW_16BIT  (AHB_AP_CSW_BASE | 1U)
#define AHB_AP_CSW_32BIT  (AHB_AP_CSW_BASE | 2U)

#define ARM_CPUID_ADDR             0xE000ED00U
#define STM32F0_DBGMCU_IDCODE_ADDR 0x40015800U

#define ARM_DHCSR_ADDR             0xE000EDF0U
#define ARM_DHCSR_DBGKEY           0xA05F0000U
#define ARM_DHCSR_C_DEBUGEN        (1U << 0)
#define ARM_DHCSR_C_HALT           (1U << 1)
#define ARM_DHCSR_S_HALT           (1U << 17)
#define ARM_DEMCR_ADDR             0xE000EDFCU
#define ARM_DEMCR_VC_CORERESET     (1U << 0)
#define ARM_AIRCR_ADDR             0xE000ED0CU
#define ARM_AIRCR_VECTKEY          (0x5FAU << 16)
#define ARM_AIRCR_SYSRESETREQ      (1U << 2)

#define STM32F0_FLASH_BASE_ADDR    0x08000000U
#define STM32F0_FLASH_PAGE_SIZE    1024U
#define STM32F0_FLASH_MAX_BYTES    (32U * 1024U)

#define STM32F0_FLASH_REG_BASE     0x40022000U
#define STM32F0_FLASH_KEYR         (STM32F0_FLASH_REG_BASE + 0x04U)
#define STM32F0_FLASH_SR           (STM32F0_FLASH_REG_BASE + 0x0CU)
#define STM32F0_FLASH_CR           (STM32F0_FLASH_REG_BASE + 0x10U)
#define STM32F0_FLASH_AR           (STM32F0_FLASH_REG_BASE + 0x14U)

#define STM32F0_FLASH_KEY1         0x45670123U
#define STM32F0_FLASH_KEY2         0xCDEF89ABU
#define STM32F0_FLASH_SR_BSY       (1U << 0)
#define STM32F0_FLASH_SR_PGERR     (1U << 2)
#define STM32F0_FLASH_SR_WRPRTERR  (1U << 4)
#define STM32F0_FLASH_SR_EOP       (1U << 5)
#define STM32F0_FLASH_SR_ERRS      (STM32F0_FLASH_SR_PGERR | STM32F0_FLASH_SR_WRPRTERR)
#define STM32F0_FLASH_CR_PG        (1U << 0)
#define STM32F0_FLASH_CR_PER       (1U << 1)
#define STM32F0_FLASH_CR_STRT      (1U << 6)
#define STM32F0_FLASH_CR_LOCK      (1U << 7)

#define STM32F0_PROG_BLIND_DELAY_US  450U
#define STM32F0_PROG_YIELD_EVERY_HW  16U
#define STM32F0_PROG_SR_CHECK_BYTES  0U
/* Long auto-increment streams were fast, but on the real P4->STM32 wiring they
 * sometimes lost SWD framing in the middle of an image. 64 bytes still avoids
 * setting TAR for every halfword, but regularly re-anchors MEM-AP to the exact
 * flash address and makes recovery deterministic. */
#define STM32F0_PROG_STREAM_CHUNK_BYTES 64U
#define STM32F0_PROG_RETRY_COUNT 10

static const char *TAG = "STM32_SWD";
static TaskHandle_t s_probe_task;
static SemaphoreHandle_t s_swd_lock;

static stm32_swd_state_t s_state = {
    .status = STM32_SWD_STATUS_IDLE,
    .last_result = ESP_OK,
    .dp_idcode = 0,
    .cpuid = 0,
    .dbgmcu_idcode = 0,
    .last_message = "idle",
};

static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;

static void state_set(stm32_swd_status_t status, esp_err_t result, const char *message)
{
    portENTER_CRITICAL(&s_state_mux);
    s_state.status = status;
    s_state.last_result = result;
    if (message) {
        snprintf(s_state.last_message, sizeof(s_state.last_message), "%s", message);
    }
    portEXIT_CRITICAL(&s_state_mux);
}

static void state_set_values(uint32_t dp_idcode, uint32_t cpuid, uint32_t dbgmcu_idcode)
{
    portENTER_CRITICAL(&s_state_mux);
    s_state.dp_idcode = dp_idcode;
    s_state.cpuid = cpuid;
    s_state.dbgmcu_idcode = dbgmcu_idcode;
    portEXIT_CRITICAL(&s_state_mux);
}

static inline void swd_delay(void)
{
    if (s_swd_delay_us > 0U) {
        esp_rom_delay_us(s_swd_delay_us);
    }
}

/* --------------------------------------------------------------------------
 * Low-level GPIO primitives.
 *
 * The write/read/clock functions below intentionally match CMSIS-DAP SW_DP.c:
 *   SW_CLOCK_CYCLE: SWCLK low -> delay -> SWCLK high -> delay
 *   SW_WRITE_BIT:   set SWDIO -> SWCLK low -> delay -> SWCLK high -> delay
 *   SW_READ_BIT:    SWCLK low -> delay -> sample SWDIO -> SWCLK high -> delay
 *
 * This means each helper leaves SWCLK high, just like CMSIS-DAP. The next bit
 * starts by taking SWCLK low again. Do not "fix" this back to the old high-low
 * style unless the entire transfer timing is reworked.
 * -------------------------------------------------------------------------- */

static inline void swclk_low(void)
{
    gpio_set_level(SWD_PIN_SWCLK, 0);
}

static inline void swclk_high(void)
{
    gpio_set_level(SWD_PIN_SWCLK, 1);
}

static inline void swd_clock_cycle(void)
{
    swclk_low();
    swd_delay();
    swclk_high();
    swd_delay();
}

static inline void swdio_drive(unsigned bit)
{
    gpio_set_level(SWD_PIN_SWDIO, bit ? 1 : 0);
}

static void swdio_output_enable(void)
{
    gpio_set_pull_mode(SWD_PIN_SWDIO, GPIO_PULLUP_ONLY);
    gpio_set_direction(SWD_PIN_SWDIO, GPIO_MODE_OUTPUT);
}

static void swdio_output_disable(void)
{
    /* Release the bidirectional wire. The internal pull-up is only a helper;
     * when the target drives ACK/data it easily overrides it. */
    gpio_set_level(SWD_PIN_SWDIO, 1);
    gpio_set_direction(SWD_PIN_SWDIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(SWD_PIN_SWDIO, GPIO_PULLUP_ONLY);
}

static void swdio_output_high(void)
{
    swdio_drive(1);
    swdio_output_enable();
    swdio_drive(1);
}

static inline void swd_write_bit(unsigned bit)
{
    swdio_drive(bit & 1U);
    swclk_low();
    swd_delay();
    swclk_high();
    swd_delay();
}

static inline unsigned swd_read_bit(void)
{
    swclk_low();
    swd_delay();
    unsigned bit = (unsigned)gpio_get_level(SWD_PIN_SWDIO) & 1U;
    swclk_high();
    swd_delay();
    return bit;
}

static void nrst_assert(void)
{
    /* NRST as open-drain style: actively pull low only. */
    gpio_set_direction(SWD_PIN_NRST, GPIO_MODE_OUTPUT);
    gpio_set_level(SWD_PIN_NRST, 0);
}

static void nrst_release(void)
{
    /* Release reset instead of driving it high. Most target boards already have
     * NRST pull-up; ESP32-P4 internal pull-up is enabled as bench insurance. */
    gpio_set_level(SWD_PIN_NRST, 1);
    gpio_set_direction(SWD_PIN_NRST, GPIO_MODE_INPUT);
    gpio_set_pull_mode(SWD_PIN_NRST, GPIO_PULLUP_ONLY);
}

static void swd_bus_prepare(void)
{
    /* Reclaim SWD pins before every transaction. The final reset path releases
     * SWDIO/SWCLK so the target application can run without the ESP32 driving
     * PA13/PA14 forever. */
    gpio_set_direction(SWD_PIN_SWCLK, GPIO_MODE_OUTPUT);
    gpio_set_level(SWD_PIN_SWCLK, 1);
    gpio_set_pull_mode(SWD_PIN_SWDIO, GPIO_PULLUP_ONLY);
    swdio_output_high();
    nrst_release();
}

static void swd_bus_release_idle(void)
{
    swdio_output_disable();
    gpio_set_level(SWD_PIN_SWCLK, 1);
    gpio_set_direction(SWD_PIN_SWCLK, GPIO_MODE_INPUT);
    gpio_set_pull_mode(SWD_PIN_SWCLK, GPIO_PULLUP_ONLY);
    nrst_release();
}

static unsigned parity32(uint32_t value)
{
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    value &= 0xFU;
    return (0x6996U >> value) & 1U;
}

static esp_err_t swd_ack_to_err(uint32_t ack)
{
    switch (ack) {
    case SWD_ACK_WAIT:
        return ESP_ERR_TIMEOUT;
    case SWD_ACK_FAULT:
        return ESP_FAIL;
    default:
        return ESP_FAIL;
    }
}

static const char *swd_ack_name(uint32_t ack)
{
    switch (ack) {
    case SWD_ACK_OK: return "OK";
    case SWD_ACK_WAIT: return "WAIT";
    case SWD_ACK_FAULT: return "FAULT";
    default: return "PROTO";
    }
}

static void swd_write_bits_lsb(uint32_t value, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        swd_write_bit((value >> i) & 1U);
    }
}

static void swd_line_reset(void)
{
    swdio_output_high();
    /* At least 50 clocks with SWDIO high. Use 64. */
    swd_write_bits_lsb(0xFFFFFFFFU, 32);
    swd_write_bits_lsb(0xFFFFFFFFU, 32);
}

static void swd_idle_cycles(unsigned count)
{
    swdio_output_enable();
    swdio_drive(0);
    for (unsigned i = 0; i < count; ++i) {
        swd_clock_cycle();
    }
    swdio_drive(1);
}

static void swd_swj_sequence_16(uint16_t sequence_lsb_first)
{
    swdio_output_high();
    swd_write_bits_lsb(sequence_lsb_first, 16);
}

static void swd_enter_swd_from_jtag(void)
{
    ESP_LOGI(TAG, "CMSIS-style SWJ enter SWD: line_reset -> 0xE79E LSB-first -> line_reset -> idle");
    swclk_high();
    swd_line_reset();
    swd_swj_sequence_16(0xE79EU);
    swd_line_reset();
    swd_idle_cycles(SWD_IDLE_CYCLES);
}

static void swd_reconnect_swd_only(void)
{
    ESP_LOGI(TAG, "CMSIS-style SWD reconnect: line_reset only -> idle");
    swclk_high();
    swd_line_reset();
    swd_idle_cycles(SWD_IDLE_CYCLES);
}

static void swd_protocol_recover(void)
{
    /* Keep the selected SW-DP, but resynchronize the serial engine after
     * parity/protocol errors. ADIv5 recommends at least 50 clocks with the
     * line high/tristated before retrying READID/a new operation. */
    swclk_high();
    swd_line_reset();
    swd_idle_cycles(SWD_IDLE_CYCLES);
}

static void swd_scope_pin_test(void)
{
#if CONFIG_STM32_SWD_SCOPE_TEST_ENABLE
    const TickType_t d = pdMS_TO_TICKS(CONFIG_STM32_SWD_SCOPE_TEST_PERIOD_MS);

    ESP_LOGW(TAG,
             "SWD scope pin test start: NRST=%d SWDIO=%d SWCLK=%d period=%dms; only real SWD pins are toggled",
             (int)SWD_PIN_NRST,
             (int)SWD_PIN_SWDIO,
             (int)SWD_PIN_SWCLK,
             CONFIG_STM32_SWD_SCOPE_TEST_PERIOD_MS);

    gpio_set_direction(SWD_PIN_SWCLK, GPIO_MODE_OUTPUT);
    gpio_set_direction(SWD_PIN_SWDIO, GPIO_MODE_OUTPUT);
    gpio_set_level(SWD_PIN_SWCLK, 0);
    gpio_set_level(SWD_PIN_SWDIO, 0);
    vTaskDelay(d);

    ESP_LOGW(TAG, "SWD scope pin test: toggling SWCLK GPIO%d", (int)SWD_PIN_SWCLK);
    for (int i = 0; i < 12; ++i) {
        gpio_set_level(SWD_PIN_SWCLK, i & 1);
        gpio_set_level(SWD_PIN_SWDIO, 0);
        ESP_LOGI(TAG, "PINTEST SWCLK_GPIO%d=%d", (int)SWD_PIN_SWCLK, gpio_get_level(SWD_PIN_SWCLK));
        vTaskDelay(d);
    }

    gpio_set_level(SWD_PIN_SWCLK, 0);
    ESP_LOGW(TAG, "SWD scope pin test: toggling SWDIO GPIO%d", (int)SWD_PIN_SWDIO);
    for (int i = 0; i < 12; ++i) {
        gpio_set_level(SWD_PIN_SWDIO, i & 1);
        ESP_LOGI(TAG, "PINTEST SWDIO_GPIO%d=%d", (int)SWD_PIN_SWDIO, gpio_get_level(SWD_PIN_SWDIO));
        vTaskDelay(d);
    }

    gpio_set_level(SWD_PIN_SWCLK, 0);
    gpio_set_level(SWD_PIN_SWDIO, 1);
    ESP_LOGW(TAG, "SWD scope pin test done; starting real SWD transaction on SWDIO GPIO%d", (int)SWD_PIN_SWDIO);
    vTaskDelay(pdMS_TO_TICKS(20));
#endif
}

static uint32_t swd_make_request(bool ap, bool read, uint8_t addr)
{
    uint32_t req = 0;
    if (ap) {
        req |= SWD_REQ_APnDP;
    }
    if (read) {
        req |= SWD_REQ_RnW;
    }
    req |= ((uint32_t)(addr >> 2) & 0x3U) << 2;
    return req;
}

/* CMSIS-DAP-style SWD_Transfer().
 * request bits are APnDP/RnW/A2/A3 in bits [3:0]. data points to WDATA/RDATA.
 * return is ACK[2:0] in canonical LSB-first numeric form: OK=1, WAIT=2, FAULT=4.
 */
static uint8_t cmsis_swd_transfer(uint32_t request, uint32_t *data)
{
    uint32_t ack = 0;
    uint32_t bit = 0;
    uint32_t val = 0;
    uint32_t parity = 0;
    bool write_ack5_no_extra_turnaround = false;

    /* Packet request. */
    swdio_output_enable();

    swd_write_bit(1U);                          /* Start */

    bit = (request >> 0) & 1U;
    swd_write_bit(bit);                         /* APnDP */
    parity += bit;

    bit = (request >> 1) & 1U;
    swd_write_bit(bit);                         /* RnW */
    parity += bit;

    bit = (request >> 2) & 1U;
    swd_write_bit(bit);                         /* A2 */
    parity += bit;

    bit = (request >> 3) & 1U;
    swd_write_bit(bit);                         /* A3 */
    parity += bit;

    swd_write_bit(parity & 1U);                 /* Parity */
    swd_write_bit(0U);                          /* Stop */
    swd_write_bit(1U);                          /* Park */

    /* Turnaround: host releases SWDIO, target owns it. */
    swdio_output_disable();
    for (uint32_t n = SWD_TURNAROUND_CYCLES; n; --n) {
        swd_clock_cycle();
    }

    /* ACK response: bit0, bit1, bit2. */
    bit = swd_read_bit();
    ack = bit << 0;
    bit = swd_read_bit();
    ack |= bit << 1;
    bit = swd_read_bit();
    ack |= bit << 2;

    if (ack == 0x05U && ((request & SWD_REQ_RnW) == 0U)) {
        /* Bench symptom from OTA logs:
         *   DP IDCODE can be read, then DP/AP writes often return raw ACK=0x05.
         * 0x05 is not a legal SWD ACK. The useful interpretation for this
         * bit-banged P4->STM32 wiring is:
         *   - bit0=1 and bit1=0 are the real ACK OK bits;
         *   - bit2=1 is not a real ACK bit, but the already released/turnaround
         *     line sampled high through pull-up.
         *
         * In fix24 we treated ACK=0x05 as OK but still inserted the normal
         * target-to-host -> host-to-target turnaround clock before WDATA. That
         * puts WDATA one clock late in the early-release case. The next read
         * then sees a shifted word/parity error and the link collapses into
         * ACK=0x07. For ACK=0x05 writes, consume no additional turnaround cycle:
         * take SWDIO immediately and start WDATA on the next clock. */
        ESP_LOGW(TAG,
                 "CMSIS SWD write req=0x%lx raw ACK=0x05 treated as OK with no extra turnaround",
                 (unsigned long)request);
        write_ack5_no_extra_turnaround = true;
    }

    if (ack == SWD_ACK_OK || write_ack5_no_extra_turnaround) {
        if (request & SWD_REQ_RnW) {
            /* Target-to-host data phase. */
            val = 0;
            parity = 0;
            for (uint32_t n = 32; n; --n) {
                bit = swd_read_bit();
                parity += bit;
                val >>= 1;
                val |= bit << 31;
            }

            bit = swd_read_bit();               /* Read parity */
            if (((parity ^ bit) & 1U) != 0U) {
                ESP_LOGW(TAG,
                         "CMSIS SWD parity error req=0x%lx val=0x%08" PRIx32 " parity_sum=%lu parity_bit=%lu",
                         (unsigned long)request,
                         val,
                         (unsigned long)(parity & 1U),
                         (unsigned long)bit);
                ack = 0U;                       /* Protocol-style error for caller. */
            }
            if (data) {
                *data = val;
            }

            /* Turnaround: target releases, host takes line back. */
            for (uint32_t n = SWD_TURNAROUND_CYCLES; n; --n) {
                swd_clock_cycle();
            }
            swdio_output_enable();
        } else {
            /* Turnaround: target releases ACK, host takes line before WDATA.
             * If raw ACK=0x05 was accepted above, the high third "ACK" bit is
             * assumed to be that release/turnaround cycle already. Do not add
             * one more clock or WDATA becomes shifted by one bit. */
            if (!write_ack5_no_extra_turnaround) {
                for (uint32_t n = SWD_TURNAROUND_CYCLES; n; --n) {
                    swd_clock_cycle();
                }
            }
            swdio_output_enable();

            val = data ? *data : 0U;
            parity = 0;
            for (uint32_t n = 32; n; --n) {
                bit = val & 1U;
                swd_write_bit(bit);             /* WDATA[0:31] */
                parity += bit;
                val >>= 1;
            }
            swd_write_bit(parity & 1U);         /* Write parity */
        }

        /* SWD idle cycles. Keep line low for clocks, then return high. */
        if (SWD_IDLE_CYCLES) {
            swdio_drive(0U);
            for (uint32_t n = SWD_IDLE_CYCLES; n; --n) {
                swd_clock_cycle();
            }
        }
        swdio_drive(1U);
        return SWD_ACK_OK;
    }

    if ((ack == SWD_ACK_WAIT) || (ack == SWD_ACK_FAULT)) {
        /* WAIT/FAULT has no successful data payload in our configuration. */
        for (uint32_t n = SWD_TURNAROUND_CYCLES; n; --n) {
            swd_clock_cycle();
        }
        swdio_output_enable();
        swdio_drive(1U);
        return (uint8_t)ack;
    }

    /* Protocol error: back off enough clocks to let target release any phase. */
    for (uint32_t n = SWD_TURNAROUND_CYCLES + 32U + 1U; n; --n) {
        swd_clock_cycle();
    }
    swdio_output_enable();
    swdio_drive(1U);
    return (uint8_t)ack;
}

static esp_err_t swd_transfer_checked(bool ap, bool read, uint8_t addr, uint32_t *data)
{
    const uint32_t req = swd_make_request(ap, read, addr);
    esp_err_t last_err = ESP_FAIL;

    for (int retry = 0; retry < SWD_RETRY_COUNT; ++retry) {
        uint8_t ack = cmsis_swd_transfer(req, data);
        if (ack == SWD_ACK_OK) {
            return ESP_OK;
        }

        last_err = swd_ack_to_err(ack);

        /* WAIT is a valid SWD response: just give the target a little time. */
        if (ack == SWD_ACK_WAIT) {
            if (retry < 3 || retry == SWD_RETRY_COUNT - 1) {
                ESP_LOGW(TAG,
                         "CMSIS SWD %s %s 0x%02x ACK=0x%02x (%s) retry=%d",
                         read ? "read" : "write",
                         ap ? "AP" : "DP",
                         addr,
                         ack,
                         swd_ack_name(ack),
                         retry);
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        /* FAULT is a real target-side error. Clear sticky errors and let the
         * higher layer decide what to do next. Do not blindly loop forever. */
        if (ack == SWD_ACK_FAULT) {
            ESP_LOGW(TAG,
                     "CMSIS SWD %s %s 0x%02x ACK=0x%02x (%s) retry=%d",
                     read ? "read" : "write",
                     ap ? "AP" : "DP",
                     addr,
                     ack,
                     swd_ack_name(ack),
                     retry);
            return ESP_FAIL;
        }

        /* Protocol/parity error. This is what we saw during STM32 OTA at
         * DP CTRL/STAT read: a valid-looking word followed by a bad parity bit,
         * then ACK=0x7 forever. Do not keep hammering in a desynchronized state.
         * Re-line-reset and retry the same transfer a few times. */
        ESP_LOGW(TAG,
                 "CMSIS SWD %s %s 0x%02x ACK=0x%02x (%s) retry=%d -> line reset recovery",
                 read ? "read" : "write",
                 ap ? "AP" : "DP",
                 addr,
                 ack,
                 swd_ack_name(ack),
                 retry);
        swd_protocol_recover();
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return last_err;
}

static esp_err_t dp_read(uint8_t addr, uint32_t *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    return swd_transfer_checked(false, true, addr, value);
}

static esp_err_t dp_write(uint8_t addr, uint32_t value)
{
    return swd_transfer_checked(false, false, addr, &value);
}

static esp_err_t ap_write(uint8_t addr, uint32_t value)
{
    return swd_transfer_checked(true, false, addr, &value);
}

static esp_err_t ap_read_posted(uint8_t addr)
{
    uint32_t ignored = 0;
    return swd_transfer_checked(true, true, addr, &ignored);
}

static esp_err_t ap_read(uint8_t addr, uint32_t *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ap_read_posted(addr), TAG, "AP posted read request failed addr=0x%02x", addr);
    return dp_read(DP_ADDR_RDBUFF, value);
}

static esp_err_t dap_read_idcode_standard(uint32_t *dp_idcode)
{
    if (!dp_idcode) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Reading DP IDCODE using CMSIS-style SWD_Transfer");
    esp_err_t ret = dp_read(DP_ADDR_IDCODE, dp_idcode);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "DP IDCODE: 0x%08" PRIx32, *dp_idcode);
    }
    return ret;
}

static esp_err_t dap_init(uint32_t *dp_idcode)
{
    if (!dp_idcode) {
        return ESP_ERR_INVALID_ARG;
    }

    *dp_idcode = 0;

    swd_enter_swd_from_jtag();
    esp_err_t ret = dap_read_idcode_standard(dp_idcode);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "DP IDCODE after SWJ switch failed: %s; trying SWD line-reset only", esp_err_to_name(ret));
        swd_reconnect_swd_only();
        ret = dap_read_idcode_standard(dp_idcode);
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "DP IDCODE read failed");

    ESP_RETURN_ON_ERROR(dp_write(DP_ADDR_ABORT, DP_ABORT_ALL), TAG, "DP ABORT failed");
    ESP_RETURN_ON_ERROR(dp_write(DP_ADDR_CTRL_STAT, DP_CTRL_PWRUP_REQ), TAG, "DP CTRL/STAT power-up request failed");

    int ctrl_read_failures = 0;
    for (int i = 0; i < 50; ++i) {
        uint32_t ctrl = 0;
        ret = dp_read(DP_ADDR_CTRL_STAT, &ctrl);
        if (ret == ESP_OK) {
            ctrl_read_failures = 0;
            if ((ctrl & DP_CTRL_PWRUP_ACK) == DP_CTRL_PWRUP_ACK) {
                ESP_LOGI(TAG, "DP CTRL/STAT power-up ACK: 0x%08" PRIx32, ctrl);
                return ESP_OK;
            }
        } else {
            ++ctrl_read_failures;
            ESP_LOGW(TAG, "DP CTRL/STAT read failed during power-up poll: %s; resyncing SWD", esp_err_to_name(ret));
            swd_protocol_recover();
            if (ctrl_read_failures >= 4) {
                ESP_LOGW(TAG, "DP CTRL/STAT power-up poll lost SWD framing %d times; aborting this connect attempt", ctrl_read_failures);
                return ESP_FAIL;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    ESP_LOGW(TAG, "DP power-up ACK timeout");
    return ESP_ERR_TIMEOUT;
}

static esp_err_t ahb_ap_select_bank(uint32_t ap_bank)
{
    /* APSEL=0. APBANKSEL lives in SELECT[7:4]. */
    return dp_write(DP_ADDR_SELECT, (ap_bank & 0xFU) << 4);
}

static esp_err_t ahb_ap_setup(void)
{
    ESP_RETURN_ON_ERROR(ahb_ap_select_bank(0), TAG, "DP SELECT AP0 bank0 failed");
    ESP_RETURN_ON_ERROR(ap_write(AP_ADDR_CSW, AHB_AP_CSW_32BIT), TAG, "AP CSW setup failed");
    return ESP_OK;
}

static esp_err_t ahb_ap_read_idr(uint32_t *ap_idr)
{
    if (!ap_idr) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ahb_ap_select_bank(0xFU), TAG, "DP SELECT AP0 bankF failed");
    esp_err_t ret = ap_read(AP_ADDR_IDR, ap_idr);
    (void)ahb_ap_select_bank(0);
    return ret;
}

static esp_err_t ahb_read32(uint32_t addr, uint32_t *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ahb_ap_setup(), TAG, "AHB-AP setup failed");
    ESP_RETURN_ON_ERROR(ap_write(AP_ADDR_TAR, addr), TAG, "AP TAR write 0x%08" PRIx32 " failed", addr);
    ESP_RETURN_ON_ERROR(ap_read(AP_ADDR_DRW, value), TAG, "AP DRW read 0x%08" PRIx32 " failed", addr);
    return ESP_OK;
}

static esp_err_t ahb_setup_csw(uint32_t csw)
{
    ESP_RETURN_ON_ERROR(ahb_ap_select_bank(0), TAG, "DP SELECT AP0 bank0 failed");
    ESP_RETURN_ON_ERROR(ap_write(AP_ADDR_CSW, csw), TAG, "AP CSW setup 0x%08" PRIx32 " failed", csw);
    return ESP_OK;
}

static esp_err_t ahb_write32_once(uint32_t addr, uint32_t value)
{
    ESP_RETURN_ON_ERROR(ahb_setup_csw(AHB_AP_CSW_32BIT), TAG, "AHB-AP 32-bit setup failed");
    ESP_RETURN_ON_ERROR(ap_write(AP_ADDR_TAR, addr), TAG, "AP TAR write 0x%08" PRIx32 " failed", addr);
    /* Give the MEM-AP time to accept the TAR write before the DRW write.
     * On the bench this is the first place that became flaky when going from
     * pure probe to real flash operations. */
    esp_rom_delay_us(50);
    ESP_RETURN_ON_ERROR(ap_write(AP_ADDR_DRW, value), TAG, "AP DRW write32 0x%08" PRIx32 " failed", addr);
    return ESP_OK;
}

static esp_err_t ahb_write32(uint32_t addr, uint32_t value)
{
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < 3; ++attempt) {
        ret = ahb_write32_once(addr, value);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG,
                 "AHB write32 0x%08" PRIx32 " failed on attempt %d: %s; SWD resync + ABORT",
                 addr,
                 attempt + 1,
                 esp_err_to_name(ret));
        swd_protocol_recover();
        (void)dp_write(DP_ADDR_ABORT, DP_ABORT_ALL);
        esp_rom_delay_us(100);
    }
    return ret;
}

static esp_err_t ahb_write16(uint32_t addr, uint16_t value)
{
    ESP_RETURN_ON_ERROR(ahb_setup_csw(AHB_AP_CSW_16BIT), TAG, "AHB-AP 16-bit setup failed");
    ESP_RETURN_ON_ERROR(ap_write(AP_ADDR_TAR, addr), TAG, "AP TAR write 0x%08" PRIx32 " failed", addr);

    /* MEM-AP sub-word accesses use the address bits to select the byte lane,
     * but the data must also be placed into the matching DRW lane.
     *
     * For little-endian 16-bit writes:
     *   addr & 2 == 0 -> DRW[15:0]
     *   addr & 2 == 2 -> DRW[31:16]
     *
     * The previous fix wrote every halfword into DRW[15:0]. That programmed
     * the first halfword of each word but left the upper halfword erased/zero,
     * causing verify like: actual=0x00001000 expected=0x20001000 at 0x08000000.
     */
    uint32_t drw = (uint32_t)value << ((addr & 0x2U) ? 16U : 0U);
    ESP_RETURN_ON_ERROR(ap_write(AP_ADDR_DRW, drw), TAG, "AP DRW write16 0x%08" PRIx32 " failed", addr);
    return ESP_OK;
}

static inline uint32_t ahb_write16_drw_lane(uint32_t addr, uint16_t value)
{
    /* MEM-AP sub-word accesses still require WDATA in the selected DRW lane. */
    return (uint32_t)value << ((addr & 0x2U) ? 16U : 0U);
}

static esp_err_t ahb_write16_stream_begin(uint32_t addr)
{
    ESP_RETURN_ON_ERROR(ahb_setup_csw(AHB_AP_CSW_16BIT), TAG, "AHB-AP 16-bit stream setup failed");
    ESP_RETURN_ON_ERROR(ap_write(AP_ADDR_TAR, addr), TAG, "AP TAR stream write 0x%08" PRIx32 " failed", addr);
    esp_rom_delay_us(20);
    return ESP_OK;
}

static esp_err_t ahb_write16_stream_drw(uint32_t addr, uint16_t value)
{
    const uint32_t drw = ahb_write16_drw_lane(addr, value);
    ESP_RETURN_ON_ERROR(ap_write(AP_ADDR_DRW, drw), TAG, "AP DRW stream write16 0x%08" PRIx32 " failed", addr);
    return ESP_OK;
}

static esp_err_t ahb_read16_flash(uint32_t addr, uint16_t *value)
{
    if (!value) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t word = 0;
    ESP_RETURN_ON_ERROR(ahb_read32(addr & ~0x3U, &word), TAG, "read16 backing word failed at 0x%08" PRIx32, addr);
    *value = (addr & 0x2U) ? (uint16_t)(word >> 16) : (uint16_t)(word & 0xFFFFU);
    return ESP_OK;
}

static esp_err_t swd_connect_after_reset_release(uint32_t *dp_idcode, const char *tag_reason)
{
    if (dp_idcode) {
        *dp_idcode = 0;
    }

    ESP_LOGI(TAG, "%s: assert NRST, release, then immediately initialise DAP",
             tag_reason ? tag_reason : "SWD reset-release connect");

    nrst_assert();
    vTaskDelay(pdMS_TO_TICKS(30));

    /* Keep SWCLK/SWDIO already prepared, then release reset and start SWD
     * almost immediately. Some STM32F0 boards do not answer DP IDCODE while
     * NRST is physically held low, but the user firmware may later reconfigure
     * PA13/PA14. This sequence catches the debug port in the small safe window
     * right after reset release. */
    nrst_release();
    esp_rom_delay_us(700);

    uint32_t id = 0;
    esp_err_t ret = dap_init(&id);
    if (ret == ESP_OK && dp_idcode) {
        *dp_idcode = id;
    }
    return ret;
}

static esp_err_t swd_connect_and_power(uint32_t *dp_idcode)
{
    if (dp_idcode) {
        *dp_idcode = 0;
    }

    ESP_LOGI(TAG, "SWD connect: NRST released");
    nrst_release();
    vTaskDelay(pdMS_TO_TICKS(30));

    uint32_t id = 0;
    esp_err_t ret = dap_init(&id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SWD normal connect failed: %s; trying reset-release connect", esp_err_to_name(ret));
        ret = swd_connect_after_reset_release(&id, "SWD connect fallback");
    }

    if (ret == ESP_OK && dp_idcode) {
        *dp_idcode = id;
    }
    return ret;
}

static esp_err_t cortexm_halt(void)
{
    /* Enable halt. DEMCR vector-catch is best-effort insurance for reset/halt
     * flows; if it fails, plain DHCSR halt can still work. */
    (void)ahb_write32(ARM_DEMCR_ADDR, ARM_DEMCR_VC_CORERESET);
    ESP_RETURN_ON_ERROR(ahb_write32(ARM_DHCSR_ADDR, ARM_DHCSR_DBGKEY | ARM_DHCSR_C_DEBUGEN | ARM_DHCSR_C_HALT),
                        TAG, "DHCSR halt request failed");
    for (int i = 0; i < 100; ++i) {
        uint32_t dhcsr = 0;
        esp_err_t ret = ahb_read32(ARM_DHCSR_ADDR, &dhcsr);
        if (ret == ESP_OK && (dhcsr & ARM_DHCSR_S_HALT)) {
            ESP_LOGI(TAG, "Cortex-M halted: DHCSR=0x%08" PRIx32, dhcsr);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGW(TAG, "Cortex-M halt timeout; continuing anyway because flash interface may still be accessible");
    return ESP_OK;
}

static esp_err_t swd_full_recover_and_halt(const char *reason)
{
    ESP_LOGW(TAG, "%s: full SWD/DAP recover + halt", reason ? reason : "SWD recover");

    swd_protocol_recover();
    (void)dp_write(DP_ADDR_ABORT, DP_ABORT_ALL);
    vTaskDelay(pdMS_TO_TICKS(2));

    uint32_t id = 0;
    esp_err_t ret = dap_init(&id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s: DAP re-init failed during recovery: %s",
                 reason ? reason : "SWD recover",
                 esp_err_to_name(ret));
        return ret;
    }

    ret = cortexm_halt();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s: halt after recovery failed: %s",
                 reason ? reason : "SWD recover",
                 esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t stm32_flash_restore_program_mode(const char *reason)
{
    ESP_RETURN_ON_ERROR(swd_full_recover_and_halt(reason), TAG, "recover DAP/halt failed");

    uint32_t cr = 0;
    ESP_RETURN_ON_ERROR(ahb_read32(STM32F0_FLASH_CR, &cr), TAG, "FLASH_CR read during program recovery failed");
    if (cr & STM32F0_FLASH_CR_LOCK) {
        ESP_LOGW(TAG, "%s: FLASH_CR is locked after recovery; writing unlock keys again",
                 reason ? reason : "program recovery");
        ESP_RETURN_ON_ERROR(ahb_write32(STM32F0_FLASH_KEYR, STM32F0_FLASH_KEY1), TAG, "FLASH_KEY1 recovery write failed");
        ESP_RETURN_ON_ERROR(ahb_write32(STM32F0_FLASH_KEYR, STM32F0_FLASH_KEY2), TAG, "FLASH_KEY2 recovery write failed");
        ESP_RETURN_ON_ERROR(ahb_read32(STM32F0_FLASH_CR, &cr), TAG, "FLASH_CR reread after recovery unlock failed");
    }

    cr &= ~(STM32F0_FLASH_CR_PER | STM32F0_FLASH_CR_STRT);
    cr |= STM32F0_FLASH_CR_PG;
    ESP_RETURN_ON_ERROR(ahb_write32(STM32F0_FLASH_CR, cr), TAG, "FLASH_CR PG restore failed");
    vTaskDelay(1);
    return ESP_OK;
}

static esp_err_t swd_connect_under_reset_then_halt(uint32_t *dp_idcode)
{
    if (dp_idcode) {
        *dp_idcode = 0;
    }

    ESP_LOGI(TAG, "SWD flash reset-release connect: assert NRST, release, DAP init, halt immediately");

    uint32_t id = 0;
    esp_err_t ret = swd_connect_after_reset_release(&id, "SWD flash reset-release");
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SWD reset-release DAP init failed: %s", esp_err_to_name(ret));
        nrst_release();
        vTaskDelay(pdMS_TO_TICKS(20));
        return ret;
    }

    ret = cortexm_halt();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Immediate halt after reset-release connect failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (dp_idcode) {
        *dp_idcode = id;
    }
    return ESP_OK;
}

static int stm32_flash_initial_wait_ms(const char *op)
{
    if (!op) {
        return 0;
    }

    /* Important bench finding:
     * Reading FLASH_SR immediately after setting STRT for a page erase can make
     * the AHB-AP transaction stall long enough that our software SWD stream loses
     * framing (ACK=0x05/0x07). Do not poll the flash controller while it is
     * certainly busy. Let the operation run for a conservative time, then read SR.
     */
    if (strstr(op, "page erase") != NULL) {
        return 45;        /* STM32F0 page erase is slow; 45 ms is safe on bench. */
    }
    if (strstr(op, "halfword program") != NULL) {
        return 0;         /* handled with sub-ms delay below */
    }
    return 0;
}

static int stm32_flash_initial_wait_us(const char *op)
{
    if (op && strstr(op, "halfword program") != NULL) {
        return 120;       /* avoid polling BSY immediately after each halfword. */
    }
    return 0;
}

static esp_err_t stm32_flash_wait_ready(const char *op, int timeout_ms)
{
    if (timeout_ms <= 0) {
        timeout_ms = 1000;
    }

    const int pre_ms = stm32_flash_initial_wait_ms(op);
    const int pre_us = stm32_flash_initial_wait_us(op);
    if (pre_ms > 0) {
        ESP_LOGI(TAG, "%s: blind wait %d ms before FLASH_SR polling", op ? op : "flash", pre_ms);
        vTaskDelay(pdMS_TO_TICKS(pre_ms));
        timeout_ms -= pre_ms;
        if (timeout_ms <= 0) {
            timeout_ms = 1;
        }
    } else if (pre_us > 0) {
        esp_rom_delay_us(pre_us);
    }

    esp_err_t last = ESP_OK;
    for (int i = 0; i < timeout_ms; ++i) {
        uint32_t sr = 0;
        last = ahb_read32(STM32F0_FLASH_SR, &sr);
        if (last != ESP_OK) {
            ESP_LOGW(TAG,
                     "%s: FLASH_SR read failed while polling (%s), SWD resync and retry",
                     op ? op : "flash",
                     esp_err_to_name(last));
            swd_protocol_recover();
            (void)dp_write(DP_ADDR_ABORT, DP_ABORT_ALL);
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if ((sr & STM32F0_FLASH_SR_BSY) == 0) {
            uint32_t clear = sr & (STM32F0_FLASH_SR_EOP | STM32F0_FLASH_SR_ERRS);
            if (clear) {
                (void)ahb_write32(STM32F0_FLASH_SR, clear);
            }
            if (sr & STM32F0_FLASH_SR_ERRS) {
                ESP_LOGE(TAG, "%s: FLASH_SR error=0x%08" PRIx32, op ? op : "flash", sr);
                return ESP_FAIL;
            }
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    ESP_LOGE(TAG, "%s: FLASH busy timeout / last=%s", op ? op : "flash", esp_err_to_name(last));
    return ESP_ERR_TIMEOUT;
}

static esp_err_t stm32_flash_unlock(void)
{
    uint32_t cr = 0;
    ESP_RETURN_ON_ERROR(ahb_read32(STM32F0_FLASH_CR, &cr), TAG, "FLASH_CR read failed");
    if ((cr & STM32F0_FLASH_CR_LOCK) == 0) {
        ESP_LOGI(TAG, "STM32 flash already unlocked: CR=0x%08" PRIx32, cr);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Unlocking STM32F0 flash");
    ESP_RETURN_ON_ERROR(ahb_write32(STM32F0_FLASH_KEYR, STM32F0_FLASH_KEY1), TAG, "FLASH_KEY1 write failed");
    ESP_RETURN_ON_ERROR(ahb_write32(STM32F0_FLASH_KEYR, STM32F0_FLASH_KEY2), TAG, "FLASH_KEY2 write failed");
    ESP_RETURN_ON_ERROR(ahb_read32(STM32F0_FLASH_CR, &cr), TAG, "FLASH_CR verify read failed");
    if (cr & STM32F0_FLASH_CR_LOCK) {
        ESP_LOGE(TAG, "STM32 flash unlock failed: CR=0x%08" PRIx32, cr);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t stm32_flash_erase_page(uint32_t page_addr)
{
    ESP_RETURN_ON_ERROR(stm32_flash_wait_ready("before page erase", 1000), TAG, "flash not ready before erase");

    uint32_t cr = 0;
    ESP_RETURN_ON_ERROR(ahb_read32(STM32F0_FLASH_CR, &cr), TAG, "FLASH_CR read before erase failed");
    cr &= ~(STM32F0_FLASH_CR_PG | STM32F0_FLASH_CR_PER | STM32F0_FLASH_CR_STRT);
    ESP_RETURN_ON_ERROR(ahb_write32(STM32F0_FLASH_CR, cr | STM32F0_FLASH_CR_PER), TAG, "FLASH_CR PER set failed");
    ESP_RETURN_ON_ERROR(ahb_write32(STM32F0_FLASH_AR, page_addr), TAG, "FLASH_AR page addr failed");
    ESP_RETURN_ON_ERROR(ahb_write32(STM32F0_FLASH_CR, cr | STM32F0_FLASH_CR_PER | STM32F0_FLASH_CR_STRT), TAG, "FLASH_CR STRT failed");
    esp_err_t ret = stm32_flash_wait_ready("page erase", 1500);
    (void)ahb_write32(STM32F0_FLASH_CR, cr & ~STM32F0_FLASH_CR_PER);
    return ret;
}

static esp_err_t stm32_flash_program_wait_settle(const char *op, uint32_t addr, uint32_t off)
{
    /* Do not poll FLASH_SR after every halfword. On this bench STM32F0 can stall
     * the AHB-AP while flash is busy, and a software SWD master then tends to
     * lose framing. A conservative blind delay is slower but much more stable. */
    esp_rom_delay_us(STM32F0_PROG_BLIND_DELAY_US);

#if STM32F0_PROG_SR_CHECK_BYTES > 0
    if (((off + 2U) % STM32F0_PROG_SR_CHECK_BYTES) == 0U) {
        vTaskDelay(1);
        uint32_t sr = 0;
        esp_err_t ret = ahb_read32(STM32F0_FLASH_SR, &sr);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "%s: deferred FLASH_SR read failed after +0x%04" PRIx32 " addr=0x%08" PRIx32 " (%s); resync and continue cautiously",
                     op ? op : "program",
                     off,
                     addr,
                     esp_err_to_name(ret));
            swd_protocol_recover();
            (void)dp_write(DP_ADDR_ABORT, DP_ABORT_ALL);
            vTaskDelay(pdMS_TO_TICKS(2));
            return ESP_OK;
        }

        if (sr & STM32F0_FLASH_SR_ERRS) {
            ESP_LOGE(TAG,
                     "%s: FLASH_SR error after +0x%04" PRIx32 ": SR=0x%08" PRIx32,
                     op ? op : "program",
                     off,
                     sr);
            return ESP_FAIL;
        }

        if (sr & STM32F0_FLASH_SR_EOP) {
            (void)ahb_write32(STM32F0_FLASH_SR, STM32F0_FLASH_SR_EOP);
        }
    } else
#endif
    if (((off / 2U) % STM32F0_PROG_YIELD_EVERY_HW) == 0U) {
        /* Let IDLE0 run. Otherwise the software SWD tight loop trips the task
         * watchdog while the flash operation itself is still progressing OK. */
        vTaskDelay(1);
    }

    return ESP_OK;
}

static esp_err_t stm32_flash_program_halfwords(uint32_t flash_base, const uint8_t *image, size_t image_size)
{
    uint32_t cr = 0;
    ESP_RETURN_ON_ERROR(ahb_read32(STM32F0_FLASH_CR, &cr), TAG, "FLASH_CR read before program failed");
    cr &= ~(STM32F0_FLASH_CR_PER | STM32F0_FLASH_CR_STRT);
    ESP_RETURN_ON_ERROR(ahb_write32(STM32F0_FLASH_CR, cr | STM32F0_FLASH_CR_PG), TAG, "FLASH_CR PG set failed");

    ESP_LOGI(TAG,
             "STM32 flash program: streamed MEM-AP 16-bit DRW, chunk=%u bytes, blind delay %u us/halfword, yield every %u halfwords, SR check %s",
             (unsigned)STM32F0_PROG_STREAM_CHUNK_BYTES,
             (unsigned)STM32F0_PROG_BLIND_DELAY_US,
             (unsigned)STM32F0_PROG_YIELD_EVERY_HW,
#if STM32F0_PROG_SR_CHECK_BYTES > 0
             "periodic"
#else
             "disabled during program"
#endif
             );

    bool stream_open = false;

    for (size_t off = 0; off < image_size; off += 2) {
        const uint32_t addr = flash_base + (uint32_t)off;
        uint16_t hw = image[off];
        if (off + 1 < image_size) {
            hw |= (uint16_t)image[off + 1] << 8;
        } else {
            hw |= 0xFF00U;
        }

        if (!stream_open || ((off % STM32F0_PROG_STREAM_CHUNK_BYTES) == 0U)) {
            esp_err_t ret = ahb_write16_stream_begin(addr);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "program stream begin failed at +0x%04x (%s); full recovery",
                         (unsigned)off,
                         esp_err_to_name(ret));
                ESP_RETURN_ON_ERROR(stm32_flash_restore_program_mode("program stream begin"), TAG, "program stream recovery failed");
                ESP_RETURN_ON_ERROR(ahb_write16_stream_begin(addr), TAG, "program stream restart failed at +0x%zx", off);
            }
            stream_open = true;
        }

        esp_err_t ret = ESP_FAIL;
        for (int attempt = 0; attempt < STM32F0_PROG_RETRY_COUNT; ++attempt) {
            ret = ahb_write16_stream_drw(addr, hw);
            if (ret == ESP_OK) {
                break;
            }

            ESP_LOGW(TAG,
                     "flash stream write16 failed at +0x%04x addr=0x%08" PRIx32 " attempt=%d/%d: %s",
                     (unsigned)off,
                     addr,
                     attempt + 1,
                     STM32F0_PROG_RETRY_COUNT,
                     esp_err_to_name(ret));

            esp_err_t rec = stm32_flash_restore_program_mode("flash stream write16");
            if (rec != ESP_OK) {
                ret = rec;
                continue;
            }

            uint16_t actual = 0xFFFFU;
            if (ahb_read16_flash(addr, &actual) == ESP_OK) {
                if (actual == hw) {
                    ESP_LOGW(TAG, "halfword at +0x%04x was already programmed OK after recovery", (unsigned)off);
                    ret = ESP_OK;
                    break;
                }
                if ((actual & hw) != hw) {
                    ESP_LOGE(TAG,
                             "halfword at +0x%04x is not recoverable: actual=0x%04x expected=0x%04x",
                             (unsigned)off,
                             actual,
                             hw);
                    return ESP_ERR_INVALID_CRC;
                }
            }

            ESP_RETURN_ON_ERROR(ahb_write16_stream_begin(addr), TAG, "program stream restart failed at +0x%zx", off);
            stream_open = true;
        }

        ESP_RETURN_ON_ERROR(ret, TAG, "flash write16 failed at +0x%zx", off);
        ESP_RETURN_ON_ERROR(stm32_flash_program_wait_settle("halfword program", addr, (uint32_t)off),
                            TAG,
                            "flash settle failed at +0x%zx",
                            off);

        if ((off & 0x3FFU) == 0) {
            ESP_LOGI(TAG, "STM32 flash program progress: %u/%u bytes", (unsigned)off, (unsigned)image_size);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    uint32_t sr = 0;
    ESP_RETURN_ON_ERROR(ahb_read32(STM32F0_FLASH_SR, &sr), TAG, "FLASH_SR final read after program failed");
    if (sr & STM32F0_FLASH_SR_ERRS) {
        ESP_LOGE(TAG, "FLASH_SR final program error: SR=0x%08" PRIx32, sr);
        return ESP_FAIL;
    }
    if (sr & STM32F0_FLASH_SR_EOP) {
        (void)ahb_write32(STM32F0_FLASH_SR, STM32F0_FLASH_SR_EOP);
    }

    ESP_RETURN_ON_ERROR(ahb_read32(STM32F0_FLASH_CR, &cr), TAG, "FLASH_CR read after program failed");
    (void)ahb_write32(STM32F0_FLASH_CR, cr & ~STM32F0_FLASH_CR_PG);
    return ESP_OK;
}

static esp_err_t stm32_flash_verify(uint32_t flash_base, const uint8_t *image, size_t image_size)
{
    for (size_t off = 0; off < image_size; off += 4) {
        uint32_t expected = 0xFFFFFFFFU;
        uint32_t bytes = 0;
        for (unsigned i = 0; i < 4 && off + i < image_size; ++i) {
            bytes |= (uint32_t)image[off + i] << (8U * i);
        }
        if (image_size - off >= 4) {
            expected = bytes;
        } else {
            uint32_t mask = (1U << ((image_size - off) * 8U)) - 1U;
            expected = (0xFFFFFFFFU & ~mask) | bytes;
        }

        uint32_t actual = 0;
        ESP_RETURN_ON_ERROR(ahb_read32(flash_base + (uint32_t)off, &actual), TAG, "verify read failed at +0x%zx", off);
        if (actual != expected) {
            ESP_LOGE(TAG, "verify mismatch at 0x%08" PRIx32 ": actual=0x%08" PRIx32 " expected=0x%08" PRIx32,
                     flash_base + (uint32_t)off,
                     actual,
                     expected);
            return ESP_ERR_INVALID_CRC;
        }
        if ((off & 0x3FFU) == 0U) {
            vTaskDelay(1);
        }
    }
    ESP_LOGI(TAG, "STM32 flash verify OK: %u bytes", (unsigned)image_size);
    return ESP_OK;
}

static void cortexm_prepare_run_after_debug(void)
{
    /* We halted the core and enabled vector-catch while programming. If those
     * bits are left set, a physical NRST pulse can look like a failed reboot:
     * the MCU resets but immediately stops in the debugger. Clear vector-catch
     * and release C_HALT before the final reset. */
    esp_err_t ret = ahb_write32(ARM_DEMCR_ADDR, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "DEMCR clear before run failed/non-fatal: %s", esp_err_to_name(ret));
    }

    ret = ahb_write32(ARM_DHCSR_ADDR, ARM_DHCSR_DBGKEY | ARM_DHCSR_C_DEBUGEN);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "DHCSR run request before reset failed/non-fatal: %s", esp_err_to_name(ret));
    }

    /* Same reset mechanism as CMSIS __NVIC_SystemReset(): VECTKEY + SYSRESETREQ.
     * The following NRST pulse is still kept as a hardware-level fallback. */
    ret = ahb_write32(ARM_AIRCR_ADDR, ARM_AIRCR_VECTKEY | ARM_AIRCR_SYSRESETREQ);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AIRCR SYSRESETREQ failed/non-fatal: %s", esp_err_to_name(ret));
    } else {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void stm32_target_reset_run(bool swd_may_be_alive)
{
    ESP_LOGI(TAG, "Resetting STM32 target after SWD operation%s",
             swd_may_be_alive ? " and releasing debug halt" : " using hardware NRST only");
    if (swd_may_be_alive) {
        cortexm_prepare_run_after_debug();
    }
    nrst_assert();
    vTaskDelay(pdMS_TO_TICKS(20));
    nrst_release();
    vTaskDelay(pdMS_TO_TICKS(80));
    swd_bus_release_idle();
}

esp_err_t stm32_swd_programmer_flash_image(const uint8_t *image, size_t image_size, uint32_t flash_base)
{
#if !CONFIG_STM32_SWD_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!image || image_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_probe_task) {
        ESP_LOGW(TAG, "SWD probe task is already running; cannot start STM32 flash");
        return ESP_ERR_INVALID_STATE;
    }
    if (image_size > STM32F0_FLASH_MAX_BYTES) {
        ESP_LOGE(TAG, "STM32 image too large for STM32F030K6T6 flash: %u > %u", (unsigned)image_size, (unsigned)STM32F0_FLASH_MAX_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }
    if (flash_base < STM32F0_FLASH_BASE_ADDR || flash_base + image_size > STM32F0_FLASH_BASE_ADDR + STM32F0_FLASH_MAX_BYTES) {
        ESP_LOGE(TAG, "STM32 flash range invalid: base=0x%08" PRIx32 " size=%u", flash_base, (unsigned)image_size);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_swd_lock && xSemaphoreTake(s_swd_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_swd_delay_us = SWD_DELAY_US_DEFAULT;
    swd_bus_prepare();

    esp_err_t ret = ESP_OK;
    int64_t t0 = esp_timer_get_time();
    uint32_t dp_idcode = 0;
    uint32_t cpuid = 0;
    uint32_t page_count = (uint32_t)((image_size + STM32F0_FLASH_PAGE_SIZE - 1U) / STM32F0_FLASH_PAGE_SIZE);
    bool swd_connected = false;

    state_set(STM32_SWD_STATUS_BUSY, ESP_OK, "STM32 OTA flashing");
    state_set_values(0, 0, 0);

    ESP_LOGW(TAG, "STM32 flash image start: base=0x%08" PRIx32 " size=%u pages=%u bit_delay=%dus",
             flash_base,
             (unsigned)image_size,
             (unsigned)page_count,
             (int)s_swd_delay_us);

    ESP_LOGI(TAG, "STM32 flash: normal SWD connect + halt-first");
    ret = swd_connect_and_power(&dp_idcode);
    if (ret == ESP_OK) {
        ret = cortexm_halt();
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "STM32 flash: normal connect/halt failed: %s; trying connect-under-reset", esp_err_to_name(ret));
        ret = swd_connect_under_reset_then_halt(&dp_idcode);
    }
    if (ret != ESP_OK && s_swd_delay_us < SWD_DELAY_US_SLOW) {
        ESP_LOGW(TAG, "STM32 flash: connect failed at %u us delay; retrying once with slow %u us delay",
                 (unsigned)s_swd_delay_us,
                 (unsigned)SWD_DELAY_US_SLOW);
        s_swd_delay_us = SWD_DELAY_US_SLOW;
        swd_bus_prepare();
        ret = swd_connect_under_reset_then_halt(&dp_idcode);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "STM32 flash: SWD connect/halt failed: %s", esp_err_to_name(ret));
        goto done;
    }
    swd_connected = true;

    ret = ahb_read32(ARM_CPUID_ADDR, &cpuid);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "STM32 flash: CPUID read after halt failed: %s", esp_err_to_name(ret));
        goto done;
    }
    ESP_LOGI(TAG, "STM32 flash: DP_IDCODE=0x%08" PRIx32 " CPUID=0x%08" PRIx32 " halted=1", dp_idcode, cpuid);

    ESP_GOTO_ON_ERROR(stm32_flash_unlock(), done, TAG, "STM32 flash unlock failed");
    ESP_GOTO_ON_ERROR(stm32_flash_wait_ready("before full erase/program", 1000), done, TAG, "flash not ready");

    for (uint32_t page = 0; page < page_count; ++page) {
        uint32_t page_addr = flash_base + page * STM32F0_FLASH_PAGE_SIZE;
        ESP_LOGI(TAG, "STM32 flash erase page %u/%u at 0x%08" PRIx32, (unsigned)(page + 1), (unsigned)page_count, page_addr);
        ESP_GOTO_ON_ERROR(stm32_flash_erase_page(page_addr), done, TAG, "erase page failed");
    }

    ESP_GOTO_ON_ERROR(stm32_flash_program_halfwords(flash_base, image, image_size), done, TAG, "program failed");
    ESP_GOTO_ON_ERROR(stm32_flash_verify(flash_base, image, image_size), done, TAG, "verify failed");

    state_set_values(dp_idcode, cpuid, 0);
    state_set(STM32_SWD_STATUS_OK, ESP_OK, "STM32 flash OK");
    ESP_LOGW(TAG, "STM32 flash image OK in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));

done:
    if (ret != ESP_OK) {
        state_set_values(dp_idcode, cpuid, 0);
        state_set(STM32_SWD_STATUS_FAILED, ret, "STM32 flash failed");
        ESP_LOGE(TAG, "STM32 flash image failed: %s", esp_err_to_name(ret));
    }
    stm32_target_reset_run(swd_connected);
    s_swd_delay_us = SWD_DELAY_US_DEFAULT;
    if (s_swd_lock) {
        xSemaphoreGive(s_swd_lock);
    }
    return ret;
#endif
}

esp_err_t stm32_swd_programmer_init(void)
{
#if !CONFIG_STM32_SWD_ENABLE
    state_set(STM32_SWD_STATUS_IDLE, ESP_ERR_NOT_SUPPORTED, "SWD disabled");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!s_swd_lock) {
        s_swd_lock = xSemaphoreCreateMutex();
        if (!s_swd_lock) {
            state_set(STM32_SWD_STATUS_FAILED, ESP_ERR_NO_MEM, "SWD mutex create failed");
            return ESP_ERR_NO_MEM;
        }
    }

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << SWD_PIN_NRST) | (1ULL << SWD_PIN_SWCLK) | (1ULL << SWD_PIN_SWDIO),
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config reset failed");

    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(SWD_PIN_NRST, GPIO_PULLUP_ONLY), TAG, "NRST pull-up failed");
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(SWD_PIN_SWDIO, GPIO_PULLUP_ONLY), TAG, "SWDIO pull-up failed");
    swd_bus_prepare();

    ESP_LOGI(TAG,
             "STM32 SWD programmer GPIO init: CMSIS-style SWD_Transfer fix25 ack5-no-extra-turnaround reset-release, NRST=%d(open-drain style, PU) SWDIO=%d(PU) SWCLK=%d bit_delay=%dus turnaround=%u idle=%u task_prio=%d core=%d",
             (int)SWD_PIN_NRST,
             (int)SWD_PIN_SWDIO,
             (int)SWD_PIN_SWCLK,
             (int)s_swd_delay_us,
             SWD_TURNAROUND_CYCLES,
             SWD_IDLE_CYCLES,
             CONFIG_STM32_SWD_TASK_PRIORITY,
             CONFIG_STM32_SWD_TASK_CORE);

    state_set(STM32_SWD_STATUS_IDLE, ESP_OK, "ready");
    return ESP_OK;
#endif
}

esp_err_t stm32_swd_programmer_probe(void)
{
#if !CONFIG_STM32_SWD_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_swd_lock && xSemaphoreTake(s_swd_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_swd_delay_us = SWD_DELAY_US_DEFAULT;
    swd_bus_prepare();

    uint32_t dp_idcode = 0;
    uint32_t ap_idr = 0;
    uint32_t cpuid = 0;
    uint32_t dbgmcu_idcode = 0;
    int64_t t0 = esp_timer_get_time();

    state_set(STM32_SWD_STATUS_BUSY, ESP_OK, "probing");
    state_set_values(0, 0, 0);

    ESP_LOGI(TAG, "SWD probe start (CMSIS-style transfer core)");

    swd_scope_pin_test();

    ESP_LOGI(TAG, "SWD normal probe: NRST released");
    nrst_release();
    vTaskDelay(pdMS_TO_TICKS(30));

    esp_err_t ret = dap_init(&dp_idcode);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SWD normal probe failed: %s; trying reset-release probe", esp_err_to_name(ret));

        dp_idcode = 0;
        ret = swd_connect_after_reset_release(&dp_idcode, "SWD probe reset-release");
    }
    if (ret != ESP_OK && s_swd_delay_us < SWD_DELAY_US_SLOW) {
        ESP_LOGW(TAG, "SWD probe failed at %u us delay; retrying once with slow %u us delay",
                 (unsigned)s_swd_delay_us,
                 (unsigned)SWD_DELAY_US_SLOW);
        s_swd_delay_us = SWD_DELAY_US_SLOW;
        swd_bus_prepare();
        dp_idcode = 0;
        ret = swd_connect_after_reset_release(&dp_idcode, "SWD probe slow reset-release");
    }

    if (ret != ESP_OK) {
        nrst_release();
        state_set_values(dp_idcode, 0, 0);
        state_set(STM32_SWD_STATUS_FAILED, ret, "DP probe failed");
        ESP_LOGE(TAG, "SWD probe failed at DP init: %s", esp_err_to_name(ret));
        swd_bus_release_idle();
        s_swd_delay_us = SWD_DELAY_US_DEFAULT;
        if (s_swd_lock) {
            xSemaphoreGive(s_swd_lock);
        }
        return ret;
    }

    ret = ahb_ap_read_idr(&ap_idr);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "AHB-AP IDR: 0x%08" PRIx32, ap_idr);
    } else {
        ESP_LOGW(TAG, "AHB-AP IDR read failed/non-fatal for now: %s", esp_err_to_name(ret));
    }

    ret = ahb_read32(ARM_CPUID_ADDR, &cpuid);
    if (ret != ESP_OK) {
        state_set_values(dp_idcode, 0, 0);
        state_set(STM32_SWD_STATUS_FAILED, ret, "CPUID read failed");
        ESP_LOGE(TAG, "SWD probe failed at CPUID read: %s", esp_err_to_name(ret));
        swd_bus_release_idle();
        s_swd_delay_us = SWD_DELAY_US_DEFAULT;
        if (s_swd_lock) {
            xSemaphoreGive(s_swd_lock);
        }
        return ret;
    }

    ret = ahb_read32(STM32F0_DBGMCU_IDCODE_ADDR, &dbgmcu_idcode);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "DBGMCU_IDCODE read failed/non-fatal: %s", esp_err_to_name(ret));
        dbgmcu_idcode = 0;
        ret = ESP_OK;
    }

    state_set_values(dp_idcode, cpuid, dbgmcu_idcode);
    state_set(STM32_SWD_STATUS_OK, ESP_OK, "probe OK");

    int64_t t1 = esp_timer_get_time();
    ESP_LOGI(TAG,
             "SWD probe OK in %lld ms: DP_IDCODE=0x%08" PRIx32 " AP_IDR=0x%08" PRIx32 " CPUID=0x%08" PRIx32 " STM32F0_DBGMCU_IDCODE=0x%08" PRIx32,
             (long long)((t1 - t0) / 1000),
             dp_idcode,
             ap_idr,
             cpuid,
             dbgmcu_idcode);

    swd_bus_release_idle();
    s_swd_delay_us = SWD_DELAY_US_DEFAULT;
    if (s_swd_lock) {
        xSemaphoreGive(s_swd_lock);
    }
    return ESP_OK;
#endif
}

static void swd_probe_task(void *arg)
{
    (void)arg;
    stm32_swd_programmer_probe();
    s_probe_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t stm32_swd_programmer_probe_async(void)
{
#if !CONFIG_STM32_SWD_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_probe_task) {
        return ESP_ERR_INVALID_STATE;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(swd_probe_task,
                                            "stm32_swd_probe",
                                            CONFIG_STM32_SWD_TASK_STACK_SIZE,
                                            NULL,
                                            CONFIG_STM32_SWD_TASK_PRIORITY,
                                            &s_probe_task,
                                            CONFIG_STM32_SWD_TASK_CORE);
    if (ok != pdPASS) {
        s_probe_task = NULL;
        state_set(STM32_SWD_STATUS_FAILED, ESP_ERR_NO_MEM, "probe task create failed");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
#endif
}

stm32_swd_status_t stm32_swd_programmer_get_status(void)
{
    stm32_swd_status_t status;
    portENTER_CRITICAL(&s_state_mux);
    status = s_state.status;
    portEXIT_CRITICAL(&s_state_mux);
    return status;
}

const stm32_swd_state_t *stm32_swd_programmer_get_state(void)
{
    return &s_state;
}

const char *stm32_swd_programmer_get_status_text(void)
{
    switch (stm32_swd_programmer_get_status()) {
    case STM32_SWD_STATUS_IDLE:
        return "SWD IDLE";
    case STM32_SWD_STATUS_BUSY:
        return "SWD BUSY";
    case STM32_SWD_STATUS_OK:
        return "SWD OK";
    case STM32_SWD_STATUS_FAILED:
        return "SWD FAIL";
    default:
        return "SWD ?";
    }
}
