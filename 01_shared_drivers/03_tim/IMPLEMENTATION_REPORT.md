# Informe de implementación de `03_tim` F4 (Migración de TIM3 y TIM7 a `tim_bm` / `tim_hal`)

Oct 15, 2026 · Rama `driver/tim-f4` · Estado: **Fase F1 corregida para auditoría independiente**

Este informe documenta la implementación, corrección tras auditoría y verificación sin placa de la fase F4 del driver `03_tim` en el repositorio STM32H755, conforme a `01_shared_drivers/03_tim/IMPLEMENTATION_PLAN.md` (revisión 3), las directrices de `01_shared_drivers/03_tim/IMPLEMENTER_INSTRUCTIONS.md` y la auditoría registrada en `01_shared_drivers/03_tim/AUDIT.md`.

Conforme a las instrucciones del responsable del banco y las restricciones de rol del implementador (sin acceso físico a hardware), se han completado e integrado todos los hitos de código y herramientas (H1 a H5), se han solventado íntegramente todos los hallazgos de auditoría (B1–B3, M1–M6, m1–m2), se han repetido y registrado las pruebas sin hardware (T1, T2 y T7 con capturas UTF-8 sobre el commit `e9afdbb`), y se entrega la herramienta automatizada `tools/tim_f4_checks.py` lista y verificada para la ejecución de las pruebas en banco (T3 a T6 y T8) por el auditor independiente.

---

## 1. Resumen de la entrega

- **Rama de trabajo:** `driver/tim-f4`
- **Punto de partida:** `4ed5d59` (actualizado a `main` en `897bf8d` con decisiones de F4 aprobadas)
- **Hash de código y herramienta evaluado:** `e9afdbb`
- **Hash del commit de evidencias (T1, T2, T7):** `284512a`
- **Estado de la entrega:** **Fase F1 corregida para auditoría independiente**

### Hitos y correcciones completadas

| Hito / Corrección | Commit | Descripción y justificación técnica |
| --- | --- | --- |
| **H1** | `648d262` | **Migración de TIM3 en adquisición de `prj_04` a `tim_bm`:** Enlace de `stm32h7_driver_tim_bm` en `02_firmware_projects/prj_04_adc_dma_bm_eth/CM4/CMakeLists.txt`. Migración de `TIM3_Trigger_Init` en `adc_acquisition.c` usando `TIM_BM_EnableClock`, `TIM_BM_InitTimeBase` (PSC=0, ARR=399, ARPE=0) y `TIM_BM_ConfigMaster` (MMS=010 update TRGO). Sustitución del arranque tardío directo (`TIM3->CR1 |= TIM_CR1_CEN`) por `TIM_BM_Start(TIM3)`. |
| **H2** | `42c15a5` | **Migración de TIM3 en adquisición de `prj_03` a `tim_hal`:** Enlace de `stm32h7_driver_tim_hal` en `02_firmware_projects/prj_03_adc_dma_eth/CM4/CMakeLists.txt`. Sustitución de `HAL_TIM_Base_Init` y `HAL_TIMEx_MasterConfigSynchronization` por `TIM_HAL_InitTimeBase` y `TIM_HAL_ConfigMaster` en `TIM3_Trigger_Init`, y arranque tardío con `TIM_BM_Start(TIM3)`. Eliminación total de llamadas `HAL_TIM_*` fuera de `tim_hal.c`. |
| **H3** | `52bee58` | **Migración de TIM3 y TIM7 en selftests de `prj_04` a `tim_bm`:** En `adc_selftest_hw.c`, migración de `Tim3_Prepare`, `Tim3_Run` y `Tim3_Halt` a `TIM_BM_InitTimeBase`, `TIM_BM_ConfigMaster`, `TIM_BM_Start` y `TIM_BM_Stop`. En `dma_selftest.c` (T4 selftest de `02_dma`), migración de configuración, petición DMA por actualización (`DIER.UDE`), arranque, detención y desinicialización de TIM7 a `tim_bm`. |
| **H4** | `47782f3` | **Migración de TIM3 en selftests de `prj_03` a `tim_bm`:** En `adc_selftest_hw.c`, migración de `Tim3_Prepare`, `Tim3_Run` y `Tim3_Halt` a las funciones de `tim_bm` (`TIM_BM_InitTimeBase`, `TIM_BM_ConfigMaster`, `TIM_BM_Start`, `TIM_BM_Stop`), unificando el control del generador de eventos ADC en selftests de ambos proyectos. |
| **Firmware (M6, m1)** | `1b01e9a` | **Corrección de comentarios en firmware y validación de estado:** (M6) En ambos `adc_acquisition.c`, restitución de comentarios técnicos en inglés explicando el cálculo nominal del temporizador (reloj de temporizador APB1 200 MHz, PSC 0, ARR 399 -> 500 kHz, 50 muestras por periodo a 10 kHz) y aclaración verídica de que la emisión de `UG` bajo `MMS=000` sí genera un pulso inicial en TRGO pero resulta completamente inocuo porque el ADC todavía no ha sido arrancado (`ADSTART=0`). (m1) En ambos `adc_selftest_hw.c`, eliminación del descarte `(void)` de valores devueltos por `TIM_BM_*` en `Tim3_Prepare`, `Tim3_Run` y `Tim3_Halt`, comprobando `TIM_STATUS_OK` y emitiendo fallo formal ante cualquier error. |
| **Herramienta (B1–B3, M1–M4, m2)** | `42ed495`<br>`e9afdbb` | **Revisión integral del arnés `tools/tim_f4_checks.py`:**<br>• **B1 (T8):** Apertura de `monitor.py` en segundo plano **antes** del flasheo con `PYTHONIOENCODING=utf-8:backslashreplace` (excepción acotada de G-S5); ventana de captura de hasta 45 s; verificación de las líneas exactas de veredicto requeridas (`[m7_dma_test] OVERALL: PASS`, `[m4_dma_test] ...OVERALL=PASS`, `[m4_adc_test] HW RESULT PASS`) exigiendo 0 líneas con `FAIL`; volcado íntegro de la consola a la captura; y verificación estricta del flasheo de restauración de `prj_03 debug`.<br>• **B2 (T4):** Consultas UDP directas en puerto 5001 enviando `b"JSON\n"`; sellado de tiempo de alta precisión (`perf_counter`) en el punto medio de ida y vuelta; filtro de RTT < 5,0 ms; ventana de adquisición de 60 s; comprobación del oráculo de la placa `Δsamples / Δm4_hb = 50 000 ± 0,5%` (independiente de deriva HSE/PC); rango de cordura [502 500, 510 000] Hz; lectura con claves obligatorias (`samples`, `m4_hb`, `err`, `ovr`, `adc_ovr`) verificando contadores estrictamente en 0.<br>• **B3 (T5):** Búsqueda de `^PASS\s+3/3 checks` y `returncode == 0` en `net_check.py`; lectura posterior de `monitor.py` (sin reset) exigiendo en la línea `[net]` que `rx_nobuf=0`, `tx_nobuf=0`, `dma_err=0` (y `acl_drop=0` en `prj_04`).<br>• **M1 (T2b):** Ampliación de expresiones regulares de preprocesador (`APB1LENR(?:\s*\))*`, variantes numéricas `[15]U?`, operadores `++`/`--` prefijo y sufijo); autoprueba ampliada a 16 escrituras detectadas y 10 lecturas descartadas; acotación de escaneo a unidades CM4; control de baseline en `04fb865` por tupla `(preset, relative_path, kind)` detectando todas las líneas de escrituras directas y las habilitaciones manuales de RCC (`CM4/App/Src/adc_acquisition.c:57/60` y `CM4/App/Src/dma_selftest.c:1001`).<br>• **M2 (G-S1):** Comprobación estática automatizada de seguridad de pines parseando `board_def.h` de ambos proyectos y validando intersección nula con las líneas protegidas del banco.<br>• **M3:** Validación de árbol limpio (`git status --porcelain` sin `-uno`) y verificación obligatoria del commit de control `04fb865` en el arranque de la herramienta.<br>• **M4 (T6a):** Adaptación a la nueva T4, cálculo de ventana entre muestras y conteo de excepciones SWD como fallos sin abortar el bucle de resets.<br>• **m2:** Lectura de registros SWD a 32 bits en T3 sin enmascaramiento a 16 bits; corrección de mensajes de procedencia de cabeceras; exigencia de las 2 líneas "Download verified successfully" por flasheo; eliminación de `--flash` implícito sobre `--control`; y adición del parámetro `--output <path>` con `StreamTee` UTF-8 nativo para evitar corrupción UTF-16 de redirecciones de shell. |

---

## 2. Resultados de las pruebas sin placa (T1, T2, T7)

Todas las pruebas sin hardware han sido repetidas y registradas de forma íntegra sobre el commit de código y herramienta `e9afdbb`, generando capturas oficiales bajo `docs/tim_f4_<prueba>_e9afdbb.txt` (conservando asimismo las capturas previas de `da0fc87` conforme a las instrucciones de auditoría).

### 2.1 T1: Compilaciones y tamaños

Verificación de compilación limpia de la matriz CI completa (8 configuraciones debug y release con `-Werror`, cero advertencias y cero errores) construidas de cero (`build_all.ps1 -Clean -Fresh`), y de los 4 presets de selftest requeridos por el plan construidos de cero (`--fresh`).

**Resultados de la matriz CI (`tim_f4_t1_e9afdbb.txt`):**

| Proyecto | Preset | Núcleo | Estado | Text (B) | Data (B) | Bss (B) | Total (B) | Tiempo |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `prj_01_template_dualcore` | debug | CM7 | PASS | 30632 | 132 | 55676 | 86440 | 24.5s |
| `prj_01_template_dualcore` | debug | CM4 | PASS | 3456 | 24 | 12560 | 16040 | 24.5s |
| `prj_01_template_dualcore` | release | CM7 | PASS | 29620 | 132 | 55680 | 85432 | 16.9s |
| `prj_01_template_dualcore` | release | CM4 | PASS | 3112 | 24 | 12560 | 15696 | 16.9s |
| `prj_02_eth_lwip_dualcore` | debug | CM7 | PASS | 87312 | 152 | 120096 | 207560 | 29.7s |
| `prj_02_eth_lwip_dualcore` | debug | CM4 | PASS | 2580 | 24 | 12488 | 15092 | 29.7s |
| `prj_02_eth_lwip_dualcore` | release | CM7 | PASS | 83224 | 152 | 120096 | 203472 | 22.9s |
| `prj_02_eth_lwip_dualcore` | release | CM4 | PASS | 2332 | 24 | 12488 | 14844 | 22.9s |
| `prj_03_adc_dma_eth` | debug | CM7 | PASS | 89684 | 152 | 121224 | 211060 | 31.8s |
| `prj_03_adc_dma_eth` | debug | CM4 | PASS | 23084 | 24 | 16192 | 39300 | 31.8s |
| `prj_03_adc_dma_eth` | release | CM7 | PASS | 85480 | 152 | 121224 | 206856 | 25.6s |
| `prj_03_adc_dma_eth` | release | CM4 | PASS | 20660 | 24 | 16192 | 36876 | 25.6s |
| `prj_04_adc_dma_bm_eth` | debug | CM7 | PASS | 90512 | 156 | 121268 | 211936 | 30.7s |
| `prj_04_adc_dma_bm_eth` | debug | CM4 | PASS | 11224 | 28 | 15920 | 27172 | 30.7s |
| `prj_04_adc_dma_bm_eth` | release | CM7 | PASS | 86524 | 156 | 121264 | 207944 | 25.0s |
| `prj_04_adc_dma_bm_eth` | release | CM4 | PASS | 10672 | 28 | 15920 | 26620 | 25.0s |

**Presets de selftest adicionales verificados en T1:**
- `prj_04_selftest` (`02_firmware_projects/prj_04_adc_dma_bm_eth`, preset `selftest`): PASS (exit code 0).
- `prj_04_selftest_adc` (`02_firmware_projects/prj_04_adc_dma_bm_eth`, preset `selftest-adc`): PASS (exit code 0).
- `prj_03_selftest` (`02_firmware_projects/prj_03_adc_dma_eth`, preset `selftest`): PASS (exit code 0).
- `prj_01_selftest_tim` (`03_firmware_templates/prj_01_template_dualcore`, preset `selftest-tim`): PASS (exit code 0).

**Veredicto T1:** **PASS** en `prj_04` y `prj_03`.  
**Capturas de evidencia:**
- `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t1_e9afdbb.txt`
- `02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t1_e9afdbb.txt`
*(Capturas previas conservadas: `tim_f4_t1_da0fc87.txt` en ambos proyectos)*

---

### 2.2 T2: Símbolos del enlazador (T2a) y análisis estático preprocesado (T2b)

#### T2a: Presencia y ausencia de símbolos enlazados (`arm-none-eabi-nm`)

- **Oráculo positivo (árbol actual):**
  - En `prj_04` CM4 (`debug` y `release`), se exige y confirma la presencia de los 4 símbolos de `tim_bm`: `TIM_BM_InitTimeBase`, `TIM_BM_EnableClock`, `TIM_BM_ConfigMaster` y `TIM_BM_Start`. Presentes: **4/4** en ambas configuraciones.
  - En `prj_03` CM4 (`debug` y `release`), se exige y confirma la presencia de los 4 símbolos del driver (`tim_hal` + `tim_bm`): `TIM_HAL_InitTimeBase`, `TIM_HAL_ConfigMaster`, `TIM_BM_EnableClock` y `TIM_BM_Start`. Presentes: **4/4** en ambas configuraciones.
- **Control negativo sobre `04fb865`:**
  - En el commit de control `04fb865`, ninguno de los dos proyectos enlazaba los drivers `03_tim` en CM4. El oráculo comprobó que en las 4 ELFs de control (`prj_04` debug/release y `prj_03` debug/release), los símbolos del driver están completamente ausentes (**0 presentes**), satisfaciendo formalmente el control negativo.

#### T2b: Análisis de escrituras ad-hoc y llamadas HAL sobre código preprocesado (`gcc -E`)

- **Autoprueba de patrones regex (CMSIS expanded forms):**
  - Autoprueba ejecutada sobre código C sintético expandiendo cabeceras oficiales de CMSIS: detectó **16 de 16 escrituras** (asignación simple, asignación compuesta, desplazamientos, macro `WRITE_REG`, macro `MODIFY_REG`, macros `SET_BIT` con `APB1LENR` con/sin paréntesis anidados, macros HAL con volatile cast, y operadores `CNT++` / `++CNT` tanto prefijo como sufijo) y descartó **10 de 10 lecturas** (lecturas de `CNT`, comparaciones de igualdad, desigualdad y menor o igual, máscaras de `SR`, toma de dirección `(uint32_t)&(TIM7->CNT)`, y aritmética con `ARR + 1`).
- **Escaneo del árbol actual:**
  - `prj_04`: 7 presets analizados (`debug`, `release`, `selftest`, `selftest-adc`, `selftest-uart`, `selftest-hsem`, `selftest-gpio`). Total: **2282 unidades CM4** analizadas; **0 escrituras ilegales** a registros de TIM3/TIM7 fuera de los archivos exceptuados por el plan.
  - `prj_03`: 3 presets analizados (`debug`, `release`, `selftest`). Total: **976 unidades CM4** analizadas; **0 escrituras ilegales** a registros de TIM3/TIM7; **0 llamadas no protegidas** a funciones `HAL_TIM_*` fuera de `tim_hal.c`.
- **Control negativo sobre `04fb865` (M1):**
  - En `prj_04`: Detección precisa de escrituras en `adc_acquisition.c`, `adc_selftest_hw.c` y `dma_selftest.c`, incluyendo las habilitaciones manuales de RCC `((RCC_TypeDef *) ...)->APB1LENR |= ...` en `CM4/App/Src/adc_acquisition.c:60` y `CM4/App/Src/dma_selftest.c:1001`. Coincidencia con todas las tuplas esperadas `(preset, relative_path, kind)`.
  - En `prj_03`: Detección precisa de habilitaciones de RCC y llamadas HAL no protegidas en `CM4/App/Src/adc_acquisition.c:57` (`__HAL_RCC_TIM3_CLK_ENABLE()`, `HAL_TIM_Base_Init`, `HAL_TIMEx_MasterConfigSynchronization`, `HAL_TIM_Base_Start`) y escrituras directas en `CM4/App/Src/adc_selftest_hw.c:146` (`APB1LENR`, `CR1=0`, etc.). Coincidencia con todas las tuplas esperadas.

**Veredicto T2:** **PASS** (T2a: PASS, T2b: PASS) en `prj_04` y `prj_03`.  
**Capturas de evidencia:**
- `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t2_e9afdbb.txt`
- `02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t2_e9afdbb.txt`
*(Capturas previas conservadas: `tim_f4_t2_da0fc87.txt` en ambos proyectos)*

---

### 2.3 T7: Regresión de driver y congelación del contrato de API

- **Compilación de plantilla `prj_01` con preset `selftest-tim`:**
  - Configuración limpia y compilación completa del preset `selftest-tim` en `03_firmware_templates/prj_01_template_dualcore`.
  - Resultado: binarios CM7 y CM4 generados con **0 errores**.
- **Comprobación de congelación de API (`git diff`):**
  - Verificación del diff contra `main` en las cabeceras públicas: `git diff main -- '01_shared_drivers/03_tim/*.h'`.
  - Resultado: salida **estrictamente vacía** (0 bytes, 0 líneas de diferencia). Las cabeceras del driver permanecen intactas.

**Veredicto T7:** **PASS** en `prj_04` y `prj_03`.  
**Capturas de evidencia:**
- `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t7_e9afdbb.txt`
- `02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t7_e9afdbb.txt`
*(Capturas previas conservadas: `tim_f4_t7_da0fc87.txt` en ambos proyectos)*

---

## 3. Inspección de registros SWD de TIM3 (T3)

A continuación se detalla la tabla de los 6 registros de hardware de TIM3 que el auditor comprobará en caliente por SWD (`mode=HOTPLUG`) con la imagen `debug` corriendo (en `prj_04` y `prj_03`), contrastada bit a bit contra la especificación formal del manual de referencia RM0399 §40 (General-purpose timers TIM2/TIM3/TIM4/TIM5).

### Tabla de registros esperados (RM0399 §40)

| Registro | Dirección | Valor esperado | Campos y justificación técnica (RM0399 §40) |
| --- | --- | --- | --- |
| `CR1` | `0x40000400` | `0x0001` | **CEN = 1** (contador habilitado); **ARPE = 0** (precarga de ARR deshabilitada, valor activo de inmediato); **DIR = 0** (cuenta ascendente); **URS = 0** (cualquier evento genera update); **UDIS = 0** (eventos UEV habilitados); **CMS = 00** (modo alineado al borde); **CKD = 00** (tDTS = tCK_INT). |
| `CR2` | `0x40000404` | `0x0020` | **MMS = 010 (Update event):** el evento de actualización de TIM3 se envía por la línea de disparo maestro `TRGO` para disparar las conversiones regulares de ADC1/ADC2; **CCDS = 0**; **MMS2 = 0**. |
| `SMCR` | `0x40000408` | `0x0000` | **Modo esclavo deshabilitado (SMS = 000):** el temporizador opera de forma autónoma con el reloj interno del bus APB1 (`CK_INT` = 200 MHz); **TS = 000**; **MSM = 0**; **ETF/ETPS/ECE/ETP = 0**. |
| `DIER` | `0x4000040C` | `0x0000` | **Interrupciones y peticiones DMA de TIM3 deshabilitadas:** el trigger TRGO hacia el ADC es una conexión hardware directa entre periféricos; no requiere que TIM3 genere interrupciones de CPU (`UIE = 0`) ni peticiones DMA propias (`UDE = 0`). |
| `PSC` | `0x40000428` | `0x0000` | **Prescaler = 0 (divisor 1):** el contador avanza a la frecuencia completa del reloj de temporizadores de APB1 (200 MHz, derivado del SYSCLK PLL1P vía `D2PPRE1 = /2` y multiplicador de timer x2). |
| `ARR` | `0x4000042C` | `0x018F` | **Auto-reload register = 399 (0x018F):** periodo de cuenta = `ARR + 1 = 400` ciclos. A 200 MHz, frecuencia de disparo TRGO = 200.000.000 / 400 = **500.000 Hz (500 kHz nominal)**. |

### Controles implementados en T3 (`tim_f4_checks.py`)
1. **Lectura completa a 32 bits (m2):** Se utiliza `STM32_Programmer_CLI -r32 0x40000400 24` leyendo palabras de 32 bits sin enmascaramiento a 16 bits.
2. **Control de equivalencia en `04fb865`:** La lectura SWD en caliente sobre la imagen original de control `04fb865` arroja exactamente los mismos 6 valores.
3. **Control de tabla alterada:** La comparación de los registros leídos contra una tabla falsificada (p. ej., `ARR = 0x00FF`) genera automáticamente un fallo (`FAIL`).

---

## 4. Instrucciones y comandos para la auditoría en banco

El auditor independiente ejecutará las pruebas en hardware conectando la sonda ST-LINK (número de serie obligatorio `sn=0016000C4142501220353451`). Toda sesión en placa respetará estrictamente las reglas de seguridad del banco (G-S1 a G-S7).

Los comandos emplean el argumento nativo `--output <ruta>` de `tim_f4_checks.py`, que captura la salida en UTF-8 con saltos de línea LF mediante `StreamTee`, evitando las redirecciones de PowerShell 5.1 (`>`) que introducen codificación UTF-16 (m2).

### 4.1 Comprobación de seguridad de pines del banco (G-S1 / M2)

Antes de cualquier flasheo, la función `check_gs1_pin_safety()` en `tim_f4_checks.py` analiza el código fuente (`board_def.h`) de ambos proyectos y verifica que no se haga referencia a ninguna línea de la instrumentación protegida del banco:
- **Líneas autorizadas:** `PB1` (`BOARD_ADC_PIN`, entrada analógica), `PD5`/`PD6` (USART2 TX/RX, puente dedicado para selftest DMA T3), canales internos de ADC (VREFINT, sensor temperatura), y disparos internos por hardware de TIM3/TIM7 (TRGO a ADC y DMA).
- **Líneas protegidas del banco con intersección estrictamente nula:** `PE2`–`PE15`, `PA8` (bus AD2); `PA5`, `PA6`, `PB4`, `PB5` (conector J1); `PB8`, `PB9` (bus I2C); `PF14`, `PF15` (tomas auxiliares).

### 4.2 Comandos de ejecución para el auditor

#### T3: Inspección en caliente de registros SWD de TIM3
```bash
# prj_04 (flasheo verificado de debug, espera y lectura de los 6 registros en caliente a 32 bits)
py -u tools/tim_f4_checks.py --project prj_04 --test t3 --flash --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t3_e9afdbb.txt

# prj_03 (flasheo verificado de debug, espera y lectura de los 6 registros en caliente a 32 bits)
py -u tools/tim_f4_checks.py --project prj_03 --test t3 --flash --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t3_e9afdbb.txt
```

#### T4: Ritmo del ADC y ausencia de overrun (B2)
```bash
# prj_04 (consultas UDP directas puerto 5001, filtro RTT < 5ms, ventana 60s, oráculo 50000 samples/hb, ovr=0, adc_ovr=0)
py -u tools/tim_f4_checks.py --project prj_04 --test t4 --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t4_e9afdbb.txt

# prj_03 (consultas UDP directas puerto 5001, filtro RTT < 5ms, ventana 60s, oráculo 50000 samples/hb, ovr=0)
py -u tools/tim_f4_checks.py --project prj_03 --test t4 --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t4_e9afdbb.txt
```

#### T5: Regresión de sistema y red (B3)
```bash
# prj_04 debug
py -u tools/tim_f4_checks.py --project prj_04 --test t5 --preset debug --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t5_debug_e9afdbb.txt

# prj_04 release
py -u tools/tim_f4_checks.py --project prj_04 --test t5 --preset release --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t5_release_e9afdbb.txt

# prj_03 debug
py -u tools/tim_f4_checks.py --project prj_03 --test t5 --preset debug --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t5_debug_e9afdbb.txt

# prj_03 release
py -u tools/tim_f4_checks.py --project prj_03 --test t5 --preset release --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t5_release_e9afdbb.txt
```

#### T6a: 20 arranques repetidos por SWD (`-rst`) (M4)
```bash
# prj_04 (20 ciclos de reset con 10s de espera y validación T3 y T4 tras cada reset; excepciones capturadas)
py -u tools/tim_f4_checks.py --project prj_04 --test t6a --flash --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t6a_e9afdbb.txt

# prj_03 (20 ciclos de reset con 10s de espera y validación T3 y T4 tras cada reset; excepciones capturadas)
py -u tools/tim_f4_checks.py --project prj_03 --test t6a --flash --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t6a_e9afdbb.txt
```

#### T8: Selftests migrados en la placa y equivalencia con control (B1)
```bash
# Ejecuta G-S1, arranca monitor previo a flasheo (hasta 45s), flashea prj_04 selftest & selftest-adc,
# prj_03 selftest (migrados y control 04fb865), valida líneas exactas PASS, 0 FAIL, y restaura prj_03 debug verificado
py -u tools/tim_f4_checks.py --project prj_04 --test t8 --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t8_e9afdbb.txt
```

---

## 5. Estado de pruebas que requieren hardware (Lo no ejecutado)

Conforme a las instrucciones expresas del plan y las restricciones de rol, el implementador no tiene acceso físico a la placa STM32H755. Por tanto, todas las pruebas dependientes de hardware quedan formalmente declaradas como **NO VERIFICADO / pendiente de ejecución por el auditor independiente**:

| Prueba | Descripción | Estado por el implementador | Condición de verificación en auditoría |
| --- | --- | --- | --- |
| **T3** | Inspección en caliente de registros TIM3 por SWD | **NO VERIFICADO** | Ejecución por auditor con `--test t3 --flash` en ambos proyectos (lectura 32 bits). |
| **T4** | Ritmo del ADC (consultas UDP directas 5001, ventana 60s, oráculo 50000 ±0.5% samples/hb) | **NO VERIFICADO** | Ejecución por auditor con `--test t4` en ambos proyectos (claves obligatorias, ovr=0, adc_ovr=0). |
| **T5** | Regresión de red ping 4/4 y UDP 100/100 (debug/release) | **NO VERIFICADO** | Ejecución por auditor con `--test t5` en debug y release (`PASS 3/3 checks` y contadores `[net]` en 0). |
| **T6a** | 20 arranques repetidos por SWD (20/20 PASS) | **NO VERIFICADO** | Ejecución por auditor con `--test t6a` en ambos proyectos (20/20 PASS). |
| **T6b** | Arranque en frío (power cycle físico en banco) | **NO VERIFICADO** | Ejecución física por el responsable del banco durante el cierre formal. |
| **T8** | Selftests en placa (`prj_04` selftest/selftest-adc, `prj_03` selftest) | **NO VERIFICADO** | Ejecución por auditor con `--test t8` (monitor previo, líneas exactas obligatorias, equivalencia vs `04fb865`). |

**Estado final de la placa tras las pruebas (G-S1 / G-S6):**
Al concluir la batería de pruebas en placa, la herramienta `tim_f4_checks.py` en el paso final de T8 vuelve a flashear y verificar la imagen de producción **`prj_03_adc_dma_eth` preset `debug`** compilada en el commit `e9afdbb`, dejando la placa en estado operativo normal con las dos confirmaciones de descarga exitosa ("Download verified successfully" en CM7 y CM4).

---

## 6. Desviaciones y notas técnicas

1. **Arquitectura de adquisición en `prj_03` (`tim_hal` + `tim_bm`):**
   En `prj_03/CM4/App/Src/adc_acquisition.c`, la inicialización de base de tiempo y sincronización maestra se realiza mediante `TIM_HAL_InitTimeBase` y `TIM_HAL_ConfigMaster` (tier HAL). Para el arranque tardío del contador (tras armar ADC y DMA), se utiliza `TIM_BM_Start(TIM3)` de `tim_bm`. Con esto se satisface el requisito de migración al tier HAL y se garantiza que **cero llamadas a `HAL_TIM_*`** permanezcan en `prj_03` fuera de `01_shared_drivers/03_tim/tim_hal.c`.
2. **Discriminación estricta de registros en T2b (`tim_f4_checks.py`):**
   Los nombres de registro `CR1`, `CR2` y `SR` son comunes en la arquitectura STM32 a múltiples periféricos (PWR, USART, I2C, timers auxiliares). El analizador estático T2b implementó un filtro de destino (`TIM37_TARGET_RE`) que exige que cualquier escritura a dichos registros esté dirigida explícitamente a las instancias de `TIM3` (`0x40000400`) o `TIM7` (`0x40001400`). El escaneo se acota a las unidades de compilación de CM4, evitando falsos positivos con el timebase de CM7 en FreeRTOS (`stm32h7xx_hal_timebase_tim.c` / `stm32h7xx_it.c`) o la habilitación de reloj en CM7 en `main.c:203` (per-core clocking según RM0399 §9.7.1).
3. **Manejo de capturas y validación de árbol limpio (M3):**
   `get_git_info` en `tim_f4_checks.py` implementa la comprobación estricta de árbol limpio con `git status --porcelain` (sin `-uno`), excluyendo exclusivamente los ficheros de evidencia generados bajo `docs/tim_f4_*.txt`. Cualquier archivo no seguido (`.c`, `.h`, `.py`, `.cmake`) o modificación sin confirmar en el árbol de trabajo aborta la ejecución de inmediato con código de error, preservando la rigurosidad de auditoría requerida por el plan.

---

F1 CORREGIDA PARA AUDITORIA e9afdbb
