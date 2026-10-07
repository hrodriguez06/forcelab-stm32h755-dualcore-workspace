# Informe de implementación 05_i2c

Implementador: Claude (Claude Code), para Hector Rodriguez · Rama: driver/i2c · Plan: IMPLEMENTATION_PLAN.md revisión 1

## Resumen

F0 (banco) está implementada y entregada para auditoría. Contiene:

- `--sn` en `tools/flash.py` y la elección del puerto COM por número de serie del ST-LINK en `tools/monitor.py` (D7, S18);
- el respaldo de la flash de fábrica de la F334, fuera del repositorio (§5.8, "Respaldo");
- el firmware de banco de la F334 en `tools/i2c_target_f334/` (D2): target en 0x42, comandos armados, informe tras cada transferencia, S15 y S21; con el subconjunto de `cmsis_device_f3` v2.3.9 y su licencia;
- en `prj_01`, solo bajo `I2C_DRIVER_SELFTEST`: la opción, el preset `selftest-i2c`, la tabla de pasos v1 y las reglas de línea S15/S16 comunes a los dos núcleos, `bench_claim()`/`bench_safe()` en los dos núcleos y el selftest de F0 (T0a, T0c, T0b) con el área SRAM4 en `0x3800D000`;
- `prj_01/tools/i2c_hw_check.py`, con S2–S8 y S15–S21, `--bench-state taps|notaps`, `sn=` en cada llamada al programador, la comprobación de exactamente dos ST-LINK y el descarte de la primera adquisición del osciloscopio;
- `scope_arm_fall()` en `tools/ad2.py` (solo añadido).

Las sesiones de entrega, con las imágenes de `f7323f2` en las dos placas:

- **taps:** `docs/i2c_f0_bench_taps_f7323f2_5.txt`, 28/28 comprobaciones del host, `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`;
- **notaps:** `docs/i2c_f0_bench_notaps_f7323f2_2.txt`, 26/26, `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`.

Antes hubo seis sesiones de desarrollo y cinco de entrega que fallaron o abortaron. Están todas commiteadas y la §5.3 y la §5.8 dicen por qué. Dos de ellas destaparon cosas del banco que corrigió el responsable: PB9 de la H755 flotando, y las sondas del osciloscopio en el lado del bus de J2/J3. Otras dos destaparon fallos míos, ya corregidos: el hilo lector del host moría con un byte del reset, y la F334 contaba un flanco de SCL espurio.

No hay código del driver ni targets `_i2c_*`, porque F0 no los lleva. F1 no ha empezado: espera a que `AUDIT.md` diga que F0 pasa.

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F0 | entregada para auditoría (el commit de este informe) | `bdaf752` (código); evidencia en `f7323f2` (desarrollo) y `6322f49` | `03_firmware_templates/prj_01_template_dualcore/docs/i2c_f0_bench_taps_f7323f2_5.txt`, `.../i2c_f0_bench_notaps_f7323f2_2.txt` |
| F1 | sin empezar | — | — |
| F2 | sin empezar | — | — |
| F3 | sin empezar | — | — |

## F0

### 5.1 Commits

Rango de la entrega: `4891cbf..` el commit que añade este informe, en `driver/i2c`.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `b54ecf3` | feat(tools): select the ST-LINK by serial number in flash.py and monitor.py (D7, S18) | — |
| `078bcc1` | feat(tools): add the NUCLEO-F334R8 bench target of 05_i2c (S15, S21) | — |
| `7e931bf` | test(prj_01): add the 05_i2c bench step table and line rules (S15, S16) | — |
| `e2018f0` | test(prj_01): add the F0 steps of the 05_i2c selftest on both cores (T0a, T0c, T0b) | — |
| `7106890` | test(prj_01): add the I2C_DRIVER_SELFTEST option and the selftest-i2c preset | — |
| `b5ec7b7` | feat(tools): add a falling-edge scope trigger to ad2.py (05_i2c) | — |
| `5fe34fa` | test(prj_01): add i2c_hw_check.py, the host of the 05_i2c F0 bench (S2-S8, S15-S21) | — |
| `1b53043` | fix(tools): count no SCL edge in the first 10 us of an F334 SDA hold | fallo propio y hecho del banco, de las sesiones de desarrollo (§5.8) |
| `89e579d` | test(prj_01): print falls, time low and first fall of the four bus views in T0C | diagnóstico de PB9 (§5.8) |
| `bdaf752` | test(prj_01): make the i2c_hw_check.py readers survive a stray byte and a fresh F334 | fallos propios de las sesiones de desarrollo (§5.8) |
| `f7323f2` | docs(i2c): add the six development captures of the F0 bench (taps) | — |
| `6322f49` | docs(i2c): add the F0 evidence at f7323f2 | — |
| (este) | docs(i2c): add the implementation report for F0 | — |

Notas sobre el orden:

- En `7e931bf` y `e2018f0` nada compila todavía los archivos nuevos del selftest. `7106890` los añade al build, solo con la opción.
- Las imágenes de entrega se compilaron en `f7323f2`, un commit de documentación. Su código es el de `bdaf752`.

### 5.2 Build y tamaños

- **`build_all -Clean` en `f7323f2`:** 8/8 (`docs/i2c_f0_build_all_f7323f2.txt`).
- **`selftest-i2c` desde un directorio vacío:** configure y build salen con 0, y hay 0 líneas `warning:` o `error:` (`docs/i2c_f0_selftest_build_f7323f2.txt`).
- **`-Werror`:** `selftest-i2c`, `debug` y `release` con `CMAKE_C_FLAGS=-Werror`, en directorios de scratch. Las tres dan configure 0, build 0 y 0 avisos (`docs/i2c_f0_werror_f7323f2.txt`). El `CMakeCache.txt` de cada una dice `CMAKE_C_FLAGS:STRING=-Werror`.
- **Firmware de la F334:** `build.ps1 -Clean` y `build.ps1 -Clean -Werror` dan los dos `Warnings : 0` y `exit=0` (`docs/i2c_f0_f334_build_f7323f2.txt`). Avisos activos: `-Wall -Wextra -Wshadow -Wdouble-promotion -Wundef -Wconversion -Wpedantic` (`tools/i2c_target_f334/CMakeLists.txt`).
- **`git diff cc8aa2c -- 02_firmware_projects`:** vacío.

Tamaños del `.map` enlazado: solo secciones de salida asignadas, empezando en "Linker script and memory map". La fuente es `docs/i2c_f0_map_sizes_f7323f2.txt`.

| Imagen | .text | .data | .bss | Δ frente a 2481396 | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| prj_01 debug CM7 | 29304 | 132 | 55668 | 0 en cada sección asignada | `build/debug/cm7_firmware.map` |
| prj_01 debug CM4 | 3456 | 24 | 12560 | 0 en cada sección asignada | `build/debug/cm4_firmware.map` |
| prj_01 release CM7 | 28356 | 132 | 55672 | 0 en cada sección asignada | `build/release/cm7_firmware.map` |
| prj_01 release CM4 | 3112 | 24 | 12560 | 0 en cada sección asignada | `build/release/cm4_firmware.map` |
| F334 `i2c_target_f334.elf` | 10304 | 20 | 3392 | — (imagen nueva) | `tools/i2c_target_f334/build/f334/i2c_target_f334.map` |

Cómo se leen estas cifras:

- **Agrupación** (la de `arm-none-eabi-size`, `build_all.ps1`, `03_tim` y `06_spi`): .text = `.isr_vector+.text+.rodata+.ARM`, .data = `.data+.init_array+.fini_array`, .bss = `.bss+._user_heap_stack+.shared_ram`.
- **Referencia `2481396`:** las filas de `docs/tim_f0_map_sizes_2481396.txt`, sección a sección.
- **F334:** `.isr_vector` 392 + `.text` 8468 + `.rodata` 1444 = 10304; .data 12 + arrays 8 = 20; los 3392 de .bss son `.bss` 1340 más la reserva de pila `._stack_check` de 2052 (`linker/stm32f334r8_flash.ld`). En la memoria de la placa: FLASH 10324 B de 64 KB, RAM 3408 B de 12 KB.

| Objeto | Núcleo | .text | .data | .bss | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| `i2c_bm.c.obj`, `i2c_ll.c.obj`, `i2c_hal.c.obj` | — | no existen en F0 (0 objetos de `05_i2c` en los dos `.map`) | — | — | `build/selftest-i2c/cm{7,4}_firmware.map` |
| `i2c_selftest.c.obj` (selftest) | CM7 | 11077 | 4 | 413 | `build/selftest-i2c/cm7_firmware.map` |
| `i2c_selftest_common.c.obj` (selftest) | CM7 | 583 | 0 | 0 | ídem |
| `i2c_selftest.c.obj` (selftest) | CM4 | 2143 | 0 | 5 | `build/selftest-i2c/cm4_firmware.map` |
| `i2c_selftest_common.c.obj` (selftest) | CM4 | 543 | 0 | 0 | ídem |

En esta tabla, .text es la suma de las secciones de entrada `.text*` y `.rodata*`. La imagen `selftest-i2c` mide 41972/136/56088 en CM7 y 7056/24/12568 en CM4.

### 5.3 Pruebas

Las cifras salen de las dos capturas de entrega:

- **taps:** `docs/i2c_f0_bench_taps_f7323f2_5.txt`;
- **notaps:** `docs/i2c_f0_bench_notaps_f7323f2_2.txt`.

Las dos tienen Rev `f7323f2`, id `ce8319fd1f61c8ea` en el host, CM7 y CM4, y la F334 con Rev `f7323f2` e id `23a92e5573a53db7`, igual al que calcula el host. Las dos dicen `tree clean` y llevan el checklist confirmado por Hector el 2026-10-07. Silicio de la H755: `REV_ID=0x2003`. F334: `IDCODE 0x10016438`.

"Igual" en la columna de notaps quiere decir el mismo valor que en taps.

| ID | Tier | Núcleo | Estado del banco | Resultado | Valor medido | Control negativo: valor | Oráculo | Oráculo F334/AD2 | Captura |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| T0a sueltas | — | CM7 + host | taps y notaps | PASS | AD2: pull-up `0x4FFF`, pull-down `0x0000` (13 líneas). MCU: `IDR` pull-up `0x1FFF`, pull-down `0x0000` | — (abajo) | `IDR` | DIO del AD2 | las dos |
| T0a walking 1 | — | CM7 + host | taps y notaps | PASS | 13 patrones; diferencias AD2 `[]`, MCU `0x0000` | — (abajo) | `IDR` | DIO del AD2 | las dos |
| T0a control | — | CM7 + host | taps y notaps | PASS | DIO9 (PE5) con pull-down a propósito donde se espera pull-up | Host y MCU marcan `0x0200` (esperado `0x0200`) | `IDR` | DIO del AD2 | las dos |
| T0b(a) | — | host | taps y notaps | PASS | Guardas sobre mocks, antes de abrir nada. `ad2_set_oe` (mock de `gpio_hw_check.py`, sin cambios): 8 máscaras rechazadas antes de la DLL. `i2c_set_oe`: 7 rechazadas (DIO8, DIO10, bus, DIO9, DIO14, DIO15, HOST_REQ\|DIO8) con S2 y sin llamar a `ad2_set_oe`. S15 (`s15_report_ok`): 6 informes de la F334 rechazados (AF push-pull, pull-up, salida GPIO, AF1, `s15=VIOLATED`, PB9 ausente). S21 (`f334_hold_guard`): 5 `HOLD` rechazados (0 ms, 501 ms, EDGES con SCL, 1000 ms, línea inválida). S18 (`s18_probes_ok`): 4 listas rechazadas (un tercer ST-LINK, la F334 ausente, otro SN, placas cruzadas). T0d (`identity_problems`): 6 identidades rechazadas | Aceptados: `ad2_set_oe` 2 máscaras; `i2c_set_oe` `0x0000` y `0x1000`; S15 AF4 OD y entrada; S21 `SDA 50`, `SCL 500`, `SDA 500 EDGES 5`; S18 el par esperado y el parser sobre un listado real; T0d la identidad completa | casos escritos a mano | — | las dos |
| T0b CM7 en T0B | — | CM7 | taps y notaps | PASS | Paso T0B (bus de CPU2): 32 rechazos (S15, S16 dueño, S4/S13/S14, R4), aceptados por error `0x00000000`; GPIOA/B/E/F intactos | 3 controles aceptados `0x7` (entradas sin pull, una línea del AD2 con pull) | lista de casos escrita a mano; foto de `MODER/OTYPER/OSPEEDR/PUPDR/AFR` | — | las dos |
| T0b CM4 reclamos | — | CM4 | taps y notaps | PASS | 20 casos. En T0B: aceptados `0x80007` (esperado `0x80007`: PB8, PB9 y los dos en AF4 OD sin pull, y la entrada simple). Entre pasos y en T0B_CM7: `0x80000` (solo la entrada). Registros cambiados `0x000000` en los tres | Los rechazos de push-pull, pull, AF6, AF1, salida GPIO, PF14/PF15, PE7, PE12, PB5, PA5 y PB8+PB7 | máscaras escritas a mano (`IST_M4_CLAIMS_EXPECT_*`); foto de GPIOA/B/E/F | — | las dos |
| T0b S16 instancia CM4 | — | CM4 | taps y notaps | PASS | PB8 AF4 frente a I2C1: `PE=1` (releído 1) y reloj apagado rechazados; aceptados `0xC` (esperado `0xC`); GPIO cambiado `0x000000`; I2C1 `CR1` después `0x00000000`; reloj como antes | `PE=0` con `ANFOFF` y en reset, aceptados | registros de I2C1 y del banco RCC de CPU2 leídos directamente | — | las dos |
| T0b S16 instancia CM7 | — | CM7 | taps y notaps | PASS | PB8 AF6 frente a I2C4: `PE=1` (releído 1) y reloj apagado rechazados; aceptados `0xC` (esperado `0xC`); I2C4 `CR1` después `0x00000000` | ídem | registros de I2C4 y del banco RCC de CPU1 | — | las dos |
| T0b S16 pines | — | CM4 y CM7 | taps y notaps | PASS | `ist_s16_cases()`: el otro pin en el otro AF, el otro pin en salida, el propio en el otro AF y el propio en salida, rechazados; aceptados `0x70` (esperado `0x70`) en los dos núcleos | Los dos en entrada, los dos analógicos y los dos ya en el AF propio, aceptados | imágenes de `MODER`/`AFR` escritas a mano | — | las dos |
| T0b CM4 escribe AF4 | — | CM4 + host | taps y notaps | PASS | PB8/PB9 en AF4 OD sin pull con I2C1 en `PE=0`: `MODER 0xA`, `OTYPER 0x3`, `PUPDR 0`, `AFR 0x44`, I2C1 `CR1=0`, `BUSY 0`. `IDR` del bus en CM4 y CM7 `0x3` (suelto). Host, en el mismo paso: taps SCL 3,157 V, SDA 3,301 V; notaps 1+ 0,057 V, 2+ 3,235 V; F334 `scl=1 sda=1 busy=0` | El reclamo de CM7 de PB8/PB9 en AF6 mientras CM4 los tiene en AF4: rechazado | registros leídos directamente | `LEVELS` de la F334; osciloscopio (taps) | las dos |
| T0b CM4 `bench_safe` | — | CM4 | taps y notaps | PASS | Desde AF4: `MODER 0x0`, `OTYPER 0x3`, `PUPDR 0`, `AFR 0x00`; I2C1 `CR1 0x00000000`; reloj como antes | — | registros leídos directamente | `LEVELS` después | las dos |
| T0b CM7 reclamos | — | CM7 | taps y notaps | PASS | Paso T0B_CM7 (bus de CPU1, I2C4 en reset): 15 casos, aceptados `0x4007` (esperado `0x4007`: AF6 OD sin pull en PB8, PB9 y los dos, y la entrada simple); registros intactos | Los 11 rechazos (push-pull, pull-up, pull-down, AF4, AF5, salida GPIO, PB8 con pull-up, PF14/PF15 en AF4, PF14 con pull, PF15 salida) | máscara escrita a mano (`CM7_CLAIMS_EXPECT`) | — | las dos |
| T0b CM7 escribe AF6 | — | CM7 + host | taps y notaps | PASS | PB8/PB9 en AF6 OD sin pull con I2C4 en `PE=0`: `MODER 0xA`, `OTYPER 0x3`, `PUPDR 0`, `AFR 0x66`, I2C4 `CR1=0`, `BUSY 0`, `IDR 0x3`. Host: taps 3,157/3,300 V; notaps 0,056/3,235 V; F334 `scl=1 sda=1` | — | registros leídos directamente | `LEVELS`; osciloscopio (taps) | las dos |
| T0b CM7 `bench_safe` | — | CM7 | taps y notaps | PASS | `MODER 0x0`, `OTYPER 0x3`, `PUPDR 0`, `AFR 0x00`; I2C4 `CR1 0x00000000`; reloj de I2C4 como antes (0) | — | registros leídos directamente | `LEVELS` después | las dos |
| T0b(d) | — | CM7 + host | taps y notaps | PASS | Sin HOST_REQ durante 10 s: `GPIOE MODER[31:4]=0x00004000`, `AFRH=0`, PB8\|PB9 y PF14\|PF15 `MODER 0`. MCU_ACK alto 9,862 s (taps) y 9,859 s (notaps), y luego bajo en 20/20 lecturas mientras el MCU espera el pulso | — (es el propio caso negativo) | `MODER` leído directamente; DIO13 | — | las dos |
| T0c reposo (S17, S19) | — | CM7 + host + F334 | taps | PASS | Antes de cada paso, en T0C y tras cada retención: osciloscopio SCL 3,156–3,159 V (3,16 ± 0,10) y SDA 3,299–3,304 V (≥ 3,20). F334 `scl=1 sda=1 busy=0`, S15 ok. CM7 `pb8=pb9=pf14=pf15=1` | Las retenciones (abajo) | `IDR` | `LEVELS`; osciloscopio | taps |
| T0c reposo (S17, S19) | — | CM7 + host + F334 | notaps | PASS | Osciloscopio 1+ 0,056–0,059 V (< 0,10: PF14 con su 22 kΩ a GND) y 2+ 3,235–3,239 V (PF15 con su 22 kΩ a 3V3, §5.5). F334 `scl=1 sda=1`. CM7 `pb8=pb9=1`, `pf14=0`, `pf15=1` | Las retenciones (abajo) | `IDR` | `LEVELS`; osciloscopio | notaps |
| T0c retención SDA | — | CM7 + host + F334 | taps | PASS | F334: `us=50001`, `cause=TIME`, flancos de SCL contados 0, `blank=1` (§5.8), `scl=1 sda=1` 1 ms después. CM7: PB9 baja una vez durante 50797 µs; PB8 0 caídas; PF15 una vez, 50797 µs; PF14 0. Osciloscopio: 2+ bajo 0,99 V durante 50,10 ms en 1 tramo, mínimo 0,047 V; 1+ mínimo 3,154 V | La retención misma: si alguna vista no la ve, falla | CYCCNT de CM7 | `HOLD_END`; osciloscopio con disparo por flanco de bajada | taps |
| T0c retención SCL | — | CM7 + host + F334 | taps | PASS | F334: `us=50001`, `cause=TIME`, `scl=1 sda=1` después. CM7: PB8 una vez, 50787 µs; PB9 0; PF14 una vez, 50787 µs; PF15 0. Osciloscopio: 1+ bajo 0,99 V durante 50,10 ms, mínimo 0,048 V; 2+ mínimo 3,295 V | ídem | CYCCNT | ídem | taps |
| T0c retenciones | — | CM7 + host + F334 | notaps | PASS | SDA: F334 `us=50001`, flancos 0, `blank=0`; CM7 PB9 una vez, 50812 µs; PB8 0. SCL: F334 `us=50001`; CM7 PB8 una vez, 50813 µs; PB9 0. PF14 y PF15 no caen nunca (sin tomas). Sin vista del osciloscopio (D3) | ídem | CYCCNT | `HOLD_END` | notaps |
| T0d | — | host | taps y notaps | PASS | Host HEAD `f7323f2`. CM7 y CM4: Rev `f7323f2`, id `ce8319fd1f61c8ea` (el del host). F334: Rev `f7323f2`, id `23a92e5573a53db7` (el del host), protocolo v1. Exactamente dos ST-LINK: `0016000C4142501220353451` (NUCLEO-H755ZI-Q, V3J17M11) en COM4 y `066BFF393731435243254117` (NUCLEO-F334R8, V2J38M27) en COM7, los dos puertos elegidos por SN. Banner de la F334: `PB8/PB9 MODER=2 OTYPER=1 PUPDR=0 AFR=4 s15=ok` | El control es T0b(a) en el mock: un SN, una Rev o un id distintos abortan | `host_build_id()` de `gpio_hw_check.py` sobre `build/selftest-i2c` y `tools/i2c_target_f334/build/f334` | banner de la F334; `STM32_Programmer_CLI -l st-link-only` | las dos |
| Sin Digital Out | — | host | taps y notaps | PASS | Enable de Digital Out releído en las 6 fronteras de paso: `0x0000` en todas. Al abrir y al cerrar: enable `0x0000`, Zet `0xFFFF` | — | — | `FDwfDigitalOutEnableGet` | las dos |
| Pila | — | CM7 | taps y notaps | PASS | `Status_Task`: 782 palabras libres de 1024 (≥ 128) | — | `uxTaskGetStackHighWaterMark` | — | las dos |
| F334 al final | — | host | taps y notaps | PASS | `scl=1 sda=1`, S15 ok; `STATUS` con 0 transferencias, 0 bytes, 0 errores, 0 cortes, 0 informes perdidos y 0 desbordes de UART; ninguna línea no pedida (un banner de reinicio la haría fallar) | — | — | `STATUS`, `LEVELS` | las dos |
| Veredicto | — | CM7 + host | taps y notaps | PASS | `bench completed without abort`; CM4 `timeout 0, aborts 0`; salidas por CYCCNT parado: CM4 0, CM7 0; `FW VERDICT=PASS failures=0`; host 28/28 (taps) y 26/26 (notaps); `HOST VERDICT=PASS` | — | — | — | las dos |

Notas sobre las comprobaciones:

- **Las que pasan porque si no, abortarían.** Las "S17 …" del host lanzan `BenchAbort` si fallan, así que su PASS dice lo mismo que el hecho de que la sesión siguiera. Las sesiones `taps_f7323f2_2/_3/_4` y `notaps_f7323f2` muestran ese aborto de verdad.
- **Notaps tiene 2 comprobaciones menos:** sin tomas no hay vista del osciloscopio de las retenciones.
- **Sesiones de entrega anteriores**, todas en `f7323f2` y commiteadas sin editar (`6322f49`):
  - `taps_f7323f2`: 28/28, PASS, pero con las sondas en el lado del bus de J2/J3, lo que la sesión siguiente demostró. No es la entregada.
  - `notaps_f7323f2`: `ABORT: S19` (osciloscopio 1+ 3,303 V con PF14 = 0).
  - `taps_f7323f2_2`: `ABORT: S19` (1+ 0,036 V, PF14 = PF15 = 0).
  - `taps_f7323f2_3`: `ABORT: S17` (1+ 0,141 V, 2+ 0,156 V, PF15 = 0).
  - `taps_f7323f2_4`: `ABORT: S17` (1+ 0,142 V, 2+ 0,157 V, PF14 = PF15 = 1).
  - En las cuatro que abortaron, CM7 abortó a su vez por falta del host (`ABORT step=T0A reason=host timeout`, `FW VERDICT=FAIL failures=2`), dejó todo en `bench_safe` y nada movió el bus.
- **Sesiones de desarrollo** (`f7323f2`, §5.8): `i2c_dev_f0_bench_taps_5fe34fa{,_2,_3}.txt` y `..._5fe34fa-dirty{,_2,_3}.txt`.

### 5.4 Medidas (B1)

- En F0 no hay funciones del driver: no hay ciclos que medir.
- Pila de `Status_Task` con el selftest: 782 palabras libres de 1024 (las dos capturas). La función más profunda del selftest de CM7 es `step_t0b_cm7`, con 288 B (`.su` de `selftest-i2c`).
- Tamaños: §5.2.

### 5.5 Desviaciones del plan

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| Firmware de la F334: "retener SDA … hasta n flancos de SCL" | Los flancos de SCL de los primeros 10 µs de una retención de SDA no cuentan (`HOLD_EDGE_BLANK_US`). Se informan aparte como `blank=` | Con las tomas, la propia bajada de SDA de la F334 pone un flanco en su entrada PB8, 2 µs después (`edge1_us=2`), que no ven ni el osciloscopio ni PB8/PF14 de la H755 (§5.8). Sin el blanking, T9 de F1 contaría un pulso de más | No consultado; queda en `tools/i2c_target_f334/README.md` |
| T0c: "la F334 retiene SDA (y luego SCL) 50 ms y las tres vistas lo ven" | Además, CM7 ve los cuatro pines (PB8, PB9, PF14, PF15) y exige que el otro pin del bus no caiga nunca; el host exige 0 flancos contados de SCL durante la retención de SDA y que la F334 lea `scl=1 sda=1` 1 ms después de soltar | Así cada vista comprueba también que la otra línea queda libre | No consultado; más estricto |
| T0b: "rechazos exactos sin escritura"; los casos válidos se aceptan | Además, cada núcleo **escribe** el caso aceptado (PB8/PB9 en su AF, OD, sin pull, con su instancia en `PE=0`). El host lee los niveles con los pines así, y después se fotografía `bench_safe()` | Muestra que con `PE=0` la instancia no conduce el bus (50:429-431) y que `bench_safe()` deshace el AF. Nadie conduce: ni la H755 (`PE=0`) ni la F334 | No consultado; ampliación |
| S16: "Cambio de instancia: `PE = 0`, pines a entrada, ≥ 1 ms" | T0B_CM7 espera 2 ms (CYCCNT) antes del primer reclamo, después del `bench_safe()` de CM4 en T0B | ≥ 1 ms con margen | No consultado |
| `ist_line_rule()`: la tabla de líneas del plan | Deniega por defecto cualquier salida o AF fuera de PB8/PB9, incluidas las líneas del AD2 (S4), SPI4 (S13), J1 (S14), PE0/PE1, PC13, PA4, PA7, PA13/PA14. Las entradas con pull solo se rechazan en PB8, PB9, PF14 y PF15 | Este banco no conduce ninguna otra línea en F0 | No consultado; más estricto |
| S2–S8 en el host | S2, S3 (con `i2c_set_oe()`, solo HOST_REQ), S7 y S8 como en `06_spi`. S5 y S6 no tienen paso aplicable: ningún paso de este banco conduce el bus del AD2 (PE8..PE15); T0a comprueba que esas líneas siguen los pulls del MCU | El banco I2C no usa el bus del AD2 | No consultado |
| S17 "SDA ≥ 3,20 V" (el plan lo da para "con tomas") | El host aplica 2+ ≥ 3,20 V también en notaps | Sin J3, la sonda 2+ ve PF15 con su 22 kΩ a 3V3 frente al 1 MΩ del AD2: 3,3 × 1000/1022 = 3,229 V calculados, 3,235–3,239 V medidos. Pasa con unos 35 mV de margen, y no mide el bus | No consultado; lo apunto en §5.8 |
| T0d "Todo coincide" | La Rev de la F334 debe ser la del checkout del host, sin `-dirty`, y su id el que calcula el host. Hay que reflashear la F334 en cada commit que se quiera capturar | Es la lectura literal de "Rev e id de CM7, CM4, host y F334 … Todo coincide" | No consultado |
| CMSIS de F3: "cabeceras del F334, `system_stm32f3xx.h`, el startup de GCC y la licencia" | Copié además `stm32f3xx.h`, la cabecera de familia que incluye `stm32f334x8.h`. No copié `system_stm32f3xx.c`: el firmware tiene su `SystemInit` en `src/system_f334.c` | `stm32f3xx.h` es la cabecera de entrada del CMSIS | No consultado |
| Capturas "en frío y sin depurador" | Sin depurador (tasklist en la captura). Arranque: el host resetea la H755 por SWD con `-c port=SWD mode=HOTPLUG sn=<SN> -rst` (un reset de sistema, no un corte de alimentación), como `06_spi`. La F334 no se resetea en la sesión: el host le pide el banner con `ID` y la pone a cero con `CLR`, `INIT`, `DISARM` y `FMP 0` | Ciclar la alimentación lo hace el responsable; no lo pedí para estas sesiones | No consultado |

### 5.6 Respuesta a la auditoría

Todavía no hay auditoría.

### 5.7 Erratas del plan encontradas

| Sección del plan | Qué dice | Qué es correcto | Fuente |
| --- | --- | --- | --- |
| "Archivos, dependencias y targets" | `docs/i2c_<fase>_<prueba>_<hash>.txt` | `docs/i2c_<fase>_<prueba>_<estado>_<hash>.txt`, como dicen las instrucciones §3.4 y el encargo de F0 (`i2c_f0_bench_taps_<hash>.txt`). Usé este | instrucciones §3.4 |
| "El bus I2C", fila J2 (y J3) | "J2 … H755 CN9-19 (PF14: osciloscopio 1+, 22 kΩ a GND, 150 pF)" | **No es una errata del plan, sino del banco.** El plan pone la sonda en el lado de PF14, y así debe ser para que "notaps" la quite del bus (D3) y para que S19 se pueda comprobar con el osciloscopio. El 2026-10-07 el banco la tenía en el lado del bus: notaps leyó 3,303 V en 1+ con PF14 = 0 (`i2c_f0_bench_notaps_f7323f2.txt`). El responsable movió las dos sondas y las capturas de entrega son con las sondas donde dice el plan. Lo pongo aquí porque la verificación del banco del 2026-10-06 ("osc. 1+ (SCL) ≈ 3,15 V") no distingue los dos sitios | capturas citadas |

### 5.8 Problemas abiertos y no ejercitado

**Respaldo de la F334.** Antes del primer flasheo, la flash de fábrica (64 KB) y los option bytes se guardaron fuera del repositorio, en `C:\Users\hrodriguez\Documents\R+D\00_Embedded_System\00_STmicro\board_backups\nucleo_f334r8_066BFF393731435243254117\`:

- `f334_flash_0x08000000_64KB_factory_2026-10-06.bin`: SHA-256 `3d674e4a1c49fb481301b08cc44b8a98dbd457a95a64d7766e76008a29ae60c8`. Se leyó dos veces con `-u 0x08000000 0x10000`, `mode=HOTPLUG` y `sn=`, y las dos lecturas tienen ese hash;
- `f334_option_bytes_factory_2026-10-06.txt` (`RDP` nivel 0);
- `README.txt`, con el comando de restauración.

**Lo que pasó en las sesiones de desarrollo** (capturas en `f7323f2`; el banco las fue destapando):

1. **PB9 de la H755 flotando.** En `i2c_dev_f0_bench_taps_5fe34fa.txt` y `..._5fe34fa-dirty.txt`:
   - durante la retención de SDA, el osciloscopio y PF15 vieron SDA bajo 50 ms; PB9 bajó 3 veces (43,9 ms en total) en una sesión y ninguna en la otra;
   - durante la retención de SCL, PB9 bajó 50,7 ms, 73 µs después que PB8, con SDA a 3,29 V.

   Es la firma de un pin sin contacto con el nodo y acoplado a su vecino CN7-2. El responsable revisó la conexión ("estaba bien"), y después la recableó y volvió a confirmar el checklist. No sé qué estaba mal. Desde entonces PB9 sigue a SDA en todas las sesiones (50,77–50,81 ms).
2. **El hilo lector del host moría.** En `..._5fe34fa_2.txt` no llegó ninguna línea de la H755 tras el reset. El byte que deja el reset se decodifica como U+FFFD; `print()` hacia una consola cp1252 lanzaba `UnicodeEncodeError` y el hilo moría sin avisar. La H755 había arrancado bien (su bucle de estado imprimía; lo leí con `monitor.py --sn`). Corregido en `bdaf752`.
3. **El banner de la F334 recién flasheada.** En `..._5fe34fa_3.txt` la F334 aún imprimía su banner de arranque cuando el host pidió `ID`, y las líneas se mezclaron. Corregido en `bdaf752`: el host espera 0,5 s de silencio y reintenta hasta 3 veces.
4. **Un flanco de SCL en la F334 durante la retención de SDA:**
   - Primero, un fallo mío: el mux de EXTI8 se cambiaba de PA8 a PB8 en cada retención. Corregido en `1b53043`.
   - Después quedó un flanco a los 2 µs (`edge1_us=2`, `..._5fe34fa-dirty_2.txt`). Con tomas sigue apareciendo (`blank=1` en `taps_f7323f2_5`); sin tomas no (`blank=0` en `notaps_f7323f2_2`). Lo atribuyo a la descarga de los 150 pF de la toma por la F334, sin resistencia en serie, que deja un glitch en su entrada PB8, sin filtro en el camino de EXTI.
   - Ninguna otra vista de SCL lo ve: ni el osciloscopio a 50 kS/s, ni PB8 y PF14 de la H755 leídos en bucle.
   - **Abierto para F1:** el filtro analógico del I2C de la F334 (50 ns) debería taparlo en las transferencias. En el protocolo, el esclavo baja SDA con SCL bajo, salvo en las retenciones del banco. Si T5 o T9 muestran algo, lo vuelvo a mirar.

**Sesiones de entrega que abortaron** (`notaps_f7323f2`, `taps_f7323f2_2/_3/_4`): las lecturas están en §5.3 y §5.7. En cada aborto paré, el banco quedó en estado seguro, describí las lecturas y pedí una revisión y una nueva confirmación del checklist:

- tras `notaps_f7323f2`, el responsable movió las sondas al lado de PF14/PF15;
- tras `taps_f7323f2_2`, `_3` y `_4`, respondió "Corregido, confirmado". No sé qué cambió en cada caso. Las lecturas apuntaban a J2/J3 sin contacto (`_2`) y a las sondas sin medir el nodo (`_3`, `_4`).

Confirmaciones del checklist:

- **2026-10-06, dos:** la inicial y la que siguió al recableado de PB9.
- **2026-10-07, seis:** J2/J3 quitados para notaps; sondas movidas; J2/J3 de vuelta; y una tras cada aborto de `taps_f7323f2_2`, `_3` y `_4`.

**No ejercitado en F0** (el README del cierre debe copiarlo entero):

- **El target I2C de la F334:** en F0 nada la direcciona (`STATUS xfers=0` al final de las dos sesiones). Nada de lo siguiente se ha ejecutado en el banco:
  - la respuesta en 0x42;
  - los registros y el puntero;
  - `NACK`, `CORRUPT`, `STRETCH ADDR/BYTE`;
  - el modo SBC;
  - `FMP 1`;
  - los informes `RPT`;
  - el corte por 500 ms sin progreso;
  - `DUMP` contra lo escrito.

  Llega con T4–T10 de F1.
- **De las retenciones de la F334:**
  - solo se ejercitó la liberación por tiempo (`cause=TIME`, 50 ms). La liberación por flancos (`EDGES n`) no se ejercitó;
  - el límite de 500 ms solo se probó en el guarda del host (mock) y con un `HOLD SDA 600` de desarrollo que la F334 rechazó (`ERR HOLD ms must be 1..500 (S21)`, sesión manual sin captura);
  - el IWDG y los handlers de fallo que sueltan los pines no se provocaron.
- **La ruta de aborto S15 de la F334** (`I2C_TARGET ABORT S15`) no se provocó; solo se probó su informe en el host, sobre mocks.
- **`od_out`** (PB8/PB9 como GPIO open-drain para la recuperación): 0 en todos los pasos de F0. Solo se probó el rechazo.
- **`tools/flash.py --sn`:**
  - el único cambio a una línea existente es el `return` de `cubeprog_connect()`, que añade `sn=` solo con `--sn`. Sin la opción, el `--dry-run` imprime el mismo comando que antes (lo comprobé en `prj_01` con `--preset debug`);
  - ejercitado al flashear las dos placas y en `--dry-run` con `--read-ob`;
  - `--read-ob` y `--ob` reales no se ejecutaron con `--sn`;
  - la negativa con `--tool openocd` se probó en `--dry-run`;
  - al flashear el ELF de la F334, `flash.py` imprime sus líneas fijas de la H755 ("Target: STM32H755ZI", "CPU1 (Cortex-M7) bank 1") y el aviso de BCM4. Lo que se programó lo dicen la línea de la sonda y la salida de CubeProgrammer (`Device ID : 0x438`).
- **`tools/monitor.py`:** `--list-sn`, `--sn` en modo seguimiento y `port_for_sn()` (desde el host) ejercitados. Un ST-LINK sin VCP o con VCP de otro tipo no se probó.
- **Los otros bancos** (`gpio_hw_check.py`, `tim_hw_check.py`, `spi_hw_check.py`) siguen reseteando sin `sn=` y eligiendo el puerto con `autodetect()`. No puedo cambiarlos (instrucciones §2.2). Según S18 se ejecutan con el USB de la F334 desconectado.
- **Tiempos del bus** (t_r, f_SCL, tLOW…) no se midieron en F0. El decodificador I2C analógico de `ad2.py` no existe todavía: es de F1 (T5).
- **Arranque en frío:** las sesiones empiezan con un reset de sistema por SWD, no con un corte de alimentación (§5.5).
- **notaps, sonda 2+:** sin J3 lee PF15 y no el bus, con 35 mV de margen sobre el umbral (§5.5).
- **El estado "GPIO" del banco** (J2/J3 quitados para los bancos de `00_gpio`) no se ejercitó: no forma parte de F0. Las sesiones notaps dejan J2/J3 quitados, pero con la F334 conectada.

## Decisiones abiertas usadas

| # | Opción usada | Consultada |
| --- | --- | --- |
| 1 | `I2cBm_Status_t` en los tres tiers (sin uso en F0) | Por defecto (encargo de F0) |
| 2 | Pull-ups de 1,0 kΩ al 3V3 de la H755 (el banco del responsable) | Por defecto |
| 3 | 1 MHz en una sesión sin tomas (`--bench-state notaps`; en F0 no hay 1 MHz) | Por defecto |
| 4 | CM4 I2C1; CM7 I2C2, I2C3, I2C4 (`bench_claim`/`bench_safe` de cada núcleo) | Por defecto |
| 5 | Recuperación con funciones del llamador (sin uso en F0) | Por defecto |
| 6 | "No reproducido" para los controles de erratas (sin uso en F0) | Por defecto |
| 7 | HAL I2C en CM7 solo con la opción (sin uso en F0) | Por defecto |
| 8 | Firmware de la F334 en `tools/i2c_target_f334/`, con el subconjunto del CMSIS de F3 y su licencia Apache-2.0 | Por defecto |
| 9 | Documentos de proceso borrados al cierre (sin uso en F0) | Por defecto |

## Tabla de capacidades

Llega con F1 (`i2c_bm`).
