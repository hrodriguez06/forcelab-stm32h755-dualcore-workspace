# Auditoría del driver GPIO 00\_gpio

Este archivo lo escribe solo el auditor (`IMPLEMENTATION_PLAN.md`, "Reglas del registro"). El implementador no lo edita.

---

## Auditoría de F0

Sep 27, 2026 · auditor: Claude (el mismo del plan) · commit auditado: `8bb0884` (rama `driver/gpio`, rango `60b1ac9..8bb0884` sobre `ca569b6`)

**Veredicto: F0 no pasa.** La base de build está bien: 8/8 con `-Werror`, el alias de CMake y los tamaños coinciden con `ca569b6`. La guarda de máscaras del host (T0b(a)) también funciona. Pero hay cinco bloqueantes:

- **B1:** el control negativo de T15 no reproduce ninguna carrera. Los núcleos nunca corren a la vez, y las "500 000 corrupciones" son el estado de reset de PF10 contado una vez por iteración. Lo he reproducido en la placa.
- **B2:** hay veredictos que no se pueden poner en FAIL.
- **B3:** el script del host incumple S5, S7, S8 y S10.
- **B4:** T0b(c) no prueba la guarda que protege el bus.
- **B5:** la referencia T18 no es una captura cruda y no es la carga que pide el plan.

Por la regla A27, no he conectado el AD2 a esta imagen para ejecutar `gpio_hw_check.py`. Lo que sí he ejecutado va en la tabla siguiente. La lista para cerrar está al final.

Guía usada: `IMPLEMENTATION_PLAN.md` (revisión 2, tal como está en `e4a7a7d`), `IMPLEMENTER_INSTRUCTIONS.md`, la entrega del implementador (mensaje y `IMPLEMENTATION_REPORT.md` en `8bb0884`), el código y la placa.

### Qué ejecuté yo

| # | Qué | Resultado |
| --- | --- | --- |
| 1 | `tools/build_all.ps1 -Clean` en `8bb0884` | 8/8 PASS. `prj_04` release: CM7 text 85 036 B, CM4 text 8 476 B. Coincide con la tabla del informe |
| 2 | Build de `ca569b6` en un worktree (`build/wt_base`, ya borrado) y diff por objeto de `.text`+`.rodata` del `.map` enlazado, sin la parte de secciones descartadas | CM4 idéntico. CM7: −12 B en `app_main.c.obj` y −12 B en `ethernetif.c.obj`. Causa: las cadenas `__FILE__` absolutas de las aserciones (153 frente a 139 caracteres, porque la ruta del worktree es más larga). `ethernetif.c` no cambia, y el cambio de `app_main.c` está dentro de `#if GPIO_DRIVER_SELFTEST`. **Código enlazado idéntico** |
| 3 | Secciones de `gpio_bm.o` en el `.map` release | CM7: `EnablePortClock` 0x44. CM4: `EnablePortClock` 0x3c, `ConfigAnalog` 0x58. `.data` = `.bss` = 0. Coincide con el informe |
| 4 | T0b(a) con una DLL simulada: 10 máscaras contra `ad2_set_oe` | 10/10: toda máscara prohibida lanza `AD2SafetyViolation` **sin** llamar a `FDwfDigitalIOOutputEnableSet`; las permitidas sí la llaman. `git grep`: una sola llamada a `OutputEnableSet` en el workspace |
| 5 | Lectura por SWD (solo lectura) del bloque `0x3800A000` tal como lo dejó la última ejecución del implementador | `cm7_ready = cm4_ready = go_signal = 1`, `t15_cm4_count = t15_corruptions_cm4 = 0x7A120`, `t15_corruptions_cm7 = 0`, `cm4_done = cm4_all_passed = 1`. `current_step = 0xF541013D` y `bus_owner = 0xB5741547` son basura: nadie los escribe ni los inicializa |
| 6 | Imagen `selftest-gpio` compilada por mí en `8bb0884`, flasheada con el AD2 en S2 (OE = 0x0000 comprobado por read-back); bloque de SRAM4 puesto a cero por SWD, reset y 80 s de consola con `monitor.py --timestamps` | `[m4_gpio_test] Rev 8bb0884` y `[m4_gpio_test] TIMEOUT waiting for go_signal from CM7`. Bloque después: `cm4_ready = 1`, `go_signal = 0`, `cm4_done = 1`, `cm4_all_passed = 0`. Captura: `prj_04_adc_dma_bm_eth/docs/audit_f0_t15_cold_8bb0884.txt` |
| 7 | Lectura del código entregado: selftest de CM7 y CM4, `gpio_bench_protocol.h`, `tools/ad2.py`, `gpio_hw_check.py` y la integración en `main.c`/`app_main.c`/CMake | Hallazgos abajo |
| 8 | Comprobación de trazabilidad: `git diff a692afc..8bb0884`, sin documentos | Ningún cambio de código desde la imagen capturada: el hash `a692afc` de la captura es válido para el código entregado |

La placa quedó con `prj_04` **debug** (producción) flasheado y el AD2 en S2.

### Protocolo A1–A27 (lo aplicable a F0)

| # | Punto | Resultado | Nota |
| --- | --- | --- | --- |
| A1 | Reglas del catálogo | Pasa | Sufijos, sin prefijo numérico en los includes |
| A2 | C1: sin literales en el driver | Pasa | `gpio_bm.[ch]` sin cambios en F0 |
| A3 | C2: API congelada | Pasa | `gpio_bm.h` sin cambios |
| A5 | C4: targets | Pasa | `_gpio_bm` con fuente; alias sin fuentes; paraguas igual. m7 (comentario borrado) |
| A7 | C7: estado | Pasa | `.data` = `.bss` = 0 en `gpio_bm.o` |
| A16 | Build | **Pasa** | 8/8 en mi ejecución |
| A17 | R3: tamaños del `.map` | **Pasa** | Nota N3 |
| A18 | Pruebas de la fase repetidas por mí | **No ejecutado** | Bloqueado por A27. Solo la ejecución en frío sin AD2 (#6), que **no** reproduce el T15 entregado |
| A19 | R1: controles negativos | **Falla** | B1, B2, B4 |
| A20 | Mapa de escritores | Pasa (como inventario) | La migración es de F1b |
| A21 | R2: trazabilidad | Pasa con notas | Hash limpio en la captura; B5 en T18; m5 |
| A22 | R4: no destructivas | Pasa | Solo pines del banco; PE1 intacto |
| A23 | Oráculo independiente | **Falla** | B2: AD2→MCU aprueba por recibir el ACK, no por leer el bit |
| A26 | Documentación | **Falla** | M3 |
| A27 | Seguridad eléctrica S1–S10 | **Falla, bloqueante** | B3, B4, M1, M2 |

Los demás puntos (A4, A6, A8–A15, A24, A25) no aplican a F0: no hay código nuevo en el driver.

### Hallazgos

Clases según el protocolo del plan. Bloqueante: la fase no pasa. Mayor: se corrige antes de seguir. Menor: puede ir al cierre. Nota: sin acción.

#### Bloqueantes

| ID | Dónde | Hallazgo | Qué hace falta |
| --- | --- | --- | --- |
| B1 | `CM4/App/Src/gpio_selftest.c:127-174`, `CM7/App/Src/gpio_selftest.c:541-629`, `gpio_bench_protocol.h:67-98` | **T15 no demuestra ninguna carrera.** (a) Nadie inicializa el bloque de SRAM4, y SRAM4 sobrevive al reset: `go_signal`, `cm4_ready` y `cm4_done` quedan de la ejecución anterior (#5). (b) CM4 espera `go_signal` solo 10⁷ vueltas (menos de 1 s), y CM7 da la señal cuando llega a T15, al final de todo (11 s en la captura entregada). En frío, CM4 se rinde antes de que CM7 empiece (#6). En la captura entregada, CM4 corrió al arrancar gracias al `go_signal` viejo y CM7 corrió solo, mucho después. (c) El detector cuenta como corrupción que los bits del otro pin no sean 0 o 1. PF10 está en su estado de reset (11, analógico) mientras CM7 no ha empezado, así que se suma una "corrupción" por iteración: `cm4 = 500000` exacto y `cm7 = 0`. El 100 % sale de ahí, no de una carrera | Lo que dicen ahora T15 y la sección "Selftest" del plan (erratas E3 y E4): bloque inicializado por CPU1 antes de liberar a CPU2, con un identificador de sesión; esperas que cubran el tiempo real hasta que el otro núcleo llega al paso; pines en un estado conocido antes de empezar; solapamiento demostrado en los dos núcleos (> 0); y el control del control, con el otro núcleo parado, a 0 corrupciones |
| B2 | `CM4 gpio_selftest.c:189-197`; `CM7 gpio_selftest.c:487-510`; `gpio_hw_check.py:262-315` | **Veredictos que no pueden fallar.** (a) `cm4_ok = true` fijo: `CM4 VERDICT=PASS` pase lo que pase. (b) T0a AD2→MCU en CM7: si un bit se lee en bajo solo lo registra, y siempre imprime `PASS`. (c) Esa misma prueba en el host da "OK" por recibir el ACK, no por lo que leyó el MCU. (d) T0a MCU→AD2 en el host solo mira el bit esperado (`dio & mask`), así que no detecta un corto ni un cruce: un bit de más pasa. (e) El control en analógico ("DIO read 0x01") no tiene criterio | Cada veredicto se calcula a partir de sus resultados. La palabra se compara entera (fila T0a del plan). El resultado de lectura del MCU llega al host (en el log con el valor leído, o por el bus) y el host lo compara con lo que condujo |
| B3 | `gpio_hw_check.py:151-159`, `:180-199`, `:236-315` | **El host incumple S8, S10, S5 y S7.** S8: habilita `OE` en `HOST_REQ` (l. 152) **antes** de resetear y de leer el banner, y `banner_found` no condiciona nada. S10: resetea el MCU con DIO12 como salida; la línea de log "Resetting MCU under S2 safe state" es falsa. S5: si falla la prueba de sueltas (`BUSCHK up/down`), solo pone `all_passed = False` y sigue; nada impide la transmisión posterior "bus = AD2"; no hay aborto a S2. S7: los pasos se reconocen por subcadenas del log, el dueño del bus lo decide el host (`b < 8`, l. 299) en vez de tomarlo del anuncio del MCU, y ningún desajuste aborta. Hoy no hay contienda solo porque la secuencia sale bien. Nada la impediría si saliera mal | S2 hasta que se haya leído y validado el banner. El reset se hace con `OE = 0` y read-back. Una única función `s5_verify_released()` que condiciona **toda** transmisión "bus = AD2" y aborta a S2 si falla. Una tabla de pasos compartida con el firmware (`gpio_bench_protocol.h` y su espejo) que valida `Tn ready bus=…` y aborta si no coincide |
| B4 | `gpio_hw_check.py:202-218`; `CM7 gpio_selftest.c:306-334` | **T0b(c) no prueba S5.** La rama del host espera 0x5A y, si lo ve, escribe "ABORT" y "PASSED". Es código de prueba con el resultado esperado dentro, no la guarda que protege el bus: esa guarda no existe (B3). En el MCU, el PASS es solo "el handshake terminó" | El control tiene que ejecutar la misma `s5_verify_released()` de B3 y comprobar que devuelve "no suelto" y deja el AD2 en S2 sin haber llamado a `ad2_set_oe` con el bus (lo que dice ahora la fila T0b del plan) |
| B5 | `docs/gpio_f0_t18_60b1ac9.txt`; `IMPLEMENTATION_REPORT.md` §5.3 fila T18 | **La referencia T18 no vale como evidencia.** Es un documento montado con la salida de varias herramientas, cabeceras de sección y líneas "Result: PASS" escritas a mano; no es la salida de una sola ejecución. Tiene 300 consultas **de texto** de `query_adc.py`, no los 300 `WAVE` seguidos que pide T18. El informe dice "300/300 respuestas a `WAVE`" y "LED boot stage = 6", y ninguna de las dos cosas está en el archivo | Regrabar la referencia con el script tal como sale: la consola con marcas de tiempo, `query_adc.py --wave --count 300` en crudo y la lectura SWD en crudo, cada una como la salida de su propia ejecución, en archivos separados o en secciones con su comando y su hash. Si no hay aún código de GPIO que cambie la producción, puede grabarse en `8bb0884` o en el siguiente commit con opción apagada, siempre que el `.map` sea igual al de `ca569b6` (lo es) |

#### Mayores

| ID | Dónde | Hallazgo | Qué hace falta |
| --- | --- | --- | --- |
| M1 | `CM7 gpio_selftest.c:19-86`, `:431`; `CM4 gpio_selftest.c:74-98` | **La guarda S4 se salta con el número de paso.** `bench_claim_output` recibe el paso como parámetro, así que quien llama elige su permiso. En T0a se reclama PA8 con `10U /* T10 authorized */` cuando el paso es T0a. En CM4 la guarda ignora el paso (`(void)step`). Aquí eléctricamente no hay riesgo, porque PA8 es "solo MCU", pero la guarda no guarda nada: cualquier llamada con otro número pasaría. El plan tenía una contradicción (errata E1): el implementador tenía que preguntar (instrucciones §7), no esquivarla | Variable de paso actual única, escrita solo por el secuenciador; la guarda la lee (fila S4 del plan corregida). T0b(b) añade el caso "paso distinto del permitido" |
| M2 | `CM7 gpio_selftest.c:313-316`, `:518-522`, `:385`, `:398`, `:409` | **S2 no se cumple en los caminos de error.** Si `bench_set_pin_mode` falla a mitad del bucle de T0b(c), vuelve con los pines ya reclamados del bus en salida y sin `bench_safe()`. T0a_scope y T0a_loose salen igual. La siguiente prueba (T0b(d)) espera 10 s con el bus quizá conducido | Todo `return false` de una prueba pasa antes por `bench_safe()` (S2: "en cada error"). Una prueba en T0b(b) que fuerce un fallo a mitad de un bucle de reclamación |
| M3 | `IMPLEMENTATION_REPORT.md` §5.3, §5.5 | **El informe afirma cosas que la evidencia no respalda.** "Desviaciones: Ninguna", pero están el paso falso de PA8 (M1), las líneas de T0a (11 y 10 frente a lo que decía el plan) y la carga de T18. T18: "300/300 `WAVE`" y "LED boot stage = 6" (B5). T0a AD2→MCU: "leído correctamente en `IDR`", sin nada en el código que lo compruebe (B2). T15: "contadores privados" como oráculo de una carrera que no ocurrió (B1) | Corregir en una sección nueva del informe ("Respuesta a la auditoría de F0"), sin reescribir la anterior. Cada afirmación, con su fuente |

#### Menores

| ID | Dónde | Hallazgo |
| --- | --- | --- |
| m1 | `CM7 gpio_selftest.c:428-437` | T0a MCU→AD2 no mira el valor de retorno de `bench_set_pin_mode`: si la guarda rechaza, la prueba sigue como si el pin estuviera en salida |
| m2 | `gpio_bench_protocol.h:63-65`, `shared_memory.h:57-59` | `GPIO_TEST_SHARED_ADDR` está definida en dos cabeceras. El linker no reserva el área: si `.shared_ram` crece más de 8 KB, se solapa sin aviso. Falta un `_Static_assert` o un símbolo del linker |
| m3 | `gpio_hw_check.py:102-111` | El script imprime la lista S9 ya marcada con `[x]`. Eso no atestigua nada: el operador tiene que confirmarla (una opción `--checklist-confirmed` o una pregunta) |
| m4 | `app_main.c` (bloque GPIO), `CM7 gpio_selftest.c:644` | El selftest corre dentro de `Status_Task` y la bloquea hasta 60 s (más 10 s en T0b(d)). Mientras tanto no hay monitor de IPC, y al volver salen de golpe decenas de líneas `CPU2 NOT RESPONDING` (#6) que parecen un fallo de CPU2 |
| m5 | `IMPLEMENTATION_REPORT.md` §5.2 | "Salida registrada en sesión de terminal": la salida de `build_all -Clean` no está en el repo (entregable 2) |
| m6 | `tools/ad2.py:166-169` | `digital_io_write(value, mask)` escribe `value & mask` en **todas** las salidas: pone a 0 las que están fuera de la máscara. Hoy funciona porque solo hay una salida a la vez, pero es una trampa para F1 |
| m7 | `01_shared_drivers/CMakeLists.txt:17-19` | Se borró el comentario que explicaba por qué `00_gpio` va en la base de la pila |

#### Notas

- **N1:** No hay `gpio_ll` ni `gpio_hal`, y es correcto: el criterio C4 y el alcance del plan los aplazan.
- **N2:** El commit `e4a7a7d` del implementador incluye la revisión del plan y de las instrucciones que yo había dejado sin commitear. El contenido es el mío, y `DRIVER_ROADMAP.md` no se ha tocado. Aun así, el plan lo commitea el auditor o el responsable, no el implementador.
- **N3:** Las imágenes llevan rutas absolutas de la máquina (`__FILE__`); por eso el tamaño cambia según dónde se compile. Queda fuera del alcance de GPIO.
- **N4:** Que T0a MCU→AD2 recorra 11 líneas y AD2→MCU 10 es correcto: PE7 y PE2 son las líneas del handshake. El plan decía otra cosa (errata E2).
- **N5:** `0x3800A000` está dentro de `RAM_SHARED` y de la región 7 del MPU (no cacheable). Es correcto; solo falta la reserva (m2).
- **N6:** T0b(b) y T0b(d) son correctas por lectura del código. No las he ejecutado yo (A27).

### Erratas del plan (mías)

Corregidas en `IMPLEMENTATION_PLAN.md` en el mismo cambio que este informe:

- **E1:** S4 solo permitía PA8 en T10, pero T0a exige que el MCU conduzca PA8 en el walking-1. Ahora PA8 se permite en el walking-1 MCU→AD2 de T0a y en T10. Además, la guarda lee el paso actual en vez de recibirlo (M1).
- **E2:** T0a pedía la prueba de sueltas de las 15 líneas. `HOST_REQ` y `MCU_ACK` no se pueden soltar mientras se usan: ahora son 13 líneas sueltas más un paso específico para las dos del handshake, con 11 líneas en el walking-1 MCU→AD2 y 10 en el AD2→MCU. Cada paso compara la palabra entera.
- **E3:** El plan no obligaba a inicializar el bloque de SRAM4 en cada arranque, aunque el precedente de HSEM (el magic que sobrevive al reset) ya lo enseñaba. Ahora está en la sección "Selftest".
- **E4:** El criterio de T15 no exigía demostrar solapamiento ni el control del control. Ahora lo exige.
- Además, la fila de T0b(c) dice ahora que el control usa la misma función que protege las transmisiones reales.

### Lo que sí queda confirmado

- La división de targets y el alias (G11), sin cambio de código enlazado frente a `ca569b6`.
- La matriz 8/8 con `-Werror`.
- La guarda de máscaras del host `ad2_set_oe`: un único camino y rechazo antes de la DLL (T0b(a)).
- En el firmware: `bench_safe()` deja todos los pines cableados en entrada y no toca PE0/PE1; la lista de pines de la guarda coincide con la tabla "Quién conduce" (salvo M1); el banner `Rev` y `GPIO_BENCH v1 idle` existe.
- El cableado del banco (verificado antes por mí, el 2026-09-26) y las lecturas del scope de la captura entregada (0,014/0,036 V y 3,307/3,314 V) son coherentes con mi referencia.
- El inventario del mapa de escritores es correcto.

### Para cerrar F0

Por orden:

1. **B3 y B4:** el host con S8/S10 (S2 hasta validar el banner, reset con `OE = 0`), una `s5_verify_released()` única que condicione toda transmisión "bus = AD2" y aborte a S2, la tabla de pasos compartida (S7) y T0b(c) sobre esa función. **Hasta que esto esté, no conecto el AD2 a ninguna imagen del selftest.**
2. **M1 y M2:** la guarda del firmware con el paso actual, y `bench_safe()` en todo camino de error, con sus casos en T0b(b).
3. **B2:** veredictos calculados (CM4, T0a en los dos sentidos) y comparación de la palabra entera.
4. **B1:** T15 según el plan corregido: inicialización con identificador de sesión, esperas reales, pines en estado conocido, solapamiento > 0 en los dos núcleos y control del control a 0.
5. **B5:** referencia T18 regrabada en crudo con los 300 `WAVE`.
6. **M3:** sección "Respuesta a la auditoría de F0" en el informe, con los menores que se corrijan.
7. Nueva entrega. En la reauditoría repetiré yo la ejecución completa con el AD2 (A18), empezando por T0b.

### Reglas para la siguiente entrega

- **Este archivo lo escribe solo el auditor.** Las correcciones se citan en los commits (`fix(prj_04): … (B3)`).
- **Un hallazgo se cierra cuando lo verifico yo,** en un apartado "Reauditoría 1" de este archivo.
- **Las capturas no se editan.** Cada una es la salida de una sola ejecución, con `Rev` limpio y marcas de tiempo.
- **Un control que no falla cuando debe invalida su prueba.** Eso incluye los veredictos fijos: un `PASS` que no se calcula es un bloqueante.
- **Ante una contradicción del plan, se pregunta** (instrucciones §7); no se rodea.
