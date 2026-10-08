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

# 00_gpio is the bottom of the stack: everything that drives a pin needs its
# port clocked first, so the drivers below link it rather than each carrying a
# private copy of the same register write.
add_library(stm32h7_driver_gpio_bm INTERFACE)
target_include_directories(stm32h7_driver_gpio_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/00_gpio)
target_sources(stm32h7_driver_gpio_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/00_gpio/gpio_bm.c)

# Compatibility: every driver below links this name and it always meant the
# _bm tier. No sources of its own, so gpio_bm.c is compiled once.
add_library(stm32h7_driver_gpio INTERFACE)
target_link_libraries(stm32h7_driver_gpio INTERFACE stm32h7_driver_gpio_bm)

# LL and HAL tiers. Both link the BM tier, which keeps the single copy of the
# validation, the port clock and the cross-core section they run in.
add_library(stm32h7_driver_gpio_ll INTERFACE)
target_include_directories(stm32h7_driver_gpio_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/00_gpio)
target_sources(stm32h7_driver_gpio_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/00_gpio/gpio_ll.c)
target_link_libraries(stm32h7_driver_gpio_ll INTERFACE stm32h7_driver_gpio_bm)

add_library(stm32h7_driver_gpio_hal INTERFACE)
target_include_directories(stm32h7_driver_gpio_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/00_gpio)
target_sources(stm32h7_driver_gpio_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/00_gpio/gpio_hal.c)
target_link_libraries(stm32h7_driver_gpio_hal INTERFACE stm32h7_driver_gpio_bm)

add_library(stm32h7_driver_led INTERFACE)
target_include_directories(stm32h7_driver_led INTERFACE ${CMAKE_CURRENT_LIST_DIR}/01_led)
target_sources(stm32h7_driver_led INTERFACE ${CMAKE_CURRENT_LIST_DIR}/01_led/led_bm.c)
target_link_libraries(stm32h7_driver_led INTERFACE stm32h7_driver_gpio)

add_library(stm32h7_driver_dma_bm INTERFACE)
target_include_directories(stm32h7_driver_dma_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/02_dma)
target_sources(stm32h7_driver_dma_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/02_dma/dma_bm.c)

# Compatibility: this name predates the tiers and always meant the _bm tier
# (prj_04 CM4, stm32h7_driver_adc_bm). Unlike uart/adc, the unsuffixed DMA
# target is therefore BM, not HAL. It carries no sources of its own, so a
# project reaching dma_bm.c through both names compiles it once.
add_library(stm32h7_driver_dma INTERFACE)
target_link_libraries(stm32h7_driver_dma INTERFACE stm32h7_driver_dma_bm)

add_library(stm32h7_driver_dma_ll INTERFACE)
target_include_directories(stm32h7_driver_dma_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/02_dma)
target_sources(stm32h7_driver_dma_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/02_dma/dma_ll.c)
target_link_libraries(stm32h7_driver_dma_ll INTERFACE stm32h7_driver_dma_bm)

add_library(stm32h7_driver_dma_hal INTERFACE)
target_include_directories(stm32h7_driver_dma_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/02_dma)
target_sources(stm32h7_driver_dma_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/02_dma/dma_hal.c)
target_link_libraries(stm32h7_driver_dma_hal INTERFACE stm32h7_driver_dma_bm)

# 03_tim: TIM1-8, TIM12-17. Like 10_hsem (rule C4) there is no unsuffixed
# target, and the umbrella below does not link the tiers: a project picks one.
# stm32h7_driver_tim_bm holds the single capability table and validation; the
# LL and HAL tiers will link it (03_tim plan, phases F2 and F3).
add_library(stm32h7_driver_tim_bm INTERFACE)
target_include_directories(stm32h7_driver_tim_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/03_tim)
target_sources(stm32h7_driver_tim_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/03_tim/tim_bm.c)

add_library(stm32h7_driver_tim_ll INTERFACE)
target_include_directories(stm32h7_driver_tim_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/03_tim)
target_sources(stm32h7_driver_tim_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/03_tim/tim_ll.c)
target_link_libraries(stm32h7_driver_tim_ll INTERFACE stm32h7_driver_tim_bm)

add_library(stm32h7_driver_tim_hal INTERFACE)
target_include_directories(stm32h7_driver_tim_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/03_tim)
target_sources(stm32h7_driver_tim_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/03_tim/tim_hal.c)
target_link_libraries(stm32h7_driver_tim_hal INTERFACE stm32h7_driver_tim_bm)

add_library(stm32h7_driver_uart INTERFACE)
target_include_directories(stm32h7_driver_uart INTERFACE ${CMAKE_CURRENT_LIST_DIR}/04_uart)
target_sources(stm32h7_driver_uart INTERFACE ${CMAKE_CURRENT_LIST_DIR}/04_uart/uart_hal.c)
target_link_libraries(stm32h7_driver_uart INTERFACE stm32h7_driver_gpio)

add_library(stm32h7_driver_uart_bm INTERFACE)
target_include_directories(stm32h7_driver_uart_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/04_uart)
target_sources(stm32h7_driver_uart_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/04_uart/uart_bm.c)
target_link_libraries(stm32h7_driver_uart_bm INTERFACE stm32h7_driver_gpio)

add_library(stm32h7_driver_uart_ll INTERFACE)
target_include_directories(stm32h7_driver_uart_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/04_uart)
target_sources(stm32h7_driver_uart_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/04_uart/uart_ll.c)
target_link_libraries(stm32h7_driver_uart_ll INTERFACE stm32h7_driver_gpio)


# 05_i2c: I2C1-I2C4, master. No unsuffixed target (rule C4) and the umbrella below
# does not link the tiers: a project picks one. stm32h7_driver_i2c_bm holds the single
# capability table and validation; the LL and HAL tiers will link it (05_i2c plan,
# phases F2 and F3).
add_library(stm32h7_driver_i2c_bm INTERFACE)
target_include_directories(stm32h7_driver_i2c_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/05_i2c)
target_sources(stm32h7_driver_i2c_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/05_i2c/i2c_bm.c)

add_library(stm32h7_driver_i2c_ll INTERFACE)
target_include_directories(stm32h7_driver_i2c_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/05_i2c)
target_sources(stm32h7_driver_i2c_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/05_i2c/i2c_ll.c)
target_link_libraries(stm32h7_driver_i2c_ll INTERFACE stm32h7_driver_i2c_bm)

# 06_spi: SPI1-SPI6. No unsuffixed target (rule C4) and the umbrella below does not
# link the tiers: a project picks one. stm32h7_driver_spi_bm holds the single
# capability table and validation; the LL and HAL tiers will link it (06_spi plan,
# phases F2 and F3).
add_library(stm32h7_driver_spi_bm INTERFACE)
target_include_directories(stm32h7_driver_spi_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/06_spi)
target_sources(stm32h7_driver_spi_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/06_spi/spi_bm.c)

add_library(stm32h7_driver_spi_ll INTERFACE)
target_include_directories(stm32h7_driver_spi_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/06_spi)
target_sources(stm32h7_driver_spi_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/06_spi/spi_ll.c)
target_link_libraries(stm32h7_driver_spi_ll INTERFACE stm32h7_driver_spi_bm)

add_library(stm32h7_driver_spi_hal INTERFACE)
target_include_directories(stm32h7_driver_spi_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/06_spi)
target_sources(stm32h7_driver_spi_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/06_spi/spi_hal.c)
target_link_libraries(stm32h7_driver_spi_hal INTERFACE stm32h7_driver_spi_bm)

add_library(stm32h7_driver_adc INTERFACE)
target_include_directories(stm32h7_driver_adc INTERFACE ${CMAKE_CURRENT_LIST_DIR}/07_adc)
target_sources(stm32h7_driver_adc INTERFACE ${CMAKE_CURRENT_LIST_DIR}/07_adc/adc_hal.c)
target_link_libraries(stm32h7_driver_adc INTERFACE stm32h7_driver_gpio)

add_library(stm32h7_driver_adc_bm INTERFACE)
target_include_directories(stm32h7_driver_adc_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/07_adc)
target_sources(stm32h7_driver_adc_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/07_adc/adc_bm.c)
target_link_libraries(stm32h7_driver_adc_bm INTERFACE stm32h7_driver_gpio stm32h7_driver_dma)

add_library(stm32h7_driver_adc_ll INTERFACE)
target_include_directories(stm32h7_driver_adc_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/07_adc)
target_sources(stm32h7_driver_adc_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/07_adc/adc_ll.c)
target_link_libraries(stm32h7_driver_adc_ll INTERFACE stm32h7_driver_gpio stm32h7_driver_dma)

add_library(stm32h7_driver_eth INTERFACE)
target_include_directories(stm32h7_driver_eth INTERFACE ${CMAKE_CURRENT_LIST_DIR}/08_eth)
target_sources(stm32h7_driver_eth INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/08_eth/eth_hal.c
    ${CMAKE_CURRENT_LIST_DIR}/08_eth/lan8742_phy.c
)
target_link_libraries(stm32h7_driver_eth INTERFACE stm32h7_driver_gpio)

# 10_hsem: Hardware Semaphore for dual-core mutual exclusion & boot synchronization.
# Rule C4: There is deliberately no unsuffixed stm32h7_driver_hsem target (no legacy
# target name existed, and unsuffixed means BM in dma/gpio vs HAL in uart/adc).
# stm32h7_driver_hsem_bm is the single source of truth for validation, barriers,
# DWT cycle timebase, and boot handshake. The LL and HAL tiers link against BM.
add_library(stm32h7_driver_hsem_bm INTERFACE)
target_include_directories(stm32h7_driver_hsem_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/10_hsem)
target_sources(stm32h7_driver_hsem_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/10_hsem/hsem_bm.c)

add_library(stm32h7_driver_hsem_ll INTERFACE)
target_include_directories(stm32h7_driver_hsem_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/10_hsem)
target_sources(stm32h7_driver_hsem_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/10_hsem/hsem_ll.c)
target_link_libraries(stm32h7_driver_hsem_ll INTERFACE stm32h7_driver_hsem_bm)

add_library(stm32h7_driver_hsem_hal INTERFACE)
target_include_directories(stm32h7_driver_hsem_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/10_hsem)
target_sources(stm32h7_driver_hsem_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/10_hsem/hsem_hal.c)
target_link_libraries(stm32h7_driver_hsem_hal INTERFACE stm32h7_driver_hsem_bm)


# Umbrella interface library, linking all available shared drivers for backwards compatibility
add_library(stm32h7_shared_drivers INTERFACE)
target_link_libraries(stm32h7_shared_drivers INTERFACE
    stm32h7_driver_gpio
    stm32h7_driver_led
    stm32h7_driver_dma
    stm32h7_driver_uart
    stm32h7_driver_adc
    stm32h7_driver_eth
)

# Include directories across all peripheral slots in bring-up order
target_include_directories(stm32h7_shared_drivers INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/00_gpio     # pin muxing - prerequisite of everything below
    ${CMAKE_CURRENT_LIST_DIR}/01_led      # boot diagnostics, usable before the clock tree
    ${CMAKE_CURRENT_LIST_DIR}/02_dma      # DMA1/DMA2/BDMA tiers + dma_mem_bm.h (alignment, cache, reachability)
    ${CMAKE_CURRENT_LIST_DIR}/03_tim      # time base
    ${CMAKE_CURRENT_LIST_DIR}/04_uart     # console / log - first thing you want talking
    ${CMAKE_CURRENT_LIST_DIR}/05_i2c
    ${CMAKE_CURRENT_LIST_DIR}/06_spi
    ${CMAKE_CURRENT_LIST_DIR}/07_adc
    ${CMAKE_CURRENT_LIST_DIR}/08_eth      # own ETH DMA; descriptors in SRAM3 - does not link 02_dma
    ${CMAKE_CURRENT_LIST_DIR}/09_fdcan
    ${CMAKE_CURRENT_LIST_DIR}/10_hsem     # M7 <-> M4 IPC - needs both cores running
)
