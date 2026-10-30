/* Beacon DATV 13 cm - IZ7PDX
 * Camera OV3660 (YUV 4:2:2, 320x240) + encoder H.264 software (esp_h264) per la diretta. Da chiamare dal core 1. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PDX_CAM_W 320
#define PDX_CAM_H 240

typedef void (*pdx_cam_overlay_fn)(uint8_t *yuyv, int w, int h, void *ctx);

/* camera ed encoder (una sola volta; le chiamate successive cambiano solo i parametri dell'encoder) */
const char *pdx_cam_live_init(int fps, int kbps);
/* acquisisce un fotogramma, applica la sovrimpressione, lo codifica in out; restituisce i byte (0 = errore) */
uint32_t pdx_cam_live_frame(uint8_t *out, uint32_t cap, pdx_cam_overlay_fn ovl, void *ctx, bool *keyframe,
                            uint32_t *t_cap_us, uint32_t *t_enc_us);
