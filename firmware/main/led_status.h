/* Beacon DATV 13 cm - IZ7PDX
 * LED RGB della scheda (WS2812 su GPIO48, schema Freenove / diymore ESP32-S3-CAM): stato del beacon.
 *   blu tenue        configurazione, nessuna TX
 *   TX               colore "di calore" del livello: rosso scuro ai livelli bassi, poi arancio, giallo e bianco a 0 dB
 * Il colore si imposta prima della TX: il WS2812 lo mantiene da solo mentre il core 0 ha gli interrupt disattivati. */
#pragma once
#include <stdint.h>

#define PDX_LED_GPIO 48

void pdx_led_init(void);
void pdx_led_idle(void);
void pdx_led_tx(double level_db);
void pdx_led_rgb(uint8_t r, uint8_t g, uint8_t b);
