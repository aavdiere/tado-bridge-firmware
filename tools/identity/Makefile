# Identity record provisioning tool (#21): plain gcc on the build machine,
# linked with the firmware's own src/store.c. `make identity-tool` from the
# repository root, or `make -C tools/identity`. Output outside the source
# tree's tracked files: build/host-tools/identity.
CC      ?= gcc
ROOT    := ../..
SRC     := $(ROOT)/src
OUT     := $(ROOT)/build/host-tools

CFLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Wundef -Wshadow -Wconversion \
          -Wno-sign-conversion -I$(SRC) -I$(ROOT)/vendor/mbedtls/include \
          -I$(ROOT)/tests/host -DMBEDTLS_CONFIG_FILE='"mbedtls_host_config.h"' \
          -DRADIO_TX_ALLOWED=0 -DRADIO_NET_CHANNEL=16 -DRADIO_ALLOW_CH26=0
# The channel (src/txgate.h, which src/store.c includes) does not matter
# here: the tool writes the identity record only, never a network record.

.PHONY: all
all: $(OUT)/identity

$(OUT):
	mkdir -p $@

$(OUT)/identity: identity.c $(SRC)/store.c $(wildcard $(SRC)/*.h) Makefile | $(OUT)
	$(CC) $(CFLAGS) identity.c $(SRC)/store.c -o $@
