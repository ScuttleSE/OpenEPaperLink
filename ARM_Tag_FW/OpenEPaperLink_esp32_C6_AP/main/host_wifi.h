#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Wi-Fi host link (host_wifi.c), for Improv Serial (improv.c)
bool hostWifiConnect(const char *ssid, const char *password, uint32_t timeoutMs);  // stores them in NVS
bool hostWifiIp(char *buf, size_t len);  // true when connected; the IP address in buf if buf is given
void hostWifiHostname(char *buf, size_t len);
bool hostWifiScanStart(void);  // pauses reconnecting while it scans; false if the scan can't start
void hostWifiScanDone(void);
uint8_t hostWifiLastReason(void);  // reason of the last disconnect (wifi_err_reason_t), 0 if none
void improvStart(void);
