/* The update task (#24). See fwupd_task.h, src/fwupd.h and docs/design.md
 * "Implementation: self-update, application side (#24)". */
#include "fwupd_task.h"

#include "fwimg.h"
#include "image_desc.h"
#include "log.h"
#include "radio.h"
#include "session.h"

#include "FreeRTOS.h"
#include "task.h"

#include <libopencm3/cm3/scb.h>

#include <string.h>

/* Below the radio and net tasks, as housekeeping (src/app.c): SHA-256 over
 * up to 448 KB runs here and must never hold up a frame, an ACK or the
 * TLS session. Equal to housekeeping, so time slicing shares the CPU with
 * the watchdog kicks while it hashes. */
#define PRIO_UPDATE (tskIDLE_PRIORITY + 1)
/* 2 KB. Deepest path [I]: fwupd_task 48 + fwupd_block 96 + finish 64 +
 * slot_valid 112 (header copy, hash) + slot_hash 56 + mbedtls_sha256_update
 * and the block function about 400, or LOG() about 1 KB with newlib's
 * formatter (as in every task here, src/app.c). Measured on the bench by
 * the "alive:" line. Static, so the FreeRTOS heap margin at the TLS
 * handshake (docs/design.md "RAM measured") stays as it was. */
#define STACK_WORDS 512
#define TICK_MS     1000u
/* The net task's wake-up after a block (any bit: src/net.c only wakes). */
#define NET_WAKE (1u << 16)

static StaticTask_t tcb;
static StackType_t  stack[STACK_WORDS];
static TaskHandle_t self, net;

/* Hand-over with the net task (fwupd_task.h). stage moves IDLE -> SUBMITTED
 * (net task), SUBMITTED -> RESULT (this task), RESULT -> IDLE (net task);
 * each side writes the data before it moves stage on and reads it after
 * seeing stage, with a barrier between (single core: the barrier keeps the
 * compiler's order). */
enum { IDLE, SUBMITTED, RESULT };
static volatile uint8_t stage;
static uint8_t          blk[FWUPD_BLOCK];
static struct {
    uint32_t num;
    bool     more;
    size_t   len;
    bool     session_ok;
} req;
static enum fwupd_rc res;
static char          res_why[96];
static volatile bool abort_req;

#define BARRIER() __asm__ volatile("dmb" ::: "memory")

static uint32_t ms(void) {
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* --- Port ------------------------------------------------------------------------ */
bool fwupd_port_op(struct fwupd_op *op) {
    return radio_update_op(op); /* the radio task runs it (it owns SPI1) */
}

void fwupd_port_lock(void) {
    taskENTER_CRITICAL();
}

void fwupd_port_unlock(void) {
    taskEXIT_CRITICAL();
}

/* fwimg_slot_verify() is the bootloader's; the application checks slots
 * through fwupd's operations and never calls it. */
void fwimg_port_progress(void) {}

/* --- Net task side --------------------------------------------------------------- */
bool fwupd_task_busy(void) {
    return stage != IDLE;
}

bool fwupd_task_submit(uint32_t num, bool more, const uint8_t *data, size_t len, bool session_ok) {
    if (stage != IDLE || len > FWUPD_BLOCK || self == NULL) {
        return false;
    }
    net = xTaskGetCurrentTaskHandle();
    if (len > 0) {
        memcpy(blk, data, len);
    }
    req.num        = num;
    req.more       = more;
    req.len        = len;
    req.session_ok = session_ok;
    BARRIER();
    stage = SUBMITTED;
    xTaskNotifyGive(self);
    return true;
}

bool fwupd_task_result(enum fwupd_rc *rc, char *why, size_t cap) {
    if (stage != RESULT) {
        return false;
    }
    BARRIER();
    *rc = res;
    if (cap > 0) {
        strncpy(why, res_why, cap - 1u);
        why[cap - 1u] = '\0';
    }
    BARRIER();
    stage = IDLE;
    return true;
}

void fwupd_task_abort(void) {
    abort_req = true;
    if (self) {
        xTaskNotifyGive(self);
    }
}

/* --- The task --------------------------------------------------------------------- */
static const char *const rc_name[] = {
    [FWUPD_CONTINUE]   = "2.31",
    [FWUPD_DONE]       = "2.04",
    [FWUPD_BAD]        = "4.00",
    [FWUPD_REFUSED]    = "4.03",
    [FWUPD_INCOMPLETE] = "4.08",
    [FWUPD_TOO_LARGE]  = "4.13",
    [FWUPD_FAILED]     = "5.00",
};

static void hex8(char *out, const uint8_t *p) {
    static const char d[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8u; i++) {
        out[2u * i]      = d[p[i] >> 4];
        out[2u * i + 1u] = d[p[i] & 15u];
    }
    out[16] = '\0';
}

static void block(void) {
    BARRIER();
    uint32_t          t0 = ms();
    enum fwupd_rc     rc = fwupd_block(req.num, req.more, blk, req.len, req.session_ok, t0);
    struct fwupd_info in;
    fwupd_info(&in);
    /* Block 0, every 64th block (64 KB), the last and every refusal. */
    if (req.num == 0 || rc != FWUPD_CONTINUE || req.num % 64u == 0) {
        LOG("update: block %lu of %lu (%u bytes): %s, %s (%lu ms)",
            (unsigned long)req.num,
            (unsigned long)in.blocks,
            (unsigned)req.len,
            rc_name[rc],
            fwupd_why(),
            (unsigned long)(ms() - t0));
    }
    res = rc;
    strncpy(res_why, fwupd_why(), sizeof res_why - 1u);
    res_why[sizeof res_why - 1u] = '\0';
    BARRIER();
    stage = RESULT;
    if (net) {
        (void)xTaskNotify(net, NET_WAKE, eSetBits);
    }
}

/* The restart: an update was written (PENDING), or the candidate under
 * test, or an image whose boot state could not be read, was not confirmed
 * within FWUPD_TEST_MAX_MS. Asked of the radio task first, which restarts
 * at a quiet moment (no flash write or frame of ours cut). If it has not
 * taken the request in 10 s, its loop does not run (the radio did not come
 * up, #24 review M1), and this task restarts the MCU itself, by the same
 * SYSRESETREQ. No flash write should be under way then: the radio task's
 * flash writes run in that same loop, at the same quiet moment as the
 * update's operations and just before them, each bounded at about 1 s
 * (src/radio.c); and the records survive a cut write (src/store.h). The
 * IWDG stays satisfied throughout: the radio task checks in during its
 * bring-up retries, and nothing here blocks another task. */
#define RESTART_LOG_MS 200u /* time for the debugger to read the last RTT lines */

static void restart(void) {
    struct fwupd_op o = {.kind = FWUPD_OP_RESTART};
    (void)radio_update_op(&o); /* taken: the radio task restarts; does not come back */
    LOG("update: the radio task did not take the restart in 10 s: restarting from the update "
        "task");
    vTaskDelay(pdMS_TO_TICKS(RESTART_LOG_MS));
    scb_reset_system(); /* SYSRESETREQ, rm0383.txt:4551-4559; does not return */
}

static void task(void *arg) {
    (void)arg;
    uint32_t t = ms();
    fwupd_init((const uint8_t *)FWIMG_APP_BASE, image_size(), image_desc.fw_version, t);
    struct fwupd_info in;
    fwupd_info(&in);
    char run[17], cand[17], prev[17];
    hex8(run, in.run_id);
    hex8(cand, in.boot.cand_id);
    hex8(prev, in.boot.prev_id);
    /* One log line holds 191 characters (src/log.c): ids as 16 hex digits. */
    LOG("update: running %s (%lu B); boot state %s tries %u cand %s prev %s; %s",
        run,
        (unsigned long)image_size(),
        in.boot_read ? fwupd_state_name(in.boot.state) : "unread",
        (unsigned)in.boot.tries,
        cand,
        prev,
        fwupd_why());
    uint32_t last_tick = t;
    for (;;) {
        if (abort_req) {
            abort_req = false;
            fwupd_info(&in);
            fwupd_abort();
            if (in.active) {
                LOG("update: transfer dropped at block %lu: the server session ended",
                    (unsigned long)in.next);
            }
        }
        if (stage == SUBMITTED) {
            block();
            continue;
        }
        t = ms();
        if (t - last_tick >= TICK_MS) {
            last_tick          = t;
            enum fwupd_event e = fwupd_tick(t, session_ready() && radio_up());
            if (e == FWUPD_EV_CONFIRMED) {
                LOG("update: running image %s... confirmed (boot state CONFIRMED) after %u s "
                    "with the server session ready and the radio up",
                    run,
                    FWUPD_CONFIRM_MS / 1000u);
            } else if (e != FWUPD_EV_NONE) {
                LOG("update: %s", fwupd_why());
            }
            if (e == FWUPD_EV_RESTART) {
                restart();
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TICK_MS));
    }
}

void fwupd_task_create(void) {
    self = xTaskCreateStatic(task, "update", STACK_WORDS, NULL, PRIO_UPDATE, stack, &tcb);
    configASSERT(self != NULL);
}

unsigned fwupd_task_stack_free(void) {
    return (unsigned)uxTaskGetStackHighWaterMark(self);
}
