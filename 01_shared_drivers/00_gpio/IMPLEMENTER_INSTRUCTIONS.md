# Instrucciones para el implementador de `00_gpio`: cierre del driver (K1–K5)

Oct 16, 2026 · @Hector

Vas a implementar el plan [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1) de esta carpeta:
- **K1:** la celda de compensación de `prj_02`, `prj_03` y `prj_04` por `GPIO_BM_EnableCompensationCell()`;
- **K4:** la herramienta `tools/gpio_close_checks.py`;
- T1–T3 sin placa y el informe;
- más tarde, en el cierre, la documentación (K3).

**El código del driver (`01_shared_drivers/00_gpio/*.c`, `*.h`) no cambia ni una línea.** Otra persona auditará tu fase y ejecutará T4 en la placa.

Si algo de este documento choca con el plan, manda el plan, salvo las prohibiciones de la sección 2.2, que son absolutas.

---

## 1. Tu papel

**Tú implementas y pruebas sin placa. No auditas.**

- El estado final de tu entrega es "entregado para auditoría", nunca "auditado", "aprobado" ni "cerrado".
- No crees, edites ni borres `01_shared_drivers/00_gpio/AUDIT.md`. Si no estás de acuerdo con una observación, argumenta en tu informe.
- Un hallazgo corregido se declara "corregido en `<hash>`, pendiente de verificación".
- No toques los README hasta que `AUDIT.md` diga que la fase pasa.
- **No afirmes nada que una captura commiteada no demuestre.** T4 queda NO VERIFICADA hasta la ejecución del auditor.

---

## 2. Qué puedes tocar y qué no

### 2.1 Lo único que puedes modificar

| Ruta | Propósito |
| --- | --- |
| `02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/Core/Src/main.c` | K1 |
| `02_firmware_projects/prj_03_adc_dma_eth/CM7/Core/Src/main.c` | K1 |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/CM7/Core/Src/main.c` | K1 |
| `tools/gpio_close_checks.py` (nuevo) | K4 |
| `01_shared_drivers/00_gpio/docs/gpio_close_*` | Tus capturas sin placa |
| `01_shared_drivers/00_gpio/IMPLEMENTATION_REPORT.md` | K5 |
| `01_shared_drivers/00_gpio/README.md` | Solo en el cierre (K3), cuando el auditor lo autorice |

Nada más: `git diff f066a8e --stat` lo demuestra.

### 2.2 Prohibido

| Archivo o acción | Razón |
| --- | --- |
| Cualquier cambio en `01_shared_drivers/00_gpio/*.c` o `*.h` | El driver no cambia (plan, K2) |
| Añadir funciones EXTI o de la celda a `gpio_ll` o `gpio_hal` | Decisión 1 del plan |
| Tocar en `SystemClock_Config()` algo distinto de las tres líneas finales y el comentario de `CSIState`: `HAL_RCC_OscConfig`, `HAL_RCC_ClockConfig`, PLL, buses, latencia de flash, `HAL_PWREx_ConfigSupply` | Un error ahí deja la placa sin arrancar. Si crees que hace falta, para y pregunta |
| Option bytes, scripts de enlazado, MPU, `stm32h7xx_it.c`, los CM4, `prj_01`, `prj_eth_lwip_node` | Fuera de alcance |
| Borrar o reescribir comentarios que no son de las líneas que cambias | En la fase de `08_eth` se borró un bloque de comentario sin pedirlo |
| Programar, resetear o leer la placa (`flash.py`, `STM32_Programmer_CLI`, `monitor.py`, `net_check.py` contra la placa) | **No tienes acceso a la placa.** T4 se prueba con salidas grabadas o simuladas |
| Retocar capturas commiteadas; mezclar codificaciones | Integridad de la evidencia |
| Merge, borrar ramas o documentos de proceso | Los hace el responsable |

---

## 3. Cómo trabajar

### 3.1 Rama y commits

- Rama **`driver/gpio-close`**, desde `main` en `f066a8e`. El plan y estas instrucciones ya están en ella.
- Un commit por objetivo, en inglés e imperativo, con el porqué en el cuerpo. Por ejemplo:
  - `feat(prj02,prj03,prj04): enable the I/O compensation cell through gpio_bm (K1)`
  - `feat(tools): add gpio_close_checks.py (K4)`
  - `test(gpio): record T1-T3 close runs (no board)`
  - `docs(gpio): add close implementation report`
- En PowerShell, cuidado con el BOM: escribe los mensajes en un archivo UTF-8 sin BOM y usa `git commit -F`.

### 3.2 Decisiones

Las aprueba el responsable antes de lanzar la fase; el mensaje de lanzamiento te dice cuáles. Aplícalas.

### 3.3 Entorno

- `.\tools\env.ps1` en cada consola nueva; la matriz, con `.\tools\build_all.ps1 -Clean`.
- `py`, nunca `python`; solo la biblioteca estándar.
- **El control `f066a8e`** se compila en un worktree aparte (`../stm32h755_control_f066a8e`) **dentro de cada ejecución** de la herramienta. Nunca reutilices binarios de una ejecución anterior.
- Tu árbol queda limpio. Si enlazas el submódulo de Cube en el worktree, dilo en el informe.

### 3.4 K1, en los tres `main.c`

Sigue el Diseño 1 del plan al pie de la letra. En especial:

- **Las tres copias de `CompensationCell_Init()` son idénticas**, texto incluido. T2 lo comprueba.
- **El orden de los pasos:**
  1. arrancar `DWT->CYCCNT` con la secuencia de `hsem_bm.c:95-101`;
  2. `__HAL_RCC_CSI_ENABLE()`;
  3. esperar `CSIRDY`, acotado por `DWT->CYCCNT`, con resta sin signo (`(now - start) < limit`);
  4. llamar al driver.

  Si falla el paso 3 o el 4, se llama a `Error_Handler()`.
- **La llamada** va en `main()`, entre `SystemClock_Config()` y `PeripheralClock_Config()`.
- **Las constantes** `COMP_CELL_CSI_MAX_CYCLES` (800 000) y `COMP_CELL_MAX_CYCLES` (2 000 000) son `#define` locales, con su cálculo en el comentario (ciclos a 400 MHz y el margen frente a los 76 493 medidos).
- **Los comentarios**, en inglés, en el estilo del archivo:
  - el bloque que hoy explica la celda pasa a la función;
  - el comentario nuevo de `osc.CSIState` dice que el campo no tiene efecto porque `OscillatorType` solo incluye HSE, y que CSI se enciende en `CompensationCell_Init()`;
  - otro comentario dice que el hook aún no está instalado y que antes de la liberación de CPU2 solo CPU1 configura (README §3.2).

### 3.5 La herramienta (K4)

`tools/gpio_close_checks.py`:

- **Argumentos:** `--test {t1,t2,t3,t4,all}` (por defecto `t1,t2,t3`) y `--output`.
- **Cabecera de la captura:** `argv`, `HEAD`, la ruta del worktree de control y el estado del árbol (limpio, incluidos los ignorados salvo `build/`) al empezar y al terminar.
- **Un `CHECK <nombre>: PASS|FAIL (<detalle>)` por comprobación**, y `OVERALL VERDICT:` al final. Un archivo, un preset o un objeto que falta es FAIL.
- **Controles negativos:** cada uno llama a **la misma función** que la comprobación real:
  - T1: un preset compilado en la ejecución con un aviso inyectado en una copia temporal de las flags;
  - T2: los objetos del control `f066a8e` por la función de símbolos, y una copia temporal de un objeto con una sección mutada por el comparador;
  - T3: el enlace sin la biblioteca `_bm`;
  - T4: la salida real del CLI con `READY` o `EN` cambiados en memoria.

  Una constante, un `assert` o un atajo que no pasa por esa función no es un control.
- **T2:** reutiliza el comparador de secciones ALLOC, contenido, relocalizaciones y símbolos de `tools/eth_e3e6_checks.py` (impórtalo o copia la función citando su origen). Si `nm` o `objdump` fallan, el resultado es FAIL.
- **T3:** el `.c` temporal se escribe fuera del repo. Las bibliotecas salen de `build.ninja`, no de una lista escrita a mano.
- **T4** (la ejecuta el auditor):
  - compila las imágenes en la ejecución y guarda el SHA-256 de cada ELF;
  - flashea con `sn=` y comprueba el "verify" del CLI;
  - cada lectura SWD es una sola llamada `HOTPLUG` con `sn=` (una sesión del CLI no conserva el halt entre llamadas);
  - ejecuta `net_check.py` y los 5 resets por NRST en `prj_04` `debug`;
  - sin placa, se prueba con una salida del CLI grabada o simulada, y la captura lo dice;
  - con `--test t4` y sin `--sn`, termina con FAIL sin tocar nada.

### 3.6 Lo que tienes que entregar (F1)

1. K1 y su compilación en todos los presets.
2. K4 con T1–T3 pasando y sus controles negativos fallando como deben.
3. T4 probada sin placa (sección 3.5).
4. La captura `01_shared_drivers/00_gpio/docs/gpio_close_t1t3_<hash>.txt`, commiteada.
5. El informe (K5), con los comandos exactos de T4 para el auditor.

---

## 4. El informe (`IMPLEMENTATION_REPORT.md`)

1. **Resumen:** commits, cambios y estado ("entregado para auditoría").
2. **K1:** cada paso del Diseño 1 con `archivo:línea` en los tres proyectos, y el diff de `SystemClock_Config()` (solo las tres líneas y el comentario).
3. **Resultados sin placa** (T1–T3): veredicto, control negativo y captura.
4. **T4:** qué hace y qué imprime, NO VERIFICADA, con los comandos exactos.
5. **Desviaciones** respecto al plan, con su motivo.

**Antes de entregar,** comprueba cada cita (`archivo:línea`, hash, cifra) contra `HEAD`.

---

## 5. Checklist de entrega

- [ ] `git status --porcelain --ignored`: vacío salvo `build/`.
- [ ] `git diff f066a8e -- 01_shared_drivers/00_gpio/` sin cambios en `*.c` ni `*.h`.
- [ ] `git diff f066a8e --stat`: solo las rutas de la sección 2.1.
- [ ] `build_all.ps1 -Clean` 8/8 sin avisos, y los presets de T1.
- [ ] Las tres `CompensationCell_Init()` son idénticas, y `SystemClock_Config()` solo pierde las tres líneas y gana el comentario.
- [ ] Cada control negativo pasa por la misma función que su comprobación y falla como debe.
- [ ] La **última línea** de `IMPLEMENTATION_REPORT.md` es exactamente `F1 ENTREGADA PARA AUDITORIA <hash del último commit de código o herramienta>`.
