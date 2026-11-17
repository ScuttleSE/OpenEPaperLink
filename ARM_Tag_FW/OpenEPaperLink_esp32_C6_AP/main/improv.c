// Improv Serial (https://www.improv-wifi.com/serial/) on the native USB port, for the Wi-Fi host link:
// the browser installer (ESP Web Tools) sets the Wi-Fi credentials right after flashing. The driver
// stores them in NVS (esp_wifi_set_config), so nothing is compiled in.
#include "sdkconfig.h"
#if defined(CONFIG_OEPL_HOST_LINK_WIFI) && defined(CONFIG_OEPL_IMPROV_SERIAL)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host_wifi.h"

#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) || defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
#error "Improv Serial needs USB Serial/JTAG free of the console: see sdkconfig.host-wifi"
#endif

static const char *TAG = "IMPROV";

#define IMPROV_VERSION 1

enum { TYPE_STATE = 1, TYPE_ERROR = 2, TYPE_RPC = 3, TYPE_RPC_RESULT = 4 };
enum { STATE_READY = 2, STATE_PROVISIONING = 3, STATE_PROVISIONED = 4 };
enum { ERR_NONE = 0, ERR_INVALID_RPC = 1, ERR_UNKNOWN_RPC = 2, ERR_UNABLE_TO_CONNECT = 3, ERR_UNKNOWN = 0xFF };
enum { RPC_WIFI_SETTINGS = 1, RPC_CURRENT_STATE = 2, RPC_DEVICE_INFO = 3, RPC_SCAN = 4 };

static void send_packet(uint8_t type, const uint8_t *data, uint8_t len) {
    uint8_t buf[6 + 3 + 255 + 2];
    memcpy(buf, "IMPROV", 6);
    buf[6] = IMPROV_VERSION;
    buf[7] = type;
    buf[8] = len;
    memcpy(buf + 9, data, len);
    uint8_t sum = 0;
    for (int i = 0; i < 9 + len; i++) sum += buf[i];
    buf[9 + len] = sum;
    buf[10 + len] = '\n';
    usb_serial_jtag_write_bytes(buf, 11 + len, pdMS_TO_TICKS(100));
}

static void send_state(uint8_t state) { send_packet(TYPE_STATE, &state, 1); }
static void send_error(uint8_t err) { send_packet(TYPE_ERROR, &err, 1); }

// An RPC result: the command, then strings, each with its length in front
static void send_result(uint8_t cmd, const char *const *strings, int count) {
    uint8_t data[255];
    int n = 2;
    for (int i = 0; i < count; i++) {
        const size_t l = strlen(strings[i]);
        if (n + 1 + l > sizeof(data)) break;
        data[n++] = (uint8_t) l;
        memcpy(data + n, strings[i], l);
        n += l;
    }
    data[0] = cmd;
    data[1] = (uint8_t) (n - 2);
    send_packet(TYPE_RPC_RESULT, data, (uint8_t) n);
}

static void url(char *buf, size_t len) {
    char ip[16];
    if (hostWifiIp(ip, sizeof(ip))) {
        snprintf(buf, len, "http://%s", ip);  // no web page on the hub yet: shows where it is
    } else {
        buf[0] = 0;
    }
}

static void handle_rpc(const uint8_t *d, uint8_t len) {
    if (len < 2 || d[1] + 2 > len) {
        send_error(ERR_INVALID_RPC);
        return;
    }
    const uint8_t cmd = d[0];
    const uint8_t *p = d + 2;
    const uint8_t plen = d[1];
    send_error(ERR_NONE);
    switch (cmd) {
        case RPC_WIFI_SETTINGS: {
            if (plen < 1 || p[0] + 2 > plen || p[0] > 32) {
                send_error(ERR_INVALID_RPC);
                return;
            }
            char ssid[33] = {0}, pass[65] = {0};
            memcpy(ssid, p + 1, p[0]);
            const uint8_t passLen = p[1 + p[0]];
            if (passLen > 64 || 2 + p[0] + passLen > plen) {
                send_error(ERR_INVALID_RPC);
                return;
            }
            memcpy(pass, p + 2 + p[0], passLen);
            send_state(STATE_PROVISIONING);
            ESP_LOGW(TAG, "connecting to \"%s\"", ssid);
            if (hostWifiConnect(ssid, pass, 20000)) {
                send_state(STATE_PROVISIONED);
                char u[40];
                url(u, sizeof(u));
                const char *s[] = {u};
                send_result(RPC_WIFI_SETTINGS, s, u[0] ? 1 : 0);
            } else {
                send_error(ERR_UNABLE_TO_CONNECT);
                send_state(STATE_READY);
            }
            memset(pass, 0, sizeof(pass));
            break;
        }
        case RPC_CURRENT_STATE: {
            const bool up = hostWifiIp(NULL, 0);
            send_state(up ? STATE_PROVISIONED : STATE_READY);
            if (up) {
                char u[40];
                url(u, sizeof(u));
                const char *s[] = {u};
                send_result(RPC_CURRENT_STATE, s, 1);
            }
            break;
        }
        case RPC_DEVICE_INFO: {
            char name[32];
            hostWifiHostname(name, sizeof(name));
            const char *s[] = {"OEPL hub", CONFIG_OEPL_HUB_FW_VERSION, "ESP32-C6", name};
            send_result(RPC_DEVICE_INFO, s, 4);
            break;
        }
        case RPC_SCAN: {
            uint16_t n = 0;
            wifi_scan_config_t sc = {0};
            if (esp_wifi_scan_start(&sc, true) == ESP_OK && esp_wifi_scan_get_ap_num(&n) == ESP_OK) {
                if (n > 20) n = 20;
                wifi_ap_record_t *aps = calloc(n ? n : 1, sizeof(wifi_ap_record_t));
                if (aps && esp_wifi_scan_get_ap_records(&n, aps) == ESP_OK) {
                    for (int i = 0; i < n; i++) {
                        if (!aps[i].ssid[0]) continue;
                        char rssi[8];
                        snprintf(rssi, sizeof(rssi), "%d", aps[i].rssi);
                        const char *s[] = {(const char *) aps[i].ssid, rssi,
                                           aps[i].authmode == WIFI_AUTH_OPEN ? "NO" : "YES"};
                        send_result(RPC_SCAN, s, 3);
                    }
                }
                free(aps);
            }
            send_result(RPC_SCAN, NULL, 0);  // end of the list
            break;
        }
        default:
            send_error(ERR_UNKNOWN_RPC);
    }
}

static void improv_task(void *arg) {
    static uint8_t buf[300];
    size_t len = 0;
    for (;;) {
        const int n = usb_serial_jtag_read_bytes(buf + len, sizeof(buf) - len, portMAX_DELAY);
        if (n <= 0) continue;
        len += n;
        for (;;) {
            // find the header
            size_t start = 0;
            while (start + 6 <= len && memcmp(buf + start, "IMPROV", 6) != 0) start++;
            if (start) {
                memmove(buf, buf + start, len - start);
                len -= start;
            }
            if (len < 9) break;
            const size_t total = 9 + buf[8] + 1;
            if (len < total) {
                if (total > sizeof(buf)) len = 0;  // can't be a valid packet
                break;
            }
            uint8_t sum = 0;
            for (size_t i = 0; i < total - 1; i++) sum += buf[i];
            if (sum == buf[total - 1] && buf[6] == IMPROV_VERSION && buf[7] == TYPE_RPC) {
                handle_rpc(buf + 9, buf[8]);
            } else if (sum != buf[total - 1]) {
                send_error(ERR_INVALID_RPC);
            }
            memmove(buf, buf + total, len - total);
            len -= total;
        }
        if (len == sizeof(buf)) len = 0;
    }
}

void improvStart(void) {
    usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = 512, .rx_buffer_size = 512};
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "no USB Serial/JTAG driver");
        return;
    }
    // ROM printf (ESP_EARLY_LOGx, panics) also writes to USB Serial/JTAG on channel 2; keep it on UART0 only
    esp_rom_install_channel_putc(2, NULL);
    xTaskCreate(improv_task, "improv", 4096, NULL, 5, NULL);
}

#endif
