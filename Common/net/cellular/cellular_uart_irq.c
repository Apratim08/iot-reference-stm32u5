/*
 * Cellular UART Interrupt Handler
 *
 * This file provides the USART3 interrupt handler for the cellular modem.
 * USART3 is connected to Arduino D0/D1 pins (CN13) on B-U585I-IOT02A.
 * The weak handler in the startup file will be overridden by this implementation.
 */

#include "cellular_prv.h"

/* External function from cellular_uart.c */
extern void vCellularUartIrqHandler( void );

/**
 * @brief USART3 interrupt handler
 *
 * This interrupt handler is called by the NVIC when USART3 generates an interrupt.
 * It delegates to the cellular UART driver's IRQ handler.
 *
 * This function overrides the weak alias defined in startup_stm32u585aiixq.s
 *
 * Wiring: CN13 D0 (PD9/RX) ←→ SIM7600G TX (Pin 8)
 *         CN13 D1 (PD8/TX) ←→ SIM7600G RX (Pin 10)
 *         CN14 GND         ←→ SIM7600G GND (Pin 6)
 */
void USART3_IRQHandler( void )
{
    vCellularUartIrqHandler();
}
