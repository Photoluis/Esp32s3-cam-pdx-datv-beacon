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

#include <math.h>
#include <string.h>
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_private/regi2c_ctrl.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rx_lo_plan.h"
#include "led_status.h"

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

bool pdx_rx_led_used;

const char *pdx_rx_start(uint32_t lo_hz, unsigned gain)
{
    pdx_led_rx();                                  /* semaforo: verde = radio in ricezione */
    pdx_rx_led_used = true;
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

/* ---------------------------------------------------------------- P1b: cattura continua con decimazione
 *
 * Il motore di cattura scrive in un banco di SRAM per volta e, finché quel banco è suo, la CPU non lo può leggere (legge
 * un valore fisso: provato il 02/11). Si usano a turno i due banchi riservati, 0x3FCC0000 (A) e 0x3FCD0000 (B):
 * il motore riempie un banco per UNIT_PAIRS coppie (~0,77 ms), poi passa all'altro; l'indice dell'anello prosegue da un
 * banco all'altro. La fine esatta del blocco nel banco lasciato si trova con i segnaposto scritti prima (stesso metodo
 * di esp32-sdr-trx): i campioni ancora in viaggio arrivano nel banco vecchio entro pochi cicli dal cambio. Mentre il
 * motore riempie il banco nuovo, la CPU decima quello lasciato; deve andare più veloce di 16 Msps.
 *
 * Decimatore: stadio 1 = somma di 4 coppie con I e Q nello stesso registro (offset binario: XOR 0x80200, due campi
 * separati da maschere), stadio 2 = CIC di ordine 2 o 3 con decimazione r2 = 2 o 4 a 4 Msps, poi filtro anti-continua
 * e coniugazione (lo spettro del motore è invertito). Uscita int8 I/Q a 16/(4*r2) Msps.
 */

#define RING_MASK    (PDX_RX_PAIRS - 1u)
#define BANK_A       1u                  /* 0x3FCC0000 */
#define BANK_B       2u                  /* 0x3FCD0000 */
#define UNIT_PAIRS   12288u              /* coppie per banco prima del cambio (0,77 ms) */
#define GUARD        1024u               /* zona di segnaposto attorno alla fine prevista */

static inline volatile uint32_t *bank_ptr(unsigned b) { return (volatile uint32_t *)(0x3FCB0000u + b * 0x10000u); }

typedef struct {
    uint32_t a1i, a2i, a3i, a1q, a2q, a3q, z1i, z2i, z3i, z1q, z2q, z3q;
    int32_t dci, dcq;                    /* stima della continua (uscita dello stadio 2, <<8) */
    uint32_t ph, r2, order, shift, k, n_out, clip, primed;
    uint32_t carry[16], nc;              /* coppie avanzate (meno di un blocco) */
    uint32_t fast;                       /* percorso veloce CIC3 /16: dci/dcq = continua nel dominio dei pettini */
    uint16_t *out;
} dec_t;

/* uscita dello stadio 2 (una volta ogni r2 gruppi): pettini, continua, coniugazione, int8 */
static IRAM_ATTR __attribute__((noinline)) void comb_out(dec_t *d, uint32_t xi, uint32_t xq)
{
    uint32_t ci = xi - d->z1i, cq = xq - d->z1q;
    d->z1i = xi; d->z1q = xq;
    uint32_t ei = ci - d->z2i, eq = cq - d->z2q;
    d->z2i = ci; d->z2q = cq;
    if (d->order == 3) {
        const uint32_t fi = ei - d->z3i, fq = eq - d->z3q;
        d->z3i = ei; d->z3q = eq;
        ei = fi; eq = fq;
    }
    int32_t yi = (int32_t)ei, yq = (int32_t)eq;
    if (d->primed < 8) {                 /* primi campioni: pettini e integratori in assestamento */
        d->dci = yi << 8; d->dcq = yq << 8;
        d->primed++;
        return;
    }
    d->dci += ((yi << 8) - d->dci) >> 12;    /* continua: costante di tempo 4096 campioni */
    d->dcq += ((yq << 8) - d->dcq) >> 12;
    yi = (yi - (d->dci >> 8)) >> d->shift;
    yq = -((yq - (d->dcq >> 8)) >> d->shift); /* coniugato: spettro dritto */
    if (yi > 127) { yi = 127; d->clip++; } else if (yi < -127) { yi = -127; d->clip++; }
    if (yq > 127) { yq = 127; d->clip++; } else if (yq < -127) { yq = -127; d->clip++; }
    if (d->k < d->n_out) d->out[d->k++] = (uint16_t)((uint8_t)yi | ((uint16_t)(uint8_t)yq << 8));
}

/* somma di 4 coppie (I e Q in campi separati, offset binario) -> valori con segno */
#define SUM4(w0, w1, w2, w3, si, sq) do {                                                                     \
        const uint32_t t0 = (w0) ^ 0x80200u, t1 = (w1) ^ 0x80200u, t2 = (w2) ^ 0x80200u, t3 = (w3) ^ 0x80200u; \
        si = ((t0 & 0x3FFu) + (t1 & 0x3FFu) + (t2 & 0x3FFu) + (t3 & 0x3FFu)) - 2048u;                        \
        sq = (((t0 & 0xFFC00u) + (t1 & 0xFFC00u) + (t2 & 0xFFC00u) + (t3 & 0xFFC00u)) >> 10) - 2048u;         \
    } while (0)

/* decima n coppie contigue; lo stato resta nei registri durante il ciclo */
static IRAM_ATTR __attribute__((noinline)) void dec_feed(dec_t *d, const volatile uint32_t *pv, uint32_t n)
{
    const uint32_t *p = (const uint32_t *)pv;            /* banco già lasciato dal motore: memoria normale */
    __asm__ volatile("memw" ::: "memory");
    while (d->nc && n) {                                  /* completa il gruppo lasciato a metà */
        d->carry[d->nc++] = *p++;
        n--;
        if (d->nc == 4) {
            uint32_t si, sq;
            SUM4(d->carry[0], d->carry[1], d->carry[2], d->carry[3], si, sq);
            d->a1i += si; d->a2i += d->a1i; d->a3i += d->a2i;
            d->a1q += sq; d->a2q += d->a1q; d->a3q += d->a2q;
            if (++d->ph == d->r2) { d->ph = 0; comb_out(d, d->order == 3 ? d->a3i : d->a2i, d->order == 3 ? d->a3q : d->a2q); }
            d->nc = 0;
        }
    }
    uint32_t a1i = d->a1i, a2i = d->a2i, a3i = d->a3i, a1q = d->a1q, a2q = d->a2q, a3q = d->a3q;
    uint32_t ph = d->ph;
    const uint32_t r2 = d->r2;
    if (d->order == 3) {
        while (n >= 4) {
            uint32_t si, sq;
            SUM4(p[0], p[1], p[2], p[3], si, sq);
            p += 4;
            n -= 4;
            a1i += si; a2i += a1i; a3i += a2i;
            a1q += sq; a2q += a1q; a3q += a2q;
            if (++ph == r2) { ph = 0; comb_out(d, a3i, a3q); }
        }
    } else {
        while (n >= 4) {
            uint32_t si, sq;
            SUM4(p[0], p[1], p[2], p[3], si, sq);
            p += 4;
            n -= 4;
            a1i += si; a2i += a1i;
            a1q += sq; a2q += a1q;
            if (++ph == r2) { ph = 0; comb_out(d, a2i, a2q); }
        }
        a3i = a2i; a3q = a2q;
    }
    d->a1i = a1i; d->a2i = a2i; d->a3i = a3i; d->a1q = a1q; d->a2q = a2q; d->a3q = a3q;
    d->ph = ph;
    while (n--) d->carry[d->nc++] = *p++;
}

/* ---- percorso veloce: CIC3, decimazione 16 (1 Msps), 16 coppie per giro, stato nei registri ---- */

#define M_OB 0x80200u                    /* complemento a 2 -> offset binario, I e Q insieme */
#define G4(i0, i1, i2, i3)                                                                             \
    do {                                                                                               \
        const uint32_t t0 = p[i0] ^ mob, t1 = p[i1] ^ mob, t2 = p[i2] ^ mob, t3 = p[i3] ^ mob;         \
        const uint32_t si = (t0 & 0x3FFu) + (t1 & 0x3FFu) + (t2 & 0x3FFu) + (t3 & 0x3FFu) - 2048u;     \
        const uint32_t sq = ((t0 >> 10) & 0x3FFu) + ((t1 >> 10) & 0x3FFu) + ((t2 >> 10) & 0x3FFu) +    \
                            ((t3 >> 10) & 0x3FFu) - 2048u;                                             \
        a1i += si; a2i += a1i; a3i += a2i;                                                             \
        a1q += sq; a2q += a1q; a3q += a2q;                                                             \
    } while (0)

/* nblk blocchi di 16 coppie contigue -> nblk campioni */
static IRAM_ATTR __attribute__((noinline)) void dec16_o3(dec_t *d, const uint32_t *p, uint32_t nblk)
{
    uint32_t a1i = d->a1i, a2i = d->a2i, a3i = d->a3i, a1q = d->a1q, a2q = d->a2q, a3q = d->a3q;
    const uint32_t mob = M_OB;
    uint16_t *o = d->out + d->k;
    d->k += nblk;
    while (nblk--) {
        G4(0, 1, 2, 3);
        G4(4, 5, 6, 7);
        G4(8, 9, 10, 11);
        G4(12, 13, 14, 15);
        p += 16;
        /* pettini (stato in memoria: una volta ogni 16 coppie) */
        const uint32_t ci = a3i - d->z1i, cq = a3q - d->z1q;
        d->z1i = a3i; d->z1q = a3q;
        const uint32_t ei = ci - d->z2i, eq = cq - d->z2q;
        d->z2i = ci; d->z2q = cq;
        const uint32_t fi = ei - d->z3i, fq = eq - d->z3q;
        d->z3i = ei; d->z3q = eq;
        int32_t yi = ((int32_t)fi - d->dci) >> d->shift;
        int32_t yq = -(((int32_t)fq - d->dcq) >> d->shift);    /* coniugato: spettro dritto */
        yi = yi > 127 ? 127 : yi < -127 ? -127 : yi;
        yq = yq > 127 ? 127 : yq < -127 ? -127 : yq;
        *o++ = (uint16_t)((uint8_t)yi | ((uint16_t)(uint8_t)yq << 8));
    }
    d->a1i = a1i; d->a2i = a2i; d->a3i = a3i; d->a1q = a1q; d->a2q = a2q; d->a3q = a3q;
}

/* n coppie contigue nel percorso veloce, con le coppie avanzate (meno di 16) tenute per il blocco seguente */
static IRAM_ATTR __attribute__((noinline)) void dec_feed16(dec_t *d, const volatile uint32_t *pv, uint32_t n)
{
    const uint32_t *p = (const uint32_t *)pv;
    __asm__ volatile("memw" ::: "memory");
    if (d->nc) {
        while (d->nc < 16 && n) { d->carry[d->nc++] = *p++; n--; }
        if (d->nc < 16) return;
        dec16_o3(d, d->carry, 1);
        d->nc = 0;
    }
    const uint32_t nb = n / 16u;
    if (nb) dec16_o3(d, p, nb);
    p += nb * 16u;
    n -= nb * 16u;
    while (n--) d->carry[d->nc++] = *p++;
}

/* ---- percorso a due core: CIC3 /16, il core 0 calcola I, il core 1 calcola Q, sugli stessi blocchi ---- */

typedef struct {
    uint32_t a1, a2, a3, z1, z2, z3;
    int32_t dc;                          /* continua nel dominio dei pettini */
    uint32_t carry[16], nc, k, shift;
    int8_t *out;                         /* I: base + 0, Q: base + 1, passo 2 */
} ch_t;

#define EXT_I(w) ((uint32_t)(((int32_t)((w) << 22)) >> 22))
#define EXT_Q(w) ((uint32_t)(((int32_t)((w) << 12)) >> 22))

#define CH16_BODY(EXT, NEG)                                                                              \
    uint32_t a1 = c->a1, a2 = c->a2, a3 = c->a3;                                                        \
    int8_t *o = c->out + 2u * c->k;                                                                     \
    c->k += nblk;                                                                                       \
    while (nblk--) {                                                                                    \
        a1 += EXT(p[0]) + EXT(p[1]) + EXT(p[2]) + EXT(p[3]);    a2 += a1; a3 += a2;                     \
        a1 += EXT(p[4]) + EXT(p[5]) + EXT(p[6]) + EXT(p[7]);    a2 += a1; a3 += a2;                     \
        a1 += EXT(p[8]) + EXT(p[9]) + EXT(p[10]) + EXT(p[11]);  a2 += a1; a3 += a2;                     \
        a1 += EXT(p[12]) + EXT(p[13]) + EXT(p[14]) + EXT(p[15]); a2 += a1; a3 += a2;                    \
        p += 16;                                                                                        \
        const uint32_t cc = a3 - c->z1; c->z1 = a3;                                                     \
        const uint32_t ee = cc - c->z2; c->z2 = cc;                                                     \
        const uint32_t ff = ee - c->z3; c->z3 = ee;                                                     \
        int32_t y = ((int32_t)ff - c->dc) >> c->shift;                                                  \
        if (NEG) y = -y;                                                                                \
        y = y > 127 ? 127 : y < -127 ? -127 : y;                                                        \
        *o = (int8_t)y;                                                                                 \
        o += 2;                                                                                         \
    }                                                                                                   \
    c->a1 = a1; c->a2 = a2; c->a3 = a3;

static IRAM_ATTR __attribute__((noinline)) void ch16_i(ch_t *c, const uint32_t *p, uint32_t nblk) { CH16_BODY(EXT_I, 0) }
/* Q negato: coniugato, lo spettro del motore è invertito */
static IRAM_ATTR __attribute__((noinline)) void ch16_q(ch_t *c, const uint32_t *p, uint32_t nblk) { CH16_BODY(EXT_Q, 1) }

static IRAM_ATTR __attribute__((noinline)) void ch_feed(ch_t *c, const uint32_t *p, uint32_t n, bool q)
{
    if (c->nc) {
        while (c->nc < 16 && n) { c->carry[c->nc++] = *p++; n--; }
        if (c->nc < 16) return;
        if (q) ch16_q(c, c->carry, 1); else ch16_i(c, c->carry, 1);
        c->nc = 0;
    }
    const uint32_t nb = n / 16u;
    if (nb) { if (q) ch16_q(c, p, nb); else ch16_i(c, p, nb); }
    p += nb * 16u;
    n -= nb * 16u;
    while (n--) c->carry[c->nc++] = *p++;
}

/* lavoro per il core 1: segmenti del blocco da elaborare (Q) */
static struct {
    volatile uint32_t seq, done, ready, stop;
    const uint32_t *p1, *p2;
    uint32_t n1, n2;
    ch_t q;
} job;
static portMUX_TYPE core1_mux = portMUX_INITIALIZER_UNLOCKED;

static IRAM_ATTR __attribute__((noinline)) void core1_loop(void)
{
    uint32_t seen = job.seq;
    job.ready = 1;
    while (!job.stop) {
        if (job.seq == seen) continue;
        seen = job.seq;
        __asm__ volatile("memw" ::: "memory");
        ch_feed(&job.q, job.p1, job.n1, true);
        if (job.n2) ch_feed(&job.q, job.p2, job.n2, true);
        __asm__ volatile("memw" ::: "memory");
        job.done = seen;
    }
}

static void core1_task(void *arg)
{
    taskENTER_CRITICAL(&core1_mux);                 /* interruzioni sospese sul core 1 per tutta la cattura */
    core1_loop();
    taskEXIT_CRITICAL(&core1_mux);
    job.ready = 2;
    vTaskDelete(NULL);
}

static ch_t ch_i;

static IRAM_ATTR __attribute__((noinline)) void fill_sentinels(volatile uint32_t *b, uint32_t at, uint32_t n)
{
    while (n--) { b[at] = SENTINEL; at = (at + 1u) & RING_MASK; }
}

/* media di I e Q (in 1/1024 di conteggio) su ~300 us catturati nel banco A, letto dopo lo stop */
static void rx_dc(int32_t *mi, int32_t *mq)
{
    volatile uint32_t *bp = bank_ptr(BANK_A);
    fill_sentinels(bp, 0, PDX_RX_PAIRS);
    __asm__ volatile("memw" ::: "memory");
    const uint32_t ctrl = DUMP_CTRL_CIRCULAR | DUMP_CTRL_16MSPS;
    REG(DUMP_CTRL_REG) = ctrl;
    REG(DUMP_BANK_SELECT_REG) = (REG(DUMP_BANK_SELECT_REG) & ~15u) | (1u << BANK_A);
    __asm__ volatile("memw" ::: "memory");
    REG(DUMP_CTRL_REG) = ctrl | DUMP_CTRL_RUN;
    esp_rom_delay_us(300);
    REG(DUMP_CTRL_REG) = ctrl;
    __asm__ volatile("memw" ::: "memory");
    REG(DUMP_BANK_SELECT_REG) &= ~15u;
    esp_rom_delay_us(20);
    int64_t si = 0, sq = 0;
    uint32_t n = 0;
    for (uint32_t j = 0; j < PDX_RX_PAIRS; j++) {
        const uint32_t w = bp[j];
        if (w == SENTINEL) continue;
        si += ((int32_t)(w << 22)) >> 22;
        sq += ((int32_t)(w << 12)) >> 22;
        n++;
    }
    *mi = n ? (int32_t)(si * 1024 / n) : 0;
    *mq = n ? (int32_t)(sq * 1024 / n) : 0;
}
extern uint32_t rx_diag[12];
static IRAM_ATTR __attribute__((noinline)) uint32_t pingpong(dec_t *d, pdx_rx_stream_stats_t *st)
{
    unsigned cur = BANK_A, nxt = BANK_B;
    uint32_t units_done = 0;
    rx_diag[11] = 0;
    uint32_t start = REG(DUMP_WRITE_INDEX_REG) & RING_MASK;
    while (d->k < d->n_out) {
        uint32_t wi;
        do {
            wi = REG(DUMP_WRITE_INDEX_REG) & RING_MASK;
        } while (((wi - start) & RING_MASK) < UNIT_PAIRS);
        REG(DUMP_BANK_SELECT_REG) = (REG(DUMP_BANK_SELECT_REG) & ~15u) | (1u << nxt);
        __asm__ volatile("memw" ::: "memory");
        const uint32_t wsw = REG(DUMP_WRITE_INDEX_REG) & RING_MASK;
        const uint32_t t0 = esp_cpu_get_cycle_count();
        while (esp_cpu_get_cycle_count() - t0 < 480u) {}       /* 2 us: ultimi campioni in arrivo nel banco vecchio */
        volatile uint32_t *bp = bank_ptr(cur);
        uint32_t e = (wsw - 64u) & RING_MASK, n = 0;
        while (bp[e] != SENTINEL && n < GUARD) { e = (e + 1u) & RING_MASK; n++; }
        if (n >= GUARD) {                                       /* fine del blocco non trovata: diagnosi */
            rx_diag[0] = start; rx_diag[1] = wi; rx_diag[2] = wsw; rx_diag[3] = cur;
            uint32_t ns = 0;
            for (uint32_t j = 0; j < PDX_RX_PAIRS; j++) ns += bp[j] == SENTINEL;
            rx_diag[4] = units_done | (ns << 16);
            for (uint32_t j = 0; j < 4; j++) rx_diag[5 + j] = bp[(wsw - 64u + j) & RING_MASK];
            rx_diag[9] = REG(DUMP_WRITE_INDEX_REG) & RING_MASK;
            volatile uint32_t *bn = bank_ptr(nxt);
            uint32_t nn = 0;
            for (uint32_t j = 0; j < PDX_RX_PAIRS; j++) nn += bn[j] == SENTINEL;
            rx_diag[10] = nn;
            return 1;
        }
        const uint32_t count = (e - start) & RING_MASK;
        const uint32_t tp0 = esp_cpu_get_cycle_count();
        const uint32_t seg1 = PDX_RX_PAIRS - start < count ? PDX_RX_PAIRS - start : count;
        if (d->fast) {                                          /* I sul core 0, Q sul core 1 */
            __asm__ volatile("memw" ::: "memory");
            job.p1 = (const uint32_t *)(bp + start); job.n1 = seg1;
            job.p2 = (const uint32_t *)bp; job.n2 = count - seg1;
            __asm__ volatile("memw" ::: "memory");
            const uint32_t sq = job.seq + 1u;
            job.seq = sq;
            ch_feed(&ch_i, (const uint32_t *)(bp + start), seg1, false);
            if (count > seg1) ch_feed(&ch_i, (const uint32_t *)bp, count - seg1, false);
            while (job.done != sq) {}
            d->k = ch_i.k;
        } else {
            dec_feed(d, bp + start, seg1);
            if (count > seg1) dec_feed(d, bp, count - seg1);
        }
        { const uint32_t tp = esp_cpu_get_cycle_count() - tp0; if (tp > rx_diag[11]) rx_diag[11] = tp; rx_diag[4] = ++units_done; }
        st->pairs_in += count;
        fill_sentinels(bp, (e + 2u * UNIT_PAIRS - GUARD / 2u) & RING_MASK, GUARD);   /* fine del prossimo blocco qui */
        const uint32_t lag = ((REG(DUMP_WRITE_INDEX_REG) & RING_MASK) - e) & RING_MASK;
        if (lag > PDX_RX_PAIRS - 2048u) { st->overruns++; return 2; }             /* troppo lento: banco nuovo pieno */
        if (lag > st->max_lag) st->max_lag = lag;
        start = e;
        const unsigned t = cur; cur = nxt; nxt = t;
    }
    return 0;
}

static portMUX_TYPE rx_mux = portMUX_INITIALIZER_UNLOCKED;
uint32_t rx_diag[12];

const char *pdx_rx_stream(int8_t *out, uint32_t n_out, unsigned decim, unsigned order, unsigned shift,
                          pdx_rx_stream_stats_t *st)
{
    if (decim != 8 && decim != 16) return "decimazione 8 o 16";
    if (order != 2 && order != 3) return "ordine del CIC 2 o 3";
    if (shift > 24) return "shift 0..24";
    memset(st, 0, sizeof(*st));
    static dec_t d;
    memset(&d, 0, sizeof(d));
    d.r2 = decim / 4u; d.order = order; d.shift = shift; d.n_out = n_out; d.out = (uint16_t *)out;
    d.fast = decim == 16 && order == 3;
    if (d.fast) {                        /* continua del ricevitore: 300 us nel banco A prima della cattura */
        int32_t mi, mq;
        rx_dc(&mi, &mq);
        st->dc_i = mi; st->dc_q = mq;
        /* somma di 4 coppie + CIC3 /4: guadagno 4 x 64 = 256; la media è in 1/1024 di conteggio */
        memset(&ch_i, 0, sizeof(ch_i));
        memset((void *)&job, 0, sizeof(job));
        ch_i.dc = mi / 4; ch_i.shift = shift; ch_i.out = out;
        job.q.dc = mq / 4; job.q.shift = shift; job.q.out = out + 1;
        if (xTaskCreatePinnedToCore(core1_task, "rx_q", 3072, NULL, configMAX_PRIORITIES - 1, NULL, 1) != pdPASS)
            return "task del core 1 non creato";
        while (job.ready != 1) vTaskDelay(1);
    }
    fill_sentinels(bank_ptr(BANK_A), 0, PDX_RX_PAIRS);
    fill_sentinels(bank_ptr(BANK_B), 0, PDX_RX_PAIRS);
    __asm__ volatile("memw" ::: "memory");
    const uint32_t ctrl = DUMP_CTRL_CIRCULAR | DUMP_CTRL_16MSPS;
    REG(DUMP_CTRL_REG) = ctrl;
    REG(DUMP_BANK_SELECT_REG) = (REG(DUMP_BANK_SELECT_REG) & ~15u) | (1u << BANK_A);
    __asm__ volatile("memw" ::: "memory");
    taskENTER_CRITICAL(&rx_mux);
    REG(DUMP_CTRL_REG) = ctrl | DUMP_CTRL_RUN;
    const uint32_t c0 = esp_cpu_get_cycle_count();
    const uint32_t r = pingpong(&d, st);
    st->cycles = esp_cpu_get_cycle_count() - c0;
    REG(DUMP_CTRL_REG) = ctrl;
    __asm__ volatile("memw" ::: "memory");
    REG(DUMP_BANK_SELECT_REG) &= ~15u;
    taskEXIT_CRITICAL(&rx_mux);
    if (d.fast) {
        job.stop = 1;
        while (job.ready != 2) vTaskDelay(1);
    }
    st->samples_out = d.k;
    if (d.fast) for (uint32_t k = 0; k < 2u * d.k; k++) d.clip += out[k] == 127 || out[k] == -127;
    st->clipped = d.clip;
    double s2 = 0;
    for (uint32_t k = 0; k < 2u * d.k; k++) s2 += (double)out[k] * out[k];
    st->rms = d.k ? (float)sqrt(s2 / (2.0 * d.k)) : 0.0f;
    if (r == 1) return "fine del blocco non trovata (segnaposto)";
    if (r == 2) return "decimatore troppo lento: il motore ha riempito il banco successivo";
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

/* diagnosi: quali banchi scrive il motore con un dato valore nel registro di selezione */
void pdx_rx_banktest(uint32_t selval, uint32_t us, uint32_t counts[4], uint32_t *idx0, uint32_t *idx1)
{
    for (unsigned b = 1; b <= 2; b++) fill_sentinels(bank_ptr(b), 0, PDX_RX_PAIRS);
    __asm__ volatile("memw" ::: "memory");
    const uint32_t ctrl = DUMP_CTRL_CIRCULAR | DUMP_CTRL_16MSPS;
    REG(DUMP_CTRL_REG) = ctrl;
    REG(DUMP_BANK_SELECT_REG) = (REG(DUMP_BANK_SELECT_REG) & ~15u) | (selval & 15u);
    __asm__ volatile("memw" ::: "memory");
    *idx0 = REG(DUMP_WRITE_INDEX_REG);
    REG(DUMP_CTRL_REG) = ctrl | DUMP_CTRL_RUN;
    esp_rom_delay_us(us);
    REG(DUMP_CTRL_REG) = ctrl;
    *idx1 = REG(DUMP_WRITE_INDEX_REG);
    __asm__ volatile("memw" ::: "memory");
    REG(DUMP_BANK_SELECT_REG) &= ~15u;
    esp_rom_delay_us(20);
    for (unsigned b = 0; b < 4; b++) counts[b] = 0;
    for (unsigned b = 1; b <= 2; b++) {
        volatile uint32_t *bp = bank_ptr(b);
        for (uint32_t j = 0; j < PDX_RX_PAIRS; j++) counts[b] += bp[j] != SENTINEL;
    }
}

/* diagnosi: A per usA microsecondi, poi B per usB; indici prima/dopo il cambio e coppie scritte in ciascun banco */
void pdx_rx_switchtest(uint32_t usA, uint32_t usB, uint32_t out[8])
{
    for (unsigned b = 1; b <= 2; b++) fill_sentinels(bank_ptr(b), 0, PDX_RX_PAIRS);
    __asm__ volatile("memw" ::: "memory");
    const uint32_t ctrl = DUMP_CTRL_CIRCULAR | DUMP_CTRL_16MSPS;
    REG(DUMP_CTRL_REG) = ctrl;
    REG(DUMP_BANK_SELECT_REG) = (REG(DUMP_BANK_SELECT_REG) & ~15u) | 2u;
    __asm__ volatile("memw" ::: "memory");
    REG(DUMP_CTRL_REG) = ctrl | DUMP_CTRL_RUN;
    esp_rom_delay_us(usA);
    out[0] = REG(DUMP_WRITE_INDEX_REG);
    REG(DUMP_BANK_SELECT_REG) = (REG(DUMP_BANK_SELECT_REG) & ~15u) | 4u;
    __asm__ volatile("memw" ::: "memory");
    out[1] = REG(DUMP_WRITE_INDEX_REG);
    esp_rom_delay_us(5);
    out[2] = REG(DUMP_WRITE_INDEX_REG);
    esp_rom_delay_us(usB);
    out[3] = REG(DUMP_WRITE_INDEX_REG);
    REG(DUMP_CTRL_REG) = ctrl;
    __asm__ volatile("memw" ::: "memory");
    REG(DUMP_BANK_SELECT_REG) &= ~15u;
    esp_rom_delay_us(20);
    uint32_t fa = 0xFFFFFFFFu, la = 0, fb = 0xFFFFFFFFu, lb = 0, na = 0, nb = 0;
    volatile uint32_t *A = bank_ptr(1), *B = bank_ptr(2);
    for (uint32_t j = 0; j < PDX_RX_PAIRS; j++) {
        if (A[j] != SENTINEL) { na++; if (fa == 0xFFFFFFFFu) fa = j; la = j; }
        if (B[j] != SENTINEL) { nb++; if (fb == 0xFFFFFFFFu) fb = j; lb = j; }
    }
    out[4] = na | (fa << 16); out[5] = la; out[6] = nb | (fb << 16); out[7] = lb;
}

/* misura del decimatore su dati fermi (banco A): cicli per coppia */
float pdx_rx_bench(unsigned decim, unsigned order, uint32_t reps)
{
    static dec_t d;
    uint16_t *sink = heap_caps_malloc(4096 * 2, MALLOC_CAP_SPIRAM);
    if (!sink) return 0;
    memset(&d, 0, sizeof(d));
    d.r2 = decim / 4u; d.order = order; d.shift = 6; d.n_out = 2048; d.out = sink;
    volatile uint32_t *bp = bank_ptr(BANK_A);
    for (uint32_t j = 0; j < PDX_RX_PAIRS; j++) bp[j] = (j * 2654435761u) & 0xFFFFFu;
    taskENTER_CRITICAL(&rx_mux);
    const uint32_t c0 = esp_cpu_get_cycle_count();
    d.fast = decim == 16 && order == 3;
    for (uint32_t r = 0; r < reps; r++) { d.k = 0; if (d.fast) dec_feed16(&d, bp, 12288u); else dec_feed(&d, bp, 12288u); }
    const uint32_t c = esp_cpu_get_cycle_count() - c0;
    taskEXIT_CRITICAL(&rx_mux);
    heap_caps_free(sink);
    return (float)c / (12288.0f * reps);
}