#ifndef FWUPD_H
#define FWUPD_H

/* The application's side of a firmware update (#24): docs/design.md
 * "Implementation: self-update, application side (#24)". Hardware
 * independent; host-tested in tests/host/test_fwupd.c (with the real
 * bootloader decisions, src/boot/bootctl.c, and power cuts) and, for the
 * flash operations under radio traffic, tests/host/test_mx25r.c.
 *
 * Two halves, in two tasks:
 *   - The update logic (fwupd_init(), fwupd_block(), fwupd_tick()) runs in
 *     the update task (src/fwupd_task.c), at low priority. It receives the
 *     image as CoAP Block1 blocks of 1024 bytes over the server session,
 *     writes it into the candidate slot, saves the running image into the
 *     previous slot, writes the boot state PENDING and asks for a restart;
 *     after the bootloader installed a candidate (TESTING) it confirms it
 *     once the bridge has been healthy for FWUPD_CONFIRM_MS. SHA-256 and all
 *     the bookkeeping run here, never in the radio task.
 *   - Every flash access is one bounded operation (struct fwupd_op) that
 *     the update logic hands to fwupd_port_op(). On the bridge the radio
 *     task, which owns SPI1, runs it with fwupd_op_run() at a moment for
 *     flash work (no frame arriving, the coordinator idle and held), as it
 *     runs the store's writes (#22, docs/design.md "Flash work and the
 *     coordinator"). The longest operation is one 4 KB sector erase with
 *     its blank check (up to about 250 ms) or one boot-state record write
 *     (one erase and a few programs); between two operations the
 *     coordinator gets a pass, so downlinks and trains are never held for
 *     longer than one operation.
 *
 * Wire format (our own, docs/design.md "Delivery"): POST b/fw, Block1 with
 * SZX 6 (1024 bytes). Block 0 is the 64-byte slot header (src/fwimg.h)
 * followed by zeros; block n >= 1 is the image at offset (n - 1) x 1024;
 * the last block is shorter if the image is. The header is written to the
 * candidate slot last, after the SHA-256 of the whole image, read back from
 * the slot, matched it, so an update cut short never leaves a valid slot.
 *
 * Writes: only the two update slots and the boot state record, enforced by
 * fwupd_op_allowed() and, below it, by the flash driver's areas
 * (src/mx25r.h mx25r_area()). The running image in internal flash is
 * never written (only the bootloader writes internal flash). */

#include "fwimg.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FWUPD_BLOCK 1024u /* Block1 block size */
#define FWUPD_SZX   6u    /* 2^(6 + 4) = 1024 (RFC 7959 2.2) */
#define FWUPD_BLOCKS                                                            \
    ((FWIMG_APP_MAX + FWUPD_BLOCK - 1u) / FWUPD_BLOCK) /* image blocks, at most \
                                                        */

#define FWUPD_PROG_MAX 256u  /* one program operation: at most one 256-byte page */
#define FWUPD_READ_MAX 1024u /* one read operation */

#define FWUPD_CONFIRM_MS  60000u  /* healthy this long in TESTING: CONFIRMED */
#define FWUPD_TEST_MAX_MS 600000u /* not confirmed this long after the boot: restart */
#define FWUPD_IDLE_MS     120000u /* a transfer without a block this long is dropped */
#define FWUPD_RESTART_MS  3000u   /* after the last block: time for its answer to go out */
#define FWUPD_RETRY_MS    10000u  /* after a failed CONFIRMED write or boot-state read */

/* --- Flash operations (the radio task runs them) ------------------------------- */
enum fwupd_op_kind {
    FWUPD_OP_READ = 1,   /* len 1..FWUPD_READ_MAX bytes inside one slot into dst */
    FWUPD_OP_ERASE,      /* the 4 KB sector at addr inside a slot, then read back blank */
    FWUPD_OP_PROGRAM,    /* len 1..FWUPD_PROG_MAX bytes from src inside one page of a slot,
                          * then read back and compared */
    FWUPD_OP_BOOT_LOAD,  /* fwimg_boot_load() into boot; found: a record was there */
    FWUPD_OP_BOOT_WRITE, /* fwimg_boot_write(boot): PENDING or CONFIRMED, tries 0, only */
    FWUPD_OP_RESTART,    /* restart the MCU at the radio task's next quiet moment */
};

struct fwupd_op {
    uint8_t           kind; /* enum fwupd_op_kind */
    uint32_t          addr;
    uint32_t          len;
    uint8_t          *dst;
    const uint8_t    *src;
    struct fwimg_boot boot;
    bool              found;
    bool              network; /* filled in by the radio task: a network is active */
    bool              ok;
    volatile bool     done; /* set by the radio task, last */
};

/* The operation's addresses and contents are ones the update may use: the
 * checks above. Nothing outside the two slots, and in the boot state only
 * the two records the application writes. */
bool fwupd_op_allowed(const struct fwupd_op *op);

/* Run one operation through store_port_* (the flash driver) and fwimg_boot_*;
 * not FWUPD_OP_RESTART. Selects the driver's writable area for it
 * (fwupd_port_area(): the slots for an erase or program, the boot state for
 * a boot-state write) and the default again afterwards. Sets and returns
 * op->ok; false at once, with no flash access, if !fwupd_op_allowed(op).
 * Radio task. */
bool fwupd_op_run(struct fwupd_op *op);

/* The flash driver's writable areas (src/mx25r.h enum mx25r_area, same
 * values). */
enum fwupd_area {
    FWUPD_AREA_DEFAULT = 0, /* the records: the store's area; nothing of the update's */
    FWUPD_AREA_BOOT    = 1,
    FWUPD_AREA_SLOTS   = 2,
};
/* Select the area (src/store_port.c: mx25r_area()). Radio task. */
void fwupd_port_area(enum fwupd_area a);

/* --- Port (src/fwupd_task.c; the host tests run operations directly) -------- */
/* Run op (the radio task does it on the bridge) and wait for it: op->ok. */
bool fwupd_port_op(struct fwupd_op *op);
/* Around changes of the status that fwupd_info() copies (another task reads it). */
void fwupd_port_lock(void);
void fwupd_port_unlock(void);

/* --- Update logic (update task) ------------------------------------------------ */
/* The running image: its bytes (internal flash at FWIMG_APP_BASE, image_size()
 * bytes), its descriptor's fw_version; now_ms: the tick clock. Hashes the
 * image (its id is the first FWIMG_ID_LEN bytes) and reads the boot state:
 * in TESTING with this image as the candidate, the confirmation is due. If
 * the read fails, fwupd_tick() tries again (and restarts in the end). */
void fwupd_init(const uint8_t *image, uint32_t size, uint32_t fw_version, uint32_t now_ms);

enum fwupd_rc {
    FWUPD_CONTINUE,   /* block stored: 2.31 Continue */
    FWUPD_DONE,       /* last block: image verified, previous image saved, PENDING written:
                       * 2.04 Changed; fwupd_tick() asks for the restart */
    FWUPD_BAD,        /* malformed: header, image head, block length: 4.00 */
    FWUPD_REFUSED,    /* not now or not this image (see fwupd_why()): 4.03 */
    FWUPD_INCOMPLETE, /* not the block expected next: 4.08 */
    FWUPD_TOO_LARGE,  /* image above FWIMG_APP_MAX: 4.13 */
    FWUPD_FAILED,     /* a flash operation or a read-back check failed: 5.00 */
};

/* One Block1 block of POST b/fw: num, M (more) and its payload. session_ok:
 * the server session is ready. Block 0 starts a transfer (after the checks:
 * header, session, network record, boot state, not the running image) and
 * first saves the running image into the previous slot unless it is
 * already there, whole; the block after the one stored last is stored; the
 * one stored last again is answered as before without a write (its answer
 * was lost). After FWUPD_FAILED, or FWUPD_BAD at block 0 or 1, the transfer
 * is dropped and has to start again at block 0. */
enum fwupd_rc fwupd_block(
    uint32_t num, bool more, const uint8_t *data, size_t len, bool session_ok, uint32_t now_ms);

/* Why the last fwupd_block() answered as it did, or the last tick event:
 * a short sentence for the log and the answer's diagnostic payload. */
const char *fwupd_why(void);

/* Drop a transfer (the session ended). Nothing is written. */
void fwupd_abort(void);

enum fwupd_event {
    FWUPD_EV_NONE,
    FWUPD_EV_CONFIRMED,      /* CONFIRMED written for the running image */
    FWUPD_EV_CONFIRM_FAILED, /* the write failed; tried again after FWUPD_RETRY_MS */
    FWUPD_EV_DROPPED,        /* a transfer saw no block for FWUPD_IDLE_MS */
    FWUPD_EV_RESTART,        /* restart now: an update was written (PENDING), or the
                              * candidate under test was not confirmed in FWUPD_TEST_MAX_MS,
                              * or the boot state could not be read in that time */
    FWUPD_EV_BOOT_READ,      /* the boot state, unread at fwupd_init(), was read now */
};

/* About once a second. healthy: the server session is ready and the radio
 * up. In TESTING with the running image as the candidate, healthy time adds
 * up (any unhealthy tick starts it again); at FWUPD_CONFIRM_MS the boot
 * state becomes CONFIRMED. If fwupd_init() could not read the boot state
 * (its operation failed or was not taken: src/handoff.h), it is read
 * again every FWUPD_RETRY_MS; still unread at FWUPD_TEST_MAX_MS after the
 * boot, FWUPD_EV_RESTART (the image may be a candidate under test). */
enum fwupd_event fwupd_tick(uint32_t now_ms, bool healthy);

/* For GET b/fw and the log (any task, through fwupd_port_lock()). */
struct fwupd_info {
    uint8_t           run_id[FWIMG_ID_LEN];
    struct fwimg_boot boot;       /* as last read or written; state NONE if none */
    bool              boot_read;  /* the boot state was read since the boot */
    bool              testing;    /* waiting to confirm the running image */
    bool              active;     /* a transfer runs */
    bool              done;       /* an update was written; restart pending */
    uint32_t          next;       /* the block expected next */
    uint32_t          blocks;     /* the transfer's last block number */
    uint32_t          healthy_ms; /* towards FWUPD_CONFIRM_MS */
};
void fwupd_info(struct fwupd_info *out);

/* The status as one line of text (GET b/fw): "run <id> boot <state> tries
 * <n> cand <id> prev <id> xfer <idle|n/N|done> healthy <s>", state
 * "unread" while the boot state has not been read. Returns its length (at
 * most cap - 1). */
size_t      fwupd_status_text(char *buf, size_t cap);
const char *fwupd_state_name(uint8_t state);

#endif
