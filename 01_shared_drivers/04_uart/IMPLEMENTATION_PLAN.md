# Plan de implementación de `04_uart`: auditoría, correcciones y validación en el banco

Oct 17, 2026 · @Hector · revisión 1

`04_uart` está "Hecho", pero sin auditoría independiente. Su única validación es del implementador (2026-09-22): un selftest en CM4 de `prj_04` con un cable PD5–PD6 y nueve bytes por velocidad (README §9). Este plan:
- corrige lo que encontró mi auditoría del código (U1–U7);
- valida en el banco del AD2 lo que el README da como no validado y lo que corrigen U1–U5;
- cierra el driver con una auditoría independiente y una ejecución propia del auditor en la placa.

**Las pruebas van en un solo proyecto:** `prj_01_template_dualcore`, con un preset `selftest-uart`, como `03_tim`, `05_i2c` y `06_spi`. `prj_02`–`prj_04` solo se compilan (T1), y su consola sigue siendo la de siempre.

Una fase de implementación (F1) y un cierre. Las reglas para el implementador están en [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md).

Base y control: `main` en `b41c4a7` (cierre de `00_gpio`).

**El desarrollador es Antigravity.** No tiene acceso a la placa ni al AD2:
- el implementador entrega el firmware, las herramientas y las pruebas sin placa (T1, T2 y la prueba del host con el mock);
- todo lo que necesita el banco (UT1–UT10 y las dos ejecuciones completas) lo ejecuta el auditor con las herramientas del implementador.

**Reglas de verificación (heredadas de `00_gpio` y `08_eth`):**
- **Comprobaciones reales:** cada una puede fallar de verdad y cuenta en el veredicto.
- **Controles negativos:** cada uno pasa por **la misma función** que la comprobación real, con una entrada mutada:
  - en una copia temporal fuera del repo;
  - o en un control compilado **en la misma ejecución**.

  Una constante o un atajo que no llama a esa función no es un control.
- **Oráculo independiente:** cada prueba de banco tiene un oráculo que no es el driver. El AD2 mide lo que pasa por el cable, y el host calcula `BRR`/`PRESC` con su propia fórmula.
- **Identidad de la imagen:** cada captura de placa prueba qué imagen corre, con `Rev` y build id de los dos núcleos, como exige `gpio_hw_check.py`.
- **Árbol limpio** al empezar y al terminar, incluidos los ignorados, salvo `build/`. Ninguna escritura en `.git/config`.
- **Capturas:** UTF-8, de una sola ejecución, con `argv` y `HEAD`, sin edición manual.
- **Citas:** cada una se comprueba contra `HEAD`.
- **Idioma:** el texto que va a un README se escribe en inglés.
- **Recuento de avisos:** sale de la salida de `cmake --build`; `build_all.ps1` no imprime la salida del compilador.

---

## Hallazgos de la auditoría del código (`b41c4a7`)

| ID | Gravedad | Hallazgo | Qué se pide |
| --- | --- | --- | --- |
| **U1** | Mayor | **La consola no devuelve el error de sus pines.** `UART_Console_Init()` llama a `UART_HAL_Init()` (`uart_hal.c:302-310`), y esa función devuelve solo lo que devuelve `HAL_UART_Init()` (`:142-162`): nunca mira `s_msp_gpio_status`. Además, `s_msp_gpio_status` solo se reinicia en `UART_HAL_InitEx()` (`:208`): un fallo de la consola queda pegado. README §3.1 y §4.5, y la fila de `04_uart` del roadmap, dicen que sí lo devuelve (F1b de `00_gpio`) | `UART_HAL_Init()` pone `s_msp_gpio_status = HAL_OK` antes de `HAL_UART_Init()` y, si `HAL_UART_Init()` da `HAL_OK`, devuelve `s_msp_gpio_status`. La firma no cambia: es una corrección de un comportamiento erróneo (regla 2 de `01_shared_drivers/README.md`). Los proyectos que hacen `(void)UART_Console_Init(...)` no cambian (fuera de alcance) |
| **U2** | Mayor | **LL no valida sus constantes antes de escribir.** `UART_LL_Init()` (`uart_ll.c:182-294`) solo comprueba los bits de parada del LPUART. `data_bits`, `stop_bits`, `parity`, `hw_flow` y `oversampling` van tal cual a `LL_USART_Set*()`, que hacen `MODIFY_REG(reg, máscara, valor)` sin enmascarar el valor. Con una constante de BM (`UART_BM_DATA_BITS_7` = 1), `SetDataWidth` pone `CR1` bit 0, que es `UE` | LL valida cada campo contra su conjunto de constantes `LL_USART_*` antes de tocar nada; un valor fuera, `false` |
| **U3** | Mayor | **LL toca el hardware antes de validar.** Configura los pines (`:202-221`) y enciende el reloj (`:223-227`) antes de comprobar el baud rate (`:229-235`). BM lo valida todo antes (fase 1, `uart_bm.c:232-372`) | El mismo orden que BM: toda la validación y el cálculo, y solo después pines, reloj y registros |
| **U4** | Mayor | **Reinicializar en LL deja bits viejos.** LL solo hace `MODIFY_REG` de campos sueltos y nunca escribe `CR1`/`CR2`/`CR3` enteros. Reinicializar una instancia ya usada conserva `FIFOEN`, `RXNEIE`/`RXFNEIE`, `EIE`, `DMAT`/`DMAR`, `SWAP`, etc. Con `fifo_enable = false` tras `true`, el handle dice `fifo_enabled = false` y el FIFO sigue encendido. BM escribe los tres registros enteros (`uart_bm.c:409-414`) | LL deja `CR1`, `CR2` y `CR3` en un estado conocido (como BM) antes de programar los campos |
| **U5** | Menor | **El ±2 % de LL con OVER8 mira el divisor redondeado.** `UART_LL_SelectPrescaler` (`uart_ll.c:146-154`) no usa el que aplica el hardware (`BRR[3] = 0` pierde el bit 0). README §6 dice que BM y LL usan el aplicado; solo BM lo hace (`uart_bm.c:127-133`). Con divisores pequeños e impares, LL acepta configuraciones fuera de tolerancia | El mismo cálculo que BM |
| **U6** | Menor | **`UART_HAL_InitEx` pierde la causa del fallo de un pin.** Configura los pines con la API booleana (`GPIO_BM_ConfigAlternate`, `uart_hal.c:176-194`), así que un lock ocupado sale como `HAL_ERROR`, no como `HAL_BUSY`. Además, enciende el reloj antes de validar los pines | La API con estado de `00_gpio` (`GPIO_BM_ConfigPin`), con `GPIO_BM_ERR_BUSY` → `HAL_BUSY` y lo demás → `HAL_ERROR`. Los pines se validan antes de encender el reloj |
| **U7** | Doc | **El README no dice dos cosas:**<br>• con 9 bits de datos, `WriteByte`/`Write` tampoco pueden enviar el bit 8 (solo dice lo de la lectura, §4.2 y §10);<br>• un `Init` que falla después de tocar el hardware (pines, `TEACK`) deja la instancia a medio configurar | Añadirlo a §4.1, §4.2 y §10, en el cierre |

**Lo que revisé y está bien:**
- la guarda de `UART_HAL_CheckDmaErrata`: `IS_DMA_STREAM_INSTANCE` existe en `stm32h755xx.h:26967`, así que un handle BDMA se rechaza;
- el cálculo de BM;
- el *ring buffer* (un productor y un consumidor, con `__DMB()`);
- `TakeErrors`, con `PRIMASK` guardado.

---

## Encargo

| ID | Qué |
| --- | --- |
| **H1** | U1–U6 en `uart_bm.c`, `uart_ll.c` y `uart_hal.c`. Las cabeceras solo pueden añadir (catálogo, regla 2) |
| **H2** | `prj_01` `selftest-uart` (nuevo preset, opción `UART_DRIVER_SELFTEST`): pasos UT0–UT10 en CM7 y CM4, en el marco del banco de `prj_01` (`k_steps[]`, `HOST_REQ`/`MCU_ACK`, `bench_claim()`) |
| **H3** | `prj_01/tools/uart_hw_check.py` (nuevo): el host de UT0–UT10 con el AD2, con su mock para probarlo sin placa |
| **H4** | `tools/uart_checks.py` (nuevo): T1 y T2 sin placa |
| **H5** | `IMPLEMENTATION_REPORT.md` |

**Fuera de este plan:**
- modos síncrono, smartcard, IrDA, LIN, RS-485 y multiprocesador;
- TX por interrupción en LL;
- `ReadByte` de 9 bits;
- los `(void)UART_Console_Init(...)` de los proyectos;
- `prj_eth_lwip_node` (copia privada);
- el selftest `selftest-uart` de `prj_04`, que solo se compila;
- LPUART1 en el cable, salvo que la Decisión 3 diga otra cosa.

---

## Banco

El banco del AD2 de `prj_01` (`00_gpio/README.md` §8), sin tocar nada de lo que ya tiene, más tres conexiones en CN9 (UM2408: `USART_B` = USART2):

| Conexión | Desde | Hasta | Por qué |
| --- | --- | --- | --- |
| **JU1** | PD5, USART2_TX (CN9-6) | **1 kΩ** → PD6, USART2_RX (CN9-4) | Bucle TX → RX. La resistencia limita a 3,3 mA el cruce cuando el AD2 inyecta en PD6 |
| **JU2** | PD4, USART2_RTS (CN9-8) | PD3, USART2_CTS (CN9-10) | Bucle de control de flujo. Un solo conductor: RTS es salida y CTS, entrada |
| **DIO15** | AD2 DIO15 | PD6 (CN9-4) | Oráculo: el analizador lógico ve la trama que sale de PD5. En UT5 inyecta tramas en PD6 |

- **DIO14 (MCO1, PA8)** ya está cableado: es la referencia de reloj de las medidas de baud. El reloj de la placa va un +1,3 % alto (`03_tim/README.md` §10.5), así que los tiempos se miden contra MCO1 en la misma captura, nunca contra el reloj del AD2.
- **Si el cable PD5–PD6 directo de `prj_04`/`02_dma` sigue puesto, se cambia por JU1.** El selftest de `prj_04` y T3 de `02_dma` funcionan igual con 1 kΩ.

**Seguridad eléctrica:** se añaden reglas a S1–S11 de `00_gpio` §8, sin relajar ninguna.
- **SU1:** DIO15 es salida del AD2 solo en los pasos de UT5. Lo exige `ad2.py` con una máscara nueva, que solo admite DIO15 y solo cuando el paso lo declara; en los demás pasos es entrada.
- **SU2:** mientras el AD2 conduce PD6, PD5 está en reposo (USART2 sin transmitir, `TE` = 0 o el pin en entrada). Lo hace el firmware con `bench_claim()` y lo comprueba el host antes de poner OE.
- **SU3:** al abrir, al cerrar, entre pasos y ante cualquier excepción, DIO15 vuelve a entrada (S2).
- **SU4:** PD3–PD6 solo los configura el firmware de `selftest-uart`. Ninguna otra imagen de `prj_01` los toca.

---

## Pruebas

### Sin placa (implementador, H4)

| ID | Qué | Criterio | Control negativo (misma función) |
| --- | --- | --- | --- |
| **T1** | La matriz: las 8 configuraciones y los presets `selftest-uart`, `selftest-gpio` de `prj_01` y `selftest-uart` de `prj_04`, compilados por la herramienta con `cmake --build --clean-first`, contando los avisos de esa salida | 0 avisos, 0 errores, imágenes recientes | Un preset en un directorio temporal fuera del repo con `-DCMAKE_C_FLAGS=-Wconversion` da avisos ≥ 1 por la misma función |
| **T2** | **Cabeceras y símbolos:**<br>• `git diff b41c4a7 -- 01_shared_drivers/04_uart/*.h` solo añade;<br>• los ELF de producción de `prj_02`–`prj_04` y `prj_01` no contienen ningún símbolo del selftest (`UT*`, `uart_selftest*`) | Todo PASS | Una copia temporal de una cabecera con una firma cambiada da FAIL; un `.map` temporal con un símbolo del selftest da FAIL |

### En el banco (`prj_01` `selftest-uart` + `uart_hw_check.py`, las ejecuta el auditor)

Las pruebas con BM y LL corren en los dos núcleos (un dueño de USART2 por paso); las de HAL y DMA, en CM7.

| ID | Qué prueba | Oráculo | Control negativo |
| --- | --- | --- | --- |
| **UT0** | El banco: JU1, JU2 y DIO15 conectados y en reposo, y PD6 alto | El AD2 lee PD6 | Un paso con PD5 en bajo por GPIO: DIO15 lee 0 |
| **UT1** | **Rechazos de los tres tiers:**<br>• instancia desconocida, constantes fuera de rango (U2, incluida una de BM en LL), baud inalcanzable, puntero nulo;<br>• estado exacto (`false`/`HAL_ERROR`);<br>• `CR1`–`CR3`, `BRR`, `PRESC`, los `MODER`/`AFR` de PD3–PD6 y `RCC_APB1LENR` sin cambiar (U3) | Instantánea de registros antes y después | Una llamada válida en la misma tabla cambia la instantánea |
| **UT2** | **`PRESC`/`BRR` de BM y LL:** una tabla de (`f_ker`, baud, OVER8/16, USART/LPUART) en instancias sin pines (UART4, UART5, UART8, USART1, USART6, LPUART1), leyendo los registros. Incluye las filas de README §6 y los divisores pequeños e impares de U5 | La fórmula del host (Python, propia), con el ±2 % sobre el divisor aplicado | Una fila mutada en la tabla del host da FAIL |
| **UT3** | **Formato en el cable** con BM, LL y HAL (`InitEx`):<br>• 8N1, 8E1, 8O1, 7E1, 7N1, 9N1 y 2 bits de parada;<br>• a 9600, 115 200 y 921 600 (OVER8), con patrones fijos | El LA del AD2 en DIO15 decodifica bit a bit: inicio, datos, paridad y parada. El ancho de bit se mide contra MCO1 (DIO14) y se compara con el teórico del `BRR` programado | La misma decodificación con la paridad esperada invertida da FAIL |
| **UT4** | **Recepción por el bucle (JU1):**<br>• BM y LL;<br>• FIFO apagado y encendido (`RXFNE`), OVER8 y OVER16;<br>• máscara de 7 bits;<br>• LL por IRQ hacia el *ring buffer*, con FIFO, en los dos núcleos (`USART2_IRQn`) | Lo enviado frente a lo recibido, byte a byte; el LA cuenta las tramas | Un byte corrompido en la tabla esperada da FAIL |
| **UT5** | **Errores inyectados** (el AD2 conduce PD6; SU1–SU3):<br>• PE (paridad mala);<br>• FE (parada a 0 y BREAK);<br>• NE (pulso corto dentro de la ventana de muestreo);<br>• ORE (bytes sin leer, con FIFO y sin él).<br>Cada error lo recoge `ReadByte`/`IrqService` y `TakeErrors` lo devuelve una sola vez | El patrón del AD2, capturado también por el LA | Una trama correcta en la misma secuencia no da error |
| **UT6** | **RTS/CTS por JU2:** con `RTS_CTS`, N bytes sin leer, N > FIFO + 1. TX se detiene sin ORE, y al leer se reanuda. Sin control de flujo, ORE | El LA cuenta las tramas en el cable en cada fase | La misma secuencia sin control de flujo da ORE |
| **UT7** | **`UART_HAL_InitEx` en USART2 (CM7), no la consola:** pines (AF, pull-up, velocidad, por instantánea), 8E1 por el bucle y en el cable; U6 con el lock de GPIO tomado por CM4 → `HAL_BUSY` | Instantánea, el LA y el valor devuelto | Lock libre: `HAL_OK` |
| **UT8** | **`UART_HAL_CheckDmaErrata` en vivo:**<br>• DMA1 con USART2 TX: bit 20 a 1 y leído, y la transferencia llega por el bucle y por el LA;<br>• `HAL_BUSY` con el stream activo;<br>• `HAL_ERROR` con una petición que no es de UART y con un handle BDMA | El LA y la lectura de `DMA_SxCR` | Los tres casos de rechazo |
| **UT9** | **U1:** `UART_HAL_Init` de la consola con el lock de GPIO tomado por CM4 → `HAL_BUSY`; liberado → `HAL_OK`. El estado no queda pegado: un `Init` correcto después devuelve `HAL_OK` | El valor devuelto y los pines PD8/PD9 | El control `b41c4a7` da `HAL_OK` con el lock tomado. Ese es el fallo de U1, y la herramienta lo marca como esperado en el control |
| **UT10** | **Regresión:** el selftest de `prj_04` (`[m4_uart_test] ... bm_loop=1,1,1 ll=1,1 ore=1`) con JU1 de 1 kΩ | La línea de la consola | — |

**Captura de cada ejecución:**
- las dos ejecuciones completas del banco: `prj_01/docs/uart_hw_*_<hash>.txt`;
- los warm resets: `run_warm_resets.py`, 20/20.

**El criterio de cierre** son dos ejecuciones limpias completas del auditor, con `FW VERDICT=PASS` y `HOST VERDICT=PASS`.

---

## Fases y entregables

1. **F1 (implementador):**
   - H1–H5;
   - T1, T2 y la prueba del host con el mock, con sus capturas;
   - los comandos del banco para el auditor.

   Termina con `F1 ENTREGADA PARA AUDITORIA <hash>`.
2. **Auditoría de F1:**
   - el diff;
   - T1 y T2 en ejecución propia;
   - UT0–UT10, dos veces, en la placa.
3. **Cierre (implementador, cuando `AUDIT.md` lo autorice):**
   - README: estado, §1.1, §3.1, §4, §6, §9 con UT0–UT10 y sus capturas, §10 con U7 y §12;
   - el puerto de `00_gpio` §8 con JU1, JU2 y DIO15.

   Termina con `CIERRE ENTREGADO PARA AUDITORIA <hash>`.
4. **Cierre del responsable:**
   - la fila del roadmap;
   - borrar los documentos de proceso;
   - quitar el worktree de control;
   - el merge.

---

## Riesgos y mitigaciones

| Riesgo | Mitigación |
| --- | --- |
| Cruce entre PD5 y el AD2 en PD6 | JU1 con 1 kΩ, más SU1–SU3 |
| Medidas de baud falseadas por el reloj de la placa (+1,3 %) | Medir contra MCO1 en la misma captura |
| La ventana de NE depende del muestreo (3 muestras a mitad de bit) | UT5 a 9600 baud, con el pulso centrado y su anchura en la captura. Si el hardware no lo marca, UT5 lo registra y se decide en la auditoría |
| `prj_04` y `02_dma` usan PD5–PD6 | UT10 los repite con JU1 |
| Una herramienta que escribe en `.git/config` o cuenta avisos donde no los hay | Reglas de verificación de arriba |

---

## Decisiones abiertas

1. **Proyecto de las pruebas.**
   - **A (recomendada):** `prj_01` `selftest-uart`, con el marco del banco del AD2 (handshake, `bench_claim()` y guardas de `ad2.py`).
   - **B:** ampliar el `selftest-uart` de `prj_04`, que no tiene handshake con el host ni guardas del AD2.
2. **Banco.**
   - **A (recomendada):** JU1 con 1 kΩ, JU2 y DIO15 en PD6, como arriba.
   - **B:** solo los bucles JU1 y JU2, sin AD2. Sin oráculo en el cable y sin UT5: los errores se limitan a ORE y BREAK.
3. **LPUART1.**
   - **A (recomendada):** solo `PRESC`/`BRR` (UT2). En el cable queda como no ejercitado: sus pines (PB6/PB7, Arduino D1/D0) dependen de puentes de soldadura que pueden llevarlos al ST-LINK (UM2408, SB11/SB96/SB15/SB94).
   - **B:** comprobar los puentes en la placa y, si PB6/PB7 están libres, añadir un bucle PB6 → 1 kΩ → PB7.
4. **Documentos de proceso:** se borran al cierre.
