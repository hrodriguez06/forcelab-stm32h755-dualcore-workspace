# CMSIS device files of the STM32F334 (subset)

Copied unchanged from `STMicroelectronics/cmsis_device_f3`, tag **v2.3.9**
(commit `402c039a0a6d73c31303a05e6a1d6b8b59474fa1`, "Release v2.3.9"), for the
05_i2c bench firmware of the NUCLEO-F334R8 (`../`). Download approved by the
owner of the catalogue (05_i2c plan, header and decision 8).

| File here | Upstream path |
| --- | --- |
| `Include/stm32f3xx.h` | `Include/stm32f3xx.h` |
| `Include/stm32f334x8.h` | `Include/stm32f334x8.h` |
| `Include/system_stm32f3xx.h` | `Include/system_stm32f3xx.h` |
| `Source/gcc/startup_stm32f334x8.s` | `Source/Templates/gcc/startup_stm32f334x8.s` |
| `LICENSE.md` | `LICENSE.md` (Apache License 2.0) |

SHA-256 of the files as downloaded (CRLF line endings; the repository's
`* text=auto` stores them with LF, so a checkout on Windows gives these bytes
back):

```text
4ef317cdf8f333438adf691925adb41d0edfc9c99e59a2f5f1eca97aa7be4b27  Include/stm32f334x8.h
760840f09f51e19306da5cac985429f2ac36ff0d9174df58c75f2803e18e4426  Include/stm32f3xx.h
29eac64446753fc5d8050ace9470962767ecb2893cfc21fb2bed91f645ee4475  Include/system_stm32f3xx.h
48f717ba588d92d93fe86d2e01ceb5c870de3d7c6ed4c5fd3244e9f590530a21  Source/gcc/startup_stm32f334x8.s
603fce6d2deceec30d8376231317743bf25351db91251661c71c07fd12773be1  LICENSE.md
```

Not copied: `system_stm32f3xx.c` (the firmware has its own `SystemInit`, in
`../src/system_f334.c`) and the Cortex-M4 core headers. `core_cm4.h` and the
`cmsis_*.h` it includes come from the CMSIS of the STM32CubeH7 package the
rest of the workspace builds against (`Drivers/CMSIS/Include`, see
`../CMakeLists.txt`).
