# ---------------------------------------------------------------------------
# CPU1 (Cortex-M7) image: Flash bank 1 at 0x08000000, FreeRTOS.
#
# CPU1 owns the clock tree, the power supply and the console, and releases
# CPU2 through HSEM 0 once all three are up. See CM7/Core/Src/main.c.
# ---------------------------------------------------------------------------

set(TARGET cm7_firmware)

set(LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/linker/stm32h755zi_cm7.ld)

# ---- Third-party sources ---------------------------------------------------
# The HAL modules that are not enabled in stm32h7xx_hal_conf.h compile to
# nothing, so globbing the whole Src directory costs no flash.
file(GLOB HAL_SOURCES ${HAL_DIR}/Src/*.c)
list(FILTER HAL_SOURCES EXCLUDE REGEX ".*_template\\.c$")

set(FREERTOS_SOURCES
    ${FREERTOS_DIR}/croutine.c
    ${FREERTOS_DIR}/event_groups.c
    ${FREERTOS_DIR}/list.c
    ${FREERTOS_DIR}/queue.c
    ${FREERTOS_DIR}/stream_buffer.c
    ${FREERTOS_DIR}/tasks.c
    ${FREERTOS_DIR}/timers.c
    ${FREERTOS_DIR}/portable/GCC/ARM_CM7/r0p1/port.c
    ${FREERTOS_DIR}/portable/MemMang/heap_4.c
)

# ---- Project sources -------------------------------------------------------
set(PROJECT_SOURCES
    ${CMSIS_DIR}/Device/ST/STM32H7xx/Source/Templates/system_stm32h7xx.c
    ${CMSIS_DIR}/Device/ST/STM32H7xx/Source/Templates/gcc/startup_stm32h755xx.s

    ${CMAKE_CURRENT_LIST_DIR}/Core/Src/main.c
    ${CMAKE_CURRENT_LIST_DIR}/Core/Src/stm32h7xx_it.c
    ${CMAKE_CURRENT_LIST_DIR}/Core/Src/stm32h7xx_hal_timebase_tim.c
    ${CMAKE_CURRENT_LIST_DIR}/Core/Src/syscalls.c

    ${CMAKE_CURRENT_LIST_DIR}/App/Src/app_main.c
    ${CMAKE_CURRENT_LIST_DIR}/App/Src/app_log.c
    ${CMAKE_CURRENT_LIST_DIR}/App/Src/mpu_config.c

    # The single definition of the shared block lives on this core.
    ${CMAKE_SOURCE_DIR}/Common/Src/shared_memory.c
)

add_executable(${TARGET}
    ${PROJECT_SOURCES}
    ${HAL_SOURCES}
    ${FREERTOS_SOURCES}
)

target_include_directories(${TARGET} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/Core/Inc
    ${CMAKE_CURRENT_LIST_DIR}/App/Inc
    ${CMAKE_SOURCE_DIR}/Common/Inc

    ${CMSIS_DIR}/Core/Include
    ${CMSIS_DIR}/Device/ST/STM32H7xx/Include
    ${HAL_DIR}/Inc

    ${FREERTOS_DIR}/include
    ${FREERTOS_DIR}/portable/GCC/ARM_CM7/r0p1
)

target_compile_definitions(${TARGET} PRIVATE
    ${COMMON_DEFINITIONS}
    CORE_CM7
)

target_compile_options(${TARGET} PRIVATE
    ${CPU_FLAGS_CM7}
    ${COMMON_WARNING_FLAGS}
    ${COMMON_C_FLAGS}
    $<$<CONFIG:Debug>:-Og -g3>
    $<$<CONFIG:Release>:-O2 -g>
)

target_link_options(${TARGET} PRIVATE
    ${CPU_FLAGS_CM7}
    -T${LINKER_SCRIPT}
    --specs=nano.specs
    -Wl,--gc-sections
    -Wl,-Map=${TARGET}.map,--cref
    -Wl,--print-memory-usage
    # binutils >= 2.39 warns about any PT_LOAD that is both writable and
    # executable. On a Cortex-M with no MMU the flash image legitimately
    # carries .text alongside the initialisers for .data and .itcm, so the
    # warning carries no information here.
    -Wl,--no-warn-rwx-segments
)

# CPU1 owns the three user LEDs and the ST-LINK console UART, so it links those
# two drivers and nothing else. CPU2 links none: a peripheral has exactly one
# owner, and its liveness shows up through g_shared.m4_heartbeat instead.
target_link_libraries(${TARGET} PRIVATE
    stm32h7_driver_led
    stm32h7_driver_uart
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

# 03_tim selftest: gpio_bm (linked above for the lock hook) for the bench, and,
# only with the option, the drivers under test (tim_bm since F1, tim_ll since F2,
# tim_hal since F3: CPU1 only, HAL_TIM_MODULE_ENABLED) and dma_bm (T13).
if(TIM_DRIVER_SELFTEST)
    target_compile_definitions(${TARGET} PRIVATE TIM_DRIVER_SELFTEST=1)
    target_sources(${TARGET} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/App/Src/tim_selftest.c
        ${CMAKE_SOURCE_DIR}/Common/Src/tim_selftest_common.c
    )
    target_link_libraries(${TARGET} PRIVATE
        stm32h7_driver_tim_bm
        stm32h7_driver_tim_ll
        stm32h7_driver_tim_hal
        stm32h7_driver_dma_bm
    )
endif()

# 06_spi selftest: gpio_bm (linked above for the lock hook) for the bench, and,
# only with the option, the driver under test (spi_bm since F1; the _spi_ll and
# _spi_hal targets join with their phases), dma_bm (DMA1 streams: T7(c), T13) and
# dma_hal (BDMA for SPI6, T7(b): the only 02_dma tier with BDMA, plan D7).
if(SPI_DRIVER_SELFTEST)
    target_compile_definitions(${TARGET} PRIVATE SPI_DRIVER_SELFTEST=1)
    target_sources(${TARGET} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/App/Src/spi_selftest.c
        ${CMAKE_SOURCE_DIR}/Common/Src/spi_selftest_common.c
    )
    target_link_libraries(${TARGET} PRIVATE
        stm32h7_driver_spi_bm
        stm32h7_driver_spi_ll
        stm32h7_driver_spi_hal
        stm32h7_driver_dma_bm
        stm32h7_driver_dma_hal
    )
endif()

# 05_i2c selftest. gpio_bm is linked above for the lock hook; F1 adds i2c_bm and,
# for T7(b) (I2C4 by BDMA, .d3_ram), dma_hal with dma_bm; F2 i2c_ll; F3 i2c_hal, CPU1
# only (HAL_I2C_MODULE_ENABLED with the option, plan decision 7).
if(I2C_DRIVER_SELFTEST)
    target_compile_definitions(${TARGET} PRIVATE I2C_DRIVER_SELFTEST=1)
    target_sources(${TARGET} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/App/Src/i2c_selftest.c
        ${CMAKE_SOURCE_DIR}/Common/Src/i2c_selftest_common.c
    )
    target_link_libraries(${TARGET} PRIVATE
        stm32h7_driver_i2c_bm
        stm32h7_driver_i2c_ll
        stm32h7_driver_i2c_hal
        stm32h7_driver_dma_bm
        stm32h7_driver_dma_hal
    )
endif()

set_target_properties(${TARGET} PROPERTIES
    OUTPUT_NAME ${TARGET}
    SUFFIX ".elf"
    LINK_DEPENDS ${LINKER_SCRIPT}
)

# Third-party code is not ours to keep warning-clean.
set_source_files_properties(
    ${HAL_SOURCES} ${FREERTOS_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-w"
)

add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET}> ${TARGET}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${TARGET}> ${TARGET}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${TARGET}>
    COMMENT "Generating ${TARGET}.bin / ${TARGET}.hex and reporting size"
)
