#include "gnss.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef GNSS_HOST_TEST
#include "board.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

#define MAX_FIELDS 24

// ---------------------------------------------------------------------------
// NMEA parsing (pure, host-testable)
// ---------------------------------------------------------------------------

// NMEA ddmm.mmmm / dddmm.mmmm -> decimal degrees.
static double parse_coord(const char *s)
{
    double v = atof(s);
    double deg = floor(v / 100.0);
    return deg + (v - deg * 100.0) / 60.0;
}

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's algorithm).
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// date: ddmmyy, time: hhmmss(.sss)
static int64_t parse_ts_ms(const char *date, const char *time)
{
    if (strlen(date) < 6 || strlen(time) < 6) {
        return 0;
    }
    int dd = (date[0] - '0') * 10 + (date[1] - '0');
    int mo = (date[2] - '0') * 10 + (date[3] - '0');
    int yy = (date[4] - '0') * 10 + (date[5] - '0');
    int hh = (time[0] - '0') * 10 + (time[1] - '0');
    int mi = (time[2] - '0') * 10 + (time[3] - '0');
    double ss = atof(time + 4);
    int64_t days = days_from_civil(2000 + yy, mo, dd);
    return (days * 86400 + hh * 3600 + mi * 60) * 1000 + (int64_t)llround(ss * 1000.0);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Validates "$...*HH" and copies the payload between '$' and '*' into out.
static bool checked_payload(const char *line, char *out, size_t out_len)
{
    if (line[0] != '$') {
        return false;
    }
    const char *star = strchr(line, '*');
    if (!star || !star[1] || !star[2]) {
        return false;
    }
    unsigned char sum = 0;
    for (const char *p = line + 1; p < star; p++) {
        sum ^= (unsigned char)*p;
    }
    int hi = hexval(star[1]), lo = hexval(star[2]);
    if (hi < 0 || lo < 0 || sum != (unsigned char)(hi * 16 + lo)) {
        return false;
    }
    size_t n = (size_t)(star - line - 1);
    if (n >= out_len) {
        return false;
    }
    memcpy(out, line + 1, n);
    out[n] = '\0';
    return true;
}

static int split_fields(char *s, char **fields, int max)
{
    int n = 0;
    fields[n++] = s;
    for (char *p = s; *p && n < max; p++) {
        if (*p == ',') {
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    return n;
}

static int system_of(const char *talker)
{
    if (strncmp(talker, "GP", 2) == 0) return GNSS_SYS_GPS;
    if (strncmp(talker, "GL", 2) == 0) return GNSS_SYS_GLONASS;
    if (strncmp(talker, "BD", 2) == 0 || strncmp(talker, "GB", 2) == 0) return GNSS_SYS_BEIDOU;
    if (strncmp(talker, "GA", 2) == 0) return GNSS_SYS_GALILEO;
    return -1;
}

static void update_snr_summary(gnss_nmea_state_t *st)
{
    int sum = 0, cnt = 0, max = 0;
    for (int i = 0; i < GNSS_SYS_COUNT; i++) {
        sum += st->snr_sum[i];
        cnt += st->snr_cnt[i];
        if (st->snr_max[i] > max) max = st->snr_max[i];
    }
    st->fix.snr_avg = cnt ? (float)sum / cnt : 0;
    st->fix.snr_max = max;
}

// GSA (NMEA 4.10+) ends with a system ID: 1 GPS, 2 GLONASS, 3 Galileo, 4 BeiDou.
static int system_of_id(int id)
{
    switch (id) {
    case 1: return GNSS_SYS_GPS;
    case 2: return GNSS_SYS_GLONASS;
    case 3: return GNSS_SYS_GALILEO;
    case 4: return GNSS_SYS_BEIDOU;
    default: return -1;
    }
}

// Rebuilds fix.sat from the per-constellation lists and the used-in-fix sets.
static void rebuild_sats(gnss_nmea_state_t *st)
{
    int n = 0;
    for (int sys = 0; sys < GNSS_SYS_COUNT; sys++) {
        for (int i = 0; i < st->sys_sat_n[sys] && n < GNSS_MAX_SATS; i++) {
            gnss_sat_t s = st->sys_sat[sys][i];
            s.used = false;
            for (int u = 0; u < st->used_n[sys]; u++) {
                if (st->used[sys][u] == s.prn) {
                    s.used = true;
                    break;
                }
            }
            st->fix.sat[n++] = s;
        }
    }
    st->fix.sat_count = n;
}

static int field_int(const char *s, int missing)
{
    return s[0] ? atoi(s) : missing;
}

bool gnss_nmea_parse(const char *line, gnss_nmea_state_t *st)
{
    char buf[128];
    if (!checked_payload(line, buf, sizeof(buf))) {
        return false;
    }
    char *f[MAX_FIELDS];
    int n = split_fields(buf, f, MAX_FIELDS);
    if (strlen(f[0]) != 5) {  // talker (GP/GN/BD/GL) + type
        return false;
    }
    const char *type = f[0] + 2;

    if (strcmp(type, "RMC") == 0 && n >= 10) {
        // $xxRMC,time,status,lat,N/S,lon,E/W,speed_kn,course,date,magvar,E/W,mode
        st->rmc_valid = f[2][0] == 'A' && f[3][0] && f[5][0];
        // The receiver keeps UTC once it has decoded any satellite, even without a fix.
        st->fix.clock_ms = parse_ts_ms(f[9], f[1]);
        st->fix.mode = n >= 13 && f[12][0] ? f[12][0] : 0;
        if (st->rmc_valid) {
            st->fix.lat = parse_coord(f[3]) * (f[4][0] == 'S' ? -1 : 1);
            st->fix.lon = parse_coord(f[5]) * (f[6][0] == 'W' ? -1 : 1);
            st->fix.speed_kmh = atof(f[7]) * 1.852f;  // knots -> km/h
            st->fix.course_deg = atof(f[8]);
            st->fix.ts_ms = parse_ts_ms(f[9], f[1]);
        }
        return true;
    }
    if (strcmp(type, "GGA") == 0 && n >= 12) {
        // $xxGGA,time,lat,N/S,lon,E/W,quality,sats,hdop,alt,M,geoid,M,...
        st->fix.quality = atoi(f[6]);
        st->fix.sats = atoi(f[7]);
        st->fix.hdop = atof(f[8]);
        st->fix.alt_m = atof(f[9]);
        st->fix.geoid_m = atof(f[11]);
        return true;
    }
    if (strcmp(type, "GSA") == 0 && n >= 18) {
        // $xxGSA,mode,fixType,sv1..sv12,pdop,hdop,vdop[,systemId]
        // One GSA per constellation; each lists the satellites used from it.
        st->fix.fix_type = atoi(f[2]);
        st->fix.pdop = atof(f[15]);
        st->fix.vdop = atof(f[17]);
        int sys = n >= 19 && f[18][0] ? system_of_id(atoi(f[18])) : system_of(f[0]);
        if (sys < 0 && f[3][0]) {  // "GN" talker without ID: guess from the numbering
            int prn = atoi(f[3]);
            sys = prn >= 65 && prn <= 96 ? GNSS_SYS_GLONASS : GNSS_SYS_GPS;
        }
        if (sys >= 0) {
            st->used_n[sys] = 0;
            for (int i = 3; i < 15; i++) {
                if (f[i][0]) {
                    st->used[sys][st->used_n[sys]++] = (uint8_t)atoi(f[i]);
                }
            }
            rebuild_sats(st);
        }
        return true;
    }
    if (strcmp(type, "GSV") == 0 && n >= 4) {
        // $xxGSV,numMsgs,msgNum,inView,{prn,elev,az,snr}x(1..4)[,signalId]
        int sys = system_of(f[0]);
        if (sys < 0) {
            return true;  // valid sentence, constellation we don't track
        }
        int total = atoi(f[1]), num = atoi(f[2]);
        if (num == 1) {
            st->gsv_sum = st->gsv_cnt = st->gsv_max = 0;
            st->gsv_sat_n = 0;
        }
        st->fix.in_view[sys] = atoi(f[3]);
        // Up to four satellites per sentence: prn, elevation, azimuth, snr.
        for (int i = 4; i + 3 < n && i < 4 + 4 * 4; i += 4) {
            if (!f[i][0]) {
                continue;
            }
            int snr = field_int(f[i + 3], -1);
            if (snr >= 0) {
                st->gsv_sum += snr;
                st->gsv_cnt++;
                if (snr > st->gsv_max) st->gsv_max = snr;
            }
            if (st->gsv_sat_n < GNSS_MAX_PER_SYS) {
                st->gsv_sat[st->gsv_sat_n++] = (gnss_sat_t){
                    .sys = (uint8_t)sys,
                    .prn = (uint8_t)atoi(f[i]),
                    .elev = (int8_t)field_int(f[i + 1], -1),
                    .az = (int16_t)field_int(f[i + 2], -1),
                    .snr = (int8_t)snr,
                };
            }
        }
        if (num == total) {
            st->snr_sum[sys] = st->gsv_sum;
            st->snr_cnt[sys] = st->gsv_cnt;
            st->snr_max[sys] = st->gsv_max;
            update_snr_summary(st);
            memcpy(st->sys_sat[sys], st->gsv_sat, sizeof(gnss_sat_t) * st->gsv_sat_n);
            st->sys_sat_n[sys] = st->gsv_sat_n;
            rebuild_sats(st);
        }
        return true;
    }
    if (strcmp(type, "GST") == 0 && n >= 9) {
        // $xxGST,time,rms,major,minor,orient,latErr,lonErr,altErr (metres, 1 sigma)
        st->fix.err_lat_m = atof(f[6]);
        st->fix.err_lon_m = atof(f[7]);
        st->fix.err_alt_m = atof(f[8]);
        return true;
    }
    if (strcmp(type, "TXT") == 0 && n >= 5) {
        // $GPTXT,01,01,02,ANTENNA OK / ANTENNA OPEN / ANTENNA SHORT
        const char *a = strstr(f[4], "ANTENNA ");
        if (a) {
            strlcpy(st->fix.antenna, a + 8, sizeof(st->fix.antenna));
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// UART reader (device only)
// ---------------------------------------------------------------------------
#ifndef GNSS_HOST_TEST

static const char *TAG = "gnss";

#define FIX_MAX_AGE_US   (5 * 1000000LL)   // a fix older than this is stale
#define NMEA_MAX_AGE_US  (5 * 1000000LL)   // no sentences for this long = no data

static SemaphoreHandle_t s_lock;
static gnss_nmea_state_t s_state;
static int64_t s_last_fix_us;   // when the last valid RMC arrived
static int64_t s_last_nmea_us;  // when any valid sentence arrived
static int64_t s_first_fix_us;  // first valid RMC since power-on (time to first fix)

// Logs each NMEA sentence type the first time it appears (e.g. GNGGA, BDGSV).
static void note_sentence_type(const char *line)
{
    static char seen[24][6];
    static int n;
    if (line[0] != '$' || strlen(line) < 6 || n >= 24) {
        return;
    }
    for (int i = 0; i < n; i++) {
        if (strncmp(seen[i], line + 1, 5) == 0) {
            return;
        }
    }
    memcpy(seen[n], line + 1, 5);
    seen[n++][5] = '\0';
    ESP_LOGI(TAG, "receiver sends %s", seen[n - 1]);
}

static void gnss_task(void *arg)
{
    char line[128];
    size_t len = 0;
    uint8_t chunk[128];

    for (;;) {
        int n = uart_read_bytes(GPS_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];
            if (c == '$') {
                len = 0;  // resync on every sentence start
            }
            if (c == '\n' || c == '\r') {
                if (len > 0) {
                    line[len] = '\0';
                    note_sentence_type(line);
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    if (gnss_nmea_parse(line, &s_state)) {
                        int64_t now = esp_timer_get_time();
                        s_last_nmea_us = now;
                        if (s_state.rmc_valid && strstr(line, "RMC")) {
                            s_last_fix_us = now;
                            if (!s_first_fix_us) {
                                s_first_fix_us = now;
                                ESP_LOGI(TAG, "first fix after %lld s", (long long)(now / 1000000));
                            }
                        }
                    }
                    xSemaphoreGive(s_lock);
                    len = 0;
                }
                continue;
            }
            if (len < sizeof(line) - 1) {
                line[len++] = c;
            } else {
                len = 0;  // overlong garbage
            }
        }
    }
}

esp_err_t gnss_init(void)
{
    s_lock = xSemaphoreCreateMutex();

    // Keep the L76K out of standby.
    gpio_reset_pin(GPS_WAKEUP_PIN);
    gpio_set_direction(GPS_WAKEUP_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(GPS_WAKEUP_PIN, 1);

    uart_config_t cfg = {
        .baud_rate = GPS_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_NUM, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(GPS_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_NUM, GPS_TX_PIN, GPS_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(gnss_task, "gnss", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "L76K GPS on UART%d (RX=%d TX=%d, %d baud)", GPS_UART_NUM, GPS_RX_PIN,
             GPS_TX_PIN, GPS_BAUDRATE);
    return ESP_OK;
}

esp_err_t gnss_get_fix(gnss_fix_t *out)
{
    int64_t now = esp_timer_get_time();
    esp_err_t ret;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_last_nmea_us == 0 || now - s_last_nmea_us > NMEA_MAX_AGE_US) {
        ret = ESP_ERR_INVALID_STATE;
    } else if (s_last_fix_us == 0 || now - s_last_fix_us > FIX_MAX_AGE_US ||
               s_state.fix.quality == 0) {
        // The L76K can briefly flag RMC valid while GGA already says "no fix"
        // (0 satellites, altitude 0); treat that as searching.
        *out = s_state.fix;  // sats/hdop are still useful while searching
        ret = ESP_ERR_NOT_FOUND;
    } else {
        *out = s_state.fix;
        ret = ESP_OK;
    }
    if (ret != ESP_ERR_INVALID_STATE) {
        // Measured from power-on (the L76K starts cold with the board).
        out->ttff_s = s_first_fix_us ? (int)(s_first_fix_us / 1000000) : -1;
    }
    xSemaphoreGive(s_lock);
    return ret;
}

#endif  // GNSS_HOST_TEST
