# Instrucciones para el implementador de `03_tim` F4: TIM3 (y TIM7 de selftest) de `prj_03` y `prj_04` por el driver

Oct 13, 2026 · @Hector

Vas a implementar el plan [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 3) de esta carpeta: sustituir la programación manual y ad-hoc de TIM3 y TIM7 en los proyectos `prj_04_adc_dma_bm_eth` y `prj_03_adc_dma_eth` por los drivers compartidos `tim_bm` y `tim_hal`, y crear la herramienta de verificación `tools/tim_f4_checks.py`.

Ninguna firma pública en `01_shared_drivers/03_tim/*.h` cambia (API congelada). Otra persona auditará independientemente tu fase y repetirá las pruebas sobre la placa física antes del cierre.

Si alguna indicación de este documento entra en conflicto con el plan, prevalece el plan, excepto en las prohibiciones explícitas de la sección 2.2, que son absolutas.

---

## 1. Tu papel

**Tú implementas y pruebas. No auditas.**

- No califiques tu propio trabajo como "auditado", "aprobado" ni "cerrado"; el estado final de tu entrega es **"entregado para auditoría"**.
- No crees, edites ni borres el archivo `01_shared_drivers/03_tim/AUDIT.md`. Si no estás de acuerdo con una observación del auditor, argumenta técnicamente en tu informe.
- Si corriges un hallazgo de auditoría, no lo marques como cerrado: declara "corregido en `<hash>`, pendiente de verificación".
- No inicies las tareas de cierre documental en los README hasta que `AUDIT.md` dictamine que la fase F1 está aprobada.
- **No afirmes en el informe nada que una captura commiteada no demuestre de forma explícita.** Toda aseveración debe contar con su correspondiente respaldo en `docs/`.

---

## 2. Qué puedes tocar y qué no

### 2.1 Lo único que puedes modificar

| Ruta | Propósito |
| --- | --- |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/CM4/App/Src/adc_acquisition.c` | H1: migración de TIM3 a `tim_bm` en producción |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/CM4/CMakeLists.txt` | H1: enlazar `stm32h7_driver_tim_bm` |
| `02_firmware_projects/prj_03_adc_dma_eth/CM4/App/Src/adc_acquisition.c` | H2: migración de TIM3 a `tim_hal` en producción |
| `02_firmware_projects/prj_03_adc_dma_eth/CM4/CMakeLists.txt` | H2: enlazar `stm32h7_driver_tim_hal` |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/CM4/App/Src/adc_selftest_hw.c` | H3: migración de TIM3 a `tim_bm` |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/CM4/App/Src/dma_selftest.c` | H3: migración de TIM7 a `tim_bm` (T4) |
| `02_firmware_projects/prj_03_adc_dma_eth/CM4/App/Src/adc_selftest_hw.c` | H4: migración de TIM3 a `tim_bm` |
| `tools/tim_f4_checks.py` (nuevo) | H5: arnés de verificación automatizado para T1–T7 |
| `prj_04/docs/tim_f4_*`, `prj_03/docs/tim_f4_*` | Capturas de evidencias de las pruebas |
| `01_shared_drivers/03_tim/IMPLEMENTATION_REPORT.md` | Tu informe técnico de implementación |
| `01_shared_drivers/03_tim/README.md`, `prj_03/README.md`, `prj_04/README.md` | Solo en el cierre, cuando el auditor lo autorice |

Cualquier otra modificación fuera de esta lista está estrictamente prohibida (`git diff 897bf8d --stat`).

### 2.2 Prohibido

| Archivo o acción | Razón técnica |
| --- | --- |
| Cabeceras públicas de `01_shared_drivers/03_tim/*.h` | Contrato de catálogo (regla 2): API congelada. Si crees que falta una función o firma, para y consulta al responsable |
| `prj_04/CM7/Core/Src/stm32h7xx_hal_timebase_tim.c` | TIM6 actúa como timebase de FreeRTOS para la HAL en CM7. Está fuera de alcance del plan |
| `SystemClock_Config`, `HAL_PWREx_ConfigSupply`, option bytes | Regla G-S7: ante trabajo en remoto, cualquier fallo en la configuración de alimentación o reloj bloquea el procesador sin posibilidad de rescate por SWD |
| Programar, resetear o leer la placa (flash.py, STM32_Programmer_CLI, monitor.py, net_check.py, query_adc.py) | **No tienes acceso a la placa.** T3–T6 y T8 las ejecuta el auditor con tu herramienta. No escribas en tu informe ninguna medida de hardware como hecha por ti |
| Otros proyectos (`prj_01`, `prj_02`, `prj_eth_lwip_node`) | Fuera de alcance (`prj_01` solo se compila en preset `selftest-tim` para la comprobación T7) |
| Retocar o editar capturas commiteadas | Violación de integridad de evidencias |
| Modificar la configuración de red del adaptador del PC | Regla G-S4: la red está fijada por el responsable del banco |
| Mezclar codificaciones en las capturas (BOM / UTF-16) | Todas las capturas deben ser UTF-8 puro generado de una sola vez |
| Merge a `main`, borrado de ramas o borrado de documentos de proceso | Operaciones reservadas al responsable tras el cierre definitivo |

---

## 3. Cómo trabajar

### 3.1 Rama y commits

- Trabaja en la rama **`driver/tim-f4`**, al día con `main` en `897bf8d` (el plan, revisión 2, está en ella). El control es `04fb865`.
- Realiza commits atómicos por objetivo claro:
  - `refactor(prj04): migrate TIM3 acquisition to tim_bm (H1)`
  - `refactor(prj03): migrate TIM3 acquisition to tim_hal (H2)`
  - `refactor(prj04): migrate TIM3 and TIM7 selftests to tim_bm (H3)`
  - `refactor(prj03): migrate TIM3 selftest to tim_bm (H4)`
  - `feat(tools): add tim_f4_checks.py automated verification tool (H5)`
  - `test(tim): record T1, T2 and T7 runs (no board)`
  - `docs(tim): add implementation report for F4`
- Redacta mensajes de commit en inglés, modo imperativo, explicando el motivo técnico en el cuerpo.
- En Windows PowerShell, usa siempre archivos temporales o pipes limpios en UTF-8 para evitar caracteres BOM residuales.

### 3.2 Decisiones abiertas

Aplica estrictamente las **opciones por defecto** declaradas en el plan (Decisiones 1–4):
1. `tim_bm` en `prj_04`; `tim_hal` en `prj_03`.
2. Migrar tanto TIM3 de `adc_selftest_hw` como TIM7 de `dma_selftest` (opción A, aprobada: el auditor los ejecutará en la placa, T8).
3. Secuencia estricta de arranque (InitTimeBase con ARPE=0 antes de ConfigMaster; arranque con `TIM_BM_Start` tras armar ADC y DMA).
4. Borrado de documentos de proceso tras el cierre.

### 3.3 Entorno de trabajo

- Ejecuta `.\tools\env.ps1` en cada nueva sesión de consola para asegurar la presencia de las herramientas en el `PATH`.
- Compilación de la matriz global: `.\tools\build_all.ps1 -Clean`.
- Utiliza el lanzador `py`, nunca `python`. Solo bibliotecas estándar de Python.
- **La placa es del auditor.** La herramienta debe usar `sn=0016000C4142501220353451` en toda llamada a `STM32_Programmer_CLI`, `flash.py` y `monitor.py`, leer por SWD en caliente (`mode=HOTPLUG`), nunca leer `HSEM_RLR` ni registros con efectos al leer, y no resetear con `monitor.py`, `net_check.py` o `query_adc.py` corriendo. Tú no la ejecutas contra la placa.
- **Compilación de la imagen de control:** compila `04fb865` en un worktree independiente (`git worktree add ../stm32h755_control_04fb865 04fb865`), con todos los presets que usan T2 y T8, garantizando que tu árbol principal permanezca limpio. Si para el submódulo de Cube enlazas el del repo, dilo en el informe.

### 3.4 Capturas de evidencia

1. **Una sola ejecución por captura:** no unas fragmentos ni edites salidas de consola. Redirige la salida completa a `prj_04/docs/tim_f4_<prueba>_<hash>.txt` y `prj_03/docs/tim_f4_<prueba>_<hash>.txt`.
2. **Identidad de la imagen comprobada:** cada sesión de hardware inicia flasheando la ELF compilada del commit exacto y registrando la línea `Download verified successfully`.
3. **Impresión de orígenes:** toda dirección de registro o variable leída debe imprimirse junto a su origen (ej. `nm cm4_firmware.elf`, `stm32h755xx.h`).
4. **Validación de árbol limpio:** si `git status --porcelain` arroja diferencias, la prueba queda invalidada.

### 3.5 Metodología de pruebas

- **T1 (Builds):** matriz 8/8 (`build_all.ps1 -Clean`) y presets de selftest (`selftest` y `selftest-adc` en `prj_04`, `selftest` en `prj_03`, `selftest-tim` en `prj_01`), compilados de cero (`--fresh`), con los comandos en la salida. Cero errores y cero avisos.
- **T2 (Símbolos y estática):**
  - T2a: con `nm`, demostrar presencia de símbolos de `tim_bm` en CM4 de `prj_04`, y de `tim_hal` / `tim_bm` en CM4 de `prj_03`. El control negativo sobre `04fb865` debe fallar.
  - T2b: solo **escrituras** a registros de TIM3/TIM7 y habilitaciones de reloj a mano, sobre el código preprocesado, con la expresión y la autoprueba de `gpio_f1b_checks.py`. Las lecturas y `&TIM7->CNT` usado como dirección (`dma_selftest.c` l. 748, 770, 807) no cuentan. Ninguna llamada `HAL_TIM_*` en `prj_03` fuera de `tim_hal.c`. El control en `04fb865` las encuentra por (preset, ruta relativa).
- **T3 (Registros SWD en caliente):**
  - Leer en caliente `CR1` (`0x40000400`), `CR2` (`0x40000404`), `SMCR` (`0x40000408`), `DIER` (`0x4000040C`), `PSC` (`0x40000428`) y `ARR` (`0x4000042C`).
  - Comparar contra la tabla esperada de RM0399 (`CR1=0x0001`, `CR2=0x0020`, `SMCR=0`, `DIER=0`, `PSC=0`, `ARR=0x018F`).
  - Controles: la imagen de control `04fb865` debe dar los mismos seis valores (equivalencia), y la comparación contra una tabla alterada debe fallar.
- **T4 (Ritmo del ADC y OVR):**
  - Tasa = Δ`Samples`/Δt (tiempo del PC) en una ventana de al menos 10 s, no el `Measured Rate` de cada respuesta, que oscila. Ventana: 500 kHz × (1 + 1,327 % ± 0,2 %).
  - `OVR == 0` en todas las respuestas (en los dos proyectos es `half_overrun_count`); en `prj_04`, además `adc_ovr == 0` del JSON. `Samples` estrictamente creciente.
  - Control: la misma cuenta contra una ventana desplazada falla.
- **T5 (Regresión de red y monitor):**
  - Identidad: el flasheo verificado de la ELF del commit (no se exige leer el banner).
  - `net_check.py` pasa 4/4 pings y 100/100 UDP con 0 pérdidas y contadores `rx_nobuf=0`, `tx_nobuf=0`, `dma_err=0`.
- **T6a (20 arranques SWD):**
  - La herramienta ejecuta 20 ciclos de reset por SWD (`-rst`), esperando 10 s tras cada reset y validando T3 y T4. Veredicto obligatorio: 20/20 PASS.
- **T6b (Arranque en frío):**
  - Prueba de encendido físico con lectura de `RCC_RSR`. Queda pendiente si el responsable opera en remoto (no bloquea F1).
- **T7 (Preset selftest-tim de prj_01 y diff de cabeceras):**
  - Demostrar que `prj_01` preset `selftest-tim` compila intacto y que `git diff main -- '01_shared_drivers/03_tim/*.h'` no produce ninguna salida.
- **T8 (Selftests en la placa, la ejecuta el auditor):** la herramienta flashea y captura `prj_04` `selftest` y `selftest-adc` y `prj_03` `selftest`, migrados y de control (`04fb865`), y compara los veredictos: tienen que ser PASS e iguales antes y después.

### 3.6 Seguridad del banco (G-S1 a G-S7)

- No tocas la placa. La herramienta que escribes tiene que respetar G-S1 a G-S7 cuando la ejecute el auditor: solo los presets que lista G-S1, nunca alimentación, reloj base, option bytes ni borrado completo de la flash.

---

## 4. Orden de trabajo recomendado

1. **Creación del arnés de verificación (`tools/tim_f4_checks.py`):**
   - Implementa los chequeos estáticos T1, T2a, T2b y prepara los inspectores SWD para T3 y los oráculos de T4–T7.
   - Ejecuta la herramienta en modo control sobre `04fb865` para verificar que los controles negativos fallan según lo esperado.
2. **Implementación de `prj_04` (H1 y H3):**
   - Modifica `CM4/CMakeLists.txt` para enlazar `stm32h7_driver_tim_bm`.
   - Modifica `adc_acquisition.c` migrando `TIM3_Trigger_Init` y el arranque tardío a `tim_bm`.
   - Modifica `adc_selftest_hw.c` y `dma_selftest.c` (T4) migrando a `tim_bm`.
   - Compila y verifica con `build_all.ps1 -Clean`.
3. **Implementación de `prj_03` (H2 y H4):**
   - Modifica `CM4/CMakeLists.txt` para enlazar `stm32h7_driver_tim_hal`.
   - Modifica `adc_acquisition.c` migrando a `tim_hal`.
   - Modifica `adc_selftest_hw.c` migrando a `tim_bm`.
   - Compila y verifica con `build_all.ps1 -Clean`.
4. **Ejecución de pruebas en hardware:**
   - **No las ejecutes tú.** Deja la herramienta lista para que el auditor ejecute T3–T6 y T8 y di en el informe el comando exacto de cada una.
   - Ejecuta T1, T2 y T7 (sin placa) y commitea sus capturas.
   - Escribe en el informe la lista de pines de G-S1 comprobada contra el código de los presets que se flashearán.
5. **Redacción de `IMPLEMENTATION_REPORT.md`:**
   - Documenta los resultados obtenidos, los valores leídos en cada prueba, las tablas de registros y el estado de la placa al terminar.
6. **Entrega para auditoría:**
   - Commitea el informe en `driver/tim-f4` con el árbol limpio. La **última línea** de `01_shared_drivers/03_tim/IMPLEMENTATION_REPORT.md` debe ser exactamente `F1 ENTREGADA PARA AUDITORIA <hash del último commit de código o herramienta>`: es la marca que el auditor vigila.

---

## 5. El informe (`IMPLEMENTATION_REPORT.md`)

El informe debe estructurarse con las siguientes secciones:
1. **Resumen de la entrega:** hash del commit, lista de cambios realizados y estado ("entregado para auditoría").
2. **Resultados de las pruebas T1 a T7:** tablas con veredicto, valores medidos, control negativo y ruta de la captura asociada.
3. **Inspección de registros SWD:** tabla detallada de los 6 registros de TIM3 leídos en placa frente a los esperados de RM0399.
4. **Comandos de las pruebas de placa** (T3–T6, T8) para el auditor, y la lista de pines de G-S1.
5. **Lo no ejecutado:** todo lo que necesita la placa queda como NO VERIFICADO hasta la ejecución del auditor.
6. **Desviaciones o notas técnicas:** discrepancias encontradas respecto al plan original o peculiaridades de la ejecución.

---

## 6. Checklist de entrega

Antes de solicitar la auditoría, verifica que:
- [ ] El árbol de trabajo está completamente limpio (`git status --porcelain` vacío).
- [ ] Todos los presets de la matriz compilan sin errores ni avisos (8/8 en `build_all.ps1 -Clean`).
- [ ] Los presets `selftest` y `selftest-adc` (`prj_04`), `selftest` (`prj_03`) y `selftest-tim` (`prj_01`) compilan limpiamente.
- [ ] No se ha modificado ninguna cabecera en `01_shared_drivers/03_tim/*.h`.
- [ ] No se ha tocado `stm32h7xx_hal_timebase_tim.c` en `prj_04 CM7`.
- [ ] Todas las capturas de evidencias existen en `prj_04/docs/` y `prj_03/docs/`, en UTF-8 y sin edición manual.
- [ ] La herramienta empieza cada sesión de placa flasheando la ELF del commit y registrando `Download verified successfully`.
- [ ] Los controles negativos de T2a y T2b se ejecutaron y fallaron como deben; los de T3, T4 y T8 están implementados y probados sin placa donde se pueda.
- [ ] El informe no da por hecha ninguna prueba de placa.
- [ ] `IMPLEMENTATION_REPORT.md` está redactado y commiteado en la rama `driver/tim-f4`.
