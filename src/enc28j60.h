#ifndef ENC28J60_H
#define ENC28J60_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ENC28J60 10BASE-T MAC and PHY on SPI2 (#12). Datasheet:
 * docs/datasheets/enc28j60.txt (Microchip DS39662E). Pins (docs/hardware.md):
 * SPI2 SCK PB13, MISO PB14, MOSI PB15 (AF5), CS PB12, INT PB2 (active low,
 * EXTI2), RESET PC14 (active low).
 *
 * The net task owns SPI2 and is the only caller of everything here
 * (docs/design.md "Tasks"); nothing is locked. The chip is the only device on
 * SPI2. Half duplex only (docs/test-bench.md "Duplex"). */

#define ENC_ETH_HDR_LEN 14u   /* destination, source, type/length */
#define ENC_FRAME_MAX   1518u /* MAMXFL: whole frame including the 4-byte CRC */
#define ENC_TX_MAX      (ENC_FRAME_MAX - 4u) /* the MAC appends the CRC */
#define ENC_RX_MIN      64u   /* IEEE minimum incl. CRC; shorter frames are runts */

struct enc28j60_info {
    uint8_t  erevid;       /* silicon revision, EREVID<4:0> */
    uint16_t phid1, phid2; /* PHY identifier: 0x0083 and 0x1400 documented */
    uint16_t phlcon;       /* LED configuration, left at its reset value */
    uint16_t phcon1;       /* after configuration: PDPXMD = 0 (half duplex) */
    const char *rev_name;  /* B1, B4, B5, B7 (errata.txt:19-21) or "unknown" */
    bool     soft_reset;   /* the RESET pin did not reset the chip; SRC used */
};

/* SPI2 pins and peripheral, EXTI2 for INT (falling edge, NVIC priority 5,
 * masked until enc28j60_init() succeeds). Call once from the net task before
 * enc28j60_init(). Each INT edge sets `notify_bits` in `notify_task` through
 * xTaskNotifyFromISR(eSetBits); an edge storm (a dead or browned-out chip)
 * masks EXTI2 and sets `storm_bits` too, and the task must re-initialise. */
void     enc28j60_setup(void *notify_task, uint32_t notify_bits, uint32_t storm_bits);
uint32_t enc28j60_sck_hz(void);

/* Hardware reset through PC14, bus and identity check, then the whole
 * configuration with read-back, then reception on and the INT interrupt
 * unmasked. Every wait is bounded. Returns false (and logs why) on any
 * failure; the chip is then left with reception off and EXTI2 masked. Safe
 * to call again to re-initialise. */
bool enc28j60_init(const uint8_t mac[6], struct enc28j60_info *info);

/* Events gathered by enc28j60_poll(). */
struct enc28j60_events {
    bool    link_up;      /* PHSTAT2.LSTAT, read on every poll */
    bool    link_changed; /* differs from the previous poll */
    bool    link_irq;     /* EIR.LINKIF was set (also for a down-up between polls) */
    bool    rx_overflow;  /* RXERIF: a frame was dropped, buffer full or 255 pending */
    uint8_t tx_ok;        /* transmissions finished since the last poll, without abort */
    uint8_t tx_err;       /* ... aborted or cancelled after ENC_TX_WAIT_MS */
    uint8_t tx_retry;     /* late-collision retransmissions (errata #13) */
    uint8_t tx_estat;     /* ESTAT after the last finished one (TXABRT 02) */
    uint16_t tx_status;   /* its transmit status vector bits 16-31 (Table 7-1) */
    uint8_t pending;      /* EPKTCNT: frames waiting in the receive buffer */
    bool    bus_error;    /* EREVID changed, RXEN lost or MII stuck: re-initialise */
};

/* Start of service. Clears EIE.INTIE (the datasheet's servicing rule,
 * enc28j60.txt:3230-3241), reads and acknowledges EIR, the PHY link status
 * and EPKTCNT. Call enc28j60_rearm() when done, also after the frames are
 * read; a flag still set then gives a new falling edge on INT. */
void enc28j60_poll(struct enc28j60_events *ev);
void enc28j60_rearm(void);

/* Receive, in three steps so that #13 can read a frame into a pbuf chain:
 * rx_begin() opens the next frame and returns its length without the CRC
 * (0 if none is pending) and the 32-bit receive status vector (Table 7-3,
 * enc28j60.txt:2295-2325); rx_read() copies the next n bytes, any number of
 * times; rx_end() frees the frame in the chip, whether all of it was read or
 * not. Nothing else may move ERDPT between rx_begin() and rx_end().
 * rx_begin() returns ENC_RX_CORRUPT if the frame header is inconsistent; the
 * caller must then re-initialise the chip. */
#define ENC_RX_CORRUPT 0xFFFFu
#define ENC_RSV_RX_OK  (1u << 23) /* valid CRC, no symbol error */
#define ENC_RSV_CRC    (1u << 20)
#define ENC_RSV_BCAST  (1u << 25)
#define ENC_RSV_MCAST  (1u << 24)
uint16_t enc28j60_rx_begin(uint32_t *rsv);
void     enc28j60_rx_read(void *buf, size_t n);
void     enc28j60_rx_end(void);

/* Transmit one frame, in three steps for pbuf chains: tx_begin() waits
 * (bounded, at most ENC_TX_WAIT_MS) for the previous frame to leave the
 * single transmit buffer and returns false if it did not; tx_write() appends
 * bytes (destination MAC first; no preamble, no CRC); tx_end() starts the
 * transmission and returns at once. A frame longer than ENC_TX_MAX is
 * refused by tx_end(). The result arrives through enc28j60_poll(); a late
 * collision is retransmitted by the driver (errata #13). Do not call
 * tx_begin() or enc28j60_poll() between rx_begin() and rx_end(): both may
 * read the transmit status vector through ERDPT. */
#define ENC_TX_WAIT_MS 50u
bool enc28j60_tx_begin(void);
void enc28j60_tx_write(const void *buf, size_t n);
bool enc28j60_tx_end(void);

/* PHSTAT2 and the read-back checks of the health check. */
uint16_t enc28j60_phstat2(void);

#endif
