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

**Pair button** (2026-11-11, partial): one side goes to 3V3, the other to a diode next to a 3-pin IC marked `AHt`. Where the diode and that IC lead is not yet traced, so the MCU pin is unknown. With the button switching 3V3, the MCU most likely sees it as **active high** **[I]**.

**Not yet known:** pair button MCU pin; what the `AHt` IC is. See `open-questions.md` #6.

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

**Power from the ST-Link 3V3 only**; no USB, no Ethernet (`README.md:211,247`).

**Outputs:** `unmodded.bin` (512 KiB internal flash) and `unmodded_spi.bin` (2 MiB external flash). External reads are done twice per chunk (`README.md:232`).

**Verify:** dump twice and compare (`README.md:238-242`). Move the first set aside first, because `read.sh` overwrites its outputs. Non-zero reliability counters mean re-seat the clip. `SPI_SLOW=1` helps diagnose marginal contact (`README.md:244`).

**Do not run `read_patch_flash.sh` for a backup:** by default it goes on to patch and flash (`README.md:228`). `read.sh` only reads.

**Not captured by `read.sh`** **[I]**: option bytes, OTP area, and the HomeKit chip's contents. The dump only reads main flash (`dump_internal_flash.tcl:14`). `docs/tools/read_ib01_system_areas.tcl` reads the option bytes, OTP and unique ID; it is read-only and untested on hardware. Its addresses are checked against ST RM0383 (`datasheets/rm0383.txt:2035-2036,46711,46766`).

Restore path: the `-Revert` option of the patch scripts flashes `original/unmodded.bin` and `original/unmodded_spi.bin` (`README.md:259`). Check the bash options at `README.md:266+` before relying on it.

Dumps contain device secrets, including the network key. Keep them **outside any git repository**, for example in `~/ib01-backups/`, and never commit them (see "Never commit firmware" in the root `README.md`).

## Not reviewed

The flash memory map and the patch procedures were not reviewed. Pointer: `tanoclo/patch_ib_firmware/README.md`.
