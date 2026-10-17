# Informe de Implementación - Cierre del Driver 00_gpio (K1–K5)

**Fecha:** 2026-10-17  
**Rama:** `driver/gpio-close`  
**Estado:** **CIERRE ENTREGADO PARA AUDITORÍA**  
**Commit base:** `f066a8e`  
**Commit de documentación y poda (K3):** `6f0967d`  
**Commit de auditoría en hardware:** `f3fc263`  

---

## 1. Resumen

Este informe documenta la ejecución completa y cierre definitivo del driver `00_gpio` (hitos K1 a K5), conforme a las directrices de [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) (revisión 1), [IMPLEMENTER_INSTRUCTIONS.md](IMPLEMENTER_INSTRUCTIONS.md) y las resoluciones de auditoría en [AUDIT.md](AUDIT.md).

- **Driver inalterado:** Los archivos fuente y cabeceras de `01_shared_drivers/00_gpio/` (`gpio_bm.c`, `gpio_bm.h`, `gpio_ll.c`, `gpio_ll.h`, `gpio_hal.c`, `gpio_hal.h`) **no han sufrido modificación alguna** respecto a `f066a8e`.
- **Decisiones aplicadas:** Aprobadas por el responsable del proyecto:
  - Decisión 1 (Opción A): EXTI 0–15 y celda de compensación se mantienen en `gpio_bm`, accesibles directamente desde cualquier tier sin envoltorios redundantes en LL o HAL (K2). G6 cerrado por diseño.
  - Decisión 2 (Opción A): `Error_Handler()` ante timeout de CSI o fallo al habilitar la celda de compensación I/O en arranque.
  - Decisión 3 (Opción A): Alcance de T4 en hardware: `prj_02` (debug), `prj_03` (debug), `prj_04` (debug y release), y 5 warm resets consecutivos por NRST en `prj_04` debug.
  - Decisión 4: Los documentos de proceso se conservan hasta el cierre final del responsable.

### Commits de la rama `driver/gpio-close`
1. `f883944` - `feat(prj02,prj03,prj04): enable the I/O compensation cell through gpio_bm (K1)`
2. `2f929a4` - `feat(tools): add gpio_close_checks.py (K4)`
3. `5826eb2` - `fix(tools): address audit findings B1-B7, M1, m1-m2 in gpio_close_checks.py`
4. `ed5c5dd` - `test(gpio): record T1-T3 and T4 simulation close runs (no board)`
5. `ac59a73` - `docs(gpio): update implementation report with audit corrections`
6. `987f310` - `fix(tools): point the T4 net check at the board's address`
7. `f3fc263` - `test(gpio): auditor T1-T4 run of the driver close passes on the board`
8. `6f0967d` - `docs(gpio): document driver close K1-K3 in README (K3)`

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

## 4. Resultados en Placa (T4) - Auditoría en Hardware

### 4.1 Ejecución del Auditor en Hardware
- **Captura en placa commiteada:** [`docs/gpio_close_audit_t1t4_987f310.txt`](docs/gpio_close_audit_t1t4_987f310.txt)
- **Commit auditado:** `987f310` (incorporó corrección de IP de placa `192.168.1.150` para `net_check`)
- **Placa física:** NUCLEO-H755ZI-Q, ST-LINK SN `0016000C4142501220353451`
- **Veredicto general:** **`OVERALL VERDICT: PASS`** (55 comprobaciones PASS, 0 FAIL)

### 4.2 Verificación de Registros y Red en Hardware

| Imagen | SHA-256 del ELF (CM7) | Flasheo CLI | `SYSCFG_CCCSR` (`0x58000420`) | `RCC_CR` (`0x58024400`) | `net_check` |
| --- | --- | --- | --- | --- | --- |
| `prj_02` `debug` | `a3c20ca4...` | `Download verified successfully` | `0x00000101` (`EN=1`, `READY=1`) | `0x0307C1A5` (`CSIRDY=1`) | PASS (intento 2, 4/4 ping) |
| `prj_03` `debug` | `4147775a...` | `Download verified successfully` | `0x00000101` (`EN=1`, `READY=1`) | `0x0F07C1A5` (`CSIRDY=1`, `PLL2`) | PASS (intento 2, 4/4 ping) |
| `prj_04` `debug` | `6ae5785a...` | `Download verified successfully` | `0x00000101` (`EN=1`, `READY=1`) | `0x0F07C1A5` (`CSIRDY=1`, `PLL2`) | PASS (intento 2, 4/4 ping) |
| `prj_04` `release` | `77e5dff7...` | `Download verified successfully` | `0x00000101` (`EN=1`, `READY=1`) | `0x0F07C1A5` (`CSIRDY=1`, `PLL2`) | PASS (intento 2, 4/4 ping) |

### 4.3 Resets por NRST y Controles Negativos en Hardware
- **5 Warm Resets por Hardware en `prj_04` debug (`-hardRst`):** Los 5 resets consecutivos superaron la verificación con código de salida 0, confirmando en cada ciclo `SYSCFG_CCCSR = 0x00000101`, `RCC_CR = 0x0F07C1A5` y `net_check` PASS.
- **Controles negativos en hardware:** La salida textual real devuelta por `STM32_Programmer_CLI` fue mutada en memoria sustituyendo `READY = 0`, y por separado `EN = 0`. Ambas mutaciones pasaron por `evaluate_t4_registers()` y produjeron veredicto FAIL conforme a diseño.
- **Comportamiento de red:** En cada arranque físico, el intento 1 de `net_check` registró pérdida parcial de ping (ICMP 3/4) debido al tiempo de estabilización física del enlace Ethernet (~3,2 s), pasando estrictamente en el intento 2 (ICMP 4/4), validando la necesidad del bucle de reintento acotado (B5).
- **Bits de PLL2:** Los bits 26–27 de `RCC_CR` (`PLL2ON`, `PLL2RDY`) están presentes en `prj_03` y `prj_04` (`0x0F07C1A5`), correspondientes al reloj de muestreo del ADC, mientras que en `prj_02` se mantienen a 0 (`0x0307C1A5`), confirmando fidelidad respecto a los contratos de reloj de cada proyecto.

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
3. **Dirección IP de la placa en T4 (987f310):**
   El auditor corrigió la dirección de sondeo UDP en `run_net_check_with_retry()` para apuntar a `192.168.1.150` (`net_config.h` de los proyectos), en lugar de `.50`.

---

## 6. Cierre de Fase (K3 - Documentación y Poda de Capturas)

El implementador completó las actividades de documentación y saneamiento requeridas para el cierre del driver:

1. **Actualización completa de `01_shared_drivers/00_gpio/README.md` (Diseño 3):**
   - **Estado inicial (líneas 28–29):** Actualizado para reflejar que EXTI 0–15 y la celda de compensación están implementados en `gpio_bm` (F2), y que el driver se encuentra completo y cerrado (K1–K5, Oct 2026).
   - **Matriz de características (§1.1):** Actualizada la fila `EXTI, compensation cell` a `yes` en BM, y `via BM (K2)` en LL y HAL.
   - **Ejemplos (§7):** Añadido el Ejemplo F mostrando la configuración de un pin mediante `gpio_hal` junto con el enrutamiento y habilitación de su línea EXTI mediante `GPIO_BM_ExtiConfig()`, compilable contra las firmas reales de `gpio_bm.h`.
   - **Preámbulo y evidencia (§9.1, §9.3.1):** Documentada la ejecución de auditoría en hardware a `f3fc263` (`docs/gpio_close_audit_t1t4_987f310.txt`) y registradas las capturas de verificación del cierre.
   - **Puntos no ejercitados (§9.5):** Eliminado el primer punto obsoleto sobre `prj_04` sin hook (cerrado en F1b); actualizado el punto de la celda de compensación indicando que `prj_02`, `prj_03` y `prj_04` la inicializan mediante el driver (`GPIO_BM_EnableCompensationCell()`), quedando sin ejercitar únicamente en CM4.
   - **Consumidores (§11):** Incorporado `prj_02_eth_lwip_dualcore` en la tabla de consumidores; actualizadas las filas de `prj_03` y `prj_04`; actualizado el párrafo posterior reflejando la inicialización en arranque de los tres proyectos de producción.
   - **Hallazgos (§12.2):** Actualizados G6 como "Closed by design (K2)", G8 como "Closed (F2 + K1)" y G12 como "Closed (F1 + F2)".
   - **Registros de fases (§13):** Renombrada la sección 13 a "Phase records" e incorporada la subsección §13.4 resumiendo los hitos K1–K5 y la evidencia cuantitativa de las pruebas T1–T4.
   - **Historial (§15):** Añadida la fila del cierre del driver cubriendo el rango de commits `f883944 … f3fc263`.

2. **Poda de capturas según Regla 7 de Sección 14:**
   - Se eliminó del repositorio mediante `git rm` la captura superada `01_shared_drivers/00_gpio/docs/gpio_close_t1t3_2f929a4.txt`, generada con la versión preliminar de la herramienta antes de las correcciones B1–B7. Dicha captura queda registrada en la tabla §9.3.2 de capturas retiradas y accesible mediante `git show 2320b0f:01_shared_drivers/00_gpio/docs/gpio_close_t1t3_2f929a4.txt`.
   - Se conservan exactamente las tres capturas canónicas de respaldo:
     1. `01_shared_drivers/00_gpio/docs/gpio_close_t1t3_5826eb2.txt` (T1–T3 en host, PASS).
     2. `01_shared_drivers/00_gpio/docs/gpio_close_t4sim_5826eb2.txt` (T4 simulada, PASS).
     3. `01_shared_drivers/00_gpio/docs/gpio_close_audit_t1t4_987f310.txt` (T1–T4 en hardware físico, 55/55 PASS).

---

## 7. Checklist de Entrega

- [x] `git status --porcelain -u`: árbol de trabajo limpio.
- [x] `git diff f066a8e -- 01_shared_drivers/00_gpio/`: 0 modificaciones en archivos `*.c` y `*.h`.
- [x] `git diff f066a8e --stat`: únicamente los `main.c` de CM7 de `prj_02`, `prj_03`, `prj_04`, la herramienta `tools/gpio_close_checks.py`, la documentación en `00_gpio/README.md`, las capturas canónicas y este informe.
- [x] `build_all.ps1 -Clean` / `gpio_close_checks.py --test t1`: 8/8 builds completados con 0 errores y 0 warnings.
- [x] Presets de selftest (`prj_01` selftest-gpio, `prj_03` selftest, `prj_04` selftest-gpio) compilados sin advertencias.
- [x] Las tres funciones `CompensationCell_Init()` son idénticas byte por byte.
- [x] `SystemClock_Config()` retira exclusivamente las 3 líneas de HAL e incorpora el comentario de `CSIState`.
- [x] Todos los controles negativos de T1, T2, T3 y T4 pasan por las mismas funciones de evaluación y fallan según lo esperado.
- [x] Captura de auditoría T1–T4 en hardware físico aprobada con 55/55 checks PASS (`gpio_close_audit_t1t4_987f310.txt`).
- [x] Captura superada `gpio_close_t1t3_2f929a4.txt` eliminada conforme a la regla 7 de la sección 14.
- [x] Citas de código, registros y commits verificadas contra `HEAD`.
- [x] Última línea del informe con el formato exacto requerido.

---

CIERRE ENTREGADO PARA AUDITORIA 6f0967d
