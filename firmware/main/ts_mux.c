/* Beacon DATV 13 cm - IZ7PDX
 * Multiplexer MPEG-TS (vedi ts_mux.h). Il tempo del multiplexer è il numero di pacchetti emessi: a bitrate costante il
 * pacchetto n esce a n * 1504 / rate secondi, quindi PCR e PTS seguono il ritmo reale dei simboli trasmessi. */
#include "ts_mux.h"

#include <string.h>

#define PID_PAT  0x0000
#define PID_SDT  0x0011
#define PID_PMT  0x1000
#define PID_V    0x0100
#define PID_A    0x0101
#define PID_PCR  0x0102
#define LEAD_90K 90000u                  /* i PES partono fino a 1 s prima del loro tempo di decodifica */
#define DELAY_90K 99000u                 /* il primo PTS è 1,1 s dopo l'inizio del flusso */

static uint32_t crc32_mpeg(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= (uint32_t)p[i] << 24;
        for (int k = 0; k < 8; k++) crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : crc << 1;
    }
    return crc;
}

static uint64_t now_90k(const pdx_mux_t *m) { return (uint64_t)((double)m->n * 1504.0 * 90000.0 / m->rate); }
uint64_t pdx_mux_now_90k(const pdx_mux_t *m) { return now_90k(m); }
bool pdx_mux_vq_space(const pdx_mux_t *m) { return m->vq_w - m->vq_r < PDX_VQ_LEN; }
pdx_vq_entry_t *pdx_mux_vq_slot(pdx_mux_t *m) { return &m->vq[m->vq_w % PDX_VQ_LEN]; }
void pdx_mux_vq_commit(pdx_mux_t *m) { m->vq_w++; }

static void ts_header(uint8_t *p, uint16_t pid, bool pusi, uint8_t afc, uint8_t *cc)
{
    p[0] = 0x47;
    p[1] = (uint8_t)((pusi ? 0x40 : 0) | (pid >> 8));
    p[2] = (uint8_t)pid;
    p[3] = (uint8_t)((afc << 4) | (*cc & 15));
    if (afc & 1) *cc = (uint8_t)((*cc + 1) & 15);       /* il contatore avanza solo con il payload */
}

/* sezione PSI in un solo pacchetto: puntatore 0, sezione, CRC, riempimento 0xFF */
static void psi_packet(uint8_t *p, uint16_t pid, uint8_t *cc, const uint8_t *sec, uint32_t len)
{
    ts_header(p, pid, true, 1, cc);
    p[4] = 0;
    memcpy(p + 5, sec, len);
    const uint32_t crc = crc32_mpeg(sec, len);
    p[5 + len] = (uint8_t)(crc >> 24);
    p[6 + len] = (uint8_t)(crc >> 16);
    p[7 + len] = (uint8_t)(crc >> 8);
    p[8 + len] = (uint8_t)crc;
    memset(p + 9 + len, 0xFF, 188 - 9 - len);
}

static void make_pat(pdx_mux_t *m, uint8_t *p)
{
    const uint8_t s[] = {0x00, 0xB0, 13, 0x00, 0x01, 0xC1, 0x00, 0x00,          /* tabella 0, lunghezza 13, TSID 1, v0 */
                         0x00, 0x01, 0xE0 | (PID_PMT >> 8), PID_PMT & 0xFF};     /* programma 1 -> PMT */
    psi_packet(p, PID_PAT, &m->cc[0], s, sizeof(s));
}

static void make_pmt(pdx_mux_t *m, uint8_t *p)
{
    const uint8_t s[] = {0x02, 0xB0, 23, 0x00, 0x01, 0xC1, 0x00, 0x00,
                         0xE0 | (PID_PCR >> 8), PID_PCR & 0xFF, 0xF0, 0x00,      /* PCR_PID, nessun descrittore */
                         m->vtype, 0xE0 | (PID_V >> 8), PID_V & 0xFF, 0xF0, 0x00,
                         (uint8_t)m->es->atype, 0xE0 | (PID_A >> 8), PID_A & 0xFF, 0xF0, 0x00};
    psi_packet(p, PID_PMT, &m->cc[1], s, sizeof(s));
}

static void make_sdt(pdx_mux_t *m, uint8_t *p)
{
    uint8_t s[120];
    const uint8_t lp = (uint8_t)strlen(m->provider), ls = (uint8_t)strlen(m->service);
    const uint8_t dlen = (uint8_t)(3 + lp + ls);           /* descrittore di servizio 0x48 */
    uint32_t k = 0;
    s[k++] = 0x42; s[k++] = 0; s[k++] = 0;                  /* lunghezza dopo */
    s[k++] = 0x00; s[k++] = 0x01; s[k++] = 0xC1; s[k++] = 0; s[k++] = 0;
    s[k++] = 0x00; s[k++] = 0x01; s[k++] = 0xFF;            /* original_network_id 1 */
    s[k++] = 0x00; s[k++] = 0x01; s[k++] = 0xFC;            /* servizio 1, niente EIT */
    s[k++] = (uint8_t)(0x80 | ((dlen + 2) >> 8)); s[k++] = (uint8_t)(dlen + 2);   /* running, non cifrato */
    s[k++] = 0x48; s[k++] = dlen; s[k++] = m->vtype == 0x24 ? 0x1F : 0x16;   /* TV digitale HEVC / AVC */
    s[k++] = lp; memcpy(s + k, m->provider, lp); k += lp;
    s[k++] = ls; memcpy(s + k, m->service, ls); k += ls;
    const uint32_t sl = k - 3 + 4;
    s[1] = (uint8_t)(0xF0 | (sl >> 8));
    s[2] = (uint8_t)sl;
    psi_packet(p, PID_SDT, &m->cc[2], s, k);
}

static void make_pcr(pdx_mux_t *m, uint8_t *p)
{
    uint8_t dummy = 0;
    ts_header(p, PID_PCR, false, 2, &dummy);              /* solo adaptation field */
    const uint64_t pcr27 = (uint64_t)((double)m->n * 1504.0 * 27e6 / m->rate);
    const uint64_t base = (pcr27 / 300u) & 0x1FFFFFFFFull, ext = pcr27 % 300u;
    p[4] = 183;
    p[5] = 0x10;
    p[6] = (uint8_t)(base >> 25);
    p[7] = (uint8_t)(base >> 17);
    p[8] = (uint8_t)(base >> 9);
    p[9] = (uint8_t)(base >> 1);
    p[10] = (uint8_t)(((base & 1u) << 7) | 0x7E | (ext >> 8));
    p[11] = (uint8_t)ext;
    memset(p + 12, 0xFF, 188 - 12);
}

/* cursore di un flusso: unità successiva del proprio tipo, con il giro del loop */
static const pdx_es_unit_t *peek(pdx_es_cursor_t *c, uint8_t kind)
{
    for (int pass = 0; pass < 2; pass++) {
        while (c->next < c->hdr->nunits && c->units[c->next].kind != kind) c->next++;
        if (c->next < c->hdr->nunits) return &c->units[c->next];
        c->next = 0;                                       /* fine del file: nuovo giro */
        c->loop++;
    }
    return NULL;
}

static uint64_t unit_pts(const pdx_mux_t *m, const pdx_es_cursor_t *c, const pdx_es_unit_t *u)
{
    return m->pts_base + (uint64_t)c->loop * m->es->loop_ticks + u->pts;
}

static void start_pes_raw(pdx_mux_t *m, pdx_es_cursor_t *c, uint64_t pts, const uint8_t *data, uint32_t len, bool video)
{
    pts &= 0x1FFFFFFFFull;
    uint8_t *h = c->ph;
    h[0] = 0; h[1] = 0; h[2] = 1;
    h[3] = video ? 0xE0 : 0xC0;
    const uint32_t plen = video ? 0 : len + 8;            /* video: lunghezza 0 (non limitata) */
    h[4] = (uint8_t)(plen >> 8);
    h[5] = (uint8_t)plen;
    h[6] = video ? 0x84 : 0x80;                            /* '10', data_alignment per il video */
    h[7] = 0x80;                                           /* solo PTS */
    h[8] = 5;
    h[9] = (uint8_t)(0x21 | ((pts >> 29) & 0x0E));
    h[10] = (uint8_t)(pts >> 22);
    h[11] = (uint8_t)(0x01 | ((pts >> 14) & 0xFE));
    h[12] = (uint8_t)(pts >> 7);
    h[13] = (uint8_t)(0x01 | ((pts << 1) & 0xFE));
    c->ph_len = 14;
    c->ph_pos = 0;
    c->pl = data;
    c->pl_rem = len;
    c->active = true;
    if (pts < (now_90k(m) & 0x1FFFFFFFFull)) m->late++;
}

static void start_pes(pdx_mux_t *m, pdx_es_cursor_t *c, const pdx_es_unit_t *u, bool video)
{
    start_pes_raw(m, c, unit_pts(m, c, u), c->data + m->es->data_off + u->off, u->len, video);
    c->next++;
}

static void pes_packet(pdx_mux_t *m, pdx_es_cursor_t *c, uint16_t pid, uint8_t *cc, uint8_t *p)
{
    const bool pusi = c->ph_pos == 0;
    const uint32_t rem = (c->ph_len - c->ph_pos) + c->pl_rem;
    uint32_t k = 4;
    if (rem >= 184) {
        ts_header(p, pid, pusi, 1, cc);
    } else {                                               /* ultimo pacchetto: riempimento nell'adaptation field */
        ts_header(p, pid, pusi, 3, cc);
        const uint32_t af = 183 - rem;
        p[4] = (uint8_t)af;
        if (af > 0) { p[5] = 0x00; memset(p + 6, 0xFF, af - 1); }
        k = 5 + af;
    }
    while (k < 188 && c->ph_pos < c->ph_len) p[k++] = c->ph[c->ph_pos++];
    const uint32_t n = 188 - k;
    memcpy(p + k, c->pl, n);
    c->pl += n;
    c->pl_rem -= n;
    if (c->pl_rem == 0) c->active = false;
}

static bool try_stream(pdx_mux_t *m, pdx_es_cursor_t *c, uint8_t kind, uint16_t pid, uint8_t *cc, uint8_t *p)
{
    if (!c->active) {
        const pdx_es_unit_t *u = peek(c, kind);
        if (!u || unit_pts(m, c, u) > now_90k(m) + LEAD_90K) return false;
        start_pes(m, c, u, kind == 0);
    }
    pes_packet(m, c, pid, cc, p);
    return true;
}

/* video dalla coda: un fotogramma alla volta, inviato quando il suo PTS è entro LEAD_90K */
static bool try_queue(pdx_mux_t *m, uint8_t *p)
{
    pdx_es_cursor_t *c = &m->v;
    if (!c->active) {
        if (m->vq_r == m->vq_w) return false;
        const pdx_vq_entry_t *e = &m->vq[m->vq_r % PDX_VQ_LEN];
        if (e->pts > now_90k(m) + LEAD_90K) return false;
        start_pes_raw(m, c, e->pts, e->buf, e->len, true);
        m->vq_sending = true;
    }
    pes_packet(m, c, PID_V, &m->cc[3], p);
    if (!c->active && m->vq_sending) {                     /* PES finito: il posto torna libero */
        m->vq_sending = false;
        m->vq_r++;
    }
    return true;
}

void pdx_mux_next(pdx_mux_t *m, uint8_t pkt[188])
{
    const double pkt_per_s = m->rate / 1504.0;
    if (m->psi_pending) {
        make_pmt(m, pkt);
        m->psi_pending = 0;
    } else if (m->n >= m->next_psi) {                     /* PAT + PMT ogni 100 ms */
        make_pat(m, pkt);
        m->psi_pending = 1;
        m->next_psi = m->n + (uint64_t)(0.1 * pkt_per_s);
    } else if (m->n >= m->next_sdt) {                     /* SDT ogni 1 s */
        make_sdt(m, pkt);
        m->next_sdt = m->n + (uint64_t)pkt_per_s;
    } else if (m->n >= m->next_pcr) {                     /* PCR ogni 40 ms */
        make_pcr(m, pkt);
        m->next_pcr = m->n + (uint64_t)(0.04 * pkt_per_s);
    } else if (try_stream(m, &m->a, 1, PID_A, &m->cc[4], pkt)) {
        m->stats_audio++;
    } else if (!m->vqueue && try_stream(m, &m->v, 0, PID_V, &m->cc[3], pkt)) {
        m->stats_video++;
    } else if (m->vqueue && try_queue(m, pkt)) {
        m->stats_video++;
    } else {                                               /* pacchetto nullo */
        pkt[0] = 0x47; pkt[1] = 0x1F; pkt[2] = 0xFF; pkt[3] = 0x10;
        memset(pkt + 4, 0xFF, 184);
        m->stats_null++;
    }
    m->n++;
}

const char *pdx_mux_init(pdx_mux_t *m, const uint8_t *es_file, double rate, const char *service, const char *provider,
                         bool vqueue)
{
    memset(m, 0, sizeof(*m));
    const pdx_es_header_t *h = (const pdx_es_header_t *)es_file;
    if (memcmp(h->magic, "PDXES001", 8)) return "nessun file ES";
    if (!h->nunits || !h->loop_ticks) return "file ES vuoto";
    m->es = h;
    m->rate = rate;
    m->vqueue = vqueue;
    m->vtype = vqueue ? 0x1B : (uint8_t)h->vtype;
    m->pts_base = DELAY_90K;
    strncpy(m->service, service, sizeof(m->service) - 1);
    strncpy(m->provider, provider, sizeof(m->provider) - 1);
    const pdx_es_unit_t *units = (const pdx_es_unit_t *)(es_file + sizeof(*h));
    m->v = (pdx_es_cursor_t){.data = es_file, .hdr = h, .units = units};
    m->a = m->v;
    return NULL;
}
