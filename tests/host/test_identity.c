/* Host tests for the identity record (#21, src/store.c): validation and
 * payload layout, loading from a stock image, slot choice, bit flips, a
 * newest record that is refused, and a power cut at every step of a write
 * (as the provisioning tool, tools/identity/, performs it). Version 2 with
 * the Ethernet MAC (#39): its layout and checks, version 1 and 2 records
 * side by side (slot choice and seq over both, what a firmware from before
 * #39 still finds), and the power cuts for every pair of old and new
 * version. The MACs here are made up (locally administered), not a real
 * bridge's. The flash model
 * lets programs and erases reach the identity sectors only, the tool's
 * rule; the firmware's own driver refuses them altogether (src/mx25r.c:
 * they lie in none of the writable areas of src/flash_layout.h). Checked after every cut: the
 * identity is the old one or the new one, never another; nothing outside
 * the identity sectors changes; no byte is programmed twice without an
 * erase. */
#include "store.h"

#include "flash_layout.h"

#include "test.h"

#include <stdint.h>

_Static_assert(FLASH_SLOTS_END <= FLASH_ID_A && FLASH_ID_B + FLASH_SECTOR <= FLASH_RECORDS_BEGIN
                   && FLASH_RECORDS_END <= FLASH_BOOT_BEGIN,
               "the firmware's writable areas exclude the identity sectors");

#define ID_BEGIN FLASH_ID_A
#define ID_END   (FLASH_ID_B + FLASH_SECTOR)

static uint8_t mem[FLASH_SIZE];

static struct {
    long     cut_in; /* program/erase steps until the cut; 0: none armed */
    bool     dead;
    unsigned ops;
    unsigned slot_a_body_reads;
    uint32_t rnd;
    uint64_t trace; /* FNV-1a of every flash operation, see trace_op() */
} fm;

/* test_trace(): taken from the code before #34 proposal 8. */
#define TRACE_ID_DIGEST 0x78e40a6aacc1dd4eull

static uint32_t rnd(void) {
    uint32_t x = fm.rnd ? fm.rnd : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    fm.rnd = x;
    return x;
}

/* Fold bytes into fm.trace (64-bit FNV-1a). */
static void trace_bytes(const void *p, size_t n) {
    const uint8_t *b = p;
    while (n--) {
        fm.trace = (fm.trace ^ *b++) * 0x100000001B3u;
    }
}

/* Every read, program and erase, with its address, length and the bytes
 * programmed, in call order (test_trace()). */
static void trace_op(char kind, uint32_t addr, const uint8_t *src, size_t n) {
    uint32_t len = (uint32_t)n;
    trace_bytes(&kind, 1);
    trace_bytes(&addr, sizeof addr);
    trace_bytes(&len, sizeof len);
    if (src) {
        trace_bytes(src, n);
    }
}

static bool cut_now(void) {
    fm.ops++;
    if (fm.cut_in > 0 && --fm.cut_in == 0) {
        fm.dead = true;
        return true;
    }
    return false;
}

bool store_port_read(uint32_t addr, uint8_t *dst, size_t n) {
    CHECK(addr < FLASH_SIZE && n <= FLASH_SIZE - addr);
    trace_op('R', addr, NULL, n);
    if (fm.dead) {
        return false;
    }
    if (addr < FLASH_ID_A + FLASH_SECTOR && addr + n > FLASH_ID_A + 4u) {
        fm.slot_a_body_reads++;
    }
    memcpy(dst, mem + addr, n);
    return true;
}

bool store_port_program(uint32_t addr, const uint8_t *src, size_t n) {
    CHECK(addr >= ID_BEGIN && addr + n <= ID_END);
    CHECK(n >= 1 && n <= STORE_XFER_MAX && addr % FLASH_PAGE + n <= FLASH_PAGE);
    trace_op('P', addr, src, n);
    if (fm.dead) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        CHECK_EQ(mem[addr + i], 0xFF);
    }
    if (cut_now()) {
        size_t k = rnd() % (n + 1u);
        for (size_t i = 0; i < k; i++) {
            mem[addr + i] &= src[i];
        }
        if (k < n) {
            mem[addr + k] &= (uint8_t)(src[k] | rnd());
        }
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        mem[addr + i] &= src[i];
    }
    return true;
}

bool store_port_erase(uint32_t addr) {
    CHECK(addr >= ID_BEGIN && addr + FLASH_SECTOR <= ID_END);
    CHECK_EQ(addr % FLASH_SECTOR, 0);
    trace_op('E', addr, NULL, 0);
    if (fm.dead) {
        return false;
    }
    if (cut_now()) {
        for (uint32_t i = 0; i < FLASH_SECTOR; i++) {
            switch (rnd() % 4u) {
                case 0:
                    mem[addr + i] = 0xFF;
                    break;
                case 1:
                    mem[addr + i] |= (uint8_t)rnd();
                    break;
                case 2:
                    mem[addr + i] &= (uint8_t)rnd();
                    break;
                default:
                    break;
            }
        }
        return false;
    }
    memset(mem + addr, 0xFF, FLASH_SECTOR);
    return true;
}

static void reboot(void) {
    fm.dead   = false;
    fm.cut_in = 0;
}

/* The bench unit's stock image (docs/design.md, #37 notes): data in
 * 0x100000-0x100FFF, identity slot B blank. */
static void flash_stock(void) {
    memset(mem, 0xFF, sizeof mem);
    static const uint32_t used[][2] = {
        {0x000000, 0x03E000}, {0x080000, 0x0BF000}, {0x100000, 0x101000}, {0x102000, 0x103000}};
    for (unsigned r = 0; r < 4; r++) {
        for (uint32_t a = used[r][0]; a < used[r][1]; a++) {
            mem[a] = (uint8_t)rnd();
        }
    }
}

static uint32_t outside_sum(void) {
    uint32_t h = 2166136261u;
    for (uint32_t a = 0; a < FLASH_SIZE; a++) {
        if (a < ID_BEGIN || a >= ID_END) {
            h = (h ^ mem[a]) * 16777619u;
        }
    }
    return h;
}

/* Test values only: not a real bridge. */
static struct store_id make_id(unsigned tag) {
    struct store_id id = {.eui64 = {0x00, 0x1B, 0xC5, 0x07, 0x00, 0x00, 0x00, 0x00}};
    snprintf(id.serial, sizeof id.serial, "IB%010u", 1000u + tag);
    id.eui64[7] = (uint8_t)tag;
    return id;
}

/* The same with a made-up, locally administered unicast MAC (version 2). */
static struct store_id make_id_mac(unsigned tag) {
    struct store_id      id     = make_id(tag);
    static const uint8_t mac[6] = {0x06, 0x00, 0x5E, 0x39, 0x00, 0x00};
    memcpy(id.mac, mac, 6);
    id.mac[5]  = (uint8_t)tag;
    id.has_mac = true;
    return id;
}

static bool same_id(const struct store_id *a, const struct store_id *b) {
    return strcmp(a->serial, b->serial) == 0 && memcmp(a->eui64, b->eui64, 8) == 0
           && a->has_mac == b->has_mac && memcmp(a->mac, b->mac, 6) == 0;
}

/* What a firmware from before #39 loads: version 1 only, by
 * store_slot_load(), as its store_id_load() did. */
static bool load_v1_only(struct store_id *out, uint32_t *seq) {
    uint8_t p[STORE_ID_LEN_V1];
    return store_slot_load(FLASH_ID_A, STORE_ID_MAGIC, STORE_ID_V1, p, sizeof p, seq)
           && store_id_unpack(STORE_ID_V1, p, out);
}

static uint16_t rec_version(uint32_t addr) {
    return (uint16_t)(mem[addr + 4] | mem[addr + 5] << 8);
}

/* A record written straight into the model, header fields as given. */
static void plant(uint32_t       addr,
                  uint32_t       magic,
                  uint16_t       version,
                  uint16_t       len,
                  const uint8_t *payload,
                  uint32_t       seq) {
    uint8_t r[STORE_REC_HDR + STORE_REC_PAYLOAD + 4u];
    memset(mem + addr, 0xFF, FLASH_SECTOR);
    for (unsigned i = 0; i < 4; i++) {
        r[i]     = (uint8_t)(magic >> (8u * i));
        r[8 + i] = (uint8_t)(seq >> (8u * i));
    }
    r[4] = (uint8_t)version;
    r[5] = (uint8_t)(version >> 8);
    r[6] = (uint8_t)len;
    r[7] = (uint8_t)(len >> 8);
    memcpy(r + STORE_REC_HDR, payload, len);
    uint32_t crc = store_crc32(r, STORE_REC_HDR + len);
    for (unsigned i = 0; i < 4; i++) {
        r[STORE_REC_HDR + len + i] = (uint8_t)(crc >> (8u * i));
    }
    memcpy(mem + addr, r, STORE_REC_HDR + len + 4u);
}

/* --- Tests -------------------------------------------------------------------- */
static void test_valid(void) {
    struct store_id id = make_id(1);
    CHECK(store_id_valid(&id));
    static const char *const good[] = {"IB0123456789", "IBX", "RU01234567", "VA1234567890ABCD"};
    for (unsigned i = 0; i < sizeof good / sizeof good[0]; i++) {
        memset(id.serial, 0, sizeof id.serial);
        strcpy(id.serial, good[i]);
        CHECK(store_id_valid(&id));
    }
    static const char *const bad[] = {
        "", "IB", "ib0123456789", "I10123456789", "1B0123456789", "IB 0123456789", "IB012345678-"};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memset(id.serial, 0, sizeof id.serial);
        strcpy(id.serial, bad[i]);
        CHECK(!store_id_valid(&id));
    }
    /* 17 characters: no terminator inside the field. */
    memset(id.serial, 'A', sizeof id.serial);
    CHECK(!store_id_valid(&id));

    id          = make_id(1);
    id.eui64[2] = 0xC6;
    CHECK(!store_id_valid(&id));
    id          = make_id(1);
    id.eui64[0] = 0x02;
    CHECK(!store_id_valid(&id));

    /* Layout: serial, zero padded to 16, then the EUI-64 as stored. */
    uint8_t  p[STORE_ID_LEN_MAX];
    uint16_t ver = 0, len = 0;
    id = make_id(3);
    CHECK(store_id_pack(&id, p, &ver, &len));
    CHECK_EQ(ver, STORE_ID_V1);
    CHECK_EQ(len, STORE_ID_LEN_V1);
    CHECK_MEM(p, "IB0000001003\0\0\0\0", 16);
    CHECK_MEM(p + 16, id.eui64, 8);
    struct store_id back;
    CHECK(store_id_unpack(STORE_ID_V1, p, &back));
    CHECK(same_id(&back, &id));
    CHECK(!back.has_mac);
    /* Anything after the serial's end is refused, not ignored. */
    p[13] = 'X';
    CHECK(!store_id_unpack(STORE_ID_V1, p, &back));
    CHECK_EQ(back.serial[0], 0);
    p[13] = 0;
    p[15] = 1;
    CHECK(!store_id_unpack(STORE_ID_V1, p, &back));
    p[15] = 0;
    p[16] = 0x01;
    CHECK(!store_id_unpack(STORE_ID_V1, p, &back));
    /* An invalid identity is not packed. */
    id.serial[0] = 'i';
    CHECK(!store_id_pack(&id, p, &ver, &len));
    CHECK_EQ(p[0], 0);
    CHECK_EQ(ver, 0);
    CHECK_EQ(len, 0);

    /* Version 2 (#39): the version 1 payload, then the MAC as sent. */
    id = make_id_mac(3);
    CHECK(store_id_valid(&id));
    CHECK(store_id_pack(&id, p, &ver, &len));
    CHECK_EQ(ver, STORE_ID_V2);
    CHECK_EQ(len, STORE_ID_LEN_V2);
    CHECK_MEM(p, "IB0000001003\0\0\0\0", 16);
    CHECK_MEM(p + 16, id.eui64, 8);
    CHECK_MEM(p + 24, id.mac, 6);
    CHECK(store_id_unpack(STORE_ID_V2, p, &back));
    CHECK(same_id(&back, &id));
    CHECK(back.has_mac);
    /* The same bytes read as version 1 are the identity without the MAC. */
    CHECK(store_id_unpack(STORE_ID_V1, p, &back));
    CHECK(!back.has_mac);
    CHECK_EQ(back.mac[0], 0);
    /* No other version. */
    CHECK(!store_id_unpack(0, p, &back));
    CHECK(!store_id_unpack(3, p, &back));
    CHECK_EQ(back.serial[0], 0);
    /* The MAC: unicast, not all zero; a MAC without has_mac is refused. */
    struct store_id m = make_id_mac(3);
    m.mac[0] |= 0x01u; /* group address */
    CHECK(!store_id_valid(&m));
    CHECK(!store_id_pack(&m, p, &ver, &len));
    memset(m.mac, 0xFF, 6); /* broadcast */
    CHECK(!store_id_valid(&m));
    memset(m.mac, 0, 6);
    CHECK(!store_id_valid(&m));
    m        = make_id_mac(3);
    m.mac[0] = 0x00; /* globally administered unicast is fine */
    CHECK(store_id_valid(&m));
    m         = make_id_mac(3);
    m.has_mac = false;
    CHECK(!store_id_valid(&m));
    /* Refused when unpacked as well. */
    m = make_id_mac(3);
    CHECK(store_id_pack(&m, p, &ver, &len));
    p[24] |= 0x01u;
    CHECK(!store_id_unpack(STORE_ID_V2, p, &back));
    CHECK_EQ(back.serial[0], 0);
    memset(p + 24, 0, 6);
    CHECK(!store_id_unpack(STORE_ID_V2, p, &back));
}

static void test_basic(void) {
    flash_stock();
    reboot();
    uint32_t        sum = outside_sum();
    struct store_id got;
    uint32_t        seq = 0;

    /* The stock data in slot A is no record: only its magic is read. */
    fm.slot_a_body_reads = 0;
    CHECK(!store_id_load(&got, &seq, NULL));
    CHECK_EQ(fm.slot_a_body_reads, 0);
    CHECK_EQ(got.serial[0], 0);

    struct store_id a = make_id(1), b = make_id(2), c = make_id(3);
    CHECK(store_id_write(&a, &seq));
    CHECK_EQ(seq, 1);
    CHECK_EQ(mem[FLASH_ID_A], 0x49); /* "IDN1": slot A was the invalid one */
    CHECK(store_id_load(&got, &seq, NULL));
    CHECK_EQ(seq, 1);
    CHECK(same_id(&got, &a));
    CHECK(store_id_write(&b, NULL));
    CHECK_EQ(mem[FLASH_ID_B], 0x49);
    CHECK(store_id_load(&got, &seq, NULL));
    CHECK_EQ(seq, 2);
    CHECK(same_id(&got, &b));
    CHECK(store_id_write(&c, NULL));
    CHECK(store_id_load(&got, &seq, NULL));
    CHECK_EQ(seq, 3);
    CHECK(same_id(&got, &c));
    CHECK_EQ(outside_sum(), sum);

    /* One flipped bit anywhere in the newest record (slot A): the older one
     * in slot B is taken. */
    for (unsigned off = 0; off < STORE_REC_HDR + STORE_ID_LEN_V1 + 4u; off++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            uint8_t keep = mem[FLASH_ID_A + off];
            mem[FLASH_ID_A + off] ^= (uint8_t)(1u << bit);
            CHECK(store_id_load(&got, &seq, NULL));
            CHECK_EQ(seq, 2);
            CHECK(same_id(&got, &b));
            mem[FLASH_ID_A + off] = keep;
        }
    }
    /* With only one record, a flip anywhere leaves no identity. */
    flash_stock();
    CHECK(store_id_write(&a, NULL));
    for (unsigned off = 0; off < STORE_REC_HDR + STORE_ID_LEN_V1 + 4u; off++) {
        uint8_t keep = mem[FLASH_ID_A + off];
        mem[FLASH_ID_A + off] ^= 0x04;
        CHECK(!store_id_load(&got, &seq, NULL));
        mem[FLASH_ID_A + off] = keep;
    }
    CHECK(store_id_load(&got, &seq, NULL));
    CHECK(same_id(&got, &a));

    /* Refused before touching the flash. */
    struct store_id bad = make_id(4);
    bad.eui64[1]        = 0;
    unsigned ops        = fm.ops;
    CHECK(!store_id_write(&bad, NULL));
    CHECK_EQ(fm.ops, ops);
}

/* The newest record has a good CRC but content store_id_unpack() refuses,
 * or another version or length: the bridge has no identity; the older
 * valid record is not used in its place. */
static void test_newest_refused(void) {
    flash_stock();
    reboot();
    struct store_id a = make_id(1), got;
    uint32_t        seq;
    CHECK(store_id_write(&a, NULL));
    uint8_t  p[STORE_ID_LEN_MAX];
    uint16_t ver, len;
    CHECK(store_id_pack(&a, p, &ver, &len));
    p[17] = 0x1C; /* EUI-64 00 1c ...: refused */
    CHECK(store_slot_write(FLASH_ID_A, STORE_ID_MAGIC, STORE_ID_V1, p, STORE_ID_LEN_V1, NULL));
    CHECK(!store_id_load(&got, &seq, NULL));
    CHECK_EQ(got.eui64[0], 0);

    /* The same for a version 2 record with a group MAC (good CRC) above a
     * valid version 1 record: no identity, the version 1 record is not used
     * in its place. */
    flash_stock();
    CHECK(store_id_write(&a, NULL)); /* slot A, seq 1 */
    struct store_id am = make_id_mac(1);
    CHECK(store_id_pack(&am, p, &ver, &len));
    p[24] |= 0x01u;
    plant(FLASH_ID_B, STORE_ID_MAGIC, STORE_ID_V2, STORE_ID_LEN_V2, p, 2u);
    CHECK(!store_id_load(&got, &seq, NULL));
    CHECK_EQ(got.serial[0], 0);

    /* A record of another version, or a length that does not match its
     * version, in the newer seq: not an identity record of ours at all, so
     * the valid v1 record in the other slot is used. */
    flash_stock();
    CHECK(store_id_write(&a, NULL)); /* slot A, seq 1 */
    CHECK(store_id_pack(&a, p, &ver, &len));
    static const uint16_t other[][2] = {{3u, STORE_ID_LEN_V1},
                                        {3u, STORE_ID_LEN_V2},
                                        {0u, STORE_ID_LEN_V1},
                                        {STORE_ID_V1, 20u},
                                        {STORE_ID_V1, STORE_ID_LEN_V2},
                                        {STORE_ID_V2, STORE_ID_LEN_V1}};
    for (unsigned i = 0; i < sizeof other / sizeof other[0]; i++) {
        plant(FLASH_ID_B, STORE_ID_MAGIC, other[i][0], other[i][1], p, 5u);
        uint16_t v = 9;
        CHECK(store_id_load(&got, &seq, &v));
        CHECK(same_id(&got, &a));
        CHECK_EQ(seq, 1);
        CHECK_EQ(v, STORE_ID_V1);
    }
    /* The same, planted with the right fields: it is taken (the planting
     * itself is right). */
    plant(FLASH_ID_B, STORE_ID_MAGIC, STORE_ID_V1, STORE_ID_LEN_V1, p, 5u);
    CHECK(store_id_load(&got, &seq, NULL));
    CHECK_EQ(seq, 5);
    CHECK(store_id_pack(&am, p, &ver, &len));
    plant(FLASH_ID_B, STORE_ID_MAGIC, STORE_ID_V2, STORE_ID_LEN_V2, p, 6u);
    CHECK(store_id_load(&got, &seq, &ver));
    CHECK_EQ(seq, 6);
    CHECK_EQ(ver, STORE_ID_V2);
    CHECK(same_id(&got, &am));
    /* A network record's magic in the identity slots is not an identity. */
    flash_stock();
    plant(FLASH_ID_A, STORE_NET_MAGIC, STORE_ID_V1, STORE_ID_LEN_V1, p, 1u);
    CHECK(!store_id_load(&got, &seq, NULL));
    plant(FLASH_ID_A, STORE_NET_MAGIC, STORE_ID_V2, STORE_ID_LEN_V2, p, 1u);
    CHECK(!store_id_load(&got, &seq, NULL));
}

/* Version 1 and 2 side by side (#39): the upgrade of the bench unit's
 * record (version 1 in slot A, slot B blank), the same with the version 1
 * record in slot B, the newest slot of either version wins, the slot
 * choice and seq of a write look at both versions, and what a firmware
 * from before #39 (version 1 only) still finds. */
static void test_versions(void) {
    struct store_id a = make_id(1), am = make_id_mac(1), bm = make_id_mac(2), got;
    uint32_t        seq = 0;
    uint16_t        ver = 0;

    /* The bench unit: v1 seq 1 in slot A, slot B blank. The v2 record goes
     * to slot B with seq 2; slot A keeps the v1 record. */
    flash_stock();
    reboot();
    uint32_t sum = outside_sum();
    CHECK(store_id_write(&a, &seq));
    CHECK_EQ(seq, 1);
    CHECK(store_id_load(&got, &seq, &ver));
    CHECK_EQ(ver, STORE_ID_V1);
    CHECK(!got.has_mac);
    uint8_t slot_a[FLASH_SECTOR];
    memcpy(slot_a, mem + FLASH_ID_A, FLASH_SECTOR);
    CHECK(store_id_write(&am, &seq));
    CHECK_EQ(seq, 2);
    CHECK_EQ(rec_version(FLASH_ID_B), STORE_ID_V2);
    CHECK_MEM(mem + FLASH_ID_A, slot_a, FLASH_SECTOR);
    CHECK(store_id_load(&got, &seq, &ver));
    CHECK_EQ(seq, 2);
    CHECK_EQ(ver, STORE_ID_V2);
    CHECK(same_id(&got, &am));
    /* A firmware from before #39 still finds the v1 record in slot A. */
    CHECK(load_v1_only(&got, &seq));
    CHECK_EQ(seq, 1);
    CHECK(same_id(&got, &a));
    /* The v2 record once more (another MAC, as with --replace): into slot
     * A, the older one, seq 3; slot B (seq 2) stays. Now a pre-#39 firmware
     * finds no v1 record any more: no identity, fail closed. */
    uint8_t slot_b[FLASH_SECTOR];
    memcpy(slot_b, mem + FLASH_ID_B, FLASH_SECTOR);
    CHECK(store_id_write(&bm, &seq));
    CHECK_EQ(seq, 3);
    CHECK_EQ(rec_version(FLASH_ID_A), STORE_ID_V2);
    CHECK_MEM(mem + FLASH_ID_B, slot_b, FLASH_SECTOR);
    CHECK(store_id_load(&got, &seq, &ver));
    CHECK_EQ(seq, 3);
    CHECK(same_id(&got, &bm));
    CHECK(!load_v1_only(&got, &seq));
    /* Back to v1 (no MAC): into slot B, seq 4; the placeholder MAC again. */
    CHECK(store_id_write(&a, &seq));
    CHECK_EQ(seq, 4);
    CHECK_EQ(rec_version(FLASH_ID_B), STORE_ID_V1);
    CHECK(store_id_load(&got, &seq, &ver));
    CHECK_EQ(ver, STORE_ID_V1);
    CHECK(same_id(&got, &a));
    CHECK_EQ(outside_sum(), sum);

    /* The v1 record in slot B (seq 1, slot A stock data): the v2 record goes
     * to slot A. */
    flash_stock();
    uint8_t  p[STORE_ID_LEN_MAX];
    uint16_t len;
    CHECK(store_id_pack(&a, p, &ver, &len));
    plant(FLASH_ID_B, STORE_ID_MAGIC, STORE_ID_V1, STORE_ID_LEN_V1, p, 1u);
    for (uint32_t i = 0; i < FLASH_SECTOR; i++) {
        mem[FLASH_ID_A + i] = (uint8_t)rnd(); /* stock data, no record */
    }
    memcpy(slot_b, mem + FLASH_ID_B, FLASH_SECTOR);
    CHECK(store_id_write(&am, &seq));
    CHECK_EQ(seq, 2);
    CHECK_EQ(rec_version(FLASH_ID_A), STORE_ID_V2);
    CHECK_MEM(mem + FLASH_ID_B, slot_b, FLASH_SECTOR);
    CHECK(store_id_load(&got, &seq, NULL));
    CHECK(same_id(&got, &am));

    /* The newest slot wins whatever the versions: a v1 record above a v2
     * one is the identity, without a MAC. */
    flash_stock();
    CHECK(store_id_pack(&am, p, &ver, &len));
    plant(FLASH_ID_A, STORE_ID_MAGIC, STORE_ID_V2, STORE_ID_LEN_V2, p, 7u);
    CHECK(store_id_pack(&a, p, &ver, &len));
    plant(FLASH_ID_B, STORE_ID_MAGIC, STORE_ID_V1, STORE_ID_LEN_V1, p, 8u);
    CHECK(store_id_load(&got, &seq, &ver));
    CHECK_EQ(seq, 8);
    CHECK_EQ(ver, STORE_ID_V1);
    CHECK(same_id(&got, &a));
    /* Equal seq in both slots (no tool writes that): slot A, as
     * store_slot_load() does; the next write goes to slot B with seq + 1. */
    plant(FLASH_ID_B, STORE_ID_MAGIC, STORE_ID_V1, STORE_ID_LEN_V1, p, 7u);
    CHECK(store_id_load(&got, &seq, &ver));
    CHECK_EQ(seq, 7);
    CHECK_EQ(ver, STORE_ID_V2);
    CHECK(same_id(&got, &am));
    CHECK(store_id_write(&bm, &seq));
    CHECK_EQ(seq, 8);
    CHECK_EQ(rec_version(FLASH_ID_B), STORE_ID_V2);
    CHECK(store_id_load(&got, &seq, NULL));
    CHECK_EQ(seq, 8);
    CHECK(same_id(&got, &bm));

    /* Bit flips in a newest v2 record: the older v1 one is taken. */
    flash_stock();
    CHECK(store_id_write(&a, NULL));  /* A, seq 1 */
    CHECK(store_id_write(&am, NULL)); /* B, seq 2 */
    for (unsigned off = 0; off < STORE_REC_HDR + STORE_ID_LEN_V2 + 4u; off++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            uint8_t keep = mem[FLASH_ID_B + off];
            mem[FLASH_ID_B + off] ^= (uint8_t)(1u << bit);
            bool ok = store_id_load(&got, &seq, &ver);
            /* The version field 2 -> 3 or 0 makes no record; 2 -> 1 neither
             * (length 30, not 24); the seq may grow, the CRC then fails. */
            CHECK(ok);
            CHECK_EQ(seq, 1);
            CHECK_EQ(ver, STORE_ID_V1);
            CHECK(same_id(&got, &a));
            mem[FLASH_ID_B + off] = keep;
        }
    }
    CHECK(store_id_load(&got, &seq, NULL));
    CHECK(same_id(&got, &am));
}

/* A cut at every step of a write, from no, one and two records, for each
 * pair of old and new record version (mode bit 0: old records v2, bit 1:
 * new record v2). */
static void test_cuts(void) {
    unsigned cases = 0;
    for (unsigned mode = 0; mode < 4; mode++) {
        for (unsigned history = 0; history < 3; history++) {
            for (unsigned k = 1;; k++) {
                bool past_end = false;
                for (unsigned seed = 0; seed < 8 && !past_end; seed++) {
                    flash_stock();
                    reboot();
                    uint32_t        sum = outside_sum();
                    struct store_id old = make_id(10), got;
                    struct store_id neu = (mode & 2u) ? make_id_mac(11) : make_id(11);
                    uint32_t        seq = 0, old_seq = 0;
                    for (unsigned h = 0; h < history; h++) {
                        old = (mode & 1u) ? make_id_mac(20u + h) : make_id(20u + h);
                        CHECK(store_id_write(&old, NULL));
                        old_seq = h + 1u;
                    }
                    /* What a firmware from before #39 finds now: the newest v1
                     * record, which a write of either version never erases. */
                    struct store_id pre;
                    uint32_t        pre_seq = 0;
                    bool            pre_ok  = load_v1_only(&pre, &pre_seq);
                    CHECK_EQ(pre_ok, history > 0 && !(mode & 1u));
                    fm.rnd    = seed * 7919u + k;
                    fm.cut_in = k;
                    bool ok   = store_id_write(&neu, NULL);
                    if (fm.cut_in != 0) {
                        CHECK(ok);
                        past_end = true;
                        continue;
                    }
                    CHECK(!ok);
                    cases++;
                    reboot();
                    bool loaded = store_id_load(&got, &seq, NULL);
                    if (history == 0) {
                        CHECK(!loaded || (seq == 1 && same_id(&got, &neu)));
                    } else {
                        CHECK(loaded);
                        CHECK((seq == old_seq && same_id(&got, &old))
                              || (seq == old_seq + 1u && same_id(&got, &neu)));
                    }
                    if (pre_ok) {
                        struct store_id still;
                        uint32_t        still_seq = 0;
                        CHECK(load_v1_only(&still, &still_seq));
                        /* The same record, or the new v1 record above it. */
                        CHECK((still_seq == pre_seq && same_id(&still, &pre))
                              || (!(mode & 2u) && still_seq == pre_seq + 1u
                                  && same_id(&still, &neu)));
                    }
                    /* Provisioning again after the cut works. */
                    uint32_t before = loaded ? seq : 0;
                    CHECK(store_id_write(&neu, NULL));
                    CHECK(store_id_load(&got, &seq, NULL));
                    CHECK_EQ(seq, before + 1u);
                    CHECK(same_id(&got, &neu));
                    CHECK_EQ(outside_sum(), sum);
                }
                if (past_end) {
                    break;
                }
            }
        }
    }
    /* Erase, the record after its magic (one program), the magic: 3 steps;
     * 3 histories, 8 seeds, 4 version pairs. */
    CHECK_EQ(cases, 4u * 3u * 3u * 8u);
    printf("test_identity: %u power cuts\n", cases);
}

/* One store_id_write() and the store_id_load() after it, results into the
 * trace. */
static void trace_id_write(const struct store_id *id) {
    uint32_t        seq = 0, back = 0;
    uint16_t        version = 0;
    struct store_id got;
    bool            ok = store_id_write(id, &seq);
    trace_bytes(&ok, sizeof ok);
    trace_bytes(&seq, sizeof seq);
    bool ld = store_id_load(&got, &back, &version);
    trace_bytes(&ld, sizeof ld);
    trace_bytes(&back, sizeof back);
    trace_bytes(&version, sizeof version);
    trace_bytes(got.serial, sizeof got.serial);
    trace_bytes(got.eui64, sizeof got.eui64);
    trace_bytes(got.mac, sizeof got.mac);
}

/* The exact flash operations of store_id_write() and their results, as
 * test_record_trace() in test_store.c does for store_slot_write(): writes of
 * both versions from stock data and over records of either version, equal
 * seqs, a seq space used up, a refused identity, and a cut at every step.
 * The digest was taken from the code before #34 batch C (ff5be65), before
 * proposal 8 shared store_slot_write()'s commit steps with
 * store_id_write(); a different digest means the identity record's format,
 * its slot choice or the order of the writes changed. */
static void test_trace(void) {
    fm.rnd = 1;
    flash_stock();
    reboot();
    fm.trace = 0xCBF29CE484222325u;
    for (unsigned i = 0; i < 6; i++) {
        struct store_id id = (i % 3u == 1u) ? make_id(30u + i) : make_id_mac(30u + i);
        trace_id_write(&id);
    }
    struct store_id bad = make_id(40);
    bad.eui64[0]        = 1;
    trace_id_write(&bad);
    /* Equal seqs over both versions: slot B is the one written. */
    uint8_t         p[STORE_ID_LEN_MAX];
    uint16_t        version = 0, len = 0;
    struct store_id v1 = make_id(41), v2 = make_id_mac(42), neu = make_id(43);
    CHECK(store_id_pack(&v1, p, &version, &len));
    plant(FLASH_ID_A, STORE_ID_MAGIC, version, len, p, 9);
    CHECK(store_id_pack(&v2, p, &version, &len));
    plant(FLASH_ID_A + FLASH_SECTOR, STORE_ID_MAGIC, version, len, p, 9);
    trace_id_write(&neu);
    CHECK_EQ(mem[FLASH_ID_A + FLASH_SECTOR + 8], 10);
    /* Seq space: one write left, then none. */
    CHECK(store_id_pack(&v2, p, &version, &len));
    plant(FLASH_ID_A, STORE_ID_MAGIC, version, len, p, 0xFFFFFFFDu);
    trace_id_write(&neu);
    trace_id_write(&v2);
    /* A cut at every step, from one and two records of either version. */
    for (unsigned mode = 0; mode < 4; mode++) {
        for (unsigned history = 1; history <= 2; history++) {
            for (unsigned k = 1;; k++) {
                fm.rnd = k;
                flash_stock();
                reboot();
                for (unsigned h = 0; h < history; h++) {
                    struct store_id old = (mode & 1u) ? make_id_mac(50u + h) : make_id(50u + h);
                    trace_id_write(&old);
                }
                struct store_id n = (mode & 2u) ? make_id_mac(60) : make_id(60);
                fm.rnd            = 77u * k + history;
                fm.cut_in         = k;
                trace_id_write(&n);
                bool done = fm.cut_in != 0;
                reboot();
                trace_id_write(&n);
                if (done) {
                    break;
                }
            }
        }
    }
    printf("test_identity: write trace %016llx\n", (unsigned long long)fm.trace);
    CHECK_EQ(fm.trace, TRACE_ID_DIGEST);
}

int main(void) {
    test_valid();
    test_trace();
    test_basic();
    test_newest_refused();
    test_versions();
    test_cuts();
    return TEST_REPORT("test_identity");
}
