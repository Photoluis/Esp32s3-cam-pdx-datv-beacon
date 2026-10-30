/* Beacon DATV 13 cm - IZ7PDX
 * Immagini e video del beacon nella flash (vedi media_store.h). */
#include "media_store.h"

#include <stdio.h>
#include <string.h>
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "ts_mux.h"

typedef struct {
    uint32_t off, size;
    const void *map;
    esp_partition_mmap_handle_t h;
} region_t;

static region_t regions[4] = {
    [PDX_MEDIA_ES_STD] = {PDX_ES_STD_OFF, PDX_ES_STD_SIZE},
    [PDX_MEDIA_ES_USR] = {PDX_ES_USR_OFF, PDX_ES_USR_SIZE},
    [PDX_MEDIA_IMG_STD] = {PDX_IMG_STD_OFF, PDX_IMG_SIZE},
    [PDX_MEDIA_IMG_USR] = {PDX_IMG_USR_OFF, PDX_IMG_SIZE},
};

static const esp_partition_t *part(void)
{
    static const esp_partition_t *p;
    if (!p) p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "symbols");
    return p;
}

static bool is_user(pdx_media_t m) { return m == PDX_MEDIA_ES_USR || m == PDX_MEDIA_IMG_USR; }
static bool is_es(pdx_media_t m) { return m == PDX_MEDIA_ES_STD || m == PDX_MEDIA_ES_USR; }

static const char *check(pdx_media_t m, const uint8_t *d)
{
    const region_t *r = &regions[m];
    if (is_es(m)) {
        const pdx_es_header_t *h = (const pdx_es_header_t *)d;
        if (memcmp(h->magic, "PDXES001", 8)) return "assente";
        if (!h->nunits || h->data_off < 64u + 16u * h->nunits || h->data_off > r->size) return "intestazione non valida";
        if (h->vtype != 0x24 && h->vtype != 0x1B) return "video né H.265 né H.264";
        const pdx_es_unit_t *u = (const pdx_es_unit_t *)(d + 64);
        for (uint32_t i = 0; i < h->nunits; i++)
            if (h->data_off + u[i].off + u[i].len > r->size || u[i].pts >= h->loop_ticks) return "tabella non valida";
        return NULL;
    }
    const pdx_img_header_t *h = (const pdx_img_header_t *)d;
    if (memcmp(h->magic, "PDXIMG01", 8)) return "assente";
    if (h->w != PDX_IMG_W || h->h != PDX_IMG_H || h->fmt != 1 || h->len != PDX_IMG_W * PDX_IMG_H * 2u)
        return "formato non valido (serve 320x240 YUYV)";
    if (esp_rom_crc32_le(0, d + PDX_IMG_HDR, h->len) != h->crc) return "CRC errato";
    return NULL;
}

const uint8_t *pdx_media_map(pdx_media_t m, const char **why)
{
    region_t *r = &regions[m];
    const char *dummy;
    if (!why) why = &dummy;
    if (!part()) { *why = "partizione symbols assente"; return NULL; }
    if (!r->map && esp_partition_mmap(part(), r->off, r->size, ESP_PARTITION_MMAP_DATA, &r->map, &r->h) != ESP_OK) {
        r->map = NULL;
        *why = "mmap";
        return NULL;
    }
    *why = check(m, r->map);
    return *why ? NULL : r->map;
}

void pdx_media_describe(pdx_media_t m, char *out, size_t n)
{
    const char *why;
    const uint8_t *d = pdx_media_map(m, &why);
    if (!d) { snprintf(out, n, "%s", why); return; }
    char label[40];
    if (is_es(m)) {
        const pdx_es_header_t *h = (const pdx_es_header_t *)d;
        snprintf(label, sizeof(label), "%.35s", h->label);
        snprintf(out, n, "%s, %s, loop %.1f s", label, h->vtype == 0x24 ? "H.265" : "H.264", h->loop_ticks / 90000.0);
    } else {
        const pdx_img_header_t *h = (const pdx_img_header_t *)d;
        snprintf(label, sizeof(label), "%.35s", h->label);
        snprintf(out, n, "%s, %ux%u", label, h->w, h->h);
    }
    for (char *p = out; *p; p++) if (*p < 32 || *p > 126 || *p == '"' || *p == '\\') *p = '_';   /* sicuro nel JSON */
}

/* ------------------------------------------------------------ scrittura delle aree personalizzate */

static struct {
    bool open;
    pdx_media_t m;
    uint32_t total, pos, erased;
} wr;

static void unmap(pdx_media_t m)
{
    region_t *r = &regions[m];
    if (r->map) {
        esp_partition_munmap(r->h);
        r->map = NULL;
    }
}

const char *pdx_media_write_begin(pdx_media_t m, uint32_t total)
{
    if (!is_user(m)) return "area protetta: si scrivono solo il video e l'immagine personalizzati";
    if (!part()) return "partizione symbols assente";
    if (total < 64u || total > regions[m].size) return "dimensione fuori dall'area";
    if (!is_es(m) && total != PDX_IMG_BYTES) return "l'immagine deve essere 320x240 YUYV con intestazione";
    unmap(m);
    wr = (typeof(wr)){.open = true, .m = m, .total = total};
    return NULL;
}

const char *pdx_media_write(const uint8_t *data, uint32_t len)
{
    if (!wr.open) return "scrittura non aperta";
    if (wr.pos + len > wr.total) return "dati oltre la dimensione dichiarata";
    const region_t *r = &regions[wr.m];
    const uint32_t end = wr.pos + len;
    while (wr.erased < end) {                                      /* cancella a blocchi di 64 KB man mano */
        const uint32_t blk = r->size - wr.erased < 0x10000u ? r->size - wr.erased : 0x10000u;
        if (esp_partition_erase_range(part(), r->off + wr.erased, blk) != ESP_OK) { wr.open = false; return "cancellazione"; }
        wr.erased += blk;
    }
    if (esp_partition_write(part(), r->off + wr.pos, data, len) != ESP_OK) { wr.open = false; return "scrittura"; }
    wr.pos = end;
    return NULL;
}

const char *pdx_media_write_end(void)
{
    if (!wr.open) return "scrittura non aperta";
    wr.open = false;
    if (wr.pos != wr.total) return "file incompleto";
    const char *why;
    return pdx_media_map(wr.m, &why) ? NULL : why;
}

const char *pdx_media_erase_user(pdx_media_t m)
{
    if (!is_user(m)) return "area protetta";
    if (!part()) return "partizione symbols assente";
    unmap(m);
    /* basta cancellare il primo settore: senza intestazione il file risulta assente */
    return esp_partition_erase_range(part(), regions[m].off, 0x1000) == ESP_OK ? NULL : "cancellazione";
}
