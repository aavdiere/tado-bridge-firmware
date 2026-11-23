/* Records in external flash: the slotted record format, which the boot
 * state also uses (#24, src/fwimg.c; this file is linked into the
 * bootloader), the identity record (#21, #39), the network record and the
 * counter log (#37), and the factory reset. See store.h and docs/design.md
 * "Persistent storage (#27)". */
#include "store.h"

#include "flash_layout.h"
#include "le.h"
#include "mac.h"
#include "txgate.h"

#include "mbedtls/platform_util.h"

#include <string.h>

_Static_assert(STORE_XFER_MAX <= FLASH_PAGE && FLASH_PAGE % STORE_XFER_MAX == 0,
               "a chunk never crosses a page");
_Static_assert(STORE_REC_HDR + STORE_REC_PAYLOAD + 4u <= FLASH_SECTOR, "record fits a slot");
_Static_assert(STORE_SECTOR == FLASH_SECTOR, "the store's sector is the flash's erase unit");
_Static_assert(FLASH_SECTOR == STORE_CTR_ENTRIES * STORE_CTR_ENTRY, "log entries fill a sector");
_Static_assert(STORE_CTR_LOW < STORE_CTR_AHEAD, "extend before the reservation runs out");

uint32_t store_crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (unsigned b = 0; b < 8; b++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

/* Program src at addr in chunks that never cross a page, then read back. */
static bool program_verified(uint32_t addr, const uint8_t *src, size_t n) {
    uint8_t back[STORE_XFER_MAX];
    while (n) {
        size_t room = FLASH_PAGE - addr % FLASH_PAGE;
        size_t k    = n < STORE_XFER_MAX ? n : STORE_XFER_MAX;
        k           = k < room ? k : room;
        bool ok     = store_port_program(addr, src, k) && store_port_read(addr, back, k)
                      && memcmp(back, src, k) == 0;
        mbedtls_platform_zeroize(back, sizeof back);
        if (!ok) {
            return false;
        }
        addr += (uint32_t)k;
        src += k;
        n -= k;
    }
    return true;
}

/* Erase the sector and check that all of it reads 0xFF. */
static bool erase_verified(uint32_t sector) {
    if (!store_port_erase(sector)) {
        return false;
    }
    uint8_t buf[STORE_XFER_MAX];
    for (uint32_t off = 0; off < FLASH_SECTOR; off += sizeof buf) {
        if (!store_port_read(sector + off, buf, sizeof buf)) {
            return false;
        }
        for (unsigned i = 0; i < sizeof buf; i++) {
            if (buf[i] != 0xFF) {
                return false;
            }
        }
    }
    return true;
}

/* --- Slotted records ------------------------------------------------------- */
static uint8_t rec[STORE_REC_HDR + STORE_REC_PAYLOAD + 4u];

/* One slot: true if it holds a valid record of this kind; payload copied. */
static bool slot_read(uint32_t  addr,
                      uint32_t  magic,
                      uint16_t  version,
                      uint8_t  *payload,
                      uint16_t  len,
                      uint32_t *seq) {
    uint8_t m[4];
    if (!store_port_read(addr, m, sizeof m) || le_get32(m) != magic) {
        return false; /* not ours: read nothing more */
    }
    size_t total = STORE_REC_HDR + len + 4u;
    bool   ok    = store_port_read(addr, rec, total) && le_get32(rec) == magic
                   && le_get16(rec + 4) == version && le_get16(rec + 6) == len
                   && le_get32(rec + STORE_REC_HDR + len) == store_crc32(rec, STORE_REC_HDR + len);
    if (ok) {
        *seq = le_get32(rec + 8);
        memcpy(payload, rec + STORE_REC_HDR, len);
    }
    mbedtls_platform_zeroize(rec, sizeof rec);
    return ok;
}

bool store_slot_load(uint32_t  slot_a,
                     uint32_t  magic,
                     uint16_t  version,
                     uint8_t  *payload,
                     uint16_t  len,
                     uint32_t *seq) {
    if (len > STORE_REC_PAYLOAD) {
        return false;
    }
    uint8_t  pb[STORE_REC_PAYLOAD];
    uint32_t sa = 0, sb = 0;
    bool     va = slot_read(slot_a, magic, version, payload, len, &sa);
    bool     vb = slot_read(slot_a + FLASH_SECTOR, magic, version, pb, len, &sb);
    if (vb && (!va || sb > sa)) {
        memcpy(payload, pb, len);
        sa = sb;
    }
    mbedtls_platform_zeroize(pb, sizeof pb);
    if (va || vb) {
        *seq = sa;
    }
    return va || vb;
}

/* Where a write goes, from what the two slots hold (va, sa: slot A valid,
 * its seq; vb, sb: slot B): the older or invalid slot, slot B on equal
 * seqs, so the newer valid one (slot A on equal seqs, the one
 * store_slot_load() and store_id_load() take) stays untouched. *newest:
 * the higher seq of the two, 0 if neither is valid. False if the seq space
 * is used up (never in practice). */
static bool slot_pick(uint32_t  slot_a,
                      bool      va,
                      uint32_t  sa,
                      bool      vb,
                      uint32_t  sb,
                      uint32_t *target,
                      uint32_t *newest) {
    *target = (!va || (vb && sa < sb)) ? slot_a : slot_a + FLASH_SECTOR;
    *newest = va && vb ? (sa > sb ? sa : sb) : va ? sa : vb ? sb : 0;
    return *newest != 0xFFFFFFFEu;
}

/* Write the record (magic, version, payload, seq newest + 1) into the
 * sector target and read it back. *written: its seq, if not NULL. */
static bool slot_commit(uint32_t       target,
                        uint32_t       magic,
                        uint16_t       version,
                        const uint8_t *payload,
                        uint16_t       len,
                        uint32_t       newest,
                        uint32_t      *written) {
    size_t total = STORE_REC_HDR + len + 4u;
    le_put32(rec, magic);
    le_put16(rec + 4, version);
    le_put16(rec + 6, len);
    le_put32(rec + 8, newest + 1u);
    memcpy(rec + STORE_REC_HDR, payload, len);
    le_put32(rec + STORE_REC_HDR + len, store_crc32(rec, STORE_REC_HDR + len));

    /* Write new, verify, then mark: the magic goes in last. */
    bool ok = erase_verified(target) && program_verified(target + 4u, rec + 4, total - 4u)
              && program_verified(target, rec, 4u);
    mbedtls_platform_zeroize(rec, sizeof rec);
    if (!ok) {
        return false;
    }
    /* The whole record once more, as a boot would read it. */
    uint8_t  scratch[STORE_REC_PAYLOAD];
    uint32_t seq = 0;
    ok           = slot_read(target, magic, version, scratch, len, &seq) && seq == newest + 1u
                   && memcmp(scratch, payload, len) == 0;
    mbedtls_platform_zeroize(scratch, sizeof scratch);
    if (ok && written) {
        *written = seq;
    }
    return ok;
}

bool store_slot_write(uint32_t       slot_a,
                      uint32_t       magic,
                      uint16_t       version,
                      const uint8_t *payload,
                      uint16_t       len,
                      uint32_t      *written) {
    if (len > STORE_REC_PAYLOAD || magic == 0xFFFFFFFFu) {
        return false;
    }
    uint32_t sa = 0, sb = 0, target, newest;
    bool     va, vb;
    {
        /* Its own scope: slot_commit()'s read-back buffer can take its place. */
        uint8_t scratch[STORE_REC_PAYLOAD];
        va = slot_read(slot_a, magic, version, scratch, len, &sa);
        vb = slot_read(slot_a + FLASH_SECTOR, magic, version, scratch, len, &sb);
        mbedtls_platform_zeroize(scratch, sizeof scratch);
    }
    return slot_pick(slot_a, va, sa, vb, sb, &target, &newest)
           && slot_commit(target, magic, version, payload, len, newest, written);
}

/* --- Identity record (#21, #39) -------------------------------------------- */
#define ID_MAC_OFF (STORE_ID_SERIAL_MAX + 8u)
_Static_assert(STORE_ID_LEN_V1 == STORE_ID_SERIAL_MAX + 8u, "identity payload layout, v1");
_Static_assert(STORE_ID_LEN_V2 == STORE_ID_LEN_V1 + 6u && ID_MAC_OFF == STORE_ID_LEN_V1,
               "identity payload layout, v2: the v1 payload, then the MAC");
_Static_assert(STORE_ID_LEN_MAX <= STORE_REC_PAYLOAD, "identity payload fits");
_Static_assert(sizeof(((struct store_id *)0)->eui64) == 8u, "EUI-64");
_Static_assert(sizeof(((struct store_id *)0)->mac) == 6u, "Ethernet MAC");

static bool upper(char c) {
    return c >= 'A' && c <= 'Z';
}

static bool mac_zero(const uint8_t m[6]) {
    uint8_t any = 0;
    for (unsigned i = 0; i < 6u; i++) {
        any |= m[i];
    }
    return any == 0;
}

bool store_id_valid(const struct store_id *id) {
    size_t n = 0;
    while (n <= STORE_ID_SERIAL_MAX && id->serial[n] != '\0') {
        char c = id->serial[n];
        if (!(upper(c) || (n >= 2u && c >= '0' && c <= '9'))) {
            return false;
        }
        n++;
    }
    /* A MAC only with has_mac, and then a unicast one (IEEE 802.3: the
     * I/G bit, bit 0 of the first octet, clear), never all zero. */
    bool mac_ok =
        id->has_mac ? !mac_zero(id->mac) && (id->mac[0] & 0x01u) == 0u : mac_zero(id->mac);
    /* tado's OUI first, in stored order (docs/build.md P4). */
    return n >= 3u && n <= STORE_ID_SERIAL_MAX && id->eui64[0] == 0x00 && id->eui64[1] == 0x1B
           && id->eui64[2] == 0xC5 && mac_ok;
}

bool store_id_pack(const struct store_id *id,
                   uint8_t                p[STORE_ID_LEN_MAX],
                   uint16_t              *version,
                   uint16_t              *len) {
    memset(p, 0, STORE_ID_LEN_MAX);
    *version = 0;
    *len     = 0;
    if (!store_id_valid(id)) {
        return false;
    }
    memcpy(p, id->serial, strlen(id->serial));
    memcpy(p + STORE_ID_SERIAL_MAX, id->eui64, 8u);
    if (id->has_mac) {
        memcpy(p + ID_MAC_OFF, id->mac, 6u);
        *version = STORE_ID_V2;
        *len     = STORE_ID_LEN_V2;
    } else {
        *version = STORE_ID_V1;
        *len     = STORE_ID_LEN_V1;
    }
    return true;
}

bool store_id_unpack(uint16_t version, const uint8_t *p, struct store_id *out) {
    memset(out, 0, sizeof *out);
    if (version != STORE_ID_V1 && version != STORE_ID_V2) {
        return false;
    }
    memcpy(out->serial, p, STORE_ID_SERIAL_MAX); /* serial[16] stays NUL */
    size_t n = strlen(out->serial);
    for (size_t i = n; i < STORE_ID_SERIAL_MAX; i++) {
        if (p[i] != 0) {
            memset(out, 0, sizeof *out);
            return false; /* padding must be zero: one way to write a serial */
        }
    }
    memcpy(out->eui64, p + STORE_ID_SERIAL_MAX, 8u);
    if (version == STORE_ID_V2) {
        out->has_mac = true;
        memcpy(out->mac, p + ID_MAC_OFF, 6u);
    }
    if (!store_id_valid(out)) {
        memset(out, 0, sizeof *out);
        return false;
    }
    return true;
}

/* One identity slot: true if it holds a valid record (magic, header, CRC)
 * of version 1 or 2; its payload, version and seq copied. A record's
 * version field holds one value, so at most one of the two reads finds it;
 * a slot with another magic is read for its first 4 bytes only, as by
 * store_slot_load(). */
static bool id_slot(uint32_t addr, uint8_t p[STORE_ID_LEN_MAX], uint16_t *version, uint32_t *seq) {
    if (slot_read(addr, STORE_ID_MAGIC, STORE_ID_V2, p, STORE_ID_LEN_V2, seq)) {
        *version = STORE_ID_V2;
        return true;
    }
    if (slot_read(addr, STORE_ID_MAGIC, STORE_ID_V1, p, STORE_ID_LEN_V1, seq)) {
        *version = STORE_ID_V1;
        return true;
    }
    return false;
}

bool store_id_load(struct store_id *out, uint32_t *seq, uint16_t *version) {
    uint8_t  pa[STORE_ID_LEN_MAX], pb[STORE_ID_LEN_MAX];
    uint16_t ra = 0, rb = 0;
    uint32_t sa = 0, sb = 0;
    bool     va = id_slot(FLASH_ID_A, pa, &ra, &sa);
    bool     vb = id_slot(FLASH_ID_A + FLASH_SECTOR, pb, &rb, &sb);
    bool     ok = false;
    /* The newest slot of either version, slot A on equal seq (as
     * store_slot_load()); only that one is unpacked. */
    if (vb && (!va || sb > sa)) {
        ok   = store_id_unpack(rb, pb, out);
        *seq = sb;
        ra   = rb;
    } else if (va) {
        ok   = store_id_unpack(ra, pa, out);
        *seq = sa;
    }
    if (!ok) {
        memset(out, 0, sizeof *out);
    }
    if (version) {
        *version = ok ? ra : 0u;
    }
    return ok;
}

/* store_slot_write() with the slot choice over both versions: the newest
 * record of either version stays untouched, and the new seq is above both.
 * store_slot_write() itself sees only records of the version it writes, so
 * a version 2 write next to a single version 1 record would erase that one.
 * The slot rule and the commit steps are store_slot_write()'s
 * (slot_pick(), slot_commit()). */
bool store_id_write(const struct store_id *id, uint32_t *seq) {
    uint8_t  p[STORE_ID_LEN_MAX], scratch[STORE_ID_LEN_MAX];
    uint16_t version, len, ra = 0, rb = 0;
    uint32_t sa = 0, sb = 0, target, newest;
    if (!store_id_pack(id, p, &version, &len)) {
        return false;
    }
    bool va = id_slot(FLASH_ID_A, scratch, &ra, &sa);
    bool vb = id_slot(FLASH_ID_A + FLASH_SECTOR, scratch, &rb, &sb);
    return slot_pick(FLASH_ID_A, va, sa, vb, sb, &target, &newest)
           && slot_commit(target, STORE_ID_MAGIC, version, p, len, newest, seq);
}

/* --- Network record -------------------------------------------------------- */
_Static_assert(STORE_NET_LEN == 2u + 1u + 1u + CCM_KEY_LEN + 4u, "network payload layout");
_Static_assert(STORE_NET_LEN <= STORE_REC_PAYLOAD, "network payload fits");

bool store_net_allowed(const struct store_net *n) {
    return n->channel == TXGATE_TEST_CHANNEL
           && (n->channel != TXGATE_PROTECTED_CHANNEL || TXGATE_NET_ON_PROTECTED)
           && mac_pan_usable(n->pan_id);
}

bool store_net_load(struct store_net *out, uint32_t *seq) {
    uint8_t p[STORE_NET_LEN];
    bool    ok = store_slot_load(FLASH_NET_A, STORE_NET_MAGIC, STORE_NET_VERSION, p, sizeof p, seq);
    if (ok) {
        out->pan_id  = le_get16(p);
        out->channel = p[2];
        memcpy(out->key, p + 4, CCM_KEY_LEN);
        out->counter_floor = le_get32(p + 4 + CCM_KEY_LEN);
        ok                 = p[3] == 0 && store_net_allowed(out);
        if (!ok) {
            mbedtls_platform_zeroize(out, sizeof *out);
        }
    }
    mbedtls_platform_zeroize(p, sizeof p);
    return ok;
}

bool store_net_other_channel(uint8_t *channel, uint32_t *seq) {
    /* Each slot either holds no record (no magic: never written, erased,
     * or a write cut before its commit mark) or a valid one for a usable
     * PAN on the plan; anything else (a damaged record, a read error)
     * gives false, as store_net_probe() then counts it as a network that
     * may still have devices. The newest must be on another channel. */
    uint8_t  p[STORE_NET_LEN];
    uint32_t newest = 0;
    uint8_t  ch     = 0;
    bool     ok = true, any = false;
    for (unsigned i = 0; i < 2u && ok; i++) {
        uint32_t addr = FLASH_NET_A + i * FLASH_SECTOR;
        uint8_t  m[4];
        uint32_t s = 0;
        if (!store_port_read(addr, m, sizeof m)) {
            ok = false;
        } else if (le_get32(m) == STORE_NET_MAGIC) {
            ok = slot_read(addr, STORE_NET_MAGIC, STORE_NET_VERSION, p, sizeof p, &s)
                 && mac_pan_usable(le_get16(p)) && p[3] == 0 && p[2] <= TXGATE_MAX_CHANNEL;
            if (ok && (!any || s > newest)) {
                newest = s;
                ch     = p[2];
                any    = true;
            }
        }
    }
    mbedtls_platform_zeroize(p, sizeof p);
    ok = ok && any && ch != TXGATE_TEST_CHANNEL;
    if (ok) {
        *channel = ch;
        *seq     = newest;
    }
    return ok;
}

bool store_net_probe(bool *any) {
    uint8_t a[4], b[4];
    if (!store_port_read(FLASH_NET_A, a, sizeof a) || !store_port_read(FLASH_NET_B, b, sizeof b)) {
        return false;
    }
    *any = le_get32(a) == STORE_NET_MAGIC || le_get32(b) == STORE_NET_MAGIC;
    return true;
}

bool store_net_write(const struct store_net *n, uint32_t *seq) {
    if (!store_net_allowed(n)) {
        return false;
    }
    uint8_t p[STORE_NET_LEN];
    le_put16(p, n->pan_id);
    p[2] = n->channel;
    p[3] = 0;
    memcpy(p + 4, n->key, CCM_KEY_LEN);
    le_put32(p + 4 + CCM_KEY_LEN, n->counter_floor);
    bool ok = store_slot_write(FLASH_NET_A, STORE_NET_MAGIC, STORE_NET_VERSION, p, sizeof p, seq);
    mbedtls_platform_zeroize(p, sizeof p);
    return ok;
}

bool store_net_commit(const struct store_net *n, struct store_net *out, uint32_t *seq) {
    uint32_t written = 0;
    bool     ok      = store_net_write(n, &written) && store_net_load(out, seq) && *seq == written
                       && out->pan_id == n->pan_id && out->channel == n->channel
                       && memcmp(out->key, n->key, CCM_KEY_LEN) == 0
                       && out->counter_floor == n->counter_floor;
    if (!ok) {
        mbedtls_platform_zeroize(out, sizeof *out);
    }
    return ok;
}

/* --- Counter log ----------------------------------------------------------- */
static const uint32_t ctr_sector[2] = {FLASH_CTR_A, FLASH_CTR_B};

static struct {
    bool     scanned, found;
    uint32_t max;     /* highest valid entry */
    unsigned max_sec; /* sector holding it (meaningful if found): never erased */
    unsigned cur;     /* sector being appended to (after a scan: max_sec, or A) */
    unsigned next;    /* first slot after the last one not blank in cur */
} ctr;

void store_ctr_forget(void) {
    memset(&ctr, 0, sizeof ctr);
}

static bool ctr_scan(void) {
    unsigned last[2] = {0, 0}; /* one past the last slot not blank */
    store_ctr_forget();
    for (unsigned s = 0; s < 2; s++) {
        uint8_t buf[STORE_XFER_MAX];
        for (unsigned i = 0; i < STORE_CTR_ENTRIES; i += sizeof buf / STORE_CTR_ENTRY) {
            if (!store_port_read(ctr_sector[s] + i * STORE_CTR_ENTRY, buf, sizeof buf)) {
                return false;
            }
            for (unsigned k = 0; k < sizeof buf / STORE_CTR_ENTRY; k++) {
                uint32_t v = le_get32(buf + k * STORE_CTR_ENTRY);
                uint32_t w = le_get32(buf + k * STORE_CTR_ENTRY + 4);
                if (v == 0xFFFFFFFFu && w == 0xFFFFFFFFu) {
                    continue;
                }
                last[s] = i + k + 1u;
                if (v == ~w && (!ctr.found || v > ctr.max)) {
                    ctr.found   = true;
                    ctr.max     = v;
                    ctr.max_sec = s;
                    ctr.cur     = s;
                }
            }
        }
    }
    ctr.next    = last[ctr.cur];
    ctr.scanned = true;
    return true;
}

bool store_ctr_highest(uint32_t *max, bool *found) {
    if (!ctr.scanned && !ctr_scan()) {
        return false;
    }
    *found = ctr.found;
    *max   = ctr.found ? ctr.max : 0;
    return true;
}

/* Append one entry above every entry in the log. */
static bool ctr_append(uint32_t value) {
    if (!ctr.scanned && !ctr_scan()) {
        return false;
    }
    if (ctr.found && value <= ctr.max) {
        return false;
    }
    if (ctr.next >= STORE_CTR_ENTRIES) {
        /* Full: continue in the other sector. Until its first entry is
         * written, the highest one stays readable in this one. If the
         * highest entry is in the other sector (every slot here failed after
         * the last change), erasing it would lose the reservation and let a
         * reboot reuse counters: fail closed instead. The coordinator stops
         * at its limit; the next boot's scan returns to the highest entry's
         * sector, which is full, and erases this one. */
        unsigned other = ctr.cur ^ 1u;
        if (ctr.found && other == ctr.max_sec) {
            return false;
        }
        if (!erase_verified(ctr_sector[other])) {
            return false;
        }
        ctr.cur  = other;
        ctr.next = 0;
    }
    uint8_t e[STORE_CTR_ENTRY];
    le_put32(e, value);
    le_put32(e + 4, ~value);
    uint32_t addr = ctr_sector[ctr.cur] + ctr.next * STORE_CTR_ENTRY;
    ctr.next++; /* used even if the write fails: never program a slot twice */
    if (!program_verified(addr, e, sizeof e)) {
        return false;
    }
    ctr.found   = true;
    ctr.max     = value;
    ctr.max_sec = ctr.cur;
    return true;
}

#define CTR_TRIES 3u /* a failed slot is skipped, so try the next one */

static bool ctr_append_retry(uint32_t value) {
    for (unsigned t = 0; t < CTR_TRIES; t++) {
        if (ctr_append(value)) {
            return true;
        }
        if (!ctr.scanned) {
            return false; /* cannot even read the log */
        }
    }
    return false;
}

bool store_counter_boot(uint32_t floor, uint32_t *first, uint32_t *limit) {
    uint32_t max;
    bool     found;
    if (!ctr_scan() || !store_ctr_highest(&max, &found)) {
        return false;
    }
    uint32_t start = found && max > floor ? max : floor;
    if (start > UINT32_MAX - STORE_CTR_AHEAD) {
        return false; /* counter space used up: never send again */
    }
    if (!ctr_append_retry(start + STORE_CTR_AHEAD)) {
        return false;
    }
    *first = start;
    *limit = start + STORE_CTR_AHEAD;
    return true;
}

bool store_counter_due(uint32_t next, uint32_t limit) {
    return next > limit || limit - next < STORE_CTR_LOW;
}

bool store_counter_extend(uint32_t next, uint32_t *limit) {
    if (next > *limit) {
        return false; /* cannot happen: the coordinator stops at the limit */
    }
    if (!store_counter_due(next, *limit)) {
        return true;
    }
    if (next > UINT32_MAX - STORE_CTR_AHEAD || !ctr_append_retry(next + STORE_CTR_AHEAD)) {
        return false;
    }
    *limit = next + STORE_CTR_AHEAD;
    return true;
}

/* --- Factory reset ------------------------------------------------------------- */
bool store_factory_reset(void) {
    /* Network first: see store.h. */
    static const uint32_t order[] = {FLASH_NET_A, FLASH_NET_B, FLASH_CTR_A, FLASH_CTR_B};
    bool                  ok      = true;
    for (unsigned i = 0; ok && i < sizeof order / sizeof order[0]; i++) {
        ok = erase_verified(order[i]);
    }
    store_ctr_forget(); /* the log is read again before its next use */
    return ok;
}
