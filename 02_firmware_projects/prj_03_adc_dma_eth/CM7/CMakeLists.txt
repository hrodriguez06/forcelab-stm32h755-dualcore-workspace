# ---------------------------------------------------------------------------
# CPU1 (Cortex-M7) image: Flash bank 1 at 0x08000000, FreeRTOS + lwIP.
#
# CPU1 owns the clock tree, the power supply, the console and the Ethernet MAC,
# and releases CPU2 through HSEM 0 once all clocks are stable.
# ---------------------------------------------------------------------------

set(TARGET cm7_firmware)

set(LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/linker/stm32h755zi_cm7.ld)

# ---- Third-party sources ---------------------------------------------------
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
    # ST's lwIP sys_arch is written against CMSIS-RTOS v2
    ${FREERTOS_DIR}/CMSIS_RTOS_V2/cmsis_os2.c
)

file(GLOB LWIP_CORE_SOURCES
    ${LWIP_DIR}/src/core/*.c
    ${LWIP_DIR}/src/core/ipv4/*.c
    ${LWIP_DIR}/src/api/*.c
)
set(LWIP_SOURCES
    ${LWIP_CORE_SOURCES}
    ${LWIP_DIR}/src/netif/ethernet.c
    ${LWIP_DIR}/system/OS/sys_arch.c
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
    ${CMAKE_CURRENT_LIST_DIR}/App/Src/ethernetif.c
    ${CMAKE_CURRENT_LIST_DIR}/App/Src/net_task.c
    ${CMAKE_CURRENT_LIST_DIR}/App/Src/adc_server.c

    # Shared IPC block
    ${CMAKE_SOURCE_DIR}/Common/Src/shared_memory.c
)

add_executable(${TARGET}
    ${PROJECT_SOURCES}
    ${HAL_SOURCES}
    ${FREERTOS_SOURCES}
    ${LWIP_SOURCES}
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
    ${FREERTOS_DIR}/CMSIS_RTOS_V2
    ${CMSIS_DIR}/RTOS2/Include

    ${LWIP_DIR}/src/include
    ${LWIP_DIR}/system
)

target_compile_definitions(${TARGET} PRIVATE
    ${COMMON_DEFINITIONS}
    CORE_CM7
    ETH_USE_HARDWARE_CHECKSUM=0
    CMSIS_device_header="stm32h7xx.h"
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
    -Wl,--no-warn-rwx-segments
)

target_link_libraries(${TARGET} PRIVATE stm32h7_shared_drivers)

if(ADC_DRIVER_SELFTEST)
    target_compile_definitions(${TARGET} PRIVATE ADC_DRIVER_SELFTEST=1)
    target_link_libraries(${TARGET} PRIVATE stm32h7_driver_adc_bm)
endif()

set_target_properties(${TARGET} PROPERTIES
    OUTPUT_NAME ${TARGET}
    SUFFIX ".elf"
    LINK_DEPENDS ${LINKER_SCRIPT}
)

# Third-party code is not ours to keep warning-clean.
set_source_files_properties(
    ${HAL_SOURCES} ${FREERTOS_SOURCES} ${LWIP_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-w"
)

add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET}> ${TARGET}.bin
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${TARGET}> ${TARGET}.hex
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${TARGET}>
    COMMENT "Generating ${TARGET}.bin / ${TARGET}.hex and reporting size"
)
