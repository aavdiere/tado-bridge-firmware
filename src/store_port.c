/* The store's flash port (#37): src/store.h on the MX25R1635F driver. The
 * address-range guard sits in src/mx25r.c, below this file. */
#include "store.h"

#include "mx25r.h"

_Static_assert(STORE_XFER_MAX == MX25R_XFER_MAX, "store chunks are driver transactions");

bool store_port_read(uint32_t addr, uint8_t *dst, size_t n) {
    return mx25r_read(addr, dst, n);
}

bool store_port_program(uint32_t addr, const uint8_t *src, size_t n) {
    return mx25r_program(addr, src, n);
}

bool store_port_erase(uint32_t addr) {
    return mx25r_erase_sector(addr);
}
