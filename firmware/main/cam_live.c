/* Beacon DATV 13 cm - IZ7PDX
 * Camera + encoder H.264 per la diretta (vedi cam_live.h). La camera viene inizializzata una sola volta: spegnerla e
 * riaccenderla blocca il driver (prova del 29/10). Pin: schema Freenove / diymore ESP32-S3-CAM; D0 (GPIO11) della scheda
 * di prova è interrotto, in YUV si perde solo il bit meno significativo. */
#include "cam_live.h"

#include <string.h>
#include "esp_camera.h"
#include "esp_h264_enc_single_sw.h"
#include "esp_timer.h"

static bool cam_ok;
static esp_h264_enc_handle_t enc;
static int enc_fps, enc_kbps;

const char *pdx_cam_live_init(int fps, int kbps)
{
    if (!cam_ok) {
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
        cam_ok = true;
    }
    if (enc && (fps != enc_fps || kbps != enc_kbps)) {
        esp_h264_enc_close(enc);
        esp_h264_enc_del(enc);
        enc = NULL;
    }
    if (!enc) {
        esp_h264_enc_cfg_sw_t cfg = {
            .pic_type = ESP_H264_RAW_FMT_YUYV, .gop = (uint8_t)(fps * 2), .fps = (uint8_t)fps,
            .res = {.width = PDX_CAM_W, .height = PDX_CAM_H},
            .rc = {.bitrate = (uint32_t)kbps * 1000u, .qp_min = 22, .qp_max = 44},
        };
        if (esp_h264_enc_sw_new(&cfg, &enc) != ESP_H264_ERR_OK || esp_h264_enc_open(enc) != ESP_H264_ERR_OK) {
            enc = NULL;
            return "encoder H.264";
        }
        enc_fps = fps;
        enc_kbps = kbps;
    }
    return NULL;
}

uint32_t pdx_cam_live_frame(uint8_t *out, uint32_t cap, pdx_cam_overlay_fn ovl, void *ctx, bool *keyframe,
                            uint32_t *t_cap_us, uint32_t *t_enc_us)
{
    if (!cam_ok || !enc) return 0;
    const int64_t t0 = esp_timer_get_time();
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return 0;
    const int64_t t1 = esp_timer_get_time();
    if (ovl) ovl(fb->buf, PDX_CAM_W, PDX_CAM_H, ctx);
    esp_h264_enc_in_frame_t in = {.raw_data = {.buffer = fb->buf, .len = PDX_CAM_W * PDX_CAM_H * 2}};
    esp_h264_enc_out_frame_t o = {.raw_data = {.buffer = out, .len = cap}};
    const esp_h264_err_t r = esp_h264_enc_process(enc, &in, &o);
    esp_camera_fb_return(fb);
    const int64_t t2 = esp_timer_get_time();
    if (t_cap_us) *t_cap_us = (uint32_t)(t1 - t0);
    if (t_enc_us) *t_enc_us = (uint32_t)(t2 - t1);
    if (r != ESP_H264_ERR_OK) return 0;
    if (keyframe) *keyframe = o.frame_type == ESP_H264_FRAME_TYPE_IDR || o.frame_type == ESP_H264_FRAME_TYPE_I;
    return o.length;
}
