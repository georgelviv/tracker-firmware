#include "cell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CELL_HOST_TEST
#include "modem.h"
#endif

// ---------------------------------------------------------------------------
// Reply parsing (pure, host-testable)
// ---------------------------------------------------------------------------

// Copies the text after `prefix` up to the end of that line into out.
static bool line_after(const char *resp, const char *prefix, char *out, size_t len)
{
    const char *p = strstr(resp, prefix);
    if (!p) {
        return false;
    }
    p += strlen(prefix);
    size_t n = strcspn(p, "\r\n");
    if (n >= len) {
        n = len - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static int split_csv(char *s, char **f, int max)
{
    int n = 0;
    f[n++] = s;
    for (char *p = s; *p && n < max; p++) {
        if (*p == ',') {
            *p = '\0';
            f[n++] = p + 1;
        }
    }
    return n;
}

static void copy_field(char *dst, size_t len, const char *src)
{
    while (*src == ' ' || *src == '"') src++;
    strlcpy(dst, src, len);
    size_t n = strlen(dst);
    while (n && (dst[n - 1] == '"' || dst[n - 1] == ' ')) dst[--n] = '\0';
}

void cell_parse_cpsi(const char *resp, cell_info_t *out)
{
    char line[160];
    if (!line_after(resp, "+CPSI: ", line, sizeof(line))) {
        return;
    }
    char *f[16];
    int n = split_csv(line, f, 16);
    copy_field(out->sys, sizeof(out->sys), f[0]);
    if (n < 6) {
        return;  // "NO SERVICE,Online"
    }
    copy_field(out->mccmnc, sizeof(out->mccmnc), f[2]);
    copy_field(out->area, sizeof(out->area), f[3]);
    out->cell_id = strtol(f[4], NULL, 0);
    if (strcmp(out->sys, "LTE") == 0 && n >= 14) {
        // LTE,Online,MCC-MNC,TAC,SCellID,PCellID,Band,EARFCN,DLBW,ULBW,RSRQ,RSRP,RSSI,RSSNR
        // On the A7670 the RSRQ/RSRP/RSSI fields here are vendor index numbers,
        // so signal levels come from AT+CESQ instead; only RSSNR (dB) is used.
        copy_field(out->band, sizeof(out->band), f[6]);
        out->channel = atoi(f[7]);
        out->pci = f[5][0] ? atoi(f[5]) : CELL_NONE;
        // DL bandwidth index: 0..5 = 1.4, 3, 5, 10, 15, 20 MHz
        static const int BW_KHZ[] = {1400, 3000, 5000, 10000, 15000, 20000};
        int bw = f[8][0] ? atoi(f[8]) : -1;
        out->bw_khz = bw >= 0 && bw <= 5 ? BW_KHZ[bw] : CELL_NONE;
        out->sinr = f[13][0] ? atof(f[13]) : CELL_NONE;
    } else if (strcmp(out->sys, "GSM") == 0 && n >= 7) {
        // GSM,Online,MCC-MNC,LAC,CellID,"ARFCN Band",RxLev(dBm),...
        char *sp = strchr(f[5], ' ');
        out->channel = atoi(f[5]);
        copy_field(out->band, sizeof(out->band), sp ? sp + 1 : f[5]);
        out->rssi = atof(f[6]);
    }
}

void cell_parse_cops(const char *resp, cell_info_t *out)
{
    char line[96];
    if (!line_after(resp, "+COPS: ", line, sizeof(line))) {
        return;
    }
    char *f[5];
    if (split_csv(line, f, 5) >= 3) {  // mode,format,"name",AcT
        copy_field(out->op, sizeof(out->op), f[2]);
    }
}

void cell_parse_sim(const char *resp, bool ok, cell_info_t *out)
{
    char text[32] = "";
    if (ok && line_after(resp, "+CPIN: ", text, sizeof(text))) {
        strlcpy(out->sim, strcmp(text, "READY") == 0 ? "ready" : text, sizeof(out->sim));
    } else if (line_after(resp, "+CME ERROR: ", text, sizeof(text))) {
        strlcpy(out->sim, text, sizeof(out->sim));  // e.g. "SIM not inserted"
    } else {
        strlcpy(out->sim, "unknown", sizeof(out->sim));
    }
}

// +CESQ: rxlev,ber,rscp,ecno,rsrq,rsrp as 3GPP TS 27.007 index numbers.
void cell_parse_cesq(const char *resp, cell_info_t *out)
{
    const char *p = strstr(resp, "+CESQ: ");
    if (!p) return;
    int rxlev, ber, rscp, ecno, rsrq, rsrp;
    if (sscanf(p + 7, "%d,%d,%d,%d,%d,%d", &rxlev, &ber, &rscp, &ecno, &rsrq, &rsrp) != 6) return;
    if (rxlev >= 0 && rxlev <= 63) out->rssi = -111 + rxlev;           // 0: below -110 dBm
    if (rsrq >= 0 && rsrq <= 34) out->rsrq = -20.0f + rsrq / 2.0f;     // 0: below -19.5 dB
    if (rsrp >= 0 && rsrp <= 97) out->rsrp = -141 + rsrp;              // 0: below -140 dBm
}

// +CNUM: "","+380...",145
void cell_parse_cnum(const char *resp, cell_info_t *out)
{
    char line[80];
    if (!line_after(resp, "+CNUM: ", line, sizeof(line))) return;
    char *f[4];
    if (split_csv(line, f, 4) >= 2) copy_field(out->number, sizeof(out->number), f[1]);
}

// +CGCONTRDP: cid,bearer,"apn","ip","gateway","dns1","dns2",...
void cell_parse_cgcontrdp(const char *resp, cell_info_t *out)
{
    char line[200];
    if (!line_after(resp, "+CGCONTRDP: ", line, sizeof(line))) return;
    char *f[8];
    int n = split_csv(line, f, 8);
    if (n >= 3) copy_field(out->apn, sizeof(out->apn), f[2]);
    if (n >= 6) copy_field(out->dns1, sizeof(out->dns1), f[5]);
    if (n >= 7) copy_field(out->dns2, sizeof(out->dns2), f[6]);
}

// +CPMS: "SM",used,total,...
void cell_parse_cpms(const char *resp, cell_info_t *out)
{
    char line[96];
    if (!line_after(resp, "+CPMS: ", line, sizeof(line))) return;
    char *f[4];
    if (split_csv(line, f, 4) >= 3) {
        out->sms_used = atoi(f[1]);
        out->sms_total = atoi(f[2]);
    }
}

int cell_parse_int(const char *resp, const char *prefix)
{
    const char *p = strstr(resp, prefix);
    if (!p) return CELL_NONE;
    p += strlen(prefix);
    if (*p != '-' && (*p < '0' || *p > '9')) return CELL_NONE;
    return atoi(p);
}

int cell_parse_reg(const char *resp)
{
    const char *p = strstr(resp, "REG: ");  // +CREG: / +CEREG: n,stat
    if (!p) return CELL_NONE;
    p = strchr(p, ',');
    return p ? atoi(p + 1) : CELL_NONE;
}

int cell_parse_csq(const char *resp)
{
    const char *p = strstr(resp, "+CSQ: ");
    if (!p) return CELL_NONE;
    int v = atoi(p + 6);
    return v >= 0 && v <= 31 ? -113 + 2 * v : CELL_NONE;  // 99 = unknown
}

// ---------------------------------------------------------------------------
// Querying the modem (device only)
// ---------------------------------------------------------------------------
#ifndef CELL_HOST_TEST

static cell_info_t s_fixed;  // model / fw / imei, read once
static cell_info_t s_sim;    // SIM details, read once the SIM is ready

// First line of a reply that is just a value (AT+CIMI).
static void bare_line(const char *resp, char *out, size_t len)
{
    const char *p = resp + strspn(resp, "\r\n");
    size_t n = strcspn(p, "\r\n");
    if (n >= len) n = len - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static void read_sim(void)
{
    char buf[128];
    if (modem_at("AT+CIMI", buf, sizeof(buf), 1000) == ESP_OK) {
        bare_line(buf, s_sim.imsi, sizeof(s_sim.imsi));
    }
    if (modem_at("AT+CNUM", buf, sizeof(buf), 1000) == ESP_OK) cell_parse_cnum(buf, &s_sim);
    if (modem_at("AT+CSPN?", buf, sizeof(buf), 1000) == ESP_OK) {
        char line[48];
        char *f[3];
        if (line_after(buf, "+CSPN: ", line, sizeof(line)) && split_csv(line, f, 3) >= 1) {
            copy_field(s_sim.spn, sizeof(s_sim.spn), f[0]);
        }
    }
    if (modem_at("AT+CSCA?", buf, sizeof(buf), 1000) == ESP_OK) {
        char line[48];
        char *f[3];
        if (line_after(buf, "+CSCA: ", line, sizeof(line)) && split_csv(line, f, 3) >= 1) {
            copy_field(s_sim.smsc, sizeof(s_sim.smsc), f[0]);
        }
    }
    s_sim.mode_pref = modem_at("AT+CNMP?", buf, sizeof(buf), 1000) == ESP_OK
                          ? cell_parse_int(buf, "+CNMP: ") : CELL_NONE;
    s_sim.psm = modem_at("AT+CPSMS?", buf, sizeof(buf), 1000) == ESP_OK
                    ? cell_parse_int(buf, "+CPSMS: ") : CELL_NONE;
}

static void read_fixed(void)
{
    char buf[128];
    if (modem_at("AT+CGMM", buf, sizeof(buf), 1000) == ESP_OK) {
        copy_field(s_fixed.model, sizeof(s_fixed.model), buf + strspn(buf, "\r\n"));
        s_fixed.model[strcspn(s_fixed.model, "\r\n")] = '\0';
    }
    if (modem_at("AT+CGMR", buf, sizeof(buf), 1000) == ESP_OK) {
        line_after(buf, "+CGMR: ", s_fixed.fw, sizeof(s_fixed.fw));
    }
    if (modem_at("AT+CGSN", buf, sizeof(buf), 1000) == ESP_OK) {
        const char *p = buf + strspn(buf, "\r\n");
        size_t n = strcspn(p, "\r\n");
        if (n < sizeof(s_fixed.imei)) {
            memcpy(s_fixed.imei, p, n);
            s_fixed.imei[n] = '\0';
        }
    }
    // Report the operator by name ("lifecell") rather than its code in AT+COPS?.
    modem_at("AT+COPS=3,0", buf, sizeof(buf), 1000);
}

esp_err_t cell_read(cell_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->reg = out->csq_dbm = out->temp_c = out->channel = CELL_NONE;
    out->cell_id = CELL_NONE;
    out->rsrp = out->rsrq = out->rssi = out->sinr = CELL_NONE;
    out->pci = out->bw_khz = out->mode_pref = out->psm = out->attached = CELL_NONE;
    out->sms_used = out->sms_total = CELL_NONE;
    if (!modem_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    out->present = true;
    if (!s_fixed.model[0]) {
        read_fixed();
    }
    memcpy(out->model, s_fixed.model, sizeof(out->model));
    memcpy(out->fw, s_fixed.fw, sizeof(out->fw));
    memcpy(out->imei, s_fixed.imei, sizeof(out->imei));

    char buf[256];
    cell_parse_sim(buf, modem_at("AT+CPIN?", buf, sizeof(buf), 1000) == ESP_OK, out);
    if (strcmp(out->sim, "ready") == 0 &&
        modem_at("AT+CICCID", buf, sizeof(buf), 1000) == ESP_OK) {
        line_after(buf, "+ICCID: ", out->iccid, sizeof(out->iccid));
    }
    if (strcmp(out->sim, "ready") == 0) {
        if (!s_sim.imsi[0]) {
            read_sim();
        }
        memcpy(out->imsi, s_sim.imsi, sizeof(out->imsi));
        memcpy(out->number, s_sim.number, sizeof(out->number));
        memcpy(out->spn, s_sim.spn, sizeof(out->spn));
        memcpy(out->smsc, s_sim.smsc, sizeof(out->smsc));
        out->mode_pref = s_sim.mode_pref;
        out->psm = s_sim.psm;
        if (modem_at("AT+CPMS?", buf, sizeof(buf), 1000) == ESP_OK) cell_parse_cpms(buf, out);
    }

    // LTE registration (CEREG) wins over GSM/GPRS (CREG) when it is registered.
    int creg = CELL_NONE, cereg = CELL_NONE;
    if (modem_at("AT+CEREG?", buf, sizeof(buf), 1000) == ESP_OK) cereg = cell_parse_reg(buf);
    if (modem_at("AT+CREG?", buf, sizeof(buf), 1000) == ESP_OK) creg = cell_parse_reg(buf);
    out->reg = (cereg == 1 || cereg == 5 || creg == CELL_NONE) ? cereg : creg;

    if (modem_at("AT+CSQ", buf, sizeof(buf), 1000) == ESP_OK) out->csq_dbm = cell_parse_csq(buf);
    if (modem_at("AT+COPS?", buf, sizeof(buf), 2000) == ESP_OK) cell_parse_cops(buf, out);
    if (modem_at("AT+CPSI?", buf, sizeof(buf), 2000) == ESP_OK) cell_parse_cpsi(buf, out);
    if (modem_at("AT+CESQ", buf, sizeof(buf), 1000) == ESP_OK) cell_parse_cesq(buf, out);
    if (modem_at("AT+CGATT?", buf, sizeof(buf), 1000) == ESP_OK) {
        out->attached = cell_parse_int(buf, "+CGATT: ");
    }
    if (out->attached == 1 && modem_at("AT+CGCONTRDP", buf, sizeof(buf), 1000) == ESP_OK) {
        cell_parse_cgcontrdp(buf, out);
    }
    if (modem_at("AT+CPMUTEMP", buf, sizeof(buf), 1000) == ESP_OK) {
        const char *p = strstr(buf, "+CPMUTEMP: ");
        if (p) out->temp_c = atoi(p + 11);
    }
    return ESP_OK;
}

#endif  // CELL_HOST_TEST
