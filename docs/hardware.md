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

SPI2 runs at up to 25 Mbit/s and the ENC28J60 at up to 20 MHz (`datasheets/enc28j60.txt:17`). The driver runs it at 6 MHz. The 12 MHz failures of 2026-11-13 were the supply sag, not the bus: with the rail in range, the firmware reads correctly at 12 and 6 MHz, and 6 MHz is kept for a factor of 2 on timing margin ("Result: margin test and 10-minute run" below) **[V]**.

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

Chosen in `src/clock.c`. **Verified on the bench 2026-11-11 [V]:** the owner confirmed a crystal on the board, and the first boot of the P1 image logged `from HSE 26 MHz crystal + PLL`, SYSCLK 96 MHz, HSE ready in under 1 ms, `RCC_PLLCFGR 2440180d`, `RCC_CFGR 0000100a`, `FLASH_ACR 00000703`, `PWR_CR 0000c000` (RTT log; not committed). All limits below are also checked by `_Static_assert`s in that file.

| Item | Value | Limit and source |
| :--- | :--- | :--- |
| HSE | 26 MHz crystal, HSEBYP = 0 (crystal confirmed by the owner and by HSE start-up on the bench [V]) | 4-26 MHz (`datasheets/stm32f411ce.txt:4848`) |
| PLL input | HSE / PLLM 13 = 2 MHz | PLLM 2-63, VCO input 1-2 MHz, 2 MHz recommended (`datasheets/rm0383.txt:5313-5316`) |
| VCO | 2 MHz x PLLN 96 = 192 MHz | PLLN 50-432 (`rm0383.txt:5293`), VCO 100-432 MHz (`rm0383.txt:5290-5291`, `stm32f411ce.txt:5096`) |
| SYSCLK = HCLK | VCO / PLLP 2 = 96 MHz | PLLP 2/4/6/8, at most 100 MHz (`rm0383.txt:5280-5285`) |
| PLL48CK | VCO / PLLQ 4 = 48 MHz (unused; USB is power only) | PLLQ 2-15 (`rm0383.txt:5253`) |
| APB1 | HCLK / 2 = 48 MHz; APB1 timers 96 MHz | at most 50 MHz (`rm0383.txt:5431`); timers x2 when the prescaler is not 1 (`rm0383.txt:4760-4763`) |
| APB2 | HCLK / 1 = 96 MHz | at most 100 MHz (`rm0383.txt:5421`) |
| Regulator | Scale 1 (VOS = 11), wait for VOSRDY | needed above 84 MHz (`rm0383.txt:4254-4263`) |
| Flash | 3 wait states, prefetch, I- and D-cache on | 3 WS for 90-100 MHz at 2.7-3.6 V and for 72-96 MHz at 2.4-2.7 V (`rm0383.txt:2067`) |

**Why 96 and not 100 MHz:** 96 MHz gives an exact 48 MHz PLL48CK, keeps APB1 below its 50 MHz limit instead of on it, needs 3 wait states down to 2.4 V where 100 MHz needs 4 (`rm0383.txt:2067-2068`), and the same PLLN/P/Q work from the 16 MHz HSI with PLLM = 8, so the fallback runs at the same frequencies.

**Bus clocks for drivers:** SPI1 (radio, flash) runs from APB2 at 96 MHz, so its prescaler gives 48, 24, 12, 6, 3 MHz...; SPI2 (ENC28J60) runs from APB1 at 48 MHz, giving 24, 12, 6 MHz..., so 12 MHz is the fastest setting under the ENC28J60's 20 MHz limit (`datasheets/enc28j60.txt:17`). The driver uses 6 MHz for timing margin ("SPI2: ENC28J60 bring-up", "Result: margin test and 10-minute run") **[V]**. `clock_apb1_hz` and `clock_apb2_hz` hold the values, and libopencm3's `rcc_apb1_frequency`/`rcc_apb2_frequency` are set to match.

**Fallback:** HSE gets 100 ms to report ready (typical start-up is 2 ms, no maximum given, `stm32f411ce.txt:4859-4862`); PLL lock, VOSRDY and the clock switch get 5 ms each (lock is at most 300 µs, `stm32f411ce.txt:5097-5099`). If any step fails, HSE is switched off and the PLL is tried from HSI at the same 96 MHz; if that fails too, the MCU stays on the 16 MHz HSI. `clock_init()` never hangs. The source and the failed step are logged over RTT at boot. On HSI the core clock is accurate to ±1% at 25 °C and ±4% from -10 to 85 °C (`stm32f411ce.txt:4972-4977`), which matters for radio timing done by MCU timers; the radio and Ethernet chips have their own crystals.

**Clock security system stays off:** with CSS on, an HSE failure raises an NMI that repeats until the handler clears CSSC (`rm0383.txt:4951-4955`). Without it, what the PLL does after a mid-run HSE loss is not documented; the plan is that the LSI-clocked independent watchdog (#8) resets a stalled core and the next boot takes the HSI fallback. A PLL that slows down without stalling would not be caught. MCO_1 on PA8 (`stm32f411ce.txt:2802`) is not configured; the pin stays as it is.

**Expected registers after boot (HSE path), for the bench check:** `RCC_PLLCFGR` 0x2440180D, `RCC_CFGR` 0x0000100A, `FLASH_ACR` 0x00000703, `PWR_CR` VOS bits 15:14 = 11 (0xC000 if nothing else is set). The boot log prints these.

## SPI1: radio and flash (P2, #9, #10)

Implemented in `src/spi1.c`, `src/cc110l.c`, `src/mx25r.c`, `src/radio.c`. Run on the bench unit 2026-11-12 (bench RTT capture, 300 s, 2026-11-12, kept outside the repo), receive-only on channel 26, flash read-only:

- **[V]** Boot: reset cause POR/PDR PIN BOR; SYSCLK 96 MHz from HSE 26 MHz; SPI1 SCK 6 MHz, mode 0.
- **[V]** Flash: JEDEC ID `C2 28 15` (MX25R1635F), status register `00`. 30 periodic ID checks in 5 min, 0 mismatches, 2 of them taken while a frame was arriving. This verifies the shared bus (#10).
- **[V]** Radio: PARTNUM `00`, VERSION `17`, as documented (`datasheets/cc110l.txt`). Calibrated FSCAL3 `ef`, FSCAL2 `2c`, FSCAL1 `1a`. SPI errors 0, status glitch 0 over the run (#9).

| Item | Setting | Source |
| :--- | :--- | :--- |
| Chip selects | PA4 (radio CSn) and PB9 (flash CS#) driven high as push-pull outputs by `board_cs_early()`, the first call in `main()`, before the 2 s startup delay (#28): the GPIOA/GPIOB clock enables, then BSRR (level first), OTYPER and a read-modify-write of the one MODER field per pin; nothing else moves before the delay. `board_init()` repeats it (idempotent) and sets the output speed | `datasheets/rm0383.txt:5900-5960` (RCC_AHB1ENR), `:8064-8110` (MODER, OTYPER), `:8222-8245` (BSRR); `src/board.c` |
| SCK, MISO, MOSI | PA5, PA6, PA7 on AF5. MISO has the internal pull-up, so an undriven line reads high ("chip not ready") | `datasheets/stm32f411ce.txt:2786-2799` |
| Mode | Master, mode 0 (CPOL 0, CPHA 0), 8-bit, MSB first, software NSS | CC110L `datasheets/cc110l.txt:1291,1300-1318`; MX25R modes 0 and 3 `datasheets/mx25r1635f.txt:124`; `datasheets/rm0383.txt:33022-33130` |
| SCK | APB2 96 MHz / 16 = **6 MHz** (4 MHz on the HSI-only fallback). The divider is the smallest that stays at or below 6.5 MHz | CC110L burst access 6.5 MHz max, single 9 MHz (`cc110l.txt:1322-1332`); MX25R RDID/RDSR/READ 33 MHz (`mx25r1635f.txt:3268-3271`) |
| GPIOA | Only PA0, PA1, PA4-PA7 are configured, through per-pin read-modify-write; a `_Static_assert` rejects PA13/PA14 in the mask | `src/board.c` |
| GDO0, GDO2 | PA0, PA1 inputs with pull-down; EXTI0 both edges (sync = rising, end of packet = falling; the ISR classifies by pin level, #11), EXTI1 rising edge, NVIC priority 5 (0x50), the highest that may call FreeRTOS `FromISR` | `datasheets/rm0383.txt:11032-11050`; `src/FreeRTOSConfig.h` |
| GDO storm guard | More than 32 edges on GDO0 and GDO2 together within 1 ms (TIM2 window) turns both EXTI lines off in the ISR and wakes the radio task to re-initialise (at most once per second; otherwise the next 10 s health check does it). Covers a radio brown-out, after which GDO0 outputs its 135 kHz reset clock (`IOCFG0 = 0x3F`, `datasheets/cc110l.txt:2894-2896,3522-3528`), one edge every 3.7 µs with both edges counted (about 270 edges/ms). Real traffic gives at most 2 GDO0 edges per packet (1.6 ms for the shortest) and one GDO2 edge per byte (160 µs) **[I]** | `src/radio.c` |
| Timebase | TIM2 free-running at 1 MHz (32-bit), for bounded driver waits; no pin | `datasheets/rm0383.txt:4758-4763,17128` |

**Bus sharing [I].** By design the radio task owns SPI1 and runs flash accesses itself (`design.md`, "Tasks"): the housekeeping task only asks for a flash check every 10 s, and the radio task reads the JEDEC ID between FIFO pumps, also while a frame is arriving (the 64-byte RX FIFO absorbs a 10 µs flash access). A mutex around every transaction and a check that refuses to select one device while the other's CS is low are a second line of defence. MISO is shared, so the CC110L must keep GDO1 (its SO pin) 3-state while CSn is high: `IOCFG1 = 0x2E`, the reset value and the stock value (`cc110l.txt:2891-2893,2901`).

**Flash is read-only in P2.** The driver can send only RDID (0x9F) and RDSR (0x05). Every command that changes the array or the status register needs WREN (0x06) first (program and erase `mx25r1635f.txt:1352-1357`, WRSR `:1471-1473`), and there is no code path that sends it. The firmware never reads array contents, which hold the stock configuration and network key. At boot a CS# pulse releases deep power-down in case the stock firmware left the chip there (tCRDP 20 ns, then tRDP 35-45 µs, `mx25r1635f.txt:2426-2433,3307-3314,3402-3409`), then the boot log prints the JEDEC ID (expected `C2 28 15`, `mx25r1635f.txt:1123-1125`) and the status register (`mx25r1635f.txt:1380-1394`).

## SPI2: ENC28J60 bring-up (P3 #12, P3.1 #31) [V]

Driver in `src/enc28j60.c`, net task in `src/net.c` (commits dd1dbd5, 061a349). Bench unit, 2026-11-13. Debugger tests use `tools/bench/enc28j60_probe.tcl` (halted core, hand-run SPI2 transactions, every register restored).

| Finding | Evidence |
| :--- | :--- |
| The ENC28J60 is on SPI2 with CS on PB12 | The stock firmware's SPI2 setup (disassembly of the owner's backup, internal flash `0x08030ef8-0x080311de`) uses PB12 CS, PB13-15 AF5, SPI2 /4, and checks EREVID 06. No other free pin (PA2, PA3, PA8-12, PA15, PB3, PB4, PC13) selects anything on MISO |
| PC14 is the chip's RESET and rises fast | Holding PC14 low for 3 s dropped the laptop link (`carrier_changes` 4 to 6); after release it reads high within 1 ms |
| Silicon B7 | EREVID 06 (`datasheets/enc28j60-errata.txt:19-21`); errata #1 (below 8 MHz) does not apply |
| 12 MHz fails (2026-11-13; superseded below) | Reads of ERXSTL/ERXSTH/ERXNDL/ERXNDH (reset values `fa 05 ff 1f`) are garbage at 12 MHz with the pins at medium, fast or high speed. The supply was not measured then; the 2026-11-14 results below suggest this was the same supply sag **[I]** |
| 6 MHz and below work from the debugger (2026-11-13) | 20 of 20 reads correct at 6, 3, 0.75 and 0.19 MHz, also with the firmware's own SPI2 setting (CR1 `0354`) and 10 ms after a RESET pulse |
| The firmware fails at 6 MHz (2026-11-13) | The 061a349 image reads ERXST as `ffff` on most bring-ups. Some reads get the first transaction right and the next one wrong (`fffa`, `00fa`), so the fault is per transaction. The hypothesis was a too-short CS-high time (about 50 ns, TCSD minimum, `enc28j60.txt:4148`). The 2026-11-14 tests below refute it |

The stock firmware also divides SPI2 by 4, but from an APB1 clock not yet read from the backup, so its real SCK is unknown **[I]**.

### Cause: the supply sags below the ENC28J60's minimum (P3.1 #31, 2026-11-14) [V]

Bench unit powered from the ST-Link 3V3 only, Ethernet linked to the laptop. VDDA was measured with the MCU's VREFINT (ADC1 channel 17, VREFIN_CAL taken at VDDA 3.3 V, `datasheets/stm32f411ce.txt:6903-6906`). VDDA is assumed to be the same 3V3 rail that feeds the ENC28J60 **[I]**, which a multimeter has not yet confirmed. The ENC28J60 needs VDD 3.10-3.60 V (`enc28j60.txt:3988`) and draws 120 mA with the PHY active, 160-180 mA while transmitting, and 1.2 mA in power save (`:4015-4024`). Power save is ECON2.PWRSV; in power save the ETH registers stay readable (`:3759-3765`). Two diagnostic images (removed afterwards; RTT logs kept outside the repo) and `tools/bench/enc28j60_supply.tcl` gave:

| ENC28J60 state | VDDA | SPI2 result |
| :--- | :--- | :--- |
| PHY on, link up, MCU running | 2.67-2.69 V | **1000 of 1000 iterations bad** at 12, 6 and 3 MHz. That holds with the firmware's CS timings (setup 50 ns, CS high 50 ns), with setup 1 µs and CS high 5 µs, with 1 µs CS hold, and with 1 µs or 5 µs between bytes. Every read is `ff` |
| PHY on, MCU halted (debugger) | 2.74-2.83 V | Erratic between runs: 6 MHz 0, 34 or 40 of 40; 12 MHz 0 or 40 of 40; 1.5-3 MHz mostly correct |
| Power save, MCU running | 3.09-3.14 V | **0 of 1000 bad** at 12, 6 and 3 MHz, with the firmware's 50 ns CS timings and with the long ones |
| Power save, MCU halted (debugger) | 3.18-3.21 V | 40 of 40 at 12, 6, 3 and 1.5 MHz |

Each iteration has 10 transactions with 8 checks: BFC and BFS bank switches, a WCR of EWRPTL with a changing pattern and its read-back, ERXST and ERXND against their reset values, EREVID `06`, and the ECON1 bank bits. The ST-Link reported 3.20 V throughout, so its reading does not show the sag at the board.

- **The CS-timing hypothesis is refuted.** With the chip in range, the firmware's datasheet-minimum CS timings work at 12 MHz. With the PHY on, no timing works.
- **The failure is the chip not answering**, not slow edges. Whole bytes read as the MISO pull level: `ff` with the pull-up, `00` with PB14 switched to a pull-down. With the PHY on, the result changes from run to run.
- **The drop is about 0.4-0.5 V at roughly 120-150 mA**, which points to about 3-4 Ω between the ST-Link's regulator and the board: the jumper wires, the Tag-Connect contacts, or the clone's regulator **[I]**. Transmitting (180 mA) will pull the rail lower still.
- The MCU itself stays in range (1.7-3.6 V, and 3 wait states are valid down to 2.4 V at 96 MHz, "Clock tree" above). Brown-out reset is off in the option bytes (P0 results), so the MCU does not reset when the rail sags.

**The supply was the fix, not firmware.** The owner changed the bench supply on 2026-11-14 (USB; the ST-Link 3V3 wire is disconnected; details in `test-bench.md`, "Setup"). Results below.

### Result: margin test and 10-minute run (P3.1 #31, 2026-11-14) [V]

Owner-changed supply, Ethernet linked to the laptop (10 Mb/s half duplex), firmware at 6 MHz (current `src/enc28j60.c`), radio receive-only on channel 26 as in P2. RTT logs kept outside the repo.

**Supply.** `tools/bench/enc28j60_supply.tcl` against the running firmware: VDDA **3.28-3.29 V** with the PHY on, in power save and on again, inside the ENC28J60's 3.10-3.60 V (`enc28j60.txt:3988`). 40 of 40 good reads at 12, 6, 3 and 1.5 MHz in every state.

**Why the script reported 0 of 40 before.** Its good-read test compared ERXST/ERXND with the reset values `fa 05 ff 1f` (`enc28j60.txt:771-774`). Once bring-up succeeds, the firmware writes its own buffer layout, RXST `0000` and RXND `19ff` (`src/enc28j60.c`, `wr16(ERXSTL, RXST)` and `wr16(ERXNDL, RXND)` in `enc28j60_init()`). Confirmed from the debugger: the reference read at 187 kHz is `00 00 ff 19`, and the firmware's own boot log shows bring-up passing (`eth: configured ...`), which needs that read-back. The script now takes a reference at 187 kHz, accepts either pattern, and counts reads against it. It was also made safe against a running firmware: it halts only between two SPI2 transactions (PB12 high), restores the chip's bank select (the old version left bank 0 selected while the firmware's cached bank was 3: ECON1 read `07` at the halt), and enters and leaves power save with the datasheet sequence (RXEN off, RXBUSY and TXRTS clear, PWRSV; then CLKRDY, RXEN back; `enc28j60.txt:3749-3770`). After a run, the firmware continued with `reinit 0`.

**Margin test** (diagnostic image, removed afterwards). The firmware's own transaction code with its CS timings, link up, run after each self-test transmission, 12 runs of 1000 iterations at each SCK (an earlier build of the same test ran 2 more runs at each SCK right after bring-up, link still down, also without a failure). Each iteration: forced BFC/BFS bank switches, WCR of EWRPTL with a changing pattern and its read-back, ERXSTL/H and ERXNDL/H against `0000`/`19ff`, EREVID `06`, the ECON1 bank bits, MAADR1 and MAADR6 (MAC registers: dummy byte, 210 ns hold), and PHID1 `0083` through MII.

| SCK | Result |
| :--- | :--- |
| 12 MHz | **12000 of 12000 iterations good**, no failing check |
| 6 MHz | **12000 of 12000 iterations good**, no failing check |

**CS timings as built**, measured with the DWT cycle counter at 96 MHz: the delay loop takes 3 cycles per iteration, not the 2 that `spin_for_ns()` assumes, so the delays are longer than asked for. Spin alone, before the GPIO and SPI accesses around it: TCSS and TCSD 94 ns (minimum 50 ns), TCSH 31 ns for ETH and buffer (minimum 10 ns) and 344 ns for MAC and MII (minimum 210 ns) (`enc28j60.txt:4143-4148`).

**SCK choice: 6 MHz stays.** 12 MHz passes, but it is the fastest rate tested (24 MHz is above the chip's 20 MHz, `enc28j60.txt:4144`), so the test shows no factor of 2 above it. The worst-case data path at 12 MHz fits its 41.7 ns half period only 1.6 times **[I]**: MOSI tv(MO) 6 ns (`stm32f411ce.txt:6218`) plus a 10 ns edge at medium pin speed and 50 pF (`stm32f411ce.txt:5843`) plus the chip's 10 ns setup (`enc28j60.txt:4149`) is 26 ns; MISO the chip's 10 ns output valid (`enc28j60.txt:4151`) plus a 10 ns SCK edge plus tsu(MI) 4 ns (`stm32f411ce.txt:6190`) is 24 ns. At 6 MHz (83.3 ns) the factor is 3.2, and the 12 MHz test is a measured factor of 2 in clock rate. 6 MHz gives 0.75 MB/s on the bus, below the 1.25 MB/s of 10 Mb/s Ethernet at full load; the bridge's traffic is far smaller **[I]**. Silicon B1 or B4 would want 8 MHz or more (errata #1, `datasheets/enc28j60-errata.txt:86-112`); the firmware warns if it reads such a revision.

**Bring-up at every boot.** Six consecutive resets (OpenOCD `reset run`, `RCC_CSR` `SOFTWARE PIN`): bring-up passed at the first attempt each time, EREVID `06`, PHID1 `0083`, PHID2 `1400`, every read-back correct, no `bring-up failed` line. Not yet tried after a power cycle.

**10-minute run.** Final image (no diagnostic code, sha256 of `build/tado-bridge.bin` `67379ceb6e0c952437174b5076e2237f205282e54c19b4ced5ef9137364e7b1e`, flashed with OpenOCD verify), reset, then 670 s of RTT: bring-up at the first attempt, link up within the first minute, then 11 per-minute reports with `reinit 0`, `int storm 0`, no chip-check failure, no read-back mismatch, no fault and no watchdog reset. Self-test: 65 frames sent, `tx ok 65`, `err 0`, `late-col retry 0`; the laptop's `rx_packets` on `enp196s0f0` rose by 67 in the same time. Radio and flash on SPI1 ran alongside with no flash mismatch. The #12 bring-up and service now work on this bench; #12 continues.

**Open:** bring-up after a full power cycle with the owner-changed supply was not tried (Claude cannot cut the power). The stock firmware's APB1 clock (its real SPI2 rate) is still unread, and no longer needed for P3.1.

### Result: half duplex, link, frame RX and TX against the laptop (#12, 2026-11-14) [V]

Same bench as above (USB supply, `enp196s0f0` at 10 Mb/s half duplex, laptop `10.42.0.1`). A test-only image extended the self-test in `src/net.c`, then the self-test was removed (project rule) and the final image flashed (sha256 of `build/tado-bridge.bin` `5e0041f7c3720772677d71b368b3b5b97bed0d0f26ea06003b93e73dc569ea85`, OpenOCD verify OK). The laptop has no `tcpdump` and no root, so the laptop side was checked with its interface counters (`/sys/class/net/enp196s0f0/statistics/`) and with frames that Linux answers. RTT logs kept outside the repo.

**Half duplex.** `MACON3` = `32` (PADCFG 001, TXCRCEN, FRMLNEN, FULDPX 0; `enc28j60.txt:1868-1908`) and `PHCON1` = `0000` (PDPXMD 0; `:3167-3199`) are written and read back at every bring-up; `PHSTAT2` reports `0400`, link up and DPXSTAT 0 = half duplex (`:1296-1301`). MABBIPG `12`, MAIPGL `12`, MAIPGH `0c` as recommended for half duplex (`:1806-1834`). Laptop side: `collisions` 0 and `rx_errors` 0 over the runs below.

**Link status.** After the PHY reset in bring-up the firmware logs `link down (PHSTAT2 0000 ...)`, then `link up (PHSTAT2 0400: half duplex, polarity ok)` once the laptop's link pulses are seen, and the router LED follows. A link loss while running (cable pull) was not tried: it needs the owner. It uses the same PHSTAT2.LSTAT poll on every service pass **[I]**.

| Test | Result |
| :--- | :--- |
| TX round trip | Every 10 s an ARP probe (RFC 5227: sender IP `0.0.0.0`, claims no address) for `10.42.0.1`. The laptop answered each one with a unicast ARP reply to the bridge's MAC, which the bridge received and logged: 24 probes, 24 replies, so the frames left with a valid CRC, padding to 60 bytes worked (laptop counts 60 bytes) and the unicast filter (ERXFCON.UCEN) passes our own address |
| TX full size | Every 10 s a 1514-byte broadcast (ethertype 88b5), written in 100-byte chunks. In a 137 s window the laptop's `rx_bytes` rose by exactly 14 x 1514 + 13 x 60 = 21976 over 27 packets, `rx_errors` 0; the 14 ethertype-88b5 frames show up as `rx_dropped` (no protocol handler). Bridge side: `tx ok` 47, `err` 0, `late-col retry` 0 |
| RX full size, whole frame | 125 broadcast pings from the laptop (`ping -b -s 1472 -i 1 10.42.0.255`, 1514-byte frames) all received, read out whole and checked with the ICMP checksum: **125 of 125 good**. That is about 190 KB through the 6.5 KB receive ring, so the read pointer wrapped from ERXND to ERXST many times. Receive status vector `03c005ee`: 1518 bytes with CRC, Received OK, type field, broadcast (`:2295-2325`) |
| RX filter, loop-back | `own 0` throughout: HDLDIS keeps our own transmissions out of the receive buffer (`:2021-2026`). `overflow 0`, `bad 0`, `reinit 0`, `int storm 0` |
| Final image (no self-test) | Bring-up and `link up` at the first attempt; 125 broadcast pings in 2 min counted as `rx ok`, `bad 0`, `reinit 0`; `tx ok 0` and the laptop's `rx_packets` did not change, so nothing is sent until lwIP (#13). Radio receive-only and flash checks ran alongside without error |

**RTT tooling notes [V].** The RTT up buffer (1 KB, skip when full, `src/rtt.c`) can drop or cut lines in the boot burst: one run lost the `link up` line and truncated another. After a flash, `tools/rtt.sh` can find the previous image's control block, still in RAM, at its old address and show stale output; check the found address against `arm-none-eabi-nm build/tado-bridge.elf | grep ' cb$'`.

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

**Power over USB with the ST-Link 3V3 wire disconnected** (owner decision, 2026-11-14; the ST-Link 3V3 sagged, see "Cause" above); never two supplies at once (`README.md:211,247`).

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
