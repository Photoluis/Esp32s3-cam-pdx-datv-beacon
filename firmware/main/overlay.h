/* Beacon DATV 13 cm - IZ7PDX
 * Sovrimpressione su immagini YUYV (YUV 4:2:2, Y0 U Y1 V): riquadri e testo con il font 8x16. */
#pragma once
#include <stdint.h>

/* riquadro pieno (x arrotondato a coppie di pixel per la crominanza) */
void pdx_ovl_box(uint8_t *yuyv, int w, int h, int x, int y, int bw, int bh, uint8_t Y, uint8_t U, uint8_t V);
/* testo bianco su fondo scuro, ingrandito scale volte; restituisce la larghezza in pixel */
int pdx_ovl_text(uint8_t *yuyv, int w, int h, int x, int y, const char *s, int scale);
