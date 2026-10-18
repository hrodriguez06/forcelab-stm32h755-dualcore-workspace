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

### Ampliación de la corrección 3 (enviada por mensaje el 2026-10-17, registrada aquí)

La revisión de solo lectura de `4db5903` terminó después de enviar la ronda corta. Estos hallazgos se mandaron a la misma conversación:

| ID | Gravedad | Hallazgo |
| --- | --- | --- |
| N1 | Bloqueante | **UT3 decodifica una sola trama en 14 de las 15 filas, y `eval_ut3` no exige el número de tramas** |
| N2 | Mayor | **Solo se comprueba el primer bit de parada** |
| N3 | Mayor | **Sin MCO1, se usa en silencio el valor teórico** |
| N4 | Mayor | **La línea de MCO1 del firmware es un literal, y se descarta el resultado de `ConfigAlternate`** |
| N5 | Mayor | **UT1, UT4, UT4_CM4, UT7 y UT8 imprimen valores derivados de `ok`; el host no lee los `IDR` de UT0** |
| N6 | Mayor | **La captura no registra lo que mide el AD2** en UT0, en UT3 por fila, ni en UT5 por fase |
| N7 | Mayor | **El modo control de UT9 está al revés** |
| N8 | Mayor | **`UART_HAL_IsValidInstance` cambia el driver congelado sin declararlo**, y las citas de `uart_hal.c` se han movido |
| N9 | Mayor | **Afirmaciones falsas en el informe** |
| N12 | Mayor | **CM7 no lee `m4_timeout`, y las ventanas dependen de la latencia del host** |
| N10, N11, N13–N15 | Menor | Pulso de NE; LA en UT4–UT8; exenciones del árbol limpio; vaciado y banner de `run_warm_resets.py`; tramas no medibles y mock |

## F1, corrección 4 (`fdbf306`, informe en `9d2edd2`)

**Condición de entrada: cumplida.** `uart_dev_hw_fdbf306.txt` está en el commit entregado, con todo PASS y la tabla de controles.

### Ejecuciones del auditor en la placa (`8d04a51`)

| Prueba | Captura | Resultado |
| --- | --- | --- |
| Sesión completa, ejecuciones 1 y 2 | `prj_01/docs/uart_audit_hw_9d2edd2_run1.txt`, `_run2.txt` | `FW VERDICT=PASS`, `HOST VERDICT=PASS`; `Rev`/id `9d2edd2`/`b4b837ff98857d86` en CM7, CM4 y host; las 15 filas de UT3, cada una con su línea y su control |
| 20 warm resets de `selftest-uart` | `uart_audit_warm_resets_9d2edd2.txt` | 20/20 |
| UT10, `prj_04` | `prj_04/docs/uart_audit_ut10_9d2edd2.txt` | `[m4_uart_test] jumper=1 bm_init=1,1,1 bm_loop=1,1,1 ll=1,1 ore=1` |
| Control de UT9 (worktree con el `uart_hal.c` de `b41c4a7`) | `uart_audit_ut9_control_9d2edd2-dirty.txt` | `returned 0 (expected 2)`, y el host da `HOST VERDICT=PASS` en modo control: U1 reproducido y juzgado bien |

### Revisión del código

**Cerrado:**
- F-M1, F-M2, F-M3, F-m1 y F-m2;
- N2: cada bit de parada; la sonda detecta 8N2 enviado con una parada;
- N5: valores reales, salvo `sent = 72`, que es literal;
- N7;
- N10: NE a 9/16 del bit, detectado;
- N14: el banner por defecto vale para los cinco proyectos;
- N15.

**El driver no cambia en esta ronda.** La sonda de UT3 detecta:
- un bit un 0,6 % o un 1 % lento;
- la paridad invertida;
- la longitud de palabra equivocada;
- bytes 2–6 erróneos a 115 200;
- MCO1 ausente o a 4,1 MHz;
- el LA sin datos.

| ID | Gravedad | Hallazgo | Qué se pide |
| --- | --- | --- | --- |
| G-B1 | Bloqueante | **N1 sigue abierto.** `eval_ut3` solo recorre las tramas que hay (`uart_hw_check.py:375-377`), sin exigir `len(frames) == len(pat)`. La fila de 9600 captura 1,64 ms (10 MS/s y 16 384 muestras) para 6,25 ms de tramas: en la placa sale `frames=1 bytes=[0x55] -> PASS` (`uart_audit_hw_9d2edd2_run1.txt:108`). La sonda da PASS con solo el primer byte enviado, con 3 de 6 y con bytes 2–6 erróneos a 9600 | Exigir `len(frames) == len(pat)`, con un control que quite una trama de los datos medidos. Para 9600, que quepan las 6 tramas con MCO1 en la misma captura (una captura por trama, o MCO1 más lento con menos MS/s) |
| G-M1 | Mayor | **El informe:**<br>• describe mal los N (R:34 sobre N1, R:35 "1,5 bits de parada", R:40, R:42, R:44, R:46);<br>• cita líneas de `uart_hal.c` que no son (U1 es 173-186, U6 229-233/249-253, D6 156-159/191-194, D2 199-200/208-209);<br>• titula §3 "Desviaciones Aprobadas", y D6 nadie la aprobó;<br>• presenta las capturas `uart_dev_` como "Verificación en Hardware Real" | Describir cada N como se pidió; citas exactas; "declarada, pendiente de aprobación"; "evidencia de desarrollo" |
| G-M2 | Mayor | **N12 sigue a medias:**<br>• CM4 se rinde a los 3 s sin comando (CM4 `uart_selftest.c:189-193`), cuando UT5/UT6 los marca el host;<br>• `m4_timeout` solo se lee en el camino de DONE y solo se imprime si no es 0;<br>• el host no lo lee | CM4 espera hasta DONE o aborto, sin plazo propio; `m4_timeout=` siempre en la captura, y el host lo exige a 0 |
| G-M3 | Mayor | **Las recetas del informe (§6.2):**<br>• la del control de UT9 modifica el checkout compartido;<br>• la de UT10 abre el monitor sin reset, así que la línea ya ha salido;<br>• ninguna lleva `--sn` | El control, en un worktree (`git worktree add --detach`, `git show b41c4a7:… >`, `-DCUBE_DIR`); UT10, con reset NRST tras abrir el monitor; `--sn` en todas |
| G-m | Menor | Detalles sueltos:<br>• N3: registrar el periodo y el número de flancos de MCO1 por fila, y exigir flancos a lo largo de las tramas (MCO1 que se para a mitad da PASS);<br>• N4: leer la fuente de MCO1 y el reloj de USART2 en vez de `HSE_VALUE` y el literal de 100 MHz;<br>• `sent = 72` literal;<br>• valores por defecto permisivos en UT1 (`snap=1, fail=0` si falta la línea);<br>• en UT5 no se comprueba el byte recibido;<br>• N6: los valores del AD2 de UT0/UT0_CTRL no van a la captura;<br>• exención `.gemini`;<br>• el mock usa un periodo de bit entero (+0,23 %);<br>• el banner no queda registrado en `run_warm_resets`;<br>• N11 (LA en UT4–UT8) ni hecho ni argumentado | Corregir o argumentar |

### Estado

**F1 no pasa, por G-B1.** Todo lo demás de la placa pasa, en las ejecuciones del implementador y en las mías. Lo que queda es G-B1, el informe, G-M2, G-M3 y menores.

## F1, corrección 5 (`9b9fe37`, informe en `adc848d`)

**Condición de entrada: cumplida.** `uart_dev_hw_9b9fe37.txt`, con todo PASS y `m4_timeout=0`.

### Ejecuciones del auditor en la placa (`882b5a9`)

| Prueba | Captura | Resultado |
| --- | --- | --- |
| Sesión completa, ejecuciones 1 y 2 | `prj_01/docs/uart_audit_hw_adc848d_run1.txt`, `_run2.txt` | `FW VERDICT=PASS`, `HOST VERDICT=PASS`; las 15 filas de UT3 decodifican las 6 tramas, también la de 9600 a 2 MS/s |
| 20 warm resets | `uart_audit_warm_resets_adc848d.txt` | 20/20 |
| UT10, `prj_04` | `prj_04/docs/uart_audit_ut10_adc848d.txt` | Línea esperada |
| Control de UT9 | `uart_audit_ut9_control_adc848d-dirty.txt` | U1 reproducido; el host da PASS en modo control |

**En la ejecución 1, CM7 esperó 9,9 s entre `STEP DONE` y `m4_timeout=0`**, y aun así todo salió PASS. En la ejecución 2 no ocurrió.

### Revisión del código

**Cerrado:**
- **G-B1:** `len(frames) == len(expected)` (`uart_hw_check.py:378-379`), y 9600 a 2 MS/s con las 6 tramas. La sonda detecta todos los casos de tramas y de bytes.
- **G-M2:** CM4 sin plazo propio, y `m4_timeout` siempre impreso; con la salvedad de H-M2.
- **Citas de `uart_hal.c`**, y los títulos de §3 y §4 del informe.
- **El cambio de MCO1:** `/10`, solo en `selftest-uart`, restaurado en cada salida, y el host toma `f_mco` de la línea.

**El driver y `ad2.py` no cambian.**

| ID | Gravedad | Hallazgo | Qué se pide |
| --- | --- | --- | --- |
| H-M1 | Mayor | **El control de UT3 solo ejercita el número de tramas** (`decoded[:-1]`, `uart_hw_check.py:1204-1206`). La ronda anterior controlaba el tiempo (×1,025), y ahora el tiempo, los bytes, la paridad y la parada no tienen control en la ejecución | Por fila, por `eval_ut3` y con los datos medidos: quitar una trama; `mco_per_bit × (1 + 2·tol)`; invertir un bit de dato; `parity_ok = False` en las filas con paridad; `stop_ok = False`. Cada una registrada, y todas tienen que dar FAIL |
| H-M2 | Mayor | **El handshake de DONE tiene una carrera y oculta su fallo:**<br>• CM4 sale del bucle con `cmd == DONE` (CM4 `:189`) antes de mirar `cmd_seq`, así que si lee entre las dos escrituras de CM7 no confirma;<br>• CM7 descarta el resultado (`(void)cm4_send_cmd(UST_CMD_DONE…)`, CM7 `:2168`) y espera 10 s. Pasó en la ejecución 1 del auditor;<br>• `m4_timeout` solo se escribe a 0 (CM4 `:181`), así que la comprobación del host (`:1529-1530`) no puede fallar | Quitar la condición `cmd == DONE` del bucle; contar la falta de confirmación como fallo e imprimir `m4_done_ack=`, que el host exige; que `m4_timeout` refleje un plazo real o quitarlo |
| H-m1 | Menor | **`f_ker` cae siempre al literal de 100 MHz:** `HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_USART234578)` devuelve 0 en este HAL. El informe (R:34) dice lo contrario | Leer `RCC->D2CCIP2R` USART234578SEL; si es D2PCLK1, `HAL_RCC_GetPCLK1Freq()`; con otra fuente, abortar |
| H-m2 | Menor | **MCO1 solo se comprueba al final:** pasan MCO1 presente solo después de las tramas y huecos cortos. A 2 MS/s el margen de ciclo de trabajo es estrecho (≥ 40,5 %) | Primer flanco ≤ primer inicio de trama, y separación entre flancos ≤ 1,5 × `mco_per` dentro de las tramas; o MCO1 /16. El criterio de R:29 no es Nyquist |
| H-m3 | Menor | **`total_sent` cuenta iteraciones:** se descartan los resultados de `WriteByte`/`Write` (CM7:1127-1362) | Contar lo que se envía de verdad |
| H-m4 | Menor | **Ninguna trama medible en una fila da PASS de tiempo** (`:390`) | Al menos 4 de 6 medibles por fila |
| H-m5 | Menor | **El informe:**<br>• las citas de D5 (`uart_bm.c:514-530`, `uart_ll.c:538-554`) no son de `TakeErrors` (es `uart_bm.c:575-609` y `uart_ll.c:514-536`);<br>• R:82 dice que D5 está aprobada, y no lo está;<br>• R:13 dice "N1..N15 atendidos", pero la lista se borró;<br>• exageraciones: R:31, R:32, R:33, R:35, R:46, R:53 | Corregir; D5 y D6, "declaradas, pendientes de aprobación" |
| H-m6 | Menor | **Las recetas no se pueden ejecutar:**<br>• el control de UT9 no configura el `build/` del worktree (`cmake --preset selftest-uart -DCUBE_DIR=…`);<br>• `git show … >` en PowerShell 5.1 escribe UTF-16;<br>• sin `--out` fuera del worktree, la captura se borra con el worktree;<br>• en UT10, el monitor está comentado y sin `--sn`, `monitor.py --baud` no existe y `-rst` no es NRST (es `-hardRst`);<br>• `run_warm_resets.py` resetea sin `sn=` (`:46`) | `git -C <wt> checkout b41c4a7 -- …/uart_hal.c`; configurar con `-DCUBE_DIR`; `--out` fuera; UT10 con `monitor.py --sn … --timeout` y `-hardRst`; `run_warm_resets.py` con `sn=` |
| H-m7 | Trivial | Comentario obsoleto de MCO1 a 4 MHz (CM7 `:2149`); `f_mco = 4000000.0` por defecto (`:216`); el mapeo de `mco1_sel` (CM7 `:186-193`) | Corregir |

### Estado

**F1 no pasa, por H-M1 y H-M2.** El resto de la fase está cerrado en la placa.

## F1, corrección 6 (`ab390d3`, informe en `2611907`)

**Condición de entrada: cumplida.** `uart_dev_hw_ab390d3_run1.txt` y `_run2.txt`, con todo PASS y `m4_done_ack=1`.

### Ejecuciones del auditor en la placa (`954c97a`)

| Prueba | Captura | Resultado |
| --- | --- | --- |
| Cinco sesiones completas seguidas | `prj_01/docs/uart_audit_hw_2611907_run1.txt` … `_run5.txt` | **5/5:**<br>• `FW VERDICT=PASS` y `HOST VERDICT=PASS`;<br>• `m4_done_ack=1`, unos 5 ms después de `STEP DONE`;<br>• 15/15 filas de UT3;<br>• los 15 controles de cinco mutaciones: FAIL esperado, o N/A para la paridad en las filas sin paridad;<br>• unos 12 s por sesión, sin la espera de 10 s |
| 20 warm resets | `uart_audit_warm_resets_2611907.txt` | 20/20 |
| Control de UT9 (receta corregida: worktree, `git -C <wt> checkout b41c4a7 -- …`, `-DCUBE_DIR`) | `uart_audit_ut9_control_2611907-dirty.txt` | U1 reproducido; el host da PASS en modo control |
| UT10, `prj_04` | `prj_04/docs/uart_audit_ut10_2611907.txt` | Línea esperada |

### Revisión del código

**Cerrado:**
- **H-M1:** cinco mutaciones por fila sobre los datos medidos y por `eval_ut3`, con una línea por fila. Si un control pasa, falla el paso.
- **H-M2:**
  - CM4 solo sale del bucle por cambio de sesión;
  - CM7 comprueba la confirmación, y el host exige `m4_done_ack=1`;
  - SRAM4 es no cacheable para CM7 (MPU R7), y todos los campos son `volatile`.
- **H-m1:** `D2CCIP2R`, y PCLK1 o aborto.
- **H-m2:** MCO1 cubre todas las tramas, sin huecos de más de 1,5 periodos.
- **H-m3, H-m4, H-m5 y H-m7.**

La sonda de UT3 (`probe_ut3_r7`) rechaza todos los fallos y, en ningún caso, la fila real pasa con un control también en PASS. El driver y `ad2.py` no cambian.

**Quedan (menores y triviales), para el cierre:**

| ID | Hallazgo | Qué se pide |
| --- | --- | --- |
| I-1 | **La receta del control de UT9** pasa un `-DCUBE_DIR` que no existe (`…/00_STmicro/STM32CubeH7`) | `<checkout principal>/02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7` |
| I-2 | **La receta de UT10:**<br>• pasa `--port` y `--sn` a la vez, y `monitor.py` lo rechaza;<br>• no configura `prj_04/build/selftest-uart` | Solo `--sn`; `cmake --preset selftest-uart` antes |
| I-3 | **`run_warm_resets.py --sn`** fija por defecto el SN de esta placa, así que con otra sonda el reset falla. Además, el puerto sigue saliendo de `autodetect()` | Por defecto `None`. Con `--sn` y sin `--port`, el puerto de `monitor.port_for_sn(sn)`. El docstring lo dice |
| I-4 | **Barreras del protocolo:**<br>• CM4 lee `cmd` antes de releer `cmd_seq`, sin DMB;<br>• CM7 lee los resultados tras la confirmación sin DMB | CM4: `seq = cmd_seq; __DMB(); cmd = …`; CM7: `__DMB()` antes del `return` de `cm4_send_cmd` |
| I-5 | **LOCK_RELEASE:** CM4 confirma aunque `HSEM_BM_Release` falle, y CM7 no comprueba `lock_held == 0` | Comprobarlo |
| I-6 | **`m4_timeout`** ya no se usa, pero sigue en `uart_selftest.h:122` | Quitarlo, o decir que está sin uso |
| I-7 | **El informe:**<br>• R:35 "FAIL (exp)" en todas (la paridad es N/A en 9 filas);<br>• `uart_dev_hw_ab390d3.txt` es idéntico a `_run1` | Precisarlo; dejar una sola copia |

### Decisión del responsable (2026-10-17)

**D5 y D6, aprobadas:**
- D5: `TakeErrors` lee y borra los flags del hardware;
- D6: `UART_HAL_Init`/`InitEx` rechazan una instancia que no es USART1–8 ni LPUART1.

Se documentan en el README en el cierre.

### Estado

**F1 pasa.** Queda autorizado el cierre del implementador, con I-1–I-7. I-4–I-6 tocan firmware, así que hace falta su ejecución de placa y otra mía.

**El README (en inglés), según la sección "Cierre" del plan:**
- **Estado.**
- **§1.1.**
- **§3.1:** U1, la consola devuelve el estado de sus pines.
- **§4:**
  - LL valida antes de tocar el hardware (U2, U3) y deja `CR1`–`CR3` limpios (U4);
  - D5, `TakeErrors`;
  - D6;
  - `InitEx` con `HAL_BUSY` (U6);
  - con 9 bits de datos, tampoco se puede escribir el bit 8 (U7).
- **§6:** el divisor aplicado en LL, U5.
- **§9:** UT0–UT10, con las capturas finales (las de `ab390d3` y las del auditor en `2611907`).
- **§10:**
  - U7, `Init` a medio camino;
  - D4: `ATOMIC_*` (LDREX/STREX) se cuelga con LPUART1, por eso LL usa RMW en `PRIMASK`. Medido en la placa, sin cita de ES0445;
  - NE, detectado con el pulso a 9/16;
  - LPUART1 solo en `PRESC`/`BRR`;
  - lo no ejercitado.
- **§12.**
- **`00_gpio/README.md` §8:** JU1, JU2 y DIO15, con SU1–SU4.

**Capturas (regla 7 de `00_gpio/README.md` §14):**
- solo quedan las que respaldan una afirmación actual;
- las superadas (las de desarrollo y de auditoría anteriores a `ab390d3`/`2611907`, y los mocks y `uart_checks` anteriores) se borran;
- se listan en una tabla de §9 con el commit desde el que se pueden leer.

## Cierre del implementador (`9229773`, informe en `f584633`)

**Condición de entrada: cumplida.** `uart_dev_hw_9229773_run1.txt` y `_run2.txt`, con todo PASS y `m4_done_ack=1`, más `uart_dev_warm_resets_9229773.txt` (20/20) y `uart_checks_9229773.txt`. Árbol limpio y sin escrituras en `.git/config` (sin cambios desde el 2026-10-17 01:26).

### Ejecuciones del auditor en la placa (en `f584633`)

`selftest-uart` compila con 0 avisos (salida de `cmake --build`).

| Prueba | Captura | Resultado |
| --- | --- | --- |
| Tres sesiones completas seguidas | `prj_01/docs/uart_audit_hw_f584633_run1.txt` … `_run3.txt` | **3/3:**<br>• `FW VERDICT=PASS` y `HOST VERDICT=PASS`;<br>• `Rev`/id `f584633`/`5f1db20ea787773e` en CM7, CM4 y host;<br>• `m4_done_ack=1`;<br>• 15/15 filas de UT3 y 15/15 líneas de control con las cinco mutaciones en FAIL esperado (o N/A en paridad);<br>• UT7 y UT9 con la comprobación nueva de `lock_held` |
| 20 warm resets, con `--sn` y sin `--port` (I-3: el puerto sale de `port_for_sn`) | `uart_audit_warm_resets_f584633.txt` | 20/20 |
| Control de UT9 (worktree en `f584633` con el `uart_hal.c` de `b41c4a7`) | `uart_audit_ut9_control_f584633-dirty.txt` | `returned 0 (expected 2)`: U1 reproducido; el host da PASS en modo control |
| Control de UT9 sobre la imagen corregida (accidental, ver abajo) | `uart_audit_ut9_control_f584633_fixed_image.txt` | `returned 2`, y el modo control da `HOST VERDICT=FAIL`, como debe |
| UT10, `prj_04` (`cmake --preset selftest-uart --fresh`, `monitor.py --sn`, `-hardRst`) | `prj_04/docs/uart_audit_ut10_f584633.txt` | `[m4_uart_test] jumper=1 bm_init=1,1,1 bm_loop=1,1,1 ll=1,1 ore=1` |

**El control accidental.** El primer intento de control puso el worktree en el scratchpad. La ruta superó el límite de Windows en el `try_compile` de CMake, la configuración falló, y `flash.py` no grabó nada. `uart_hw_check.py --expect-control-u1` corrió entonces contra la imagen corregida, y la juzgó FAIL. Con un worktree de ruta corta (`00_STmicro/wt_u9`) el control salió bien. Ya no queda ningún worktree.

`prj_01` `debug` queda otra vez en la placa.

### Revisión del código

**Cerrado:**
- **I-1:** la receta del control de UT9 usa `CUBE_DIR` del checkout principal (informe:205-206);
- **I-2:** la receta de UT10 configura `prj_04` y usa solo `--sn`;
- **I-3:** `--sn` sin valor por defecto; con `--sn` y sin `--port`, el puerto sale de `monitor.port_for_sn` (probado en la placa);
- **I-4:** CM4 lee `cmd_seq`, `__DMB()` y después `cmd`; CM7 hace `__DMB()` antes de devolver la confirmación. CM4 ya tenía `__DMB()` antes y después de escribir `ack_seq`;
- **I-5:** CM4 deja `lock_held = 1` si `HSEM_BM_Release` falla, y CM7 exige `lock_held == 0` tras LOCK_RELEASE en UT7 y UT9;
- **I-6:** `m4_timeout` fuera del área de UART. Las otras áreas (`gpio`, `i2c`, `spi`, `tim`) tienen su propio campo y no cambian;
- **I-7:** la paridad N/A en 9 filas está en el informe, y queda una sola copia de la captura.

**El driver y `ad2.py` no cambian.** Los cambios son el firmware del selftest (I-4–I-6), `run_warm_resets.py`, los README y la limpieza.

**Limpieza de capturas.** Las 41 capturas borradas están en la tabla de README §9.3, y cada una se puede leer en el commit que indica (comprobado con `git cat-file -e` una a una).

### Pendiente de documentación (para el cierre del responsable)

| ID | Dónde | Hallazgo | Corrección |
| --- | --- | --- | --- |
| R-1 | README:12-13, :673 | D5/D6 "approved by technical leadership" | Aprobadas por el responsable del catálogo |
| R-2 | README:671 | `474f514` es un hash anterior al rebase y no está en la rama | `43b3a79` |
| R-3 | README:500 | UT3 "100 MS/s or 2 MS/s" y "1 and 2 stop bits" | 20 MS/s a 115 200, 2 MS/s a 9600, 100 MS/s a 921 600; las 15 filas reales (8N2, solo 8N1/8E1 en HAL) |
| R-4 | README:506 | El control de UT8 dice "rejections evaluate to FAIL if accepted" | El control muta `bit20 = 0` en `eval_ut8`; los rechazos solo los juzga el firmware |
| R-5 | README:507-508 | UT9 "on base commit `b41c4a7`"; UT10 presenta `jumper=0` como control | UT9: HEAD con el `uart_hal.c` de `b41c4a7`; UT10 no tiene control en la ejecución |
| R-6 | README:618 | D4 atribuye el cuelgue al dominio D3 | Solo lo observado; la causa no está medida |
| R-7 | README §9.2, §9.4 | Las capturas del auditor citadas son las de `2611907`; la receta de `prj_01` no configura; falta la receta del control de UT9 | Citar las de `f584633` y retirar las de `2611907` a §9.3; añadir `cmake --preset selftest-uart`; la receta del control, con worktree de ruta corta |
| R-8 | `00_gpio/README.md` §8 | Remite a `04_uart/README.md` §8 (es §9); SU1 cita `ad2_digital_out_start`, que no existe (`ad2_set_dout`); "no series resistors" y "the four of PF14/PF15" no cuentan la resistencia de JU1 | Corregir |

### Estado

**El cierre del implementador pasa.** Quedan R-1–R-8, todos de documentación, y los pasos de cierre del responsable.
