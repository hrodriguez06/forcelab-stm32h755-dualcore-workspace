# ---------------------------------------------------------------------------
# prj_01_template_dualcore - golden base for an STM32H755ZI product.
#
# Builds BOTH images in one configure:
#   cm7_firmware.elf  Flash bank 1 (0x08000000), FreeRTOS, owns the clock tree
#   cm4_firmware.elf  Flash bank 2 (0x08100000), bare metal, released by HSEM 0
#
# Copy this directory into 02_firmware_projects/, rename the project, and
# delete whichever core the product does not need. Deleting CM4 also means
# setting CM4_BOOT_ENABLED to 0 in CM7/Core/Inc/main.h - CPU1 otherwise waits
# for a core that never parks.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.22)

project(prj_01_template_dualcore C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/common_flags.cmake)

# ---- Driver selftests --------------------------------------------------------
# 00_gpio: BM, LL and HAL tiers on both cores, plus the Analog Discovery 2 bench
# of 01_shared_drivers/00_gpio/README.md. Preset selftest-gpio; never in a
# product image.
option(GPIO_DRIVER_SELFTEST "Build the 00_gpio selftest (needs the AD2 bench)" OFF)
# 03_tim: the Analog Discovery 2 bench of 01_shared_drivers/03_tim/
# IMPLEMENTATION_PLAN.md. Preset selftest-tim; never in a product image. Both
# selftests drive the same bench lines from their own step tables, so one
# image never carries both.
option(TIM_DRIVER_SELFTEST "Build the 03_tim selftest (needs the AD2 bench)" OFF)
if(GPIO_DRIVER_SELFTEST AND TIM_DRIVER_SELFTEST)
    message(FATAL_ERROR
        "GPIO_DRIVER_SELFTEST and TIM_DRIVER_SELFTEST are incompatible: each owns the "
        "AD2 bench lines through its own step table. Configure one of them (presets "
        "selftest-gpio or selftest-tim).")
endif()
# 06_spi: the Analog Discovery 2 bench and the J1 jumper of 01_shared_drivers/
# 06_spi/IMPLEMENTATION_PLAN.md. Preset selftest-spi; never in a product image.
# It drives the same bench lines from its own step table, so one image never
# carries it together with another selftest.
option(SPI_DRIVER_SELFTEST "Build the 06_spi selftest (needs the AD2 bench and J1)" OFF)
if(SPI_DRIVER_SELFTEST AND (GPIO_DRIVER_SELFTEST OR TIM_DRIVER_SELFTEST))
    message(FATAL_ERROR
        "SPI_DRIVER_SELFTEST is incompatible with GPIO_DRIVER_SELFTEST and TIM_DRIVER_SELFTEST: "
        "each owns the AD2 bench lines through its own step table. Configure one of them "
        "(presets selftest-gpio, selftest-tim or selftest-spi).")
endif()

# Every selftest capture starts with the revision it was built from, and says
# -dirty when tracked files differ from it, so a capture can be traced to a
# commit (and one from an uncommitted tree cannot pass for one). Both, and a
# build id over the diff, the C flags and the preset (R3-a), are computed at
# BUILD time into generated/fw_build_id.h: computed at configure time, an edit
# made after `cmake --preset` compiled with the old revision and no -dirty.
set(FW_PRESET "" CACHE STRING "Configure preset name, hashed into FW_BUILD_ID (set by CMakePresets.json)")
add_custom_target(fw_build_id ALL
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_CURRENT_LIST_DIR} -DBIN_DIR=${CMAKE_BINARY_DIR}
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/fw_build_id.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/fw_build_id.h
    COMMENT "fw_build_id.h (FW_GIT_REV, FW_BUILD_ID)"
    VERBATIM
)

# ---- External dependencies -------------------------------------------------
# The Cube package is a submodule of the workspace, shared by every project
# rather than checked out once per product: it is 110 MB and identical for all
# of them. Override CUBE_DIR to point somewhere else.
set(CUBE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7"
    CACHE PATH "STM32CubeH7 checkout to build against")

set(CMSIS_DIR      ${CUBE_DIR}/Drivers/CMSIS)
set(HAL_DIR        ${CUBE_DIR}/Drivers/STM32H7xx_HAL_Driver)
set(FREERTOS_DIR   ${CUBE_DIR}/Middlewares/Third_Party/FreeRTOS/Source)

if(NOT EXISTS ${HAL_DIR}/Src/stm32h7xx_hal_rcc.c)
    message(FATAL_ERROR
        "STM32CubeH7 is missing or incomplete at ${CUBE_DIR}.\n"
        "Restore it from the workspace root with:\n"
        "    .\\tools\\init_cube_submodule.ps1\n"
        "Do NOT use 'git submodule update --init --recursive'; see that "
        "script's header for why.")
endif()

# ---- Shared sources --------------------------------------------------------
# Compiled into whichever core consumes them, so each keeps its own flags,
# its own HAL configuration and its own section placement.
# ---- Shared Drivers (01_shared_drivers) ------------------------------------
# This template consumes the central library rather than vendoring copies, so a
# project cloned from it inherits every errata fix made there. Write a driver
# into a project-local Custom_Drivers/ only when it is specific to that product.
if(NOT DEFINED SHARED_DRIVERS_DIR)
    if(DEFINED ENV{STM32_SHARED_DRIVERS})
        set(SHARED_DRIVERS_DIR "$ENV{STM32_SHARED_DRIVERS}")
    else()
        set(SHARED_DRIVERS_DIR "${CMAKE_CURRENT_LIST_DIR}/../../01_shared_drivers")
    endif()
endif()

if(NOT EXISTS "${SHARED_DRIVERS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "
[ERROR] Shared drivers not found at: ${SHARED_DRIVERS_DIR}
"
        "Fix:
"
        "  1. Verify workspace relative path: ../../01_shared_drivers
"
        "  2. Or provide CLI argument: cmake -DSHARED_DRIVERS_DIR=<path> ..
"
        "  3. Or define environment variable STM32_SHARED_DRIVERS
"
    )
endif()

add_subdirectory("${SHARED_DRIVERS_DIR}" "${CMAKE_BINARY_DIR}/shared_drivers")

# ---- The two cores ---------------------------------------------------------
add_subdirectory(CM7)
add_subdirectory(CM4)

# Both images include generated/fw_build_id.h, written before they compile.
foreach(fw_target cm7_firmware cm4_firmware)
    add_dependencies(${fw_target} fw_build_id)
    target_include_directories(${fw_target} PRIVATE ${CMAKE_BINARY_DIR}/generated)
endforeach()
