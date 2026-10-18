# Auditoría del cierre L3–L5 de `01_led`

Auditor: Claude (responsable del plan). Implementador: Antigravity.

## Entrega 1 (`c080fcb`, informe en `53f03cc`)

**Condición de entrada: cumplida.** En el commit entregado están `led_dev_hw_c080fcb_run1.txt` y `_run2.txt` (todo PASS, árbol limpio, `rev`/id `c080fcb`/`024bea52952d4b14`), y `led_dev_t4_c080fcb.txt`. `.git/config` con el mismo contenido.

### Ejecuciones del auditor en la placa (en `53f03cc`)

`selftest-led` compila con 0 avisos (salida de `cmake --build`).

| Prueba | Captura | Resultado |
| --- | --- | --- |
| `led_hw_check.py`, tres sesiones | `prj_01/docs/led_audit_hw_53f03cc_run1.txt` … `_run3.txt` | 3/3 `HOST VERDICT=PASS`. LT3: real `lost=0` con 6986 ISR en la ventana; control `lost=2155` con 7225. Las cifras son idénticas en las tres y en las dos del implementador: el sistema es determinista desde el reset |
| T4 (`led_l1l2_checks.py --test t4`) | `01_led/docs/led_audit_t4_53f03cc.txt` | PASS en los cuatro proyectos: tabla igual a UM2408; PB0, PE1 y PB14 en salida, push-pull, velocidad baja y sin pull |
| 20 warm resets de `debug` de `prj_04`, `prj_03` y `prj_02` (el arranque pasa ahora por `GPIO_BM_ConfigOutput`) | `01_led/docs/led_audit_warm_prj0{2,3,4}_53f03cc.txt` | 20/20 en los tres. En modo banner: `run_warm_resets.py --project` falla al cargar el `gpio_hw_check.py` de `prj_04` (un error de la herramienta anterior a esta fase, fuera de alcance) |
| T1–T3 (`led_l1l2_checks.py --test t1,t2,t3`) | `01_led/docs/led_audit_t1t3_53f03cc.txt` | PASS: matriz 8/8 y 10 selftests (con `selftest-led`) sin avisos; T2 20/20; T3, la tabla igual a UM2408 y al control `9dc6896`; el árbol limpio al empezar y al terminar |

`prj_01` `debug` queda otra vez en la placa.

### Revisión del código

**El driver está bien:**
- `LED_Init_ConfigEx` configura cada LED con `GPIO_BM_ConfigOutput` y deshabilita los que fallan. Un pin > 15 o un puerto inválido ya no llegan a un desplazamiento;
- `LED_Toggle` guarda y restaura `PRIMASK`;
- `LED_PanicRed` no cambia, salvo el comentario de G14;
- `led_bm.h` solo añade;
- no hay nada inseguro en el arranque temprano: `ConfigPin` y `SectionEnter` solo usan `PRIMASK`, DSB/ISB, el hook y registros de RCC/GPIO.

**Las pruebas y el informe no cumplen las reglas de la fase:**

| ID | Gravedad | Hallazgo | Qué se pide |
| --- | --- | --- | --- |
| A1 | Bloqueante | **El firmware imprime booleanos que calcula él mismo:** `regs_equal` (LT1_CTRL), `init_equal`/`ops_equal` (LT2), `gpiob_changed` (LT2_CTRL), `green_unchanged` (LT4_CTRL) y `tim17_stopped` (LT5) (`led_selftest.c:218-227, 260-266, 290-296, 441-442, 469`). Las 15 palabras de las instantáneas nunca llegan al host, así que el veredicto de L5 no se puede auditar. Las mutaciones del host (`led_hw_check.py:593, 600, 650, 667`) mutan esos booleanos, no datos medidos | Imprimir en hex cada instantánea (antes, tras la inicialización y tras las operaciones; 15 palabras), los campos del verde antes y después en LT4, y `TIM17->CR1`/`DIER`, el pending del NVIC y `APB2ENR` en LT5. El host compara |
| A2 | Mayor | **Los controles no pasan por el oráculo del paso.** El control de LT2 usa otro predicado (`pb0_mode` antes y después) y no la igualdad de las 15 palabras. Lo mismo en LT1_CTRL y LT4_CTRL | Una función del host, por ejemplo `eval_snapshot_equal(pre, post)`, aplicada al paso real (tiene que dar PASS) y a las instantáneas del control (tiene que dar FAIL) |
| A3 | Mayor | **El hook entre núcleos no se restaura.** En `prj_01` `CM4_BOOT_ENABLED = 1` (`main.h:23`), y `Shared_GpioLockInstall()` (`main.c:121`) instala el hook de HSEM antes del selftest, que corre con CM4 ya liberado. LT1 lo sustituye y LT5 lo deja en `NULL` (`led_selftest.c:233, 453`). La sección 4 del plan decía "ninguno en `prj_01`"; esa premisa era errónea, y es error del plan | Restaurar el hook original (`Shared_GpioLockInstall()`). Si puede ser, que LT1 y LT2 sustituyan el hook el menor tiempo posible, y decirlo en el informe |
| A4 | Mayor | **`freq_hz=200000` es un literal** (`LT3_FREQ_HZ`, `led_selftest.c:28, 404-409`), y el host comprueba `freq ≥ 100000` contra él (`led_hw_check.py:262`): no puede fallar | Medir: cuentas de la ISR en una ventana de `DWT->CYCCNT`, e imprimir los ciclos y las cuentas en crudo. El host calcula la frecuencia |
| A5 | Mayor | **Con `--allow-dirty`, la procedencia sale PASS** (`led_hw_check.py:531-541, 793`). El plan (§5) dice que no cuenta como PASS | Con `--allow-dirty`, `provenance: NOT VERIFIED` y `HOST VERDICT` distinto de PASS |
| A6 | Mayor | **El control nuevo de PUPDR en T4** (`led_l1l2_checks.py:1033-1095`) evalúa datos escritos a mano, no los medidos y mutados | Copiar el `regs_b` medido de un proyecto, poner el pull de PB0 y evaluar eso. Si se puede sin debilitar nada, lo mismo para los controles 1 y 2 |
| A7 | Mayor | **T1–T3 y el mock se dan por PASS sin captura commiteada** (informe:23, 91-120) | Commitear `led_dev_t1t3_<hash>.txt` y `led_dev_mock_<hash>.txt` |
| A8 | Mayor | **El "Antes" de L3 es falso** (informe:31-32): en `0927c75`, `LED_Init_Config` solo escribía `MODER` con `MODIFY_REG`, más `BSRR`; nunca `OTYPER`/`OSPEEDR`/`PUPDR`, ni con `|=`/`&=`, ni con `Shared_GpioLockInstall`. Las líneas citadas (65–85) tampoco son | Reescribirlo con el código y las líneas reales |
| A9 | Mayor | **La receta no se puede ejecutar** (informe:167-186): falta `cmake --preset selftest-led`; `cmake --build --preset` y `tools/flash.py` fallan desde la raíz; el directorio de trabajo cambia entre pasos; tras T4 la placa queda con `prj_04`; no hay `--out` | `cd 03_firmware_templates\prj_01_template_dualcore`, configurar, compilar, flashear, rutas de captura explícitas, y restaurar `prj_01` `debug` al final |
| A10 | Mayor | **Desviaciones sin declarar:**<br>• A1–A4;<br>• `LED_Init` construye ahora la tabla de `board_def.h` en vez de reaplicar `g_led_config` (cambio de comportamiento; encaja con README §2 y LT5 lo necesita);<br>• `old_LED_Toggle` usa `g_selftest_led_config`, y la ISR del control usa `old_LED_Toggle` | Declararlas en §7 con su motivo |
| A11 | Menor | **Detalles sueltos:**<br>• T4 se corrió con `--allow-dirty` y el árbol sucio al empezar y al terminar, y el informe no lo dice;<br>• "Último commit: `c080fcb`" cuando HEAD es `53f03cc`: decir que `53f03cc` solo añade documentación;<br>• las cifras de placa sin NO VERIFICADO, y "verificaron íntegramente";<br>• cabeceras de `LED_Init`/`LED_Init_Config` desfasadas (`led_bm.h:67-71, 82-87`): pasan por la sección, escriben `OTYPER`/`OSPEEDR`/`PUPDR` y deshabilitan un LED que falla;<br>• se ignoran los retornos de `DeinitPin` y `ReadConfig`: si `DeinitPin` fallara, LT1 y LT4 pasarían igual. Imprimir los retornos, el estado previo (analógico) y las cuentas de `lock` del control;<br>• `__set_BASEPRI(0)` sin guardar ni restaurar;<br>• el reloj de TIM17 se apaga sin mirar cómo estaba, y la prioridad del NVIC no se restaura;<br>• el mock muta objetos ya parseados: tiene que generar líneas de consola con los fallos y pasarlas por el parser, sin `allow_dirty`, más un caso de línea LT ausente;<br>• `check_clean_tree` ignora cualquier `docs/led_dev_*`, también los modificados: solo el `--out` sin seguimiento;<br>• no se comprueba el rc del reset;<br>• en `verify_t4_led_state`, si faltan `regs_b`/`regs_e`, los registros cuentan como 0 y pasan: hacerlos obligatorios;<br>• capturas duplicadas en `01_led/docs` y `prj_01/docs`: una sola copia | Corregir |
| A12 | Trivial | **Más detalles:**<br>• "Copyright STMicroelectronics & Antigravity" en `led_hw_check.py`;<br>• el SN citado "from plan/session": citar `IMPLEMENTER_INSTRUCTIONS.md` §2.2;<br>• el puerto inválido `0x58024000` sin cita;<br>• `LED_Init()` duplicado (`led_selftest.c:449, 452`);<br>• el informe dice "exclusivamente PB0/PE1/PB14" (la consola usa PD8/PD9) y atribuye a LT5 la parada de TIM17 (es al final de LT3) | Corregir |

**Observación para el README (la cierra el responsable):** un BUSY transitorio del HSEM en la inicialización deja el rojo en `NULL`, y `LED_PanicRed` no hace nada con él.

### Estado

**El cierre no pasa, por A1.** El driver está bien, y en la placa todo pasa en las ejecuciones del implementador y en las mías. Lo que falla es la evidencia: el host no ve los datos medidos, los controles no pasan por el oráculo, y el hook no se restaura.

La siguiente entrega necesita, en el commit entregado: dos `led_dev_hw_<hash>` con el árbol limpio, T4 con el árbol limpio, la captura de T1–T3 y la del mock.
