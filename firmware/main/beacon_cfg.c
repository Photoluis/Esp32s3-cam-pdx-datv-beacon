/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Configurazione del beacon (NVS, spazio "beacon", chiave "cfg") e ciclo autonomo. */
#include "beacon_cfg.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "live_tx.h"
#include "media_store.h"
#include "led_status.h"

#define NVS_NS  "beacon"
#define NVS_KEY "cfg"

int pdx_level_to_amp(double db)
{
    if (db > 0.0 || db < PDX_LEVEL_MIN_DB) return -1;
    const int a = (int)(PDX_RF_AMP_MAX * pow(10.0, db / 20.0) + 0.5);
    return a < 1 ? 1 : a;
}

double pdx_amp_to_level(int amp) { return 20.0 * log10((double)amp / PDX_RF_AMP_MAX); }

int pdx_parse_amp(const char *s)
{
    char *e;
    const double v = strtod(s, &e);
    if (e == s) return -1;
    const bool db = !strcasecmp(e, "db");
    if (*e && !db) return -1;
    if (db || v <= 0.0) return pdx_level_to_amp(v);
    return v >= 1.0 && v <= PDX_RF_AMP_MAX && v == (int)v ? (int)v : -1;
}

void pdx_cfg_defaults(pdx_cfg_t *c)
{
    /* 2375 MHz: segmento DATV dei 13 cm, lontano dai multipli dei clock di quarzo (40 MHz) e di flash/PSRAM (80 MHz) */
    *c = (pdx_cfg_t){
        .version = PDX_CFG_VERSION, .auto_tx = 0, .freq_khz = 2375000, .amp = 200, .dci = 0, .dcq = 0,
        .mono_s = 60, .cam_s = 60, .pause_s = 0, .boot_wait_s = 30, .callsign = "IZ7PDX", .ap_pass = "beacon13cm",
        .mono_src = PDX_MONO_STD, .cam_mode = PDX_CAMMODE_PIP, .pip_bg = 0, .ovl = 1, .fec = "3/4",
        .lo_khz = 0, .xvtr = PDX_XVTR_SUM,
    };
}

uint64_t pdx_cfg_final_khz(const pdx_cfg_t *c, uint32_t if_khz, bool *inverted)
{
    bool inv = false;
    uint64_t f = if_khz;
    if (c->lo_khz) {
        if (c->xvtr == PDX_XVTR_DIFF) {
            inv = c->lo_khz > if_khz;
            f = inv ? (uint64_t)c->lo_khz - if_khz : (uint64_t)if_khz - c->lo_khz;
        } else {
            f = (uint64_t)c->lo_khz + if_khz;
        }
    }
    if (inverted) *inverted = inv;
    return f;
}

const char *pdx_cfg_provider(const pdx_cfg_t *c, uint32_t if_khz)
{
    static char s[24];
    snprintf(s, sizeof(s), "BEACON %.3f MHz", pdx_cfg_final_khz(c, if_khz, NULL) / 1000.0);
    return s;
}

bool pdx_cfg_load(pdx_cfg_t *c)
{
    pdx_cfg_defaults(c);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    pdx_cfg_t tmp;
    size_t len = sizeof(tmp);
    const esp_err_t e = nvs_get_blob(h, NVS_KEY, &tmp, &len);
    nvs_close(h);
    if (e != ESP_OK) return false;
    /* versioni precedenti: la struttura salvata arriva fino ai campi di allora (con il riempimento finale), i campi
     * nuovi prendono i valori predefiniti */
    const size_t old_len = tmp.version == 2 ? offsetof(pdx_cfg_t, mono_src) : tmp.version == 3 ? offsetof(pdx_cfg_t, lo_khz) : 0;
    if (old_len && len == ((old_len + 3u) & ~(size_t)3u)) {
        pdx_cfg_t d;
        pdx_cfg_defaults(&d);
        memcpy((uint8_t *)&tmp + old_len, (const uint8_t *)&d + old_len, sizeof(tmp) - old_len);
        tmp.version = PDX_CFG_VERSION;
    } else if (len != sizeof(tmp) || tmp.version != PDX_CFG_VERSION) {
        return false;
    }
    tmp.fec[sizeof(tmp.fec) - 1] = 0;
    tmp.callsign[sizeof(tmp.callsign) - 1] = 0;
    tmp.ap_pass[sizeof(tmp.ap_pass) - 1] = 0;
    *c = tmp;
    return true;
}

bool pdx_cfg_save(const pdx_cfg_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_blob(h, NVS_KEY, c, sizeof(*c));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

void pdx_cfg_print(const pdx_cfg_t *c)
{
    static const char *const src[] = {"STD", "VIDEO", "IMAGE", "LIVE"};
    bool inv;
    const uint64_t fin = pdx_cfg_final_khz(c, c->freq_khz, &inv);
    if (c->lo_khz)
        printf("CFG transverter: IF %.3f MHz, OL %.3f MHz, finale %s = %.3f MHz%s\n", c->freq_khz / 1000.0,
               c->lo_khz / 1000.0, c->xvtr == PDX_XVTR_DIFF ? "|OL-IF|" : "OL+IF", fin / 1000.0,
               inv ? " (spettro invertito dal mixer: compensato)" : "");
    printf("CFG auto %s | freq %.3f MHz | FEC %s | livello %.1f dB (amp %d) | dc %d/%d | ciclo: monoscopio %s %u s, camera %s %u s, "
           "pausa %u s | sfondo PiP %s | sovrimpressione %s | attesa all'avvio %u s | QRZ %s\n",
           c->auto_tx ? "ON" : "OFF", c->freq_khz / 1000.0, c->fec, pdx_amp_to_level(c->amp), c->amp, c->dci, c->dcq, src[c->mono_src % 4],
           c->mono_s, c->cam_mode == PDX_CAMMODE_PIP ? "PIP" : "FULL", c->cam_s, c->pause_s,
           c->pip_bg ? "IMAGE" : "STD", c->ovl ? "ON" : "OFF", c->boot_wait_s, c->callsign);
}

static bool parse_int(const char *s, long lo, long hi, long *out)
{
    char *e;
    const long v = strtol(s, &e, 10);
    if (e == s || *e || v < lo || v > hi) return false;
    *out = v;
    return true;
}

const char *pdx_cfg_set(pdx_cfg_t *c, const char *key, const char *value)
{
    long v;
    if (!strcasecmp(key, "auto")) {
        if (!strcasecmp(value, "on") || !strcmp(value, "1")) c->auto_tx = 1;
        else if (!strcasecmp(value, "off") || !strcmp(value, "0")) c->auto_tx = 0;
        else return "auto ON/OFF";
    } else if (!strcasecmp(key, "freq")) {
        char *e;
        const double f = strtod(value, &e);
        const uint32_t khz = (uint32_t)(f * 1000.0 + 0.5);
        if (e == value || *e || khz < PDX_RF_FMIN_KHZ || khz > PDX_RF_FMAX_KHZ) return "freq 2300.000..2450.000 MHz";
        c->freq_khz = khz;
    } else if (!strcasecmp(key, "amp") || !strcasecmp(key, "level")) {
        /* level: dB teorici (0 = massimo, fino a -40); amp: ampiezza 1..480 o anche dB con il suffisso "dB" */
        char tmp[16];
        snprintf(tmp, sizeof(tmp), "%s%s", value, !strcasecmp(key, "level") && !strpbrk(value, "dD") ? "dB" : "");
        const int a = pdx_parse_amp(tmp);
        if (a < 1) return "livello da -40 a 0 dB (0 = massimo)";
        c->amp = (int16_t)a;
    } else if (!strcasecmp(key, "dci") || !strcasecmp(key, "dcq")) {
        if (!parse_int(value, -100, 100, &v)) return "dc -100..100";
        if (!strcasecmp(key, "dci")) c->dci = (int16_t)v; else c->dcq = (int16_t)v;
    } else if (!strcasecmp(key, "mono") || !strcasecmp(key, "cam") || !strcasecmp(key, "pause")) {
        if (!parse_int(value, 0, 3600, &v)) return "secondi 0..3600";
        if (!strcasecmp(key, "mono")) c->mono_s = (uint16_t)v;
        else if (!strcasecmp(key, "cam")) c->cam_s = (uint16_t)v;
        else c->pause_s = (uint16_t)v;
    } else if (!strcasecmp(key, "wait")) {
        if (!parse_int(value, 10, 600, &v)) return "wait 10..600 s";
        c->boot_wait_s = (uint16_t)v;
    } else if (!strcasecmp(key, "call")) {
        const size_t n = strlen(value);
        if (n < 3 || n >= sizeof(c->callsign)) return "QRZ da 3 a 15 caratteri";
        for (size_t i = 0; i < n; i++)
            if (!((value[i] >= 'A' && value[i] <= 'Z') || (value[i] >= 'a' && value[i] <= 'z') ||
                  (value[i] >= '0' && value[i] <= '9') || value[i] == '/' || value[i] == '-'))
                return "QRZ: lettere, cifre, / e -";
        strcpy(c->callsign, value);
        for (char *p = c->callsign; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    } else if (!strcasecmp(key, "appass")) {
        const size_t n = strlen(value);
        if (n < 8 || n >= sizeof(c->ap_pass)) return "password access point da 8 a 23 caratteri";
        strcpy(c->ap_pass, value);
    } else if (!strcasecmp(key, "monosrc")) {
        if (!strcasecmp(value, "std")) c->mono_src = PDX_MONO_STD;
        else if (!strcasecmp(value, "video")) c->mono_src = PDX_MONO_USR_VIDEO;
        else if (!strcasecmp(value, "image")) c->mono_src = PDX_MONO_USR_IMAGE;
        else if (!strcasecmp(value, "live")) c->mono_src = PDX_MONO_LIVE;
        else return "monosrc STD/VIDEO/IMAGE/LIVE";
    } else if (!strcasecmp(key, "cammode")) {
        if (!strcasecmp(value, "full")) c->cam_mode = PDX_CAMMODE_FULL;
        else if (!strcasecmp(value, "pip")) c->cam_mode = PDX_CAMMODE_PIP;
        else return "cammode FULL/PIP";
    } else if (!strcasecmp(key, "pipbg")) {
        if (!strcasecmp(value, "std")) c->pip_bg = 0;
        else if (!strcasecmp(value, "image")) c->pip_bg = 1;
        else return "pipbg STD/IMAGE";
    } else if (!strcasecmp(key, "ovl")) {
        if (!strcasecmp(value, "on") || !strcmp(value, "1")) c->ovl = 1;
        else if (!strcasecmp(value, "off") || !strcmp(value, "0")) c->ovl = 0;
        else return "ovl ON/OFF";
    } else if (!strcasecmp(key, "fec")) {
        static const char *const ok[] = {"1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "8/9", "9/10"};
        size_t i = 0;
        while (i < sizeof(ok) / sizeof(ok[0]) && strcmp(value, ok[i])) i++;
        if (i == sizeof(ok) / sizeof(ok[0])) return "fec 1/2 3/5 2/3 3/4 4/5 5/6 8/9 9/10";
        strcpy(c->fec, ok[i]);
    } else if (!strcasecmp(key, "lo")) {                 /* oscillatore locale esterno in MHz, 0 = nessun mixer */
        char *e;
        const double f = strtod(value, &e);
        if (e == value || *e || f < 0.0 || f * 1000.0 > PDX_LO_MAX_KHZ) return "lo 0..100000 MHz (0 = nessun mixer)";
        c->lo_khz = (uint32_t)(f * 1000.0 + 0.5);
    } else if (!strcasecmp(key, "xvtr")) {
        if (!strcasecmp(value, "sum")) c->xvtr = PDX_XVTR_SUM;
        else if (!strcasecmp(value, "diff")) c->xvtr = PDX_XVTR_DIFF;
        else return "xvtr SUM (OL+IF) / DIFF (|OL-IF|)";
    } else {
        return "chiavi: auto freq fec level amp dci dcq mono monosrc cam cammode pipbg ovl pause wait call appass lo xvtr";
    }
    return NULL;
}

/* attesa a TX spenta, interrotta dallo stop */
static bool wait_s(uint32_t s, pdx_rf_abort_fn abort_fn)
{
    const int64_t end = esp_timer_get_time() + (int64_t)s * 1000000;
    while (esp_timer_get_time() < end) {
        if (abort_fn && abort_fn()) return false;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return true;
}

/* un segmento del ciclo; restituisce false se la TX è stata fermata */
static bool segment(const pdx_cfg_t *c, uint32_t cycle, bool mono, pdx_rf_abort_fn tx_abort, bool *sent)
{
    bool inv;
    const uint64_t fin = pdx_cfg_final_khz(c, c->freq_khz, &inv);
    pdx_live_cfg_t lc = {.sr = 333000, .fec = c->fec, .pilots = true, .amp = c->amp, .dci = c->dci, .dcq = c->dcq,
                         .service = c->callsign, .provider = pdx_cfg_provider(c, c->freq_khz), .ovl_full = c->ovl != 0,
                         .freq_khz = fin, .es = pdx_media_map(PDX_MEDIA_ES_STD, NULL)};
    pdx_rf_set_invert(inv);
    const uint8_t *usr_img = pdx_media_map(PDX_MEDIA_IMG_USR, NULL);
    if (mono) {
        lc.video = PDX_LV_ES;
        if (c->mono_src == PDX_MONO_USR_VIDEO) {
            const uint8_t *v = pdx_media_map(PDX_MEDIA_ES_USR, NULL);
            if (v) lc.es = v;
            else printf("BEACON video personalizzato assente: monoscopio standard\n");
        } else if (c->mono_src == PDX_MONO_USR_IMAGE) {
            if (usr_img) { lc.video = PDX_LV_IMG; lc.bg = usr_img; }
            else printf("BEACON immagine personalizzata assente: monoscopio standard\n");
        } else if (c->mono_src == PDX_MONO_LIVE) {        /* barre standard con i dati in sovrimpressione */
            lc.video = PDX_LV_IMG;
            lc.bg = pdx_media_map(PDX_MEDIA_IMG_STD, NULL);
            lc.ovl_full = true;
        }
    } else {
        lc.video = c->cam_mode == PDX_CAMMODE_PIP ? PDX_LV_PIP : PDX_LV_CAM;
        lc.bg = c->pip_bg && usr_img ? usr_img : pdx_media_map(PDX_MEDIA_IMG_STD, NULL);
    }
    if (!pdx_rf_tune(c->freq_khz)) { printf("BEACON ERR sintonia\n"); return false; }
    const uint32_t want = (uint32_t)(mono ? c->mono_s : c->cam_s) * 1000u;
    printf("BEACON ciclo %u: %s %.3f MHz SR 333 kS FEC %s livello %.1f dB per %u s\n", (unsigned)cycle,
           pdx_live_video_name(lc.video), c->freq_khz / 1000.0, c->fec, pdx_amp_to_level(c->amp),
           (unsigned)(want / 1000u));
    if (c->lo_khz) printf("BEACON frequenza finale dopo il mixer %.3f MHz%s\n", fin / 1000.0, inv ? " (spettro compensato)" : "");
    fflush(stdout);
    const char *why = NULL;
    pdx_led_tx(pdx_amp_to_level(c->amp));
    const uint32_t ms = pdx_live_play(&lc, want, tx_abort, &why);
    pdx_led_idle();
    if (why) {
        printf("BEACON ERR %s: segmento saltato\n", why);
        return true;
    }
    *sent = true;
    const pdx_rf_stats_t *st = pdx_rf_last_stats();
    printf("BEACON TX OFF dopo %u ms (buffer vuoto %u, campioni in ritardo %u)\n", (unsigned)ms,
           (unsigned)st->underruns, (unsigned)st->late);
    return ms + 50u >= want;
}

uint32_t pdx_beacon_run(const pdx_cfg_t *c, pdx_rf_abort_fn tx_abort, pdx_rf_abort_fn idle_abort)
{
    if (!c->mono_s && !c->cam_s) {
        printf("BEACON niente da trasmettere: monoscopio e camera a 0 s\n");
        return 0;
    }
    for (uint32_t cycle = 1;; cycle++) {
        bool sent = false;
        if (c->mono_s && !segment(c, cycle, true, tx_abort, &sent)) return cycle - 1;
        if (c->cam_s && !segment(c, cycle, false, tx_abort, &sent)) return cycle - 1;
        if (!sent) {
            printf("BEACON nessun segmento trasmesso: ciclo fermato\n");
            return cycle - 1;
        }
        if (c->pause_s) {
            printf("BEACON pausa %u s (TX spenta)\n", c->pause_s);
            fflush(stdout);
            if (!wait_s(c->pause_s, idle_abort)) return cycle;
        }
    }
}
