# ---------------------------------------------------------------------------
# i2c_target_f334 - bench firmware of the NUCLEO-F334R8 used as the I2C target
# of the 05_i2c bench (01_shared_drivers/05_i2c/IMPLEMENTATION_PLAN.md, D1, D2).
#
# An instrument, not a catalogue project: bare metal on the CMSIS device files
# of cmsis/ (cmsis_device_f3 v2.3.9), no HAL. Build with build.ps1 (or
# `cmake --preset f334` and `cmake --build --preset f334` after tools/env.ps1);
# flash with tools/flash.py --sn <F334 ST-LINK SN> --elf <this ELF> (README.md).
# ---------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.22)

project(i2c_target_f334 C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(WORKSPACE_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)

# core_cm4.h and the cmsis_*.h it includes: from the CMSIS of the Cube package
# the rest of the workspace builds against (the same default as prj_01).
set(CUBE_DIR "${WORKSPACE_DIR}/02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7"
    CACHE PATH "STM32CubeH7 checkout whose Drivers/CMSIS/Include provides core_cm4.h")
set(CMSIS_CORE_DIR ${CUBE_DIR}/Drivers/CMSIS/Include)
if(NOT EXISTS ${CMSIS_CORE_DIR}/core_cm4.h)
    message(FATAL_ERROR
        "core_cm4.h not found under ${CMSIS_CORE_DIR}.\n"
        "Restore the Cube submodule from the workspace root with .\\tools\\init_cube_submodule.ps1, "
        "or pass -DCUBE_DIR=<STM32CubeH7 checkout>.")
endif()

# Rev and build id by the same method as prj_01 (its cmake/fw_build_id.cmake,
# run at BUILD time): FW_GIT_REV = git rev-parse --short HEAD (+ -dirty),
# FW_BUILD_ID = first 16 hex digits of SHA-256("HEAD=" <full hash> <git diff
# HEAD> "CMAKE_C_FLAGS=" <cache> " PRESET=" <FW_PRESET>). The 05_i2c host
# recomputes it over build/f334/CMakeCache.txt (gpio_hw_check.host_build_id).
set(FW_PRESET "" CACHE STRING "Configure preset name, hashed into FW_BUILD_ID (set by CMakePresets.json)")
set(FW_BUILD_ID_SCRIPT ${WORKSPACE_DIR}/03_firmware_templates/prj_01_template_dualcore/cmake/fw_build_id.cmake)
add_custom_target(fw_build_id ALL
    COMMAND ${CMAKE_COMMAND} -DSRC_DIR=${CMAKE_CURRENT_LIST_DIR} -DBIN_DIR=${CMAKE_BINARY_DIR}
            -P ${FW_BUILD_ID_SCRIPT}
    BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/fw_build_id.h
    COMMENT "fw_build_id.h (FW_GIT_REV, FW_BUILD_ID)"
    VERBATIM
)

set(TARGET i2c_target_f334)
set(LINKER_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/linker/stm32f334r8_flash.ld)

add_executable(${TARGET}
    src/main.c
    src/serial.c
    src/target.c
    src/system_f334.c
    cmsis/Source/gcc/startup_stm32f334x8.s
)
add_dependencies(${TARGET} fw_build_id)

target_compile_definitions(${TARGET} PRIVATE STM32F334x8)
target_include_directories(${TARGET} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/cmsis/Include
    ${CMSIS_CORE_DIR}
    ${CMAKE_BINARY_DIR}/generated
)

# Soft-float ABI: the firmware has no floating point, so no FPU context is ever
# stacked by an interrupt.
set(CPU_FLAGS -mcpu=cortex-m4 -mthumb -mfloat-abi=soft)
target_compile_options(${TARGET} PRIVATE
    ${CPU_FLAGS}
    -O2 -g3
    -ffunction-sections -fdata-sections -fno-common -fstack-usage
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Wshadow -Wdouble-promotion -Wundef -Wconversion -Wpedantic>
    $<$<COMPILE_LANGUAGE:ASM>:-x assembler-with-cpp>
)
target_link_options(${TARGET} PRIVATE
    ${CPU_FLAGS}
    -T${LINKER_SCRIPT}
    --specs=nano.specs
    -Wl,--gc-sections
    -Wl,-Map=${TARGET}.map,--cref
    -Wl,--print-memory-usage
    -Wl,--no-warn-rwx-segments
)

set_target_properties(${TARGET} PROPERTIES
    OUTPUT_NAME ${TARGET}
    SUFFIX ".elf"
    LINK_DEPENDS ${LINKER_SCRIPT}
)

add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET}> ${TARGET}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${TARGET}>
    COMMENT "Generating ${TARGET}.bin and reporting size"
)
