#ifndef MX25R_OPS_H
#define MX25R_OPS_H

/* MX25R1635F command opcodes and status bits, shared by the application's
 * driver (src/mx25r.c) and the bootloader's (src/boot/bl_hw.c). Defines
 * only. Datasheet: docs/datasheets/mx25r1635f.txt (Macronix PM2161 Rev.
 * 1.6). Which commands each driver may send, and where, is written in
 * each driver; no status/configuration or security register write, no
 * block or chip erase is defined here. */

#define MX25R_OP_READ   0x03u /* 3-byte address MSB first, then data, :1656-1685 */
#define MX25R_OP_PP     0x02u /* Page Program, :2306-2343 */
#define MX25R_OP_SE     0x20u /* Sector Erase, :2134-2171 */
#define MX25R_OP_WREN   0x06u /* :877-897 */
#define MX25R_OP_WRDI   0x04u /* :910-947 */
#define MX25R_OP_RDSR   0x05u /* :1380-1394, 2647 */
#define MX25R_OP_RDSCUR 0x2Bu /* :2488-2560, 2649 */
#define MX25R_OP_RDID   0x9Fu /* :960-966, 1123-1125 */

#define MX25R_SR_WIP     0x01u /* status register, :1347-1350, 1383 */
#define MX25R_SR_WEL     0x02u /* :1352-1357 */
#define MX25R_SCUR_PFAIL 0x20u /* security register bit 5, :2521-2523, Table 8 :2531-2540 */
#define MX25R_SCUR_EFAIL 0x40u /* bit 6, :2525-2527, Table 8 */

#endif
