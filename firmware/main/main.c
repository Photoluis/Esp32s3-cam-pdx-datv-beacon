/* Beacon DATV 13 cm - IZ7PDX
 * RF0007: modulatore I/Q dell'ESP32-S3 (CW, tono, QPSK RRC, simboli DVB-S2 dalla flash) beacon autonomo con configurazione in NVS e
 * interfaccia web.
 *
 * Comandi sulla UART0 (porta TTL, 115200), uno per riga:
 *   INFO
 *   CW   <MHz> <amp> <secondi>              portante sull'LO
 *   TONE <MHz> <offset_kHz> <amp> <secondi> [fs_kHz]  tono a LO + offset (si vedono anche LO residuo e immagine);
 *        campionamento 1000/2000/4000 kHz (predefinito 2000); a fine TX i ritardi delle scritture nel DAC
 *   TONE ... <fs_kHz> <mode>   diagnosi: mode bit 0 = TX riaccesa ogni secondo, bit 1 = senza misura dei ritardi
 *   QPSK <MHz> <SR_kS> <amp> <secondi> [dcI dcQ]  simboli casuali (PRBS), RRC roll-off 0.35, correzione DC opzionale
 *   SYMINFO                                  descrive il file di simboli nella partizione "symbols"
 *   PLAY <MHz> <amp> <secondi> [dcI dcQ]     trasmette in loop i simboli della partizione (SR dal file)
 *   CAMBENCH <w> <h> <fotogrammi> <kbps> <fps>  prova camera + H.264 software sul core 1 (nessuna RF)
 *   CONFIG | SET <chiave> <valore> | SAVE | DEFAULTS   configurazione del beacon (chiavi: auto freq amp dci dcq mono cam
 *        pause wait call); SAVE la scrive in NVS
 *   BEACON                                   ciclo del beacon con la configurazione attuale, fino allo stop
 *   WIFI ON|OFF                              access point di configurazione "<QRZ>-BEACON", pagina http://192.168.4.1
 *        (acceso a TX ferma, spento durante ogni TX: la radio Wi-Fi fa da modulatore)
 * All'accensione, se AUTO = ON, il beacon parte da solo dopo l'attesa impostata (un tasto o BOOT la annullano).
 * Qualsiasi carattere ricevuto o il pulsante BOOT fermano la TX. Frequenze ammesse 2300..2450 MHz; i comandi di prova
 * trasmettono al massimo 300 s.
 */
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_chip_info.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/uart_ll.h"
#include "soc/gpio_reg.h"
#include "rf_s3.h"
#include "sym_player.h"
#include "cam_bench.h"
#include "beacon_cfg.h"
#include "web_ui.h"
#include "live_tx.h"
#include "dvbs2_enc.h"
#include "ts_mux.h"
#include "cam_live.h"

int pdx_s2_selftest(void);                  /* s2_test.c */

#define FW_VERSION "RF0007"
#define TX_MAX_S   300u

#define BOOT_GPIO 0                 /* pulsante BOOT della scheda: premuto = livello basso */

static pdx_cfg_t cfg;

/* TX con la radio "pulita": se l'access point è stato acceso dall'avvio, configurazione e comando vengono messi nella RAM
 * RTC (sopravvive al riavvio software), la scheda riparte e il comando viene eseguito prima di accendere il Wi-Fi */
#define RTC_PENDING_MAGIC 0x50445831u
typedef struct {
    uint32_t magic;
    pdx_cfg_t cfg;
    char cmd[128];
    uint32_t crc;
} rtc_pending_t;
RTC_NOINIT_ATTR static rtc_pending_t rtc_pending;
static const char *current_line = "";      /* riga del comando in esecuzione, prima della divisione in parole */

static uint32_t pending_crc(const rtc_pending_t *p)
{
    return esp_rom_crc32_le(0, (const uint8_t *)p, offsetof(rtc_pending_t, crc));
}

static void reboot_and_run(const char *cmd)
{
    pdx_cfg_lock();
    rtc_pending.cfg = cfg;
    pdx_cfg_unlock();
    snprintf(rtc_pending.cmd, sizeof(rtc_pending.cmd), "%s", cmd);
    rtc_pending.magic = RTC_PENDING_MAGIC;
    rtc_pending.crc = pending_crc(&rtc_pending);
    printf("RIAVVIO per TX con la radio pulita (access point usato): poi \"%s\"\n", cmd);
    fflush(stdout);
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(200));
    esp_restart();
}

/* comando in sospeso dopo il riavvio: false se non c'è */
static bool take_pending(char *cmd, size_t n)
{
    const bool ok = esp_reset_reason() == ESP_RST_SW && rtc_pending.magic == RTC_PENDING_MAGIC &&
                    rtc_pending.crc == pending_crc(&rtc_pending);
    rtc_pending.magic = 0;
    if (!ok) return false;
    cfg = rtc_pending.cfg;
    snprintf(cmd, n, "%s", rtc_pending.cmd);
    return true;
}

/* durante la TX gli interrupt del core 0 sono mascherati e il byte resta nella FIFO hardware; fuori dalla TX il driver
 * lo sposta subito nel suo buffer: vanno guardati tutti e due */
static bool uart_has_byte(void)
{
    size_t n = 0;
    uart_get_buffered_data_len(UART_NUM_0, &n);
    return n > 0 || uart_ll_get_rxfifo_len(UART_LL_GET_HW(UART_NUM_0)) > 0;
}

/* dopo uno stop: il carattere che ha fermato la TX (e quanto arriva subito dopo) non deve finire nel comando seguente */
static void discard_input(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    uart_flush_input(UART_NUM_0);
}

/* stop durante la TX: solo registri (FIFO della UART, pin BOOT), pochi cicli e nessun accesso alla memoria. Con gli
 * interrupt del core 0 mascherati il carattere resta nella FIFO hardware. */
static IRAM_ATTR bool tx_stop_hw(void)
{
    return uart_ll_get_rxfifo_len(UART_LL_GET_HW(UART_NUM_0)) > 0 || !(REG_READ(GPIO_IN_REG) & BIT(BOOT_GPIO));
}

/* stop fuori dalla TX (attese, pause): un carattere sulla seriale oppure il pulsante BOOT */
static bool stop_requested(void)
{
    return uart_has_byte() || gpio_get_level(BOOT_GPIO) == 0;
}

/* informazioni del file di simboli per la pagina web */
static void refresh_symbols(void)
{
    pdx_sym_header_t h;
    const char *why = "";
    if (pdx_sym_open(&h, &why)) pdx_web_set_symbols(true, h.label, h.sr, h.nsym);
    else pdx_web_set_symbols(false, why, 0, 0);
}

static void run_beacon(void)
{
    if (pdx_web_radio_dirty()) reboot_and_run("BEACON");
    pdx_cfg_lock();
    const pdx_cfg_t snap = cfg;                    /* il server web non deve cambiare i valori durante il ciclo */
    pdx_cfg_unlock();
    pdx_web_set_mode("beacon in TX");
    pdx_web_stop();
    printf("BEACON avvio (un tasto o BOOT per fermare)\n");
    fflush(stdout);
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(100));
    uart_flush_input(UART_NUM_0);
    const uint32_t n = pdx_beacon_run(&snap, tx_stop_hw, stop_requested);
    while (gpio_get_level(BOOT_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));   /* attende il rilascio di BOOT */
    discard_input();
    printf("BEACON fermo dopo %u cicli completi: modo configurazione\n", (unsigned)n);
    pdx_web_set_mode("configurazione");
    pdx_web_start();
}

static bool parse_mhz(const char *s, uint32_t *khz)
{
    char *e;
    double f = strtod(s, &e);
    if (e == s || *e) return false;
    *khz = (uint32_t)(f * 1000.0 + 0.5);
    return *khz >= PDX_RF_FMIN_KHZ && *khz <= PDX_RF_FMAX_KHZ;
}

/* parte comune di ogni TX: frequenza, ampiezza e durata già lette; restituisce false se i valori non vanno bene */
static bool tx_common(const char *f_arg, int amp, uint32_t secs, uint32_t *fkhz)
{
    if (!parse_mhz(f_arg, fkhz)) { printf("ERR frequenza fuori da 2300..2450 MHz\n"); return false; }
    if (amp < 1 || amp > PDX_RF_AMP_MAX) { printf("ERR amp 1..%d\n", PDX_RF_AMP_MAX); return false; }
    if (secs < 1 || secs > TX_MAX_S) { printf("ERR secondi 1..%u\n", (unsigned)TX_MAX_S); return false; }
    if (pdx_web_radio_dirty()) reboot_and_run(current_line);
    pdx_web_set_mode("prova in TX");
    pdx_web_stop();                                 /* la radio serve al modulatore */
    if (!pdx_rf_tune(*fkhz)) { printf("ERR sintonia\n"); return false; }
    return true;
}

static void tx_start_note(void)
{
    fflush(stdout);
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(100));
    uart_flush_input(UART_NUM_0);
}

static void tx_end_note(uint32_t ms, bool stats)
{
    discard_input();
    if (stats) {
        const pdx_rf_stats_t *st = pdx_rf_last_stats();
        printf("STAT fs %u Hz periodo %u cicli, campioni %u, ritardo max %u cicli, in ritardo %u\n", (unsigned)st->fs_hz,
               (unsigned)st->period, (unsigned)st->samples, (unsigned)st->lag_max, (unsigned)st->late);
    }
    printf("TX OFF dopo %u ms\n", (unsigned)ms);
    pdx_web_set_mode("configurazione");
    pdx_web_start();
}

static void handle(char *line)
{
    char raw[128];
    snprintf(raw, sizeof(raw), "%s", line);
    current_line = raw;
    char *argv[8] = {0};
    int argc = 0;
    for (char *t = strtok(line, " \t"); t && argc < 8; t = strtok(NULL, " \t")) argv[argc++] = t;
    if (!argc) return;
    uint32_t fkhz;

    if (!strcasecmp(argv[0], "INFO")) {
        esp_chip_info_t ci;
        esp_chip_info(&ci);
        printf("OK %s ESP32-S3 rev %d, LO %.3f Hz, limiti %u..%u kHz\n", FW_VERSION, ci.revision, pdx_rf_lo_hz(),
               (unsigned)PDX_RF_FMIN_KHZ, (unsigned)PDX_RF_FMAX_KHZ);
        /* frequenza reale della CPU: cicli contati in 200 ms del timer di sistema */
        const int64_t t0 = esp_timer_get_time();
        const uint32_t c0 = esp_cpu_get_cycle_count();
        while (esp_timer_get_time() - t0 < 200000) {}
        const uint32_t c1 = esp_cpu_get_cycle_count();
        const int64_t t1 = esp_timer_get_time();
        printf("CPU %.3f MHz misurati (atteso 240)\n", (double)(c1 - c0) / (double)(t1 - t0));
        printf("RAM interna libera %u KB (blocco DMA max %u KB), PSRAM libera %u KB\n",
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
               (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) >> 10),
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));

    } else if (!strcasecmp(argv[0], "CW")) {                 /* CW <MHz> <amp> <s> */
        if (argc != 4) { printf("ERR argomenti\n"); return; }
        const int amp = atoi(argv[2]);
        const uint32_t secs = (uint32_t)atoi(argv[3]);
        if (!tx_common(argv[1], amp, secs, &fkhz)) return;
        printf("TX ON CW %.3f MHz (LO %.3f Hz) amp %d per %u s\n", fkhz / 1000.0, pdx_rf_lo_hz(), amp, (unsigned)secs);
        tx_start_note();
        tx_end_note(pdx_rf_tx_cw(amp, secs * 1000u, tx_stop_hw), false);

    } else if (!strcasecmp(argv[0], "TONE")) {               /* TONE <MHz> <off_kHz> <amp> <s> [fs_kHz [mode]] */
        if (argc < 5 || argc > 7) { printf("ERR argomenti\n"); return; }
        const int32_t off_hz = (int32_t)(atof(argv[2]) * 1000.0);
        const int amp = atoi(argv[3]);
        const uint32_t secs = (uint32_t)atoi(argv[4]);
        const uint32_t fs_hz = argc >= 6 ? (uint32_t)atoi(argv[5]) * 1000u : 2000000u;
        const uint32_t mode = argc == 7 ? (uint32_t)atoi(argv[6]) & 3u : 0u;
        if (fs_hz != 1000000u && fs_hz != 2000000u && fs_hz != 4000000u) { printf("ERR fs 1000/2000/4000 kHz\n"); return; }
        const int32_t off_max = (int32_t)fs_hz / 2 - 50000;
        if (off_hz < -off_max || off_hz > off_max) { printf("ERR offset oltre fs/2\n"); return; }
        if (!tx_common(argv[1], amp, secs, &fkhz)) return;
        printf("TX ON TONE %.3f MHz (LO %.3f Hz) offset %+ld Hz amp %d fs %u kHz mode %u per %u s\n", fkhz / 1000.0,
               pdx_rf_lo_hz(), (long)off_hz, amp, (unsigned)(fs_hz / 1000u), (unsigned)mode, (unsigned)secs);
        tx_start_note();
        tx_end_note(pdx_rf_tx_tone(amp, off_hz, fs_hz, secs * 1000u, mode, tx_stop_hw), true);

    } else if (!strcasecmp(argv[0], "QPSK")) {               /* QPSK <MHz> <SR_kS> <amp> <s> [dcI dcQ] */
        if (argc != 5 && argc != 7) { printf("ERR argomenti\n"); return; }
        const uint32_t sr = (uint32_t)(atof(argv[2]) * 1000.0 + 0.5);
        const int amp = atoi(argv[3]);
        const uint32_t secs = (uint32_t)atoi(argv[4]);
        const int dci = argc == 7 ? atoi(argv[5]) : 0, dcq = argc == 7 ? atoi(argv[6]) : 0;
        if (sr < PDX_RF_SR_MIN || sr > PDX_RF_SR_MAX) {
            printf("ERR SR %u..%u kS\n", (unsigned)(PDX_RF_SR_MIN / 1000u), (unsigned)(PDX_RF_SR_MAX / 1000u));
            return;
        }
        if (dci < -100 || dci > 100 || dcq < -100 || dcq > 100) { printf("ERR dc -100..100\n"); return; }
        if (!tx_common(argv[1], amp, secs, &fkhz)) return;
        const uint32_t sps = pdx_rf_qpsk_sps(sr);
        printf("TX ON QPSK %.3f MHz (LO %.3f Hz) SR %u S/s, %u campioni/simbolo (fs %u Hz), RRC 0.35, amp %d dc %d/%d per %u s\n",
               fkhz / 1000.0, pdx_rf_lo_hz(), (unsigned)sr, (unsigned)sps, (unsigned)(sr * sps), amp, dci, dcq, (unsigned)secs);
        tx_start_note();
        tx_end_note(pdx_rf_tx_qpsk(amp, sr, dci, dcq, secs * 1000u, tx_stop_hw), true);

    } else if (!strcasecmp(argv[0], "SYMINFO") || !strcasecmp(argv[0], "PLAY")) {   /* PLAY <MHz> <amp> <s> [dcI dcQ] */
        const bool play = !strcasecmp(argv[0], "PLAY");
        if (play && argc != 4 && argc != 6) { printf("ERR argomenti\n"); return; }
        pdx_sym_header_t h;
        const char *why = "";
        if (!pdx_sym_open(&h, &why)) { printf("ERR simboli: %s\n", why); return; }
        printf("OK simboli \"%s\": SR %u S/s, %u simboli = %.2f s di loop\n", h.label, (unsigned)h.sr, (unsigned)h.nsym,
               (double)h.nsym / h.sr);
        if (!play) return;
        const int amp = atoi(argv[2]);
        const uint32_t secs = (uint32_t)atoi(argv[3]);
        const int dci = argc == 6 ? atoi(argv[4]) : 0, dcq = argc == 6 ? atoi(argv[5]) : 0;
        if (dci < -100 || dci > 100 || dcq < -100 || dcq > 100) { printf("ERR dc -100..100\n"); return; }
        if (!tx_common(argv[1], amp, secs, &fkhz)) return;
        printf("TX ON PLAY %.3f MHz (LO %.3f Hz) SR %u S/s, %u campioni/simbolo, RRC 0.35, amp %d dc %d/%d per %u s\n",
               fkhz / 1000.0, pdx_rf_lo_hz(), (unsigned)h.sr, (unsigned)pdx_rf_qpsk_sps(h.sr), amp, dci, dcq, (unsigned)secs);
        tx_start_note();
        const uint32_t rx0 = pdx_rf_rx_frames();
        const uint32_t ms = pdx_sym_play(amp, dci, dcq, secs * 1000u, tx_stop_hw);
        vTaskDelay(pdMS_TO_TICKS(100));                /* il task Wi-Fi (core 0) recupera le trame arrivate durante la TX */
        printf("STAT buffer simboli vuoto %u volte, trame Wi-Fi ricevute durante la TX %u\n",
               (unsigned)pdx_rf_last_stats()->underruns, (unsigned)(pdx_rf_rx_frames() - rx0));
        tx_end_note(ms, true);

    } else if (!strcasecmp(argv[0], "ESINFO")) {
        char label[40];
        uint32_t units;
        double loop_s;
        if (pdx_live_es_info(label, sizeof(label), &units, &loop_s))
            printf("OK ES \"%s\": %u unita, loop %.2f s\n", label, (unsigned)units, loop_s);
        else
            printf("ERR nessun file ES a 0x%x nella partizione symbols\n", (unsigned)PDX_ES_OFFSET);

    } else if (!strcasecmp(argv[0], "TSDUMP")) {              /* TSDUMP <pacchetti>: TS del multiplexer in esadecimale */
        if (argc != 2) { printf("ERR TSDUMP <pacchetti>\n"); return; }
        const uint32_t n = (uint32_t)atoi(argv[1]);
        char label[40];
        uint32_t units;
        double loop_s;
        if (!pdx_live_es_info(label, sizeof(label), &units, &loop_s)) { printf("ERR file ES\n"); return; }
        pdx_s2_t s2;
        if (pdx_s2_init(&s2, "3/4", false, true)) { printf("ERR s2\n"); return; }
        const double rate = pdx_s2_ts_bitrate(&s2, 333000);
        pdx_s2_free(&s2);
        static pdx_mux_t mux;
        const uint8_t *es = pdx_live_es_data();
        const char *e = pdx_mux_init(&mux, es, rate, cfg.callsign, "IZ7PDX BEACON", false);
        if (e) { printf("ERR %s\n", e); return; }
        printf("TSDUMP %u pacchetti a %.0f bit/s\n", (unsigned)n, rate);
        uint8_t pk[188];
        for (uint32_t i = 0; i < n; i++) {
            pdx_mux_next(&mux, pk);
            printf("T ");
            for (int k = 0; k < 188; k++) printf("%02x", pk[k]);
            printf("\n");
        }
        printf("TSDUMP fine: video %u audio %u nulli %u, PES in ritardo %u\n", (unsigned)mux.stats_video,
               (unsigned)mux.stats_audio, (unsigned)mux.stats_null, (unsigned)mux.late);

    } else if (!strcasecmp(argv[0], "CAMDUMP")) {             /* CAMDUMP <pacchetti>: TS del modo camera, senza RF */
        if (argc != 2) { printf("ERR CAMDUMP <pacchetti>\n"); return; }
        pdx_live_cfg_t lc = {.sr = 333000, .fec = "3/4", .pilots = true, .service = cfg.callsign,
                             .provider = "IZ7PDX BEACON", .camera = true, .cam_fps = 6, .cam_kbps = 250,
                             .freq_khz = cfg.freq_khz};
        printf("CAMDUMP %u pacchetti\n", (unsigned)atoi(argv[1]));
        const char *e = pdx_live_cam_dump(&lc, (uint32_t)atoi(argv[1]));
        if (e) printf("ERR %s\n", e);

    } else if (!strcasecmp(argv[0], "ESPLAY")) {              /* ESPLAY <MHz> <amp> <s> [fec] [short] [nopilots] [cam] */
        if (argc < 4) { printf("ERR ESPLAY <MHz> <amp> <s> [fec] [short] [nopilots] [cam]\n"); return; }
        pdx_live_cfg_t lc = {.sr = 333000, .fec = "3/4", .shortf = false, .pilots = true, .amp = atoi(argv[2]),
                             .service = cfg.callsign, .provider = "IZ7PDX BEACON", .cam_fps = 6, .cam_kbps = 250};
        for (int i = 4; i < argc; i++) {
            if (!strcasecmp(argv[i], "short")) lc.shortf = true;
            else if (!strcasecmp(argv[i], "nopilots")) lc.pilots = false;
            else if (!strcasecmp(argv[i], "cam")) lc.camera = true;
            else lc.fec = argv[i];
        }
        const uint32_t secs = (uint32_t)atoi(argv[3]);
        if (!tx_common(argv[1], lc.amp, secs, &fkhz)) return;
        lc.freq_khz = fkhz;
        printf("TX ON ESPLAY %.3f MHz SR %u FEC %s %s %s, amp %d per %u s\n", fkhz / 1000.0, (unsigned)lc.sr, lc.fec,
               lc.shortf ? "corti" : "normali", lc.pilots ? "pilot" : "senza pilot", lc.amp, (unsigned)secs);
        if (lc.camera) printf("    video dalla camera: H.264 %dx%d, %d fps, %d kbit/s\n", PDX_CAM_W, PDX_CAM_H, lc.cam_fps, lc.cam_kbps);
        tx_start_note();
        const char *why;
        const uint32_t ms = pdx_live_play(&lc, secs * 1000u, tx_stop_hw, &why);
        if (why) printf("ERR %s\n", why);
        tx_end_note(ms, true);

    } else if (!strcasecmp(argv[0], "S2TEST")) {             /* codificatore DVB-S2 del firmware contro quello del PC */
        pdx_s2_selftest();

    } else if (!strcasecmp(argv[0], "HEAPS")) {              /* diagnosi: regioni dell'heap (il banco del DAC deve restarne fuori) */
        heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
        heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);

    } else if (!strcasecmp(argv[0], "CONFIG")) {
        pdx_cfg_print(&cfg);

    } else if (!strcasecmp(argv[0], "SET")) {                /* SET <chiave> <valore>  (poi SAVE per salvare) */
        if (argc != 3) { printf("ERR SET <chiave> <valore>\n"); return; }
        pdx_cfg_lock();
        const char *why = pdx_cfg_set(&cfg, argv[1], argv[2]);
        pdx_cfg_unlock();
        if (why) { printf("ERR %s\n", why); return; }
        printf("OK ");
        pdx_cfg_print(&cfg);

    } else if (!strcasecmp(argv[0], "SAVE")) {
        pdx_cfg_lock();
        const bool saved = pdx_cfg_save(&cfg);
        pdx_cfg_unlock();
        printf(saved ? "OK configurazione salvata\n" : "ERR salvataggio\n");

    } else if (!strcasecmp(argv[0], "DEFAULTS")) {
        pdx_cfg_defaults(&cfg);
        printf("OK valori predefiniti (SAVE per salvarli) ");
        pdx_cfg_print(&cfg);

    } else if (!strcasecmp(argv[0], "BEACON")) {
        run_beacon();

    } else if (!strcasecmp(argv[0], "WIFI")) {              /* WIFI ON|OFF: access point di configurazione */
        if (argc == 2 && !strcasecmp(argv[1], "OFF")) pdx_web_stop();
        else if (argc == 2 && !strcasecmp(argv[1], "ON")) pdx_web_start();
        else printf("ERR WIFI ON|OFF\n");

    } else if (!strcasecmp(argv[0], "CAMBENCH")) {           /* CAMBENCH <w> <h> <fotogrammi> <kbps> <fps> */
        if (argc != 6) { printf("ERR argomenti\n"); return; }
        const int frames = atoi(argv[3]);
        if (frames < 1 || frames > 1000) { printf("ERR fotogrammi 1..1000\n"); return; }
        pdx_cam_bench(atoi(argv[1]), atoi(argv[2]), frames, atoi(argv[4]), atoi(argv[5]));

    } else {
        printf("ERR comando sconosciuto\n");
    }
}

/* partenza automatica: solo se AUTO = ON, dopo boot_wait_s secondi in cui un tasto o BOOT la annullano */
static void autostart(void)
{
    if (!cfg.auto_tx) {
        printf("AUTO TX OFF: nessuna trasmissione finché non arriva un comando (BEACON per avviare)\n");
        return;
    }
    printf("AUTO TX ON: partenza tra %u s. Un tasto, il pulsante BOOT o l'apertura della pagina web annullano.\n",
           cfg.boot_wait_s);
    pdx_web_set_mode("AUTO TX in attesa");
    pdx_web_start();
    for (int s = cfg.boot_wait_s; s > 0; s--) {
        if (s % 5 == 0 || s <= 3) { printf("AUTO TX tra %d s\n", s); fflush(stdout); }
        for (int k = 0; k < 20; k++) {
            if (pdx_web_take_start_request()) break;      /* AVVIA dalla pagina: subito */
            if (stop_requested() || pdx_web_seen_client()) {
                while (gpio_get_level(BOOT_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));
                discard_input();
                printf("AUTO TX annullata: modo configurazione\n");
                pdx_web_set_mode("configurazione");
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    run_beacon();
}

void app_main(void)
{
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));
    gpio_config_t boot_pin = {.pin_bit_mask = 1ull << BOOT_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    gpio_config(&boot_pin);
    printf("\n=== Beacon DATV IZ7PDX %s ===\n", FW_VERSION);
    pdx_rf_init();
    printf("PHY pronta, LO %.3f Hz\n", pdx_rf_lo_hz());
    printf(pdx_cfg_load(&cfg) ? "configurazione caricata\n" : "nessuna configurazione salvata: valori predefiniti\n");
    pdx_cfg_print(&cfg);
    pdx_web_init(&cfg, FW_VERSION);
    refresh_symbols();
    char pending[128];
    if (take_pending(pending, sizeof(pending))) {
        printf("comando dopo il riavvio: %s\n", pending);
        pdx_cfg_print(&cfg);
        handle(pending);                            /* TX con la radio mai passata dal modo AP */
    } else {
        autostart();
    }
    pdx_web_start();

    char line[128];
    size_t used = 0;
    for (;;) {
        uint8_t c;
        if (pdx_web_take_start_request()) {         /* AVVIA BEACON dalla pagina web */
            run_beacon();
            used = 0;
        }
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(50)) != 1) continue;
        if (c == '\r') continue;
        if (c != '\n') {
            if (used < sizeof(line) - 1) line[used++] = (char)c;
            continue;
        }
        line[used] = 0;
        used = 0;
        handle(line);
        fflush(stdout);
    }
}
