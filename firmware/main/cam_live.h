/* Beacon DATV 13 cm - IZ7PDX
 * Video H.264 320x240 generato dal vivo sul core 1: camera OV3660 a pieno schermo, camera in PiP su un'immagine di sfondo,
 * oppure un'immagine fissa (monoscopio personalizzato statico); encoder software esp_h264. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PDX_CAM_W 320
#define PDX_CAM_H 240

typedef enum { PDX_SRC_CAM, PDX_SRC_PIP, PDX_SRC_IMG } pdx_live_src_t;

typedef void (*pdx_cam_overlay_fn)(uint8_t *yuyv, int w, int h, void *ctx);

/* encoder (e camera se serve; la camera si inizializza una sola volta). Da chiamare dal core 1. */
const char *pdx_cam_live_init(int fps, int kbps, bool camera);
/* compone un fotogramma (bg = immagine 320x240 YUYV per PiP e IMG), applica la sovrimpressione e lo codifica in out;
 * restituisce i byte (0 = errore) */
uint32_t pdx_cam_live_frame(pdx_live_src_t src, const uint8_t *bg, uint8_t *out, uint32_t cap, pdx_cam_overlay_fn ovl,
                            void *ctx, uint32_t *t_cap_us, uint32_t *t_enc_us);
