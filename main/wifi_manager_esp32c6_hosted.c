#include "wifi_manager.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "WIFI_C6";

#define WIFI_SSID        CONFIG_ESP_WIFI_SSID
#define WIFI_PASS        CONFIG_ESP_WIFI_PASSWORD
#define WIFI_MAX_RETRY   CONFIG_ESP_MAXIMUM_RETRY
#define WIFI_WAIT_MS     CONFIG_DASHBOARD_WIFI_CONNECT_TIMEOUT_MS

typedef struct {
    const char *ssid;
    const char *password;
    const char *label;
} wifi_profile_t;

/*
 * Built-in field profiles.  These are intentionally tried before the single
 * menuconfig profile so the demo can move between home / office / service APs
 * without reflashing or editing sdkconfig each time.
 *
 * Security note: credentials compiled here are plain-text inside the firmware
 * image.  This is acceptable for the current lab/demo firmware, but production
 * builds should load profiles from NVS or a provisioning screen instead.
 */
static const wifi_profile_t s_wifi_profiles[] = {
    /* Priority order: production/work AP first, home AP second, legacy/fallbacks after that. */
    { "ARD",                "1qw23er4",  "work/service primary" },
    { "www.madmentat.ru",   "Rctybz82#", "home primary" },
    { "madmentat.ru",       "Rctybz82",  "legacy home/office" },
    { "https://madmentat.ru", "Rctybz82",  "URL-style SSID, password variant 1" },
    { "https://madmentat.ru", "Rctybz82#", "URL-style SSID, password variant 2" },
    { "https://madmentat.ru", "1qw23er4",  "URL-style SSID, password variant 3" },
    { WIFI_SSID,               WIFI_PASS,     "menuconfig fallback" },
};

#define WIFI_PROFILE_COUNT ((size_t)(sizeof(s_wifi_profiles) / sizeof(s_wifi_profiles[0])))

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

#if CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HUNT_AND_PECK
  #define WIFI_SAE_MODE       WPA3_SAE_PWE_HUNT_AND_PECK
  #define WIFI_H2E_IDENTIFIER ""
#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HASH_TO_ELEMENT
  #define WIFI_SAE_MODE       WPA3_SAE_PWE_HASH_TO_ELEMENT
  #define WIFI_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_BOTH
  #define WIFI_SAE_MODE       WPA3_SAE_PWE_BOTH
  #define WIFI_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#else
  #define WIFI_SAE_MODE       WPA3_SAE_PWE_BOTH
  #define WIFI_H2E_IDENTIFIER ""
#endif

#if CONFIG_ESP_WIFI_AUTH_OPEN
  #define WIFI_SCAN_AUTH_MODE WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
  #define WIFI_SCAN_AUTH_MODE WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
  #define WIFI_SCAN_AUTH_MODE WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
  #define WIFI_SCAN_AUTH_MODE WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
  #define WIFI_SCAN_AUTH_MODE WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
  #define WIFI_SCAN_AUTH_MODE WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
  #define WIFI_SCAN_AUTH_MODE WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
  #define WIFI_SCAN_AUTH_MODE WIFI_AUTH_WAPI_PSK
#else
  #define WIFI_SCAN_AUTH_MODE WIFI_AUTH_WPA2_PSK
#endif

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_sta_netif;
static int s_retry_num;
static bool s_wifi_driver_started;
static bool s_wifi_async_task_started;
static bool s_event_handlers_registered;
static bool s_connect_on_sta_start = true;
static TaskHandle_t s_wifi_task_handle;
static size_t s_current_profile_index;
static int s_last_disconnect_reason;
static bool s_scan_valid;
static char s_connected_ssid[33];
static bool s_profile_seen[WIFI_PROFILE_COUNT];
static uint8_t s_profile_channel[WIFI_PROFILE_COUNT];
static int8_t s_profile_rssi[WIFI_PROFILE_COUNT];
static wifi_auth_mode_t s_profile_auth[WIFI_PROFILE_COUNT];

volatile wifi_manager_state_t g_wifi_state = {
    .status = WIFI_MANAGER_STATUS_IDLE,
    .connected = false,
    .rssi = 0,
};

static void wifi_manager_set_state(wifi_manager_connection_status_t status,
                                   bool connected,
                                   int8_t rssi)
{
    g_wifi_state.status = status;
    g_wifi_state.connected = connected;
    g_wifi_state.rssi = rssi;
    if (!connected) {
        s_connected_ssid[0] = '\0';
    }
}

static void wifi_manager_set_connected_ssid(const char *ssid)
{
    if (!ssid) {
        s_connected_ssid[0] = '\0';
        return;
    }
    strlcpy(s_connected_ssid, ssid, sizeof(s_connected_ssid));
}

static void log_heap(const char *where)
{
    ESP_LOGI(TAG,
             "heap %s: internal=%lu dma=%lu psram=%lu",
             where,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static const char *authmode_to_str(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    default: return "UNKNOWN";
    }
}

static const char *disconnect_reason_to_str(int reason)
{
    /* Use numeric reason codes to avoid build breakage if ESP-IDF renames
     * individual WIFI_REASON_* enum symbols between hosted/remote versions.
     */
    switch (reason) {
    case 1: return "UNSPECIFIED";
    case 2: return "AUTH_EXPIRE";
    case 3: return "AUTH_LEAVE";
    case 4: return "ASSOC_EXPIRE";
    case 5: return "ASSOC_TOOMANY";
    case 6: return "NOT_AUTHED";
    case 7: return "NOT_ASSOCED";
    case 8: return "ASSOC_LEAVE";
    case 9: return "ASSOC_NOT_AUTHED";
    case 10: return "DISASSOC_PWRCAP_BAD";
    case 11: return "DISASSOC_SUPCHAN_BAD";
    case 12: return "BSS_TRANSITION_DISASSOC";
    case 13: return "IE_INVALID";
    case 14: return "MIC_FAILURE";
    case 15: return "4WAY_HANDSHAKE_TIMEOUT";
    case 16: return "GROUP_KEY_UPDATE_TIMEOUT";
    case 17: return "IE_IN_4WAY_DIFFERS";
    case 18: return "GROUP_CIPHER_INVALID";
    case 19: return "PAIRWISE_CIPHER_INVALID";
    case 20: return "AKMP_INVALID";
    case 21: return "UNSUPP_RSN_IE_VERSION";
    case 22: return "INVALID_RSN_IE_CAP";
    case 23: return "802_1X_AUTH_FAILED";
    case 24: return "CIPHER_SUITE_REJECTED";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default: return "UNKNOWN";
    }
}


static bool profile_matches_ssid(const wifi_profile_t *profile, const uint8_t ssid[33])
{
    if (!profile || !profile->ssid || !profile->ssid[0]) {
        return false;
    }
    return strcmp(profile->ssid, (const char *)ssid) == 0;
}

static void mark_profiles_seen(const wifi_ap_record_t *records, uint16_t record_count)
{
    memset(s_profile_seen, 0, sizeof(s_profile_seen));
    memset(s_profile_channel, 0, sizeof(s_profile_channel));
    memset(s_profile_rssi, 0, sizeof(s_profile_rssi));
    memset(s_profile_auth, 0, sizeof(s_profile_auth));

    for (uint16_t i = 0; i < record_count; ++i) {
        for (size_t p = 0; p < WIFI_PROFILE_COUNT; ++p) {
            if (profile_matches_ssid(&s_wifi_profiles[p], records[i].ssid)) {
                s_profile_seen[p] = true;
                s_profile_channel[p] = records[i].primary;
                s_profile_rssi[p] = records[i].rssi;
                s_profile_auth[p] = records[i].authmode;
            }
        }
    }
}

static bool any_profile_seen(void)
{
    for (size_t i = 0; i < WIFI_PROFILE_COUNT; ++i) {
        if (s_profile_seen[i]) {
            return true;
        }
    }
    return false;
}

static void log_profile_list(void)
{
    ESP_LOGI(TAG, "Built-in WiFi profiles: %u candidate(s)", (unsigned)WIFI_PROFILE_COUNT);
    for (size_t i = 0; i < WIFI_PROFILE_COUNT; ++i) {
        ESP_LOGI(TAG,
                 "  profile[%u]: ssid=\"%s\" label=\"%s\"%s",
                 (unsigned)i + 1U,
                 s_wifi_profiles[i].ssid,
                 s_wifi_profiles[i].label,
                 i == WIFI_PROFILE_COUNT - 1U ? " (from menuconfig)" : "");
    }
}

static void log_confirmed_hosted_path(void)
{
    ESP_LOGI(TAG, "Confirmed JC4880P443C SDIO path from OTA-host experiment:");
    ESP_LOGI(TAG, "  C6 reset GPIO54, SDIO slot 1, 4-bit, known-good 10000 kHz safe mode");
    ESP_LOGI(TAG, "  CLK GPIO18, CMD GPIO19, D0 GPIO14, D1 GPIO15, D2 GPIO16, D3 GPIO17");
    ESP_LOGI(TAG, "  C6 network_adapter / ESP-Hosted slave firmware observed: 2.6.7");

#ifdef CONFIG_ESP_EXT_CONN_SLAVE_ENABLE_PIN
    ESP_LOGI(TAG, "sdkconfig extconn: reset=%d level=%d slot=%d CLK=%d CMD=%d D0=%d D1=%d D2=%d D3=%d",
             CONFIG_ESP_EXT_CONN_SLAVE_ENABLE_PIN,
#ifdef CONFIG_ESP_EXT_CONN_SLAVE_ENABLE_LVL
             CONFIG_ESP_EXT_CONN_SLAVE_ENABLE_LVL,
#else
             -1,
#endif
#ifdef CONFIG_ESP_EXT_CONN_SDIO_SLOT
             CONFIG_ESP_EXT_CONN_SDIO_SLOT,
#else
             -1,
#endif
#ifdef CONFIG_ESP_EXT_CONN_SDIO_CLK_PIN
             CONFIG_ESP_EXT_CONN_SDIO_CLK_PIN,
#else
             -1,
#endif
#ifdef CONFIG_ESP_EXT_CONN_SDIO_CMD_PIN
             CONFIG_ESP_EXT_CONN_SDIO_CMD_PIN,
#else
             -1,
#endif
#ifdef CONFIG_ESP_EXT_CONN_SDIO_D0_PIN
             CONFIG_ESP_EXT_CONN_SDIO_D0_PIN,
#else
             -1,
#endif
#ifdef CONFIG_ESP_EXT_CONN_SDIO_D1_PIN
             CONFIG_ESP_EXT_CONN_SDIO_D1_PIN,
#else
             -1,
#endif
#ifdef CONFIG_ESP_EXT_CONN_SDIO_D2_PIN
             CONFIG_ESP_EXT_CONN_SDIO_D2_PIN,
#else
             -1,
#endif
#ifdef CONFIG_ESP_EXT_CONN_SDIO_D3_PIN
             CONFIG_ESP_EXT_CONN_SDIO_D3_PIN
#else
             -1
#endif
             );
#else
    ESP_LOGW(TAG, "CONFIG_ESP_EXT_CONN_* pin macros not visible here; check menuconfig ESP-Hosted SDIO settings");
#endif
}

static void log_wifi_country(const char *where)
{
    wifi_country_t country = {0};
    esp_err_t ret = esp_wifi_get_country(&country);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "WiFi country %s: cc=\"%c%c\" schan=%u nchan=%u policy=%u",
                 where,
                 country.cc[0] ? country.cc[0] : '?',
                 country.cc[1] ? country.cc[1] : '?',
                 (unsigned)country.schan,
                 (unsigned)country.nchan,
                 (unsigned)country.policy);
    } else {
        ESP_LOGW(TAG, "esp_wifi_get_country(%s) failed: %s", where, esp_err_to_name(ret));
    }
}

static void configure_wifi_country_for_field_tests(void)
{
    /*
     * Android/Samsung hotspots in Europe/Russia may choose channels 12/13 in
     * 2.4 GHz.  If the hosted WiFi stack defaults to a US-like domain (1..11),
     * esp_wifi_connect() can return reason=201 (NO_AP_FOUND) even though a
     * laptop/phone sees and uses the AP.  Use the lab field-test domain 1..13
     * unless a future production provisioning screen overrides it.
     */
    wifi_country_t country = {
        .schan = 1,
        .nchan = CONFIG_DASHBOARD_WIFI_COUNTRY_NCHAN,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    strlcpy(country.cc, CONFIG_DASHBOARD_WIFI_COUNTRY_CODE, sizeof(country.cc));

    esp_err_t ret = esp_wifi_set_country(&country);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "Configured WiFi country/domain for scan/connect: cc=\"%s\" channels=1..%u",
                 CONFIG_DASHBOARD_WIFI_COUNTRY_CODE,
                 (unsigned)CONFIG_DASHBOARD_WIFI_COUNTRY_NCHAN);
    } else {
        ESP_LOGW(TAG, "esp_wifi_set_country failed: %s", esp_err_to_name(ret));
    }

    log_wifi_country("after set_country");
}


static void log_scan_results(void)
{
    s_scan_valid = false;
    memset(s_profile_seen, 0, sizeof(s_profile_seen));
    memset(s_profile_channel, 0, sizeof(s_profile_channel));
    memset(s_profile_rssi, 0, sizeof(s_profile_rssi));
    memset(s_profile_auth, 0, sizeof(s_profile_auth));
#if CONFIG_DASHBOARD_WIFI_SCAN_BEFORE_CONNECT
    ESP_LOGI(TAG, "Running diagnostic WiFi scan before connecting");

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {
            .min = 120,
            .max = 360,
        },
    };
    ESP_LOGI(TAG,
             "Diagnostic scan config: all channels, show_hidden=1, active min/max=120/360 ms");
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_start failed: %s", esp_err_to_name(ret));
        return;
    }

    uint16_t ap_count = 0;
    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_scan_get_ap_num failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "WiFi scan found %u AP(s)", (unsigned)ap_count);
    s_scan_valid = true;
    if (ap_count == 0) {
        return;
    }

    uint16_t record_count = ap_count;
    if (record_count > 32) {
        record_count = 32;
    }

    wifi_ap_record_t *records = calloc(record_count, sizeof(*records));
    if (!records) {
        ESP_LOGW(TAG, "No heap for scan records (%u entries)", (unsigned)record_count);
        return;
    }

    ret = esp_wifi_scan_get_ap_records(&record_count, records);
    if (ret == ESP_OK) {
        for (uint16_t i = 0; i < record_count; ++i) {
            ESP_LOGI(TAG,
                     "AP[%u]: ssid=\"%s\" rssi=%d auth=%s channel=%u",
                     (unsigned)i,
                     (const char *)records[i].ssid,
                     records[i].rssi,
                     authmode_to_str(records[i].authmode),
                     (unsigned)records[i].primary);
        }
        mark_profiles_seen(records, record_count);
        for (size_t p = 0; p < WIFI_PROFILE_COUNT; ++p) {
            if (s_profile_seen[p]) {
                ESP_LOGI(TAG,
                         "Visible known WiFi profile[%u]: ssid=\"%s\" label=\"%s\"",
                         (unsigned)p + 1U,
                         s_wifi_profiles[p].ssid,
                         s_wifi_profiles[p].label);
            }
        }
        if (ap_count > record_count) {
            ESP_LOGI(TAG, "... %u more AP(s) not printed/scanned for profiles", (unsigned)(ap_count - record_count));
        }
    } else {
        ESP_LOGW(TAG, "esp_wifi_scan_get_ap_records failed: %s", esp_err_to_name(ret));
    }

    free(records);
#else
    (void)authmode_to_str;
#endif
}

static esp_err_t init_nvs_once(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition problem, erasing and reinitialising");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t create_default_event_loop_once(void)
{
    esp_err_t ret = esp_event_loop_create_default();
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    return ret;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        wifi_manager_set_state(WIFI_MANAGER_STATUS_CONNECTING, false, 0);
        if (!s_connect_on_sta_start) {
            ESP_LOGI(TAG, "STA started; first connect deferred until diagnostic scan finishes");
            return;
        }

        const esp_err_t ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect on STA_START failed: %s", esp_err_to_name(ret));
            wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)event_data;
        s_last_disconnect_reason = disc ? disc->reason : -1;
        const wifi_profile_t *profile = &s_wifi_profiles[s_current_profile_index];
        ESP_LOGW(TAG,
                 "Disconnected from \"%s\", reason=%d (%s)",
                 profile->ssid,
                 s_last_disconnect_reason,
                 disconnect_reason_to_str(s_last_disconnect_reason));

        wifi_manager_set_state(WIFI_MANAGER_STATUS_DISCONNECTED, false, 0);

        if (s_last_disconnect_reason == 201) {
            ESP_LOGW(TAG,
                     "NO_AP_FOUND for \"%s\": do not spend %d retries on this profile; moving to next candidate",
                     profile->ssid,
                     WIFI_MAX_RETRY);
            wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        } else if (s_retry_num < WIFI_MAX_RETRY) {
            s_retry_num++;
            wifi_manager_set_state(WIFI_MANAGER_STATUS_CONNECTING, false, 0);
            ESP_LOGI(TAG, "Retrying current profile (%d/%d)", s_retry_num, WIFI_MAX_RETRY);
            const esp_err_t ret = esp_wifi_connect();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_connect retry failed: %s", esp_err_to_name(ret));
            }
        } else {
            wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
            if (s_wifi_event_group) {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
            ESP_LOGW(TAG,
                     "Connection profile failed after %d retries: ssid=\"%s\", last_reason=%d (%s)",
                     WIFI_MAX_RETRY,
                     s_wifi_profiles[s_current_profile_index].ssid,
                     s_last_disconnect_reason,
                     disconnect_reason_to_str(s_last_disconnect_reason));
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        if (event) {
            ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        } else {
            ESP_LOGI(TAG, "Got IP");
        }

        s_retry_num = 0;

        int8_t rssi = 0;
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
            wifi_manager_set_connected_ssid((const char *)ap_info.ssid);
            ESP_LOGI(TAG, "Connected AP: ssid=\"%s\" RSSI: %d dBm", s_connected_ssid, rssi);
        } else {
            wifi_manager_set_connected_ssid(s_wifi_profiles[s_current_profile_index].ssid);
            ESP_LOGI(TAG, "Connected AP: ssid=\"%s\" RSSI unavailable", s_connected_ssid);
        }

        wifi_manager_set_state(WIFI_MANAGER_STATUS_CONNECTED, true, rssi);
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

static esp_err_t register_event_handlers_once(void)
{
    if (s_event_handlers_registered) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT,
                            ESP_EVENT_ANY_ID,
                            &wifi_event_handler,
                            NULL,
                            NULL),
                        TAG,
                        "register WIFI handler failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT,
                            IP_EVENT_STA_GOT_IP,
                            &wifi_event_handler,
                            NULL,
                            NULL),
                        TAG,
                        "register IP handler failed");

    s_event_handlers_registered = true;
    return ESP_OK;
}

static esp_err_t create_sta_netif_once(void)
{
    if (s_sta_netif) {
        return ESP_OK;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta returned NULL");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t apply_wifi_profile(const wifi_profile_t *profile)
{
    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_SCAN_AUTH_MODE,
            .sae_pwe_h2e = WIFI_SAE_MODE,
            .sae_h2e_identifier = WIFI_H2E_IDENTIFIER,
        },
    };

    strlcpy((char *)wifi_config.sta.ssid, profile->ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, profile->password, sizeof(wifi_config.sta.password));

    const size_t profile_index = (size_t)(profile - s_wifi_profiles);
    if (profile_index < WIFI_PROFILE_COUNT && s_profile_seen[profile_index] && s_profile_channel[profile_index] != 0) {
        wifi_config.sta.channel = s_profile_channel[profile_index];
        ESP_LOGI(TAG,
                 "Applying STA config for visible AP: ssid=\"%s\" channel=%u rssi=%d auth=%s",
                 profile->ssid,
                 (unsigned)s_profile_channel[profile_index],
                 s_profile_rssi[profile_index],
                 authmode_to_str(s_profile_auth[profile_index]));
    } else {
        ESP_LOGI(TAG,
                 "Applying STA config for ssid=\"%s\" without known channel (will scan/probe)",
                 profile->ssid);
    }

    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

static esp_err_t wait_for_profile_result(const wifi_profile_t *profile)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIFI_WAIT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to \"%s\" RSSI: %d dBm", profile->ssid, g_wifi_state.rssi);
        return ESP_OK;
    }

    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG,
                 "Failed to connect to \"%s\": last_reason=%d (%s)",
                 profile->ssid,
                 s_last_disconnect_reason,
                 disconnect_reason_to_str(s_last_disconnect_reason));
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "WiFi connection timeout for \"%s\" after %d ms", profile->ssid, WIFI_WAIT_MS);
    return ESP_ERR_TIMEOUT;
}

static esp_err_t try_wifi_profile(size_t profile_index)
{
    const wifi_profile_t *profile = &s_wifi_profiles[profile_index];

    s_current_profile_index = profile_index;
    s_retry_num = 0;
    s_last_disconnect_reason = 0;

    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    wifi_manager_set_state(WIFI_MANAGER_STATUS_CONNECTING, false, 0);

    ESP_LOGI(TAG,
             "Trying WiFi profile %u/%u: ssid=\"%s\", label=\"%s\"",
             (unsigned)profile_index + 1U,
             (unsigned)WIFI_PROFILE_COUNT,
             profile->ssid,
             profile->label);

    ESP_RETURN_ON_ERROR(apply_wifi_profile(profile), TAG, "set WiFi config failed");

    esp_err_t ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect failed for \"%s\": %s", profile->ssid, esp_err_to_name(ret));
        return ret;
    }

    return wait_for_profile_result(profile);
}

static esp_err_t connect_using_profiles(void)
{
    /*
     * Always try profiles in configured priority order.  The scan results are
     * used only as a channel/RSSI hint in apply_wifi_profile(); they no longer
     * reorder the list.  This keeps ARD as the first candidate whenever it is
     * configured, with www.madmentat.ru as the second/home fallback.
     */
    if (s_scan_valid && any_profile_seen()) {
        ESP_LOGI(TAG, "Trying WiFi profiles in configured priority order; scan data is used only as a channel hint");
    } else if (s_scan_valid) {
        ESP_LOGW(TAG, "No built-in WiFi profile was visible in scan; trying configured priority order anyway");
    } else {
        ESP_LOGW(TAG, "WiFi scan unavailable; trying configured priority order");
    }

    for (size_t i = 0; i < WIFI_PROFILE_COUNT; ++i) {
        esp_err_t ret = try_wifi_profile(i);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "WiFi profile connected; profile scanning is stopped until link loss: ssid=\"%s\"",
                     s_wifi_profiles[i].ssid);
            return ESP_OK;
        }
    }

    wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
    return ESP_FAIL;
}

esp_err_t wifi_manager_start(void)
{
    if (s_wifi_driver_started) {
        ESP_LOGI(TAG, "WiFi already started, status=%d", (int)g_wifi_state.status);
        return g_wifi_state.connected ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    wifi_manager_set_state(WIFI_MANAGER_STATUS_CONNECTING, false, 0);
    log_heap("before NVS/netif/remote WiFi init");

    ESP_LOGI(TAG, "Starting ESP32-C6 hosted WiFi backend");
    ESP_LOGI(TAG, "Profile retries=%d, per-profile timeout=%d ms", WIFI_MAX_RETRY, WIFI_WAIT_MS);
    log_profile_list();
    log_confirmed_hosted_path();

    ESP_RETURN_ON_ERROR(init_nvs_once(), TAG, "NVS init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(create_default_event_loop_once(), TAG, "event loop init failed");
    ESP_RETURN_ON_ERROR(create_sta_netif_once(), TAG, "default STA netif failed");

    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
        if (!s_wifi_event_group) {
            wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
            return ESP_ERR_NO_MEM;
        }
    }

    log_heap("before esp_wifi_init");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
        log_heap("after esp_wifi_init failure");
        return ret;
    }

    log_heap("after esp_wifi_init");

    ESP_RETURN_ON_ERROR(register_event_handlers_once(), TAG, "event handler registration failed");

    ESP_LOGI(TAG, "Setting WiFi mode STA");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set WiFi mode failed");
    configure_wifi_country_for_field_tests();

#if CONFIG_DASHBOARD_WIFI_DISABLE_POWER_SAVE
    /*
     * Do not call esp_wifi_set_ps() on the ESP32-P4 hosted/remote WiFi path.
     * On this target the normal local-WiFi power-management hook is not present;
     * calling esp_wifi_set_ps(WIFI_PS_NONE) after esp_wifi_init() was observed to
     * jump through a NULL/unsupported PM callback and crash at pm_set_sleep_type
     * (MEPC=0x00000000, RA=pm_set_sleep_type).  The C6 network_adapter owns the
     * radio, so power-save policy must be handled by the hosted/slave firmware or
     * a supported ESP-Hosted API later.
     */
    ESP_LOGW(TAG, "Skipping esp_wifi_set_ps(WIFI_PS_NONE) on ESP32-C6 hosted backend");
#else
    ESP_LOGI(TAG, "WiFi power-save override disabled by sdkconfig");
#endif

#if CONFIG_DASHBOARD_WIFI_SCAN_BEFORE_CONNECT
    s_connect_on_sta_start = false;
#else
    s_connect_on_sta_start = true;
#endif

    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    ESP_LOGI(TAG, "Starting WiFi driver / hosted STA");
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
        log_heap("after esp_wifi_start failure");
        return ret;
    }

    s_wifi_driver_started = true;

#if CONFIG_DASHBOARD_WIFI_SCAN_BEFORE_CONNECT
    log_scan_results();
#endif

    ret = connect_using_profiles();
    if (ret != ESP_OK) {
        log_heap("after all WiFi profiles failed");
    }

    return ret;
}

static void wifi_manager_task(void *arg)
{
    (void)arg;

#if CONFIG_DASHBOARD_WIFI_START_DELAY_MS > 0
    ESP_LOGI(TAG,
             "Delaying ESP32-C6 hosted WiFi init by %d ms so UI/display can settle",
             CONFIG_DASHBOARD_WIFI_START_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(CONFIG_DASHBOARD_WIFI_START_DELAY_MS));
#endif

    esp_err_t ret = wifi_manager_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Async ESP32-C6 WiFi start finished with %s", esp_err_to_name(ret));
        if (g_wifi_state.status != WIFI_MANAGER_STATUS_CONNECTING &&
            g_wifi_state.status != WIFI_MANAGER_STATUS_CONNECTED) {
            wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
        }
    }

    /*
     * Keep the manager alive.  While connected we do not scan or try other
     * profiles at all; profile checks resume only after link loss / failure.
     */
    for (;;) {
        if (g_wifi_state.connected) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        if (g_wifi_state.status == WIFI_MANAGER_STATUS_CONNECTING) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGW(TAG, "WiFi link is down; resuming profile checks in priority order");
#if CONFIG_DASHBOARD_WIFI_SCAN_BEFORE_CONNECT
        log_scan_results();
#endif
        ret = connect_using_profiles();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "WiFi reconnect pass failed: %s; will retry later", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

esp_err_t wifi_manager_start_async(void)
{
    if (s_wifi_async_task_started || s_wifi_driver_started || s_wifi_task_handle) {
        ESP_LOGI(TAG, "WiFi async start ignored: already started or running");
        return ESP_OK;
    }

    wifi_manager_set_state(WIFI_MANAGER_STATUS_CONNECTING, false, 0);

    BaseType_t ret = xTaskCreate(wifi_manager_task,
                                 "wifi_c6_mgr",
                                 CONFIG_DASHBOARD_WIFI_TASK_STACK_SIZE,
                                 NULL,
                                 CONFIG_DASHBOARD_WIFI_TASK_PRIORITY,
                                 &s_wifi_task_handle);
    if (ret != pdPASS) {
        s_wifi_async_task_started = false;
        s_wifi_task_handle = NULL;
        wifi_manager_set_state(WIFI_MANAGER_STATUS_FAILED, false, 0);
        return ESP_ERR_NO_MEM;
    }

    s_wifi_async_task_started = true;
    ESP_LOGI(TAG, "ESP32-C6 hosted WiFi manager task started");
    return ESP_OK;
}

wifi_manager_connection_status_t wifi_manager_get_status(void)
{
    return g_wifi_state.status;
}

int8_t wifi_manager_get_rssi(void)
{
    if (!g_wifi_state.connected) {
        return 0;
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        g_wifi_state.rssi = ap.rssi;
        wifi_manager_set_connected_ssid((const char *)ap.ssid);
    }
    return g_wifi_state.rssi;
}

const char *wifi_manager_get_connected_ssid(void)
{
    if (!g_wifi_state.connected || !s_connected_ssid[0]) {
        return "";
    }
    return s_connected_ssid;
}

const char *wifi_manager_get_backend_name(void)
{
    return "ESP32-C6 ESP-Hosted";
}
