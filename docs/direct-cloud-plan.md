# Plan: build option to talk directly to the tado cloud

## Context
The firmware connects only to tanoclo (`wss://<NET_SERVER_HOST>:988/hw/v2`). The owner wants an additional **build option** that makes it connect to tado's cloud (`wss://ingress.tado.com/hw/v2`, probably port 443) instead. tanoclo already has a transparent proxy to that endpoint (`tanoclo/ws-server/lib/proxy-manager.js:197-202`), but nobody has observed what the real cloud expects from a bridge (`docs/open-questions.md` section C, rows 10-14). So the plan starts with a capture and only then changes firmware.

Decisions (owner, this session): selection is a **build option**, no storage format change; **capture first**.

## Known gaps in the current firmware (from exploration)
1. **SNI is off** (`src/tado_mbedtls_config.h:98-100`). A public cloud host very likely needs it.
2. **One cipher suite, one curve**: ECDHE-ECDSA-AES128-GCM-SHA256, secp256r1, SHA-256 only (`src/tado_mbedtls_config.h:51,79`). tado's PKI is probably compatible (tanoclo's clone chain is ECDSA P-256, `tanoclo/patch_ib_firmware/clone_chain.sh`), but this is unverified.
3. **Single trust anchor**: `PROVISION_CA_PEM` from `SERVER_CA_PEM` (`cmake/CMakeLists.txt:146-158`, `src/tls.c:184-224`). The real tado root is not in either repo; it must come from the owner's own stock flash backup (`tanoclo/patch_ib_firmware/README.md:321`, offset 0x59FF0, 639 bytes) or from the capture.
4. **Server certificate validity is not checked** (no `MBEDTLS_HAVE_TIME`, no clock before first connect). Acceptable for tanoclo, a weaker position against a real public server. Note it, do not fix now.
5. **Port**: `ws.c` sends `Host: <host>` without port; the real endpoint is probably 443 (`patch_endpoint.sh` uses `ws://ingress.tado.com:443`, unverified).
6. **Our extensions the cloud has never seen**: `b/fw` (our own update), `POST found` (guessed URI), no nonce on `auth/token`, `field_a/b/c` zero on uplink. The real cloud also pushes firmware (`d/fw`, `fw/rq`, `fw/state`) that tanoclo's proxy blocks (`proxy-manager.js:274-306`); direct mode would receive them unfiltered.
7. **RAM margin**: TLS handshake peak leaves about 9.5 KB free (`docs/design.md:446`). A longer chain, RSA or a second curve could break it.
8. **No host tests** for `tls.c`, `ws.c`, `net.c`, `session.c` state machine; only the pulled-out parts (`test_coap.c`, `test_pair_*`, `test_fwupd.c`) are tested.

## Stages

### Stage 0: capture through tanoclo proxy (no firmware change)
- Obtain tado root CA from the owner's own P0 backup (stays outside git, e.g. `~/ib01-backups/`), configure `TADO_ROOT_CA_PATH` for tanoclo.
- Enable proxy for the bench home with logging (`tanoclo/QUICKSTART.md:201-213`); bridge stays on the **test network, channel 16**; never the stock network.
- Record: what the cloud answers to `auth/key`, `auth/token`, `GET time`; what it pushes down (`d/fw`, `fw/rq`, ETags); the uplink `field_a/b/c`; keepalive timing; whether our `b/fw` and `found` cause trouble; the TLS facts (cipher suite, curve, signature algorithm, whether SNI is required, chain length).
- This sends the bench bridge's serial to tado's cloud. Needs the owner's explicit go-ahead before running. Delegate log reading to `log-reader`; results go in `docs/open-questions.md` (rows 10-14) and `docs/server-api.md`.
- **Gate**: if the cloud rejects a serial-only login, the remaining stages stop and the owner decides.

### Stage 1: TLS and WebSocket changes (`fw-driver`)
- New cmake option `SERVER_MODE` (`tanoclo` default, `tado`) in `cmake/CMakeLists.txt` next to `NET_SERVER_HOST` (line 21). `tado` sets host `ingress.tado.com`, port 443 (values confirmed by Stage 0) and requires `SERVER_CA_PEM` to be the tado root (fail the configure otherwise).
- SNI: enable `MBEDTLS_SSL_SERVER_NAME_INDICATION` only if Stage 0 shows it is needed; in `src/tls.c` send the host name only in `tado` mode, so tanoclo mode behaves exactly as now.
- `src/ws.c:154-170`: Host header with port when it is not 443, any extra headers Stage 0 shows the cloud needs. Keep HTTP/1.1.
- mbedTLS config: add only what Stage 0 proves necessary (a suite, a curve, SHA-384). Re-measure the handshake heap peak and flash size after each addition and record it in `docs/design.md`/`docs/build.md`.
- Reuse: `tls_setup`/`tls_open` (`src/tls.c`), `ws_open` (`src/ws.c:379-396`), `cmake/CMakeLists.txt:355-356` define pattern, `build/generated/provision.h` flow.

### Stage 2: session behaviour in `tado` mode (`fw-critical` for anything touching radio or update, `fw-driver` otherwise)
- `src/session.c`: wrap the tanoclo-specific parts in the mode: the give-up text at `:357` and mismatch log at `:1030`; nonce on `auth/token` if the cloud needs it; uplink `field_a/b/c` values if they matter; `GET time` and `auth/key` handling per the capture.
- **Safety in direct mode**: refuse everything that changes the bridge from the cloud side. The cloud's `d/fw`, `fw/rq` and similar are ignored (no answer or a CoAP refusal, as the capture shows is safe); our `b/fw` handler is compiled out or refused, so only a local build and OpenOCD flash updates it. Do not send `POST found` unless Stage 0 gives the real URI. Keep `d/rfkey` unanswered.
- Radio rules are unchanged: only the test channel, never answer frames not for our EUI-64. Cloud downlinks addressed to stock devices are not relayed to the radio in this mode (decision to confirm after Stage 0).
- Print no serial, key or token in logs beyond what the code logs now; commit nothing from the capture.

### Stage 3: tests and review
- Host tests: extend `tests/host/test_coap.c` with the byte vectors captured in Stage 0 (auth, time, blocked paths); add a host test for the direct-mode filter (new small file, pattern of `test_pair_remote.c`) by moving the decision logic out of `session.c` as was done for pairing and `fwupd`. `session.c`, `tls.c`, `ws.c` stay bench-tested.
- `fw-reviewer` verdict "safe to flash" is required before flashing (CLAUDE.md: every default image counts as radio-transmit code). A default build (`SERVER_MODE=tanoclo`) must stay byte-identical in behaviour; compare the size report and the P4 bench session against tanoclo as a regression check.

### Stage 4: bench verification (owner go-ahead, owner does cabling)
- Flash the `tado` build with OpenOCD (P0 backup ready, verify every write, ST-Link 3V3 disconnected, no option-byte changes).
- Expected result: TLS up, auth and time OK, session READY against the real cloud for the bench serial, keepalive stable for 30+ minutes, no firmware push accepted, nothing transmitted on channel 26.
- Then flash the tanoclo build back and confirm the regression run.

### Stage 5: docs
- `docs/server-api.md` (cloud behaviour with [V] tags), `docs/build.md` (the option), `docs/design.md` (sizes), `docs/open-questions.md` (rows 10-14), `docs/README.md` project decisions (owner: build option, capture first), root `README.md` stage table, GitHub issue and milestone per stage; commits with `Closes #N`. Use `docs-keeper`.

## Critical files
`cmake/CMakeLists.txt`, `config/local.cmake.example`, `src/tls.c`, `src/ws.c`, `src/session.c`, `src/net.c`, `src/tado_mbedtls_config.h`, `tests/host/test_coap.c`; sibling `tanoclo/ws-server/lib/proxy-manager.js` (read only).

## Verification
1. Stage 0 capture log answers rows 10-14 and decides the go/no-go gate.
2. `make` default build: size and behaviour unchanged; host tests pass (`make test` or the repo's equivalent, check `docs/build.md`); `tools/check-no-firmware.sh` passes before any push.
3. `make SERVER_MODE=tado SERVER_CA_PEM=...`: builds; handshake heap peak and flash size recorded and within the budget (about 9.5 KB heap margin, 297 KB flash).
4. Bench run per Stage 4, with log reading by `log-reader` and a `fw-reviewer` verdict first.

## Risks
- The real cloud may reject a non-stock client or flag the account/serial. Stage 0 limits this to one bench serial.
- The serial goes to a third party; keep it out of git and public logs.
- OTA pushes from the cloud could brick the unit if mishandled; Stage 2 blocks them by design.
- No certificate-date check in direct mode (known, documented).
