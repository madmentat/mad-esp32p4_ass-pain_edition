# deploy.ps1 — копирует проект в git_deploy без паролей и мусора
# Запуск: powershell -ExecutionPolicy Bypass -File deploy.ps1

$ErrorActionPreference = "Stop"
$src = $PSScriptRoot
$dst = Join-Path $src "git_deploy"

Write-Host "=== Deploy: $src -> $dst ===" -ForegroundColor Cyan

# Удаляем предыдущую копию
if (Test-Path $dst) {
    Remove-Item $dst -Recurse -Force
    Write-Host "Old git_deploy removed" -ForegroundColor Yellow
}

# Копируем всё
Write-Host "Copying project..."
Copy-Item $src $dst -Recurse

# Удаляем мусор
$removeDirs = @(
    ".git",
    "build",
    "build_work_verify",
    "build_transition_lab",
    "build_transition_verify_user",
    "managed_components",
    "tmp",
    "tools",
    "trash",
    "docs"  # справочные PDF/архивы, не нужны для сборки
)

foreach ($dir in $removeDirs) {
    $path = Join-Path $dst $dir
    if (Test-Path $path) {
        Remove-Item $path -Recurse -Force
        Write-Host "  Removed: $dir" -ForegroundColor DarkGray
    }
}

# Удаляем sdkconfig.bak и sdkconfig.old
Get-ChildItem $dst -Filter "sdkconfig.bak*" -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem $dst -Filter "sdkconfig.old" -ErrorAction SilentlyContinue | Remove-Item -Force

# Удаляем .bak файлы в main/
Get-ChildItem (Join-Path $dst "main") -Filter "*.bak_*" -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem (Join-Path $dst "main") -Filter "*.bak-*" -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force

# Удаляем .bak файлы в main/update/
Get-ChildItem (Join-Path $dst "main\update") -Filter "*.bak_*" -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem (Join-Path $dst "main\update") -Filter "*.bak-*" -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force

# Удаляем personal notes
Remove-Item (Join-Path $dst "ptompt.txt") -ErrorAction SilentlyContinue

# Затираем пароли в sdkconfig
$sdkconfig = Join-Path $dst "sdkconfig"
if (Test-Path $sdkconfig) {
    $content = Get-Content $sdkconfig -Raw

    $content = $content -replace 'CONFIG_ESP_WIFI_SSID="[^"]*"', 'CONFIG_ESP_WIFI_SSID="YOUR_WIFI_SSID"'
    $content = $content -replace 'CONFIG_ESP_WIFI_PASSWORD="[^"]*"', 'CONFIG_ESP_WIFI_PASSWORD="YOUR_WIFI_PASSWORD"'
    $content = $content -replace 'CONFIG_MAD_OTA_LOCAL_SSID="[^"]*"', 'CONFIG_MAD_OTA_LOCAL_SSID="YOUR_OTA_LOCAL_SSID"'
    $content = $content -replace 'CONFIG_MQTT_BROKER_URI="[^"]*"', 'CONFIG_MQTT_BROKER_URI="mqtt://YOUR_MQTT_BROKER_IP"'

    Set-Content $sdkconfig $content -NoNewline
    Write-Host "  sdkconfig: credentials replaced with placeholders" -ForegroundColor Green
}

# Готово
$fileCount = (Get-ChildItem $dst -Recurse -File).Count
$sizeMB = [math]::Round((Get-ChildItem $dst -Recurse -File | Measure-Object -Property Length -Sum).Sum / 1MB, 1)
Write-Host "`n=== Done: git_deploy ($fileCount files, $sizeMB MB) ===" -ForegroundColor Green
