# IB01 firmware design

**Status:** storage (#27) and software stack (#18) approved by the user (2026-11-11); self-update (#24, first #20) approved on condition that the firmware fits the layout (size check before building). Built and tested so far: the entropy pool (#14, 2026-11-14) and TLS, WebSocket and auth to tanoclo (#15, 2026-11-14); the rest is not built yet. Other sections (entropy, network stack, self-update) will be added as they are decided.

Datasheets are in `datasheets/`. Status tags: **[D]** from a datasheet or repo, **[I]** our own inference.

## Persistent storage (#27)

### What has to persist

| Data | Changes | If lost |
| :--- | :--- | :--- |
| Serial, EUI-64 | Never; set at flashing | tanoclo no longer recognises the bridge |
| Network key, RF channel | At network creation | Every device must be re-paired |
| Frame counter reservation | About once per 2048 frames | Minor; devices accept lower counters (`freedo/docs/PROTOCOL.md:1290`) |

Not stored: wake-up anchors, which are relearned after reboot at the cost of a few full 1 s trains (`radio-runtime.md`). Schedules and the device list are on the server.

### Why external flash

| | MX25R1635F (external) | F411 internal |
| :--- | :--- | :--- |
| Smallest erase | 4 KB (`mx25r1635f.txt:219`) | 16 KB, up to 128 KB (`stm32f411ce.txt:5311-5329`) |
| Endurance | 100,000 cycles (`mx25r1635f.txt:142`) | 10,000 cycles (`stm32f411ce.txt:5389`) |
| Page program | 256 bytes (`tanoclo/patch_ib_firmware/spi_stub.c:6`) | n/a |

Keep internal flash for code only.

### Layout (external flash)


A backup exists, so stock contents need not be preserved.

| Offset | Size | Use |
| :--- | :--- | :--- |
| `0x000000` | 512 KB | Update image: candidate (#24) |
| `0x080000` | 512 KB | Update image: previous version (#24) |
| `0x100000` | 4 KB × 2 | Identity, slots A and B |
| `0x102000` | 4 KB × 2 | Network, slots A and B |
| `0x104000` | 4 KB × 2 | Counter log, sectors A and B |
| `0x106000` | 4 KB × 2 | Boot state, slots A and B (#24) |
| `0x108000` | Rest | Unassigned |

### Slotted records (identity, network)

Stock also keeps active and backup configuration partitions (`tanoclo/patch_ib_firmware/extract_channel.js:6`).

Format: `magic u32 | version u16 | length u16 | seq u32 | payload | CRC-32`. The F411 has a CRC unit (`stm32f411ce.txt:43`).

- **Payloads:** identity holds the serial and EUI-64, and since version 2 (#39) the Ethernet MAC. Network holds the key and channel.
- **Write:** choose the slot that is older or invalid, erase it, program the record with `seq + 1`, read back and verify.
- **Boot:** take the valid slot with the highest `seq`.
- **Power cut:** a torn write fails its CRC, and the other slot is intact. No state is lost.

### Counter log

Follows freedo's reservation scheme (`freedo/docs/PROTOCOL.md:1296-1299`). One counter is shared by unicast and broadcast frames (`:1291-1292`).

- Each 8-byte entry holds `reserved_until u32` and its bitwise inverse. An entry is valid only if the two match, which also catches torn writes.
- Entries append, so 512 fit per sector.
- **Boot:** scan both sectors for the highest valid `reserved_until`, start the counter from it, and write a new entry `+4096` **before the first frame**.
- **Running:** write a new entry `+4096` once less than 2048 remains.
- **Sector full:** erase the other sector and continue there, so the latest entries always remain readable.

**Wear [I]:** at a high estimate of 1 frame per second, that is about 42 entries a day, and each sector is erased about every 24 days. 100,000 cycles is far more than the product's lifetime. Even a reboot loop every 10 s would take decades.

### Sharing SPI1 with the radio

Radio and flash share the bus (`hardware.md`).

- During an erase or program the flash is busy internally, but with CS high the bus is free **[I]**. The radio keeps working, and the WIP status bit is polled only when the radio is idle (`mx25r1635f.txt:223`).
- A counter entry is a transaction of a few tens of µs. Never start any flash transaction during a wake train, ACK window or ACK transmission.
- Network records are rare: write only when no pairing window is open.
- Erase Suspend exists (`mx25r1635f.txt:161`) but should not be needed **[I]**.
- **Built (#22, 2026-11-18):** see "Implementation: flash work during radio traffic (#22)" below. The WIP poll is not restricted to idle radio moments; instead the radio task keeps receiving and acknowledging between the polls.

### Lifecycle

- **At flashing:** a host tool reads serial and EUI-64 from the backup (`tanoclo/patch_ib_firmware/extract_rf_key.js`), the Ethernet MAC from the owner's file (#39), and writes the identity record.
- **First boot with no network record:** create the key (depends on #26), then write the record before starting the radio.
- **Identity missing or invalid:** the receiver stays on, but nothing can transmit, ACK, authenticate or pair; LED fault code 4 (owner decision, 2026-11-18; `docs/README.md`, "Project decisions").
- **Factory reset** (factory reset button, PB10): erase network and counter records, keeping identity. Devices must then be re-paired. Built 2026-11-18: "Implementation: factory reset (P6)" below.

### Read protection

- **Level 0 during development:** full SWD access.
- **Never level 2:** it disables debug access and removes recovery (`datasheets/rm0383.txt:2537-2538`).
- **Level 1 is optional, decide at the end.** It would not protect the key in external flash, and the key also crosses the air during pairing (`radio.md`).

### Implementation: network and counter records (#37)

Built 2026-11-17, host-tested, fw-reviewer "safe to flash" for 93f25d8..265cb70, flashed and verified 2026-11-17 (bin sha256 8401c00d..., boot: JEDEC c22815, "no valid network record", TX off, session ready) [V]. Code: `src/flash_layout.h`, `src/mx25r.c` (program and erase), `src/store.c` (records, hardware independent), `src/store_port.c`, `src/network.c` (boot and running), the counter gate in `src/csl.c`; tests in `tests/host/test_store.c`. Where it differs from the design above, the difference is marked **Differs**.

**Stock contents of the external flash [V]:** the bench unit's backup (`~/ib01-backups/2026-11-11/a/unmodded_spi.bin`, outside the repository; only a map of 4 KB sectors that are not all `0xFF` was taken, no contents) has data in `0x000000-0x03DFFF`, `0x080000-0x0BEFFF`, `0x100000-0x100FFF` and `0x102000-0x102FFF`. Everything else is blank, including `0x103000` (the stock backup partition, `tanoclo/patch_ib_firmware/extract_channel.js:6`) and the counter log at `0x104000-0x105FFF`. Stock keeps its active configuration partition at `0x102000` (`extract_channel.js:6,66`), with the channel 26 setting (`test-bench.md`, "The bench unit's own stock configuration"), and a descriptor table in `0x100000-0x103FFF` (`tanoclo/patch_ib_firmware/check_firmware.sh:83`).

**Decided (owner, 2026-11-17): keep slot A at `0x102000`; the stock configuration is not kept, the backup is enough.** The conflict was: network slot A (`0x102000`) is the stock active configuration. The layout accepts overwriting stock contents because a backup exists ("Layout (external flash)"), so the code follows it, but the first `network_create()` (pairing, #19) will erase that sector. Moving the network slots to the unassigned `0x108000-0x109FFF` would have avoided it; the owner chose not to. Until #19 nothing writes there: see "Writes" below.

**Address guard:** `src/mx25r.c` panics on any Page Program or Sector Erase outside `0x102000-0x105FFF` (network and counter-log sectors; since #24 the default of three writable areas, one at a time: "Implementation: self-update, application side (#24)", write guard), on an erase address that is not sector aligned, and on a program that would cross a 256-byte page (a PP past the page end wraps to the page start, `datasheets/mx25r1635f.txt:2310-2315`). No other write command exists in the driver: no WRSR, no block or chip erase, no security-register write.

**Program and erase:** the datasheet's flow with fail flags (`mx25r1635f.txt:1281-1330`): WREN, RDSR until WEL = 1, the command, RDSR until WIP = 0, WEL cleared, then RDSCUR P_FAIL (bit 5) or E_FAIL (bit 6) clear (`:2521-2540`). Timeouts are twice the worst case of either power mode: tPP 10 ms (`:3337`), tSE 240 ms (`:3340`). Every bus transaction moves at most 64 data bytes (about 90 µs at 6 MHz), because SPI1 transactions run in a critical section (`src/spi1.c`). The store reads back every program and checks every erased sector for `0xFF`.

**Boot wait:** after the deep power-down release, RDSR is polled until WIP = 0 (at most 480 ms, 2 x tSE) before the JEDEC ID or any array read. An MCU reset (watchdog, debugger) does not stop a program or erase the chip is running, and while WIP = 1 the chip ignores array reads and does not decode RDID **[D]** (`mx25r1635f.txt:556-557, 968-969`; RDSR works at any time, `:1142-1144`). If WIP stays set, or the JEDEC ID is not `C2 28 15`, the flash is not used: no network record is read, so the link keeps no key and nothing is sent (`flash_boot()` in `src/radio.c`; found by `fw-reviewer`, 2026-11-17).

**Slotted record, as built:** format as designed; little endian; CRC-32 is the IEEE reflected CRC in software, not the F411 CRC unit, so the code is host-testable. **Differs (stricter):** the magic is the commit mark: erase the older or invalid slot, check it blank, program everything after the magic, read back, then program the magic and read the whole record back. A slot whose magic does not match is read no further, so the stock configuration in slot A is read only for its first 4 bytes.

**Network record v1:** magic `0x3154454E` ("NET1"), version 1, payload 24 bytes: PAN ID u16, channel u8, reserved u8 (0), key 16 bytes, counter floor u32; 40 bytes in all with the header and CRC. **Differs:** the design listed key and channel only; the PAN (`0xABCD`, `src/mac.h`) and a counter floor were added. Only the test channel (16 since 2026-11-17, 10 before) and PAN `0xABCD` are written or loaded; a record for channel 10 is now refused like any other channel; a record for channel 26 is refused when written and ignored when read (`store_net_allowed()`). The floor is the counter log's highest entry when the network is created, so a new network does not reuse counters of an old one.

**Counter log, as built:** entries as designed; the highest valid entry is the reservation, the first counter not reserved. Entries are appended after the last slot that is not blank, and a slot is used once even if its write fails. The sector holding the highest valid entry is never erased **[I]**: if every slot of the next sector fails as well (512 failed writes with erases still working), appends fail until the next boot instead of erasing it, and the coordinator stops at its limit; the boot's scan starts again from the highest entry's sector, which is full, and erases the other one (found by `fw-reviewer`, 2026-11-17; before, the 513th failed write erased the sector with the only valid reservation, and a reboot would have restarted the counters at the record's floor). Boot takes the higher of the log and the record's floor, writes `+4096` and passes both values to `link_set_key(key, first, limit)`. Running: at a quiet moment in the radio task, once less than 2048 remain, `+4096` from the next counter. **Differs:** the coordinator itself stops at the limit (`csl_set_counter_limit()`, `src/csl.c`): no transaction starts at or above it, so a failed log write stops transmission instead of letting the counter outrun the flash; the write is retried every 10 s.

**Differs, SPI1 sharing:** the store blocks the radio task for the length of a write: a counter entry is two short transactions plus the page program cycle, tPP 3.2 ms typical and 10 ms max in ultra low power mode (`datasheets/mx25r1635f.txt:3337`; 0.85 ms and 4 ms in high performance mode, `:3432`) **[D]**. The datasheet gives no shorter time for an 8-byte program than for a page, so 10 ms is the bound used; the chip runs in ultra low power mode, its default (`:3746`), since the driver never writes the configuration register. A frame that arrives during a program gets no ACK, never a late one **[I]**: `ack_consider()` answers only a frame the RXBYTES read showed still arriving, with no GDO0 edge since (`src/radio.c:700-714`), so a frame that ended while the task was blocked is not answered, and the device sends it again. At 10 ms a frame that starts just after the program begins brings about 62 bytes at 50 kbps, close to the 64-byte RX FIFO (`datasheets/cc110l.txt:1483`), so a longer frame may overflow it and is dropped by the overflow path **[I]**. A sector erase, once per 512 entries (about every million frames), 40-240 ms, during which the radio FIFO is not served (other tasks run, `vTaskDelay` between polls). Both happen only when no frame is arriving and no transaction is on the air. The design's non-blocking WIP poll is left for #22. **Changed by #22 (2026-11-18):** the radio task now keeps receiving and acknowledging during a program or erase, and no flash transaction starts while a frame arrives ("Implementation: flash work during radio traffic (#22)" below); the paragraph above describes the code up to `3a5de0e`.

**Differs, lifecycle:** no network is created at boot. Without a valid record the link keeps no key, so nothing is decrypted, acknowledged or sent, and the counter log is neither read nor written. `network_create()` (fresh key from the CTR_DRBG with `rng_random()`, test channel only) is called by the pairing trigger (#19, `radio-runtime.md`, "Pairing (#19)"), before the window opens, and only if the boot read the flash and found no network record at all (`network_absent()`: neither slot holds the record's magic, `store_net_probe()`); after any failure it is refused until the next boot, and an existing network's key is read back from its record instead (`network_key()`). In a build without a pairing target (`PAIR_TARGET_EUI64` in the gitignored `config/local.cmake`; every build from a fresh clone) or a receive-only build (`RADIO_TX_ALLOWED=0`), the trigger stops before it and the linker drops `network_create()`, so such an image contains no code that writes a network record (checked 2026-11-17: the function's log string is absent from both default images, transmitter on and off). Since 2026-11-17 the committed build has the transmitter on by default (owner decision, `README.md`, "Project decisions"); until then it was off. It succeeds only if the network read back as a boot would read it (`store_net_commit()`) is the record just written, by seq and contents, and it hands the key read back to its caller, for pairing to send under the pairing key (`radio.md`, "Link security and pairing"). Once the write has started, a failure leaves no key set until the next boot. Host test: a magic bit that reads right during the write's own verify and then fades makes the commit fail, where before the older network's record would have been activated (found by `fw-reviewer`, 2026-11-17). The key is never logged; the boot log shows the record's seq, channel and counter values only.

**Power-cut tests [I] (host, `make test`):** a NOR model (programming only clears bits) cuts the power inside any program (a torn prefix plus one byte with part of its bits) or erase (bytes erased, with bits set, or with bits cleared as in the chip's pre-program phase). Covered: a cut at every step of a record write from three histories (none, one, two records), 8 random tears each (72 cases); a cut at every step of a counter-log sector change, to a blank and to a stale sector (64 cases); a 3000-extend random run with 158 cuts (1 in an erase); create-then-boot with a cut at every step; a silently weak program bit; one flipped bit at every record offset; noise and planted entries in both log sectors; with the power on, every program failing (fail flag with nothing or part of the bits done, or a silent miss the read-back catches) from 8 fill levels (24 cases), and every erase failing in the same three ways at a change to a blank and to a stale sector (6 cases), with the model checking that the sector with the highest entry is never erased. Checked after every reboot: a record is either the old or the new one; the counter never starts below one already used on the air; the log's highest entry never falls below a reservation reported written; nothing outside `0x102000-0x105FFF` changes; no byte is programmed twice without an erase. Mutations of the slot choice, the scan and the boot start were each caught, and so was removing the guard that keeps the highest entry's sector.

### Implementation: identity record (#21)

Built 2026-11-18, host-tested, reviewed, provisioned and flashed on the bench 2026-11-18 (identity record seq 1, kept over a power cycle) **[V]**; #21 closed (`test-bench.md`). Code: `src/store.c` (`store_id_*`), `src/identity.c` (boot and hand-over), `src/radio.c` (`identity_boot()` before `link_start()`), `src/session.c`; the host tool `tools/identity/` (`identity.c`, `identity.tcl`, `provision.sh`); tests `tests/host/test_identity.c` and `tests/host/test_identity_tool.sh`. Serial and EUI-64 are no longer build values: `BRIDGE_SERIAL` and `BRIDGE_EUI64` in `config/local.cmake` are ignored with a CMake warning.

**Identity record v1 [I]:** the slotted record format above, at `0x100000` (slot A) and `0x101000` (slot B). Magic `0x314E4449` ("IDN1"), version 1, payload 24 bytes: serial 16 bytes ASCII, zero padded, then the EUI-64, 8 bytes in stored order (`00 1b c5` first, as `BRIDGE_EUI64` was, `build.md` P4); 40 bytes in all. Accepted only if the serial is 3 to 16 characters, two capital letters then capital letters or digits (tanoclo's key, "IB" and 10 digits for a bridge, `build.md` P4), the padding after it is all zero, and the EUI-64 starts with tado's OUI `00 1b c5` (`store_id_valid()`). Boot takes the valid slot with the highest seq (`store_slot_load()`, as for the network); if that newest record is refused, the bridge has no identity: an older slot is not used in its place.

**Who writes it:** only the host tool, over SWD. The firmware cannot: both identity sectors lie in none of the flash driver's writable areas (the records `0x102000-0x105FFF`, "Address guard" above; since #24 also the boot state and the update slots, one at a time), so `src/mx25r.c` panics on an erase or program there, and a `_Static_assert` in `src/flash_layout.h` keeps the sectors outside it. `store_id_write()` is in `src/store.c` for the tool, and the firmware image does not contain it (nothing calls it; checked with `arm-none-eabi-nm`, 2026-11-18). **Differs:** the design's lifecycle says the tool writes the record "at flashing", as part of a 2 MB external image (#20, "First flash"); the tool writes one 4 KB sector in place instead, so the network record and counter log already on the bench unit stay. **Differs (commit order):** on the chip the SPI stub programs the whole 40-byte record in one page program (it programs and compares whole pages, `spi_stub.c`), so "magic programmed last" holds for `store.c` (the host tests and the model) only; a torn program is caught by the CRC-32, which gives no identity and fails closed.

**Provisioning (`tools/identity/provision.sh`):** builds `build/host-tools/identity` (plain gcc, linked with `src/store.c`, so the record comes from the same code and rules as the firmware's records); checks that tanoclo's SRAM stub (`tanoclo/patch_ib_firmware/spi_stub.elf`, the one the P0 backup was read with) has its symbols at the addresses in `spi_stub_addrs.tcl`; drives the radio CSn (PA4) high before the stub runs (#28; `src/board.c`); reads both identity sectors, each until two reads agree; builds the new sector (the record written into the older or invalid slot, the rest `0xFF`); asks; writes that one sector (the stub erases it, programs all 16 pages and compares each page, `spi_stub.c`); reads both sectors again and checks that the record loads as the firmware loads it, that the written sector reads back as built and that the other sector did not change; resets the bridge. `tools/identity/identity.tcl` refuses any address but `0x100000` and `0x101000` and any file that is not one 4 KB sector. The serial and EUI-64 come from the backup of the internal flash: the configuration marker `42 49 00 00 54 46`, serial u32 at marker + 8, EUI-64 at page start + 50 (`tanoclo/patch_ib_firmware/extract_rf_key.js:51-71`); the key next to them is not read. The tool and the script print the serial and EUI-64 masked (serial prefix and length, last EUI-64 byte); the full values only with the tool's `--show` flag. The script sends the tool's messages to `<workdir>/make.log`, shows the sector line and the masked new record before the confirmation, and ends with `identity record OK` and `provision: done`. Before the erase, `verify_image` checks the sector in the stub's buffer and then the stub's code. `identity.tcl` first moves OpenOCD's work area to `0x20010000` (32 KB) so that verify_image's CRC routine cannot overwrite the stub, and `provision.sh` refuses a stub whose `_end` reaches past `0x20010000`. The work folder must lie outside git (the script refuses one inside a repository). The tool refuses to replace a different valid identity without `--replace`, and writes nothing if the record already holds the identity.

**First write attempt failed, nothing written [V]:** 2026-11-18. The read phase worked (slot A held stock data, slot B was blank), but the write of slot A ended with `stub timeout at 0x100000`, and slot A read back unchanged (log `~/ib01-backups/identity-2026-11-18.log`, outside git). OpenOCD reported "halted due to breakpoint" at pc `0x2000002e`. That address is the middle of a 32-bit `mov.w` in the stub (`arm-none-eabi-objdump` of `spi_stub.elf`: `0x2000002c`), and it is the `bkpt #0` offset of OpenOCD's armv7m CRC routine **[D]** (`contrib/loaders/checksum/armv7m_crc.s`). Cause: `target/stm32f4x.cfg:50` puts the work area at `0x20000000` with `-work-area-backup 0`, and the stub's code is linked at `0x20000000`. The `verify_image` added before the erase (commit `368910f`) ran its CRC routine there, overwrote `spi1_xfer`, and the stub ran into the routine's `bkpt` on its first SPI byte, before any command reached the flash. The read phase was unaffected because it has no `verify_image`. Reproduced in SRAM only the same day **[V]**: after `verify_image`, the stub's first 64 bytes differ from the ELF and the halfword at `0x2000002e` is `0xBE00` (`bkpt #0`). With the work area at `0x20010000`, the code is unchanged and the stub still reads a sector (status `0x11`). Flash registers read by a read-only SRAM probe (`9Fh`, `05h`, `15h`, `2Bh`; `mx25r1635f.txt:829`) **[V]**: JEDEC `C2 28 15`; SR `00`, so WIP, WEL, BP3-BP0 and SRWD are clear and nothing is block-protected (`mx25r1635f.txt:1359-1375`, Table 2 at :397-405); CR1 `00`, so TB = 0 (top protect; moot with BP = 0, :1413-1417); CR2 `00`, ultra-low-power mode (:1441-1450); SCUR `00`, so E_FAIL and P_FAIL are clear (:2530-2540).

**Extraction checked [V]:** 2026-11-18, `identity from-backup` on the bench unit's internal-flash backup (`~/ib01-backups/2026-11-11/a/`, outside git) gives the same serial and EUI-64 as the `config/local.cmake` values that tanoclo accepted in P4 (compared by script, values not printed). The external-flash dump holds no marker.

**Slot A replaces stock data [I]:** in the bench unit's stock image `0x100000-0x100FFF` holds data (part of stock's descriptor table, `check_firmware.sh:83`) and `0x101000` is blank ("Stock contents" above). Neither is a valid record, so the first write goes to slot A, as the slot rule says, and erases that stock sector; "Layout (external flash)" accepts this because a backup exists, as the owner decided for network slot A (2026-11-17). Only the first 4 bytes of slot A are read while it holds stock data.

**Fail closed without identity:** no valid record, or the flash not usable at boot (`flash_boot()`): `identity: no valid identity record ...` in the log; the link gets no own address (`link_init(NULL)`), so nothing is acknowledged, answered or sent and the network record is not read (`network_boot()`); the session does not connect or authenticate (`session: no bridge identity ...`); the power LED shows fault code 4 (`hardware.md`, "LED roles"). The receiver still runs and counts frames, as P5 did without a built-in identity (owner decision, 2026-11-18: this is the intended behaviour, matching the lifecycle above); the transmit gate refuses every frame without an identity (`TXGATE_NO_IDENTITY`, `src/cc110l.c`). The session waits for the radio task's verdict (`identity_state()`, published with release/acquire), so it never authenticates with a half-read identity.

**Host tests (`make test`) [I]:** validation and payload layout; loading from a stock image reads only slot A's magic; slot order A, B, A; one flipped bit at every bit of the newest record (the older one wins) and at every byte of a single record (no identity); a newest record with a good CRC but a refused EUI-64 gives no identity, while one of another version or length is not ours and the older one is used; a power cut at every step of a write from no, one and two records, 8 random tears each (72 cases), with nothing outside the identity sectors changed and no byte programmed twice. `test_identity_tool.sh` runs the tool on made-up values: extraction from a fake backup, slot A first on a stock image, the record loads back, "nothing to write" when it is there, `--replace` needed for another identity, malformed inputs and disagreeing markers refused. Mutations of the padding check, the OUI check and the serial's first two characters were each caught.

The application's side of the boot state: built 2026-11-19, flashed and run on the bench 2026-11-19 [V] (`test-bench.md`, "Self-update (#24): first over-the-air update, result"; "Implementation: self-update, application side (#24)"). (Factory reset: built 2026-11-18, "Implementation: factory reset (P6)" below.) The Ethernet MAC: since #39 (built 2026-11-20, not yet on the bench) a version 2 identity record holds the original bridge's MAC; with a version 1 record the firmware keeps the placeholder of #12 (`src/net.c`). See "Implementation: Ethernet MAC, identity record v2 (#39)" below.

**Slot order (owner decision, 2026-11-18):** the first identity write goes to slot A (`0x100000`); later changes use `--replace`, which writes the older slot first.

**Bench result [V]:** 2026-11-18, identity provisioned and flashed with `tools/identity/provision.sh`; the boot log shows identity record seq 1; a power cycle (POR) keeps the identity and the network record.

### Implementation: Ethernet MAC, identity record v2 (#39)

Owner decision 2026-11-18: the Ethernet MAC must be the original bridge's. Built and host-tested 2026-11-20; `fw-reviewer` verdict safe to flash (2026-11-20); installed on the bench over the air 2026-11-20 with the version 1 record, so with the placeholder MAC **[V]** (`test-bench.md`, "Identity v2 image (#39): first flash"); **not yet provisioned**. Code: `src/store.h`, `src/store.c` (`store_id_*`), `src/identity.c`, `src/net.c` (`mac_choose()`); the tool `tools/identity/` (`identity.c`, `provision.sh`); tests `tests/host/test_identity.c` and `tests/host/test_identity_tool.sh`.

**Where the MAC comes from:** the owner gave it by hand (way (a) of the decision). It is kept outside git, in `~/ib01-backups/original-bridge-mac.txt`, and is never printed, logged or committed; the tests use made-up, locally administered MACs. Way (b), copying it from the backup, was checked first, by script and without printing the value (2026-11-20) **[V]**: the MAC is in none of the P0 backup files (`~/ib01-backups/2026-11-11/`: internal flash, external flash, OTP, unique ID) in plain form (the 6 bytes, reversed, ASCII hex with and without colons, the last 3 bytes alone), and no run of 3 or more of its bytes occurs in the unique ID, the EUI-64 or the serial. So the stock firmware computes it some other way or keeps it elsewhere **[I]**; not looked into further, since the owner's value settles it.

**Identity record v2 [I]:** the same magic `0x314E4449` ("IDN1"), version 2, payload 30 bytes: the version 1 payload (serial 16 bytes, EUI-64 8 bytes, "Implementation: identity record (#21)" above), then the MAC, 6 bytes in transmission order; 46 bytes in all, still inside the sector's first page. Accepted only under the version 1 rules plus: the MAC is unicast (bit 0 of the first octet clear) and not all zero; globally and locally administered MACs are both accepted (`store_id_valid()`). A version 1 record stays valid; it gives the identity without a MAC.

**One slot rule over both versions:** a slot holds a version 1 or a version 2 record (a record's version field holds one value). Boot takes the newest slot of either version, slot A on equal seq, as `store_slot_load()` does; if that newest record is refused, the bridge has no identity, as for version 1. A record of any other version, or with a length that does not match its version, is not ours. The tool's write chooses the slot and the seq over both versions: the slot with the newest record of either version is never erased, and the new seq is above both. **Found while building:** `store_slot_write()` sees only records of the version it writes, so a version 2 write next to a single version 1 record would have erased that record (and written a second seq 1). **Differs (code):** `store_id_write()` therefore repeats `store_slot_write()`'s commit steps (erase, check blank, program all but the magic, read back, magic, read the whole record back) instead of sharing them, so that `store_slot_write()`, which the bootloader links for the boot state (`src/fwimg.c`), is unchanged. The bootloader binary is byte-identical before and after #39 (sha256 of the `.bin`, 2026-11-20). The firmware image still contains no `store_id_write()` (`arm-none-eabi-nm`, 2026-11-20).

**A firmware from before #39** knows version 1 only: for it a version 2 record does not exist, and it loads a version 1 record in the other slot if there is one. On the bench unit (version 1 seq 1 in slot A, slot B blank, "Bench result" above) the first version 2 write goes to slot B with seq 2, so slot A keeps the version 1 record, and a rollback to an image from before #39 (#24) still has its identity, with the placeholder MAC. A second version 2 write (another MAC, `--replace`) goes to slot A; from then on an image from before #39 finds no identity and fails closed (LED code 4). The tool says which case applies (its `note:` lines).

**Firmware:** `identity_boot()` logs the record's seq and version and "Ethernet MAC set" or "no Ethernet MAC"; the MAC itself is not logged, like the EUI-64. The net task waits for the radio task's identity verdict (`identity_state()`, release/acquire, as the session does) before `lwip_init()`, and then takes the record's MAC, or the placeholder `02:7a:d0:b1:00:01` with a version 1 record or without identity, once (`mac_choose()`, `src/net.c`). The netif, the ENC28J60's MAC address registers (`enc28j60_init()`, also on every re-initialisation) and the own-frame filter on receive all use that one value. The wait is short: the radio task publishes the verdict right after `flash_boot()` (at most 480 ms of WIP polling, "Boot wait" above), and the net task keeps its watchdog check-in while it waits. A new MAC means a new DHCP client on the bench network: dnsmasq gives a new lease, and the placeholder's lease (`10.42.0.43`, `prototype-plan.md` P3) is no longer used **[I]**.

**Tool:** `<id>` takes `--mac-file <file>` (one line, 6 bytes in hex; so the MAC stays out of the shell history) or `--mac <hex>` after `--backup` or `--serial ... --eui64 ...`; with a MAC the record is version 2, without one version 1. Adding a MAC to a version 1 record with the same serial and EUI-64 is no replacement; changing or removing a MAC needs `--replace`. The masked output says "MAC set" or "no MAC"; the MAC itself is printed only with `--show`. `provision.sh` passes the options through and shows the record's seq, version and the tool's notes before it asks. **Dry run, no bench (2026-11-20) [I]:** the tool, given the owner's file and the bench unit's identity sectors as read back on 2026-11-18 (`~/ib01-backups/identity-2026-11-18b/after/`), builds a version 2 record seq 2 for slot B (`0x101000`), notes that a firmware from before #39 finds the version 1 record seq 1, and `check` loads it with the MAC; output in `~/ib01-backups/identity-39-dryrun/`, outside git.

**Size:** application text 171132 to 171844 bytes (+712) with the bench `config/local.cmake`, 165804 with `RADIO_TX_ALLOWED=0`; bootloader 6120, unchanged.

**Host tests (`make test`) [I]:** `test_identity` 113331 checks (27273 before), 288 power cuts (72 before). Added: the version 2 layout and checks (group, broadcast and zero MACs refused, a MAC without `has_mac` refused, versions 0 and 3 refused); a newest version 2 record with a group MAC and a good CRC gives no identity, the older version 1 record is not used; six pairs of version and length that are not ours; the bench unit's upgrade (version 1 in slot A, version 2 into slot B, slot A byte for byte unchanged, a firmware from before #39 still finds version 1), then a second version 2 into slot A and back to version 1 into slot B; version 1 in slot B, version 2 into slot A; the newest slot wins across versions; equal seq takes slot A and the next write goes to slot B; one flipped bit at every bit of a newest version 2 record above a version 1 record; a power cut at every step of a write for each pair of old and new version from no, one and two records, checking that the identity is the old or the new one and that the version 1 record a firmware from before #39 would load is never lost. `test_identity_tool.sh` 47 checks (22 before): the upgrade without `--replace`, `check` with, without and with another MAC, "nothing to write", another MAC or none refused without `--replace`, refused MAC files (group, zero, broadcast, short, two lines, not hex, missing), and no MAC in the default output or in an error. Mutations caught: the MAC's group-bit check removed; the write's slot choice by the new version only; version 1 records not read; equal seq to slot B.

### Implementation: flash work during radio traffic (#22)

Built 2026-11-18, host-tested, reviewed (`fw-reviewer` "safe to flash") and flashed on the bench 2026-11-18, measured on air the same day **[V]** (`test-bench.md`). Code: `src/mx25r.c` and `src/mx25r.h` (gate, waits, port), `src/radio.c` (`mx25r_port_radio_busy()`, `mx25r_port_radio_serve()`, `rx_service()`, `quiet()`); test `tests/host/test_mx25r.c`.

**The problem [I].** Until #22 the store blocked the radio task for the length of a write ("Differs, SPI1 sharing" above): the program wait spun for up to 10 ms and the erase wait slept for up to 240 ms, and during both the RX FIFO was not read. The Enhanced ACK depends on that read: the task prepares the ACK once a Data frame's header is out of the FIFO, while the frame is still arriving (`radio-runtime.md`, "Enhanced ACK path"), and the GDO2 interrupt sends it only if it was prepared. So a Data frame that arrived during a write got no ACK, and one longer than about 62 bytes could overflow the FIFO. The ACK interrupt itself never waited for the flash: SPI1 transactions run in critical sections of at most about 91 us, and the interrupt finds the bus between two of them (`src/spi1.c`).

**Datasheet facts used [D].**
- A program or erase is self-timed from CS# high; WIP stays readable with RDSR throughout (`datasheets/mx25r1635f.txt:2149-2151, 2323-2325, 1142-1144`). So the bus is free for the radio during the whole cycle; only the flash's own array and RDID are unavailable (`:556-557, 968-969`).
- Cycle times in ultra low power mode, the chip's mode (it is never changed, "Differs, SPI1 sharing" above): byte program 40 us typical, 100 us max; page program 3.2 ms, 10 ms; sector erase 58 ms, 240 ms (`:3334-3341`). High performance mode: 0.85 ms, 4 ms; 40 ms, 240 ms (`:3432-3436`).
- Program/Erase Suspend (B0h) and Resume (30h) exist so that the array can be read during a cycle, except the page or sector being written (`:2581-2607`, Table 9), with a latency tPSL/tESL of up to 60 us (`:3330-3331`).

**Design [I].** Not Erase Suspend: the ACK path needs the bus, not the array, and the bus is free while the cycle runs. Instead, two rules in the driver, with a call back into the radio task (`mx25r_port_*` in `src/mx25r.h`):
1. **Gate:** no flash bus transaction starts while `mx25r_port_radio_busy()`: a frame arriving or not yet taken from the FIFO (`packet_arriving()`: GDO0 high, a sync edge seen, or a frame being assembled), or a frame of ours on the air (`cc110l_tx_expected()`, an ACK's airtime plus 2 ms). The driver serves the radio instead. The longest transaction (a 64-byte READ or PP, 68 bytes at 6 MHz, 91 us) is shorter than the 1.28 ms from a frame's first preamble bit to its sync edge (`_Static_assert` in `src/mx25r.c`), so a transaction started on a quiet radio never holds a CRC-OK edge, and the ACK interrupt is never delayed by the flash.
2. **Serve during the cycle:** between RDSR polls the driver calls `mx25r_port_radio_serve()`: one receiver pass of the task loop (`rx_service()`: wait for a GDO interrupt or the next 1 ms tick, ACK-interrupt recovery, sync handling, FIFO pump with `ack_consider()`, `frame_done()` with the link, the beacon answer and the frame's log line), then the TX-state watch and the watchdog check-in. The pump runs every tick from the sync edge, as in the loop, so the ACK is prepared as without flash work.

Not run from inside flash work: the coordinator (`csl_step()`, so no transaction starts during flash work; it resumes afterwards, as after any long loop pass), and the quiet-moment work (health check, calibration, minute log, pairing trigger). Their notifications wait in `pending_bits`, and the loop does not block while any are parked. The loop asks again after each step that may touch the flash (flash ID check, factory reset, pairing trigger, counter extend), because frames are taken during flash work. Flash work starts only with the coordinator idle (`flash_quiet()`, since the bench finding below; before it, `quiet()`).

**Bounds [I].** If the radio stays busy for `MX25R_GATE_MAX_US` (100 ms, longer than the radio's 60 ms stale-frame limit plus an ACK's 8.5 ms), the transaction goes ahead (counted as "gave up"), and the gate does not wait again until it sees the radio quiet: a GDO0 pin stuck high costs one 100 ms wait, not one per transaction, and the health check after the flash work re-initialises the radio. The program timeout (20 ms) and erase timeout (480 ms), twice the worst case, now run on the microsecond clock. A serve pass blocks for at most one tick, so programs take up to about 1 ms longer than the chip needs; the net task gets the CPU meanwhile instead of a busy spin.

**Nesting [I].** A serve pass runs inside the store's call (for example `network_create()`, a sector erase, a serve pass, `frame_done()`, the link, CCM, a log line). Nothing on that path touches the flash, and a `serving` flag stops any nesting. Stack, by `-fstack-usage` per function (without LTO): about 2.1 KB on the deepest such path (radio task 152 B, `network_create()` 88, `store_slot_write()` 136, `erase_verified()` 80, `rx_service()` 208, `link_on_frame()` 136, `relay_uplink()` 152, `lowpan_reasm_add()` 120, CCM 168, `log_printf()` 216 plus newlib's formatter, and an exception frame of up to 104 B), against the radio task's 3.5 KB (896 words) and the 1.4 KB measured at most on the bench before (`open-questions.md` row 17d). To check on the bench ("alive:" line after a factory reset or a network creation).

**Logged every 60 s:** `flash: 60 s: radio served N times during flash work, frames N, acks N; totals: cycles N (longest N us), held N (longest N us, gave up N)`.

**Host test (`tests/host/test_mx25r.c`, `make test`) [I].** The real `src/mx25r.c`, with the real `src/store.c` and `src/store_port.c` above it, against a simulated MX25R1635F on SPI1 (command decoding, WIP, WEL, cycle times drawn up to the datasheet maxima, fail flags) and a simulated receiver with random traffic (frames of 5-128 bytes, 0.3-20 ms apart, Data frames of 31 bytes or more mostly addressed to us, our ACK's airtime after each), on one simulated clock. Checked: every frame that ends during flash work and needs an ACK had it prepared before its end; no transaction started on a busy radio; no CRC-OK edge inside a transaction; no command but RDSR and RDSCUR during a cycle; data correct; the boot wait for a cycle left from before a reset; the timeouts with the radio served throughout; the fail flags; the address guard; a stuck GDO0 (one give-up, then the flash works). One run: 353 flash calls in 55 s of simulated air, 1343 frames acknowledged during flash work, 2128 transactions held (longest wait 21 ms), ACK prepared at most 997 us after the header was in, 430268 checks. Mutations: the old driver (no gate, no serving) fails it with 1167 late ACKs and transactions on a busy radio; removing only the gate is caught (transactions on a busy radio and over CRC-OK edges); a gate that never gives up is caught (a deadline in the test).

**Measured on the bench (2026-11-18) [V]:** 89 forced sector erases during traffic; 87 of 87 uplinks ACKed, 42 of them inside a flash window, Data end to ACK preamble 225.1-233.8 us (median 225.9 us), too late 0, overflow 0, gave up 0; the radio stack used at most 398 words. One finding: an erase can start while a short train waits in `ST_WAIT` (`csl_busy()` looks only 10 ms ahead), and the train then starts late; the retry delivers it. Details and the open read-back after a reset: `test-bench.md`, "Flash work during traffic (#22) result". Fixed ("Flash work and the coordinator" below) and rerun the same evening aimed at that case: 71 of 71 downlinks delivered at attempt 1 with 21-22 wake-up frames ("Flash work during traffic (#22) rerun").

#### Flash work and the coordinator (#22 bench finding)

Built 2026-11-18 (`37b81c8`), host-tested, flashed on the bench (main `774ec94`), **verified on air 2026-11-18 [V]**: 71 forced erases, each on a downlink waiting in `ST_WAIT`, all 71 delivered at attempt 1 with 21-22 wake-up frames (`test-bench.md`, "Flash work during traffic (#22) rerun"). Code: `src/csl.c:535, 720-726` (`csl_idle()`, `csl_hold()`), `src/radio.c:1927` (`flash_quiet()`) and `:2074-2091` (the loop), `src/network.c:150` (`network_service_due()`), `src/store.c:441` (`store_counter_due()`).

**Cause [V]/[I].** `quiet()` allowed flash work while `csl_busy()` was false, which includes a job waiting in `ST_WAIT` with its listen 10 ms or more away. `csl_step()` does not run during flash work, so a sector erase pushed the short train of such a job late: on the bench 72 ms, 4 wake-up frames instead of 21-23, no ACK, delivered by the retry 2 s later.

**Rule [I].** A flash program or erase starts only when no transaction is set up at all (`csl_idle()`: `ST_IDLE`) and no frame is arriving (`flash_quiet()`). Only `csl_step()` sets a transaction up, and it does not run during flash work, so the coordinator stays idle for the whole of the work, however long it takes (one 10 ms program, a 240 ms erase, a create of about 0.8 s, a factory reset of about 1 s, or retries after failing programs). No scheduled listen or train can be pushed late. A rule based on the work's length (`csl_quiet_for(240 ms)`) would need a bound on every piece of work, including failure paths, so it was not used. The flash ID check (one read, no cycle) also waits for `flash_quiet()`; it has no hold and is only put off.

**Hold, so the work is never starved [I].** While work that may program or erase is due (factory reset, pair button, counter reservation with fewer than 2048 counters left), the radio task sets `csl_hold()` on every loop pass: `schedule()` starts no new transaction, so the coordinator is idle once the transaction in flight ends on its own timing (nothing is cut short). The work then runs at the next pass with no frame arriving, and the hold is dropped on that same pass, before the next `csl_step()`. Without the hold, a queue that never empties starts its next job on the pass after each transaction, and a frame arriving on that one pass would put the work off to the next gap. The hold changes nothing else: frames are received and acknowledged, the beacon answer still takes its seq and counter (`csl_take_seq_counter()`), and queued jobs only wait.

**Counter reservation margin [I].** The write is due once fewer than 2048 counters remain (`STORE_CTR_LOW`, `src/store.h`). From then on, at most the transaction in flight (its counter is already taken) and beacon answers take counters before the write: a transaction lasts at most about 1.6 s (an anchored train with the largest guard: listen and backoff 7.8 ms, sample up to 1 s ahead, 0.5 s guard after it, 50 ms ACK window; `deadline_us` adds 0.4 s), and the coordinator takes at most one counter per transaction. If the write fails, `network_service_due()` is false for the 10 s retry wait, so the hold is not kept on; 2048 counters last more than 100 s even at about 12 transactions per second, the most short trains allow, so many retries fit before the limit. At the limit the coordinator stops by itself (`schedule()`), which also leaves it idle for the write: a counter is never used twice.

**Deadlines [I].** The Enhanced ACK path is untouched: the ACK interrupt and `ack_consider()` run as before, also inside flash work (`mx25r_port_radio_serve()`). The pairing beacon's gate, `csl_quiet_for(PAIR_BEACON_QUIET_US)`, is true whenever the coordinator is idle, so the hold can only make it pass more often. Cost: a downlink or broadcast queued while work is due waits for the work (at most about 0.25 s for a counter-log write with an erase, about 1 s for a create or a factory reset), and a flash write waits for the transaction in flight (at most about 2 s).

**Power cuts [I].** The store's write order and formats are unchanged; only the moment a write starts moves. Every power-cut case in "Counter log" and "Slotted records" above holds as before.

**Host tests [I].** `tests/host/test_csl.c`: `test_flash_waits_for_idle()` (the bench case: an erase due while an anchored job waits 80 ms before its listen; the old gate reproduces the late train, the new one runs the erase after the delivery, with 19-23 wake-up frames on attempt 1; a full bootstrap train is the longest wait), `test_hold()`, and `test_counter_reservation_under_traffic()` (two devices, a queue that never empties, a frame arriving on the first pass after every transaction, each reservation write a 240 ms erase: never at the limit, every downlink delivered at its first attempt; without the hold the limit is reached). `tests/host/test_store.c`: `store_counter_due()` agrees with `store_counter_extend()` on every step of the counter run (no flash operation when not due). Mutations: `csl_idle()` as `!csl_busy()` fails 6 checks; a `csl_hold()` that does nothing fails 11.

**Bench check (2026-11-18) [V]:** 71 forces aimed at the worst case (the coordinator in `WAIT` at every `limit` write): every downlink in a force window delivered at attempt 1 with a short train of 21-22 wake-up frames, in the RTT and on air; no `no ack` line; `limit` moved 224-1207 ms after the write (the work waits for the transaction in flight, then runs with the coordinator idle); the hold was off after every append; uplink ACK gap 225.9 us median, too late 0, overflow 0, gave up 0; radio stack 494 words never used. No uplink fell inside an erase in this run (`test-bench.md`, "Flash work during traffic (#22) rerun").

### Implementation: factory reset (P6)

Built 2026-11-18, host-tested, flashed on the bench (main `774ec94`). The owner ran a factory reset and re-paired the spare device on 2026-11-18, and both worked **[V, owner by eye]**; the RTT lines were not recorded (`test-bench.md`, "Factory reset and LEDs, owner check"). Code: `src/button.c` (debounce, long press), `src/app.c` (housekeeping task), `src/radio.c` (`factory_reset()`), `src/network.c` (`network_factory_reset()`), `src/store.c` (`store_factory_reset()`); tests `tests/host/test_button.c`, `tests/host/test_store.c`.

- **Trigger [I]:** the factory reset button (PB10, active low with the internal pull-up, `hardware.md`) held **10 s** (`FACTORY_HOLD_MS`; our choice, not an owner spec: long enough not to happen by accident, and the pair button's 1 s hold is distinct). Debounced as before (3 readings 10 ms apart); a bounce shorter than that neither ends the press nor counts. Once per press. **A press counts only if it began after the button had read released for 30 ms:** a button held from power-on, or PB10 stuck low, never erases anything. The pair button keeps its 1 s hold.
- **Action:** the housekeeping task asks the radio task, which owns SPI1 (`radio_request_factory_reset()`). At its next quiet moment the radio task: refuses if the flash was not usable at boot (nothing erased, logged); else the link drops its key and any pairing window (nothing is acknowledged, decrypted or sent from then on), no network may be created in this boot, and `store_factory_reset()` erases network slot A, slot B, then counter-log sector A, sector B, each checked blank. Then, success or not, the MCU restarts (SYSRESETREQ, `datasheets/rm0383.txt:4551-4559`) after 200 ms for the last RTT lines. The next boot reads what the flash holds: after a complete reset `network: no network record: no key, nothing sent`, and the next pair-button window creates a new network. The identity record is never touched: its sectors lie in none of the driver's writable areas (`src/flash_layout.h`, `src/mx25r.c` guard). Frames are still received during the erases (#22) but, without a key, not acknowledged.
- **Order and power cuts [I]:** a network record must never outlive the counter log it relies on, or a reboot would restart its frame counters at the record's floor and reuse counters under the same key. So the counter log is erased only after both network slots read back blank, and the reset stops at the first failed erase. A cut at any step leaves the old network with its whole counter log, a damaged record (no key, and no new network over it until a reset completes), or no network; holding the button again finishes the job.
- **Warning flicker [I]** (owner request 2026-11-18; built, host-tested, flashed; **seen by the owner on the bench on 2026-11-18 and fine [V, owner by eye]**: router, power and cloud flicker, the pairing LED stays as it was; `test-bench.md`, "Factory reset and LEDs, owner check"): from 7 s to 10 s of the hold the router, power and cloud LEDs go together (owner correction 2026-11-18: the pairing LED does not take part) 500 ms off, 500 ms on, starting off (three cycles, ending on), then the erase starts. The phase is the pure function `button_warn_led(held_ms, hold_ms)` (`src/button.c`, `tests/host/test_button.c`) of the hold time the button already counts; the housekeeping task (`src/app.c`) applies it every 10 ms poll through `board_led_override()` (`src/board.c`), the one place the LEDs are written. `board_led()` stores the requested state per LED (one byte each) and writes the forced level while the override is active; on release the override clears and the requested states are written back at once (first released reading, not after the 30 ms release debounce; a bounce of 1-2 readings costs a 10-20 ms blink). The power LED (PB1) takes part and the pairing LED does not (owner correction 2026-11-18; `src/board.c:133-134`); the panic path clears the override, so a fault code still shows on PB1 (`774ec94`). No log line, lock or wait; the radio task's `board_led(LED_PAIRING, ...)` calls stay a byte store plus the same BSRR write. Costs: during the flicker the pairing LED's 3 ms beacon scope marker is not visible, and a `board_led()` racing with an override change can leave one LED wrong for up to 10 ms while active. After the hold fires the override ends and the LEDs follow their normal rules.
- **Receive-only images** (`RADIO_TX_ALLOWED=0`) and images without a pairing target also carry the factory reset: it erases, it never creates a network [I].
- **Host tests [I]:** `test_store.c`: uncut (no network, empty log, nothing else in the flash changed, identity included; a new network can be created; repeatable); a cut at every one of the four erases from one and from two records, 8 random tears each (64 cases: 8 kept the network, 56 had none, none damaged in this run; a network that loads has its whole counter log and hands out no counter below one used; a second reset finishes); erases failing with the power on (fail flag, partial, silent) leave the counter log untouched. Reversing the order (counter log first) and going on after a failed erase are both caught. `test_button.c`: debounce, hold timing to the poll, bounces, held at power-on, the pair button unchanged.

### Depends on

- #26: how the network key is generated (see "Entropy").
- #24: see "Self-update and first flash".

## Entropy (#26)

**Decision (user):** use the ADC value of an unconnected pin. The F411 has no hardware RNG (`datasheets/stm32f411ce.txt`).

**Pin:** **PA3** (package pin 13, ADC1 channel 3), reported unconnected by the user on 2026-11-09 (`stm32f411ce.txt:2288`).

**Use [I]:** a floating pin picks up mains hum and board noise, so raw readings are correlated and partly predictable. Treat them as a weak source:

- Take many samples, keep only the low bits, and run them through a hash (SHA-256 is already needed for TLS).
- Mix in other varying inputs: the 96-bit unique ID, radio `RSSI` readings, and timing jitter.
- Refuse to create a key if the readings are stuck or barely vary.
- Seed a DRBG for TLS from this pool, as TLS needs fresh randomness at every connection.
- Measure the real pin's readings on the board before relying on it. Done on 2026-11-14, see "Entropy: measurement" below.

Configuring a pin as an analog input does not drive it, so testing a candidate is safe **[I]**.

### Entropy: measurement (#14) [V]

Measured on the bench unit on 2026-11-14. Setup: ADC1 channel 3, 12 bit, ADCCLK = PCLK2 / 4 = 24 MHz (36 MHz maximum at VDDA 2.4-3.6 V, `datasheets/stm32f411ce.txt:6506`), single conversions started by software in a tight loop (`src/pa3_adc.c`). The readings were taken in the net task before the ENC28J60 bring-up, with the radio receiving on channel 26, as the production seed is. USB power, VDDA 3.28 V (`test-bench.md`). For each of the 8 sampling times, 128 blocks of 8192 readings, with the settings interleaved block by block, were read out of RAM over SWD. The first 64 readings of each block were dropped, leaving 1.04 million readings per setting. The temporary sampling code is removed. The captures stay outside git.

Analysis: `tools/entropy/pa3_noise.py`. It runs our own implementation of NIST SP 800-90B section 6.3 estimators on the low 8 bits: MCV, t-tuple, LRS, MultiMCW and Lag. It also runs a nearest-neighbour predictor of our own, which predicts each reading from what followed the closest match of its last 4 readings. NIST's reference tool was not built here; its source was read only to check the structure of the estimators.

**Readings.** At every sampling time the mean is 910 LSB, about 0.73 V **[I]**, and the standard deviation is 26 LSB. Right after power-up the readings start near 560 and reach the pin's level within 10-20 readings as the sample-and-hold capacitor charges, so the first 64 are discarded. The lag-1 autocorrelation is +0.12 to +0.18, and it is under 0.07 from lag 2 on. Less than 6 % of the power lies below 1 kHz, so mains hum is not the main part. The low 4 bits are not uniform: the most common value occurs 1.6-2.2 times as often as the least common one. Bits 0-5 are 1 with a probability of 0.45-0.56.

**The readings are mostly deterministic.** The 26 LSB spread is not 26 LSB of noise. The sequence often falls into exact cycles: a period-3 cycle such as `919 905 869 919 905 869 ...`, which recurs in different blocks and at different sampling times, or one value repeated up to 67 times in a row (at 15 cycles; at most 12 at 3 cycles). A 37-reading pattern recurs 48 times in a million readings at 15 cycles. Our reading is that the ADC's sampling capacitor shares its charge, which depends on the last result, with the floating pin. That makes each reading mostly a function of the previous ones, with a little noise on top **[I]**. Predictors that look for repeats therefore estimate far less than the spread suggests: MCV gives about 5 bits per reading, but t-tuple, LRS and Lag give 0.34-1.1.

| Sampling time (cycles) | µs per reading | MCV | t-tuple | LRS | MultiMCW | Lag | Nearest neighbour | **Lowest** | Longest run of one value | APT max of 512 |
| --: | --: | --: | --: | --: | --: | --: | --: | --: | --: | --: |
| **3** | 1.04 | 5.19 | 0.87 | 1.05 | 4.44 | 1.07 | 1.44 | **0.87** | 12 | 50 |
| 15 | 1.42 | 5.52 | 0.34 | 0.56 | 0.41 | 0.38 | 1.68 | **0.34** | 67 | 55 |
| 28 | 1.99 | 5.15 | 0.67 | 0.77 | 4.45 | 1.12 | 1.48 | **0.67** | 10 | 49 |
| 56 | 3.30 | 5.00 | 0.84 | 0.97 | 1.87 | 1.12 | 1.45 | **0.84** | 16 | 59 |
| 84 | 4.43 | 5.25 | 0.84 | 1.08 | 4.42 | 1.07 | 1.53 | **0.84** | 15 | 52 |
| 112 | 5.56 | 5.04 | 0.57 | 0.90 | 1.74 | 1.03 | 1.36 | **0.57** | 17 | 54 |
| 144 | 6.87 | 5.25 | 0.87 | 1.02 | 4.54 | 0.98 | 1.58 | **0.87** | 10 | 50 |
| 480 | 20.97 | 5.31 | 0.74 | 1.03 | 4.43 | 0.77 | 1.88 | **0.74** | 11 | 40 |

Figures are min-entropy in bits per 12-bit reading. Each estimate is the 99 % bound of SP 800-90B. A low figure comes from a few long exact repeats in the data, through the longest-run term of the prediction estimates and through t-tuple's long tuples. For example, the 15-cycle row comes from one block that stayed on one value for 61-67 readings. No setting gives clearly more noise. **3 cycles** shares the highest lowest estimate (0.87, with 144 cycles) and is the fastest.

**Not locked to the sampling loop.** The mean over the 128 blocks at each index varies by 2.3 LSB. That equals the 26 LSB spread divided by the square root of 128, so no part of the waveform repeats from block to block at the same position.

**Boot to boot.** A block was captured at a fixed time after each of 12 resets, as the production seed is taken. No two boots agree beyond the third reading. From reading 64 on, readings at the same index match in 1.2 % of pairs, which is chance level for this spread.

**Decision [I]:** sample at 3 cycles. **Credit 0.1 bit per reading,** about a ninth of the lowest estimate at this setting and a third of the lowest at any setting. The large margin is there because a model of the charge-sharing process could predict better than generic estimators can. Collecting is cheap: 5120 readings for 512 credited bits take about 5 ms.

**Limits:** these figures come from one board, at room temperature, on one supply. The estimators are our own implementation. The collision, compression, MultiMMC and LZ78Y estimators and the restart tests of SP 800-90B 3.1.4 were not run. The 12-boot comparison is the only restart check.

### Entropy: implementation (#14)

Code: `src/pa3_adc.c` (ADC driver), `src/entropy.c` (health tests and conditioning), `src/rng.c` (DRBG). mbedTLS 3.6.7 LTS in `vendor/mbedtls`, configured in `src/tado_mbedtls_config.h`. The net task owns ADC1 and the DRBG, and seeds the DRBG before `lwip_init()`.

- **Start-up test** (SP 800-90B 4.3) at every boot: power ADC1 up, discard 64 readings, then run the repetition count test (RCT) and the adaptive proportion test (APT) over 1024 readings, and require that the readings span at least 16 LSB. On the bench they span 102-129 LSB, which covers the design's "refuse readings that are stuck or barely vary".
- **Health-test cutoffs** for H = 0.5 bit per reading and a false-positive probability of 2^-30 per reading: RCT 61 (4.4.1), APT 422 of 512 (4.4.2). They are computed by `pa3_noise.py --h 0.5`, and the tool's values match SP 800-90B's table for 2^-20. H = 0.5 lies between the credit (0.1) and the assessed 0.87. Measured at 3 cycles, the largest values over a million readings were RCT 12 and APT 50, so normal operation stays far from the cutoffs, while a stuck or collapsed source fails. These tests only catch gross failures. A large alternating pattern, for example, would pass them **[I]**.
- **Conditioning:** each 32-byte block is SHA-256 over 5120 readings (512 credited bits, twice the output size), all tested continuously, plus an uncredited header with a domain tag, a block counter and the TIM2 microsecond count before and after. The whole 12-bit reading is hashed. The design said to keep only the low bits, but hashing more bits cannot lower the entropy, and the credit only counts what the low 8 bits carry **[I]**.
- **DRBG:** mbedTLS CTR_DRBG with AES-256 and the derivation function. It is seeded with 48 bytes from the pool (1.5 times the 256-bit strength, so no separate nonce is needed), with the personalization string `tado-bridge ctr_drbg` plus the 96-bit unique ID (uncredited). It reseeds from the pool every 10000 requests (mbedTLS default). `entropy_get()` has the `f_entropy` signature of mbedTLS, so P4 can use it directly: as `ALTCP_MBEDTLS_RNG_FN` for lwIP's `altcp_tls`, which builds its own CTR_DRBG (`vendor/lwip/src/apps/altcp_tls/altcp_tls_mbedtls.c:88-96,747`), or by passing `rng_random` to `mbedtls_ssl_conf_rng`.
- **Fail closed:** a failed start-up test or seed keeps the bridge off the network: the net task logs it, does not start lwIP, and retries every 10 s. A health-test failure during a later reseed latches, and `rng_bytes()`/`rng_u32()` then call `fw_panic()` rather than return a value.
- **Not yet mixed in:** radio `RSSI` (design list above). It needs an interface to the radio task, which owns SPI1. P5 reworks the radio task, so add it then, uncredited. The pool does not depend on it.
- **Not yet thread-safe:** callers in other tasks, such as network-key generation in P5/P6, need a lock first.

**Bench result (2026-11-14) [V]:** 11 boots of the final image. Every start-up test passed: range 833-966, longest run 1-3, APT maximum 4-18 of 512, no read timeouts. Every seed took 43-45 ms. DHCP got its lease and ping answered. Host-side runs of `entropy.c` against stub readings gave these results: a constant value fails the RCT after 61 readings, two values 10 LSB apart fail the range check, a stuck stretch in the middle of a collection fails it and latches, and an ADC that never completes fails at once.

**Fail-closed on the board (#35 check 1, 2026-11-16) [V]:** production image c2b9b4d (flash matched the ELF by OpenOCD `verify_image`), USB power, Ethernet and tanoclo up. The internal pull-down does nothing in analog mode (`datasheets/rm0383.txt:7926`), so PA3 was switched to input with pull-down over SWD. A hardware breakpoint at `0x08000714` in `source_on()`, the first instruction after the inlined `pa3_adc_on()` (after its `delay_us(TSTAB_US)`, `src/pa3_adc.c:45`, `src/entropy.c:111-112`), then `reset halt` and `resume`. At each hit, read-modify-write of PA3's bits only (MODER bits 7:6 = 00 input, `rm0383.txt:8084-8089`; PUPDR at offset 0x0C, bits 7:6 = 10 pull-down, `rm0383.txt:8150,8171-8175`): GPIOA MODER `0xa800a9c0` to `0xa800a900`, PUPDR `0x6400100a` to `0x6400108a`, read back equal, PA13/PA14 bits (`0xa8000000`, alternate function) unchanged. `pa3_adc_on()` sets analog and no pull again on every attempt (`src/pa3_adc.c:30`), so the change was re-applied at each of 4 hits: the boot and 3 retries, 10.1 s apart.

- Every sabotaged attempt: `rng: PA3 start-up test failed: repetition count test: 61 readings, range 0-0, longest run 61, APT max 61 of 512, 0 read timeouts`, then `rng: no random numbers; network stays off, retrying in 10 s`. The ADC still converts with the pin in input mode, and the pull-down holds the floating pin at exactly 0 **[V]**; that the analog path stays connected in input mode is not documented **[I]**. The RCT fails at its cutoff of 61 (`src/entropy.c:48`).
- No DHCP while failing: dnsmasq logged no DISCOVER from the bridge's MAC between the boot before the test and the first passing attempt (`journalctl`, `dnsmasq-dhcp`).
- Power LED code 3: GPIOB ODR bit 1 (PB1), sampled every 20 ms over SWD while running, rose 11 times in 9 s, 3 per 2.5 s cycle (`src/net.c:120`, `hardware.md`, "LED roles"). Watched by eye on 2026-11-16, see below.
- Breakpoint removed, no reset: the next 10 s retry passed (range 852-964, longest run 2, APT max 9), the DRBG seeded in 43 ms, the LED went back to steady on (one rising edge in 15 s, `src/net.c:422`), DHCP lease 10.42.0.43, TLS 1.2 handshake 1073 ms, WebSocket and auth to tanoclo `10.42.0.1:988` ready.
- `reset run` (twice): start-up test passed (range 851-962 and 851-961, longest run 5 and 3, APT max 9 and 11), DHCP lease, `session: ready` after 1093-1101 ms, `ping` 3 of 3.

So a stuck source keeps the bridge off the network on every retry, and a working source brings it back on the next retry or after a reset.

**LED code 3 by eye (#36, 2026-11-16) [V]:** production image, USB power, 4 sabotaged attempts as above (PB1's ODR rose 12 times in 9 s). The owner watched the power LED: "three pulses indeed, then it recovers after 40s-ish".

**Cold start and low supply (#35 checks 2 and 3, 2026-11-16) [V].** Owner-approved temporary exception: the bench unit was powered from the ST-Link 3V3 (`README.md`, "Project decisions"). Raw captures are in `~/ib01-captures/pa3-35/` (outside git). Check 3 used a temporary capture build (measurement code, never committed) that read 128 blocks of 8192 readings from RAM over SWD at 3 cycles, analysed with `tools/entropy/pa3_noise.py --h 0.5` (skip 64) and a run/APT script; the USB baseline is the same build on USB.

| | USB (VDDA 3.288-3.291 V) | ST-Link 3V3 (VDDA 2.666-2.732 V) |
| :-- | --: | --: |
| Mean, sd (LSB) | 911.7, 28.5 | 782.5, 27.8 |
| Range | 840-963 | 704-938 |
| Lag-1 autocorrelation | +0.24 | +0.213 |
| Low-4-bit count max/min | 4.2 | 1.33 |
| MCV / t-tuple / LRS | 4.47 / 0.85 / 1.00 | 5.87 / 1.11 / 1.28 |
| MultiMCW / Lag / nearest neighbour | 4.43 / 1.03 / 1.31 | 1.74 / 0.95 / 1.58 |
| **Minimum estimate (bit per reading)** | **0.85** | **0.95** |
| Longest run of one value | 7 | 14 |
| APT maximum of 512 | 37 | 28 |
| Read timeouts | 0 | 0 |

- **Check 3 result:** at 2.67-2.73 V the minimum estimate is 0.95, no lower than at 3.29 V. The mean drops by 129 LSB (the pin's level scales with VDDA) and the spread is unchanged. The longest run of 14 is far below the RCT cutoff of 61, and the APT maximum of 28 far below 422. The 0.1-bit credit and the H = 0.5 cutoffs (RCT 61, APT 422, `src/entropy.c:40-50`) hold at 2.67-2.73 V.
- **USB baseline versus #14:** the low-4-bit counts are more uneven (max/min 4.2, against 1.6-2.2 in #14) and lag-1 is +0.24 (against +0.12 to +0.18), but the minimum estimate is nearly unchanged (0.85, against 0.87). The unevenness is not a change in the credited entropy [I].
- **Cold boots (check 2):** RCC_CSR `0e000000` (POR/PDR, PIN, BOR) on each. Cold boot on the ST-Link supply, capture build: start-up test passed (1024 readings, range 719-830, longest run 1, APT 12), seed 45 ms. Cold boot on USB, capture build: passed (run 5, APT 33), seed 45 ms, session ready on attempt 1 (1092 ms). Cold boot on the ST-Link supply, production image: passed (range 766-940, run 3, APT 3), seed 43 ms. Final cold boot on USB, production c2b9b4d: passed (range 848-962, run 8, APT 20), ping 3 of 3, then after `reset halt` and `verify_image` (131644 bytes) and `reset run`: passed, session ready on attempt 1 (1090 ms).
- **Settling:** the first ~8 readings after ADC power-on settle upward (446 to 730 on the ST-Link supply, 586 to 928 on USB). The analysis skipped 64 readings per block. The production code also discards them: `source_on()` calls `pa3_adc_on()`, `ht_reset()`, then reads and drops `SETTLE_SAMPLES` = 64 readings through `read_one()` only, without `ht_feed()`, and zeroes `ht.samples` (`src/entropy.c:25,111-121`). The start-up range, RCT and APT therefore never see the settling readings **[I]** (read from the code, not separately tested). The start-up test passed on every boot, which fits that.

## Software stack (#18, #17, #19)

### Proposal

| Layer | Choice | Licence |
| :--- | :--- | :--- |
| Scheduler | FreeRTOS | MIT |
| Ethernet driver | Own ENC28J60 driver (Zephyr's as a reference) | n/a |
| IPv4, TCP, DHCP, DNS | lwIP | BSD |
| TLS | mbedTLS, TLS 1.2, P-256 | Apache-2.0 |
| WebSocket client, 28-byte frame | Own, small | n/a |
| CoAP (bridge-local resources only) | Own, small | n/a |
| 6LoWPAN and MAC | Port from freedo | Apache-2.0 |
| AES-CCM, SHA-256, DRBG | mbedTLS | Apache-2.0 |

**Why not a full stack on the radio side [D/I]:** the radio segment is single-hop and link-local only (`radio.md`), and the bridge only has to translate. It needs no routing, and no general IPv6 stack:

- 6LoWPAN IPHC forms actually used: `7e 33` and `7c d7`, with UDP-NHC `f0` (`freedo/tado/build_coap.go:22-24,150-156`), plus FRAG1 and FRAGN.
- Addresses are link-local and derived from the EUI-64.
- ICMPv6 handled locally: RS, RA, NS, NA, Echo.
- UDP 5683 maps directly to the WebSocket frame header.

**Alternatives rejected [I]** (from memory of those projects, not checked here):
- **Contiki or Contiki-NG:** stock looks Contiki-derived, but its standard MACs don't match this CSL scheme, so the radio layer would still be custom, and its IPv6 stack is more than is needed.
- **Zephyr:** the strongest alternative, with an ENC28J60 driver, mbedTLS, WebSocket client and MCUboot available. Rejected for RAM overhead and because its 802.15.4 layer doesn't fit this MAC. Worth revisiting if the driver work becomes a burden.

### Tasks

Preemption is required, because a TLS handshake uses hundreds of milliseconds of CPU for P-256 **[I]**, and the radio cannot stop for that long.

| Priority | Context | Work |
| :--- | :--- | :--- |
| Highest | GDO0, GDO2 interrupts | Timestamp and wake the radio task only |
| High | Radio task | Owns SPI1: FIFO, ACKs, wake trains, CCM, 6LoWPAN, flash requests |
| Medium | Net task | ENC28J60, lwIP, TLS, WebSocket, local CoAP |
| Low | Housekeeping | LEDs, switches, watchdog |

- **One owner per bus,** as in freedo (`freedo/firmware/cc1101_bridge/README.md:58-59`). Flash access is a request to the radio task, which runs it only when no deadline is near.
- Radio and net talk through two queues, so neither blocks the other.
- The ENC28J60 is on SPI2, separate from the radio and flash on SPI1, confirmed on the board (`hardware.md`). The net task owns SPI2.
- lwIP runs with `NO_SYS = 1` inside the net task, which is its only context: no `tcpip_thread`, no lwIP locking (#13, `build.md`, P3) **[I]**.
- The ENC28J60 has an 8 KB packet buffer and an INT pin (`datasheets/enc28j60.txt:22,257`), so net can run late without losing Ethernet frames.

### TLS settings

- Client, TLS 1.2, `ECDHE-ECDSA-AES128-GCM-SHA256`, P-256 chain (`tanoclo/patch_ib_firmware/clone_chain.sh:42,97`). Server is plain Node `tls.createServer` (`tanoclo/ws-server/lib/proxy-manager.js:110-113`), so its defaults should accept this **[I]**.
- Trust root is a build or config value, so no patching applies.
- Buffers: 4 KB in, 2 KB out, instead of mbedTLS's 16 KB default. Depends on the server not sending records over 4 KB. WebSocket messages are small, but confirm on a prototype **[I]**. **Built (#15):** the client also sends max_fragment_length, so the server is bound to small records. mbedTLS can only ask for the smaller of its two buffers (`vendor/mbedtls/library/ssl_misc.h:334-341`, `ssl_tls.c:3033-3038`), so it asks for 2048, and tanoclo honours it **[V]** (see "TLS and WebSocket (#15)").
- WebSocket clients must mask frames with random values, using the same DRBG (RFC 6455).

### TLS and WebSocket (#15)

Code: `src/tls.c` (TLS over lwIP TCP), `src/ws.c` (WebSocket), `src/coap.c` (CoAP codec, TLV), `src/session.c` (auth, time, reconnect); mbedTLS options in `src/tado_mbedtls_config.h`. Build, provisioning and bench results: `build.md`, "P4: TLS and WebSocket (#15)".

**Transport: mbedTLS directly over lwIP's raw TCP API, not `altcp_tls_mbedtls` [I].** The lwIP port builds its own entropy context and a second CTR_DRBG (`vendor/lwip/src/apps/altcp_tls/altcp_tls_mbedtls.c:88-96,740-750`), allocates through lwIP's 6 KB heap (`altcp_tls_mbedtls_mem.c:165-198`), and steps the handshake inside TCP callbacks. The bridge has one connection, so the altcp layering buys nothing. The direct port (about 300 lines) uses the one DRBG of `src/rng.c` (`mbedtls_ssl_conf_rng(rng_random)`, as the entropy section planned), allocates mbedTLS memory from the FreeRTOS heap, and runs one `mbedtls_ssl_handshake_step()` per net-task pass. Received pbufs wait in a queue, and the TCP window is opened (`tcp_recved`) only once mbedTLS has taken the bytes.

**Handshake pacing [V]:** the handshake needs 1.05-1.06 s of CPU in 19-21 steps. The longest step is 364-367 ms (one P-256 operation). Between steps the net task blocks for one tick, so the lower-priority housekeeping task can kick the watchdog. The housekeeping kick window is timed (500 ms by the clock), so the catch-up after a long step cannot produce an empty window. The radio task has the higher priority and is not held up.

**Handshake steps (#16, 2026-11-16) [V]:** 13 connections (boots and a `docker restart tanoclo-ws`), temporary per-step trace, removed afterwards. Three steps carry the CPU: server certificate (chain check, one ECDSA verify) 364-366 ms, ServerKeyExchange (ECDSA verify of the ECDHE parameters) 364-366 ms, ClientKeyExchange (ECDH key pair and shared secret) 316-324 ms; key derivation 7 ms, client Finished 2 ms, every other step under 1 ms. Total 1055-1064 ms CPU, handshake 1070-1083 ms wall time, `session: ready` 1093-1114 ms after the connect started. No small speed-up is left in mbedTLS: `MBEDTLS_ECP_FIXED_POINT_OPTIM` is already on by default (`vendor/mbedtls/include/mbedtls/ecp.h:296`, ROM table at `library/ecp_curves.c:748`). Splitting the P-256 work into short steps would need `MBEDTLS_ECP_RESTARTABLE`; not needed now **[I]**.

**Immediate ACK (#16) [V]:** lwIP delays an ACK (`vendor/lwip/src/core/tcp_in.c:1631`) until its TCP fast timer, every 250 ms (`include/lwip/priv/tcp_priv.h:117`, `core/tcp.c:1497-1499`). The net task runs no lwIP timers inside a 365 ms P-256 step, and tanoclo's Linux TCP retransmitted after 201-209 ms. Before the fix, packet captures on the bench link (`enp196s0f0`, tcpdump in a `--net host` container, kept out of git) showed 2 retransmissions in every connection (5 of 5 boots): the 730-byte first flight (ACKed only 372 ms after it arrived, when the certificate step ended) and the 73-byte `GET time` reply, after which the bridge sends nothing and its delayed ACK came after the server's timeout. WebSocket pongs were ACKed after 202-203 ms, on the edge of the timeout. `on_recv()` now sets `TF_ACK_NOW`, and `tcp_input()` sends the ACK right after the callback (`tcp_in.c:552-553`) (`src/tls.c`). After: 0 retransmissions in 8 of 8 connections, first flight ACKed 3.1 ms after it arrived, pongs within 1 ms. Cost: one 60-byte frame per server message that an answer would otherwise carry, and 24 bytes of flash. The handshake time is unchanged (1070-1073 ms), since the server was never waiting for the ACK. The shrinking advertised window seen in the captures (2920 down to 1450 in 31-byte steps) is lwIP's silly-window avoidance: it went back to 2920 once 1460 bytes had been read, so it is not a leak.

**Certificate check:** `MBEDTLS_SSL_VERIFY_REQUIRED` against one built-in trust anchor, with `mbedtls_ssl_set_hostname(NET_SERVER_HOST)`. For an address literal, mbedTLS matches the IP subjectAltName (`vendor/mbedtls/library/x509_crt.c:2912-2980`). **Validity dates are not checked:** there is no `MBEDTLS_HAVE_TIME`, because the bridge has no clock until the server sends the time over this very connection **[I]**. The bench certificates run to 2046 (`test-bench.md`).

**WebSocket (RFC 6455):** HTTP/1.1 upgrade with `Sec-WebSocket-Protocol: binary`. The client checks `101`, `Upgrade: websocket` and the `Sec-WebSocket-Accept` value (SHA-1 and Base64 from mbedTLS), and rejects any other subprotocol. Every client frame is masked with 4 fresh bytes from the DRBG. Ping is answered with pong. Fragmented messages are reassembled in place, up to 2 KB. The close handshake waits up to 2 s. **Liveness [I]:** tanoclo never pings (uWebSockets `idleTimeout: 0`, `tanoclo/ws-server/server.js:404-408`). So the client pings after 30 s without a frame and gives up when nothing arrives within 10 s after that. Stock timing is unknown (`open-questions.md` row 14).

**Session.** In order: `POST auth/key`, then `POST auth/token`, then `GET time`. Each is a CON request with a random 4-byte token, matched by message ID and token, with a 10 s timeout. Message IDs start at a random value. The 28-byte frame carries direction 1, the bridge's link-local address, `field_a/b/c` = 0, and port 5683.
- **Identity in requests:** TLV `0x0260` holds the full serial in ASCII, which tanoclo looks up in its `devices` table (`tanoclo/ws-server/lib/handlers.js:118-133,214-232`).
- **Source address [I]:** `fe80::` plus the EUI-64 with the universal/local bit inverted (RFC 4291). tanoclo stores it as the bridge's address (`handlers.js:269-276`). What stock uses is open (`open-questions.md` row 10).
- **Replies [V]:** `auth/key` returns `0x0260` and 16 bytes of `0x0261`. The firmware does not use the `0x0261` value (open question 11). `auth/token` returns an 8-byte token and its lifetime, 1440 min. Requests after auth/token carry the token as option 2048; tanoclo only compares it (`server-api.md`). The lifetime is not acted on, because tanoclo does not expire tokens **[I]**.
- **Time:** taken from the `GET time` reply and from the NON time broadcast (`tanoclo/ws-server/server.js:220-246`).
- **Downlinks:** other downlinks are only logged until P5.

**Reconnect:** a failure or close gives a retry delay of 75-125 % of a backoff that starts at 2 s and doubles up to 60 s. The backoff falls back to 2 s once a session has stayed ready for 60 s. A 4.01 on auth/token (not whitelisted) jumps straight to 60 s. A link or address loss drops the session, and it restarts when the network returns.

**Cloud LED (PB5):** off without a session, 2 Hz while connecting and authenticating, on once ready. Since P2 it had toggled per radio frame (`build.md` P2).

**Identity and trust anchor:** build values from a gitignored `config/local.cmake`, written to a generated header (`build.md`, P4 TLS). Since #21 (2026-11-18) the serial and EUI-64 come from the identity record in external flash instead ("Implementation: identity record (#21)"). The trust anchor stays a build value, as "TLS settings" says.

### RAM estimate (#17) [I]

Estimates made before the prototype; measured figures follow in "RAM measured (#16, #17)".

| Item | KB |
| :--- | --: |
| TLS, peak during handshake | 30 |
| lwIP and packet buffers (measured in P3: about 16.4 KB static, `build.md`) | 12 |
| Task and interrupt stacks | 13 |
| Inbound frame ring (32 × 143 bytes) | 5 |
| CSL job pool, reduced to about 24 | 4 |
| 6LoWPAN reassembly, 3 × 300 bytes | 1 |
| Anchors and neighbours, 16 each | 1 |
| WebSocket and CoAP buffers | 2 |
| FreeRTOS and misc | 3 |
| **Total** | **71 of 128** |

### RAM measured (#16, #17) [V]

2026-11-16, bench bridge on USB, production code plus a temporary probe (removed afterwards) that logged at each phase: heap free and the lowest point since the last phase (sampled after every mbedTLS allocation, the only heap user after the scheduler starts), `uxTaskGetStackHighWaterMark()` of every task, the 4 KB main stack (painted at reset), and lwIP's `MEM_STATS` and `MEMP_STATS`. 6 connections before the immediate-ACK change and 8 after, each set with one `docker restart tanoclo-ws`; the figures were the same in both sets.

**Static RAM** (production image `8e1c455`, `arm-none-eabi-nm -S`): `.data` 136 + `.bss` 56188 = 56324 bytes.

| Item | Bytes |
| :--- | --: |
| FreeRTOS heap (`ucHeap`, `configTOTAL_HEAP_SIZE`) | 32768 |
| lwIP pbuf pool (6 x 1516) | 9199 |
| lwIP heap (`MEM_SIZE`) | 6163 |
| lwIP PCBs, segments, timers, ARP, DNS, DHCP | 1242 |
| WebSocket buffers (receive 2062, send 1280) | 3342 |
| mbedTLS contexts (ssl, config, trust anchor, DRBG) | 1036 |
| RTT buffers and control block | 1112 |
| FreeRTOS kernel lists and state | 269 |
| Other (drivers, counters, newlib, session) | 1106 |
| Main stack (MSP, reserved by `cmake/stm32f411ce.ld`) | 4096 |
| **Reserved in total** | **60420** |

**Heap by phase** (free bytes of 32768; lowest point within the phase):

| Phase | Free | Lowest |
| :--- | --: | --: |
| Boot: 4 task stacks, TCBs, 2 mutexes | 21456 | 21456 |
| DHCP (trust anchor parsed, held: 656) | 20800 | 20192 |
| TLS handshake | 12824 at the end | 9568 |
| WebSocket upgrade and auth | 12824 | 12824 |
| Steady session (2 min, pings) | 12824 | 12824 |
| Closed by the server | 20800 | 12824 |
| Reconnect handshake | 12824 | 9568 |

So a connection holds 7976 bytes, the handshake needs 11232 more than the idle baseline, and every close returns the heap to 20800: no leak. The peak, 23200 bytes used, is the same on every handshake.

**Stacks** (peak use; never-used words from `uxTaskGetStackHighWaterMark`): radio 230 of 512 words (920 B), net 608 of 1536 (2432 B, reached in the handshake), housekeeping 172 of 512 (688 B), idle 11 of 128 (44 B), main stack 720 of 4096 B.

**lwIP pools** (highest use seen in any phase, no allocation errors): `MEM_SIZE` heap 468 of 6144 B (during DHCP), pbuf pool 2 of 6, TCP segments 3 of 8, TCP PCBs 1 of 2, UDP PCBs 1 of 3, pbuf ref/rom 0 of 4. The timeout pool is always 6 of 6: lwIP sizes it to its own fixed set of timers (`MEMP_NUM_SYS_TIMEOUT`), and the firmware adds none.

**Against the 71 KB estimate:**

| Item | Estimate KB | Measured KB |
| :--- | --: | --: |
| TLS, peak during handshake | 30 | 11.0 heap + 1.0 static |
| lwIP and packet buffers | 12 | 16.2 static |
| Task and interrupt stacks | 13 | 14.5 reserved, 4.7 used |
| WebSocket and CoAP buffers | 2 | 3.3 |
| FreeRTOS and misc | 3 | 3.0 (TCBs, mutexes, RTT, counters) |
| Radio side for P5 (frame ring, CSL jobs, 6LoWPAN, anchors) | 11 | not built |
| **Total** | **71** | **about 46 used at the peak (static 23.0 + heap 22.7 + main stack 0.7), 59 reserved** |

**Headroom for P5 [I]:** 131072 - 60420 = 70652 bytes (69 KB) of RAM are not reserved at all, and at the handshake peak 9568 bytes of the FreeRTOS heap are still free. P5's estimated 11 KB of radio buffers fits about six times over. P5 should put its buffers in static RAM (`.bss`), not the heap, so that the heap margin at the handshake stays as measured. If RAM ever gets short, lwIP is the first place to look: its heap peaked at 468 of 6144 bytes and its pbuf pool at 2 of 6 buffers. Do not shrink them before P5 traffic has been measured.

Freedo's job pool of 128 (`freedo/firmware/cc1101_bridge/cc1101_bridge.ino:203`) is too large to carry over. **Decision (user): support 16 devices for now,** matching freedo's 16 anchors and neighbours (`:191,201`).

**P5 static size (#17, 2026-11-17) [V]:** build `88c7894`, `arm-none-eabi-size build/tado-bridge.elf`: text 142640, data 136, bss 79392 (earlier P4 production image: bss 56188). Static RAM is now 79528 bytes, 83624 with the 4 KB main stack, so 47448 bytes (46 KB) are not reserved. The radio side added 23204 bytes of `.bss`, about twice the 11 KB estimate. Largest new symbols (`arm-none-eabi-nm -S`): `relay` 6320 bytes, the two link queues `up_store` and `down_store` 4096 each, `g` 4008. The heap margin is unchanged because P5 uses `.bss`, as planned above. Not yet measured on the board: stack use of the radio task (896 words since the #19 review, 640 before, 512 when the heap was measured above; `radio-runtime.md`, "Radio task stack"). The task stacks come from the FreeRTOS heap, so the lowest free heap, 8976 bytes measured with 640 words (`test-bench.md:160`), becomes about 8976 - 1024 = 7952 bytes **[I]**; to be measured again on the bench. Static RAM is unchanged. The estimate table above is superseded by these figures.

### Flash estimate [I]

About 200-250 KB of 512 KB: mbedTLS 100, lwIP 40, FreeRTOS 10, application 60-100. Stock reaches past 360 KB (`tanoclo/patch_ib_firmware/patch_endpoint.sh:24`).

**Flash, measured [V]:** text 142640 bytes of 512 KB (28 %) at `88c7894` (`arm-none-eabi-size`), below the 200-250 KB estimate. Bootloader and second slot (#20) still have to fit beside it. **Update (#23, 2026-11-18) [V]:** 161256 bytes text, 161392 bytes image at `9c36419`; the layout fits ("Size check (#23)" below).

### To prove on a prototype

1. Full TLS and WebSocket connection to tanoclo within the RAM budget.
2. Enhanced ACK inside the 0-1 ms gap, during a TLS handshake.
3. Radio traffic unaffected while flash is erasing.

A Nucleo-F411 with ENC28J60 and CC1101 modules could test all three without touching the IB01 **[I]**.

## Self-update and first flash (#24)

### Internal flash layout

**Before building (user request, 2026-11-11): first check the required sizes** to confirm this layout is possible: the real bootloader size against its 64 KB, the application image against the 448 KB region, and the image plus header against the 512 KB update slots in external flash. Adjust the layout if anything does not fit. **Done 2026-11-18 (#23): everything fits, the layout is kept** ("Size check (#23)" below).

F411 sectors: 4 × 16 KB, 1 × 64 KB, 3 × 128 KB (`datasheets/rm0383.txt:2023-2031`).

| Address | Sectors | Size | Use |
| :--- | :--- | :--- | :--- |
| `0x08000000` | 0-3 | 64 KB | Bootloader, estimated 6-8 KB without `printf` (size check below), the rest spare |
| `0x08010000` | 4-7 | 448 KB | Application (161392 bytes at `9c36419`, size check below) |

Stock's own layout (`tanoclo/patch_ib_firmware/check_firmware.sh`) does not apply: first flash replaces all of it, and the backup keeps the original.

### Size check (#23, 2026-11-18)

**Result: the planned layout fits with wide headroom; no change is proposed.**

**Application, measured [V]:** HEAD `9c36419` (P6 commits, `src/airtime.h` working-tree edit left out), Release build (`-Os -flto`, `--gc-sections`; `cmake/toolchain-arm-none-eabi.cmake:111,148`), `arm-none-eabi-size`. The image is text + data, and equals the `.bin` size.

| Build | text | data | Image | Share of the 448 KB region |
| :--- | ---: | ---: | ---: | ---: |
| Default, transmitter on, bench `config/local.cmake` (pairing target) | 161256 | 136 | 161392 | 35.2 % |
| `RADIO_TX_ALLOWED=0`, same config | 154724 | 136 | 154860 | 33.8 % |
| Default, no `config/local.cmake` (fresh clone) | 151824 | 136 | 151960 | 33.1 % |
| Debug (`-Og -g`), transmitter on, bench config | 197084 | 136 | 197220 | 43.0 % |

The largest image (default, bench config) leaves 458752 - 161392 = 297360 bytes (290 KB) of the application region free, so it can grow 2.8 times. Growth so far: 131.7 KB at P4 (`prototype-plan.md`, P7), 142640 bytes text at P5 (`88c7894`, "Flash, measured" above), 161256 now. P7 itself adds the Block1 receiver, slot and boot-state writes and the header check, estimated 5-10 KB, and #39 under 1 KB **[I]**, so about 170 KB after P7, under 40 % of the region. A Debug build also fits.

**Bootloader, estimated [I]:** no code yet. Sizes of the pieces it would share with the application, from `arm-none-eabi-nm -S` of the default ELF above **[V]**: vector table 428 B, `reset_handler` 160 B; SHA-256 with `MBEDTLS_SHA256_SMALLER` (`src/tado_mbedtls_config.h:45`) about 0.9 KB (block function 304 B, update 126 B, finish 150 B, the 256 B constant table and init); CRC-32 52 B (bitwise, `store_crc32`); slotted-record load 116 B; MX25R read path under 0.2 KB (`spi1_xfer` 48 B, `mx25r_read` 68 B, `mx25r_read_id` 48 B).

| Part | KB |
| :--- | ---: |
| Vector table and startup (16 MHz HSI, no clock setup) | 0.6 |
| SPI1 polling and MX25R read, page program, sector and 64 KB block erase, WIP poll, wake | 0.8 |
| Internal flash unlock, sector erase, word program, lock | 0.4 |
| SHA-256 | 1.0 |
| CRC-32 and the boot-state slotted record (load and write) | 0.6 |
| Image header check, install copy and read-back verify, rollback, vector table check and jump | 1.5-2.5 |
| IWDG, LED GPIO, RCC clock enables | 0.3 |
| Small `memcpy`/`memset`, fault handlers | 0.3 |
| **Total without logging** | **about 6-8** |
| Optional RTT log of fixed strings and hex numbers, no `printf` | +1 |
| Optional newlib `printf` instead | +5-8 |

Even with `printf` this stays near the 16 KB already expected, which is a quarter of the 64 KB in sectors 0-3, and it fits sector 0 alone. The rest stays spare for growth (for example a recovery path) **[I]**. Not proposed: moving the application down to `0x08004000` would add 48 KB that is not needed and would leave the bootloader no room to grow in place.

**Update staging in external flash [D]/[I]:** the candidate and previous images live in the MX25R1635F, 2 MB (`datasheets/mx25r1635f.txt:205`), in two 512 KB slots at `0x000000` and `0x080000` ("Layout (external flash)"). Both are 64 KB-block aligned, so a slot erases as 8 block erases (`mx25r1635f.txt:219,482-484`). The binding limit is the internal region, not the slot: an image of up to 448 KB plus the header (`magic | version | size | SHA-256`, under 64 bytes, "Image format" below) leaves at least 64 KB of each slot unused. The current image fills 30.8 % of a slot. The stock firmware uses the same two slots: the bench unit's backup has data in `0x000000-0x03DFFF` and `0x080000-0x0BEFFF` ("Stock contents of the external flash" above), so stock images reach about 248 KB **[V]** (4 KB sector map only), against its internal image past 360 KB (`tanoclo/patch_ib_firmware/patch_endpoint.sh:24`).

**Install time and the watchdog [D]/[I]:** at 3.3 V with 32-bit parallelism (PSIZE x32, allowed from 2.7 to 3.6 V, `datasheets/stm32f411ce.txt:5343`; `rm0383.txt:2269-2291`), sector 4 (64 KB) erases in 550 ms typical, 1.1 s max, and each 128 KB sector in 1 s typical, 2 s max; a word program takes 16 us typical, 100 us max (`stm32f411ce.txt:5305,5320-5334`). Erasing sectors 4-7 takes 3.55 s typical, 7.1 s max, and programming 161392 bytes (40348 words) 0.65 s typical, 4.0 s max. The bootloader must refresh the IWDG between sectors, with a timeout above 2 s. Erasing only the sectors the image covers (4 and 5, 192 KB, for today's image) would halve the erase time; that is a detail for the bootloader, not a layout change.

**For the P7 build (not layout changes) [I]:**

- Nothing sets `SCB_VTOR` today: libopencm3's `reset_handler` (`vendor/libopencm3/lib/cm3/vector.c:62-82`) does not, and `src/` does not. An application at `0x08010000` needs the bootloader to set VTOR before the jump, or the application to set it first thing. `0x08010000` is 64 KB aligned, enough for any vector table.
- The application linker script (`cmake/stm32f411ce.ld`) becomes `ORIGIN = 0x08010000, LENGTH = 448K`, with a new one for the bootloader (`0x08000000`, 64K). The linker then fails the build if either outgrows its region.
- The external-flash write guard (`src/flash_layout.h`, `FLASH_WRITE_BEGIN`/`END` = `0x102000-0x106000`) must widen to the update slots and boot state (`0x000000-0x0FFFFF`, `0x106000-0x107FFF`) for the update path, still excluding the identity sectors. **Done 2026-11-19 (#24), as three areas selected one at a time:** "Implementation: self-update, application side (#24)".
- The running image must know its own length to be saved into the previous slot (step 3 of the update flow), for example from a linker symbol at the end of `.data`'s load image.

### Bootloader

- Written once over SWD and never updated over the network.
- Contains only a polling SPI1 flash driver, internal flash programming, SHA-256 and the watchdog. No TLS or Ethernet.
- Runs on the internal 16 MHz oscillator, so does not depend on the crystal setup **[I]**.
- Starts the independent watchdog, which cannot be stopped except by a reset (`datasheets/rm0383.txt:3956`), so a hung application resets. It can be frozen while debugging with `DBG_IWDG_STOP` (`rm0383.txt:23191`). Set the timeout above the longest sector erase, or refresh during erase, as a 128 KB erase takes seconds (`stm32f411ce.txt:5329`).

### Update flow

1. **Receive** the image into the candidate slot, as CoAP Block1 over the existing WebSocket. The block ACK is only sent after the flash write, which gives flow control.
2. **Check** in the application: SHA-256, size and version.
3. **Save** the running application to the previous slot if it is not already there, then write boot state `PENDING`, and reboot.
4. **Install** in the bootloader: verify the candidate SHA-256, erase sectors 4-7, copy, verify, set `TESTING` with attempts at 0, and jump.
5. **Each boot in `TESTING`** adds 1 to attempts before jumping. After 3 failures, the bootloader restores the previous slot and records `ROLLED_BACK`.
6. **Confirm** from the application, once TLS, WebSocket auth and the radio have run for about 60 s. State becomes `CONFIRMED`.

A power cut is safe: the boot state only advances after a verified step, so the bootloader restarts the copy from the external image.

If there is no valid application and no valid previous slot: stay in the bootloader and signal on the LED. Recovery is over SWD.

### Delivery

- Stock downloads its own firmware over the WebSocket (`tanoclo/docs/coap_tlv.md:547`), but the byte format is undocumented and tanoclo has no handler, so this is **our own format**.
- Use a bridge-only path, for example `b/fw`, so it cannot be confused with the devices' `d/fw`. **Built as `b/fw` (#24, 2026-11-19).**
- Use 1024-byte blocks: about 160 blocks for today's 161392-byte image (#23), at most 448 for a full region.
- `POST /api/send-raw` (`tanoclo/docs/websocket.md:249-250`) may let a small uploader run with tanoclo unchanged. Check it can see the ACKs **[I]**. Otherwise add one additive handler to tanoclo. **Checked 2026-11-19 [D]: it cannot see them;** the additive change (an optional `waitAck` on `send-raw`) is in "Uploader and tanoclo (#24, #38)", and `tools/update/upload.py` has a blind fallback for tanoclo as it is.

### Image format

`magic | version | size | SHA-256 | application`.

**Decision (user): images are not signed.** SHA-256 catches corruption only. Anyone controlling the server can install firmware, so the server is fully trusted.

### First flash

All over SWD, powered from the ST-Link only (`hardware.md`). Do not change the option bytes.

1. **Back up** with `read.sh` twice and compare, then run `tools/read_ib01_system_areas.tcl`. Confirm read protection is level 0 (`0xAA`). If not, stop: going from level 1 to level 0 mass-erases the user flash (`datasheets/rm0383.txt:2529-2534`), so no backup could be made first.
2. **Rehearse the restore** by writing the backup back and re-reading it, to prove the path before anything new goes on.
3. **Build** a 512 KB internal image (bootloader + application) and a 2 MB external image (`0xFF` plus the identity record).
4. **Write** with `flash write_image erase` and `verify_image`, as `tanoclo/patch_ib_firmware/program_internal_flash.tcl` does. External flash goes through the RAM stub, `program_external_flash.tcl`.
5. **Read back and compare.** Disconnect the programmer, then power normally.

**Restore:** `read_patch_flash.sh --revert` flashes `original/unmodded.bin` and `original/unmodded_spi.bin` (`tanoclo/patch_ib_firmware/README.md:259,272`), so the dumps must be placed there. Not yet tried.

### Implementation: bootloader and boot state (#24)

Built 2026-11-18, host-tested, reviewed (`fw-reviewer` verdict A, 2026-11-18), **not flashed**: bootloader code, a new storage record and a new application layout need a `fw-reviewer` "safe to flash" verdict first (`CLAUDE.md`; `test-bench.md`, rule 4). Code: `src/fwimg.[ch]` (formats, shared by both images), `src/boot/bootctl.[ch]` (the decisions, hardware independent), `src/boot/bl_hw.[ch]` and `src/boot/main.c` (bootloader hardware), `src/image_desc.[ch]` (the application's descriptor), `cmake/sections.ld`, `cmake/boot.ld`, `cmake/stm32f411ce.ld`; test `tests/host/test_boot.c`. **The application's side** (receive into the candidate slot, save itself into the previous slot, write `PENDING`, confirm), the wider write guard and the uploader: built 2026-11-19, flashed and run on the bench 2026-11-19 [V] ("Implementation: self-update, application side (#24)" below). Before that nothing wrote a boot state, and the bootloader only started the application ("No boot state" below).

**Application image [I].** Linked at `0x08010000`, 448 KB (`cmake/stm32f411ce.ld`). The vector table comes first (428 bytes: 16 + 91 entries, `vendor/libopencm3/include/libopencm3/stm32/f4/nvic.h:108`). At offset `0x200` follows the descriptor, `struct fwimg_desc` (32 bytes): magic `BTD1`, version 1, length, `image_end` (the linker's `_image_end`, the end of `.data`'s initial values), `fw_version` (0 for now) and reserved words. So the application knows its own length (`image_size()`), which it needs to save itself into the previous slot. `image_end` equals the `.bin` size: 152176 bytes, checked on this build **[V]**. The build fails if the vector table reaches `0x200`, the image outgrows its region, or its size is not a multiple of 4 (`cmake/sections.ld`). `cmake/sections.ld` is our own replacement for libopencm3's `cortex-m-generic.ld`, with the same symbols for libopencm3's `reset_handler`. `main()` sets `SCB_VTOR` first thing, and the bootloader also sets it before the jump.

**Valid application** (`fwimg_app_check()`): the descriptor's magic, version and length are right; `image_end` lies inside the region past the descriptor; the initial stack pointer is word aligned, inside RAM or at its top; the reset vector is Thumb and points inside the image, after the descriptor.

**Commit mark [I]:** the descriptor's magic word. Every install programs it last, after everything else is written and checked. The F411 allows a still-erased word to be programmed later without an erase (`datasheets/rm0383.txt:2346-2347`). So an install cut short never passes `fwimg_app_check()`, whatever the boot state says.

**Slot header [I].** The header sits in the slot's first 4 KB sector (`0x000000`, `0x080000`); the image starts at slot + `0x1000`. Layout: magic `TFW1`, version 1, length 64, image size, `fw_version`, load address `0x08010000`, SHA-256 of the image, 8 reserved zero bytes, CRC-32 over bytes 0-59. **Differs** from "Image format" above: it adds length, load address and a CRC, and gets its own sector, so that an uploader can write the image first and the header last. A slot is valid (`fwimg_slot_verify()`) if:
- the header parses;
- the image's first 544 bytes pass `fwimg_app_check()` with the header's size;
- the SHA-256, read back over the whole image, matches the header.

The stock data in both slots of the bench unit ("Stock contents of the external flash") never counts as valid (test). An image's **id** is the first 16 bytes of its SHA-256.

**Boot state record [I].** A slotted record (format in "Slotted records") at `0x106000`/`0x107000`, magic `BOT1`, version 1. Its 36-byte payload is `state u8 | tries u8 | 0 u16 | cand_id[16] | prev_id[16]`. Unknown states count as none.

| State | Meaning | Written by |
| :--- | :--- | :--- |
| none | no record | |
| `CONFIRMED` | the candidate runs and was confirmed | application |
| `PENDING` | install `cand_id`; `prev_id` is saved; `tries` = installs begun | application (sets `tries` 0); bootloader counts |
| `TESTING` | `cand_id` is installed, not yet confirmed; `tries` = starts | bootloader |
| `ROLLBACK` | put `prev_id` back; `tries` = installs begun | bootloader |
| `ROLLED_BACK` | the previous image is back after the candidate failed | bootloader |
| `REJECTED` | a slot was not whole or not the image named at install time: nothing was done | bootloader |
| `NO_ROLLBACK` | rollback given up: the previous slot is not valid, or its install was begun 3 times | bootloader |
| `EXTERNAL` | under test, but internal flash holds another image (written over SWD): no rollback | bootloader |

**Decisions per boot** (`bootctl_run()`, `src/boot/bootctl.c:189`) **[I]:**

| State read | Bootloader |
| :--- | :--- |
| none, `CONFIRMED`, `ROLLED_BACK`, `REJECTED`, `NO_ROLLBACK`, `EXTERNAL` | `start()`: jump if internal flash holds a valid application. If it holds none, install one from a slot, each slot tried once: first the image the state names (the candidate for `CONFIRMED`; the previous image for the others), then the previous slot, then the candidate slot. If nothing can be installed, halt with LED code 5 |
| `PENDING` | Both slots must be whole and hold `cand_id` and `prev_id`, else `REJECTED`. If `tries` is 3 or more: `ROLLBACK` (the update is given up). Otherwise write `tries + 1`, install the candidate, then write `TESTING` with `tries` 0 |
| `TESTING` | If internal flash holds no valid application: `ROLLBACK`. If it holds an image other than `cand_id`: `EXTERNAL`. If `tries` is 3 or more: `ROLLBACK`. Otherwise write `tries + 1` and jump: the start is counted before it happens |
| `ROLLBACK` | If the previous slot is not whole or not `prev_id`, or `tries` is 3 or more: `NO_ROLLBACK`. Otherwise write `tries + 1`, install the previous image, then write `ROLLED_BACK` |

After each state write the loop reads the state again, for at most 16 rounds; the tries bound it to about 8. **If a state write fails, the bootloader stops managing updates for that boot:** it calls `start()` instead of looping. In `TESTING` it still jumps, since the candidate is whole. **Differs** from "Update flow" above:
- the bootloader also checks the previous slot before installing;
- installs are counted;
- a failed test or a given-up rollback ends in `ROLLED_BACK` or `NO_ROLLBACK`;
- `start()` adds a rescue: a valid slot is installed when internal flash holds nothing;
- only the sectors the image covers are erased, not always all of 4-7.

**No boot state (the bench today, and after the first P7 flash) [I]:** the bootloader reads the record (none), checks the application in internal flash and jumps. Nothing is written, in either flash. The stock slots are never valid, so a board without a valid application halts with code 5 instead of installing anything.

**Install** (`install()`, `src/boot/bootctl.c:81`) **[I]:**
1. Erase the sectors the image covers, sector 4 first, so the old commit mark goes first. With PSIZE x32 a 64 KB sector erases in at most 1.1 s and a 128 KB sector in at most 2 s (`datasheets/stm32f411ce.txt:5320-5330`); x32 is allowed at 2.7-3.6 V (`rm0383.txt:2280-2291`), and the board runs at 3.28 V (`test-bench.md`) **[V]**.
2. Copy the image in 256-byte chunks, leaving out the magic word. Each chunk is read back and compared.
3. Compute the SHA-256 of internal flash, counting the magic as it will be, and compare it with the header. This catches a bad read of the slot between its check and the copy, which the chunk compare cannot see.
4. Program the magic.
5. Check the result with `fwimg_app_check()`.

The IWDG is reloaded before every sector erase and every 4 KB. During an erase or program the CPU stalls on its own fetches from flash (`rm0383.txt:2243-2246`), so the gap between reloads is one sector erase, at most 2 s. On the host model a 161 KB update took 12.8 s and a 448 KB image 29 s, both counted at datasheet maxima; the longest IWDG gap was 2.0 s **[I]**.

**Power cuts, failure point by failure point [I]** (each is covered by the host test's cut at every step unless noted):
1. **While the bootloader writes a boot state** (any of the writes above): the slotted record keeps the older generation. The magic is programmed last and the CRC covers the record (`src/store.c`), so the next boot repeats the step from the older state. A cut while `TESTING` `tries + 1` is written happens before the jump, so no start goes uncounted.
2. **During an internal sector erase:** sector 4 goes first, so the commit mark is gone or the sector is noise. Internal flash then holds no valid application. The state is still `PENDING` or `ROLLBACK`, so the next boot installs again (counted).
3. **During the copy or the SHA-256 check:** the commit mark is still erased, so the next boot installs again.
4. **While the magic is programmed:** the word either is not the magic (no valid application) or reads as the magic but is not fully programmed (`rm0383.txt:2240-2242`: flash contents are not guaranteed after a reset during an operation). In both cases the state is still `PENDING` or `ROLLBACK`, so the next boot erases and installs again. A weak magic would matter only at a boot that cannot read the boot state. Not modelled in the test.
5. **After the magic, before `TESTING` is written:** still `PENDING`, so the same image is installed once more.
6. **After `TESTING` is written, during the candidate's start or run:** the start was counted; after 3 starts without `CONFIRMED`, the previous image is put back.
7. **During a rollback:** as points 2-5, with `ROLLBACK`.
8. **During a rescue install (no update state):** internal flash holds no valid application, so the next boot rescues again. Rescues are not counted, because each try needs a boot.
9. **Cut and then the external flash unreadable, or the state unwritable:** `start()` sees no commit mark and installs from a slot, or halts. It never starts a torn image (tested: one such boot after every cut).
10. **IWDG or MCU reset during an external program or erase:** the chip finishes its cycle by itself. `bl_flash_init()` waits up to 480 ms for WIP before reading (as `mx25r_wait_idle()` does).

**Limits [I]:**
- Three installs of the candidate cut short give the update up: `ROLLBACK`, which puts the previous image back.
- Three rollbacks cut short give `NO_ROLLBACK`. If internal flash is then torn, `start()` installs the previous image first, not the candidate that failed its test (random test: 1 of 100 updates ended this way).
- A candidate that cannot confirm because the server is unreachable is rolled back only if it restarts 3 times; a candidate that keeps running is never rolled back.
- If the external flash cannot be written, a `TESTING` boot is not counted: a bad candidate on a failing chip can loop without being rolled back.

**Watchdog [D]/[I].** The bootloader starts the IWDG at /256 with reload 4095, which gives 22.3 s at the LSI's fastest 47 kHz and 61.7 s at its slowest 17 kHz (`stm32f411ce.txt:5028`; `rm0383.txt:23214-23226`, 32.8 s at 32 kHz). That is longer than any bootloader step and than the application's start before its own `watchdog_start()` (2 s delay, then set-up). The bootloader also sets `DBG_IWDG_STOP`, as OpenOCD itself does on attach (`/usr/share/openocd/scripts/target/stm32f4x.cfg:89-91`). **Changes in the application:** the IWDG now already runs when `main()` starts. A panic before `watchdog_start()` therefore resets the board after 22-62 s; before P7 it could not reset (`hardware.md`, "LED roles"). `watchdog_start()` then sets its own /64 and 1500 as before.

**Hard limits of the bootloader** (`src/boot/bl_hw.c`) **[I]:**
- Internal flash: erase only sectors 4-7 (`bl_port_int_erase()`, `:387`); program only inside `0x08010000-0x0807FFFF` (`:403`).
- External flash: Page Program and Sector Erase only inside `0x106000-0x107FFF` (`ext_guard()`, `:286`); no other write command exists in the bootloader.
- Pins: only PA4-PA7, PB1 and PB9 are touched (static asserts against PA13/PA14 and PB6/PB7).
- No interrupts, no low-power modes.
- A guard violation halts with LED code 6 and writes nothing.

The disassembly of the bootloader references FLASH_KEYR, FLASH_SR and FLASH_CR only (offsets `0xC04`, `0xC0C`, `0xC10` from `0x40023000`), never FLASH_OPTKEYR (`0xC08`) or FLASH_OPTCR (`0xC14`) **[V]** (`arm-none-eabi-objdump -d`, this build).

**Power LED in the bootloader:** off while it decides; it flickers on every IWDG reload while it hashes or installs; code 5 (`BL_LED_NO_APP`) means no application and nothing to install; code 6 (`BL_LED_GUARD`) means a refused write (a bug). Both codes repeat every 2.5 s and kick the IWDG, so the board stays in the bootloader until a power cycle or SWD (`hardware.md`, "LED roles").

**Sizes [V]** (`make size`, Release, without `config/local.cmake`; build of `c946452`):

| Build | Bootloader | Application |
| :--- | ---: | ---: |
| Release, transmitter on | 6124 B (text 6120 + data 4), 9.3 % of 64 KB | 152176 B, 33.2 % of 448 KB |
| Release, `RADIO_TX_ALLOWED=0` | 6112 B | 148068 B |
| Debug (`-Og -g`) | 7448 B | 186724 B |

The bootloader fits the 6-8 KB estimate ("Size check (#23)"). The application grows by 216 bytes against `9c36419` (151960 B): the 84-byte gap after the vector table, the descriptor, VTOR and one log line.

**Host tests (`tests/host/test_boot.c`, `make test`) [I].** The real `bootctl.c`, `fwimg.c` and `store.c` run against two flash models:
- the external flash: NOR, a program only clears bits;
- the F411's internal flash: sectors 0-7, x32 word programming.

Both cut the power at any program or erase step: a torn word or prefix, or a sector partly erased. Both can also fail with the power on (fail flag with nothing done, partly done, or a silent miss). A clock charges every operation at its datasheet maximum, which gives the IWDG gap.

Checked on every start: internal flash holds a whole known image, byte for byte; it passes `fwimg_app_check()`; an unconfirmed candidate starts at most 3 times. Checked on every write: the bootloader writes only the boot-state sectors of the external flash and only sectors 4-7 of the internal flash, and never programs a byte twice without an erase.

Cases:
- the formats (every bit flip of a header refused);
- a plain boot with no state and the stock slots: no write;
- rescue from the previous slot; external flash dead;
- an update confirmed; an update rolled back after 3 starts;
- 5 kinds of rejected slots; no rollback possible; an image written over SWD under test;
- internal program and erase failing (bounded, then halt or the old image); the boot state unwritable;
- a bad read during the copy (18 of 18 caught by the SHA-256 before the commit);
- the largest image (448 KB);
- a power cut at 6320 steps of an update and of a rollback: every external program and erase, every internal erase, the first and last word of every chunk, the commit word, and every 97th step. Each cut has 4 tears; after a cut, one boot with the external flash unreadable and one with the state unwritable. Results: 2944 cases ended on the new image (good candidate), 3376 on the old one (bad candidate);
- 100 random updates with random cuts and random good or bad candidates: 28 rolled back, 1 given up after cut installs, 1600 boots, 40 cut.

Result: 752969325 checks, 0 failed.

**Mutations,** each caught: erasing sector 7 first instead of 4; programming the magic with the rest; not counting `TESTING` starts; not checking a slot's id; no `EXTERNAL` check; no SHA-256 of internal flash before the commit; not checking that the previous slot holds `prev_id`.

**To check on the bench:** `fw-reviewer` verdict A (2026-11-18) is recorded: the plan is accepted with changes (`test-bench.md`, "Bootloader (#24): first flash and checks, plan", which lists them and the non-blocking notes).

### Recovery over SWD (#24)

Designed before the bootloader code. Step 1 was rehearsed on the bench on 2026-11-19 (application sector erased, LED code 5 read over SWD, both images written back and booted) **[V]** (`test-bench.md`, "Bootloader (#24): first flash and checks, result"); steps 2-4 have not been tried. Why SWD stays available whatever the bootloader does **[D]/[I]:**
- The bootloader never touches the option bytes: no read protection or write protection change, so the flash stays readable and writable over SWD. It never touches PA13/PA14 and never enters a low-power mode.
- OpenOCD resets the chip with SYSRESETREQ (no NRST on the test points; `target/stm32f4x.cfg:76,81`) and halts it at the reset vector, before the bootloader's first instruction. A system reset puts every register back to its reset value (`rm0383.txt:4545-4546`), and after `reset halt` the IWDG was found not yet running (`build.md`, "Checking the flash against an ELF", 2026-11-16 **[V]**).
- `DBG_IWDG_STOP` survives system resets, since it is reset only by POR (`rm0383.txt:46200`), and OpenOCD sets it on attach. So a halted core is never reset by the IWDG.
- The bootloader's own sectors 0-3 are never erased by the bootloader. Only SWD writes them.

**Procedure (power over USB, ST-Link with GND, SWDIO, SWCLK only, `test-bench.md`):**
1. **Bootloader halts (code 5 or 6), boot-loops, or the application does not start:** `tools/flash.sh`, which writes the bootloader and the application (`program` resets and halts first, writes, verifies), then resets. `tools/flash.sh build/tado-bridge.elf` writes only the application.
2. **The update state itself is the problem** (for example a bad image keeps being installed): erase the two boot-state sectors through the identity tool's SPI stub path (`tools/identity/identity.tcl`, which today refuses any address but the identity slots; a variant for `0x106000`/`0x107000` would be needed), or flash an application that ignores the state. The bootloader with no state just starts what is in internal flash. Not built: only needed if the update path misbehaves.
3. **Back to stock:** `read_patch_flash.sh --revert` with the P0 backups (`tanoclo/patch_ib_firmware/README.md:259,272`), as in "First flash" above. This restores sectors 0-7 and the external flash, boot-state sectors included.
4. **Last resort, if the core cannot be halted at all** (not expected: nothing in the bootloader can stop SWD): the owner pulls BOOT0 high (`hardware.md`: it has a pull-down), so the chip starts from system memory with SWD free, then step 1 (`prototype-plan.md`, "Safety rules").

### Implementation: self-update, application side (#24)

Built 2026-11-19 (`0b62e27`, `07d747f`), host-tested, reviewed by `fw-reviewer` (verdict A with findings M1 and L1-L4), then fixed for M1 and L1 the same day ("Review fixes M1 and L1" below). Delta review verdict A (2026-11-19); **flashed and run on the bench 2026-11-19 [V]** (`test-bench.md`, "Self-update (#24): first over-the-air update, result"): an update A → B installed and confirmed, a session cut and a corrupted transfer marked nothing, and a candidate that never reached the server was rolled back after 3 starts. Code: `src/fwupd.[ch]` (the update logic and the flash operations, hardware independent), `src/fwupd_task.[ch]` (the update task), `src/handoff.[ch]` (one operation handed to the radio task, with a time limit), `src/radio.c` (`radio_update_op()` `:424`, `update_op_run()` `:1852`, the loop `:2161-2181`, `radio_up()` `:1962`), `src/session.c` (`b/fw`: `fw_answer()` `:415`, `fw_poll()` `:443`, `on_fw()` `:463`, the abort `:119`), `src/mx25r.c` (areas, `:79-99`), `src/flash_layout.h`, `src/store_port.c`, `src/coap.[ch]` (Block1), `src/FreeRTOSConfig.h:47` and `src/hooks.c:19` (static allocation); uploader `tools/update/upload.py`; tests `tests/host/test_fwupd.c`, `tests/host/test_handoff.c`, `tests/host/test_mx25r.c` (`test_guard()`, `test_update_ops_traffic()`), `tests/host/test_csl.c` (`test_update_windows_under_traffic()`), `tests/host/test_coap.c` (`test_block1()`), `tests/host/test_upload_tool.sh`.

**Flow [I]** (the six steps of "Update flow" above, the application's part):
1. **Receive.** `POST b/fw` (a bridge-only path, "Delivery"), CON, CoAP Block1 with SZX 6 (1024-byte blocks, RFC 7959). Block 0 is the 64-byte slot header (`src/fwimg.h`, as `fwimg_hdr_build()` writes it) followed by zeros; block n ≥ 1 is the image at (n - 1) × 1024. So the header reaches the bridge first but is written last. Each block is answered only once it is programmed and read back: 2.31 Continue with the Block1 echo, no payload (RFC 7959 2.9.1), and 2.04 Changed for the last one. This is the flow control. Refusals carry a short diagnostic text: 4.00 malformed, 4.03 refused, 4.08 not the block expected, 4.13 over 448 KB, 5.00 a flash or check failure, 5.03 a block still being written.
2. **Check, before anything is written** (block 0, `start()`, `src/fwupd.c:316`): the header parses (magic, CRC-32, load address, size up to 448 KB and a multiple of 4); the server session is ready; a network record is in use (the radio task reports `network_active()` with every operation); the boot state is not `TESTING`, `PENDING` or `ROLLBACK`; the image is not the running one (its id, the first 16 bytes of its SHA-256). Block 1's first 544 bytes must pass `fwimg_app_check()` with the header's size, or the transfer is dropped. Every block must have the length and the M flag that the header's size gives.
3. **Save** the running image into the previous slot (`save_running()`, `:225`), unless that slot already holds it whole: its header names the running image's SHA-256 and the slot hashes to it. Then: erase the header sector, erase and program the image sector by sector from internal flash (`FWIMG_APP_BASE`, `image_size()` bytes), hash it back, and program the header last. Then the candidate slot's header sector is erased, so the candidate slot holds no valid image until the end. The bootloader's `PENDING` check needs both slots whole (`src/boot/bootctl.c:204-219`).
4. **Last block** (`finish()`, `:384`): the SHA-256 of the whole candidate, read back from the slot, must match the header; then the candidate's header is programmed and read back; the previous slot is verified again; then the boot state `PENDING` (tries 0, `cand_id`, `prev_id`) is written and loaded back (`fwimg_boot_write()`). If that write reports a failure, the boot state is loaded once more (`pending_landed()`, `:373`): `PENDING` with exactly these ids means the record landed and only its read-back failed, so the update counts as written (review finding L1). Anything else is 5.00 with nothing marked. Only then 2.04. 3 s later (`FWUPD_RESTART_MS`, time for the answer to leave) the update task asks the radio task to restart (`FWUPD_OP_RESTART`, at a quiet moment, SYSRESETREQ), and the bootloader installs.
5. **Confirm** (`fwupd_tick()`, `:478`, once a second): at boot the update task hashes the running image and reads the boot state. If that read fails (the flash did not answer, or the radio task did not take the operation in 10 s), it is tried again every 10 s (`FWUPD_RETRY_MS`), and `GET b/fw` shows `boot unread` meanwhile. In `TESTING`, with the running image as `cand_id`, healthy seconds add up: the session is ready (`session_ready()`) and the radio is up (`radio_up()`: running, interrupts on, PLL calibrated). Any unhealthy second starts the count again. At `FWUPD_CONFIRM_MS` (60 s) the boot state becomes `CONFIRMED` with the same ids, and a failed write is tried again after 10 s. "No panic" is implied: a panic resets the board, and the bootloader counts the start.
6. **Not confirmed in time.** Under test, without a confirmation for `FWUPD_TEST_MAX_MS` (10 min) since the boot, the application restarts itself. The same applies when the boot state was never read in those 10 min, since the image may be a candidate under test. The bootloader counts each start, so a candidate that runs but never gets healthy is rolled back after 3 starts, about 31 min. Examples: a server it cannot reach, or a radio that does not come up. In the second case the radio task stays in its bring-up retries and never takes an operation, so the boot state is not read and the restart comes from the update task itself ("Review fixes M1 and L1" below). An image that is not under test also restarts every 10 min while its boot state cannot be read. The bootloader writes nothing for such a boot (`src/boot/bootctl.c:257-258`, the default case), so this costs a boot and no flash wear. **Differs** from "Limits" above ("a candidate that keeps running is never rolled back"). Without this, such a candidate would also refuse every new update, since none is taken in `TESTING`. Its cost: a good image whose server is down for 30 min after the update is rolled back. **Decided (owner, 2026-11-20): kept**, at the cost stated above; without it a candidate that runs but never gets healthy would run for ever and refuse every new update (`docs/README.md`, "Project decisions"; `open-questions.md`, row 20).

**Where it runs, and the radio deadlines [I].** Two halves, two tasks:
- The **update task** (`src/fwupd_task.c:192`, priority `tskIDLE_PRIORITY + 1` like housekeeping, below net and radio) runs all of the logic: the hashing (SHA-256 over up to 448 KB), the checks and the bookkeeping. It takes a block from the net task, decides, and turns the work into **flash operations**, one at a time.
- An **operation** (`struct fwupd_op`) is one of: read up to 1 KB; erase one 4 KB sector and check it blank; program up to one 256-byte page and read it back; load or write the boot state; restart. The update task hands it to the radio task (`radio_update_op()`, `NOTE_UPDATE`, `src/handoff.c`) and waits. If the radio task has not taken it after 10 s, the update task withdraws it and it never runs ("Review fixes M1 and L1" below).
- The **radio task**, which owns SPI1, runs it in its loop as flash work, under exactly the store's rules (#22, "Flash work and the coordinator" above). While an operation waits, `csl_hold()` is on. It starts only at `flash_quiet()`: no frame arriving, the coordinator idle. During it the driver serves the receiver between status polls and never starts a bus transaction on a busy radio (`src/mx25r.h`).
- So the **Enhanced ACK path is untouched**: the ACK interrupt and `ack_consider()` run as before, also inside the operation. The pairing beacon's gate, `csl_quiet_for()`, is true while the coordinator is idle.
- Nothing CPU-heavy runs in the radio task: the operation's own work is one 64-byte compare per driver transaction.
- **No block erase is used** (the driver has no 64 KB erase command, and none was added): a slot is erased as 4 KB sectors, one per operation, so no operation holds the coordinator longer than one sector erase.

**Bounds of one operation [I]** (host model of the real driver under random traffic, `test_update_ops_traffic()`: cycle times drawn up to the datasheet maxima, tPP 10 ms, tSE 240 ms, `mx25r1635f.txt:3337-3341`): erase with its blank check ≤ 249 ms; one page (4 programs and the read-back) ≤ 72 ms; a 1 KB read ≤ 22 ms; a boot-state write ≤ 209 ms. In that run: 802 operations (the save of a 152 KB image, its hash, its header, a boot-state write), 738 frames acknowledged during them, every ACK prepared at most 999 us after the header was in, no transaction on a busy radio, no CRC-OK edge inside a transaction, nothing outside the slots and the boot state changed.

**The coordinator between operations [I].** The update task posts its next operation as soon as one ends, but at a lower priority than the radio task. So the post lands only once the radio task blocks, after its next `csl_step()`, which runs with the hold off: the coordinator gets one transaction between two operations. `test_update_windows_under_traffic()` models the worst case: every operation a 250 ms erase, two devices with a queue that never empties, and a frame arriving on the first pass after every transaction. In 60 s, 117 operations and 116 downlinks, all delivered at attempt 1, 114 of them with a short train of 19-23 wake-up frames (the other two were the bootstrap trains). Without the hold, operations wait for gaps that never come (checked by the same test). Cost: under a full queue each downlink waits at most one operation (about 0.25 s), and the update moves at one operation per transaction. Normally the queue is empty and operations follow each other directly.

**Speed [I], datasheet typical values:** for a 161376-byte image (the build without `config/local.cmake`; the bench image is about 170 KB), block 0's save is 41 sector erases (the header sector and 40 for the image, 58 ms each) and 631 page operations (4 programs of 64 bytes each), about 2.3 s + 2-3 s, plus the hash-back. Each later block is 4 page operations, with an erase every fourth block. The final check hashes both slots back in 1 KB reads, 316 reads. At datasheet maxima, a 448 KB save is 113 erases × 0.25 s and 1792 pages × 72 ms, about 2.6 min: the uploader waits up to 180 s for block 0's answer (`tools/update/upload.py`). To be measured on the bench. **Measured 2026-11-19 [V]** (171268-byte bench image, RTT `(<n> ms)` figures): block 0's save 9217 and 9890 ms, 720-725 ms when the previous slot already held the image; 32 ms per later block; the last block (SHA-256 of both slots, header, `PENDING`) 1356-1358 ms; restart to the new image's first RTT line 12.4 s (install plus the 2 s start delay), 10.4 s for the rollback install (`test-bench.md`, "Self-update (#24): first over-the-air update, result").

**Write guard [I]** (`src/flash_layout.h`, `src/mx25r.c:79-99`), widened as planned in "For the P7 build" above, but per area rather than as one range:

| Area | Range | Selected by |
| :--- | :--- | :--- |
| Records (default) | `0x102000-0x105FFF`: network record, counter log | the store, always (`src/store.c` never selects another) |
| Boot state | `0x106000-0x107FFF` | `fwupd_op_run()` for `FWUPD_OP_BOOT_WRITE` only |
| Slots | `0x000000-0x0FFFFF`: candidate and previous image | `fwupd_op_run()` for `FWUPD_OP_ERASE`/`PROGRAM` only |

- The driver panics on an erase or program outside the area selected at that moment (`mx25r_area()`). `fwupd_op_run()` selects the area for one operation and the records again afterwards (`fwupd_port_area()`, `src/store_port.c`).
- The identity sectors (`0x100000-0x101FFF`) and `0x108000-0x1FFFFF` are in no area.
- So the update path cannot write the network record or the counter log, and the store cannot write an image or the boot state. Erasing the counter log while a network record exists would let a reboot reuse frame counters under the network key ("Factory reset"), hence one area at a time and not one wide range. **Differs** from the plan's single range.
- Above the driver, `fwupd_op_allowed()` (`src/fwupd.c:34`) refuses an operation before any bus access: an address range not inside one slot, a program crossing a page, a boot-state write other than `PENDING` or `CONFIRMED` with tries 0.
- `_Static_assert`s in `src/flash_layout.h` keep the areas apart and the identity outside them.

**Power cuts, failure point by failure point [I]** (each covered by `test_power_cuts()`: a cut at every program and erase step of an update and of its confirmation, 256 steps; then one boot, and from there the upload again and the confirmation, which must end on the new image):
1. **During the save into the previous slot:** that slot has no valid header (erased first, programmed last). The boot state is unchanged, internal flash is untouched, so the old image starts. The next update saves again.
2. **During the candidate's header erase or its image:** the candidate slot has no header, the boot state is unchanged, and the old image starts.
3. **During the candidate's header program:** a torn header fails its CRC-32, and the old image starts.
4. **During the `PENDING` write:** the slotted record keeps the older generation, so the old image starts. Or the new one is whole (the cut came after its commit mark), and the bootloader installs, which is correct, since both slots were verified first. In the test, 4 of 256 cuts ended this way.
5. **After `PENDING`, before or during the requested restart:** the next boot installs, as after the restart.
6. **During the `CONFIRMED` write:** still `TESTING`. The boot counts as a start, and the image confirms again after 60 s healthy. If the cut came after the commit mark, it is `CONFIRMED`.
7. **During a restart for an unconfirmed candidate:** that is just the next counted start. When the update task restarts the MCU itself (the radio task did not take the request in 10 s), no flash write should be under way. All of the radio task's flash writes run in its loop, at the same quiet moment as the update's operations and just before them, and each is bounded at about 1 s. If one were cut anyway, the records are power-cut safe (`test_store.c`) **[I]** ("Review fixes M1 and L1" below).
8. **`PENDING` landed, its read-back failed (L1):** the reload finds it, the update counts as done, and the restart follows. If the reload fails too, it is 5.00, and the next boot installs anyway, as in case 4. Covered by `test_pending_readback()`.
9. **The running image is never touched:** the application never writes internal flash (the model counts any attempt: 0), and the bootloader installs only from a `PENDING` written after both slots were verified.

All 256 cases ended confirmed on the new image. The first boot after the cut started the old image 252 times and the new one 4 times, never a torn or unknown image. The identity, records and boot-state areas outside the update's writes never changed.

**Corruption and refusals [I]** (`test_fwupd.c`):
- A bit flipped on the bus while the candidate is hashed, a program that silently leaves a byte erased, and a header whose SHA-256 is not the image's: 36 of 36 cases fail before anything is marked. The boot state stays, the candidate slot is invalid, and the next boot starts the old image; a clean upload then works.
- A previous slot damaged in flash is saved again (the skip needs a whole hash), and one damaged between block 0 and the last block fails the update before `PENDING` (`test_prev_damaged()`).
- Refusals: no session, no network, the running image, every boot state under way, 448 KB + 4, a flipped header bit at 74 positions, padding not zero, block 0 short or with M = 0, block 1 not an application of the header's size, out of order (the transfer stays), wrong length or M, a block 2 back, nothing after `DONE` but the last block's answer again, 120 s without a block, the session ending (`fwupd_task_abort()`).
- The largest image (448 KB): saved, received, installed.
- The full cycle: A → B confirmed; B → C never healthy, 3 self-restarts, rolled back to B; C again with B already saved (no erase in the previous slot).
- Mutations, each caught: no final SHA-256 compare (79 failed checks), no same-image check, erase outside the slots, confirm at once, no save, updates allowed in `TESTING`, no network check, no recheck of the previous slot, a save skipped on its header alone, no 10-minute restart, the header written before the hash. Not caught: removing a program's own read-back, which the final SHA-256 covers.
- Boot state unread (M1, `test_boot_state_unread()`) and `PENDING` read-back (L1, `test_pending_readback()`): see "Review fixes M1 and L1" below.

**Review fixes M1 and L1 [I]** (2026-11-19, after `fw-reviewer` verdict A on `0b62e27`/`07d747f`):

**M1, the update task could hang for ever.** Before the fix, `radio_update_op()` waited with no limit. If `radio_configure()` kept failing, the radio task stayed in its bring-up retries (`src/radio.c:2057-2064`) and never ran its loop, so the boot-state read in `fwupd_init()` was never taken. The update task then hung: it never confirmed, never restarted, and every `POST b/fw` got 5.03. The IWDG never fired, since the radio task checks in during those retries. Fixed in three parts:
- **A time limit on the hand-over** (`src/handoff.[ch]`, used by `radio_update_op()`). The operation sits in one slot: empty, posted (its address) or claimed (its address with bit 0 set). The radio task claims it by compare-and-swap. After `UPDATE_OP_WAIT_MS` (10 s) the update task tries to withdraw it by compare-and-swap. Exactly one of the two wins: an operation runs once, or it is withdrawn and never runs. A claimed operation is always waited for to its end, since it lives on the update task's stack and is bounded (one sector erase or one boot-state write, "Bounds of one operation" above). The 10 s are well above the longest normal wait, which is the transaction in flight (at most about 2.5 s: a 1 s wait for the sample, a 1 s train, the ACK window) plus one store write. With the radio task above the update task in priority, the update task cannot even run between the radio task's load and its swap; the compare-and-swap makes the slot safe without relying on that.
- **The boot state read again** (`fwupd_tick()`): unread at `fwupd_init()`, it is tried every 10 s. The first read that succeeds (a retry, or block 0's own read) sets `testing` as at boot. Event `FWUPD_EV_BOOT_READ` logs `update: boot state read late: ...`.
- **The restart without the radio task loop** (`restart()`, `src/fwupd_task.c:183`). Still unread 10 min after the boot, `fwupd_tick()` returns `FWUPD_EV_RESTART`. The update task asks the radio task to restart, as before. If the request is not taken in 10 s, it logs `update: the radio task did not take the restart in 10 s: restarting from the update task`, waits 200 ms for RTT, and calls the same `scb_reset_system()` (SYSRESETREQ). The IWDG stays satisfied throughout: the radio task checks in during its retries, and the update task blocks no other task.

What happens in each case:

| Case | Before | After |
| :--- | :--- | :--- |
| Radio never comes up, candidate under test | update task hangs; never rolled back | restart at 10 min (plus up to 20 s: the last read and the restart's 10 s); rolled back after 3 starts |
| Radio never comes up, confirmed image | update task hangs | restart every 10 min; the bootloader writes nothing for it |
| Flash not ready at boot | read fails at once; no restart | restart every 10 min (nothing works without the flash: no key) |
| Radio comes up late | update task waits, then goes on | read retried every 10 s; confirmation from the first read on |
| `POST b/fw` while the radio is down | 5.03 for ever | 5.03 while a read waits, then 5.00 `boot state not read (flash not ready or radio task not running)` after 10 s |

**L1, `PENDING` landed but reported as failed.** See flow step 4 and power-cut case 8 above.

**Unchanged:** the Enhanced ACK and beacon paths. In the bench-config Release build, every receive and transmit function is identical instruction by instruction apart from addresses, literal-pool words and `__LINE__` constants. That covers `exti1_isr`, `gdo_edge_from_isr`, `rx_service`, `frame_done`, `mac_ack_for_data`, `send_frame`, `tx_begin` and the coordinator's functions. `radio_task` (the loop, with `update_op_run()` inlined) gains 9 instructions, the claim and the pending check; its prologue frame stays 304 bytes. The bootloader binary is byte for byte the same (SHA-256 `eb2f44fb…`).

**Tests** (`make test`, all pass):
- `tests/host/test_handoff.c` (84942 checks): never claimed, withdrawn at exactly 10 s and never run; claimed and waited for past the limit; every claim time from 9980 to 10020 ms; 5000 random runs; a second post refused.
- `test_fwupd.c`, `test_boot_state_unread()`: a candidate whose boot state is never read restarts at 600-601 s, three times, and is rolled back; the read retried every 10 s (60 tries); a read that comes through after 25 s confirms as usual; a confirmed image read late never restarts; a confirmed image never read restarts at 10 min with no bootloader write; block 0 gives 5.00 while unread, then finds the candidate under test.
- `test_pending_readback()`: the record whole but its read-back failing gives `DONE` and the restart; the reload failing too gives 5.00, and the next boot still installs; a record that did not land gives 5.00, also over an older `CONFIRMED`.
- Mutations, each caught: no restart while unread, no retry, unread treated as not under test, no reload after `PENDING`, a reload that ignores the ids, block 0's read not counted, no withdraw (the old wait: the model clock's one-hour guard), withdraw of a claimed operation, the slot not emptied before `done`. Not caught: a claim without compare-and-swap. The host model runs the two sides in turn, so it cannot interleave them inside the claim, and the task priorities prevent that interleaving on the bridge too (above).

**Sizes [V]** (`arm-none-eabi-size`, Release; `.bin` = text + data):

| Build | `07d747f` (reviewed) | With the fixes | Bootloader |
| :--- | ---: | ---: | ---: |
| No `config/local.cmake` (no CA, no server) | 161376 B | 162272 B (+896; bss 85208 → 85220) | 6124 B, `eb2f44fb…`, unchanged |
| Bench `config/local.cmake` (the bench image A) | 170392 B | 171268 B (+876; bss 85732 → 85744) | the same |

The 161376 B quoted elsewhere in this section is the build without `config/local.cmake`. The bench image is about 9 KB larger (the CA and the server settings).

**Stack after LTO [V]/[I]:**
- **Radio task:** prologue frame 304 bytes, the same as in `07d747f` **[V]** (`objdump`). The review measured the radio stack of `0b62e27` at +8 bytes over `64433bd` after LTO **[D]**. The 24 bytes in "RAM and stack" below come from `-fstack-usage` without LTO.
- **Update task:** the worst static call path from its entry is 940 bytes, against 1312 in `07d747f` **[V]**. It follows direct calls only, from prologue frames, and ends in `LOG()` inside `fw_panic()`. The task's own frame shrank from 392 to 12 bytes because LTO no longer inlines the update logic into it. So expect at least as many unused words in the `alive:` line as before (about 250) **[I]**.

**Data hand-over [I]:**
- The net task copies a block into the update task's 1 KB buffer only while the update task is idle (`fwupd_task_submit()`, `src/fwupd_task.c:84`), and answers the request when the result is there (`fw_poll()`).
- A request resent with the same Message ID while its block is being written gets no second answer (the first one comes when done). Another request then gets 5.03.
- The session leaving `ready` aborts the transfer (`set_phase()`, `src/session.c:119`). Nothing is marked, and the uploader starts again at block 0, where a previous slot that already holds the running image is not written again.
- `GET b/fw` answers a status line: `run <id> boot <state> tries <n> cand <id> prev <id> xfer <idle|n/N|done> healthy <s>` (`fwupd_status_text()`), with state `unread` until the boot state has been read.

**RAM and stack [V]/[I]** (`arm-none-eabi-size`, `-nm`, this build) **[V]**:
- `.bss` grows 80024 → 85208 bytes (+5184). The update task's stack (2048) and TCB (80, static); the idle task's stack (512) and TCB (80), now static instead of from the heap; the block buffer (1024) and the read buffer (1024); the SHA-256 context (104); the update state (160); the session's request (28) and result text (96).
- The heap no longer holds the idle task, so its free bytes should rise by about 0.6 KB **[I]**. With `xTaskCreate` it would have lost 2.1 KB at the handshake peak (7960 bytes lowest, `test-bench.md`, "Bootloader (#24) ... result"), hence `xTaskCreateStatic`.

Stack, by `-fstack-usage` per function (without LTO) **[I]**:
- **Radio task:** the deepest new path is a boot-state write with a frame served inside its erase. `radio_task_run` 136, `fwupd_op_run` 16, `fwimg_boot_write` 96, `store_slot_write` 136, `erase_verified` 80, `mx25r_erase_sector` 32, `wait_cycle`/`wait_wip` 32, `mx25r_port_radio_serve` 48, `rx_service` 208, then the #22 receive path (link, relay, 6LoWPAN, CCM, `log_printf()`, about 1 KB). In all, 24 bytes above the #22 path's 2.1 KB, against 3.5 KB.
- **Update task:** `task` 264 + `fwupd_block` 256 + `slot_valid` 104 + `slot_hash` 40 + `mbedtls_sha256_update` 24 + `mbedtls_internal_sha256_process` 352, about 1.05 KB. Or `task` + `LOG()` (about 1 KB with newlib's formatter), about 1.3 KB. Plus up to 104 B of exception frame, against 2 KB.
- **Net task:** `on_message` 304 + `fw_answer` 320 (with its 256-byte message buffer) + `send_coap` + `ws_send`, about 1 KB under `session_poll`, well inside its 6 KB.

The `alive:` line now prints the update task's unused words too.

**Sizes [V]** (`make size`, Release, no `config/local.cmake`):

| Build | Application | Against `64433bd` | Bootloader |
| :--- | ---: | ---: | ---: |
| Release, transmitter on | text 161240 + data 136 = 161376 B, 35.2 % of 448 KB; bss 85208 | +8784 text, +5184 bss | 6124 B, unchanged |
| `RADIO_TX_ALLOWED=0` | text 157704 + 136; bss 85204 | | 6124 B |
| Debug (`-Og -g`) | text 197412 + 136; bss 85236 | | 7448 B |

The 5-10 KB estimate of "Size check (#23)" holds.

**To check on the bench** (`test-bench.md`, "Self-update (#24): first over-the-air update, plan"): an update installs and confirms; the time per block and for block 0's save; ACK timing during an update; the stack and heap lines; an interrupted update (Ethernet cut); a bad image refused; a candidate that never confirms, rolled back.

### Uploader and tanoclo (#24, #38)

**What tanoclo offers today [D]:**
- `POST /api/send-raw` takes `{deviceId, wsHex}` and puts that WebSocket frame on the bridge's connection (`tanoclo/ws-server/lib/command-api.js:517-537`; the API child process passes it to the WebSocket server as `SEND_TO_DEVICE`, `ws-server/api/server.js:641-645`, `ws-server/server.js:642-647`).
- It answers `{ok: true}` at once. It registers no Message ID, so the bridge's ACK reaches `handleAckReceived()` (`lib/message-router/index.js:542-566`, `api/server.js:715-719`) with nobody waiting for it (`lib/coap-transport.js:237-300`).
- No other route sends arbitrary options and waits for the answer. `memory-dumper.js:141` waits with `waitForAck()`, but for its own `d/dbg/m` requests only.
- **Conclusion: with tanoclo unchanged, an uploader can send blocks but cannot see the bridge's answers [D].**
- The command API also answers only requests from localhost (`api/server.js:320-345`). From the bench host, `10.42.0.1:3111` gives 403, and inside the container it answers. Checked read-only with `GET /api/clients` on 2026-11-19 **[V]**.

**The tanoclo change for #38 (additive, about 15 lines) [I].** In `handleSendRaw()` (`lib/command-api.js:517`), accept two optional fields, `waitAck` (bool) and `timeoutMs` (default 35000, at most 300000). With `waitAck`:
1. Read the CoAP Message ID from the frame (bytes 30-31: the 28-byte frame header plus 2).
2. Register `waitForAck(mid, timeoutMs)` (`lib/coap-transport.js:328`) before `_sendFn()`.
3. Send, await the ACK, and answer `{ok: true, ack: {code, mid, payloadHex}}` from the object `handleAckReceived()` resolves (`code`, `hex`).
4. On a timeout answer 504 `{error: "Timeout waiting for device ACK"}`.

Without `waitAck`, nothing changes. Nothing else is needed:
- the bridge's ACK already reaches the API process (`ACK_RECEIVED`);
- an ACK returns before tanoclo's own Block1 reassembly (`lib/message-router/index.js:542-573`);
- `b/fw` is not one of the firmware paths tanoclo blocks, and those rules apply to proxied traffic only (`lib/proxy-manager.js:274-307`).

The uploader uses Message IDs 0x0100-0x6FFF, below tanoclo's own 0x7000-0xFFFF (`getNextMid()`, `lib/coap-transport.js:73-77`), so an answer of ours never cancels one of tanoclo's retries.

**`tools/update/upload.py` [I]** (Python standard library only):
- `info IMAGE` checks an image as `fwimg_app_check()` does (descriptor, vectors, size) and prints its id and header.
- `upload IMAGE [--bridge SERIAL] [--watch]` finds the bridge in `GET /api/clients` (its `ipv6` is the address the bridge's frames carry) and sends block after block through `send-raw` with `waitAck`. It resends the same message on a timeout and waits on 5.03. It stops at any other answer with its diagnostic, then waits for the bridge to reconnect.
- `--watch` follows `GET b/fw` until the new image is `CONFIRMED`, or reports a rollback.
- `status` sends one `GET b/fw`.
- `--docker tanoclo-ws` runs every API request inside the container (one `docker exec ... node` relay for the whole run), as the localhost rule needs.
- Without #38 (the PR is postponed to the very end, owner decision 2026-11-20, so `--blind` is the normal mode until then), `--blind` sends at a fixed pace (120 s after block 0, 1.5 s per block) and sees only the reconnect. A lost or early block makes the bridge refuse the rest, and nothing is marked, so it is safe but slow and blind; it is the working mode until #38 lands.
- Host test `tests/host/test_upload_tool.sh`: the tool's frames for four image sizes are taken apart as `src/session.c` does and fed to `src/fwupd.c` (DONE, candidate verified, `PENDING`, installed by the real `bootctl.c`); the body is the header block and the image byte for byte; odd, oversized and non-application images are refused. 39 checks. Run on the real build: `info build/tado-bridge.bin` gives 161376 bytes, 158 image blocks, and the bootloader's `.bin` is refused **[V]**.
