/* Host tests for src/store.c (#37): slotted network record and counter log
 * against a NOR flash model that cuts the power at chosen program and erase
 * steps. Checked after every cut and reboot: a record is either the old or
 * the new one, never anything else; the counter never goes below a counter
 * already used on the air; the log's highest entry never falls below a
 * reservation that was reported written; nothing outside the record sectors
 * is ever erased or programmed, and no byte is programmed twice without an
 * erase. */
#include "store.h"

#include "flash_layout.h"
#include "mac.h"
#include "txgate.h"

#include "test.h"

#include <stdint.h>
#include <stdlib.h>

/* --- Flash model -------------------------------------------------------------- */
static uint8_t mem[FLASH_SIZE];

static struct {
    long     cut_in;     /* program/erase steps until the cut; 0: none armed */
    bool     dead;       /* power is gone: everything fails until reboot() */
    bool     weak;       /* the next program leaves one bit unprogrammed, reports success */
    unsigned prog_fail;  /* without a power cut, every program: see enum fail */
    unsigned erase_fail; /* without a power cut, every erase: see enum fail */
    uint32_t protect;    /* a sector that must never be erased (0: none) */
    /* A weak bit: once a program covers fade_addr, fade_after more reads of
     * that byte see it programmed, then fade_mask reads 1 again. */
    uint32_t fade_addr; /* 0: none */
    uint8_t  fade_mask;
    unsigned fade_after;
    bool     fade_armed, fade_done;
    unsigned ops; /* program + erase steps so far */
    unsigned cuts_program, cuts_erase;
    unsigned ctr_erases;        /* completed erases of a counter-log sector */
    uint32_t last_prog_addr;    /* address of the last completed program */
    unsigned slot_a_body_reads; /* reads past the magic of slot A */
    uint32_t rnd;
} fm;

/* Failures with the power on (the chip's fail flags, a verify mismatch). */
enum fail {
    FAIL_NONE,
    FAIL_NOTHING, /* reports failure, nothing changed */
    FAIL_PARTIAL, /* reports failure, part of the bits done (P_FAIL / E_FAIL) */
    FAIL_SILENT,  /* reports success, part of the bits not done (read-back catches it) */
};

static uint32_t rnd(void) {
    /* xorshift32; a zero state would stick */
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
    if (addr < FLASH_NET_A + FLASH_SECTOR && addr + n > FLASH_NET_A + 4u) {
        fm.slot_a_body_reads++;
    }
    if (fm.fade_armed && addr <= fm.fade_addr && fm.fade_addr < addr + n) {
        if (fm.fade_after == 0) {
            mem[fm.fade_addr] |= fm.fade_mask;
            fm.fade_armed = false;
            fm.fade_done  = true;
        } else {
            fm.fade_after--;
        }
    }
    memcpy(dst, mem + addr, n);
    return true;
}

bool store_port_program(uint32_t addr, const uint8_t *src, size_t n) {
    /* The driver's guard and page rule, as hard checks. */
    CHECK(addr >= FLASH_WRITE_BEGIN && addr + n <= FLASH_WRITE_END);
    CHECK(n >= 1 && n <= STORE_XFER_MAX && addr % FLASH_PAGE + n <= FLASH_PAGE);
    if (fm.dead) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        CHECK_EQ(mem[addr + i], 0xFF); /* never programmed twice without an erase */
    }
    if (fm.prog_fail != FAIL_NONE) {
        fm.ops++;
        for (size_t i = 0; fm.prog_fail != FAIL_NOTHING && i < n; i++) {
            mem[addr + i] &= (uint8_t)(src[i] | rnd());
        }
        if (fm.prog_fail == FAIL_SILENT) {
            /* At least one bit that should be 0 still reads 1 (the bytes
             * were blank, so that bit was never programmed). */
            for (size_t j = 0, i = rnd() % n; j < n; j++, i = (i + 1u) % n) {
                unsigned zeros = ~(unsigned)src[i] & 0xFFu;
                if (zeros) {
                    mem[addr + i] |= (uint8_t)(zeros & (0u - zeros)); /* the lowest one */
                    break;
                }
            }
        }
        return fm.prog_fail == FAIL_SILENT;
    }
    if (cut_now()) {
        /* Torn: a prefix done, one byte with only some of its bits. */
        size_t k = rnd() % (n + 1u);
        for (size_t i = 0; i < k; i++) {
            mem[addr + i] &= src[i];
        }
        if (k < n) {
            mem[addr + k] &= (uint8_t)(src[k] | rnd());
        }
        fm.cuts_program++;
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        mem[addr + i] &= src[i];
    }
    fm.last_prog_addr = addr;
    if (fm.fade_addr && !fm.fade_done && addr <= fm.fade_addr && fm.fade_addr < addr + n) {
        fm.fade_armed = true;
    }
    if (fm.weak) {
        fm.weak  = false;
        size_t i = rnd() % n;
        for (unsigned b = 0; b < 8; b++) {
            if (!(src[i] >> b & 1u)) {
                mem[addr + i] |= (uint8_t)(1u << b);
                break;
            }
        }
    }
    return true;
}

bool store_port_erase(uint32_t addr) {
    CHECK(addr >= FLASH_WRITE_BEGIN && addr + FLASH_SECTOR <= FLASH_WRITE_END);
    CHECK_EQ(addr % FLASH_SECTOR, 0);
    if (fm.dead) {
        return false;
    }
    if (fm.protect != 0) {
        CHECK(addr != fm.protect); /* the sector with the highest entry */
    }
    if (fm.erase_fail != FAIL_NONE) {
        fm.ops++;
        for (uint32_t i = 0; fm.erase_fail != FAIL_NOTHING && i < FLASH_SECTOR; i++) {
            mem[addr + i] |= (uint8_t)rnd();
        }
        if (fm.erase_fail == FAIL_SILENT) {
            mem[addr + rnd() % FLASH_SECTOR] &= 0x7F; /* at least one byte not blank */
        }
        return fm.erase_fail == FAIL_SILENT;
    }
    if (cut_now()) {
        /* Torn: some bytes erased, some with random bits set, and (the
         * chip's pre-program phase) some with random bits cleared. */
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
        fm.cuts_erase++;
        return false;
    }
    memset(mem + addr, 0xFF, FLASH_SECTOR);
    if (addr == FLASH_CTR_A || addr == FLASH_CTR_B) {
        fm.ctr_erases++;
    }
    return true;
}

/* Power comes back: RAM state is gone. */
static void reboot(void) {
    fm.dead      = false;
    fm.cut_in    = 0;
    fm.weak      = false;
    fm.prog_fail = fm.erase_fail = FAIL_NONE;
    fm.protect                   = 0;
    fm.fade_addr                 = 0;
    fm.fade_armed = fm.fade_done = false;
    store_ctr_forget();
}

/* The bench unit's stock image uses 0x000000-0x03DFFF, 0x080000-0x0BEFFF,
 * 0x100000-0x100FFF and 0x102000-0x102FFF (docs/design.md, #37 notes).
 * Fill those with noise, the rest blank. */
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
        if (a < FLASH_WRITE_BEGIN || a >= FLASH_WRITE_END) {
            h = (h ^ mem[a]) * 16777619u;
        }
    }
    return h;
}

/* --- Helpers ------------------------------------------------------------------ */
static struct store_net make_net(uint8_t tag, uint32_t floor) {
    struct store_net n = {.pan_id = MAC_PAN_ID, .channel = TXGATE_TEST_CHANNEL};
    for (unsigned i = 0; i < CCM_KEY_LEN; i++) {
        n.key[i] = (uint8_t)(tag * 17u + i);
    }
    n.counter_floor = floor;
    return n;
}

static bool same_net(const struct store_net *a, const struct store_net *b) {
    return a->pan_id == b->pan_id && a->channel == b->channel
           && memcmp(a->key, b->key, CCM_KEY_LEN) == 0 && a->counter_floor == b->counter_floor;
}

/* Steps (programs + erases) of one uncut call. */
static unsigned steps_of_net_write(const struct store_net *n) {
    unsigned before = fm.ops;
    CHECK(store_net_write(n, NULL));
    return fm.ops - before;
}

/* --- Tests -------------------------------------------------------------------- */
static void test_crc(void) {
    CHECK_EQ(store_crc32((const uint8_t *)"123456789", 9), 0xCBF43926u);
    CHECK_EQ(store_crc32(NULL, 0), 0);
}

static void test_record_basic(void) {
    flash_stock();
    reboot();
    uint32_t         sum = outside_sum();
    struct store_net got;
    uint32_t         seq = 0;

    /* The stock configuration in slot A is not a record, and only its
     * first four bytes are read. */
    fm.slot_a_body_reads = 0;
    CHECK(!store_net_load(&got, &seq));
    CHECK_EQ(fm.slot_a_body_reads, 0);
    bool any = true;
    CHECK(store_net_probe(&any));
    CHECK(!any); /* no network has ever been written */
    CHECK_EQ(fm.slot_a_body_reads, 0);

    struct store_net a = make_net(1, 0), b = make_net(2, 4096), c = make_net(3, 8192);
    CHECK(store_net_write(&a, NULL));
    CHECK(store_net_load(&got, &seq));
    CHECK_EQ(seq, 1);
    CHECK(same_net(&got, &a));
    CHECK(store_net_probe(&any));
    CHECK(any);
    /* Slot A held the stock data, so it was the invalid one and got the
     * first record; the next goes to B, then A again. */
    CHECK_EQ(mem[FLASH_NET_A], 0x4E);
    CHECK(store_net_write(&b, NULL));
    CHECK(store_net_load(&got, &seq));
    CHECK_EQ(seq, 2);
    CHECK(same_net(&got, &b));
    CHECK_EQ(mem[FLASH_NET_B], 0x4E);
    CHECK(store_net_write(&c, NULL));
    CHECK(store_net_load(&got, &seq));
    CHECK_EQ(seq, 3);
    CHECK(same_net(&got, &c));
    CHECK_EQ(outside_sum(), sum);

    /* One flipped bit anywhere in the newest record: the older one wins. */
    for (unsigned off = 0; off < STORE_REC_HDR + STORE_NET_LEN + 4u; off++) {
        uint8_t keep = mem[FLASH_NET_A + off];
        mem[FLASH_NET_A + off] ^= 0x10;
        CHECK(store_net_load(&got, &seq));
        CHECK_EQ(seq, 2);
        CHECK(same_net(&got, &b));
        mem[FLASH_NET_A + off] = keep;
    }

    /* A program that silently leaves a bit set is caught by the read-back,
     * and the older record stays the valid one. */
    struct store_net d = make_net(4, 0);
    for (unsigned t = 0; t < 20; t++) {
        fm.weak = true;
        CHECK(!store_net_write(&d, NULL));
        fm.weak = false;
        CHECK(store_net_load(&got, &seq));
        CHECK_EQ(seq, 3);
        CHECK(same_net(&got, &c));
    }
    CHECK(store_net_write(&d, NULL));
    CHECK(store_net_load(&got, &seq));
    CHECK_EQ(seq, 4);
    CHECK(same_net(&got, &d));
}

static void test_record_policy(void) {
    flash_stock();
    reboot();
    struct store_net n = make_net(5, 0), got;
    uint32_t         seq;
    unsigned         ops_start = fm.ops;
    n.channel                  = TXGATE_PROTECTED_CHANNEL;
    CHECK(!store_net_write(&n, NULL));
    n.channel = TXGATE_TEST_CHANNEL - 1u;
    CHECK(!store_net_write(&n, NULL));
    n.channel = 10u; /* the test channel until 2026-11-17 */
    CHECK(!store_net_write(&n, NULL));
    n.channel     = TXGATE_TEST_CHANNEL;
    n.pan_id      = 0xFFFF;
    unsigned ops0 = fm.ops;
    CHECK(!store_net_write(&n, NULL));
    CHECK_EQ(fm.ops, ops0); /* refused before touching the flash */
    CHECK_EQ(fm.ops, ops_start);

    /* A well-formed record for channel 26 (written around the policy) is
     * never loaded. */
    uint8_t p[STORE_NET_LEN] = {0xCD, 0xAB, TXGATE_PROTECTED_CHANNEL, 0};
    CHECK(store_slot_write(FLASH_NET_A, STORE_NET_MAGIC, STORE_NET_VERSION, p, sizeof p, NULL));
    CHECK(!store_net_load(&got, &seq));
    bool any = false;
    CHECK(store_net_probe(&any));
    CHECK(any); /* a record, refused: not "no network" */
    /* Nor one for the old test channel 10 (owner decision 2026-11-17). */
    p[2] = 10u;
    CHECK(store_slot_write(FLASH_NET_A, STORE_NET_MAGIC, STORE_NET_VERSION, p, sizeof p, NULL));
    CHECK(!store_net_load(&got, &seq));
    p[2] = TXGATE_TEST_CHANNEL;
    CHECK(store_slot_write(FLASH_NET_A, STORE_NET_MAGIC, STORE_NET_VERSION, p, sizeof p, NULL));
    CHECK(store_net_load(&got, &seq));
    CHECK_EQ(seq, 3); /* after the two refused records */
    p[2] = TXGATE_PROTECTED_CHANNEL;
    CHECK(store_slot_write(FLASH_NET_A, STORE_NET_MAGIC, STORE_NET_VERSION, p, sizeof p, NULL));
    CHECK(!store_net_load(&got, &seq)); /* the newest is refused, not an older one taken */
}

/* A cut at every step of a record write, from each starting history:
 * no record, one record, two records. */
static void test_record_cuts(void) {
    unsigned cases = 0;
    for (unsigned history = 0; history < 3; history++) {
        for (unsigned k = 1;; k++) {
            bool past_end = false;
            for (unsigned seed = 0; seed < 8 && !past_end; seed++) {
                flash_stock();
                reboot();
                uint32_t         sum = outside_sum();
                struct store_net old = make_net(10, 0), neu = make_net(11, 777), got;
                uint32_t         seq = 0, old_seq = 0;
                for (unsigned h = 0; h < history; h++) {
                    old = make_net((uint8_t)(20 + h), h);
                    CHECK(store_net_write(&old, NULL));
                    old_seq = h + 1u;
                }
                fm.rnd    = seed * 7919u + k;
                fm.cut_in = k;
                bool ok   = store_net_write(&neu, NULL);
                if (fm.cut_in != 0) {
                    /* Fewer than k steps: the write finished uncut. */
                    CHECK(ok);
                    past_end = true;
                    continue;
                }
                CHECK(!ok);
                cases++;
                reboot();
                bool loaded = store_net_load(&got, &seq);
                bool any    = !loaded;
                CHECK(store_net_probe(&any));
                CHECK(any || !loaded);
                if (history == 0) {
                    /* A first write cut before its magic leaves "no network". */
                    CHECK_EQ(any, loaded);
                    CHECK(!loaded || (seq == 1 && same_net(&got, &neu)));
                } else {
                    CHECK(loaded);
                    CHECK((seq == old_seq && same_net(&got, &old))
                          || (seq == old_seq + 1u && same_net(&got, &neu)));
                }
                /* And the store works on after the cut. */
                uint32_t before = loaded ? seq : 0;
                CHECK(store_net_write(&neu, NULL));
                CHECK(store_net_load(&got, &seq));
                CHECK_EQ(seq, before + 1u);
                CHECK(same_net(&got, &neu));
                CHECK_EQ(outside_sum(), sum);
            }
            if (past_end) {
                break;
            }
        }
    }
    printf("test_store: record write cut at every step: %u cases\n", cases);
    CHECK(cases >= 3u * 8u * 3u);
}

static bool all_zero(const void *p, size_t n) {
    const uint8_t *b = p;
    while (n--) {
        if (*b++) {
            return false;
        }
    }
    return true;
}

/* store_net_commit(): true only if the network read back as a boot would
 * read it is the one just written. A magic bit that reads right while the
 * record is written and verified and then fades (a weak bit) leaves the
 * older record as the newest valid one: the commit fails instead of
 * handing back the older network's key. Swept over when the bit fades. */
static void test_net_commit(void) {
    struct store_net a = make_net(1, 0), b = make_net(2, 100), c = make_net(3, 200), got;
    uint32_t         seq = 0;
    flash_stock();
    reboot();
    CHECK(store_net_write(&a, &seq));
    CHECK_EQ(seq, 1);
    CHECK(store_net_commit(&b, &got, &seq));
    CHECK_EQ(seq, 2);
    CHECK(same_net(&got, &b));
    /* A bit the write's own read-back catches. */
    fm.weak = true;
    CHECK(!store_net_commit(&c, &got, &seq));
    CHECK(all_zero(&got, sizeof got));

    unsigned by_write = 0, by_reload = 0, held = 0;
    for (unsigned after = 0; after < 12; after++) {
        bool     write_alone = false;
        uint32_t wseq        = 0;
        for (unsigned pass = 0; pass < 2; pass++) {
            flash_stock();
            reboot();
            CHECK(store_net_write(&a, NULL));
            CHECK(store_net_write(&b, NULL)); /* seq 1 in A, 2 in B: the next goes to A */
            fm.fade_addr  = FLASH_NET_A;      /* magic byte 0x4E: bit 0 programmed */
            fm.fade_mask  = 0x01;
            fm.fade_after = after;
            if (pass == 0) {
                write_alone = store_net_write(&c, &wseq); /* what the write itself sees */
                continue;
            }
            bool ok    = store_net_commit(&c, &got, &seq);
            bool faded = fm.fade_done;
            reboot();
            struct store_net now;
            uint32_t         now_seq = 0;
            CHECK(store_net_load(&now, &now_seq));
            if (ok) {
                CHECK(write_alone && wseq == 3);
                CHECK_EQ(seq, 3);
                CHECK(same_net(&got, &c));
                CHECK(!faded);
                held++;
            } else {
                CHECK(all_zero(&got, sizeof got));
                CHECK(faded);
                /* The older network is what a boot would run. */
                CHECK_EQ(now_seq, 2);
                CHECK(same_net(&now, &b));
                if (write_alone) {
                    by_reload++; /* reported written, gone at the read-back */
                } else {
                    by_write++;
                }
            }
        }
    }
    printf("test_store: network commit with a fading magic bit: caught by the write %u, by the "
           "read-back %u, held %u\n",
           by_write,
           by_reload,
           held);
    CHECK(by_write > 0 && by_reload > 0 && held > 0);
}

/* Counter log: boot, use counters, extend; cuts at random steps, many
 * sector changes. */
static void test_counter_run(void) {
    flash_stock();
    reboot();
    uint32_t sum  = outside_sum();
    fm.rnd        = 4242;
    fm.cuts_erase = fm.cuts_program = 0;

    uint32_t sent_upto = 0; /* one past the highest counter used on the air */
    uint32_t committed = 0; /* highest reservation reported written */
    uint32_t next = 0, limit = 0;
    bool     up    = false;
    unsigned boots = 0, failed_boots = 0, extends = 0, cuts = 0;
    unsigned erases0 = fm.ctr_erases;

    for (unsigned step = 0; step < 400000 && extends < 3000; step++) {
        if (!up) {
            reboot();
            if (rnd() % 4u == 0) {
                fm.cut_in = 1 + rnd() % 3u; /* sometimes cut during the boot write */
            }
            uint32_t first = 0, lim = 0;
            boots++;
            if (store_counter_boot(0, &first, &lim)) {
                CHECK(first >= sent_upto);
                CHECK(lim == first + STORE_CTR_AHEAD);
                CHECK(lim > committed);
                committed = lim;
                next      = first;
                limit     = lim;
                up        = true;
                fm.cut_in = 0;
            } else {
                failed_boots++;
                CHECK(fm.dead); /* only a cut makes the boot fail here */
                cuts++;
            }
            continue;
        }
        if (fm.cut_in == 0 && rnd() % 8u == 0) {
            fm.cut_in = 1 + rnd() % 40u; /* cut somewhere in the run ahead */
        }
        /* Frames: never at or above the limit (the coordinator's gate). */
        uint32_t burst = 1 + rnd() % 400u;
        for (uint32_t f = 0; f < burst && next < limit; f++) {
            next++;
        }
        if (next > sent_upto) {
            sent_upto = next;
        }
        uint32_t old_limit = limit;
        bool     ok        = store_counter_extend(next, &limit);
        if (ok && limit != old_limit) {
            CHECK(limit > old_limit);
            CHECK(limit - next == STORE_CTR_AHEAD);
            committed = limit;
            extends++;
        }
        if (!ok) {
            CHECK(limit == old_limit);
            if (fm.dead) {
                cuts++;
                up = false; /* power gone */
            }
        }
        /* The persisted reservation never falls below what was committed. */
        if (!up) {
            reboot();
            uint32_t max;
            bool     found;
            CHECK(store_ctr_highest(&max, &found));
            CHECK(found);
            CHECK(max >= committed);
            CHECK(max >= sent_upto);
        }
    }
    printf("test_store: counter run: %u boots (%u cut), %u extends, %u cuts (%u in erase, %u in "
           "program), counter at %lu\n",
           boots,
           failed_boots,
           extends,
           cuts,
           fm.cuts_erase,
           fm.cuts_program,
           (unsigned long)sent_upto);
    CHECK(extends >= 3000);
    CHECK(fm.cuts_program > 50);
    CHECK(fm.ctr_erases - erases0 > 4); /* sector changes, both ways */
    CHECK_EQ(outside_sum(), sum);
}

/* Index in its sector of the last counter entry programmed, and the sector. */
static unsigned last_entry(uint32_t *sector) {
    uint32_t a = fm.last_prog_addr;
    *sector    = a < FLASH_CTR_B ? FLASH_CTR_A : FLASH_CTR_B;
    return (a - *sector) / STORE_CTR_ENTRY;
}

/* Extend until `full` sectors have been filled to their last slot, then cut
 * at every step of the append that moves to the other sector (erase it,
 * program its first slot): first to a blank sector, then to a stale one. */
static void test_counter_switch_cuts(void) {
    unsigned cases = 0;
    for (unsigned full = 1; full <= 2; full++) {
        for (unsigned k = 1; k <= 3; k++) {
            for (unsigned seed = 0; seed < 16; seed++) {
                flash_stock();
                reboot();
                fm.rnd = seed * 31u + k;
                uint32_t first, limit, sector;
                CHECK(store_counter_boot(1000, &first, &limit));
                unsigned filled = 0;
                for (unsigned guard = 0; guard < 4u * STORE_CTR_ENTRIES && filled < full; guard++) {
                    CHECK(store_counter_extend(limit - STORE_CTR_LOW + 1u, &limit));
                    if (last_entry(&sector) == STORE_CTR_ENTRIES - 1u) {
                        filled++;
                    }
                }
                CHECK_EQ(filled, full);
                uint32_t full_sector = sector;
                uint32_t committed   = limit;
                uint32_t next        = limit - 1u;
                fm.cut_in            = k;
                uint32_t lim2        = limit;
                bool     ok          = store_counter_extend(next, &lim2);
                if (fm.cut_in != 0) {
                    CHECK(ok); /* fewer than k steps */
                    CHECK(lim2 == next + STORE_CTR_AHEAD);
                    continue;
                }
                CHECK(!ok);
                cases++;
                reboot();
                uint32_t max;
                bool     found;
                CHECK(store_ctr_highest(&max, &found));
                CHECK(found);
                CHECK(max >= committed);
                reboot();
                uint32_t f2, l2;
                CHECK(store_counter_boot(0, &f2, &l2));
                CHECK(f2 >= committed);
                /* The boot's entry went to the other sector, at its start
                 * or just after a torn first slot. */
                uint32_t now_sector;
                unsigned idx = last_entry(&now_sector);
                CHECK(now_sector != full_sector);
                CHECK(idx <= 1u);
            }
        }
    }
    printf("test_store: counter sector change cut at every step: %u cases\n", cases);
    CHECK(cases >= 2u * 2u * 16u);
}

/* The log's highest valid entry, read straight from the model: false if
 * there is none; *sector is the sector it is in. */
static bool mem_ctr_max(uint32_t *max, uint32_t *sector) {
    bool found = false;
    for (uint32_t a = FLASH_CTR_A; a < FLASH_CTR_B + FLASH_SECTOR; a += STORE_CTR_ENTRY) {
        uint32_t v = (uint32_t)mem[a] | (uint32_t)mem[a + 1] << 8 | (uint32_t)mem[a + 2] << 16
                     | (uint32_t)mem[a + 3] << 24;
        uint32_t w = (uint32_t)mem[a + 4] | (uint32_t)mem[a + 5] << 8 | (uint32_t)mem[a + 6] << 16
                     | (uint32_t)mem[a + 7] << 24;
        if (v == ~w && (!found || v > *max)) {
            found   = true;
            *max    = v;
            *sector = a < FLASH_CTR_B ? FLASH_CTR_A : FLASH_CTR_B;
        }
    }
    return found;
}

/* Every counter-log program fails with the power on (fail flag with nothing
 * or part done, or a silent miss the read-back catches), starting at
 * several fill levels: the extends fail, the limit stays, the sector with
 * the highest entry is never erased (the model checks every erase), the
 * log's highest entry never falls below the last reservation reported
 * written, and once the slots run out nothing more is tried until the next
 * boot, which starts above that reservation. */
static void test_counter_failing_programs(void) {
    /* Successful extends after the boot entry, before the failures. */
    static const unsigned before[] = {0, 1, 300, 510, 511, 512, 700, 1023};
    unsigned              cases    = 0;
    for (unsigned mode = FAIL_NOTHING; mode <= FAIL_SILENT; mode++) {
        for (unsigned b = 0; b < sizeof before / sizeof before[0]; b++) {
            flash_stock();
            reboot();
            fm.rnd       = mode * 101u + b + 1u;
            uint32_t sum = outside_sum();
            uint32_t first, limit, max, sector, max_sector;
            CHECK(store_counter_boot(1000, &first, &limit));
            for (unsigned i = 0; i < before[b]; i++) {
                CHECK(store_counter_extend(limit - STORE_CTR_LOW + 1u, &limit));
            }
            uint32_t committed = limit;
            (void)last_entry(&sector);
            CHECK(mem_ctr_max(&max, &max_sector));
            CHECK(max == committed && max_sector == sector);
            fm.protect       = sector;
            fm.prog_fail     = mode;
            unsigned erases0 = fm.ctr_erases;
            bool     stuck   = false;
            for (unsigned call = 0; call < 3u * STORE_CTR_ENTRIES; call++) {
                uint32_t lim2 = limit;
                unsigned ops0 = fm.ops;
                CHECK(!store_counter_extend(limit - STORE_CTR_LOW + 1u, &lim2));
                CHECK_EQ(lim2, limit);
                CHECK(mem_ctr_max(&max, &max_sector));
                CHECK(max >= committed);
                if (fm.ops == ops0) {
                    stuck = true; /* nothing tried */
                } else {
                    CHECK(!stuck); /* and it stays so */
                }
            }
            CHECK(stuck);
            CHECK(fm.ctr_erases - erases0 <= 1u); /* at most the change to the next sector */
            /* Programs work again: still closed until the next boot. */
            fm.prog_fail  = FAIL_NONE;
            uint32_t lim2 = limit;
            CHECK(!store_counter_extend(limit - STORE_CTR_LOW + 1u, &lim2));
            CHECK_EQ(lim2, limit);

            reboot();
            CHECK(mem_ctr_max(&max, &max_sector));
            fm.protect = max_sector;
            uint32_t f2, l2;
            CHECK(store_counter_boot(0, &f2, &l2));
            CHECK(f2 >= committed);
            CHECK(mem_ctr_max(&max, &max_sector));
            CHECK_EQ(max, l2);
            CHECK_EQ(outside_sum(), sum);
            cases++;
        }
    }
    printf("test_store: counter log with every program failing: %u cases\n", cases);
    CHECK_EQ(cases, 3u * sizeof before / sizeof before[0]);
}

/* The sector change meets failing erases (fail flag with nothing or part
 * done, or a silent miss the blank check catches), to a blank and to a
 * stale sector: the extends fail, the full sector with the highest entry is
 * left exactly as it was, and once erases work the next extend goes to the
 * first slot of the other sector. */
static void test_counter_failing_erases(void) {
    static uint8_t keep[FLASH_SECTOR];
    unsigned       cases = 0;
    for (unsigned mode = FAIL_NOTHING; mode <= FAIL_SILENT; mode++) {
        for (unsigned full = 1; full <= 2; full++) {
            flash_stock();
            reboot();
            fm.rnd = mode * 7u + full;
            uint32_t first, limit, sector, max, max_sector;
            CHECK(store_counter_boot(1000, &first, &limit));
            unsigned filled = 0;
            for (unsigned guard = 0; guard < 4u * STORE_CTR_ENTRIES && filled < full; guard++) {
                CHECK(store_counter_extend(limit - STORE_CTR_LOW + 1u, &limit));
                if (last_entry(&sector) == STORE_CTR_ENTRIES - 1u) {
                    filled++;
                }
            }
            CHECK_EQ(filled, full);
            uint32_t committed = limit;
            memcpy(keep, mem + sector, sizeof keep);
            fm.protect    = sector;
            fm.erase_fail = mode;
            for (unsigned call = 0; call < 50; call++) {
                uint32_t lim2 = limit;
                CHECK(!store_counter_extend(limit - STORE_CTR_LOW + 1u, &lim2));
                CHECK_EQ(lim2, limit);
                CHECK(memcmp(keep, mem + sector, sizeof keep) == 0);
                CHECK(mem_ctr_max(&max, &max_sector));
                CHECK(max == committed && max_sector == sector);
            }
            fm.erase_fail = FAIL_NONE;
            uint32_t lim2 = limit;
            CHECK(store_counter_extend(limit - STORE_CTR_LOW + 1u, &lim2));
            CHECK(lim2 > limit);
            uint32_t now_sector;
            CHECK_EQ(last_entry(&now_sector), 0);
            CHECK(now_sector != sector);
            reboot();
            uint32_t f2, l2;
            CHECK(store_counter_boot(0, &f2, &l2));
            CHECK_EQ(f2, lim2);
            cases++;
        }
    }
    printf("test_store: counter sector change with failing erases: %u cases\n", cases);
    CHECK_EQ(cases, 3u * 2u);
}

/* A network written with a floor, then the counter boot honours the floor
 * when the log is empty, and the log when it is higher. */
static void test_counter_floor(void) {
    flash_stock();
    reboot();
    uint32_t first, limit;
    CHECK(store_counter_boot(50000, &first, &limit));
    CHECK_EQ(first, 50000);
    CHECK_EQ(limit, 50000 + STORE_CTR_AHEAD);
    reboot();
    CHECK(store_counter_boot(10, &first, &limit));
    CHECK_EQ(first, 50000 + STORE_CTR_AHEAD);
    /* Counter space used up: refuse. */
    reboot();
    CHECK(!store_counter_boot(UINT32_MAX - 100u, &first, &limit));
    /* Extend refuses a counter above the limit (cannot happen). */
    uint32_t l = 100;
    CHECK(!store_counter_extend(101, &l));
    CHECK_EQ(l, 100);
}

/* Garbage everywhere in the log (a torn erase of both sectors): the scan
 * still finds a place to write, and the result is above every valid-looking
 * entry. */
static void test_counter_garbage(void) {
    for (unsigned seed = 1; seed < 20; seed++) {
        flash_stock();
        reboot();
        fm.rnd = seed;
        for (uint32_t a = FLASH_CTR_A; a < FLASH_CTR_B + FLASH_SECTOR; a++) {
            mem[a] = (uint8_t)(rnd() | (seed & 1u ? 0x00u : 0xF0u));
        }
        /* A few valid-looking entries among the noise, one in the very
         * last slot; for one seed so high that the counter space is used up. */
        for (unsigned i = 0; i < 4; i++) {
            uint32_t slot = i == 3 ? 2u * STORE_CTR_ENTRIES - 1u : rnd() % (2u * STORE_CTR_ENTRIES);
            uint32_t v    = seed == 7 && i == 1 ? UINT32_MAX - 10u : rnd();
            uint32_t w    = ~v;
            memcpy(mem + FLASH_CTR_A + slot * STORE_CTR_ENTRY, &v, 4);
            memcpy(mem + FLASH_CTR_A + slot * STORE_CTR_ENTRY + 4u, &w, 4);
        }
        uint32_t max0 = 0;
        bool     f0   = false;
        CHECK(store_ctr_highest(&max0, &f0));
        reboot();
        uint32_t first, limit;
        bool     ok = store_counter_boot(0, &first, &limit);
        if (f0 && max0 > UINT32_MAX - STORE_CTR_AHEAD) {
            CHECK(!ok);
            continue;
        }
        CHECK(ok);
        CHECK(!f0 || first >= max0);
        reboot();
        uint32_t max1;
        bool     f1;
        CHECK(store_ctr_highest(&max1, &f1));
        CHECK(f1 && max1 == limit);
    }
}

/* The whole bridge flow with cuts: create (record, then counter boot), then
 * boots that load the record and reserve. After any cut: either no network
 * (nothing may be sent) or the full network with its key. */
static void test_network_flow_cuts(void) {
    unsigned         cases = 0;
    struct store_net net   = make_net(42, 0);
    flash_stock();
    reboot();
    unsigned n_steps = steps_of_net_write(&net);
    CHECK(n_steps >= 3); /* erase, body, magic */
    for (unsigned k = 1; k <= n_steps + 2u; k++) {
        flash_stock();
        reboot();
        fm.rnd         = k * 13u;
        fm.cut_in      = k;
        bool     wrote = store_net_write(&net, NULL);
        uint32_t first = 0, limit = 0;
        bool     reserved = wrote && store_counter_boot(net.counter_floor, &first, &limit);
        bool     cut      = fm.cut_in == 0;
        reboot();
        struct store_net got;
        uint32_t         seq;
        bool             have = store_net_load(&got, &seq);
        if (!cut) {
            CHECK(have && reserved);
            continue;
        }
        cases++;
        if (have) {
            CHECK(same_net(&got, &net));
            uint32_t f2, l2;
            CHECK(store_counter_boot(got.counter_floor, &f2, &l2));
            CHECK(f2 >= first);
        } else {
            CHECK(!wrote);
        }
    }
    CHECK(cases >= n_steps);
}

int main(void) {
    test_crc();
    test_record_basic();
    test_record_policy();
    test_record_cuts();
    test_net_commit();
    test_counter_floor();
    test_counter_garbage();
    test_counter_switch_cuts();
    test_counter_failing_programs();
    test_counter_failing_erases();
    test_counter_run();
    test_network_flow_cuts();
    return TEST_REPORT("test_store");
}
