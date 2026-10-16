# Reporte de Implementación: Fase F1 de `01_led` (L1/L2) — Cierre

Oct 16, 2026 · Agente Implementador (Antigravity)
Rama: `driver/led-l1l2` · Base / Control: `9dc6896` · Aprobación de auditoría: `c5ce471` · Commit de cierre: `ecba772`

---

## 1. Propósito y Decisiones Aprobadas

El objetivo de la fase F1 es resolver las limitaciones L1 y L2 del driver compartido `01_shared_drivers/01_led`, junto con la validación de rango de bits en tiempo de compilación (L5 por la vía de `board_def.h`), de acuerdo con [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1), [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md) y [`AUDIT.md`](AUDIT.md):
- **L1 resuelto:** `prj_02`, `prj_03` y `prj_04` definían `BOARD_LED_<x>_PIN` (máscaras `GPIO_PIN_x`), pero el driver bare-metal requería `BOARD_LED_<x>_BIT` (números de bit 0..15). Al no encontrarlos, recurría en silencio a valores hardcodeados por defecto.
- **L2 resuelto:** Se eliminaron los valores hardcodeados por defecto en el driver (`#ifndef ... #define`), reemplazándolos por directivas obligatorias `#error` que hacen cumplir la Regla 1 de drivers compartidos ("never hardcode GPIO pins").
- **L5 (en compilación) resuelto:** Se añadieron comprobaciones `#if (BOARD_LED_<x>_BIT) > 15U` → `#error`.

### Decisiones Aprobadas por el Responsable (2026-10-16):
- **Decisión 1 (Alcance):** Opción A. L1, L2 y rango de `_BIT` en compilación. La comprobación dinámica de `pin_num` en `LED_Init_Config()` y L3/L4 quedan fuera de esta fase.
- **Decisión 2 (Prueba en placa T4):** Opción A. Lectura por SWD de `g_led_config` y registros `MODER` en modo HOTPLUG objetivo, sin AD2.
- **Decisión 3 (Documentos de proceso):** Se conservan durante la auditoría y se archivan en el historial de Git tras el cierre.

---

## 2. Correcciones de Auditoría y Acciones de Cierre

En respuesta a las revisiones de auditoría documentadas en [`AUDIT.md`](AUDIT.md) (commits `3c2176c`, `1f63515` y `c5ce471`), se implementaron las siguientes acciones definitivas:

1. **Condición de Cierre MA1 (Herramienta de Verificación):**
   - Se eliminó de [`tools/led_l1l2_checks.py`](../../tools/led_l1l2_checks.py) la inyección artificial de `-DCMAKE_C_FLAGS:STRING=-DFW_GIT_REV` en configure para `prj_02` y `prj_03`, así como su posterior lectura en `CMakeCache.txt`.
   - En [`check_image_freshness_and_rev`](../../tools/led_l1l2_checks.py#L355-L358), la herramienta reporta explícitamente para `prj_02` y `prj_03` que la revisión no está embebida en el ELF y documenta su procedencia: árbol de trabajo limpio al inicio y al final de la ejecución, y compilación fresca garantizada dentro de la misma corrida.

2. **Alineación de Comentario en `prj_01`:**
   - En [`03_firmware_templates/prj_01_template_dualcore/Common/Inc/board_def.h#L50-L53`](../../03_firmware_templates/prj_01_template_dualcore/Common/Inc/board_def.h#L50-L53), se actualizó el comentario de arquitectura de LEDs para indicar que una máscara de pin o macro faltante genera un `#error` en tiempo de compilación, alineándolo con los comentarios de `prj_02`, `prj_03` y `prj_04`.

3. **Control Negativo de Avisos Real (MA2):**
   - El control negativo de avisos compila un archivo C temporal (`warning_sample.c`) fuera del repositorio con flags reales del proyecto vía `run_cmd`, contando los objetos generados con [`count_built_objects`](../../tools/led_l1l2_checks.py#L288-L290) y enrutando la salida completa por [`evaluate_build_output`](../../tools/led_l1l2_checks.py#L293-L315) para verificar que el aviso emitido por el compilador es detectado y rechazado ([`run_t1`](../../tools/led_l1l2_checks.py#L442-L474)).

4. **Validación Estricta de Longitud de Sección (m4):**
   - En [`parse_and_verify_led_config`](../../tools/led_l1l2_checks.py#L672-L704), se extraen todos los bytes de la sección sin truncamiento y se valida que la longitud sea exactamente 24 bytes (`len(extracted) == len(expected_bytes) == 24`).

5. **Aislamiento de T2 (M1):**
   - [`compile_variant`](../../tools/led_l1l2_checks.py#L526-L558) opera con `cwd` en un directorio temporal fuera del repositorio para evitar la generación de archivos auxiliares `.su` en el árbol de trabajo.

6. **Limpieza de Capturas Superadas:**
   - Se removieron de `docs/` las capturas intermedias superadas (`led_l1l2_checks_2e6a15d.txt`, `led_l1l2_checks_411673b.txt`, `led_l1l2_checks_b9fbaad.txt`), dejando exclusivamente las 3 capturas oficiales del auditor y la captura final del implementador ([`docs/led_l1l2_checks_824d989.txt`](docs/led_l1l2_checks_824d989.txt)).

---

## 3. Verificación de Código contra HEAD (`archivo:línea`)

Todos los archivos modificados fueron verificados rigurosamente contra `HEAD`:

### 3.1 Driver compartido: `01_shared_drivers/01_led/led_bm.c`
Archivo: [`01_shared_drivers/01_led/led_bm.c`](led_bm.c)
- **Línea 13:** Inclusión incondicional de `board_def.h` (`#include "board_def.h"`).
- **Líneas 15–23:** Validaciones de preprocesador para `LED_GREEN` (LD1):
  - Líneas 15–17: `#ifndef BOARD_LED_GREEN_PORT` → `#error "BOARD_LED_GREEN_PORT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"`
  - Líneas 18–20: `#ifndef BOARD_LED_GREEN_BIT` → `#error "BOARD_LED_GREEN_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"`
  - Líneas 21–23: `#if (BOARD_LED_GREEN_BIT) > 15U` → `#error "BOARD_LED_GREEN_BIT must be in range 0..15"`
- **Líneas 25–33:** Validaciones de preprocesador para `LED_YELLOW` (LD2):
  - Líneas 25–27: `#ifndef BOARD_LED_YELLOW_PORT` → `#error "BOARD_LED_YELLOW_PORT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"`
  - Líneas 28–30: `#ifndef BOARD_LED_YELLOW_BIT` → `#error "BOARD_LED_YELLOW_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"`
  - Líneas 31–33: `#if (BOARD_LED_YELLOW_BIT) > 15U` → `#error "BOARD_LED_YELLOW_BIT must be in range 0..15"`
- **Líneas 35–43:** Validaciones de preprocesador para `LED_RED` (LD3):
  - Líneas 35–37: `#ifndef BOARD_LED_RED_PORT` → `#error "BOARD_LED_RED_PORT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"`
  - Líneas 38–40: `#ifndef BOARD_LED_RED_BIT` → `#error "BOARD_LED_RED_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"`
  - Líneas 41–43: `#if (BOARD_LED_RED_BIT) > 15U` → `#error "BOARD_LED_RED_BIT must be in range 0..15"`
- **Líneas 45–51:** Definición estática de `g_led_config` con las macros de `board_def.h`.

### 3.2 Definiciones de placa: `Common/Inc/board_def.h`
- [`03_firmware_templates/prj_01_template_dualcore/Common/Inc/board_def.h#L50-L60`](../../03_firmware_templates/prj_01_template_dualcore/Common/Inc/board_def.h#L50-L60): Comentario actualizado indicando `#error` y macros `_BIT` (0U, 1U, 14U).
- [`02_firmware_projects/prj_02_eth_lwip_dualcore/Common/Inc/board_def.h#L70-L80`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/Common/Inc/board_def.h#L70-L80): Bloque de LEDs con números de bit 0U, 1U, 14U y comentario alineado.
- [`02_firmware_projects/prj_03_adc_dma_eth/Common/Inc/board_def.h#L70-L80`](../../02_firmware_projects/prj_03_adc_dma_eth/Common/Inc/board_def.h#L70-L80): Bloque de LEDs idéntico.
- [`02_firmware_projects/prj_04_adc_dma_bm_eth/Common/Inc/board_def.h#L70-L80`](../../02_firmware_projects/prj_04_adc_dma_bm_eth/Common/Inc/board_def.h#L70-L80): Bloque de LEDs idéntico.

---

## 4. Resultados de las Pruebas sin Placa (T1, T2, T3)

La captura completa y secuencial de la corrida final con el arnés corregido está archivada y commiteada en:
[`01_shared_drivers/01_led/docs/led_l1l2_checks_824d989.txt`](docs/led_l1l2_checks_824d989.txt) (commit `02c7c6c`).

### 4.1 T1: Compilación limpia de matriz y presets selftest
- **8 Configuraciones de matriz de producción (compiladas directamente con `cmake --clean-first`):**
  - `prj_01_template_dualcore` (debug: 271 objetos, 0 avisos; release: 271 objetos, 0 avisos; rev=`824d989` en `fw_build_id.h`)
  - `prj_02_eth_lwip_dualcore` (debug: 315 objetos, 0 avisos; release: 315 objetos, 0 avisos; rev no embebida, procedencia: árbol limpio y compilación en corrida)
  - `prj_03_adc_dma_eth` (debug: 323 objetos, 0 avisos; release: 323 objetos, 0 avisos; rev no embebida, procedencia: árbol limpio y compilación en corrida)
  - `prj_04_adc_dma_bm_eth` (debug: 321 objetos, 0 avisos; release: 321 objetos, 0 avisos; rev=`824d989` en ELF)
  - Resultado: 8/8 `PASS` (0 avisos, `led_bm.c.obj` verificado en log y disco, imágenes frescas).
- **Presets de selftest que enlazan `led_bm` (con `--fresh` y `--clean-first`):**
  - `prj_01`: `selftest-gpio` (278 objs, 0 w), `selftest-tim` (281 objs, 0 w), `selftest-spi` (283 objs, 0 w), `selftest-i2c` (283 objs, 0 w).
  - `prj_04`: `selftest` (327 objs, 0 w), `selftest-adc` (326 objs, 0 w), `selftest-uart` (324 objs, 0 w), `selftest-hsem` (326 objs, 0 w), `selftest-gpio` (323 objs, 0 w).
  - Resultado: 9/9 `PASS` (0 avisos, `led_bm.c.obj` verificado).
- **Control negativo de contador de avisos (MA2):**
  - Compilación real de `warning_sample.c` en directorio temporal vía `run_cmd` con flags del proyecto; evaluado por [`evaluate_build_output`](../../tools/led_l1l2_checks.py#L293-L315), detectando el aviso del compilador y marcando fallo esperado con éxito (`PASS`).
- **Veredicto T1:** `PASS`.

### 4.2 T2: Comprobación de preprocesador y `#error` en directorio temporal
Compilación con `arm-none-eabi-gcc -fsyntax-only` ejecutada con `cwd` en directorio temporal fuera del repositorio:
- **Controles positivos:** Los 4 proyectos compilan con código de salida 0 y 0 avisos.
- **16 Mutaciones negativas (4 por proyecto):**
  1. Sin `BOARD_LED_RED_BIT`: Falla con `#error "BOARD_LED_RED_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"` en línea 39.
  2. Sin `BOARD_LED_GREEN_PORT`: Falla con `#error "BOARD_LED_GREEN_PORT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"` en línea 16.
  3. `_PIN` en vez de `_BIT`: Falla con `#error "BOARD_LED_GREEN_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"` en línea 19.
  4. `BOARD_LED_YELLOW_BIT 16U`: Falla con `#error "BOARD_LED_YELLOW_BIT must be in range 0..15"` en línea 32.
- **Resultado:** 20/20 casos comprobados con éxito. Comprobación de `git status --ignored` constata que no se crearon archivos `.su` residuales en el árbol.
- **Veredicto T2:** `PASS`.

### 4.3 T3: Comparación binaria de `g_led_config`
Extracción mediante `arm-none-eabi-objdump -s` de los 24 bytes de `g_led_config` del objeto CM7 `led_bm.c.obj`:
- **8 Builds actuales:** Coincidencia exacta con el oráculo UM2408 (`words=['0x58020400', '0x0', '0x58021000', '0x1', '0x58020400', '0xe']`).
- **Control `9dc6896` compilado in-situ:** Los 8 builds del árbol de control compilaron limpiamente en 259.9s (pass_count=24) y coinciden al 100% con el oráculo UM2408 y con los binarios actuales.
- **Control negativo de T3:** Verificación de muestra original exitosa, confirmación de alteración de texto, y rechazo de la entrada mutada validando tamaño exacto de 24 bytes sin truncamiento (`PASS`).
- **Veredicto T3:** `PASS`.

---

## 5. Validación en Hardware (T4) Medida por el Auditor

La verificación en hardware real Nucleo-144 STM32H755ZI-Q fue ejecutada y certificada directamente por el auditor (@Claude para Hector Rodriguez):

### Capturas oficiales del auditor en el repositorio:
- Captura T1–T3 del auditor: [`01_shared_drivers/01_led/docs/led_l1l2_audit_t1t3_736948f.txt`](docs/led_l1l2_audit_t1t3_736948f.txt) (`OVERALL VERDICT: PASS`).
- Captura T4 en la placa por el auditor: [`01_shared_drivers/01_led/docs/led_l1l2_audit_t4_736948f.txt`](docs/led_l1l2_audit_t4_736948f.txt) (`OVERALL VERDICT: PASS`).
  - Flasheo de imágenes `debug` con verificación de descarga (`-v`) en los 4 proyectos (`PASS`).
  - Lectura SWD en `mode=HOTPLUG` con `sn=0016000C4142501220353451`:
    - `g_led_config` en RAM verificado contra la tabla UM2408 en los 4 proyectos (`words_match=True`).
    - Registros `MODER` de GPIOB y GPIOE verificados: PB0=`01b`, PB14=`01b`, PE1=`01b` (modo salida de propósito general).
  - Control negativo rechazado con éxito (`PASS`).

---

## 6. Elementos no Verificados por el Implementador

- **T4 no fue ejecutada por el implementador:** En estricto cumplimiento de las instrucciones de rol, no se ejecutó ningún comando contra la placa (`flash.py`, `STM32_Programmer_CLI`, `monitor.py`).
- Las mediciones de T4 que sustentan la certificación física del driver corresponden a las registradas por el auditor en [`01_shared_drivers/01_led/docs/led_l1l2_audit_t4_736948f.txt`](docs/led_l1l2_audit_t4_736948f.txt).

---

CIERRE ENTREGADO PARA AUDITORIA ecba772
