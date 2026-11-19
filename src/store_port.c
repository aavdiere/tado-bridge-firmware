/* The store's flash port (#37): src/store.h on the MX25R1635F driver. The
 * address-range guard sits in src/mx25r.c, below this file. Also the
 * update's choice of the driver's writable area (#24, src/fwupd.h). */
#include "store.h"

#include "fwupd.h"
#include "mx25r.h"

_Static_assert(STORE_XFER_MAX == MX25R_XFER_MAX, "store chunks are driver transactions");
_Static_assert((int)FWUPD_AREA_DEFAULT == (int)MX25R_AREA_RECORDS
                   && (int)FWUPD_AREA_BOOT == (int)MX25R_AREA_BOOT
                   && (int)FWUPD_AREA_SLOTS == (int)MX25R_AREA_SLOTS,
               "the update's areas are the driver's");

bool store_port_read(uint32_t addr, uint8_t *dst, size_t n) {
    return mx25r_read(addr, dst, n);
}

bool store_port_program(uint32_t addr, const uint8_t *src, size_t n) {
    return mx25r_program(addr, src, n);
}

bool store_port_erase(uint32_t addr) {
    return mx25r_erase_sector(addr);
}

void fwupd_port_area(enum fwupd_area a) {
    (void)mx25r_area((enum mx25r_area)a);
}
