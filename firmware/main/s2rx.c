/* ESP32-S3_PDX Parrot - IZ7PDX - GPL-3.0
 * Ricevitore DVB-S2 in C (porting di tools/parrot/s2_rx.py), pensato per la memoria dell'ESP32-S3:
 *   - la cattura resta int8; i simboli sono salvati int8 (1,3 MB per 2 s a 333 kS)
 *   - filtro adattato RRC + mescolatore a blocchi, in due passate (temporizzazione, poi simboli): il segnale filtrato
 *     non viene mai memorizzato
 *   - PLHEADER cercati nel primo tratto (due frame) e poi seguiti di frame in frame
 *   - frequenza fine dai PLHEADER (correlazione differenziale), niente FFT grandi
 *   - LDPC min-sum a strati (righe in ordine: la catena della parità a scala si percorre in una sola iterazione)
 * Tutto in float a singola precisione (FPU dell'S3) e interi. */
#include "s2rx.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dvbs2_tables.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440
#endif
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

typedef struct { float re, im; } cf;

static const s2rx_env_t *E;
static bool droop_comp;                /* cattura dal decimatore della scheda: compensare la sua risposta */
void s2rx_set_board_capture(bool on) { droop_comp = on; }
static void lg(const char *fmt, ...)
{
    if (!E || !E->log) return;
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    E->log(b);
}
static uint32_t now_ms(void) { return E && E->ms ? E->ms() : 0; }
static void *zalloc(size_t n) { void *p = E->alloc(n); if (p) memset(p, 0, n); return p; }

static const char *const RATES[11] = {"1/4", "1/3", "2/5", "1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "8/9", "9/10"};
static const float THRESH[11] = {-2.35f, -1.24f, -0.30f, 1.00f, 2.23f, 3.10f, 4.03f, 4.68f, 5.18f, 6.20f, 6.42f};
static const uint32_t SR_STD[] = {66000, 125000, 250000, 333000, 500000, 1000000};
#define NSR (sizeof(SR_STD) / sizeof(SR_STD[0]))

/* ------------------------------------------------------------------------------------ PLHEADER */

#define SOF_BITS 0x18D2E82u
static const uint32_t PLS_ROWS[6] = {0x55555555u, 0x33333333u, 0x0F0F0F0Fu, 0x00FF00FFu, 0x0000FFFFu, 0xFFFFFFFFu};
static const char PLS_SCR[65] = "0111000110011101100000111100100101010011010000100010110111111010";

static int8_t (*pls_a)[64];          /* 128 x 64, +-1 dopo lo scrambling (PSRAM: la RAM statica è contata) */
static float sof_a[26];
static cf U90[90];                   /* pi/2 BPSK: h = a * U */
static cf dref_sof[26];              /* D[n] = h[n] conj(h[n-1]) per n = 1..25 */
static cf dref_pair[32];             /* coppie del PLS code, n = 27 + 2i, con il segno per TYPE LSB = 0 */
static bool tables_ready;

static cf cmul(cf a, cf b) { return (cf){a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; }
static cf cmulc(cf a, cf b) { return (cf){a.re * b.re + a.im * b.im, a.im * b.re - a.re * b.im}; }   /* a * conj(b) */
static float cabs2(cf a) { return a.re * a.re + a.im * a.im; }

static void init_tables(void)
{
    if (tables_ready) return;
    pls_a = E->alloc(128 * 64);
    if (!pls_a) return;
    const float s = (float)M_SQRT1_2;
    for (int n = 0; n < 90; n++) U90[n] = (cf){(n % 2 == 0 ? 1.0f : -1.0f) * s, s};
    for (int n = 0; n < 26; n++) sof_a[n] = ((SOF_BITS >> (25 - n)) & 1u) ? -1.0f : 1.0f;
    for (int v = 0; v < 128; v++) {
        int b[7];
        for (int k = 0; k < 7; k++) b[k] = (v >> (6 - k)) & 1;
        uint32_t y = 0;
        for (int k = 0; k < 6; k++) if (b[k]) y ^= PLS_ROWS[k];
        for (int i = 0; i < 32; i++) {
            const int bit = (y >> (31 - i)) & 1;
            const int o0 = bit ^ (PLS_SCR[2 * i] - '0'), o1 = bit ^ b[6] ^ (PLS_SCR[2 * i + 1] - '0');
            pls_a[v][2 * i] = o0 ? -1 : 1;
            pls_a[v][2 * i + 1] = o1 ? -1 : 1;
        }
    }
    for (int n = 1; n < 26; n++) {
        const cf hn = {sof_a[n] * U90[n].re, sof_a[n] * U90[n].im}, hp = {sof_a[n - 1] * U90[n - 1].re, sof_a[n - 1] * U90[n - 1].im};
        dref_sof[n] = cmulc(hn, hp);
    }
    for (int i = 0; i < 32; i++) {
        const int n = 27 + 2 * i;
        const float sign = 1.0f - 2.0f * (float)((PLS_SCR[2 * i] - '0') ^ (PLS_SCR[2 * i + 1] - '0'));
        const cf u = cmulc(U90[n], U90[n - 1]);
        dref_pair[i] = (cf){sign * u.re, sign * u.im};
    }
    tables_ready = true;
}

static cf hdr_sym(int v, int n) { const float a = n < 26 ? sof_a[n] : pls_a[v][n - 26]; return (cf){a * U90[n].re, a * U90[n].im}; }

/* ------------------------------------------------------------------------------------ simboli */

typedef struct {
    int8_t *sym;                     /* simboli complessi int8 interlacciati */
    uint32_t nsym;
    float dphi;                      /* correzione di frequenza per simbolo (rad) */
} symbuf_t;

static inline cf sym_at(const symbuf_t *S, uint32_t k) { return (cf){(float)S->sym[2 * k], (float)S->sym[2 * k + 1]}; }

/* metrica di correlazione differenziale del PLHEADER in m (0..1), e la somma complessa sul SOF (per la frequenza) */
static float hdr_metric(const symbuf_t *S, uint32_t m, cf *sof_sum)
{
    if (m + 90 > S->nsym || m == 0) return 0;
    cf sp = {0, 0}, pp = {0, 0};
    float en = 0;
    cf prev = sym_at(S, m);
    for (int n = 1; n < 90; n++) {
        const cf cur = sym_at(S, m + n);
        if (n < 26 || (n >= 27 && (n & 1))) {
            const cf d = cmulc(cur, prev);
            en += cabs2(d);                                      /* energia: una sola radice alla fine */
            if (n < 26) { const cf t = cmulc(d, dref_sof[n]); sp.re += t.re; sp.im += t.im; }
            else { const cf t = cmulc(d, dref_pair[(n - 27) / 2]); pp.re += t.re; pp.im += t.im; }
        }
        prev = cur;
    }
    if (sof_sum) *sof_sum = sp;
    const float a = cabs2((cf){sp.re + pp.re, sp.im + pp.im}), b = cabs2((cf){sp.re - pp.re, sp.im - pp.im});
    /* |somma| / sqrt(57 * energia): uguale a |somma| / somma dei moduli per simboli di ampiezza costante */
    return en > 0 ? sqrtf((a > b ? a : b) / (57.0f * en)) : 0;
}

/* ------------------------------------------------------------------------------------ spettro */

static void fft(cf *x, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { const cf t = x[i]; x[i] = x[j]; x[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const float a = -2.0f * (float)M_PI / (float)len;
        const cf wl = {cosf(a), sinf(a)};
        for (int i = 0; i < n; i += len) {
            cf w = {1, 0};
            for (int k = 0; k < len / 2; k++) {
                const cf u = x[i + k], v = cmul(x[i + k + len / 2], w);
                x[i + k] = (cf){u.re + v.re, u.im + v.im};
                x[i + k + len / 2] = (cf){u.re - v.re, u.im - v.im};
                w = cmul(w, wl);
            }
        }
    }
}

static int cmp_float(const void *a, const void *b) { const float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }

#define NFFT 1024
static bool detect(const int8_t *iq, uint32_t n, float fs, float mi, float mq, float *center, float *width, float *peak_db)
{
    cf *x = zalloc(NFFT * sizeof(cf));
    float *p = zalloc(NFFT * sizeof(float)), *ps = zalloc(NFFT * sizeof(float)), *srt = zalloc(NFFT * sizeof(float));
    if (!x || !p || !ps || !srt) return false;
    uint32_t nseg = n / NFFT;
    if (nseg > 400) nseg = 400;
    for (uint32_t s = 0; s < nseg; s++) {
        for (int k = 0; k < NFFT; k++) {
            const float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * (float)k / (NFFT - 1));
            const uint32_t i = s * NFFT + (uint32_t)k;
            x[k] = (cf){((float)iq[2 * i] - mi) * w, ((float)iq[2 * i + 1] - mq) * w};
        }
        fft(x, NFFT);
        for (int k = 0; k < NFFT; k++) p[(k + NFFT / 2) % NFFT] += cabs2(x[k]);   /* fftshift */
    }
    if (droop_comp && fs > 0.99e6f && fs < 1.01e6f) {
        /* il decimatore della scheda (somma di 4 + CIC3 /4 a 16 Msps) cala verso i bordi: il rumore ha una forma a
         * cupola che sembrava un segnale (04/11). Si divide per |H(f)|^2, normalizzata in continua */
        for (int k = 0; k < NFFT; k++) {
            const float f = ((float)(k - NFFT / 2) * fs / NFFT) / 16e6f;          /* frequenza normalizzata a 16 Msps */
            const float x4 = (float)M_PI * f * 4, x16 = (float)M_PI * f * 16;
            const float h1 = fabsf(f) < 1e-9f ? 4.0f : sinf(x4) / sinf((float)M_PI * f);       /* somma di 4 */
            const float h3 = fabsf(f) < 1e-9f ? 4.0f : sinf(x16) / sinf(x4);                  /* CIC /4 a 4 Msps */
            float g = (h1 / 4) * (h3 / 4) * (h3 / 4) * (h3 / 4);
            g = g * g;
            if (g < 1e-3f) g = 1e-3f;
            p[k] /= g;
        }
    }
    {   /* la continua della radio varia lentamente: picco stretto al centro, più alto della gobba di un segnale DATV.
         * I bin entro +-12 kHz si sostituiscono con la media dei vicini appena fuori (provato in RF il 04/11) */
        const int c = NFFT / 2, half = (int)(12e3f / (fs / NFFT)) + 1;
        if (c - half - 3 >= 0 && c + half + 3 < NFFT) {
            float a = 0;
            for (int j = 1; j <= 3; j++) a += p[c - half - j] + p[c + half + j];
            a /= 6;
            for (int k = c - half; k <= c + half; k++) p[k] = a;
        }
    }
    for (int k = 0; k < NFFT; k++) {
        float a = 0;
        int c = 0;
        for (int j = k - 15; j <= k + 15; j++) if (j >= 0 && j < NFFT) { a += p[j]; c++; }   /* 31 kHz: i picchi stretti si appiattiscono */
        ps[k] = a / (float)c;
    }
    memcpy(srt, ps, NFFT * sizeof(float));
    qsort(srt, NFFT, sizeof(float), cmp_float);
    const float floor = srt[NFFT / 10];
    int pk = 0;
    for (int k = 1; k < NFFT; k++) if (ps[k] > ps[pk]) pk = k;
    *peak_db = floor > 0 ? 10.0f * log10f(ps[pk] / floor) : 0;
    bool ok = *peak_db >= 3.0f;
    if (ok) {
        const float lvl = floor + (ps[pk] - floor) / 2, df = fs / NFFT;
        int lo = pk, hi = pk;
        while (lo > 0 && ps[lo - 1] > lvl) lo--;
        while (hi < NFFT - 1 && ps[hi + 1] > lvl) hi++;
        const float f_lo = (float)(lo - NFFT / 2) * df - (lo > 0 ? df * (ps[lo] - lvl) / fmaxf(ps[lo] - ps[lo - 1], 1e-30f) : 0);
        const float f_hi = (float)(hi - NFFT / 2) * df + (hi < NFFT - 1 ? df * (ps[hi] - lvl) / fmaxf(ps[hi] - ps[hi + 1], 1e-30f) : 0);
        *center = (f_lo + f_hi) / 2;
        *width = f_hi - f_lo;
    }
    E->release(x); E->release(p); E->release(ps); E->release(srt);
    return ok;
}

/* ------------------------------------------------------------------------------------ filtro adattato e simboli */

static float rrc(float t, float b)
{
    if (fabsf(t) < 1e-6f) return 1.0f - b + 4.0f * b / (float)M_PI;
    if (fabsf(fabsf(t) - 1.0f / (4.0f * b)) < 1e-6f)
        return b / (float)M_SQRT2 * ((1 + 2 / (float)M_PI) * sinf((float)M_PI / (4 * b)) + (1 - 2 / (float)M_PI) * cosf((float)M_PI / (4 * b)));
    const float pt = (float)M_PI * t;
    return (sinf(pt * (1 - b)) + 4 * b * t * cosf(pt * (1 + b))) / (pt * (1 - (4 * b * t) * (4 * b * t)));
}

#define MAXTAP 41
#define LUT_BITS 10
static int16_t *cos_lut, *sin_lut;
static bool lut_ready;

typedef struct {
    int16_t h[MAXTAP];               /* coefficienti Q13 */
    int ntap;
    int16_t bi[2 * MAXTAP], bq[2 * MAXTAP];   /* anello doppio: lettura contigua */
    int pos;
    uint32_t phase, inc;             /* mescolatore: 2^32 = 2 pi */
    int32_t dci, dcq;                /* continua x16 */
    float gain;
} fe_t;

/* virgola fissa: ingresso int8 x16 meno la continua, mescolatore a tabella Q14, FIR con coefficienti Q13 */
static void fe_init(fe_t *f, float fs, float fc, float sr, float mi, float mq)
{
    if (!lut_ready) {
        cos_lut = E->alloc(2u << LUT_BITS);
        sin_lut = E->alloc(2u << LUT_BITS);
        if (!cos_lut || !sin_lut) return;
        for (int i = 0; i < (1 << LUT_BITS); i++) {
            const float a = 2.0f * (float)M_PI * (float)i / (float)(1 << LUT_BITS);
            cos_lut[i] = (int16_t)lroundf(16384.0f * cosf(a));
            sin_lut[i] = (int16_t)lroundf(16384.0f * sinf(a));
        }
        lut_ready = true;
    }
    memset(f, 0, sizeof(*f));
    int half = (int)lroundf(4.0f * fs / sr);                         /* +-4 simboli */
    if (2 * half + 1 > MAXTAP) half = (MAXTAP - 1) / 2;
    f->ntap = 2 * half + 1;
    float hf[MAXTAP], hmax = 0;
    for (int t = 0; t < f->ntap; t++) { hf[t] = rrc((float)(t - half) * sr / fs, 0.35f); if (fabsf(hf[t]) > hmax) hmax = fabsf(hf[t]); }
    for (int t = 0; t < f->ntap; t++) f->h[t] = (int16_t)lroundf(hf[t] / hmax * 8192.0f);
    f->inc = (uint32_t)(int64_t)llroundf(-fc / fs * 4294967296.0f);
    f->dci = (int32_t)lroundf(mi * 16);
    f->dcq = (int32_t)lroundf(mq * 16);
    f->gain = 1.0f / (8192.0f * 16.0f);
}

static inline void fe_seek(fe_t *f, uint32_t n) { f->phase = (uint32_t)((uint64_t)n * f->inc); }

/* un campione d'ingresso -> un campione filtrato */
static inline cf fe_step(fe_t *f, int8_t xi8, int8_t xq8)
{
    const int32_t x = xi8 * 16 - f->dci, y = xq8 * 16 - f->dcq;
    const uint32_t li = f->phase >> (32 - LUT_BITS);
    f->phase += f->inc;
    const int32_t c = cos_lut[li], s = sin_lut[li];
    const int16_t mi = (int16_t)((x * c - y * s) >> 14), mq = (int16_t)((x * s + y * c) >> 14);
    f->bi[f->pos] = mi; f->bi[f->pos + f->ntap] = mi;
    f->bq[f->pos] = mq; f->bq[f->pos + f->ntap] = mq;
    f->pos = f->pos + 1 == f->ntap ? 0 : f->pos + 1;
    const int16_t *bi = &f->bi[f->pos], *bq = &f->bq[f->pos], *h = f->h;
    int32_t ai0 = 0, ai1 = 0, aq0 = 0, aq1 = 0;
    int t = 0;
    for (; t + 1 < f->ntap; t += 2) {                                /* due accumulatori per canale */
        ai0 += bi[t] * h[t]; ai1 += bi[t + 1] * h[t + 1];
        aq0 += bq[t] * h[t]; aq1 += bq[t + 1] * h[t + 1];
    }
    if (t < f->ntap) { ai0 += bi[t] * h[t]; aq0 += bq[t] * h[t]; }
    return (cf){(float)(ai0 + ai1) * f->gain, (float)(aq0 + aq1) * f->gain};
}

#define OM_BLOCK 1024                /* simboli per blocco della stima di temporizzazione */
/* front-end completo su n campioni: due passate; restituisce i simboli (int8, rms ~40) e la pendenza dell'errore
 * di temporizzazione (per correggere l'SR) */
static uint32_t front_end(const int8_t *iq, uint32_t n, float fs, float fc, float sr, float mi, float mq,
                          int8_t *sym, uint32_t sym_max, float *slope, float *pow_out)
{
    const float sps = fs / sr;
    const uint32_t nsym_est = (uint32_t)((float)n / sps);
    const uint32_t nb = nsym_est / OM_BLOCK;
    if (nb < 2) return 0;
    cf *v = zalloc(nb * sizeof(cf));
    float *tau = zalloc(nb * sizeof(float));
    if (!v || !tau) return 0;
    const uint32_t t_fe0 = now_ms();
    fe_t f;
    fe_init(&f, fs, fc, sr, mi, mq);
    /* passata 1: |y|^2 e^{-j 2 pi n sr / fs} (Oerder-Meyr) sui blocchi pari; i dispari si interpolano */
    const uint32_t om_inc = (uint32_t)(int64_t)llroundf(-sr / fs * 4294967296.0f);
    float pw = 0, pwb = 0;                                           /* float: niente double emulati */
    uint32_t np = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t b = (uint32_t)((float)i / sps) / OM_BLOCK;
        if (b & 1u) {                                                /* blocco saltato: si riparte poco prima del pari */
            const uint32_t nxt = (uint32_t)ceilf((float)((b + 1) * OM_BLOCK) * sps);
            if (nxt > (uint32_t)f.ntap + 1 && i < nxt - (uint32_t)f.ntap - 1) { i = nxt - (uint32_t)f.ntap - 1; fe_seek(&f, i + 1); continue; }
        }
        const cf y = fe_step(&f, iq[2 * i], iq[2 * i + 1]);
        if ((b & 1u) || b >= nb) continue;
        const float e = cabs2(y);
        pwb += e;
        if ((np & 1023u) == 1023u) { pw += pwb; pwb = 0; }               /* a blocchi: niente perdita di precisione */
        np++;
        const uint32_t li = (i * om_inc) >> (32 - LUT_BITS);
        v[b].re += e * (float)cos_lut[li];
        v[b].im += e * (float)sin_lut[li];
    }
    pw += pwb;
    const float meanp = pw / (float)(np ? np : 1);
    if (pow_out) *pow_out = meanp;
    for (uint32_t b = 0; b < nb; b += 2) tau[b] = -atan2f(v[b].im, v[b].re) / (2.0f * (float)M_PI);
    for (uint32_t b = 2; b < nb; b += 2) {                           /* svolgimento sui pari */
        float d = tau[b] - tau[b - 2];
        while (d > 0.5f) { tau[b] -= 1.0f; d -= 1.0f; }
        while (d < -0.5f) { tau[b] += 1.0f; d += 1.0f; }
    }
    for (uint32_t b = 1; b < nb; b += 2) tau[b] = b + 1 < nb ? (tau[b - 1] + tau[b + 1]) / 2 : tau[b - 1];
    float *ts = zalloc(nb * sizeof(float));                          /* media mobile di 7 */
    for (uint32_t b = 0; b < nb; b++) {
        float s = 0;
        int c = 0;
        for (int j = (int)b - 3; j <= (int)b + 3; j++) if (j >= 0 && j < (int)nb) { s += tau[j]; c++; }
        ts[b] = s / (float)c;
    }
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (uint32_t b = 0; b < nb; b++) {
        const double x = (b + 0.5) * OM_BLOCK;
        sx += x; sy += ts[b]; sxx += x * x; sxy += x * ts[b];
    }
    *slope = (float)((nb * sxy - sx * sy) / (nb * sxx - sx * sx));    const uint32_t t_p1 = now_ms();
    /* passata 2: simboli per interpolazione cubica sul segnale filtrato (anello di 64 campioni) */
    fe_init(&f, fs, fc, sr, mi, mq);
    cf ring[64];
    uint32_t k = 0, nk = 0;
    const float scale = 40.0f / sqrtf(meanp > 0 ? meanp : 1);
    for (uint32_t i = 0; i < n && nk < sym_max; i++) {
        ring[i & 63] = fe_step(&f, iq[2 * i], iq[2 * i + 1]);
        for (;;) {
            const float bc = (float)k / OM_BLOCK - 0.5f;             /* tau al simbolo k, interpolato tra i blocchi */
            float tk;
            if (bc <= 0) tk = ts[0];
            else if (bc >= (float)(nb - 1)) tk = ts[nb - 1];
            else { const uint32_t b0 = (uint32_t)bc; const float fr = bc - (float)b0; tk = ts[b0] * (1 - fr) + ts[b0 + 1] * fr; }
            const float t = ((float)k + tk) * sps;
            const int32_t i0 = (int32_t)floorf(t);
            if (i0 + 2 > (int32_t)i) break;                          /* serve ancora un campione */
            if (i0 - 1 < (int32_t)i - 60) { k++; continue; }           /* troppo vecchio (solo all'inizio) */
            if (i0 < 1) { k++; continue; }
            const float mu = t - (float)i0;
            const cf xm1 = ring[(i0 - 1) & 63], x0 = ring[i0 & 63], x1 = ring[(i0 + 1) & 63], x2 = ring[(i0 + 2) & 63];
            cf r;
            #define CUB(c) ((((-0.5f * xm1.c + 1.5f * x0.c - 1.5f * x1.c + 0.5f * x2.c) * mu + (xm1.c - 2.5f * x0.c + 2.0f * x1.c - 0.5f * x2.c)) * mu + (-0.5f * xm1.c + 0.5f * x1.c)) * mu + x0.c)
            r.re = CUB(re);
            r.im = CUB(im);
            #undef CUB
            float qi = r.re * scale, qq = r.im * scale;
            qi = qi > 127 ? 127 : qi < -127 ? -127 : qi;
            qq = qq > 127 ? 127 : qq < -127 ? -127 : qq;
            sym[2 * nk] = (int8_t)lroundf(qi);
            sym[2 * nk + 1] = (int8_t)lroundf(qq);
            nk++;
            k++;
            if (nk >= sym_max) break;
        }
    }
    lg("  front-end %u campioni: passata 1 %u ms, passata 2 %u ms (%d coefficienti)", (unsigned)n,
       (unsigned)(t_p1 - t_fe0), (unsigned)(now_ms() - t_p1), f.ntap);
    E->release(v); E->release(tau); E->release(ts);
    return nk;
}

/* ------------------------------------------------------------------------------------ geometria del frame */

typedef struct {
    int v, modcod, shortf, pilots, ri;
    uint32_t L, nldpc, slots, ndata;
    const pdx_s2_table_t *tab;
} geom_t;

static bool geom(int v, geom_t *g)
{
    memset(g, 0, sizeof(*g));
    g->v = v; g->modcod = v >> 2; g->shortf = (v >> 1) & 1; g->pilots = v & 1;
    if (g->modcod < 1 || g->modcod > 11) return false;
    g->ri = g->modcod - 1;
    if (g->shortf && g->ri == 10) return false;
    g->tab = &pdx_s2_tables[g->shortf ? 11 + g->ri : g->ri];
    g->nldpc = g->shortf ? 16200 : 64800;
    g->slots = g->nldpc / 2 / 90;
    g->ndata = g->nldpc / 2;
    g->L = 90 * (g->slots + 1) + (g->pilots ? 36 * ((g->slots - 1) / 16) : 0);
    return true;
}

static uint32_t frame_len(int v) { geom_t g; if (geom(v, &g)) return g.L; return (v >> 2) == 0 ? 3330 : 0; }

/* indice nel frame (dal PLHEADER) del simbolo di dati d */
static inline uint32_t data_pos(const geom_t *g, uint32_t d)
{
    if (!g->pilots) return 90 + d;
    const uint32_t blk = d / (16 * 90);
    return 90 + d + blk * 36;
}

/* ------------------------------------------------------------------------------------ scrambling PL (Gold n = 0) */

static uint8_t *gold_R;              /* R(i) per i = 0..gold_len-1 */
static uint32_t gold_len;

static bool gold_init(uint32_t len)
{
    if (gold_R && gold_len >= len) return true;
    if (gold_R) E->release(gold_R);
    const uint32_t need = 131072u + len + 18;
    uint8_t *x = zalloc(need), *y = zalloc(need);
    gold_R = zalloc(len);
    if (!x || !y || !gold_R) return false;
    x[0] = 1;
    for (uint32_t i = 0; i < 18; i++) y[i] = 1;
    for (uint32_t i = 0; i + 18 < need; i++) {
        x[i + 18] = x[i + 7] ^ x[i];
        y[i + 18] = y[i + 10] ^ y[i + 7] ^ y[i + 5] ^ y[i];
    }
    for (uint32_t k = 0; k < len; k++) gold_R[k] = (uint8_t)(2 * (x[k + 131072] ^ y[k + 131072]) + (x[k] ^ y[k]));
    gold_len = len;
    E->release(x); E->release(y);
    return true;
}

static const cf JM[4] = {{1, 0}, {0, -1}, {-1, 0}, {0, 1}};          /* j^(-R) */

/* ------------------------------------------------------------------------------------ LDPC a strati */

typedef struct {
    int ri, shortf;
    uint32_t n, k, nk, nedge;
    uint32_t *rowptr;                /* nk + 1 */
    uint16_t *col;                   /* nedge */
    int16_t *R;                      /* messaggi check -> variabile */
    int16_t *Lv;                     /* LLR totali (|Lv| < 4096: 16 bit bastano) */
} ldpc_t;

static ldpc_t LD;
static uint32_t *par_bits;            /* bitset della parità (PSRAM), allocato con l'LDPC */
static const pdx_s2_table_t *LD_tab;

static bool ldpc_build(const geom_t *g)
{
    if (LD.rowptr && LD.ri == g->ri && LD.shortf == g->shortf) return true;
    if (LD.rowptr) { E->release(LD.rowptr); E->release(LD.col); E->release(LD.R); E->release(LD.Lv); memset(&LD, 0, sizeof(LD)); }
    const pdx_s2_table_t *t = g->tab;
    LD_tab = t;
    if (!par_bits) par_bits = E->alloc(((48600 / 32) + 2) * 4);
    if (!par_bits) return false;
    LD.ri = g->ri; LD.shortf = g->shortf; LD.n = t->nldpc; LD.k = t->kldpc; LD.nk = t->nldpc - t->kldpc;
    LD.rowptr = zalloc((LD.nk + 1) * sizeof(uint32_t));
    if (!LD.rowptr) return false;
    /* conteggio: indirizzi delle tabelle (bit di informazione) + parità p_j (riga j) + p_(j-1) (riga j) */
    uint32_t a = 0;
    for (uint32_t r = 0; r < t->nrows; r++) {
        for (uint32_t c = 0; c < t->rowlen[r]; c++, a++) {
            const uint32_t x = t->addr[a];
            for (uint32_t m = 0; m < 360; m++) LD.rowptr[(x + m * t->q) % LD.nk + 1]++;
        }
    }
    for (uint32_t j = 0; j < LD.nk; j++) LD.rowptr[j + 1] += j ? 2 : 1;
    for (uint32_t j = 0; j < LD.nk; j++) LD.rowptr[j + 1] += LD.rowptr[j];
    LD.nedge = LD.rowptr[LD.nk];
    LD.col = E->alloc(LD.nedge * sizeof(uint16_t));
    LD.R = E->alloc(LD.nedge * sizeof(int16_t));
    LD.Lv = E->alloc(LD.n * sizeof(int16_t));
    uint32_t *fill = zalloc(LD.nk * sizeof(uint32_t));
    if (!LD.col || !LD.R || !LD.Lv || !fill) return false;
    a = 0;
    for (uint32_t r = 0; r < t->nrows; r++) {
        for (uint32_t c = 0; c < t->rowlen[r]; c++, a++) {
            const uint32_t x = t->addr[a];
            for (uint32_t m = 0; m < 360; m++) {
                const uint32_t j = (x + m * t->q) % LD.nk;
                LD.col[LD.rowptr[j] + fill[j]++] = (uint16_t)(360 * r + m);
            }
        }
    }
    for (uint32_t j = 0; j < LD.nk; j++) {
        LD.col[LD.rowptr[j] + fill[j]++] = (uint16_t)(LD.k + j);
        if (j) LD.col[LD.rowptr[j] + fill[j]++] = (uint16_t)(LD.k + j - 1);
    }
    E->release(fill);
    return true;
}

/* equazioni non soddisfatte dalle decisioni (segno di Lv), calcolate come il codificatore: i bit di informazione in
 * ordine accumulano la parità in un bitset di 2 KB (RAM interna), senza percorrere gli archi in PSRAM */
static uint32_t ldpc_syndrome_lv(void)
{
    const pdx_s2_table_t *t = LD_tab;
    const uint32_t nk = LD.nk, words = nk / 32 + 1;
    memset(par_bits, 0, words * 4);
    uint32_t a0 = 0;
    for (uint32_t r = 0; r < t->nrows; r++) {
        const uint32_t rl = t->rowlen[r];
        const uint16_t *ad = t->addr + a0;
        const int16_t *lv = LD.Lv + 360 * r;
        for (uint32_t m = 0; m < 360; m++) {
            if (lv[m] >= 0) continue;
            const uint32_t mq = m * t->q;
            for (uint32_t c = 0; c < rl; c++) {
                uint32_t j = ad[c] + mq;
                if (j >= nk) j %= nk;
                par_bits[j >> 5] ^= 1u << (j & 31);
            }
        }
        a0 += rl;
    }
    uint32_t bad = 0, prev = 0;
    const int16_t *p = LD.Lv + LD.k;
    for (uint32_t j = 0; j < nk; j++) {
        const uint32_t pj = p[j] < 0;
        bad += ((par_bits[j >> 5] >> (j & 31)) & 1u) ^ pj ^ prev;
        prev = pj;
    }
    return bad;
}

#define LLR_MAX 2047
static int ldpc_decode(int max_iter, bool *ok)
{
    memset(LD.R, 0, LD.nedge * sizeof(int16_t));
    int16_t q[64];
    uint32_t best = 0xFFFFFFFFu;
    int stall = 0;
    for (int it = 0; it <= max_iter; it++) {
        const uint32_t w = ldpc_syndrome_lv();
        if (w == 0) { *ok = true; return it; }
        if (it == max_iter) break;
        /* niente miglioramenti per 3 iterazioni: ci si ferma (di solito il disturbo è nella parità a scala e i dati
         * sono già giusti: lo dicono i CRC) */
        if (w < best) { best = w; stall = 0; } else if (++stall >= 3) { *ok = false; return it; }
        for (uint32_t j = 0; j < LD.nk; j++) {
            const uint32_t e0 = LD.rowptr[j], d = LD.rowptr[j + 1] - e0;
            int32_t m1 = 1 << 30, m2 = 1 << 30;
            uint32_t pos = 0, sg = 0;
            for (uint32_t i = 0; i < d; i++) {
                int32_t x = LD.Lv[LD.col[e0 + i]] - LD.R[e0 + i];
                x = x > LLR_MAX ? LLR_MAX : x < -LLR_MAX ? -LLR_MAX : x;
                q[i] = (int16_t)x;
                const int32_t ax = x < 0 ? -x : x;
                sg ^= x < 0;
                if (ax < m1) { m2 = m1; m1 = ax; pos = i; } else if (ax < m2) m2 = ax;
            }
            m1 = (m1 * 3) >> 2;                                    /* min-sum normalizzato, alfa 0,75 */
            m2 = (m2 * 3) >> 2;
            for (uint32_t i = 0; i < d; i++) {
                const int32_t mag = i == pos ? m2 : m1;
                const int32_t r = (sg ^ (q[i] < 0)) ? -mag : mag;
                LD.R[e0 + i] = (int16_t)r;
                LD.Lv[LD.col[e0 + i]] = (int16_t)(q[i] + r);
            }
        }
    }
    *ok = false;
    return max_iter;
}

/* sindrome dalle decisioni grezze sui simboli (per scegliere il quadrante senza pilot) */
static uint32_t syndrome_sym(const cf *s, uint32_t nd)
{
    for (uint32_t i = 0; i < nd; i++) { LD.Lv[2 * i] = s[i].re < 0 ? -1 : 1; LD.Lv[2 * i + 1] = s[i].im < 0 ? -1 : 1; }
    return ldpc_syndrome_lv();
}

/* ------------------------------------------------------------------------------------ BB e TS */

static uint8_t crc8(const uint8_t *p, uint32_t n)
{
    uint8_t c = 0;
    for (uint32_t i = 0; i < n; i++)
        for (int k = 7; k >= 0; k--) {
            const int fb = ((c >> 7) & 1) ^ ((p[i] >> k) & 1);
            c = (uint8_t)(c << 1);
            if (fb) c ^= 0xD5;
        }
    return c;
}

typedef struct {
    uint8_t buf[188 * 2];
    uint32_t nbuf;
    bool aligned, have_prev;
    uint8_t prev[188];
    uint8_t *out, *ok;
    uint32_t nout, max, crc_ok, crc_bad;
} tsasm_t;

static void ts_push(tsasm_t *a, const uint8_t *d, uint32_t len, uint32_t syncd, bool good)
{
    if (!good) { a->aligned = false; a->have_prev = false; a->nbuf = 0; return; }
    uint32_t i = 0;
    if (!a->aligned) {
        if (syncd == 65535 || (syncd & 7)) return;
        i = syncd / 8;
        a->aligned = true;
        a->nbuf = 0;
    }
    for (; i < len; i++) {
        a->buf[a->nbuf++] = d[i];
        if (a->nbuf < 188) continue;
        a->nbuf = 0;
        uint8_t flag = 2;
        if (a->have_prev) {
            const bool g = crc8(a->prev + 1, 187) == a->buf[0];
            if (g) a->crc_ok++; else a->crc_bad++;
            if (a->ok && a->nout) a->ok[a->nout - 1] = g ? 1 : 0;   /* il CRC protegge il pacchetto precedente */
        }
        memcpy(a->prev, a->buf, 188);
        a->have_prev = true;
        if (a->out && a->nout < a->max) {
            memcpy(a->out + 188 * a->nout, a->buf, 188);
            a->out[188 * a->nout] = 0x47;
            if (a->ok) a->ok[a->nout] = flag;
        }
        if (a->nout < a->max) a->nout++;
    }
}

static void dvb_str(char *dst, size_t n, const uint8_t *s, uint32_t len)
{
    if (len && s[0] < 0x20) { s++; len--; }
    if (len >= n) len = (uint32_t)n - 1;
    memcpy(dst, s, len);
    dst[len] = 0;
    for (char *p = dst; *p; p++) if ((unsigned char)*p < 0x20) *p = ' ';
}

static void parse_ts(const uint8_t *ts, uint32_t np, s2rx_report_t *rep)
{
    int pmt_pid = -1;
    bool have_sdt = false, have_pmt = false;
    for (uint32_t k = 0; k < np && !(have_sdt && have_pmt); k++) {
        const uint8_t *p = ts + 188 * k;
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (!(p[1] & 0x40)) continue;                                /* solo sezioni che iniziano nel pacchetto */
        if (pid != 0 && pid != 0x11 && pid != pmt_pid) continue;
        const int afc = (p[3] >> 4) & 3;
        int off = 4 + ((afc & 2) ? 1 + p[4] : 0);
        if (!(afc & 1) || off >= 188) continue;
        off += 1 + p[off];                                           /* pointer field */
        if (off + 3 > 188) continue;
        const uint8_t *s = p + off;
        const int slen = ((s[1] & 0x0F) << 8) | s[2];
        if (off + 3 + slen > 188) continue;
        if (s[0] == 0x00 && pmt_pid < 0) {
            for (int i = 8; i + 4 <= 3 + slen - 4; i += 4)
                if ((s[i] << 8 | s[i + 1]) != 0) { pmt_pid = ((s[i + 2] & 0x1F) << 8) | s[i + 3]; break; }
        } else if (s[0] == 0x02 && !have_pmt) {
            const int pil = ((s[10] & 0x0F) << 8) | s[11];
            for (int i = 12 + pil; i + 5 <= 3 + slen - 4;) {
                const uint8_t st = s[i];
                const int el = ((s[i + 3] & 0x0F) << 8) | s[i + 4];
                if (st == 0x1B || st == 0x24 || st == 0x02) { if (!rep->vtype) rep->vtype = st; }
                else if (!rep->atype) rep->atype = st;
                i += 5 + el;
            }
            have_pmt = true;
        } else if (s[0] == 0x42 && !have_sdt) {
            for (int i = 11; i + 5 <= 3 + slen - 4;) {
                const int dl = ((s[i + 3] & 0x0F) << 8) | s[i + 4];
                for (int j = i + 5; j + 2 <= i + 5 + dl;) {
                    const uint8_t tag = s[j], ln = s[j + 1];
                    if (tag == 0x48 && ln >= 3) {
                        const uint8_t *d = s + j + 2;
                        const uint8_t pl = d[1];
                        dvb_str(rep->provider, sizeof(rep->provider), d + 2, pl);
                        dvb_str(rep->service, sizeof(rep->service), d + 3 + pl, d[2 + pl]);
                        have_sdt = true;
                    }
                    j += 2 + ln;
                }
                i += 5 + dl;
            }
        }
    }
}

/* nominativo: prima parola del nome del servizio (o del fornitore) con lettere e almeno una cifra */
static void find_call(s2rx_report_t *rep)
{
    const char *src[2] = {rep->service, rep->provider};
    for (int s = 0; s < 2; s++) {
        const char *p = src[s];
        while (*p) {
            while (*p == ' ') p++;
            const char *w = p;
            int dig = 0, let = 0, len = 0;
            while (*p && *p != ' ') {
                const char c = *p;
                if (c >= '0' && c <= '9') dig++;
                else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) let++;
                else if (c != '/' && c != '-') { let = -99; }
                len++;
                p++;
            }
            if (dig >= 1 && let >= 2 && len >= 3 && len < (int)sizeof(rep->call)) {
                memcpy(rep->call, w, (size_t)len);
                rep->call[len] = 0;
                for (char *c = rep->call; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
                return;
            }
        }
    }
}

/* ------------------------------------------------------------------------------------ demodulazione di un frame */

typedef struct {
    cf *y;                           /* simboli del frame (con la frequenza corretta), L + 90 */
    cf *s;                           /* simboli di dati derotati */
    float *ph;                       /* fase per simbolo di dati */
    float *loc;                      /* rumore locale */
    uint8_t *bits;
    uint8_t *df;                     /* DATA FIELD */
} work_t;

static float wrap_to(float x, float ref, float period) { return x + period * roundf((ref - x) / period); }

static void frame_symbols(const symbuf_t *S, uint32_t m, uint32_t cnt, cf *y)
{
    const float a0 = S->dphi * (float)m;
    cf ph = {cosf(-a0), sinf(-a0)};
    const cf rot = {cosf(-S->dphi), sinf(-S->dphi)};
    for (uint32_t i = 0; i < cnt; i++) {
        y[i] = (m + i < S->nsym) ? cmul(sym_at(S, m + i), ph) : (cf){0, 0};
        ph = cmul(ph, rot);
    }
}

static float block_phase(const cf *y, int v, uint32_t at, uint32_t cnt, const cf *known)
{
    cf a = {0, 0};
    for (uint32_t i = 0; i < cnt; i++) {
        const cf h = known ? known[i] : hdr_sym(v, (int)i);
        const cf t = cmulc(y[at + i], h);
        a.re += t.re; a.im += t.im;
    }
    return atan2f(a.im, a.re);
}

typedef struct { float mer; int iter; bool ok; } fres_t;

static bool demod_frame(const symbuf_t *S, uint32_t m, int v, bool has_next, int vn, work_t *W, fres_t *res)
{
    geom_t g;
    if (!geom(v, &g)) return false;
    if (m + g.L + (has_next ? 90 : 0) > S->nsym) return false;
    if (!gold_init(g.L) || !ldpc_build(&g)) return false;
    const uint32_t t_d0 = now_ms();
    frame_symbols(S, m, g.L + (has_next ? 90 : 0), W->y);
    const float ph0 = block_phase(W->y, v, 0, 90, NULL);
    const float phn = has_next ? block_phase(W->y, vn, g.L, 90, NULL) : 0;
    const float Q = (float)M_PI / 2;
    if (g.pilots) {
        /* fasi: header (45), pilot (centro del blocco), header successivo; svolte e interpolate */
        float pts[40], phs[40];
        int np = 0;
        pts[np] = 45; phs[np++] = ph0;
        const cf pil = {(float)M_SQRT1_2, (float)M_SQRT1_2};
        for (uint32_t b = 0; 16 * (b + 1) < g.slots; b++) {
            const uint32_t at = 90 + 90 * 16 * (b + 1) + 36 * b;
            cf acc = {0, 0};
            for (uint32_t i = 0; i < 36; i++) {
                const cf known = cmulc(pil, JM[gold_R[at + i - 90]]);   /* p * j^R */
                const cf t = cmulc(W->y[at + i], known);
                acc.re += t.re; acc.im += t.im;
            }
            pts[np] = (float)at + 17.5f; phs[np] = atan2f(acc.im, acc.re); np++;
        }
        if (has_next) { pts[np] = (float)g.L + 45; phs[np++] = phn; }
        for (int i = 1; i < np; i++) phs[i] = wrap_to(phs[i], phs[i - 1], 2 * (float)M_PI);
        for (uint32_t d = 0; d < g.ndata; d++) {
            const float p = (float)data_pos(&g, d);
            int i = 0;
            while (i < np - 2 && p > pts[i + 1]) i++;
            const float fr = (p - pts[i]) / (pts[i + 1] - pts[i]);
            W->ph[d] = np > 1 ? phs[i] + (phs[i + 1] - phs[i]) * fr : phs[0];
        }
        for (uint32_t d = 0; d < g.ndata; d++) {
            const uint32_t pos = data_pos(&g, d);
            const cf r = {cosf(-W->ph[d]), sinf(-W->ph[d])};
            W->s[d] = cmul(cmul(W->y[pos], r), JM[gold_R[pos - 90]]);
        }
    } else {
        /* quarta potenza per slot; catena in avanti dal PLHEADER, all'indietro dal successivo se c'è un salto */
        const uint32_t ns = g.slots;
        float *p4 = W->ph, *rel = W->ph + 400, *fw = W->ph + 800, *bw = W->ph + 1200;   /* ph è libero fino alla fine */
        for (uint32_t sl = 0; sl < ns; sl++) {
            cf q = {0, 0};
            float den = 0;
            for (uint32_t i = 0; i < 90; i++) {
                const uint32_t pos = 90 + 90 * sl + i;
                cf z = cmul(W->y[pos], JM[gold_R[pos - 90]]);
                const cf z2 = cmul(z, z), z4 = cmul(z2, z2);
                q.re -= z4.re; q.im -= z4.im;
                den += cabs2(z2);
            }
            p4[sl] = atan2f(q.im, q.re) / 4;
            rel[sl] = sqrtf(cabs2(q)) / (den + 1e-12f);
        }
        float c = ph0;
        for (uint32_t sl = 0; sl < ns; sl++) { fw[sl] = c = wrap_to(p4[sl], c, Q); }
        if (has_next) {
            const float k = roundf((phn - fw[ns - 1]) / Q);
            if (((int)k % 4 + 4) % 4 != 0) {
                c = phn;
                for (int sl = (int)ns - 1; sl >= 0; sl--) bw[sl] = c = wrap_to(p4[sl], c, Q);
                uint32_t cut = 0;
                for (uint32_t sl = 1; sl < ns; sl++) if (rel[sl] < rel[cut]) cut = sl;
                for (uint32_t sl = cut; sl < ns; sl++) fw[sl] = bw[sl];
            }
        }
        /* punti (indice nei dati): header a -45, slot sl a 45 + 90 sl, header successivo a ndata + 45 */
        const float tail = has_next ? wrap_to(phn, fw[ns - 1], 2 * (float)M_PI) : fw[ns - 1];
        /* tra due punti la fase è lineare: un fasore iniziale e un passo costante per simbolo (niente seno e coseno
         * per ogni simbolo) */
        for (uint32_t kn = 0; kn <= ns; kn++) {
            const float x0 = kn == 0 ? -45.0f : 45.0f + 90.0f * (float)(kn - 1);
            const float v0 = kn == 0 ? ph0 : fw[kn - 1];
            float x1, v1;
            if (kn < ns) { x1 = 45.0f + 90.0f * (float)kn; v1 = fw[kn]; }
            else { x1 = (float)g.ndata + 45.0f; v1 = has_next ? tail : v0; }
            const int32_t d0 = x0 < 0 ? 0 : (int32_t)ceilf(x0);
            int32_t d1 = (int32_t)ceilf(x1);
            if (d1 > (int32_t)g.ndata) d1 = (int32_t)g.ndata;
            if (d0 >= d1) continue;
            const float slope = (v1 - v0) / (x1 - x0);
            const float p0 = v0 + slope * ((float)d0 - x0);
            cf r = {cosf(-p0), sinf(-p0)};
            const cf step = {cosf(-slope), sinf(-slope)};
            for (int32_t d = d0; d < d1; d++) {
                const uint32_t pos = 90 + (uint32_t)d;
                W->s[d] = cmul(cmul(W->y[pos], r), JM[gold_R[pos - 90]]);
                r = cmul(r, step);
            }
        }
        /* dopo ogni tratto disturbato: rotazione del resto scelta con la sindrome */
        uint32_t best = syndrome_sym(W->s, g.ndata);
        for (uint32_t sl = 0; sl < ns && best; sl++) {
            if (rel[sl] >= 0.6f || (sl && rel[sl - 1] < 0.6f)) continue;
            uint32_t bw_best = best;
            int br = 0;
            for (int r = 1; r < 4; r++) {
                for (uint32_t d = 90 * sl; d < g.ndata; d++) W->s[d] = cmul(W->s[d], (cf){0, 1});   /* +90 gradi */
                const uint32_t w = syndrome_sym(W->s, g.ndata);
                if (w < bw_best) { bw_best = w; br = r; }
            }
            for (uint32_t d = 90 * sl; d < g.ndata; d++) W->s[d] = cmul(W->s[d], (cf){0, 1});       /* torna a 0 */
            for (int r = 0; r < br; r++) for (uint32_t d = 90 * sl; d < g.ndata; d++) W->s[d] = cmul(W->s[d], (cf){0, 1});
            best = bw_best;
        }
    }
    /* MER (momenti M2/M4), rumore locale, LLR */
    float m2 = 0, m4 = 0;
    for (uint32_t d = 0; d < g.ndata; d++) { const float e = cabs2(W->s[d]); m2 += e; m4 += e * e; }
    m2 /= g.ndata; m4 /= g.ndata;
    float S2 = 2 * m2 * m2 - m4;
    if (S2 < 1e-12f * m2 * m2) S2 = 1e-12f * m2 * m2;
    const float Sg = sqrtf(S2);
    float N = m2 - Sg;
    if (N < 1e-9f * m2) N = 1e-9f * m2;
    const float A = sqrtf(Sg / 2), sig2 = N / 2;
    res->mer = 10.0f * log10f(Sg / N);
    const uint32_t win = 180;
    float run = 0;
    for (uint32_t d = 0; d < g.ndata; d++) {
        const float ei = W->s[d].re - (W->s[d].re < 0 ? -A : A), eq = W->s[d].im - (W->s[d].im < 0 ? -A : A);
        W->loc[d] = (ei * ei + eq * eq) / 2;
    }
    for (uint32_t d = 0; d < g.ndata + win / 2; d++) {             /* media mobile centrata */
        if (d < g.ndata) run += W->loc[d];
        if (d >= win) run -= W->loc[d - win];
        if (d >= win / 2) {
            const uint32_t c = d - win / 2;
            const uint32_t lo = d >= win ? d - win + 1 : 0, hi = d < g.ndata ? d : g.ndata - 1;
            float v = run / (float)(hi - lo + 1);
            W->ph[c] = v > sig2 ? v : sig2;                          /* riuso di ph come rumore locale */
        }
    }
    const float llr_scale = 4.0f;                                    /* LLR in quarti */
    for (uint32_t d = 0; d < g.ndata; d++) {
        const float k = 2 * A / W->ph[d] * llr_scale;
        float li = W->s[d].re * k, lq = W->s[d].im * k;
        li = li > LLR_MAX ? LLR_MAX : li < -LLR_MAX ? -LLR_MAX : li;
        lq = lq > LLR_MAX ? LLR_MAX : lq < -LLR_MAX ? -LLR_MAX : lq;
        LD.Lv[2 * d] = (int16_t)lrintf(li);
        LD.Lv[2 * d + 1] = (int16_t)lrintf(lq);
    }
    const uint32_t t_l0 = now_ms();
    res->iter = ldpc_decode(10, &res->ok);
    lg("  frame: fase+LLR %u ms, LDPC %u ms (%d iterazioni, %s)", (unsigned)(t_l0 - t_d0), (unsigned)(now_ms() - t_l0),
       res->iter, res->ok ? "ok" : "no");
    for (uint32_t i = 0; i < g.tab->kbch; i++) W->bits[i] = LD.Lv[i] < 0;
    return true;
}

/* ------------------------------------------------------------------------------------ catena completa */

static uint32_t refine(const symbuf_t *S, int64_t m, int search, float thr, int v0, int *v_out)
{
    if (m < 1) return 0;
    float best = 0;
    uint32_t bk = 0;
    for (int64_t k = m - search; k <= m + search; k++) {
        if (k < 1 || k + 90 > (int64_t)S->nsym) continue;
        const float x = hdr_metric(S, (uint32_t)k, NULL);
        if (x > best) { best = x; bk = (uint32_t)k; }
    }
    if (best < thr || !bk) return 0;
    /* PLS code. La frequenza residua non è ancora corretta (S->dphi arriva dopo): con 2 kHz a 333 kS la fase ruota di
     * oltre 90° lungo l'header e il MODCOD si leggeva sbagliato (3/4 letto 2/3, RF del 04/11). Rotazione per simbolo
     * dalla correlazione differenziale del SOF, poi fase in coerenza */
    /* fase coerente delle due metà del SOF (13 simboli ciascuna): la differenza / 13 dà la rotazione per simbolo con
     * poco rumore (la stima differenziale simbolo per simbolo era troppo rumorosa) */
    cf h1 = {0, 0}, h2 = {0, 0};
    for (int n = 0; n < 26; n++) {
        const cf t = cmulc(sym_at(S, bk + n), hdr_sym(0, n));
        if (n < 13) { h1.re += t.re; h1.im += t.im; } else { h2.re += t.re; h2.im += t.im; }
    }
    const cf dh = cmulc(h2, h1);
    const float w = atan2f(dh.im, dh.re) / 13.0f;
    const cf a = {h1.re + h2.re, h1.im + h2.im};
    const float ph = atan2f(a.im, a.re) - w * 12.5f;                 /* fase riferita al simbolo 0 */
    float aa[64], tot = 0;
    for (int n = 26; n < 90; n++) {
        const cf r = {cosf(-ph - w * n), sinf(-ph - w * n)};
        const cf z = cmulc(cmul(sym_at(S, bk + n), r), U90[n]);
        aa[n - 26] = z.re; tot += fabsf(z.re);
    }
    int bv = 0;
    float bc = -1e30f;
    for (int v = 0; v < 128; v++) {
        float c = 0;
        for (int i = 0; i < 64; i++) c += pls_a[v][i] * aa[i];
        if (c > bc) { bc = c; bv = v; }
    }
    if (tot <= 0 || bc / tot < 0.45f) return 0;
    if (v0 >= 0 && bv != v0 && (bv >> 2) != 0) return 0;
    *v_out = bv;
    return bk;
}

/* a fine decodifica: tutta la memoria grande torna libera (la prossima cattura può essere più lunga) */
static void release_all(void)
{
    if (LD.rowptr) { E->release(LD.rowptr); E->release(LD.col); E->release(LD.R); E->release(LD.Lv); }
    memset(&LD, 0, sizeof(LD));
    if (gold_R) { E->release(gold_R); gold_R = NULL; gold_len = 0; }
}
#define MAXHDR 128
uint32_t s2rx_run(const int8_t *iq, uint32_t n, float fs, uint32_t sr_hint, const s2rx_env_t *env,
                  s2rx_report_t *rep, uint8_t *ts_out, uint32_t ts_max, uint8_t *ts_ok)
{
    E = env;
    memset(rep, 0, sizeof(*rep));
    init_tables();
    const uint32_t t0 = now_ms();
    int64_t si = 0, sq = 0;
    for (uint32_t i = 0; i < n; i++) { si += iq[2 * i]; sq += iq[2 * i + 1]; }
    const float mi = (float)((double)si / n), mq = (float)((double)sq / n);
    float fc, width, pk;
    if (!detect(iq, n, fs, mi, mq, &fc, &width, &pk)) { lg("nessun segnale"); return 0; }
    rep->peak_db = pk; rep->width_hz = width;
    /* symbol rate */
    float sr = (float)sr_hint;
    if (!sr_hint) {
        uint32_t near = SR_STD[0];
        for (uint32_t i = 0; i < NSR; i++) if (fabsf((float)SR_STD[i] - width) / SR_STD[i] < fabsf((float)near - width) / near) near = SR_STD[i];
        if (fabsf((float)near - width) / near < 0.10f) sr = (float)near;   /* entro il 10%: niente prove */
        else {
            float bm = -1;
            const uint32_t nprobe = (uint32_t)(0.15f * fs) < n ? (uint32_t)(0.15f * fs) : n;
            int8_t *tmp = E->alloc(2 * (size_t)(nprobe / 2 + 16));
            for (uint32_t i = 0; i < NSR && tmp; i++) {
                const float s = (float)SR_STD[i];
                if (s < 0.65f * width || s > 1.35f * width || 2 * s > fs) continue;
                float slope;
                symbuf_t P = {tmp, front_end(iq, nprobe, fs, fc, s, mi, mq, tmp, nprobe / 2, &slope, NULL), 0};
                float best = 0;
                for (uint32_t k = 1; k + 90 < P.nsym; k++) { const float x = hdr_metric(&P, k, NULL); if (x > best) best = x; }
                if (best > bm) { bm = best; sr = s; }
            }
            if (tmp) E->release(tmp);
            if (sr <= 0) sr = width;
        }
    }
    lg("segnale: centro %+.0f Hz, larghezza %.0f Hz -> SR %.0f, picco %.1f dB", fc, width, sr, pk);
    if (sr < 50e3f || sr > fs / 2) { lg("SR %.0f fuori dai limiti: non è DVB-S2", sr); return 0; }
    /* front-end; seconda passata se l'SR va corretto */
    const uint32_t symmax = (uint32_t)((float)n / (fs / sr)) + 64;
    symbuf_t S = {E->alloc(2 * (size_t)symmax), 0, 0};
    if (!S.sym) { lg("memoria per i simboli"); return 0; }
    float slope = 0, pw;
    for (int pass = 0; pass < 2; pass++) {
        S.nsym = front_end(iq, n, fs, fc, sr, mi, mq, S.sym, symmax, &slope, &pw);
        if (fabsf(slope) < 30e-6f) break;
        sr = sr / (1 + slope);
    }
    rep->sr = (uint32_t)lroundf(sr / (1 + slope));
    rep->ms_front = now_ms() - t0;
    /* primo PLHEADER: massimo della metrica su due frame lunghi */
    /* fino a 6 frame lunghi (~0,6 s a 333 kS): la cattura può iniziare prima del segnale */
    uint32_t lim = 6 * 33282 + 90 < S.nsym - 90 ? 6 * 33282 + 90 : S.nsym - 90;
    float best = 0;
    uint32_t m0 = 0;
    for (uint32_t k = 1; k < lim; k++) { const float x = hdr_metric(&S, k, NULL); if (x > best) { best = x; m0 = k; } }
    int v0 = -1;
    if (best < 0.30f || !refine(&S, m0, 0, 0.30f, -1, &v0)) { lg("nessun PLHEADER"); E->release(S.sym); return 0; }
    {
        geom_t gv;
        if (!geom(v0, &gv)) {                                        /* MODCOD non QPSK (o falso aggancio) */
            lg("primo PLHEADER con MODCOD %d non gestito: segnale non riconosciuto", v0 >> 2);
            E->release(S.sym);
            return 0;
        }
    }
    static uint32_t hm[MAXHDR];
    static int hv[MAXHDR];
    int nh = 0;
    /* all'indietro fino all'inizio, poi in avanti */
    uint32_t back[MAXHDR];
    int nb = 0, vb[MAXHDR];
    {
        int64_t m = m0;
        for (;;) {
            int v = -1;
            uint32_t k = refine(&S, m - (int64_t)frame_len(v0), 4, 0.24f, v0, &v);
            if (!k) k = refine(&S, m - 3330, 4, 0.24f, v0, &v);
            if (!k || nb >= MAXHDR) break;
            back[nb] = k; vb[nb++] = v; m = k;
        }
    }
    for (int i = nb - 1; i >= 0; i--) { hm[nh] = back[i]; hv[nh++] = vb[i]; }
    hm[nh] = m0; hv[nh++] = v0;
    for (;;) {
        const uint32_t L = frame_len(hv[nh - 1]);
        int v = -1;
        const uint32_t k = L ? refine(&S, (int64_t)hm[nh - 1] + L, 4, 0.24f, v0, &v) : 0;
        if (!k || nh >= MAXHDR) break;
        hm[nh] = k; hv[nh++] = v;
    }
    if (nh < 3) { lg("solo %d PLHEADER: segnale non riconosciuto", nh); E->release(S.sym); return 0; }
    /* frequenza fine dai SOF: angolo medio della correlazione differenziale */
    cf acc = {0, 0};
    for (int i = 0; i < nh; i++) { cf s; hdr_metric(&S, hm[i], &s); acc.re += s.re; acc.im += s.im; }
    S.dphi = atan2f(acc.im, acc.re);
    {   /* affinamento: con il PLS noto i 90 simboli dell'header sono noti; correlazione a distanza 45 dopo la
         * correzione grossolana (la stima differenziale da sola sbagliava di ~400 Hz a MER 12, prova del 04/11) */
        cf q = {0, 0};
        for (int i = 0; i < nh; i++) {
            cf t[90];
            for (int n = 0; n < 90; n++) {
                const cf r = {cosf(-S.dphi * n), sinf(-S.dphi * n)};
                t[n] = cmulc(cmul(sym_at(&S, hm[i] + n), r), hdr_sym(hv[i], n));
            }
            for (int n = 45; n < 90; n++) { const cf c = cmulc(t[n], t[n - 45]); q.re += c.re; q.im += c.im; }
        }
        S.dphi += atan2f(q.im, q.re) / 45.0f;
    }
    rep->freq_hz = fc + S.dphi * sr / (2 * (float)M_PI);
    rep->detected = true;
    rep->plframes = nh;
    geom_t g0;
    geom(v0, &g0);
    rep->modcod = g0.modcod; rep->shortf = g0.shortf; rep->pilots = g0.pilots;
    snprintf(rep->fec, sizeof(rep->fec), "%s", g0.modcod ? RATES[g0.ri] : "?");
    rep->ms_sync = now_ms() - t0 - rep->ms_front;
    lg("%d PLHEADER, MODCOD %d = QPSK %s %s pilot %s, frequenza %+.1f Hz, SR %u", nh, g0.modcod, rep->fec,
       g0.shortf ? "corti" : "normali", g0.pilots ? "si" : "no", rep->freq_hz, (unsigned)rep->sr);
    /* frame */
    work_t W;
    W.y = E->alloc(sizeof(cf) * (g0.L + 96));
    W.s = E->alloc(sizeof(cf) * g0.ndata);
    W.ph = E->alloc(sizeof(float) * g0.ndata);
    W.loc = E->alloc(sizeof(float) * g0.ndata);
    W.bits = E->alloc(g0.tab ? g0.tab->kbch : 1);
    W.df = E->alloc(g0.tab ? g0.tab->kbch / 8 + 8 : 8);
    tsasm_t T;
    memset(&T, 0, sizeof(T));
    T.out = ts_out; T.ok = ts_ok; T.max = ts_out ? ts_max : 0;
    float mers[MAXHDR];
    int nm = 0;
    if (!W.y || !W.s || !W.ph || !W.loc || !W.bits || !W.df) lg("memoria per i frame insufficiente");
    for (int i = 0; i < nh && W.y && W.s && W.ph && W.loc && W.bits && W.df; i++) {
        geom_t g;
        if (!geom(hv[i], &g)) { ts_push(&T, NULL, 0, 0, false); continue; }
        fres_t fr;
        if (!demod_frame(&S, hm[i], hv[i], i + 1 < nh, i + 1 < nh ? hv[i + 1] : 0, &W, &fr)) {
            if (hm[i] + g.L <= S.nsym) lg("frame %d non demodulato (memoria per LDPC o scrambling?)", i);
            break;
        }
        rep->frames++;
        rep->ldpc_ok += fr.ok;
        rep->ldpc_iter_sum += fr.iter;
        mers[nm++] = fr.mer;
        /* BBHEADER: descrambling con la PRBS 1 + X^14 + X^15 */
        uint32_t reg = 0;                                            /* stadi 1..15 = 100101010000000 */
        const char *init = "100101010000000";
        for (int b = 0; b < 15; b++) reg |= (uint32_t)(init[b] - '0') << b;
        uint8_t hdr[10] = {0};
        const uint32_t kb = g.tab->kbch;
        memset(W.df, 0, kb / 8 + 8);
        uint32_t dfl = 0, syncd = 0;
        bool hok = false;
        for (uint32_t b = 0; b < kb; b++) {
            const uint32_t pr = ((reg >> 13) ^ (reg >> 14)) & 1u;
            reg = ((reg << 1) | pr) & 0x7FFFu;
            const uint8_t bit = W.bits[b] ^ (uint8_t)pr;
            if (b < 80) {
                hdr[b / 8] |= (uint8_t)(bit << (7 - (b & 7)));
                if (b == 79) {
                    hok = crc8(hdr, 9) == hdr[9];
                    dfl = (uint32_t)hdr[4] << 8 | hdr[5];
                    syncd = (uint32_t)hdr[7] << 8 | hdr[8];
                    if (!hok || dfl % 8 || dfl > kb - 80) break;
                }
            } else if (b - 80 < dfl) {
                W.df[(b - 80) / 8] |= (uint8_t)(bit << (7 - ((b - 80) & 7)));
            }
        }
        rep->bb_ok += hok;
        ts_push(&T, W.df, hok ? dfl / 8 : 0, syncd, hok && dfl % 8 == 0);
    }
    rep->ms_frames = now_ms() - t0 - rep->ms_front - rep->ms_sync;
    if (nm) {                                                        /* mediana del MER */
        qsort(mers, (size_t)nm, sizeof(float), cmp_float);
        rep->mer_db = mers[nm / 2];
        rep->d_margin_db = rep->mer_db - (g0.modcod ? THRESH[g0.ri] : 0);
    }
    rep->ts_packets = T.nout; rep->ts_crc_ok = T.crc_ok; rep->ts_crc_bad = T.crc_bad;
    if (ts_out) { parse_ts(ts_out, T.nout < ts_max ? T.nout : ts_max, rep); find_call(rep); }
    lg("MER %.1f dB, D %.1f, LDPC ok %d/%d (iterazioni medie %.1f), BBHEADER ok %d, TS %u pacchetti (CRC ok %u, errati %u)",
       rep->mer_db, rep->d_margin_db, rep->ldpc_ok, rep->frames, rep->frames ? (float)rep->ldpc_iter_sum / rep->frames : 0.0f,
       rep->bb_ok, (unsigned)T.nout, (unsigned)T.crc_ok, (unsigned)T.crc_bad);
    lg("servizio \"%s\" fornitore \"%s\" QRZ %s video 0x%02x audio 0x%02x", rep->service, rep->provider,
       rep->call[0] ? rep->call : "-", rep->vtype, rep->atype);
    E->release(S.sym);
    E->release(W.y); E->release(W.s); E->release(W.ph); E->release(W.loc); E->release(W.bits); E->release(W.df);
    release_all();
    rep->ms_total = now_ms() - t0;
    return T.nout;
}

bool s2rx_detect(const int8_t *iq, uint32_t n, float fs, const s2rx_env_t *env, float min_db, float *center,
                 float *width, float *peak_db)
{
    E = env;
    int64_t si = 0, sq = 0;
    for (uint32_t i = 0; i < n; i++) { si += iq[2 * i]; sq += iq[2 * i + 1]; }
    const float mi = (float)((double)si / n), mq = (float)((double)sq / n);
    *peak_db = 0;
    if (!detect(iq, n, fs, mi, mq, center, width, peak_db)) return false;
    return *peak_db >= min_db && *width >= 90e3f && *width <= 700e3f;    /* larghezze del DATV (SR 66..500 kS) */
}
