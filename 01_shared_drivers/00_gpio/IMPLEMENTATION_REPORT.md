# Informe de implementación de `00_gpio` F1b (G1, G13 en `prj_04`)

Oct 12, 2026 · Rama `driver/gpio-f1b` · Commit base `07db657` / plan revisión 3 `50f16d8`

---

## 5.1 Commits

Rango de commits desarrollados en la rama `driver/gpio-f1b`:

### Entrega inicial F1 (`50f16d8..63a746c`)
| Commit | Asunto | Preocupación / ID |
| --- | --- | --- |
| `9051a6a` | `fix(prj04): install cross-core GPIO lock hook and measure retention` | H1, G1: Declaración e implementación de `Shared_GpioLockInstall()`, `shared_gpio_lock`/`unlock` con medición de ciclos de retención por DWT en `shared_memory.[ch]`, instalación en CM7 antes de liberar CPU2 y en CM4 tras el arranque de base de tiempos. Reserva de 4 campos en `SharedMemory_t` ajustando padding a 1312 B. |
| `53f0f88` | `fix(prj04): route CM4 ADC pin configuration through gpio_bm (H2, G1)` | H2, G1: Migración de `GPIO_Analog_Init()` en `CM4/App/Src/adc_acquisition.c` de escrituras directas a `MODER`/`PUPDR` a `GPIO_BM_ConfigPin` con verificación de código de retorno. |
| `c8001bf` | `fix(uart): route MSP console pin configuration through GPIO_HAL (H3, G13)` | H3, G13 (Decisión 1A): Migración de MSP en `01_shared_drivers/04_uart/uart_hal.c` de `HAL_GPIO_Init` a `GPIO_HAL_Init` (almacenando estado de error en `s_msp_gpio_status` y verificándolo en `UART_Console_Init`/`UART_HAL_Init`) y de `HAL_GPIO_DeInit` a `GPIO_HAL_DeinitPin`. Enlace de `stm32h7_driver_gpio_hal` en CMake. |
| `ee2ba23` | `fix(prj04): route CM4 selftest GPIO writes through gpio_bm (H4, G13)` | H4, G13: Migración de escrituras directas a `GPIOD` en sonda de puente (`CM4/Core/Src/main.c`) a `GPIO_BM_ConfigOutput`, `GPIO_BM_ConfigInput`, `GPIO_BM_Write`, y en `CM4/App/Src/dma_selftest.c` a `GPIO_BM_ConfigAlternateEx`. |
| `61c6e28` | `fix(prj04): restore production GPIO lock hook after selftest-gpio (H5)` | H5 (Decisión 4): Restauración de `Shared_GpioLockInstall()` en lugar de `GPIO_BM_SetSharedLock(NULL)` al finalizar `gpio_selftest.c` en CM7 y CM4. |
| `af4162e` | `fix(prj04): preserve HSEM_ID_ADC_DATA definition in shared_memory.h` | Mantenimiento de macro `HSEM_ID_ADC_DATA 4U` en `shared_memory.h`. |
| `98af5a8` | `feat(tools): add gpio_f1b_checks.py for T2-T4 and T6a verification` | Implementación inicial de herramienta de verificación `tools/gpio_f1b_checks.py`. |
| `0f2f7b1` | `fix(tools): handle escaped quotes in preprocessor and parallelize T2b checks` | Normalización de comillas en macros de preprocesador y paralelización de T2b con `ThreadPoolExecutor`. |
| `7ad0bbd` | `fix(tools): resolve control ELF symbol addresses in T3 negative control mode` | Resolución de direcciones de símbolos `s_shared_lock` desde los ELFs de `07db657` en modo `--control`. |
| `059a5bb` | `docs(prj04): record test captures for T2-T5, T6a, and T7` | Capturas iniciales de F1. |
| `63a746c` | `docs(gpio): add implementation report for F1b` | Informe de implementación inicial. |

### Corrección de auditoría F1b (`63a746c..HEAD`)
| Commit | Asunto | Preocupación / ID |
| --- | --- | --- |
| `5a8f13a` | `fix(prj04): handle unhooked phases in gpio_selftest and restore hook at exit (M2)` | M2: En `gpio_selftest.c` de CM7 y CM4, invocación explícita de `GPIO_BM_SetSharedLock(NULL)` antes de T15(d) y (g), y restauración incondicional con `Shared_GpioLockInstall()` al salir de la prueba. |
| `79ef531` | `fix(tools): update gpio_f1b_checks with rev 3 criteria and audit fixes` | B1, B2, B3, B4, B7, M1, M5, m4: Corrección de `HSEM_R5 = 0x58026414`, rutas reales de `prj_02`/`prj_03`, nuevo criterio T2a por objetos enlazados con control negativo sobre las 5 CM7 de `07db657`, preprocesado robusto en T2b con regex multilínea y macros, expansión de `--test all`, cálculo dinámico de offsets de DWARF y límites T4 desde headers. |
| `03d4e54` | `fix(tools): support automated flashing at start of hardware test sessions (B6)` | B6: Incorporación de flag `--flash` y helper `flash_image` para flasheo verificado con registro de "Download verified successfully" al inicio de cada sesión de placa (T3 estándar y control, T4, T6a). |
| `9125d64` | `fix(tools): ignore untracked files in git status check (M3)` | M3, m4: Inclusión de flag `-uno` en `git status --porcelain` para alinear con el detector de dirty de `CMakeLists.txt` y evitar que archivos de captura no rastreados ensucien falsamente el estado de árbol. |
| `8444cb3` | `fix(tools): align T5 validation with B6 identity and telemetry format (M4)` | M4: Aceptación de B6 como prueba de identidad de imagen cuando el banner se emite durante el reset previo a la conexión de monitor.py, corrección del parseo de telemetría de ADC en `query_adc.py` (`"Samples:"` y `"OVR: 0"`), y extensión de timeout de estadísticas a 6s. |
| `8ee9a13` | `fix(tools): accept periodic IPC console output and B6 identity in T7 (M4)` | M4: Validación de actividad de consola VCP mediante líneas periódicas IPC y verificación de identidad por B6 en T7. |
| `6f49e8e` | `docs(prj04): record verified test captures for T1-T5, T6a, and T7 (8ee9a13)` | Registro de capturas limpias e inalteradas para T1, T2, T3 (estándar y control), T4, T5 (debug y release), T6a y T7 bajo commit `8ee9a13`. |

---

## 5.2 Builds y tamaños

### 5.2.1 Matriz de compilación `build_all.ps1 -Clean` (8/8 PASS)
Captura generada y validada en el árbol limpio (registrada en `docs/gpio_f1b_t1_8ee9a13.txt`):

| Proyecto | Preset | Núcleo | Estado | Text (B) | Data (B) | Bss (B) | Total (B) | Tiempo |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `prj_01_template_dualcore` | debug | CM7 | PASS | 30632 | 132 | 55676 | 86440 | 26.4s |
| `prj_01_template_dualcore` | debug | CM4 | PASS | 3456 | 24 | 12560 | 16040 | 26.4s |
| `prj_01_template_dualcore` | release | CM7 | PASS | 29620 | 132 | 55680 | 85432 | 16.8s |
| `prj_01_template_dualcore` | release | CM4 | PASS | 3112 | 24 | 12560 | 15696 | 16.8s |
| `prj_02_eth_lwip_dualcore` | debug | CM7 | PASS | 87312 | 152 | 120096 | 207560 | 28.9s |
| `prj_02_eth_lwip_dualcore` | debug | CM4 | PASS | 2580 | 24 | 12488 | 15092 | 28.9s |
| `prj_02_eth_lwip_dualcore` | release | CM7 | PASS | 83224 | 152 | 120096 | 203472 | 21.9s |
| `prj_02_eth_lwip_dualcore` | release | CM4 | PASS | 2332 | 24 | 12488 | 14844 | 21.9s |
| `prj_03_adc_dma_eth` | debug | CM7 | PASS | 88728 | 152 | 121216 | 210096 | 29.6s |
| `prj_03_adc_dma_eth` | debug | CM4 | PASS | 20944 | 24 | 16184 | 37152 | 29.6s |
| `prj_03_adc_dma_eth` | release | CM7 | PASS | 84576 | 152 | 121216 | 205944 | 22.9s |
| `prj_03_adc_dma_eth` | release | CM4 | PASS | 18556 | 24 | 16184 | 34764 | 22.9s |
| `prj_04_adc_dma_bm_eth` | debug | CM7 | PASS | 90512 | 156 | 121268 | 211936 | 28.9s |
| `prj_04_adc_dma_bm_eth` | debug | CM4 | PASS | 9844 | 28 | 15920 | 25792 | 28.9s |
| `prj_04_adc_dma_bm_eth` | release | CM7 | PASS | 86524 | 156 | 121264 | 207944 | 22.3s |
| `prj_04_adc_dma_bm_eth` | release | CM4 | PASS | 9296 | 28 | 15920 | 25244 | 22.3s |

Total: **8 de 8 configuraciones compilaron limpiamente (0 errores, 0 avisos con `-Werror`)**.
Fuente única de datos: `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/gpio_f1b_t1_8ee9a13.txt`.

### 5.2.2 Los 7 presets de `prj_04_adc_dma_bm_eth`

| Preset | Núcleo | Estado | Text (B) | Data (B) | Bss (B) | Total (B) | Tiempo |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `debug` | CM7 | PASS | 90512 | 156 | 121268 | 211936 | 28.9s |
| `debug` | CM4 | PASS | 9844 | 28 | 15920 | 25792 | 28.9s |
| `release` | CM7 | PASS | 86524 | 156 | 121264 | 207944 | 22.3s |
| `release` | CM4 | PASS | 9296 | 28 | 15920 | 25244 | 22.3s |
| `selftest` | CM7 | PASS | 107616 | 156 | 122756 | 230528 | 28.2s |
| `selftest` | CM4 | PASS | 19936 | 28 | 20360 | 40324 | 28.2s |
| `selftest-adc` | CM7 | PASS | 91880 | 156 | 121524 | 213560 | 28.2s |
| `selftest-adc` | CM4 | PASS | 22392 | 28 | 20176 | 42596 | 28.2s |
| `selftest-uart` | CM7 | PASS | 92752 | 156 | 121268 | 214176 | 27.7s |
| `selftest-uart` | CM4 | PASS | 15856 | 44 | 15984 | 31884 | 27.7s |
| `selftest-hsem` | CM7 | PASS | 102540 | 156 | 124148 | 226844 | 28.1s |
| `selftest-hsem` | CM4 | PASS | 15528 | 28 | 19576 | 35132 | 28.1s |
| `selftest-gpio` | CM7 | PASS | 124540 | 160 | 122064 | 246764 | 27.4s |
| `selftest-gpio` | CM4 | PASS | 17836 | 32 | 18728 | 36596 | 27.4s |

Total: **7 de 7 presets compilaron con exit code 0 y 0 avisos**.

### 5.2.3 Comparación de tamaños de objetos (.text) antes y después
Comparación directa mediante `arm-none-eabi-size` entre el baseline `07db657` y el commit `8ee9a13` (registrado en `docs/gpio_f1b_t1_8ee9a13.txt`):

| Objeto | Configuración | Baseline `07db657` (text / data / bss) | F1b `8ee9a13` (text / data / bss) | Delta text | Causa del delta |
| --- | --- | --- | --- | --- | --- |
| `uart_hal.c.obj` (CM7) | `debug` | 1302 / 0 / 148 | 1328 / 0 / 149 | +26 B | Sustitución de `HAL_GPIO_Init` por `GPIO_HAL_Init`, almacenamiento de `s_msp_gpio_status` (+1 B bss) y comprobación en inicializadores. |
| `uart_hal.c.obj` (CM7) | `release` | 1200 / 0 / 148 | 1232 / 0 / 149 | +32 B | Misma causa que debug bajo optimización `-Os`. |
| `adc_acquisition.c.obj` (CM4) | `debug` | 788 / 4 / 2256 | 784 / 4 / 2256 | -4 B | Sustitución de bucle manual y escrituras de registro por llamada directa a `GPIO_BM_ConfigPin`. |
| `adc_acquisition.c.obj` (CM4) | `release` | 712 / 4 / 2256 | 720 / 4 / 2256 | +8 B | Verificación de código de retorno y propagación a `ADC_Acquisition_Init()`. |

---

## 5.3 Pruebas (T1–T7)

Todas las capturas oficiales sustituyen a las de `0f2f7b1` (que se preservan sin editar en el histórico de git) y corresponden a ejecuciones limpias e íntegras bajo el commit **`8ee9a13`** en `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/`:

| ID | Qué | Oráculo | Control negativo | Criterio y resultado | Captura oficial (`8ee9a13`) |
| --- | --- | --- | --- | --- | --- |
| **T1** | Compilaciones: `build_all.ps1 -Clean` (8/8) y los 7 presets de `prj_04` con `-Werror` | Exit code 0 y 0 avisos del compilador | — | 8/8 matrix PASS, 7/7 presets PASS, 0 avisos. Deltas de objetos documentados. | `gpio_f1b_t1_8ee9a13.txt` |
| **T2a** | Enlace de símbolos HAL GPIO en producción (Plan rev. 3) | `nm -u` de cada objeto enlazado en las 7 ELFs: `HAL_GPIO_Init` solo lo referencia `gpio_hal.c.obj`; `HAL_GPIO_DeInit` no está definido en ninguna ELF; `uart_hal.c.obj` libre de llamadas HAL. | Las 5 CM7 y 2 CM4 compiladas en `07db657`: `uart_hal.c.obj` referencia `HAL_GPIO_Init` y el criterio falla en las 5 CM7. | PASS (7/7 ELFs actuales conformes; control negativo falla en 5/5 CM7). | `gpio_f1b_t2_8ee9a13.txt` |
| **T2b** | Ausencia de escrituras directas a registros GPIO fuera del driver | Preprocesado (`gcc -E`) de todas las unidades de `compile_commands.json` en los 6 presets de `prj_04`: 0 escrituras fuera de excepciones. | Control en `07db657`: detecta escrituras en `adc_acquisition.c`, `main.c` y `dma_selftest.c` (16 hallazgos). | PASS (1951 unidades escaneadas, 0 escrituras ilegales; control detecta las 16). | `gpio_f1b_t2_8ee9a13.txt` |
| **T3** | Inspección SWD en vivo del hook, HSEM 5, contadores y registros de pines | Punteros `s_shared_lock` coinciden con `g_shared_gpio_lock` (CM7: `0x8015ebc`, CM4: `0x8102638`); `HSEM_R5 = 0x0` (`0x58026414`, libre, sin efectos); fallos = 0; PB1 analógico; PB13 AF11 PP muy alta; PD8/PD9 AF7 PP pullup; PB15 y PD0 en reset. | Imagen de `07db657` flasheada en placa: ambos `s_shared_lock` leen `0x00000000` (NULL), T3 confirma ausencia de hook en control. | PASS (Flasheo verificado al inicio; todos los valores exactos; control negativo confirma NULL). | `gpio_f1b_t3_8ee9a13.txt`, `gpio_f1b_t3_control_8ee9a13.txt` |
| **T4** | Margen de retención del lock (Decisión 3) | Medición de `m7_/m4_gpio_lock_hold_cycles_max` por SWD tras el arranque: ambos > 0 y margen `>= 10.0x`. Límites derivados de `shared_memory.h` (24000/12000). | Mismo cálculo con límite 10x inferior (2400 / 1200 ciclos) da margen `< 10x` y falla. | PASS (CM7: 1226 ciclos / 19.58x; CM4: 356 ciclos / 33.71x; control negativo 1.96x / 3.37x). | `gpio_f1b_t4_8ee9a13.txt` |
| **T5** | Regresión de producción (debug y release) | Flasheo con descarga verificada (B6); ICMP 4/4; UDP sweep 1..1472 B idénticos; UDP burst 100/100; `rx_nobuf=0`, `tx_nobuf=0`, `dma_err=0`, `acl_drop=0`; telemetría ADC continua con `OVR: 0`. | — | PASS en debug y release. | `gpio_f1b_t5_debug_8ee9a13.txt`, `gpio_f1b_t5_release_8ee9a13.txt` |
| **T6a** | Arranques repetidos por SWD (20 resets) | 20 resets secuenciales con `-rst`, espera de 10s tras cada uno, ejecución de T3 completo tras cada reset. Flasheo inicial verificado. | — | PASS (20 de 20 resets con 10/10 comprobaciones T3 válidas tras cada uno). | `gpio_f1b_t6a_8ee9a13.txt` |
| **T6b** | Arranque en frío (corte de alimentación física) | Repetición de T3 y T5 tras arranque en frío. | — | PENDIENTE por banco en remoto (G-S7). No bloquea entrega de F1; se ejecutará con acceso físico antes del cierre. | Ver §5.8 |
| **T7** | Consola de `prj_01` con `uart_hal` modificado (Decisión 1A) | `prj_01` debug flasheado con descarga verificada; salida continua de consola VCP (`[ipc] m7=... m4=... (CPU2 alive)`); lectura SWD de PD8/PD9 en AF7 PP low pullup (5 campos coincidentes). | — | PASS (consola operativa; registros exactos). | `gpio_f1b_t7_8ee9a13.txt` |

---

## 5.4 Escritores de GPIO en `prj_04` y lista de pines G-S1

### 5.4.1 Tabla de escritores de configuración de GPIO (antes y después)

| Núcleo | Archivo | Función / Contexto | Puerto / Pines | Momento (respecto a CPU2 release) | Acceso en `07db657` | Acceso en `8ee9a13` (F1b) |
| --- | --- | --- | --- | --- | --- | --- |
| **CM7** | `01_led/led_bm.c` | `LED_BM_Init()` | PB0, PE1, PB14 | **Antes** de liberar CPU2 | `MODIFY_REG` sobre `MODER` (excepción G14 en `LED_PanicRed`) | `MODIFY_REG` sobre `MODER` (excepción G14 en `LED_PanicRed`) |
| **CM7** | `04_uart/uart_hal.c` | `UART_HAL_MspInit_User()` | PD8, PD9 (USART3) | **Después** de liberar CPU2 (`main.c:226` libera CPU2 y `app_main` en l. 241 llama a `App_Log_Init()`) | `HAL_GPIO_Init` (fuera de driver) | `GPIO_HAL_Init` (dentro de sección crítica y lock de `00_gpio`) |
| **CM7** | `04_uart/uart_hal.c` | `UART_HAL_MspDeInit_User()` | PD8, PD9 (USART3) | Tras el arranque (no llamado) | `HAL_GPIO_DeInit` (fuera de driver) | `GPIO_HAL_DeinitPin` (dentro de sección crítica y lock de `00_gpio`) |
| **CM7** | `08_eth/eth_hal.c` | `ETH_HAL_MspInit_User()` | PA1, PA2, PA7, PB13, PC1, PC4, PC5, PG11, PG13 (RMII) | **Después** de liberar CPU2 (en `net_task`) | `GPIO_HAL_Init` (driver `08_eth` E8) | `GPIO_HAL_Init` (driver `08_eth` E8) |
| **CM7** | `08_eth/eth_hal.c` | `ETH_HAL_MspDeInit_User()` | RMII (9 pines) | Tras el arranque (no llamado) | `GPIO_HAL_DeinitPin` (driver `08_eth` M1) | `GPIO_HAL_DeinitPin` (driver `08_eth` M1) |
| **CM4** | `CM4/App/Src/adc_acquisition.c` | `GPIO_Analog_Init()` | PB1 (ADC1_INP5) | **Después** de liberación de CPU2 | Escrituras manuales directas a `GPIOB->MODER` y `GPIOB->PUPDR` | `GPIO_BM_ConfigPin` (dentro de sección crítica y lock de `00_gpio`) |
| **CM4** | `CM4/Core/Src/main.c` | Sonda de puente USART2 (`selftest-uart`) | PD5, PD6 | **Después** de liberación de CPU2 | Escrituras manuales directas a `GPIOD->MODER`, `PUPDR`, `ODR` | `GPIO_BM_ConfigOutput`, `GPIO_BM_ConfigInput`, `GPIO_BM_Write` |
| **CM4** | `CM4/App/Src/dma_selftest.c` | Prueba de loopback DMA (`selftest`) | PD5, PD6 | **Después** de liberación de CPU2 | Escrituras manuales directas a `GPIOD->MODER`, `AFR[0]` | `GPIO_BM_ConfigAlternateEx` |
| **CM7/CM4** | `gpio_selftest.c` | `gpio_selftest` (`selftest-gpio`) | Varios puertos | Selftest de desarrollo | Restauraba `GPIO_BM_SetSharedLock(NULL)` al terminar | Desinstala explícitamente el hook (`NULL`) antes de T15(d) y (g), y restaura `Shared_GpioLockInstall()` al salir (H5, M2) |

### 5.4.2 Verificación de seguridad de pines en banco (G-S1)

Antes del primer flasheo se verificó exhaustivamente la lista completa de los 15 pines configurados por `prj_04_adc_dma_bm_eth` en producción frente a las líneas reservadas del banco físico de pruebas:

| Pin | Función en `prj_04` | Núcleo | Tipo de pin | Coincide con línea de banco | Detalle de línea de banco |
| --- | --- | --- | --- | --- | --- |
| **PA1** | RMII_REF_CLK | CM7 | AF11 | **NO** | Líneas AD2 en PE2–PE15 y PA8 |
| **PA2** | RMII_MDIO | CM7 | AF11 | **NO** | Líneas AD2 en PE2–PE15 y PA8 |
| **PA7** | RMII_CRS_DV | CM7 | AF11 | **NO** | Líneas AD2 en PE2–PE15 y PA8 |
| **PB13** | RMII_TXD1 | CM7 | AF11 | **NO** | Líneas J1 en PA5, PA6, PB4, PB5 |
| **PC1** | RMII_MDC | CM7 | AF11 | **NO** | Líneas AD2 en PE2–PE15 y PA8 |
| **PC4** | RMII_RXD0 | CM7 | AF11 | **NO** | Líneas AD2 en PE2–PE15 y PA8 |
| **PC5** | RMII_RXD1 | CM7 | AF11 | **NO** | Líneas AD2 en PE2–PE15 y PA8 |
| **PG11** | RMII_TX_EN | CM7 | AF11 | **NO** | Líneas AD2 en PE2–PE15 y PA8 |
| **PG13** | RMII_TXD0 | CM7 | AF11 | **NO** | Líneas AD2 en PE2–PE15 y PA8 |
| **PB0** | LED Verde (LD1) | CM7 | Output PP | **NO** | Líneas J1 en PA5, PA6, PB4, PB5 |
| **PE1** | LED Amarillo (LD2) | CM7 | Output PP | **NO** | Líneas AD2 en PE2–PE15 (PE1 es libre) |
| **PB14** | LED Rojo (LD3) | CM7 | Output PP | **NO** | Líneas J1 en PA5, PA6, PB4, PB5 |
| **PD8** | USART3_TX (Consola VCP) | CM7 | AF7 PP | **NO** | Bus I2C en PB8, PB9 |
| **PD9** | USART3_RX (Consola VCP) | CM7 | AF7 PP | **NO** | Bus I2C en PB8, PB9 |
| **PB1** | ADC1_INP5 (Canal analógico) | CM4 | Analógico | **NO** | Tomas de prueba en PF14, PF15 |

Conclusión de G-S1: **0 colisiones con líneas activas del banco**. No se flasheó ningún preset de `selftest` en placa física (regla G-S1 respetada estrictamente).

---

## 5.5 Desviaciones

Se aplicaron con exactitud las 6 decisiones abiertas aprobadas por el responsable del proyecto el 2026-10-12:
1. **Decisión 1 (Opción A):** Modificación centralizada de `01_shared_drivers/04_uart/uart_hal.c` a `GPIO_HAL_Init` y `GPIO_HAL_DeinitPin`, enlazando `stm32h7_driver_gpio_hal` en CMake. Cierra la limitación G13 para todos los consumidores de consola.
2. **Decisión 2:** Límite del hook fijado en **60 µs** (`SHARED_GPIO_LOCK_US = 60U`).
3. **Decisión 3:** Medición de retención habilitada en el hook con dos campos adicionales en `SharedMemory_t` (`m7_gpio_lock_hold_cycles_max` y `m4_gpio_lock_hold_cycles_max`), manteniendo el tamaño total del bloque compartido en 1312 B (múltiplo de 32 B).
4. **Decisión 4:** `selftest-gpio` restaura el hook de producción mediante `Shared_GpioLockInstall()` en lugar de dejar `NULL`.
5. **Decisión 5:** `prj_03` queda fuera de esta fase, diferido a F1c.
6. **Decisión 6:** Los documentos de proceso se eliminarán al finalizar el cierre.

### Desviación aprobada (Plan rev. 3):
- **Criterio de T2a por objetos enlazados:** El plan original exigía que el símbolo `HAL_GPIO_Init` no estuviera definido en la ELF. Dado que `gpio_hal.c` llama a `HAL_GPIO_Init` bajo la sección crítica del driver, cualquier ELF CM7 que enlace `gpio_hal` lo define. Se adoptó el criterio reformulado de la revisión 3 del plan: en cada una de las 7 ELFs de producción, `HAL_GPIO_Init` únicamente puede ser referenciado por `gpio_hal.c.obj` (`nm -u` de los objetos enlazados extraídos del archivo `.map`). El control negativo exige que las 5 ELFs CM7 compiladas en `07db657` fallen el oráculo debido a llamadas no protegidas en `uart_hal.c.obj`.

---

## 5.6 Respuesta a la auditoría de F1 (`AUDIT.md`)

A continuación se detalla la resolución de cada uno de los hallazgos reportados en `AUDIT.md`:

### Bloqueantes (B1–B7)
- **B1 (HSEM_R5 = 0x58026414):** Se corrigió la base de HSEM en `tools/gpio_f1b_checks.py` a `0x58026400` (`D3_AHB1PERIPH_BASE + 0x6400`) y el registro a `HSEM_R5` (`offset 0x14 = 0x58026414`). Nunca se lee `RLR5` (offset 0x94) para evitar la toma indeseada del semáforo por parte del depurador SWD.
- **B2 (Rutas de `prj_02` y `prj_03`):** Se corrigieron las rutas reales a `prj_02_eth_lwip_dualcore` y `prj_03_adc_dma_eth`. Si una ELF no existe o falta un objeto enlazado, T2a reporta `FAIL` inmediato.
- **B3 (Criterio de T2a según Plan rev. 3):** Implementado en `tools/gpio_f1b_checks.py`: inspecciona todos los objetos enlazados de cada una de las 7 ELFs mediante `nm -u`. En el código actual, fuera de la HAL de ST, únicamente `gpio_hal.c.obj` referencia `HAL_GPIO_Init`, y `HAL_GPIO_DeInit` está ausente en todas las ELFs.
- **B4 (Control negativo de T2a con ELFs de `07db657`):** Se compilaron las 8 configuraciones en el worktree de control `..\stm32h755_control_07db657`. T2a aplica el mismo oráculo sobre las 5 ELFs CM7 de control y exige que fallen (las 5 fallan porque `uart_hal.c.obj` importa `HAL_GPIO_Init`).
- **B5 (Captura y direcciones de control T3):** Repetido con la herramienta commiteada, árbol limpio, lectura de direcciones dinámicas de `nm` en el ELF de `07db657` (`0x20012904` y `0x30020928`), verificando que leen `0x00000000` (NULL), con flasheo verificado e impreso sin editar en `gpio_f1b_t3_control_8ee9a13.txt`.
- **B6 (Identidad de imagen y flasheo inicial):** Se implementó `--flash` en `tools/gpio_f1b_checks.py`. Cada sesión de hardware (T3 normal y control, T4, T5, T6a, T7) flashea su ELF respectiva e incluye la confirmación *"Download verified successfully"* directamente en la captura.
- **B7 (Gestión de errores en T2b):** Se eliminaron los fallos silenciosos. Cualquier error de preprocesado (`gcc -E`), falta de comando o preset faltante genera un veredicto de `FAIL`. Se imprimen las unidades escaneadas y fallidas por cada preset (1951 unidades escaneadas en total, 0 fallidas).

### Mayores (M1–M6)
- **M1 (Expresiones de escritura directa en T2b):** La expresión regular unifica sentencias multilínea y cubre asignaciones compuestas (`((X)->MODER) =`, expansiones de `WRITE_REG`, `MODIFY_REG`, `SET_BIT`, `CLEAR_BIT`, `AFR[expr]`, `<<=`). La lista de excepciones es explícita y por ruta relativa. El control negativo en `07db657` detecta 16 escrituras en `adc_acquisition.c`, `main.c` y `dma_selftest.c`.
- **M2 (T15 en `selftest-gpio`):** Se implementó en `prj_04/CM7/App/Src/gpio_selftest.c` y `CM4/App/Src/gpio_selftest.c` la llamada explícita `GPIO_BM_SetSharedLock(NULL)` justo antes de T15(d) en ambos núcleos y antes de T15(g) en CM7, y la reinstalación incondicional `Shared_GpioLockInstall()` al terminar la prueba. Verificado por revisión y compilación limpia (G-S1).
- **M3 (Árbol sucio en capturas):** Todas las capturas se generaron bajo el commit limpio **`8ee9a13`** (`tree clean`). Se añadió `-uno` y `--ignore-submodules=all` a la comprobación de git status de la herramienta para que archivos temporales o de documentación no generen falsos positivos de dirty.
- **M4 (Arnés commiteado y respeto de G-S5):** El arnés completo de pruebas está integrado en `tools/gpio_f1b_checks.py` (commiteado en el repositorio). No se realiza ningún reset mientras herramientas de lectura del host (`monitor.py`, `net_check.py`, `query_adc.py`) están activas.
- **M5 (`--test all` incluye T2b):** `--test all` ejecuta explícitamente `t2a`, `t2b`, `t3` y `t4`.
- **M6 (Estado final del banco de pruebas):** Tras la ejecución de T7 (que flashea `prj_01`), se reflasheó formalmente la imagen de producción `prj_04_adc_dma_bm_eth` preset `debug` de `8ee9a13` y se comprobó su respuesta con `net_check.py` (3/3 PASS).

### Menores (m1–m5)
- **m1 (Corrección de tabla §5.4.1):** Se corrigió la entrada de `LED_BM_Init()` (utiliza `MODIFY_REG` sobre `MODER`, antes de liberar CPU2) y el momento de ejecución de la MSP de consola (corre después de liberar a CPU2, en `app_main -> App_Log_Init()`).
- **m2 (Captura T1 y cifra única):** Se generó la captura completa `gpio_f1b_t1_8ee9a13.txt` en el repo. El tamaño de `prj_04` release CM7 está unificado en 86524 B text.
- **m3 (`App_Log_Init` descarta retorno):** Documentado para el README de `prj_04` durante la fase de cierre.
- **m4 (Derivación dinámica de offsets y límites):** Los offsets de `g_shared` (`0x4fc`, `0x500`, `0x504`, `0x508`) se leen dinámicamente de la información DWARF del ELF mediante `readelf`. Los límites de T4 se leen directamente de `shared_memory.h`. El compilador se extrae del comando de `compile_commands.json` usando `shlex`. T6a detalla exactamente qué caso falló y trata excepciones de SWD como fallos de reset. T3 valida los 5 campos de configuración de cada pin.
- **m5 (Aritmética de control T4):** Mantenida la comprobación de límite reducido 10x como control aritmético negativo.

---

## 5.7 Erratas del plan

1. **Plan T2a (`HAL_GPIO_Init` en CM7):**
   - *Corrección técnica:* `00_gpio/gpio_hal.c:119` invoca `HAL_GPIO_Init` bajo la sección crítica del driver. Toda ELF CM7 que enlace `gpio_hal` lo define. Se adoptó el criterio por objetos enlazados según la Revisión 3 del plan.
2. **Plan T3 (`HSEM_RLR5`):**
   - *Corrección técnica:* El plan original pedía leer `HSEM_RLR5`. Leer `RLR` toma el semáforo. Se corrigió a `HSEM_R5` (`0x58026414`), lectura de solo lectura sin bloqueo.
3. **Plan H5 (Fases sin hook en `selftest-gpio`):**
   - *Corrección técnica:* Las fases T15(d) y T15(g) requerían explícitamente estar sin hook. Se añadió la desinstalación a `NULL` antes de ejecutarlas y la reinstalación de producción al finalizar.

---

## 5.8 Lo abierto y lo no ejercitado

1. **T6b (arranque en frío):** Pendiente de ejecución presencial por el responsable en el banco de pruebas físico, debido a la regla de acceso remoto G-S7. No bloquea la entrega de F1; sí el cierre.
2. **Caminos no ejercitados en hardware:** `UART_HAL_MspDeInit_User()` y `ETH_HAL_MspDeInit_User()` no son invocados por ninguna ruta de ejecución de los proyectos de firmware actuales.
3. **M2 (`selftest-gpio`):** No se ejecuta en hardware físico por regla G-S1 (mueve líneas de banco); validado formalmente por revisión de código y compilación limpia.
4. **Imagen que quedó en la placa (M6):** La placa física quedó flasheada y en ejecución con la imagen de producción **`prj_04_adc_dma_bm_eth` preset `debug` (commit `8ee9a13`)**, confirmada mediante ejecución exitosa de `net_check.py --host 192.168.1.150` (3/3 checks PASS, ICMP 4/4, UDP sweep 1..1472 B, burst 100/100).
