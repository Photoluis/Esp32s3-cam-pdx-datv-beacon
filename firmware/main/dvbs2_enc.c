/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Codificatore DVB-S2 (vedi dvbs2_enc.h). Verificato bit per bit contro tools/dvbs2/encoder.py con il comando S2TEST. */
#include "dvbs2_enc.h"

#include <stdlib.h>
#include <string.h>
#include "dvbs2_tables.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

static const char *RATES[] = {"1/4", "1/3", "2/5", "1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "8/9", "9/10"};
static const char PLS_SCRAMBLE[] = "0111000110011101100000111100100101010011010000100010110111111010";
static const uint32_t PLS_ROWS[6] = {0x55555555u, 0x33333333u, 0x0F0F0F0Fu, 0x00FF00FFu, 0x0000FFFFu, 0xFFFFFFFFu};

/* il lavoro del codificatore sta in PSRAM: in RAM interna le continue letture/scritture del core 1 contendevano la
 * memoria al core 0 (buffer dei simboli, banco del DAC) e i campioni uscivano in ritardo (ESPLAY del 30/10) */
static void *alloc_work(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM);
    return p ? p : heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

uint8_t pdx_s2_crc8(const uint8_t *p, uint32_t n)
{
    uint8_t crc = 0;                                      /* 5.1.4: X^8+X^7+X^6+X^4+X^2+1, bit più significativo prima */
    for (uint32_t i = 0; i < n; i++) {
        for (int k = 7; k >= 0; k--) {
            const uint8_t fb = (uint8_t)(((crc >> 7) ^ (p[i] >> k)) & 1u);
            crc = (uint8_t)(crc << 1);
            if (fb) crc ^= 0xD5;
        }
    }
    return crc;
}

/* ------------------------------------------------------------ BCH (resto in 192 bit, allineato in alto) */

static void w_shl(uint32_t w[6], int n)                 /* n = 1..8 */
{
    for (int i = 0; i < 5; i++) w[i] = (w[i] << n) | (w[i + 1] >> (32 - n));
    w[5] <<= n;
}

static void bch_build(pdx_s2_t *s, const uint8_t *g, uint32_t deg)
{
    uint32_t gl[6] = {0};                                 /* coefficienti x^(deg-1) .. x^0 allineati al bit 191 */
    for (uint32_t i = 0; i < deg; i++) {
        const uint32_t src = i + 1;                       /* salta x^deg */
        if (g[src >> 3] & (0x80u >> (src & 7))) gl[i >> 5] |= 0x80000000u >> (i & 31);
    }
    for (int v = 0; v < 256; v++) {
        uint32_t r[6] = {0};
        for (int k = 7; k >= 0; k--) {
            const uint32_t fb = (r[0] >> 31) ^ ((uint32_t)(v >> k) & 1u);
            w_shl(r, 1);
            if (fb) for (int i = 0; i < 6; i++) r[i] ^= gl[i];
        }
        memcpy(s->bch_tab[v], r, sizeof(r));
    }
    s->deg = deg;
}

/* parità BCH dei primi kbch bit di cw, scritta subito dopo (deg bit) */
static void bch_encode(pdx_s2_t *s)
{
    uint32_t r[6] = {0};
    const uint32_t nb = s->kbch / 8u;
    for (uint32_t i = 0; i < nb; i++) {
        const uint32_t idx = (r[0] >> 24) ^ s->cw[i];
        w_shl(r, 8);
        const uint32_t *t = s->bch_tab[idx];
        for (int k = 0; k < 6; k++) r[k] ^= t[k];
    }
    for (uint32_t i = 0; i < s->deg / 8u; i++) s->cw[nb + i] = (uint8_t)(r[i >> 2] >> (24 - 8 * (i & 3)));
}

/* ------------------------------------------------------------ LDPC (5.3.2) */

/* Gli indirizzi di un gruppo di 360 bit di informazione sono x + m*q (m = 0..359) modulo nk = 360 q: con x = c q + a
 * diventano a + q ((c + m) mod 360). Con la parità disposta in q righe da 360 bit (P[a], bit j = parità a + q j), ogni
 * voce della tabella è lo XOR del gruppo G (bit m = informazione 360 r + m) ruotato di c in P[a]: 12 parole a 32 bit
 * invece di 360 bit uno per uno. */
#define W360 12u                                          /* parole per 360 bit (384, gli ultimi 24 inutilizzati) */
static uint8_t bitrev8[256];

static void ldpc_encode(pdx_s2_t *s)
{
    const pdx_s2_table_t *t = s->tab;
    uint8_t *info = s->cw, *par = s->cw + s->kldpc / 8u;
    const uint32_t q = t->q, nk = s->nk;
    uint32_t (*P)[W360] = s->ldpc_rows;                   /* q righe */
    memset(P, 0, q * sizeof(P[0]));
    const uint16_t *a = t->addr;
    uint32_t D[2 * W360];                                 /* gruppo raddoppiato: D[k] = G[k mod 360], k = 0..719 */
    for (uint32_t r = 0; r < t->nrows; r++) {
        const uint8_t *g8 = info + 45u * r;               /* 360 bit = 45 byte, bit più significativo per primo */
        uint32_t G[W360] = {0};
        for (uint32_t i = 0; i < 45u; i++) G[i >> 2] |= (uint32_t)bitrev8[g8[i]] << ((i & 3u) * 8u);
        memset(D, 0, sizeof(D));
        for (uint32_t w = 0; w < W360; w++) D[w] = G[w];
        for (uint32_t w = 0; w < W360; w++) {             /* copia di G a partire dal bit 360 (= parola 11, bit 8) */
            D[11 + w] |= G[w] << 8;
            D[12 + w] |= G[w] >> 24;
        }
        const uint32_t len = t->rowlen[r];
        for (uint32_t j = 0; j < len; j++) {
            const uint32_t x = a[j], c = x / q, row = x - c * q;
            const uint32_t o = 360u - c;                  /* R[j] = D[j + 360 - c] */
            const uint32_t ow = o >> 5, ob = o & 31u;
            uint32_t *pr = P[row];
            if (ob) {
                for (uint32_t w = 0; w < W360; w++) pr[w] ^= (D[ow + w] >> ob) | (D[ow + w + 1] << (32u - ob));
            } else {
                for (uint32_t w = 0; w < W360; w++) pr[w] ^= D[ow + w];
            }
        }
        a += len;
    }
    memset(par, 0, nk / 8u);                              /* di nuovo in ordine lineare: parità a + q j */
    for (uint32_t row = 0; row < q; row++) {
        P[row][11] &= 0xFFu;                              /* solo i bit 352..359 */
        for (uint32_t w = 0; w < W360; w++) {
            uint32_t v = P[row][w];
            while (v) {
                const uint32_t b = (uint32_t)__builtin_ctz(v);
                v &= v - 1u;
                const uint32_t p = row + q * (w * 32u + b);
                par[p >> 3] |= (uint8_t)(0x80u >> (p & 7));
            }
        }
    }
    uint32_t carry = 0;                                   /* p_i = p_i xor p_(i-1): xor cumulativo, bit più significativo prima */
    for (uint32_t i = 0; i < nk / 8u; i++) {
        uint32_t v = par[i];
        v ^= v >> 1;
        v ^= v >> 2;
        v ^= v >> 4;
        if (carry) v ^= 0xFFu;
        par[i] = (uint8_t)v;
        carry = v & 1u;
    }
}

/* ------------------------------------------------------------ PL framing (5.5) */

static uint8_t sym4[256];      /* byte della parola di codice -> 4 codici QPSK (5.4.1: bit 0 -> +1), primo nei bit bassi */
static uint8_t rot_lut[16];    /* codice | R << 2 -> codice ruotato */

static inline uint32_t rotate(uint32_t code, uint32_t r)   /* (I + jQ) * j^R sui codici (bit 0 = I > 0, bit 1 = Q > 0) */
{
    const uint32_t i = code & 1u, q = code >> 1;
    switch (r) {
    case 1: return (q ^ 1u) | (i << 1);                   /* (-Q,  I) */
    case 2: return (i ^ 1u) | ((q ^ 1u) << 1);            /* (-I, -Q) */
    case 3: return q | ((i ^ 1u) << 1);                   /* ( Q, -I) */
    default: return code;
    }
}

static void build_header(pdx_s2_t *s, uint32_t modcod)
{
    uint8_t y[90];
    for (int i = 0; i < 26; i++) y[i] = (uint8_t)((0x18D2E82u >> (25 - i)) & 1u);         /* SOF */
    const uint32_t pls = (modcod << 2) | ((uint32_t)s->shortf << 1) | (uint32_t)s->pilots;  /* b1..b7 */
    uint32_t cw32 = 0;
    for (int k = 0; k < 6; k++) if ((pls >> (6 - k)) & 1u) cw32 ^= PLS_ROWS[k];
    const uint32_t b7 = pls & 1u;
    for (int i = 0; i < 32; i++) {
        const uint8_t v = (uint8_t)((cw32 >> (31 - i)) & 1u);
        y[26 + 2 * i] = (uint8_t)(v ^ (PLS_SCRAMBLE[2 * i] - '0'));
        y[27 + 2 * i] = (uint8_t)((v ^ b7) ^ (PLS_SCRAMBLE[2 * i + 1] - '0'));
    }
    for (int i = 0; i < 90; i++)                          /* pi/2 BPSK: pari (da 0) I = Q = a, dispari I = -a, Q = a */
        s->hdr[i] = (i & 1) ? (uint8_t)((y[i] ? 1u : 0u) | ((y[i] ? 0u : 1u) << 1))
                            : (uint8_t)(y[i] ? 0u : 3u);
}

/* R(i) = 2 z((i + 131072) mod (2^18-1)) + z(i), z(i) = x(i) xor y(i) (codice n = 0) */
static void build_rotation(pdx_s2_t *s)
{
    uint32_t xa = 1u, ya = 0x3FFFFu, xb = 1u, yb = 0x3FFFFu;
#define XSTEP(st) (st = ((st) >> 1) | (((((st) >> 7) ^ (st)) & 1u) << 17))
#define YSTEP(st) (st = ((st) >> 1) | (((((st) >> 10) ^ ((st) >> 7) ^ ((st) >> 5) ^ (st)) & 1u) << 17))
    for (uint32_t i = 0; i < 131072u; i++) { XSTEP(xb); YSTEP(yb); }
    const uint32_t n = s->nsym - 90u;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t za = (xa ^ ya) & 1u, zb = (xb ^ yb) & 1u;
        s->rot[i >> 2] |= (uint8_t)(((zb << 1) | za) << ((i & 3u) * 2u));
        XSTEP(xa); YSTEP(ya); XSTEP(xb); YSTEP(yb);
    }
#undef XSTEP
#undef YSTEP
}

void pdx_s2_frame(pdx_s2_t *s, const uint8_t *data_field, uint16_t syncd, pdx_s2_out_t *out)
{
    const int64_t t0 = esp_timer_get_time();
    uint8_t *bb = s->cw;                                  /* BBHEADER + DATA FIELD, poi BB scrambling (5.1.6, 5.2) */
    const uint32_t dfl = s->dfl;
    bb[0] = 0xF0;                                         /* TS, singolo flusso, CCM, ISSYI 0, NPD 0, roll-off 0,35 */
    bb[1] = 0x00;
    bb[2] = 1504 >> 8;
    bb[3] = 1504 & 255;
    bb[4] = (uint8_t)(dfl >> 8);
    bb[5] = (uint8_t)dfl;
    bb[6] = 0x47;
    bb[7] = (uint8_t)(syncd >> 8);
    bb[8] = (uint8_t)syncd;
    bb[9] = pdx_s2_crc8(bb, 9);
    memcpy(bb + 10, data_field, dfl / 8u);
    for (uint32_t i = 0; i < s->kbch / 8u; i++) bb[i] ^= s->prbs[i];
    bch_encode(s);
    const int64_t t1 = esp_timer_get_time();
    ldpc_encode(s);
    const int64_t t2 = esp_timer_get_time();

    /* scrittura a byte interi: accumulatore di 4 simboli, qualunque sia l'allineamento iniziale */
    uint32_t idx = out->pos >> 2, nacc = out->pos & 3u;
    uint32_t acc = out->buf[idx] & ((1u << (2u * nacc)) - 1u);
#define EMIT(code)                                                   \
    do {                                                             \
        acc |= (uint32_t)(code) << (2u * nacc);                      \
        if (++nacc == 4u) { out->buf[idx++] = (uint8_t)acc; acc = 0; nacc = 0; } \
    } while (0)
    for (int i = 0; i < 90; i++) EMIT(s->hdr[i]);
    /* dati: ogni byte della parola di codice dà 4 simboli (sym4); lo scrambling ruota ogni simbolo (rot_lut). Le pilot
     * sono blocchi da 36 simboli (multiplo di 4), quindi la sequenza R resta allineata ai byte dei dati. */
    const uint32_t nbytes = s->nldpc / 8u;
    uint32_t ri = 0;
    for (uint32_t b = 0; b < nbytes; b++) {
        const uint32_t codes = sym4[s->cw[b]], rr = s->rot[ri >> 2];
        for (uint32_t k = 0; k < 4u; k++) EMIT(rot_lut[((codes >> (2u * k)) & 3u) | (((rr >> (2u * k)) & 3u) << 2)]);
        ri += 4u;
        if (s->pilots && (b + 1u) % 360u == 0 && b + 1u < nbytes) {   /* 1440 simboli = 360 byte; pilot non alla fine */
            for (uint32_t p = 0; p < 36u; p += 4u, ri += 4u) {
                const uint32_t rp = s->rot[ri >> 2];
                for (uint32_t k = 0; k < 4u; k++) EMIT(rot_lut[3u | (((rp >> (2u * k)) & 3u) << 2)]);
            }
        }
    }
    if (nacc) out->buf[idx] = (uint8_t)acc;
#undef EMIT
    out->pos += 90u + s->nldpc / 2u + (s->pilots ? 36u * ((s->slots - 1u) / 16u) : 0u);
    const int64_t t3 = esp_timer_get_time();
    s->t_bch_us = (uint32_t)(t1 - t0);
    s->t_ldpc_us = (uint32_t)(t2 - t1);
    s->t_map_us = (uint32_t)(t3 - t2);
}

double pdx_s2_ts_bitrate(const pdx_s2_t *s, uint32_t sr) { return (double)sr * s->dfl / s->nsym; }

void pdx_s2_free(pdx_s2_t *s)
{
    heap_caps_free(s->bch_tab);
    heap_caps_free(s->prbs);
    heap_caps_free(s->rot);
    heap_caps_free(s->cw);
    heap_caps_free(s->ldpc_rows);
    memset(s, 0, sizeof(*s));
}

const char *pdx_s2_init(pdx_s2_t *s, const char *rate, bool shortf, bool pilots)
{
    memset(s, 0, sizeof(*s));
    int ri = -1;
    for (int i = 0; i < 11; i++) if (!strcmp(rate, RATES[i])) ri = i;
    if (ri < 0 || (shortf && ri == 10)) return "rate non disponibile";
    const pdx_s2_table_t *t = &pdx_s2_tables[shortf ? 11 + ri : ri];
    s->tab = t;
    s->shortf = shortf;
    s->pilots = pilots;
    s->kbch = t->kbch;
    s->dfl = t->kbch - 80u;
    s->nldpc = t->nldpc;
    s->kldpc = t->kldpc;
    s->nk = t->nldpc - t->kldpc;
    s->slots = t->nldpc / 2u / 90u;
    s->nsym = 90u * (s->slots + 1u) + (pilots ? 36u * ((s->slots - 1u) / 16u) : 0u);

    s->bch_tab = heap_caps_calloc(256, sizeof(s->bch_tab[0]), MALLOC_CAP_SPIRAM);
    s->prbs = heap_caps_calloc(1, s->kbch / 8u, MALLOC_CAP_SPIRAM);
    s->rot = heap_caps_calloc(1, (s->nsym - 90u + 3u) / 4u, MALLOC_CAP_SPIRAM);
    s->cw = alloc_work(s->nldpc / 8u);
    s->ldpc_rows = alloc_work(t->q * sizeof(s->ldpc_rows[0]));
    if (!s->bch_tab || !s->prbs || !s->rot || !s->cw || !s->ldpc_rows) { pdx_s2_free(s); return "memoria"; }

    const uint8_t *g = shortf ? s2_bch_short_t12 : t->t == 12 ? s2_bch_normal_t12 : t->t == 10 ? s2_bch_normal_t10
                                                                                            : s2_bch_normal_t8;
    bch_build(s, g, shortf ? 168u : 16u * t->t);
    if (s->deg != t->nbch - t->kbch) { pdx_s2_free(s); return "grado BCH"; }

    uint8_t st[15] = {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0};   /* 5.2.2: stadi 1..15 = 100101010000000 */
    for (uint32_t i = 0; i < s->kbch; i++) {
        const uint8_t b = st[13] ^ st[14];
        if (b) s->prbs[i >> 3] |= (uint8_t)(0x80u >> (i & 7));
        memmove(st + 1, st, 14);
        st[0] = b;
    }
    for (uint32_t v = 0; v < 256u; v++) {
        uint32_t r = 0, c = 0;
        for (int k = 0; k < 8; k++) r |= ((v >> k) & 1u) << (7 - k);
        bitrev8[v] = (uint8_t)r;
        for (uint32_t k = 0; k < 4u; k++) {               /* simbolo k: bit 7-2k (I) e 6-2k (Q) */
            const uint32_t b0 = (v >> (7u - 2u * k)) & 1u, b1 = (v >> (6u - 2u * k)) & 1u;
            c |= ((b0 ^ 1u) | ((b1 ^ 1u) << 1)) << (2u * k);
        }
        sym4[v] = (uint8_t)c;
    }
    for (uint32_t i = 0; i < 16u; i++) rot_lut[i] = (uint8_t)rotate(i & 3u, i >> 2);
    build_header(s, t->modcod);
    build_rotation(s);
    return NULL;
}
