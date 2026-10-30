/* Beacon DATV 13 cm - IZ7PDX
 * TX con TS generato sull'ESP (fase C2): multiplexer (ts_mux) -> DVB-S2 (dvbs2_enc) sul core 1 -> buffer circolare ->
 * modulatore QPSK RRC sul core 0. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rf_s3.h"

typedef struct {
    uint32_t sr;
    const char *fec;                     /* "3/4" ... */
    bool shortf, pilots;
    int amp, dci, dcq;
    const char *service, *provider;
    bool camera;                         /* video dalla camera (H.264) invece del monoscopio del file ES */
    int cam_fps, cam_kbps;
    uint32_t freq_khz;                   /* per la sovrimpressione */
} pdx_live_cfg_t;

/* file ES mappato (NULL se la partizione manca) */
const uint8_t *pdx_live_es_data(void);
/* informazioni sul file ES nella partizione; false se assente */
bool pdx_live_es_info(char *label, size_t n, uint32_t *units, double *loop_s);
/* diagnosi senza RF: pacchetti TS del modo camera stampati in esadecimale (righe "T <hex>") */
const char *pdx_live_cam_dump(const pdx_live_cfg_t *c, uint32_t npk);
/* trasmette per ms millisecondi (stop con abort_fn); restituisce i ms trasmessi, why riceve un eventuale errore */
uint32_t pdx_live_play(const pdx_live_cfg_t *c, uint32_t ms, pdx_rf_abort_fn abort_fn, const char **why);
