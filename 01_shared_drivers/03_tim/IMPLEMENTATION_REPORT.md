# Informe de implementación de `03_tim` F4 (Migración de TIM3 y TIM7 a `tim_bm` / `tim_hal`)

Oct 15, 2026 · Rama `driver/tim-f4` · Estado: **Fase F1 corregida (ronda 3) para auditoría independiente**

Este informe documenta la implementación, correcciones tras la tercera ronda de auditoría y verificación sin placa de la fase F4 del driver `03_tim` en el repositorio STM32H755, conforme a `01_shared_drivers/03_tim/IMPLEMENTATION_PLAN.md` (revisión 5), las directrices de `01_shared_drivers/03_tim/IMPLEMENTER_INSTRUCTIONS.md` y las auditorías registradas en `01_shared_drivers/03_tim/AUDIT.md`.

Conforme a las instrucciones del responsable del banco y las restricciones de rol del implementador (sin acceso físico a hardware), se han completado e integrado todos los hitos de código y herramientas (H1 a H5), se han implementado las correcciones para los hallazgos de la tercera ronda de auditoría (R1, B2, B4, M2, M5, m2), se han repetido y registrado las pruebas sin hardware (T1, T2 y T7 con capturas UTF-8 sobre el commit `0fa3f5a`), y se entrega la herramienta automatizada `tools/tim_f4_checks.py` lista y verificada para la ejecución de las pruebas en banco (T3 a T6 y T8) por el auditor independiente.

---

## 1. Resumen de la entrega

- **Rama de trabajo:** `driver/tim-f4`
- **Punto de partida:** `4ed5d59` (actualizado a `main` en `897bf8d` con decisiones de F4 aprobadas)
- **Hash de código y herramienta evaluado:** `0fa3f5a`
- **Hash del commit de evidencias (T1, T2, T7):** `f8b81d3`
- **Estado de la entrega:** **Fase F1 corregida (ronda 3) para auditoría independiente**

### Hitos y correcciones completadas

| Hito / Corrección | Commit | Descripción y justificación técnica |
| --- | --- | --- |
| **H1** | `648d262` | **Migración de TIM3 en adquisición de `prj_04` a `tim_bm`:** Enlace de `stm32h7_driver_tim_bm` en `02_firmware_projects/prj_04_adc_dma_bm_eth/CM4/CMakeLists.txt`. Migración de `TIM3_Trigger_Init` en `adc_acquisition.c` usando `TIM_BM_EnableClock`, `TIM_BM_InitTimeBase` (PSC=0, ARR=399, ARPE=0) y `TIM_BM_ConfigMaster` (MMS=010 update TRGO). Sustitución del arranque tardío directo (`TIM3->CR1 \|= TIM_CR1_CEN`) por `TIM_BM_Start(TIM3)`. |
| **H2** | `42c15a5` | **Migración de TIM3 en adquisición de `prj_03` a `tim_hal`:** Enlace de `stm32h7_driver_tim_hal` en `02_firmware_projects/prj_03_adc_dma_eth/CM4/CMakeLists.txt`. Sustitución de `HAL_TIM_Base_Init` y `HAL_TIMEx_MasterConfigSynchronization` por `TIM_HAL_InitTimeBase` y `TIM_HAL_ConfigMaster` en `TIM3_Trigger_Init`, y arranque tardío con `TIM_BM_Start(TIM3)`. Eliminación total de llamadas `HAL_TIM_*` fuera de `tim_hal.c`. |
| **H3** | `52bee58` | **Migración de TIM3 y TIM7 en selftests de `prj_04` a `tim_bm`:** En `adc_selftest_hw.c`, migración de `Tim3_Prepare`, `Tim3_Run` y `Tim3_Halt` a `TIM_BM_InitTimeBase`, `TIM_BM_ConfigMaster`, `TIM_BM_Start` y `TIM_BM_Stop`. En `dma_selftest.c` (T4 selftest de `02_dma`), migración de configuración, petición DMA por actualización (`DIER.UDE`), arranque, detención y desinicialización de TIM7 a `tim_bm`. |
| **H4** | `47782f3` | **Migración de TIM3 en selftests de `prj_03` a `tim_bm`:** En `adc_selftest_hw.c`, migración de `Tim3_Prepare`, `Tim3_Run` y `Tim3_Halt` a las funciones de `tim_bm` (`TIM_BM_InitTimeBase`, `TIM_BM_ConfigMaster`, `TIM_BM_Start`, `TIM_BM_Stop`), unificando el control del generador de eventos ADC en selftests de ambos proyectos. |
| **Firmware R1 (M6, m1)** | `1b01e9a` | **Corrección de comentarios en firmware y validación de estado:** (M6) En ambos `adc_acquisition.c`, restitución de comentarios técnicos en inglés explicando el cálculo nominal del temporizador (reloj de temporizador APB1 200 MHz, PSC 0, ARR 399 -> 500 kHz, 50 muestras por periodo a 10 kHz) y aclaración verídica de que la emisión de `UG` bajo `MMS=000` sí genera un pulso inicial en TRGO pero resulta inocuo porque el ADC todavía no ha sido arrancado (`ADSTART=0`). (m1) En ambos `adc_selftest_hw.c`, eliminación del descarte `(void)` de valores devueltos por `TIM_BM_*` en `Tim3_Prepare`, `Tim3_Run` y `Tim3_Halt`, comprobando `TIM_STATUS_OK` y emitiendo fallo formal ante cualquier error. |
| **Firmware R2 (k1, k2)** | `2936c1e` | **Habilitación de reloj antes de parada y cita RM0399:** (k1) En `adc_selftest_hw.c` (ambos proyectos), `Tim3_Prepare` ejecuta `TIM_BM_EnableClock(TIM3)` **antes** de `TIM_BM_Stop(TIM3)`, garantizando que el acceso al periférico para detenerlo se realice con el reloj habilitado en el bus APB1. (k2) Corrección de la cita a RM0399 de §40 a §41 (TIM2–TIM5, TIMx_CR2 en §41.4.2) en comentarios de código y documentación. |
| **Herramienta R1 / R2** | `42ed495`<br>`e9afdbb`<br>`1f8876b` | Implementación base y correcciones de rondas 1 y 2 (N1, B1, B2, B3, M1, M2, M3, m2). |
| **Herramienta R3 (R1, B2, B4, M2, m2)** | `0fa3f5a` | **Corrección de hallazgos de auditoría (ronda 3):**<br>• **R1 (T6a):** Alineación de muestras A y B al flanco de incremento de `m4_hb` (`measure_udp_aligned_window`), eliminando el error de cuantización de $\pm 1$ latido (~0,94%) que causaba fallo intermitente en la ventana arbitraria de 10,5 s; controles negativos $ds \times 0,8$ y $ds \times 1,01$ utilizando los datos medidos del ciclo.<br>• **B2:** Funciones compartidas `check_rate_equivalence`, `check_board_oracle` y `check_rate_sanity` evaluando tanto las mediciones nominales como los controles negativos con datos medidos en tiempo real ($rate_{\text{mig}} \times 1,01$ vs $rate_{\text{ctrl}}$, $ds \times 0,8 / dm4\_hb$).<br>• **B4 (T4):** En presencia de control, T4 flashea la imagen migrada primero, evalúa oráculo y sanidad sobre ambas imágenes (migrada y control), y registra el flasheo de restauración como caso de prueba verificado.<br>• **M2:** Conversión del chequeo previo de seguridad de pines G-S1 en una nota informativa sin veredicto constante PASS.<br>• **m2:** `--test all` ejecuta la suite de hardware sin recompilar (`t2a, t2b, t3, t4, t5, t6a`); timeout dedicado de 25,0 s en `subprocess.run` para `monitor.py` en T5; actualización de `--help` y docstring conforme al Plan de Implementación rev 5. |

---

## 2. Resultados de las pruebas sin placa (T1, T2, T7)

Todas las pruebas sin hardware han sido repetidas y registradas de forma íntegra sobre el commit de código y herramienta `0fa3f5a`, generando capturas oficiales bajo `docs/tim_f4_<prueba>_0fa3f5a.txt` (conservando las capturas previas de rondas anteriores conforme a las directrices de auditoría).

### 2.1 T1: Compilaciones y tamaños

Verificación de compilación limpia de la matriz CI completa (8 configuraciones debug y release construidas de cero con `build_all.ps1 -Clean -Fresh`, con `-Werror` en `prj_03` y `prj_04`), y de los 4 presets de selftest requeridos por el plan construidos de cero (`--fresh`).

**Resultados de la matriz CI (`tim_f4_t1_0fa3f5a.txt`):**

| Proyecto | Preset | Núcleo | Estado | Text (B) | Data (B) | Bss (B) | Total (B) | Tiempo |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `prj_01_template_dualcore` | debug | CM7 | PASS | 30632 | 132 | 55676 | 86440 | 28.9s |
| `prj_01_template_dualcore` | debug | CM4 | PASS | 3456 | 24 | 12560 | 16040 | 28.9s |
| `prj_01_template_dualcore` | release | CM7 | PASS | 29620 | 132 | 55680 | 85432 | 18.1s |
| `prj_01_template_dualcore` | release | CM4 | PASS | 3112 | 24 | 12560 | 15696 | 18.1s |
| `prj_02_eth_lwip_dualcore` | debug | CM7 | PASS | 87312 | 152 | 120096 | 207560 | 30.4s |
| `prj_02_eth_lwip_dualcore` | debug | CM4 | PASS | 2580 | 24 | 12488 | 15092 | 30.4s |
| `prj_02_eth_lwip_dualcore` | release | CM7 | PASS | 83224 | 152 | 120096 | 203472 | 23.5s |
| `prj_02_eth_lwip_dualcore` | release | CM4 | PASS | 2332 | 24 | 12488 | 14844 | 23.5s |
| `prj_03_adc_dma_eth` | debug | CM7 | PASS | 89684 | 152 | 121224 | 211060 | 32.0s |
| `prj_03_adc_dma_eth` | debug | CM4 | PASS | 23084 | 24 | 16192 | 39300 | 32.0s |
| `prj_03_adc_dma_eth` | release | CM7 | PASS | 85480 | 152 | 121224 | 206856 | 26.8s |
| `prj_03_adc_dma_eth` | release | CM4 | PASS | 20660 | 24 | 16192 | 36876 | 26.8s |
| `prj_04_adc_dma_bm_eth` | debug | CM7 | PASS | 90512 | 156 | 121268 | 211936 | 31.5s |
| `prj_04_adc_dma_bm_eth` | debug | CM4 | PASS | 11224 | 28 | 15920 | 27172 | 31.5s |
| `prj_04_adc_dma_bm_eth` | release | CM7 | PASS | 86524 | 156 | 121264 | 207944 | 24.3s |
| `prj_04_adc_dma_bm_eth` | release | CM4 | PASS | 10672 | 28 | 15920 | 26620 | 24.3s |

**Presets de selftest adicionales verificados en T1:**
- `prj_04_selftest` (`02_firmware_projects/prj_04_adc_dma_bm_eth`, preset `selftest`): PASS (exit code 0).
- `prj_04_selftest_adc` (`02_firmware_projects/prj_04_adc_dma_bm_eth`, preset `selftest-adc`): PASS (exit code 0).
- `prj_03_selftest` (`02_firmware_projects/prj_03_adc_dma_eth`, preset `selftest`): PASS (exit code 0).
- `prj_01_selftest_tim` (`03_firmware_templates/prj_01_template_dualcore`, preset `selftest-tim`): PASS (exit code 0).

**Veredicto T1:** **PASS** en `prj_04` y `prj_03`.  
**Capturas de evidencia:**
- `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t1_0fa3f5a.txt`
- `02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t1_0fa3f5a.txt`

---

### 2.2 T2: Símbolos del enlazador (T2a) y análisis estático preprocesado (T2b)

#### T2a: Presencia y ausencia de símbolos enlazados (`arm-none-eabi-nm`)

- **Oráculo positivo (árbol actual):**
  - En `prj_04` CM4 (`debug` y `release`), presencia confirmada de los 4 símbolos de `tim_bm`: `TIM_BM_InitTimeBase`, `TIM_BM_EnableClock`, `TIM_BM_ConfigMaster` y `TIM_BM_Start` (**4/4**).
  - En `prj_03` CM4 (`debug` y `release`), presencia confirmada de los 4 símbolos del driver (`tim_hal` + `tim_bm`): `TIM_HAL_InitTimeBase`, `TIM_HAL_ConfigMaster`, `TIM_BM_EnableClock` y `TIM_BM_Start` (**4/4**).
- **Control negativo sobre `04fb865`:**
  - En las 4 ELFs de control (`prj_04` debug/release y `prj_03` debug/release), los símbolos del driver están completamente ausentes (**0 presentes**), satisfaciendo formalmente el control negativo.

#### T2b: Análisis de escrituras ad-hoc y llamadas HAL sobre código preprocesado (`gcc -E`)

- **Autoprueba de patrones regex (CMSIS expanded forms):**
  - Autoprueba ejecutada sobre código C sintético expandiendo cabeceras oficiales de CMSIS: detectó **20 de 20 escrituras** y descartó **12 de 12 lecturas**.
- **Escaneo del árbol actual (M1):**
  - `prj_04`: 7 presets analizados (`debug`, `release`, `selftest`, `selftest-adc`, `selftest-uart`, `selftest-hsem`, `selftest-gpio`). Total: **2282 unidades** analizadas; **0 escrituras ilegales** a registros de TIM3/TIM7 fuera de los archivos exceptuados por el plan. Se detectaron 7 hallazgos conocidos y explicados correspondientes a `CM7/Core/Src/main.c:203` (`__HAL_RCC_TIM3_CLK_ENABLE()`), legítima habilitación de reloj por el núcleo maestro CM7 requerida antes de arrancar CM4 (regla D, RM0399 §9.7.1).
  - `prj_03`: 3 presets analizados (`debug`, `release`, `selftest`). Total: **976 unidades** analizadas; **0 escrituras ilegales** a registros de TIM3/TIM7; **0 llamadas no protegidas** a funciones `HAL_TIM_*` fuera de `tim_hal.c`.
- **Control negativo sobre `04fb865`:**
  - En `prj_04`: Detección precisa de 78 escrituras directas / llamadas heredadas en las 2275 unidades analizadas de control `04fb865`, coincidiendo con todas las 7 tuplas esperadas `(preset, relative_path, kind)`.
  - En `prj_03`: Detección precisa de 23 escrituras directas / llamadas heredadas en las 970 unidades analizadas de control `04fb865`, coincidiendo con todas las 4 tuplas esperadas.

**Veredicto T2:** **PASS** (T2a: PASS, T2b: PASS) en `prj_04` y `prj_03`.  
**Capturas de evidencia:**
- `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t2_0fa3f5a.txt`
- `02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t2_0fa3f5a.txt`

---

### 2.3 T7: Regresión de driver y congelación del contrato de API

- **Compilación de plantilla `prj_01` con preset `selftest-tim`:**
  - Configuración limpia y compilación completa del preset `selftest-tim` en `03_firmware_templates/prj_01_template_dualcore`.
  - Resultado: binarios CM7 y CM4 generados limpiamente con **código de salida 0** (0 errores, 0 advertencias).
- **Comprobación de congelación de API (`git diff`):**
  - Verificación del diff estrictamente contra la rama base `main`: `git diff main -- '01_shared_drivers/03_tim/*.h'`.
  - Resultado: salida **estrictamente vacía** (0 bytes, 0 líneas de diferencia). Las cabeceras públicas del driver permanecen intactas e inalteradas.

**Veredicto T7:** **PASS** en `prj_04` y `prj_03`.  
**Capturas de evidencia:**
- `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t7_0fa3f5a.txt`
- `02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t7_0fa3f5a.txt`

---

## 3. Inspección de registros SWD de TIM3 (T3)

A continuación se detalla la tabla de los 6 registros de hardware de TIM3 contrastada bit a bit contra la especificación formal del manual de referencia RM0399 §41 (General-purpose timers TIM2/TIM3/TIM4/TIM5).

### Tabla de registros esperados (RM0399 §41)

| Registro | Dirección | Valor esperado | Campos y justificación técnica (RM0399 §41) |
| --- | --- | --- | --- |
| `CR1` | `0x40000400` | `0x0001` | **CEN = 1** (contador habilitado); **ARPE = 0** (precarga de ARR deshabilitada, valor activo de inmediato); **DIR = 0** (cuenta ascendente); **URS = 0** (cualquier evento genera update); **UDIS = 0** (eventos UEV habilitados); **CMS = 00** (modo alineado al borde); **CKD = 00** (tDTS = tCK_INT). |
| `CR2` | `0x40000404` | `0x0020` | **MMS = 010 (Update event, RM0399 §41.4.2):** el evento de actualización de TIM3 se envía por la línea de disparo maestro `TRGO` para disparar las conversiones regulares de ADC1/ADC2; **CCDS = 0**; **MMS2 = 0**. |
| `SMCR` | `0x40000408` | `0x0000` | **Modo esclavo deshabilitado (SMS = 000):** el temporizador opera de forma autónoma con el reloj interno del bus APB1 (`CK_INT` = 200 MHz); **TS = 000**; **MSM = 0**; **ETF/ETPS/ECE/ETP = 0**. |
| `DIER` | `0x4000040C` | `0x0000` | **Interrupciones y peticiones DMA de TIM3 deshabilitadas:** el trigger TRGO hacia el ADC es una conexión hardware directa entre periféricos; no requiere que TIM3 genere interrupciones de CPU (`UIE = 0`) ni peticiones DMA propias (`UDE = 0`). |
| `PSC` | `0x40000428` | `0x0000` | **Prescaler = 0 (divisor 1):** el contador avanza a la frecuencia completa del reloj de temporizadores de APB1 (200 MHz, derivado del SYSCLK PLL1P vía `D2PPRE1 = /2` y multiplicador de timer x2). |
| `ARR` | `0x4000042C` | `0x018F` | **Auto-reload register = 399 (0x018F):** periodo de cuenta = `ARR + 1 = 400` ciclos. A 200 MHz, frecuencia de disparo TRGO = 200.000.000 / 400 = **500.000 Hz (500 kHz nominal)**. |

### Resultados medidos en placa por el auditor (ronda 3, `1caa7c3`)
En las capturas oficiales de hardware del auditor (`tim_f4_audit3_t3_1caa7c3.txt` y `tim_f4_audit3_t3_control_1caa7c3.txt`), la lectura directa SWD a 32 bits confirmó exactamente los 6 registros esperados en la imagen migrada y en la imagen de control `04fb865` para ambos proyectos:
- `CR1 = 0x00000001`
- `CR2 = 0x00000020`
- `SMCR = 0x00000000`
- `DIER = 0x00000000`
- `PSC = 0x00000000`
- `ARR = 0x0000018F`
- Control negativo de tabla alterada: correctamente rechazado.

---

## 4. Instrucciones y comandos para la auditoría en banco

El auditor independiente ejecutará las pruebas en hardware conectando la sonda ST-LINK (número de serie obligatorio `sn=0016000C4142501220353451`). Toda sesión en placa respetará estrictamente las reglas de seguridad del banco (G-S1 a G-S7).

Los comandos emplean el argumento nativo `--output <ruta>` de `tim_f4_checks.py`, que captura la salida en UTF-8 con saltos de línea LF mediante `StreamTee`, evitando redirecciones de PowerShell.

### 4.1 Comprobación de seguridad de pines del banco (G-S1 / M2)

Conforme a la especificación del banco y reflejado como nota informativa en la herramienta:
- **Líneas autorizadas utilizadas por los presets de prueba:** `PB1` (`BOARD_ADC_PIN`, canal analógico 5 / ADC1_INP5), `PD5`/`PD6` (USART2 TX/RX, puente dedicado para selftest DMA T3), canales internos de ADC (VREFINT, sensor temperatura), y disparos internos por hardware de TIM3/TIM7 (TRGO a ADC y DMA).
- **Líneas protegidas del banco con intersección estrictamente nula:** `PE2`–`PE15`, `PA8` (bus AD2); `PA5`, `PA6`, `PB4`, `PB5` (conector J1); `PB8`, `PB9` (bus I2C); `PF14`, `PF15` (tomas auxiliares). Ningún preset migrado o de prueba reconfigura estos pines.

### 4.2 Comandos de ejecución para el auditor

#### T3: Inspección en caliente de registros SWD de TIM3
```bash
# prj_04 (flasheo verificado de debug, espera y lectura de los 6 registros en caliente a 32 bits)
py -u tools/tim_f4_checks.py --project prj_04 --test t3 --flash --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t3_0fa3f5a.txt

# prj_03 (flasheo verificado de debug, espera y lectura de los 6 registros en caliente a 32 bits)
py -u tools/tim_f4_checks.py --project prj_03 --test t3 --flash --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t3_0fa3f5a.txt
```

#### T4: Ritmo del ADC, overrun y equivalencia secuencial vs control (B2, B4)
```bash
# prj_04 (flasheo migrada primero, ventana 60s UDP 5001, flasheo control 04fb865, ventana 60s, |Δ| <= 0.1%, oráculo y sanidad evaluados en ambas, controles negativos con datos medidos, restauración verificada)
py -u tools/tim_f4_checks.py --project prj_04 --test t4 --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t4_0fa3f5a.txt

# prj_03 (flasheo migrada primero, ventana 60s UDP 5001, flasheo control 04fb865, ventana 60s, |Δ| <= 0.1%, oráculo y sanidad evaluados en ambas, controles negativos con datos medidos, restauración verificada)
py -u tools/tim_f4_checks.py --project prj_03 --test t4 --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t4_0fa3f5a.txt
```

#### T5: Regresión de sistema y red (m2)
```bash
# prj_04 debug (timeout dedicado 25s en monitor)
py -u tools/tim_f4_checks.py --project prj_04 --test t5 --preset debug --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t5_debug_0fa3f5a.txt

# prj_04 release (timeout dedicado 25s en monitor)
py -u tools/tim_f4_checks.py --project prj_04 --test t5 --preset release --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t5_release_0fa3f5a.txt

# prj_03 debug (timeout dedicado 25s en monitor)
py -u tools/tim_f4_checks.py --project prj_03 --test t5 --preset debug --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t5_debug_0fa3f5a.txt

# prj_03 release (timeout dedicado 25s en monitor)
py -u tools/tim_f4_checks.py --project prj_03 --test t5 --preset release --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t5_release_0fa3f5a.txt
```

#### T6a: 20 arranques repetidos por SWD con telemetría alineada a flancos de `m4_hb` (R1)
```bash
# prj_04 (20 ciclos de reset con 10s de espera, T3 y T4 alineado a flancos de m4_hb, controles negativos con datos medidos)
py -u tools/tim_f4_checks.py --project prj_04 --test t6a --flash --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t6a_0fa3f5a.txt

# prj_03 (20 ciclos de reset con 10s de espera, T3 y T4 alineado a flancos de m4_hb, controles negativos con datos medidos)
py -u tools/tim_f4_checks.py --project prj_03 --test t6a --flash --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t6a_0fa3f5a.txt
```

#### T8: Selftests migrados en la placa y equivalencia con control (M5)
```bash
# Detecta banner de arranque de CM7 por '=== STM32H755' o '(Cortex-M7)',
# flashea prj_04 selftest & selftest-adc, prj_03 selftest (migrados y control 04fb865),
# valida líneas exactas PASS, 0 FAIL, y restaura prj_03 debug verificado
py -u tools/tim_f4_checks.py --project prj_04 --test t8 --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t8_0fa3f5a.txt
```

---

## 5. Estado de pruebas que requieren hardware

Conforme a las directrices de rol y banco de pruebas, el implementador no tiene acceso físico a la placa STM32H755 y no ejecuta pruebas de hardware directamente. A continuación se reporta el estado y los resultados medidos por el auditor independiente en la ronda 3 (`1caa7c3`):

| Prueba | Descripción | Estado implementador | Resultados medidos por el auditor (ronda 3, `1caa7c3`) |
| --- | --- | --- | --- |
| **T3** | Inspección en caliente de registros TIM3 por SWD | **Pendiente auditor** | **PASS:** Registros idénticos a RM0399 §41 (`CR1=0x1`, `CR2=0x20`, `SMCR=0`, `DIER=0`, `PSC=0`, `ARR=0x18F`) en ambas imágenes (`prj_04` y `prj_03`). |
| **T4** | Ritmo del ADC (consultas UDP 5001, ventana 60s, equivalencia vs control, oráculo $50\,000 \pm 0,5\%$ samples/hb) | **Pendiente auditor** | **PASS:** Ritmo nominal ~505,8 kHz; equivalencia $|\Delta| = 0,0025\%$ en `prj_04` y $0,0319\%$ en `prj_03` ($\le 0,1\%$); oráculo 49 923 en `prj_04` y 50 066 en `prj_03`; ovr=0, err=0, adc_ovr=0. |
| **T5** | Regresión de red ping y UDP (debug/release) | **Pendiente auditor** | **PASS:** 3/3 checks en debug y release para ambos proyectos; contadores `[net]` (`rx_nobuf`, `tx_nobuf`, `dma_err`, `acl_drop`) estrictamente en 0. |
| **T6a** | 20 arranques repetidos por SWD (20/20 PASS) | **Pendiente auditor** | **Falló en ronda 3 (0/20) por ventana no alineada; corregido en `0fa3f5a` (R1):** La ventana fija de 10,5 s introducía un error de cuantización de $\pm 1$ latido en 106 (~0,94%), superando la tolerancia de $\pm 0,5\%$. En `0fa3f5a` los extremos A y B se sincronizan a los flancos de incremento de `m4_hb`, garantizando $\Delta m4\_hb$ exacto. |
| **T6b** | Arranque en frío (power cycle físico en banco) | **Pendiente auditor** | Ejecución manual por el responsable del banco durante el cierre formal. |
| **T8** | Selftests en placa (`prj_04` selftest/selftest-adc, `prj_03` selftest) | **Pendiente auditor** | **PASS:** Selftests completados en placa con veredictos exactos (`[m7_dma_test] OVERALL: PASS`, `[m4_dma_test] OVERALL=PASS`, `HW RESULT PASS`); equivalencia con control verificada; restauración de `prj_03 debug` verificada. |

**Estado final de la placa tras las pruebas (G-S1 / G-S6):**
Al concluir la batería de pruebas en placa, la herramienta `tim_f4_checks.py` en el paso final de T8 vuelve a flashear y verificar la imagen de producción **`prj_03_adc_dma_eth` preset `debug`**, dejando la placa en estado operativo normal con confirmación de descarga verificada (N1).

---

## 6. Desviaciones y notas técnicas

1. **Sincronización a flanco de `m4_hb` en T6a (R1):**
   En la ronda 3, la auditoría observó que una ventana de tiempo arbitraria (10,5 s) generaba variaciones en $\Delta m4\_hb$ entre 106 y 107 latidos. Al ser un contador entero, una discrepancia de 1 latido sobre 106 representa un error de cuantización de aproximadamente 0,94%, superando el umbral de tolerancia del oráculo ($\pm 0,5\%$). En `0fa3f5a`, la función `measure_udp_aligned_window` muestrea el inicio esperando activamente hasta que `m4_hb` avanza exactamente una unidad (fijando el punto A al flanco de incremento), espera al menos 10 segundos y luego muestrea el final esperando de nuevo a que `m4_hb` vuelva a incrementarse (fijando el punto B al flanco de incremento). De este modo, $\Delta m4\_hb$ es un número exacto de latidos enteros y el error residual se reduce a la fluctuación del sondeo UDP ($< 0,05\%$).
2. **Evaluación uniforme de oráculo, sanidad y equivalencia con datos medidos (B2, B4):**
   Las funciones `check_rate_equivalence`, `check_board_oracle` y `check_rate_sanity` son compartidas y reutilizadas en T4 y T6a. Los controles negativos no emplean cadenas simuladas, sino que pasan por las mismas funciones de evaluación matemática utilizando los datos medidos del ciclo ($rate_{\text{mig}} \times 1,01$ frente a $rate_{\text{ctrl}}$, y $ds \times 0,8 / dm4\_hb$). Asimismo, en T4 con control, se evalúa oráculo y sanidad tanto en la imagen migrada como en la de control.
3. **Detección de banner de arranque de CM7 (M5):**
   En consonancia con las emisiones reales del firmware observado por el auditor en hardware, la detección de arranque del núcleo maestro CM7 en T8 y en el monitor no depende de cadenas rígidas inexistentes, sino de los patrones estándar `=== STM32H755` o `(Cortex-M7)`.
4. **Compilación limpia de `prj_01 selftest-tim` (M5):**
   La plantilla `prj_01_template_dualcore` bajo el preset `selftest-tim` compila de forma reproducible y limpia con código de salida 0 y cero advertencias, asegurando la integridad de las pruebas de regresión de driver en T7.

---

F1 CORREGIDA 3 PARA AUDITORIA 0fa3f5a
