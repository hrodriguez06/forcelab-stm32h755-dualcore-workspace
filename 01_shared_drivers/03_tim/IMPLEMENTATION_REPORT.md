# Informe de implementación de `03_tim` F4 (Migración de TIM3 y TIM7 a `tim_bm` / `tim_hal`)

Oct 15, 2026 · Rama `driver/tim-f4` · Estado: **Fase F1 corregida (ronda 2) para auditoría independiente**

Este informe documenta la implementación, correcciones tras la segunda ronda de auditoría y verificación sin placa de la fase F4 del driver `03_tim` en el repositorio STM32H755, conforme a `01_shared_drivers/03_tim/IMPLEMENTATION_PLAN.md` (revisión 4), las directrices de `01_shared_drivers/03_tim/IMPLEMENTER_INSTRUCTIONS.md` y las auditorías registradas en `01_shared_drivers/03_tim/AUDIT.md`.

Conforme a las instrucciones del responsable del banco y las restricciones de rol del implementador (sin acceso físico a hardware), se han completado e integrado todos los hitos de código y herramientas (H1 a H5), se han implementado las correcciones para los hallazgos de la segunda ronda de auditoría (N1, B1, B2, B3, M1, M2, M3, m2, k1, k2), se han repetido y registrado las pruebas sin hardware (T1, T2 y T7 con capturas UTF-8 sobre el commit `1f8876b`), y se entrega la herramienta automatizada `tools/tim_f4_checks.py` lista y verificada para la ejecución de las pruebas en banco (T3 a T6 y T8) por el auditor independiente.

---

## 1. Resumen de la entrega

- **Rama de trabajo:** `driver/tim-f4`
- **Punto de partida:** `4ed5d59` (actualizado a `main` en `897bf8d` con decisiones de F4 aprobadas)
- **Hash de código y herramienta evaluado:** `1f8876b`
- **Hash del commit de evidencias (T1, T2, T7):** `57744b5`
- **Estado de la entrega:** **Fase F1 corregida (ronda 2) para auditoría independiente**

### Hitos y correcciones completadas

| Hito / Corrección | Commit | Descripción y justificación técnica |
| --- | --- | --- |
| **H1** | `648d262` | **Migración de TIM3 en adquisición de `prj_04` a `tim_bm`:** Enlace de `stm32h7_driver_tim_bm` en `02_firmware_projects/prj_04_adc_dma_bm_eth/CM4/CMakeLists.txt`. Migración de `TIM3_Trigger_Init` en `adc_acquisition.c` usando `TIM_BM_EnableClock`, `TIM_BM_InitTimeBase` (PSC=0, ARR=399, ARPE=0) y `TIM_BM_ConfigMaster` (MMS=010 update TRGO). Sustitución del arranque tardío directo (`TIM3->CR1 \|= TIM_CR1_CEN`) por `TIM_BM_Start(TIM3)`. |
| **H2** | `42c15a5` | **Migración de TIM3 en adquisición de `prj_03` a `tim_hal`:** Enlace de `stm32h7_driver_tim_hal` en `02_firmware_projects/prj_03_adc_dma_eth/CM4/CMakeLists.txt`. Sustitución de `HAL_TIM_Base_Init` y `HAL_TIMEx_MasterConfigSynchronization` por `TIM_HAL_InitTimeBase` y `TIM_HAL_ConfigMaster` en `TIM3_Trigger_Init`, y arranque tardío con `TIM_BM_Start(TIM3)`. Eliminación total de llamadas `HAL_TIM_*` fuera de `tim_hal.c`. |
| **H3** | `52bee58` | **Migración de TIM3 y TIM7 en selftests de `prj_04` a `tim_bm`:** En `adc_selftest_hw.c`, migración de `Tim3_Prepare`, `Tim3_Run` y `Tim3_Halt` a `TIM_BM_InitTimeBase`, `TIM_BM_ConfigMaster`, `TIM_BM_Start` y `TIM_BM_Stop`. En `dma_selftest.c` (T4 selftest de `02_dma`), migración de configuración, petición DMA por actualización (`DIER.UDE`), arranque, detención y desinicialización de TIM7 a `tim_bm`. |
| **H4** | `47782f3` | **Migración de TIM3 en selftests de `prj_03` a `tim_bm`:** En `adc_selftest_hw.c`, migración de `Tim3_Prepare`, `Tim3_Run` y `Tim3_Halt` a las funciones de `tim_bm` (`TIM_BM_InitTimeBase`, `TIM_BM_ConfigMaster`, `TIM_BM_Start`, `TIM_BM_Stop`), unificando el control del generador de eventos ADC en selftests de ambos proyectos. |
| **Firmware R1 (M6, m1)** | `1b01e9a` | **Corrección de comentarios en firmware y validación de estado:** (M6) En ambos `adc_acquisition.c`, restitución de comentarios técnicos en inglés explicando el cálculo nominal del temporizador (reloj de temporizador APB1 200 MHz, PSC 0, ARR 399 -> 500 kHz, 50 muestras por periodo a 10 kHz) y aclaración verídica de que la emisión de `UG` bajo `MMS=000` sí genera un pulso inicial en TRGO pero resulta inocuo porque el ADC todavía no ha sido arrancado (`ADSTART=0`). (m1) En ambos `adc_selftest_hw.c`, eliminación del descarte `(void)` de valores devueltos por `TIM_BM_*` en `Tim3_Prepare`, `Tim3_Run` y `Tim3_Halt`, comprobando `TIM_STATUS_OK` y emitiendo fallo formal ante cualquier error. |
| **Firmware R2 (k1, k2)** | `2936c1e` | **Habilitación de reloj antes de parada y cita RM0399:** (k1) En `adc_selftest_hw.c` (ambos proyectos), `Tim3_Prepare` ejecuta `TIM_BM_EnableClock(TIM3)` **antes** de `TIM_BM_Stop(TIM3)`, garantizando que el acceso al periférico para detenerlo se realice con el reloj habilitado en el bus APB1. (k2) Corrección de la cita a RM0399 de §40 a §41 (TIM2–TIM5, TIMx_CR2 en §41.4.2) en comentarios de código y documentación. |
| **Herramienta R1** | `42ed495`<br>`e9afdbb` | Implementación base de pruebas automatizadas y ajustes de la primera ronda de auditoría. |
| **Herramienta R2 (N1, B1–B3, M1–M3, m2)** | `1f8876b` | **Corrección de hallazgos de auditoría (ronda 2):**<br>• **N1:** `flash_image` verifica `returncode == 0` y `count("Download verified successfully") >= 1` (plan rev 4, una línea por llamada unificada a CubeProgrammer).<br>• **B1 (T8):** Apertura de monitor con hilo lector desacoplado mediante cola y límite estricto de 45,0 s; descarte de líneas de consola previas al banner de arranque de CM7 (`CM7 Booting`); detención de flasheos sucesivos ante un fallo real en `flash.py` (G-S7); y comprobación del flasheo de restauración final de `prj_03 debug`.<br>• **B2 (T4):** Secuencia de equivalencia comparando imagen migrada frente a control `04fb865` en la misma sesión ($(\|r_{\text{mig}} - r_{\text{ctrl}}\| / r_{\text{ctrl}}) \le 0,1\%$); verificación de claves obligatorias (`samples`, `m4_hb`, `err`, `ovr`, `adc_ovr`); controles negativos con tasa desplazada (+1%) y oráculo desplazado (40 000 samples/hb); restauración de la imagen evaluada al concluir.<br>• **B3 (T5):** Aumento de la ventana de captura de `monitor.py` a 15,0 s; comprobación obligatoria de presencia de las claves `rx_nobuf`, `tx_nobuf`, `dma_err` (y `acl_drop` en `prj_04`) exigiendo valor estrictamente 0.<br>• **M1 (T2b):** Inclusión del escaneo de unidades de CM7 sin omisiones; identificación de `CM7/Core/Src/main.c:203` (`__HAL_RCC_TIM3_CLK_ENABLE()`) como hallazgo conocido y explicado conforme a la regla D y RM0399 §9.7.1; ampliación de regex `TIM37_TARGET_RE` con `\bInstance\b`; comprobación de `(?:HAL_TIM\|__HAL_TIM)` descartando llamadas legítimas de timebase en `prj_03` (`htim6`/`TIM6`); normalización de líneas en blanco evitando desvíos; autoprueba ampliada a 20 escrituras y 12 lecturas.<br>• **M2 (G-S1):** Verificación manual de seguridad de pines documentada en informe y código conforme a la especificación del banco.<br>• **M3:** `get_git_info` comprueba árbol limpio con `git status --porcelain` ignorando exclusivamente archivos no seguidos (`??`) de capturas bajo `docs/tim_f4_*.txt`.<br>• **m2:** Etiquetado dinámico de ciclos en T6a (`{reset_count}/{reset_count}`); captura de excepciones SWD sin abortar el bucle; diff de T7 estrictamente contra `main` (`git diff main -- '01_shared_drivers/03_tim/*.h'`); `--control` fuerza `--flash` en pruebas de placa; y `--test all` restaura `prj_03 debug` en placa. |

---

## 2. Resultados de las pruebas sin placa (T1, T2, T7)

Todas las pruebas sin hardware han sido repetidas y registradas de forma íntegra sobre el commit de código y herramienta `1f8876b`, generando capturas oficiales bajo `docs/tim_f4_<prueba>_1f8876b.txt` (conservando asimismo las capturas previas de `da0fc87` y `e9afdbb` conforme a las instrucciones de auditoría).

### 2.1 T1: Compilaciones y tamaños

Verificación de compilación limpia de la matriz CI completa (8 configuraciones debug y release construidas de cero con `build_all.ps1 -Clean -Fresh`, con `-Werror` en `prj_03` y `prj_04`), y de los 4 presets de selftest requeridos por el plan construidos de cero (`--fresh`).

**Resultados de la matriz CI (`tim_f4_t1_1f8876b.txt`):**

| Proyecto | Preset | Núcleo | Estado | Text (B) | Data (B) | Bss (B) | Total (B) | Tiempo |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `prj_01_template_dualcore` | debug | CM7 | PASS | 30632 | 132 | 55676 | 86440 | 25.5s |
| `prj_01_template_dualcore` | debug | CM4 | PASS | 3456 | 24 | 12560 | 16040 | 25.5s |
| `prj_01_template_dualcore` | release | CM7 | PASS | 29620 | 132 | 55680 | 85432 | 17.6s |
| `prj_01_template_dualcore` | release | CM4 | PASS | 3112 | 24 | 12560 | 15696 | 17.6s |
| `prj_02_eth_lwip_dualcore` | debug | CM7 | PASS | 87312 | 152 | 120096 | 207560 | 30.6s |
| `prj_02_eth_lwip_dualcore` | debug | CM4 | PASS | 2580 | 24 | 12488 | 15092 | 30.6s |
| `prj_02_eth_lwip_dualcore` | release | CM7 | PASS | 83224 | 152 | 120096 | 203472 | 23.0s |
| `prj_02_eth_lwip_dualcore` | release | CM4 | PASS | 2332 | 24 | 12488 | 14844 | 23.0s |
| `prj_03_adc_dma_eth` | debug | CM7 | PASS | 89684 | 152 | 121224 | 211060 | 34.6s |
| `prj_03_adc_dma_eth` | debug | CM4 | PASS | 23084 | 24 | 16192 | 39300 | 34.6s |
| `prj_03_adc_dma_eth` | release | CM7 | PASS | 85480 | 152 | 121224 | 206856 | 25.9s |
| `prj_03_adc_dma_eth` | release | CM4 | PASS | 20660 | 24 | 16192 | 36876 | 25.9s |
| `prj_04_adc_dma_bm_eth` | debug | CM7 | PASS | 90512 | 156 | 121268 | 211936 | 32.1s |
| `prj_04_adc_dma_bm_eth` | debug | CM4 | PASS | 11224 | 28 | 15920 | 27172 | 32.1s |
| `prj_04_adc_dma_bm_eth` | release | CM7 | PASS | 86524 | 156 | 121264 | 207944 | 24.5s |
| `prj_04_adc_dma_bm_eth` | release | CM4 | PASS | 10672 | 28 | 15920 | 26620 | 24.5s |

**Presets de selftest adicionales verificados en T1:**
- `prj_04_selftest` (`02_firmware_projects/prj_04_adc_dma_bm_eth`, preset `selftest`): PASS (exit code 0).
- `prj_04_selftest_adc` (`02_firmware_projects/prj_04_adc_dma_bm_eth`, preset `selftest-adc`): PASS (exit code 0).
- `prj_03_selftest` (`02_firmware_projects/prj_03_adc_dma_eth`, preset `selftest`): PASS (exit code 0).
- `prj_01_selftest_tim` (`03_firmware_templates/prj_01_template_dualcore`, preset `selftest-tim`): PASS (exit code 0).

**Veredicto T1:** **PASS** en `prj_04` y `prj_03`.  
**Capturas de evidencia:**
- `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t1_1f8876b.txt`
- `02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t1_1f8876b.txt`
*(Capturas previas conservadas: `tim_f4_t1_da0fc87.txt` y `tim_f4_t1_e9afdbb.txt` en ambos proyectos)*

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
  - Autoprueba ejecutada sobre código C sintético expandiendo cabeceras oficiales de CMSIS: detectó **20 de 20 escrituras** (asignación simple, asignación compuesta, desplazamientos, macro `WRITE_REG`, macro `MODIFY_REG`, macros `SET_BIT` con `APB1LENR` con/sin paréntesis anidados, macros HAL con volatile cast, operadores `CNT++` / `++CNT` tanto prefijo como sufijo, accesos a través de handle `s_htim3.Instance->PSC = 0`, `(&s_htim3)->Instance->CR1 |= 1`, macros `__HAL_TIM_SET_AUTORELOAD` y escrituras anidadas en `do { ... } while(0)`) y descartó **12 de 12 lecturas** (lecturas de `CNT`, comparaciones de igualdad, desigualdad y menor o igual, máscaras de `SR`, toma de dirección `(uint32_t)&(TIM7->CNT)`, aritmética con `ARR + 1`, lectura de `s_htim3.Instance->CNT` y comprobaciones condicionales `if (s_htim3.Instance->CR1 == 0)`).
- **Escaneo del árbol actual (M1):**
  - `prj_04`: 7 presets analizados (`debug`, `release`, `selftest`, `selftest-adc`, `selftest-uart`, `selftest-hsem`, `selftest-gpio`). Total: **2282 unidades** (incluyendo unidades de CM7) analizadas; **0 escrituras ilegales** a registros de TIM3/TIM7 fuera de los archivos exceptuados por el plan. Se detectaron 7 hallazgos conocidos y explicados correspondientes a `CM7/Core/Src/main.c:203` (`__HAL_RCC_TIM3_CLK_ENABLE()`), legítima habilitación de reloj por el núcleo maestro CM7 requerida antes de arrancar CM4 (regla D, RM0399 §9.7.1).
  - `prj_03`: 3 presets analizados (`debug`, `release`, `selftest`). Total: **976 unidades** (incluyendo unidades de CM7) analizadas; **0 escrituras ilegales** a registros de TIM3/TIM7; **0 llamadas no protegidas** a funciones `HAL_TIM_*` fuera de `tim_hal.c`.
- **Control negativo sobre `04fb865`:**
  - En `prj_04`: Detección precisa de 78 escrituras directas / llamadas heredadas en las 2275 unidades analizadas de control `04fb865`, incluyendo `adc_acquisition.c`, `adc_selftest_hw.c`, `dma_selftest.c` y `main.c:203`, coincidiendo con todas las 7 tuplas esperadas `(preset, relative_path, kind)`.
  - En `prj_03`: Detección precisa de 23 escrituras directas / llamadas heredadas en las 970 unidades analizadas de control `04fb865` (`adc_acquisition.c:58` y `adc_selftest_hw.c:146`), coincidiendo con todas las 4 tuplas esperadas.

**Veredicto T2:** **PASS** (T2a: PASS, T2b: PASS) en `prj_04` y `prj_03`.  
**Capturas de evidencia:**
- `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t2_1f8876b.txt`
- `02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t2_1f8876b.txt`
*(Capturas previas conservadas: `tim_f4_t2_da0fc87.txt` y `tim_f4_t2_e9afdbb.txt` en ambos proyectos)*

---

### 2.3 T7: Regresión de driver y congelación del contrato de API

- **Compilación de plantilla `prj_01` con preset `selftest-tim`:**
  - Configuración limpia y compilación completa del preset `selftest-tim` en `03_firmware_templates/prj_01_template_dualcore`.
  - Resultado: binarios CM7 y CM4 generados con **0 errores** y **0 advertencias**.
- **Comprobación de congelación de API (`git diff`):**
  - Verificación del diff estrictamente contra la rama base `main`: `git diff main -- '01_shared_drivers/03_tim/*.h'`.
  - Resultado: salida **estrictamente vacía** (0 bytes, 0 líneas de diferencia). Las cabeceras públicas del driver permanecen intactas e inalteradas.

**Veredicto T7:** **PASS** en `prj_04` y `prj_03`.  
**Capturas de evidencia:**
- `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t7_1f8876b.txt`
- `02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t7_1f8876b.txt`
*(Capturas previas conservadas: `tim_f4_t7_da0fc87.txt` y `tim_f4_t7_e9afdbb.txt` en ambos proyectos)*

---

## 3. Inspección de registros SWD de TIM3 (T3)

A continuación se detalla la tabla de los 6 registros de hardware de TIM3 que el auditor comprobará en caliente por SWD (`mode=HOTPLUG`) con la imagen `debug` corriendo (en `prj_04` y `prj_03`), contrastada bit a bit contra la especificación formal del manual de referencia RM0399 §41 (General-purpose timers TIM2/TIM3/TIM4/TIM5).

### Tabla de registros esperados (RM0399 §41)

| Registro | Dirección | Valor esperado | Campos y justificación técnica (RM0399 §41) |
| --- | --- | --- | --- |
| `CR1` | `0x40000400` | `0x0001` | **CEN = 1** (contador habilitado); **ARPE = 0** (precarga de ARR deshabilitada, valor activo de inmediato); **DIR = 0** (cuenta ascendente); **URS = 0** (cualquier evento genera update); **UDIS = 0** (eventos UEV habilitados); **CMS = 00** (modo alineado al borde); **CKD = 00** (tDTS = tCK_INT). |
| `CR2` | `0x40000404` | `0x0020` | **MMS = 010 (Update event, RM0399 §41.4.2):** el evento de actualización de TIM3 se envía por la línea de disparo maestro `TRGO` para disparar las conversiones regulares de ADC1/ADC2; **CCDS = 0**; **MMS2 = 0**. |
| `SMCR` | `0x40000408` | `0x0000` | **Modo esclavo deshabilitado (SMS = 000):** el temporizador opera de forma autónoma con el reloj interno del bus APB1 (`CK_INT` = 200 MHz); **TS = 000**; **MSM = 0**; **ETF/ETPS/ECE/ETP = 0**. |
| `DIER` | `0x4000040C` | `0x0000` | **Interrupciones y peticiones DMA de TIM3 deshabilitadas:** el trigger TRGO hacia el ADC es una conexión hardware directa entre periféricos; no requiere que TIM3 genere interrupciones de CPU (`UIE = 0`) ni peticiones DMA propias (`UDE = 0`). |
| `PSC` | `0x40000428` | `0x0000` | **Prescaler = 0 (divisor 1):** el contador avanza a la frecuencia completa del reloj de temporizadores de APB1 (200 MHz, derivado del SYSCLK PLL1P vía `D2PPRE1 = /2` y multiplicador de timer x2). |
| `ARR` | `0x4000042C` | `0x018F` | **Auto-reload register = 399 (0x018F):** periodo de cuenta = `ARR + 1 = 400` ciclos. A 200 MHz, frecuencia de disparo TRGO = 200.000.000 / 400 = **500.000 Hz (500 kHz nominal)**. |

### Controles implementados en T3 (`tim_f4_checks.py`)
1. **Lectura completa a 32 bits (m2):** Se utiliza `STM32_Programmer_CLI -r32 0x40000400 24` leyendo palabras de 32 bits sin enmascaramiento a 16 bits.
2. **Control de equivalencia en `04fb865`:** La lectura SWD en caliente sobre la imagen original de control `04fb865` debe arrojar exactamente los mismos 6 valores (equivalencia verificada por el auditor en su script independiente en la ronda 1: `CR1=0x1`, `CR2=0x20`, `SMCR=0`, `DIER=0`, `PSC=0`, `ARR=0x18F`).
3. **Control de tabla alterada:** La comparación de los registros leídos contra una tabla falsificada (p. ej., `ARR = 0x00FF`) genera automáticamente un fallo (`FAIL`).

---

## 4. Instrucciones y comandos para la auditoría en banco

El auditor independiente ejecutará las pruebas en hardware conectando la sonda ST-LINK (número de serie obligatorio `sn=0016000C4142501220353451`). Toda sesión en placa respetará estrictamente las reglas de seguridad del banco (G-S1 a G-S7).

Los comandos emplean el argumento nativo `--output <ruta>` de `tim_f4_checks.py`, que captura la salida en UTF-8 con saltos de línea LF mediante `StreamTee`, evitando las redirecciones de PowerShell 5.1 (`>`) que introducen codificación UTF-16 (m2).

### 4.1 Comprobación de seguridad de pines del banco (G-S1 / M2)

Conforme a la especificación y verificación manual documentada en código y plan (M2):
- **Líneas autorizadas utilizadas por los presets de prueba:** `PB1` (`BOARD_ADC_PIN`, canal analógico 5 / ADC1_INP5), `PD5`/`PD6` (USART2 TX/RX, puente dedicado para selftest DMA T3), canales internos de ADC (VREFINT, sensor temperatura), y disparos internos por hardware de TIM3/TIM7 (TRGO a ADC y DMA).
- **Líneas protegidas del banco con intersección estrictamente nula:** `PE2`–`PE15`, `PA8` (bus AD2); `PA5`, `PA6`, `PB4`, `PB5` (conector J1); `PB8`, `PB9` (bus I2C); `PF14`, `PF15` (tomas auxiliares). Ningún preset migrado o de prueba reconfigura estos pines.

### 4.2 Comandos de ejecución para el auditor

#### T3: Inspección en caliente de registros SWD de TIM3
```bash
# prj_04 (flasheo verificado de debug, espera y lectura de los 6 registros en caliente a 32 bits)
py -u tools/tim_f4_checks.py --project prj_04 --test t3 --flash --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t3_1f8876b.txt

# prj_03 (flasheo verificado de debug, espera y lectura de los 6 registros en caliente a 32 bits)
py -u tools/tim_f4_checks.py --project prj_03 --test t3 --flash --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t3_1f8876b.txt
```

#### T4: Ritmo del ADC, overrun y equivalencia secuencial vs control (B2)
```bash
# prj_04 (flasheo migrada, ventana 60s UDP 5001, flasheo control 04fb865, ventana 60s, |Δ| <= 0.1%, oráculo m4_hb, controles negativos)
py -u tools/tim_f4_checks.py --project prj_04 --test t4 --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t4_1f8876b.txt

# prj_03 (flasheo migrada, ventana 60s UDP 5001, flasheo control 04fb865, ventana 60s, |Δ| <= 0.1%, oráculo m4_hb, controles negativos)
py -u tools/tim_f4_checks.py --project prj_03 --test t4 --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t4_1f8876b.txt
```

#### T5: Regresión de sistema y red (B3)
```bash
# prj_04 debug
py -u tools/tim_f4_checks.py --project prj_04 --test t5 --preset debug --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t5_debug_1f8876b.txt

# prj_04 release
py -u tools/tim_f4_checks.py --project prj_04 --test t5 --preset release --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t5_release_1f8876b.txt

# prj_03 debug
py -u tools/tim_f4_checks.py --project prj_03 --test t5 --preset debug --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t5_debug_1f8876b.txt

# prj_03 release
py -u tools/tim_f4_checks.py --project prj_03 --test t5 --preset release --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t5_release_1f8876b.txt
```

#### T6a: 20 arranques repetidos por SWD (`-rst`) (M4, m2)
```bash
# prj_04 (20 ciclos de reset con 10s de espera y validación T3 y T4 tras cada reset; excepciones capturadas)
py -u tools/tim_f4_checks.py --project prj_04 --test t6a --flash --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t6a_1f8876b.txt

# prj_03 (20 ciclos de reset con 10s de espera y validación T3 y T4 tras cada reset; excepciones capturadas)
py -u tools/tim_f4_checks.py --project prj_03 --test t6a --flash --output 02_firmware_projects/prj_03_adc_dma_eth/docs/tim_f4_t6a_1f8876b.txt
```

#### T8: Selftests migrados en la placa y equivalencia con control (N1, B1)
```bash
# Ejecuta G-S1, monitor con hilo desacoplado previo a flasheo (hasta 45s), filtro de banner CM7,
# flashea prj_04 selftest & selftest-adc, prj_03 selftest (migrados y control 04fb865),
# valida líneas exactas PASS, 0 FAIL, y restaura prj_03 debug verificado
py -u tools/tim_f4_checks.py --project prj_04 --test t8 --output 02_firmware_projects/prj_04_adc_dma_bm_eth/docs/tim_f4_t8_1f8876b.txt
```

---

## 5. Estado de pruebas que requieren hardware (Lo no ejecutado)

Conforme a las instrucciones expresas del plan y las restricciones de rol, el implementador no tiene acceso físico a la placa STM32H755. Por tanto, todas las pruebas dependientes de hardware quedan formalmente declaradas como **NO VERIFICADO / pendiente de ejecución por el auditor independiente**:

| Prueba | Descripción | Estado por el implementador | Condición de verificación en auditoría |
| --- | --- | --- | --- |
| **T3** | Inspección en caliente de registros TIM3 por SWD | **NO VERIFICADO** | Ejecución por auditor con `--test t3 --flash` en ambos proyectos (lectura 32 bits, control equivalencia vs `04fb865`). |
| **T4** | Ritmo del ADC (consultas UDP directas 5001, ventana 60s, equivalencia $\|r_{\text{mig}} - r_{\text{ctrl}}\| / r_{\text{ctrl}} \le 0,1\%$, oráculo $50\,000 \pm 0,5\%$ samples/hb) | **NO VERIFICADO** | Ejecución por auditor con `--test t4` en ambos proyectos (claves obligatorias, ovr=0, adc_ovr=0, controles negativos). |
| **T5** | Regresión de red ping 4/4 y UDP 100/100 (debug/release) | **NO VERIFICADO** | Ejecución por auditor con `--test t5` en debug y release (`PASS 3/3 checks` y contadores `[net]` estrictamente en 0 tras 15s de monitor). |
| **T6a** | 20 arranques repetidos por SWD (20/20 PASS) | **NO VERIFICADO** | Ejecución por auditor con `--test t6a` en ambos proyectos (20/20 PASS, excepciones capturadas). |
| **T6b** | Arranque en frío (power cycle físico en banco) | **NO VERIFICADO** | Ejecución física por el responsable del banco durante el cierre formal. |
| **T8** | Selftests en placa (`prj_04` selftest/selftest-adc, `prj_03` selftest) | **NO VERIFICADO** | Ejecución por auditor con `--test t8` (monitor previo con hilo y cola, filtro banner CM7, líneas exactas obligatorias, equivalencia vs `04fb865`, restauración verificada). |

**Estado final de la placa tras las pruebas (G-S1 / G-S6):**
Al concluir la batería de pruebas en placa, la herramienta `tim_f4_checks.py` en el paso final de T8 vuelve a flashear y verificar la imagen de producción **`prj_03_adc_dma_eth` preset `debug`** compilada en el commit `1f8876b`, dejando la placa en estado operativo normal con confirmación de descarga exitosa verificada (N1).

---

## 6. Desviaciones y notas técnicas

1. **Arquitectura de adquisición en `prj_03` (`tim_hal` + `tim_bm`):**
   En `prj_03/CM4/App/Src/adc_acquisition.c`, la inicialización de base de tiempo y sincronización maestra se realiza mediante `TIM_HAL_InitTimeBase` y `TIM_HAL_ConfigMaster` (tier HAL). Para el arranque tardío del contador (tras armar ADC y DMA), se utiliza `TIM_BM_Start(TIM3)` de `tim_bm`. Con esto se satisface el requisito de migración al tier HAL y se garantiza que **cero llamadas a `HAL_TIM_*`** permanezcan en `prj_03` fuera de `01_shared_drivers/03_tim/tim_hal.c`.
2. **Escaneo estático integral en T2b (`tim_f4_checks.py`) (M1):**
   El analizador estático T2b escanea tanto las unidades de compilación de CM4 como las de CM7. Para evitar falsos positivos con otros periféricos que comparten nombres de registro (`CR1`, `CR2`, `SR`), el filtro `TIM37_TARGET_RE` exige que la escritura esté dirigida a la instancia `TIM3` (`0x40000400`), `TIM7` (`0x40001400`) o a través de `Instance`. En CM7 se identifica la habilitación de reloj en `CM7/Core/Src/main.c:203` (`__HAL_RCC_TIM3_CLK_ENABLE()`) como un hallazgo conocido y explicado conforme a la regla D y RM0399 §9.7.1 (relojes independientes por núcleo).
3. **Manejo de capturas y validación de árbol limpio (M3):**
   `get_git_info` en `tim_f4_checks.py` implementa la comprobación estricta de árbol limpio con `git status --porcelain` (sin `-uno`), ignorando exclusivamente los ficheros no seguidos (`??`) de evidencia generados bajo `docs/tim_f4_*.txt`. Cualquier archivo modificado (` M`), borrado (` D`) o en el índice invalida la ejecución de inmediato con código de error, preservando la rigurosidad requerida por el plan.
4. **Habilitación de reloj antes de parada en selftest (k1) y referencia a RM0399 (k2):**
   En `adc_selftest_hw.c`, `Tim3_Prepare` habilita el reloj del periférico antes de invocar `TIM_BM_Stop(TIM3)`, garantizando que el acceso al periférico para poner a cero `CR1.CEN` se ejecute con reloj activo. Asimismo, todas las referencias al capítulo de temporizadores de propósito general TIM2 a TIM5 se han alineado formalmente a RM0399 §41 (con `TIMx_CR2` en §41.4.2).

---

F1 CORREGIDA 2 PARA AUDITORIA 1f8876b
