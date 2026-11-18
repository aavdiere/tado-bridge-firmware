/* Host tests for flash work while frames arrive (#22): the real flash
 * driver, src/mx25r.c, and the store above it (src/store.c through
 * src/store_port.c), against a simulated MX25R1635F on SPI1 and a simulated
 * receiver with random traffic, on one simulated clock.
 *
 * The question: does an erase or program keep the radio task from
 * preparing an Enhanced ACK in time, or put a flash transaction where the
 * ACK interrupt needs the bus? The radio side is modelled as src/radio.c
 * behaves (mx25r_port_radio_busy() and mx25r_port_radio_serve()):
 *   - a frame: preamble and sync word (8 bytes at 160 us), then the GDO0
 *     sync edge; the Data frame's header is out of the FIFO 18 bytes after
 *     the sync edge (length byte, 16 header bytes, and the one byte the pump
 *     leaves in the FIFO); its end, the CRC-OK edge that fires the ACK, is
 *     (1 + len + 2) bytes after the sync edge (src/cc110l.c airtime);
 *   - busy from the sync edge until a serve pass has taken the complete
 *     frame, and during our ACK's airtime and margin (2 ms) after it;
 *   - a serve pass waits for a GDO edge or the next 1 ms tick, then pumps:
 *     a frame whose header is in and which has not ended gets its ACK
 *     prepared (ack_consider()), a frame that has ended is done.
 * Checked for every frame that ends during flash work and needs an ACK:
 * the ACK was prepared before the frame ended. Checked for every flash
 * transaction (CS# low): it started on a quiet radio, and no frame ended
 * (no CRC-OK edge) inside it. The flash model checks the datasheet's rules:
 * no array access, WREN, program or erase while a cycle runs (WIP = 1,
 * mx25r1635f.txt:556-557, 968-969), WEL needed for program and erase. */
#include "../../src/mx25r.c"

#include "mac.h"
#include "store.h"
#include "txgate.h"

#include "test.h"

#include <setjmp.h>
#include <stdlib.h>

/* --- Clock ----------------------------------------------------------------------- */
static uint64_t now_ns;

uint32_t timebase_us(void) {
    return (uint32_t)(now_ns / 1000u);
}

void delay_us(uint32_t us) {
    now_ns += (uint64_t)us * 1000u;
}

static uint32_t rnd_state = 0x12345678u;

static uint32_t rnd(void) {
    uint32_t x = rnd_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rnd_state = x;
    return x;
}

static uint64_t rnd_range(uint64_t lo, uint64_t hi) { /* [lo, hi] */
    return lo + (uint64_t)rnd() % (hi - lo + 1u);
}

/* --- Panics ---------------------------------------------------------------------- */
static jmp_buf panic_env;
static bool    panic_armed;

void fw_panic(const char *what, const char *file, int line) {
    if (panic_armed) {
        panic_armed = false;
        longjmp(panic_env, 1);
    }
    printf("unexpected fw_panic: %s at %s:%d\n", what, file, line);
    exit(1);
}

#define PANICS(stmt, result)          \
    do {                              \
        panic_armed = true;           \
        if (setjmp(panic_env) == 0) { \
            stmt;                     \
            panic_armed = false;      \
            (result)    = false;      \
        } else {                      \
            (result) = true;          \
        }                             \
    } while (0)

/* --- Radio model ------------------------------------------------------------------ */
#define BYTE_NS    160000u /* 50 kBaud */
#define TICK_NS    1000000u
#define ACK_AIR_NS ((uint64_t)((11u + 29u) * 160u + 75u + 2000u) * 1000u) /* src/cc110l.c */
#define MAX_FRAMES 20000u

struct frame {
    uint64_t sync, hdr, end, ack_end;
    bool     needs_ack, considered, done;
    uint64_t considered_at;
};

static struct {
    struct frame f[MAX_FRAMES];
    unsigned     n;
    unsigned     first;       /* frames before this one are done */
    bool         running;     /* radio_running in src/radio.c */
    bool         stuck_high;  /* GDO0 held high: busy for ever */
    uint64_t     edges_until; /* GDO edges up to this time have woken the task */
    unsigned     serves;
    uint64_t     last_serve;
    unsigned     acks_late;  /* frames that ended without their ACK prepared */
    unsigned     acks_ok;    /* frames acknowledged during flash work */
    uint64_t     worst_prep; /* longest header-in to ACK-prepared, ns */
    bool         in_op;      /* the clock is inside a flash call */
} radio;

/* Random traffic from t on, for span ns: frames of 5-128 bytes, gaps of
 * 0.3-20 ms after the previous frame and its ACK; frames of 31 bytes or
 * more (Data frames) mostly addressed to us. */
static void traffic(uint64_t t, uint64_t span) {
    radio.n = radio.first = 0;
    uint64_t p            = t + rnd_range(0, 3000000u);
    while (p < t + span && radio.n < MAX_FRAMES) {
        struct frame *f   = &radio.f[radio.n++];
        unsigned      len = (unsigned)rnd_range(5, 128);
        memset(f, 0, sizeof *f);
        f->sync      = p + 8u * BYTE_NS;
        f->hdr       = f->sync + 18u * BYTE_NS;
        f->end       = f->sync + (uint64_t)(1u + len + 2u) * BYTE_NS;
        f->needs_ack = len >= 31u && rnd() % 10u < 8u;
        f->ack_end   = f->needs_ack ? f->end + ACK_AIR_NS : f->end;
        uint64_t gap = rnd() % 4u == 0 ? 300000u : rnd_range(300000u, 20000000u);
        p            = f->ack_end + gap;
    }
}

static void no_traffic(void) {
    radio.n = radio.first = 0;
}

/* Frames that end now are done only when served; skip the old ones. */
static bool radio_busy_at(uint64_t t) {
    if (radio.stuck_high) {
        return true;
    }
    for (unsigned i = radio.first; i < radio.n && radio.f[i].sync <= t; i++) {
        const struct frame *f = &radio.f[i];
        if (t < f->end || !f->done || (f->considered && t < f->ack_end)) {
            return true;
        }
    }
    return false;
}

bool mx25r_port_radio_busy(void) {
    return radio.running && radio_busy_at(now_ns);
}

/* The first GDO edge (sync or end) after t, or UINT64_MAX. */
static uint64_t next_edge(uint64_t t) {
    for (unsigned i = radio.first; i < radio.n; i++) {
        if (radio.f[i].sync > t) {
            return radio.f[i].sync;
        }
        if (radio.f[i].end > t) {
            return radio.f[i].end;
        }
    }
    return UINT64_MAX;
}

static void pump(void) {
    for (unsigned i = radio.first; i < radio.n && radio.f[i].sync <= now_ns; i++) {
        struct frame *f = &radio.f[i];
        if (f->done) {
            continue;
        }
        if (f->needs_ack && !f->considered && f->hdr <= now_ns && now_ns < f->end) {
            f->considered    = true;
            f->considered_at = now_ns;
            if (now_ns - f->hdr > radio.worst_prep) {
                radio.worst_prep = now_ns - f->hdr;
            }
        }
        if (f->end <= now_ns) {
            f->done = true;
            if (f->needs_ack) {
                if (f->considered) {
                    radio.acks_ok++;
                } else {
                    radio.acks_late++;
                }
            }
        }
    }
    while (radio.first < radio.n && radio.f[radio.first].done) {
        radio.first++;
    }
}

/* A flash call that never ends (a gate that never gives up) fails the
 * test instead of hanging it. */
static uint64_t deadline_ns = UINT64_MAX;

void mx25r_port_radio_serve(void) {
    if (now_ns > deadline_ns) {
        printf("FAIL test_mx25r.c: a flash call did not end by its deadline\n");
        exit(1);
    }
    radio.serves++;
    radio.last_serve = now_ns;
    if (!radio.running) {
        now_ns = (now_ns / TICK_NS + 1u) * TICK_NS; /* vTaskDelay(1) */
        return;
    }
    /* xTaskNotifyWait(1 tick): back at once for an edge that came since the
     * last wake, else at the next edge or tick, a few us later. */
    uint64_t e = next_edge(radio.edges_until);
    if (e > now_ns) {
        uint64_t tick = (now_ns / TICK_NS + 1u) * TICK_NS;
        now_ns        = (e < tick ? e : tick) + 3000u;
    } else {
        now_ns += 2000u;
    }
    radio.edges_until = now_ns;
    pump();
    now_ns += 20000u; /* the pass itself: FIFO reads, at most a frame's handling */
}

/* --- Flash model on SPI1 ----------------------------------------------------------- */
#define SPI_BYTE_NS 1334u /* 8 bits at 6 MHz */

static uint8_t mem[FLASH_SIZE];

static struct {
    bool     sel;
    unsigned nbytes;
    uint8_t  op;
    uint32_t addr;
    bool     wel, wip, wip_stuck;
    uint64_t wip_until;
    uint8_t  scur;
    uint8_t  buf[FLASH_PAGE];
    unsigned buf_n;
    bool     erase; /* the cycle running is an erase */
    uint32_t cyc_addr;
    uint8_t  cyc_buf[FLASH_PAGE]; /* the program's data, latched at CS# high */
    unsigned cyc_n;
    unsigned fail_next; /* 1: the next cycle sets its fail flag and changes nothing */
    uint64_t tpp_min, tpp_max, tse_min, tse_max;
    uint64_t cs_start;
    unsigned violations;    /* datasheet rules broken by the driver */
    unsigned cs_while_busy; /* transactions started on a busy radio */
    unsigned cs_over_end;   /* transactions with a CRC-OK edge inside */
    unsigned transactions;
    unsigned locks;
} fl;

static void fl_update(void) {
    if (fl.wip && !fl.wip_stuck && now_ns >= fl.wip_until) {
        fl.wip = false;
        fl.wel = false;
        if (fl.fail_next) {
            fl.fail_next = 0;
            fl.scur |= (uint8_t)(fl.erase ? SCUR_EFAIL : SCUR_PFAIL);
        } else if (fl.erase) {
            memset(mem + fl.cyc_addr, 0xFF, FLASH_SECTOR);
        } else {
            for (unsigned i = 0; i < fl.cyc_n; i++) {
                uint32_t a = (fl.cyc_addr & ~(FLASH_PAGE - 1u)) | ((fl.cyc_addr + i) % FLASH_PAGE);
                mem[a] &= fl.cyc_buf[i];
            }
        }
    }
}

void spi1_lock(void) {
    CHECK_EQ(fl.locks, 0);
    fl.locks++;
}

void spi1_unlock(void) {
    CHECK_EQ(fl.locks, 1);
    fl.locks--;
}

void spi1_select(enum spi1_dev dev) {
    CHECK(dev == SPI1_DEV_FLASH);
    CHECK(!fl.sel);
    now_ns += 500u;
    fl_update();
    fl.sel      = true;
    fl.nbytes   = 0;
    fl.cs_start = now_ns;
    fl.transactions++;
    /* As the gate saw it, just before (a sync edge may come in between:
     * its frame ends milliseconds later, see cs_over_end). */
    if (radio.running && radio_busy_at(now_ns - 600u)) {
        fl.cs_while_busy++;
    }
}

static void start_cycle(bool erase, uint64_t lo, uint64_t hi) {
    fl.wip      = true;
    fl.erase    = erase;
    fl.cyc_addr = fl.addr;
    fl.cyc_n    = fl.buf_n;
    memcpy(fl.cyc_buf, fl.buf, sizeof fl.buf);
    fl.wip_until = now_ns + rnd_range(lo, hi);
    fl.scur &= (uint8_t)~(SCUR_PFAIL | SCUR_EFAIL);
}

void spi1_deselect(enum spi1_dev dev) {
    CHECK(dev == SPI1_DEV_FLASH);
    CHECK(fl.sel);
    now_ns += 500u;
    fl.sel = false;
    /* No CRC-OK edge (ACK interrupt) inside the transaction. */
    for (unsigned i = radio.first; i < radio.n && radio.f[i].sync <= now_ns; i++) {
        if (radio.f[i].end >= fl.cs_start && radio.f[i].end <= now_ns) {
            fl.cs_over_end++;
        }
    }
    bool whole = fl.nbytes >= 1u;
    switch (whole ? fl.op : 0) {
        case OP_WREN:
            if (fl.wip) {
                fl.violations++;
            } else if (fl.nbytes == 1u) {
                fl.wel = true;
            }
            break;
        case OP_WRDI:
            if (fl.nbytes == 1u) {
                fl.wel = false;
            }
            break;
        case OP_PP:
            if (fl.wip || fl.nbytes < 5u) {
                fl.violations++;
            } else if (fl.wel) {
                start_cycle(false, fl.tpp_min, fl.tpp_max);
            }
            break;
        case OP_SE:
            if (fl.wip || fl.nbytes != 4u || fl.addr % FLASH_SECTOR) {
                fl.violations++;
            } else if (fl.wel) {
                start_cycle(true, fl.tse_min, fl.tse_max);
            }
            break;
        default:
            break;
    }
}

uint8_t spi1_xfer(uint8_t out) {
    CHECK(fl.sel);
    now_ns += SPI_BYTE_NS;
    fl_update();
    unsigned i = fl.nbytes++;
    if (i == 0) {
        fl.op     = out;
        fl.addr   = 0;
        fl.buf_n  = 0;
        bool busy = fl.wip && out != OP_RDSR && out != OP_RDSCUR;
        if (busy) {
            fl.violations++; /* only RDSR and RDSCUR while WIP = 1 */
        }
        return 0xFF;
    }
    switch (fl.op) {
        case OP_RDSR:
            return (uint8_t)((fl.wip ? SR_WIP : 0u) | (fl.wel ? SR_WEL : 0u));
        case OP_RDSCUR:
            return fl.scur;
        case OP_RDID:
            return i == 1u ? 0xC2 : i == 2u ? 0x28 : 0x15;
        case OP_READ:
        case OP_PP:
        case OP_SE:
            if (i <= 3u) {
                fl.addr = fl.addr << 8 | out;
                return 0xFF;
            }
            if (fl.op == OP_READ) {
                return mem[(fl.addr + (i - 4u)) % FLASH_SIZE];
            }
            if (fl.op == OP_PP && fl.buf_n < FLASH_PAGE) {
                fl.buf[fl.buf_n++] = out;
            }
            return 0xFF;
        default:
            return 0xFF;
    }
}

/* ULP mode (the driver never leaves it): tPP 3.2 typical, 10 ms max; tSE
 * 58 typical, 240 ms max (mx25r1635f.txt:3337-3341). The model draws from
 * a range below the maximum, so the driver's 2x timeouts never trip. */
static void flash_reset_model(void) {
    memset(&fl, 0, sizeof fl);
    fl.tpp_min = 100000u;
    fl.tpp_max = 10000000u;
    fl.tse_min = 40000000u;
    fl.tse_max = 240000000u;
    memset(&stats, 0, sizeof stats);
    radio_stuck = false;
}

static void radio_reset(bool running) {
    memset(&radio, 0, sizeof radio);
    radio.running     = running;
    radio.edges_until = now_ns;
}

/* After a flash call: the task loop takes over the frames that are still
 * arriving; account for those that ended inside the call. */
static void op_done(void) {
    pump();
}

static void check_clean(void) {
    CHECK_EQ(fl.violations, 0);
    CHECK_EQ(fl.cs_while_busy, 0);
    CHECK_EQ(fl.cs_over_end, 0);
    CHECK_EQ(radio.acks_late, 0);
    CHECK(!fl.sel);
    CHECK_EQ(fl.locks, 0);
}

/* --- Tests ------------------------------------------------------------------------- */
/* Boot: the radio is not running yet; a cycle left over from before the
 * reset is waited out, then program and erase work. */
static void test_no_radio(void) {
    flash_reset_model();
    radio_reset(false);
    memset(mem, 0xFF, sizeof mem);
    fl.wip       = true; /* a cycle an MCU reset did not stop */
    fl.erase     = true;
    fl.cyc_addr  = FLASH_CTR_A;
    fl.wip_until = now_ns + 150000000u;
    uint64_t t0  = now_ns;
    CHECK(mx25r_wait_idle());
    CHECK(now_ns - t0 >= 150000000u && now_ns - t0 < 152000000u);
    CHECK(mx25r_wake());

    uint8_t data[MX25R_XFER_MAX], back[MX25R_XFER_MAX];
    for (unsigned i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)(i * 7u + 1u);
    }
    unsigned serves = radio.serves;
    CHECK(mx25r_program(FLASH_NET_A + 0x40u, data, sizeof data));
    CHECK(radio.serves > serves); /* the CPU went to other tasks */
    CHECK(mx25r_read(FLASH_NET_A + 0x40u, back, sizeof back));
    CHECK_MEM(back, data, sizeof data);
    CHECK(mx25r_erase_sector(FLASH_NET_A));
    CHECK(mx25r_read(FLASH_NET_A + 0x40u, back, sizeof back));
    for (unsigned i = 0; i < sizeof back; i++) {
        CHECK_EQ(back[i], 0xFF);
    }
    CHECK_EQ(stats.cycles, 2);
    CHECK(stats.cycle_max >= 40000u && stats.cycle_max <= 242000u);
    CHECK_EQ(stats.held, 0);
    check_clean();
}

/* Random programs and erases under heavy random traffic. */
static void test_traffic(void) {
    flash_reset_model();
    memset(mem, 0xFF, sizeof mem);
    radio_reset(true);
    traffic(now_ns, 60ull * 1000000000u); /* a minute of air */
    uint64_t t_end     = now_ns + 55ull * 1000000000u;
    unsigned ops       = 0;
    unsigned next_page = 0;
    uint8_t  data[MX25R_XFER_MAX], back[MX25R_XFER_MAX];
    while (now_ns < t_end) {
        /* The task loop between flash calls, which serves the receiver;
         * flash work starts at a quiet moment only. */
        for (uint64_t until = now_ns + rnd_range(0, 3000000u); now_ns < until;) {
            mx25r_port_radio_serve();
        }
        if (mx25r_port_radio_busy()) {
            continue;
        }
        uint32_t sector = FLASH_WRITE_BEGIN + (uint32_t)(rnd() % 4u) * FLASH_SECTOR;
        if (rnd() % 8u == 0) {
            CHECK(mx25r_erase_sector(sector));
            op_done();
            CHECK(mx25r_read(sector + (rnd() % 64u) * 64u, back, sizeof back));
            op_done();
            for (unsigned i = 0; i < sizeof back; i++) {
                CHECK_EQ(back[i], 0xFF);
            }
        } else {
            /* A fresh 64-byte chunk somewhere in the writable range. */
            uint32_t addr = FLASH_WRITE_BEGIN + (next_page++ % 256u) * 64u;
            if (!mx25r_erase_sector(addr & ~(FLASH_SECTOR - 1u))) {
                CHECK(false);
            }
            op_done();
            size_t n = 1u + rnd() % MX25R_XFER_MAX;
            for (size_t i = 0; i < n; i++) {
                data[i] = (uint8_t)rnd();
            }
            CHECK(mx25r_program(addr, data, n));
            op_done();
            CHECK(mx25r_read(addr, back, n));
            op_done();
            CHECK_MEM(back, data, n);
        }
        ops++;
    }
    check_clean();
    CHECK(ops > 50u);
    CHECK(radio.acks_ok > 500u); /* the test means something */
    CHECK(stats.held > 50u);
    CHECK_EQ(stats.gave_up, 0);
    /* The ACK is prepared within one tick and a pass of the header being in. */
    CHECK(radio.worst_prep <= TICK_NS + 30000u);
    printf("test_mx25r: traffic: %u flash calls, %u frames acked during flash work, %u held "
           "transactions (longest %u us), header to ACK prepared at most %llu us\n",
           ops,
           radio.acks_ok,
           stats.held,
           stats.held_max,
           (unsigned long long)(radio.worst_prep / 1000u));
}

/* The store above the driver: a network record, then the counter log past
 * a sector change (an erase), under traffic. */
static void test_store_traffic(void) {
    flash_reset_model();
    memset(mem, 0xFF, sizeof mem);
    radio_reset(true);
    traffic(now_ns, 400ull * 1000000000u);
    struct store_net n = {.pan_id = MAC_PAN_ID, .channel = TXGATE_TEST_CHANNEL};
    for (unsigned i = 0; i < CCM_KEY_LEN; i++) {
        n.key[i] = (uint8_t)(i + 1u);
    }
    struct store_net got;
    uint32_t         seq = 0;
    CHECK(store_net_commit(&n, &got, &seq));
    op_done();
    CHECK_EQ(seq, 1);
    uint32_t first = 0, limit = 0;
    CHECK(store_counter_boot(0, &first, &limit));
    op_done();
    unsigned erases = stats.cycles;
    for (unsigned k = 0; k < 600u; k++) { /* 512 entries per sector */
        while (mx25r_port_radio_busy()) {
            mx25r_port_radio_serve();
        }
        CHECK(store_counter_extend(limit - 1u, &limit));
        op_done();
    }
    CHECK(stats.cycles > erases + 600u); /* programs and at least one erase */
    uint32_t max   = 0;
    bool     found = false;
    store_ctr_forget();
    CHECK(store_ctr_highest(&max, &found));
    CHECK(found && max == limit);
    check_clean();
    CHECK(radio.acks_ok > 100u);
}

/* GDO0 stuck high: the flash still works, after one wait of
 * MX25R_GATE_MAX_US, not one per transaction. */
static void test_stuck(void) {
    flash_reset_model();
    memset(mem, 0xFF, sizeof mem);
    radio_reset(true);
    no_traffic();
    radio.stuck_high = true;
    uint8_t  data[8] = {1, 2, 3, 4, 5, 6, 7, 8}, back[8];
    uint64_t t0      = now_ns;
    deadline_ns      = now_ns + 10ull * 1000000000u;
    CHECK(mx25r_program(FLASH_CTR_B, data, sizeof data));
    CHECK(mx25r_read(FLASH_CTR_B, back, sizeof back));
    CHECK_MEM(back, data, sizeof data);
    CHECK_EQ(stats.gave_up, 1);
    CHECK(now_ns - t0 < (MX25R_GATE_MAX_US + 12000u) * 1000ull);
    /* Seen quiet again: the gate waits again next time. */
    radio.stuck_high = false;
    CHECK(mx25r_read(FLASH_CTR_B, back, sizeof back));
    CHECK(!radio_stuck);
    CHECK_EQ(fl.violations, 0);
    deadline_ns = UINT64_MAX;
}

/* A cycle that never ends: false after twice the worst case, with the
 * radio served (and the watchdog fed) all along. */
static void test_timeouts(void) {
    flash_reset_model();
    memset(mem, 0xFF, sizeof mem);
    radio_reset(true);
    traffic(now_ns, 5ull * 1000000000u);
    uint8_t data[4] = {0, 1, 2, 3};
    fl.wip_stuck    = true;
    uint64_t t0     = now_ns;
    CHECK(!mx25r_program(FLASH_CTR_A, data, sizeof data));
    op_done();
    uint64_t dt = now_ns - t0;
    CHECK(dt >= PP_TIMEOUT_US * 1000ull && dt < (PP_TIMEOUT_US + 3000u) * 1000ull);
    fl.wip_stuck = false;
    while (fl.wip) {
        now_ns += 1000000u;
        fl_update();
    }
    fl.wip_stuck = true;
    t0           = now_ns;
    CHECK(!mx25r_erase_sector(FLASH_CTR_A));
    op_done();
    dt = now_ns - t0;
    CHECK(dt >= SE_TIMEOUT_US * 1000ull && dt < (SE_TIMEOUT_US + 3000u) * 1000ull);
    /* About one serve per tick throughout. */
    CHECK(radio.serves >= 450u);
    fl.wip_stuck = false;
    while (fl.wip) {
        now_ns += 1000000u;
        fl_update();
    }
    check_clean();
}

/* The fail flags of the datasheet flow still end a write as failed. */
static void test_fail_flags(void) {
    flash_reset_model();
    memset(mem, 0xFF, sizeof mem);
    radio_reset(true);
    no_traffic();
    uint8_t data[4] = {0, 1, 2, 3};
    fl.fail_next    = 1;
    CHECK(!mx25r_program(FLASH_CTR_A, data, sizeof data));
    CHECK(mx25r_program(FLASH_CTR_A, data, sizeof data));
    fl.fail_next = 1;
    CHECK(!mx25r_erase_sector(FLASH_CTR_A));
    CHECK(mx25r_erase_sector(FLASH_CTR_A));
    check_clean();
}

/* The address guard is unchanged: nothing below the network sectors (the
 * identity) or above the counter log. */
static void test_guard(void) {
    flash_reset_model();
    radio_reset(true);
    no_traffic();
    uint8_t data[4] = {0};
    bool    p       = false;
    PANICS((void)mx25r_erase_sector(FLASH_ID_A), p);
    CHECK(p);
    PANICS((void)mx25r_erase_sector(FLASH_ID_B), p);
    CHECK(p);
    PANICS((void)mx25r_program(FLASH_ID_B + FLASH_SECTOR - 4u, data, sizeof data), p);
    CHECK(p);
    PANICS((void)mx25r_erase_sector(FLASH_WRITE_END), p);
    CHECK(p);
    PANICS((void)mx25r_program(FLASH_CTR_A + 254u, data, sizeof data), p); /* crosses a page */
    CHECK(p);
    CHECK_EQ(fl.transactions, 0); /* refused before the bus */
    fl.locks = 0;
}

int main(void) {
    test_no_radio();
    test_traffic();
    test_store_traffic();
    test_stuck();
    test_timeouts();
    test_fail_flags();
    test_guard();
    return TEST_REPORT("test_mx25r");
}
