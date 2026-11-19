/* Host tests for the identity record (#21, src/store.c): validation and
 * payload layout, loading from a stock image, slot choice, bit flips, a
 * newest record that is refused, and a power cut at every step of a write
 * (as the provisioning tool, tools/identity/, performs it). The flash model
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
} fm;

static uint32_t rnd(void) {
    uint32_t x = fm.rnd ? fm.rnd : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    fm.rnd = x;
    return x;
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

static bool same_id(const struct store_id *a, const struct store_id *b) {
    return strcmp(a->serial, b->serial) == 0 && memcmp(a->eui64, b->eui64, 8) == 0;
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
    uint8_t p[STORE_ID_LEN];
    id = make_id(3);
    CHECK(store_id_pack(&id, p));
    CHECK_MEM(p, "IB0000001003\0\0\0\0", 16);
    CHECK_MEM(p + 16, id.eui64, 8);
    struct store_id back;
    CHECK(store_id_unpack(p, &back));
    CHECK(same_id(&back, &id));
    /* Anything after the serial's end is refused, not ignored. */
    p[13] = 'X';
    CHECK(!store_id_unpack(p, &back));
    CHECK_EQ(back.serial[0], 0);
    p[13] = 0;
    p[15] = 1;
    CHECK(!store_id_unpack(p, &back));
    p[15] = 0;
    p[16] = 0x01;
    CHECK(!store_id_unpack(p, &back));
    /* An invalid identity is not packed. */
    id.serial[0] = 'i';
    CHECK(!store_id_pack(&id, p));
    CHECK_EQ(p[0], 0);
}

static void test_basic(void) {
    flash_stock();
    reboot();
    uint32_t        sum = outside_sum();
    struct store_id got;
    uint32_t        seq = 0;

    /* The stock data in slot A is no record: only its magic is read. */
    fm.slot_a_body_reads = 0;
    CHECK(!store_id_load(&got, &seq));
    CHECK_EQ(fm.slot_a_body_reads, 0);
    CHECK_EQ(got.serial[0], 0);

    struct store_id a = make_id(1), b = make_id(2), c = make_id(3);
    CHECK(store_id_write(&a, &seq));
    CHECK_EQ(seq, 1);
    CHECK_EQ(mem[FLASH_ID_A], 0x49); /* "IDN1": slot A was the invalid one */
    CHECK(store_id_load(&got, &seq));
    CHECK_EQ(seq, 1);
    CHECK(same_id(&got, &a));
    CHECK(store_id_write(&b, NULL));
    CHECK_EQ(mem[FLASH_ID_B], 0x49);
    CHECK(store_id_load(&got, &seq));
    CHECK_EQ(seq, 2);
    CHECK(same_id(&got, &b));
    CHECK(store_id_write(&c, NULL));
    CHECK(store_id_load(&got, &seq));
    CHECK_EQ(seq, 3);
    CHECK(same_id(&got, &c));
    CHECK_EQ(outside_sum(), sum);

    /* One flipped bit anywhere in the newest record (slot A): the older one
     * in slot B is taken. */
    for (unsigned off = 0; off < STORE_REC_HDR + STORE_ID_LEN + 4u; off++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            uint8_t keep = mem[FLASH_ID_A + off];
            mem[FLASH_ID_A + off] ^= (uint8_t)(1u << bit);
            CHECK(store_id_load(&got, &seq));
            CHECK_EQ(seq, 2);
            CHECK(same_id(&got, &b));
            mem[FLASH_ID_A + off] = keep;
        }
    }
    /* With only one record, a flip anywhere leaves no identity. */
    flash_stock();
    CHECK(store_id_write(&a, NULL));
    for (unsigned off = 0; off < STORE_REC_HDR + STORE_ID_LEN + 4u; off++) {
        uint8_t keep = mem[FLASH_ID_A + off];
        mem[FLASH_ID_A + off] ^= 0x04;
        CHECK(!store_id_load(&got, &seq));
        mem[FLASH_ID_A + off] = keep;
    }
    CHECK(store_id_load(&got, &seq));
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
    uint8_t p[STORE_ID_LEN];
    CHECK(store_id_pack(&a, p));
    p[17] = 0x1C; /* EUI-64 00 1c ...: refused */
    CHECK(store_slot_write(FLASH_ID_A, STORE_ID_MAGIC, STORE_ID_VERSION, p, sizeof p, NULL));
    CHECK(!store_id_load(&got, &seq));
    CHECK_EQ(got.eui64[0], 0);

    /* A record of another version, or another length, in the newer seq:
     * not an identity record of ours at all, so the valid v1 record in the
     * other slot is used. */
    flash_stock();
    CHECK(store_id_write(&a, NULL)); /* slot A, seq 1 */
    CHECK(store_id_pack(&a, p));
    plant(FLASH_ID_B, STORE_ID_MAGIC, 2u, STORE_ID_LEN, p, 5u);
    CHECK(store_id_load(&got, &seq));
    CHECK(same_id(&got, &a));
    CHECK_EQ(seq, 1);
    plant(FLASH_ID_B, STORE_ID_MAGIC, STORE_ID_VERSION, 20u, p, 5u);
    CHECK(store_id_load(&got, &seq));
    CHECK_EQ(seq, 1);
    /* The same, planted with the right fields: it is taken (the planting
     * itself is right). */
    plant(FLASH_ID_B, STORE_ID_MAGIC, STORE_ID_VERSION, STORE_ID_LEN, p, 5u);
    CHECK(store_id_load(&got, &seq));
    CHECK_EQ(seq, 5);
    /* A network record's magic in the identity slots is not an identity. */
    flash_stock();
    plant(FLASH_ID_A, STORE_NET_MAGIC, STORE_ID_VERSION, STORE_ID_LEN, p, 1u);
    CHECK(!store_id_load(&got, &seq));
}

/* A cut at every step of a write, from no, one and two records. */
static void test_cuts(void) {
    unsigned cases = 0;
    for (unsigned history = 0; history < 3; history++) {
        for (unsigned k = 1;; k++) {
            bool past_end = false;
            for (unsigned seed = 0; seed < 8 && !past_end; seed++) {
                flash_stock();
                reboot();
                uint32_t        sum = outside_sum();
                struct store_id old = make_id(10), neu = make_id(11), got;
                uint32_t        seq = 0, old_seq = 0;
                for (unsigned h = 0; h < history; h++) {
                    old = make_id(20u + h);
                    CHECK(store_id_write(&old, NULL));
                    old_seq = h + 1u;
                }
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
                bool loaded = store_id_load(&got, &seq);
                if (history == 0) {
                    CHECK(!loaded || (seq == 1 && same_id(&got, &neu)));
                } else {
                    CHECK(loaded);
                    CHECK((seq == old_seq && same_id(&got, &old))
                          || (seq == old_seq + 1u && same_id(&got, &neu)));
                }
                /* Provisioning again after the cut works. */
                uint32_t before = loaded ? seq : 0;
                CHECK(store_id_write(&neu, NULL));
                CHECK(store_id_load(&got, &seq));
                CHECK_EQ(seq, before + 1u);
                CHECK(same_id(&got, &neu));
                CHECK_EQ(outside_sum(), sum);
            }
            if (past_end) {
                break;
            }
        }
    }
    /* Erase, the record after its magic (one program), the magic: 3 steps. */
    CHECK_EQ(cases, 3u * 3u * 8u);
    printf("test_identity: %u power cuts\n", cases);
}

int main(void) {
    test_valid();
    test_basic();
    test_newest_refused();
    test_cuts();
    return TEST_REPORT("test_identity");
}
