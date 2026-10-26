#ifndef REDEV_HCI_UART_H
#define REDEV_HCI_UART_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define HCI_UART_RX_SIZE 2048u
#define HCI_UART_TX_SIZE 1029u
typedef struct {
    uint32_t baud, rx_bytes, tx_bytes, overruns, uart_errors, dma_errors;
    uint32_t idle_wakes, idle_intervals, idle_total_us, idle_max_us, rx_high_water;
    bool fault;
} hci_uart_status_t;
extern volatile hci_uart_status_t hci_uart_status;
bool hci_uart_init(uint32_t baud);
bool hci_uart_set_baud(uint32_t baud);
bool hci_uart_send(const uint8_t *data, size_t length);
bool hci_uart_tx_idle(void);
/* Free-running 48 MHz timestamp; reading it adds no interrupts. */
uint32_t hci_uart_cycles(void);
size_t hci_uart_read(uint8_t *data, size_t capacity);
void DMA0_Channel5_IRQHandler(void);
void USART1_IRQHandler(void);
#endif
