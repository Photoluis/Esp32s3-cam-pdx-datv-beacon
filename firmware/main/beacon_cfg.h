/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Configurazione del beacon salvata in NVS e ciclo autonomo (monoscopio / camera / pausa). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "rf_s3.h"

#define PDX_CFG_VERSION 4

typedef struct {
    uint16_t version;
    uint8_t auto_tx;        /* 1 = all'accensione parte da solo dopo boot_wait_s */
    uint8_t reserved;
    uint32_t freq_khz;      /* 2300000..2450000 */
    int16_t amp;            /* 1..PDX_RF_AMP_MAX */
    int16_t dci, dcq;       /* correzione DC del modulatore */
    uint16_t mono_s;        /* durata del monoscopio in ogni ciclo, s (0 = saltato) */
    uint16_t cam_s;         /* durata della diretta camera in ogni ciclo, s (0 = saltata) */
    uint16_t pause_s;       /* TX spenta tra un ciclo e l'altro, s */
    uint16_t boot_wait_s;   /* attesa prima della partenza automatica, s (minimo 10) */
    char callsign[16];
    char ap_pass[24];       /* password WPA2 dell'access point di configurazione (8..23 caratteri) */
    /* dalla versione 3 */
    uint8_t mono_src;       /* PDX_MONO_*: monoscopio standard, video personalizzato, immagine personalizzata */
    uint8_t cam_mode;       /* PDX_CAMMODE_*: camera a pieno schermo o in PiP sul monoscopio */
    uint8_t pip_bg;         /* sfondo del PiP: 0 standard, 1 immagine personalizzata */
    uint8_t ovl;            /* sovrimpressione con QRZ e frequenza sui video dal vivo */
    char fec[6];            /* "1/2" ... "9/10" (DVB-S2 QPSK, frame normali con pilot) */
    /* dalla versione 4: uscita usata come IF verso un mixer esterno (transverter) */
    uint32_t lo_khz;        /* oscillatore locale esterno, kHz (0 = nessun mixer: si trasmette sulla IF) */
    uint8_t xvtr;           /* PDX_XVTR_*: frequenza finale = OL + IF oppure |OL - IF| */
    uint8_t led;            /* PDX_LED_*: 0 normale, 1 minimo, 2 spento (era riservato: 0 nelle configurazioni salvate) */
    uint8_t telem;          /* riga di telemetria in sovrimpressione: temperatura del chip, accensione, ciclo */
    uint8_t rsv4[1];
} pdx_cfg_t;

enum { PDX_MONO_STD, PDX_MONO_USR_VIDEO, PDX_MONO_USR_IMAGE, PDX_MONO_LIVE };
enum { PDX_XVTR_SUM, PDX_XVTR_DIFF };

#define PDX_LO_MAX_KHZ 100000000u   /* 100 GHz */
/* frequenza finale in aria per una IF; *inverted = spettro invertito dal mixer (OL sopra la IF, prodotto differenza) */
uint64_t pdx_cfg_final_khz(const pdx_cfg_t *c, uint32_t if_khz, bool *inverted);
/* nome del fornitore nella SDT, con la frequenza finale ("BEACON 10368.250 MHz"): il ricevitore lo mostra */
const char *pdx_cfg_provider(const pdx_cfg_t *c, uint32_t if_khz);
/* riga di telemetria per la sovrimpressione ("TEMP 41C  UP 3h12m  CYCLE 5"); NULL se disattivata */
const char *pdx_cfg_telemetry(const pdx_cfg_t *c, uint32_t cycle);
enum { PDX_CAMMODE_FULL, PDX_CAMMODE_PIP };

/* livello di uscita in dB TEORICI rispetto al massimo (ampiezza PDX_RF_AMP_MAX = 0 dB, -6 dB = metà ampiezza):
 * il valore 1..480 è l'ampiezza digitale del DAC; i dBm reali sul connettore richiedono una taratura misurata */
#define PDX_LEVEL_MIN_DB (-40.0)
int pdx_level_to_amp(double db);
double pdx_amp_to_level(int amp);
/* "-7.6", "-7.6dB" o "0dB" = livello in dB; un numero positivo senza "dB" = ampiezza 1..480; -1 se non valido */
int pdx_parse_amp(const char *s);

void pdx_cfg_defaults(pdx_cfg_t *c);
bool pdx_cfg_load(pdx_cfg_t *c);          /* false = nessuna configurazione salvata (restano i valori predefiniti) */
bool pdx_cfg_save(const pdx_cfg_t *c);
void pdx_cfg_print(const pdx_cfg_t *c);
/* SET <chiave> <valore>: restituisce NULL se riuscito, altrimenti il motivo */
const char *pdx_cfg_set(pdx_cfg_t *c, const char *key, const char *value);

/* ciclo del beacon fino allo stop: tx_abort è chiamata dentro i cicli di modulazione (deve leggere solo registri),
 * idle_abort nelle pause; restituisce il numero di cicli completati */
uint32_t pdx_beacon_run(const pdx_cfg_t *c, pdx_rf_abort_fn tx_abort, pdx_rf_abort_fn idle_abort);
