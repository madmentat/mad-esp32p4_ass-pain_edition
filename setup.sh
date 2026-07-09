#!/usr/bin/env bash
# setup.sh — настраивает sdkconfig: WiFi, OTA сервер
# Запуск: bash setup.sh
#
# После запуска:
#   1. Введите WiFi SSID
#   2. Введите WiFi пароль
#   3. Введите OTA local SSID (или Enter чтобы пропустить)

set -euo pipefail

src="$(cd "$(dirname "$0")" && pwd)"
sdkconfig="$src/sdkconfig"

if [ ! -f "$sdkconfig" ]; then
    echo "ERROR: sdkconfig not found"
    exit 1
fi

echo "=== WiFi & OTA Setup ==="
echo ""

# Проверяем placeholders
if ! grep -q "YOUR_WIFI_SSID\|YOUR_WIFI_PASSWORD\|YOUR_OTA_LOCAL_SSID" "$sdkconfig"; then
    echo "sdkconfig already has real credentials (no placeholders found)."
    read -p "Overwrite? (y/N) " overwrite
    if [ "$overwrite" != "y" ]; then
        echo "Skipped."
        exit 0
    fi
fi

# --- WiFi SSID ---
echo ""
echo "Step 1/3: WiFi"
current_ssid=$(grep -oP 'CONFIG_ESP_WIFI_SSID="\K[^"]+' "$sdkconfig" 2>/dev/null || echo "")
if [ -n "$current_ssid" ] && [ "$current_ssid" != "YOUR_WIFI_SSID" ]; then
    echo "  Current SSID: $current_ssid"
fi
read -p "  WiFi SSID: " ssid
if [ -z "$ssid" ]; then
    echo "  Empty SSID, using current value"
    ssid="$current_ssid"
fi

# --- WiFi Password ---
echo ""
echo "Step 2/3: WiFi Password"
read -s -p "  WiFi Password: " password
echo ""
if [ -z "$password" ]; then
    current_pass=$(grep -oP 'CONFIG_ESP_WIFI_PASSWORD="\K[^"]+' "$sdkconfig" 2>/dev/null || echo "")
    echo "  Empty password, using current value"
    password="$current_pass"
fi

# --- OTA Local SSID ---
echo ""
echo "Step 3/3: OTA Local SSID (optional)"
current_ota=$(grep -oP 'CONFIG_MAD_OTA_LOCAL_SSID="\K[^"]+' "$sdkconfig" 2>/dev/null || echo "")
echo "  Current: $current_ota"
read -p "  OTA local SSID (Enter to skip): " ota_ssid
if [ -z "$ota_ssid" ]; then
    ota_ssid="$current_ota"
    echo "  Kept current"
fi

# --- Применяем ---
echo ""
echo "Applying..."

# Используем sed для замены (macOS и Linux совместимость)
if [[ "$OSTYPE" == "darwin"* ]]; then
    sed -i '' \
        -e "s|CONFIG_ESP_WIFI_SSID=\"[^\"]*\"|CONFIG_ESP_WIFI_SSID=\"$ssid\"|" \
        -e "s|CONFIG_ESP_WIFI_PASSWORD=\"[^\"]*\"|CONFIG_ESP_WIFI_PASSWORD=\"$password\"|" \
        -e "s|CONFIG_MAD_OTA_LOCAL_SSID=\"[^\"]*\"|CONFIG_MAD_OTA_LOCAL_SSID=\"$ota_ssid\"|" \
        "$sdkconfig"
else
    sed -i \
        -e "s|CONFIG_ESP_WIFI_SSID=\"[^\"]*\"|CONFIG_ESP_WIFI_SSID=\"$ssid\"|" \
        -e "s|CONFIG_ESP_WIFI_PASSWORD=\"[^\"]*\"|CONFIG_ESP_WIFI_PASSWORD=\"$password\"|" \
        -e "s|CONFIG_MAD_OTA_LOCAL_SSID=\"[^\"]*\"|CONFIG_MAD_OTA_LOCAL_SSID=\"$ota_ssid\"|" \
        "$sdkconfig"
fi

echo ""
echo "=== Done ==="
echo "  SSID:  $ssid"
echo "  OTA:   $ota_ssid"
echo ""
echo "Run 'idf.py build flash monitor' to build and flash."
