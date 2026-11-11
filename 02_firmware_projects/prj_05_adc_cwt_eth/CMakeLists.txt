# ---------------------------------------------------------------------------
# prj_05_adc_cwt_eth - Dual-core STM32H755ZI Continuous Wavelet Transform (CWT)
#                      Bare-Metal ADC1+DMA1+TIM3 on CM4 (500 kSPS, 1.5V DC offset),
#                      Complex Morlet CWT DSP + FreeRTOS + lwIP Ethernet on CM7.
#
# Builds BOTH images in one configure:
#   cm7_firmware.elf  Flash bank 1 (0x08000000), FreeRTOS + CWT + lwIP Ethernet, owns clocks
#   cm4_firmware.elf  Flash bank 2 (0x08100000), bare metal ADC1+DMA1+TIM3, released by HSEM 0
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.22)

project(prj_05_adc_cwt_eth C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/common_flags.cmake)

# ---- Git revision injection for traceability ------------------------------
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_REV)
    set(GIT_REV "unknown")
else()
    execute_process(
        COMMAND git status --porcelain -uno
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE GIT_STATUS_OUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(GIT_STATUS_OUT)
        set(GIT_REV "${GIT_REV}-dirty")
    endif()
endif()
add_compile_definitions(FW_GIT_REV="${GIT_REV}")

# ---- External dependencies -------------------------------------------------
if(NOT DEFINED CUBE_DIR)
    if(DEFINED ENV{STM32CUBEH7_DIR})
        set(CUBE_DIR "$ENV{STM32CUBEH7_DIR}")
    else()
        set(CUBE_DIR "${CMAKE_CURRENT_LIST_DIR}/../prj_eth_lwip_node/external/STM32CubeH7")
    endif()
endif()
set(CUBE_DIR "${CUBE_DIR}" CACHE PATH "STM32CubeH7 checkout to build against")

set(CMSIS_DIR      ${CUBE_DIR}/Drivers/CMSIS)
set(HAL_DIR        ${CUBE_DIR}/Drivers/STM32H7xx_HAL_Driver)
set(FREERTOS_DIR   ${CUBE_DIR}/Middlewares/Third_Party/FreeRTOS/Source)
set(LWIP_DIR       ${CUBE_DIR}/Middlewares/Third_Party/LwIP)

if(NOT EXISTS ${HAL_DIR}/Src/stm32h7xx_hal_eth.c)
    message(FATAL_ERROR
        "STM32CubeH7 is missing or incomplete at ${CUBE_DIR}.\n"
        "Restore it from the workspace root with:\n"
        "    .\\tools\\init_cube_submodule.ps1\n"
        "Do NOT use 'git submodule update --init --recursive'; see that "
        "script's header for why.")
endif()

# ---- Shared Drivers (01_shared_drivers) ------------------------------------
if(NOT DEFINED SHARED_DRIVERS_DIR)
    if(DEFINED ENV{STM32_SHARED_DRIVERS})
        set(SHARED_DRIVERS_DIR "$ENV{STM32_SHARED_DRIVERS}")
    else()
        set(SHARED_DRIVERS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../01_shared_drivers")
    endif()
endif()

if(NOT EXISTS "${SHARED_DRIVERS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "\n[ERROR] Shared drivers not found at: ${SHARED_DRIVERS_DIR}\n"
        "Fix:\n"
        "  1. Verify workspace relative path: ../../01_shared_drivers\n"
        "  2. Or provide CLI argument: cmake -DSHARED_DRIVERS_DIR=<path> ..\n"
        "  3. Or define environment variable STM32_SHARED_DRIVERS\n"
    )
endif()

add_subdirectory("${SHARED_DRIVERS_DIR}" "${CMAKE_BINARY_DIR}/shared_drivers")

# ---- The two cores ---------------------------------------------------------
add_subdirectory(CM7)
add_subdirectory(CM4)
