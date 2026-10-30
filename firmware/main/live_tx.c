/* Beacon DATV 13 cm - IZ7PDX
 * Catena del TS generato sull'ESP (vedi live_tx.h). Durante la TX il core 0 ha gli interrupt mascherati e il tick di
 * FreeRTOS è fermo: il produttore sul core 1 aspetta con esp_rom_delay_us, mai con vTaskDelay. */
#include "live_tx.h"

#include <stdio.h>
#include <string.h>
#include "dvbs2_enc.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sym_player.h"
#include "ts_mux.h"

#define ES_MAP_SIZE 0x200000u

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
} live_t;

static const void *es_map;
static esp_partition_mmap_handle_t es_handle;

static const uint8_t *es_open(void)
{
    if (es_map) return es_map;
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "symbols");
    if (!p || esp_partition_mmap(p, PDX_ES_OFFSET, ES_MAP_SIZE, ESP_PARTITION_MMAP_DATA, &es_map, &es_handle) != ESP_OK)
        return NULL;
    return es_map;
}

const uint8_t *pdx_live_es_data(void) { return es_open(); }

bool pdx_live_es_info(char *label, size_t n, uint32_t *units, double *loop_s)
{
    const pdx_es_header_t *h = (const pdx_es_header_t *)es_open();
    if (!h || memcmp(h->magic, "PDXES001", 8)) return false;
    snprintf(label, n, "%.35s", h->label);
    *units = h->nunits;
    *loop_s = h->loop_ticks / 90000.0;
    return true;
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
    while (L->run && produce_frame(L)) {}
    L->done = true;
    vTaskDelete(NULL);
}

uint32_t pdx_live_play(const pdx_live_cfg_t *c, uint32_t ms, pdx_rf_abort_fn abort_fn, const char **why)
{
    *why = NULL;
    const uint8_t *es = es_open();
    if (!es) { *why = "partizione o mmap del file ES"; return 0; }
    live_t *L = heap_caps_calloc(1, sizeof(live_t), MALLOC_CAP_SPIRAM);
    if (!L) { *why = "memoria"; return 0; }
    const char *e = pdx_s2_init(&L->s2, c->fec, c->shortf, c->pilots);
    if (e) { *why = e; heap_caps_free(L); return 0; }
    const double rate = pdx_s2_ts_bitrate(&L->s2, c->sr);
    e = pdx_mux_init(&L->mux, es, rate, c->service, c->provider);
    L->field = heap_caps_malloc(L->s2.dfl / 8u, MALLOC_CAP_SPIRAM);
    L->tmp = heap_caps_calloc(1, L->s2.nsym / 4u + 2u, MALLOC_CAP_SPIRAM);
    uint32_t done_ms = 0;
    if (e || !L->field || !L->tmp) {
        *why = e ? e : "memoria";
    } else {
        L->rb = pdx_sym_ring();
        L->rb->w = L->rb->r = 0;
        L->pkt_pos = 188u;
        L->run = true;
        xTaskCreatePinnedToCore(producer, "live_s2", 6144, L, 10, NULL, 1);
        while (L->rb->w < L->rb->size / 2u && !L->done) vTaskDelay(1);   /* metà buffer pieno prima di partire */
        printf("LIVE TS %.0f bit/s, PLFRAME %u simboli, DATA FIELD %u bit, %u frame pronti\n", rate,
               (unsigned)L->s2.nsym, (unsigned)L->s2.dfl, (unsigned)L->frames);
        fflush(stdout);
        done_ms = pdx_rf_tx_qpsk_ring(c->amp, c->sr, c->dci, c->dcq, ms, L->rb, abort_fn);
        L->run = false;
        while (!L->done) vTaskDelay(1);
        const pdx_mux_t *m = &L->mux;
        printf("LIVE buffer simboli vuoto %u volte\n", (unsigned)pdx_rf_last_stats()->underruns);
        printf("DIAG campioni in ritardo per fase: altro %u, mux %u, codifica %u, attesa %u, copia %u\n",
               (unsigned)pdx_diag_late[0], (unsigned)pdx_diag_late[1], (unsigned)pdx_diag_late[2],
               (unsigned)pdx_diag_late[3], (unsigned)pdx_diag_late[4]);
        printf("LIVE %u PLFRAME, codifica max %u us, attesa spazio %u ms; TS: video %u, audio %u, nulli %u pacchetti, "
               "PES in ritardo %u, giri del loop video %u\n", (unsigned)L->frames, (unsigned)L->enc_max_us,
               (unsigned)(L->wait_us / 1000u), (unsigned)m->stats_video, (unsigned)m->stats_audio,
               (unsigned)m->stats_null, (unsigned)m->late, (unsigned)m->v.loop);
    }
    heap_caps_free(L->field);
    heap_caps_free(L->tmp);
    pdx_s2_free(&L->s2);
    heap_caps_free(L);
    return done_ms;
}
