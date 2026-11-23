/* Radio task, receive: the GDO interrupts (the Enhanced ACK leaves from the
 * GDO2 interrupt), the RX FIFO pump, the Enhanced ACK's task side
 * (ack_consider()), complete frames (frame_done(), which hands a Beacon
 * Request of the pairing target to the beacon answer), the in-packet
 * FREQEST sample and the wake-up train summary. How it works and its
 * deadlines: the top of src/radio.c. Part of the radio task; the state it
 * shares, src/radio_internal.h. */
#include "radio.h"

#include "radio_internal.h"

#include "cc110l.h"
#include "csl.h"
#include "freqoff.h"
#include "link.h"
#include "log.h"
#include "mac.h"
#include "timebase.h"

#include "FreeRTOS.h"
#include "task.h"

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/exti.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "board.h"

/* RSSI_dBm = RSSI_dec(signed) / 2 - RSSI_offset, offset 74 dB at 868 MHz
 * (cc110l.txt:2130-2144). Kept in half dB. */
#define RSSI_OFFSET_DB 74
/* FREQEST resolution fXOSC / 2^14 = 1586.9 Hz = 203125 / 128 Hz
 * (cc110l.txt:4429-4434). */
#define FREQEST_HZ(fe) ((int32_t)(int8_t)(fe) * 203125 / 128)

/* --- Interrupts -------------------------------------------------------------- */
/* Notification bits: src/radio_internal.h. */

/* FreeRTOS FromISR calls need a numeric priority >= 5 (FreeRTOSConfig.h,
 * configMAX_SYSCALL_INTERRUPT_PRIORITY = 0x50). */
#define GDO_IRQ_PRIO (5u << 4)
_Static_assert(GDO_IRQ_PRIO >= configMAX_SYSCALL_INTERRUPT_PRIORITY, "FromISR needs prio >= 5");

static void notify_from_isr(uint32_t bits) {
    BaseType_t woken = pdFALSE;
    if (radio_self) {
        xTaskNotifyFromISR(radio_self, bits, eSetBits, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/* Interrupt storm guard. If the radio browns out or resets, IOCFG0 returns to
 * its reset value 0x3F, CLK_XOSC/192 = 135 kHz on GDO0 (cc110l.txt:2894-2896,
 * 3522-3528), and EXTI0 would fire every 3.7 us (both edges) and starve every
 * lower task. Real edges are far rarer: GDO0 rises and falls once per packet,
 * and the shortest packet (4 preamble + 2 sync + length + 1 + 2 CRC bytes at
 * 160 us per byte) takes 1.6 ms; GDO2 rises at most once per packet. So
 * more than STORM_MAX_EDGES edges of either pin within STORM_WINDOW_US means
 * the pins are not doing what the table says: both interrupts go off at once
 * and the radio task re-initialises, which enables them again only after the
 * table is loaded and verified. Both ISRs run at the same priority, so they never
 * preempt each other and share the counter safely. STORM_WINDOW_US and
 * STORM_MAX_EDGES: src/radio_internal.h. */
static uint32_t storm_win_start, storm_edges;
volatile bool   gdo_irq_on;

static void gdo_edge_from_isr(uint32_t note) {
    uint32_t now = timebase_us();
    if ((uint32_t)(now - storm_win_start) >= STORM_WINDOW_US) {
        storm_win_start = now;
        storm_edges     = 0;
    }
    if (++storm_edges > STORM_MAX_EDGES) {
        exti_disable_request(EXTI0 | EXTI1);
        gdo_irq_on = false;
        note |= NOTE_STORM;
    }
    notify_from_isr(note);
}

/* Every GDO0 edge, both directions. sample_in_packet() compares it before and
 * after its reads: unchanged means no packet started or ended in between. */
volatile uint32_t gdo0_edges;

/* The pin level after the edge tells the direction: high = sync word, low =
 * end of packet, discard or overflow. GDO0 stays high from sync to end of
 * packet, (len + 3) bytes at 160 us: at least 640 us for len = 1. A packet
 * dropped by the length filter (cc110l.txt:2917) de-asserts once its length
 * byte is in, about 160 us after sync. Either way far longer than the
 * interrupt latency of a few microseconds; a low level is not held long
 * enough to be misread either, since the next sync needs a preamble first. */
/* Time of the last falling GDO0 edge: the end of the last packet. */
static volatile uint32_t gdo0_fall_us;
/* Enhanced ACKs sent by the interrupt below, and attempts that failed after
 * the chip had been made IDLE. */
static volatile uint32_t acks_fired;
volatile uint32_t        acks_failed;

void exti0_isr(void) {
    exti_reset_request(EXTI0);
    gdo0_edges++;
    if (gpio_get(GPIOA, BOARD_GDO0_PIN)) {
        /* A new packet: an ACK still prepared answers an earlier one, which
         * ended without a good CRC. It must not go out after this one. */
        cc110l_ack_cancel();
        gdo_edge_from_isr(NOTE_SYNC);
    } else {
        gdo0_fall_us = timebase_us();
        gdo_edge_from_isr(NOTE_GDO0);
    }
}

/* A packet has arrived with a good CRC. If the task prepared the Enhanced
 * ACK for it, send it now: this is the ACK path, so no logging and nothing
 * that can block here. */
void exti1_isr(void) {
    exti_reset_request(EXTI1);
    uint32_t note = NOTE_CRCOK;
    switch (cc110l_ack_fire_from_isr()) {
        case CC_ACK_SENT:
            acks_fired++;
            break;
        case CC_ACK_FAILED:
            acks_failed++;
            note |= NOTE_ACK_CHECK;
            break;
        case CC_ACK_REARM:
            acks_failed++;
            note |= NOTE_ACK_REARM;
            break;
        case CC_ACK_NONE:
            break;
    }
    gdo_edge_from_isr(note);
}

void gdo_irq_enable(bool on) {
    /* EXTI_IMR is read-modify-written here and by the storm guard in the ISR:
     * mask the GDO interrupts (priority 5 = configMAX_SYSCALL) meanwhile. */
    taskENTER_CRITICAL();
    if (on) {
        exti_reset_request(EXTI0 | EXTI1);
        nvic_clear_pending_irq(NVIC_EXTI0_IRQ);
        nvic_clear_pending_irq(NVIC_EXTI1_IRQ);
        storm_edges = 0;
        gdo_irq_on  = true;
        exti_enable_request(EXTI0 | EXTI1);
    } else {
        /* After SRES GDO0 outputs a 135 kHz clock (cc110l.txt:2894-2896,
         * freedo .ino:143-146): keep the interrupt off until configured. */
        exti_disable_request(EXTI0 | EXTI1);
        gdo_irq_on = false;
    }
    taskEXIT_CRITICAL();
}

void gdo_irq_setup(void) {
    rcc_periph_clock_enable(RCC_SYSCFG); /* rm0383.txt:6122 */
    exti_disable_request(EXTI0 | EXTI1);
    exti_select_source(EXTI0 | EXTI1, GPIOA);   /* SYSCFG_EXTICR1, rm0383.txt:11032-11050 */
    exti_set_trigger(EXTI0, EXTI_TRIGGER_BOTH); /* RTSR + FTSR both set, rm0383.txt:11173-11174 */
    exti_set_trigger(EXTI1, EXTI_TRIGGER_RISING);
    nvic_set_priority(NVIC_EXTI0_IRQ, GDO_IRQ_PRIO);
    nvic_set_priority(NVIC_EXTI1_IRQ, GDO_IRQ_PRIO);
    nvic_enable_irq(NVIC_EXTI0_IRQ);
    nvic_enable_irq(NVIC_EXTI1_IRQ);
}

/* A recent 32-bit timestamp on that clock. */
static uint64_t stamp64(uint32_t stamp) {
    uint64_t n = now64();
    return n - (uint32_t)((uint32_t)n - stamp);
}

/* --- Formatting helpers ------------------------------------------------------- */
const char *half_db(char *b, size_t sz, int x2) {
    unsigned a = (unsigned)(x2 < 0 ? -x2 : x2);
    snprintf(b, sz, "%s%u.%u", x2 < 0 ? "-" : "", a / 2, (a & 1) ? 5u : 0u);
    return b;
}

int rssi_x2(uint8_t raw) {
    return (int)(int8_t)raw - 2 * RSSI_OFFSET_DB;
}

/* --- Wake-up train summary ----------------------------------------------------
 * Wake-up frames (802.15.4 multipurpose, frame type 5 in FCF bits 2:0,
 * docs/radio.md "Frames") come in trains of up to 254 frames 4 ms apart
 * (docs/radio.md "Coordinator behaviour"). One log line per train instead of
 * one per frame keeps the RTT log readable. */
#define WAKE_GAP_US 30000u

static struct {
    bool     active;
    uint8_t  seq;
    uint16_t n, n_ok, n_fe;
    int      rssi_min, rssi_max; /* half dB */
    int32_t  fe_sum;
    int      fe_min, fe_max;
    uint32_t last_us;
} wake;

/* Mean of sum / n in tenths, rounded half away from zero. */
static int32_t mean_x10(int32_t sum, unsigned n) {
    return (sum * 20 + (sum < 0 ? -(int32_t)n : (int32_t)n)) / (2 * (int32_t)n);
}

const char *tenths(char *b, size_t sz, int32_t x10) {
    unsigned a = (unsigned)(x10 < 0 ? -x10 : x10);
    snprintf(b, sz, "%s%u.%u", x10 < 0 ? "-" : "", a / 10, a % 10);
    return b;
}

void wake_flush(void) {
    if (!wake.active) {
        return;
    }
    char a[12], b[12], c[12];
    if (wake.n_fe) {
        int32_t fe_x10 = mean_x10(wake.fe_sum, wake.n_fe); /* |x10| <= 1280: no overflow below */
        LOG("rx: wake train seq %02x: %u frames (%u crc ok), rssi %s..%s dBm, freqest %u in pkt "
            "avg %s "
            "(%ld Hz) %d..%d",
            wake.seq,
            wake.n,
            wake.n_ok,
            half_db(a, sizeof a, wake.rssi_min),
            half_db(b, sizeof b, wake.rssi_max),
            wake.n_fe,
            tenths(c, sizeof c, fe_x10),
            (long)(fe_x10 * 203125 / 1280),
            wake.fe_min,
            wake.fe_max);
    } else {
        LOG("rx: wake train seq %02x: %u frames (%u crc ok), rssi %s..%s dBm, freqest none in pkt",
            wake.seq,
            wake.n,
            wake.n_ok,
            half_db(a, sizeof a, wake.rssi_min),
            half_db(b, sizeof b, wake.rssi_max));
    }
    wake.active = false;
}

/* The in-packet FREQEST sample as a signed unit count (-128..127), or
 * FE_NONE when the frame has none. */
#define FE_NONE 0x7FFF

static void wake_add(uint8_t seq, bool crc_ok, int rssi, int fe) {
    if (wake.active && seq != wake.seq) {
        wake_flush();
    }
    if (!wake.active) {
        wake.active   = true;
        wake.seq      = seq;
        wake.n        = 0;
        wake.n_ok     = 0;
        wake.n_fe     = 0;
        wake.rssi_min = rssi;
        wake.rssi_max = rssi;
        wake.fe_sum   = 0;
        wake.fe_min   = INT8_MAX;
        wake.fe_max   = INT8_MIN;
    }
    wake.n++;
    wake.n_ok += crc_ok;
    wake.rssi_min = rssi < wake.rssi_min ? rssi : wake.rssi_min;
    wake.rssi_max = rssi > wake.rssi_max ? rssi : wake.rssi_max;
    if (fe != FE_NONE) {
        wake.n_fe++;
        wake.fe_sum += fe;
        wake.fe_min = fe < wake.fe_min ? fe : wake.fe_min;
        wake.fe_max = fe > wake.fe_max ? fe : wake.fe_max;
    }
    wake.last_us = timebase_us();
}

/* --- Receive ----------------------------------------------------------------- */
#define RX_STALE_US 60000u /* longest frame 131 B x 160 us = 21 ms; freedo uses 60 ms */

struct radio_rx radio_rx; /* struct radio_rx: src/radio_internal.h */

bool packet_arriving(void) {
    return radio_rx.assembling || radio_rx.in_packet || gpio_get(GPIOA, BOARD_GDO0_PIN);
}

/* FREQEST and RSSI sampled while a packet is on the air (#11).
 *
 * Why: the P2 bench run read FREQEST after the last byte was in the FIFO and
 * got 0 or -1 on 452 of 453 frames from senders the SDR saw 5.1 kHz (about 3
 * units) apart. On the bench (2026-11-12, 300 s, 1706 frames [V]) the
 * in-packet sample differed by sender while a read after the packet, on the
 * same frames, still gave only 0 or -1. By then the packet has ended and,
 * with MCSM1.RXOFF_MODE = 11, the chip has gone through RX_END and RX_RST
 * back to RX (MARCSTATE 14, 15, 13: cc110l.txt:4481-4482). That restart
 * re-arms the demodulator: RSSI is frozen at sync "until the next time the
 * chip enters the RX state" (cc110l.txt:2113-2115) and needs its response
 * time again after a packet "even if the radio has never exited RX mode"
 * (cc110l.txt:2485-2489). The datasheet says nothing about when FREQEST is
 * held (cc110l.txt:1695-1698, 4429-4434); that the frequency offset loop
 * restarts there too is our inference [I] from the bench numbers.
 *
 * So the task samples on the GDO0 rising edge (sync word found, IOCFG0 0x06,
 * cc110l.txt:2916): the offset loop has run over the preamble with
 * FOC_PRE_K and keeps tracking at FOC_POST_K after sync (cc110l.txt:1687-
 * 1689; FOCCFG = 0x16: 3K before sync, K/2 after, limit BW/4 = +-33.9 kHz,
 * cc110l.txt:4013-4050). The sample counts only if GDO0 was high before and
 * after the reads and no GDO0 edge came in between, so it was taken inside
 * one packet. rx_pump() runs first in the same pass, so every packet that
 * ended earlier is already out of the FIFO and the sample belongs to the next
 * frame that completes. A flush drops it; the next sync replaces it.
 *
 * A packet the chip drops itself (length filter, cc110l.txt:2917) leaves its
 * sample behind, and if the task only runs after the next packet has ended,
 * the pump would complete that next frame before its own sync is handled. So
 * the sample keeps the GDO0 edge count it was taken at, and frame_done() uses
 * it only if at most 2 edges have come since: this packet's end, and at most
 * the next packet's sync. A frame completes once its status bytes are in the
 * FIFO, at its own end edge, so a sample from an earlier packet is at least 3
 * edges old (that packet's end, this packet's sync and end). This assumes
 * GDO0 falls no later than the chip writes the last status byte [I]; if it
 * fell a few cycles after, a pump pass landing exactly in that gap would see
 * only 2. A backlog of a whole further packet also gives 3 or more and drops
 * a good sample: conservative, counted as "stale".
 *
 * RSSI is read too: frozen at sync, it should equal the RSSI status byte the
 * chip appends to this packet (cc110l.txt:1744-1750). The datasheet does not
 * say the two are the same value [I], so a mismatch is only counted and
 * logged, not used to reject the sample. */
#define FE_SAMPLE_MAX_EDGES 2u

struct fe_sample fe_sample; /* struct fe_sample: src/radio_internal.h */

/* Status registers the radio updates by itself can read corrupt once in a
 * while (cc110l.txt:1436-1441): stable reads. At most 2 x 5 short SPI
 * transfers at the start of the packet; the pump already reads the FIFO
 * during packets, and the datasheet warns only against high-rate polling
 * (cc110l.txt:1993-1994). */
static void sample_in_packet(void) {
    fe_sample.valid = false;
    if (!gdo_irq_on) {
        return; /* no edge accounting without the interrupt */
    }
    uint32_t edges = gdo0_edges;
    if (!gpio_get(GPIOA, BOARD_GDO0_PIN)) {
        return; /* packet already over: the pump will report it without a sample */
    }
    uint8_t fe, rssi;
    if (!cc110l_read_status_stable(CC_FREQEST, &fe) || !cc110l_read_status_stable(CC_RSSI, &rssi)) {
        COUNT(spi_err);
        return;
    }
    if (!gpio_get(GPIOA, BOARD_GDO0_PIN) || gdo0_edges != edges) {
        return;
    }
    fe_sample.fe    = fe;
    fe_sample.rssi  = rssi;
    fe_sample.edges = edges;
    fe_sample.valid = true;
}

/* SIDLE, then SFRX once IDLE is reached (SFRX only in IDLE or overflow,
 * cc110l.txt:3252-3253, 1501; SIDLE drops strobes sent before IDLE,
 * cc110l.txt:1449-1451), then SRX. RX to IDLE takes ~0.1 us, IDLE to RX
 * without calibration 75 us (cc110l.txt:2538-2549). */
bool rx_flush_and_arm(void) {
    cc110l_ack_cancel();
    radio_rx.assembling = false;
    radio_rx.in_packet  = false;
    fe_sample.valid     = false;
    bool ok             = cc110l_strobe(CC_SIDLE, NULL) && wait_marcstate(CC_MARC_IDLE, 1000)
                          && cc110l_strobe(CC_SFRX, NULL) && cc110l_strobe(CC_SRX, NULL);
    if (!ok) {
        COUNT(spi_err);
    }
    return ok;
}

/* --- Enhanced ACK, task side ---------------------------------------------------
 * Runs as soon as the first 16 bytes of a frame are out of the FIFO, while
 * the rest still arrives. No logging here (ACK path). The ACK is prepared
 * only if all of this holds:
 *   - the link has its identity and key, and the transmitter is enabled;
 *   - the frame is a unicast Data frame addressed to our EUI-64
 *     (mac_ack_for_data; the driver's transmit gate checks it again);
 *   - the coordinator is not waiting for a device's ACK or sending a train
 *     (ours would cover it, freedo README.md:110-117);
 *   - the uplink queue has room: an uplink that cannot be stored gets no
 *     ACK, so the device sends it again (freedo .ino:1235-1248);
 *   - the chip's frequency registers still read back as written;
 *   - the GDO interrupts are on (the storm guard has not tripped);
 *   - this frame is still the packet on the air, so the next CRC-OK edge is
 *     this frame's and no other's.
 * The last point is what keeps an ACK from answering a frame that is not
 * addressed to us, so it does not rest on how fast this task runs:
 *   - arriving: the RXBYTES read that brought the header in showed the frame
 *     not complete in the chip, so at that moment it was on the air;
 *   - edges: the GDO0 edge count taken before that read. With the GDO
 *     interrupts masked, the ACK is handed to the driver only if no edge
 *     has come since and GDO0 is still high. Then the frame has not ended,
 *     and the only edges that can follow are its own end, with the CRC-OK
 *     edge that sends the ACK, or without one; in that case the next sync
 *     edge (exti0_isr) or frame_done() drops the ACK.
 * A frame that ends inside the critical section (about 10 us [I]) is
 * answered that much later. One that ended before it gets no ACK; the
 * device sends it again.
 * The CSL phase is 0, freedo's default: "safe while our receiver listens
 * continuously" (freedo internal/daemon/daemon.go:4258). */
#define ACK_CSL_PHASE 0u

static void ack_consider(const uint8_t *hdr, uint32_t edges, bool arriving) {
    uint8_t        ack[MAC_ACK_LEN];
    const uint8_t *own = link_own();
    if (!RADIO_TX_ALLOWED || !arriving || !gdo_irq_on || !cc110l_cal_ok() || !own || !link_ready()
        || !csl_may_ack() || !link_uplink_room() || !mac_ack_for_data(ack, hdr, own, ACK_CSL_PHASE)
        || !cc110l_tx_verify_channel()) {
        return;
    }
    taskENTER_CRITICAL();
    bool                  same = gdo0_edges == edges && gpio_get(GPIOA, BOARD_GDO0_PIN);
    enum cc110l_tx_result r    = same ? cc110l_ack_prepare(ack, hdr) : CC_TX_REFUSED;
    taskEXIT_CRITICAL();
    if (r == CC_TX_OK) {
        COUNT(ack_prepared);
    } else if (!same) {
        COUNT(ack_late);
    } else if (r == CC_TX_DUTY) {
        COUNT(ack_duty); /* no ACK: the device sends the frame again */
    }
}

static const char *const link_rx_name[LINK_RX_COUNT] = {
    [LINK_RX_BAD_CRC]       = "bad crc",
    [LINK_RX_ACK_TAKEN]     = "ack for our frame",
    [LINK_RX_ACK_OTHER]     = "ack, not ours",
    [LINK_RX_FORWARDED]     = "to server",
    [LINK_RX_HELD]          = "fragment held",
    [LINK_RX_DUPLICATE]     = "duplicate",
    [LINK_RX_NOT_RELAYED]   = "not relayed",
    [LINK_RX_DROPPED]       = "DROPPED, queue full",
    [LINK_RX_NOT_FOR_US]    = "not for us",
    [LINK_RX_NO_KEY]        = "no key",
    [LINK_RX_MIC_FAIL]      = "mic fail",
    [LINK_RX_BROADCAST]     = "broadcast",
    [LINK_RX_BEACON_REQ]    = "beacon request",
    [LINK_RX_BEACON_ANSWER] = "beacon request of the pairing target",
    [LINK_RX_PAIRING]       = "pairing target, pairing key",
    [LINK_RX_PAIRED]        = "PAIRED: the target took the network key",
    [LINK_RX_OTHER]         = "other",
};
_Static_assert(LINK_RX_OTHER + 1 == LINK_RX_COUNT, "a name for every link_rx");

/* FREQEST of our own network's frames and the drift warning (#11,
 * src/freqoff.h): fed by frame_done() after the frame is handled, reported
 * by log_minute(). Radio task only. */
struct freqoff radio_fo;

/* A complete frame is in radio_rx.buf. By now the GDO2 interrupt has sent the
 * Enhanced ACK if one was prepared, so this may take its time and log. */
static void frame_done(void) {
    static uint32_t acks_seen;
    /* The frame's end edge, taken together with the edge count (#19). */
    taskENTER_CRITICAL();
    uint32_t fall_us = gdo0_fall_us;
    bool     own_end = (uint32_t)(gdo0_edges - radio_rx.edges_at_len) == 1u;
    taskEXIT_CRITICAL();
    unsigned       len      = radio_rx.buf[0];
    const uint8_t *mac      = &radio_rx.buf[1];
    uint8_t        rssi_raw = radio_rx.buf[1 + len];
    uint8_t        st2 =
        radio_rx.buf[2 + len]; /* bit 7 CRC_OK, bits 6:0 reserved (cc110l.txt:1747-1757) */
    bool    crc_ok    = (st2 & 0x80) != 0;
    int     rssi      = rssi_x2(rssi_raw);
    int     fe        = FE_NONE;
    uint8_t sync_rssi = fe_sample.rssi;
    bool    rssi_diff = false;

    /* An ACK prepared for this frame and not sent (bad CRC, or prepared too
     * late) must not answer a later frame. */
    cc110l_ack_cancel();
    uint32_t fired     = acks_fired;
    bool     acked     = fired != acks_seen;
    acks_seen          = fired;
    radio_rx.in_packet = false;

    if (fe_sample.valid && (uint32_t)(gdo0_edges - fe_sample.edges) > FE_SAMPLE_MAX_EDGES) {
        fe_sample.valid = false; /* from an earlier packet, see fe_sample */
        COUNT(fe_stale);
    }
    if (fe_sample.valid) {
        fe        = (int8_t)fe_sample.fe;
        rssi_diff = sync_rssi != rssi_raw;
        COUNT(fe_pkt);
        if (rssi_diff) {
            COUNT(fe_rssi_diff);
        }
    } else {
        COUNT(fe_none);
    }
    fe_sample.valid = false;

    COUNT(frames);
    if (crc_ok) {
        COUNT(crc_ok); /* the cloud LED shows the server connection since P4 (src/session.c) */
    } else {
        COUNT(crc_bad);
    }
    if (acked) {
        COUNT(ack_sent);
    }

    /* The frame's end: the last falling GDO0 edge. If the pump is behind and
     * a later packet has ended since, the stamp is too late; the coordinator
     * bounds what it accepts (csl_anchor_store). */
    enum link_rx what =
        link_on_frame(mac, len, crc_ok, stamp64(fall_us), (uint32_t)xTaskGetTickCount());
    if (what == LINK_RX_BEACON_ANSWER) {
        beacon_answer(fall_us, own_end); /* before anything logs: it has a deadline */
    }
    /* After the ACK (sent from the CRC-OK interrupt before this runs) and the
     * beacon (answered just above): no deadline left for this frame (#11). */
    if (fe != FE_NONE && crc_ok && link_rx_from_network(what)) {
        freqoff_add(&radio_fo, fe);
    }

    if ((mac[0] & 0x07) == 5 && len >= 2) { /* wake-up: 25 <seq> CD AB ... */
        COUNT(wake);
        /* A bad-CRC frame's seq byte may be corrupt: count it in the open train. */
        wake_add((crc_ok || !wake.active) ? mac[1] : wake.seq, crc_ok, rssi, fe);
        return;
    }
    wake_flush();

    /* Header only: frame control and sequence number. No addresses or
     * payload (they identify the owner's devices). */
    char r[12], f[24], rs[20] = "";
    if (fe != FE_NONE) {
        snprintf(f, sizeof f, "%d (%ld Hz)", fe, (long)FREQEST_HZ(fe));
    } else {
        snprintf(f, sizeof f, "none");
    }
    if (rssi_diff) {
        char x[12];
        snprintf(rs, sizeof rs, " (sync rssi %s)", half_db(x, sizeof x, rssi_x2(sync_rssi)));
    }
    LOG("rx: t %lu len %u crc %s rssi %s dBm%s lqi(rsvd) %u freqest %s hdr %02x %02x %02x: %s%s",
        (unsigned long)xTaskGetTickCount(),
        len,
        crc_ok ? "ok" : "BAD",
        half_db(r, sizeof r, rssi),
        rs,
        st2 & 0x7F,
        f,
        mac[0],
        len > 1 ? mac[1] : 0,
        len > 2 ? mac[2] : 0,
        link_rx_name[what],
        acked ? ", acked" : "");
}

/* Drain the RX FIFO (radio-runtime.md "Receive", freedo .ino:772-816). Never
 * empties the FIFO while a packet is still arriving: a read of the last byte
 * as a new one is written duplicates it (cc110l.txt:2593-2597), so one byte
 * stays until the rest of the packet is in. */
static void rx_pump(void) {
    if (radio_rx.assembling && (uint32_t)(timebase_us() - radio_rx.start_us) > RX_STALE_US) {
        COUNT(stale);
        rx_flush_and_arm();
        return;
    }
    if (radio_rx.in_packet && (uint32_t)(timebase_us() - radio_rx.sync_us) > RX_STALE_US) {
        radio_rx.in_packet =
            false; /* a sync edge without a frame (length-filtered, or our own TX) */
    }
    for (int guard = 0; guard < 16; guard++) {
        uint32_t edges = gdo0_edges; /* before RXBYTES: see ack_consider() */
        uint8_t  rb;
        if (!cc110l_read_status_stable(CC_RXBYTES, &rb)) {
            COUNT(spi_err);
            return;
        }
        if (rb & CC_RXBYTES_OVERFLOW) {
            COUNT(overflow);
            rx_flush_and_arm();
            return;
        }
        unsigned n = rb & CC_RXBYTES_NUM;
        if (n > CC_FIFO_SIZE) {
            continue; /* a corrupt read (cc110l.txt:1436-1441); read again */
        }
        if (!radio_rx.assembling) {
            if (n < 2) {
                return;
            }
            if (!cc110l_read_rxfifo(radio_rx.buf, 1)) {
                COUNT(spi_err);
                return;
            }
            unsigned len = radio_rx.buf[0];
            if (len == 0 || len > PKTLEN_MAX) {
                COUNT(bad_len);
                rx_flush_and_arm();
                return;
            }
            radio_rx.got            = 1;
            radio_rx.total          = 1 + len + 2;
            radio_rx.assembling     = true;
            radio_rx.ack_considered = false;
            radio_rx.edges_at_len   = edges;
            radio_rx.start_us       = timebase_us();
            continue;
        }
        unsigned remaining = radio_rx.total - radio_rx.got;
        unsigned to_read   = 0;
        if (n >= remaining) {
            to_read = remaining;
        } else if (n > 1) {
            to_read = n - 1;
        }
        if (to_read == 0) {
            return;
        }
        if (!cc110l_read_rxfifo(radio_rx.buf + radio_rx.got, to_read)) {
            COUNT(spi_err);
            return;
        }
        radio_rx.got += to_read;
        if (!radio_rx.ack_considered && radio_rx.got >= 1u + MAC_HDR_LEN) {
            /* The header is in. n < remaining: when RXBYTES was read just
             * now, the chip had not received the whole frame yet, so the
             * packet on the air is this one. */
            radio_rx.ack_considered = true;
            ack_consider(radio_rx.buf + 1, edges, n < remaining);
        }
        if (radio_rx.got >= radio_rx.total) {
            radio_rx.assembling = false;
            frame_done(); /* FREQEST comes from the in-packet sample (#11) */
        }
    }
}

/* --- The receiver's share of a task loop pass -------------------------------
 * Run by the task loop, and from inside flash work (#22, src/radio_flash.c). bits: the
 * notifications just taken. */
void rx_service(uint32_t bits) {
    /* The ACK interrupt failed after it had made the chip IDLE: back to
     * RX now, not at the next health check. CHECK: the interrupt strobed
     * SRX and kept the RX FIFO, so the frame can still be read if the
     * chip is receiving; otherwise, and on REARM, abort (SIDLE, SFTX)
     * and flush and re-arm (SIDLE, SFRX, SRX), which drops the frame;
     * its device sends it again. */
    if (bits & (NOTE_ACK_CHECK | NOTE_ACK_REARM)) {
        if ((bits & NOTE_ACK_REARM) || !wait_marcstate(CC_MARC_RX, 1000)) {
            COUNT(ack_rearm);
            cc110l_tx_abort();
            if (!rx_flush_and_arm() || !wait_marcstate(CC_MARC_RX, 2000)) {
                radio_reinit();
            }
        }
    }

    if (bits & NOTE_SYNC) {
        radio_rx.in_packet = true;
        radio_rx.sync_us   = timebase_us();
    }
    rx_pump();
    if (bits & NOTE_SYNC) {
        sample_in_packet(); /* after the pump: see fe_sample */
    }
    if (wake.active && (uint32_t)(timebase_us() - wake.last_us) > WAKE_GAP_US) {
        wake_flush();
    }
}
