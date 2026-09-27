# Instrucciones para la segunda corrección de F0 de 00\_gpio

Sep 27, 2026 · auditor · referencia: `AUDIT.md`, "Reauditoría 1 de F0" (commit `5190d96`)

La entrega `d472beb` no pasó la reauditoría 1. T15 (B1) y la referencia T18 (B5) quedan cerrados, y no se tocan salvo lo que se pide en §2.7. Este documento dice qué falta, en qué orden, cómo tiene que comportarse y qué voy a comprobar.

**Siguen vigentes:**

- [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md): no auditas; no tocas `AUDIT.md`, `IMPLEMENTATION_PLAN.md`, `DRIVER_ROADMAP.md` ni los archivos de instrucciones; las capturas no se editan.
- [`F0_CORRECTION_INSTRUCTIONS.md`](F0_CORRECTION_INSTRUCTIONS.md), con la errata E6 ya aplicada en su §2.1. Donde este documento diga otra cosa, manda este.

**Lee antes** la reauditoría 1 entera. Cada hallazgo tiene su línea de código y su captura.

---

## 0. Antes de empezar

1. **Trabaja sobre `driver/gpio` desde `5190d96` o posterior.** No reescribas la historia: nada de `rebase`, `--amend` ni `push --force`.
2. **Una corrección por commit, citando el ID:** `fix(tools): queue serial lines instead of dropping them (R-B2)`. Mensajes escritos desde bash con heredoc.
3. **El AD2 no conduce nada hasta terminar los pasos 1–5 de §1,** con las pruebas de §2.1 en verde. Mientras tanto, el AD2 queda en S2 o desconectado. Puedes ejecutar T15 sin el AD2, y también el script contra imágenes que deben abortar en el banner.
4. **Si una regla de estas instrucciones o del plan no se puede cumplir tal como está escrita, para y pregunta (§6).** No la rodees. Es lo que pasó con el testigo de un solo uso (E6): la respuesta correcta era preguntar, no fabricar testigos.
5. **No borres ni edites capturas anteriores.** Las nuevas llevan el hash nuevo.

---

## 1. Orden de trabajo

| Paso | Hallazgos | Por qué en este orden |
| --- | --- | --- |
| 1 | Arnés de pruebas del host (§2.1) y R-B2 | Sin un lector de líneas que no pierda nada, y sin una forma de probar el script entero sin hardware, ninguna de las demás correcciones se puede comprobar |
| 2 | R-B1 | Un solo handshake, definido una vez, para el firmware y para el host |
| 3 | B3 | Timeouts, testigos, dueño del bus y banner: la seguridad eléctrica |
| 4 | B4 | El control de la guarda del bus |
| 5 | B2 | Veredictos que pueden fallar |
| 6 | M1, m2, R-m1, R-m2, R-m3, N8 | Guardas del firmware y menores |
| 7 | M3 | Informe |
| 8 | Ejecución en la placa y entrega | §3 y §4 |

---

## 2. Qué corregir, y cómo tiene que quedar

### 2.1 Arnés del host: el script entero sin hardware (paso 1)

Hoy las pruebas unitarias prueban piezas sueltas, y algunas ni eso (S8b copia el parser dentro de la prueba). En la reauditoría, un arnés de 100 líneas bastó para que el script diera `HOST VERDICT=PASS` con un firmware mudo. Tiene que ser al revés: el arnés es tuyo y va en el repo.

**Inyección de dependencias.** `run_selftest` recibe, en lugar de crearlos dentro:

- el puerto serie;
- el AD2 (o su DLL);
- el reloj (`monotonic` y `sleep`);
- la función de reset;
- el hash esperado.

`main()` pasa los reales. Las pruebas pasan los simulados. **Nada de `monkeypatch` sobre el módulo.**

**Un modelo del firmware** (`prj_04/tools/fw_model.py`, biblioteca estándar):

- Ejecuta la misma tabla de pasos y los mismos handshakes que el firmware (§2.3), sobre las DIO simuladas y el puerto simulado.
- Imprime las mismas líneas, con el mismo formato.
- Lleva el estado de cada pin del MCU (entrada, salida, analógico, pull) y el valor que conduce.

**Un invariante que el arnés comprueba en cada paso de reloj, en todos los escenarios:** ninguna línea tiene dos conductores. Es decir, nunca hay a la vez un bit en el `OE` del AD2 y el mismo pin del MCU en salida. Si se viola, la prueba falla, dé el script el veredicto que dé.

**Escenarios obligatorios.** Cada uno comprueba el veredicto, el código de salida y que el `OE` acaba en 0.

| # | Escenario | Tiene que dar |
| --- | --- | --- |
| E1 | Modelo correcto | `HOST VERDICT=PASS`, código 0 |
| E2 | Modelo correcto, pero el puerto entrega todo lo pendiente en una sola lectura, y otra vez de byte en byte | PASS en los dos casos (R-B2) |
| E3 | El firmware se calla después de arrancar la sesión | FAIL, sin esperar 120 s |
| E4 | `T15 VERDICT: FAIL` | FAIL |
| E5 | `CM4 VERDICT=FAIL`, o no llega `CM4 VERDICT` | FAIL |
| E6 | En T0b(c) el MCU no conduce el bus (lee 0xFF en `BUSCHK up`) | FAIL de T0b(c) |
| E7 | En T0b(c) no llega `BUSCHK up` | FAIL de T0b(c) (timeout, no PASS) |
| E8 | Se pierde una línea de protocolo cualquiera: se repite con cada una | Aborto a S2 y FAIL, nunca PASS |
| E9 | Un `STEP` con un ID fuera de secuencia, o con otro dueño del bus | Aborto antes de tocar el bus |
| E10 | Cada handshake, uno por uno, agota el tiempo | Aborto a S2, FAIL, y ninguna escritura posterior en el AD2 salvo `OE = 0` |
| E11 | Un bit cambiado en cada comparación del host: sueltas, handshake, walking-1 en los dos sentidos, controles y scope. Uno por ejecución | FAIL, y el log nombra ese bit |
| E12 | El firmware da FAIL en cada una de sus comparaciones (`T0a_ad2 … FAIL`, control analógico) | FAIL |
| E13 | Banner con otro hash, `-dirty`, otra versión, o el `Rev` de CM4 distinto del de CM7 | Aborto en el banner, con `OE` = 0 en todo momento |
| E14 | Una línea ajena con `Rev ` antes del banner (por ejemplo, la de arranque `=== … Rev <hash> ===`) | No cuenta como banner |

Los tres escenarios de la reauditoría (captura `audit_r1_host_sim_d472beb.txt`) son E3, E2+E6 y E7.

Las pruebas actuales de S8a–S7b siguen, pero **llamando al código del script**, no a una copia. La salida de `py -m unittest` va en `docs/gpio_f0_unittest_<hash>.txt`.

### 2.2 R-B2: un lector de líneas con cola

- **Una sola clase** lee el puerto, parte en líneas y las mete en una cola. Todo el que necesita una línea la saca de esa cola, en orden. Nadie más llama a `ser.read()`.
- **Cada línea se registra una vez, en el orden de llegada.**
- **Tres tipos de líneas:**
  - **Protocolo:** empiezan por `[m7_gpio_test]` y no son `INFO`. El host las espera **en el orden exacto** que dice la tabla del paso (§2.3). Si llega otra, se aborta a S2.
  - **Informativas:** `[m7_gpio_test] INFO …`. Se registran y se saltan. El firmware pasa a este prefijo las líneas que el host no necesita, como `T15 phase (a)` o `Waiting for HOST_REQ`.
  - **Del resto:** `[m4_gpio_test]`, `[ipc]`, `[net]`… Se registran. Las de `[m4_gpio_test]` se guardan para el veredicto (§2.6).
- **Esperar una línea es `expect(regex, timeout)`,** que devuelve la línea o lanza el aborto. No existe ninguna espera que devuelva `None` y deje seguir.

### 2.3 R-B1: un handshake, definido una vez

**Dos primitivas, y ninguna más.** Las dos con 10 s de timeout en cada fase y en los dos lados.

| Primitiva | Secuencia |
| --- | --- |
| `HS_MCU` (el MCU avisa) | MCU sube `MCU_ACK` → host sube `HOST_REQ` → MCU baja `MCU_ACK` → host baja `HOST_REQ` |
| `HS_HOST` (el host avisa) | host sube `HOST_REQ` → MCU sube `MCU_ACK` → host baja `HOST_REQ` → MCU baja `MCU_ACK` |

**La tabla por paso** va en `gpio_bench_protocol.h`, con su espejo en el script. Para cada paso dice, en orden, qué líneas de protocolo se imprimen, qué primitivas se hacen, y qué mide cada lado entre una y otra.

- El firmware y el host recorren esa misma tabla.
- **Nadie mueve `HOST_REQ` ni `MCU_ACK` fuera de una primitiva:** se acaban los "pulsos para resincronizar".
- En el host, la tabla es un dato que el código recorre, no un `if` por paso con los pulsos escritos a mano.

**El paso 7 (líneas del handshake)** se define con las mismas primitivas:

1. `HS_HOST`: el MCU comprueba y anuncia que leyó `HOST_REQ` a 1 cuando lo subió el host.
2. `HS_MCU`: el host comprueba que leyó `MCU_ACK` a 1 y después a 0.

Con eso las dos líneas se prueban en los dos niveles. Si necesitas otra forma, pregunta (§6).

**Timeouts del firmware (S7).** Si una primitiva agota el tiempo, el firmware:

1. llama a `bench_safe()`;
2. anuncia `[m7_gpio_test] ABORT step=<n> reason=timeout`;
3. **no ejecuta ningún paso más**, y termina con `VERDICT: GPIO SELF-TESTS FAILED`.

La única espera que se agota a propósito es la de T0b(d), y la tabla la marca como tal.

### 2.4 B3: el host cumple S5, S7 y S8

**S7, timeouts.** Toda espera (`expect`, las fases de `HS_MCU`/`HS_HOST`, `wait_mcu_ack`) que agota el tiempo lleva a S2, a un `ABORT: …` en el log y a código de salida ≠ 0. Ningún valor de retorno de una espera se ignora. Lo prueba E10.

**S7, secuencia.** La secuencia esperada empieza en el paso 1 (T15) e incluye el 2 (IDLE). El host valida también esos dos anuncios.

**S5, testigos (errata E6):**

- `BusToken` solo lo crea `s5_verify_released`. Por ejemplo, con un objeto centinela privado del módulo que el constructor exige. `BusToken(9)` desde fuera lanza una excepción.
- El testigo es válido mientras el seguimiento de pasos esté en su paso y no haya habido ningún aborto. Al cambiar de paso o al abortar, queda invalidado.
- `drive_bus(ad2, tracker, value)` toma el testigo y el dueño del bus del seguimiento de pasos, no de sus argumentos. Si el paso actual no es "bus = AD2" o no hay testigo válido: excepción **antes** de la DLL.
- Pruebas unitarias: un testigo fabricado, un testigo de un paso anterior y un testigo después de un aborto, los tres rechazados antes de la DLL.

**S8, banner:**

1. El puerto serie se abre **antes** del reset. Se vacía lo que hubiera en el búfer, se resetea con `reset_mcu_safe`, y solo cuentan las líneas posteriores.
2. El banner son exactamente `^\[m7_gpio_test\] Rev ([0-9a-f]{7})$` y, después, `^\[m7_gpio_test\] GPIO_BENCH v(\d+) idle$`, las dos antes de cualquier salida. Ninguna otra línea cuenta (E14).
3. `^\[m4_gpio_test\] Rev ([0-9a-f]{7})$` tiene que coincidir con el mismo hash antes del veredicto. Si no coincide, se aborta.
4. `reset_mcu_safe` lanza la excepción si el CLI falla o no devuelve 0. Hoy la convierte en un aviso.

### 2.5 B4: T0b(c) solo pasa por la razón correcta

- **En T0b(c) el firmware anuncia el patrón que conduce:** `[m7_gpio_test] BUSCHK up drive=0x5A`.
- **`s5_verify_released` distingue el motivo del aborto** con una subclase, por ejemplo `BusContention(BenchAbort)`, que lleva el valor leído. Un timeout es otra excepción.
- **PASS de T0b(c) solo si se cumplen las tres cosas:**
  1. salta `BusContention`;
  2. el valor leído es el patrón anunciado;
  3. el `OE` leído tiene 0 en el bus en los tres puntos del paso: al empezar, justo antes de leer y después del aborto. Los tres valores van al log.
- **Cualquier otro resultado es FAIL,** timeout incluido.
- **Después viene el handshake que diga la tabla, y ninguno más.** El MCU hace `bench_safe()`, comprueba que todos los pines cableados están en entrada y lo anuncia.

### 2.6 B2: veredictos que pueden fallar

**El veredicto del host es una lista explícita,** impresa al final ítem por ítem. `HOST VERDICT=PASS` solo si están todos:

- la secuencia completa de pasos observada, hasta `DONE`;
- `T15 VERDICT: PASS`;
- `CM4 VERDICT=PASS`, con el `Rev` de CM4 correcto;
- `VERDICT: ALL GPIO SELF-TESTS PASSED` del firmware;
- cada comparación del host en OK;
- ningún aborto y ningún timeout, salvo el de T0b(d), que es a propósito.

El valor inicial es FAIL. No hay ningún camino por el que salir del bucle deje el veredicto como estaba.

**La prueba de sueltas compara las 13 líneas:** bus, DIO8, DIO9, DIO10, DIO11 y DIO14. Con pull-up, la palabra entera en 1; con pull-down, entera en 0. El log imprime la palabra leída y la esperada.

**El firmware no imprime `PASS` en lo que no mide.** En sueltas, MCU→AD2 y scope, el firmware imprime `DONE (host verdict)`. Su `VERDICT` global solo incluye lo que mide él: T15, T0b(b), T0b(d), el handshake desde su lado, T0a AD2→MCU y su control, y CM4.

### 2.7 Guardas del firmware y menores (paso 6)

**M1:**

- En CM7, `s_current_step_id` solo lo escribe `bench_enter_step`.
- Para que T0b(b) pueda probar otros pasos sin tocar esa variable, separa la decisión en una función pura, `bench_step_allows(step_id, port, pin)`, que consulta la tabla. `bench_claim_output(port, pin)` la llama con el paso actual, y T0b(b) la llama con los pasos que quiere probar.
- En CM4, lo mismo: tabla constante, `bench_enter_step` y la función pura. CM4 solo tiene el paso T15, con PF11. **PE6 sale de CM4**: en F0 lo conduce solo CM7.

**m2.** Aserciones con las constantes de `shared_memory.h` y `hsem_selftest_shared.h`, no con literales:

- `SHARED_RAM_BASE + sizeof(SharedMemory_t) <= HSEM_TEST_SHARED_ADDR`;
- `HSEM_TEST_SHARED_ADDR + sizeof(<struct de HSEM>) <= GPIO_TEST_SHARED_ADDR`;
- `GPIO_TEST_SHARED_ADDR + sizeof(GpioSelfTestShared_t) <=` el final de `RAM_SHARED`.

Si hace falta una constante nueva, se define en `shared_memory.h` y el linker la usa o la comprueba.

**R-m1.** Cada núcleo publica sus resultados, hace `__DMB()` y **después** pone su `in_loop` a 0. Las dos esperas finales de T15 miran su timeout: si expira, FAIL con la causa.

**R-m2.** Una línea de protocolo no se puede perder en silencio.

- Bajo `#if GPIO_DRIVER_SELFTEST`, `App_Log` no descarta la línea: espera el mutex sin límite, o cuenta los descartes y el selftest da FAIL si la cuenta no es 0.
- **La imagen de producción no puede cambiar.** Tras el cambio, repite la comparación byte a byte con `ca569b6` y pon el método en el informe. Si cambia algún byte fuera del hash, hay que regrabar T18.

**R-m3.** La máscara del control MCU→AD2 es `0xFFFF3FC0`. El pull-down de PE2 no se toca.

**N8.** `cm7_ready`: o se usa o se quita.

### 2.8 M3: el informe (paso 7)

En `IMPLEMENTATION_REPORT.md`, una **sección nueva**, "Respuesta a la reauditoría 1". Las anteriores no se tocan. La sección lleva:

- **Una tabla** con cada hallazgo abierto o nuevo: B2, B3, B4, M1, M3, m2, R-B1, R-B2, R-m1, R-m2, R-m3 y N8. Para cada uno: el commit, qué ejecutaste para comprobarlo (con la captura o el escenario del arnés) y el estado "corregido, pendiente de verificación".
- **La corrección de las cuatro afirmaciones de M3 (a)–(d),** cada una con su fuente:
  - el paso 7 no ejecutado;
  - PA8 según el UM2408 y la desviación real del paso `10U`;
  - lo que hace de verdad la prueba citada en B4;
  - la captura que faltaba de B1.
- **Lo que no está hecho,** dicho explícitamente.

El mensaje de entrega no dice nada que no esté en el informe. Nada de "implementado y verificado": verificar lo hago yo.

---

## 3. Cómo ejecutar en la placa (paso 8)

Solo con los pasos 1–7 hechos y los 14 escenarios del arnés en verde:

1. **Compila la imagen `selftest-gpio` en el commit que vas a capturar,** con el árbol limpio. Las capturas se commitean después, así que su hash será el del commit anterior. Está bien, siempre que entre ese commit y el entregado no cambie código: el informe lo dice, con `git diff --stat`.
2. **Procedimiento operativo S9** del plan, con `--checklist-confirmed`.
3. **Primera ejecución completa** de `gpio_hw_check.py --log docs/gpio_f0_selftest_<hash>.txt`.
   - Orden: T15, T0b (a, b, c, d) y T0a (sueltas, handshake, walking-1 en los dos sentidos con sus controles, scope).
   - La captura empieza en la lista S9 e incluye el banner de CM7, porque el puerto ya se abre antes del reset.
   - Termina en `HOST VERDICT=` con la lista de §2.6.
4. **Segunda ejecución tras otro reset, sin tocar SRAM4:** `docs/gpio_f0_selftest_<hash>_2.txt`.
5. **Si algo aborta o sale raro:** para, deja el AD2 en S2 y apúntalo en el informe antes de repetir. Una ejecución abortada también se commitea: no se borra para quedarse con la buena.
6. **T18 no se regraba,** salvo que cambie la imagen de producción (§2.7, R-m2).

---

## 4. Qué entregar

1. El rango de commits nuevo en `driver/gpio`, con el árbol limpio.
2. `docs/gpio_f0_build_<hash>.txt`: `tools/build_all.ps1 -Clean` 8/8, en ASCII o UTF-8 (redirige desde bash o usa `Out-File -Encoding utf8`).
3. `docs/gpio_f0_unittest_<hash>.txt`: las pruebas del host y los 14 escenarios del arnés.
4. Las dos capturas de §3, y cualquier ejecución abortada.
5. `IMPLEMENTATION_REPORT.md` con la sección "Respuesta a la reauditoría 1".
6. Un aviso con el hash. Después esperas a la "Reauditoría 2" en `AUDIT.md`.

---

## 5. Qué voy a comprobar en la reauditoría 2

| Hallazgo | Cómo lo compruebo | Se cierra si |
| --- | --- | --- |
| R-B2 | Mi arnés de la reauditoría 1 contra el script nuevo, más E2 del tuyo | Ninguna línea se pierde ni se desordena, se lea el puerto como se lea |
| R-B1 | Lectura de la tabla de pasos en los dos lados y ejecución en la placa | Un solo handshake por paso, idéntico en los dos lados; la ejecución completa no tiene timeouts |
| B3 | Código, tus escenarios E8–E10 y E13–E14, y mis propios casos de fallo. En la placa, otra vez las tres imágenes que deben abortar | Cada espera fallida aborta; no se puede fabricar un testigo; el banner es el de CM7, con el puerto abierto antes del reset; `OE` = 0 hasta el banner |
| B4 | E6 y E7, y la ejecución con el AD2 | T0b(c) solo pasa por contención con el patrón anunciado; el bus nunca se habilita |
| B2 | E3–E5 y E11–E12. En la placa, una build mía que invierte un bit esperado en el firmware y otra que calla el firmware a mitad | Cada una da `HOST VERDICT=FAIL` |
| M1, m2, R-m1–R-m3, N8 | Lectura del código, T0b(b) en mi ejecución y la compilación | Lo pedido en §2.7 |
| M3 | El informe contra el código y las capturas | Ninguna afirmación sin fuente, y lo no hecho dicho como tal |
| A18, A27 | Repito yo la ejecución completa con el AD2, dos veces, en frío | Mis resultados coinciden con los tuyos, con el mismo hash, y ningún invariante eléctrico se incumple |

---

## 6. Cuándo parar y preguntar

Las situaciones de `IMPLEMENTER_INSTRUCTIONS.md` §7 y de `F0_CORRECTION_INSTRUCTIONS.md` §6, y además estas:

- Una regla de estas instrucciones no se puede cumplir tal como está escrita. Lo que pasó con E6.
- El paso 7 no se puede definir con las dos primitivas de §2.3.
- El modelo del firmware no puede reproducir algo que el firmware hace de verdad. Eso suele querer decir que el firmware hace algo que la tabla no dice.
- Un cambio de §2.7 cambia la imagen de producción.
