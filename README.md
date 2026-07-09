# mad-esp32p4_ass-pain_edition

Working template for ESP32-P4 + JC4880P443C with OTA firmware updates and a built-in STM32 SWD programmer.

Firmware for ESP32-P4 that turns the JC4880P443C display module (480x800 MIPI-DSI, ST7701, GT911 capacitive touch) into a full IoT platform with an MMI interface backed by background images, over-the-air firmware updates, and in-system programming of the onboard STM32 co-processor. **The screen never blinks during any OTA operation.**

---

## Features

- **OTA updates** — firmware updates without screen flickering
- **STM32 SWD programmer** — program the STM32 co-processor wirelessly
- **EEZ LVGL UI** — MMI interface with background images
- **MIPI-DSI display** — ST7701, 480x800, 2 data lanes
- **Capacitive touch** — GT911 via I2C
- **WiFi** — via ESP32-C6 co-processor (ESP-Hosted)
- **Multiple WiFi backends** — AT commands, Service, Hosted (SDIO)

---

## Hardware

| Parameter | Value |
|-----------|-------|
| MCU | ESP32-P4 |
| Display module | JC4880P443C (AliExpress) |
| Display controller | ST7701 |
| Resolution | 480 × 800 portrait |
| Display interface | MIPI-DSI, 2 data lanes |
| Touch controller | GT911 capacitive, I2C |
| Flash | 16 MB |
| PSRAM | Required (SPIRAM) |

### Pin Assignments

| Signal | GPIO |
|--------|------|
| LCD reset | GPIO 5 |
| Backlight PWM | GPIO 23 |
| Touch SDA | GPIO 7 |
| Touch SCL | GPIO 8 |
| Touch RST | GPIO 35 |
| Touch INT | GPIO 3 |
| MIPI PHY LDO | channel 3 (2500 mV) |

---

## Project Structure

```
mad-esp32p4_ass-pain_edition/
├── components/
│   └── jc4880p443c/              ST7701 init sequence + timing constants
│       ├── jc4880p443c.c         Validated init commands (39 commands)
│       ├── jc4880p443c.h         Public API
│       └── idf_component.yml
├── main/
│   ├── jc4880p443c_demo.c        Entry point: display, touch, LVGL, app_main
│   ├── stm32_swd_programmer.c    SWD programmer for STM32
│   ├── update/update_manager.c   OTA update manager
│   ├── eez_ui_port.c             EEZ LVGL UI port
│   ├── eez_ui_runtime.c          EEZ UI runtime
│   ├── ui_background_direct.c    Background image rendering
│   ├── display_direct_timing.c   Display timing parameters
│   ├── display_experiments.c     Display experiments
│   ├── wifi_manager*.c           WiFi backends (AT/Service/Hosted/Stub)
│   ├── src/ui/                   EEZ UI files (images, styles, screens)
│   ├── Kconfig.projbuild         WiFi/OTA config via menuconfig
│   ├── CMakeLists.txt
│   └── idf_component.yml
├── partitions.csv                Partition table (16 MB)
├── partitions_ota_16mb.csv       Partition table with OTA
├── sdkconfig                     Current configuration
├── sdkconfig.defaults            Default configuration for ESP32-P4
├── deploy.ps1 / deploy.sh       Deploy scripts
└── setup.ps1 / setup.sh         Setup scripts
```

---

## Requirements

- **ESP-IDF 5.5.x** — uses the `i2c_master` driver API from IDF 5.x
- **Python 3.8+** (for IDF tools)

Auto-fetched by IDF Component Manager:

| Component | Version |
|-----------|---------|
| `lvgl/lvgl` | ^9.0.0 |
| `espressif/esp_lcd_st7701` | ^2.0.2 |
| `espressif/esp_lcd_touch` | ^1.1.0 |
| `espressif/esp_lcd_touch_gt911` | ^1.1.0 |
| `espressif/esp_wifi_remote` | >=0.10,<2.0 |
| `espressif/esp_hosted` | ~2 |

> **Note on WiFi:** The ESP32-P4 has no built-in WiFi. It uses an external ESP32-C6 co-processor via `esp_wifi_remote` and `esp_hosted`.

---

## Quick Start

### 1. Clone

```bash
git clone https://github.com/madmentat/mad-esp32p4_ass-pain_edition.git
cd mad-esp32p4_ass-pain_edition
```

### 2. Set target

```bash
idf.py set-target esp32p4
```

### 3. Configure

```bash
idf.py menuconfig
```

Set up WiFi and connection backend.

### 4. Build and flash

```bash
idf.py -p COM3 build flash monitor
```

---

## WiFi Backends

The project supports multiple ways to connect the ESP32-C6:

| Backend | Description |
|---------|-------------|
| ESP32-C6 AT | AT commands via UART |
| ESP32-C6 Service | Service mode |
| ESP32-C6 Hosted | SDIO/SPI Hosted (recommended) |
| Stub | Stub for testing without WiFi |

Select via `idf.py menuconfig` → Dashboard WiFi Backend.

---

## OTA Updates

The update manager (`update_manager.c`) provides:
- HTTP/HTTPS firmware download
- Writing to OTA partition
- Switching to new firmware
- **No screen flickering** during the entire operation

---

## STM32 SWD Programmer

The built-in SWD programmer (`stm32_swd_programmer.c`) allows:
- Programming the STM32 co-processor wirelessly
- Using ESP32-P4 as an SWD host
- Updating STM32 firmware without physical access to the board

---

## License

MIT. Do whatever you like with it.
