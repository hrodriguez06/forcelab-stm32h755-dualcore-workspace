# ---------------------------------------------------------------------------
# Custom_Drivers - three-tier peripheral drivers (README.md section 10).
#
# Built as an INTERFACE library: the sources are compiled into whichever core
# target consumes them, so each core keeps its own flags, its own HAL
# configuration and its own section placement.
#
# The NN_ prefix is the driver's slot in the board bring-up sequence and it
# matches 01_shared_drivers/ exactly, so promoting a driver out of this project
# is a plain move. The gaps (02, 03, 05..07, 09, 10) are the slots this product
# does not use yet. The prefix never appears in an #include: each directory is
# on the include path, so it stays #include "eth_hal.h".
# ---------------------------------------------------------------------------

add_library(custom_drivers INTERFACE)

target_sources(custom_drivers INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/01_led/led_bm.c
    ${CMAKE_CURRENT_LIST_DIR}/04_uart/uart_hal.c
    ${CMAKE_CURRENT_LIST_DIR}/08_eth/eth_hal.c
    ${CMAKE_CURRENT_LIST_DIR}/08_eth/lan8742_phy.c
)

target_include_directories(custom_drivers INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/01_led
    ${CMAKE_CURRENT_LIST_DIR}/04_uart
    ${CMAKE_CURRENT_LIST_DIR}/08_eth
)
