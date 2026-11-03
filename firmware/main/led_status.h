/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * LED RGB della scheda (WS2812 su GPIO48, schema Freenove / diymore ESP32-S3-CAM): semaforo dello stato, per tutte le
 * funzioni del beacon (richiesta dell'utente del 04/11, al posto del colore "di calore"):
 *   RX        verde fisso al 50%
 *   attesa    giallo fisso al 30% (configurazione, pause del ciclo, decodifica)
 *   TX        rosso al 50% lampeggiante, da 1 Hz a PDX_LED_DB_MIN fino a 5 Hz a PDX_LED_DB_MAX
 * Modi: normale, minimo (stessi colori alla luminosità più bassa visibile), spento.
 * Il lampeggio è un ciclo hardware dell'RMT: continua da solo mentre il core 0 ha gli interrupt disattivati. */
#pragma once
#include <stdint.h>

#define PDX_LED_GPIO 48
#define PDX_LED_DB_MIN (-40.0)       /* scala del livello del beacon (cursore della pagina web) */
#define PDX_LED_DB_MAX (-5.0)

enum { PDX_LED_NORMAL, PDX_LED_MIN, PDX_LED_OFF };   /* minimo / spento: per alimentazione a batteria e pannello */

void pdx_led_init(void);
void pdx_led_set_mode(int mode);                    /* riapplica subito l'ultimo stato */
void pdx_led_idle(void);                            /* giallo fisso */
void pdx_led_rx(void);                              /* verde fisso */
void pdx_led_tx(double level_db);                   /* rosso lampeggiante */
void pdx_led_rgb(uint8_t r, uint8_t g, uint8_t b);  /* colore fisso qualsiasi */
