#include "board.h"

#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>

static const uint16_t led_pin[LED_COUNT] = {
    [LED_ROUTER] = GPIO0, [LED_POWER] = GPIO1, [LED_CLOUD] = GPIO5, [LED_PAIRING] = GPIO8};

#define RESET_BTN_PIN GPIO10 /* PB10, active low */
#define PAIR_BTN_PIN  GPIO15 /* PC15, active high */

void board_init(void) {
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_GPIOC);
    for (int i = 0; i < LED_COUNT; i++) {
        gpio_clear(GPIOB, led_pin[i]);
        gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, led_pin[i]);
        gpio_set_output_options(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, led_pin[i]);
    }
    gpio_mode_setup(GPIOB, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, RESET_BTN_PIN);
    gpio_mode_setup(GPIOC, GPIO_MODE_INPUT, GPIO_PUPD_PULLDOWN, PAIR_BTN_PIN);
}

void board_led(enum led led, bool on) {
    if (on) {
        gpio_set(GPIOB, led_pin[led]);
    } else {
        gpio_clear(GPIOB, led_pin[led]);
    }
}

bool board_reset_button_raw(void) {
    return gpio_get(GPIOB, RESET_BTN_PIN) == 0;
}

bool board_pair_button_raw(void) {
    return gpio_get(GPIOC, PAIR_BTN_PIN) != 0;
}
