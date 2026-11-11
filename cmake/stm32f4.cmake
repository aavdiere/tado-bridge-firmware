###############################################################################
# Set target specific compiler/linker flags

set(MCU_FAMILY STM32F4)

set(ARCH_CPU_FLAGS "-mcpu=cortex-m4")
set(ARCH_FPU_FLAGS "-mfloat-abi=hard -mfpu=fpv4-sp-d16")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} \
    ${ARCH_CPU_FLAGS} \
    ${ARCH_FPU_FLAGS} \
" CACHE INTERNAL "C Compiler options")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} \
    ${ARCH_CPU_FLAGS} \
    ${ARCH_FPU_FLAGS} \
" CACHE INTERNAL "C++ Compiler options")

set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} \
    ${ARCH_CPU_FLAGS} \
    ${ARCH_FPU_FLAGS} \
" CACHE INTERNAL "ASM Compiler options")

# The linker script (LINKER_SCRIPT) is added with target_link_options in
# cmake/CMakeLists.txt, not appended to CMAKE_EXE_LINKER_FLAGS: that cache
# entry persists across cmake runs, so appending duplicated -T on every
# re-run.
