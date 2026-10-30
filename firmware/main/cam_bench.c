/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Prova di fattibilità della diretta (nessuna RF): cattura YUV422 dalla OV3660 e codifica H.264 software, su un task del
 * core 1 (nel beacon il core 0 è occupato dal modulatore). Pin della camera: schema Freenove / diymore ESP32-S3-CAM.
 * D0 (GPIO11) sulla scheda di prova è interrotto: in YUV si perde solo il bit meno significativo. */
#include "cam_bench.h"

#include <stdio.h>
#include <string.h>
#include "esp_camera.h"
#include "esp_h264_enc_single_sw.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    int w, h, frames, kbps, fps, result;
    SemaphoreHandle_t done;
} bench_job_t;

static esp_err_t cam_start(framesize_t size)
{
    camera_config_t c = {
        .pin_pwdn = -1, .pin_reset = -1, .pin_xclk = 15, .pin_sccb_sda = 4, .pin_sccb_scl = 5,
        .pin_d7 = 16, .pin_d6 = 17, .pin_d5 = 18, .pin_d4 = 12, .pin_d3 = 10, .pin_d2 = 8, .pin_d1 = 9, .pin_d0 = 11,
        .pin_vsync = 6, .pin_href = 7, .pin_pclk = 13,
        .xclk_freq_hz = 20000000, .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_YUV422, .frame_size = size, .jpeg_quality = 12, .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM, .grab_mode = CAMERA_GRAB_LATEST,
    };
    return esp_camera_init(&c);
}

static void bench_task(void *arg)
{
    bench_job_t *j = arg;
    j->result = -1;
    const framesize_t size = j->w <= 320 ? FRAMESIZE_QVGA : FRAMESIZE_VGA;
    const int src_w = j->w <= 320 ? 320 : 640, src_h = j->w <= 320 ? 240 : 480;
    if (j->w != src_w || j->h > src_h || (j->h & 15)) {
        printf("ERR risoluzione: larghezza 320 o 640, altezza multipla di 16 fino a %d\n", src_h);
        goto out;
    }
    esp_err_t e = cam_start(size);
    if (e != ESP_OK) { printf("ERR camera 0x%x\n", e); goto out; }
    sensor_t *s = esp_camera_sensor_get();
    printf("camera PID 0x%04X, sorgente %dx%d YUV422, codifica %dx%d (ritaglio centrale)\n", s ? s->id.PID : 0, src_w, src_h,
           j->w, j->h);

    esp_h264_enc_cfg_sw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_YUYV, .gop = (uint8_t)(j->fps * 2), .fps = (uint8_t)j->fps,
        .res = {.width = (uint16_t)j->w, .height = (uint16_t)j->h},
        .rc = {.bitrate = (uint32_t)j->kbps * 1000u, .qp_min = 20, .qp_max = 45},
    };
    esp_h264_enc_handle_t enc = NULL;
    if (esp_h264_enc_sw_new(&cfg, &enc) != ESP_H264_ERR_OK || esp_h264_enc_open(enc) != ESP_H264_ERR_OK) {
        printf("ERR encoder H.264\n");
        esp_camera_deinit();
        goto out;
    }
    const uint32_t out_cap = (uint32_t)(j->w * j->h);
    uint8_t *out_buf = heap_caps_aligned_alloc(16, out_cap, MALLOC_CAP_SPIRAM);
    if (!out_buf) { printf("ERR memoria\n"); goto close; }

    for (int k = 0; k < 5; k++) { camera_fb_t *fb = esp_camera_fb_get(); if (fb) esp_camera_fb_return(fb); }  /* esposizione */
    int64_t t_cap = 0, t_enc = 0, t_max = 0;
    uint64_t bytes = 0;
    int nidr = 0, ok = 0;
    const int64_t t0 = esp_timer_get_time();
    for (int f = 0; f < j->frames; f++) {
        const int64_t a = esp_timer_get_time();
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { printf("ERR fotogramma %d mancante\n", f); break; }
        const int64_t b = esp_timer_get_time();
        const int y0 = (src_h - j->h) / 2;
        esp_h264_enc_in_frame_t in = {.raw_data = {.buffer = fb->buf + (size_t)y0 * src_w * 2,
                                                   .len = (uint32_t)(j->w * j->h * 2)},
                                      .pts = (uint32_t)(f * 90000 / j->fps)};
        esp_h264_enc_out_frame_t out = {.raw_data = {.buffer = out_buf, .len = out_cap}};
        const esp_h264_err_t r = esp_h264_enc_process(enc, &in, &out);
        const int64_t c = esp_timer_get_time();
        esp_camera_fb_return(fb);
        if (r != ESP_H264_ERR_OK) { printf("ERR codifica fotogramma %d: %d\n", f, r); break; }
        t_cap += b - a;
        t_enc += c - b;
        if (c - b > t_max) t_max = c - b;
        bytes += out.length;
        if (out.frame_type == ESP_H264_FRAME_TYPE_IDR || out.frame_type == ESP_H264_FRAME_TYPE_I) nidr++;
        ok++;
    }
    const double secs = (esp_timer_get_time() - t0) / 1e6;
    if (ok) {
        printf("OK %dx%d: %d fotogrammi in %.2f s = %.1f fps reali; codifica media %.1f ms (max %.1f ms) -> max %.1f fps; "
               "cattura media %.1f ms; %.0f kbit/s a %d fps (obiettivo %d), %d immagini I/IDR\n",
               j->w, j->h, ok, secs, ok / secs, t_enc / 1000.0 / ok, t_max / 1000.0, 1e6 * ok / (double)t_enc,
               t_cap / 1000.0 / ok, bytes * 8.0 * j->fps / ok / 1000.0, j->fps, j->kbps, nidr);
        printf("    memoria libera: interna %u KB, PSRAM %u KB\n", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
               (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));
        j->result = ok == j->frames ? 0 : -1;
    }
    heap_caps_free(out_buf);
close:
    esp_h264_enc_close(enc);
    esp_h264_enc_del(enc);
    esp_camera_deinit();
out:
    xSemaphoreGive(j->done);
    vTaskDeleteWithCaps(NULL);
}

int pdx_cam_bench(int w, int h, int frames, int kbps, int fps)
{
    bench_job_t j = {.w = w, .h = h, .frames = frames, .kbps = kbps, .fps = fps, .done = xSemaphoreCreateBinary()};
    /* openh264 vuole uno stack ampio: in PSRAM, la RAM interna serve al DMA della camera */
    xTaskCreatePinnedToCoreWithCaps(bench_task, "cam_bench", 16384, &j, 5, NULL, 1, MALLOC_CAP_SPIRAM);
    xSemaphoreTake(j.done, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(50));                 /* il task si cancella da solo */
    vSemaphoreDelete(j.done);
    return j.result;
}
