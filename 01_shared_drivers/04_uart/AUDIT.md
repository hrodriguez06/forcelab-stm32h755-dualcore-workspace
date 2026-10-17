# Auditoría de `04_uart`

## Banco (2026-10-17, antes de la entrega)

**El responsable cableó JU1 (PD5 → 1 kΩ → PD6), JU2 (PD4 → PD3) y DIO15 → PD6, y revisó a ojo que la resistencia es de 1 kΩ.**

Comprobación del auditor:
- **Método:**
  - por SWD (HOTPLUG, `sn=`), escrituras solo en `GPIOD` `MODER`/`PUPDR`/`ODR`, autorizadas por el responsable y restauradas al final;
  - el AD2 solo como entrada (OE = `0x0000` todo el tiempo);
  - PD5 = PD4 en 1, 0, 1, 0.
- **Resultado:**
  - PD6 y PD3 siguen a PD5 y PD4;
  - DIO15 ve PD6, **4/4 PASS**;
  - los registros quedaron restaurados (`MODER` `0xFFFAFFFF`, `PUPDR` `0x00050000`, `ODR` `0x00000000`).
- **Lo que no mide:** la resistencia. UT0 lo hará con el AD2 conduciendo PD6.

## F1 (`76eb6d7`, informe en `01ef495`)

**Cómo la revisé:**
- el diff del driver, yo;
- dos revisiones de solo lectura: el selftest, el host y `ad2.py`; y `uart_checks.py` con sus capturas;
- las afirmaciones bloqueantes de esas revisiones, comprobadas contra el código;
- T1/T2 en ejecución propia, con `OVERALL VERDICT: PASS`. Pero ver T-B1–T-M4: esa ejecución no demuestra lo que dice.

### El driver (U1–U6)

**Corregido:**
- **U1:** `uart_hal.c:162-176`.
- **U2/U3:** fase 1 en `uart_ll.c:205-289`, antes de cualquier escritura.
- **U4:** `CR1`–`CR3` a 0 tras `Disable`.
- **U5:** el divisor aplicado (`uart_ll.c:154-164`).
- **U6:** `GPIO_BM_ConfigPin`, con `ERR_BUSY` → `HAL_BUSY`; los pines van antes del reloj.

| ID | Gravedad | Hallazgo | Qué se pide |
| --- | --- | --- | --- |
| D1 | Menor | **LL ahora rechaza LPUART con OVER8** (`uart_ll.c:262-266`). Antes ese campo se ignoraba en LPUART, y BM lo sigue ignorando (`uart_bm.c:362`). Una llamada que antes funcionaba ahora falla | Ignorarlo en LPUART, como BM y como antes |
| D2 | Menor | **El AF de los pines no se valida en la fase 1** (LL `:269-287`, HAL `InitEx`), así que un AF de RX inválido deja TX configurado | `af ≤ 15` en la validación previa |
| D3 | Menor | **`UART_HAL_InitEx` valida y configura TX antes de validar RX** | Validar los dos pines, y después configurar |

### El selftest y el host (H2/H3): no se pueden ejecutar en la placa

| ID | Gravedad | Hallazgo | Qué se pide |
| --- | --- | --- | --- |
| S-B1 | Bloqueante | **El host no tiene camino de hardware:**<br>• fuera del mock, termina con "physical hardware execution prohibited for implementer" (`uart_hw_check.py:623-625`);<br>• no hay apertura del AD2, ni reset, ni comprobación de `Rev`/build id, ni armado del LA, ni `finally` a S2/SU3.<br>Prohibirte *ejecutar* contra la placa no es prohibirte *escribir* esa ruta | La sesión real, como `gpio_hw_check.py`:<br>• el AD2 como gestor de contexto;<br>• reset con el AD2 en S2;<br>• banner con `Rev` y build id de los dos núcleos, comprobados;<br>• S2/SU3 en `finally`;<br>• probada con el mock por la **misma** ruta |
| S-B2 | Bloqueante | **El host y el firmware se desincronizan en UT4/UT4_CM4:**<br>• el firmware no imprime veredicto de UT4 y pasa a `UT4_CM4` (`uart_selftest.c` CM7:772-788);<br>• el host espera el veredicto de UT4 (`:398-399`), se traga `STEP UT4_CM4` y aborta;<br>• `STEPS` (`:58-73`) está definido pero no se usa;<br>• el mock lo oculta, porque su guion tiene una línea que el firmware no imprime (`:509`) | Un veredicto por paso, y el host recorre `STEPS`. El mock reproduce la salida real del firmware, generada a partir de `k_ust_steps` |
| S-B3 | Bloqueante | **UT3 no tiene oráculo:**<br>• `la_read` sin `la_arm`, antes de que el firmware transmita (`:391-395`);<br>• `bit_period_samples = 16` fijo;<br>• paridad por defecto `"none"`, así que nunca falla;<br>• sin tramas, PASS;<br>• no hay medida contra MCO1, y la imagen no enciende MCO1;<br>• el firmware no imprime `PRESC`, y solo prueba BM | Ver la revisión 2 del plan, UT3 |
| S-B4 | Bloqueante | **UT5 no existe.** El firmware hace un `Init`, `TakeErrors() == 0` y `Deinit` (CM7:792-831). El host pone OE en DIO15 y lo quita sin mandar nada (`:408-414`). Además, falta en el plan cómo generar tramas temporizadas en DIO15: con E/S estática desde Python no se llega a 104 µs por bit, y el generador de patrones solo se permite en DIO1/DIO3 (S11) | Ver la revisión 2 del plan, SU1 y UT5 |
| S-B5 | Bloqueante | **UT2 no tiene oráculo y fallará:**<br>• el host solo lee la línea PASS (`:384-386`); no hay fórmula propia;<br>• el firmware no lee `PRESC` ni llama a LL, así que U5 no se ejercita;<br>• la fila "divisor impar pequeño" da 18, que es par (CM7:511);<br>• todas las filas van con `enable_clock = false` y esas instancias no tienen reloj en `prj_01`, así que `Init` devuelve `false` (sin `TEACK`) | Reloj encendido; BM y LL; por fila, `f_ker`/baud/OVER8/`PRESC`/`BRR` impresos; el host calcula con su fórmula sobre el divisor aplicado; filas con `USARTDIV` impar (17, 19…) donde se vea U5 |
| S-B6 | Bloqueante | **UT9 no puede ejercitar U1:**<br>• la consola ya está inicializada (`app_log.c:26`), y `HAL_UART_Init` solo llama a la MSP con `gState == RESET` (Cube `stm32h7xx_hal_uart.c:326`): con y sin la corrección da `HAL_OK`;<br>• con `--expect-control-u1`, cualquier fallo, incluido un timeout, cuenta como "esperado" (`:434-435`);<br>• nadie compila la imagen de control | Un handle nuevo (`gState = RESET`) para la instancia de la consola, con una instantánea de PD8/PD9. El control se construye en la ejecución (revisión 2 del plan) y tiene que dar exactamente la línea `HAL_OK` con el lock tomado |
| S-M1 | Mayor | **UT7 fallará y no es 8E1.** `UART_WORDLENGTH_8B` + `UART_PARITY_EVEN` (CM7:913-915) es 7E1 en HAL (máscara `0x7F`), así que `0xA5` vuelve como `0x25`. Tampoco hay instantánea de pines ni LA | `UART_WORDLENGTH_9B` para 8E1, más la instantánea y el LA |
| S-M2 | Mayor | **La instantánea de UT1 no puede fallar:**<br>• USART2 está sin reloj y todos los rechazos van con `enable_clock = false` y sin pines, así que ni U2 ni U3 cambiarían la instantánea;<br>• falta el caso central de U2: `UART_BM_DATA_BITS_7` en LL;<br>• falta la instancia desconocida en los tres tiers;<br>• HAL se compara con `== HAL_OK` y no con el estado exacto;<br>• el control es una comparación en línea, no la misma función | Reloj encendido, con pines y `enable_clock = true` en los rechazos; los casos que faltan; una sola función de instantánea para rechazos y control; estados exactos |
| S-M3 | Mayor | **UT6 no puede fallar.** `UART_LL_WriteByte` no bloquea y se ignora lo que devuelve (CM7:867), así que no se mandan N > FIFO + 1 bytes. No hay control sin control de flujo ni recuento en el LA | Escrituras comprobadas; bytes por fase; el control sin RTS/CTS da ORE; el LA cuenta las tramas |
| S-M4 | Mayor | **UT8 no tiene transferencia viva:** no hay USART2, ni `DMAT`, ni datos. El "ocupado" se fuerza con `EN` en un stream con `NDTR = 0` | Una transferencia real de USART2 TX por DMA1; `HAL_BUSY` mientras corre; bytes comprobados por RX y por el LA |
| S-M5 | Mayor | **A UT4 le faltan casos:** FIFO apagado, OVER8, la máscara de 7 bits y LL por sondeo; tampoco hay recuento en el LA ni control | Los casos del plan |
| S-M6 | Mayor | **UT10 fabrica evidencia.** CM7 imprime literalmente `[m4_uart_test] bm_loop=1,1,1 ll=1,1 ore=1` (CM7:1080) a partir de una reimplementación en `prj_01` | Quitarlo. UT10 es la ejecución real del selftest de `prj_04`, que hace el auditor |
| S-M7 | Mayor | **UT0 no comprueba nada:**<br>• PD6 queda sin pull y sin TX, con un nivel indefinido;<br>• los veredictos del firmware son `true` constantes (CM7:344, 353);<br>• UT0_CTRL baja PD5 durante 5 ms tras el handshake y el host lee una vez después: depende de la latencia | PD6 alto por TX en reposo o pull-up; la condición se mantiene hasta el siguiente handshake; el veredicto sale de lo que lee el AD2 |
| S-M8 | Mayor | **S4/SU2 no se aplican a los pines del driver:**<br>• `bench_claim` solo se llama desde `pin_cfg`, y cada `Init` del driver configura PD5/PD6/PD8/PD9 sin claim;<br>• CM4 no tiene claim;<br>• la regla permite PD5 en AF en UT5 (`uart_selftest_common.c:25`), al revés que SU2 | Claim de cada pin antes de cada `Init`, en los dos núcleos; en UT5, PD5 en entrada |
| S-M9 | Mayor | **SU1 confía en un literal y rompe A27:**<br>• el host pasa `"UT5"` a mano y no el paso actual de `STEPS`;<br>• `AD2_UART_INJECT_STEPS` lista nombres que no existen;<br>• `ad2_set_uart_oe` añade un **segundo** `FDwfDigitalIOOutputEnableSet` (`ad2.py:139`), cuando `ad2.py:7, 77, 105` exigen uno solo (A27);<br>• ante un error de relectura lanza la excepción sin volver a S2 (`:142-145`) | Ver la revisión 2 del plan, SU1: un solo punto de OE y un solo punto de Digital Out, con DIO15 solo si el paso actual de `STEPS` lo declara; S2 antes de lanzar |
| S-M10 | Mayor | **No se comprueba la identidad de la imagen.** CM7 imprime el `Rev`/build id de CM4 sin esperar a `m4_ready`, y el host no los analiza | Como `gpio_hw_check.py` (R3-a, R3-b, R4-a) |
| S-M11 | Mayor | **El mock no detecta los tres fallos de §3.5 por las funciones reales.** Llama a las funciones de decodificación y de guarda directamente, y la cuenta de menos es un `len(...) < 6` en línea | Los tres fallos, inyectados en la salida simulada del firmware y del LA, por la misma sesión que la ejecución real |
| S-M12 | Mayor | **El informe describe lo que el código no hace** (`IMPLEMENTATION_REPORT.md:149-155, 206`): fórmula propia, MCO1, recuentos, instantánea de UT7 e inyección real. Las conexiones de PE2/PE7 son erróneas (CN11-1/CN11-13; son CN9-14/CN11-46 y CN10-20/CN12-44, `00_gpio/README.md:556-557`) | Corregirlo y comprobar cada cita |
| S-m | Menor | Detalles sueltos:<br>• UT3 corta tramas a 9600 (se ignora `Flush` y espera 2 ms para 6,25 ms de tramas);<br>• `bench_safe` no limpia el pending de NVIC;<br>• CM4 no restaura los pines en sus salidas de error;<br>• se ignora el resultado de `LOCK_RELEASE`;<br>• índice fijo `step == 1U`;<br>• `UST_AREA_ADDR` sin origen;<br>• el host espera 2 s el ACK, y hay pasos de hasta 10 s;<br>• BM/LL solo en CM7, salvo UT4 | Corregir |

### La herramienta sin placa (`uart_checks.py`)

| ID | Gravedad | Hallazgo | Qué se pide |
| --- | --- | --- | --- |
| T-B1 | Bloqueante | **Los controles de T2 son constantes:**<br>• el de cabeceras es una cadena de diff literal (`:266-273`);<br>• el de símbolos, una lista de Python (`:309-311`);<br>• el código de salida de `nm` se ignora: si `nm` falla, una lista vacía da PASS (`:300-302`) | Cabecera copiada a un temporal con una firma cambiada, por `git diff --no-index` y `evaluate_header_diff`; `nm` con código 0 y ≥ 1 símbolo; un control positivo: los ELF de `selftest-uart` de la misma ejecución deben dar ≥ 1 fuga por la misma extracción |
| T-M1 | Mayor | **El árbol limpio no mira los ignorados** (`:110`: `git status --porcelain -u`, sin `--ignored`) | `--ignored`, comparado con la instantánea inicial y sin contar `build/` |
| T-M2 | Mayor | **T1 no comprueba que la imagen sea de esta ejecución:**<br>• `RUN_START_TIME` no se usa;<br>• basta un ELF de CM7 (`:208`);<br>• configura sin `--fresh` | Los dos ELF, con mtime ≥ el inicio de T1, `--fresh` y el número de objetos compilados |
| T-M3 | Mayor | **El patrón de símbolos no ve el selftest:**<br>• `\bUT[0-9]` nunca casa;<br>• `nm -g` oculta los `static`;<br>• `ust_step_index` y `k_ust_step_count` no casan;<br>• el informe dice que `ust_*` está cubierto | `nm` sin `-g` y un patrón que cubra `ust_`, `k_ust_`, `run_ut`, `UART_Selftest`, demostrado por el control positivo; también los ELF `debug` |
| T-M4 | Mayor | **La captura no dice de qué proyecto es cada línea de símbolos:** las 8 dicen `(release)` | La ruta relativa completa |
| T-m | Menor | Detalles sueltos:<br>• el control de T1 no es un preset (`-G Ninja` a mano), ignora el código de configuración y fija `elf_found = True`;<br>• la cabecera no comprueba que el pathspec case ≥ 1 archivo;<br>• `--test t2` solo no comprueba que los ELF sean de la ejecución | `cmake --preset debug -B <tmp> --fresh -DCMAKE_C_FLAGS=-Wconversion`, con al menos un aviso `[-Wconversion]`, y lo demás |

### El informe y las capturas

| ID | Gravedad | Hallazgo |
| --- | --- | --- |
| R1 | Mayor | **Los comandos del auditor no se pueden ejecutar:**<br>• `run_warm_resets.py` está en `tools/` de la raíz y necesita `--out`, `--title` y `--project`;<br>• `b41c4a7` no tiene el preset `selftest-uart`, así que la receta del control de UT9 no existe |
| R2 | Mayor | **§6 dice "sin desviaciones", y las hay:** los controles constantes; el control de T1, que no es un preset; solo ELF `release`; y `CM7/App/Src/app_main.c` modificado sin declararlo |
| R3 | Mayor | **La captura del mock (`uart_mock_593d720.txt`) se hizo con el host aún sin commitear:** se commiteó en `ae3e620`, y la captura no registra el estado del árbol |
| R4 | Menor | **El "antes" de U1 no es el de `b41c4a7`.** Además, §3.3 cita una comprobación de 7B + OVER8 que no existe, y §2 dice que el mock verificó el firmware |

### Estado

**F1 no pasa.** El driver está casi listo (D1–D3, menores). El selftest y el host son un esqueleto: no hay ruta de hardware, y UT0–UT9 no tienen oráculo o no pueden fallar. La herramienta sin placa tiene controles constantes.

Parte de la culpa es mía: el plan no decía cómo inyectar tramas en DIO15 ni que la imagen tenía que encender MCO1. La revisión 2 del plan lo añade.

**No se ejecuta nada en la placa hasta que la corrección pase la revisión.**

## F1, corrección 1 (`a39abd7`, informe en `48ffc6c`)

**Cómo la revisé:**
- el driver y `ad2.py`, yo;
- dos revisiones de solo lectura: el selftest y el host; y `uart_checks.py` con el informe;
- los bloqueantes, comprobados contra el código.

**Placa:** Antigravity la usó con permiso del responsable (mensaje del 2026-10-17), así que sus capturas `uart_dev_*` son evidencia de desarrollo. La regla de §2.2 de las instrucciones queda actualizada.

### Cerrado

**El driver:**
- D1 (LPUART ignora OVER8, como BM);
- D2 (AF ≤ 15 en la fase 1, en LL y HAL);
- D3 (`InitEx` valida los dos pines antes de configurar).

**El selftest y el host:**
- **Ruta de hardware:** existe, con el AD2 como gestor de contexto, reset en S2, `Rev`/build id comprobados y `finally` a S2.
- **Pasos:** un veredicto por paso, y el nombre del paso se comprueba.
- **Pasos reales:**
  - UT0 comprueba de verdad la resistencia (`IDR` y AD2);
  - UT5 inyecta de verdad PE, FE y ORE (sin FIFO) por Digital Out, y `TakeErrors` una sola vez;
  - UT7 es 8E1 de verdad (`9B`), con `HAL_BUSY`/`HAL_OK`;
  - UT8 tiene transferencia viva por DMA1 y `HAL_BUSY` mientras corre;
  - UT9 usa un handle nuevo y estado exacto.
- **UT2:** fórmula propia en el host, y filas con `USARTDIV` impar en LL (U5).

**`ad2.py`:** un solo punto de OE y uno de Digital Out, y S2 antes de cada excepción, también en el fallo de relectura.

**`uart_checks.py`:**
- **T-B1:** controles por las funciones reales: diff de una cabecera temporal; control positivo con los ELF de `selftest-uart`; `nm` con código y ≥ 1 símbolo.
- **T-M3:** `nm` sin `-g`, con un patrón que ve los `static`, y ELF `debug`.
- **T-M4:** rutas completas.

### Hallazgos nuevos o abiertos

| ID | Gravedad | Hallazgo | Qué se pide |
| --- | --- | --- | --- |
| C-B1 | Bloqueante | **UT10 es una constante.** CM4 pone `m4_ut10_ok = 1U` sin hacer nada (CM4 `uart_selftest.c:215-217`), y el informe lo llama "regresión de `prj_04`". Es S-M6 con otro nombre | Quitar UT10 de `prj_01` del todo. UT10 la ejecuta el auditor con `prj_04` |
| C-B2 | Bloqueante | **UT3 no puede fallar:**<br>• `la_arm(100e6, 4096)` captura 41 µs, y una trama a 115 200 dura 86,8 µs; con 868 muestras por bit fijas no cabe ninguna (`uart_hw_check.py:647-659`);<br>• la paridad siempre es `"none"`;<br>• `stop_ok` es verdadero si la parada cae fuera;<br>• no hay comparación de bytes;<br>• `mco_per_bit` no se usa, y la tolerancia solo se imprime;<br>• el LA se arma después del handshake, con el firmware ya transmitiendo;<br>• la frecuencia de MCO1 es un literal del firmware | La revisión 2 del plan, punto 5, más la regla de los oráculos de abajo |
| C-M1 | Mayor | **NE no se inyecta:** su patrón es el mismo que el de CLEAN (`:676`/`:678`), y el firmware solo lo registra, aunque el paso da PASS. Tampoco hay BREAK, ni ORE con FIFO, ni captura del patrón en el LA, ni comprobación de SU2 en el host | Un pulso más corto que un bit, con más velocidad de Digital Out; BREAK; ORE con FIFO; y si el hardware no marca NE, una línea explícita de "NE no detectado" que no cuenta como PASS |
| C-M2 | Mayor | **UT6 no ejercita el control de flujo:** `written = 16/20` cabe en el FIFO de 16, y nadie mira `written`. El control sin control de flujo usa otra configuración | N ≥ 20 bytes enviados de verdad y comprobados, y la **misma** secuencia con `hw_flow` apagado; recuento en el LA por fase |
| C-M3 | Mayor | **CM7 espera `m4_ready == 1U`, pero CM4 escribe la sesión** (CM7:1737 frente a CM4:177). La espera siempre agota sus 2 s, y el `Rev` de CM4 se lee por suerte | Comparar con `session`; FAIL si se agota |
| C-M4 | Mayor | **La instantánea de UT1 no ve U3:** USART2EN y el AF7 de PD5/PD6 ya están puestos antes, y faltan `PUPDR`/`OSPEEDR`. El control positivo es otra comparación | Partir de reloj apagado y pines en entrada; una sola función de instantánea (`MODER`, `AFR`, `PUPDR`, `OSPEEDR`, `CR1`–`CR3`, `BRR`, `PRESC`, `APB1LENR`) para rechazos y control |
| C-M5 | Mayor | **A UT2 le faltan casos:** todas las filas dan `PRESC = 0`, así que la búsqueda del prescaler no se ejercita; las filas de rechazo no llegan al host; con 0 filas, PASS; no hay fila mutada | Filas con `PRESC ≠ 0`; las filas de rechazo, impresas y juzgadas por la fórmula del host; un número esperado de filas; una fila mutada por la misma función |
| C-M6 | Mayor | **Las instantáneas de pines de UT7 y UT9 se calculan y se tiran** (`(void)`, CM7:1456-1457, 1661-1662) | Que cuenten: AF, pull-up y velocidad |
| C-M7 | Mayor | **El mock no recorre la misma sesión:** `sync` devuelve verdadero, `MockAD2` sustituye a `AD2` en vez de `AD2(dwf_lib = mock)`, las líneas del firmware están escritas a mano, y los fallos de §3.5 se prueban llamando directamente a las funciones | `AD2(dwf_lib = mock)`; la salida del firmware generada a partir de `k_ust_steps`; los fallos inyectados en esa salida y en las muestras del LA, por la sesión |
| C-M8 | Mayor | **El host no recorre `STEPS`, y el permiso de SU1 sale de literales:**<br>• `step == "UT0"`/`"UT5"` (`ad2.py:80-81, 106-107`);<br>• `step_name = "UT5"` (`:686`);<br>• `self.current_step or step_name` (`ad2.py:793`);<br>• `AD2_UART_INJECT_STEPS` está obsoleto | El host recorre `STEPS`, y su columna `dio15` (OE o DOUT) va a `ad2.set_step()`. `ad2.py` solo acepta esa columna, sin ramas por nombre ni de cadenas sueltas |
| C-M9 | Mayor | **`dout_custom` cambia el comportamiento para todos:** fija siempre `RepeatSet(1)` y `RunSet(...)` (`ad2.py:870-872`), y `tim_hw_check.py:1069` lo usa en `03_tim` | Opcional, por un parámetro que por defecto deja lo de antes; que el mock de `tim_hw_check.py` siga pasando |
| C-M10 | Mayor | **El reset del host no lleva `sn=`, y su salida se tira** (`uart_hw_check.py:952`). Con la F334 conectada hay dos ST-LINK | `sn=` siempre, y la salida a la captura |
| C-M11 | Mayor | **S4 no se aplica a los `Init` del driver en CM7:** UT3, UT4, UT6 (TX/RX), UT7, UT8, UT9 y PA8 (MCO1) configuran pines sin claim | Claim de cada pin antes de cada `Init` |
| D4 | Mayor | **`UART_LL_EnableRxIrq`/`DisableRxIrq` usan `ATOMIC_SET_BIT`/`ATOMIC_CLEAR_BIT`** (LDREX/STREX), la misma causa por la que `Init` se colgaba con LPUART1 (`a39abd7`, `uart_ll.c:358`); con LPUART1 se colgarían igual | RMW no exclusivo dentro de una sección `PRIMASK`. La causa, con una cita de RM0399 o ES0445 si existe; si no, decirlo y apoyarlo en una captura `uart_dev_` en la que el código anterior se cuelga con LPUART1 |
| D5 | Menor | **`TakeErrors` cambia de semántica sin que se pidiera:** ahora lee y borra los flags del hardware (BM y LL). La función está congelada | Declararlo como desviación, con su motivo y su efecto en el modo FIFO (el flag deja de ir asociado al byte); documentarlo en §4.1/§4.3 en el cierre |
| C-m | Menor | Detalles sueltos:<br>• en UT5, un `sync0` fallido deja `ok` verdadero;<br>• relojes de UART4/5/8, USART1/6 y LPUART1 (UT2) y DMA1 (UT8) quedan encendidos;<br>• MCO1 solo se restaura por el camino de DONE;<br>• el canal 15 de Digital Out no se reinicia entre pasos;<br>• se ignoran `LOCK_RELEASE` y `Flush`, y los bucles de TC no tienen límite;<br>• el host no comprueba el árbol limpio;<br>• `--allow-dirty` se salta R3-a;<br>• UT0_CTRL suelta la condición antes del siguiente handshake;<br>• `UST_AREA_ADDR` sin cita del enlazador | Corregir |
| T-N1 | Mayor | **El árbol limpio de `uart_checks.py` no ve `MM`, `UU`, `AA`, `DD`, `RM`…** (`:163`), y al empezar no hay instantánea | Todo prefijo salvo `!!` es sucio |
| T-n | Menor | Detalles sueltos:<br>• exenciones generales de `uart_checks_*`/`uart_mock_*`, en vez de solo `out_path`;<br>• el número de objetos no se exige y es el recuento de *edges* de Ninja;<br>• el control de `-Wconversion` acepta `-Wsign-conversion`;<br>• el pathspec por glob;<br>• los ELF del control positivo sin comprobar que son de la ejecución | Corregir |
| R5 | Mayor | **El informe:**<br>• presenta lo de la placa como verificado;<br>• dice "sin desviaciones";<br>• falta §2 (U1–U6 con `archivo:línea`);<br>• citas erróneas (`uart_hal.c:198-216`, `:205-235`);<br>• la marca final no es la pedida;<br>• falta lo que prepara el responsable;<br>• el comando de `run_warm_resets.py` crea `docs/` en la raíz;<br>• no hay receta del control de UT9 | Corregir; las capturas `uart_dev_*`, como evidencia de desarrollo, NO VERIFICADO |

### La regla de los oráculos

En las tres entregas, varias comprobaciones de banco no podían fallar.

**Desde esta ronda, cada paso de UT0–UT9 lleva en la misma ejecución de placa un control que hace fallar su oráculo, pasando por la misma función de evaluación del host.** Por ejemplo, en UT3 la decodificación con la paridad esperada invertida, o con un byte esperado cambiado, tiene que dar FAIL.

La captura termina con una tabla: paso, comprobación, resultado real y resultado del control. Un paso cuyo control no falla es un FAIL del paso.

### Fuera de alcance

**`00_docs_stm32h7/HOWTO_INTEGRATE_SHARED_DRIVERS.md`** (`77aabca`, 927 líneas, con la línea de coautoría de Claude) no es de esta fase. Está pendiente de la decisión del responsable. No se toca.

### Estado

**F1 no pasa.** El driver está bien, salvo D4 y D5. El selftest y el host han avanzado mucho, pero UT3 y UT10 no pueden fallar, NE y RTS/CTS no se ejercitan, y el mock no recorre la sesión real.

## F1, corrección 2 (`43b3a79`, informe en `c42381e`)

**La rama se reescribió el 2026-10-17** (`git rebase --onto 77aabca^ 77aabca`) para sacar `00_docs_stm32h7/HOWTO_INTEGRATE_SHARED_DRIVERS.md`. Es un documento del responsable, que ahora está en la rama `docs/howto-integrate-shared-drivers` (`777e908`). La única diferencia del árbol es ese archivo.

Las capturas llevan en el nombre los hashes anteriores:

| Hash anterior | Hash actual | Commit |
| --- | --- | --- |
| `a39abd7` | `f8ef5c2` | fix(uart): fix LPUART1 hang, TakeErrors harvesting… |
| `48ffc6c` | `641ac26` | docs(uart): update IMPLEMENTATION_REPORT.md… |
| `d8777e4` | `c8d8628` | docs(uart): audit the F1 correction… |
| `ef7b581` | `9827978` | Phase F1 audit fixes |
| `5d55a28` | `5344e64` | tools: match conversion and sign-conversion… |
| `474f514` | `43b3a79` | tools: permit docs capture files… |
| `98e5b1d` | `c42381e` | docs(uart): add Phase F1 implementation report… |

### Ejecución del auditor en la placa

Captura `docs/uart_audit_hw_c42381e.txt`, de `prj_01`, en `52fef6e`. **La sesión no llega a UT0:**
- el firmware imprime `ABORT step=- reason=MCO1 claim refused`: el claim de PA8 se hace fuera de un paso y la regla lo rechaza;
- CPU2 no responde en 12 s;
- `CHECK step_timeout_UT0: FAIL`.

**La entrega no se probó en la placa.** El informe dice que siguió una "prohibición expresa", cuando la fila de §2.2 ya daba permiso (`c8d8628`) y el mensaje de la ronda lo pedía.

### Cerrado

| Qué | Dónde |
| --- | --- |
| C-B1: UT10 fuera de `prj_01` | Quedan comentarios obsoletos |
| C-M3: `session` | — |
| C-M4: una sola instantánea de 11 registros, partiendo de reloj apagado y pines en entrada | — |
| C-M6: las instantáneas de UT7 y UT9 cuentan | — |
| C-M10: reset con `sn=` y su salida en la captura | — |
| D4: RMW dentro de `PRIMASK`, sin `ATOMIC_*` | `uart_ll.c:556-564, 583-588`; falta la cita o la captura |
| T-N1: todo prefijo salvo `!!` es sucio | — |
| La fila de `PRESC ≠ 0` y la fila mutada de UT2 | — |
| `ad2.py` | Sin ramas permisivas; `dout_custom` vuelve a su comportamiento salvo con `single_shot` |

### Abierto o nuevo

| ID | Gravedad | Hallazgo | Qué se pide |
| --- | --- | --- | --- |
| E-B1 | Bloqueante | **El claim de MCO1 fuera de un paso aborta la sesión** (CM7 `uart_selftest.c:153-160`, llamado en `:2015` con `UST_STEP_NONE`; `uart_selftest_common.c:70-73`) | Un paso de prólogo declarado para MCO1; la línea de MCO1, solo si sale bien |
| E-B2 | Bloqueante | **UT3 se desincroniza:** el firmware hace un `sync0()` por fila (15 filas, CM7:869, 947, 1019) y el host uno solo (`uart_hw_check.py:977`) | Un `sync` por fila en el host, con el LA armado por fila |
| E-B3 | Bloqueante | **El LA de UT3 no tiene disparo:** `la_arm` sin trigger empieza ya y 16 384 muestras a 20 MS/s duran 819 µs, así que terminan antes de que el firmware transmita. Además, el plan pide 100 MS/s | Disparo en el flanco de bajada de DIO15, con pre-trigger; buffer según el `n` que devuelve `la_arm`; 100 MS/s |
| E-B4 | Bloqueante | **UT5 no inyecta nada:** `dout_custom` solo configura, y nunca se llama a `set_dout` (`:1027`; `ad2.py:819`) | `ad2.set_dout(1 << 15)` por fase, y después parar y reiniciar Digital Out |
| E-M1 | Mayor | **UT3 no mide el ancho de bit:** cuenta flancos de MCO1 en una ventana de largo supuesto, así que mide la frecuencia de MCO1. Un bit un 2,53 % lento da PASS. La teoría son literales (`f_mco = 4e6`, `baud = 115200`). Solo decodifica 8N1 | El ancho de bit entre flancos reales de la trama, en periodos de MCO1; la teoría, de `PRESC`/`BRR` y de la línea de MCO1 que imprime el firmware; cada fila decodificada con su formato (paridad, bits, paradas) |
| E-M2 | Mayor | **Siete de los once controles no pueden salir distintos:** UT1, UT4, UT4_CM4, UT5, UT6, UT7 y UT8 evalúan literales (`:926-1063`), y en UT0/UT0_CTRL el control fuerza a `False` una entrada de un AND | El host lee los valores que mide el firmware (máscaras de error, recuentos, `IDR`, códigos devueltos), los evalúa y muta **esos** datos en el control |
| E-M3 | Mayor | **UT6 no necesita el control de flujo:** nunca hay más de 16 bytes sin leer. La fase sin control de flujo es otra secuencia | Más de 16 + 1 bytes sin leer con RTS/CTS, y la misma secuencia sin control de flujo da ORE; recuento en el LA |
| E-M4 | Mayor | **NE sigue igual que CLEAN** (`:1018` frente a `:1021`) | Un pulso más corto que un bit; el resultado, impreso tal cual |
| E-M5 | Mayor | **El mock no reproduce el protocolo real:** número de syncs, el prefijo doble y la ruta de ABORT. Por eso no vio E-B1–E-B4 | Generarlo a partir de `k_ust_steps`, con el número de syncs por paso |
| E-m | Menor | Detalles sueltos:<br>• las filas de rechazo de UT2 no las juzga el host;<br>• el número de filas se compara con `>= 10`;<br>• `--allow-dirty` se salta el árbol limpio y R3-a;<br>• el árbol limpio solo se mira al empezar;<br>• Digital Out no se reinicia entre pasos;<br>• se ignora la espera de DMA de UT8;<br>• el control positivo de UT1 no hace claim;<br>• ORE con patrones de 12 bits para tramas de 11;<br>• prefijo `[uart] [uart]`;<br>• comentarios de UT10 | Corregir |
| D5 | Menor | **El cambio de semántica de `TakeErrors` sigue sin declararse** | Una sección de desviaciones |
| R6 | Mayor | **El informe:**<br>• dice que hubo prohibición de placa;<br>• no menciona las capturas `uart_dev_*`;<br>• falta la sección de desviaciones;<br>• falta la receta del control de UT9;<br>• cita hashes anteriores al rebase;<br>• afirma cosas falsas: UT0 "PD5 en entrada pull-up", UT1 "cada rechazo", UT6 "secuencia idéntica" | Corregir |

### Condición para la siguiente entrega

**No se audita una entrega sin la ejecución del implementador en la placa en el commit entregado:**
- la captura `prj_01/docs/uart_dev_hw_<hash>.txt`, con `FW VERDICT=PASS`, `HOST VERDICT=PASS` y la tabla de controles;
- cada control sale de datos medidos en esa misma ejecución;
- más `uart_dev_warm_resets_<hash>.txt` de la imagen `selftest-uart`.

Si algo del plan no se puede conseguir en el hardware (por ejemplo, NE), la captura lo dice y la entrega lo argumenta. No se marca PASS.

### Estado

**F1 no pasa.** El driver está bien (U1–U6, D1–D4). El selftest y el host no han funcionado en la placa en ninguna de las cuatro entregas.

## F1, corrección 3 (`4db5903`, informe en `12a3020`)

**Condición de entrada: cumplida.** `uart_dev_hw_4db5903.txt` está en el commit entregado, con `FW VERDICT=PASS`, `HOST VERDICT=PASS` y la tabla de controles. Hay 20/20 warm resets de `selftest-uart`, y NE se detecta (`err=0x2 flagged=1`).

### Ejecuciones del auditor en la placa (`297e04d`)

| Prueba | Captura | Resultado |
| --- | --- | --- |
| Sesión completa, ejecución 1 | `prj_01/docs/uart_audit_hw_12a3020_run1.txt` | `FW VERDICT=PASS`, `HOST VERDICT=PASS`; `Rev`/id `12a3020`/`9764bb36233f0808` en CM7, CM4 y host; árbol limpio |
| Sesión completa, ejecución 2 | `…_run2.txt` | Igual |
| 20 warm resets de `selftest-uart` | `uart_audit_warm_resets_12a3020.txt` | 20/20 |
| UT10, selftest real de `prj_04` con JU1 de 1 kΩ | `prj_04/docs/uart_audit_ut10_12a3020.txt` | `[m4_uart_test] jumper=1 bm_init=1,1,1 bm_loop=1,1,1 ll=1,1 ore=1` |
| Control de UT9 | `uart_audit_ut9_control_12a3020-dirty.txt` | `returned 0 (expected 2)` con el lock tomado: **U1 reproducido**. Con HEAD, `HAL_BUSY` |

El control de UT9 lo construí yo: un worktree en HEAD con el `uart_hal.c` de `b41c4a7`, `selftest-uart` con `-DCUBE_DIR`, y `--expect-control-u1 --allow-dirty`.

**Driver:**
- `uart_hal.c` valida la instancia en `Init`/`InitEx` (UT1);
- `ad2.py` añade `config_idx` y `trigger_fall_mask`, que no cambian lo existente.

**Lo demás:** `run_warm_resets.py` exige ahora el banner antes de la frase de vida, y los cinco proyectos lo imprimen.

### Ronda corta

| ID | Gravedad | Hallazgo | Qué se pide |
| --- | --- | --- | --- |
| F-M1 | Mayor | **UT3 no deja rastro en la captura.** Una sola línea `CHECK UT3 …: PASS` para 15 filas: no se ven ni la frecuencia ni las muestras del LA, ni el ancho de bit medido en periodos de MCO1, ni el teórico de `PRESC`/`BRR`, ni el error en %, ni los bytes decodificados. Con eso no se puede auditar el oráculo | Una línea por fila: formato, baud, frecuencia y muestras del LA, tramas decodificadas y bytes, ancho de bit medido y teórico, error % y PASS/FAIL. Y una línea de control por fila |
| F-M2 | Mayor | **El modo control del host está al revés:** con `--expect-control-u1`, el `returned 0` esperado sale como "PASS (unexp)" y da `HOST VERDICT=FAIL` | En modo control, UT9 pasa si y solo si el firmware imprime `returned 0` con el lock tomado; el resto de pasos se informa pero no cuenta |
| F-M3 | Mayor | **El informe no trae las recetas que el auditor necesita:**<br>• §6.2 paso 4 corre `--expect-control-u1` sobre la imagen normal, sin construir el control;<br>• falta UT10 (`prj_04`) | La receta del control (la de esta sección) y la de UT10 |
| F-m1 | Menor | **El vaciado de `run_warm_resets.py` no tiene límite:** `while ser.read(4096)` no termina con una imagen que imprima sin pausas de 100 ms | Limitarlo por tiempo (por ejemplo, 1 s) |
| F-m2 | Menor | **Enlaces `file:///c:/…` en el informe** | Rutas relativas |

**La revisión de solo lectura de los controles (UT0–UT9) sigue en curso.** Lo que encuentre irá en una sección aparte.
