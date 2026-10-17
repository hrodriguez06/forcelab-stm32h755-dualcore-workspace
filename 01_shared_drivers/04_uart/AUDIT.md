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
