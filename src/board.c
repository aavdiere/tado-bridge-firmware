#include "board.h"

#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>

static const uint16_t led_pin[LED_COUNT] = {
    [LED_ROUTER] = GPIO0, [LED_POWER] = GPIO1, [LED_CLOUD] = GPIO5, [LED_PAIRING] = GPIO8};

#define RESET_BTN_PIN GPIO10 /* PB10, active low */
#define PAIR_BTN_PIN  GPIO15 /* PC15, active high */

/* GPIOA pins this firmware may configure. PA13/PA14 (SWD) must never appear in
 * any GPIOA mask (CLAUDE.md, docs/test-bench.md rule 5). libopencm3's
 * gpio_mode_setup/gpio_set_af/gpio_set_output_options read-modify-write only
 * the pins in the mask, so the SWD pins keep their reset configuration. */
#define GPIOA_ALLOWED (GPIO0 | GPIO1 | GPIO4 | GPIO5 | GPIO6 | GPIO7)
_Static_assert((GPIOA_ALLOWED & (GPIO13 | GPIO14)) == 0, "never touch PA13/PA14 (SWD)");
_Static_assert(((BOARD_RADIO_CS_PIN | BOARD_GDO0_PIN | BOARD_GDO2_PIN | BOARD_SPI1_SCK_PIN
                 | BOARD_SPI1_MISO_PIN | BOARD_SPI1_MOSI_PIN)
                & ~GPIOA_ALLOWED)
                   == 0,
               "GPIOA pin outside the allowed set");

void board_init(void) {
    /* #28: both SPI1 chip selects high (deselected) as push-pull outputs
     * before anything else and before SPI1 is enabled. Until now PA4 floated,
     * and the CC110L has no reset line. ODR is set through BSRR first, so the
     * pins go straight from input to output-high without a low glitch. */
    rcc_periph_clock_enable(RCC_GPIOA);
    rcc_periph_clock_enable(RCC_GPIOB);
    rcc_periph_clock_enable(RCC_GPIOC);
    gpio_set(GPIOA, BOARD_RADIO_CS_PIN);
    gpio_set(GPIOB, BOARD_FLASH_CS_PIN);
    gpio_set_output_options(GPIOA, GPIO_OTYPE_PP, GPIO_OSPEED_25MHZ, BOARD_RADIO_CS_PIN);
    gpio_set_output_options(GPIOB, GPIO_OTYPE_PP, GPIO_OSPEED_25MHZ, BOARD_FLASH_CS_PIN);
    gpio_mode_setup(GPIOA, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, BOARD_RADIO_CS_PIN);
    gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, BOARD_FLASH_CS_PIN);

    /* Radio GDO0 (PA0) and GDO2 (PA1): inputs. The CC110L drives both pins
     * (cc110l.txt:2885-2900); the pull-down only keeps them defined if the
     * chip is absent, so a dead radio cannot flood the EXTI. */
    gpio_mode_setup(GPIOA, GPIO_MODE_INPUT, GPIO_PUPD_PULLDOWN, BOARD_GDO0_PIN | BOARD_GDO2_PIN);

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
