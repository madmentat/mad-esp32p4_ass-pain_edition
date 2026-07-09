# mad-esp32p4_ass-pain_edition

Рабочий шаблон для ESP32-P4 + JC4880P443C с OTA-обновлением и встроенным STM32 SWD программатором.

Прошивка для ESP32-P4, превращающая модуль JC4880P443C (480x800 MIPI-DSI, ST7701, GT911 тачскрин) в полноценную IoT-платформу с MMI-интерфейсом на фоновых изображениях, удалённым обновлением прошивки и программированием встроенного STM32-копроцессора прямо по воздуху. **Экран не моргает ни при одной операции OTA.**

---

## Возможности

- **OTA-обновления** — прошивка обновляется без мерцания экрана
- **STM32 SWD программатор** — программирование STM32-копроцессора по воздуху
- **EEZ LVGL UI** — MMI-интерфейс с фоновыми изображениями
- **MIPI-DSI дисплей** — ST7701, 480x800, 2 линии данных
- **Capacitive touch** — GT911 через I2C
- **WiFi** — через ESP32-C6 копроцессор (ESP-Hosted)
- **Несколько бэкендов WiFi** — AT-команды, Service, Hosted (SDIO)

---

## Аппаратная часть

| Параметр | Значение |
|----------|----------|
| MCU | ESP32-P4 |
| Дисплейный модуль | JC4880P443C (AliExpress) |
| Контроллер дисплея | ST7701 |
| Разрешение | 480 × 800 (портрет) |
| Интерфейс дисплея | MIPI-DSI, 2 линии данных |
| Тачскрин | GT911, capacitive, I2C |
| Flash | 16 MB |
| PSRAM | Требуется (SPIRAM) |

### Назначение пинов

| Сигнал | GPIO |
|--------|------|
| LCD reset | GPIO 5 |
| Backlight PWM | GPIO 23 |
| Touch SDA | GPIO 7 |
| Touch SCL | GPIO 8 |
| Touch RST | GPIO 35 |
| Touch INT | GPIO 3 |
| MIPI PHY LDO | канал 3 (2500 mV) |

---

## Структура проекта

```
mad-esp32p4_ass-pain_edition/
├── components/
│   └── jc4880p443c/              Последовательность инициализации ST7701
│       ├── jc4880p443c.c         Инициализационные команды (39 команд)
│       ├── jc4880p443c.h         Публичный API
│       └── idf_component.yml
├── main/
│   ├── jc4880p443c_demo.c        Точка входа: дисплей, тач, LVGL, app_main
│   ├── stm32_swd_programmer.c    SWD программатор STM32
│   ├── update/update_manager.c   Менеджер OTA-обновлений
│   ├── eez_ui_port.c             Порт EEZ LVGL UI
│   ├── eez_ui_runtime.c          Рантайм EEZ UI
│   ├── ui_background_direct.c    Фоновые изображения
│   ├── display_direct_timing.c   Тайминги дисплея
│   ├── display_experiments.c     Эксперименты с дисплеем
│   ├── wifi_manager*.c           WiFi бэкенды (AT/Service/Hosted/Stub)
│   ├── src/ui/                   EEZ UI файлы (изображения, стили, экраны)
│   ├── Kconfig.projbuild         Конфигурация WiFi/OTA через menuconfig
│   ├── CMakeLists.txt
│   └── idf_component.yml
├── partitions.csv                Таблица разделов (16 MB)
├── partitions_ota_16mb.csv       Таблица разделов с OTA
├── sdkconfig                     Текущая конфигурация
├── sdkconfig.defaults            Конфигурация по умолчанию для ESP32-P4
├── deploy.ps1 / deploy.sh       Скрипты деплоя
└── setup.ps1 / setup.sh         Скрипты настройки
```

---

## Требования

- **ESP-IDF 5.5.x** — проект использует API `i2c_master` из IDF 5.x
- **Python 3.8+** (для IDF tools)

Автоматически подтягиваются IDF Component Manager:

| Компонент | Версия |
|-----------|--------|
| `lvgl/lvgl` | ^9.0.0 |
| `espressif/esp_lcd_st7701` | ^2.0.2 |
| `espressif/esp_lcd_touch` | ^1.1.0 |
| `espressif/esp_lcd_touch_gt911` | ^1.1.0 |
| `espressif/esp_wifi_remote` | >=0.10,<2.0 |
| `espressif/esp_hosted` | ~2 |

> **О WiFi:** ESP32-P4 не имеет встроенного WiFi. Используется внешний копроцессор ESP32-C6 через `esp_wifi_remote` и `esp_hosted`.

---

## Быстрый старт

### 1. Клонирование

```bash
git clone https://github.com/madmentat/mad-esp32p4_ass-pain_edition.git
cd mad-esp32p4_ass-pain_edition
```

### 2. Выбор цели

```bash
idf.py set-target esp32p4
```

### 3. Конфигурация

```bash
idf.py menuconfig
```

Настройте WiFi и бэкенд подключения.

### 4. Сборка и прошивка

```bash
idf.py -p COM3 build flash monitor
```

---

## Бэкенды WiFi

Проект поддерживает несколько способов подключения ESP32-C6:

| Бэкенд | Описание |
|--------|----------|
| ESP32-C6 AT | AT-команды через UART |
| ESP32-C6 Service | Service-режим |
| ESP32-C6 Hosted | SDIO/SPI Hosted (рекомендуется) |
| Stub | Заглушка для тестирования без WiFi |

Выбор через `idf.py menuconfig` → Dashboard WiFi Backend.

---

## OTA-обновления

Менеджер обновлений (`update_manager.c`) обеспечивает:
- Загрузку прошивки по HTTP/HTTPS
- Запись в分区 OTA
- Переключение на новую прошивку
- **Без мерцания экрана** во время всей операции

---

## STM32 SWD Программатор

Встроенный SWD программатор (`stm32_swd_programmer.c`) позволяет:
- Программировать STM32-копроцессор по воздуху
- Использовать ESP32-P4 как SWD хост
- Обновлять прошивку STM32 без физического доступа к плате

---

## Лицензия

MIT. Используйте как хотите.
