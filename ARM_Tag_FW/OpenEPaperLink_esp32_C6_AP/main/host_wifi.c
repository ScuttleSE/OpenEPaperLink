// The host link over Wi-Fi: a TCP server for one client, with the same functions as the UART link in
// second_uart.c. The byte stream is the same as on the UART (ASCII startup, then v2 frames); there is
// no baud rate, so the switch to 2 Mbaud after FRM1 does nothing. A new client replaces the old one.
// When the client goes away the radio keeps its v2 state; after V2_LINK_LOST_MS without frames it
// falls back to the ASCII startup, as it does on the UART.
#include "sdkconfig.h"
#ifdef CONFIG_OEPL_HOST_LINK_WIFI

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "radio.h"
#include "host_wifi.h"
#include "second_uart.h"

static const char *TAG = "HOST_WIFI";

#define RX_BUF_SIZE 4096
#define TX_TIMEOUT_MS 50

static StreamBufferHandle_t rxStream;
static SemaphoreHandle_t clientMutex;
static int clientSock = -1;
volatile uint32_t uartOverflows = 0;

#define WIFI_GOT_IP BIT0
static EventGroupHandle_t wifiEvents;
static esp_netif_t *staNetif;

// --- Wi-Fi ------------------------------------------------------------------------------------------

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        wifi_config_t wc;
        if (esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0]) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(wifiEvents, WIFI_GOT_IP);
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d), reconnecting", d->reason);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifiEvents, WIFI_GOT_IP);
        const ip_event_got_ip_t *e = data;
        wifi_ap_record_t ap;
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        ESP_LOGW(TAG, "Wi-Fi up: " IPSTR ", RSSI %d, Wi-Fi channel %d", IP2STR(&e->ip_info.ip), rssi, ap.primary);
    }
}

void hostWifiHostname(char *buf, size_t len) {
    if (strlen(CONFIG_OEPL_WIFI_HOSTNAME)) {
        snprintf(buf, len, "%s", CONFIG_OEPL_WIFI_HOSTNAME);
        return;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "oepl-hub-%02x%02x", mac[4], mac[5]);
}

static void wifi_start(void) {
    wifiEvents = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    staNetif = netif;
    char name[32];
    hostWifiHostname(name, sizeof(name));
    esp_netif_set_hostname(netif, name);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Credentials from the build if given, otherwise what the Wi-Fi driver stored in NVS before
    wifi_config_t wc = {0};
    if (strlen(CONFIG_OEPL_WIFI_SSID)) {
        strlcpy((char *) wc.sta.ssid, CONFIG_OEPL_WIFI_SSID, sizeof(wc.sta.ssid));
        strlcpy((char *) wc.sta.password, CONFIG_OEPL_WIFI_PASSWORD, sizeof(wc.sta.password));
        wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    } else if (esp_wifi_get_config(WIFI_IF_STA, &wc) != ESP_OK || wc.sta.ssid[0] == 0) {
        ESP_LOGE(TAG, "no Wi-Fi credentials: set them with the installer (Improv Serial)");
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    // no modem sleep: the host's ACKs must not wait for a DTIM beacon
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(name);
        mdns_instance_name_set("OpenEPaperLink hub");
        mdns_service_add(NULL, "_oepl-hub", "_tcp", CONFIG_OEPL_HOST_TCP_PORT, NULL, 0);
    }
    ESP_LOGW(TAG, "hub %s, TCP port %d", name, CONFIG_OEPL_HOST_TCP_PORT);
#ifdef CONFIG_OEPL_IMPROV_SERIAL
    improvStart();
#endif
}

bool hostWifiConnect(const char *ssid, const char *password, uint32_t timeoutMs) {
    wifi_config_t wc = {0};
    strlcpy((char *) wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *) wc.sta.password, password, sizeof(wc.sta.password));
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_disconnect();
    xEventGroupClearBits(wifiEvents, WIFI_GOT_IP);
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return false;
    esp_wifi_connect();
    return xEventGroupWaitBits(wifiEvents, WIFI_GOT_IP, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeoutMs)) & WIFI_GOT_IP;
}

bool hostWifiIp(char *buf, size_t len) {
    if (!(xEventGroupGetBits(wifiEvents) & WIFI_GOT_IP)) return false;
    if (buf) {
        esp_netif_ip_info_t ip;
        if (esp_netif_get_ip_info(staNetif, &ip) != ESP_OK) return false;
        snprintf(buf, len, IPSTR, IP2STR(&ip.ip));
    }
    return true;
}

// --- TCP server ---------------------------------------------------------------------------------

static void setClient(int sock) {
    xSemaphoreTake(clientMutex, portMAX_DELAY);
    if (clientSock >= 0) {
        shutdown(clientSock, SHUT_RDWR);
        close(clientSock);
    }
    clientSock = sock;
    xSemaphoreGive(clientMutex);
}

static void client_rx_task(void *arg) {
    const int sock = (int) arg;
    uint8_t buf[512];
    for (;;) {
        const int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        if (xStreamBufferSend(rxStream, buf, n, 0) != (size_t) n) uartOverflows++;
        if (radioWakeTask) xTaskNotifyGive(radioWakeTask);
    }
    xSemaphoreTake(clientMutex, portMAX_DELAY);
    if (clientSock == sock) {
        close(sock);
        clientSock = -1;
        ESP_LOGW(TAG, "host disconnected");
    }
    xSemaphoreGive(clientMutex);
    vTaskDelete(NULL);
}

static void server_task(void *arg) {
    const int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    const int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(CONFIG_OEPL_HOST_TCP_PORT),
                               .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(srv, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(srv, 1) != 0) {
        ESP_LOGE(TAG, "can't listen on port %d", CONFIG_OEPL_HOST_TCP_PORT);
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        const int sock = accept(srv, (struct sockaddr *) &peer, &plen);
        if (sock < 0) continue;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        const int idle = 5, intvl = 2, cnt = 3;
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
        const struct timeval tv = {.tv_sec = 0, .tv_usec = TX_TIMEOUT_MS * 1000};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        char ip[16];
        inet_ntoa_r(peer.sin_addr, ip, sizeof(ip));
        ESP_LOGW(TAG, "host connected from %s", ip);
        setClient(sock);
        xStreamBufferReset(rxStream);
        xTaskCreate(client_rx_task, "host_rx", 3072, (void *) sock, 12, NULL);
    }
}

// --- the host link API (second_uart.h) ----------------------------------------------------------

void init_second_uart() {
    rxStream = xStreamBufferCreate(RX_BUF_SIZE, 1);
    clientMutex = xSemaphoreCreateMutex();
    wifi_start();
    xTaskCreate(server_task, "host_srv", 3072, NULL, 10, NULL);
}

void uart_switch_speed(int baudrate) { (void) baudrate; }

void uartWrite(const uint8_t *data, size_t len) {
    xSemaphoreTake(clientMutex, portMAX_DELAY);
    const int sock = clientSock;
    while (sock >= 0 && len) {
        const int n = send(sock, data, len, 0);
        if (n <= 0) break;  // timeout or error: the frame is lost, the protocol retransmits
        data += n;
        len -= n;
    }
    xSemaphoreGive(clientMutex);
}

void uartTx(uint8_t data) { uartWrite(&data, 1); }

int uartRead(uint8_t *buf, int max) { return (int) xStreamBufferReceive(rxStream, buf, max, 0); }

void uartFlushInput() { xStreamBufferReset(rxStream); }

void uartWaitTxDone(uint32_t ms) { (void) ms; }  // send() returns once lwIP has the data

void uart_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[128];
    const int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (len > 0) uartWrite((const uint8_t *) buffer, len < (int) sizeof(buffer) ? len : sizeof(buffer) - 1);
}

#endif
