/* ESP32-S3-CAM_PDX DATV Beacon - IZ7PDX - GPL-3.0
 * Porta dei comandi: UART0 (CH340, scheda ESP32-S3-CAM) oppure USB nativa (USB-Serial-JTAG, XIAO ESP32S3).
 * con_rx_pending_hw() legge solo registri: si usa durante la TX, con gli interrupt del core 0 mascherati il byte resta
 * nella FIFO hardware. Sulla USB la velocità non conta: con_set_baud() non fa nulla. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_PDX_BOARD_XIAO_S3
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_select.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "hal/usb_serial_jtag_ll.h"

static inline void con_init(void)
{
    usb_serial_jtag_driver_config_t c = {.tx_buffer_size = 1024, .rx_buffer_size = 4096 + 64};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&c));
    usb_serial_jtag_vfs_use_driver();               /* printf passa dal driver: niente perdite con il PC collegato */
}
/* come uart_read_bytes: aspetta tutti gli n byte o il tempo (il driver USB restituisce solo quelli già arrivati) */
static inline int con_read(void *buf, uint32_t n, TickType_t t)
{
    const TickType_t t0 = xTaskGetTickCount();
    uint32_t got = 0;
    while (got < n) {
        const TickType_t el = xTaskGetTickCount() - t0;
        if (got && el >= t) break;
        const int k = usb_serial_jtag_read_bytes((uint8_t *)buf + got, n - got, el < t ? t - el : 0);
        if (k > 0) got += (uint32_t)k;
        else if (xTaskGetTickCount() - t0 >= t) break;
    }
    return (int)got;
}
static inline void con_write(const void *buf, size_t n) { usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(1000)); }
static inline void con_wait_tx(TickType_t t) { usb_serial_jtag_wait_tx_done(t); }
static inline size_t con_buffered(void) { return usb_serial_jtag_get_read_bytes_available(); }
static inline void con_flush_input(void)
{
    uint8_t b[64];
    while (usb_serial_jtag_read_bytes(b, sizeof(b), 0) > 0) {}
}
static inline void con_set_baud(uint32_t baud) { (void)baud; }
static inline IRAM_ATTR bool con_rx_pending_hw(void) { return usb_serial_jtag_ll_rxfifo_data_available() != 0; }

#else
#include "driver/uart.h"
#include "hal/uart_ll.h"

static inline void con_init(void) { ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0)); }
static inline int con_read(void *buf, uint32_t n, TickType_t t) { return uart_read_bytes(UART_NUM_0, buf, n, t); }
static inline void con_write(const void *buf, size_t n) { uart_write_bytes(UART_NUM_0, buf, n); }
static inline void con_wait_tx(TickType_t t) { uart_wait_tx_done(UART_NUM_0, t); }
static inline size_t con_buffered(void)
{
    size_t n = 0;
    uart_get_buffered_data_len(UART_NUM_0, &n);
    return n;
}
static inline void con_flush_input(void) { uart_flush_input(UART_NUM_0); }
static inline void con_set_baud(uint32_t baud) { uart_set_baudrate(UART_NUM_0, baud); }
static inline IRAM_ATTR bool con_rx_pending_hw(void) { return uart_ll_get_rxfifo_len(UART_LL_GET_HW(UART_NUM_0)) > 0; }
#endif
