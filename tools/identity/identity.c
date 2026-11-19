/* Identity record provisioning tool (#21), host side. Builds the identity
 * record (src/store.h) with the firmware's own src/store.c, against an
 * image of the two identity sectors read from the bridge, so the record is
 * written by the same code and rules as the network record: CRC-32, seq,
 * A/B slots, the older or invalid slot erased and written, magic last.
 *
 *   identity from-backup <internal.bin>
 *       Prints the serial and EUI-64 of the stock configuration in a backup
 *       of the internal flash (tanoclo/patch_ib_firmware/extract_rf_key.js).
 *   identity make <id> <slot_a.bin> <slot_b.bin> <out.bin>
 *       <slot_a.bin>, <slot_b.bin>: the sectors at 0x100000 and 0x101000 as
 *       read from the bridge (4096 bytes each). Prints "sector 0x100000" or
 *       "sector 0x101000" and writes that sector's new contents (4096 bytes)
 *       to <out.bin>; prints "sector none" if the newest record already
 *       holds this identity. Refuses to replace a different valid identity
 *       unless --replace comes first. Adding a MAC to a version 1 record of
 *       the same serial and EUI-64 is no replacement; changing or removing
 *       a MAC is.
 *   identity check <id> <slot_a.bin> <slot_b.bin>
 *       Loads the record as the firmware does (store_id_load()) and exits 0
 *       only if it is valid and holds <id> (the MAC included).
 *
 *   <id> is either --backup <internal.bin> or --serial <S> --eui64 <hex>
 *   (8 bytes, stored order, 00 1b c5 first; separators ' ', ':' or '-'),
 *   then optionally the Ethernet MAC (#39) as --mac-file <file> (one line,
 *   6 bytes in hex, as in ~/ib01-backups/, so the MAC stays off the command
 *   line) or --mac <hex>. With a MAC the record is version 2, without one
 *   version 1 (the firmware then uses its placeholder MAC).
 *
 * Device values (serial, EUI-64, MAC) only pass through files and the
 * terminal: keep every file outside git (CLAUDE.md). The MAC is printed
 * only with --show. The network key next to them in the backup is never
 * read. */
#include "store.h"

#include "flash_layout.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- The two identity sectors as a NOR flash: programs only clear bits ------- */
#define WIN_BASE FLASH_ID_A
#define WIN_SIZE (2u * FLASH_SECTOR)

static uint8_t  win[WIN_SIZE];
static int      erased = -1; /* sector index erased by the write, -1 none */
static unsigned erases;

static bool in_win(uint32_t addr, size_t n) {
    return addr >= WIN_BASE && n <= WIN_SIZE && addr - WIN_BASE <= WIN_SIZE - n;
}

bool store_port_read(uint32_t addr, uint8_t *dst, size_t n) {
    if (!in_win(addr, n)) {
        return false; /* this tool knows the identity sectors only */
    }
    memcpy(dst, win + (addr - WIN_BASE), n);
    return true;
}

bool store_port_program(uint32_t addr, const uint8_t *src, size_t n) {
    /* The same rules as the chip and src/mx25r.c: within one page, at most
     * STORE_XFER_MAX bytes; and only inside the identity sectors. */
    if (!in_win(addr, n) || n == 0 || n > STORE_XFER_MAX || addr % FLASH_PAGE + n > FLASH_PAGE) {
        fprintf(stderr, "identity: program outside the identity sectors refused\n");
        exit(3);
    }
    for (size_t i = 0; i < n; i++) {
        win[addr - WIN_BASE + i] &= src[i];
    }
    return true;
}

bool store_port_erase(uint32_t addr) {
    if (!in_win(addr, FLASH_SECTOR) || addr % FLASH_SECTOR) {
        fprintf(stderr, "identity: erase outside the identity sectors refused\n");
        exit(3);
    }
    memset(win + (addr - WIN_BASE), 0xFF, FLASH_SECTOR);
    erased = (int)((addr - WIN_BASE) / FLASH_SECTOR);
    erases++;
    return true;
}

/* --- Files --------------------------------------------------------------------- */
static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "identity: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    size_t   cap = 1u << 20, n = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        size_t k = fread(buf + n, 1, cap - n, f);
        n += k;
        if (n < cap) {
            break;
        }
        uint8_t *nb = cap <= (64u << 20) ? realloc(buf, cap * 2u) : NULL;
        if (!nb) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = nb;
        cap *= 2u;
    }
    bool err = ferror(f) != 0;
    fclose(f);
    if (!buf || err) {
        fprintf(stderr, "identity: %s: read failed\n", path);
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

static bool load_sector(const char *path, unsigned index) {
    size_t   n;
    uint8_t *b = read_file(path, &n);
    if (!b) {
        return false;
    }
    bool ok = n == FLASH_SECTOR;
    if (ok) {
        memcpy(win + index * FLASH_SECTOR, b, FLASH_SECTOR);
    } else {
        fprintf(stderr, "identity: %s is %zu bytes, want %u (one sector)\n", path, n, FLASH_SECTOR);
    }
    free(b);
    return ok;
}

static bool write_file(const char *path, const uint8_t *p, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "identity: %s: %s\n", path, strerror(errno));
        return false;
    }
    bool ok = fwrite(p, 1, n, f) == n;
    ok      = fclose(f) == 0 && ok;
    if (!ok) {
        fprintf(stderr, "identity: %s: write failed\n", path);
    }
    return ok;
}

/* --- Identity from the backup ---------------------------------------------------- */
/* The stock configuration page in internal flash: marker 42 49 00 00 54 46
 * ("BI..TF") at page start + 6 (0x4006 in the bench unit's dump,
 * docs/hardware.md "Identity"); serial u32 little endian at marker + 8,
 * EUI-64 at page start + 50, 8 bytes in stored order
 * (tanoclo/patch_ib_firmware/extract_rf_key.js:51-71). The serial string is
 * "IB" and the u32 in decimal, zero padded to 10 digits (docs/build.md P4,
 * [I]; tanoclo accepted it on the bench [V]). The key at marker + 12 is
 * skipped. */
static const uint8_t marker[6] = {0x42, 0x49, 0x00, 0x00, 0x54, 0x46};
#define MARK_OFF   6u
#define SERIAL_OFF (MARK_OFF + 8u)
#define EUI_OFF    50u
#define PAGE_NEED  (EUI_OFF + 8u)

static bool from_backup(const char *path, struct store_id *out) {
    size_t   n;
    uint8_t *b = read_file(path, &n);
    if (!b) {
        return false;
    }
    unsigned found = 0;
    bool     agree = true;
    for (size_t off = MARK_OFF; off + sizeof marker <= n; off++) {
        if (memcmp(b + off, marker, sizeof marker) != 0) {
            continue;
        }
        size_t page = off - MARK_OFF;
        if (page + PAGE_NEED > n) {
            continue;
        }
        const uint8_t *s = b + page + SERIAL_OFF;
        unsigned long v = (unsigned long)s[0] | (unsigned long)s[1] << 8 | (unsigned long)s[2] << 16
                          | (unsigned long)s[3] << 24;
        struct store_id id = {0};
        snprintf(id.serial, sizeof id.serial, "IB%010lu", v);
        memcpy(id.eui64, b + page + EUI_OFF, 8);
        if (found == 0) {
            *out = id;
        } else if (strcmp(out->serial, id.serial) != 0 || memcmp(out->eui64, id.eui64, 8) != 0) {
            agree = false;
        }
        found++;
    }
    free(b);
    if (found == 0) {
        fprintf(stderr, "identity: %s: no stock configuration marker (42 49 00 00 54 46)\n", path);
        return false;
    }
    if (!agree) {
        fprintf(stderr, "identity: %s: %u configuration markers that disagree\n", path, found);
        return false;
    }
    if (!store_id_valid(out)) {
        fprintf(stderr,
                "identity: %s: configuration found, but the EUI-64 does not start 00 1b c5\n",
                path);
        return false;
    }
    return true;
}

static int hexval(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}

/* n bytes in hex, separators ' ', ':' or '-' between them. */
static bool parse_hex(const char *s, uint8_t *out, unsigned n) {
    unsigned k = 0;
    while (*s) {
        if (*s == ' ' || *s == ':' || *s == '-') {
            s++;
            continue;
        }
        int hi = hexval(s[0]), lo = s[1] ? hexval(s[1]) : -1;
        if (hi < 0 || lo < 0 || k == n) {
            return false;
        }
        out[k++] = (uint8_t)(hi << 4 | lo);
        s += 2;
    }
    return k == n;
}

/* The MAC file: one line, 6 bytes in hex; white space around it is
 * ignored, anything else is refused. The contents are never echoed. */
static bool mac_from_file(const char *path, uint8_t mac[6]) {
    size_t   n;
    uint8_t *b = read_file(path, &n);
    if (!b) {
        return false;
    }
    char   line[64];
    size_t s = 0, e = n;
    while (s < e && (b[s] == ' ' || b[s] == '\t' || b[s] == '\n' || b[s] == '\r')) {
        s++;
    }
    while (e > s && (b[e - 1] == ' ' || b[e - 1] == '\t' || b[e - 1] == '\n' || b[e - 1] == '\r')) {
        e--;
    }
    bool ok = e - s < sizeof line;
    if (ok) {
        memcpy(line, b + s, e - s);
        line[e - s] = '\0';
        ok          = parse_hex(line, mac, 6);
    }
    memset(line, 0, sizeof line);
    memset(b, 0, n);
    free(b);
    if (!ok) {
        fprintf(stderr, "identity: %s: want one line with the MAC, 6 bytes in hex\n", path);
    }
    return ok;
}

static bool show_secrets; /* --show: print serial, EUI-64 and MAC in full */

/* By default only a masked form (serial prefix and length, last EUI-64 byte,
 * whether there is a MAC) reaches the terminal and logs; the full values
 * need --show. */
static void print_id(const char *what, const struct store_id *id) {
    if (show_secrets) {
        printf("%s serial %s, EUI-64 %02x %02x %02x %02x %02x %02x %02x %02x",
               what,
               id->serial,
               id->eui64[0],
               id->eui64[1],
               id->eui64[2],
               id->eui64[3],
               id->eui64[4],
               id->eui64[5],
               id->eui64[6],
               id->eui64[7]);
        if (id->has_mac) {
            printf(", MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                   id->mac[0],
                   id->mac[1],
                   id->mac[2],
                   id->mac[3],
                   id->mac[4],
                   id->mac[5]);
        } else {
            printf(", no MAC\n");
        }
        return;
    }
    printf("%s serial %.2s.. (%u characters), EUI-64 ..:..:..:..:..:..:..:%02x, %s\n",
           what,
           id->serial,
           (unsigned)strlen(id->serial),
           id->eui64[7],
           id->has_mac ? "MAC set" : "no MAC");
}

/* <id>: --backup FILE | --serial S --eui64 HEX, then optionally
 * --mac-file FILE | --mac HEX. Returns the next argv index. */
static int parse_id(int argc, char **argv, int i, struct store_id *id) {
    if (i + 1 < argc && strcmp(argv[i], "--backup") == 0) {
        if (!from_backup(argv[i + 1], id)) {
            return -1;
        }
        i += 2;
    } else if (i + 3 < argc && strcmp(argv[i], "--serial") == 0
               && strcmp(argv[i + 2], "--eui64") == 0) {
        memset(id, 0, sizeof *id);
        if (strlen(argv[i + 1]) > STORE_ID_SERIAL_MAX) {
            fprintf(stderr, "identity: serial longer than %u characters\n", STORE_ID_SERIAL_MAX);
            return -1;
        }
        strcpy(id->serial, argv[i + 1]);
        if (!parse_hex(argv[i + 3], id->eui64, 8)) {
            fprintf(stderr, "identity: EUI-64 must be 8 bytes in hex\n");
            return -1;
        }
        if (!store_id_valid(id)) {
            fprintf(stderr,
                    "identity: refused: serial must be two capital letters then capital letters "
                    "or digits (IB0123456789), EUI-64 must start 00 1b c5\n");
            return -1;
        }
        i += 4;
    } else {
        fprintf(stderr,
                "identity: want --backup FILE or --serial S --eui64 HEX, then optionally "
                "--mac-file FILE or --mac HEX\n");
        return -1;
    }
    if (i + 1 < argc && (strcmp(argv[i], "--mac-file") == 0 || strcmp(argv[i], "--mac") == 0)) {
        bool ok = strcmp(argv[i], "--mac") == 0 ? parse_hex(argv[i + 1], id->mac, 6)
                                                : mac_from_file(argv[i + 1], id->mac);
        if (!ok) {
            if (strcmp(argv[i], "--mac") == 0) {
                fprintf(stderr, "identity: MAC must be 6 bytes in hex\n");
            }
            return -1;
        }
        id->has_mac = true;
        if (!store_id_valid(id)) {
            fprintf(stderr,
                    "identity: refused: the MAC must be unicast (first byte even) "
                    "and not all zero\n");
            return -1;
        }
        i += 2;
    }
    return i;
}

static bool same_id(const struct store_id *a, const struct store_id *b) {
    return strcmp(a->serial, b->serial) == 0 && memcmp(a->eui64, b->eui64, 8) == 0
           && a->has_mac == b->has_mac && memcmp(a->mac, b->mac, 6) == 0;
}

/* Is want only have plus a MAC (a version 1 record upgraded to version 2,
 * #39)? That is no replacement. */
static bool adds_mac(const struct store_id *have, const struct store_id *want) {
    return !have->has_mac && want->has_mac && strcmp(have->serial, want->serial) == 0
           && memcmp(have->eui64, want->eui64, 8) == 0;
}

static int usage(void) {
    fprintf(stderr,
            "usage: identity [--show] from-backup <internal.bin>\n"
            "       identity [--show] make [--replace] <id> <slot_a.bin> <slot_b.bin> <out.bin>\n"
            "       identity [--show] check <id> <slot_a.bin> <slot_b.bin>\n"
            "  --show: print serial, EUI-64 and MAC in full (default: masked)\n"
            "  <id>: (--backup <internal.bin> | --serial <S> --eui64 <hex>)\n"
            "        [--mac-file <file> | --mac <hex>]\n");
    return 2;
}

int main(int argc, char **argv) {
    /* --show anywhere on the command line: full values in the output. */
    for (int k = 1; k < argc;) {
        if (strcmp(argv[k], "--show") == 0) {
            show_secrets = true;
            memmove(&argv[k], &argv[k + 1], (size_t)(argc - k) * sizeof *argv);
            argc--;
        } else {
            k++;
        }
    }
    if (argc < 2) {
        return usage();
    }
    struct store_id want, have;
    uint32_t        seq = 0;

    if (strcmp(argv[1], "from-backup") == 0 && argc == 3) {
        if (!from_backup(argv[2], &want)) {
            return 1;
        }
        print_id("backup:", &want);
        return 0;
    }

    if (strcmp(argv[1], "make") == 0) {
        int  i       = 2;
        bool replace = i < argc && strcmp(argv[i], "--replace") == 0;
        i += replace ? 1 : 0;
        if ((i = parse_id(argc, argv, i, &want)) < 0) {
            return 1;
        }
        if (argc != i + 3) {
            return usage();
        }
        if (!load_sector(argv[i], 0) || !load_sector(argv[i + 1], 1)) {
            return 1;
        }
        uint16_t version = 0;
        if (store_id_load(&have, &seq, &version)) {
            printf(
                "bridge now: record seq %lu, version %u:", (unsigned long)seq, (unsigned)version);
            print_id("", &have);
            if (same_id(&have, &want)) {
                printf("sector none\n");
                return 0;
            }
            if (adds_mac(&have, &want)) {
                printf("adding the MAC to the same serial and EUI-64 (version 1 to 2)\n");
            } else if (!replace) {
                fprintf(stderr,
                        "identity: the bridge already holds a different valid identity; "
                        "refused without --replace\n");
                return 1;
            }
        }
        if (!store_id_write(&want, &seq) || erases != 1 || erased < 0) {
            fprintf(stderr, "identity: record not built\n");
            return 1;
        }
        /* Read back as the firmware will. */
        if (!store_id_load(&have, &seq, &version) || !same_id(&have, &want)) {
            fprintf(stderr, "identity: record built but does not load back\n");
            return 1;
        }
        if (!write_file(argv[i + 2], win + (unsigned)erased * FLASH_SECTOR, FLASH_SECTOR)) {
            return 1;
        }
        print_id("new record:", &want);
        printf("record seq %lu, version %u\n", (unsigned long)seq, (unsigned)version);
        /* What a firmware from before #39 (version 1 only, for example the
         * previous image after a rollback, #24) would load from these
         * sectors. */
        uint8_t  p1[STORE_ID_LEN_V1];
        uint32_t s1 = 0;
        if (!store_slot_load(FLASH_ID_A, STORE_ID_MAGIC, STORE_ID_V1, p1, sizeof p1, &s1)) {
            printf("note: no version 1 record left: a firmware from before #39 finds no "
                   "identity here\n");
        } else if (s1 != seq) {
            printf("note: a firmware from before #39 finds the version 1 record seq %lu\n",
                   (unsigned long)s1);
        }
        printf("sector 0x%06x\n", (unsigned)(WIN_BASE + (unsigned)erased * FLASH_SECTOR));
        return 0;
    }

    if (strcmp(argv[1], "check") == 0) {
        int i = parse_id(argc, argv, 2, &want);
        if (i < 0) {
            return 1;
        }
        if (argc != i + 2) {
            return usage();
        }
        if (!load_sector(argv[i], 0) || !load_sector(argv[i + 1], 1)) {
            return 1;
        }
        uint16_t version = 0;
        if (!store_id_load(&have, &seq, &version)) {
            fprintf(stderr, "identity: no valid identity record\n");
            return 1;
        }
        printf("record seq %lu, version %u\n", (unsigned long)seq, (unsigned)version);
        print_id("bridge:", &have);
        if (!same_id(&have, &want)) {
            fprintf(stderr, "identity: the record holds a different identity\n");
            return 1;
        }
        printf("identity record OK\n");
        return 0;
    }
    return usage();
}
