/* Little-endian loads and stores at any alignment, for the formats in
 * flash: the slot header and the image descriptor (src/fwimg.c), the
 * records and the counter log (src/store.c). The bootloader uses them
 * through those two files and src/boot/bootctl.c. */
#ifndef LE_H
#define LE_H

#include <stdint.h>

static inline uint16_t le_get16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static inline uint32_t le_get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline void le_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void le_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

#endif
