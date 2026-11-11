##
## Author:  Johannes Bruder
## License: See LICENSE.TXT file included in the project
##
##
## CMake arm-none-eabi toolchain file
##

# Append current directory to CMAKE_MODULE_PATH for making device specific
# cmake modules visible
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR})

# Target definition
set(CMAKE_SYSTEM_NAME  Generic)
set(CMAKE_SYSTEM_PROCESSOR ARM)

#-------------------------------------------------------------------------------
# Set toolchain paths
#-------------------------------------------------------------------------------
set(TOOLCHAIN arm-none-eabi)
if(NOT DEFINED TOOLCHAIN_PREFIX)
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL Linux)
        set(TOOLCHAIN_PREFIX "/usr")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL Darwin)
        set(TOOLCHAIN_PREFIX "/usr/local")
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL Windows)
        message(STATUS
            "Please specify the TOOLCHAIN_PREFIX !\n"
            "For example: "
            "-DTOOLCHAIN_PREFIX=\"C:/Program Files/GNU Tools ARM Embedded\" "
        )
    else()
        set(TOOLCHAIN_PREFIX "/usr")
        message(STATUS
            "No TOOLCHAIN_PREFIX specified, using default: "
            ${TOOLCHAIN_PREFIX}
        )
    endif()
endif()
set(TOOLCHAIN_BIN_DIR ${TOOLCHAIN_PREFIX}/bin)
set(TOOLCHAIN_INC_DIR ${TOOLCHAIN_PREFIX}/${TOOLCHAIN}/include)
set(TOOLCHAIN_LIB_DIR ${TOOLCHAIN_PREFIX}/${TOOLCHAIN}/lib)

# Set system depended extensions
if(WIN32)
    set(TOOLCHAIN_EXT ".exe" )
else()
    set(TOOLCHAIN_EXT "" )
endif()

# Perform compiler test with static library
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

#-------------------------------------------------------------------------------
# Set compiler/linker flags
#-------------------------------------------------------------------------------

# Object build options
# -O0                   No optimizations, reduce compilation time and make
#                       debugging produce the expected results.
# -mthumb               Generate thumb instructions (required for Cortex-M).
# -mabi=aapcs           Defines enums to be a variable sized type.
# -Wall                 Print only standard warnings, for all use Wextra
# -Wextra               Print all warnings
# -fno-builtin          Do not use built-in functions provided by GCC.
# -ffunction-sections   Place each function item into its own section in the
#                       output file.
# -fdata-sections       Place each data item into its own section in the
#                       output file.
# -fomit-frame-pointer  Omit the frame pointer in functions that don’t need one.

# -mabi=aapcs \
# -fno-builtin \
# -fomit-frame-pointer \
set(OBJECT_GEN_FLAGS "\
    -O0 \
    -mthumb \
    -Wall \
    -Wextra \
    -fno-common \
    -ffunction-sections \
    -fdata-sections \
")

set(CMAKE_C_FLAGS
    "${OBJECT_GEN_FLAGS} --specs=nano.specs -std=gnu99"
    CACHE INTERNAL "C Compiler options"
)
set(CMAKE_CXX_FLAGS
    "${OBJECT_GEN_FLAGS} -std=c++11"
    CACHE INTERNAL "C++ Compiler options"
)
set(CMAKE_ASM_FLAGS
    "${OBJECT_GEN_FLAGS} -x assembler-with-cpp"
    CACHE INTERNAL "ASM Compiler options"
)


# -Wl,--gc-sections     Perform the dead code elimination.
# --specs=nano.specs    Newlib-nano; set in CMAKE_C_FLAGS (also used at link).
# --specs=nosys.specs   No syscalls, provide empty implementations for the
#                       POSIX system calls.
# --specs=nosys.specs \
# -nostdlib \
# -mabi=aapcs \
# Set, not appended: the cache entry survives a re-run of cmake on the same
# build directory, and appending to it would repeat every flag (and -T, which
# ld refuses twice). The linker script is a target link option instead
# (cmake/CMakeLists.txt).
set(CMAKE_EXE_LINKER_FLAGS " \
    -Wl,--gc-sections \
    -nostartfiles \
    --static \
    -Wl,-Map=${BINARY}.map \
    -Wl,--cref \
    -Wl,--print-memory-usage \
    -Wl,--start-group \
    -lc \
    -lgcc \
    -lnosys \
    -Wl,--end-group \
" CACHE INTERNAL "Linker options")

#-------------------------------------------------------------------------------
# Set debug/release build configuration Options
#-------------------------------------------------------------------------------

# Options for DEBUG build
# -Og   Enables optimizations that do not interfere with debugging.
# -g    Produce debugging information in the operating system’s native format.
set(CMAKE_C_FLAGS_DEBUG
    "-Og -g" CACHE INTERNAL "C Compiler options for debug build type"
)
set(CMAKE_CXX_FLAGS_DEBUG
    "-Og -g" CACHE INTERNAL "C++ Compiler options for debug build type"
)
set(CMAKE_ASM_FLAGS_DEBUG
    "-g" CACHE INTERNAL "ASM Compiler options for debug build type"
)
set(CMAKE_EXE_LINKER_FLAGS_DEBUG
    "" CACHE INTERNAL "Linker options for debug build type"
)

# Options for RELEASE build
# -Os   Optimize for size. -Os enables all -O2 optimizations.
# -flto Runs the standard link-time optimizer.
set(CMAKE_C_FLAGS_RELEASE
    "-Os -flto" CACHE INTERNAL "C Compiler options for release build type"
)
set(CMAKE_CXX_FLAGS_RELEASE
    "-Os -flto" CACHE INTERNAL "C++ Compiler options for release build type"
)
set(CMAKE_ASM_FLAGS_RELEASE
    "" CACHE INTERNAL "ASM Compiler options for release build type"
)
set(CMAKE_EXE_LINKER_FLAGS_RELEASE
    "-flto" CACHE INTERNAL "Linker options for release build type"
)


#-------------------------------------------------------------------------------
# Set compilers
#-------------------------------------------------------------------------------
set(CMAKE_C_COMPILER
    ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN}-gcc${TOOLCHAIN_EXT}
    CACHE INTERNAL "C Compiler"
)
set(CMAKE_CXX_COMPILER
    ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN}-g++${TOOLCHAIN_EXT}
    CACHE INTERNAL "C++ Compiler"
)
set(CMAKE_ASM_COMPILER
    ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN}-gcc${TOOLCHAIN_EXT}
    CACHE INTERNAL "ASM Compiler"
)

set(CMAKE_FIND_ROOT_PATH
    ${TOOLCHAIN_PREFIX}/${${TOOLCHAIN}}
    ${CMAKE_PREFIX_PATH}
)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_EXECUTABLE_SUFFIX_ASM ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_C ".elf")
set(CMAKE_EXECUTABLE_SUFFIX_CXX ".elf")

#-------------------------------------------------------------------------------
# Set tools
#-------------------------------------------------------------------------------
set(CMAKE_OBJCOPY ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN}-objcopy${TOOLCHAIN_EXT})
set(CMAKE_OBJDUMP ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN}-objdump${TOOLCHAIN_EXT})
set(CMAKE_SIZE ${TOOLCHAIN_BIN_DIR}/${TOOLCHAIN}-size${TOOLCHAIN_EXT})


#-------------------------------------------------------------------------------
# Prints the section sizes
#-------------------------------------------------------------------------------
function(print_section_sizes TARGET)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${TARGET}>
    )
endfunction()

#-------------------------------------------------------------------------------
# Creates output in hex format
#-------------------------------------------------------------------------------
function(create_hex_output TARGET)
    add_custom_target(
        ${TARGET}.hex ALL DEPENDS ${TARGET} COMMAND ${CMAKE_OBJCOPY}
        -O ihex $<TARGET_FILE:${TARGET}> ${TARGET}.hex
    )
endfunction()

#-------------------------------------------------------------------------------
# Creates output in binary format
#-------------------------------------------------------------------------------
function(create_bin_output TARGET)
    add_custom_target(
        ${TARGET}.bin ALL DEPENDS ${TARGET} COMMAND ${CMAKE_OBJCOPY}
        -O binary $<TARGET_FILE:${TARGET}> ${TARGET}.bin
    )
endfunction()
