# tado-bridge-firmware

Replacement firmware for the tado Internet Bridge (IB01): STM32F411 + CC110L radio + ENC28J60 Ethernet + MX25R1635F flash. It talks to a self-hosted server ([tanoclo](https://github.com/tanoclo/tanoclo)) instead of the tado cloud.

## Start here

- `docs/README.md`: index of the design notes and every project decision.
- `docs/open-questions.md`: what is still open, with a status summary at the top.
- `docs/test-bench.md`: bench setup, what Claude may do on hardware, and how the stock network is protected.
- `docs/prototype-plan.md`: stages P0-P7. Each stage is a GitHub milestone, and its tasks are issues (`gh issue list`).
- Source citations such as `freedo/...`, `tanoclo/...`, `Tado_Hardware_Teardown.wiki/...` refer to sibling folders of this repo (`../freedo`, `../tanoclo`, ...).

## Hard rules

- **Never commit firmware, flash dumps, captures or keys.** A dump of a real bridge holds its network key. `.gitignore`, `tools/check-no-firmware.sh` and the `no-firmware` CI job enforce this. The pre-commit hook needs `git config core.hooksPath .githooks` once per clone. Never use `git add -f` or `--no-verify` to get around them.
- **This repo is public.** Check content before every push. Backups of the bridge belong outside any git repository, for example `~/ib01-backups/`.
- **Bench permission (owner, 2026-11-11):** Claude may flash and test the bench bridge (ST-Link + Ethernet + RTL-SDR), within the gates in `docs/test-bench.md`: P0 done first, never change option bytes without asking, verify every write, and get a `fw-reviewer` "safe to flash" verdict before radio-transmit (P5) or bootloader (P7) code.
- **The owner's stock tado network runs nearby on channel 26 (868.3237 MHz), the protected channel.** The test network uses channel 10 (865.1245 MHz). Never transmit on channel 26, never ACK or answer frames not addressed to our EUI-64, pair only on the test channel, and keep SDR captures out of git. Details: `docs/test-bench.md`.
- **Never drive the HomeKit coprocessor's I2C bus** (MFI337S3959, probably PB6/PB7). HomeKit is out of scope, and the chip must stay exactly as shipped.
- **IB01 safety:** power over USB with the ST-Link's 3V3 wire disconnected (owner decision, 2026-11-14: the ST-Link 3V3 sagged below the ENC28J60's minimum; never feed the board from two sources at once); never reconfigure PA13 or PA14 (SWD); no NRST on the test points, so flash with OpenOCD, not `st-flash --connect-under-reset`. Details: `docs/hardware.md`, `docs/prototype-plan.md`.

## Docs conventions

- One Markdown file per topic in `docs/`, listed in `docs/README.md`.
- Cite every factual claim with a path and line number. Tag claims **[V]** measured on hardware, **[D]** documented, **[I]** our inference.
- Record conflicts between sources instead of silently picking one. Record owner decisions in `docs/README.md` under "Project decisions".
- Update docs as findings arrive. Keep the stage table in the root `README.md` current.
- Close issues from commits (`Closes #N`) when a task is done.

## Delegate work to the matching subagent

Pick the subagent by the task, not by the stage. Each one has its own model and effort set in `.claude/agents/`. Delegate one issue or one coherent task per subagent call, and give it the issue number and the relevant docs. Do not switch models in the middle of a task.

| Task | Subagent | Model, effort |
| :--- | :--- | :--- |
| Project setup from `stm-template`, CMake, build scripts, simple GPIO (LEDs, button), routine code | `fw-routine` | Sonnet, medium |
| Clock tree, CC110L driver, ENC28J60 driver, lwIP port, mbedTLS config, entropy, storage records | `fw-driver` | Opus, high |
| freedo MAC and 6LoWPAN port, ACK timing, pairing, wake trains; bootloader, self-update, rollback | `fw-critical` | Fable, high |
| Review before anything is flashed, and of any change to bootloader, storage format or radio timing | `fw-reviewer` | Fable, high, read-only |
| RTL-SDR: channel scans, GFSK demodulation, frequency offset, ACK and beacon timing over the air | `rf-bench` | Opus, high |
| Reading long RTT logs, OpenOCD output, logic captures, size reports | `log-reader` | Haiku, low, read-only |
| Docs upkeep, issue and milestone updates, commit messages | `docs-keeper` | Sonnet, low |

The main session coordinates: it talks to the owner, splits the work, delegates, and checks the results. Anything the owner must do by hand (cabling, probing, pressing buttons) is asked of the owner. Flashing and testing on the bench are allowed within `docs/test-bench.md`.

Refusals from safety filters have come up on firmware-dumping and key-extraction wording. Frame tasks as what they are: interoperability work on the owner's own device and own network key.
