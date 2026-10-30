/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Codificatore DVB-S2 sull'ESP32-S3 (QPSK, CCM, un solo TS): porting in C di tools/dvbs2/encoder.py, secondo
 * ETSI EN 302 307-1 (5.1 mode adaptation, 5.2 BB scrambling, 5.3 BCH + LDPC, 5.4.1 QPSK, 5.5 PL framing). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const void *tab;           /* pdx_s2_table_t */
    bool shortf, pilots;
    uint32_t kbch, dfl, nldpc, kldpc, nk;   /* bit */
    uint32_t slots, nsym;      /* slot da 90 simboli del codice; simboli per PLFRAME (header compreso) */
    uint32_t deg;              /* grado BCH */
    uint32_t (*bch_tab)[6];    /* 256 voci, resto di grado deg allineato in alto in 192 bit */
    uint8_t hdr[90];           /* PLHEADER: codici a 2 bit (bit 0 = I, bit 1 = Q, 1 = +1) */
    uint8_t *prbs;             /* sequenza del BB scrambler, kbch bit */
    uint8_t *rot;              /* scrambling PL R(i), 4 per byte, nsym - 90 valori */
    uint8_t *cw;               /* parola di codice: kldpc bit di informazione + nk di parità */
    uint32_t (*ldpc_rows)[12]; /* parità LDPC in q righe da 360 bit (lavoro) */
    uint32_t t_bch_us, t_ldpc_us, t_map_us;  /* tempi dell'ultimo frame */
} pdx_s2_t;

/* rate "1/4" ... "9/10"; restituisce NULL se riuscito, altrimenti il motivo */
const char *pdx_s2_init(pdx_s2_t *s, const char *rate, bool shortf, bool pilots);
void pdx_s2_free(pdx_s2_t *s);
double pdx_s2_ts_bitrate(const pdx_s2_t *s, uint32_t sr);   /* bitrate TS utile, bit/s */

/* scrittore di simboli a 2 bit, 4 per byte, primo simbolo nei bit bassi (formato del lettore) */
typedef struct {
    uint8_t *buf;
    uint32_t pos;              /* simboli scritti */
} pdx_s2_out_t;

/* un PLFRAME dal DATA FIELD (dfl/8 byte, già con i CRC-8 al posto dei sync byte) e dal SYNCD */
void pdx_s2_frame(pdx_s2_t *s, const uint8_t *data_field, uint16_t syncd, pdx_s2_out_t *out);

/* mode adaptation (5.1.4): sostituisce il sync byte di ogni pacchetto TS con il CRC-8 del precedente */
uint8_t pdx_s2_crc8(const uint8_t *p, uint32_t n);
