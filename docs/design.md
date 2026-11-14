# IB01 firmware design

**Status:** storage (#27) and software stack (#18) approved by the user (2026-11-11); self-update (#20) approved on condition that the firmware fits the layout (size check before building). Nothing here is built or tested. Other sections (entropy, network stack, self-update) will be added as they are decided.

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
- Measure the real pin's readings on the board before relying on it.

Configuring a pin as an analog input does not drive it, so testing a candidate is safe **[I]**.

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
- Buffers: 4 KB in, 2 KB out, instead of mbedTLS's 16 KB default. Depends on the server not sending records over 4 KB. WebSocket messages are small, but confirm on a prototype **[I]**.
- WebSocket clients must mask frames with random values, using the same DRBG (RFC 6455).

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
