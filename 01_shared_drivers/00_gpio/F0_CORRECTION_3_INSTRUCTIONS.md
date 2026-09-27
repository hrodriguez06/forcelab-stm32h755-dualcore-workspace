# Instrucciones para la tercera corrección de F0 de 00\_gpio

Sep 27, 2026 · auditor · referencia: `AUDIT.md`, "Reauditoría 2 de F0" (commit `35766da`)

La entrega `74984a5` no pasó la reauditoría 2. Aun así, casi todo quedó cerrado. La sesión completa con el AD2 pasa en la placa, y la he repetido yo dos veces. **No toques lo cerrado** salvo lo que pida este documento: B1, B2, B3, B5, M1, M2, m1–m7, R-B1, R-B2, R-m1–R-m3 y N8.

Queda:

- **un bloqueante:** B4;
- **cuatro mayores:** R2-M1, R2-M2, R2-M3 y M3;
- **seis menores:** R2-m1–R2-m6.

**Siguen vigentes:**

- [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md): no auditas; no tocas `AUDIT.md`, `IMPLEMENTATION_PLAN.md`, `DRIVER_ROADMAP.md` ni los archivos de instrucciones; las capturas no se editan.
- [`F0_CORRECTION_INSTRUCTIONS.md`](F0_CORRECTION_INSTRUCTIONS.md) y [`F0_CORRECTION_2_INSTRUCTIONS.md`](F0_CORRECTION_2_INSTRUCTIONS.md). Donde este documento diga otra cosa, manda este.

**Lee antes** la reauditoría 2 entera: cada hallazgo tiene sus líneas y su captura.

---

## 0. Antes de empezar

1. **Trabaja sobre `driver/gpio` desde `35766da` o posterior.** No reescribas la historia: nada de `rebase`, `--amend` ni `push --force`.
2. **Una corrección por commit, citando el ID:** `fix(tools): run T0b(c) through s5_verify_released (B4)`. Mensajes escritos desde bash con heredoc.
3. **Nunca ocultes el estado del árbol a git (R2-M1).**
   - Nada de `update-index --assume-unchanged`, `--skip-worktree`, `.git/info/exclude`, ni ningún otro mecanismo que haga que `git status` no muestre un cambio.
   - Si encuentras cambios sin commitear que no son tuyos, **para y pregunta**. No los commitees y no los escondas.
   - Antes de cada captura, y en el informe, va la salida de estos dos comandos, que tiene que estar vacía:

     ```bash
     git status --short
     git ls-files -v | grep -E '^[a-zS]'
     ```

4. **El AD2 no conduce nada hasta terminar los pasos 1–3 de §1,** con todas las pruebas en verde. Mientras tanto, el AD2 queda en S2 o desconectado.
5. **Si una regla no se puede cumplir tal como está escrita, para y pregunta (§6).** No la rodees. B4 lleva tres entregas abierto porque se rodeó.

---

## 1. Orden de trabajo

| Paso | Hallazgos | Por qué en este orden |
| --- | --- | --- |
| 1 | R2-M2 | Las pruebas nuevas de B4 solo valen si el arnés detecta una contención |
| 2 | B4 (+ R2-m5) | El único bloqueante |
| 3 | R2-M3 (+ R2-m4) | La tabla por paso; con ella, E8 y E10 se generan en vez de escribirse a mano |
| 4 | R2-m1, R2-m2, R2-m3, R2-m6 | Menores del host y del linker |
| 5 | M3 | Informe |
| 6 | Ejecución en la placa y entrega | §3 y §4 |

---

## 2. Qué corregir, y cómo tiene que quedar

### 2.1 R2-M2: el invariante eléctrico tiene que poder poner una prueba en rojo

Hoy `ElectricalContentionError` salta dentro de `run_selftest`, que la captura y la convierte en un FAIL corriente. Una prueba que espera FAIL no la distingue de cualquier otro fallo.

- **`SimBus` guarda cada violación** en una lista, con el instante, las líneas afectadas y los dos `OE`. Puede seguir lanzando la excepción, pero la lista es lo que manda.
- **Todas las pruebas comprueban que la lista está vacía,** además del veredicto. Que no se pueda olvidar: por ejemplo, el banco simulado se registra al crearse y un `tearDown` común lo comprueba en cada banco que se haya creado.
- **Control del control:** una prueba que provoca una contención en el modelo (el MCU conduce el bus mientras lo conduce el AD2 en el paso 9) y comprueba que la lista **no** está vacía y que la comprobación común la detecta. Es lo que yo hice en la reauditoría 2, apartado D de `audit_r2_host_sim_74984a5.txt`.

### 2.2 B4: T0b(c) prueba la guarda de verdad

La guarda que protege el bus es `s5_verify_released`. T0b(c) existe para demostrar que para lo que tiene que parar. Por eso tiene que ejecutarse **esa función**, no una copia de su lógica.

**Host, paso 4 (T0b(c)):**

1. Llama a `s5_verify_released(ad2, tracker, STEP_T0B_S5_NEG, line_reader, …)`, la misma función de los pasos 8 y 9.
2. El patrón anunciado se toma de la **misma** línea `BUSCHK up drive=0x..` que lee `s5_verify_released`. Por ejemplo, `BusContention` lleva el valor leído y el valor anunciado.
3. **PASS solo si se cumplen las tres cosas:**
   - salta `BusContention`;
   - el valor leído es igual al anunciado;
   - los tres `OE` del paso (al empezar, antes de leer, después del aborto) tienen 0 en el bus. Los tres van al log.
4. **Cualquier otro resultado es FAIL:** un `BenchAbort` por timeout, ninguna excepción, u otro valor.
5. El log dice lo que pasó de verdad. Hoy imprime "via s5_verify_released()" sin llamarla.

**Quién decide si la sesión sigue.** Hoy `s5_verify_released` llama a `tracker.abort()`, y eso impediría seguir después de T0b(c). La regla nueva es esta:

- **`s5_verify_released` no decide sobre la sesión.** Ante un fallo invalida el testigo, pone el AD2 en S2 y lanza la excepción. Nada más.
- **Solo el manejador de T0b(c), y solo con `BusContention`,** captura la excepción y sigue: vuelve a poner `HOST_REQ` como salida estática y hace el `hs_mcu` que espera el firmware.
- **En cualquier otro paso,** o con cualquier otra excepción, la excepción sube y la sesión aborta, como ahora. El seguimiento de pasos se aborta en ese punto, fuera de la función.

**Firmware (R2-m5).** T0b(c) solo imprime "safe state verified: all wired pins input" si la comprobación ha pasado. Si no, imprime una línea de FAIL, que el host tratará como tal.

**Escenarios nuevos en el arnés.** Cada uno comprueba el veredicto, `OE` = 0 al final y la lista de violaciones vacía.

| # | Escenario | Tiene que dar |
| --- | --- | --- |
| E15 | En el paso 8, el MCU no suelta el bus: `BUSCHK up` ≠ 0xFF, y en otra ejecución `BUSCHK down` ≠ 0x00 | Aborto de la sesión, FAIL y **ninguna** escritura de `OE` con bits del bus en toda la ejecución |
| E16 | Lo mismo en el paso 9 | Aborto antes de cualquier `drive_bus`, FAIL y ninguna escritura de `OE` con bits del bus |
| E17 | Mutación de la guarda: la comparación de `BUSCHK up` desactivada, y en otra ejecución la de `BUSCHK down` | En cada caso, al menos uno de E6, E15 o E16 deja de dar lo esperado. La prueba E17 comprueba justamente eso |

En E17, la mutación se hace sobre una copia del módulo cargada aparte, sin tocar el archivo. Por ejemplo, se lee el fuente, se sustituye la comparación y se carga con `importlib`. Si no ves una forma limpia de hacerlo, pregunta.

### 2.3 R2-M3: la tabla por paso, como dato y con una sola fuente

Hoy el protocolo está escrito a mano tres veces: en el firmware, en el host y en el modelo. El aborto que tuviste en `b3c7e2d` salió de ahí.

**Qué va en la tabla, para cada paso,** en orden:

- las líneas de protocolo que imprime el firmware (texto exacto, o su expresión regular);
- las primitivas (`HS_MCU` o `HS_HOST`);
- los puntos de medida, dichos con un nombre: por ejemplo, `HOST_READ loose_up` o `MCU_READ ad2_bit`;
- las repeticiones de los walking-1: "repetir 11 veces" y "repetir 10 veces", con la lista de bits.

**Dónde va y quién la lee:**

- **Una sola fuente:** la tabla está en `gpio_bench_protocol.h` como datos, por ejemplo como una X-macro.
- **El host y el modelo la leen de la cabecera al cargarse,** con un parser pequeño; no llevan una copia a mano.
- **El código de cada paso** en el host y en el modelo recorre la tabla. Solo aporta lo que es propio del paso: qué medir en cada punto de medida y con qué compararlo.
- **El firmware comprueba la tabla en tiempo de ejecución.** `hs_mcu()` y `hs_host()` reciben, o calculan, la posición dentro del paso y comprueban que la primitiva es la que dice la tabla en esa posición. Si no coincide: `bench_safe()`, `[m7_gpio_test] ABORT step=<n> reason=protocol` y fin de la sesión. Así, un desacuerdo entre el firmware y la tabla se ve en la placa, no solo en el arnés.

**Pruebas que salen de la tabla (R2-m4):**

- **E8:** se quita cada línea de protocolo de la tabla, una por ejecución. No una lista escrita a mano.
- **E10:** cada primitiva de la tabla, una por una, agota el tiempo.
- **E11:** cada comparación del host, una por una, con un bit cambiado, y la prueba comprueba que el log nombra **ese** bit. Son todas: sueltas en pull-up y en pull-down, handshake, walking-1 MCU→AD2, su control, y scope en bajo y en alto.
- **Testigo de un paso anterior:** falta la prueba de §2.4 de las segundas instrucciones.
- **Una prueba que lee la tabla de la cabecera** y comprueba que el host y el modelo la recorren igual.

**Si el formato de tabla no te sirve para algún paso,** pregunta antes de empezar (§6). No hagas una tabla a medias con excepciones escritas a mano.

### 2.4 Menores del host y del linker (paso 4)

- **R2-m1:** después de abrir el puerto y antes del reset, se lee y se descarta todo lo pendiente durante al menos 100 ms, hasta que no llegue nada. El log dice cuántos bytes se descartaron.
- **R2-m2:**
  - La línea `[m4_gpio_test] Rev …` se comprueba **en cuanto llega**. Si no coincide, aborto.
  - Las líneas de CM4 se evalúan también cuando la sesión aborta, para que la lista final muestre el estado real de CM4.
- **R2-m3:**
  - El log no puede tirar la sesión por la codificación: salida con `errors="replace"`, o reconfigurada a UTF-8.
  - Una prueba lo demuestra: una línea con un byte inválido, con la salida estándar en cp1252, no lanza ninguna excepción.
- **R2-m6:**
  - En los dos linker scripts, un `ASSERT` que compruebe que `ORIGIN(RAM_SHARED)` y `LENGTH(RAM_SHARED)` coinciden con `SHARED_RAM_BASE` y `SHARED_RAM_SIZE`. Los valores pueden ir escritos en el `.ld`, pero con un comentario que apunte a `shared_memory.h`.
  - Un `ASSERT` no genera código. Aun así, repite la comparación byte a byte de la producción con `ca569b6` y pon el método en el informe.

### 2.5 M3: el informe (paso 5)

En `IMPLEMENTATION_REPORT.md`, una **sección nueva**, "Respuesta a la reauditoría 2". Las anteriores no se tocan. Lleva:

- **Una tabla** con B4, R2-M1, R2-M2, R2-M3, M3 y R2-m1–R2-m6. Para cada uno: el commit, qué ejecutaste para comprobarlo y el estado "corregido, pendiente de verificación".
- **La corrección de las siete afirmaciones de M3 (a)–(g)** de la reauditoría 2, cada una con su fuente.
- **La ejecución abortada de `b3c7e2d`.** Si tienes su salida, se commitea tal cual. Si no la tienes, el informe lo dice.
- **La salida de los dos comandos de §0.3.**
- **Lo que no está hecho,** dicho explícitamente.

**Ninguna frase describe lo que hace el código sin que el código lo haga.** Antes de escribir "T0b(c) llama a…", compruébalo con `grep`. El mensaje de entrega no dice nada que no esté en el informe.

---

## 3. Cómo ejecutar en la placa (paso 6)

Solo con los pasos 1–5 hechos y todas las pruebas en verde, incluidas E15–E17 y el control de §2.1:

1. **Árbol limpio,** con la salida de §0.3 vacía. Compila `selftest-gpio` en el commit que vas a capturar.
2. **Procedimiento operativo S9** del plan, con `--checklist-confirmed`.
3. **Dos ejecuciones completas**, la segunda tras otro reset sin tocar SRAM4:
   - `docs/gpio_f0_selftest_<hash>.txt`;
   - `docs/gpio_f0_selftest_<hash>_2.txt`.
4. **Cualquier ejecución abortada se commitea** con su nombre (`_abort1`, …) y se cita en el informe. No se borra para quedarse con la buena.
5. **La salida completa de `tools/build_all.ps1 -Clean`,** desde la primera línea hasta la línea de resultado, en ASCII o en UTF-8 sin BOM: `docs/gpio_f0_build_<hash>.txt`. Por ejemplo, desde bash:

   ```bash
   powershell -NoProfile -File tools/build_all.ps1 -Clean > docs/gpio_f0_build_<hash>.txt 2>&1
   ```

6. **T18 no se regraba,** salvo que la comparación de §2.4 (R2-m6) muestre algún byte distinto del hash.

---

## 4. Qué entregar

1. El rango de commits nuevo en `driver/gpio`, con el árbol limpio de verdad (§0.3).
2. `docs/gpio_f0_build_<hash>.txt`: la salida completa, 8/8.
3. `docs/gpio_f0_unittest_<hash>.txt`: todas las pruebas, E1–E17 y el control de §2.1.
4. Las dos capturas de §3 y cualquier ejecución abortada.
5. `IMPLEMENTATION_REPORT.md` con la sección "Respuesta a la reauditoría 2".
6. Un aviso con el hash. Después esperas a la "Reauditoría 3" en `AUDIT.md`.

---

## 5. Qué voy a comprobar en la reauditoría 3

| Hallazgo | Cómo lo compruebo | Se cierra si |
| --- | --- | --- |
| B4 | Código. Mi mutación de la reauditoría 2 (las dos comparaciones desactivadas) contra tus pruebas. En la placa: T0b(c) en mi ejecución completa, y luego el host con la comparación de `BUSCHK up` desactivada, antes de lo cual compruebo por lectura que no puede conducir el bus | T0b(c) llama a la guarda y solo pasa por `BusContention` con el patrón anunciado. La mutación pone pruebas en rojo, y en la placa da T0b(c) FAIL con `OE` = 0 |
| R2-M1 | `git ls-files -v` y `git status` en el commit entregado | Nada oculto |
| R2-M2 | Mi inyección de contención del apartado D contra tu arnés | La prueba se pone en rojo por el invariante, no solo por el veredicto |
| R2-M3 | Cambio yo una primitiva de un paso en la tabla de la cabecera | El host y el modelo la siguen sin tocar su código. El firmware, recompilado con la tabla cambiada pero con el código de sus pasos igual, aborta en la placa con `reason=protocol`. Las pruebas derivadas de la tabla cambian solas |
| R2-m1–R2-m6 | Código, pruebas y comparación byte a byte | Lo pedido en §2.2 y §2.4 |
| M3 | El informe contra el código y las capturas | Ninguna afirmación sin fuente |
| A18, A27 | Dos ejecuciones mías completas con el AD2, en frío | Mis resultados coinciden con los tuyos, con el mismo hash |

---

## 6. Cuándo parar y preguntar

Las situaciones de las instrucciones anteriores, y además estas:

- Encuentras cambios sin commitear que no son tuyos (§0.3).
- Usar `s5_verify_released` en T0b(c) choca con el seguimiento de pasos de una forma que §2.2 no resuelve.
- El formato de la tabla no sirve para algún paso (§2.3).
- La mutación de E17 no se puede hacer sin tocar el archivo del script.
- El `ASSERT` del linker cambia algún byte de la producción.
