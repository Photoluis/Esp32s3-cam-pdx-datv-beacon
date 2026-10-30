/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * S2TEST: confronta il codificatore DVB-S2 del firmware con tools/dvbs2/encoder.py sui casi di make_s2test.py
 * (file a 0xC00000 nella partizione "symbols"). Nessuna RF. */
#include <stdio.h>
#include <string.h>
#include "dvbs2_enc.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"

#define S2TEST_OFFSET 0xC00000u
#define S2TEST_MAP    0x100000u

static const char *RATES[] = {"1/4", "1/3", "2/5", "1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "8/9", "9/10"};

int pdx_s2_selftest(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "symbols");
    if (!part) { printf("ERR partizione symbols\n"); return -1; }
    const void *map;
    esp_partition_mmap_handle_t mh;
    if (esp_partition_mmap(part, S2TEST_OFFSET, S2TEST_MAP, ESP_PARTITION_MMAP_DATA, &map, &mh) != ESP_OK) {
        printf("ERR mmap\n");
        return -1;
    }
    const uint8_t *p = map;
    if (memcmp(p, "PDXS2T01", 8)) { printf("ERR file di prova assente\n"); esp_partition_munmap(mh); return -1; }
    uint32_t ncase;
    memcpy(&ncase, p + 8, 4);
    p += 12;
    int ok = 0;
    for (uint32_t c = 0; c < ncase; c++) {
        const uint8_t ri = p[0], shortf = p[1], pilots = p[2], nf = p[3];
        uint32_t ts_len, sym_len;
        memcpy(&ts_len, p + 4, 4);
        memcpy(&sym_len, p + 8, 4);
        const uint8_t *ts = p + 12, *expect = ts + ts_len;
        p = expect + sym_len;

        pdx_s2_t s;
        const char *why = pdx_s2_init(&s, RATES[ri], shortf, pilots);
        if (why) { printf("S2TEST %s %s: ERR %s\n", shortf ? "corto " : "normale", RATES[ri], why); continue; }
        const uint32_t nb = s.dfl / 8u, npk = ts_len / 188u;
        uint8_t *stream = heap_caps_malloc(npk * 188u, MALLOC_CAP_SPIRAM);
        uint8_t *sym = heap_caps_calloc(1, sym_len, MALLOC_CAP_SPIRAM);
        if (!stream || !sym) { printf("ERR memoria\n"); heap_caps_free(stream); heap_caps_free(sym); pdx_s2_free(&s); break; }
        uint8_t crc = 0;                                  /* mode adaptation: sync byte -> CRC-8 del pacchetto precedente */
        for (uint32_t k = 0; k < npk; k++) {
            stream[k * 188u] = crc;
            memcpy(stream + k * 188u + 1, ts + k * 188u + 1, 187);
            crc = pdx_s2_crc8(ts + k * 188u + 1, 187);
        }
        pdx_s2_out_t out = {.buf = sym};
        for (uint32_t f = 0; f < nf; f++) {
            const uint32_t pos = f * nb, first = (pos + 187u) / 188u * 188u;
            const uint16_t syncd = first < pos + nb ? (uint16_t)((first - pos) * 8u) : 0xFFFFu;
            pdx_s2_frame(&s, stream + pos, syncd, &out);
        }
        const bool same = out.pos == nf * s.nsym && !memcmp(sym, expect, sym_len);
        uint32_t first_diff = 0;
        if (!same) while (first_diff < sym_len && sym[first_diff] == expect[first_diff]) first_diff++;
        printf("S2TEST %s %-4s pilot %d: %s  (BCH %u us, LDPC %u us, mappatura %u us per frame)",
               shortf ? "corto  " : "normale", RATES[ri], pilots, same ? "UGUALE" : "DIVERSO", (unsigned)s.t_bch_us,
               (unsigned)s.t_ldpc_us, (unsigned)s.t_map_us);
        if (!same) printf("  primo byte diverso %u", (unsigned)first_diff);
        printf("\n");
        ok += same;
        heap_caps_free(stream);
        heap_caps_free(sym);
        pdx_s2_free(&s);
    }
    esp_partition_munmap(mh);
    printf("S2TEST %d/%u casi uguali al codificatore di riferimento\n", ok, (unsigned)ncase);
    return ok == (int)ncase ? 0 : -1;
}
