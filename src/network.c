/* The bridge's network (#37). See network.h, docs/design.md "Persistent
 * storage (#27)". */
#include "network.h"

#include "link.h"
#include "log.h"
#include "mac.h"
#include "radio.h"
#include "rng.h"
#include "store.h"
#include "txgate.h"

#include "mbedtls/platform_util.h"

_Static_assert(RADIO_CHANNEL == TXGATE_TEST_CHANNEL, "the record holds the test channel only");

#define RETRY_MS 10000u /* after a failed counter-log write */

static bool     active, failing;
static uint32_t limit, retry_ms;

bool network_active(void) {
    return active;
}

/* Reserve counters and set the key of a network read back from flash. */
static bool activate(const struct store_net *n, uint32_t seq) {
    uint32_t first, lim;
    if (n->channel != RADIO_CHANNEL) {
        LOG("network: record seq %lu is for channel %u, radio on %u: not used",
            (unsigned long)seq,
            n->channel,
            RADIO_CHANNEL);
        return false;
    }
    if (!store_counter_boot(n->counter_floor, &first, &lim)) {
        LOG("network: counter reservation not written: no key, nothing sent");
        return false;
    }
    link_set_key(n->key, first, lim);
    limit   = lim;
    active  = true;
    failing = false;
    LOG("network: record seq %lu, channel %u: key set, frame counter %lu, reserved below %lu",
        (unsigned long)seq,
        n->channel,
        (unsigned long)first,
        (unsigned long)lim);
    return true;
}

void network_boot(void) {
    active = false;
    if (!link_own()) {
        LOG("network: no identity: network record not read");
        return;
    }
    struct store_net n;
    uint32_t         seq = 0;
    if (store_net_load(&n, &seq)) {
        (void)activate(&n, seq);
    } else {
        LOG("network: no valid network record: no key, nothing sent");
    }
    mbedtls_platform_zeroize(&n, sizeof n);
}

bool network_create(uint8_t channel) {
    if (channel != TXGATE_TEST_CHANNEL || channel == TXGATE_PROTECTED_CHANNEL) {
        LOG("network: create on channel %u refused (test channel %u only)",
            channel,
            TXGATE_TEST_CHANNEL);
        return false;
    }
    if (!link_own()) {
        LOG("network: create refused: no identity");
        return false;
    }
    /* Counters of an earlier network stay used: start above them. */
    uint32_t max   = 0;
    bool     found = false;
    if (!store_ctr_highest(&max, &found)) {
        LOG("network: create failed: counter log unreadable");
        return false;
    }
    struct store_net n  = {.pan_id = MAC_PAN_ID, .channel = channel, .counter_floor = max};
    bool             ok = rng_random(NULL, n.key, sizeof n.key) == 0;
    if (!ok) {
        LOG("network: create failed: no random numbers");
    } else if (!(ok = store_net_write(&n))) {
        LOG("network: create failed: network record not written");
    }
    mbedtls_platform_zeroize(&n, sizeof n);
    if (!ok) {
        return false;
    }
    /* Use what a boot would read. */
    link_clear_key();
    active       = false;
    uint32_t seq = 0;
    ok           = store_net_load(&n, &seq) && activate(&n, seq);
    mbedtls_platform_zeroize(&n, sizeof n);
    if (ok) {
        LOG("network: created on channel %u", channel);
    }
    return ok;
}

void network_service(uint32_t now_ms) {
    if (!active || (failing && (int32_t)(now_ms - retry_ms) < 0)) {
        return;
    }
    uint32_t lim = limit;
    if (store_counter_extend(link_counter(), &lim)) {
        if (lim != limit) {
            limit = lim;
            link_set_counter_limit(lim);
        }
        failing = false;
        return;
    }
    /* The coordinator stops at the old limit until a write succeeds. */
    failing  = true;
    retry_ms = now_ms + RETRY_MS;
    LOG("network: counter reservation write failed (counter %lu, limit %lu); retry in %u s",
        (unsigned long)link_counter(),
        (unsigned long)limit,
        RETRY_MS / 1000u);
}
