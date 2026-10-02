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
