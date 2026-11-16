#ifndef CC110L_H
#define CC110L_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "txgate.h"

/* TI CC110L register-level access over SPI1 (docs/datasheets/cc110l.txt,
 * SWRS109C). Every SPI access to the radio goes through one header check in
 * cc110l.c, which is the transmit guard. It refuses (fw_panic) every strobe
 * other than SRES, SCAL, SRX, SIDLE, SFRX and SNOP, every TX FIFO or PATABLE
 * access, and any MCSM1 value whose RXOFF_MODE or TXOFF_MODE could lead into
 * TX or FSTXON. STX, SFTX and the TX FIFO open only inside the transmit
 * functions below, for a frame the transmit gate (src/txgate.h) has
 * approved against the frequency registers last written to the chip. */

/* Master switch for the transmitter. 0: the image cannot transmit; the
 * transmit functions refuse, and STX, SFTX and the TX FIFO panic as in the
 * receive-only images. Set to 1 only with a fw-reviewer "safe to flash"
 * verdict for the transmit path (docs/test-bench.md rule 4, CLAUDE.md). */
#define RADIO_TX_ALLOWED 0

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

/* Our wire EUI-64, for the transmit gate. Without it nothing is sent. */
void cc110l_set_identity(const uint8_t own[8]);

/* Reads CHANNR, FSCTRL0, FREQ2-0, MDMCFG1 and MDMCFG0 back from the chip and
 * compares them with the values last written. On a difference, or without a
 * full register table written since the last reset, transmitting stays
 * refused until the radio is configured again. */
bool cc110l_tx_verify_channel(void);

enum cc110l_tx_result {
    CC_TX_OK,
    CC_TX_DISABLED, /* RADIO_TX_ALLOWED is 0 */
    CC_TX_REFUSED,  /* by the transmit gate: cc110l_tx_verdict() says why */
    CC_TX_SPI,      /* the chip did not answer */
};
enum txgate_verdict cc110l_tx_verdict(void);

/* Task context. Checks the frame (without the radio's CRC) against the
 * transmit gate, then: SIDLE, SFRX and SFTX in one access (the RX FIFO is
 * emptied: reset the receive assembler), the length byte and the first
 * bytes into the TX FIFO, STX. The chip returns to RX by itself after the
 * frame (MCSM1). flags: TXGATE_*. */
enum cc110l_tx_result cc110l_tx_start(const uint8_t *frame, size_t len, unsigned flags);

/* Tops the TX FIFO up for a frame longer than the FIFO. *left: bytes not
 * yet written. False on a TX FIFO underflow or an SPI failure; the caller
 * then re-arms the receiver. */
bool cc110l_tx_feed(size_t *left);

/* True from a transmission's STX until its airtime plus a margin has
 * passed: a TX state reported by the chip is then ours. Safe from any
 * context. */
bool cc110l_tx_expected(void);

/* Enhanced ACK, in two steps so that it can start well inside 1 ms after
 * the Data frame (docs/radio.md "ACK prebuild"):
 * - prepare, task context, while the Data frame is still arriving: the
 *   transmit gate checks that ack is the exact answer to answered (the
 *   frame's first 16 bytes), a unicast Data frame addressed to us;
 * - fire, from the GDO2 interrupt when the chip reports the frame complete
 *   with a good CRC: SIDLE, SFTX, the prepared bytes into the TX FIFO, STX.
 * cancel drops a prepared ACK. Each prepared ACK is sent at most once. */
enum cc110l_tx_result cc110l_ack_prepare(const uint8_t ack[29], const uint8_t answered[16]);
void                  cc110l_ack_cancel(void);
bool                  cc110l_ack_fire_from_isr(void);

#endif
