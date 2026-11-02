/* ESP32-S3_PDX Parrot - IZ7PDX - GPL-3.0
 * Ricezione con il motore di cattura dell'ESP32-S3 (P1a: cattura grezza a 16 Msps in un banco di SRAM). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PDX_RX_PAIRS     16384u   /* un banco: 64 KB, 16384 coppie I/Q = 1,024 ms a 16 Msps */
#define PDX_RX_FS_HZ     16000000u
#define PDX_RX_GAIN_DEF  60u      /* selettore di guadagno (indice di tabella, circa 1 dB a passo tra 35 e 76) */

/* sintonizza il PLL in ricezione (lo_hz) e configura la catena RX con il guadagno dato; NULL se riuscito, altrimenti il
 * motivo. Dopo la ricezione la radio va riportata allo stato del modulatore con un riavvio. */
const char *pdx_rx_start(uint32_t lo_hz, unsigned gain);
/* frequenza effettiva del LO (passi di ~458 Hz) */
uint32_t pdx_rx_lo_hz(void);
/* una cattura di n coppie (<= PDX_RX_PAIRS) nel banco 0x3FCD0000: parole a 32 bit, I = bit 0..9, Q = bit 10..19 con
 * segno; restituisce il numero di coppie scritte dal motore, 0 se non ha scritto nulla.
 * ATTENZIONE: lo spettro è invertito (prova del 02/11): il segnale vero è I - jQ. */
uint32_t pdx_rx_capture(uint32_t n, const uint32_t **data);
/* P1b: cattura continua con decimazione in tempo reale sul core 0 (interruzioni sospese).
 * Filtro CIC di ordine `order` (2 o 3) e decimazione `decim` (8 o 16: 2 o 1 Msps), uscita int8 I/Q interlacciata già
 * coniugata (spettro dritto), valore >> shift con saturazione. out: n_out campioni complessi (2 byte ciascuno). */
typedef struct {
    uint32_t pairs_in, samples_out, overruns, cycles, clipped, max_lag;
    int32_t dc_i, dc_q;           /* continua misurata prima della cattura, 1/1024 di conteggio */
    float rms;                    /* valore efficace dell'uscita, conteggi int8 */
} pdx_rx_stream_stats_t;
const char *pdx_rx_stream(int8_t *out, uint32_t n_out, unsigned decim, unsigned order, unsigned shift,
                          pdx_rx_stream_stats_t *st);
/* guadagni RF e banda base scelti dal selettore (diagnostica) */
void pdx_rx_gains(unsigned *rf, unsigned *bb);
