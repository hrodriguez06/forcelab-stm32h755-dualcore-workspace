# Plan del driver I2C 05\_i2c

Oct 7, 2026 · @Hector · revisión 2 (sustituye a la revisión 1, `3d9c830`)

Este plan cubre el paso 5 de `DRIVER_ROADMAP.md`: el driver I2C `05_i2c` en sus tres tiers, `i2c_bm`, `i2c_ll` e `i2c_hal`, en modo maestro. Tiene una fase F0 de preparación del banco (con el target externo), tres fases de implementación (F1 BM, F2 LL, F3 HAL) y una de cierre.

Cada fase pasa una auditoría independiente antes de empezar la siguiente, con las reglas de [Protocolo de auditoría](#protocolo-de-auditoría). Las reglas del implementador están en [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md).

Base: `cc8aa2c` de `main` (cierre de `06_spi` y corrección N3 de `02_dma`), árbol limpio, rama `driver/i2c`.

**Decisiones del responsable ya tomadas (2026-10-05/06):** banco de opción A (bus en la Nucleo, sin dispositivo comercial) con una **NUCLEO-F334R8 como target I2C independiente**, con su propio firmware de banco; se acepta ese firmware como desviación del encargo (D2) y la descarga del CMSIS de STM32F3 (`STMicroelectronics/cmsis_device_f3`, v2.3.9).

## Cambios respecto a la revisión 1

Salen de la auditoría de F0 (`AUDIT.md`, `85f9d29` y `ee96c37`): la observación O5, que el responsable del catálogo aprobó como D9 el 2026-10-07, y el hallazgo M1. Afectan a F1 en adelante; F0 no cambia.

| # | Cambio | Por qué |
| --- | --- | --- |
| R2-1 | **S17 relativa (D9).** Con tomas: SDA (2+) ≥ 3,0 V y la razón SCL/SDA = 0,956 ± 0,015, el divisor del pull-up de 1 kΩ con los 22 kΩ a GND de PF14 (22/23). Sin tomas: 1+ < 0,10 V (S19, sin cambios) y 2+ ≥ 3,0 V. La H755 y la F334 siguen leyendo 1 en PB8/PB9 en los dos estados | Los umbrales absolutos de la revisión 1 (SCL 3,16 ± 0,10 V, SDA ≥ 3,20 V) daban unos 40 mV de margen: en la sesión taps del auditor los dos niveles salieron un 1,7 % más bajos con la misma razón (O5), y en notaps 2+ mide PF15 contra el 1 MΩ del AD2, unos 35 mV sobre 3,20 V (m13). La razón detecta lo mismo (el 22 kΩ a GND en el nodo de SCL; un pull-up de 2,2 kΩ da 0,909 y uno de 10 kΩ, 0,688) sin depender del riel ni de la ganancia del AD2 |
| R2-2 | **T8(b) y T10(a) no se fían solo de la F334 armada (M1).** Antes de usarlos, la F334 decide el ACK/NACK de cada byte con `RXNE` y `TCR` juntos (si `TCR` llega con `RXNE` pendiente, consume primero el byte), y el stretching por byte en lecturas (SBC con `RELOAD` en transmisión de esclavo) se comprueba. En la prueba, el oráculo es triple: `nk` y `rx` del informe de la F334, el recuento del driver y, con tomas, el NACK y la duración del stretching vistos en el osciloscopio | En la revisión 1, el NACK del byte k dependía del orden en que el ISR de la F334 ve `RXNE` y `TCR` (`target.c:439-482`); un NACK perdido o tardío haría pasar o fallar T8(b) por la razón equivocada |
| R2-3 | **Nombre de las capturas:** `docs/i2c_<fase>_<prueba>_<estado>_<hash>.txt`, como en las instrucciones §3.4 | Errata del plan que señaló el informe de F0 (§5.7) |

## Encargo

Texto del responsable del catálogo (el mismo de `03_tim` y `06_spi`, aplicado al paso siguiente):

> Desarrolla el driver bajo el estándar de driver de catálogo, asegurando que incluya implementaciones: BM, HAL y LL.
>
> Como restricción de alcance, no intentes validar ni corregir el resto de los proyectos del repositorio; enfoca la validación exclusivamente en prj_01_template_dualcore, aprovechando las conexiones ya establecidas con el AD2 (Analog Discovery 2).
>
> Al finalizar, genera un README.md limpio y conciso con la información técnica trascendental (diagrama/pines con AD2, guía de uso y configuración), manteniendo el mismo formato y calidad que los drivers ya terminados en el proyecto.

## Fuentes normativas

Por orden de precedencia:

1. Este encargo y las decisiones del responsable de la cabecera.
2. `01_shared_drivers/README.md`, reglas 1–4.
3. `DRIVER_ROADMAP.md` §5.5 y §6 (reglas R1–R4), con las desviaciones de la tabla siguiente.
4. `00_docs_stm32h7/DRIVER_ARCHITECTURE_GUIDELINES.md` y `00_docs_stm32h7/CLAUDE.md`.
5. `00_gpio/README.md` §8 (S1–S10), `03_tim/README.md` §8 (S11, S12) y `06_spi/README.md` §8 (S13, S14).
6. Silicio y herramientas:
   - RM0399 rev 4: cap. 50 (I2C; `00_docs_stm32h7/docs/peripherals/i2c/50_inter-integrated-circuit-i2c-interface.txt`, se cita `50:<línea>`), cap. 9 (RCC, tabla 62), cap. 13 (SYSCFG_PMCR), cap. 18 (DMAMUX, tablas 126 y 130).
   - ES0445 rev 6, §2.19.
   - DS12923: tabla 8 (tipo de I/O `FT_f`), tablas 59/60 (niveles), §6.3.33 y tablas 104–105 (I2C).
   - UM2408 y el esquema MB1363; de la F334: UM1724, DS9994, RM0364 y el CMSIS `cmsis_device_f3` v2.3.9.
   - WaveForms SDK Reference Manual: §8 Digital I/O, AnalogIn, y `FDwfDigitalI2c*` (p. 137: solo maestro y espía).
   - HAL del Cube `8ec3113` (STM32H7xx HAL V1.11.6).

## Desviaciones respecto a la hoja de ruta

| # | Hoja de ruta §5.5 | Este plan | Por qué |
| --- | --- | --- | --- |
| D1 | "El AD2 como esclavo I2C … con registros conocidos" | El target es una **NUCLEO-F334R8** con firmware de banco (registros conocidos, NACK, stretching y bus colgado bajo demanda). El AD2 **observa** el bus con su osciloscopio | El WaveForms SDK no tiene esclavo I2C: solo `FDwfDigitalI2c*` de maestro y espía (manual p. 137) |
| D2 | Validación solo en `prj_01` | Además de `prj_01`, un **firmware de banco** para la F334 en `tools/i2c_target_f334/`. No es un proyecto del catálogo ni se valida: es un instrumento, con su propia identidad (Rev, id) comprobada en cada captura | Aprobado por el responsable el 2026-10-06 |
| D3 | "Tiempos medidos por el AD2" a las tres velocidades | El AD2 mide los tiempos a **100 y 400 kHz**, con las tomas J2/J3 puestas. **1 MHz** se ejecuta sin J2/J3, en una sesión aparte, y se juzga por los datos (oráculo de la F334) y la duración con `CYCCNT` | Con las tomas, el bus carga unos 250 pF: con 1 kΩ, t\_r ≈ 225–270 ns, fuera del límite de Fm+ (120 ns, 50:379-383). Sin ellas, unos 50 ns |
| D4 | "Bus colgado provocado (el AD2 retiene SDA)" | Lo provoca la **F334** (SDA o SCL en GPIO open-drain, acotado en tiempo) | El AD2 no conduce ninguna línea del bus (las tomas son entradas analógicas) |
| D5 | "Control negativo sin pull-up" | Sin quitar componentes a mitad de sesión no hay "sin pull-up". Lo sustituyen T9 (SCL o SDA retenidos abajo) y T15 (instancia sin pines en AF): el driver devuelve error acotado y no se cuelga | Una sesión es una ejecución sin intervención (instrucciones §3.4) |
| D6 | Erratas 2.19.1, 2.19.2, 2.19.4, 2.19.6 (+2.19.7 en la segunda iteración) | Se tratan las siete de ES0445 §2.19, también 2.19.3 y 2.19.5, que la hoja de ruta omite (ver la matriz) | ES0445 rev 6 |
| D7 | — | Las herramientas comunes ganan selección por número de serie: `tools/flash.py --sn`, y el host elige probe y puerto COM por SN | Con dos ST-LINK conectados, `flash.py`, el reset por SWD y `monitor.autodetect()` usan el primero que encuentran: podrían flashear o resetear la F334 |
| D8 | — | No hay migración de proyectos | Ningún proyecto usa I2C (`git grep` de `I2C1`–`I2C4`, `hi2c`, `LL_I2C`, `HAL_I2C_` fuera de `external/`) |

## Criterios de catálogo

Los mismos 15 criterios de `06_spi`, adaptados. Se auditan en cada fase.

| # | Criterio | Origen |
| --- | --- | --- |
| C1 | **Sin pines ni instancias del proyecto.** El driver no configura GPIO, no incluye `board_def.h`, `gpio_*.h`, `dma_*.h`, `tim_*.h` ni `hsem_*.h`, y no contiene frecuencias, prioridades ni streams. Solo hechos del silicio en una tabla de capacidades por instancia. La recuperación del bus usa funciones del llamador para mover SCL/SDA (regla 9) | README regla 1 |
| C2 | **API congelada al pasar su auditoría** (BM en F1, LL en F2, HAL en F3); después solo se añade | README regla 2 |
| C3 | Sufijo de tier, nombres únicos, `#include` sin prefijo numérico | README reglas 3 y 4 |
| C4 | `stm32h7_driver_i2c_bm`, `_ll` y `_hal` (estos dos enlazan `_bm`); sin target sin sufijo; el paraguas no los enlaza | `06_spi` C4 |
| C5 | **Erratas:** se aplican en el driver o van al README con su ID | CLAUDE.md §F |
| C6 | **Validar antes de escribir**, también RCC y SYSCFG. Orden: `NULL`, instancia, capacidad, rango, estado (`ERR_BUSY`) | `06_spi` C6 |
| C7 | **Sin estado en RAM:** `.data = .bss = 0` en los tres objetos; el estado de IT vive en `I2cBm_Xfer_t` del llamador | `06_spi` C7 |
| C8 | **Dependencias hacia abajo.** `i2c_bm`: `stm32h7xx.h`, `<stdint.h>`, `<stdbool.h>`. `i2c_ll`: inlines de `stm32h7xx_ll_i2c.h` y `LL_I2C_WriteReg`/`ReadReg`, sin `USE_FULL_LL_DRIVER`. `i2c_hal`: `stm32h7xx_hal.h`, vacío sin `HAL_I2C_MODULE_ENABLED`. Sin FreeRTOS | `06_spi` C8 |
| C9 | **Evidencia:** `build_all -Clean` 8/8; `selftest-i2c` con 0 avisos y aparte con `-Werror`; capturas con `Rev` e id limpios en H755, CM4 y F334 | Hoja de ruta §6 |
| C10 | **Propiedad y concurrencia.** Un núcleo dueño por instancia, sin locks entre núcleos. En RCC, solo el banco del núcleo que llama (`APB1LENR` I2C1–3, `APB4ENR` I2C4); nunca `RCC_APBxRSTR`, `RCC_D2CCIP2R` ni `RCC_D3CCIPR` (solo lectura). `SYSCFG_PMCR` (compartido con `EPIS` de `08_eth`, `BOOSTE`, `PxSO`) solo con RMW bajo PRIMASK y read-back. `ICR` en una escritura. RMW de `CR1`/`CR2` bajo PRIMASK guardado y restaurado | RM0399 §9.7.1; 13:33-137 |
| C11 | **Pines sin glitch y siempre open-drain.** El driver no toca pines; el llamador los pone en AF con `OTYPE = OD`. Con `PE = 0` el periférico suelta SCL y SDA (50:430-431) | RM0399 50:429-445 |
| C12 | **Honestidad.** Read-back de `CR1`, `TIMINGR`, `TIMEOUTR` y `OAR1`; `ERR_BUSY` para lo que el hardware ignoraría (`TIMINGR`, `ANFOFF`, `DNF`, `NOSTRETCH` con `PE = 1`, 50:2505-2529, 2808; `CR2` con `START = 1`, 50:2620-2674). `CalcTiming` devuelve la frecuencia que su modelo predice, no la pedida | RM0399 cap. 50 |
| C13 | **Esperas acotadas** por `max_cycles` de DWT, con salida si `CYCCNT` no cuenta o se para (más de 1000 lecturas iguales), **también dentro de cualquier espera fija** (lección m1 de `06_spi`). En `i2c_hal`, salida si `HAL_GetTick()` se para (lección m4) | `06_spi` C13 |
| C14 | **El driver no define símbolos del proyecto** (`I2Cx_EV/ER_IRQHandler`, `HAL_I2C_*Callback`, MSP) ni llama a `NVIC_*`. Las funciones de recuperación del bus las pasa el llamador como datos (regla 9) | `06_spi` C14 |
| C15 | **Aritmética exacta:** `I2C_BM_CalcTiming` es pura, entera de 64 bits, con las [Reglas de cálculo](#reglas-de-cálculo); el host la reproduce (T3) | `06_spi` C15 |

## Lo que existe hoy (`cc8aa2c`)

| Elemento | Estado |
| --- | --- |
| `01_shared_drivers/05_i2c/` | Solo `.gitkeep`. El paraguas ya incluye el directorio |
| Consumidores | Ninguno (D8) |
| HAL I2C en `prj_01` | `HAL_I2C_MODULE_ENABLED` no está en ningún núcleo; `stm32h7xx_hal_i2c.c` se compila vacío por el `GLOB` |
| Reloj de kernel en `prj_01` | Nada escribe `I2C123SEL` ni `I2C4SEL`: reset, 00 = `pclk1` (I2C1–3) y `pclk4` (I2C4), 100 MHz nominales cada uno (APB /2, `CM7/Core/Src/main.c:233-242`; 09:4744-4749, 4868-4873). Es el máximo de VOS1 (tabla 62, 09:1757, 1849); con el HSE real (+1,3 %, N1) van a unos 101,3 MHz |
| SRAM4 | Bloques de selftest en 0x3800A000 (gpio), 0x3800B000 (tim), 0x3800C000 (spi). **Libre: 0x3800D000** |
| Banco | AD2 de `00_gpio` §8 y J1 de `06_spi`. **PB8/PB9 no los usa ningún banco**; PF14/PF15 los usa `00_gpio` (T5, T6, T9) con sus cargas (22 kΩ a GND / a 3V3, 150 pF) |
| Herramientas | `tools/flash.py`, el reset de los `*_hw_check.py` y `monitor.autodetect()` no eligen ST-LINK por SN (D7) |

### Hallazgos sobre lo existente

| ID | Sev. | Dónde | Hallazgo | Acción |
| --- | --- | --- | --- | --- |
| IC1 | Media | HAL V1.11.6, `I2C_IsErrorOccurred` (`stm32h7xx_hal_i2c.c:7164-7284`) y `HAL_I2C_ER_IRQHandler` (4664-4706) | La HAL aborta la transferencia con `BERR` en modo maestro; ES0445 2.19.2 dice que es espurio y que la transferencia sigue | Limitación documentada de `i2c_hal` (regla 11) |
| IC2 | Baja | `06_spi/README.md` l. 171 y 557-560 | Describen N3 de `02_dma` como abierto; `57b1bf7` lo corrigió | Fuera de alcance: el responsable decide |
| IC3 | Nota | N1 (`03_tim`), SP4 (`06_spi`) | El kernel de I2C a ~101,3 MHz supera los 100 MHz de VOS1 | Abierto fuera del driver; va al README |

## Alcance

| Qué | Estado | Nota |
| --- | --- | --- |
| Maestro, direcciones de 7 bits; escritura, lectura, escritura + repeated start + lectura; sondeo de dirección (escritura de 0 bytes) | Dentro | |
| 1 a 65 535 bytes por transferencia (`RELOAD` por tramos de 255) | Dentro | |
| Sm, Fm, Fm+ (hasta 1 MHz); filtro analógico y digital (`ANFOFF`, `DNF`); `TIMEOUTR` (SCL bajo, `TIMEOUTA` con `TIDLE = 0`) | Dentro | |
| `CalcTiming`: `TIMINGR` a partir del reloj de kernel, la velocidad, t\_r, t\_f y los filtros | Dentro | C15 |
| Fm+ por instancia (`SYSCFG_PMCR.I2CxFMP`) | Dentro | C10. Los bits `PBxFMP` son del proyecto |
| Sondeo, interrupción (estado del llamador) y DMA (DMA1/2 y BDMA, programados por el llamador) | Dentro | |
| `NACK`, `ARLO`, `BERR` (2.19.2), `TIMEOUT`; recuperación del bus (hasta 9 pulsos de SCL y STOP) con funciones del llamador | Dentro | Regla 9 |
| Esclavo, direcciones de 10 bits, SMBus/PMBus, PEC, `GCEN`, wakeup desde Stop, `NOSTRETCH = 1` en maestro | Fuera | Hoja de ruta §5.5: segunda iteración. `NOSTRETCH` debe ser 0 en maestro (50:2505) |
| Escribir `I2C123SEL`/`I2C4SEL` | Fuera | Del proyecto (C10) |

## Archivos, dependencias y targets

```text
01_shared_drivers/05_i2c/
├── IMPLEMENTATION_PLAN.md, IMPLEMENTER_INSTRUCTIONS.md   # se borran al cierre
├── IMPLEMENTATION_REPORT.md      # SOLO el implementador
├── AUDIT.md                      # SOLO el auditor
├── README.md                     # al cierre
├── i2c_bm.h / i2c_bm.c           # F1
├── i2c_ll.h / i2c_ll.c           # F2
└── i2c_hal.h / i2c_hal.c         # F3

03_firmware_templates/prj_01_template_dualcore/
├── CMakeLists.txt, CMakePresets.json        # I2C_DRIVER_SELFTEST, preset selftest-i2c; FATAL_ERROR con GPIO_/TIM_/SPI_
├── CM7/CMakeLists.txt, CM4/CMakeLists.txt   # _i2c_* y _dma_* solo con la opción
├── CM7/Core/Inc/stm32h7xx_hal_conf.h        # HAL_I2C_MODULE_ENABLED, su include y USE_HAL_I2C_REGISTER_CALLBACKS 0U, solo con la opción (F3)
├── CM7/App/Src/app_main.c                   # pila de Status_Task: 1024 palabras con la opción
├── CM7/App/Src/i2c_selftest.c, CM4/App/Src/i2c_selftest.c
├── Common/Inc/i2c_selftest.h, Common/Src/i2c_selftest_common.c
├── CM7/Core/Src/main.c, CM4/Core/Src/main.c # solo llamadas bajo #if
├── tools/i2c_hw_check.py                    # host del banco I2C
└── docs/i2c_<fase>_<prueba>_<hash>.txt

tools/i2c_target_f334/            # firmware de banco de la F334 (D2): fuentes, CMakeLists, build.ps1
├── cmsis/                        # subconjunto de cmsis_device_f3 v2.3.9 (cabeceras y startup del F334, LICENSE)
tools/flash.py, tools/monitor.py  # solo se añade: --sn / selección por SN (D7)
tools/ad2.py                      # solo se añade: disparo por flanco de bajada y decodificador I2C analógico
```

Targets CMake como los de `06_spi` (`CMakeLists.txt:101-117`). El núcleo Cortex-M4 del firmware de la F334 usa `core_cm4.h` del CMSIS del Cube H7.

## Contrato de API: i2c\_bm (F1)

Contrato mínimo; cualquier función pública más necesita aprobación (instrucciones §7).

```c
typedef enum
{
    I2C_BM_OK = 0,
    I2C_BM_ERR_PARAM,        /*!< refused before any peripheral access (C6)                     */
    I2C_BM_ERR_UNSUPPORTED,  /*!< not available on this instance                                */
    I2C_BM_ERR_RANGE,        /*!< no TIMINGR meets the mode with this kernel clock (2.19.1)     */
    I2C_BM_ERR_BUSY,         /*!< the hardware would ignore the write in this state (C12)       */
    I2C_BM_ERR_VERIFY,       /*!< read-back differs (C12)                                        */
    I2C_BM_ERR_TIMEOUT,      /*!< max_cycles expired or CYCCNT stopped (C13)                    */
    I2C_BM_ERR_NACK,         /*!< address or data not acknowledged (STOP sent by hardware)      */
    I2C_BM_ERR_ARLO,         /*!< arbitration lost                                               */
    I2C_BM_ERR_BUS,          /*!< bus not free (BUSY), SCL held low (TIMEOUT), or not recovered  */
    I2C_BM_PENDING,          /*!< interrupt transfer running (I2cBm_Xfer_t.status)               */
    I2C_BM_ERR_HAL           /*!< i2c_hal only                                                   */
} I2cBm_Status_t;

/* Capabilities: one row per instance (rule 2). */
#define I2C_BM_KSEL_I2C123  0U   /* RCC_D2CCIP2R.I2C123SEL [13:12] */
#define I2C_BM_KSEL_I2C4    1U   /* RCC_D3CCIPR.I2C4SEL    [9:8]   */
typedef struct { uint8_t kernel_select; uint8_t dmamux; } I2cBm_Caps_t;   /* dmamux 1 or 2 (BDMA) */
bool           I2C_BM_IsValidInstance(const I2C_TypeDef *i2c);
I2cBm_Status_t I2C_BM_GetCaps(const I2C_TypeDef *i2c, I2cBm_Caps_t *out);

I2cBm_Status_t I2C_BM_EnableClock(I2C_TypeDef *i2c);     /* this core's RCC bank, read back */
I2cBm_Status_t I2C_BM_DisableClock(I2C_TypeDef *i2c);
I2cBm_Status_t I2C_BM_GetKernelSource(const I2C_TypeDef *i2c, uint32_t *field);   /* read only */
I2cBm_Status_t I2C_BM_SetFastModePlus(I2C_TypeDef *i2c, bool enable);   /* SYSCFG_PMCR.I2CxFMP, RMW
                                                                           under PRIMASK; SYSCFG clock
                                                                           must be on (else ERR_BUSY) */
/* Pure (C15). */
#define I2C_BM_SPEED_SM   0U     /* <= 100 kHz  */
#define I2C_BM_SPEED_FM   1U     /* <= 400 kHz  */
#define I2C_BM_SPEED_FMP  2U     /* <= 1000 kHz */
typedef struct
{
    uint32_t kernel_hz;
    uint32_t scl_hz;         /*!< 1..1 000 000; the mode follows from it                      */
    uint32_t rise_ns;        /*!< bus t_r, <= the mode's maximum (50:379-383)                 */
    uint32_t fall_ns;        /*!< bus t_f, idem                                                */
    bool     analog_filter;  /*!< ANFOFF = 0                                                   */
    uint32_t dnf;            /*!< 0..15                                                        */
} I2cBm_TimingIn_t;
typedef struct { uint32_t timingr; uint32_t mode; uint32_t scl_hz_model; } I2cBm_TimingOut_t;
I2cBm_Status_t I2C_BM_CalcTiming(const I2cBm_TimingIn_t *in, I2cBm_TimingOut_t *out);

/* Configuration. */
typedef struct
{
    I2C_TypeDef *i2c;
    uint32_t     timingr;        /*!< from CalcTiming (or the caller's own)                  */
    bool         analog_filter;
    uint32_t     dnf;            /*!< 0..15                                                   */
    uint32_t     timeout_scl_low;/*!< 0 = off; else TIMEOUTA + 1 (1..4096), TIDLE = 0, TIMOUTEN */
} I2cBm_Config_t;
I2cBm_Status_t I2C_BM_CheckConfig(const I2cBm_Config_t *cfg);   /* C6 order and states, no write */
I2cBm_Status_t I2C_BM_Init(const I2cBm_Config_t *cfg);          /* rule 3                         */
I2cBm_Status_t I2C_BM_Deinit(I2C_TypeDef *i2c);                 /* rule 4; no RCC write           */

/* Blocking transfers (7-bit address 0x08..0x77; n 0..65 535, 0 only for Write = address probe). */
I2cBm_Status_t I2C_BM_CheckTransfer(const I2C_TypeDef *i2c, uint32_t addr7, uint32_t ntx, uint32_t nrx);
I2cBm_Status_t I2C_BM_Write(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *tx, uint32_t n, uint32_t max_cycles);
I2cBm_Status_t I2C_BM_Read(I2C_TypeDef *i2c, uint32_t addr7, uint8_t *rx, uint32_t n, uint32_t max_cycles);
I2cBm_Status_t I2C_BM_WriteRead(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *tx, uint32_t ntx,
                                uint8_t *rx, uint32_t nrx, uint32_t max_cycles);   /* repeated start */

/* Interrupt transfers (state in the caller's struct, C7). */
typedef struct
{
    uint32_t                 addr7;
    const uint8_t           *tx;   uint32_t ntx;
    uint8_t                 *rx;   uint32_t nrx;
    uint32_t                 done_tx, done_rx;     /*!< driver-maintained */
    volatile I2cBm_Status_t  status;               /*!< I2C_BM_PENDING until the end */
} I2cBm_Xfer_t;
I2cBm_Status_t I2C_BM_StartIt(I2C_TypeDef *i2c, I2cBm_Xfer_t *x);
bool           I2C_BM_XferIrq(I2C_TypeDef *i2c, I2cBm_Xfer_t *x);   /* from both EV and ER vectors;
                                                                       true when x->status is final */
/* DMA (the caller owns streams/channels, 02_dma). */
I2cBm_Status_t I2C_BM_DmaStartWrite(I2C_TypeDef *i2c, uint32_t addr7, const uint8_t *buf, uint32_t n);
                         /* n >= 2: buf[0] preloaded in TXDR (2.19.6); the caller's TX stream sends
                            buf[1..n-1] (n - 1 items), started before this call */
I2cBm_Status_t I2C_BM_DmaStartRead(I2C_TypeDef *i2c, uint32_t addr7, uint32_t n);   /* RX stream first */
I2cBm_Status_t I2C_BM_WaitEnd(I2C_TypeDef *i2c, uint32_t max_cycles);   /* STOPF, NACK, ARLO, TIMEOUT */
I2cBm_Status_t I2C_BM_DmaEnd(I2C_TypeDef *i2c);   /* after the caller stopped its streams */
I2cBm_Status_t I2C_BM_GetDmaRequest(const I2C_TypeDef *i2c, bool tx, uint32_t *request_id);
I2cBm_Status_t I2C_BM_GetIRQn(const I2C_TypeDef *i2c, IRQn_Type *ev, IRQn_Type *er);

/* Bus recovery (rule 9): the caller moves the pins; the driver times the sequence. */
typedef struct
{
    void  (*scl_write)(void *ctx, bool high);   /*!< open drain: false = drive low, true = release */
    void  (*sda_write)(void *ctx, bool high);
    bool  (*scl_read)(void *ctx);
    bool  (*sda_read)(void *ctx);
    void   *ctx;
    uint32_t half_period_cycles;                /*!< CPU cycles, >= 1 (5 us at Sm is safe)        */
} I2cBm_Recovery_t;
I2cBm_Status_t I2C_BM_RecoverBus(const I2cBm_Recovery_t *r, uint32_t max_cycles, uint32_t *pulses);
uint32_t       I2C_BM_TakeErrors(I2C_TypeDef *i2c);   /* BERR/ARLO/OVR/TIMEOUT; one ICR write */
```

### Reglas de implementación

1. **C6 completo antes del primer acceso;** un rechazo no toca RCC, SYSCFG ni el periférico.
2. **Tabla de capacidades** `static const`, una fila por instancia con su cita: base (`I2C1..4_BASE`); RCC (`APB1LENR` bits 21/22/23, `APB4ENR` bit 7; 09:7224-7240, 7683-7688); campo de kernel; `SYSCFG_PMCR` I2C1FMP..I2C4FMP bits 0..3 (13:33-137); DMAMUX1 RX/TX 33/34, 35/36, 73/74 y DMAMUX2 13/14 (18:110-116, 235-236); IRQ EV/ER 31/32, 33/34, 72/73, 95/96 (`stm32h755xx.h`).
3. **`Init`** (50:404-425, 441-445): exige `PE = 0` o hace el reset software: `PE = 0` → read-back `PE = 0` (≥ 3 ciclos de APB) → `ANFOFF`, `DNF` → `TIMINGR` (máscara de reservados) → `TIMEOUTR` (con `TIMOUTEN = 0` al escribir `TIMEOUTA`, 50:2845) → `CR1` con `NOSTRETCH = 0` y todas las IE y DMA a 0 → `PE = 1` → read-back de `CR1`, `TIMINGR`, `TIMEOUTR`, `OAR1 = 0`, e `ISR` sin `BUSY`. Si `BUSY = 1` tras `PE = 1`: `ERR_BUS` (el bus no está libre; hace falta `RecoverBus`).
4. **`Deinit`:** `CR1` IE/DMA = 0 → `PE = 0` (suelta SCL/SDA) → read-back → `CR2 = 0`, `TIMINGR`, `TIMEOUTR`, `OAR1`, `OAR2` a reset → `ICR` = todos → read-back de la imagen de reset (`ISR = 0x1`). No escribe RCC ni SYSCFG.
5. **Transferencias** (50:517-559, 1072-1103, 1185-1194, 1367-1387): antes de `START`, `BUSY = 0` (si no, `ERR_BUS`). `NBYTES` y `RELOAD` por tramos de 255 (`TCR`); `AUTOEND = 1` en el último tramo, salvo el de escritura de `WriteRead` (`AUTOEND = 0`, `TC`, repeated start). En escritura, **el primer byte se escribe en `TXDR` antes de `START`** (2.19.6). Fin normal: `STOPF` → `ICR.STOPCF` → `CR2 = 0`. Ningún campo de `CR2` se cambia con `START = 1` (C12).
6. **Errores:** `NACKF` → el hardware manda STOP (50:1192-1194): esperar `STOPF` acotado, limpiar, `ERR_NACK`. `ARLO` → `ERR_ARLO` (el periférico pasa a esclavo, 50:2239-2249). `BERR` en maestro → **se borra y la transferencia sigue** (2.19.2). `TIMEOUT` → `ERR_BUS`. Toda espera acotada por `max_cycles` (C13); al vencer: `PE = 0` → read-back → `PE = 1` (reset software), `ERR_TIMEOUT`.
7. **IRQ:** `StartIt` escribe las IE después de configurar `CR2` y antes de `START`; `XferIrq` sirve `TXIS`/`RXNE`/`TCR`/`TC`/`STOPF`/`NACKF` y los errores, y deja `CR1` IE = 0 al terminar. Se llama desde los dos vectores (EV y ER).
8. **DMA** (50:2300-2337): el stream del llamador se inicializa antes de `START`; `TXDMAEN`/`RXDMAEN` antes de `START`; escritura con el primer byte precargado (regla 5); `WaitEnd` espera `STOPF` o error; `DmaEnd` borra `TXDMAEN`/`RXDMAEN` y los flags.
9. **`RecoverBus`** (la hoja de ruta: "9 pulsos de SCL por GPIO"; el RM no la describe): con la instancia en `PE = 0` y los pines ya en GPIO open-drain por el llamador. Si SDA está alto, no hace nada. Si no: hasta 9 pulsos de SCL (medio periodo `half_period_cycles`, con `CYCCNT` y salida por contador parado) hasta ver SDA alto; después STOP (SDA bajo con SCL alto, luego SDA alto); comprueba SCL y SDA altos. `pulses` = pulsos dados. SCL que no sube al soltarlo (otro lo retiene) → `ERR_BUS` sin seguir. Las funciones del llamador son datos (C1, C14); el driver no sabe qué pines son.
10. **`SetFastModePlus`:** RMW de `SYSCFG_PMCR` solo de su bit `I2CxFMP`, bajo PRIMASK, read-back.
11. **`i2c_hal`:** exige `USE_HAL_I2C_REGISTER_CALLBACKS == 0`; limitaciones a documentar: `BERR` aborta (IC1), las esperas internas de la HAL dependen del tick, `HAL_I2C_Init` llama a la MSP con `State = RESET`.
12. **Sin literales de proyecto** (C1): `grep -nE "BOARD_|GPIO[A-K]|DMA[12]_Stream|BDMA_Channel|NVIC_|100000000" i2c_*.[ch]` = 0.

### Reglas de cálculo

`CalcTiming` (C15), en enteros de 64 bits. Los tiempos del modo salen de las tablas 405 y 407 (50:356-384, 1043-1064), en ns:

| Modo | `scl_hz` | tLOW min | tHIGH min | tSU;DAT min | tHD;DAT min | tVD;DAT max | t\_r max | t\_f max | kernel mín. (2.19.1) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Sm | ≤ 100 000 | 4700 | 4000 | 250 | 0 | 3450 | 1000 | 300 | 4 MHz |
| Fm | ≤ 400 000 | 1300 | 600 | 100 | 0 | 900 | 300 | 300 | 10 MHz |
| Fm+ | ≤ 1 000 000 | 500 | 260 | 50 | 0 | 450 | 120 | 120 | 20 MHz |

1. `ERR_PARAM` si `kernel_hz = 0`, `scl_hz = 0` o > 1 000 000, `dnf > 15`, `out` o `in` nulos. `ERR_RANGE` si `rise_ns` o `fall_ns` superan el máximo del modo, o el kernel está por debajo del mínimo de 2.19.1.
2. `ticks(t_ns, p) = ceil(t_ns · kernel_hz / (10⁹ · (p + 1)))` y `ticks_down` con `floor`. `t_af` = 50 ns mín. / 80 ns máx. si `analog_filter` (DS12923 tabla 105), 0 si no.
3. Para `PRESC = p` de 0 a 15 (el primero que cumple, máxima resolución):
   - `SCLDEL = max(0, ticks(t_r + tSU;DAT, p) − 1)`; debe ser ≤ 15 (50:342).
   - `SDADEL_min = max(0, ceil((t_f + tHD;DAT − t_af,min)·k − (dnf + 3)) / (p + 1))` y `SDADEL_max = floor(((tVD;DAT − t_r − t_af,max)·k − (dnf + 4)) / (p + 1))`, con `k` en ciclos de kernel por ns (50:319-331); exige `SDADEL_min ≤ SDADEL_max` y ≤ 15; `SDADEL = SDADEL_min`.
   - `sync = ceil((t_r + t_f + 2·t_af,min)·kernel_hz / 10⁹) + 2·(dnf + 3)` ciclos de kernel (modelo de tSYNC1 + tSYNC2, 50:977-989).
   - `N = floor((floor(kernel_hz / scl_hz) − sync) / (p + 1))`; `Lmin = ticks(tLOW), Hmin = ticks(tHIGH)`; `L = max(Lmin, ceil(N · tLOW / (tLOW + tHIGH)))`; `H = max(Hmin, N − L)`; exige `L, H ≤ 256` y las dos condiciones de 50:159-171 (`t_kernel < (tLOW − t_filtros)/4`, `t_kernel < tHIGH`).
   - `TIMINGR = p << 28 | SCLDEL << 20 | SDADEL << 16 | (H − 1) << 8 | (L − 1)`; `scl_hz_model = floor(kernel_hz / (sync + (L + H)·(p + 1)))`.
4. Si ningún `p` cumple: `ERR_RANGE`. `scl_hz_model ≤ scl_hz` siempre (el bus nunca va más rápido de lo pedido).

## Contrato de API: i2c\_ll (F2) e i2c\_hal (F3)

**`i2c_ll`:** las operaciones con constantes de `stm32h7xx_ll_i2c.h` donde aplican (`LL_I2C_ANALOGFILTER_*`, `LL_I2C_ADDRSLAVE_7BIT`, …), traducidas y **validadas con `I2C_BM_Check*`** (una sola copia); escritura con inlines LL y `LL_I2C_WriteReg`; paridad de imagen bit a bit con BM (T2). Reloj, `Deinit`, `TakeErrors`, `GetIRQn`, `GetDmaRequest`, `CalcTiming`, `SetFastModePlus` y `RecoverBus` se usan de `i2c_bm`.

**`i2c_hal`:** envoltorio de la HAL sobre el `I2C_HandleTypeDef` del llamador: `I2C_HAL_Init(h, cfg)` (`I2C_BM_Deinit` primero y el handle a RESET, como `spi_hal`; después `HAL_I2C_Init` y `HAL_I2CEx_ConfigAnalogFilter/DigitalFilter`), `I2C_HAL_Write/Read/WriteRead(h, …, timeout_ms)` (`HAL_I2C_Master_Transmit/Receive`, `HAL_I2C_Mem_Read` o la secuencia `_Seq_`), `I2C_HAL_StartIt`, `I2C_HAL_StartDma`, `I2C_HAL_Wait` (con salida si el tick se para, lección m4) e `I2C_HAL_Deinit` (sin `HAL_I2C_DeInit`). Valida con `i2c_bm` antes de la HAL; read-back después; sin callbacks ni MSP; diferencias de imagen por diseño listadas con su línea de `stm32h7xx_hal_i2c.c`.

## Matriz RM0399 / ES0445

| Regla | Mecanismo | Prueba |
| --- | --- | --- |
| 50:2505-2529, 2808: filtros, `NOSTRETCH` y `TIMINGR` solo con `PE = 0` | Regla 3; `ERR_BUSY` | T1, T2 |
| 50:441-445: `PE = 0` al menos 3 ciclos de APB | Read-back de `PE = 0` antes de `PE = 1` | T2 (lectura) |
| 50:2620-2674: `CR2` no se cambia con `START = 1` | Regla 5 | Revisión |
| 50:1081-1082, 526-534: `NBYTES ≥ 255` con `RELOAD` | Regla 5 | T4 (256, 1000, 65 535) |
| 50:544-549, 1185-1188: repeated start con `AUTOEND = 0` y `TC` | `WriteRead` | T4, T5 (Sr en el bus) |
| 50:1192-1194: NACK → STOP automático | Regla 6 | T8 |
| 50:1793-1800: `TIMEOUTA` detecta SCL bajo | `timeout_scl_low` | T10(b) |
| 50:2300-2337: DMA antes de `START` | Regla 8 | T7 |
| 50:159-171, tablas 405/407 | `CalcTiming` | T3, T5 |
| ES0445 2.19.1: tSU;DAT < un ciclo de kernel | Kernel mínimo por modo en `CalcTiming`/`CheckConfig` (`ERR_RANGE`) | T1, T3 |
| ES0445 2.19.2: `BERR` espurio en maestro | Se borra y se sigue (regla 6) | Revisión (no se puede provocar) |
| ES0445 2.19.3: transferencia espuria al coincidir la dirección propia | Fuera: sin esclavo ni multimaestro; README | Revisión |
| ES0445 2.19.4: errata de documentación (`START` y `ADDRCF`) | README | — |
| ES0445 2.19.5: `OVR` no se pone en underrun (esclavo, `NOSTRETCH = 1`) | Fuera; README | — |
| ES0445 2.19.6: segundo byte perdido con APB/kernel entre 1,5 y 3 | Primer byte precargado antes de `START` (regla 5, regla 8) | T12 |
| ES0445 2.19.7: SDA bajo tras timeout SMBus en target | Fuera (SMBus); README | — |

## Banco de pruebas

Es el banco de `00_gpio` §8 y J1 de `06_spi`, **más el bus I2C** que coloca el responsable antes de F0. Valen S1–S14; este plan añade S15–S21.

### El bus I2C (lo coloca el responsable)

| Enlace | Desde | Hasta |
| --- | --- | --- |
| Nodo SCL (protoboard) | H755 CN7-2 (D15, **PB8**) | F334 CN5-10 (D15, PB8) — cable W1 |
| Nodo SDA (protoboard) | H755 CN7-4 (D14, **PB9**) | F334 CN5-9 (D14, PB9) — cable W2 |
| Pull-ups | 1,0 kΩ (1 %) de cada nodo a H755 3V3 (CN8-7) | — |
| GND | H755 CN8-11 | F334 CN5-7 — cable W3 (primero en conectarse, último en quitarse) |
| **J2** (desmontable) | nodo SCL | H755 CN9-19 (PF14: osciloscopio 1+, 22 kΩ a GND, 150 pF) |
| **J3** (desmontable) | nodo SDA | H755 CN9-21 (PF15: osciloscopio 2+, 22 kΩ a 3V3, 150 pF) |

- **H755:** I2C1 en PB8/PB9 **AF4** (DMA1/2) e I2C4 en PB8/PB9 **AF6** (BDMA), nunca a la vez. PF14/PF15 son **solo tomas**: entrada sin pull, nunca AF ni salida.
- **F334:** I2C1 en PB8/PB9 AF4, `FT_f`; mandada por su puerto COM (USART2 del ST-LINK/V2-1), sin líneas del AD2.
- **Estados del banco:**
  - **"I2C, con tomas"** (J2/J3 puestos): 100 y 400 kHz, con el AD2 midiendo. Con 1 kΩ y unos 250 pF, t\_r ≈ 225–270 ns (Fm ≤ 300 ns).
  - **"I2C, sin tomas"** (J2/J3 quitados): 1 MHz, sin AD2 (t\_r ≈ 50 ns). Sesión aparte (D3).
  - **"GPIO":** J2/J3 quitados; el banco de `00_gpio` queda como estaba (su T5/T6 fallarían con el bus conectado). El resto del bus I2C no toca ninguna línea de otro banco.
- **Antes de F0, el responsable confirma** (UM1724 y el esquema MB1136 no se pudieron descargar al preparar este plan; sus datos aquí son de memoria y de resúmenes): en la F334, que D15/D14 son PB8/PB9 en CN5-10/CN5-9 y GND en CN5-7, que `BOOT0` es un pin propio (no PB8) y que no hay pull-ups ni puentes de soldadura activos en PB8/PB9; en la H755, que SB39/SB62 siguen sin puentear. El implementador lo comprueba además en T0c (la F334 ve sus pines y el bus).

### Reglas nuevas S15–S21

| # | Regla | Dónde |
| --- | --- | --- |
| S15 | **Solo open-drain.** PB8/PB9 (H755 y F334) nunca push-pull ni con pull interno: entrada, AF de I2C con `OTYPER = 1`, o GPIO open-drain solo en los pasos de recuperación y de bus colgado. PF14/PF15 de la H755: solo entrada sin pull | `bench_claim()` de los dos núcleos lee `OTYPER`/`PUPDR` además de `MODER`/`AFR`; la F334 imprime `MODER`/`OTYPER`/`PUPDR` de PB8/PB9 en su banner y el host rechaza otra cosa |
| S16 | **Un maestro y un juego de pines.** En cada paso, a lo sumo una instancia I2C de la H755 en AF sobre PB8/PB9, de un solo núcleo (CM4 con I2C1 en los suyos, CM7 con I2C4 en los suyos). `bench_claim()` lee antes la instancia dueña: reloj en su banco y `PE = 0` (lección R2-3). Cambio de instancia: `PE = 0`, pines a entrada, ≥ 1 ms, y después la otra | Firmware |
| S17 | **Niveles antes de conducir** (R2-1). Con todo suelto: con tomas, el osciloscopio lee SDA ≥ 3,0 V y SCL/SDA = 0,956 ± 0,015; sin tomas, 1+ < 0,10 V y 2+ ≥ 3,0 V; la H755 y la F334 leen 1 en PB8/PB9 (en los dos estados). Se repite tras cada paso | Host |
| S18 | **Identidad de probes y puertos.** Cada llamada a `STM32_Programmer_CLI` lleva `sn=`; cada puerto COM se elige por el SN de su ST-LINK. `i2c_hw_check.py` se niega a correr si hay más ST-LINK que los dos esperados. Los otros bancos (`gpio`, `tim`, `spi`) se ejecutan **con el USB de la F334 desconectado** | Host, `tools/` (D7), procedimiento |
| S19 | **Estado del banco declarado.** El host recibe `--bench-state taps\|notaps` y lo comprueba (con tomas, el osciloscopio ve el bus; sin tomas, PF14 lee < 0,1 V por su 22 kΩ a GND). 1 MHz solo en "notaps" | Host |
| S20 | **Alimentación.** Las dos placas por USB del mismo PC; ningún riel entre placas (los pull-ups van al 3V3 de la H755); la F334 encendida antes que cualquier paso que toque el bus; flashear o resetear cualquiera de las dos solo con el host parado | Procedimiento, checklist |
| S21 | **Bus colgado acotado.** La F334 retiene SDA o SCL solo en open-drain, solo en un paso anunciado, con un límite de tiempo propio (≤ 500 ms) y liberación por su watchdog o temporizador; el host confirma la liberación | Firmware de la F334, host |

El AD2 no usa Digital Out ni conduce el bus: solo `HOST_REQ` (handshake) y sus dos canales de osciloscopio como entradas.

### Firmware de banco de la F334 (`tools/i2c_target_f334/`)

- Bare-metal con el CMSIS de F3 (subconjunto en `cmsis/`, con su licencia), compilado con el GCC del entorno (`build.ps1`), flasheado con `tools/flash.py --sn`. Banner por su puerto COM: `I2C_TARGET v1`, Rev e id de build (mismo método que `prj_01`), y `MODER/OTYPER/PUPDR` de PB8/PB9 (S15). Antes del primer flasheo, F0 guarda una copia de la flash original de la F334 (lección del banco: respaldar antes de sobrescribir).
- **Target en 0x42:** 256 registros, puntero autoincremental (el primer byte escrito fija el puntero), lectura desde el puntero; contenido inicial conocido (`reg[i] = i ^ 0xA5`).
- **Comandos armados de antemano** por el puerto COM (la latencia USB no es determinista): NACK en el byte k; stretching de t µs en la dirección o en cada byte; retener SDA (o SCL) abajo hasta n flancos de SCL o hasta t ms; Fm+ en sus pines; borrar contadores.
- **Informe tras cada transferencia:** START, Sr, STOP, bytes recibidos y enviados, NACK dados, CRC-32 de los recibidos, flancos de SCL contados mientras retiene SDA. Es el segundo oráculo, independiente de la H755.

### Herramientas

| Archivo | Contenido |
| --- | --- |
| `tools/flash.py`, `tools/monitor.py` | Solo se añade: `--sn` en `flash.py`, puerto COM por SN del ST-LINK en `monitor.py` (D7). Sin cambio de comportamiento para quien no lo use |
| `tools/ad2.py` | Solo se añade: disparo del osciloscopio por flanco de bajada (START) y un decodificador I2C sobre las dos señales analógicas (umbrales 0,3/0,7·VDD): START, Sr, STOP, bits, ACK/NACK, tLOW, tHIGH, tSU;DAT, tHD;STA, tSU;STO, tBUF, t\_r (30–70 %) y f\_SCL |
| `prj_01/tools/i2c_hw_check.py` | Como `spi_hw_check.py` (protocolo con `sync_ack_low` desde el principio, lección B1), más el puerto de la F334, sus comandos e informes, y `--bench-state` |

### Selftest

- Preset `selftest-i2c` (`I2C_DRIVER_SELFTEST=ON`, Debug), incompatible con `GPIO_`, `TIM_` y `SPI_DRIVER_SELFTEST`.
- Producción de `prj_01` con los tamaños de `2481396` (A16). `Status_Task` con 1024 palabras solo con la opción; la captura de entrega con ≥ 128 libres.
- Área SRAM4 en **0x3800D000** (4 KB, `_Static_assert`). Buffers de BDMA en `.d3_ram`, **solo desde CM7** (SP3).
- **Propiedad:** CM4 tiene I2C1; CM7 tiene I2C2, I2C3 e I2C4. Los pines del bus los toma en cada paso el núcleo de ese paso (S16).
- Handshake y veredicto como `06_spi` (protocolo con `sync_ack_low`); las ISR de prueba en `i2c_selftest.c`, prioridad 6.

## Fases

| Fase | Entregable | Gate |
| --- | --- | --- |
| **F0: banco** | **Requisito previo:** el bus colocado y confirmado por el responsable (tabla del bus y su última viñeta). Firmware de la F334 con su banner e informe; `tools/` con `--sn`; opción, preset, esqueleto del selftest; `i2c_hw_check.py` con S2–S8 y S15–S21 desde el primer commit. **T0a, T0b, T0c, T0d** | 8/8 y C9; producción igual a `2481396`; T0b–T0d en verde en la ejecución del auditor antes de cualquier prueba que mueva el bus; `git diff cc8aa2c -- 02_firmware_projects` vacío |
| **F1: `i2c_bm`** | Contrato de `i2c_bm`; T1–T15 y B1 en los dos núcleos y en los dos estados del banco | A1–A28. **API de `i2c_bm` congelada** |
| **F2: `i2c_ll`** | Contrato de `i2c_ll`; T1, T2, T4, T5, T6, T7 y T14 con LL | A1–A28; paridad LL = BM. **API de `i2c_ll` congelada** |
| **F3: `i2c_hal`** | Contrato de `i2c_hal`; `HAL_I2C_MODULE_ENABLED` en CM7 solo con la opción; T1, T2 (también contra la HAL cruda), T4, T5, T6, T7(b) y T14 con HAL en CM7; el tick sigue | A1–A28. **API de `i2c_hal` congelada** |
| **Cierre** | Implementador: `05_i2c/README.md` y la línea del catálogo. Responsable: `DRIVER_ROADMAP.md`, la línea de I2C4 de `02_dma/README.md` §9, la mención del bus en `00_gpio/README.md` §8, IC2, borrado de los documentos de proceso, merge | Auditoría de cierre |

## Plan de pruebas

Cada prueba tiene un control negativo (R1) y un oráculo que no es el driver. **Oráculos:** la F334 (lo que recibió y envió, con CRC-32), el osciloscopio del AD2 decodificado por el host (estado "con tomas"), `CYCCNT`, registros leídos directamente y la tabla del RM escrita a mano. **Datos:** xorshift32 con semilla impresa, CRC-32 y cobertura de bits, como `06_spi`.

**Tolerancias del AD2** (estado "con tomas"): osciloscopio a 20 MS/s (100 kHz) y 50 MS/s (400 kHz), con el buffer que dé `FDwfAnalogInBufferSizeInfo`. Los tiempos se juzgan contra los **mínimos del modo** (tablas 405/407) y contra la predicción de `CalcTiming` (± 2 muestras + 2 ciclos de kernel por flanco, más la desviación del reloj de la placa, N1). f\_SCL: ≤ la pedida y dentro de ± 5 % de `scl_hz_model`.

| ID | Fase / tier | Qué | Oráculo | Control negativo | Criterio |
| --- | --- | --- | --- | --- | --- |
| T0a | F0 y cada sesión | Las 13 líneas del AD2, como `06_spi` | DIO del AD2 e `IDR` | DIO9 con pull-down | Igual que `06_spi` |
| T0b | F0 | Guardas: S15 (`bench_claim` rechaza push-pull, pull interno, AF distinto de 4 en CM4 o de 6 en CM7, PF14/PF15 en AF o salida, la instancia dueña con `PE = 1` o sin reloj, el núcleo que no es el del paso); S16; y las de S4/S13/S14 heredadas | Fotos de `MODER/OTYPER/PUPDR/AFR` y de la instancia | Los casos válidos se aceptan | Rechazos exactos sin escritura |
| T0c | F0 y cada sesión | Bus (S17, S19): niveles en reposo en la H755 (`IDR` de PB8/PB9), en la F334 (su informe) y, con tomas, en el osciloscopio; la F334 retiene SDA (y luego SCL) 50 ms y las tres vistas lo ven | `IDR`, informe de la F334, osciloscopio | La retención misma: si las vistas no la ven, el bus no está | Niveles dentro de S17; las retenciones vistas por todos y liberadas |
| T0d | F0 y cada sesión | Identidad (S18): Rev e id de CM7, CM4, host y F334; SN de los dos ST-LINK; `MODER/OTYPER/PUPDR` de la F334 | Banners | Un SN o un Rev distinto aborta | Todo coincide |
| T1 | F1 BM, F2 LL, F3 HAL | Rechazos: `NULL`; puntero que no es I2C; `addr7` < 0x08 o > 0x77; `n` > 65 535; `Read` de 0 bytes; `dnf` 16; `timeout_scl_low` 4097; kernel bajo el mínimo de 2.19.1; t\_r por encima del modo; `Init` con `PE = 1` y `TIMINGR` distinto; transferencia con `BUSY = 1`; `DmaStartWrite` con `n` < 2 | Foto de los registros de la instancia, de los bancos de RCC y de `SYSCFG_PMCR` | Una llamada válida cambia la foto | Estado exacto y fotos idénticas |
| T2 | F1, F2, F3 | Imagen de `CR1`, `TIMINGR`, `TIMEOUTR`, `OAR1/2` y `CR2` en reposo, por instancia, sobre una matriz de `TIMINGR` × filtros × `timeout` | Campos del RM a mano; LL y HAL contra BM; HAL contra `HAL_I2C_Init` crudo (F3) | Imagen esperada alterada | Bits idénticos; diferencias de la HAL listadas |
| T3 | F1 | `CalcTiming`: ≥ 300 vectores (kernel 4, 8, 10, 16, 20, 48, 64, 100, 125 MHz × 10 kHz…1 MHz × t\_r/t\_f × filtros) | El host reimplementa las reglas de cálculo; además comprueba que cada salida cumple las tablas 405/407 y 50:159-171 | Esperado alterado (`SCLL + 1`) y otro con `SDADEL` fuera de su ventana | Coincidencia exacta; toda salida válida cumple el RM |
| T4 | F1 BM (CM4 I2C1, CM7 I2C4), F2 LL, F3 HAL | Con la F334 en 0x42, a 100 k, 400 k (con tomas) y 1 M (sin tomas): `Write` de 1, 2, 255, 256, 1000 y 65 535 bytes; `Read` de los mismos; `WriteRead` (puntero + lectura); sondeo de 0x42 y de 0x43 | La F334 (bytes y CRC-32, START/Sr/STOP) y el contenido leído frente al conocido | La F334 armada para corromper un byte (invierte uno de los que devuelve): el CRC lo detecta | 0 discrepancias; los recuentos de la F334 coinciden con los de la H755 |
| T5 | F1 BM, F2 LL, F3 HAL (con tomas) | Osciloscopio en cada velocidad: decodificación de las transferencias de T4 (cortas) y tiempos tLOW, tHIGH, tSU;DAT, tHD;STA, tSU;STO, tBUF, t\_r, f\_SCL | Decodificador del host | Cada captura decodificada con SCL y SDA intercambiados, o con umbral 0,5 V: discrepancias | Bytes, ACK/NACK, START/Sr/STOP exactos; tiempos ≥ mínimos del modo y dentro de la predicción de `CalcTiming` |
| T6 | F1, F2, F3 | Por interrupción: `Write`, `Read`, `WriteRead` de 1, 2, 256 y 1000 bytes | La F334 + CRC | IE a 0 a mano antes del primer evento: no hay progreso y el `max_cycles` del sondeo vence | 0 discrepancias; IE = 0 al terminar; 0 IRQ en 10 ms tras el fin |
| T7 | F1, F2, F3 | DMA: (a) I2C1 por DMA1/2 (CM4, `dma_bm`); (b) **I2C4 por BDMA** (CM7, `dma_hal`, buffers en `.d3_ram`): 2…4096 bytes, lectura y escritura | La F334 + CRC | `RXDMAEN = 0` en lectura: el buffer conserva su centinela y la espera vence | 0 discrepancias |
| T8 | F1 | NACK: (a) dirección 0x43 (sin target) → `ERR_NACK`, STOP en el bus, `BUSY = 0` después; (b) la F334 hace NACK en el byte k de una escritura → `ERR_NACK` y k bytes contados | La F334, el osciloscopio (STOP) | La transferencia a 0x42 sin NACK armado termina bien | Estado exacto; el bus queda libre; la transferencia siguiente funciona |
| T9 | F1 | Bus colgado y recuperación: la F334 retiene SDA hasta ver 5 flancos de SCL; la H755 encuentra el bus ocupado (`ERR_BUS` o `ERR_TIMEOUT` acotado); el selftest pone PB8/PB9 en GPIO open-drain y llama a `RecoverBus` → SDA libre, STOP, `pulses` = 5; después `Init` y una transferencia correcta | La F334 (flancos contados), `IDR`, osciloscopio | Sin `RecoverBus`, SDA sigue abajo hasta el límite de la F334 | Estados exactos; `pulses` = 5; transferencia posterior correcta |
| T10 | F1 | (a) Stretching: la F334 estira SCL 200 µs en la dirección y en cada byte → datos correctos y duración mayor en `CYCCNT`; (b) SCL retenido 50 ms con `timeout_scl_low` de ~10 ms → `ERR_BUS` por `TIMEOUT` y STOP | La F334, `CYCCNT`, osciloscopio | (b) sin `timeout_scl_low`: la espera acaba solo por `max_cycles` (`ERR_TIMEOUT`) | Estados exactos; el bus libre al final |
| T11 | F1 | `ARLO` | — | — | **No ejercitado** (un solo maestro); revisión |
| T12 | F1 | 2.19.6: con `I2C123SEL = HSI` (64 MHz; APB/kernel ≈ 1,56, dentro de la ventana) escrito y restaurado por el selftest con read-back: 1000 escrituras de 2 bytes por sondeo y por DMA | La F334 | A mano, sin precargar el primer byte: se registra si la transmisión se detiene (decisión 6 de `06_spi`: "no reproducido" si no) | 0 detenciones con el driver |
| T13 | F1 | Bancos de RCC: CM4 `DisableClock(I2C2)` mientras CM7 lo tiene habilitado | Registros de RCC y de I2C2 leídos por CM7 | CM7 deshabilita: se para | Como `06_spi` T13 |
| T14 | F1, F2, F3 | `Deinit` tras cada prueba: imagen de reset y bus en reposo (S17) | Tabla de reset del RM a mano | Instancia sin `Deinit`: detectada | Imagen exacta |
| T15 | F1 | Hechos de silicio sin pines (lección E2): I2C2 sin pines en AF → `Write` termina en `ERR_TIMEOUT` o `ERR_BUS` acotado, registros tras el aborto e imagen de `Deinit`; control: I2C4 con sus pines en AF termina bien | Registros, `CYCCNT` | El control | Estado exacto y acotado |
| B1 | F1, F2, F3 | `.text/.data/.bss` (`.map`); ciclos de `Init`, `Write` de 1 byte, `XferIrq` | `.map`, DWT | — | Solo registro; `.data = .bss = 0` |

### No ejercitado por diseño (va al README)

- `ARLO` y multimaestro; esclavo, 10 bits, SMBus; 2.19.2 (no se puede provocar), 2.19.3, 2.19.5, 2.19.7.
- I2C2 e I2C3 sin pines en el banco: solo imagen (T2), bancos (T13) y T15. I2C1 con DMA solo desde CM4; I2C4 con BDMA solo desde CM7.
- Tiempos medidos a 1 MHz (D3): 1 MHz se valida por datos y duración.
- "Sin pull-up" literal (D5). Silicio distinto de rev V; árboles de reloj distintos del de `prj_01`, salvo `CalcTiming` (T3).

## Protocolo de auditoría

Igual que el de `06_spi` (A1–A28, clases B/M/m/Nota), con estos cambios:

- **A18:** el auditor ejecuta las **dos** sesiones de la fase (con y sin tomas), con su propio flasheo de la H755 y de la F334, en frío y sin depurador.
- **A21:** cada captura lleva la identidad de la H755 (CM7, CM4, host) y de la F334 (banner, Rev, id, SN).
- **A23:** el alcance permitido es `01_shared_drivers/05_i2c`, `01_shared_drivers/CMakeLists.txt`, `03_firmware_templates/prj_01_template_dualcore`, `tools/i2c_target_f334/` y solo añadidos en `tools/flash.py`, `tools/monitor.py` y `tools/ad2.py`.
- **A27:** S1–S21; además, la lectura del firmware de la F334 (solo open-drain, retenciones acotadas).

## README de cierre

Como `06_spi` (11 secciones, ≤ 600 líneas, formato de `03_tim`), con el diagrama del bus (H755, F334, pull-ups, J2/J3, tomas del osciloscopio), los dos estados del banco, S15–S21, el uso de la F334 y cómo reproducirlo, `CalcTiming` y la guía de pines open-drain, y las lecciones del README de `06_spi`: cada evidencia atribuida a la función que la produjo, umbrales del RM citados exactos, qué referencia usa cada prueba, todo lo no ejercitado de los informes, los controles que no se reproducen siempre, en qué núcleos compila la HAL, y la forma de recuperar los documentos de proceso.

## Riesgos

| Riesgo | Mitigación |
| --- | --- |
| Un pin del bus en push-pull contra otro que tira abajo (sin resistencia en serie) | S15 en los dos núcleos y en la F334; T0b |
| Dos instancias de la H755 en AF a la vez | S16; T0b |
| Las herramientas flashean o resetean la placa equivocada | D7, S18; los otros bancos con la F334 desconectada |
| El banco de `00_gpio` alterado por el bus | J2/J3 desmontables; estado "GPIO" (S19) |
| Bus colgado sin salida | S21 (límite propio en la F334), C13 en el driver |
| 1 MHz fuera de t\_r con las tomas | D3: sesión sin tomas |
| `SYSCFG_PMCR` compartido con `08_eth` | RMW de un solo bit bajo PRIMASK (C10, regla 10) |
| Errata 2.19.6 en otro árbol de reloj | Precarga del primer byte siempre (regla 5); T12 lo provoca |
| La HAL aborta con `BERR` (IC1) | Limitación documentada; BM y LL lo tratan bien |

## Decisiones abiertas

Cada una con su opción por defecto.

1. **Retorno:** `I2cBm_Status_t` en los tres tiers.
2. **Pull-ups:** 1,0 kΩ al 3V3 de la H755 (Fm cumple con las tomas; Sm también).
3. **1 MHz:** sesión sin tomas (D3). Alternativa: un enlace desmontable en serie con los 150 pF, si el responsable confirma que en el banco se pueden quitar.
4. **Propiedad:** CM4 I2C1; CM7 I2C2, I2C3 e I2C4.
5. **Recuperación del bus:** funciones del llamador en `I2cBm_Recovery_t` (regla 9). Alternativa: dejarla fuera del driver como ejemplo del README (la hoja de ruta la pide demostrada).
6. **Controles de erratas (T12):** "no reproducido" si no falla, como `06_spi`.
7. **HAL I2C en CM7:** solo bajo `I2C_DRIVER_SELFTEST`.
8. **Firmware de la F334:** en `tools/i2c_target_f334/`, con el subconjunto del CMSIS de F3 dentro (licencia Apache-2.0). Se queda en el repositorio tras el cierre como instrumento del banco.
9. **Documentos de proceso al cierre:** se borran, como en `06_spi`.
