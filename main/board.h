#pragma once

// LilyGO T-A7670G R2 with L76K GPS (board code Q425), ESP32 variant.
// Modem: SIMCom A7670G-LLSE, which has no built-in GNSS. Position comes from
// a separate Quectel L76K GPS module wired to the ESP32.
// Source: Xinyuan-LilyGO/LilyGO-T-A76XX, examples/*/utilities.h (LILYGO_T_A7670)
// and examples/ExternalGPS_A7670G_Only.

// ---- Modem (A7670G) ----
#define MODEM_UART_NUM        UART_NUM_1
#define MODEM_BAUDRATE        115200
#define MODEM_TX_PIN          26   // ESP32 TX -> modem RX
#define MODEM_RX_PIN          27   // ESP32 RX <- modem TX
#define MODEM_DTR_PIN         25
#define MODEM_RING_PIN        33

#define BOARD_POWERON_PIN     12   // enables the modem power rail
#define BOARD_PWRKEY_PIN      4
#define MODEM_RESET_PIN       5
#define MODEM_RESET_LEVEL     1

// ---- GPS (L76K) ----
#define GPS_UART_NUM          UART_NUM_2
#define GPS_BAUDRATE          9600
#define GPS_RX_PIN            22   // ESP32 RX <- L76K TX
#define GPS_TX_PIN            21   // ESP32 TX -> L76K RX
#define GPS_PPS_PIN           23
#define GPS_WAKEUP_PIN        19   // high = running, low = standby

#define BOARD_BAT_ADC_PIN     35
