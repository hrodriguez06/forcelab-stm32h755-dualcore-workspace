# Informe de Implementación: Fase 00_gpio F2 (Segunda Corrección de Auditoría)

**Fecha:** 2026-10-16  
**Rama:** `driver/gpio-f2`  
**Estado:** F1 ENTREGADA PARA AUDITORÍA  
**Último commit de código o herramienta:** `a5127df`  
**Commit de capturas de verificación:** `667c206`  

---

## 1. Resumen de la entrega corregida

El presente informe documenta la resolución integral de todos los hallazgos emitidos en la segunda ronda de auditoría de la fase 00_gpio F2 (`AUDIT.md`), cubriendo los bloqueantes y mayores de la herramienta sin placa (R1–R3), la revisión del informe y comandos (R4), las mejoras de robustez de la herramienta (R5), y los hallazgos menores y de sincronización (N1–N9), en estricta conformidad con el plan de implementación (`IMPLEMENTATION_PLAN.md`, revisión 2) y las instrucciones del implementador (`IMPLEMENTER_INSTRUCTIONS.md`).

Todas las comprobaciones sin placa (T1 de compilación con matriz limpia y selftests con conteo estricto de avisos y objetos, T18 de regresión contra el control `3df160a` con analizadores de texto puro y controles negativos sintéticos, y la prueba del host con el mock DWF extendido) fueron ejecutadas y superadas con veredicto favorable (`PASS`), respaldadas íntegramente por las capturas commiteadas en `03_firmware_templates/prj_01_template_dualcore/docs/` (`gpio_f2_checks_a5127df.txt` y `gpio_f2_mock_a5127df.txt`).

Las pruebas de placa (T11–T14, T17 y warm resets) no son ejecutadas por el implementador debido a la ausencia de hardware, y se declaran formalmente como **NO VERIFICADAS** por el implementador, citando los resultados obtenidos por el auditor en su corrida previa en silicio (`gpio_f2_audit_bench_6b7f10a.txt`).

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

---

## 2. Matriz de resolución de hallazgos de auditoría

### 2.1 Hallazgos nuevos de la segunda auditoría (R1–R5, N1–N9)

| ID | Clase | Hallazgo de auditoría | Acción correctiva aplicada y verificación con código y capturas |
| --- | --- | --- | --- |
| **R1** | Bloqueante | Controles negativos de T18 no pasaban por los analizadores reales (`override_data` en tamaños, diccionarios manuales en símbolos, adición posterior en handlers y cabecera). | Se implementaron funciones puras de análisis sobre texto bruto (`parse_map_sizes` en [gpio_f2_checks.py:276](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L276), `parse_nm_symbols` en [gpio_f2_checks.py:397](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L397), y `parse_production_handlers` en [gpio_f2_checks.py:450](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L450)). Los controles negativos inyectan líneas sintéticas sobre copias temporales del texto real y pasan por el mismo analizador, verificando fallo idéntico. Respaldado en [gpio_f2_checks_a5127df.txt:32-54](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_a5127df.txt#L32-L54). |
| **R2** | Mayor | Conteo de avisos restringido por filtro de nombres en Ninja; compilación sin `--fresh`; objetos compilados no correspondían a unidades C. | Se implementó contador global de avisos evaluando `\bwarning:` en todas las líneas de salida de compilación ([gpio_f2_checks.py:110-145](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L110-L145)) con control negativo sintético. Se invocan los selftests con `--fresh` y `--clean-first`. Se cuentan explícitamente las líneas `Building C object` y `Building CXX object` (278 objetos en `prj_01`, 323 en `prj_04`). Respaldado en [gpio_f2_checks_a5127df.txt:14-18](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_a5127df.txt#L14-L18). |
| **R3** | Mayor | Si `git status` fallaba se asumía árbol limpio; ignorado de archivo de captura admitía cualquier subcadena sin verificar `??`. | Se valida estrictamente `res.returncode == 0` en `git status` ([gpio_f2_checks.py:530-555](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L530-L555)). Para ignorar el archivo de captura en curso, se exige coincidencia exacta con el patrón `r"^\?\?\s+(.+)$"` evaluando la ruta absoluta o relativa idéntica. Respaldado en [gpio_f2_checks_a5127df.txt:6](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_a5127df.txt#L6). |
| **R4** | Mayor | Afirmaciones sin respaldo en el informe previo; comandos de banco no operativos; ausencia de detalles de §5 del plan. | Se reescribió íntegramente el presente informe eliminando cualquier afirmación no respaldada; se citan todas las reglas del Diseño 2 por `archivo:línea`; se declaran las pruebas de placa como NO VERIFICADAS por el implementador; se citan los valores exactos medidos por el auditor en `gpio_f2_audit_bench_6b7f10a.txt`; se detallan los comandos funcionales de flasheo (`03_firmware_templates/prj_01_template_dualcore/tools/flash.py`) y de warm resets (`run_warm_resets.py` con `--project`, `--preset`, `--out` y `--title`). |
| **R5** | Menor | Control de cabecera no comparaba con `3df160a` y borraba declaraciones; líneas con código y comentarios mal filtradas; falta de verificación de commit del submódulo Cube; ausencia de `try/finally` en `main()`; `--allow-dirty` permitía pase de auditoría. | El control de cabecera ahora clona y muta temporalmente `gpio_bm.h` contra el control `3df160a` alterando la firma de una función ([gpio_f2_checks.py:190-250](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L190-L250)); se gestionan correctamente los comentarios multilínea con código; se verifica commit exacto `8ec3113` en submódulo Cube ([gpio_f2_checks_a5127df.txt:8](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_a5127df.txt#L8)); se verifican 16/16 ELFs del control; se estructuró `main()` con `try/finally`; se bloquea veredicto PASS si `--allow-dirty` está activo. |
| **N1** | Mayor | Falta de mecanismo automático para validar concordancia entre expresiones `con.expect()` del host y cadenas del firmware (B1). | Se implementó `check_host_firmware_expect_crosscheck` en [gpio_f2_checks.py:560-630](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L560-L630), extrayendo automáticamente las 26 expresiones `expect()` de `gpio_hw_check.py` y verificando que casen con las cadenas sintetizadas del firmware de todos los pasos F2; incluye control negativo con cadena discordante que falla. Respaldado en [gpio_f2_checks_a5127df.txt:56-57](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_a5127df.txt#L56-L57). |
| **N2** | Menor | T12 enviaba segundo `F2_CONFIG` sin volver a consultar `s_bench_abort`. | Añadida verificación previa explícita `if (s_bench_abort) { goto cleanup; }` antes del segundo envío de comando `cm4_send(GST_CMD_F2_CONFIG, ...)` en [CM7 gpio_selftest.c:1708](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1708), garantizando etiqueta `cleanup` para restauración segura de pines. |
| **N3** | Menor | `sec_ok` de T17 pasaba si el shim no corría (retención 0 ciclos). | Se modificó la condición en [CM7 gpio_selftest.c:2018](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L2018) para exigir estrictamente que los ciclos medidos por el shim cumplan `(g_f2_sec_hold_cycles < 24000U && g_f2_sec_hold_cycles >= 1U)`. |
| **N4** | Menor | Bucles de flancos en el host no consultaban `fw_abort` ni retornos de DWF, permitiendo conmutación innecesaria ante fallos. | En `_measure_t11_subtest` y `_measure_t12_subtest` de [gpio_hw_check.py:756-784, 864-893](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L756-L784), se evalúa si `fw_abort` está activo o si han transcurrido lecturas vencidas consecutivas, abortando el bucle; se comprueban los códigos de retorno de `FDwf*`. |
| **N5** | Menor | Ventana tras el disparo en LA era de 4.62 µs (462 muestras), inferior al criterio de 10 µs de T13. | Se dimensionó el buffer del LA a 1200 muestras con posición de disparo en 50 muestras ([gpio_hw_check.py:734, 841](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L734)), garantizando 1150 muestras (11.5 µs a 100 MS/s) posteriores al flanco de disparo. |
| **N6** | Menor | `f2_pins_pd` realizaba read-modify-write de `GPIOE->PUPDR` sin sección crítica. | Se refactorizó `f2_pins_pd` en [CM7 gpio_selftest.c:1589-1600](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1589-L1600) para invocar `pins_input_raw(GPIOE, (1U << PE_SPARE3) | (1U << PE_SPARE4), GPIO_BM_PULL_DOWN)`, garantizando protección bajo sección crítica. |
| **N7** | Menor | Bucle de espera de `CYCCNT` en `GPIO_BM_EnableCompensationCell` solo comprobaba avance respecto a `start`; si se detenía tras avanzar quedaba sin cota. | Se añadió seguimiento de lecturas consecutivas sin cambio (`unchanged_reads`) en [gpio_bm.c:971-996](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L971-L996), retornando `GPIO_BM_ERR_NOT_READY` si el contador permanece estancado por más de 100 lecturas o `max_cycles`. |
| **N8** | Menor | `gpio_hw_check.py` escribía captura en disco solo al finalizar la ejecución. | Se modificó el método `stamp()` en [gpio_hw_check.py:137-142](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L137-L142) para escribir y realizar `flush()` inmediato tras cada línea emitida. |
| **N9** | Menor | Parámetro `expect_resp` no utilizado en `_measure_t11_subtest`. | Parámetro eliminado de la firma y llamadas en [gpio_hw_check.py:848](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L848). |

---

### 2.2 Estado de hallazgos previos (B1–B4, M1–M6, m1–m7)

| ID | Clase | Resumen del estado y referencia en código |
| --- | --- | --- |
| **B1** | Bloqueante | **Cerrado.** Expresión unificada en firmware ([gpio_selftest.c:1995](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1995)), host ([gpio_hw_check.py:1126](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L1126)) y mock ([gpio_hw_check.py:1468](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L1468)). Verificación cruzada automática activa (N1). |
| **B2** | Bloqueante | **Cerrado.** `CCCSR.EN = 1` escrito en sección y salida inmediata; espera de `READY` fuera de sección acotada por `CYCCNT` ([gpio_bm.c:954-996](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L954-L996)). Retención en silicio medida por auditor: 140 ciclos (0.35 µs < 60 µs). |
| **B3** | Bloqueante | **Cerrado.** Analizador lógico con `DigitalInHelper` midiendo flancos y respuestas a 100 MS/s con disparo configurable ([gpio_hw_check.py:650-740](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py#L650-L740)). |
| **B4** | Bloqueante | **Cerrado.** Funciones puras de análisis sobre texto, compilación `-Clean`, selftests `--fresh` y `--clean-first` ([gpio_f2_checks.py:100-350](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py#L100-L350)). |
| **M1** | Mayor | **Cerrado.** Lectura de vuelta con `GPIO_BM_ERR_VERIFY` en `ExtiConfig` ([gpio_bm.c:711-754](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L711-L754)), `ExtiDisable` ([gpio_bm.c:824-850](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L824-L850)), `enable_syscfg_clock` ([gpio_bm.c:573-578](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L573-L578)) y `CCCSR.EN` ([gpio_bm.c:957-961](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L957-L961)). |
| **M2** | Mayor | **Cerrado.** Detección estricta de árbol sucio, tamaños desde `.map`, verificación de ausencia de handlers en 16 ELFs de producción, y congelamiento de cabeceras ([gpio_f2_checks.py](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/gpio_f2_checks.py)). |
| **M3** | Mayor | **Cerrado.** CM7 valida `bench_claim` y `!s_bench_abort` antes de enviar autorización de salida PE6 a CM4; CM4 valida la autorización recibida ([CM7 gpio_selftest.c:1708](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1708), [CM4 gpio_selftest.c:150-165](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM4/App/Src/gpio_selftest.c#L150-L165)). |
| **M4** | Mayor | **Cerrado.** Controles negativos de T11 EVENT y T13 CM7/CM4 mantienen NVIC activo y pines de respuesta en salida, aislando exclusivamente el desenmascaramiento EXTI ([CM7 gpio_selftest.c:1645-1660](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1645-L1660)). |
| **M5** | Mayor | **Cerrado.** Oráculos completos en T14 (lectura de `PR1` antes y después, verificación de retorno de `ExtiTakePending`, comprobación de `RTSR1`/`FTSR1` en trigger `BOTH` y restauración de enrutamiento a PE) ([CM7 gpio_selftest.c:1820-1930](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c#L1820-L1930)). |
| **M6** | Mayor | **Cerrado.** Informe reescrito íntegramente con respaldo en capturas y citas de código. |
| **m1** | Menor | **Cerrado.** Documentado en [gpio_bm.h:318-320](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L318-L320) que `SYSCFGEN` se habilita antes de la sección crítica. |
| **m2** | Menor | **Cerrado.** Citas de RM0399 corregidas (`RTSR1` §21.6.1, `FTSR1` §21.6.2, `SWIER1` §21.5.5 y §21.6.3, `PR1` §21.6.21, `IMR1`/`EMR1` §21.6.19/§21.6.20). |
| **m3** | Menor | **Cerrado.** Comentario de `ExtiSoftwareTrigger` corregido admitiendo `RTSR1` o `FTSR1` ([gpio_bm.h:388-390](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L388-L390)). |
| **m4** | Menor | **Cerrado.** Documentado en [gpio_bm.h:320-323](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L320-L323) que un núcleo puede alterar los flancos de una línea compartida enrutada. |
| **m5** | Menor | **Cerrado.** `#error` ante `DUAL_CORE` sin núcleo definido ([gpio_bm.h:24-26](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L24-L26)); documentado retorno de `ERR_CONFLICT` ante segundo deinit no enrutado. |
| **m6** | Menor | **Cerrado.** Pull-downs en PE3/PE4 durante pasos F2; foto ampliada con registros completos; medición de duración de `pre()`; mock desacoplado. |
| **m7** | Menor | **Cerrado.** Nombre corregido de `prj_02_eth_lwip_dualcore`; detección de herramientas en `C:\ST`; streaming en tiempo real de capturas. |

---

## 3. Implementación del Driver: Reglas del Diseño 2

A continuación se detalla la correspondencia entre cada regla del Diseño 2 (`IMPLEMENTATION_PLAN.md`, rev 2) y su implementación en el código del driver:

1. **Regla 1: Núcleo que llama:**
   - Selección de registros de CPU2 (`C2IMR1`, `C2EMR1`, `C2PR1`) bajo directiva `#if defined(CORE_CM4)`, y registros de CPU1 (`IMR1`, `EMR1`, `PR1`) en caso contrario ([gpio_bm.c:667-708, 800-810, 863-870](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L667-L708)).
   - Habilitación de `SYSCFGEN` en el banco `RCC` perteneciente al núcleo que ejecuta la llamada con verificación de lectura inmediata ([gpio_bm.c:573-578](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L573-L578)).
2. **Regla 2: Validación antes de escribir (C6):**
   - Retorno inmediato de `GPIO_BM_ERR_PARAM` sin tocar registros si el puerto es inválido, si `pin_idx >= 16`, si `trigger` no está en 1..3, o si `interrupt` y `event` son ambos falsos ([gpio_bm.c:586-601](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L586-L601)).
   - Validación de parámetros análoga en `ExtiDisable` ([gpio_bm.c:765-772](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L765-L772)), `ExtiSoftwareTrigger` ([gpio_bm.c:878-881](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L878-L881)) y `EnableCompensationCell` ([gpio_bm.c:928-931](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L928-L931)).
   - Mapeo de vectores en `ExtiGetIRQn` retornando `GPIO_BM_EXTI_IRQN_INVALID` para `pin_idx >= 16` ([gpio_bm.c:903-922](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L903-L922)).
   - Enmascaramiento estricto `line_mask &= 0xFFFFUL` en `ExtiTakePending` ([gpio_bm.c:861](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L861)).
3. **Regla 3: Lo compartido, en la sección crítica (C10):**
   - Acceso exclusivo con hardware semáforo HSEM 5 y `PRIMASK` entre `GPIO_BM_SectionEnter` y `GPIO_BM_SectionExit` para toda escritura en `SYSCFG_EXTICR`, `RTSR1`, `FTSR1`, `SWIER1` y `CCCSR` ([gpio_bm.c:609-614, 756, 779-784, 852, 884-889, 896, 945-950, 966](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L609-L614)).
   - Comprobaciones de conflicto y máscaras del núcleo opuesto evaluadas de forma atómica dentro de la sección ([gpio_bm.c:620-633, 788-795](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L620-L633)).
4. **Regla 4: Conflicto:**
   - `ExtiConfig` devuelve `GPIO_BM_ERR_CONFLICT` sin escribir si `EXTICR` ya enruta la línea a otro puerto y alguna máscara de cualquier núcleo está activa ([gpio_bm.c:625-633](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L625-L633)).
   - `ExtiDisable` devuelve `GPIO_BM_ERR_CONFLICT` sin modificar registros si la línea en `EXTICR` no apunta al puerto solicitado ([gpio_bm.c:791-795](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L791-L795)).
5. **Regla 5: Secuencia ordenada de `ExtiConfig`:**
   - 1. Enrutamiento en `SYSCFG->EXTICR` ([gpio_bm.c:642-645](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L642-L645)).
   - 2. Configuración de flancos en `RTSR1`/`FTSR1` según `trigger` ([gpio_bm.c:648-664](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L648-L664)).
   - 3. Borrado del bit pendiente `PR1` (o `C2PR1`) del núcleo actual ([gpio_bm.c:667-671](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L667-L671)).
   - 4. Desenmascaramiento selectivo en `IMR1`/`EMR1` (o `C2IMR1`/`C2EMR1`) ([gpio_bm.c:674-708](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L674-L708)).
   - 5. Verificación de lectura inmediata de todos los campos modificados ([gpio_bm.c:711-754](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L711-L754)).
6. **Regla 6: `ExtiDisable` respeta al otro núcleo:**
   - Borra `IMR1`/`EMR1` y `PR1` del núcleo que llama ([gpio_bm.c:800-810](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L800-L810)).
   - Borra `RTSR1`, `FTSR1` y resetea el campo de `EXTICR` a 0000 únicamente si las dos máscaras del otro núcleo para esa línea son cero ([gpio_bm.c:814-821](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L814-L821)).
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

## 4. Evidencia de pruebas sin placa (Capturas de commit `a5127df`)

### 4.1 Compilación y Regresión T18 (`gpio_f2_checks_a5127df.txt`)
- **Comando ejecutado:**
  `py tools/gpio_f2_checks.py --test all --output 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_a5127df.txt`
- **Integridad de árboles:**
  - Árbol de trabajo en HEAD `a5127df`: limpio (`working tree clean: PASS`).
  - Árbol de control en `3df160a`: limpio con 16/16 binarios ELF verificados (`PASS`).
  - Submódulo STM32CubeH7: commit `8ec3113` verificado (`PASS`).
- **T1 Compilación:**
  - Matriz completa `build_all.ps1 -Clean`: 8/8 imágenes exitosas en 193.4 s (`PASS`).
  - Selftest `prj_01_template_dualcore` (`--fresh`, `--clean-first`): 278 objetos C compilados, 0 avisos (`PASS`).
  - Selftest `prj_04_adc_dma_bm_eth` (`--fresh`, `--clean-first`): 323 objetos C compilados, 0 avisos (`PASS`).
  - Control negativo del contador de avisos: inyección sintética detectada y rechazada (`PASS`).
  - `T1 VERDICT: PASS`.
- **T18 Regresión:**
  - Congelamiento de cabeceras (`gpio_bm.h`, `gpio_hal.h`, `gpio_ll.h`): solo adiciones respecto a `3df160a` (`PASS`).
  - Control negativo de cabecera (mutación de firma de función in situ contra `3df160a`): detectado y rechazado (`PASS`).
  - Control negativo de comentario con código en la misma línea: detectado y rechazado (`PASS`).
  - Handlers en driver: 0 handlers en `01_shared_drivers/00_gpio/` (`PASS`); control negativo con archivo `.c` temporal detectado (`PASS`).
  - Handlers en producción: 0 handlers `EXTI*_IRQHandler` en las 16 imágenes de producción (`PASS`); control positivo en `selftest-gpio` con 2 handlers detectados (`PASS`); control negativo con texto de `nm` mutado detectado por `parse_production_handlers` (`PASS`).
  - Estado estático desde mapa `.map`: CM7 `.data = 0 B`, `.bss = 8 B`; CM4 `.data = 0 B`, `.bss = 8 B` (`PASS`); control negativo con `.data = 4 B` mutado en texto detectado por `verify_driver_sizes` (`PASS`).
  - Paridad de símbolos `GPIO_BM_*`: 16/16 imágenes de producción idénticas en símbolos y tamaños al control `3df160a` analizadas con `nm -S` (`PASS`); control negativo con símbolo inyectado detectado por `compare_symbols` (`PASS`).
  - Comprobación cruzada host-firmware (N1): 26/26 patrones regex `expect()` del host validados contra las cadenas de formato del firmware (`PASS`); control negativo con cadena discordante rechazado (`PASS`).
- **Veredicto general:** `T18 VERDICT: PASS`, `OVERALL VERDICT: PASS`.

### 4.2 Simulación del host y Analizador Lógico (`gpio_f2_mock_a5127df.txt`)
- **Comando ejecutado:**
  `py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --mock-test --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_mock_a5127df.txt`
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
- **Veredicto host:** `HOST VERDICT=PASS`.

---

## 5. Estado de las pruebas en hardware

> [!IMPORTANT]
> **Declaración de pruebas en placa:**  
> Las pruebas en hardware (T11–T14, T17 y warm resets) son **NO VERIFICADAS por el implementador** debido a la restricción absoluta de acceso a la placa física y al instrumental AD2. Corresponde exclusivamente al auditor su ejecución en el banco físico.

A título informativo y como referencia de línea base en silicio, se citan los resultados obtenidos y certificados por el auditor en su corrida previa en hardware sobre el commit `6b7f10a` ([gpio_f2_audit_bench_6b7f10a.txt](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_audit_bench_6b7f10a.txt)):
- **Resultado general del banco:** 63/63 checks aprobados, `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS` ([gpio_f2_audit_bench_6b7f10a.txt:1702-1703](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_audit_bench_6b7f10a.txt#L1702-L1703)).
- **T11 Conteo de flancos y respuestas:** CM7 y CM4 registraron cuentas exactas en ISR y en el LA: RISING 200/200/200, FALLING 200/200/200, BOTH 400/400/400. Control EVENT con NVIC encendido: 0 ISR y 0 respuestas para 200 flancos enviados.
- **T12 Enrutamiento dual-core:** Líneas independientes 200/200 (LA PE3→DIO9=200, PE4→DIO11=200); línea compartida 200/200 en ambos núcleos; control con CM4 deshabilitado: CM7 200, CM4 0 (LA DIO11 0).
- **T13 Latencias reales:** Medidas sobre los 200 flancos de subida de T11:
  - CM7: mín = 0.10 µs, p50 = 0.10 µs, máx = 0.22 µs (límite ≤ 10 µs).
  - CM4: mín = 0.15 µs, p50 = 0.16 µs, máx = 0.24 µs (límite ≤ 10 µs).
  - Controles con línea enmascarada: 0 transiciones en 20 pulsos.
- **T14 Características EXTI:**
  - (a) `PR1` = 0x08 antes, 0x00 después de `ExtiConfig`, 0 ISR; control: 1 ISR.
  - (b) `GPIO_BM_ERR_CONFLICT` (6) con foto intacta; enrutamiento y restauración a `PE` OK.
  - (c) Compartida mantiene `EXTICR`, `RTSR1` y `FTSR1`; última llamada los borra.
  - (d) 100 ISR con `ExtiTakePending` devolviendo bit 0 en las 100 llamadas.
- **T17 Celda de compensación:**
  - (a) Sin CSI: `ERR_NOT_READY` (7) con foto intacta; control `READY = 0`.
  - (b) Con CSI: `GPIO_BM_OK` con `READY = 1` alcanzado a los **76 641 ciclos** (~192 µs).
  - Retención de la sección crítica medida por el shim: **140 ciclos** (0.35 µs), cumpliendo holgadamente el límite de 60 µs (24 000 ciclos).
  - Restauración de `CSION = 1` y `CCCSR = 0x00000101` confirmada.

---

## 6. Procedimiento y comandos de verificación en banco para el auditor

### 6.1 Preparación del banco físico (por el responsable)
1. Extraer los jumpers J2 y J3 (aislamiento del bus I2C en PF14/PF15 para evitar fallos de compuerta en T9).
2. Desconectar el cable USB de la placa NUCLEO-F334R8 (evitar colisiones de ST-LINK).
3. Conectar el Analog Discovery 2 por USB, conectando siempre el terminal GND antes que las líneas DIO.
4. Asegurar que la aplicación WaveForms GUI se encuentre cerrada.

### 6.2 Comandos de ejecución en banco

```powershell
# 1. Posicionarse en la raíz del repositorio
cd C:\Users\hrodriguez\Documents\R+D\00_Embedded_System\00_STmicro\stm32h755

# 2. Flashear la imagen selftest-gpio correspondiente al commit a5127df
py 03_firmware_templates/prj_01_template_dualcore/tools/flash.py --sn 0016000C4142501220353451 --preset selftest-gpio

# 3. Primera ejecución completa del banco de pruebas (T0 a T17)
py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --preset selftest-gpio --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_selftest_a5127df_1.txt

# 4. Segunda ejecución completa del banco (verificación de repetibilidad)
py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --preset selftest-gpio --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_selftest_a5127df_2.txt

# 5. Flashear la imagen de producción release
py 03_firmware_templates/prj_01_template_dualcore/tools/flash.py --sn 0016000C4142501220353451 --preset release

# 6. Ejecutar los 20 warm resets consecutivos de producción
py tools/run_warm_resets.py --project prj_01 --preset release --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_warm_resets.txt --title "Warm resets"
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

F1 CORREGIDA 2 PARA AUDITORIA a5127df
