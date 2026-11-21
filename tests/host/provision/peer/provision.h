/* Host-test stand-in for the generated provision.h (cmake/CMakeLists.txt
 * "Provisioning"), for the programs built as a channel-26 image is
 * (tests/host/Makefile, test_<name>_CH 26): a pairing target, freedo's test
 * joiner (freedo tado/pair_window_test.go:13, wire order 0b b1 ce fa 07 c5
 * 1b 00; the source of its Beacon Request vector, tado/beacon_test.go:57),
 * in stored order. A made-up address, not a device of the bench. */
#ifndef PROVISION_H
#define PROVISION_H
#define PROVISION_PAIR_EUI64 {0x00, 0x1b, 0xc5, 0x07, 0xfa, 0xce, 0xb1, 0x0b}
#endif
