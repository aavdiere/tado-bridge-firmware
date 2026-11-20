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

#include <string.h>

/* The record holds the build's channel only (RADIO_NET_CHANNEL, the test
 * channel, src/txgate.h): a record written by an image for another channel
 * is not used, and a new network replaces it only when pairing asks for one
 * (network_replaceable(), stage P7.1). */
_Static_assert(RADIO_CHANNEL == TXGATE_TEST_CHANNEL, "the record holds the test channel only");

#define RETRY_MS 10000u /* after a failed counter-log write */

static bool     active, failing;
static uint32_t limit, retry_ms;
static uint32_t active_seq; /* seq of the record whose key is set */

/* What the boot found (#19): network_create() runs only on NET_NONE, or on
 * NET_OTHER (a sound record for another channel, P7.1). */
static enum { NET_UNKNOWN, NET_NONE, NET_RECORD, NET_OTHER } found = NET_UNKNOWN;
static uint8_t  other_channel;
static uint32_t other_seq;

bool network_active(void) {
    return active;
}

bool network_absent(void) {
    return found == NET_NONE && !active;
}

bool network_replaceable(uint8_t *channel) {
    if (found != NET_OTHER || active) {
        return false;
    }
    if (channel) {
        *channel = other_channel;
    }
    return true;
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
    limit      = lim;
    active     = true;
    active_seq = seq;
    failing    = false;
    LOG("network: record seq %lu, channel %u: key set, frame counter %lu, reserved below %lu",
        (unsigned long)seq,
        n->channel,
        (unsigned long)first,
        (unsigned long)lim);
    return true;
}

void network_boot(void) {
    active = false;
    found  = NET_UNKNOWN;
    if (!link_own()) {
        LOG("network: no identity: network record not read");
        return;
    }
    struct store_net n;
    uint32_t         seq = 0;
    bool             any = true;
    if (store_net_load(&n, &seq)) {
        found = NET_RECORD;
        (void)activate(&n, seq);
    } else if (store_net_probe(&any) && !any) {
        found = NET_NONE;
        LOG("network: no network record: no key, nothing sent");
    } else if (store_net_other_channel(&other_channel, &other_seq)) {
        /* Written by an image for another channel (stage P7.1): never
         * retuned to, never used; replaced only by a pairing request. */
        found = NET_OTHER;
        LOG("network: record seq %lu is for channel %u, this image for channel %u only: not used, "
            "no key, nothing sent; receiving goes on",
            (unsigned long)other_seq,
            other_channel,
            RADIO_CHANNEL);
        LOG("network: a pair request (button or server) replaces it with a new network on channel "
            "%u; devices of the channel %u network must then pair again",
            RADIO_CHANNEL,
            other_channel);
    } else {
        found = NET_RECORD;
        LOG("network: a network record that is damaged or not for this channel, or unreadable: "
            "no key, nothing sent, and no new network over it");
    }
    mbedtls_platform_zeroize(&n, sizeof n);
}

bool network_key(uint8_t key_out[CCM_KEY_LEN]) {
    struct store_net n;
    uint32_t         seq = 0;
    bool ok = active && store_net_load(&n, &seq) && seq == active_seq && n.channel == RADIO_CHANNEL;
    if (ok) {
        memcpy(key_out, n.key, CCM_KEY_LEN);
    }
    mbedtls_platform_zeroize(&n, sizeof n);
    return ok;
}

bool network_create(uint8_t channel, uint8_t key_out[CCM_KEY_LEN]) {
    if (channel != TXGATE_TEST_CHANNEL
        || (channel == TXGATE_PROTECTED_CHANNEL && !TXGATE_NET_ON_PROTECTED)) {
        LOG("network: create on channel %u refused (test channel %u only)",
            channel,
            TXGATE_TEST_CHANNEL);
        return false;
    }
    if (!link_own()) {
        LOG("network: create refused: no identity");
        return false;
    }
    uint8_t old_ch   = 0;
    bool    replaces = network_replaceable(&old_ch);
    if (!network_absent() && !replaces) {
        LOG("network: create refused: a network record exists, or the flash was not read at "
            "boot");
        return false;
    }
    /* From here on, whatever happens, no second create in this boot. */
    found = NET_RECORD;
    if (replaces) {
        /* The record for another channel stays the newest until the new
         * one is complete (src/store.h, store_slot_write()): a cut leaves
         * it as it was, and the next boot finds it again. */
        LOG("network: replacing the record seq %lu for channel %u with a new network on channel %u",
            (unsigned long)other_seq,
            old_ch,
            channel);
    }
    /* Counters of an earlier network stay used: start above them. */
    uint32_t max     = 0;
    bool     any_ctr = false; /* unused: max stays 0 on an empty log */
    if (!store_ctr_highest(&max, &any_ctr)) {
        LOG("network: create failed: counter log unreadable");
        return false;
    }
    struct store_net n = {.pan_id = MAC_PAN_ID, .channel = channel, .counter_floor = max};
    struct store_net got;
    uint32_t         seq = 0;
    if (rng_random(NULL, n.key, sizeof n.key) != 0) {
        mbedtls_platform_zeroize(&n, sizeof n);
        LOG("network: create failed: no random numbers");
        return false;
    }
    /* From here the flash may hold the new record or the old one: run
     * neither until it is read back as a boot would read it. */
    link_clear_key();
    active  = false;
    bool ok = store_net_commit(&n, &got, &seq);
    mbedtls_platform_zeroize(&n, sizeof n);
    if (!ok) {
        LOG("network: create failed: record not written or not read back: no key until the next "
            "boot");
    } else if ((ok = activate(&got, seq))) {
        if (key_out) {
            memcpy(key_out, got.key, CCM_KEY_LEN); /* the key now in use, as read back */
        }
        LOG("network: created on channel %u", channel);
    }
    mbedtls_platform_zeroize(&got, sizeof got);
    return ok;
}

bool network_service_due(uint32_t now_ms) {
    return active && !(failing && (int32_t)(now_ms - retry_ms) < 0)
           && store_counter_due(link_counter(), limit);
}

void network_service(uint32_t now_ms) {
    if (!network_service_due(now_ms)) {
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

bool network_factory_reset(void) {
    link_clear_key(); /* also closes a pairing window */
    active  = false;
    failing = false;
    found   = NET_RECORD; /* no network_create() until the next boot */
    return store_factory_reset();
}
