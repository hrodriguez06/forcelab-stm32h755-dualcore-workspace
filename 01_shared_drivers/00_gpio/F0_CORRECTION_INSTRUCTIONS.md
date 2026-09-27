# Instrucciones para corregir F0 de 00\_gpio

Sep 27, 2026 · auditor · referencia: `AUDIT.md`, "Auditoría de F0" (commit `c5a3f0c`)

La entrega de F0 (`8bb0884`) no pasó la auditoría. Este documento dice qué corregir, en qué orden, cómo tiene que comportarse cada cosa y qué voy a comprobar en la reauditoría.

Siguen vigentes todas las reglas de [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md): no auditas, no tocas `AUDIT.md`, `IMPLEMENTATION_PLAN.md`, `DRIVER_ROADMAP.md` ni este archivo, y las capturas no se editan. El plan cambió en `b7b6340` y en este commit: **léelo otra vez** antes de empezar. Cambiaron S4, "Selftest", T0a, T0b y T15.

---

## 0. Antes de empezar

1. **Trabaja sobre `driver/gpio` desde `c5a3f0c` o posterior.** No reescribas la historia entregada: nada de `rebase`, `--amend` ni `push --force`.
2. **Una corrección por commit, citando el ID:** `fix(tools): gate every bus=AD2 drive on s5_verify_released (B3)`. Mensajes escritos desde bash con heredoc.
3. **El AD2 no se usa hasta terminar el paso 1 de §1** (B3 y B4) **con sus pruebas unitarias en verde.** Mientras tanto, el AD2 queda en S2 o desconectado de la placa. Las partes del selftest que no necesitan el AD2 (T15 en la versión nueva, §2.4) sí se pueden ejecutar.
4. **No borres las capturas viejas** (`gpio_f0_selftest_a692afc.txt`, `gpio_f0_t18_60b1ac9.txt`). Quedan como historia. El informe dirá que quedan sustituidas y por cuál.

---

## 1. Orden de trabajo

| Paso | Hallazgos | Por qué en este orden |
| --- | --- | --- |
| 1 | B3, B4 (+ m3, m6) | Es la seguridad eléctrica: sin ella no se puede volver a conectar el AD2 |
| 2 | M1, M2 (+ m1) | Guardas del firmware: la otra mitad de S1–S10 |
| 3 | B2 | Veredictos calculados: sin ellos, ninguna prueba de hardware vale |
| 4 | B1 (+ m4) | T15 rehecho |
| 5 | B5 | Referencia T18 regrabada |
| 6 | M3 (+ m2, m5, m7) | Informe y menores |
| 7 | Ejecución completa en la placa y entrega | §4 |

---

## 2. Qué corregir, y cómo tiene que quedar

### 2.1 B3: el host cumple S8, S10, S5 y S7 (`gpio_hw_check.py`, `tools/ad2.py`)

**S8, banner primero.** Desde que se abre el AD2 hasta que el banner está validado, `OE = 0`: ninguna llamada a `ad2_set_oe` con máscara distinta de 0. Para dar el banner por validado:

- Una línea `[m7_gpio_test] Rev <hash>` cuyo `<hash>` es exactamente el de `git rev-parse --short HEAD` del repositorio, sin `-dirty`.
- Una línea `[m7_gpio_test] GPIO_BENCH v<n> idle` cuya `<n>` es la versión de protocolo del script.
- Si falta cualquiera de las dos o no coincide: S2, `ABORT: banner` y código de salida ≠ 0.

`HOST_REQ` (DIO12) solo se habilita después de validar el banner.

**S10, reset seguro.** Una función `reset_mcu_safe(ad2)` que hace el read-back de `OE`, se niega a resetear si no es 0 (lanza una excepción) y solo entonces llama a `STM32_Programmer_CLI -rst`. El log dice lo que pasó de verdad, con el valor de `OE` leído. Nada de "under S2" si no se ha comprobado.

**S5, una sola función que protege el bus: `s5_verify_released(ad2, step)`.**

1. Comprueba por read-back que `OE & 0x00FF == 0`.
2. Espera el anuncio `BUSCHK up` del paso `step` y lee el bus: tiene que ser 0xFF.
3. Espera `BUSCHK down` y lee: tiene que ser 0x00.
4. Si algo no cuadra: S2 (`OE = 0` con read-back), log `ABORT: bus contention check failed at <step>` y excepción `BenchAbort`.
5. Si todo cuadra, devuelve un objeto testigo ligado a `step`.

Toda transmisión "bus = AD2" pasa por una única función, `drive_bus(ad2, token, value)`, que exige un testigo válido de **ese mismo paso**. Sin testigo, no hay bus. *(Errata E6, reauditoría 1: antes decía que el testigo se consumía en cada transmisión.)* El testigo:

- solo lo crea `s5_verify_released`; ningún otro código puede fabricarlo;
- autoriza todas las transmisiones de su paso;
- deja de valer en cuanto el seguimiento de pasos (S7) sale de ese paso, y ante cualquier aborto;
- `drive_bus` toma el dueño del bus del paso validado, no de un argumento. Los pasos "bus = MCU" también llaman a `s5_verify_released` antes de dar el `HOST_REQ` que autoriza al MCU a conducir.

**S7, tabla de pasos compartida.**

- **Firmware:** `gpio_bench_protocol.h` define, para cada paso, su ID y su dueño del bus. El firmware anuncia cada paso **con un formato fijo:** `[m7_gpio_test] STEP id=<n> bus=<NONE|MCU|AD2> v=<proto>`.
- **Host:** tiene el espejo de esa tabla y la **secuencia esperada**, en una lista ordenada. Reconoce los anuncios con una expresión regular sobre ese formato, no con subcadenas.
- **Qué comprueba el host:** que el ID sea el siguiente de la secuencia y que el dueño del bus coincida con su tabla.
- **Si no coincide, o llega un paso inesperado:** S2 y aborto.
- **El `bus_owner` que se pasa a `ad2_set_oe`** sale del anuncio validado, nunca de una cuenta del host (como el actual `b < 8`).
- **Timeouts:** cualquier `wait_mcu_ack` que agote el tiempo lleva a S2 y a aborto. Nunca se sigue.

**Pruebas unitarias sin hardware** (`prj_04/tools/test_gpio_hw_check.py`, `unittest` de la biblioteca estándar, con una DLL simulada que registra las llamadas):

| Caso | Tiene que pasar |
| --- | --- |
| S8a | Ninguna llamada a `FDwfDigitalIOOutputEnableSet` con máscara ≠ 0 antes de un banner válido |
| S8b | Banner con otro hash, con `-dirty` u otra versión → aborto, `OE` = 0 |
| S10 | `reset_mcu_safe` con `OE` ≠ 0 → excepción y ninguna llamada al CLI (simula `subprocess`) |
| S5a | `BUSCHK up` ≠ 0xFF o `down` ≠ 0x00 → `BenchAbort`, S2, y **ninguna** llamada a `OutputEnableSet` con bits del bus |
| S5b | `drive_bus` sin testigo, o con el testigo de otro paso → excepción antes de la DLL |
| S7a | Anuncio con un ID fuera de secuencia, o con el dueño del bus equivocado → aborto |
| S7b | Timeout de handshake → aborto (no sigue) |

La salida de `py -m unittest` va en el repo (`docs/gpio_f0_unittest_<hash>.txt`).

**Menores de esta parte:**

- **m3:** la lista S9 solo se imprime marcada si se pasa `--checklist-confirmed`; sin la opción, el script se detiene. El texto dice "declarado por el operador".
- **m6:** `digital_io_write` conserva el valor de las salidas que están fuera de la máscara. Lleva la cuenta del valor actual y hace lectura-modificación-escritura.

### 2.2 B4: T0b(c) prueba la guarda de verdad

- El paso T0b(c) del host llama a la **misma** `s5_verify_released()` de §2.1, mientras el MCU conduce 0x5A con el AD2 en entrada.
- **PASS** solo si se cumplen las dos cosas: salta `BenchAbort` de esa función, y el log de read-back de `OE` muestra 0 en el bus durante todo el paso.
- Después, el host avisa al MCU por el handshake de que abortó. El MCU llama a `bench_safe()`, comprueba que todos los pines cableados están en entrada y lo anuncia.
- **La rama que espera 0x5A desaparece.**

### 2.3 M1 y M2: guardas del firmware (`CM7/CM4 gpio_selftest.c`)

**M1, el paso lo conoce la guarda.**

- Hay una única variable estática del paso actual en cada núcleo. Solo la escribe `bench_enter_step(id)`, que también imprime el anuncio `STEP id=… bus=…`.
- `bench_claim_output(port, pin)` **pierde el parámetro `step`** y lee esa variable.
- Los pines permitidos por paso salen de una **tabla constante**, sin `if` sueltos.
- En CM4 lo mismo: su único paso es el de T15.

**M2, S2 en todo error.** Nadie deja un pin a medio configurar:

- `bench_claim_pins(port, mask, mode)` valida **todos** los pines antes de escribir ninguno. O se reclaman todos o ninguno (m1).
- Cada `return false` de una prueba pasa antes por `bench_safe()`. Una forma sencilla: un único punto de salida por prueba.

**T0b(b) amplía sus casos:**

- Reclamar PA8 en el paso T0a de sueltas → rechazado.
- Reclamar PA8 en el walking-1 MCU→AD2 → aceptado.
- Reclamar el bus en un paso con dueño "AD2" → rechazado.
- `bench_claim_pins` con una máscara que tiene un pin prohibido → rechazo y fotos de `MODER`/`OTYPER`/`AFR` sin cambios.

### 2.4 B2: veredictos que pueden fallar

- **CM4:** `cm4_all_passed` se calcula a partir de los resultados de T15 (§2.5). Se acaba el `cm4_ok = true`.
- **T0a MCU→AD2 (host):**
  - Conjunto observado: `bus | DIO9 | DIO11 | DIO14`, sin DIO13, que es el ACK.
  - En cada paso, `(dio & conjunto) == bit esperado`; cualquier otro valor, incluido un bit de más, es FAIL.
  - Control: `ODR` con el patrón y los pines en **entrada con pull-down** → el AD2 lee 0x00 (fila T0a del plan).
- **T0a AD2→MCU (firmware):**
  - Conjunto observado: `PE3 | PE4 | PE8..PE15`.
  - En cada paso el MCU lee `IDR & conjunto`, lo compara con lo esperado y lo anuncia: `[m7_gpio_test] T0a_ad2 bit=<n> idr=0x…. exp=0x…. OK|FAIL`.
  - El veredicto de la prueba sale de esas comparaciones. El host lee esa línea y también da FAIL si no es OK.
  - Control: el AD2 conduce 0xFF con el bus del MCU en analógico → `IDR` = 0x00.
- **Handshake (PE2 y PE7):**
  - Un paso específico: el host sube `HOST_REQ` y el MCU anuncia que lo leyó a 1; el host lo baja y el MCU anuncia 0.
  - Lo mismo para `MCU_ACK` al revés: el MCU lo sube y el host lo lee.
- **El control en analógico de las sueltas** ("DIO read 0x01") se elimina, o se convierte en uno de los controles anteriores.

### 2.5 B1: T15 rehecho (plan: fila T15 y sección "Selftest")

1. **Inicialización en cada arranque.** En el `main.c` de CM7, dentro de `#if GPIO_DRIVER_SELFTEST` y **antes** de `HSEM_BM_BootReleaseCpu2`: `memset` de todo el bloque `GPIO_TEST_SHARED` a 0, y escritura de un `session_id` nuevo, por ejemplo `DWT->CYCCNT` o un contador en la sección no inicializada. El `current_step` y el `bus_owner` que hoy son basura se eliminan o se escriben.
2. **T15 va antes que el resto,** justo después del banner y **antes** de esperar al host. Así no depende del AD2 y los dos núcleos están cerca en el tiempo.
3. **Esperas medidas en tiempo, no en vueltas.** Se usa `CYCCNT`, que ya arranca `HSEM_BM_TimebaseStart` en los dos núcleos, con un límite escrito, por ejemplo 5 s. Todas las banderas de sincronización llevan el `session_id`: por ejemplo, `go_signal = session_id`. Si una bandera no lleva el `session_id` actual, no cuenta.
4. **Estado conocido antes de empezar.** CM7 pone PF10 y PF11 en entrada (`00`) antes de dar `go`. La comprobación "los bits del otro pin son `00` o `01`" solo tiene sentido a partir de ahí.
5. **Tres fases, cada una con su resultado impreso:**
   - (a) CM7 solo, con CM4 esperando: 0 corrupciones.
   - (b) CM4 solo, con CM7 esperando: 0 corrupciones.
   - (c) Los dos a la vez: más de 0 corrupciones en total.
6. **Solapamiento.** Cada núcleo marca `in_loop_cmX` mientras corre. En (c) cuenta las iteraciones en las que vio activo el `in_loop` del otro. Las dos cuentas tienen que ser > 0. Si (c) da 0 corrupciones **con** solapamiento, se ensancha la ventana. Nunca se da por buena.
7. **Veredicto T15 = (a) == 0 y (b) == 0 y (c) > 0 y solapamiento > 0 en los dos núcleos.** Se imprime cada número.

**m4:** el selftest de CM7 corre en **su propia tarea** de FreeRTOS, creada en `app_main` bajo la opción y con prioridad baja, no dentro de `Status_Task`. El monitor de IPC sigue funcionando mientras tanto. Documenta en el informe que CM4 queda esperando durante T15 y que su heartbeat se para ese tiempo.

**La prueba mínima que voy a repetir:** SRAM4 con el bloque a cero (por SWD), reset, y T15 tiene que pasar sin el AD2. Luego, un segundo reset sin tocar SRAM4: tiene que dar lo mismo. Eso demuestra que la inicialización funciona.

### 2.6 B5: referencia T18 regrabada

Con la imagen de producción (`debug`, opción apagada) del commit que entregues:

1. `py tools/monitor.py --timestamps --log docs/gpio_f0_t18_<hash>_console.txt` durante toda la sesión, en una terminal propia.
2. `py tools/net_check.py > docs/gpio_f0_t18_<hash>_net.txt`.
3. `py tools/query_adc.py --wave --count 300 > docs/gpio_f0_t18_<hash>_wave.txt`. Los 300 `WAVE` seguidos que pide T18.
4. `STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 0x380084E0 32 > docs/gpio_f0_t18_<hash>_swd.txt`, al terminar.

Cada archivo es la salida tal cual de su comando. **Sin encabezados, sin "Result: PASS", sin ediciones.** Si quieres un índice, va en el informe: comando exacto, archivo, hash y hora. El informe dice explícitamente que la imagen de producción de ese commit tiene el mismo `.map` que `ca569b6`, y cómo lo comprobaste.

### 2.7 M3 y menores de documentación

- **M3:** en `IMPLEMENTATION_REPORT.md`, una **sección nueva** "Respuesta a la auditoría de F0". La sección F0 anterior no se toca. La sección nueva lleva:
  - una tabla con cada hallazgo (B1–B5, M1–M3, m1–m7): commit que lo corrige, qué ejecutaste para comprobarlo y estado "corregido, pendiente de verificación";
  - las afirmaciones falsas de la sección anterior, cada una con su corrección: "Desviaciones: Ninguna", T18 con `WAVE` y LED, T0a AD2→MCU "leído correctamente" y el oráculo de T15;
  - las desviaciones de verdad, por ejemplo el paso falso de PA8, que ahora cubre la errata E1.
- **m2:** `GPIO_TEST_SHARED_ADDR` se define en un solo sitio. Además, un `_Static_assert` o un símbolo del linker garantiza que `.shared_ram` más el área de HSEM no llegan a `0x3800A000`.
- **m5:** la salida completa de `tools/build_all.ps1 -Clean` va en el repo (`docs/gpio_f0_build_<hash>.txt`).
- **m7:** vuelve a poner el comentario de `01_shared_drivers/CMakeLists.txt` que explica por qué `00_gpio` va en la base de la pila.

---

## 3. Cómo ejecutar en la placa, cuando toque (paso 7)

Solo cuando los pasos 1–6 estén hechos y las pruebas unitarias de §2.1 pasen:

1. **Procedimiento operativo S9** del plan, con `--checklist-confirmed`.
2. **Una sola ejecución** de `gpio_hw_check.py` con la imagen `selftest-gpio` del commit entregado. La captura es `docs/gpio_f0_selftest_<hash>.txt` y cubre desde la lista S9 hasta `HOST VERDICT=`.
3. **Orden de las pruebas en esa ejecución:** T15, luego T0b (a, b, c, d) y luego T0a (sueltas, handshake, walking-1 con sus controles, scope). Si T0b falla, el script aborta sin llegar a T0a.
4. **Una segunda ejecución tras otro reset,** sin tocar SRAM4 (§2.5): una captura más.
5. **Si algo aborta o sale raro:** para, deja el AD2 en S2 y apúntalo en el informe (§5.8) antes de repetir.

---

## 4. Qué entregar

1. El rango de commits nuevo en `driver/gpio`, con el árbol limpio.
2. `docs/gpio_f0_build_<hash>.txt`: `build_all -Clean` 8/8.
3. `docs/gpio_f0_unittest_<hash>.txt`: las pruebas del host.
4. Las dos capturas del selftest (§3) y los cuatro archivos de T18 (§2.6).
5. `IMPLEMENTATION_REPORT.md` con la sección "Respuesta a la auditoría de F0".
6. Un aviso con el hash. Luego esperas a "Reauditoría 1" en `AUDIT.md`.

---

## 5. Qué voy a comprobar en la reauditoría

| Hallazgo | Cómo lo compruebo | Se cierra si |
| --- | --- | --- |
| B3 | Leo el código del host, ejecuto yo las pruebas unitarias y hago dos pruebas en la placa: arranco el script con la imagen de producción (sin banner válido) y con una imagen de otro commit | Con banner inválido: aborta con `OE` = 0 y sin resetear con `OE` ≠ 0. Toda transmisión del bus pasa por un testigo de `s5_verify_released` |
| B4 | Ejecución con el AD2 (A18), y el control de la DLL simulada | T0b(c) aborta dentro de `s5_verify_released` y el bus nunca se habilita |
| M1, M2 | Lectura del código + T0b(b) ampliada | Sin parámetro `step` en la guarda; reclamación todo-o-nada; `bench_safe()` en todo error |
| B2 | Rompo a propósito una comparación (en la DLL simulada, y en el firmware con una build de prueba mía que invierte un bit esperado) | Esa prueba y el veredicto global dan FAIL |
| B1 | La prueba mínima de §2.5 hecha por mí: SRAM4 a cero y reset, y luego reset sin tocar | T15 pasa las dos veces; (a) = (b) = 0; (c) > 0; solapamiento > 0 en los dos núcleos |
| B5 | Los cuatro archivos contra sus comandos | Salida cruda; 300 `WAVE`; contadores de SWD a 0 |
| M3 | El informe contra el código y las capturas | Ninguna afirmación sin fuente ni con lenguaje de veredicto |
| A18, A27 | Repito yo la ejecución completa con el AD2 | Mis resultados coinciden con los tuyos, con el mismo hash |

---

## 6. Cuándo parar y preguntar

Las mismas situaciones que en `IMPLEMENTER_INSTRUCTIONS.md` §7, y además estas:

- Algo de §2 parece exigir que el MCU y el AD2 conduzcan la misma línea.
- No encuentras la forma de que T15 (c) solape en los dos núcleos.
- Necesitas cambiar un criterio o una prueba del plan.
