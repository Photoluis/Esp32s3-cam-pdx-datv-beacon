/* ESP32-S3_PDX Parrot - IZ7PDX - GPL-3.0
 * Riquadro della risposta al QSO: immagine ricevuta (H.264) o costellazione (vedi parrot_pip.h). */
#include "parrot_pip.h"

#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_h264_dec.h"
#include "esp_h264_dec_param.h"
#include "esp_h264_dec_sw.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "overlay.h"

#define PIP_MAGIC 0x50495031u
typedef struct {
    uint32_t magic;
    uint16_t w, h;                           /* immagine 160x90, costellazione quadrata 84x84 */
    char label[28];
    uint8_t yuyv[PDX_PIP_W * PDX_PIP_H * 2];
    uint32_t crc;
} pip_t;
EXT_RAM_NOINIT_ATTR static pip_t pip;

static uint32_t pip_crc(void) { return esp_rom_crc32_le(0, (const uint8_t *)&pip, offsetof(pip_t, crc)); }
bool pdx_pip_valid(void) { return pip.magic == PIP_MAGIC && pip.crc == pip_crc(); }
const char *pdx_pip_label(void) { return pdx_pip_valid() ? pip.label : ""; }
void pdx_pip_clear(void) { pip.magic = 0; }

static void pip_seal(const char *label)
{
    snprintf(pip.label, sizeof(pip.label), "%s", label);
    pip.magic = PIP_MAGIC;
    pip.crc = pip_crc();
}

/* ------------------------------------------------------------------ costellazione (quadrata, 06/11) */
#define CST_S 84                     /* lato in pixel (pari) */
static void put_y(int x, int y, uint8_t v)
{
    if (x < 0 || y < 0 || x >= CST_S || y >= CST_S) return;
    pip.yuyv[(y * CST_S + x) * 2] = v;
}

void pdx_pip_from_const(const int16_t *iq, uint32_t n, const char *label)
{
    pip.w = pip.h = CST_S;
    for (int i = 0; i < CST_S * CST_S; i++) { pip.yuyv[2 * i] = 20; pip.yuyv[2 * i + 1] = 128; }
    const int c = CST_S / 2;
    for (int k = 0; k < CST_S; k++) { put_y(k, c, 70); put_y(c, k, 70); }   /* assi */
    /* |s| medio = 1 -> punti ideali a +-0,707: 0,707 x 40 = 28 px dal centro (bordo a 42); i punti fuori si scartano */
    for (uint32_t i = 0; i < n; i++) {
        const int x = c + (int)(iq[2 * i] * 40 / 8192), y = c - (int)(iq[2 * i + 1] * 40 / 8192);
        if (x >= 1 && y >= 1 && x < CST_S - 1 && y < CST_S - 1) put_y(x, y, 235);
    }
    /* crominanza verde sui punti: U e V bassi dove almeno un pixel della coppia è acceso */
    for (int i = 0; i < CST_S * CST_S; i += 2) {
        const bool on = pip.yuyv[2 * i] > 200 || pip.yuyv[2 * i + 2] > 200;
        pip.yuyv[2 * i + 1] = on ? 60 : 128;
        pip.yuyv[2 * i + 3] = on ? 60 : 128;
    }
    pip_seal(label);
}

int pdx_pip_w(void) { return pdx_pip_valid() ? pip.w : 0; }
int pdx_pip_h(void) { return pdx_pip_valid() ? pip.h : 0; }

/* ------------------------------------------------------------------ immagine H.264 dal TS */
static int find_video(const uint8_t *ts, const uint8_t *ok, uint32_t np, uint8_t *vtype)
{
    int pmt = -1;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t k = 0; k < np; k++) {
            const uint8_t *p = ts + 188 * k;
            if (!ok[k] || !(p[1] & 0x40)) continue;
            const int pid = ((p[1] & 0x1F) << 8) | p[2];
            if (pid != 0 && pid != pmt) continue;
            const int afc = (p[3] >> 4) & 3;
            int off = 4 + ((afc & 2) ? 1 + p[4] : 0);
            if (!(afc & 1) || off >= 188) continue;
            off += 1 + p[off];
            if (off + 12 > 188) continue;
            const uint8_t *s = p + off;
            const int slen = ((s[1] & 0x0F) << 8) | s[2];
            if (off + 3 + slen > 188) continue;
            if (pid == 0 && s[0] == 0x00 && pmt < 0) {
                for (int i = 8; i + 4 <= 3 + slen - 4; i += 4)
                    if ((s[i] << 8 | s[i + 1]) != 0) { pmt = ((s[i + 2] & 0x1F) << 8) | s[i + 3]; break; }
            } else if (pid == pmt && s[0] == 0x02) {
                const int pil = ((s[10] & 0x0F) << 8) | s[11];
                for (int i = 12 + pil; i + 5 <= 3 + slen - 4;) {
                    const uint8_t st = s[i];
                    const int epid = ((s[i + 1] & 0x1F) << 8) | s[i + 2];
                    const int el = ((s[i + 3] & 0x0F) << 8) | s[i + 4];
                    if (st == 0x1B || st == 0x24 || st == 0x02) { *vtype = st; return epid; }
                    i += 5 + el;
                }
            }
        }
    return -1;
}

#define PES_MAX (256u * 1024u)
#define PS_MAX 128u
typedef struct {
    uint8_t sps[PS_MAX], pps[PS_MAX];        /* con il codice di inizio 00 00 00 01 */
    uint32_t nsps, npps;
} ps_t;

/* NAL (dopo il codice di inizio) in un flusso Annex B; restituisce il tipo del primo IDR o -1 */
static bool scan_nals(const uint8_t *b, uint32_t n, ps_t *ps)
{
    bool idr = false;
    for (uint32_t i = 0; i + 3 < n; i++) {
        if (b[i] || b[i + 1] || b[i + 2] != 1) continue;
        const uint32_t s = i + 3;
        uint32_t e = s;
        while (e + 3 < n && !(b[e] == 0 && b[e + 1] == 0 && (b[e + 2] == 1 || (b[e + 2] == 0 && b[e + 3] == 1)))) e++;
        if (e + 3 >= n) e = n;
        const int t = b[s] & 0x1F;
        if (t == 5) idr = true;
        if ((t == 7 || t == 8) && e - s + 4 <= PS_MAX) {
            uint8_t *d = t == 7 ? ps->sps : ps->pps;
            d[0] = d[1] = d[2] = 0; d[3] = 1;
            memcpy(d + 4, b + s, e - s);
            if (t == 7) ps->nsps = e - s + 4; else ps->npps = e - s + 4;
        }
        i = e - 1;
    }
    return idr;
}

/* riduzione I420 w x h -> riquadro YUYV 128x72 (media dei pixel, proporzioni mantenute con bande nere) */
#define IMG_W 128                    /* immagine ricevuta nella risposta 16:9 (06/11) */
#define IMG_H 72
static void scale_into_pip(const uint8_t *yuv, uint32_t w, uint32_t h)
{
    pip.w = IMG_W; pip.h = IMG_H;
    for (int i = 0; i < IMG_W * IMG_H; i++) { pip.yuyv[2 * i] = 16; pip.yuyv[2 * i + 1] = 128; }
    float sc = (float)IMG_W / w;
    if ((float)IMG_H / h < sc) sc = (float)IMG_H / h;
    const int ow = (int)(w * sc) & ~1, oh = (int)(h * sc);
    const int ox = ((IMG_W - ow) / 2) & ~1, oy = (IMG_H - oh) / 2;
    const uint8_t *Y = yuv, *U = yuv + w * h, *V = U + (w / 2) * (h / 2);
    for (int y = 0; y < oh; y++) {
        const uint32_t y0 = (uint32_t)(y / sc), y1 = (uint32_t)((y + 1) / sc) > y0 ? (uint32_t)((y + 1) / sc) : y0 + 1;
        for (int x = 0; x < ow; x++) {
            const uint32_t x0 = (uint32_t)(x / sc), x1 = (uint32_t)((x + 1) / sc) > x0 ? (uint32_t)((x + 1) / sc) : x0 + 1;
            uint32_t a = 0, c = 0;
            for (uint32_t yy = y0; yy < y1 && yy < h; yy++)
                for (uint32_t xx = x0; xx < x1 && xx < w; xx++) { a += Y[yy * w + xx]; c++; }
            uint8_t *d = pip.yuyv + ((oy + y) * IMG_W + ox + x) * 2;
            d[0] = (uint8_t)(c ? a / c : 16);
            const uint32_t cxs = ((x0 + x1) / 2) / 2, cys = ((y0 + y1) / 2) / 2;
            const uint32_t ci = (cys < h / 2 ? cys : h / 2 - 1) * (w / 2) + (cxs < w / 2 ? cxs : w / 2 - 1);
            d[1] = (x & 1) ? V[ci] : U[ci];
        }
    }
}

static bool decode_idr(const uint8_t *es, uint32_t n, const ps_t *ps, char *info, size_t ni)
{
    static const uint8_t aud[] = {0, 0, 0, 1, 0x09, 0xF0, 0, 0, 0, 1, 0x09, 0xF0};
    const uint32_t tot = ps->nsps + ps->npps + n + sizeof(aud);
    uint8_t *b = heap_caps_malloc(tot, MALLOC_CAP_SPIRAM);
    if (!b) return false;
    uint32_t k = 0;
    memcpy(b + k, ps->sps, ps->nsps); k += ps->nsps;
    memcpy(b + k, ps->pps, ps->npps); k += ps->npps;
    memcpy(b + k, es, n); k += n;
    memcpy(b + k, aud, sizeof(aud)); k += sizeof(aud);
    esp_h264_dec_cfg_sw_t cfg = {.pic_type = ESP_H264_RAW_FMT_I420};
    esp_h264_dec_handle_t d = NULL;
    bool got = false;
    if (esp_h264_dec_sw_new(&cfg, &d) == ESP_H264_ERR_OK && esp_h264_dec_open(d) == ESP_H264_ERR_OK) {
        esp_h264_dec_in_frame_t in = {.raw_data = {.buffer = b, .len = k}};
        esp_h264_dec_out_frame_t out;
        for (int guard = 0; in.raw_data.len && guard < 4000 && !got; guard++) {
            memset(&out, 0, sizeof(out));
            if (esp_h264_dec_process(d, &in, &out) != ESP_H264_ERR_OK) break;
            if (out.outbuf && out.out_size) {
                esp_h264_dec_param_sw_handle_t ph;
                esp_h264_resolution_t res = {0};
                if (esp_h264_dec_sw_get_param_hd(d, &ph) == ESP_H264_ERR_OK) esp_h264_dec_get_resolution(ph, &res);
                if (res.width >= 64 && res.height >= 48 && (uint32_t)res.width * res.height * 3u / 2u <= out.out_size) {
                    scale_into_pip(out.outbuf, res.width, res.height);
                    snprintf(info, ni, "H.264 %ux%u", (unsigned)res.width, (unsigned)res.height);
                    got = true;
                }
            }
            if (!in.consume) break;
            in.raw_data.buffer += in.consume;
            in.raw_data.len -= in.consume;
        }
    }
    if (d) { esp_h264_dec_close(d); esp_h264_dec_del(d); }
    heap_caps_free(b);
    return got;
}

bool pdx_pip_from_ts(const uint8_t *ts, const uint8_t *ok, uint32_t np, char *info, size_t ni)
{
    snprintf(info, ni, "nessun video");
    uint8_t vt = 0;
    const int vpid = find_video(ts, ok, np, &vt);
    if (vpid < 0) return false;
    if (vt != 0x1B) { snprintf(info, ni, vt == 0x24 ? "H.265" : "MPEG-2"); return false; }
    uint8_t *pes = heap_caps_malloc(PES_MAX, MALLOC_CAP_SPIRAM);
    ps_t *ps = heap_caps_calloc(1, sizeof(ps_t), MALLOC_CAP_SPIRAM);
    if (!pes || !ps) { heap_caps_free(pes); heap_caps_free(ps); snprintf(info, ni, "memoria"); return false; }
    /* primo giro: SPS e PPS da qualsiasi PES integro; secondo giro: prima IDR integra che si decodifica */
    bool got = false;
    unsigned idr_seen = 0;
    for (int pass = 0; pass < 2 && !got; pass++) {
        uint32_t len = 0;
        bool coll = false, good = false;
        int cc = -1;
        for (uint32_t k = 0; k <= np && !got; k++) {
            const uint8_t *p = k < np ? ts + 188 * k : NULL;
            const int pid = p ? (((p[1] & 0x1F) << 8) | p[2]) : -1;
            if (p && pid != vpid) continue;
            const bool start = !p || (p[1] & 0x40);
            if (start && coll && good && len) {                        /* PES finito e integro */
                const bool idr = scan_nals(pes, len, ps);
                if (pass == 1 && idr && ps->nsps && ps->npps) {
                    idr_seen++;
                    got = decode_idr(pes, len, ps, info, ni);
                }
            }
            if (!p) break;
            const int afc = (p[3] >> 4) & 3;
            int off = 4 + ((afc & 2) ? 1 + p[4] : 0);
            const int c = p[3] & 15;
            if (!(afc & 1) || off >= 188) continue;
            if (start) {
                coll = true; good = ok[k] != 0; len = 0; cc = c;
                const uint8_t *h = p + off;
                if (off + 9 > 188 || h[0] || h[1] || h[2] != 1) { good = false; continue; }
                off += 9 + h[8];
                if (off > 188) { good = false; continue; }
            } else {
                if (!coll) continue;
                if (!ok[k] || c != ((cc + 1) & 15)) good = false;
                cc = c;
            }
            const uint32_t m = 188u - (uint32_t)off;
            if (len + m > PES_MAX) { good = false; continue; }
            memcpy(pes + len, p + off, m);
            len += m;
        }
    }
    if (!got) snprintf(info, ni, idr_seen ? "H.264 IDR non decodificata" : "H.264 senza IDR integra");
    heap_caps_free(pes);
    heap_caps_free(ps);
    if (got) {
        char l[28];
        snprintf(l, sizeof(l), "RX H.264");                    /* risoluzione solo nel log */
        pip_seal(l);
    }
    return got;
}

void pdx_pip_paste(uint8_t *yuyv, int w, int h, int x, int y)
{
    if (!pdx_pip_valid() || pip.w > PDX_PIP_W || pip.h > PDX_PIP_H) return;
    x &= ~1;
    pdx_ovl_box(yuyv, w, h, x - 2, y - 2, pip.w + 4, pip.h + 4, 235, 128, 128);   /* bordo bianco */
    for (int r = 0; r < pip.h; r++) {
        if (y + r < 0 || y + r >= h || x < 0 || x + pip.w > w) continue;
        memcpy(yuyv + ((size_t)(y + r) * w + x) * 2, pip.yuyv + (size_t)r * pip.w * 2, (size_t)pip.w * 2);
    }
}
