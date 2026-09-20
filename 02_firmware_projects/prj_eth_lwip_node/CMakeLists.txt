# ---------------------------------------------------------------------------
# prj_eth_lwip_node - STM32H755ZI dual-core, Ethernet / lwIP node on CPU1.
#
# Only the Cortex-M7 image is built here: the Ethernet MAC, the TCP/IP stack
# and the networking tasks all live on CPU1 (README.md section 1, principle 2).
# A CM7/ sibling directory for CPU2 plugs in the same way when the product
# needs a real-time core.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.22)

project(prj_eth_lwip_node C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/common_flags.cmake)

# ---- External dependencies -------------------------------------------------
set(CUBE_DIR       ${CMAKE_CURRENT_LIST_DIR}/external/STM32CubeH7)
set(CMSIS_DIR      ${CUBE_DIR}/Drivers/CMSIS)
set(HAL_DIR        ${CUBE_DIR}/Drivers/STM32H7xx_HAL_Driver)
set(FREERTOS_DIR   ${CUBE_DIR}/Middlewares/Third_Party/FreeRTOS/Source)
set(LWIP_DIR       ${CUBE_DIR}/Middlewares/Third_Party/LwIP)

if(NOT EXISTS ${HAL_DIR}/Src/stm32h7xx_hal_eth.c)
    message(FATAL_ERROR
        "STM32CubeH7 is missing or incomplete at ${CUBE_DIR}.\n"
        "Restore it from the WORKSPACE root (the repository root, three levels up):\n"
        "    git submodule update --init --depth 1 "
            "02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7\n"
        "    cd 02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7\n"
        "    git sparse-checkout init --cone && git sparse-checkout set Drivers Middlewares\n"
        "    git submodule update --init --depth 1 -- "
            "Drivers/STM32H7xx_HAL_Driver Drivers/CMSIS/Device/ST/STM32H7xx "
            "Middlewares/Third_Party/LwIP Middlewares/Third_Party/FreeRTOS\n"
        "\n"
        "Do NOT use a plain --recursive: it checks out ST's Projects/ tree, whose "
        "paths exceed the Windows 260-character limit from here, and pulls 48 BSP "
        "repositories for boards this product does not use. "
        "See README.md section 6.")
endif()

add_subdirectory(Custom_Drivers)
add_subdirectory(CM7)
