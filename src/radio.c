/* Radio task: receive, Enhanced ACKs, the CSL coordinator and the relay
 * queues (#17), plus the flash ID check on the shared SPI1 bus (#10). The
 * receive flow follows docs/radio-runtime.md (freedo's working receive
 * path, freedo/firmware/cc1101_bridge/cc1101_bridge.ino:772-816); the
 * protocol logic is in the hardware-independent modules src/mac.c,
 * src/lowpan.c, src/csl.c, src/relay.c and src/link.c, and everything that
 * can key the transmitter in src/cc110l.c behind the transmit gate.
 *
 * Interrupts, both at configMAX_SYSCALL_INTERRUPT_PRIORITY:
 *   GDO0 (PA0, EXTI0, both edges): IOCFG0 = 0x06 asserts on sync word and
 *     deasserts at the end of the packet, on a length-filtered packet, or when
 *     the RX FIFO overflows (cc110l.txt:2916-2918); in TX it asserts when the
 *     sync word has been sent and deasserts at the end of the packet. The
 *     rising edge wakes the task to sample FREQEST while the packet is still
 *     arriving (#11, see sample_in_packet()) and drops a prepared ACK that
 *     was not sent (it belonged to an earlier packet). The falling edge is
 *     timestamped: the end of the frame, for the CSL anchors.
 *   GDO2 (PA1, EXTI1, rising edge): IOCFG2 = 0x07 asserts when a packet has
 *     been received with CRC OK (cc110l.txt:2919). If an Enhanced ACK is
 *     prepared, the interrupt sends it (cc110l_ack_fire_from_isr()).
 * No interrupt announces a filling RX FIFO: from the sync edge to the end of the frame the task
 * pumps the FIFO every tick (1 ms, 6 bytes of a 64-byte FIFO), and every 100 ms otherwise.
 *
 * ENHANCED ACK (docs/radio.md "ACK prebuild"; target: starts within 1 ms of
 * the end of the Data frame, also while the net task holds the CPU for a
 * P-256 step). Two contexts:
 *   1. This task, the highest-priority one, while the Data frame is still
 *      arriving: once its first 16 bytes are out of the FIFO, ack_consider()
 *      builds the ACK, has the chip's frequency registers read back, and
 *      hands the ACK to the driver, which checks it against the transmit
 *      gate. The hand-over is tied to this frame: it happens with the GDO
 *      interrupts masked and only if no GDO0 edge has come since the frame
 *      was seen arriving. The shortest Data frame (one datagram byte, 31
 *      bytes) leaves 17 bytes, 2.7 ms, for this after its header [I].
 *   2. The GDO2 interrupt, at the frame's CRC-OK edge: three strobes and 30
 *      FIFO bytes, about 70 us to the STX strobe, then 75 us IDLE to TX in
 *      the chip [I]. It preempts every task and is masked only by critical
 *      sections (one SPI1 access at most, about 0.1 ms).
 * Nothing between the sync edge and the ACK logs: LOG() takes a mutex. The
 * frame's own log line is written when the frame is complete, after the
 * interrupt has sent the ACK; periodic log lines wait while a packet is
 * arriving.
 *
 * TX-state watch. A single bit error on MOSI can turn any header the guard in
 * cc110l.c approved into a transmit strobe at the chip (SRX 0x34 -> STX 0x35,
 * MARCSTATE read 0xF5 -> 0xB5 = STX, a CSn bounce mid-burst that makes the
 * TEST1 data byte 0x35 a header). The chip would then send preamble until
 * stopped. Every status byte the chip returns is checked for STATE TX,
 * FSTXON or TXFIFO_UNDERFLOW outside the airtime of a frame we started
 * (cc110l.c), and tx_watch() takes one fresh status byte and acts on that
 * flag. It runs after every radio step of the bring-up, on every bring-up
 * failure, every 100 ms of the retry wait, and at the end of every pass of
 * the task loop, so a TX state that is not ours is stopped within
 * microseconds of the next radio access and at most one loop pass (<= 100 ms
 * when idle, with no other radio access in between) after the access that
 * caused it.
 *
 * PAIRING (#19, src/pair.h). The pair button (held 1 s, src/app.c) asks this
 * task to open a window for the device named in the build
 * (PAIR_TARGET_EUI64 in config/local.cmake): at a quiet moment it creates
 * the network first if there has never been one, then opens the window.
 * The Enhanced Beacon that answers the target's Beacon Request is built
 * while the request is taken (link_on_frame()) and sent from frame_done(),
 * before anything logs, PAIR_BEACON_TX_AT_US after the request's end edge
 * (see beacon_answer()). The pairing LED (PB8, owner spec 2026-11-17)
 * flashes while a window is open and waits for the target, is lit while an
 * exchange with the target goes on (from a beacon sent to it until its
 * 2.04, a failure or no progress, src/pair.h), and is off once the window
 * has closed. It is also the scope marker of the beacon's STX strobe: off
 * while the beacon waits for its moment, on right after the strobe. */
#include "radio.h"

#include "airtime.h"
#include "cc110l.h"
#include "csl.h"
#include "fwupd.h"
#include "handoff.h"
#include "identity.h"
#include "link.h"
#include "log.h"
#include "mac.h"
#include "mx25r.h"
#include "network.h"
#include "pair.h"
#include "pair_led.h"
#include "provision.h"
#include "rng.h"
#include "spi1.h"
#include "timebase.h"
#include "watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

#include "mbedtls/platform_util.h"

#include <libopencm3/cm3/nvic.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/exti.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board.h"

/* Also refused at run time by the transmit gate, whatever these say. */
_Static_assert(RADIO_CHANNEL != RADIO_PROTECTED_CHANNEL,
               "the bridge's channel is the protected one");
_Static_assert(RADIO_CHANNEL == TXGATE_TEST_CHANNEL, "transmit only on the test channel for now");

/* --- Register table ---------------------------------------------------------
 * The stock IB01 burst write 0x00..0x2E from the logic capture
 * (docs/radio.md "Stock CC110L register table", wiki
 * CC110-Register-Configuration.md:43-95), with four changes:
 *   IOCFG2  0x04 -> 0x07  GDO2 = packet received with CRC OK (see above)
 *   IOCFG0  0x01 -> 0x06  GDO0 = sync .. end of packet (see above)
 *   CHANNR  0x00/0x1A -> RADIO_CHANNEL (stock ends up on 26 = 0x1A)
 *   FSCTRL1 0x18 -> 0x06  IF 152 kHz, owner decision (docs/radio.md Conflicts)
 * IOCFG1 stays 0x2E: GDO1 is the SO pin and must be 3-state while CSn is
 * high, because the flash shares MISO (cc110l.txt:2891-2893, 2901). */
#define PKTLEN_MAX   0x80u /* variable length: max length byte (cc110l.txt:3607-3612) */
#define MCSM1_VAL    0x3Fu /* RXOFF_MODE 11 stay in RX, TXOFF_MODE 11 RX, CCA_MODE 11 */
#define PKTCTRL0_VAL 0x05u

static const uint8_t cc_config[CC_NUM_CONFIG_REGS] = {
    [CC_IOCFG2]   = 0x07,
    [CC_IOCFG1]   = 0x2E,
    [CC_IOCFG0]   = 0x06,
    [CC_FIFOTHR]  = 0x03,
    [CC_SYNC1]    = 0xD3,
    [CC_SYNC0]    = 0x91,
    [CC_PKTLEN]   = PKTLEN_MAX,
    [CC_PKTCTRL1] = 0x04,         /* APPEND_STATUS, no address check, no autoflush */
    [CC_PKTCTRL0] = PKTCTRL0_VAL, /* FIFO mode, CRC on, variable length, no whitening */
    [CC_ADDR]     = 0x00,
    [CC_CHANNR]   = RADIO_CHANNEL,
    [CC_FSCTRL1]  = 0x06,
    [CC_FSCTRL0]  = 0x00,
    [CC_FREQ2]    = 0x21,
    [CC_FREQ1]    = 0x32,
    [CC_FREQ0]    = 0x76,
    [CC_MDMCFG4]  = 0xAA,
    [CC_MDMCFG3]  = 0xF8,
    [CC_MDMCFG2]  = 0x13,
    [CC_MDMCFG1]  = 0x22,
    [CC_MDMCFG0]  = 0xF8,
    [CC_DEVIATN]  = 0x40,
    [CC_MCSM2]    = 0x07,
    [CC_MCSM1]    = MCSM1_VAL,
    [CC_MCSM0]    = 0x08,
    [CC_FOCCFG]   = 0x16,
    [CC_BSCFG]    = 0x6C,
    [CC_AGCCTRL2] = 0x43,
    [CC_AGCCTRL1] = 0x40,
    [CC_AGCCTRL0] = 0x91,
    [0x1E]        = 0x87,
    [0x1F]        = 0x6B, /* "Not used" (cc110l.txt:3335); stock's dummy bytes */
    [CC_RSVD_20]  = 0xFB,
    [CC_FREND1]   = 0x56,
    [CC_FREND0]   = 0x10,
    [CC_FSCAL3]   = 0xE9,
    [CC_FSCAL2]   = 0x2A,
    [CC_FSCAL1]   = 0x00,
    [CC_FSCAL0]   = 0x1F,
    [0x27]        = 0x41,
    [0x28]        = 0x00, /* "Not used" (cc110l.txt:3369) */
    [0x29]        = 0x59,
    [0x2A]        = 0x7F,
    [0x2B]        = 0x3F, /* reserved, = reset values */
    [CC_TEST2]    = 0x81,
    [CC_TEST1]    = 0x35,
    [CC_TEST0]    = 0x09,
};

/* MCSM1 RXOFF_MODE = 11 (stay in RX after a packet) and TXOFF_MODE = 11
 * (back to RX after a frame of ours, cc110l.txt:3929-3955): the chip enters
 * TX only on an STX strobe. The runtime guard in cc110l.c refuses any MCSM1
 * that leads to TX or FSTXON by itself; this catches a bad table at build
 * time. CCA_MODE = 11 has no effect here: every frame is sent from IDLE. */
_Static_assert(((MCSM1_VAL >> 2) & 3) == 3 && (MCSM1_VAL & 3) == 3, "MCSM1 must stay in RX");
_Static_assert(((PKTCTRL0_VAL >> 4) & 3) == 0, "PKTCTRL0 FIFO mode (no serial or random TX mode)");
_Static_assert(RADIO_CHANNEL <= 49, "channel plan 0-49 (docs/radio.md)");

/* Frequency plan checks, all from the table above with fXOSC = 26 MHz:
 *   carrier = fXOSC / 2^16 x FREQ (cc110l.txt:3729-3736)
 *   spacing = fXOSC / 2^18 x (256 + CHANSPC_M) x 2^CHANSPC_E (cc110l.txt:3869-3880)
 *   IF      = fXOSC / 2^10 x FREQ_IF (cc110l.txt:3700-3711)
 *   rate    = (256 + DRATE_M) x 2^DRATE_E / 2^28 x fXOSC (cc110l.txt:3768-3778)
 *   BW      = fXOSC / (8 x (4 + CHANBW_M) x 2^CHANBW_E) (cc110l.txt:3755-3763)
 *   dev     = fXOSC / 2^17 x (8 + DEVIATION_M) x 2^DEVIATION_E (cc110l.txt:3882-3906) */
#define FXOSC_HZ         26000000ull
#define FREQ_WORD        0x213276ull
#define CH_SPACING_X2_18 (FXOSC_HZ * (256u + 0xF8u) * (1u << (0x22 & 3)))
#define CARRIER_HZ(ch)   ((FXOSC_HZ * FREQ_WORD) / 65536u + ((ch) * CH_SPACING_X2_18) / 262144u)
_Static_assert(CARRIER_HZ(0) > 863124000ull && CARRIER_HZ(0) < 863126000ull, "base 863.125 MHz");
_Static_assert(CH_SPACING_X2_18 / 262144u == 199951u, "spacing 199.951 kHz");
_Static_assert(CARRIER_HZ(26) > 868322700ull && CARRIER_HZ(26) < 868324700ull,
               "ch 26 = 868.3237 MHz");
_Static_assert(CARRIER_HZ(16) > 866323500ull && CARRIER_HZ(16) < 866325000ull,
               "ch 16 = 866.3242 MHz");
_Static_assert(CARRIER_HZ(RADIO_CHANNEL) + 999000ull <= CARRIER_HZ(RADIO_PROTECTED_CHANNEL)
                   || CARRIER_HZ(RADIO_CHANNEL) >= CARRIER_HZ(RADIO_PROTECTED_CHANNEL) + 999000ull,
               "the bridge's channel is at least 999 kHz from the protected one");
_Static_assert(FXOSC_HZ * 6u / 1024u == 152343u, "IF 152 kHz with FSCTRL1 = 0x06");
_Static_assert((256u + 0xF8u) * (1ull << 10) * FXOSC_HZ / (1ull << 28) == 49987u, "49.99 kBaud");
_Static_assert(FXOSC_HZ / (8u * (4u + 2u) * 4u) == 135416u, "RX BW 135 kHz");
_Static_assert(FXOSC_HZ * 8u * 16u / (1u << 17) == 25390u, "deviation 25.39 kHz");

/* RSSI_dBm = RSSI_dec(signed) / 2 - RSSI_offset, offset 74 dB at 868 MHz
 * (cc110l.txt:2130-2144). Kept in half dB. */
#define RSSI_OFFSET_DB 74
/* FREQEST resolution fXOSC / 2^14 = 1586.9 Hz = 203125 / 128 Hz
 * (cc110l.txt:4429-4434). */
#define FREQEST_HZ(fe) ((int32_t)(int8_t)(fe) * 203125 / 128)

/* --- Interrupts -------------------------------------------------------------- */
#define NOTE_GDO0  (1u << 0) /* end of a packet, received or sent */
#define NOTE_CRCOK (1u << 1) /* a packet arrived with a good CRC */
#define NOTE_FLASH (1u << 2)
#define NOTE_STORM (1u << 3)
#define NOTE_SYNC  (1u << 4)
#define NOTE_LINK  (1u << 5) /* the net task queued a message for a device */
/* The ACK interrupt could not strobe STX after making the chip IDLE (see
 * cc110l_ack_fire_from_isr()): CHECK, it strobed SRX again, confirm RX;
 * REARM, it could not, abort and re-arm now. */
#define NOTE_ACK_CHECK (1u << 6)
#define NOTE_ACK_REARM (1u << 7)
#define NOTE_PAIR      (1u << 8)  /* the pair button: open or close a window (#19) */
#define NOTE_RESET     (1u << 9)  /* the factory reset button (P6) */
#define NOTE_UPDATE    (1u << 10) /* the update task posted a flash operation (#24) */

/* FreeRTOS FromISR calls need a numeric priority >= 5 (FreeRTOSConfig.h,
 * configMAX_SYSCALL_INTERRUPT_PRIORITY = 0x50). */
#define GDO_IRQ_PRIO (5u << 4)
_Static_assert(GDO_IRQ_PRIO >= configMAX_SYSCALL_INTERRUPT_PRIORITY, "FromISR needs prio >= 5");

static TaskHandle_t self;

static void notify_from_isr(uint32_t bits) {
    BaseType_t woken = pdFALSE;
    if (self) {
        xTaskNotifyFromISR(self, bits, eSetBits, &woken);
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
 * preempt each other and share the counter safely. */
#define STORM_WINDOW_US 1000u
#define STORM_MAX_EDGES 32u

static uint32_t      storm_win_start, storm_edges;
static volatile bool gdo_irq_on;

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
static volatile uint32_t gdo0_edges;

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
static volatile uint32_t acks_fired, acks_failed;

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

static void gdo_irq_enable(bool on) {
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

static void gdo_irq_setup(void) {
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

void radio_request_flash_check(void) {
    if (self) {
        xTaskNotify(self, NOTE_FLASH, eSetBits);
    }
}

void radio_notify_link(void) {
    if (self) {
        xTaskNotify(self, NOTE_LINK, eSetBits);
    }
}

void radio_request_pairing(void) {
    if (self) {
        xTaskNotify(self, NOTE_PAIR, eSetBits);
    }
}

void radio_request_factory_reset(void) {
    if (self) {
        xTaskNotify(self, NOTE_RESET, eSetBits);
    }
}

/* The update's flash operations (#24, src/fwupd.h): one at a time, posted
 * by the update task through src/handoff.c, run by this task's loop as
 * flash work (update_op_run() below). The hand-over has a time limit
 * (#24 review M1): an operation this task has not claimed after
 * RADIO_UPDATE_OP_WAIT_MS (src/radio.h, 10 s) is withdrawn and never runs,
 * so the update task never waits for ever on a loop that does not run
 * (radio_configure() failing, below). The limit is well above the longest
 * wait in normal operation: the transaction in flight (at most a 1 s wait
 * for its sample, a 1 s train and the ACK window, docs/design.md "Flash
 * work and the coordinator") plus one store write before the operation.
 * src/fwupd_task.c writes the 10 s out in a log line. */

static TaskHandle_t volatile upd_task;

uint32_t handoff_port_ms(void) {
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

void handoff_port_wake(void) {
    xTaskNotify(self, NOTE_UPDATE, eSetBits);
}

void handoff_port_sleep(uint32_t ms) {
    /* Any notification wakes it (a block handed over meanwhile, too):
     * handoff_run() looks at done and the clock. */
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
}

bool radio_update_op(struct fwupd_op *op) {
    TickType_t t0 = xTaskGetTickCount();
    while (self == NULL) {
        /* The radio task has not started yet: it sets self first thing. */
        if (xTaskGetTickCount() - t0 >= pdMS_TO_TICKS(RADIO_UPDATE_OP_WAIT_MS)) {
            op->ok   = false;
            op->done = false;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    upd_task = xTaskGetCurrentTaskHandle();
    return handoff_run(op, RADIO_UPDATE_OP_WAIT_MS);
}

/* Microseconds since boot that do not wrap: TIM2's 32 bits extended. Radio
 * task only, called on every loop pass, far more often than the 71 minutes
 * of one wrap. */
static uint64_t now64(void) {
    static uint32_t last, high;
    uint32_t        t = timebase_us();
    if (t < last) {
        high++;
    }
    last = t;
    return (uint64_t)high << 32 | t;
}

/* A recent 32-bit timestamp on that clock. */
static uint64_t stamp64(uint32_t stamp) {
    uint64_t n = now64();
    return n - (uint32_t)((uint32_t)n - stamp);
}

/* --- Counters ---------------------------------------------------------------- */
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
static struct counters minute, total;

#define COUNT(field)    \
    do {                \
        minute.field++; \
        total.field++;  \
    } while (0)

/* --- Formatting helpers ------------------------------------------------------- */
static const char *half_db(char *b, size_t sz, int x2) {
    unsigned a = (unsigned)(x2 < 0 ? -x2 : x2);
    snprintf(b, sz, "%s%u.%u", x2 < 0 ? "-" : "", a / 2, (a & 1) ? 5u : 0u);
    return b;
}

static int rssi_x2(uint8_t raw) {
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

static const char *tenths(char *b, size_t sz, int32_t x10) {
    unsigned a = (unsigned)(x10 < 0 ? -x10 : x10);
    snprintf(b, sz, "%s%u.%u", x10 < 0 ? "-" : "", a / 10, a % 10);
    return b;
}

static void wake_flush(void) {
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
#define RX_BUF_MAX  (1u + PKTLEN_MAX + 2u) /* length byte, payload, RSSI, CRC_OK */
#define RX_STALE_US 60000u /* longest frame 131 B x 160 us = 21 ms; freedo uses 60 ms */

static struct {
    uint8_t  buf[RX_BUF_MAX];
    unsigned got, total;
    bool     assembling;
    uint32_t start_us;
    /* A sync edge was seen and its frame is not complete yet: pump every
     * tick (see the top of this file). Given up after RX_STALE_US. */
    bool     in_packet;
    uint32_t sync_us;
    bool     ack_considered; /* ack_consider() has run for this frame */
    /* gdo0_edges before the RXBYTES read that brought this frame's length
     * byte: at frame_done() exactly one more edge (the frame's end) means
     * gdo0_fall_us is this frame's end (#19, beacon_answer()). */
    uint32_t edges_at_len;
} rx;

static bool packet_arriving(void) {
    return rx.assembling || rx.in_packet || gpio_get(GPIOA, BOARD_GDO0_PIN);
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

static struct {
    bool     valid;
    uint8_t  fe, rssi;
    uint32_t edges; /* gdo0_edges when sampled (after this packet's sync) */
} fe_sample;

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

/* MARCSTATE values reachable only through a transmit strobe: FSTXON, TX,
 * TX_END, RXTX_SWITCH, TXFIFO_UNDERFLOW (0x12-0x16), and TXRX_SWITCH (0x10),
 * the settling step from TX back to RX (cc110l.txt:4462-4495, state diagram
 * :2316-2324). */
static bool marc_is_tx(uint8_t m) {
    m &= 0x1F;
    return m == CC_MARC_TXRX_SWITCH || (m >= CC_MARC_FSTXON && m <= CC_MARC_TXFIFO_UNF);
}

/* Returns false at once on a TX state, so the caller's failure path (which
 * runs tx_watch) stops it without waiting out the timeout. */
static bool wait_marcstate(uint8_t want, uint32_t timeout_us) {
    uint32_t start = timebase_us();
    uint8_t  m;
    do {
        if (cc110l_read_status_stable(CC_MARCSTATE, &m)) {
            if ((m & 0x1F) == want) {
                return true;
            }
            if (marc_is_tx(m)) {
                return false;
            }
        }
    } while ((uint32_t)(timebase_us() - start) < timeout_us);
    return false;
}

/* Take one fresh chip status byte, then act on the TX-state flag: confirm with
 * MARCSTATE (one status byte can be corrupt, cc110l.txt:1994-1997); a TX
 * state confirmed: SIDLE, then stop the board (fw_panic, watchdog reset).
 * Not confirmed: counted as a glitch and checked again, since the MARCSTATE
 * reads bring status bytes of their own. Still flagged after three rounds,
 * or MARCSTATE unreadable: SIDLE and return false; the caller re-initialises.
 *
 * The probe reads RXBYTES (header 0xFB): at least 3 bit errors away from any
 * transmit strobe (STX 0x35/0xB5, SFSTXON 0x31/0xB1). The confirmation reads
 * MARCSTATE (0xF5), 1 bit from 0xB5 (STX); a TX state caused that way shows
 * in the next status byte and is caught by the next round. */
static bool tx_watch(void) {
    for (int round = 0; round < 3; round++) {
        uint8_t rb;
        (void)cc110l_read_status(CC_RXBYTES, &rb);
        if (!cc110l_take_tx_state_flag()) {
            return true;
        }
        uint8_t m = 0;
        if (!cc110l_read_status_stable(CC_MARCSTATE, &m)) {
            break;
        }
        if (marc_is_tx(m)) {
            if (cc110l_tx_expected()) {
                return true; /* a frame of ours started while we looked */
            }
            (void)radio_strobe(CC_SIDLE, NULL);
            fw_panic("radio: chip in a TX state that is not ours", __FILE__, __LINE__);
        }
        COUNT(status_glitch);
    }
    (void)radio_strobe(CC_SIDLE, NULL);
    LOG("radio: status bytes keep reporting a TX state, MARCSTATE unconfirmed or unreadable; "
        "forced IDLE");
    return false;
}

/* Bring-up failure or re-init: check first (a TX state found here is stopped
 * and stops the board), then force IDLE whatever the state, then check the
 * status byte that SIDLE and the probe returned. GDO interrupts off. */
static void radio_force_idle(void) {
    gdo_irq_enable(false);
    (void)tx_watch();
    (void)radio_strobe(CC_SIDLE, NULL);
    (void)tx_watch();
}

/* SIDLE, then SFRX once IDLE is reached (SFRX only in IDLE or overflow,
 * cc110l.txt:3252-3253, 1501; SIDLE drops strobes sent before IDLE,
 * cc110l.txt:1449-1451), then SRX. RX to IDLE takes ~0.1 us, IDLE to RX
 * without calibration 75 us (cc110l.txt:2538-2549). */
static bool rx_flush_and_arm(void) {
    cc110l_ack_cancel();
    rx.assembling   = false;
    rx.in_packet    = false;
    fe_sample.valid = false;
    bool ok         = radio_strobe(CC_SIDLE, NULL) && wait_marcstate(CC_MARC_IDLE, 1000)
                      && radio_strobe(CC_SFRX, NULL) && radio_strobe(CC_SRX, NULL);
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

static void beacon_answer(uint32_t fall_us, bool own_end);

/* A complete frame is in rx.buf. By now the GDO2 interrupt has sent the
 * Enhanced ACK if one was prepared, so this may take its time and log. */
static void frame_done(void) {
    static uint32_t acks_seen;
    /* The frame's end edge, taken together with the edge count (#19). */
    taskENTER_CRITICAL();
    uint32_t fall_us = gdo0_fall_us;
    bool     own_end = (uint32_t)(gdo0_edges - rx.edges_at_len) == 1u;
    taskEXIT_CRITICAL();
    unsigned       len      = rx.buf[0];
    const uint8_t *mac      = &rx.buf[1];
    uint8_t        rssi_raw = rx.buf[1 + len];
    uint8_t st2    = rx.buf[2 + len]; /* bit 7 CRC_OK, bits 6:0 reserved (cc110l.txt:1747-1757) */
    bool    crc_ok = (st2 & 0x80) != 0;
    int     rssi   = rssi_x2(rssi_raw);
    int     fe     = FE_NONE;
    uint8_t sync_rssi = fe_sample.rssi;
    bool    rssi_diff = false;

    /* An ACK prepared for this frame and not sent (bad CRC, or prepared too
     * late) must not answer a later frame. */
    cc110l_ack_cancel();
    uint32_t fired = acks_fired;
    bool     acked = fired != acks_seen;
    acks_seen      = fired;
    rx.in_packet   = false;

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
    if (rx.assembling && (uint32_t)(timebase_us() - rx.start_us) > RX_STALE_US) {
        COUNT(stale);
        rx_flush_and_arm();
        return;
    }
    if (rx.in_packet && (uint32_t)(timebase_us() - rx.sync_us) > RX_STALE_US) {
        rx.in_packet = false; /* a sync edge without a frame (length-filtered, or our own TX) */
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
        if (!rx.assembling) {
            if (n < 2) {
                return;
            }
            if (!cc110l_read_rxfifo(rx.buf, 1)) {
                COUNT(spi_err);
                return;
            }
            unsigned len = rx.buf[0];
            if (len == 0 || len > PKTLEN_MAX) {
                COUNT(bad_len);
                rx_flush_and_arm();
                return;
            }
            rx.got            = 1;
            rx.total          = 1 + len + 2;
            rx.assembling     = true;
            rx.ack_considered = false;
            rx.edges_at_len   = edges;
            rx.start_us       = timebase_us();
            continue;
        }
        unsigned remaining = rx.total - rx.got;
        unsigned to_read   = 0;
        if (n >= remaining) {
            to_read = remaining;
        } else if (n > 1) {
            to_read = n - 1;
        }
        if (to_read == 0) {
            return;
        }
        if (!cc110l_read_rxfifo(rx.buf + rx.got, to_read)) {
            COUNT(spi_err);
            return;
        }
        rx.got += to_read;
        if (!rx.ack_considered && rx.got >= 1u + MAC_HDR_LEN) {
            /* The header is in. n < remaining: when RXBYTES was read just
             * now, the chip had not received the whole frame yet, so the
             * packet on the air is this one. */
            rx.ack_considered = true;
            ack_consider(rx.buf + 1, edges, n < remaining);
        }
        if (rx.got >= rx.total) {
            rx.assembling = false;
            frame_done(); /* FREQEST comes from the in-packet sample (#11) */
        }
    }
}

/* --- Bring-up ---------------------------------------------------------------- */
static bool reg_is_unused(unsigned a) {
    return a == 0x1E || a == 0x1F || a == 0x27 || a == 0x28;
}

/* Written with the stock (= reset) values; if this die reads them back
 * differently the radio stays idle, and the log line says which. */
static bool reg_is_reserved(unsigned a) {
    return a == CC_RSVD_20 || (a >= 0x29 && a <= 0x2B);
}

/* Registers 0x00-0x18 (IOCFG2 .. MCSM0) read back in one 25-byte burst and
 * compared with the table: they include IOCFG2/IOCFG0 (interrupt pins),
 * IOCFG1 (SO 3-state for the shared MISO), PKTCTRL0, CHANNR, FREQ and MCSM1
 * (RXOFF/TXOFF modes), and the chip changes none of them by itself. FSCAL3-0
 * (0x23-0x26) are not compared: calibration rewrites them. PATABLE[0]
 * (output power) is read back too and counts as one more register. Returns
 * the number of registers that differ, or HEALTH_REGS if the chip did not
 * answer. Used by the health check and after every calibration. */
#define HEALTH_REGS (CC_MCSM0 + 1u)
_Static_assert(CC_MCSM1 < HEALTH_REGS && CC_CHANNR < HEALTH_REGS && CC_PKTCTRL0 < HEALTH_REGS
                   && CC_IOCFG2 < HEALTH_REGS && CC_IOCFG0 < HEALTH_REGS,
               "health check covers the safety registers");

static unsigned regs_lost(void) {
    uint8_t back[HEALTH_REGS];
    if (!cc110l_read_burst(0x00, back, sizeof back)) {
        COUNT(spi_err);
        LOG("radio: register read-back: no answer");
        return HEALTH_REGS;
    }
    unsigned bad = 0;
    for (unsigned a = 0; a < HEALTH_REGS; a++) {
        if (back[a] != cc_config[a]) {
            if (bad < 4) {
                LOG("radio: reg %02x reads %02x, want %02x", a, back[a], cc_config[a]);
            }
            bad++;
        }
    }
    if (!cc110l_pa_reads_back()) {
        LOG("radio: PATABLE does not read back %02x", RADIO_PA_BRINGUP);
        bad++;
    }
    return bad;
}

/* Recalibration policy (#11). The datasheet asks for calibration
 * "regularly" and after power-up or a frequency change, and notes that
 * supply voltage or temperature changes make a calibration invalid
 * (cc110l.txt:2740-2752); it gives no interval. MCSM0 FS_AUTOCAL = 0 (stock
 * table) means the chip never calibrates by itself, and an Enhanced ACK
 * cannot wait for the 0.8 ms an auto-calibration adds to IDLE -> TX. So:
 * once at every bring-up, then every RECAL_PERIOD_MS from IDLE at a quiet
 * moment (no packet arriving, no transaction on the air), independent of
 * temperature, which the bridge does not measure [I]. A calibration older
 * than CC_CAL_MAX_AGE_US (two missed periods and more) stops the
 * transmitter until the next one. Each recalibration costs about 1 ms of
 * deaf receiver: a Data frame lost then is sent again by its device. */
#define RECAL_PERIOD_MS 300000u
_Static_assert(2ull * RECAL_PERIOD_MS * 1000u < CC_CAL_MAX_AGE_US,
               "a missed recalibration does not stop the transmitter yet");

static uint64_t cal_at;      /* last calibration attempt, now64() */
static uint64_t cal_good_at; /* last calibration with lock */

/* With the chip in IDLE: calibrate (lock check and carrier read-back in the
 * driver), then the same register read-back as the health check, so every
 * calibration ends with both. False if the chip failed to calibrate at all
 * or lost registers: the caller re-initialises. A calibration that did not
 * lock returns true: the receiver may run, the driver refuses to transmit
 * until the next bring-up. */
static bool radio_calibrate(const char *why) {
    uint8_t         fscal[3];
    unsigned        tries = 0;
    enum cc110l_cal c     = cc110l_calibrate(fscal, &tries);
    cal_at                = now64();
    if (c == CC_CAL_ERROR) {
        LOG("radio: %s calibration failed on try %u (not IDLE, no answer, or the carrier "
            "registers changed)",
            why,
            tries);
        return false;
    }
    unsigned lost = regs_lost();
    if (lost) {
        LOG("radio: %u registers differ after %s calibration", lost, why);
        return false;
    }
    if (c == CC_CAL_LOCKED) {
        cal_good_at = cal_at;
    }
    LOG("radio: %s calibration, %u %s: FSCAL3 %02x FSCAL2 %02x FSCAL1 %02x, PLL %s, transmitter "
        "%s",
        why,
        tries,
        tries == 1 ? "try" : "tries",
        fscal[0],
        fscal[1],
        fscal[2],
        c == CC_CAL_LOCKED ? "locked" : "NOT locked",
        cc110l_cal_ok() ? "may be used" : "REFUSED until the next bring-up");
    return tx_watch();
}

static bool radio_configure_steps(void) {
    gdo_irq_enable(false);
    cc110l_ack_cancel();
    rx.assembling   = false;
    rx.in_packet    = false;
    fe_sample.valid = false;

    if (!cc110l_reset()) {
        LOG("radio: no CHIP_RDYn after SRES (SO stayed high)");
        return false;
    }
    uint8_t part = 0xFF, ver = 0xFF;
    if (!cc110l_read_status(CC_PARTNUM, &part) || !cc110l_read_status(CC_VERSION, &ver)) {
        LOG("radio: PARTNUM/VERSION read failed");
        return false;
    }
    /* CC110L: PARTNUM 0x00, VERSION 0x17 "subject to change" (cc110l.txt:4419-4426). */
    LOG("radio: PARTNUM %02x VERSION %02x%s",
        part,
        ver,
        (part == 0x00 && ver == 0x17) ? " (CC110L as documented)" : " (not the documented 00/17)");
    if (ver == 0x00 || ver == 0xFF) {
        LOG("radio: VERSION %02x looks like a dead bus; not configuring", ver);
        return false;
    }
    if (!tx_watch()) {
        return false;
    }

    if (!cc110l_write_burst(0x00, cc_config, CC_NUM_CONFIG_REGS)) {
        LOG("radio: register burst write failed");
        return false;
    }
    if (!tx_watch()) {
        return false;
    }
    uint8_t back[CC_NUM_CONFIG_REGS];
    if (!cc110l_read_burst(0x00, back, sizeof back)) {
        LOG("radio: register read-back failed");
        return false;
    }
    unsigned bad = 0;
    for (unsigned a = 0; a < CC_NUM_CONFIG_REGS; a++) {
        if (!reg_is_unused(a) && back[a] != cc_config[a]) {
            LOG("radio: reg %02x%s wrote %02x read %02x",
                a,
                reg_is_reserved(a) ? " (reserved)" : "",
                cc_config[a],
                back[a]);
            bad++;
        }
    }
    if (bad) {
        LOG("radio: %u registers did not verify; staying idle", bad);
        return false;
    }

    /* Output power: SRES left PATABLE at 0xC6; the bring-up level is about
     * 20 dB lower (src/cc110l.h). */
    if (!cc110l_set_pa(RADIO_PA_BRINGUP)) {
        LOG("radio: PATABLE %02x not set or did not read back", RADIO_PA_BRINGUP);
        return false;
    }

    /* The chip is IDLE since SRES. A calibration without lock leaves the
     * transmitter refused for this bring-up; receiving goes on. */
    if (!radio_calibrate("bring-up")) {
        return false;
    }

    if (!radio_strobe(CC_SFRX, NULL) || !radio_strobe(CC_SRX, NULL)
        || !wait_marcstate(CC_MARC_RX, 2000)) {
        LOG("radio: did not enter RX");
        return false;
    }
    if (!tx_watch()) {
        return false;
    }
    gdo_irq_enable(true);
    LOG("radio: RX on channel %u (%lu Hz), PATABLE %02x, transmitter %s",
        RADIO_CHANNEL,
        (unsigned long)CARRIER_HZ(RADIO_CHANNEL),
        RADIO_PA_BRINGUP,
        !RADIO_TX_ALLOWED            ? "switched off (receive-only image)"
        : !cc110l_cal_ok()           ? "refused: no PLL lock"
        : cc110l_tx_verify_channel() ? "enabled on this channel only"
                                     : "refused: frequency registers do not read back");
    return true;
}

/* Reset, load and verify the table, calibrate, enter RX. On any failure the
 * chip is checked for a TX state and forced to IDLE before returning, with
 * the GDO interrupts left off. */
static bool radio_configure(void) {
    if (radio_configure_steps()) {
        return true;
    }
    radio_force_idle();
    return false;
}

/* Runtime re-init: check and force IDLE first, then the full bring-up. */
static void radio_reinit(void) {
    COUNT(reinit);
    radio_force_idle();
    (void)radio_configure();
}

/* Every RECAL_PERIOD_MS, at a quiet moment: from RX to IDLE, calibrate,
 * back to RX. A prepared ACK is dropped first; GDO2 cannot fire while the
 * chip is IDLE. Any failure re-initialises the radio. */
static void radio_recalibrate(void) {
    cc110l_ack_cancel();
    rx.assembling   = false;
    rx.in_packet    = false;
    fe_sample.valid = false;
    if (!radio_strobe(CC_SIDLE, NULL) || !wait_marcstate(CC_MARC_IDLE, 1000)
        || !radio_calibrate("periodic") || !rx_flush_and_arm()
        || !wait_marcstate(CC_MARC_RX, 2000)) {
        LOG("radio: periodic calibration failed; re-initialising");
        radio_reinit();
    }
}

/* Every 10 s: the chip must still hold our configuration and be in RX. A
 * brown-out or reset of the radio leaves reset defaults (IOCFG0 = 0x3F);
 * regs_lost() compares the registers that matter. */
static void health_check(void) {
    if (!gdo_irq_on) {
        /* A storm guard trip or a failed re-init left the interrupts off. */
        LOG("radio: GDO interrupts off; re-initialising");
        radio_reinit();
        return;
    }
    unsigned bad = regs_lost();
    if (bad == HEALTH_REGS) {
        /* No CHIP_RDYn: the chip is not answering, as the old IOCFG0 check
         * also treated it. */
        LOG("radio: health: no answer; re-initialising");
        radio_reinit();
        return;
    }
    if (bad) {
        LOG("radio: %u registers lost; forcing IDLE and re-initialising", bad);
        radio_reinit();
        return;
    }
    uint8_t m = 0;
    if (!cc110l_read_status_stable(CC_MARCSTATE, &m)) {
        COUNT(spi_err);
        return;
    }
    m &= 0x1F;
    if (marc_is_tx(m)) {
        if (cc110l_tx_expected()) {
            return; /* our own frame is on the air */
        }
        (void)radio_strobe(CC_SIDLE, NULL);
        fw_panic("radio: MARCSTATE in a TX state that is not ours", __FILE__, __LINE__);
    }
    if (m == CC_MARC_IDLE) {
        LOG("radio: found IDLE, re-arming RX");
        COUNT(rearm);
        rx_flush_and_arm();
    }
}

/* --- Flash on the shared bus (#10) ----------------------------------------- */
/* Wake the chip, wait for a program or erase still running (an MCU reset
 * does not stop it, and the array reads garbage meanwhile), then check the
 * JEDEC ID. True if the flash may be read: otherwise no record is read. */
static bool flash_boot(void) {
    (void)mx25r_wake(); /* its ID check fails while WIP = 1; checked below */
    bool     idle = mx25r_wait_idle();
    uint32_t id   = 0;
    uint8_t  sr   = 0;
    bool     ok   = idle && mx25r_read_id(&id) && mx25r_read_status(&sr) && id == MX25R_JEDEC_ID;
    LOG("flash: JEDEC ID %06lx%s, status register %02x%s",
        (unsigned long)id,
        id == MX25R_JEDEC_ID ? " (MX25R1635F)" : " (UNEXPECTED, want c22815)",
        sr,
        ok     ? ""
        : idle ? " (read failed): flash not used"
               : " (still busy after 480 ms): flash not used");
    return ok;
}

static void flash_check(void) {
    bool     in_frame = rx.assembling || gpio_get(GPIOA, BOARD_GDO0_PIN);
    uint32_t id       = 0;
    if (mx25r_read_id(&id) && id == MX25R_JEDEC_ID) {
        COUNT(flash_ok);
    } else {
        COUNT(flash_bad);
        LOG("flash: JEDEC ID mismatch: %06lx", (unsigned long)id);
    }
    if (in_frame) {
        COUNT(flash_in_frame);
    }
}

/* Listen before transmit by RSSI (see csl_port_listen_begin()). Threshold
 * [I]: -85 dBm, about 19 dB above the sensitivity at 868 MHz and 38.4
 * kBaud, -104 dBm (cc110l.txt:600-603; 50 kBaud here), and well below the
 * -41 to -72 dBm of the tado frames seen on the bench (radio-runtime.md,
 * P2). The highest RSSI of the minute's listens is logged, to set it from
 * bench data. */
#define LBT_RSSI_BUSY_DBM (-85)
static bool lbt_listening;
static bool lbt_rssi_seen; /* lbt_rssi_max_x2 holds a value this minute */
static int  lbt_rssi_max_x2;

static void log_minute(void) {
    LOG("radio: 60 s: frames %lu (crc ok %lu, bad %lu, wake %lu), overflow %lu, bad len %lu, stale "
        "%lu, "
        "rearm %lu, reinit %lu, spi err %lu, status glitch %lu, irq storm %lu",
        (unsigned long)minute.frames,
        (unsigned long)minute.crc_ok,
        (unsigned long)minute.crc_bad,
        (unsigned long)minute.wake,
        (unsigned long)minute.overflow,
        (unsigned long)minute.bad_len,
        (unsigned long)minute.stale,
        (unsigned long)minute.rearm,
        (unsigned long)minute.reinit,
        (unsigned long)minute.spi_err,
        (unsigned long)minute.status_glitch,
        (unsigned long)minute.storm);
    LOG("flash: 60 s: JEDEC ID ok %lu, mismatch %lu (%lu during a frame); total frames %lu, "
        "flash mismatches %lu",
        (unsigned long)minute.flash_ok,
        (unsigned long)minute.flash_bad,
        (unsigned long)minute.flash_in_frame,
        (unsigned long)total.frames,
        (unsigned long)total.flash_bad);
    const struct mx25r_stats *fs = mx25r_stats();
    LOG("flash: 60 s: radio served %lu times during flash work, frames %lu, acks %lu; totals: "
        "cycles %lu (longest %lu us), held %lu (longest %lu us, gave up %lu)",
        (unsigned long)minute.flash_serve,
        (unsigned long)minute.flash_frames,
        (unsigned long)minute.flash_acks,
        (unsigned long)fs->cycles,
        (unsigned long)fs->cycle_max,
        (unsigned long)fs->held,
        (unsigned long)fs->held_max,
        (unsigned long)fs->gave_up);
    LOG("radio: 60 s: freqest in packet %lu, none %lu (stale sample dropped %lu), sync rssi "
        "differs %lu",
        (unsigned long)minute.fe_pkt,
        (unsigned long)minute.fe_none,
        (unsigned long)minute.fe_stale,
        (unsigned long)minute.fe_rssi_diff);
    const struct link_stats *ls = link_stats();
    /* Two lines: one log line holds 191 characters (src/log.c). */
    LOG("link: 60 s: acks prepared %lu sent %lu too late %lu, tx frames %lu fail %lu refused %lu "
        "(no pll lock %lu)",
        (unsigned long)minute.ack_prepared,
        (unsigned long)minute.ack_sent,
        (unsigned long)minute.ack_late,
        (unsigned long)minute.tx_frames,
        (unsigned long)minute.tx_fail,
        (unsigned long)minute.tx_refused,
        (unsigned long)minute.tx_no_lock);
    LOG("link: airtime last hour %lu ms of %lu ms; 60 s: refused for airtime %lu frames, %lu acks; "
        "acks failed %lu (re-armed %lu)",
        (unsigned long)(cc110l_airtime_used_us() / 1000u),
        (unsigned long)(AIRTIME_BUDGET_US / 1000u),
        (unsigned long)minute.tx_duty,
        (unsigned long)minute.ack_duty,
        (unsigned long)acks_failed,
        (unsigned long)minute.ack_rearm);
    {
        char r[12];
        LOG("link: 60 s: listens busy by rssi %lu (threshold %d dBm), highest listen rssi %s%s",
            (unsigned long)minute.lbt_rssi_busy,
            LBT_RSSI_BUSY_DBM,
            lbt_rssi_seen ? half_db(r, sizeof r, lbt_rssi_max_x2) : "none",
            lbt_rssi_seen ? " dBm" : "");
        lbt_rssi_seen = false;
    }
    LOG("link: totals: to server %lu, dup %lu, mic fail %lu, dropped %lu, downlinks %lu refused "
        "%lu, RA %lu",
        (unsigned long)ls->rx[LINK_RX_FORWARDED],
        (unsigned long)ls->rx[LINK_RX_DUPLICATE],
        (unsigned long)ls->rx[LINK_RX_MIC_FAIL],
        (unsigned long)ls->rx[LINK_RX_DROPPED],
        (unsigned long)ls->down_queued,
        (unsigned long)(ls->down_refused + ls->down_full),
        (unsigned long)ls->ra_queued);
    const struct pair_stats *ps = pair_stats();
    if (ps->windows) {
        LOG("pair: 60 s: beacons sent %lu, not sent: end not live %lu, busy %lu, late %lu, failed "
            "%lu; totals: requests answered %lu, from others %lu",
            (unsigned long)minute.beacon_sent,
            (unsigned long)minute.beacon_no_stamp,
            (unsigned long)minute.beacon_busy,
            (unsigned long)minute.beacon_late,
            (unsigned long)minute.beacon_fail,
            (unsigned long)ps->req[PAIR_REQ_OK],
            (unsigned long)ps->req[PAIR_REQ_NOT_TARGET]);
        LOG("pair: totals: windows %lu, paired %lu, target frames %lu, RA pairs %lu, d/pair sent "
            "%lu delivered %lu failed %lu",
            (unsigned long)ps->windows,
            (unsigned long)ps->paired,
            (unsigned long)ps->joiner_frames,
            (unsigned long)ps->ra_pairs,
            (unsigned long)ps->dpair_sent,
            (unsigned long)ps->dpair_delivered,
            (unsigned long)ps->dpair_failed);
    }
    minute = (struct counters){0};
}

/* --- The coordinator's radio (src/csl.h "Port") ------------------------------- */
/* Notification bits taken while a frame of ours is on the air, for the task
 * loop to act on afterwards. */
static uint32_t pending_bits;

uint64_t csl_port_now(void) {
    return now64();
}

/* Backoff source: xorshift32, seeded once from the DRBG when it is ready
 * (rng_try_bytes never waits). Until then, and mixed in afterwards, the
 * microsecond clock: the backoff only has to differ between attempts. */
static uint32_t lbt_state;
static bool     lbt_seeded;

uint32_t csl_port_random(void) {
    uint32_t x = lbt_state ^ timebase_us();
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    lbt_state = x ? x : 1u;
    return x;
}

#define TX_TIMEOUT_US 100000u /* freedo TX_TMO_MS; the longest frame is 22 ms on the air */

/* A transmission failed part way: stop it, empty both FIFOs, receive again. */
static bool tx_failed(void) {
    COUNT(tx_fail);
    cc110l_tx_abort();
    rx_flush_and_arm();
    return false;
}

/* Sleep until a GDO edge or the next tick. Edges of our own frame are not
 * the receiver's business; anything else is kept for the task loop. */
static void tx_wait_tick(void) {
    uint32_t bits = 0;
    (void)xTaskNotifyWait(0, UINT32_MAX, &bits, 1);
    pending_bits |= bits & ~(NOTE_GDO0 | NOTE_SYNC | NOTE_CRCOK);
    watchdog_checkin(WDG_RADIO);
}

/* One frame through the driver: cc110l_tx_start(), or cc110l_tx_answer()
 * for a frame that answers a received one. mark: the pairing LED goes on
 * right after the STX strobe (#19, the beacon's scope marker), and *stx_us
 * (if not NULL) is the time just after it. */
static bool send_frame(const uint8_t *frame,
                       size_t         len,
                       const uint8_t *answered,
                       unsigned       flags,
                       bool           mark,
                       uint32_t      *stx_us) {
    if (!gdo_irq_on) {
        /* The storm guard tripped or a re-init failed: the chip may have
         * reset and is not known to hold our configuration. Nothing goes
         * out until the bring-up has passed again. */
        COUNT(tx_refused);
        return false;
    }
    /* An Enhanced ACK of ours may still be on the air (it was sent before
     * the coordinator stopped the receiver's ACKs): at most its airtime and
     * margin, 8.5 ms. The driver refuses while one is. */
    for (unsigned i = 0; i < 16 && cc110l_tx_expected(); i++) {
        tx_wait_tick();
    }
    enum cc110l_tx_result r = answered ? cc110l_tx_answer(frame, len, answered, flags)
                                       : cc110l_tx_start(frame, len, flags);
    if (r == CC_TX_OK) {
        if (mark) {
            board_led(LED_PAIRING, true);
        }
        if (stx_us) {
            *stx_us = timebase_us();
        }
    }
    if (r == CC_TX_DISABLED || r == CC_TX_REFUSED || r == CC_TX_BUSY || r == CC_TX_NO_LOCK
        || r == CC_TX_DUTY || r == CC_TX_NO_PA) {
        /* The chip was not touched: still receiving. A frame refused for
         * airtime fails its transaction; the coordinator gives up on the
         * datagram after CSL_ATTEMPTS (src/csl.h), it does not wait. */
        COUNT(tx_refused);
        if (r == CC_TX_NO_LOCK) {
            COUNT(tx_no_lock);
        } else if (r == CC_TX_DUTY) {
            COUNT(tx_duty);
        }
        return false;
    }
    /* cc110l_tx_start() emptied the RX FIFO. */
    rx.assembling   = false;
    rx.in_packet    = false;
    fe_sample.valid = false;
    if (r != CC_TX_OK) {
        return tx_failed();
    }
    /* Feed a frame longer than the FIFO, then wait for the packet to leave
     * the chip: out of TX and TX_END with an empty TX FIFO. MCSM1 returns
     * the chip to RX, so it never shows IDLE (freedo .ino:608-625). */
    uint32_t t0 = timebase_us();
    for (;;) {
        size_t left = 0;
        if (!cc110l_tx_feed(&left)) {
            return tx_failed();
        }
        if (left == 0) {
            uint8_t m = 0, tb = 0xFF;
            if (cc110l_read_status_stable(CC_MARCSTATE, &m)
                && cc110l_read_status_stable(CC_TXBYTES, &tb)) {
                m &= 0x1F;
                if (m == CC_MARC_TXFIFO_UNF || m == CC_MARC_IDLE) {
                    return tx_failed(); /* IDLE: the chip lost its configuration */
                }
                if (m != CC_MARC_TX && m != CC_MARC_TX_END && (tb & 0x7Fu) == 0) {
                    break;
                }
            }
        }
        if ((uint32_t)(timebase_us() - t0) > TX_TIMEOUT_US) {
            return tx_failed();
        }
        tx_wait_tick();
    }
    /* The frame is out and the chip is on its way back to RX. Without this
     * the next frame of a train would wait out the margin of this one, and
     * its rendezvous count would be 2 ms old when it goes out. */
    cc110l_tx_done();
    COUNT(tx_frames);
    if (gpio_get(GPIOA, BOARD_GDO0_PIN)) {
        /* A sync word already: its edge went to tx_wait_tick(), which
         * drops the edges of our own frame. Pump every tick as for any
         * other packet. */
        rx.in_packet = true;
        rx.sync_us   = timebase_us();
    }
    return true;
}

bool csl_port_send(const uint8_t *frame, size_t len, unsigned flags) {
    return send_frame(frame, len, NULL, flags, false, NULL);
}

/* Listen before transmit: "a packet on the air" is a sync word seen at any
 * time during the listen (GDO0 edges), a frame still being read, a frame
 * of our own (an Enhanced ACK) on the air, or energy in the channel: the
 * RSSI at or above LBT_RSSI_BUSY_DBM at the start of the listen, at its
 * end, or at any loop pass in between (about one, 2-3 ms in; the task
 * waits for the rest of the 3.968 ms). Sync words show tado frames; the
 * RSSI also shows other users of the band, which the sync words miss
 * (test-bench.md: LoRaWAN-like bursts on neighbouring channels). In RX the
 * RSSI register updates continuously until a sync word freezes it
 * (cc110l.txt:2113-2115). The chip's frequency registers are read back
 * here as well: if they are not what was written, the transaction ends as
 * "busy" before its first frame, which the driver would refuse anyway (it
 * reads them back again for every frame). A failed RSSI read counts as busy;
 * threshold and statistics are with the minute log above. */
static uint32_t lbt_edges;
static bool     lbt_busy, lbt_rssi_hit;

static bool on_air_now(void) {
    return packet_arriving() || cc110l_tx_expected();
}

static void lbt_sample_rssi(void) {
    uint8_t raw;
    if (!cc110l_read_status_stable(CC_RSSI, &raw)) {
        COUNT(spi_err);
        lbt_rssi_hit = true;
        return;
    }
    int x2 = rssi_x2(raw);
    if (!lbt_rssi_seen || x2 > lbt_rssi_max_x2) {
        lbt_rssi_max_x2 = x2;
        lbt_rssi_seen   = true;
    }
    if (x2 >= 2 * LBT_RSSI_BUSY_DBM) {
        lbt_rssi_hit = true;
    }
}

void csl_port_listen_begin(void) {
    lbt_edges     = gdo0_edges;
    lbt_rssi_hit  = false;
    lbt_listening = true;
    lbt_busy      = on_air_now() || !cc110l_tx_verify_channel();
    lbt_sample_rssi();
}

bool csl_port_listen_busy(void) {
    lbt_sample_rssi();
    lbt_listening = false;
    if (lbt_rssi_hit) {
        COUNT(lbt_rssi_busy);
    }
    return lbt_busy || lbt_rssi_hit || gdo0_edges != lbt_edges || on_air_now();
}

/* --- Pairing: the beacon answer (#19) -------------------------------------------
 * The target's Beacon Request has just been taken (frame_done()) and its
 * Enhanced Beacon built (link_beacon()). Deadline: the beacon's preamble
 * starts 2.4-7.4 ms after the request ended (pair.h). The request's end is
 * gdo0_fall_us, stamped by the GDO0 interrupt within microseconds of the
 * edge, later by at most one SPI1 critical section (about 0.1 ms), which
 * only moves the beacon later. Steps:
 *   1. The stamp must be the request's own end (own_end: exactly one GDO0
 *      edge since its length byte was read, so this task took the request
 *      while it was arriving). Otherwise the stamp may belong to a later
 *      packet: no beacon.
 *   2. Wait, spinning, until PAIR_BEACON_TX_AT_US after the stamp (about
 *      3.7 ms minus the time this task needed to get here: the pump, the
 *      CCM check of the request and building the beacon, well under
 *      0.5 ms [I]). Any GDO0 edge (a sync word), GDO0 high, a frame of ours
 *      on the air, a coordinator transaction holding the air or starting
 *      its listen less than PAIR_BEACON_QUIET_US (15 ms) from the decision
 *      (the beacon must not shorten that listen), or an RSSI at or above
 *      LBT_RSSI_BUSY_DBM at the end makes the channel busy: no beacon. The
 *      commercial bridge listens 3.968 ms here (freedo
 *      cc1101_bridge.ino:228-230).
 *   3. Too late (past PAIR_BEACON_TX_LATEST_US, e.g. this task was blocked
 *      by a flash program): no beacon.
 *   4. cc110l_tx_answer(): channel read-back, transmit gate (pairing flag,
 *      test channel, answers this request), airtime, SIDLE/SFRX/SFTX, 33
 *      FIFO bytes, STX: about 0.15 ms [I]. Then IDLE to TX 75.2 us
 *      (docs/datasheets/cc110l.txt:2542) to the first preamble bit.
 * So the preamble starts about 3.9-4.0 ms after the request ended, 1.5 ms
 * after the earliest and 3.4 ms before the latest the device expects [I].
 * The device asks again 8 s later when no beacon comes. Logged after the
 * beacon is on the air: STX time after the request's end. PB8 (pairing LED)
 * goes off here, a few microseconds of GPIO write, and rises right after
 * the STX strobe, whatever the LED showed before (a flash phase, or lit
 * during an exchange): about 3.3 ms dark, too short to see. A beacon not
 * sent leaves it off until the end of this task loop pass, which sets it
 * as the window says. */
static void beacon_answer(uint32_t fall_us, bool own_end) {
    size_t         len      = 0;
    const uint8_t *answered = NULL;
    const uint8_t *b        = link_beacon(&len, &answered);
    if (!b) {
        return;
    }
    board_led(LED_PAIRING, false);
    uint32_t edges = gdo0_edges;
    uint32_t el    = timebase_us() - fall_us;
    bool     busy  = false;
    while (own_end && el < PAIR_BEACON_TX_AT_US) {
        if (gdo0_edges != edges || gpio_get(GPIOA, BOARD_GDO0_PIN) || cc110l_tx_expected()
            || !csl_quiet_for(PAIR_BEACON_QUIET_US)) {
            busy = true;
            break;
        }
        uint32_t left = PAIR_BEACON_TX_AT_US - el;
        delay_us(left > 100u ? 100u : left);
        el = timebase_us() - fall_us;
    }
    if (own_end && !busy) {
        uint8_t raw = 0;
        busy = gdo0_edges != edges || gpio_get(GPIOA, BOARD_GDO0_PIN)
               || !csl_quiet_for(PAIR_BEACON_QUIET_US) || !cc110l_read_status_stable(CC_RSSI, &raw)
               || rssi_x2(raw) >= 2 * LBT_RSSI_BUSY_DBM;
        el   = timebase_us() - fall_us;
    }
    uint32_t stx = 0;
    bool     ok  = false;
    if (!own_end) {
        COUNT(beacon_no_stamp);
    } else if (busy) {
        COUNT(beacon_busy);
    } else if (el > PAIR_BEACON_TX_LATEST_US) {
        COUNT(beacon_late);
    } else if ((ok = send_frame(b, len, answered, TXGATE_PAIRING, true, &stx))) {
        COUNT(beacon_sent);
    } else {
        COUNT(beacon_fail);
    }
    uint8_t seq = b[2];
    link_beacon_done(ok, (uint32_t)xTaskGetTickCount());
    if (ok) {
        LOG("pair: beacon seq %02x to the target sent: STX %lu us after the request ended "
            "(preamble about %lu us; window %u-%u us)",
            seq,
            (unsigned long)(stx - fall_us),
            (unsigned long)(stx - fall_us + 75u),
            PAIR_BEACON_START_MIN_US,
            PAIR_BEACON_START_MAX_US);
    } else {
        LOG("pair: beacon seq %02x to the target NOT sent: %s (%lu us after the request ended)",
            seq,
            !own_end                        ? "request end not seen live"
            : busy                          ? "channel busy"
            : el > PAIR_BEACON_TX_LATEST_US ? "too late"
                                            : "refused or failed",
            (unsigned long)el);
    }
}

/* The pair button (#19): open a window for the device named in the build,
 * or close the one that is open. Runs at flash_quiet() (no packet
 * arriving, no transaction set up), never inside a window: creating a
 * network blocks this task for up to about 0.8 s (fw-reviewer items 1-3 of
 * #19). The pairing trigger therefore needs, all of them: a target named in
 * config/local.cmake (PAIR_TARGET_EUI64), the transmitter built in
 * (RADIO_TX_ALLOWED, the CMake option), the flash read at boot, an
 * identity, the radio up with PLL lock, and the owner's hand on the
 * button. Without a target the code below is not even compiled: the build
 * then holds no path to network_create() or link_pair_open(). */
static bool flash_ready; /* flash_boot() succeeded: the records may be read and written */

static void pairing_trigger(uint32_t now_ms) {
    if (link_pair_active(now_ms)) {
        link_pair_close();
        LOG("pair: window closed by the pair button");
        return;
    }
#ifndef PROVISION_PAIR_EUI64
    LOG("pair: no device named for pairing (PAIR_TARGET_EUI64 in config/local.cmake): no window");
#else
    static const uint8_t eui[MAC_EUI_LEN] = PROVISION_PAIR_EUI64;
    uint8_t              target[MAC_EUI_LEN];
    for (unsigned i = 0; i < MAC_EUI_LEN; i++) {
        target[i] = eui[MAC_EUI_LEN - 1u - i]; /* wire order, as link_start() */
    }
    if (!RADIO_TX_ALLOWED) {
        LOG("pair: receive-only image (RADIO_TX_ALLOWED 0): no window");
        return;
    }
    if (!flash_ready || !link_own()) {
        LOG("pair: %s: no window", !flash_ready ? "flash not ready at boot" : "no identity");
        return;
    }
    /* The target must not be the bridge itself (its EUI-64 from the
     * identity record, #21); checked before any network is created. */
    if (memcmp(target, link_own(), MAC_EUI_LEN) == 0) {
        LOG("pair: PAIR_TARGET_EUI64 is the bridge's own EUI-64: no window");
        return;
    }
    if (!gdo_irq_on || !cc110l_cal_ok()) {
        LOG("pair: radio not ready to transmit (no PLL lock or interrupts off): no window");
        return;
    }
    if (link_pair_frames_queued()) {
        LOG("pair: frames of the last window still queued: no window; hold the button again in "
            "a few seconds");
        return;
    }
    uint8_t key[CCM_KEY_LEN], rnd[PAIR_RAND_LEN];
    bool    ok;
    if (network_active()) {
        ok = network_key(key);
    } else if (network_absent()) {
        LOG("pair: no network yet: creating one on channel %u before the window opens",
            RADIO_CHANNEL);
        ok = network_create(RADIO_CHANNEL, key);
    } else {
        LOG("pair: a network record exists but is not in use: no window, no new network over it");
        return;
    }
    ok = ok && rng_random(NULL, rnd, sizeof rnd) == 0
         && link_pair_open(target, key, rnd, (uint32_t)xTaskGetTickCount());
    mbedtls_platform_zeroize(key, sizeof key);
    mbedtls_platform_zeroize(rnd, sizeof rnd);
    if (ok) {
        LOG("pair: window open for %u s on channel %u, for the device named in the build only; "
            "LED flashing until it is found",
            PAIR_WINDOW_MS / 1000u,
            RADIO_CHANNEL);
    } else {
        LOG("pair: window not opened (network key not read back, no random numbers, or the "
            "target is not a device address)");
    }
#endif
}

/* Factory reset (P6, docs/design.md "Lifecycle"): erase the network record
 * and the counter log, keep the identity, restart. At a quiet moment; the
 * erases (four sectors, up to about 1 s) receive frames meanwhile (#22),
 * which, without a key, are not acknowledged. The restart makes the next
 * boot read what the flash holds: no network after a complete reset
 * ("network: no network record"), or whatever survived a failed one. */
#define FACTORY_LOG_MS 200u /* time for the debugger to read the last RTT lines */

static void factory_reset(void) {
    if (!flash_ready) {
        LOG("factory reset: flash not ready at boot: nothing erased");
        return;
    }
    LOG("factory reset: erasing the network record and the counter log; the identity stays");
    bool ok = network_factory_reset();
    LOG("factory reset: %s; restarting",
        ok ? "done" : "FAILED (an erase failed or did not read back blank); hold the button again");
    vTaskDelay(pdMS_TO_TICKS(FACTORY_LOG_MS));
    scb_reset_system(); /* SYSRESETREQ, rm0383.txt:4551-4559 */
}

/* One update operation (#24), at flash_quiet() with the coordinator held,
 * like the store's writes (#22): the driver serves the receiver throughout,
 * so Enhanced ACKs and beacon answers go on; the coordinator starts nothing
 * until the operation is over (at most one sector erase with its blank
 * check, about 250 ms, or one boot-state write). Between two operations
 * the loop runs csl_step() before the update task (lower priority) can
 * post the next one, so the coordinator gets a transaction in between
 * (tests/host/test_csl.c, test_update_windows_under_traffic()). */
static void update_op_run(void) {
    struct fwupd_op *op = handoff_claim();
    if (op == NULL) {
        return; /* withdrawn by the update task a moment ago (src/handoff.h) */
    }
    op->network = network_active();
    if (op->kind == FWUPD_OP_RESTART) {
        /* The update task asked: an update was written (PENDING), or the
         * candidate under test was not confirmed in time. At a quiet
         * moment, so no flash write is cut; the bootloader takes over. */
        LOG("update: restarting into the bootloader");
        vTaskDelay(pdMS_TO_TICKS(FACTORY_LOG_MS));
        scb_reset_system(); /* SYSRESETREQ, rm0383.txt:4551-4559 */
    }
    if (flash_ready) {
        (void)fwupd_op_run(op); /* selects its writable area and the default again */
    } else {
        op->ok = false; /* the flash did not answer at boot: nothing is touched */
    }
    handoff_finish(op); /* op->done last: the update task may return at once */
    xTaskNotifyGive(upd_task);
}

static const char *const outcome_name[] = {
    [CSL_DELIVERED] = "delivered",
    [CSL_NO_ACK]    = "no ack",
    [CSL_BAD_ACK]   = "bad ack",
    [CSL_TX_FAILED] = "tx failed",
    [CSL_BUSY]      = "channel busy",
    [CSL_NO_KEY]    = "no key",
    [CSL_ABANDONED] = "abandoned",
};

/* After the fact: the transaction is over when this runs. No address in the
 * log, only the MAC seq and our own datagram id. */
void csl_port_report(const struct csl_report *r) {
    link_on_report(r);
    if (r->outcome == CSL_BUSY && !r->final) {
        return; /* a busy listen is retried 20 ms later; the outcome gets its line */
    }
    LOG("tx: datagram %u seq %02x attempt %u: %s%s, %u wake frames, %s train%s",
        (unsigned)r->id,
        r->seq,
        (unsigned)r->attempt,
        outcome_name[r->outcome],
        r->final ? "" : ", will retry",
        (unsigned)r->wake_frames,
        r->anchored ? "short" : "full",
        r->outcome == CSL_TX_FAILED || r->outcome == CSL_NO_KEY
            ? cc110l_tx_verdict() == TXGATE_OK ? "" : " (transmit gate refused)"
            : "");
}

/* --- The receiver's share of a task loop pass -------------------------------
 * Run by the task loop, and from inside flash work (#22, below). bits: the
 * notifications just taken. */
#define RX_SERVICE_BITS (NOTE_GDO0 | NOTE_CRCOK | NOTE_SYNC | NOTE_ACK_CHECK | NOTE_ACK_REARM)

static void rx_service(uint32_t bits) {
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
        rx.in_packet = true;
        rx.sync_us   = timebase_us();
    }
    rx_pump();
    if (bits & NOTE_SYNC) {
        sample_in_packet(); /* after the pump: see fe_sample */
    }
    if (wake.active && (uint32_t)(timebase_us() - wake.last_us) > WAKE_GAP_US) {
        wake_flush();
    }
}

/* --- Flash work while frames arrive (#22, src/mx25r.h) -------------------------
 * Flash writes start only at a quiet moment of the task loop (no frame
 * arriving, no coordinator transaction set up: flash_quiet()), but a
 * program cycle takes up to 10 ms and a sector erase up to 240 ms
 * (mx25r1635f.txt:3337-3341), and frames keep coming. The driver therefore calls back here: between
 * its status polls, and instead of starting a bus transaction while a frame arrives or a frame of
 * ours is on the air. A pass here is the receiver's part of a task loop pass: the FIFO is pumped
 * every tick from the sync edge on, so ack_consider() prepares the Enhanced ACK as in the loop, and
 * complete frames are handled (frame_done(): link, beacon answer, log).
 * Not here: the coordinator (csl_step(), so no transaction starts during
 * flash work), the quiet-moment work, and the notifications for it, which
 * stay in pending_bits for the loop. A flash access made from here would
 * nest: there is none, and `serving` stops the nesting anyway. */
static bool radio_running; /* bring-up done, the task loop runs */
static bool serving;

bool mx25r_port_radio_busy(void) {
    return radio_running && (packet_arriving() || cc110l_tx_expected());
}

bool radio_up(void) {
    return radio_running && gdo_irq_on && cc110l_cal_ok();
}

void mx25r_port_radio_serve(void) {
    watchdog_checkin(WDG_RADIO);
    if (!radio_running || serving) {
        vTaskDelay(1);
        return;
    }
    serving         = true;
    uint32_t bits   = 0;
    uint32_t frames = total.frames, acks = total.ack_sent;
    /* As the loop: wake on a GDO interrupt, else at the next tick. */
    (void)xTaskNotifyWait(0, UINT32_MAX, &bits, 1);
    watchdog_checkin(WDG_RADIO);
    bits |= pending_bits;
    pending_bits = 0;
    rx_service(bits);
    pending_bits |= bits & ~RX_SERVICE_BITS;
    COUNT(flash_serve);
    minute.flash_frames += total.frames - frames;
    total.flash_frames += total.frames - frames;
    minute.flash_acks += total.ack_sent - acks;
    total.flash_acks += total.ack_sent - acks;
    (void)cc110l_tx_expected();
    if (!tx_watch()) {
        radio_reinit();
    }
    serving = false;
}

/* The loop's quiet moment: no packet arriving (its Enhanced ACK must not
 * wait behind a log line or the flash), no transaction on the air. Checked
 * again after each piece of work that may touch the flash, since frames
 * are taken in it (#22). */
static bool quiet(void) {
    return !packet_arriving() && !csl_busy();
}

/* The moment for flash work (#22 bench finding, docs/design.md "Flash work
 * and the coordinator"): no packet arriving, and no transaction set up at
 * all. quiet() is not enough: it lets a job wait in ST_WAIT with its listen
 * 10 ms or more away, csl_step() does not run during flash work, and a
 * sector erase (up to 240 ms) then pushed a short train's start 72 ms late
 * on the bench. Nothing but csl_step() sets a transaction up, so the
 * coordinator stays idle for the whole of the work. */
static bool flash_quiet(void) {
    return !packet_arriving() && csl_idle();
}

#define HEALTH_PERIOD_MS    10000u
#define STATS_PERIOD_MS     60000u
#define RETRY_PERIOD_MS     10000u
#define STORM_REINIT_MIN_MS 1000u

/* Our wire EUI-64: the identity record's EUI-64 reversed (docs/radio.md
 * "Source address tail"; #21). Without an identity the link has no own
 * address: nothing is acknowledged, answered or sent. */
static void link_start(void) {
    const struct store_id *id = identity();
    if (id) {
        uint8_t own[MAC_EUI_LEN];
        for (unsigned i = 0; i < MAC_EUI_LEN; i++) {
            own[i] = id->eui64[MAC_EUI_LEN - 1u - i];
        }
        if (memcmp(own + 5, mac_eui_suffix, 3) == 0) {
            link_init(own);
            cc110l_set_identity(own);
            LOG("link: identity set");
            return;
        }
        LOG("link: identity EUI-64 does not start 00 1b c5; no identity"); /* store_id_valid() */
    } else {
        LOG("link: no bridge identity (identity record): radio link off, nothing sent");
    }
    link_init(NULL);
}

void radio_task_run(void) {
    self = xTaskGetCurrentTaskHandle();
    LOG("spi1: SCK %lu Hz, mode 0", (unsigned long)spi1_sck_hz());

    watchdog_checkin(WDG_RADIO);
    bool flash_ok = flash_boot();
    flash_ready   = flash_ok;
    /* Before the link, before the session may authenticate (#21) and before
     * the net task sets the Ethernet MAC (#39, src/net.c mac_choose()). */
    identity_boot(flash_ok);
    link_start();
    if (flash_ok) {
        network_boot(); /* key and counter reservation from flash, before the radio (#37) */
    } else {
        LOG("network: flash not ready: network record not read: no key, nothing sent");
    }
    gdo_irq_setup();

    while (!radio_configure()) {
        LOG("radio: bring-up failed; retrying in %u s", RETRY_PERIOD_MS / 1000);
        for (unsigned ms = 0; ms < RETRY_PERIOD_MS; ms += 100) {
            watchdog_checkin(WDG_RADIO);
            (void)tx_watch(); /* radio_configure forced IDLE; keep watching */
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    radio_running                   = true;
    TickType_t      next_health     = xTaskGetTickCount() + pdMS_TO_TICKS(HEALTH_PERIOD_MS);
    TickType_t      next_stats      = xTaskGetTickCount() + pdMS_TO_TICKS(STATS_PERIOD_MS);
    TickType_t      last_storm      = xTaskGetTickCount() - pdMS_TO_TICKS(STORM_REINIT_MIN_MS);
    bool            flash_due       = false;
    bool            pair_due        = false;
    bool            reset_due       = false;
    bool            window_was_open = false;
    bool            was_engaged     = false;
    struct pair_led pled;
    pair_led_init(&pled, pair_stats()->paired);
    for (;;) {
        /* The coordinator first: it may send (a wake-up frame per call during
         * a train) and says how long it can wait. */
        (void)now64();
        link_poll((uint32_t)xTaskGetTickCount(), csl_port_random());
        uint32_t   hint = csl_step();
        TickType_t wait = pdMS_TO_TICKS(100);
        if (hint == 0) {
            wait = 0;
        } else if (hint < 2000u) {
            /* Too short for the 1 ms tick: spin, so that a listen or a train
             * starts on its microsecond. */
            delay_us(hint);
            wait = 0;
        } else if (hint != UINT32_MAX && hint / 1000u - 1u < wait) {
            wait = pdMS_TO_TICKS(hint / 1000u - 1u);
        }
        if (wait > 20 && link_pair_engaged((uint32_t)xTaskGetTickCount())) {
            /* The quick flash of the pairing LED (100 ms halves) needs a pass every 20 ms. */
            wait = 20;
        }
        if (wait > 1 && (rx.assembling || rx.in_packet)) {
            wait = 1; /* a frame is arriving: pump the FIFO every tick */
        }
        if (pending_bits) {
            wait = 0; /* taken during a frame of ours or flash work (#22) */
        }

        uint32_t bits = 0;
        /* Block with a timeout, never forever, so the watchdog check-in keeps
         * coming while the channel is quiet. */
        (void)xTaskNotifyWait(0, UINT32_MAX, &bits, wait);
        bits |= pending_bits;
        pending_bits = 0;
        watchdog_checkin(WDG_RADIO);

        TickType_t now = xTaskGetTickCount();
        if (bits & NOTE_STORM) {
            COUNT(storm);
            fe_sample.valid = false; /* edges during a storm are not packet edges (#28) */
            /* Re-init at once, but at most once per STORM_REINIT_MIN_MS; a
             * storm that comes straight back leaves the interrupts off until
             * the next health check re-initialises. */
            if ((uint32_t)(now - last_storm) >= pdMS_TO_TICKS(STORM_REINIT_MIN_MS)) {
                last_storm = now;
                LOG("radio: GDO interrupt storm (> %u edges in %u us); interrupts off, "
                    "re-initialising",
                    STORM_MAX_EDGES,
                    STORM_WINDOW_US);
                radio_reinit();
            }
        }

        rx_service(bits);
        if (lbt_listening) {
            if (csl_busy()) {
                lbt_sample_rssi(); /* a pass inside the listen */
            } else {
                lbt_listening = false; /* the transaction ended without its verdict */
            }
        }

        if (!lbt_seeded && rng_try_bytes(&lbt_state, sizeof lbt_state)) {
            lbt_seeded = true;
        }
        /* The driver's age check runs on the 32-bit microsecond clock, which
         * wraps every 71 minutes; this one cannot be fooled by a wrap. */
        if (now64() - cal_good_at > CC_CAL_MAX_AGE_US) {
            cc110l_cal_expire();
        }

        /* Everything below can wait for a quiet moment. The flash work in
         * it takes frames (#22), so the moment is asked for again after
         * each step that may write or read the flash. Flash work waits for
         * flash_quiet(): no transaction set up at all. While work that may
         * program or erase is due (factory reset, pairing trigger, which
         * may create a network, counter reservation, an update operation),
         * the coordinator is held: it starts nothing new, so it is idle by
         * the end of the transaction in flight, however full its queue.
         * The hold is dropped on the same pass as the work, before the
         * next csl_step(). */
        flash_due |= (bits & NOTE_FLASH) != 0;
        pair_due |= (bits & NOTE_PAIR) != 0;
        reset_due |= (bits & NOTE_RESET) != 0;
        csl_hold(reset_due || pair_due || network_service_due((uint32_t)now) || handoff_pending());
        if (flash_due && flash_quiet()) {
            flash_due = false;
            flash_check();
        }
        if (reset_due && flash_quiet()) {
            factory_reset(); /* restarts the MCU; returns only without flash */
            reset_due = false;
            pair_due  = false;
        }
        if (pair_due && flash_quiet()) {
            pair_due = false;
            pairing_trigger((uint32_t)now);
        }
        if (flash_quiet()) {
            network_service((uint32_t)now); /* writes only if network_service_due() */
        }
        if (handoff_pending() && flash_quiet()) {
            update_op_run();
        }
        csl_hold(reset_due || pair_due || network_service_due((uint32_t)now) || handoff_pending());
        if (quiet()) {
            if ((int32_t)(now - next_health) >= 0) {
                next_health = now + pdMS_TO_TICKS(HEALTH_PERIOD_MS);
                health_check();
            } else if (gdo_irq_on && now64() - cal_at >= (uint64_t)RECAL_PERIOD_MS * 1000u) {
                radio_recalibrate();
            }
            if ((int32_t)(now - next_stats) >= 0) {
                next_stats = now + pdMS_TO_TICKS(STATS_PERIOD_MS);
                wake_flush();
                log_minute();
            }
        }

        /* The pairing LED (#19, owner spec 2026-11-17, amended 2026-11-18;
         * src/pair_led.h): slow flash while the window waits, quick flash
         * during an exchange, lit for 2 s after a pairing, off once the
         * window has closed (after PAIR_WINDOW_MS, or by the button; a
         * pairing does not close it). On the tick clock at this loop's
         * pace (at most 100 ms late); at its 49.7-day wrap one phase is
         * cut short. */
        uint32_t t       = (uint32_t)xTaskGetTickCount();
        bool     open    = link_pair_active(t);
        bool     engaged = open && link_pair_engaged(t);
        board_led(LED_PAIRING, pair_led_level(&pled, open, engaged, pair_stats()->paired, t));
        /* Their log lines wait for a quiet moment, as everything that logs
         * in this loop: an exchange starts on a frame of the target, and the
         * next frame's Enhanced ACK must not wait behind the log mutex. */
        if (!packet_arriving() && !csl_busy()) {
            const struct pair_stats *ps = pair_stats();
            if (engaged && !was_engaged) {
                LOG("pair: target found: pairing (LED lit)");
            } else if (!engaged && was_engaged) {
                LOG("pair: exchange over%s; exchanges %lu: paired %lu, failed %lu, no progress %lu",
                    open ? ", window still open (LED flashing)" : "",
                    (unsigned long)ps->exchanges,
                    (unsigned long)ps->paired,
                    (unsigned long)ps->ex_failed,
                    (unsigned long)ps->ex_idle);
            }
            if (window_was_open && !open) {
                LOG("pair: window closed; paired so far %lu, d/pair sent %lu delivered %lu",
                    (unsigned long)ps->paired,
                    (unsigned long)ps->dpair_sent,
                    (unsigned long)ps->dpair_delivered);
            }
            was_engaged     = engaged;
            window_was_open = open;
        }

        /* Retire a frame of ours that is over, so its airtime never looks
         * current again after the 32-bit clock wraps (src/cc110l.c). */
        (void)cc110l_tx_expected();

        /* Last radio access of every pass: a TX state that is not ours,
         * caused by any access above, is stopped now, not one pass later. */
        if (!tx_watch()) {
            radio_reinit();
        }
    }
}
