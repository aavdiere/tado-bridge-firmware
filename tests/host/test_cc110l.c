/* Host tests for the transmit functions of the radio driver, src/cc110l.c
 * (#17), with the transmitter switched ON, against a simulated CC110L
 * (cc110l_model.h). The question each test asks is what reaches the chip:
 * an STX strobe, on which carrier by the chip's own registers, with which
 * bytes.
 *
 * The firmware's switch RADIO_TX_ALLOWED is a build option, ON by default
 * (cmake/CMakeLists.txt, owner decision 2026-11-17): this program tests the
 * driver as the default firmware builds it, and tests/host/Makefile
 * compiles it with RADIO_TX_ALLOWED=1. test_cc110l_off.c tests a
 * receive-only build (RADIO_TX_ALLOWED=0).
 *
 * The network channel is a build option too (RADIO_NET_CHANNEL, stage
 * P7.1, src/txgate.h): test_cc110l runs the default channel 16,
 * test_cc110l_ch26 the channel-26 build with its opt-in RADIO_ALLOW_CH26
 * (tests/host/Makefile). In that one every STX must be on channel 26's own
 * carrier, and its neighbours stay refused. */
#include "../../src/cc110l.c"

#include "cc110l_model.h"
#include "test.h"

_Static_assert(RADIO_TX_ALLOWED == 1, "this program tests the driver with the transmitter built in");

#define CH16_HZ 866324157ll /* the test channel 16 (src/txgate.h) */
#define CH26_HZ 868323668ll /* the protected channel */
#if RADIO_NET_CHANNEL == 16 && !TXGATE_NET_ON_PROTECTED
#define TEST_HZ   CH16_HZ
#define OTHER_CH  26u /* a channel the chip may be moved to behind the shadow */
#define PROT_DIST 999000 /* every STX at least this far from channel 26 */
#elif RADIO_NET_CHANNEL == 26 && TXGATE_NET_ON_PROTECTED
#define TEST_HZ   CH26_HZ
#define OTHER_CH  16u
#define PROT_DIST 0 /* channel 26's own carrier; TEST_HZ checks it */
#else
#error "test_cc110l covers the channel 16 and channel 26 builds"
#endif
#define MIN_US  60000000u

static const uint8_t own[8]     = {0x11, 0x22, 0x33, 0x44, 0x55, 0xC5, 0x1B, 0x00};
static const uint8_t device[8]  = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xC5, 0x1B, 0x00};
static const uint8_t another[8] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xC5, 0x1B, 0x00};
static const uint8_t key_bytes[CCM_KEY_LEN] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static struct ccm_key key;

/* The carrier registers of src/radio.c's table, on channel ch; MCSM1 and
 * PKTCTRL0 as there, because the driver's guard looks at them. */
static uint8_t table[CC_NUM_CONFIG_REGS];

static void make_table(unsigned ch) {
    memset(table, 0, sizeof table);
    table[CC_FREQ2]    = 0x21;
    table[CC_FREQ1]    = 0x32;
    table[CC_FREQ0]    = 0x76;
    table[CC_CHANNR]   = (uint8_t)ch;
    table[CC_MDMCFG1]  = 0x22;
    table[CC_MDMCFG0]  = 0xF8;
    table[CC_MCSM1]    = 0x3F;
    table[CC_PKTCTRL0] = 0x05;
}

/* What the radio task's bring-up does: reset, the whole table, calibrate,
 * RX. */
static void bring_up_table(void) {
    uint8_t  fs[3];
    unsigned tries = 0;
    CHECK(cc110l_reset());
    CHECK(cc110l_write_burst(0x00, table, sizeof table));
    CHECK(cc110l_set_pa(RADIO_PA_BRINGUP));
    CHECK_EQ(cc110l_calibrate(fs, &tries), CC_CAL_LOCKED);
    CHECK(radio_strobe(CC_SRX, NULL));
}

static void bring_up(unsigned ch) {
    make_table(ch);
    bring_up_table();
}

static long long dist(long long a, long long b) {
    return a > b ? a - b : b - a;
}

/* Every STX the chip has seen was on the test channel by the chip's own
 * registers, and nowhere near the protected one; no forbidden access and
 * no strobe in a wrong state ever. Called after every step. */
static void check_chip(void) {
    for (unsigned i = 0; i < chip.stx_count && i < MODEL_MAX_STX; i++) {
        CHECK(dist(chip.stx[i].carrier_hz, TEST_HZ) <= 51000);
        CHECK(dist(chip.stx[i].carrier_hz, CH26_HZ) >= PROT_DIST);
        CHECK_EQ(chip.stx[i].pa, RADIO_PA_BRINGUP);
    }
    CHECK_EQ(chip.forbidden, 0);
    CHECK_EQ(chip.misuse, 0);
}

/* The same, and no strobe was lost behind an SIDLE: the scripted tests
 * never leave the chip in a state where the driver has to give one up. */
static void check_clean(void) {
    check_chip();
    CHECK_EQ(chip.dropped, 0);
}

static size_t wake_frame(uint8_t *out) {
    uint8_t dst_short[2] = {device[1], device[0]};
    mac_build_wakeup(out, 7, dst_short, 100);
    return MAC_WAKE_LEN;
}

static size_t data_frame(uint8_t *out, const uint8_t *dst, const uint8_t *src, size_t body_len) {
    uint8_t body[MAC_DATA_MAX_DGRAM];
    for (size_t i = 0; i < sizeof body; i++) {
        body[i] = (uint8_t)(i * 7u + 1u);
    }
    return mac_build_data(&key, out, MAC_FRAME_MAX, false, 9, dst, src, 1, body, body_len);
}

/* Let the frame on the air finish, and tell the driver, as the radio task
 * does once MARCSTATE has left TX with an empty TX FIFO. */
static void finish_frame(void) {
    model_air(300);
    CHECK_EQ(chip.st, M_RX);
    cc110l_tx_done();
}

static void sent_is(unsigned index, const uint8_t *frame, size_t len) {
    CHECK(chip.stx_count == index + 1u);
    if (chip.stx_count != index + 1u) {
        return;
    }
    const struct model_stx *x = &chip.stx[index];
    CHECK_EQ(x->air_n, len + 1u);
    CHECK_EQ(x->air[0], len);
    CHECK_MEM(x->air + 1, frame, len);
    CHECK(dist(x->carrier_hz, TEST_HZ) <= 1);
}

/* --- Nothing without table, identity and gate ---------------------------------- */
static void test_preconditions(void) {
    uint8_t f[MAC_FRAME_MAX];
    size_t  n = wake_frame(f);

    /* Driver state at boot: no table written, no calibration. */
    model_init();
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_NO_LOCK);
    CHECK(!cc110l_tx_verify_channel());
    CHECK_EQ(chip.stx_count, 0);
    CHECK_EQ(chip.tx_fifo_writes, 0);

    /* Table on the test channel, but no identity yet. */
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_REFUSED);
    CHECK_EQ(cc110l_tx_verdict(), TXGATE_NO_IDENTITY);
    CHECK_EQ(chip.stx_count, 0);
    CHECK_EQ(chip.tx_fifo_writes, 0);

    /* A partial table after a reset is not a table: even the calibration
     * fails on it, since the carrier registers cannot be checked. */
    cc110l_set_identity(own);
    CHECK(cc110l_reset());
    CHECK(cc110l_write_burst(CC_CHANNR, table + CC_CHANNR, 6));
    uint8_t  fs[3];
    unsigned tries;
    CHECK_EQ(cc110l_calibrate(fs, &tries), CC_CAL_ERROR);
    CHECK(radio_strobe(CC_SRX, NULL));
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_NO_LOCK);
    CHECK_EQ(chip.stx_count, 0);
    check_clean();
}

/* --- One frame, as approved ----------------------------------------------------- */
static void test_send(void) {
    uint8_t f[MAC_FRAME_MAX];
    size_t  n = wake_frame(f);

    model_init();
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
    CHECK_EQ(chip.st, M_TX);
    CHECK(cc110l_tx_expected());
    size_t left = 99;
    CHECK(cc110l_tx_feed(&left));
    CHECK_EQ(left, 0);
    finish_frame();
    sent_is(0, f, n);
    CHECK(!cc110l_tx_expected());

    /* A Data frame of ours. */
    n = data_frame(f, device, own, 20);
    CHECK(n > 0);
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
    finish_frame();
    sent_is(1, f, n);
    check_clean();
}

/* --- One transmission at a time -------------------------------------------------- */
static void test_busy(void) {
    uint8_t f[MAC_FRAME_MAX];
    size_t  n = wake_frame(f);

    model_init();
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);

    /* A second frame while the first is on the air: refused, and the chip
     * is not touched (SIDLE would cut the first frame). */
    unsigned bytes = chip.spi_bytes;
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_BUSY);
    CHECK_EQ(chip.spi_bytes, bytes);
    CHECK_EQ(chip.frames_cut, 0);
    CHECK_EQ(chip.stx_count, 1);

    /* The frame has left the chip, but nobody has told the driver: its
     * margin still holds the next frame back... */
    model_air(300);
    CHECK_EQ(chip.st, M_RX);
    CHECK(cc110l_tx_expected());
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_BUSY);
    /* ...until cc110l_tx_done(): a wake-up train sends its frames back to
     * back, not 2 ms apart. */
    cc110l_tx_done();
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
    CHECK_EQ(chip.stx_count, 2);

    /* Without the report the margin runs out by itself: airtime of the 12
     * bytes plus preamble, sync, length and CRC, IDLE to TX, 2 ms. */
    model_air(300);
    delay_us((11u + 12u) * 160u + 75u + 2000u);
    CHECK(!cc110l_tx_expected());
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
    finish_frame();
    CHECK_EQ(chip.frames_cut, 0);
    check_clean();
}

/* --- Channels -------------------------------------------------------------------- */
static void test_channels(void) {
    uint8_t f[MAC_FRAME_MAX];
    size_t  n = wake_frame(f);

    model_init();
    for (unsigned ch = 0; ch < 256; ch++) {
        bring_up(ch);
        enum cc110l_tx_result r = cc110l_tx_start(f, n, 0);
        if (ch == TXGATE_TEST_CHANNEL) {
            CHECK_EQ(r, CC_TX_OK);
            finish_frame();
        } else {
            CHECK_EQ(r, CC_TX_REFUSED);
            CHECK_EQ(chip.st, M_RX);
        }
    }
    CHECK_EQ(chip.stx_count, 1);
    CHECK_EQ(chip.tx_fifo_writes, n + 1u);

    /* The protected channel by name, with the verdict (in the channel-26
     * build: a neighbour of it). */
#if TXGATE_NET_ON_PROTECTED
    bring_up(TXGATE_PROTECTED_CHANNEL - 1u);
#else
    bring_up(TXGATE_PROTECTED_CHANNEL);
#endif
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_REFUSED);
    CHECK_EQ(cc110l_tx_verdict(), TXGATE_PROTECTED);

    /* "Channel 16" moved onto 868.3 MHz by the base frequency: 10 channels
     * of 199.951 kHz are 0x13B0 FREQ units up. Refused in every build: as
     * the protected channel, or (channel-26 build) as off the plan. */
    make_table(16u);
    table[CC_FREQ1] = 0x46;
    table[CC_FREQ0] = 0x26;
    bring_up_table();
    CHECK(dist(model_carrier_hz(), CH26_HZ) <= 1);
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_REFUSED);
    CHECK_EQ(cc110l_tx_verdict(), TXGATE_NET_ON_PROTECTED ? TXGATE_OFF_PLAN : TXGATE_PROTECTED);
    CHECK_EQ(chip.stx_count, 1);
    check_clean();
}

/* --- The chip no longer holds what was written ------------------------------------ */
static void test_chip_changed(void) {
    uint8_t f[MAC_FRAME_MAX];
    size_t  n = wake_frame(f);

    /* A reset of the chip that the driver did not do (brown-out): the
     * registers are at their reset values, 800 MHz. */
    model_init();
    bring_up(TXGATE_TEST_CHANNEL);
    model_chip_reset();
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_REFUSED);
    CHECK_EQ(chip.stx_count, 0);
    CHECK_EQ(chip.tx_fifo_writes, 0);
    /* Refused until the table has been written again, even if the chip
     * reads right by then. */
    memcpy(chip.reg, table, sizeof table);
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_REFUSED);
    CHECK(!cc110l_tx_verify_channel());
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
    finish_frame();
    CHECK_EQ(chip.stx_count, 1);

    /* One bit in any register that sets the carrier, in the middle of a
     * train: the next frame does not go out. */
    static const uint8_t regs[] = {CC_CHANNR,  CC_FSCTRL0, CC_FREQ2,  CC_FREQ1,
                                   CC_FREQ0,   CC_MDMCFG1, CC_MDMCFG0};
    for (unsigned i = 0; i < sizeof regs; i++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            bring_up(TXGATE_TEST_CHANNEL);
            unsigned before = chip.stx_count;
            CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
            finish_frame();
            chip.reg[regs[i]] ^= (uint8_t)(1u << bit);
            CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_REFUSED);
            CHECK_EQ(chip.stx_count, before + 1u);
        }
    }

    /* The chip on the protected channel behind a shadow that says 16 (in
     * the channel-26 build: on channel 16 behind a shadow that says 26). */
    bring_up(TXGATE_TEST_CHANNEL);
    unsigned before          = chip.stx_count;
    chip.reg[CC_CHANNR]      = OTHER_CH;
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_REFUSED);
    uint8_t ackf[MAC_ACK_LEN], up[MAC_FRAME_MAX];
    CHECK(data_frame(up, own, device, 10) > 0);
    CHECK(mac_ack_for_data(ackf, up, own, 0));
    CHECK(!cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_REFUSED);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    CHECK_EQ(chip.stx_count, before);
    check_clean();
}

/* --- Frame rules, at the driver --------------------------------------------------- */
static void test_frames(void) {
    uint8_t f[MAC_FRAME_MAX];

    model_init();
    bring_up(TXGATE_TEST_CHANNEL);

    /* Somebody else's source address. */
    size_t n = data_frame(f, device, another, 20);
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_REFUSED);
    CHECK_EQ(cc110l_tx_verdict(), TXGATE_NOT_OUR_SRC);

    /* An Enhanced ACK through the frame path: there is no frame it answers. */
    uint8_t ackf[MAC_ACK_LEN], up[MAC_FRAME_MAX];
    CHECK(data_frame(up, own, device, 10) > 0);
    CHECK(mac_ack_for_data(ackf, up, own, 0));
    CHECK_EQ(cc110l_tx_start(ackf, sizeof ackf, 0), CC_TX_REFUSED);
    CHECK_EQ(cc110l_tx_verdict(), TXGATE_NO_ANSWER);

    /* Not a frame at all. */
    memset(f, 0xAA, sizeof f);
    CHECK_EQ(cc110l_tx_start(f, 40, 0), CC_TX_REFUSED);

    CHECK_EQ(chip.stx_count, 0);
    CHECK_EQ(chip.tx_fifo_writes, 0);
    CHECK_EQ(chip.st, M_RX);
    check_clean();
}

/* --- Enhanced Beacon (#19) ---------------------------------------------------------- */
/* freedo's request and beacon (tado/beacon_test.go:62, 90): the beacon goes
 * out only through cc110l_tx_answer(), as a pairing frame answering that
 * request, on the test channel; the driver's copy is wiped afterwards. */
static void test_beacon(void) {
    static const uint8_t bridge[8] = {0x09, 0xa1, 0xce, 0xfa, 0x07, 0xc5, 0x1b, 0x00};
    static const uint8_t ebr[27]   = {
        0x0b, 0xe8, 0x7b, 0xff, 0xff, 0xff, 0xff, 0x0b, 0xb1, 0xce, 0xfa, 0x07, 0xc5, 0x1b,
        0x00, 0x04, 0x58, 0x14, 0x63, 0xf9, 0x01, 0x47, 0xed, 0x74, 0xca, 0xf1, 0xbd,
    };
    static const uint8_t eb[32] = {
        0x18, 0xec, 0x59, 0xcd, 0xab, 0x0b, 0xb1, 0xce, 0xfa, 0x07, 0xc5,
        0x1b, 0x00, 0x09, 0xa1, 0xce, 0x08, 0x5a, 0x08, 0x64, 0xb7, 0x71,
        0x9c, 0x19, 0xe3, 0x42, 0x85, 0xc9, 0xcd, 0xed, 0x7e, 0xc2,
    };
    model_init();
    bring_up(TXGATE_TEST_CHANNEL);
    cc110l_set_identity(bridge);
    CHECK_EQ(cc110l_tx_start(eb, sizeof eb, TXGATE_PAIRING), CC_TX_REFUSED);
    CHECK_EQ(cc110l_tx_verdict(), TXGATE_NO_ANSWER);
    CHECK_EQ(cc110l_tx_answer(eb, sizeof eb, NULL, TXGATE_PAIRING), CC_TX_REFUSED);
    CHECK_EQ(cc110l_tx_verdict(), TXGATE_NO_ANSWER);
    CHECK_EQ(cc110l_tx_answer(eb, sizeof eb, ebr, 0), CC_TX_REFUSED);
    CHECK_EQ(cc110l_tx_verdict(), TXGATE_NOT_TEST_CHANNEL);
    CHECK_EQ(chip.stx_count, 0);
    CHECK_EQ(chip.tx_fifo_writes, 0);

    CHECK_EQ(cc110l_tx_answer(eb, sizeof eb, ebr, TXGATE_PAIRING), CC_TX_OK);
    CHECK(cc110l_tx_expected());
    size_t left = 99;
    CHECK(cc110l_tx_feed(&left));
    CHECK_EQ(left, 0);
    CHECK(tx.buf[0] == sizeof eb);
    finish_frame();
    sent_is(0, eb, sizeof eb);
    static const uint8_t zero[sizeof tx.buf];
    CHECK_MEM(tx.buf, zero, sizeof tx.buf);

    /* Any other channel: refused before the chip is touched. */
    unsigned writes = chip.tx_fifo_writes;
    for (unsigned ch = 0; ch <= 49; ch++) {
        if (ch == TXGATE_TEST_CHANNEL) {
            continue;
        }
        bring_up(ch);
        CHECK_EQ(cc110l_tx_answer(eb, sizeof eb, ebr, TXGATE_PAIRING), CC_TX_REFUSED);
        CHECK(cc110l_tx_verdict() == TXGATE_NOT_TEST_CHANNEL
              || cc110l_tx_verdict() == TXGATE_PROTECTED);
    }
    CHECK_EQ(chip.tx_fifo_writes, writes);
    cc110l_set_identity(own);
    check_clean();
}

/* --- Enhanced ACK ------------------------------------------------------------------ */
static void test_ack(void) {
    uint8_t ackf[MAC_ACK_LEN], up[MAC_FRAME_MAX], other[MAC_FRAME_MAX];
    CHECK(data_frame(up, own, device, 10) > 0);        /* a device to us */
    CHECK(data_frame(other, another, device, 10) > 0); /* a device to another node */

    model_init();
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK(mac_ack_for_data(ackf, up, own, 0));

    /* Nothing prepared: the interrupt sends nothing. */
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);

    /* Not without a read-back of the frequency registers since the reset. */
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_REFUSED);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);

    /* The normal case, and at most once. */
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    CHECK_EQ(chip.stx_count, 0);
    CHECK_EQ(chip.tx_fifo_writes, 0); /* the chip sees nothing before the interrupt */
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_SENT);
    CHECK(cc110l_tx_expected());
    model_air(300);
    sent_is(0, ackf, sizeof ackf);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    CHECK_EQ(chip.stx_count, 1);
    delay_us(10000);

    /* A frame for another node: no ACK can be prepared for it, whatever
     * the caller passes as the ACK. */
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, other), CC_TX_REFUSED);
    CHECK_EQ(cc110l_tx_verdict(), TXGATE_NOT_FOR_US);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    uint8_t forged[MAC_ACK_LEN];
    mac_build_enh_ack(forged, other[2], device, another, 0);
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(forged, other), CC_TX_REFUSED);
    CHECK_EQ(cc110l_tx_verdict(), TXGATE_NOT_FOR_US);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    /* The right frame, the wrong answer (another sequence number). */
    mac_build_enh_ack(forged, (uint8_t)(up[2] + 1u), device, own, 0);
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(forged, up), CC_TX_REFUSED);
    CHECK_EQ(cc110l_tx_verdict(), TXGATE_WRONG_ANSWER);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    /* A refused prepare also drops the ACK prepared before it. */
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, other), CC_TX_REFUSED);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);

    /* One read-back covers one ACK: the next prepare needs its own. */
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_REFUSED);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);

    /* Cancelled. */
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    cc110l_ack_cancel();
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);

    /* The read-back is from another frame's reception: too old. */
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    delay_us(50001);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    delay_us(49000);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_SENT);
    model_air(300);
    CHECK_EQ(chip.stx_count, 2);
    delay_us(10000);

    /* A frame of ours started by the task drops the prepared ACK. */
    uint8_t w[MAC_WAKE_LEN];
    wake_frame(w);
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    CHECK_EQ(cc110l_tx_start(w, sizeof w, 0), CC_TX_OK);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    finish_frame();
    CHECK_EQ(chip.stx_count, 3);

    /* While a frame of the task is on the air the interrupt sends no ACK
     * over it (the chip cannot have received anything then). */
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    cc110l_ack_cancel();
    CHECK_EQ(cc110l_tx_start(w, sizeof w, 0), CC_TX_OK);
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    CHECK_EQ(chip.frames_cut, 0);
    finish_frame();
    CHECK_EQ(chip.stx_count, 4);

    /* While the interrupt's ACK is on the air the task cannot start a
     * frame over it. */
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_SENT);
    CHECK_EQ(cc110l_tx_start(w, sizeof w, 0), CC_TX_BUSY);
    CHECK_EQ(chip.frames_cut, 0);
    model_air(300);
    CHECK_EQ(chip.stx_count, 5);
    CHECK_MEM(chip.stx[4].air + 1, ackf, sizeof ackf);
    delay_us(10000);

    /* A status byte that wrongly reports a transmit state at the
     * interrupt's SIDLE: no ACK, nothing in the TX FIFO, and the chip is
     * receiving again with what it had received. */
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    unsigned fifo = chip.tx_fifo_writes;
    chip.rxn      = 3;
    chip.lie_once = true;
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    CHECK_EQ(chip.st, M_RX);
    CHECK_EQ(chip.rxn, 3);
    CHECK_EQ(chip.tx_fifo_writes, fifo);
    CHECK_EQ(chip.stx_count, 5);
    CHECK(!cc110l_tx_expected());
    chip.rxn = 0;

    /* A reset by the driver, a re-configuration: the prepared ACK is gone. */
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    CHECK_EQ(chip.stx_count, 5);
    check_clean();
}

/* --- The ACK interrupt fails part way ------------------------------------------------ */
/* The chip as the interrupt finds it: receiving, with the end of the Data
 * frame still in the RX FIFO. */
static void prepared_in_rx(const uint8_t *ackf, const uint8_t *up) {
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    CHECK_EQ(chip.st, M_RX);
    chip.rxn = 5;
}

static void test_ack_failures(void) {
    uint8_t ackf[MAC_ACK_LEN], up[MAC_FRAME_MAX];
    CHECK(data_frame(up, own, device, 10) > 0);
    CHECK(mac_ack_for_data(ackf, up, own, 0));
    model_init();
    bring_up(TXGATE_TEST_CHANNEL);

    /* The chip not ready for the first access: nothing reaches it. */
    prepared_in_rx(ackf, up);
    unsigned writes = chip.tx_fifo_writes;
    chip.so_high    = 1;
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    CHECK_EQ(chip.st, M_RX);
    CHECK_EQ(chip.rxn, 5);
    CHECK_EQ(chip.tx_fifo_writes, writes);

    /* Not ready for the STX access: the interrupt puts the chip back in RX
     * itself, the ACK out of the TX FIFO, the frame still in the RX FIFO. */
    prepared_in_rx(ackf, up);
    chip.so_skip = 1;
    chip.so_high = 1;
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_FAILED);
    CHECK_EQ(chip.st, M_RX);
    CHECK_EQ(chip.txn, 0);
    CHECK_EQ(chip.rxn, 5);
    CHECK(!cc110l_tx_expected());

    /* The bus refused for the STX access: the same. */
    prepared_in_rx(ackf, up);
    chip.isr_skip   = 1;
    chip.isr_refuse = 1;
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_FAILED);
    CHECK_EQ(chip.st, M_RX);
    CHECK_EQ(chip.txn, 0);
    CHECK_EQ(chip.rxn, 5);

    /* Neither the STX access nor the one after it: the chip is IDLE with
     * the ACK in its TX FIFO, and the task is told to re-arm. What the task
     * then does (tx_abort, flush and re-arm) brings it back to RX empty. */
    prepared_in_rx(ackf, up);
    chip.so_skip = 1;
    chip.so_high = 2;
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_REARM);
    CHECK_EQ(chip.st, M_IDLE);
    CHECK_EQ(chip.txn, sizeof ackf + 1u);
    CHECK(!cc110l_tx_expected());
    cc110l_tx_abort();
    CHECK(radio_strobe(CC_SIDLE, NULL));
    CHECK(radio_strobe(CC_SFRX, NULL));
    CHECK(radio_strobe(CC_SRX, NULL));
    CHECK_EQ(chip.st, M_RX);
    CHECK_EQ(chip.txn, 0);
    CHECK_EQ(chip.rxn, 0);

    /* Bus refused twice in a row (STX and recovery): re-arm too. */
    prepared_in_rx(ackf, up);
    chip.isr_skip   = 1;
    chip.isr_refuse = 2;
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_REARM);
    CHECK_EQ(chip.st, M_IDLE);
    cc110l_tx_abort();
    CHECK(radio_strobe(CC_SFRX, NULL));
    CHECK(radio_strobe(CC_SRX, NULL));

    /* No STX in any of this, and the next ACK goes out whole. */
    CHECK_EQ(chip.stx_count, 0);
    chip.rxn = 0;
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_SENT);
    model_air(300);
    sent_is(0, ackf, sizeof ackf);
    check_clean();
}

/* --- Our own frame on the air, hours later -------------------------------------------- */
static void test_tx_expected_wrap(void) {
    uint8_t ackf[MAC_ACK_LEN], up[MAC_FRAME_MAX], v;
    CHECK(data_frame(up, own, device, 10) > 0);
    CHECK(mac_ack_for_data(ackf, up, own, 0));
    model_init();
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_SENT);
    model_air(300);
    CHECK(cc110l_tx_expected());

    /* Nobody looks for 40 minutes (a quiet network). The ACK is long over:
     * a TX state now is not ours and must be flagged. Until 2026-11-17 a
     * signed end-time compare read "still on the air" from 35.8 minutes. */
    for (unsigned m = 0; m < 40; m++) {
        delay_us(MIN_US);
    }
    (void)cc110l_take_tx_state_flag();
    chip.st = M_TX;
    CHECK(cc110l_read_status(CC_RXBYTES, &v));
    CHECK(cc110l_take_tx_state_flag());
    CHECK(!cc110l_tx_expected());
    chip.st = M_RX;

    /* While an ACK is prepared the task leaves tx_active alone: the
     * interrupt may start that ACK at any moment. The interrupt itself
     * clears it. */
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_SENT);
    model_air(300);
    delay_us(20000);
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    CHECK(!cc110l_tx_expected());
    CHECK(tx_active);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_SENT);
    CHECK(cc110l_tx_expected());
    model_air(300);
    delay_us(20000);
    CHECK(!cc110l_tx_expected());
    CHECK(!tx_active);
    check_clean();
}

/* --- A frame longer than the FIFO, underflow, abort --------------------------------- */
static void test_long_and_abort(void) {
    uint8_t f[MAC_FRAME_MAX];
    size_t  n = data_frame(f, device, own, MAC_DATA_MAX_DGRAM);
    CHECK_EQ(n, MAC_FRAME_MAX);

    model_init();
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
    CHECK_EQ(chip.txn, 64);
    size_t left = 0;
    CHECK(cc110l_tx_feed(&left)); /* FIFO full: nothing written, not an error */
    CHECK_EQ(left, n + 1u - 64u);
    for (unsigned guard = 0; left > 0 && guard < 100; guard++) {
        model_air(7);
        CHECK(cc110l_tx_feed(&left));
    }
    CHECK_EQ(left, 0);
    finish_frame();
    sent_is(0, f, n);

    /* The task comes too late: underflow. Only SFTX leaves that state, and
     * an SIDLE before it would have it dropped. */
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
    model_air(65);
    CHECK_EQ(chip.st, M_UNDERFLOW);
    CHECK(!cc110l_tx_feed(&left));
    cc110l_tx_abort();
    CHECK_EQ(chip.st, M_IDLE);
    CHECK_EQ(chip.txn, 0);
    CHECK(!cc110l_tx_expected());
    CHECK_EQ(chip.dropped, 0);
    CHECK(radio_strobe(CC_SRX, NULL));

    /* Abort in the middle of a frame: SIDLE, and SFTX only once the chip
     * is IDLE, a quarter bit later. */
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
    model_air(10);
    cc110l_tx_abort();
    CHECK_EQ(chip.st, M_IDLE);
    CHECK_EQ(chip.txn, 0);
    CHECK_EQ(chip.dropped, 0);
    CHECK_EQ(chip.frames_cut, 1);
    CHECK(radio_strobe(CC_SRX, NULL));

    /* The next frame is whole: nothing of the aborted one leads it. */
    uint8_t w[MAC_WAKE_LEN];
    wake_frame(w);
    CHECK_EQ(cc110l_tx_start(w, sizeof w, 0), CC_TX_OK);
    finish_frame();
    CHECK_EQ(chip.stx_count, 4);
    CHECK_EQ(chip.stx[3].air_n, sizeof w + 1u);
    CHECK_MEM(chip.stx[3].air + 1, w, sizeof w);

    /* An underflow nobody dealt with, found by the next frame long after
     * the margin of the first: that frame is not loaded on top of the
     * rest (its SFTX would be dropped). After the abort the chip is clean. */
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
    model_air(65);
    CHECK_EQ(chip.st, M_UNDERFLOW);
    delay_us(100000);
    CHECK(!cc110l_tx_expected());
    unsigned writes = chip.tx_fifo_writes;
    CHECK_EQ(cc110l_tx_start(w, sizeof w, 0), CC_TX_STATE);
    CHECK_EQ(chip.tx_fifo_writes, writes);
    CHECK_EQ(chip.stx_count, 5);
    size_t none = 99;
    CHECK(cc110l_tx_feed(&none)); /* and nothing is left to feed */
    CHECK_EQ(none, 0);
    cc110l_tx_abort();
    CHECK_EQ(chip.st, M_IDLE);
    CHECK_EQ(chip.txn, 0);
    CHECK(radio_strobe(CC_SRX, NULL));
    (void)cc110l_take_tx_state_flag();
    CHECK_EQ(cc110l_tx_start(w, sizeof w, 0), CC_TX_OK);
    finish_frame();
    CHECK_EQ(chip.stx[5].air_n, sizeof w + 1u);
    CHECK_MEM(chip.stx[5].air + 1, w, sizeof w);
    check_clean();
}

/* --- The guard below the transmit functions ----------------------------------------- */
static void test_guard(void) {
    volatile bool p = false;
    uint8_t       v;

    model_init();
    bring_up(TXGATE_TEST_CHANNEL);
    /* No transmit strobe from outside the transmit functions, switch on or
     * off: the permit is closed between them. */
    PANICS(radio_strobe(CC_STX, NULL), p);
    CHECK(p);
    PANICS(radio_strobe(CC_SFTX, NULL), p);
    CHECK(p);
    PANICS(radio_strobe(CC_SFSTXON, NULL), p);
    CHECK(p);
    PANICS(radio_strobe(CC_SXOFF, NULL), p);
    CHECK(p);
    PANICS(radio_strobe(CC_SPWD, NULL), p);
    CHECK(p);
    /* STX as a status read is not a strobe: MARCSTATE has the same address. */
    PANICS(cc110l_read_status(CC_MARCSTATE, &v), p);
    CHECK(!p);
    /* MCSM1 values that enter TX or FSTXON by themselves. */
    PANICS(cc110l_write_reg(CC_MCSM1, 0x3B), p); /* RXOFF_MODE = TX */
    CHECK(p);
    PANICS(cc110l_write_reg(CC_MCSM1, 0x37), p); /* RXOFF_MODE = FSTXON */
    CHECK(p);
    PANICS(cc110l_write_reg(CC_MCSM1, 0x3E), p); /* TXOFF_MODE = stay in TX */
    CHECK(p);
    PANICS(cc110l_write_reg(CC_MCSM1, 0x3D), p); /* TXOFF_MODE = FSTXON */
    CHECK(p);
    PANICS(cc110l_write_reg(CC_PKTCTRL0, 0x35), p); /* random TX mode */
    CHECK(p);
    PANICS(cc110l_write_reg(CC_PKTCTRL0, 0x06), p); /* infinite length */
    CHECK(p);
    PANICS(cc110l_write_reg(CC_PKTCTRL0, 0x04), p); /* fixed length */
    CHECK(p);
    PANICS(cc110l_write_reg(CC_PKTCTRL0, 0x07), p); /* reserved length mode */
    CHECK(p);
    PANICS(cc110l_write_reg(CC_PKTCTRL0, 0x45), p); /* reset value: variable length */
    CHECK(!p);
    PANICS(cc110l_write_reg(CC_PKTCTRL0, 0x05), p); /* the table's value */
    CHECK(!p);
    bring_up(TXGATE_TEST_CHANNEL); /* those writes cleared the PLL lock */
    PANICS(cc110l_write_reg(0x3E, 0xC0), p); /* PATABLE */
    CHECK(p);
    PANICS(cc110l_write_burst(0x2E, table, 2), p); /* runs past 0x2E */
    CHECK(p);
    CHECK_EQ(chip.stx_count, 0);
    CHECK_EQ(chip.tx_fifo_writes, 0);

    /* A TX state the chip reports is flagged unless a frame of ours is on
     * the air. */
    (void)cc110l_take_tx_state_flag();
    chip.st = M_TX;
    CHECK(cc110l_read_status(CC_RXBYTES, &v));
    CHECK(cc110l_take_tx_state_flag());
    CHECK(!cc110l_take_tx_state_flag());
    chip.st = M_RX;
    uint8_t w[MAC_WAKE_LEN];
    wake_frame(w);
    CHECK_EQ(cc110l_tx_start(w, sizeof w, 0), CC_TX_OK);
    CHECK(cc110l_read_status(CC_RXBYTES, &v));
    CHECK(!cc110l_take_tx_state_flag());
    finish_frame();
    check_clean();
}

/* --- Calibration and PLL lock (#11) ---------------------------------------------- */
static void table_rx_uncalibrated(void) {
    CHECK(cc110l_reset());
    CHECK(cc110l_write_burst(0x00, table, sizeof table));
    CHECK(cc110l_set_pa(RADIO_PA_BRINGUP));
    CHECK(radio_strobe(CC_SRX, NULL));
}

/* Nothing goes out, by either path, and the chip is not touched. */
static void refused_no_lock(const uint8_t *f, size_t n, const uint8_t *ackf, const uint8_t *up) {
    unsigned stx = chip.stx_count, writes = chip.tx_fifo_writes;
    CHECK(!cc110l_cal_ok());
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_NO_LOCK);
    (void)cc110l_tx_verify_channel(); /* the ACK's read-back, where the chip allows it */
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_NO_LOCK);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    CHECK_EQ(chip.stx_count, stx);
    CHECK_EQ(chip.tx_fifo_writes, writes);
}

static void test_calibration(void) {
    uint8_t  f[MAC_FRAME_MAX], ackf[MAC_ACK_LEN], up[MAC_FRAME_MAX], fs[3];
    unsigned tries = 0;
    size_t   n     = wake_frame(f);
    CHECK(data_frame(up, own, device, 10) > 0);
    CHECK(mac_ack_for_data(ackf, up, own, 0));

    /* Table and RX without a calibration: nothing goes out. */
    model_init();
    make_table(TXGATE_TEST_CHANNEL);
    table_rx_uncalibrated();
    refused_no_lock(f, n, ackf, up);

    /* SCAL needs IDLE: from RX the driver does not strobe it at all. */
    CHECK_EQ(cc110l_calibrate(fs, &tries), CC_CAL_ERROR);
    CHECK_EQ(chip.scal_count, 0);
    refused_no_lock(f, n, ackf, up);

    /* Two calibrations without lock, the third locks. */
    CHECK(radio_strobe(CC_SIDLE, NULL));
    chip.cal_fail_left = 2;
    CHECK_EQ(cc110l_calibrate(fs, &tries), CC_CAL_LOCKED);
    CHECK_EQ(tries, 3);
    CHECK_EQ(chip.scal_count, 3);
    CHECK(fs[2] != 0x3F);
    CHECK(cc110l_cal_ok());
    CHECK(radio_strobe(CC_SRX, NULL));
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
    finish_frame();
    CHECK_EQ(chip.stx_count, 1);

    /* Three without lock: refused, and latched for this bring-up. */
    table_rx_uncalibrated();
    CHECK(radio_strobe(CC_SIDLE, NULL));
    chip.cal_fail_left = 3;
    chip.scal_count    = 0;
    CHECK_EQ(cc110l_calibrate(fs, &tries), CC_CAL_NO_LOCK);
    CHECK_EQ(tries, 3);
    CHECK_EQ(chip.scal_count, 3);
    CHECK_EQ(fs[2], 0x3F);
    CHECK(radio_strobe(CC_SRX, NULL));
    refused_no_lock(f, n, ackf, up);
    /* A later calibration that locks does not lift the latch... */
    CHECK(radio_strobe(CC_SIDLE, NULL));
    CHECK_EQ(cc110l_calibrate(fs, &tries), CC_CAL_LOCKED);
    CHECK(radio_strobe(CC_SRX, NULL));
    refused_no_lock(f, n, ackf, up);
    /* ...the next bring-up does. */
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK(cc110l_cal_ok());
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
    finish_frame();
    CHECK_EQ(chip.stx_count, 2);

    /* Any SCAL strobe outside cc110l_calibrate() clears the lock. */
    CHECK(radio_strobe(CC_SIDLE, NULL));
    CHECK(radio_strobe(CC_SCAL, NULL));
    CHECK(radio_strobe(CC_SRX, NULL));
    refused_no_lock(f, n, ackf, up);

    /* So does any configuration write: a new frequency needs a new
     * calibration (cc110l.txt:2741-2742). */
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK(cc110l_write_reg(CC_FSCTRL0, 0));
    refused_no_lock(f, n, ackf, up);

    /* A lock that goes away between prepare and interrupt: no ACK. */
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    cc110l_cal_expire();
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    CHECK_EQ(chip.stx_count, 2);

    /* Too old: refused once CC_CAL_MAX_AGE_US has passed. */
    bring_up(TXGATE_TEST_CHANNEL);
    delay_us(CC_CAL_MAX_AGE_US - 1000u);
    CHECK(cc110l_cal_ok());
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
    finish_frame();
    delay_us(2000u);
    refused_no_lock(f, n, ackf, up);
    CHECK_EQ(chip.stx_count, 3);

    /* The carrier registers changed behind the driver: the calibration
     * fails, and nothing goes out. */
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK(radio_strobe(CC_SIDLE, NULL));
    chip.reg[CC_CHANNR] = OTHER_CH;
    CHECK_EQ(cc110l_calibrate(fs, &tries), CC_CAL_ERROR);
    CHECK(radio_strobe(CC_SRX, NULL));
    refused_no_lock(f, n, ackf, up);
    CHECK_EQ(chip.stx_count, 3);
    check_clean();
}

/* --- Output power ---------------------------------------------------------------------- */
/* The settings the datasheet documents at or below the reset level 0xC6
 * (cc110l.txt Tables 5-14 to 5-17), at 0 dBm and less, and 0xC6 itself. */
static bool documented_low(unsigned v) {
    static const uint8_t ok[] = {0xC6, 0x50, 0x37, 0x26, 0x27, 0x1D, 0x1E, 0x17, 0x0F, 0x03};
    for (unsigned i = 0; i < sizeof ok; i++) {
        if (ok[i] == v) {
            return true;
        }
    }
    return false;
}

static void test_power(void) {
    uint8_t       f[MAC_FRAME_MAX], ackf[MAC_ACK_LEN], up[MAC_FRAME_MAX];
    size_t        n = wake_frame(f);
    volatile bool p = false;
    CHECK(data_frame(up, own, device, 10) > 0);
    CHECK(mac_ack_for_data(ackf, up, own, 0));

    model_init();
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK_EQ(chip.patable, RADIO_PA_BRINGUP);
    CHECK(documented_low(RADIO_PA_BRINGUP));

    /* Every byte value: only the documented low settings reach the chip.
     * Above all the ones numerically below 0xC6 but louder (0xC0 +12 dBm,
     * 0xC2/0xC3/0xC5 +10 dBm), everything above 0xC6, and 0x61-0x6F,
     * which the datasheet does not allow. */
    unsigned accepted = 0;
    for (unsigned v = 0; v < 256; v++) {
        unsigned writes = chip.pa_writes, bytes = chip.spi_bytes;
        bool     ok     = cc110l_set_pa((uint8_t)v);
        CHECK_EQ(ok, documented_low(v));
        if (ok) {
            accepted++;
            CHECK_EQ(chip.patable, v);
            CHECK_EQ(chip.pa_writes, writes + 1u);
        } else {
            CHECK_EQ(chip.pa_writes, writes);
            CHECK_EQ(chip.spi_bytes, bytes);
            /* A refused value also withdraws the old one: nothing is sent
             * until a good value has been written. */
            CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_NO_PA);
            CHECK(!cc110l_tx_verify_channel());
            CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_NO_PA);
        }
    }
    CHECK_EQ(accepted, 10);
    static const uint8_t louder[] = {0xC0, 0xC2, 0xC3, 0xC5, 0xC7, 0xCB, 0xCD, 0xFF, 0x61, 0x6F};
    for (unsigned i = 0; i < sizeof louder; i++) {
        CHECK(!cc110l_set_pa(louder[i]));
    }
    CHECK(chip.patable != 0xC0);
    CHECK_EQ(chip.stx_count, 0);

    /* No PATABLE write outside cc110l_set_pa(), and never PATABLE[1]
     * (burst), whatever the switch; PA_POWER must select PATABLE[0]. */
    PANICS((void)begin(PATABLE, NULL), p);
    CHECK(p);
    PANICS((void)begin(PATABLE | HDR_BURST, NULL), p);
    CHECK(p);
    PANICS((void)begin(PATABLE | HDR_BURST | HDR_READ, NULL), p);
    CHECK(p);
    PANICS(cc110l_write_reg(CC_FREND0, 0x11), p);
    CHECK(p);
    PANICS(cc110l_write_reg(CC_FREND0, 0x10), p);
    CHECK(!p);

    /* After a reset of the chip by the driver PATABLE is 0xC6 again, and
     * nothing is sent until the bring-up has set it. */
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
    finish_frame();
    CHECK(cc110l_reset());
    CHECK_EQ(chip.patable, 0xC6);
    CHECK(cc110l_write_burst(0x00, table, sizeof table));
    uint8_t  fs[3];
    unsigned tries;
    CHECK_EQ(cc110l_calibrate(fs, &tries), CC_CAL_LOCKED);
    CHECK(radio_strobe(CC_SRX, NULL));
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_NO_PA);

    /* PATABLE changed behind the driver: the next frame and the next ACK
     * are refused, and stay refused until it is set again. */
    bring_up(TXGATE_TEST_CHANNEL);
    chip.patable = 0xC0;
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_REFUSED);
    CHECK(!cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_NO_PA);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    chip.patable = RADIO_PA_BRINGUP;
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_NO_PA);
    CHECK(cc110l_set_pa(RADIO_PA_BRINGUP));
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_OK);
    finish_frame();
    CHECK_EQ(chip.stx_count, 2);
    check_clean();
}

/* --- Airtime budget (duty cycle) ----------------------------------------------------- */

/* Re-calibrate as the radio task does every 5 minutes, and let time pass. */
static void wait_minutes(unsigned minutes) {
    for (unsigned m = 0; m < minutes; m++) {
        delay_us(MIN_US);
        if (m % 5u == 4u) {
            bring_up(TXGATE_TEST_CHANNEL);
        }
    }
    bring_up(TXGATE_TEST_CHANNEL);
}

static void test_duty(void) {
    uint8_t f[MAC_FRAME_MAX], ackf[MAC_ACK_LEN], up[MAC_FRAME_MAX];
    size_t  n = wake_frame(f);
    CHECK(data_frame(up, own, device, 10) > 0);
    CHECK(mac_ack_for_data(ackf, up, own, 0));

    /* The airtime of earlier tests leaves the window first. */
    model_init();
    wait_minutes(62);
    CHECK_EQ(cc110l_airtime_used_us(), 0);

    /* Wake-up frames back to back, as a train sends them, until the budget
     * is spent: exactly as many as fit in 30 s. */
    unsigned frames = 0;
    for (;;) {
        if (chip.stx_count >= MODEL_MAX_STX) {
            check_chip();
            chip.stx_count = 0; /* the model logs 64 at a time */
        }
        enum cc110l_tx_result r = cc110l_tx_start(f, n, 0);
        if (r != CC_TX_OK) {
            CHECK_EQ(r, CC_TX_DUTY);
            break;
        }
        finish_frame();
        frames++;
        CHECK(frames < 100000u);
    }
    CHECK_EQ(frames, AIRTIME_BUDGET_US / AIRTIME_FRAME_US(MAC_WAKE_LEN));
    CHECK(cc110l_airtime_used_us() <= AIRTIME_BUDGET_US);
    CHECK(cc110l_airtime_used_us() + AIRTIME_FRAME_US(MAC_WAKE_LEN) > AIRTIME_BUDGET_US);

    /* Refused without touching the chip, by both paths. */
    unsigned stx = chip.stx_count, writes = chip.tx_fifo_writes, bytes = chip.spi_bytes;
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_DUTY);
    CHECK(cc110l_tx_verify_channel());
    bytes = chip.spi_bytes;
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_DUTY);
    CHECK_EQ(chip.spi_bytes, bytes);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
    CHECK_EQ(chip.stx_count, stx);
    CHECK_EQ(chip.tx_fifo_writes, writes);
    CHECK_EQ(chip.st, M_RX);

    /* A reset and new bring-up of the chip do not give the budget back. */
    bring_up(TXGATE_TEST_CHANNEL);
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_DUTY);

    /* Still refused 59 minutes later; after 61, the hour's frames have left
     * the window. */
    wait_minutes(59);
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_DUTY);
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_DUTY);
    wait_minutes(2);
    CHECK_EQ(cc110l_airtime_used_us(), 0);
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_SENT);
    model_air(300);
    CHECK_EQ(cc110l_airtime_used_us(), AIRTIME_FRAME_US(MAC_ACK_LEN));
    delay_us(10000);

    /* A prepared ACK that is never sent stays charged (conservative). */
    CHECK(cc110l_tx_verify_channel());
    CHECK_EQ(cc110l_ack_prepare(ackf, up), CC_TX_OK);
    cc110l_ack_cancel();
    CHECK_EQ(cc110l_airtime_used_us(), 2u * AIRTIME_FRAME_US(MAC_ACK_LEN));

    /* A frame refused by the gate costs nothing. */
    uint8_t bad[MAC_FRAME_MAX];
    size_t  bn = data_frame(bad, device, another, 20);
    CHECK_EQ(cc110l_tx_start(bad, bn, 0), CC_TX_REFUSED);
    CHECK_EQ(cc110l_airtime_used_us(), 2u * AIRTIME_FRAME_US(MAC_ACK_LEN));
    check_clean();
}

/* --- Random use: whatever the caller does, the chip only sends on channel 16 -------- */
static uint32_t rnd_state = 0x1234567u;

static uint32_t rnd(void) {
    rnd_state ^= rnd_state << 13;
    rnd_state ^= rnd_state >> 17;
    rnd_state ^= rnd_state << 5;
    return rnd_state;
}

/* Time passes, and a frame on the air leaves the chip at 160 us per byte. */
static void pass_time(uint32_t us) {
    delay_us(us);
    model_air(us / 160u);
}

/* The chip holds the plan's carrier registers on the test channel, with an
 * offset trim the gate allows. */
static bool chip_on_test_channel(void) {
    int trim = (int8_t)chip.reg[CC_FSCTRL0];
    return chip.reg[CC_FREQ2] == 0x21 && chip.reg[CC_FREQ1] == 0x32 && chip.reg[CC_FREQ0] == 0x76
           && chip.reg[CC_CHANNR] == TXGATE_TEST_CHANNEL && chip.reg[CC_MDMCFG1] == 0x22
           && chip.reg[CC_MDMCFG0] == 0xF8 && trim <= TXGATE_MAX_TRIM && trim >= -TXGATE_MAX_TRIM;
}

/* Every STX of the random test with its time and airtime, for the duty
 * cycle check: the airtime keyed in any hour, by the chip's own STX log. */
#define EMIT_MAX 32768u
static struct {
    uint64_t t;
    uint32_t cost;
} emit[EMIT_MAX];
static unsigned emit_head, emit_n;
static uint64_t emit_sum, emit_max;

static void emitted(size_t len) {
    while (emit_n && model_now_us - emit[emit_head].t >= 3600000000ull) {
        emit_sum -= emit[emit_head].cost;
        emit_head = (emit_head + 1u) % EMIT_MAX;
        emit_n--;
    }
    CHECK(emit_n < EMIT_MAX);
    unsigned i  = (emit_head + emit_n++) % EMIT_MAX;
    emit[i].t    = model_now_us;
    emit[i].cost = AIRTIME_FRAME_US(len);
    emit_sum += emit[i].cost;
    emit_max = emit_sum > emit_max ? emit_sum : emit_max;
    /* The budget, plus what an ACK charged at most 50 ms before its STX
     * can add at the edge of the hour; far below 1 % (36 s). */
    CHECK(emit_sum <= AIRTIME_BUDGET_US + 2u * AIRTIME_FRAME_US(MAC_FRAME_MAX));
    CHECK(emit_sum <= 36000000u);
}

/* A whole wake-up train, up to 250 frames back to back (about 1 s on the
 * air), until the driver refuses one. Returns 1 if that was for airtime. */
static unsigned train_burst(void) {
    uint8_t w[MAC_WAKE_LEN];
    wake_frame(w);
    for (unsigned i = 0; i < 250; i++) {
        if (chip.stx_count >= MODEL_MAX_STX) {
            check_chip();
            chip.stx_count = 0;
        }
        bool                  on_plan = chip_on_test_channel();
        bool                  busy    = cc110l_tx_expected();
        enum cc110l_tx_result r       = cc110l_tx_start(w, sizeof w, 0);
        if (r != CC_TX_OK) {
            return r == CC_TX_DUTY;
        }
        CHECK(on_plan && !busy);
        emitted(sizeof w);
        model_air(300);
        if (chip.st != M_RX) {
            return 0;
        }
        cc110l_tx_done();
    }
    return 0;
}

static void test_random(void) {
    uint8_t f[MAC_FRAME_MAX], ackf[MAC_ACK_LEN], up[MAC_FRAME_MAX];
    model_init();
    bring_up(TXGATE_TEST_CHANNEL);
    unsigned sent = 0, refused = 0, duty = 0;
    for (unsigned step = 0; step < 200000; step++) {
        if (rnd() % 64u == 0) {
            duty += train_burst();
        }
        if (chip.stx_count >= MODEL_MAX_STX) {
            check_chip();
            chip.stx_count = 0;
        }
        unsigned before  = chip.stx_count;
        bool     started = false, legit = false;
        switch (rnd() % 12u) {
            case 0: /* a new table: mostly on the plan, any channel */
                make_table(rnd() % 4u ? TXGATE_TEST_CHANNEL : rnd() % 64u);
                if (rnd() % 8u == 0) {
                    table[CC_FREQ2 + rnd() % 3u] = (uint8_t)rnd();
                }
                if (rnd() % 8u == 0) {
                    table[CC_FSCTRL0] = (uint8_t)rnd();
                }
                bring_up_table();
                break;
            case 1: /* the chip changes behind the driver */
                if (rnd() % 2u) {
                    model_chip_reset();
                } else {
                    chip.reg[CC_CHANNR + rnd() % 6u] ^= (uint8_t)(1u << rnd() % 8u);
                }
                break;
            case 2:
            case 3:
            case 4: { /* a frame from the task */
                size_t   n;
                unsigned kind = rnd() % 4u;
                if (kind == 0) {
                    n     = wake_frame(f);
                    legit = true;
                } else if (kind == 1) {
                    n     = data_frame(f, device, own, 1u + rnd() % MAC_DATA_MAX_DGRAM);
                    legit = true;
                } else if (kind == 2) {
                    n = data_frame(f, device, another, 1u + rnd() % MAC_DATA_MAX_DGRAM);
                } else {
                    n = 2u + rnd() % (MAC_FRAME_MAX - 1u);
                    for (size_t i = 0; i < n; i++) {
                        f[i] = (uint8_t)rnd();
                    }
                    /* Random bytes that happen to be a frame the gate lets
                     * through are not this test's subject. */
                    struct txgate_radio r = shadow_radio();
                    legit = txgate_check_frame(&r, own, f, n, NULL, 0) == TXGATE_OK;
                }
                bool                  on_plan = chip_on_test_channel();
                bool                  busy    = cc110l_tx_expected();
                enum cc110l_tx_result r       = cc110l_tx_start(f, n, 0);
                started                       = r == CC_TX_OK;
                CHECK(!started || (legit && on_plan && !busy));
                duty += r == CC_TX_DUTY;
                if (started) {
                    emitted(n);
                }
                break;
            }
            case 5:
            case 6: { /* an Enhanced ACK: verify, prepare, and the interrupt */
                bool to_us = rnd() % 2u;
                CHECK(data_frame(up, to_us ? own : another, device, 5) > 0);
                mac_build_enh_ack(ackf, up[2], device, to_us || rnd() % 2u ? own : another, 0);
                bool verify   = rnd() % 4u != 0 && cc110l_tx_verify_channel();
                bool on_plan  = chip_on_test_channel();
                bool prepared = cc110l_ack_prepare(ackf, up) == CC_TX_OK;
                CHECK(!prepared || (to_us && verify && on_plan));
                if (rnd() % 4u == 0) {
                    pass_time(rnd() % 100000u);
                }
                started = cc110l_ack_fire_from_isr() == CC_ACK_SENT;
                CHECK(!started || prepared);
                if (started) {
                    emitted(MAC_ACK_LEN);
                }
                break;
            }
            case 7: /* the interrupt with nothing new prepared */
                CHECK_EQ(cc110l_ack_fire_from_isr(), CC_ACK_NONE);
                break;
            case 8:
                model_air(rnd() % 200u);
                break;
            case 9:
                if (chip.st != M_TX) {
                    cc110l_tx_done();
                }
                break;
            case 10:
                pass_time(rnd() % 30000u);
                if (rnd() % 1024u == 0) {
                    pass_time(20u * MIN_US); /* a quiet spell: the window rolls on */
                }
                break;
            default: {
                size_t left;
                if (!cc110l_tx_feed(&left) || rnd() % 16u == 0) {
                    cc110l_tx_abort();
                    (void)radio_strobe(CC_SRX, NULL);
                }
                break;
            }
        }
        CHECK_EQ(chip.stx_count, before + (started ? 1u : 0u));
        if (started) {
            /* The STX of this step, by the chip's registers at that moment. */
            const struct model_stx *x = &chip.stx[chip.stx_count - 1u];
            CHECK(dist(x->carrier_hz, TEST_HZ) <= 51000);
            CHECK(dist(x->carrier_hz, CH26_HZ) >= PROT_DIST);
            sent++;
        } else {
            refused++;
        }
        CHECK_EQ(chip.forbidden, 0);
        CHECK_EQ(chip.misuse, 0);
    }
    check_chip();
    CHECK(sent > 1000);
    CHECK(refused > 1000);
    CHECK(duty > 1000);                       /* the budget ran out, more than once */
    CHECK(emit_max > AIRTIME_BUDGET_US - 1000000u); /* and was used up to its end */
    printf("test_cc110l random: %u sent, %u refused (%u for airtime), max %llu us keyed in an "
           "hour\n",
           sent,
           refused,
           duty,
           (unsigned long long)emit_max);
}

int main(void) {
    ccm_set_key(&key, key_bytes);
    test_preconditions();
    test_send();
    test_busy();
    test_channels();
    test_chip_changed();
    test_frames();
    test_beacon();
    test_ack();
    test_ack_failures();
    test_tx_expected_wrap();
    test_long_and_abort();
    test_guard();
    test_calibration();
    test_power();
    test_duty();
    test_random();
    return TEST_REPORT(TEST_NAME);
}
