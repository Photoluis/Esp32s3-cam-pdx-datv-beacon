/* Beacon DATV 13 cm - IZ7PDX
 * Prova di fattibilità della diretta: camera OV3660 in YUV422 + encoder H.264 software (esp_h264), sul core 1. */
#pragma once
#include <stdint.h>

/* w x h: 320x240, 320x192, 320x176 (ritagli di QVGA) oppure 640x480, 640x352 (ritagli di VGA); restituisce 0 se riuscita */
int pdx_cam_bench(int w, int h, int frames, int kbps, int fps);
