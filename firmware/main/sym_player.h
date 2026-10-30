/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Lettore dei simboli preparati sul PC (partizione "symbols"). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "rf_s3.h"

/* intestazione del file di simboli (64 byte, little endian), seguita dai simboli: 4 per byte */
#define PDX_SYM_MAGIC "PDXSYM01"
typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t sr;          /* symbol rate, S/s */
    uint32_t nsym;        /* simboli nel loop (multiplo di 4) */
    uint32_t crc32;       /* CRC-32 (zlib) dei nsym/4 byte di simboli */
    uint32_t flags;       /* riservato */
    char label[40];       /* descrizione, terminata da 0 */
} pdx_sym_header_t;

/* apre la partizione e controlla intestazione e CRC; why riceve il motivo se fallisce */
bool pdx_sym_open(pdx_sym_header_t *hdr, const char **why);
/* buffer circolare dei simboli (anche per live_tx.c) */
pdx_rf_ring_t *pdx_sym_ring(void);
/* trasmette il loop per ms millisecondi con il modulatore QPSK RRC */
uint32_t pdx_sym_play(int amp, int dci, int dcq, uint32_t ms, pdx_rf_abort_fn abort_fn);
