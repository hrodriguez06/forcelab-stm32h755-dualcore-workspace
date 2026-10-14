# Instrucciones para el implementador de `03_tim` F4: TIM3 (y TIM7 de selftest) de `prj_03` y `prj_04` por el driver

Oct 13, 2026 · @Hector

Vas a implementar el plan [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1) de esta carpeta: sustituir la programación manual y ad-hoc de TIM3 y TIM7 en los proyectos `prj_04_adc_dma_bm_eth` y `prj_03_adc_dma_eth` por los drivers compartidos `tim_bm` y `tim_hal`, y crear la herramienta de verificación `tools/tim_f4_checks.py`.

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

Cualquier otra modificación fuera de esta lista está estrictamente prohibida (`git diff 04fb865 --stat`).

### 2.2 Prohibido

| Archivo o acción | Razón técnica |
| --- | --- |
| Cabeceras públicas de `01_shared_drivers/03_tim/*.h` | Contrato de catálogo (regla 2): API congelada. Si crees que falta una función o firma, para y consulta al responsable |
| `prj_04/CM7/Core/Src/stm32h7xx_hal_timebase_tim.c` | TIM6 actúa como timebase de FreeRTOS para la HAL en CM7. Está fuera de alcance del plan |
| `SystemClock_Config`, `HAL_PWREx_ConfigSupply`, option bytes | Regla G-S7: ante trabajo en remoto, cualquier fallo en la configuración de alimentación o reloj bloquea el procesador sin posibilidad de rescate por SWD |
| Flashear presets de selftest (`selftest`, `selftest-adc`, etc.) | Regla G-S1: los selftests conmutan pines y pueden interferir con el cableado del banco analógico. Solo se compilan en host |
| Otros proyectos (`prj_01`, `prj_02`, `prj_eth_lwip_node`) | Fuera de alcance (`prj_01` solo se compila en preset `selftest-tim` para la comprobación T7) |
| Retocar o editar capturas commiteadas | Violación de integridad de evidencias |
| Modificar la configuración de red del adaptador del PC | Regla G-S4: la red está fijada por el responsable del banco |
| Mezclar codificaciones en las capturas (BOM / UTF-16) | Todas las capturas deben ser UTF-8 puro generado de una sola vez |
| Merge a `main`, borrado de ramas o borrado de documentos de proceso | Operaciones reservadas al responsable tras el cierre definitivo |

---

## 3. Cómo trabajar

### 3.1 Rama y commits

- Trabaja en la rama **`driver/tim-f4`**, creada a partir de `04fb865` (`main`).
- Realiza commits atómicos por objetivo claro:
  - `refactor(prj04): migrate TIM3 acquisition to tim_bm (H1)`
  - `refactor(prj03): migrate TIM3 acquisition to tim_hal (H2)`
  - `refactor(prj04): migrate TIM3 and TIM7 selftests to tim_bm (H3)`
  - `refactor(prj03): migrate TIM3 selftest to tim_bm (H4)`
  - `feat(tools): add tim_f4_checks.py automated verification tool (H5)`
  - `test(tim): record T1-T7 test runs on prj_03 and prj_04`
  - `docs(tim): add implementation report for F4`
- Redacta mensajes de commit en inglés, modo imperativo, explicando el motivo técnico en el cuerpo.
- En Windows PowerShell, usa siempre archivos temporales o pipes limpios en UTF-8 para evitar caracteres BOM residuales.

### 3.2 Decisiones abiertas

Aplica estrictamente las **opciones por defecto** declaradas en el plan (Decisiones 1–4):
1. `tim_bm` en `prj_04`; `tim_hal` en `prj_03`.
2. Migrar tanto TIM3 de `adc_selftest_hw` como TIM7 de `dma_selftest`.
3. Secuencia estricta de arranque (InitTimeBase con ARPE=0 antes de ConfigMaster; arranque con `TIM_BM_Start` tras armar ADC y DMA).
4. Borrado de documentos de proceso tras el cierre.

### 3.3 Entorno de trabajo

- Ejecuta `.\tools\env.ps1` en cada nueva sesión de consola para asegurar la presencia de las herramientas en el `PATH`.
- Compilación de la matriz global: `.\tools\build_all.ps1 -Clean`.
- Utiliza el lanzador `py`, nunca `python`. Solo bibliotecas estándar de Python.
- **Sonda ST-LINK:** `sn=0016000C4142501220353451` en toda invocación a `STM32_Programmer_CLI`, `flash.py` y `monitor.py`.
- **Inspección SWD:** en caliente (`mode=HOTPLUG`) sin detener los núcleos, empleando `STM32_Programmer_CLI -c port=SWD sn=0016000C4142501220353451 mode=HOTPLUG -r32 <addr> <bytes>`.
- **Compilación de la imagen de control:** compila la versión base `04fb865` en un worktree independiente (`git worktree add ../stm32h755_control_04fb865 04fb865`), garantizando que tu árbol principal permanezca limpio.

### 3.4 Capturas de evidencia

1. **Una sola ejecución por captura:** no unas fragmentos ni edites salidas de consola. Redirige la salida completa a `prj_04/docs/tim_f4_<prueba>_<hash>.txt` y `prj_03/docs/tim_f4_<prueba>_<hash>.txt`.
2. **Identidad de la imagen comprobada:** cada sesión de hardware inicia flasheando la ELF compilada del commit exacto y registrando la línea `Download verified successfully`.
3. **Impresión de orígenes:** toda dirección de registro o variable leída debe imprimirse junto a su origen (ej. `nm cm4_firmware.elf`, `stm32h755xx.h`).
4. **Validación de árbol limpio:** si `git status --porcelain` arroja diferencias, la prueba queda invalidada.

### 3.5 Metodología de pruebas

- **T1 (Builds):** matriz 8/8 (`build_all.ps1 -Clean`) y presets de selftest (`selftest-adc` en `prj_04`, `selftest` en `prj_03`, `selftest-tim` en `prj_01`). Cero errores y cero avisos.
- **T2 (Símbolos y estática):**
  - T2a: con `nm`, demostrar presencia de símbolos de `tim_bm` en CM4 de `prj_04`, y de `tim_hal` / `tim_bm` en CM4 de `prj_03`. El control negativo sobre `04fb865` debe fallar.
  - T2b: verificar con regex/grep que no queden accesos `TIM3->` ni `TIM7->` en los archivos migrados. El control negativo sobre `04fb865` debe detectar las escrituras preexistentes.
- **T3 (Registros SWD en caliente):**
  - Leer en caliente `CR1` (`0x40000400`), `CR2` (`0x40000404`), `SMCR` (`0x40000408`), `DIER` (`0x4000040C`), `PSC` (`0x40000428`) y `ARR` (`0x4000042C`).
  - Comparar contra la tabla esperada de RM0399 (`CR1=0x0001`, `CR2=0x0020`, `SMCR=0`, `DIER=0`, `PSC=0`, `ARR=0x018F`).
  - El control negativo debe comparar contra una tabla intencionadamente alterada y fallar.
- **T4 (Ritmo del ADC y OVR):**
  - Ejecutar `query_adc.py --json --count 20` para `prj_04` y `prj_03`.
  - Comprobar que `Measured Rate` se sitúa estrictamente en el intervalo `[502.000, 509.000] Hz` (reloj ST-LINK con +1,327 %).
  - Comprobar que `OVR == 0` en el 100 % de los paquetes y que `Samples` crece continuamente.
- **T5 (Regresión de red y monitor):**
  - `monitor.py --sn` muestra banner con commit y build id limpio.
  - `net_check.py` pasa 4/4 pings y 100/100 UDP con 0 pérdidas y contadores `rx_nobuf=0`, `tx_nobuf=0`, `dma_err=0`.
- **T6a (20 arranques SWD):**
  - La herramienta ejecuta 20 ciclos de reset por SWD (`-rst`), esperando 10 s tras cada reset y validando T3 y T4. Veredicto obligatorio: 20/20 PASS.
- **T6b (Arranque en frío):**
  - Prueba de encendido físico con lectura de `RCC_RSR`. Queda pendiente si el responsable opera en remoto (no bloquea F1).
- **T7 (Preset selftest-tim de prj_01 y diff de cabeceras):**
  - Demostrar que `prj_01` preset `selftest-tim` compila intacto y que `git diff main -- '01_shared_drivers/03_tim/*.h'` no produce ninguna salida.

### 3.6 Seguridad del banco (G-S1 a G-S7)

- Respeta rigurosamente las reglas del banco del plan. Nunca intentes flashear presets de selftest ni modificar registros de bajo nivel del sistema de alimentación o reloj base. Si la placa deja de responder, avisa de inmediato.

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
   - Flashea `prj_04` debug, ejecuta T3, T4, T5 y T6a guardando las capturas UTF-8 en `prj_04/docs/`.
   - Flashea `prj_04` release, ejecuta T4 y T5 guardando las capturas.
   - Flashea `prj_03` debug, ejecuta T3, T4, T5 y T6a guardando las capturas en `prj_03/docs/`.
   - Flashea `prj_03` release, ejecuta T4 y T5 guardando las capturas.
   - Ejecuta T7 (build de `prj_01` `selftest-tim` y diff de cabeceras).
5. **Redacción de `IMPLEMENTATION_REPORT.md`:**
   - Documenta los resultados obtenidos, los valores leídos en cada prueba, las tablas de registros y el estado de la placa al terminar.
6. **Entrega para auditoría:**
   - Notifica la finalización de F1 para la revisión independiente del auditor.

---

## 5. El informe (`IMPLEMENTATION_REPORT.md`)

El informe debe estructurarse con las siguientes secciones:
1. **Resumen de la entrega:** hash del commit, lista de cambios realizados y estado ("entregado para auditoría").
2. **Resultados de las pruebas T1 a T7:** tablas con veredicto, valores medidos, control negativo y ruta de la captura asociada.
3. **Inspección de registros SWD:** tabla detallada de los 6 registros de TIM3 leídos en placa frente a los esperados de RM0399.
4. **Medición de frecuencia del ADC:** tabla con el `Measured Rate` obtenido, `OVR` y comparación con el valor teórico con offset (+1,327 %).
5. **Estado de la placa al término de la sesión:** imagen y preset que quedan flasheados en la memoria Flash.
6. **Desviaciones o notas técnicas:** discrepancias encontradas respecto al plan original o peculiaridades de la ejecución.

---

## 6. Checklist de entrega

Antes de solicitar la auditoría, verifica que:
- [ ] El árbol de trabajo está completamente limpio (`git status --porcelain` vacío).
- [ ] Todos los presets de la matriz compilan sin errores ni avisos (8/8 en `build_all.ps1 -Clean`).
- [ ] Los presets `selftest-adc` (`prj_04`), `selftest` (`prj_03`) y `selftest-tim` (`prj_01`) compilan limpiamente.
- [ ] No se ha modificado ninguna cabecera en `01_shared_drivers/03_tim/*.h`.
- [ ] No se ha tocado `stm32h7xx_hal_timebase_tim.c` en `prj_04 CM7`.
- [ ] Todas las capturas de evidencias existen en `prj_04/docs/` y `prj_03/docs/`, en UTF-8 y sin edición manual.
- [ ] Cada sesión de hardware en las capturas inicia con la línea de verificación de descarga (`Download verified successfully`).
- [ ] Los controles negativos de T2a, T2b, T3 y T4 han sido ejecutados y han fallado con datos no conformes.
- [ ] Las 20 iteraciones de reset en caliente (T6a) han completado con éxito (20/20 PASS).
- [ ] `IMPLEMENTATION_REPORT.md` está redactado y commiteado en la rama `driver/tim-f4`.
