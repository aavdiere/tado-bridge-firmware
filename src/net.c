/* Net task (#12, #13): ENC28J60 bring-up and service, lwIP (IPv4, DHCP, DNS,
 * ICMP echo), link and address on the router LED, per-minute counters, power
 * LED heartbeat, watchdog check-in.
 * Design: docs/design.md "Tasks" (the net task owns SPI2 and runs lwIP);
 * driver: src/enc28j60.c; lwIP options: src/lwipopts.h (NO_SYS: this task is
 * the only lwIP context). */
#include "net.h"

#include "board.h"
#include "enc28j60.h"
#include "log.h"
#include "rng.h"
#include "watchdog.h"

#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The server's host name or address (docs/server-api.md "Transport": a build
 * or config value). The bench server is 10.42.0.1 (docs/test-bench.md "Bench
 * network"); an address literal resolves without a query. Set with
 * `cmake -DNET_SERVER_HOST=...`. P4 connects to it. */
#ifndef NET_SERVER_HOST
#error "NET_SERVER_HOST not defined (cmake/CMakeLists.txt)"
#endif
#define NET_HOSTNAME "tado-bridge" /* DHCP option 12 */

#define NOTE_INT   (1u << 0)
#define NOTE_STORM (1u << 1)

#define POLL_MS         50u  /* service pass at least this often, also without INT */
#define HEARTBEAT_MS    500u /* power LED toggles: 1 Hz blink, as in P1 */
#define ROUTER_BLINK_MS 250u /* router LED with link but no address: 2 Hz blink */
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
_Static_assert(sizeof mac_addr == ETH_HWADDR_LEN, "Ethernet address");

/* One full frame fits in the first pbuf of a pool chain, so the receive
 * path's header check can read it from p->payload. */
_Static_assert(PBUF_POOL_BUFSIZE >= ETH_PAD_SIZE + ENC_TX_MAX, "a frame fits one pool buffer");

struct counters {
    uint32_t rx_ok, rx_bad, rx_crc, rx_runt, rx_own, rx_overflow, rx_nomem;
    uint32_t tx_ok, tx_err, tx_retry, tx_busy, tx_drop, link_irq, reinit, storm;
};
static struct counters minute, total;

#define COUNT(field, n)      \
    do {                     \
        minute.field += (n); \
        total.field += (n);  \
    } while (0)

static struct netif eth;
static bool         link_up;
static bool         have_addr;
static bool         power_on, router_blink;
static TickType_t   next_beat, next_blink;

/* Server name lookup, once per lease; retried every RETRY_PERIOD_MS. */
static enum { DNS_IDLE, DNS_PENDING, DNS_DONE } dns_state;
static ip_addr_t  server_ip;
static TickType_t dns_t0, dns_retry_at;

/* Power LED heartbeat; router LED: off without link, blinking with link but
 * no address, on with a DHCP lease (prototype-plan.md P3: "the router LED
 * shows link and DHCP"). */
static void leds(void) {
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - next_beat) >= 0) {
        next_beat = now + pdMS_TO_TICKS(HEARTBEAT_MS);
        power_on  = !power_on;
        board_led(LED_POWER, power_on);
    }
    if ((int32_t)(now - next_blink) >= 0) {
        next_blink   = now + pdMS_TO_TICKS(ROUTER_BLINK_MS);
        router_blink = !router_blink;
    }
    board_led(LED_ROUTER, link_up && (have_addr || router_blink));
}

/* Wait without starving the watchdog or the heartbeat. */
static void idle_ms(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += POLL_MS) {
        watchdog_checkin(WDG_NET);
        leds();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

static const char *mac_str(char *b, size_t sz, const uint8_t *m) {
    snprintf(b, sz, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

/* lwIP follows the PHY link: link up starts or reboots DHCP
 * (dhcp_network_changed_link_up(), vendor/lwip/src/core/ipv4/dhcp.c), link
 * down stops address probing; the lease is kept and re-requested on the next
 * link up. */
static void set_link(bool up, uint16_t phstat2) {
    link_up = up;
    if (up) {
        netif_set_link_up(&eth);
    } else {
        netif_set_link_down(&eth);
    }
    leds();
    LOG("eth: link %s (PHSTAT2 %04x: %s duplex, polarity %s)",
        up ? "up" : "down",
        (unsigned)phstat2,
        (phstat2 & PHSTAT2_DPXSTAT) ? "full" : "half",
        (phstat2 & PHSTAT2_PLRITY) ? "reversed" : "ok");
}

static void bring_up(void) {
    char m[18];
    link_up = false;
    netif_set_link_down(&eth); /* nothing is sent while the chip is down */
    leds();
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

/* --- lwIP netif ------------------------------------------------------------ */
/* Transmit one frame. lwIP puts ETH_PAD_SIZE padding bytes in front of the
 * Ethernet header in the first pbuf (struct eth_hdr, lwip/prot/ethernet.h);
 * they are skipped here, not sent. Called only from the net task, never
 * between enc28j60_rx_begin() and enc28j60_rx_end() (rx_frames() hands a
 * frame to lwIP only after rx_end()). */
static err_t eth_output(struct netif *nif, struct pbuf *p) {
    (void)nif;
    if (!link_up) {
        COUNT(tx_drop, 1u);
        return ERR_IF;
    }
    if (!enc28j60_tx_begin()) {
        COUNT(tx_busy, 1u); /* previous frame stuck for ENC_TX_WAIT_MS: cancelled, err counted */
        return ERR_IF;
    }
    LWIP_ASSERT("pad in the first pbuf", p->len >= ETH_PAD_SIZE);
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        const uint8_t *d = q->payload;
        size_t         n = q->len;
        if (q == p) {
            d += ETH_PAD_SIZE;
            n -= ETH_PAD_SIZE;
        }
        enc28j60_tx_write(d, n);
    }
    if (!enc28j60_tx_end()) {
        COUNT(tx_drop, 1u); /* longer than ENC_TX_MAX or shorter than a header */
        return ERR_IF;
    }
    return ERR_OK;
}

static err_t eth_netif_init(struct netif *nif) {
    nif->name[0]    = 'e';
    nif->name[1]    = 'n';
    nif->hwaddr_len = ETH_HWADDR_LEN;
    memcpy(nif->hwaddr, mac_addr, ETH_HWADDR_LEN);
    nif->mtu        = ENC_TX_MAX - ENC_ETH_HDR_LEN; /* 1500 */
    nif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    nif->output     = etharp_output;
    nif->linkoutput = eth_output;
    netif_set_hostname(nif, NET_HOSTNAME);
    return ERR_OK;
}

/* Reads up to RX_PER_PASS frames, each into a pbuf chain, and hands the good
 * ones to lwIP. Returns false if the receive buffer is inconsistent (the chip
 * must be re-initialised). */
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
        n++;

        if (!(rsv & ENC_RSV_RX_OK)) {
            enc28j60_rx_end();
            COUNT(rx_bad, 1u);
            if (rsv & ENC_RSV_CRC) {
                COUNT(rx_crc, 1u);
            }
            continue;
        }
        if (len + 4u < ENC_RX_MIN) {
            enc28j60_rx_end();
            COUNT(rx_bad, 1u); /* runt (enc28j60.txt:1720-1725) */
            COUNT(rx_runt, 1u);
            continue;
        }
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)(len + ETH_PAD_SIZE), PBUF_POOL);
        if (p == NULL) {
            enc28j60_rx_end(); /* dropped: every pool buffer is in use */
            COUNT(rx_nomem, 1u);
            continue;
        }
        for (struct pbuf *q = p; q != NULL; q = q->next) {
            uint8_t *d = q->payload;
            size_t   m = q->len;
            if (q == p) {
                d += ETH_PAD_SIZE;
                m -= ETH_PAD_SIZE;
            }
            enc28j60_rx_read(d, m);
        }
        enc28j60_rx_end();

        const uint8_t *frame = (const uint8_t *)p->payload + ETH_PAD_SIZE;
        if (memcmp(frame + 6, mac_addr, 6) == 0) {
            pbuf_free(p);
            COUNT(rx_own, 1u); /* our own frame looped back: HDLDIS not working */
            continue;
        }
        COUNT(rx_ok, 1u);
        if (eth.input(p, &eth) != ERR_OK) {
            pbuf_free(p);
        }
    }
    *count = n;
    return true;
}

/* --- Address and server name ----------------------------------------------- */
static void dns_found(const char *name, const ip_addr_t *addr, void *arg) {
    (void)arg;
    uint32_t ms = (uint32_t)(xTaskGetTickCount() - dns_t0);
    if (addr == NULL) {
        LOG("dns: %s not resolved after %lu ms; retrying in %u s",
            name,
            (unsigned long)ms,
            RETRY_PERIOD_MS / 1000u);
        dns_state    = DNS_IDLE;
        dns_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(RETRY_PERIOD_MS);
        return;
    }
    server_ip = *addr;
    dns_state = DNS_DONE;
    LOG("dns: %s is %s (%lu ms)", name, ipaddr_ntoa(addr), (unsigned long)ms);
}

static void dns_start(void) {
    dns_t0    = xTaskGetTickCount();
    err_t err = dns_gethostbyname(NET_SERVER_HOST, &server_ip, dns_found, NULL);
    if (err == ERR_OK) {
        dns_state = DNS_DONE; /* an address literal, or still cached */
        LOG("dns: %s is %s (no query needed)", NET_SERVER_HOST, ipaddr_ntoa(&server_ip));
    } else if (err == ERR_INPROGRESS) {
        dns_state = DNS_PENDING;
    } else {
        LOG("dns: lookup of %s not started (err %d); retrying in %u s",
            NET_SERVER_HOST,
            (int)err,
            RETRY_PERIOD_MS / 1000u);
        dns_state    = DNS_IDLE;
        dns_retry_at = dns_t0 + pdMS_TO_TICKS(RETRY_PERIOD_MS);
    }
}

static void log_address(void) {
    const struct dhcp *d = netif_dhcp_data(&eth);
    char               ip[16], mask[16], gw[16];
    ip4addr_ntoa_r(netif_ip4_addr(&eth), ip, sizeof ip);
    ip4addr_ntoa_r(netif_ip4_netmask(&eth), mask, sizeof mask);
    ip4addr_ntoa_r(netif_ip4_gw(&eth), gw, sizeof gw);
    LOG("ip: DHCP lease %s/%s, gateway %s, DNS %s, lease %lu s",
        ip,
        mask,
        gw,
        ipaddr_ntoa(dns_getserver(0)),
        (unsigned long)(d ? d->offered_t0_lease : 0u));
}

static void address_check(void) {
    bool now_addr = dhcp_supplied_address(&eth) != 0u;
    if (now_addr != have_addr) {
        have_addr = now_addr;
        leds();
        if (have_addr) {
            log_address();
            dns_state    = DNS_IDLE;
            dns_retry_at = xTaskGetTickCount();
        } else {
            LOG("ip: address lost (DHCP state %u)",
                (unsigned)(netif_dhcp_data(&eth) ? netif_dhcp_data(&eth)->state : 0u));
        }
    }
    if (have_addr && dns_state == DNS_IDLE && (int32_t)(xTaskGetTickCount() - dns_retry_at) >= 0) {
        dns_start();
    }
}

/* --- Counters -------------------------------------------------------------- */
static void log_minute(void) {
    LOG("eth: 60 s: rx ok %lu, bad %lu (crc %lu, runt %lu), own %lu, overflow %lu, no pbuf %lu; "
        "tx ok %lu, err %lu, late-col retry %lu, busy %lu, drop %lu",
        (unsigned long)minute.rx_ok,
        (unsigned long)minute.rx_bad,
        (unsigned long)minute.rx_crc,
        (unsigned long)minute.rx_runt,
        (unsigned long)minute.rx_own,
        (unsigned long)minute.rx_overflow,
        (unsigned long)minute.rx_nomem,
        (unsigned long)minute.tx_ok,
        (unsigned long)minute.tx_err,
        (unsigned long)minute.tx_retry,
        (unsigned long)minute.tx_busy,
        (unsigned long)minute.tx_drop);
    LOG("eth: 60 s: link %s, link irq %lu, reinit %lu, int storm %lu; total rx ok %lu, tx ok %lu, "
        "tx err %lu",
        link_up ? "up" : "down",
        (unsigned long)minute.link_irq,
        (unsigned long)minute.reinit,
        (unsigned long)minute.storm,
        (unsigned long)total.rx_ok,
        (unsigned long)total.tx_ok,
        (unsigned long)total.tx_err);
    /* The _r forms: ipaddr_ntoa() returns one shared static buffer. */
    char ip[16] = "none", srv[16] = "unresolved";
    if (have_addr) {
        ip4addr_ntoa_r(netif_ip4_addr(&eth), ip, sizeof ip);
    }
    if (dns_state == DNS_DONE) {
        ipaddr_ntoa_r(&server_ip, srv, sizeof srv);
    }
    LOG("ip: 60 s: address %s, server %s is %s", ip, NET_SERVER_HOST, srv);
    minute = (struct counters){0};
}

void net_task_run(void) {
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    next_beat         = xTaskGetTickCount();
    next_blink        = next_beat;
    enc28j60_setup(self, NOTE_INT, NOTE_STORM);
    LOG("eth: SPI2 SCK %lu Hz, mode 0", (unsigned long)enc28j60_sck_hz());

    /* Random numbers before lwIP: lwip_init() already draws the first TCP
     * port (LWIP_RAND, src/lwipopts.h). Fail closed: without a seeded DRBG
     * the bridge stays off the network (#14, docs/design.md "Entropy"). */
    while (!rng_init()) {
        LOG("rng: no random numbers; network stays off, retrying in %u s", RETRY_PERIOD_MS / 1000u);
        idle_ms(RETRY_PERIOD_MS);
    }

    lwip_init();
    netif_add(&eth, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, eth_netif_init, netif_input);
    netif_set_default(&eth);
    netif_set_up(&eth);
    if (dhcp_start(&eth) != ERR_OK) { /* waits for link up */
        fw_panic("dhcp_start", __FILE__, __LINE__);
    }
    LOG("ip: lwIP %s, DHCP as %s, server %s", LWIP_VERSION_STRING, NET_HOSTNAME, NET_SERVER_HOST);

    bring_up();

    TickType_t next_stats = xTaskGetTickCount() + pdMS_TO_TICKS(STATS_PERIOD_MS);
    bool       more       = false;
    for (;;) {
        uint32_t bits = 0;
        /* Never block forever: the watchdog check-in, the heartbeat, lwIP's
         * timers and the EPKTCNT poll (errata #6, enc28j60.c) keep coming
         * without INT. lwIP's shortest timer is TCP's 250 ms (TCP_TMR_INTERVAL),
         * so POLL_MS keeps every timer within 50 ms of its time. */
        (void)xTaskNotifyWait(0, UINT32_MAX, &bits, more ? 0 : pdMS_TO_TICKS(POLL_MS));
        watchdog_checkin(WDG_NET);
        leds();
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

        sys_check_timeouts();
        address_check();

        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(now - next_stats) >= 0) {
            next_stats = now + pdMS_TO_TICKS(STATS_PERIOD_MS);
            log_minute();
        }
    }
}
