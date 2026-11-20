# Bridge-to-server API

The contract the new firmware must speak so that tanoclo works as the server unchanged. Paths are relative to `tanoclo/`.

Status tags: **[V]** seen on real hardware by a repo author, **[D]** stated in a repo or its code, **[I]** our own inference.

**Evidence limit:** this comes from server code and docs. Nobody here has observed a stock bridge's own uplinks. Treat uplink details as provisional until the capture in `open-questions.md` section C.

## Transport [D]

- `wss://<server>/hw/v2`, header `Sec-WebSocket-Protocol: binary` (`ws-server/lib/proxy-manager.js:197-202`). Self-hosted server listens on TLS port 988 (`ws-server/lib/config.js:47`).
- With our own firmware, server URL and trust root are build or config values. No stock-image patching applies **[I]**.
- The stock client probably sends HTTP/1.0, since the server rewrites it to 1.1 (`ws-server/lib/proxy-manager.js:85-95`) **[I]**. New firmware should send HTTP/1.1.
- DNS is required for a hostname; DHCP is assumed. No NTP: time arrives over CoAP.
- No per-frame crypto at the WebSocket layer; TLS only (`ws-server/lib/ws-bridge.js`).

## Frame [D]

Big-endian, 28-byte header, then raw RFC 7252 CoAP (`docs/websocket.md:13-25`, `ws-server/lib/ws-bridge.js:11-15,62-130`).

| Offset | Size | Field | Value |
| :--- | :--- | :--- | :--- |
| 0 | 2 | direction | `0x0001` uplink, `0x0002` downlink |
| 2 | 1 | ipv6_len | `0x10` |
| 3 | 16 | ipv6 | End device or bridge address |
| 19 | 2 | field_a | Downlink `4`; uplink unknown |
| 21 | 1 | field_b | Downlink `2`; uplink unknown |
| 22 | 2 | udp_port | Usually 5683 |
| 24 | 2 | field_c | Downlink `5`; uplink unknown |
| 26 | 2 | coap_len | CoAP length |
| 28 | n | CoAP | Message |

The server accepts `field_a/b/c` as 0 on uplink (`ws-server/test/test_ws_comprehensive.js:144-149`).

TLV payloads: FID u16be, length u8, value (`docs/coap_tlv.md:183-197`). FID dictionary: `docs/coap_tlv.md:243-474`. Options: 4, 7, 11, 12, 15, 17, 23, 27, 2048 (`ws-server/lib/coap.js:40-52`). Server message IDs are `0x7000-0xFFFF` (`ws-server/lib/coap-transport.js:39-44`).

## Bridge sends [D]

| Resource | Purpose | Source |
| :--- | :--- | :--- |
| `POST auth/key` | Serial `0x0260`; reply `0x0260` + 16-byte `0x0261` | `ws-server/lib/handlers.js:118-156` |
| `POST auth/token` | Serial, optional nonce `0x0007`; reply token `0x025E`, lifetime `0x025F` minutes | `ws-server/lib/handlers.js:214-257` |
| `GET time` | Reply `0D` + u32le Unix seconds | `ws-server/lib/coap.js:503-508` |
| `PUT d/{id}/neighbors` | Block1 at 64 bytes | `docs/coap_tlv.md:600-618` |
| `...found` | Pairing: new device IPv6, reply factory key. Exact URI unknown | `ws-server/lib/handlers.js:177-210` |

**Observed with our firmware against tanoclo (2026-11-14) [V]** (`build.md`, P4 TLS):
- All three replies are piggybacked ACKs with 2.05: `auth/key` with `0x0260` (the serial echoed) and a 16-byte `0x0261`; `auth/token` with an 8-byte `0x025E` and `0x025F` = 1440; `GET time` with `0D` plus u32le.
- The serial only has to be a row in `devices`. The whitelist is empty on the bench, so it lets every known device through. An unknown serial gets no answer at all.
- tanoclo stores the address from the frame header as the bridge's IPv6 address.
- It takes HTTP/1.1, `Sec-WebSocket-Protocol: binary` and TLS 1.2 with max_fragment_length 2048.

## Bridge handles itself [D]

| Resource | Purpose | Source |
| :--- | :--- | :--- |
| `POST d/I/IB0000FT0100<digits>/pair` | Open or close pairing; `dur` 0 closes. **Handled since #40** (below) | `ws-server/lib/commands/device.js:341-388` |
| `GET d/rfkey` | Network key | `ws-server/lib/commands/device.js:475-488` |
| `PUT d/config` | Device registration push after a device paired (CON; TLV `0x0001` serial, `0x0036` type, `0x003a` firmware, `0x4000` zone, `0x4020` role) | `ws-server/lib/commands/device.js:773-827`, `ws-server/lib/handlers/device-handlers.js:151` |
| `POST d/reboot`, `PUT d/identify`, `GET d/info` | Control; responses unspecified | `ws-server/lib/commands/device.js:269,287` |
| Time broadcast | NON 2.05 every 4 h, same 5 bytes as `GET time` | `ws-server/server.js:220-246` |
| `POST b/fw`, `GET b/fw` | **Ours, not tanoclo's (#24):** the bridge's own firmware update, CoAP Block1 of 1024 bytes, and its status line. tanoclo has no handler; `tools/update/upload.py` sends it through `POST /api/send-raw` | `src/session.c`, `src/fwupd.h`; `design.md`, "Implementation: self-update, application side (#24)" |

**Answers to the server's confirmable requests.** The server resends a CON downlink at 6, 13, 26, 50 and 90 s until an ACK with its Message ID comes back from the bridge; it matches on the Message ID alone and reads neither the code nor the token (`ws-server/lib/coap-transport.js:34,195-265`, `ws-server/lib/message-router/index.js:197,542-549`) **[D]**. Its own answer to a PUT is a piggybacked 2.04 with the request's token and no options (`ws-server/lib/handlers/coap-helpers.js:24-49`, `ws-server/lib/coap.js:341-351`) **[D]**. The bridge answers `PUT d/config` the same way: ACK, 2.04, Message ID and token echoed, no options, no payload; the push is logged and not stored, since the bridge keeps no device table of its own (`src/session.c`, `on_config()`; host test `tests/host/test_coap.c` against bytes from tanoclo's own codec). Stateless, so a resent copy gets the same answer. What stock answers, and what it does with the push, is not known **[I]**. `b/fw` is answered as RFC 7959 says: 2.31 with the Block1 echo after each block is in flash, 2.04 after the last, 4.xx or 5.xx with a diagnostic text (`src/session.c`, `on_fw()`) **[I]**. Every other request for the bridge (`d/rfkey`, `d/reboot`, ...) is still only logged and gets no answer.

**Remote pairing trigger, `POST d/I/IB0000FT0100<digits>/pair` (#40) [I].** Owner decision 2026-11-17 (`open-questions.md`, row 28a). What tanoclo sends (`ws-server/lib/commands/device.js:341-388`, `ws-server/lib/command-api.js:102-157`) **[D]**: CON POST to the IPv6 address of the bridge's own `devices` row, so the bridge's address in the frame header; options 7 = `ff ff` and 12 = `2a`; no option 2048 (`skipVendor`); Uri-Path `d` / `I` / `IB0000FT0100` + the serial without its leading `IB` / `pair`; payload `01 04 00 00 <dur u16 BE> 02 01 00` (one-byte tags and lengths; `dur` 1-65535 s, default 300, 0 to close; tag `0x02` unknown). tanoclo also runs its own timer and sends `dur` 0 after the duration (`device.js:357-376`). Its triggers: `POST`/`DELETE .../devices/<bridge serial>/pairing` (`ws-server/api/routes/devices.js:332-375,981-982`, and the command router `lib/command-api.js:745-763`), a new device in the home (`devices.js:466-470`, opens) and a deleted one (`devices.js:520-524`, closes). What the bridge does (`src/session.c` `on_pair()`, `src/pair_remote.c`, `src/radio.c` `pairing_remote()`; host test `tests/host/test_pair_remote.c` with bytes from tanoclo's codec):

| Request | Bridge | Answer |
| :--- | :--- | :--- |
| Uri-Path segment is not `IB0000FT0100` + our serial without `IB` (as tanoclo builds it) | Nothing | 4.04 |
| Not POST | Nothing | 4.05 |
| Payload not a list of tag, length, value with exactly one tag `0x01` of 4 bytes (other tags skipped) | Nothing | 4.00 |
| `dur` > 0, no `PAIR_TARGET_EUI64` in the build, or a receive-only image | Nothing | 5.01 |
| `dur` > 0, no window open | Opens the pair button's window through the same code and guards (`radio-runtime.md`, "Pairing (#19)"), for `min(dur, 600)` s: tanoclo's 300 s gives 300 s | 2.04, or 5.03 if a runtime guard refuses (flash, identity, radio, queued frames of the last window, network record) |
| `dur` > 0, a window open (button or server) | Nothing: its length, counters and any exchange stay as they are | 2.04 |
| `dur` 0 | Closes the open window, whoever opened it | 2.04 (also if none was open) |
| A second request while one is in work, or the radio task does not take it within 10 s | Nothing (a withdrawn request never runs) | 5.03 |

The answer is a piggybacked ACK with the request's Message ID and token, no options, no payload, as for `d/config`; tanoclo reads neither the code nor the token (above), so every answer stops its resends. A copy with the same Message ID while the radio task has the request is answered when it is done; a later copy, for 247 s (RFC 7252 EXCHANGE_LIFETIME; tanoclo's resends end after 185 s), gets the first answer again and nothing is done again, also over a new connection (tanoclo resends by bridge). So a late copy of an "open" never reopens a window a later "close" closed. One RTT line per request: `session: POST pair (mid ..., dur ... s): window opened for 300 s on channel 16 ...: 2.04 sent`, `...: window closed: 2.04 sent`, `...: refused: <why>: 5.xx sent`. Not built: the `found` report (row 28a, not decided). What stock answers is not known.

Omit `d/dbg/m`, `d/dbg/st` and `d/dbg2/tlvs` (`docs/coap_debug_endpoints.md`): not needed, and they expose memory and NVM. **Decision.**

## Bridge only relays [D]

- Devices to server: `d/{id}/sen`, `config`, `act`, `mnt`, `err`, `lock`, `selftest`, `fw/state`; `z/p`, `z/{id}/extui`, `z/{id}/fallback`, `z/{id}/ow`; `h/{id}/hvac/{mon,config,dhw,maint,codes}`.
- Server to devices: `z/s`, `z/ow`, `z/extui`, `d/config`, `d/lock`, `d/fallback`, `d/dispsettings`, `mnt`, `selftest`.
- CoAP message IDs appear to be preserved **[I]**: the server matches device ACKs to its own IDs (`ws-server/lib/coap-transport.js:237-265`).

## Who does what

- **Bridge:** network key, CSL coordination, Router Advertisements, pairing exchange, neighbour table, device firmware blocks from external flash.
- **Server:** schedules, sleepy-device queues, command retries at 6, 13, 26, 50, 90 s (`docs/websocket.md:270-285`).

## Test harness

- `ws-server/test/test_ws_comprehensive.js`: simulated bridge
- `ws-server/test/test_unit_wsbridge.test.js`, `test_unit_coap.test.js`, `test_unit_tlv.test.js`: codec vectors
- `ws-server/test/ci_seed.sql:39-67`: seeds a bridge and whitelist
- `POST /api/send-raw`: inject frames (`docs/websocket.md:249-250`)
- `esphome/tado_sniffer`: check the RF side

## Conflicts between tanoclo docs and code

| Topic | Docs | Code |
| :--- | :--- | :--- |
| Auth reply | 2.01 (`docs/websocket.md:58,62`) | 2.05 (`ws-server/lib/handlers.js:154,309`) |
| Option 7 | Location-Path (`docs/coap_tlv.md:101`) | `OPT_URI_PORT` (`ws-server/lib/coap.js:44`) |
| Accept | Option 6 (`docs/coap_debug_endpoints.md:23`) | 6 and 17 both used (`ws-server/lib/commands/device.js:680`, `ws-server/lib/coap.js:49`) |
| Time broadcast | `d/time` (`docs/websocket.md:319`) | No URI (`ws-server/server.js:226-232`) |
| Pair FIDs | `0x01fc`, `0x0104`, `0x0201` (`docs/coap_tlv.md:255,303,307`) | 1-byte tags (`ws-server/lib/commands/device.js:379`) |
| `0x016a` | Calibration state (`docs/coap_tlv.md:276`) | RF channel (`patch_ib_firmware/extract_channel.js:56`) |
| Token | "Validated on every payload" (`docs/coap_tlv.md:108`) | Mismatch only logged (`ws-server/lib/message-router/index.js:461-467`) |

Because compatibility with tanoclo is the goal, **code wins** over its docs for these.
