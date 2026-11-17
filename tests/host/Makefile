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
          -I$(SRC) -I. -I$(MBEDTLS)/include \
          -DMBEDTLS_CONFIG_FILE='"mbedtls_host_config.h"'
# mbedTLS is third-party code: built without our warning set.
MBEDTLS_CFLAGS := -std=c11 -O1 -g -fsanitize=address,undefined -I. -I$(MBEDTLS)/include \
          -DMBEDTLS_CONFIG_FILE='"mbedtls_host_config.h"'

AES_OBJS := $(OUT)/aes.o $(OUT)/platform_util.o

TESTS := test_mac test_lowpan test_txgate test_csl test_relay test_link \
         test_airtime test_cc110l test_cc110l_off test_store

# Protocol modules each test links.
test_mac_SRCS := mac.c ccm.c
test_lowpan_SRCS := lowpan.c mac.c ccm.c
test_txgate_SRCS := txgate.c mac.c ccm.c
test_csl_SRCS := csl.c txgate.c mac.c ccm.c
test_relay_SRCS := relay.c msgq.c lowpan.c csl.c txgate.c mac.c ccm.c
test_link_SRCS := link.c relay.c msgq.c lowpan.c csl.c txgate.c mac.c ccm.c
# Network record and counter log against a flash model with power cuts (#37).
test_store_SRCS := store.c
# The radio driver's transmit functions against a simulated chip
# (cc110l_model.h). Both programs include src/cc110l.c themselves: one with
# the transmitter switched on for that translation unit only, one as the
# firmware builds it.
test_airtime_SRCS := airtime.c
test_cc110l_SRCS := txgate.c mac.c ccm.c airtime.c
test_cc110l_DEPS := $(SRC)/cc110l.c cc110l_model.h
test_cc110l_off_SRCS := txgate.c mac.c ccm.c airtime.c
test_cc110l_off_DEPS := $(SRC)/cc110l.c cc110l_model.h

.PHONY: all run
all: run

run: $(addprefix $(OUT)/,$(TESTS))
	@fail=0; for t in $^; do $$t || fail=1; done; \
	if [ $$fail = 0 ]; then echo "host tests: all passed"; else echo "host tests: FAILED"; exit 1; fi

$(OUT):
	mkdir -p $@

$(OUT)/aes.o: $(MBEDTLS)/library/aes.c mbedtls_host_config.h | $(OUT)
	$(CC) $(MBEDTLS_CFLAGS) -c $< -o $@
$(OUT)/platform_util.o: $(MBEDTLS)/library/platform_util.c mbedtls_host_config.h | $(OUT)
	$(CC) $(MBEDTLS_CFLAGS) -c $< -o $@

.SECONDEXPANSION:
$(OUT)/test_%: test_%.c test.h vectors_freedo.h $$(addprefix $(SRC)/,$$(test_$$*_SRCS)) $$(test_$$*_DEPS) $(wildcard $(SRC)/*.h) $(AES_OBJS) | $(OUT)
	$(CC) $(CFLAGS) $< $(addprefix $(SRC)/,$(test_$*_SRCS)) $(AES_OBJS) -o $@
