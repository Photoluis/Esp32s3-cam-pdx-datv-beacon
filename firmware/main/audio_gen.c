/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Suono generato per il TS (vedi audio_gen.h). La telegrafia è fatta di blocchi codificati in mezzo al silenzio
 * (punto + pausa, linea + pausa, silenzio): giuntati, l'AAC decodifica come se il segnale fosse stato codificato d'un
 * pezzo (verifica in make_audio_frames.py). Il tono: l'inizio dal silenzio, poi sempre lo stesso frame. */
#include "audio_gen.h"

#include <stdio.h>
#include <string.h>
#include "audio_frames.h"

enum { BLK_SIL, BLK_DOT, BLK_DASH };
#define SEQ(b, i) ((uint8_t)((b) << 4 | (i)))

static const char *const NAMES[PDX_AUDIO_N] = {"off", "tone", "cw", "file"};
const char *pdx_audio_name(uint8_t mode) { return mode < PDX_AUDIO_N ? NAMES[mode] : "?"; }

/* Morse come il CW ID di ESP32-ATV (AudioSources.kt) */
static const char *morse(char ch)
{
    static const char *const AZ[26] = {".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-",
                                       ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-",
                                       ".--", "-..-", "-.--", "--.."};
    static const char *const D09[10] = {"-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...",
                                        "---..", "----."};
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch >= 'A' && ch <= 'Z') return AZ[ch - 'A'];
    if (ch >= '0' && ch <= '9') return D09[ch - '0'];
    return ch == '/' ? "-..-." : NULL;
}

static uint32_t block_len(int b) { return b == BLK_DOT ? sizeof(pdx_af_dot_len) / 2u : b == BLK_DASH ? sizeof(pdx_af_dash_len) / 2u : 1u; }

/* frame della telegrafia di text a partire da pos (fino a max); restituisce la posizione finale, oltre max se non ci sta */
static uint32_t put_text(uint8_t *seq, uint32_t pos, uint32_t max, const char *text)
{
    for (const char *p = text; *p; p++) {
        if (*p == ' ') {                                   /* 7 unità tra le parole (3 già dopo la lettera) */
            for (uint32_t i = 0; i < 4u * PDX_AF_UNIT; i++, pos++) if (pos < max) seq[pos] = SEQ(BLK_SIL, 0);
            continue;
        }
        const char *m = morse(*p);
        if (!m) continue;
        for (; *m; m++) {                                  /* ogni blocco porta già 1 unità di pausa */
            const int b = *m == '.' ? BLK_DOT : BLK_DASH;
            for (uint32_t i = 0; i < block_len(b); i++, pos++) if (pos < max) seq[pos] = SEQ(b, i);
        }
        for (uint32_t i = 0; i < 2u * PDX_AF_UNIT; i++, pos++) if (pos < max) seq[pos] = SEQ(BLK_SIL, 0);   /* 3 unità */
    }
    return pos;
}

void pdx_agen_init(pdx_agen_t *g, uint8_t mode, const char *call, const char *locator)
{
    memset(g, 0, sizeof(*g));
    g->mode = mode;
    if (mode != PDX_AUDIO_CW) return;
    /* con il locator solo se resta silenzioso un terzo del giro (come ESP32-ATV), altrimenti il solo QRZ */
    snprintf(g->text, sizeof(g->text), "%s%s%s", call, locator && locator[0] ? " " : "", locator ? locator : "");
    uint32_t end = put_text(g->seq, PDX_AGEN_FIRST, PDX_AGEN_PERIOD, g->text);
    if (end > PDX_AGEN_PERIOD * 2u / 3u && locator && locator[0]) {
        memset(g->seq, 0, sizeof(g->seq));
        snprintf(g->text, sizeof(g->text), "%s", call);
        end = put_text(g->seq, PDX_AGEN_FIRST, PDX_AGEN_PERIOD, g->text);
    }
    g->cw_frames = (end > PDX_AGEN_PERIOD ? PDX_AGEN_PERIOD : end) - PDX_AGEN_FIRST;   /* troppo lungo: troncato */
}

const uint8_t *pdx_agen_frame(const pdx_agen_t *g, uint32_t k, uint32_t *len)
{
    const uint8_t *d;
    const uint16_t *l;
    uint32_t i;
    if (g->mode == PDX_AUDIO_TONE) {
        const uint32_t n0 = sizeof(pdx_af_tone0_len) / 2u;
        if (k >= n0) { *len = pdx_af_tone_len[0]; return pdx_af_tone; }
        d = pdx_af_tone0; l = pdx_af_tone0_len; i = k;
    } else {
        const uint8_t s = g->seq[k % PDX_AGEN_PERIOD];
        i = s & 15u;
        switch (s >> 4) {
        case BLK_DOT: d = pdx_af_dot; l = pdx_af_dot_len; break;
        case BLK_DASH: d = pdx_af_dash; l = pdx_af_dash_len; break;
        default: *len = pdx_af_sil_len[0]; return pdx_af_sil;
        }
    }
    for (uint32_t j = 0; j < i; j++) d += l[j];
    *len = l[i];
    return d;
}

double pdx_agen_bps(const pdx_agen_t *g, uint32_t fpp)
{
    double bytes = 0;
    const uint32_t n = g->mode == PDX_AUDIO_TONE ? 1u : PDX_AGEN_PERIOD;
    uint32_t len;
    for (uint32_t k = 0; k < n; k++) {
        pdx_agen_frame(g, g->mode == PDX_AUDIO_TONE ? 1000u : k, &len);
        bytes += len;
    }
    /* per PES: intestazione 14 byte e in media mezzo pacchetto di riempimento; 4 byte di intestazione TS ogni 184 */
    const double per_pes = (bytes / n * fpp + 14.0 + 92.0) * 188.0 / 184.0;
    return per_pes * 8.0 * PDX_AGEN_HZ / ((double)fpp * PDX_AGEN_FRAME);
}
