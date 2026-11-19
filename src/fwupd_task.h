#ifndef FWUPD_TASK_H
#define FWUPD_TASK_H

/* The update task (#24): runs the update logic of src/fwupd.h at low
 * priority, hands each flash operation to the radio task
 * (radio_update_op(), src/radio.h) and waits for it, and takes the blocks
 * of POST b/fw from the net task (src/session.c), one at a time.
 *
 * Hand-over with the net task, one block at a time:
 *   net:    fwupd_task_submit()  (only while !fwupd_task_busy())
 *   update: fwupd_block(), then the result and a wake-up of the net task
 *   net:    fwupd_task_result()  (answers the request), and the next block.
 * The block's bytes stay in this module's buffer until the result is
 * taken, so the net task never writes them while the radio task programs
 * them. */

#include "fwupd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Create the task (app.c, before the scheduler). Statically allocated. */
void fwupd_task_create(void);
/* For the "alive:" line: its stack words never used. */
unsigned fwupd_task_stack_free(void);

/* Net task. A block is being processed or its result not yet taken. */
bool fwupd_task_busy(void);
/* Net task: hand over one block (copied). False if busy or len > FWUPD_BLOCK.
 * session_ok: the server session is ready. The net task is woken when the
 * result is there. */
bool fwupd_task_submit(uint32_t num, bool more, const uint8_t *data, size_t len, bool session_ok);
/* Net task: the result of the block handed over last, once there; why: its
 * reason (fwupd_why()), NUL terminated. True once per block. */
bool fwupd_task_result(enum fwupd_rc *rc, char *why, size_t cap);
/* Net task: the session ended; the transfer is dropped (nothing written). */
void fwupd_task_abort(void);

#endif
