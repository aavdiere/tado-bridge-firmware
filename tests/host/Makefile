# Host-side unit tests for the hardware-independent protocol modules (#17).
# Plain gcc on the build machine; run with `make test` from the repository
# root or `make -C tests/host`. The vectors in vectors_freedo.h come from
# freedo's Go code (gen-vectors.sh).
CC      ?= gcc
ROOT    := ../..
SRC     := $(ROOT)/src
MBEDTLS := $(ROOT)/vendor/mbedtls
OUT     := $(ROOT)/build/host-tests

CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Wundef -Wshadow -Wconversion \
          -Wno-sign-conversion -fsanitize=address,undefined -fno-sanitize-recover=all \
          -I$(SRC) -I$(SRC)/boot -I. -I$(MBEDTLS)/include \
          -DMBEDTLS_CONFIG_FILE='"mbedtls_host_config.h"'
# mbedTLS is third-party code: built without our warning set.
MBEDTLS_CFLAGS := -std=c11 -O1 -g -fsanitize=address,undefined -I. -I$(MBEDTLS)/include \
          -DMBEDTLS_CONFIG_FILE='"mbedtls_host_config.h"'

AES_OBJS := $(OUT)/aes.o $(OUT)/platform_util.o $(OUT)/sha256.o

TESTS := test_mac test_lowpan test_txgate test_csl test_relay test_link \
         test_airtime test_cc110l test_cc110l_off test_store test_identity test_pair test_coap \
         test_mx25r test_button test_pair_led test_boot test_fwupd test_handoff test_pair_remote \
         test_freqoff

# Protocol modules each test links.
test_mac_SRCS := mac.c ccm.c
test_lowpan_SRCS := lowpan.c mac.c ccm.c
test_txgate_SRCS := txgate.c mac.c ccm.c
test_csl_SRCS := csl.c txgate.c mac.c ccm.c
test_relay_SRCS := relay.c msgq.c lowpan.c csl.c txgate.c mac.c ccm.c
test_link_SRCS := link.c pair.c relay.c msgq.c lowpan.c csl.c txgate.c mac.c ccm.c
# Pairing window, beacon and d/pair (#19).
test_pair_SRCS := pair.c mac.c ccm.c
# CoAP codec and the bridge's answer to PUT d/config (refs #19).
test_coap_SRCS := coap.c
# The server's remote pairing trigger (#40): path, payload, outcomes,
# duplicates and the hand-over to the radio task.
test_pair_remote_SRCS := pair_remote.c coap.c
# Network record and counter log against a flash model with power cuts (#37).
test_store_SRCS := store.c
# Identity record (#21), as the provisioning tool writes it and the boot loads it.
test_identity_SRCS := store.c
# Flash work while frames arrive (#22): the flash driver (included by the
# test program) and the store above it, against a simulated flash on SPI1
# and a simulated receiver with random traffic.
# Also the update's flash operations (#24, src/fwupd.c fwupd_op_run()) on the
# real driver under traffic, and the driver's writable areas.
test_mx25r_SRCS := store.c store_port.c fwupd.c fwimg.c
test_mx25r_DEPS := $(SRC)/mx25r.c
# Bootloader decisions and the image and boot-state formats (#24), against
# models of the external and internal flash with power cuts.
test_boot_SRCS := store.c fwimg.c boot/bootctl.c
# The application's side of an update (#24): receive, save, PENDING, confirm,
# with the real formats and bootloader decisions on the same flash models.
# With tools/update/upload.py: test_upload_tool.sh feeds the tool's frames through
# the session's parsers (coap.c) into fwupd.c.
test_fwupd_SRCS := fwupd.c store.c fwimg.c boot/bootctl.c coap.c
# The update task's hand-over of one operation to the radio task, with its
# time limit (#24 review M1), against a scripted runner and clock.
test_handoff_SRCS := handoff.c
# Debounce and long press of the buttons (factory reset, pair).
test_button_SRCS := button.c
# Pairing LED phase (#19).
test_pair_led_SRCS := pair_led.c
# Frequency offset of our network's frames and the drift warning (#11).
test_freqoff_SRCS := freqoff.c
# The rolling one-hour airtime budget (src/airtime.h).
test_airtime_SRCS := airtime.c
# The radio driver's transmit functions against a simulated chip
# (cc110l_model.h). Both programs include src/cc110l.c themselves:
# test_cc110l as the default firmware builds it, transmitter on;
# test_cc110l_off as a receive-only build (RADIO_TX_ALLOWED=0) does.
test_cc110l_SRCS := txgate.c mac.c ccm.c airtime.c
test_cc110l_DEPS := $(SRC)/cc110l.c cc110l_model.h
test_cc110l_off_SRCS := txgate.c mac.c ccm.c airtime.c
test_cc110l_off_DEPS := $(SRC)/cc110l.c cc110l_model.h

# The firmware's transmitter switch (cmake option RADIO_TX_ALLOWED, default
# ON): every program and the sources it links are compiled with the value
# the variant it tests has, 1 unless test_<name>_TX says otherwise.
test_cc110l_off_TX := 0

.PHONY: all run
all: run

# The identity provisioning tool (#21), built as tools/identity/ builds it,
# run against made-up values by test_identity_tool.sh.
IDENTITY_TOOL := $(ROOT)/build/host-tools/identity

run: $(addprefix $(OUT)/,$(TESTS)) identity-tool
	@fail=0; for t in $(addprefix $(OUT)/,$(TESTS)); do $$t || fail=1; done; \
	./test_identity_tool.sh $(IDENTITY_TOOL) $(OUT)/identity-tool || fail=1; \
	./test_upload_tool.sh $(OUT)/test_fwupd $(OUT)/upload-tool || fail=1; \
	if [ $$fail = 0 ]; then echo "host tests: all passed"; else echo "host tests: FAILED"; exit 1; fi

.PHONY: identity-tool
identity-tool:
	$(MAKE) -C $(ROOT)/tools/identity --no-print-directory

$(OUT):
	mkdir -p $@

$(OUT)/aes.o: $(MBEDTLS)/library/aes.c mbedtls_host_config.h | $(OUT)
	$(CC) $(MBEDTLS_CFLAGS) -c $< -o $@
$(OUT)/platform_util.o: $(MBEDTLS)/library/platform_util.c mbedtls_host_config.h | $(OUT)
	$(CC) $(MBEDTLS_CFLAGS) -c $< -o $@
$(OUT)/sha256.o: $(MBEDTLS)/library/sha256.c mbedtls_host_config.h | $(OUT)
	$(CC) $(MBEDTLS_CFLAGS) -c $< -o $@

.SECONDEXPANSION:
$(OUT)/test_%: test_%.c test.h vectors_freedo.h Makefile $$(addprefix $(SRC)/,$$(test_$$*_SRCS)) $$(test_$$*_DEPS) $(wildcard $(SRC)/*.h $(SRC)/boot/*.h) $(AES_OBJS) | $(OUT)
	$(CC) $(CFLAGS) -DRADIO_TX_ALLOWED=$(if $(test_$*_TX),$(test_$*_TX),1) \
	    $< $(addprefix $(SRC)/,$(test_$*_SRCS)) $(AES_OBJS) -o $@
