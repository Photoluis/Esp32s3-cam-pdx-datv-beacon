/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Video dal vivo per il TS (vedi cam_live.h). La camera viene inizializzata una sola volta: spegnerla e riaccenderla
 * blocca il driver (prova del 29/10). Pin: schema Freenove / diymore ESP32-S3-CAM; D0 (GPIO11) della scheda di prova è
 * interrotto, in YUV si perde solo il bit meno significativo. */
#include "cam_live.h"

#include <string.h>
#include "esp_camera.h"
#include "esp_h264_enc_single_sw.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define FRAME_BYTES (PDX_CAM_W * PDX_CAM_H * 2)
/* riquadro della camera nel PiP: metà risoluzione, in basso a destra sopra la fascia del contatore */
#define PIP_X 154
#define PIP_Y 88

static bool cam_ok;
static esp_h264_enc_handle_t enc;
static uint8_t *work;                     /* immagine composta (PiP, immagine fissa) */

const char *pdx_cam_live_init(int fps, int kbps, bool camera)
{
    if (camera && !cam_ok) {
        camera_config_t c = {
            .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 15, .pin_sccb_sda = 4, .pin_sccb_scl = 5,
            .pin_d7 = 16, .pin_d6 = 17, .pin_d5 = 18, .pin_d4 = 12, .pin_d3 = 10, .pin_d2 = 8, .pin_d1 = 9, .pin_d0 = 11,
            .pin_vsync = 6, .pin_href = 7, .pin_pclk = 13,
            .xclk_freq_hz = 20000000, .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
            .pixel_format = PIXFORMAT_YUV422, .frame_size = FRAMESIZE_QVGA, .jpeg_quality = 12, .fb_count = 2,
            .fb_location = CAMERA_FB_IN_PSRAM, .grab_mode = CAMERA_GRAB_LATEST,
        };
        if (esp_camera_init(&c) != ESP_OK) return "camera";
        sensor_t *s = esp_camera_sensor_get();
        if (s && s->set_hmirror) s->set_hmirror(s, 1);    /* la OV3660 di questa scheda dà l'immagine specchiata */
        /* i primi fotogrammi dopo l'accensione arrivano lenti: scartati qui, prima che parta la TX (con la TX già in
         * corso svuotavano il buffer dei simboli, 30/10: 4395 volte nel primo segmento PiP) */
        for (int i = 0; i < 4; i++) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) esp_camera_fb_return(fb);
        }
        cam_ok = true;
    }
    if (!work && !(work = heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM))) return "memoria";
    /* encoder nuovo a ogni segmento: il primo fotogramma è un IDR con SPS/PPS, così il ricevitore decodifica subito
     * (con l'encoder riutilizzato il segmento partiva a metà GOP) */
    if (enc) {
        esp_h264_enc_close(enc);
        esp_h264_enc_del(enc);
        enc = NULL;
    }
    esp_h264_enc_cfg_sw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_YUYV, .gop = (uint8_t)(fps * 2), .fps = (uint8_t)fps,
        .res = {.width = PDX_CAM_W, .height = PDX_CAM_H},
        .rc = {.bitrate = (uint32_t)kbps * 1000u, .qp_min = 26, .qp_max = 44},
    };
    if (esp_h264_enc_sw_new(&cfg, &enc) != ESP_H264_ERR_OK || esp_h264_enc_open(enc) != ESP_H264_ERR_OK) {
        enc = NULL;
        return "encoder H.264";
    }
    return NULL;
}

/* camera ridotta a metà (media orizzontale, una riga su due) nel riquadro del PiP, con cornice bianca */
static void pip_inset(uint8_t *dst, const uint8_t *cam)
{
    for (int y = PIP_Y - 2; y < PIP_Y + PDX_CAM_H / 2 + 2; y++) {
        uint8_t *p = dst + ((size_t)y * PDX_CAM_W + PIP_X - 2) * 2;
        for (int x = 0; x < PDX_CAM_W / 2 + 4; x += 2, p += 4) { p[0] = 235; p[1] = 128; p[2] = 235; p[3] = 128; }
    }
    for (int oy = 0; oy < PDX_CAM_H / 2; oy++) {
        const uint8_t *s = cam + (size_t)(2 * oy) * PDX_CAM_W * 2;
        uint8_t *d = dst + ((size_t)(PIP_Y + oy) * PDX_CAM_W + PIP_X) * 2;
        for (int j = 0; j < PDX_CAM_W / 4; j++, s += 8, d += 4) {
            d[0] = (uint8_t)((s[0] + s[2] + 1) >> 1);
            d[1] = (uint8_t)((s[1] + s[5] + 1) >> 1);
            d[2] = (uint8_t)((s[4] + s[6] + 1) >> 1);
            d[3] = (uint8_t)((s[3] + s[7] + 1) >> 1);
        }
    }
}

uint32_t pdx_cam_live_frame(pdx_live_src_t src, const uint8_t *bg, uint8_t *out, uint32_t cap, pdx_cam_overlay_fn ovl,
                            void *ctx, uint32_t *t_cap_us, uint32_t *t_enc_us)
{
    if (!enc || !work || (src != PDX_SRC_IMG && !cam_ok) || (src != PDX_SRC_CAM && !bg)) return 0;
    const int64_t t0 = esp_timer_get_time();
    camera_fb_t *fb = NULL;
    if (src != PDX_SRC_IMG && !(fb = esp_camera_fb_get())) return 0;
    uint8_t *img;
    if (src == PDX_SRC_CAM) {
        img = fb->buf;
    } else {
        memcpy(work, bg, FRAME_BYTES);
        if (src == PDX_SRC_PIP) pip_inset(work, fb->buf);
        img = work;
    }
    if (fb && src != PDX_SRC_CAM) {                        /* il fotogramma della camera è già copiato */
        esp_camera_fb_return(fb);
        fb = NULL;
    }
    const int64_t t1 = esp_timer_get_time();
    if (ovl) ovl(img, PDX_CAM_W, PDX_CAM_H, ctx);
    esp_h264_enc_in_frame_t in = {.raw_data = {.buffer = img, .len = FRAME_BYTES}};
    esp_h264_enc_out_frame_t o = {.raw_data = {.buffer = out, .len = cap}};
    const esp_h264_err_t r = esp_h264_enc_process(enc, &in, &o);
    if (fb) esp_camera_fb_return(fb);
    const int64_t t2 = esp_timer_get_time();
    if (t_cap_us) *t_cap_us = (uint32_t)(t1 - t0);
    if (t_enc_us) *t_enc_us = (uint32_t)(t2 - t1);
    return r == ESP_H264_ERR_OK ? o.length : 0;
}
