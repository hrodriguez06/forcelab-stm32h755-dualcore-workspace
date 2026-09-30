# Instrucciones para F1a de 00\_gpio: núcleo `gpio_bm`

Sep 29, 2026 · auditor · referencias: `IMPLEMENTATION_PLAN.md` (revisión 2 con sus erratas, incluida la E8 de F1a), `AUDIT.md` ("Reauditoría 4 de F0 (cierre)", commit `c1653c6`)

F0 está cerrado. El banco, el protocolo host–firmware, la tabla de pasos, el arnés y la referencia T18 están verificados en la placa. F1a es la primera fase que **cambia el driver**: el contrato de F1a del plan, sus pruebas y B1. Al pasar su auditoría, **la API nueva queda congelada**, porque SPI e I2C van a construir sobre ella.

**Siguen vigentes:**

- [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md) entero: no auditas; no tocas `AUDIT.md`, el plan, `DRIVER_ROADMAP.md` ni ningún archivo de instrucciones; las capturas no se editan.
- Las reglas que salieron de F0, que ahora valen siempre:
  - **Nada oculto a git.** Antes de cada captura y en el informe va la salida de estos dos comandos, que tiene que estar vacía:

    ```bash
    git status --short
    git ls-files -v | grep -E '^[a-zS]'
    ```

  - **Si una regla no se puede cumplir tal como está escrita, para y pregunta.** No la rodees.
  - **Cada identificador que cite el informe** (función, prueba, macro, variable, archivo, mensaje) se comprueba con `git grep -c`, y la lista va en el informe. Ninguno puede dar 0.
  - **Una ejecución abortada también se commitea.**
  - **Un control negativo que no puede fallar invalida su prueba.**

---

## 0. Antes de empezar

1. **Crea una rama `driver/gpio` nueva desde `main`,** en el último commit que toca este archivo o posterior (`git log -1 --format=%h -- 01_shared_drivers/00_gpio/F1A_INSTRUCTIONS.md`). La de F0 ya está integrada y borrada. No reescribas la historia.
2. **Un commit por preocupación, citando el ID** del hallazgo o de la prueba, en el estilo del plan: `feat(gpio): add GPIO_BM_ConfigPin with pre-access validation (G5, C6)`. Mensajes escritos desde bash con heredoc.
3. **Decisiones abiertas del plan:** valen las opciones por defecto 1, 2 y 5.
   - La API nueva devuelve `GpioBm_Status_t`.
   - El lock es un hook sobre `HSEM_BM_TakeTimeout`, con un límite medido.
   - PA13 y PA14 se rechazan sin `allow_debug_pins`.

   Si quieres otra, pregunta antes.
4. **El AD2 no conduce nada** hasta tener los pasos 1–3 de §1 hechos y todas las pruebas del arnés en verde, incluidas las nuevas de §2.4. Mientras tanto queda en S2 o desconectado. Las pruebas sin AD2 (T1, T2, T15, T16, B1) sí se pueden ejecutar en la placa antes.

---

## 1. Alcance y orden

**Entra en F1a:**

- el contrato de F1a del plan: "Contrato de API: gpio\_bm (F1a)", con sus reglas 1–7;
- G3, G4, G5, G8 (lock, deinit, `ReadConfig`), G9, G10 y G15;
- los criterios C1, C6–C8 y C10–C13;
- las pruebas T1–T10, T15 con hook, T16 y B1;
- los menores que vienen de F0: R4-m1 y R4-m2.

**No entra en F1a:**

- el hook del proyecto en `shared_memory.c` y la migración de escritores (G1 y G13 en el proyecto): son de **F1b**;
- EXTI y SYSCFG: son de **F2**;
- `gpio_ll` y `gpio_hal`: están aplazados (C4).

En F1a, el hook de T15 es **de prueba** y vive en `gpio_selftest.c` (§2.3).

| Paso | Qué | Por qué en este orden |
| --- | --- | --- |
| 1 | R4-m1, R4-m2 | Son pequeños, y la prueba de la captura real se va a usar en todo F1a |
| 2 | El driver: el contrato de F1a | Es lo que se congela |
| 3 | El selftest sin AD2: T1, T2, T15, T16, B1 | Prueba el driver sin tocar el banco |
| 4 | La extensión del banco: los pasos nuevos de la tabla, el host, el modelo y el arnés para T3–T10 | La seguridad eléctrica, antes de conectar |
| 5 | Las pruebas con el AD2 en la placa | §3 |
| 6 | La regresión de producción (T18) y los tamaños | El driver ya se enlaza en producción (§2.6) |
| 7 | El informe y la entrega | §4 |

---

## 2. Qué hacer, y cómo tiene que quedar

### 2.1 Menores de F0 (paso 1)

- **R4-m1.** `test_real_capture_matches_protocol_table` no elige la captura por `mtime`. La captura se le da explícitamente, por argumento o por variable de entorno, o bien la prueba toma la del commit más reciente según `git log`. Si no hay captura del protocolo actual, la prueba lo dice y se salta; no falla con una captura de otro formato.
- **R4-m2.** Quita el campo `bus_name` de `k_step_perms`, que ya no usa nadie. Quita también la duplicación de los dueños del bus: en vez de 11 `_Static_assert` con literales, una sola fuente o una comprobación que no obligue a cambiar dos sitios.

### 2.2 El driver (paso 2)

Implementa el contrato tal como está en el plan: tipos, constantes y firmas **exactamente** como allí. Lo que audito línea a línea:

- **`gpio_bm.h` solo añade (C2).** Antes de entregar, comprueba con `git diff ca569b6 -- 01_shared_drivers/00_gpio/gpio_bm.h` que no cambia ni se borra ninguna línea existente. La salida va en el informe.
- **Validación antes del primer acceso (C6, regla 1).**
  - Orden fijo: `NULL`, puerto, pin, campos, pin SWD.
  - Un rechazo no toca ningún registro, **ni el reloj del puerto**.
  - Puerto válido quiere decir de GPIOA a GPIOK y alineado al stride (G4), con un `_Static_assert` de `GPIOK − GPIOA = 10 × 0x400`.
- **Legacy (C2, G3).**
  - `GPIO_BM_ConfigAlternate` y `GPIO_BM_ConfigAnalog` conservan sus firmas y sus `bool`.
  - Rechazan `pull > 2` y `speed > 3`.
  - Son envolturas de la API nueva.
  - Devuelven `false` si el pin está bloqueado o si el read-back no coincide.
  - Las llamadas de `uart_*` y `adc_*` dejan los mismos bits que en `ca569b6` (T2).
- **Sección (C10, regla 2).**
  - PRIMASK guardado antes que el hook; anidamiento por núcleo; el hook solo en el nivel 1.
  - `ERR_BUSY` restaura todo.
  - `SectionExit` usa `__set_PRIMASK(saved)`, **nunca** `__enable_irq()`.
  - Sin sección en `EnablePortClock`, `ReadConfig`, `IsLocked`, `Read*` ni en las operaciones de pin.
- **Orden de escritura (C11, regla 4):**
  1. comprobar `LCKR`;
  2. `BSRR` si es OUTPUT;
  3. `OTYPER`, `OSPEEDR`, `PUPDR`;
  4. `AFR` si es AF;
  5. `MODER`;
  6. read-back de los cinco campos del pin.

  En `DeinitPin`, `MODER = analógico` va primero.
- **Honestidad (C12).** `ERR_LOCKED` sin escribir nada; `ERR_VERIFY` si el read-back no coincide.
- **LCKR (regla 5).**
  - La secuencia de RM0399 §12.4.8, con accesos de 32 bits y dentro de la sección.
  - Después, se relee `LCKK`: si no vale 1, `ERR_VERIFY`.
  - Ningún bucle sin límite (C13).
- **Estado (C7).** En el `.map`, `gpio_bm.c.obj` tiene `.data = 0` y `.bss ≤ 8 B`: solo el puntero al hook y el contador de anidamiento, por núcleo.
- **Dependencias (C8).** Solo `stm32h7xx.h`: ni HAL, ni LL, ni FreeRTOS, ni `hsem_*`. Tiene que funcionar antes del árbol de reloj, porque `01_led` depende de eso.
- **Sin literales del proyecto (C1, regla 7).** Ningún `GPIOB`, `5U` ni `BOARD_` en `gpio_bm.[ch]`. Lo compruebo con `grep`.
- **Operaciones de pin.** Son `static inline`. `Set`, `Reset`, `Write` y `WritePort` son **un solo `STR`** a `BSRR`; `Toggle`, una lectura de `ODR` y un `STR`; `Read` y `ReadPort`, una sola lectura de `IDR`. Lo pruebas con objdump de las siete (B1).
- **Commits.** Sepáralos por preocupación: status y tipos; validación; sección y hook; `ConfigPin` y derivadas; LCKR; `DeinitPin` y `ReadConfig`; legacy como envoltura.

### 2.3 Selftest sin AD2 (paso 3)

Todo va bajo `GPIO_DRIVER_SELFTEST`, en `CM7/App/Src/gpio_selftest.c` y `CM4/App/Src/gpio_selftest.c`, igual que en F0. Cada prueba sigue su fila del plan, incluida la columna "Control", que tiene que poder fallar.

- **T1 (rechazos).**
  - Cada caso de la fila, con el **estado exacto** esperado (`ERR_PARAM` o `ERR_DEBUG_PIN`).
  - Una foto de `MODER/OTYPER/OSPEEDR/PUPDR/AFR/ODR` de A..K, más `RCC_AHB4ENR` y `LCKR`, antes y después: tienen que ser idénticas.
  - **Control:** una llamada válida sí cambia la foto.
- **T2 (paridad).**
  - La matriz de la fila, en PF6–PF11 (sin cablear).
  - En CM7, la referencia es la imagen de registros que deja `HAL_GPIO_Init` con la misma configuración; en CM4, una tabla de campos de RM escrita a mano.
  - Incluye las llamadas legacy de `uart` y `adc`.
  - **Control:** una foto alterada a propósito se detecta.
- **T15 (carrera entre núcleos, en PF10/PF11).** Conserva todo lo que se verificó en F0 (sesión, esperas por `CYCCNT`, estado inicial conocido, solapamiento contado) y añade fases, cada una con su resultado impreso:
  - **(a) y (b):** cada núcleo solo, con RMW a mano → 0 corrupciones. Es el control del control.
  - **(c):** los dos a la vez, con RMW a mano → más de 0, con solapamiento mayor que 0 en los dos núcleos. Es el control.
  - **(d):** los dos a la vez con `gpio_bm` **sin** hook → más de 0 corrupciones, con solapamiento. PRIMASK no protege entre núcleos, y por eso existe F1b.
  - **(e):** los dos a la vez con `gpio_bm` **con el hook de prueba** → 0 corrupciones, y el total de llamadas igual a la suma de las privadas.
  - **El hook de prueba:**
    - usa `HSEM_BM_TakeTimeout` sobre `BOARD_GPIO_HSEM_ID` (de `shared_memory.h`), con el límite en ciclos DWT escrito y justificado;
    - se instala con `GPIO_BM_SetSharedLock` solo durante (e) y se desinstala al terminar;
    - no va en `gpio_bm`, ni en `shared_memory.c`, ni en `main.c`.
  - **(f) y (g): el mismo núcleo.** Prueban el PRIMASK, que (e) no puede probar: con el hook, el HSEM ya ordena a los dos núcleos, y en (a)–(e) no hay ninguna ISR que toque el puerto. CM4 queda parado, como en (a). CM7 reconfigura PF10 en bucle, y una interrupción periódica de CM7 reconfigura PF6 (sin cablear), alternando entre dos configuraciones.
    - La interrupción es de un timer que no use ninguna otra prueba, con su frecuencia escrita, y se deshabilita al terminar.
    - Al entrar, la ISR comprueba que PF6 sigue como la dejó la entrada anterior; si no, cuenta una corrupción. También cuenta cuántas veces interrumpió al bucle.
    - **(f):** el bucle con RMW a mano, sin PRIMASK → más de 0 corrupciones. Es el control: prueba que la ISR cae dentro de la ventana y que el detector la ve.
    - **(g):** el bucle con `gpio_bm` sin hook → 0 corrupciones.
  - **Veredicto de T15:** (a) = (b) = 0, (c) > 0, (d) > 0, (e) = 0, (f) > 0, (g) = 0. Solapamiento > 0 en los dos núcleos en (c), (d) y (e), e interrupciones del bucle > 0 en (f) y (g).
- **T16 (LCKR).**
  - `LockPins` en PF7–PF9 y, después, `ConfigPin` sobre ellos: `ERR_LOCKED`, registros sin cambio y `LCKK` = 1. El estado que devuelve cada llamada se imprime tal cual.
  - **Control:** un pin no bloqueado sí cambia.
  - **Es la última prueba de la imagen,** porque `LCKR` solo se borra con un reset. El siguiente arranque tiene que empezar con los pines sin bloquear: compruébalo y anúncialo al principio del selftest.
- **B1 (medidas).** Solo se registran, pero van **todas** al informe:
  - ciclos de `ConfigPin` con y sin hook;
  - tiempo máximo con PRIMASK y con el lock, en cada núcleo;
  - objdump de las siete operaciones de pin de §2.2.

  De aquí sale `SHARED_GPIO_LOCK_US` para F1b, con el margen escrito.

### 2.4 Extensión del banco para T3–T10 (paso 4)

Estas pruebas usan el AD2. Van por el **mismo** camino que F0: la tabla de `gpio_bench_protocol.h`, el host, el modelo, el arnés, las guardas S1–S10 y `s5_verify_released` en cada traspaso del bus.

- **Versión del protocolo.** `GPIO_BENCH_PROTOCOL_VERSION` pasa a 2. El banner la anuncia, y el host rechaza la v1.
- **Pasos nuevos en `GPIO_BENCH_STEPS_TABLE` y en la tabla de ítems,** cada uno con su dueño del bus y con sus permisos en `k_step_perms` (S4). Lo que sigue es lo mínimo. Cualquier línea que el plan no asigne se pregunta antes de usarla.

  | Prueba | Quién conduce | Dueño del bus | Qué mide el AD2 |
  | --- | --- | --- | --- |
  | T3 | MCU: el bus, con `Set`/`Reset`/`Write`/`WritePort`/`Toggle` | MCU | Analizador lógico en el bus |
  | T4 | AD2: los 256 patrones en el bus, con estrobo en `HOST_REQ` | AD2, con testigo de S5 | Nada: el MCU lee con `ReadPort`/`Read` y el host calcula el CRC32 |
  | T5, T6, T9 | MCU: PF14 y PF15 | NONE | Scope 1 y 2 |
  | T7 | AD2: 0xFF en el bus | AD2, con testigo de S5 | Nada: el MCU lee `IDR` en analógico y en entrada |
  | T8 | MCU: una línea "solo MCU" (PE5 o PE6), nunca el bus | NONE | Analizador lógico con trigger en cualquier flanco |
  | T10 | MCU: PA8 en AF0 (MCO1); PA8 solo se permite en este paso y en el walking-1 de T0a (errata E1) | NONE | La frecuencia en DIO14 |

- **`tools/ad2.py`.** Añade lo que haga falta (analizador lógico, frecuencia, scope a 100 MS/s para T9) **sin** ningún camino nuevo hacia `OutputEnableSet`. Toda escritura de `OE` sigue pasando por `ad2_set_oe`.
- **El control de cada prueba** (columna "Control" del plan) es un paso del protocolo o una variante del mismo paso, y tiene que dar FAIL cuando debe:
  - T3: un valor equivocado a propósito;
  - T4: un patrón invertido y el CRC distinto;
  - T7: en entrada se lee 0xFF;
  - T8: el orden ingenuo, con el pulso visto;
  - T10: AF1, sin señal;
  - T5 y T6: las condiciones de validez de su fila.
- **El arnés (`fw_model.py` y las pruebas):**
  - cada paso nuevo en el modelo;
  - los escenarios E8 y E10 del arnés cubren solos las líneas y las primitivas nuevas, porque salen de la tabla;
  - los escenarios E15 y E16 del arnés, los de un MCU que no suelta el bus, se amplían a T4 y T7;
  - un escenario por control, en el que el control falla y el host da FAIL;
  - el invariante eléctrico sigue en todas las pruebas.
- **Los cuatro cambios de tabla de la reauditoría 4** siguen poniendo pruebas en rojo. Repítelos sobre los pasos nuevos (cambiar una primitiva, un patrón, un dueño del bus y el orden de dos líneas) y pon en el informe qué cae en cada caso.

### 2.5 En la placa (paso 5)

- Procedimiento operativo S9 del plan, con `--checklist-confirmed`.
- **Dos ejecuciones completas** de `selftest-gpio` (F0 + F1a), la segunda tras un reset sin tocar SRAM4:
  - `docs/gpio_f1a_selftest_<hash>.txt`;
  - `docs/gpio_f1a_selftest_<hash>_2.txt`.
- Los valores de referencia de T5, T6 y T9 están en sus filas del plan. Si un valor sale fuera, **no ajustes el umbral**: para y apúntalo.

### 2.6 Producción (paso 6)

A diferencia de F0, **F1a cambia código que se enlaza en producción**: `gpio_bm.c` lo usan `uart` y `adc`. Por eso ya no se exige la paridad byte a byte. En su lugar:

- **Diff del `.map` por objeto,** entre la producción (`debug` y `release`) de F0 (`c1653c6`) y la de F1a: solo pueden cambiar `gpio_bm.c.obj` y el total. Cualquier otro objeto que cambie se explica o se corrige. El método va en el informe.
- **C7 en el `.map`:** `.data` y `.bss` de `gpio_bm.c.obj`, en los dos núcleos.
- **T18 de regresión,** con la imagen de producción de F1a, grabada como en F0: cuatro archivos crudos (consola con marcas de tiempo, `net_check.py`, `query_adc.py --wave --count 300` y el volcado SWD de contadores). Tiene que dar lo mismo que la referencia de `90327d1`: 300 `WAVE`, burst 100/100 y contadores a 0. Además, como pide la fila T18 del plan, los códigos LED de arranque correctos y los selftests UART y ADC en verde: son los consumidores de las envolturas legacy. `mX_gpio_lock_fail_count` no existe hasta F1b. **Esta regresión la añado a la puerta de F1a** (errata E8 del plan), porque la producción cambia en esta fase y no en F1b.

### 2.7 El informe (paso 7)

En `IMPLEMENTATION_REPORT.md`, una sección "F1a" según la plantilla de `IMPLEMENTER_INSTRUCTIONS.md` §5, con:

- los commits;
- el build con los tamaños frente a F0;
- cada prueba con su resultado y su control, con el valor que dio;
- **B1** entero;
- los fragmentos de objdump y de `.map`;
- la salida de `git diff ca569b6 -- …/gpio_bm.h`;
- las desviaciones y las erratas del plan que encuentres;
- lo no ejercitado;
- la salida de §0 (árbol limpio);
- la **lista de `git grep -c`** de los identificadores que cites.

Sin lenguaje de veredicto.

---

## 3. Seguridad eléctrica

- **Sin resistencias en serie, nada limita la corriente en una contienda.** Las reglas S1–S10 del plan se aplican a cada paso nuevo exactamente igual que en F0:
  - un conductor por línea como mucho;
  - traspaso del bus solo con `s5_verify_released`;
  - un testigo por paso;
  - S2 ante cualquier fallo.
- **Antes de conectar el AD2 a la imagen de F1a, repasa tú la lista de A27 del plan** sobre el código nuevo:
  - una sola llamada a `OutputEnableSet`;
  - toda configuración en salida o AF pasando por `bench_claim_*`;
  - cada traspaso del bus haciendo S5.
- **Si algún paso nuevo parece necesitar que el MCU y el AD2 conduzcan la misma línea, para y pregunta.**

---

## 4. Qué entregar

1. El rango de commits en la rama `driver/gpio` nueva, con el árbol limpio de verdad.
2. `docs/gpio_f1a_build_<hash>.txt`: la salida completa de `tools/build_all.ps1 -Clean`, 8/8, en ASCII o en UTF-8 sin BOM.
3. `docs/gpio_f1a_unittest_<hash>.txt`: todas las pruebas del arnés.
4. Las dos capturas de §2.5 y cualquier ejecución abortada.
5. Los cuatro archivos de T18 de §2.6, y el diff del `.map` por objeto.
6. `IMPLEMENTATION_REPORT.md` con la sección F1a.
7. Un aviso con el hash. Después esperas a la "Auditoría de F1a" en `AUDIT.md`.

---

## 5. Qué voy a comprobar

Además del protocolo A1–A27 del plan entero, haré lo siguiente:

| Qué | Cómo | Tiene que dar |
| --- | --- | --- |
| C2 | `git diff ca569b6 -- gpio_bm.h` | Solo líneas añadidas |
| C1, C8 | `grep` de literales del proyecto y de `#include` en `gpio_bm.[ch]` | Nada |
| C7 | `.map` de producción y de `selftest-gpio` | `.data = 0`, `.bss ≤ 8 B` |
| C10 | Mutación mía: sin PRIMASK | T15 (g) con más de 0 corrupciones |
| C10 | Mutación mía: el hook sin llamar | T15 (e) con más de 0 corrupciones |
| C11 | Mutación mía: `MODER` antes que `BSRR` | T8 ve el pulso |
| C12 | Mutación mía: sin comprobar `LCKR` | T16 falla: da `ERR_VERIFY` en vez de `ERR_LOCKED` (el read-back ve la escritura ignorada) |
| C12 | Mutación mía: sin comprobar `LCKR` y sin read-back | T16 falla: la escritura ignorada devuelve `OK` |
| C6 | Mutación mía: validación después del reloj del puerto | T1 ve cambiar `RCC_AHB4ENR` |
| T2 | Una foto de referencia alterada | T2 falla |
| Operaciones de pin | objdump de mi build | Un `STR` por operación |
| Banco | Los cuatro cambios de tabla sobre los pasos nuevos, y una contención inyectada en T4 | Pruebas en rojo |
| A18 | Dos ejecuciones mías completas con el AD2 | Igual que las tuyas, con el mismo hash |
| Producción | Mi diff del `.map` por objeto, y T18 con mi imagen | Solo cambia `gpio_bm.c.obj`; T18 igual que la referencia |
| Informe | `git grep` de cada identificador, y cada afirmación contra el código | Ninguna afirmación sin fuente |

Si pasa, la API de F1a queda congelada y F1b puede empezar.

---

## 6. Cuándo parar y preguntar

Las situaciones de `IMPLEMENTER_INSTRUCTIONS.md` §7, y además estas:

- Algún tipo o firma del contrato no se puede implementar tal como está escrito.
- T15 (d) da 0 corrupciones **con** solapamiento. Puede ser que el driver cierre la ventana por casualidad; no lo des por bueno.
- T15 (f) da 0 corrupciones. Como en (c), se ensancha la ventana o se sube la frecuencia de la ISR; nunca se da por bueno.
- T2 no encuentra una referencia fiable para algún caso en CM4.
- Un paso del banco necesita una línea o una dirección que la tabla "Quién conduce" del plan no prevé.
- El diff del `.map` de producción muestra cambios fuera de `gpio_bm.c.obj`.
- Algún valor de T5, T6 o T9 sale del rango de referencia.
