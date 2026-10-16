# Informe de Implementación: Fase 00_gpio F2 (Cuarta Corrección de Auditoría)

**Fecha:** 2026-10-16  
**Rama:** `driver/gpio-f2`  
**Estado:** F1 ENTREGADA PARA AUDITORÍA  
**Último commit de código o herramienta:** `b1b6342`  
**Commit de capturas de verificación:** `ddca912`  

---

## 1. Resumen de la entrega corregida

El presente informe documenta la resolución integral, rigurosa y verificada de todos los hallazgos emitidos en la cuarta ronda de auditoría de la fase 00_gpio F2 (`AUDIT.md`, sección *F1, corrección 3 (36aab61)*), cerrando todos los aspectos pendientes:
1. **Procedencia estricta de las ELFs de control (R5):** La función `verify_elf_embedded_revision` comprueba la presencia del commit esperado `3df160a` en las secciones de código/datos/rodata fuera de cualquier ruta del sistema de archivos (`stm32h755_control_3df160a`), garantizando que las imágenes ELF provienen genuinamente de la compilación de `3df160a`. Se incluye un control negativo en `main()` que muta el hash a `deadbeef` y comprueba que cualquier binario foráneo es rechazado.
2. **Robustez del verificador cruzado AST host-firmware (N1):** La función `extract_host_expect_patterns` extrae la lista de `TIERS` dinámicamente desde el AST de `gpio_hw_check.py` (eliminando listas cableadas a mano). Cualquier marcador de formato que no pueda ser expandido (e.g. `{self.label}`) es clasificado como no expandible y provoca un veredicto de `FAIL` en `match_expect_patterns`. Se incluye un control negativo específico (control 4) que verifica el rechazo de marcadores desconocidos.
3. **Precisión en citas y afirmaciones del informe (R4):** Se corrigieron todas las citas de línea desfasadas en el informe (líneas 51, 57, 59, 73, 74 y 88) referenciando con total exactitud las funciones reales (por ejemplo `pulse_train_measured` en `gpio_hw_check.py:907-987`, `FDwfGetLastErrorMsg` en `gpio_hw_check.py:315`, `find_tools` en `gpio_f2_checks.py:46-49`, `stamp()` en `gpio_hw_check.py:1611`, las comprobaciones de autorización en `CM7 gpio_selftest.c:1636` y `CM4 gpio_selftest.c:146-151`, y el mantenimiento de NVIC en CM7 y CM4). Se eliminaron las afirmaciones imprecisas y se añadieron en §6.2 los pasos explícitos de compilación CMake (`cmake --fresh` y `cmake --build --clean-first`) previos al flasheo para asegurar que `Rev` en placa coincida exactamente con HEAD.
4. **Hallazgos menores de la herramienta de comprobación:**
   - Envoltura global de `main()` en `tools/gpio_f2_checks.py` mediante bloque `try ... except Exception as e:` que registra limpiamente el error y sale con código 1 sin propagar trazas no controladas.
   - Verificación explícita del código de salida de `nm` (`code_nm == 0`) al analizar la imagen `dummy_elf` de control en `check_no_handlers`.
   - Listado de cabeceras en `check_headers` obtenido directamente del árbol git de control `3df160a` (`git ls-tree`), previniendo la omisión inadvertida de cabeceras eliminadas.
   - Eliminación de comentarios C mediante `strip_c_comments` previa a la evaluación de líneas de código en `is_c_code_line`, garantizando que sentencias válidas precedidas de asteriscos (como `*ptr = 0;` o `*/ int x;`) no sean clasificadas como comentarios.
   - Comparación de tamaños y tipos de símbolos (`T` vs `W`) en `compare_symbols`, acompañada de su correspondiente control negativo.

Todas las pruebas sin placa (T1 de compilación con matriz limpia 8/8 y selftests; T18 de regresión contra `3df160a`; y prueba del host con el mock DWF en 4 fases) fueron ejecutadas satisfactoriamente con veredicto favorable (`PASS`), respaldadas íntegramente por las capturas commiteadas en `03_firmware_templates/prj_01_template_dualcore/docs/` (`gpio_f2_checks_b1b6342.txt` y `gpio_f2_mock_b1b6342.txt`).

Las pruebas de placa (T11–T14, T17 y warm resets) no son ejecutadas por el implementador debido a la prohibición estricta de acceso al hardware físico y al instrumental AD2. Se declaran formalmente como **NO VERIFICADAS por el implementador**, citando los resultados certificados por el auditor en sus corridas previas en silicio (`gpio_f2_audit_bench_6b7f10a.txt` y `gpio_f2_audit_bench_c90f2b5.txt`).

### Historial completo de commits en la rama `driver/gpio-f2`
1. `628eea6` — `feat(gpio): add EXTI 0-15 and compensation cell to gpio_bm (H1)`
2. `25fbe4f` — `feat(tools): add gpio_f2_checks.py (H4)`
3. `0a025c9` — `test(prj01): add F2 EXTI and compensation-cell steps to selftest-gpio (H2)`
4. `ed4f31c` — `test(prj01): drive and measure the F2 steps in gpio_hw_check.py (H3)`
5. `55e9119` — `test(gpio): record T1 build, T18, and mock runs (no board)`
6. `cbd68a9` — `docs(gpio): add F2 F1 implementation report (H5)`
7. `696b5be` — `docs(gpio): audit F2 F1 at cbd68a9 and revise the plan (revision 2)`
8. `3497df9` — `docs(gpio): point the F2 implementer instructions to plan revision 2`
9. `31c5187` — `fix(gpio): correct EXTI read-back, hold time, and headers in gpio_bm (H1)`
10. `7fb71ca` — `fix(prj01): correct selftest negative controls, authorization, and compensation cell (H2)`
11. `493a152` — `fix(prj01): add LA edge counting, T13 latency measurement, and mock fixes (H3)`
12. `bc7e64d` — `fix(tools): overhaul gpio_f2_checks.py with pure functions and clean checks (H4)`
13. `25add1f` — `test(gpio): record T1 build, T18 regression, and mock test captures for bc7e64d`
14. `51bf03e` — `docs(gpio): update F2 implementation report for audit corrections`
15. `6b7f10a` — `docs(gpio): finalize report marker as F1 CORREGIDA PARA AUDITORIA bc7e64d`
16. `755614a` — `docs(gpio): audit the F2 correction at 6b7f10a; board run 63/63`
17. `a5127df` — `fix(gpio): resolve audit 2 findings R1-R3, R5, N1-N9 across driver, selftest, and tools`
18. `667c206` — `test(gpio): record T1 build, T18 regression, and mock test captures for a5127df`
19. `c90f2b5` — `docs(gpio): update implementation report for audit round 2 corrections`
20. `379cc3b` — `docs(gpio): audit the second F2 correction at c90f2b5`
21. `fdf7200` — `fix(gpio): resolve audit 3 findings R1, R2, R3, R5, N1, N4, N5 across selftest and tools`
22. `6a584dc` — `test(gpio): record T1 build, T18 regression, and mock test captures for fdf7200`
23. `36aab61` — `docs(gpio): update implementation report for audit round 3 corrections`
24. `8b2b2ce` — `docs(gpio): audit the third F2 correction at 36aab61`
25. `b1b6342` — `fix(tools): address audit round 4 findings in gpio_f2_checks.py`
26. `ddca912` — `test(gpio): record T1 build, T18 regression, and mock test captures for b1b6342`

---

## 2. Matriz de resolución de hallazgos de auditoría

### 2.1 Hallazgos de la cuarta auditoría (R5, N1, R4 y menores)

| ID | Clase | Hallazgo de auditoría | Acción correctiva aplicada y verificación con código y capturas |
| --- | --- | --- | --- |
| **R5** | Bloqueante | Comprobación de procedencia de las ELFs del control dependía de `b"3df160a" in data` en rutas de depuración; líneas borradas con código como `*ptr = 0;` podían enmascararse como comentarios. | En [gpio_f2_checks.py:127-146](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L127-L146), `verify_elf_embedded_revision` ignora ocurrencias del hash que forman parte de nombres de directorios o rutas del filesystem (`control`, `stm32`, `/`, `\`), exigiendo la presencia real del commit embebido en código/datos compilados de las ELFs de control. En [gpio_f2_checks.py:990-1004](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L990-L1004), se implementó un control negativo sobre un ELF de control mutado con hash foráneo (`deadbeef`), verificando su rechazo. En [gpio_f2_checks.py:293-312](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L293-L312), `strip_c_comments` se ejecuta primero y `is_c_code_line` identifica con exactitud sentencias C válidas (incluso precedidas por `*` o `*/`). En [gpio_f2_checks.py:344-350](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L344-L350), `check_headers` obtiene la lista canónica de cabeceras desde el árbol de control git `3df160a`. Respaldado en [gpio_f2_checks_b1b6342.txt:7, 8, 22-26](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_b1b6342.txt#L7). |
| **N1** | Bloqueante | Marcadores desconocidos o no expandibles (e.g. `{self.label}`) en `expect()` se omitían y computaban falsamente como casados (30/30); `tier` se obtenía de lista cableada a mano. | En [gpio_f2_checks.py:732-809](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L732-L809), `extract_host_expect_patterns` extrae `TIERS` dinámicamente mediante el AST de `gpio_hw_check.py`, y clasifica cualquier marcador desconocido o no expandible en `unexpandable_patterns`. En [gpio_f2_checks.py:862-885](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L862-L885), `match_expect_patterns` registra cualquier patrón no expandible como no casado, forzando `FAIL`. Se añadió el control negativo 4 ([gpio_f2_checks.py:937-944](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L937-L944)), que inyecta `{self.label} NO SUCH LINE ANYWHERE` y prueba que es rechazado. Respaldado en [gpio_f2_checks_b1b6342.txt:59, 63](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_b1b6342.txt#L59). |
| **R4** | Mayor | Citas desfasadas en el informe en líneas 51, 57, 59, 73, 74 y 88; afirmaciones imprecisas sobre analizadores y listas manuales; falta de compilación previa en §6.2. | Se auditaron y actualizaron todas las citas a las líneas canónicas del commit `b1b6342`: `parse_nm_symbols` ([gpio_f2_checks.py:585-600](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L585-L600)); bucle y salida anticipada `pulse_train_measured` ([gpio_hw_check.py:907-987](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L907-L987)); fallback `C:\ST` ([gpio_f2_checks.py:46-49](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L46-L49)); `FDwfGetLastErrorMsg` ([gpio_hw_check.py:315](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L315)); verificación de autorización `bench_claim` y `!s_bench_abort` ([CM7 gpio_selftest.c:1636, 1666, 1782, 1879](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1636), [gpio_selftest.c:1704](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1704); [CM4 gpio_selftest.c:146-151](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM4/App/Src/gpio_selftest.c#L146-L151)); mantenimiento del NVIC ([CM7 gpio_selftest.c:1609-1614](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1609-L1614); [CM4 gpio_selftest.c:158-165](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM4/App/Src/gpio_selftest.c#L158-L165)); host check T13 ([gpio_hw_check.py:1209-1274](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L1209-L1274)); y `stamp()` ([gpio_hw_check.py:1611](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L1611)). Se eliminaron las afirmaciones imprecisas y se agregaron en §6.2 los comandos explícitos de compilación limpia para selftest y release previo al flasheo. |
| **R1 / Minors** | Menor | Comprobación de tipos de símbolos (`T` vs `W`); validación de código de salida de `nm` en `dummy_elf`; excepciones sin captura en `main()`; y sección `.data` en control negativo de mapa. | En [gpio_f2_checks.py:585-600](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L585-L600), `parse_nm_symbols` rechaza explícitamente líneas de 3 campos y símbolos con tamaño <= 0. En [gpio_f2_checks.py:611-627](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L611-L627), `compare_symbols` compara tamaños y tipos de símbolos (`T` vs `W`), con control negativo de tipo en [gpio_f2_checks.py:717-724](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L717-L724). En [gpio_f2_checks.py:494-497](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L494-L497), se valida `code_nm == 0` sobre `dummy_elf`. En [gpio_f2_checks.py:570-577](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L570-L577), se fijó el texto del control negativo de mapa declarando la sección `.data`. En [gpio_f2_checks.py:964, 1052-1055](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L964), `main()` captura excepciones generales y sale limpiamente con código 1. Respaldado en [gpio_f2_checks_b1b6342.txt:33, 37, 57](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_b1b6342.txt#L33). |

---

### 2.2 Estado de hallazgos previos (B1–B4, M1–M6, m1–m7, N2–N9, R1–R3)

| ID | Clase | Estado y trayectoria de resolución |
| --- | --- | --- |
| **B1** | Bloqueante | **Resuelto en Rondas 3 y 4.** Verificador cruzado AST [gpio_f2_checks.py:732-946](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L732-L946) extrayendo las 29 llamadas `expect()` de `gpio_hw_check.py` con `TIERS` dinámico, cotejándolas contra las cadenas sintetizadas de fuentes C sin listas manuales, y validado por 4 controles negativos que fallan. |
| **B2** | Bloqueante | **Resuelto en Ronda 2.** `CCCSR.EN = 1` se escribe bajo sección crítica con comprobación inmediata, saliendo instantáneamente ([gpio_bm.c:945-966](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L945-L966)). La espera del bit `READY` se ejecuta fuera de la sección, acotada por `DWT->CYCCNT` y detección de lecturas estancadas ([gpio_bm.c:971-996](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L971-L996)). Retención en silicio medida por el auditor: 140 ciclos en `6b7f10a` y 151 ciclos en `c90f2b5` (<< 60 µs / 24 000 ciclos). |
| **B3** | Bloqueante | **Resuelto en Ronda 2.** Analizador lógico con `DigitalInHelper` adquiriendo flancos y respuestas a 100 MS/s con disparo configurable y prellenado ([gpio_hw_check.py:227-294](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L227-L294)). |
| **B4** | Bloqueante | **Resuelto en Rondas 2, 3 y 4.** Funciones de análisis puras sobre texto bruto, compilación `-Clean`, selftests con `--fresh` y `--clean-first`, y controles negativos independientes sin tocar el árbol ([gpio_f2_checks.py](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py)). |
| **M1** | Mayor | **Resuelto en Ronda 2.** Lectura de verificación con retorno `GPIO_BM_ERR_VERIFY` en `ExtiConfig` ([gpio_bm.c:711-754](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L711-L754)), `ExtiDisable` ([gpio_bm.c:824-850](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L824-L850)), `enable_syscfg_clock` ([gpio_bm.c:573-578](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L573-L578)), y `CCCSR.EN` ([gpio_bm.c:957-961](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L957-L961)). |
| **M2** | Mayor | **Resuelto en Rondas 2, 3 y 4.** Verificación estricta de árbol limpio y commit del submódulo Cube; procedencia de ELFs de control verificada en código/datos; tamaños de estado analizados desde `.map`; verificación de 0 handlers EXTI en las 16 imágenes de producción; congelamiento de cabeceras respecto a `3df160a` ([gpio_f2_checks.py](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py)). |
| **M3** | Mayor | **Resuelto en Rondas 2 y 3.** CM7 verifica `bench_claim` y `!s_bench_abort` antes de autorizar a CM4 la salida PE6 ([CM7 gpio_selftest.c:1636, 1666, 1782, 1879](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1636) y [gpio_selftest.c:1704](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1704)); CM4 rechaza la configuración si no cuenta con dicha autorización `pe6_auth` ([CM4 gpio_selftest.c:146-151](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM4/App/Src/gpio_selftest.c#L146-L151)). |
| **M4** | Mayor | **Resuelto en Ronda 2.** Controles negativos de T11 EVENT y T13 CM7/CM4 mantienen el NVIC encendido ([CM7 gpio_selftest.c:1609-1614](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1609-L1614); [CM4 gpio_selftest.c:158-165](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM4/App/Src/gpio_selftest.c#L158-L165)) y los pines de respuesta configurados como salidas, aislando de forma inequívoca el enmascaramiento de la línea EXTI; host check en [gpio_hw_check.py:1209-1274](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L1209-L1274). |
| **M5** | Mayor | **Resuelto en Ronda 2.** Oráculos exhaustivos en T14: lectura directa de `PR1` antes y después de `ExtiConfig`; verificación de retorno del bit 0 en `ExtiTakePending`; validación de `RTSR1`/`FTSR1` con trigger `BOTH`; restauración verificada a puerto `PE` ([CM7 gpio_selftest.c:1820-1930](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1820-L1930)). |
| **M6** | Mayor | **Resuelto en Rondas 3 y 4.** El presente informe respalda cada afirmación con citas exactas de línea del commit `b1b6342` y capturas commiteadas; las pruebas de hardware se declaran formalmente como NO VERIFICADAS por el implementador; y se proveen comandos de banco plenamente funcionales con pasos de compilación previa. |
| **R1** | Bloqueante | **Resuelto en Rondas 3 y 4.** `parse_nm_symbols` rechaza líneas de 3 campos y tamaños <= 0 ([gpio_f2_checks.py:585-600](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L585-L600)); `compare_symbols` compara tamaños y tipos de símbolos (`T` vs `W`) ([gpio_f2_checks.py:611-627](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L611-L627)); verificación de códigos de salida de `nm` en imágenes actuales y de control ([gpio_f2_checks.py:494-497, 605-609](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L494-L497)); y controles negativos independientes en directorios temporales para cabeceras, handlers, tamaños, símbolos extra y tipos de símbolos. |
| **R2** | Mayor | **Resuelto en Ronda 3.** `check_selftest_builds` valida explícitamente `cfg_code == 0` tras `--fresh` antes de compilar con `--clean-first` ([gpio_f2_checks.py:253-290](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L253-L290)). |
| **R3** | Mayor | **Resuelto en Ronda 3.** `check_clean_working_tree` compara de forma estricta contra `output_file.resolve()` exigiendo `git status` con retorno 0 ([gpio_f2_checks.py:101-125](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L101-L125)). |
| **N4** | Menor | **Resuelto en Ronda 3.** `main()` captura `RuntimeError`, asegura la transición del AD2 a S2 y emite `HOST VERDICT=FAIL` ([gpio_hw_check.py:1697-1720](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L1697-L1720)); `pulse_train_measured` ([gpio_hw_check.py:907-987](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L907-L987)) verifica `fw_abort` y sale anticipadamente; probado en Fase 4 del mock ([gpio_hw_check.py:1571-1576](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L1571-L1576)). |
| **N5** | Menor | **Resuelto en Ronda 3.** `DigitalInHelper.arm` lanza `RuntimeError` ante solicitud superior a `BufferSizeInfo` ([gpio_hw_check.py:245-247](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L245-L247)); probado en Fase 4 del mock ([gpio_hw_check.py:1566-1570](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L1566-L1570)). |
| **m1** | Menor | **Resuelto en Ronda 2.** Documentado en [gpio_bm.h:318-320](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L318-L320) que `SYSCFGEN` se habilita en `RCC` antes de la sección crítica. |
| **m2** | Menor | **Resuelto en Ronda 2.** Citas de RM0399 corregidas (`RTSR1` §21.6.1, `FTSR1` §21.6.2, `SWIER1` §21.5.5 y §21.6.3, `PR1` §21.6.21, `IMR1`/`EMR1` §21.6.19/§21.6.20). |
| **m3** | Menor | **Resuelto en Ronda 2.** Comentario de `ExtiSoftwareTrigger` corregido reconociendo `RTSR1` o `FTSR1` ([gpio_bm.h:388-390](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L388-L390)). |
| **m4** | Menor | **Resuelto en Ronda 2.** Documentado en [gpio_bm.h:320-323](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L320-L323) que un núcleo puede modificar los flancos de una línea compartida activa. |
| **m5** | Menor | **Resuelto en Ronda 2.** Añadido `#error` ante `DUAL_CORE` sin núcleo definido ([gpio_bm.h:19-20](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L19-L20)); documentado retorno de `ERR_CONFLICT` ante segundo intento de deinit no enrutado. |
| **m6** | Menor | **Resuelto en Ronda 2.** Pull-downs en PE3/PE4 durante pasos F2; foto ampliada con registros completos; medición de duración de `pre()`; mock desacoplado. |
| **m7** | Menor | **Resuelto en Rondas 2 y 3.** Nombre corregido de `prj_02_eth_lwip_dualcore`; detección de herramientas en `C:\ST` como fallback condicional ([gpio_f2_checks.py:46-49](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L46-L49)); escritura y flush inmediato de capturas. |
| **N2** | Menor | **Resuelto en Ronda 2.** Verificación explícita `if (s_bench_abort) { goto cleanup; }` en [CM7 gpio_selftest.c:1704-1707](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1704-L1707) antes del segundo envío de configuración a CM4 en T12. |
| **N3** | Menor | **Resuelto en Ronda 2.** Condición de `sec_ok` en [CM7 gpio_selftest.c:2003](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L2003) exige `(hold_cycles >= 1U) && (hold_cycles < (60U * CYC_PER_US))`. |
| **N6** | Menor | **Resuelto en Ronda 2.** `f2_pins_pd` invoca `pins_input_raw(GPIOE, (1U << PE_EXTI3) | (1U << PE_EXTI4), GPIO_BM_PULL_DOWN)` bajo sección crítica en [CM7 gpio_selftest.c:1589-1600](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1589-L1600). |
| **N7** | Menor | **Resuelto en Ronda 2.** Detección de lecturas consecutivas sin avance en `GPIO_BM_EnableCompensationCell` ([gpio_bm.c:971-996](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L971-L996)), retornando `GPIO_BM_ERR_NOT_READY` si `CYCCNT` se estanca por más de 100 lecturas. |
| **N8** | Menor | **Resuelto en Ronda 2.** Método `stamp()` en [gpio_hw_check.py:1611](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L1611) escribe y ejecuta `flush()` inmediato tras cada línea emitida. |
| **N9** | Menor | **Resuelto en Ronda 2.** Parámetro sin uso `expect_resp` eliminado de firmas y llamadas en `pulse_train_measured`. |

---

## 3. Implementación del Driver: Reglas del Diseño 2

A continuación se detalla la correspondencia rigurosa entre cada regla del Diseño 2 (`IMPLEMENTATION_PLAN.md`, rev 2) y su implementación en el código del driver:

1. **Regla 1: Núcleo que llama:**
   - Selección condicional de registros de CPU2 (`C2IMR1`, `C2EMR1`, `C2PR1`) bajo directiva `#if defined(CORE_CM4)`, y registros de CPU1 (`IMR1`, `EMR1`, `PR1`) en caso contrario ([gpio_bm.c:667-708, 800-810, 863-870](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L667-L708)).
   - Habilitación de `SYSCFGEN` en el banco de registros `RCC` correspondiente al núcleo que ejecuta la llamada, con verificación de lectura inmediata ([gpio_bm.c:573-578](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L573-L578)).
2. **Regla 2: Validación antes de escribir (C6):**
   - Retorno inmediato de `GPIO_BM_ERR_PARAM` sin tocar ningún registro si el puerto es nulo o inválido, si `pin_idx >= 16`, si `trigger` no está en 1..3, o si `interrupt` y `event` son ambos falsos ([gpio_bm.c:586-601](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L586-L601)).
   - Validación rigurosa de parámetros análoga en `ExtiDisable` ([gpio_bm.c:765-772](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L765-L772)), `ExtiSoftwareTrigger` ([gpio_bm.c:878-881](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L878-L881)) y `EnableCompensationCell` ([gpio_bm.c:928-931](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L928-L931)).
   - Mapeo de vectores en `ExtiGetIRQn` retornando `GPIO_BM_EXTI_IRQN_INVALID` para `pin_idx >= 16` ([gpio_bm.c:903-922](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L903-L922)).
   - Enmascaramiento estricto `line_mask &= 0xFFFFUL` en `ExtiTakePending` ([gpio_bm.c:861](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L861)).
3. **Regla 3: Lo compartido, en la sección crítica (C10):**
   - Acceso exclusivo con hardware semáforo HSEM 5 y `PRIMASK` entre `GPIO_BM_SectionEnter` y `GPIO_BM_SectionExit` para toda modificación en `SYSCFG_EXTICR`, `RTSR1`, `FTSR1`, `SWIER1` y `CCCSR` ([gpio_bm.c:609-614, 756, 779-784, 852, 884-889, 896, 945-950, 966](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L609-L614)).
   - Detección de conflictos de enrutamiento y comprobaciones de máscaras del núcleo opuesto evaluadas de forma atómica dentro de la sección ([gpio_bm.c:620-633, 788-795](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L620-L633)).
4. **Regla 4: Conflicto:**
   - `ExtiConfig` retorna `GPIO_BM_ERR_CONFLICT` sin escribir si `EXTICR` ya enruta la línea a otro puerto y alguna máscara de cualquier núcleo está activa ([gpio_bm.c:625-633](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L625-L633)).
   - `ExtiDisable` retorna `GPIO_BM_ERR_CONFLICT` sin modificar registros si la línea en `EXTICR` no apunta al puerto solicitado ([gpio_bm.c:791-795](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L791-L795)).
5. **Regla 5: Secuencia ordenada de `ExtiConfig`:**
   - 1. Enrutamiento del puerto en `SYSCFG->EXTICR` ([gpio_bm.c:642-645](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L642-L645)).
   - 2. Configuración de flancos en `RTSR1`/`FTSR1` según `trigger` ([gpio_bm.c:648-664](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L648-L664)).
   - 3. Borrado del bit pendiente en `PR1` (o `C2PR1`) del núcleo llamador ([gpio_bm.c:667-671](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L667-L671)).
   - 4. Desenmascaramiento selectivo en `IMR1`/`EMR1` (o `C2IMR1`/`C2EMR1`) ([gpio_bm.c:674-708](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L674-L708)).
   - 5. Verificación de lectura inmediata de todos los campos modificados, retornando `GPIO_BM_ERR_VERIFY` si no coinciden ([gpio_bm.c:711-754](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L711-L754)).
6. **Regla 6: `ExtiDisable` respeta al otro núcleo:**
   - Borra las máscaras `IMR1`/`EMR1` y limpia el bit pendiente `PR1` del núcleo que llama ([gpio_bm.c:800-810](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L800-L810)).
   - Borra los flancos `RTSR1`, `FTSR1` y resetea el campo de `EXTICR` a 0000 únicamente si las dos máscaras del otro núcleo para esa línea son cero ([gpio_bm.c:814-821](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L814-L821)).
   - Verificación de lectura antes de salir de la sección ([gpio_bm.c:824-850](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L824-L850)).
7. **Regla 7: `ExtiTakePending`:**
   - Lectura de `PR1` (o `C2PR1`), enmascaramiento con bits 0..15, borrado mediante escritura directa w1c (sin RMW), ejecución de barrera `__DSB()` y retorno del valor capturado ([gpio_bm.c:860-872](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L860-L872)).
   - Diseñada para ejecución segura desde ISR sin entrar en sección crítica ni tomar semáforos.
8. **Regla 8: `ExtiSoftwareTrigger`:**
   - Escritura de `(1 << pin_idx)` en `EXTI->SWIER1` bajo sección crítica ([gpio_bm.c:884-896](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L884-L896)).
   - Documentado en comentario que el disparo requiere habilitación de flancos en hardware (RM0399 §21.5.5).
9. **Regla 9: `EnableCompensationCell(max_cycles)`:**
   - Si `RCC_CR.CSIRDY == 0`, retorna `GPIO_BM_ERR_NOT_READY` sin modificar el periférico ([gpio_bm.c:934-937](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L934-L937)).
   - Activa `CCCSR.EN` en sección crítica, verifica lectura y **sale inmediatamente de la sección** ([gpio_bm.c:945-966](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L945-L966)).
   - Espera el bit `CCCSR.READY` fuera de la sección acotada por `DWT->CYCCNT` y seguimiento de lecturas estancadas ([gpio_bm.c:971-996](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L971-L996)).
   - No escribe en ningún caso `RCC_CR`, `HSLV`, `CS` ni `CCCR`.
10. **Regla 10: Restricciones de diseño (C7, C14):**
    - El driver no define ningún handler de interrupción (`01_shared_drivers/00_gpio/` libre de `IRQHandler`).
    - Estado estático en RAM inalterado: `.data = 0 B`, `.bss = 8 B` (`s_shared_lock`, `s_section_depth`).

---

## 4. Evidencia de pruebas sin placa (Capturas de commit `b1b6342`)

### 4.1 Compilación y Regresión T18 (`gpio_f2_checks_b1b6342.txt`)
- **Comando ejecutado:**  
  `py tools/gpio_f2_checks.py --test all --output 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_b1b6342.txt`
- **Integridad de árboles y procedencia:**
  - Árbol de trabajo en HEAD `b1b6342`: limpio (`working tree clean: PASS`, línea 6).
  - Árbol de control en `3df160a`: limpio con 16/16 binarios ELF verificados conteniendo la revisión en código/datos fuera de rutas de disco (`PASS`, línea 7).
  - Control negativo de procedencia: binario mutado a revisión foránea (`deadbeef`) detectado y rechazado (`PASS`, línea 8).
  - Submódulo STM32CubeH7: commit `8ec3113` verificado (`PASS`, línea 9).
- **T1 Compilación:**
  - Matriz completa `build_all.ps1 -Clean`: 8/8 imágenes exitosas en 193.5 s (`PASS`, líneas 11-14).
  - Selftest `prj_01_template_dualcore` (`--fresh`, `--clean-first`): 278 objetos C compilados, 0 avisos (`PASS`, línea 16).
  - Selftest `prj_04_adc_dma_bm_eth` (`--fresh`, `--clean-first`): 323 objetos C compilados, 0 avisos (`PASS`, línea 17).
  - Control negativo del contador de avisos: inyección sintética detectada y rechazada (`PASS`, línea 18).
  - `T1 VERDICT: PASS` (línea 19).
- **T18 Regresión:**
  - Congelamiento de cabeceras (`gpio_bm.h`, `gpio_hal.h`, `gpio_ll.h`): solo adiciones respecto a `3df160a` (`PASS`, líneas 22-24).
  - Control negativo de cabecera en directorio temporal (mutación de firma de función): detectado y rechazado (`PASS`, línea 25).
  - Control negativo de comentario con código en la misma línea: detectado y rechazado (`PASS`, línea 26).
  - Handlers en driver: 0 handlers en `01_shared_drivers/00_gpio/` (`PASS`, línea 28); control negativo con archivo `.c` en directorio temporal detectado (`PASS`, línea 29).
  - Handlers en producción: 0 handlers `EXTI*_IRQHandler` en las 16 imágenes de producción (`PASS`, línea 31); control positivo en `selftest-gpio` con 2 handlers detectados (`PASS`, línea 32); control negativo con texto de `nm` mutado detectado por `parse_production_handlers` (`PASS`, línea 33).
  - Estado estático desde mapa `.map`: CM7 `.data = 0 B`, `.bss = 8 B`; CM4 `.data = 0 B`, `.bss = 8 B` (`PASS`, líneas 35-36); control negativo con `.data = 4 B` mutado en texto detectado por `verify_driver_sizes` (`PASS`, línea 37).
  - Paridad de símbolos `GPIO_BM_*`: 16/16 imágenes de producción idénticas en símbolos, tamaños y tipos al control `3df160a` analizadas con `nm -S` (`PASS`, líneas 39-54).
  - Control negativo 1 de símbolos: mutación de tamaño del símbolo real `GPIO_BM_CheckConfig` detectada por `compare_symbols` (`PASS`, línea 55).
  - Control negativo 2 de símbolos: símbolo extra `+GPIO_BM_ExtiConfig` detectado por `compare_symbols` (`PASS`, línea 56).
  - Control negativo 3 de símbolos: alteración de tipo de símbolo (`T` -> `W`) en `GPIO_BM_CheckConfig` detectada por `compare_symbols` (`PASS`, línea 57).
  - Comprobación cruzada host-firmware (N1): 29/29 patrones regex `expect()` del host validados contra las cadenas de formato sintetizadas del firmware con `TIERS` extraído de AST (`PASS`, línea 59).
  - Control negativo 1 de crosscheck: mutación de `CM7 count=` en firmware rechazada (`PASS`, línea 60).
  - Control negativo 2 de crosscheck: mutación de `FW VERDICT=` en firmware rechazada (`PASS`, línea 61).
  - Control negativo 3 de crosscheck: mutación de patrón en el host rechazada (`PASS`, línea 62).
  - Control negativo 4 de crosscheck: marcador no reconocido/no expandible `{self.label}` rechazado (`PASS`, línea 63).
- **Veredicto general:** `T18 VERDICT: PASS`, `OVERALL VERDICT: PASS` (líneas 64-65).

### 4.2 Simulación del host y Analizador Lógico (`gpio_f2_mock_b1b6342.txt`)
- **Comando ejecutado:**  
  `py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --mock-test --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_mock_b1b6342.txt`
- **Fase 1 (Seguridad eléctrica S3):** 11 combinaciones de DIO prohibidas rechazadas por la guarda antes de la DLL (`PASS`); 5 combinaciones permitidas aceptadas (`PASS`).
- **Fase 2 (Sesión completa de pasos F2 simulada):** 25/25 checks del host aprobados (`PASS`):
  - T11 RISING, FALLING, BOTH y EVENT en CM7 y CM4 validados (200, 200, 400 y 0 pulsos/respuestas).
  - T12 enrutamiento independiente, línea compartida 3 y aislamiento de CM4 validados.
  - T13 latencias y oráculos de deshabilitación validados.
  - T14(a) borrado de pending en `ExtiConfig` y control sin borrado validados.
  - T14(b) detección de conflicto en `GPIOD` y restauración a `PE` validados.
  - T14(c) regla 6 de deinit evaluada con trigger `BOTH` verificando retención y limpieza final.
  - T14(d) 100 disparos por software con retorno del bit 0 en `ExtiTakePending` y control enmascarado validados.
  - T17 celda de compensación: rechazo sin CSI (`ERR_NOT_READY`), `READY = 1` con CSI, retención de sección simulada en 140 ciclos (`sec_ok = 1`), y restauración de `CSION` y `CCCSR` validados.
- **Fase 3 (Control negativo de subconteo):** Detección inmediata de subconteo de firmware (199 vs 200) marcada correctamente como FAIL (`PASS`, líneas 143-149).
- **Fase 4 (Pruebas de error DWF y rutas N4/N5):**
  - Detección de error en `_check_dwf`: `PASS` (línea 151).
  - Verificación de desbordamiento de buffer N5 en `DigitalInHelper.arm`: `PASS` (línea 152).
  - Salida anticipada en bucles por flanco ante `fw_abort` (N4): `PASS` (líneas 153-154).
- **Veredicto general del mock:** `OVERALL MOCK VERDICT: PASS`, `HOST VERDICT=PASS` (líneas 155-156).

---

## 5. Estado de las pruebas en hardware

> [!IMPORTANT]
> **Declaración formal de pruebas en placa:**  
> Las pruebas en hardware (T11–T14, T17 y warm resets) son **NO VERIFICADAS por el implementador** debido a la restricción absoluta de acceso a la placa física y al instrumental AD2. Corresponde exclusivamente al auditor su ejecución en el banco físico con el instrumental calibrado.

A título informativo y como referencia de línea base en silicio, se citan los resultados obtenidos y certificados por el auditor en sus corridas en hardware sobre los commits `6b7f10a` ([gpio_f2_audit_bench_6b7f10a.txt](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_audit_bench_6b7f10a.txt)) y `c90f2b5` ([gpio_f2_audit_bench_c90f2b5.txt](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_audit_bench_c90f2b5.txt)):
- **T11 Conteo de flancos y respuestas:** En CM7 y CM4, cuentas exactas registradas tanto por ISR como por el analizador lógico: RISING 200/200/200, FALLING 200/200/200, BOTH 400/400/400. Control EVENT con NVIC encendido: 0 ISR y 0 respuestas para 200 flancos enviados.
- **T12 Enrutamiento dual-core:** Líneas independientes 200/200 (LA PE3→DIO9=200, PE4→DIO11=200); línea compartida 200/200 en ambos núcleos; control con CM4 deshabilitado: CM7 200, CM4 0 (LA DIO11 0).
- **T13 Latencias reales:** Medidas sobre los 200 flancos de subida de T11:
  - En `6b7f10a`: CM7 mín = 0.10 µs, p50 = 0.10 µs, máx = 0.22 µs; CM4 mín = 0.15 µs, p50 = 0.16 µs, máx = 0.24 µs (límite ≤ 10 µs).
  - En `c90f2b5`: CM7 0.10 / 0.10 / 0.22 µs; CM4 0.15 / 0.16 / 0.25 µs.
  - Controles con línea enmascarada: 0 transiciones en 20 pulsos.
- **T14 Características EXTI:**
  - (a) `PR1` = 0x08 antes, 0x00 después de `ExtiConfig`, 0 ISR; control: 1 ISR.
  - (b) `GPIO_BM_ERR_CONFLICT` (6) con foto intacta; enrutamiento y restauración a `PE` OK.
  - (c) Compartida mantiene `EXTICR`, `RTSR1` y `FTSR1`; última llamada los borra.
  - (d) 100 ISR con `ExtiTakePending` devolviendo bit 0 en las 100 llamadas.
- **T17 Celda de compensación:**
  - (a) Sin CSI: `ERR_NOT_READY` (7) con foto intacta; control `READY = 0`.
  - (b) Con CSI: `GPIO_BM_OK` con `READY = 1` alcanzado a los **76 641 ciclos** (~192 µs) en `6b7f10a`, y **76 545 ciclos** en `c90f2b5`.
  - Retención de la sección crítica medida por el shim: **140 ciclos** (0.35 µs) en `6b7f10a`, y **151 ciclos** (0.38 µs) en `c90f2b5`, cumpliendo holgadamente el límite de 60 µs (24 000 ciclos).
  - Restauración de `CSION = 1` y `CCCSR = 0x00000101` confirmada.
- **Compuerta T9 en PF14:** En la corrida `c90f2b5`, la compuerta de T9 en PF14 registró 1.17 ns frente a 1.34 ns requeridos, debido al margen escaso inherente documentado en README §10 tras la reconexión del banco, sin afectar el funcionamiento del driver F2.

---

## 6. Procedimiento y comandos de verificación en banco para el auditor

### 6.1 Preparación del banco físico (por el responsable)
1. Extraer los jumpers J2 y J3 (aislamiento del bus I2C en PF14/PF15 para evitar distorsión capacitiva en T9).
2. Desconectar el cable USB de la placa NUCLEO-F334R8 (evitar colisiones en el controlador ST-LINK).
3. Conectar el Analog Discovery 2 por USB, conectando siempre el terminal GND antes que las líneas DIO.
4. Asegurar que la aplicación WaveForms GUI se encuentre completamente cerrada.

### 6.2 Comandos de ejecución en banco

```powershell
# 1. Posicionarse en la raíz del repositorio
cd C:\Users\hrodriguez\Documents\R+D\00_Embedded_System\00_STmicro\stm32h755

# 2. Compilar la imagen selftest-gpio asegurando que la revisión embebida coincida con HEAD
cmake --preset selftest-gpio --fresh
cmake --build --preset selftest-gpio --clean-first

# 3. Flashear la imagen selftest-gpio correspondiente al commit b1b6342
py 03_firmware_templates/prj_01_template_dualcore/tools/flash.py --sn 0016000C4142501220353451 --preset selftest-gpio

# 4. Primera ejecución completa del banco de pruebas (T0 a T17)
py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --preset selftest-gpio --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_selftest_b1b6342_1.txt

# 5. Segunda ejecución completa del banco (verificación de repetibilidad)
py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --preset selftest-gpio --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_selftest_b1b6342_2.txt

# 6. Compilar la imagen de producción release
cmake --build 03_firmware_templates/prj_01_template_dualcore/build/release --clean-first

# 7. Flashear la imagen de producción release
py 03_firmware_templates/prj_01_template_dualcore/tools/flash.py --sn 0016000C4142501220353451 --preset release

# 8. Ejecutar los 20 warm resets consecutivos de producción con el proyecto completo
py tools/run_warm_resets.py --project 03_firmware_templates/prj_01_template_dualcore --preset release --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_warm_resets.txt --title "Warm resets"
```

---

## 7. Desviaciones y Decisiones Aprobadas

### 7.1 Decisiones Aprobadas (2026-10-15)
- **Decisión 1A:** Antigravity implementa y prueba sin placa; el auditor ejecuta las pruebas en hardware con las herramientas provistas.
- **Decisión 2A:** Pulsos en DIO8 y DIO10 generados mediante escrituras estáticas con oráculo de conteo en el LA; `tools/ad2.py` y las guardas S1–S11 se mantienen inalteradas.
- **Decisión 3A:** Celda de compensación implementada en driver y validada en T17; `SystemClock_Config` permanece con la llamada existente.
- **Decisión 4:** Los documentos de proceso se conservan hasta el cierre formal de la fase.

### 7.2 Desviaciones Aprobadas y Justificadas
1. **Escritura previa de `SYSCFGEN` (`m1`):**  
   `enable_syscfg_clock` escribe en `RCC->APB4ENR` antes de entrar a la sección crítica ([gpio_bm.c:573-578](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L573-L578)). Como este registro está bancado por hardware para cada núcleo, no existe colisión inter-núcleo; `SYSCFGEN` permanece habilitado aun si la función retorna error posterior ([gpio_bm.h:318-320](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L318-L320)).
2. **Alteración de trigger en línea compartida (`m4`):**  
   Se permite que un núcleo modifique `RTSR1`/`FTSR1` de una línea compartida activa si el llamador así lo solicita ([gpio_bm.h:320-323](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L320-L323)).
3. **Segundo `ExtiDisable` no enrutado devuelve conflicto (`m5`):**  
   Al deshabilitarse una línea por ambos núcleos, el campo de `EXTICR` se resetea a 0000 (GPIOA). Si posteriormente se intenta deshabilitar dicha línea indicando otro puerto, la función retorna `GPIO_BM_ERR_CONFLICT` porque el registro ya no apunta a ese puerto ([gpio_bm.h:356-360](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L356-L360)).
4. **Espera de `READY` fuera de sección crítica con seguimiento de estancamiento (`B2`, `N7`):**  
   Para evitar violar el límite de retención de 60 µs del semáforo entre núcleos, la espera de `READY` se ejecuta fuera de la sección crítica, con timeout acotado por ciclos de `DWT->CYCCNT` y detección de lecturas estancadas consecutivas ([gpio_bm.c:963-996](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L963-L996)).

---

F1 CORREGIDA 4 PARA AUDITORIA b1b6342
