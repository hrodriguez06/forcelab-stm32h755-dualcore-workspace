# Plan de `00_gpio` F1c: el lock entre núcleos en `prj_03` (G1)

Oct 13, 2026 · @Hector · revisión 1

Este plan cierra G1 de `00_gpio/README.md` §12.2 en `prj_03_adc_dma_eth`, el último proyecto donde sigue abierto. En `prj_03`, CM7 configura PB13 (TXD1 de Ethernet) en `net_task` después de liberar a CPU2, y CM4 configura PB1 (entrada del ADC) con `HAL_GPIO_Init` en `GPIO_Analog_Init()`. Las dos escrituras son read-modify-write de `GPIOB_MODER` sin el lock entre núcleos. Si se intercalan, PB13 puede volver a analógico: el enlace sigue arriba y la transmisión muere sin que nada lo detecte.

Es el mismo trabajo que F1b hizo en `prj_04`, más pequeño. Las MSP de la consola (`04_uart`) y de Ethernet (`08_eth`) ya pasan por `gpio_hal` desde F1b y el cierre de `08_eth`. Falta que el proyecto instale el hook y que CM4 configure su pin por el driver. No es un driver nuevo y ninguna firma cambia.

Una fase de implementación (F1) y un cierre. La fase pasa una auditoría independiente con ejecución propia del auditor en la placa. Las reglas del implementador están en [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md).

Base: `e6cb75e` de `main` (cierre de F1b), árbol limpio, rama `driver/gpio-f1c`.

**Decisiones del responsable (2026-10-13):** las cuatro decisiones abiertas, aprobadas con su opción por defecto (1: `10_hsem` BM solo para el hook, el arranque de CPU2 sigue con `HAL_HSEM`; 2: 60 µs; 3: `--project` en `gpio_f1b_checks.py`; 4: los documentos de proceso se borran al cierre).

**Lo que se aprendió en F1b y aquí es norma desde el principio.** La auditoría de F1b no pasó dos veces por la evidencia, no por el código:
- **HSEM:** leer `HSEM_R5`, nunca `RLR` (leerlo toma el semáforo);
- **comprobaciones:** cada una tiene que poder fallar y cuenta en el veredicto;
- **identidad:** cada captura de placa prueba qué imagen había;
- **sin silencios:** un fallo de una herramienta o un archivo que falta es FAIL, nunca "sin hallazgos";
- **T2b:** ve también las escrituras con macro y lleva su autoprueba.

## Encargo

| ID | Hoy | Qué cambia |
| --- | --- | --- |
| H1 | `prj_03` no instala el hook ni enlaza `10_hsem`: el arranque de CPU2 usa `HAL_HSEM` (`CM7/Core/Src/main.c:98-100`, `CPU2_WaitForCPU1()` en CM4), y CM4 solo enlaza `stm32h7_driver_adc` | El hook de `prj_04` copiado a `prj_03/Common`:<br>• `BOARD_GPIO_HSEM_ID 5U` y `SHARED_GPIO_LOCK_US` (decisión 2) con su conversión por núcleo;<br>• `shared_gpio_lock`/`shared_gpio_unlock` sobre `HSEM_BM_TakeTimeout`/`HSEM_BM_Release`, con la medida de la retención;<br>• `g_shared_gpio_lock` y `Shared_GpioLockInstall()`;<br>• `m7_/m4_gpio_lock_fail_count` y `m7_/m4_gpio_lock_hold_cycles_max` añadidos **al final** de `SharedMemory_t`.<br>Los dos núcleos enlazan `stm32h7_driver_hsem_bm` y llaman `HSEM_BM_Init()` y `HSEM_BM_TimebaseStart()` antes de instalar el hook (decisión 1) |
| H2 | CM4: `GPIO_Analog_Init()` (`CM4/App/Src/adc_acquisition.c:38-55`) configura `BOARD_ADC_PORT` con `HAL_GPIO_Init` | `GPIO_BM_ConfigPin` en modo analógico sin pull, como `prj_04` (`53f0f88`); `GPIO_Analog_Init()` devuelve `bool` y un fallo hace fallar `ADC_Acquisition_Init()` |

**Fuera de este plan:**
- el arranque de CPU2 por `HAL_HSEM`, que no cambia (decisión 1);
- F2 (EXTI, G6, la celda de compensación) y G14 (`LED_PanicRed`, excepción aceptada);
- `prj_eth_lwip_node`, que tiene copias privadas de los drivers y un solo núcleo;
- los README de los drivers, salvo `00_gpio` en el cierre.

## Fuentes normativas

- `00_gpio/README.md` §3 (contrato: hook, orden, escritores), §10 (A2: tiempo enmascarado), §12.2, §13.1 (F1b: lo que se hizo en `prj_04`).
- El hook de referencia: `02_firmware_projects/prj_04_adc_dma_bm_eth/Common/Inc/shared_memory.h` y `Common/Src/shared_memory.c` (F1b, `9051a6a`), y dónde se instala en sus dos `main.c`.
- `10_hsem/README.md` §3 (`HSEM_BM_Init`, `HSEM_BM_TimebaseStart`, `HSEM_BM_TakeTimeout`; leer `RLR` toma el semáforo).
- RM0399 §11 (HSEM) y §12 (GPIO).

## Lo que existe hoy (`e6cb75e`)

| Elemento | Estado |
| --- | --- |
| Escritores de configuración de GPIO de `prj_03` | CM7: `LED_Init` (antes de liberar a CPU2), `LED_PanicRed` (G14), la MSP de la consola (`App_Log_Init` en `app_main`, después de liberar a CPU2; `GPIO_HAL_Init` desde F1b) y la MSP de `08_eth` en `net_task` (después; `GPIO_HAL_Init`). CM4: `GPIO_Analog_Init` (`HAL_GPIO_Init`, después). Preset `selftest`: `ADC_SelfTestHw_Run` configura sus pines por los drivers de `07_adc` (`GPIO_BM_ConfigAnalog`) |
| Presets | `debug`, `release`, `selftest` (`ADC_DRIVER_SELFTEST`) |
| Orden de arranque | CM7: `SystemClock_Config`, `Shared_Init` (l. 93), liberación de CPU2 por `HAL_HSEM` y espera de `D2CKRDY` (l. 95-108), `app_main` (l. 111). CM4: `CPU2_WaitForCPU1`, `HAL_Init`, selftests (preset `selftest`), `ADC_Acquisition_Init` (l. 82) |
| `Common/Inc/shared_memory.h` | No define `BOARD_GPIO_HSEM_ID` (ocupa los HSEM 0-4). `SharedMemory_t` es `aligned(32)` y termina en `m4_to_m7[]` |
| Enlace | CM7: `stm32h7_shared_drivers` (sin `10_hsem`); CM4: `stm32h7_driver_adc` |
| Telemetría | `[net] link=… rx_nobuf=… tx_nobuf=… dma_err=…` (sin `acl_drop`); `query_adc.py` imprime `Samples` y `OVR` (`half_overrun_count`); el JSON trae también `err` (`dma_error_count`), que `query_adc.py` no imprime |
| Pines en producción | RMII PA1, PA2, PA7, PB13, PC1, PC4, PC5, PG11, PG13; LED PB0, PE1, PB14; USART3 PD8, PD9; ADC PB1 (`BOARD_ADC_USE_FAST_CHANNEL_PB1 = 1`). Ninguno es línea del banco |

## Diseño

1. **H1.**
   - **Bloque compartido:** `BOARD_GPIO_HSEM_ID 5U` en `shared_memory.h`, con la tabla de IDs al día.
   - **El hook:** se copia de `prj_04` (`Common/Src/shared_memory.c` desde "00_gpio cross-core lock hook").
   - **Los campos:** van al final de `SharedMemory_t`; ningún campo existente se mueve.
   - **Instalación en CM7:** después de `Shared_Init()` y antes de liberar a CPU2, precedida de `HSEM_BM_Init()` y `HSEM_BM_TimebaseStart()` (decisión 1), comprobando lo que devuelven; un fallo va a `Error_Handler()`.
   - **Instalación en CM4:** justo después de `CPU2_WaitForCPU1()` y `HAL_Init()`, con las mismas dos llamadas antes, y siempre antes de la primera configuración de un pin (selftests y `ADC_Acquisition_Init`).
   - **Enlace:** los dos `CMakeLists.txt` de `prj_03` enlazan `stm32h7_driver_hsem_bm`.
2. **H2.** Como `prj_04` (`53f0f88`): `GPIO_BM_ConfigPin` con `BOARD_ADC_PORT` y `GPIO_BM_PIN_INDEX(BOARD_ADC_PIN)`, analógico, sin pull; sin los `__HAL_RCC_GPIOx_CLK_ENABLE` (la llamada habilita el reloj).
3. **Sin firmas nuevas** ni cambios en `01_shared_drivers`.
4. **Herramienta.** `tools/gpio_f1b_checks.py` gana `--project {prj_04,prj_03}` (por defecto `prj_04`, que tiene que seguir funcionando igual). Lo que cambia por proyecto va en una tabla dentro de la herramienta:
   - las ELF de T2a: CM7 y CM4 de debug y release;
   - los presets de T2b;
   - el commit y el directorio de control;
   - las excepciones de T2b;
   - los pines y controles de T3;
   - los contadores de `[net]` (sin `acl_drop` en `prj_03`);
   - el formato de `query_adc.py`.

   Además:
   - **los tres menores pendientes de F1b:**
     - `g_shared` sale de `nm` sin respaldo;
     - el control de T2b compara por ruta relativa completa (`CM4/Core/Src/main.c`, no "main.c");
     - las rutas del control se calculan respecto a su raíz y las líneas son las de la escritura;
   - **la regla de F1b:** todo valor que no se pueda derivar (de `nm`, DWARF o el header) es FAIL.

## Archivos

| Ruta | Cambio |
| --- | --- |
| `02_firmware_projects/prj_03_adc_dma_eth/Common/Inc/shared_memory.h`, `Common/Src/shared_memory.c` | H1 |
| `prj_03/CM7/Core/Src/main.c`, `prj_03/CM4/Core/Src/main.c` | H1 (init de `10_hsem`, instalación) |
| `prj_03/CM7/CMakeLists.txt`, `prj_03/CM4/CMakeLists.txt` | H1 (`stm32h7_driver_hsem_bm`) |
| `prj_03/CM4/App/Src/adc_acquisition.c` | H2 |
| `tools/gpio_f1b_checks.py` | `--project`, los menores de F1b |
| `prj_03/docs/gpio_f1c_*` | Las capturas |
| `00_gpio/README.md`, `prj_03/README.md` | Solo en el cierre |

## Pruebas

**Oráculos:**
- el enlazador (`nm`);
- el preprocesador;
- los registros leídos por SWD frente a una tabla escrita a mano y a las direcciones de `nm`;
- `net_check.py`, `query_adc.py` y los contadores del firmware.

Como en F1b, no se intenta provocar la carrera: se prueba que todos los escritores pasan por el lock y que el lock está instalado y funciona.

| ID | Qué | Oráculo | Control negativo | Criterio |
| --- | --- | --- | --- | --- |
| T1 | Builds: `build_all -Clean` 8/8; los tres presets de `prj_03`; `prj_03` debug y release con `-Werror`; `prj_04` debug (la herramienta y `--project prj_04`) | Exit y avisos | — | Todo compila, 0 avisos. Una sola salida UTF-8 con los comandos |
| T2a | `nm -u` de cada objeto enlazado (de `build.ninja`) en las ELF CM7 y CM4 de `prj_03` debug y release | `HAL_GPIO_Init` solo lo referencian `gpio_hal.c.obj` (y los objetos `stm32h7xx_hal_*` de ST, declarados); `HAL_GPIO_DeInit` no está definido; una ELF u objeto que falta es FAIL | Las mismas ELF compiladas en `e6cb75e`: `adc_acquisition.c.obj` referencia `HAL_GPIO_Init` en las dos CM4, y el criterio **falla** | Se cumple en las cuatro; el control falla en las dos CM4 |
| T2b | Escrituras a mano de `MODER`, `OTYPER`, `OSPEEDR`, `PUPDR`, `AFR` en cada unidad de `compile_commands.json` de `prj_03` (`debug`, `release`, `selftest`), preprocesada. Excepciones de F1b, por ruta relativa | Escrituras restantes por (preset, ruta, línea); unidades escaneadas y fallidas por preset | La autoprueba de F1b (12 formas de escritura detectadas, 9 lecturas ignoradas), que tiene que pasar en la misma ejecución. No hay control de árbol: en `e6cb75e`, `prj_03` no escribe registros a mano (su escritor es una llamada a la HAL, que cubre T2a). La captura lo dice | 0 escrituras fuera de las excepciones, 0 unidades fallidas, autoprueba 12/12 y 0/9 |
| T3 | Con la imagen debug corriendo, por SWD y sin parar:<br>• `s_shared_lock` de cada núcleo;<br>• `HSEM_R5` (`0x58026414`; **nunca `RLR`**);<br>• `m7_/m4_gpio_lock_fail_count`;<br>• PB1, PB13, PD8, PD9 y los controles PB15 y PD0 | `s_shared_lock` = `g_shared_gpio_lock` de su ELF; `HSEM_R5 = 0`; contadores a 0; PB1 analógico sin pull; PB13 AF11, PP, muy alta, sin pull; PD8/PD9 AF7, PP, baja, pull-up; controles en reset; los cinco campos de cada pin | La imagen de `e6cb75e` flasheada (`--control` implica `--flash`): `s_shared_lock` = 0 en los dos núcleos, en las direcciones de `nm` de **esas** ELF | Todo exacto; el control falla |
| T4 | `m7_/m4_gpio_lock_hold_cycles_max` por SWD tras el arranque | Los dos distintos de 0 y `límite / máximo ≥ 10` (límites del header) | El mismo cálculo con un límite diez veces menor falla | Los dos ≥ 10× |
| T5 | Regresión, debug y release: `monitor.py --sn`, `net_check.py`, `query_adc.py` (secuencial, como F1b; nunca un reset con una herramienta corriendo) | ICMP 4/4, UDP de 1 a 1472 B idénticos, ráfaga 100/100; en cada línea `[net]`, `rx_nobuf`, `tx_nobuf` y `dma_err` a 0 (al menos una línea); `Samples` crece entre lecturas y `OVR` = 0 en todas | — | Todo, en las dos imágenes |
| T6a | 20 resets por SWD (`-rst`), 10 s, T3 completa tras cada uno | T3 | — | 20/20 |
| T6b | Arranque en frío: el responsable apaga y enciende la placa; `RCC_RSR`, T3 y T5 sin flasheo ni reset desde el encendido | T3 y T5; `RCC_RSR` con los flags de encendido | — | Pasa. Necesita al responsable en el banco: no bloquea la entrega de F1, sí el cierre |
| T7 | La herramienta sigue funcionando para `prj_04`: `--project prj_04 --test t2` | El mismo resultado que `gpio_f1b_t2_7cc456d.txt` (1951 unidades, 0 escrituras, control con 20 hallazgos), ahora con las rutas y líneas corregidas | — | PASS |

**Capturas** (las reglas de F1b, revisión 3):
- **Qué:** una ejecución sin editar por prueba y por herramienta, con la línea de comando, la fecha y el hash del árbol limpio, en `prj_03/docs/gpio_f1c_<prueba>_<hash>.txt`; T7 va en `prj_04/docs/`.
- **Identidad:** cada sesión de placa empieza flasheando la ELF compilada en ese commit, con su "Download verified successfully" en la captura, y no hay otra compilación ni otro flasheo en medio.
- **Lo leído:** toda dirección que se lee se imprime junto a su origen.
- **Árbol sucio:** con el árbol sucio, `git status --porcelain` impreso y la captura no cuenta.

## Banco

El de F1b, sin cambiar un cable. Incluye las tomas J2/J3 y la F334 de `05_i2c`, y el cable Ethernet a la red del responsable (`prj_03` usa `192.168.1.150`, como `prj_02` y `prj_04`).

| # | Regla |
| --- | --- |
| G-S1 | Solo se flashean `prj_03` debug y release y la imagen de control de `e6cb75e`. Nunca el preset `selftest` de `prj_03` (`ADC_SelfTestHw` toca ADC1/2/3 y sus pines). Antes del primer flasheo, la lista de pines de "Lo que existe hoy" se comprueba contra el código y se pone en el informe |
| G-S2 | Dos ST-LINK: `sn=0016000C4142501220353451` siempre |
| G-S3 | El AD2, en estado seguro (sin script) |
| G-S4 | La red la configura el responsable |
| G-S5 | Flashear o resetear solo con `monitor.py`, `net_check.py` y `query_adc.py` parados |
| G-S6 | Todas las imágenes son del repo: sin copia previa de la flash. El informe dice qué imagen quedó en la placa al terminar, con evidencia |
| G-S7 | Si el responsable está en remoto: nada que solo se recupere a mano. No se tocan la configuración de alimentación ni `SystemClock_Config`, ni los option bytes, ni el firmware del ST-LINK, y no se borra la flash entera; la recuperación es el NRST del ST-LINK (`mode=UR`, `-hardRst`); si la placa no responde, se para y se avisa |

## Fases

| Fase | Entregable | Gate |
| --- | --- | --- |
| **F1** | H1, H2; `--project` y los menores en la herramienta; T1–T5, T6a y T7 con sus capturas; el informe | Auditoría con revisión, builds y la ejecución propia del auditor (T2, T3 con su control, T5, T6a) |
| **Cierre** | Implementador: `00_gpio/README.md` (introducción, §11, §12.2 G1 cerrado en todos los proyectos, §13.1 o una §13.x de F1c, §15) y `prj_03/README.md` (el hook y la validación). Responsable: `DRIVER_ROADMAP.md`, borrado de los documentos de proceso, merge | T6b hecha; auditoría de cierre |

## Riesgos

| Riesgo | Mitigación |
| --- | --- |
| `HSEM_BM_Init()` interfiere con el arranque por `HAL_HSEM` | Solo habilita `HSEMEN` en el banco del núcleo y comprueba el núcleo (`10_hsem/hsem_bm.c:68-93`); no libera ni borra semáforos. En CM7 se llama después de `Shared_Init` y antes de liberar a CPU2; en CM4, después de despertar |
| El DWT de CM4 no está en marcha | `HSEM_BM_TimebaseStart()` lo arranca y devuelve `false` si `CYCCNT` no avanza: va a `Error_Handler()` |
| Añadir campos a `SharedMemory_t` mueve algo | Solo al final; la estructura es `aligned(32)`. T5 (`query_adc.py` y la consola) lo comprueba |
| La retención en `prj_03` (CM4 con HAL) supera 6 µs y 60 µs no da 10× | T4 lo mide en esta imagen; si no llega, se sube el límite (nunca se baja la puerta) |
| La herramienta deja de funcionar para `prj_04` | T7 |

## Decisiones abiertas

Cada una con su opción por defecto.

1. **El HSEM del hook:**
   - **Por defecto:** `prj_03` enlaza `stm32h7_driver_hsem_bm` y lo usa solo para el hook (`HSEM_BM_Init`, `HSEM_BM_TimebaseStart`, `HSEM_BM_TakeTimeout`). El arranque de CPU2 sigue con `HAL_HSEM`, sin cambios.
   - **Alternativa:** migrar también el arranque a `HSEM_BM_BootReleaseCpu2`/`BootWaitCpu2Parked` como `prj_04`. Es más trabajo y otra validación.
2. **El límite:** 60 µs, como `prj_01` y `prj_04`, con la puerta de 10× medida en esta imagen (T4).
3. **La herramienta:** **extender `tools/gpio_f1b_checks.py` con `--project`**, sin duplicarla. T7 prueba que `prj_04` sigue igual.
4. **Documentos de proceso al cierre:** se borran, como en F1b.
