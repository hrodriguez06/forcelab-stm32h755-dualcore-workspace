# Informe de implementación de `00_gpio` F1b (G1, G13 en `prj_04`)

Oct 12, 2026 · Rama `driver/gpio-f1b` · Commit base `07db657` / plan `50f16d8`

---

## 5.1 Commits

Rango de commits desarrollados en la rama `driver/gpio-f1b`:

| Commit | Asunto | Preocupación / ID |
| --- | --- | --- |
| `9051a6a` | `fix(prj04): install cross-core GPIO lock hook and measure retention` | H1, G1: Declaración e implementación de `Shared_GpioLockInstall()`, `shared_gpio_lock`/`unlock` con DWT hold cycle measurement en `shared_memory.[ch]`, instalación en CM7 antes de liberar CPU2 y en CM4 tras el arranque de base de tiempos. Reserva de 4 campos en `SharedMemory_t` ajustando padding a 1312 B. |
| `53f0f88` | `fix(prj04): route CM4 ADC pin configuration through gpio_bm (H2, G1)` | H2, G1: Migración de `GPIO_Analog_Init()` en `CM4/App/Src/adc_acquisition.c` de escrituras directas a `MODER`/`PUPDR` a `GPIO_BM_ConfigPin` con verificación de código de retorno. |
| `c8001bf` | `fix(uart): route MSP console pin configuration through GPIO_HAL (H3, G13)` | H3, G13 (Decisión 1A): Migración de MSP en `01_shared_drivers/04_uart/uart_hal.c` de `HAL_GPIO_Init` a `GPIO_HAL_Init` (almacenando estado de error en `s_msp_gpio_status` y verificándolo en `UART_Console_Init`/`UART_HAL_Init`) y de `HAL_GPIO_DeInit` a `GPIO_HAL_DeinitPin`. Enlace de `stm32h7_driver_gpio_hal` en CMake. |
| `ee2ba23` | `fix(prj04): route CM4 selftest GPIO writes through gpio_bm (H4, G13)` | H4, G13: Migración de escrituras directas a `GPIOD` en sonda de puente (`CM4/Core/Src/main.c`) a `GPIO_BM_ConfigOutput`, `GPIO_BM_ConfigInput`, `GPIO_BM_Write`, y en `CM4/App/Src/dma_selftest.c` a `GPIO_BM_ConfigAlternateEx`. |
| `61c6e28` | `fix(prj04): restore production GPIO lock hook after selftest-gpio (H5)` | H5 (Decisión 4): Restauración de `Shared_GpioLockInstall()` en lugar de `GPIO_BM_SetSharedLock(NULL)` al finalizar `gpio_selftest.c` en CM7 y CM4. |
| `af4162e` | `fix(prj04): preserve HSEM_ID_ADC_DATA definition in shared_memory.h` | Mantenimiento de macro `HSEM_ID_ADC_DATA 4U` en `shared_memory.h`. |
| `98af5a8` | `feat(tools): add gpio_f1b_checks.py for T2-T4 and T6a verification` | Implementación de herramienta de verificación `tools/gpio_f1b_checks.py` cubriendo T2a, T2b, T3, T4, T6a y controles negativos. |
| `0f2f7b1` | `fix(tools): handle escaped quotes in preprocessor and parallelize T2b checks` | Normalización de comillas en macros de preprocesador, adición de `-Wno-error` para escaneo de preprocesado, y paralelización de T2b con `ThreadPoolExecutor`. |
| `7ad0bbd` | `fix(tools): resolve control ELF symbol addresses in T3 negative control mode` | Resolución de direcciones de símbolos `s_shared_lock` desde los ELFs de `07db657` en modo `--control`, verificando que ambos punteros leen `NULL`. |
| `059a5bb` | `docs(prj04): record test captures for T2-T5, T6a, and T7` | Incorporación de las capturas crudas sin editar en `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/`. |

---

## 5.2 Builds y tamaños

### 5.2.1 Matriz de compilación `build_all.ps1 -Clean` (8/8 PASS)
Captura generada y validada en el árbol limpio (salida de `task-1297`):

| Proyecto | Preset | Núcleo | Estado | Text (B) | Data (B) | Bss (B) | Total (B) | Tiempo |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `prj_01_template_dualcore` | debug | CM7 | PASS | 30632 | 132 | 55676 | 86440 | 23.8s |
| `prj_01_template_dualcore` | debug | CM4 | PASS | 3456 | 24 | 12560 | 16040 | 23.8s |
| `prj_01_template_dualcore` | release | CM7 | PASS | 29620 | 132 | 55680 | 85432 | 17.3s |
| `prj_01_template_dualcore` | release | CM4 | PASS | 3112 | 24 | 12560 | 15696 | 17.3s |
| `prj_02_eth_lwip_dualcore` | debug | CM7 | PASS | 87312 | 152 | 120096 | 207560 | 28.5s |
| `prj_02_eth_lwip_dualcore` | debug | CM4 | PASS | 2580 | 24 | 12488 | 15092 | 28.5s |
| `prj_02_eth_lwip_dualcore` | release | CM7 | PASS | 83224 | 152 | 120096 | 203472 | 22.2s |
| `prj_02_eth_lwip_dualcore` | release | CM4 | PASS | 2332 | 24 | 12488 | 14844 | 22.2s |
| `prj_03_adc_dma_eth` | debug | CM7 | PASS | 88728 | 152 | 121216 | 210096 | 29.5s |
| `prj_03_adc_dma_eth` | debug | CM4 | PASS | 20944 | 24 | 16184 | 37152 | 29.5s |
| `prj_03_adc_dma_eth` | release | CM7 | PASS | 84576 | 152 | 121216 | 205944 | 23.3s |
| `prj_03_adc_dma_eth` | release | CM4 | PASS | 18556 | 24 | 16184 | 34764 | 23.3s |
| `prj_04_adc_dma_bm_eth` | debug | CM7 | PASS | 90512 | 156 | 121268 | 211936 | 29.1s |
| `prj_04_adc_dma_bm_eth` | debug | CM4 | PASS | 9844 | 28 | 15920 | 25792 | 29.1s |
| `prj_04_adc_dma_bm_eth` | release | CM7 | PASS | 86524 | 156 | 121264 | 207944 | 22.9s |
| `prj_04_adc_dma_bm_eth` | release | CM4 | PASS | 9296 | 28 | 15920 | 25244 | 22.9s |

Total: **8 de 8 configuraciones compilaron limpiamente (0 errores, 0 avisos con `-Werror`)**.

### 5.2.2 Los 7 presets de `prj_04_adc_dma_bm_eth`

| Preset | Núcleo | Estado | Text (B) | Data (B) | Bss (B) | Total (B) |
| --- | --- | --- | --- | --- | --- | --- |
| `debug` | CM7 | PASS | 90512 | 156 | 121268 | 211936 |
| `debug` | CM4 | PASS | 9844 | 28 | 15920 | 25792 |
| `release` | CM7 | PASS | 86532 | 156 | 121264 | 207952 |
| `release` | CM4 | PASS | 9296 | 28 | 15920 | 25244 |
| `selftest` | CM7 | PASS | 107616 | 156 | 122756 | 230528 |
| `selftest` | CM4 | PASS | 19936 | 28 | 20360 | 40324 |
| `selftest-adc` | CM7 | PASS | 91880 | 156 | 121524 | 213560 |
| `selftest-adc` | CM4 | PASS | 22392 | 28 | 20176 | 42596 |
| `selftest-uart` | CM7 | PASS | 92752 | 156 | 121268 | 214176 |
| `selftest-uart` | CM4 | PASS | 15856 | 44 | 15984 | 31884 |
| `selftest-hsem` | CM7 | PASS | 102540 | 156 | 124148 | 226844 |
| `selftest-hsem` | CM4 | PASS | 15528 | 28 | 19576 | 35132 |
| `selftest-gpio` | CM7 | PASS | 124516 | 160 | 122064 | 246740 |
| `selftest-gpio` | CM4 | PASS | 17820 | 32 | 18728 | 36580 |

Total: **7 de 7 presets compilaron con exit code 0 y 0 avisos**.

### 5.2.3 Comparación de tamaños de objetos (.text) antes y después

Comparación de los objetos modificados entre el baseline `07db657` y la rama `driver/gpio-f1b` (`0f2f7b1`):

| Objeto | Configuración | Baseline `07db657` (text / data / bss) | F1b `0f2f7b1` (text / data / bss) | Delta text | Causa del delta |
| --- | --- | --- | --- | --- | --- |
| `uart_hal.c.obj` (CM7) | `debug` | 1302 / 0 / 148 | 1328 / 0 / 149 | +26 B | Sustitución de `HAL_GPIO_Init` por `GPIO_HAL_Init`, almacenamiento de `s_msp_gpio_status` (+1 B bss) y comprobación en inicializadores. |
| `uart_hal.c.obj` (CM7) | `release` | 1200 / 0 / 148 | 1232 / 0 / 149 | +32 B | Misma causa que debug bajo optimización `-Os`. |
| `adc_acquisition.c.obj` (CM4) | `debug` | 788 / 4 / 2256 | 784 / 4 / 2256 | -4 B | Sustitución de bucle manual y escrituras de registro por llamada directa a `GPIO_BM_ConfigPin`. |
| `adc_acquisition.c.obj` (CM4) | `release` | 712 / 4 / 2256 | 720 / 4 / 2256 | +8 B | Verificación de código de retorno y propagación a `ADC_Acquisition_Init()`. |

---

## 5.3 Pruebas (T1–T7)

Todas las capturas se encuentran almacenadas sin editar en `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/`.

| ID | Qué | Oráculo | Control negativo | Criterio y resultado | Captura |
| --- | --- | --- | --- | --- | --- |
| **T1** | Compilaciones: `build_all.ps1 -Clean` (8/8) y los 7 presets de `prj_04` con `-Werror` | Exit code 0 y 0 avisos del compilador | — | 8/8 matrix PASS, 7/7 presets PASS, 0 avisos. | Log de tareas `task-1297` y tareas individuales |
| **T2a** | Enlace de símbolos HAL GPIO en producción | `nm` de `uart_hal.c.obj` importa `GPIO_HAL_Init` y `GPIO_HAL_DeinitPin`, y NO `HAL_GPIO_Init` ni `HAL_GPIO_DeInit`. `HAL_GPIO_DeInit` ausente en todas las ELFs de producción; `HAL_GPIO_Init` ausente en ELFs de CM4. | Control en `07db657`: `uart_hal.c.obj` importa `HAL_GPIO_Init` y `HAL_GPIO_DeInit`. | PASS (6/6 comprobaciones buenas; control negativo PASS). | `gpio_f1b_t2_0f2f7b1.txt` |
| **T2b** | Ausencia de escrituras directas a registros GPIO fuera del driver | Preprocesado (`gcc -E`) de todas las unidades de `compile_commands.json` en los 6 presets de `prj_04`: 0 escrituras a `MODER`, `OTYPER`, `OSPEEDR`, `PUPDR`, `AFR` fuera de excepciones. | Control en `07db657`: detecta escrituras en `adc_acquisition.c` (debug), `main.c` (selftest-uart) y `dma_selftest.c` (selftest) (14 coincidencias). | PASS (0 escrituras en rama actual; 14 detectadas en control). | `gpio_f1b_t2_0f2f7b1.txt` |
| **T3** | Inspección SWD en vivo del hook, HSEM 5, contadores y registros de pines | Punteros `s_shared_lock` coinciden con `g_shared_gpio_lock` (CM7: `0x8015ebc`, CM4: `0x8102638`); `HSEM_RLR5 = 0x0` (libre); fallos = 0; PB1 analógico; PB13 AF11 PP velocidad muy alta; PD8/PD9 AF7 PP pullup; pines de control PB15 y PD0 en reset. | Imagen de `07db657` flasheada en placa: ambos `s_shared_lock` leen `0x00000000` (NULL), T3 falla en ejecución estándar y pasa en modo control. | PASS (todos los valores exactos; control negativo falla en estándar y confirma NULL). | `gpio_f1b_t3_0f2f7b1.txt`, `gpio_f1b_t3_control_0f2f7b1.txt` |
| **T4** | Margen de retención del lock (Decisión 3) | Medición de `m7_/m4_gpio_lock_hold_cycles_max` por SWD tras el arranque: ambos > 0 y margen `>= 10.0x`. | Mismo cálculo con límite 10x inferior (2400 / 1200 ciclos) da margen `< 10x` y falla. | PASS (CM7: 1226 ciclos / 19.58x; CM4: 356 ciclos / 33.71x; control 1.96x / 3.37x). | `gpio_f1b_t4_0f2f7b1.txt` |
| **T5** | Regresión de producción (debug y release) | Banner con `Rev 0f2f7b1`; ICMP 4/4; UDP sweep 1..1472 B idénticos; UDP burst 100/100; `rx_nobuf=0`, `tx_nobuf=0`, `dma_err=0`, `acl_drop=0`; ADC telemetry streaming continuo (`OVR: 0`). | — | PASS en debug y release. | `gpio_f1b_t5_debug_0f2f7b1.txt`, `gpio_f1b_t5_release_0f2f7b1.txt` |
| **T6a** | Arranques repetidos por SWD (20 resets) | 20 resets secuenciales con `-rst`, espera de 10s tras cada uno, ejecución de T3 completo (10/10) tras cada reset. | — | PASS (20 de 20 resets con 10/10 comprobaciones T3 válidas). | `gpio_f1b_t6a_0f2f7b1.txt` |
| **T6b** | Arranque en frío (corte de alimentación física) | Repetición de T3 y T5 tras arranque en frío. | — | PENDIENTE por banco en remoto (G-S7). No bloquea F1; se ejecutará con acceso físico. | Ver §5.8 |
| **T7** | Consola de `prj_01` con `uart_hal` modificado (Decisión 1A) | `prj_01` debug flasheado en placa; banner con `Rev 0f2f7b1-dirty` y build id `51a310ffdf382d9c`; lectura SWD de PD8/PD9 en AF7 PP low pullup. | — | PASS (banner capturado íntegro; registros MODER, PUPDR, AFRH verificados). | `gpio_f1b_t7_0f2f7b1.txt` |

---

## 5.4 Escritores de GPIO en `prj_04` y lista de pines G-S1

### 5.4.1 Tabla de escritores de configuración de GPIO (antes y después)

| Núcleo | Archivo | Función / Contexto | Puerto / Pines | Momento (respecto a CPU2 release) | Acceso en `07db657` | Acceso en `0f2f7b1` (F1b) |
| --- | --- | --- | --- | --- | --- | --- |
| **CM7** | `01_led/led_bm.c` | `LED_BM_Init()` | PB0, PE1, PB14 | Antes de liberar CPU2 | `GPIO_BM_ConfigOutput` (driver protegido) | `GPIO_BM_ConfigOutput` (driver protegido) |
| **CM7** | `04_uart/uart_hal.c` | `UART_HAL_MspInit_User()` | PD8, PD9 (USART3) | Antes de liberar CPU2 | `HAL_GPIO_Init` (fuera de driver) | `GPIO_HAL_Init` (dentro de sección crítica y lock de `00_gpio`) |
| **CM7** | `04_uart/uart_hal.c` | `UART_HAL_MspDeInit_User()` | PD8, PD9 (USART3) | Tras el arranque (no llamado) | `HAL_GPIO_DeInit` (fuera de driver) | `GPIO_HAL_DeinitPin` (dentro de sección crítica y lock de `00_gpio`) |
| **CM7** | `08_eth/eth_hal.c` | `ETH_HAL_MspInit_User()` | PA1, PA2, PA7, PB13, PC1, PC4, PC5, PG11, PG13 (RMII) | Después de liberar CPU2 (en `net_task`) | `GPIO_HAL_Init` (driver `08_eth` E8) | `GPIO_HAL_Init` (driver `08_eth` E8) |
| **CM7** | `08_eth/eth_hal.c` | `ETH_HAL_MspDeInit_User()` | RMII (9 pines) | Tras el arranque (no llamado) | `GPIO_HAL_DeinitPin` (driver `08_eth` M1) | `GPIO_HAL_DeinitPin` (driver `08_eth` M1) |
| **CM4** | `CM4/App/Src/adc_acquisition.c` | `GPIO_Analog_Init()` | PB1 (ADC1_INP5) | Después de liberación de CPU2 | Escrituras manuales directas a `GPIOB->MODER` y `GPIOB->PUPDR` | `GPIO_BM_ConfigPin` (dentro de sección crítica y lock de `00_gpio`) |
| **CM4** | `CM4/Core/Src/main.c` | Sonda de puente USART2 (`selftest-uart`) | PD5, PD6 | Después de liberación de CPU2 | Escrituras manuales directas a `GPIOD->MODER`, `PUPDR`, `ODR` | `GPIO_BM_ConfigOutput`, `GPIO_BM_ConfigInput`, `GPIO_BM_Write` |
| **CM4** | `CM4/App/Src/dma_selftest.c` | Prueba de loopback DMA (`selftest`) | PD5, PD6 | Después de liberación de CPU2 | Escrituras manuales directas a `GPIOD->MODER`, `AFR[0]` | `GPIO_BM_ConfigAlternateEx` |
| **CM7/CM4** | `gpio_selftest.c` | `gpio_selftest` (`selftest-gpio`) | Varios puertos | Selftest de desarrollo | Restauraba `GPIO_BM_SetSharedLock(NULL)` al terminar | Llama a `Shared_GpioLockInstall()` restaurando el hook de producción (H5) |

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

No hubo desviaciones respecto a los requisitos ni consultas adicionales necesarias.

---

## 5.6 Respuesta a la auditoría

Esta es la entrega inicial de la fase F1b. No existen hallazgos de auditoría previos sobre esta fase.

---

## 5.7 Erratas del plan

1. **Plan T2a (`HAL_GPIO_Init` en CM7):**
   - *Texto del plan:* *"Ningún HAL_GPIO_Init/HAL_GPIO_DeInit enlazado en producción: nm de las ELF... Los símbolos no están definidos en la ELF. Control: HAL_GPIO_Init está en todas las CM7"*.
   - *Corrección técnica:* `00_gpio/gpio_hal.c:119` invoca explícitamente a `HAL_GPIO_Init(port, &init)` dentro de secciones críticas `GPIO_BM_SectionEnter`/`Exit`. Por diseño, el driver `gpio_hal` encapsula y utiliza las rutinas de inicialización de la HAL de ST bajo protección de cerrojo. En consecuencia, cualquier ELF que enlace el driver `gpio_hal` (incluidos `prj_01`, `prj_02`, `prj_03` y `prj_04` CM7) contiene `HAL_GPIO_Init` definido.
   - *Criterio exacto aplicado en T2a:*
     1. `uart_hal.c.obj` importa `GPIO_HAL_Init` y `GPIO_HAL_DeinitPin`, y NO importa `HAL_GPIO_Init` ni `HAL_GPIO_DeInit`.
     2. `HAL_GPIO_DeInit` está completamente ausente en todas las imágenes de producción.
     3. `HAL_GPIO_Init` está completamente ausente en las imágenes de CM4.
     4. En CM7 de `prj_04`, `gpio_hal.c.obj` es el único objeto del proyecto que referencia `HAL_GPIO_Init`.
2. **Plan T2b (Preprocesado en Windows):**
   - *Comportamiento:* Las macros de compilador con comillas escapadas (`-DFW_GIT_REV=\"...\"`) presentes en `compile_commands.json` provocaban errores de escape al invocarse a través de `subprocess` en Windows, abortando el preprocesado.
   - *Corrección implementada:* Normalización de comillas escapadas y adición de flag `-Wno-error` para la invocación exclusiva de preprocesado (`gcc -E`), permitiendo escanear limpiamente las 1440 unidades de compilación y detectar con exactitud matemática las 14 apariciones en el control `07db657`.
3. **Plan T3 (Direcciones en control negativo `07db657`):**
   - *Comportamiento:* En el commit baseline `07db657`, los símbolos estáticos `s_shared_lock` se ubicaban en `0x20012904` (CM7) y `0x30020928` (CM4) debido a diferencias menores de tamaño de variables previas.
   - *Corrección implementada:* `tools/gpio_f1b_checks.py` en modo `--control` resuelve las direcciones directamente desde los ELFs del worktree de control `07db657`, verificando que ambos punteros leen efectivamente `0x00000000` (NULL).

---

## 5.8 Lo abierto y lo no ejercitado

1. **T6b (arranque en frío):** Pendiente de ejecución presencial por el responsable en el banco de pruebas físico, debido a la política de acceso remoto seguro (G-S7: nadie en el laboratorio puede desconectar la alimentación o pulsar botones manualmente). Se validará durante la fase presencial previa al cierre definitivo.
2. **Caminos no ejercitados en hardware:** `UART_HAL_MspDeInit_User()` y `ETH_HAL_MspDeInit_User()` no son invocados por ninguna ruta de ejecución de los proyectos de firmware actuales (ningún proyecto de producción desinicializa la consola ni la interfaz Ethernet durante su ciclo de vida).
3. **Imagen que quedó en la placa:** Al finalizar la sesión de pruebas, la placa quedó flasheada y corriendo con la imagen de producción **`prj_04_adc_dma_bm_eth` preset `debug` (commit `0f2f7b1`)**, verificada con `net_check.py` respondiendo al 100% (3/3 PASS, IP `192.168.1.150`).
