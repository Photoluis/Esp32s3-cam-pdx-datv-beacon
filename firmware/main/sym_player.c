/* Beacon DATV 13 cm - IZ7PDX
 * Lettore dei simboli: la partizione "symbols" è mappata in memoria; un task sul core 1 copia i byte in un buffer circolare
 * nella RAM interna, il core 0 li modula senza mai passare dalla cache della flash. */
#include "sym_player.h"

#include <stdio.h>
#include <string.h>
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RING_BYTES  32768u          /* 131072 simboli: 0,39 s a 333 kS */
#define FEED_CHUNK  4096u

static const esp_partition_t *part;
static esp_partition_mmap_handle_t map_handle;
static const uint8_t *map_data;     /* intestazione + simboli */
static pdx_sym_header_t header;
static DRAM_ATTR uint8_t ring_buf[RING_BYTES];
static pdx_rf_ring_t ring = {.buf = ring_buf, .size = RING_BYTES};
static volatile bool feed_run;
static TaskHandle_t feed_task;

pdx_rf_ring_t *pdx_sym_ring(void) { return &ring; }

bool pdx_sym_open(pdx_sym_header_t *hdr, const char **why)
{
    if (!part) part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "symbols");
    if (!part) { *why = "partizione symbols assente"; return false; }
    pdx_sym_header_t h;
    if (esp_partition_read(part, 0, &h, sizeof(h)) != ESP_OK) { *why = "lettura fallita"; return false; }
    if (memcmp(h.magic, PDX_SYM_MAGIC, 8)) { *why = "nessun file di simboli"; return false; }
    const uint32_t bytes = h.nsym / 4u;
    if (h.nsym == 0 || (h.nsym & 3u) || sizeof(h) + bytes > part->size) { *why = "lunghezza non valida"; return false; }
    if (h.sr < PDX_RF_SR_MIN || h.sr > PDX_RF_SR_MAX) { *why = "symbol rate non ammesso"; return false; }
    if (map_data) { esp_partition_munmap(map_handle); map_data = NULL; }
    const void *p;
    if (esp_partition_mmap(part, 0, sizeof(h) + bytes, ESP_PARTITION_MMAP_DATA, &p, &map_handle) != ESP_OK) {
        *why = "mmap fallita";
        return false;
    }
    map_data = p;
    if (esp_rom_crc32_le(0, map_data + sizeof(h), bytes) != h.crc32) { *why = "CRC errato"; return false; }
    h.label[sizeof(h.label) - 1] = 0;
    header = h;
    *hdr = h;
    return true;
}

/* core 1: tiene pieno il buffer circolare, ricominciando dall'inizio del loop alla fine del file */
static void feed(void *arg)
{
    const uint8_t *sym = map_data + sizeof(pdx_sym_header_t);
    const uint32_t bytes = header.nsym / 4u;
    uint32_t pos = 0;
    while (feed_run) {
        const uint32_t room = RING_BYTES - (ring.w - ring.r);
        /* attesa attiva: durante la TX il core 0 ha gli interrupt mascherati e il tick di FreeRTOS (che avanza sul core 0)
         * è fermo, quindi vTaskDelay non tornerebbe */
        if (room < FEED_CHUNK) { esp_rom_delay_us(100); continue; }
        uint32_t n = FEED_CHUNK;
        const uint32_t w = ring.w;
        for (uint32_t done = 0; done < n;) {           /* il blocco può attraversare la fine del file o del buffer */
            uint32_t k = n - done;
            if (k > bytes - pos) k = bytes - pos;
            const uint32_t wi = (w + done) & (RING_BYTES - 1u);
            if (k > RING_BYTES - wi) k = RING_BYTES - wi;
            memcpy(ring_buf + wi, sym + pos, k);
            done += k;
            pos += k;
            if (pos == bytes) pos = 0;
        }
        __asm__ volatile("memw" ::: "memory");
        ring.w = w + n;
    }
    feed_task = NULL;
    vTaskDelete(NULL);
}

uint32_t pdx_sym_play(int amp, int dci, int dcq, uint32_t ms, pdx_rf_abort_fn abort_fn)
{
    ring.w = ring.r = 0;
    feed_run = true;
    xTaskCreatePinnedToCore(feed, "sym_feed", 4096, NULL, 10, &feed_task, 1);
    while (ring.w < RING_BYTES - FEED_CHUNK) vTaskDelay(1);   /* buffer quasi pieno prima di partire */
    const uint32_t done = pdx_rf_tx_qpsk_ring(amp, header.sr, dci, dcq, ms, &ring, abort_fn);
    feed_run = false;
    while (feed_task) vTaskDelay(1);
    /* diagnosi: stesso checksum sui byte del file (in loop) che il modulatore avrebbe dovuto consumare */
    const pdx_rf_stats_t *st = pdx_rf_last_stats();
    const uint8_t *sym = map_data + sizeof(pdx_sym_header_t);
    const uint32_t nb = header.nsym / 4u;
    uint32_t s1 = 0, s2 = 0;
    for (uint32_t i = 0; i < st->bytes; i++) { s1 += sym[i % nb]; s2 += s1; }
    printf("CHECK simboli: %u byte consumati, checksum %08x/%08x, atteso %08x/%08x -> %s\n", (unsigned)st->bytes,
           (unsigned)st->sum1, (unsigned)st->sum2, (unsigned)s1, (unsigned)s2,
           (st->sum1 == s1 && st->sum2 == s2) ? "UGUALI" : "DIVERSI");
    return done;
}
