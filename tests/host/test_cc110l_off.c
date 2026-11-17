/* Host tests for the radio driver, src/cc110l.c, exactly as the firmware
 * builds it today: RADIO_TX_ALLOWED 0 (#17). Whatever is called, the
 * simulated chip (cc110l_model.h) must never see a transmit strobe or a
 * byte for its TX FIFO. test_cc110l.c tests the driver with the switch on. */
#include "../../src/cc110l.c"

#include "cc110l_model.h"
#include "test.h"

_Static_assert(RADIO_TX_ALLOWED == 0, "this program tests the switched-off driver");

static const uint8_t own[8]    = {0x11, 0x22, 0x33, 0x44, 0x55, 0xC5, 0x1B, 0x00};
static const uint8_t device[8] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xC5, 0x1B, 0x00};
static const uint8_t key_bytes[CCM_KEY_LEN] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

int main(void) {
    struct ccm_key key;
    ccm_set_key(&key, key_bytes);

    /* Everything in place that a transmission needs: the plan's table on the
     * test channel, RX, our identity, a read-back. */
    uint8_t table[CC_NUM_CONFIG_REGS] = {0};
    table[CC_FREQ2]                   = 0x21;
    table[CC_FREQ1]                   = 0x32;
    table[CC_FREQ0]                   = 0x76;
    table[CC_CHANNR]                  = TXGATE_TEST_CHANNEL;
    table[CC_MDMCFG1]                 = 0x22;
    table[CC_MDMCFG0]                 = 0xF8;
    table[CC_MCSM1]                   = 0x3F;
    table[CC_PKTCTRL0]                = 0x05;
    model_init();
    CHECK(cc110l_reset());
    CHECK(cc110l_write_burst(0x00, table, sizeof table));
    CHECK(radio_strobe(CC_SRX, NULL));
    cc110l_set_identity(own);
    CHECK(cc110l_tx_verify_channel());

    /* A wake-up frame and a Data frame of ours: refused as "disabled". */
    uint8_t w[MAC_WAKE_LEN], dst_short[2] = {device[1], device[0]};
    mac_build_wakeup(w, 1, dst_short, 10);
    CHECK_EQ(cc110l_tx_start(w, sizeof w, 0), CC_TX_DISABLED);
    uint8_t f[MAC_FRAME_MAX], body[10] = {0};
    size_t  n = mac_build_data(&key, f, sizeof f, false, 2, device, own, 1, body, sizeof body);
    CHECK(n > 0);
    CHECK_EQ(cc110l_tx_start(f, n, 0), CC_TX_DISABLED);
    size_t left = 0;
    CHECK(cc110l_tx_feed(&left));
    CHECK_EQ(left, 0);

    /* The Enhanced ACK for a frame addressed to us: not prepared, not sent. */
    uint8_t up[MAC_FRAME_MAX], ack_frame[MAC_ACK_LEN];
    CHECK(mac_build_data(&key, up, sizeof up, false, 3, own, device, 1, body, sizeof body) > 0);
    CHECK(mac_ack_for_data(ack_frame, up, own, 0));
    CHECK_EQ(cc110l_ack_prepare(ack_frame, up), CC_TX_DISABLED);
    CHECK(!cc110l_ack_fire_from_isr());

    /* The abort of a failed transmission does not touch the chip either. */
    unsigned bytes = chip.spi_bytes;
    cc110l_tx_abort();
    CHECK_EQ(chip.spi_bytes, bytes);
    CHECK(!cc110l_tx_expected());

    /* The strobes themselves stop the board. */
    volatile bool p = false;
    PANICS(radio_strobe(CC_STX, NULL), p);
    CHECK(p);
    PANICS(radio_strobe(CC_SFTX, NULL), p);
    CHECK(p);
    PANICS(radio_strobe(CC_SFSTXON, NULL), p);
    CHECK(p);
    PANICS(cc110l_write_reg(CC_MCSM1, 0x3B), p);
    CHECK(p);
    PANICS(cc110l_write_reg(0x3E, 0xC0), p);
    CHECK(p);

    CHECK_EQ(chip.stx_count, 0);
    CHECK_EQ(chip.tx_fifo_writes, 0);
    CHECK_EQ(chip.forbidden, 0);
    CHECK_EQ(chip.misuse, 0);
    CHECK_EQ(chip.st, M_RX);
    return TEST_REPORT("test_cc110l_off");
}
