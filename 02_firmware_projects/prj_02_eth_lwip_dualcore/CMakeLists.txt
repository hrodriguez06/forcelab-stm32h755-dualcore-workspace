# ---------------------------------------------------------------------------
# prj_02_eth_lwip_dualcore - Dual-core STM32H755ZI product with Ethernet / lwIP.
#
# Builds BOTH images in one configure:
#   cm7_firmware.elf  Flash bank 1 (0x08000000), FreeRTOS + lwIP Ethernet, owns clocks
#   cm4_firmware.elf  Flash bank 2 (0x08100000), bare metal, released by HSEM 0
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.22)

project(prj_02_eth_lwip_dualcore C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/common_flags.cmake)

# ---- External dependencies -------------------------------------------------
# The Cube package is a submodule of the workspace, shared by every project
# rather than checked out once per product: it is 110 MB and identical for all
# of them. Override CUBE_DIR to point somewhere else.
set(CUBE_DIR "${CMAKE_CURRENT_LIST_DIR}/../prj_eth_lwip_node/external/STM32CubeH7"
    CACHE PATH "STM32CubeH7 checkout to build against")

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
