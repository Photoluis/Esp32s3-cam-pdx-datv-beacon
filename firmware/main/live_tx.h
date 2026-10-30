/* Beacon DATV 13 cm - IZ7PDX
 * TX con TS generato sull'ESP: multiplexer (ts_mux) -> DVB-S2 (dvbs2_enc) sul core 1 -> buffer circolare ->
 * modulatore QPSK RRC sul core 0. Video dal file ES (monoscopio precodificato) oppure H.264 generato dal vivo
 * (camera, camera in PiP, immagine fissa); l'audio viene sempre dal file ES. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rf_s3.h"

typedef enum { PDX_LV_ES, PDX_LV_CAM, PDX_LV_PIP, PDX_LV_IMG } pdx_live_video_t;

typedef struct {
    uint32_t sr;
    const char *fec;                     /* "3/4" ... */
    bool shortf, pilots;
    int amp, dci, dcq;
    const char *service, *provider;
    pdx_live_video_t video;
    const uint8_t *es;                   /* file PDXES001: audio sempre, video con PDX_LV_ES */
    const uint8_t *bg;                   /* immagine PDXIMG01 per PiP e immagine fissa */
    int cam_fps, cam_kbps;               /* 0 = automatico (kbit/s dal bitrate del TS) */
    bool ovl_full;                       /* sovrimpressione con QRZ e frequenza (altrimenti solo la fascia in basso) */
    uint64_t freq_khz;                   /* frequenza finale in aria, per la sovrimpressione */
} pdx_live_cfg_t;

const char *pdx_live_video_name(pdx_live_video_t v);
/* diagnosi senza RF: pacchetti TS di un modo dal vivo stampati in esadecimale (righe "T <hex>") */
const char *pdx_live_dump(const pdx_live_cfg_t *c, uint32_t npk);
/* trasmette per ms millisecondi (stop con abort_fn); restituisce i ms trasmessi, why riceve un eventuale errore */
uint32_t pdx_live_play(const pdx_live_cfg_t *c, uint32_t ms, pdx_rf_abort_fn abort_fn, const char **why);
