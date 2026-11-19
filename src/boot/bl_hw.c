/* Bootloader hardware (#24): clock, pins, SPI1 and the MX25R1635F by
 * polling, internal flash erase and program, IWDG, power LED, the jump.
 * Nothing here runs with interrupts: none are enabled in the bootloader.
 *
 * Clock: the HSI at 16 MHz as after reset (rm0383.txt:2073), no PLL, flash
 * at 0 wait states (the reset value, enough up to 30 MHz at 2.7-3.6 V,
 * rm0383.txt:2058-2068), ART caches off (reset value), so a read after an
 * erase or program never sees a stale cache line (rm0383.txt "Programming
 * and caches", 3.5.4).
 *
 * Hard limits, each checked before any write; a violation halts with LED
 * code 6 and writes nothing (bl_fatal()):
 *   - internal flash: erase sectors 4-7 only, program only inside
 *     0x08010000-0x0807FFFF; the bootloader's own sectors 0-3, the option
 *     bytes (FLASH_OPTCR, FLASH_OPTKEYR) and OTP are never written;
 *   - external flash: Page Program and Sector Erase only inside the boot
 *     state sectors 0x106000-0x107FFF; no other write command exists here;
 *   - pins: PA4-PA7, PB1 and PB9 only, by read-modify-write of their own
 *     bits; PA13/PA14 (SWD) and PB6/PB7 (HomeKit I2C) are never touched. */
#include "bl_hw.h"

#include "bootctl.h"
#include "flash_layout.h"
#include "fwimg.h"
#include "store.h"

#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/iwdg.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/spi.h>

#include <string.h>

/* --- Pins (docs/hardware.md; the same as src/board.h) ------------------------ */
#define PA_RADIO_CS 4u /* CC110L CSn, kept high so the radio never sees itself selected */
#define PA_SCK      5u
#define PA_MISO     6u
#define PA_MOSI     7u
#define PB_LED      1u /* power LED, active high */
#define PB_FLASH_CS 9u
#define PA_USED     ((1u << PA_RADIO_CS) | (1u << PA_SCK) | (1u << PA_MISO) | (1u << PA_MOSI))
#define PB_USED     ((1u << PB_LED) | (1u << PB_FLASH_CS))
_Static_assert((PA_USED & ((1u << 13) | (1u << 14))) == 0, "never touch PA13/PA14 (SWD)");
_Static_assert((PB_USED & ((1u << 6) | (1u << 7))) == 0, "never touch PB6/PB7 (HomeKit I2C)");

/* MODER/OSPEEDR/PUPDR field of pin p set to v (2 bits). */
static void field2(volatile uint32_t *reg, unsigned p, uint32_t v) {
    *reg = (*reg & ~(3u << (2u * p))) | (v << (2u * p));
}

/* --- Time: SysTick on the 16 MHz HSI, polled -------------------------------- */
#define HSI_HZ 16000000u
static uint32_t ms_count;

/* Milliseconds since bl_hw_init(), as long as it is called at least once a
 * millisecond (it only sees one wrap per call); slower callers just see
 * time pass more slowly, which only lengthens timeouts. */
static uint32_t ms_now(void) {
    if (STK_CSR & STK_CSR_COUNTFLAG) {
        ms_count++;
    }
    return ms_count;
}

/* Between ms - 1 and ms milliseconds (the first wrap may be close). */
static void delay_ms(uint32_t ms) {
    uint32_t t0 = ms_now();
    while (ms_now() - t0 < ms) {
    }
}

/* --- IWDG and LED --------------------------------------------------------- */
#define DBGMCU_APB1_FZ (*(volatile uint32_t *)0xE0042008u) /* rm0383.txt:46197 */
#define DBG_IWDG_STOP  (1u << 12)                          /* rm0383.txt:46240 */

/* A few LSI cycles per update; bounded (about 0.1 s at 16 MHz). */
static void sr_wait(uint32_t mask) {
    for (uint32_t n = 400000u; n && (IWDG_SR & mask); n--) {
    }
}

void bl_iwdg_start(void) {
    /* Frozen while the core is halted by the debugger, so SWD recovery is
     * never cut short by a reset. */
    DBGMCU_APB1_FZ |= DBG_IWDG_STOP;
    /* The same sequence as src/watchdog.c watchdog_start(): each update of
     * PR and RLR must finish (PVU, RVU) before the next write and before
     * the reload key, or the reset value is used (rm0383.txt:23254-23300). */
    IWDG_KR = IWDG_KR_START;
    IWDG_KR = IWDG_KR_UNLOCK;
    sr_wait(IWDG_SR_PVU);
    IWDG_PR = IWDG_PR_DIV256;
    sr_wait(IWDG_SR_PVU);
    IWDG_RLR = BL_IWDG_RELOAD;
    sr_wait(IWDG_SR_RVU);
    IWDG_KR = IWDG_KR_RESET;
}

static bool led_on;

static void led(bool on) {
    led_on     = on;
    GPIOB_BSRR = on ? (1u << PB_LED) : (1u << (PB_LED + 16u));
}

void bl_port_kick(void) {
    IWDG_KR = IWDG_KR_RESET;
    led(!led_on); /* flickers while the bootloader works on flash */
}

void fwimg_port_progress(void) {
    bl_port_kick();
}

/* N short flashes, then dark, every 2.5 s, as the application's fault codes
 * (docs/hardware.md "LED roles"); kicks the IWDG, so it stays here until a
 * power cycle or SWD. */
__attribute__((noreturn)) void bl_halt(unsigned code) {
    for (;;) {
        for (unsigned i = 0; i < code; i++) {
            led(true);
            for (unsigned t = 0; t < 150u; t++) {
                delay_ms(1);
                IWDG_KR = IWDG_KR_RESET;
            }
            led(false);
            for (unsigned t = 0; t < 250u; t++) {
                delay_ms(1);
                IWDG_KR = IWDG_KR_RESET;
            }
        }
        for (unsigned t = 400u * code; t < 2500u; t++) {
            delay_ms(1);
            IWDG_KR = IWDG_KR_RESET;
        }
    }
}

__attribute__((noreturn)) static void bl_fatal(void) {
    bl_halt(BL_LED_GUARD);
}

/* --- Init ----------------------------------------------------------------- */
void bl_hw_init(void) {
    RCC_AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN;
    RCC_APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC_APB2ENR; /* the enables complete before the first access */

    /* Chip selects high before they become outputs (as src/board.c
     * board_cs_early()); LED off. */
    GPIOA_BSRR = 1u << PA_RADIO_CS;
    GPIOB_BSRR = (1u << PB_FLASH_CS) | (1u << (PB_LED + 16u));
    GPIOA_OTYPER &= ~(uint32_t)(1u << PA_RADIO_CS | 1u << PA_SCK | 1u << PA_MOSI);
    GPIOB_OTYPER &= ~(uint32_t)PB_USED;
    field2(&GPIOA_MODER, PA_RADIO_CS, 1u);
    field2(&GPIOB_MODER, PB_FLASH_CS, 1u);
    field2(&GPIOB_MODER, PB_LED, 1u);

    /* SPI1 on AF5 (stm32f411ce.txt:2786-2799); MISO pulled up as in
     * src/spi1.c. */
    for (unsigned p = PA_SCK; p <= PA_MOSI; p++) {
        GPIOA_AFRL = (GPIOA_AFRL & ~(0xFu << (4u * p))) | (5u << (4u * p));
    }
    field2(&GPIOA_OSPEEDR, PA_SCK, 1u);
    field2(&GPIOA_OSPEEDR, PA_MOSI, 1u);
    field2(&GPIOA_PUPDR, PA_SCK, 0u);
    field2(&GPIOA_PUPDR, PA_MOSI, 0u);
    field2(&GPIOA_PUPDR, PA_MISO, 1u);
    field2(&GPIOA_MODER, PA_SCK, 2u);
    field2(&GPIOA_MODER, PA_MISO, 2u);
    field2(&GPIOA_MODER, PA_MOSI, 2u);

    /* Master, software NSS, mode 0, fPCLK/4 = 4 MHz (BR = 001), under the
     * CC110L's and the flash's limits (src/spi1.c). */
    SPI1_CR1 = 0;
    SPI1_CR2 = 0;
    SPI1_CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | (1u << 3);
    SPI1_CR1 |= SPI_CR1_SPE;

    STK_RVR = HSI_HZ / 1000u - 1u;
    STK_CVR = 0;
    STK_CSR = STK_CSR_CLKSOURCE_AHB | STK_CSR_ENABLE;
}

/* --- SPI1 and the MX25R1635F (as src/mx25r.c, without the radio) ---------- */
#define OP_READ   0x03u
#define OP_PP     0x02u
#define OP_SE     0x20u
#define OP_WREN   0x06u
#define OP_WRDI   0x04u
#define OP_RDSR   0x05u
#define OP_RDSCUR 0x2Bu
#define OP_RDID   0x9Fu

#define SR_WIP     0x01u /* mx25r1635f.txt:1347-1350 */
#define SR_WEL     0x02u /* :1352-1357 */
#define SCUR_PFAIL 0x20u /* :2521-2523 */
#define SCUR_EFAIL 0x40u /* :2525-2527 */

#define PP_TIMEOUT_MS 20u  /* 2 x tPP max, mx25r1635f.txt:3337 */
#define SE_TIMEOUT_MS 480u /* 2 x tSE max, :3340 */
#define SPIN          100000u

static bool bus_ok = true; /* false after any SPI timeout: the flash is not used again */

static uint8_t spi_byte(uint8_t out) {
    uint32_t n;
    for (n = SPIN; n && !(SPI1_SR & SPI_SR_TXE); n--) {
    }
    SPI1_DR = out;
    for (n = SPIN; n && !(SPI1_SR & SPI_SR_RXNE); n--) {
    }
    if (!n) {
        bus_ok = false;
        return 0xFFu;
    }
    return (uint8_t)SPI1_DR;
}

static void cs(bool low) {
    if (low) {
        GPIOB_BSRR = 1u << (PB_FLASH_CS + 16u);
    } else {
        for (uint32_t n = SPIN; n && (SPI1_SR & SPI_SR_BSY); n--) {
        }
        GPIOB_BSRR = 1u << PB_FLASH_CS;
    }
}

/* One transaction: opcode, optional 3-byte address, then n bytes out (src)
 * or in (dst). */
static void xfer(uint8_t op, const uint32_t *addr, const uint8_t *src, uint8_t *dst, size_t n) {
    cs(true);
    spi_byte(op);
    if (addr) {
        spi_byte((uint8_t)(*addr >> 16));
        spi_byte((uint8_t)(*addr >> 8));
        spi_byte((uint8_t)*addr);
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t in = spi_byte(src ? src[i] : 0xFFu);
        if (dst) {
            dst[i] = in;
        }
    }
    cs(false);
}

static uint8_t rdsr(void) {
    uint8_t sr = 0xFFu;
    xfer(OP_RDSR, NULL, NULL, &sr, 1);
    return sr;
}

static bool wait_wip(uint32_t timeout_ms, uint8_t *sr) {
    uint32_t t0 = ms_now();
    while (bus_ok && ((*sr = rdsr()) & SR_WIP)) {
        if (ms_now() - t0 > timeout_ms) {
            return false;
        }
    }
    return bus_ok;
}

bool bl_flash_init(void) {
    /* Deep power-down release by a CS# pulse (>= 20 ns), stand-by after
     * tRDP <= 45 us (mx25r1635f.txt:2426-2433, 3409); then any cycle left
     * from before an MCU reset ends (WIP), then the JEDEC ID. */
    cs(true);
    __asm__ volatile("nop\n nop\n nop\n nop");
    cs(false);
    delay_ms(2); /* at least 1 ms: delay_ms(n) waits between n - 1 and n ms */
    uint8_t sr;
    if (!wait_wip(SE_TIMEOUT_MS, &sr)) {
        return false;
    }
    uint8_t id[3] = {0};
    xfer(OP_RDID, NULL, NULL, id, 3);
    return bus_ok && id[0] == 0xC2u && id[1] == 0x28u && id[2] == 0x15u;
}

/* The bootloader's only external write range: the boot state. */
static void ext_guard(uint32_t addr, size_t n) {
    if (n == 0 || addr < FLASH_BOOT_A || addr >= FLASH_BOOT_B + FLASH_SECTOR
        || n > FLASH_BOOT_B + FLASH_SECTOR - addr) {
        bl_fatal();
    }
}

bool store_port_read(uint32_t addr, uint8_t *dst, size_t n) {
    if (!bus_ok || addr >= FLASH_SIZE || n > FLASH_SIZE - addr) {
        return false;
    }
    xfer(OP_READ, &addr, NULL, dst, n);
    return bus_ok;
}

static bool write_enable(void) {
    if (rdsr() & SR_WIP) {
        return false;
    }
    for (unsigned i = 0; i < 4u; i++) {
        xfer(OP_WREN, NULL, NULL, NULL, 0);
        if (rdsr() & SR_WEL) {
            return bus_ok;
        }
    }
    return false;
}

static bool cycle_ok(bool waited, uint8_t sr, uint8_t fail_bit) {
    if (!waited || (sr & (SR_WIP | SR_WEL))) {
        xfer(OP_WRDI, NULL, NULL, NULL, 0);
        return false;
    }
    uint8_t s = 0xFFu;
    xfer(OP_RDSCUR, NULL, NULL, &s, 1);
    return bus_ok && (s & fail_bit) == 0;
}

bool store_port_program(uint32_t addr, const uint8_t *src, size_t n) {
    ext_guard(addr, n);
    if (n > FLASH_PAGE || addr % FLASH_PAGE + n > FLASH_PAGE) {
        bl_fatal(); /* a PP past the page end wraps (mx25r1635f.txt:2310-2315) */
    }
    if (!bus_ok || !write_enable()) {
        return false;
    }
    xfer(OP_PP, &addr, src, NULL, n);
    uint8_t sr     = 0;
    bool    waited = wait_wip(PP_TIMEOUT_MS, &sr);
    return cycle_ok(waited, sr, SCUR_PFAIL);
}

bool store_port_erase(uint32_t addr) {
    ext_guard(addr, FLASH_SECTOR);
    if (addr % FLASH_SECTOR) {
        bl_fatal();
    }
    if (!bus_ok || !write_enable()) {
        return false;
    }
    xfer(OP_SE, &addr, NULL, NULL, 0);
    uint8_t sr     = 0;
    bool    waited = wait_wip(SE_TIMEOUT_MS, &sr);
    return cycle_ok(waited, sr, SCUR_EFAIL);
}

/* --- Internal flash (rm0383.txt 3.5, :2238-2380) ----------------------------- */
/* FLASH_SR error flags, all rc_w1 (rm0383.txt:2893-2930): RDERR 8, PGSERR 7,
 * PGPERR 6, PGAERR 5, WRPERR 4, OPERR 1; EOP 0. */
#define SR_ERRORS ((1u << 8) | (1u << 7) | (1u << 6) | (1u << 5) | (1u << 4) | (1u << 1))
#define SR_EOP    (1u << 0)
/* PSIZE x32: allowed at 2.7-3.6 V (rm0383.txt:2280-2291); the board runs at
 * 3.28 V (docs/test-bench.md) [V]. */
#define CR_PSIZE_X32 (FLASH_CR_PROGRAM_X32 << FLASH_CR_PROGRAM_SHIFT)
/* BSY polls: the CPU stalls on its own instruction fetches from flash while
 * an erase or program runs (rm0383.txt:2243-2246), so these loops mostly
 * wait inside a stalled fetch; the bound only catches a BSY stuck high. */
#define BSY_SPIN 50000000u

static bool bsy_wait(void) {
    for (uint32_t n = BSY_SPIN; n; n--) {
        if (!(FLASH_SR & FLASH_SR_BSY)) {
            return true;
        }
    }
    return false;
}

static void iflash_unlock(void) {
    if (FLASH_CR & FLASH_CR_LOCK) {
        FLASH_KEYR = FLASH_KEYR_KEY1;
        FLASH_KEYR = FLASH_KEYR_KEY2;
    }
}

/* Clear the request bits and lock again (LOCK is write-1 only). */
static void iflash_lock(void) {
    FLASH_CR = CR_PSIZE_X32;
    FLASH_CR = CR_PSIZE_X32 | FLASH_CR_LOCK;
}

bool bl_port_int_erase(unsigned sector) {
    if (sector < FWIMG_APP_FIRST_SECTOR || sector > FWIMG_APP_LAST_SECTOR) {
        bl_fatal();
    }
    if (!bsy_wait()) {
        return false;
    }
    FLASH_SR = SR_ERRORS | SR_EOP;
    iflash_unlock();
    FLASH_CR = CR_PSIZE_X32 | FLASH_CR_SER | (sector << FLASH_CR_SNB_SHIFT);
    FLASH_CR |= FLASH_CR_STRT;
    bool ok = bsy_wait() && !(FLASH_SR & SR_ERRORS);
    iflash_lock();
    return ok;
}

bool bl_port_int_program(uint32_t addr, const uint8_t *src, size_t n) {
    if (addr < FWIMG_APP_BASE || addr % 4u || n % 4u || n > FWIMG_APP_MAX
        || addr - FWIMG_APP_BASE > FWIMG_APP_MAX - n) {
        bl_fatal();
    }
    if (!bsy_wait()) {
        return false;
    }
    FLASH_SR = SR_ERRORS | SR_EOP;
    iflash_unlock();
    FLASH_CR = CR_PSIZE_X32 | FLASH_CR_PG;
    bool ok  = true;
    for (size_t i = 0; ok && i < n; i += 4u) {
        uint32_t w;
        memcpy(&w, src + i, 4u);
        *(volatile uint32_t *)(addr + i) = w;
        ok                               = bsy_wait() && !(FLASH_SR & SR_ERRORS);
    }
    iflash_lock();
    return ok;
}

void bl_port_int_read(uint32_t addr, uint8_t *dst, size_t n) {
    memcpy(dst, (const void *)addr, n);
}

/* --- The jump ------------------------------------------------------------- */
__attribute__((noreturn)) void bl_jump(void) {
    /* Leave what the application sets up itself as after a reset: SPI1
     * reset and unclocked, SysTick off, LED off. The chip selects stay
     * high outputs, which is what the application sets first
     * (board_cs_early()). Interrupts were never enabled. */
    led(false);
    SPI1_CR1 = 0;
    RCC_APB2RSTR |= RCC_APB2RSTR_SPI1RST;
    RCC_APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;
    RCC_APB2ENR &= ~RCC_APB2ENR_SPI1EN;
    STK_CSR = 0;
    STK_RVR = 0;
    STK_CVR = 0;

    const volatile uint32_t *v  = (const volatile uint32_t *)FWIMG_APP_BASE;
    uint32_t                 sp = v[0], pc = v[1];
    SCB_VTOR = FWIMG_APP_BASE;
    __asm__ volatile("dsb\n isb" ::: "memory");
    __asm__ volatile("msr control, %0\n"
                     "isb\n"
                     "msr msp, %1\n"
                     "bx %2\n" ::"r"(0u),
                     "r"(sp),
                     "r"(pc)
                     : "memory");
    __builtin_unreachable();
}
