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
| `POST found` | Joiner report after a pairing (#41): the new device's IPv6 address (`05 10` + 16 bytes); reply 2.05 with `06 10` + factory key, or 2.03. Stock URI unknown; tanoclo routes any path with a segment `found`. **Sent since #41** (below, "Joiner report") | `ws-server/lib/handlers/path-classifier.js:59`, `ws-server/lib/handlers.js:158-212` |

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

**Answers to the server's confirmable requests.** The server resends a CON downlink 6, 13, 26, 50 and 90 s apart (6, 19, 45, 95 and 185 s after the first send) until an ACK with its Message ID comes back from the bridge; it matches on the Message ID alone and reads neither the code nor the token (`ws-server/lib/coap-transport.js:35,195-265`, `ws-server/lib/message-router/index.js:197,542-549`) **[D]**. Its own answer to a PUT is a piggybacked 2.04 with the request's token and no options (`ws-server/lib/handlers/coap-helpers.js:24-49`, `ws-server/lib/coap.js:341-351`) **[D]**. The bridge answers `PUT d/config` the same way: ACK, 2.04, Message ID and token echoed, no options, no payload; the push is logged and not stored, since the bridge keeps no device table of its own (`src/session.c`, `on_config()`; host test `tests/host/test_coap.c` against bytes from tanoclo's own codec). Stateless, so a resent copy gets the same answer. What stock answers, and what it does with the push, is not known **[I]**. `b/fw` is answered as RFC 7959 says: 2.31 with the Block1 echo after each block is in flash, 2.04 after the last, 4.xx or 5.xx with a diagnostic text (`src/session.c`, `on_fw()`) **[I]**. Every other request for the bridge (`d/rfkey`, `d/reboot`, ...) is still only logged and gets no answer.

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
| Another request while one is in work (a close sent while an open is in work, say) | Nothing yet | **None**: no ACK, so tanoclo sends it again after its retry interval and the copy is acted on once the first is done (below) |
| The radio task does not take the request within 10 s | Nothing (a withdrawn request never runs) | 5.03 |

The answer is a piggybacked ACK with the request's Message ID and token, no options, no payload, as for `d/config`; tanoclo reads neither the code nor the token (above), so every answer stops its resends. For that reason a request that arrives while another is in work gets no ACK at all (a 5.03 would end its resends and the request would be lost; fw-reviewer L1 of #40): tanoclo sends the same frame again 6, 19, 45, 95 and 185 s after the first send (`RETRY_INTERVALS` 6, 13, 26, 50, 90 s, `ws-server/lib/coap-transport.js:35,195-234`, the same `wsFrame` at line 222) **[D]**, and a request is in work at most about 11 s (10 s unclaimed, then withdrawn; a claimed one at most about 0.8 s), so the copy at 19 s at the latest is acted on **[I]** (`src/pair_remote.h`, `pair_remote_sort()`; host test `test_sort()`). A copy of the request the radio task has is likewise not answered until it is done. A copy of one acted on, for 247 s (RFC 7252 EXCHANGE_LIFETIME; tanoclo's resends end after 185 s), gets the first answer again and nothing is done again, also over a new connection (tanoclo resends by bridge). So a late copy of an "open" never reopens a window a later "close" closed. A copy is recognised by Message ID, token and duration together, not by the Message ID alone: tanoclo starts its Message IDs at a random point when it starts (`coap-transport.js:44`) and gives each request a new random token of 1-8 bytes (`ws-server/lib/command-api.js:115-118`) **[D]**, so after a restart a new request may carry the ID of one in the cache; with another token or duration it is acted on (fw-reviewer L2 of #40; host tests `test_key()`, `test_seen()`). One RTT line per request: `session: POST pair (mid ..., dur ... s): window opened for 300 s on channel 16 ...: 2.04 sent`, `...: window closed: 2.04 sent`, `...: refused: <why>: 5.xx sent`; a copy of one acted on `session: POST pair resent, nothing done again (mid ...): <the first outcome, its code> sent`; a request while another is in work `session: POST pair (mid ..., dur ... s) while another is in work: no ACK, the server resends it`. The joiner report that follows a pairing: "Joiner report" below. What stock answers is not known.

**Joiner report, `POST found` (#41).** Owner decision 2026-11-20: in scope (`open-questions.md`, row 28a); the tanoclo half goes into the P9 pull request (#38).

What tanoclo does with it **[D]**:
- Routing: `found` is one of the flat routes (`ws-server/lib/handlers/path-classifier.js:59`). Any Uri-Path with a segment `found` and none of the segments tested before it (`auth`, `act`, `config`, `fw`, `fallback`, `s`, `overlay`, `hvac`, `path-classifier.js:76-201`) and none of the flat routes listed above it (`sen` ... `time`, `:46-58`) goes to `handlePairFound()` (`ws-server/lib/message-router/index.js:755-757`). The device of the request is the sender (the bridge), found from the frame header's address (`index.js:308-317`). Run against tanoclo's classifier: `found`, `pair/found` and `d/found` all give `pair_found` **[V, host]**.
- Payload: the handler takes the 16 bytes after the first `05 10` in the payload as the joiner's address (`handlers.js:177-185`): a one-byte tag and length, as in the pairing payloads (`pair` request `01 04 ...`, `d/pair` `12 10 ...`). It reads nothing else, not the method nor the options.
- Answer: it looks for a `devices` row with that `ipv6_address` (`handlers.js:189-194`). A row with a `factory_key`: piggybacked ACK 2.05 with `06 10` + the 16-byte key (`handlers.js:197-203`); otherwise ACK 2.03 with no payload (`handlers.js:207-211`). Both echo the Message ID and token (`ws-server/lib/coap.js:341-366`). A resent copy (same Message ID within the dedup window) gets the router's 2.04 instead and the handler does not run again (`index.js:646-653`, `lib/handlers/coap-helpers.js:24-49`).
- **It creates no row**: upstream tanoclo only reads `devices`. And it cannot from the report alone: the bridge knows the joiner's EUI-64 and address, not its serial, which is the `devices` key (`serial_no`, `ws-server/tanoclo.sql:23013-23014`). The serial first arrives in the device's own `POST auth/token` (TLV `0x0260`, `tanoclo/docs/pairing.md:54,107`; freedo: the valve "resumes with `auth/token`" after its 2.04, `freedo/docs/PROTOCOL.md:635-636`), which tanoclo drops without a row (`handlers.js:228-232`; row 28b) **[V]**. A device that was in this network before need not send it: on the bench (2026-11-21) the spare valve of #19 and #40 re-paired and went straight to telemetry, uplinks of 42, 113 and 101 bytes, no auth request, and tanoclo logged `Device VA… has no zone assignment` (`ws-server/lib/handlers/telemetry-handlers.js:138`) **[V]**.

What it means for stock **[I]**: tanoclo's own notes have the cloud hand the bridge the device's factory key so that the bridge can wrap the network key (`d/pair` tag `0x07`, `tanoclo/docs/pairing.md:11-14,41-42`; freedo: "a per-device KEK that only the cloud holds", `freedo/docs/PROTOCOL.md:631-633`). So on stock the report probably comes before `d/pair`, and the answer's key is what `d/pair` uses. We hand the key over in plaintext (tag `0x12`) and report after the 2.04, so the order is ours and the key in the answer is not used. Stock's URI, timing and payload are not observed.

**Registration: tanoclo patch `bench/found-register`** (local branch in `../tanoclo`, based on `bench/send-raw-waitack`, not pushed; for the P9 pull request; line numbers below are of that branch, worktree `../tanoclo-found-register`): `handlePairFound()` notes the joiner's address with the reporting bridge for 15 minutes, in memory (`ws-server/lib/joiner-registry.js`, new). **Rule (owner decision 2026-11-21, option B):** the device is registered at its first message, of any kind, that comes from that address over the same bridge's connection within the 15 minutes and carries a full serial that has no `devices` row. Where the serial comes from:
- `auth/key` and `auth/token`: TLV `0x0260`, in their handlers as before (`ws-server/lib/handlers.js:192,300`).
- Every other uplink request: the path's `d/<serial>` segment, as in telemetry `h/<home>/d/<serial>/sen` (`joiner-registry.js:61-67` `serialFromPath()`; the router calls `handlers.registerJoinerAtUplink()`, `handlers.js:156-168`, from `ws-server/lib/message-router/index.js:322-332`). The router then classifies the path again with the new serial, so the message itself is handled as if the row had existed (`index.js:328-330`).
- No full serial, so no registration; the report waits for a later message: `time`, `found`, `pair`, `identify`, zone, circuit, hvac and home paths, a `d/` path without a valid serial (`d/lock`, an `IB` serial), and every response (an ACK carries no path). TLV `0x0260` is not read from other messages.

The row: serial as sent (`VA`, `RU`, `BU`, `WR`, `SU`, `BP` or `BR` + 10 digits, as `createDevice()` accepts, `IB` excluded; `joiner-registry.js:48-53`), in the bridge's home (not the home in the device's path, which may be the home it was in before), `device_type` from the prefix as `createDevice()` derives it (`ws-server/api/routes/devices.js:394-400`), no zone, `ipv6_address` set, the other columns as `createDevice()` sets them (`devices.js:446-460`), the 25-device limit of a home kept. Guards: only the reporting bridge and the reported address; a serial that has a row is never overwritten (`handlers.js:161`, and `INSERT IGNORE`, `joiner-registry.js:149-152`); an address that another row already has registers nothing (`ipv6_taken`, `joiner-registry.js:134-136`, for both paths); not on a proxied connection (`index.js:322`); a database error registers nothing and the message goes on as before. Ordinary traffic costs one in-memory lookup: without a live report nothing else is done (`handlers.js:157`). Without a report, an unknown serial is handled (an auth request refused) as before. A tanoclo restart forgets the reports. Not changed: the whitelist check of `auth/token` (`handlers.js:316-324`), so with a non-empty `websocket_whitelist` the new device's serial or home must be in it. A device that gets its row from telemetry still logs `has no zone assignment` until it is put in a zone: the line to look for is `PAIR_FOUND: registered <serial> (<type>, home <id>, no zone) at <address> from /<path>, reported by bridge <bridge>`.

Trust **[I]**, for the pull request: any authenticated bridge can create a row in its own home for any serial not yet in `devices`, by reporting an address and then relaying a message from it whose path or TLV `0x0260` names that serial. Neither the path nor TLV `0x0260` is bound to the device by anything tanoclo checks; the guards above (own home, never overwrite, 15 minutes, home limit) bound what a misbehaving bridge can do, they do not prevent it.

Unit tests (vitest) **[V, host]**: `ws-server/test/test_joiner_registry.test.js` (21: the registry, `registerJoiner()`, `serialFromPath()`, and `handlePairFound()` followed by `registerJoinerAtUplink()` or `handleAuthKey()` with an in-memory `devices` table: registers from a telemetry path in the bridge's home, waits through messages without a serial, no database access without a report, never overwrites, other bridge, other address, address taken, database error) and `ws-server/test/test_joiner_router.test.js` (5: the real router with fakes: a registration classifies again and the handler gets the new row; auth requests, a proxied connection and a response do not call the hook). The full suite without a database: the same 15 files fail as before the change, all on `ECONNREFUSED 127.0.0.1:3306`.

What the bridge sends (`src/pair_found.c`, `src/session.c` `found_poll()`; host test `tests/host/test_pair_found.c` against bytes from tanoclo's codec) **[I]**:
- **When:** after the target's 2.04 to `d/pair` (`PAIRED`, `radio-runtime.md`, "Pairing (#19)"), once per pairing. The radio task posts the address in a queue of 4 (`src/link.c`); the net task sends it once the session is ready.
- **Request:** CON POST, token of 4 random bytes, Uri-Path `found`, option 2048 with the session token (as the bridge's other requests after `auth/token`), payload `05 10` + the joiner's IPv6 address: `fe80::` + its interface identifier, the very address its uplinks carry in the frame header (`src/relay.c`, `relay_uplink()`), which tanoclo keys it by. 44 bytes with option 2048. The frame header carries the bridge's own address, as every bridge request.
- **Retries:** RFC 7252: first timeout 2-3 s, doubled after each send, 5 sends at most (0, T, 3T, 7T, 15T), given up at 31T (62-93 s): the report is dropped and the log says to register the device by hand. Any answer with its Message ID and token ends it (2.xx, 4.xx, 5.xx, an empty ACK or a Reset); a refusal is not resent.
- **Without a session:** the report waits in the queue until the session is ready; a session that ends during the exchange only means a new send (new Message ID) on the next one. A report already queued for the same address is not queued again; a full queue drops the new report and logs it. Not kept over a restart of the bridge (RAM only, as the window and its key copy): a report lost that way means registering by hand, as before.
- **The answer's factory key** is not used, not stored and never logged.
- **RTT,** one line when sent and one for the answer: `session: POST found (mid ....): the device paired just now, its address: sent`, then `session: POST found (mid ....): answer 2.03 after N ms (send 1): taken` (2.05 with a key: `taken; the server has a factory key for the device (16 bytes, not used, not logged)`; 4.xx or 5.xx: `REFUSED: register the device in tanoclo by hand`). A resend: `... no answer in N ms: send 2 of 5 sent`; giving up: `... no answer after 5 sends in N s: dropped; register the device in tanoclo by hand`; a session's end: `... the session ended before an answer; sent again once the next one is ready`. No address is logged; tanoclo logs it (`PAIR_FOUND: Discovered device target IPv6 = ...`).

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

## Tado cloud, serial-only login (#44)

Stage 0 of `direct-cloud-plan.md` (2026-11-23, owner go-ahead; test channel 16 only, stock network untouched). The bench bridge, running our image, was put behind tanoclo's proxy mode (`tanoclo/ws-server/lib/proxy-manager.js:197-202`); raw logs stay in `~/ib01-captures/cloud0/`, outside git.

**Proxy mode needs no cloud account.** It only needs `homes.is_proxied` set, the tado root CA in `TADO_ROOT_CA_PATH` (as PEM) and the bridge's reconnect (`tanoclo/ws-server/lib/proxy-manager.js:171-198`). The root CA came from the owner's own P0 backup (offset 0x59FF0, 639 bytes, self-signed, valid 2019-2039); the cloud's chain verified against it **[V]**.

**Result [V].** Both attempts (one per bridge retry): TLS and WebSocket upgrade to the cloud succeeded, tanoclo forwarded `POST auth/key` with the bridge serial as the only TLV field, and **the cloud sent nothing** for the 10 s until our bridge closed (close code 1006 on the upstream side because we closed first). Nothing was pushed down either. Whether it would answer after more than 10 s, or closes an unknown serial silently, is unknown **[I]**. Gate reached: proxy mode was turned off. No `auth/token`, `GET time`, `d/fw`, `fw/rq`, keepalive, `field_a/b/c`, `b/fw` or `POST found` could be observed.

**TLS facts [V]** (`openssl s_client`, no bridge data sent):

| Item | Result |
| :--- | :--- |
| Port | 443 accepts TLS; port 988 gave no answer in 8 s |
| Version, suite | TLS 1.2 only (a TLS 1.3 offer fell back); ECDHE-ECDSA-AES128-GCM-SHA256 |
| Curve | prime256v1 accepted; P-384 and X25519 only offered: refused |
| Signature | ecdsa_secp256r1_sha256; both certificates ECDSA P-256 with ecdsa-with-SHA256 |
| Chain | 3 levels: leaf CN ingress.tado.com, "tado Ingress IntermediateCA", "tado RootCA"; the server sends 2 certificates (leaf and intermediate) |
| SNI | **Not required**: the same suite and chain with and without it, and by IP address |
| RSA suite | ECDHE-RSA-AES128-GCM-SHA256 refused |

So our fixed mbedTLS suite, curve and SHA-256 signature match the cloud **[I]**; the open points for stage 1 are only the chain size against the heap margin and the Host header.

**Retry with long wait [V] (2026-11-23, owner decision: same bench serial, wait much longer).** A host-side script (no bridge, no radio) opened `wss://ingress.tado.com:443/hw/v2` with `Sec-WebSocket-Protocol: binary` and the root CA from the P0 backup, and sent the same 61-byte `POST auth/key` frame the bridge had sent (taken from the tanoclo log of the first attempt). Raw log stays in `~/ib01-captures/cloud0/probe-*.log`, outside git.
- Upgrade answered 101 in 0.21 s; response headers `server: tado`, `sec-websocket-extensions: permessage-deflate;client_max_window_bits=15`; the cloud did not echo the `binary` subprotocol **[V]**.
- Run 1, no keepalive: connection held 190 s, **zero frames received**, no close from the cloud; we closed with 1000 cleanly **[V]**.
- Run 2, WebSocket ping every 20 s for 150 s: the cloud answered every ping with a pong within about 40 ms, still **zero data frames**, no close **[V]**.
- So the cloud accepts the connection and keeps it open but never answers a serial-only `auth/key`, within at least 190 s. It does not close an unknown or unauthenticated serial either; the connection just idles **[V]**. Why (serial unknown to the cloud, a credential missing in the frame, or the answer being sent elsewhere) is unknown **[I]**. The 10 s give-up of our bridge was therefore not the cause.
- **Gate result: STOP.** No `auth/token`, `GET time`, `d/fw`, `fw/rq`, keepalive timing or `field_a/b/c` could be observed. No other serial or credential was tried. The firmware's own WebSocket ping (30 s idle) would be answered by the cloud's server layer, so a direct-mode session would stay up, but with no CoAP answers it cannot reach `ready`.

**Three changed probes [V] (2026-11-23, owner-approved, host-side, bench unit's own serial, one fresh connection each, 120 s, WebSocket ping every 20 s).** Same 28-byte bridge header and TLV payload as above; only the CoAP options changed. Raw logs in `~/ib01-captures/cloud0/probe2-*.log`, outside git. Path shape from `tanoclo/ws-server/lib/handlers/path-classifier.js:72-84` (`auth` plus `key`, device id taken from the path).

| Probe | CoAP change | Result |
| :--- | :--- | :--- |
| A | option 12 (Content-Format) = 42 added | upgrade 101 in 0.2 s; 6 pings, 6 pongs; **0 data frames**; no close |
| B | Uri-Path `d/<serial>/auth/key` instead of `auth/key` | same: **0 data frames**, no close |
| C | A and B together | same: **0 data frames**, no close |

- In all three, the cloud kept the socket open, answered every ping with a pong in about 40 ms, and sent nothing, exactly as with the unchanged frame **[V]**. We closed with 1000 each time.
- None of the three variants changes the cloud's behaviour. Content-Format and a device path in the URI are therefore not what the cloud waits for **[I]**. What stays unknown: the serial is not known to or not active at the cloud, a credential or session token is missing from the frame (the stock frame may carry more than the 61 bytes our bridge sends), or the answer depends on the source (stock bridge identity, IP) **[I]**.
- **Gate result (second): STOP.** No other variant was tried (no other serial, header, HTTP/1.0 or SNI). `auth/token`, `GET time`, pushed commands, keepalive timing and `field_a/b/c` remain unobserved.
- **For stage 1:** a direct cloud mode cannot be developed against the cloud from a serial-only `auth/key`. Stage 1 can only proceed on the transport (TLS 1.2, WebSocket, ping keepalive: all verified) with the CoAP session logic unproven; the next evidence would have to come from a stock bridge's own first frames (needs the owner), not from more probes **[I]**.

**Bench side effects [V].** The first proxy start created a local state snapshot (empty, no cloud data). Bridge back on local tanoclo: `ready` with auth/token accepted. The bench override now mounts the four patched tanoclo files from `~/tanoclo-bench/prev-files/` (the found-register worktree was rebased onto upstream during the run and its new `handlers.js` needs a module the image lacks).
