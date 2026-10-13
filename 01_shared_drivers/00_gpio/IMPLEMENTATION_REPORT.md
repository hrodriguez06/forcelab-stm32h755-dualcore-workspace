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

### Primera corrección de auditoría (`63a746c..32f343a`)
| Commit | Asunto | Preocupación / ID |
| --- | --- | --- |
| `5a8f13a` | `fix(prj04): handle unhooked phases in gpio_selftest and restore hook at exit (M2)` | M2: En `gpio_selftest.c` de CM7 y CM4, invocación explícita de `GPIO_BM_SetSharedLock(NULL)` antes de T15(d) y (g), y restauración incondicional con `Shared_GpioLockInstall()` al salir de la prueba. |
| `79ef531` | `fix(tools): update gpio_f1b_checks with rev 3 criteria and audit fixes` | B1, B2, B3, B4, B7, M1, M5, m4: Corrección de `HSEM_R5 = 0x58026414`, rutas reales de `prj_02`/`prj_03`, nuevo criterio T2a por objetos enlazados con control negativo sobre las 5 CM7 de `07db657`, preprocesado robusto en T2b con regex multilínea y macros, expansión de `--test all`, cálculo dinámico de offsets de DWARF y límites T4 desde headers. |
| `03d4e54` | `fix(tools): support automated flashing at start of hardware test sessions (B6)` | B6: Incorporación de flag `--flash` y helper `flash_image` para flasheo verificado con registro de "Download verified successfully" al inicio de cada sesión de placa (T3 estándar y control, T4, T6a). |
| `9125d64` | `fix(tools): ignore untracked files in git status check (M3)` | M3, m4: Inclusión de flag `-uno` en `git status --porcelain` para alinear con el detector de dirty de `CMakeLists.txt` y evitar que archivos de captura no rastreados ensucien falsamente el estado de árbol. |
| `8444cb3` | `fix(tools): align T5 validation with B6 identity and telemetry format (M4)` | M4: Aceptación de B6 como prueba de identidad de imagen cuando el banner se emite durante el reset previo a la conexión de monitor.py, corrección del parseo de telemetría de ADC en `query_adc.py` (`"Samples:"` y `"OVR: 0"`), y extensión de timeout de estadísticas a 6s. |
| `8ee9a13` | `fix(tools): accept periodic IPC console output and B6 identity in T7 (M4)` | M4: Validación de actividad de consola VCP mediante líneas periódicas IPC y verificación de identidad por B6 en T7. |
| `6f49e8e` | `docs(prj04): record verified test captures for T1-T5, T6a, and T7 (8ee9a13)` | Registro de capturas de T1, T2, T3, T4, T5, T6a y T7 bajo commit `8ee9a13`. |
| `32f343a` | `docs(gpio): update implementation report answering audit findings` | Actualización de informe de implementación para re-auditoría. |

### Segunda corrección (re-auditoría `32f343a..HEAD`)
| Commit | Asunto | Preocupación / ID |
| --- | --- | --- |
| `7cc456d` | `fix(tools): address re-audit findings M1, B7, N1-N6, m4 in gpio_f1b_checks` | M1, B7, N1-N6, m4: Regex unificada `RAW_WRITE_RE` con detección de macros y bases expandidas del CMSIS, autoprueba con headers CMSIS reales (12/12 escrituras, 0/9 lecturas), escaneo estricto de control por `(preset, ruta_relativa)` sobre los 6 presets (1951 unidades escaneadas, 20 hallazgos), corrección `pd8_ok` en T3, crecimiento de `Samples` y `OVR == 0` en T5, validación estricta de contadores de error en todas las líneas `[net]`, reporte verídico en T7, espera de 3.0s tras flasheo en T3, `--control` implica `--flash`, y offsets de DWARF estrictos sin respaldos silenciosos. |

---

## 5.2 Builds y tamaños

### 5.2.1 Matriz de compilación `build_all.ps1 -Clean` (8/8 PASS)
Captura generada y validada en el árbol limpio (registrada en `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/gpio_f1b_t1_7cc456d.txt`):

| Proyecto | Preset | Núcleo | Estado | Text (B) | Data (B) | Bss (B) | Total (B) | Tiempo |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `prj_01_template_dualcore` | debug | CM7 | PASS | 30632 | 132 | 55676 | 86440 | 26.4s |
| `prj_01_template_dualcore` | debug | CM4 | PASS | 3456 | 24 | 12560 | 16040 | 26.4s |
| `prj_01_template_dualcore` | release | CM7 | PASS | 29620 | 132 | 55680 | 85432 | 19.2s |
| `prj_01_template_dualcore` | release | CM4 | PASS | 3112 | 24 | 12560 | 15696 | 19.2s |
| `prj_02_eth_lwip_dualcore` | debug | CM7 | PASS | 87312 | 152 | 120096 | 207560 | 33.9s |
| `prj_02_eth_lwip_dualcore` | debug | CM4 | PASS | 2580 | 24 | 12488 | 15092 | 33.9s |
| `prj_02_eth_lwip_dualcore` | release | CM7 | PASS | 83224 | 152 | 120096 | 203472 | 22.7s |
| `prj_02_eth_lwip_dualcore` | release | CM4 | PASS | 2332 | 24 | 12488 | 14844 | 22.7s |
| `prj_03_adc_dma_eth` | debug | CM7 | PASS | 88728 | 152 | 121216 | 210096 | 33.9s |
| `prj_03_adc_dma_eth` | debug | CM4 | PASS | 20944 | 24 | 16184 | 37152 | 33.9s |
| `prj_03_adc_dma_eth` | release | CM7 | PASS | 84576 | 152 | 121216 | 205944 | 25.3s |
| `prj_03_adc_dma_eth` | release | CM4 | PASS | 18556 | 24 | 16184 | 34764 | 25.3s |
| `prj_04_adc_dma_bm_eth` | debug | CM7 | PASS | 90512 | 156 | 121268 | 211936 | 31.8s |
| `prj_04_adc_dma_bm_eth` | debug | CM4 | PASS | 9844 | 28 | 15920 | 25792 | 31.8s |
| `prj_04_adc_dma_bm_eth` | release | CM7 | PASS | 86524 | 156 | 121264 | 207944 | 25.3s |
| `prj_04_adc_dma_bm_eth` | release | CM4 | PASS | 9296 | 28 | 15920 | 25244 | 25.3s |

Total: **8 de 8 configuraciones compilaron limpiamente (0 errores, 0 avisos con `-Werror`)**.
Fuente única de datos: `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/gpio_f1b_t1_7cc456d.txt`.

### 5.2.2 Los 7 presets de `prj_04_adc_dma_bm_eth`

| Preset | Núcleo | Estado | Text (B) | Data (B) | Bss (B) | Total (B) | Tiempo |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `debug` | CM7 | PASS | 90512 | 156 | 121268 | 211936 | 31.8s |
| `debug` | CM4 | PASS | 9844 | 28 | 15920 | 25792 | 31.8s |
| `release` | CM7 | PASS | 86524 | 156 | 121264 | 207944 | 25.3s |
| `release` | CM4 | PASS | 9296 | 28 | 15920 | 25244 | 25.3s |
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
Comparación directa mediante `arm-none-eabi-size` entre el baseline `07db657` y el commit `7cc456d` (registrado en `docs/gpio_f1b_t1_7cc456d.txt`):

| Objeto | Configuración | Baseline `07db657` (text / data / bss) | F1b `7cc456d` (text / data / bss) | Delta text | Causa del delta |
| --- | --- | --- | --- | --- | --- |
| `uart_hal.c.obj` (CM7) | `debug` | 1302 / 0 / 148 | 1328 / 0 / 149 | +26 B | Sustitución de `HAL_GPIO_Init` por `GPIO_HAL_Init`, almacenamiento de `s_msp_gpio_status` (+1 B bss) y comprobación en inicializadores. |
| `uart_hal.c.obj` (CM7) | `release` | 1200 / 0 / 148 | 1232 / 0 / 149 | +32 B | Misma causa que debug bajo optimización `-Os`. |
| `adc_acquisition.c.obj` (CM4) | `debug` | 788 / 4 / 2256 | 784 / 4 / 2256 | -4 B | Sustitución de bucle manual y escrituras de registro por llamada directa a `GPIO_BM_ConfigPin`. |
| `adc_acquisition.c.obj` (CM4) | `release` | 712 / 4 / 2256 | 720 / 4 / 2256 | +8 B | Verificación de código de retorno y propagación a `ADC_Acquisition_Init()`. |

---

## 5.3 Pruebas (T1–T7)

Todas las capturas oficiales corresponden a ejecuciones limpias e íntegras en `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/`:

| ID | Qué | Oráculo | Control negativo | Criterio y resultado | Captura oficial |
| --- | --- | --- | --- | --- | --- |
| **T1** | Compilaciones: `build_all.ps1 -Clean` (8/8), 7 presets de `prj_04` y 5 compilaciones estrictas con `-Werror` | Exit code 0 y 0 avisos del compilador | — | 8/8 matrix PASS, 7/7 presets PASS, 5/5 `-Werror` PASS con 0 avisos. Deltas de objetos documentados. | `gpio_f1b_t1_7cc456d.txt` |
| **T2a** | Enlace de símbolos HAL GPIO en producción (Plan rev. 3) | `nm -u` de cada objeto enlazado en las 7 ELFs: `HAL_GPIO_Init` solo lo referencia `gpio_hal.c.obj`; `HAL_GPIO_DeInit` no está definido en ninguna ELF; `uart_hal.c.obj` libre de llamadas HAL. | Las 5 CM7 y 2 CM4 compiladas en `07db657`: `uart_hal.c.obj` referencia `HAL_GPIO_Init` y el criterio falla en las 5 CM7. | PASS (7/7 ELFs actuales conformes; control negativo falla en 5/5 CM7). | `gpio_f1b_t2_7cc456d.txt` |
| **T2b** | Ausencia de escrituras directas a registros GPIO fuera del driver | Preprocesado (`gcc -E`) de todas las unidades de `compile_commands.json` en los 6 presets de `prj_04`: 0 escrituras fuera de excepciones. Autoprueba CMSIS verifica detección de macros y rechazo de lecturas/comparaciones. | Control en `07db657` escaneado en los 6 presets: detecta por `(preset, ruta_relativa)` 20 escrituras en `adc_acquisition.c`, `main.c` y `dma_selftest.c`. | PASS (Autoprueba 12/12 escrituras detectadas, 0/9 lecturas falsas; 1951 unidades actuales limpias; control detecta los 20 hallazgos en los 6 presets). | `gpio_f1b_t2_7cc456d.txt` |
| **T3** | Inspección SWD en vivo del hook, HSEM 5, contadores y registros de pines | Punteros `s_shared_lock` coinciden con `g_shared_gpio_lock` (CM7: `0x8015ebc`, CM4: `0x8102638`); `HSEM_R5 = 0x0` (`0x58026414`, libre, sin efectos); fallos = 0; PB1 analógico; PB13 AF11 PP muy alta; PD8/PD9 AF7 PP pullup; PB15 y PD0 en reset. Espera de 3.0s tras flasheo. | Imagen de `07db657` flasheada en placa: ambos `s_shared_lock` leen `0x00000000` (NULL), T3 confirma ausencia de hook en control. | PASS (Flasheo verificado al inicio; todos los valores exactos con comprobación `pd8_ok`; control negativo confirma NULL). | `gpio_f1b_t3_7cc456d.txt`, `gpio_f1b_t3_control_7cc456d.txt` |
| **T4** | Margen de retención del lock (Decisión 3) | Medición de `m7_/m4_gpio_lock_hold_cycles_max` por SWD tras el arranque: ambos > 0 y margen `>= 10.0x`. Límites derivados de `shared_memory.h` (24000/12000). Offsets derivados de DWARF sin respaldos silenciosos. | Mismo cálculo con límite 10x inferior (2400 / 1200 ciclos) da margen `< 10x` y falla. | PASS (CM7: 1228 ciclos / 19.54x; CM4: 356 ciclos / 33.71x; control negativo 1.95x / 3.37x). | `gpio_f1b_t4_7cc456d.txt` |
| **T5** | Regresión de producción (debug y release) | Flasheo con descarga verificada (B6); ICMP 4/4; UDP sweep 1..1472 B idénticos; UDP burst 100/100; todas las líneas `[net]` con `rx_nobuf=0`, `tx_nobuf=0`, `dma_err=0`, `acl_drop=0`; telemetría ADC continua con `Samples` creciente y `OVR == 0`. | — | PASS en debug y release. | `gpio_f1b_t5_debug_7cc456d.txt`, `gpio_f1b_t5_release_7cc456d.txt` |
| **T6a** | Arranques repetidos por SWD (20 resets) | 20 resets secuenciales con `-rst`, espera de 10s tras cada uno, ejecución de T3 completo tras cada reset. Flasheo inicial verificado. | — | PASS (20 de 20 resets con 10/10 comprobaciones T3 válidas tras cada uno). Conservada de `8ee9a13` según dictamen de auditoría en AUDIT.md. | `gpio_f1b_t6a_8ee9a13.txt` |
| **T6b** | Arranque en frío (corte de alimentación física) | Repetición de T3 y T5 tras arranque en frío. | — | PENDIENTE por banco en remoto (G-S7). No bloquea entrega de F1; sí el cierre. Lo ejecutará el auditor/responsable con acceso físico. | Ver §5.8 |
| **T7** | Consola de `prj_01` con `uart_hal` modificado (Decisión 1A) | `prj_01` debug flasheado con descarga verificada; consola VCP activa (`[ipc]` periódicos con CPU2 alive); identidad confirmada por B6; lectura SWD de PD8/PD9 en AF7 PP low pullup (5 campos coincidentes). | — | PASS (consola activa; identidad verificada; registros exactos). | `gpio_f1b_t7_7cc456d.txt` |
| **M6** | Evidencia de reflasheo final del banco | Flasheo verificado de `prj_04_adc_dma_bm_eth` debug (`7cc456d`) tras T7 y verificación de red con `net_check.py`. | — | PASS (3/3 checks PASS, enlace asentado). | `gpio_f1b_m6_netcheck_7cc456d.txt` |

---

## 5.4 Escritores de GPIO en `prj_04` y lista de pines G-S1

### 5.4.1 Tabla de escritores de configuración de GPIO (antes y después)

| Núcleo | Archivo | Función / Contexto | Puerto / Pines | Momento (respecto a CPU2 release) | Acceso en `07db657` | Acceso en `7cc456d` (F1b) |
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

### Desviaciones declaradas y aprobadas:
1. **Origen de objetos enlazados en T2a (`build.ninja`):** Los objetos enlazados en cada una de las 7 ELFs se extraen de las reglas de enlace del archivo `build.ninja` de CMake (no del archivo `.map`), garantizando la lista exacta de objetos que participan en el ejecutable final.
2. **Filtro de objetos de la HAL de ST en T2a:** Se filtran los objetos correspondientes al driver del proveedor de ST (`stm32h7xx_hal_*`). En particular, `stm32h7xx_hal_rcc.c.obj` contiene la función `HAL_RCC_MCOConfig`, la cual referencia `HAL_GPIO_Init`; sin embargo, `HAL_RCC_MCOConfig` no está enlazada ni llamada en ninguna de las 7 ELFs de producción (comprobado mediante desensamblado con `arm-none-eabi-objdump`), siendo `gpio_hal.c.obj` el único llamador real en las 5 ELFs CM7.
3. **Ejecución secuencial en T5:** El arnés de T5 ejecuta las pruebas de forma secuencial (`monitor.py` inicial, `net_check.py`, `query_adc.py`, y `monitor.py` final con verificación de estadísticas de red). Esta secuencia evita contenciones y colisiones en el puerto USB-VCP (COM4) y la pila de red, garantizando estabilidad absoluta bajo acceso remoto (G-S7).
4. **Inclusión de `stm32h7xx_ll_gpio.h` en excepciones de T2b:** El encabezado `stm32h7xx_ll_gpio.h` contiene macros y funciones inline provistas por ST para la manipulación de registros GPIO. Se incluye en la lista cerrada de excepciones relativas debido a que es incluido transitivamente por los drivers del sistema.

---

## 5.6 Respuesta a la auditoría de F1 (`AUDIT.md`)

A continuación se detalla la resolución de los hallazgos de la auditoría y de la re-auditoría:

### Hallazgos de la primera entrega (B1–B7, M1–M6, m1–m5)
- **B1 (HSEM_R5 = 0x58026414):** Base corregida a `0x58026400` y lectura exclusiva de `HSEM_R5` (`0x58026414`), evitando la toma del semáforo.
- **B2 (Rutas reales):** Rutas corregidas a `prj_02_eth_lwip_dualcore` y `prj_03_adc_dma_eth`.
- **B3 (Criterio T2a):** Inspección de todos los objetos enlazados vía `nm -u`.
- **B4 (Control negativo T2a):** Verificado el fallo del criterio en las 5 CM7 de `07db657`.
- **B5 (Captura T3 control):** Captura limpia con lectura de símbolos NULL en `07db657`.
- **B6 (Identidad por flasheo verificado):** Cada sesión incluye *"Download verified successfully"*.
- **B7 (Gestión de errores en T2b):** Fallos de preprocesado o presets faltantes concluyen en `FAIL`.
- **M2 (T15 en `selftest-gpio`):** Desinstalación explícita antes de (d) y (g), y restauración incondicional al salir.
- **M3 (Árbol limpio):** Capturas ejecutadas con `tree clean` y filtros `-uno` / `--ignore-submodules=all`.
- **M4 (Arnés commiteado y G-S5):** Arnés integrado en `tools/gpio_f1b_checks.py`; ningún reset con herramientas activas.
- **M5 (`--test all`):** Expande y ejecuta T2a, T2b, T3 y T4.
- **m1 (Corrección tabla §5.4.1):** Actualizada con el momento exacto y tipo de llamada.
- **m3 (`App_Log_Init`):** Documentado para el README de `prj_04`.
- **m5 (Aritmética T4):** Comprobación negativa de 10x mantenida.

### Respuesta a la re-auditoría de F1 (`32f343a`)
- **M1 (Expresión regular unificada, macros CMSIS y autoprueba de T2b):** Se implementó la expresión regular unificada `RAW_WRITE_RE` en `tools/gpio_f1b_checks.py`. Detecta bases expandidas de CMSIS (`((GPIO_TypeDef *) (...))->MODER =`), macros (`WRITE_REG`, `MODIFY_REG`, `SET_BIT`, `CLEAR_BIT`), operadores compuestos (`|=`, `&=`, `<<=`), y sentencias distribuidas en múltiples líneas con cualquier nivel de anidamiento de paréntesis. La expresión rechaza comparaciones (`==`, `!=`, `<=`) y lecturas. La autoprueba automatizada `run_t2b_selftest()` procesa los encabezados CMSIS reales (`stm32h7xx.h`) sobre 12 formas de escritura y 9 de lectura, detectando exactamente 12/12 escrituras y 0/9 lecturas falsas. El control negativo en `07db657` busca por tupla `(preset, ruta_relativa)`, detectando 20 escrituras e imprimiendo la lista detallada de hallazgos.
- **B7 (Control negativo exige presets completos en T2b):** Se exige que los 6 presets de `prj_04` estén configurados y preprocesados sin fallos en el árbol de control (1951 unidades escaneadas, 0 fallidas). La ausencia de un preset o fallo de preprocesado en control resulta en `FAIL`.
- **N1 (Corrección de `pd8_ok` en T3):** Se corrigió la verificación de `PD8` en `gpio_f1b_checks.py` (evaluaba erróneamente `pb1_ok`, ahora evalúa `pd8_ok`).
- **N2 (Verificación estricta de telemetría ADC en T5):** T5 analiza los valores numéricos devueltos por `query_adc.py`: exige que `Samples` crezca estrictamente entre la primera y la última lectura (`samples[-1] > samples[0]`) y que `OVR == 0` en todas las muestras. La herramienta imprime explícitamente en la captura que `query_adc.py` reporta CH5, Rate, Seq, Samples, TC, OVR y M4 HB, pero no emite `dma_error_count` ni `half_overrun_count`.
- **N3 (Verificación estricta de contadores en T5 y reporte verídico en T7):** En T5, se parsean todas las líneas `[net]` de `monitor.py` y se verifica estrictamente que `rx_nobuf=0`, `tx_nobuf=0`, `dma_err=0` y `acl_drop=0` en todas ellas. En T7, la herramienta no finge la captura del banner inicial (el cual se emite antes de la conexión serial del host por VCP); en su lugar, reporta verídicamente `T7_prj01_console_active` (mediante recepción de líneas periódicas `[ipc]` con CPU2 alive) y `T7_prj01_image_identity` (confirmada mediante flasheo verificado con B6).
- **N4 (`--control` implica `--flash`):** En la CLI de `gpio_f1b_checks.py`, el flag `--control` activa automáticamente el flasheo de la imagen de control correspondiente, evitando pasar inadvertidamente sobre la imagen actual.
- **N5 (Retardo de asentamiento tras flasheo en T3):** Se añadió una espera de 3.0s tras el flasheo en T3 para permitir que el firmware arranque e inicialice los periféricos antes de la inspección por SWD.
- **N6 (Directorio de control obligatorio en T2b):** Si no se proporciona un directorio de control válido, T2b concluye en `FAIL`.
- **N7 (Veracidad del informe):** Se actualizaron §5.5, §5.6 y §5.8 reflejando con exactitud el origen de objetos en `build.ninja`, el filtro de HAL, la ejecución secuencial de T5 y las capturas reales.
- **M6 (Evidencia de reflasheo final de `prj_04` debug):** Tras completar T7, se reflasheó formalmente `prj_04` debug y se ejecutó `net_check.py --host 192.168.1.150` (3/3 PASS), registrando la evidencia completa en `docs/gpio_f1b_m6_netcheck_7cc456d.txt`.
- **m2 (Captura T1 en formato limpio):** Captura `gpio_f1b_t1_7cc456d.txt` generada en UTF-8 puro, sin BOM ni fragmentos UTF-16, registrando las líneas de comando, los 8 proyectos del build matrix, los 5 presets adicionales de `prj_04`, las 5 compilaciones con `-Werror` (todas con 0 avisos) y los tamaños de objetos.
- **m4 (Offsets DWARF y límites dinámicos sin respaldos silenciosos):** En `gpio_f1b_checks.py`, la función `get_struct_member_offset()` devuelve `None` si un campo no se encuentra en DWARF (generando una excepción explícita si se requiere), y `get_t4_limits()` lanza excepción si las macros no están presentes en el header. En modo control, T3 no intenta consultar en DWARF campos inexistentes de versiones anteriores.

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

1. **T6b (arranque en frío):** Ejecutado con éxito en el banco físico por el responsable en `492d156` / `74c9119` (`RCC_RSR = 0x00FE0000`, SWD checks PASS, T3 PASS, T5 PASS en debug).
2. **Caminos no ejercitados en hardware:** `UART_HAL_MspDeInit_User()` y `ETH_HAL_MspDeInit_User()` no son invocados por ninguna ruta de ejecución de los proyectos de firmware actuales.
3. **M2 (`selftest-gpio`):** No se ejecuta en hardware físico por regla G-S1 (mueve líneas de banco); validado formalmente por revisión de código y compilación limpia.
4. **Imagen que quedó en la placa (M6):** La placa física quedó flasheada y en ejecución con la imagen de producción **`prj_04_adc_dma_bm_eth` preset `debug` (commit `7cc456d`)**, confirmada mediante ejecución exitosa de `net_check.py --host 192.168.1.150` (3/3 checks PASS, ICMP 4/4, UDP sweep 1..1472 B, burst 100/100), registrada en `docs/gpio_f1b_m6_netcheck_7cc456d.txt`.

---

## 5.9 Cierre

Tras la aprobación de la auditoría de F1 en `7687e91` y la ejecución exitosa de la prueba de arranque en frío T6b en el banco físico (`74c9119`), se procedió a la fase de cierre de la documentación técnica. Se actualizaron los siguientes documentos:

### 1. `01_shared_drivers/00_gpio/README.md`
- **§3.3 (Escritores fuera del driver):** Se actualizó la tabla de escritores externos: `04_uart` (`uart_hal`) marcado como migrado a `GPIO_HAL_Init` y `GPIO_HAL_DeinitPin` desde F1b; `08_eth` verificado bajo `gpio_hal`; se mantienen como excepciones documentadas `led_bm` (inicialización mono-núcleo previa a la liberación de CPU2) y `LED_PanicRed` (G14, ruta de pánico).
- **Límite de T2b (m9):** Se documentó explícitamente en §3.3 la limitación del análisis estático de T2b, el cual opera a nivel de AST/código fuente preprocesado y detecta accesos directos a periféricos GPIO o macros CMSIS, pero no puede resolver escrituras a registros a través de punteros genéricos o desreferencias indirectas (`uint32_t *p = ...; *p = ...`).
- **§11 (Consumidores):**
  - `04_uart`: Documentada la MSP de consola mediante `GPIO_HAL_Init` y `GPIO_HAL_DeinitPin` bajo la sección crítica de `00_gpio`.
  - `08_eth`: Corregida la descripción (utiliza `GPIO_HAL_Init` y `GPIO_HAL_DeinitPin` desde el cierre de 08_eth).
  - `prj_04`: Documentado el hook de bloqueo compartido instalado en ambos núcleos desde F1b.
- **§12.2 (Hallazgos G1 y G13):**
  - Marcados como cerrados en `prj_04` con los hashes de commit (`9051a6a`, `53f0f88`, `c8001bf`, `ee2ba23`) y la evidencia documental completa (`docs/gpio_f1b_*`, incluyendo las capturas del auditor `docs/gpio_f1b_audit*`).
  - Se explicitó que el hallazgo G1 permanece abierto en `prj_03` a la espera de la fase F1c.
- **§13.1 (F1b completado):**
  - Transformado de trabajo abierto a entrega completada y cerrada en la rama `driver/gpio-f1b`.
  - Detalle de la implementación del hook HSEM 5 (60 µs, límites de ciclos, variables en SRAM4 `0x38008000`), migración de escritores en CM7 y CM4, preservación de hooks en `selftest-gpio`, matriz completa de pruebas T1–T7, T6b en frío y pruebas independientes del auditor.
  - Registro de caminos no ejercitados en hardware: `selftest-gpio` solo compilado (regla de seguridad G-S1); fase T15 de contención no ejecutada en el banco físico para `prj_04` (validada previamente en `prj_01`).
- **§15 (Historia):**
  - Añadido el hito F1b con el rango de commits (`9051a6a … 7cc456d, 492d156, 74c9119`).
  - Actualizada la instrucción para recuperar los documentos de proceso una vez que sean retirados por el responsable de integración mediante `git show <commit que los borra>^:ruta` (con comando exacto usando `git log --diff-filter=D`), idéntico al mecanismo establecido en `05_i2c` y `08_eth`.

### 2. `01_shared_drivers/04_uart/README.md`
- **Uso de `GPIO_HAL_Init` y `GPIO_HAL_DeinitPin`:** Se especificó en la sección referente a `00_gpio` que la función MSP por defecto de la consola utiliza `GPIO_HAL_Init` y `GPIO_HAL_DeinitPin` bajo la sección crítica de `gpio_bm`, eliminando cualquier referencia a llamadas no protegidas de `HAL_GPIO_Init`.
- **Propagación de errores:** Se documentó que tanto `UART_HAL_InitEx` como `UART_Console_Init` capturan y propagan cualquier error devuelto por la inicialización de la MSP hacia el llamador.

### 3. `02_firmware_projects/prj_04_adc_dma_bm_eth/README.md`
- **Hook de `00_gpio`:** Se documentó la arquitectura del hook basado en HSEM 5, timeout de 60 µs (`SHARED_GPIO_LOCK_US = 60U`), con límites de ciclos calibrados por núcleo (24.000 ciclos en CM7 @ 400 MHz, 12.000 ciclos en CM4 @ 200 MHz).
- **Estructura en memoria compartida:** Se detalló la ubicación en `SharedMemory_t` (SRAM4, base `0x38008000`) de los contadores de fallos (`m7_gpio_lock_fail_count` en `0x4fc`, `m4_gpio_lock_fail_count` en `0x500`) y de los registros de retención máxima en ciclos (`m7_gpio_lock_hold_cycles_max` en `0x504`, `m4_gpio_lock_hold_cycles_max` en `0x508`).
- **Puntos de instalación:** Documentada la secuencia de arranque en CM7 (`main.c`, tras `Shared_Init()` y antes de `HSEM_BM_BootReleaseCpu2()`) y en CM4 (`main.c`, tras `HSEM_BM_TimebaseStart()` y antes de `ADC_Acquisition_Init()`).
- **Evidencia documental de F1b:** Tabla detallada referenciando todas las capturas generadas (`gpio_f1b_t1_*`, `gpio_f1b_t2_*`, `gpio_f1b_t3_*`, `gpio_f1b_t4_*`, `gpio_f1b_t5_*`, `gpio_f1b_t6a_*`, `gpio_f1b_t7_*`, `gpio_f1b_m6_*`, `gpio_f1b_t6b_*`, y las capturas del auditor `gpio_f1b_audit*`).
- **Observación m3:** Se documentó que `App_Log_Init()` en CM7 descarta mediante casteo a `(void)` el valor de retorno de `UART_Console_Init()`, por lo que un fallo en la inicialización de pines de la consola en la MSP no detiene el arranque de la aplicación.
