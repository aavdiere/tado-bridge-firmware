/* MX25R1635F, read-only (#10). Datasheet: docs/datasheets/mx25r1635f.txt
 * (Macronix PM2161 Rev. 1.6).
 *
 * Only two opcodes can be sent, both reads: RDID 0x9F (manufacturer, memory
 * type, density, mx25r1635f.txt:960-966, 1123-1125) and RDSR 0x05. Every
 * command that changes the array or the status/configuration registers needs
 * WREN (0x06) first (mx25r1635f.txt:1352-1357, 1471-1473), and this file has
 * no way to send it. The chip may be in
 * deep power-down if the stock firmware left it there (the state survives an
 * MCU reset): a CS# low pulse of at least tCRDP = 20 ns releases it, and it is
 * in stand-by tRDP = 35 us (ultra low power) or 45 us (high performance)
 * after CS# goes high (mx25r1635f.txt:2426-2433, 3307-3314, 3402-3409). The
 * RES opcode is not needed for that on this part. SCK limit for these
 * commands: 33 MHz (mx25r1635f.txt:3268-3271), far above the bus's 6 MHz. */
#include "mx25r.h"

#include "spi1.h"
#include "timebase.h"

#define OP_RDID 0x9F
#define OP_RDSR 0x05

#define T_RDP_US 50u /* >= 45 us worst case, mx25r1635f.txt:3409 */

bool mx25r_wake(void) {
    spi1_lock();
    spi1_select(SPI1_DEV_FLASH);
    delay_us(1); /* tCRDP >= 20 ns */
    spi1_deselect(SPI1_DEV_FLASH);
    spi1_unlock();
    delay_us(T_RDP_US);
    uint32_t id;
    return mx25r_read_id(&id) && id == MX25R_JEDEC_ID;
}

/* The only command path. op is one of the two read opcodes above. */
static bool read_cmd(uint8_t op, uint8_t *dst, unsigned n) {
    if (op != OP_RDID && op != OP_RDSR) {
        return false;
    }
    spi1_lock();
    spi1_select(SPI1_DEV_FLASH);
    (void)spi1_xfer(op);
    for (unsigned i = 0; i < n; i++) {
        dst[i] = spi1_xfer(0x00);
    }
    spi1_deselect(SPI1_DEV_FLASH);
    spi1_unlock();
    return true;
}

bool mx25r_read_id(uint32_t *id) {
    uint8_t b[3];
    if (!read_cmd(OP_RDID, b, 3)) {
        return false;
    }
    *id = ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
    return true;
}

bool mx25r_read_status(uint8_t *sr) {
    return read_cmd(OP_RDSR, sr, 1);
}
