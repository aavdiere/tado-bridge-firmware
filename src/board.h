#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>

/* LEDs: push-pull, high = on (docs/hardware.md). */
enum led { LED_ROUTER, LED_POWER, LED_CLOUD, LED_PAIRING, LED_COUNT };

/* Enables GPIOB and GPIOC clocks, configures the LED outputs (all off), the
 * factory reset button PB10 (active low, pull-up) and the pair button PC15
 * (active high, pull-down). No other pin is touched. */
void board_init(void);

/* Atomic (BSRR); safe from any task. */
void board_led(enum led led, bool on);

/* Raw (undebounced) logical state: true = pressed. */
bool board_reset_button_raw(void);
bool board_pair_button_raw(void);

#endif
