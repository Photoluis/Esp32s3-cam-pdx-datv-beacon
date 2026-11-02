/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Multiplexer MPEG-TS sull'ESP (fase C2): un programma, PAT/PMT/SDT, PCR su un PID dedicato, PES video e audio con
 * timestamp sempre crescenti, CBR al bitrate del MODCOD (pacchetti nulli). Sorgente: flussi elementari "PDXES001"
 * (tools/make_es.py) ripetuti in loop aggiungendo a ogni giro la durata del loop. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PDX_ES_OFFSET 0x600000u          /* file PDXES001 nella partizione "symbols" */

typedef struct {
    char magic[8];
    uint32_t nunits, loop_ticks, vtype, atype, data_off;
    char label[36];
} pdx_es_header_t;

typedef struct {
    uint8_t kind, rsv[3];                /* 0 video, 1 audio */
    uint32_t pts, off, len;
} pdx_es_unit_t;

typedef struct {
    const uint8_t *data;                 /* file mappato */
    const pdx_es_header_t *hdr;
    const pdx_es_unit_t *units;
    uint32_t loop;                       /* giri completati del loop di questo flusso */
    uint32_t next;                       /* prossima unità di questo tipo */
    /* PES in corso */
    uint8_t ph[14];
    uint32_t ph_len, ph_pos;
    const uint8_t *pl;
    uint32_t pl_rem;
    bool active;
} pdx_es_cursor_t;

#define PDX_VQ_LEN 4                     /* fotogrammi in coda (modo camera) */

typedef struct {
    uint8_t *buf;                        /* fornito dal chiamante, resta valido finché il PES non è stato inviato */
    uint32_t len;
    uint64_t pts;                        /* 90 kHz, assoluto */
} pdx_vq_entry_t;

typedef struct {
    double rate;                         /* bit/s del TS */
    uint8_t vtype;                       /* stream_type del video nel PMT: 0x24 H.265, 0x1B H.264 */
    uint16_t vpid;                       /* PID del video: diverso per codec, così il ricevitore crea un decoder nuovo */
    uint8_t pmt_ver;                     /* versione del PMT: cambia a ogni segmento */
    uint64_t t0_90k;                     /* tempo del primo pacchetto: prosegue da un segmento all'altro */
    bool vqueue;                         /* video dalla coda (camera) invece che dal file ES */
    pdx_vq_entry_t vq[PDX_VQ_LEN];
    volatile uint32_t vq_r, vq_w;
    bool vq_sending;
    uint64_t n;                          /* pacchetti emessi */
    uint64_t pts_base;                   /* 90 kHz: tempo di presentazione dell'inizio del primo giro */
    uint64_t next_psi, next_sdt, next_pcr;   /* in pacchetti */
    uint8_t cc[5];                       /* PAT, PMT, SDT, video, audio */
    uint8_t psi_pending;                 /* 1 = PMT da inviare dopo la PAT */
    char service[24], provider[40];
    const pdx_es_header_t *es;
    pdx_es_cursor_t v, a;
    uint32_t late;                       /* PES iniziati dopo il loro tempo di decodifica */
    uint32_t stats_null, stats_video, stats_audio;
} pdx_mux_t;

/* apre il file ES mappato (audio sempre dal file; video dal file oppure, con vqueue, dalla coda); NULL se riuscito */
const char *pdx_mux_init(pdx_mux_t *m, const uint8_t *es_file, double rate, const char *service, const char *provider,
                         bool vqueue);
uint64_t pdx_mux_now_90k(const pdx_mux_t *m);           /* tempo del multiplexer (pacchetti emessi) */
bool pdx_mux_vq_space(const pdx_mux_t *m);              /* c'è posto per un fotogramma nella coda */
pdx_vq_entry_t *pdx_mux_vq_slot(pdx_mux_t *m);          /* posto libero: il chiamante riempie buf, len, pts */
void pdx_mux_vq_commit(pdx_mux_t *m);                   /* mette in coda il fotogramma del posto libero */
void pdx_mux_next(pdx_mux_t *m, uint8_t pkt[188]);      /* prossimo pacchetto da 188 byte */
