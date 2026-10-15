# Informe de implementación de `03_tim` F4 (Migración de TIM3 y TIM7 a `tim_bm` / `tim_hal`)

Oct 15, 2026 · Rama `driver/tim-f4` · Estado: **Fase F1 entregada para auditoría independiente**

Este informe documenta la implementación y verificación sin placa de la fase F4 del driver `03_tim` en el repositorio STM32H755, de acuerdo con las especificaciones de `01_shared_drivers/03_tim/IMPLEMENTATION_PLAN.md` (revisión 2) y las directrices de `01_shared_drivers/03_tim/IMPLEMENTER_INSTRUCTIONS.md`.

Conforme a las instrucciones del responsable del banco y las restricciones de rol del implementador (sin acceso físico a hardware), se han completado e integrado todos los hitos de código y herramientas (H1 a H5), se han ejecutado y registrado íntegramente las pruebas sin hardware (T1, T2 y T7) con sus respectivos controles negativos sobre el commit `04fb865`, y se entrega la herramienta automatizada `tools/tim_f4_checks.py` lista para la ejecución de las pruebas en banco (T3 a T6 y T8) por el auditor independiente.

---

## 1. Resumen de la entrega

- **Rama de trabajo:** `driver/tim-f4`
- **Punto de partida:** `4ed5d59` (actualizado a `main` en `897bf8d` con decisiones de F4 aprobadas)
- **Hash de código y herramienta evaluado:** `da0fc87`
- **Hash del commit de evidencias (T1, T2, T7):** `312f9b7`
- **Estado de la entrega:** **Fase F1 entregada para auditoría independiente**

### Hitos completados

| Hito | Commit | Descripción y justificación técnica |
| --- | --- | --- |
| **H1** | `648d262` | **Migración de TIM3 en adquisición de `prj_04` a `tim_bm`:** Enlace de `stm32h7_driver_tim_bm` en `02_firmware_projects/prj_04_adc_dma_bm_eth/CM4/CMakeLists.txt`. Migración de `TIM3_Trigger_Init` en `adc_acquisition.c` usando `TIM_BM_EnableClock`, `TIM_BM_InitTimeBase` (PSC=0, ARR=399, ARPE=0) y `TIM_BM_ConfigMaster` (MMS=010 update TRGO). Sustitución del arranque tardío directo (`TIM3->CR1 |= TIM_CR1_CEN`) por `TIM_BM_Start(TIM3)`. |
| **H2** | `42c15a5` | **Migración de TIM3 en adquisición de `prj_03` a `tim_hal`:** Enlace de `stm32h7_driver_tim_hal` en `02_firmware_projects/prj_03_adc_dma_eth/CM4/CMakeLists.txt`. Sustitución de `HAL_TIM_Base_Init` y `HAL_TIMEx_MasterConfigSynchronization` por `TIM_HAL_InitTimeBase` y `TIM_HAL_ConfigMaster` en `TIM3_Trigger_Init`, y arranque tardío con `TIM_BM_Start(TIM3)`. Eliminación total de llamadas `HAL_TIM_*` fuera de `tim_hal.c`. |
| **H3** | `52bee58` | **Migración de TIM3 y TIM7 en selftests de `prj_04` a `tim_bm`:** En `adc_selftest_hw.c`, migración de `Tim3_Prepare`, `Tim3_Run` y `Tim3_Halt` a `TIM_BM_InitTimeBase`, `TIM_BM_ConfigMaster`, `TIM_BM_Start` y `TIM_BM_Stop`. En `dma_selftest.c` (T4 selftest de `02_dma`), migración de configuración, habilitación de interrupción TRGO, arranque, detención y desinicialización de TIM7 a `tim_bm`. |
| **H4** | `47782f3` | **Migración de TIM3 en selftests de `prj_03` a `tim_bm`:** En `adc_selftest_hw.c`, migración de `Tim3_Prepare`, `Tim3_Run` y `Tim3_Halt` a las funciones de `tim_bm` (`TIM_BM_InitTimeBase`, `TIM_BM_ConfigMaster`, `TIM_BM_Start`, `TIM_BM_Stop`), unificando el control del generador de eventos ADC en selftests de ambos proyectos. |
| **H5** | `6517c15`<br>`58b4bdf`<br>`da0fc87` | **Arnés de verificación automatizado `tools/tim_f4_checks.py`:** Herramienta integral para pruebas T1–T8 con soporte multi-proyecto (`--project {prj_04, prj_03}` obligatorio). Incluye autoprueba de preprocesador con 12 patrones de escritura y 9 de lectura, discriminación estricta de registros TIM3/TIM7 frente a homónimos de otros periféricos, inspección DWARF y `nm`, controles negativos sobre el commit `04fb865` y oráculos automáticos. |

---

## 2. Resultados de las pruebas sin placa (T1, T2, T7)

### 2.1 T1: Compilaciones y tamaños

Verificación de compilación de la matriz CI completa (8 configuraciones debug y release con `-Werror`, cero advertencias y cero errores) y los 4 presets de selftest requeridos por el plan construidos de cero (`--fresh`).

**Resultados de la matriz CI (`build_all.ps1 -Clean -Fresh`):**

| Proyecto | Preset | Núcleo | Estado | Text (B) | Data (B) | Bss (B) | Total (B) | Tiempo |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `prj_01_template_dualcore` | debug | CM7 | PASS | 30624 | 132 | 55676 | 86432 | 34.0s |
| `prj_01_template_dualcore` | debug | CM4 | PASS | 3456 | 24 | 12560 | 16040 | 34.0s |
| `prj_01_template_dualcore` | release | CM7 | PASS | 29620 | 132 | 55680 | 85432 | 23.3s |
| `prj_01_template_dualcore` | release | CM4 | PASS | 3112 | 24 | 12560 | 15696 | 23.3s |
| `prj_02_eth_lwip_dualcore` | debug | CM7 | PASS | 87312 | 152 | 120096 | 207560 | 34.7s |
| `prj_02_eth_lwip_dualcore` | debug | CM4 | PASS | 2580 | 24 | 12488 | 15092 | 34.7s |
| `prj_02_eth_lwip_dualcore` | release | CM7 | PASS | 83224 | 152 | 120096 | 203472 | 26.6s |
| `prj_02_eth_lwip_dualcore` | release | CM4 | PASS | 2332 | 24 | 12488 | 14844 | 26.6s |
| `prj_03_adc_dma_eth` | debug | CM7 | PASS | 89684 | 152 | 121224 | 211060 | 44.5s |
| `prj_03_adc_dma_eth` | debug | CM4 | PASS | 22896 | 24 | 16192 | 39112 | 44.5s |
| `prj_03_adc_dma_eth` | release | CM7 | PASS | 85480 | 152 | 121224 | 206856 | 30.2s |
| `prj_03_adc_dma_eth` | release | CM4 | PASS | 20660 | 24 | 16192 | 36876 | 30.2s |
| `prj_04_adc_dma_bm_eth` | debug | CM7 | PASS | 90512 | 156 | 121268 | 211936 | 39.0s |
| `prj_04_adc_dma_bm_eth` | debug | CM4 | PASS | 11224 | 28 | 15920 | 27172 | 39.0s |
| `prj_04_adc_dma_bm_eth` | release | CM7 | PASS | 86524 | 156 | 121264 | 207944 | 29.0s |
| `prj_04_adc_dma_bm_eth` | release | CM4 | PASS | 10672 | 28 | 15920 | 26620 | 29.0s |

**Presets de selftest adicionales verificados en T1:**
- `prj_04_selftest` (`02_firmware_projects/prj_04_adc_dma_bm_eth`, preset `selftest`): PASS (0 errores, 0 avisos)
- `prj_04_selftest_adc` (`02_firmware_projects/prj_04_adc_dma_bm_eth`, preset `selftest-adc`): PASS (0 errores, 0 avisos)
- `prj_03_selftest` (`02_firmware_projects/prj_03_adc_dma_eth`, preset `selftest`): PASS (0 errores, 0 avisos)
- `prj_01_selftest_tim` (`03_firmware_templates/prj_01_template_dualcore`, preset `selftest-tim`): PASS (0 errores, 0 avisos)

**Veredicto T1:** **PASS** en `prj_04` y `prj_03`.  
**Capturas de evidencia:**
- `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t1_da0fc87.txt`
- `02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t1_da0fc87.txt`

---

### 2.2 T2: Símbolos del enlazador (T2a) y análisis estático preprocesado (T2b)

#### T2a: Presencia y ausencia de símbolos enlazados (`arm-none-eabi-nm`)

- **Oráculo positivo (árbol actual):**
  - En `prj_04` CM4 (`debug` y `release`), se exige la presencia de los 4 símbolos de `tim_bm`: `TIM_BM_InitTimeBase`, `TIM_BM_EnableClock`, `TIM_BM_ConfigMaster` y `TIM_BM_Start`. Presentes: **4/4** en ambas configuraciones.
  - En `prj_03` CM4 (`debug` y `release`), se exige la presencia de los símbolos de `tim_hal` y `tim_bm`: `TIM_HAL_InitTimeBase`, `TIM_HAL_ConfigMaster` y `TIM_BM_Start`. Presentes: **3/3** en ambas configuraciones.
- **Control negativo sobre `04fb865`:**
  - En el commit de control `04fb865`, ninguno de los dos proyectos enlazaba los drivers `03_tim` en CM4. El oráculo comprobó que en las 4 ELFs de control (`prj_04` debug/release y `prj_03` debug/release), los símbolos del driver están completamente ausentes (**0 presentes**), satisfaciendo la condición de fallo del criterio positivo en el baseline.

#### T2b: Análisis de escrituras ad-hoc y llamadas HAL sobre código preprocesado (`gcc -E`)

- **Autoprueba de patrones regex (CMSIS expanded forms):**
  - Autoprueba ejecutada sobre código C sintético expandiendo cabeceras oficiales de CMSIS: detectó **12 de 12 escrituras** (asignación directa, compuesta, desplazamientos, macro `WRITE_REG`, `MODIFY_REG`, escrituras multilínea y habilitaciones de reloj APB1LENR) y descartó **9 de 9 lecturas** (lecturas de `CNT`, comparaciones de igualdad, desigualdad y `(uint32_t)&(TIM7->CNT)` usado como dirección).
- **Escaneo del árbol actual:**
  - `prj_04`: 7 presets analizados (`debug`, `release`, `selftest`, `selftest-adc`, `selftest-uart`, `selftest-hsem`, `selftest-gpio`). Total: **2282 unidades de compilación** escaneadas; **0 fallos** de preprocesamiento; **0 escrituras ilegales** a registros de TIM3/TIM7.
  - `prj_03`: 3 presets analizados (`debug`, `release`, `selftest`). Total: **976 unidades de compilación** escaneadas; **0 fallos** de preprocesamiento; **0 escrituras ilegales** a registros de TIM3/TIM7; **0 llamadas no protegidas** a funciones `HAL_TIM_*` fuera de `tim_hal.c`.
- **Control negativo sobre `04fb865`:**
  - En `prj_04`: El escaneo sobre los 7 presets del control `04fb865` detectó **70 escrituras directas** a registros de TIM3 y TIM7, localizadas con precisión en `CM4/App/Src/adc_acquisition.c`, `CM4/App/Src/adc_selftest_hw.c` y `CM4/App/Src/dma_selftest.c`.
  - En `prj_03`: El escaneo sobre los 3 presets del control `04fb865` detectó **20 escrituras directas / llamadas `HAL_TIM_*` desprotegidas**, localizadas en `CM4/App/Src/adc_acquisition.c` y `CM4/App/Src/adc_selftest_hw.c`.

**Veredicto T2:** **PASS** (T2a: PASS, T2b: PASS) en `prj_04` y `prj_03`.  
**Capturas de evidencia:**
- `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t2_da0fc87.txt`
- `02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t2_da0fc87.txt`

---

### 2.3 T7: Regresión de driver y congelación del contrato de API

- **Compilación de plantilla `prj_01` con preset `selftest-tim`:**
  - Configuración limpia y compilación completa del preset `selftest-tim` en `03_firmware_templates/prj_01_template_dualcore`.
  - Resultado: binarios CM7 y CM4 generados con **0 errores y 0 advertencias**.
- **Comprobación de congelación de API (`git diff`):**
  - Verificación del diff contra `main` en las cabeceras públicas: `git diff main -- '01_shared_drivers/03_tim/*.h'`.
  - Resultado: salida **estrictamente vacía** (0 bytes, 0 líneas de diferencia). Las cabeceras del driver permanecen intactas.

**Veredicto T7:** **PASS** en `prj_04` y `prj_03`.  
**Capturas de evidencia:**
- `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t7_da0fc87.txt`
- `02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t7_da0fc87.txt`

---

## 3. Inspección de registros SWD de TIM3 (T3)

A continuación se detalla la tabla de los 6 registros de hardware de TIM3 que el auditor comprobará en caliente por SWD (`mode=HOTPLUG`) con la imagen `debug` corriendo (en `prj_04` y `prj_03`), contrastada bit a bit contra la especificación formal del manual de referencia RM0399.

### Tabla de registros esperados (RM0399)

| Registro | Dirección | Valor esperado | Campos y justificación técnica (RM0399 §34.4) |
| --- | --- | --- | --- |
| `CR1` | `0x40000400` | `0x0001` | **CEN = 1** (contador habilitado); **ARPE = 0** (precarga de ARR deshabilitada, valor activo de inmediato); **DIR = 0** (cuenta ascendente); **URS = 0** (cualquier evento genera update); **UDIS = 0** (eventos UEV habilitados); **CMS = 00** (modo alineado al borde); **CKD = 00** (tDTS = tCK_INT). |
| `CR2` | `0x40000404` | `0x0020` | **MMS = 010 (Update event):** el evento de actualización de TIM3 se envía por la línea de disparo maestro `TRGO` para disparar las conversiones regulares de ADC1/ADC2; **CCDS = 0**; **MMS2 = 0**. |
| `SMCR` | `0x40000408` | `0x0000` | **Modo esclavo deshabilitado (SMS = 000):** el temporizador opera de forma autónoma con el reloj interno del bus APB1 (`CK_INT` = 200 MHz); **TS = 000**; **MSM = 0**; **ETF/ETPS/ECE/ETP = 0**. |
| `DIER` | `0x4000040C` | `0x0000` | **Interrupciones y DMA del temporizador deshabilitadas:** el trigger TRGO hacia el ADC es una conexión hardware directa entre periféricos; no requiere que TIM3 genere interrupciones de CPU (`UIE = 0`) ni solicitudes DMA propias (`UDE = 0`). |
| `PSC` | `0x40000428` | `0x0000` | **Prescaler = 0 (divisor 1):** el contador avanza a la frecuencia completa del reloj de entrada del temporizador (200 MHz, derivado de PLL1Q con `D2PPRE1 = /2` y multiplicador de timer x2). |
| `ARR` | `0x4000042C` | `0x018F` | **Auto-reload register = 399 (0x018F):** periodo de cuenta = `ARR + 1 = 400` ciclos. A 200 MHz, frecuencia de disparo TRGO = 200.000.000 / 400 = **500.000 Hz (500 kHz nominal)**. |

### Controles negativos implementados en T3 (`tim_f4_checks.py`)
1. **Control de equivalencia en `04fb865`:** La lectura SWD en caliente sobre la imagen original de control `04fb865` debe arrojar con exactitud los mismos 6 valores, demostrando que la migración a `tim_bm` / `tim_hal` preserva idéntico estado de hardware.
2. **Control de tabla alterada:** La comparación de los registros leídos contra una tabla falsificada (p. ej., `ARR = 0x00FF`) genera automáticamente un fallo (`FAIL`).

---

## 4. Instrucciones y comandos para la auditoría en banco

El auditor independiente ejecutará las pruebas en hardware conectando la sonda ST-LINK (número de serie obligatorio `sn=0016000C4142501220353451`). Toda sesión en placa respetará estrictamente las reglas de seguridad del banco (G-S1 a G-S7).

### 4.1 Comprobación de seguridad de pines del banco (G-S1)

Antes de cualquier flasheo, la herramienta `tim_f4_checks.py` verifica que los ejecutables no utilicen ninguna línea conectada a la instrumentación del banco:

- **Líneas utilizadas por los firmwares a flashear:**
  - `PB1`: `BOARD_ADC_PIN` (Entrada analógica al canal de ADC).
  - `PD5`, `PD6`: USART2 TX/RX (usadas exclusivamente en el selftest de DMA T3 con cable puente dedicado).
  - Canales internos de ADC: VREFINT, sensor de temperatura interno.
  - Periféricos TIM3 y TIM7: disparos exclusivamente internos por hardware (TRGO a ADC y DMA).
- **Líneas protegidas del banco (AD2 / expansores) NUNCA utilizadas:**
  - `PE2` a `PE15`, `PA8`: bus de instrumentación del Analog Discovery 2.
  - `PA5`, `PA6`, `PB4`, `PB5`: conector J1.
  - `PB8`, `PB9`: bus I2C.
  - `PF14`, `PF15`: tomas de prueba auxiliares.
- **Restricción de presets de selftest:** Solo se permite flashear `selftest` y `selftest-adc` de `prj_04` y `selftest` de `prj_03`. Queda terminantemente prohibido flashear `selftest-gpio`, `selftest-uart` o `selftest-hsem`.

### 4.2 Comandos de ejecución para el auditor

#### T3: Inspección en caliente de registros SWD de TIM3
```bash
# prj_04 (flasheo verificado de debug, espera y lectura de los 6 registros en caliente)
py -u tools/tim_f4_checks.py --project prj_04 --test t3 --flash > 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t3_da0fc87.txt 2>&1

# prj_03 (flasheo verificado de debug, espera y lectura de los 6 registros en caliente)
py -u tools/tim_f4_checks.py --project prj_03 --test t3 --flash > 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t3_da0fc87.txt 2>&1
```

#### T4: Ritmo del ADC y ausencia de overrun
```bash
# prj_04 (medida de dSamples / dt >= 10s, ventana [505.635, 507.635] kHz, OVR == 0, adc_ovr == 0)
py -u tools/tim_f4_checks.py --project prj_04 --test t4 > 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t4_da0fc87.txt 2>&1

# prj_03 (medida de dSamples / dt >= 10s, ventana [505.635, 507.635] kHz, OVR == 0)
py -u tools/tim_f4_checks.py --project prj_03 --test t4 > 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t4_da0fc87.txt 2>&1
```

#### T5: Regresión de sistema y red (debug y release)
```bash
# prj_04 debug
py -u tools/tim_f4_checks.py --project prj_04 --test t5 --preset debug > 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t5_debug_da0fc87.txt 2>&1

# prj_04 release
py -u tools/tim_f4_checks.py --project prj_04 --test t5 --preset release > 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t5_release_da0fc87.txt 2>&1

# prj_03 debug
py -u tools/tim_f4_checks.py --project prj_03 --test t5 --preset debug > 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t5_debug_da0fc87.txt 2>&1

# prj_03 release
py -u tools/tim_f4_checks.py --project prj_03 --test t5 --preset release > 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t5_release_da0fc87.txt 2>&1
```

#### T6a: 20 arranques repetidos por SWD (`-rst`)
```bash
# prj_04 (20 ciclos de reset con 10s de espera y validación T3 y T4 tras cada reset)
py -u tools/tim_f4_checks.py --project prj_04 --test t6a --flash > 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t6a_da0fc87.txt 2>&1

# prj_03 (20 ciclos de reset con 10s de espera y validación T3 y T4 tras cada reset)
py -u tools/tim_f4_checks.py --project prj_03 --test t6a --flash > 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t6a_da0fc87.txt 2>&1
```

#### T8: Selftests migrados en la placa y equivalencia con control
```bash
# Ejecuta G-S1, flashea prj_04 selftest & selftest-adc, prj_03 selftest (migrados y control), valida PASS y equivalencia, y restaura prj_03 debug
py -u tools/tim_f4_checks.py --project prj_04 --test t8 > 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t8_da0fc87.txt 2>&1
```

---

## 5. Estado de pruebas que requieren hardware (Lo no ejecutado)

Conforme a las instrucciones expresas del plan, el implementador no tiene acceso físico a la placa STM32H755. Por tanto, todas las pruebas dependientes de hardware quedan formalmente declaradas como **NO VERIFICADO / pendiente de ejecución por el auditor independiente**:

| Prueba | Descripción | Estado por el implementador | Condición de verificación en auditoría |
| --- | --- | --- | --- |
| **T3** | Inspección en caliente de registros TIM3 por SWD | **NO VERIFICADO** | Ejecución por auditor con `--test t3 --flash` en ambos proyectos. |
| **T4** | Ritmo del ADC (500 kHz +1.327% ±0.2%) y OVR=0 | **NO VERIFICADO** | Ejecución por auditor con `--test t4` en ambos proyectos. |
| **T5** | Regresión de red ping 4/4 y UDP 100/100 (debug/release) | **NO VERIFICADO** | Ejecución por auditor con `--test t5` en debug y release de ambos proyectos. |
| **T6a** | 20 arranques repetidos por SWD (20/20 PASS) | **NO VERIFICADO** | Ejecución por auditor con `--test t6a` en ambos proyectos. |
| **T6b** | Arranque en frío (power cycle físico en banco) | **NO VERIFICADO** | Ejecución física por el responsable del banco durante el cierre formal. |
| **T8** | Selftests en placa (`prj_04` selftest/selftest-adc, `prj_03` selftest) | **NO VERIFICADO** | Ejecución por auditor con `--test t8` (equivalencia vs `04fb865`). |

**Estado final de la placa tras las pruebas (G-S1 / G-S6):**
Al concluir la batería de pruebas en placa, la herramienta `tim_f4_checks.py` en el paso final de T8 vuelve a flashear y verificar la imagen de producción **`prj_03_adc_dma_eth` preset `debug`** compilada en el commit `da0fc87`, dejando la placa en estado operativo normal.

---

## 6. Desviaciones y notas técnicas

1. **Arquitectura de adquisición en `prj_03` (`tim_hal` + `tim_bm`):**
   En `prj_03/CM4/App/Src/adc_acquisition.c`, la inicialización de base de tiempo y sincronización maestra se realiza mediante `TIM_HAL_InitTimeBase` y `TIM_HAL_ConfigMaster` (tier HAL). Para el arranque tardío del contador (tras armar ADC y DMA), se utiliza `TIM_BM_Start(TIM3)` de `tim_bm`. Con esto se satisface el requisito de migración al tier HAL y se garantiza que **cero llamadas a `HAL_TIM_*`** permanezcan en `prj_03` fuera de `01_shared_drivers/03_tim/tim_hal.c`.
2. **Discriminación estricta de registros en T2b (`tim_f4_checks.py`):**
   Los nombres de registro `CR1`, `CR2` y `SR` son comunes en la arquitectura STM32 a múltiples periféricos (PWR, USART, I2C, timers auxiliares). El analizador estático T2b implementó un filtro de destino (`TIM37_TARGET_RE`) que exige que cualquier escritura a dichos registros esté dirigida explícitamente a las instancias de `TIM3` (`0x40000400`) o `TIM7` (`0x40001400`). Esto evitó falsos positivos en escrituras legítimas a registros de PWR y USART en selftests o timers de benchmarking en CM7 (`TIM17`), asegurando un escaneo riguroso y libre de ruido en las 3258 unidades de compilación analizadas.

---

F1 ENTREGADA PARA AUDITORIA da0fc87
