/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Temperatura del chip (vedi chip_temp.h). */
#include "chip_temp.h"

#include <math.h>
#include "driver/temperature_sensor.h"

static temperature_sensor_handle_t tsens;

void pdx_temp_init(void)
{
    if (tsens) return;
    const temperature_sensor_config_t c = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&c, &tsens) != ESP_OK || temperature_sensor_enable(tsens) != ESP_OK) tsens = NULL;
}

float pdx_temp_read(void)
{
    float t;
    return tsens && temperature_sensor_get_celsius(tsens, &t) == ESP_OK ? t : NAN;
}
