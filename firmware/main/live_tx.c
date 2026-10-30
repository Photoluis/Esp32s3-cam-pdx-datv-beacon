/* Beacon DATV 13 cm - IZ7PDX
 * Catena del TS generato sull'ESP (vedi live_tx.h). Durante la TX il core 0 ha gli interrupt mascherati e il tick di
 * FreeRTOS è fermo: il produttore sul core 1 aspetta con esp_rom_delay_us, mai con vTaskDelay. */
#include "live_tx.h"

#include <stdio.h>
#include <string.h>
#include "cam_live.h"
#include "dvbs2_enc.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media_store.h"
#include "overlay.h"
#include "sym_player.h"
#include "ts_mux.h"

typedef struct {
    pdx_s2_t s2;
    pdx_mux_t mux;
    pdx_rf_ring_t *rb;
    uint8_t *field, *tmp;                /* DATA FIELD; simboli di un PLFRAME (più quelli rimasti dal precedente) */
    uint8_t pkt[188];
    uint32_t pkt_pos;                    /* byte del pacchetto corrente già usati (188 = da prendere) */
    uint8_t prev_crc;
    uint8_t carry_byte, carry_n;         /* simboli che non riempivano un byte alla fine del frame precedente */
    volatile bool run, done;
    uint32_t frames, enc_max_us, wait_us;
    /* video dal vivo */
    const pdx_live_cfg_t *cfg;
    int fps, kbps;
    uint8_t *vbuf[PDX_VQ_LEN];
    uint32_t cam_frames, cam_dropped, cam_enc_max_us, cam_enc_sum_us, cam_bytes;
    const char *cam_err;
} live_t;

#define VBUF_CAP (64u * 1024u)
#define CAM_DELAY_90K 90000u             /* PTS di un fotogramma = tempo del multiplexer + 1 s */

static const char *const VIDEO_NAMES[] = {"monoscopio", "camera", "camera in PiP", "immagine fissa"};
const char *pdx_live_video_name(pdx_live_video_t v) { return v <= PDX_LV_IMG ? VIDEO_NAMES[v] : "?"; }

static bool live_video(const pdx_live_cfg_t *c) { return c->video != PDX_LV_ES; }

static void live_overlay(uint8_t *yuyv, int w, int h, void *ctx)
{
    live_t *L = ctx;
    char s[48];
    const uint32_t t = (uint32_t)((pdx_mux_now_90k(&L->mux) - L->mux.t0_90k) / 90000u);   /* dall'inizio del segmento */
    if (L->cfg->ovl_full) {
        pdx_ovl_text(yuyv, w, h, 6, 4, L->cfg->service, 2);
        snprintf(s, sizeof(s), "%.3f MHz  DVB-S2 %s", L->cfg->freq_khz / 1000.0, L->cfg->fec);
        pdx_ovl_text(yuyv, w, h, 6, 40, s, 1);
    }
    static const char *const tag[] = {"", "CAM", "PIP", "IMG"};
    snprintf(s, sizeof(s), "%s %05u  T %02u:%02u", tag[L->cfg->video], (unsigned)L->cam_frames, (unsigned)(t / 60u),
             (unsigned)(t % 60u));
    pdx_ovl_text(yuyv, w, h, 6, h - 20, s, 1);
    /* barra che avanza di un passo per fotogramma: un fotogramma perso si vede come un salto */
    const int x = 6 + (int)((L->cam_frames * 12u) % (unsigned)(w - 24));
    pdx_ovl_box(yuyv, w, h, 0, h - 26, w, 4, 16, 128, 128);
    pdx_ovl_box(yuyv, w, h, x, h - 26, 12, 4, 210, 16, 146);
}

/* un fotogramma dal vivo nella coda del multiplexer; false se non c'era posto o è fallito */
static bool video_step(live_t *L)
{
    if (!pdx_mux_vq_space(&L->mux)) return false;
    pdx_vq_entry_t *e = pdx_mux_vq_slot(&L->mux);
    e->buf = L->vbuf[L->mux.vq_w % PDX_VQ_LEN];
    uint32_t tc = 0, te = 0;
    const uint8_t *bg = L->cfg->bg ? L->cfg->bg + PDX_IMG_HDR : NULL;
    const uint32_t len = pdx_cam_live_frame((pdx_live_src_t)(L->cfg->video - 1), bg, e->buf, VBUF_CAP, live_overlay, L,
                                            &tc, &te);
    if (!len) { L->cam_dropped++; return false; }
    e->len = len;
    e->pts = pdx_mux_now_90k(&L->mux) + CAM_DELAY_90K;
    pdx_mux_vq_commit(&L->mux);
    L->cam_frames++;
    L->cam_bytes += len;
    L->cam_enc_sum_us += te;
    if (te > L->cam_enc_max_us) L->cam_enc_max_us = te;
    return true;
}

/* bit/s dei flussi del file ES che vanno nel TS (solo audio se il video è dal vivo) */
static double es_content_bps(const uint8_t *es, bool with_video)
{
    const pdx_es_header_t *h = (const pdx_es_header_t *)es;
    const pdx_es_unit_t *u = (const pdx_es_unit_t *)(es + 64);
    double bytes = 0;
    for (uint32_t i = 0; i < h->nunits; i++)
        if (with_video || u[i].kind == 1) bytes += u[i].len + 14.0;            /* più l'intestazione PES */
    return bytes * 8.0 * 188.0 / 184.0 / (h->loop_ticks / 90000.0);
}

/* controlli comuni a TX e diagnosi: sorgenti presenti, bitrate compatibile, parametri dell'encoder */
static const char *prepare(live_t *L, const pdx_live_cfg_t *c, double rate)
{
    if (!c->es) return "file ES (audio) assente";
    if (c->video >= PDX_LV_PIP && !c->bg) return "immagine di sfondo assente";
    const double need = es_content_bps(c->es, !live_video(c));
    static char msg[96];
    if (need > rate * 0.92) {
        snprintf(msg, sizeof(msg), "il file ES richiede %.0f kbit/s, il TS con questo FEC ne porta %.0f", need / 1000.0,
                 rate / 1000.0);
        return msg;
    }
    L->cfg = c;
    if (live_video(c)) {
        L->fps = c->cam_fps ? c->cam_fps : 6;
        /* ciò che resta dopo audio, PSI/PCR e un margine per le immagini I più grandi */
        int k = c->cam_kbps ? c->cam_kbps : (int)((rate - need - 30000.0) * 0.75 / 1000.0);
        L->kbps = k > 300 ? 300 : k;
        if (L->kbps < 60) return "bitrate troppo basso per il video dal vivo";
        for (int i = 0; i < PDX_VQ_LEN; i++)
            if (!(L->vbuf[i] = heap_caps_malloc(VBUF_CAP, MALLOC_CAP_SPIRAM))) return "memoria";
    }
    return pdx_mux_init(&L->mux, c->es, rate, c->service, c->provider, live_video(c));
}

/* un DATA FIELD dal flusso dei pacchetti con il CRC-8 al posto del sync byte (5.1.4); SYNCD = bit fino al primo pacchetto */
static uint16_t next_field(live_t *L)
{
    const uint32_t nb = L->s2.dfl / 8u;
    const uint32_t to_start = (188u - L->pkt_pos) % 188u;
    const uint16_t syncd = to_start < nb ? (uint16_t)(to_start * 8u) : 0xFFFFu;
    uint32_t k = 0;
    while (k < nb) {
        if (L->pkt_pos == 188u) {
            uint8_t raw[188];
            pdx_mux_next(&L->mux, raw);
            L->pkt[0] = L->prev_crc;
            memcpy(L->pkt + 1, raw + 1, 187);
            L->prev_crc = pdx_s2_crc8(raw + 1, 187);
            L->pkt_pos = 0;
        }
        uint32_t n = 188u - L->pkt_pos;
        if (n > nb - k) n = nb - k;
        memcpy(L->field + k, L->pkt + L->pkt_pos, n);
        k += n;
        L->pkt_pos += n;
    }
    return syncd;
}

/* codifica un PLFRAME e lo mette nel buffer circolare; false se fermato mentre aspettava spazio */
static bool produce_frame(live_t *L)
{
    pdx_diag_phase = 1;                                   /* multiplexer + DATA FIELD */
    const uint16_t syncd = next_field(L);
    pdx_diag_phase = 2;                                   /* codifica DVB-S2 */
    const int64_t t0 = esp_timer_get_time();
    L->tmp[0] = L->carry_byte;
    pdx_s2_out_t out = {.buf = L->tmp, .pos = L->carry_n};
    pdx_s2_frame(&L->s2, L->field, syncd, &out);
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > L->enc_max_us) L->enc_max_us = us;
    const uint32_t full = out.pos / 4u;
    pdx_rf_ring_t *rb = L->rb;
    const uint32_t mask = rb->size - 1u;
    pdx_diag_phase = 3;                                   /* attesa di spazio */
    while (rb->size - (rb->w - rb->r) < full) {           /* attesa attiva: il tick è fermo durante la TX */
        if (!L->run) return false;
        esp_rom_delay_us(200);
        L->wait_us += 200;
    }
    pdx_diag_phase = 4;                                   /* copia nel buffer circolare */
    uint32_t w = rb->w;
    for (uint32_t i = 0; i < full; i++) rb->buf[(w + i) & mask] = L->tmp[i];
    __asm__ volatile("memw" ::: "memory");
    rb->w = w + full;
    L->carry_byte = L->tmp[full];
    L->carry_n = (uint8_t)(out.pos & 3u);
    L->frames++;
    pdx_diag_phase = 0;
    return true;
}

static void producer(void *arg)
{
    live_t *L = arg;
    if (!live_video(L->cfg)) {
        while (L->run && produce_frame(L)) {}
    } else {
        /* camera e encoder inizializzati qui: interrupt e task della camera sul core 1 */
        L->cam_err = pdx_cam_live_init(L->fps, L->kbps, L->cfg->video != PDX_LV_IMG);
        const uint32_t need = L->s2.nsym / 4u + 2u;
        while (L->run && !L->cam_err) {
            /* prima il buffer dei simboli pieno (circa 0,4 s), poi un fotogramma (circa 150 ms) */
            while (L->run && L->rb->size - (L->rb->w - L->rb->r) >= need) produce_frame(L);
            if (!L->run) break;
            if (!video_step(L)) esp_rom_delay_us(500);
        }
    }
    L->done = true;
    vTaskDeleteWithCaps(NULL);
}

static void live_free(live_t *L)
{
    for (int i = 0; i < PDX_VQ_LEN; i++) heap_caps_free(L->vbuf[i]);
    heap_caps_free(L->field);
    heap_caps_free(L->tmp);
    pdx_s2_free(&L->s2);
    heap_caps_free(L);
}

uint32_t pdx_live_play(const pdx_live_cfg_t *c, uint32_t ms, pdx_rf_abort_fn abort_fn, const char **why)
{
    *why = NULL;
    live_t *L = heap_caps_calloc(1, sizeof(live_t), MALLOC_CAP_SPIRAM);
    if (!L) { *why = "memoria"; return 0; }
    const char *e = pdx_s2_init(&L->s2, c->fec, c->shortf, c->pilots);
    if (e) { *why = e; heap_caps_free(L); return 0; }
    const double rate = pdx_s2_ts_bitrate(&L->s2, c->sr);
    e = prepare(L, c, rate);
    L->field = heap_caps_malloc(L->s2.dfl / 8u, MALLOC_CAP_SPIRAM);
    L->tmp = heap_caps_calloc(1, L->s2.nsym / 4u + 2u, MALLOC_CAP_SPIRAM);
    if (e || !L->field || !L->tmp) {
        *why = e ? e : "memoria";
        live_free(L);
        return 0;
    }
    L->rb = pdx_sym_ring();
    L->rb->w = L->rb->r = 0;
    L->pkt_pos = 188u;
    L->run = true;
    /* stack ampio (l'encoder H.264 ne vuole circa 16 KB) e in PSRAM, per non consumare RAM interna */
    xTaskCreatePinnedToCoreWithCaps(producer, "live_s2", 16384, L, 10, NULL, 1, MALLOC_CAP_SPIRAM);
    while (L->rb->w < L->rb->size / 2u && !L->done) vTaskDelay(1);   /* metà buffer pieno prima di partire */
    if (L->cam_err) {
        *why = L->cam_err;
        L->run = false;
        while (!L->done) vTaskDelay(1);
        live_free(L);
        return 0;
    }
    printf("LIVE TS %.0f bit/s, PLFRAME %u simboli, DATA FIELD %u bit, %u frame pronti\n", rate, (unsigned)L->s2.nsym,
           (unsigned)L->s2.dfl, (unsigned)L->frames);
    if (live_video(c)) printf("LIVE video %s: H.264 %dx%d, %d fps, %d kbit/s\n", pdx_live_video_name(c->video), PDX_CAM_W,
                              PDX_CAM_H, L->fps, L->kbps);
    fflush(stdout);
    const uint32_t done_ms = pdx_rf_tx_qpsk_ring(c->amp, c->sr, c->dci, c->dcq, ms, L->rb, abort_fn);
    L->run = false;
    while (!L->done) vTaskDelay(1);
    const pdx_mux_t *m = &L->mux;
    printf("LIVE buffer simboli vuoto %u volte\n", (unsigned)pdx_rf_last_stats()->underruns);
    printf("DIAG campioni in ritardo per fase: altro %u, mux %u, codifica %u, attesa %u, copia %u\n",
           (unsigned)pdx_diag_late[0], (unsigned)pdx_diag_late[1], (unsigned)pdx_diag_late[2],
           (unsigned)pdx_diag_late[3], (unsigned)pdx_diag_late[4]);
    printf("LIVE %u PLFRAME, codifica max %u us, attesa spazio %u ms; TS: video %u, audio %u, nulli %u pacchetti, "
           "PES in ritardo %u, giri del loop %u\n", (unsigned)L->frames, (unsigned)L->enc_max_us,
           (unsigned)(L->wait_us / 1000u), (unsigned)m->stats_video, (unsigned)m->stats_audio,
           (unsigned)m->stats_null, (unsigned)m->late, (unsigned)(live_video(c) ? m->a.loop : m->v.loop));
    if (live_video(c))
        printf("LIVE %u fotogrammi (%.1f fps), persi %u, codifica media %u ms max %u ms, %.0f kbit/s video\n",
               (unsigned)L->cam_frames, L->cam_frames * 1000.0 / (done_ms ? done_ms : 1), (unsigned)L->cam_dropped,
               (unsigned)(L->cam_frames ? L->cam_enc_sum_us / L->cam_frames / 1000u : 0),
               (unsigned)(L->cam_enc_max_us / 1000u), L->cam_bytes * 8.0 / (done_ms ? done_ms : 1));
    live_free(L);
    return done_ms;
}

/* ------------------------------------------------------------ diagnosi senza RF: TS di un modo dal vivo */
typedef struct {
    live_t *L;
    uint32_t npk;
    volatile bool done;
} dump_job_t;

static void dump_task(void *arg)
{
    dump_job_t *j = arg;
    live_t *L = j->L;
    L->cam_err = pdx_cam_live_init(L->fps, L->kbps, L->cfg->video != PDX_LV_IMG);
    const double pkt_per_us = L->mux.rate / 1504.0 / 1e6;
    uint32_t sent = 0;
    uint8_t pk[188];
    while (!L->cam_err && sent < j->npk) {
        const int64_t t0 = esp_timer_get_time();
        video_step(L);
        /* i pacchetti che il TS avrebbe trasmesso nel tempo della composizione e della codifica */
        uint32_t n = (uint32_t)((esp_timer_get_time() - t0) * pkt_per_us) + 1u;
        for (; n && sent < j->npk; n--, sent++) {
            pdx_mux_next(&L->mux, pk);
            printf("T ");
            for (int k = 0; k < 188; k++) printf("%02x", pk[k]);
            printf("\n");
        }
    }
    j->done = true;
    vTaskDeleteWithCaps(NULL);
}

const char *pdx_live_dump(const pdx_live_cfg_t *c, uint32_t npk)
{
    if (!live_video(c)) return "solo per i modi dal vivo (camera, PiP, immagine)";
    live_t *L = heap_caps_calloc(1, sizeof(live_t), MALLOC_CAP_SPIRAM);
    if (!L) return "memoria";
    const char *e = pdx_s2_init(&L->s2, c->fec, c->shortf, c->pilots);
    if (!e) e = prepare(L, c, pdx_s2_ts_bitrate(&L->s2, c->sr));
    if (!e) {
        dump_job_t j = {.L = L, .npk = npk};
        xTaskCreatePinnedToCoreWithCaps(dump_task, "live_dump", 16384, &j, 5, NULL, 1, MALLOC_CAP_SPIRAM);
        while (!j.done) vTaskDelay(pdMS_TO_TICKS(10));
        if (L->cam_err) e = L->cam_err;
        printf("DUMP fine: %u fotogrammi, persi %u, codifica media %u ms, %d kbit/s, video %u, audio %u, nulli %u, "
               "ritardo %u\n", (unsigned)L->cam_frames, (unsigned)L->cam_dropped,
               (unsigned)(L->cam_frames ? L->cam_enc_sum_us / L->cam_frames / 1000u : 0), L->kbps,
               (unsigned)L->mux.stats_video, (unsigned)L->mux.stats_audio, (unsigned)L->mux.stats_null,
               (unsigned)L->mux.late);
    }
    live_free(L);
    return e;
}
