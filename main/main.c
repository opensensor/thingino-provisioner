#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#if __has_include("provision_config.h")
#include "provision_config.h"
#else
#include "provision_config.example.h"
#endif

#define I2C_PORT I2C_NUM_0
#define OLED_SDA_GPIO GPIO_NUM_5
#define OLED_SCL_GPIO GPIO_NUM_6
#define OLED_I2C_FREQ_HZ 400000
#define OLED_ADDR_PRIMARY 0x3c
#define OLED_ADDR_SECONDARY 0x3d
#define OLED_WIDTH 72
#define OLED_HEIGHT 40
#define OLED_PAGES (OLED_HEIGHT / 8)
#define OLED_X_OFFSET 28

#define MAX_AP_RECORDS 24
#define SCAN_INTERVAL_MS 6000
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define THINGINO_SSID_PREFIX "THINGINO-"
#define THINGINO_PORTAL_API_SAVE_URL "http://172.16.0.1/x/api.cgi?action=save"
#define THINGINO_PORTAL_LEGACY_URL "http://172.16.0.1/x/index.cgi"
#define THINGINO_CONNECT_TIMEOUT_MS 16000
#define THINGINO_HTTP_TIMEOUT_MS 8000
#define THINGINO_RETRY_COOLDOWN_MS 30000
#define THINGINO_SUCCESS_COOLDOWN_MS 600000

static const char *TAG = "wifi_scanner";

static uint8_t s_oled_addr = OLED_ADDR_PRIMARY;
static bool s_oled_ready;
static uint8_t s_framebuffer[OLED_WIDTH * OLED_PAGES];
static EventGroupHandle_t s_wifi_events;
static bool s_portal_connect_active;
static bool s_last_provision_valid;
static bool s_last_provision_success;
static int64_t s_last_provision_attempt_us;
static uint8_t s_last_provision_bssid[6];
static wifi_ap_record_t s_scan_records[MAX_AP_RECORDS];

typedef struct {
    char c;
    uint8_t rows[5];
} glyph_t;

static const glyph_t GLYPHS[] = {
    {'0', {0x7, 0x5, 0x5, 0x5, 0x7}},
    {'1', {0x2, 0x6, 0x2, 0x2, 0x7}},
    {'2', {0x7, 0x1, 0x7, 0x4, 0x7}},
    {'3', {0x7, 0x1, 0x7, 0x1, 0x7}},
    {'4', {0x5, 0x5, 0x7, 0x1, 0x1}},
    {'5', {0x7, 0x4, 0x7, 0x1, 0x7}},
    {'6', {0x7, 0x4, 0x7, 0x5, 0x7}},
    {'7', {0x7, 0x1, 0x1, 0x2, 0x2}},
    {'8', {0x7, 0x5, 0x7, 0x5, 0x7}},
    {'9', {0x7, 0x5, 0x7, 0x1, 0x7}},
    {'A', {0x2, 0x5, 0x7, 0x5, 0x5}},
    {'B', {0x6, 0x5, 0x6, 0x5, 0x6}},
    {'C', {0x7, 0x4, 0x4, 0x4, 0x7}},
    {'D', {0x6, 0x5, 0x5, 0x5, 0x6}},
    {'E', {0x7, 0x4, 0x6, 0x4, 0x7}},
    {'F', {0x7, 0x4, 0x6, 0x4, 0x4}},
    {'G', {0x7, 0x4, 0x5, 0x5, 0x7}},
    {'H', {0x5, 0x5, 0x7, 0x5, 0x5}},
    {'I', {0x7, 0x2, 0x2, 0x2, 0x7}},
    {'J', {0x1, 0x1, 0x1, 0x5, 0x7}},
    {'K', {0x5, 0x5, 0x6, 0x5, 0x5}},
    {'L', {0x4, 0x4, 0x4, 0x4, 0x7}},
    {'M', {0x5, 0x7, 0x7, 0x5, 0x5}},
    {'N', {0x5, 0x7, 0x7, 0x7, 0x5}},
    {'O', {0x7, 0x5, 0x5, 0x5, 0x7}},
    {'P', {0x7, 0x5, 0x7, 0x4, 0x4}},
    {'Q', {0x7, 0x5, 0x5, 0x7, 0x1}},
    {'R', {0x6, 0x5, 0x6, 0x5, 0x5}},
    {'S', {0x7, 0x4, 0x7, 0x1, 0x7}},
    {'T', {0x7, 0x2, 0x2, 0x2, 0x2}},
    {'U', {0x5, 0x5, 0x5, 0x5, 0x7}},
    {'V', {0x5, 0x5, 0x5, 0x5, 0x2}},
    {'W', {0x5, 0x5, 0x7, 0x7, 0x5}},
    {'X', {0x5, 0x5, 0x2, 0x5, 0x5}},
    {'Y', {0x5, 0x5, 0x2, 0x2, 0x2}},
    {'Z', {0x7, 0x1, 0x2, 0x4, 0x7}},
    {' ', {0x0, 0x0, 0x0, 0x0, 0x0}},
    {'-', {0x0, 0x0, 0x7, 0x0, 0x0}},
    {'_', {0x0, 0x0, 0x0, 0x0, 0x7}},
    {'.', {0x0, 0x0, 0x0, 0x0, 0x2}},
    {':', {0x0, 0x2, 0x0, 0x2, 0x0}},
    {'/', {0x1, 0x1, 0x2, 0x4, 0x4}},
    {'?', {0x7, 0x1, 0x3, 0x0, 0x2}},
};

static esp_err_t oled_write_control(uint8_t control, const uint8_t *data, size_t len)
{
    uint8_t buffer[OLED_WIDTH + 1];
    if (len > OLED_WIDTH) {
        return ESP_ERR_INVALID_SIZE;
    }

    buffer[0] = control;
    if (len > 0) {
        memcpy(&buffer[1], data, len);
    }

    return i2c_master_write_to_device(I2C_PORT, s_oled_addr, buffer, len + 1, pdMS_TO_TICKS(100));
}

static esp_err_t oled_command(uint8_t command)
{
    return oled_write_control(0x00, &command, 1);
}

static esp_err_t oled_command_pair(uint8_t command, uint8_t value)
{
    uint8_t payload[] = {command, value};
    return oled_write_control(0x00, payload, sizeof(payload));
}

static void oled_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }

    uint8_t *cell = &s_framebuffer[(y / 8) * OLED_WIDTH + x];
    uint8_t mask = BIT(y % 8);
    if (on) {
        *cell |= mask;
    } else {
        *cell &= (uint8_t)~mask;
    }
}

static const uint8_t *glyph_rows_for(char c)
{
    c = (char)toupper((unsigned char)c);
    for (size_t i = 0; i < sizeof(GLYPHS) / sizeof(GLYPHS[0]); ++i) {
        if (GLYPHS[i].c == c) {
            return GLYPHS[i].rows;
        }
    }

    return GLYPHS[sizeof(GLYPHS) / sizeof(GLYPHS[0]) - 1].rows;
}

static void oled_draw_char(int x, int y, char c)
{
    const uint8_t *rows = glyph_rows_for(c);

    for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 3; ++col) {
            bool on = (rows[row] & BIT(2 - col)) != 0;
            oled_set_pixel(x + col, y + row, on);
        }
    }
}

static void oled_draw_text(int x, int y, const char *text)
{
    while (*text && x < OLED_WIDTH) {
        oled_draw_char(x, y, *text++);
        x += 4;
    }
}

static void oled_clear(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
}

static esp_err_t oled_flush(void)
{
    if (!s_oled_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    for (uint8_t page = 0; page < OLED_PAGES; ++page) {
        ESP_RETURN_ON_ERROR(oled_command(0xb0 | page), TAG, "set page failed");
        ESP_RETURN_ON_ERROR(oled_command(0x00 | (OLED_X_OFFSET & 0x0f)), TAG, "set low column failed");
        ESP_RETURN_ON_ERROR(oled_command(0x10 | (OLED_X_OFFSET >> 4)), TAG, "set high column failed");
        ESP_RETURN_ON_ERROR(oled_write_control(0x40, &s_framebuffer[page * OLED_WIDTH], OLED_WIDTH), TAG, "write page failed");
    }

    return ESP_OK;
}

static void oled_show_lines(const char *line0, const char *line1, const char *line2,
                            const char *line3, const char *line4, const char *line5)
{
    if (!s_oled_ready) {
        return;
    }

    oled_clear();
    if (line0) {
        oled_draw_text(0, 0, line0);
    }
    if (line1) {
        oled_draw_text(0, 6, line1);
    }
    if (line2) {
        oled_draw_text(0, 12, line2);
    }
    if (line3) {
        oled_draw_text(0, 18, line3);
    }
    if (line4) {
        oled_draw_text(0, 24, line4);
    }
    if (line5) {
        oled_draw_text(0, 30, line5);
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(oled_flush());
}

static esp_err_t oled_try_init_at(uint8_t address)
{
    s_oled_addr = address;

    ESP_RETURN_ON_ERROR(oled_command(0xae), TAG, "display off failed");
    ESP_RETURN_ON_ERROR(oled_command_pair(0xd5, 0x80), TAG, "clock failed");
    ESP_RETURN_ON_ERROR(oled_command_pair(0xa8, 0x27), TAG, "mux failed");
    ESP_RETURN_ON_ERROR(oled_command_pair(0xd3, 0x00), TAG, "offset failed");
    ESP_RETURN_ON_ERROR(oled_command(0x40), TAG, "start line failed");
    ESP_RETURN_ON_ERROR(oled_command_pair(0x8d, 0x14), TAG, "charge pump failed");
    ESP_RETURN_ON_ERROR(oled_command_pair(0x20, 0x02), TAG, "page addressing failed");
    ESP_RETURN_ON_ERROR(oled_command(0xa1), TAG, "segment remap failed");
    ESP_RETURN_ON_ERROR(oled_command(0xc8), TAG, "scan direction failed");
    ESP_RETURN_ON_ERROR(oled_command_pair(0xda, 0x12), TAG, "com pins failed");
    ESP_RETURN_ON_ERROR(oled_command_pair(0x81, 0xaf), TAG, "contrast failed");
    ESP_RETURN_ON_ERROR(oled_command_pair(0xd9, 0xf1), TAG, "precharge failed");
    ESP_RETURN_ON_ERROR(oled_command_pair(0xdb, 0x40), TAG, "vcomh failed");
    ESP_RETURN_ON_ERROR(oled_command(0xa4), TAG, "resume display failed");
    ESP_RETURN_ON_ERROR(oled_command(0xa6), TAG, "normal display failed");
    ESP_RETURN_ON_ERROR(oled_command(0xaf), TAG, "display on failed");

    s_oled_ready = true;
    oled_clear();
    return oled_flush();
}

static void oled_init(void)
{
    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = OLED_I2C_FREQ_HZ,
        .clk_flags = 0,
    };

    ESP_ERROR_CHECK(i2c_param_config(I2C_PORT, &config));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    esp_err_t err = oled_try_init_at(OLED_ADDR_PRIMARY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OLED not found at 0x%02x: %s", OLED_ADDR_PRIMARY, esp_err_to_name(err));
        err = oled_try_init_at(OLED_ADDR_SECONDARY);
    }

    if (err != ESP_OK) {
        s_oled_ready = false;
        ESP_LOGW(TAG, "OLED init failed; continuing with serial output only");
        return;
    }

    ESP_LOGI(TAG, "OLED initialized at I2C address 0x%02x", s_oled_addr);
}

static void sanitize_ssid(char *dst, size_t dst_len, const uint8_t *ssid)
{
    size_t out = 0;

    if (dst_len == 0) {
        return;
    }

    for (size_t i = 0; ssid[i] != 0 && out + 1 < dst_len; ++i) {
        unsigned char ch = ssid[i];
        if (ch >= 32 && ch <= 126) {
            dst[out++] = (char)toupper(ch);
        } else {
            dst[out++] = '?';
        }
    }

    if (out == 0) {
        strlcpy(dst, "HIDDEN", dst_len);
    } else {
        dst[out] = '\0';
    }
}

static const char *auth_mode_name(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "OPEN";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK:
        return "WAPI";
    default:
        return "UNKNOWN";
    }
}

static void copy_ssid(char *dst, size_t dst_len, const uint8_t *ssid)
{
    size_t out = 0;

    if (dst_len == 0) {
        return;
    }

    for (size_t i = 0; i < 32 && ssid[i] != 0 && out + 1 < dst_len; ++i) {
        unsigned char ch = ssid[i];
        dst[out++] = (ch >= 32 && ch <= 126) ? (char)ch : '?';
    }
    dst[out] = '\0';
}

static bool is_thingino_portal_ap(const wifi_ap_record_t *record)
{
    char ssid[33];

    copy_ssid(ssid, sizeof(ssid), record->ssid);
    return record->authmode == WIFI_AUTH_OPEN &&
           strncasecmp(ssid, THINGINO_SSID_PREFIX, strlen(THINGINO_SSID_PREFIX)) == 0;
}

static const wifi_ap_record_t *find_thingino_portal(const wifi_ap_record_t *records, uint16_t count)
{
    for (uint16_t i = 0; i < count; ++i) {
        if (is_thingino_portal_ap(&records[i])) {
            return &records[i];
        }
    }

    return NULL;
}

static bool same_bssid(const uint8_t left[6], const uint8_t right[6])
{
    return memcmp(left, right, 6) == 0;
}

static bool should_attempt_thingino_provision(const wifi_ap_record_t *record)
{
    if (!s_last_provision_valid || !same_bssid(s_last_provision_bssid, record->bssid)) {
        return true;
    }

    int64_t elapsed_ms = (esp_timer_get_time() - s_last_provision_attempt_us) / 1000;
    int64_t cooldown_ms = s_last_provision_success ? THINGINO_SUCCESS_COOLDOWN_MS : THINGINO_RETRY_COOLDOWN_MS;

    return elapsed_ms >= cooldown_ms;
}

static void mark_thingino_provision_attempt(const wifi_ap_record_t *record, bool success)
{
    memcpy(s_last_provision_bssid, record->bssid, sizeof(s_last_provision_bssid));
    s_last_provision_valid = true;
    s_last_provision_success = success;
    s_last_provision_attempt_us = esp_timer_get_time();
}

static void make_hostname_from_portal_ssid(char *hostname, size_t hostname_len, const char *ssid)
{
    size_t out;

    strlcpy(hostname, THINGINO_HOSTNAME_PREFIX, hostname_len);
    out = strlen(hostname);

    if (out == 0) {
        strlcpy(hostname, "thingino", hostname_len);
        out = strlen(hostname);
    }

    if (out + 1 < hostname_len) {
        hostname[out++] = '-';
        hostname[out] = '\0';
    }

    const char *suffix = ssid;
    if (strncasecmp(ssid, THINGINO_SSID_PREFIX, strlen(THINGINO_SSID_PREFIX)) == 0) {
        suffix += strlen(THINGINO_SSID_PREFIX);
    }

    for (size_t i = 0; suffix[i] != '\0' && out + 1 < hostname_len; ++i) {
        unsigned char ch = (unsigned char)suffix[i];
        if (isalnum(ch) || ch == '-') {
            hostname[out++] = (char)tolower(ch);
        }
    }

    if (hostname[out - 1] == '-') {
        strlcpy(&hostname[out], "camera", hostname_len - out);
    } else {
        hostname[out] = '\0';
    }
}

static bool append_byte(char *buffer, size_t buffer_len, size_t *used, char value)
{
    if (*used + 1 >= buffer_len) {
        return false;
    }

    buffer[(*used)++] = value;
    buffer[*used] = '\0';
    return true;
}

static bool append_urlencoded(char *buffer, size_t buffer_len, size_t *used, const char *value)
{
    static const char hex[] = "0123456789ABCDEF";

    while (*value) {
        unsigned char ch = (unsigned char)*value++;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            if (!append_byte(buffer, buffer_len, used, (char)ch)) {
                return false;
            }
        } else if (ch == ' ') {
            if (!append_byte(buffer, buffer_len, used, '+')) {
                return false;
            }
        } else {
            if (!append_byte(buffer, buffer_len, used, '%') ||
                !append_byte(buffer, buffer_len, used, hex[ch >> 4]) ||
                !append_byte(buffer, buffer_len, used, hex[ch & 0x0f])) {
                return false;
            }
        }
    }

    return true;
}

static bool append_form_field(char *buffer, size_t buffer_len, size_t *used,
                              const char *key, const char *value)
{
    if (*used > 0 && !append_byte(buffer, buffer_len, used, '&')) {
        return false;
    }

    return append_urlencoded(buffer, buffer_len, used, key) &&
           append_byte(buffer, buffer_len, used, '=') &&
           append_urlencoded(buffer, buffer_len, used, value);
}

typedef struct {
    char *data;
    size_t capacity;
    size_t used;
} http_response_buffer_t;

static esp_err_t portal_http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 || event->user_data == NULL) {
        return ESP_OK;
    }

    http_response_buffer_t *response = (http_response_buffer_t *)event->user_data;
    size_t available = response->capacity - response->used - 1;
    size_t copy_len = event->data_len < available ? event->data_len : available;

    if (copy_len > 0) {
        memcpy(&response->data[response->used], event->data, copy_len);
        response->used += copy_len;
        response->data[response->used] = '\0';
    }

    return ESP_OK;
}

static bool http_status_is_redirect(int status)
{
    return status >= 300 && status < 400;
}

static bool parse_hidden_value_last(const char *html, const char *name, char *value, size_t value_len)
{
    char name_pattern[48];
    const char *search = html;
    bool found = false;

    if (value_len == 0) {
        return false;
    }
    value[0] = '\0';

    snprintf(name_pattern, sizeof(name_pattern), "name=\"%s\"", name);

    while ((search = strstr(search, name_pattern)) != NULL) {
        const char *tag_end = strchr(search, '>');
        const char *value_start;
        const char *value_end;
        size_t copy_len;

        if (tag_end == NULL) {
            break;
        }

        value_start = strstr(search, "value=\"");
        if (value_start == NULL || value_start > tag_end) {
            search = tag_end + 1;
            continue;
        }

        value_start += strlen("value=\"");
        value_end = strchr(value_start, '"');
        if (value_end == NULL || value_end > tag_end) {
            search = tag_end + 1;
            continue;
        }

        copy_len = (size_t)(value_end - value_start);
        if (copy_len >= value_len) {
            copy_len = value_len - 1;
        }
        memcpy(value, value_start, copy_len);
        value[copy_len] = '\0';
        found = true;
        search = tag_end + 1;
    }

    return found;
}

static bool build_thingino_modern_save_body(char *body, size_t body_len, const char *portal_ssid)
{
    char hostname[64];
    size_t used = 0;

    body[0] = '\0';
    make_hostname_from_portal_ssid(hostname, sizeof(hostname), portal_ssid);

    return append_form_field(body, body_len, &used, "hostname", hostname) &&
           append_form_field(body, body_len, &used, "rootpass", THINGINO_ROOT_PASSWORD) &&
           append_form_field(body, body_len, &used, "rootpkey", THINGINO_ROOT_PUBLIC_KEY) &&
           append_form_field(body, body_len, &used, "timezone", THINGINO_TIMEZONE) &&
           append_form_field(body, body_len, &used, "wlan_ssid", THINGINO_WIFI_SSID) &&
           append_form_field(body, body_len, &used, "wlan_pass", THINGINO_WIFI_PASSWORD) &&
           append_form_field(body, body_len, &used, "wlan_ap", "false");
}

static bool build_thingino_legacy_review_body(char *body, size_t body_len,
                                              const char *portal_ssid,
                                              const char *timestamp)
{
    char hostname[64];
    size_t used = 0;

    body[0] = '\0';
    make_hostname_from_portal_ssid(hostname, sizeof(hostname), portal_ssid);

    return append_form_field(body, body_len, &used, "hostname", hostname) &&
           append_form_field(body, body_len, &used, "rootpass", THINGINO_ROOT_PASSWORD) &&
           append_form_field(body, body_len, &used, "rootpkey", THINGINO_ROOT_PUBLIC_KEY) &&
           append_form_field(body, body_len, &used, "wlanssid", THINGINO_WIFI_SSID) &&
           append_form_field(body, body_len, &used, "wlanpass", THINGINO_WIFI_PASSWORD) &&
           append_form_field(body, body_len, &used, "wlanap_enabled", "false") &&
           append_form_field(body, body_len, &used, "wlanap_ssid", "") &&
           append_form_field(body, body_len, &used, "wlanap_pass", "") &&
           append_form_field(body, body_len, &used, "timezone", THINGINO_TIMEZONE) &&
           append_form_field(body, body_len, &used, "timestamp", timestamp) &&
           append_form_field(body, body_len, &used, "mode", "review");
}

static bool build_thingino_legacy_save_body(char *body, size_t body_len,
                                            const char *portal_ssid,
                                            const char *timestamp)
{
    char hostname[64];
    size_t used = 0;

    body[0] = '\0';
    make_hostname_from_portal_ssid(hostname, sizeof(hostname), portal_ssid);

    return append_form_field(body, body_len, &used, "mode", "save") &&
           append_form_field(body, body_len, &used, "hostname", hostname) &&
           append_form_field(body, body_len, &used, "rootpass", THINGINO_ROOT_PASSWORD) &&
           append_form_field(body, body_len, &used, "rootpkey", THINGINO_ROOT_PUBLIC_KEY) &&
           append_form_field(body, body_len, &used, "timestamp", timestamp) &&
           append_form_field(body, body_len, &used, "timezone", THINGINO_TIMEZONE) &&
           append_form_field(body, body_len, &used, "wlanap_enabled", "false") &&
           append_form_field(body, body_len, &used, "wlanap_pass", "") &&
           append_form_field(body, body_len, &used, "wlanap_ssid", "") &&
           append_form_field(body, body_len, &used, "wlanpass", THINGINO_WIFI_PASSWORD) &&
           append_form_field(body, body_len, &used, "wlanssid", THINGINO_WIFI_SSID);
}

static bool portal_get(const char *url, char *response_data, size_t response_len, int *status_code)
{
    http_response_buffer_t response = {
        .data = response_data,
        .capacity = response_len,
        .used = 0,
    };

    memset(response_data, 0, response_len);
    *status_code = 0;

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = THINGINO_HTTP_TIMEOUT_MS,
        .event_handler = portal_http_event_handler,
        .user_data = &response,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_err_t err = esp_http_client_perform(client);
    *status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Thingino portal GET failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

static bool portal_post_form(const char *url, const char *body,
                             bool disable_auto_redirect,
                             char *response_data, size_t response_len, int *status_code)
{
    http_response_buffer_t response = {
        .data = response_data,
        .capacity = response_len,
        .used = 0,
    };

    memset(response_data, 0, response_len);
    *status_code = 0;

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = THINGINO_HTTP_TIMEOUT_MS,
        .event_handler = portal_http_event_handler,
        .user_data = &response,
        .disable_auto_redirect = disable_auto_redirect,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to create HTTP client");
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    *status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Thingino portal POST failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

static bool portal_save_modern(const char *portal_ssid)
{
    static char body[768];
    static char response_data[256];
    int status = 0;

    if (!build_thingino_modern_save_body(body, sizeof(body), portal_ssid)) {
        ESP_LOGE(TAG, "Modern provisioning form body is too large");
        return false;
    }

    if (!portal_post_form(THINGINO_PORTAL_API_SAVE_URL, body, false,
                          response_data, sizeof(response_data), &status)) {
        return false;
    }

    bool success = status == 200 &&
                   strstr(response_data, "\"success\"") != NULL &&
                   strstr(response_data, "true") != NULL;

    ESP_LOGI(TAG, "Thingino modern save status=%d success=%s", status, success ? "true" : "false");
    if (!success && response_data[0] != '\0') {
        ESP_LOGW(TAG, "Thingino modern response: %.120s", response_data);
    }

    return success;
}

static bool portal_save_legacy(const char *portal_ssid)
{
    static char body[768];
    static char response_data[4096];
    char initial_timestamp[24] = "0";
    char save_timestamp[24] = "0";
    int status = 0;

    if (!portal_get(THINGINO_PORTAL_LEGACY_URL, response_data, sizeof(response_data), &status) ||
        status != 200) {
        ESP_LOGW(TAG, "Thingino legacy form fetch status=%d", status);
        return false;
    }

    if (!parse_hidden_value_last(response_data, "timestamp",
                                 initial_timestamp, sizeof(initial_timestamp))) {
        ESP_LOGW(TAG, "Thingino legacy form did not include a timestamp; using fallback");
    }

    if (!build_thingino_legacy_review_body(body, sizeof(body), portal_ssid, initial_timestamp)) {
        ESP_LOGE(TAG, "Legacy review form body is too large");
        return false;
    }

    if (!portal_post_form(THINGINO_PORTAL_LEGACY_URL, body, false,
                          response_data, sizeof(response_data), &status)) {
        return false;
    }

    bool review_ok = status == 200 &&
                     strstr(response_data, "Ready to connect") != NULL &&
                     strstr(response_data, "name=\"mode\" value=\"save\"") != NULL &&
                     parse_hidden_value_last(response_data, "timestamp",
                                             save_timestamp, sizeof(save_timestamp));

    ESP_LOGI(TAG, "Thingino legacy review status=%d success=%s",
             status, review_ok ? "true" : "false");
    if (!review_ok) {
        ESP_LOGW(TAG, "Thingino legacy review did not return the expected confirm form");
        return false;
    }

    if (!build_thingino_legacy_save_body(body, sizeof(body), portal_ssid, save_timestamp)) {
        ESP_LOGE(TAG, "Legacy save form body is too large");
        return false;
    }

    if (!portal_post_form(THINGINO_PORTAL_LEGACY_URL, body, true,
                          response_data, sizeof(response_data), &status)) {
        return false;
    }

    bool success = http_status_is_redirect(status) ||
                   (status == 200 &&
                    (strstr(response_data, "Configuration Completed") != NULL ||
                     strstr(response_data, "reboot") != NULL));

    ESP_LOGI(TAG, "Thingino legacy save status=%d success=%s", status, success ? "true" : "false");
    if (!success) {
        ESP_LOGW(TAG, "Thingino legacy save did not return a completion or redirect response");
    }

    return success;
}

static bool portal_save_configuration(const char *portal_ssid)
{
    if (portal_save_modern(portal_ssid)) {
        return true;
    }

    ESP_LOGI(TAG, "Falling back to legacy Thingino portal form");
    bool success = portal_save_legacy(portal_ssid);
    if (!success) {
        ESP_LOGE(TAG, "Thingino portal save did not succeed");
    }
    return success;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED && s_portal_connect_active) {
        xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP && s_portal_connect_active) {
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static bool connect_to_thingino_portal(const wifi_ap_record_t *record, const char *portal_ssid)
{
    static wifi_config_t wifi_config;

    memset(&wifi_config, 0, sizeof(wifi_config));
    strlcpy((char *)wifi_config.sta.ssid, portal_ssid, sizeof(wifi_config.sta.ssid));
    wifi_config.sta.channel = record->primary;
    wifi_config.sta.bssid_set = true;
    memcpy(wifi_config.sta.bssid, record->bssid, sizeof(wifi_config.sta.bssid));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    s_portal_connect_active = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    vTaskDelay(pdMS_TO_TICKS(200));
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_portal_connect_active = true;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "Connecting to Thingino portal SSID \"%s\"", portal_ssid);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_portal_connect_active = false;
        ESP_LOGE(TAG, "Thingino portal connect start failed: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(THINGINO_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to Thingino portal");
        return true;
    }

    s_portal_connect_active = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    ESP_LOGW(TAG, "Thingino portal connection timed out or failed");
    return false;
}

static void disconnect_from_thingino_portal(void)
{
    s_portal_connect_active = false;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static bool provision_thingino_portal(const wifi_ap_record_t *record)
{
    char portal_ssid[33];
    bool success = false;

    copy_ssid(portal_ssid, sizeof(portal_ssid), record->ssid);

    if (!should_attempt_thingino_provision(record)) {
        ESP_LOGI(TAG, "Skipping recent Thingino provisioning attempt for \"%s\"", portal_ssid);
        return false;
    }

    mark_thingino_provision_attempt(record, false);
    oled_show_lines("THINGINO FOUND", portal_ssid, "CONNECTING", NULL, NULL, NULL);

    if (!connect_to_thingino_portal(record, portal_ssid)) {
        oled_show_lines("THINGINO", "CONNECT FAILED", portal_ssid, NULL, NULL, NULL);
        return false;
    }

    oled_show_lines("THINGINO", "SENDING CONFIG", portal_ssid, NULL, NULL, NULL);
    success = portal_save_configuration(portal_ssid);

    if (success) {
        mark_thingino_provision_attempt(record, true);
        oled_show_lines("THINGINO", "PROVISION OK", "CAM REBOOTING", NULL, NULL, NULL);
        ESP_LOGI(TAG, "Thingino portal accepted provisioning config");
        vTaskDelay(pdMS_TO_TICKS(5000));
    } else {
        oled_show_lines("THINGINO", "PROVISION FAIL", portal_ssid, NULL, NULL, NULL);
        ESP_LOGW(TAG, "Thingino portal provisioning did not complete");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    disconnect_from_thingino_portal();
    return success;
}

static int compare_rssi_desc(const void *a, const void *b)
{
    const wifi_ap_record_t *left = (const wifi_ap_record_t *)a;
    const wifi_ap_record_t *right = (const wifi_ap_record_t *)b;

    return (int)right->rssi - (int)left->rssi;
}

static void display_scan_results(const wifi_ap_record_t *records, uint16_t shown, uint16_t total)
{
    char lines[6][24] = {0};

    snprintf(lines[0], sizeof(lines[0]), "WIFI SCAN %u", total);
    for (uint16_t i = 0; i < shown && i < 5; ++i) {
        char ssid[12];
        sanitize_ssid(ssid, sizeof(ssid), records[i].ssid);
        snprintf(lines[i + 1], sizeof(lines[i + 1]), "%u%4d %.11s", i + 1, records[i].rssi, ssid);
    }

    oled_show_lines(lines[0], lines[1], lines[2], lines[3], lines[4], lines[5]);
}

static void wifi_init_scanner(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(s_wifi_events == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void scan_once(void)
{
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 300,
    };

    ESP_LOGI(TAG, "Starting Wi-Fi scan");
    oled_show_lines("WIFI SCANNER", "SCANNING", NULL, NULL, NULL, NULL);

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        oled_show_lines("WIFI SCANNER", "SCAN FAILED", esp_err_to_name(err), NULL, NULL, NULL);
        return;
    }

    uint16_t total = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_num(&total));

    uint16_t shown = MAX_AP_RECORDS;
    memset(s_scan_records, 0, sizeof(s_scan_records));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_records(&shown, s_scan_records));
    qsort(s_scan_records, shown, sizeof(s_scan_records[0]), compare_rssi_desc);

    ESP_LOGI(TAG, "Found %u access points; showing strongest %u", total, shown);
    for (uint16_t i = 0; i < shown; ++i) {
        char ssid[34];
        sanitize_ssid(ssid, sizeof(ssid), s_scan_records[i].ssid);
        ESP_LOGI(TAG, "%2u: ch=%2u rssi=%4d auth=%-9s ssid=\"%s\"",
                 i + 1, s_scan_records[i].primary, s_scan_records[i].rssi,
                 auth_mode_name(s_scan_records[i].authmode), ssid);
    }

    display_scan_results(s_scan_records, shown, total);

#if THINGINO_PROVISION_ENABLED
    const wifi_ap_record_t *portal = find_thingino_portal(s_scan_records, shown);
    if (portal != NULL) {
        provision_thingino_portal(portal);
    }
#endif
}

void app_main(void)
{
    oled_init();
    oled_show_lines("ESP32-C3", "WIFI SCANNER", "BOOTING", NULL, NULL, NULL);

    wifi_init_scanner();

    while (true) {
        scan_once();
        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}
