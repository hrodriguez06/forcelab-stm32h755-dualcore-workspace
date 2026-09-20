# ---------------------------------------------------------------------------
# CPU1 (Cortex-M7) image: Flash bank 1 at 0x08000000.
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
    # ST's lwIP sys_arch is written against CMSIS-RTOS v2, which is why this
    # wrapper is in the build and why configMAX_PRIORITIES is 56.
    ${FREERTOS_DIR}/CMSIS_RTOS_V2/cmsis_os2.c
)

# src/api holds the tcpip thread (tcpip.c), which NO_SYS==0 needs whether or
# not the netconn and socket APIs are enabled. The rest of that directory is
# guarded by LWIP_NETCONN / LWIP_SOCKET and compiles to nothing while both are
# off, so the whole directory is globbed rather than cherry-picked.
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
    # cmsis_os2.h is the ARM CMSIS-RTOS2 API header and ships in the CMSIS
    # pack, not in stm32-mw-freertos, which only provides the FreeRTOS
    # implementation of it (cmsis_os2.c + freertos_os2.h).
    ${CMSIS_DIR}/RTOS2/Include

    ${LWIP_DIR}/src/include
    ${LWIP_DIR}/system
)

target_compile_definitions(${TARGET} PRIVATE
    ${COMMON_DEFINITIONS}
    # Single source of truth for the checksum policy: eth_hal.c programs the
    # MAC from it and lwipopts.h switches the CHECKSUM_* options from it.
    ETH_USE_HARDWARE_CHECKSUM=0
    # freertos_os2.h does `#include CMSIS_device_header` and leaves the choice
    # of device header to the build system.
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
    # binutils >= 2.39 warns about any PT_LOAD that is both writable and
    # executable. On a Cortex-M with no MMU the flash image legitimately
    # carries .text alongside the initialisers for .data and .itcm, so the
    # warning carries no information here.
    -Wl,--no-warn-rwx-segments
)

target_link_libraries(${TARGET} PRIVATE custom_drivers)

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
