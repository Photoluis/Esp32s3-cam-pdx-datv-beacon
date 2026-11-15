/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Suono generato per il TS: tono continuo o identificazione CW (QRZ in Morse), messi in fila da frame AAC precodificati
 * (tools/make_audio_frames.py -> audio_frames.h). Niente encoder sull'ESP: solo la scelta del frame. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* suono nel TS (valori salvati in NVS: non riordinare) */
enum { PDX_AUDIO_OFF, PDX_AUDIO_TONE, PDX_AUDIO_CW, PDX_AUDIO_FILE, PDX_AUDIO_N };
const char *pdx_audio_name(uint8_t mode);    /* "off", "tone", "cw", "file" */

#define PDX_AGEN_HZ      48000u
#define PDX_AGEN_FRAME   1024u               /* campioni per frame AAC = 1920 tick a 90 kHz */
#define PDX_AGEN_PERIOD  1406u               /* CW: un giro ogni 30 s (1406 frame = 29,99 s) */
#define PDX_AGEN_FIRST   47u                 /* CW: 1 s di silenzio all'inizio di ogni giro */

typedef struct {
    uint8_t mode;                            /* PDX_AUDIO_TONE o PDX_AUDIO_CW */
    uint8_t seq[PDX_AGEN_PERIOD];            /* CW: blocco e frame nel blocco per ogni frame del giro */
    uint32_t cw_frames;                      /* CW: frame con la telegrafia (dopo il silenzio iniziale) */
    char text[32];                           /* CW: testo trasmesso */
} pdx_agen_t;

/* text: QRZ e locator (il locator viene tolto se la telegrafia supera 2/3 del giro, come in ESP32-ATV) */
void pdx_agen_init(pdx_agen_t *g, uint8_t mode, const char *call, const char *locator);
/* frame k dall'inizio del suono (k = 0 è il primo frame del segmento) */
const uint8_t *pdx_agen_frame(const pdx_agen_t *g, uint32_t k, uint32_t *len);
/* bit/s nel TS con fpp frame per PES (intestazione PES e riempimento dell'ultimo pacchetto compresi) */
double pdx_agen_bps(const pdx_agen_t *g, uint32_t fpp);
