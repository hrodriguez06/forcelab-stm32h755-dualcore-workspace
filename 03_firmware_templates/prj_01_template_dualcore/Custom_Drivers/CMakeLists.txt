# ---------------------------------------------------------------------------
# Custom_Drivers - three-tier peripheral drivers, staging area before a driver
# is promoted to 01_shared_drivers/.
#
# INTERFACE, not STATIC: the sources are compiled into whichever core target
# consumes them, so CM7 gets them with the M7 flags and its HAL configuration.
# A static library would have to pick one core's flags for both.
#
# The NN_ prefix is the driver's slot in the board bring-up sequence and
# matches 01_shared_drivers/ exactly, so promoting a driver is a plain move.
# It never appears in an #include: each directory is on the include path.
#
# Nothing here is linked into CPU2, on purpose. Both drivers drive hardware
# that CPU1 owns - the three user LEDs and the ST-LINK console UART - and a
# peripheral has exactly one owner. Giving CPU2 led_bm as well would have both
# cores configuring the same GPIO port, which is the bug this layout exists to
# prevent. CPU2's liveness is visible through g_shared.m4_heartbeat instead.
# ---------------------------------------------------------------------------

add_library(custom_drivers INTERFACE)

target_sources(custom_drivers INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/01_led/led_bm.c
    ${CMAKE_CURRENT_LIST_DIR}/04_uart/uart_hal.c
)

target_include_directories(custom_drivers INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/01_led
    ${CMAKE_CURRENT_LIST_DIR}/04_uart
)
