/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Sovrimpressione su immagini YUYV (vedi overlay.h). */
#include "overlay.h"

#include <string.h>
#include "font8x16.h"

void pdx_ovl_box(uint8_t *yuyv, int w, int h, int x, int y, int bw, int bh, uint8_t Y, uint8_t U, uint8_t V)
{
    if (x < 0) { bw += x; x = 0; }
    if (y < 0) { bh += y; y = 0; }
    x &= ~1;
    if (x + bw > w) bw = w - x;
    if (y + bh > h) bh = h - y;
    bw &= ~1;
    for (int r = y; r < y + bh; r++) {
        uint8_t *p = yuyv + ((size_t)r * w + x) * 2;
        for (int c = 0; c < bw; c += 2, p += 4) { p[0] = Y; p[1] = U; p[2] = Y; p[3] = V; }
    }
}

/* riquadro trasparente al 50% (06/11): media tra l'immagine e il colore */
void pdx_ovl_box50(uint8_t *yuyv, int w, int h, int x, int y, int bw, int bh, uint8_t Y, uint8_t U, uint8_t V)
{
    if (x < 0) { bw += x; x = 0; }
    if (y < 0) { bh += y; y = 0; }
    x &= ~1;
    if (x + bw > w) bw = w - x;
    if (y + bh > h) bh = h - y;
    bw &= ~1;
    for (int r = y; r < y + bh; r++) {
        uint8_t *p = yuyv + ((size_t)r * w + x) * 2;
        for (int c = 0; c < bw; c += 2, p += 4) {
            p[0] = (uint8_t)((p[0] + Y) >> 1); p[1] = (uint8_t)((p[1] + U) >> 1);
            p[2] = (uint8_t)((p[2] + Y) >> 1); p[3] = (uint8_t)((p[3] + V) >> 1);
        }
    }
}

int pdx_ovl_text(uint8_t *yuyv, int w, int h, int x, int y, const char *s, int scale)
{
    const int len = (int)strlen(s), cw = 8 * scale, ch = 16 * scale;
    pdx_ovl_box50(yuyv, w, h, x - 2, y - 1, len * cw + 4, ch + 2, 16, 128, 128);   /* fondo nero al 50% */
    for (int i = 0; i < len; i++) {
        const int c = (unsigned char)s[i];
        if (c < 32 || c > 126) continue;
        const uint8_t *g = font8x16[c - 32];
        for (int gy = 0; gy < 16; gy++) {
            for (int gx = 0; gx < 8; gx++) {
                if (!(g[gy] & (0x80 >> gx))) continue;
                for (int sy = 0; sy < scale; sy++) {
                    const int py = y + gy * scale + sy;
                    if (py < 0 || py >= h) continue;
                    for (int sx = 0; sx < scale; sx++) {
                        const int px = x + i * cw + gx * scale + sx;
                        if (px < 0 || px >= w) continue;
                        yuyv[((size_t)py * w + px) * 2] = 235;            /* luminanza bianca */
                    }
                }
            }
        }
    }
    return len * cw;
}
