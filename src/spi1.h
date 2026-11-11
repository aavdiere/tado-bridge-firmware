#ifndef SPI1_H
#define SPI1_H

#include <stdbool.h>
#include <stdint.h>

/* SPI1 bus shared by the CC110L radio (CS PA4) and the MX25R1635F flash
 * (CS PB9), docs/hardware.md. Master, mode 0, 8-bit, MSB first, polled.
 * By design the radio task owns the bus and runs flash accesses itself
 * (docs/design.md "Tasks"); the mutex below is a second line of defence. */

enum spi1_dev { SPI1_DEV_RADIO, SPI1_DEV_FLASH };

/* After board_init() (chip selects already high) and clock_init(); before
 * the scheduler starts (creates the bus mutex on the FreeRTOS heap). */
void     spi1_init(void);
uint32_t spi1_sck_hz(void);

/* Bus mutex. Hold it across every select..deselect. Panics after 100 ms. */
void spi1_lock(void);
void spi1_unlock(void);

/* Chip select. spi1_select panics if the other device is still selected.
 * spi1_deselect waits until the last byte has left the shifter (TXE, then
 * BSY clear, rm0383.txt:31740-31745) before raising CS. */
void spi1_select(enum spi1_dev dev);
void spi1_deselect(enum spi1_dev dev);

/* One full-duplex byte. Panics if SPI1 stops responding (bounded wait). */
uint8_t spi1_xfer(uint8_t out);

/* Current level of the MISO pin (PA6), read through GPIOA_IDR, which samples
 * the pin also in alternate-function mode (rm0383.txt:7857-7866). The CC110L
 * signals CHIP_RDYn on it after CSn goes low (cc110l.txt:1297-1299). */
bool spi1_miso_high(void);

#endif
