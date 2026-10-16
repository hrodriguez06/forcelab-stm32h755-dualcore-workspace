# Reporte de Implementación: Fase F1 de `01_led` (L1/L2)

Oct 16, 2026 · Agente Implementador (Antigravity)
Rama: `driver/led-l1l2` · Base / Control: `9dc6896` · Commit de código/herramienta: `2e6a15d`

---

## 1. Propósito y Decisiones Aprobadas

El objetivo de la fase F1 es resolver las limitaciones L1 y L2 del driver compartido `01_shared_drivers/01_led`, junto con la validación de rango de bits en tiempo de compilación (L5 por la vía de `board_def.h`), de acuerdo con [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1) e [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md):
- **L1 resuelto:** `prj_02`, `prj_03` y `prj_04` definían `BOARD_LED_<x>_PIN` (máscaras `GPIO_PIN_x`), pero el driver bare-metal requería `BOARD_LED_<x>_BIT` (números de bit 0..15). Al no encontrarlos, recurría en silencio a valores hardcodeados por defecto.
- **L2 resuelto:** Se eliminaron los valores hardcodeados por defecto en el driver (`#ifndef ... #define`), reemplazándolos por directivas obligatorias `#error` que hacen cumplir la Regla 1 de drivers compartidos ("never hardcode GPIO pins").
- **L5 (en compilación) resuelto:** Se añadieron comprobaciones `#if (BOARD_LED_<x>_BIT) > 15U` → `#error`.

### Decisiones del Responsable (2026-10-16):
- **Decisión 1 (Alcance):** Opción A. L1, L2 y rango de `_BIT` en compilación. La comprobación dinámica de `pin_num` en `LED_Init_Config()` y L3/L4 quedan fuera de esta fase.
- **Decisión 2 (Prueba en placa T4):** Opción A. Lectura por SWD de `g_led_config` y registros `MODER` en modo HOTPLUG objetivo, sin AD2.
- **Decisión 3 (Documentos de proceso):** Se conservan durante la auditoría y se borran en el cierre.

---

## 2. Detalle de Cambios Realizados y Verificación contra HEAD

Cada cambio fue implementado en commits atómicos mediante bash heredoc y verificado exhaustivamente contra `HEAD`:

### 2.1 H1: Macros obligatorias y comprobación de rango en driver (`4c15fba`)
Archivo modificado: [`01_shared_drivers/01_led/led_bm.c`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/01_led/led_bm.c)
- **Línea 13:** Inclusión directa e incondicional de `board_def.h` (`#include "board_def.h"`), eliminando la comprobación condicional débil `__has_include`.
- **Líneas 15–23:** Macros para `LED_GREEN` (LD1):
  - Líneas 15–17: `#ifndef BOARD_LED_GREEN_PORT` → `#error "BOARD_LED_GREEN_PORT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"`
  - Líneas 18–20: `#ifndef BOARD_LED_GREEN_BIT` → `#error "BOARD_LED_GREEN_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"`
  - Líneas 21–23: `#if (BOARD_LED_GREEN_BIT) > 15U` → `#error "BOARD_LED_GREEN_BIT must be in range 0..15"`
- **Líneas 25–33:** Macros para `LED_YELLOW` (LD2):
  - Líneas 25–27: `#ifndef BOARD_LED_YELLOW_PORT` → `#error "BOARD_LED_YELLOW_PORT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"`
  - Líneas 28–30: `#ifndef BOARD_LED_YELLOW_BIT` → `#error "BOARD_LED_YELLOW_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"`
  - Líneas 31–33: `#if (BOARD_LED_YELLOW_BIT) > 15U` → `#error "BOARD_LED_YELLOW_BIT must be in range 0..15"`
- **Líneas 35–43:** Macros para `LED_RED` (LD3):
  - Líneas 35–37: `#ifndef BOARD_LED_RED_PORT` → `#error "BOARD_LED_RED_PORT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"`
  - Líneas 38–40: `#ifndef BOARD_LED_RED_BIT` → `#error "BOARD_LED_RED_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"`
  - Líneas 41–43: `#if (BOARD_LED_RED_BIT) > 15U` → `#error "BOARD_LED_RED_BIT must be in range 0..15"`
- **Líneas 45–51:** Definición estática de `g_led_config` inicializada con las macros garantizadas de `board_def.h`.

### 2.2 H2: Migración de `_PIN` a `_BIT` en `board_def.h` (`4c15fba`)
Se sustituyeron las definiciones de máscaras `BOARD_LED_<x>_PIN` por números de bit `BOARD_LED_<x>_BIT`, incorporando el comentario de advertencia arquitectónica alineado con `prj_01`:
- [`02_firmware_projects/prj_02_eth_lwip_dualcore/Common/Inc/board_def.h#L69-L79`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/02_firmware_projects/prj_02_eth_lwip_dualcore/Common/Inc/board_def.h#L69-L79)
- [`02_firmware_projects/prj_03_adc_dma_eth/Common/Inc/board_def.h#L69-L79`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/02_firmware_projects/prj_03_adc_dma_eth/Common/Inc/board_def.h#L69-L79)
- [`02_firmware_projects/prj_04_adc_dma_bm_eth/Common/Inc/board_def.h#L69-L79`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/02_firmware_projects/prj_04_adc_dma_bm_eth/Common/Inc/board_def.h#L69-L79)
- Se constató que [`03_firmware_templates/prj_01_template_dualcore/Common/Inc/board_def.h#L49-L60`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/Common/Inc/board_def.h#L49-L60) ya definía `BOARD_LED_<x>_BIT` correctamente y no requirió modificaciones.

### 2.3 H3: Herramienta de comprobación y harness de verificación (`2e6a15d`)
Archivo creado: [`tools/led_l1l2_checks.py`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/led_l1l2_checks.py) (852 líneas).
- Incorpora T1, T2 y T3 automatizados sin placa, y T4 para ejecución por el auditor en placa.
- Garantiza que cada control negativo pasa exactamente por la misma función que la comprobación real con entradas mutadas en directorios temporales fuera del árbol del repositorio.
- Gestiona automáticamente el worktree de control en `9dc6896` y el junction NTFS hacia `STM32CubeH7`, compilando los 8 binarios de control dentro de la propia ejecución.
- Implementa logging secuencial con timestamp de milisegundos, verificación estricta de árbol limpio y volcado a archivo UTF-8 de corrida única.

---

## 3. Resultados de las Pruebas sin Placa (T1, T2, T3)

La ejecución completa de verificación sin placa fue capturada y commiteada en:
[`01_shared_drivers/01_led/docs/led_l1l2_checks_2e6a15d.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/01_led/docs/led_l1l2_checks_2e6a15d.txt) (commit `78aee90`).

### 3.1 T1: Compilación limpia de matriz y presets selftest
- **Matriz de producción (`tools/build_all.ps1 -Clean`):** 8/8 compilaciones exitosas (exit code 0, 0 warnings, 0 errores) en 203.5 segundos:
  - `prj_01_template_dualcore` (debug y release)
  - `prj_02_eth_lwip_dualcore` (debug y release)
  - `prj_03_adc_dma_eth` (debug y release)
  - `prj_04_adc_dma_bm_eth` (debug y release)
- **Presets de selftest que enlazan `led_bm` (con `--fresh` y `--clean-first`):** 9/9 presets exitosos con 0 warnings y 0 errores:
  - `prj_01_template_dualcore`: `selftest-gpio`, `selftest-tim`, `selftest-spi`, `selftest-i2c`.
  - `prj_04_adc_dma_bm_eth`: `selftest`, `selftest-adc`, `selftest-uart`, `selftest-hsem`, `selftest-gpio`.
- **Control negativo:** Se inyectó una línea de advertencia sintética a través del mismo analizador de warnings (`check_build_output_warnings`), verificando que se rechaza inmediatamente (PASS).
- **Veredicto T1:** `PASS`.

### 3.2 T2: Comprobación de `#error` y rango de bits en preprocesador
Compilación con `arm-none-eabi-gcc -fsyntax-only` usando los flags exactos de cada proyecto (`compile_commands.json`) contra archivos `board_def.h` generados en directorios temporales fuera del repositorio:
- **Controles positivos:** Los 4 proyectos compilan limpiamente (exit code 0).
- **16 Mutaciones negativas (4 por proyecto):**
  1. Sin `BOARD_LED_RED_BIT`: Falla con `#error "BOARD_LED_RED_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"` en línea 39.
  2. Sin `BOARD_LED_GREEN_PORT`: Falla con `#error "BOARD_LED_GREEN_PORT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"` en línea 16.
  3. `_PIN` en lugar de `_BIT` (estado original anterior a F1): Falla con `#error "BOARD_LED_GREEN_BIT must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"` en línea 19.
  4. `BOARD_LED_YELLOW_BIT 16U`: Falla con `#error "BOARD_LED_YELLOW_BIT must be in range 0..15"` en línea 32.
- **Resultado:** 20/20 casos comprobados con éxito.
- **Veredicto T2:** `PASS`.

### 3.3 T3: Idéntico contenido binario de `g_led_config`
Extracción mediante `arm-none-eabi-objdump -s` de la sección de datos del objeto CM7 `led_bm.c.obj` (24 bytes: 3 entradas de 8 bytes `[port, pin_num]`):
- **Oráculo UM2408:** PB0 (LD1 Verde, `0x58020400`, bit 0), PE1 (LD2 Amarillo, `0x58021000`, bit 1), PB14 (LD3 Rojo, `0x58020400`, bit 14/0x0e).
- **8 Builds actuales:** Los 8 binarios de producción coinciden exactamente con el oráculo UM2408 (`words=['0x58020400', '0x0', '0x58021000', '0x1', '0x58020400', '0xe']`).
- **Compilación in-situ del control `9dc6896`:** La herramienta compiló los 8 proyectos del worktree de control en 230.7 segundos. Los 8 binarios de control coinciden al 100% con el oráculo UM2408 y con los binarios actuales.
- **Control negativo:** Se inyectó un byte mutado (`0x0f` en vez de `0x0e`) en la salida de `objdump` a través de la misma función de análisis (`extract_and_verify_led_config`), comprobando que es detectado y rechazado (PASS).
- **Veredicto T3:** `PASS`.

---

## 4. Instrucciones para la Prueba en Placa (T4) para el Auditor

La prueba en placa T4 verifica en hardware real (Nucleo-144 STM32H755ZI-Q) que tras el arranque de cada proyecto en configuración `debug`:
1. `g_led_config` en RAM contiene la configuración idéntica a UM2408.
2. Los registros `MODER` de GPIOB y GPIOE configuran PB0, PE1 y PB14 como salidas de propósito general (`01b`).

### Comando exacto para el auditor:
```bash
py tools/led_l1l2_checks.py --test t4 --output 01_shared_drivers/01_led/docs/led_l1l2_t4_<hash>.txt
```
*(Nota: El script utiliza por defecto el número de serie de la placa ST-LINK `0016000C4142501220353451`; si se usa otro, puede especificarse con `--sn <numero_serie>`)*.

### Procedimiento seguro implementado en el harness T4:
- Flasheo de cada proyecto mediante `tools/flash.py` del propio proyecto con verificación de hash.
- Espera de arranque estabilizado (2.0 segundos).
- Lectura no intrusiva por SWD mediante `STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32`:
  - No detiene el procesador (`mode=HOTPLUG`).
  - No escribe ningún registro ni memoria (`-r32`).
  - No altera option bytes, relojes ni fuentes de alimentación.
- Evaluación automática contra el oráculo UM2408 y ejecución del control negativo de tabla alterada.

---

## 5. Elementos no Verificados por el Implementador

De acuerdo con las restricciones de rol (implementador sin acceso a hardware):
- **T4 queda NO VERIFICADA por el implementador:** No se ejecutaron comandos de flasheo (`flash.py`, `STM32_Programmer_CLI`) ni monitores serie (`monitor.py`).
- Corresponde al auditor ejecutar T4 en la placa física y registrar la captura correspondiente en `01_shared_drivers/01_led/docs/`.

---

F1 ENTREGADA PARA AUDITORIA 2e6a15d
