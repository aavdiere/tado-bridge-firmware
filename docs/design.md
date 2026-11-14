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

Estimates, to be measured on a prototype.

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

Freedo's job pool of 128 (`freedo/firmware/cc1101_bridge/cc1101_bridge.ino:203`) is too large to carry over. **Decision (user): support 16 devices for now,** matching freedo's 16 anchors and neighbours (`:191,201`).

### Flash estimate [I]

About 200-250 KB of 512 KB: mbedTLS 100, lwIP 40, FreeRTOS 10, application 60-100. Stock reaches past 360 KB (`tanoclo/patch_ib_firmware/patch_endpoint.sh:24`).

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
