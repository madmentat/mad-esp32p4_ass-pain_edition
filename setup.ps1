# setup.ps1 — настраивает sdkconfig: WiFi, OTA сервер
# Запуск: powershell -ExecutionPolicy Bypass -File setup.ps1
#
# После запуска:
#   1. Введите WiFi SSID
#   2. Введите WiFi пароль
#   3. Введите OTA local SSID (или нажмите Enter чтобы пропустить)

$ErrorActionPreference = "Stop"
$sdkconfig = Join-Path $PSScriptRoot "sdkconfig"

if (-not (Test-Path $sdkconfig)) {
    Write-Host "ERROR: sdkconfig not found" -ForegroundColor Red
    exit 1
}

Write-Host "=== WiFi & OTA Setup ===" -ForegroundColor Cyan
Write-Host ""

# Читаем текущий sdkconfig
$content = Get-Content $sdkconfig -Raw

# Проверяем, есть ли что настраивать
$hasPlaceholder = $content -match "YOUR_WIFI_SSID|YOUR_WIFI_PASSWORD|YOUR_OTA_LOCAL_SSID"
if (-not $hasPlaceholder) {
    Write-Host "sdkconfig already has real credentials (no placeholders found)." -ForegroundColor Yellow
    $overwrite = Read-Host "Overwrite? (y/N)"
    if ($overwrite -ne "y") { Write-Host "Skipped."; exit 0 }
}

Write-Host ""
Write-Host "Step 1/3: WiFi" -ForegroundColor Green

# --- WiFi SSID ---
$current_ssid = ""
if ($content -match 'CONFIG_ESP_WIFI_SSID="([^"]*)"') {
    $current_ssid = $Matches[1]
}
if ($current_ssid -and $current_ssid -ne "YOUR_WIFI_SSID") {
    Write-Host "  Current SSID: $current_ssid"
}
$ssid = Read-Host "  WiFi SSID"
if (-not $ssid) {
    Write-Host "  Empty SSID, using current value" -ForegroundColor Yellow
    $ssid = $current_ssid
}

# --- WiFi Password ---
$current_pass = ""
if ($content -match 'CONFIG_ESP_WIFI_PASSWORD="([^"]*)"') {
    $current_pass = $Matches[1]
}
Write-Host ""
Write-Host "Step 2/3: WiFi Password" -ForegroundColor Green
$password = Read-Host "  WiFi Password" -AsSecureString
$plainPassword = [System.Runtime.InteropServices.Marshal]::PtrToStringAuto(
    [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($password)
)
if (-not $plainPassword) {
    Write-Host "  Empty password, using current value" -ForegroundColor Yellow
    $plainPassword = $current_pass
}

# --- OTA Local SSID ---
Write-Host ""
Write-Host "Step 3/3: OTA Local SSID (optional)" -ForegroundColor Green
$current_ota_ssid = ""
if ($content -match 'CONFIG_MAD_OTA_LOCAL_SSID="([^"]*)"') {
    $current_ota_ssid = $Matches[1]
}
Write-Host "  Current: $current_ota_ssid"
$ota_ssid = Read-Host "  OTA local SSID (Enter to skip)"
if (-not $ota_ssid) {
    $ota_ssid = $current_ota_ssid
    Write-Host "  Kept current" -ForegroundColor Yellow
}

# --- Применяем ---
Write-Host ""
Write-Host "Applying..." -ForegroundColor Cyan

$content = $content -replace 'CONFIG_ESP_WIFI_SSID="[^"]*"', "CONFIG_ESP_WIFI_SSID=`"$ssid`""
$content = $content -replace 'CONFIG_ESP_WIFI_PASSWORD="[^"]*"', "CONFIG_ESP_WIFI_PASSWORD=`"$plainPassword`""
$content = $content -replace 'CONFIG_MAD_OTA_LOCAL_SSID="[^"]*"', "CONFIG_MAD_OTA_LOCAL_SSID=`"$ota_ssid`""

Set-Content $sdkconfig $content -NoNewline

Write-Host ""
Write-Host "=== Done ===" -ForegroundColor Green
Write-Host "  SSID:  $ssid"
Write-Host "  OTA:   $ota_ssid"
Write-Host ""
Write-Host "Run 'idf.py build flash monitor' to build and flash." -ForegroundColor Cyan
