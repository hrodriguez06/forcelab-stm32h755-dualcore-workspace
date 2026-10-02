# Informe de implementación 03_tim

Implementador: Claude (Claude Code), para Hector Rodriguez · Rama: driver/tim · Plan: IMPLEMENTATION_PLAN.md revisión 1

## Resumen

F0 (banco) está implementada y entregada para auditoría. Contiene:

- la guarda S11 y el S2 ampliado en `tools/ad2.py`;
- `bench_claim()`/`bench_safe()` en los dos núcleos, con S12 en CM7;
- la opción `TIM_DRIVER_SELFTEST` y el preset `selftest-tim`;
- el selftest de F0 en `prj_01` (área SRAM4 en `0x3800B000`, tabla de pasos v1, Rev e id de los dos núcleos, veredicto calculado por CM7);
- `tools/tim_hw_check.py`.

La captura de entrega, `docs/tim_f0_bench_456a7a7.txt`, da 14/14 comprobaciones del host, `FW VERDICT=PASS` y `HOST VERDICT=PASS`.

TREF no se hizo como dice el plan: el responsable cambió dos cosas el 2026-10-01, el control (PA8 en AF1) y el criterio (|e| ≤ 2 % más la verificación cruzada). Las dos primeras ejecuciones fallaron por esas dos erratas del plan y se conservan (§5.7).

No hay código del driver ni targets `_tim_*`, porque F0 no los lleva. F1 no ha empezado: espera a que `AUDIT.md` diga que F0 pasa.

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F0 | entregada para auditoría | `456a7a7` (código); evidencia en `a233989` | `03_firmware_templates/prj_01_template_dualcore/docs/tim_f0_bench_456a7a7.txt` |
| F1 | sin empezar | — | — |
| F2 | sin empezar | — | — |
| F3 | sin empezar | — | — |

## F0

### 5.1 Commits

Rango de la entrega: `21c0683..` el commit que añade este informe, en `driver/tim`.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `eb5e440` | feat(tools): guard Digital Out and extend S2 to it in ad2.py (S11) | — |
| `987e207` | test(prj_01): add the TIM_DRIVER_SELFTEST option and the selftest-tim preset | — |
| `a271fd5` | test(prj_01): add the F0 bench selftest of 03_tim on both cores (S4, S12) | — |
| `d01061e` | test(prj_01): add tim_hw_check.py, the host of the 03_tim F0 bench | — |
| `38d15c1` | test(prj_01): use PA8 in AF1 as the TREF control instead of MCO1PRE = 0000 | errata E1 (§5.7) |
| `456a7a7` | test(prj_01): judge TREF at \|e\| <= 2 % plus the HAL_GetTick cross-check | errata E2 (§5.7) |
| `a233989` | docs(tim): add the F0 captures and the build evidence | — |
| (este) | docs(tim): add the implementation report for F0 | — |

El mensaje de `456a7a7` cita "UM2408 6.7.1", y esa sección no es correcta: es "OSC clock supply", página 28 de UM2408 Rev 6 (§5.7, E2). El historial no se reescribe, así que lo corrijo aquí.

### 5.2 Build y tamaños

- `build_all -Clean` en `456a7a7`: 8/8 (`docs/tim_f0_build_all_456a7a7.txt`).
- `selftest-tim` desde un directorio vacío: sale con 0 y 0 líneas `warning:` (`docs/tim_f0_selftest_build_456a7a7.txt`).
- `-Werror`: `prj_01` no lleva `-Werror` en sus flags (§5.7, E3). Por eso compilé `selftest-tim`, `debug` y `release` con `CMAKE_C_FLAGS=-Werror` en directorios de scratch: salida 0 y 0 avisos en las tres (`docs/tim_f0_werror_456a7a7.txt`).

Tamaños del `.map` enlazado (solo secciones de salida asignadas; fuente `docs/tim_f0_map_sizes_456a7a7.txt`):

| Imagen | .text | .data | .bss | Δ frente a e7542dc | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| prj_01 debug CM7 | 29144 | 132 | 55668 | 0 en cada sección asignada | `build/debug/cm7_firmware.map` |
| prj_01 debug CM4 | 3456 | 24 | 12560 | 0 | `build/debug/cm4_firmware.map` |
| prj_01 release CM7 | 28228 | 132 | 55672 | 0 | `build/release/cm7_firmware.map` |
| prj_01 release CM4 | 3112 | 24 | 12560 | 0 | `build/release/cm4_firmware.map` |

La columna .text agrupa `.isr_vector+.text+.rodata+.ARM`, .data agrupa `.data+.init_array+.fini_array` y .bss agrupa `.bss+._user_heap_stack+.shared_ram`. Es la misma agrupación de `arm-none-eabi-size` y de `build_all.ps1`.

`e7542dc` se compiló en un worktree desacoplado, con los valores de caché de sus presets. Las secciones `.debug_*` cambian con la ruta del build y no forman parte de la imagen.

| Objeto | Núcleo | .text | .data | .bss | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| `tim_bm.c.obj`, `tim_ll.c.obj`, `tim_hal.c.obj` | — | no existen en F0 | — | — | — |
| `tim_selftest.c.obj` (selftest) | CM7 | 8245 | 4 | 138 | `build/selftest-tim/cm7_firmware.map` |
| `tim_selftest_common.c.obj` (selftest) | CM7 | 629 | 0 | 0 | ídem |
| `tim_selftest.c.obj` (selftest) | CM4 | 979 | 0 | 0 | `build/selftest-tim/cm4_firmware.map` |
| `tim_selftest_common.c.obj` (selftest) | CM4 | 601 | 0 | 0 | ídem |

### 5.3 Pruebas

Todas las cifras salen de `docs/tim_f0_bench_456a7a7.txt`, salvo donde se indica otra captura.

| ID | Tier | Núcleo | Resultado | Valor medido | Control negativo: valor | Oráculo | Captura |
| --- | --- | --- | --- | --- | --- | --- | --- |
| T0a sueltas | — | CM7 | PASS | AD2: pull-up `0x4FFF`, pull-down `0x0000` (13 líneas). MCU: `IDR` pull-up `0x1FFF`, pull-down `0x0000` | — (abajo) | DIO del AD2, `IDR` | `tim_f0_bench_456a7a7.txt` |
| T0a walking 1 | — | CM7 | PASS | 13 patrones; diferencias AD2 `[]`, MCU `0x0000` | — (abajo) | DIO del AD2, `IDR` | ídem |
| T0a control | — | CM7 | PASS | DIO9 (PE5) con pull-down a propósito donde el host espera pull-up | El chequeo marca `0x0200` (esperado `0x0200`), tanto el del host como el del MCU | DIO del AD2, `IDR` | ídem |
| TREF | — | CM7 | PASS | MCO1 = HSE/8: 1 013 171,226 Hz, 40 periodos, 3948 de 4096 muestras a 100 MS/s; e = +13 171,2 ppm, resolución 253,3 ppm. Cruzada: +13 236 ± 191 ppm, \|e − cruzada\| = 65 ≤ 573 | PA8 en AF1 (TIM1_CH1), `TIM1EN` C1 = C2 = 0 y MCO1 sigue en HSE/8: 0 flancos de subida y 0 de bajada | DIO14 del AD2; `HAL_GetTick` frente al reloj del host | ídem |
| TREF RCC_CFGR | — | CM7 | PASS | Campos MCO1 `0x00A00000` antes del control y durante él; `RCC_CFGR` antes `0x0000001B`, después `0x0000001B` | — | lectura directa de `RCC->CFGR` | ídem |
| T0b(a) | — | host | PASS | `ad2_set_oe` rechaza 8 máscaras antes de la DLL (el mock de `gpio_hw_check.py`, sin cambios) | 2 máscaras permitidas pasan a la DLL | DLL mock | ídem |
| T0b(b) CM7 | — | CM7 | PASS | 20 rechazos; aceptados por error `0x00000000`; registros de GPIOA/E/F intactos | PA8 en AF0 aceptado en el paso T0B | lista de casos escrita a mano; foto de `MODER/OTYPER/PUPDR/AFR` | ídem |
| T0b(b) CM4 | — | CM4 | PASS | 13 casos en T0B: aceptados `0x0001` (solo PE5 AF4). Entre pasos: `0x0000`. Registros cambiados: 0 | PE5 AF4 sí se acepta en T0B (y se rechaza entre pasos); `bench_safe` de CM4: PE5 pasa de `MODER=2`/`AFR=4` a `0`/`0`, `PUPDR=0` | máscara esperada escrita a mano (`TST_M4_CLAIMS_EXPECT_*`); hash de los registros | ídem |
| T0b(c) | — | CM7 + host | PASS | — | El MCU conduce `0x5A`: `s5_released()` devuelve `False` dos veces, con DIO = `0x5A` | DIO del AD2 | ídem |
| T0b(d) | — | CM7 | PASS | Sin HOST_REQ durante 10 s: `GPIOE MODER[31:4] = 0x00004000`, `AFRH = 0`, ACK bajo | — (es el propio caso negativo) | `MODER` leído directamente, DIO13 | ídem |
| T0b(e) | — | host | PASS | 12 rechazos de `ad2_set_dout`: S2 alcanzado y ninguna llamada de habilitación llega a la DLL | 4 llamadas permitidas pasan, con Idle y push-pull antes de `EnableSet(1)` y `Configure(1)` al final | DLL mock que registra el orden de las llamadas | ídem |
| T0b(f) | — | CM7 + host | PASS | DIO1 a 1,0000 MHz, 500,0 ns en alto, 40 periodos; enable `0x0002`; OE de Digital-IO `0x0000` mientras corre; 0 flancos en las otras líneas. Tras `Configure(0)` con Idle = Zet, `s5_released()` arriba y abajo da True; el MCU lee arriba `0xFF` y abajo `0x00` | Idle = Low: `s5_released()` arriba da False con DIO `0xFD`; el MCU lee arriba `0xFD`. Después de S2: enable `0x0000`, Zet `0xFFFF` y S5 vuelve a pasar | DIO del AD2, `IDR`, `FDwfDigitalOutEnableGet`/`IdleGet` | ídem |
| T0b(g) | — | CM7 + host | PASS | 13 casos TIM1/pin rechazados (CCxS = 00 ×3, MOE, OSSI, OSSR, CC1NE, CC2NE, CC3E, CC4E, AF2, PE13, PE8), con los registros GPIO intactos. Con PE9/PE11 en AF1, S5 pasa en el host y el MCU lee arriba `0xFF` y abajo `0x00`. TIM1 vuelve a 0 y su reloj queda como estaba (apagado) | La configuración correcta (`CCMR1 = 0x00000101`, `CCER = 0`, `BDTR = 0`) sí se acepta | fotos de `MODER`/`AFR` (`MODER = 0x00884007`, `AFRH = 0x00001010`) y de los registros de TIM1 | ídem |

Notas sobre las comprobaciones:

- **Las dos de "pasa porque si no, abortaría".** "T0B(f) bus released after S2" y "T0B(g) S5 with PE9/PE11 in AF1" registran PASS porque `buschk()` lanza `BenchAbort` si falla. No son comprobaciones independientes de la ejecución de S5 que tienen justo encima.
- **Ejecuciones anteriores que fallaron:**
  - `tim_f0_bench_d01061e.txt`: TREF dio e = +13 171,2 ppm, fuera de los 500 ppm del plan. Su control `MCO1PRE = 0000` dio 332 flancos de subida y 331 de bajada (esperados 0). Las otras 12 comprobaciones pasaron.
  - `tim_f0_bench_38d15c1.txt`: TREF dio e = +13 684,7 ppm, fuera de 500 ppm. El control nuevo dio 0 flancos. Las otras 13 pasaron.

### 5.4 Medidas (B1)

- En F0 no hay funciones del driver, así que no hay ciclos que medir ni peor caso de `CalcTimeBase`.
- Resolución de TREF: 253,3 ppm. Es una muestra sobre un tramo de 3948 muestras; el buffer máximo de DigitalIn del AD2 es de 4096.
- Incertidumbre de la verificación cruzada: ±191 ppm, que equivale a 2 ms (supuestos) sobre 10,464 s.
- Pila de `Status_Task` con el selftest: high-water mark de 314 palabras libres de 512, con el tamaño de producción, que no cambié.

### 5.5 Desviaciones del plan

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| Control de TREF: "`MCO1PRE = 0000` (prescaler deshabilitado): sin flancos" | PA8 en AF1 (TIM1_CH1) con TIM1 sin reloj en los dos bancos y MCO1 en HSE/8. El paso TREF permite PA8 AF1 (`af1_pa`); T0B sigue rechazándolo | `MCO1PRE = 0000` deja pasar HSE sin dividir (E1). Es el control de T10 de 00_gpio (0 flancos en `gpio_selftest_7456494.txt` y `gpio_selftest_b45120e.txt`) | Hector, 2026-10-01: "Usa PA8 en AF1 como control de TREF" |
| Criterio de TREF: \|`e_ppm`\| ≤ 500 | \|e\| ≤ 20 000 ppm, y además \|e − cruzada\| ≤ 3·u. La cruzada es `HAL_GetTick` (entre el banner y el veredicto) frente al reloj del host, con u = 2 ms / tramo | El HSE de esta placa va a +1,3 % (E2) | Hector, 2026-10-01: "Usa la opción (a): \|e\| ≤ 2 % con la verificación cruzada" |
| Controles de T0a: "Los de 00_gpio T0a" (allí "-", ninguno) | Añadí un control: el firmware pone DIO9 con pull-down donde el host espera pull-up, y los dos chequeos tienen que marcar exactamente `0x0200`. También hice el walking 1, que el plan pide y 00_gpio no tiene (E4) | Instrucciones §3.5: toda prueba imprime su control negativo | No consultado: lo añadí porque lo exige §3.5 |
| Tabla de líneas: PE8, PE10, PE12 y PE15 "solo entrada en las pruebas de TIM" | En T0b(c), como en 00_gpio, el MCU conduce `0x5A` en las 8 líneas del bus como salidas GPIO (nunca en AF), en un paso "bus = MCU" | El plan pide T0b(a)–(d) "como en 00_gpio" | No consultado; lo señalo para el auditor |
| TREF "al principio" y T0b "antes que todo lo demás" | Orden: T0A → TREF → T0B… | T0A comprueba primero que nadie más conduce las líneas, PA8 incluida. TREF no mueve el bus ni usa Digital Out | No consultado |
| S11 (c): "Todo canal habilitado lleva Idle = Zet" | `ad2_set_dout` acepta además `idle = Low`, solo para DIO1/DIO3 en un paso "bus = AD2" | El control de T0b(f) lo exige; S2 vuelve a dejar Zet | No consultado; es el control que pide el plan |
| S11 (d): OE de Digital-IO a 0 "en todos los bits" mientras corre Digital Out | Lo apliqué literalmente: HOST_REQ también queda sin conducir (el MCU tiene pull-down en PE2) y Digital Out corre y se para dentro de un mismo handshake | Es la lectura literal | No consultado. Para T8 (F1) importa: ver §5.8 |
| T0b(g): tres controles (CC1S = 00, MOE = 1, OSSI = 1) | 13 casos: también CC2S = 00, OSSR, CC1NE, CC2NE, CC3E, CC4E, AF2, PE13 y PE8 | Cubre todas las condiciones de S12 | Ampliación, sin consulta |
| `bench_safe()`: pines cableados en entrada | Además pone a 0 los campos `AFR` de los pines cableados | Un S2 más limpio | Ampliación, sin consulta |
| Lista de comprobación "marcada" | Los puntos físicos (cableado, flasheo con el script parado) solo se marcan con `--checklist-confirmed NAME`, de quien está en el banco. WaveForms y los servidores de depuración los comprueba `tasklist` | Yo no puedo verificar el cableado físico | Hector confirmó la lista el 2026-10-01 antes de flashear |

### 5.6 Respuesta a la auditoría n

No hay auditoría todavía.

### 5.7 Erratas del plan encontradas

| Id | Sección del plan | Qué dice | Qué es correcto | Fuente |
| --- | --- | --- | --- | --- |
| E1 | Plan de pruebas, fila TREF, control | `MCO1PRE = 0000` (prescaler deshabilitado): sin flancos | En este silicio (rev V, `REV_ID = 0x2003`) `0000` deja pasar HSE sin dividir: 332 flancos de subida en 40,96 µs, unos 8,1 MHz. "Prescaler disabled" no significa salida apagada | RM0399, RCC_CFGR bits 21:18 (`00_docs_stm32h7/docs/peripherals/rcc/09_reset-and-clock-control-rcc.txt`, líneas 3751–3756); `docs/tim_f0_bench_d01061e.txt`, línea 67 |
| E2 | Fila TREF, criterio, y "Lo que existe hoy", reloj de los timers = 200 MHz | \|`e_ppm`\| ≤ 500, y relojes nominales de 8/400/200 MHz | El HSE de la placa (MCO del ST-LINK) mide +13 171 / +13 685 / +13 171 ppm con el AD2 en tres ejecuciones, y `HAL_GetTick` frente al host +13 657 / +13 725 / +13 236 ppm: unos 8,105 MHz. Los timers van a unos 202,7 MHz y CM7 a unos 405 MHz. UM2408 dice que el MCO es 8 MHz por defecto, pero que "might be changed during an ST-LINK firmware upgrade (RN0093)". En F1, lo que se mide contra `CYCCNT` no cambia (mismo reloj); lo que mide el AD2 tiene que corregirse con TREF, como ya dice el plan | `docs/tim_f0_bench_{d01061e,38d15c1,456a7a7}.txt`; UM2408 Rev 6, "OSC clock supply", p. 28; 00_gpio T10: 4,061–4,063 MHz para HSE/2 (`00_gpio/README.md` §9.1) |
| E3 | C9 y Fases, gate F0: "8/8 con `-Werror`" | `build_all` da 8/8 con `-Werror` | `prj_01` no compila con `-Werror`: `03_firmware_templates/prj_01_template_dualcore/cmake/common_flags.cmake` no lo lleva, solo `prj_04` (`02_firmware_projects/prj_04_adc_dma_bm_eth/cmake/common_flags.cmake:20`). Añadirlo cambiaría la producción de `prj_01`, que está fuera de §2.1; lo suplí con `docs/tim_f0_werror_456a7a7.txt` | los dos `common_flags.cmake` |
| E4 | Fila T0a | "Prueba de líneas sueltas y walking-1 del banco, exactamente como en 00_gpio" con "los controles de 00_gpio T0a" | El T0a de 00_gpio no tiene walking 1 ni control negativo | `01_shared_drivers/00_gpio/README.md` §9.1, fila T0a (control "-"); `CM7/App/Src/gpio_selftest.c`, `step_t0a` |

### 5.8 Problemas abiertos y no ejercitado

- **T8 (F1) y S11 (d).** Con OE de Digital-IO a 0 en todos los bits, HOST_REQ no se puede conducir mientras corre Digital Out. En F0 Digital Out corre y se para dentro de un solo handshake. Para T8, que captura mientras el AD2 genera, hay que decidir antes de F1 si (d) se aplica a todos los bits o solo a los canales de Digital Out. Lo pregunto antes de empezar F1.
- **Lectura al abrir el AD2.** En la captura de entrega, al abrir el AD2 se leyó `DIO = 0x2001` (DIO13 = MCU_ACK y DIO0 = PE8 en alto). Fue antes del reset por SWD, con OE = 0 y Digital Out deshabilitado, así que el AD2 solo leía y no hubo contienda. Mi explicación, que no he verificado: la imagen recién flasheada esperaba en el primer handshake de T0A (ACK alto), con PE8 en entrada sin pull. En las otras dos ejecuciones se leyó `0x0000`.
- **Incidente de esta sesión, sin efecto en las capturas.** Durante el desarrollo, importar el shim `tools/monitor.py` abrió COM4 y se quedó escuchando unos 2 minutos a la imagen que había entonces en la placa, que no era `prj_01`. No se envió nada ni se usó el AD2. Quedó corregido antes del primer commit del script (`d01061e`, orden de `sys.path`).
- **Supuesto de la verificación cruzada.** Los 2 ms de jitter de llegada de una línea por el VCP son un supuesto, no una medida. Con él, la diferencia en `d01061e` fue de 486 frente a una cota de 570.
- **`DBGMCU`.** El plan dice que `DBGMCU` no se toca nunca. El banner de CM7 lee `DBGMCU->IDCODE` (solo lectura, para imprimir `REV_ID`), igual que el selftest de 00_gpio. No hay ninguna escritura. Si el auditor entiende que leerlo también cuenta como tocarlo, se quita.
- **Placa.** Las pruebas se hicieron en una sola placa y un solo silicio (rev V, `REV_ID = 0x2003`), con un solo operador. Nadie las ha repetido de forma independiente todavía.
- **Imagen de la placa.** La placa tenía otra imagen (imprimía `[DSP] …` en COM4) y ahora tiene `selftest-tim` de `456a7a7`.

## Decisiones abiertas usadas

En F0 ninguna de las seis tiene código detrás. Las anoto con la opción por defecto del plan para F1–F3.

| # | Opción usada | Consultada |
| --- | --- | --- |
| 1 | `TimBm_Status_t` en los tres tiers (por defecto) | No; sin código en F0 |
| 2 | El alcance de la tabla "Alcance" (por defecto) | No |
| 3 | El PSC más pequeño que cabe (por defecto) | No |
| 4 | TIM6 de `prj_01` sin migrar (por defecto); no se toca | No |
| 5 | `tim_hal` no se ejercita en CM4 (por defecto) | No |
| 6 | Documentos de proceso borrados al cierre (por defecto); no hago yo el borrado | No |

## Tabla de capacidades

Pendiente de F1 (instrucciones §6: F1 empieza por ella, con la cita del RM en cada fila). En F0 no he comparado ninguna celda con el RM.

## F0: cambio después de la entrega (S11 d)

Estado: entregada para auditoría (`e30622b` código, evidencia en `6e2494a`). Hay una pregunta abierta sobre la cota de TREF, más abajo. Las secciones anteriores no se tocan; esta sección sustituye a la fila "S11 (d)" de §5.5 y al primer punto de §5.8.

### Commits

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `e30622b` | feat(tools): apply S11 (d) to the Digital Out channels only | — |
| `6e2494a` | docs(tim): add the F0 capture at e30622b (S11 d on the Digital Out lines) | — |
| (este) | docs(tim): record the S11 (d) change and the TREF bound question | — |

### Desviación

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| S11 (d): mientras corre Digital Out, el OE de Digital-IO es 0 "en todos los bits" | `ad2_set_dout` rechaza el arranque solo si el OE de Digital-IO lee 1 en un bit de su máscara (DIO1/DIO3). HOST_REQ puede seguir habilitado | T8 (F1) necesita el handshake mientras el AD2 genera. SDK §8: Digital Out se aplica por línea, donde Enable y Output de Digital-IO son 0 | Hector, 2026-10-01: "Aplica S11 (d) solo a los canales de Digital Out" |

### Build

- Desde `456a7a7` solo cambiaron `tools/ad2.py` y `tools/tim_hw_check.py` (`git diff --stat 456a7a7 e30622b`).
- Por eso `tim_f0_build_all_456a7a7.txt`, `tim_f0_werror_456a7a7.txt` y `tim_f0_map_sizes_456a7a7.txt` siguen describiendo el código C y las imágenes de producción.
- `selftest-tim` en `e30622b`, desde un directorio vacío: salida 0 y 0 avisos (`docs/tim_f0_selftest_build_e30622b.txt`).

### Pruebas que cambian

Las cifras salen de `docs/tim_f0_bench_e30622b.txt`, que da 14/14, `FW VERDICT=PASS` y `HOST VERDICT=PASS`. Las demás pruebas dan los mismos valores que en `456a7a7`, salvo TREF.

| ID | Resultado | Valor medido | Control negativo: valor |
| --- | --- | --- | --- |
| T0b(e) | PASS | 14 rechazos: los 12 de antes, sustituyendo el caso de OE = HOST_REQ por OE en DIO1 (con HOST_REQ), OE en DIO3 y DIO1\|DIO3 con OE en DIO3. En todos, S2 alcanzado y ninguna llamada de habilitación | 6 controles pasan; dos de ellos con OE = HOST_REQ (`0x1000`) |
| T0b(f) | PASS | DIO1 a 1,0000 MHz, 500,0 ns en alto; OE leído mientras corre: `0x1000` (solo HOST_REQ, 0 en DIO1). Con Idle = Zet, S5 da True arriba y abajo | Idle = Low: `s5_released()` arriba da False con DIO `0xFD` |
| TREF | PASS, en el límite | e = +13 941,7 ppm, resolución 253,5 ppm; cruzada +13 368 ± 191 ppm. \|e − cruzada\| = 574 frente a una cota 3 × 191 = 574 (los dos valores salen redondeados en la captura) | PA8 en AF1: 0 flancos |

### Problema abierto: la cota de TREF no cuenta la cuantización del AD2

En las cuatro ejecuciones el e de TREF salta en pasos de una muestra del LA: 40 periodos medidos sobre 3948, 3946, 3948 y 3945 muestras, que dan +13 171,2, +13 684,7, +13 171,2 y +13 941,7 ppm. Cada muestra vale unos 254 ppm.

La cota que se decidió, 3·u, solo cuenta la incertidumbre de la verificación cruzada (u ≈ 191 ppm). No cuenta la resolución de TREF. En `e30622b` la diferencia quedó justo en la cota. Por azar, la misma imagen puede fallar en otra ejecución, también en la del auditor (A18).

Propuesta, que no he aplicado porque es un criterio: \|e − cruzada\| ≤ 3·√(u² + r²), donde r es la resolución de TREF de esa misma captura (≈ 253 ppm). Con los datos de esta ejecución daría ≈ 951. Queda pendiente de que lo decida el responsable.

## F0: cota de TREF con la resolución

Estado: entregada para auditoría (`d69508e` código, evidencia en `3fffb50`). Esta sección responde a la pregunta abierta de la sección anterior con la decisión del responsable.

### Commits

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `d69508e` | test(prj_01): count the TREF resolution in the cross-check bound | — |
| `3fffb50` | docs(tim): add the F0 capture at d69508e (TREF bound with the resolution) | — |
| (este) | docs(tim): record the TREF bound with the resolution | — |

### Desviación

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| TREF: \|e\| ≤ 500 ppm (ya cambiado a \|e\| ≤ 2 % con \|e − cruzada\| ≤ 3·u) | \|e\| ≤ 2 % y \|e − cruzada\| ≤ 3·√(u² + r²), donde r es la resolución de TREF de la misma captura | Con 3·u sola, `e30622b` pasó justo en la cota (574 frente a 574), porque e se mueve en pasos de una muestra (unos 254 ppm) | Hector, 2026-10-01: "Usa la cota 3·√(u² + r²) para TREF" |

### Build

- Desde `456a7a7` no cambió ningún archivo C, de cabecera, CMake, preset ni linker script: solo `tools/ad2.py` y `tools/tim_hw_check.py`. Por eso la evidencia de `build_all`, `-Werror` y `.map` de `456a7a7` sigue valiendo.
- `selftest-tim` en `d69508e`, desde un directorio vacío: salida 0 y 0 avisos (`docs/tim_f0_selftest_build_d69508e.txt`).

### Pruebas

`docs/tim_f0_bench_d69508e.txt` da 14/14, `FW VERDICT=PASS` y `HOST VERDICT=PASS`. Las demás pruebas dan los mismos valores que en `e30622b`.

| ID | Resultado | Valor medido | Control negativo: valor |
| --- | --- | --- | --- |
| TREF | PASS | e = +12 914,7 ppm (40 periodos sobre 3949 muestras), r = 253,2 ppm; cruzada +12 846,7 ± 191,7 ppm. \|e − cruzada\| = 68,0 frente a una cota de 952,9 | PA8 en AF1: 0 flancos |

Las cuatro ejecuciones anteriores, con la cota nueva aplicada a mano sobre sus valores impresos, darían 485,8, 40,3, 64,8 y 573,7, todas frente a unos 950. Con la cota de cada una de ellas, `d01061e` y `38d15c1` fallaron por las erratas E1 y E2, no por esta cota.

Observación: al abrir el AD2 se volvió a leer `DIO = 0x2001`, el mismo caso de §5.8: antes del reset por SWD, con OE = 0 y Digital Out deshabilitado.

## Respuesta a la auditoría 1 (F0, parte 1 y adenda; plan revisión 2)

Desde esta sección, el plan de referencia es la revisión 2 (`0c8c86d`). Estado de F0: entregada para auditoría (`8c4ab93` código, evidencia en `6f9c1f2`).

### Hallazgos

| Hallazgo | Commit que lo corrige | Qué ejecuté para comprobarlo | Estado |
| --- | --- | --- | --- |
| m3: comentarios que llaman LD2 a PE0 | `34de37b` | Releí los cuatro comentarios contra `Common/Inc/board_def.h:56` (LD2 = PE1). Además de los dos señalados, corregí otros dos iguales en `CM7/App/Src/tim_selftest.c` (comentario de `bench_safe` y primera fila de `k_t0b_refuse`). Solo comentarios | corregido en `34de37b`, pendiente de verificación |
| m4: oráculo de CM4 en T0b(b) basado en un hash | `e9a09c9` | `port_snap()` de CM4 toma `MODER`, `OTYPER`, `OSPEEDR`, `PUPDR`, `AFR[0]` y `AFR[1]` de GPIOA y GPIOE, y `regs_changed` pasa a ser la máscara de registros distintos. La foto de CM7 tampoco tenía `OSPEEDR`; también se lo añadí. Captura: `regs changed 0x000` en los dos casos de CM4 | corregido en `e9a09c9`, pendiente de verificación |
| m1 / R2-4: `Idle = Low` aceptado en cualquier paso "bus = AD2" | `6a6b5f1` | `ad2_set_dout(…, step_name)`: Low solo con `step_name == "T0B_DOUT"` (`AD2_DOUT_IDLE_LOW_STEP`). Mock de T0b(e): 16 rechazos, entre ellos Low en `T0B_S12` y Low sin paso; 7 controles, entre ellos Zet en `T0B_S12`. `gpio_hw_check.py` T0b(a) sigue pasando | corregido en `6a6b5f1`, pendiente de verificación |
| m2 / R2-10: u sale de un jitter supuesto de 2 ms | `8c4ab93` | CM7 imprime 16 líneas `TICK` en el banner y 16 en el veredicto, cada 50 ms y justo al cambiar `HAL_GetTick`. El host ajusta por mínimos cuadrados la llegada frente al tick: x es la pendiente, la dispersión de los residuos es la medida y u es el error estándar de la pendiente. En `tim_f0_bench_8c4ab93.txt`: 32 líneas en 12,121 s, dispersión 0,067 ms (banner 0,047, veredicto 0,083), x = +13 571,2 ppm, u = 2,2 ppm. Con datos sintéticos (x = +12 900, 0,6 ms) el ajuste da +12 866,6 ± 17,3 | corregido en `8c4ab93`, pendiente de verificación |

### Evidencia en `8c4ab93`

- `build_all -Clean`: 8/8 (`docs/tim_f0_build_all_8c4ab93.txt`).
- `selftest-tim` desde un directorio vacío: 0 avisos (`docs/tim_f0_selftest_build_8c4ab93.txt`).
- `CMAKE_C_FLAGS=-Werror` en `selftest-tim`, `debug` y `release`: salida 0 y 0 avisos (`docs/tim_f0_werror_8c4ab93.txt`).
- `.map`: `prj_01` debug/release frente a un build nuevo de `e7542dc`, 0 de diferencia en cada sección asignada (`docs/tim_f0_map_sizes_8c4ab93.txt`).
- Objetos del selftest (`.text+.rodata` / `.data` / `.bss`): CM7 `tim_selftest.c.obj` 8259 / 4 / 138; CM4 `tim_selftest.c.obj` 1039 / 0 / 0.
- Captura `docs/tim_f0_bench_8c4ab93.txt`: `Rev` 8c4ab93 en los dos núcleos, id `82382834bed143e0` igual en host, CM7 y CM4, árbol limpio, lista de comprobación confirmada por Hector. Resultado: 14/14, `FW VERDICT=PASS`, `HOST VERDICT=PASS`.

### Problema abierto: TREF varía más que su resolución r

Con u medido (2,2 ppm), la cota de R2-2 queda casi entera en r: 3·√(u² + r²) = 760,7. En `8c4ab93`, \|e − x\| = 627,5.

Si se compara, en cada ejecución, el tramo que miden los 40 periodos de TREF con el que predice x (4000 / (1 + x) muestras), sale esto:

| Ejecución | Tramo medido | Tramo según x | Desviación | e − x |
| --- | --- | --- | --- | --- |
| `d01061e` | 3948 | 3946,11 | +1,89 muestras | −486 ppm |
| `38d15c1` | 3946 | 3945,84 | +0,16 | −40 |
| `456a7a7` | 3948 | 3947,75 | +0,25 | −65 |
| `e30622b` | 3945 | 3947,23 | −2,23 | +574 |
| `d69508e` | 3949 | 3949,26 | −0,26 | +68 |
| `8c4ab93` | 3944 | 3946,44 | −2,44 | +628 |

Las cinco primeras x salen de dos puntos y la última del ajuste; con la dispersión medida, ninguna tiene más de unos 10 ppm de error.

La cuantización de los flancos solo puede mover el tramo menos de una muestra hacia cada lado. Tres de las seis ejecuciones se van más lejos. Por tanto, en una ventana de 40 µs la frecuencia de MCO1 se separa de su media de unos 12 s hasta unas 2,4 muestras, es decir, unos 620 ppm.

Mi hipótesis, que no he verificado, es que el MCO del ST-LINK tiene variación de frecuencia a corto plazo (jitter o *wander*). x también cambia de una ejecución a otra en unos ±400 ppm.

La consecuencia es que la cota actual pasa por poco: 627,5 frente a 760,7. Otra ejecución, también la del auditor (A18), puede fallar por esta variación del reloj y no por un defecto. No he cambiado nada, porque cambiaría cómo se mide TREF o su criterio. Lo pregunto al responsable. Dos opciones posibles:

- **(a)** Medir e como media de N capturas del LA, con su dispersión medida, y usar esa dispersión en lugar de r.
- **(b)** Añadir a r un término medido de la variación de MCO1.

## Respuesta a la auditoría 3 (F0; plan revisión 3)

Plan de referencia: revisión 3 (`92983ec`).

**Estado de F0: en curso, no se entrega para auditoría.** La ejecución del banco con el criterio de R3-1 falla en TREF. Como pide el plan, me detengo y lo informo con la captura (`docs/tim_f0_bench_2481396.txt`) sin cambiar el criterio.

### Cambios de la revisión 3

| Cambio | Commit | Qué ejecuté para comprobarlo | Estado |
| --- | --- | --- | --- |
| R3-3: `TIM_BENCH_VERSION = 2` y tabla `T0A`, `TREF_A`, `T0B`, `T0B_S5NEG`, `T0B_TIMEOUT`, `T0B_DOUT`, `T0B_S12`, `TREF_B`, `DONE`, igual en `k_tst_steps` y en `STEPS` | `949c39c` | `TREF_B` solo permite PA8 en AF0 (`af1_pa = 0`); `TREF_A` mantiene AF1 para su control; T0B sigue rechazando PA8 AF1. En la captura de `2481396`: protocolo v2 y S7 sin diferencias en los 9 pasos | aplicado en `949c39c`, pendiente de verificación |
| R3-1, TREF: 8 capturas por grupo, ≥ 40 periodos y ≥ 100 ms entre comienzos; el control solo en `TREF_A`; `RCC_CFGR` guardado y restaurado con read-back en cada grupo; se imprimen cada eᵢ con su rᵢ, ē_A, ē_B, ē, s y s_ef | `9986015` (host) y `949c39c` (`RCC_CFGR` por grupo, en el firmware) | Captura: `TREF_A captures` y `TREF_B captures` en PASS (8 capturas, mínimo 40 periodos, separación mínima de 100,2 y 100,4 ms); control de `TREF_A` con 0 flancos; `RCC_CFGR` restaurado en los dos grupos | aplicado en `9986015`, pendiente de verificación |
| R3-1, x: una ráfaga de 4 líneas `TICK` cada 50 ms al empezar cada paso, antes de `STEP`, más las de 16 en el banner y en el veredicto; ajuste de las M medias de ráfaga, u = error estándar de la pendiente con M − 2 grados de libertad; se imprime cada media, la pendiente de cada ráfaga, x y u | `abea673` | Captura: 11 ráfagas (16 + 9 × 4 + 16), 9 grados de libertad, x = +9 994,7 ppm, u = 15,4 ppm, dispersión de los residuos 0,304 ms. Con datos sintéticos (x = +13 500, 0,07 ms de jitter y 0,3 ms de latencia añadida a la ráfaga del veredicto), +13 494,8 ± 4,4 | aplicado en `abea673`, pendiente de verificación |
| R3-1, criterio: \|ē\| ≤ 20 000 ppm y \|ē − x\| ≤ 3·√(u² + s_ef²/16) | `0f9534c` | Fuera de línea: pasa con 28,8 ≤ 247,6 y falla con 328,8 y sin x. En la placa: **falla**, ver abajo | aplicado en `0f9534c`, pendiente de verificación |

### Defecto de `prj_01` encontrado y corregido (decisión del responsable)

Después de un arranque en frío, `prj_01` no arrancaba: `tim_f0_bench_0f9534c.txt` no recibe ninguna línea de consola tras el reset. Lo que leí por SWD, sin parar el núcleo:

- CM7 está en `SystemClock_Config` (`CM7/Core/Src/main.c:187-188`, PC `0x080008EA`/`0x080008EE`), esperando `VOSRDY`.
- `CFSR = HFSR = 0`, así que no hay ningún fallo.
- `PWR_CR3 = 0x46` es el valor de reset, y `PWR_CSR1 = 0x4000` tiene `ACTVOSRDY = 0`.
- `PWR_D3CR = 0xC000`: se pidió VOS 1, pero `VOSRDY = 0`.
- `RCC_RSR` tiene `PORRSTF`.

Un apagado y encendido no lo arregló.

La causa: `prj_01` nunca configura la alimentación. RM0399 §7.4.1 dice que mientras `ACTVOSRDY` no es válido VOS no puede cambiar. Esa configuración solo se escribe una vez hasta el siguiente arranque en frío, y hasta ahora la había escrito la imagen anterior de la placa.

El responsable decidió el 2026-10-02: "Aplica la opción (a) en un commit propio". `2481396` llama a `HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY)` al principio de `SystemClock_Config` y comprueba su retorno. La MB1363 es "internal SMPS only" por defecto (UM2408 Rev 6, §7.4.8, tabla 10), y es lo que hace `prj_04` (`main.c:297`). Con eso la placa arranca después de un arranque en frío (`tim_f0_bench_2481396.txt`).

Esto queda fuera del §2.1 de las instrucciones y cambia la producción de `prj_01`:

| Imagen | e7542dc (.text) | 2481396 (.text) | Δ | Resto de secciones asignadas |
| --- | --- | --- | --- | --- |
| debug CM7 | 27740 | 27900 | +160 | iguales |
| release CM7 | 26824 | 26952 | +128 | iguales |
| debug CM4 / release CM4 | — | — | 0 | iguales |

Fuente: `docs/tim_f0_map_sizes_2481396.txt`. El crecimiento son `HAL_PWREx_ConfigSupply` (0x94 B) y 16 B más en `SystemClock_Config`. **El gate de F0 "producción con los mismos tamaños que `e7542dc`" no se cumple desde `2481396`, y queda pendiente de que el responsable lo actualice.** Evidencia en `2481396`: `build_all` 8/8 (`tim_f0_build_all_2481396.txt`); `selftest-tim` y los tres builds con `-Werror` a 0 avisos (`tim_f0_selftest_build_2481396.txt`, `tim_f0_werror_2481396.txt`).

### Resultado en la placa (`docs/tim_f0_bench_2481396.txt`)

`Rev` 2481396 e id `934979b06027008b` en el host, CM7 y CM4, árbol limpio y lista de comprobación confirmada por Hector. Pasa todo lo que no es el criterio de TREF: T0a, T0b(a)–(g), las capturas y el control de `TREF_A`, las capturas de `TREF_B`, `RCC_CFGR` restaurado y `FW VERDICT=PASS`. Hay 15/16 comprobaciones del host y `HOST VERDICT=FAIL`, por una sola:

| Magnitud | Valor |
| --- | --- |
| eᵢ de TREF_A (ppm) | +10 866,8, +10 611,4, +10 599,0, +11 097,4, +10 599,0, +9 852,2, +10 356,2, +10 349,9 |
| eᵢ de TREF_B (ppm) | +10 349,9, +10 611,4, +10 349,9, +10 356,2, +10 101,0, +10 101,0, +10 101,0, +10 356,2 |
| ē_A, ē_B, ē | +10 541,5, +10 290,8, +10 416,2 ppm |
| s, r̄, r̄/√6, s_ef | 310,9, 249,9, 102,0, 310,9 ppm |
| x, u (11 ráfagas, 9 g. l., residuos 0,304 ms) | +9 994,7 ± 15,4 ppm |
| \|ē − x\| frente a la cota | 421,4 > 3·√(u² + s_ef²/16) = 237,7 → **FAIL** |

Hechos que el responsable debería tener en cuenta, sin que yo saque conclusiones:

- **e y x no coinciden, y los dos grupos de TREF caen del mismo lado.** ē_A y ē_B quedan por encima de x, +547 y +296 ppm, cuando según s cada media de 8 capturas tendría una desviación de unos 110 ppm.
- **Las ventanas cortas medidas con el host también dan más que el ajuste.** La pendiente propia de las dos ráfagas de 16 líneas, de 0,75 s cada una, es +10 228 (banner) y +9 893 (veredicto). La de las ráfagas de 4 líneas va de +9 689 a +11 096. El ajuste de las medias en 14,4 s da +9 995, y sus residuos (0,304 ms) son 4,5 veces la dispersión de la ráfaga larga de `8c4ab93` (0,067 ms).
- **El reloj de la placa cambió mucho entre sesiones.** x fue +13 571 en `8c4ab93` y +9 995 en `2481396`. Entre medias se desconectó y volvió a conectar el USB del ST-LINK, que es el que genera el HSE. No sé si es la causa.
- **u puede infravalorar la incertidumbre de x.** u (15,4 ppm) sale de 11 puntos con residuos de unos 0,3 ms y no incluye ninguna deriva de la latencia de llegada a lo largo de la sesión. No he podido separar si la diferencia de 421 ppm es del reloj de la placa (ventanas cortas frente a largas), de las referencias (cristal del AD2 frente al reloj del host) o de la latencia.

No he cambiado el criterio, el número de capturas ni el método. Pido decisión al responsable antes de seguir.

## F0: repetición de la ejecución del banco (revisión 3)

El responsable pidió el 2026-10-02 repetir la ejecución "para ver si TREF se repite".

La nueva captura es `docs/tim_f0_bench_89f11b5.txt`. Desde `2481396` solo cambió documentación, y la imagen se recompiló para que su `Rev` sea el de `HEAD`. Resultado: `Rev` 89f11b5 e id `cbcdd2b652baae60` en el host, CM7 y CM4, árbol limpio, lista de comprobación confirmada por Hector, **16/16, `FW VERDICT=PASS` y `HOST VERDICT=PASS`**.

| Magnitud | `2481396` (FAIL) | `89f11b5` (PASS) |
| --- | --- | --- |
| ē_A, ē_B | +10 541,5, +10 290,8 | +13 188,6, +12 038,9 |
| ē, s, s_ef | +10 416,2, 310,9, 310,9 | +12 613,7, 689,8, 689,8 |
| x, u (11 ráfagas) | +9 994,7 ± 15,4 | +12 549,1 ± 13,1 |
| Dispersión de los residuos de x | 0,304 ms | 0,258 ms |
| \|ē − x\| frente a la cota | 421,4 > 237,7 | 64,6 ≤ 518,8 |

El fallo de TREF **no se repite**. Pero en `89f11b5` las dos referencias muestran, de forma independiente, que el reloj de la placa baja durante la sesión:

- **AD2:** las eᵢ de `TREF_A` (al principio) van de +12 845,8 a +13 941,7 ppm; las de `TREF_B` (unos 12 s después), de +11 346,8 a +12 401,9.
- **Host:** la pendiente propia de cada ráfaga `TICK` baja de +12 736 (banner) y +13 884 (T0A) a +11 502 (DONE) y +11 846 (veredicto).

Es una deriva de unos 1 300 ppm en 14 s. Entre `2481396` y `89f11b5`, x pasó de +9 995 a +12 549 ppm en pocos minutos, sin que cambiara nada del banco.

Lo que se desprende de los datos, sin verificar la causa:

- **En cada momento las dos referencias coinciden.** El cristal del AD2 y el reloj del host dan lo mismo; lo que cambia es el reloj de la placa, es decir, el MCO del ST-LINK. Coincide con la nota N1 de la auditoría.
- **El criterio de R3-1 depende de la forma de la deriva.** Con una deriva casi lineal, como en `89f11b5`, la media de los extremos (ē) coincide con la pendiente del intervalo (x). Con una deriva no lineal, como parece en `2481396` (ē_A − ē_B de solo 250 ppm y x por debajo de los dos), no coinciden.
- **La cota crece con la propia deriva**, a través de s. Pasar o no pasar depende del comportamiento del reloj durante esos 14 s, no del firmware.

Las dos capturas quedan en el repositorio sin editar.

Estado de F0: **en curso**. `tim_f0_bench_89f11b5.txt` cumple R3-1, pero hay dos puntos pendientes del responsable:

1. El gate de tamaños de producción, que no se cumple desde `2481396`.
2. Si un criterio que una ejecución pasa y otra no, con la misma imagen y por el reloj de la placa, sirve para A18.

## F0: tercera ejecución con la revisión 3

El responsable pidió el 2026-10-02 repetir la ejecución otra vez. La captura es `docs/tim_f0_bench_781212b.txt`. Desde `2481396` solo cambió documentación, y la imagen se recompiló para que su `Rev` sea el de `HEAD`. Resultado: `Rev` 781212b e id `979fb6e3d6ccf243` en el host, CM7 y CM4, árbol limpio, lista de comprobación confirmada por Hector, **16/16, `FW VERDICT=PASS` y `HOST VERDICT=PASS`**.

| Ejecución | ē_A | ē_B | ē | s_ef | x ± u | \|ē − x\| / cota | Resultado |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `2481396` | +10 541,5 | +10 290,8 | +10 416,2 | 310,9 | +9 994,7 ± 15,4 | 421,4 / 237,7 | FAIL |
| `89f11b5` | +13 188,6 | +12 038,9 | +12 613,7 | 689,8 | +12 549,1 ± 13,1 | 64,6 / 518,8 | PASS |
| `781212b` | +13 619,1 | +13 365,3 | +13 492,2 | 356,0 | +13 485,0 ± 13,4 | 7,3 / 270,0 | PASS |

Todos los valores en ppm. Con la misma imagen, de las tres ejecuciones con el criterio de R3-1, dos pasan y una falla.

En `781212b` el reloj de la placa volvió a unos +13 500 ppm, como en las capturas del 2026-10-01 (`8c4ab93`: x = +13 571,2). La diferencia entre grupos dentro de la sesión (ē_A − ē_B = 254 ppm) queda dentro de la dispersión de una sola captura.

Estado de F0: **en curso**. Sigue pendiente de las dos decisiones del responsable de la sección anterior: el gate de tamaños de producción desde `2481396` y la repetibilidad de R3-1.

## F0: cuarta ejecución con la revisión 3

El responsable pidió el 2026-10-02 repetir la ejecución otra vez. La captura es `docs/tim_f0_bench_20e5b89.txt`. Desde `2481396` solo cambió documentación, y la imagen se recompiló para que su `Rev` sea el de `HEAD`. Resultado: `Rev` 20e5b89 e id `0ec167b9c37f97c4` en el host, CM7 y CM4, árbol limpio, lista de comprobación confirmada por Hector, **16/16, `FW VERDICT=PASS` y `HOST VERDICT=PASS`**.

| Ejecución | ē_A | ē_B | ē | s_ef | x ± u | \|ē − x\| / cota | Resultado |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `2481396` | +10 541,5 | +10 290,8 | +10 416,2 | 310,9 | +9 994,7 ± 15,4 | 421,4 / 237,7 | FAIL |
| `89f11b5` | +13 188,6 | +12 038,9 | +12 613,7 | 689,8 | +12 549,1 ± 13,1 | 64,6 / 518,8 | PASS |
| `781212b` | +13 619,1 | +13 365,3 | +13 492,2 | 356,0 | +13 485,0 ± 13,4 | 7,3 / 270,0 | PASS |
| `20e5b89` | +13 438,3 | +13 380,5 | +13 409,4 | 527,4 | +13 534,4 ± 12,4 | 125,0 / 397,3 | PASS |

Todos los valores en ppm. Con la misma imagen, de las cuatro ejecuciones con el criterio de R3-1, tres pasan y una falla. La que falla es la primera después de reconectar el USB del ST-LINK. En las dos últimas, x se mantiene en +13 485 y +13 534 y la diferencia entre grupos es pequeña (254 y 58 ppm). En esta, s = 527,4 ppm sin que ē_A y ē_B se separen: la dispersión está dentro de cada grupo.

Estado de F0: **en curso**. Siguen pendientes las dos decisiones del responsable: el gate de tamaños de producción desde `2481396` y la repetibilidad de R3-1.

## F0: quinta ejecución con la revisión 3

El responsable pidió el 2026-10-02 repetir la ejecución otra vez. La captura es `docs/tim_f0_bench_105edfb.txt`. Desde `2481396` solo cambió documentación, y la imagen se recompiló para que su `Rev` sea el de `HEAD`. Resultado: `Rev` 105edfb e id `ccf10a34cba8e061` en el host, CM7 y CM4, árbol limpio, lista de comprobación confirmada por Hector, **16/16, `FW VERDICT=PASS` y `HOST VERDICT=PASS`**.

| Ejecución | ē_A | ē_B | ē | s_ef | x ± u | ē − x | \|ē − x\| / cota | Resultado |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `2481396` | +10 541,5 | +10 290,8 | +10 416,2 | 310,9 | +9 994,7 ± 15,4 | +421,4 | 421,4 / 237,7 | FAIL |
| `89f11b5` | +13 188,6 | +12 038,9 | +12 613,7 | 689,8 | +12 549,1 ± 13,1 | +64,6 | 64,6 / 518,8 | PASS |
| `781212b` | +13 619,1 | +13 365,3 | +13 492,2 | 356,0 | +13 485,0 ± 13,4 | +7,3 | 7,3 / 270,0 | PASS |
| `20e5b89` | +13 438,3 | +13 380,5 | +13 409,4 | 527,4 | +13 534,4 ± 12,4 | −125,0 | 125,0 / 397,3 | PASS |
| `105edfb` | +13 303,5 | +13 152,6 | +13 228,0 | 315,1 | +13 019,8 ± 10,6 | +208,3 | 208,3 / 238,4 | PASS |

Todos los valores en ppm. Con la misma imagen, de las cinco ejecuciones con el criterio de R3-1, cuatro pasan y una falla. En `105edfb` el margen es pequeño: la diferencia es el 87 % de la cota.

Hechos, sin conclusión:

- **En cuatro de las cinco ejecuciones, ē queda por encima de x.** La media de ē − x es +115 ppm.
- **La cota de R3-1 solo cuenta s_ef/4 y u.** Con s_ef ≈ 310 ppm, como en `2481396` y en `105edfb`, eso da unos 80 ppm por σ. Las dos ejecuciones con esa s_ef quedaron a 5,3σ y a 2,6σ.
- **x varía de una ejecución a otra con una desviación de unos 230 ppm** en las tres últimas (+13 485, +13 534, +13 020), aunque u declara unos 12 ppm en cada una.

Estado de F0: **en curso**. Siguen pendientes las dos decisiones del responsable: el gate de tamaños de producción desde `2481396` y la repetibilidad de R3-1.

## F0: sexta ejecución con la revisión 3

El responsable pidió el 2026-10-02 repetir la ejecución otra vez. La captura es `docs/tim_f0_bench_aef5596.txt`. Desde `2481396` solo cambió documentación, y la imagen se recompiló para que su `Rev` sea el de `HEAD`. Resultado: `Rev` aef5596 e id `c5fcd1e36f7c489b` en el host, CM7 y CM4, árbol limpio, lista de comprobación confirmada por Hector, **16/16, `FW VERDICT=PASS` y `HOST VERDICT=PASS`**.

| Ejecución | ē_A | ē_B | ē | s_ef | x ± u | ē − x | \|ē − x\| / cota | Resultado |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `2481396` | +10 541,5 | +10 290,8 | +10 416,2 | 310,9 | +9 994,7 ± 15,4 | +421,4 | 421,4 / 237,7 | FAIL |
| `89f11b5` | +13 188,6 | +12 038,9 | +12 613,7 | 689,8 | +12 549,1 ± 13,1 | +64,6 | 64,6 / 518,8 | PASS |
| `781212b` | +13 619,1 | +13 365,3 | +13 492,2 | 356,0 | +13 485,0 ± 13,4 | +7,3 | 7,3 / 270,0 | PASS |
| `20e5b89` | +13 438,3 | +13 380,5 | +13 409,4 | 527,4 | +13 534,4 ± 12,4 | −125,0 | 125,0 / 397,3 | PASS |
| `105edfb` | +13 303,5 | +13 152,6 | +13 228,0 | 315,1 | +13 019,8 ± 10,6 | +208,3 | 208,3 / 238,4 | PASS |
| `aef5596` | +13 131,4 | +12 986,0 | +13 058,7 | 377,4 | +13 066,8 ± 13,6 | −8,1 | 8,1 / 285,9 | PASS |

Todos los valores en ppm. Con la misma imagen, de las seis ejecuciones con el criterio de R3-1, cinco pasan y una falla: la primera después de reconectar el USB del ST-LINK.

En las cinco que pasan, ē − x vale +64,6, +7,3, −125,0, +208,3 y −8,1 ppm: media +29 ppm y desviación típica unos 118 ppm. Las cotas de esas ejecuciones van de 238 a 519 ppm.

Estado de F0: **en curso**. Siguen pendientes las dos decisiones del responsable: el gate de tamaños de producción desde `2481396` y la repetibilidad de R3-1.
