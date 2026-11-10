#include "firebase.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "battery.h"
#include "diag.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "firebase";

#define AUTH_URL "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key="
#define TOKEN_MAX 2048
#define RESP_MAX 4096

static char s_id_token[TOKEN_MAX];
static int64_t s_token_expires_us;

// Performs a request and reads up to resp_len-1 bytes of body into resp.
static esp_err_t http_request(esp_http_client_method_t method, const char *url,
                              const char *body, char *resp, size_t resp_len,
                              int *status)
{
    if (resp && resp_len > 0) {
        resp[0] = '\0';  // never log stale buffer contents on failure
    }
    *status = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 4096,  // the ID token travels in the query string
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");

    size_t body_len = body ? strlen(body) : 0;
    esp_err_t err = esp_http_client_open(c, body_len);
    if (err == ESP_OK && body_len > 0 &&
        esp_http_client_write(c, body, body_len) != (int)body_len) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(c) < 0) {
        err = ESP_FAIL;
    }
    if (err == ESP_OK) {
        *status = esp_http_client_get_status_code(c);
        int total = 0, n;
        if (resp && resp_len > 0) {
            while (total < (int)resp_len - 1 &&
                   (n = esp_http_client_read(c, resp + total, resp_len - 1 - total)) > 0) {
                total += n;
            }
            resp[total] = '\0';
        }
    }
    esp_http_client_cleanup(c);
    return err;
}

static esp_err_t sign_in(void)
{
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "email", CONFIG_TRACKER_DEVICE_EMAIL);
    cJSON_AddStringToObject(req, "password", CONFIG_TRACKER_DEVICE_PASSWORD);
    cJSON_AddBoolToObject(req, "returnSecureToken", true);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    char url[256];
    snprintf(url, sizeof(url), AUTH_URL "%s", CONFIG_TRACKER_FIREBASE_API_KEY);

    char *resp = malloc(RESP_MAX);
    int status = 0;
    esp_err_t err = http_request(HTTP_METHOD_POST, url, body, resp, RESP_MAX, &status);
    free(body);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "sign-in failed (err=%s, HTTP %d): %s", esp_err_to_name(err), status,
                 resp);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(resp);
    free(resp);
    const cJSON *tok = cJSON_GetObjectItem(json, "idToken");
    const cJSON *exp = cJSON_GetObjectItem(json, "expiresIn");
    if (!cJSON_IsString(tok) || strlen(tok->valuestring) >= TOKEN_MAX) {
        ESP_LOGE(TAG, "sign-in response has no usable idToken");
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    strlcpy(s_id_token, tok->valuestring, sizeof(s_id_token));
    int expires_s = cJSON_IsString(exp) ? atoi(exp->valuestring) : 3600;
    // Refresh five minutes early.
    s_token_expires_us = esp_timer_get_time() + (int64_t)(expires_s - 300) * 1000000;
    cJSON_Delete(json);

    ESP_LOGI(TAG, "signed in as %s", CONFIG_TRACKER_DEVICE_EMAIL);
    return ESP_OK;
}

static esp_err_t ensure_token(void)
{
    if (s_id_token[0] && esp_timer_get_time() < s_token_expires_us) {
        return ESP_OK;
    }
    return sign_in();
}

static const char *SYS_NAMES[GNSS_SYS_COUNT] = {"gps", "glonass", "beidou", "galileo"};

static void add_satellites(cJSON *o, const gnss_fix_t *fix)
{
    cJSON_AddNumberToObject(o, "sats", fix->sats);
    cJSON_AddNumberToObject(o, "snrAvg", fix->snr_avg);
    cJSON_AddNumberToObject(o, "snrMax", fix->snr_max);
    cJSON *view = cJSON_AddObjectToObject(o, "inView");
    for (int i = 0; i < GNSS_SYS_COUNT; i++) {
        cJSON_AddNumberToObject(view, SYS_NAMES[i], fix->in_view[i]);
    }
}

static void add_server_time(cJSON *o)
{
    cJSON *sv = cJSON_AddObjectToObject(o, "receivedAt");
    cJSON_AddStringToObject(sv, ".sv", "timestamp");  // server-side timestamp
}

// Last known position; written only when there is a fix.
static cJSON *position_json(const gnss_fix_t *fix, const link_info_t *link)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "lat", fix->lat);
    cJSON_AddNumberToObject(o, "lon", fix->lon);
    cJSON_AddNumberToObject(o, "alt", fix->alt_m);
    cJSON_AddNumberToObject(o, "speed", fix->speed_kmh);
    cJSON_AddNumberToObject(o, "course", fix->course_deg);
    cJSON_AddNumberToObject(o, "hdop", fix->hdop);
    cJSON_AddNumberToObject(o, "sats", fix->sats);
    cJSON_AddNumberToObject(o, "ts", (double)fix->ts_ms);
    cJSON_AddStringToObject(o, "via", link ? link->type : "wifi");

    cJSON *gps = cJSON_AddObjectToObject(o, "gps");
    cJSON_AddNumberToObject(gps, "quality", fix->quality);
    cJSON_AddNumberToObject(gps, "fixType", fix->fix_type);
    cJSON_AddNumberToObject(gps, "pdop", fix->pdop);
    cJSON_AddNumberToObject(gps, "vdop", fix->vdop);
    cJSON_AddNumberToObject(gps, "geoid", fix->geoid_m);
    add_satellites(gps, fix);
    add_server_time(o);
    return o;
}

static const char *GPS_STATE_NAMES[] = {"fix", "searching", "no-data"};

// Live health; written every report cycle, with or without a fix.
static cJSON *status_json(gps_state_t state, const gnss_fix_t *fix, const link_info_t *link)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *gps = cJSON_AddObjectToObject(o, "gps");
    cJSON_AddStringToObject(gps, "state", GPS_STATE_NAMES[state]);
    if (state != GPS_NO_DATA) {
        add_satellites(gps, fix);
    }

    if (link) {
        cJSON *net = cJSON_AddObjectToObject(o, "net");
        cJSON_AddStringToObject(net, "type", link->type);
        cJSON_AddStringToObject(net, "name", link->name);
        cJSON_AddNumberToObject(net, "rssi", link->rssi);
        cJSON_AddNumberToObject(net, "channel", link->channel);
        cJSON_AddStringToObject(net, "ip", link->ip);
    }

    cJSON *dev = cJSON_AddObjectToObject(o, "device");
    cJSON_AddNumberToObject(dev, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(dev, "fw", esp_app_get_description()->version);
    int bat = battery_read_mv();
    if (bat > 0) {
        cJSON_AddNumberToObject(dev, "battery", bat);  // mV
    }
    const diag_info_t *d = diag_get();
    cJSON_AddStringToObject(dev, "reset", d->reset_reason);
    cJSON_AddNumberToObject(dev, "boots", d->boots);
    cJSON_AddNumberToObject(dev, "brownouts", d->brownouts);
    if (d->prev.boot) {
        cJSON *prev = cJSON_AddObjectToObject(dev, "prev");
        cJSON_AddNumberToObject(prev, "boot", d->prev.boot);
        cJSON_AddNumberToObject(prev, "uptime", d->prev.uptime_s);
        cJSON_AddNumberToObject(prev, "uploads", d->prev.uploads);
        cJSON_AddNumberToObject(prev, "failures", d->prev.failures);
        cJSON_AddNumberToObject(prev, "noFix", d->prev.no_fix);
        if (d->prev.min_mv > 0) {
            cJSON_AddNumberToObject(prev, "minBattery", d->prev.min_mv);
        }
    }
    add_server_time(o);
    return o;
}

esp_err_t firebase_publish(gps_state_t state, const gnss_fix_t *fix, const link_info_t *link)
{
    if (ensure_token() != ESP_OK) {
        return ESP_FAIL;
    }

    // One multi-path update: status always, latest only with a fix.
    cJSON *json = cJSON_CreateObject();
    cJSON_AddItemToObject(json, "status", status_json(state, fix, link));
    if (state == GPS_FIX) {
        cJSON_AddItemToObject(json, "latest", position_json(fix, link));
    }
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    size_t url_len = strlen(CONFIG_TRACKER_FIREBASE_DB_URL) + strlen(s_id_token) + 128;
    char *url = malloc(url_len);
    char resp[256];
    int status = 0;
    esp_err_t err = ESP_FAIL;

    for (int attempt = 0; attempt < 2; attempt++) {
        snprintf(url, url_len, "%s/devices/%s.json?print=silent&auth=%s",
                 CONFIG_TRACKER_FIREBASE_DB_URL, CONFIG_TRACKER_DEVICE_ID, s_id_token);
        err = http_request(HTTP_METHOD_PATCH, url, body, resp, sizeof(resp), &status);
        if (err == ESP_OK && (status == 200 || status == 204)) {
            break;
        }
        if (status == 401 && attempt == 0) {  // token revoked/expired early
            s_id_token[0] = '\0';
            if (sign_in() == ESP_OK) {
                continue;
            }
        }
        ESP_LOGE(TAG, "write failed (err=%s, HTTP %d): %s", esp_err_to_name(err), status, resp);
        err = ESP_FAIL;
        break;
    }

    free(url);
    free(body);
    return err;
}
