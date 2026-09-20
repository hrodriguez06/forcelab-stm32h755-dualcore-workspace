# ---------------------------------------------------------------------------
# Flags shared by every target of this project.
# ---------------------------------------------------------------------------

# Cortex-M7 with the double-precision FPU, as fitted on the STM32H755ZI.
set(CPU_FLAGS_CM7
    -mcpu=cortex-m7
    -mfpu=fpv5-d16
    -mfloat-abi=hard
    -mthumb
)

set(COMMON_WARNING_FLAGS
    -Wall
    -Wextra
    -Wshadow
    -Wdouble-promotion
    -Wundef
    -Wno-unused-parameter
    -Werror
)

# -fno-common keeps a stray duplicate definition from being merged silently,
# which matters because the DMA-visible objects are placed by section name.
set(COMMON_C_FLAGS
    -ffunction-sections
    -fdata-sections
    -fno-common
    -fstack-usage
)

set(COMMON_DEFINITIONS
    STM32H755xx
    CORE_CM7
    USE_HAL_DRIVER
)
