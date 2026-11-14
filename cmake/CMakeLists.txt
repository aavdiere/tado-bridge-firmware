cmake_minimum_required(VERSION 3.13) # target_link_options

################################################################################
# Customize
if(NOT DEFINED BINARY)
    set(BINARY tado-bridge)
endif()
message("Project name: ${BINARY}")

# Repo root, so __FILE__ is repo-relative (no home path in the binary)
get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(FREERTOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/FreeRTOS-Kernel")
set(OPENCM3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/libopencm3")
set(LWIP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/lwip")

# Server host name or address the firmware resolves and (P4) connects to
# (docs/server-api.md "Transport"). The bench server is 10.42.0.1
# (docs/test-bench.md "Bench network").
set(NET_SERVER_HOST "10.42.0.1" CACHE STRING "tanoclo server host name or IPv4 address")

# Build type: Debug, Release
if ("${CMAKE_BUILD_TYPE}" STREQUAL "")
    set(CMAKE_BUILD_TYPE Debug)
endif()
message("Build type: ${CMAKE_BUILD_TYPE}")

################################################################################
# Export CMake commands to load into Visual Studio Code
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

################################################################################
# ARM specific setup, must precede project() so compiler detection uses it
include(./toolchain-arm-none-eabi.cmake)

################################################################################
# Setup project
project(${BINARY})

################################################################################
# Device, e.g. STM32L476RG -> stm32l4.cmake + stm32l476rg.ld
set(DEVICE STM32F411CE CACHE STRING "Target device")
string(TOLOWER ${DEVICE} DEVICE_LC)
string(SUBSTRING ${DEVICE_LC} 0 7 DEVICE_FAMILY)
message("Device: ${DEVICE}")

set(FAMILY_CMAKE "${CMAKE_CURRENT_SOURCE_DIR}/${DEVICE_FAMILY}.cmake")
set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/${DEVICE_LC}.ld")
foreach(f ${FAMILY_CMAKE} ${LINKER_SCRIPT})
    if(NOT EXISTS ${f})
        message(FATAL_ERROR "${f} not found for DEVICE=${DEVICE}")
    endif()
endforeach()

# MPU series (uses LINKER_SCRIPT, so include after it is set)
include(${FAMILY_CMAKE})

################################################################################
# Linker setup
string(TOLOWER ${MCU_FAMILY} MCU_LIBNAME)

################################################################################
# lwIP 2.2.1 (#13): IPv4 core, DHCP with address conflict detection, DNS,
# ICMP, UDP, TCP, Ethernet/ARP. Options in src/lwipopts.h.
set(LWIP_SRCS
    ${LWIP_DIR}/src/core/init.c
    ${LWIP_DIR}/src/core/def.c
    ${LWIP_DIR}/src/core/dns.c
    ${LWIP_DIR}/src/core/inet_chksum.c
    ${LWIP_DIR}/src/core/ip.c
    ${LWIP_DIR}/src/core/mem.c
    ${LWIP_DIR}/src/core/memp.c
    ${LWIP_DIR}/src/core/netif.c
    ${LWIP_DIR}/src/core/pbuf.c
    ${LWIP_DIR}/src/core/raw.c
    ${LWIP_DIR}/src/core/stats.c
    ${LWIP_DIR}/src/core/sys.c
    ${LWIP_DIR}/src/core/tcp.c
    ${LWIP_DIR}/src/core/tcp_in.c
    ${LWIP_DIR}/src/core/tcp_out.c
    ${LWIP_DIR}/src/core/timeouts.c
    ${LWIP_DIR}/src/core/udp.c
    ${LWIP_DIR}/src/core/ipv4/acd.c
    ${LWIP_DIR}/src/core/ipv4/dhcp.c
    ${LWIP_DIR}/src/core/ipv4/etharp.c
    ${LWIP_DIR}/src/core/ipv4/icmp.c
    ${LWIP_DIR}/src/core/ipv4/ip4.c
    ${LWIP_DIR}/src/core/ipv4/ip4_addr.c
    ${LWIP_DIR}/src/core/ipv4/ip4_frag.c
    ${LWIP_DIR}/src/netif/ethernet.c
)

################################################################################
# Files
add_executable(${BINARY}
    ../src/main.c
    ../src/clock.c
    ../src/rtt.c
    ../src/log.c
    ../src/syscalls.c
    ../src/board.c
    ../src/watchdog.c
    ../src/app.c
    ../src/hooks.c
    ../src/faults.c
    ../src/timebase.c
    ../src/spi1.c
    ../src/cc110l.c
    ../src/mx25r.c
    ../src/radio.c
    ../src/enc28j60.c
    ../src/net.c
    ../src/lwip_port.c
    ${FREERTOS_DIR}/tasks.c
    ${FREERTOS_DIR}/queue.c
    ${FREERTOS_DIR}/list.c
    ${FREERTOS_DIR}/portable/GCC/ARM_CM4F/port.c
    ${FREERTOS_DIR}/portable/MemMang/heap_4.c
    ${LWIP_SRCS}
)

target_include_directories(${BINARY} PRIVATE
    ${OPENCM3_DIR}/include
    ../src
    ${FREERTOS_DIR}/include
    ${FREERTOS_DIR}/portable/GCC/ARM_CM4F
    ${LWIP_DIR}/src/include
)

target_compile_definitions(${BINARY} PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    ${MCU_FAMILY}
    NET_SERVER_HOST="${NET_SERVER_HOST}"
)

target_compile_options(${BINARY} PRIVATE
    -MD
    -fmacro-prefix-map=${REPO_ROOT}/=
    -Wundef
    $<$<COMPILE_LANGUAGE:CXX>:
        -Wconversion
        -Wno-volatile
        -Wold-style-cast
        -Wuseless-cast
        -Wsuggest-override
        -Weffc++
        -Wredundant-decls
    >
)

# Linker script per target, so a cmake re-run cannot repeat it; relink when it
# changes.
target_link_options(${BINARY} PRIVATE -T${LINKER_SCRIPT})
set_target_properties(${BINARY} PROPERTIES LINK_DEPENDS ${LINKER_SCRIPT})

target_link_libraries(${BINARY} PRIVATE
    -L${OPENCM3_DIR}/lib
    -lopencm3_${MCU_LIBNAME}
)

print_section_sizes(${BINARY})
create_hex_output(${BINARY})
create_bin_output(${BINARY})
