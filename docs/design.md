# IB01 firmware design

**Status:** storage (#27) and software stack (#18) approved by the user (2026-11-11); self-update (#20) approved on condition that the firmware fits the layout (size check before building). Built and tested so far: the entropy pool (#14, 2026-11-14) and TLS, WebSocket and auth to tanoclo (#15, 2026-11-14); the rest is not built yet. Other sections (entropy, network stack, self-update) will be added as they are decided.

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
| `0x000000` | 512 KB | Update image: candidate (#20) |
| `0x080000` | 512 KB | Update image: previous version (#20) |
| `0x100000` | 4 KB × 2 | Identity, slots A and B |
| `0x102000` | 4 KB × 2 | Network, slots A and B |
| `0x104000` | 4 KB × 2 | Counter log, sectors A and B |
| `0x106000` | 4 KB × 2 | Boot state, slots A and B (#20) |
| `0x108000` | Rest | Unassigned |

### Slotted records (identity, network)

Stock also keeps active and backup configuration partitions (`tanoclo/patch_ib_firmware/extract_channel.js:6`).

Format: `magic u32 | version u16 | length u16 | seq u32 | payload | CRC-32`. The F411 has a CRC unit (`stm32f411ce.txt:43`).

- **Payloads:** identity holds the serial and EUI-64. Network holds the key and channel.
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

### Lifecycle

- **At flashing:** a host tool reads serial and EUI-64 from the backup (`tanoclo/patch_ib_firmware/extract_rf_key.js`) and writes the identity record.
- **First boot with no network record:** create the key (depends on #26), then write the record before starting the radio.
- **Identity missing or invalid:** do not start the radio; indicate on the LED.
- **Factory reset** (factory reset button, PB10): erase network and counter records, keeping identity. Devices must then be re-paired.

### Read protection

- **Level 0 during development:** full SWD access.
- **Never level 2:** it disables debug access and removes recovery (`datasheets/rm0383.txt:2537-2538`).
- **Level 1 is optional, decide at the end.** It would not protect the key in external flash, and the key also crosses the air during pairing (`radio.md`).

### Implementation: network and counter records (#37)

Built 2026-11-17, host-tested, not yet reviewed or flashed. Code: `src/flash_layout.h`, `src/mx25r.c` (program and erase), `src/store.c` (records, hardware independent), `src/store_port.c`, `src/network.c` (boot and running), the counter gate in `src/csl.c`; tests in `tests/host/test_store.c`. Where it differs from the design above, the difference is marked **Differs**.

**Stock contents of the external flash [V]:** the bench unit's backup (`~/ib01-backups/2026-11-11/a/unmodded_spi.bin`, outside the repository; only a map of 4 KB sectors that are not all `0xFF` was taken, no contents) has data in `0x000000-0x03DFFF`, `0x080000-0x0BEFFF`, `0x100000-0x100FFF` and `0x102000-0x102FFF`. Everything else is blank, including `0x103000` (the stock backup partition, `tanoclo/patch_ib_firmware/extract_channel.js:6`) and the counter log at `0x104000-0x105FFF`. Stock keeps its active configuration partition at `0x102000` (`extract_channel.js:6,66`), with the channel 26 setting (`test-bench.md`, "The bench unit's own stock configuration"), and a descriptor table in `0x100000-0x103FFF` (`tanoclo/patch_ib_firmware/check_firmware.sh:83`).

**Decided (owner, 2026-11-17): keep slot A at `0x102000`; the stock configuration is not kept, the backup is enough.** The conflict was: network slot A (`0x102000`) is the stock active configuration. The layout accepts overwriting stock contents because a backup exists ("Layout (external flash)"), so the code follows it, but the first `network_create()` (pairing, #19) will erase that sector. Moving the network slots to the unassigned `0x108000-0x109FFF` would avoid it; that is a layout change for the owner to decide. Until then nothing writes there: see "Writes" below.

**Address guard:** `src/mx25r.c` panics on any Page Program or Sector Erase outside `0x102000-0x105FFF` (network and counter-log sectors), on an erase address that is not sector aligned, and on a program that would cross a 256-byte page (a PP past the page end wraps to the page start, `datasheets/mx25r1635f.txt:2310-2315`). No other write command exists in the driver: no WRSR, no block or chip erase, no security-register write.

**Program and erase:** the datasheet's flow with fail flags (`mx25r1635f.txt:1281-1330`): WREN, RDSR until WEL = 1, the command, RDSR until WIP = 0, WEL cleared, then RDSCUR P_FAIL (bit 5) or E_FAIL (bit 6) clear (`:2521-2540`). Timeouts are twice the worst case of either power mode: tPP 10 ms (`:3337`), tSE 240 ms (`:3340`). Every bus transaction moves at most 64 data bytes (about 90 µs at 6 MHz), because SPI1 transactions run in a critical section (`src/spi1.c`). The store reads back every program and checks every erased sector for `0xFF`.

**Boot wait:** after the deep power-down release, RDSR is polled until WIP = 0 (at most 480 ms, 2 x tSE) before the JEDEC ID or any array read. An MCU reset (watchdog, debugger) does not stop a program or erase the chip is running, and while WIP = 1 the chip ignores array reads and does not decode RDID **[D]** (`mx25r1635f.txt:556-557, 968-969`; RDSR works at any time, `:1142-1144`). If WIP stays set, or the JEDEC ID is not `C2 28 15`, the flash is not used: no network record is read, so the link keeps no key and nothing is sent (`flash_boot()` in `src/radio.c`; found by `fw-reviewer`, 2026-11-17).

**Slotted record, as built:** format as designed; little endian; CRC-32 is the IEEE reflected CRC in software, not the F411 CRC unit, so the code is host-testable. **Differs (stricter):** the magic is the commit mark: erase the older or invalid slot, check it blank, program everything after the magic, read back, then program the magic and read the whole record back. A slot whose magic does not match is read no further, so the stock configuration in slot A is read only for its first 4 bytes.

**Network record v1:** magic `0x3154454E` ("NET1"), version 1, payload 24 bytes: PAN ID u16, channel u8, reserved u8 (0), key 16 bytes, counter floor u32; 40 bytes in all with the header and CRC. **Differs:** the design listed key and channel only; the PAN (`0xABCD`, `src/mac.h`) and a counter floor were added. Only channel 10 and PAN `0xABCD` are written or loaded; a record for channel 26 is refused when written and ignored when read (`store_net_allowed()`). The floor is the counter log's highest entry when the network is created, so a new network does not reuse counters of an old one.

**Counter log, as built:** entries as designed; the highest valid entry is the reservation, the first counter not reserved. Entries are appended after the last slot that is not blank, and a slot is used once even if its write fails. The sector holding the highest valid entry is never erased **[I]**: if every slot of the next sector fails as well (512 failed writes with erases still working), appends fail until the next boot instead of erasing it, and the coordinator stops at its limit; the boot's scan starts again from the highest entry's sector, which is full, and erases the other one (found by `fw-reviewer`, 2026-11-17; before, the 513th failed write erased the sector with the only valid reservation, and a reboot would have restarted the counters at the record's floor). Boot takes the higher of the log and the record's floor, writes `+4096` and passes both values to `link_set_key(key, first, limit)`. Running: at a quiet moment in the radio task, once less than 2048 remain, `+4096` from the next counter. **Differs:** the coordinator itself stops at the limit (`csl_set_counter_limit()`, `src/csl.c`): no transaction starts at or above it, so a failed log write stops transmission instead of letting the counter outrun the flash; the write is retried every 10 s.

**Differs, SPI1 sharing:** the store blocks the radio task for the length of a write: a counter entry is two short transactions plus the page program cycle, tPP 3.2 ms typical and 10 ms max in ultra low power mode (`datasheets/mx25r1635f.txt:3337`; 0.85 ms and 4 ms in high performance mode, `:3432`) **[D]**. The datasheet gives no shorter time for an 8-byte program than for a page, so 10 ms is the bound used; the chip runs in ultra low power mode, its default (`:3746`), since the driver never writes the configuration register. A frame that arrives during a program gets no ACK, never a late one **[I]**: `ack_consider()` answers only a frame the RXBYTES read showed still arriving, with no GDO0 edge since (`src/radio.c:700-714`), so a frame that ended while the task was blocked is not answered, and the device sends it again. At 10 ms a frame that starts just after the program begins brings about 62 bytes at 50 kbps, close to the 64-byte RX FIFO (`datasheets/cc110l.txt:1483`), so a longer frame may overflow it and is dropped by the overflow path **[I]**. A sector erase, once per 512 entries (about every million frames), 40-240 ms, during which the radio FIFO is not served (other tasks run, `vTaskDelay` between polls). Both happen only when no frame is arriving and no transaction is on the air. The design's non-blocking WIP poll is left for #22.

**Differs, lifecycle:** no network is created at boot. Without a valid record the link keeps no key, so nothing is decrypted, acknowledged or sent, and the counter log is neither read nor written. `network_create()` (fresh key from the CTR_DRBG with `rng_random()`, test channel only) is for pairing (#19); it has no caller yet, so the linker drops it and the image contains no code that writes a network record. It succeeds only if the network read back as a boot would read it (`store_net_commit()`) is the record just written, by seq and contents, and it hands the key read back to its caller, for pairing to send under the pairing key (`radio.md`, "Link security and pairing"). Once the write has started, a failure leaves no key set until the next boot. Host test: a magic bit that reads right during the write's own verify and then fades makes the commit fail, where before the older network's record would have been activated (found by `fw-reviewer`, 2026-11-17). The key is never logged; the boot log shows the record's seq, channel and counter values only.

**Power-cut tests [I] (host, `make test`):** a NOR model (programming only clears bits) cuts the power inside any program (a torn prefix plus one byte with part of its bits) or erase (bytes erased, with bits set, or with bits cleared as in the chip's pre-program phase). Covered: a cut at every step of a record write from three histories (none, one, two records), 8 random tears each (72 cases); a cut at every step of a counter-log sector change, to a blank and to a stale sector (64 cases); a 3000-extend random run with 158 cuts (1 in an erase); create-then-boot with a cut at every step; a silently weak program bit; one flipped bit at every record offset; noise and planted entries in both log sectors; with the power on, every program failing (fail flag with nothing or part of the bits done, or a silent miss the read-back catches) from 8 fill levels (24 cases), and every erase failing in the same three ways at a change to a blank and to a stale sector (6 cases), with the model checking that the sector with the highest entry is never erased. Checked after every reboot: a record is either the old or the new one; the counter never starts below one already used on the air; the log's highest entry never falls below a reservation reported written; nothing outside `0x102000-0x105FFF` changes; no byte is programmed twice without an erase. Mutations of the slot choice, the scan and the boot start were each caught, and so was removing the guard that keeps the highest entry's sector.

**Not built yet:** factory reset (erase network and counter sectors, PB10); the identity record (#21); boot state (#20).

### Depends on

- #26: how the network key is generated (see "Entropy").
- #20: see "Self-update and first flash".

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

**Identity and trust anchor:** build values from a gitignored `config/local.cmake`, written to a generated header (`build.md`, P4 TLS). P6 moves the serial and EUI-64 into the identity record. The trust anchor stays a build value, as "TLS settings" says.

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

**P5 static size (#17, 2026-11-17) [V]:** build `88c7894`, `arm-none-eabi-size build/tado-bridge.elf`: text 142640, data 136, bss 79392 (earlier P4 production image: bss 56188). Static RAM is now 79528 bytes, 83624 with the 4 KB main stack, so 47448 bytes (46 KB) are not reserved. The radio side added 23204 bytes of `.bss`, about twice the 11 KB estimate. Largest new symbols (`arm-none-eabi-nm -S`): `relay` 6320 bytes, the two link queues `up_store` and `down_store` 4096 each, `g` 4008. The heap margin is unchanged because P5 uses `.bss`, as planned above. Not yet measured on the board: stack use of the radio task (640 words, `radio-runtime.md`, "P5 link implementation (#17)"). The estimate table above is superseded by these figures.

### Flash estimate [I]

About 200-250 KB of 512 KB: mbedTLS 100, lwIP 40, FreeRTOS 10, application 60-100. Stock reaches past 360 KB (`tanoclo/patch_ib_firmware/patch_endpoint.sh:24`).

**Flash, measured [V]:** text 142640 bytes of 512 KB (28 %) at `88c7894` (`arm-none-eabi-size`), below the 200-250 KB estimate. Bootloader and second slot (#20) still have to fit beside it.

### To prove on a prototype

1. Full TLS and WebSocket connection to tanoclo within the RAM budget.
2. Enhanced ACK inside the 0-1 ms gap, during a TLS handshake.
3. Radio traffic unaffected while flash is erasing.

A Nucleo-F411 with ENC28J60 and CC1101 modules could test all three without touching the IB01 **[I]**.

## Self-update and first flash (#20)

### Internal flash layout

**Before building (user request, 2026-11-11): first check the required sizes** to confirm this layout is possible: the real bootloader size against its 64 KB, the application image against the 448 KB region, and the image plus header against the 512 KB update slots in external flash. Adjust the layout if anything does not fit.

F411 sectors: 4 × 16 KB, 1 × 64 KB, 3 × 128 KB (`datasheets/rm0383.txt:2023-2031`).

| Address | Sectors | Size | Use |
| :--- | :--- | :--- | :--- |
| `0x08000000` | 0-3 | 64 KB | Bootloader, expected about 16 KB, the rest spare |
| `0x08010000` | 4-7 | 448 KB | Application (estimated 200-250 KB) |

Stock's own layout (`tanoclo/patch_ib_firmware/check_firmware.sh`) does not apply: first flash replaces all of it, and the backup keeps the original.

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
- Use a bridge-only path, for example `b/fw`, so it cannot be confused with the devices' `d/fw`.
- Use 1024-byte blocks: about 250 blocks.
- `POST /api/send-raw` (`tanoclo/docs/websocket.md:249-250`) may let a small uploader run with tanoclo unchanged. Check it can see the ACKs **[I]**. Otherwise add one additive handler to tanoclo.

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
