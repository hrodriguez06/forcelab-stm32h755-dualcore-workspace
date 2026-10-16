# Informe de Implementación: Fase 00_gpio F2 (Tercera Corrección de Auditoría)

**Fecha:** 2026-10-16  
**Rama:** `driver/gpio-f2`  
**Estado:** F1 ENTREGADA PARA AUDITORÍA  
**Último commit de código o herramienta:** `fdf7200`  
**Commit de capturas de verificación:** `6a584dc`  

---

## 1. Resumen de la entrega corregida

El presente informe documenta la resolución rigurosa e integral de todos los hallazgos emitidos en la tercera ronda de auditoría de la fase 00_gpio F2 (`AUDIT.md`, sección *F1, corrección 2 (c90f2b5)*), cubriendo los bloqueantes y mayores de la herramienta sin placa (R1, R2, R3, R5), la corrección del informe y comandos de banco (R4), la comprobación cruzada automática AST host-firmware (N1), el manejo de errores y rutas de seguridad DWF (N4, N5), y los hallazgos menores (comentarios desfasados, codificación de consola, búsqueda de herramientas en PATH y cobertura del mock).

Todas las comprobaciones sin placa (T1 de compilación con matriz limpia 8/8 y selftests con verificación estricta de avisos y conteo de objetos C; T18 de regresión contra el control `3df160a` con analizadores de texto puro, paridad estricta de tamaños de símbolos y controles negativos que no tocan el árbol de trabajo; y la prueba del host con el mock DWF extendido a 4 fases) fueron ejecutadas satisfactoriamente con veredicto favorable (`PASS`), respaldadas íntegramente por las capturas commiteadas en `03_firmware_templates/prj_01_template_dualcore/docs/` (`gpio_f2_checks_fdf7200.txt` y `gpio_f2_mock_fdf7200.txt`).

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

---

## 2. Matriz de resolución de hallazgos de auditoría

### 2.1 Hallazgos de la tercera auditoría (R1–R5, N1, N4, N5 y menores)

| ID | Clase | Hallazgo de auditoría | Acción correctiva aplicada y verificación con código y capturas |
| --- | --- | --- | --- |
| **R1** | Bloqueante | `parse_nm_symbols` asignaba tamaño 0 a líneas de 3 campos; `compare_symbols` daba `True` si ambos tenían tamaño 0; control de símbolos no probaba alteración de tamaño; `nm` actual ignoraba código de salida; controles de cabecera y handlers mutaban el árbol de trabajo (`gpio_bm.h` y `temp_dummy_exti_test.c`). | En [gpio_f2_checks.py:513-528](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L513-L528), `parse_nm_symbols` rechaza líneas de 3 campos sin tamaño o con tamaño 0. En [gpio_f2_checks.py:554-646](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L554-L646), `check_production_symbols` verifica el código de salida de `nm -S` (`code == 0`) sobre las ELFs actuales y exige que la comparación de tamaños no esté vacía en las 16 ELFs. Se añadieron dos controles negativos: el primero altera el tamaño de un símbolo real (`GPIO_BM_CheckConfig`) y es rechazado por `compare_symbols` ([gpio_f2_checks.py:613-630](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L613-L630)); el segundo añade un símbolo extra (`+GPIO_BM_ExtiConfig`). Los controles de cabecera ([gpio_f2_checks.py:317-340](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L317-L340)) y de handler ([gpio_f2_checks.py:427-447](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L427-L447)) operan exclusivamente dentro de `tempfile.TemporaryDirectory()`, dejando el árbol 100% limpio. Respaldado en [gpio_f2_checks_fdf7200.txt:24-25, 28, 32, 54-55](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_fdf7200.txt#L24-L55). |
| **R2** | Mayor | Código de salida del paso de configuración CMake (`cmake --preset selftest-gpio --fresh`) no era comprobado en la herramienta sin placa. | En [gpio_f2_checks.py:221-258](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L221-L258), `check_selftest_builds` valida explícitamente `cfg_code == 0` tras el paso `--fresh`, antes de compilar con `--clean-first`. Se valida también `build_code == 0`, conteo de avisos con `parse_compiler_warnings` ([gpio_f2_checks.py:207-214](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L207-L214)), y objetos C con `count_built_objects` ([gpio_f2_checks.py:216-219](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L216-L219)). Respaldado en [gpio_f2_checks_fdf7200.txt:14-18](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_fdf7200.txt#L14-L18). |
| **R3** | Mayor | La comparación de rutas en `check_clean_working_tree` utilizaba `endswith`, pudiendo enmascarar archivos en directorios anidados. | En [gpio_f2_checks.py:101-125](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L101-L125), la ruta del archivo no rastreado se normaliza y se compara de forma estricta contra `output_file.resolve()`. Se exige además `res.returncode == 0` en `git status`. Respaldado en [gpio_f2_checks_fdf7200.txt:6](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_fdf7200.txt#L6). |
| **R4** | Mayor | Comando de warm resets erróneo (`--project prj_01`); citas de línea desfasadas en el informe; B1 y M6 dados por cerrados; afirmaciones no respaldadas; pruebas de placa no declaradas como NO VERIFICADAS. | Se corrigió el comando de warm resets utilizando la ruta completa (`--project 03_firmware_templates/prj_01_template_dualcore`). Se reescribió íntegramente el informe actualizando cada cita de línea al commit `fdf7200`. Se describe la trayectoria real de B1 y M6 en lugar de darlos por cerrados en la primera ronda. Se declaran todas las pruebas de hardware como **NO VERIFICADAS por el implementador**, respaldando sus referencias únicamente con las capturas de auditoría en silicio (`gpio_f2_audit_bench_6b7f10a.txt` y `gpio_f2_audit_bench_c90f2b5.txt`). |
| **R5** | Menor | Líneas borradas como `*/ int legacy;` pasaban el filtro de comentarios de cabecera; no se verificaba que las 16 ELFs de control provinieran de `3df160a`; presencia de `finally: pass`. | En [gpio_f2_checks.py:261-296](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L261-L296), `verify_header_diff` elimina comentarios C con `strip_c_comments` ([gpio_f2_checks.py:355-359](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L355-L359)) y detecta si la línea borrada contenía identificadores de código válidos. En [gpio_f2_checks.py:127-169](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L127-L169), `check_control_worktree` inspecciona las 16 imágenes ELF del árbol de control y verifica la presencia del hash `b"3df160a"`. Se eliminó el bloque vacío `finally: pass` en [gpio_f2_checks.py:863-868](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L863-L868). Respaldado en [gpio_f2_checks_fdf7200.txt:7, 24-25](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_fdf7200.txt#L7). |
| **N1** | Mayor | Extractor de expresiones `expect()` omitía patrones `rf"..."` (cubriendo solo 16 de 18); cadenas del firmware inyectadas a mano; falta de controles negativos en copias temporales. | Se implementó el extractor basado en AST `extract_host_expect_patterns` ([gpio_f2_checks.py:648-707](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L648-L707)), extrayendo las 29 llamadas `expect()` de `gpio_hw_check.py` (incluyendo prefijos `r`, `rf` y `f`, garantizando conteo exacto de llamadas). La función `synthesize_fw_lines_from_sources` ([gpio_f2_checks.py:709-757](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L709-L757)) extrae directamente de las fuentes C (`gpio_selftest.c` y `gpio_selftest_common.c`) todas las plantillas de `LOG`, `verdict` y `step_begin` sin listas manuales. Se añadieron 3 controles negativos sobre copias temporales (mutando `CM7 count=`, mutando `FW VERDICT=` y alterando un patrón del host), verificando que los 3 fallen ([gpio_f2_checks.py:799-825](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L799-L825)). Respaldado en [gpio_f2_checks_fdf7200.txt:56-60](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_fdf7200.txt#L56-L60). |
| **N4** | Menor | `RuntimeError` lanzado ante fallo DWF no era capturado por el bloque de excepción en `main()`, terminando la ejecución sin imprimir veredicto del host. | En [gpio_hw_check.py:1697-1720](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L1697-L1720), `main()` captura `RuntimeError`, asegura la transición del AD2 a estado seguro S2, registra el mensaje en `abort` y emite `HOST VERDICT=FAIL`. Los bucles de emisión de flancos en `_measure_t11_subtest` ([gpio_hw_check.py:783-831](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L783-L831)) y `_measure_t12_subtest` ([gpio_hw_check.py:833-885](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L833-L885)) verifican `fw_abort` y lecturas vencidas, saliendo anticipadamente ante abortos del firmware. Validado en la Fase 4 del mock ([gpio_hw_check.py:1571-1576](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L1571-L1576)). Respaldado en [gpio_f2_mock_fdf7200.txt:153-154](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_mock_fdf7200.txt#L153-L154). |
| **N5** | Menor | `DigitalInHelper.arm` recortaba en silencio las muestras solicitadas si superaban `BufferSizeInfo`. | En [gpio_hw_check.py:245-247](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L245-L247), `DigitalInHelper.arm` lanza `RuntimeError` explícito si `samples > max_size.value`. Probado en la Fase 4 del mock ([gpio_hw_check.py:1566-1570](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L1566-L1570)). Respaldado en [gpio_f2_mock_fdf7200.txt:152](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_mock_fdf7200.txt#L152). |
| **Menores** | Menor | Comentarios desfasados en wiring; decodificación de consola; búsqueda agresiva en PATH; `_MockDwf` sin `FDwfGetLastErrorMsg`; mock sin Fase 4. | Comentarios actualizados en [gpio_hw_check.py:73, 75, 76](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L73) y [CM7 gpio_selftest.c:56-59](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L56-L59). Decodificación con `errors="backslashreplace"` en [gpio_hw_check.py:581](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L581). `find_tools` añade `C:\ST` al PATH solo como fallback condicional si `arm-none-eabi-gcc` no se localiza inicialmente ([gpio_f2_checks.py:53-57](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L53-L57)). `_MockDwf` implementa `FDwfGetLastErrorMsg` retornando 1 ([gpio_hw_check.py:343-346](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L343-L346)). `run_mock_test` incluye Fase 4 ([gpio_hw_check.py:1561-1579](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L1561-L1579)), cubriendo `_check_dwf`, desbordamiento N5 y salida anticipada N4. |

---

### 2.2 Estado de hallazgos previos (B1–B4, M1–M6, m1–m7, N2, N3, N6–N9)

| ID | Clase | Estado y trayectoria de resolución |
| --- | --- | --- |
| **B1** | Bloqueante | **Resuelto en Ronda 3.** En la Ronda 1 se identificó discordancia en la cadena de T17. En la Ronda 2 se unificó la cadena en firmware y host, pero el mock continuaba escrito a mano y sin mecanismo automático de validación (N1). En la Ronda 3 (commit `fdf7200`) se implementó el verificador cruzado AST [gpio_f2_checks.py:648-830](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L648-L830), extrayendo automáticamente las 29 expresiones `expect()` del host y cotejándolas contra las cadenas sintetizadas de las fuentes C, respaldado con 3 controles negativos que fallan. |
| **B2** | Bloqueante | **Resuelto en Ronda 2.** `CCCSR.EN = 1` se escribe bajo sección crítica con comprobación inmediata, saliendo instantáneamente ([gpio_bm.c:945-966](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L945-L966)). La espera del bit `READY` se ejecuta fuera de la sección, acotada por `DWT->CYCCNT` y detección de lecturas estancadas ([gpio_bm.c:971-996](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L971-L996)). Retención en silicio medida por el auditor: 140 ciclos en `6b7f10a` y 151 ciclos en `c90f2b5` (<< 60 µs / 24 000 ciclos). |
| **B3** | Bloqueante | **Resuelto en Ronda 2.** Analizador lógico con `DigitalInHelper` adquiriendo flancos y respuestas a 100 MS/s con disparo configurable y prellenado ([gpio_hw_check.py:227-294](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L227-L294)). |
| **B4** | Bloqueante | **Resuelto en Ronda 2 y perfeccionado en Ronda 3.** Funciones de análisis puras sobre texto bruto, compilación `-Clean`, selftests con `--fresh` y `--clean-first`, y controles negativos independientes sin tocar el árbol ([gpio_f2_checks.py](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py)). |
| **M1** | Mayor | **Resuelto en Ronda 2.** Lectura de verificación con retorno `GPIO_BM_ERR_VERIFY` en `ExtiConfig` ([gpio_bm.c:711-754](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L711-L754)), `ExtiDisable` ([gpio_bm.c:824-850](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L824-L850)), `enable_syscfg_clock` ([gpio_bm.c:573-578](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L573-L578)), y `CCCSR.EN` ([gpio_bm.c:957-961](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L957-L961)). |
| **M2** | Mayor | **Resuelto en Rondas 2 y 3.** Verificación estricta de árbol limpio y commit del submódulo Cube; tamaños de estado analizados desde `.map`; verificación de 0 handlers EXTI en las 16 imágenes de producción; congelamiento de cabeceras respecto a `3df160a` ([gpio_f2_checks.py](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py)). |
| **M3** | Mayor | **Resuelto en Rondas 2 y 3.** CM7 verifica `bench_claim` y `!s_bench_abort` antes de autorizar a CM4 la salida PE6 ([CM7 gpio_selftest.c:1636, 1666, 1704, 1782, 1879](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1636)); CM4 rechaza la configuración si no cuenta con dicha autorización ([CM4 gpio_selftest.c:150-165](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM4/App/Src/gpio_selftest.c#L150-L165)). |
| **M4** | Mayor | **Resuelto en Ronda 2.** Controles negativos de T11 EVENT y T13 CM7/CM4 mantienen el NVIC encendido y los pines de respuesta configurados como salidas, aislando de forma inequívoca el enmascaramiento de la línea EXTI ([CM7 gpio_selftest.c:1645-1660](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1645-L1660)). |
| **M5** | Mayor | **Resuelto en Ronda 2.** Oráculos exhaustivos en T14: lectura directa de `PR1` antes y después de `ExtiConfig`; verificación de retorno del bit 0 en `ExtiTakePending`; validación de `RTSR1`/`FTSR1` con trigger `BOTH`; restauración verificada a puerto `PE` ([CM7 gpio_selftest.c:1820-1930](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1820-L1930)). |
| **M6** | Mayor | **Resuelto en Ronda 3.** En la Ronda 1 se emitieron afirmaciones sin respaldo en capturas. En la Ronda 2 persistieron desviaciones de formato (R4). En la Ronda 3 el presente informe respalda cada afirmación con citas exactas de línea de `fdf7200` y capturas commiteadas; las pruebas de hardware se declaran formalmente como NO VERIFICADAS por el implementador; y se proveen comandos de banco plenamente funcionales. |
| **m1** | Menor | **Resuelto en Ronda 2.** Documentado en [gpio_bm.h:318-320](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L318-L320) que `SYSCFGEN` se habilita en `RCC` antes de la sección crítica. |
| **m2** | Menor | **Resuelto en Ronda 2.** Citas de RM0399 corregidas (`RTSR1` §21.6.1, `FTSR1` §21.6.2, `SWIER1` §21.5.5 y §21.6.3, `PR1` §21.6.21, `IMR1`/`EMR1` §21.6.19/§21.6.20). |
| **m3** | Menor | **Resuelto en Ronda 2.** Comentario de `ExtiSoftwareTrigger` corregido reconociendo `RTSR1` o `FTSR1` ([gpio_bm.h:388-390](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L388-L390)). |
| **m4** | Menor | **Resuelto en Ronda 2.** Documentado en [gpio_bm.h:320-323](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L320-L323) que un núcleo puede modificar los flancos de una línea compartida activa. |
| **m5** | Menor | **Resuelto en Ronda 2.** Añadido `#error` ante `DUAL_CORE` sin núcleo definido ([gpio_bm.h:19-20](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L19-L20)); documentado retorno de `ERR_CONFLICT` ante segundo intento de deinit no enrutado. |
| **m6** | Menor | **Resuelto en Ronda 2.** Pull-downs en PE3/PE4 durante pasos F2; foto ampliada con registros completos; medición de duración de `pre()`; mock desacoplado. |
| **m7** | Menor | **Resuelto en Rondas 2 y 3.** Nombre corregido de `prj_02_eth_lwip_dualcore`; detección de herramientas en `C:\ST` como fallback; escritura y flush inmediato de capturas. |
| **N2** | Menor | **Resuelto en Ronda 2.** Verificación explícita `if (s_bench_abort) { goto cleanup; }` en [CM7 gpio_selftest.c:1704-1707](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1704-L1707) antes del segundo envío de configuración a CM4 en T12. |
| **N3** | Menor | **Resuelto en Ronda 2.** Condición de `sec_ok` en [CM7 gpio_selftest.c:2003](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L2003) exige `(hold_cycles >= 1U) && (hold_cycles < (60U * CYC_PER_US))`. |
| **N6** | Menor | **Resuelto en Ronda 2.** `f2_pins_pd` invoca `pins_input_raw(GPIOE, (1U << PE_EXTI3) | (1U << PE_EXTI4), GPIO_BM_PULL_DOWN)` bajo sección crítica en [CM7 gpio_selftest.c:1589-1600](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1589-L1600). |
| **N7** | Menor | **Resuelto en Ronda 2.** Detección de lecturas consecutivas sin avance en `GPIO_BM_EnableCompensationCell` ([gpio_bm.c:971-996](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L971-L996)), retornando `GPIO_BM_ERR_NOT_READY` si `CYCCNT` se estanca por más de 100 lecturas. |
| **N8** | Menor | **Resuelto en Ronda 2.** Método `stamp()` en [gpio_hw_check.py:137-142](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L137-L142) escribe y ejecuta `flush()` inmediato tras cada línea emitida. |
| **N9** | Menor | **Resuelto en Ronda 2.** Parámetro sin uso `expect_resp` eliminado de firmas y llamadas en `_measure_t11_subtest`. |

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

## 4. Evidencia de pruebas sin placa (Capturas de commit `fdf7200`)

### 4.1 Compilación y Regresión T18 (`gpio_f2_checks_fdf7200.txt`)
- **Comando ejecutado:**
  `py tools/gpio_f2_checks.py --test all --output 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_fdf7200.txt`
- **Integridad de árboles:**
  - Árbol de trabajo en HEAD `fdf7200`: limpio (`working tree clean: PASS`).
  - Árbol de control en `3df160a`: limpio con 16/16 binarios ELF verificados conteniendo `b"3df160a"` (`PASS`).
  - Submódulo STM32CubeH7: commit `8ec3113` verificado (`PASS`).
- **T1 Compilación:**
  - Matriz completa `build_all.ps1 -Clean`: 8/8 imágenes exitosas en 194.6 s (`PASS`).
  - Selftest `prj_01_template_dualcore` (`--fresh`, `--clean-first`): 278 objetos C compilados, 0 avisos (`PASS`).
  - Selftest `prj_04_adc_dma_bm_eth` (`--fresh`, `--clean-first`): 323 objetos C compilados, 0 avisos (`PASS`).
  - Control negativo del contador de avisos: inyección sintética detectada y rechazada (`PASS`).
  - `T1 VERDICT: PASS`.
- **T18 Regresión:**
  - Congelamiento de cabeceras (`gpio_bm.h`, `gpio_hal.h`, `gpio_ll.h`): solo adiciones respecto a `3df160a` (`PASS`).
  - Control negativo de cabecera en directorio temporal (mutación de firma de función): detectado y rechazado (`PASS`).
  - Control negativo de comentario con código en la misma línea: detectado y rechazado (`PASS`).
  - Handlers en driver: 0 handlers en `01_shared_drivers/00_gpio/` (`PASS`); control negativo con archivo `.c` en directorio temporal detectado (`PASS`).
  - Handlers en producción: 0 handlers `EXTI*_IRQHandler` en las 16 imágenes de producción (`PASS`); control positivo en `selftest-gpio` con 2 handlers detectados (`PASS`); control negativo con texto de `nm` mutado detectado por `parse_production_handlers` (`PASS`).
  - Estado estático desde mapa `.map`: CM7 `.data = 0 B`, `.bss = 8 B`; CM4 `.data = 0 B`, `.bss = 8 B` (`PASS`); control negativo con `.data = 4 B` mutado en texto detectado por `verify_driver_sizes` (`PASS`).
  - Paridad de símbolos `GPIO_BM_*`: 16/16 imágenes de producción idénticas en símbolos y tamaños al control `3df160a` analizadas con `nm -S` (`PASS`).
  - Control negativo 1 de símbolos: mutación de tamaño del símbolo real `GPIO_BM_CheckConfig` detectada por `compare_symbols` (`PASS`).
  - Control negativo 2 de símbolos: símbolo extra `+GPIO_BM_ExtiConfig` detectado por `compare_symbols` (`PASS`).
  - Comprobación cruzada host-firmware (N1): 29/29 patrones regex `expect()` del host validados contra las cadenas de formato sintetizadas del firmware (`PASS`).
  - Control negativo 1 de crosscheck: mutación de `CM7 count=` en firmware rechazada (`PASS`).
  - Control negativo 2 de crosscheck: mutación de `FW VERDICT=` en firmware rechazada (`PASS`).
  - Control negativo 3 de crosscheck: mutación de patrón en el host rechazada (`PASS`).
- **Veredicto general:** `T18 VERDICT: PASS`, `OVERALL VERDICT: PASS`.

### 4.2 Simulación del host y Analizador Lógico (`gpio_f2_mock_fdf7200.txt`)
- **Comando ejecutado:**
  `py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --mock-test --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_mock_fdf7200.txt`
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
- **Fase 3 (Control negativo de subconteo):** Detección inmediata de subconteo de firmware (199 vs 200) marcada correctamente como FAIL (`PASS`).
- **Fase 4 (Pruebas de error DWF y rutas N4/N5):**
  - Detección de error en `_check_dwf`: `PASS`.
  - Verificación de desbordamiento de buffer N5 en `DigitalInHelper.arm`: `PASS`.
  - Salida anticipada en bucles por flanco ante `fw_abort` (N4): `PASS`.
- **Veredicto general del mock:** `OVERALL MOCK VERDICT: PASS`, `HOST VERDICT=PASS`.

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

# 2. Flashear la imagen selftest-gpio correspondiente al commit fdf7200
py 03_firmware_templates/prj_01_template_dualcore/tools/flash.py --sn 0016000C4142501220353451 --preset selftest-gpio

# 3. Primera ejecución completa del banco de pruebas (T0 a T17)
py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --preset selftest-gpio --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_selftest_fdf7200_1.txt

# 4. Segunda ejecución completa del banco (verificación de repetibilidad)
py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --preset selftest-gpio --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_selftest_fdf7200_2.txt

# 5. Flashear la imagen de producción release
py 03_firmware_templates/prj_01_template_dualcore/tools/flash.py --sn 0016000C4142501220353451 --preset release

# 6. Ejecutar los 20 warm resets consecutivos de producción con el proyecto completo
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

F1 CORREGIDA 3 PARA AUDITORIA fdf7200
