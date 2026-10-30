/* Beacon DATV 13 cm - IZ7PDX
 * Configurazione del beacon (NVS, spazio "beacon", chiave "cfg") e ciclo autonomo. */
#include "beacon_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sym_player.h"

#define NVS_NS  "beacon"
#define NVS_KEY "cfg"

void pdx_cfg_defaults(pdx_cfg_t *c)
{
    /* 2375 MHz: segmento DATV dei 13 cm, lontano dai multipli dei clock di quarzo (40 MHz) e di flash/PSRAM (80 MHz) */
    *c = (pdx_cfg_t){
        .version = PDX_CFG_VERSION, .auto_tx = 0, .freq_khz = 2375000, .amp = 200, .dci = 0, .dcq = 0,
        .mono_s = 60, .cam_s = 0, .pause_s = 0, .boot_wait_s = 30, .callsign = "IZ7PDX", .ap_pass = "beacon13cm",
    };
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
    if (e != ESP_OK || len != sizeof(tmp) || tmp.version != PDX_CFG_VERSION) return false;
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
    printf("CFG auto %s | freq %.3f MHz | amp %d | dc %d/%d | ciclo: monoscopio %u s, camera %u s, pausa %u s | "
           "attesa all'avvio %u s | QRZ %s\n",
           c->auto_tx ? "ON" : "OFF", c->freq_khz / 1000.0, c->amp, c->dci, c->dcq, c->mono_s, c->cam_s, c->pause_s,
           c->boot_wait_s, c->callsign);
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
    } else if (!strcasecmp(key, "amp")) {
        if (!parse_int(value, 1, PDX_RF_AMP_MAX, &v)) return "amp 1..480";
        c->amp = (int16_t)v;
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
    } else {
        return "chiavi: auto freq amp dci dcq mono cam pause wait call appass";
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

uint32_t pdx_beacon_run(const pdx_cfg_t *c, pdx_rf_abort_fn tx_abort, pdx_rf_abort_fn idle_abort)
{
    pdx_sym_header_t h;
    const char *why = "";
    const bool have_mono = pdx_sym_open(&h, &why);
    if (c->mono_s && !have_mono) printf("BEACON monoscopio non disponibile: %s\n", why);
    if ((!c->mono_s || !have_mono) && !c->pause_s) {
        printf("BEACON niente da trasmettere (monoscopio assente o mono = 0, camera non ancora disponibile)\n");
        return 0;
    }
    for (uint32_t cycle = 1;; cycle++) {
        if (c->mono_s && have_mono) {
            if (!pdx_rf_tune(c->freq_khz)) { printf("BEACON ERR sintonia\n"); return cycle - 1; }
            printf("BEACON ciclo %u: MONOSCOPIO \"%s\" %.3f MHz SR %u S/s amp %d per %u s\n", (unsigned)cycle, h.label,
                   c->freq_khz / 1000.0, (unsigned)h.sr, c->amp, c->mono_s);
            fflush(stdout);
            const uint32_t want = (uint32_t)c->mono_s * 1000u;
            const uint32_t ms = pdx_sym_play(c->amp, c->dci, c->dcq, want, tx_abort);
            const pdx_rf_stats_t *st = pdx_rf_last_stats();
            printf("BEACON TX OFF dopo %u ms (buffer vuoto %u, campioni in ritardo %u)\n", (unsigned)ms,
                   (unsigned)st->underruns, (unsigned)st->late);
            if (ms + 50u < want) return cycle - 1;                 /* fermato */
        }
        if (c->cam_s) printf("BEACON camera: non ancora disponibile (fase C), salto %u s\n", c->cam_s);
        if (c->pause_s) {
            printf("BEACON pausa %u s (TX spenta)\n", c->pause_s);
            fflush(stdout);
            if (!wait_s(c->pause_s, idle_abort)) return cycle;
        }
    }
}
