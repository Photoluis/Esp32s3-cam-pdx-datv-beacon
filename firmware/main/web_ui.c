/* Beacon DATV 13 cm - IZ7PDX
 * Access point Wi-Fi e server HTTP di configurazione. Il server gira sul core 1; le modifiche passano da pdx_cfg_set(),
 * con gli stessi controlli del comando SET della seriale. */
#include "web_ui.h"

#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "media_store.h"
#include "rf_s3.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static pdx_cfg_t *cfg;
static const char *fw;
static httpd_handle_t server;
static SemaphoreHandle_t cfg_mutex;
static volatile bool start_request, client_seen;
static bool radio_dirty;                     /* l'access point è stato acceso dall'avvio: la PHY non è più quella collaudata */
static char mode_text[48] = "configurazione";
void pdx_cfg_lock(void) { if (cfg_mutex) xSemaphoreTake(cfg_mutex, portMAX_DELAY); }
void pdx_cfg_unlock(void) { if (cfg_mutex) xSemaphoreGive(cfg_mutex); }

void pdx_web_init(pdx_cfg_t *c, const char *fw_version)
{
    cfg = c;
    fw = fw_version;
    if (!cfg_mutex) cfg_mutex = xSemaphoreCreateMutex();
}

void pdx_web_set_mode(const char *mode) { snprintf(mode_text, sizeof(mode_text), "%s", mode); }

bool pdx_web_take_start_request(void)
{
    const bool r = start_request;
    start_request = false;
    return r;
}

bool pdx_web_seen_client(void) { return client_seen; }
bool pdx_web_radio_dirty(void) { return radio_dirty; }
bool pdx_web_running(void) { return server != NULL; }

/* ------------------------------------------------------------ risposte */

static int cfg_json(char *b, size_t n)
{
    static const char *const src[] = {"std", "video", "image"};
    return snprintf(b, n,
                    "{\"auto\":%d,\"freq\":%.3f,\"fec\":\"%s\",\"amp\":%d,\"level\":%.1f,\"dci\":%d,\"dcq\":%d,\"mono\":%u,"
                    "\"monosrc\":\"%s\",\"cam\":%u,\"cammode\":\"%s\",\"pipbg\":\"%s\",\"ovl\":%d,\"pause\":%u,"
                    "\"wait\":%u,\"call\":\"%s\"}",
                    cfg->auto_tx, cfg->freq_khz / 1000.0, cfg->fec, cfg->amp, pdx_amp_to_level(cfg->amp), cfg->dci, cfg->dcq, cfg->mono_s,
                    src[cfg->mono_src % 3], cfg->cam_s, cfg->cam_mode == PDX_CAMMODE_PIP ? "pip" : "full",
                    cfg->pip_bg ? "image" : "std", cfg->ovl, cfg->pause_s, cfg->boot_wait_s, cfg->callsign);
}

static esp_err_t send_json(httpd_req_t *r, const char *s)
{
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, s);
}

static esp_err_t h_index(httpd_req_t *r)
{
    client_seen = true;
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, index_html_start, index_html_end - index_html_start);
}

static esp_err_t h_status(httpd_req_t *r)
{
    client_seen = true;
    char c[420], b[1100], md[4][96];
    pdx_cfg_lock();
    cfg_json(c, sizeof(c));
    pdx_cfg_unlock();
    for (int m = 0; m < 4; m++) pdx_media_describe((pdx_media_t)m, md[m], sizeof(md[m]));
    snprintf(b, sizeof(b),
             "{\"fw\":\"%s\",\"mode\":\"%s\",\"uptime\":%lld,\"cfg\":%s,"
             "\"media\":{\"es_std\":\"%s\",\"es_usr\":\"%s\",\"img_std\":\"%s\",\"img_usr\":\"%s\","
             "\"es_usr_ok\":%s,\"img_usr_ok\":%s,\"es_max\":%u},\"fmin\":%u,\"fmax\":%u,\"ampmax\":%d}",
             fw, mode_text, (long long)(esp_timer_get_time() / 1000000), c, md[0], md[1], md[2], md[3],
             pdx_media_map(PDX_MEDIA_ES_USR, NULL) ? "true" : "false",
             pdx_media_map(PDX_MEDIA_IMG_USR, NULL) ? "true" : "false", (unsigned)PDX_ES_USR_SIZE,
             (unsigned)(PDX_RF_FMIN_KHZ / 1000), (unsigned)(PDX_RF_FMAX_KHZ / 1000), PDX_RF_AMP_MAX);
    return send_json(r, b);
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && s[1] && s[2]) {
            char hx[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hx, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

/* POST /api/set  corpo "chiave=valore&chiave=valore" (form urlencoded) */
static esp_err_t h_set(httpd_req_t *r)
{
    client_seen = true;
    char body[512];
    const int len = r->content_len < (int)sizeof(body) - 1 ? r->content_len : (int)sizeof(body) - 1;
    int got = 0;
    while (got < len) {
        const int k = httpd_req_recv(r, body + got, len - got);
        if (k <= 0) return ESP_FAIL;
        got += k;
    }
    body[got] = 0;
    char errs[300] = "", c[420], out[800];
    pdx_cfg_lock();
    for (char *save = NULL, *kv = strtok_r(body, "&", &save); kv; kv = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = kv, *val = eq + 1;
        url_decode(key);
        url_decode(val);
        if (!strcmp(key, "appass") && !*val) continue;        /* campo password vuoto = invariata */
        const char *why = pdx_cfg_set(cfg, key, val);
        if (why) {
            const size_t n = strlen(errs);
            snprintf(errs + n, sizeof(errs) - n, "%s\"%s: %s\"", n ? "," : "", key, why);
        }
    }
    cfg_json(c, sizeof(c));
    pdx_cfg_unlock();
    snprintf(out, sizeof(out), "{\"ok\":%s,\"errors\":[%s],\"cfg\":%s}", errs[0] ? "false" : "true", errs, c);
    return send_json(r, out);
}

static esp_err_t h_save(httpd_req_t *r)
{
    pdx_cfg_lock();
    const bool ok = pdx_cfg_save(cfg);
    pdx_cfg_unlock();
    return send_json(r, ok ? "{\"ok\":true,\"msg\":\"configurazione salvata\"}" : "{\"ok\":false,\"msg\":\"errore NVS\"}");
}

static esp_err_t h_defaults(httpd_req_t *r)
{
    pdx_cfg_lock();
    char pass[sizeof(cfg->ap_pass)];
    strcpy(pass, cfg->ap_pass);                              /* la password resta: non si perde l'accesso */
    pdx_cfg_defaults(cfg);
    strcpy(cfg->ap_pass, pass);
    pdx_cfg_unlock();
    return send_json(r, "{\"ok\":true,\"msg\":\"valori predefiniti (non ancora salvati)\"}");
}

/* tipo di file dalla query: ?kind=video | ?kind=image */
static bool query_kind(httpd_req_t *r, pdx_media_t *m)
{
    char q[32], v[8];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK || httpd_query_key_value(q, "kind", v, sizeof(v)) != ESP_OK)
        return false;
    if (!strcmp(v, "video")) *m = PDX_MEDIA_ES_USR;
    else if (!strcmp(v, "image")) *m = PDX_MEDIA_IMG_USR;
    else return false;
    return true;
}

static esp_err_t media_reply(httpd_req_t *r, pdx_media_t m, const char *err)
{
    char d[96], out[200];
    if (err) snprintf(out, sizeof(out), "{\"ok\":false,\"msg\":\"%s\"}", err);
    else {
        pdx_media_describe(m, d, sizeof(d));
        snprintf(out, sizeof(out), "{\"ok\":true,\"msg\":\"caricato: %s\"}", d);
    }
    return send_json(r, out);
}

/* POST /api/upload?kind=video|image, corpo = file binario (PDXES001 dal PC, PDXIMG01 preparato dalla pagina) */
static esp_err_t h_upload(httpd_req_t *r)
{
    client_seen = true;
    pdx_media_t m;
    if (!query_kind(r, &m)) return send_json(r, "{\"ok\":false,\"msg\":\"kind=video|image\"}");
    const char *e = pdx_media_write_begin(m, (uint32_t)r->content_len);
    static uint8_t buf[2048];
    for (int left = r->content_len; !e && left > 0;) {
        const int k = httpd_req_recv(r, (char *)buf, left < (int)sizeof(buf) ? left : (int)sizeof(buf));
        if (k == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (k <= 0) { e = "connessione interrotta"; break; }
        e = pdx_media_write(buf, (uint32_t)k);
        left -= k;
    }
    const char *e2 = pdx_media_write_end();
    if (!e) e = e2;
    if (e) pdx_media_erase_user(m);                  /* un file a metà non deve restare in flash */
    return media_reply(r, m, e);
}

/* POST /api/erase?kind=video|image: cancella un file personalizzato (quelli standard non si possono cancellare) */
static esp_err_t h_erase(httpd_req_t *r)
{
    pdx_media_t m;
    if (!query_kind(r, &m)) return send_json(r, "{\"ok\":false,\"msg\":\"kind=video|image\"}");
    const char *e = pdx_media_erase_user(m);
    return send_json(r, e ? "{\"ok\":false,\"msg\":\"cancellazione non riuscita\"}"
                          : "{\"ok\":true,\"msg\":\"file personalizzato cancellato\"}");
}

static esp_err_t h_start(httpd_req_t *r)
{
    start_request = true;
    return send_json(r, "{\"ok\":true,\"msg\":\"beacon in partenza: il Wi-Fi si spegne. Per fermarlo premere BOOT.\"}");
}

/* ------------------------------------------------------------ access point e server */

bool pdx_web_start(void)
{
    if (server) return true;
    wifi_config_t ap = {0};
    pdx_cfg_lock();
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s-BEACON", cfg->callsign);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", cfg->ap_pass);
    pdx_cfg_unlock();
    ap.ap.ssid_len = (uint8_t)strlen((char *)ap.ap.ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;
    radio_dirty = true;
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        printf("WEB ERR access point\n");
        pdx_rf_wifi_null();
        return false;
    }
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.core_id = 1;
    hc.stack_size = 6144;
    hc.max_uri_handlers = 10;
    hc.recv_wait_timeout = 15;                                  /* la flash si cancella a blocchi durante il caricamento */
    hc.lru_purge_enable = true;
    if (httpd_start(&server, &hc) != ESP_OK) {
        printf("WEB ERR server\n");
        server = NULL;
        pdx_rf_wifi_null();
        return false;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/set", .method = HTTP_POST, .handler = h_set},
        {.uri = "/api/save", .method = HTTP_POST, .handler = h_save},
        {.uri = "/api/defaults", .method = HTTP_POST, .handler = h_defaults},
        {.uri = "/api/start", .method = HTTP_POST, .handler = h_start},
        {.uri = "/api/upload", .method = HTTP_POST, .handler = h_upload},
        {.uri = "/api/erase", .method = HTTP_POST, .handler = h_erase},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(server, &uris[i]);
    client_seen = false;
    start_request = false;
    printf("WEB access point \"%s\" attivo: http://192.168.4.1\n", (char *)ap.ap.ssid);
    return true;
}

void pdx_web_stop(void)
{
    if (!server) return;
    httpd_stop(server);
    server = NULL;
    pdx_rf_wifi_null();
    printf("WEB access point spento (Wi-Fi nello stato del modulatore)\n");
}
