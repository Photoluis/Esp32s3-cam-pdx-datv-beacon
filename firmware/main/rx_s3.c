/* ESP32-S3_PDX Parrot - IZ7PDX - GPL-3.0
 * Ricezione con il motore di cattura ("dump") della radio dell'ESP32-S3.
 *
 * Registri e sequenza ripresi da esp32-sdr-trx (jochenhammes, firmware/src/radio.c e board.h), a sua volta basato su
 * eSpDR (h0m3us3r): codice con licenza 0BSD, qui adattato all'ESP-IDF e al firmware del beacon. La PHY è già calibrata
 * dal driver Wi-Fi (pdx_rf_init); qui si prende il PLL in software, si spegne l'AGC del Wi-Fi, si forza il guadagno e
 * si accende il motore di cattura a 16 Msps sul banco 0x3FCD0000, lo stesso già riservato al DAC del modulatore.
 *
 * P1a: una sola cattura grezza di 1 ms (16384 coppie). La decimazione in tempo reale per catture lunghe è la P1b.
 */
#include "rx_s3.h"

#include <string.h>
#include "esp_cpu.h"
#include "esp_private/regi2c_ctrl.h"
#include "esp_rom_sys.h"
#include "rx_lo_plan.h"

#define REG(a) (*(volatile uint32_t *)(a))

/* motore di cattura */
#define DUMP_CTRL_REG        0x60033D5Cu   /* bit 31 avvio, bit 0..15 lunghezza dell'anello */
#define DUMP_WRITE_INDEX_REG 0x60033D60u
#define DUMP_CONFIG_REG      0x60033D90u
#define DUMP_BANK_SELECT_REG 0x600C101Cu   /* bit 0..3: banco 0..3 (lo stesso registro del DAC del modulatore) */
#define DUMP_CTRL_RUN        0x80000000u
#define DUMP_CTRL_CIRCULAR   0x00024000u   /* anello di 16384 coppie */
#define DUMP_CTRL_16MSPS     0x00010000u
#define DUMP_CONFIG_IQ       0x000C2040u
#define BANK_INDEX           2u            /* 0x3FCB0000 + 2 * 64 KB = 0x3FCD0000 */
#define BANK                 ((volatile uint32_t *)0x3FCD0000u)
#define SENTINEL             0xA5C33C5Au

/* banda base, AGC, front-end */
#define BB_ENABLE_REG        0x6002600Cu
#define AGC_CTRL_REG         0x6001C01Cu
#define AGC_GAIN_FORCE_REG   0x6001C02Cu
#define AGC_DISABLE_REG      0x6001C034u
#define AGC_RX_FORCE_REG     0x6001C080u
#define FE_WIDTH_REG         0x60006100u
#define PBUS_CTRL_REG        0x60006104u
#define PBUS_MODE_REG        0x6000610Cu
#define PBUS_STATUS_REG      0x60006110u
#define PBUS_BB_GAIN_REG     0x60006118u
#define PBUS_RF_GAIN_REG     0x6000611Cu
#define RFPLL_OWNER_REG      0x6000E0C4u
#define I2C_RFPLL            0x62
#define I2C_SDM              0x63
#define I2C_BB_FILTER        0x67
#define PBUS_TIMEOUT_CYCLES  24000u

static uint32_t lo_eff;
static unsigned rf_gain, bb_gain;

static uint8_t analog_read(uint8_t block, uint8_t reg) { return regi2c_ctrl_read_reg(block, 1, reg); }
static void analog_write(uint8_t block, uint8_t reg, uint8_t v) { regi2c_ctrl_write_reg(block, 1, reg, v); }
static void analog_write_bits(uint8_t block, uint8_t reg, uint8_t mask, uint8_t v)
{
    const uint8_t old = analog_read(block, reg);
    analog_write(block, reg, (uint8_t)((old & ~mask) | (v & mask)));
}

static void set_pll_capacitor(unsigned cap)
{
    analog_write(I2C_RFPLL, 1, (uint8_t)cap);
    analog_write_bits(I2C_RFPLL, 2, 0x10, (uint8_t)((cap >> 8) << 4));
}

static void set_pll_manual_capacitor(bool manual) { analog_write_bits(I2C_RFPLL, 11, 0x40, manual ? 0x40 : 0); }

/* parola sigma-delta, calibrazione del PLL, poi ricerca del condensatore del VCO al centro della zona agganciata */
static const char *tune_pll(uint32_t hz, struct esp32s3_lo_plan *plan)
{
    if (!esp32s3_plan_lo(hz, ESP32S3_LO_AUTO, plan)) return "frequenza fuori dai limiti del PLL";
    const uint32_t word = plan->sdm_word;
    analog_write_bits(ESP32S3_CKGEN_BLOCK, ESP32S3_CKGEN_REG, ESP32S3_CKGEN_5_6_BIT, 0);
    REG(RFPLL_OWNER_REG) |= 1u << 25;
    set_pll_manual_capacitor(false);
    analog_write(I2C_SDM, 0, 0x07);
    analog_write(I2C_SDM, 3, (uint8_t)(word >> 16));
    analog_write(I2C_SDM, 4, (uint8_t)(word >> 8));
    analog_write(I2C_SDM, 5, (uint8_t)word);
    analog_write(I2C_SDM, 0, 0x17);
    analog_write_bits(I2C_RFPLL, 0, 0x40, 0x40);
    analog_write_bits(I2C_RFPLL, 0, 0x20, 0x00);
    analog_write_bits(I2C_RFPLL, 0, 0x20, 0x20);
    analog_write_bits(I2C_RFPLL, 0, 0x40, 0x00);
    bool calibrated = false;
    for (unsigned poll = 0; poll < 100 && !calibrated; poll++) {
        esp_rom_delay_us(20);
        calibrated = analog_read(I2C_RFPLL, 7) & 2;
    }
    if (!calibrated) return "calibrazione del PLL non riuscita";
    esp_rom_delay_us(5);
    const uint8_t saved_low = analog_read(I2C_RFPLL, 1), saved_high = analog_read(I2C_RFPLL, 2);
    const uint8_t saved_mode = analog_read(I2C_RFPLL, 11);
    set_pll_manual_capacitor(true);
    unsigned run_start = 0, run_len = 0, best_start = 0, best_len = 0;
    for (unsigned cap = 0; cap < 512; cap++) {
        set_pll_capacitor(cap);
        esp_rom_delay_us(20);
        if (((analog_read(I2C_RFPLL, 12) >> 2) & 3) != 0) { run_len = 0; continue; }
        if (run_len++ == 0) run_start = cap;
        if (run_len > best_len) { best_start = run_start; best_len = run_len; }
    }
    analog_write(I2C_RFPLL, 1, saved_low);
    analog_write_bits(I2C_RFPLL, 2, 0x10, saved_high);
    analog_write_bits(I2C_RFPLL, 11, 0x40, saved_mode);
    if (!best_len) return "PLL mai agganciato";
    set_pll_capacitor(best_start + (best_len - 1) / 2);
    set_pll_manual_capacitor(true);
    return NULL;
}

static bool pbus_write(unsigned block, unsigned index, unsigned value)
{
    const uint32_t fields = ((value & 511u) << 6) | ((block & 15u) << 2) | ((index & 3u) << 15);
    REG(PBUS_CTRL_REG) = (REG(PBUS_CTRL_REG) & 0xFFFE0001u) | (fields & 0x1FFFCu) | 2u;
    const uint32_t t0 = esp_cpu_get_cycle_count();
    while (REG(PBUS_STATUS_REG) & 0x80000000u) {
        if (esp_cpu_get_cycle_count() - t0 > PBUS_TIMEOUT_CYCLES) {
            REG(PBUS_CTRL_REG) &= ~2u;
            return false;
        }
    }
    REG(PBUS_CTRL_REG) &= ~2u;
    return true;
}

static void set_width_20(void)
{
    REG(FE_WIDTH_REG) &= ~0x003F0000u;
    REG(BB_ENABLE_REG) &= ~0xCu;
}

static void park(void)
{
    REG(DUMP_CTRL_REG) &= ~DUMP_CTRL_RUN;
    REG(DUMP_BANK_SELECT_REG) &= ~15u;
    REG(AGC_RX_FORCE_REG) &= ~0xC1u;
    REG(PBUS_STATUS_REG) &= ~0xCF00u;
    REG(BB_ENABLE_REG) &= ~2u;
}

/* banda base accesa, AGC del Wi-Fi spento, guadagno forzato, stadi analogici tenuti via PBUS, TX spento */
static const char *configure_rx(unsigned gain)
{
    park();
    set_width_20();
    REG(BB_ENABLE_REG) |= 0x10000000u;
    REG(BB_ENABLE_REG) &= ~2u;
    esp_rom_delay_us(1);
    REG(BB_ENABLE_REG) |= 2u;
    REG(AGC_CTRL_REG) = (REG(AGC_CTRL_REG) & 0xFF00FFFFu) | 0x007F0000u;
    REG(AGC_DISABLE_REG) |= 0x80u;
    REG(AGC_RX_FORCE_REG) |= 1u;
    REG(AGC_GAIN_FORCE_REG) = (REG(AGC_GAIN_FORCE_REG) & 0x007FFFFFu) | ((gain & 127u) << 24) | 0x00800000u;
    REG(PBUS_STATUS_REG) |= 0xC000u;
    set_width_20();
    analog_write(I2C_BB_FILTER, 4, 0);                       /* filtro RC della banda base, 20 MHz, codice 0 */
    analog_write(I2C_BB_FILTER, 5, 0);
    esp_rom_delay_us(100);
    bb_gain = (REG(PBUS_BB_GAIN_REG) >> 9) & 511;
    rf_gain = (REG(PBUS_RF_GAIN_REG) >> 18) & 511;
    REG(PBUS_MODE_REG) &= ~0x08000000u;
    REG(PBUS_CTRL_REG) |= 1u;
    REG(BB_ENABLE_REG) &= ~2u;
    const bool ok = pbus_write(4, 1, 0) && pbus_write(5, 1, 0) && pbus_write(0, 1, 0x184) && pbus_write(1, 1, 0x189) &&
                    pbus_write(1, 2, rf_gain) && pbus_write(0, 1, bb_gain);
    if (!ok) return "scrittura PBUS non riuscita";
    REG(DUMP_CONFIG_REG) = DUMP_CONFIG_IQ;
    esp_rom_delay_us(100);
    REG(DUMP_CTRL_REG) = DUMP_CTRL_CIRCULAR | DUMP_CTRL_16MSPS;
    return NULL;
}

const char *pdx_rx_start(uint32_t lo_hz, unsigned gain)
{
    struct esp32s3_lo_plan plan;
    const char *e = tune_pll(lo_hz, &plan);
    if (e) return e;
    lo_eff = plan.lo_hz;
    e = configure_rx(gain);
    if (e) return e;
    const uint8_t mode = plan.mode == ESP32S3_LO_5_6 ? ESP32S3_CKGEN_5_6_BIT : 0;
    analog_write_bits(ESP32S3_CKGEN_BLOCK, ESP32S3_CKGEN_REG, ESP32S3_CKGEN_5_6_BIT, mode);
    esp_rom_delay_us(3000);
    if ((analog_read(ESP32S3_CKGEN_BLOCK, ESP32S3_CKGEN_REG) & ESP32S3_CKGEN_5_6_BIT) != mode)
        return "modo di conversione del LO non applicato";
    return NULL;
}

uint32_t pdx_rx_lo_hz(void) { return lo_eff; }
void pdx_rx_gains(unsigned *rf, unsigned *bb) { *rf = rf_gain; *bb = bb_gain; }

uint32_t pdx_rx_capture(uint32_t n, const uint32_t **data)
{
    if (n > PDX_RX_PAIRS) n = PDX_RX_PAIRS;
    for (uint32_t i = 0; i < PDX_RX_PAIRS; i++) BANK[i] = SENTINEL;
    __asm__ volatile("memw" ::: "memory");
    const uint32_t ctrl = DUMP_CTRL_CIRCULAR | DUMP_CTRL_16MSPS;
    REG(DUMP_CTRL_REG) = ctrl;
    REG(DUMP_BANK_SELECT_REG) = (REG(DUMP_BANK_SELECT_REG) & ~15u) | (1u << BANK_INDEX);
    __asm__ volatile("memw" ::: "memory");
    REG(DUMP_CTRL_REG) = ctrl | DUMP_CTRL_RUN;
    /* 16384 coppie a 16 Msps = 1,024 ms: si ferma un po' prima del giro dell'anello */
    const uint32_t t0 = esp_cpu_get_cycle_count();
    while ((REG(DUMP_WRITE_INDEX_REG) & (PDX_RX_PAIRS - 1u)) < n - 256u && esp_cpu_get_cycle_count() - t0 < 240000u * 5u) {
    }
    REG(DUMP_CTRL_REG) = ctrl;
    __asm__ volatile("memw" ::: "memory");
    REG(DUMP_BANK_SELECT_REG) &= ~15u;
    esp_rom_delay_us(20);                                    /* ultimi campioni ancora in arrivo */
    uint32_t written = 0;
    for (uint32_t i = 0; i < n; i++) written += BANK[i] != SENTINEL;
    *data = (const uint32_t *)BANK;
    return written;
}
