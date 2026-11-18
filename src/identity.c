/* Bridge identity (#21). See identity.h. */
#include "identity.h"

#include "log.h"

#include <string.h>

static struct store_id id;
/* Written by the radio task after `id` (release), read by any task
 * (acquire), so a reader that sees IDENTITY_OK sees all of `id`. */
static enum identity_state state = IDENTITY_PENDING;

void identity_boot(bool flash_ok) {
    enum identity_state s   = IDENTITY_NONE;
    uint32_t            seq = 0;
    if (!flash_ok) {
        LOG("identity: flash not usable: no identity: no radio link, no server session");
    } else if (store_id_load(&id, &seq)) {
        s = IDENTITY_OK;
        /* The serial, as the session logs it; the EUI-64 is not logged (no
         * device address in the log, as in src/radio.c). */
        LOG("identity: record seq %lu: serial %s, EUI-64 set", (unsigned long)seq, id.serial);
    } else {
        memset(&id, 0, sizeof id);
        LOG("identity: no valid identity record at 0x100000/0x101000 (provision it with "
            "tools/identity/provision.sh): no radio link, no server session; power LED code 4");
    }
    __atomic_store_n(&state, s, __ATOMIC_RELEASE);
}

enum identity_state identity_state(void) {
    return __atomic_load_n(&state, __ATOMIC_ACQUIRE);
}

const struct store_id *identity(void) {
    return identity_state() == IDENTITY_OK ? &id : NULL;
}
