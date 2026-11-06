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
#include <math.h>
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
#include "s2rx.h"
#include "esp_rom_crc.h"
#include <ctype.h>

int pdx_s2_selftest(void);                  /* s2_test.c */

#define FW_VERSION "v1.2.0-dev Parrot"
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
#define RX_BUF_BYTES (3920u * 1024u)                  /* 2,1 s a 1 Msps: il resto della PSRAM serve al ricevitore */
static uint8_t *rx_buf;
static uint32_t rx_len, rx_fs, rx_lo;
static bool rx_board = true;                    /* la cattura in memoria viene dal decimatore della scheda */
/* Parrot: ultimo QSO ricevuto, conservato in RTC per la risposta dopo il riavvio (radio pulita per la TX) */
#define PARROT_MAGIC 0x50415254u
typedef struct {
    uint32_t magic;
    char call[16], fec[6];
    float mer, d, foff_hz;
    uint32_t sr;
    uint32_t n;                      /* QSO ricevuti dall'accensione (la memoria RTC resta con i riavvii software) */
    uint32_t crc;
} parrot_state_t;
static bool beacon_pause(const pdx_cfg_t *c, uint32_t s);
RTC_NOINIT_ATTR static parrot_state_t parrot;
static uint32_t parrot_crc(void) { return esp_rom_crc32_le(0, (const uint8_t *)&parrot, offsetof(parrot_state_t, crc)); }
static bool parrot_valid(void) { return parrot.magic == PARROT_MAGIC && parrot.crc == parrot_crc(); }
static char parrot_l1[24], parrot_l2[48], parrot_prov[40];
static void parrot_texts(void)
{
    snprintf(parrot_l1, sizeof(parrot_l1), "QSO %s", parrot.call);
    snprintf(parrot_l2, sizeof(parrot_l2), "MER %.1f dB D%.1f %ukS %s %+.1f kHz", (double)parrot.mer, (double)parrot.d,
             (unsigned)((parrot.sr + 500u) / 1000u), parrot.fec, (double)parrot.foff_hz / 1000.0);
    snprintf(parrot_prov, sizeof(parrot_prov), "QRK %s %.1fdB D%.1f", parrot.call, (double)parrot.mer, (double)parrot.d);
}

/* ambiente del ricevitore DVB-S2 (s2rx.c) */
#define RXDEC_TS_MAX 2000u
static void *rxdec_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void rxdec_free(void *p) { heap_caps_free(p); }
static void rxdec_log(const char *s) { printf("RXDEC %s\n", s); fflush(stdout); }
static uint32_t rxdec_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }static const char *current_line = "";      /* riga del comando in esecuzione, prima della divisione in parole */

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

/* fine di una TX (o del ciclo): la radio è nello stato del modulatore e l'access point acceso subito dopo non riceve
 * bene (06/11: il telefono vede la rete ma "password errata"). Si riavvia e si riparte in configurazione con la radio
 * pulita, senza partenza automatica. */
static void config_after_tx(void)
{
    while (gpio_get_level(BOOT_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));   /* BOOT rilasciato prima del riavvio */
    pdx_led_idle();
    reboot_and_run("CONFIGMODE");
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
    const uint32_t n = pdx_beacon_run(&snap, tx_stop_hw, beacon_pause);
    while (gpio_get_level(BOOT_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));   /* attende il rilascio di BOOT */
    discard_input();
    printf("BEACON fermo dopo %u cicli completi: modo configurazione\n", (unsigned)n);
    config_after_tx();
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
    config_after_tx();
}

/* parametri del TS dal vivo dalla configurazione (sfondo PiP e sovrimpressione come nel beacon) */
static void live_cfg_from(pdx_live_cfg_t *lc, pdx_live_video_t v, uint32_t freq_khz, int amp, const char *fec)
{
    pdx_cam_set_flip(cfg.cam_flip & PDX_CAMFLIP_H, cfg.cam_flip & PDX_CAMFLIP_V);
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

/* ------------------------------------------------------------ Parrot: ascolto, QSO, pause del ciclo */

static uint8_t *parrot_ts, *parrot_ok;          /* pacchetti TS ricostruiti e loro CRC */

static bool parrot_bufs(void)
{
    if (!rx_buf) rx_buf = heap_caps_malloc(RX_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (!parrot_ts) parrot_ts = heap_caps_malloc(188u * RXDEC_TS_MAX, MALLOC_CAP_SPIRAM);
    if (!parrot_ok) parrot_ok = heap_caps_malloc(RXDEC_TS_MAX, MALLOC_CAP_SPIRAM);
    return rx_buf && parrot_ts && parrot_ok;
}

static void parrot_store(const s2rx_report_t *rep)
{
    const uint32_t n = parrot_valid() ? parrot.n + 1u : 1u;
    memset(&parrot, 0, sizeof(parrot));
    parrot.magic = PARROT_MAGIC;
    snprintf(parrot.call, sizeof(parrot.call), "%s", rep->call);
    snprintf(parrot.fec, sizeof(parrot.fec), "%s", rep->fec);
    parrot.mer = rep->mer_db; parrot.d = rep->d_margin_db; parrot.foff_hz = rep->freq_hz; parrot.sr = rep->sr;
    parrot.n = n;
    parrot.crc = parrot_crc();
    parrot_texts();
    printf("PARROT QSO %u: %s | %s | SDT \"%s\"\n", (unsigned)n, parrot_l1, parrot_l2, parrot_prov);
}

/* ultimo QSO per la pagina web (JSON, "null" se nessuno) */
const char *pdx_parrot_json(void)
{
    static char s[160];
    if (!parrot_valid()) return "null";
    snprintf(s, sizeof(s), "{\"n\":%u,\"call\":\"%s\",\"mer\":%.1f,\"d\":%.1f,\"sr\":%u,\"fec\":\"%s\",\"foff\":%.1f}",
             (unsigned)parrot.n, parrot.call, (double)parrot.mer, (double)parrot.d, (unsigned)((parrot.sr + 500u) / 1000u),
             parrot.fec, (double)parrot.foff_hz / 1000.0);
    return s;
}

/* ascolto sulla frequenza del beacon per listen_s secondi: occhiate di 60 ms allo spettro; con un segnale, attesa
 * stab_s, cattura di 2 s e decodifica. 1 = QSO letto (in "parrot"), 0 = nessun QSO, -1 = fermato o errore.
 * Dopo l'ascolto la radio non è più nello stato del modulatore: la TX seguente richiede un riavvio. */
#define PARROT_DET_DB 10.0f   /* soglia del rilevatore (06/11: 8 dB dava falsi allarmi a 8,4 dB; segnali veri 20-27 dB) */
static void parrot_on_call(const char *call)
{
    printf("PARROT nominativo %s acquisito: LED giallo lampeggiante fino alla risposta\n", call);
    fflush(stdout);
    pdx_led_qso();
}

static int parrot_listen(unsigned listen_s, unsigned stab_s)
{
    if (!parrot_bufs()) { printf("ERR memoria\n"); return -1; }
    s2rx_env_t env = {rxdec_alloc, rxdec_free, rxdec_log, rxdec_ms, parrot_on_call};
    s2rx_report_t rep;
    pdx_web_mark_radio_dirty();
    const uint32_t lo = cfg.freq_khz * 1000u;
    const char *e = pdx_rx_start(lo, PDX_RX_GAIN_DEF);
    if (e) { printf("ERR RX %s\n", e); return -1; }
    printf("PARROT ascolto su %.3f MHz per %u s (un tasto o BOOT per fermare)\n", cfg.freq_khz / 1000.0, listen_s);
    fflush(stdout);
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(100));
    uart_flush_input(UART_NUM_0);
    const int64_t t_end = esp_timer_get_time() + (int64_t)listen_s * 1000000;
    uint32_t looks = 0, fails = 0;
    float best_pk = 0, best_fc = 0, best_w = 0;              /* diagnosi (06/11): occhiata migliore sotto la soglia */
    int64_t t_diag = esp_timer_get_time() + 5000000;
    const int64_t t_warm = esp_timer_get_time() + 1500000;
    pdx_rx_stream_stats_t st;
    while (esp_timer_get_time() < t_end) {
        if (stop_requested()) { printf("PARROT ascolto fermato\n"); return -1; }
        if (esp_timer_get_time() > t_diag) {
            printf("PARROT ascolto: %u occhiate (%u errori RX), picco max %.1f dB a %+.1f kHz largo %.0f kHz\n",
                   (unsigned)looks, (unsigned)fails, (double)best_pk, (double)best_fc / 1000.0, (double)best_w / 1000.0);
            fflush(stdout);
            best_pk = 0;
            t_diag = esp_timer_get_time() + 5000000;
        }
        /* radio risintonizzata a ogni occhiata: nelle pause la catena RX perde la configurazione (RF del 04/11) */
        if (pdx_rx_start(lo, PDX_RX_GAIN_DEF) ||
            pdx_rx_stream((int8_t *)rx_buf, 60000u, 16, 3, 8, &st)) { fails++; vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        looks++;
        /* riscaldamento (06/11): nel primo secondo dopo l'accensione della RX lo spettro ha gobbe finte (8-11 dB) */
        if (esp_timer_get_time() < t_warm) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        float fc = 0, w = 0, pk = 0;
        if (!s2rx_detect((const int8_t *)rx_buf, st.samples_out, 1e6f, &env, PARROT_DET_DB, &fc, &w, &pk)) {
            if (pk > best_pk) { best_pk = pk; best_fc = fc; best_w = w; }
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        {   /* seconda occhiata di conferma (06/11): un disturbo a +110 kHz largo 104 kHz faceva perdere attesa e cattura */
            float fc2 = 0, w2 = 0, pk2 = 0;
            const bool ok2 = !pdx_rx_start(lo, PDX_RX_GAIN_DEF) &&
                             !pdx_rx_stream((int8_t *)rx_buf, 60000u, 16, 3, 8, &st) &&
                             s2rx_detect((const int8_t *)rx_buf, st.samples_out, 1e6f, &env, 6.0f, &fc2, &w2, &pk2) &&
                             fabsf(fc2 - fc) < 30e3f && w2 > 0.7f * w && w2 < 1.4f * w;
            if (!ok2) {
                printf("PARROT scartato: %+.1f kHz largo %.0f kHz %.1f dB non confermato (%+.1f kHz %.0f kHz %.1f dB)\n",
                       (double)fc / 1000.0, (double)w / 1000.0, (double)pk,
                       (double)fc2 / 1000.0, (double)w2 / 1000.0, (double)pk2);
                fflush(stdout);
                continue;
            }
        }
        printf("PARROT segnale dopo %u occhiate: %+.1f kHz, largo %.0f kHz, %.1f dB sul rumore; attesa %u s\n",
               (unsigned)looks, (double)fc / 1000.0, (double)w / 1000.0, (double)pk, stab_s);
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(stab_s * 1000u));
        if (pdx_rx_start(lo, PDX_RX_GAIN_DEF) || pdx_rx_stream((int8_t *)rx_buf, 2000000u, 16, 3, 8, &st)) continue;
        rx_len = 2u * st.samples_out; rx_fs = 1000000u; rx_lo = pdx_rx_lo_hz(); rx_board = true;
        s2rx_set_board_capture(true);
        pdx_led_idle();                                      /* decodifica: radio non in ascolto */
        printf("PARROT cattura di 2 s: %u campioni, motore oltre il lettore %u volte; decodifica...\n",
               (unsigned)st.samples_out, (unsigned)st.overruns);
        fflush(stdout);
        s2rx_run((const int8_t *)rx_buf, st.samples_out, 1e6f, 0, &env, &rep, parrot_ts, RXDEC_TS_MAX, parrot_ok);
        if (rep.call[0]) {
            parrot_store(&rep);
            pdx_led_qso();                   /* giallo 4 Hz fino alla risposta (già acceso da on_call a metà decodifica) */
            return 1;
        }
        printf("PARROT nessun nominativo letto: si riprova finché c'è tempo\n");
    }
    printf("PARROT fine dell'ascolto: nessun QSO (%u occhiate)\n", (unsigned)looks);
    return 0;
}

/* pausa del ciclo del beacon: con il Parrot acceso si ascolta per tutta la pausa; poi si riavvia (radio pulita) per
 * rispondere ("PARROTREPLY <s> BEACON", il ciclo riprende dopo la risposta) o per riprendere il ciclo ("BEACON").
 * Restituisce false se la pausa è stata fermata. */
static bool beacon_pause(const pdx_cfg_t *c, uint32_t s)
{
    if (!c->parrot_on) {
        const int64_t end = esp_timer_get_time() + (int64_t)s * 1000000;
        while (esp_timer_get_time() < end) {
            if (stop_requested()) return false;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        return true;
    }
    /* la ricezione parte da una radio pulita: dopo la TX la PHY è nello stato del modulatore (come per l'access point) */
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "PARROTPAUSE %u", (unsigned)s);
    reboot_and_run(cmd);                                     /* non ritorna */
    return false;
}

/* PARROTPAUSE <s>, dopo il riavvio: ascolto per s secondi, poi risposta o ripresa del ciclo (altro riavvio) */
static void parrot_pause_run(uint32_t s)
{
    pdx_web_stop();
    printf("BEACON pausa %u s: ascolto Parrot (Parrot acceso)\n", (unsigned)s);
    const int r = parrot_listen(s, cfg.parrot_stab);
    if (r <= 0) pdx_led_idle();                              /* con un QSO il giallo lampeggiante resta fino al riavvio */
    if (r < 0) {                                             /* fermato con BOOT o dalla seriale */
        while (gpio_get_level(BOOT_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20));
        discard_input();
        printf("BEACON fermo nella pausa: modo configurazione\n");
        reboot_and_run("CONFIGMODE");
    }
    char cmd[40];
    if (r > 0) snprintf(cmd, sizeof(cmd), "PARROTREPLY %u BEACON", (unsigned)cfg.parrot_reply);
    else snprintf(cmd, sizeof(cmd), "BEACON");
    reboot_and_run(cmd);
}

static void handle_cmd(char *line);
/* semaforo: un comando che ha acceso la ricezione (LED verde) torna in attesa (giallo) quando finisce */
static void handle(char *line)
{
    pdx_rx_led_used = false;
    pdx_led_activity();                                       /* verde veloce: comando dalla seriale */
    handle_cmd(line);
    if (pdx_rx_led_used) { pdx_rx_led_used = false; pdx_led_idle(); }
}

static void handle_cmd(char *line)
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
        rx_board = true;
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

    } else if (!strcasecmp(argv[0], "CONFIGMODE")) {        /* dopo il riavvio di fine TX: solo configurazione */
        printf("OK modo configurazione (radio pulita, access point)\n");

    } else if (!strcasecmp(argv[0], "PARROTPAUSE")) {       /* pausa del ciclo con il Parrot (dopo il riavvio) */
        const long s = argc >= 2 ? atol(argv[1]) : 0;
        if (s < 5 || s > 3600) { printf("ERR PARROTPAUSE <5..3600 s> [dry]\n"); return; }
        if (argc >= 3 && !strcasecmp(argv[2], "dry")) {      /* prova: ascolto vero, nessun riavvio e nessuna TX */
            const int r = parrot_listen((unsigned)s, cfg.parrot_stab);
            printf("PARROTPAUSE dry: dopo la pausa %s\n", r > 0 ? "risposta (PARROTREPLY ... BEACON)" : r == 0 ? "ripresa del ciclo (BEACON)" : "configurazione (fermato)");
            return;
        }
        parrot_pause_run((uint32_t)s);

    } else if (!strcasecmp(argv[0], "PARROT") || !strcasecmp(argv[0], "PARROTDEC")) {
        /* PARROT <ascolto_s> <risposta_s> [stabilizzazione_s] [dry]: ascolta sulla frequenza del beacon, cattura 2 s,
         * decodifica, poi riavvia e risponde con QRZ e QRK in sovrimpressione (dry: senza TX).
         * PARROTDEC: solo la decodifica della cattura in memoria (prove senza RF). */
        const bool only_dec = !strcasecmp(argv[0], "PARROTDEC");
        const unsigned listen_s = !only_dec && argc >= 2 ? (unsigned)atoi(argv[1]) : 0;
        const unsigned reply_s = !only_dec && argc >= 3 ? (unsigned)atoi(argv[2]) : 60;
        const unsigned stab_s = !only_dec && argc >= 4 ? (unsigned)atoi(argv[3]) : 3;
        bool dry = false;
        for (int i = 1; i < argc; i++) if (!strcasecmp(argv[i], "dry")) dry = true;
        if (!only_dec && (listen_s < 5 || listen_s > 3600 || reply_s < 10 || reply_s > TX_MAX_S)) {
            printf("ERR PARROT <ascolto 5..3600 s> <risposta 10..%u s> [stabilizzazione s] [dry]\n", (unsigned)TX_MAX_S);
            return;
        }
        if (!parrot_bufs()) { printf("ERR memoria\n"); return; }
        pdx_web_set_mode("Parrot: ascolto");
        pdx_web_stop();
        if (only_dec) {
            if (!rx_len) { printf("ERR nessuna cattura\n"); return; }
            s2rx_env_t env = {rxdec_alloc, rxdec_free, rxdec_log, rxdec_ms, parrot_on_call};
            s2rx_report_t rep;
            s2rx_set_board_capture(rx_board);
            s2rx_run((const int8_t *)rx_buf, rx_len / 2u, (float)rx_fs, 0, &env, &rep, parrot_ts, RXDEC_TS_MAX, parrot_ok);
            pdx_led_idle();
            if (!rep.call[0]) { printf("PARROT nessun nominativo letto\n"); return; }
            parrot_store(&rep);
        } else if (parrot_listen(listen_s, stab_s) <= 0) {
            return;
        }
        if (only_dec || dry) { pdx_led_idle(); printf("PARROT %s: nessuna TX\n", only_dec ? "solo decodifica" : "prova a secco"); return; }
        char c[32];
        snprintf(c, sizeof(c), "PARROTREPLY %u", reply_s);
        reboot_and_run(c);

    } else if (!strcasecmp(argv[0], "PARROTREPLY") || !strcasecmp(argv[0], "PARROTDUMP")) {
        /* PARROTREPLY <s>: risposta in TX (barre + sovrimpressione con QRZ e QRK); PARROTDUMP <pacchetti>: lo stesso TS
         * in esadecimale, senza RF */
        if (!parrot_valid()) { printf("ERR nessun QSO in memoria (PARROT o PARROTDEC)\n"); return; }
        const bool dump = !strcasecmp(argv[0], "PARROTDUMP");
        const uint32_t secs = argc >= 2 ? (uint32_t)atoi(argv[1]) : 60u;
        parrot_texts();
        pdx_live_cfg_t lc;
        live_cfg_from(&lc, PDX_LV_IMG, cfg.freq_khz, cfg.amp, cfg.fec);
        lc.bg = pdx_media_map(PDX_MEDIA_IMG_STD, NULL);              /* barre standard con i dati in sovrimpressione */
        lc.ovl_full = true;
        lc.qrk1 = parrot_l1;
        lc.qrk2 = parrot_l2;
        lc.provider = parrot_prov;
        /* si risponde con il symbol rate della stazione ricevuta (il suo ricevitore è già lì), al massimo 500 kS */
        { static const uint32_t SRS[] = {125000, 250000, 333000, 500000}; uint32_t b = SRS[0];
          for (unsigned i = 0; i < 4; i++) if ((parrot.sr > SRS[i] ? parrot.sr - SRS[i] : SRS[i] - parrot.sr) < (parrot.sr > b ? parrot.sr - b : b - parrot.sr)) b = SRS[i];
          lc.sr = b; }
        if (dump) {
            printf("PARROTDUMP %u pacchetti\n", (unsigned)secs);
            const char *e = pdx_live_dump(&lc, secs);
            if (e) printf("ERR %s\n", e);
            return;
        }
        char fa[16];
        snprintf(fa, sizeof(fa), "%.3f", cfg.freq_khz / 1000.0);
        uint32_t fkhz;
        if (!tx_common(fa, lc.amp, secs, &fkhz)) return;
        lc.freq_khz = pdx_cfg_final_khz(&cfg, fkhz, NULL);
        printf("TX ON PARROT risposta a %s per %u s: %s | %s\n", parrot.call, (unsigned)secs, parrot_l1, parrot_l2);
        tx_start_note();
        const char *why;
        const uint32_t ms = pdx_live_play(&lc, secs * 1000u, tx_stop_hw, &why);
        if (why) printf("ERR %s\n", why);
        /* PARROTREPLY <s> BEACON: risposta data dentro il ciclo del beacon, che riprende subito (la radio è già nello
         * stato del modulatore); se la risposta è stata fermata si va in configurazione come sempre */
        if (argc >= 3 && !strcasecmp(argv[2], "BEACON") && ms + 50u >= secs * 1000u) {
            discard_input();
            printf("TX OFF dopo %u ms: il ciclo del beacon riprende\n", (unsigned)ms);
            pdx_led_idle();
            run_beacon();
            return;
        }
        tx_end_note(ms, true);

    } else if (!strcasecmp(argv[0], "RXLOAD")) {             /* RXLOAD <byte> <fs>: cattura dal PC in PSRAM (prove) */
        if (argc < 3) { printf("ERR RXLOAD <byte> <fs>\n"); return; }
        const uint32_t len = (uint32_t)atoi(argv[1]);
        if (!rx_buf) rx_buf = heap_caps_malloc(RX_BUF_BYTES, MALLOC_CAP_SPIRAM);
        if (!rx_buf || len > RX_BUF_BYTES) { printf("ERR memoria o lunghezza\n"); return; }
        pdx_web_stop();
        printf("RXLOAD pronto a 230400 baud\n");
        fflush(stdout);
        uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(200));
        uart_set_baudrate(UART_NUM_0, 230400);
        static uint8_t *blk;
        if (!blk) blk = heap_caps_malloc(4096 + 12, MALLOC_CAP_SPIRAM);
        if (!blk) { printf("ERR memoria\n"); return; }
        const uint32_t nblk = (len + 4095u) / 4096u;
        uint32_t got = 0;
        int64_t t_last = esp_timer_get_time();
        uint8_t *seen = calloc(nblk, 1);
        while (got < nblk && esp_timer_get_time() - t_last < 20000000) {
            /* cerca A5 5A, poi intestazione, dati e CRC */
            uint8_t c;
            if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100)) != 1 || c != 0xA5) continue;
            if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(100)) != 1 || c != 0x5A) continue;
            if (uart_read_bytes(UART_NUM_0, blk, 6, pdMS_TO_TICKS(200)) != 6) continue;
            const uint32_t idx = blk[0] | blk[1] << 8 | blk[2] << 16 | (uint32_t)blk[3] << 24, k = blk[4] | blk[5] << 8;
            if (idx >= nblk || k > 4096u) { const char n = 'N'; uart_write_bytes(UART_NUM_0, &n, 1); continue; }
            if (uart_read_bytes(UART_NUM_0, blk, k + 4u, pdMS_TO_TICKS(1000)) != (int)(k + 4u)) {
                const char n = 'N'; uart_write_bytes(UART_NUM_0, &n, 1); continue;
            }
            uint32_t crc;
            memcpy(&crc, blk + k, 4);
            if (esp_rom_crc32_le(0, blk, k) != crc) { const char n = 'N'; uart_write_bytes(UART_NUM_0, &n, 1); continue; }
            memcpy(rx_buf + idx * 4096u, blk, k);
            if (seen && !seen[idx]) { seen[idx] = 1; got++; }
            const char a = 'K';
            uart_write_bytes(UART_NUM_0, &a, 1);
            t_last = esp_timer_get_time();
        }
        free(seen);
        uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(200));
        vTaskDelay(pdMS_TO_TICKS(300));
        uart_set_baudrate(UART_NUM_0, 115200);
        vTaskDelay(pdMS_TO_TICKS(100));
        if (got == nblk) { rx_len = len; rx_fs = (uint32_t)atoi(argv[2]); rx_board = false; }
        printf("\nRXLOAD fine: %u/%u blocchi%s\n", (unsigned)got, (unsigned)nblk, got == nblk ? "" : " (incompleto)");

    } else if (!strcasecmp(argv[0], "RXDEC")) {              /* RXDEC [SR]: decodifica DVB-S2 della cattura in PSRAM */
        if (!rx_len) { printf("ERR nessuna cattura\n"); return; }
        static uint8_t *ts;
        static uint8_t *ok;
        if (!ok) ok = heap_caps_malloc(RXDEC_TS_MAX, MALLOC_CAP_SPIRAM);
        if (!ts) ts = heap_caps_malloc(188u * RXDEC_TS_MAX, MALLOC_CAP_SPIRAM);
        if (!ts || !ok) { printf("ERR memoria\n"); return; }
        pdx_web_stop();
        s2rx_env_t env = {rxdec_alloc, rxdec_free, rxdec_log, rxdec_ms, NULL};
        s2rx_report_t rep;
        printf("RXDEC %u campioni a %u S/s, PSRAM libera %u KB\n", (unsigned)(rx_len / 2u), (unsigned)rx_fs,
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u));
        s2rx_set_board_capture(rx_board);
        const uint32_t np = s2rx_run((const int8_t *)rx_buf, rx_len / 2u, (float)rx_fs, argc >= 2 ? (uint32_t)atoi(argv[1]) : 0u,
                                     &env, &rep, ts, RXDEC_TS_MAX, ok);
        printf("RXDEC risultato: rilevato %d SR %u freq %+.1f Hz MER %.1f dB D %.1f frame %d LDPC %d BB %d TS %u (errati %u) "
               "QRZ %s | tempo %u ms (front-end %u, aggancio %u, frame %u) PSRAM libera %u KB\n", rep.detected,
               (unsigned)rep.sr, (double)rep.freq_hz, (double)rep.mer_db, (double)rep.d_margin_db, rep.frames, rep.ldpc_ok,
               rep.bb_ok, (unsigned)np, (unsigned)rep.ts_crc_bad, rep.call[0] ? rep.call : "-", (unsigned)rep.ms_total,
               (unsigned)rep.ms_front, (unsigned)rep.ms_sync, (unsigned)rep.ms_frames,
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u));
    } else if (!strcasecmp(argv[0], "LEDTEST")) {            /* semaforo del LED senza RF: verde, giallo, rosso a 3 livelli */
        printf("LED verde fisso (RX) per 3 s\n"); fflush(stdout);
        pdx_led_rx();
        vTaskDelay(pdMS_TO_TICKS(3000));
        printf("LED giallo fisso (attesa) per 3 s\n"); fflush(stdout);
        pdx_led_idle();
        vTaskDelay(pdMS_TO_TICKS(3000));
        static const int lv[3] = {-40, -20, -5};
        for (int i = 0; i < 3; i++) {
            printf("LED rosso lampeggiante (TX a %d dB) per 4 s\n", lv[i]); fflush(stdout);
            pdx_led_tx(lv[i]);
            vTaskDelay(pdMS_TO_TICKS(4000));
        }
        pdx_led_idle();
        printf("OK LED di nuovo in attesa (giallo)\n");

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
        if (saved) pdx_led_confirm();                         /* giallo lento: conferma */

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
