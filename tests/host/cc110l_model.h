/* A small CC110L behind the SPI1 functions, for the host tests of the radio
 * driver's transmit guard (#17). Included once by a test program, after
 * src/cc110l.c itself, so that the driver under test is the real one and
 * only the bus below it is simulated.
 *
 * The model keeps what matters for "can this firmware key the transmitter,
 * and with what": the configuration registers, the two FIFOs, the states
 * IDLE, RX, TX and TXFIFO_UNDERFLOW, and the strobes. Every STX is logged
 * with the carrier the chip's OWN registers give at that moment and the
 * bytes that then leave the TX FIFO. Datasheet behaviour the driver has to
 * respect is modelled as the datasheet states it (docs/datasheets/
 * cc110l.txt):
 *   - a strobe that follows SIDLE before IDLE is reached is dropped
 *     (:1449-1451); RX to IDLE is immediate, TX to IDLE takes a quarter bit,
 *     5 us at 50 kBaud (Table 5-11);
 *   - SFTX only in IDLE or TXFIFO_UNDERFLOW, SFRX only in IDLE (:3252-3257);
 *     in any other state the model counts a misuse;
 *   - only SFTX leaves TXFIFO_UNDERFLOW (:1944-1945);
 *   - after a frame the chip returns to RX (MCSM1 TXOFF_MODE, as the
 *     firmware's table sets it).
 * Not modelled: timing of the air interface (the test moves bytes out of
 * the TX FIFO itself), calibration, RX. */
#ifndef CC110L_MODEL_H
#define CC110L_MODEL_H

#include "log.h"
#include "mac.h"
#include "spi1.h"
#include "timebase.h"
#include "txgate.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum model_state { M_IDLE, M_RX, M_TX, M_UNDERFLOW };

#define MODEL_TX_TO_IDLE_US 5u
#define MODEL_MAX_STX       64u

struct model_stx {
    int64_t  carrier_hz;         /* from the chip's registers at the strobe */
    uint8_t  air[257]; /* length byte and frame, as they left the FIFO */
    unsigned air_n;
};

static struct {
    uint8_t          reg[0x2F];
    enum model_state st;
    uint8_t          txf[64], rxf[64];
    unsigned         txn, rxn;

    /* The SPI access in progress. */
    bool    cs_low, in_data, read, burst;
    uint8_t addr;

    uint32_t strobes_dropped_until; /* after SIDLE */
    bool     dropping;

    /* What the tests look at. */
    struct model_stx stx[MODEL_MAX_STX];
    unsigned         stx_count;
    unsigned         spi_bytes;
    unsigned         misuse;        /* SFTX/SFRX in a wrong state, FIFO overrun, xfer without CS */
    unsigned         forbidden;     /* SFSTXON, SXOFF, SPWD, PATABLE: never, in any build */
    unsigned         tx_fifo_writes; /* bytes written to the TX FIFO */
    unsigned         dropped;       /* strobes dropped after SIDLE */
    unsigned         frames_cut;    /* SIDLE while a frame was on the air */
    bool             locked;
} chip;

static uint32_t model_now_us;

/* Reset values of the registers that set the carrier: FREQ 0x1EC4EC
 * (800 MHz), CHANNR 0, MDMCFG1 0x22, MDMCFG0 0xF8, FSCTRL0 0
 * (cc110l.txt register tables). The rest is not looked at. */
static void model_chip_reset(void) {
    memset(chip.reg, 0, sizeof chip.reg);
    chip.reg[CC_FREQ2]   = 0x1E;
    chip.reg[CC_FREQ1]   = 0xC4;
    chip.reg[CC_FREQ0]   = 0xEC;
    chip.reg[CC_MDMCFG1] = 0x22;
    chip.reg[CC_MDMCFG0] = 0xF8;
    chip.st              = M_IDLE;
    chip.txn             = 0;
    chip.rxn             = 0;
    chip.dropping        = false;
}

/* A fresh chip and empty logs. The driver's own state is not touched. */
static void model_init(void) {
    memset(&chip, 0, sizeof chip);
    model_chip_reset();
}

static int64_t model_carrier_hz(void) {
    struct txgate_radio r = {
        .freq2   = chip.reg[CC_FREQ2],
        .freq1   = chip.reg[CC_FREQ1],
        .freq0   = chip.reg[CC_FREQ0],
        .channr  = chip.reg[CC_CHANNR],
        .mdmcfg1 = chip.reg[CC_MDMCFG1],
        .mdmcfg0 = chip.reg[CC_MDMCFG0],
        .fsctrl0 = chip.reg[CC_FSCTRL0],
    };
    return txgate_carrier_hz(&r);
}

/* Move up to n bytes from the TX FIFO onto the air. The frame is complete
 * after its length byte plus that many bytes, and the chip returns to RX;
 * an empty FIFO before that is an underflow. */
__attribute__((unused)) static void model_air(unsigned n) {
    if (chip.st != M_TX || chip.stx_count == 0 || chip.stx_count > MODEL_MAX_STX) {
        return;
    }
    struct model_stx *x = &chip.stx[chip.stx_count - 1u];
    for (; n; n--) {
        if (chip.txn == 0) {
            chip.st = M_UNDERFLOW;
            return;
        }
        x->air[x->air_n++] = chip.txf[0];
        memmove(chip.txf, chip.txf + 1, --chip.txn);
        if (x->air_n == 1u + x->air[0]) {
            chip.st = M_RX;
            return;
        }
    }
}

static void model_strobe(uint8_t s) {
    if (chip.dropping && (int32_t)(model_now_us - chip.strobes_dropped_until) < 0) {
        if (s != CC_SIDLE) {
            chip.dropped++;
            return;
        }
    }
    chip.dropping = false;
    switch (s) {
        case CC_SRES:
            model_chip_reset();
            break;
        case CC_SIDLE:
            if (chip.st == M_TX) {
                chip.frames_cut++;
                chip.st                    = M_IDLE;
                chip.dropping              = true;
                chip.strobes_dropped_until = model_now_us + MODEL_TX_TO_IDLE_US;
            } else if (chip.st == M_RX) {
                chip.st = M_IDLE;
            } else if (chip.st == M_UNDERFLOW) {
                /* No way to IDLE from here: what follows is dropped. */
                chip.dropping              = true;
                chip.strobes_dropped_until = model_now_us + MODEL_TX_TO_IDLE_US;
            }
            break;
        case CC_SFTX:
            if (chip.st == M_IDLE || chip.st == M_UNDERFLOW) {
                chip.txn = 0;
                chip.st  = M_IDLE;
            } else {
                chip.misuse++;
            }
            break;
        case CC_SFRX:
            if (chip.st == M_IDLE) {
                chip.rxn = 0;
            } else {
                chip.misuse++;
            }
            break;
        case CC_SRX:
            if (chip.st == M_IDLE) {
                chip.st = M_RX;
            }
            break;
        case CC_STX:
            if (chip.st == M_IDLE || chip.st == M_RX) {
                chip.st = M_TX;
                if (chip.stx_count < MODEL_MAX_STX) {
                    struct model_stx *x = &chip.stx[chip.stx_count];
                    x->carrier_hz       = model_carrier_hz();
                    x->air_n            = 0;
                }
                chip.stx_count++;
            }
            break;
        case CC_SCAL:
        case CC_SNOP:
            break;
        default: /* SFSTXON, SXOFF, SPWD, reserved */
            chip.forbidden++;
            break;
    }
}

static uint8_t model_status_byte(void) {
    static const uint8_t state[] = {[M_IDLE] = 0, [M_RX] = 1, [M_TX] = 2, [M_UNDERFLOW] = 7};
    return (uint8_t)(state[chip.st] << 4);
}

static uint8_t model_status_reg(uint8_t addr) {
    static const uint8_t marc[] = {
        [M_IDLE] = CC_MARC_IDLE, [M_RX] = CC_MARC_RX, [M_TX] = CC_MARC_TX, [M_UNDERFLOW] = CC_MARC_TXFIFO_UNF};
    switch (addr) {
        case CC_MARCSTATE:
            return marc[chip.st];
        case CC_TXBYTES:
            return (uint8_t)(chip.txn | (chip.st == M_UNDERFLOW ? 0x80u : 0u));
        case CC_RXBYTES:
            return (uint8_t)chip.rxn;
        case CC_VERSION:
            return 0x17;
        default:
            return 0;
    }
}

/* --- The SPI1 functions the driver calls -------------------------------------- */
void spi1_lock(void) {
    if (chip.locked) {
        chip.misuse++;
    }
    chip.locked = true;
}

void spi1_unlock(void) {
    chip.locked = false;
}

static void model_select(void) {
    if (chip.cs_low) {
        chip.misuse++;
    }
    chip.cs_low  = true;
    chip.in_data = false;
    model_now_us += 1u;
}

static void model_deselect(void) {
    chip.cs_low = false;
    model_now_us += 1u;
}

void spi1_select(enum spi1_dev dev) {
    (void)dev;
    model_select();
}

void spi1_deselect(enum spi1_dev dev) {
    (void)dev;
    model_deselect();
}

bool spi1_select_from_isr(enum spi1_dev dev) {
    (void)dev;
    if (chip.cs_low) {
        return false;
    }
    model_select();
    return true;
}

void spi1_deselect_from_isr(enum spi1_dev dev) {
    (void)dev;
    model_deselect();
}

bool spi1_miso_high(void) {
    model_now_us += 1u;
    return !chip.cs_low; /* CHIP_RDYn: low as soon as the chip is selected */
}

uint8_t spi1_xfer(uint8_t out) {
    model_now_us += 2u; /* 1.33 us per byte at 6 MHz, and the loop around it */
    chip.spi_bytes++;
    if (!chip.cs_low) {
        chip.misuse++;
        return 0xFF;
    }
    if (!chip.in_data) {
        uint8_t status = model_status_byte();
        chip.addr      = out & 0x3Fu;
        chip.read      = (out & 0x80u) != 0;
        chip.burst     = (out & 0x40u) != 0;
        if (chip.addr >= 0x30 && chip.addr <= 0x3D && !chip.burst) {
            model_strobe(chip.addr);
            return status; /* the next byte is a header again */
        }
        chip.in_data = true;
        return status;
    }
    uint8_t in = 0;
    if (chip.addr < 0x2F) {
        if (chip.read) {
            in = chip.reg[chip.addr];
        } else {
            chip.reg[chip.addr] = out;
        }
        if (chip.burst) {
            chip.addr = (uint8_t)(chip.addr + 1u < 0x2F ? chip.addr + 1u : chip.addr);
        } else {
            chip.in_data = false;
        }
    } else if (chip.addr >= 0x30 && chip.addr <= 0x3D) {
        in = model_status_reg(chip.addr); /* status register: read with the burst bit */
    } else if (chip.addr == 0x3F) {
        if (chip.read) {
            if (chip.rxn) {
                in = chip.rxf[0];
                memmove(chip.rxf, chip.rxf + 1, --chip.rxn);
            }
        } else {
            chip.tx_fifo_writes++;
            if (chip.txn < sizeof chip.txf) {
                chip.txf[chip.txn++] = out;
            } else {
                chip.misuse++;
            }
        }
        if (!chip.burst) {
            chip.in_data = false;
        }
    } else {
        chip.forbidden++; /* PATABLE, or 0x2F */
    }
    return in;
}

/* --- Clock, log and panic ------------------------------------------------------ */
uint32_t timebase_us(void) {
    return model_now_us;
}

void delay_us(uint32_t us) {
    model_now_us += us + 1u;
}

void log_printf(const char *fmt, ...) {
    (void)fmt;
}

/* fw_panic never returns: in a test it jumps back to the PANICS() that
 * expected it, and ends the program anywhere else. The driver panics before
 * it touches the bus, so its state stays usable. */
static jmp_buf model_panic_env;
static bool    model_panic_armed;

void fw_panic(const char *what, const char *file, int line) {
    if (model_panic_armed) {
        model_panic_armed = false;
        chip.cs_low       = false;
        chip.locked       = false;
        longjmp(model_panic_env, 1);
    }
    printf("unexpected fw_panic: %s at %s:%d\n", what, file, line);
    exit(1);
}

/* Runs stmt and sets panicked to whether it called fw_panic. */
#define PANICS(stmt, panicked)              \
    do {                                    \
        model_panic_armed = true;           \
        if (setjmp(model_panic_env) == 0) { \
            stmt;                           \
            model_panic_armed = false;      \
            panicked          = false;      \
        } else {                            \
            panicked = true;                \
        }                                   \
    } while (0)

#endif
