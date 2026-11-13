#pragma once

// How the tracker is currently connected to the internet. Filled by the
// active transport and published with each report.
typedef struct {
    const char *type;  // "wifi" or "lte"
    char name[33];     // Wi-Fi SSID or mobile operator
    int rssi;          // Wi-Fi RSSI / LTE RSRP, dBm; 0 if unknown
    int channel;       // Wi-Fi channel; 0 for LTE
    char ip[16];       // local IP address
} link_info_t;
