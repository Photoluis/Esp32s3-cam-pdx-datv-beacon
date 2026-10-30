/* Beacon DATV 13 cm - IZ7PDX
 * Accesso al DAC I/Q del blocco RF dell'ESP32-S3.
 *
 * Indirizzi ricavati dalla libreria Espressif librftest.a (esp32s3, funzioni dactrig/adctrig) e confrontati con quelli
 * del C3 usati da ESP32-DATV-SA (SP8ESA):
 *   controllo del DAC replay   0x60033D64  (uguale al C3)
 *   banco dati del DAC         0x3FCD0000  (C3: 0x3FCB0000)
 *   assegnazione del banco     0x600C101C  SENSITIVE_INTERNAL_SRAM_USAGE_3_REG, campo MAC_DUMP_USAGE [3:0] = 4
 *                              (C3: 0x600C1020, valore 2 + bit 3)
 *   TX forzato                 g_phyFuns[47]  (C3: g_phyFuns[50], stessa funzione chiamata da force_txtone)
 */
#include "rf_s3.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include "esp_cpu.h"
#include "esp_event.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heap_memory_layout.h"
#include "nvs_flash.h"

/* il blocco RF prende tutto il banco: niente heap né dati statici dentro */
SOC_RESERVE_MEMORY_REGION(0x3fcc0000, 0x3fce0000, s3_rf_dump);

#define DAC_CTRL   0x60033D64u
#define DAC_BUF    ((volatile uint32_t *)0x3fcd0000)
#define SRAM_OWNER 0x600c101cu
#define CPU_HZ     240000000u

extern void stop_tx_tone(unsigned);
extern void rom_pbus_workmode(void);
extern void rom_pbus_xpd_rx_on(unsigned);
extern void rom_pbus_xpd_tx_off(void);
extern void rom_set_rxclk_en(unsigned);
extern void set_chanfreq(unsigned, unsigned);
extern void phy_set_freq(unsigned, int);
extern void force_rx_gain(unsigned, unsigned, unsigned);
extern void **g_phyFuns;
extern void txcal_work_mode(void);

static double lo_hz;
static portMUX_TYPE tx_mux = portMUX_INITIALIZER_UNLOCKED;

static inline uint32_t rd(uint32_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uint32_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static void phy_txforce(int on) { ((void (*)(int))g_phyFuns[47])(on); }
static unsigned chan_mhz(unsigned ch) { return ch == 14 ? 2484 : 2407 + 5 * ch; }

void pdx_rf_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();      /* per l'access point di configurazione (web_ui.c) */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    pdx_rf_wifi_null();
}

/* diagnosi: trame Wi-Fi ricevute in modo promiscuo (il ricevitore della radio resta acceso anche durante la TX) */
static volatile uint32_t rx_frames;
static void promisc_rx(void *buf, wifi_promiscuous_pkt_type_t type) { rx_frames++; }
uint32_t pdx_rf_rx_frames(void) { return rx_frames; }

/* Wi-Fi nello stato del modulatore: modo NULL (nessuna trama trasmessa dal driver), promiscuo, canale 1, PLL su 2412 */
void pdx_rf_wifi_null(void)
{
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promisc_rx));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    pdx_rf_tune(2412000);  /* la PHY vuole un canale valido prima del primo TX */
}

/* stesso calcolo intero di rfpll_set_freq (quarzo 40 MHz), da verificare sull'S3 */
static double pll_hz(unsigned mhz, int khz)
{
    const int32_t d = 120000;
    int32_t a = 4 * (int32_t)(1000 * mhz + khz) - 32 * d;
    int32_t o0 = a / d;
    a -= o0 * d;
    a <<= 8;
    int32_t o1 = a / d;
    a -= o1 * d;
    a <<= 8;
    int32_t o2 = a / d;
    return 30e6 * ((o0 & 255) + 32 + ((o1 & 255) * 256 + (o2 & 255)) / 65536.0);
}

static void rx_idle(void)
{
    rom_pbus_workmode();
    rom_pbus_xpd_tx_off();
    rom_pbus_xpd_rx_on(1);
    rom_set_rxclk_en(1);
    force_rx_gain(0, 40, 0);
}

bool pdx_rf_tune(uint32_t fkhz)
{
    if (fkhz < PDX_RF_FMIN_KHZ - 100000u || fkhz > PDX_RF_FMAX_KHZ + 50000u) return false;  /* 2412 serve all'avvio */
    int ch = ((int)fkhz - 2407000 + 2500) / 5000;
    ch = ch < 1 ? 1 : ch > 13 ? 13 : ch;
    unsigned mhz = fkhz / 1000;
    int khz = (int)(fkhz % 1000);
    set_chanfreq(chan_mhz(ch), 0);
    if (fkhz != chan_mhz(ch) * 1000u) phy_set_freq(mhz, khz);
    stop_tx_tone(1);
    rx_idle();
    lo_hz = pll_hz(mhz, khz);
    return true;
}

double pdx_rf_lo_hz(void) { return lo_hz; }

static void tx_begin(uint32_t *owner0)
{
    *owner0 = rd(SRAM_OWNER);
    phy_txforce(1);
    esp_rom_delay_us(3000);
    DAC_BUF[0] = 0;
    taskENTER_CRITICAL(&tx_mux);
    wr(SRAM_OWNER, (*owner0 & ~0xFu) | 4u);
    __asm__ volatile("memw" ::: "memory");
    wr(DAC_CTRL, rd(DAC_CTRL) & ~(1u << 31));
    wr(DAC_CTRL, 0x80000u | 1u);               /* una sola parola, ripetuta: il registro DAC "tenuto" */
    wr(DAC_CTRL, 0x80000u | 1u | (1u << 31));
}

static void tx_end(uint32_t owner0)
{
    DAC_BUF[0] = 0;
    wr(DAC_CTRL, rd(DAC_CTRL) & ~(1u << 31));
    wr(SRAM_OWNER, owner0);
    taskEXIT_CRITICAL(&tx_mux);
    phy_txforce(0);
    txcal_work_mode();
    rx_idle();
}

uint32_t pdx_rf_tx_cw(int amp, uint32_t ms, pdx_rf_abort_fn abort_fn)
{
    uint32_t owner0;
    if (amp > PDX_RF_AMP_MAX) amp = PDX_RF_AMP_MAX;
    tx_begin(&owner0);
    DAC_BUF[0] = pdx_rf_dac_word(amp, 0);
    taskEXIT_CRITICAL(&tx_mux);                /* parola fissa: nessuna scadenza, il sistema può girare */
    const int64_t t0 = esp_timer_get_time();
    uint32_t elapsed_ms = 0;
    while (elapsed_ms < ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        if (abort_fn && abort_fn()) break;
    }
    taskENTER_CRITICAL(&tx_mux);
    tx_end(owner0);
    return elapsed_ms;
}

/* tono complesso: I = A cos, Q = A sin, con scadenze sul contatore di cicli; ciclo in IRAM (niente cache della flash) */
#define TONE_LUT  1024u
static DRAM_ATTR int16_t tone_cos[TONE_LUT];
static pdx_rf_stats_t last_stats;

const pdx_rf_stats_t *pdx_rf_last_stats(void) { return &last_stats; }

/* noinline: senza, il compilatore inserisce il ciclo nella funzione chiamante (in flash) e IRAM_ATTR si perde: il ciclo
 * girava dalla cache della flash e ogni attività del core 1 sulla cache ritardava i campioni (30/10) */
static IRAM_ATTR __attribute__((noinline)) uint32_t tone_loop(uint32_t step, uint32_t period, uint32_t n, uint32_t check_every, pdx_rf_abort_fn abort_fn,
                                    bool measure, pdx_rf_stats_t *st)
{
    uint32_t ph = 0, s = 0, next_check = check_every, lag_max = 0, late = 0;
    uint32_t tn = esp_cpu_get_cycle_count() + 1000u;
    for (; s < n; s++) {
        const uint32_t idx = ph >> 22;
        const uint32_t w = pdx_rf_dac_word(tone_cos[idx], tone_cos[(idx - TONE_LUT / 4u) & (TONE_LUT - 1u)]);
        ph += step;
        while ((int32_t)(esp_cpu_get_cycle_count() - tn) < 0) {}
        DAC_BUF[0] = w;
        if (measure) {
            const uint32_t lag = esp_cpu_get_cycle_count() - tn;   /* cicli di ritardo della scrittura */
            if (lag > lag_max) lag_max = lag;
            if (lag > period / 4u) late++;
        }
        tn += period;
        if (s == next_check) {                                  /* stop richiesto: una lettura ogni check_every campioni */
            next_check += check_every;
            if (abort_fn && abort_fn()) { s++; break; }
        }
    }
    st->samples = s;
    st->lag_max = lag_max;
    st->late = late;
    return s;
}

uint32_t pdx_rf_tx_tone(int amp, int32_t offset_hz, uint32_t fs_hz, uint32_t ms, uint32_t mode, pdx_rf_abort_fn abort_fn)
{
    uint32_t owner0;
    if (amp > PDX_RF_AMP_MAX) amp = PDX_RF_AMP_MAX;
    for (unsigned k = 0; k < TONE_LUT; k++) tone_cos[k] = (int16_t)lroundf(amp * cosf(6.2831853f * k / TONE_LUT));
    const uint32_t step = (uint32_t)(int32_t)llround((double)offset_hz * 4294967296.0 / fs_hz);
    const uint32_t period = CPU_HZ / fs_hz;
    const bool chunked = mode & PDX_RF_MODE_CHUNK_1S, measure = !(mode & PDX_RF_MODE_NO_STATS);
    last_stats = (pdx_rf_stats_t){.fs_hz = fs_hz, .period = period};
    uint32_t done_ms = 0;
    while (done_ms < ms) {
        const uint32_t chunk_ms = chunked ? (ms - done_ms > 1000u ? 1000u : ms - done_ms) : ms;
        pdx_rf_stats_t st = {0};
        tx_begin(&owner0);
        const uint32_t s = tone_loop(step, period, (uint32_t)((uint64_t)chunk_ms * fs_hz / 1000u),
                                     chunked ? 0xFFFFFFFFu : fs_hz / 4u /* 4 controlli al secondo */,
                                     chunked ? NULL : abort_fn, measure, &st);
        tx_end(owner0);
        last_stats.samples += st.samples;
        last_stats.late += st.late;
        if (st.lag_max > last_stats.lag_max) last_stats.lag_max = st.lag_max;
        done_ms += (uint32_t)((uint64_t)s * 1000u / fs_hz);
        if (s * 1000ull < (uint64_t)chunk_ms * fs_hz) break;      /* fermato dallo stop */
        if (chunked && abort_fn && abort_fn()) break;
    }
    return done_ms;
}

/* ------------------------------------------------------------ QPSK con RRC
 * Ogni campione di uscita è la somma di 8 simboli (±1) pesati dal filtro: con 8 bit di storia dei simboli I (o Q) la somma
 * è precalcolata in QPSK_T[fase][storia]. Per campione: 2 letture, l'impacchettamento e una scrittura nel DAC. */
#define QPSK_SPAN    8u
#define QPSK_SPS_MAX 16u
#define QPSK_BETA    0.35f
static DRAM_ATTR int16_t qpsk_t[QPSK_SPS_MAX][256];

uint32_t pdx_rf_qpsk_sps(uint32_t sr)
{
    uint32_t sps = 2100000u / sr;  /* circa 2 MS/s */
    return sps < 4u ? 4u : sps > QPSK_SPS_MAX ? QPSK_SPS_MAX : sps;
}

static float rrc(float t, float b)  /* t in simboli */
{
    const float pi = 3.14159265f;
    if (fabsf(t) < 1e-6f) return 1.0f - b + 4.0f * b / pi;
    if (fabsf(fabsf(t) - 1.0f / (4.0f * b)) < 1e-4f)
        return b / 1.41421356f * ((1.0f + 2.0f / pi) * sinf(pi / (4.0f * b)) + (1.0f - 2.0f / pi) * cosf(pi / (4.0f * b)));
    return (sinf(pi * t * (1.0f - b)) + 4.0f * b * t * cosf(pi * t * (1.0f + b))) /
           (pi * t * (1.0f - (4.0f * b * t) * (4.0f * b * t)));
}

/* tabella per un ramo: il bit m della storia è il simbolo di m periodi fa (bit 0 = il più recente) */
/* sign = -1 sul ramo Q: complesso coniugato, cioè spettro invertito (mixer esterno con oscillatore sopra la IF) */
static bool iq_invert;
void pdx_rf_set_invert(bool on) { iq_invert = on; }

static void qpsk_build(uint32_t sps, int amp, int dc, int sign, int16_t (*t)[256])
{
    float h[QPSK_SPAN * QPSK_SPS_MAX];
    for (uint32_t n = 0; n < QPSK_SPAN * sps; n++) h[n] = rrc(((float)n - (float)(QPSK_SPAN / 2u * sps)) / (float)sps, QPSK_BETA);
    float peak = 0;
    for (uint32_t j = 0; j < sps; j++) {
        float worst = 0;
        for (uint32_t m = 0; m < QPSK_SPAN; m++) worst += fabsf(h[m * sps + j]);
        if (worst > peak) peak = worst;
    }
    const float g = (float)amp / peak;
    for (uint32_t j = 0; j < sps; j++)
        for (uint32_t hist = 0; hist < 256; hist++) {
            float v = 0;
            for (uint32_t m = 0; m < QPSK_SPAN; m++) v += ((hist >> m) & 1u ? 1.0f : -1.0f) * h[m * sps + j];
            int x = (int)lroundf(sign * g * v) + dc;
            t[j][hist] = (int16_t)(x > 511 ? 511 : x < -511 ? -511 : x);
        }
}

/* noinline: senza, il compilatore inserisce il ciclo nella funzione chiamante (in flash) e IRAM_ATTR si perde: il ciclo
 * girava dalla cache della flash e ogni attività del core 1 sulla cache ritardava i campioni (30/10) */
static IRAM_ATTR __attribute__((noinline)) uint32_t qpsk_loop(uint32_t sps, uint32_t period, uint32_t rem, uint32_t fs, uint32_t nsym,
                                    uint32_t check_every, pdx_rf_abort_fn abort_fn, pdx_rf_stats_t *st, const int16_t *ti,
                                    const int16_t *tq)
{
    uint32_t hi = 0, hq = 0, rng = 0x2545F491u, frac = 0, lag_max = 0, late = 0, sym = 0, next_check = check_every;
    uint32_t tn = esp_cpu_get_cycle_count() + 2000u;
    for (; sym < nsym; sym++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;  /* xorshift32: 2 bit per simbolo */
        hi = ((hi << 1) | (rng & 1u)) & 0xFFu;
        hq = ((hq << 1) | ((rng >> 1) & 1u)) & 0xFFu;
        const int16_t *ri = ti + hi, *rq = tq + hq;
        for (uint32_t j = 0; j < sps; j++, ri += 256, rq += 256) {
            const uint32_t w = pdx_rf_dac_word(*ri, *rq);
            while ((int32_t)(esp_cpu_get_cycle_count() - tn) < 0) {}
            DAC_BUF[0] = w;
            const uint32_t lag = esp_cpu_get_cycle_count() - tn;
            if (lag > lag_max) lag_max = lag;
            if (lag > period / 4u) late++;
            tn += period;                                   /* scadenze razionali: CPU_HZ / fs senza errore cumulato */
            frac += rem;
            if (frac >= fs) { frac -= fs; tn++; }
        }
        if (sym == next_check) {
            next_check += check_every;
            if (abort_fn && abort_fn()) { sym++; break; }
            /* l'orologio dei campioni prosegue: riallinearlo qui farebbe saltare la fase dei simboli (29/10) */
        }
    }
    st->samples = sym * sps;
    st->lag_max = lag_max;
    st->late = late;
    return sym;
}

static DRAM_ATTR int16_t qpsk_tq[QPSK_SPS_MAX][256];

uint32_t pdx_rf_tx_qpsk(int amp, uint32_t sr, int dci, int dcq, uint32_t ms, pdx_rf_abort_fn abort_fn)
{
    uint32_t owner0;
    if (amp > PDX_RF_AMP_MAX) amp = PDX_RF_AMP_MAX;
    const uint32_t sps = pdx_rf_qpsk_sps(sr), fs = sr * sps;
    qpsk_build(sps, amp, dci, 1, qpsk_t);
    qpsk_build(sps, amp, dcq, iq_invert ? -1 : 1, qpsk_tq);
    last_stats = (pdx_rf_stats_t){.fs_hz = fs, .period = CPU_HZ / fs};
    const uint32_t nsym = (uint32_t)((uint64_t)ms * sr / 1000u);
    tx_begin(&owner0);
    const uint32_t sym = qpsk_loop(sps, CPU_HZ / fs, CPU_HZ % fs, fs, nsym, sr / 4u, abort_fn, &last_stats, &qpsk_t[0][0],
                                   &qpsk_tq[0][0]);
    tx_end(owner0);
    return (uint32_t)((uint64_t)sym * 1000u / sr);
}

/* QPSK da un flusso di simboli: 4 simboli per byte nel buffer circolare (bit 0 = I, bit 1 = Q, 1 = +1, primo simbolo nei
 * bit bassi). Il buffer lo riempie un altro core; se si svuota si contano gli svuotamenti e si ripete l'ultimo byte. */
/* noinline: senza, il compilatore inserisce il ciclo nella funzione chiamante (in flash) e IRAM_ATTR si perde: il ciclo
 * girava dalla cache della flash e ogni attività del core 1 sulla cache ritardava i campioni (30/10) */
static IRAM_ATTR __attribute__((noinline)) uint32_t qpsk_ring_loop(uint32_t sps, uint32_t period, uint32_t rem, uint32_t fs, uint32_t nsym,
                                         uint32_t check_every, pdx_rf_abort_fn abort_fn, pdx_rf_stats_t *st,
                                         const int16_t *ti, const int16_t *tq, pdx_rf_ring_t *rb)
{
    uint32_t hi = 0, hq = 0, cur = 0, frac = 0, lag_max = 0, late = 0, sym = 0, next_check = check_every, under = 0;
    uint32_t r = rb->r, nbytes = 0, s1 = 0, s2 = 0;
    const uint32_t mask = rb->size - 1u;
    uint32_t tn = esp_cpu_get_cycle_count() + 2000u;
    for (; sym < nsym; sym++) {
        if ((sym & 3u) == 0) {
            if (r != rb->w) { cur = rb->buf[r & mask]; r++; rb->r = r; nbytes++; s1 += cur; s2 += s1; }
            else under++;
        }
        hi = ((hi << 1) | (cur & 1u)) & 0xFFu;
        hq = ((hq << 1) | ((cur >> 1) & 1u)) & 0xFFu;
        cur >>= 2;
        const int16_t *ri = ti + hi, *rq = tq + hq;
        for (uint32_t j = 0; j < sps; j++, ri += 256, rq += 256) {
            const uint32_t w = pdx_rf_dac_word(*ri, *rq);
            while ((int32_t)(esp_cpu_get_cycle_count() - tn) < 0) {}
            DAC_BUF[0] = w;
            const uint32_t lag = esp_cpu_get_cycle_count() - tn;
            if (lag > lag_max) lag_max = lag;
            if (lag > period / 4u) { late++; pdx_diag_late[pdx_diag_phase & 7u]++; }
            tn += period;
            frac += rem;
            if (frac >= fs) { frac -= fs; tn++; }
        }
        if (sym == next_check) {
            next_check += check_every;
            if (abort_fn && abort_fn()) { sym++; break; }
            /* l'orologio dei campioni prosegue: riallinearlo qui farebbe saltare la fase dei simboli (29/10) */
        }
    }
    st->samples = sym * sps;
    st->lag_max = lag_max;
    st->late = late;
    st->underruns = under;
    st->bytes = nbytes;
    st->sum1 = s1;
    st->sum2 = s2;
    return sym;
}

volatile uint32_t pdx_diag_phase;
uint32_t pdx_diag_late[8];

uint32_t pdx_rf_tx_qpsk_ring(int amp, uint32_t sr, int dci, int dcq, uint32_t ms, pdx_rf_ring_t *rb, pdx_rf_abort_fn abort_fn)
{
    uint32_t owner0;
    if (amp > PDX_RF_AMP_MAX) amp = PDX_RF_AMP_MAX;
    const uint32_t sps = pdx_rf_qpsk_sps(sr), fs = sr * sps;
    qpsk_build(sps, amp, dci, 1, qpsk_t);
    qpsk_build(sps, amp, dcq, iq_invert ? -1 : 1, qpsk_tq);
    last_stats = (pdx_rf_stats_t){.fs_hz = fs, .period = CPU_HZ / fs};
    for (int k = 0; k < 8; k++) pdx_diag_late[k] = 0;
    const uint32_t nsym = (uint32_t)((uint64_t)ms * sr / 1000u) & ~3u;
    tx_begin(&owner0);
    /* stop controllato ogni 1/4 s, su un multiplo di 4 simboli (inizio di un byte) */
    const uint32_t sym = qpsk_ring_loop(sps, CPU_HZ / fs, CPU_HZ % fs, fs, nsym, (sr / 4u) & ~3u, abort_fn, &last_stats,
                                        &qpsk_t[0][0], &qpsk_tq[0][0], rb);
    tx_end(owner0);
    return (uint32_t)((uint64_t)sym * 1000u / sr);
}
