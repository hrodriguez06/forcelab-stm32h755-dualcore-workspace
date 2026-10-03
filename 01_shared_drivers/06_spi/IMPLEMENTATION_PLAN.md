# Plan del driver SPI 06\_spi

Oct 3, 2026 · @Hector · revisión 1

Este plan cubre el paso 4 de `DRIVER_ROADMAP.md`: el driver SPI `06_spi` en sus tres tiers, `spi_bm`, `spi_ll` y `spi_hal`. Tiene una fase F0 de preparación del banco, tres fases de implementación (F1 BM, F2 LL, F3 HAL) y una de cierre.

Cada fase pasa una auditoría independiente antes de empezar la siguiente, con las reglas de [Protocolo de auditoría](#protocolo-de-auditoría). Las reglas del implementador están en [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md).

Base: `8901b77` de `main` (cierre de `03_tim`), árbol limpio, rama `driver/spi`.

**Estado al entregar este plan (2026-10-03):**

- El responsable del catálogo aprobó las decisiones abiertas 1–8 con su opción por defecto.
- **J1 está colocado y verificado**, sin flashear nada. Con los dos núcleos detenidos por SWD y los cuatro pines en entrada, sin ningún pin en salida, se alternó el pull de un extremo del puente y se leyó el otro.
  - El pull de PB5 fija PA6, y el de PA6 fija PB5, a 1 y a 0.
  - PA5 y PB4, con el pull contrario, siguen el suyo.
  - Los registros se restauraron (read-back igual al inicial).
- Que un pull interno lleve la línea a los dos niveles prueba que ningún otro conductor (PA7 por SB56, el PHY) está en la línea. Por eso la comprobación visual de SB52, SB56 y SB57 que pide F0 queda cubierta para J1.
- T0c sigue siendo obligatoria en cada sesión.

## Encargo

Texto del responsable del catálogo, 2026-10-03 (el mismo encargo de `03_tim`, aplicado al paso siguiente de la hoja de ruta):

> Desarrolla el driver bajo el estándar de driver de catálogo, asegurando que incluya implementaciones: BM, HAL y LL.
>
> Como restricción de alcance, no intentes validar ni corregir el resto de los proyectos del repositorio; enfoca la validación exclusivamente en prj_01_template_dualcore, aprovechando las conexiones ya establecidas con el AD2 (Analog Discovery 2).
>
> Al finalizar, genera un README.md limpio y conciso con la información técnica trascendental (diagrama/pines con AD2, guía de uso y configuración), manteniendo el mismo formato y calidad que los drivers ya terminados en el proyecto.

## Fuentes normativas

Por orden de precedencia:

1. Este encargo.
2. `01_shared_drivers/README.md`, reglas 1–4.
3. `DRIVER_ROADMAP.md` §5.4 (alcance de la primera iteración) y §6 (ciclo de trabajo, reglas R1–R4), con las desviaciones de la tabla siguiente.
4. `00_docs_stm32h7/DRIVER_ARCHITECTURE_GUIDELINES.md` y `00_docs_stm32h7/CLAUDE.md`.
5. `00_gpio/README.md` §8 (banco del AD2, reglas S1–S10) y `03_tim/README.md` §9 (S11, S12, protocolo del banco), que este plan hereda sin cambios salvo lo que dicen S13 y S14.
6. Silicio y herramientas:
   - RM0399 rev 4: cap. 53 (SPI; `00_docs_stm32h7/docs/peripherals/spi/53_serial-peripheral-interface-spi.txt`), cap. 9 (RCC: §9.5.10, §9.7.19, §9.7.21, §9.7.44–47, tabla 62), cap. 17 (BDMA) y cap. 18 (DMAMUX, tablas 126 y 130).
   - ES0445 rev 6, §2.22.
   - DS12923, tabla 107 (fSCK máxima por instancia).
   - UM2408 y el esquema MB1363 (`00_docs_stm32h7/docs/02_board_hardware/`).
   - WaveForms SDK Reference Manual (revisado el 4 de marzo de 2026): §8 Digital I/O y Digital In.
   - HAL del Cube `8ec3113` (STM32H7xx HAL V1.11.6).

## Desviaciones respecto a la hoja de ruta

| # | Hoja de ruta §5.4 | Este plan | Por qué |
| --- | --- | --- | --- |
| D1 | "Un puente MOSI–MISO en el conector" | **Puente J1: CN7-14 (D11, PB5) ↔ CN7-12 (D12, PA6)**, con SCK en CN7-10 (D13, PA5). Sirve a SPI1 (AF5, CM4) y a SPI6 (AF8, CM7). Lo coloca el responsable del catálogo antes de la ejecución de F0, y T0c comprueba que está al empezar cada sesión | Ninguno de los cuatro pines está cableado al AD2 ni lo usa otro banco (`00_gpio` usa PF3–PF11, GPIOJ/K; `03_tim`, PA8 y PF6–PF9). D11 es PB5 por defecto (SB57 ON, SB56 DNF; UM2408 tabla 17). **PA7 no:** SB52 lo une a RMII\_CRS\_DV, una salida del PHY. PG13 (SPI6\_SCK AF5) es RMII\_TXD0 |
| D2 | "Control negativo sin puente" | MISO pasa a **PB4** (CN7-19, sin cablear; SPI1\_MISO AF5, SPI6\_MISO AF8) con pull-up y luego pull-down, sin quitar el cable | El mismo efecto que quitar el puente, sin tocar el banco a mitad de una ejecución: la captura sigue siendo una sola ejecución sin intervención |
| D3 | "Captura de SCK/CS con el AD2" | La captura la hace el AD2 sobre **SPI4 en PE11 (NSS), PE12 (SCK), PE13 (MISO) y PE14 (MOSI), AF5**, que ya están cableados a DIO3–DIO6. SPI1 y SPI6 no se observan con el AD2 | Es la única instancia con sus cuatro señales en líneas del AD2 sin recablear. El juego PE2/PE4/PE5/PE6 de SPI4 cae en HOST\_REQ y en líneas "solo AD2" (S4) |
| D4 | Erratas 2.22.4 y 2.22.5 "entran en la iteración de CRC" | Entran en esta: el driver las aplica | 2.22.4 afecta con `CRCEN = 0` y `TSIZE > 0` (el caso de este driver) y 2.22.5 a todo fin de transferencia con `TSIZE > 0`. Solo 2.22.2 (esclavo con `UDRDET`) queda fuera |
| D5 | "Barrido de velocidad hasta el límite del pin" | Pasa obligatoriamente hasta **25 MHz**. Por encima (SPI1 a 50 y 100 MHz, SPI6 a 50 MHz) se registra el resultado, sin criterio | DS12923 tabla 107 da 100/80 MHz (SPI1–3) y 50 MHz (SPI4–6) con compensación de I/O activa, `OSPEED = 11` y 30 pF. `prj_01` no activa la celda de compensación, y el puente es un cable. Con el HSE de la placa (+1,3 %, N1), SPI6 a /2 va a unos 50,6 MHz, por encima de la tabla |
| D6 | — | El banco **no usa Digital Out ni TREF** | Ninguna prueba necesita que el AD2 conduzca un dato, y ninguna medida depende del reloj absoluto del AD2: las frecuencias se juzgan como cociente con MCO1 en la misma adquisición, y los tiempos, en periodos de SCK de la misma captura |
| D7 | "DMA en DMA1/2 y en BDMA (SPI6)" | BDMA se programa en la prueba con `dma_hal`, el único tier de `02_dma` que cubre BDMA y DMAMUX2 (`02_dma/README.md` §2.3). No se crea `bdma_bm` | `02_dma` está cerrado y su §11 difiere `bdma_bm`. El driver SPI no incluye ningún `dma_*.h` (C8): solo da el número de petición y el orden de `TXDMAEN`/`RXDMAEN` |
| D8 | — | No hay migración de proyectos | Ningún proyecto del repositorio usa SPI (`git grep` de `SPI1`–`SPI6`, `hspi`, `LL_SPI`, `HAL_SPI_` en `02_firmware_projects` y `03_firmware_templates`, fuera de `external/`) |

## Criterios de catálogo

Estos 15 criterios se auditan en cada fase. La columna Origen dice de qué fuente sale cada uno.

| # | Criterio | Origen |
| --- | --- | --- |
| C1 | **Sin pines ni instancias del proyecto.** El driver no configura GPIO, no incluye `board_def.h`, `shared_memory.h`, `gpio_*.h`, `dma_*.h`, `tim_*.h` ni `hsem_*.h`, y no contiene frecuencias de reloj, prioridades de NVIC, streams de DMA ni IDs de HSEM. En él solo viven hechos del silicio, en una tabla de capacidades por instancia: dirección (macros `SPIx` de CMSIS), bus y bit de enable en RCC, ancho máximo de trama, tamaño de FIFO, campo del multiplexor de reloj de kernel, IRQn y peticiones de DMAMUX1/DMAMUX2 | README regla 1 |
| C2 | **API congelada al pasar su auditoría.** La de `spi_bm` al pasar F1, la de `spi_ll` al pasar F2 y la de `spi_hal` al pasar F3. Después, solo se añade | README regla 2 |
| C3 | Cada archivo lleva su sufijo de tier, los nombres son únicos y ningún `#include` lleva el prefijo numérico (`#include "spi_bm.h"`) | README reglas 3 y 4 |
| C4 | **Targets.** `stm32h7_driver_spi_bm` compila `spi_bm.c`. `stm32h7_driver_spi_ll` compila `spi_ll.c` y `stm32h7_driver_spi_hal` compila `spi_hal.c`, y los dos enlazan `_spi_bm`. No hay target sin sufijo, y el paraguas `stm32h7_shared_drivers` no los enlaza; ya incluye el directorio `06_spi` | Precedente de `03_tim` y `10_hsem` |
| C5 | **Erratas.** Si una errata se puede aplicar en el driver, la aplica el driver. Si no, va al README con su ID (ver [Matriz RM0399 / ES0445](#matriz-rm0399--es0445)) | CLAUDE.md §F |
| C6 | **Validar antes de escribir.** Todo parámetro inválido, reservado o no soportado por la instancia devuelve error **antes del primer acceso**, también a RCC. Orden fijo de los rechazos: `NULL`, instancia, capacidad de la instancia, rango de campos y estado (`ERR_BUSY`) | C6 de `00_gpio`, `10_hsem` y `03_tim` |
| C7 | **Sin estado en RAM.** `.data = 0` y `.bss = 0` en `spi_bm.c.obj`, `spi_ll.c.obj` y `spi_hal.c.obj`, medido en el `.map`. El estado de una transferencia por interrupción vive en una estructura del llamador (`SpiBm_Xfer_t`); el `SPI_HandleTypeDef`, también | C7 de `03_tim` |
| C8 | **Dependencias hacia abajo.** `spi_bm` usa solo `stm32h7xx.h`, `<stdint.h>` y `<stdbool.h>`. `spi_ll` añade solo funciones `static inline` de `stm32h7xx_ll_spi.h` y sus macros `LL_SPI_WriteReg`/`LL_SPI_ReadReg`: sin `USE_FULL_LL_DRIVER` y sin `LL_SPI_Init`. `spi_hal` añade `stm32h7xx_hal.h`, no compila nada sin `HAL_SPI_MODULE_ENABLED` y, con él, exige `USE_SPI_CRC == 0` (`#error`; ver regla 10). Ningún tier usa FreeRTOS | `03_tim` C8; ES0445 2.22.4 |
| C9 | **Evidencia.** `tools/build_all.ps1 -Clean` da 8/8. `prj_01` no lleva `-Werror` en sus flags: sus imágenes, también `selftest-spi`, compilan con 0 avisos, y una compilación aparte con `CMAKE_C_FLAGS=-Werror` sale con 0. Cada captura lleva `Rev` e id de build limpios, y los tamaños salen del `.map` enlazado | Hoja de ruta §6.3, R2 y R3 |
| C10 | **Propiedad y concurrencia.** Cada instancia tiene un único núcleo dueño, y el driver no toma locks entre núcleos. En RCC solo escribe el banco del núcleo que llama (`RCC->APB1LENR` para SPI2/3, `RCC->APB2ENR` para SPI1/4/5, `RCC->APB4ENR` para SPI6; RM0399 §9.7.1). Nunca escribe `RCC_APBxRSTR`, `RCC_D2CCIP1R` ni `RCC_D3CCIPR`: los dos últimos solo los lee. Dentro de un núcleo: los flags se borran con una sola escritura de `IFCR`. Los RMW de `CR1`, `CFG1` e `IER` van con PRIMASK guardado y restaurado con `__set_PRIMASK(saved)`, sin `__enable_irq()` | RM0399 §9.5.10, §9.7.1 |
| C11 | **Pines sin glitch.** `CFG1`/`CFG2` se escriben solo con `SPE = 0`. Con `keep_pins = false` (`AFCNTR = 0`, por defecto), el SPI suelta SCK, MOSI y SS mientras `SPE = 0`, y su nivel lo fija el pull del llamador (RM0399 53:722). Con `keep_pins = true`, `CFG2` se escribe primero con `AFCNTR = 0` y la polaridad final, y después `AFCNTR = 1`, para que el pin salte directamente al nivel de reposo; un `Init` con `AFCNTR = 1` vigente y otro `CPOL` → `ERR_BUSY` (hay que llamar antes a `Deinit`). La deinit suelta los pines (`AFCNTR = 0`) | RM0399 53:820-823, 53:3456-3464 |
| C12 | **Honestidad.** Después de escribir hay read-back de `CFG1`, `CFG2` y `CR2`: si no coinciden, `ERR_VERIFY`. Una escritura que el hardware ignoraría se rechaza con `ERR_BUSY` sin escribir: configuración o `TSIZE` con `SPE = 1`. `CalcPrescaler` devuelve la SCK real, no la pedida | RM0399 53:3273-3275, 53:3302 |
| C13 | **Esperas acotadas.** Toda espera del driver (`TXP`, `RXP`, `EOT`, la guarda de 2.22.5) tiene un límite en ciclos DWT que pasa el llamador. Si `CYCCNT` no avanza o el límite vence, la transferencia se aborta (`IER = 0`, `SPE = 0`) y devuelve `ERR_TIMEOUT`. No hay ningún bucle sin salida | C13 de `03_tim` |
| C14 | **El driver no define símbolos del proyecto:** ni `SPIx_IRQHandler`, ni `HAL_SPI_*Callback`, ni `HAL_SPI_MspInit/MspDeInit`. Tampoco llama a `NVIC_*`: `GetIRQn` solo devuelve el número | C14 de `03_tim` |
| C15 | **Aritmética exacta y reproducible.** `SPI_BM_CalcPrescaler` y `SPI_BM_CalcGuardCycles` son funciones puras: enteros de 64 bits, sin coma flotante, con las reglas de [Reglas de cálculo](#reglas-de-cálculo). Un host independiente las reproduce bit a bit (T3) | C15 de `03_tim` |

## Lo que existe hoy (`8901b77`)

| Elemento | Estado |
| --- | --- |
| `01_shared_drivers/06_spi/` | Solo `.gitkeep`. Sin target en `CMakeLists.txt`; el paraguas ya incluye el directorio (`CMakeLists.txt:163`) |
| Consumidores de SPI | Ninguno en el repositorio (D8) |
| HAL SPI en `prj_01` | `HAL_SPI_MODULE_ENABLED` no está definido en ningún núcleo, y los `stm32h7xx_hal_conf.h` no tienen la línea `#include "stm32h7xx_hal_spi.h"`. `USE_SPI_CRC` vale 0 en los dos núcleos (`C*/Core/Inc/stm32h7xx_hal_conf.h:68`). `stm32h7xx_hal_spi.c` se compila, vacío, por el `GLOB` de la HAL. `HAL_DMA_MODULE_ENABLED` sí está en los dos núcleos |
| Reloj de kernel de SPI en `prj_01` | Nada escribe `RCC_D2CCIP1R` ni `RCC_D3CCIPR`: todo en su valor de reset. SPI1–3: `pll1_q_ck` = 25·HSE = 200 MHz nominales (PLL1 `M = 4`, `N = 400`, `Q = 4`, `CM7/Core/Src/main.c:210-219`). SPI4/5: `pclk2` = 100 MHz. SPI6: `pclk4` = 100 MHz (APB /2, `main.c:226-235`). Con el HSE real (+1,3 %, N1): unos 202,6 y 101,3 MHz |
| SRAM4 en `prj_01` | `.d3_ram` (0x38000000, 32 KB, `RAM_D3`) está vacía en todas las imágenes; `g_shared` en 0x38008000, el área de `00_gpio` en 0x3800A000 y la de `03_tim` en 0x3800B000. **0x3800C000–0x3800FFFF libres.** La región MPU 7 cubre los 64 KB de SRAM4, no cacheable en CM7 (`mpu_config.c:136-142`) |
| Banco del AD2 | El de `00_gpio` §8 y `03_tim` §9, sin cambios. DIO15 sin cablear |

### Hallazgos sobre lo existente

| ID | Sev. | Dónde | Hallazgo | Acción |
| --- | --- | --- | --- | --- |
| SP1 | Baja | `prj_01/CMakeLists.txt:29-30`, `Common/Inc/tim_selftest.h:9-10`, `CM7/App/Src/tim_selftest.c:20`, `tools/tim_hw_check.py:7`, `tools/ad2.py:8` | Comentarios que remiten a `03_tim/IMPLEMENTATION_PLAN.md`, borrado en `8901b77` (recuperable con la forma de `03_tim/README.md` §13) | Fuera de alcance: son evidencia de `03_tim`. El responsable decide |
| SP2 | Media | Cube `stm32h7xx_hal_spi.c` | `SPI_CloseTransfer` (l. 3937-4002) pone `SPE = 0` antes de deshabilitar las interrupciones (2.22.3, una ventana de unas instrucciones), no espera nada entre `EOT` y `SPE = 0` (2.22.5) y, con `USE_SPI_CRC = 1`, programa `CRCSIZE = DSIZE` (l. 326; 2.22.4 con tramas < 8 bits) | Reglas de `spi_hal` (C8, regla 10) y README |
| SP3 | Baja | Linker scripts de CM7 (l. 182-190) y CM4 (l. 107-115) | Los dos núcleos colocan `.d3_ram` en 0x38000000: si los dos pusieran algo, se solaparía | El selftest solo pone buffers en `.d3_ram` desde CM7. La producción no cambia. Va al README como limitación del proyecto |
| SP4 | Nota | N1 de `03_tim` | Con el HSE real, `pll1_q_ck` va a unos 202,6 MHz, por encima de los 200 MHz que la tabla 62 permite al kernel de SPI1–3 en VOS1 | Abierto fuera del driver, con N1. El README lo cita |

## Alcance

| Qué | Estado | Nota |
| --- | --- | --- |
| Maestro, Motorola (`SP = 000`), full-duplex y solo transmisión (`COMM = 00/01`) | Dentro | |
| Modos 0–3, tramas de 4 a 32 bits (4–16 en SPI4–6), MSB o LSB primero | Dentro | Tabla de capacidades |
| `MBR` /2…/256; cálculo de `MBR` con la SCK real; reloj de kernel leído del multiplexor | Dentro | C15 |
| Umbral de FIFO (`FTHLV`) | Dentro | Regla 6 |
| SS por software (el llamador mueve un GPIO; `SSM = 1`, `SSI` inactivo) y por hardware (`SSOE = 1`), con `SSIOP`, `MSSI`, `MIDI` y pulso entre tramas (`SSOM`) | Dentro | |
| `AFCNTR` (`keep_pins`) | Dentro | C11 |
| Transferencias por sondeo, por interrupción (estado del llamador) y por DMA (DMA1/2 y BDMA, programados por el llamador con `02_dma`) | Dentro | C7, D7 |
| `TSIZE` de 1 a 65 535 tramas por llamada | Dentro | `TSER` (recarga), fuera |
| Detección y borrado de `MODF` y `OVR` | Dentro | |
| Deinit a valores de reset sin RCC reset | Dentro | C10 |
| Esclavo, CRC, TI (`SP = 001`), half-duplex, solo recepción, `MASRX`, `TSER`, `TSIZE = 0` (transferencia sin fin), suspensión (`CSUSP`), `IOSWP`, `IOLOCK` | Fuera | Hoja de ruta §5.4; segunda iteración |
| I2S (`I2SCFGR`) | Fuera | `I2SMOD` se deja a 0 |
| Escribir el multiplexor de kernel (`SPIxSEL`) | Fuera | Es del proyecto (C10) |

Una función que el alcance deja fuera no existe en la API. No se deja un parámetro "reservado para después".

## Archivos, dependencias y targets

```text
01_shared_drivers/06_spi/
├── IMPLEMENTATION_PLAN.md       # este archivo; se borra al cierre (queda en git)
├── IMPLEMENTER_INSTRUCTIONS.md  # reglas del implementador; se borra al cierre
├── IMPLEMENTATION_REPORT.md     # lo escribe SOLO el implementador
├── AUDIT.md                     # lo escribe SOLO el auditor
├── README.md                    # al cierre (ver "README de cierre")
├── spi_bm.h / spi_bm.c          # F1
├── spi_ll.h / spi_ll.c          # F2
└── spi_hal.h / spi_hal.c        # F3

03_firmware_templates/prj_01_template_dualcore/
├── CMakeLists.txt               # option(SPI_DRIVER_SELFTEST ...), incompatible con GPIO_ y TIM_DRIVER_SELFTEST
├── CMakePresets.json            # preset selftest-spi
├── CM7/CMakeLists.txt, CM4/CMakeLists.txt   # enlazan _spi_* y _dma_* SOLO con la opción
├── CM7/Core/Inc/stm32h7xx_hal_conf.h        # HAL_SPI_MODULE_ENABLED e include, SOLO con la opción (F3)
├── CM7/App/Src/spi_selftest.c   # secuenciador, pruebas CM7, ISR de prueba
├── CM4/App/Src/spi_selftest.c   # pruebas CM4 (SPI1), servidor de comandos
├── Common/Inc/spi_selftest.h    # área SRAM4, comandos, resultados, tabla de pasos (versión)
├── Common/Src/spi_selftest_common.c
├── CM7/Core/Src/main.c, CM7/App/Src/app_main.c, CM4/Core/Src/main.c   # solo llamadas bajo #if
├── tools/spi_hw_check.py        # host del banco SPI
└── docs/spi_<fase>_<prueba>_<hash>.txt      # capturas

tools/ad2.py                     # solo se añade (decodificación de SPI sobre la captura LA, si hace falta)
```

```cmake
add_library(stm32h7_driver_spi_bm INTERFACE)
target_include_directories(stm32h7_driver_spi_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/06_spi)
target_sources(stm32h7_driver_spi_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/06_spi/spi_bm.c)

add_library(stm32h7_driver_spi_ll INTERFACE)
target_include_directories(stm32h7_driver_spi_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/06_spi)
target_sources(stm32h7_driver_spi_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/06_spi/spi_ll.c)
target_link_libraries(stm32h7_driver_spi_ll INTERFACE stm32h7_driver_spi_bm)

add_library(stm32h7_driver_spi_hal INTERFACE)
target_include_directories(stm32h7_driver_spi_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/06_spi)
target_sources(stm32h7_driver_spi_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/06_spi/spi_hal.c)
target_link_libraries(stm32h7_driver_spi_hal INTERFACE stm32h7_driver_spi_bm)
```

Los pines los configura el llamador con `gpio_bm` (AF, pull de SCK según `CPOL`, pull de SS inactivo). Los streams y canales de DMA, con `02_dma`. `spi_bm` no enlaza ninguno de los dos (C8).

## Contrato de API: spi\_bm (F1)

Es el contrato mínimo. El implementador puede añadir funciones `static` internas, pero cualquier función pública que no esté aquí necesita aprobación (instrucciones §7). Las funciones `SPI_BM_Check*` existen desde F1 para que `spi_ll` y `spi_hal` validen con la misma copia (lección de `03_tim`, su R7-2).

```c
/* ---- Status ------------------------------------------------------------------- */
typedef enum
{
    SPI_BM_OK = 0,
    SPI_BM_ERR_PARAM,        /*!< refused before any peripheral access (C6)                  */
    SPI_BM_ERR_UNSUPPORTED,  /*!< the instance lacks this feature (24-bit frames on SPI4)     */
    SPI_BM_ERR_RANGE,        /*!< not reachable with this prescaler or FIFO                   */
    SPI_BM_ERR_BUSY,         /*!< the hardware would ignore the write in this state (C12)     */
    SPI_BM_ERR_VERIFY,       /*!< read-back differs from what was written (C12)               */
    SPI_BM_ERR_TIMEOUT,      /*!< bounded wait expired, or CYCCNT not running (C13)           */
    SPI_BM_ERR_MODF,         /*!< mode fault: SPE and MASTER cleared by hardware              */
    SPI_BM_ERR_OVERRUN,      /*!< OVR: received data lost                                     */
    SPI_BM_PENDING,          /*!< interrupt transfer still running (SpiBm_Xfer_t.status)       */
    SPI_BM_ERR_HAL           /*!< spi_hal only: a HAL call returned an error                  */
} SpiBm_Status_t;

/* ---- Capabilities (silicon facts, RM0399 table 435) ---------------------------- */
#define SPI_BM_KSEL_SPI123  0U   /* RCC_D2CCIP1R.SPI123SEL */
#define SPI_BM_KSEL_SPI45   1U   /* RCC_D2CCIP1R.SPI45SEL  */
#define SPI_BM_KSEL_SPI6    2U   /* RCC_D3CCIPR.SPI6SEL    */

typedef struct
{
    uint8_t  max_data_bits;    /*!< 32 (SPI1-3) or 16 (SPI4-6)                               */
    uint8_t  fifo_bytes;       /*!< 16 (SPI1-3) or 8 (SPI4-6)                                */
    uint8_t  kernel_select;    /*!< SPI_BM_KSEL_*                                            */
    uint8_t  dmamux;           /*!< 1 (DMA1/DMA2) or 2 (BDMA, SPI6)                          */
} SpiBm_Caps_t;

bool           SPI_BM_IsValidInstance(const SPI_TypeDef *spi);
SpiBm_Status_t SPI_BM_GetCaps(const SPI_TypeDef *spi, SpiBm_Caps_t *out);

/* ---- Clock -------------------------------------------------------------------- */
SpiBm_Status_t SPI_BM_EnableClock(SPI_TypeDef *spi);    /* this core's RCC bank, read back  */
SpiBm_Status_t SPI_BM_DisableClock(SPI_TypeDef *spi);   /* this core's bank only (§9.5.10)  */
SpiBm_Status_t SPI_BM_GetKernelSource(const SPI_TypeDef *spi, uint32_t *field); /* raw SPIxSEL, read only */

/* ---- Pure arithmetic (C15) ------------------------------------------------------ */
SpiBm_Status_t SPI_BM_CalcPrescaler(uint32_t kernel_hz, uint32_t max_sck_hz,
                                    uint32_t *mbr, uint32_t *sck_hz);
SpiBm_Status_t SPI_BM_CalcGuardCycles(uint32_t cpu_hz, uint32_t kernel_hz, uint32_t mbr,
                                      uint32_t *cycles);   /* one SCK period in CPU cycles, rounded up */

/* ---- Configuration -------------------------------------------------------------- */
#define SPI_BM_DIR_FULL_DUPLEX  0U   /* COMM = 00 */
#define SPI_BM_DIR_TX_ONLY      1U   /* COMM = 01 */
#define SPI_BM_NSS_SOFT         0U   /* SSM = 1, SSI = inactive; CS is the caller's GPIO         */
#define SPI_BM_NSS_HW           1U   /* SSM = 0, SSOE = 1, SSOM = 0: active from CSTART to EOT   */
#define SPI_BM_NSS_HW_PULSE     2U   /* SSM = 0, SSOE = 1, SSOM = 1: inactive between frames     */

typedef struct
{
    SPI_TypeDef *spi;
    uint32_t     mbr;            /*!< 0..7: SCK = kernel / 2^(mbr + 1)                         */
    uint32_t     mode;           /*!< 0..3 = CPOL << 1 | CPHA                                  */
    uint32_t     data_bits;      /*!< 4..caps.max_data_bits                                    */
    bool         lsb_first;      /*!< LSBFRST                                                  */
    uint32_t     direction;      /*!< SPI_BM_DIR_*                                             */
    uint32_t     fifo_threshold; /*!< frames per RXP/TXP event, 1..16 (rule 6)                 */
    uint32_t     nss;            /*!< SPI_BM_NSS_*                                             */
    bool         nss_active_high;/*!< SSIOP                                                    */
    uint32_t     mssi;           /*!< 0..15 SCK periods, hardware NSS only (0 with NSS_SOFT)   */
    uint32_t     midi;           /*!< 0..15 SCK periods between frames                         */
    bool         keep_pins;      /*!< AFCNTR (C11)                                             */
    uint32_t     guard_cycles;   /*!< >= 1: CPU cycles between EOT and SPE = 0 (ES0445 2.22.5) */
} SpiBm_Config_t;

SpiBm_Status_t SPI_BM_CheckConfig(const SpiBm_Config_t *cfg);   /* C6 order and states, no write */
SpiBm_Status_t SPI_BM_Init(const SpiBm_Config_t *cfg);          /* SPE = 0 required (ERR_BUSY)   */
SpiBm_Status_t SPI_BM_Deinit(SPI_TypeDef *spi);   /* reset values, pins released; no RCC write  */

/* ---- Blocking transfer -------------------------------------------------------- */
/* Buffer element per frame: uint8_t (4..8 bits), uint16_t (9..16), uint32_t (17..32),
   right-aligned; bits above data_bits are ignored on tx and zero on rx.                   */
SpiBm_Status_t SPI_BM_CheckTransfer(const SPI_TypeDef *spi, const void *tx, const void *rx,
                                    uint32_t frames);
SpiBm_Status_t SPI_BM_Transfer(SPI_TypeDef *spi, const void *tx, void *rx, uint32_t frames,
                               uint32_t max_cycles);   /* rx may be NULL (discarded); NULL in TX_ONLY */

/* ---- Interrupt transfer (state in the caller's struct, C7) --------------------- */
typedef struct
{
    const void              *tx;
    void                    *rx;
    uint32_t                 frames;
    uint32_t                 tx_done;   /*!< driver-maintained                              */
    uint32_t                 rx_done;   /*!< driver-maintained                              */
    volatile SpiBm_Status_t  status;    /*!< SPI_BM_PENDING until the end                   */
} SpiBm_Xfer_t;

SpiBm_Status_t SPI_BM_TransferStartIt(SPI_TypeDef *spi, SpiBm_Xfer_t *x);
bool           SPI_BM_TransferIrq(SPI_TypeDef *spi, SpiBm_Xfer_t *x);   /* from the project's ISR;
                                                                           true when x->status is final */
/* ---- DMA (streams and channels belong to the caller, 02_dma) ------------------- */
SpiBm_Status_t SPI_BM_DmaPrepare(SPI_TypeDef *spi, uint32_t frames, bool rx);   /* TSIZE, RXDMAEN */
SpiBm_Status_t SPI_BM_DmaStart(SPI_TypeDef *spi);                    /* TXDMAEN, SPE, CSTART     */
SpiBm_Status_t SPI_BM_WaitEnd(SPI_TypeDef *spi, uint32_t max_cycles);   /* EOT or error, no disable */
SpiBm_Status_t SPI_BM_DmaEnd(SPI_TypeDef *spi);   /* after the caller stopped its streams: guard,
                                                     SPE = 0, TXDMAEN = RXDMAEN = 0, flags        */
SpiBm_Status_t SPI_BM_GetDmaRequest(const SPI_TypeDef *spi, bool tx, uint32_t *request_id);
SpiBm_Status_t SPI_BM_GetIRQn(const SPI_TypeDef *spi, IRQn_Type *out);

/* ---- Errors --------------------------------------------------------------------- */
#define SPI_BM_ERRF_OVR    SPI_SR_OVR      /* bit positions of SR / IFCR */
#define SPI_BM_ERRF_MODF   SPI_SR_MODF
#define SPI_BM_ERRF_TIFRE  SPI_SR_TIFRE
uint32_t       SPI_BM_TakeErrors(SPI_TypeDef *spi);   /* IFCR = taken (one write), then DSB */
```

### Reglas de implementación (lo que audito línea a línea)

1. **Validación completa antes del primer acceso (C6).** Un rechazo no toca ni el reloj de RCC. Las comprobaciones de estado (`SPE`, `CSTART`, `AFCNTR`) leen registros, pero no escriben.
2. **Tabla de capacidades.** Una sola tabla `static const` indexada por instancia, con la sección o línea del RM en un comentario por fila. Valores mínimos:

   | Instancia | Bus, bit de enable | Dominio | Trama máx. | FIFO | Kernel | DMAMUX: RX / TX | IRQn |
   | --- | --- | --- | --- | --- | --- | --- | --- |
   | SPI1 | APB2, bit 12 | D2 | 32 | 16 B | SPI123SEL | 1: 37 / 38 | 35 |
   | SPI2 | APB1L, bit 14 | D2 | 32 | 16 B | SPI123SEL | 1: 39 / 40 | 36 |
   | SPI3 | APB1L, bit 15 | D2 | 32 | 16 B | SPI123SEL | 1: 61 / 62 | 51 |
   | SPI4 | APB2, bit 13 | D2 | 16 | 8 B | SPI45SEL | 1: 83 / 84 | 84 |
   | SPI5 | APB2, bit 20 | D2 | 16 | 8 B | SPI45SEL | 1: 85 / 86 | 85 |
   | SPI6 | APB4, bit 5 | D3 | 16 | 8 B | SPI6SEL | 2: 11 / 12 (BDMA) | 86 |

   Fuentes: tabla 435 (53:53-71), §9.7.44–47 (09:7115, 7283, 7277, 7428, 7516, 7536, 7542, 7586, 7690), tablas 126 y 130 (18:127-133, 18:98-99, 18:233-234) y `stm32h755xx.h:96-144`. DS12923 (l. 2493-2495) dice "4 to 16 bits" y "8x 8-bit FIFOs" para todas: manda el RM. Si el implementador encuentra en el RM un dato distinto, manda el RM: lo apunta como errata del plan (instrucciones §5.7).
3. **`Init` (C11, C12).** Exige `SPE = 0` (`ERR_BUSY`). Orden: `IER = 0` → `IFCR` = todos los flags → `CFG1` (`MBR`, `DSIZE`, `FTHLV`, `CRCSIZE = 00111`, `CRCEN = 0`, DMA a 0, `UDRDET = UDRCFG = 0`) → `CFG2` con `AFCNTR = 0` (`MASTER = 1`, `SP = 000`, `COMM`, `CPOL`, `CPHA`, `LSBFRST`, `SSM`, `SSOE`, `SSOM`, `SSIOP`, `MSSI`, `MIDI`, `IOSWP = 0`) → `CR1` con `SSI` inactivo si `SSM = 1` (`MASRX = 0`, `IOLOCK = 0`) → `CFG2.AFCNTR = 1` si `keep_pins` → read-back de `CFG1`, `CFG2` y `CR1`. `I2SCFGR` se deja a 0. `SSI` inactivo es `SSI ≠ SSIOP` (RM0399 53:566-567).
4. **Una transacción por activación (2.22.1).** Cada transferencia hace `TSIZE = frames` (con `SPE = 0`) → `SPE = 1` → precarga de la TxFIFO → `CSTART`, y termina con: `EOT` → drenaje de la RxFIFO (`RXWNE`/`RXPLVL`, RM0399 53:1197-1202) → guarda de `guard_cycles` (2.22.5) → `IER = 0` → `SPE = 0` → `IFCR` → `TXDMAEN = RXDMAEN = 0` si se usaron. Nunca hay un `CSTART` con `SPE = 1` después de un `EOT`.
5. **Interrupciones e `IER` (2.22.3).** `IER` solo es distinto de 0 mientras `SPE = 1`: se escribe después de `SPE = 1` y se pone a 0 **antes** de `SPE = 0`, en el fin normal, en el aborto y en la deinit. `SPI_BM_TransferIrq` sirve `RXP`/`TXP`/`EOT`/`OVR`/`MODF` con el umbral de la regla 6 y aplica el fin de la regla 4 dentro de la ISR (la guarda incluida).
6. **Tramas, accesos y umbral.** El acceso a `TXDR`/`RXDR` es del ancho del elemento del buffer (8, 16 o 32 bits), nunca menor que `DSIZE` (53:3783). Con `DSIZE = 4` un acceso de 8 bits empaqueta dos tramas (53:772-776): el driver las combina por pares, y con un número impar de tramas trata la última según el RM (el implementador cita la línea). `fifo_threshold` es t tramas por evento: t × bytes por trama ≤ `fifo_bytes / 2` (53:3394), y si no, `ERR_RANGE`. `FTHLV = t − 1`.
7. **Abortos (C13).** Si vence `max_cycles` o `CYCCNT` no avanza: `IER = 0` → `SPE = 0` (RM0399 53:1071: la trama en curso se corta y la RxFIFO se vacía) → `IFCR` → `ERR_TIMEOUT`. `MODF` → `ERR_MODF` (el hardware ya puso `SPE = MASTER = 0`; el driver borra `MODF` y deja `CFG2.MASTER` como lo dejó el hardware, así que hace falta `Init` otra vez). `OVR` → la transferencia sigue hasta `EOT` o aborto, y devuelve `ERR_OVERRUN`.
8. **DMA (RM0399 53:1262-1274).** `DmaPrepare`: `SPE = 0` requerido; `TSIZE`, `RXDMAEN` si `rx`. El llamador programa y arranca sus streams. `DmaStart`: `TXDMAEN`, `SPE = 1`, `CSTART`. `WaitEnd`: `EOT`, `OVR` o `MODF`. El llamador para sus streams. `DmaEnd`: guarda, `IER = 0`, `SPE = 0`, `TXDMAEN = RXDMAEN = 0`, `IFCR`. Es el orden de cierre del RM: DMA, después SPI, después los bits de DMA.
9. **Deinit (C10, C11).** Exige una instancia válida. Orden: `IER = 0` → `SPE = 0` → `CR1`/`CR2`/`CFG1`/`CFG2` a su valor de reset (`CFG1 = 0x00070007`; `CFG2 = 0`, que suelta los pines) → `IFCR` = todos → read-back. No escribe en RCC. `CRCPOLY`, `UDRDR` e `I2SCFGR` se comprueban en reset (el driver nunca los escribe).
10. **`spi_hal`.** Exige `USE_SPI_CRC == 0` (`#error`): con 1, `HAL_SPI_Init` pone `CRCSIZE = DSIZE` con el CRC apagado (SP2, 2.22.4). 2.22.5: la HAL no deja guarda entre `EOT` y `SPE = 0`; `spi_hal` no puede insertarla dentro de `HAL_SPI_TransmitReceive`, así que el README lo lista como limitación y recomienda BM/LL con SCK lenta y `CPHA = 0` (decisión abierta 5).
11. **Sin literales de proyecto (C1).** `grep -nE "BOARD_|GPIO[A-K]|HSEM|DMA[12]_Stream|BDMA_Channel|NVIC_|200000000|100000000" spi_*.[ch]` = 0.

### Reglas de cálculo

Son las que T3 reproduce en el host. Cambiar una es cambiar la API: hay que preguntar antes.

- **`CalcPrescaler`.** `ERR_PARAM` si `kernel_hz = 0` o `max_sck_hz = 0`. `mbr` = el menor valor 0..7 con `kernel_hz / 2^(mbr+1) ≤ max_sck_hz` (comparación exacta: `kernel_hz ≤ max_sck_hz · 2^(mbr+1)` en 64 bits). Si ninguno cumple → `ERR_RANGE`. `sck_hz = kernel_hz >> (mbr + 1)` (truncado; el valor exacto es `kernel_hz / 2^(mbr+1)`).
- **`CalcGuardCycles`.** `ERR_PARAM` si `cpu_hz = 0`, `kernel_hz = 0` o `mbr > 7`. `cycles = ceil(cpu_hz · 2^(mbr+1) / kernel_hz)`, en 64 bits; si no cabe en 32 bits → `ERR_RANGE`. Es un periodo de SCK. ES0445 2.22.5 pide "a delay" sin cifra; el plan adopta un periodo de SCK, la medida que 2.22.1 da para el caso parecido ("at least one SCK cycle").

## Contrato de API: spi\_ll (F2) y spi\_hal (F3)

**`spi_ll`.** Mismas operaciones que `spi_bm` (`SPI_LL_*`), con constantes LL de `stm32h7xx_ll_spi.h`: `LL_SPI_BAUDRATEPRESCALER_DIV2..256`, `LL_SPI_POLARITY_LOW/HIGH`, `LL_SPI_PHASE_1EDGE/2EDGE`, `LL_SPI_DATAWIDTH_4BIT..32BIT`, `LL_SPI_MSB_FIRST/LSB_FIRST`, `LL_SPI_FULL_DUPLEX/SIMPLEX_TX`, `LL_SPI_FIFO_TH_01DATA..`, `LL_SPI_NSS_SOFT/HARD_OUTPUT`, `LL_SPI_NSS_POLARITY_LOW/HIGH` y las de `MSSI`/`MIDI` de la misma cabecera (el implementador lista las usadas con su línea). Reglas:

- Traduce cada constante LL al valor BM y valida **llamando a `SPI_BM_Check*`**: una sola copia de la validación y de la tabla. Una constante LL desconocida se convierte en un valor BM fuera de rango y se rechaza con `ERR_PARAM` en el sitio del orden C6.
- Escribe con funciones `static inline` de la cabecera LL (y `LL_SPI_WriteReg`/`ReadReg` donde ninguna inline escribe el registro entero), en el orden de las reglas 3–9.
- El reloj, `Deinit`, `TakeErrors`, `GetIRQn`, `GetDmaRequest` y las funciones puras se usan de `spi_bm` (no tienen constantes LL).
- **Paridad:** para la misma configuración, la imagen de registros es la de BM bit a bit (T2).

**`spi_hal`.** Envoltorio fino de la HAL para código que ya usa `SPI_HandleTypeDef`:

```c
SpiBm_Status_t SPI_HAL_Init(SPI_HandleTypeDef *h, const SpiBm_Config_t *cfg);   /* HAL_SPI_Init  */
SpiBm_Status_t SPI_HAL_Transfer(SPI_HandleTypeDef *h, const void *tx, void *rx, uint32_t frames,
                                uint32_t timeout_ms);   /* HAL_SPI_TransmitReceive / _Transmit */
SpiBm_Status_t SPI_HAL_StartTransferIt(SPI_HandleTypeDef *h, const void *tx, void *rx, uint32_t frames);
SpiBm_Status_t SPI_HAL_StartTransferDma(SPI_HandleTypeDef *h, const void *tx, void *rx, uint32_t frames);
SpiBm_Status_t SPI_HAL_Wait(SPI_HandleTypeDef *h, uint32_t timeout_ms);   /* State READY + ErrorCode */
SpiBm_Status_t SPI_HAL_Deinit(SPI_HandleTypeDef *h);
```

Reglas:

- Valida con `spi_bm` **antes** de llamar a la HAL (C6). `h == NULL`, `h->Instance` que no es un SPI o distinto de `cfg->spi` → `ERR_PARAM`.
- Read-back de lo que escribe cada llamada (C12). Error de la HAL → `ERR_HAL`; `HAL_OK` con read-back distinto → `ERR_VERIFY`.
- No define callbacks ni MSP (C14). `HAL_SPI_Init` llama a la MSP del proyecto si `h->State` es RESET. Con IT y DMA, el proyecto define el `SPIx_IRQHandler` que llama a `HAL_SPI_IRQHandler(h)`, y `SPI_HAL_Wait` sondea el estado del handle: no hace falta ningún callback. Para DMA, `h->hdmatx`/`h->hdmarx` los enlaza el llamador (con `DMA_HAL_Init` de `02_dma`).
- `SPI_HAL_Deinit` no llama a `HAL_SPI_DeInit` (llamaría a la MSP): llama a `SPI_BM_Deinit` y deja el handle en RESET.
- **Paridad:** la imagen es la de BM salvo los bits que la HAL escribe distinto por diseño, cada uno con su línea de `stm32h7xx_hal_spi.c` en el informe y en el README (T2).

## Matriz RM0399 / ES0445

| Regla | Mecanismo | Prueba |
| --- | --- | --- |
| 53:3273-3275: `CFG1`, `CFG2`, `CRCPOLY`, `UDRDR` protegidos con `SPE = 1` | `ERR_BUSY` en `Init`; regla 4 | T1 |
| 53:3302: `TSIZE` solo con el SPI deshabilitado | Regla 4; `DmaPrepare` exige `SPE = 0` | T1, T4 |
| 53:1012-1022: deshabilitar y volver a habilitar entre transacciones | Regla 4 | T4, T12 |
| 53:1128-1134: fin del maestro (`EOT`, drenar, `SPE = 0`) | Regla 4 | T4 |
| 53:1262-1274: orden de DMA al empezar y al cerrar | Regla 8 | T7 |
| 53:772-776, 53:3783: empaquetado y ancho de acceso | Regla 6 | T4 (`DSIZE` 4, 8, 16) |
| 53:3394: `FTHLV` ≤ media FIFO | Regla 6, `ERR_RANGE` | T1 |
| 53:539-542, 53:1465-1472: `MODF` | `ERR_MODF`, regla 7 | T11 |
| 53:722, 53:3456-3464: `AFCNTR` y nivel de reposo de SCK | C11 | T5(e), T0b(e) |
| 53:3476, 53:531-537: SS por hardware, `SSOM` | `SPI_BM_NSS_HW*` | T5(c) |
| 53:3523-3534: `MIDI`, `MSSI` | Campos de la configuración | T5(d) |
| Tabla 435: trama y FIFO por instancia | Tabla de capacidades | T1, T2 |
| §9.7.1 / §9.5.10: `PERxEN` por núcleo | `Enable/DisableClock` solo en el banco del que llama | T13 |
| `RCC_APBxRSTR`, `RCC_D2CCIP1R`, `RCC_D3CCIPR` | Nunca se escriben | A9 |
| Tablas 126 y 130: peticiones de DMAMUX1/2 | `GetDmaRequest` | T7(d) |
| ES0445 2.22.1: bloqueo con `CSTART` a menos de una SCK de `EOT` | Regla 4 (`SPE = 0` tras cada `EOT`) | T12(a) |
| ES0445 2.22.3: `TXP`/`TXC` con `SPE = 0` disparan la IRQ | Regla 5 | T6(c) |
| ES0445 2.22.4: `CRCSIZE < 00111` con `CRCEN = 0` y `TSIZE > 0` | `CRCSIZE = 00111` siempre (regla 3); `spi_hal` exige `USE_SPI_CRC = 0` | T2 |
| ES0445 2.22.5: salidas truncadas si `SPE = 0` llega muy pronto tras `EOT` | Guarda de `guard_cycles` (regla 4); en `spi_hal`, limitación del README | T12(b) |
| ES0445 2.22.2: CRC con `UDRDET ≠ 0` (esclavo) | Fuera de alcance; README | Revisión |

## Banco de pruebas

Es el banco de `00_gpio/README.md` §8 (placa NUCLEO-H755ZI-Q MB1363 Rev V, AD2 SN `210321B5D38D`, GUI de WaveForms cerrada), **más el puente J1** (D1). S1–S12 siguen vigentes tal como están. Este plan añade S13 y S14 y decide qué hace cada línea en las pruebas de SPI.

### Puente J1 (lo coloca el responsable del catálogo)

| Extremo | Conector | MCU | Función |
| --- | --- | --- | --- |
| A | CN7-14 (D11) | PB5 | SPI1\_MOSI (AF5) / SPI6\_MOSI (AF8) |
| B | CN7-12 (D12) | PA6 | SPI1\_MISO (AF5) / SPI6\_MISO (AF8) |

Cable macho–macho en el lado superior (hembra) de CN7; sin resistencia. Sin cablear, pero usados: PA5 (CN7-10, D13; SCK de SPI1/SPI6) y PB4 (CN7-19; MISO alternativo para el control D2). El esquema MB1363 no muestra pull ni carga en PA5, PA6 ni PB5 (SB65 DNF en PA5). **Antes de F0, el responsable confirma visualmente SB52 (PA7–RMII), SB56 DNF y SB57 ON**, porque el informe de pines los dedujo del texto del UM2408.

### Uso de las líneas en las pruebas de SPI

| Línea | MCU | Función SPI | Uso | Quién conduce |
| --- | --- | --- | --- | --- |
| DIO3 | PE11 | SPI4\_NSS (AF5) | SS por hardware (T5) | MCU, solo en pasos "bus = MCU" (S13) |
| DIO4 | PE12 | SPI4\_SCK (AF5) | SCK (T5, T12b) | Igual |
| DIO5 | PE13 | SPI4\_MISO (AF5) | MISO, **entrada** con pull: el RX de SPI4 lee el nivel del pull | Nadie (el AD2 en entrada) |
| DIO6 | PE14 | SPI4\_MOSI (AF5) | MOSI (T5) | MCU, solo en "bus = MCU" |
| DIO0–2, DIO7 | PE8–PE10, PE15 | — | Entrada con pulls de S5; nunca AF | Bus |
| DIO14 | PA8 | MCO1 (AF0) | Referencia de frecuencia en las capturas de T5 | Solo MCU |
| DIO12 / DIO13 | PE2 / PE7 | — | `HOST_REQ` / `MCU_ACK` | AD2 / MCU |
| DIO8–11 | PE3, PE5, PE4, PE6 | — | Entrada; nunca AF ni salida en el banco SPI | — |
| J1-A | PB5 | MOSI de SPI1/SPI6 | MOSI (T4, T6, T7, T8); salida GPIO solo en T0c | MCU |
| J1-B | PA6 | MISO de SPI1/SPI6 | MISO; **nunca salida** (S14) | Lo conduce PB5 a través de J1 |
| — | PA5 | SCK de SPI1/SPI6 | SCK (sin observar) | MCU |
| — | PB4 | MISO alternativo | Control D2, entrada con pull; **nunca salida** | Nadie |

**Nunca se tocan:** PE0, PE1 (LD2), PC13, PA7 (RMII), PA4, PA13/PA14 (SWD), TIM6 (tick de la HAL de CM7), SysTick, `RCC_D2CFGR`, `RCC_D2CCIP1R`, `RCC_D3CCIPR`, `RCC_APBxRSTR` y `DBGMCU`. `RCC_CFGR` solo se escribe al empezar y acabar los pasos de T5 (`MCO1SEL`/`MCO1PRE`, MCO1 = HSE/8, como `03_tim` R3-2): se guarda antes y se restaura con read-back.

### Reglas nuevas S13 y S14

| # | Regla | Cómo se aplica | Quién lo comprueba |
| --- | --- | --- | --- |
| S13 | **SPI4 en el bus.** PE11, PE12 y PE14 solo van a AF5 en un paso "bus = MCU" de la tabla de pasos; PE13 va a AF5 como entrada en ese mismo paso. `bench_claim()` lee SPI4 antes de aceptar: reloj de SPI4 en el banco de CM7, `CR1.SPE = 0`, `CFG2.AFCNTR = 0` y `CFG2.MASTER = 1` (o `CFG2 = 0`, reset). Con `SPE = 0` y `AFCNTR = 0` el SPI no conduce (RM0399 53:3456-3464), así que la prueba S5 se hace **con los pines ya en AF5** y tiene que pasar. Un paso que use `keep_pins = true` termina con `Deinit` antes de devolver el bus, y `bench_safe()` lo comprueba. PE8–PE10 y PE15 siguen sin AF; PE0–PE7 sin AF ni salida | `CM7/App/Src/spi_selftest.c`; ningún otro camino a `MODER`/`AFR` de un pin cableado | grep (A27) + T0b(e) |
| S14 | **El puente J1.** PA6 y PB4 nunca son salida GPIO: solo entrada o AF de MISO (AF5 desde CM4, AF8 desde CM7). PB5 es salida GPIO solo en T0c, y AF de MOSI en los demás pasos. PA5, PA6, PB4 y PB5 los toma un solo núcleo por paso (CM4 en los pasos de SPI1, CM7 en los de SPI6), con el lock GPIO entre núcleos de `prj_01`. `bench_safe()` deja los cuatro en entrada sin pull | `bench_claim()` en los dos núcleos, con una tabla común de pasos | grep (A27) + T0b(f) |

El AD2 no usa Digital Out en ningún paso (D6): `spi_hw_check.py` no llama a `ad2_set_dout`, y en cada paso lee que el enable de Digital Out es 0 (S2 de `03_tim`). Las líneas del AD2 son entradas salvo `HOST_REQ` y lo que S1–S10 permiten en T0a/T0b.

### Herramientas

| Archivo | Contenido |
| --- | --- |
| `tools/ad2.py` | Solo se añade: si hace falta, un decodificador de SPI sobre la captura LA existente (muestras → flancos de SCK → bits de MOSI según `CPOL`/`CPHA`/orden). Las funciones existentes no cambian. `gpio_hw_check.py` y `tim_hw_check.py` siguen pasando sus pruebas con el mock |
| `prj_01/tools/spi_hw_check.py` | Igual que `tim_hw_check.py`: S2 en `try/finally`, banner `SPI_BENCH v1 idle`, tabla de pasos con versión (S7), comprobación del id de build, `[host] argv:`/`[host] options:`, una captura por ejecución con marcas de tiempo hasta `HOST VERDICT=`, `--checklist-confirmed` |

### Selftest

- Preset `selftest-spi` (`SPI_DRIVER_SELFTEST=ON`, Debug). Incompatible con `GPIO_DRIVER_SELFTEST` y `TIM_DRIVER_SELFTEST` (`FATAL_ERROR`).
- Con la opción apagada, las imágenes `debug`/`release` de `prj_01` tienen el mismo `.text`, `.data` y `.bss` que en `2481396` (los números de `docs/tim_f0_map_sizes_2481396.txt`). F0 lo mide en `8901b77` y en cada entrega (A16).
- **Área SRAM4:** `0x3800C000`, 4 KB (`_Static_assert`), dentro de `RAM_SHARED` y de la región MPU 7. CPU1 la pone a cero y abre sesión antes de `HSEM_BM_BootReleaseCpu2`.
- **Buffers de BDMA** (SPI6): en `.d3_ram`, **solo desde CM7** (SP3). Buffers de DMA1/2: en `.dma_buf` (CM7) o en la SRAM2 de CM4.
- **Propiedad de las instancias** (C10): **CM7 tiene SPI2, SPI3, SPI4, SPI5 y SPI6. CM4 tiene SPI1.** En T13, CM4 llama a `Enable/DisableClock(SPI2)`: solo escribe su banco.
- CM7 corre en `Status_Task` y CM4 después de `Shared_GpioLockInstall()` y `CPU2_PublishBuild()`, como en `03_tim`. Las ISR de prueba (`SPIx_IRQHandler`, `DMAx_Streamy_IRQHandler`, `BDMA_Channelx_IRQHandler`) viven en `spi_selftest.c` bajo la opción, con prioridad 6 y sin FreeRTOS. `stm32h7xx_it.c` no gana ningún handler. Los streams de DMA de la prueba los declara `spi_selftest.c`, uno por uso y núcleo, con `fail_if_busy = true`.
- Cada núcleo imprime primero `Rev <FW_GIT_REV>` e id de build. **El veredicto global lo imprime CM7** cuando CM4 ha terminado. Las pruebas con AD2 pasan solo si también pasan sus líneas `[ad2]` en la misma captura.
- Cada prueba deja su instancia con `Deinit` y comprueba la imagen de reset (T14).

## Fases

Todo va en la rama `driver/spi`. Commits del estilo del repo: `build(spi)`, `feat(spi)`, `fix(spi)`, `test(prj_01)`, `feat(tools)`, `docs(spi)`. Una preocupación por commit, el porqué en el cuerpo y el ID del hallazgo entre paréntesis.

| Fase | Entregable | Gate (lo comprueba el auditor) |
| --- | --- | --- |
| **F0: banco** | **Requisito previo: J1 colocado y confirmado por el responsable** (hecho el 2026-10-03; ver la cabecera). Sin código del driver ni targets. Opción, preset y esqueleto del selftest (Rev, id, veredicto, área SRAM4, tabla de pasos). `spi_hw_check.py` con S2–S8, S13 y S14 **desde el primer commit**. **T0a, T0b(a)–(f) y T0c** (protocolo v1) | 8/8 y C9. Producción de `prj_01` con los tamaños de `2481396`. **T0b y T0c en verde en la ejecución del auditor antes de cualquier prueba que mueva el bus o los pines de J1.** `git diff 8901b77 -- 02_firmware_projects` vacío |
| **F1: `spi_bm`** | Contrato completo de `spi_bm`. Pruebas BM en los dos núcleos: T1–T14 y B1 | A1–A28. **API de `spi_bm` congelada** |
| **F2: `spi_ll`** | Contrato de `spi_ll`. T1, T2, T4, T5(a), T6, T7(a)(b) y T14 con LL en los dos núcleos | A1–A28. Paridad LL = BM bit a bit (T2). `spi_bm.h` solo añade. **API de `spi_ll` congelada** |
| **F3: `spi_hal`** | Contrato de `spi_hal`. `HAL_SPI_MODULE_ENABLED` en CM7 solo con la opción. T1, T2, T4, T5(a), T6, T7(b)(c) y T14 con HAL en CM7 | A1–A28. Paridad HAL contra BM con las diferencias listadas. `HAL_GetTick` sigue avanzando. **API de `spi_hal` congelada** |
| **Cierre** | A petición del auditor, después de F3. **Implementador:** `06_spi/README.md` y la línea de `06_spi` en `01_shared_drivers/README.md`. **Responsable del catálogo, no el implementador:** `DRIVER_ROADMAP.md`, la línea de BDMA de `02_dma/README.md` §9, la mención de J1 en `00_gpio/README.md` §8, borrado de los documentos de proceso, merge y borrado de la rama | Nada documentado sin código ni prueba detrás. Auditoría de cierre en `AUDIT.md` |

## Plan de pruebas

Cada prueba tiene un **control negativo** (R1) y un **oráculo que no es el driver**. "Tier" dice con qué tiers se ejecuta en F1 (BM), F2 (LL) y F3 (HAL).

**Patrón de datos.** Todas las transferencias de datos usan xorshift32 con una semilla por transferencia que el firmware imprime. El firmware imprime además un resumen (CRC-32 de las tramas enmascaradas a `DSIZE`) y la máscara de bits que tomaron los dos valores: el host regenera la secuencia, comprueba el resumen y exige que cada bit de la trama haya valido 0 y 1. Así un patrón constante (que haría pasar un control de pull) no cuenta.

**Lo que el puente no ve.** Un lazo MOSI→MISO en el mismo maestro no distingue el orden de bits ni `CPOL`/`CPHA`: un error simétrico en TX y RX se cancela. Eso lo cubre T5, donde el AD2 decodifica MOSI con el modo y el orden declarados.

**Tolerancias del AD2.** El LA muestrea a 100 MS/s (10 ns). Las capturas de SPI4 van a `MBR = /16` (SCK nominal 6,25 MHz, 16 muestras por periodo) salvo donde se diga. El tamaño de buffer se lee de `FDwfDigitalInBufferSizeInfo` y el firmware ajusta el número de tramas para que la transferencia entera quepa. Frecuencia de SCK: cociente con MCO1 en la misma adquisición, ±0,5 % (un error de `MBR` es un factor 2). Tiempos: en periodos de SCK medidos en la misma captura, ±(2 muestras).

| ID | Fase / tier | Qué | Oráculo | Control negativo | Criterio |
| --- | --- | --- | --- | --- | --- |
| T0a | F0 y al empezar cada sesión | Las 13 líneas que no son del handshake, sueltas y en walking-1, como T0a de `03_tim` | DIO del AD2 e `IDR` del MCU | DIO9 (PE5) con pull-down donde el host espera pull-up | Las 13 líneas siguen cada patrón; el control marca solo DIO9 |
| T0b | F0 | Guardas. (a)–(d) como en `03_tim` (rechazo sin escritura, `ABORT … contention check`, pines en entrada al final). **(e)** S13: `bench_claim` de PE11–PE14 en AF5 fuera de un paso "bus = MCU", o con SPI4 `SPE = 1`, o con `AFCNTR = 1` → rechazo sin escribir; con la configuración correcta, aceptado, y la prueba S5 con los pines en AF5 pasa. **(f)** S14: salida en PA6 o PB4, salida en PB5 fuera de T0c, AF distinto de 5/8, o un núcleo que no es el del paso → rechazo sin escribir | Fotos de `MODER`/`AFR`/`OTYPER` y de los registros de SPI4; DIO del AD2 | (e) SPI4 con `AFCNTR = 1` y `SPE = 0` en el paso de control: SCK queda en `CPOL` y no sigue al pull, y la función S5 tiene que abortar (sin contienda: el AD2 está en entrada) | Rechazos exactos; (e) aborta con `AFCNTR = 1` y pasa con `AFCNTR = 0` |
| T0c | F0 y al empezar cada sesión | Puente J1: PB5 salida push-pull (velocidad baja) escribe 0/1 en 16 pasos; PA6 entrada sin pull; PB4 y PA5 en entrada con el pull contrario al nivel de PB5 | `IDR` de PA6, PB4 y PA5 | PB5 en entrada sin pull: PA6 sigue a su propio pull (up y down), así que la prueba distingue "conducido" de "pull" | PA6 sigue a PB5 en los 16 pasos; PB4 y PA5 siguen a su pull (sin corto) |
| T1 | F1 BM, F2 LL, F3 HAL | Rechazos: `NULL`; puntero que no es un SPI (`(SPI_TypeDef *)USART3`, `SPI1 + 4`); `data_bits` 3, 33, y 17 en SPI4/5/6; `mode` 4; `mbr` 8; `direction` 2; `nss` 3; `mssi`/`midi` 16; `mssi ≠ 0` con `NSS_SOFT`; `fifo_threshold` 0 y por encima de media FIFO (16 bits × 3 en SPI4 → `ERR_RANGE`); `guard_cycles = 0`; `frames` 0 y 65 536; `rx ≠ NULL` en `TX_ONLY`; `tx = NULL`; `Init`/`DmaPrepare` con `SPE = 1` (`ERR_BUSY`); `keep_pins` con `AFCNTR = 1` vigente y otro `CPOL` (`ERR_BUSY`) | Foto de todos los registros de la instancia y de los bancos `RCC_C1`/`RCC_C2` de `APB1LENR`/`APB2ENR`/`APB4ENR`, antes y después | Una llamada válida sí cambia la foto | Estado exacto por caso y fotos idénticas en los rechazos |
| T2 | F1 BM, F2 LL, F3 HAL | Imagen de registros por instancia (las seis): `mbr` 0..7 × modos × `data_bits` (4, 8, 16, 32 donde exista y los extremos) × orden × dirección × `fifo_threshold` (1 y máximo) × `nss` (3) × `SSIOP` × `mssi`/`midi` (0, 15) × `keep_pins` | Tabla de campos del RM escrita a mano en la prueba. En CM7, también la imagen que deja la HAL cruda (`HAL_SPI_Init` sin el driver). LL y HAL: también contra BM | Una imagen esperada alterada a propósito se detecta | Bits idénticos; `CRCSIZE = 00111` y `CRCEN = 0` en todos (2.22.4); `I2SCFGR`, `CRCPOLY` y `UDRDR` en reset. Diferencias de la HAL por diseño, una por una |
| T3 | F1 | Funciones puras: ≥ 200 vectores de `CalcPrescaler` (límites: `max = kernel/2`, `max = kernel/256` exacto y uno por debajo → `ERR_RANGE`, potencias de 2, primos) y ≥ 100 de `CalcGuardCycles` (`mbr` 0..7, relojes de `prj_01` y extremos que desbordan 32 bits) | El host reimplementa las [reglas de cálculo](#reglas-de-cálculo) en Python con enteros | El host altera a propósito un esperado: tiene que marcar fallo | Coincidencia exacta |
| T4 | F1 BM (los dos núcleos), F2 LL, F3 HAL | Lazo por sondeo: SPI6 en CM7 (AF8) y SPI1 en CM4 (AF5), `MBR` para 12,5 MHz. Modos 0–3 × `data_bits` {4, 5, 7, 8, 9, 15, 16} (+ {17, 24, 31, 32} en SPI1) × MSB/LSB × tramas {1, 2, 3, FIFO − 1, FIFO, FIFO + 1, 255, 4096} × `fifo_threshold` {1, máximo}. Una vez, 65 535 tramas de 8 bits en SPI6. `DSIZE = 4` con número par e impar de tramas | El cable: RX = TX enmascarado a `DSIZE`, comparado por el firmware; el host comprueba el resumen y la cobertura del patrón | MISO a **PB4** (D2) con pull-up y luego pull-down: RX = todo unos / todo ceros, y el número de discrepancias es el que el host calcula con la semilla | 0 discrepancias, `SPI_BM_OK` y tiempo dentro de `max_cycles` en todos; el control da exactamente las discrepancias esperadas |
| T5 | F1 BM, F2 LL (a), F3 HAL (a) | SPI4 observado por el AD2 (CM7, paso "bus = MCU", S13), con DIO14 = MCO1. (a) Modos 0–3 × `data_bits` {4, 8, 13, 16} × MSB/LSB, SS por hardware: el host decodifica MOSI con el modo y el orden declarados y lo compara con la semilla; SCK en reposo = `CPOL`; flancos de SCK = 2 × `DSIZE` × tramas. (b) `SSIOP` = 0 y 1. (c) `NSS_HW_PULSE` con `midi` 2: SS inactivo entre cada par de tramas. (d) `mssi` y `midi` 0, 1, 4 y 15. (e) `keep_pins` = 1 frente a 0 con pull contrario a `CPOL` en PE12 entre dos transferencias. (f) RX de SPI4 = nivel del pull de PE13 (up y down). (g) Frecuencia de SCK con `MBR` /4 … /256 | Captura LA a 100 MS/s; el decodificador del host es independiente del firmware | El host decodifica cada captura de (a) también con el `CPHA` contrario y con el orden contrario: tiene que encontrar discrepancias (el decodificador distingue). (e) con `keep_pins = 0` SCK sigue al pull | (a) 0 discrepancias; (b) nivel activo = `SSIOP`; (c) tramas − 1 pulsos; (d) retardo(k) − retardo(0) = k periodos ± 2 muestras; (e) con 1, SCK = `CPOL` entre transferencias; (f) exacto; (g) f\_SCK / f\_MCO1 = 100 / 2^(MBR+1) ± 0,5 % |
| T6 | F1 BM (los dos núcleos), F2 LL, F3 HAL | Por interrupción: SPI6 (CM7) y SPI1 (CM4), lazo J1, 8 y 16 bits (y 32 en SPI1), tramas {1, FIFO + 1, 1024}. (a) Datos correctos. (b) Número de IRQ entre la primera y la de `EOT`, registrado. (c) **2.22.3:** 10 ms después del fin, con `SPE = 0`, 0 IRQ del SPI | Cable; contador de la ISR de prueba | (c) En la prueba, a mano: `TXPIE = 1` con `SPE = 0` → la IRQ salta (en reset `SR.TXP = 1`, 53:3597), así que el contador ve una IRQ espuria | (a) 0 discrepancias; (c) 0 IRQ; el control da ≥ 1 |
| T7 | F1 BM, F2 LL (a)(b), F3 HAL (b)(c) | DMA. (a) SPI1 (CM4), DMA1 o DMA2 con `dma_bm`, full-duplex 8/16/32 bits, 1…4096 tramas. (b) **SPI6 (CM7) con BDMA** (`dma_hal`, `BDMA_REQUEST_SPI6_RX/TX`), buffers en `.d3_ram`, 8/16 bits, hasta 8192 tramas. (c) SPI4 (CM7) solo TX por DMA con `dma_bm`, observado por el AD2 (decodificación de T5). (d) `GetDmaRequest` de las seis instancias | (a)(b) cable; (c) AD2; (d) `DMA_REQUEST_SPI*` y `BDMA_REQUEST_SPI6_*` de `stm32h7xx_hal_dma.h` | (a)(b) `RXDMAEN = 0`: el buffer RX conserva su centinela y el estado es `ERR_OVERRUN` | (a)(b) 0 discrepancias; (c) 0 discrepancias; (d) igual a la tabla |
| T8 | F1 BM | Barrido de velocidad: SPI1 (CM4, kernel 200 MHz) y SPI6 (CM7, kernel 100 MHz), `MBR` /256 … /2, 1024 tramas de 8 y de máximo ancho, pines de J1 a velocidad `VERY_HIGH` | Cable | MISO a PB4 a la velocidad más alta que pasa: falla | **Pasa obligatoriamente hasta 25 MHz.** Por encima se registra el resultado (D5); el README da la frecuencia más alta que pasó |
| T9 | F1 BM (CM7: SPI2, SPI3, SPI5; CM4: SPI1) | Motor sin pines: solo TX, `MBR` /16 y /64, 8 y 16 bits, N = 1000 y 2000 tramas; duración de N a `EOT` con `CYCCNT` | t(2000) − t(1000) = 1000 × `DSIZE` × 2^(MBR+1) ticks de kernel, en ciclos de CPU (CM7: ×2 en SPI1–3, ×4 en SPI4–6; CM4: ×1 en SPI1) | La misma cuenta con `MBR + 1`: fuera de tolerancia | ± 1 % |
| T10 | F1 BM | Reloj de kernel: `GetKernelSource` de las seis instancias = 0 (reset en `prj_01`), contrastado con la lectura directa de `RCC_D2CCIP1R`/`RCC_D3CCIPR` | Registro leído directamente | — (solo lectura; el control es T9: un kernel distinto cambia el tiempo) | Igual |
| T11 | F1 BM | Errores: (a) `MODF`: la prueba fuerza `SSI` activo a mano con `SSM = 1` → `Transfer` devuelve `ERR_MODF`, `SPE = MASTER = 0`; `Deinit` + `Init` recuperan. (b) `TakeErrors` borra con una escritura de `IFCR` | Registros leídos directamente | (a) con `SSI` inactivo no hay `MODF` | (a) estado y registros exactos; transferencia posterior correcta |
| T12 | F1 BM | Erratas. **(a) 2.22.1:** 10 000 transferencias seguidas de 1 trama en SPI6 a `MBR` /256, sin pausa: 0 `ERR_TIMEOUT`. **(b) 2.22.5:** SPI4, `CPHA = 0`, `MBR` /256, observado por el AD2: el último pulso de SCK dura medio periodo | (a) `CYCCNT`; (b) LA | A mano en la prueba: (a) `CSTART` con `SPE = 1` justo después de `EOT`, 10 000 veces; (b) `SPE = 0` sin guarda. Se registra si el fallo se reproduce (decisión abierta 6) | (a) 0 timeouts; (b) \|último − medio periodo\| ≤ 2 muestras |
| T13 | F1 BM | Bancos de RCC: CM7 habilita SPI2 y lo deja transmitiendo (solo TX por DMA, 65 535 tramas a `MBR` /256, unos 0,7 s); CM4 llama a `DisableClock(SPI2)` → SPI2 sigue; CM4 `EnableClock` + `DisableClock` → sigue; CM7 `DisableClock(SPI2)` → se para | `CTSIZE`/`EOT` de SPI2 leídos por CM7; `RCC_C1`/`RCC_C2` `APB1LENR` | El último paso sí lo para | Cada paso como se describe; solo cambia el banco del que llama |
| T14 | F1 BM, F2 LL, F3 HAL | Deinit tras cada prueba: registros con su valor de reset; en los pines cableados, la prueba S5 de pulls pasa | Tabla de reset del RM escrita a mano | Una instancia sin desinicializar a propósito se detecta | Imagen exacta; S5 en verde |
| B1 | F1, F2, F3 | `.text`/`.data`/`.bss` de cada `spi_*.c.obj` en CM4 y CM7 (`.map`); ciclos de `Init`, de `Transfer` de 1 trama y de `TransferIrq` por evento | `.map` y DWT | — | Solo registro; `.data = .bss = 0` (C7) |

### No ejercitado por diseño (va al README)

- SCK de SPI1/SPI6 y SS por hardware de SPI1/2/3/5/6: sin línea del AD2 (D3). SPI2, SPI3 y SPI5 sin pines: solo motor (T9) e imagen (T2).
- Datos de RX desde un dispositivo distinto del propio maestro (solo lazo y pull).
- Velocidades por encima de 25 MHz con criterio (D5).
- `spi_hal` en CM4 (HAL SPI no se habilita en CM4).
- Revisiones de silicio distintas de V; otros árboles de reloj, salvo por las funciones puras (T3).

## Protocolo de auditoría

### Qué entrega el implementador al cerrar una fase

1. El rango de commits en `driver/spi`, con el árbol limpio.
2. La salida de `tools/build_all.ps1 -Clean` (8/8), la de `selftest-spi` (0 avisos) y la de la compilación aparte con `CMAKE_C_FLAGS=-Werror` (C9).
3. Tamaños del `.map` enlazado (B1), sin "Discarded input sections", y la comparación de la producción con `2481396`.
4. La captura de `spi_hw_check.py`: una ejecución, sin editar, con marcas de tiempo, `Rev` igual al commit y sin `-dirty` en los dos núcleos, id de build igual al del host, T0a y T0c al principio y `HOST VERDICT=` al final.
5. La lista de controles negativos de la fase con el valor que dio cada uno.
6. `IMPLEMENTATION_REPORT.md` con la sección de la fase.

### Qué compruebo y cómo

| # | Punto | Cómo | Pasa si |
| --- | --- | --- | --- |
| A1 | Reglas 1–4 del README y checklist §6 de las guías | Lectura del diff | Sin excepciones |
| A2 | C1 | `grep -nE "BOARD_\|GPIO[A-K]\|HSEM\|DMA[12]_Stream\|BDMA_Channel\|NVIC_\|board_def\|shared_memory\|gpio_\|dma_\|tim_" 06_spi/spi_*.[ch]` | 0, salvo `IRQn_Type` y los nombres de la tabla de capacidades |
| A3 | C2 | `git diff <cierre de la fase anterior> -- 06_spi/spi_*.h` | Solo añade o comenta |
| A4 | C3 | `git grep -n "06_spi/"` en código C | 0 |
| A5 | C4 | `CMakeLists.txt` | Los tres targets, LL/HAL enlazan BM, sin target sin sufijo, paraguas igual |
| A6 | C6 | Lectura de cada función pública + T1 con fotos | Ningún camino toca RCC o SPI antes de validar |
| A7 | C7 | `.map` de CM4 y CM7 | `.data = .bss = 0` en los tres `.obj` |
| A8 | C8 | `grep -n "#include" 06_spi/spi_*.[ch]`; un `.c` que solo incluye `spi_bm.h` compilado sin HAL | Solo lo permitido; compila |
| A9 | C10 | `grep -nE "RSTR\|D2CCIP1R\s*=\|D3CCIPR\s*=\|D2CCIP1R\s*[\|&]=\|D3CCIPR\s*[\|&]=\|__enable_irq\|IFCR\s*[\|&]=" 06_spi/` y lectura de los RMW de `CR1`/`CFG1`/`IER` | 0; cada RMW con PRIMASK guardado y restaurado |
| A10 | C11 y reglas 3, 4, 8, 9 | Lectura + T5(e) | Orden de las reglas |
| A11 | C12 | Lectura + T1 (`ERR_BUSY`) | Read-back en cada escritura de configuración |
| A12 | C13 | Lectura de cada bucle | Todos acotados por el llamador |
| A13 | C14 | `grep -nE "IRQHandler\|HAL_SPI_[A-Za-z]*Callback\|MspInit\|MspDeInit\|NVIC_" 06_spi/` | 0 |
| A14 | C15 | T3 ejecutado por mí | Coincidencia exacta |
| A15 | Tabla de capacidades | Cada fila contra el RM | Sin diferencias, o justificadas |
| A16 | C9 y producción intacta | `build_all -Clean` y tamaños de `prj_01` debug/release | 8/8; iguales a `2481396` |
| A17 | R3 | `.map` | Coinciden con la entrega |
| A18 | Pruebas de la fase | Flasheo `selftest-spi` y ejecuto **yo** `spi_hw_check.py`, en frío y sin depurador | Mis resultados coinciden, con el mismo `Rev` |
| A19 | R1 | Cada control imprime su valor y falla como se espera | Un control que no falla invalida su prueba (salvo la decisión abierta 6) |
| A20 | Oráculo independiente | Lectura de las pruebas y del script | Ninguna prueba compara el driver consigo mismo |
| A21 | R2 | Cada captura | `Rev` e id limpios, marcas de tiempo, T0a y T0c |
| A22 | R4 | Lectura + ejecución | TIM6, SysTick, PE0/PE1/PC13, PA7, `D2CFGR`, `D2CCIP1R`, `D3CCIPR`, `RSTR` y `DBGMCU` intactos; `RCC_CFGR` restaurado; `HAL_GetTick` avanza |
| A23 | Alcance | `git diff 8901b77 --stat` | Nada fuera de `01_shared_drivers/06_spi`, `01_shared_drivers/CMakeLists.txt`, `03_firmware_templates/prj_01_template_dualcore` y `tools/ad2.py`. Al cierre, además, la línea de `01_shared_drivers/README.md` |
| A24 | Paridad (F2, F3) | T2 | LL = BM; HAL = BM salvo diferencias listadas |
| A25 | Erratas | Lectura + T2, T6(c), T12 | 2.22.1, 2.22.3, 2.22.4 y 2.22.5 aplicadas (2.22.5 en `spi_hal`, documentada); 2.22.2 en el README |
| A26 | Documentación | README y comentarios contra el código | Nada prometido que no exista |
| **A27** | **Seguridad eléctrica S1–S14** (en todas las fases y **antes** de conectar el AD2 a la imagen) | `grep -n "FDwfDigitalIOOutputEnableSet\|FDwfDigitalOutEnableSet\|FDwfDigitalOutConfigure\|ad2_set_dout" prj_01/tools/spi_hw_check.py`: Digital Out nunca habilitado; OE solo en su guarda. `grep -nE "MODER\|OTYPER\|AFR" C*/App/Src/spi_selftest.c`: todo pasa por `bench_claim`. Las máscaras coinciden con la tabla de líneas. T0b y T0c en verde en **mi** ejecución | Todo se cumple. **Cualquier incumplimiento es bloqueante** |
| A28 | Informe | `IMPLEMENTATION_REPORT.md` contra el código y las capturas | Cada número con su fuente; sin lenguaje de veredicto |

### Clasificación de lo que encuentre

| Clase | Significado | Efecto |
| --- | --- | --- |
| Bloqueante (B) | Incumple un criterio C, una regla R o S, o un gate; una prueba no se reproduce en mi ejecución; evidencia editada o sin hash | La fase no pasa |
| Mayor (M) | Defecto real que no invalida la fase | Se corrige antes de la fase siguiente |
| Menor (m) | Robustez, estilo o documentación | Puede ir al cierre |
| Nota | Observación sin acción | Solo registro |

`AUDIT.md` lo escribe solo el auditor. Un hallazgo se cierra cuando lo verifica el auditor. Las capturas no se editan.

## README de cierre

Lo escribe el implementador **solo cuando el auditor lo pida**, después de F3. En inglés, con la estructura de `03_tim/README.md` y **como mucho 600 líneas**:

1. Cabecera: qué cubre, estado (placa, fecha, `Rev` de las capturas) y enlace a la validación.
2. **Tiers:** tiers, targets, dependencias y uso típico; tier × núcleo; matriz de funciones BM/LL/HAL.
3. **API summary.**
4. **Configuration guide:** reloj de kernel (`GetKernelSource` + la frecuencia del proyecto); `CalcPrescaler` y `CalcGuardCycles`; orden (reloj → `Init` → pines con `gpio_bm`, pull de SCK = `CPOL` y de SS = inactivo → transferencia); SS por software y por hardware; IRQ con `SpiBm_Xfer_t`; DMA con `GetDmaRequest`, `02_dma` y el orden de la regla 8; BDMA y `.d3_ram`.
5. **Project contract:** propiedad por núcleo, bancos de RCC, multiplexor de kernel del proyecto, sin callbacks ni MSP.
6. **Rules the driver enforces:** C6, C10–C13.
7. **Silicon facts and errata:** tabla de capacidades, 2.22.1–2.22.5, SP2, SP4.
8. **Examples** (compilan con `-Wall -Wextra -Wpedantic -Wconversion -Werror`): transferencia BM por sondeo con CS por GPIO; LL por interrupción; HAL con BDMA en SPI6.
9. **Test bench:** diagrama ASCII con J1 y las líneas de SPI4, tabla de líneas, S13 y S14.
10. **Validation:** pruebas con resultado y captura; reproducción; no ejercitado.
11. **Known limitations** (2.22.5 en `spi_hal`, SP3), **Consumers** (`prj_01` selftest; ninguno más), **Changing this driver**, **History**.

## Riesgos

| Riesgo | Mitigación |
| --- | --- |
| PA6 conducido por el MCU contra PB5 a través de J1 (MCU contra MCU, sin resistencia) | S14: PA6 y PB4 nunca salida; `bench_claim` en los dos núcleos; T0b(f) |
| SPI4 conduciendo PE11/12/14 fuera de su paso | S13: `AFCNTR = 0`, `SPE = 0` al reclamar, S5 con los pines en AF5, `Deinit` antes de devolver el bus |
| PA7 o PG13 (Ethernet) en AF | Fuera de la tabla de líneas; `bench_claim` los rechaza |
| Romper el tick de la HAL (TIM6) | El selftest SPI no toca timers; A22 comprueba `HAL_GetTick` |
| Solape de `.d3_ram` entre núcleos (SP3) | Buffers de BDMA solo desde CM7 |
| Velocidades por encima del límite del pin | D5: criterio hasta 25 MHz |
| La errata no se reproduce en rev V y el control de T12 no falla | Decisión abierta 6 |
| Que el plan tenga un dato del RM equivocado | Manda el RM; errata del plan con la cita (instrucciones §5.7) |

## Decisiones abiertas

Cada una trae la opción por defecto que asume este plan.

1. **Retorno:** `SpiBm_Status_t` en los tres tiers (recomendado, como `03_tim`).
2. **Banco:** J1 en D11–D12 (recomendado). Alternativa: sin puente, solo SPI4 con el AD2, que no valida RX con datos.
3. **DIO15 → PA5:** no se cablea (recomendado; cambia §8 de `00_gpio`). Alternativa: un cable más para ver SCK de SPI1/SPI6 con el AD2.
4. **Propiedad:** CM7 tiene SPI2–SPI6 y CM4 tiene SPI1 (recomendado: CM4 ejercita DMA1/2 y CM7 el BDMA con los tres tiers).
5. **2.22.5 en `spi_hal`:** se documenta como limitación (recomendado). Alternativa: rechazar en `spi_hal` las configuraciones lentas, lo que exige conocer los relojes.
6. **Controles de erratas (T12):** si el fallo no se reproduce en rev V, el control se registra como "no reproducido", la prueba positiva cuenta y el README lo dice (recomendado: el ES manda aplicar el workaround aunque esta placa no falle). Alternativa: quitar T12 y dejar 2.22.1/2.22.5 solo en revisión.
7. **HAL SPI en CM7:** `HAL_SPI_MODULE_ENABLED` e include solo bajo `SPI_DRIVER_SELFTEST` (recomendado: la producción no cambia).
8. **Documentos de proceso al cierre:** se borran el plan, las instrucciones, el informe y `AUDIT.md`, como en `03_tim`.
