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
