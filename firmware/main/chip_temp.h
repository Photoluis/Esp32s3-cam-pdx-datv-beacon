/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Temperatura del chip dal sensore interno dell'ESP32-S3 (indicativa: è la temperatura del silicio, non dell'aria).
 * Da leggere fuori dalla TX: il sensore sta nella parte analogica condivisa con la radio. */
#pragma once

void pdx_temp_init(void);
float pdx_temp_read(void);      /* gradi Celsius; NAN se il sensore non è disponibile */
