/* Beacon DATV 13 cm - IZ7PDX
 * Immagini e video del beacon nella partizione "symbols" (offset relativi alla partizione, che parte a 0x210000):
 *
 *   0x000000  simboli DVB-S2 precalcolati (PLAY, prove)
 *   0x600000  monoscopio STANDARD, flussi elementari PDXES001 (tools/make_es.py)          protetto
 *   0x700000  monoscopio PERSONALIZZATO video, PDXES001, fino a 3 MB                      scrivibile
 *   0xA00000  sfondo STANDARD del PiP, immagine PDXIMG01 320x240 YUYV (tools/make_bg.py)  protetto
 *   0xA40000  immagine PERSONALIZZATA, PDXIMG01 320x240 YUYV                              scrivibile
 *   0xC00000  vettori di prova del codificatore DVB-S2 (S2TEST)
 *
 * "Protetto": il firmware non ha alcun comando che scriva o cancelli le aree standard; le scritture (pagina web,
 * seriale) sono accettate solo dentro le due aree personalizzate. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PDX_ES_STD_OFF  0x600000u
#define PDX_ES_STD_SIZE 0x100000u
#define PDX_ES_USR_OFF  0x700000u
#define PDX_ES_USR_SIZE 0x300000u
#define PDX_IMG_STD_OFF 0xA00000u
#define PDX_IMG_USR_OFF 0xA40000u
#define PDX_IMG_SIZE    0x40000u

#define PDX_IMG_W 320
#define PDX_IMG_H 240
#define PDX_IMG_HDR 64u
#define PDX_IMG_BYTES (PDX_IMG_HDR + PDX_IMG_W * PDX_IMG_H * 2u)

typedef enum { PDX_MEDIA_ES_STD, PDX_MEDIA_ES_USR, PDX_MEDIA_IMG_STD, PDX_MEDIA_IMG_USR } pdx_media_t;

/* intestazione delle immagini: magic "PDXIMG01", larghezza, altezza, formato (1 = YUYV), byte di dati, CRC-32 dei dati,
 * label[36] */
typedef struct {
    char magic[8];
    uint16_t w, h;
    uint32_t fmt, len, crc;
    char label[36];
    uint8_t rsv[4];
} pdx_img_header_t;

/* file mappato e verificato (intestazione e dimensioni; per le immagini anche il CRC); NULL se assente o non valido */
const uint8_t *pdx_media_map(pdx_media_t m, const char **why);
/* descrizione breve per la pagina web e la seriale ("assente" se manca) */
void pdx_media_describe(pdx_media_t m, char *out, size_t n);

/* scrittura di un'area personalizzata: begin cancella man mano, write accoda, end verifica il contenuto */
const char *pdx_media_write_begin(pdx_media_t m, uint32_t total);
const char *pdx_media_write(const uint8_t *data, uint32_t len);
const char *pdx_media_write_end(void);
const char *pdx_media_erase_user(pdx_media_t m);
