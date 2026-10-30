/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * LED RGB della scheda (WS2812 su GPIO48, schema Freenove / diymore ESP32-S3-CAM): stato del beacon.
 *   blu tenue        configurazione, nessuna TX
 *   TX               colore "di calore" del livello: rosso scuro ai livelli bassi, poi arancio, giallo e bianco a 0 dB
 * Modi: normale, minimo (stessi colori alla luminosità più bassa visibile), spento.
 * Il colore si imposta prima della TX: il WS2812 lo mantiene da solo mentre il core 0 ha gli interrupt disattivati. */
#pragma once
#include <stdint.h>

#define PDX_LED_GPIO 48

enum { PDX_LED_NORMAL, PDX_LED_MIN, PDX_LED_OFF };   /* minimo / spento: per alimentazione a batteria e pannello */

void pdx_led_init(void);
void pdx_led_set_mode(int mode);                    /* riapplica subito l'ultimo colore */
void pdx_led_idle(void);
void pdx_led_tx(double level_db);
void pdx_led_rgb(uint8_t r, uint8_t g, uint8_t b);
