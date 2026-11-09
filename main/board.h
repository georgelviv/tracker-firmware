#pragma once

// LilyGO T-SIM A7670E (ESP32 variant).
// Source: Xinyuan-LilyGO/LilyGO-T-A76XX examples/*/utilities.h, LILYGO_T_A7670 block.

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

#define BOARD_BAT_ADC_PIN     35
