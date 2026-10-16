# Reporte de Implementación: Fase F1 de `01_led` (L1/L2) — Revisión 3

Oct 16, 2026 · Agente Implementador (Antigravity)
Rama: `driver/led-l1l2` · Base / Control: `9dc6896` · Auditoría previa: `1f63515` (`af836ea`) · Commit de código/herramienta: `b9fbaad`

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

## 2. Correcciones de Auditoría Aplicadas (Rondas 1 y 2)

En respuesta a las auditorías documentadas en [`AUDIT.md`](AUDIT.md), se aplicaron y commitearon en [`b9fbaad`](../../tools/led_l1l2_checks.py) las correcciones requeridas:

1. **MA1 & M2 (Revisión y Frescura en T1, T3 y T4):**
   - Para `prj_01`, la herramienta verifica la revisión en [`build/<preset>/generated/fw_build_id.h`](../../03_firmware_templates/prj_01_template_dualcore) y la frescura del binario ELF ([`check_image_freshness_and_rev`](../../tools/led_l1l2_checks.py#L317-L369)).
   - Para `prj_04`, la revisión se comprueba dentro de la tabla de cadenas del binario ELF CM7 y se verifica la frescura del archivo.
   - Para `prj_02` y `prj_03` (cuyos ELF no embeben `FW_GIT_REV`), la herramienta inyecta la macro en el configure via `-DCMAKE_C_FLAGS:STRING=-DFW_GIT_REV=\"<head_rev>\"`, verifica que la bandera quede registrada de forma determinista en `CMakeCache.txt`, y valida la frescura del ELF CM7 frente a la hora de inicio de la ejecución ([`check_image_freshness_and_rev`](../../tools/led_l1l2_checks.py#L355-L367)).

2. **MA2 & B1 (Captura de Avisos y Control Negativo Real):**
   - Las 8 configuraciones de producción y los 9 presets de selftest se compilan directamente mediante `cmake --preset <p> --fresh` y `cmake --build --preset <p> --clean-first` ([`compile_single_config`](../../tools/led_l1l2_checks.py#L371-L406)).
   - El control negativo de avisos compila un archivo C temporal (`warning_sample.c`) fuera del repositorio con flags reales del proyecto vía `run_cmd`, contando los objetos generados con [`count_built_objects`](../../tools/led_l1l2_checks.py#L288-L290) y enrutando la salida completa por [`evaluate_build_output`](../../tools/led_l1l2_checks.py#L293-L315) para verificar que el compilador emite un aviso real que es detectado y rechazado ([`run_t1`](../../tools/led_l1l2_checks.py#L452-L485)).

3. **M1 & m3 (Aislamiento de T2 y Archivos Auxiliares):**
   - En T2, [`compile_variant`](../../tools/led_l1l2_checks.py#L537-L569) ejecuta GCC con `cwd=board_def_dir` en un directorio temporal fuera del repositorio (`tempfile.TemporaryDirectory`). Los archivos `.su` generados por `-fstack-usage` se depositan fuera del árbol de trabajo.
   - En las mutaciones negativas se busca el texto esperado específicamente en la línea diagnóstica del `#error`.
   - En los controles positivos se comprueba código de retorno 0 y 0 avisos.

4. **m1 & m2 (Comprobación Rigurosa de Compilaciones):**
   - En [`compile_control_images`](../../tools/led_l1l2_checks.py#L252-L273), se verifica que el código de retorno sea 0, que el recuento de líneas `-> PASS` o `PASS` sea al menos 8 (`pass_count >= 8`), y que figure el mensaje de éxito global.
   - En [`compile_single_config`](../../tools/led_l1l2_checks.py#L371-L406), se comprueba el código de retorno y la ausencia de errores en configure, se contabilizan los objetos generados (`objs > 0`), y se exige que `led_bm.c.obj` esté presente tanto en la salida de compilación como en el disco (`led_obj_in_log and led_obj_on_disk`).
   - Se registra el código real de compilación `code={bld_code}` en lugar de un valor fijo ([`run_t1`](../../tools/led_l1l2_checks.py#L425-L427)).

5. **m4 (Validación de Tamaño Exacto de 24 Bytes sin Truncamiento):**
   - En [`parse_and_verify_led_config`](../../tools/led_l1l2_checks.py#L682-L714), se extraen todos los bytes de la sección sin truncar (`extracted = bytes(hex_bytes)`), exigiendo que `len(extracted) == len(expected_bytes) == 24`. Si la sección tiene un tamaño distinto a 24 bytes, falla explícitamente.

6. **m5 (Controles Negativos de Hardware en T4):**
   - En [`run_t4`](../../tools/led_l1l2_checks.py#L921-L1004), se evalúan controles negativos independientes: mutación de la tabla `g_led_config` en RAM y mutación del registro `MODER` (PB0 como entrada `00b`).

7. **m6 & m7 (Higiene de Árbol y Robustez):**
   - [`check_clean_working_tree`](../../tools/led_l1l2_checks.py#L187-L211) valida `git status --porcelain -u` al inicio y al final, ignorando exclusivamente la ruta exacta del archivo de salida.
   - En [`ensure_control_worktree`](../../tools/led_l1l2_checks.py#L213-L250), se valida explícitamente `git status` del worktree de control.
   - En [`check_image_freshness_and_rev`](../../tools/led_l1l2_checks.py#L344-L345), la eliminación del sufijo `-dirty` utiliza `endswith("-dirty")` y rebanado exacto `[:-len("-dirty")]`.
   - Todos los subprocesos en [`run_cmd`](../../tools/led_l1l2_checks.py#L100-L107) operan con `timeout` explícito.

---

## 3. Verificación de Código contra HEAD (`archivo:línea`)

Todos los archivos modificados fueron verificados contra `HEAD` (`b9fbaad`):

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
- [`02_firmware_projects/prj_02_eth_lwip_dualcore/Common/Inc/board_def.h#L69-L80`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/Common/Inc/board_def.h#L69-L80): Bloque de LEDs con números de bit 0U, 1U, 14U y comentario indicando `#error` en compilación.
- [`02_firmware_projects/prj_03_adc_dma_eth/Common/Inc/board_def.h#L69-L80`](../../02_firmware_projects/prj_03_adc_dma_eth/Common/Inc/board_def.h#L69-L80): Bloque de LEDs idéntico.
- [`02_firmware_projects/prj_04_adc_dma_bm_eth/Common/Inc/board_def.h#L69-L80`](../../02_firmware_projects/prj_04_adc_dma_bm_eth/Common/Inc/board_def.h#L69-L80): Bloque de LEDs idéntico.
- [`03_firmware_templates/prj_01_template_dualcore/Common/Inc/board_def.h#L49-L60`](../../03_firmware_templates/prj_01_template_dualcore/Common/Inc/board_def.h#L49-L60): Conservado intacto sin modificaciones.

---

## 4. Resultados de las Pruebas sin Placa (T1, T2, T3)

La captura completa y secuencial de la corrida con el arnés corregido sobre árbol limpio está archivada y commiteada en:
[`01_shared_drivers/01_led/docs/led_l1l2_checks_b9fbaad.txt`](docs/led_l1l2_checks_b9fbaad.txt) (commit `2d221e8`).

### 4.1 T1: Compilación limpia de matriz y presets selftest
- **8 Configuraciones de matriz de producción (compiladas directamente con `cmake --clean-first`):**
  - `prj_01_template_dualcore` (debug: 271 objetos, 0 avisos; release: 271 objetos, 0 avisos; rev=`b9fbaad` en `fw_build_id.h`)
  - `prj_02_eth_lwip_dualcore` (debug: 315 objetos, 0 avisos; release: 315 objetos, 0 avisos; rev=`b9fbaad` en `CMakeCache.txt`)
  - `prj_03_adc_dma_eth` (debug: 323 objetos, 0 avisos; release: 323 objetos, 0 avisos; rev=`b9fbaad` en `CMakeCache.txt`)
  - `prj_04_adc_dma_bm_eth` (debug: 321 objetos, 0 avisos; release: 321 objetos, 0 avisos; rev=`b9fbaad` en ELF)
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
- **Resultado:** 20/20 casos comprobados con éxito. La comprobación manual con `git status --ignored` constata que no se crearon archivos `.su` residuales en el árbol.
- **Veredicto T2:** `PASS`.

### 4.3 T3: Comparación binaria de `g_led_config`
Extracción mediante `arm-none-eabi-objdump -s` de los 24 bytes de `g_led_config` del objeto CM7 `led_bm.c.obj`:
- **8 Builds actuales:** Coincidencia exacta con el oráculo UM2408 (`words=['0x58020400', '0x0', '0x58021000', '0x1', '0x58020400', '0xe']`).
- **Control `9dc6896` compilado in-situ:** Los 8 builds del árbol de control compilaron limpiamente en 270.9s (pass_count=24) y coinciden al 100% con el oráculo UM2408 y con los binarios actuales.
- **Control negativo de T3:** Verificación de muestra original exitosa, confirmación de alteración de texto, y rechazo de la entrada mutada validando tamaño exacto de 24 bytes sin truncamiento (`PASS`).
- **Veredicto T3:** `PASS`.

---

## 5. Validación en Hardware (T4) Medida por el Auditor

De acuerdo con el rol asignado, el agente implementador no tiene acceso físico a la placa y no ejecutó comandos de hardware. La verificación en hardware real Nucleo-144 STM32H755ZI-Q fue ejecutada y certificada directamente por el auditor (@Claude para Hector Rodriguez):

### Capturas oficiales del auditor en el repositorio:
- Captura T1–T3 del auditor: [`01_shared_drivers/01_led/docs/led_l1l2_audit_t1t3_736948f.txt`](docs/led_l1l2_audit_t1t3_736948f.txt) (`OVERALL VERDICT: PASS`).
- Captura T4 en la placa por el auditor: [`01_shared_drivers/01_led/docs/led_l1l2_audit_t4_736948f.txt`](docs/led_l1l2_audit_t4_736948f.txt) (`OVERALL VERDICT: PASS`).
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
1. Asegura la compilación limpia de las imágenes `debug` desde `HEAD` en la propia ejecución y verifica su `FW_GIT_REV` y frescura ([`ensure_debug_images_compiled`](../../tools/led_l1l2_checks.py#L900-L919)).
2. Flashea con `tools/flash.py` de cada proyecto mediante `STM32_Programmer_CLI -v`.
3. Efectúa lecturas no intrusivas `-r32` en `mode=HOTPLUG` con `sn=` sin detener el microcontrolador ni escribir registros ([`read_swd_mem32`](../../tools/led_l1l2_checks.py#L846-L869)).
4. Registra los valores hexadecimales exactos leídos en el log.
5. Evalúa controles negativos independientes tanto para la tabla en RAM como para el registro `MODER` ([`verify_t4_led_state`](../../tools/led_l1l2_checks.py#L882-L898)).

---

## 6. Elementos no Verificados por el Implementador

- **T4 no fue ejecutada por el implementador:** En estricto cumplimiento de las instrucciones de rol, no se ejecutó ningún comando contra la placa (`flash.py`, `STM32_Programmer_CLI`, `monitor.py`).
- Las mediciones de T4 que sustentan la certificación física del driver corresponden a las registradas por el auditor en [`01_shared_drivers/01_led/docs/led_l1l2_audit_t4_736948f.txt`](docs/led_l1l2_audit_t4_736948f.txt).

---

F1 CORREGIDA 2 PARA AUDITORIA b9fbaad
