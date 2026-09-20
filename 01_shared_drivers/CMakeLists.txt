# ---------------------------------------------------------------------------
# STM32H755 Shared Drivers Library
#
# Configured as an INTERFACE library: sources are compiled into whichever
# core target (CM7 or CM4) consumes them, preserving core-specific compilation
# flags, MPU attributes, and HAL configurations.
# Reference: 00_docs_stm32h7/DRIVER_ARCHITECTURE_GUIDELINES.md
#
# Directories carry a numeric prefix that encodes BRING-UP ORDER, not the
# alphabet: 00_gpio is what everything else configures its pins through,
# 10_hsem is the last thing to come up because it needs both cores alive.
# Read the list top to bottom and you have the init sequence of a board.
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.22)

add_library(stm32h7_driver_led INTERFACE)
target_include_directories(stm32h7_driver_led INTERFACE ${CMAKE_CURRENT_LIST_DIR}/01_led)
target_sources(stm32h7_driver_led INTERFACE ${CMAKE_CURRENT_LIST_DIR}/01_led/led_bm.c)

add_library(stm32h7_driver_uart INTERFACE)
target_include_directories(stm32h7_driver_uart INTERFACE ${CMAKE_CURRENT_LIST_DIR}/04_uart)
target_sources(stm32h7_driver_uart INTERFACE ${CMAKE_CURRENT_LIST_DIR}/04_uart/uart_hal.c)

add_library(stm32h7_driver_eth INTERFACE)
target_include_directories(stm32h7_driver_eth INTERFACE ${CMAKE_CURRENT_LIST_DIR}/08_eth)
target_sources(stm32h7_driver_eth INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/08_eth/eth_hal.c
    ${CMAKE_CURRENT_LIST_DIR}/08_eth/lan8742_phy.c
)

# Umbrella interface library, linking all available shared drivers for backwards compatibility
add_library(stm32h7_shared_drivers INTERFACE)
target_link_libraries(stm32h7_shared_drivers INTERFACE
    stm32h7_driver_led
    stm32h7_driver_uart
    stm32h7_driver_eth
)

# Include directories across all peripheral slots in bring-up order
target_include_directories(stm32h7_shared_drivers INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/00_gpio     # pin muxing - prerequisite of everything below
    ${CMAKE_CURRENT_LIST_DIR}/01_led      # boot diagnostics, usable before the clock tree
    ${CMAKE_CURRENT_LIST_DIR}/02_dma      # alignment + cache helpers, needed by 04..09
    ${CMAKE_CURRENT_LIST_DIR}/03_tim      # time base
    ${CMAKE_CURRENT_LIST_DIR}/04_uart     # console / log - first thing you want talking
    ${CMAKE_CURRENT_LIST_DIR}/05_i2c
    ${CMAKE_CURRENT_LIST_DIR}/06_spi
    ${CMAKE_CURRENT_LIST_DIR}/07_adc
    ${CMAKE_CURRENT_LIST_DIR}/08_eth      # depends on 02_dma (descriptors in SRAM3)
    ${CMAKE_CURRENT_LIST_DIR}/09_fdcan
    ${CMAKE_CURRENT_LIST_DIR}/10_hsem     # M7 <-> M4 IPC - needs both cores running
)
