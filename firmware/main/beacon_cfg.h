/* Beacon DATV 13 cm - IZ7PDX
 * Configurazione del beacon salvata in NVS e ciclo autonomo (monoscopio / camera / pausa). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "rf_s3.h"

#define PDX_CFG_VERSION 3

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
} pdx_cfg_t;

enum { PDX_MONO_STD, PDX_MONO_USR_VIDEO, PDX_MONO_USR_IMAGE };
enum { PDX_CAMMODE_FULL, PDX_CAMMODE_PIP };

void pdx_cfg_defaults(pdx_cfg_t *c);
bool pdx_cfg_load(pdx_cfg_t *c);          /* false = nessuna configurazione salvata (restano i valori predefiniti) */
bool pdx_cfg_save(const pdx_cfg_t *c);
void pdx_cfg_print(const pdx_cfg_t *c);
/* SET <chiave> <valore>: restituisce NULL se riuscito, altrimenti il motivo */
const char *pdx_cfg_set(pdx_cfg_t *c, const char *key, const char *value);

/* ciclo del beacon fino allo stop: tx_abort è chiamata dentro i cicli di modulazione (deve leggere solo registri),
 * idle_abort nelle pause; restituisce il numero di cicli completati */
uint32_t pdx_beacon_run(const pdx_cfg_t *c, pdx_rf_abort_fn tx_abort, pdx_rf_abort_fn idle_abort);
