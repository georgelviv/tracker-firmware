#pragma once

// How the tracker is currently connected to the internet. Filled by the
// active transport (Wi-Fi now; LTE later) and published with each fix.
typedef struct {
    const char *type;  // "wifi" (later: "lte")
    char name[33];     // Wi-Fi SSID (later: mobile operator)
    int rssi;          // signal strength, dBm
    int channel;       // Wi-Fi channel
    char ip[16];       // local IP address
} link_info_t;
