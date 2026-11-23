/* The radio task's internals (src/radio.h is its interface to the other
 * tasks): the state and helpers its parts share. Included by the radio
 * task's own sources only, never by another module. Everything here runs in
 * the radio task, unless it says it is also read or written by the GDO
 * interrupts. How the task works: the top of src/radio.c. */
#ifndef RADIO_INTERNAL_H
#define RADIO_INTERNAL_H

#include "freqoff.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- Task notification bits --------------------------------------------------- */
#define NOTE_GDO0  (1u << 0) /* end of a packet, received or sent */
#define NOTE_CRCOK (1u << 1) /* a packet arrived with a good CRC */
#define NOTE_FLASH (1u << 2)
#define NOTE_STORM (1u << 3)
#define NOTE_SYNC  (1u << 4)
#define NOTE_LINK  (1u << 5) /* the net task queued a message for a device */
/* The ACK interrupt could not strobe STX after making the chip IDLE (see
 * cc110l_ack_fire_from_isr()): CHECK, it strobed SRX again, confirm RX;
 * REARM, it could not, abort and re-arm now. */
#define NOTE_ACK_CHECK   (1u << 6)
#define NOTE_ACK_REARM   (1u << 7)
#define NOTE_PAIR        (1u << 8)  /* the pair button: open or close a window (#19) */
#define NOTE_RESET       (1u << 9)  /* the factory reset button (P6) */
#define NOTE_UPDATE      (1u << 10) /* the update task posted a flash operation (#24) */
#define NOTE_PAIR_REMOTE (1u << 11) /* a pair request of the server is posted (#40) */

/* The notifications the receiver acts on (rx_service()). */
#define RX_SERVICE_BITS (NOTE_GDO0 | NOTE_CRCOK | NOTE_SYNC | NOTE_ACK_CHECK | NOTE_ACK_REARM)

/* The radio task, set first thing when it starts; NULL before. */
extern TaskHandle_t radio_self;

/* Notification bits taken while a frame of ours is on the air or during
 * flash work, for the task loop to act on afterwards. */
extern uint32_t pending_bits;

/* --- Counters ------------------------------------------------------------------ */
struct counters {
    uint32_t frames, crc_ok, crc_bad, wake, overflow, bad_len, stale, rearm, reinit, spi_err;
    uint32_t flash_ok, flash_bad, flash_in_frame, status_glitch, storm;
    uint32_t fe_pkt, fe_none, fe_stale, fe_rssi_diff; /* #11, see fe_sample */
    uint32_t ack_prepared, ack_sent, ack_late, tx_frames, tx_fail, tx_refused;
    uint32_t tx_no_lock;        /* of tx_refused: no calibration with PLL lock */
    uint32_t tx_duty, ack_duty; /* refused: the hour's airtime budget is spent */
    uint32_t ack_rearm;         /* the ACK interrupt failed and the task re-armed RX */
    uint32_t lbt_rssi_busy;     /* listens found busy by RSSI */
    /* Enhanced Beacons (#19): sent, and not sent because the request's end
     * was not seen live, the channel was busy, it was too late, or the
     * driver refused or failed. */
    uint32_t beacon_sent, beacon_no_stamp, beacon_busy, beacon_late, beacon_fail;
    /* Flash work while frames arrive (#22): radio services run from inside
     * a flash program or erase, and the frames and Enhanced ACKs done in
     * them. */
    uint32_t flash_serve, flash_frames, flash_acks;
};
/* This minute's (cleared by the minute log) and since boot. */
extern struct counters radio_minute, radio_total;

#define COUNT(field)          \
    do {                      \
        radio_minute.field++; \
        radio_total.field++;  \
    } while (0)

/* --- Clock and formatting helpers --------------------------------------------- */
/* Microseconds since boot that do not wrap: TIM2's 32 bits extended. */
uint64_t now64(void);
/* x2 in half dB as text ("-72.5"); returns b. */
const char *half_db(char *b, size_t sz, int x2);
/* x10 in tenths as text ("-1.3"); returns b. */
const char *tenths(char *b, size_t sz, int32_t x10);
/* An RSSI register or status byte in half dB. */
int rssi_x2(uint8_t raw);

/* --- GDO interrupts --------------------------------------------------------------
 * Interrupt storm guard: more than STORM_MAX_EDGES edges within
 * STORM_WINDOW_US turn both GDO interrupts off (gdo_edge_from_isr()). */
#define STORM_WINDOW_US 1000u
#define STORM_MAX_EDGES 32u

/* The GDO interrupts are on; cleared by the storm guard in the interrupt. */
extern volatile bool gdo_irq_on;
/* Every GDO0 edge, both directions, counted by the interrupt. */
extern volatile uint32_t gdo0_edges;
/* Enhanced ACK attempts of the interrupt that failed after the chip had
 * been made IDLE. */
extern volatile uint32_t acks_failed;

void gdo_irq_setup(void);
void gdo_irq_enable(bool on);

/* --- Receive and Enhanced ACK, task side ----------------------------------------- */
#define PKTLEN_MAX 0x80u /* variable length: max length byte (cc110l.txt:3607-3612) */
#define RX_BUF_MAX (1u + PKTLEN_MAX + 2u) /* length byte, payload, RSSI, CRC_OK */

/* The frame being read out of the RX FIFO. */
struct radio_rx {
    uint8_t  buf[RX_BUF_MAX];
    unsigned got, total;
    bool     assembling;
    uint32_t start_us;
    /* A sync edge was seen and its frame is not complete yet: pump every
     * tick (see the top of src/radio.c). Given up after RX_STALE_US. */
    bool     in_packet;
    uint32_t sync_us;
    bool     ack_considered; /* ack_consider() has run for this frame */
    /* gdo0_edges before the RXBYTES read that brought this frame's length
     * byte: at frame_done() exactly one more edge (the frame's end) means
     * gdo0_fall_us is this frame's end (#19, beacon_answer()). */
    uint32_t edges_at_len;
};
extern struct radio_rx radio_rx;

/* FREQEST and RSSI sampled while a packet is on the air (#11; why and how:
 * above sample_in_packet()). */
struct fe_sample {
    bool     valid;
    uint8_t  fe, rssi;
    uint32_t edges; /* gdo0_edges when sampled (after this packet's sync) */
};
extern struct fe_sample fe_sample;

/* FREQEST of our own network's frames and the drift warning (#11,
 * src/freqoff.h): fed by frame_done() after the frame is handled, reported
 * by the minute log. */
extern struct freqoff radio_fo;

/* A packet is arriving: a frame being read, a sync edge without its end
 * yet, or GDO0 high. */
bool packet_arriving(void);
/* Drop the frame being read and any prepared ACK, then SIDLE, SFRX, SRX. */
bool rx_flush_and_arm(void);
/* The receiver's share of a task loop pass; bits: the notifications just
 * taken. */
void rx_service(uint32_t bits);
/* Write the log line of the wake-up train in progress, if any. */
void wake_flush(void);

/* --- Bring-up, calibration, health ------------------------------------------------ */
/* Last calibration attempt, and last calibration with lock, on now64(). */
extern uint64_t cal_at, cal_good_at;
/* Recalibration period (#11), with its policy in the bring-up. */
#define RECAL_PERIOD_MS 300000u

/* Wait for MARCSTATE want; false on a timeout, or at once on a TX state. */
bool wait_marcstate(uint8_t want, uint32_t timeout_us);
/* One fresh status byte, then act on the driver's TX-state flag; false if
 * the chip was forced IDLE and the caller must re-initialise. */
bool tx_watch(void);
/* Reset, load and verify the table, calibrate, enter RX. */
bool radio_configure(void);
/* Runtime re-init: check and force IDLE first, then the full bring-up. */
void radio_reinit(void);
/* Calibrate from IDLE and go back to RX, at a quiet moment. */
void radio_recalibrate(void);
/* The chip still holds our configuration and receives. */
void health_check(void);

/* --- The coordinator's radio (src/csl.h "Port"): src/radio_csl.c --------------- */
/* Listen before transmit by RSSI: the busy threshold (with its source at
 * csl_port_listen_begin()). */
#define LBT_RSSI_BUSY_DBM (-85)
/* A listen is running; this minute's highest listen RSSI, if any. */
extern bool lbt_listening, lbt_rssi_seen;
extern int  lbt_rssi_max_x2;
/* The backoff source and whether the DRBG has seeded it. */
extern uint32_t lbt_state;
extern bool     lbt_seeded;

/* One RSSI sample of the listen in progress. */
void lbt_sample_rssi(void);
/* One frame through the driver (cc110l_tx_start(), or cc110l_tx_answer()
 * for a frame that answers answered); mark: the pairing LED goes on right
 * after the STX strobe, and *stx_us (if not NULL) is the time just after
 * it. */
bool send_frame(const uint8_t *frame,
                size_t         len,
                const uint8_t *answered,
                unsigned       flags,
                bool           mark,
                uint32_t      *stx_us);

/* --- Pairing (#19, #40): src/radio_pair.c ---------------------------------------- */
/* Send the Enhanced Beacon that answers the target's Beacon Request just
 * taken, on time or not at all. */
void beacon_answer(uint32_t fall_us, bool own_end);
/* The pair button: open a window, or close the open one. */
void pairing_trigger(uint32_t now_ms);
/* The server's pair request, if one is posted. */
void pairing_remote(uint32_t now_ms);

/* --- Flash work and update operations (#10, #22, #24, P6) ----------------------- */
/* flash_boot() succeeded: the records may be read and written. */
extern bool flash_ready;
/* Bring-up done, the task loop runs. */
extern bool radio_running;

/* Wake the flash, wait for it, check its JEDEC ID; true if it may be used. */
bool flash_boot(void);
/* The JEDEC ID check requested by radio_request_flash_check(). */
void flash_check(void);
/* Erase the network record and counter log and restart (P6). */
void factory_reset(void);
/* Run the update operation posted by the update task (#24). */
void update_op_run(void);

#endif
