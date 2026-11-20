# P8 clean-up: survey and progress list (#32)

Survey of the whole repository for the P8 code clean-up checkpoint (`prototype-plan.md`, "P8: Code clean-up checkpoint"; issues #32, #33, #34). Behaviour-preserving only. This file is the resumable progress list: tick a row when its commit lands, and put the commit hash in the row. The same checklist is mirrored in a comment on #32.

Every finding below is our own reading of the code, so every claim is **[I]** unless marked otherwise. Line numbers are as of `6462644`.

## Scope and method

- **Surveyed at:** `6462644` (main), 2026-11-20. `src/airtime.h` carries an uncommitted local bench edit (the airtime budget lifted for tests); it is out of scope and must not be touched or committed by any P8 step.
- **Read in full:** `src/` main, app, board, hooks, syscalls, faults, log, rtt, timebase, watchdog, msgq, image_desc, identity, handoff, airtime, button, pair_led, clock, entropy, pa3_adc, rng, spi1, lwip_port, store_port, arch/cc.h, FreeRTOSConfig.h, lwipopts.h, tado_mbedtls_config.h, flash_layout.h, fwimg, fwupd, fwupd_task, mx25r, store, network, txgate, ccm, cc110l, radio.h; `tests/host/Makefile`.
- **Read in part:** `src/radio.c` (header, register table and frequency checks, the structure of every section), `src/net.c` (LEDs, fault code, MAC choice, bring-up), `src/session.c` (LEDs, identity fault code).
- **Skimmed** (file header, function list and lengths, and the automated checks below): mac, link, lowpan, csl, pair, relay, coap, session, ws, tls, enc28j60, net, `src/boot/*`, every host test, `tools/`, `cmake/`, `Makefile`. Each of these gets a detailed read at the start of its #33 commit; findings then go into this file.
- **Automated checks over all of `src/`:** functions named in comments as `name()` that exist nowhere; constants named in comments that are defined nowhere; stale stage references (P2, P4, "provisional"); duplicated helpers (byte order, hex, wipe); functions over 70 lines.

## Baseline for "the build and size report match" [V]

Built from a clean export of `6462644` with the owner's `config/local.cmake` (not printed), outside the working tree, on 2026-11-20:

| Image | text | data | bss | `.bin` SHA-256 |
| :--- | ---: | ---: | ---: | :--- |
| `tado-bridge.elf` | 171848 | 136 | 85756 | `d5f66a4788e19604ac9e8c307983cf761cc39709b1d140d26948159242939382` |
| `tado-bridge-boot.elf` | 6120 | 4 | 1148 | `eb2f44fb2e0bce1be81b9d887a04da6da8174021f05479d10204d72bb9fa5290` |

`make test`: all host tests passed, including `test_identity_tool` and `test_upload_tool`.

## Rules for every #33 commit

1. One file or module per commit, build and `make test` passing, `make format` clean (`.clang-format`, clang-format 23.1.4). Commit with `Refs #33` and tick the row here and in #32.
2. **Size:** text, data and bss must match the baseline unless the row says otherwise. A comment-only commit can still change the application `.bin`: 25 `fw_panic()`/`configASSERT` sites in the application pass `__LINE__`, so moved lines change immediates (count by `grep -c __LINE__ src/*.c`). Compare `arm-none-eabi-objdump -d` before and after; the only differences allowed are those line numbers.
3. **Bootloader:** `src/boot/*`, `src/fwimg.c`, `src/store.c` and the headers they include are linked into the bootloader, which is written once over SWD and never updated over the network (`src/boot/main.c:1-5`). None of them uses `__LINE__`, so for every commit that touches them the bootloader `.bin` SHA-256 must stay exactly as in the baseline. If it changes, the step is not a #33 step: it goes to #34.
4. **Reviewer:** since 2026-11-17 every default image counts as radio-transmit code (`CLAUDE.md`, "Bench permission"), so no P8 image goes onto the bench bridge without a `fw-reviewer` "safe to flash" verdict, whatever the commit touched. Plan one review and one flash per batch of commits, not one per commit. Rows marked **R** touch radio timing, the transmit gate or the storage format and need that review even before they are merged.
5. **Bench check after a flash:** the existing checks in `test-bench.md` (boot log, session ready, Enhanced ACK gap with the SDR on the test channel 16 only). Nothing is transmitted on channel 26.
6. **Keep the layout of `struct ctr` in `src/store.c:431-437`:** `tools/bench/flash22_force.py:6,287` writes `ctr.next` at offset 16 over SWD.

## Ordered commit list for #33

Expected size effect: "0" means text, data and bss equal to the baseline, with the bootloader hash unchanged where rule 3 applies.

| # | Status | Files | What | Size | Gate |
| ---: | :--- | :--- | :--- | :--- | :--- |
| 1 | done `eacc70e`, `23f8dd7` | `src/FreeRTOSConfig.h`, `src/lwipopts.h`, `src/rng.h` | Stale stage notes: heap "PROVISIONAL ... revisit it in P4" (`FreeRTOSConfig.h:38-40`), "GDO0/GDO2 in P2" (`:55`), "from P4, TLS" (`lwipopts.h:9`), "TCP: provisional, to be sized with TLS in P4" (`lwipopts.h:71`), "by P4 for TLS" (`rng.h:10`). Replace with the measured state (`design.md`, "RAM measured (#16, #17)"). | 0 | |
| 2 | done `5c19e0f` | `src/radio.h`, `src/radio.c` | Comment drift: "Until P5 ... P6 takes the channel from the network record" (`radio.h:17-19`; the network record is #37 in P5 and the channel is asserted to be the test channel, `src/network.c:17`); history notes "until P5, GDO2 did" (`radio.c:21`), "until P5" (`radio.c:121`), "CMake checked this against BRIDGE_EUI64 until ..." (`radio.c:1780`). | 0 (`__LINE__`) | R |
| 3 | done `1da9cc0` | `src/cc110l.h`, `src/cc110l.c` | "as in the P2 images" (`cc110l.h:31`), "bring-up on the bench (P5)" (`cc110l.h:39`), "(30 s)" repeats the airtime budget (`cc110l.h:240`), "(as until 2026-11-17)" (`cc110l.c:216`); macro comments split by clang-format (`cc110l.c:73-76`); prototype array sizes `ack[29]`, `answered[16]` (`cc110l.h:258,295`) against `MAC_ACK_LEN`, `MAC_HDR_LEN` in the definitions (`cc110l.c:784,856-857`). | 0 (`__LINE__`) | R |
| 4 | done `d1b10e7` | `src/network.h` | A doc block for `network_create()` sits after its declaration (`network.h:37-41`); literal 2048 and 512 (`network.h:44-48`) stand for `STORE_CTR_LOW` and `STORE_CTR_ENTRIES` (`store.h:162,164`). | 0 | |
| 5 | done `fc887e1` | `src/store.c` | File comment says "Network record and counter log (#37)" (`store.c:1-2`); the file also holds the identity record and the slotted records the boot state uses. | 0, boot hash | R |
| 6 | done `f2bf731`, `5a51bfc`, `1a81295` | `src/board.c`, `src/app.c`, `src/main.c`, `src/watchdog.c`, `src/clock.c` | Orphan comment repeating `board.c:117-118` (`board.c:131-133`); "as before the button module" (`app.c:53`); "~2-6 s" against 2.0-5.6 s (`main.c:101`, `watchdog.h:8`); "at 100 MHz" for a 96 MHz clock (`watchdog.c:30`, `clock.h:7`); "Bench VDD is 3.20 V (hardware.md P0)" (`clock.c:39`) is the ST-Link's own reading from before the USB supply (`hardware.md:173,267`). | 0 (`__LINE__`) | |
| 7 | done `13d80e9`, `cde3050`, `23f8dd7` | `src/fwupd.h`, `src/pa3_adc.c` | Comments broken by clang-format inside macros: `FWUPD_BLOCKS` (`fwupd.h:50-52`), `EOC_WAIT_US` (`pa3_adc.c:22-24`); move them above the macro. | 0 | |
| 8 | done `fe5fa6b` | `src/enc28j60.h`, `src/net.c` | Short names `rx_begin()`, `rx_read()`, `rx_end()`, `tx_end()` (`enc28j60.h:69-72`, `net.c:229`) for `enc28j60_rx_begin()` etc. (`enc28j60.h:81-97`). | 0 | |
| 9 | done `9fdbf11` | `src/boot/bootctl.h` | `MAX_TRIES` (`bootctl.h:24`) for `BOOTCTL_MAX_TRIES` (`bootctl.h:32`). | 0, boot hash | |
| 10 | done `4bbfbe4` | `tests/host/Makefile` | `test_airtime_SRCS` sits under the comment on the two cc110l programs (`Makefile:69-74`). | none | |
| 11 | done `e9b659f` | `src/watchdog.c` | `IWDG_PR_DIV64` (`watchdog.c:47`) and `WDG_PRESCALER_DIV 64` (`watchdog.h:9`) are set apart; tie them with a `_Static_assert`. | 0 | |
| 12 | done `c95dd81`, `87f5779`, `cc8bc79` | `src/fwupd.c`, `src/fwupd_task.c` | Durations and limits written into `why` strings next to the constants they repeat: "every 10 s", "448 KB", "0x08010000", "120 s", "10 min", "in 10 s" (`fwupd.c:296,323,454,486,495,529,533`; `fwupd_task.c:174,187` against `UPDATE_OP_WAIT_MS`, `radio.c:406`). Pin each with a `_Static_assert` on its constant (strings unchanged); name the 5 s cap (`fwupd.c:515`). | 0 | |
| 13 | done `7739e1d` | `src/cc110l.c`, `src/cc110l.h` | Magic numbers: strobe and status range 0x30-0x3D and FIFO 0x3F (`cc110l.c:152,161,164,170,463,677,979`), MARCSTATE mask 0x1F (`:612,633,841`), TXBYTES 0x80/0x7F (`:801,804`; `CC_RXBYTES_*` exist, `cc110l.h:129-130`), ISR CHIP_RDYn wait 100 us (`:907,964,986`), `tx_expect()` 11/160/75 (`:237`). Names only; codegen identical. | 0 | R |
| 14 | done `7703ecd` | `src/radio.c` | Frequency checks use literals 0xF8, 0x22 and 999000 (`radio.c:199-212`) instead of the table (`radio.c:127-188`) and `TXGATE_MIN_DIST_HZ` (`txgate.h:38`); IDLE to TX "+ 75u" in the beacon log (`radio.c:1732`). | 0 | R |
| 15 | done `6f27d3f` | `src/txgate.c` | Frame offsets 3, 5, 7, 11, 13, 21 and the beacon length 32 (`txgate.c:122,125,134,137,150,166,179,182,185`) as named offsets, from `src/mac.h` where they exist. Safety code: `test_txgate` must pass unchanged. | 0 | R |
| 16 | done `54ce676` | `src/fwimg.c` | Slot header and descriptor field offsets 4/6/8/12/16/20/52/60 (`fwimg.c:45-94`) and the sector map 0x08020000/0x20000/0x10000 (`fwimg.c:36,40`) as named constants. | 0, boot hash | R |
| 17 | done `a052290` | `src/store.c`, `src/store.h` | `STORE_CTR_ENTRIES *STORE_CTR_ENTRY` formatted as a pointer (`store.c:14`); 4096u (`store.h:162`) for the sector size. | 0, boot hash | R |
| 18 | done `b7d22bd` | `src/spi1.c` | `clock.h` included after the libopencm3 headers (`spi1.c:28`); `br << 3` (`spi1.c:64`) as the BR field shift. | 0 | |
| 19 | done `fe15ebc` | `src/clock.c` | SysTick set-up repeated (`clock.c:102-105`, `:298-301`) and "back to HSI and wait" repeated (`clock.c:182-187`, `:202-207`): two static helpers; `0` passed for a NULL pointer (`clock.c:125,151,158,181,187,200,207`). | about -20 to -40 B text | |
| 20 | done `28f8b68` | `src/board.c`, `src/board.h` | LED and button pins defined in `board.c:8-12` and raw in `GPIOB_USED` (`board.c:35-37`), unlike the `BOARD_*_PIN` table (`board.h:7-21`); the PB1 MODER write by hand (`board.c:128`) repeats the pattern of `board_cs_early()` (`board.c:61-64`): one static helper. | about 0 | |
| 21 | owner | `cmake/toolchain-arm-none-eabi.cmake`, `cmake/CMakeLists.txt` | Template leftovers: C++ flags with no C++ source (`toolchain-arm-none-eabi.cmake:89,134,150,168`), the "e.g. STM32L476RG" comment (`CMakeLists.txt:130`). Optional: lwIP sources compiled while their options are off (`raw.c`, `stats.c`, `ip4_frag.c`; `lwipopts.h:36,47-48,87`). | 0 |  Skipped (owner decision, 2026-11-20): left as is. |
| 22 | owner | `tests/host/test_store.c`, `test_identity.c`, `test_boot.c`, `test_fwupd.c` | Four copies of the external-flash model with power cuts (`test_store.c:71-152`, `test_identity.c:59-96`, `test_boot.c:93-151`, `test_fwupd.c:112-173`) into one `tests/host/flash_model.h`. Test code only, but four files in one commit. | none | Skipped (owner decision, 2026-11-20): the four copies stay; they differ in the fault injection that each suite needs. |
| 23 | owner | `tools/sdr/hs18place.py` | Reads the `hs18:` log lines of the temporary #18 test image (`hs18place.py:3-6`); no such line is left in `src/`. Delete, or keep as the record of the #18 measurement. | none |  Kept (owner decision, 2026-11-20): record of the #18 measurement. |

Counts: 23 commits; 21 are #33 steps; rows 21-23 are decided by the owner (21 skipped, 22 skipped, 23 kept). 10 are comment-only (1-10), 8 are names and asserts with identical code (11-18), 3 change code inside one file or module (19-21). 9 rows (marked R) need a `fw-reviewer` verdict before they are merged; 6 must keep the bootloader hash.

### Rows 1-10: result (#33, 2026-11-20) [V]

Built from a clean export of each commit with the owner's `config/local.cmake` (not printed), outside the working tree, without the owner's local `src/airtime.h` edit. After every commit: `make format` clean; `make test` all passed (including `test_identity_tool` and `test_upload_tool`); text 171848, data 136, bss 85756, as in the baseline; bootloader `.bin` `eb2f44fb…5290` unchanged. A receive-only build (`RADIO_TX_ALLOWED=0`) of the last commit also compiles.

- **Application `.bin`:** `508b33a1adbe1e2a4c13b9e0878138f7cf0540d3b408c6710f955b6ff16c61d9` after row 10. Against the baseline, `objdump -d` and `cmp -l` differ only in three `__LINE__` immediates passed to `fw_panic()`: `radio.c` 743 to 742 and 1296 to 1295 (row 2), `clock.c` 296 to 297 (row 6); 9 bytes in all. Every other commit left the `.bin` byte for byte as before.
- **Split rows:** rows 6 and 7 span several modules, so they went in one commit per module (board; startup; clock and watchdog; `fwupd.h`; `pa3_adc.c`), as rule 1 asks.
- **Clearly wrong comments found in the detailed read and fixed in the same commits:** `board.h` said the factory reset warning overrides the router, cloud and pairing LEDs and never the power LED. `led_overridden()` does the opposite (router, power and cloud; never the pairing LED), which is what the owner saw (`test-bench.md`, "Factory reset and LEDs"). `cc110l.c` named `cc110l_tx_start()` as the place where STX is strobed and frames are charged; since #19 that place is `tx_begin()`, which `cc110l_tx_answer()` also uses. `enc28j60.c:240` used a short name, the same drift as row 8. In `app.c`, `STACK_WORDS` is the housekeeping task's size only, not "2 KB each".
- **Follow-up `23f8dd7` (entropy module):** `rng.h`, `entropy.h` and `pa3_adc.h` said the net task alone owns ADC1 and makes every call that can reseed. Since #19 the radio task also draws with `rng_random()`, in `network_create()` and `pairing_trigger()`, so a reseed can read the ADC in the radio task. `radio-runtime.md`, "Pairing (#19)", counts that reseed in the up to 0.8 s the trigger may take. The DRBG mutex serialises every request after `rng_init()`, so the ADC has one user at a time; the comments now say so. No code changed.
- **On the bench (rule 5), 2026-11-20 [V]:** `424420e` (rows 1-10 plus one comment-only follow-up) was built with `FW_VERSION=4` (id `68a24dfcd65549c4`), installed over the air and confirmed. Built with `FW_VERSION=0`, the same export is `508b33a1…` byte for byte, the build `fw-reviewer` cleared. The boot log is unchanged (identity version 1, placeholder MAC), and the session came up. Too late 0, airtime refusals 0, stack words never used 532/844/332/261. On channel 16 the bridge's Enhanced ACK gap is 225.5-236.6 us, median 225.9 us (n=5). Details: `test-bench.md`, "P8 rows 1-10 image (#33): first flash".
- **Seen and left alone:** log format strings that clang-format split mid-sentence (`radio.c` wake-train and minute lines, `net.c`). Splitting them is codegen-neutral, but they are code, not comments. History notes that record where a measurement came from ("the P2 bench run" in the `radio.c` FREQEST block) stay. `app.c:31-32` still says the radio stack's deepest path is "to be measured". The bench logs (`test-bench.md`) show at least 494 of 896 words never used, but not whether that run took the deepest path.

### Rows 11-20: result (#33, 2026-11-20) [V]

Built as for rows 1-10: a clean export of each commit outside the working tree, the owner's `config/local.cmake` (not printed), no local `src/airtime.h` edit, `FW_VERSION=0`. After every commit: `make format` clean; `make test` all passed (21 programs, including `test_txgate`, `test_identity_tool` and `test_upload_tool`); data 136 and bss 85756 as in the baseline; bootloader `.bin` `eb2f44fb…5290` unchanged (rows 16 and 17 touch boot-linked files). A receive-only build (`RADIO_TX_ALLOWED=0`) of the last commit also compiles.

| After row | text | Application `.bin` SHA-256 | Against the commit before |
| ---: | ---: | :--- | :--- |
| 11, 12 (`cc8bc79`) | 171848 | `b7fc3216…6f35` | one `__LINE__`: `fwupd_task.c` `configASSERT` 249 to 251 |
| 13 (`7739e1d`) | 171848 | `e2dfec41…e71b` | eight `__LINE__` in `cc110l.c` (+8 in the guard, +10 after `tx_expect()`) |
| 14 (`7703ecd`) | 171848 | `ac15eb2d…5bb1` | two `__LINE__` in `radio.c`, 742 to 760 and 1295 to 1313; the register table bytes unchanged |
| 15, 16, 17 | 171848 | `ac15eb2d…5bb1` | none: byte for byte |
| 18 (`b7d22bd`) | 171848 | `a0bd4dbf…e2ca` | five `__LINE__` in `spi1.c`, +2 each |
| 19 (`fe15ebc`) | 171816 | `e7f292d1…b894` | code: -32 B, as predicted (-20 to -40) |
| 20 (`28f8b68`) | 171816 | `e7f292d1…b894` | none: byte for byte |

- **Rows 11-18:** from the row 10 image to row 18, `objdump -d` differs in 16 instructions, every one a `__LINE__` immediate loaded into r2 for `fw_panic()`; 19 bytes of the `.bin`.
- **Row 14, the static asserts still bite:** in a scratch copy, FREQ1 0x33, MDMCFG0 0xF9, MDMCFG4 0xAB and DEVIATN 0x41 each fail the build on the carrier, spacing, data rate or deviation assert.
- **Row 19, why the code is the same in effect:** `systick_ms_start()` makes the same four calls in the same order (CLKSOURCE AHB, reload `clock_hz / 1000 - 1`, clear, enable) for `wait_bits()` and the inlined `delay_ms()`; `sysclk_to_hsi()` makes the same `rcc_set_sysclk_source(HSI)` and the same `wait_bits(&RCC_CFGR, 0xC, 0, 5, NULL)` that `pll_start()` and the inlined `clock_init()` made, checked in the disassembly. The 32 bytes are the two copies folded into calls.
- **Row 20:** `moder_output()` is inlined; the `.bin` does not change.
- **On the bench (rule 5), 2026-11-20 [V]:** `dabd7d1` (rows 11-20) was built with `FW_VERSION=5` (id `ba8af1a3e7720e44`, 171952 bytes), installed over the air and confirmed. Built with `FW_VERSION=0`, the same export is `e7f292d1…b894` byte for byte, the build `fw-reviewer` cleared. The boot log is unchanged (identity version 1, placeholder MAC; clock `from HSE 26 MHz crystal + PLL` after row 19), and the session came up at attempt 1. Too late 0, airtime refusals 0, stack words never used 532/844/332/261. On channel 16 the bridge's Enhanced ACK gap is 225.9-231.2 us, median 226.1 us (n=7, 4 overlay PUTs). New: one PA3 read timeout in the start-up test, most likely a preemption by the radio task's bring-up **[I]**. Details: `test-bench.md`, "P8 rows 11-20 image (#33): first flash".
- **Seen and left alone:** `radio.c` masks MARCSTATE with a literal 0x1F in four places (`CC_MARC_STATE_MASK` now exists); the boot state payload offsets in `fwimg.c` (`p + 4`); `own + 5` and the length 3 in `txgate.c`'s `eui_set()`; FSCAL1 0x3F ("no lock") in `cc110l.c`. Row 14 puts the IDLE to TX time in one place (`CC_IDLE_TO_TX_US`, used by `cc110l.c` and `radio.c`); the rest of proposal 6 stays for #34.

## Not to change

These look redundant but are defence in depth; P8 leaves them as they are:

- `store_net_allowed()` checks both "test channel" and "not the protected channel" (`store.c:371`), as does `network_create()` (`network.c:99`).
- `radio_strobe()` checks the strobe before `guard_header()` checks it again (`cc110l.c:377`).
- Rule 5 of the transmit gate stays separate although rule 2 implies it (`txgate.c:99-104`).
- `store_id_write()` repeats the commit steps of `store_slot_write()` on purpose, so that the bootloader image stayed the same with #39 (`store.c:316-323`). Merging them is proposal 8 below.

## Proposals for #34 (owner approval first; none made)

| # | Kind | Proposal | Sources | Effect |
| ---: | :--- | :--- | :--- | :--- |
| 1 | architecture | Split `src/radio.c` (2244 lines; `radio_task_run()` 203 lines from `radio.c:2041`) into the task loop, receive and ACK task side (`radio.c:588-1032`), bring-up (`:1033-1304`), flash on the shared bus and update operations (`:1305-1344`, `:1829-1886`, `:1940-2012`) and pairing (`:1648-1828`). | function list of `radio.c` | no size change expected; review of the whole radio path |
| 2 | module | One little-endian helper header for `get16/get32/put16/put32`, now in four files. | `fwimg.c:17-32`, `store.c:24-37`, `fwupd.c:303-305`, `boot/bootctl.c:23` | bootloader `.bin` changes: needs a reflash decision |
| 3 | module | One secure wipe helper; five local copies next to `mbedtls_platform_zeroize()` elsewhere. | `store.c:17`, `csl.c:186`, `pair.c:82`, `link.c:64`, `cc110l.c:690`; `network.c:84` | boot `.bin` changes (store.c) |
| 4 | module | One fault-code LED pattern (code and time to level); four copies of 150/250/2500 ms. | `log.c:58-83`, `net.c:123-137`, `session.c:589-602`, `boot/bl_hw.c:125-135` | bootloader copy may stay |
| 5 | API | Export the hex formatter of the update module; two copies. | `fwupd.c:569-576`, `fwupd_task.c:135-142` | small |
| 6 | API | One home for the channel plan and radio timing constants: fXOSC and the frequency word in two files; IDLE to TX 75 us in two places; the airtime charge (161 us per byte, 100 us ramp) and the TX watch window (160 us, 75 us) differ on purpose and should say so in one place. Values unchanged. | `radio.c:197-198`, `txgate.c:11-14`, `cc110l.c:237`, `radio.c:1732`, `airtime.h:45-49` | R; `airtime.h` only after the local edit is gone |
| 7 | rename | `radio_strobe()` to `cc110l_strobe()`; `RADIO_PA_BRINGUP` to a name without "bring-up"; drop the alias `RADIO_PROTECTED_CHANNEL` for `TXGATE_PROTECTED_CHANNEL`. | `cc110l.h:47,146`, `radio.h:25`, `radio.c:112,207-208`; tests `test_cc110l.c:52-73`, `test_cc110l_off.c:38-85` | R |
| 8 | storage code | Merge the commit steps of `store_id_write()` into a helper shared with `store_slot_write()`, and the slot choice repeated in both. | `store.c:139-184`, `store.c:324-364`, `store.c:154-155`, `store.c:335-336` | bootloader `.bin` changes; R |
| 9 | module | One MX25R opcode header for the driver and the bootloader. | `mx25r.c:32-44`, `boot/bl_hw.c:189-196` | boot hash should stay (defines only) |
| 10 | API | One test for "a pairing target is named in the build" instead of two. | `main.c:26-31`, `radio.c:1764` | small |
| 11 | API | `mx25r_read_id()` and `mx25r_read_status()` return `bool` but always return true. | `mx25r.c:186-196` | small |
| 12 | behaviour (log text) | Boot banner still reads "tado-bridge P5 #19" (`main.c:58`). Log text is observable; bench tools parse log lines. | `main.c:58-70` | none |

## Progress by module

Status: **todo** (rows above), **done** (all its rows landed), **none** (nothing found by the survey; detailed read at #33 time), **proposal** (only #34 items), **out** (not in scope).

| Module | Files | Read | Status | Rows |
| :--- | :--- | :--- | :--- | :--- |
| Startup, tasks | `main.c`, `app.c/.h`, `hooks.c`, `syscalls.c`, `faults.c/.h` | full | done | 6 done; P10, P12 |
| Board | `board.c/.h` | full | done | 6, 20 done |
| Log, RTT | `log.c/.h`, `rtt.c/.h` | full | proposal | P4 |
| Time, clock, watchdog | `timebase.c/.h`, `clock.c/.h`, `watchdog.c/.h` | full | done | 6, 11, 19 done |
| Buttons, pairing LED | `button.c/.h`, `pair_led.c/.h` | full | none | |
| Config headers | `FreeRTOSConfig.h`, `lwipopts.h`, `tado_mbedtls_config.h`, `arch/cc.h`, `lwip_port.c` | full | done | 1 done |
| Entropy, DRBG | `pa3_adc.c/.h`, `entropy.c/.h`, `rng.c/.h` | full | done | 1, 7 done |
| SPI1 | `spi1.c/.h` | full | done | 18 done |
| CC110L driver | `cc110l.c/.h` | full | done | 3, 13 done; P6, P7 |
| Airtime | `airtime.c/.h` | full | out (local edit) | P6 later |
| Transmit gate | `txgate.c/.h` | full | done | 15 done |
| Radio task | `radio.c/.h` | full (#33 row 2) | done | 2, 14 done; P1, P6, P7, P10 |
| MAC, CCM | `mac.c/.h`, `ccm.c/.h` | ccm full, mac skimmed | none | |
| Link, 6LoWPAN, CSL, relay, msgq | `link.c/.h`, `lowpan.c/.h`, `csl.c/.h`, `relay.c/.h`, `msgq.c/.h` | msgq full, rest skimmed | proposal | P3 |
| Pairing | `pair.c/.h` | skimmed | proposal | P3 |
| External flash driver | `mx25r.c/.h`, `flash_layout.h` | full | proposal | P9, P11 |
| Store, identity, network | `store.c/.h`, `store_port.c`, `identity.c/.h`, `network.c/.h` | full | done | 4, 5, 17 done; P8 |
| Images, update | `fwimg.c/.h`, `image_desc.c/.h`, `fwupd.c/.h`, `fwupd_task.c/.h`, `handoff.c/.h` | full | done | 7, 12, 16 done; P2, P5 |
| Bootloader | `boot/main.c`, `boot/bootctl.c/.h`, `boot/bl_hw.c/.h`, `boot/bl_mbedtls_config.h` | `bootctl.h` full, rest skimmed | done | 9 done; P2, P4, P9 |
| Ethernet, lwIP, net task | `enc28j60.c/.h`, `net.c/.h` | `enc28j60.h`, `net.c` full; `enc28j60.c` part | done | 8 done; P4 |
| TLS, WebSocket, CoAP, session | `tls.c/.h`, `ws.c/.h`, `coap.c/.h`, `session.c/.h` | skimmed | proposal | P4 |
| Host tests | `tests/host/*` | Makefile full, rest skimmed | done | 10 done; 22 skipped |
| Tools | `tools/sdr`, `tools/bench`, `tools/entropy`, `tools/identity`, `tools/update`, scripts | headers | done | 23 kept; rule 6 |
| Build | `Makefile`, `cmake/*` | skimmed | done (owner skipped) | 21 skipped |
