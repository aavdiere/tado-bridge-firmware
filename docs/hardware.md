# IB01 hardware

Paths are relative to the workspace root. `wiki/` means `Tado_Hardware_Teardown.wiki/`.

Status tags: **[V]** measured on real hardware by a repo author (nothing here has been measured by us yet), **[D]** stated in a repo or wiki, **[I]** our own inference.

## Components

| Part | Device | Bus | Status | Source |
| :--- | :--- | :--- | :--- | :--- |
| MCU | STM32F411CEU6, Cortex-M4 at 100 MHz, 512 KB flash, 128 KB RAM, 26 MHz crystal | n/a | [D] | `wiki/Inside-the-Internet-Bridge.md:11`, `tanoclo/docs/coap_debug_endpoints.md:13` |
| Radio | TI CC110L, 868 MHz, 26 MHz crystal | SPI | [V] | `wiki/CC110-Register-Configuration.md:38` |
| Storage | Macronix MX25R1635F, 2 MB NOR, marked `R16L 244BA` | SPI, shared with the radio | [V] | `wiki/Inside-the-Internet-Bridge.md:15`, `wiki/CC110-Register-Configuration.md:13` |
| Ethernet | ENC28J60-I/ML, 10BASE-T MAC and PHY, 25 MHz crystal | SPI | [D] | `wiki/Inside-the-Internet-Bridge.md:13` |
| HomeKit | MFI337S3959 | I2C | Out of scope | `wiki/Inside-the-Internet-Bridge.md:14` |
| Other | LEDs, pair and reset switches, Arm SWD, USB micro (power only) | GPIO | [D] | `Tado_Hardware_Teardown/img/Internet_bridge/Tado Internet Bridge - Block Diagram.png` |

## Pin map so far

The wiki gives package pin numbers (`wiki/CC110-Register-Configuration.md:17-28`). The GPIO names below are confirmed from the UFQFPN48 pin table in `docs/datasheets/stm32f411ce.txt:2225-2722` (ST DS10314 Rev 8). Pins 15, 16, 17 and 46 also give exactly the flash pins that `tanoclo/patch_ib_firmware/spi_stub.c:113,129` drives.

| Signal | CC110L pin | STM32 pin | GPIO |
| :--- | :--- | :--- | :--- |
| SCK | 1 | 15 | PA5 |
| MISO | 2 | 16 | PA6 |
| MOSI | 20 | 17 | PA7 |
| Radio CS | 7 | 14 | PA4 |
| GDO0 | 6 | 10 | PA0 |
| GDO2 | 3 | 11 | PA1 |
| NRST | n/a | 7 | NRST |
| Flash CS | n/a | 46 (marked "?" in the wiki) | PB9 ([D] in `spi_stub.c:129`) |

PA5/6/7 carry SPI1 SCK, MISO and MOSI (`docs/datasheets/stm32f411ce.txt:2301-2313`), so radio and flash share one bus **[D]**. Any flash access therefore blocks radio FIFO service. SPI1 runs at up to 50 Mbit/s, SPI2 and SPI3 at up to 25 Mbit/s (`stm32f411ce.txt:1594-1595`).

**ENC28J60 is on SPI2, on its own bus**, confirmed by continuity (2026-11-11). ENC28J60 QFN pin to STM32 package pin: CS 5 to 25 (PB12), SCK 4 to 26 (PB13), SO 2 to 27 (PB14, MISO), SI 3 to 28 (PB15, MOSI) (`datasheets/enc28j60.txt:257-263`, `stm32f411ce.txt:2433-2455`).

SPI2 runs at up to 25 Mbit/s and the ENC28J60 at up to 20 MHz (`datasheets/enc28j60.txt:17`). SPI2 divides down from its bus clock, so expect about 12.5 MHz with a 50 MHz APB1 **[I]**.

The MCU has a 96-bit unique ID and 16 OTP blocks of 32 bytes (`stm32f411ce.txt:45,891`).

**PA3** (pin 13) is unconnected, reported by the user; used as the ADC entropy source (`design.md`).

**ENC28J60 INT is on PB2** (package pin 20, from ENC28J60 QFN pin 28), confirmed by continuity (2026-11-11). It maps to EXTI2, separate from the radio's EXTI0 and EXTI1 **[I]**. PB2 doubles as BOOT1 on this family (from memory; the manual only says BOOT1 is shared with a GPIO, `datasheets/rm0383.txt:1873-1874`). After reset the pin is free for normal use, and BOOT1 matters only when BOOT0 is high.

**BOOT0 has a pull-down to ground**, reported by the user (2026-11-09). The MCU therefore always boots from main flash, and whatever is on BOOT1/PB2 at reset has no effect **[I]**. ST's ROM bootloader cannot be entered without pulling BOOT0 high by hand, so SWD is the recovery route.

**ENC28J60 RESET is on PC14** (package pin 3, from ENC28J60 QFN pin 6), confirmed by continuity (2026-11-11). The chip's RESET input is active-low with an internal weak pull-up (`datasheets/enc28j60.txt:263,288`), so the chip stays out of reset while PC14 is an input. PC14 is one of the current-limited pins (3 mA, `stm32f411ce.txt:2734`); it is fine for a reset line. Drive it low to reset and otherwise release it. Since PC14 is used as a GPIO, the 32 kHz crystal function is unused **[I]**, matching the block diagram, which shows only the 26 MHz crystal. The chip also has a software reset command (`enc28j60.txt:1587`).

**LEDs**, confirmed by continuity (2026-11-11). Each runs from its MCU pin through the LED to ground, so **driving the pin high turns it on**.

| LED | GPIO | Package pin |
| :--- | :--- | :--- |
| Power | PB1 | 19 |
| Router | PB0 | 18 |
| Cloud | PB5 | 41 |
| Pairing | PB8 | 45 |

The earlier guess of PA8 for the cloud LED was wrong; PA8 is not accounted for.

**Factory reset button is on PB10** (package pin 21), confirmed by continuity (2026-11-11). The button connects PB10 to ground, so it is **active low: pressed reads 0** (2026-11-11). Enable the internal pull-up; it is harmless if the board also has an external one **[I]**.

**Pair button is on PC15** (package pin 4), **active high: pressed reads 1** [V], found 2026-11-11 with `tools/bench/io_check.tcl`: three presses gave three clean 0-1-0 transitions on PC15 and nothing else, with the internal pull-down on. On the board the button switches 3V3 through a diode next to a 3-pin IC marked `AHt`; that IC's role is unknown and not needed for firmware. PC15 is a current-limited pin (`stm32f411ce.txt:2734`), which is fine for an input.

**Bench I/O check (2026-11-11) [V]:** all four LEDs lit in the expected order when their pins were driven high; factory reset PB10 gave two clean 1-0-1 presses with the internal pull-up. At rest PB2 (ENC28J60 INT) reads 1, idle for an active-low interrupt, and PB6 and PB7 read 1 despite internal pull-downs, which suggests external I2C pull-ups for the HomeKit chip **[I]**.

**Firmware use (#8)** [I]: router LED mirrors the reset button, pairing LED mirrors the pair button, power LED is the heartbeat and the cloud LED a placeholder blink; these roles are P1 test wiring, not the final LED behaviour. The firmware sets the debug freeze bit for the IWDG (`build.md`, P1) and touches no other pin.

**Pin map complete.** Every pin the firmware needs is known.

**HomeKit coprocessor (MFI337S3959): leave untouched.** Owner question, 2026-11-11: does it need a backup? The stock firmware's HomeKit data (pairings, setup code, accessory identity) can only live in the bridge's flash, which the P0 backup covers **[I]**. The coprocessor holds a factory certificate and a private key that cannot be read out by design; the board cannot write either **[I]** (from general knowledge of Apple MFi authentication chips, not checked against a datasheet). So it needs no backup, and the firmware must simply never drive its I2C bus (probably PB6/PB7, from the external pull-ups seen there; not verified). A restore of the backup then brings back the stock firmware with HomeKit intact. Reading the coprocessor's certificate is possible later if HomeKit is ever wanted, as a separate task.

## Clock tree (#7) [I]

Chosen in `src/clock.c`; not yet measured on the bench. All limits below are also checked by `_Static_assert`s in that file.

| Item | Value | Limit and source |
| :--- | :--- | :--- |
| HSE | 26 MHz crystal, HSEBYP = 0 (crystal per Components table above) | 4-26 MHz (`datasheets/stm32f411ce.txt:4848`) |
| PLL input | HSE / PLLM 13 = 2 MHz | PLLM 2-63, VCO input 1-2 MHz, 2 MHz recommended (`datasheets/rm0383.txt:5313-5316`) |
| VCO | 2 MHz x PLLN 96 = 192 MHz | PLLN 50-432 (`rm0383.txt:5293`), VCO 100-432 MHz (`rm0383.txt:5290-5291`, `stm32f411ce.txt:5096`) |
| SYSCLK = HCLK | VCO / PLLP 2 = 96 MHz | PLLP 2/4/6/8, at most 100 MHz (`rm0383.txt:5280-5285`) |
| PLL48CK | VCO / PLLQ 4 = 48 MHz (unused; USB is power only) | PLLQ 2-15 (`rm0383.txt:5253`) |
| APB1 | HCLK / 2 = 48 MHz; APB1 timers 96 MHz | at most 50 MHz (`rm0383.txt:5431`); timers x2 when the prescaler is not 1 (`rm0383.txt:4760-4763`) |
| APB2 | HCLK / 1 = 96 MHz | at most 100 MHz (`rm0383.txt:5421`) |
| Regulator | Scale 1 (VOS = 11), wait for VOSRDY | needed above 84 MHz (`rm0383.txt:4254-4263`) |
| Flash | 3 wait states, prefetch, I- and D-cache on | 3 WS for 90-100 MHz at 2.7-3.6 V and for 72-96 MHz at 2.4-2.7 V (`rm0383.txt:2067`) |

**Why 96 and not 100 MHz:** 96 MHz gives an exact 48 MHz PLL48CK, keeps APB1 below its 50 MHz limit instead of on it, needs 3 wait states down to 2.4 V where 100 MHz needs 4 (`rm0383.txt:2067-2068`), and the same PLLN/P/Q work from the 16 MHz HSI with PLLM = 8, so the fallback runs at the same frequencies.

**Bus clocks for drivers:** SPI1 (radio, flash) runs from APB2 at 96 MHz, so its prescaler gives 48, 24, 12, 6, 3 MHz...; SPI2 (ENC28J60) runs from APB1 at 48 MHz, giving 24, 12, 6 MHz..., so 12 MHz is the fastest setting under the ENC28J60's 20 MHz limit (`datasheets/enc28j60.txt:17`). `clock_apb1_hz` and `clock_apb2_hz` hold the values, and libopencm3's `rcc_apb1_frequency`/`rcc_apb2_frequency` are set to match.

**Fallback:** HSE gets 100 ms to report ready (typical start-up is 2 ms, no maximum given, `stm32f411ce.txt:4859-4862`); PLL lock, VOSRDY and the clock switch get 5 ms each (lock is at most 300 µs, `stm32f411ce.txt:5097-5099`). If any step fails, HSE is switched off and the PLL is tried from HSI at the same 96 MHz; if that fails too, the MCU stays on the 16 MHz HSI. `clock_init()` never hangs. The source and the failed step are logged over RTT at boot. On HSI the core clock is accurate to ±1% at 25 °C and ±4% from -10 to 85 °C (`stm32f411ce.txt:4972-4977`), which matters for radio timing done by MCU timers; the radio and Ethernet chips have their own crystals.

**Clock security system stays off:** with CSS on, an HSE failure raises an NMI that repeats until the handler clears CSSC (`rm0383.txt:4951-4955`). Without it, what the PLL does after a mid-run HSE loss is not documented; the plan is that the LSI-clocked independent watchdog (#8) resets a stalled core and the next boot takes the HSI fallback. A PLL that slows down without stalling would not be caught. MCO_1 on PA8 (`stm32f411ce.txt:2802`) is not configured; the pin stays as it is.

**Expected registers after boot (HSE path), for the bench check:** `RCC_PLLCFGR` 0x2440180D, `RCC_CFGR` 0x0000100A, `FLASH_ACR` 0x00000703, `PWR_CR` VOS bits 15:14 = 11 (0xC000 if nothing else is set). The boot log prints these.

## RF path

Differential CC110L output, balun, insert-to-break test connector, three-element match, TI helical antenna (`wiki/RF-Hardware.md:1-12`). The test connector is fitted backwards on the stock board: a probe disconnects the CC110L and attaches to the antenna (`wiki/RF-Hardware.md:15`). Refitting it the correct way with an external dipole improved range by about 50% **[V]** (`wiki/RF-Hardware.md:30-34`).

## Handling

- Never power the bridge from USB or Ethernet with the programmer attached; the double supply can brick it (`tanoclo/patch_ib_firmware/README.md:211`).
- The case is clips only, no screws (`tanoclo/patch_ib_firmware/README.md:113`).

## Tooling

`arm-none-eabi-gcc` and `openocd` are installed locally. Saleae Logic is not.

## Full backup [D]

Tooling: `tanoclo/patch_ib_firmware/read.sh`, with ST-Link V2 and OpenOCD. Guide: `tanoclo/patch_ib_firmware/README.md` sections 5 and 6.1.

**SWD test points** (10-pin 1.27 mm cluster; single hole left, double holes right, `README.md:131-158`): pin 1 3V3, pin 5 GND, pin 7 SWDIO, pin 8 SWCLK.

**It is a Tag-Connect TC2050 legged footprint [D]:** 2 x 5 pads at 1.27 mm, one alignment hole at one end and two at the other, four leg latch holes (Tag-Connect TC2050-IDC-FP drawing, sheet 1; board photo `tanoclo/patch_ib_firmware/images/ib_pcb_overview.jpg`). Seen from the pad side with the single hole on the left, pad 1 is bottom-left, pads 1-5 run left to right on the bottom row, and pads 6-10 run right to left on the top row. The plug is keyed by the alignment pins. The TC2050-IDC cable marks pin 1 with the red wire at both ends, and its IDC end view numbers pins 1-10 (Tag-Connect TC2050-IDC drawing, sheet 2). That pad n goes to IDC pin n is **[I]**: check it with a continuity test before first use.

| Tag-Connect pad / IDC pin | Signal | tanoclo position | STM32 pin | ST-Link V2 dongle pin (tanoclo table) |
| :--- | :--- | :--- | :--- | :--- |
| 10 | 3V3 | 1 | VDD | 7 (3.3V) |
| 6 | GND | 5 | VSS | 5 (GND) |
| 2 | SWDIO | 7 | 34 (PA13) | 4 (SWDIO) |
| 3 | SWCLK | 8 | 37 (PA14) | 2 (SWCLK) |
| 1, 4, 5, 7, 8, 9 | not connected | 2, 3, 4, 6, 9, 10 | | leave open |

**Not the ARM Cortex 10-pin layout [I]:** SWDIO is on pad 2 as in that layout, but SWCLK is on a pad the ARM layout uses for GND, and 3V3 is on the pad it uses for nRESET. A standard TC2050 to ARM-10 adapter would therefore short SWCLK to ground and put 3V3 on the debugger's reset line. Wire the four signals individually.

**Before powering, with the board unpowered, check by continuity through the cable:** IDC pin 2 to STM32 pin 34, IDC pin 3 to STM32 pin 37, IDC pin 6 to ground (e.g. the Ethernet jack shield), and IDC pin 10 to the 3V3 rail (STM32 pin 24, 36 or 48). Only connect the ST-Link after all four agree.

**Power from the ST-Link 3V3 only**; no USB, no Ethernet (`README.md:211,247`).

**Outputs:** `unmodded.bin` (512 KiB internal flash) and `unmodded_spi.bin` (2 MiB external flash). External reads are done twice per chunk (`README.md:232`).

**Verify:** dump twice and compare (`README.md:238-242`). Move the first set aside first, because `read.sh` overwrites its outputs. Non-zero reliability counters mean re-seat the clip. `SPI_SLOW=1` helps diagnose marginal contact (`README.md:244`).

**Do not run `read_patch_flash.sh` for a backup:** by default it goes on to patch and flash (`README.md:228`). `read.sh` only reads.

**Not captured by `read.sh`** **[I]**: option bytes, OTP area, and the HomeKit chip's contents. The dump only reads main flash (`dump_internal_flash.tcl:14`). `docs/tools/read_ib01_system_areas.tcl` reads the option bytes, OTP and unique ID; it is read-only and untested on hardware. Its addresses are checked against ST RM0383 (`datasheets/rm0383.txt:2035-2036,46711,46766`).

Restore path: the `-Revert` option of the patch scripts flashes `original/unmodded.bin` and `original/unmodded_spi.bin` (`README.md:259`). Check the bash options at `README.md:266+` before relying on it.

Dumps contain device secrets, including the network key. Keep them **outside any git repository**, for example in `~/ib01-backups/`, and never commit them (see "Never commit firmware" in the root `README.md`).

## P0 results on the bench unit (2026-11-11) [V]

Device-specific values (serial, EUI-64, hashes) are kept in the private backup folder, not in this public repo.

| Item | Result |
| :--- | :--- |
| SWD via TC2050 wiring | Works; ST-Link V2 (J29S7), target voltage 3.20 V |
| Chip | `DBGMCU_IDCODE` 0x10006431: STM32F411, 512 KB flash |
| Option bytes | `0x5510AAEF`, `0x80007FFF`: read protection level 0, no sector write protection, software watchdog, brown-out reset off |
| OTP | Blank: 0 of 528 bytes programmed, no blocks locked |
| Backup | `read.sh` run twice: internal 512 KB and external 2 MB identical between runs, zero stub errors or read mismatches |
| Identity | Config marker `42 49 00 00 54 46` at internal offset 0x4006. EUI-64 starts with tado OUI `00:1B:C5:07`; reversed on air it ends in the network suffix `31 07 c5 1b 00` (`radio.md`) |

Restore rehearsal: done. Internal flash written from the backup with `flash write_image erase` and `verify_image` (512 KB in 15 s, verified), external flash written through tanoclo's RAM stub (2 MB, about 90 s, completed), then read back with `read.sh`: both chips byte-identical to the backup. The way back to stock firmware is proven.

## Not reviewed

The flash memory map and the patch procedures were not reviewed. Pointer: `tanoclo/patch_ib_firmware/README.md`.
