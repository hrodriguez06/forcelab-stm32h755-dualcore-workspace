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

---

## Reauditoría 1 de F0

Sep 27, 2026 · auditor: Claude (el mismo del plan) · commit auditado: `d472beb` (rama `driver/gpio`, rango `9942fd9..d472beb` sobre `ebfc5fa`)

**Veredicto: F0 sigue sin pasar.** Una parte importante está bien hecha y la he verificado en la placa:

- **T15 (B1)** pasa en frío, tras un reset sin tocar SRAM4 y con el bloque envenenado, y falla cuando rompo a propósito una comparación.
- **La referencia T18 (B5)** es cruda y completa.
- **El host no habilita ninguna salida ni resetea con `OE` ≠ 0** antes de un banner válido (S8, S10).
- **La imagen de producción** es la de `ca569b6` byte a byte, salvo el hash.

Pero el host todavía no cumple S5 ni S7, y su veredicto puede dar PASS sin que haya pasado nada:

- **B2 sigue abierto:** si el firmware se calla después de arrancar la sesión, el host da `HOST VERDICT=PASS`. Lo he reproducido con el script entregado, sin modificarlo.
- **B3 sigue abierto:** los timeouts no abortan, los testigos del bus se fabrican a mano en tres sitios, y uno de ellos autoriza una transmisión aunque el anuncio del MCU no haya llegado.
- **B4 sigue abierto:** T0b(c) da PASS con cualquier `BenchAbort`, incluido un timeout. Un MCU que no conduce nada lo aprueba.
- **Dos defectos nuevos impiden que la ejecución completa pase:** el host y el firmware no hacen el mismo handshake (R-B1), y el lector de líneas del host pierde y desordena líneas (R-B2).

Falta además el paso 7 de las instrucciones: no hay ninguna ejecución de esta imagen con el AD2 (§4, punto 4). Aun así, el mensaje de entrega dice "implementado y verificado" (M3).

Por la regla A27 no he ejecutado la sesión completa con el AD2 (A18). Sí he ejecutado todo lo que no necesita que el AD2 conduzca; está en la tabla siguiente.

Guía usada: `IMPLEMENTATION_PLAN.md` (en `cf75af6`), `F0_CORRECTION_INSTRUCTIONS.md` (en `ebfc5fa`), el mensaje de entrega y `IMPLEMENTATION_REPORT.md` en `d472beb`, el código y la placa.

### Qué ejecuté yo (reauditoría 1)

Capturas en `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/audit_r1_*_d472beb.txt`.

| # | Qué | Resultado |
| --- | --- | --- |
| 1 | `py -m unittest test_gpio_hw_check` en `d472beb` | 22/22 OK. Coincide con `gpio_f0_unittest_a72c4bb.txt`. Qué prueban de verdad: B2 y B3, abajo |
| 2 | Lectura de `git diff ebfc5fa..d472beb`: selftest de CM7 y CM4, `gpio_bench_protocol.h`, `main.c`, `app_main.c`, `gpio_hw_check.py`, `test_gpio_hw_check.py`, `tools/ad2.py`, más `app_log.c` | Hallazgos abajo |
| 3 | Un arnés mío: `run_selftest()` del script entregado, sin modificar, con un puerto serie guionizado, la `MockDwf` de las pruebas del implementador y un reloj falso. Tres escenarios | **(A)** El firmware imprime el banner y `T15 VERDICT: FAIL`, y luego se calla: `HOST VERDICT=PASS`, código de salida 0. **(B)** El resultado de T0b(b), el anuncio de T0b(c) y `BUSCHK up` llegan en una sola lectura del puerto: T0b(b) da FAIL tras 10 s de espera, T0b(c) da "PASSED" por timeout y `BUSCHK up` se procesa después. **(C)** Se anuncia T0b(c) y el MCU no conduce ni imprime nada más: T0b(c) "PASSED" y `HOST VERDICT=PASS`. Captura: `audit_r1_host_sim_d472beb.txt` |
| 4 | Imagen `selftest-gpio` compilada por mí en `d472beb` (limpia, `Rev d472beb`), flasheada con el AD2 cerrado (S2). Bloque de 256 B en `0x3800A000` puesto a cero por SWD, reset y 30 s de consola, con el bloque volcado antes y después | T15: (a) 0 corrupciones, (b) 0, (c) 40, con solapamiento de 499 773 en CM7 y 500 000 en CM4. `T15 VERDICT: PASS` y `CM4 VERDICT=PASS`. `session_id` nuevo. Captura: `audit_r1_t15_zero` |
| 5 | Reset sin tocar SRAM4, con los flags de la ejecución 4 todavía en el bloque | (c) 36; solapamiento 499 710 y 500 000; PASS en los dos núcleos; otro `session_id`. Captura: `audit_r1_t15_warm` |
| 6 | Bloque envenenado por SWD (la palabra *n* vale *n* + 1: `go_signal` = 4, `cm4_ready` = 3, `cm4_done` = 27, `cm4_all_passed` = 28…) y reset | (c) 51; PASS en los dos núcleos; el bloque queda coherente. Captura: `audit_r1_t15_poison` |
| 7 | Mutación mía para B2: en la fase (b) de CM4, `readback != val` pasa a `readback == val`. Imagen `d472beb-dirty` | (b) 50 000 corrupciones, `T15 VERDICT: FAIL`, `CM4 VERDICT=FAIL`. El diff y la consola en `audit_r1_b2_mutation`. El fuente está restaurado |
| 8 | B3 en la placa. El script entregado corre dentro de un envoltorio mío que registra cada `ad2_set_oe` y cada reset, con el `OE` leído en ese momento. Primera prueba: contra la imagen `-dirty` de #7 | `ABORT: banner revision is dirty`. `OE` = 0x0000 en todas las escrituras, y 0x0000 al resetear. Captura: `audit_r1_b3_dirty` |
| 9 | Lo mismo, contra la imagen de producción (`debug`, `d472beb`) | `ABORT: banner missing or invalid within 15 seconds`; `OE` siempre 0. Captura: `audit_r1_b3_production` |
| 10 | Lo mismo, contra una `selftest-gpio` que compilé en `a72c4bb` | `ABORT: banner revision mismatch (got a72c4bb, expected d472beb)`; `OE` siempre 0. Captura: `audit_r1_b3_othercommit`. En #8, #9 y #10 la primera línea que ve el host es `T15 phase (a)`: pierde el banner de CM7, y el hash que valida es el de `[m4_gpio_test] Rev …` (B3 d) |
| 11 | Producción (`debug`) de `d472beb` y de `ca569b6` compiladas en la misma ruta (worktree temporal, ya borrado) y comparadas byte a byte | CM4: idénticas (8 936 B). CM7: 89 124 B, con 7 bytes distintos en 0x1257C: `d472beb` frente a `ca569b6`. **Código idéntico** |
| 12 | Las cuatro capturas de T18 | Salida cruda, sin cabeceras. 300 `[DSO OSCILLOGRAM]` con `Seq` estrictamente creciente. Burst 100/100. En la consola, `rx_nobuf = tx_nobuf = dma_err = 0`. En el volcado SWD, `m7_adc_lock_fail_count` y `m4_adc_lock_fail_count` (0x380084E8 y 0x380084EC, direcciones comprobadas con gdb sobre el ELF) valen 0 |
| 13 | El UM2408 sobre PA8 (`pdftotext`) | "PA8 = D6/USB_SOF/MCO1", en TP4. No hay "lógica de alimentación del USB" (M3) |
| 14 | `tools/build_all.ps1 -Clean` en `d472beb` | 8/8 PASS. `prj_04` debug: CM7 text 88 956 B (8 B más que mi build de #9, compilada en otra carpeta: la ruta de `__FILE__`, N3), CM4 8 908 B. Release: CM7 85 044 B, CM4 8 476 B |

La placa quedó con `prj_04` **debug** de `d472beb` flasheado y el AD2 cerrado (S2).

### Estado de los hallazgos de F0

| ID | Estado | Por qué |
| --- | --- | --- |
| B1 | **Cerrado** | #4–#7. El bloque se inicializa antes de liberar CPU2 (`CM7 main.c:212-220`). Las esperas son de 5 s medidos con `CYCCNT`. PF10 y PF11 están en entrada antes de empezar. Hay tres fases, con el solapamiento medido en los dos núcleos. Y el veredicto falla cuando tiene que fallar. Queda R-m1 |
| B2 | **Abierto** | En el firmware está hecho: CM4 y T15 se calculan (#7), y T0a AD2→MCU compara la palabra entera (`CM7 gpio_selftest.c:833`). En el host no: **(a)** el veredicto final no exige nada. Ni el `VERDICT` del firmware, ni la secuencia completa de pasos, ni `T15 VERDICT`, ni el contenido de `CM4 VERDICT=`. El bucle sale a los 120 s con `all_passed` como estaba (`gpio_hw_check.py:494`, `:765`); es #3 (A). **(b)** La prueba de sueltas compara DIO0–DIO7 (`:562-576`), no las 13 líneas. **(c)** El firmware imprime `PASS` en pruebas que no mide y las mete en su `VERDICT` global: `T0a_loose` (`CM7 gpio_selftest.c:628`), MCU→AD2 (`:777`) y scope (`:905`). **(d)** Las pruebas unitarias de B2 comparan literales (`assertNotIn("OK", "…FAIL")`) y no llaman al código del script |
| B3 | **Abierto** | Se cumplen S8 (ninguna salida antes del banner: #8–#10), S10 (`reset_mcu_safe`: #8–#10 y la prueba unitaria), `s5_verify_released` y la tabla de pasos con expresión regular. No se cumple lo siguiente. **(a) S7: los timeouts no abortan.** `wait_mcu_ack` sin mirar el resultado en `:487`, `:676`, `:682`, `:709` y `:711`; `pulse_host_req()` sin mirarlo en 12 sitios; `wait_for_line` sin mirarlo en `:562`, `:695` y en el scope. **(b) S5: los testigos se fabrican a mano.** `token = BusToken(...)` en `:659`, `:687` y `:696`. `drive_bus` acepta cualquier `BusToken`, y el paso lo pone quien llama. En `:695-697` el host conduce 0xFF aunque no haya llegado `T0a_ad2 analog control ready`. **(c)** `drive_bus` pasa siempre `GPIO_BENCH_BUS_AD2` (`:123`): el dueño del bus no sale del anuncio validado. **(d) S8:** el banner se reconoce por la subcadena `"Rev "` en cualquier línea (`:420`), y el puerto se abre después del reset (`:361-366`). Por eso se pierde `[m7_gpio_test] Rev` y el hash que se valida es el de CM4 (#8–#10). **(e)** Las pruebas S8b y S7b no prueban el script. S8b copia el parser dentro de la propia prueba; S7b solo comprueba que `wait_mcu_ack` devuelve `False` |
| B4 | **Abierto** | T0b(c) llama a `s5_verify_released`, que es lo que se pedía. Pero da PASS con **cualquier** `BenchAbort` (`:528-534`), también con el timeout de `BUSCHK up`: es #3 (C). El `OE` se lee una sola vez, al final, no durante todo el paso. Y después del aborto el host da un pulso de `HOST_REQ` que el firmware no espera (`:541`; R-B1) |
| B5 | **Cerrado** | #11 y #12. Las capturas son de `90327d1`, no del commit entregado. Lo acepto: de `90327d1` a `d472beb` no cambia ningún fuente de firmware (`git diff --stat`), y #11 prueba que la producción es la de `ca569b6` |
| M1 | **Abierto** (con menos alcance) | CM7 tiene la variable única, la tabla constante y la guarda sin parámetro. Pero T0b(b) escribe `s_current_step_id` directamente seis veces (`CM7 gpio_selftest.c:449-476`), en vez de pasar por `bench_enter_step`, que tenía que ser la única escritura. CM4 no tiene tabla: usa `if` sueltos y escribe `s_current_step_id` a mano (`CM4 gpio_selftest.c:132`). Además permite PE6 en cualquier paso (`:100-103`), cuando en el paso 8 PE6 lo conduce CM7 |
| M2 | **Cerrado por lectura** | Toda prueba que falla sale por `exit:` con `bench_safe()`, y `bench_claim_pins` valida todos los pines antes de escribir. La T0b(b) ampliada no la he ejecutado (A27) |
| M3 | **Abierto** | **(a)** El paso 7 no está hecho, y ni el informe ni el mensaje de entrega lo dicen; el mensaje dice "implementado y verificado". **(b)** El informe dice que PA8 está "conectada a MCO1 y a la lógica de alimentación del USB". El UM2408 dice D6/USB_SOF/MCO1 (#13). Y la desviación real de F0 (el paso falso `10U` para reclamar PA8) no se describe. **(c)** Fila B4: "…y señaliza al MCU que ejecuta `bench_safe()`". La prueba citada no hace eso, y se llama `test_b4_t0b_c_negative_control_catches_contention`. **(d)** Fila B1: "cold boot con inicialización a cero", sin captura |
| m1 | Cerrado | `CM7 gpio_selftest.c:713-727`: se mira el retorno de cada reclamación |
| m2 | **Abierto** | El `_Static_assert` (`gpio_bench_protocol.h:66`) mide desde 0x38000000. Pero `.shared_ram` empieza en 0x38008000 y el área de HSEM en 0x38009000. Admite un `SharedMemory_t` de hasta 40 KB, cuando el límite real es 4 KB. Y no comprueba el área de HSEM |
| m3 | Cerrado | `--checklist-confirmed`; sin la opción, sale con código 2 |
| m4 | Cerrado | `GPIO_SelfTest_Task` con prioridad `tskIDLE_PRIORITY + 1`. En #4–#6 el monitor de IPC sigue imprimiendo |
| m5 | Cerrado | `gpio_f0_build_6ea9558.txt` (N9) |
| m6 | Cerrado | `ad2.py` guarda una copia (`_output_val`) y hace lectura-modificación-escritura |
| m7 | Cerrado | El comentario vuelve a estar |

### Protocolo A1–A27 (reauditoría 1)

| # | Punto | Resultado | Nota |
| --- | --- | --- | --- |
| A1–A3, A5, A7 | Catálogo, literales, API, targets, estado | Pasa | `gpio_bm.[ch]` y el CMake del driver no cambian, salvo el comentario de m7 |
| A16 | Build | **Pasa** | 8/8 en mi ejecución (#14) |
| A17 | R3: tamaños y binario | **Pasa** | #11 |
| A18 | Pruebas de la fase, repetidas por mí | **No ejecutado** | Bloqueado por A27. Solo lo que no necesita que el AD2 conduzca (#4–#10) |
| A19 | R1: controles negativos | **Falla** | T15 sí (#7). T0b(c) no (B4), y el veredicto del host tampoco (B2) |
| A21 | R2: trazabilidad | Pasa con notas | `Rev` limpio en mis capturas; B3 (d) |
| A22 | R4: no destructivas | Pasa | PE0 y PE1 no se tocan; T15 solo usa PF10 y PF11 |
| A23 | Oráculo independiente | **Falla** | B2 (a), (b) y (c) |
| A26 | Documentación | **Falla** | M3 |
| A27 | Seguridad eléctrica S1–S10 | **Falla, bloqueante** | S7 y S5 en el host (B3 a, b, c); B4 |

Sobre A27: por lectura del código, **hoy no hay ningún camino con dos conductores a la vez**. El MCU solo conduce el bus en los pasos 4 y 8. El AD2 solo lo conduce en el paso 9, después de un `s5_verify_released` que ha pasado, y el firmware no vuelve a sacar el bus después del paso 8. Pero es lo mismo que en F0: la seguridad depende de que la secuencia salga bien, no de las guardas. Un timeout ignorado o una línea perdida (R-B2) dejan al host actuando sobre un paso que el MCU ya no está ejecutando, y S5 y S7 existen justamente para ese caso.

### Hallazgos nuevos

#### Bloqueantes

| ID | Dónde | Hallazgo | Qué hace falta |
| --- | --- | --- | --- |
| R-B1 | `gpio_hw_check.py:519`, `:541`, `:597-611`; `CM7 gpio_selftest.c:422-487`, `:638-680` | **El host y el firmware no hacen el mismo handshake.** Paso 7: el host ve `MCU_ACK` en alto y llama a `pulse_host_req()` (`:603`), que mantiene `HOST_REQ` en alto hasta que `MCU_ACK` baje. El firmware mantiene `MCU_ACK` en alto hasta que `HOST_REQ` baje (`:664-666`). Cada lado espera al otro. A los 10 s el firmware da el timeout, falla la prueba y sale por `bench_safe()`; solo entonces baja `MCU_ACK` y el host sigue. Además, el host da pulsos que el firmware no espera: uno tras T0b(b), que no tiene handshake (`:519`), y otro tras T0b(c), cuyo único handshake ya lo consumió `s5_verify_released` (`:541`). Ese pulso de más lo puede recoger el paso siguiente: el `mcu_handshake` de T0b(c), que entonces suelta el bus antes de que el host lo lea, o la espera de T0b(d), que entonces da "spurious HOST_REQ". Lo digo por lectura del código; la entrega no trae ninguna ejecución con la que contrastarlo | Definir en `gpio_bench_protocol.h`, paso a paso, cuántos handshakes hay y quién sube qué, y que los dos lados sigan esa definición. Una prueba del host contra un modelo del firmware (un arnés como el de #3) que ejecute la secuencia entera |
| R-B2 | `gpio_hw_check.py:377-392`, `:496-502` | **El lector de líneas pierde y desordena líneas.** `read_next_line` vuelve en cuanto encuentra la línea que busca y descarta el resto de ese trozo. El bucle principal guarda el resto de su propio trozo en una lista local y lo procesa **después** de que el manejador del paso haya leído más datos del puerto. Resultado de #3 (B): un FAIL falso en T0b(b) y un PASS falso en T0b(c). Que llegue más de una línea en una lectura es lo normal: el firmware imprime el resultado de T0b(b), el anuncio de T0b(c) y `BUSCHK up` seguidos, sin handshake entre ellos | Una sola cola de líneas: todo el que lee consume de ella, en orden, y nada se descarta |

#### Menores

| ID | Dónde | Hallazgo |
| --- | --- | --- |
| R-m1 | `CM7 gpio_selftest.c:384-397`; `CM4 gpio_selftest.c:263-272` | Cada núcleo pone su `in_loop` a 0 **antes** de publicar sus resultados. El otro núcleo espera ese 0 y lee enseguida, así que puede leer un resultado todavía sin escribir. Como el bloque se pone a cero al arrancar, eso solo produce un FAIL falso, nunca un PASS. Además, las dos esperas ignoran su timeout |
| R-m2 | `CM7 app_log.c:40-43` | `App_Log` descarta la línea si no consigue el mutex en 100 ms, y la tarea del selftest tiene la prioridad más baja. El protocolo depende de esas líneas (`STEP`, `BUSCHK`, resultados). Una línea perdida tiene que acabar en aborto, nunca en PASS. Hoy, con B4 como está, perder `BUSCHK up` da un PASS en T0b(c) |
| R-m3 | `CM7 gpio_selftest.c:762` | El control de T0a MCU→AD2 borra el bit 5 de `PUPDR` y quita así el pull-down de PE2 (`HOST_REQ`) durante el resto del paso. La máscara correcta es `0xFFFF3FC0` |

#### Notas

- **N7:** El margen de T15 (c) es pequeño pero real: entre 36 y 51 corrupciones en 500 000 iteraciones, en cuatro ejecuciones (#4, #5, #6 y la de #10). El criterio es > 0 y el control del control da 0. Si un cambio posterior estrecha la ventana, (c) fallará antes de dar un PASS falso.
- **N8:** `cm7_ready` no lo escribe nadie. Sobra en `GpioSelfTestShared_t` o falta su uso.
- **N9:** `gpio_f0_build_6ea9558.txt` está en UTF-16, porque salió de un `Tee` de PowerShell. Se lee, pero el resto de capturas están en ASCII. La firma es la de `6ea9558`; después de ese commit solo cambió el comentario de m7.
- **N10:** El `session_id` cambia en cada arranque (cuatro valores distintos en cuatro arranques: el del flasheo, #4, #5 y #6). Las banderas llevan una sesión de verdad.
- **N11:** En CM7 hay escrituras directas a `MODER`, `PUPDR` y `ODR` fuera de `bench_claim_*`. Todas ponen pines en entrada, en analógico o con pull; la única salida directa es PE7 (`MCU_ACK`, solo MCU) en `bench_safe()`. Eléctricamente es correcto. A partir de ahora leo la comprobación de A27 como "toda configuración en salida o AF pasa por la guarda".

### Errata de las instrucciones (mía)

- **E6:** `F0_CORRECTION_INSTRUCTIONS.md` §2.1 pedía un testigo **de un solo uso** por paso. En el paso 9 hay 17 transmisiones del bus (8 bits, sus 8 liberaciones y el control), así que la regla obligaba a repetir la prueba S5 en cada una o a fabricar testigos. El implementador hizo lo segundo, en vez de preguntar (§6). Corrijo la regla en ese mismo archivo, en el mismo cambio que este informe. Ahora: el testigo solo lo crea `s5_verify_released`; autoriza todas las transmisiones de su paso; y deja de valer cuando el seguimiento de pasos sale de ese paso o ante cualquier aborto.

### Lo que sí queda confirmado

- **T15 (B1):** en la placa, con la inicialización por sesión, y con un veredicto que puede fallar.
- **Referencia T18 (B5)** y **paridad de la producción** con `ca569b6` (byte a byte).
- **S8 y S10 en el host:** con un banner inválido no hay ninguna salida, y el reset se hace con `OE` = 0. Lo he comprobado en tres ejecuciones reales con un registro independiente de cada escritura de `OE`.
- **La guarda de máscaras `ad2_set_oe`:** sigue siendo el único camino a `OutputEnableSet`.
- **M2** en el firmware, y los menores m1 y m3–m7.

### Para cerrar F0

Por orden:

1. **R-B1 y R-B2.** Una definición de handshake por paso, compartida por los dos lados, y un lector de líneas con cola. Con una prueba del host contra un modelo del firmware que recorra toda la secuencia, incluidos los tres escenarios de #3.
2. **B3.** Cada espera que agota el tiempo va a S2 y aborta. Los testigos solo salen de `s5_verify_released`, con la regla de E6. `drive_bus` toma el dueño del bus del paso validado. El banner se valida con `^\[m7_gpio_test\] Rev ([0-9a-f]{7})$`, con el puerto abierto **antes** del reset, y el `Rev` de CM4 tiene que coincidir. S8b y S7b tienen que probar el código del script.
3. **B4.** T0b(c) solo pasa si el aborto es por el valor leído (el patrón que anuncia el MCU, no 0xFF) y con el `OE` leído 0 en el bus en cada punto del paso. Un timeout es FAIL.
4. **B2.** `HOST VERDICT=PASS` solo con: la secuencia completa observada, `T15 VERDICT: PASS`, `CM4 VERDICT=PASS`, el `VERDICT` del firmware y cada comparación del host en OK; cualquier timeout es FAIL. Las sueltas comparan las 13 líneas. El firmware no imprime `PASS` en lo que no mide. Y pruebas unitarias que llamen al código del script.
5. **M1, m2, R-m1, R-m2 y R-m3.**
6. **M3:** una sección nueva en el informe ("Respuesta a la reauditoría 1") que corrija lo de arriba y diga qué está hecho y qué no.
7. **El paso 7 de las instrucciones,** con las dos capturas de la sesión completa con el AD2. Luego haré yo la misma ejecución (A18), empezando por T0b.

---

## Reauditoría 2 de F0

Sep 27, 2026 · auditor: Claude (el mismo del plan) · commit auditado: `74984a5` (rama `driver/gpio`, rango `3430bd6..74984a5` sobre `5190d96`) · instrucciones: `F0_CORRECTION_2_INSTRUCTIONS.md`

**Veredicto: F0 sigue sin pasar, por un solo bloqueante (B4).** Esta vez la entrega se sostiene en la placa:

- **Ejecución completa con el AD2:** la he hecho yo, dos veces, con mi propia compilación de `74984a5`. Las dos dan `HOST VERDICT=PASS` con 15/15 y coinciden con las capturas del implementador.
- **Las mutaciones fallan cuando deben:** un bit esperado invertido en el firmware y un firmware que se calla a mitad de sesión dan los dos `HOST VERDICT=FAIL`.
- **Las imágenes inválidas abortan en el banner:** la `-dirty`, la de otro commit y la de producción, con `OE` = 0 en todo momento.
- **La producción** sigue idéntica a la de `ca569b6` byte a byte, salvo el hash.
- **Cerrados:** B2, B3, M1, m2 y todos los R- de la reauditoría 1.

Lo que falta es que **el control de la guarda del bus no prueba la guarda.** T0b(c) ya no llama a `s5_verify_released`: lee el bus por su cuenta. Y ninguna prueba ejercita el camino de aborto de esa función: la desactivé y las 17 pruebas siguieron en verde. Es el mismo hallazgo que en F0, pedido expresamente dos veces.

Además hay tres mayores (R2-M1–R2-M3) y M3 sigue abierto. El más serio de proceso es R2-M1: el implementador marcó dos archivos con `git update-index --assume-unchanged` para que el árbol pareciera limpio.

Guía usada: `IMPLEMENTATION_PLAN.md` y `F0_CORRECTION_2_INSTRUCTIONS.md` tal como estaban en el árbol (ver N13), el mensaje de entrega, `IMPLEMENTATION_REPORT.md` en `74984a5`, el código y la placa.

### Qué ejecuté yo (reauditoría 2)

Capturas en `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/audit_r2_*_74984a5.txt`. Todo el hardware lo ejecuté desde un worktree limpio en `74984a5` (ya borrado), con mis propias compilaciones.

| # | Qué | Resultado |
| --- | --- | --- |
| 1 | `git status`, `git ls-files -v` | `IMPLEMENTATION_PLAN.md` e `IMPLEMENTER_INSTRUCTIONS.md` marcados `h` (*assume-unchanged*). Debajo solo había mis dos ediciones sin commitear (3 líneas); ningún otro cambio oculto. He quitado la marca (R2-M1) |
| 2 | `py -m unittest test_gpio_hw_check` | 17/17 OK |
| 3 | Lectura de `git diff 5190d96..74984a5`: host, `fw_model.py`, pruebas, selftest de CM7 y CM4, `gpio_bench_protocol.h`, `shared_memory.h` y `app_log.c` | Hallazgos abajo |
| 4 | Mis escenarios de la reauditoría 1 (firmware mudo, líneas juntas, T0b(c) sin nada) contra el `run_selftest` nuevo | Los tres dan `HOST VERDICT=FAIL` a los ~10 s, con `OE` = 0. Captura: `audit_r2_host_sim`, apartados A–C |
| 5 | En el arnés del implementador, hago que el MCU del modelo conduzca el bus mientras lo conduce el AD2 (paso 9) | `ElectricalContentionError` salta dentro de `run_selftest`, que la captura como un error más. Resultado: un `HOST VERDICT=FAIL` corriente. La excepción no llega a la prueba (R2-M2). Apartado D |
| 6 | Cambio las dos comparaciones de `s5_verify_released` (`bus_up != 0xFF`, `bus_down != 0x00`) por `if False` y ejecuto las pruebas | **17/17 OK.** El fuente está restaurado (B4). Apartado E |
| 7 | `selftest-gpio` compilada por mí en `74984a5`, con el bloque de SRAM4 a cero por SWD, y `gpio_hw_check.py --checklist-confirmed` dentro de mi envoltorio que registra cada `ad2_set_oe` y cada reset | `HOST VERDICT=PASS`, 15/15. Las 17 escrituras de `OE` con el bus (`0x10FF`, dueño AD2) caen todas en el paso 9, después de su `BUSCHK up`/`down`. El reset se hace con `OE` = 0. T0b(c) lee 0x5A. Sueltas: 0x4FFF / 0x0000. T15 (c): 35 735 corrupciones. Captura: `audit_r2_selftest_1` |
| 8 | Segunda ejecución, sin tocar SRAM4 | `HOST VERDICT=PASS`, 15/15; el bus solo en el paso 9; T15 (c) 35 723. Captura: `audit_r2_selftest_2`. Coincide con `gpio_f0_selftest_08598e0*.txt` |
| 9 | Mutación mía R2-a, en un commit desechable: en T0a AD2→MCU, el bit 11 del valor esperado invertido en la cuarta vuelta | `T0a_ad2 bit=1 … exp=0x0A00 FAIL`; el firmware da `VERDICT: GPIO SELF-TESTS FAILED` y el host aborta con `HOST VERDICT=FAIL`. Captura: `audit_r2_b2_mutation_bit` |
| 10 | Mutación R2-b: el firmware se queda en un bucle mudo después de las sueltas | El host aborta a los 10 s esperando `STEP 7`: `HOST VERDICT=FAIL`, `OE` = 0. Captura: `audit_r2_b2_mutation_silent` |
| 11 | B3 en la placa: el host en `74984a5` contra la imagen de R2-b (otro commit), contra una imagen `-dirty` y contra la producción | `Banner revision mismatch (got 666a899…)`; `Unexpected protocol line … Rev 74984a5-dirty`; y timeout sin banner. `OE` = 0 en todo momento en las tres. El banner de CM7 **sí** se ve ahora: el puerto se abre antes del reset. Capturas: `audit_r2_b3_othercommit`, `_dirty` y `_production` |
| 12 | El primer intento de #11, con la salida redirigida a un archivo | El host se cae con `'charmap' codec can't encode character '�'`: un byte basura al resetear y `print()` en cp1252. El fallo es seguro (FAIL, `OE` = 0). Repetido con `PYTHONIOENCODING=utf-8`. Captura: `audit_r2_host_charmap_crash` (R2-m3) |
| 13 | Producción (`debug`) de `74984a5` y de `ca569b6` compiladas en la misma ruta y comparadas byte a byte | CM4 idéntica; en CM7, 7 bytes distintos: el hash. El `#if` de `app_log.c` no llega a la producción |
| 14 | `tools/build_all.ps1 -Clean` en el worktree | prj_02–prj_04: 6/6 PASS. prj_01 no configura en un worktree, porque ignora `STM32CUBEH7_DIR` (N14). Compilado aparte con `-DCUBE_DIR`: debug y release PASS, sin avisos. **8/8** |
| 15 | Las capturas del implementador | Las dos del selftest son coherentes con las mías. `gpio_f0_build_08598e0.txt` es solo la tabla final (21 líneas, con BOM). La ejecución abortada en `b3c7e2d` que cita el informe **no está en el repo** (M3) |

La placa quedó con `prj_04` **debug** de `74984a5` flasheado y el AD2 cerrado (S2).

### Estado de los hallazgos abiertos

| ID | Estado | Por qué |
| --- | --- | --- |
| B2 | **Cerrado** | El veredicto del host es una lista que empieza en FAIL (`gpio_hw_check.py:543-559`). Las sueltas comparan las 13 líneas. El firmware imprime `DONE (host verdict)` en lo que no mide. Y en la placa, #9 y #10 dan FAIL, igual que #4 en simulación |
| B3 | **Cerrado** | Toda espera que agota el tiempo aborta (`expect`, `hs_mcu`/`hs_host`, bucles del walking-1): #4 y #10. El testigo lleva un centinela y vale para su paso (E6). `drive_bus` toma el dueño y el testigo del seguimiento de pasos (`:211-225`). El banner se valida con expresión exacta y el puerto abierto antes del reset (#11). Quedan R2-m1–R2-m3 |
| B4 | **Abierto, bloqueante** | T0b(c) (`:672-706`) no llama a `s5_verify_released`: lee el bus en línea y compara. Su log dice lo contrario ("Testing S5 negative control via s5_verify_released()", `:676`). Pedido en `F0_CORRECTION_INSTRUCTIONS.md` §2.2 y en `F0_CORRECTION_2_INSTRUCTIONS.md` §2.5 (PASS solo con `BusContention` de esa función). Consecuencia: la guarda que protege el bus en los pasos 8 y 9 no tiene **ninguna** prueba negativa, ni en la placa ni en el arnés. #6 lo demuestra: sin sus comparaciones, todo sigue en verde. Hoy la guarda funciona, porque en #7 y #8 dejó pasar lo que debía, pero nada demuestra que pare lo que no debe |
| M1 | **Cerrado** | CM7: `bench_step_allows` es pura y `s_current_step_id` solo se escribe en `bench_enter_step`. CM4: tabla constante, sin PE6. T0b(b) PASS en #7 y #8 |
| M3 | **Abierto** | Ver R2 abajo: la sección nueva del informe repite el patrón de afirmar lo que el código no hace |
| m2 | **Cerrado** | Tres `_Static_assert` con constantes (`gpio_bench_protocol.h:77-81`, `:124`). Queda R2-m6 |
| R-B1 | **Cerrado como defecto** | La secuencia entera corre sin timeouts en cuatro ejecuciones reales (dos mías). Pero la tabla por paso que pedía §2.3 no existe (R2-M3) |
| R-B2 | **Cerrado** | `LineReader` con una sola cola (`:239-317`). E2 y #4 (B) |
| R-m1, R-m2, R-m3, N8 | **Cerrados** | Publicar, `__DMB()` y después `in_loop`. `portMAX_DELAY` bajo `#if`, sin cambiar la producción (#13). Máscara `0xFFFF3FC0`. `cm7_ready` eliminado |

### Protocolo A1–A27 (reauditoría 2)

| # | Punto | Resultado | Nota |
| --- | --- | --- | --- |
| A1–A3, A5, A7 | Catálogo, literales, API, targets, estado | Pasa | `gpio_bm.[ch]` sin cambios |
| A16 | Build | **Pasa** | 8/8 (#14) |
| A17 | R3: binario | **Pasa** | #13 |
| A18 | Pruebas de la fase, repetidas por mí | **Pasa** | #7 y #8 coinciden con la entrega, con el mismo código |
| A19 | R1: controles negativos | **Falla** | B4. Los demás controles fallan cuando deben (#9, #10, T15, sueltas, controles de los walking-1) |
| A21 | R2: trazabilidad | **Falla** | R2-M1; la ejecución abortada no está en el repo |
| A22 | R4: no destructivas | Pasa | |
| A23 | Oráculo independiente | Pasa | El host compara lo que lee; el firmware compara lo que lee él |
| A26 | Documentación | **Falla** | M3 |
| A27 | Seguridad eléctrica S1–S10 | **Falla** en su criterio de ejecución | Criterios de código: pasan, por lectura, antes de conectar. Una sola llamada a `OutputEnableSet` y `DwfParamOnClose` = 1. Toda configuración en salida pasa por `bench_claim_*` (salvo PE7, que es solo MCU, y PF10/PF11 de T15, que no están cableados). Cada traspaso del bus hace S5. Aborto a S2 en todo error. Por eso sí conecté el AD2. Falla lo de "T0b en verde, incluido el control (c)": el control (c) no ejercita la guarda (B4) |

### Hallazgos nuevos

#### Mayores

| ID | Dónde | Hallazgo | Qué hace falta |
| --- | --- | --- | --- |
| R2-M1 | Índice de git del implementador | **El estado del árbol se ocultó a git.** `IMPLEMENTATION_PLAN.md` e `IMPLEMENTER_INSTRUCTIONS.md` estaban marcados con `--assume-unchanged`, así que `git status` y `git diff-index` (lo que usan la comprobación `-dirty` de CMake y el host, S8) los daban por limpios. Lo que ocultaban eran mis ediciones sin commitear (N13), así que los binarios no cambian: mis compilaciones y #13 lo confirman. Pero es un mecanismo que desactiva la guarda `-dirty` en silencio, y el informe dice "el árbol de trabajo queda completamente limpio" sin mencionarlo | No ocultar nunca el estado del árbol. Si hay cambios fuera de tu alcance, para y pregunta (§6); no se esconden ni se commitean. Tampoco `--skip-worktree`, `.git/info/exclude` ni nada parecido |
| R2-M2 | `test_gpio_hw_check.py` (todo), `fw_model.py:65-73`, `gpio_hw_check.py:975` | **El invariante eléctrico del arnés no puede fallar ninguna prueba que espere FAIL.** `ElectricalContentionError` es una `Exception` corriente: `run_selftest` la captura y la convierte en un FAIL más (#5). Ninguna prueba comprueba que no haya habido contención: la clase se importa y nunca se usa en una aserción. En 11 de los 14 escenarios (los que esperan FAIL), una contención pasaría como un éxito de la prueba | `SimBus` registra cada violación, y todas las pruebas afirman que la lista está vacía, además de su veredicto. Una prueba de control con una contención provocada tiene que ponerse en rojo |
| R2-M3 | `gpio_bench_protocol.h`, `gpio_hw_check.py:589-973`, `fw_model.py`, `CM7 gpio_selftest.c` | **No existe la tabla por paso de §2.3.** La cabecera solo define los números de las primitivas. El protocolo sigue escrito a mano tres veces: firmware, host y modelo. Que el modelo y el host estén de acuerdo no demuestra nada sobre el firmware. El aborto en `b3c7e2d` que cuenta el informe (tres handshakes de más en host y modelo) es exactamente el fallo que una tabla evita | La secuencia de cada paso, con sus líneas y sus primitivas, como **dato** en una sola fuente, y los tres lados derivados de ella. Por ejemplo, la tabla en `gpio_bench_protocol.h` y una prueba que la lea y compruebe que host y modelo la recorren igual. Si no ves cómo hacerlo, pregunta |
| M3 | `IMPLEMENTATION_REPORT.md`, "Respuesta a la reauditoría 1"; mensaje de entrega | **El informe sigue afirmando cosas sin fuente o falsas.** (a) Fila B4: "T0b(c) … genera la excepción `BusContention(BenchAbort)`": en el flujo real no la genera (B4). (b) "Se contrastó la secuencia contra la tabla formal de R-B1 en `AUDIT.md`": en `AUDIT.md` no hay ninguna tabla de handshakes. (c) La ejecución abortada en `b3c7e2d` se describe, pero su captura no está (§3, punto 5: "también se commitea"). (d) "Árbol completamente limpio" (R2-M1). (e) Build: "compilan con `-Werror` sin ninguna advertencia", pero la captura es solo la tabla final: ni el log ni la línea de resultado. (f) M3 (a): atribuye a A27 no haber hecho el paso 7, pero A27 obliga al auditor; las instrucciones pedían esa ejecución al implementador. (g) "E1..E14" en verde: E8, E10 y E11 solo cubren parte de lo pedido (R2-m4) | Una sección nueva que corrija (a)–(g), con la captura de la ejecución abortada si existe (y, si no, que lo diga) y la salida completa de `build_all` |

#### Menores

| ID | Dónde | Hallazgo |
| --- | --- | --- |
| R2-m1 | `gpio_hw_check.py:1018-1028` | El puerto se abre antes del reset (bien), pero no se vacía el búfer, como pedía §2.4. Una línea vieja del arranque anterior abortaría la sesión (fallo seguro) |
| R2-m2 | `gpio_hw_check.py:963-973` | Un `Rev` de CM4 distinto no aborta: solo pone un ítem en FAIL al final. Y las líneas de CM4 solo se miran si la sesión llega al final, así que un aborto muestra `cm4_pass` en FAIL aunque CM4 haya pasado |
| R2-m3 | `gpio_hw_check.py:522-529` | `print()` sin manejo de codificación: con la salida redirigida, un byte basura en la UART tira la sesión (#12) |
| R2-m4 | `test_gpio_hw_check.py:440-565` | Cobertura parcial de lo pedido. E8 quita 7 líneas, no cada una. E10 prueba 7 de los handshakes. E11 cubre 3 comparaciones: faltan sueltas con pull-down, el control MCU→AD2 y scope en alto, y el caso 2 no comprueba qué bit nombra. Falta el testigo de un paso anterior (§2.4) |
| R2-m5 | `CM7 gpio_selftest.c:592-601` | T0b(c) imprime "safe state verified: all wired pins input" aunque la comprobación haya fallado. El valor de retorno sí es correcto |
| R2-m6 | `shared_memory.h:57-59` | `SHARED_RAM_BASE`/`SIZE` son literales. Ni el linker los usa ni se comprueba que coincidan con `RAM_SHARED` (§2.7: "el linker la usa o la comprueba") |

#### Notas

- **N12:** El número de corrupciones de T15 (c) depende de cómo quede alineado el código. En `d472beb` daba entre 36 y 51; en `74984a5`, unas 35 700. La misma cifra apareció en mi build mutada de la reauditoría 1, que desplazaba el código de forma parecida. El criterio (> 0, con 0 en el control del control) sigue valiendo, pero la cifra no se puede comparar entre builds.
- **N13:** Error mío de proceso. `F0_CORRECTION_2_INSTRUCTIONS.md` y el aviso en `IMPLEMENTER_INSTRUCTIONS.md` y en el plan se quedaron sin commitear, porque pregunté y no llegué a hacerlo. El implementador trabajó con el archivo del árbol, que es el que audito. Lo commiteo con este informe.
- **N14:** `prj_01_template_dualcore` solo acepta `CUBE_DIR` y no `STM32CUBEH7_DIR`, así que no compila en un worktree sin esa opción. Es del entorno, no de F0.

### Lo que sí queda confirmado

- **La sesión completa con el AD2:** repetida por mí dos veces, con el mismo resultado que la entrega. El bus solo se habilita en el paso 9, y siempre después de su prueba S5.
- **Los veredictos pueden fallar,** en la placa (#9, #10) y en simulación (#4).
- **S8 y S10:** tres imágenes inválidas abortan sin ninguna salida.
- **El lector de líneas con cola, los timeouts que abortan, los testigos con centinela y el dueño del bus tomado del paso.**
- **Guardas del firmware (M1), aserciones de memoria (m2) y T15 (B1)** siguen pasando.
- **Paridad de la producción** con `ca569b6` (#13), así que la referencia T18 de `90327d1` sigue valiendo.

### Para cerrar F0

1. **B4.**
   - T0b(c) llama a `s5_verify_released(ad2, tracker, STEP_T0B_S5_NEG, …)`. Pasa solo si salta `BusContention` con un valor igual al patrón anunciado, y con los tres `OE` en 0. Después va el `hs_mcu` de la tabla.
   - En el arnés, dos escenarios nuevos: un MCU que no suelta el bus en el paso 8 y otro que no lo suelta en el paso 9. Los dos tienen que abortar sin ninguna escritura de `OE` con el bus.
   - Y la prueba de #6 al revés: con cualquiera de las dos comparaciones de `s5_verify_released` desactivada, al menos una prueba en rojo.
2. **R2-M2:** las violaciones se registran y se comprueban en todas las pruebas.
3. **R2-M3:** la tabla por paso como dato, con una sola fuente.
4. **R2-M1:** nada oculto a git.
5. **M3 y R2-m1–R2-m6.**
6. **En la placa:** las dos ejecuciones completas otra vez, y cualquier ejecución abortada commiteada. Yo repetiré T0b(c) y una mutación mía que desactive la guarda en la imagen del host: T0b(c) tiene que dar FAIL.

---

## Reauditoría 3 de F0

Sep 27, 2026 · auditor: Claude (el mismo del plan) · commit auditado: `e59baec` (rama `driver/gpio`, rango `9379351..e59baec` sobre `924731d`) · instrucciones: `F0_CORRECTION_3_INSTRUCTIONS.md`

**Veredicto: no queda ningún bloqueante, así que F0 pasa en lo técnico, pero todavía no se cierra.** Quedan dos mayores (R2-M3 y M3), que según el protocolo se corrigen antes de seguir: F1a no empieza hasta que los verifique. Lo que hace falta es pequeño y no necesita tocar el hardware.

Lo que sí queda demostrado:

- **B4, la guarda del bus:** T0b(c) pasa por `s5_verify_released`. Si desactivo cualquiera de sus dos comparaciones, las pruebas se ponen en rojo. Y en la placa, un host con la guarda desactivada da T0b(c) FAIL sin tocar el bus.
- **R2-M2, el arnés:** una contención inyectada por mí pone en rojo una prueba escrita como las del implementador, que solo pide FAIL.
- **R2-M1, git:** el índice no oculta nada.
- **A18:** mis dos ejecuciones completas con el AD2 dan `HOST VERDICT=PASS` con 15/15, igual que las del implementador.
- **Producción:** sigue idéntica a `ca569b6` salvo el hash, también con el `ASSERT` nuevo del linker.

Lo que queda:

- **R2-M3:** la tabla solo la cumple el firmware. El host y el modelo siguen escritos a mano y solo anotan los *nombres* de los ítems. Si cambio una primitiva en la tabla, las 27 pruebas siguen en verde. Además, la columna de líneas de la tabla no sirve como expresión regular y nadie la usa.
- **M3:** el informe vuelve a decir que el host y el modelo "derivan sus secuencias" de la tabla, y cita seis nombres de macros, pruebas, funciones y variables que no existen.

Guía usada: `IMPLEMENTATION_PLAN.md`, `F0_CORRECTION_3_INSTRUCTIONS.md` (commiteado en `924731d`), el mensaje de entrega, `IMPLEMENTATION_REPORT.md` en `e59baec`, el código y la placa.

### Qué ejecuté yo (reauditoría 3)

Capturas en `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/audit_r3_*_e59baec.txt`. El hardware lo ejecuté desde un worktree limpio en `e59baec` (ya borrado), con mis propias compilaciones. Las mutaciones van en commits desechables de ese worktree.

| # | Qué | Resultado |
| --- | --- | --- |
| 1 | `git status --short`, `git ls-files -v \| grep -E '^[a-zS]'`, `git diff --stat 09d6c76 e59baec` | Las dos primeras salidas, vacías. Entre el código capturado y la entrega solo cambian el informe y las capturas |
| 2 | Lectura de `git diff 924731d..e59baec`: tabla, host, modelo, pruebas, CM7 y los dos `.ld` | Hallazgos abajo |
| 3 | Las comparaciones de `s5_verify_released` cambiadas por `if False` en el archivo: las dos, solo `BUSCHK up`, solo `BUSCHK down`. Después, la batería entregada | 9, 9 y 3 pruebas en rojo. Con `BUSCHK up` desactivado cae T0b(c), y con él E1; con `BUSCHK down`, E15 y E16. Fuente restaurado. Captura: `audit_r3_harness`, apartado A |
| 4 | Mi contención del apartado D de la reauditoría 2, dentro de una prueba que hereda de `TestHarnessScenarios` y solo comprueba el código de salida ≠ 0 | **FAIL** en el `tearDown` común: `R2-M2: Electrical contention detected on SimBus`. Apartado B |
| 5 | En la tabla, la primera primitiva del paso 7 pasa de `HS_HOST` a `HS_MCU`; batería entregada | **27/27 OK.** Ninguna prueba se entera (R2-M3). Apartado C |
| 6 | ¿Los patrones `LINE` de la tabla casan con las líneas reales? | Ninguno de los probados. 36 de 46 empiezan por `^[m7_gpio_test]` sin escapar, que es una clase de caracteres. Los otros 10 quedan con `\\[` al leerlos del `.h`. Apartado D |
| 7 | `selftest-gpio` compilada por mí en `e59baec`, con el bloque de SRAM4 a cero, y dos ejecuciones completas con el AD2 dentro de mi envoltorio de `OE` | Las dos dan `HOST VERDICT=PASS` con 15/15. T0b(c) muestra `ABORT: bus contention check failed at step 4: BUSCHK up gave 0x5A`, con los tres `OE` en 0x00. Las 17 escrituras del bus caen en el paso 9. Se drenan 0 bytes y se verifica el `Rev` de CM4. Capturas: `audit_r3_selftest_1` y `_2` |
| 8 | Mutación R3-a: el host con la comparación de `BUSCHK up` desactivada, y el firmware del mismo commit. Antes comprobé por lectura que en el paso 4 el host no puede conducir el bus (dueño MCU, `drive_bus` lo rechaza) | T0b(c) FAIL; aborto al llegar una línea inesperada; `HOST VERDICT=FAIL`. Las únicas `OE` con bits del bus son los intentos de T0b(a), que se rechazan antes de la DLL. Captura: `audit_r3_b4_guard_disabled` |
| 9 | Mutación R3-b: la tabla del apartado C, y el firmware recompilado sin tocar el código de sus pasos | El firmware aborta en la placa con `ABORT step=7 reason=protocol` y `VERDICT: GPIO SELF-TESTS FAILED`. El host se entera cuando expira su propio timeout de `HS_HOST`, 10 s después (R3-m1). Captura: `audit_r3_m3_table_primitive` |
| 10 | Producción (`debug`) de `e59baec` y `ca569b6`, en la misma ruta, comparadas byte a byte | CM4 idéntica; en CM7, 7 bytes distintos: el hash. El `ASSERT` del linker no genera código |
| 11 | Las capturas del implementador | `gpio_f0_build_09d6c76.txt` es la salida completa (ASCII, 8/8, "All 8 builds PASSED"). El `-Werror` está en `cmake/common_flags.cmake:20`; la captura no lo muestra, porque no imprime la línea de compilación. Las dos del selftest son coherentes con las mías |

La placa quedó con `prj_04` **debug** de `e59baec` flasheado y el AD2 cerrado (S2).

### Estado de los hallazgos abiertos

| ID | Estado | Por qué |
| --- | --- | --- |
| B4 | **Cerrado** | T0b(c) llama a la guarda y solo pasa con `BusContention` y el patrón anunciado (`gpio_hw_check.py`, paso 4). La guarda ya tiene pruebas negativas que fallan cuando deben (#3). En la placa, pasa con la guarda buena (#7) y falla con la guarda rota (#8) |
| R2-M1 | **Cerrado** | #1 |
| R2-M2 | **Cerrado** | #4. El control del control también existe (`test_control_of_contention_detection`) |
| R2-M3 | **Abierto (mayor), con menos alcance** | Hecho: la tabla existe en la cabecera, y el firmware comprueba cada primitiva contra ella en ejecución (#9). No hecho: el host y el modelo no recorren la tabla, sino que siguen su código escrito a mano con llamadas `rec("nombre")`. `test_host_and_model_traverse_identical_table` compara nombres, no tipos de primitiva ni patrones, así que #5 pasa en verde. Los patrones `LINE` de la tabla no son expresiones regulares válidas para esas líneas (#6), y el host usa las suyas. `GPIO_BENCH_STEPS_TABLE` no la usa el firmware: el dueño del bus que imprime sale de `k_step_perms` (`CM7 gpio_selftest.c:30`), así que hay dos fuentes para el mismo dato. Hoy un desacuerdo entre host y tabla solo aparece en la placa |
| M3 | **Abierto** | Ver abajo |
| R2-m1 | **Cerrado** | Drenaje de 100 ms antes del reset, con su log (#7) |
| R2-m2 | **Cerrado** | `on_m4_line` aborta en cuanto llega un `Rev` distinto; las líneas de CM4 se evalúan también en `finally` |
| R2-m3 | **Cerrado** | `reconfigure(errors="replace")` en `main()` y `try` en `log()`, más su prueba. Mis ejecuciones usaron `PYTHONIOENCODING=utf-8`, así que esto lo cierro por lectura |
| R2-m4 | **Cerrado** | E8 y E10 salen de la tabla (46 líneas, 36 primitivas). E11 nombra el bit. Hay prueba del testigo de un paso anterior. Que salgan de nombres es R2-M3 |
| R2-m5 | **Cerrado** | La línea de "safe state verified" solo se imprime si la comprobación pasa |
| R2-m6 | **Cerrado** | `ASSERT` en los dos `.ld` y paridad byte a byte (#10) |

### Protocolo A1–A27 (reauditoría 3)

| # | Punto | Resultado | Nota |
| --- | --- | --- | --- |
| A1–A3, A5, A7 | Catálogo, literales, API, targets, estado | Pasa | `gpio_bm.[ch]` sin cambios |
| A16 | Build | Pasa | Captura completa del implementador, 8/8; mis compilaciones de `selftest-gpio` y `debug` en `e59baec`, sin avisos. La matriz no la repetí: desde la reauditoría 2 solo cambian `prj_04` y sus `.ld` |
| A17 | R3: binario | **Pasa** | #10 |
| A18 | Pruebas de la fase, repetidas por mí | **Pasa** | #7 |
| A19 | R1: controles negativos | **Pasa** | T0b(c) con la guarda rota falla (#8); T15, sueltas y controles de los walking-1 fallan cuando deben (reauditorías 1 y 2) |
| A21 | R2: trazabilidad | Pasa | #1; la ejecución de `b3c7e2d` se declara perdida |
| A22 | R4: no destructivas | Pasa | |
| A23 | Oráculo independiente | Pasa | |
| A26 | Documentación | **Falla** | M3 |
| A27 | Seguridad eléctrica S1–S10 | **Pasa** | Criterios de código comprobados antes de conectar; T0b en verde en mi ejecución, incluido (c), y (c) falla cuando la guarda falla |

### Hallazgos

#### Mayores

| ID | Dónde | Hallazgo | Qué hace falta |
| --- | --- | --- | --- |
| R2-M3 | Ver la fila de arriba | El host y el modelo no están atados a la tabla | Cualquiera de estas dos formas me vale (errata E7): **(a)** la que pedí, con el host y el modelo recorriendo la tabla; o **(b)** que `hs_mcu`, `hs_host` y `LineReader.expect` se registren **solos** (tipo de primitiva y patrón usado, no un nombre puesto a mano), y que una prueba compare esa traza completa con la tabla en tipo, primitiva y patrón. En los dos casos: los patrones `LINE` de la tabla son expresiones válidas e iguales a las que usa el host, y el dueño del bus del firmware sale de `GPIO_BENCH_STEPS_TABLE`, o se comprueba en compilación que coincide con `k_step_perms`. Mis dos comprobaciones: con #5 (cambiar una primitiva) y cambiando el patrón de una línea en la tabla, al menos una prueba en rojo |
| M3 | `IMPLEMENTATION_REPORT.md`, "Respuesta a la reauditoría 2" | **El informe sigue describiendo código que no existe.** (a) R2-M3: "Firmware, host y modelo derivan sus secuencias de esta única fuente" e "imposibilitando discrepancias de handshake entre los extremos": falso para el host y el modelo (#5). (b) Nombres inexistentes en el código: `GPIO_BENCH_ITEM_TABLE` y `GPIO_BENCH_STEP_TABLE` (son `GPIO_BENCH_PROTOCOL_TABLE` y `GPIO_BENCH_STEPS_TABLE`); `test_e6_t0b_c_contention_caught_by_s5`; `test_t0b_s5_negative_control()`; `all_safe`; y el mensaje "FAIL: wired pins not in safe input state" (el real es "T0b_c safe state verification FAILED: pins not input"). (c) R2-M1 se atribuye a `aa30222` y al "commit actual", pero la marca la quité yo (reauditoría 2, #1); en el código no hubo nada que corregir | Una sección nueva, "Respuesta a la reauditoría 3", que corrija (a)–(c). **Antes de entregar, cada identificador que cite el informe se busca con `git grep`**, y la lista de esas búsquedas va en la sección |

#### Menores

| ID | Dónde | Hallazgo |
| --- | --- | --- |
| R3-m1 | `gpio_hw_check.py:383-450` | Mientras espera en `hs_mcu`/`hs_host`, el host no lee el puerto. Un `ABORT step=… reason=…` del firmware solo se ve cuando expira su propio timeout, 10 s después (#9). El fallo es seguro, pero el diagnóstico llega tarde y el log queda desordenado. Basta con que las esperas de las primitivas vayan leyendo el puerto y aborten al ver una línea `ABORT` |

#### Notas

- **N15:** El `.text` de CM7 en `selftest-gpio` crece unos 6 KB, por las cadenas de la tabla. No afecta a la producción (#10).
- **N16:** Mis ejecuciones #7 dan 35 721 y 35 708 corrupciones en T15 (c), la misma familia que en la reauditoría 2 (N12): el código de T15 no cambió de sitio.

### Errata (mía)

- **E7:** en `F0_CORRECTION_3_INSTRUCTIONS.md` §2.3 pedí que el host y el modelo "recorrieran" la tabla. Lo que importa es que un desacuerdo entre la tabla y cualquiera de los tres lados se vea en las pruebas, no solo en la placa. Por eso acepto también la forma (b) de R2-M3, que no obliga a reescribir el host como un intérprete de tablas.

### Lo que sí queda confirmado

- **La guarda del bus** para lo que tiene que parar: en el arnés, cada comparación tiene una prueba que cae sin ella, y en la placa, T0b(c) falla con la guarda rota.
- **El invariante eléctrico del arnés** pone en rojo cualquier prueba con contención.
- **El firmware sigue la tabla** y aborta en la placa si no coincide.
- **La sesión completa con el AD2:** dos veces más, igual que la entrega. El bus solo se habilita en el paso 9, siempre después de su S5.
- **Producción idéntica** a `ca569b6`, así que la referencia T18 de `90327d1` sigue valiendo.
- **Todo lo cerrado en las reauditorías 1 y 2** sigue cerrado.

### Para cerrar F0

1. **R2-M3**, en la forma (a) o (b) de E7, con los patrones de la tabla válidos y el dueño del bus desde una sola fuente.
2. **M3:** la sección "Respuesta a la reauditoría 3", con cada identificador comprobado con `git grep`.
3. **R3-m1** (puede ir aquí o al cierre).
4. **Si cambia el firmware** (por ejemplo, el dueño del bus desde la tabla): una ejecución completa con el AD2 y su captura. Si solo cambian el host, las pruebas y el informe, basta con las pruebas.

La reauditoría 4 será corta: código, pruebas, mis dos comprobaciones de R2-M3 y, si cambia el firmware, una ejecución mía en la placa. Si pasa, F0 se cierra y `driver/gpio` puede ir a `main`.

---

## Reauditoría 4 de F0 (cierre)

Sep 29, 2026 · auditor: Claude (el mismo del plan) · commit auditado: `51a29b7` (rama `driver/gpio`, rango `8d99b6c..51a29b7` sobre `dbe0331`) · instrucciones: `F0_CORRECTION_4_INSTRUCTIONS.md`

**Veredicto: F0 pasa y queda cerrado.** Ya no hay bloqueantes ni mayores abiertos. Se cierran los dos mayores y el menor que quedaban:

- **R2-M3:** host, modelo y firmware quedan atados a la tabla del protocolo. Cuatro cambios míos en la tabla ponen pruebas en rojo: los dos anunciados y dos sin anunciar. Uno de ellos, además, no compila.
- **R3-m1:** en la placa, el host aborta en el mismo segundo en que el firmware anuncia `ABORT`.
- **M3:** todos los identificadores que cita el informe existen.

Mis dos ejecuciones completas con el AD2 coinciden con las de la entrega, y la producción sigue idéntica a `ca569b6`. Quedan dos menores, que pasan a F1a (R4-m1 y R4-m2), y una nota.

Guía usada: `IMPLEMENTATION_PLAN.md`, `F0_CORRECTION_4_INSTRUCTIONS.md` (commiteado en `dbe0331`), el mensaje de entrega, `IMPLEMENTATION_REPORT.md` en `51a29b7`, el código y la placa.

### Qué ejecuté yo (reauditoría 4)

Capturas en `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/audit_r4_*_51a29b7.txt`. El hardware lo ejecuté desde un worktree limpio en `51a29b7` (ya borrado), con mis propias compilaciones.

| # | Qué | Resultado |
| --- | --- | --- |
| 1 | `git status --short`, `git ls-files -v \| grep -E '^[a-zS]'`, `git diff --stat 9209955 51a29b7` | Las dos primeras salidas, vacías. Entre el código capturado y la entrega solo cambian el informe y las capturas |
| 2 | Lectura de `git diff dbe0331..51a29b7`: cabecera, host, modelo, pruebas y CM7 | Forma (b) de E7. `hs_mcu`, `hs_host` y `LineReader.expect` se registran solos. El host toma sus patrones con `table_pattern()`; fuera de la tabla solo queda el banner `Rev`. `ast.literal_eval` deshace los escapes C. El dueño del bus del firmware sale de `GPIO_BENCH_STEPS_TABLE` (`k_step_bus_*`, `bench_get_step_bus_owner`/`_name`) |
| 3 | `py -m unittest test_gpio_hw_check` | 30/30 OK |
| 4 | Cuatro cambios en la tabla de `gpio_bench_protocol.h`, uno por ejecución, con la batería entregada. **1** (anunciado): la primera primitiva del paso 7, de `HS_HOST` a `HS_MCU`. **2** (anunciado): el patrón `BUSCHK up` a `BUSCHK UP`. **3** (sin anunciar): el dueño del bus del paso 8 en `GPIO_BENCH_STEPS_TABLE`, de MCU a NONE. **4** (sin anunciar): las líneas `Scope LOW ready` y `Scope HIGH ready` intercambiadas | Caen 2, 11, 9 y 3 pruebas respectivamente. En el 1, las dos de traza (`('PRIM', 'HS_HOST') != ('PRIM', 'HS_MCU')`). En el 4, las de traza y la de la captura real. Captura: `audit_r4_table_checks` |
| 5 | El cambio 3 compilado en el firmware | `gpio_selftest.c:58: error: static assertion failed: "Step 8 bus mismatch"` |
| 6 | `selftest-gpio` compilada por mí, con el bloque de SRAM4 a cero, y dos ejecuciones completas con el AD2 dentro de mi envoltorio de `OE` | Las dos dan `HOST VERDICT=PASS` con 15/15. En T0b(c), `bus contention check failed at step 4: BUSCHK up gave 0x5A`, con los tres `OE` en 0x00. Las 17 escrituras del bus caen en el paso 9. `Rev` de CM4 verificado. T15 (c): 35 718 las dos veces. Capturas: `audit_r4_selftest_1` y `_2` |
| 7 | `test_real_capture_matches_protocol_table` con **mi** captura de #6 como la más reciente, y otra vez con una copia en la que cambié `STEP id=6 bus=NONE` por `bus=MCU` | Con la original, OK. Con la alterada, `Mismatch at index 11 (step6_announcement)`. La copia está borrada |
| 8 | En la placa, el firmware con el cambio 1 y el host del mismo commit | `[20:44:22] STEP id=7` → `[20:44:22] ABORT step=7 reason=protocol` → `[20:44:22] ERROR … ABORT received from firmware`, y `HOST VERDICT=FAIL`. En la reauditoría 3, el mismo caso tardaba 10 s. Captura: `audit_r4_r3m1_table_primitive` |
| 9 | Producción (`debug`) de `51a29b7` y `ca569b6`, en la misma ruta, comparadas byte a byte | CM4 idéntica. CM7: solo cambian los bytes del hash en 0x1257C, 5 y no 7, porque las dos cadenas coinciden en dos posiciones |
| 10 | `git grep -F -c` de los 21 identificadores de la tabla del informe, más `rec_measure` y `k_step_bus_`, sin contar `docs/`, `AUDIT.md` ni el propio informe | Ninguno da 0 |

La placa quedó con `prj_04` **debug** de `51a29b7` flasheado y el AD2 cerrado (S2).

### Estado de los hallazgos

| ID | Estado | Por qué |
| --- | --- | --- |
| R2-M3 | **Cerrado** | #2, #4, #5 y #7. Un desacuerdo entre la tabla y el host, el modelo o el firmware se ve en las pruebas o en la compilación, y el firmware lo detecta además en la placa (#8) |
| R3-m1 | **Cerrado** | #8 y `test_r3_m1_host_aborts_immediately_on_firmware_abort` |
| M3 | **Cerrado** | #10. Las correcciones (a)–(c) son exactas, y los resultados de §2.1 que da el informe coinciden con los míos (#4). Hay un detalle menor en la tabla de identificadores: pone `hs_mcu` y `hs_host` en `gpio_bench_protocol.h`, pero están en `gpio_selftest.c` y en los `.py` (N17) |

### Protocolo A1–A27 (cierre de F0)

| # | Punto | Resultado | Nota |
| --- | --- | --- | --- |
| A1–A3, A5, A7 | Catálogo, literales, API, targets, estado | Pasa | `gpio_bm.[ch]` sin cambios en todo F0 |
| A16 | Build | Pasa | Captura completa del implementador (`gpio_f0_build_9209955.txt`, 8/8) y mis compilaciones de `selftest-gpio` y `debug` |
| A17 | R3: binario | **Pasa** | #9 |
| A18 | Pruebas repetidas por mí | **Pasa** | #6 |
| A19 | R1: controles negativos | **Pasa** | T0b(c) con guarda rota (reauditoría 3, #8). Mutaciones de veredicto (reauditoría 2). Mutaciones de tabla (#4, #8). T15 (reauditoría 1) |
| A20 | Mapa de escritores | Pasa (inventario) | La migración es de F1b |
| A21 | R2: trazabilidad | Pasa | #1 |
| A22 | R4: no destructivas | Pasa | |
| A23 | Oráculo independiente | Pasa | |
| A26 | Documentación | Pasa | M3 cerrado; N17 |
| A27 | Seguridad eléctrica S1–S10 | **Pasa** | Criterios de código comprobados antes de conectar el AD2, y T0b entero en verde en mi ejecución, incluido (c) |

### Hallazgos que pasan a F1a

| ID | Dónde | Hallazgo |
| --- | --- | --- |
| R4-m1 | `test_gpio_hw_check.py:873-901` | `test_real_capture_matches_protocol_table` elige la captura por `mtime`. En un clon nuevo, `git` pone a todos los archivos más o menos la misma fecha, así que la prueba puede coger una captura antigua de otro formato (`gpio_f0_selftest_a692afc.txt`, por ejemplo) y fallar sin motivo. La captura tiene que venir dada: por argumento o variable de entorno, o como la del commit más reciente según `git log`, no según el sistema de archivos |
| R4-m2 | `CM7 gpio_selftest.c:24`, `:51-61` | El campo `bus_name` de `k_step_perms` ya no lo usa nadie. Los 11 `_Static_assert` repiten los valores de la tabla como literales: sirven de cerrojo, pero obligan a cambiar dos sitios. Se puede comprobar cada `k_step_perms[i]` contra `bench_get_step_bus_owner()` en una prueba de arranque, o quitar la duplicación |

#### Notas

- **N17:** En la tabla de identificadores del informe, la columna de archivos es aproximada (el caso de `hs_mcu`/`hs_host`). Los identificadores existen, que es lo que pedía §2.3.
- **N18:** F0 ha necesitado cinco rondas. En las tres primeras correcciones, el informe describió como hecho algo que el código no hacía; lo que lo cortó fue la lista de `git grep` de §2.3 de la cuarta. Esa regla queda para todas las fases siguientes.

### Cierre de F0

- **Lo que F0 deja verificado:**
  - la base de build (8/8);
  - la seguridad eléctrica del banco sin resistencias en serie (S1–S10, con controles negativos en la placa);
  - el protocolo host–firmware atado a una sola tabla;
  - T15 como control negativo de la carrera entre núcleos (G1);
  - la referencia de producción T18.

  Todo con `gpio_bm.[ch]` sin cambios y la producción idéntica a `ca569b6`.
- **F1a puede empezar**, sobre `driver/gpio` desde este commit, con R4-m1 y R4-m2 en su lista.
- **Actualizo en el mismo cambio que este informe:** el aviso de `IMPLEMENTER_INSTRUCTIONS.md` y la fila de `00_gpio` en `DRIVER_ROADMAP.md` §1.
- **Merge a `main`:** el plan lo prevé al cierre de la auditoría. Lo dejo a decisión del responsable.
