#include "hci_uart.h"
#include "gd32_sdk.h"
#include <string.h>

/* Official GD32 USART1 / DMA0 channels 5 (RX) and 6 (TX). No RTS/CTS.
 * RX interrupts occur at DMA ring boundaries/errors and once per idle burst,
 * never per byte. Idle wakes foreground without parsing in the interrupt. */
static uint8_t rx_buffer[HCI_UART_RX_SIZE], tx_buffer[HCI_UART_TX_SIZE];
static volatile uint32_t rx_wraps;
static uint32_t consumed;
static bool tx_active;
static uint32_t last_idle_cycle;
volatile hci_uart_status_t hci_uart_status;
uint32_t hci_uart_cycles(void) { return DWT->CYCCNT; }

void USART1_IRQHandler(void) {
    uint32_t status=USART_STAT(USART1);
    if (!(status&USART_STAT_IDLEF)) return;
    /* GD32 idle/error flags clear through STAT then DATA reads, while DMA
     * retains its ring and transfer count. No byte-received interrupt. */
    (void)usart_data_receive(USART1);
    uint32_t cycle=DWT->CYCCNT;
    if (hci_uart_status.idle_wakes) {
        uint32_t gap=(cycle-last_idle_cycle)/48u;
        hci_uart_status.idle_intervals++;
        hci_uart_status.idle_total_us+=gap;
        if (gap>hci_uart_status.idle_max_us) hci_uart_status.idle_max_us=gap;
    }
    last_idle_cycle=cycle; hci_uart_status.idle_wakes++;
    if (status&(USART_STAT_ORERR|USART_STAT_FERR|USART_STAT_NERR|USART_STAT_PERR)) {
        hci_uart_status.uart_errors++; hci_uart_status.fault=true;
    }
}

static uint32_t producer(void) {
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    uint32_t first, second;
    FlagStatus pending;
    do {
        first = dma_transfer_number_get(DMA0, DMA_CH5);
        pending = dma_flag_get(DMA0, DMA_CH5, DMA_FLAG_FTF);
        second = dma_transfer_number_get(DMA0, DMA_CH5);
    } while (second > first); /* DMA reloaded during the snapshot */
    uint32_t total = (rx_wraps + (pending == SET ? 1u : 0u)) * HCI_UART_RX_SIZE +
                     ((HCI_UART_RX_SIZE - second) % HCI_UART_RX_SIZE);
    __set_PRIMASK(mask);
    return total;
}

void DMA0_Channel5_IRQHandler(void) {
    if (dma_interrupt_flag_get(DMA0, DMA_CH5, DMA_INT_FLAG_ERR)) {
        hci_uart_status.dma_errors++;
        hci_uart_status.fault = true;
        dma_channel_disable(DMA0, DMA_CH5);
        dma_interrupt_flag_clear(DMA0, DMA_CH5, DMA_INT_FLAG_ERR);
    }
    if (dma_interrupt_flag_get(DMA0, DMA_CH5, DMA_INT_FLAG_FTF)) {
        rx_wraps++;
        dma_interrupt_flag_clear(DMA0, DMA_CH5, DMA_INT_FLAG_FTF);
    }
    if (dma_interrupt_flag_get(DMA0, DMA_CH5, DMA_INT_FLAG_HTF))
        dma_interrupt_flag_clear(DMA0, DMA_CH5, DMA_INT_FLAG_HTF);
}

bool hci_uart_init(uint32_t baud) {
    if (baud != 115200 && baud != 1500000)
        return false;
    rcu_periph_clock_enable(RCU_GPIOA);
    rcu_periph_clock_enable(RCU_USART1);
    rcu_periph_clock_enable(RCU_DMA0);
    gpio_bit_set(GPIOA, GPIO_PIN_2);
    gpio_init(GPIOA, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, GPIO_PIN_2);
    gpio_init(GPIOA, GPIO_MODE_IN_FLOATING, GPIO_OSPEED_50MHZ, GPIO_PIN_3);
    NVIC_DisableIRQ(DMA0_Channel5_IRQn);
    NVIC_DisableIRQ(USART1_IRQn);
    usart_deinit(USART1);
    usart_baudrate_set(USART1, baud);
    usart_word_length_set(USART1, USART_WL_8BIT);
    usart_stop_bit_set(USART1, USART_STB_1BIT);
    usart_parity_config(USART1, USART_PM_NONE);
    usart_hardware_flow_rts_config(USART1, USART_RTS_DISABLE);
    usart_hardware_flow_cts_config(USART1, USART_CTS_DISABLE);
    usart_receive_config(USART1, USART_RECEIVE_ENABLE);
    usart_transmit_config(USART1, USART_TRANSMIT_ENABLE);
    dma_parameter_struct params;
    dma_struct_para_init(&params);
    params.periph_addr = USART1 + 4;
    params.periph_width = DMA_PERIPHERAL_WIDTH_8BIT;
    params.memory_addr = (uint32_t)rx_buffer;
    params.memory_width = DMA_MEMORY_WIDTH_8BIT;
    params.memory_inc = DMA_MEMORY_INCREASE_ENABLE;
    params.direction = DMA_PERIPHERAL_TO_MEMORY;
    params.number = HCI_UART_RX_SIZE;
    params.priority = DMA_PRIORITY_ULTRA_HIGH;
    dma_deinit(DMA0, DMA_CH5);
    dma_init(DMA0, DMA_CH5, &params);
    dma_circulation_enable(DMA0, DMA_CH5);
    dma_interrupt_enable(DMA0, DMA_CH5, DMA_INT_HTF | DMA_INT_FTF | DMA_INT_ERR);
    dma_deinit(DMA0, DMA_CH6);
    params.memory_addr = (uint32_t)tx_buffer;
    params.direction = DMA_MEMORY_TO_PERIPHERAL;
    params.number = 0;
    params.priority = DMA_PRIORITY_HIGH;
    dma_init(DMA0, DMA_CH6, &params);
    memset((void *)&hci_uart_status, 0, sizeof(hci_uart_status));
    rx_wraps = consumed = 0;
    tx_active = false;
    hci_uart_status.baud = baud;
    CoreDebug->DEMCR|=CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL|=DWT_CTRL_CYCCNTENA_Msk;
    NVIC_ClearPendingIRQ(DMA0_Channel5_IRQn);
    NVIC_SetPriority(DMA0_Channel5_IRQn, 1);
    NVIC_EnableIRQ(DMA0_Channel5_IRQn);
    NVIC_ClearPendingIRQ(USART1_IRQn);
    NVIC_SetPriority(USART1_IRQn,1);
    usart_interrupt_enable(USART1,USART_INT_IDLE);
    NVIC_EnableIRQ(USART1_IRQn);
    dma_channel_enable(DMA0, DMA_CH5);
    usart_dma_receive_config(USART1, USART_DENR_ENABLE);
    usart_dma_transmit_config(USART1, USART_DENT_ENABLE);
    usart_enable(USART1);
    return true;
}
bool hci_uart_tx_idle(void) {
    if (!tx_active)
        return true;
    if (dma_flag_get(DMA0, DMA_CH6, DMA_FLAG_ERR)) {
        hci_uart_status.dma_errors++;
        hci_uart_status.fault = true;
        return false;
    }
    if (dma_transfer_number_get(DMA0, DMA_CH6) != 0 ||
        usart_flag_get(USART1, USART_FLAG_TC) == RESET)
        return false;
    dma_channel_disable(DMA0, DMA_CH6);
    tx_active = false;
    return true;
}
bool hci_uart_set_baud(uint32_t baud) {
    if ((baud != 115200 && baud != 1500000) || !hci_uart_tx_idle())
        return false;
    usart_disable(USART1);
    usart_baudrate_set(USART1, baud);
    usart_enable(USART1);
    hci_uart_status.baud = baud;
    return true;
}
bool hci_uart_send(const uint8_t *data, size_t length) {
    if (length == 0 || length > sizeof(tx_buffer) || hci_uart_status.fault || !hci_uart_tx_idle())
        return false;
    memcpy(tx_buffer, data, length);
    dma_channel_disable(DMA0, DMA_CH6);
    dma_flag_clear(DMA0, DMA_CH6, DMA_FLAG_G);
    dma_memory_address_config(DMA0, DMA_CH6, (uint32_t)tx_buffer);
    dma_transfer_number_config(DMA0, DMA_CH6, length);
    tx_active = true;
    hci_uart_status.tx_bytes += length;
    dma_channel_enable(DMA0, DMA_CH6);
    return true;
}
size_t hci_uart_read(uint8_t *data, size_t capacity) {
    if (hci_uart_status.fault || capacity == 0)
        return 0;
    uint32_t status = USART_STAT(USART1);
    if (status & (USART_STAT_ORERR | USART_STAT_FERR | USART_STAT_NERR | USART_STAT_PERR)) {
        (void)usart_data_receive(USART1);
        hci_uart_status.uart_errors++;
        hci_uart_status.fault = true;
        return 0;
    }
    uint32_t head = producer();
    uint32_t available = head - consumed;
    if (available>hci_uart_status.rx_high_water) hci_uart_status.rx_high_water=available;
    if (available > sizeof(rx_buffer)) {
        hci_uart_status.overruns++;
        hci_uart_status.fault = true;
        consumed = head;
        return 0;
    }
    size_t count = available < capacity ? available : capacity;
    for (size_t i = 0; i < count; i++)
        data[i] = rx_buffer[(consumed + i) % sizeof(rx_buffer)];
    if (producer() - consumed > sizeof(rx_buffer)) {
        hci_uart_status.overruns++;
        hci_uart_status.fault = true;
        return 0;
    }
    consumed += count;
    hci_uart_status.rx_bytes += count;
    return count;
}
