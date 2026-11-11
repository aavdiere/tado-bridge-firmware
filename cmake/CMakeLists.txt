cmake_minimum_required(VERSION 3.12)

################################################################################
# Customize
if(NOT DEFINED BINARY)
    set(BINARY tado-bridge)
endif()
message("Project name: ${BINARY}")

set(FREERTOS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/FreeRTOS-Kernel")
set(OPENCM3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/libopencm3")

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
    ${FREERTOS_DIR}/tasks.c
    ${FREERTOS_DIR}/queue.c
    ${FREERTOS_DIR}/list.c
    ${FREERTOS_DIR}/portable/GCC/ARM_CM4F/port.c
    ${FREERTOS_DIR}/portable/MemMang/heap_4.c
)

target_include_directories(${BINARY} PRIVATE
    ${OPENCM3_DIR}/include
    ../src
    ${FREERTOS_DIR}/include
    ${FREERTOS_DIR}/portable/GCC/ARM_CM4F
)

target_compile_definitions(${BINARY} PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    ${MCU_FAMILY}
)

target_compile_options(${BINARY} PRIVATE
    -MD
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

target_link_libraries(${BINARY} PRIVATE
    -L${OPENCM3_DIR}/lib
    -lopencm3_${MCU_LIBNAME}
)

print_section_sizes(${BINARY})
create_hex_output(${BINARY})
create_bin_output(${BINARY})
