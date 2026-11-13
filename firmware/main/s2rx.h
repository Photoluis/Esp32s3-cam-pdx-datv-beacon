/* ESP32-S3_PDX Parrot - IZ7PDX - GPL-3.0
 * Ricevitore DVB-S2 (QPSK, CCM, un TS) su una cattura I/Q int8 a 1 Msps (0,5 Msps per SR sotto 100 kS): porting in C di tools/parrot/s2_rx.py.
 * C portabile (compilato anche sul PC per il confronto con il prototipo): memoria dalle funzioni dell'ambiente. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *(*alloc)(size_t n);        /* memoria grande (PSRAM sull'S3) */
    void (*release)(void *p);
    void (*log)(const char *s);      /* facoltativo */
    uint32_t (*ms)(void);            /* facoltativo: millisecondi, per i tempi */
    void (*on_call)(const char *call);   /* facoltativo: nominativo letto dalla SDT durante la decodifica */
    int16_t *cst;                    /* facoltativo: simboli di un frame per la costellazione, I/Q x 8192 / |s| medio */
    uint32_t cst_max;                /* coppie I/Q che cst può contenere */
} s2rx_env_t;

typedef struct {
    bool detected;
    float peak_db, width_hz, freq_hz;
    uint32_t sr;
    int plframes, frames, ldpc_ok, bb_ok, ldpc_iter_sum;
    int modcod;                      /* MODCOD QPSK 1..11 */
    bool shortf, pilots;
    char fec[6];
    float mer_db, d_margin_db;
    uint32_t ts_packets, ts_crc_ok, ts_crc_bad;
    uint32_t cst_n;                  /* simboli messi in env->cst */
    char service[48], provider[48], call[16];
    uint8_t vtype, atype;            /* stream_type del video e dell'audio (PMT) */
    uint32_t ms_front, ms_sync, ms_frames, ms_total;
} s2rx_report_t;

/* iq: n campioni complessi int8 interlacciati (I, Q), già con lo spettro dritto. sr_hint: 0 = stimato.
 * ts_out (facoltativo): fino a ts_max pacchetti da 188 byte; ts_ok (facoltativo): esito del CRC di ogni pacchetto
 * (1 buono, 0 errato, 2 non verificabile). Restituisce il numero di pacchetti TS ricostruiti. */
uint32_t s2rx_run(const int8_t *iq, uint32_t n, float fs, uint32_t sr_hint, const s2rx_env_t *env,
                  s2rx_report_t *rep, uint8_t *ts_out, uint32_t ts_max, uint8_t *ts_ok);

/* controllo rapido (solo spettro) per l'ascolto: true se c'è un segnale largo almeno 50 kHz e almeno min_db sul rumore */
bool s2rx_detect(const int8_t *iq, uint32_t n, float fs, const s2rx_env_t *env, float min_db, float *center,
                 float *width, float *peak_db);

/* true se la cattura viene dal decimatore della scheda (RXSTREAM, PARROT): il rilevatore ne compensa la risposta;
 * false per catture caricate dal PC (RXLOAD) */
void s2rx_set_board_capture(bool on);

/* true per i segnali stretti (SR 40..100 kS) catturati con il LO spostato di 80 kHz sopra il segnale: il rilevatore
 * ignora +-35 kHz attorno al LO (gobba della radio), cerca il segnale solo da -150 a -20 kHz e s2rx_detect accetta
 * larghezze da 30 a 90 kHz */
void s2rx_set_narrow(bool on);
