# ---------------------------------------------------------------------------
# CPU2 (Cortex-M4) image: Flash bank 2 at 0x08100000, bare metal.
#
# No RTOS, by design: CPU2 exists for deterministic work, and a scheduler is
# the first thing that takes determinism away. See README.md section 2.
#
# The same startup file and the same system_stm32h7xx.c serve both cores - ST
# ships one per device, not one per core - and CORE_CM4 below is what selects
# the CPU2 paths inside them.
# ---------------------------------------------------------------------------

set(TARGET cm4_firmware)

set(LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/linker/stm32h755zi_cm4.ld)

# Cortex-M4 with the single-precision FPU. Not CPU_FLAGS_CM7: building CPU2
# code with -mcpu=cortex-m7 -mfpu=fpv5-d16 produces instructions this core
# does not have, and the failure appears at run time as a UsageFault.
set(CPU_FLAGS_CM4
    -mcpu=cortex-m4
    -mfpu=fpv4-sp-d16
    -mfloat-abi=hard
    -mthumb
)

file(GLOB HAL_SOURCES ${HAL_DIR}/Src/*.c)
list(FILTER HAL_SOURCES EXCLUDE REGEX ".*_template\\.c$")

set(PROJECT_SOURCES
    ${CMSIS_DIR}/Device/ST/STM32H7xx/Source/Templates/system_stm32h7xx.c
    ${CMSIS_DIR}/Device/ST/STM32H7xx/Source/Templates/gcc/startup_stm32h755xx.s

    ${CMAKE_CURRENT_LIST_DIR}/Core/Src/main.c
    ${CMAKE_CURRENT_LIST_DIR}/Core/Src/stm32h7xx_it.c

    # Same definition as the CPU1 image: see Common/Src/shared_memory.c.
    ${CMAKE_SOURCE_DIR}/Common/Src/shared_memory.c
)

add_executable(${TARGET}
    ${PROJECT_SOURCES}
    ${HAL_SOURCES}
)

target_include_directories(${TARGET} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/Core/Inc
    ${CMAKE_CURRENT_LIST_DIR}/App/Inc
    ${CMAKE_SOURCE_DIR}/Common/Inc

    ${CMSIS_DIR}/Core/Include
    ${CMSIS_DIR}/Device/ST/STM32H7xx/Include
    ${HAL_DIR}/Inc
)

# CORE_CM4 is what system_stm32h7xx.c and the HAL test to take the CPU2 paths.
# COMMON_DEFINITIONS is expanded by hand rather than reused because it carries
# CORE_CM7, which must NOT be defined here.
#
# DUAL_CORE is deliberately absent: stm32h755xx.h already defines it, and
# passing it on the command line as well produces a "DUAL_CORE redefined"
# warning on every translation unit.
target_compile_definitions(${TARGET} PRIVATE
    STM32H755xx
    USE_HAL_DRIVER
    CORE_CM4
)

target_compile_options(${TARGET} PRIVATE
    ${CPU_FLAGS_CM4}
    ${COMMON_WARNING_FLAGS}
    ${COMMON_C_FLAGS}
    $<$<CONFIG:Debug>:-Og -g3>
    $<$<CONFIG:Release>:-O2 -g>
)

target_link_options(${TARGET} PRIVATE
    ${CPU_FLAGS_CM4}
    -T${LINKER_SCRIPT}
    --specs=nano.specs
    # No --specs=nosys.specs. CPU2 calls nothing that needs a syscall, and
    # adding it only pulls in newlib's stubs, which each emit a link warning
    # ("_write is not implemented and will always fail"). CPU1 does not need it
    # either: it has Core/Src/syscalls.c. If CPU2 ever gains printf, give it the
    # same file rather than nosys - a stub that silently fails is worse than a
    # link error that tells you what is missing.
    -Wl,--gc-sections
    -Wl,-Map=${TARGET}.map,--cref
    -Wl,--print-memory-usage
    -Wl,--no-warn-rwx-segments
)

target_link_libraries(${TARGET} PRIVATE
    stm32h7_driver_hsem_bm
    stm32h7_driver_gpio_bm      # the cross-core GPIO lock hook in shared_memory.c
)

if(GPIO_DRIVER_SELFTEST)
    target_compile_definitions(${TARGET} PRIVATE GPIO_DRIVER_SELFTEST=1)
    target_sources(${TARGET} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/App/Src/gpio_selftest.c
        ${CMAKE_SOURCE_DIR}/Common/Src/gpio_selftest_common.c
    )
    target_link_libraries(${TARGET} PRIVATE
        stm32h7_driver_gpio_ll
        stm32h7_driver_gpio_hal
    )
endif()

# 03_tim selftest, CPU2 side (see CM7/CMakeLists.txt): TIM15 and T16 through tim_bm, TIM15 through tim_ll.
if(TIM_DRIVER_SELFTEST)
    target_compile_definitions(${TARGET} PRIVATE TIM_DRIVER_SELFTEST=1)
    target_sources(${TARGET} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/App/Src/tim_selftest.c
        ${CMAKE_SOURCE_DIR}/Common/Src/tim_selftest_common.c
    )
    target_link_libraries(${TARGET} PRIVATE
        stm32h7_driver_tim_bm
        stm32h7_driver_tim_ll
    )
endif()

# 06_spi selftest, CPU2 side (see CM7/CMakeLists.txt): SPI1 through spi_bm, and
# dma_bm for its DMA2 streams (T7(a)).
if(SPI_DRIVER_SELFTEST)
    target_compile_definitions(${TARGET} PRIVATE SPI_DRIVER_SELFTEST=1)
    target_sources(${TARGET} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/App/Src/spi_selftest.c
        ${CMAKE_SOURCE_DIR}/Common/Src/spi_selftest_common.c
    )
    target_link_libraries(${TARGET} PRIVATE
        stm32h7_driver_spi_bm
        stm32h7_driver_dma_bm
    )
endif()

set_target_properties(${TARGET} PROPERTIES
    OUTPUT_NAME ${TARGET}
    SUFFIX ".elf"
    LINK_DEPENDS ${LINKER_SCRIPT}
)

set_source_files_properties(
    ${HAL_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-w"
)

add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET}> ${TARGET}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${TARGET}> ${TARGET}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${TARGET}>
    COMMENT "Generating ${TARGET}.bin / ${TARGET}.hex and reporting size"
)
