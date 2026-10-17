# Informe de Implementación - Cierre del Driver 00_gpio (Fase F1)

## 1. Resumen

Este informe documenta la ejecución completa y corregida de la fase **F1** para el cierre definitivo del driver `00_gpio`, conforme a las directrices de [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) (revisión 1), [IMPLEMENTER_INSTRUCTIONS.md](IMPLEMENTER_INSTRUCTIONS.md) y las observaciones de auditoría recogidas en [AUDIT.md](AUDIT.md).

- **Rama de trabajo:** `driver/gpio-close` (base `f066a8e`).
- **Estado de entrega:** **Entregado para auditoría (F1 corregida)**.
- **Driver inalterado:** Los archivos fuente y cabeceras de `01_shared_drivers/00_gpio/` (`gpio_bm.c`, `gpio_bm.h`, `gpio_ll.c`, `gpio_ll.h`, `gpio_hal.c`, `gpio_hal.h`) **no han sufrido modificación alguna** respecto a `f066a8e`.
- **Decisiones aplicadas:** Aprobadas por el responsable:
  - Decisión 1 (Opción A): EXTI y celda de compensación se mantienen en `gpio_bm`, accesibles desde cualquier tier sin envoltorios redundantes.
  - Decisión 2 (Opción A): `Error_Handler()` ante timeout de CSI o fallo de la celda de compensación.
  - Decisión 3 (Opción A): Alcance de T4 en hardware: `prj_02` (debug), `prj_03` (debug), `prj_04` (debug y release), y 5 warm resets por NRST en `prj_04` debug.
  - Decisión 4: Los documentos de proceso se eliminarán al cierre final.

### Commits realizados
1. `f883944` - `feat(prj02,prj03,prj04): enable the I/O compensation cell through gpio_bm (K1)`
2. `5826eb2` - `fix(tools): address audit findings B1-B7, M1, m1-m2 in gpio_close_checks.py`
3. `ed5c5dd` - `test(gpio): record T1-T3 and T4 simulation close runs (no board)`

---

## 2. K1: Inicialización de la Celda de Compensación (Diseño 1)

Se implementó una inicialización dedicada y uniforme de la celda de compensación I/O mediante `GPIO_BM_EnableCompensationCell()` en los Cortex-M7 de `prj_02`, `prj_03` y `prj_04`.

### 2.1 Pasos del Diseño 1 y Referencias `archivo:línea`

| Paso del Diseño 1 | `prj_02_eth_lwip_dualcore` | `prj_03_adc_dma_eth` | `prj_04_adc_dma_bm_eth` |
| --- | --- | --- | --- |
| Inclusión de `gpio_bm.h` | [main.c:27](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/Core/Src/main.c#L27) | [main.c:28](../../02_firmware_projects/prj_03_adc_dma_eth/CM7/Core/Src/main.c#L28) | [main.c:28](../../02_firmware_projects/prj_04_adc_dma_bm_eth/CM7/Core/Src/main.c#L28) |
| Prototipo `static void CompensationCell_Init(void)` | [main.c:30](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/Core/Src/main.c#L30) | [main.c:31](../../02_firmware_projects/prj_03_adc_dma_eth/CM7/Core/Src/main.c#L31) | [main.c:42](../../02_firmware_projects/prj_04_adc_dma_bm_eth/CM7/Core/Src/main.c#L42) |
| Llamada en `main()` (entre `SystemClock_Config` y `PeripheralClock_Config`) | [main.c:89](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/Core/Src/main.c#L89) | [main.c:90](../../02_firmware_projects/prj_03_adc_dma_eth/CM7/Core/Src/main.c#L90) | [main.c:193](../../02_firmware_projects/prj_04_adc_dma_bm_eth/CM7/Core/Src/main.c#L193) |
| Comentario explicativo `osc.CSIState = RCC_CSI_OFF` | [main.c:194](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/Core/Src/main.c#L194) | [main.c:206](../../02_firmware_projects/prj_03_adc_dma_eth/CM7/Core/Src/main.c#L206) | [main.c:315](../../02_firmware_projects/prj_04_adc_dma_bm_eth/CM7/Core/Src/main.c#L315) |
| Constantes locales `#define COMP_CELL_*` | [main.c:230-234](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/Core/Src/main.c#L230-L234) | [main.c:242-246](../../02_firmware_projects/prj_03_adc_dma_eth/CM7/Core/Src/main.c#L242-L246) | [main.c:351-355](../../02_firmware_projects/prj_04_adc_dma_bm_eth/CM7/Core/Src/main.c#L351-L355) |
| Definición idéntica `CompensationCell_Init(void)` | [main.c:246-271](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/Core/Src/main.c#L246-L271) | [main.c:258-283](../../02_firmware_projects/prj_03_adc_dma_eth/CM7/Core/Src/main.c#L258-L283) | [main.c:367-392](../../02_firmware_projects/prj_04_adc_dma_bm_eth/CM7/Core/Src/main.c#L367-L392) |

### 2.2 Diff de `SystemClock_Config()`

El diff en `SystemClock_Config()` es estrictamente la eliminación de las 3 líneas previas junto con su comentario, y la incorporación del comentario explicativo sobre `osc.CSIState`:

```diff
@@ -193,6 +194,7 @@ static void SystemClock_Config(void)
     osc.HSEState         = RCC_HSE_BYPASS;
     osc.HSIState         = RCC_HSI_OFF;
+    /* Field has no effect: OscillatorType only includes HSE; CSI is enabled in CompensationCell_Init(). */
     osc.CSIState         = RCC_CSI_OFF;
     osc.PLL.PLLState     = RCC_PLL_ON;
...
@@ -220,9 +222,0 @@ static void SystemClock_Config(void)
-    /* The I/O compensation cell trims the slew rate of the fast I/O banks. ST
-       recommends it above 100 MHz HCLK for high-speed interfaces, and the
-       50 MHz RMII pins are exactly that case. */
-    __HAL_RCC_CSI_ENABLE();
-    __HAL_RCC_SYSCFG_CLK_ENABLE();
-    HAL_EnableCompensationCell();
 }
```

### 2.3 Secuencia en `CompensationCell_Init()`
1. **DWT CYCCNT:** Inicializado idénticamente a `HSEM_BM_TimebaseStart()` (`CoreDebug->DEMCR |= TRCENA`, desbloqueo de `0xE0001FB0` con `0xC5ACCE55`, `DWT->CTRL |= CYCCNTENA`).
2. **CSI:** Activado con `__HAL_RCC_CSI_ENABLE()`.
3. **Espera acotada CSIRDY:** Límite `COMP_CELL_CSI_MAX_CYCLES = 800000UL` (2 ms a 400 MHz). En caso de timeout, llama a `Error_Handler()`.
4. **Habilitación de celda por driver:** `GPIO_BM_EnableCompensationCell(COMP_CELL_MAX_CYCLES)` con `COMP_CELL_MAX_CYCLES = 2000000UL` (5 ms a 400 MHz, ~26x el valor de latencia medido de 76 493 ciclos). En caso de fallo o timeout, llama a `Error_Handler()`.
5. **Exclusión PRIMASK:** Se documenta que el hook cross-core aún no está instalado en este punto (`Shared_GpioLockInstall()` se ejecuta con posterioridad) y CPU2 se encuentra retenido/parqueado, por lo que PRIMASK dentro del driver provee exclusión atómica suficiente.

---

## 3. Resultados sin placa (T1–T3)

La suite de verificación host implementada en `tools/gpio_close_checks.py` fue ejecutada de forma automatizada y completa, registrando un veredicto general de **PASS**.

- **Archivo de captura T1-T3 commiteado:** [`docs/gpio_close_t1t3_5826eb2.txt`](docs/gpio_close_t1t3_5826eb2.txt)
- **Archivo de captura T4 simulación commiteado:** [`docs/gpio_close_t4sim_5826eb2.txt`](docs/gpio_close_t4sim_5826eb2.txt)
- **Commit base de comparación:** `f066a8e`
- **Árbol de trabajo:** Limpio antes y después de la ejecución (`PASS`).

### Resumen de Checks y Controles Negativos

| Prueba | Descripción | Resultado Real | Control Negativo (Misma Función) | Veredicto |
| --- | --- | --- | --- | --- |
| **T1** | Compilación directa de las 8 configuraciones de la matriz (`compile_single_config` con `--clean-first`), verificación de warnings en la salida del compilador, frescura de binarios por mtime y compilación de los presets selftest (`prj_01` selftest-gpio, `prj_03` selftest, `prj_04` selftest-gpio). | 0 errores, 0 warnings, todas las imágenes recién generadas (8/8 matriz + 3 selftests). | Configuración `-B` en directorio temporal fuera del repositorio con `-DCMAKE_C_FLAGS=-Wconversion`, compilada y evaluada con `evaluate_build_output()`; detecta 224 warnings reales y evalúa a `False`. | **PASS** |
| **T2 (Símbolos)** | Inspección `nm` en `main.c.obj` y ELFs de CM7 (`prj_02`, `prj_03`, `prj_04`, `debug` y `release`). | `GPIO_BM_EnableCompensationCell` sin resolver (`U`), 0 referencias a `HAL_EnableCompensationCell` en `main.c.obj` y 0 en ELF. | Objetos del control `f066a8e` evaluados por la misma función; detecta `HAL_EnableCompensationCell` y evalúa a `False`. | **PASS** |
| **T2 (Texto)** | Identidad textual estricta de `CompensationCell_Init()` en los tres proyectos. | Byte por byte idénticos (1674 caracteres extraídos). | Copia de texto con identificador mutado evaluada por la misma función; detecta discrepancia y evalúa a `False`. | **PASS** |
| **T2 (Equivalencia binaria)** | Comparación bidireccional exhaustiva de conjuntos de objetos (320 objetos por preset) y comparación profunda de secciones ALLOC, contenido raw, relocalizaciones y símbolos contra `f066a8e` (2 454 objetos). | Todos los objetos fuera de CM7 `main.c.obj` son idénticos al control f066a8e. | Objeto con byte mutado en sección ALLOC `.text.main` (offset `0x4ec` resuelto dinámicamente desde `File off` de `arm-none-eabi-objdump -h`) evaluado por el comparador; detecta discrepancia de contenido y evalúa a `False`. | **PASS** |
| **T3** | Enlace estricto contra objetos `gpio_hal.c.obj` y `gpio_bm.c.obj` resueltos dinámicamente desde `build.ninja`. Archivo `.c` temporal con `#include "gpio_hal.h"` y `#include "gpio_bm.h"`, compilado con DEFINES, FLAGS e INCLUDES de `gpio_hal.c.obj` extraídos de `build.ninja`, llamando a `GPIO_HAL_Init`, `GPIO_BM_ExtiConfig` y `GPIO_BM_EnableCompensationCell` con firmas reales. Stubs únicamente para llamadas externas no-gpio (`HAL_GPIO_Init`, `HAL_GPIO_LockPin`, `Error_Handler`). | Enlace completo y enlace parcial `gcc -r` limpios sin símbolos sin resolver. | Enlace sin el objeto `gpio_bm.c.obj`; detecta símbolos sin resolver de `GPIO_BM_*` y evalúa a `False`. | **PASS** |

---

## 4. Prueba en Placa (T4) - Para el Auditor

### 4.1 Alcance y Declaración de Estado
- **Estado:** **NO VERIFICADA SIN PLACA**.
- **Validación del Implementador:** La herramienta `tools/gpio_close_checks.py` cuenta con modo de simulación y controles negativos sobre respuestas CLI (`--simulate-t4`), los cuales fueron ejecutados y verificados con éxito, generando la captura commiteada [`docs/gpio_close_t4sim_5826eb2.txt`](docs/gpio_close_t4sim_5826eb2.txt) con "SIMULATED" en la cabecera. Asimismo, invocar `--test t4` sin `--sn` aborta de forma segura con código 1 sin tocar el hardware.

### 4.2 Qué hace T4 y qué imprime
1. **Compilación fresca (B3):** Compila en la ejecución las 4 imágenes requeridas (`prj_02` debug, `prj_03` debug, `prj_04` debug, `prj_04` release) con `--clean-first`, comprueba su frescura temporal y registra el SHA-256 de los ELFs de CM7 y CM4.
2. **Flasheo verificado (B4):** Invoca el `tools/flash.py` propio de cada proyecto con `cwd` en el proyecto, `--preset <PRESET>` y `--sn <SERIAL>`. La salida completa del programador se vuelca a la captura y el éxito exige código de salida 0 y la cadena `Download verified successfully`.
3. **Lectura atómica de registros SWD:** Tras cada arranque, ejecuta **una sola llamada SWD en modo `HOTPLUG`** con:
   ```
   STM32_Programmer_CLI -c port=SWD sn=<SERIAL> mode=HOTPLUG -r32 0x58000420 4 -r32 0x58024400 4
   ```
   para leer atómicamente:
   - `SYSCFG_CCCSR` (`0x58000420`): comprueba `EN == 1` (bit 0) y `READY == 1` (bit 8).
   - `RCC_CR` (`0x58024400`): comprueba `CSIRDY == 1` (bit 8, máscara `0x100`).
4. **Comprobación de red con reintento (B5):** Ejecuta `tools/net_check.py` con un bucle de reintento de hasta 20 s para acomodar la latencia de autonegociación del enlace Ethernet (~3,2 s), registrando cada intento y resultado en la captura.
5. **Resets NRST (B6):** En `prj_04` debug, ejecuta 5 warm resets consecutivos por hardware mediante `-hardRst`, comprobando el código de salida 0 del comando y registrando `CCCSR`, `RCC_CR` y el resultado de `net_check` en cada reset (Decisión 3).
6. **Controles negativos en hardware (B7):** El texto real devuelto por el CLI se muta en memoria (`READY = 0` y `EN = 0`) y se evalúa a través de `parse_swd_hotplug_output()` y `evaluate_t4_registers()`, verificando que ambas mutaciones producen FAIL.

### 4.3 Comandos exactos para el Auditor

```powershell
# Posicionarse en la raíz del repositorio stm32h755 (rama driver/gpio-close)
cd C:\Users\hrodriguez\Documents\R+D\00_Embedded_System\00_STmicro\stm32h755

# Ejecutar T4 completo en hardware con el número de serie de la placa
py tools/gpio_close_checks.py --test t4 --sn 0016000C4142501220353451 --output 01_shared_drivers/00_gpio/docs/gpio_close_audit_t4_5826eb2.txt
```

---

## 5. Desviaciones respecto al Plan y Resoluciones de Auditoría

1. **Gestión del submódulo STM32CubeH7 sin escrituras de configuración (B2):**
   Para no modificar la configuración del repositorio principal ni de los worktrees (`.git/config`), se eliminó toda invocación de `git config`. Las comprobaciones de estado en el control utilizan la opción de línea de comandos `--ignore-submodules`, y durante las compilaciones CMake en el worktree de control, la directiva de ignorar el submódulo se propaga limpiamente a través de variables de entorno estándar de Git (`GIT_CONFIG_COUNT=1`, `GIT_CONFIG_KEY_0=submodule.02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7.ignore`, `GIT_CONFIG_VALUE_0=all`), manteniendo `.git/config` absolutamente inalterado.
2. **Determinismo binario y comparación bidireccional en T2 (m1, m2):**
   - Se implementó la comparación bidireccional estricta de conjuntos de objetos (`head_keys == ctrl_keys`) antes de proceder a la comparación binaria profunda.
   - En el control negativo de mutación, el offset del byte de `.text.main` se extrae dinámicamente analizando la columna `File off` en la salida de `arm-none-eabi-objdump -h` (offset `0x4ec`), eliminando offsets fijos.
   - Se configuró `-ffile-prefix-map=<control_dir>=<repo_root>` en la compilación del control para alinear macros `__FILE__`.
   - Se normalizan `FW_GIT_REV` y `FW_BUILD_ID` únicamente en `main.c.obj` y `app_main.c.obj`.
   - En la extracción de símbolos (`extract_nondebug_symbols`), se ignoran símbolos de depuración de tipo `a` y `n` (así como identificadores internos de módulo del compilador `wm4.*` y `wm7.*`), permitiendo comparar con total fidelidad la tabla de símbolos funcionales.

---

## 6. Checklist de Entrega

- [x] `git status --porcelain -u`: árbol limpio fuera de `build/`.
- [x] `git diff f066a8e -- 01_shared_drivers/00_gpio/`: 0 cambios en `*.c` y `*.h`.
- [x] `git diff f066a8e --stat`: únicamente `main.c` de los CM7 de `prj_02`, `prj_03` y `prj_04`, la herramienta `tools/gpio_close_checks.py`, las capturas commiteadas y este informe.
- [x] `build_all.ps1 -Clean` / `gpio_close_checks.py --test t1`: 8/8 builds superados con 0 errores y 0 warnings mediante compilación directa `--clean-first`.
- [x] Presets de selftest (`prj_01` selftest-gpio, `prj_03` selftest, `prj_04` selftest-gpio) compilados sin warnings.
- [x] Las tres funciones `CompensationCell_Init()` son idénticas byte por byte.
- [x] `SystemClock_Config()` únicamente retira las tres líneas de HAL y añade el comentario explicativo de `CSIState`.
- [x] Todos los controles negativos (T1 inyección de flags, T2 símbolos control, T2 mutación de objeto dinámica, T2 mutación de texto, T3 enlace sin BM, T4 registros con READY=0 y EN=0) pasan por las mismas funciones y fallan según lo esperado.
- [x] Capturas commiteadas: `docs/gpio_close_t1t3_5826eb2.txt` y `docs/gpio_close_t4sim_5826eb2.txt`.
- [x] Citas de código y registro verificadas contra HEAD (`-r32 ... 4`, `CSIRDY` bit 8 `0x100`, `-hardRst`, enlaces relativos).
- [x] Última línea del informe con el formato exacto requerido.

---

F1 CORREGIDA PARA AUDITORIA 5826eb2
