# Auditoría de `01_led` L1/L2

Auditor: Claude (Claude Code), para Hector Rodriguez · Rama: `driver/led-l1l2` · Plan: revisión 1

## F1 (`736948f`, código en `4c15fba`, herramienta en `2e6a15d`)

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| Revisión del código | **H1 y H2 correctos:**<br>• `led_bm.c` incluye `board_def.h` sin condición y tiene un `#error` por cada una de las seis macros, con el texto de `08_eth`, más el rango `> 15U` de los tres bits;<br>• los tres `board_def.h` pasan a `_BIT` con 0, 1 y 14, y los bloques de LED de `prj_02`–`prj_04` son idénticos.<br>La herramienta la revisó un subagente; lo de abajo lo comprobé yo |
| T1–T3, ejecutadas por mí | `led_l1l2_audit_t1t3_736948f.txt` (`01_led/docs/`): HEAD `736948f`, árbol limpio, **`OVERALL VERDICT: PASS`**.<br>• T1: `build_all -Clean` 8/8 y los 9 presets de selftest con 0 avisos (objetos de 278 a 327).<br>• T2: los 4 originales compilan y las 16 mutaciones dan su `#error`.<br>• T3: la tabla de los 8 builds es igual a la de UM2408 (`0x58020400/0`, `0x58021000/1`, `0x58020400/14`) y a la del control `9dc6896`, compilado en la ejecución; el byte mutado se rechaza |
| Avisos de la matriz, compilando yo | `build_all.ps1` no imprime la salida del compilador cuando la compilación pasa (B1), así que el recuento de T1 en la matriz no prueba nada. Lo hice directamente: `prj_01` y `prj_02` (los que no tienen `-Werror`), `debug` y `release`, con `--clean-first`: 271 y 315 objetos, **0 avisos** |
| T4 en la placa, ejecutada por mí | `led_l1l2_audit_t4_736948f.txt`, con las imágenes `debug` que acababa de compilar desde HEAD. En los cuatro proyectos: flasheo verificado, `g_led_config` en RAM igual a la tabla, y `MODER` de PB0, PB14 y PE1 en `01` (salida). Control negativo rechazado. **`OVERALL VERDICT: PASS`** |
| Seguridad de T4 | Revisada antes de la placa: `flash.py` de cada proyecto con `--sn`, y lecturas `-r32` en `mode=HOTPLUG` de la tabla y de los dos `MODER`. Nada más |

**El producto está verificado:** compila sin avisos, falla donde debe, tiene los mismos bytes que antes y funciona igual en la placa.

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| B1 | Bloqueante | El recuento de avisos de la matriz no puede fallar:<br>• `build_all.ps1:123` guarda la salida de `cmake --build` y solo la imprime si falla, y aun así solo la primera línea (l. 178-180);<br>• `led_l1l2_checks.py:279-281` cuenta siempre 0;<br>• el control con el aviso sintético (l. 314-326) no pasa por ese camino | Compilar los 8 configs directamente (`cmake --preset` y `cmake --build --preset --clean-first`), como el bucle de selftests, y pasar el control por el mismo camino de captura |
| M1 | Mayor | T2 escribe en el árbol: `compile_variant` corre con `cwd=proj_dir` (l. 390) y `-fstack-usage` deja `a-led_bm.su` en cada proyecto (ignorados por `.gitignore`; los borré yo) | `cwd` en el directorio temporal |
| M2 | Mayor | T3 y T4 usan lo que haya en `build/` sin comprobar que salga de HEAD ni de esta ejecución (l. 572-592, 715). El comando de T4 del informe (`--test t4`) flashearía cualquier cosa | T3/T4 compilan en la propia ejecución, o exigen T1 en ella, y comprueban el `FW_GIT_REV` de cada imagen |
| m1 | Menor | `build_all.ps1` escribe "All 8 builds PASSED" fijo (l. 216) aunque se salte un proyecto (l. 92-95), y la herramienta no cuenta las ocho líneas `-> PASS` | Contarlas, si se sigue usando |
| m2 | Menor | T1 no mira la salida del configure (l. 296) y no comprueba que `led_bm.c.obj` se compile en cada preset | Añadirlo |
| m3 | Menor | T2 busca el texto del error en cualquier parte de stderr (l. 399), y el control positivo no mira avisos | Buscarlo en la línea del `#error`; contar avisos |
| m4 | Menor | El control negativo de T3 pasa con entrada vacía (l. 626-635), y no se comprueba que la sección mida 24 B | Comprobar primero que la muestra sin mutar pasa y que el texto cambió; el tamaño, 24 |
| m5 | Menor | El control negativo de T4 solo muta la tabla, no los `MODER` (l. 752-754) | Añadir uno con un `MODER` mutado |
| m6 | Menor | El árbol limpio ignora cualquier `docs/led_l1l2_*` sin seguimiento, no solo la captura (l. 175), y solo se comprueba al empezar | Solo `?? <ruta exacta>`, al empezar y al terminar |
| m7 | Menor | Sin límite de tiempo en los subprocesos; T4 no registra el hexadecimal leído | Añadirlos |
| m8 | Menor | Los comentarios de los cuatro `board_def.h` (`prj_02`/`03`/`04` l. 70-73, `prj_01` l. 50-53) dicen que una máscara se "ignoraría en silencio"; ahora da `#error` | Corregir los tres de `prj_02`–`prj_04`; el de `prj_01` necesita el visto bueno del responsable |
| m9 | Menor | **El informe:**<br>• cita funciones que no existen (`check_build_output_warnings`, l. 73; `extract_and_verify_led_config`, l. 92); las reales son `parse_compiler_warnings` (l. 251) y `parse_and_verify_led_config` (l. 525);<br>• dice "verificación de hash" (l. 110), cuando `flash.py` hace `-v`;<br>• al comando de lectura (l. 112) le falta `sn=`;<br>• "0 avisos" en la matriz (l. 65) no tiene respaldo (B1) | Corregir; citar mis capturas como medidas del auditor |

### Estado

**F1 no pasa todavía, por la herramienta (B1, M1, M2) y el informe.**
- El cambio del driver y de los `board_def.h` está verificado:
  - por mí sin placa: T1–T3 y los avisos de `prj_01`/`prj_02`;
  - en la placa: T4 en los cuatro proyectos.
- Siguiente ronda: B1, M1, M2, m1–m7, m8 en `prj_02`–`prj_04` y m9. Repetir T1–T3 sin placa.

## F1, corrección (`af836ea`, herramienta en `411673b`)

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| Comentarios de `prj_02`–`prj_04` (m8) | Correctos: ahora dicen que una máscara o una macro que falta da `#error`. Una máscara de `_BIT` (por ejemplo `0x4000`) cae en el rango `> 15U` |
| T1–T3, ejecutadas por mí | `led_l1l2_audit_t1t3_af836ea.txt`: HEAD `af836ea`, **`OVERALL VERDICT: PASS`**.<br>• Los 8 configs se compilan ahora directamente con `--clean-first`, con los avisos contados en su salida real: 0 en todos, con 271/315/323/321 objetos.<br>• Los 9 presets de selftest, con 0 avisos.<br>• T2: 4 + 16, ahora sin escribir en el árbol; `git status --ignored` no muestra ningún `.su` nuevo.<br>• T3: PASS |
| Revisión de la herramienta | Un subagente con pruebas sobre copias. Resultados abajo. T4 sigue siendo segura |

### Estado de los hallazgos

| ID | Estado |
| --- | --- |
| B1 | **Parcial:**<br>• el camino real ya cuenta los avisos de la salida de cada compilación, y una salida vacía da FAIL;<br>• el control negativo (l. 426-447) es una cadena fija pasada a `evaluate_build_output`, con `built_objects=10` y `led_obj_found=True` escritos a mano: no pasa por `run_cmd` ni por la captura real (MA2) |
| M1 | Cerrado (el `.su` va al directorio temporal). La comprobación final del árbol no mira los archivos ignorados (l. 184) |
| M2 | **Parcial:**<br>• la revisión solo se comprueba en `prj_01` (por `fw_build_id.h`) y en `prj_04` (en la ELF);<br>• en `prj_02` y `prj_03` solo se mira la fecha, pero la función devuelve `rev=<HEAD>` (l. 338-344), y la captura muestra `rev=af836ea` sin haberlo comprobado (MA1);<br>• sus ELF no embeben la revisión |
| m1 | No cerrado para el control (l. 257) |
| m2 | Parcial: la salida del configure no se mira cuando pasa, y `led_bm.c.obj` vale con el log **o** el disco (l. 372) |
| m3, m5, m6, m7 | Cerrados |
| m4 | Parcial: la sección se trunca a 24 B (l. 667), así que 28 B o datos de más pasan |
| m9 | **Parcial:**<br>• las 6 citas de funciones de la herramienta están mal (por ejemplo `evaluate_build_output` es l. 284-305, no 262-280);<br>• **afirmaciones falsas:**<br>&nbsp;&nbsp;– la revisión de `prj_01` "y ELF" (l. 40);<br>&nbsp;&nbsp;– FW_GIT_REV comprobado en todas (l. 106);<br>&nbsp;&nbsp;– `git status --ignored` comprobado por la herramienta (l. 124);<br>&nbsp;&nbsp;– log "y" disco (l. 29);<br>&nbsp;&nbsp;– "exactamente 24 bytes" (l. 44);<br>• enlaces `file:///c:/` |

**Menores nuevos:**
- el `git status` del control se lee pero no se comprueba (l. 221-223);
- `rstrip(\"-dirty\")` quita un conjunto de caracteres, no un sufijo (l. 334);
- `code=0` está fijo en el log (l. 399, 420).

### Estado

**F1 no pasa todavía.**
- **El producto está verificado por mí, dos veces:**
  - T1–T3 en `736948f` y en `af836ea`;
  - los avisos reales;
  - T4 en la placa.
- **Lo que queda:** MA1 y MA2 de la herramienta, las citas y afirmaciones del informe (m9) y los menores.
- **Para esta fase, que es pequeña, el alcance de la siguiente ronda queda cerrado:** si MA1, MA2 y m9 quedan bien, F1 pasa, y los menores que queden se anotan en el README.

## F1, corrección 2 (`9546db3`, herramienta en `b9fbaad`)

La revisé yo, porque la ronda tenía el alcance cerrado.

| ID | Estado |
| --- | --- |
| MA2 | **Cerrado.** El control negativo compila un `.c` temporal fuera del repo, con un aviso deliberado, usando `run_cmd` y las flags de `prj_01`. Su salida real pasa por `count_built_objects` y `evaluate_build_output` |
| m9 | **Cerrado.** Las seis citas de funciones casan con HEAD (`evaluate_build_output` l. 293, `compile_variant` 537, `check_image_freshness_and_rev` 317, `parse_and_verify_led_config` 682, `check_clean_working_tree` 187, `run_cmd` 100), y los enlaces son relativos |
| Menores de una línea | Cerrados:<br>• el sufijo `-dirty`;<br>• `code=` con el código real;<br>• el `git status` del control;<br>• `led_bm.c.obj` en el log **y** en el disco;<br>• la sección de 24 B exactos |
| MA1 | **No cerrado, pero sin efecto en el producto.** Para `prj_02` y `prj_03` la herramienta añade `-DCMAKE_C_FLAGS:STRING=-DFW_GIT_REV=\"<HEAD>\"` al configurar y después \"lo comprueba\" en `CMakeCache.txt`: lee lo que ella misma escribió. Esos proyectos no usan esa macro y sus flags vienen de `cmake/common_flags.cmake`, así que el binario no cambia. Pero no es una prueba. La procedencia de esas imágenes la dan el árbol limpio al empezar y al terminar y la compilación dentro de la ejecución |

### Estado

**F1 pasa, con una condición para el cierre**, igual que F1c de `00_gpio`.
- **El producto está verificado por mí:**
  - T1–T3 en `736948f` y en `af836ea`;
  - los avisos reales a 0;
  - T4 en la placa en los cuatro proyectos.
- **El código** (`led_bm.c` y los `board_def.h`) no ha cambiado desde `4c15fba`, salvo los comentarios de `prj_02`–`prj_04`.
- **La condición de MA1, en el cierre:**
  - quitar la inyección de `CMAKE_C_FLAGS`;
  - escribir la revisión de `prj_02`/`prj_03` como \"no embebida\", con su procedencia (árbol limpio y compilación en la ejecución), sin darla por comprobada.

**El cierre:**
- MA1;
- el comentario de `prj_01` (`board_def.h:50-53`), aprobado por el responsable el 2026-10-16;
- `01_led/README.md` (§3 contrato sin valores por defecto, §4, §5 sin L1/L2 y con L5 parcial, §6 historia, validación con las capturas del auditor);
- después, la hoja de ruta, el borrado de los documentos de proceso y el merge.

## Cierre del implementador (`1bc32d6`, código en `824d989`)

- **MA1, cerrado:** la inyección de `CMAKE_C_FLAGS` y su lectura en `CMakeCache.txt` desaparecen. Para `prj_02`/`prj_03` la herramienta escribe que la revisión no está embebida y cuál es su procedencia, sin darla por comprobada (`led_l1l2_checks.py:357`). El mensaje quedó en español: copió el mío. Lo dejo así para que la captura `led_l1l2_checks_824d989.txt`, que lo contiene, siga casando con el código.
- **Comentario de `prj_01`:** `board_def.h:50-53`, ahora igual que en `prj_02`–`prj_04`.
- **README:** el contrato, los consumidores, las limitaciones, la validación con las capturas y la historia, comprobados contra el código y las capturas. Corregí yo tres cosas:
  - el commit inexistente `6687a7d` pasa a `04fb865`;
  - la cita `#L672-L704` pasa a `#L671-L703`;
  - la sección "Accepted Tool Minors", que listaba cosas ya arregladas, pasa a "Accepted Tool Limitations" con lo que de verdad queda: avisos del configure, frescura por fecha, revisión de `prj_02`/`prj_03` y ficheros ignorados.
- **Capturas:** en `docs/` quedan mis tres capturas y la final del implementador (`824d989`, `OVERALL VERDICT: PASS`). Las superadas se leen desde su commit.

**El cierre del implementador pasa.** Queda lo del responsable:
- la hoja de ruta;
- el borrado de los documentos de proceso;
- la retirada de `../stm32h755_control_9dc6896`;
- el merge.
