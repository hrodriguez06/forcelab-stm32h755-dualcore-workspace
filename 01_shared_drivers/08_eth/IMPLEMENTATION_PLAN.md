# Plan de cierre de limitaciones de `08_eth` (E1, E2, E5, E8)

Oct 11, 2026 · @Hector · revisión 1

Este plan cierra cuatro de las limitaciones de `08_eth/README.md` §10, la opción B que eligió el responsable del catálogo el 2026-10-11: E1 (pines de reserva), E2 (callbacks registrables no comprobados), E5 (contadores no atómicos) y E8 (carrera de GPIO entre núcleos). No es un driver nuevo ni una reescritura: `08_eth` sigue siendo un tier HAL único, con su API congelada, y la validación de hardware es la de su README §9.3, a través de `prj_02_eth_lwip_dualcore`.

Una fase de implementación (F1) y un cierre. La fase pasa una auditoría independiente con ejecución propia del auditor en la placa. Las reglas del implementador están en [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md).

Base: `7154af6` de `main` (cierre de `05_i2c`), árbol limpio, rama `driver/eth`.

**Decisiones del responsable ya tomadas (2026-10-11):** opción B (E1, E2, E5, E8); nadie más trabaja en Ethernet; el responsable tiene un puerto Ethernet en el PC para la red de prueba.

## Encargo

| ID | Limitación (`08_eth/README.md` §10) | Qué cambia |
| --- | --- | --- |
| E1 | `eth_hal.c` incluye `board_def.h` solo si existe (`__has_include`) y define pines de la Nucleo-144 para cualquier `BOARD_ETH_*` que falte | `board_def.h` obligatorio y un `#error` por cada una de las 18 macros `BOARD_ETH_<SIG>_PORT`/`_PIN` que falte, como `04_uart/uart_hal.c` con `BOARD_CONSOLE_*`. Sin pines por defecto |
| E2 | `USE_HAL_ETH_REGISTER_CALLBACKS == 0` se supone, no se comprueba | `#error` si está definida y es distinta de 0 |
| E5 | `g_stats` se incrementa desde tarea e ISR sin protección | Cada incremento, atómico (decisión 3) |
| E8 | Carrera entre núcleos en los registros de configuración de los puertos A, B, C y G | Los nueve pines RMII se configuran con `GPIO_HAL_Init` de `00_gpio` (la sección de `gpio_bm`: PRIMASK y el lock compartido que instale el proyecto) en lugar de `HAL_GPIO_Init`, como prescribe `00_gpio/README.md` (G13) |

**Fuera de este plan:** E3 (callbacks no desactivables), E4 (una instancia, estado global: es el silicio), E6 (camino con copia), E7 (la copia privada de `prj_eth_lwip_node`: no se porta, decisión 2), lo no ejercitado de §9.4 (10 Mbps, read-back de erratas, desenchufar el cable…), los demás escritores de GPIO fuera de `gpio_bm` (`04_uart/uart_hal.c`, el ajuste a mano de `prj_04`: G1/G13 de `00_gpio`) y la instalación del lock compartido en los proyectos (ver E8 abajo).

## Fuentes normativas

- `01_shared_drivers/README.md` (regla 1: nada de hardware en los drivers compartidos; regla 2: firmas congeladas).
- `08_eth/README.md` completo, sobre todo §3 (contrato del proyecto), §8 (reglas de doble núcleo), §9 (validación), §10 y §11 (cómo cambiarlo).
- `00_gpio/README.md` §3.3 y §4 (la sección, el lock compartido, G1 y G13) y `gpio_hal.h` (`GPIO_HAL_Init`).
- HAL V1.11.6 (`stm32h7xx_hal_eth.c`, `stm32h7xx_hal_conf.h` de cada proyecto), RM0399 (GPIO, SYSCFG), ES0445 §2.25 (las erratas de ETH ya aplicadas, que no cambian).

## Lo que existe hoy (`7154af6`)

| Elemento | Estado |
| --- | --- |
| `eth_hal.c` | 821 líneas. Pines: tabla `s_rmii_pins` desde `BOARD_ETH_*` con reserva de la Nucleo (l. 28-84), configurados con `GPIO_BM_EnablePortClock` + `HAL_GPIO_Init` (l. 245-260). Contadores: `g_stats` con `++` en l. 580, 633, 668, 732, 794 (tarea e ISR) |
| `eth_hal.h` | API congelada (README §11, regla 1); `eth_driver_stats_t` de cinco `uint32_t` |
| Consumidores | `prj_02` y `prj_03` por el paraguas, `prj_04` por `stm32h7_driver_eth`. Los tres definen las 18 macros `BOARD_ETH_*` en `Common/Inc/board_def.h` y `USE_HAL_ETH_REGISTER_CALLBACKS 0U` en el `stm32h7xx_hal_conf.h` de CM7. `prj_01` enlaza el paraguas sin `HAL_ETH_MODULE_ENABLED`: `eth_hal.c` compila a nada. `prj_eth_lwip_node` usa su copia privada (E7) |
| Lock compartido de GPIO | Ningún proyecto de producción lo instala (`GPIO_BM_SetSharedLock` solo aparece en selftests de `prj_04` y en `prj_01`). Sin él, la sección de `gpio_bm` es PRIMASK del núcleo propio |
| Validación | `08_eth/README.md` §9.1 (2026-09-20, `prj_02` en `68ec30a`): ICMP, UDP 1..1472 B, ráfaga de 100, contadores a 0 |

## Diseño

1. **E1.** `#include "board_def.h"` sin `__has_include`; dentro de `#if defined(HAL_ETH_MODULE_ENABLED)` (para que `prj_01` y cualquier proyecto sin Ethernet sigan compilando), un `#error` por cada `BOARD_ETH_<SIG>_PORT` y `_PIN` que falte, con el nombre de la macro y la regla 1 en el texto, como `uart_hal.c`. La tabla `s_rmii_pins` y su orden no cambian. Se borra el comentario de "fallback".
2. **E2.** `#if defined(USE_HAL_ETH_REGISTER_CALLBACKS) && (USE_HAL_ETH_REGISTER_CALLBACKS != 0) #error …`, también dentro de la guarda del módulo. Si la macro no está definida, la HAL la toma como 0: no es error.
3. **E5.** Cada `g_stats.x++` pasa a una operación atómica de lectura-modificación-escritura (decisión 3). `eth_driver_stats_t` y `ETH_Driver_GetStats()` no cambian: leer un campo de 32 bits alineado ya es atómico; el informe dice que una lectura de la estructura entera no es una instantánea coherente.
4. **E8.** Cada pin RMII con `GPIO_HAL_Init(port, &gpio)` (mismos campos: AF push-pull, sin pull, velocidad muy alta, AF11), comprobando su `HAL_StatusTypeDef`. Si un pin falla (`HAL_BUSY`: no se obtuvo el lock compartido; `HAL_ERROR`), la MSP no puede devolver error: el driver lo registra (decisión 4) y `ETH_Driver_Init` devuelve `ETH_DRIVER_ERROR` en lugar de seguir con un pin sin configurar. `stm32h7_driver_eth` enlaza `stm32h7_driver_gpio_hal` (y deja de depender de `HAL_GPIO_Init`). **Lo que esto cierra:** el driver ya no escribe ningún registro de GPIO fuera de la sección de `gpio_bm`. **Lo que no cierra:** la protección entre núcleos solo existe si el proyecto instala el lock compartido en los dos núcleos y el otro núcleo también configura sus pines por `gpio_bm`; ningún proyecto de producción lo hace hoy (fuera de alcance). El README lo dice en §3 (contrato) y §8, y E8 queda como limitación del proyecto, no del driver.
5. **Sin cambios de firma.** `eth_hal.h` y `lan8742_phy.h` no cambian salvo comentarios. Si alguna corrección parece pedir una función nueva, para y pregunta.

## Archivos y dependencias

| Ruta | Cambio |
| --- | --- |
| `01_shared_drivers/08_eth/eth_hal.c` | E1, E2, E5, E8 |
| `01_shared_drivers/08_eth/eth_hal.h` | Solo comentarios (el contrato de `board_def.h`, los contadores) |
| `01_shared_drivers/CMakeLists.txt` | `stm32h7_driver_eth` enlaza `stm32h7_driver_gpio_hal` |
| `tools/eth_build_checks.py` (nuevo) | Las comprobaciones de compilación T2–T4 (abajo), sin hardware |
| `01_shared_drivers/08_eth/docs/` (nuevo) | Las capturas de esta fase (`eth_<fase>_<prueba>_<hash>.txt`) |
| `01_shared_drivers/08_eth/README.md` | Solo en el cierre |
| `02_firmware_projects/prj_02_eth_lwip_dualcore/README.md` §6 | Solo en el cierre, y solo si el responsable lo aprueba (decisión 1) |

Nada más de `02_firmware_projects` ni de `03_firmware_templates` cambia: ni `board_def.h`, ni `stm32h7xx_hal_conf.h`, ni el código de los proyectos.

## Pruebas

Ninguna prueba nueva corre en la placa salvo T5 y T6. **Oráculos:** el compilador (los `#error` y su texto), el desensamblado, los registros de GPIO leídos por SWD frente a una tabla escrita a mano, `net_check.py` y los contadores del firmware.

| ID | Qué | Oráculo | Control negativo | Criterio |
| --- | --- | --- | --- | --- |
| T1 | Builds: `build_all -Clean` 8/8; `prj_02`, `prj_03`, `prj_04` debug y release con `-Werror`; `prj_01` con los tamaños de `2481396` | Exit y avisos; `.map` | — | 8/8, 0 avisos; `prj_01` sin cambios; el `.text` de `eth_hal.c.obj` en `prj_02/03/04` antes y después, registrado |
| T2 | E1: `eth_hal.c` compilado con las flags de `prj_02` CM7 contra 19 variantes de `board_def.h`: sin el archivo, y sin cada una de las 18 macros | El `#error` y su texto nombran la macro que falta | El `board_def.h` completo compila sin avisos; `prj_01` (sin el módulo) compila | 19 de 19 fallan con el mensaje esperado; el control compila |
| T3 | E2: con `USE_HAL_ETH_REGISTER_CALLBACKS 1U` (una copia del `hal_conf` de `prj_02` delante en la ruta de includes) | El `#error` | Con `0U` compila; sin definir compila | Falla con el mensaje esperado; los dos controles compilan |
| T4 | E5 y E8 en el objeto: desensamblado de `eth_hal.c.obj` de `prj_02` CM7 | Los cinco incrementos son lectura-modificación-escritura atómicas (p. ej. bucles `LDREX`/`STREX`); `arm-none-eabi-nm -u` no lista `HAL_GPIO_Init` y sí `GPIO_HAL_Init` | El objeto de `7154af6`: los incrementos son `LDR`/`ADD`/`STR` y `HAL_GPIO_Init` aparece | Todo como se espera, y el control lo distingue |
| T5 | E8 en la placa: con `prj_02` corriendo, leer por SWD `MODER`, `OTYPER`, `OSPEEDR`, `PUPDR` y `AFRL/AFRH` de los puertos A, B, C y G | Los nueve pines en AF, push-pull, velocidad muy alta, sin pull, AF11, frente a una tabla escrita a mano | Un pin de esos puertos que el proyecto no usa, en su valor de reset | Los nueve exactos |
| T6 | Camino de datos (README §9.3): `prj_02` en la placa, `net_check.py` (ICMP, UDP de 1 a 1472 B, ráfaga de 100) y `monitor.py` | Respuestas byte a byte y la ráfaga de `net_check.py`; los contadores del firmware | — | ICMP sin pérdida; UDP idéntico en todos los tamaños; ráfaga 100/100; `rx_nobuf`, `tx_nobuf` y `dma_err` a 0 antes y después |

**Capturas:** una ejecución sin editar por prueba con hardware (T5, T6) y una por herramienta (T2–T4), con el hash del árbol limpio, en `08_eth/docs/`. T5 y T6 con la imagen de `prj_02` del mismo commit.

## Banco

La placa del catálogo, sin cambiar un cable, **más un cable Ethernet** entre el RJ45 de la Nucleo y el puerto del PC del responsable, que configura la red (`prj_02` usa `192.168.1.150` estática en `192.168.1.0/24`; README §9.1).

| # | Regla |
| --- | --- |
| E-S1 | **Antes del primer flasheo de `prj_02`,** listar cada pin que configuran sus dos núcleos (de su código y su `board_def.h`) y comprobar que ninguno es una línea del banco: las del AD2 (PE2–PE15, PA8), J1 (PA5, PA6, PB4, PB5), el bus I2C (PB8, PB9) y las tomas (PF14, PF15). Si alguno coincide, para y pregunta |
| E-S2 | **Dos ST-LINK conectados:** `sn=` en cada llamada al programador y `monitor.py --sn` (S18 de `05_i2c`). O la F334 desconectada |
| E-S3 | **El AD2, en estado seguro** (sin script): `prj_02` no lo usa |
| E-S4 | **La red la configura el responsable.** El implementador no cambia la configuración de red del PC |
| E-S5 | Flashear o resetear solo con `net_check.py` y `monitor.py` parados |

## Fases

| Fase | Entregable | Gate |
| --- | --- | --- |
| **F1** | E1, E2, E5, E8 en `eth_hal.c` (y el CMake); `tools/eth_build_checks.py`; T1–T6 con sus capturas; el informe | Auditoría con revisión, builds y la ejecución propia del auditor (T5, T6) |
| **Cierre** | Implementador: `08_eth/README.md` (§3, §8, §9, §10, §12). Responsable: `prj_02/README.md` §6 si lo aprueba (decisión 1), `DRIVER_ROADMAP.md`, borrado de los documentos de proceso, merge | Auditoría de cierre |

## Riesgos

| Riesgo | Mitigación |
| --- | --- |
| Un proyecto que hoy compila deja de hacerlo (E1) | Los tres consumidores definen las 18 macros (comprobado); T1 |
| `GPIO_HAL_Init` difiere de `HAL_GPIO_Init` en algún campo para AF | T5 compara los registros con la tabla escrita a mano; T6 prueba el enlace |
| Un fallo de `GPIO_HAL_Init` en la MSP | La MSP no devuelve estado: se registra y `ETH_Driver_Init` falla en lugar de seguir (decisión 4) |
| El lock compartido no está instalado en ningún proyecto, así que E8 no protege de verdad entre núcleos | Dicho en el README como contrato del proyecto; fuera de alcance |
| `prj_eth_lwip_node` se queda con la copia vieja (E7) | Decisión explícita (decisión 2), registrada en el README |

## Decisiones abiertas

Cada una con su opción por defecto.

1. **El registro en `prj_02/README.md` §6:** el README de `08_eth` (§11, paso 7) pide actualizarlo con el hash de la imagen. Por defecto: **sí, solo esa sección y solo en el cierre**, como excepción explícita a "nada en `02_firmware_projects`".
2. **E7:** **no se porta** a `prj_eth_lwip_node`; su README dice que es una decisión explícita y se registra.
3. **E5, mecanismo:** **`__atomic_fetch_add(&g_stats.campo, 1U, __ATOMIC_RELAXED)`** (LDREX/STREX en el M7). Alternativa: una sección con PRIMASK alrededor de cada incremento.
4. **E8, fallo de un pin:** la MSP guarda el primer estado distinto de `HAL_OK` en una variable estática del archivo, y `ETH_Driver_Init` la consulta tras `HAL_ETH_Init()` y devuelve `ETH_DRIVER_ERROR`. Sin firma nueva.
5. **Documentos de proceso al cierre:** se borran, como en `05_i2c`.
