#!/usr/bin/env bash
# deploy.sh — копирует проект в git_deploy без паролей и мусора
# Запуск: bash deploy.sh

set -euo pipefail

src="$(cd "$(dirname "$0")" && pwd)"
dst="$src/git_deploy"

echo "=== Deploy: $src -> $dst ==="

# Удаляем предыдущую копию
if [ -d "$dst" ]; then
    rm -rf "$dst"
    echo "Old git_deploy removed"
fi

# Копируем всё (кроме уже существующего git_deploy)
mkdir -p "$dst"
for item in "$src"/*; do
    name=$(basename "$item")
    [ "$name" = "git_deploy" ] && continue
    cp -a "$item" "$dst/"
done
echo "Copying project..."

# Удаляем мусор
for dir in .git build build_work_verify build_transition_lab \
           build_transition_verify_user managed_components tmp tools trash docs; do
    if [ -d "$dst/$dir" ]; then
        rm -rf "$dst/$dir"
        echo "  Removed: $dir"
    fi
done

# Удаляем sdkconfig.bak и sdkconfig.old
find "$dst" -maxdepth 1 -name "sdkconfig.bak*" -delete 2>/dev/null || true
find "$dst" -maxdepth 1 -name "sdkconfig.old" -delete 2>/dev/null || true

# Удаляем .bak файлы в main/
find "$dst/main" -name "*.bak_*" -delete 2>/dev/null || true
find "$dst/main" -name "*.bak-*" -delete 2>/dev/null || true

# Удаляем personal notes
rm -f "$dst/ptompt.txt" 2>/dev/null || true

# Затираем пароли в sdkconfig
if [ -f "$dst/sdkconfig" ]; then
    sed -i \
        -e 's/CONFIG_ESP_WIFI_SSID="[^"]*"/CONFIG_ESP_WIFI_SSID="YOUR_WIFI_SSID"/' \
        -e 's/CONFIG_ESP_WIFI_PASSWORD="[^"]*"/CONFIG_ESP_WIFI_PASSWORD="YOUR_WIFI_PASSWORD"/' \
        -e 's/CONFIG_MAD_OTA_LOCAL_SSID="[^"]*"/CONFIG_MAD_OTA_LOCAL_SSID="YOUR_OTA_LOCAL_SSID"/' \
        -e 's/CONFIG_MQTT_BROKER_URI="[^"]*"/CONFIG_MQTT_BROKER_URI="mqtt:\/\/YOUR_MQTT_BROKER_IP"/' \
        "$dst/sdkconfig"
    echo "  sdkconfig: credentials replaced with placeholders"
fi

# Готово
file_count=$(find "$dst" -type f | wc -l)
size_mb=$(du -sm "$dst" | cut -f1)
echo ""
echo "=== Done: git_deploy ($file_count files, ${size_mb} MB) ==="
