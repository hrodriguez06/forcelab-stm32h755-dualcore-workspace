# Plan de implementación de `03_tim` F4: TIM3 (y TIM7 de selftest) de `prj_03` y `prj_04` por el driver

Oct 13, 2026 · @Hector

Migración de los temporizadores de disparo del ADC y de los selftests de hardware de los proyectos dual-core `prj_04_adc_dma_bm_eth` y `prj_03_adc_dma_eth` a los drivers compartidos de `01_shared_drivers/03_tim` (`tim_bm` y `tim_hal`).

Hasta ahora, ambos proyectos programan TIM3 y TIM7 escribiendo directamente en los registros del periférico o invocando la HAL de ST directamente en su código de aplicación. Esta fase sustituye esas escrituras por las APIs formales del catálogo de drivers compartidos:
- En `prj_04_adc_dma_bm_eth`, cuyo Cortex-M4 opera bajo arquitectura bare-metal pura, se integra `tim_bm` (`stm32h7_driver_tim_bm`).
- En `prj_03_adc_dma_eth`, cuyo Cortex-M4 opera bajo arquitectura HAL, se integra `tim_hal` (`stm32h7_driver_tim_hal`), consumiendo su API con la estructura de control nativa del driver.

Esta fase constituye la **primera validación funcional en hardware de dos hitos pendientes** del driver (`03_tim/README.md` §9.5):
1. El disparo por hardware **TRGO → ADC/DMA** sobre silicio real.
2. El funcionamiento del tier **`tim_hal` ejecutándose en el núcleo Cortex-M4 (CPU2)**.

No es un driver nuevo y **ninguna firma pública de `01_shared_drivers/03_tim/*.h` cambia** (regla de catálogo 2: API congelada).

Una sola fase de implementación (F1) y un cierre formal. La fase pasa una auditoría independiente con ejecución propia del auditor sobre la placa física. Las reglas para el implementador se recogen en [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md).

Base: `04fb865` de `main`, árbol limpio, rama `driver/tim-f4`.

**Decisiones abiertas del plan y opciones por defecto:**
1. **Tier por proyecto:** `tim_bm` en `prj_04` (arquitectura bare-metal); `tim_hal` en `prj_03` (arquitectura HAL). Valida ambos tiers y resuelve §9.5.
2. **Alcance de los selftests:** entrar tanto el TIM7 de `dma_selftest.c` (T4/T5 de `02_dma`) como los TIM3 de `adc_selftest_hw.c` en ambos proyectos, erradicando todas las escrituras ad-hoc a registros de temporizadores.
3. **Preservación del ritmo del ADC:** secuencia estricta de arranque (InitTimeBase con ARPE=0 antes de ConfigMaster; arranque del contador `TIM_BM_Start` únicamente tras el armado de ADC y DMA); oráculo de medida en `query_adc.py` centrado en 506,6 kHz ([502..509 kHz], ST-LINK MCO +1,327 %).
4. **Documentos de proceso al cierre:** se borran al cierre, como en `00_gpio` F1b/F1c, `05_i2c` y `08_eth`.

**Reglas de verificación normativas desde el primer día (heredadas de `00_gpio` F1b y F1c):**
- **Comprobaciones:** cada comprobación puede fallar de verdad y cuenta en el veredicto;
- **Controles negativos:** cada verificación algorítmica incluye un control negativo real que falla con datos no conformes;
- **Identidad de la imagen:** cada captura de hardware prueba qué imagen corre (flasheo con verificación previa y origen de símbolos derivado por `nm`);
- **Árbol limpio:** `git status --porcelain` limpio; cualquier aviso o árbol sucio invalida la prueba;
- **Sin silencios:** un fallo de herramienta, archivo o preset faltante es FAIL, nunca "sin hallazgos";
- **Capturas UTF-8:** generadas en una sola ejecución sin edición manual;
- **Banco y seguridad remota (G-S1 a G-S7):** no tocar configuración de alimentación, reloj base ni option bytes; pines verificados contra el banco.

---

## Encargo

| ID | Hoy (`04fb865`) | Qué cambia |
| --- | --- | --- |
| **H1** | `prj_04`: CM4 `adc_acquisition.c` configura TIM3 a mano por registros (`RCC->APB1LENR`, `TIM3->CR1`, `PSC`, `ARR`, `CR2.MMS`; l. 58-79) y arranca con `TIM3->CR1 |= TIM_CR1_CEN` (l. 168). CM4 no enlaza `03_tim` | `adc_acquisition.c` migra a `tim_bm`: `TIM_BM_EnableClock(TIM3)`, `TIM_BM_InitTimeBase(&cfg)` (PSC 0, ARR 399, up), `TIM_BM_ConfigMaster(TIM3, TIM_BM_MMS_UPDATE, false)` y `TIM_BM_Start(TIM3)`. `prj_04/CM4/CMakeLists.txt` enlaza `stm32h7_driver_tim_bm` |
| **H2** | `prj_03`: CM4 `adc_acquisition.c` configura TIM3 con la HAL directa (`HAL_TIM_Base_Init(&s_htim3)`, `HAL_TIMEx_MasterConfigSynchronization`; l. 54-84) y arranca con `HAL_TIM_Base_Start` (l. 146). CM4 no enlaza `03_tim` | `adc_acquisition.c` migra a `tim_hal`: `TIM_BM_EnableClock(TIM3)`, `TIM_HAL_InitTimeBase(&s_htim3, &cfg)`, `TIM_HAL_ConfigMaster(&s_htim3, TIM_BM_MMS_UPDATE, false)` y `TIM_BM_Start(TIM3)` (o `HAL_TIM_Base_Start`). `prj_03/CM4/CMakeLists.txt` enlaza `stm32h7_driver_tim_hal` |
| **H3** | `prj_04` selftests: CM4 `adc_selftest_hw.c` (`Tim3_Prepare`, `Tim3_Run`, `Tim3_Halt`; l. 140-166) y `dma_selftest.c` (T4, l. 1001-1050: TIM7 a 10 kHz por registros) escriben TIM3 y TIM7 a mano | Ambos migran a `tim_bm`: `adc_selftest_hw.c` usa `TIM_BM_EnableClock`, `TIM_BM_InitTimeBase`, `TIM_BM_ConfigMaster`, `TIM_BM_Start` y `TIM_BM_Stop`. `dma_selftest.c` (T4) usa `TIM_BM_EnableClock`, `TIM_BM_InitTimeBase`, `TIM_BM_ConfigMaster`, `TIM_BM_EnableDmaRequest`, `TIM_BM_Start`, `TIM_BM_Stop` y `TIM_BM_Deinit` |
| **H4** | `prj_03` selftest: CM4 `adc_selftest_hw.c` (`Tim3_Prepare`, `Tim3_Run`, `Tim3_Halt`; l. 144-167) escribe TIM3 a mano por registros | Migra a `tim_bm` (`TIM_BM_EnableClock`, `TIM_BM_InitTimeBase`, `TIM_BM_ConfigMaster`, `TIM_BM_Start`, `TIM_BM_Stop`) |
| **H5** | Herramienta de pruebas de la fase (`tools/tim_f4_checks.py`) | Nueva herramienta que ejecuta y documenta T1–T7 bajo los estándares de F1b/F1c: lectura SWD en caliente de registros de TIM3, medida de frecuencia y OVR con `query_adc.py`, regresión de red y 20 resets |

**Fuera de este plan:**
- **TIM6 en `prj_04 CM7` (`CM7/Core/Src/stm32h7xx_hal_timebase_tim.c`):** actúa como base de tiempos de 1 ms de la HAL para el planificador FreeRTOS (`HAL_InitTick`, `TIM6_DAC_IRQHandler`). No se modifica (RM0399 §44, regla de proyecto y README §9.5).
- **`prj_01_template_dualcore` en producción:** no enlaza ningún tier de temporizador (solo compila el preset `selftest-tim`, que se usa como regresión T7).
- **`prj_02_eth_lwip_dualcore`:** no contiene ADC ni temporizadores de disparo.
- **`prj_eth_lwip_node`:** firmware mono-núcleo de pruebas con copia privada de drivers.
- **Nuevas firmas o cambios en `01_shared_drivers/03_tim/*.h`:** las cabeceras están congeladas por contrato de catálogo.

---

## Fuentes normativas

- `01_shared_drivers/03_tim/README.md` completo:
  - §1.1 (matriz de funciones entre BM, LL y HAL);
  - §2 (códigos de estado `TimBm_Status_t` y firmas);
  - §3 (guía de configuración y reglas de cálculo de base de tiempos);
  - §4 (contrato del proyecto: pertenencia de cada instancia a un solo núcleo, sin lock entre núcleos);
  - §5 (reglas que impone el driver: `CEN = 0` para `InitTimeBase`, secciones críticas bajo PRIMASK);
  - §6.1 (tabla de capacidades de silicio: TIM3 general de 16 bits, 4 canales, MMS máx 7; TIM7 básico de 16 bits, MMS máx 2);
  - §9.5 (hitos no ejercitados: TRGO → ADC/DAC, `tim_hal` en CM4);
  - §10.3 (comportamiento de `tim_hal` frente a `tim_bm` por diseño);
  - §10.5 (desviación del oscilador HSE ST-LINK MCO: +1,327 %, ~506,6 kHz real para 500 kHz nominal);
  - §11 (consumidores pendientes: `prj_04` TIM3 y TIM7, y `prj_03` TIM3);
  - §12 (política de congelación de API).
- Cabeceras del driver: `tim_bm.h`, `tim_hal.h`.
- `01_shared_drivers/02_dma/README.md` (T4/T5 de DMA pacing con TIM7_UP, request 70).
- `00_gpio/README.md` §3 (regla D: habilitación de relojes APB1LENR desde CM7 para evitar carreras) y §13.1.
- RM0399:
  - §40 (temporizadores de propósito general TIM2 a TIM5);
  - §44 (temporizadores básicos TIM6 y TIM7);
  - §24 (disparos externos del ADC1/2/3 por TRGO).

---

## Lo que existe hoy (`04fb865`)

| Elemento | Estado |
| --- | --- |
| Escritores de TIM3 en `prj_04` | CM4: `TIM3_Trigger_Init` en `CM4/App/Src/adc_acquisition.c` escribe `RCC->APB1LENR`, `TIM3->CR1 = 0`, `PSC = 0`, `ARR = 399`, `CR2.MMS = 010`, y luego `TIM3->CR1 |= TIM_CR1_CEN`. Preset `selftest-adc`: `adc_selftest_hw.c` manipula directamente `TIM3->CR1`, `PSC`, `ARR`, `CR2`, `CNT`, `SR`. CM7: solo habilita `__HAL_RCC_TIM3_CLK_ENABLE()` en `main.c:203` (Regla D) |
| Escritores de TIM7 en `prj_04` | CM4: `CM4/App/Src/dma_selftest.c` (T4, presets de selftest) escribe `TIM7->CR1`, `PSC = 199`, `ARR = 99`, `CR2.MMS = 010`, `DIER.UDE = 1` y controla `CEN` a mano |
| Escritores de TIM3 en `prj_03` | CM4: `TIM3_Trigger_Init` en `CM4/App/Src/adc_acquisition.c` configura con `HAL_TIM_Base_Init(&s_htim3)` y `HAL_TIMEx_MasterConfigSynchronization`, y arranca con `HAL_TIM_Base_Start`. Preset `selftest`: `adc_selftest_hw.c` manipula `TIM3` a mano por registros exactamente igual que `prj_04` |
| TIM6 en `prj_04 CM7` | `CM7/Core/Src/stm32h7xx_hal_timebase_tim.c`: timebase de 1 kHz de la HAL para FreeRTOS. Fuera de alcance |
| Enlace de drivers en CM4 | `prj_04`: enlaza `stm32h7_driver_adc_bm`, `dma`, `gpio`, `hsem_bm`. No enlaza `03_tim`.<br>`prj_03`: enlaza `stm32h7_driver_adc`, `hsem_bm`. No enlaza `03_tim` |
| Presets de CMake | `prj_04`: `debug`, `release`, `selftest`, `selftest-adc`, `selftest-uart`, `selftest-hsem`, `selftest-gpio`.<br>`prj_03`: `debug`, `release`, `selftest` |
| Cadencia y configuración de TIM3 | APB1 timer clock = 200 MHz. PSC = 0, ARR = 399. Periodo nominal = 400 ticks @ 200 MHz = 2,000 µs (500.000 Hz). TRGO = MMS 010 (Update). En la placa real, el reloj de entrada tiene un offset de +1,327 %, arrojando ~506,6 kHz |
| Telemetría del ADC | `query_adc.py` reporta muestras por segundo (`Measured Rate`), `OVR` (`half_overrun_count` en `prj_03`; `adc_overrun_count` en `prj_04`), y `Samples` (creciente) |
| Pines en producción | Sin pines de TIM conectados al banco (TIM3 opera enteramente como trigger interno TRGO conectado por matriz de interconexión interna al ADC1). Ningún pin de producción es línea del banco AD2 |

---

## Diseño

### 1. Tier por proyecto (Decisión 1)
- **`prj_04_adc_dma_bm_eth`:** su CM4 es estrictamente bare-metal (regla C1). Enlaza `stm32h7_driver_tim_bm` en `CM4/CMakeLists.txt`. Toda la configuración se realiza mediante estructuras `TimBm_TimeBaseConfig_t` y funciones `TIM_BM_*`.
- **`prj_03_adc_dma_eth`:** su CM4 utiliza el tier HAL (`USE_HAL_DRIVER`, `adc_hal`). Enlaza `stm32h7_driver_tim_hal` en `CM4/CMakeLists.txt` (que a su vez arrastra `tim_bm`). Se mantiene la instancia de `TIM_HandleTypeDef s_htim3` en `adc_acquisition.c` y se inicializa mediante `TIM_HAL_InitTimeBase(&s_htim3, &cfg)` y `TIM_HAL_ConfigMaster(&s_htim3, TIM_BM_MMS_UPDATE, false)`. Esto valida formalmente `tim_hal` en CM4 (§9.5).

### 2. Configuración en producción (`adc_acquisition.c`)
Tanto en `prj_04` como en `prj_03`:
```c
static bool TIM3_Trigger_Init(void)
{
    /* Habilitar reloj en el banco RCC del propio núcleo CM4 */
    if (TIM_BM_EnableClock(TIM3) != TIM_BM_OK)
    {
        return false;
    }

    const TimBm_TimeBaseConfig_t cfg = {
        .tim               = TIM3,
        .psc               = 0U,
        .arr               = 399U,
        .count_mode        = TIM_BM_COUNT_UP,
        .repetition        = 0U,
        .arr_preload       = false,  /* ARPE = 0: actualización inmediata */
        .one_pulse         = false,
        .urs_overflow_only = false
    };

    /* Inicializar base de tiempos (genera UG bajo MMS=000, sin TRGO espurio) */
#if defined(USE_HAL_DRIVER) && !defined(ADC_BM_H) // prj_03
    if (TIM_HAL_InitTimeBase(&s_htim3, &cfg) != TIM_BM_OK)
    {
        return false;
    }
    if (TIM_HAL_ConfigMaster(&s_htim3, TIM_BM_MMS_UPDATE, false) != TIM_BM_OK)
    {
        return false;
    }
#else // prj_04
    if (TIM_BM_InitTimeBase(&cfg) != TIM_BM_OK)
    {
        return false;
    }
    if (TIM_BM_ConfigMaster(TIM3, TIM_BM_MMS_UPDATE, false) != TIM_BM_OK)
    {
        return false;
    }
#endif

    return true;
}
```

### 3. Preservación estricta del ritmo del ADC (Decisión 3)
Para garantizar que el DMA y el ADC capturen sin pérdidas ni desbordamientos desde la primera muestra:
1. **Precarga deshabilitada (`arr_preload = false`):** `ARPE = 0` coincide exactamente con la configuración histórica de ambos proyectos.
2. **Orden de inicialización libre de disparos espurios:**
   - `InitTimeBase` se llama **antes** de `ConfigMaster`. La función `InitTimeBase` genera por diseño un pulso `UG` para transferir PSC y ARR a los registros de sombra. Al encontrarse todavía `CR2.MMS = 000` (Reset), este pulso `UG` **no** se propaga como TRGO al periférico ADC.
   - A continuación, `ConfigMaster` configura `CR2.MMS = 010` (Update). A partir de este instante, solo los eventos de update periódicos enviarán un pulso TRGO.
3. **Arranque tardío del temporizador:**
   - En `ADC_Acquisition_Init()`, el temporizador se arranca como **último paso**, una vez que el flujo DMA y las conversiones del ADC han sido activados (`DMA_BM_Start` + `ADC_BM_StartConversion` en `prj_04`; `ADC_HAL_Start_DMA` en `prj_03`).
   - El arranque se realiza mediante `TIM_BM_Start(TIM3)` (o `HAL_TIM_Base_Start(&s_htim3)` en `prj_03`, que escribe `CR1.CEN = 1`).
   - El primer flanco de TRGO ocurrirá con certeza 400 ciclos de 200 MHz (2,0 µs) tras `TIM_BM_Start`, con el ADC y el DMA ya en escucha activa. Esto garantiza que `OVR = 0` desde el arranque.

### 4. Selftests de hardware (Decisión 2)
- **`adc_selftest_hw.c` (en `prj_03` y `prj_04`):**
  - `Tim3_Prepare(rate_hz)` pasa a invocar `TIM_BM_EnableClock(TIM3)`, `TIM_BM_InitTimeBase` con `arr = (STHW_TIM_CLK_HZ / rate_hz) - 1U`, y `TIM_BM_ConfigMaster(TIM3, TIM_BM_MMS_UPDATE, false)`.
  - `Tim3_Run()` pasa a `TIM_BM_Start(TIM3)`.
  - `Tim3_Halt()` pasa a `TIM_BM_Stop(TIM3)`.
- **`dma_selftest.c` (`prj_04`):**
  - En la prueba T4 (cadencia por TIM7 a 10 kHz):
    - Sustituir `__HAL_RCC_TIM7_CLK_ENABLE()` por `TIM_BM_EnableClock(TIM7)`.
    - Base de tiempos (PSC 199, ARR 99) configurada mediante `TIM_BM_InitTimeBase`.
    - `TIM_BM_ConfigMaster(TIM7, TIM_BM_MMS_UPDATE, false)`.
    - `TIM_BM_EnableDmaRequest(TIM7, TIM_BM_DMA_UPDATE)`.
    - `TIM_BM_Start(TIM7)` para arrancar; `TIM_BM_Stop(TIM7)` y `TIM_BM_Deinit(TIM7)` para parar y limpiar al terminar la prueba.
  - En la prueba T5 (doble buffer): solo utiliza la dirección física `(uint32_t)&(TIM7->CNT)` y el request ID 70 (`BOARD_SELFTEST_TIM_REQUEST`); no requiere modificaciones funcionales.

### 5. Herramienta de verificación (`tools/tim_f4_checks.py`)
Se desarrolla una herramienta dedicada que automatiza la batería completa de pruebas:
- Admite `--project {prj_04,prj_03}` y `--test {t1,t2,t3,t4,t5,t6a,t7,all}`.
- Comprueba que el árbol de trabajo esté rigurosamente limpio (`git status --porcelain`).
- Inspecciona por SWD (`mode=HOTPLUG`) sin detener la ejecución de los núcleos.
- Deriva todas las direcciones y registros de los ficheros ELF compilados y cabeceras oficiales.
- Incorpora controles negativos reales en cada oráculo.

---

## Archivos

| Ruta | Cambio |
| --- | --- |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/CM4/App/Src/adc_acquisition.c` | H1: migración de TIM3 a `tim_bm` |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/CM4/CMakeLists.txt` | H1: enlazar `stm32h7_driver_tim_bm` |
| `02_firmware_projects/prj_03_adc_dma_eth/CM4/App/Src/adc_acquisition.c` | H2: migración de TIM3 a `tim_hal` |
| `02_firmware_projects/prj_03_adc_dma_eth/CM4/CMakeLists.txt` | H2: enlazar `stm32h7_driver_tim_hal` |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/CM4/App/Src/adc_selftest_hw.c` | H3: migración de TIM3 a `tim_bm` |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/CM4/App/Src/dma_selftest.c` | H3: migración de TIM7 a `tim_bm` en T4 |
| `02_firmware_projects/prj_03_adc_dma_eth/CM4/App/Src/adc_selftest_hw.c` | H4: migración de TIM3 a `tim_bm` |
| `tools/tim_f4_checks.py` (nuevo) | H5: arnés de verificación automatizado para T1–T7 |
| `prj_04/docs/tim_f4_*`, `prj_03/docs/tim_f4_*` | Capturas UTF-8 completas de las ejecuciones en placa |
| `01_shared_drivers/03_tim/README.md`, `prj_03/README.md`, `prj_04/README.md` | Solo en el cierre: actualización de consumidores y validación |

---

## Pruebas

**Oráculos:**
1. **El enlazador (`arm-none-eabi-nm`):** presencia de símbolos de `tim_bm` / `tim_hal` en los ejecutables CM4.
2. **El preprocesador / análisis estático:** ausencia total de escrituras ad-hoc a registros de TIM (`TIM3->`, `TIM7->`) en el código migrado.
3. **Inspección SWD en caliente (`mode=HOTPLUG`):** lectura de los registros hardware de TIM3 (`CR1`, `CR2`, `PSC`, `ARR`, `DIER`, `SMCR`) contrastada contra una tabla esperada escrita a mano a partir de RM0399.
4. **Telemetría de la aplicación (`query_adc.py`):** cálculo de tasa de muestreo derivada de `ds / dt` y verificación de `OVR == 0`.
5. **Comprobación de red (`net_check.py` + `monitor.py`):** integridad de la pila de red y ausencia de pérdidas de paquetes.

| ID | Qué | Oráculo | Control negativo | Criterio de éxito |
| --- | --- | --- | --- | --- |
| **T1** | Compilación de la matriz completa:<br>• `build_all.ps1 -Clean` (8/8 builds: debug y release de prj_01, prj_02, prj_03, prj_04);<br>• Presets de selftest: `selftest-adc` en `prj_04`, `selftest` en `prj_03`, `selftest-tim` en `prj_01`;<br>• Builds con `-Werror` | Código de retorno de cmake y ausencia de avisos | — | Todos los presets compilan con 0 errores y 0 avisos. Salida única en UTF-8 |
| **T2a** | Auditoría de símbolos enlazados con `nm` en las ELF de CM4 (debug y release de `prj_03` y `prj_04`) | Símbolos de `tim_bm` presentes en `prj_04`; símbolos de `tim_hal` y `tim_bm` presentes en `prj_03` | Las ELF del commit de control `04fb865`: los símbolos del driver no aparecen en CM4 y el criterio **falla** | Se cumple en las 4 ELF evaluadas; el control falla en las 4 |
| **T2b** | Escaneo estático de accesos directos a registros de TIM en las unidades de compilación de `compile_commands.json` | 0 accesos directos a `TIM3->` o `TIM7->` en los archivos migrados | El código de control en `04fb865`: detecta las escrituras manuales en `adc_acquisition.c`, `adc_selftest_hw.c` y `dma_selftest.c` | 0 escrituras manuales fuera del propio driver; el control detecta todas las escrituras preexistentes |
| **T3** | Inspección en caliente por SWD de los registros de TIM3 con la imagen `debug` corriendo (en `prj_04` y `prj_03`):<br>• `CR1` (`0x40000400`)<br>• `CR2` (`0x40000404`)<br>• `SMCR` (`0x40000408`)<br>• `DIER` (`0x4000040C`)<br>• `PSC` (`0x40000428`)<br>• `ARR` (`0x4000042C`) | Tabla escrita a mano desde RM0399:<br>• `CR1 = 0x0001` (CEN=1, ARPE=0, URS=0, DIR=0);<br>• `CR2 = 0x0020` (MMS = 010 update);<br>• `SMCR = 0x0000`;<br>• `DIER = 0x0000`;<br>• `PSC = 0x0000`;<br>• `ARR = 0x018F` (399 decimal) | Se evalúa contra una tabla alterada deliberadamente (ej. `ARR = 0x00FF` o `CR2 = 0x0000`): la comparación **falla** explícitamente | Los 6 registros coinciden bit a bit en ambas imágenes; el control negativo detecta discrepancia |
| **T4** | Verificación del ritmo de adquisición y ausencia de overrun con `query_adc.py --json --count 20` (en `prj_04` y `prj_03`) | • Tasa medida (`ds / dt`) en la ventana `[502.000, 509.000] Hz` (centrada en 506,6 kHz debido al offset de +1,327 % del HSE ST-LINK);<br>• `OVR == 0` en todas las lecturas;<br>• `Samples` estrictamente creciente | Un test con ventana nominal desplazada (ej. [490..500 kHz]) o umbral forzado a fallo se evalúa y **falla** | Ritmo exacto dentro de tolerancia en el 100 % de las muestras; `OVR = 0`; control negativo falla |
| **T5** | Regresión de sistema y red (en imágenes debug y release de `prj_04` y `prj_03`):<br>• `monitor.py --sn` (captura banner);<br>• `net_check.py` (ICMP ping 4/4, barrido de tamaños UDP 1 a 1472 B, ráfaga 100/100 UDP);<br>• Contadores de red | Banner muestra el commit y build id exacto del checkout; ping 4/4; UDP 100/100 sin pérdidas; `rx_nobuf=0`, `tx_nobuf=0`, `dma_err=0` (y `acl_drop=0` en `prj_04`) | — | Todo el tráfico pasa limpio; contadores en 0 en todas las imágenes |
| **T6a** | 20 arranques repetidos por SWD (`-rst`) sobre la imagen `debug`: tras cada reset se espera 10 s y se ejecutan T3 y T4 | T3 y T4 pasan satisfactoriamente en cada uno de los 20 arranques | — | 20/20 PASS |
| **T6b** | Arranque en frío (power cycle físico en banco con el responsable): lectura de flags de arranque en `RCC_RSR` y ejecución de T3, T4 y T5 | T3, T4 y T5 PASS tras el encendido en frío | — | PASS. Requiere presencia física en el banco (no bloquea entrega de F1, sí el cierre) |
| **T7** | Regresión de driver y compatibilidad cruzada:<br>• Preset `selftest-tim` de `prj_01_template_dualcore` compila limpiamente;<br>• Comprobación de API congelada (`git diff main -- '01_shared_drivers/03_tim/*.h'`) | `selftest-tim` genera binario con 0 errores; diff de cabeceras es estrictamente vacío | — | PASS en ambos aspectos |

### Normas para las capturas de evidencia
- **Unicidad e integridad:** una captura por prueba y herramienta, generada en una sola ejecución sin cortes ni ediciones manuales, guardada en codificación UTF-8 en `prj_04/docs/tim_f4_<prueba>_<hash>.txt` y `prj_03/docs/tim_f4_<prueba>_<hash>.txt`.
- **Identidad de la imagen:** toda sesión en placa comienza flasheando la ELF compilada en ese commit limpio mediante `tools/flash.py --sn`, registrando en la captura el mensaje `Download verified successfully`, la fecha, el commit y el build ID. No se permite ninguna compilación ni reflasheo intermedio.
- **Origen de datos explícito:** toda dirección o desplazamiento leído por SWD imprime su procedencia (`nm` de la ELF, DWARF o cabecera oficial).
- **Control de limpieza del árbol:** si `git status --porcelain` no está vacío al comenzar, la captura lo imprime de inmediato y la prueba se califica automáticamente como INVALID / FAIL.

---

## Banco de pruebas y reglas de seguridad (G-S1 a G-S7)

El banco de pruebas de hardware permanece idéntico al de las fases `00_gpio` F1b y F1c, sin modificar ningún cableado físico.

| # | Regla |
| --- | --- |
| **G-S1** | **Solo se flashean imágenes de producción** (`debug` y `release`) de `prj_04` y `prj_03`, junto con la imagen de control del commit `04fb865`. **Nunca se flashean presets de selftest** (`selftest-adc`, `selftest`, etc.) a la placa física, pues configuran líneas del banco analógico AD2 o conmutan pines reservados. Los presets de selftest solo se compilan en host para validar la ausencia de errores de sintaxis y enlace |
| **G-S2** | **Identificación unívoca del ST-LINK:** se pasa obligatoriamente `sn=0016000C4142501220353451` en cada llamada a `STM32_Programmer_CLI`, `flash.py` y `monitor.py` |
| **G-S3** | **El Analog Discovery 2 (AD2) permanece en estado seguro**, desconectado de scripts de host activos durante las sesiones de esta fase |
| **G-S4** | **Configuración de red:** gestionada por el responsable del banco (`192.168.1.150` para la placa STM32H755) |
| **G-S5** | **Exclusión mutua en la sonda:** cualquier flasheo o reset por SWD se ejecuta únicamente con los scripts lectores (`monitor.py`, `net_check.py`, `query_adc.py`) completamente detenidos |
| **G-S6** | **Imágenes reproducibles del repositorio:** no se requiere volcado previo de la memoria Flash; el informe final declara documentalmente qué commit y binario quedaron instalados en la placa al término de la sesión |
| **G-S7** | **Operación en remoto (sin recuperación manual):** bajo acceso remoto, la única vía de rescate es el pin NRST vía ST-LINK (`mode=UR`, `-hardRst`). Queda terminantemente prohibido alterar la configuración de alimentación (`HAL_PWREx_ConfigSupply`), la inicialización de reloj base (`SystemClock_Config`), los option bytes (BCM4, BCM7, RDP, direcciones de arranque), el firmware de la sonda ST-LINK o realizar borrados masivos de la memoria Flash. Ante cualquier pérdida de respuesta por SWD, se suspende la actividad de inmediato sin reintentos agresivos |

---

## Fases y entregables

| Fase | Entregable | Criterio de pase (Gate) |
| --- | --- | --- |
| **F1 (Implementación y pruebas)** | • Cambios en código (H1–H4);<br>• Enlace en CMakeLists de ambos proyectos;<br>• Herramienta `tools/tim_f4_checks.py` (H5);<br>• Evidencias T1–T5, T6a y T7 en `prj_04/docs/` y `prj_03/docs/`;<br>• `IMPLEMENTATION_REPORT.md` | Auditoría independiente de código, revisión de trazas y ejecución propia del auditor sobre la placa (T2, T3 con control, T4, T5, T6a) |
| **Cierre** | Implementador:<br>• `01_shared_drivers/03_tim/README.md` (actualizar §9.5 validando TRGO → ADC y `tim_hal` en CM4; actualizar §11 reflejando la migración de `prj_03` y `prj_04`; actualizar §13);<br>• `prj_03/README.md` y `prj_04/README.md` (documentar integración de `03_tim`).<br>Responsable:<br>• Actualización de `DRIVER_ROADMAP.md`;<br>• Eliminación de documentos de proceso temporales (`IMPLEMENTATION_PLAN.md`, `IMPLEMENTER_INSTRUCTIONS.md`, `IMPLEMENTATION_REPORT.md`, `AUDIT.md`);<br>• Merge a `main` | Ejecución de T6b (arranque en frío) con el responsable en el banco; auditoría de cierre conforme |

---

## Riesgos y mitigaciones

| Riesgo | Mitigación |
| --- | --- |
| Generación de un pulso espurio de TRGO al inicializar la base de tiempos que descoordine el primer ciclo del ADC | La función `InitTimeBase` se invoca antes de `ConfigMaster`. Como el valor por defecto tras reset de `CR2.MMS` es `000` (Reset), el bit `UG` emitido por `InitTimeBase` no se propaga como TRGO. `ConfigMaster` se llama inmediatamente después |
| Desalineación de reloj entre núcleos al manipular `APB1LENR` (Regla D) | `prj_04 CM7` ya habilita `TIM3` de forma anticipada antes de despertar a CM4. En CM4, la llamada a `TIM_BM_EnableClock(TIM3)` escribe en el registro de banco propio del núcleo (`C2_APB1LENR` / macro CMSIS bancada) y verifica la lectura |
| Falsa alarma en la verificación de frecuencia por la desviación del oscilador HSE (+1,3 %) | El oráculo de T4 no compara contra 500.000 Hz exactos, sino contra una ventana formal de `[502.000, 509.000] Hz` derivada de la desviación documentada en `03_tim/README.md` §10.5 (+13.273 ppm → ~506,6 kHz) |
| Errores de verificación en `tim_hal` sobre CM4 | `tim_hal` ejecuta sus lecturas de verificación bajo sección crítica con PRIMASK. En T1 se valida que no existan avisos de compilación ni conflictos de tipos con `USE_HAL_DRIVER` en CM4 |
| La prueba T4 de `dma_selftest` deja TIM7 corriendo al finalizar con error | Se asegura un bloque de salida con `TIM_BM_Stop(TIM7)` y `TIM_BM_Deinit(TIM7)` tanto en ramas de éxito como de fallo |

---

## Decisiones abiertas y justificación

1. **Tier del driver en cada proyecto:**
   - **Opción A (Por defecto): `tim_bm` en `prj_04` y `tim_hal` en `prj_03`.**
     *Justificación:* Respeta la arquitectura troncal de cada proyecto (`prj_04` es bare-metal; `prj_03` es HAL). Permite validar ambos tiers en un solo ciclo y resuelve los dos aspectos pendientes de `03_tim` §9.5: ejercicio de `tim_hal` en CM4 y ejercicio de TRGO → ADC en silicio.
   - **Opción B:** `tim_bm` en ambos proyectos (`prj_04` y `prj_03`).
     *Justificación:* Más homogéneo, pero introduce un driver bare-metal en un proyecto que estructura sus periféricos con HAL (`adc_hal`), dejando `tim_hal` en CM4 sin ejercitar en producción.

2. **Alcance de los selftests de temporizadores:**
   - **Opción A (Por defecto): Incluir TIM3 de `adc_selftest_hw.c` (ambos proyectos) y TIM7 de `dma_selftest.c` (`prj_04`).**
     *Justificación:* Cumple textualmente con la deuda registrada en `03_tim/README.md` §11 ("TIM3 y TIM7 todavía a mano... esperando migrar"). Erradica todas las escrituras ad-hoc a registros de TIM en el firmware de aplicación y selftests de CM4.
   - **Opción B:** Migrar únicamente el código de producción (`adc_acquisition.c`), excluyendo selftests.
     *Justificación:* Reduce el diff, pero mantiene deuda técnica y código ad-hoc que incumple §11.

3. **Mecanismo para no romper el ritmo del ADC:**
   - **Opción A (Por defecto): Configuración con `arr_preload = false`, secuencia `InitTimeBase` → `ConfigMaster`, y arranque con `TIM_BM_Start` postergado hasta que ADC y DMA estén armados.**
     *Justificación:* Garantiza que el primer flanco de TRGO encuentre al convertidor analógico y al controlador DMA preparados, asegurando `OVR = 0` y cadencia nominal de 500 kHz (+1,3 % reloj real) sin transitorios.

4. **Documentos de proceso al cierre:**
   - **Por defecto:** Se eliminan del árbol al cierre de la fase (como en `00_gpio` F1b y F1c). Los comandos `git show` para consultarlos en el historial se anotan en `03_tim/README.md` §13.
