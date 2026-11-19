#ifndef HANDOFF_H
#define HANDOFF_H

/* Hand-over of one update flash operation (#24, src/fwupd.h) from the
 * update task (the poster) to the radio task (the runner), with a time
 * limit. Hardware independent; host-tested in tests/host/test_handoff.c.
 *
 * One slot, one operation at a time, three states:
 *   empty -> posted       handoff_run(), poster
 *   posted -> claimed     handoff_claim(), runner: it runs the operation
 *   posted -> empty       the poster withdraws it after wait_ms: it never runs
 *   claimed -> empty      handoff_finish(), runner, then op->done
 * The two changes out of "posted" are compare-and-swap on the slot, so
 * exactly one of them wins: an operation is either run once, or withdrawn
 * and never run. A claimed operation is always waited for to the end
 * (it lives on the poster's stack, and it is bounded: one sector erase or
 * one boot-state write, docs/design.md "Bounds of one operation"), so the
 * runner never touches an operation the poster has given up.
 *
 * Why the limit (#24 review finding M1): if the radio task never reaches
 * its loop (radio_configure() failing, src/radio.c), no operation is ever
 * claimed. Without the limit the update task waited for ever in
 * fwupd_init(), never confirmed and never restarted. */

#include <stdbool.h>
#include <stdint.h>

struct fwupd_op;

/* Poster. Post op, wake the runner, wait until it is done or, if it is
 * still unclaimed after wait_ms, withdraw it. Returns op->ok if it ran,
 * false if it was withdrawn (op->ok false, op->done false) or the slot was
 * not empty (a second poster: never on the bridge). */
bool handoff_run(struct fwupd_op *op, uint32_t wait_ms);

/* Runner. An operation is posted and not yet claimed, or claimed and not
 * yet finished (the coordinator is held meanwhile, src/radio.c). */
bool handoff_pending(void);
/* Runner. Take the posted operation for running; NULL if there is none
 * (or the poster withdrew it a moment ago). */
struct fwupd_op *handoff_claim(void);
/* Runner. The claimed operation is over (op->ok set): empty the slot, then
 * op->done, last. Then wake the poster (port). */
void handoff_finish(struct fwupd_op *op);

/* --- Port (src/radio.c; a model in the host test) -------------------------- */
uint32_t handoff_port_ms(void);           /* a millisecond clock */
void     handoff_port_wake(void);         /* tell the runner an operation is posted */
void     handoff_port_sleep(uint32_t ms); /* poster: sleep up to ms, or until woken */

#endif
