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
