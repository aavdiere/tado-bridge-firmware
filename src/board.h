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

/* LEDs: push-pull, high = on (docs/hardware.md). */
enum led { LED_ROUTER, LED_POWER, LED_CLOUD, LED_PAIRING, LED_COUNT };

/* First: drives the radio CSn (PA4) and flash CS# (PB9) high as push-pull
 * outputs (#28), so neither device is selected before SPI1 runs. Then GDO0/GDO2
 * (PA0/PA1) as inputs, the LED outputs (all off), the factory reset button
 * PB10 (active low, pull-up) and the pair button PC15 (active high,
 * pull-down). SPI1's own pins are configured by spi1_init(). No other pin is
 * touched; never PA13/PA14 (SWD) or PB6/PB7 (HomeKit I2C). */
void board_init(void);

/* Atomic (BSRR); safe from any task. */
void board_led(enum led led, bool on);

/* Raw (undebounced) logical state: true = pressed. */
bool board_reset_button_raw(void);
bool board_pair_button_raw(void);

#endif
