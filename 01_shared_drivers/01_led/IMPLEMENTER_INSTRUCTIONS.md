# Instrucciones para el implementador de `01_led`: cierre de L3, L4 y L5

Oct 17, 2026 · @Hector

Vas a implementar el plan [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1) de esta carpeta:
- H1: L3, L4 y L5 en `led_bm.c`, más `LED_Init_ConfigEx`;
- H2: el preset `selftest-led` de `prj_01` con LT1–LT5;
- H3: el host `prj_01/tools/led_hw_check.py` y su mock;
- H4: los cambios aditivos de `tools/led_l1l2_checks.py`;
- H5: el informe.

Otra persona auditará tu fase y repetirá en la placa todo lo que la necesita.

Si algo de este documento choca con el plan, manda el plan, salvo las prohibiciones de la sección 2.2, que son absolutas.

---

## 1. Tu papel

**Tú implementas y pruebas, también en la placa. No auditas.**

- El estado final de tu entrega es "entregado para auditoría", nunca "auditado", "aprobado" ni "cerrado".
- No crees, edites ni borres `01_shared_drivers/01_led/AUDIT.md`. Si no estás de acuerdo con una observación, argumenta en tu informe.
- Un hallazgo corregido se declara "corregido en `<hash>`, pendiente de verificación".
- No toques el README de `01_led` ni la hoja de ruta: los cierra el responsable.
- **No afirmes nada que una captura commiteada no demuestre.** Tus cifras de placa quedan NO VERIFICADO hasta la ejecución del auditor.

---

## 2. Qué puedes tocar y qué no

### 2.1 Lo único que puedes modificar

| Ruta | Propósito |
| --- | --- |
| `01_shared_drivers/01_led/led_bm.c` | H1 |
| `01_shared_drivers/01_led/led_bm.h` | **Solo añadir** `LED_Init_ConfigEx` y su comentario |
| `03_firmware_templates/prj_01_template_dualcore/`: el preset `selftest-led` en `CMakePresets.json`, la opción `LED_DRIVER_SELFTEST` en los `CMakeLists.txt`, los archivos nuevos `CM7/App/Src/led_selftest.c` y su cabecera, y su enganche en `CM7/Core/Src/main.c` bajo la opción | H2 |
| `03_firmware_templates/prj_01_template_dualcore/tools/led_hw_check.py` (nuevo) | H3 |
| `tools/led_l1l2_checks.py` | H4, **solo aditivo**: `selftest-led` en `SELFTEST_PRESETS` y los registros nuevos en T4. Lo demás no cambia |
| `01_shared_drivers/01_led/docs/led_*` (nuevos) y `03_firmware_templates/prj_01_template_dualcore/docs/led_*` (nuevos) | Tus capturas |
| `01_shared_drivers/01_led/IMPLEMENTATION_REPORT.md` | H5 |

Nada más: `git diff 0927c75 --stat` lo demuestra.

### 2.2 Prohibido

| Archivo o acción | Razón |
| --- | --- |
| Cambiar o borrar una declaración existente de `led_bm.h`, o la tabla `g_led_config` | API congelada; T3 la compara con UM2408 |
| Cambiar `LED_PanicRed()` para que use la sección o el lock | Excepción G14: el camino de fallo no puede esperar |
| Tocar `00_gpio` o cualquier otro driver | Fuera de alcance |
| Que el selftest configure pines distintos de PB0, PE1 y PB14 (leer registros de GPIOA/B/E sí) | Banco: el AD2 sigue conectado a PD6, PA8, PE2, PE7, PF14 y PF15 |
| Abrir el AD2 o la interfaz de WaveForms | No hace falta en esta fase |
| `SystemClock_Config`, `HAL_PWREx_ConfigSupply`, option bytes, MPU, scripts de enlazado | Fuera de alcance |
| Escribir registros por SWD fuera del flasheo normal; option bytes; borrado completo; actualizar el firmware del ST-LINK; usar el ST-LINK de la F334 | **Desde el 2026-10-17 puedes usar la placa en esta fase** (decisión del responsable):<br>• flashear imágenes del repo con el `tools/flash.py` de cada proyecto y `--sn 0016000C4142501220353451`;<br>• ejecutar `led_hw_check.py`, `led_l1l2_checks.py --test t4`, `monitor.py` y `tools/run_warm_resets.py`;<br>• leer por SWD y resetear por NRST, siempre con `sn=`.<br>Lo de esta fila sigue prohibido. Si la placa deja de responder, para y repórtalo. Tus capturas de placa llevan el prefijo `led_dev_`. Al terminar, la placa queda con `prj_01` `debug` |
| Escribir en `.git/config` o en la configuración de git, también desde un worktree (los worktrees comparten `.git/config`) | Pasó en el cierre de `00_gpio` |
| Contar avisos con la salida de `build_all.ps1` | No imprime la salida del compilador |
| Borrar o reescribir comentarios que no son de las líneas que cambias | Pasó en `08_eth` |
| Constantes de la placa escritas a mano en las herramientas (SN, puerto, pines) sin citar su origen | Pasó en el cierre de `00_gpio` y en `04_uart` |
| Retocar capturas commiteadas; mezclar codificaciones | Integridad de la evidencia |
| Merge, borrar ramas o documentos de proceso | Los hace el responsable |

---

## 3. Cómo trabajar

### 3.1 Rama y commits

- Rama **`driver/led-close`**, desde `main` en `0927c75`. El plan y estas instrucciones ya están en ella.
- Un commit por objetivo, en inglés e imperativo, con el porqué en el cuerpo. Por ejemplo:
  - `fix(led): configure LEDs through GPIO_BM_ConfigOutput (L3, L5)`
  - `feat(led): add LED_Init_ConfigEx returning the configured mask (L5)`
  - `fix(led): make LED_Toggle atomic on the calling core (L4)`
  - `test(prj01): add the selftest-led preset with LT1-LT5`
  - `test(prj01): add led_hw_check.py and its mock`
  - `test(tools): add selftest-led and the new T4 registers to led_l1l2_checks`
  - `docs(led): add implementation report`
- En PowerShell, cuidado con el BOM: escribe los mensajes en un archivo UTF-8 sin BOM y usa `git commit -F`.

### 3.2 Entorno

- `.\tools\env.ps1` en cada consola nueva.
- `py`, nunca `python`; solo la biblioteca estándar.
- Un worktree de control (por ejemplo, para T3 con `9dc6896`) va en una ruta **corta** junto al repo (`..\wt_led`), nunca en un directorio temporal: con una ruta larga falla el `try_compile` de CMake. Pásale `-DCUBE_DIR=<checkout principal>\02_firmware_projects\prj_eth_lwip_node\external\STM32CubeH7` y bórralo al terminar.

### 3.3 El driver (H1)

- **Sigue la sección 2 del plan.** `LED_Init_ConfigEx` copia la tabla, configura cada LED con `GPIO_BM_ConfigOutput` y pone `port = NULL` en la copia de los que no devuelven `GPIO_BM_OK`.
- **`LED_Toggle`:** guarda `PRIMASK`, `__disable_irq()`, lee `ODR`, escribe `BSRR` y restaura `PRIMASK` (`__set_PRIMASK(saved)`), igual que `TakeErrors` de `04_uart`.
- **Comentarios:** en inglés, en el estilo del archivo; cita `00_gpio/README.md` §3.3 para la sección y G14 para `LED_PanicRed`.

### 3.4 El selftest (H2) y el host (H3)

- **Sigue las secciones 4 y 5 del plan.** El selftest corre en CM7, tras el `LED_Init()` del arranque, y no usa el AD2.
- **LT3:** el control es una copia local, en `led_selftest.c`, de la `LED_Toggle` de `0927c75`, sin cambios salvo el nombre. Imprime N, la frecuencia de la ISR, las interrupciones que cayeron dentro de la ventana y `lost` del real y del control. Si el control da `lost = 0`, el paso falla: sube N o la frecuencia hasta que el control pierda cientos en cada ejecución, y di en el informe qué elegiste.
- **TIM17:** como en `gpio_selftest.c`. Al acabar, parado, sin `UIE`, sin pending en el NVIC y con el reloj como estaba.
- **El host:** una función `eval_ltN` por paso; los controles y las mutaciones de los datos medidos pasan por esa misma función. Una constante, un `assert` o un atajo que no pase por ella no es un control.

---

## 4. El informe (`IMPLEMENTATION_REPORT.md`)

1. **Resumen:** commits, cambios y estado ("entregado para auditoría").
2. **L3, L4 y L5:** cada cambio con `archivo:línea`, antes y después.
3. **LT1–LT5:** qué hace cada paso, qué imprime, qué comprueba el host y qué hace fallar su control.
4. **Resultados sin placa** (T1–T3 y el mock): veredicto y captura.
5. **Resultados en la placa** (tus dos ejecuciones de `led_hw_check.py` y T4): captura, con NO VERIFICADO.
6. **Los comandos para el auditor:** configurar, compilar y flashear `selftest-led`; `led_hw_check.py`; `led_l1l2_checks.py --test t4`; y restaurar `prj_01` `debug`.
7. **Desviaciones** respecto al plan, con su motivo.

**Antes de entregar,** comprueba cada cita (`archivo:línea`, hash, cifra) contra `HEAD`.

---

## 5. Checklist de entrega

- [ ] `git status --porcelain --ignored`: vacío salvo `build/`, y `git config --get-regexp submodule` sin entradas nuevas.
- [ ] `git diff 0927c75 --stat`: solo las rutas de la sección 2.1.
- [ ] `git diff 0927c75 -- 01_shared_drivers/01_led/led_bm.h`: solo añade.
- [ ] T1–T3 pasan, con los avisos contados de la salida de `cmake --build`.
- [ ] **En el commit entregado:** dos capturas `led_dev_hw_<hash>_run1/2.txt` con `FW VERDICT=PASS` y `HOST VERDICT=PASS`, y una de T4. **Sin ellas no se audita.**
- [ ] Cada control pasa por la misma función que su comprobación y falla como debe; el mock detecta sus tres fallos.
- [ ] La placa queda con `prj_01` `debug`.
- [ ] La **última línea** de `IMPLEMENTATION_REPORT.md` es exactamente `CIERRE L3-L5 ENTREGADO PARA AUDITORIA <hash del último commit>`.
