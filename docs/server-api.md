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
| `POST d/I/IB0000FT0100<digits>/pair` | Open or close pairing; `dur` 0 closes | `ws-server/lib/commands/device.js:378-387` |
| `GET d/rfkey` | Network key | `ws-server/lib/commands/device.js:475-488` |
| `PUT d/config` | Bridge configuration | `ws-server/lib/commands/device.js:795-824` |
| `POST d/reboot`, `PUT d/identify`, `GET d/info` | Control; responses unspecified | `ws-server/lib/commands/device.js:269,287` |
| Time broadcast | NON 2.05 every 4 h, same 5 bytes as `GET time` | `ws-server/server.js:220-246` |

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
