/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Interfaccia web di configurazione: access point Wi-Fi "<QRZ>-BEACON" e pagina su http://192.168.4.1.
 * Attiva solo a TX ferma: durante la TX la radio Wi-Fi fa da modulatore DATV. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "beacon_cfg.h"

void pdx_web_init(pdx_cfg_t *cfg, const char *fw_version);
bool pdx_web_start(void);                  /* access point + server (se già attivi non fa nulla) */
void pdx_web_stop(void);                   /* spegne server e access point e rimette il Wi-Fi nello stato del modulatore */
bool pdx_web_running(void);

void pdx_web_set_mode(const char *mode);   /* testo di stato mostrato nella pagina */

bool pdx_web_take_start_request(void);     /* true una volta dopo "AVVIA BEACON" dalla pagina */
bool pdx_web_seen_client(void);            /* true se la pagina è stata aperta dall'ultimo avvio del server */
/* true se l'access point è stato acceso dall'avvio: dopo il modo AP il DVB-S2 non è più stato agganciato (29/10),
 * quindi prima di una TX la scheda va riavviata (main.c, reboot_and_run) */
bool pdx_web_radio_dirty(void);
void pdx_web_mark_radio_dirty(void);   /* la ricezione (Parrot) ha cambiato la PHY: la prossima TX riavvia */

/* ultimo QSO del Parrot in JSON ("null" se nessuno), da main.c */
const char *pdx_parrot_json(void);

/* accesso alla configurazione condiviso con il server web */
void pdx_cfg_lock(void);
void pdx_cfg_unlock(void);
