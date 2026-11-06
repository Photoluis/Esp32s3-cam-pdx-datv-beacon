/* ESP32-S3_PDX Parrot - IZ7PDX - GPL-3.0
 * Riquadro della risposta al QSO (P4, 06/11): l'immagine ricevuta (H.264: un'immagine IDR integra decodificata e ridotta
 * a 160x90) oppure la costellazione QPSK (H.265 o immagine non decodificabile). Conservato in PSRAM non inizializzata
 * (EXT_RAM_NOINIT_ATTR) con CRC: sopravvive al riavvio software tra la decodifica e la TX della risposta. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PDX_PIP_W 160
#define PDX_PIP_H 90

/* immagine dal TS ricevuto (pacchetti da 188 byte, ok[i] != 0 = CRC buono); false se nessuna IDR H.264 decodificabile.
 * info: testo breve per i log e per l'etichetta (es. "H.264 640x360") */
bool pdx_pip_from_ts(const uint8_t *ts, const uint8_t *ok, uint32_t np, char *info, size_t n);
/* costellazione dai simboli (I/Q x 8192 / |s| medio, vedi s2rx_env_t.cst) */
void pdx_pip_from_const(const int16_t *iq, uint32_t n, const char *label);
void pdx_pip_clear(void);
bool pdx_pip_valid(void);
int pdx_pip_w(void);                       /* dimensioni del riquadro (0 se assente) */
int pdx_pip_h(void);
const char *pdx_pip_label(void);          /* "RX H.264 640x360" o "RX H.265 QPSK"... */
/* copia il riquadro (con bordo bianco di 2 px) in un'immagine YUYV w x h, angolo in alto a sinistra dell'immagine x, y */
void pdx_pip_paste(uint8_t *yuyv, int w, int h, int x, int y);
