#ifndef CC110L_H
#define CC110L_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "txgate.h"

/* TI CC110L register-level access over SPI1 (docs/datasheets/cc110l.txt,
 * SWRS109C). Every SPI access to the radio goes through one header check in
 * cc110l.c, which is the transmit guard. It refuses (fw_panic) every strobe
 * other than SRES, SCAL, SRX, SIDLE, SFRX and SNOP, every TX FIFO access,
 * every PATABLE write other than cc110l_set_pa()'s, and any MCSM1 value
 * whose RXOFF_MODE or TXOFF_MODE could lead into TX or FSTXON. STX, SFTX
 * and the TX FIFO open only inside the transmit functions below, for a
 * frame the transmit gate (src/txgate.h) has approved against the
 * frequency registers last written to the chip. */

/* Master switch for the transmitter. 0: the image cannot transmit; the
 * transmit functions refuse, and STX, SFTX and the TX FIFO panic as in the
 * receive-only images. Set to 1 only with a fw-reviewer "safe to flash"
 * verdict for the transmit path (docs/test-bench.md rule 4, CLAUDE.md). */
#define RADIO_TX_ALLOWED 0

/* Output power for the bring-up on the bench (P5): PATABLE 0x26, -10 dBm
 * at 868 MHz with wire-wound inductors (cc110l.txt:2828, Table 5-14); with
 * multi-layer inductors -10 dBm is 0x27 (cc110l.txt:2862, Table 5-16). The
 * IB01's inductor type is not known, so the real level is within about a
 * PA step of -10 dBm, about 20 dB below the reset level 0xC6 that stock
 * keeps (docs/radio.md:53) and far below the band's 25 mW e.r.p. The bench
 * bridge sits next to the SDR. cc110l_set_pa() refuses any setting above
 * 0xC6's level. */
#define RADIO_PA_BRINGUP 0x26u

/* Configuration registers, cc110l.txt:3274-3373. */
#define CC_IOCFG2          0x00
#define CC_IOCFG1          0x01
#define CC_IOCFG0          0x02
#define CC_FIFOTHR         0x03
#define CC_SYNC1           0x04
#define CC_SYNC0           0x05
#define CC_PKTLEN          0x06
#define CC_PKTCTRL1        0x07
#define CC_PKTCTRL0        0x08
#define CC_ADDR            0x09
#define CC_CHANNR          0x0A
#define CC_FSCTRL1         0x0B
#define CC_FSCTRL0         0x0C
#define CC_FREQ2           0x0D
#define CC_FREQ1           0x0E
#define CC_FREQ0           0x0F
#define CC_MDMCFG4         0x10
#define CC_MDMCFG3         0x11
#define CC_MDMCFG2         0x12
#define CC_MDMCFG1         0x13
#define CC_MDMCFG0         0x14
#define CC_DEVIATN         0x15
#define CC_MCSM2           0x16
#define CC_MCSM1           0x17
#define CC_MCSM0           0x18
#define CC_FOCCFG          0x19
#define CC_BSCFG           0x1A
#define CC_AGCCTRL2        0x1B
#define CC_AGCCTRL1        0x1C
#define CC_AGCCTRL0        0x1D
#define CC_RSVD_20         0x20 /* "RESERVED", use the SmartRF value (cc110l.txt:4275-4280) */
#define CC_FREND1          0x21
#define CC_FREND0          0x22
#define CC_FSCAL3          0x23
#define CC_FSCAL2          0x24
#define CC_FSCAL1          0x25
#define CC_FSCAL0          0x26
#define CC_TEST2           0x2C
#define CC_TEST1           0x2D
#define CC_TEST0           0x2E
#define CC_NUM_CONFIG_REGS 0x2F /* 0x00..0x2E */

/* Command strobes, cc110l.txt:3225-3260. */
#define CC_SRES    0x30
#define CC_SFSTXON 0x31
#define CC_SXOFF   0x32
#define CC_SCAL    0x33
#define CC_SRX     0x34
#define CC_STX     0x35
#define CC_SIDLE   0x36
#define CC_SPWD    0x39
#define CC_SFRX    0x3A
#define CC_SFTX    0x3B
#define CC_SNOP    0x3D

/* Status registers (read with the burst bit), cc110l.txt:3376-3398. */
#define CC_PARTNUM   0x30
#define CC_VERSION   0x31
#define CC_FREQEST   0x32
#define CC_CRC_REG   0x33
#define CC_RSSI      0x34
#define CC_MARCSTATE 0x35
#define CC_PKTSTATUS 0x38
#define CC_TXBYTES   0x3A
#define CC_RXBYTES   0x3B

/* MARCSTATE values, cc110l.txt:4462-4495. */
#define CC_MARC_IDLE        0x01
#define CC_MARC_RX          0x0D
#define CC_MARC_RX_END      0x0E
#define CC_MARC_RX_RST      0x0F
#define CC_MARC_TXRX_SWITCH 0x10
#define CC_MARC_RXFIFO_OVF  0x11
#define CC_MARC_FSTXON      0x12
#define CC_MARC_TX          0x13
#define CC_MARC_TX_END      0x14
#define CC_MARC_RXTX_SWITCH 0x15
#define CC_MARC_TXFIFO_UNF  0x16

#define CC_RXBYTES_OVERFLOW 0x80 /* RXBYTES bit 7, cc110l.txt:4545-4548 */
#define CC_RXBYTES_NUM      0x7F
#define CC_FIFO_SIZE        64 /* cc110l.txt:2580-2582 */

/* Chip status byte STATE field (bits 6:4), cc110l.txt:1381-1405. */
#define CC_STATE(status)    (((status) >> 4) & 0x07)
#define CC_STATE_TX         2
#define CC_STATE_FSTXON     3
#define CC_STATE_TXFIFO_UNF 7

/* Manual reset: CSn strobe low/high (>= 40 us high), then SRES with a wait
 * for CHIP_RDYn before and after (cc110l.txt:2398-2408). Leaves the chip in
 * IDLE with reset defaults. False if SO never went low. */
bool cc110l_reset(void);

/* The only strobe entry point. Panics on a strobe outside the receive-only
 * whitelist. status may be NULL. */
bool radio_strobe(uint8_t strobe, uint8_t *status);

/* Configuration registers 0x00..0x2E. Burst writes are checked byte by byte
 * against the guard. */
bool cc110l_write_reg(uint8_t addr, uint8_t val);
bool cc110l_write_burst(uint8_t addr, const uint8_t *val, size_t n);
bool cc110l_read_reg(uint8_t addr, uint8_t *val);
bool cc110l_read_burst(uint8_t addr, uint8_t *val, size_t n);

/* Status register, one read. */
bool cc110l_read_status(uint8_t addr, uint8_t *val);
/* Status register read until two consecutive reads agree (errata, cc110l.txt:
 * 1436-1441, 1994-1997; freedo README.md:99-101). */
bool cc110l_read_status_stable(uint8_t addr, uint8_t *val);

/* Burst read of n bytes from the RX FIFO (header 0xFF, cc110l.txt:1491-1494). */
bool cc110l_read_rxfifo(uint8_t *dst, size_t n);

/* Set when a chip status byte reported TX, FSTXON or TXFIFO_UNDERFLOW
 * outside a transmission of our own (cc110l_tx_expected()). The radio task
 * confirms with MARCSTATE and stops the board if it is real. Cleared by the
 * call. */
bool cc110l_take_tx_state_flag(void);

/* --- Transmit (#17) ---------------------------------------------------------
 * The only code that can key the transmitter. */

/* Frequency synthesizer calibration (#11). MCSM0 FS_AUTOCAL = 0, so the
 * chip calibrates only on SCAL, from IDLE; the datasheet asks for it
 * "regularly" and recommends repeating it until the PLL locks
 * (cc110l.txt:2740-2761). With the chip in IDLE: up to 3 times SCAL, wait
 * for IDLE, read FSCAL3-1; FSCAL1 = 0x3F is no lock. On lock the carrier
 * registers are read back against the table. Task context, busy-waits at
 * most about 18 ms (3 tries of 0.75 ms plus at most 5 ms waiting for IDLE
 * each); about 0.8 ms per try, 2.4 ms for all three, when the chip answers
 * as documented.
 *   LOCKED:  the transmitter may be used, unless an earlier calibration
 *            since the last reset gave NO_LOCK (cc110l_cal_ok());
 *   NO_LOCK: 3 tries without lock. Latched until the next cc110l_reset():
 *            the transmitter stays refused for this bring-up. RX may run;
 *   ERROR:   the chip was not IDLE, did not answer, did not return to IDLE,
 *            or its carrier registers no longer read back. No lock either.
 * fscal: FSCAL3, FSCAL2, FSCAL1 of the last try, for the log. Every SCAL
 * strobe, configuration write and reset clears the lock until this
 * function has checked it again. */
enum cc110l_cal { CC_CAL_LOCKED, CC_CAL_NO_LOCK, CC_CAL_ERROR };
enum cc110l_cal cc110l_calibrate(uint8_t fscal[3], unsigned *tries);

/* A calibration older than this does not count: the radio task
 * recalibrates every 5 minutes (src/radio.c, RECAL_PERIOD_MS) and, with its
 * 64-bit clock, calls cc110l_cal_expire() past this age, which the 32-bit
 * check here cannot see after a timer wrap (71 minutes). */
#define CC_CAL_MAX_AGE_US (12u * 60u * 1000000u)
_Static_assert(CC_CAL_MAX_AGE_US < 0x80000000u, "age compared on the 32-bit microsecond clock");

/* The transmitter may be used as far as calibration goes: locked at the
 * last calibration, not latched off, younger than CC_CAL_MAX_AGE_US. */
bool cc110l_cal_ok(void);
void cc110l_cal_expire(void);

/* Output power: write PATABLE[0] and read it back. Only the settings
 * documented at or below the reset level 0xC6 (9.6 dBm) are accepted; any
 * other value returns false without touching the chip. Until a write has
 * read back since the last cc110l_reset(), and whenever a later read-back
 * differs, nothing is sent. Task context. */
bool cc110l_set_pa(uint8_t v);
/* PATABLE[0] still holds what cc110l_set_pa() wrote. Task context. */
bool cc110l_pa_reads_back(void);

/* Our wire EUI-64, for the transmit gate. Without it nothing is sent. */
void cc110l_set_identity(const uint8_t own[8]);

/* Reads CHANNR, FSCTRL1, FSCTRL0, FREQ2-0, MDMCFG1 and MDMCFG0 back from the
 * chip and compares them with the values last written. On a difference, or
 * without a full register table written since the last reset, transmitting
 * stays refused until the radio is configured again. Task context.
 * cc110l_tx_start() calls it for every frame. For an Enhanced ACK the
 * caller calls it before each cc110l_ack_prepare(): one read-back covers
 * one ACK, and the interrupt refuses to send on one older than 50 ms. */
bool cc110l_tx_verify_channel(void);

enum cc110l_tx_result {
    CC_TX_OK,
    CC_TX_DISABLED, /* RADIO_TX_ALLOWED is 0 */
    CC_TX_REFUSED,  /* by the transmit gate: cc110l_tx_verdict() says why */
    CC_TX_BUSY,     /* another frame of ours is still on the air; nothing was touched */
    CC_TX_NO_LOCK,  /* no valid calibration with PLL lock (cc110l_cal_ok()); not touched */
    CC_TX_DUTY,     /* the hour's airtime budget is spent (src/airtime.h); not touched */
    CC_TX_NO_PA,    /* no output power set and read back since the last reset; not touched */
    CC_TX_STATE,    /* the chip was found in a transmit state; abort and re-arm */
    CC_TX_SPI,      /* the chip did not answer; abort and re-arm */
};
enum txgate_verdict cc110l_tx_verdict(void);

/* Airtime charged in the last hour, at most AIRTIME_BUDGET_US (30 s).
 * Radio task only. */
uint32_t cc110l_airtime_used_us(void);

/* Task context. Drops a prepared Enhanced ACK, refuses while another frame
 * of ours is on the air, reads the frequency registers back, checks the
 * frame (without the radio's CRC) against the transmit gate, then: SIDLE,
 * SFRX and SFTX in one access (the RX FIFO is emptied: reset the receive
 * assembler), the length byte and the first bytes into the TX FIFO, STX.
 * The chip returns to RX by itself after the frame (MCSM1). flags:
 * TXGATE_*. */
enum cc110l_tx_result cc110l_tx_start(const uint8_t *frame, size_t len, unsigned flags);

/* The same for a frame that answers a received one, which the transmit
 * gate needs to see: the Enhanced Beacon answering a Beacon Request
 * (answered: the request's first 16 bytes; flags: TXGATE_PAIRING; #19).
 * Refused without answered. */
enum cc110l_tx_result
cc110l_tx_answer(const uint8_t *frame, size_t len, const uint8_t answered[16], unsigned flags);

/* Tops the TX FIFO up for a frame longer than the FIFO. *left: bytes not
 * yet written. False on a TX FIFO underflow or an SPI failure; the caller
 * then re-arms the receiver. */
bool cc110l_tx_feed(size_t *left);

/* Stops a transmission that went wrong and empties the TX FIFO. The chip
 * is left in IDLE; the caller re-arms the receiver. The driver's copy of
 * the frame is wiped (also by cc110l_tx_done()). */
void cc110l_tx_abort(void);

/* The caller has seen the frame started with cc110l_tx_start() leave the
 * chip (MARCSTATE out of TX and TX_END, TX FIFO empty): the next frame may
 * start, and a TX state the chip reports from now on is not ours. */
void cc110l_tx_done(void);

/* True from a transmission's STX until cc110l_tx_done(), or until its
 * airtime plus a 2 ms margin has passed: a TX state reported by the chip
 * is then ours. Safe from any context. */
bool cc110l_tx_expected(void);

/* Enhanced ACK, in two steps so that it can start well inside 1 ms after
 * the Data frame (docs/radio.md "ACK prebuild"):
 * - prepare, while the Data frame is still arriving: the transmit gate
 *   checks that ack is the exact answer to answered (the frame's first 16
 *   bytes), a unicast Data frame addressed to us. No SPI access and no
 *   waiting, so the caller can run it with the GDO interrupts masked, in
 *   the same critical section in which it checks that this frame is still
 *   the one on the air. Refused without a read-back of the frequency
 *   registers (cc110l_tx_verify_channel()) since the last frame or ACK;
 * - fire, from the GDO2 interrupt when the chip reports the frame complete
 *   with a good CRC: SIDLE, SFTX, the prepared bytes into the TX FIFO, STX.
 *   If the STX cannot be sent after the chip has been made IDLE, the
 *   interrupt puts it back in RX itself (CC_ACK_FAILED), or reports
 *   CC_ACK_REARM if it cannot.
 * cancel drops a prepared ACK. Each prepared ACK is sent at most once. */
enum cc110l_tx_result cc110l_ack_prepare(const uint8_t ack[29], const uint8_t answered[16]);
void                  cc110l_ack_cancel(void);

/* Result of the interrupt's attempt. */
enum cc110l_ack_fire {
    CC_ACK_NONE,   /* nothing prepared or allowed, or no byte reached the chip: still receiving */
    CC_ACK_SENT,   /* STX strobed */
    CC_ACK_FAILED, /* STX could not be sent; SRX strobed again: the task confirms RX */
    CC_ACK_REARM,  /* the chip may be IDLE: the radio task must abort and re-arm at once */
};
enum cc110l_ack_fire cc110l_ack_fire_from_isr(void);

#endif
