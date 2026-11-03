/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * LED RGB WS2812 con la periferica RMT, a semaforo (vedi led_status.h).
 * Il lampeggio della TX è fatto dall'RMT in ciclo hardware continuo (loop_count = -1): la sequenza
 * "pausa di reset, rosso, pausa, spento, pausa" sta tutta nella memoria del canale e si ripete senza CPU, quindi
 * continua anche con gli interrupt del core 0 disattivati durante la TX. */
#include <stdint.h>
#include <string.h>
#include "led_status.h"

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

#define RES_HZ   2500000u            /* 0,4 us per tick: WS2812 bit 0 = 1+2 tick, bit 1 = 2+1 tick (valori nominali) */
#define MEM_SYM  96u                 /* due blocchi da 48 simboli: tutta la sequenza del lampeggio */
#define LOW_MAX  32767u              /* durata massima di metà simbolo, in tick */

static rmt_channel_handle_t chan;
static rmt_encoder_handle_t enc;
static int led_mode;
static rmt_symbol_word_t pat[MEM_SYM];
static int last_kind;                /* 0 fisso, 1 lampeggio */
static uint8_t last_rgb[3];
static uint32_t last_half_ms;

void pdx_led_init(void)
{
    if (chan) return;
    const rmt_tx_channel_config_t cc = {
        .gpio_num = PDX_LED_GPIO, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = RES_HZ,
        .mem_block_symbols = MEM_SYM, .trans_queue_depth = 2,
    };
    const rmt_copy_encoder_config_t ec = {};
    if (rmt_new_tx_channel(&cc, &chan) != ESP_OK || rmt_new_copy_encoder(&ec, &enc) != ESP_OK ||
        rmt_enable(chan) != ESP_OK) {
        chan = NULL;
        return;
    }
    pdx_led_idle();
}

/* colore con il modo applicato (minimo: canale più forte a 2 su 255; spento: nero) */
static void apply_mode(uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (led_mode == PDX_LED_OFF) {
        *r = *g = *b = 0;
    } else if (led_mode == PDX_LED_MIN) {
        const int mx = *r > *g ? (*r > *b ? *r : *b) : (*g > *b ? *g : *b);
        if (mx > 2) {
            *r = (uint8_t)((*r * 2 + mx / 2) / mx);
            *g = (uint8_t)((*g * 2 + mx / 2) / mx);
            *b = (uint8_t)((*b * 2 + mx / 2) / mx);
        }
    }
}

static unsigned put_low(unsigned n, uint32_t ticks)                 /* pausa a livello basso, a pezzi da 2 x 32767 */
{
    while (ticks && n < MEM_SYM) {
        const uint32_t a = ticks > LOW_MAX ? LOW_MAX : ticks;
        ticks -= a;
        const uint32_t b = ticks > LOW_MAX ? LOW_MAX : ticks;
        ticks -= b;
        pat[n++] = (rmt_symbol_word_t){.level0 = 0, .duration0 = a, .level1 = 0, .duration1 = b ? b : 1};
    }
    return n;
}

static unsigned put_rgb(unsigned n, uint8_t r, uint8_t g, uint8_t b)  /* 24 bit, ordine G R B, MSB per primo */
{
    const uint32_t v = (uint32_t)g << 16 | (uint32_t)r << 8 | b;
    for (int i = 23; i >= 0 && n < MEM_SYM; i--)
        pat[n++] = (v >> i) & 1u ? (rmt_symbol_word_t){.level0 = 1, .duration0 = 2, .level1 = 0, .duration1 = 1}
                                 : (rmt_symbol_word_t){.level0 = 1, .duration0 = 1, .level1 = 0, .duration1 = 2};
    return n;
}

/* ferma un eventuale ciclo in corso e invia la nuova sequenza */
static void send(unsigned n, bool loop)
{
    if (!chan || !n) return;
    rmt_disable(chan);                                   /* interrompe anche il ciclo infinito */
    rmt_enable(chan);
    const rmt_transmit_config_t tc = {.loop_count = loop ? -1 : 0};
    rmt_transmit(chan, enc, pat, n * sizeof(rmt_symbol_word_t), &tc);
    if (!loop) rmt_tx_wait_all_done(chan, 50);           /* il LED deve essere aggiornato prima della TX */
}

static void show_fixed(void)
{
    uint8_t r = last_rgb[0], g = last_rgb[1], b = last_rgb[2];
    apply_mode(&r, &g, &b);
    unsigned n = put_low(0, 750);                        /* 300 us di reset */
    n = put_rgb(n, r, g, b);
    n = put_low(n, 750);
    send(n, false);
}

static void show_blink(void)
{
    uint8_t r = last_rgb[0], g = last_rgb[1], b = last_rgb[2];
    apply_mode(&r, &g, &b);
    const uint32_t half = last_half_ms * (RES_HZ / 1000u);
    unsigned n = put_low(0, 750);
    n = put_rgb(n, r, g, b);                             /* acceso */
    n = put_low(n, half);
    n = put_rgb(n, 0, 0, 0);                             /* spento */
    n = put_low(n, half);
    send(n, true);
}

void pdx_led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    last_rgb[0] = r; last_rgb[1] = g; last_rgb[2] = b;
    last_kind = 0;
    show_fixed();
}

void pdx_led_idle(void) { pdx_led_rgb(77, 60, 0); }                  /* attesa: giallo fisso al 30% (utente, 04/11) */
void pdx_led_rx(void) { pdx_led_rgb(0, 128, 0); }                     /* ricezione: verde fisso al 50% */

void pdx_led_tx(double level_db)
{
    /* TX: rosso al 50% che lampeggia, più veloce con più potenza: da 1 Hz a PDX_LED_DB_MIN fino a 5 Hz a PDX_LED_DB_MAX */
    double t = (level_db - PDX_LED_DB_MIN) / (PDX_LED_DB_MAX - PDX_LED_DB_MIN);
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    const double hz = 1.0 + 4.0 * t;
    last_half_ms = (uint32_t)(500.0 / hz + 0.5);
    last_rgb[0] = 128; last_rgb[1] = 0; last_rgb[2] = 0;
    last_kind = 1;
    show_blink();
}

void pdx_led_set_mode(int mode)
{
    led_mode = mode;
    if (last_kind) show_blink(); else show_fixed();
}
