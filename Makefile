BINARY			?= tado-bridge
DEVICE			?= STM32F411CE
BUILD_TYPE 		?= Release
BUILD_DIR		?= build
FIRMWARE		?= $(BUILD_DIR)/$(BINARY).elf

DEVICE_LC		:= $(shell echo $(DEVICE) | tr '[:upper:]' '[:lower:]')
FAMILY			:= $(shell echo $(DEVICE_LC) | cut -c1-7)
FAMILY_CMAKE	:= cmake/$(FAMILY).cmake
LINKER_SCRIPT	:= cmake/$(DEVICE_LC).ld
DEVICE_STAMP	:= $(BUILD_DIR)/.device-$(DEVICE_LC)-$(BUILD_TYPE)
OPENCM3_LIB		:= vendor/libopencm3/lib/libopencm3_$(FAMILY).a

.PHONY: all format build cmake clean flash rtt size

all: build
build: $(FIRMWARE)

SRCS := $(shell find src -name '*.[ch]' -or -name '*.[ch]pp')
format: $(addsuffix .format, $(SRCS))

%.format: %
	clang-format -i $<

# libopencm3 must be built once (git submodule update --init first)
$(OPENCM3_LIB):
	$(MAKE) -C vendor/libopencm3 TARGETS=stm32/f4 -j8

.PHONY: $(FIRMWARE)
$(FIRMWARE): cmake $(OPENCM3_LIB)
	$(MAKE) -C $(BUILD_DIR) -j8 --no-print-directory

cmake: $(BUILD_DIR)/Makefile

$(BUILD_DIR)/Makefile: cmake/CMakeLists.txt $(FAMILY_CMAKE) $(LINKER_SCRIPT) cmake/toolchain-arm-none-eabi.cmake $(DEVICE_STAMP)
	cmake -G "Unix Makefiles" -B$(BUILD_DIR) -DBINARY=$(BINARY) \
		-DDEVICE=$(DEVICE) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) cmake/

# Start from a clean build directory whenever DEVICE or BUILD_TYPE changes
$(DEVICE_STAMP):
	rm -rf $(BUILD_DIR)
	mkdir -p $(BUILD_DIR)
	touch $@

$(FAMILY_CMAKE) $(LINKER_SCRIPT):
	$(error $@ not found for DEVICE=$(DEVICE))

clean:
	rm -rf $(BUILD_DIR)

size: $(FIRMWARE)
	arm-none-eabi-size $(FIRMWARE)

# OpenOCD only: the IB01 test points carry no NRST, so never use
# --connect-under-reset or st-flash.
flash: $(FIRMWARE)
	tools/flash.sh $(FIRMWARE)

rtt:
	tools/rtt.sh
