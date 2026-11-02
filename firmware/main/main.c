/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * v1.0.0: modulatore I/Q dell'ESP32-S3 (CW, tono, QPSK RRC, simboli DVB-S2 dalla flash) beacon autonomo con configurazione in NVS e
 * interfaccia web.
 * v1.1.0: symbol rate 125/250/333/500/1000 kS (1000 kS solo monoscopio), QTH locator, livello a cursore (-40..-5 dB),
 * temperatura nel registro a fine segmento.
 *
 * Comandi sulla UART0 (porta TTL, 115200), uno per riga:
 *   INFO
 *   (<liv> = livello in dB teorici, 0 = massimo, es. -7.6dB; oppure ampiezza 1..480)
 *   CW   <MHz> <liv> <secondi>              portante sull'LO
 *   TONE <MHz> <offset_kHz> <liv> <secondi> [fs_kHz]  tono a LO + offset (si vedono anche LO residuo e immagine);
 *        campionamento 1000/2000/4000 kHz (predefinito 2000); a fine TX i ritardi delle scritture nel DAC
 *   TONE ... <fs_kHz> <mode>   diagnosi: mode bit 0 = TX riaccesa ogni secondo, bit 1 = senza misura dei ritardi
 *   QPSK <MHz> <SR_kS> <liv> <secondi> [dcI dcQ]  simboli casuali (PRBS), RRC roll-off 0.35, correzione DC opzionale
 *   SYMINFO                                  descrive il file di simboli nella partizione "symbols"
 *   PLAY <MHz> <liv> <secondi> [dcI dcQ]     trasmette in loop i simboli della partizione (SR dal file)
 *   CAMBENCH <w> <h> <fotogrammi> <kbps> <fps>  prova camera + H.264 software sul core 1 (nessuna RF)
 *   ESPLAY <MHz> <liv> <s> [fec] [<SR>k] [short] [nopilots] [cam|pip|img|live|user]  TS generato sull'ESP: monoscopio standard,
 *        camera, camera in PiP, immagine personalizzata, video personalizzato
 *   TSDUMP | CAMDUMP | PIPDUMP | IMGDUMP <pacchetti>   TS in esadecimale senza RF (tools/ts_capture.py)
 *   MEDIA | MEDIAERASE VIDEO|IMAGE | MEDIAW VIDEO|IMAGE <byte> <crc32>   monoscopi in flash (tools/upload_media.py)
 *   CONFIG | SET <chiave> <valore> | SAVE | DEFAULTS   configurazione del beacon (chiavi: auto freq fec level amp dci dcq
 *        mono monosrc cam cammode pipbg ovl pause wait call loc appass lo xvtr led telem); SAVE la scrive in NVS
 *        loc = QTH locator Maidenhead (JN80 o JN80PS, - per toglierlo), in sovrimpressione accanto alla frequenza
 *        led NORMAL|MIN|OFF = LED di stato normale, al minimo o spento (alimentazione a batteria)
 *        telem ON|OFF = riga con temperatura del chip, tempo di accensione e ciclo nei modi dal vivo
 *        lo = oscillatore locale di un mixer esterno (MHz, 0 = nessuno), xvtr SUM|DIFF: la scheda trasmette sempre sulla IF
 *        (2300..2450 MHz); sovrimpressione, SDT e pagina web mostrano la frequenza finale
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
#include "media_store.h"
#include "led_status.h"
#include "chip_temp.h"
#include "rx_s3.h"
#include "esp_rom_crc.h"
#include <ctype.h>

int pdx_s2_selftest(void);                  /* s2_test.c */

#define FW_VERSION "v1.1.0"
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
/* Parrot P1b: ultima cattura decimata (int8 I/Q) in PSRAM */
#define RX_BUF_BYTES (6u * 1024u * 1024u)
static uint8_t *rx_buf;
static uint32_t rx_len, rx_fs, rx_lo;
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
    printf("RIAVVIO per TX con la radio pulita (access point o ricezione usati): poi \"%s\"\n", cmd);
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

static void run_beacon(void)
{
    if (pdx_web_radio_dirty()) reboot_and_run("BEACON");
    pdx_cfg_lock();
    const pdx_cfg_t snap = cfg;                    /* il server web non deve cambiare i valori durante il ciclo */
    pdx_cfg_unlock();
    pdx_web_set_mode("beacon in TX");
    pdx_web_stop();
    if (!strcmp(snap.callsign, "NOCALL")) printf("ATTENZIONE nominativo non impostato (NOCALL)\n");
    printf("BEACON avvio (un tasto o BOOT per fermare)\n");
    fflush(stdout);
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(100));
    uart_flush_input(UART_NUM_0);
    const uint32_t n = pdx_beacon_run(&snap, tx_stop_hw, stop_requested);
    while (gpio_get_level(BOOT_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));   /* attende il rilascio di BOOT */
    discard_input();
    printf("BEACON fermo dopo %u cicli completi: modo configurazione\n", (unsigned)n);
    pdx_led_idle();
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
    if (amp < 1 || amp > PDX_RF_AMP_MAX) {
        printf("ERR livello: dB da -40 a 0 (es. -7.6dB) oppure ampiezza 1..%d\n", PDX_RF_AMP_MAX);
        return false;
    }
    if (secs < 1 || secs > TX_MAX_S) { printf("ERR secondi 1..%u\n", (unsigned)TX_MAX_S); return false; }
    if (pdx_web_radio_dirty()) reboot_and_run(current_line);
    pdx_web_set_mode("prova in TX");
    pdx_web_stop();                                 /* la radio serve al modulatore */
    if (!pdx_rf_tune(*fkhz)) { printf("ERR sintonia\n"); return false; }
    bool inv;
    const uint64_t fin = pdx_cfg_final_khz(&cfg, *fkhz, &inv);
    pdx_rf_set_invert(inv);
    pdx_led_tx(pdx_amp_to_level(amp));
    if (cfg.lo_khz)
        printf("TRANSVERTER OL %.3f MHz: frequenza finale %.3f MHz%s\n", cfg.lo_khz / 1000.0, fin / 1000.0,
               inv ? " (spettro invertito dal mixer: compensato)" : "");
    printf("LIVELLO %.1f dB teorici (ampiezza %d/%d)\n", pdx_amp_to_level(amp), amp, PDX_RF_AMP_MAX);
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
    pdx_led_idle();
    pdx_web_set_mode("configurazione");
    pdx_web_start();
}

/* parametri del TS dal vivo dalla configurazione (sfondo PiP e sovrimpressione come nel beacon) */
static void live_cfg_from(pdx_live_cfg_t *lc, pdx_live_video_t v, uint32_t freq_khz, int amp, const char *fec)
{
    const uint8_t *usr_img = pdx_media_map(PDX_MEDIA_IMG_USR, NULL);
    *lc = (pdx_live_cfg_t){.sr = pdx_cfg_sr(&cfg), .fec = fec, .pilots = true, .amp = amp, .dci = cfg.dci, .dcq = cfg.dcq,
                           .service = cfg.callsign, .provider = pdx_cfg_provider(&cfg, freq_khz), .video = v,
                           .es = pdx_media_map(PDX_MEDIA_ES_STD, NULL), .ovl_full = cfg.ovl != 0,
                           .freq_khz = pdx_cfg_final_khz(&cfg, freq_khz, NULL), .telem = pdx_cfg_telemetry(&cfg, 0),
                           .locator = cfg.locator};
    if (v == PDX_LV_IMG) lc->bg = usr_img;
    else lc->bg = cfg.pip_bg && usr_img ? usr_img : pdx_media_map(PDX_MEDIA_IMG_STD, NULL);
}

/* MEDIAW VIDEO|IMAGE <byte> <crc32>: il PC (tools/upload_media.py) manda blocchi binari da 2048 byte e aspetta "ACK <n>"
 * dopo ciascuno; il firmware scrive solo nelle aree personalizzate e alla fine verifica CRC e contenuto */
static void media_upload(int argc, char **argv)
{
    if (argc != 4 || (strcasecmp(argv[1], "VIDEO") && strcasecmp(argv[1], "IMAGE"))) {
        printf("ERR MEDIAW VIDEO|IMAGE <byte> <crc32 esadecimale>\n");
        return;
    }
    const pdx_media_t m = !strcasecmp(argv[1], "VIDEO") ? PDX_MEDIA_ES_USR : PDX_MEDIA_IMG_USR;
    const uint32_t total = (uint32_t)strtoul(argv[2], NULL, 10), crc_want = (uint32_t)strtoul(argv[3], NULL, 16);
    const char *e = pdx_media_write_begin(m, total);
    if (e) { printf("ERR %s\n", e); return; }
    static uint8_t blk[2048];
    uint32_t pos = 0, crc = 0;
    printf("READY\n");
    fflush(stdout);
    while (pos < total && !e) {
        const uint32_t want = total - pos < sizeof(blk) ? total - pos : sizeof(blk);
        uint32_t got = 0;
        while (got < want) {
            const int k = uart_read_bytes(UART_NUM_0, blk + got, want - got, pdMS_TO_TICKS(5000));
            if (k <= 0) { e = "tempo scaduto in ricezione"; break; }
            got += (uint32_t)k;
        }
        if (e) break;
        crc = esp_rom_crc32_le(crc, blk, want);
        e = pdx_media_write(blk, want);
        pos += want;
        if (!e) { printf("ACK %u\n", (unsigned)pos); fflush(stdout); }
    }
    if (!e && crc != crc_want) e = "CRC del file diverso";
    const char *e2 = pdx_media_write_end();
    if (!e) e = e2;
    if (e) {
        pdx_media_erase_user(m);                     /* un file a metà non deve restare in flash */
        printf("ERR %s\n", e);
        return;
    }
    char d[96];
    pdx_media_describe(m, d, sizeof(d));
    printf("OK caricato: %s\n", d);
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
        printf("TEMP chip %.1f C (sensore interno)\n", pdx_temp_read());
        printf("CPU %.3f MHz misurati (atteso 240)\n", (double)(c1 - c0) / (double)(t1 - t0));
        printf("RAM interna libera %u KB (blocco DMA max %u KB), PSRAM libera %u KB\n",
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
               (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) >> 10),
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));

    } else if (!strcasecmp(argv[0], "CW")) {                 /* CW <MHz> <amp> <s> */
        if (argc != 4) { printf("ERR argomenti\n"); return; }
        const int amp = pdx_parse_amp(argv[2]);
        const uint32_t secs = (uint32_t)atoi(argv[3]);
        if (!tx_common(argv[1], amp, secs, &fkhz)) return;
        printf("TX ON CW %.3f MHz (LO %.3f Hz) amp %d per %u s\n", fkhz / 1000.0, pdx_rf_lo_hz(), amp, (unsigned)secs);
        tx_start_note();
        tx_end_note(pdx_rf_tx_cw(amp, secs * 1000u, tx_stop_hw), false);

    } else if (!strcasecmp(argv[0], "TONE")) {               /* TONE <MHz> <off_kHz> <amp> <s> [fs_kHz [mode]] */
        if (argc < 5 || argc > 7) { printf("ERR argomenti\n"); return; }
        const int32_t off_hz = (int32_t)(atof(argv[2]) * 1000.0);
        const int amp = pdx_parse_amp(argv[3]);
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
        const int amp = pdx_parse_amp(argv[3]);
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
        const int amp = pdx_parse_amp(argv[2]);
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

    } else if (!strcasecmp(argv[0], "MEDIA")) {               /* immagini e video in flash */
        static const char *const names[] = {"monoscopio standard   ", "video personalizzato  ", "sfondo PiP standard   ",
                                            "immagine personalizzata"};
        for (int m = 0; m < 4; m++) {
            char d[96];
            pdx_media_describe((pdx_media_t)m, d, sizeof(d));
            printf("MEDIA %s %s\n", names[m], d);
        }

    } else if (!strcasecmp(argv[0], "MEDIAERASE")) {          /* MEDIAERASE VIDEO|IMAGE: solo le aree personalizzate */
        const pdx_media_t m = argc == 2 && !strcasecmp(argv[1], "VIDEO") ? PDX_MEDIA_ES_USR
                            : argc == 2 && !strcasecmp(argv[1], "IMAGE") ? PDX_MEDIA_IMG_USR : PDX_MEDIA_ES_STD;
        const char *e = m == PDX_MEDIA_ES_STD ? "MEDIAERASE VIDEO|IMAGE (il monoscopio standard non si cancella)"
                                              : pdx_media_erase_user(m);
        printf(e ? "ERR %s\n" : "OK cancellato%s\n", e ? e : "");

    } else if (!strcasecmp(argv[0], "MEDIAW")) {              /* MEDIAW VIDEO|IMAGE <byte> <crc32>: caricamento binario */
        media_upload(argc, argv);

    } else if (!strcasecmp(argv[0], "TSDUMP")) {              /* TSDUMP <pacchetti>: TS del monoscopio standard */
        if (argc != 2) { printf("ERR TSDUMP <pacchetti>\n"); return; }
        const uint32_t n = (uint32_t)atoi(argv[1]);
        const char *why;
        const uint8_t *es = pdx_media_map(PDX_MEDIA_ES_STD, &why);
        if (!es) { printf("ERR monoscopio standard: %s\n", why); return; }
        pdx_s2_t s2;
        if (pdx_s2_init(&s2, "3/4", false, true)) { printf("ERR s2\n"); return; }
        const double rate = pdx_s2_ts_bitrate(&s2, pdx_cfg_sr(&cfg));
        pdx_s2_free(&s2);
        static pdx_mux_t mux;
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

    } else if (!strcasecmp(argv[0], "CAMDUMP") || !strcasecmp(argv[0], "PIPDUMP") || !strcasecmp(argv[0], "IMGDUMP")) {
        /* <modo>DUMP <pacchetti>: TS di un modo dal vivo con la configurazione attuale, senza RF */
        if (argc != 2) { printf("ERR %s <pacchetti>\n", argv[0]); return; }
        pdx_live_cfg_t lc;
        const pdx_live_video_t v = toupper((unsigned char)argv[0][0]) == 'C' ? PDX_LV_CAM
                                 : toupper((unsigned char)argv[0][0]) == 'P' ? PDX_LV_PIP : PDX_LV_IMG;
        live_cfg_from(&lc, v, cfg.freq_khz, cfg.amp, cfg.fec);
        printf("%s %u pacchetti\n", argv[0], (unsigned)atoi(argv[1]));
        const char *e = pdx_live_dump(&lc, (uint32_t)atoi(argv[1]));
        if (e) printf("ERR %s\n", e);

    } else if (!strcasecmp(argv[0], "ESPLAY")) {              /* ESPLAY <MHz> <amp> <s> [fec] [short] [nopilots] [modo] */
        if (argc < 4) { printf("ERR ESPLAY <MHz> <amp> <s> [fec] [short] [nopilots] [cam|pip|img|live|user]\n"); return; }
        pdx_live_cfg_t lc;
        live_cfg_from(&lc, PDX_LV_ES, 0, pdx_parse_amp(argv[2]), "3/4");
        for (int i = 4; i < argc; i++) {
            if (!strcasecmp(argv[i], "short")) lc.shortf = true;
            else if (!strcasecmp(argv[i], "nopilots")) lc.pilots = false;
            else if (!strcasecmp(argv[i], "cam")) lc.video = PDX_LV_CAM;
            else if (!strcasecmp(argv[i], "pip")) lc.video = PDX_LV_PIP;
            else if (!strcasecmp(argv[i], "img")) { lc.video = PDX_LV_IMG; lc.bg = pdx_media_map(PDX_MEDIA_IMG_USR, NULL); }
            else if (!strcasecmp(argv[i], "live")) { lc.video = PDX_LV_IMG; lc.bg = pdx_media_map(PDX_MEDIA_IMG_STD, NULL); lc.ovl_full = true; }
            else if (!strcasecmp(argv[i], "user")) lc.es = pdx_media_map(PDX_MEDIA_ES_USR, NULL);
            else if (isdigit((unsigned char)argv[i][0]) && toupper((unsigned char)argv[i][strlen(argv[i]) - 1]) == 'K') {
                const uint32_t sr = (uint32_t)atoi(argv[i]) * 1000u;     /* "500k": symbol rate */
                if (sr < PDX_RF_SR_MIN || sr > PDX_RF_SR_MAX) { printf("ERR SR %u..%u kS\n", (unsigned)(PDX_RF_SR_MIN / 1000u),
                                                                       (unsigned)(PDX_RF_SR_MAX / 1000u)); return; }
                lc.sr = sr;
            }
            else lc.fec = argv[i];
        }
        const uint32_t secs = (uint32_t)atoi(argv[3]);
        if (!tx_common(argv[1], lc.amp, secs, &fkhz)) return;
        lc.freq_khz = pdx_cfg_final_khz(&cfg, fkhz, NULL);
        lc.provider = pdx_cfg_provider(&cfg, fkhz);
        printf("TX ON ESPLAY %.3f MHz SR %u FEC %s %s %s, amp %d per %u s, video: %s\n", fkhz / 1000.0, (unsigned)lc.sr,
               lc.fec, lc.shortf ? "corti" : "normali", lc.pilots ? "pilot" : "senza pilot", lc.amp, (unsigned)secs,
               pdx_live_video_name(lc.video));
        tx_start_note();
        const char *why;
        const uint32_t ms = pdx_live_play(&lc, secs * 1000u, tx_stop_hw, &why);
        if (why) printf("ERR %s\n", why);
        tx_end_note(ms, true);

    } else if (!strcasecmp(argv[0], "RXCAP")) {               /* RXCAP <MHz> [guadagno 0..127]: Parrot P1a, cattura grezza */
        if (argc < 2) { printf("ERR RXCAP <MHz LO> [guadagno 0..127]\n"); return; }
        const double mhz = atof(argv[1]);
        const unsigned gain = argc >= 3 ? (unsigned)atoi(argv[2]) : PDX_RX_GAIN_DEF;
        if (mhz < 2210.0 || mhz > 2790.0 || gain > 127) { printf("ERR LO 2210..2790 MHz, guadagno 0..127\n"); return; }
        pdx_web_set_mode("ricezione");
        pdx_web_stop();
        pdx_web_mark_radio_dirty();                           /* la prossima TX riparte da un riavvio */
        const int64_t t0 = esp_timer_get_time();
        const char *e = pdx_rx_start((uint32_t)(mhz * 1e6 + 0.5), gain);
        if (e) { printf("ERR RX %s\n", e); return; }
        const uint32_t t_tune = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        const uint32_t *d;
        const uint32_t w = pdx_rx_capture(PDX_RX_PAIRS, &d);
        unsigned rf, bb;
        pdx_rx_gains(&rf, &bb);
        printf("RXCAP LO %u Hz fs %u guadagno %u (rf %u bb %u) sintonia %u ms, coppie scritte %u/%u\n",
               (unsigned)pdx_rx_lo_hz(), (unsigned)PDX_RX_FS_HZ, gain, rf, bb, (unsigned)t_tune, (unsigned)w,
               (unsigned)PDX_RX_PAIRS);
        for (uint32_t i = 0; i < PDX_RX_PAIRS; i += 64) {
            printf("R ");
            for (uint32_t k = 0; k < 64; k++) printf("%08" PRIx32, d[i + k]);
            printf("\n");
        }
        printf("RXCAP fine (la prossima TX riavvia la scheda per riportare la radio allo stato del modulatore)\n");

    } else if (!strcasecmp(argv[0], "RXSTREAM")) {           /* RXSTREAM <MHz> <guadagno> <ms> [decim 8|16] [ordine 2|3] [shift] */
        if (argc < 4) { printf("ERR RXSTREAM <MHz> <guadagno> <ms> [decim 8|16] [ordine 2|3] [shift]\n"); return; }
        const double mhz = atof(argv[1]);
        const unsigned gain = (unsigned)atoi(argv[2]), ms = (unsigned)atoi(argv[3]);
        const unsigned decim = argc >= 5 ? (unsigned)atoi(argv[4]) : 16, order = argc >= 6 ? (unsigned)atoi(argv[5]) : 3;
        const unsigned shift = argc >= 7 ? (unsigned)atoi(argv[6]) : decim == 16 ? (order == 3 ? 8u : 4u) : (order == 3 ? 3u : 2u);
        if (mhz < 2210.0 || mhz > 2790.0 || gain > 127) { printf("ERR LO 2210..2790 MHz, guadagno 0..127\n"); return; }
        const uint32_t fs = PDX_RX_FS_HZ / (decim ? decim : 16);
        const uint32_t n = (uint32_t)((uint64_t)ms * fs / 1000u);
        if (!rx_buf) rx_buf = heap_caps_malloc(RX_BUF_BYTES, MALLOC_CAP_SPIRAM);
        if (!rx_buf) { printf("ERR memoria PSRAM\n"); return; }
        if (!n || 2u * n + 8192u > RX_BUF_BYTES) { printf("ERR durata: al massimo %u ms a %u S/s\n", (unsigned)(RX_BUF_BYTES / 2u * 1000u / fs), (unsigned)fs); return; }
        pdx_web_set_mode("ricezione");
        pdx_web_stop();
        pdx_web_mark_radio_dirty();
        const char *e = pdx_rx_start((uint32_t)(mhz * 1e6 + 0.5), gain);
        if (e) { printf("ERR RX %s\n", e); return; }
        printf("RXSTREAM LO %u Hz, %u ms a %u S/s (CIC%u /%u, shift %u)\n", (unsigned)pdx_rx_lo_hz(), ms, (unsigned)fs,
               order, decim, shift);
        fflush(stdout);
        uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(100));
        pdx_rx_stream_stats_t st;
        e = pdx_rx_stream((int8_t *)rx_buf, n, decim, order, shift, &st);
        if (e) {
            extern uint32_t rx_diag[12];
            printf("ERR %s | blocchi riusciti %u, elaborazione max %u cicli | start %u wi %u wsw %u banco %u segnaposto %u | parole %08x %08x %08x %08x | indice dopo %u | banco nuovo segnaposto %u\n", e,
                   (unsigned)(rx_diag[4] & 0xFFFFu), (unsigned)rx_diag[11], (unsigned)rx_diag[0], (unsigned)rx_diag[1], (unsigned)rx_diag[2], (unsigned)rx_diag[3], (unsigned)(rx_diag[4] >> 16), (unsigned)rx_diag[5],
                   (unsigned)rx_diag[6], (unsigned)rx_diag[7], (unsigned)rx_diag[8], (unsigned)rx_diag[9], (unsigned)rx_diag[10]);
            return;
        }
        rx_len = 2u * st.samples_out;
        rx_fs = fs;
        rx_lo = pdx_rx_lo_hz();
        printf("RXSTREAM fine: %u coppie lette, %u campioni, %.2f cicli/coppia, motore oltre il lettore %u volte, "
               "rms %.1f, saturati %u, ritardo max %u coppie\n", (unsigned)st.pairs_in, (unsigned)st.samples_out,
               st.pairs_in ? (double)st.cycles / st.pairs_in : 0.0, (unsigned)st.overruns, (double)st.rms,
               (unsigned)st.clipped, (unsigned)st.max_lag);
        printf("RXSTREAM primi campioni (I,Q):");
        for (int k = 0; k < 24; k++) printf(" %d,%d", (int)(int8_t)rx_buf[1000 + 2 * k], (int)(int8_t)rx_buf[1001 + 2 * k]);
        printf("\n");

    } else if (!strcasecmp(argv[0], "RXBANK")) {             /* RXBANK <valore selezione> <us>: quali banchi scrive il motore */
        extern void pdx_rx_banktest(uint32_t, uint32_t, uint32_t[4], uint32_t *, uint32_t *);
        if (argc < 3) { printf("ERR RXBANK <sel> <us>\n"); return; }
        pdx_web_stop();
        pdx_web_mark_radio_dirty();
        const char *e = pdx_rx_start(2375000000u, PDX_RX_GAIN_DEF);
        if (e) { printf("ERR RX %s\n", e); return; }
        uint32_t c[4], i0, i1;
        pdx_rx_banktest((uint32_t)atoi(argv[1]), (uint32_t)atoi(argv[2]), c, &i0, &i1);
        printf("RXBANK sel %s: scritte banco A(0x3FCC) %u, banco B(0x3FCD) %u, indice %u -> %u\n", argv[1],
               (unsigned)c[1], (unsigned)c[2], (unsigned)(i0 & 16383u), (unsigned)(i1 & 16383u));
    } else if (!strcasecmp(argv[0], "RXSWITCH")) {           /* RXSWITCH <usA> <usB>: cambio di banco durante la corsa */
        extern void pdx_rx_switchtest(uint32_t, uint32_t, uint32_t[8]);
        if (argc < 3) { printf("ERR RXSWITCH <usA> <usB>\n"); return; }
        pdx_web_stop();
        pdx_web_mark_radio_dirty();
        const char *e = pdx_rx_start(2375000000u, PDX_RX_GAIN_DEF);
        if (e) { printf("ERR RX %s\n", e); return; }
        uint32_t o[8];
        pdx_rx_switchtest((uint32_t)atoi(argv[1]), (uint32_t)atoi(argv[2]), o);
        printf("RXSWITCH indice: prima del cambio %u, subito dopo %u, dopo 5 us %u, alla fine %u | A: %u coppie (da %u a %u) | "
               "B: %u coppie (da %u a %u)\n", (unsigned)(o[0] & 16383u), (unsigned)(o[1] & 16383u), (unsigned)(o[2] & 16383u),
               (unsigned)(o[3] & 16383u), (unsigned)(o[4] & 0xFFFFu), (unsigned)(o[4] >> 16), (unsigned)o[5],
               (unsigned)(o[6] & 0xFFFFu), (unsigned)(o[6] >> 16), (unsigned)o[7]);
    } else if (!strcasecmp(argv[0], "RXBENCH")) {            /* RXBENCH <decim> <ordine>: cicli per coppia del decimatore */
        extern float pdx_rx_bench(unsigned, unsigned, uint32_t);
        if (argc < 3) { printf("ERR RXBENCH <decim 8|16> <ordine 2|3>\n"); return; }
        pdx_web_stop();
        printf("RXBENCH decim %s ordine %s: %.2f cicli/coppia (limite 15,0 a 16 Msps)\n", argv[1], argv[2],
               (double)pdx_rx_bench((unsigned)atoi(argv[1]), (unsigned)atoi(argv[2]), 20));
    } else if (!strcasecmp(argv[0], "RXDUMP")) {             /* cattura in binario a 230400 baud, con CRC32 (a 460800 la CH340 perde byte) */
        if (!rx_len) { printf("ERR nessuna cattura\n"); return; }
        const uint32_t crc = esp_rom_crc32_le(0, rx_buf, rx_len);
        printf("RXDUMP %u %08" PRIx32 " %u %u\n", (unsigned)rx_len, crc, (unsigned)rx_fs, (unsigned)rx_lo);
        fflush(stdout);
        uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(200));
        /* RXDUMP [primo blocco] [numero di blocchi]: blocchi da 4 KB, ognuno A5 5A, indice (u32), lunghezza (u16),
         * dati, CRC32 (u32): il PC richiede solo quelli rovinati */
        const uint32_t nblk = (rx_len + 4095u) / 4096u;
        const uint32_t b0 = argc >= 2 ? (uint32_t)atoi(argv[1]) : 0, bn = argc >= 3 ? (uint32_t)atoi(argv[2]) : nblk;
        uart_set_baudrate(UART_NUM_0, 230400);
        vTaskDelay(pdMS_TO_TICKS(800));                       /* il PC cambia velocità */
        for (uint32_t b = b0; b < b0 + bn && b < nblk; b++) {
            const uint32_t off = b * 4096u, k = rx_len - off < 4096u ? rx_len - off : 4096u;
            const uint32_t c = esp_rom_crc32_le(0, rx_buf + off, k);
            const uint8_t h[8] = {0xA5, 0x5A, (uint8_t)b, (uint8_t)(b >> 8), (uint8_t)(b >> 16), (uint8_t)(b >> 24),
                                  (uint8_t)k, (uint8_t)(k >> 8)};
            uart_write_bytes(UART_NUM_0, (const char *)h, 8);
            uart_write_bytes(UART_NUM_0, (const char *)rx_buf + off, k);
            uart_write_bytes(UART_NUM_0, (const char *)&c, 4);
        }
        uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(120000));
        vTaskDelay(pdMS_TO_TICKS(300));
        uart_set_baudrate(UART_NUM_0, 115200);
        vTaskDelay(pdMS_TO_TICKS(100));
        printf("\nRXDUMP fine\n");

    } else if (!strcasecmp(argv[0], "LEDTEST")) {            /* scala dei colori del LED senza TX: da -30 a 0 dB */
        for (int db = -30; db <= 0; db += 2) {
            printf("LED %d dB\n", db);
            fflush(stdout);
            pdx_led_tx(db);
            vTaskDelay(pdMS_TO_TICKS(450));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        pdx_led_idle();
        printf("OK LED di nuovo in configurazione (blu)\n");

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
    printf("\n=== ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - %s ===\n", FW_VERSION);
    pdx_rf_init();
    printf("PHY pronta, LO %.3f Hz\n", pdx_rf_lo_hz());
    printf(pdx_cfg_load(&cfg) ? "configurazione caricata\n" : "nessuna configurazione salvata: valori predefiniti\n");
    pdx_cfg_print(&cfg);
    pdx_web_init(&cfg, FW_VERSION);
    pdx_led_set_mode(cfg.led);
    pdx_led_init();
    pdx_temp_init();
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
