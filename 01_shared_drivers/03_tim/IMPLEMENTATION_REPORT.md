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

En las cinco que pasan, ē − x vale +64,6, +7,3, −125,0, +208,3 y −8,1 ppm: media +29,4 ppm y desviación típica 121,4 ppm (n − 1). Las cotas de esas ejecuciones van de 238 a 519 ppm.

Estado de F0: **en curso**. Siguen pendientes las dos decisiones del responsable: el gate de tamaños de producción desde `2481396` y la repetibilidad de R3-1.

## F0: séptima ejecución con la revisión 3

El responsable pidió el 2026-10-02 repetir la ejecución otra vez. La captura es `docs/tim_f0_bench_94abb65.txt`. Desde `2481396` solo cambió documentación, y la imagen se recompiló para que su `Rev` sea el de `HEAD`. Resultado: `Rev` 94abb65 e id `370f7db57d18e9e1` en el host, CM7 y CM4, árbol limpio, lista de comprobación confirmada por Hector, **16/16, `FW VERDICT=PASS` y `HOST VERDICT=PASS`**.

| Ejecución | ē_A | ē_B | ē | s_ef | x ± u | ē − x | \|ē − x\| / cota | Resultado |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `2481396` | +10 541,5 | +10 290,8 | +10 416,2 | 310,9 | +9 994,7 ± 15,4 | +421,4 | 421,4 / 237,7 | FAIL |
| `89f11b5` | +13 188,6 | +12 038,9 | +12 613,7 | 689,8 | +12 549,1 ± 13,1 | +64,6 | 64,6 / 518,8 | PASS |
| `781212b` | +13 619,1 | +13 365,3 | +13 492,2 | 356,0 | +13 485,0 ± 13,4 | +7,3 | 7,3 / 270,0 | PASS |
| `20e5b89` | +13 438,3 | +13 380,5 | +13 409,4 | 527,4 | +13 534,4 ± 12,4 | −125,0 | 125,0 / 397,3 | PASS |
| `105edfb` | +13 303,5 | +13 152,6 | +13 228,0 | 315,1 | +13 019,8 ± 10,6 | +208,3 | 208,3 / 238,4 | PASS |
| `aef5596` | +13 131,4 | +12 986,0 | +13 058,7 | 377,4 | +13 066,8 ± 13,6 | −8,1 | 8,1 / 285,9 | PASS |
| `94abb65` | +12 292,1 | +12 501,3 | +12 396,7 | 542,0 | +12 559,6 ± 13,4 | −162,9 | 162,9 / 408,5 | PASS |

Todos los valores en ppm. Con la misma imagen, de las siete ejecuciones con el criterio de R3-1, seis pasan y una falla: la primera después de reconectar el USB del ST-LINK.

En las seis que pasan, ē − x vale de media −2,6 ppm, con una desviación típica de 134,0 ppm (n − 1). La tendencia de ē − x a ser positiva que anotó la sección de la quinta ejecución (media +115 ppm en cinco ejecuciones, contando el fallo) no se confirma. x cambia entre ejecuciones de +12 549 a +13 534 ppm.

Estado de F0: **en curso**. Siguen pendientes las dos decisiones del responsable: el gate de tamaños de producción desde `2481396` y la repetibilidad de R3-1.

## F0: serie de 10 ejecuciones con la misma imagen (revisión 3)

El responsable pidió el 2026-10-02 "repetir la ejecución 10 más para terminar de verificar".

Una sola imagen, compilada en `cd0d613` y flasheada una vez, con 10 ejecuciones seguidas de `tim_hw_check.py`; cada una resetea la placa por SWD. El árbol estuvo limpio en todas. `Rev` cd0d613 coincide en el host, CM7 y CM4. Capturas: `docs/tim_f0_bench_cd0d613.txt` y de `_2` a `_10`.

| Ejecución | ē | x ± u | ē − x | Cota | \|ē − x\| / cota | Resultado |
| --- | --- | --- | --- | --- | --- | --- |
| `cd0d613` | +12 705,7 | +12 609,7 ± 14,1 | +96,0 | 367,4 | 0,26 | PASS |
| `cd0d613_2` | +12 550,4 | +12 471,7 ± 13,2 | +78,7 | 202,2 | 0,39 | PASS |
| `cd0d613_3` | +12 115,1 | +12 312,4 ± 11,6 | −197,3 | 303,8 | 0,65 | PASS |
| `cd0d613_4` | +12 872,2 | +13 032,2 ± 17,4 | −160,0 | 274,2 | 0,58 | PASS |
| `cd0d613_5` | +12 457,5 | +12 418,3 ± 15,5 | +39,3 | 278,5 | 0,14 | PASS |
| `cd0d613_6` | +12 943,8 | +13 143,9 ± 11,5 | −200,1 | 366,0 | 0,55 | PASS |
| `cd0d613_7` | +12 719,8 | +12 735,1 ± 16,6 | −15,3 | 324,8 | 0,05 | PASS |
| `cd0d613_8` | +13 040,0 | +12 729,5 ± 14,2 | +310,5 | 355,4 | 0,87 | PASS |
| `cd0d613_9` | +12 642,3 | +12 359,0 ± 11,7 | +283,3 | 244,7 | 1,16 | **FAIL** |
| `cd0d613_10` | +12 844,9 | +12 872,0 ± 19,1 | −27,1 | 409,3 | 0,07 | PASS |

Todos los valores en ppm. En las 10 ejecuciones, todo lo demás pasa: T0a, T0b(a)–(g), las capturas y el control de TREF y `FW VERDICT=PASS`. La ejecución 9 da 15/16 y `HOST VERDICT=FAIL`, solo por el criterio de TREF.

### Las 17 ejecuciones con el criterio de R3-1 (las 7 anteriores y estas 10)

- **2 de 17 fallan:** `2481396` y `cd0d613_9`. La segunda no fue justo después de reconectar el USB del ST-LINK, así que la explicación por calentamiento que sugerí antes no basta.
- **ē − x:** media +36,1 ppm y desviación típica 184,5 ppm (n − 1). En las 10 de esta serie: +20,8 y 181,0 ppm. Ya no hay sesgo.
- **El criterio está mal calibrado.** Si la cota de R3-1 fuera 3σ de la diferencia, z = (ē − x)/(cota/3) tendría un rms cercano a 1, y fallaría alrededor de 3 de cada 1 000 ejecuciones. El rms de z es **2,00** en las 17 ejecuciones (1,75 en esta serie), y el máximo de \|z\| es 5,32. Es decir, la cota corresponde a unos 1,5σ de la dispersión real, y con eso se espera que falle alrededor de 1 de cada 8 ejecuciones. Se observaron 2 de 17.
- **u no cubre la variación de x.** Cada ejecución declara u entre 11,5 y 19,1 ppm; la dispersión real de ē − x es unas 10 veces mayor.

Lo que no puedo separar con estos datos es cuánto de la dispersión de ē − x viene de x (latencia del VCP, que el ajuste no ve como ruido) y cuánto de ē (variación del reloj dentro de las ventanas de 40 µs más allá de lo que mide s). Tampoco propongo un criterio nuevo: es decisión del responsable.

Estado de F0: **en curso**. Con la misma imagen, el criterio de R3-1 falla en unas 2 de cada 17 ejecuciones por la medida de TREF, no por el firmware. Una ejecución del auditor (A18) tiene una probabilidad del orden del 10 % de fallar así. Siguen pendientes las dos decisiones del responsable: el criterio de TREF y el gate de tamaños de producción desde `2481396`.

## Respuesta a la auditoría 4 (F0; plan revisión 4)

Plan de referencia: revisión 4 (`336180b`). La auditoría 4 y su adenda (`AUDIT.md`) aceptan la revisión 3 y el cambio de SMPS (`2481396`) como desviación aprobada (R4-2). Para F0 faltaba R4-1.

**Estado de F0: entregada para auditoría** (`7ac9e44` código, evidencia en `1abf3d3`). Falta la ejecución del banco por el auditor (A18/A27).

### Cambios de la revisión 4

| Cambio | Commit | Qué ejecuté para comprobarlo | Estado |
| --- | --- | --- | --- |
| R4-1: la cota de TREF pasa a ser 3·√(u² + s_ef²/16 + σ_ser²), con σ_ser = 145 ppm como constante nombrada (`TREF_SIGMA_SER_PPM` en `tools/tim_hw_check.py`). Su comentario cita el plan R4-1, `AUDIT.md` auditoría 4 (D6) y las 17 capturas de la revisión 3, y la línea del criterio de la captura imprime σ_ser junto a u y s_ef. Las capturas, el ajuste de x, el firmware y \|ē\| ≤ 20 000 ppm no cambian | `7ac9e44` | Recalculé σ_ser con las 17 capturas: 145,1 ppm, rms de z 1,05 y máximo de \|z\| 2,55, como dice la auditoría. Fuera de línea, con s_ef = 330 y x = +13 571,2 ± 2,2: pasa con 28,8 ≤ 500,5 y falla con 528,8, sin x y con \|ē\| > 20 000. En la placa: pasa, ver abajo | aplicado en `7ac9e44`, pendiente de verificación |
| R4-2: la base de la producción de `prj_01` es `2481396` | — (no requiere código) | `.map` de `7ac9e44` frente a un worktree aislado de `2481396`: 0 en todas las secciones asignadas de CM7 y CM4, debug y release. CM7: 29304/132/55668 (debug) y 28356/132/55672 (release); CM4: 3456/24/12560 y 3112/24/12560 (`docs/tim_f0_map_sizes_7ac9e44.txt`) | igual a `2481396` |

Evidencia en `7ac9e44`:

- `build_all -Clean` 8/8 (`tim_f0_build_all_7ac9e44.txt`).
- `selftest-tim` con 0 avisos (`tim_f0_selftest_build_7ac9e44.txt`).
- Los tres builds con `-Werror`, también a 0 avisos (`tim_f0_werror_7ac9e44.txt`).
- `git diff e7542dc -- 02_firmware_projects` sale vacío.

### Resultado en la placa (`docs/tim_f0_bench_7ac9e44.txt`)

Una sola ejecución, como pide la revisión 4. `Rev` 7ac9e44 e id `e890fb1c2f192c33` en el host, CM7 y CM4. Árbol limpio y lista de comprobación confirmada por Hector. Resultado: **16/16, `FW VERDICT=PASS` y `HOST VERDICT=PASS`**.

| Magnitud | Valor |
| --- | --- |
| ē_A, ē_B, ē | +13 230,2, +12 713,2, +12 971,7 ppm |
| s, r̄, r̄/√6, s_ef | 578,2, 248,6, 101,5, 578,2 ppm |
| x, u (11 ráfagas, 9 g. l., residuos 0,274 ms) | +12 843,5 ± 14,0 ppm |
| σ_ser | 145,0 ppm |
| \|ē − x\| frente a la cota | 128,2 ≤ 3·√(u² + s_ef²/16 + σ_ser²) = 615,7 → PASS |

Con el criterio de R3-1, esta ejecución también habría pasado: la cota era 3·√(u² + s_ef²/16) = 435,7. No empiezo F1 hasta que `AUDIT.md` diga que F0 pasa.

## F0: serie de 10 ejecuciones con la misma imagen (revisión 4)

El responsable pidió el 2026-10-02 "repetir la ejecución del banco 10 veces para ver si TREF se repite". Esta serie no sustituye a la ejecución entregada (`tim_f0_bench_7ac9e44.txt`): se hizo a petición expresa del responsable y no cambia nada del código.

Una sola imagen, compilada en `2a78fe5` y flasheada una vez, con 10 ejecuciones seguidas de `tim_hw_check.py`; cada una resetea la placa por SWD. Entre `7ac9e44` y `2a78fe5` solo cambió documentación, y la imagen se recompiló para que su `Rev` sea el de `HEAD`. El árbol estuvo limpio en todas. `Rev` 2a78fe5 e id `844b5a8f2376c415` coinciden en el host, CM7 y CM4. Capturas: `docs/tim_f0_bench_2a78fe5.txt` y de `_2` a `_10`.

| Ejecución | ē | s_ef | x ± u | ē − x | Cota R4-1 | \|ē − x\| / cota | Resultado |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `2a78fe5` | +13 153,9 | 505,3 | +13 033,2 ± 15,5 | +120,7 | 578,8 | 0,21 | PASS |
| `2a78fe5_2` | +13 549,8 | 330,9 | +13 374,9 ± 11,1 | +174,9 | 501,9 | 0,35 | PASS |
| `2a78fe5_3` | +13 624,3 | 483,0 | +13 387,1 ± 13,7 | +237,2 | 567,6 | 0,42 | PASS |
| `2a78fe5_4` | +13 500,1 | 740,9 | +13 651,0 ± 25,7 | −150,9 | 709,9 | 0,21 | PASS |
| `2a78fe5_5` | +13 486,5 | 533,2 | +13 556,2 ± 18,7 | −69,7 | 593,5 | 0,12 | PASS |
| `2a78fe5_6` | +13 597,2 | 434,1 | +13 233,7 ± 16,4 | +363,5 | 545,6 | 0,67 | PASS |
| `2a78fe5_7` | +13 789,1 | 451,8 | +13 864,7 ± 12,5 | −75,6 | 552,7 | 0,14 | PASS |
| `2a78fe5_8` | +13 559,0 | 603,8 | +13 587,9 ± 13,9 | −28,9 | 629,3 | 0,05 | PASS |
| `2a78fe5_9` | +12 964,9 | 386,3 | +12 976,5 ± 12,9 | −11,6 | 524,1 | 0,02 | PASS |
| `2a78fe5_10` | +12 876,5 | 446,4 | +12 797,8 ± 11,1 | +78,7 | 549,9 | 0,14 | PASS |

Todos los valores en ppm. **Pasan las 10:** 16/16, `FW VERDICT=PASS` y `HOST VERDICT=PASS` en todas.

- **ē − x:** media +63,8 ppm y desviación típica 161,0 ppm (n − 1). En la revisión 3 fueron +36,1 y 184,5.
- **z = (ē − x)/(cota/3):** rms 0,89 y máximo de \|z\| 2,00 (`2a78fe5_6`). Con una cota de 3σ bien calibrada, el rms debería estar cerca de 1.
- **σ_ser medida solo con estas 10**, con la misma fórmula de R4-1: 106,3 ppm, por debajo de los 145 ppm de la constante. No la cambio: la fija el plan.
- **Con el criterio de R3-1**, `2a78fe5_6` habría fallado (363,5 > 329,3), y el rms de z habría sido 1,48.
- x va de +12 798 a +13 865 ppm entre ejecuciones, y ē de +12 877 a +13 789: el reloj de la placa sigue derivando en cientos de ppm.

Estado de F0: **entregada para auditoría**. Falta la ejecución del banco por el auditor (A18/A27). F1 no se ha empezado.


## F1 (tim_bm)

Plan de referencia: revisión 4 (`336180b`). F1 empieza sobre `708c212`, el commit en el que `AUDIT.md` (auditoría 6) dice que F0 pasa.

### Resumen

- `tim_bm` implementa el contrato completo de F1 en `01_shared_drivers/03_tim/tim_bm.{h,c}`. El target es `stm32h7_driver_tim_bm`, sin target sin sufijo y fuera del paraguas. `prj_01` lo enlaza solo con `TIM_DRIVER_SELFTEST`.
- El selftest cubre T1–T16 y B1 en los dos núcleos. CPU2 prueba TIM15 y su banco de RCC (T16) mediante comandos en el bloque SRAM4. La regresión de F0 (T0a, TREF\_A/TREF\_B con R4-1 y T0b) sigue en cada captura. El protocolo del banco pasa a v3, con MCO1 en los pasos de T6 y T8 (R3-2).
- La captura de entrega es `docs/tim_f1_bench_f86706c.txt`, de una sola ejecución: 109/109 comprobaciones del host, `FW VERDICT=PASS` (0 fallos) y `HOST VERDICT=PASS`.
- Hay también una captura anterior del mismo código salvo dos comentarios, `docs/tim_f1_bench_e26d289.txt`: 109/109. Se repitió para que la captura de entrega sea del commit entregado: `f86706c` corrige una cita del RM en dos comentarios (§5.5, fila 1).
- T8 no se juzga con el criterio del plan, sino con la corrección del retardo de reset que decidió el responsable el 2026-10-02 (§5.5, fila 1). El cambio está pendiente de la revisión del plan.
- Durante el desarrollo, el selftest encontró un defecto del driver: Deinit e InitTimeBase no borraban `CR1.DIR` al salir del modo center-aligned. Está corregido en `6ededca`.
- Sin bloqueos. F2 no ha empezado: espera a que `AUDIT.md` diga que F1 pasa.

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F0 | pasa (`AUDIT.md`, auditoría 6) | `cae5414` | `docs/tim_f0_audit_bench_cae5414.txt` (la del auditor) |
| F1 | entregada para auditoría (`f86706c`; evidencia en `9e7388c`) | el commit que añade esta sección | `03_firmware_templates/prj_01_template_dualcore/docs/tim_f1_bench_f86706c.txt` |
| F2 | sin empezar | — | — |
| F3 | sin empezar | — | — |

### 5.1 Commits

Rango de la entrega: `708c212..` el commit que añade esta sección.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `1f0fedc` | feat(tim): add tim_bm, the bare-metal timer tier | — |
| `3eb5514` | build(tim): add the stm32h7_driver_tim_bm target | — |
| `6ededca` | fix(tim): clear CR1.DIR when a timer leaves center-aligned mode | propio, encontrado por T14 y T6 en la ejecución de desarrollo 1 |
| `4f4c5c3` | feat(tools): add a Digital Out custom pattern to ad2.py | — (T8, filtro) |
| `2732f2c` | build(tim): link tim_bm into selftest-tim, and dma_bm on CPU1 | — |
| `1570fdb` | test(prj_01): bench protocol v3 and the F1 oracles typed from RM0399 | R3-2 |
| `84be1d4` | test(prj_01): the F1 tests of tim_bm on CPU1 and CPU2 | O1, R3-2 |
| `a446006` | test(prj_01): the host side of the F1 bench | R3-2 |
| `a7b1f9d` | docs(tim): add the seven F1 development runs (not evidence) | — |
| `e26d289` | docs(tim): name the README in tim_bm.h without the 03_tim/ path | — (A4) |
| `18b2ea9` | docs(tim): add the F1 evidence at e26d289 | — (sustituida por la de `f86706c`) |
| `f86706c` | test(prj_01): cite RM0399 40.3.26 for the reset delay of PWM input | — (cita del RM) |
| `9e7388c` | docs(tim): add the F1 evidence at f86706c, the delivered commit | — |

### 5.2 Build y tamaños

- `build_all -Clean` da 8/8: `docs/tim_f1_build_all_f86706c.txt`.
- `selftest-tim`, compilado desde un directorio vacío, da 0 avisos: `docs/tim_f1_selftest_build_f86706c.txt`.
- La compilación aparte con `CMAKE_C_FLAGS=-Werror` de `selftest-tim`, `debug` y `release` da 0 avisos en las tres: `docs/tim_f1_werror_f86706c.txt`.
- `git diff e7542dc -- 02_firmware_projects` sale vacío. `git diff e7542dc --stat` no toca nada fuera de `01_shared_drivers/03_tim`, `01_shared_drivers/CMakeLists.txt`, `03_firmware_templates/prj_01_template_dualcore` y `tools/ad2.py`.

Producción de `prj_01` frente a `2481396` (R4-2). Fuente: `docs/tim_f1_map_sizes_f86706c.txt`, `.map` enlazado sin "Discarded input sections".

| Imagen | .text | .data | .bss | Δ frente a 2481396 (R4-2) | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| debug CM7 | 29304 | 132 | 55668 | 0 en todas las secciones asignadas | `tim_f1_map_sizes_f86706c.txt` |
| debug CM4 | 3456 | 24 | 12560 | 0 | ídem |
| release CM7 | 28356 | 132 | 55672 | 0 | ídem |
| release CM4 | 3112 | 24 | 12560 | 0 | ídem |

Objetos de `selftest-tim`. La columna .text suma `.text` y `.rodata`, como `arm-none-eabi-size`.

| Objeto | Núcleo | .text | .data | .bss | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| `tim_bm.c.obj` | CM7 | 5992 | 0 | 0 | `tim_f1_map_sizes_f86706c.txt` |
| `tim_bm.c.obj` | CM4 | 4514 | 0 | 0 | ídem |
| `tim_selftest.c.obj` | CM7 | 40001 | 8 | 8874 | ídem |
| `tim_selftest_common.c.obj` | CM7 | 3662 | 0 | 956 | ídem |
| `tim_selftest.c.obj` | CM4 | 3591 | 0 | 296 | ídem |
| `tim_selftest_common.c.obj` | CM4 | 3636 | 0 | 956 | ídem |

`.data = .bss = 0` en `tim_bm.c.obj` en los dos núcleos (C7).

`tim_bm.c` también compila solo, fuera de `prj_01`, para CM7 y para CM4, con 0 diagnósticos. Para eso, los headers de CMSIS van como `-isystem`, con `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Werror`, como en la compilación del auditor (`AUDIT.md`, F1 parte 1). Fuente: `docs/tim_f1_isolated_build_b65fd03.txt`, variante C.

Corrección: esta nota decía antes "con `-Wall -Wextra -Wpedantic -Wconversion -Werror` (mensaje de `1f0fedc`)", y era incompleta:

- La compilación de `1f0fedc` no usó `-isystem`. Pasaba CMSIS como `-I` y llevaba además `-Wshadow` y `-Wno-sign-conversion` (variante A).
- Con `-I` y sin `-Wno-sign-conversion`, `-Wsign-conversion` salta en `cachel1_armv7.h` en CM7 (16 diagnósticos, todos de CMSIS y ninguno del driver; variante B).
- El mensaje de `1f0fedc` no nombra ese flag. No reescribo la historia: la corrección queda aquí.

### 5.3 Pruebas

Todos los valores salen de `docs/tim_f1_bench_f86706c.txt` (una ejecución, `Rev` `f86706c`, id `e3df266a1b52e8fc`). Es la captura de la tabla salvo que se indique otra.

| ID | Tier | Núcleo | Resultado | Valor medido | Control negativo: valor | Oráculo | Captura |
| --- | --- | --- | --- | --- | --- | --- | --- |
| T0a | banco | CM7 | PASS | 13 líneas: pull-up `0x1FFF` en el MCU y `0x4FFF` en el AD2 (DIO0–11 y DIO14), pull-down `0x0000` en los dos; walking-1 sin diferencias | DIO9 al revés: los dos lados marcan `0x0200` | `IDR`, DIO del AD2 | `tim_f1_bench_f86706c.txt` |
| TREF\_A/B | banco | CM7 | PASS | ē = +12 780,0 ppm (16 capturas, s\_ef 530,9); x = +13 083,5 ± 7,5 ppm; \|ē − x\| = 303,4 ≤ 590,1 (R4-1) | PA8 en AF1, TIM1 sin reloj: 0 flancos | DIO14 del AD2, reloj del host | ídem |
| T0b(a)(e) | host | — | PASS | mocks S3 y S11 (los de F0) | los controles de F0 pasan | mock de la DLL | ídem |
| T0b(b) | banco | CM7, CM4 | PASS | CM7: 20 rechazos, registros sin tocar; CM4: `accepted 0x0001`, `regs changed 0x000` | O1: PA8 a AF0 cambia exactamente `0x00021` (GPIOA MODER, AFR[1]); PE5 a AF4 en CM4, `0x440` (GPIOE MODER, AFR[0]) | fotos de GPIO | ídem |
| T0b(c)(d)(f)(g) | banco | CM7 | PASS | como en F0 | `0x5A` detectado; timeout; Idle=Low `0xFD`; 13 casos S12 rechazados | AD2, fotos | ídem |
| T1 | BM | CM7 | PASS | 39 rechazos, estado esperado en los 39, fotos (6 timers y los dos bancos de RCC) sin cambios | O1: `ConfigOutput(TIM2 CH1)` cambia exactamente CCMR1 y CCR1 (`0x0002040`); `EnableClock(TIM13)` solo `RCC_C1 APB1LENR` (`0x01`) | registros leídos en crudo | ídem |
| T1 | BM | CM4 | PASS | 10 rechazos de TIM15, estados esperados, foto sin cambios | O1: `ConfigOutput(TIM15 CH1)` cambia `0x0002040` (CCMR1, CCR1) | ídem | ídem |
| T2 | BM | CM7 | PASS | `GetCaps` de las 14 instancias = tabla a mano; 6008 imágenes de 12 instancias sin diferencias con la codificación a mano | una imagen esperada alterada (ARPE) se detecta en las 12 instancias | campos del RM a mano; HAL cruda | ídem |
| T2 HAL | — | CM7 | (lista) | 6008 configuraciones por la HAL cruda, 0 llamadas con error; difieren solo los bits OCxPE de CCMR1/CCMR2 en OC (17 entradas instancia × registro) | — | `HAL_TIM_*` | ídem; explicación en §5.5 |
| T2 | BM | CM4 | PASS | TIM15: 1313 imágenes, 0 diferencias | imagen alterada detectada | campos del RM a mano | ídem |
| T3 | puro | CM7 + host | PASS | 234 `CalcTimeBase`, 70 `DutyToCompare` y 34 `KernelClockFromFields` (32 filas de la tabla 59): 0 diferencias | el primer vector OK con psc + 1 como esperado se detecta | Python con `fractions` | ídem |
| T5(a) | BM | CM7 | PASS | 12 instancias, 1000 ticks: de −1 a +2 ppm | PSC + 2: −333 333 ppm aprox. en todas | `CYCCNT` (2 ciclos por tick) | ídem |
| T5(b) | BM | CM7 | PASS | RCR = 3 en TIM1, TIM8, TIM16 y TIM17: de −2 a +2 ppm | sin RCR (1000 ticks): +3 000 000 ppm aprox. | ídem | ídem |
| T5(c) | BM | CM7 | PASS | TIM2 +0 ppm y TIM5 +2 ppm con `ARR + 1 = 0x20000` | 16 bits (65 536 ticks): +1 000 000 ppm aprox. | ídem | ídem |
| T5(d) | BM | CM7 | PASS | down 0 ppm (TIM1 y TIM2); center 1: TIM1 (RCR = 1) 2000 ticks por update, −1 ppm; TIM2 1000 ticks por update (dos por periodo de 2 ARR), 0 ppm | fórmula de flanco (ARR + 1): −1997 ppm aprox. | ídem | ídem |
| T5(e) | BM | CM7, CM4 | PASS | `GetKernelClockHz` = 200 000 000 Hz en las 14 instancias (D2PPRE1 = D2PPRE2 = 4, `100` = /2; TIMPRE = 0) y en TIM15 desde CM4 | — | tabla 59 y `HAL_RCC_GetHCLKFreq` | ídem |
| T5 | BM | CM4 | PASS | TIM15 (a) +7 ppm, (b) −9 ppm | −333 328 y +2 999 960 ppm | `CYCCNT` de CM4 (1 ciclo por tick) | ídem |
| T6 | BM | CM7 | PASS | TIM1 CH1–CH4 × 9 puntos: 36/36 dentro de ±0,1 % sobre f/f\_MCO1 y de ±15 ns de tiempo en alto; 0 % y 100 % sin flancos y al nivel esperado; ningún flanco en las otras líneas | MOE = 0 con los 4 canales habilitados: 0 flancos, nivel 0 | LA con DIO14 en la misma adquisición (R3-2) | ídem |
| T6 | BM | CM4 | PASS | TIM15 CH1/CH2 × 8 puntos: 16/16 | CC1E = CC2E = 0: 0 flancos, nivel 0 | ídem | ídem |
| T7 | BM | CM7 | PASS | TIM13 (PF8), TIM14 (PF9), TIM16 (PF6), TIM17 (PF7) a 1 kHz y 25 %: periodo de −10 a +5 ppm, duty de 24,99 a 25,00 %, hueco máximo entre muestras de 231–248 ciclos | CC1E = 0: 0 flancos, nivel 0 | `IDR` + `CYCCNT` | ídem |
| T8 | BM | CM7 | PASS | 24/24 puntos (TI1/TI2 × 1 k/10 k/100 k/1 MHz × 25/50/75 %): cada captura dentro de ±(1 tick + 0,1 %) en periodo y ±(2 ticks + 0,1 %) en alto, con d = 1,94 ticks restado (§5.5, fila 1); prescaler 8 (1620–1622 frente a 1620,6); con filtro, 2073–2076 frente a 2075,9 | polaridad invertida: duty complementario (1516–1518 frente a 1517,4); CC1E = 0: CC1IF 0; sin filtro, el glitch se captura (518–519 y 1555–1556) | Digital Out y MCO1 en DIO14 en la misma generación (R3-2) | ídem |
| T8 | BM | CM7 | PASS | sobrecaptura: CC1OF = 1 sin leer CCR1 en 3 periodos | leyendo cada captura, CC1OF = 0 | `SR` crudo | ídem |
| T9 | BM | CM7 | PASS | TIM17 con `TI1SEL = 0011`: 16 periodos, 199..201 ticks (200 ± 1); el AD2 mide MCO1 a 1,0131 MHz en PA8 a la vez | `TI1SEL = 0000` (PF7 sin cablear, pull-down): CC1IF 0 en 1 ms | AD2 en DIO14 | ídem |
| T10(a) | BM | CM7 | PASS | TIM3 update → TIM2 ext clock 1 (ITR2): 1000 updates, CNT = 1000 | ITR1 (TIM8 parado): CNT = 0 | `CNT` del esclavo | ídem |
| T10(b) | BM | CM7 | PASS | TIM2 OC1REF → TIM4 gated (ITR1): CNT = 20 000 = N·CCR | ITR3 (TIM8 parado): CNT = 0 | ídem | ídem |
| T10(c) | BM | CM7 | PASS | TIM1 TRGO → TIM8 trigger (ITR0): CEN 0 → 1 tras el primer update | ITR3 (TIM5 parado): CEN 0 | `CR1` del esclavo | ídem |
| T10(d) | BM | CM7, CM4 | PASS | TRC de TIM2, TIM3, TIM4, TIM5, TIM8 (TIM1 por ITR0), TIM12 (TIM4 por ITR0) y TIM15 en CM4 (TIM1): diferencia 1000 (± 1) en las 7 | TIM2 con TIM1 parado: 0 capturas | `CCR1` del esclavo | ídem |
| T10(e) | BM | CM7 | PASS | TIM13 OC1 → TIM12 (ITR2) y TIM14 OC1 → TIM12 (ITR3), gated: CNT = 20 000 | (el control de (b) cubre el ITR parado) | `CNT` | ídem |
| T11 | BM | CM7 | PASS | 10 disparos → 10 pulsos; D (subida CH1 − subida CH2, referencia a CNT = 1) y W dentro de ±(1 tick + 1 muestra) = ±14,9 ns en los 10 | OPM = 0: 59 pulsos en la ventana | LA disparado en DIO1, tick a partir de MCO1 | ídem |
| T12(a) | BM | CM7 | PASS | 13 vectores (14 filas: TIM8 UP y TIM13 comparten el suyo) a 10 kHz durante 100 ms: 1000 en cada uno; TIM8 UP/CC, TIM12, TIM13 y TIM14 juntos, 1000 cada uno, sin cruces; 0 llamadas a ISR sin flag | UIE = 0: 0 IRQ | contadores de ISR + `CYCCNT` | ídem |
| T12(b) | BM | CM7, CM4 | PASS | 0 IRQ espurias tras InitTimeBase + EnableIrq sin Start (TIM2; TIM15 en CM4) | a mano (UG con URS = 0): 1 IRQ en los dos | ídem | ídem |
| T12(a) | BM | CM4 | PASS | TIM15: 1000 IRQ en 100 ms | UIE = 0: 0 | ídem | ídem |
| T12(c) | BM | CM7 | PASS | `GetIRQn`: 84 pares (instancia, evento), 0 diferencias | — | tabla a mano de `stm32h755xx.h` | ídem |
| T13(a) | BM | CM7 | PASS | TIM1 UP (petición 15) → DMA1 S0 → CCR1: los 16 tiempos en alto de 10 a 160 ticks, en secuencia, ±15 ns | UDE = 0: 40 periodos todos de 100 ticks | LA | ídem |
| T13(b) | BM | CM7 | PASS | TIM1 CC1 (petición 11) → DMA1 S1: 64 capturas, diferencias 202..203 frente a 202,58 ± 1 | CC1DE = 0: NDTR 64, centinela intacto en 64/64 | Digital Out + MCO1 | ídem |
| T13(c) | BM | CM7 | PASS | `GetDmaRequest`: 70 pares, 0 diferencias | — | `DMA_REQUEST_TIM*` de `stm32h7xx_hal_dma.h` | ídem |
| T14 | BM | CM7, CM4 | PASS | 101 Deinit en CM7 con su imagen de reset exacta; TIM15 en CM4 después de T5, T6, T10(d) y T12; S5 en verde al final de T6\_TIM1 y de T8 | TIM2 sin Deinit (PSC = 5, ARR = 1234): se detecta (PSC lee 5) | tabla de reset del RM a mano | ídem |
| T15 | BM | CM7 | PASS | 10⁶ × Enable/DisableIrq(TIM2, CC2) contra 42 446 ISR de TIM7 a 50 kHz: 0 pérdidas | RMW a mano sin PRIMASK, ventana de 24 ciclos: 12 688 ISR dentro de la ventana, 8893 pérdidas | estado propio de la ISR + `DIER` final | ídem |
| T16 | BM | CM7, CM4 | PASS | CM4 `DisableClock(TIM2)`: TIM2 sigue contando, solo cambia `RCC_C2`; CM4 Enable/Disable: bit de C2 1 → 0, sigue contando | CM7 `DisableClock(TIM2)`: TIM2 se para | `CNT` y `RCC_C1`/`RCC_C2` crudos | ídem |
| B1 | — | CM7, CM4 | registro | §5.4 | — | `.map` y DWT | ídem |

T2 abarca, por instancia: base de tiempo (todos los modos de cuenta × ARPE × OPM × URS), OC (8 modos × 2 polaridades × preload), IC (3 selecciones × 3 flancos × 4 prescalers × filtros 0/7/15 × TISEL válidos), MMS (y MSM donde hay SMCR) y modo esclavo (todos los SMS × todos los ITR cableados, TI1F\_ED, TI1FP1 y TI2FP2). Las cuentas por instancia están en la captura (líneas `T2 TIMx: ...`).

### 5.4 Medidas (B1)

Ciclos medidos con DWT y PRIMASK en `docs/tim_f1_bench_f86706c.txt`:

| Función | CM7 (ciclos a 400 MHz) | CM4 (ciclos a 200 MHz, TIM15) |
| --- | --- | --- |
| `TIM_BM_TakeFlags` | 152 | 280 |
| `TIM_BM_OutputEnable` | 348 | 489 |
| `TIM_BM_ConfigOutput` | 442 | 514 |
| `TIM_BM_InitTimeBase` | 404 | 511 |

- **Peor caso de `CalcTimeBase` en el barrido de T3:** 1101 ciclos en CM7.
- **Resolución de TREF:** una captura del LA de 4096 muestras a 100 MS/s abarca 40–41 periodos de MCO1, es decir, unos 250 ppm (las líneas `TREF_x capture i: r_i`). Desde R3-2 no se usa para corregir ninguna medida.
- **Pila de Status\_Task:** quedan libres 238 palabras de 512.

### 5.5 Desviaciones del plan

| # | Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- | --- |
| 1 | T8: periodo en ticks = 200·f\_MCO1/f ± (1 tick + 0,1 %); tiempo en alto ± 2 ticks | Sigue el modo PWM input. Al empezar el paso T8, sobre una sola generación de 1 MHz en PE9, se mide el retardo de reset del modo esclavo: d = media de 16 diferencias de captura directa − media de 16 periodos en PWM input. d se resta del periodo y del alto esperados, y el alto se juzga con ±(2 ticks + 0,1 %). En la entrega, d = 1,94 ticks | En PWM input, CCR1 y CCR2 leen unos 2 ticks menos que lo que da el AD2 en todas las frecuencias (1 MHz: 200–201 frente a 202,5; ejecuciones de desarrollo 1–3). Las capturas directas de la misma señal no tienen ese desfase (DMA a 1 MHz: 202–203; ICPSC 8: 1620–1621 frente a 1620,8). RM0399 40.3.26, "Slave mode: Reset mode" (`40_*.txt` l. 2640), describe el retardo ("due to the resynchronization circuit on TI1 input"), pero no da su valor. Mi pregunta al responsable y los mensajes de `84be1d4` y `a446006` lo citaban mal, como 40.4.3; `f86706c` corrige los comentarios. Además, ±2 ticks a 1–10 kHz (12 650–37 960 ticks) es más fino que los unos 250 ppm con los que se mide MCO1 en una ventana de 41 µs | Sí: responsable del catálogo (Hector), en el chat de esta sesión, el 2026-10-02 (opción "Correct the reset delay"). Está pendiente de que el plan lo recoja en una revisión |
| 2 | T8: el plan no fija cómo leer las capturas ni MCO1 | Se descartan dos capturas tras `Start`, no una; MCO1 se mide antes y después de las capturas del timer en la misma generación y se usa la media; el contador arranca antes de habilitar la captura | En el desarrollo, la primera captura útil tras `Start` salió hasta 9 ticks corta en periodo y alto a la vez (ejecución 3: 192/91), y una captura tomada con CEN = 0 guardaba el CNT parado (ejecución 4: T13(b) 152; ejecución 6: 155). Una sola ventana de MCO1 antes de unos 10 ms de capturas a 1 kHz dejaba hasta −0,15 % (ejecución 5). El criterio no cambia | No: es el procedimiento de medida |
| 3 | Pasos: el plan nombra T6, T8, T9, T11 y T13 sin decir qué paso lleva cada uno; `RCC_CFGR` solo en TREF, T9 y los pasos de T6 y T8 | Cuatro pasos nuevos: `T6_TIM1` (T6, T11 y T13a), `T6_TIM15`, `T8` (T8 y T13b) y `T9`. T11 y T13 usan el tick de MCO1 del paso en el que corren | Así ningún otro paso escribe `RCC_CFGR` (instrucciones §3.5), y T11 y T13 miden también en ticks de la placa | No |
| 4 | T9: "con la frecuencia del AD2 dentro de la tolerancia común" | El host comprueba que MCO1 está en DIO14 durante la captura de TIM17 (≥ 40 periodos) y que \|e\| ≤ 20 000 ppm (la cota absoluta de TREF). El criterio de TIM17 es 200 ± 1 ticks | La tolerancia común es ±0,1 % sobre el cociente con MCO1 en la misma adquisición. En T9 la señal medida es MCO1, así que el cociente vale 1 por construcción; frente al valor nominal, la placa está a +1,3 % | No |
| 5 | T7: "IDR muestreado en bucle con PRIMASK" | PRIMASK en cada muestra (lectura de `CYCCNT` e `IDR` juntas), no en los 12 ms de la ventana | Con PRIMASK durante 12 ms, TIM6 (el tick de la HAL) perdería ticks y estropearía la verificación cruzada de TREF. Se registra el hueco máximo entre muestras: 231–248 ciclos | No |
| 6 | T10(b)(e): "tras N periodos, CNT = N·CCR" | N periodos contados desde el primer update del maestro | OC1REF no se pone a 1 hasta el primer update: contando desde el arranque, la ejecución 1 dio (N − 1)·CCR = 19 800 | No |
| 7 | T5(d): "center da un periodo de 2·ARR ticks" | TIM1 con RCR = 1 (un update por periodo de 2·ARR); TIM2, sin RCR, da un update cada ARR (dos por periodo) | El update del modo center llega en el overflow y en el underflow | No |
| 8 | Contrato: solo las funciones listadas | Además, la macro pública `TIM_BM_CH(n)` (bit n − 1 = canal n) para los `ch_mask` | El contrato usa `ch_mask` sin definir su codificación. No es una función | No: pregunto aquí si se acepta antes de congelar la API |
| 9 | Comportamientos que el plan no detalla | `DutyToCompare`: `ERR_PARAM` si `counter_bits` no es 16/32 o `arr` supera el contador. `EnableDmaRequest`: `ERR_UNSUPPORTED` donde la tabla 126 no tiene petición (TIM4 CC4, TIM15 CC2, TIM12–14). `OutputEnable`/`CaptureEnable`: `ERR_PARAM` sobre un canal de la otra dirección. `ConfigSlave`: `ERR_BUSY` con SMS ≠ 0, salvo para pedir `SMS_DISABLED`. ES0445 2.16.1 también se rechaza en `InitTimeBase` con OPM, la tercera vía a la combinación. `Start` no hace read-back (en OPM, CEN baja solo). `Deinit` no genera UG | C6/C12; la combinación de 2.16.1 se puede formar por las tres llamadas; un UG con MMS = reset sería un pulso TRGO hacia los esclavos | No |
| 10 | T1: la lista de casos del plan | Están todos. `repetition > 0xFF` en TIM15 se prueba en CM4 (TIM15 es de CPU2), y en CM7 con TIM17. ETRF se prueba de dos formas: valor de driver inexistente (19, `ERR_PARAM`) y el código TS 7 de ETRF, que en el driver es ITR7, no cableado en TIM4 (`ERR_UNSUPPORTED`). Hay 21 casos más (NULL, canales, capacidades, `ERR_BUSY` de TS) | Propiedad de las instancias | No |
| 11 | T2: "la imagen que deja la HAL cruda" | URS con `__HAL_TIM_URS_ENABLE`, OPM con `HAL_TIM_OnePulse_Init` y, en los modos OC que no son PWM, el preload con `__HAL_TIM_ENABLE_OCxPRELOAD`. La única diferencia que queda es OCxPE en PWM con `preload = false`: la HAL la pone siempre (`stm32h7xx_hal_tim.c` l. 4294, 4311, 4328, 4345, `HAL_TIM_PWM_ConfigChannel`) | Para comparar la misma intención | No |
| 12 | Captura `tim_<fase>_<prueba>_<hash>.txt` | `docs/tim_f1_bench_<hash>.txt` | Una captura por ejecución, como en F0 | No |

### 5.6 Puntos de las auditorías de F0 que pasan a F1

| Punto | Commit | Qué ejecuté para comprobarlo | Estado |
| --- | --- | --- | --- |
| R3-2: en T6 y T8, cada frecuencia del AD2 frente a MCO1 en la misma adquisición; PA8 en AF0 con MCO1 = HSE/8 en esos pasos; tabla de pasos y reglas de línea ampliadas; `TIM_BENCH_VERSION` = 3 igual en el firmware y en `STEPS`; `RCC_CFGR` guardado y restaurado con read-back | `1570fdb`, `84be1d4`, `a446006` | Captura: los cuatro pasos imprimen los campos MCO1 `0x00A00000` y `RCC_CFGR restored: before 0x0000001B after 0x0000001B`; T6 juzga f/f\_MCO1 y T8 200·f\_MCO1/f, con DIO14 en la misma adquisición; banner v3 en los dos lados | aplicado en `84be1d4`, pendiente de verificación |
| O1: control de sensibilidad en las fotos de T1 y T0b(b), con la máscara impresa | `84be1d4` | T0b(b) CM7 `0x00021` (GPIOA MODER, AFR[1]), CM4 `0x440`; T1 CM7 `0x0002040` y `RCC 0x01`, T1 CM4 `0x0002040` | aplicado en `84be1d4`, pendiente de verificación |
| Regresión de F0 en cada captura | — | T0a, TREF\_A/TREF\_B (R4-1) y T0b(a)–(g) en verde en la captura de entrega | — |
| Producción igual a `2481396` (R4-2) | — | `.map` a 0 de diferencia (§5.2) | — |

### 5.7 Erratas del plan encontradas

| Sección del plan | Qué dice | Qué es correcto | Fuente |
| --- | --- | --- | --- |
| Regla 2, tabla de capacidades, TIM1/TIM8 | `TISEL` ≠ 0 válidos: "ninguno (§40.4.30–31)" | TIM1 `TI1SEL = 0001` es COMP1 output; TIM8 `TI1SEL = 0001` es COMP2 output. TI2–TI4SEL solo 0000. El driver sigue el RM: `tisel_mask[0] = 0x3` | RM0399 40.4.30 (`40_*.txt` l. 4769–4796) y 40.4.31 (l. 4799–4828) |
| Regla 3 (InitTimeBase) y regla 11 (Deinit) | Una escritura de CR1 con el modo, o `CR1.CEN = 0` y después los valores de reset | `DIR` es de solo lectura mientras `CMS ≠ 00`. Al salir del modo center hace falta una escritura que borre CMS antes de la que fija o borra DIR (`6ededca`) | RM0399 40.4.1 (`40_*.txt` l. 2889 y siguientes), nota de DIR |

### 5.8 Problemas abiertos y no ejercitado

- **TREF tuvo poco margen en la captura de `e26d289`:** \|ē − x\| = 556,1 frente a una cota de 589,9 (94 %), con x = +13 410 y ē = +12 854. En la de entrega, `f86706c`, es 303,4 frente a 590,1. La sesión de F1 dura 30 s, frente a unos 18 s en F0. No cambié nada de TREF.
- **T8 a 1 MHz:** las capturas en PWM input toman tres valores (200, 201 y, a veces, 202) y la tolerancia de periodo es ±1,2 ticks. En la ejecución de desarrollo 5, una captura de 202 quedó a +1,7 del esperado. En la entrega pasan las 24.
- **Arranque del modo esclavo reset:** la primera captura útil tras `Start` salió hasta 9 ticks corta en una ejecución de desarrollo. La causa no está en el RM. El selftest descarta dos capturas (§5.5, fila 2).
- **No ejercitado, según el plan:**
  - TRGO → ADC/DAC (D2), TRGO2 y los canales 5–6;
  - la captura desde pin en TIM2–5, TIM8, TIM12, TIM15 (que capturan TRC en T10(d)), TIM13, TIM14 y TIM16;
  - la salida PWM en pin de TIM2–5, TIM8 y TIM12;
  - `tim_hal` en CM4.
- **OCxREF por TRGO + gated:** T10(b) lo ejercita solo en TIM2 → TIM4, más TIM13/TIM14 → TIM12 en T10(e). El OCxREF de TIM3, TIM4, TIM5 y TIM8 no tiene una medida propia: solo su imagen de registros (T2).
- **TIM6:** el driver lo soporta, pero las pruebas nunca lo pasan a una función que escriba. Solo `GetKernelClockHz`, `GetIRQn`, `GetDmaRequest` y `GetCaps` lo leen, y `git grep TIM6` en los dos `tim_selftest.c` no da ninguna línea que escriba en él.
- **Errata ES0445 2.16.2–2.16.4:** quedan para el README de cierre, sin workaround en el driver.

## Decisiones abiertas usadas (F1)

| # | Opción usada | Consultada |
| --- | --- | --- |
| 1 | `TimBm_Status_t` | No (por defecto) |
| 2 | El alcance de la tabla "Alcance" | No (por defecto) |
| 3 | El PSC más pequeño que cabe; la búsqueda de error mínimo global solo se registra en T3 como información | No (por defecto) |
| 4 | TIM6 de `prj_01` no se migra | No (por defecto) |
| 5 | `tim_hal` en CM4 no se ejercita (F3) | No (por defecto) |
| 6 | Los documentos de proceso se borran al cierre | No (por defecto) |

## Tabla de capacidades (F1)

La del driver es `k_tim[]` en `tim_bm.c`, con la cita en cada fila. La de la prueba es `tst_hand_caps()` en `Common/Src/tim_selftest_common.c`, tecleada aparte; T2 las compara para las 14 instancias. Las líneas son de los `.txt` de `00_docs_stm32h7/docs/peripherals/timers_pwm/` (40 a 44) y de `dma_mdma/18_*.txt` (tabla 126). Se marca **(≠ plan)** la celda que difiere del plan.

| Instancia | Bits | Canales | Up/down/center | RCR | BDTR | Slave | MMS válidos | ITR válidos | TISEL ≠ 0 válidos | DMAMUX1 (UP; CC1–CC4) | Fuente (sección y línea) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| TIM1 | 16 | 4 | sí | 16 bits | sí | sí | 0–7 | 0–3 (TIM15, TIM2, TIM3, TIM4) | **TI1: 1 (COMP1) (≠ plan)** | 15; 11–14 | 40.4.1 l. 2889; tabla 353 l. 3214; 40.4.2 l. 3050; 40.4.15 l. 3970; 40.4.20 l. 4088; 40.4.30 l. 4769; tabla 126 l. 76 |
| TIM8 | 16 | 4 | sí | 16 bits | sí | sí | 0–7 | 0–3 (TIM1, TIM2, TIM4, TIM5) | **TI1: 1 (COMP2) (≠ plan)** | 51; 47–50 | tabla 353 l. 3214; 40.4.31 l. 4799; tabla 126 |
| TIM2 | 32 | 4 | sí | — | — | sí | 0–7 | 0–6 (TIM1, TIM8, TIM3, TIM4, ETH PPS, OTG HS SOF, OTG FS SOF) | TI4: 1–3 | 22; 18–21 | `41_*.txt` l. 21 (32 bits); 41.4.1 l. 2356; tabla 358 l. 2604; 41.4.2 l. 2448; 41.4.26 l. 3464 |
| TIM3 | 16 | 4 | sí | — | — | sí | 0–7 | 0–4 (TIM1, TIM2, TIM15, TIM4, ETH PPS) | TI1: 1–3 | 27; 23–26 | tabla 358; 41.4.27 l. 3501 |
| TIM4 | 16 | 4 | sí | — | — | sí | 0–7 | 0–3 (TIM1, TIM2, TIM3, TIM8) | ninguno | 32; 29–31, CC4 sin petición | tabla 358; 41.4.28 l. 3541; tabla 126 |
| TIM5 | 32 | 4 | sí | — | — | sí | 0–7 | 0–3, 6–8 (fdcan1\_soc, OTG HS SOF, OTG FS SOF) | TI1: 1–2 (fdcan1\_tmp, \_rtp) | 59; 55–58 | `41_*.txt` l. 21; tabla 358; 41.4.29 l. 3577 |
| TIM6, TIM7 | 16 | 0 | solo up | — | — | — | 0–2 | — | — | 69 / 70 | 44.4.1 l. 411; 44.4.2 l. 476; 44.4.3 l. 496 |
| TIM12 | 16 | 2 | solo up | — | — | sí | 0–5 | 0–3 (TIM4, TIM5, TIM13 OC1, TIM14 OC1) | TI1: 1 (spdifrx) | ninguna | 42.4.1 l. 1264; tabla 361 l. 1426; 42.4.2 l. 1337; 42.4.15 l. 1884 |
| TIM13, TIM14 | 16 | 1 | solo up | — | — | — | sin TRGO (no hay CR2) | — | ninguno | ninguna | 42.5.1 l. 2382; 42.5.12 l. 2789; 42.5.13 l. 2805 |
| TIM15 | 16 | 2 | solo up | 8 bits | sí | sí | 0–5 | 0–3 (TIM1, TIM3, TIM16 OC1, TIM17 OC1) | TI1: 1–6; TI2: 1–3 | 106; 105, CC2 sin petición | 43.5.1 l. 1811; tabla 366 l. 2004; 43.5.2 l. 1899; 43.5.13 l. 2531; 43.5.16 l. 2594; 43.5.20 l. 2830 |
| TIM16 | 16 | 1 | solo up | 8 bits | sí | — | sin TRGO (CR2 sin MMS) | — | TI1: 1–3 (LSI, LSE, WKUP\_IT) | 110; 109 | 43.6.2 l. 3605; 43.6.12 l. 4072; 43.6.14 l. 4112; 43.6.18 l. 4357 |
| TIM17 | 16 | 1 | solo up | 8 bits | sí | — | sin TRGO (CR2 sin MMS) | — | TI1: 2 (HSE\_1MHz), 3 (MCO1); 0001 reservado | 112; 111 | 43.6.20 l. 4450 |

Vectores de interrupción (`stm32h755xx.h`):

- TIM1: UP / CC / TRG\_COM.
- TIM8: UP\_TIM13 / CC / TRG\_COM\_TIM14.
- TIM12: TIM8\_BRK\_TIM12.
- TIM13: TIM8\_UP\_TIM13.
- TIM14: TIM8\_TRG\_COM\_TIM14.
- TIM6: TIM6\_DAC.
- El resto: su vector global.

El evento TRIGGER solo existe con controlador esclavo (TIE en 41.4.4 l. 2625, 42.4.4 l. 1432 y 43.5.4 l. 2010). TIM13/14 tienen solo CC1IE/UIE (42.5.2 l. 2439), TIM16/17 no tienen TIE (43.6.3 l. 3646) y TIM6/7 tienen solo UDE/UIE (44.4.3 l. 496).

Estado de F1: **entregada para auditoría** (`f86706c`; evidencia en `9e7388c`).

## Respuesta a la auditoría de F1 (partes 1 y 2; plan revisión 6)

Plan de referencia: revisión 6 (`294e049`). La parte 1 de la auditoría de F1 (`a9f8c7c`) no tiene hallazgos bloqueantes; su adenda aprueba D7, que es la revisión 5 (R5-1 a R5-4, sin código). La parte 2 (`faeb41e`) da 108/109 en la ejecución del auditor en `70e0181`, por `T8 TI1 1000000 Hz 25 %`: una captura de periodo de 202 frente a 200,6 ± 1,2. Su adenda aprueba D8, que es R6-1 en la revisión 6.

**Estado de F1: entregada para auditoría** (`b65fd03` código, evidencia en `3854edd`). Falta la ejecución del banco por el auditor (A18/A27).

### Cambios de la revisión 6

| Cambio | Commit | Qué ejecuté para comprobarlo | Estado |
| --- | --- | --- | --- |
| R6-1: en los puntos de T8 en PWM input (el bucle de `T8_POINTS`), la tolerancia de periodo pasa a `ptol = 2 + 0.001 * per`, igual que la del tiempo en alto. Cambia una línea de `Session.t8()` en `tools/tim_hw_check.py`. La línea de cada punto sigue imprimiendo la tolerancia usada (`+-2.2` a 1 MHz y `+-4.0` a 100 kHz, antes `+-1.2` y `+-3.0`). Las capturas directas mantienen `1 + 0.001 * periodo`: input prescaler 8 (`tol8`), filtro `ICxF` y T13(b). Tampoco cambian el control de polaridad invertida (juzga solo el tiempo en bajo, ya con ±(2 ticks + 0,1 %)), la corrección d de R5-1 (0 ≤ d ≤ 4), los demás criterios ni el firmware | `b65fd03` | `git diff 294e049 b65fd03` cambia solo esa línea. Ejecución del banco: ver abajo | aplicado en `b65fd03`, pendiente de verificación |
| Nota de §5.2 sobre la compilación aislada de `tim_bm.c` (`AUDIT.md`, F1 parte 1) | `3854edd` (evidencia) y el commit que añade esta sección | Tres variantes, en CM7 y CM4 (`docs/tim_f1_isolated_build_b65fd03.txt`):<br>• A, el comando de `1f0fedc`: `-I` con `-Wno-sign-conversion`, 0 diagnósticos.<br>• B, el conjunto completo con CMSIS como `-I`: CM7 falla por `-Wsign-conversion` en `cachel1_armv7.h`.<br>• C, el conjunto completo con CMSIS como `-isystem`: 0 diagnósticos en los dos núcleos | corregida en §5.2. La compilación de `1f0fedc` usaba `-I`, no `-isystem`; la nota dice ahora qué se ejecutó en cada caso |

Evidencia en `b65fd03`, en `03_firmware_templates/prj_01_template_dualcore/docs/`:

- `build_all -Clean`: 8/8 (`tim_f1_build_all_b65fd03.txt`).
- `selftest-tim`, compilado desde un directorio vacío: 0 avisos (`tim_f1_selftest_build_b65fd03.txt`).
- Los tres builds con `-Werror`: 0 avisos (`tim_f1_werror_b65fd03.txt`).
- `.map`: la producción de `prj_01` es igual a `2481396` en todas las secciones asignadas de CM7 y CM4, en debug y en release (`tim_f1_map_sizes_b65fd03.txt`). Los objetos de `selftest-tim` son iguales a los de `f86706c`.
- `git diff e7542dc -- 02_firmware_projects` sale vacío.

### Resultado en la placa (`docs/tim_f1_bench_b65fd03.txt`)

Una sola ejecución, sin repetir.

- **Trazabilidad:** `Rev` `b65fd03` e id `9a2a6f192c9ecbd8` iguales en el host, CM7 y CM4. Árbol limpio, protocolo v3 y lista de comprobación física confirmada por Hector.
- **Resultado:** **109/109 comprobaciones del host, 0 líneas FAIL, `FW VERDICT=PASS` (0 fallos) y `HOST VERDICT=PASS`.**

| Magnitud | Valor |
| --- | --- |
| T8, retardo de reset d (R5-1) | 2,31 ticks (0 ≤ d ≤ 4: PASS) |
| T8, 24 puntos en PWM input | 24/24. Peor residuo de periodo frente a la nueva cota: 0,7 de 2,2 a 1 MHz (32 %), 1,6 de 4,0 a 100 kHz (40 %), 16,3 de 22,3 a 10 kHz (73 %) y 31,9 de 52,7 a 1 kHz (61 %). Peor residuo del tiempo en alto: 1,4 de 2,1 a 1 MHz (67 %) |
| T8, capturas directas, con ±(1 tick + 0,1 %) | input prescaler 8: 1619–1621 frente a 1620,7 ± 2,6. Filtro: 2073–2075 frente a 2075,1 ± 3,1. T13(b): 202..203 frente a 202,63 ± 1 |
| T8, control de polaridad invertida | 1517–1520 frente a 1516,8 ± 3,5 |
| TREF (R4-1) | ē = +13 104,2 ppm (s\_ef 549,9); x = +13 467,0 ± 15,7 ppm; \|ē − x\| = 362,8 ≤ 601,3 |

En esta ejecución, el peor residuo de periodo cabe también en la tolerancia anterior, ±(1 tick + 0,1 %): 0,7 frente a 1,2 a 1 MHz y 1,6 frente a 3,0 a 100 kHz. El fallo del auditor fue una captura de 202 en `70e0181`; aquí, a 1 MHz, todas las capturas en PWM input son 200 o 201. La captura no muestra el caso que R6-1 cubre: solo muestra que el cambio no rompe nada.

No empiezo F2 hasta que `AUDIT.md` diga que F1 pasa.

## F2 (tim_ll)

Plan de referencia: revisión 7 (`9611ada`). F2 empieza sobre `ec0a6bc`, con la revisión 6. `AUDIT.md`, F1 parte 4 (`d1bdcca`), dice que F1 pasa y congela la API de `tim_bm` en `tim_bm.h` de `3174fef`.

### Resumen

- `tim_ll` implementa el contrato de F2 en `01_shared_drivers/03_tim/tim_ll.{h,c}`: las operaciones de `tim_bm` (`TIM_LL_*`) con las constantes de `stm32h7xx_ll_tim.h`.
- Valida llamando a `tim_bm`. Para eso, `tim_bm.h` gana diez funciones de solo validación, `TIM_BM_Check*` (R7-2). Hay una sola copia de los chequeos y de la tabla de capacidades.
- El target es `stm32h7_driver_tim_ll`, que enlaza `_tim_bm`, sin target sin sufijo y fuera del paraguas. `prj_01` lo enlaza solo con `TIM_DRIVER_SELFTEST`.
- Las pruebas LL corren en los dos núcleos: T1, T2 (con la paridad bit a bit contra `tim_bm`), T5, T6, T8, T10(a)(b), T12 y T14. El protocolo del banco pasa a v4, con tres pasos nuevos: `T6_TIM1_LL`, `T6_TIM15_LL` y `T8_LL` (R7-4).
- **Historia de la fase:**
  - La fase se paró el 2026-10-02 en un caso de §7 (`b0f1706`): T8 y T13(b) fallaban en 3 de 7 sesiones por la deriva de MCO1 entre ventanas.
  - La parte 1 de la auditoría de F2 (`0853663`) no encontró defectos y propuso D9.
  - El responsable aprobó D9 como revisión 7. R7-1 es la tolerancia de 0,5 %, aplicada en `b63204a`.
- **La captura de evidencia es `docs/tim_f2_bench_b63204a.txt`.** Una sola ejecución, sin repetir: 192/192 comprobaciones del host, `FW VERDICT=PASS` (0 fallos) y `HOST VERDICT=PASS`.
- F3 no ha empezado: espera a que `AUDIT.md` diga que F2 pasa.

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F0 | pasa (`AUDIT.md`, auditoría 6) | `cae5414` | `docs/tim_f0_audit_bench_cae5414.txt` |
| F1 | pasa (`AUDIT.md`, F1 parte 4) | `3174fef` | `docs/tim_f1_audit_bench_3174fef.txt` |
| F2 | entregada para auditoría (`b63204a`; evidencia en `fe8a07c`) | el commit que actualiza esta sección | `03_firmware_templates/prj_01_template_dualcore/docs/tim_f2_bench_b63204a.txt` |
| F3 | sin empezar | — | — |

### 5.1 Commits

Rango de la entrega: `ec0a6bc..` el commit que actualiza esta sección. Los commits de `0853663` y `9611ada` son del auditor y del responsable.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `916a3d4` | feat(tim): add TIM_BM_Check*, the validation-only entry points | — (decisión del responsable; R7-2) |
| `bb7de38` | feat(tim): add tim_ll, the LL timer tier | — |
| `3c47a3c` | build(tim): add the stm32h7_driver_tim_ll target | — |
| `95fa214` | build(tim): link tim_ll into selftest-tim on CPU1 and CPU2 | — |
| `3652bdf` | fix(tim): one capability-table lookup per tim_bm operation again | propio: `916a3d4` buscaba la instancia dos veces por llamada (B1, §5.4) |
| `5c4fe1c` | test(prj_01): the F2 tests of tim_ll on CPU1 and CPU2, bench protocol v4 | — |
| `a1c537b` | test(prj_01): T9 polls TakeFlags under PRIMASK for its 16 captures | propio: T9 de F1 perdía una captura (§5.5 fila 8; R7-4) |
| `2d1e525` | test(prj_01): the host side of the F2 bench, protocol v4 | — |
| `6d71ece` | docs(tim): add the seven F2 development runs (not evidence) | — |
| `10c808e` | docs(tim): add the F2 build evidence at 2d1e525 | — |
| `b0f1706` | docs(tim): add the F2 section of the report (stopped, not delivered) | — (§7) |
| `b63204a` | test(prj_01): R7-1, 0.5 % relative term in T8 and T13(b) | R7-1 |
| `fe8a07c` | docs(tim): add the F2 evidence at b63204a (R7-1) | — |

Errata del mensaje de `bb7de38`: dice "Two whole-register writes use the header's LL_TIM_WriteReg: SR = 0 in InitTimeBase (rule 3) and nothing else". Es una sola: `SR = 0` en `TIM_LL_InitTimeBase`. No reescribo la historia.

### 5.2 Build y tamaños

Evidencia en `03_firmware_templates/prj_01_template_dualcore/docs/`, todo en `b63204a`. Los mismos archivos en `2d1e525` (`10c808e`) dan los mismos valores: `b63204a` solo cambia el host.

- `build_all -Clean`: 8/8 (`tim_f2_build_all_b63204a.txt`).
- `selftest-tim` compilado desde un directorio vacío: 0 avisos (`tim_f2_selftest_build_b63204a.txt`).
- La compilación aparte con `CMAKE_C_FLAGS=-Werror` de `selftest-tim`, `debug` y `release`: 0 avisos en las tres (`tim_f2_werror_b63204a.txt`).
- `git diff e7542dc -- 02_firmware_projects` sale vacío. El alcance desde `e7542dc` sigue siendo `01_shared_drivers/03_tim`, `01_shared_drivers/CMakeLists.txt`, `03_firmware_templates/prj_01_template_dualcore` y `tools/ad2.py`.
- `git diff 3174fef -- 01_shared_drivers/03_tim/tim_bm.h`: 0 líneas quitadas, 24 añadidas (`tim_f2_tim_bm_h_diff_b63204a.txt`). Son los diez `TIM_BM_Check*` y los cinco selectores `TIM_BM_USE_*` (R7-2).
- **Compilación aislada** (`tim_f2_isolated_build_b63204a.txt`): 0 diagnósticos para `tim_ll.c` y `tim_bm.c`, en CM7 y en CM4.
  - Flags exactos: `arm-none-eabi-gcc 14.3.1 -c -std=c11 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Werror -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -DSTM32H755xx -DCORE_CM7` (en CM4, `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -DCORE_CM4`).
  - Includes: `-isystem` para `CMSIS/Include`, `CMSIS/Device/ST/STM32H7xx/Include` y `STM32H7xx_HAL_Driver/Inc` del paquete Cube que usa `prj_01` (`02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7/Drivers`, solo lectura), e `-I` para `01_shared_drivers/03_tim`.
  - Sin `-DUSE_HAL_DRIVER` ni `-DUSE_FULL_LL_DRIVER`.
  - `-M` muestra que `tim_ll.c` incluye solo CMSIS, `stm32h7xx_ll_tim.h` (ningún otro header del directorio de la HAL), `tim_bm.h` y `tim_ll.h` (A8, C8 con R7-3).

Producción de `prj_01` frente a `2481396` (R4-2), de `tim_f2_map_sizes_b63204a.txt`: 0 de diferencia en todas las secciones asignadas de CM7 y CM4, en debug y en release. CM7 da 29304/132/55668 y 28356/132/55672; CM4, 3456/24/12560 y 3112/24/12560.

Objetos de `selftest-tim`; la columna .text suma `.text` y `.rodata`:

| Objeto | Núcleo | .text | .data | .bss | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| `tim_ll.c.obj` | CM7 | 4224 | 0 | 0 | `tim_f2_map_sizes_b63204a.txt` |
| `tim_ll.c.obj` | CM4 | 4122 | 0 | 0 | ídem |
| `tim_bm.c.obj` | CM7 | 6532 | 0 | 0 | ídem (F1: 5992) |
| `tim_bm.c.obj` | CM4 | 5882 | 0 | 0 | ídem (F1: 4514) |
| `tim_selftest.c.obj` | CM7 | 48678 | 12 | 8874 | ídem |
| `tim_selftest_common.c.obj` | CM7 | 5362 | 0 | 1064 | ídem |
| `tim_selftest.c.obj` | CM4 | 4719 | 4 | 324 | ídem |
| `tim_selftest_common.c.obj` | CM4 | 5340 | 0 | 1064 | ídem |

`.data = .bss = 0` en `tim_ll.c.obj` y en `tim_bm.c.obj`, en los dos núcleos (C7). Los greps del plan sobre `tim_ll.[ch]` dan 0 líneas: A2 (`BOARD_|GPIO[A-K]|HSEM|DMA[12]_Stream|NVIC_|board_def|shared_memory|gpio_|dma_`), A4 (`03_tim/`), A9 (`RSTR|RCC->CFGR\s*=|D2CFGR\s*=|__enable_irq|->SR\s*&=|->SR\s*\|=`), A13 y `LL_TIM_Init|USE_FULL_LL_DRIVER` fuera de comentarios.

### 5.3 Pruebas

Todos los valores salen de `docs/tim_f2_bench_b63204a.txt`:

- una sola ejecución, sin repetir (1626 líneas);
- checkout `b63204a`, `tree clean`; `Rev` `b63204a` e id `09b6373620b56df8` iguales en el host, CM7 y CM4; protocolo v4; lista de comprobación física confirmada por Hector;
- **192/192, 0 líneas FAIL, `FW VERDICT=PASS` (0 fallos) y `HOST VERDICT=PASS`**.

| ID | Tier | Núcleo | Resultado | Valor medido | Control negativo: valor | Oráculo |
| --- | --- | --- | --- | --- | --- | --- |
| Regresión F0 | banco | CM7 | PASS | T0a: pull-up `0x1FFF`, pull-down `0x0000`; T0b(a)–(g); TREF\_A/TREF\_B: ē = +12 464,9 ppm (s\_ef 377,8), x = +12 111,9 ± 9,0 ppm, \|ē − x\| = 353,0 ≤ 519,8 (R4-1) | los de F0 | los de F0 |
| Regresión F1 | BM | CM7, CM4 | PASS | todas las pruebas de F1: T6 BM 36/36 y 16/16; T8 BM 24/24 con d = 2,12 ticks; T9 16 periodos de 200..200 ticks; T13(b) 202..203 frente a 202,38 ± 2,01 (R7-1); T15 0 pérdidas contra 46 686 ISR (control: 9536) | los de F1 | los de F1 |
| T1 | LL | CM7 | PASS | 47 rechazos con constantes LL, estado esperado en los 47, fotos (6 timers y los dos bancos de RCC) sin cambios. Incluyen constantes desconocidas (`ERR_PARAM`), constantes LL fuera de la API (`LL_TIM_OCMODE_RETRIG_OPM1`, `LL_TIM_TS_ETRF`, `LL_TIM_TS_ITR9`, `LL_TIM_TRGO2_UPDATE`, `LL_TIM_CLOCKSOURCE_EXT_MODE2`, `COMIE`, `BIE`), `LL_TIM_TIM17_TI1_RMP_SPDIF_FS` (`ERR_UNSUPPORTED`, §5.8) y el orden de C6 (TRC en TIM13 con un filtro desconocido: `ERR_UNSUPPORTED`) | O1: `TIM_LL_ConfigOutput(TIM2 CH1)` cambia exactamente CCMR1 y CCR1 (`0x0002040`) | registros leídos en crudo |
| T1 | LL | CM4 | PASS | 12 rechazos de TIM15, estados esperados, foto sin cambios | O1: `0x0002040` (CCMR1, CCR1) | ídem |
| T2 | LL | CM7 | PASS | 6008 imágenes de 12 instancias por LL: 0 diferencias con la codificación del RM a mano, 0 llamadas con error. **Paridad:** las 6008 comparadas bit a bit (todo registro salvo CNT) con la imagen de la misma configuración por `tim_bm`: 0 diferencias | por instancia, una imagen esperada alterada se detecta, y una imagen de `tim_bm` alterada (ARR ^ 1) también | campos del RM a mano + imagen de BM |
| T2 | LL | CM4 | PASS | TIM15: 1313 imágenes, 0 diferencias con la mano; paridad 1313/1313, 0 diferencias | ídem | ídem |
| T5 | LL | CM7, CM4 | PASS | (a)–(d) como en F1 en las 12 instancias de CPU1 y TIM15 (a)(b) en CM4, 24 medidas: de −6 a +3 ppm, \|e\| ≤ 100 | PSC + 1 / sin RCR / 16 bits: fuera de tolerancia, como en F1 | `CYCCNT` |
| T6 | LL | CM7 | PASS | `T6_TIM1_LL`: TIM1 CH1–CH4 × 9 puntos, 36/36 dentro de ±0,1 % sobre f/f\_MCO1 y de ±15 ns en alto | MOE = 0 con los 4 canales: 0 flancos, nivel 0 | LA con DIO14 en la misma adquisición (R3-2) |
| T6 | LL | CM4 | PASS | `T6_TIM15_LL`: TIM15 CH1/CH2 × 8 puntos, 16/16 | CC1E = CC2E = 0: 0 flancos, nivel 0 | ídem |
| T8 | LL | CM7 | PASS | `T8_LL`, con R5-1, R6-1 y R7-1:<br>• d = 2,00 ticks (0 ≤ d ≤ 4);<br>• 24/24 puntos en PWM input;<br>• prescaler 8: 1618–1620 frente a 1620,4 ± 9,1;<br>• filtro: 2072–2074 frente a 2073,9 ± 11,4;<br>• overcapture: `CC1OF` 1 y luego 0 | polaridad invertida: 1515–1516 frente a 1516,6 ± 9,6; CC1E = 0: CC1IF 0; sin filtro, el glitch se captura (518–519 y 1555–1556) | Digital Out y MCO1 en DIO14 en la misma generación |
| T10(a) | LL | CM7 | PASS | TIM3 → TIM2 external clock 1 (`LL_TIM_CLOCKSOURCE_EXT_MODE1`, ITR2): CNT = 1000 exactos | ITR1 (TIM8 parado): CNT = 0 | `CNT` del esclavo |
| T10(b) | LL | CM7 | PASS | TIM2 OC1REF → TIM4 gated (ITR1): CNT = 20 000 = N·CCR | ITR3 (TIM8 parado): CNT = 0 | ídem |
| T12 | LL | CM7 | PASS | (a) los 14 grupos de F1 a 10 kHz durante 100 ms, 1000 en cada vector, 0 ISR sin flag; (b) 0 IRQ espurias; (c) `TIM_LL_GetIRQn`, 84 pares, 0 diferencias | UIE = 0: 0 IRQ; a mano (UG con URS = 0): 1 IRQ | contadores de ISR, tabla a mano |
| T12 | LL | CM4 | PASS | TIM15: 1000 IRQ en 100 ms; 0 espurias; `GetIRQn` = `TIM15_IRQn` | a mano: 1; UIE = 0: 0 | ídem |
| T14 | LL | CM7, CM4 | PASS | 168 Deinit en CM7 con su imagen de reset exacta (F1 y F2); TIM15 tras T5, T6 y T12 por LL; S5 en verde al final de `T6_TIM1_LL` y de `T8_LL` | el control de F1 (TIM2 sin Deinit) se detecta | tabla de reset del RM a mano |
| B1 | LL | CM7, CM4 | registro | §5.4 | — | `.map` y DWT |

**Margen de T8 con R7-1.** Peor residuo frente a su cota en la captura de evidencia (periodo o alto, de las 8 capturas de cada punto):

| Frecuencia | BM | LL |
| --- | --- | --- |
| 1 kHz | 0,17 | 0,13 |
| 10 kHz | 0,16 | 0,15 |
| 100 kHz | 0,31 | 0,31 |
| 1 MHz | 0,52 | 0,33 |

A 1 MHz manda el término en ticks, que R7-1 no cambia.

Las siete ejecuciones de desarrollo de F2 (`6d71ece`), sin editar, en `docs/`, anteriores a R7-1 (tolerancias de la revisión 6):

| Captura | Imagen | Host | FW | Fallos |
| --- | --- | --- | --- | --- |
| `tim_dev_f2_bench_95fa214-dirty.txt` | `95fa214-dirty` | 192/192 | FAIL (1) | T9 BM: 200..400 ticks (una captura perdida; corregido en `a1c537b`) |
| `tim_dev_f2_bench_95fa214-dirty_2.txt` | `95fa214-dirty` | 190/192 | FAIL (1) | T9 BM: 199..401; T8 BM filtro: 2077 frente a 2073,9 ± 3,1; T8 LL TI2 10 kHz 75 %: las 8 capturas unos +1100 ppm sobre la media de MCO1 (1,01291 antes, 1,01185 MHz después) |
| `tim_dev_f2_series_2d1e525_1.txt` | `2d1e525` | 191/192 | PASS | T13(b) BM: 202..204 frente a 202,69 ± 1 |
| `tim_dev_f2_series_2d1e525_2.txt` | `2d1e525` | 192/192 | PASS | — |
| `tim_dev_f2_series_2d1e525_3.txt` | `2d1e525` | 191/192 | PASS | T8 BM TI1 10 kHz 75 %: las 8 capturas unos +1000 ppm sobre la media de MCO1 |
| `tim_dev_f2_series_2d1e525_4.txt` | `2d1e525` | 192/192 | PASS | — |
| `tim_dev_f2_series_2d1e525_5.txt` | `2d1e525` | 192/192 | PASS | — |

### 5.4 Medidas (B1)

Ciclos de una llamada en frío, con DWT y PRIMASK, en `docs/tim_f2_bench_b63204a.txt`. Instancias: TIM2 en CM7 (400 MHz) y TIM15 en CM4 (200 MHz).

| Función | BM CM7 | LL CM7 | BM CM4 | LL CM4 | BM en F1 (CM7 / CM4) |
| --- | --- | --- | --- | --- | --- |
| `TakeFlags` | 186 | 213 | 276 | 301 | 152 / 280 |
| `OutputEnable` | 393 | 614 | 526 | 1116 | 348 / 489 |
| `ConfigOutput` | 533 | 916 | 568 | 1166 | 442 / 514 |
| `InitTimeBase` | 455 | 881 | 544 | 1138 | 404 / 511 |

- **Coste de LL:** `tim_ll` llama a `TIM_BM_Check*` y escribe campo a campo con los inlines de LL. Cada inline es una read-modify-write propia, así que un registro que `tim_bm` escribe una vez, LL lo escribe varias.
- **Primeras ejecuciones (antes de `3652bdf`):** cada operación de `tim_bm` buscaba la instancia dos veces. Con eso, `OutputEnable` en CM4 llegaba a 704 ciclos e `InitTimeBase` a 678 (capturas `95fa214-dirty`). Con `3652bdf` CM4 vuelve a estar cerca de F1.
- **CM7, en frío:** sigue entre 30 y 90 ciclos por encima de F1. La medida es de una llamada en frío y depende de la caché de instrucciones y de la posición del código.
- **Peor caso de `CalcTimeBase` (T3):** 1141 ciclos en CM7.
- **Pila de Status\_Task:** quedan libres 238 palabras de 512.

### 5.5 Desviaciones del plan

Todas quedan recogidas en la revisión 7 del plan (columna de la derecha).

| # | Qué decía el plan (revisión 6) | Qué hice | Por qué | Consulta y revisión 7 |
| --- | --- | --- | --- | --- |
| 1 | `tim_ll` "valida llamando a `tim_bm`", con una sola copia; la API de `tim_bm` está congelada y solo se añade | Diez funciones nuevas en `tim_bm.h`: `TIM_BM_CheckTimeBase`, `CheckPeriod`, `CheckOutput`, `CheckCompare`, `CheckChannels`, `CheckInput`, `CheckPwmInput`, `CheckEvents`, `CheckMaster` y `CheckSlave`, con los selectores `TIM_BM_USE_*`. Cada una hace los chequeos de su operación, en el orden de C6 y con los mismos estados, sin escribir nada; solo lee los registros de estado (`ERR_BUSY`, 2.16.1, CCxS), como la operación. La operación de `tim_bm` las usa (`3652bdf`: a través de un `chk_*` estático, con una sola búsqueda) | Las funciones congeladas validan y escriben a la vez: sin una entrada de solo validación, LL no puede validar con `tim_bm` y escribir con los inlines de LL. Es el patrón de `GPIO_BM_CheckConfig`, `HSEM_BM_CheckInit` y `DMA_BM_Check*`. `CheckChannels` lleva un selector en vez del `bool` del ejemplo aprobado, porque `OutputDisable` comprueba solo la máscara | El responsable, en el chat, el 2026-10-02 (instrucciones §7). **R7-2** |
| 2 | `tim_ll` escribe con los inlines de `stm32h7xx_ll_tim.h` | `TIM_LL_EnableClock/DisableClock`, `TIM_LL_Deinit`, `TakeFlags`, `TakeOvercapture` (traduce la máscara de canales), `WaitFlags`, `GetIRQn` y `GetDmaRequest` llaman a `tim_bm` | `stm32h7xx_ll_tim.h` no tiene el banco de RCC (está en `ll_bus.h`, fuera de C8). La imagen de reset por instancia es parte de la tabla de capacidades, y otra copia sería un hallazgo. `SR = ~taken` queda en un solo sitio | El responsable, 2026-10-02. **R7-2** |
| 3 | "Mismas operaciones que `tim_bm`" | Sin versión LL de las funciones que no llevan constantes LL: `IsValidInstance`, `GetCaps`, `KernelClockFromFields`, `GetKernelClockHz`, `CalcTimeBase`, `DutyToCompare` y los `Check*`; se usan como `TIM_BM_*` a través de `tim_ll.h`. `TIM_LL_GetCounter`, `TIM_LL_WriteCcr` y `TIM_LL_ReadCapture` son `static inline` sobre los getters/setters de LL | Una copia LL de una función pura no traduciría nada | **R7-3 (a)** |
| 4 | Constantes LL (`LL_TIM_COUNTERMODE_*`, `LL_TIM_OCMODE_*`, `LL_TIM_CHANNEL_CH1..4`, `LL_TIM_IC_POLARITY_*`, `LL_TIM_TS_*`) | Además, `LL_TIM_ACTIVEINPUT_*`, `LL_TIM_ICPSC_*`, `LL_TIM_IC_FILTER_*`, `LL_TIM_TRGO_*` y `LL_TIM_SLAVEMODE_*`; external clock 1 es `LL_TIM_CLOCKSOURCE_EXT_MODE1`. TISEL es una `LL_TIM_TIMx_TIy_RMP_*`. Las máscaras de eventos usan `LL_TIM_DIER_UIE/CCxIE/TIE`. ARPE, OPM, URS, la polaridad de OC y el preload son `bool`. Una constante desconocida da `ERR_PARAM` en el lugar del orden C6 que le toca (T1 caso 24) | Una constante para cada campo enumerado; el orden de los rechazos lo fija `tim_bm` | **R7-4** |
| 5 | C8: solo funciones `static inline` de `stm32h7xx_ll_tim.h` | Un macro de la misma cabecera: `LL_TIM_WriteReg(tim, SR, 0U)`, el `SR = 0` de la regla 3. `LL_TIM_ReadReg` para los read-back de registros enteros | No hay ningún inline que borre todos los flags en una escritura | **R7-3 (b)**, C8 |
| 6 | Paridad bit a bit con BM | CCxP y CCxNP de una salida se escriben con `LL_TIM_IC_SetPolarity` (una RMW de los dos bits), y la configuración de entrada pone primero `LL_TIM_OC_SetMode(FROZEN)` para borrar OCxM[3] | `LL_TIM_OC_SetPolarity` deja CCxNP y la cabecera no tiene `LL_TIM_CHANNEL_CH4N`. `LL_TIM_IC_Config` no toca OCxM[3] | **R7-4** |
| 7 | `GenerateEvent`: `EGR = ev_mask` | `TIM_LL_GenerateEvent` genera los eventos uno a uno, en orden de bit | Cada inline escribe un bit de EGR | **R7-4** |
| 8 | T9 (F1): 16 periodos de 200 ± 1 ticks | `a1c537b`: el bucle de captura de T9 consulta `TIM_BM_TakeFlags` con PRIMASK durante las 17 capturas (como mucho unos 0,4 ms). El criterio y el oráculo no cambian | Con el código de F2, T9 perdió una captura en las dos primeras ejecuciones (200..400 y 199..401). En las cinco de la serie y en la de evidencia, T9 da 199..201 o 200..200 | **R7-4** |
| 9 | T6 y T8 con LL; T11 y T13 solo en F1 BM | Pasos nuevos `T6_TIM1_LL`, `T6_TIM15_LL` y `T8_LL` (protocolo v4). T11, T13(a) y T13(b) quedan en los pasos BM. `T8_LL` mide su propia d (R5-1). T5(e) queda en BM | Un paso por tier deja cada captura del LA atribuida a un tier | **R7-4** |
| 10 | Captura `tim_<fase>_<prueba>_<hash>.txt` | `docs/tim_f2_bench_b63204a.txt`; las de desarrollo, `docs/tim_dev_f2_*` | Como en F1 | No |
| 11 | T8 y T13(b): término relativo 0,1 % (T13(b): ±1 tick) | `b63204a`: 0,5 % (`CAP_REL_TOL`) en los puntos en PWM input, la polaridad invertida, ICPSC = 8, el filtro y T13(b). El control sin filtro comparte el código del filtro y mantiene 1 tick + 0,1 %, porque R7-1 no lo nombra | La deriva de MCO1 entre ventanas (§5.8) | **R7-1** |

### 5.6 Respuesta a la auditoría de F2 (parte 1 y adenda; plan revisión 7)

La parte 1 de la auditoría de F2 (`0853663`) no encuentra defectos en `tim_ll` ni en el refactor de `tim_bm`, y confirma el problema de T8/T13(b). Su adenda aprueba D9, que es la revisión 7 del plan (`9611ada`). Con ella, la parada de §7 queda resuelta.

| Cambio | Commit | Qué ejecuté para comprobarlo | Estado |
| --- | --- | --- | --- |
| R7-1: término relativo de 0,5 % en T8, en BM y en LL (puntos en PWM input, periodo y alto, `2 + 0.005 * valor`; polaridad invertida, `2 + 0.005 * valor`; ICPSC = 8 y filtro, `1 + 0.005 * valor`) y en T13(b) (`1 + 0.005 * periodo`, antes ±1 tick). Cada línea imprime la tolerancia usada. T6, T9, T11, la d de R5-1, los demás criterios y el firmware no cambian | `b63204a` | `git diff 2d1e525 b63204a`: solo `tools/tim_hw_check.py`, con 0,005 en `CAP_REL_TOL` y sus seis usos. En la captura, las líneas imprimen las cotas nuevas: a 50 %, el periodo `+-255.1` (1 kHz), `+-103.2` (10 kHz), `+-12.1` (100 kHz) y `+-3.0` (1 MHz); además, `+-9.6` (polaridad invertida), `+-9.1` (ICPSC 8), `+-11.4` (filtro) y `+-2.01` (T13(b)). La captura de evidencia da 192/192 | aplicado en `b63204a`, pendiente de verificación |
| R7-2: `TIM_BM_Check*`, `TIM_BM_USE_*` y la delegación de `tim_ll` en `tim_bm` | — (ya en `916a3d4`, `3652bdf` y `bb7de38`) | `git diff 3174fef -- tim_bm.h`: +24/−0 | sin cambios de código |
| R7-3: API de `tim_ll` sin copias de las funciones puras, y las macros `LL_TIM_WriteReg`/`ReadReg` en C8 | — (ya en `bb7de38`) | `-M` de la compilación aislada | sin cambios de código |
| R7-4: banco v4, T9 con PRIMASK y las desviaciones 4, 6 y 7 | — (ya en `5c4fe1c`, `a1c537b` y `2d1e525`) | la captura de evidencia | sin cambios de código |

### 5.7 Erratas del plan encontradas

Ninguna.

### 5.8 Problemas abiertos y no ejercitado

- **T8 y T13(b) contra la deriva de MCO1 (resuelto con R7-1).** Entre las ventanas de 41 µs antes y después de capturas que duran milisegundos, MCO1 se mueve una mediana de unos 250–300 ppm y hasta unos 1700 ppm (N1). Con 0,1 %, 3 de 7 sesiones de F2 fallaron por eso (`6d71ece`). Con 0,5 %, el peor residuo de la captura de evidencia queda en el 52 % de su cota (1 MHz, donde manda el término en ticks) y por debajo del 31 % en las demás frecuencias.
- **`LL_TIM_TIM17_TI1_RMP_SPDIF_FS`.** La cabecera LL define `TIM17_TI1SEL = 0001` como SPDIF FS, y RM0399 rev 4 lo da como reservado. El plan y el driver siguen el RM: `tim_ll` lo rechaza con `ERR_UNSUPPORTED` (T1 LL caso 20).
- **No ejercitado con LL:**
  - `TIM_LL_EnableDmaRequest`/`DisableDmaRequest`, salvo el rechazo de T1: T13 es solo BM según el plan;
  - `TIM_LL_GenerateEvent`, `TIM_LL_SetPeriod` y `TIM_LL_SetCompare`, salvo sus rechazos de T1;
  - T7, T9, T11, T13, T15 y T16, que el plan no pide para LL.
- **Lo no ejercitado de F1** sigue igual (F1 §5.8).

## Decisiones abiertas usadas (F2)

| # | Opción usada | Consultada |
| --- | --- | --- |
| 1–6 | Las de F1, sin cambios | No (por defecto) |

Decisiones del responsable de esta fase:

- añadir `TIM_BM_Check*`, y que LL delegue en `tim_bm` (2026-10-02, chat; R7-2);
- la serie de diagnóstico y la parada (2026-10-02, chat);
- D9, aprobada como revisión 7 (`AUDIT.md`, F2 adenda a la parte 1).

Estado de F2: **entregada para auditoría** (`b63204a`; evidencia en `fe8a07c`).

## F3 (tim_hal)

Plan de referencia: revisión 7 (`9611ada`). F3 empieza sobre `8e968c1`. `AUDIT.md`, F2 parte 3, dice que F2 pasa y congela la API de `tim_ll` en `tim_ll.h` de `071bddb`, junto con los añadidos R7-2 de `tim_bm.h`.

### Resumen

- `tim_hal` implementa el contrato de F3 en `01_shared_drivers/03_tim/tim_hal.{h,c}`: las nueve funciones del plan sobre un `TIM_HandleTypeDef` del llamador, con los valores de `tim_bm` (sus estructuras, canal 1..4, `TIM_BM_MMS_*`, `TIM_BM_SMS_*`, `TIM_BM_TRG_*`).
- Cada función valida el handle y luego llama a `TIM_BM_Check*` antes de la HAL. Después de cada llamada a la HAL hace read-back de los campos que esa llamada escribe: `ERR_HAL` o `ERR_VERIFY`.
- Sin callbacks ni MSP (C14). Sin `HAL_TIM_MODULE_ENABLED` no compila nada (C8).
- El target es `stm32h7_driver_tim_hal`, que enlaza `_tim_bm`, sin target sin sufijo y fuera del paraguas. `prj_01` lo enlaza solo en CM7 y solo con `TIM_DRIVER_SELFTEST`.
- Pruebas HAL en CM7: T1, T2 (paridad con `tim_bm` y 15 casos de diferencias por diseño), T5, T6, T8, T9, T14 y B1. TIM6 se comprueba antes, entre y después de las pruebas HAL. El protocolo del banco pasa a v5, con tres pasos nuevos: `T6_TIM1_HAL`, `T8_HAL` y `T9_HAL`.
- **La captura de evidencia es `docs/tim_f3_bench_d33d9e5.txt`.** Una sola ejecución: 259/259 comprobaciones del host, `FW VERDICT=PASS` (0 fallos) y `HOST VERDICT=PASS`. Antes hubo una ejecución de desarrollo con la misma imagen (`docs/tim_dev_f3_bench_d33d9e5.txt`, `81cb614`), con el mismo resultado.
- `git diff 071bddb -- tim_bm.h tim_ll.h` sale vacío.
- El cierre no ha empezado: espera a que `AUDIT.md` diga que F3 pasa y a que el auditor lo pida.

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F0 | pasa (`AUDIT.md`, auditoría 6) | `cae5414` | `docs/tim_f0_audit_bench_cae5414.txt` |
| F1 | pasa (`AUDIT.md`, F1 parte 4) | `3174fef` | `docs/tim_f1_audit_bench_3174fef.txt` |
| F2 | pasa (`AUDIT.md`, F2 parte 3) | `071bddb` | `docs/tim_f2_audit_bench_071bddb.txt` |
| F3 | entregada para auditoría (`d33d9e5`; evidencia en `1f0e8aa`) | el commit que añade esta sección | `03_firmware_templates/prj_01_template_dualcore/docs/tim_f3_bench_d33d9e5.txt` |

### 5.1 Commits

Rango de la entrega: `8e968c1..` el commit que añade esta sección.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `f29d578` | feat(tim): add tim_hal, the HAL timer tier | — |
| `0610f74` | build(tim): add the stm32h7_driver_tim_hal target | — |
| `3a804ea` | build(tim): link tim_hal into selftest-tim on CPU1 | — |
| `765874e` | test(prj_01): the F3 tests of tim_hal on CPU1, bench protocol v5 | — |
| `d33d9e5` | test(prj_01): the host side of the F3 bench, protocol v5 | — |
| `1f0e8aa` | docs(tim): add the F3 evidence at d33d9e5 | — |
| `81cb614` | docs(tim): add the F3 development run (not evidence) | — |

### 5.2 Build y tamaños

Evidencia en `03_firmware_templates/prj_01_template_dualcore/docs/`, todo en `d33d9e5` con el árbol limpio:

- `build_all -Clean`: 8/8 (`tim_f3_build_all_d33d9e5.txt`).
- `selftest-tim` compilado desde un directorio vacío: 0 avisos (`tim_f3_selftest_build_d33d9e5.txt`).
- La compilación aparte con `CMAKE_C_FLAGS=-Werror` de `selftest-tim`, `debug` y `release`: 0 avisos en las tres (`tim_f3_werror_d33d9e5.txt`).
- `git diff 071bddb d33d9e5 -- tim_bm.h tim_ll.h`: 0 líneas quitadas y 0 añadidas (`tim_f3_api_diff_d33d9e5.txt`).
- `git diff e7542dc -- 02_firmware_projects` sale vacío. El alcance desde `e7542dc` sigue siendo `01_shared_drivers/03_tim`, `01_shared_drivers/CMakeLists.txt`, `03_firmware_templates/prj_01_template_dualcore` y `tools/ad2.py`.
- **Compilación aislada** (`tim_f3_isolated_build_d33d9e5.txt`): 0 diagnósticos para `tim_hal.c`, `tim_ll.c` y `tim_bm.c`, en CM7 y en CM4.
  - Flags exactos, los de `10c808e`: `arm-none-eabi-gcc 14.3.1 -c -std=c11 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Werror -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -DSTM32H755xx -DCORE_CM7` (en CM4, `-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -DCORE_CM4`).
  - Includes: `-isystem` para `CMSIS/Include`, `CMSIS/Device/ST/STM32H7xx/Include` y `STM32H7xx_HAL_Driver/Inc` del Cube que usa `prj_01`, e `-I` para `01_shared_drivers/03_tim`.
  - Solo para `tim_hal.c`, además: `-DUSE_HAL_DRIVER` e `-isystem` para el `Core/Inc` de `prj_01` del núcleo, donde está su `stm32h7xx_hal_conf.h`, que incluye `stm32h7xx_hal.h`.
  - CM7, con `HAL_TIM_MODULE_ENABLED`: el objeto define las nueve funciones `TIM_HAL_*`, 2800 B de `.text` y `.data = .bss = 0`.
  - CM4, sin `HAL_TIM_MODULE_ENABLED`: objeto vacío, sin símbolos (C8).
  - `-M` de `tim_hal.c` (CM7): `tim_hal.h`, `tim_bm.h`, CMSIS, `stm32h7xx_hal.h`, `stm32h7xx_hal_conf.h` de `prj_01` y los headers de los módulos HAL que activa esa configuración (`def`, `rcc`/`rcc_ex`, `gpio`/`gpio_ex`, `dma`/`dma_ex`, `mdma`, `cortex`, `exti`, `flash`/`flash_ex`, `hsem`, `pwr`/`pwr_ex`, `tim`/`tim_ex`, `uart`/`uart_ex`, `Legacy/stm32_hal_legacy.h`). Ni FreeRTOS ni otro header del proyecto.

Producción de `prj_01` frente a `2481396` (R4-2), de `tim_f3_map_sizes_d33d9e5.txt`: 0 de diferencia en todas las secciones asignadas de CM7 y CM4, en debug y en release. CM7 da 29304/132/55668 y 28356/132/55672; CM4, 3456/24/12560 y 3112/24/12560.

Objetos de `selftest-tim`; la columna .text suma `.text` y `.rodata`:

| Objeto | Núcleo | .text | .data | .bss | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| `tim_hal.c.obj` | CM7 | 3038 | 0 | 0 | `tim_f3_map_sizes_d33d9e5.txt` |
| `tim_ll.c.obj` | CM7 | 4224 | 0 | 0 | ídem (F2: 4224) |
| `tim_ll.c.obj` | CM4 | 4122 | 0 | 0 | ídem (F2: 4122) |
| `tim_bm.c.obj` | CM7 | 6532 | 0 | 0 | ídem (F2: 6532) |
| `tim_bm.c.obj` | CM4 | 5882 | 0 | 0 | ídem (F2: 5882) |
| `tim_selftest.c.obj` | CM7 | 59427 | 12 | 10354 | ídem |
| `tim_selftest_common.c.obj` | CM7 | 5438 | 0 | 1064 | ídem |
| `tim_selftest.c.obj` | CM4 | 4719 | 4 | 324 | ídem |
| `tim_selftest_common.c.obj` | CM4 | 5416 | 0 | 1064 | ídem |

`.data = .bss = 0` en los tres objetos del driver (C7). Los 10 354 B de `.bss` de `tim_selftest.c.obj` incluyen los handles de la prueba, uno por instancia de CPU1. El handle es del llamador.

Greps del plan sobre `tim_hal.[ch]`: A2, A9 y A13 dan 0 líneas, y la regla 12 también. A4 solo encuentra, en comentarios, la ruta del plan. El único `#include` de `tim_hal.c` es `tim_hal.h`, que incluye `stm32h7xx_hal.h` y `tim_bm.h`.

`git grep -n TIM6` en los `tim_selftest.c`: ninguna escritura. Son lecturas (`tim6_snap`), comentarios y las tablas de `GetIRQn`/`GetDmaRequest` de F1.

### 5.3 Pruebas

Todos los valores salen de `docs/tim_f3_bench_d33d9e5.txt`:

- una sola ejecución (2045 líneas);
- checkout `d33d9e5`, `tree clean`; `Rev` `d33d9e5` e id `4e15f9841b7cb7cc` iguales en el host, CM7 y CM4; protocolo v5; lista de comprobación física confirmada por Hector;
- **259/259, 0 líneas FAIL, `FW VERDICT=PASS` (0 fallos) y `HOST VERDICT=PASS`**.

| ID | Tier | Núcleo | Resultado | Valor medido | Control negativo: valor | Oráculo |
| --- | --- | --- | --- | --- | --- | --- |
| Regresión F0 | banco | CM7 | PASS | T0a, T0b(a)–(g); TREF\_A/TREF\_B: ē = +12 005,8 ppm (s\_ef 427,8), x = +12 072,0 ± 8,2 ppm, \|ē − x\| = 66,2 ≤ 541,1 (R4-1) | los de F0 | los de F0 |
| Regresión F1 | BM | CM7, CM4 | PASS | todas las pruebas de F1: T6 BM 36/36 y 16/16; T8 BM 24/24 con d = 2,00 ticks; T9 16 periodos de 199..201 ticks; T13(b) 202..203 frente a 202,27 ± 2,01 (R7-1); T15 0 pérdidas contra 48 364 ISR | los de F1 (T15: 9067 pérdidas) | los de F1 |
| Regresión F2 | LL | CM7, CM4 | PASS | todas las pruebas de F2: T2 LL paridad 6008/6008 y 1313/1313; T6 LL 36/36 y 16/16; T8 LL 24/24 con d = 1,88 ticks | los de F2 | los de F2 |
| T1 | HAL | CM7 | PASS | 42 rechazos por `tim_hal`, con el estado esperado en los 42. En cada uno, sin cambios las fotos de 6 timers, de los dos bancos de RCC y del handle. Incluyen:<br>• los propios del handle (`ERR_PARAM`): h o cfg `NULL`, `Instance` USART3 o TIM1 + 4, `Instance` ≠ `cfg->tim`;<br>• los de `tim_bm` a través de `tim_hal`: canal, capacidad, rango, 2.16.1, `ERR_BUSY`;<br>• `StartPwm`/`StartCapture` sobre un canal de la otra dirección;<br>• un rechazo de la propia HAL: `StartPwm` dos veces, `ERR_HAL` sin escribir nada (canal en `BUSY`) | O1: `TIM_HAL_ConfigOutput(TIM2 CH1)` cambia exactamente CCMR1 y CCR1 (`0x0002040`) | registros y handle leídos en crudo |
| T1 (handle) | HAL | CM7 | PASS | `InitTimeBase` → `State` READY y los canales READY; `StartPwm` → canal BUSY; `StopPwm` → READY; `Deinit` → `State`, canales y ráfaga DMA RESET, desbloqueado | — | campos del handle |
| T2 | HAL | CM7 | PASS | 6008 imágenes de 12 instancias por `tim_hal`: 0 diferencias con la codificación del RM a mano, 0 llamadas con error. **Paridad:** las 6008 bit a bit (todo registro salvo CNT) contra `tim_bm`: 0 diferencias | por instancia, una imagen esperada alterada se detecta, y una de `tim_bm` alterada (ARR ^ 1) también | campos del RM a mano + imagen de BM |
| T2 (por diseño) | HAL | CM7 | PASS | 15 casos desde una imagen que no es la de reset, por `tim_bm` y por `tim_hal`: en cada uno las imágenes difieren exactamente en los bits listados (tabla siguiente) y en nada más | 3 casos con 0 bits listados (R5-3, CC1NP en TIM1 CH1, la etapa de entrada en `ConfigSlave`): sin diferencia | imágenes de BM leídas en crudo |
| T2 (HAL cruda) | — | CM7 | registro | la prueba de F1 con `HAL_TIM_*` sin el driver: 17 entradas, todas OCxPE (`0x808` en CCMR1/CCMR2 de TIM1–5, 8 y 12; `0x8` en CCMR1 de TIM13, 14, 16 y 17). `HAL_TIM_PWM_ConfigChannel` pone siempre OCxPE (línea 4294); `tim_hal` lo deja como pide `preload` | — | — |
| T5 | HAL | CM7 | PASS | (a)–(d) de F1 en las 12 instancias de CPU1, 22 medidas de −1 a +3 ppm (\|e\| ≤ 100). (e) es solo BM | PSC + 1, sin RCR, 16 bits y edge en vez de center: fuera de tolerancia (el menor, 1994 ppm) | `CYCCNT` |
| T6 | HAL | CM7 | PASS | `T6_TIM1_HAL`: TIM1 CH1–CH4 × 9 puntos, 36/36. Peor error de f/f\_MCO1: 378 ppm (±1000). Peor error del tiempo en alto: 10,6 ns (±15). BM y LL en la misma captura: 362 y 337 ppm, 7,5 y 7,3 ns | MOE = 0 con los 4 canales: 0 flancos, nivel 0 | LA con DIO14 en la misma adquisición (R3-2) |
| T8 | HAL | CM7 | PASS | `T8_HAL`, con R5-1, R6-1 y R7-1:<br>• d = 2,00 ticks (0 ≤ d ≤ 4);<br>• 24/24 puntos en PWM input;<br>• prescaler 8: 1618–1619 frente a 1619,0 ± 9,1;<br>• filtro: 2070–2072 frente a 2071,8 ± 11,4;<br>• overcapture: `CC1OF` 1 y luego 0 | polaridad invertida: 1515–1517 frente a 1515,3 ± 9,6; CC1E = 0: CC1IF 0; sin filtro, el glitch se captura (518 y 1554) | Digital Out y MCO1 en DIO14 en la misma generación |
| T9 | HAL | CM7 | PASS | `T9_HAL`: TIM17 `TI1SEL = 0011` (MCO1), 16 periodos de 199..201 ticks (200 ± 1); MCO1 en PA8: 1,0124 MHz | `TI1SEL = 0000` (PF7 sin cablear, pull-down) durante 1 ms: CC1IF 0 | `CCR1` por `tim_bm`; AD2 en DIO14 |
| T14 | HAL | CM7 | PASS | 206 Deinit en CM7 con su imagen de reset exacta (F1, F2 y F3), 0 fallos; S5 en verde al final de `T6_TIM1_HAL` y de `T8_HAL` | el control de F1 (TIM2 sin Deinit) se detecta | tabla de reset del RM a mano |
| TIM6 | — | CM7 | PASS | `HAL_GetTick` contra 100 ms de `CYCCNT`: 100 ms antes de las pruebas HAL, después de T1/T2/T5/B1 HAL, después de `T6_TIM1_HAL`, después de `T8_HAL` y al final; 5/5. CR1, CR2, DIER, PSC y ARR de TIM6 iguales antes y después (`0x0001`, `0x0000`, `0x0001`, 199, 999). Las ráfagas `TICK` de `T6_TIM1_HAL`, `T8_HAL` y `T9_HAL` dan, contra el reloj del host, +12 298, +11 723 y +11 928 ppm (la deriva de la placa, N1), dentro del ajuste de las 21 ráfagas | — | `CYCCNT` (mismo PLL1) y reloj del host |
| B1 | HAL | CM7 | registro | §5.4 | — | `.map` y DWT |

**Diferencias de `tim_hal` frente a `tim_bm` por diseño.** Desde la imagen de reset no hay ninguna (paridad 6008/6008). Desde otra imagen, la HAL deja o borra bits de otra forma que `tim_bm`. Líneas de `Src/stm32h7xx_hal_tim.c` (y `_tim_ex.c` en la fila 11) del Cube `8ec3113`, HAL `e3c518e` (V1.11.6). La última columna es el caso de T2 que lo muestra en la captura:

| # | Función | Bits | La HAL | `tim_bm` | Caso de T2 (bits que difieren) |
| --- | --- | --- | --- | --- | --- |
| 1 | `InitTimeBase` | CR1 UDIS, UIFREMAP | `TIM_Base_SetConfig` parte de CR1 (6937) y solo cambia DIR/CMS (6943), CKD y ARPE antes de escribirlo (6977): los conserva. Con UDIS = 1 el UG no carga PSC ni ARR (RM0399 40.4.1) | escribe 0 | 0: CR1 `0x0802` |
| 2 | `InitTimeBase` | SR salvo UIF | no toca SR. `tim_hal` borra UIF con `__HAL_TIM_CLEAR_FLAG` (C11) | SR = 0 (regla 3) | 1: SR `0x0002` |
| 3 | `InitTimeBase` | CR1 CKD en TIM12–14 | escribe CKD solo si `IS_TIM_CLOCK_DIVISION_INSTANCE` (6947), y esa macro deja fuera TIM12–14, que sí tienen CKD (RM0399 42.4.1, 42.5.1) | escribe 0 | 2: CR1 `0x0200` |
| 4 | `InitTimeBase` | DIR y CMS | los escribe en la misma escritura (6943, 6977). `tim_hal` hace antes un `HAL_TIM_Base_Init` en modo up si CMS ≠ 00 (R5-3) | CMS = 00 primero | 3: ninguno |
| 5 | `ConfigOutput` | OCxCE; OCxFE en los modos que no son PWM | `TIM_OC1_SetConfig` borra solo OC1M y CC1S (7005–7006). `HAL_TIM_PWM_ConfigChannel` borra OCxFE (4297) | escribe el campo entero | 4: CCMR1 `0x0084`; 5: CCMR1 `0x0080` |
| 6 | `ConfigOutput` | CCxNP en los canales sin salida complementaria (TIM2–5, TIM12–14, CH4 de TIM1/TIM8, CH2 de TIM15) | lo escribe solo si `IS_TIM_CCXN_INSTANCE` (7015, 7024) | CCxNP = 0 | 6 (TIM2 CH1): CCER `0x0008`; 7 (TIM1 CH1): ninguno |
| 7 | `ConfigOutput` | CR2 OISx/OISxN en TIM1, TIM8 y TIM15–17 | los pone a 0, porque `tim_hal` pasa `OCIdleState`/`OCNIdleState` RESET (7036) | no toca CR2 | 8: CR2 `0x0100` |
| 8 | `ConfigOutput`, `ConfigInput` | CCxNE de los canales con salida complementaria | lo borra (7021, 7544) | no lo toca; este driver nunca lo pone | no alcanzable con este driver |
| 9 | `ConfigInput` | OCxM[3] (bit 16 o 24 de CCMRx) | `TIM_TI1_SetConfig` cambia solo CC1S e IC1F (7553, 7562); ICxPSC lo escribe aparte | escribe el campo entero | no ejercitado (§5.8) |
| 10 | `ConfigSlave` | DIER TIE y TDE | `HAL_TIM_SlaveConfigSynchro` los borra (5655, 5658) | no toca DIER | 10: DIER `0x4040` |
| 11 | `ConfigMaster` | CR2 MMS2 en TIM1/TIM8 | `MasterOutputTrigger2 = TIM_TRGO2_RESET` (`stm32h7xx_hal_tim_ex.c`, 2018) | no toca MMS2 | 9: CR2 `0x00100000` |
| 12 | `ConfigSlave` | etapa de entrada de TI1/TI2 (CCxP/CCxNP, ICxF) | `TIM_SlaveTimer_SetConfig` la reescribe con `TriggerPolarity`/`TriggerFilter` (7386). `tim_hal` le pasa los valores que ya hay | no la toca | 11: ninguno |
| 13 | `ConfigSlave` | TS y SMS | los escribe en la misma escritura (7398–7408). `tim_hal` hace una llamada por paso: SMS = 0, TS, SMS (40.4.3) | SMS = 0 → TS → SMS | secuencia, no imagen |
| 14 | `StartCapture` | CR1 CEN | `HAL_TIM_IC_Start` pone CEN salvo en los modos trigger (2158, 2163) | `CaptureEnable` no toca CEN | 12: CR1 `0x0001` |
| 15 | `StartPwm` | CR1 CEN | `HAL_TIM_PWM_Start` pone CEN salvo en los modos trigger (1494, 1499) | `OutputEnable` no toca CEN | 13: CR1 `0x0001` |
| 16 | `StopPwm` | CR1 CEN | `__HAL_TIM_DISABLE` cuando no queda ningún CCxE/CCxNE (1534) | `OutputDisable` no toca CEN | 14: CR1 `0x0001` |

El read-back de `tim_hal` mira los campos que escribe la llamada a la HAL. Los bits que la HAL no escribe (filas 1, 2 salvo UIF, 3, 5, 6 y 9) no dan `ERR_VERIFY`.

**Margen de T8 con R7-1.** Peor residuo frente a su cota en la captura de evidencia (periodo o alto, de las 8 capturas de cada punto):

| Frecuencia | BM | LL | HAL |
| --- | --- | --- | --- |
| 1 kHz | 0,20 | 0,16 | 0,18 |
| 10 kHz | 0,15 | 0,21 | 0,21 |
| 100 kHz | 0,27 | 0,30 | 0,27 |
| 1 MHz | 0,32 | 0,33 | 0,32 |

### 5.4 Medidas (B1)

Ciclos de una llamada en frío, con DWT y PRIMASK, en `docs/tim_f3_bench_d33d9e5.txt`. Instancia: TIM2 en CM7 (400 MHz).

| Función | BM | LL | HAL |
| --- | --- | --- | --- |
| `TakeFlags` | 200 | 227 | — (`tim_hal` no la tiene) |
| `OutputEnable` | 348 | 673 | `StartPwm` (CCxE y CEN): 820 |
| `ConfigOutput` | 469 | 926 | 1064 |
| `InitTimeBase` | 456 | 982 | 763 con `State` RESET (incluye el hook MSP del proyecto); 652 con `State` READY |

- **Coste de HAL:** `TIM_BM_Check*`, la llamada a la HAL con su `__HAL_LOCK` y el read-back.
- **Peor caso de `CalcTimeBase` (T3):** 1057 ciclos en CM7.
- **Pila de Status\_Task:** quedan libres 238 palabras de 512, como en F2.

### 5.5 Desviaciones del plan

| # | Qué dice el plan (revisión 7) | Qué hice | Por qué | ¿Consultado? |
| --- | --- | --- | --- | --- |
| 1 | `TIM_HAL_Deinit` "llama a `TIM_BM_Deinit` y pone `h->State = HAL_TIM_STATE_RESET`" | Además pone en RESET los estados de canal, los de canal N y el de ráfaga DMA, y desbloquea el handle, como `HAL_TIM_Base_DeInit` (329), sin su llamada a la MSP | Sin eso, después de `Deinit` un canal se queda en `BUSY` y `HAL_TIM_PWM_Start`/`IC_Start` lo rechazan hasta el siguiente `HAL_TIM_Base_Init` | No |
| 2 | Read-back "de los registros que esa llamada escribe" | Se comparan los campos que escribe esa llamada a la HAL, con las condiciones de la propia HAL (las macros `IS_TIM_*_INSTANCE` de CMSIS que usa). Los bits que la HAL deja como estaban no se comparan | Así lo dice la regla. Lo que la HAL deja distinto de `tim_bm` está en la tabla de diferencias por diseño | No |
| 3 | `tim_hal`: envoltorio fino de la HAL | Llamadas añadidas, todas de la HAL:<br>• un `HAL_TIM_Base_Init` previo en modo up si CMS ≠ 00 (R5-3, regla 3);<br>• `HAL_TIM_OnePulse_Init` cuando OPM tiene que cambiar;<br>• `__HAL_TIM_URS_ENABLE/DISABLE`;<br>• `__HAL_TIM_CLEAR_FLAG(UPDATE)` (C11);<br>• `__HAL_TIM_ENABLE/DISABLE_OCxPRELOAD`;<br>• hasta tres `HAL_TIM_SlaveConfigSynchro`, una por paso de la nota de TS (40.4.3), con la polaridad y el filtro que ya tiene la etapa de entrada | `HAL_TIM_Base_Init` no escribe OPM ni URS y escribe DIR y CMS a la vez. `HAL_TIM_PWM_ConfigChannel` pone siempre OCxPE. `TIM_SlaveTimer_SetConfig` escribe TS y SMS a la vez y reescribe la etapa de entrada | No |
| 4 | C10: las RMW de DIER, CCER y BDTR con PRIMASK | Las llamadas a la HAL que las hacen van dentro de PRIMASK guardado y restaurado. `HAL_TIM_Base_Init` y `HAL_TIM_OnePulse_Init` no: solo tocan CR1, PSC, ARR, RCR y EGR, y pueden llamar a la MSP del proyecto, que puede esperar al tick | Un `HAL_Delay` en una MSP con PRIMASK no saldría nunca | No |
| 5 | Pruebas HAL: T1, T2, T5, T6, T8, T9 y T14 | El tier HAL de la prueba llama a `tim_hal` con un handle por instancia de CPU1. Para lo que el contrato de `tim_hal` no tiene usa la HAL sobre el mismo handle (`HAL_TIM_Base_Start`/`Stop`, `HAL_TIM_IC_Stop`) y `tim_bm` para los flags y los registros de captura, que son el lado del oráculo. T1 HAL cubre las funciones de `tim_hal`; los casos de T1 de funciones que `tim_hal` no tiene (`DutyToCompare`, `EnableClock`, `EnableIrq`, …) siguen en BM. T5(e) es solo BM. T9 pasa por el tier también en BM, con las mismas llamadas de antes | El plan no da a `tim_hal` arranque, parada ni flags, y una función pública más necesitaría aprobación (§7). Con la HAL en el mismo handle, sus estados siguen siendo los de la HAL | No |
| 6 | Banco: pasos de T6, T8 y T9 | Protocolo v5, con tres pasos nuevos: `T6_TIM1_HAL`, `T8_HAL` y `T9_HAL`. Llevan los permisos de sus pasos BM (MCO1 en PA8 y `RCC_CFGR` como en R3-2). `T8_HAL` mide su propia d (R5-1) y no repite T13(b). No hay paso de TIM15 (CPU2 sin `tim_hal`) | Como R7-4 para LL: un paso por tier deja cada captura del LA atribuida a un tier | No; es el mismo esquema que R7-4 aceptó para LL |
| 7 | "La captura tiene que mostrar `HAL_GetTick` avanzando 1 ms por ms durante y después de las pruebas HAL" | Cinco comprobaciones de 100 ms de `CYCCNT` (antes, entre las pruebas internas y los pasos, entre los pasos HAL y al final), más una foto de los registros de TIM6, que solo se leen, antes y después. Las ráfagas `TICK` de cada paso las ajusta el host contra su reloj | `CYCCNT` y TIM6 salen del mismo PLL1: 1 ms por ms del reloj de la placa. Contra el host, la placa va un +1,2 % rápida (N1) | No |
| 8 | Captura `tim_<fase>_<prueba>_<hash>.txt` | `docs/tim_f3_bench_d33d9e5.txt`; la de desarrollo, `docs/tim_dev_f3_bench_d33d9e5.txt` | Como en F1 y F2 | No |

### 5.6 Respuesta a la auditoría de F3

No hay auditoría de F3 todavía.

### 5.7 Erratas del plan encontradas

Ninguna.

### 5.8 Problemas abiertos y no ejercitado

- **`tim_hal` en CM4:** no se ejercita. `HAL_TIM_MODULE_ENABLED` está desactivado en `CM4/Core/Inc/stm32h7xx_hal_conf.h` (decisión abierta 5, opción por defecto). Compila sin símbolos (§5.2).
- **OCxM[3] en `ConfigInput` (fila 9):** sale de la lectura del código de la HAL, y no está en T2. RM0399 no da ese bit en el modo de captura, así que no sé qué lee el registro cuando CCxS ≠ 00, y no he puesto un caso cuyo resultado no puedo predecir.
- **`ERR_VERIFY`** no lo produce ninguna prueba. Tampoco lo producía en BM ni en LL. Lo he revisado en el código.
- **Lo que la HAL rechaza después de escribir:** `TIM_SlaveTimer_SetConfig` devuelve `HAL_ERROR` en gated sobre TI1F\_ED después de escribir SMCR (7434). Con `tim_hal` no se llega: `tim_bm` lo rechaza antes con `ERR_PARAM` (T1 HAL caso 25).
- **No ejercitado con HAL:**
  - `TIM_HAL_ConfigMaster`, salvo en T2 (todos los MMS) y en el caso 9 de las diferencias por diseño: T10 no está en las filas de F3;
  - `TIM_HAL_StopPwm`, salvo en T1 (el rechazo y la prueba del handle) y en el caso 14;
  - T3, T7, T10, T11, T12, T13, T15 y T16, que el plan no pide para HAL.
- **Lo no ejercitado de F1 y F2** sigue igual (F1 §5.8, F2 §5.8).

## Decisiones abiertas usadas (F3)

| # | Opción usada | Consultada |
| --- | --- | --- |
| 1–6 | Las de F1, sin cambios. La 5, `tim_hal` no se ejercita en CM4 | No (por defecto) |

La tabla de capacidades no cambia (F1).

Estado de F3: **entregada para auditoría** (`d33d9e5`; evidencia en `1f0e8aa`).
