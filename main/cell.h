#pragma once

#include <stdbool.h>
#include "esp_err.h"

// What the A7670 modem reports about itself, the SIM and the mobile network.
// Integer fields use CELL_NONE when the modem did not report them.
#define CELL_NONE (-9999)

typedef struct {
    bool present;          // modem answered AT commands
    // Fixed details, read once.
    char model[24];        // e.g. A7670G-LLSE
    char fw[32];           // modem firmware revision
    char imei[20];         // modem's unique hardware id
    // SIM
    char sim[24];          // ready / not inserted / PIN required / failure / ...
    char iccid[24];        // SIM card number, empty without a SIM
    char imsi[16];         // subscriber id stored on the SIM
    char number[20];       // phone number, if the SIM stores it
    char spn[24];          // operator name stored on the SIM
    char smsc[20];         // SMS centre number
    int sms_used, sms_total;  // messages stored on the SIM / its capacity
    int mode_pref;         // AT+CNMP: 2 automatic, 13 GSM only, 38 LTE only, 51 GSM+LTE
    int psm;               // power saving mode enabled (1) or not (0)
    // Packet data
    int attached;          // attached to the packet data service (1) or not (0)
    char apn[48];          // APN as granted by the network
    char dns1[16], dns2[16];  // DNS servers handed out by the network
    // Registration (3GPP 27.007): 0 not searching, 1 home, 2 searching,
    // 3 denied, 4 unknown, 5 roaming.
    int reg;
    char op[32];           // operator name
    int csq_dbm;           // signal from AT+CSQ, dBm
    int temp_c;            // modem chip temperature
    // Serving cell (AT+CPSI?, levels from AT+CESQ).
    char sys[16];          // LTE / GSM / NO SERVICE
    char mccmnc[8];        // country + network code, e.g. 255-03
    char area[12];         // TAC (LTE) / LAC (GSM), hex as reported
    long cell_id;          // cell identity, or CELL_NONE
    int pci;               // LTE physical cell id (0-503)
    int bw_khz;            // LTE downlink channel bandwidth
    char band[24];         // e.g. EUTRAN-BAND3 / EGSM 900
    int channel;           // EARFCN (LTE) / ARFCN (GSM)
    float rsrp, rsrq, rssi, sinr;  // LTE dBm / dB / dBm / dB; CELL_NONE if absent
} cell_info_t;

// Fills `out` from the modem (about 0.5 s of AT commands).
// Returns ESP_ERR_INVALID_STATE if the modem never came up.
esp_err_t cell_read(cell_info_t *out);

// ---- exposed for host unit tests ----
void cell_parse_cpsi(const char *resp, cell_info_t *out);
void cell_parse_cesq(const char *resp, cell_info_t *out);
void cell_parse_cops(const char *resp, cell_info_t *out);
void cell_parse_sim(const char *resp, bool ok, cell_info_t *out);
void cell_parse_cnum(const char *resp, cell_info_t *out);
void cell_parse_cgcontrdp(const char *resp, cell_info_t *out);
void cell_parse_cpms(const char *resp, cell_info_t *out);
// Integer after `prefix` (e.g. "+CGATT: "), or CELL_NONE.
int cell_parse_int(const char *resp, const char *prefix);
int cell_parse_reg(const char *resp);  // +CREG/+CEREG stat, or CELL_NONE
int cell_parse_csq(const char *resp);  // dBm, or CELL_NONE
