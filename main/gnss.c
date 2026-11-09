#include "gnss.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "modem.h"

static const char *TAG = "gnss";

#define MAX_FIELDS 24

esp_err_t gnss_init(void)
{
    char buf[128];
    if (modem_at("AT+CGNSSPWR=1", buf, sizeof(buf), 5000) != ESP_OK) {
        ESP_LOGE(TAG, "AT+CGNSSPWR=1 failed: %s", buf);
        return ESP_FAIL;
    }
    // GPS + GLONASS + BeiDou + Galileo (falls back silently if unsupported).
    modem_at("AT+CGNSSMODE=15", NULL, 0, 2000);
    ESP_LOGI(TAG, "GNSS powered on; first fix may take a minute outdoors");
    return ESP_OK;
}

// Values come either as decimal degrees ("49.8397") or NMEA ddmm.mmmm
// ("4950.3820") depending on modem firmware. Decimal degrees never have more
// than 3 integer digits, NMEA always has at least 4.
static double parse_coord(const char *s)
{
    const char *dot = strchr(s, '.');
    size_t int_digits = dot ? (size_t)(dot - s) : strlen(s);
    double v = atof(s);
    if (int_digits >= 4) {
        double deg = floor(v / 100.0);
        v = deg + (v - deg * 100.0) / 60.0;
    }
    return v;
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

static int64_t parse_ts_ms(const char *date, const char *time)
{
    // date: ddmmyy, time: hhmmss(.s)
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

// Splits on commas in place, keeping empty fields.
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

bool gnss_parse_cgnssinfo(const char *line, gnss_fix_t *out)
{
    // +CGNSSINFO: <mode>,<GPS-SVs>,[<GLONASS-SVs>,]<BEIDOU-SVs>,[<GALILEO-SVs>,]
    //             <lat>,<N/S>,<lon>,<E/W>,<date>,<UTC-time>,<alt>,<speed>,<course>,
    //             <PDOP>,<HDOP>,<VDOP>
    // The number of satellite-count fields differs between firmware versions, so
    // the parser anchors on the N/S hemisphere field instead of fixed indices.
    const char *p = strstr(line, "+CGNSSINFO:");
    if (!p) {
        return false;
    }
    p += strlen("+CGNSSINFO:");
    while (*p == ' ') {
        p++;
    }

    char buf[192];
    strlcpy(buf, p, sizeof(buf));
    char *eol = strpbrk(buf, "\r\n");
    if (eol) {
        *eol = '\0';
    }

    char *f[MAX_FIELDS];
    int n = split_fields(buf, f, MAX_FIELDS);

    int ns = -1;
    for (int i = 1; i < n; i++) {
        if (strcmp(f[i], "N") == 0 || strcmp(f[i], "S") == 0) {
            ns = i;
            break;
        }
    }
    // Need lat, N/S, lon, E/W, date, time, alt, speed, course, PDOP, HDOP.
    if (ns < 1 || ns + 9 >= n || f[ns - 1][0] == '\0') {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->lat = parse_coord(f[ns - 1]) * (f[ns][0] == 'S' ? -1 : 1);
    out->lon = parse_coord(f[ns + 1]) * (f[ns + 2][0] == 'W' ? -1 : 1);
    out->ts_ms = parse_ts_ms(f[ns + 3], f[ns + 4]);
    out->alt_m = atof(f[ns + 5]);
    out->speed_kmh = atof(f[ns + 6]) * 1.852f;  // knots -> km/h
    out->course_deg = atof(f[ns + 7]);
    out->hdop = atof(f[ns + 9]);
    for (int i = 1; i < ns - 1; i++) {  // satellite counts sit between mode and lat
        out->sats += atoi(f[i]);
    }
    return true;
}

esp_err_t gnss_get_fix(gnss_fix_t *out)
{
    char buf[256];
    if (modem_at("AT+CGNSSINFO", buf, sizeof(buf), 3000) != ESP_OK) {
        ESP_LOGW(TAG, "AT+CGNSSINFO failed: %s", buf);
        return ESP_FAIL;
    }
    if (!gnss_parse_cgnssinfo(buf, out)) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}
