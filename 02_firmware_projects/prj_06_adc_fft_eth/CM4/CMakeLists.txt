# ---------------------------------------------------------------------------
# CPU2 (Cortex-M4) image: Flash bank 2 at 0x08100000, bare metal.
#
# No RTOS, by design: CPU2 exists for deterministic low-noise ADC acquisition.
# Pure integer bare-metal acquisition engine (`*_bm` shared drivers only).
# ---------------------------------------------------------------------------

set(TARGET cm4_firmware)

set(LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/linker/stm32h755zi_cm4.ld)

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
    ${CMAKE_CURRENT_LIST_DIR}/App/Src/adc_acquisition.c

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
    -Wl,--gc-sections
    -Wl,-Map=${TARGET}.map,--cref
    -Wl,--print-memory-usage
    -Wl,--no-warn-rwx-segments
)

# CPU2 links bare-metal shared drivers (adc_bm, dma_bm, gpio_bm, hsem_bm, tim_bm)
target_link_libraries(${TARGET} PRIVATE
    stm32h7_driver_adc_bm
    stm32h7_driver_dma
    stm32h7_driver_gpio
    stm32h7_driver_hsem_bm
    stm32h7_driver_tim_bm
)

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
