# Auditoría de `03_tim` F4 (TIM3 y TIM7 de `prj_03` y `prj_04` por el driver)

Auditor: Claude (Claude Code), para Hector Rodriguez · Rama: `driver/tim-f4` · Plan: `IMPLEMENTATION_PLAN.md` revisión 2 (revisión 3 con esta auditoría) · Implementador: Antigravity (conversación "STM32H755 03_tim F4 implementacion")

Este archivo lo escribe solo el auditor. Un hallazgo se cierra cuando el auditor lo verifica.

## F1 (`554fd13`, herramienta en `da0fc87`)

Entrega: `4ed5d59..554fd13`.
- **Firmware:** H1 `648d262`, H2 `42c15a5`, H3 `52bee58`, H4 `47782f3`.
- **Herramienta:** `6517c15`, `58b4bdf`, `da0fc87`.
- **Capturas sin placa:** `312f9b7`.
- **Informe:** `554fd13`.

El implementador no tiene placa, y el informe declara NO VERIFICADO todo lo que la necesita (T3–T6, T8): correcto.

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| Alcance | Solo los archivos que permite el plan: los dos `adc_acquisition.c`, los dos `adc_selftest_hw.c`, `dma_selftest.c`, los dos `CM4/CMakeLists.txt`, `tools/tim_f4_checks.py`, capturas e informe. Ninguna cabecera de `03_tim` cambia |
| Builds en `554fd13`, árbol limpio (mi ejecución) | `build_all -Clean` 8/8. Los presets `selftest` y `selftest-adc` de `prj_04`, `selftest` de `prj_03` y `selftest-tim` de `prj_01`, compilados de cero: 0 avisos. `prj_03`/`prj_04` debug y release con `-Werror`: 0 avisos |
| Revisión del firmware | **Producción:**<br>• `prj_04`: `TIM_BM_EnableClock`, `TIM_BM_InitTimeBase` (PSC 0, ARR 399, ARPE 0), `TIM_BM_ConfigMaster(MMS_UPDATE)` y `TIM_BM_Start` al final, tras `DMA_BM_Start` y `ADC_BM_StartConversion`;<br>• `prj_03`: lo mismo con `TIM_HAL_InitTimeBase`/`TIM_HAL_ConfigMaster` sobre `s_htim3` y arranque con `TIM_BM_Start`;<br>• en los dos, `TIM3_Trigger_Init()` sigue antes de inicializar el ADC.<br>**`dma_selftest.c` T4:** TIM7 con `TIM_BM_*` y `TIM_BM_EV_UPDATE`, con `Stop`/`Deinit` en todos los caminos de error; después de T4 nadie más usa TIM7 (las líneas 749, 771 y 808 son configuraciones de T2 que deben rechazarse).<br>**`adc_selftest_hw.c`:** igual en los dos proyectos |
| Placa: mi script independiente, sin la herramienta del implementador (`tim_f4_audit_board_554fd13.txt`, `tim_f4_audit_board_control_04fb865.txt`, `tim_f4_audit_rate60_554fd13.txt`, `tim_f4_audit_rate60_control_04fb865.txt`, en `docs/` de cada proyecto) | Para `prj_04` y `prj_03`, imagen migrada y de control flasheadas una detrás de otra:<br>• **TIM3 por SWD:** `CR1 = 0x1`, `CR2 = 0x20`, `SMCR = DIER = PSC = 0`, `ARR = 0x18F` en las cuatro imágenes;<br>• **ritmo en 60 s** (consultas UDP `JSON` directas, solo con menos de 5 ms de ida y vuelta): `prj_04` control 505 631 Hz frente a migrada 505 818 Hz (+0,037 %); `prj_03` control 505 693 Hz frente a migrada 505 751 Hz (+0,011 %);<br>• **muestras por latido de `m4_hb`:** 50 001 (`prj_04`) y 49 994 (`prj_03`), frente a 50 000;<br>• `ovr`, `err` y `adc_ovr` a 0.<br>**La migración es equivalente al control.** (Una medida de 12 s dio 505 426 Hz y 507 208 Hz: es el ruido del reloj del PC con consultas de hasta 75 ms de ida y vuelta, no el firmware) |
| Revisión de la herramienta | Un subagente la revisó línea a línea; lo de abajo lo comprobé yo en el código |

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| B1 | Bloqueante | **T8 da PASS sin ver ningún veredicto** (`tim_f4_checks.py:1336-1339`, `1360-1363`): el criterio es `"PASS" in out and "FAIL" not in out`. Además:<br>• `monitor.py` se abre después de que `flash.py` resetea, con 12 s en total, así que pierde las líneas de CM7, que salen en ~0,1 s, y las de CM4, que llegan a los ~16–18 s;<br>• en `selftest` de `prj_04`, líneas parciales como `T1_M2M=PASS` bastan;<br>• no guarda la consola en la captura;<br>• ignora el resultado del flasheo final | Plan revisión 3, T8: monitor antes del flasheo (excepción acotada de G-S5) con `PYTHONIOENCODING=utf-8:backslashreplace`, hasta 45 s; las líneas exactas de veredicto, todas obligatorias; consola completa en la captura; el flasheo final comprobado |
| B2 | Bloqueante | **T4 nunca obtiene una muestra:** hace `json.loads` de la salida de `query_adc.py --json`, que imprime texto (`[0001] CH5: … Samples: N | …`), así que siempre falla. Sin datos, T6a tampoco sirve. Aunque se arreglara el análisis, seguiría mal:<br>• `--count 10` a 10 Hz dura ~1 s, nunca 10 s;<br>• con la salida canalizada, las marcas de tiempo del PC no valen;<br>• `adc_ovr` no aparece en esa salida;<br>• `.get(..., 0)` daría por buenos los campos que faltan | Plan revisión 3, T4: consultas UDP directas al 5001 con `perf_counter`, ventana de 60 s, equivalencia con el control y el oráculo de `m4_hb`; claves obligatorias. Mi script de auditoría (`tim_f4_audit_rate60_*`) es una referencia |
| B3 | Bloqueante | **T5 no puede pasar nunca:** busca "ALL CHECKS PASSED" o "4/4 packets" (`:1130`), que `net_check.py` no imprime (imprime `PASS  3/3 checks`). Tampoco lee los contadores `[net]`, aunque el docstring diga que sí | `returncode == 0` y `^PASS\s+3/3 checks`; luego `monitor.py --sn` sin reset, y en la última línea `[net]` exigir `rx_nobuf`, `tx_nobuf` y `dma_err` a 0 (y `acl_drop` en `prj_04`). Sin línea `[net]`, FAIL |
| M1 | Mayor | **T2b no ve las habilitaciones de reloj a mano:** `RAW_RCC_TIM_RE` (`:498-500`) no admite `)` tras `APB1LENR`, y `__HAL_RCC_TIM3/7_CLK_ENABLE()` ya no existe tras `-E`. El control en `04fb865` tiene `__HAL_RCC_TIM3_CLK_ENABLE()` (`prj_03` `adc_acquisition.c:58`) y `__HAL_RCC_TIM7_CLK_ENABLE()` (`dma_selftest.c:1001`), y su captura no los lista; pasó porque solo comprueba que cada archivo aparezca en algún hallazgo. Tampoco ve `++`/`--` ni `__HAL_TIM_*` sobre un handle. La lista de excepciones exime `main.c` y `stm32h7xx_it.c` de CM7 enteros, más de lo que dice el plan. (El firmware de `HEAD` no tiene ninguna de esas escrituras: solo falla la herramienta) | `(?:\s*\))*` tras `APB1LENR` y tras los registros; autoprueba con `__HAL_RCC_TIM3_CLK_ENABLE()`, `SET_BIT(RCC->APB1LENR, …)` y `++`; control por (preset, ruta relativa, tipo), con las dos líneas de RCC; excepciones solo para los archivos que justifica el plan (`stm32h7xx_hal_timebase_tim.c`, `tim_bm.c`, `tim_hal.c`, `stm32h7xx_hal_tim*.c`) |
| M2 | Mayor | **La comprobación de G-S1 es una tautología** (`:1272-1283`): cruza dos conjuntos escritos a mano sin mirar el código ni las ELF. El informe (§4.1) dice que verifica los ejecutables | Sacar los pines del código de los presets de T8 (por ejemplo, los `BOARD_*_PORT/PIN` y `analog_pin_*` de las unidades preprocesadas) y cruzarlos con la lista del banco; o decir en el informe que es manual |
| M3 | Mayor | Un árbol sucio o un control de otro commit no hacen fallar nada: `main` solo los imprime (`:1419-1435`); T3 `--control` y T8 no comprueban el commit del control; `get_git_info` ignora los archivos sin seguimiento (`-uno`), contra la regla del plan | Como en `gpio_f1b_checks.py` tras F1c: el commit del control comparado y un FAIL si no coincide, y un árbol sucio invalida la ejecución |
| M4 | Mayor | T6a: su ventana de T4 son 10,5 s desde que arranca el proceso, y T4 exige ≥ 10 s entre la primera y la última muestra, así que fallaría en falso. Una excepción de SWD aborta toda la ejecución en lugar de contar un reset fallido | Con la T4 nueva: ventana medida entre respuestas; excepciones por reset contadas como FAIL |
| M5 | Mayor | **El informe afirma cosas que las capturas no respaldan:**<br>• la tabla de T1 no coincide con la captura de T1 (p. ej. `.text` de `prj_01` debug CM7 30624 frente a 30632);<br>• "0 avisos" en `selftest-tim` de `prj_01` sin `-Werror` y sin un recuento que falle;<br>• "restauración verificada" cuando la herramienta ignora el resultado;<br>• "3/3" símbolos en `prj_03` (la captura dice 4/4), "inspección DWARF" (no hay), RM0399 "§34.4" (es §40), "PLL1Q" (el SYSCLK es PLL1P), "habilitación de interrupción TRGO" para TIM7 (es `DIER.UDE`, una petición de DMA) | Corregir; cada cifra, de una captura |
| M6 | Mayor (firmware, comentarios) | Los comentarios nuevos de los dos `adc_acquisition.c` dicen "genera UG bajo MMS=000, sin TRGO espurio": es falso, porque con `MMS = 000` el `UG` es la salida TRGO (plan revisión 2, Diseño 3). Están en castellano dentro de código comentado en inglés, y se perdió el comentario que explicaba el cálculo (APB1 200 MHz, PSC 0, ARR 399, 500 kHz, 50 muestras por periodo a 10 kHz) | Comentarios en inglés, verdaderos (el `UG` sí da un TRGO, inocuo porque el ADC aún no está arrancado) y con el cálculo de vuelta |
| m1 | Menor | En `adc_selftest_hw.c` (los dos), `Tim3_Prepare`/`Run`/`Halt` descartan el estado de `TIM_BM_*` con `(void)`: un fallo de `InitTimeBase` (por ejemplo, con `CEN = 1`) dejaría la configuración anterior sin aviso. El código original paraba el contador con `CR1 = 0` antes de reconfigurar | Comprobar el estado y hacer fallar la prueba |
| m2 | Menor | Herramienta:<br>• T3 enmascara a 16 bits y anuncia que las direcciones salen de `stm32h755xx.h` (están escritas a mano);<br>• el flasheo acepta una sola línea "Download verified" (debe haber dos, una por núcleo);<br>• `--control` fuerza `--flash` también en T4/T6a;<br>• `--test all` recompila todo y no deja `prj_03` debug en la placa;<br>• `proc.kill()` sin `wait`;<br>• los comandos del informe usan `>`, que en PowerShell 5.1 escribe UTF-16 | Corregir |

### Erratas del plan (del auditor), corregidas en la revisión 3

- **T4:** la ventana absoluta +1,327 % ± 0,2 % era demasiado estrecha para la deriva del reloj de esta placa; el control de `prj_04` midió 505 631 Hz en 60 s. Pasa a equivalencia con el control y al oráculo de `m4_hb`.
- **T8:** pedía `rev=<hash>` en los dos núcleos, imposible en `prj_03` y en los selftests de ADC, y la lectura de la consola desde el arranque choca con G-S5. Pasa a identidad por flasheo y a una excepción acotada de G-S5.
- La fila de riesgos del pulso de TRGO conservaba la justificación falsa. Corregida.

### Estado

**F1 no pasa.**
- **El firmware es correcto:** mi ejecución independiente lo muestra equivalente al control en registros de TIM3, ritmo del ADC y contadores.
- **La herramienta no puede dar un veredicto válido** en T4, T5 y T8 (B1–B3); T2b tiene un hueco (M1).
- **El informe afirma cosas sin respaldo** (M5), y los comentarios del firmware son falsos (M6).

Con la revisión 3: corregir B1–B3, M1–M6, m1 y m2; repetir T1, T2 y T7, y dejar T3–T6 y T8 listas para que las ejecute el auditor. Las capturas de `da0fc87` se quedan.

## F1, corrección (`7168361`, herramienta en `e9afdbb`)

Entrega: `765e2b3..7168361` (`1b01e9a` firmware, `42ed495` y `e9afdbb` herramienta, `284512a` capturas, `7168361` informe).

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| Revisión del firmware (`1b01e9a`) | **M6 cerrado:** comentarios en inglés, la afirmación correcta sobre `UG`/TRGO y el cálculo de PSC/ARR de vuelta. **m1 cerrado:** `Tim3_Prepare`/`Run`/`Halt` devuelven `bool` y se comprueban; `started` solo tras arrancar. Nuevo, menor: `Tim3_Prepare` llama a `TIM_BM_Stop(TIM3)` **antes** de `TIM_BM_EnableClock(TIM3)`. En `prj_03` el reloj de TIM3 de CM4 no está habilitado antes del selftest, así que el primer `Stop` escribe y lee TIM3 sin reloj; T8 lo ejercitará, pero el orden correcto es habilitar el reloj primero (k1) |
| Revisión de la herramienta | Un subagente la revisó línea a línea, con sondas que sustituyen el flasheo y la red por las capturas reales; lo de abajo lo comprobé yo en el código |

Los analizadores de veredicto ya son correctos con las capturas reales:
- T8 da PASS con la consola de `dma_t13_independent_04fb865.txt` y FAIL si falta un `OVERALL` o si dice `FAIL`;
- T4 da PASS con los pares JSON de mis capturas de 60 s y FAIL con 40 000 muestras por latido, `ovr = 1` o una clave que falta;
- T5 da PASS con mi captura de `prj_03` y FAIL sin línea `[net]` o con un contador distinto de 0.

La parte de la placa sigue siendo segura.

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| N1 | Bloqueante (errata del plan, del auditor) | `flash_image` exige `count("Download verified successfully") >= 2` (`tim_f4_checks.py:334-335`), como pedía la revisión 3. `flash.py` imprime esa línea una vez por llamada, así que **todo flasheo da FAIL**: T3/T4/T6a con `--flash`, T5, las seis imágenes de T8 y la restauración final | `returncode == 0` y una línea por llamada (plan revisión 4) |
| B2 | Bloqueante (parcial) | T4 no implementa la **equivalencia** migrada frente a control en la misma sesión (\|Δ\| ≤ 0,1 %), que es el criterio principal de la revisión 3. Su control negativo es una constante (`not (49750 <= 40000 <= 50250)`, l. 1164), que no puede fallar, y falta el de la tasa desplazada un 1 % | T4 flashea la migrada, mide, flashea el control, mide y compara, en una ejecución; controles negativos que pasen por la misma comparación |
| B1 | Mayor (parcial) | T8:<br>• el límite de 45 s depende del `--timeout` de `monitor.py`, que deja de contar si el puerto no vuelve, y entonces T8 se queda colgada;<br>• no separa la salida anterior al reset de la posterior;<br>• si `flash.py` falla de verdad, sigue flasheando la siguiente imagen (G-S7 pide parar) | Límite propio en la herramienta; contar solo las líneas posteriores al banner de arranque de CM7; parar ante un fallo real de `flash.py` |
| B3 | Mayor (parcial) | T5:<br>• no exige que existan `rx_nobuf`, `tx_nobuf` y `dma_err` en la línea `[net]` (si faltan, pasa);<br>• el monitor dura 5 s, y las líneas `[net]` salen cada ~5 s, así que puede fallar sin motivo | Claves obligatorias; monitor de 15 s |
| M1 | Mayor (parcial) | T2b:<br>• ahora **salta todas las unidades de CM7** (l. 680-683), más que antes y contra "excepciones solo las del plan": no ve `CM7/Core/Src/main.c:203 __HAL_RCC_TIM3_CLK_ENABLE()` ni una escritura futura de CM7;<br>• no detecta escrituras a TIM a través de un handle (`(&s_htim3)->Instance->CR1 \|= …`, `__HAL_TIM_SET_AUTORELOAD`, `htim->Instance->PSC = 0`), y la autoprueba no las cubre;<br>• algunas líneas se desvían en una | Escanear CM7 con las excepciones del plan (`stm32h7xx_hal_timebase_tim.c`); `main.c:203` es una habilitación de reloj legítima, que se lista como hallazgo conocido y explicado, no se oculta; detectar `Instance->REG` escrito y `__HAL_TIM_*` en `prj_03` fuera de `tim_hal.c`, con casos en la autoprueba |
| M2 | Mayor (no cerrado) | La comprobación de G-S1 busca `PE5` y similares en `board_def.h`, que solo aparecen en comentarios. Cruza dos constantes y da True | Quitarla y decir en el informe que la lista de pines es manual (la comprobé yo en la revisión del plan: PB1, canales internos, PD5/PD6), o derivarla de las unidades preprocesadas |
| M3 | Mayor (parcial) | `e9afdbb` ignora **cualquier** línea de `git status` que acabe en `docs/tim_f4_*.txt` (l. 268): también capturas commiteadas modificadas (` M`), borradas (` D`) o en el índice. Una captura editada pasaría por árbol limpio, y el informe (§6.3) dice "excluyendo exclusivamente" las nuevas | Ignorar solo líneas `??` de las capturas de esta ejecución |
| M5 | Mayor | El informe afirma cosas sin respaldo:<br>• "solventado íntegramente";<br>• las 8 compilaciones "con -Werror, cero advertencias" (`prj_01`/`prj_02` no usan `-Werror` y T1 no cuenta avisos);<br>• "2282 unidades CM4" (incluye unidades saltadas);<br>• dos confirmaciones de descarga;<br>• "excluyendo exclusivamente";<br>• la equivalencia de los registros del control, afirmada como hecho sin citar mi captura | Corregir; cada afirmación, de una captura |
| m2 | Menor | `--test all` sigue recompilando todo y no deja `prj_03` debug; `--control` sin `--flash` mide la imagen que haya, sin identidad; `--reset-count` menor que 20 sigue etiquetando "20/20"; T7 cae a `897bf8d` si el diff contra `main` no está vacío | Corregir |
| k1 | Menor (firmware) | `Stop` antes de `EnableClock` en `Tim3_Prepare` (arriba) | `EnableClock` primero |
| k2 | Menor (errata del auditor) | Mi primera auditoría corrigió "RM0399 §34.4" por "§40" para TIM3: es §41 (TIM2–TIM5; TIMx_CR2 en §41.4.2) | Citas a §41 en código, informe y plan (revisión 4) |

### Estado

**F1 no pasa.**
- **Por qué no pasa:** por N1, que es un error mío del plan e impide cualquier flasheo; por la equivalencia de T4, que falta; y por M1, M3 y M5.
- **El firmware sigue siendo correcto:** mi evidencia independiente de la primera ronda vale, porque `1b01e9a` solo cambia comentarios en producción y el manejo de errores en los selftests.
- **Siguiente ronda:** corregir N1, B1–B3, M1–M3, M5, m2, k1 y k2, y repetir T1, T2 y T7 sin placa.

## F1, corrección 2 (`1caa7c3`, herramienta en `1f8876b`)

Entrega: `bc7dfdd..1caa7c3` (`2936c1e` firmware, `1f8876b` herramienta, `57744b5` capturas, `1caa7c3` informe).

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| Revisión del firmware (`2936c1e`) | **k1 cerrado:** `Tim3_Prepare` habilita el reloj antes de `TIM_BM_Stop` en los dos proyectos. **k2 cerrado:** las citas de TIM3 son RM0399 §41 (§41.4.2 para TIMx_CR2) |
| Revisión de la herramienta | Un subagente la revisó línea a línea; lo de abajo lo comprobé yo en el código. Cerrados: N1, B1 (T8: límite propio, banner de CM7, parada ante un fallo real de `flash.py`), B3 (T5), M1 (T2b con CM7 y escrituras vía `Instance`/`__HAL_TIM_*`), M3 y la equivalencia de T4 |
| Sesión de placa, mía, con la herramienta del implementador | HEAD `1caa7c3`, imágenes recompiladas desde ese commit, SN `0016000C4142501220353451`. Por proyecto (`prj_04` y después `prj_03`): T3 `--flash`, T3 `--control`, T4 `--flash`, T5 `debug` y `release`, T6a `--flash`; al final, T8 desde `prj_04`. Capturas `tim_f4_audit3_*_1caa7c3.txt` en `docs/` de cada proyecto (13), cada una de una sola ejecución, con el flasheo verificado y la ruta de la imagen en la propia captura |

### Resultados en placa

| Prueba | `prj_04` | `prj_03` |
| --- | --- | --- |
| T3, migrada | PASS: CR1 0x1, CR2 0x20, SMCR 0, DIER 0, PSC 0, ARR 0x18F; la tabla alterada (ARR 0xFF) falla | PASS, mismos seis valores |
| T3, control (`stm32h755_control_04fb865`) | PASS, mismos seis valores | PASS, mismos seis valores |
| T4, 60 s: migrada → control → restauración de la migrada | 505 855,4 frente a 505 842,5 Hz: Δ 0,0025 %. Oráculo de la migrada: 49 923 muestras por latido. `ovr`, `err` y `adc_ovr` a 0 | 505 638,3 frente a 505 476,9 Hz: Δ 0,032 %. Oráculo de la migrada: 50 066 muestras por latido. `ovr` y `err` a 0 |
| T5, `debug` y `release` | PASS 3/3; `[net]` con `rx_nobuf`, `tx_nobuf`, `dma_err` y `acl_drop` a 0 | PASS 3/3; `rx_nobuf`, `tx_nobuf` y `dma_err` a 0 |
| T6a, 20 resets por SWD | **FAIL 11/20** (ver abajo) | **FAIL 14/20** (ver abajo) |
| T8 (`--project prj_04`, cubre los tres selftests) | `selftest`: `[m7_dma_test] OVERALL: PASS (T8-T11, T13)` y `[m4_dma_test] OVERALL=PASS (mask=0xFE/0xFE)`, con `T4_TIM7=PASS (tc=9,ht=10)`, en la migrada y en el control. `selftest-adc`: `HW RESULT PASS mask=2047/2047`, con `HW T4 BM:1 LL:1`, en las dos | `selftest`: `HW RESULT PASS mask=31/31`, con `HW T4 HAL:1`, en la migrada y en el control. La placa queda en `prj_03` `debug`, flasheo verificado |

El banner de cada arranque de T8 dice `Rev 1caa7c3` en la migrada. Las seis imágenes de T8 se flashearon con el monitor ya abierto (excepción de G-S5), y el único reset fue el del flasheo.

### T6a: falla la herramienta, no el firmware

En los 40 ciclos (20 por proyecto):
- T3 da los seis registros exactos;
- la tasa está entre 505 580 y 506 077 Hz;
- `ovr`, `err` y `adc_ovr` valen 0.

Todos los ciclos que fallan, fallan en el oráculo de `m4_hb`:
- T6a llama a T4 con una ventana de 10,5 s, y en ella `Δm4_hb` vale 106 o 107;
- un latido es el 0,94 % del cociente, más que la tolerancia de ±0,5 %;
- con 107 latidos el cociente sale entre 49 635 y 49 678, y en `prj_04`, ciclo 18 (Δt 10,539 s, 106 latidos), sale 50 292.

En los 40 ciclos, 50 000 cae dentro de [Δ`samples`/(Δ`m4_hb`+1), Δ`samples`/(Δ`m4_hb`−1)], que es lo que permite la cuantización.

El veredicto de T6a sigue siendo FAIL y cuenta: la prueba, tal como está, no puede pasar de forma fiable. Las causas son dos:
- **errata mía del plan:** no decía qué ventana usa T4 dentro de T6a;
- **la herramienta:** acortó la ventana sin adaptar el oráculo.

La revisión 5 del plan alinea A y B a un incremento de `m4_hb`. Repetiré T6a con la herramienta corregida.

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| R1 | Bloqueante (errata del plan, del auditor, y herramienta) | T6a: oráculo de `m4_hb` sin resolución en una ventana de 10,5 s (arriba) | Plan revisión 5: A y B alineados a un incremento de `m4_hb`, tolerancia de ±0,5 % sin cambios, controles negativos con los datos medidos del ciclo |
| B2 | Mayor (parcial) | Controles negativos de T4 que no pueden fallar:<br>• el de la tasa compara `rate_ctrl * 1.01` con `rate_ctrl` (siempre un 1 %, l. 1272-1283);<br>• el del oráculo compara el literal 40 000 con el intervalo (l. 1289-1298).<br>Ninguno pasa por la función que decide el veredicto real | Una función de equivalencia y otra de oráculo, usadas por la comprobación real y por los controles negativos con los datos medidos (`rate_mig * 1.01` frente a `rate_ctrl`; Δ`samples` × 0,8 / Δ`m4_hb`) |
| B4 | Mayor | T4:<br>• la imagen de control solo comprueba contadores, sin oráculo de `m4_hb` ni cordura de la tasa;<br>• sin `--flash`, mide como "migrada" lo que haya en la placa, y después flashea el control y "restaura";<br>• el resultado del flasheo de restauración se ignora (l. 1287) | Oráculo y cordura también en el control; T4 con control flashea siempre la migrada primero; la restauración es un caso que puede fallar |
| M2 | Mayor (no cerrado) | `T8_gs1_pin_safety_precheck` es un `[PASS]` constante ("verified manually"): no comprueba nada y suma un PASS al veredicto | Quitarlo de los casos; una nota informativa que no cuente |
| M5 | Mayor (parcial) | El informe afirma cosas sin respaldo:<br>• T8 descarta lo anterior al banner "`CM7 Booting`": el código busca `=== STM32H755` o `(Cortex-M7)`;<br>• T7 dice `prj_01` con "0 advertencias": la herramienta solo mira el código de salida (l. 1526);<br>• el estado de T3–T8 sigue "NO VERIFICADO" sin citar las capturas del auditor | Corregir. Los resultados de placa se citan de mis capturas `tim_f4_audit3_*_1caa7c3.txt` (y de T6a cuando la repita), como medidos por el auditor |
| m2 | Menor | • `--test all` recompila todo dentro de una sesión de placa;<br>• T5 llama a `monitor.py` sin límite de tiempo propio;<br>• el `--help` describe la revisión 3 del plan: T4 sin equivalencia, "-Werror" en todo y "verified 2/2 cores" en T5 (l. 1-32) | Corregir |
| k3 | Observación fuera de F4 | En `prj_03`, la respuesta JSON da `"ch": 351272992` (0x14F00020, la constante HAL `ADC_CHANNEL_5`, no el número de canal), en la migrada, en el control y en mis capturas anteriores. No viene de F4 | Tarea aparte; no bloquea F4 |

### Estado

**F1 no pasa todavía.**
- **El firmware está verificado en placa:** T3, T4, T5 y T8 pasan en los dos proyectos, con el control de `04fb865` en la misma sesión. Los datos de T6a muestran 40 arranques sanos.
- **Por qué no pasa:** el veredicto de T6a es FAIL (R1), y quedan B2, B4, M2, M5 y m2.
- **Siguiente ronda:** corregir R1, B2, B4, M2, M5 y m2, y repetir T1, T2 y T7 sin placa. Después repetiré T4 y T6a en los dos proyectos.
