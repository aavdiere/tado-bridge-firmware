#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>

/* Pin map (docs/hardware.md "Pin map so far"). libopencm3 GPIOn bit masks. */
#define BOARD_RADIO_CS_PIN  (1u << 4) /* PA4, CC110L CSn, active low */
#define BOARD_FLASH_CS_PIN  (1u << 9) /* PB9, MX25R1635F CS#, active low */
#define BOARD_GDO0_PIN      (1u << 0) /* PA0, CC110L GDO0 -> EXTI0 */
#define BOARD_GDO2_PIN      (1u << 1) /* PA1, CC110L GDO2 -> EXTI1 */
#define BOARD_SPI1_SCK_PIN  (1u << 5) /* PA5, SPI1_SCK AF5 */
#define BOARD_SPI1_MISO_PIN (1u << 6) /* PA6, SPI1_MISO AF5; also the CC110L SO / CHIP_RDYn */
#define BOARD_SPI1_MOSI_PIN (1u << 7) /* PA7, SPI1_MOSI AF5 */

/* ENC28J60 (#12), confirmed by continuity (docs/hardware.md). */
#define BOARD_ENC_CS_PIN    (1u << 12) /* PB12, ENC28J60 CS, active low */
#define BOARD_SPI2_SCK_PIN  (1u << 13) /* PB13, SPI2_SCK AF5 */
#define BOARD_SPI2_MISO_PIN (1u << 14) /* PB14, SPI2_MISO AF5 (ENC28J60 SO) */
#define BOARD_SPI2_MOSI_PIN (1u << 15) /* PB15, SPI2_MOSI AF5 (ENC28J60 SI) */
#define BOARD_ENC_INT_PIN   (1u << 2)  /* PB2, ENC28J60 INT, active low -> EXTI2 */
#define BOARD_ENC_RESET_PIN (1u << 14) /* PC14, ENC28J60 RESET, active low */

/* LEDs: push-pull, high = on (docs/hardware.md). */
enum led { LED_ROUTER, LED_POWER, LED_CLOUD, LED_PAIRING, LED_COUNT };

/* Called first in main(), before the 2 s debugger-attach delay (#28): drives
 * PA4 (radio CSn) and PB9 (flash CS#) high as push-pull outputs, so neither
 * device is selected while the MCU waits. Touches only the GPIOA and GPIOB
 * clock enables and, for those two pins only, BSRR, OTYPER and MODER. No
 * other pin moves before the delay; never PA13/PA14 (SWD) or PB6/PB7. */
void board_cs_early(void);

/* First: drives the radio CSn (PA4) and flash CS# (PB9) high as push-pull
 * outputs (#28), so neither device is selected before SPI1 runs. Then GDO0/GDO2
 * (PA0/PA1) as inputs, the LED outputs (all off), the factory reset button
 * PB10 (active low, pull-up) and the pair button PC15 (active high,
 * pull-down). ENC28J60 (#12): CS PB12 high push-pull output, INT PB2 input
 * with pull-up, RESET PC14 released (input with pull-up). SPI1's and SPI2's
 * own pins are configured by spi1_init() and enc28j60_setup(). No other pin is
 * touched; never PA13/PA14 (SWD) or PB6/PB7 (HomeKit I2C). */
void board_init(void);

/* Atomic (BSRR); safe from any task. */
void board_led(enum led led, bool on);

/* Raw (undebounced) logical state: true = pressed. */
bool board_reset_button_raw(void);
bool board_pair_button_raw(void);

#endif
