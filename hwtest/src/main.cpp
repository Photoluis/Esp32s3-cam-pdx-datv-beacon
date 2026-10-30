// Beacon DATV 13 cm - IZ7PDX
// HW0001: prova della scheda (chip, flash, PSRAM, camera, microSD). Nessuna RF, Wi-Fi spento.

#include <Arduino.h>
#include <esp_camera.h>
#include <esp_chip_info.h>
#include <esp_wifi.h>
#include <SD_MMC.h>

// Pin della camera: schema Freenove / ESP32-S3-EYE (da confermare con la scheda)
#define CAM_PWDN  -1
#define CAM_RESET -1
#define CAM_XCLK  15
#define CAM_SIOD   4
#define CAM_SIOC   5
#define CAM_Y9    16
#define CAM_Y8    17
#define CAM_Y7    18
#define CAM_Y6    12
#define CAM_Y5    10
#define CAM_Y4     8
#define CAM_Y3     9
#define CAM_Y2    11
#define CAM_VSYNC  6
#define CAM_HREF   7
#define CAM_PCLK  13

// microSD in modo 1 bit (schema Freenove)
#define SD_CLK 39
#define SD_CMD 38
#define SD_D0  40

static int results_ok = 0, results_ko = 0;

static void result(const char *name, bool ok, const char *detail)
{
  Serial.printf("[%s] %-28s %s\n", ok ? "OK" : "KO", name, detail);
  if (ok) results_ok++; else results_ko++;
}

static void test_chip()
{
  esp_chip_info_t info;
  esp_chip_info(&info);
  char buf[160];
  snprintf(buf, sizeof(buf), "core=%d rev=%d cpu=%lu MHz model=%s",
           info.cores, info.revision, (unsigned long)getCpuFrequencyMhz(), ESP.getChipModel());
  result("chip", info.cores == 2 && getCpuFrequencyMhz() == 240, buf);

  uint32_t fs = ESP.getFlashChipSize();
  snprintf(buf, sizeof(buf), "%lu MB, %lu MHz", (unsigned long)(fs >> 20),
           (unsigned long)(ESP.getFlashChipSpeed() / 1000000));
  result("flash", fs == 16u << 20, buf);
}

static void test_psram()
{
  char buf[160];
  size_t total = ESP.getPsramSize();
  const size_t n = 4u << 20;  // 4 MB di prova
  uint32_t *p = (uint32_t *)ps_malloc(n);
  bool ok = p != nullptr;
  uint32_t t_us = 0;
  if (ok) {
    uint32_t t0 = micros();
    for (size_t i = 0; i < n / 4; i++) p[i] = i * 2654435761u;
    for (size_t i = 0; i < n / 4 && ok; i++) ok = p[i] == i * 2654435761u;
    t_us = micros() - t0;
    free(p);
  }
  snprintf(buf, sizeof(buf), "totale %u KB, libera %u KB, prova 4 MB %s (%.1f MB/s scrittura+lettura)",
           (unsigned)(total >> 10), (unsigned)(ESP.getFreePsram() >> 10), ok ? "superata" : "FALLITA",
           t_us ? (2.0 * n / t_us) : 0.0);
  result("psram", ok && total >= (7u << 20), buf);
}

static bool cam_init(pixformat_t fmt, framesize_t size)
{
  camera_config_t c = {};
  c.pin_pwdn = CAM_PWDN;   c.pin_reset = CAM_RESET; c.pin_xclk = CAM_XCLK;
  c.pin_sccb_sda = CAM_SIOD; c.pin_sccb_scl = CAM_SIOC;
  c.pin_d7 = CAM_Y9; c.pin_d6 = CAM_Y8; c.pin_d5 = CAM_Y7; c.pin_d4 = CAM_Y6;
  c.pin_d3 = CAM_Y5; c.pin_d2 = CAM_Y4; c.pin_d1 = CAM_Y3; c.pin_d0 = CAM_Y2;
  c.pin_vsync = CAM_VSYNC; c.pin_href = CAM_HREF; c.pin_pclk = CAM_PCLK;
  c.xclk_freq_hz = 20000000;
  c.ledc_timer = LEDC_TIMER_0; c.ledc_channel = LEDC_CHANNEL_0;
  c.pixel_format = fmt;
  c.frame_size = size;
  c.jpeg_quality = 12;
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  return esp_camera_init(&c) == ESP_OK;
}

// esp_camera_deinit() + nuova init si blocca (gdma) nel core 2.0.17: si cambia solo la risoluzione,
// e il cambio di formato JPEG -> YUV avviene dopo un riavvio (fase in RTC).
RTC_NOINIT_ATTR static uint32_t phase_magic;
#define PHASE_YUV 0x59555632u

static bool cam_measure(const char *name, framesize_t size)
{
  char buf[160];
  sensor_t *s = esp_camera_sensor_get();
  if (!s || s->set_framesize(s, size) != 0) {
    result(name, false, "set_framesize fallita");
    return false;
  }
  // scarta i primi fotogrammi (esposizione automatica)
  for (int i = 0; i < 8; i++) { camera_fb_t *fb = esp_camera_fb_get(); if (fb) esp_camera_fb_return(fb); }
  int frames = 0, w = 0, h = 0;
  size_t bytes = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 3000) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) break;
    frames++; bytes += fb->len; w = fb->width; h = fb->height;
    esp_camera_fb_return(fb);
  }
  float secs = (millis() - t0) / 1000.0f;
  snprintf(buf, sizeof(buf), "%dx%d %.1f fps, media %u byte/fotogramma",
           w, h, frames / secs, frames ? (unsigned)(bytes / frames) : 0);
  result(name, frames > 0, buf);
  return frames > 0;
}

// diagnosi: commutazioni di ogni GPIO in 200 ms di campionamento continuo
static void pin_scan()
{
  static uint16_t toggles[49];
  memset(toggles, 0, sizeof(toggles));
  // abilita l'ingresso sui GPIO liberi, per trovare un eventuale segnale della camera su un pin diverso
  static const int spare[] = {1, 2, 3, 14, 21, 38, 39, 40, 41, 42, 45, 46, 47};
  for (int p : spare) gpio_set_direction((gpio_num_t)p, GPIO_MODE_INPUT);
  uint32_t prev0 = REG_READ(GPIO_IN_REG), prev1 = REG_READ(GPIO_IN1_REG);
  uint32_t t0 = millis();
  while (millis() - t0 < 200) {
    uint32_t v0 = REG_READ(GPIO_IN_REG), v1 = REG_READ(GPIO_IN1_REG);
    uint32_t d0 = v0 ^ prev0, d1 = v1 ^ prev1;
    for (int i = 0; d0 && i < 32; i++) if (d0 & (1u << i)) { if (toggles[i] < 65535) toggles[i]++; }
    for (int i = 0; d1 && i < 17; i++) if (d1 & (1u << i)) { if (toggles[32 + i] < 65535) toggles[32 + i]++; }
    prev0 = v0; prev1 = v1;
  }
  Serial.print("[--] GPIO attivi (commutazioni/200 ms):");
  for (int i = 0; i < 49; i++) if (toggles[i]) Serial.printf(" %d=%u", i, toggles[i]);
  Serial.println();

  // D0 (GPIO11): con pull-up e pull-down interni si vede se la linea è scollegata o tenuta bassa
  int highs_up = 0, highs_down = 0;
  gpio_set_pull_mode((gpio_num_t)CAM_Y2, GPIO_PULLUP_ONLY);
  delay(5);
  for (int i = 0; i < 1000; i++) highs_up += gpio_get_level((gpio_num_t)CAM_Y2);
  gpio_set_pull_mode((gpio_num_t)CAM_Y2, GPIO_PULLDOWN_ONLY);
  delay(5);
  for (int i = 0; i < 1000; i++) highs_down += gpio_get_level((gpio_num_t)CAM_Y2);
  gpio_set_pull_mode((gpio_num_t)CAM_Y2, GPIO_FLOATING);
  Serial.printf("[--] GPIO11 livelli alti su 1000 letture: pull-up %d, pull-down %d\n", highs_up, highs_down);
}

static void test_camera_jpeg()
{
  char buf[160];
  // buffer allocati per la risoluzione massima, poi si scende
  if (!cam_init(PIXFORMAT_JPEG, FRAMESIZE_QXGA)) {
    result("camera init", false, "nessuna risposta: pin diversi o camera non collegata");
    return;
  }
  sensor_t *s = esp_camera_sensor_get();
  uint16_t pid = s ? s->id.PID : 0;
  snprintf(buf, sizeof(buf), "PID 0x%04X (%s)", pid,
           pid == OV3660_PID ? "OV3660" : pid == OV2640_PID ? "OV2640" : pid == OV5640_PID ? "OV5640" : "?");
  result("camera sensore", pid == OV3660_PID, buf);
  pin_scan();

  if (!cam_measure("JPEG 2048x1536", FRAMESIZE_QXGA)) return;  // nessun JPEG valido: inutile scendere
  cam_measure("JPEG 1280x720",  FRAMESIZE_HD);
  cam_measure("JPEG 640x480",   FRAMESIZE_VGA);
  cam_measure("JPEG 320x240",   FRAMESIZE_QVGA);
}

static void test_camera_yuv()
{
  if (!cam_init(PIXFORMAT_YUV422, FRAMESIZE_VGA)) {
    result("camera init YUV", false, "esp_camera_init fallita");
    return;
  }
  cam_measure("YUV422 640x480", FRAMESIZE_VGA);
  // 320x240 dopo 640x480 va in panic (set_framesize in YUV): da studiare
}

static void test_sd()
{
  char buf[160];
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true)) {
    result("microSD", false, "non montata (scheda assente o pin diversi)");
    return;
  }
  snprintf(buf, sizeof(buf), "tipo %d, %llu MB (sola lettura, nessun file scritto)",
           (int)SD_MMC.cardType(), SD_MMC.cardSize() >> 20);
  result("microSD", true, buf);
  SD_MMC.end();
}

void setup()
{
  Serial.begin(115200);
  delay(1500);
  esp_wifi_stop();  // nessuna emissione RF in questa prova
  esp_log_level_set("*", ESP_LOG_DEBUG);
  if (phase_magic != PHASE_YUV) {
    Serial.printf("\n=== Beacon DATV IZ7PDX - %s - fase 1/2 ===\n", HWTEST_VERSION);
    test_chip();
    test_psram();
    test_camera_jpeg();
    test_sd();
    Serial.printf("=== FASE 1: %d OK, %d KO - riavvio per la fase YUV ===\n", results_ok, results_ko);
    Serial.flush();
    phase_magic = PHASE_YUV;
    esp_restart();
  }
  phase_magic = 0;
  Serial.printf("\n=== Beacon DATV IZ7PDX - %s - fase 2/2 ===\n", HWTEST_VERSION);
  test_camera_yuv();
  Serial.printf("=== FINE: %d OK, %d KO ===\n", results_ok, results_ko);
}

void loop()
{
  delay(1000);
}
