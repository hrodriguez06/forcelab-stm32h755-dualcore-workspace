# Reporte de Implementación: Fase F1 de `01_led` (L1/L2) — Revisión 2

Oct 16, 2026 · Agente Implementador (Antigravity)
Rama: `driver/led-l1l2` · Base / Control: `9dc6896` · Auditoría previa: `3c2176c` (`736948f`) · Commit de código/herramienta: `411673b`

---

## 1. Propósito y Decisiones Aprobadas

El objetivo de la fase F1 es resolver las limitaciones L1 y L2 del driver compartido `01_shared_drivers/01_led`, junto con la validación de rango de bits en tiempo de compilación (L5 por la vía de `board_def.h`), de acuerdo con [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1), [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md) y [`AUDIT.md`](AUDIT.md):
- **L1 resuelto:** `prj_02`, `prj_03` y `prj_04` definían `BOARD_LED_<x>_PIN` (máscaras `GPIO_PIN_x`), pero el driver bare-metal requería `BOARD_LED_<x>_BIT` (números de bit 0..15). Al no encontrarlos, recurría en silencio a valores hardcodeados por defecto.
- **L2 resuelto:** Se eliminaron los valores hardcodeados por defecto en el driver (`#ifndef ... #define`), reemplazándolos por directivas obligatorias `#error` que hacen cumplir la Regla 1 de drivers compartidos ("never hardcode GPIO pins").
- **L5 (en compilación) resuelto:** Se añadieron comprobaciones `#if (BOARD_LED_<x>_BIT) > 15U` → `#error`.

### Decisiones Aprobadas por el Responsable (2026-10-16):
- **Decisión 1 (Alcance):** Opción A. L1, L2 y rango de `_BIT` en compilación. La comprobación dinámica de `pin_num` en `LED_Init_Config()` y L3/L4 quedan fuera de esta fase.
- **Decisión 2 (Prueba en placa T4):** Opción A. Lectura por SWD de `g_led_config` y registros `MODER` en modo HOTPLUG objetivo, sin AD2.
- **Decisión 3 (Documentos de proceso):** Se conservan durante la auditoría y se borran en el cierre.

---

## 2. Correcciones de Auditoría Aplicadas (B1, M1, M2, m1–m8)

En respuesta a los hallazgos documentados en [`AUDIT.md`](AUDIT.md), se aplicaron y commitearon en [`411673b`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/led_l1l2_checks.py) las siguientes correcciones:

1. **B1 y m1, m2 (Bloqueante & Menores):**
   - Se eliminó la invocación opaca de `build_all.ps1 -Clean` para el recuento de avisos de la matriz en T1.
   - Las 8 configuraciones de producción se compilan directamente mediante `cmake --preset <p> --fresh` y `cmake --build --preset <p> --clean-first`.
   - Se valida el código de retorno y la salida de configure (m2), se capturan todas las líneas de aviso reales del compilador, se comprueba que se hayan generado objetos (`objs > 0`) y se valida que `led_bm.c.obj` se haya compilado y esté presente en disco (m2).
   - El control negativo de avisos pasa por la misma función pura de evaluación [`evaluate_build_output`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/led_l1l2_checks.py#L262-L280) utilizada en las compilaciones reales, verificando que un aviso sintético genera `FAIL`.
   - Se contabilizan e informan individualmente las 8 compilaciones exitosas (m1).

2. **M1 y m3 (Mayor & Menor):**
   - En T2, la función [`compile_variant`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/led_l1l2_checks.py#L463-L494) ejecuta GCC con `cwd=board_def_dir` en el directorio temporal fuera del repositorio (`tempfile.TemporaryDirectory`). De este modo, `-fstack-usage` deposita los archivos auxiliares `a-led_bm.su` fuera del árbol de trabajo, garantizando que el árbol permanece limpio sin archivos ignorados residuales.
   - En mutaciones negativas, se valida que el texto esperado aparezca específicamente en la línea diagnóstica del `#error`.
   - En los controles positivos, se verifica que la compilación produzca 0 avisos.

3. **M2 (Mayor):**
   - En T3 y T4, la herramienta verifica que las imágenes provengan de la ejecución actual y de `HEAD`:
     - Función [`check_image_freshness_and_rev`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/led_l1l2_checks.py#L283-L317): valida que la marca de tiempo del binario CM7 sea reciente (posterior al inicio de la corrida) y que `FW_GIT_REV` coincida con el SHA corto de `HEAD` (para `prj_01` en `fw_build_id.h` y ELF; para `prj_04` en el binario ELF).
     - Si se invoca `--test t3` o `--test t4` de forma aislada, la herramienta compila limpiamente las imágenes necesarias antes de proceder.

4. **m4 (Menor):**
   - En T3, la función [`parse_and_verify_led_config`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/led_l1l2_checks.py#L607-L635) valida que la sección extraída mida exactamente 24 bytes (no se acepta entrada vacía).
   - En el control negativo de T3, se comprueba primero que la muestra original pasa, se verifica que la mutación altera el texto, y se comprueba que la entrada mutada es rechazada con una longitud de exactamente 24 bytes.

5. **m5 (Menor):**
   - En T4, se añadieron dos controles negativos independientes:
     - Control 1: Tabla RAM mutada (número de pin alterado) rechazada.
     - Control 2: Registro `MODER` mutado (PB0 configurado como entrada `00b` en lugar de salida `01b`) rechazado.

6. **m6 (Menor):**
   - La función [`check_clean_working_tree`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/led_l1l2_checks.py#L177-L200) ignora exclusivamente la ruta exacta del archivo de salida proporcionado por el usuario (`?? <ruta_exacta>`).
   - Se ejecuta y verifica el árbol limpio tanto al inicio como al finalizar la ejecución del script.

7. **m7 (Menor):**
   - Todos los subprocesos en [`run_cmd`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/led_l1l2_checks.py#L79-L86) imponen límites de tiempo explícitos (`timeout`).
   - En T4, se registra en el log el valor hexadecimal exacto de las palabras leídas de `g_led_config` y de los registros `MODER_B` y `MODER_E`.

8. **m8 (Menor):**
   - Se actualizaron los comentarios de advertencia arquitectónica en los tres `board_def.h` para reflejar que el uso de una máscara o la ausencia de la macro genera un `#error` en tiempo de compilación.
   - El archivo `prj_01` se mantuvo intacto sin modificaciones.

---

## 3. Verificación de Código contra HEAD (`archivo:línea`)

Todos los archivos modificados fueron verificados rigurosamente contra `HEAD` en commit [`411673b`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/led_l1l2_checks.py):

### 3.1 Driver compartido: `01_shared_drivers/01_led/led_bm.c`
Archivo: [`01_shared_drivers/01_led/led_bm.c`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/01_led/led_bm.c)
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
- [`02_firmware_projects/prj_02_eth_lwip_dualcore/Common/Inc/board_def.h#L69-L79`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/02_firmware_projects/prj_02_eth_lwip_dualcore/Common/Inc/board_def.h#L69-L79): Bloque de LEDs con números de bit 0U, 1U, 14U y comentario actualizado indicando `#error` en compilación.
- [`02_firmware_projects/prj_03_adc_dma_eth/Common/Inc/board_def.h#L69-L79`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/02_firmware_projects/prj_03_adc_dma_eth/Common/Inc/board_def.h#L69-L79): Bloque de LEDs idéntico.
- [`02_firmware_projects/prj_04_adc_dma_bm_eth/Common/Inc/board_def.h#L69-L79`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/02_firmware_projects/prj_04_adc_dma_bm_eth/Common/Inc/board_def.h#L69-L79): Bloque de LEDs idéntico.
- [`03_firmware_templates/prj_01_template_dualcore/Common/Inc/board_def.h#L49-L60`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/Common/Inc/board_def.h#L49-L60): Conservado intacto sin modificaciones.

---

## 4. Resultados de las Pruebas sin Placa (T1, T2, T3)

La captura completa y secuencial de la corrida con el arnés corregido está archivada y commiteada en:
[`01_shared_drivers/01_led/docs/led_l1l2_checks_411673b.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/01_led/docs/led_l1l2_checks_411673b.txt) (commit `afe8ee8`).

### 4.1 T1: Compilación limpia de matriz y presets selftest
- **8 Configuraciones de matriz de producción (compiladas directamente con `cmake --clean-first`):**
  - `prj_01_template_dualcore` (debug: 271 objetos, 0 avisos; release: 271 objetos, 0 avisos)
  - `prj_02_eth_lwip_dualcore` (debug: 315 objetos, 0 avisos; release: 315 objetos, 0 avisos)
  - `prj_03_adc_dma_eth` (debug: 323 objetos, 0 avisos; release: 323 objetos, 0 avisos)
  - `prj_04_adc_dma_bm_eth` (debug: 321 objetos, 0 avisos; release: 321 objetos, 0 avisos)
  - Verificación de imagen: Todas las imágenes confirmadas como frescas de la corrida y con `FW_GIT_REV` verificado (`411673b`).
  - Resultado: 8/8 `PASS` (0 avisos, `led_bm.c.obj` verificado en log y disco).
- **Presets de selftest que enlazan `led_bm` (con `--fresh` y `--clean-first`):**
  - `prj_01`: `selftest-gpio` (278 objs, 0 w), `selftest-tim` (281 objs, 0 w), `selftest-spi` (283 objs, 0 w), `selftest-i2c` (283 objs, 0 w).
  - `prj_04`: `selftest` (327 objs, 0 w), `selftest-adc` (326 objs, 0 w), `selftest-uart` (324 objs, 0 w), `selftest-hsem` (326 objs, 0 w), `selftest-gpio` (323 objs, 0 w).
  - Resultado: 9/9 `PASS` (0 avisos, `led_bm.c.obj` verificado).
- **Control negativo de contador de avisos:**
  - Evaluado a través de [`evaluate_build_output`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/led_l1l2_checks.py#L262-L280), detectando el aviso sintético inyectado y marcando fallo con éxito (`PASS`).
- **Veredicto T1:** `PASS`.

### 4.2 T2: Comprobación de preprocesador y `#error` en directorio temporal
Compilación con `arm-none-eabi-gcc -fsyntax-only` ejecutada con `cwd` en directorio temporal fuera del repositorio:
- **Controles positivos:** Los 4 proyectos compilan con código de salida 0 y 0 avisos.
- **16 Mutaciones negativas (4 por proyecto):**
  1. Sin `BOARD_LED_RED_BIT`: Falla con `#error "BOARD_LED_RED_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"` en línea 39.
  2. Sin `BOARD_LED_GREEN_PORT`: Falla con `#error "BOARD_LED_GREEN_PORT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"` en línea 16.
  3. `_PIN` en vez de `_BIT`: Falla con `#error "BOARD_LED_GREEN_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"` en línea 19.
  4. `BOARD_LED_YELLOW_BIT 16U`: Falla con `#error "BOARD_LED_YELLOW_BIT must be in range 0..15"` en línea 32.
- **Resultado:** 20/20 casos comprobados con éxito. `git status --ignored` constató que no se crearon archivos `.su` residuales.
- **Veredicto T2:** `PASS`.

### 4.3 T3: Comparación binaria de `g_led_config`
Extracción mediante `arm-none-eabi-objdump -s` de los 24 bytes de `g_led_config` del objeto CM7 `led_bm.c.obj`:
- **8 Builds actuales:** Coincidencia exacta con el oráculo UM2408 (`words=['0x58020400', '0x0', '0x58021000', '0x1', '0x58020400', '0xe']`).
- **Control `9dc6896` compilado in-situ:** Los 8 builds del árbol de control compilaron limpiamente en 249.9s y coinciden al 100% con el oráculo UM2408 y con los binarios actuales.
- **Control negativo de T3:** Verificación de muestra original exitosa, confirmación de alteración de texto, y rechazo de la entrada mutada validando tamaño exacto de 24 bytes (`PASS`).
- **Veredicto T3:** `PASS`.

---

## 5. Validación en Hardware (T4) Medida por el Auditor

De acuerdo con el rol asignado, el agente implementador no tiene acceso físico a la placa y no ejecutó comandos de hardware. La verificación en hardware real Nucleo-144 STM32H755ZI-Q fue ejecutada y certificada directamente por el auditor (@Claude para Hector Rodriguez):

### Capturas oficiales del auditor en el repositorio:
- Captura T1–T3 del auditor: [`01_shared_drivers/01_led/docs/led_l1l2_audit_t1t3_736948f.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/01_led/docs/led_l1l2_audit_t1t3_736948f.txt) (`OVERALL VERDICT: PASS`).
- Captura T4 en la placa por el auditor: [`01_shared_drivers/01_led/docs/led_l1l2_audit_t4_736948f.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/01_led/docs/led_l1l2_audit_t4_736948f.txt) (`OVERALL VERDICT: PASS`).
  - Flasheo de imágenes `debug` con verificación de descarga (`-v`) en los 4 proyectos (`PASS`).
  - Lectura SWD en `mode=HOTPLUG` con `sn=0016000C4142501220353451`:
    - `g_led_config` en RAM verificado contra la tabla UM2408 en los 4 proyectos (`words_match=True`).
    - Registros `MODER` de GPIOB y GPIOE verificados: PB0=`01b`, PB14=`01b`, PE1=`01b` (modo salida de propósito general).
  - Control negativo rechazado con éxito (`PASS`).

### Comando de T4 para el auditor con el arnés corregido:
```bash
py tools/led_l1l2_checks.py --test t4 --output 01_shared_drivers/01_led/docs/led_l1l2_t4_<hash>.txt
```
*(Utiliza por defecto `sn=0016000C4142501220353451`; si se usa otro programador, especificar con `--sn <numero_serie>`)*.

El arnés T4 actualizado:
1. Asegura la compilación limpia de las imágenes `debug` desde `HEAD` en la propia ejecución y verifica su `FW_GIT_REV`.
2. Flashea con `tools/flash.py` de cada proyecto mediante `STM32_Programmer_CLI -v`.
3. Efectúa lecturas no intrusivas `-r32` en `mode=HOTPLUG` con `sn=` sin detener el microcontrolador ni escribir registros.
4. Registra los valores hexadecimales exactos leídos en el log (m7).
5. Evalúa controles negativos independientes tanto para la tabla en RAM como para el registro `MODER` (m5).

---

## 6. Elementos no Verificados por el Implementador

- **T4 no fue ejecutada por el implementador:** En estricto cumplimiento de las instrucciones de rol, no se ejecutó ningún comando contra la placa (`flash.py`, `STM32_Programmer_CLI`, `monitor.py`).
- Las mediciones de T4 que sustentan la certificación física del driver corresponden a las registradas por el auditor en [`01_shared_drivers/01_led/docs/led_l1l2_audit_t4_736948f.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/01_led/docs/led_l1l2_audit_t4_736948f.txt).

---

F1 CORREGIDA PARA AUDITORIA 411673b
