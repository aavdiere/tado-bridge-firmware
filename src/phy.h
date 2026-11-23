/* The tado radio PHY: one home for the channel plan and the radio timing
 * constants (#34 proposal 6). Defines and static asserts only, no code; the
 * radio task (src/radio.c), the CC110L driver (src/cc110l.c), the transmit
 * gate (src/txgate.c) and the airtime limiter (src/airtime.h) take their
 * values from here, and the host tests build with it. */
#ifndef PHY_H
#define PHY_H

/* CC110L crystal, 26 MHz. Signed 64-bit: the transmit gate's carrier sum is
 * signed (a negative offset trim), and the radio task's frequency plan
 * checks need more than 32 bits. */
#define PHY_FXOSC_HZ 26000000ll

/* Channel plan as the stock register table programs it (docs/radio.md
 * "PHY", "Stock CC110L register table"): FREQ = 0x213276 (863.125 MHz),
 * CHANSPC_E = 2 and CHANSPC_M = 0xF8 (199.951 kHz), so channel n is at
 * 863.125 MHz + n x 199.951 kHz, n = 0..49. The register table in
 * src/radio.c stays a copy of the stock capture and is checked against
 * these at build time; the transmit gate refuses a carrier programmed with
 * any other base or spacing (src/txgate.c, TXGATE_OFF_PLAN). */
#define PHY_FREQ_WORD 0x213276l
#define PHY_CHANSPC_E 2u
#define PHY_CHANSPC_M 0xF8u

/* Carrier of channel ch in Hz, for the checks below:
 *   fXOSC / 2^16 x FREQ + ch x fXOSC / 2^18 x (256 + CHANSPC_M) x 2^CHANSPC_E
 * (cc110l.txt:3729-3736, 3869-3880). */
#define PHY_CARRIER_HZ(ch) \
    (PHY_FXOSC_HZ * (PHY_FREQ_WORD * 4 + (ch) * ((256 + PHY_CHANSPC_M) << PHY_CHANSPC_E)) / 262144)
_Static_assert(PHY_CARRIER_HZ(0) > 863124000ll && PHY_CARRIER_HZ(0) < 863126000ll,
               "channel plan base 863.125 MHz");
_Static_assert(PHY_CARRIER_HZ(26) > 868322700ll && PHY_CARRIER_HZ(26) < 868324700ll,
               "channel plan: channel 26, the protected channel, at 868.3237 MHz");

/* Bit rate, 49.988 kBaud (docs/radio.md "PHY": 49,987.79 bps; src/radio.c
 * checks MDMCFG4/MDMCFG3 against it): a byte takes 160.04 us. */
#define PHY_BAUD 49987u

/* On air around the MAC frame: 4 preamble bytes, 4 sync bytes, the length
 * byte and 2 CRC bytes (docs/radio.md:24,82). */
#define PHY_FRAME_EXTRA_BYTES (4u + 4u + 1u + 2u)

/* One byte on air, nominal: 160 us (160.04 us rounded down). */
#define PHY_BYTE_US 160u
_Static_assert(8000000u >= PHY_BYTE_US * PHY_BAUD && 8000000u < (PHY_BYTE_US + 1u) * PHY_BAUD,
               "PHY_BYTE_US is a byte's time on air, rounded down");

/* IDLE to TX without calibration (MCSM0 FS_AUTOCAL = 0): 75.2 us
 * (cc110l.txt:2542, Table 5-11). */
#define PHY_IDLE_TO_TX_US 75u

/* Two estimates of a frame's time on air, different on purpose:
 *   - The airtime charge (AIRTIME_FRAME_US, src/airtime.h): 161 us per byte
 *     and 100 us for the ramp from IDLE, both above the real 160.04 and
 *     75.2 us, and no other margin. The duty-cycle budget must never be
 *     charged less than the real time on air, so it errs towards refusing a
 *     frame. src/airtime.h asserts both against the values here.
 *   - The TX watch window (tx_expect(), src/cc110l.c): the nominal
 *     PHY_BYTE_US and PHY_IDLE_TO_TX_US, plus an explicit 2 ms margin for
 *     the interrupt and the task that watch the frame. The margin covers
 *     the rounding many times over (at most 138 bytes x 0.04 us, plus
 *     0.2 us). While the window is open a TX state counts as ours and the
 *     radio counts as busy, so it is kept close to the real time.
 * The values are the ones used before this header (#34), so the code that
 * uses them is unchanged, the Enhanced ACK's interrupt path (tx_expect()
 * just before its STX) included. */

#endif
