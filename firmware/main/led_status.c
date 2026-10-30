/* Beacon DATV 13 cm - IZ7PDX
 * LED RGB WS2812 con la periferica RMT (vedi led_status.h). */
#include <stdint.h>
#include "led_status.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

static rmt_channel_handle_t chan;
static rmt_encoder_handle_t enc;

void pdx_led_init(void)
{
    if (chan) return;
    const rmt_tx_channel_config_t cc = {
        .gpio_num = PDX_LED_GPIO, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10000000,
        .mem_block_symbols = 64, .trans_queue_depth = 2,
    };
    /* tempi WS2812 a 10 MHz: bit 0 = 0,3 us alto + 0,9 us basso, bit 1 = 0,9 + 0,3 us */
    const rmt_bytes_encoder_config_t bc = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
        .flags.msb_first = 1,
    };
    if (rmt_new_tx_channel(&cc, &chan) != ESP_OK || rmt_new_bytes_encoder(&bc, &enc) != ESP_OK ||
        rmt_enable(chan) != ESP_OK) {
        chan = NULL;
        return;
    }
    pdx_led_idle();
}

void pdx_led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!chan) return;
    static uint8_t grb[3];
    grb[0] = g; grb[1] = r; grb[2] = b;
    const rmt_transmit_config_t tc = {.loop_count = 0};
    rmt_transmit(chan, enc, grb, sizeof(grb), &tc);
    rmt_tx_wait_all_done(chan, 50);                       /* il LED deve essere aggiornato prima della TX */
}

void pdx_led_idle(void) { pdx_led_rgb(0, 0, 10); }

void pdx_led_tx(double level_db)
{
    /* da -30 dB (rosso scuro) a 0 dB (bianco), passando per arancio e giallo; più potenza = più luce */
    double t = (level_db + 30.0) / 30.0;
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    static const uint8_t stops[4][3] = {{255, 0, 0}, {255, 70, 0}, {255, 170, 10}, {255, 255, 255}};
    const double x = t * 3.0;
    const int i = x >= 3.0 ? 2 : (int)x;
    const double f = x - i;
    const double bright = 0.10 + 0.40 * t;                /* il WS2812 a piena luce abbaglia */
    uint8_t c[3];
    for (int k = 0; k < 3; k++) c[k] = (uint8_t)((stops[i][k] + (stops[i + 1][k] - stops[i][k]) * f) * bright + 0.5);
    pdx_led_rgb(c[0], c[1], c[2]);
}
