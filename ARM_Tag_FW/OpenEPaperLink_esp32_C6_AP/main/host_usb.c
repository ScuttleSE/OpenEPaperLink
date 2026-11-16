// The host link over the chip's native USB port (USB Serial/JTAG), with the same functions as the
// UART link in second_uart.c. There is no baud rate: the switch to 2 Mbaud after FRM1 does nothing.
#include "sdkconfig.h"
#ifdef CONFIG_OEPL_HOST_LINK_USB

#include <stdarg.h>
#include <stdio.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "radio.h"
#include "second_uart.h"

#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG) || defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG)
#error "The USB host link needs the console off USB Serial/JTAG: add sdkconfig.host-usb to SDKCONFIG_DEFAULTS"
#endif

static const char *TAG = "HOST_USB";

#define RX_BUF_SIZE 4096
#define TX_BUF_SIZE 2048
#define TX_WAIT_MS 20

static StreamBufferHandle_t rxStream;
volatile uint32_t uartOverflows = 0;

// Moves what the driver received to rxStream and wakes the main loop, like the UART event task
static void usb_rx_task(void *pvParameters) {
    uint8_t buf[256];
    for (;;) {
        const int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), portMAX_DELAY);
        if (n <= 0) continue;
        if (xStreamBufferSend(rxStream, buf, n, 0) != (size_t) n) uartOverflows++;
        if (radioWakeTask) xTaskNotifyGive(radioWakeTask);
    }
}

void init_second_uart() {
    usb_serial_jtag_driver_config_t cfg = {.tx_buffer_size = TX_BUF_SIZE, .rx_buffer_size = RX_BUF_SIZE};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    rxStream = xStreamBufferCreate(RX_BUF_SIZE, 1);
    ESP_LOGI(TAG, "host link on USB Serial/JTAG");
    xTaskCreate(usb_rx_task, "usb_rx_task", 3072, NULL, 12, NULL);
}

void uart_switch_speed(int baudrate) { (void) baudrate; }

void uartWrite(const uint8_t *data, size_t len) {
    if (!usb_serial_jtag_is_connected()) return;
    usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(TX_WAIT_MS));
}

void uartTx(uint8_t data) { uartWrite(&data, 1); }

int uartRead(uint8_t *buf, int max) { return (int) xStreamBufferReceive(rxStream, buf, max, 0); }

void uartFlushInput() { xStreamBufferReset(rxStream); }

void uartWaitTxDone(uint32_t ms) {
    if (usb_serial_jtag_is_connected()) usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(ms));
}

void uart_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char buffer[128];
    const int len = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (len > 0) uartWrite((const uint8_t *) buffer, len < (int) sizeof(buffer) ? len : sizeof(buffer) - 1);
}

#endif
