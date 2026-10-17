# Instrucciones para el implementador de `04_uart`: auditoría, correcciones y validación

Oct 17, 2026 · @Hector

Vas a implementar el plan [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1) de esta carpeta:
- las correcciones U1–U6 del driver;
- el preset `selftest-uart` de `prj_01`, con los pasos UT0–UT10;
- el host `prj_01/tools/uart_hw_check.py` y su mock;
- la herramienta sin placa `tools/uart_checks.py`;
- el informe.

Otra persona auditará tu fase y ejecutará en la placa todo lo que necesita la placa.

Si algo de este documento choca con el plan, manda el plan, salvo las prohibiciones de la sección 2.2, que son absolutas.

---

## 1. Tu papel

**Tú implementas y pruebas sin placa. No auditas.**

- El estado final de tu entrega es "entregado para auditoría", nunca "auditado", "aprobado" ni "cerrado".
- No crees, edites ni borres `01_shared_drivers/04_uart/AUDIT.md`. Si no estás de acuerdo con una observación, argumenta en tu informe.
- Un hallazgo corregido se declara "corregido en `<hash>`, pendiente de verificación".
- No toques los README hasta que `AUDIT.md` diga que la fase pasa.
- **No afirmes nada que una captura commiteada no demuestre.** Todo lo que necesita la placa o el AD2 queda NO VERIFICADO hasta la ejecución del auditor.

---

## 2. Qué puedes tocar y qué no

### 2.1 Lo único que puedes modificar

| Ruta | Propósito |
| --- | --- |
| `01_shared_drivers/04_uart/uart_bm.c`, `uart_ll.c`, `uart_hal.c` | H1 (U1–U6) |
| `01_shared_drivers/04_uart/*.h` | Solo añadir, si una corrección lo necesita |
| `03_firmware_templates/prj_01_template_dualcore/`: el preset `selftest-uart` en `CMakePresets.json`, la opción `UART_DRIVER_SELFTEST` en los `CMakeLists.txt`, y los archivos nuevos `uart_selftest*` de `CM7/App`, `CM4/App` y `Common`, con su enganche en los `main.c` bajo la opción | H2 |
| `03_firmware_templates/prj_01_template_dualcore/tools/uart_hw_check.py` (nuevo) | H3 |
| `tools/ad2.py` | **Solo** la máscara y la función de SU1 (DIO15 como salida en los pasos de UT5), aditivas. El resto del archivo no cambia |
| `tools/uart_checks.py` (nuevo) | H4 |
| `01_shared_drivers/04_uart/docs/uart_*` y `03_firmware_templates/prj_01_template_dualcore/docs/uart_*` | Tus capturas sin placa |
| `01_shared_drivers/04_uart/IMPLEMENTATION_REPORT.md` | H5 |
| `01_shared_drivers/04_uart/README.md` y la sección de banco de `00_gpio/README.md` (§8) | Solo en el cierre, cuando el auditor lo autorice |

Nada más: `git diff b41c4a7 --stat` lo demuestra.

### 2.2 Prohibido

| Archivo o acción | Razón |
| --- | --- |
| Cambiar o borrar una declaración existente de `uart_*.h` | API congelada |
| `SystemClock_Config`, `HAL_PWREx_ConfigSupply`, option bytes, MPU, scripts de enlazado | Fuera de alcance |
| En `ad2.py`: las guardas S3, S4, S5 y S11, sus listas de DIO, o cualquier cosa fuera de SU1 | Seguridad eléctrica del banco |
| Hacer salida del AD2 cualquier DIO salvo DIO15, y DIO15 fuera de UT5 | SU1 |
| Que `bench_claim()` acepte PE0–PE4, PE7 o el bus fuera de sus pasos; configurar PD3–PD6 fuera de `selftest-uart` | S4, SU4 |
| Programar, resetear o leer la placa; abrir el AD2 (`flash.py`, `STM32_Programmer_CLI`, `monitor.py`, `uart_hw_check.py` o `run_warm_resets.py` contra el hardware) | **No tienes acceso a la placa ni al AD2.** El host se prueba con su mock |
| Escribir en `.git/config` o en la configuración de git, también desde un worktree (los worktrees comparten `.git/config`) | Pasó en el cierre de `00_gpio` |
| Contar avisos con la salida de `build_all.ps1` | No imprime la salida del compilador |
| Borrar o reescribir comentarios que no son de las líneas que cambias | Pasó en `08_eth` |
| Constantes de la placa escritas a mano en las herramientas (IP, SN, pines) sin citar su origen | Pasó en el cierre de `00_gpio` (`.50` por `.150`) |
| Retocar capturas commiteadas; mezclar codificaciones | Integridad de la evidencia |
| Merge, borrar ramas o documentos de proceso | Los hace el responsable |

---

## 3. Cómo trabajar

### 3.1 Rama y commits

- Rama **`driver/uart-audit`**, desde `main` en `b41c4a7`. El plan y estas instrucciones ya están en ella.
- Un commit por objetivo, en inglés e imperativo, con el porqué en el cuerpo. Por ejemplo:
  - `fix(uart): return the console MSP pin status from UART_HAL_Init (U1)`
  - `fix(uart): validate every LL constant before touching hardware (U2, U3)`
  - `fix(uart): reset CR1-CR3 on LL init (U4)`
  - `fix(uart): check the OVER8 divider the hardware applies in LL (U5)`
  - `fix(uart): report a busy GPIO lock from UART_HAL_InitEx (U6)`
  - `test(prj01): add the selftest-uart preset with UT0-UT10`
  - `test(prj01): add uart_hw_check.py and its mock`
  - `feat(tools): add uart_checks.py`
  - `docs(uart): add implementation report`
- En PowerShell, cuidado con el BOM: escribe los mensajes en un archivo UTF-8 sin BOM y usa `git commit -F`.

### 3.2 Decisiones

Las aprueba el responsable antes de lanzar la fase; el mensaje de lanzamiento te dice cuáles. Aplícalas.

### 3.3 Entorno

- `.\tools\env.ps1` en cada consola nueva.
- `py`, nunca `python`; solo la biblioteca estándar.
- El control `b41c4a7` se compila en un worktree aparte (`../stm32h755_control_b41c4a7`) **dentro de cada ejecución** de la herramienta que lo necesite. Si enlazas el submódulo de Cube, dilo en el informe.

### 3.4 El driver (H1)

Sigue la tabla de hallazgos del plan:

- **U1:** solo `UART_HAL_Init()` cambia de comportamiento; su firma, no.
- **U2/U3:** en LL, todo lo que puede rechazarse se rechaza antes de la primera escritura en GPIO, RCC o USART, como la fase 1 de BM. Cada constante se compara con su conjunto exacto de `LL_USART_*`.
- **U4:** antes de programar, `CR1`, `CR2` y `CR3` quedan en un estado conocido, con `UE` = 0.
- **U5:** el ±2 % se calcula sobre el divisor que aplica el hardware, como en `uart_bm.c:127-133`.
- **U6:** `GPIO_BM_ConfigPin`, con el estado traducido a `HAL_StatusTypeDef`, y los pines validados antes de encender el reloj.
- **Comentarios:** en inglés, en el estilo de cada archivo, con la cita de RM0399 o ES0445 de cada regla nueva.

### 3.5 El selftest (H2) y el host (H3)

- **El marco:** el mismo que `selftest-gpio`:
  - los pasos van en `k_steps[]`, con su espejo exacto en `STEPS` del host;
  - el handshake es `HOST_REQ`/`MCU_ACK`;
  - `bench_claim()` da los permisos por paso;
  - el formato de línea es `[uart] UTn ... -> PASS|FAIL`, y el veredicto, `FW VERDICT=`.
- **Cada paso deja todo como estaba:** USART2 deshabilitada, PD3–PD6 en entrada, IRQ y DMA apagados y sin pending, y el lock de GPIO libre.
- **El oráculo del cable (UT3–UT6):** el LA del AD2 en DIO15 y DIO14 (MCO1), con la frecuencia de muestreo y el número de muestras en la captura.
  - El ancho de bit se mide en periodos de MCO1 y se compara con el teórico del `BRR` y `PRESC` programados, que el firmware imprime.
  - El criterio es una tolerancia escrita en el código y en la captura, más estrecha que el ±2 % del driver, que es su motivo.
- **La inyección (UT5):**
  - DIO15 solo como salida dentro del paso, y por la función de SU1 de `ad2.py`;
  - antes de poner OE, el host comprueba que el firmware confirmó que PD5 está en reposo (SU2);
  - al salir del paso, DIO15 vuelve a entrada (SU3).
- **CM4 para U6 y U1 (UT7, UT9):** CM4 toma el HSEM del lock de GPIO por un comando del selftest. CM7 llama mientras CM4 lo tiene y lo vuelve a llamar cuando CM4 lo suelta.
- **UT9 sobre el control:** el host acepta `--expect-control-u1`. Así, el control `b41c4a7` da el FAIL esperado de U1 y lo marca como esperado.
- **El mock:** prueba cada paso, incluidos tres fallos que el host tiene que detectar:
  - el firmware dice PASS y el LA ve una paridad mala;
  - DIO15 en OE fuera de UT5;
  - una cuenta de tramas de menos.

### 3.6 La herramienta sin placa (H4)

`tools/uart_checks.py` hace T1 y T2 del plan:
- **Argumentos:** `--test {t1,t2,all}` y `--output`.
- **Cabecera de la captura:** `argv`, `HEAD` y el árbol limpio al empezar y al terminar.
- **Una línea `CHECK <nombre>: PASS|FAIL (<detalle>)` por comprobación**, y `OVERALL VERDICT:` al final.
- **Controles negativos:** cada uno llama a la misma función que la comprobación real, con la entrada mutada en una copia temporal fuera del repo. Una constante, un `assert` o un atajo que no pasa por esa función no es un control.

---

## 4. El informe (`IMPLEMENTATION_REPORT.md`)

1. **Resumen:** commits, cambios y estado ("entregado para auditoría").
2. **U1–U6:** cada corrección con `archivo:línea`, antes y después.
3. **UT0–UT10:** qué hace cada paso, qué imprime y qué comprueba el host. Todos NO VERIFICADOS.
4. **Resultados sin placa** (T1, T2 y la prueba del host con el mock): veredicto, control negativo y captura.
5. **Los comandos del banco para el auditor:**
   - compilar y flashear `selftest-uart`;
   - `uart_hw_check.py` dos veces;
   - UT9 sobre el control;
   - `run_warm_resets.py`;
   - UT10 en `prj_04`.
6. **Lo que el responsable tiene que preparar:**
   - JU1 con 1 kΩ, JU2 y DIO15 en PD6;
   - quitar el cable PD5–PD6 directo si sigue;
   - J2/J3 fuera y el USB de la F334 desconectado (`00_gpio/README.md` §8);
   - el AD2 con GND primero.
7. **Desviaciones** respecto al plan, con su motivo.

**Antes de entregar,** comprueba cada cita (`archivo:línea`, hash, cifra) contra `HEAD`.

---

## 5. Checklist de entrega

- [ ] `git status --porcelain --ignored`: vacío salvo `build/`, y `git config --get-regexp submodule` sin entradas nuevas.
- [ ] `git diff b41c4a7 --stat`: solo las rutas de la sección 2.1.
- [ ] `git diff b41c4a7 -- '01_shared_drivers/04_uart/*.h'`: solo añade.
- [ ] T1 y T2 pasan, con los avisos contados de la salida de `cmake --build`.
- [ ] En `ad2.py`, solo la máscara y la función de SU1.
- [ ] Cada control negativo pasa por la misma función que su comprobación y falla como debe; el mock detecta los tres fallos de la sección 3.5.
- [ ] La **última línea** de `IMPLEMENTATION_REPORT.md` es exactamente `F1 ENTREGADA PARA AUDITORIA <hash del último commit de código o herramienta>`.
