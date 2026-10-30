/* Beacon DATV 13 cm - IZ7PDX
 * Trasmettitore Wi-Fi dell'ESP32-S3 usato come modulatore I/Q (porting del metodo di SP8ESA, scritto da zero). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PDX_RF_FMIN_KHZ 2300000u
#define PDX_RF_FMAX_KHZ 2450000u
#define PDX_RF_AMP_MAX  480          /* codici DAC (10 bit con segno); oltre ~430 l'uscita comprime */

/* parola DAC: I nei bit 0..9, Q nei bit 10..19, complemento a due */
static inline uint32_t pdx_rf_dac_word(int i, int q) { return ((uint32_t)i & 0x3FFu) | (((uint32_t)q & 0x3FFu) << 10); }

void pdx_rf_init(void);                       /* Wi-Fi in modo NULL: accende e calibra la PHY */
void pdx_rf_wifi_null(void);                  /* rimette il Wi-Fi nello stato del modulatore (dopo l'access point) */
uint32_t pdx_rf_rx_frames(void);              /* diagnosi: trame Wi-Fi ricevute in modo promiscuo dall'avvio */
bool pdx_rf_tune(uint32_t fkhz);              /* PLL sulla frequenza (2300..2450 MHz) */
double pdx_rf_lo_hz(void);

/* TX: la funzione abort() viene chiamata in polling (ogni 1/4 s) e ferma la trasmissione se restituisce true;
 * deve essere in IRAM e leggere solo registri: il suo tempo di esecuzione non viene recuperato dall'orologio */
typedef bool (*pdx_rf_abort_fn)(void);
uint32_t pdx_rf_tx_cw(int amp, uint32_t ms, pdx_rf_abort_fn abort_fn);                    /* parola fissa: portante sull'LO */
/* modo del tono (diagnosi): bit 0 = TX spenta e riaccesa ogni secondo (come RF0001), bit 1 = senza misura dei ritardi */
#define PDX_RF_MODE_CHUNK_1S 1u
#define PDX_RF_MODE_NO_STATS 2u
uint32_t pdx_rf_tx_tone(int amp, int32_t offset_hz, uint32_t fs_hz, uint32_t ms, uint32_t mode,
                        pdx_rf_abort_fn abort_fn);   /* tono a LO + offset */

/* QPSK con filtro RRC (roll-off 0.35, 8 simboli), simboli casuali; fs = SR * campioni per simbolo (4..16, ~2 MS/s) */
#define PDX_RF_SR_MIN 33000u
#define PDX_RF_SR_MAX 1000000u
uint32_t pdx_rf_qpsk_sps(uint32_t sr);
/* spettro invertito nelle TX QPSK successive (compensa un mixer esterno con oscillatore locale sopra la IF) */
void pdx_rf_set_invert(bool on);
uint32_t pdx_rf_tx_qpsk(int amp, uint32_t sr, int dci, int dcq, uint32_t ms, pdx_rf_abort_fn abort_fn);

/* buffer circolare di simboli (4 per byte), riempito da un altro core; size potenza di 2, w e r contatori liberi */
typedef struct {
    volatile uint8_t *buf;
    uint32_t size;
    volatile uint32_t w, r;
} pdx_rf_ring_t;
/* diagnosi: fase del produttore sul core 1 (0..7) e campioni in ritardo contati per fase */
extern volatile uint32_t pdx_diag_phase;
extern uint32_t pdx_diag_late[8];
uint32_t pdx_rf_tx_qpsk_ring(int amp, uint32_t sr, int dci, int dcq, uint32_t ms, pdx_rf_ring_t *rb, pdx_rf_abort_fn abort_fn);

/* statistiche dell'ultima TX campionata: ritardo massimo delle scritture nel DAC, campioni in ritardo (> 1/4 periodo),
 * byte mancanti nel buffer dei simboli */
typedef struct {
    uint32_t fs_hz, period, samples, lag_max, late, underruns;
    uint32_t bytes, sum1, sum2;     /* byte di simboli consumati dal modulatore e loro checksum di Fletcher */
} pdx_rf_stats_t;
const pdx_rf_stats_t *pdx_rf_last_stats(void);
