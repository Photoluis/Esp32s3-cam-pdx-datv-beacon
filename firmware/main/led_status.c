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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
static SemaphoreHandle_t mtx;
static esp_timer_handle_t tmr;
static void ovl_end(void *arg);

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
    mtx = xSemaphoreCreateMutex();
    const esp_timer_create_args_t ta = {.callback = ovl_end, .name = "led"};
    if (esp_timer_create(&ta, &tmr) != ESP_OK) tmr = NULL;
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

/* ---- avvisi brevi sopra lo stato di attesa (06/11): giallo lento = parametri applicati / salvati,
 * verde veloce = attività sulla seriale o sul web. Poi si torna allo stato di base. Mai durante TX o RX. */
static int ovl_kind;                 /* 0 nessuno, 1 conferma, 2 attività */
static void lock(void) { if (mtx) xSemaphoreTake(mtx, portMAX_DELAY); }
static void unlock(void) { if (mtx) xSemaphoreGive(mtx); }

static void show_base(void) { if (last_kind) show_blink(); else show_fixed(); }

static void show_pattern(uint8_t r, uint8_t g, uint8_t b, uint32_t half_ms)
{
    apply_mode(&r, &g, &b);
    const uint32_t half = half_ms * (RES_HZ / 1000u);
    unsigned n = put_low(0, 750);
    n = put_rgb(n, r, g, b);
    n = put_low(n, half);
    n = put_rgb(n, 0, 0, 0);
    n = put_low(n, half);
    send(n, true);
}

static void ovl_end(void *arg)
{
    (void)arg;
    lock();
    if (ovl_kind) { ovl_kind = 0; show_base(); }
    unlock();
}

static void notify(int kind, uint32_t dur_ms)
{
    if (!chan || !mtx) return;
    lock();
    if (last_kind == 0 && last_rgb[1] != 128 && !(kind == 2 && ovl_kind == 1)) {   /* solo in attesa; la conferma vince */
        if (ovl_kind != kind) {
            if (kind == 1) show_pattern(128, 100, 0, 500);                /* giallo, 1 Hz */
            else show_pattern(0, 128, 0, 60);                            /* verde, ~8 Hz */
            ovl_kind = kind;
        }
        esp_timer_stop(tmr);
        esp_timer_start_once(tmr, (uint64_t)dur_ms * 1000u);
    }
    unlock();
}

void pdx_led_confirm(void) { notify(1, 3000); }
void pdx_led_activity(void) { notify(2, 400); }

static void set_base(void)                                /* stato nuovo: annulla l'avviso in corso */
{
    if (tmr) esp_timer_stop(tmr);
    ovl_kind = 0;
}

void pdx_led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    lock();
    set_base();
    last_rgb[0] = r; last_rgb[1] = g; last_rgb[2] = b;
    last_kind = 0;
    show_fixed();
    unlock();
}

void pdx_led_idle(void) { pdx_led_rgb(77, 60, 0); }                  /* attesa: giallo fisso al 30% (utente, 04/11) */
void pdx_led_rx(void) { pdx_led_rgb(0, 128, 0); }                     /* ricezione: verde fisso al 50% */

void pdx_led_tx(double level_db)
{
    /* TX: rosso al 50% che lampeggia, più veloce con più potenza: da 1 Hz a PDX_LED_DB_MIN fino a 5 Hz a PDX_LED_DB_MAX */
    double t = (level_db - PDX_LED_DB_MIN) / (PDX_LED_DB_MAX - PDX_LED_DB_MIN);
    t = t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    const double hz = 1.0 + 4.0 * t;
    lock();
    set_base();
    last_half_ms = (uint32_t)(500.0 / hz + 0.5);
    last_rgb[0] = 128; last_rgb[1] = 0; last_rgb[2] = 0;
    last_kind = 1;
    show_blink();
    unlock();
}

void pdx_led_set_mode(int mode)
{
    lock();
    led_mode = mode;
    set_base();
    show_base();
    unlock();
}
