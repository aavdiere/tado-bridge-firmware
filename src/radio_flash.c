/* Radio task, flash work and update operations: the flash check at boot
 * and on request (#10), the factory reset (P6), the update's flash
 * operations handed over by the update task (#24), and serving the
 * receiver from inside flash work (#22, src/mx25r.h "Radio port"). The
 * radio task owns SPI1, which the flash shares with the radio. Part of the
 * radio task (src/radio.c); the state it shares, src/radio_internal.h. */
#include "radio.h"

#include "radio_internal.h"

#include "cc110l.h"
#include "fwupd.h"
#include "handoff.h"
#include "log.h"
#include "mx25r.h"
#include "network.h"
#include "watchdog.h"

#include "FreeRTOS.h"
#include "task.h"

#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/gpio.h>

#include <stdbool.h>
#include <stdint.h>

#include "board.h"

/* --- Flash on the shared bus (#10) ----------------------------------------- */
/* Wake the chip, wait for a program or erase still running (an MCU reset
 * does not stop it, and the array reads garbage meanwhile), then check the
 * JEDEC ID. True if the flash may be read: otherwise no record is read. */
bool flash_boot(void) {
    (void)mx25r_wake(); /* its ID check fails while WIP = 1; checked below */
    bool     idle = mx25r_wait_idle();
    uint32_t id   = 0;
    uint8_t  sr   = 0;
    if (idle) {
        id = mx25r_read_id();
        sr = mx25r_read_status();
    }
    bool ok = idle && id == MX25R_JEDEC_ID;
    LOG("flash: JEDEC ID %06lx%s, status register %02x%s",
        (unsigned long)id,
        id == MX25R_JEDEC_ID ? " (MX25R1635F)" : " (UNEXPECTED, want c22815)",
        sr,
        ok     ? ""
        : idle ? " (read failed): flash not used"
               : " (still busy after 480 ms): flash not used");
    return ok;
}

void flash_check(void) {
    bool     in_frame = radio_rx.assembling || gpio_get(GPIOA, BOARD_GDO0_PIN);
    uint32_t id       = mx25r_read_id();
    if (id == MX25R_JEDEC_ID) {
        COUNT(flash_ok);
    } else {
        COUNT(flash_bad);
        LOG("flash: JEDEC ID mismatch: %06lx", (unsigned long)id);
    }
    if (in_frame) {
        COUNT(flash_in_frame);
    }
}

/* The update's flash operations (#24, src/fwupd.h): one at a time, posted
 * by the update task through src/handoff.c, run by this task's loop as
 * flash work (update_op_run() below). The hand-over has a time limit
 * (#24 review M1): an operation this task has not claimed after
 * RADIO_UPDATE_OP_WAIT_MS (src/radio.h, 10 s) is withdrawn and never runs,
 * so the update task never waits for ever on a loop that does not run
 * (radio_configure() failing, src/radio.c). The limit is well above the longest
 * wait in normal operation: the transaction in flight (at most a 1 s wait
 * for its sample, a 1 s train and the ACK window, docs/design.md "Flash
 * work and the coordinator") plus one store write before the operation.
 * src/fwupd_task.c writes the 10 s out in a log line. */

static TaskHandle_t volatile upd_task;

uint32_t handoff_port_ms(void) {
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

void handoff_port_wake(void) {
    xTaskNotify(radio_self, NOTE_UPDATE, eSetBits);
}

void handoff_port_sleep(uint32_t ms) {
    /* Any notification wakes it (a block handed over meanwhile, too):
     * handoff_run() looks at done and the clock. */
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms));
}

bool radio_update_op(struct fwupd_op *op) {
    TickType_t t0 = xTaskGetTickCount();
    while (radio_self == NULL) {
        /* The radio task has not started yet: it sets radio_self first thing. */
        if (xTaskGetTickCount() - t0 >= pdMS_TO_TICKS(RADIO_UPDATE_OP_WAIT_MS)) {
            op->ok   = false;
            op->done = false;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    upd_task = xTaskGetCurrentTaskHandle();
    return handoff_run(op, RADIO_UPDATE_OP_WAIT_MS);
}

bool flash_ready; /* flash_boot() succeeded: the records may be read and written */

/* Factory reset (P6, docs/design.md "Lifecycle"): erase the network record
 * and the counter log, keep the identity, restart. At a quiet moment; the
 * erases (four sectors, up to about 1 s) receive frames meanwhile (#22),
 * which, without a key, are not acknowledged. The restart makes the next
 * boot read what the flash holds: no network after a complete reset
 * ("network: no network record"), or whatever survived a failed one. */
#define FACTORY_LOG_MS 200u /* time for the debugger to read the last RTT lines */

void factory_reset(void) {
    if (!flash_ready) {
        LOG("factory reset: flash not ready at boot: nothing erased");
        return;
    }
    LOG("factory reset: erasing the network record and the counter log; the identity stays");
    bool ok = network_factory_reset();
    LOG("factory reset: %s; restarting",
        ok ? "done" : "FAILED (an erase failed or did not read back blank); hold the button again");
    vTaskDelay(pdMS_TO_TICKS(FACTORY_LOG_MS));
    scb_reset_system(); /* SYSRESETREQ, rm0383.txt:4551-4559 */
}

/* One update operation (#24), at flash_quiet() with the coordinator held,
 * like the store's writes (#22): the driver serves the receiver throughout,
 * so Enhanced ACKs and beacon answers go on; the coordinator starts nothing
 * until the operation is over (at most one sector erase with its blank
 * check, about 250 ms, or one boot-state write). Between two operations
 * the loop runs csl_step() before the update task (lower priority) can
 * post the next one, so the coordinator gets a transaction in between
 * (tests/host/test_csl.c, test_update_windows_under_traffic()). */
void update_op_run(void) {
    struct fwupd_op *op = handoff_claim();
    if (op == NULL) {
        return; /* withdrawn by the update task a moment ago (src/handoff.h) */
    }
    op->network = network_active();
    if (op->kind == FWUPD_OP_RESTART) {
        /* The update task asked: an update was written (PENDING), or the
         * candidate under test was not confirmed in time. At a quiet
         * moment, so no flash write is cut; the bootloader takes over. */
        LOG("update: restarting into the bootloader");
        vTaskDelay(pdMS_TO_TICKS(FACTORY_LOG_MS));
        scb_reset_system(); /* SYSRESETREQ, rm0383.txt:4551-4559 */
    }
    if (flash_ready) {
        (void)fwupd_op_run(op); /* selects its writable area and the default again */
    } else {
        op->ok = false; /* the flash did not answer at boot: nothing is touched */
    }
    handoff_finish(op); /* op->done last: the update task may return at once */
    xTaskNotifyGive(upd_task);
}

/* --- Flash work while frames arrive (#22, src/mx25r.h) -------------------------
 * Flash writes start only at a quiet moment of the task loop (no frame
 * arriving, no coordinator transaction set up: flash_quiet()), but a
 * program cycle takes up to 10 ms and a sector erase up to 240 ms
 * (mx25r1635f.txt:3337-3341), and frames keep coming. The driver therefore calls back here: between
 * its status polls, and instead of starting a bus transaction while a frame arrives or a frame of
 * ours is on the air. A pass here is the receiver's part of a task loop pass: the FIFO is pumped
 * every tick from the sync edge on, so ack_consider() prepares the Enhanced ACK as in the loop, and
 * complete frames are handled (frame_done(): link, beacon answer, log).
 * Not here: the coordinator (csl_step(), so no transaction starts during
 * flash work), the quiet-moment work, and the notifications for it, which
 * stay in pending_bits for the loop. A flash access made from here would
 * nest: there is none, and `serving` stops the nesting anyway. */
bool        radio_running; /* bring-up done, the task loop runs */
static bool serving;

bool mx25r_port_radio_busy(void) {
    return radio_running && (packet_arriving() || cc110l_tx_expected());
}

bool radio_up(void) {
    return radio_running && gdo_irq_on && cc110l_cal_ok();
}

void mx25r_port_radio_serve(void) {
    watchdog_checkin(WDG_RADIO);
    if (!radio_running || serving) {
        vTaskDelay(1);
        return;
    }
    serving         = true;
    uint32_t bits   = 0;
    uint32_t frames = radio_total.frames, acks = radio_total.ack_sent;
    /* As the loop: wake on a GDO interrupt, else at the next tick. */
    (void)xTaskNotifyWait(0, UINT32_MAX, &bits, 1);
    watchdog_checkin(WDG_RADIO);
    bits |= pending_bits;
    pending_bits = 0;
    rx_service(bits);
    pending_bits |= bits & ~RX_SERVICE_BITS;
    COUNT(flash_serve);
    radio_minute.flash_frames += radio_total.frames - frames;
    radio_total.flash_frames += radio_total.frames - frames;
    radio_minute.flash_acks += radio_total.ack_sent - acks;
    radio_total.flash_acks += radio_total.ack_sent - acks;
    (void)cc110l_tx_expected();
    if (!tx_watch()) {
        radio_reinit();
    }
    serving = false;
}
