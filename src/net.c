/* Net task (#12): ENC28J60 bring-up and service, link on the router LED,
 * per-minute counters, power LED heartbeat, watchdog check-in. lwIP (#13)
 * will take the received frames and replace the bench self-test below.
 * Design: docs/design.md "Tasks" (the net task owns SPI2); driver:
 * src/enc28j60.c. */
#include "net.h"

#include "board.h"
#include "enc28j60.h"
#include "log.h"
#include "watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NOTE_INT   (1u << 0)
#define NOTE_STORM (1u << 1)

#define POLL_MS         50u  /* service pass at least this often, also without INT */
#define HEARTBEAT_MS    500u /* power LED toggles: 1 Hz blink, as in P1 */
#define RETRY_PERIOD_MS 10000u
#define STATS_PERIOD_MS 60000u
#define RX_PER_PASS     16u

#define PHSTAT2_LSTAT   0x0400u /* enc28j60.txt:1296-1298 */
#define PHSTAT2_DPXSTAT 0x0200u /* :1299-1301 */
#define PHSTAT2_PLRITY  0x0020u /* :1303-1305 */

/* PLACEHOLDER MAC address (#12): locally administered (bit 1 of the first
 * octet set) and unicast (bit 0 clear). Replace it with the address from the
 * identity record once #21 exists. Not 02:42:xx, the prefix Docker gives its
 * containers on the bench PC. */
#define MAC0 0x02u
_Static_assert((MAC0 & 0x03u) == 0x02u, "locally administered unicast");
static const uint8_t mac_addr[6] = {MAC0, 0x7A, 0xD0, 0xB1, 0x00, 0x01};

struct counters {
    uint32_t rx_ok, rx_bad, rx_crc, rx_runt, rx_own, rx_overflow;
    uint32_t tx_ok, tx_err, tx_retry, tx_busy, link_irq, reinit, storm;
};
static struct counters minute, total;

#define COUNT(field, n)      \
    do {                     \
        minute.field += (n); \
        total.field += (n);  \
    } while (0)

static bool       link_up;
static bool       power_on;
static TickType_t next_beat;

static void heartbeat(void) {
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - next_beat) >= 0) {
        next_beat = now + pdMS_TO_TICKS(HEARTBEAT_MS);
        power_on  = !power_on;
        board_led(LED_POWER, power_on);
    }
}

/* Wait without starving the watchdog or the heartbeat. */
static void idle_ms(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += POLL_MS) {
        watchdog_checkin(WDG_NET);
        heartbeat();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

static const char *mac_str(char *b, size_t sz, const uint8_t *m) {
    snprintf(b, sz, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

static void set_link(bool up, uint16_t phstat2) {
    link_up = up;
    board_led(LED_ROUTER, up); /* router LED = link, for now (#12) */
    LOG("eth: link %s (PHSTAT2 %04x: %s duplex, polarity %s)",
        up ? "up" : "down",
        (unsigned)phstat2,
        (phstat2 & PHSTAT2_DPXSTAT) ? "full" : "half",
        (phstat2 & PHSTAT2_PLRITY) ? "reversed" : "ok");
}

static void bring_up(void) {
    char m[18];
    board_led(LED_ROUTER, false);
    link_up = false;
    for (;;) {
        struct enc28j60_info info;
        watchdog_checkin(WDG_NET);
        bool ok = enc28j60_init(mac_addr, &info);
        if (info.erevid) {
            LOG("eth: EREVID %02x (%s), PHID1 %04x PHID2 %04x, PHLCON %04x (reset value kept)%s",
                (unsigned)info.erevid,
                info.rev_name,
                (unsigned)info.phid1,
                (unsigned)info.phid2,
                (unsigned)info.phlcon,
                info.soft_reset ? ", reset by SPI command (RESET pin had no effect)" : "");
        }
        if (ok) {
            LOG("eth: configured: MAC %s (placeholder), half duplex (PHCON1 %04x), RX 0000-19ff, "
                "TX 1a00-1fff, filter unicast+broadcast+CRC",
                mac_str(m, sizeof m, mac_addr),
                (unsigned)info.phcon1);
            uint16_t st = enc28j60_phstat2();
            set_link((st & PHSTAT2_LSTAT) != 0u, st);
            return;
        }
        LOG("eth: bring-up failed; retrying in %u s", RETRY_PERIOD_MS / 1000u);
        idle_ms(RETRY_PERIOD_MS);
    }
}

static void reinit(void) {
    COUNT(reinit, 1u);
    bring_up();
}

static void log_minute(void) {
    LOG("eth: 60 s: rx ok %lu, bad %lu (crc %lu, runt %lu), own %lu, overflow %lu; tx ok %lu, "
        "err %lu, late-col retry %lu, busy %lu",
        (unsigned long)minute.rx_ok,
        (unsigned long)minute.rx_bad,
        (unsigned long)minute.rx_crc,
        (unsigned long)minute.rx_runt,
        (unsigned long)minute.rx_own,
        (unsigned long)minute.rx_overflow,
        (unsigned long)minute.tx_ok,
        (unsigned long)minute.tx_err,
        (unsigned long)minute.tx_retry,
        (unsigned long)minute.tx_busy);
    LOG("eth: 60 s: link %s, link irq %lu, reinit %lu, int storm %lu; total rx ok %lu, tx ok %lu, "
        "tx err %lu",
        link_up ? "up" : "down",
        (unsigned long)minute.link_irq,
        (unsigned long)minute.reinit,
        (unsigned long)minute.storm,
        (unsigned long)total.rx_ok,
        (unsigned long)total.tx_ok,
        (unsigned long)total.tx_err);
    minute = (struct counters){0};
}

/* ===== TEST-ONLY: #12 bench self-test ======================================
 * Remove this block, its two call sites in net_task_run() and its log lines
 * once #13 (lwIP) passes its own bench test. No build flag (project rule).
 *
 * Transmit: one broadcast frame every SELFTEST_PERIOD_MS while the link is
 * up, ethertype 0x88B5 (IEEE 802 local experimental ethertype 1). Chosen over
 * a gratuitous ARP because no protocol on the laptop reacts to it: it claims
 * no IP address (10.42.0.0/24 is NetworkManager's DHCP pool, and a fake ARP
 * claim could collide with a later DHCP lease), it is never routed or NATed,
 * and it shows up as one RX packet per frame in the laptop's interface
 * counters, which can be read without root. Payload: a 16-byte ASCII tag, a
 * sequence number and the uptime in ms, both big-endian.
 *
 * Receive: the first SELFTEST_RX_LOG_MAX frames per minute are logged with
 * their addresses and ethertype; ARP requests also with the IP addresses,
 * so `ping 10.42.0.200` on the laptop (ARP broadcasts, no root) shows here. */
#define SELFTEST_PERIOD_MS  10000u
#define SELFTEST_RX_LOG_MAX 5u
static uint32_t selftest_seq;
static unsigned selftest_rx_logged;

static void selftest_send(void) {
    static const char tag[16] = {
        't', 'a', 'd', 'o', '-', 'b', 'r', 'i', 'd', 'g', 'e', ' ', '#', '1', '2', ' '};
    uint8_t  f[ENC_ETH_HDR_LEN + sizeof tag + 8];
    uint32_t up = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    memset(f, 0xFF, 6);
    memcpy(f + 6, mac_addr, 6);
    f[12] = 0x88;
    f[13] = 0xB5;
    memcpy(f + ENC_ETH_HDR_LEN, tag, sizeof tag);
    uint8_t *p = f + ENC_ETH_HDR_LEN + sizeof tag;
    for (int i = 0; i < 4; i++) {
        p[i]     = (uint8_t)(selftest_seq >> (24 - 8 * i));
        p[4 + i] = (uint8_t)(up >> (24 - 8 * i));
    }
    if (!enc28j60_tx_begin()) {
        COUNT(tx_busy, 1u);
        return;
    }
    enc28j60_tx_write(f, sizeof f);
    if (enc28j60_tx_end()) {
        selftest_seq++;
    }
}

static void selftest_log_rx(const uint8_t *h, size_t got, uint16_t len, uint32_t rsv) {
    if (selftest_rx_logged >= SELFTEST_RX_LOG_MAX || got < ENC_ETH_HDR_LEN) {
        return;
    }
    selftest_rx_logged++;
    char     d[18], s[18];
    unsigned type = (unsigned)(h[12] << 8 | h[13]);
    if (type == 0x0806u && got >= 42u) {
        LOG("eth: rx len %u dst %s src %s ARP op %u: who has %u.%u.%u.%u tell %u.%u.%u.%u",
            (unsigned)len,
            mac_str(d, sizeof d, h),
            mac_str(s, sizeof s, h + 6),
            (unsigned)(h[20] << 8 | h[21]),
            h[38],
            h[39],
            h[40],
            h[41],
            h[28],
            h[29],
            h[30],
            h[31]);
    } else {
        LOG("eth: rx len %u dst %s src %s type %04x status %08lx",
            (unsigned)len,
            mac_str(d, sizeof d, h),
            mac_str(s, sizeof s, h + 6),
            type,
            (unsigned long)rsv);
    }
}
/* ===== end TEST-ONLY ======================================================= */

/* Reads up to RX_PER_PASS frames. Returns false if the receive buffer is
 * inconsistent (the chip must be re-initialised). */
static bool rx_frames(unsigned *count) {
    unsigned n = 0;
    while (n < RX_PER_PASS) {
        uint32_t rsv = 0;
        uint16_t len = enc28j60_rx_begin(&rsv);
        if (len == 0u) {
            break;
        }
        if (len == ENC_RX_CORRUPT) {
            *count = n;
            return false;
        }
        uint8_t h[42]; /* Ethernet header + ARP body, for the log */
        size_t  got = len < sizeof h ? len : sizeof h;
        enc28j60_rx_read(h, got);
        enc28j60_rx_end();
        n++;

        if (!(rsv & ENC_RSV_RX_OK)) {
            COUNT(rx_bad, 1u);
            if (rsv & ENC_RSV_CRC) {
                COUNT(rx_crc, 1u);
            }
        } else if (len + 4u < ENC_RX_MIN) {
            COUNT(rx_bad, 1u); /* runt (enc28j60.txt:1720-1725) */
            COUNT(rx_runt, 1u);
        } else {
            COUNT(rx_ok, 1u);
        }
        if (got >= 12u && memcmp(h + 6, mac_addr, 6) == 0) {
            COUNT(rx_own, 1u); /* our own frame looped back: HDLDIS not working */
        }
        selftest_log_rx(h, got, len, rsv); /* TEST-ONLY */
    }
    *count = n;
    return true;
}

void net_task_run(void) {
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    next_beat         = xTaskGetTickCount();
    enc28j60_setup(self, NOTE_INT, NOTE_STORM);
    LOG("eth: SPI2 SCK %lu Hz, mode 0", (unsigned long)enc28j60_sck_hz());
    bring_up();
    LOG("eth: self-test (#12, test only): broadcast ethertype 88b5 every %u s while the link is up",
        SELFTEST_PERIOD_MS / 1000u);

    TickType_t next_stats = xTaskGetTickCount() + pdMS_TO_TICKS(STATS_PERIOD_MS);
    TickType_t next_test  = xTaskGetTickCount() + pdMS_TO_TICKS(SELFTEST_PERIOD_MS);
    bool       more       = false;
    for (;;) {
        uint32_t bits = 0;
        /* Never block forever: the watchdog check-in, the heartbeat and the
         * EPKTCNT poll (errata #6, enc28j60.c) keep coming without INT. */
        (void)xTaskNotifyWait(0, UINT32_MAX, &bits, more ? 0 : pdMS_TO_TICKS(POLL_MS));
        watchdog_checkin(WDG_NET);
        heartbeat();
        more = false;

        if (bits & NOTE_STORM) {
            COUNT(storm, 1u);
            LOG("eth: INT interrupt storm; EXTI2 masked, re-initialising");
            reinit();
            continue;
        }

        struct enc28j60_events ev;
        enc28j60_poll(&ev);
        if (ev.bus_error) {
            LOG("eth: chip check failed (EREVID changed, RXEN lost or MII stuck); re-initialising");
            reinit();
            continue;
        }
        if (ev.link_irq) {
            COUNT(link_irq, 1u);
        }
        if (ev.link_changed) {
            set_link(ev.link_up, enc28j60_phstat2());
        }
        if (ev.rx_overflow) {
            COUNT(rx_overflow, 1u);
        }
        COUNT(tx_ok, ev.tx_ok);
        COUNT(tx_err, ev.tx_err);
        COUNT(tx_retry, ev.tx_retry);
        if (ev.tx_err) {
            LOG("eth: transmission aborted or cancelled (ESTAT %02x, TSV bits 16-31 %04x)",
                (unsigned)ev.tx_estat,
                (unsigned)ev.tx_status);
        }

        unsigned n = 0;
        if (ev.pending && !rx_frames(&n)) {
            reinit();
            continue;
        }
        more = (n == RX_PER_PASS);
        enc28j60_rearm();

        TickType_t now = xTaskGetTickCount();
        if (link_up && (int32_t)(now - next_test) >= 0) { /* TEST-ONLY */
            next_test = now + pdMS_TO_TICKS(SELFTEST_PERIOD_MS);
            selftest_send();
        }
        if ((int32_t)(now - next_stats) >= 0) {
            next_stats = now + pdMS_TO_TICKS(STATS_PERIOD_MS);
            log_minute();
            selftest_rx_logged = 0; /* TEST-ONLY */
        }
    }
}
