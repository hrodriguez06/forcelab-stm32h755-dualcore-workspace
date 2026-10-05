# Informe de implementación 06_spi

Implementador: Claude (Claude Code), para Hector Rodriguez · Rama: driver/spi · Plan: IMPLEMENTATION_PLAN.md revisión 1

## Resumen

F0 (banco) está implementada y entregada para auditoría. Contiene:

- la tabla de pasos v1 y las reglas de línea S13/S14, comunes a los dos núcleos;
- `bench_claim()`/`bench_safe()` en los dos núcleos, con la lectura de SPI4 de S13 en CM7;
- `tools/spi_hw_check.py`, con S2–S8, `set_oe()` limitado a HOST_REQ y sin Digital Out;
- la opción `SPI_DRIVER_SELFTEST` y el preset `selftest-spi`;
- el selftest de F0 en `prj_01`: área SRAM4 en `0x3800C000`, Rev e id de los dos núcleos, veredicto calculado por CM7, T0a, T0c y T0b(b)–(f).

Hubo dos ejecuciones en el banco, las dos en frío y sin depurador.

- **La primera** (`docs/spi_f0_bench_65e19f7.txt`) dio `FW VERDICT=FAIL`. El caso "SPE=1" de T0b(e) leyó `SPE = 0` y no llegó a probar la guarda (§5.3, §5.8).
- **La de entrega** (`docs/spi_f0_bench_008db8b.txt`) da 10/10 comprobaciones del host, `FW VERDICT=PASS` con 0 fallos y `HOST VERDICT=PASS`.

No hay código del driver ni targets `_spi_*`, porque F0 no los lleva. F1 no ha empezado: espera a que `AUDIT.md` diga que F0 pasa. Antes de F1 hay una pregunta abierta sobre el orden de la regla 3 del plan (§5.7, P1).

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F0 | entregada para auditoría (el commit de este informe) | `008db8b` (código); evidencia en `f1e6033` | `03_firmware_templates/prj_01_template_dualcore/docs/spi_f0_bench_008db8b.txt` |
| F1 | sin empezar | — | — |
| F2 | sin empezar | — | — |
| F3 | sin empezar | — | — |

## F0

### 5.1 Commits

Rango de la entrega: `c9cdf78..` el commit que añade este informe, en `driver/spi`.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `2f95686` | test(prj_01): add the 06_spi bench step table and line rules (S13, S14) | — |
| `16458a1` | test(prj_01): add bench_claim and bench_safe of the 06_spi bench on both cores (S13, S14) | — |
| `c9ede0f` | test(prj_01): add spi_hw_check.py, the host of the 06_spi F0 bench (S2-S8, no Digital Out) | — |
| `8305fb3` | test(prj_01): add the SPI_DRIVER_SELFTEST option and the selftest-spi preset | — |
| `65e19f7` | test(prj_01): add the F0 steps and the verdict of the 06_spi selftest (T0a, T0c, T0b) | — |
| `450049b` | docs(spi): add the first F0 bench run at 65e19f7 (FW VERDICT=FAIL) | — |
| `0262bd0` | test(prj_01): set SSI before CFG2 and clear SPI4's flags in the S13 cases | fallo propio de la primera ejecución (§5.8) |
| `008db8b` | test(prj_01): print long selftest lines in pieces App_Log does not cut | fallo propio de la primera ejecución (§5.8) |
| `f1e6033` | docs(spi): add the F0 evidence at 008db8b | — |
| (este) | docs(spi): add the implementation report for F0 | — |

Notas sobre el orden:

- `16458a1` añade solo la parte de guardas de los dos `spi_selftest.c`; en ese commit nada los compila todavía.
- En `8305fb3`, `selftest-spi` no enlaza, porque los puntos de entrada llegan en `65e19f7`. Es el mismo orden que siguió `03_tim` (`987e207`). Con la opción apagada no se compila nada nuevo.

### 5.2 Build y tamaños

- **`build_all -Clean` en `008db8b`:** 8/8 (`docs/spi_f0_build_all_008db8b.txt`).
- **`selftest-spi` desde un directorio vacío:** sale con 0 y 0 líneas `warning:` o `error:` (`docs/spi_f0_selftest_build_008db8b.txt`).
- **`-Werror`:** `selftest-spi`, `debug` y `release` con `CMAKE_C_FLAGS=-Werror`, en directorios de scratch. Las tres dan configure 0, build 0 y 0 avisos (`docs/spi_f0_werror_008db8b.txt`). El `CMakeCache.txt` de cada uno dice `CMAKE_C_FLAGS:STRING=-Werror`.

Tamaños del `.map` enlazado: solo secciones de salida asignadas, empezando en "Linker script and memory map". La fuente es `docs/spi_f0_map_sizes_008db8b.txt`.

| Imagen | .text | .data | .bss | Δ frente a 2481396 | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| prj_01 debug CM7 | 29304 | 132 | 55668 | 0 (y 0 en cada sección asignada frente a `8901b77`) | `build/debug/cm7_firmware.map` |
| prj_01 debug CM4 | 3456 | 24 | 12560 | 0 | `build/debug/cm4_firmware.map` |
| prj_01 release CM7 | 28356 | 132 | 55672 | 0 | `build/release/cm7_firmware.map` |
| prj_01 release CM4 | 3112 | 24 | 12560 | 0 | `build/release/cm4_firmware.map` |

Cómo se leen estas cifras:

- **Agrupación** (la de `arm-none-eabi-size`, `build_all.ps1` y `03_tim`): .text = `.isr_vector+.text+.rodata+.ARM`, .data = `.data+.init_array+.fini_array`, .bss = `.bss+._user_heap_stack+.shared_ram`.
- **Referencia `8901b77`:** se compiló en un worktree desacoplado con sus presets `debug`/`release`, `-B` a un directorio corto de scratch y `-DCUBE_DIR` apuntando al Cube del checkout principal. Dentro del worktree, el `try_compile` de CMake falló por longitud de ruta, y el worktree no tiene el submódulo del Cube. Da los mismos números que `2481396` (`docs/tim_f0_map_sizes_2481396.txt`).
- **Árbol sucio:** con cambios sin commitear, `FW_GIT_REV` lleva `-dirty` y las imágenes de `prj_01` (y las de `prj_04`) crecen 8 B de `.text`. Lo vi en un `build_all` previo a los commits, que no es evidencia. Las cifras de la tabla son de un árbol limpio.

| Objeto | Núcleo | .text | .data | .bss | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| `spi_bm.c.obj`, `spi_ll.c.obj`, `spi_hal.c.obj` | — | no existen en F0 (0 objetos de `06_spi` en los dos `.map`) | — | — | `build/selftest-spi/cm{7,4}_firmware.map` |
| `spi_selftest.c.obj` (selftest) | CM7 | 10795 | 4 | 405 | `build/selftest-spi/cm7_firmware.map` |
| `spi_selftest_common.c.obj` (selftest) | CM7 | 621 | 0 | 0 | ídem |
| `spi_selftest.c.obj` (selftest) | CM4 | 1197 | 0 | 0 | `build/selftest-spi/cm4_firmware.map` |
| `spi_selftest_common.c.obj` (selftest) | CM4 | 577 | 0 | 0 | ídem |

En esta tabla, .text es la suma de las secciones de entrada `.text*` y `.rodata*`. La imagen `selftest-spi` mide 41652/136/56080 en CM7 y 6156/24/12560 en CM4.

### 5.3 Pruebas

Las cifras salen de `docs/spi_f0_bench_008db8b.txt` (Rev `008db8b`, id `a343a195b09c7a13` en host, CM7 y CM4, `tree clean`, checklist confirmado por Hector), salvo donde se indica otra captura. Silicio: `REV_ID=0x2003`.

| ID | Tier | Núcleo | Resultado | Valor medido | Control negativo: valor | Oráculo | Captura |
| --- | --- | --- | --- | --- | --- | --- | --- |
| T0a sueltas | — | CM7 + host | PASS | AD2: pull-up `0x4FFF`, pull-down `0x0000` (13 líneas). MCU: `IDR` pull-up `0x1FFF`, pull-down `0x0000` | — (abajo) | DIO del AD2, `IDR` | `spi_f0_bench_008db8b.txt` |
| T0a walking 1 | — | CM7 + host | PASS | 13 patrones; diferencias AD2 `[]`, MCU `0x0000` | — (abajo) | DIO del AD2, `IDR` | ídem |
| T0a control | — | CM7 + host | PASS | DIO9 (PE5) con pull-down a propósito donde se espera pull-up | Host y MCU marcan `0x0200` (esperado `0x0200`) | DIO del AD2, `IDR` | ídem |
| T0c | — | CM7 | PASS | PB5 escribió `0x5A3C` en 16 pasos y PA6 leyó `0x5A3C` (diferencias `0x0000`). PB4 y PA5, con el pull contrario a PB5, siguieron su pull (diferencias `0x0000`). PB5 `MODER` 1 durante la prueba y 0 después | Control del plan, ejecutado antes de conducir: PB5 en entrada sin pull; PA6|PB4|PA5|PB5 = `0x9` con PA6 pull-up y `0x6` con pull-down (esperados `0x9`/`0x6`). PA6 sigue a su pull, y PB5 a PA6 a través de J1 | `IDR` de PA6, PB4, PA5, PB5 | ídem |
| T0b(a) | — | host | PASS | `ad2_set_oe`: el mock de `gpio_hw_check.py`, sin cambios, rechaza 8 máscaras antes de la DLL. `spi_set_oe`: rechaza 8 máscaras (DIO8, DIO10, bus, DIO4, DIO9, DIO14, DIO15, HOST_REQ\|DIO8) con S2 y sin llamar a `ad2_set_oe` | `ad2_set_oe`: 2 máscaras permitidas pasan a la DLL. `spi_set_oe`: `0x0000` y `0x1000` pasan | DLL mock; objeto AD2 mock | ídem |
| T0b(b) CM7 | — | CM7 | PASS | Paso T0B: 23 rechazos; aceptados por error `0x00000000`; registros de GPIOA/B/E/F intactos | PA8 en AF0 aceptado | lista de casos escrita a mano; foto de `MODER/OTYPER/OSPEEDR/PUPDR/AFR` | ídem |
| T0b(b)(f) CM4 | — | CM4 | PASS | 19 casos en T0B (J1 de CM4): aceptados `0x0000F` (esperado `0x0000F`). Entre pasos: `0x00000`. Registros cambiados: `0x00000` | En T0B se aceptan PA5, PA6, PB4 y PB5 en AF5, y se rechazan entre pasos | máscaras esperadas escritas a mano (`SST_M4_CLAIMS_EXPECT_*`); foto de GPIOA/B/E | ídem |
| T0b(c) | — | CM7 + host | PASS | El MCU conduce `0x5A` en el bus; luego `MODER` del bus `0x0000` | `s5_released()` devuelve `False` dos veces, con DIO = `0x5A` | DIO del AD2 | ídem |
| T0b(d) | — | CM7 + host | PASS | Sin HOST_REQ durante 10 s: `GPIOE MODER[31:4] = 0x00004000`, `AFRH = 0`, J1 `MODER` = 0, ACK bajo | — (es el propio caso negativo) | `MODER` leído directamente, DIO13 | ídem |
| T0b(e) fuera de "bus = MCU" | — | CM7 | PASS | Paso T0B (bus = NONE) con SPI4 en un estado que S13 acepta (lectura S13 = 1): PE11..PE14 en AF5, rechazado | El mismo reclamo, en T0B_S13 y con el mismo estado de SPI4, se acepta (fila siguiente) | foto de GPIO; registros de SPI4 | ídem |
| T0b(e) S13 | — | CM7 + host | PASS | SPI4 al empezar el paso: `CR1=0`, `CFG2=0`, `SR=0x00001002` (MODF 0). Rechazados 4 estados de SPI4: `SPE=1` (releído 1, `SR=0x00001002`), `AFCNTR=1`, `MASTER=0` con `CFG2≠0`, y reloj apagado en el banco de CPU1. También 11 casos de pin/AF. Aceptados por error `0x0`/`0x0000`; GPIO intacto | `CFG2=MASTER\|SSM` aceptado y `CFG2=0` aceptado | fotos de GPIO; registros de SPI4 leídos directamente | ídem |
| T0b(e) S5 en AF5 | — | CM7 + host | PASS | PE11..PE14 en AF5 (`MODER 0xAA`, `AFR 0x5555`) con `CFG2=0x04400000`: el host ve el bus seguir los dos pulls y el MCU lee arriba `0xFF` y abajo `0x00` | — (abajo) | DIO del AD2, `IDR` | ídem |
| T0b(e) control | — | CM7 + host | PASS | — | `AFCNTR=1`, `SPE=0`. Host: CPOL=0 pull-up `s5_released` False con DIO `0xAF`; pull-down True con `0x00`. CPOL=1 pull-up False con `0xBF`; pull-down False con `0x10`. SCK = CPOL en las 4; MOSI 0 en las 4. MCU: `0xAF`/`0x00`/`0xBF`/`0x10`. Tras el Deinit (`CFG2=0`), con los pines aún en AF5, S5 vuelve a pasar (`0xFF`/`0x00`) | DIO del AD2, `IDR` | ídem |
| T0b(e) Deinit | — | CM7 | PASS | SPI4 en valores de reset: `CR1=0`, `CR2=0`, `CFG1=0x00070007`, `CFG2=0`, `IER=0`. Reloj de SPI4 en el banco de CPU1 como antes del paso (0) | — | registros leídos directamente; valores de reset de RM0399 53:3188 (CR1), 53:3285 (CR2), 53:3309 (CFG1), 53:3442 (CFG2), 53:3544 (IER) | ídem |
| T0b(f) CM7 | — | CM7 | PASS | En T0B (J1 de CM4): 5 rechazos (AF8, AF5, PB5 salida). En T0C: 6 casos, aceptado `0x01` (solo PB5 salida). En T0B_S14 (J1 de CM7): 16 casos, aceptado `0x0001F` (AF8 en los cuatro pines y PA5\|PA6 juntos). Registros intactos en los tres | Los aceptados (`0x01` en T0C, `0x1F` en T0B_S14) muestran que la guarda distingue | máscaras esperadas escritas a mano (`T0C_CLAIMS_EXPECT`, `S14_CLAIMS_EXPECT`) | ídem |
| T0b(f) CM4 | — | CM4 | PASS | En T0B_S14 (J1 de CM7): 19 casos, aceptados `0x00000`. `bench_safe` de CM4: PA5 en AF5 (`MODER=2`, `AFR=5`) y PA6\|PB4 `PUPDR=0x6`; después, J1 `MODER=0x00`, `PUPDR=0x00`, `AFR=0x0000` | En T0B el mismo núcleo sí recibe `0x0000F` | máscara escrita a mano; registros leídos directamente | ídem |
| Sin Digital Out | — | host | PASS | Enable de Digital Out releído en las 8 fronteras de paso: `0x0000` en todas. Al abrir y al cerrar: enable `0x0000`, Zet `0xFFFF` | — | `FDwfDigitalOutEnableGet` | ídem |
| Identidad | — | host | PASS | `R4-a`: cm7 = cm4 = host = `008db8b`, id `a343a195b09c7a13` en los tres; `tree clean` | — | `host_build_id()` de `gpio_hw_check.py` | ídem |
| Veredicto | — | CM7 + host | PASS | `bench completed without abort`; CM4 `timeout 0, aborts 0`; `FW VERDICT=PASS failures=0`; 10/10 comprobaciones del host; `HOST VERDICT=PASS` | — | — | ídem |

Notas sobre las comprobaciones:

- **Las de "pasa porque si no, abortaría".** Tres del host ("T0B(e) S5 with PE11..PE14 in AF5", "T0B(e) S5 after the Deinit" y "No Digital Out", esta última por la parte de `BenchAbort`) registran PASS porque `buschk()`/`step()` lanzan `BenchAbort` si falla. No son independientes de la comprobación que tienen justo encima.
- **T0c en el host.** El host no ve J1 (ningún DIO llega a esos pines). Su comprobación "T0C J1 (firmware)" solo repite el resumen de CPU1.
- **Ejecución anterior que falló, `docs/spi_f0_bench_65e19f7.txt`:**
  - Sin aborto, 10/10 comprobaciones del host y `FW VERDICT=FAIL failures=1`.
  - En T0b(e), el caso "SPE=1" leyó `SPE` = 0 (`SPE=1 (read back 0)`), y el reclamo se aceptó (`accepted by mistake 0x1`). La guarda leyó lo que había; el caso no llegó a preparar su estado.
  - En esa captura dos líneas de T0b(e) salieron cortadas por `App_Log`, una de ellas sin su `-> PASS`.
  - Todas las demás pruebas dieron los mismos valores que en `008db8b`. Causa y corrección en §5.8.

### 5.4 Medidas (B1)

- En F0 no hay funciones del driver, así que no hay ciclos que medir ni frecuencias de T8.
- Pila de `Status_Task` con el selftest: high-water mark de 276 palabras libres de 512. Es el tamaño de producción, que no cambié. La función más profunda del selftest, `step_t0b_s13`, usa 296 B (`.su` de `selftest-spi`).
- Tamaños de los objetos del selftest: §5.2.

### 5.5 Desviaciones del plan

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| T0c: la prueba (PB5 conduce 16 pasos) y, como control, PB5 en entrada sin pull | El control va **primero**, y además exige que PB5 siga a PA6 a través de J1. Si no cumple las dos cosas, PB5 no pasa a salida y la sesión aborta. Cualquier fallo de T0c aborta la sesión | PB5 es el único pin que el banco hace salida sobre la red de J1. Así queda demostrado, antes de conducir, que el cable está y que ningún otro conductor (PA7 por SB56, el PHY) está en la red. Instrucciones §6: si T0c falla, parar | No consultado; es más estricto que el plan |
| `sst_line_rule()`: el plan da la tabla de líneas | Deniega por defecto todo pin fuera de esa tabla (PA4, PA7, PG13, PC13, SWD, LEDs, PF14/PF15…). `03_tim` aceptaba los pines sin cablear | En el banco de SPI, J1 une dos pines del MCU, y los pines de la placa que no están en la tabla llevan otras cargas (PHY, LEDs) | No consultado; más estricto |
| S3 en el host: `ad2_set_oe()` | `spi_hw_check.py` pasa por `spi_set_oe()`, que solo acepta HOST_REQ, antes de `ad2_set_oe()`; T0b(a) lo prueba con un mock | `ad2_set_oe()` aceptaría DIO8, DIO10 y el bus en un paso "bus = AD2". El banco de SPI no usa ninguno (D6) | No consultado; más estricto |
| T0b(e): "fuera de un paso bus = MCU … rechazo" | Se prueba en el paso T0B (bus = NONE), con SPI4 en un estado que S13 acepta. Dentro de T0B_S13 añadí dos estados (`MASTER=0` con `CFG2≠0`, reloj apagado) y 11 casos de pin/AF | Así el rechazo depende solo del paso, y cada condición de S13 tiene su caso | No consultado; ampliación |
| T0b(e) control: "SPI4 con `AFCNTR = 1` y `SPE = 0` … SCK queda en CPOL" | `AFCNTR=1` solo durante cada lectura. Antes, el reclamo del nuevo pull se acepta con `AFCNTR=0` y `CFG2` se escribe con la polaridad final (el orden de C11). Son 4 lecturas: CPOL 0 y 1, pull arriba y abajo | `bench_claim` rechaza `AFCNTR=1`, y cambiar el pull de un pin en AF pasa por el reclamo. Así ningún pin se configura con `AFCNTR=1` y el control se ve en las dos polaridades | No consultado |
| Línea de PA8 en el plan: MCO1 en los pasos de T5 | T0B acepta PA8 en AF0 como control positivo de T0b(b), como `03_tim`. Es solo el reclamo: no se escribe y MCO1 no se toca | Toda prueba de rechazos necesita un caso que la guarda acepte | No consultado |
| T0b(f): rechazos | CM4 escribe de verdad PA5 en AF5 en su paso (T0B) para probar su `bench_safe()`. Es el único pin de J1 que F0 pone en AF | PA5 no tiene nada en su red (CN7-10, SB65 DNF) y SPI1 está en reset (`AFCNTR=0`): nadie conduce | No consultado |
| `bench_safe()`: pines en entrada | Además, si SPI4 tiene reloj en el banco de CPU1, `IER=0`, `SPE=0` y `CFG2=0`. `step_end()` falla y aborta si un paso deja `SPE` o `AFCNTR` puestos | S13: "bench_safe() lo comprueba" | No consultado |
| Formato de las líneas | `verdict()` y `log_long()` parten el texto en trozos de ≤ 120 caracteres, y los que siguen empiezan con `\| ` | `App_Log` corta a 160 caracteres (§5.8), y `app_log.c` es producción | No consultado |
| Capturas "en frío y sin depurador" | Procedimiento seguido: flasheo con el script parado; el responsable desconecta y conecta el USB del ST-LINK; después, el script, que resetea por SWD con `STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -rst` (una llamada, no una sesión de depuración), como en `03_tim` | — | Hector confirmó cada ejecución en el chat |

### 5.6 Respuesta a la auditoría

Todavía no hay auditoría.

### 5.7 Erratas del plan encontradas

| Sección del plan | Qué dice | Qué es correcto | Fuente (53:<línea>, ES0445 §, página del SDK) |
| --- | --- | --- | --- |
| P1. Regla 3 (`Init`) y regla 9 (`Deinit`); afecta a F1 | Regla 3: "`CFG2` con `AFCNTR = 0` (`MASTER = 1`, …, `SSM`, …) → `CR1` con `SSI` inactivo si `SSM = 1`". Regla 9: "`CR1`/`CR2`/`CFG1`/`CFG2` a su valor de reset" | **Es una pregunta, no algo comprobado.** Con `SSM = 1` y `SSIOP = 0`, escribir `CFG2` con `MASTER = 1` mientras `CR1.SSI` aún vale 0 (su reset) deja un maestro con SS interno activo. Según el RM eso es un fallo de modo (MODF), que pone `MASTER = 0` y bloquea `SPE` mientras MODF siga activo. En la regla 9 pasa lo mismo si `CR1` vuelve a 0 antes que `CFG2`. La HAL pone `SSI` **antes** de escribir `CFG2` (`stm32h7xx_hal_spi.c` l. 380 frente a l. 398, Cube `8ec3113`). En F0, el selftest usa ese orden desde `0262bd0` (§5.8). La duda es si MODF se evalúa también con `SPE = 0`. La primera ejecución es coherente con que sí, pero no lo medí (§5.8) | 53:1464-1476, 53:3280, 53:3657-3661; 53:3483-3487 (`SSM`) |

Antes de F1 pregunto al responsable cómo quiere que se haga: `SSI` antes de `CFG2` en `Init`, `CFG2` antes de `CR1` en `Deinit`, o ninguno de los dos cambios. No he cambiado el plan (instrucciones §2.2).

### 5.8 Problemas abiertos y no ejercitado

- **Fallo propio de la primera ejecución (`65e19f7`), corregido en `0262bd0`.**
  - En T0B, `spi4_set(0, 0)` escribía `CR1 = 0` antes de `CFG2 = 0`, y `bench_safe()` hacía lo mismo.
  - La causa probable, según el RM: en ese instante SPI4 era un maestro con `SSM = 1` y `SSI = 0`, y quedó MODF activo. En T0B_S13, `SPE` no se pudo poner a 1 y el caso no probó la guarda.
  - **No está medido:** en la primera ejecución no se leía `SR`. En la de entrega, `SR` al empezar T0B_S13 es `0x00001002` (MODF 0), y el caso lee `SPE = 1`.
  - Que la corrección funcione no demuestra la causa. Para demostrarla haría falta una ejecución de desarrollo con el orden antiguo y la lectura de `SR`, y no la he hecho.
  - Efecto eléctrico: ninguno. MODF solo pone `SPE` y `MASTER` a 0, y en esas ventanas `AFCNTR` valía 0 o los pines estaban en entrada.
- **Líneas cortadas, corregido en `008db8b`.** `App_Log` corta cada línea a `APP_LOG_LINE_MAX` (160). En la primera captura, la línea del veredicto del control `AFCNTR=1` perdió su `-> PASS`. El fallo contó igual en `FW VERDICT`, porque `s_fail` se incrementa antes de imprimir.
- **No ejercitado en F0:**
  - MCO1, T5 y ninguna prueba que conduzca SPI1, SPI4 o SPI6 (son de F1 en adelante).
  - PB5 en AF; PA6 y PB4 en AF con una escritura real. Solo hay reclamos; PA5 en AF5 sí se escribe, en CM4.
  - Un `bench_safe()` que encuentre SPI4 con `SPE = 1` de verdad.
  - Revisiones de silicio distintas de V (`REV_ID=0x2003`).
- **La comprobación visual de SB52, SB56 y SB57** queda cubierta por la verificación de J1 del responsable (cabecera del plan). T0c no la sustituye: solo muestra que la red de J1 sigue al pull de PA6 y a PB5.

## Decisiones abiertas usadas

| # | Opción usada | Consultada |
| --- | --- | --- |
| 1 | `SpiBm_Status_t` en los tres tiers (no aplica todavía en F0) | El responsable eligió las opciones por defecto de las 8 (cabecera del plan, 2026-10-03) |
| 2 | J1 en D11–D12 | Ídem; T0c lo comprueba en cada sesión |
| 3 | DIO15 sin cablear | Ídem |
| 4 | CM7: SPI2–SPI6; CM4: SPI1 (en F0 se refleja en los dueños de J1 de la tabla de pasos: AF8 en CPU1, AF5 en CPU2) | Ídem |
| 5 | 2.22.5 en `spi_hal` documentada como limitación (F3) | Ídem |
| 6 | Controles de T12 como "no reproducido" si no fallan (F1) | Ídem |
| 7 | `HAL_SPI_MODULE_ENABLED` solo bajo `SPI_DRIVER_SELFTEST` (F3) | Ídem |
| 8 | Documentos de proceso borrados al cierre | Ídem |

## Tabla de capacidades

Entra en F1 (regla 2 del plan; instrucciones §6). En F0 no existe.

## Respuesta a la auditoría 1 (F0, partes 1 y 2; plan revisión 2)

Auditoría en `AUDIT.md`: parte 1 en `f7dc54f`, parte 2 en `b688884` (B1). La captura del auditor es `docs/spi_f0_audit_bench_153d5b3.txt`. Desde `d9a9df7` el plan está en la revisión 2 (D9), y estas correcciones la siguen. P1 queda resuelto en la revisión 2 (R2-1, R2-2) y se aplica en F1, así que aquí no se contesta.

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F0 | entregada para auditoría (el commit que añade esta sección) | `efc1be8` (código); evidencia en `0e7a7c4` | `03_firmware_templates/prj_01_template_dualcore/docs/spi_f0_bench_efc1be8.txt` |
| F1 | sin empezar | — | — |

### 5.6 Hallazgos

| Hallazgo | Commit que lo corrige | Qué ejecuté para comprobarlo | Estado |
| --- | --- | --- | --- |
| B1: T0B(d) en el host leía MCU_ACK después de `T0B timeout handled`, y el resultado dependía del orden de llegada | `d8f5e69` | Ejecución de entrega en la placa, `docs/spi_f0_bench_efc1be8.txt`: ACK alto 9,870 s sin HOST_REQ (cota 9..11 s), luego bajo en 20/20 lecturas mientras el firmware espera. `STEP T0B_S13` (+13,993 s) llega después de la comprobación del host (+13,990 s), que se imprime tras el pulso de HOST_REQ | corregido en `d8f5e69`, pendiente de verificación |
| M1 (R2-3 del plan): S14 no leía el SPI dueño de J1 | `efc1be8` | Misma ejecución: los 7 casos por núcleo dan `0x60` (esperado `0x60`) en SPI6/CPU1 y en SPI1/CPU2, con `SPE=1` releído 1 en los dos. Cada `bench_safe()` deja su SPI en `CR1=0`, `CFG2=0` y MODF 0. Los reclamos de J1 que deben aceptarse siguen dando `0x0000F` (CPU2 en T0B) y `0x0001F` (CPU1 en T0B_S14) | corregido en `efc1be8`, pendiente de verificación |

**B1, la corrección elegida.** El firmware ya no empieza el paso siguiente por su cuenta. Tras su espera de 10 s y `bench_safe()`, mantiene MCU_ACK bajo y espera un pulso de HOST_REQ (`sync_ack_low()`), acotado por los mismos 10 s. Si no llega, aborta.

El host hace lo siguiente:
1. Espera ACK bajo con un límite de 15 s.
2. Mide cuánto estuvo alto.
3. Lo lee bajo 20 veces en 100 ms.
4. Lee la línea `T0B timeout handled`.
5. Da el pulso, y comprueba que ACK sigue bajo después del flanco de subida.

Elegí esta opción en vez de solo sondear DIO13 en el host. Sondeando, el host tendría que caer en la ventana entre `bench_safe()` y el `sync0()` del paso siguiente, que dura lo que tarda la consola en sacar dos o tres líneas. Seguiría siendo una carrera, solo que más difícil de perder.

Cambia el protocolo (un handshake más en T0B_TIMEOUT), así que `SPI_BENCH_VERSION` y el `BENCH_VERSION` del host pasan a 2. La tabla de pasos, sus nombres y sus dueños no cambian.

**M1/R2-3, lo implementado.**
- `sst_spi_at_rest(cr1, cfg2)`: una función pura en `Common/Src/spi_selftest_common.c`. Comprueba `SPE = 0`, `AFCNTR = 0`, `IOSWP = 0`, y `MASTER = 1` o `CFG2 = 0`. Es una sola copia para SPI4 (S13), SPI6 y SPI1.
- `bench_claim()` de cada núcleo exige, antes de un AF en PA5, PA6, PB4 o PB5, que su SPI tenga reloj en su banco (`RCC_C1->APB4ENR` SPI6EN en CPU1, `RCC_C2->APB2ENR` SPI1EN en CPU2) y esté en reposo.
- `bench_safe()` de cada núcleo suelta su SPI en el orden de la regla 9 de la revisión 2 (`spi_release()`): `IER = 0`, `SPE = 0` conservando `SSI`, `CFG2 = 0`, `CR1 = 0`, `IFCR`. CPU1 suelta SPI4 y SPI6, y CPU2 suelta SPI1.
- `step_end()` también falla si un paso deja SPI6 con `SPE` o `AFCNTR`.
- T0b(f) tiene un caso de rechazo por condición, con los pines de J1 en entrada: `SPE=1`, `AFCNTR=1`, `IOSWP=1`, esclavo con `CFG2≠0` y reloj apagado. Los controles aceptados son "maestro en reposo" y `CFG2=0`. En CPU1 los ejecuta `spi6_cases()` en T0B_S14; en CPU2, `spi1_cases()` en T0B, con el comando nuevo `SST_CMD_SPI1`.

### Build y tamaños en `efc1be8`

Fuentes: `docs/spi_f0_build_all_efc1be8.txt`, `docs/spi_f0_selftest_build_efc1be8.txt`, `docs/spi_f0_werror_efc1be8.txt` y `docs/spi_f0_map_sizes_efc1be8.txt`.

- `build_all -Clean`: 8/8.
- `selftest-spi` desde un directorio vacío: 0 líneas `warning:` o `error:`.
- `-Werror` (`selftest-spi`, `debug`, `release`): configure 0, build 0 y 0 avisos en cada uno.

| Imagen | .text | .data | .bss | Δ frente a 2481396 | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| prj_01 debug CM7 | 29304 | 132 | 55668 | 0 (y 0 en cada sección asignada frente a `8901b77`) | `build/debug/cm7_firmware.map` |
| prj_01 debug CM4 | 3456 | 24 | 12560 | 0 | `build/debug/cm4_firmware.map` |
| prj_01 release CM7 | 28356 | 132 | 55672 | 0 | `build/release/cm7_firmware.map` |
| prj_01 release CM4 | 3112 | 24 | 12560 | 0 | `build/release/cm4_firmware.map` |

| Objeto | Núcleo | .text | .data | .bss | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| `spi_bm.c.obj`, `spi_ll.c.obj`, `spi_hal.c.obj` | — | no existen en F0 (0 objetos de `06_spi` en los dos `.map`) | — | — | `build/selftest-spi/cm{7,4}_firmware.map` |
| `spi_selftest.c.obj` (selftest) | CM7 | 12662 | 4 | 405 | `build/selftest-spi/cm7_firmware.map` |
| `spi_selftest_common.c.obj` (selftest) | CM7 | 661 | 0 | 0 | ídem |
| `spi_selftest.c.obj` (selftest) | CM4 | 1933 | 0 | 0 | `build/selftest-spi/cm4_firmware.map` |
| `spi_selftest_common.c.obj` (selftest) | CM4 | 617 | 0 | 0 | ídem |

Pila de `Status_Task` con el selftest: 272 palabras libres de 512 (`spi_f0_bench_efc1be8.txt`).

### Pruebas en `efc1be8`

Ejecución de entrega, `docs/spi_f0_bench_efc1be8.txt`: arranque en frío, checklist confirmado por Hector y `tree clean`. Rev `efc1be8` e id `b2d94e5e65f27406` en host, CM7 y CM4. Protocolo v2 y `REV_ID=0x2003`. Resultado: 10/10 comprobaciones del host, `FW VERDICT=PASS failures=0` y `HOST VERDICT=PASS`, sin aborto.

Las filas de §5.3 que no aparecen aquí dan los mismos valores que en `008db8b`:
- T0a: `0x4FFF`/`0x0000`, walking 1 sin diferencias, el control marca `0x0200`.
- T0c: control `0x9`/`0x6`, `0x5A3C` leído en PA6.
- T0b(b): 23 rechazos y PA8 AF0 aceptado; CPU2 da `0x0000F` en T0B y `0x00000` entre pasos.
- T0b(c): `0x5A`.
- T0b(e): los mismos valores, incluidos `SR=0x00001002` al empezar y `SPE` releído 1.
- No Digital Out: 0 en las 8 fronteras.

| ID | Tier | Núcleo | Resultado | Valor medido | Control negativo: valor | Oráculo | Captura |
| --- | --- | --- | --- | --- | --- | --- | --- |
| T0b(d) | — | CM7 + host | PASS | Firmware: tras 10 s sin HOST_REQ, `GPIOE MODER[31:4] = 0x00004000`, `AFRH = 0` y J1 `MODER` = 0. Host: ACK alto 9,870 s y luego bajo en 20/20 lecturas mientras el firmware espera | — (es el propio caso negativo) | DIO13 del AD2, `MODER` leído directamente | `spi_f0_bench_efc1be8.txt` |
| T0b(f) R2-3 CPU1 | — | CM7 | PASS | PA6 AF8 frente a SPI6: rechazado con `SPE=1` (releído 1), `AFCNTR=1`, `IOSWP=1`, esclavo con `CFG2≠0` y sin reloj; aceptado `0x60` (esperado `0x60`), GPIO intacto. `bench_safe()` lleva SPI6 de `CR1 0x00001000`, `CFG2 0x04400000` a 0 y 0, `SR 0x00001002` (MODF 0); reloj como antes (0) | Maestro en reposo y `CFG2=0` aceptados (bits 5 y 6) | registros de SPI6 y de RCC leídos directamente; máscara escrita a mano (`SST_SPI_CASES_EXPECT`) | ídem |
| T0b(f) R2-3 CPU2 | — | CM4 | PASS | PA6 AF5 frente a SPI1: los mismos 7 casos, aceptado `0x60` (`0x60`), registros GPIO cambiados `0x00000`, SPI1 devuelto a `CR1=0`, `CFG2=0` y reloj como antes. `bench_safe()` lleva SPI1 de `CR1 0x00001000`, `CFG2 0x04400000` a 0 y 0, `SR 0x00001002` (MODF 0) | Maestro en reposo y `CFG2=0` aceptados | ídem | ídem |
| T0b(f) J1 | — | CM7, CM4 | PASS | CPU1 en T0B_S14, con SPI6 en reset: 16 casos, aceptado `0x0001F` (`0x0001F`), registros intactos. CPU2 en T0B_S14: `0x00000`. CPU1 en T0B: 5 rechazos. T0C: `0x01` | Los aceptados de T0B_S14 y T0C | máscaras escritas a mano | ídem |

### Desviaciones

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| S13: `bench_claim()` lee SPI4 con `SPE = 0`, `AFCNTR = 0` y `MASTER = 1` o `CFG2 = 0` | SPI4 pasa por la misma `sst_spi_at_rest()` que R2-3, así que S13 también exige `IOSWP = 0` | Una sola copia de la condición. Es más estricto | No consultado |
| Protocolo v1 de T0B(d) (como `03_tim`) | Un handshake más, con ACK bajo, al final de T0B_TIMEOUT (protocolo v2) | B1 | Elección que dejaba el auditor; explicada arriba |

### Pendiente

- La captura de la entrega anterior (`spi_f0_bench_008db8b.txt`) y la del auditor (`spi_f0_audit_bench_153d5b3.txt`) son del protocolo v1. No se tocan.
- No empiezo F1 hasta que `AUDIT.md` diga que F0 pasa.

## F1

Plan: revisión 2 (`d9a9df7`). F1 empezó cuando `AUDIT.md` (parte 3, `6a55bb0`) dio F0 por buena.

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F1 | entregada para auditoría (el commit que añade esta sección) | `5c117ce` (código); evidencia en `81ac85c` | `03_firmware_templates/prj_01_template_dualcore/docs/spi_f1_bench_4142921.txt` |
| F2 | sin empezar | — | — |
| F3 | sin empezar | — | — |

Qué hay en F1:

- `01_shared_drivers/06_spi/spi_bm.{h,c}` y el target `stm32h7_driver_spi_bm`.
- Los pasos de F1 del selftest en los dos núcleos (protocolo del banco v3): F1_NOPIN, J1_SPI6, J1_SPI1 y SPI4_AD2.
- Los oráculos del host en `tools/spi_hw_check.py`, y un decodificador SPI nuevo en `tools/ad2.py` (solo funciones añadidas).

La ejecución de entrega es `docs/spi_f1_bench_4142921.txt`: 29/29 comprobaciones del host, `FW VERDICT=PASS failures=0` y `HOST VERDICT=PASS`, sin aborto.

Hay dos cosas que el plan no preveía:

- **Un maestro sin SCK en su AF no termina ninguna transferencia en esta placa.** Por eso T9, T11 y B1 no corren donde decía el plan. El responsable decidió moverlos el 2026-10-03 (§5.5, §5.7).
- **La regla 6 (`DSIZE = 4`) contradice el RM.** Seguí el RM, con la aprobación del responsable del 2026-10-03 (§5.7).

### 5.1 Commits

Rango de la entrega: `6a55bb0..` el commit que añade esta sección, en `driver/spi`.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `30fdad8` | feat(spi): add spi_bm, the bare-metal tier of 06_spi | — |
| `25426f8` | build(spi): add the stm32h7_driver_spi_bm target | — |
| `67e471c` | feat(tools): add an SPI decoder for logic-analyzer captures to ad2.py | — |
| `ef9b464` | test(prj_01): add the spi_bm selftest steps of 06_spi F1 (bench v3) | — |
| `9f8d66f` | test(prj_01): run the F1 steps from spi_hw_check.py (protocol v3) | — |
| `6fd75d4` | docs(prj_01): add the two F1 development captures of 06_spi | — |
| `c41040a` | docs(spi): state in spi_bm.h that a transfer needs its SCK pin in AF | hallazgo propio (§5.7, E2) |
| `35d36e3` | docs(spi): add the F1 evidence at c41040a | — |
| `9c8f597` | test(prj_01): photograph every SPI register in T1 and run T7(b) up to 8192 frames | fallo propio: menos que el plan (§5.8) |
| `4142921` | test(prj_01): add the T5(a) decoder control and the plan's 2-sample tolerances | fallo propio: menos que el plan (§5.8) |
| `81ac85c` | docs(spi): add the F1 evidence at 4142921 | — |
| `5c117ce` | docs(spi): correct six RM0399 line citations in comments | fallo propio (§5.8) |
| (este) | docs(spi): add the F1 section of the implementation report | — |

Notas sobre el orden:

- **La evidencia es de `4142921`.** `5c117ce`, posterior, solo cambia comentarios: `git diff 4142921 5c117ce` son 6 líneas de comentario en `spi_bm.c` y `spi_selftest.c` de CM7.
- **La evidencia de `c41040a` (`35d36e3`) se queda como está.** Es una ejecución que también dio PASS (28/28, `FW VERDICT=PASS`), pero de antes de `9c8f597` y `4142921`. Esos dos commits acercan el selftest y el host a la tabla de pruebas del plan (§5.8). No es la entrega.
- **`6fd75d4` añade dos capturas de desarrollo**, hechas con el árbol sucio sobre `25426f8` (`--allow-dirty`). Son la fuente del hallazgo E2 y de la corrección del oráculo de T5(d) (§5.8).

### 5.2 Build y tamaños

Fuentes: `docs/spi_f1_build_all_4142921.txt`, `docs/spi_f1_selftest_build_4142921.txt`, `docs/spi_f1_werror_4142921.txt` y `docs/spi_f1_map_sizes_4142921.txt`.

- `build_all -Clean` en `4142921`: 8/8.
- `selftest-spi` desde un directorio vacío: 0 líneas `warning:` o `error:`.
- `-Werror` (`selftest-spi`, `debug`, `release`, en directorios de scratch, `CMAKE_C_FLAGS:STRING=-Werror` en cada `CMakeCache.txt`): exit 0 y 0 avisos en los tres.

| Imagen | .text | .data | .bss | Δ frente a 2481396 | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| prj_01 debug CM7 | 29304 | 132 | 55668 | 0 (y 0 en cada sección asignada frente a `8901b77`) | `build/debug/cm7_firmware.map` |
| prj_01 debug CM4 | 3456 | 24 | 12560 | 0 | `build/debug/cm4_firmware.map` |
| prj_01 release CM7 | 28356 | 132 | 55672 | 0 | `build/release/cm7_firmware.map` |
| prj_01 release CM4 | 3112 | 24 | 12560 | 0 | `build/release/cm4_firmware.map` |

| Objeto | Núcleo | .text | .data | .bss | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| `spi_bm.c.obj` | CM7 | 3420 | 0 | 0 | `build/selftest-spi/cm7_firmware.map` |
| `spi_bm.c.obj` | CM4 | 3362 | 0 | 0 | `build/selftest-spi/cm4_firmware.map` |
| `spi_ll.c.obj`, `spi_hal.c.obj` | — | no existen en F1 | — | — | ídem |
| `spi_selftest.c.obj` (selftest) | CM7 | 35609 | 8 | 197775 | `build/selftest-spi/cm7_firmware.map` |
| `spi_selftest_common.c.obj` (selftest) | CM7 | 1349 | 0 | 0 | ídem |
| `dma_bm.c.obj` (02_dma, lo enlaza el selftest) | CM7 | 2064 | 0 | 0 | ídem |
| `dma_hal.c.obj` (02_dma, lo enlaza el selftest) | CM7 | 852 | 0 | 96 | ídem |
| `spi_selftest.c.obj` (selftest) | CM4 | 7147 | 4 | 32829 | `build/selftest-spi/cm4_firmware.map` |
| `spi_selftest_common.c.obj` (selftest) | CM4 | 1305 | 0 | 0 | ídem |
| `dma_bm.c.obj` (02_dma, lo enlaza el selftest) | CM4 | 1974 | 0 | 0 | ídem |

En esta tabla, .text es la suma de las secciones de entrada `.text*` y `.rodata*`.

- **La imagen `selftest-spi`** mide 79752/140/253556 en CM7 y 19596/28/45400 en CM4.
- **El `.bss` del selftest de CM7** son sobre todo sus buffers:
  - `.axi_ram`: `s_tx`/`s_rx` de 4096 palabras, y `s_big_tx`/`s_big_rx` de 65 536 B para las 65 535 tramas de T4;
  - `.d3_ram`: los dos buffers de BDMA de 8192 medias palabras, que llenan los 32 KB;
  - `.dma_buf`: 256 B.
- **El de CM4** son `s_tx`/`s_rx` de 4096 palabras en SRAM2.

### 5.3 Pruebas

Las cifras salen de `docs/spi_f1_bench_4142921.txt`, salvo donde se indica otra captura:

- Rev `4142921` e id `4312466b8fdd8b5e` en host, CM7 y CM4; `tree clean`.
- Checklist confirmado por Hector antes de la ejecución.
- Arranque en frío y sin depurador.
- Protocolo v3; `REV_ID=0x2003`.
- Relojes, según la HAL a partir de `HSE_VALUE`: CPU1 400 MHz; kernel de SPI1-3 200 MHz; SPI4/5 100 MHz; SPI6 100 MHz.

Las filas de F0 (T0a, T0c, T0b) dan los mismos valores que en `efc1be8`. No se repiten.

| ID | Tier | Núcleo | Resultado | Valor medido | Control negativo: valor | Oráculo | Captura |
| --- | --- | --- | --- | --- | --- | --- | --- |
| T1 | BM | CM7 | PASS | 52 casos en SPI2, SPI4, SPI5 y SPI6, 0 con otro estado. Los 4 grupos (sin `Init`; transferencia en `TX_ONLY`; `SPE=1` a mano, releído 1; `AFCNTR=1` vigente con otro `CPOL`/`SSIOP`) dejan la foto intacta: `0xF`. La foto tiene 11 registros por instancia (todos menos `TXDR`/`RXDR`) y los seis de enable de `RCC_C1`/`RCC_C2` | Un `SPI_BM_Init(SPI2)` válido cambia la foto (1) | estado esperado escrito a mano por caso; foto de registros | `spi_f1_bench_4142921.txt` |
| T1 | BM | CM4 | PASS | 35 casos en SPI1, 0 con otro estado; foto intacta en los 4 grupos (`0xF`) | Un `Init` válido cambia la foto (1) | ídem | ídem |
| T2 | BM | CM7 | PASS | SPI2 y SPI3: 40 960 configuraciones cada uno; SPI4, SPI5 y SPI6: 30 720 cada uno (sin 32 bits). `CFG1`/`CFG2`/`CR1` difieren en 0, `Deinit` falla en 0, y `I2SCFGR`/`CRCPOLY`/`UDRDR` fuera de reset en 0 | La imagen esperada con `MASTER` invertido se detecta: 1 de 1 por instancia | `sst_t2_expect()`: posiciones de bit escritas a mano desde RM0399 53.11.1–53.11.4 | ídem |
| T2 | BM | CM4 | PASS | SPI1: 40 960 configuraciones, 0 diferencias, 0 `Deinit` fallidos, 0 fuera de reset | Detectado (1) | ídem | ídem |
| T2 `GetCaps` | BM | CM7 | PASS | SPI1..6 frente a la tabla 435 escrita a mano (32/16 bits, FIFO 16/8 B, campo de kernel, DMAMUX): difieren `0x00` | — | tabla escrita a mano | ídem |
| T3 | BM | CM7 + host | PASS | 202 vectores de `CalcPrescaler` y 102 de `CalcGuardCycles`, recalculados en el host desde las reglas de cálculo del plan: 0 diferencias | El host compara con `MBR + 1`: marca 153 de los 153 vectores OK | Python con enteros (`calc_prescaler`, `calc_guard`) | ídem |
| T4 | BM | CM7 (SPI6) + host | PASS | 896 transferencias en J1 a /8 (12,5 MHz): modos 0–3 × {4, 5, 7, 8, 9, 15, 16} × MSB/LSB × {1, 2, 3, FIFO−1, FIFO, FIFO+1, 255, 4096} × umbral {1, máx}. 0 con estado, discrepancia o `Init`/`Deinit` fallido. La más larga dura 2 098 966 ciclos, dentro de su `max_cycles`. Más 65 535 tramas de 8 bits: estado 0, 0 discrepancias. Host: 897 líneas, CRC-32 de RX = CRC del patrón xorshift32 de la semilla, y cobertura completa en los 7 tamaños | MISO a PB4 con pull-up y pull-down, 255 tramas de 4/8/16 bits: RX = todo unos / todo ceros por CRC. Las discrepancias son las que calcula el host desde la semilla (p. ej. 4 bits arriba 242, 8 bits abajo 253) | cable + CRC-32 en el host | ídem |
| T4 | BM | CM4 (SPI1) + host | PASS | 1408 transferencias a /16 (12,5 MHz), con además {17, 24, 31, 32} bits; 0 fallidas. Host: 1408 líneas, sin diferencias; cobertura completa en los 11 tamaños | PB4 arriba/abajo, 4/8/16/32 bits: las discrepancias coinciden con las del host (p. ej. 32 bits 255/255) | ídem | ídem |
| T5(a) | BM | CM7 (SPI4) + host | PASS | 32 capturas LA a 100 MS/s: modos 0–3 × {4, 8, 13, 16} × MSB/LSB, SS por hardware. El host decodifica MOSI y lo compara con la semilla: 0 fallidas. SCK en reposo = `CPOL`; SS activo en cada flanco de muestreo y en un solo intervalo | Cada captura decodificada con el `CPHA` contrario y con el orden contrario: las dos encuentran discrepancias en 32 de 32 (p. ej. a0: 27 y 25) | `ad2.spi_decode()` en el host | ídem |
| T5(b) | BM | CM7 + host | PASS | `SSIOP=1`: SS activo alto en los flancos y bajo antes de la transferencia; tramas correctas | El caso `SSIOP=0` es (a) | ídem | ídem |
| T5(c) | BM | CM7 + host | PASS | `NSS_HW_PULSE`, `midi` 2: un intervalo de SS activo por trama; tramas correctas | — | ídem | ídem |
| T5(d) | BM | CM7 + host | PASS | Retardo(k) − retardo(0), en muestras, frente a k × el periodo de SCK medido en la misma captura (± 2). `mssi` 1/4/15: 16 / 63 / 236, esperado 15,8 / 63,1 / 237,9. `midi` 1/4/15: 16,0 / 63,2 / 237,0, esperado 15,8 / 63,1 / 237,5. Las 8 capturas decodifican bien | — (el plan no lo pide) | LA, periodo medido dentro de las tramas | ídem |
| T5(e) | BM | CM7 + host | PASS | `keep_pins=1`, SCK con pull-up frente a `CPOL=0`: DIO4 = 0 entre dos transferencias | `keep_pins=0`: DIO4 = 1 (sigue al pull) | DIO del AD2 | ídem |
| T5(f) | BM | CM7 | PASS | Full duplex, PE13 con pull-up: 16 tramas, 0 distintas de `0xFF`. Con pull-down: 0 distintas de `0x00`. Estado 0 | — | RX frente al pull | ídem |
| T5(g) | BM | CM7 + host | PASS | f(SCK)/f(MCO1) para /4../256 frente a 100 MHz / 2^(MBR+1) / 1 MHz, ± 0,5 %: entre −132 y +274 ppm. MCO1 = HSE/8 (`0x00A00000`) durante el paso; `RCC_CFGR` antes `0x0000001B`, después `0x0000001B` | — | MCO1 en DIO14, misma captura | ídem |
| T6 | BM | CM7 (SPI6) | PASS | 8 y 16 bits, {1, FIFO+1, 1024} tramas: 0 fallidas. IRQ hasta el fin: 1/9/1024 (8 bits) y 1/5/1024 (16 bits). (c) 0 IRQ en los 10 ms tras cada fin, con `SPE = 0` | `TXPIE = 1` a mano con `SPE = 0`: 2 IRQ espurias (≥ 1) | ISR de prueba + CRC en el host | ídem |
| T6 | BM | CM4 (SPI1) | PASS | 8/16/32 bits, {1, FIFO+1, 1024}: 0 fallidas; IRQ 1/17/1024, 1/9/1024, 1/5/1024; (c) 0 | 1 IRQ espuria (≥ 1) | ídem | ídem |
| T7(a) | BM | CM4 | PASS | SPI1 con DMA2 streams 0/1 (`dma_bm`), 8/16/32 bits, {1, 2, 17, 255, 4096} tramas: 0 fallidas (CRC también en el host) | `RXDMAEN = 0`, 255 tramas: `WaitEnd` = 8 (`ERR_OVERRUN`), 255 palabras RX con el centinela | cable | ídem |
| T7(b) | BM | CM7 | PASS | SPI6 con BDMA canales 0/1 (`dma_hal`, `BDMA_REQUEST_SPI6_RX/TX`), buffers en `.d3_ram`, 8/16 bits, {1, 9, 255, 8192} tramas: 0 fallidas | `RXDMAEN = 0`: `ERR_OVERRUN`, 255 con centinela | cable | ídem |
| T7(c) | BM | CM7 + host | PASS | SPI4 solo TX con DMA1 stream 0, 8 y 16 bits: 2 capturas decodificadas sin discrepancias | — | LA | ídem |
| T7(d) | BM | CM7 | PASS | `GetDmaRequest` RX/TX de SPI1..6 = `DMA_REQUEST_SPIx_RX/TX` (y `BDMA_REQUEST_SPI6_RX/TX`) de `stm32h7xx_hal_dma.h`; `GetIRQn` = `SPIx_IRQn`. Difieren `0x00` | — | constantes de la HAL y de CMSIS | ídem |
| T8 | BM | CM7 (SPI6) | PASS | 1024 tramas de 8 y 16 bits, /256 … /2, pines `VERY_HIGH`. Pasa en todas (máscara `0xFF`): obligatorio hasta /4 = 25 MHz; registrado /2 = 50 MHz | MISO a PB4 con pull-up a /2: 1021 discrepancias | cable + CRC en el host | ídem |
| T8 | BM | CM4 (SPI1) | PASS | 8 y 32 bits: pasa en todas (máscara `0xFF`); obligatorio hasta /8 = 25 MHz; registrado /4 = 50 MHz y /2 = 100 MHz | PB4 a /2: 1022 discrepancias | ídem | ídem |
| T9 | BM | CM7 (SPI6, SPI4) | PASS | Solo TX, /16 y /64, 8 y 16 bits, t(2000) − t(1000) frente a 1000 × `DSIZE` × 2^(MBR+1) ticks de kernel. SPI6: −179, −74, +3 y −6 ppm. SPI4: −109, −62, −7 y −3 ppm | La misma cuenta con `MBR + 1`: unos −500 000 ppm, fuera | `CYCCNT` | ídem |
| T9 | BM | CM4 (SPI1) | PASS | −335, −453, +7 y −9 ppm | ídem | ídem | ídem |
| T10 | BM | CM7 | PASS | `GetKernelSource` SPI1..6 = 0 0 0 0 0 0; `RCC_D2CCIP1R` y `RCC_D3CCIPR` = 0 leídos directamente | — (el control es T9) | registro leído directamente | ídem |
| T11(a) | BM | CM7 (SPI2) | PASS | `SSI` activo a mano: `SR=0x00001202` (MODF). `CheckTransfer` = 7 (`ERR_MODF`) y MODF sigue puesto. `Transfer` = 7; después `SPE` 0, `MASTER` 0 y MODF borrado. `Deinit` 0 + `Init` 0: `MASTER` 1 y `CheckTransfer` 0 | `SSI` inactivo: `CheckTransfer` 0 y MODF 0 | registros leídos directamente | ídem |
| T11(b) | BM | CM7 (SPI6) | PASS | OVR a mano: 32 tramas con la RxFIFO sin leer. `SR=0x0000905F` (`EOT`, `OVR`); `TakeErrors` = `0x040`; después `SR=0x0000901F` (OVR borrado) | — | registros | ídem |
| T12(a) | BM | CM7 (SPI6) | PASS | 10 000 transferencias de 1 trama a /256 sin pausa: 0 `ERR_TIMEOUT`, 0 erróneas | A mano, `CSTART` con `SPE = 1` justo después de `EOT`: 20 bloqueos en 40 transferencias (reproducido; decisión 6: solo se registra) | `CYCCNT` | ídem |
| T12(b) | BM | CM7 (SPI4) + host | PASS | `CPHA = 0`, /256, 1 trama: último pulso alto de SCK 126 muestras frente a 126,4 (medio periodo medido en la trama), ± 2 | A mano, `SPE = 0` sin guarda: 2 muestras (se registra). En las capturas de desarrollo fue 153 y 151 muestras (§5.8) | LA | ídem |
| T13 | BM | CM7 + CM4 | PASS | SPI2 transmite con DMA1 stream 1 (circular, 64 B), 65 535 tramas a /256. CTSIZE 63583. CPU2 `DisableClock(SPI2)` (`RCC_C2` SPI2EN 0): 30 ms después 60653, sigue. CPU2 `EnableClock` + `DisableClock`: `RCC_C2` 0 y `RCC_C1` 1; 30 ms después 57723, sigue | CPU1 `DisableClock(SPI2)`: los dos bancos a 0; 30 ms después (con el reloj de vuelta) 57723, igual que antes: parado | `CTSIZE` leído por CM7; `RCC_C1`/`RCC_C2` `APB1LENR` | ídem |
| T14 | BM | CM7, CM4 | PASS | `Deinit` 0 y registros iguales a la imagen de reset del RM (`0x00`) en SPI1..6, tras F1_NOPIN y tras cada paso con pines. J1 en entrada (`MODER` `0x00`) en los dos núcleos. S5 pasa con PE11..PE14 aún en AF5 tras el `Deinit` de SPI4, y otra vez con los pines en entrada | Instancia aún inicializada: difiere `0x0D` (SPI2, SPI4, SPI6) | reset de RM0399 53:3188, 3285, 3309, 3442, 3544, 3815, 3902, 3923 escrito a mano | ídem |
| Sin Digital Out | — | host | PASS | Enable de Digital Out releído en las 12 fronteras de paso: `0x0000` en todas | — | `FDwfDigitalOutEnableGet` | ídem |
| Veredicto | — | CM7 + host | PASS | `bench completed without abort`; CM4 `timeout 0, aborts 0`; `FW VERDICT=PASS failures=0`; 29/29; `HOST VERDICT=PASS` | — | — | ídem |

Notas:

- **T4, T6, T7 y T8 tienen dos oráculos.** El firmware compara RX con TX. El host recalcula, línea por línea, el CRC-32 y la cobertura desde la semilla impresa, sin usar lo que dice el firmware.
- **Las comprobaciones de S5 del host** registran PASS porque `buschk()` lanza `BenchAbort` si falla. Pasa lo mismo que en F0.

### 5.4 Medidas (B1)

De `docs/spi_f1_bench_4142921.txt`, con `PRIMASK` puesto durante la medida de `Init` y `Transfer`.

| Función | CM7 (400 MHz) | CM4 (200 MHz) |
| --- | --- | --- |
| `SPI_BM_Init` (en frío, tras `Deinit`) | 453 ciclos (SPI6) | 426 ciclos (SPI1) |
| `SPI_BM_Transfer` de 1 trama a /2, solo TX | 1330 ciclos (SPI6) | 1376 ciclos (SPI1) |
| `SPI_BM_TransferIrq`, por llamada (T6) | mín. 389, máx. 751 (SPI6) | mín. 335, máx. 504 (SPI1) |
| `SPI_BM_CalcPrescaler`, el peor de T3 | 98 ciclos | — |

- **Frecuencia más alta que pasó en T8:** SPI6 a /2 = 50 MHz (8 y 16 bits) y SPI1 a /2 = 100 MHz nominales (8 y 32 bits). Las dos con 1024 tramas por el cable de J1 y pines `VERY_HIGH`. Por encima de 25 MHz solo se registra (D5).
- **Pila de `Status_Task` con el selftest:** high-water mark de 182 palabras libres de 512 (en F0, 272).
- **Tamaños:** §5.2.

### 5.5 Desviaciones del plan

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| Contrato: `guard_cycles` en `SpiBm_Config_t` | `guard_cycles` se pasa donde se aplica: `SPI_BM_Transfer(spi, tx, rx, frames, guard_cycles, max_cycles)`, `SPI_BM_DmaEnd(spi, guard_cycles)` y `SpiBm_Xfer_t.guard_cycles`. Cada uno rechaza 0 (`ERR_PARAM`) | Las funciones que aplican la guarda no reciben la configuración, y C7 prohíbe guardarla en RAM | Sí: el responsable, en el chat, 2026-10-03 ("Pasarla donde se usa") |
| T9: CM7 en SPI2, SPI3 y SPI5, y CM4 en SPI1, "motor sin pines" | T9 en SPI6 y SPI4 (CM7) y en SPI1 (CM4), con sus pines en AF: SPI6 y SPI1 por J1, SPI4 por el bus del AD2 en SPI4_AD2, con `NSS_SOFT` para que PE11 no se conduzca | Sin SCK en AF el maestro no termina (§5.7, E2) | Sí: el responsable, 2026-10-03 ("Move to pinned") |
| T11 en SPI2 (R2-4): `Transfer` → `ERR_MODF`; la recuperación se comprueba con una "transferencia posterior correcta"; (b) en SPI2 | (a) sigue en SPI2. El control y la recuperación se leen con `SPI_BM_CheckTransfer` y `MASTER`, sin transferir. El fallo usa `Transfer`, que vuelve antes de arrancar nada. (b) va en SPI6 por J1, y además exige `EOT` | Igual: en SPI2 ninguna transferencia termina | Sí, en la misma decisión |
| B1: ciclos de `Transfer` de 1 trama (sin instancia fijada) | En SPI6 (CM7) y SPI1 (CM4), con los pines de J1 en AF | Igual | Sí, en la misma decisión |
| T5(d) "retardo(k) − retardo(0) = k periodos ± 2 muestras"; T12(b) "medio periodo ± 2 muestras" | El periodo es el de SCK **medido dentro de las tramas de la misma captura**, no el nominal de 2^(MBR+1) muestras | El HSE de la placa viene del MCO del ST-LINK, ~1,3 % rápido. Con el periodo nominal, `mssi = 15` daba 236 frente a 240 ± 3 en la segunda captura de desarrollo (§5.8) | No consultado |
| T5(a) "flancos de SCK = 2 × DSIZE × tramas"; T5(c) "tramas − 1 pulsos" | (a) Cuento los flancos de muestreo (= `DSIZE` × tramas), que son la mitad de todos. (c) Cuento los intervalos de SS activo con datos (= tramas); entre ellos hay tramas − 1 pulsos inactivos | Son la misma condición contada de otra forma | No consultado |
| T5: captura LA disparada por un flanco de SCK o de SS (el plan no lo fija) | El LA se dispara con el flanco de subida de HOST_REQ, que da el propio host, con 50 muestras de antes. El firmware arranca la transferencia al ver HOST_REQ alto | Un solo disparo vale para los cuatro modos y las dos polaridades de SS. El AD2 no conduce nada más | No consultado |
| `SPI_BM_CheckTransfer` y las llamadas que transfieren | Un MODF ya presente cuando empiezan es `ERR_MODF`. Las llamadas que pueden escribir lo borran (`IFCR.MODFC`); `CheckTransfer` solo lee | Regla 7: "`MODF` → `ERR_MODF`". Sin esto, una transferencia con un MODF pendiente se quedaría en `ERR_BUSY` (`MASTER = 0`) sin decir por qué | No consultado |
| Protocolo v2 | v3: cuatro pasos nuevos en la tabla común. Hasta cuatro argumentos por comando a CPU2 (`cm4_send4`). CPU2 espera hasta 300 s un comando (`SST_CM4_IDLE_TIMEOUT_MS`, antes 60 s). El handshake de cierre de un paso de F1 espera hasta 300 s (`F1_STEP_TIMEOUT_S`) | Los pasos de F1 imprimen durante decenas de segundos a 115200 Bd | No consultado |

### 5.6 Respuesta a la auditoría

Todavía no hay auditoría de F1.

### 5.7 Erratas del plan encontradas

| Sección del plan | Qué dice | Qué es correcto | Fuente (53:<línea>, ES0445 §, página del SDK) |
| --- | --- | --- | --- |
| E1. Regla 6 (tramas, accesos y umbral) y T4 | "Con `DSIZE = 4` un acceso de 8 bits empaqueta dos tramas (53:772-776): el driver las combina por pares" | Las tramas de hasta 8 bits solo se empaquetan con accesos de 16 o 32 bits. Un acceso de 8 bits lleva una trama. La figura 666 muestra cuatro tramas de 4 bits en un acceso de 32 bits, una por byte, y la RxFIFO guarda N tramas de ≤ 8 bits en N bytes. 53:775 ("If the access is a multiple of the configured data size, data packing is applied automatically") es la regla general. Para tramas de ≤ 8 bits, 53:1158-1165 la concreta: solo con accesos de 16 o 32 bits. Leí ese párrafo y la figura como mandatos sobre la regla general. El driver hace un acceso de 8 bits por trama y no empaqueta. T4 prueba `DSIZE = 4` con número par e impar de tramas (1, 2, 3, 7, 8, 9, 255, 4096) | 53:1158-1165; 53:1204-1226 (figura 666); 53:927-929. El responsable aprobó seguir el RM el 2026-10-03 |
| E2. T9 ("motor sin pines"), T11 (R2-4: SPI2 "sin pines", "transferencia posterior correcta"), B1 | Que un maestro sin pines termina sus transferencias | En esta placa (STM32H755, rev V, `REV_ID=0x2003`), un maestro cuyo SCK no está en su AF **nunca pone `EOT` y no recibe nada**. SPI2, SPI3, SPI5 y SPI1 con los pines de J1 en entrada se quedaron en `CTSIZE = 1`, y `Transfer` devolvió `ERR_TIMEOUT` al agotar `max_cycles`. SPI2 envió 31 de 32 tramas a una RxFIFO sin leer y no dio OVR (`SR=0x00010012`). La misma transferencia en SPI4, con sus pines en AF5, terminó. T13 pasó en SPI2 sin pines porque solo mira `CTSIZE`. No está en RM0399 cap. 53 ni en ES0445 | Observación propia: `docs/spi_dev_f1_bench_25426f8-dirty.txt` (19 fallos de firmware, todos de este tipo). Contrato anotado en `spi_bm.h` (`c41040a`) |
| E3. T6(c) | "en reset `SR.TXP = 1`, 53:3597" | 53:3597 es el título de la sección del registro. El valor de reset `0x0000 1002` está en 53:3599. Es una errata de cita, no de contenido | 53:3599 |
| E4. R2-2 | "en esclavo no hay fallo de modo" (el plan cita 53:1474-1475 en la regla) | La frase está en 53:1475-1476. Errata de cita | 53:1475-1476 |

### 5.8 Problemas abiertos y no ejercitado

- **Fallos propios: la primera entrega (`c41040a`) hacía menos de lo que pide la tabla de pruebas.** Lo vi al cruzar fila por fila la tabla del plan con lo ejecutado, antes de entregar. No lo vio ningún auditor. Lo corregí y repetí la ejecución de entrega en `4142921`:
  - T1: la foto no tenía `SR`, `TXCRC` ni `RXCRC` (corregido en `9c8f597`).
  - T7(b): llegaba a 4096 tramas, no a 8192. Lo había reducido para dejar margen en `.d3_ram` (corregido en `9c8f597`).
  - T5(a): faltaba el control del decodificador, con el `CPHA` y el orden contrarios (corregido en `4142921`).
  - T5(d) y T12(b): la tolerancia era ± 3 muestras y no ± 2 (corregido en `4142921`).
  - La captura `spi_f1_bench_c41040a.txt` queda como registro de aquella ejecución.
- **Fallo propio: citas del RM.** Seis comentarios citaban líneas equivocadas de RM0399 (corregido en `5c117ce`; detalle en el mensaje del commit). Uno es el número de tramas de la RxFIFO, que citaba 53:950-953 en vez de 53:927-929. El mensaje de `30fdad8` repite esa cita mala y no se puede cambiar.
- **Margen de T5(d):** `mssi = 15` dio 236 muestras frente a 237,9 ± 2. Pasó por 0,1 muestras. El retardo se cuenta en muestras enteras, y el periodo esperado sale de una media.
- **Control de T12(b), sin guarda:**
  - dos capturas de desarrollo: 153 y 151 muestras;
  - dos ejecuciones de entrega: 2 muestras.
  - Medio periodo es unas 126 muestras. Con 2 muestras el último pulso sale asimétrico (2.22.5 reproducido); con 151–153 es más largo que medio periodo.
  - No sé qué cambia entre unas ejecuciones y otras. El control es "solo registro" (decisión 6).
- **Control de T3:** comprueba que el oráculo distingue en `CalcPrescaler` (`MBR + 1`). En `CalcGuardCycles` no hay un esperado alterado a propósito.
- **No ejercitado en F1:**
  - ninguna transferencia que termine en SPI2, SPI3 o SPI5 (E2). SPI2 solo transmite por DMA en T13, sin `EOT`;
  - la imagen que deja la HAL cruda en T2 (el plan la pone en CM7). Necesita `HAL_SPI_MODULE_ENABLED`, que según la decisión 7 entra en F3;
  - `ERR_VERIFY` y el camino de `CYCCNT` parado (C13). `ERR_TIMEOUT` solo salió sin querer en la primera captura de desarrollo;
  - `MODF` y `OVR` servidos dentro de `SPI_BM_TransferIrq`;
  - `keep_pins` solo en SPI4 con `CPOL = 0`;
  - 65 535 tramas solo en SPI6 con 8 bits por sondeo;
  - MISO de SPI4 con datos reales: el AD2 no conduce (D6), así que la RX de SPI4 solo se ve como el nivel del pull (T5(f));
  - B1 mide `TransferIrq` por llamada, no por tipo de evento;
  - silicio distinto de la revisión V.
- **Árbol sucio en desarrollo:** las dos capturas de `6fd75d4` son de un árbol sin commitear sobre `25426f8`. Las cito solo como fuente de E2 y del cambio de oráculo de T5(d), no como evidencia de la entrega.

### Tabla de capacidades (regla 2 del plan)

Es la de `k_spi[]` en `spi_bm.c`. Cada valor lleva la línea del `.txt` del RM de la que sale, comprobada antes de entregar (RM0399 rev 4: `00_docs_stm32h7/docs/peripherals/`). Ninguna celda difiere del plan.

| Instancia | Bus, bit de enable | Dominio | Trama máx. | FIFO | Kernel | DMAMUX: RX / TX | IRQn | Líneas del RM |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| SPI1 | APB2, bit 12 | D2 | 32 | 16 B | SPI123SEL | 1: 37 / 38 | 35 | 53:56, 53:68-69; 09:7419 (`RCC_APB2ENR`), 09:7542 (SPI1EN); 09:4653; 18:127-128 |
| SPI2 | APB1L, bit 14 | D2 | 32 | 16 B | SPI123SEL | 1: 39 / 40 | 36 | 53:56, 53:68-69; 09:7106 (`RCC_APB1LENR`), 09:7283 (SPI2EN); 09:4653; 18:129-130 |
| SPI3 | APB1L, bit 15 | D2 | 32 | 16 B | SPI123SEL | 1: 61 / 62 | 51 | 53:56, 53:68-69; 09:7106, 09:7277 (SPI3EN); 09:4653; 18:98-99 |
| SPI4 | APB2, bit 13 | D2 | 16 | 8 B | SPI45SEL | 1: 83 / 84 | 84 | 53:56, 53:68-69; 09:7419, 09:7536 (SPI4EN); 09:4643; 18:130-131 |
| SPI5 | APB2, bit 20 | D2 | 16 | 8 B | SPI45SEL | 1: 85 / 86 | 85 | 53:56, 53:68-69; 09:7419, 09:7516 (SPI5EN); 09:4643; 18:132-133 |
| SPI6 | APB4, bit 5 | D3 | 16 | 8 B | SPI6SEL | 2: 11 / 12 (BDMA) | 86 | 53:56, 53:68-69; 09:7577 (`RCC_APB4ENR`), 09:7690 (SPI6EN); 09:4802; 18:233-234 |

- **Tabla 435:** 53:53-71.
- **Kernel:** SPI123SEL `RCC_D2CCIP1R[14:12]`, SPI45SEL `RCC_D2CCIP1R[18:16]`, SPI6SEL `RCC_D3CCIPR[30:28]`.
- **DMAMUX1:** tabla 126 (las peticiones de SPI4/SPI5 están en la segunda columna de 18:130-133). **DMAMUX2:** tabla 130.
- **IRQn:** `stm32h755xx.h`.
- **Contraste en el banco:** T2 `GetCaps` frente a la tabla escrita a mano, y T7(d) frente a las constantes de la HAL y de CMSIS.

### Decisiones abiertas usadas en F1

| # | Opción usada | Consultada |
| --- | --- | --- |
| 1 | `SpiBm_Status_t` en BM | Opción por defecto (cabecera del plan, 2026-10-03) |
| 2 | J1 en D11–D12 | Ídem; T0c en cada sesión |
| 4 | CM7: SPI2–SPI6; CM4: SPI1 | Ídem |
| 6 | Controles de T12 solo registrados: 2.22.1 reproducido (20 bloqueos en 40); 2.22.5, 2 muestras en la entrega | Ídem |

## Respuesta a la auditoría de F1 (`a58172f`; plan revisión 3, `b9de729`)

F1 pasa. La API de `spi_bm` queda congelada con `spi_bm.h` de `b4fa22d`. `git diff b4fa22d -- 01_shared_drivers/06_spi/spi_bm.h` no quita ninguna línea; las dos funciones que añade están en §5.5 de F2. Los hallazgos que la auditoría dejaba para esta entrega:

| Hallazgo | Commit que lo corrige | Qué ejecuté para comprobarlo | Estado |
| --- | --- | --- | --- |
| M2: E2 sin evidencia de entrega; el aborto de C13 sin ejercitar | `d87a576` (prueba T15 de R3-3, con BM y LL) | Ejecución de entrega `docs/spi_f2_bench_73c1b76.txt`, paso J1_SPI6_LL. SPI6 por J1, 1 trama de 8 bits, PA5 en entrada sin pull y PA6/PB5 en AF8. **BM:** `Transfer` = 6 (`ERR_TIMEOUT`) tras 400 942 ciclos (`max_cycles` 400 000; cota +10 %: 440 000). **LL:** igual, tras 401 058 ciclos. En los dos, después: `SPE` 0, `IER` `0x0`, `SR=0x00010002` (`CTSIZE` 1, `TXP`). `Deinit` 0 y los registros iguales a la imagen de reset (`0x00`). **Controles** con PA5 en AF8: `Transfer` 0 tras 3008 (BM) y 3201 (LL) ciclos, y se recibe `0xA5` por J1 | corregido en `d87a576`, pendiente de verificación |
| m1: `wait_from()` sin salida si `CYCCNT` se para a mitad de la llamada | `544fb99` (y la misma función en `spi_ll.c`, `e731a9f`) | Lectura y compilación: el bucle cuenta lecturas iguales de `CYCCNT` como los bucles de `Transfer` y `WaitEnd` (`STALL_READS`, 1000, ahora una sola constante). Si se rinde, `finish()` deshabilita igual (`IER = 0`, `SPE = 0`, `IFCR`) y devuelve `ERR_TIMEOUT`. **No ejercitado en la placa:** parar `CYCCNT` en medio de una guarda necesitaría un depurador o escribir `DWT->CTRL` desde la prueba, y la prueba no lo hace (§5.8 de F2) | corregido en `544fb99`, pendiente de verificación |
| m2: el control de T3 no alteraba ningún esperado de `CalcGuardCycles` (R3-6) | `73c1b76` (host) | Misma ejecución: "T3 control CalcGuardCycles (R3-6)": la comparación con un ciclo de más marca los 83 vectores OK de 83. El control de `CalcPrescaler` sigue marcando 153 de 153 | corregido en `73c1b76`, pendiente de verificación |
| N1: T2 contra la HAL cruda | — | La revisión 3 lo pasa a F3 (R3-4); en F2 no hay nada que hacer | Pasa a F3 |

## F2

Plan: revisión 3 (`b9de729`).

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F2 | entregada para auditoría (el commit que añade esta sección) | `73c1b76` (código); evidencia en `0f05976` | `03_firmware_templates/prj_01_template_dualcore/docs/spi_f2_bench_73c1b76.txt` |
| F3 | sin empezar | — | — |

Qué hay en F2:

- **`spi_ll`:** `01_shared_drivers/06_spi/spi_ll.{h,c}` y el target `stm32h7_driver_spi_ll`, que enlaza `_spi_bm`.
- **Dos funciones añadidas a `spi_bm.h`:** `SPI_BM_CheckDmaPrepare` y `SPI_BM_CheckDmaStart` (§5.5).
- **m1** de la auditoría de F1.
- **Selftest:** cuatro pasos nuevos (protocolo v4), con T1, T2, T4, T5(a), T6, T7(a)(b), T14 y B1 por `spi_ll` en los dos núcleos, y T15 con BM y LL.
- **Host:** los oráculos de los pasos nuevos, y el control de T3 para `CalcGuardCycles`.

La ejecución de entrega es `docs/spi_f2_bench_73c1b76.txt`: 38/38 comprobaciones del host, `FW VERDICT=PASS failures=0` y `HOST VERDICT=PASS`, sin aborto. En la misma sesión pasan otra vez todas las pruebas de F0 y F1.

### 5.1 Commits

Rango de la entrega: `b9de729..` el commit que añade esta sección, en `driver/spi`.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `544fb99` | fix(spi): give wait_from() the stalled-CYCCNT exit of the other waits (m1) | m1 |
| `14ea027` | feat(spi): add SPI_BM_CheckDmaPrepare and SPI_BM_CheckDmaStart | — (§5.5) |
| `e731a9f` | feat(spi): add spi_ll, the LL tier of 06_spi, and its target | — |
| `d87a576` | test(prj_01): add the spi_ll selftest steps of 06_spi F2 (bench v4) | M2 (T15) |
| `73c1b76` | test(prj_01): run the F2 steps from spi_hw_check.py (protocol v4) | m2 |
| `0f05976` | docs(spi): add the F2 evidence at 73c1b76 | — |
| (este) | docs(spi): add the F2 section and the reply to the F1 audit | — |

Hubo una ejecución de desarrollo, con el árbol sucio sobre `e731a9f`, antes de `d87a576`. Dio los mismos resultados (38/38, `FW VERDICT=PASS`). No está commiteada: ninguna conclusión se apoya en ella.

### 5.2 Build y tamaños

Fuentes: `docs/spi_f2_build_all_73c1b76.txt`, `docs/spi_f2_selftest_build_73c1b76.txt`, `docs/spi_f2_werror_73c1b76.txt` y `docs/spi_f2_map_sizes_73c1b76.txt`.

- `build_all -Clean` en `73c1b76`: 8/8.
- `selftest-spi` desde un directorio vacío: 0 líneas `warning:` o `error:`.
- `-Werror` (`selftest-spi`, `debug`, `release`, en directorios de scratch, `CMAKE_C_FLAGS:STRING=-Werror` en cada `CMakeCache.txt`): exit 0 y 0 avisos en los tres.
- `spi_bm.c` y `spi_ll.c` sueltos, en CM7 y CM4, con `-O2` y `-Og`, compilan sin avisos con `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Wcast-align -Wundef -Werror` (cabeceras de CMSIS y de la HAL como `-isystem`). Esta compilación no está en una captura: la cito porque así se comprobó, no como evidencia.

| Imagen | .text | .data | .bss | Δ frente a 2481396 | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| prj_01 debug CM7 | 29304 | 132 | 55668 | 0 (y 0 en cada sección asignada frente a `8901b77`) | `build/debug/cm7_firmware.map` |
| prj_01 debug CM4 | 3456 | 24 | 12560 | 0 | `build/debug/cm4_firmware.map` |
| prj_01 release CM7 | 28356 | 132 | 55672 | 0 | `build/release/cm7_firmware.map` |
| prj_01 release CM4 | 3112 | 24 | 12560 | 0 | `build/release/cm4_firmware.map` |

| Objeto | Núcleo | .text | .data | .bss | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| `spi_bm.c.obj` | CM7 | 3540 | 0 | 0 | `build/selftest-spi/cm7_firmware.map` |
| `spi_ll.c.obj` | CM7 | 2400 | 0 | 0 | ídem |
| `spi_bm.c.obj` | CM4 | 3502 | 0 | 0 | `build/selftest-spi/cm4_firmware.map` |
| `spi_ll.c.obj` | CM4 | 2376 | 0 | 0 | ídem |
| `spi_hal.c.obj` | — | no existe en F2 | — | — | — |
| `spi_selftest.c.obj` (selftest) | CM7 | 44525 | 8 | 197779 | `build/selftest-spi/cm7_firmware.map` |
| `spi_selftest_common.c.obj` (selftest) | CM7 | 2017 | 0 | 0 | ídem |
| `spi_selftest.c.obj` (selftest) | CM4 | 10151 | 4 | 32833 | `build/selftest-spi/cm4_firmware.map` |
| `spi_selftest_common.c.obj` (selftest) | CM4 | 1973 | 0 | 0 | ídem |

.text es la suma de las secciones de entrada `.text*` y `.rodata*`.

- `spi_bm.c.obj` creció de 3420/3362 (F1) a 3540/3502 por m1 y las dos funciones de comprobación.
- La imagen `selftest-spi` mide 91816/140/253556 en CM7 y 25788/28/45400 en CM4.

### 5.3 Pruebas

Las cifras salen de `docs/spi_f2_bench_73c1b76.txt`:

- Rev `73c1b76` e id `42089a7905c54ab9` en host, CM7 y CM4; `tree clean`.
- Checklist confirmado por Hector antes de la ejecución; arranque en frío y sin depurador.
- Protocolo v4; `REV_ID=0x2003`.

Las filas de F0 y F1 dan otra vez PASS en la misma ejecución; aquí solo van las de F2.

| ID | Tier | Núcleo | Resultado | Valor medido | Control negativo: valor | Oráculo | Captura |
| --- | --- | --- | --- | --- | --- | --- | --- |
| T1 | LL | CM7 | PASS | 48 casos por `spi_ll` en SPI2, SPI4, SPI5 y SPI6, 0 con otro estado. Incluyen: constante LL desconocida en cada campo (polaridad, fase, orden, polaridad de SS, prescaler, ancho, umbral, `MSSI`, `MIDI`); `SIMPLEX_RX`, `HALF_DUPLEX_TX` y `NSS_HARD_INPUT`; `nss_pulse` y `MSSI` con `NSS_SOFT`; 3 bits; 17 bits en SPI4/5/6 (`ERR_UNSUPPORTED`); umbral por encima de media FIFO (`ERR_RANGE`); los parámetros de `Transfer`, `StartIt`, `DmaPrepare`, `DmaStart`, `WaitEnd` y `DmaEnd`; `SPE = 1` y `AFCNTR = 1` vigente (`ERR_BUSY`). Foto de los 11 registros (todos menos `TXDR`/`RXDR`) de las cuatro instancias y de los seis enable de RCC, intacta en los 4 grupos (`0xF`) | Un `SPI_LL_Init(SPI2)` válido cambia la foto (1) | estado esperado escrito a mano por caso | `spi_f2_bench_73c1b76.txt` |
| T1 | LL | CM4 | PASS | 40 casos en SPI1, 0 con otro estado; foto intacta (`0xF`) | Un `SPI_LL_Init` válido cambia la foto (1) | ídem | ídem |
| T2 | LL | CM7 | PASS | SPI2 y SPI3: 40 960 configuraciones cada uno; SPI4, SPI5 y SPI6: 30 720 cada uno. La imagen de `spi_ll` (`CR1`, `CR2`, `CFG1`, `CFG2`, `IER`, `CRCPOLY`, `UDRDR`, `I2SCFGR`) difiere de la de `spi_bm` en 0, y `CFG1`/`CFG2`/`CR1` de la tabla del RM en 0. 0 `Init` y 0 `Deinit` fallidos | La misma comparación con `MASTER` invertido en la foto LL lo detecta (1 de 1 por instancia) | imagen de `spi_bm` de la misma configuración (A24) y `sst_t2_expect()` | ídem |
| T2 | LL | CM4 | PASS | SPI1: 40 960 configuraciones; 0 frente al RM, 0 frente a `spi_bm`, 0 fallos de `Init`/`Deinit` | Detectado (1) | ídem | ídem |
| T3 control (R3-6) | BM | CM7 + host | PASS | — | Esperado de `CalcGuardCycles` alterado (un ciclo de más): la comparación marca los 83 vectores OK de 83 | Python con enteros | ídem |
| T4 | LL | CM7 (SPI6) + host | PASS | 896 transferencias por J1 a /8 (12,5 MHz), con la matriz de F1. 0 con estado, discrepancia o `Init`/`Deinit` fallido; la más larga, 2 099 269 ciclos, dentro de su `max_cycles`. 65 535 tramas de 8 bits: estado 0, 0 discrepancias. Host: 897 líneas con CRC-32 y cobertura de la semilla, 0 diferencias; cobertura completa en los 7 tamaños | MISO a PB4 con pull-up y pull-down, 4/8/16 bits: RX todo unos / todo ceros por CRC. Discrepancias = las del host (p. ej. 4 bits arriba 234, 16 bits abajo 255) | cable + CRC-32 en el host | ídem |
| T4 | LL | CM4 (SPI1) + host | PASS | 1408 transferencias a /16 (12,5 MHz), con además 17/24/31/32 bits; 0 fallidas; la más larga, 2 098 698 ciclos. Host: 0 diferencias; cobertura completa en los 11 tamaños | PB4 arriba/abajo, 4/8/16/32 bits: discrepancias = las del host (p. ej. 8 bits arriba 251) | ídem | ídem |
| T5(a) | LL | CM7 (SPI4) + host | PASS | 32 capturas LA a 100 MS/s: modos 0–3 × {4, 8, 13, 16} × MSB/LSB, SS por hardware, por `spi_ll`. El host las decodifica contra la semilla: 0 fallidas; SCK en reposo = `CPOL`; SS activo en un solo intervalo con datos | Cada captura decodificada con el `CPHA` contrario y con el orden contrario: las dos encuentran discrepancias en 32 de 32 (p. ej. A0: 30 y 25) | `ad2.spi_decode()` en el host | ídem |
| T6 | LL | CM7 (SPI6) | PASS | 8 y 16 bits, {1, FIFO + 1, 1024} tramas: 0 fallidas. IRQ hasta el fin: 1/9/1024 y 1/5/1024. (c) 0 IRQ en los 10 ms tras cada fin, con `SPE = 0` | `TXPIE = 1` a mano con `SPE = 0`: 2 IRQ espurias (≥ 1) | ISR de prueba + CRC en el host | ídem |
| T6 | LL | CM4 (SPI1) | PASS | 8/16/32 bits, {1, FIFO + 1, 1024}: 0 fallidas. IRQ: 1/17/823 (8 bits: con LL en CM4 una IRQ sirve a veces más de un paquete), 1/9/1024 y 1/5/1024. (c) 0 | El control `TXPIE` a mano no se repite con LL: no pasa por el driver; el de F1, en la misma ejecución, da 1 | ídem | ídem |
| T7(a) | LL | CM4 | PASS | SPI1 con DMA2 streams 0/1, 8/16/32 bits, {1, 2, 17, 255, 4096} tramas, por `spi_ll`: 0 fallidas (CRC también en el host) | `RXDMAEN = 0`, 255 tramas: `WaitEnd` = 8 (`ERR_OVERRUN`), 255 palabras con el centinela | cable | ídem |
| T7(b) | LL | CM7 | PASS | SPI6 con BDMA canales 0/1, buffers en `.d3_ram`, 8/16 bits, {1, 9, 255, 8192} tramas, por `spi_ll`: 0 fallidas | `RXDMAEN = 0`: `ERR_OVERRUN`, 255 con centinela | cable | ídem |
| T14 | LL | CM7, CM4 | PASS | Tras un `SPI_LL_Init`, `SPI_BM_Deinit` deja SPI2..6 (F2_NOPIN), SPI6 (J1_SPI6_LL), SPI4 (SPI4_AD2_LL) y SPI1 (CM4) iguales a la imagen de reset (`0x00`). J1 en entrada en los dos núcleos. S5 pasa con PE11..PE14 en AF5 tras el `Deinit`, y en entrada | Tras `SPI_LL_Init` y antes del `Deinit`: SPI6 `0x0D`, SPI4 `0x0D` (≠ 0) | reset de RM0399 escrito a mano | ídem |
| T15 | BM y LL | CM7 (SPI6) | PASS | PA5 (SCK) en entrada: `ERR_TIMEOUT` tras 400 942 (BM) y 401 058 (LL) ciclos, frente a `max_cycles` 400 000 (+10 %: 440 000); después `SPE` 0, `IER` 0, `SR=0x00010002`; `Deinit` 0 e imagen de reset | PA5 en AF8: `OK` tras 3008 (BM) y 3201 (LL) ciclos, y `0xA5` recibido por J1 | registros leídos directamente; `CYCCNT` | ídem |
| Sin Digital Out | — | host | PASS | Enable releído en las 16 fronteras de paso: `0x0000` en todas | — | `FDwfDigitalOutEnableGet` | ídem |
| Veredicto | — | CM7 + host | PASS | `bench completed without abort`; CM4 `timeout 0, aborts 0`; `FW VERDICT=PASS failures=0`; 38/38; `HOST VERDICT=PASS` | — | — | ídem |

**El oráculo de las pruebas LL.** La configuración LL de cada prueba sale de `sst_ll_of()` (`Common/Src/spi_selftest_common.c`), con las constantes LL escritas a mano en la prueba, no de la traducción de `spi_ll.c`. Esa traducción es justo lo que prueban T1 y T2. T2 compara además la imagen LL con la de `spi_bm` para la misma configuración, y con la tabla del RM.

### 5.4 Medidas (B1)

De `docs/spi_f2_bench_73c1b76.txt`. `Init` y `Transfer` se miden con `PRIMASK` puesto. La instancia es SPI6 en CM7 y SPI1 en CM4 (R3-3), con los pines de J1 en AF. `Transfer` es 1 trama a /2, solo TX.

| Función | BM, CM7 (400 MHz) | LL, CM7 | BM, CM4 (200 MHz) | LL, CM4 |
| --- | --- | --- | --- | --- |
| `Init` | 530 ciclos | 850 | 473 | 891 |
| `Transfer` de 1 trama | 1569 | 1901 | 1446 | 1604 |
| `TransferIrq` por llamada (T6) | mín. 413, máx. 808 | mín. 520, máx. 967 | mín. 354, máx. 543 | mín. 354, máx. 678 |

- **Diferencias con F1:** las cifras de BM no son las de la captura de F1 (`Init` 453 en `4142921`) porque la imagen cambió. Las dos columnas de este B1 salen de la misma imagen y la misma ejecución.
- **Por qué LL es más lento:** el preset `selftest-spi` es Debug (`-Og`) y no expande del todo las inline de LL. No lo he medido en `-O2`.
- `CalcPrescaler`, el peor de T3: 145 ciclos.
- Pila de `Status_Task` con el selftest: high-water mark de 128 palabras libres de 512 (F1: 182; F0: 272), y baja con cada fase (§5.8).

### 5.5 Desviaciones del plan

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| Contrato de `spi_ll`: "valida llamando a `SPI_BM_Check*`"; la API de `spi_bm` está congelada desde `b4fa22d` (solo se añade) | Añadí a `spi_bm.h` `SPI_BM_CheckDmaPrepare` y `SPI_BM_CheckDmaStart` (`14ea027`): las comprobaciones que ya hacían `DmaPrepare` y `DmaStart`, que ahora las llaman. `spi_ll` valida con ellas sus llamadas de DMA | Sin ellas, las comprobaciones de DMA tendrían una segunda copia en `spi_ll` | **No pregunté antes, y §7 lo pide** ("una función pública que no está en el contrato del plan"). Me di cuenta al escribir este informe. El responsable lo aprobó después, el 2026-10-04, en el chat ("Keep them") |
| Contrato de `spi_ll`: "Mismas operaciones que `spi_bm` (`SPI_LL_*`)" | `SPI_LL_CheckConfig`, `_Init`, `_Transfer`, `_TransferStartIt`, `_TransferIrq`, `_DmaPrepare`, `_DmaStart`, `_WaitEnd` y `_DmaEnd`. Reloj, `Deinit`, `TakeErrors`, `GetIRQn`, `GetDmaRequest`, `GetCaps`, `GetKernelSource`, `CheckTransfer` y las funciones puras se usan de `spi_bm`, sin envoltorio LL | Es lo que dice el plan para esas funciones ("se usan de `spi_bm`") | No consultado |
| "Una constante LL desconocida se convierte en un valor BM fuera de rango" | Los campos LL de dos valores (polaridad, fase, orden, polaridad de SS) son `bool` o un bit en `spi_bm`, que no tienen valor fuera de rango. Una constante desconocida en cualquiera de ellos se convierte en `mode = 4` (`ERR_PARAM` en el sitio de los errores de campo). `nss_pulse` con `NSS_SOFT`, y `NSS_HARD_INPUT`, se convierten en `nss = 3` | Es la forma de dar `ERR_PARAM` en el orden de C6 sin otra validación en LL | No consultado |
| "Escribe con funciones `static inline` de la cabecera LL (y `LL_SPI_WriteReg`/`ReadReg` donde ninguna inline escribe el registro entero)" | `CR1`, `CFG1`, `CFG2`, `IER` e `IFCR` con `LL_SPI_WriteReg`, incluido el segundo `CFG2` con `AFCNTR` (C11). Nunca uso `LL_SPI_ClearFlag_*`: son `SET_BIT` sobre `IFCR`, un RMW de un registro de solo escritura. El resto con las inline | Ninguna inline escribe esos registros enteros, y escribir `CFG2` campo a campo dejaría estados intermedios (p. ej. `MASTER` antes que `SSM`: MODF) | No consultado |
| T6(c), control: "`TXPIE = 1` a mano con `SPE = 0`" | Con LL se repite en CM7 (SPI6, 2 IRQ espurias), pero no en CM4: el control no pasa por el driver, y el de F1 corre en la misma sesión (1 IRQ) | Es independiente del tier | No consultado |
| B1 en F2 | Las líneas de B1 de CM7 dicen ahora `Init` y `Transfer`, sin `SPI_BM_`, porque la misma función mide los dos tiers | — | No consultado |
| Encargo de F2: "sube `SPI_BM_BENCH_VERSION`" | La macro se llama `SPI_BENCH_VERSION` (`Common/Inc/spi_selftest.h`), y la del host `BENCH_VERSION`. Las dos pasan a 4 | No hay ninguna `SPI_BM_BENCH_VERSION` | — |

### 5.6 Respuesta a la auditoría

La de F1 está arriba, en su propia sección. De F2 todavía no hay auditoría.

### 5.7 Erratas del plan encontradas

Ninguna nueva en F2.

### 5.8 Problemas abiertos y no ejercitado

- **Fallo propio de proceso:** las dos funciones de `spi_bm.h` se añadieron sin la pregunta de §7 (§5.5).
- **m1 no está ejercitado en la placa.** La prueba no para `CYCCNT` en medio de una guarda (no se escribe `DWT->CTRL` desde la prueba, y no hay depurador).
- **La pila de `Status_Task` baja con cada fase:** 272, 182 y 128 palabras libres de 512. Las funciones de F2 con más pila son `f2_t1` (dos fotos de 50 palabras) y `t2ll_one` (dos de 8). Si F3 añade otro tanto, quedará poco margen. El tamaño de la pila es de producción y no lo toco (§2.2).
- **T6 en CM4 con LL:** con 1024 tramas de 8 bits hubo 823 IRQ, no 1024. Cada IRQ sirve todos los paquetes que hay; con LL en CM4 a `-Og` a veces se juntan. T6(b) solo pide registrarlo.
- **B1 de LL frente a BM:** medido solo en Debug (`-Og`).
- **No ejercitado en F2:**
  - `SPI_LL_TransferIrq` con `MODF` u `OVR`;
  - `ERR_VERIFY` de LL;
  - `keep_pins` con LL fuera de T2 y T1 (sin pines);
  - T5(b)–(g), T8, T9, T11, T12 y T13 con LL (el plan no los pide en F2).
- **Lo que sigue de F1:** sin transferencias que terminen en SPI2, SPI3 y SPI5 (R3-3); solo silicio rev V.

### Constantes y funciones de `stm32h7xx_ll_spi.h` que usa `spi_ll`

Cabecera del Cube con el que compila `prj_01` (`CUBE_DIR` → `02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7`, `STM32H7xx_HAL_Driver` en `e3c518e`). Es idéntica, byte a byte, a la copia de `00_docs_stm32h7`. Las líneas son las del `#define` o de la cabecera de la función.

| Constante o función | Línea | Uso |
| --- | --- | --- |
| `LL_SPI_MODE_MASTER` | 187 | `CFG2` |
| `LL_SPI_SS_LEVEL_HIGH` / `_LOW` | 196 / 197 | `CR1.SSI` inactivo |
| `LL_SPI_SS_IDLENESS_00CYCLE` … `_15CYCLE` | 205–220 | `ss_idleness` (`MSSI`) |
| `LL_SPI_ID_IDLENESS_00CYCLE` … `_15CYCLE` | 229–244 | `interdata_idleness` (`MIDI`) |
| `LL_SPI_PROTOCOL_MOTOROLA` | 291 | `CFG2` (`SP = 000`) |
| `LL_SPI_PHASE_1EDGE` / `_2EDGE` | 300 / 301 | `clock_phase` |
| `LL_SPI_POLARITY_LOW` / `_HIGH` | 309 / 310 | `clock_polarity` |
| `LL_SPI_NSS_POLARITY_LOW` / `_HIGH` | 318 / 319 | `nss_polarity` |
| `LL_SPI_BAUDRATEPRESCALER_DIV2` … `_DIV256` | 327–334 | `baudrate` |
| `LL_SPI_LSB_FIRST` / `LL_SPI_MSB_FIRST` | 342 / 343 | `bit_order` |
| `LL_SPI_FULL_DUPLEX` / `LL_SPI_SIMPLEX_TX` | 351 / 352 | `transfer_direction` |
| `LL_SPI_DATAWIDTH_4BIT` … `_32BIT` | 363–396 | `data_width` |
| `LL_SPI_FIFO_TH_01DATA` … `_16DATA` | 405–420 | `fifo_threshold` |
| `LL_SPI_CRC_8BIT` | 445 | `CFG1.CRCSIZE = 00111` (ES0445 2.22.4) |
| `LL_SPI_NSS_SOFT` / `LL_SPI_NSS_HARD_OUTPUT` | 483 / 485 | `nss` |
| `LL_SPI_WriteReg` / `LL_SPI_ReadReg` | 521 / 529 | `CR1`, `CFG1`, `CFG2`, `IER`, `IFCR`; read-back |
| `LL_SPI_Enable` / `_Disable` / `_IsEnabled` | 555 / 567 / 578 | `SPE` (bajo PRIMASK) |
| `LL_SPI_SetTransferSize` / `_GetTransferSize` | 799 / 811 | `TSIZE` y read-back |
| `LL_SPI_StartMasterTransfer` | 997 | `CSTART` (bajo PRIMASK) |
| `LL_SPI_GetTransferDirection` | 1324 | full duplex o no |
| `LL_SPI_GetDataWidth` / `_GetFIFOThreshold` | 1437 / 1493 | tamaño del elemento, paquete |
| `LL_SPI_IsActiveFlag_RXP` / `_TXP` / `_EOT` / `_MODF` / `_OVR` | 1698 / 1709 / 1731 / 1775 / 1786 | servicio y fin |
| `LL_SPI_EnableDMAReq_RX` / `_DisableDMAReq_RX` / `_IsEnabledDMAReq_RX` | 2356 / 2367 / 2378 | `RXDMAEN` |
| `LL_SPI_EnableDMAReq_TX` / `_DisableDMAReq_TX` / `_IsEnabledDMAReq_TX` | 2389 / 2400 / 2411 | `TXDMAEN` |
| `LL_SPI_ReceiveData8` / `16` / `32` | 2450 / 2461 / 2477 | `RXDR` con el ancho del elemento |
| `LL_SPI_TransmitData8` / `16` / `32` | 2489 / 2501 / 2518 | `TXDR` con el ancho del elemento |

En el selftest se usan además `LL_SPI_SIMPLEX_RX` (353), `LL_SPI_HALF_DUPLEX_TX` (355) y `LL_SPI_NSS_HARD_INPUT` (484), como casos de rechazo de T1.

Sin usar a propósito: `LL_SPI_ClearFlag_*` (desde la línea 1878), porque son `SET_BIT` sobre `IFCR`, y los setters de campo de `CFG2` (`LL_SPI_SetNSSMode`, `LL_SPI_EnableNSSPulseMgt`, `LL_SPI_EnableGPIOControl`…), porque escribirían `CFG2` por partes.

## Respuesta a la auditoría de F2 (`ea8430b`)

| ID | Qué pedía | Qué hice | Estado |
| --- | --- | --- | --- |
| m3 | La pila de `Status_Task` baja con cada fase (128 palabras libres de 512 en F2); D11, aprobada el 2026-10-04 | `dbab128`: con `SPI_DRIVER_SELFTEST`, `STATUS_TASK_STACK_WORDS` es `1024U` (`app_main.c:54-55`), con el mismo mecanismo que `GPIO_DRIVER_SELFTEST` (`app_main.c:53`); la rama de producción sigue en `512U` (`app_main.c:57`). Comprobado con la captura de entrega de F3: `Status_Task stack high-water mark 640 words`, y con las imágenes de producción iguales a `8901b77` en cada sección asignada (`docs/spi_f3_map_sizes_efb4a3f.txt`) | corregido en `dbab128`, pendiente de verificación |

## F3

Plan: revisión 3 (`b9de729`).

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F3 | entregada para auditoría (el commit que añade esta sección) | `efb4a3f` (código probado en la placa); `4e96c69` solo corrige tres números de línea en comentarios; evidencia en `5f9f1f3` | `03_firmware_templates/prj_01_template_dualcore/docs/spi_f3_bench_efb4a3f.txt` |

Qué hay en F3:

- **`spi_hal`:** `01_shared_drivers/06_spi/spi_hal.{h,c}` y el target `stm32h7_driver_spi_hal`, que enlaza `_spi_bm`. Seis funciones: `SPI_HAL_Init`, `_Transfer`, `_StartTransferIt`, `_StartTransferDma`, `_Wait` y `_Deinit`, sobre `SPI_HandleTypeDef`. Sin el módulo `HAL_SPI_MODULE_ENABLED` la cabecera no declara nada y `spi_hal.c` compila a nada; con `USE_SPI_CRC != 0` da `#error`.
- **Build de `prj_01`:** `HAL_SPI_MODULE_ENABLED`, su `#include` y `USE_HAL_SPI_REGISTER_CALLBACKS 0U` en `CM7/Core/Inc/stm32h7xx_hal_conf.h`, los tres solo bajo `SPI_DRIVER_SELFTEST`. CM4 no habilita la HAL de SPI. La pila de `Status_Task` de D11.
- **Selftest:** tres pasos nuevos de CPU1 (protocolo v5): `F3_NOPIN` (T1, T2, T14), `J1_SPI6_HAL` (T4, T6, T7(b), B1, T14) y `SPI4_AD2_HAL` (T5(a), T7(c), T14). Líneas `TICK` con `HAL_GetTick()`.
- **Host:** los oráculos de los pasos nuevos, el decodificador para las capturas `H` y `Q`, y la comprobación de las líneas `TICK`.

La ejecución de entrega es `docs/spi_f3_bench_efb4a3f.txt`: 45/45 comprobaciones del host, `FW VERDICT=PASS failures=0` y `HOST VERDICT=PASS`, sin aborto. En la misma sesión pasan otra vez todas las pruebas de F0, F1 y F2.

### 5.1 Commits

Rango de la entrega: `5d6f21a..` el commit que añade esta sección, en `driver/spi`.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `4e147b5` | feat(spi): add spi_hal, the HAL tier of 06_spi, and its target | — |
| `dbab128` | build(prj_01): enable the HAL SPI module and a larger Status_Task stack under the SPI selftest | m3 (D11) |
| `3c23568` | fix(spi): match spi_hal's close read-back and DMA checks to the HAL | — (§5.8, ejecuciones de desarrollo) |
| `b9b6237` | test(prj_01): add the F3 tests of 06_spi (spi_hal on CPU1), bench protocol v5 | — |
| `efb4a3f` | docs(prj_01): add the three F3 development captures of 06_spi | — |
| `5f9f1f3` | docs(spi): add the F3 evidence at efb4a3f | — |
| `4e96c69` | docs(spi): correct three stm32h7xx_hal_spi.c line citations in comments | — |
| (este) | docs(spi): add the F3 section and the reply to the F2 audit | — |

- **Las ejecuciones de desarrollo sí están commiteadas** (`efb4a3f`), porque dos conclusiones de esta sección se apoyan en ellas (§5.8): `docs/spi_dev_f3_bench_dbab128-dirty.txt`, `_2.txt` y `_3.txt`, con el árbol sucio sobre `dbab128`.
- **`4e96c69` va después de la ejecución de entrega.** Solo cambia tres comentarios (`git diff efb4a3f 4e96c69 -- '*.c'`: tres líneas, todas de comentario). El mensaje de `3c23568` repite dos de esos números mal: dice l. 3946 donde es l. 3948, y l. 2446 donde es l. 2447. Los mensajes de commit no se reescriben; la corrección queda aquí.

### 5.2 Build y tamaños

Fuentes: `docs/spi_f3_build_all_efb4a3f.txt`, `docs/spi_f3_selftest_build_efb4a3f.txt`, `docs/spi_f3_werror_efb4a3f.txt` y `docs/spi_f3_map_sizes_efb4a3f.txt`.

- `build_all -Clean` en `efb4a3f`: 8/8.
- `selftest-spi` desde un directorio vacío: 0 líneas `warning:` o `error:`.
- `-Werror` (`selftest-spi`, `debug`, `release`, en directorios de scratch, `CMAKE_C_FLAGS:STRING=-Werror` en cada `CMakeCache.txt`): exit 0 y 0 avisos en los tres.

| Imagen | .text | .data | .bss | Δ frente a 2481396 | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| prj_01 debug CM7 | 29304 | 132 | 55668 | 0 (y 0 en cada sección asignada frente a `8901b77`) | `build/debug/cm7_firmware.map` |
| prj_01 debug CM4 | 3456 | 24 | 12560 | 0 | `build/debug/cm4_firmware.map` |
| prj_01 release CM7 | 28356 | 132 | 55672 | 0 | `build/release/cm7_firmware.map` |
| prj_01 release CM4 | 3112 | 24 | 12560 | 0 | `build/release/cm4_firmware.map` |

| Objeto | Núcleo | .text | .data | .bss | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| `spi_bm.c.obj` | CM7 | 3540 | 0 | 0 | `build/selftest-spi/cm7_firmware.map` |
| `spi_ll.c.obj` | CM7 | 2400 | 0 | 0 | ídem |
| `spi_hal.c.obj` | CM7 | 1158 | 0 | 0 | ídem |
| `spi_bm.c.obj` | CM4 | 3502 | 0 | 0 | `build/selftest-spi/cm4_firmware.map` |
| `spi_ll.c.obj` | CM4 | 2376 | 0 | 0 | ídem |
| `spi_hal.c.obj` | CM4 | no enlazado (CM4 no habilita la HAL de SPI) | — | — | ídem |
| `spi_selftest.c.obj` (selftest) | CM7 | 53838 | 8 | 198700 | `build/selftest-spi/cm7_firmware.map` |
| `spi_selftest_common.c.obj` (selftest) | CM7 | 2105 | 0 | 0 | ídem |
| `dma_hal.c.obj` (02_dma, lo usa el selftest) | CM7 | 852 | 0 | 96 | ídem |
| `spi_selftest.c.obj` (selftest) | CM4 | 10151 | 4 | 32833 | `build/selftest-spi/cm4_firmware.map` |
| `spi_selftest_common.c.obj` (selftest) | CM4 | 2061 | 0 | 0 | ídem |

.text es la suma de las secciones de entrada `.text*` y `.rodata*`.

- `spi_bm.c.obj` y `spi_ll.c.obj` no cambian frente a F2.
- La imagen `selftest-spi` mide 110684/140/254484 en CM7 y 25876/28/45400 en CM4.
- Con los avisos de `-Wundef`: la cabecera de la HAL de SPI evalúa `USE_HAL_SPI_REGISTER_CALLBACKS`, que el `stm32h7xx_hal_conf.h` de `prj_01` no definía. Lo defino a `0U` bajo `SPI_DRIVER_SELFTEST` (§5.5).

### 5.3 Pruebas

Las cifras salen de `docs/spi_f3_bench_efb4a3f.txt`:

- Rev `efb4a3f` e id `47f3d7b8ec959318` en host, CM7 y CM4; `tree clean`.
- Checklist confirmado por Hector antes de la ejecución; flasheo con el script parado, arranque en frío y sin depurador.
- Protocolo v5; `REV_ID=0x2003`.

Las filas de F0, F1 y F2 dan otra vez PASS en la misma ejecución; aquí solo van las de F3. Todas son de CM7: la HAL de SPI solo está en CPU1.

| ID | Tier | Núcleo | Resultado | Valor medido | Control negativo: valor | Oráculo | Captura |
| --- | --- | --- | --- | --- | --- | --- | --- |
| T1 | HAL | CM7 | PASS | 33 casos por `spi_hal` en SPI2, SPI4, SPI5 y SPI6, 0 con otro estado. Incluyen: `h == NULL`, `cfg == NULL`, una instancia que no es SPI (USART3) y `h->Instance ≠ cfg->spi` (`ERR_PARAM`); los rechazos de `SPI_BM_CheckConfig` (3 bits, modo 4, `MSSI` con SS por software, umbral fuera de rango, 17 bits en SPI6); `Deinit`, `Wait`, `Transfer`, `StartTransferIt` y `StartTransferDma` con `NULL`, timeout 0, 0 y 65 536 tramas, `tx == NULL`, `rx` en `TX_ONLY`, dirección impar con 16 bits, `StartTransferDma` sin `hdmatx`, sin `hdmarx` con `rx`, y con un DMA de memoria byte para tramas de 16 bits; `SPE = 1` a mano y el handle en `BUSY_TX_RX` (`ERR_BUSY`); `AFCNTR = 1` vigente con otro `CPOL` u otro `SSIOP` (`ERR_BUSY`). Foto de los registros de las cuatro instancias y de los enable de RCC de C1/C2, intacta en los 4 grupos (`0xF`). Tras el rechazo por tamaño de DMA, el handle de SPI5 sigue en `READY` (1) | Un `SPI_HAL_Init(SPI2)` válido cambia la foto (1) | estado esperado escrito a mano por caso | `spi_f3_bench_efb4a3f.txt` |
| T2 | HAL | CM7 | PASS | SPI2 y SPI3: 40 960 configuraciones cada uno; SPI4, SPI5 y SPI6: 30 720 cada uno. La imagen de `SPI_HAL_Init` difiere de la de `HAL_SPI_Init` crudo en 0 (R3-4: desde la imagen de reset, con un `SPI_InitTypeDef` de `sst_hal_of()`, constantes HAL escritas a mano en la prueba). Frente a `spi_bm`, 0 fuera de la diferencia por diseño (`CR1.SSI` con SS por hardware activo bajo: 16 384 configuraciones en SPI2/3, 12 288 en SPI4/5/6). `CFG1`/`CFG2`/`CR1` de la tabla del RM en 0. 0 estados distintos de OK | La misma comparación con `MASTER` invertido en la foto de `spi_hal` lo detecta (1 de 1 por instancia) | `HAL_SPI_Init` crudo, imagen de `spi_bm` y `sst_t2_expect()` | ídem |
| T4 | HAL | CM7 (SPI6) + host | PASS | 896 transferencias por J1 a /8 (12,5 MHz), con la matriz de F1. 0 con estado, discrepancia o `Init`/`Deinit` fallido; la más larga, 2 099 373 ciclos, dentro de su `max_cycles`. 65 535 tramas de 8 bits: estado 0, 0 discrepancias. Host: 897 líneas con CRC-32 y cobertura de la semilla, 0 diferencias; cobertura completa en los 7 tamaños | MISO a PB4 con pull-up y pull-down, 4/8/16 bits: RX todo unos / todo ceros por CRC. Discrepancias = las del host (p. ej. 4 bits arriba 233, 8 bits abajo 251) | cable + CRC-32 en el host | ídem |
| T5(a) | HAL | CM7 (SPI4) + host | PASS | 32 capturas LA a 100 MS/s (tipo `H`): modos 0–3 × {4, 8, 13, 16} × MSB/LSB, SS por hardware, por `SPI_HAL_Transfer` (`HAL_SPI_Transmit`). El host las decodifica contra la semilla: 0 fallidas; SCK en reposo = `CPOL`; SS según su modo | Cada captura decodificada con el `CPHA` contrario y con el orden contrario: las dos encuentran discrepancias en 32 de 32 (p. ej. H0: 26 y 19) | `ad2.spi_decode()` en el host | ídem |
| T6 | HAL | CM7 (SPI6) | PASS | 8 y 16 bits, {1, FIFO + 1, 1024} tramas, por `SPI_HAL_StartTransferIt` y `SPI_HAL_Wait`, con el handler de la prueba llamando a `HAL_SPI_IRQHandler`: 0 fallidas. IRQ contadas: 3/2/1017 (8 bits) y 3/2/1021 (16 bits). (c) 0 IRQ en los 10 ms tras cada fin, con `SPE = 0`. IRQ después del cierre de la HAL: 1 con 1 trama y 0 con FIFO + 1 y 1024, en los dos anchos, igual al esperado (§5.5, decisión del responsable) | `TXPIE = 1` a mano con `SPE = 0`: 2 IRQ espurias (≥ 1) | ISR de prueba + CRC en el host | ídem |
| T7(b) | HAL | CM7 | PASS | SPI6 con BDMA canales 0/1 (`dma_hal`, buffers en `.d3_ram`), 8/16 bits, {1, 9, 255, 8192} tramas, por `SPI_HAL_StartTransferDma` (`HAL_SPI_TransmitReceive_DMA`) y `SPI_HAL_Wait`: 0 fallidas | Sin stream de RX (`HAL_SPI_Transmit_DMA`, `COMM` solo TX), 255 tramas: estado 0 y 255 palabras con el centinela (§5.5) | cable | ídem |
| T7(c) | HAL | CM7 (SPI4) + host | PASS | 2 capturas LA (tipo `Q`, 8 bits × 16 tramas y 16 bits × 11) por `HAL_SPI_Transmit_DMA` con DMA1 stream 0 (`dma_hal`, modo FIFO, §5.5): 0 fallidas en el host | el control de decodificación de T5(a), en la misma captura | `ad2.spi_decode()` | ídem |
| T14 | HAL | CM7 | PASS | `SPI_HAL_Deinit` deja SPI2..6 (F3_NOPIN), SPI6 (J1_SPI6_HAL) y SPI4 (SPI4_AD2_HAL) iguales a la imagen de reset (`0x00`) y el handle en `RESET`; también tras un `Init` con `keep_pins`. J1 en entrada. S5 pasa con PE11..PE14 en AF5 tras el `Deinit`, y en entrada | Tras `SPI_HAL_Init` y antes del `Deinit`: SPI6 `0x0D`, SPI4 `0x0C` (≠ 0) | reset de RM0399 escrito a mano | ídem |
| Tick | HAL | CM7 + host | PASS | 12 líneas `TICK` (`HAL_GetTick()`, base de tiempo TIM6, que la prueba no toca) estrictamente crecientes, de 66 038 a 77 645 ms; la última, después de `DONE` | — | host | ídem |
| Pila (D11) | — | CM7 | — | `Status_Task stack high-water mark 640 words` libres de 1024 (requisito: ≥ 128) | — | `uxTaskGetStackHighWaterMark` | ídem |
| Sin Digital Out | — | host | PASS | Enable releído en las 19 fronteras de paso: `0x0000` en todas | — | `FDwfDigitalOutEnableGet` | ídem |
| Veredicto | — | CM7 + host | PASS | `bench completed without abort`; CM4 `timeout 0, aborts 0`; `FW VERDICT=PASS failures=0`; 45/45; `HOST VERDICT=PASS` | — | — | ídem |

**El oráculo de las pruebas HAL.** La configuración HAL de la imagen cruda de T2 sale de `sst_hal_of()` (`CM7/App/Src/spi_selftest.c`), con las constantes HAL escritas a mano en la prueba, no de `fill_init()` de `spi_hal.c`. El selftest no define `HAL_SPI_MspInit`: la HAL llama a su versión débil, vacía, y ningún pin cambia en `HAL_SPI_Init`.

### 5.4 Medidas (B1)

De `docs/spi_f3_bench_efb4a3f.txt`, SPI6 en CM7 con los pines de J1 en AF. `Init` y `Transfer` se miden con `PRIMASK` puesto. `Transfer` es 1 trama a /2, solo TX. Las tres columnas salen de la misma imagen y la misma ejecución.

| Función | BM, CM7 (400 MHz) | LL, CM7 | HAL, CM7 |
| --- | --- | --- | --- |
| `Init` | 489 ciclos | 892 | 1533 |
| `Transfer` de 1 trama | 1464 | 2005 | 2181 |
| Por IRQ (T6): `TransferIrq` o `HAL_SPI_IRQHandler` | mín. 362, máx. 852 | mín. 522, máx. 977 | mín. 202, máx. 989 |

- `SPI_HAL_Init` incluye el `SPI_BM_Deinit` previo (§5.5) y la llamada a `HAL_SPI_Init`, que llama a la MSP débil.
- El mínimo por IRQ de la HAL (202) es menor que los de BM y LL. No he medido qué llamada lo da. La HAL tampoco sirve cada trama con una IRQ: con FIFO + 1 tramas hay 2 IRQ en total, y lo que queda en RX lo vacía el `EOT` (l. 3006-3034 de `stm32h7xx_hal_spi.c`).
- Las cifras de BM y LL no son las de F2: la imagen cambió.
- `CalcPrescaler`, el peor de T3: 97 ciclos.

### 5.5 Desviaciones del plan

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| Regla 3 / R2-1: `SSI` antes de `CFG2` en el `Init` | `SPI_HAL_Init` llama primero a `SPI_BM_Deinit`, pone el handle en `RESET` y luego llama a `HAL_SPI_Init`. Desde la imagen de reset, el orden propio de la HAL (`SSI`, l. 375-381, antes de `CFG2`, l. 398) cumple R2-1, y la HAL encuentra `CRCSIZE = 00111` (ES0445 2.22.4), que conserva con `USE_SPI_CRC = 0` (l. 367) | Sin el `Deinit`, un `Init` sobre una imagen previa haría que la HAL escribiera `CFG2` sobre un `SSI` anterior | Sí: el responsable eligió "SPI_BM_Deinit first" el 2026-10-04, antes de escribir `spi_hal.c` |
| ES0445 2.22.3 (`SPE` a 0 sin una IRQ después) en el camino IT | La HAL queda como es. `HAL_SPI_TransmitReceive_IT` llena la FIFO (l. 1926) antes de poner `DXPIE` (l. 1963); si caben todas las tramas, `TXTF` llegó antes y `DXPIE` sigue a 1 (RM0399 53.11.5), y `SPI_CloseTransfer` borra `SPE` (l. 3945) antes que `IER` (l. 3948): una IRQ después del cierre, con `h->State = READY`. Documentado en `spi_hal.h`; T6 cuenta esas IRQ y espera 1 en ese caso | Corregirlo exigiría apartarse de la secuencia de la HAL | Sí: el responsable eligió "Keep HAL, document" el 2026-10-04 |
| T6(c) | En CM7, T6 cuenta además, en los tres tiers, toda IRQ que llega después del cierre (la rama espuria del handler de la prueba), contra un esperado: 0, salvo la HAL con todas las tramas dentro de la FIFO (1). El host lo comprueba en las líneas `post=`/`exp=`. BM y LL dan 0 | El contador de "10 ms tras el fin" empezaba después de `SPI_HAL_Wait`, y la IRQ de arriba llegaba antes: la prueba no la veía (ejecuciones de desarrollo) | Como consecuencia de la decisión anterior; el cambio de la prueba no lo consulté |
| T7(b), control: `RXDMAEN = 0` → `ERR_OVERRUN` | Con la HAL el control es `rx == NULL` (`HAL_SPI_Transmit_DMA`, sin stream de RX, `COMM` solo TX): estado 0 y las 255 palabras con el centinela | La API de la HAL no deja `RXDMAEN = 0` con un stream de RX enlazado; el control comprueba igual que sin la petición de RX no llega nada | No consultado |
| T7(c) con DMA1 stream 0 | Por `dma_hal` en modo FIFO (umbral `FULL`, ráfagas simples), no en modo directo | `DMA_HAL_Init` de 02_dma (`dma_hal.c:183-186`) compara `DMA_PDATAALIGN_*` (bits 12:11) con `DMA_MDATAALIGN_*` (bits 14:13) en modo directo, y rechaza todo tamaño por encima de byte aunque `PSIZE = MSIZE` (ejecución de desarrollo 2, `CAPH k33 ... dinit=1`). No toco 02_dma (§2.2) | No consultado; está propuesto como tarea aparte para 02_dma |
| Contrato: read-back tras cada llamada a la HAL | El read-back del cierre acepta `IER.TSERFIE = 1`, y solo ese bit | Lo enciende la HAL en sus arranques IT (l. 1743, 1837, 1964) y su cierre no lo borra (máscara de l. 3948). Es lo que escribe la HAL, no una desviación de ella. `TSERF` solo lo pone una recarga de `TSER`, que `spi_hal` no programa; `SPI_HAL_Deinit` lo borra | No consultado |
| Contrato: validar antes de la HAL (C6) | `SPI_HAL_StartTransferDma` rechaza con `ERR_PARAM` un handle de DMA cuyo tamaño de memoria no cabe en la trama (la regla de la HAL, l. 2157, 2304, 2447) | La HAL lo rechaza después de poner `h->State` en ocupado, y lo deja así (l. 2157-2163): el handle no aceptaría otra llamada sin otro `Init` | No consultado; es una comprobación dentro de una función del contrato, no una función nueva |
| `SPI_HAL_Transfer`/`StartTransferIt`/`StartTransferDma` | Con `rx == NULL` usan `HAL_SPI_Transmit*`, que pone `CFG2.COMM` en solo TX (`SPI_2LINES_TX`, l. 872, 1730, 2153) aunque la configuración sea full duplex; la siguiente transferencia con `rx` lo vuelve a poner (`SPI_2LINES`, l. 1398, 1918, 2441). El read-back comprueba ese `COMM` | Es lo que hace la HAL con un solo buffer | No consultado; documentado en `spi_hal.h` |
| Encargo de F3: `HAL_SPI_MODULE_ENABLED` y su `#include` bajo `SPI_DRIVER_SELFTEST` | Además, `USE_HAL_SPI_REGISTER_CALLBACKS 0U` bajo la misma guarda (`b9b6237`) | La cabecera de la HAL de SPI lo evalúa en `#if`, y con `-Wundef` daba avisos | No consultado |
| — | Líneas de diagnóstico `T6H` (IER, CR1, IRQ espurias y `ErrorCode` tras `SPI_HAL_Wait`) y `CAPH` (estado del handle de SPI y del de DMA tras una captura de DMA con la HAL). El host no las usa | Localizaron los dos fallos de desarrollo; las dejo como registro | — |

**Diferencias por diseño de la imagen HAL frente a `spi_bm` (T2, R3-4).** Solo una: `CR1.SSI` con SS por hardware activo bajo. `spi_bm` escribe `SSI` inactivo con cualquier modo de SS; la HAL solo pone `SSI` con SS por software, maestro y polaridad baja (`stm32h7xx_hal_spi.c` l. 375-381, `SET_BIT` en l. 380). Con `SSM = 0`, `SSI` no actúa. Ningún otro bit de `CR1`, `CR2`, `CFG1`, `CFG2`, `IER`, `CRCPOLY`, `UDRDR` o `I2SCFGR` difiere en las 174 080 configuraciones.

### 5.6 Respuesta a la auditoría

La de F2 está arriba, en su propia sección. De F3 todavía no hay auditoría.

### 5.7 Erratas del plan encontradas

- **T7(b), control con la HAL:** el plan pide `RXDMAEN = 0` con `ERR_OVERRUN`, que la API de la HAL no permite producir (§5.5).

### 5.8 Problemas abiertos y no ejercitado

- **Dos fallos encontrados en las ejecuciones de desarrollo**, los dos corregidos en `3c23568`:
  - **T6 HAL con `ERR_VERIFY`** (`spi_dev_f3_bench_dbab128-dirty.txt`): 4 de 6 casos, con los datos bien (0 discrepancias). Mi read-back del cierre esperaba `IER = 0`, y la HAL deja `TSERFIE` (`T6H ... ier=00000400` en `_2.txt`). Con 1 trama pasaba solo porque la IRQ posterior al cierre caía en la rama espuria del handler de la prueba, que pone `IER = 0` (`spurious=1`, `ier=00000000`).
  - **T7(c) HAL con `ERR_HAL`** (`_dirty.txt`, `_2.txt`): la segunda captura (16 bits). `DMA_HAL_Init` falló por el defecto de 02_dma (§5.5); el handle de DMA conservaba el tamaño byte de la captura anterior, y `HAL_SPI_Transmit_DMA` lo rechazó dejando el handle de SPI en `BUSY_TX` (`CAPH k33 ... hstate=3`).
- **Defecto en otro driver:** `DMA_HAL_Init` de 02_dma rechaza el modo directo con tamaños de media palabra o palabra en DMA1/DMA2 (§5.5). No lo toco; queda propuesto como tarea aparte.
- **Limitaciones de la HAL, documentadas en `spi_hal.h` (para el README):**
  - ES0445 2.22.5: `SPI_CloseTransfer` borra `SPE` justo tras `EOT`, sin guarda (decisión 5 del plan: no hay `guard_cycles` en este tier);
  - ES0445 2.22.3 en el camino IT con todas las tramas en la FIFO (§5.5);
  - `HAL_SPI_Init` escribe `CFG2` (l. 398) y después `AFCNTR` (l. 474): un `Init` con `keep_pins` suelta los pines unos ciclos;
  - el camino de error del `IRQHandler` borra `SPE` (l. 3121) antes que las IRQ (l. 3124); no ejercitado;
  - `IER.TSERFIE` queda a 1 tras una transferencia IT.
- **No ejercitado en F3:**
  - `SPI_HAL_StartTransferIt` sin `rx` (`HAL_SPI_Transmit_IT`) y por IT en SPI4;
  - los estados `ERR_MODF`, `ERR_OVERRUN`, `ERR_TIMEOUT` y `ERR_VERIFY` de `spi_hal` en la placa (el `ERR_VERIFY` de desarrollo era un defecto, ya corregido);
  - T5(b)–(g), T8, T9, T11, T12 y T13 con la HAL (el plan no los pide en F3);
  - la HAL en CM4 (no habilitada, por el encargo).
- **B1 de la HAL:** medido solo en Debug (`-Og`).
- **Lo que sigue de F1:** sin transferencias que terminen en SPI2, SPI3 y SPI5 (R3-3); solo silicio rev V.

### Líneas de `stm32h7xx_hal_spi.c` en las que se apoya `spi_hal`

Fuente del Cube con el que compila `prj_01` (`CUBE_DIR` → `02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7`, `STM32H7xx_HAL_Driver` en `e3c518e`).

| Línea | Qué hace la HAL | Uso en `spi_hal` |
| --- | --- | --- |
| 356 | `HAL_SPI_MspInit` con `State = RESET` | sin MSP propia: la débil, vacía |
| 363 | `SPE` a 0 | — |
| 367 | conserva `CRCSIZE` con `USE_SPI_CRC = 0` | read-back de `CFG1` con `CRCSIZE = 00111` |
| 375-381 (`SET_BIT` en 380) | `SSI` solo con SS por software, maestro, polaridad baja | read-back de `CR1`; la diferencia por diseño de T2 |
| 386 | `MASRX` | — |
| 394 / 398 | `CFG1` y `CFG2` con `WRITE_REG` | read-back |
| 467 | borra `I2SMOD` | — |
| 474 | `AFCNTR` con `MODIFY_REG`, después de `CFG2` | read-back; limitación de `keep_pins` |
| 872, 1730, 2153 / 1398, 1918, 2441 | `SPI_2LINES_TX` / `SPI_2LINES` | read-back de `COMM` |
| 1743, 1837, 1964 | los arranques IT encienden `TSERFIE` | read-back del cierre |
| 1926, 1963 | el llenado de la FIFO, y después `DXPIE` | IRQ tras el cierre (2.22.3) |
| 2157, 2304, 2447 | tamaño de memoria del DMA frente al de trama | comprobado antes, `ERR_PARAM` |
| 3006-3034 | el `EOT` vacía lo que queda en RX | — |
| 3121 / 3124 | camino de error: `SPE` antes que las IRQ | limitación, no ejercitada |
| 3937, 3945, 3948 | `SPI_CloseTransfer`: `SPE`, después `IER` (sin `TSERFIE`) | read-back del cierre; 2.22.3 y 2.22.5 |
