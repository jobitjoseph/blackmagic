/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2012  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/dma.h>
#include <libopencm3/cm3/cortex.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scs.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/usb/cdc.h>

#include "general.h"
#include "usbuart.h"
#include "usb.h"
#include "aux_serial.h"

#ifdef DMA_STREAM0
#define dma_channel_reset(dma, channel) dma_stream_reset(dma, channel)
#define dma_enable_channel(dma, channel) dma_enable_stream(dma, channel)
#define dma_disable_channel(dma, channel) dma_disable_stream(dma, channel)

#define DMA_PSIZE_8BIT DMA_SxCR_PSIZE_8BIT
#define DMA_MSIZE_8BIT DMA_SxCR_MSIZE_8BIT
#define DMA_PL_HIGH	DMA_SxCR_PL_HIGH
#define DMA_CGIF DMA_ISR_FLAGS
#else
#define DMA_PSIZE_8BIT DMA_CCR_PSIZE_8BIT
#define DMA_MSIZE_8BIT DMA_CCR_MSIZE_8BIT
#define DMA_PL_HIGH	DMA_CCR_PL_HIGH
#define DMA_CGIF DMA_IFCR_CGIF_BIT
#endif

/* TX double buffer */
static uint8_t buf_tx[TX_BUF_SIZE * 2];
/* Active buffer part idx */
static uint8_t buf_tx_act_idx;
/* Active buffer part used capacity */
static uint8_t buf_tx_act_sz;
/* TX transfer complete */
bool tx_trfr_cplt = true;
/* RX Fifo buffer with space for copy fn overrun */
static uint8_t buf_rx[RX_FIFO_SIZE + sizeof(uint64_t)];
/* RX Fifo out pointer, writes assumed to be atomic */
static uint8_t buf_rx_out;
/* RX usb transfer complete */
bool rx_usb_trfr_cplt = true;

#ifdef ENABLE_DEBUG
/* Debug Fifo buffer with space for copy fn overrun */
char usb_dbg_buf[RX_FIFO_SIZE + sizeof(uint64_t)];
/* Debug Fifo in pointer */
uint8_t usb_dbg_in;
/* Debug Fifo out pointer */
uint8_t usb_dbg_out;
#endif

/*
 * Update led state atomically respecting RX anb TX states.
 */
void usbuart_set_led_state(uint8_t ledn, bool state)
{
	CM_ATOMIC_CONTEXT();

	static uint8_t led_state = 0;

	if (state)
	{
		led_state |= ledn;
		gpio_set(LED_PORT_UART, LED_UART);
	}
	else
	{
		led_state &= ~ledn;
		if (!led_state)
			gpio_clear(LED_PORT_UART, LED_UART);
	}
}

void aux_serial_init(void)
{
	/* Enable clocks */
	rcc_periph_clock_enable(USBUSART_CLK);
	rcc_periph_clock_enable(USBUSART_DMA_CLK);

	/* Setup UART parameters */
	UART_PIN_SETUP();
	usart_set_baudrate(USBUSART, 38400);
	usart_set_databits(USBUSART, 8);
	usart_set_stopbits(USBUSART, USART_STOPBITS_1);
	usart_set_mode(USBUSART, USART_MODE_TX_RX);
	usart_set_parity(USBUSART, USART_PARITY_NONE);
	usart_set_flow_control(USBUSART, USART_FLOWCONTROL_NONE);
	USART_CR1(USBUSART) |= USART_CR1_IDLEIE;

	/* Setup USART TX DMA */
#if !defined(USBUSART_TDR) && defined(USBUSART_DR)
# define USBUSART_TDR USBUSART_DR
#elif !defined(USBUSART_TDR)
# define USBUSART_TDR USART_DR(USBUSART)
#endif
#if !defined(USBUSART_RDR) && defined(USBUSART_DR)
# define USBUSART_RDR USBUSART_DR
#elif !defined(USBUSART_RDR)
# define USBUSART_RDR USART_DR(USBUSART)
#endif
	dma_channel_reset(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN);
	dma_set_peripheral_address(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN, (uint32_t)&USBUSART_TDR);
	dma_enable_memory_increment_mode(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN);
	dma_set_peripheral_size(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN, DMA_PSIZE_8BIT);
	dma_set_memory_size(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN, DMA_MSIZE_8BIT);
	dma_set_priority(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN, DMA_PL_HIGH);
	dma_enable_transfer_complete_interrupt(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN);
#ifdef DMA_STREAM0
	dma_set_transfer_mode(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN, DMA_SxCR_DIR_MEM_TO_PERIPHERAL);
	dma_channel_select(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN, USBUSART_DMA_TRG);
	dma_set_dma_flow_control(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN);
	dma_enable_direct_mode(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN);
#else
	dma_set_read_from_memory(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN);
#endif

	/* Setup USART RX DMA */
	dma_channel_reset(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN);
	dma_set_peripheral_address(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN, (uint32_t)&USBUSART_RDR);
	dma_set_memory_address(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN, (uint32_t)buf_rx);
	dma_set_number_of_data(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN, RX_FIFO_SIZE);
	dma_enable_memory_increment_mode(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN);
	dma_enable_circular_mode(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN);
	dma_set_peripheral_size(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN, DMA_PSIZE_8BIT);
	dma_set_memory_size(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN, DMA_MSIZE_8BIT);
	dma_set_priority(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN, DMA_PL_HIGH);
	dma_enable_half_transfer_interrupt(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN);
	dma_enable_transfer_complete_interrupt(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN);
#ifdef DMA_STREAM0
	dma_set_transfer_mode(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN, DMA_SxCR_DIR_PERIPHERAL_TO_MEM);
	dma_channel_select(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN, USBUSART_DMA_TRG);
	dma_set_dma_flow_control(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN);
	dma_enable_direct_mode(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN);
#else
	dma_set_read_from_peripheral(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN);
#endif
	dma_enable_channel(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN);

	/* Enable interrupts */
	nvic_set_priority(USBUSART_IRQ, IRQ_PRI_USBUSART);
#if defined(USBUSART_DMA_RXTX_IRQ)
	nvic_set_priority(USBUSART_DMA_RXTX_IRQ, IRQ_PRI_USBUSART_DMA);
#else
	nvic_set_priority(USBUSART_DMA_TX_IRQ, IRQ_PRI_USBUSART_DMA);
	nvic_set_priority(USBUSART_DMA_RX_IRQ, IRQ_PRI_USBUSART_DMA);
#endif
	nvic_enable_irq(USBUSART_IRQ);
#if defined(USBUSART_DMA_RXTX_IRQ)
	nvic_enable_irq(USBUSART_DMA_RXTX_IRQ);
#else
	nvic_enable_irq(USBUSART_DMA_TX_IRQ);
	nvic_enable_irq(USBUSART_DMA_RX_IRQ);
#endif

	/* Finally enable the USART */
	usart_enable(USBUSART);
	usart_enable_tx_dma(USBUSART);
	usart_enable_rx_dma(USBUSART);
}

/*
 * Copy data from fifo into continuous buffer. Return copied length.
 */
static uint32_t copy_from_fifo(uint8_t *dst, const uint8_t *src, uint32_t start, uint32_t end, uint32_t len, uint32_t fifo_sz)
{
	uint32_t out_len = 0;
	for (uint32_t buf_out = start; buf_out != end && out_len < len; buf_out %= fifo_sz)
		dst[out_len++] = src[buf_out++];

	return out_len;
}

/*
 * Changes USBUSART TX buffer in which data is accumulated from USB.
 * Filled buffer is submitted to DMA for transfer.
 */
static void usbuart_change_dma_tx_buf(void)
{
	/* Select buffer for transmission */
	uint8_t *const tx_buf_ptr = &buf_tx[buf_tx_act_idx * TX_BUF_SIZE];

	/* Configure DMA */
	dma_set_memory_address(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN, (uint32_t)tx_buf_ptr);
	dma_set_number_of_data(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN, buf_tx_act_sz);
	dma_enable_channel(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN);

	/* Change active buffer */
	buf_tx_act_sz = 0;
	buf_tx_act_idx ^= 1;
}

#ifndef ENABLE_RTT
void usbuart_usb_out_cb(usbd_device *dev, uint8_t ep)
{
	(void)ep;

	usbd_ep_nak_set(dev, CDCACM_UART_ENDPOINT, 1);

	/* Read new packet directly into TX buffer */
	uint8_t *const tx_buf_ptr = &buf_tx[buf_tx_act_idx * TX_BUF_SIZE];
	const uint16_t len = usbd_ep_read_packet(dev, CDCACM_UART_ENDPOINT,
						tx_buf_ptr + buf_tx_act_sz, CDCACM_PACKET_SIZE);

#if defined(BLACKMAGIC)
	/* Don't bother if uart is disabled.
	 * This will be the case on mini while we're being debugged.
	 */
	if(!(RCC_APB2ENR & RCC_APB2ENR_USART1EN) &&
	   !(RCC_APB1ENR & RCC_APB1ENR_USART2EN))
	{
		usbd_ep_nak_set(dev, CDCACM_UART_ENDPOINT, 0);
		return;
	}
#endif

	if (len)
	{
		buf_tx_act_sz += len;

		/* If DMA is idle, schedule new transfer */
		if (tx_trfr_cplt)
		{
			tx_trfr_cplt = false;
			usbuart_change_dma_tx_buf();

			/* Enable LED */
			usbuart_set_led_state(TX_LED_ACT, true);
		}
	}

	/* Enable USBUART TX packet reception if buffer has enough space */
	if (TX_BUF_SIZE - buf_tx_act_sz >= CDCACM_PACKET_SIZE)
		usbd_ep_nak_set(dev, CDCACM_UART_ENDPOINT, 0);
}
#endif

/*
 * Runs deferred processing for USBUSART RX, draining RX FIFO by sending
 * characters to host PC via CDCACM. Allowed to write to FIFO OUT pointer.
 */
void usbuart_send_rx_packet(void)
{
	rx_usb_trfr_cplt = false;
	/* Calculate writing position in the FIFO */
	const uint32_t buf_rx_in = (RX_FIFO_SIZE - dma_get_number_of_data(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN)) % RX_FIFO_SIZE;

	/* Forcibly empty fifo if no USB endpoint.
	 * If fifo empty, nothing further to do. */
	if (usb_get_config() != 1 || (buf_rx_in == buf_rx_out
#ifdef ENABLE_DEBUG
		&& usb_dbg_in == usb_dbg_out
#endif
	))
	{
#ifdef ENABLE_DEBUG
		usb_dbg_out = usb_dbg_in;
#endif
		buf_rx_out = buf_rx_in;
		/* Turn off LED */
		usbuart_set_led_state(RX_LED_ACT, false);
		rx_usb_trfr_cplt = true;
	}
	else
	{
		/* To avoid the need of sending ZLP don't transmit full packet.
		 * Also reserve space for copy function overrun.
		 */
		uint8_t packet_buf[CDCACM_PACKET_SIZE - 1 + sizeof(uint64_t)];
		uint32_t packet_size;

#ifdef ENABLE_DEBUG
		/* Copy data from DEBUG FIFO into local usb packet buffer */
		packet_size = copy_from_fifo(packet_buf, (uint8_t *)usb_dbg_buf, usb_dbg_out, usb_dbg_in, CDCACM_PACKET_SIZE - 1, RX_FIFO_SIZE);
		/* Send if buffer not empty */
		if (packet_size)
		{
			const uint16_t written = usbd_ep_write_packet(usbdev, CDCACM_UART_ENDPOINT, packet_buf, packet_size);
			usb_dbg_out = (usb_dbg_out + written) % RX_FIFO_SIZE;
			return;
		}
#endif

		/* Copy data from uart RX FIFO into local usb packet buffer */
		packet_size = copy_from_fifo(packet_buf, buf_rx, buf_rx_out, buf_rx_in, CDCACM_PACKET_SIZE - 1, RX_FIFO_SIZE);

		/* Advance fifo out pointer by amount written */
		const uint16_t written = usbd_ep_write_packet(usbdev, CDCACM_UART_ENDPOINT, packet_buf, packet_size);
		buf_rx_out = (buf_rx_out + written) % RX_FIFO_SIZE;
	}
}

void usbuart_usb_in_cb(usbd_device *dev, uint8_t ep)
{
	(void) ep;
	(void) dev;

	usbuart_send_rx_packet();
}

#if defined(USART_ICR)
#define USBUSART_ISR_TEMPLATE(USART, DMA_IRQ) do {			\
	nvic_disable_irq(DMA_IRQ);					\
									\
	/* Get IDLE flag and reset interrupt flags */ 			\
	const bool isIdle = usart_get_flag(USART, USART_FLAG_IDLE);	\
	usart_recv(USART);						\
									\
	/* If line is now idle, then transmit a packet */		\
	if (isIdle) {							\
		USART_ICR(USART) = USART_ICR_IDLECF;			\
		debug_uart_run();						\
	}								\
									\
	nvic_enable_irq(DMA_IRQ);					\
} while(0)
#else
#define USBUSART_ISR_TEMPLATE(USART, DMA_IRQ) do {			\
	nvic_disable_irq(DMA_IRQ);					\
									\
	/* Get IDLE flag and reset interrupt flags */ 			\
	const bool isIdle = usart_get_flag(USART, USART_FLAG_IDLE);	\
	usart_recv(USART);						\
									\
	/* If line is now idle, then transmit a packet */		\
	if (isIdle) {							\
		/* On the older uarts, the sequence "read flags", */	\
		/* "read DR" clears the flags                     */	\
									\
		debug_uart_run();						\
	}								\
									\
	nvic_enable_irq(DMA_IRQ);					\
} while(0)
#endif

#if defined(USBUSART_ISR)
void USBUSART_ISR(void)
{
#if defined(USBUSART_DMA_RXTX_IRQ)
	USBUSART_ISR_TEMPLATE(USBUSART, USBUSART_DMA_RXTX_IRQ);
#else
	USBUSART_ISR_TEMPLATE(USBUSART, USBUSART_DMA_RX_IRQ);
#endif
}
#endif

#if defined(USBUSART1_ISR)
void USBUSART1_ISR(void)
{
#if defined(USBUSART1_DMA_RXTX_IRQ)
	USBUSART_ISR_TEMPLATE(USBUSART1, USBUSART1_DMA_RXTX_IRQ);
#else
	USBUSART_ISR_TEMPLATE(USBUSART1, USBUSART1_DMA_RX_IRQ);
#endif
}
#endif

#if defined(USBUSART2_ISR)
void USBUSART2_ISR(void)
{
#if defined(USBUSART2_DMA_RXTX_IRQ)
	USBUSART_ISR_TEMPLATE(USBUSART2, USBUSART2_DMA_RXTX_IRQ);
#else
	USBUSART_ISR_TEMPLATE(USBUSART2, USBUSART2_DMA_RX_IRQ);
#endif
}
#endif

#define USBUSART_DMA_TX_ISR_TEMPLATE(DMA_TX_CHAN) do {				\
	nvic_disable_irq(USB_IRQ);						\
										\
	/* Stop DMA */								\
	dma_disable_channel(USBUSART_DMA_BUS, DMA_TX_CHAN);			\
	dma_clear_interrupt_flags(USBUSART_DMA_BUS, DMA_TX_CHAN, DMA_CGIF);	\
										\
	/* If new buffer is ready, continue transmission.			\
	 * Otherwise report transfer completion.				\
	 */									\
	if (buf_tx_act_sz)							\
	{									\
		usbuart_change_dma_tx_buf();					\
		usbd_ep_nak_set(usbdev, CDCACM_UART_ENDPOINT, 0);		\
	}									\
	else									\
	{									\
		usbuart_set_led_state(TX_LED_ACT, false);			\
		tx_trfr_cplt = true;						\
	}									\
										\
	nvic_enable_irq(USB_IRQ);						\
} while(0)

#if defined(USBUSART_DMA_TX_ISR)
void USBUSART_DMA_TX_ISR(void)
{
	USBUSART_DMA_TX_ISR_TEMPLATE(USBUSART_DMA_TX_CHAN);
}
#endif

#if defined(USBUSART1_DMA_TX_ISR)
void USBUSART1_DMA_TX_ISR(void)
{
	USBUSART_DMA_TX_ISR_TEMPLATE(USBUSART1_DMA_TX_CHAN);
}
#endif

#if defined(USBUSART2_DMA_TX_ISR)
void USBUSART2_DMA_TX_ISR(void)
{
	USBUSART_DMA_TX_ISR_TEMPLATE(USBUSART2_DMA_TX_CHAN);
}
#endif

#define USBUSART_DMA_RX_ISR_TEMPLATE(USART_IRQ, DMA_RX_CHAN) do {		\
	nvic_disable_irq(USART_IRQ);						\
										\
	/* Clear flags */							\
	dma_clear_interrupt_flags(USBUSART_DMA_BUS, DMA_RX_CHAN, DMA_CGIF);	\
	/* Transmit a packet */							\
	debug_uart_run();								\
										\
	nvic_enable_irq(USART_IRQ);						\
} while(0)

#if defined(USBUSART_DMA_RX_ISR)
void USBUSART_DMA_RX_ISR(void)
{
	USBUSART_DMA_RX_ISR_TEMPLATE(USBUSART_IRQ, USBUSART_DMA_RX_CHAN);
}
#endif

#if defined(USBUSART1_DMA_RX_ISR)
void USBUSART1_DMA_RX_ISR(void)
{
	USBUSART_DMA_RX_ISR_TEMPLATE(USBUSART1_IRQ, USBUSART1_DMA_RX_CHAN);
}
#endif

#if defined(USBUSART2_DMA_RX_ISR)
void USBUSART2_DMA_RX_ISR(void)
{
	USBUSART_DMA_RX_ISR_TEMPLATE(USBUSART2_IRQ, USBUSART2_DMA_RX_CHAN);
}
#endif

#if defined(USBUSART_DMA_RXTX_ISR)
void USBUSART_DMA_RXTX_ISR(void)
{
	if (dma_get_interrupt_flag(USBUSART_DMA_BUS, USBUSART_DMA_RX_CHAN, DMA_CGIF))
		USBUSART_DMA_RX_ISR();
	if (dma_get_interrupt_flag(USBUSART_DMA_BUS, USBUSART_DMA_TX_CHAN, DMA_CGIF))
		USBUSART_DMA_TX_ISR();
}
#endif
