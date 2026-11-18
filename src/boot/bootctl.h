#ifndef BOOTCTL_H
#define BOOTCTL_H

/* The bootloader's decisions (#24): install, test, roll back, start.
 * Hardware independent, host-tested with power cuts at every step
 * (tests/host/test_boot.c). docs/design.md "Implementation: bootloader and
 * boot state (#24)" has the state diagram and the power-cut walk-through.
 *
 * Writes: the boot state record (src/fwimg.h) in external flash, and
 * internal flash sectors 4-7 (the application). Nothing else: the update
 * slots are only read here, the bootloader's own sectors 0-3 are never
 * erased or programmed.
 *
 * Power-cut rules [I]:
 *   - The boot state only advances after the step it names is done and
 *     checked; every step can be repeated from the start, so a cut at any
 *     point is followed by the same step again.
 *   - An install erases sector 4 first, which removes the descriptor magic
 *     (the commit mark, src/fwimg.h), programs everything else, checks the
 *     SHA-256 of what is in internal flash, and programs the magic last. An
 *     install cut short therefore never passes fwimg_app_check(), whatever
 *     the boot state says.
 *   - Every loop is bounded: installs and test boots are counted in the
 *     record before they happen (MAX_TRIES each), and a boot state that
 *     cannot be written makes the bootloader stop managing updates for that
 *     boot (it starts a valid application, or installs one from a slot). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOTCTL_MAX_TRIES 3u /* test boots of a candidate; installs of one image */

enum bootctl_result {
    BOOTCTL_JUMP, /* internal flash holds a valid application: start it */
    BOOTCTL_HALT, /* nothing to start: stay in the bootloader, LED code, SWD recovery */
};

/* What a run did, for the tests and the bootloader's LED. */
struct bootctl_report {
    unsigned installs;   /* installs begun */
    unsigned installed;  /* installs finished and checked */
    unsigned writes;     /* boot state writes */
    unsigned rescue;     /* installs from a slot with no update in progress */
    uint8_t  state_seen; /* boot state at the start of the run */
};

/* One boot. ext_ok: the external flash answered at boot (JEDEC ID, idle);
 * without it no boot state or slot is used and only a valid application in
 * internal flash is started. */
enum bootctl_result bootctl_run(bool ext_ok, struct bootctl_report *rep);

/* --- Port (src/boot/bl_hw.c; a model in tests/host/test_boot.c) --------- */
/* Erase internal flash sector 4..7; false on an error flag. */
bool bl_port_int_erase(unsigned sector);
/* Program n bytes (n and addr multiples of 4) inside the application region,
 * word by word; false on an error flag. The caller reads back. */
bool bl_port_int_program(uint32_t addr, const uint8_t *src, size_t n);
/* Read internal flash. */
void bl_port_int_read(uint32_t addr, uint8_t *dst, size_t n);
/* Reload the IWDG. */
void bl_port_kick(void);

#endif
