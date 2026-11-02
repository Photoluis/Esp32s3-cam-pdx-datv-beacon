/* ESP32-S3_PDX Parrot - IZ7PDX - GPL-3.0
 * Prova sul PC del ricevitore C (firmware/main/s2rx.c) su una cattura .cs8 (int8 I/Q a 1 Msps).
 * Uso: s2rx_pc cattura.cs8 [fs] [sr] [uscita.ts] */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "s2rx.h"

static void *a(size_t n) { return malloc(n); }
static void r(void *p) { free(p); }
static void l(const char *s) { printf("%s\n", s); }
static uint32_t ms(void) { return (uint32_t)(clock() * 1000.0 / CLOCKS_PER_SEC); }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "uso: s2rx_pc cattura.cs8 [fs] [sr] [uscita.ts]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    int8_t *iq = malloc((size_t)sz);
    if (fread(iq, 1, (size_t)sz, f) != (size_t)sz) return 1;
    fclose(f);
    const float fs = argc > 2 ? (float)atof(argv[2]) : 1e6f;
    const uint32_t sr = argc > 3 ? (uint32_t)atoi(argv[3]) : 0;
    const uint32_t tsmax = 4000;
    uint8_t *ts = malloc(188 * tsmax), *ok = malloc(tsmax);
    s2rx_env_t env = {a, r, l, ms};
    s2rx_report_t rep;
    const uint32_t np = s2rx_run(iq, (uint32_t)(sz / 2), fs, sr, &env, &rep, ts, tsmax, ok);
    printf("RISULTATO rilevato=%d SR=%u freq=%+.1f MER=%.1f D=%.1f frame=%d LDPC=%d BB=%d TS=%u errati=%u QRZ=%s tempo %u ms "
           "(front-end %u, aggancio %u, frame %u)\n", rep.detected, (unsigned)rep.sr, rep.freq_hz, rep.mer_db, rep.d_margin_db,
           rep.frames, rep.ldpc_ok, rep.bb_ok, (unsigned)rep.ts_packets, (unsigned)rep.ts_crc_bad, rep.call[0] ? rep.call : "-",
           (unsigned)rep.ms_total, (unsigned)rep.ms_front, (unsigned)rep.ms_sync, (unsigned)rep.ms_frames);
    if (argc > 4 && np) { FILE *o = fopen(argv[4], "wb"); fwrite(ts, 188, np < tsmax ? np : tsmax, o); fclose(o); }
    return 0;
}
