# mad-esp32p4_ass-pain_edition

Автор проекта: [madmentat.ru](https://madmentat.ru)

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

#### Дисплей (MIPI-DSI + подсветка)

| Сигнал | GPIO | Зачем нужен |
|--------|------|-------------|
| LCD reset | 5 | Сброс контроллера ST7701 при инициализации |
| Backlight PWM | 23 | Управление яркостью подсветки через ШИМ |
| MIPI PHY LDO | канал 3, 2500 mV | Питание PHY-слоя MIPI-DSI (внутренний LDO ESP32-P4, не GPIO) |

> Данные MIPI-DSI (DPI-clk, D0–D3, CKE, CS, DE, HSYNC, VSYNC) идут через внутреннюю шину ESP32-P4 — отдельные GPIO для них не назначаются.

#### Тачскрин (GT911, I2C)

| Сигнал | GPIO | Зачем нужен |
|--------|------|-------------|
| Touch SDA | 7 | Линия данных I2C ( bidirectional ) |
| Touch SCL | 8 | Линия тактирования I2C |
| Touch RST | 22 | Аппаратный сброс GT911 (активный LOW) |
| Touch INT | 21 | Прерывание от GT911 (оповещение о касании) |

> Адрес I2C GT911 зависит от состояния INT при сбросе: INT LOW → `0x5D` (по умолчанию), INT HIGH → `0x14`.

#### STM32 SWD программатор

| Сигнал | GPIO | Зачем нужен |
|--------|------|-------------|
| SWCLK | 31 | Тактирование SWD-интерфейса (подтяжка к VDD 4.7–10k обязательна) |
| SWDIO | 33 | Данные SWD (bidirectional, подтяжка к VDD 4.7–10k обязательна) |
| NRST | 29 | Сброс целевого STM32 (подтяжка к VDD 10k обязательна) |

> Все три линии требуют подтяжек на стороне целевой платы (STM32). Без них SWD-транзакции будут нестабильными.

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
├── setup.ps1                     Интерактивная настройка WiFi/MQTT/OTA (Windows)
├── setup.sh                      Интерактивная настройка WiFi/MQTT/OTA (Linux/macOS)
└── README.md
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

## Скрипт настройки (setup)

Интерактивный скрипт для быстрой настройки WiFi, MQTT и OTA-SSID в `sdkconfig`:

```bash
# Windows (PowerShell):
powershell -ExecutionPolicy Bypass -File setup.ps1

# Linux / macOS:
bash setup.sh
```

Скрипт спросит:
1. **WiFi SSID** и пароль
2. **MQTT broker** (например `mqtt://192.168.1.10`) — можно пропустить
3. **OTA local SSID** —SSID локальной WiFi-сети для OTA-обновлений — можно пропустить

Если `sdkconfig` уже содержит реальные значения (не плейсхолдеры), скрипт предложит перезаписать.

> **Примечание:** `setup.sh` требует GNU grep (`grep -oP`). На macOS из коробки не работает — установите `grep` через `brew install grep`.

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

Проект включает программный SWD-мастер (`stm32_swd_programmer.c`), реализованный на ESP32-P4 через bit-banging GPIO. Позволяет программировать встроенный STM32F030-копроцессор прямо по воздуху, без физического доступа к плате и без внешнего программатора.

### Назначение GPIO для SWD

| Сигнал | GPIO по умолчанию | Описание |
|--------|-------------------|----------|
| NRST | GPIO 29 | Reset целевого STM32 |
| SWDIO | GPIO 33 | Данные SWD (двунаправленная линия) |
| SWCLK | GPIO 31 | Тактирование SWD |

НазначениеGPIO настраивается через `idf.py menuconfig` → **STM32F030 SWD Programmer**.

### Подтяжки (pull-up / pull-down)

Для стабильной работы SWD-линка **необходимы** подтяжки на целевой стороне (STM32):

| Линия | Требование | Пояснение |
|-------|------------|-----------|
| **SWCLK** | Pull-up к VDD (4.7k — 10k) | Линия тактирования должна быть в HIGH по умолчанию. Без подтяжки ESP32-P4 может считать шум как тактовые импульсы, что ломает SWD-транзакции |
| **SWDIO** | Pull-up к VDD (4.7k — 10k) | Линия данных в состоянии покоя должна быть HIGH (режим чтения). Без подтяжки ACK-биты будут нестабильными |
| **NRST** | Pull-up к VDD (10k) | Reset должен быть неактивен (HIGH) во время работы SWD. Без подтяжки STM32 может случайно уйти в reset |

> **Важно:**这些 подтяжки должны быть на стороне целевой платы (STM32), а не на стороне ESP32-P4. Большинство отладочных板 (ST-Link, J-Link) имеют встроенные подтяжки — при программировании через наш SWD-мастер их нужно обеспечить самостоятельно.

### Как это работает

Программатор реализует полный стек CMSIS-DAP через bit-banging:

1. **SWD line reset** — последовательность 50+ тактов с SWDIO=LOW сбивает состояние SWD
2. **DP IDCODE read** — чтение идентификатора debug port ( Cortex-M0: `0x0BC11477` )
3. **Debug port power-up** — запрос CTRL/STAT с битом `CSYSPWRUPREQ` и `CDBGPWRUPREQ`
4. **AP ID read** — чтение MEM-AP IDR для определения типа доступа к памяти
5. **Flash programming** — unlock → page erase → half-word program → verify → SYSRESETREQ

Протокол соответствует спецификации **ARM Debug Interface Architecture Specification (ADIv5.2)**:

- [ARM ADIv5.2 Specification (ARM IHI 0031)](https://developer.arm.com/documentation/ihi0031/latest/)
- [ARM Cortex-M0 Technical Reference Manual](https://developer.arm.com/documentation/ddi0419/latest/)
- [ARM SWD (Serial Wire Debug) Protocol](https://developer.arm.com/documentation/ddi0314/h/Serial-Wire-Debug--SWD--interface)

### Конфигурация через menuconfig

```
idf.py menuconfig
  └── STM32F030 SWD Programmer
        ├── Enable software SWD programmer for STM32F030 (y/n)
        ├── STM32 NRST GPIO (default: 29)
        ├── STM32 SWDIO GPIO (default: 33)
        ├── STM32 SWCLK GPIO (default: 31)
        ├── SWD bit half-period delay, us (default: 2)
        └── SWD probe task CPU core (default: 0)
```

---

## Интеграция с EEZ Studio

Проект совместим с [EEZ Studio](https://eez-studio.com/) — визуальным редактором LVGL-интерфейсов. Весь MMI-интерфейс (экраны, стили, изображения, виджеты) генерируется EEZ Studio и размещается в папке `main/src/ui/`.

### Импорт UI из EEZ Studio проекта

1. Откройте ваш проект в EEZ Studio
2. Экспортируйте LVGL-проект (File → Export → LVGL)
3. Скопируйте папку `src/` из экспортированного проекта в `main/` этого репозитория, заменив существующую:

```bash
# Из папки экспорта EEZ Studio:
cp -r ./exported_project/src/ ./main/src/
```

4. Пересоберите проект:

```bash
idf.py build
```

Порт EEZ UI (`eez_ui_port.c`, `eez_ui_runtime.c`) обеспечивает совместимость между сгенерированным EEZ кодом и средой выполнения ESP-IDF. Файлы `main/src/ui/` содержат:
- `ui.c / ui.h` — инициализация UI, создание экранов
- `screens.c / screens.h` — определения экранов
- `styles.c / styles.h` — стили виджетов
- `images.c / images.h` — LVGL-обёртки над изображениями
- `ui_image_*.c` — массивы данных изображений (закодированные в C)

> **Совет:** При импорте нового UI из EEZ Studio убедитесь, что версия LVGL в EEZ Studio совпадает с версией в проекте (^9.0.0). Несовместимости между LVGL 8.x и 9.x сломают сборку.

---

## Лицензия

MIT. Используйте как хотите.
