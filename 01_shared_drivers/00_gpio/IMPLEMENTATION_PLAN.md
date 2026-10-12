# Plan de `00_gpio` F1b: el lock entre núcleos en `prj_04` (G1, G13)

Oct 12, 2026 · @Hector · revisión 2

Este plan cierra en `prj_04_adc_dma_bm_eth` el fallo real G1 de `00_gpio/README.md` §12.2 y la parte de G13 que le toca (§13.1): dos núcleos que escriben sin protección los registros de configuración del mismo puerto. En `prj_04`, CM7 configura PB13 (TXD1 de Ethernet) en `net_task` después de liberar a CPU2, y CM4 configura PB1 (entrada del ADC) a mano en `GPIO_Analog_Init()`. Las dos escrituras son read-modify-write de `GPIOB_MODER`. Si se intercalan, PB13 puede volver a analógico: el enlace sigue arriba y la transmisión muere sin que nada lo detecte.

El driver ya tiene todo lo necesario (la sección, el hook, `GPIO_HAL_Init`, T15 en `prj_01`); lo que falta es que el proyecto lo use. No es un driver nuevo y ninguna firma cambia.

Una fase de implementación (F1) y un cierre. La fase pasa una auditoría independiente con ejecución propia del auditor en la placa. Las reglas del implementador están en [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md).

Base: `07db657` de `main` (corrección de la alimentación, X1), árbol limpio, rama `driver/gpio-f1b`.

**Decisiones del responsable (2026-10-12):** las seis decisiones abiertas, aprobadas con su opción por defecto (1 A: `04_uart` cambia; 2: 60 µs; 3: se mide la retención; 4: se restaura el hook; 5: `prj_03` fuera, en F1c; 6: los documentos de proceso se borran al cierre).

**Revisión 2 (2026-10-12): la placa, en remoto.** El responsable no tiene acceso físico al banco durante F1: nadie puede apagar la placa, pulsar RESET, reconectar un USB ni cambiar un cable. Cambia T6 (el arranque en frío pasa a T6b, pendiente hasta que el responsable esté en el banco) y se añade G-S7. El último estado verificado del banco es el de las 09:45 del 2026-10-12: cable Ethernet conectado y red funcionando (T6 del auditor con `prj_eth_lwip_node`, PASS).

## Encargo

| ID | Hoy | Qué cambia |
| --- | --- | --- |
| H1 | `prj_04` no instala el hook: sin él, la sección de `gpio_bm` es solo PRIMASK del núcleo propio | `Shared_GpioLockInstall()` en los dos núcleos, como `prj_01` (`00_gpio/README.md` §3.1–§3.2): el hook sobre `BOARD_GPIO_HSEM_ID` (5) con el límite `SHARED_GPIO_LOCK_US` (decisión 2), sus contadores de fallo en el bloque compartido y, si se aprueba, la medida de la retención (decisión 3) |
| H2 | CM4: `GPIO_Analog_Init()` (`CM4/App/Src/adc_acquisition.c:43-61`) escribe `MODER` y `PUPDR` de `BOARD_ADC_PORT` a mano | Una llamada de `gpio_bm` con estado (`GPIO_BM_ConfigPin` en modo analógico, sin pull); si falla, `ADC_Acquisition_Init()` devuelve `false` (el camino de error que ya existe en `CM4/Core/Src/main.c:125`) |
| H3 | `04_uart/uart_hal.c`: la MSP por defecto de la consola configura PD8/PD9 con `HAL_GPIO_Init` (l. 98, 103) y los desconfigura con `HAL_GPIO_DeInit` (l. 112-113); en `prj_04` corre en `App_Log_Init()`, después de liberar a CPU2 | Decisión 1. Por defecto, `GPIO_HAL_Init` y `GPIO_HAL_DeinitPin` en el driver, como hizo `08_eth` (E8), y `stm32h7_driver_uart` enlaza `stm32h7_driver_gpio_hal` |
| H4 | CM4, solo en presets de selftest: la sonda del puente PD5→PD6 (`CM4/Core/Src/main.c:236-244`, `selftest-uart`) y la AF de PD5/PD6 (`CM4/App/Src/dma_selftest.c:834-837`) escriben `GPIOD` a mano mientras CM7 configura la consola en el mismo puerto | Llamadas de `gpio_bm` (salida, entrada con pull, AF7). La lógica de la prueba no cambia |
| H5 | `selftest-gpio` de `prj_04` deja `GPIO_BM_SetSharedLock(NULL)` al terminar sus fases con hook propio (`CM7/App/Src/gpio_selftest.c:742, 1294`; `CM4/App/Src/gpio_selftest.c:751, 864`) | Decisión 4: restaurar el hook de producción en lugar de `NULL` |

**Fuera de este plan:**

- **G1 en `prj_03`.** Tiene el mismo defecto: su CM4 configura PB1 con `HAL_GPIO_Init` (`CM4/App/Src/adc_acquisition.c:54`) mientras CM7 configura PB13, y no instala el hook. `00_gpio/README.md` no lo lista. Va a una fase F1c con la misma herramienta (decisión 5).
- **El resto:**
  - F2 (EXTI, G6, la celda de compensación);
  - G14 (`LED_PanicRed`, excepción aceptada);
  - H5c de `10_hsem` (EXTI, otro driver);
  - repetir el selftest F1a de `prj_04` con el procedimiento de 40 flancos (T9 en PF14): el banco tiene hoy las tomas de `05_i2c` en PF14/PF15, así que `selftest-gpio` solo se compila;
  - `prj_02`, cuyo CPU2 no configura ningún pin.

## Fuentes normativas

- `00_gpio/README.md` completo, sobre todo §3 (contrato del proyecto: hook, orden de llamadas, escritores fuera del driver), §4, §10 (A2: el tiempo enmascarado), §12.2 (G1, G13) y §13.1.
- `gpio_bm.h` y `gpio_hal.h` (estados, `GPIO_BM_ConfigPin`, `GPIO_HAL_Init`, `GPIO_HAL_DeinitPin`).
- `prj_01_template_dualcore`: `Common/Inc/shared_memory.h` y `Common/Src/shared_memory.c` (el hook de referencia), `CM7/Core/Src/main.c:115` y `CM4/Core/Src/main.c:73` (dónde se instala).
- `04_uart/README.md` §3 y §3.1 (la MSP y cómo se sustituye), `10_hsem/README.md` §3.5 (`HSEM_BM_TakeTimeout`, ciclos por núcleo).
- RM0399 §11 (HSEM), §12 (GPIO).

## Lo que existe hoy (`07db657`)

| Elemento | Estado |
| --- | --- |
| Escritores de configuración de GPIO de `prj_04` en producción (debug, release) | CM7: `LED_Init` (antes de liberar a CPU2), `LED_PanicRed` (fallo), MSP de la consola (`App_Log_Init`, después), MSP de `08_eth` en `net_task` (después; ya con `GPIO_HAL_Init` desde el cierre de `08_eth`). CM4: `GPIO_Analog_Init` (a mano) |
| Hook | No instalado. `GPIO_BM_SetSharedLock` solo aparece en `gpio_selftest.c` de los dos núcleos, con un hook de prueba (`k_t15_hook`) sobre el mismo HSEM 5 |
| `Common/Inc/shared_memory.h` | Reserva `BOARD_GPIO_HSEM_ID 5U`; no define el límite ni los contadores. `SharedMemory_t` termina en `m7_/m4_adc_lock_hold_cycles_max` y `_pad2[4]` (múltiplo de 32 B) |
| Orden de arranque | CM7: `SystemClock_Config`, `HSEM_BM_TimebaseStart` (l. 195), `Shared_Init` (l. 210), `HSEM_BM_BootReleaseCpu2` (l. 223), `app_main`. CM4: `HSEM_BM_Init` y `HSEM_BM_TimebaseStart` (l. 88-89), selftests, `ADC_Acquisition_Init` (l. 125) |
| Precedente de medida | `prj_04` ya mide la retención del HSEM 4 (`m7_/m4_adc_lock_hold_cycles_max`, R-B1) |
| Consumidores de `uart_hal.c` | `prj_01` (consola en CM7, antes de liberar a CPU2), `prj_02`, `prj_03`, `prj_04`. `prj_eth_lwip_node` tiene copia privada |
| Pines de `prj_04` en producción | RMII PA1, PA2, PA7, PB13, PC1, PC4, PC5, PG11, PG13; LED PB0, PE1, PB14; USART3 PD8, PD9; ADC PB1. Ninguno es línea del banco |

## Diseño

1. **H1, el hook.** Se copia el de `prj_01` en `prj_04/Common`:
   - `SHARED_GPIO_LOCK_US` (decisión 2) y su conversión por núcleo;
   - `shared_gpio_lock`/`shared_gpio_unlock` con `HSEM_BM_TakeTimeout`/`HSEM_BM_Release`;
   - `g_shared_gpio_lock`, `Shared_GpioLockInstall()`;
   - `m7_/m4_gpio_lock_fail_count` añadidos **al final** de `SharedMemory_t`, reajustando `_pad2` para conservar el múltiplo de 32 B. Ningún campo existente se mueve: las herramientas y el otro núcleo leen por desplazamiento.

   Se instala en CM7 después de `Shared_Init()` (que pone los contadores a cero) y antes de `HSEM_BM_BootReleaseCpu2`, y en CM4 justo después de `HSEM_BM_TimebaseStart()`. Los dos núcleos necesitan su DWT en marcha antes de instalarlo: el hook cuenta ciclos.
2. **H1, la medida (decisión 3).** Por defecto, `unlock` calcula la retención desde el `lock` correspondiente con `DWT->CYCCNT` y guarda el máximo por núcleo en `m7_/m4_gpio_lock_hold_cycles_max`, como R-B1 con el HSEM 4. Es el oráculo de T4: el límite tiene que ser al menos 10 veces la retención más larga medida en **esta** imagen, la regla de `prj_01` (§10, B1).
3. **H2.** `GPIO_Analog_Init()` pasa a devolver `bool` con `GPIO_BM_ConfigPin` (puerto e índice desde `BOARD_ADC_PORT` y `GPIO_BM_PIN_INDEX(BOARD_ADC_PIN)`, analógico, sin pull), sin el bucle a mano que calcula el índice. `GPIO_BM_EnablePortClock` sobra (la llamada habilita el reloj). Un estado distinto de `GPIO_BM_OK` hace fallar `ADC_Acquisition_Init()`.
4. **H3 (decisión 1, opción A).** En `uart_hal.c`, las dos llamadas de la MSP de inicialización pasan a `GPIO_HAL_Init` (mismos campos). Las dos de desinicialización pasan a `GPIO_HAL_DeinitPin(port, GPIO_BM_PIN_INDEX(pin))`, por la misma razón que M1 de `08_eth`: `HAL_GPIO_DeInit` borra EXTI compartidos.

   La MSP de la HAL no devuelve estado. Si `GPIO_HAL_Init` falla, el error se guarda en una variable estática y `UART_Console_Init` falla. Es la decisión 4 de `08_eth` y no cambia ninguna firma; si resulta que hace falta una, para y pregunta.

   Los `GPIO_BM_EnablePortClock` previos sobran. `stm32h7_driver_uart` enlaza `stm32h7_driver_gpio_hal`. El README de `04_uart` dice el cambio en el cierre.
5. **H4.** Las escrituras a mano de los selftests de CM4 pasan a `gpio_bm`. La sonda del puente pasa a `GPIO_BM_ConfigOutput` con nivel inicial bajo y a `GPIO_BM_ConfigInput` con pull-up y luego pull-down. La AF de PD5/PD6 pasa a `GPIO_BM_ConfigAlternateEx` o `GPIO_BM_ConfigAlternate`. Los niveles y retardos de la prueba no cambian.
6. **H5 (decisión 4).** Donde `selftest-gpio` deja `NULL`, se llama `Shared_GpioLockInstall()`.
7. **Sin firmas nuevas.** Ni `gpio_bm.h`/`gpio_ll.h`/`gpio_hal.h` ni `uart_hal.h` cambian salvo comentarios.

## Archivos

| Ruta | Cambio |
| --- | --- |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/Common/Inc/shared_memory.h`, `Common/Src/shared_memory.c` | H1 |
| `prj_04/CM7/Core/Src/main.c`, `prj_04/CM4/Core/Src/main.c` | H1 (instalación); H4 (sonda de `selftest-uart`) |
| `prj_04/CM4/App/Src/adc_acquisition.c` | H2 |
| `prj_04/CM4/App/Src/dma_selftest.c` | H4 |
| `prj_04/CM7/App/Src/gpio_selftest.c`, `prj_04/CM4/App/Src/gpio_selftest.c` | H5 |
| `01_shared_drivers/04_uart/uart_hal.c`, `01_shared_drivers/CMakeLists.txt` (solo `stm32h7_driver_uart`) | H3 (decisión 1) |
| `tools/gpio_f1b_checks.py` (nuevo) | T2, T3, T4 y T6 (abajo) |
| `prj_04/docs/` | Las capturas (`gpio_f1b_<prueba>_<hash>.txt`) |
| `00_gpio/README.md`, `04_uart/README.md`, `prj_04/README.md` | Solo en el cierre |

## Pruebas

**Oráculos:**
- el enlazador (`nm` de las ELF);
- el preprocesador (las escrituras que quedan en cada unidad de compilación);
- los registros leídos por SWD frente a una tabla escrita a mano y frente a las direcciones de `nm`;
- `net_check.py`, `query_adc.py` y los contadores del firmware.

**No se intenta provocar la carrera de G1 en `prj_04`:** su probabilidad por arranque es ínfima. El mecanismo está probado en `prj_01` (T15 `c`/`g`/`h`). Aquí se prueba que **todos** los escritores pasan por él y que el hook está instalado y funciona.

| ID | Qué | Oráculo | Control negativo | Criterio |
| --- | --- | --- | --- | --- |
| T1 | Builds: `build_all -Clean` 8/8; los siete presets de `prj_04`; `prj_04` debug y release, y `prj_01`/`prj_02`/`prj_03` debug, con `-Werror` | Exit y avisos | — | Todo compila, 0 avisos; `.text` de `uart_hal.c.obj` y de `adc_acquisition.c.obj` antes y después, registrado |
| T2a | Ningún `HAL_GPIO_Init`/`HAL_GPIO_DeInit` enlazado en producción: `nm` de las ELF CM7 y CM4 de `prj_04` debug y release, y CM7 de `prj_01`/`prj_02`/`prj_03` debug | Los símbolos no están definidos en la ELF | Las mismas ELF compiladas en `07db657`: `HAL_GPIO_Init` está en todas las CM7 (la MSP de la consola) | Ausentes en todas; el control los encuentra |
| T2b | Ninguna escritura a mano de `MODER`, `OTYPER`, `OSPEEDR`, `PUPDR` o `AFR` fuera del driver. Cada unidad de `compile_commands.json` de `prj_04` en los presets debug, release, `selftest` (DMA), `selftest-adc`, `selftest-uart` y `selftest-hsem` se preprocesa (`-E`) y se busca la escritura. Lista de excepciones cerrada y explícita: `gpio_bm.c`, `gpio_ll.c`, `stm32h7xx_hal_gpio.c`, `led_bm.c` (antes de liberar a CPU2; G14) y `gpio_selftest.c` (el instrumento de F1a) | Las escrituras que quedan, por archivo y línea | `07db657`: encuentra `adc_acquisition.c` (debug), y `main.c` de CM4 y `dma_selftest.c` en sus presets | Cero fuera de las excepciones; el control encuentra las tres |
| T3 | Con la imagen debug corriendo, por SWD y sin parar:<br>• `s_shared_lock` de `gpio_bm.c` en cada núcleo (direcciones de `nm` de su ELF);<br>• `HSEM_RLR5`;<br>• `m7_/m4_gpio_lock_fail_count`;<br>• la configuración de PB1, PB13, PD8 y PD9 | Cada `s_shared_lock` es la dirección de `g_shared_gpio_lock` de su ELF; `RLR5 = 0` (libre); contadores a 0; los pines frente a una tabla escrita a mano (PB1 analógico sin pull; PB13 AF11, PP, muy alta, sin pull; PD8/PD9 AF7, PP, baja, pull-up) | La imagen de `07db657` flasheada y leída por la misma herramienta: los dos `s_shared_lock` valen 0 y T3 **falla**. Más un pin no usado de cada puerto, en su valor de reset | Todo exacto; el control falla |
| T4 | Margen del límite (decisión 3): `m7_/m4_gpio_lock_hold_cycles_max` por SWD tras el arranque | Cada máximo es distinto de 0 (los dos núcleos tomaron el hook al menos una vez) y `límite en ciclos / máximo ≥ 10` | El mismo cálculo con un límite diez veces menor tiene que fallar | Los dos núcleos ≥ 10× |
| T5 | Regresión de producción, debug y release: `monitor.py --sn`, `net_check.py` y `query_adc.py --json` a la vez | Banner con `Rev` = el commit; ICMP 4/4, UDP de 1 a 1472 B idénticos, ráfaga 100/100; `rx_nobuf`, `tx_nobuf`, `dma_err` y `acl_drop` a 0; `sample_count` crece entre dos consultas; `dma_error_count`, `half_overrun_count` y `adc_overrun_count` a 0 | — | Todo como se espera, en las dos imágenes |
| T6a | Arranques repetidos: 20 resets por SWD (`-rst`) con la imagen debug y, tras cada uno, a los 10 s, T3 completa | T3 en cada arranque | — | 20/20 T3 PASS |
| T6b | Un arranque en frío (el responsable apaga y enciende la placa) con T3 y T5. **Pendiente mientras el responsable esté en remoto (G-S7):** no bloquea la entrega de F1; sí el cierre. Lo ejecuta quien esté con el responsable en el banco (implementador o auditor) | T3 y T5 | — | T3 y T5 PASS tras el arranque en frío |
| T7 | La consola de `prj_01` con `uart_hal` cambiado (decisión 1, A): `prj_01` debug flasheado, banner y build id leídos por `monitor.py --sn`; PD8/PD9 leídos por SWD | Banner con el build id del commit; PD8/PD9 como en T3 | — | Banner correcto; pines exactos |

**Capturas:** una ejecución sin editar por prueba (T2–T7) y por herramienta, con el hash del árbol limpio, en `prj_04/docs/gpio_f1b_<prueba>_<hash>.txt`. T7 va en el mismo directorio. Las de placa, con la imagen del mismo commit.

## Banco

El de siempre, sin cambiar un cable. Incluye las tomas J2/J3 y la F334 de `05_i2c`, y el cable Ethernet a la red del responsable (`prj_04` usa `192.168.1.150` estática, como `prj_02`).

| # | Regla |
| --- | --- |
| G-S1 | **Solo se flashean** `prj_04` debug y release (y la de control de `07db657`) y `prj_01` debug. Ningún preset de selftest de `prj_04`: `selftest-gpio` mueve líneas del AD2 y PF14/PF15, y el resto no hace falta. Antes del primer flasheo, la lista de pines de §"Lo que existe hoy" se comprueba contra el código y se pone en el informe |
| G-S2 | **Dos ST-LINK:** `sn=0016000C4142501220353451` en cada llamada al programador y `monitor.py --sn` |
| G-S3 | **El AD2, en estado seguro** (sin script) |
| G-S4 | **La red la configura el responsable** |
| G-S5 | Flashear o resetear solo con `monitor.py`, `net_check.py` y `query_adc.py` parados |
| G-S6 | Todas las imágenes que se flashean son del repo, así que no hace falta copia previa de la flash; el informe dice qué imagen quedó en la placa al terminar |
| G-S7 | **Placa en remoto: nada que solo se recupere a mano.** Sin acceso físico, la única recuperación es el NRST del ST-LINK (`mode=UR`, `-hardRst`). Por eso: no se cambia la configuración de alimentación (`HAL_PWREx_ConfigSupply` y `SystemClock_Config` quedan como en `07db657`: una configuración equivocada bloquea la placa hasta recuperarla con BOOT0), ni los option bytes (`flash.py --ob`, BCM4/BCM7, direcciones de arranque, RDP), ni el firmware del ST-LINK, y no se borra la flash entera. Si la placa deja de responder por SWD incluso bajo reset, o la red deja de contestar, se para y se avisa: no se insiste. La confirmación física del banco no se puede pedir; la sustituyen G-S1 (ningún pin de `prj_04` toca una línea del banco, esté como esté) y una comprobación de red antes de la primera sesión (`net_check.py` con la imagen que haya) |

## Fases

| Fase | Entregable | Gate |
| --- | --- | --- |
| **F1** | H1–H5; `tools/gpio_f1b_checks.py`; T1–T5, T6a y T7 con sus capturas; el informe | Auditoría con revisión, builds y la ejecución propia del auditor (T3–T5, T6a) |
| **Cierre** | Implementador: `00_gpio/README.md` (§11 consumidores, también la fila de `08_eth`, que ya usa `GPIO_HAL_Init`; §12.2 G1 y G13; §13.1; §15), `04_uart/README.md` (la MSP), `prj_04/README.md` (el hook y la validación). Responsable: `DRIVER_ROADMAP.md`, borrado de los documentos de proceso, merge | T6b hecha con el responsable en el banco; auditoría de cierre |

## Riesgos

| Riesgo | Mitigación |
| --- | --- |
| Añadir campos a `SharedMemory_t` mueve algo que otro lee por desplazamiento | Solo al final y ajustando `_pad2`; T5 (`query_adc.py` y la consola) lo comprueba |
| `GPIO_HAL_Init` en la consola falla antes de que haya consola para decirlo | El error se guarda y `UART_Console_Init` falla; el LED de fallo del proyecto lo muestra. T7 y T5 cubren el camino bueno |
| La retención en `prj_04` es mayor que en `prj_01` y 60 µs no da 10× | T4 lo mide en esta imagen; si no llega, se sube el límite (nunca se baja la puerta), con su coste en tiempo enmascarado (§10, A2) |
| Interrupciones enmascaradas hasta el límite más una llamada | Cada configuración ocurre al arrancar: la de CM4 antes de arrancar el DMA del ADC; las de CM7 antes del planificador (consola) o en `net_task` antes de arrancar el MAC |
| Un núcleo parado por el depurador dentro de la sección deja el HSEM 5 tomado (§10, A2) | T3 lee `RLR5` sin parar los núcleos; no se usa el depurador durante las capturas |
| `s_shared_lock` de CM7 en DTCM no se puede leer por el AP por defecto | El auditor leyó la DTCM (`0x20000000`) y la SRAM2 (`0x30020000`) con `mode=HOTPLUG` y los núcleos corriendo el 2026-10-12. Si con `prj_04` no se puede, para y pregunta: no se añade un getter a `gpio_bm.h` sin aprobación |

## Decisiones abiertas

Cada una con su opción por defecto.

1. **La MSP de la consola (H3):**
   - **A (por defecto):** cambiar `04_uart/uart_hal.c` (`GPIO_HAL_Init`/`GPIO_HAL_DeinitPin`). Cierra G13 para todos los consumidores, como `08_eth` E8.
   - **B:** un `UART_HAL_MspInit_User` fuerte solo en `prj_04`.
   - **C:** no tocarla, porque CPU2 no escribe `GPIOD` en producción. Sí lo hace en `selftest-uart` y en el de DMA.
2. **El límite del hook:** **60 µs**, como `prj_01` (`00_gpio/README.md` §3.1), con la puerta de 10× medida en esta imagen (T4).
3. **Medir la retención en producción:** **sí**, dos campos más en el bloque compartido y unas instrucciones en `unlock`, como R-B1 con el HSEM 4. Sin la medida, T4 no existe y el margen de `prj_04` se supone.
4. **`selftest-gpio` al terminar:** **restaurar el hook de producción** en lugar de `NULL`.
5. **G1 en `prj_03`:** **fuera**, en una fase F1c después del cierre, con la misma herramienta. Alternativa: incluirlo aquí, lo que añade el hook, `adc_acquisition.c` de `prj_03` y T3/T5 con su imagen.
6. **Documentos de proceso al cierre:** se borran, como en `05_i2c` y `08_eth`.
