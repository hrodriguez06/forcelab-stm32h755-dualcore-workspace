# Informe de implementación 05_i2c

Implementador: Claude (Claude Code), para Hector Rodriguez · Rama: driver/i2c · Plan: IMPLEMENTATION_PLAN.md revisión 3 (F0 se hizo con la revisión 1, la primera entrega de F1 con la 2)

## Resumen

**F1 (corrección 3) está entregada para auditoría** (§F1, corrección 3): responde a la auditoría de F1, parte 3 (B1, O22). Sesiones de entrega con las imágenes de `26c2b4f`: taps `docs/i2c_f1_bench_taps_26c2b4f.txt` 96/96 y notaps `docs/i2c_f1_bench_notaps_26c2b4f.txt` 74/74, `FW VERDICT=PASS` y `HOST VERDICT=PASS` en las dos.

**F1, corrección 2** (§F1, corrección 2): responde a la auditoría de F1, parte 2 (m33, m34, m35, O16–O20). Sesiones de entrega con las imágenes de `fefacf4`: taps `docs/i2c_f1_bench_taps_fefacf4.txt` 96/96 y notaps `docs/i2c_f1_bench_notaps_fefacf4.txt` 74/74, `FW VERDICT=PASS` y `HOST VERDICT=PASS` en las dos.

**F1, corrección (plan revisión 3)** (§F1, corrección): responde a la auditoría de F1, parte 1 (M2, M3, m17–m32, O6–O15). Las sesiones de entrega, con las imágenes de `9c07b93`: taps `docs/i2c_f1_bench_taps_9c07b93.txt` 95/95 y notaps `docs/i2c_f1_bench_notaps_9c07b93.txt` 73/73, `FW VERDICT=PASS` y `HOST VERDICT=PASS` en las dos. Lo que sigue de este resumen es la entrega anterior de F1.

**F1 (`i2c_bm`), primera entrega** (§F1). F0 pasó la auditoría (`AUDIT.md`, `85f9d29`); su respuesta está en §F1 5.6.

F1 contiene:

- `01_shared_drivers/05_i2c/i2c_bm.h/.c` y el target `stm32h7_driver_i2c_bm`: el contrato de la revisión 2 con las tres respuestas del 2026-10-07 (§F1 5.5), la tabla de capacidades (al final), `.data = .bss = 0`;
- `tools/ad2.py`: solo añadidos, el decodificador I2C analógico y su control;
- la F334 con M1 y m1–m7 corregidos (protocolo v2) y, después, su `TIMINGR` según `FMP` (protocolo v3);
- el selftest de F1 en los dos núcleos (T1–T15, B1; I2C1 con DMA2 en CM4, I2C4 con BDMA y `.d3_ram` en CM7) y su host en `i2c_hw_check.py`.

Las sesiones de entrega, con las imágenes de `e27c673` en las dos placas:

- **taps:** `docs/i2c_f1_bench_taps_e27c673.txt`, 90/90, `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`;
- **notaps:** `docs/i2c_f1_bench_notaps_e27c673.txt`, 69/69, `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`.

Antes hubo cuatro sesiones de desarrollo, todas commiteadas (§F1 5.8). T10(b) se entrega sin el STOP que piden el plan y el RM: el silicio no lo manda y el driver suelta el bus con un reset por software; el responsable lo aceptó (§F1 5.5).

F2 no ha empezado: espera a que `AUDIT.md` diga que F1 pasa.

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F0 | pasa (`AUDIT.md`, `85f9d29`) | `bdaf752` (código); evidencia en `f7323f2` (desarrollo) y `6322f49` | `03_firmware_templates/prj_01_template_dualcore/docs/i2c_f0_bench_taps_f7323f2_5.txt`, `.../i2c_f0_bench_notaps_f7323f2_2.txt` |
| F1 | corrección 2 auditada en placa con B1 (`AUDIT.md`, `d88f579`); corrección 3 entregada para auditoría | `26c2b4f` (código e imágenes; evidencia de build en `5648533`). Antes: `fefacf4`, `9c07b93`, `d326575` | `03_firmware_templates/prj_01_template_dualcore/docs/i2c_f1_bench_taps_9c07b93.txt`, `.../i2c_f1_bench_notaps_9c07b93.txt`; corrección 2: `.../i2c_f1_bench_taps_fefacf4.txt`, `.../i2c_f1_bench_notaps_fefacf4.txt`; corrección 3: `.../i2c_f1_bench_taps_26c2b4f.txt`, `.../i2c_f1_bench_notaps_26c2b4f.txt` |
| F2 | sin empezar | — | — |
| F3 | sin empezar | — | — |

### Resumen de F0

F0 (banco) contiene:

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

No hay código del driver ni targets `_i2c_*`, porque F0 no los lleva.

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

La de F0 está en §F1 5.6.

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

## F1

### 5.1 Commits

Rango de la entrega: `4abd264..` el commit que añade esta sección, en `driver/i2c`.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `8b5fd00` | fix(tools): answer the F0 audit in the F334 bench target, protocol v2 (M1, m1-m7) | M1, m1–m7 (§5.6) |
| `eeac84f` | test(prj_01): answer the F0 audit in i2c_hw_check.py; S17 as a ratio (R2-1, m7, m11-m16) | R2-1, m7 (host), m11–m16 |
| `fc4338e` | test(prj_01): answer the F0 audit in the 05_i2c selftest (m8, m9, m10) | m8–m10 |
| `e74de3c` | feat(i2c): add i2c_bm, the bare-metal tier of 05_i2c, and its target | — |
| `3aa49e3` | feat(tools): add an analog I2C decoder to ad2.py (05_i2c T5) | — |
| `9a8f918` | fix(tools): read the F334's idle time and S15 state with interrupts masked | dos carreras de la F334 que destaparon las sesiones de desarrollo (§5.8) |
| `610e057` | fix(i2c): software-reset the instance at once on TIMEOUT | T10(b) en desarrollo (§5.5, §5.8) |
| `8cb32ac` | docs(i2c): say in i2c_bm.h what a TIMEOUT does since 610e057 | — |
| `3e877d4` | test(prj_01): add the F1 steps of the 05_i2c selftest on both cores (T1-T15, B1) | — |
| `32cd2e5` | test(prj_01): add the F1 host of the 05_i2c bench to i2c_hw_check.py | — |
| `e346766` | fix(tools): give each tSU;DAT of the I2C decoder the side that drives its bit | T5 en desarrollo (§5.8) |
| `3bb1569` | fix(tools): select the F334 target's TIMINGR with FMP (protocol v3) | T5 en desarrollo (§5.8) |
| `7a22a3f` | test(prj_01): fix the taps findings of the first F1 development run | T5, T9, T10(b) en desarrollo (§5.8) |
| `e27c673` | docs(prj_01): add the F1 development captures of the 05_i2c bench so far | — |
| `3a26563` | docs(i2c): add the F1 delivery captures of the 05_i2c bench at e27c673 | — |
| `eea9f6e` | docs(i2c): add the F1 build evidence at 3a26563 | — |
| `d326575` | docs(i2c): cite the RM0399 lines of the automatic STOP on TIMEOUT | cita mía equivocada (§5.7) |
| (este) | docs(i2c): add the F1 section of the implementation report | — |

Notas sobre el orden:

- Las imágenes de las dos capturas de entrega se compilaron y flashearon en `e27c673`, con el árbol limpio. Su código es el de `7a22a3f`.
- La evidencia de build y de `.map` es de `3a26563`, cuyo código es el mismo que el de `e27c673`.
- `d326575` cambia solo un comentario de `i2c_bm.c`: `git diff e27c673 d326575 -- 01_shared_drivers 03_firmware_templates/prj_01_template_dualcore/CM7 03_firmware_templates/prj_01_template_dualcore/CM4 03_firmware_templates/prj_01_template_dualcore/Common tools` da esa línea y nada más, con el mismo número de líneas.

### 5.2 Build y tamaños

Todo en `3a26563`, con el árbol limpio:

- **`build_all -Clean`:** 8/8 (`docs/i2c_f1_build_all_3a26563.txt`).
- **`selftest-i2c` desde un directorio vacío:** configure 0, build 0 y 0 líneas `warning:` o `error:` (`docs/i2c_f1_selftest_build_3a26563.txt`).
- **`-Werror`:** `selftest-i2c`, `debug` y `release` con `CMAKE_C_FLAGS=-Werror` en directorios de scratch: configure 0, build 0 y 0 avisos en las tres; el `CMakeCache.txt` de cada una dice `-Werror` (`docs/i2c_f1_werror_3a26563.txt`).
- **Firmware de la F334:** `build.ps1 -Clean` y `build.ps1 -Clean -Werror` dan `Warnings : 0` y `exit=0` (`docs/i2c_f1_f334_build_3a26563.txt`).
- **`git diff cc8aa2c -- 02_firmware_projects`:** vacío.

Tamaños del `.map` enlazado (`docs/i2c_f1_map_sizes_3a26563.txt`, mismo método y agrupación que F0):

| Imagen | .text | .data | .bss | Δ frente a 2481396 | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| prj_01 debug CM7 | 29304 | 132 | 55668 | 0 en cada sección de la referencia | `build/debug/cm7_firmware.map` |
| prj_01 debug CM4 | 3456 | 24 | 12560 | ídem | `build/debug/cm4_firmware.map` |
| prj_01 release CM7 | 28356 | 132 | 55672 | ídem | `build/release/cm7_firmware.map` |
| prj_01 release CM4 | 3112 | 24 | 12560 | ídem | `build/release/cm4_firmware.map` |
| F334 `i2c_target_f334.elf` | 11288 | 20 | 3392 | — (FLASH 11308 B de 64 KB, RAM 3408 B de 12 KB) | `arm-none-eabi-size` |

Las secciones que el `.map` tiene y la referencia no (`.axi_ram`, `.d3_ram`, `.dma_buf`, `.eth_ram`, `.igot.plt`, `.iplt`, `.preinit_array`, `.rel.dyn`, `.v4_bx`, `.vfp11_veneer`) miden 0 en las cuatro imágenes; el archivo las lista.

| Objeto | Núcleo | .text (+.rodata) | .data | .bss | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| `i2c_bm.c.obj` | CM7 | 5142 | 0 | 0 | `build/selftest-i2c/cm7_firmware.map` |
| `i2c_bm.c.obj` | CM4 | 5124 | 0 | 0 | `build/selftest-i2c/cm4_firmware.map` |
| `i2c_selftest.c.obj` (selftest) | CM7 | 32648 | 8 | 911 | CM7 |
| `i2c_selftest_common.c.obj` (selftest) | CM7 | 4175 | 0 | 0 | CM7 |
| `dma_hal.c.obj` (BDMA de T7(b)) | CM7 | 872 | 0 | 96 | CM7 |
| `dma_bm.c.obj` | CM7 | 352 | 0 | 0 | CM7 |
| `i2c_selftest.c.obj` (selftest) | CM4 | 5443 | 4 | 65951 | CM4 (65536 son `s_buf` de T4/T7 en SRAM2) |
| `i2c_selftest_common.c.obj` (selftest) | CM4 | 4135 | 0 | 0 | CM4 |
| `dma_bm.c.obj` | CM4 | 1974 | 0 | 0 | CM4 |

La imagen `selftest-i2c` mide 78488/140/56692 en CM7 y 22076/28/78576 en CM4 (agrupación de la tabla de arriba). `arm-none-eabi-size` da 130452 de bss en CM7 porque suma `.d3_ram` y las otras secciones de RAM.

### 5.3 Pruebas

Las cifras salen de las dos capturas de entrega:

- **taps:** `docs/i2c_f1_bench_taps_e27c673.txt`: 90/90 comprobaciones del host, `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`;
- **notaps:** `docs/i2c_f1_bench_notaps_e27c673.txt`: 69/69, `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`.

Las dos tienen Rev `e27c673` e id `3417b1bd4c6653f1` en el host, CM7 y CM4, y la F334 con Rev `e27c673`, id `83794128d14afc2b` (el que calcula el host) y protocolo v3. Las dos dicen `tree clean` y llevan el checklist confirmado por Hector el 2026-10-07: taps con "J2/J3 puestos, confirmado"; notaps con "Listo ya removí los jumpers", la respuesta a mi petición de quitar J2/J3 y confirmar. Sin depurador ni WaveForms (tasklist en la línea 11 de cada una). CLKREF: la CPU y los kernels I2C van un +1,35 % (taps) y un +1,31 % (notaps) por encima del nominal; el host normaliza con eso las frecuencias medidas (N1).

Las comprobaciones de F0 (T0a, T0b, T0c, T0d) se repiten en las dos y pasan como en F0. S17 (R2-1): taps 1+ 3,159–3,162 V, 2+ 3,304–3,306 V, razón 0,956; notaps 1+ 0,020–0,028 V, 2+ 3,238–3,241 V; 22 lecturas por sesión, todas dentro.

"Igual" en notaps quiere decir el mismo resultado que en taps. Velocidades: taps 100 y 400 kHz en T4/T5 y 400 kHz en T6–T10; notaps 400 kHz y 1 MHz en T4 y 1 MHz en T6–T10 (S19, D3).

| ID | Tier | Núcleo | Estado del banco | Resultado | Valor medido | Control negativo: valor | Oráculo | Oráculo F334/AD2 | Captura |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Tabla de capacidades | BM | CM7 | taps y notaps | PASS | I2C1..4 frente a `stm32h755xx.h` (IRQn EV/ER), `stm32h7xx_hal_dma.h` (DMAMUX1 33/34, 35/36, 73/74; DMAMUX2 13/14) y RCC leído directamente: filas distintas `0x0` | I2C1+4, USART3 y `NULL` no son instancias | cabeceras de ST y registros | — | las dos |
| T1 | BM | CM7 (I2C2) y CM4 (I2C1) | taps y notaps | PASS | 62 rechazos en cada núcleo, 0 con otro estado; foto de la instancia, `RCC_C1/C2` `APB1LENR`/`APB4ENR`, `D2CCIP2R`, `D3CCIPR` y `SYSCFG_PMCR` sin cambios en los 2 grupos | Un `Init` válido cambia la foto | casos escritos a mano (`ist_t1`) | — | las dos |
| T1 con `BUSY = 1` | BM | CM4 (I2C1) y CM7 (I2C4) | taps y notaps | PASS | La F334 retiene SDA (`ISR 0x00008001`): 8 transferencias rechazadas con `ERR_BUS`; registros y RCC sin cambios | — (el propio caso) | `ISR` leído | `HOLD_END` | las dos |
| T2 | BM | CM7 (I2C2, I2C3, I2C4), CM4 (I2C1) | taps y notaps | PASS | 120 imágenes por instancia (TIMINGR × filtro × DNF × timeout): 0 distintas de los campos del RM escritos a mano; `Deinit` siempre a la imagen de reset | Imagen esperada con `DNF + 1`: distinguida | campos del RM a mano | — | las dos |
| T3 | BM | CM7 + host | taps y notaps | PASS | 403 vectores (≥ 300); el host los recalcula con sus reglas: 0 distintos. Las 286 salidas OK cumplen 50:342, 50:319-331, tabla 407, 50:162-163, 2.19.1 y modelo ≤ f: 0 no. El más lento, 12291 ciclos | En T0B(a), sobre el mock: `SCLL + 1` y `SDADEL` fuera de su ventana (vector de 100 MHz, 100 kHz, t\_r 1000, t\_f 300): los dos detectados | `calc_timing()` y `timingr_rm_problems()` del host | — | las dos |
| T4 | BM | CM4 (I2C1) y CM7 (I2C4) | taps (100, 400 kHz) y notaps (400 kHz, 1 MHz) | PASS | Por velocidad y núcleo: 6 `Read`, 6 `WriteRead`, 2 sondeos y 6 `Write` de 1, 2, 255, 256, 1000 y 65 535 bytes: 0 con otro estado o con un byte distinto. Host: 20 transferencias por grupo emparejadas con su `RPT`, 0 con problema. TIMINGR: taps `0x30D06C81` (100 k) y `0x20D0142E` (400 k); notaps `0x00D0479C` (400 k) y `0x00801A33` (1 M) | F334 con `CORRUPT 5`, lectura de 16: 1 byte distinto, en el índice 5 | datos regenerados por el host desde la semilla o el contenido conocido; CRC-32 | `RPT` de la F334 (bytes, CRC, punteros, STOP) | las dos |
| T5 | BM | CM4 y CM7 | taps | PASS | 5 transferencias por velocidad y núcleo, decodificadas exactas (`S 85A A5A A4N P`, `S 84A 07A \| Sr 85A A2N P`, `S 84A P \| S 84A 00A P`, `S 84A 14A 5AA P`, `S 86N P`), 0 glitches. 100 kHz: tLOW 5,30 µs (ventana 4,77–5,94), tHIGH 4,26–4,29 µs (contador 4,30), tSU;DAT de la H755 ≥ 4,80 µs, tHD;STA ≥ 4,38 µs, tSU;STO ≥ 4,38 µs, t\_r 248–253 ns, f\_SCL normalizada 99,63–99,77 kHz (modelo 99,70). 400 kHz: tLOW 1,57 µs (1,19–2,03), tHIGH 0,60–0,62 µs (0,62), tSU;DAT de la H755 ≥ 1,06 µs, tHD;STA ≥ 0,72 µs, tSU;STA 1,39–1,41 µs, tSU;STO ≥ 0,70 µs, f\_SCL normalizada 397,9–399,0 kHz (modelo 398,4). tSU;DAT de los bits de la F334, registrado: 529–806 ns | Cada captura decodificada con SCL y SDA intercambiados: distinta en las 20. Con umbral 0,5 V: igual en las 20 | `expected_frames()`, `t5_timing_problems()` | AD2 (12,5 MS/s a 100 kHz, 25 MS/s a 400 kHz) | taps |
| T6 | BM | CM4 y CM7 | taps (400 kHz) y notaps (1 MHz) | PASS | `Write`, `Read`, `WriteRead` de 1, 2, 256 y 1000 bytes por interrupción: 0 con otro estado o dato; IE en `CR1` al terminar `0x00`; 0 IRQ en los 10 ms siguientes | IE a 0 a mano tras `StartIt`: sigue `PENDING`, la espera acaba por su presupuesto (`ERR_TIMEOUT`), 0 IRQ | datos del host + CRC | `RPT` (la F334 corta la transferencia abandonada a los 500 ms: `ev=08`) | las dos |
| T7(a) | BM | CM4 (I2C1, DMA2 con `dma_bm`) | taps (400 kHz) y notaps (1 MHz) | PASS | Lectura y escritura de 2, 255, 256 y 4096 bytes: 0 con otro estado o dato | `RXDMAEN` a 0 a mano, lectura de 64: `WaitEnd` `ERR_TIMEOUT`, los 64 bytes conservan el centinela | ídem | `RPT` | las dos |
| T7(b) | BM | CM7 (I2C4, BDMA con `dma_hal`, `.d3_ram`) | ídem | PASS | Igual que T7(a) | Igual | ídem | `RPT` | las dos |
| T8(a) | BM | CM4 y CM7 | taps y notaps | PASS | Dirección 0x43: `Write` y sondeo IT `ERR_NACK`, `BUSY = 0` después (el STOP salió); la transferencia siguiente a 0x42 bien | — (la siguiente a 0x42 es el control) | `ISR` | `RPT` de la siguiente; osciloscopio de T5 k=4 (`S 86N P`) | las dos |
| T8(b) | BM | CM4 y CM7 | taps y notaps | PASS | La F334 hace NACK en el dato 3 de una escritura de 8: `ERR_NACK` en IT y sondeo; bytes reconocidos según el driver 3 | Sin NACK armado: la escritura termina bien | contador del driver (`done_tx`) | Tres vistas (M1): `RPT` `nk=1 rx=4`; osciloscopio (taps) `S 84A 81A 41A 63A E0N P` | las dos |
| T9 | BM | CM7 (I2C4, PB8/PB9 en GPIO open-drain) | taps y notaps | PASS | Con SDA retenido, `ISR 0x00008001` (BUSY), `Write` `ERR_BUS` en 326 (taps) y 350 (notaps) ciclos; `RecoverBus` OK, `pulses` 5, PB8/PB9 = 3 después; AF6, `Init` y una escritura correcta. F334: liberada en el 5.º flanco (`cause=EDGES`). Osciloscopio (taps): SDA bajo al empezar, 5 pulsos de SCL, SDA libre tras el 5.º y después el STOP | Sin `RecoverBus`: SDA abajo 95 ms (taps) y 96 ms (notaps), hasta el límite de 100 ms de la F334 | `IDR`, `pulses` | `HOLD_END` (`edges=5`); osciloscopio (taps) | las dos |
| T10(a) | BM | CM4 y CM7 | taps y notaps | PASS | La F334 estira 200 µs en la dirección y tras cada byte: escritura y lectura de 4 bytes correctas; CYCCNT ≥ +5 × 200 µs en las dos. Osciloscopio (taps): SCL retenido 203–209 µs, 5 veces en la escritura y 5 en la lectura (el stretching por byte en lectura, M1) | La misma transferencia sin `STRETCH` (`c_w0`, `c_r0`) | CYCCNT | `RPT`; osciloscopio (taps) | las dos |
| T10(b) | BM | CM4 y CM7 | taps y notaps | PASS (con la desviación de §5.5) | SCL retenido 50 ms, `timeout_scl_low` 489 (10 014 µs a 100 MHz): `ERR_BUS` por TIMEOUT; el driver termina con el reset por software. Fin ≤ 11 014 µs después de ver SCL bajo: taps 4220/4295 µs, notaps 5638/5028 µs (CM4/CM7). Osciloscopio (taps): SDA libre 9,885 y 9,890 ms después de la bajada de SCL (TIMEOUTA en tiempo del AD2: 9,881 ms); los dos altos al final; **sin STOP** | Sin `timeout_scl_low`: `ERR_TIMEOUT` 30 ms después (su `max_cycles`) | CYCCNT; `ISR` | osciloscopio (taps); `HOLD_END` | las dos |
| T11 | BM | — | — | **No ejercitado** | `ARLO`: un solo maestro en el banco (§5.8) | — | — | — | — |
| T12 | BM | CM4 (I2C1) | taps (400 kHz) y notaps (1 MHz) | PASS | `I2C123SEL = HSI` (64 MHz; APB1/kernel = 1,56) escrito y restaurado con read-back. Driver: 1000/1000 escrituras de 2 bytes por sondeo y 1000/1000 por DMA, 0 detenciones. F334: +1000 transferencias y +2000 bytes en cada variante, 0 NACK, 0 eventos, 0 cortes | A mano sin precargar el primer byte, por sondeo y por DMA: 0 detenciones en 1000 cada una: **no reproducido** (decisión 6) | estado del driver | totales de `STATUS` de la F334 | las dos |
| T13 | BM | CM4 sobre I2C2, leído por CM7 | taps y notaps | PASS | CPU2 `DisableClock(I2C2)` mientras CPU1 lo habilita: una escritura en `TIMEOUTR` llega igual; `EnableClock` + `DisableClock` de CPU2: igual | CPU1 deshabilita también: `TIMINGR 0x10101010` escrito se pierde (se lee el `0x209095B0` anterior) | registros de RCC e I2C2 | — | las dos |
| T14 | BM | CM4 (I2C1), CM7 (I2C4) | taps y notaps | PASS | `Deinit` tras T4/T5 en cada velocidad: 0 registros distintos de la imagen de reset. Bus en reposo: S17 tras cada paso | I2C3 inicializado y sin `Deinit`: distinto en `CR1` y `TIMINGR` (`0x11`); tras `Deinit`, 0 | tabla de reset del RM a mano | — | las dos |
| T15 | BM | CM7 (I2C2 sin pines) | taps y notaps | PASS | `Write` y `Read` `ERR_TIMEOUT` (6), cada una dentro de `max_cycles` + 1 ms (2 000 731 y 2 000 592 ≤ 2 400 000 ciclos); `Deinit`, 0 registros distintos del reset | I2C4 con sus pines en AF termina bien: es T4 en I2C4 (§5.5) | registros, CYCCNT | — | las dos |
| `SetFastModePlus` | BM | CM7 (I2C2) | taps y notaps | PASS | `SYSCFG_PMCR` `0x03000000` → `0x03000002` (solo el bit 1) → `0x03000000` | — | `SYSCFG_PMCR` leído | — | las dos |
| B1 | BM | CM4 y CM7 | taps y notaps | registro | §5.4 | — | DWT, `.map` | — | las dos |
| F334 al final | — | host | taps y notaps | PASS | `scl=1 sda=1`, S15 ok, `uart_ovr 0`, ninguna línea no pedida. `ev`: solo `0x10` (sondeos: 8 en taps, 4 en notaps) y `0x08` (los cortes de los controles de T6 y T7: 4 en cada estado); ningún `BERR`, `ARLO`, `OVR` ni `0x20` | — | — | `STATUS` | las dos |
| Veredicto | — | CM7 + host | taps y notaps | PASS | `bench completed without abort`; CM4 `timeout 0, aborts 0`; salidas por CYCCNT parado 0 en los dos núcleos; pila de `Status_Task` 710 (taps) y 712 (notaps) palabras libres de 1024; host 90/90 y 69/69 | — | — | — | las dos |

Notas:

- **Notaps tiene 21 comprobaciones menos:** sin tomas no hay T5 ni vistas del osciloscopio (T8, T9, T10), y T4 de 100 kHz pasa a 400 kHz.
- **O1, por estado del banco.** `blank` de la F334: 1 en las 5 retenciones de SDA de taps (50 ms de T0C, tres de 100 ms y la de T9 con `EDGES 5`) y 0 en las 5 de notaps. Discrepancias de T4: 0 en taps (80 transferencias) y 0 en notaps (80); de T5: 0 en 20 capturas. Ningún `RPT` con `BERR`, `ARLO` ni `OVR` en ningún estado. El glitch de O1 no aparece en las transferencias.
- **Sesiones de desarrollo** (commiteadas sin editar en `e27c673`): `i2c_dev_f1_bench_notaps_3aa49e3-dirty{,_2,_3}.txt` y `i2c_dev_f1_bench_taps_32cd2e5.txt`. Qué destaparon: §5.8.

### 5.4 Medidas (B1)

| Medida | CM4 (200 MHz) | CM7 (400 MHz) | Fuente |
| --- | --- | --- | --- |
| `I2C_BM_Init` | 312 ciclos | 588 ciclos | DWT, las dos capturas |
| `I2C_BM_Write` de 1 byte | 11 690 (400 kHz) / 5750 (1 MHz) | 22 036 (400 kHz) / 11 472 (1 MHz) | ídem |
| `I2C_BM_XferIrq` por llamada (escritura IT de 16) | 121–240 | 118–332 | ídem |
| `i2c_bm.c.obj` | 5124 / 0 / 0 | 5142 / 0 / 0 | §5.2 |
| `Status_Task` | — | 710 y 712 palabras libres | las dos capturas |

La línea B1 del firmware se corta a 160 caracteres (`App_Log`): pierde el texto fijo "(IT write of 16)", no cifras (§5.8).

### 5.5 Desviaciones del plan

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| `CalcTiming`: N con la división hacia abajo | N = ceil((ceil(k/f) − sync)/(p+1)) | Con el redondeo del plan la frecuencia del modelo salía por encima de `scl_hz` (100 MHz, 100 kHz, t\_r 250, t\_f 20: 100 100 Hz), contra la regla 4 | Hector, 2026-10-07: "ceil en los dos" |
| `WaitEnd(i2c, max_cycles)` | `WaitEnd(i2c, n, max_cycles)` | Una transferencia DMA de más de 255 bytes necesita la cuenta en cada `TCR`; `WaitEnd` cuenta los trozos en su pila (C7) | Hector, 2026-10-07 |
| 2.19.1 en `CheckConfig` | Solo en `CalcTiming` | `I2cBm_Config_t` no tiene la frecuencia del kernel | Hector, 2026-10-07 |
| T12: encender el HSI (`HSION`) | **Corregido (auditoría de F1, m19):** en `e27c673` el código hacía el RMW de `RCC_CR` con `HSION` aunque ya estuviera a 1, así que "no se escribe" era falso. Desde `a5e1135` se escribe solo si estaba a 0 (estaba a 1: no se escribe) y `I2C123SEL` y `HSION` se restauran por separado con read-back (§F1 corrección) | La premisa de mi pregunta 3 era falsa: `RCC_CR` dice `HSION 1`, `HSIRDY 1` antes de T12 | Hector, 2026-10-07 ("HSION solo en T12"); la premisa equivocada la digo aquí |
| T10(b): "`ERR_BUS` por `TIMEOUT` y STOP" | `ERR_BUS` por TIMEOUT y reset por software inmediato (`PE = 0`): las dos líneas libres, **sin STOP** en el bus | RM0399 50:2286-2287 dice que el maestro manda un STOP; en el banco el maestro siguió reteniendo SCL y SDA hasta el presupuesto del llamador (desarrollo, `610e057`). Con el reset, el osciloscopio ve SDA libre a TIMEOUTA (9,885/9,890 ms frente a 9,881) y los dos altos cuando la F334 suelta | Hector, 2026-10-07: "Accept and deliver" |
| T10(b): tiempo del TIMEOUT | El firmware acota el fin por arriba desde que ve SCL bajo (≤ TIMEOUTA + 1 ms) y por abajo desde el `AT HOLD` (≥ (TIMEOUTA − 1) × 2048 / kernel); el tiempo exacto lo mide el osciloscopio (taps, ±5 %) | La F334 baja SCL dentro del intercambio del `AT`, unos 5 ms antes de que CM7 lo vea (desarrollo); la cota inferior desde el `AT` es débil (~400 ms) | No consultado |
| T5: "tiempos ≥ mínimos del modo" | tSU;DAT se juzga en los bits que conduce la H755 (dirección, datos escritos, ACK de lectura); el de la F334 se registra | El tSU;DAT de los bits del esclavo lo pone el SCLDEL de la F334, no el driver. Con 172 ns, el primer bit tras un stretching midió −27 ns a 400 kHz (desarrollo) | No consultado |
| T5: entrada de `CalcTiming` | t\_r del banco con tomas: 300 ns (en notaps, 40 ns) | El osciloscopio mide 248–253 ns (30–70 %); con 200 ns el tSU;DAT de la H755 quedaba por debajo de 250 ns a 100 kHz | No consultado; valor del banco, no del plan |
| T4: 400 kHz "con tomas" | También en notaps (400 kHz y 1 MHz) | Así T4 compara los dos estados a la misma velocidad (O1) | No consultado; ampliación |
| T6–T10: velocidad | La principal de cada sesión: 400 kHz en taps, 1 MHz en notaps | El plan no la fija | No consultado |
| T15: control "I2C4 con sus pines en AF termina bien" | El control es T4 en I2C4 (el mismo paso, mismas funciones), no una línea propia de T15 | T15 corre en F1_NOPIN, sin pines; I2C4 en AF solo existe en F1_CM7 | No consultado |
| F334: un solo `TIMINGR` | `FMP 0` (por defecto) usa `0x20F10000` (SCLDEL 750 ns) y `FMP 1` el de Fm+ (`0x00A20000`); protocolo v3 | El SCLDEL de la F334 es el tSU;DAT de sus bits tras un stretching (fila de T5) | No consultado; herramienta del banco |

### 5.6 Respuesta a la auditoría de F0

Cada corrección: "corregido en <hash>, pendiente de verificación".

| ID | Respuesta |
| --- | --- |
| M1 | Corregido en `8b5fd00`, pendiente de verificación. En recepción con control de byte, el `TCR` consume primero su propio byte (espera `RXNE` 200 µs y 20 000 lecturas como mucho; si no llega, ACK y `ev 0x20`). Orden del ISR: errores, `RXNE`, `TCR`, `TXIS`, `NACKF`, `STOPF`, `ADDR`. T8(b) comprueba las tres vistas: el driver cuenta 3, la F334 `nk=1 rx=4` y el osciloscopio (taps) `… 63A E0N P`. T10(a) comprueba el stretching por byte también en lectura, con el osciloscopio (5 tramos de 203–209 µs). Ningún `ev 0x20` en las capturas |
| m1 | Corregido en `8b5fd00`, pendiente de verificación: IWDG `RLR = 449` (288–480 ms) |
| m2 | Corregido en `8b5fd00`, pendiente de verificación: `src/vectors.c` define los 49 vectores no usados y cada uno llama a `bench_fault_release()`; el startup del CMSIS queda como viene |
| m3 | Corregido en `8b5fd00`, pendiente de verificación: TIM2 0, I2C1 1, EXTI9_5 2, USART2 3 |
| m4 | Corregido en `8b5fd00`, pendiente de verificación: `STOPF` antes que `ADDR` |
| m5 | Corregido en `8b5fd00`, pendiente de verificación: `CORRUPT k` cuenta los bytes que salen al bus, por transferencia (T4 control: índice 5) |
| m6 | Corregido en `8b5fd00`, pendiente de verificación: tokens sobrantes y `EDGES 0` rechazados |
| m7 | Corregido en `8b5fd00` (comentario) y `eeac84f` (el host aborta con `sysclk ≠ 64000000`), pendiente de verificación. `3bb1569` cambia el comentario otra vez: ahora hay dos `TIMINGR` |
| m8 | Corregido en `fc4338e`, pendiente de verificación: la línea dice que la rechaza la regla de dueño, y un segundo veredicto pasa `ist_s16_pins_ok()` sobre los registros vivos de GPIOB con PB8/PB9 en AF4 (AF6 rechazado, AF4 aceptado como control) |
| m9 | Corregido en `fc4338e`, pendiente de verificación: `bench_abort()` publica `IST_STEP_NONE` |
| m10 | Corregido en `fc4338e`, pendiente de verificación: `_Static_assert` de ≤ 32 casos en cada tabla de los dos núcleos |
| m11 | Corregido en `eeac84f`, pendiente de verificación: `main()` escribe el resumen, `HOST VERDICT` y la captura en un `finally`, termine como termine |
| m12 | Corregido en `eeac84f`, pendiente de verificación: T0d y "No Digital Out" son líneas `RECORD`, no se cuentan ("2 records not counted") |
| m13 | Corregido en `eeac84f`, pendiente de verificación: S17 según R2-1 (taps: razón 0,956 ± 0,015 y 2+ ≥ 3,0 V; notaps: 1+ < 0,10 V y 2+ ≥ 3,0 V) y cada fallo nombra su línea y su sonda; el mock de T0B(a) lo prueba |
| m14 | Corregido en `eeac84f`, pendiente de verificación: exactamente una aparición de cada pin, con límite de palabra |
| m15 | Corregido en `eeac84f`, pendiente de verificación: `--checklist-confirmed` y `--checklist-date` van juntas, fecha ISO; mock en T0B(a) |
| m16 | Corregido en `eeac84f`, pendiente de verificación: `H755Console.expect(stop_on_abort=True)` en `step()` |
| O1 | Respondido en §5.3 (notas): `blank` y discrepancias separadas por estado |
| O2 | Sin cambio: el mock sigue probando `f334_hold_guard`; `F334.hold()` lo llama antes de escribir y en F1 cada `HOLD_END` se juzga contra lo pedido |
| O3 | Sin cambio: en `tools/monitor.py` solo puedo añadir (A23); falla en seguro |
| O4 | Sin cambio en el `_sink` de respaldo; con m11, un hilo muerto acaba en `BenchAbort` y la captura se escribe igual |
| O5 | R2-1 del plan: implementado en `eeac84f` (ver m13) |

### 5.7 Erratas del plan encontradas

| Sección del plan | Qué dice | Qué es correcto | Fuente |
| --- | --- | --- | --- |
| Reglas de cálculo, N | Los paréntesis del redondeo de N admiten dos lecturas, y la de división hacia abajo da f > `scl_hz` | ceil en los dos (aprobado) | §5.5 |
| Contrato, `CheckConfig` | Comprueba 2.19.1 | No puede: `I2cBm_Config_t` no lleva la frecuencia del kernel | §5.5 |
| Contrato, `WaitEnd` | `WaitEnd(i2c, max_cycles)` | Necesita `n` | §5.5 |
| T10(b) | "→ `ERR_BUS` por `TIMEOUT` y STOP" | En rev V no hay STOP; el driver suelta el bus con el reset por software. Lo que se ve: `ERR_BUS`, SDA libre a TIMEOUTA, bus libre al final | RM0399 50:2286-2287 frente a las capturas |
| (mío) `i2c_bm.c` y `610e057` | Citaban 50:2276-2278 para el STOP automático | 50:2286-2287. Corregido en `d326575` (el comentario); el mensaje de `610e057` no se puede cambiar | RM0399 cap. 50 |

### 5.8 Problemas abiertos y no ejercitado

**Lo que pasó en las sesiones de desarrollo** (capturas en `e27c673`):

1. **La F334 cortaba transferencias vivas** (`notaps_3aa49e3-dirty`: 37 informes, `_2`: 73): `RPT ev=08` con `us` de unos cientos y el maestro leyendo `0xFF` desde ahí. El corte por inactividad leía `us_now()` y luego `s_last_us`; una interrupción entre las dos hacía negativa la diferencia. Corregido en `9a8f918`.
2. **Aborto S15 falso en la F334** al soltar una retención por `EDGES` (T9, `notaps_3aa49e3-dirty`): la comprobación leía `MODER` y luego `s_hold_pin`. Corregido en `9a8f918`.
3. **T10(b): el maestro no soltaba el bus tras un TIMEOUT** hasta el presupuesto del llamador (§5.5, §5.7). Corregido en `610e057`.
4. **T10(b): la medida incluía la impresión por UART** (`notaps_3aa49e3-dirty_3`: 46 ms en el control). Corregido en `7a22a3f`, que además acota el tiempo como dice §5.5.
5. **T5 con tomas** (`taps_32cd2e5`): tSU;DAT de −27 ns, que era el primer bit de la F334 tras su stretching, no un bit de la H755 (`e346766`, `3bb1569`). tSU;DAT de la H755 por debajo de 250 ns a 100 kHz con t\_r = 200 ns de entrada (`7a22a3f`). Capturas que no llegaban al STOP de un `WriteRead` o a la escritura que sigue a un sondeo (`7a22a3f`).
6. **T9 con tomas** (`taps_32cd2e5`): armar el osciloscopio tardó más que la retención de 300 ms y la F334 soltó por tiempo, sin flancos (`RecoverBus` vio el bus libre, `pulses 0`). Corregido en `7a22a3f`: el osciloscopio se arma antes de la retención.

**Abierto:**

- **T10 en el osciloscopio: 45 glitches** en cada una de las 4 capturas de T10(a) (4 MS/s, tramos de 250 ns frente a un tHIGH de unos 600 ns). La decodificación es exacta y el umbral de 0,5 V da otra cosa. Creo que el contador de glitches (pulsos más cortos que su mínimo) es demasiado estricto para 4 MS/s, pero no lo he comprobado. T10 no juzga glitches.
- **La línea B1** pierde su texto final por el corte de 160 caracteres de `App_Log` (§5.4).
- **T10(b) en notaps:** sin osciloscopio, el tiempo del TIMEOUT queda acotado por arriba (≤ 11 ms desde que CM7 ve SCL bajo) y solo débilmente por abajo (§5.5).

**No ejercitado en F1** (el README del cierre debe copiarlo entero):

- **T11 (`ARLO`) y multimaestro:** un solo maestro en el banco (S16). `TakeErrors` con `ARLO` no se ha visto nunca a 1.
- **ES0445 2.19.2** (`BERR` en modo maestro): no se puede provocar; el driver lo borra y sigue (regla), sin evidencia en el banco: ningún `BERR` en las capturas. **2.19.3, 2.19.5, 2.19.7:** fuera de alcance (plan).
- **2.19.6 (T12):** el control a mano no reprodujo la detención: "no reproducido" (decisión 6).
- **Esclavo, 10 bits, SMBus/PEC, `TIDLE = 1`, `TIMEOUTB`:** fuera del contrato.
- **I2C2 e I2C3:** sin pines; solo imagen (T2), bancos (T13), rechazos (T1) y T15. I2C1 con DMA solo desde CM4 (DMA2; DMA1 no) e I2C4 con BDMA solo desde CM7.
- **Tiempos a 1 MHz:** no medidos (D3); 1 MHz se valida por datos (T4, T6–T10, T12) y duración.
- **Fm+ (`SetFastModePlus`):** el bit de I2C2 sin bus (fila propia de §5.3), y los de I2C1 e I2C4 puestos y quitados alrededor del tráfico a 1 MHz (`SYSCFG_PMCR 0x03000001` y `0x03000008`, notaps), con `FMP 1` en la F334. El efecto eléctrico del bit no se midió (sin osciloscopio a 1 MHz, D3).
- **`RecoverBus` con la línea SCL retenida:** solo SDA (T9); la rama de SCL bajo devuelve `ERR_BUS` por lectura del código, sin prueba.
- **`I2C_BM_DmaStartWrite` con `n` < 2:** rechazo probado (T1); `n` de más de 65 535 por DMA: rechazado (T1); el reparto en trozos de 255 solo con 256 y 4096 bytes (T7).
- **`I2C_BM_GetKernelSource`:** solo con el kernel de `prj_01` (PCLK), en la tabla de capacidades; no se llamó con el HSI de T12 ni con otra fuente.
- **El estado "GPIO" del banco** y los otros bancos (`gpio`, `tim`, `spi`): no se ejecutaron en F1.
- **Arranque en frío:** las sesiones empiezan con un reset de sistema por SWD (igual que F0, §5.5 de F0).
- **Silicio distinto de rev V** y árboles de reloj distintos del de `prj_01`, salvo `CalcTiming` (T3).

## F1, corrección (revisión 3)

Responde a la auditoría de F1, parte 1 (`AUDIT.md`, `16964d1`), con el plan en su revisión 3 (`063c299`). Esta sección sustituye a la de F1 en lo que cambia: las pruebas (§5.3), las desviaciones (§5.5) y lo abierto (§5.8) de aquí son las vigentes. La sección de F1 queda como se entregó, salvo la frase de `HSION` de su §5.5, que era falsa y está corregida allí (m19).

### 5.1 Commits

Rango: `063c299..` el commit que añade esta sección, en `driver/i2c`.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `daf1bbe` | fix(i2c): answer the F1 audit in i2c_bm (M2, m28-m32, R3, O8-O10) | M2, m28–m32, R3-1/2/3/5/6/8/9, O8, O9, O10 |
| `9b6db50` | fix(tools): answer the F1 audit in the F334 bench target (m17, O14) | m17, O14 |
| `a5e1135` | test(prj_01): answer the F1 audit in the 05_i2c selftest (M2, m18-m22, m24) | M2 (T9(b)), m18–m22, m24, m25 (firmware), O15, R3-4 |
| `5e44044` | test(prj_01): answer the F1 audit in i2c_hw_check.py (M3, m23, m25-m27, O13) | M3, m23, m25–m27, O13 |
| `4746d7b` | docs(i2c): add the first F1 correction sessions at 5e44044 | — (notaps PASS; taps FAIL, §5.8) |
| `3d0ad6d` | test(prj_01): fix the two failures of the taps session at 5e44044 | fallos propios (§5.8) |
| `6177624` | docs(i2c): add the taps F1 correction session at 3d0ad6d | — (taps FAIL, §5.8) |
| `9c07b93` | test(prj_01): T5 lets the target stretch before the address acknowledge | fallo propio (§5.8) |
| `89847dc` | docs(i2c): add the F1 correction delivery captures at 9c07b93 | — |
| `f5f5154` | docs(i2c): add the F1 correction build evidence at 89847dc | — |
| (este) | docs(i2c): add the F1 correction section of the implementation report | — |

Las imágenes de las dos capturas de entrega son de `9c07b93`; la evidencia de build y de `.map`, de `89847dc` (mismo código).

### 5.2 Build y tamaños

Todo en `89847dc`, árbol limpio:

- **`build_all -Clean`:** 8/8 (`docs/i2c_f1_build_all_89847dc.txt`).
- **`selftest-i2c` desde un directorio vacío:** configure 0, build 0, 0 líneas `warning:`/`error:` (`docs/i2c_f1_selftest_build_89847dc.txt`).
- **`-Werror`:** `selftest-i2c`, `debug` y `release`, 0 avisos en las tres (`docs/i2c_f1_werror_89847dc.txt`).
- **F334:** `build.ps1 -Clean` y `-Clean -Werror`, `Warnings : 0`, `exit=0` (`docs/i2c_f1_f334_build_89847dc.txt`); 11340/20/3392 (FLASH 11360 B, RAM 3408 B).
- **`git diff cc8aa2c -- 02_firmware_projects`:** vacío.

`.map` (`docs/i2c_f1_map_sizes_89847dc.txt`): `prj_01` debug y release, CM7 y CM4, iguales a `2481396` en cada sección de la referencia (29304/132/55668, 3456/24/12560, 28356/132/55672, 3112/24/12560); las secciones que la referencia no lista miden 0.

| Objeto | Núcleo | .text (+.rodata) | .data | .bss |
| --- | --- | --- | --- | --- |
| `i2c_bm.c.obj` | CM7 | 5174 | 0 | 0 |
| `i2c_bm.c.obj` | CM4 | 5156 | 0 | 0 |
| `i2c_selftest.c.obj` | CM7 | 35849 | 8 | 911 |
| `i2c_selftest_common.c.obj` | CM7 | 4201 | 0 | 0 |
| `i2c_selftest.c.obj` | CM4 | 5583 | 4 | 65951 |
| `i2c_selftest_common.c.obj` | CM4 | 4169 | 0 | 0 |

`i2c_bm.c.obj` crece 32 B por los read-back de m31.

### 5.3 Pruebas

Capturas de entrega, imágenes de `9c07b93`:

- **taps:** `docs/i2c_f1_bench_taps_9c07b93.txt`: 95/95 comprobaciones del host (22 `RECORD` sin contar: 20 de f\_SCL en bruto, T0d y "No Digital Out"), `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`;
- **notaps:** `docs/i2c_f1_bench_notaps_9c07b93.txt`: 73/73 (2 `RECORD`), `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`.

Las dos: Rev `9c07b93` e id `7f44332da01807d9` en host, CM7 y CM4; F334 Rev `9c07b93`, id `1b42e3699a62bd2a` (el del host), protocolo v3; `tree clean`; checklist confirmado por Hector el 2026-10-07 (notaps: "notaps (J2/J3 quitados)" al empezar y "listo" tras quitarlos de nuevo; taps: "listo" tras ponerlos). CLKREF: +1,35 % (taps) y +1,31 % (notaps).

Las filas de F1 que no cambian dan los mismos resultados que en `e27c673` (tabla de F1, §5.3); estas son las nuevas o las que cambian:

| ID | Tier | Núcleo | Estado del banco | Resultado | Valor medido | Control negativo: valor | Oráculo | Oráculo F334/AD2 | Captura |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| T0b S16 vivo en CM7 (m18) | — | CM7 | taps y notaps | PASS | En T0B_CM7, PB9 puesto en AF4 con I2C1 a `PE = 0` (`MODER 0xDFF8FEBD`, `AFRH 0x00000040`): `bench_claim()` de PB8 en AF6 rechazado por la comprobación de pines; la regla de dueño lo acepta | PB9 de nuevo en entrada: el mismo reclamo, aceptado | `MODER`/`AFR` vivos | — | las dos |
| T1 | BM | CM7, CM4 | taps y notaps | PASS | 62 casos exactos (O15) en cada núcleo, 0 con otro estado; con `BUSY = 1`, 8 exactos | Un `Init` válido cambia la foto | casos a mano | `HOLD_END` | las dos |
| T2 (m22) | BM | CM7 (I2C2/3/4), CM4 (I2C1) | taps y notaps | PASS | 120 imágenes por instancia, 0 distintas | La misma comparación contra la imagen esperada con DNF + 1 (mod 16): distinta en los 120 casos de cada instancia | campos del RM a mano | — | las dos |
| T3 (m23) | BM | CM7 + host | taps y notaps | PASS | 403 vectores, 0 distintos de `calc_timing()`; 286 OK que cumplen el RM | La misma comparación (`t3_judge`) con el TIMINGR esperado alterado (SCLL + 1): 286 de 286 distintos; en T0B(a), 3 de 3 sobre vectores del host | `calc_timing()`, `timingr_rm_problems()` | — | las dos |
| T5 (m25, M3) | BM | CM4, CM7 | taps | PASS | 20 capturas decodificadas exactas. **f\_SCL en bruto (AD2): 100 901–101 082 Hz con 100 kHz pedidos (+0,90 a +1,08 %) y 403 078–404 146 Hz con 400 kHz (+0,77 a +1,04 %)**; normalizada con CLKREF: 99 555–99 733 Hz (modelo 99 700) y 397 699–398 753 Hz (modelo 398 406), juzgada ≤ lo pedido y a ± 5 % del modelo. Cada tLOW y tHIGH contra [contador − tol, contador + sync + tol], sync con filtro analógico y DNF 0 (los del `Init`, en la línea SCOPE) a su retardo máximo: todos dentro: 100 kHz tLOW mínima 5,29–5,31 µs y mediana 5,31–5,32 µs (ventana 4,77–6,00), tHIGH 4,26–4,31 µs (3,94–5,17); 400 kHz tLOW mínima 1,57–1,58 µs y mediana 1,58–1,59 µs (1,19–2,08), tHIGH 0,60–0,64 µs (0,42–1,31). Donde la F334 estira SCL (antes del ACK de su dirección, 5,4–8,4 µs, y antes del primer byte que devuelve, 3,4–5,9 µs) solo la cota inferior (§5.5). tSU;DAT de la H755 ≥ 4,81 µs (100 k) y ≥ 1,07 µs (400 k); de la F334, registrado, 525–533 ns. TIMINGR de cada captura igual al de `calc_timing()` del host | Intercambio de canales: distinto en las 20 | `expected_frames()`, `t5_timing_problems()` | AD2 a 12,5/25 MS/s (§5.5) | taps |
| T9 | BM | CM7 | taps y notaps | PASS | Retención después del `Init`: `ISR 0x00008001` (BUSY), `Write` `ERR_BUS` en 350 ciclos; `RecoverBus` OK, 5 pulsos; F334 `EDGES`; osciloscopio (taps) 5 pulsos y STOP | Sin `RecoverBus`: SDA abajo 95 ms | `IDR`, `pulses` | `HOLD_END`; AD2 | las dos |
| **T9(b)** (M2, R3-6) | BM | CM7 | taps y notaps | PASS | La F334 retiene SDA **antes** del `Init`: `Init` OK, `ISR 0x00000001` (BUSY 0), `CheckTransfer` OK; **`Write` → `ERR_ARLO`** en 4052 ciclos (taps) y 1777 (notaps), dentro de su presupuesto de 2 ms; SCL alto y SDA bajo después; `RecoverBus` OK con **4 pulsos** (el `Write` rechazado dio el primero de los 5 flancos de la F334), PB8/PB9 = 3; AF6, `Init` y una escritura correcta. Osciloscopio (taps): 5 subidas de SCL antes de soltar SDA y el STOP. F334: `cause=EDGES`, `edges=5` | T9: la retención después del `Init` da `ERR_BUS` | `ISR`, `IDR`, `pulses` | `HOLD_END`; AD2 (taps) | las dos |
| T10(b) | BM | CM4, CM7 | taps y notaps | PASS | `ERR_BUS` por TIMEOUT, fin 4,1–5,1 ms después de ver SCL bajo (≤ 11,0); osciloscopio (taps): SDA libre a 9,882/9,885 ms de la bajada de SCL (TIMEOUTA 9,881 ms en tiempo del AD2); sin STOP (R3-5) | Sin `timeout_scl_low`: `ERR_TIMEOUT` a los 30 ms | CYCCNT | AD2 | las dos |
| T12 (m19) | BM | CM4 | taps y notaps | PASS | Igual que en F1; `HSION` estaba a 1: **no se escribe** (`written 0`); `I2C123SEL` y `HSION` restaurados por separado, con read-back | A mano sin precarga: 0 detenciones, "no reproducido" | estado del driver | `STATUS` | las dos |
| T14 (m24) | BM | CM4, CM7 | taps y notaps | PASS | El `Deinit` de cada prueba juzgado contra la imagen de reset: tras T4/T5 por velocidad, el control de T6, T10(a), T10(b), su control, antes y después de T12, el resto de cada paso, T9, el `Write` de T9 y el de T9(b), y T9(b): 0 registros distintos en todos (20 veredictos en cada sesión) | I2C3 sin `Deinit`: distinto en `0x11` | tabla de reset a mano | — | las dos |
| Fm+ (m20) | BM | CM7 | taps y notaps | PASS | Al final de F1_CM4, F1_CM7 y F1_RECOVER, `SYSCFG_PMCR` I2C1FMP\|I2C4FMP = 0 (los devuelve el propio paso); `bench_safe()` los limpia en todo aborto | — | `SYSCFG_PMCR` | — | las dos |
| Listas del host (m26) | — | host | taps y notaps | PASS | taps: 178 líneas X, las 178 esperadas y nada más; 30 capturas, las 30 esperadas; nada pendiente. notaps: 154 líneas X | En T0B(a): un tamaño de T4 quitado, una escritura de más y una captura de T5 quitada, detectados | `expected_transfers()`, `expected_scopes()` | — | las dos |
| S19 en el host (m27) | — | host | taps | PASS (sin disparos) | Una línea X o una captura por encima de 400 kHz, o `FMP 1`, con tomas, terminan la sesión | — | — | — | taps |

**f\_SCL en esta placa (M3, R3-7).** El HSE de la placa va un +1,3 % (CLKREF: la CPU y los kernels I2C a +1,31–1,35 % del nominal). La aritmética de `CalcTiming` es exacta para el reloj nominal y deja el modelo un 0,3 % por debajo de lo pedido (99 700 Hz, 398 406 Hz), así que el bus va en bruto un **+0,8 a +1,1 %** por encima de lo pedido: 101 kHz con 100 kHz pedidos y 403–404 kHz con 400 kHz. Eso pasa el máximo de Sm y de Fm. Un producto con este reloj debe pedir con margen (por ejemplo 98 kHz y 390 kHz) o corregir la frecuencia del kernel que pasa a `CalcTiming`.

Notas:

- **Notaps tiene 22 comprobaciones menos** que taps (T5, las vistas del osciloscopio de T8, T9, T9(b), T10, y T4 a 100 kHz pasa a 400 kHz).
- **O1, por estado:** `blank=1` en las retenciones de SDA con tomas y `blank=0` sin ellas; 0 discrepancias en T4 en los dos estados; ningún `BERR`, `ARLO` ni `OVR` en los `RPT`.

### 5.4 Medidas (B1)

Iguales a F1 en `Init`, `Write` de 1 byte y `XferIrq` (las líneas B1 de las dos capturas). `i2c_bm.c.obj`: 5156 B (CM4) y 5174 B (CM7), `.data = .bss = 0`. Pila de `Status_Task`: 700 (taps) y 720 (notaps) palabras libres de 1024.

### 5.5 Desviaciones del plan (vigentes)

Las aprobadas el 2026-10-07 son ahora la revisión 3 (R3-1 a R3-5) y no son desviaciones. Quedan:

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| T5: osciloscopio a 20 MS/s (100 kHz) y 50 MS/s (400 kHz) | 12,5 MS/s y 25 MS/s | Con el buffer de 8192 muestras del AD2 (`FDwfAnalogInBufferSizeInfo`), un `WriteRead` 1+1 a 100 kHz (unos 400 µs) no cabe a 20 MS/s (410 µs con el pre-disparo) ni un sondeo y su escritura a 400 kHz a 50 MS/s. La tolerancia de cada intervalo usa las muestras reales (± 2 muestras por flanco) | No consultado (m25 pide la desviación listada) |
| T5: "tiempos … dentro de la predicción de `CalcTiming`" | Cada tLOW y tHIGH en [contador − tol, contador + sync + tol]; donde el target puede estirar SCL (antes del ACK de su dirección, por `ADDR`, y en el bit 0 de un byte tras la dirección) solo la cota inferior; esos periodos se imprimen | La F334 estira 5,4–8,4 µs antes del ACK de su dirección en cada trama y 3,4–5,9 µs antes del primer byte que devuelve (UM10204 3.1.9): no es el contador del maestro | No consultado |
| T5: tSU;DAT ≥ el mínimo del modo | Se juzga en los bits que conduce la H755; el de la F334 se registra | Lo pone el SCLDEL de la F334 | No consultado |
| T5: entrada de `CalcTiming` con tomas | t\_r 300 ns (notaps 40 ns) | El osciloscopio mide 248–253 ns (30–70 %) | No consultado |
| T4: 400 kHz "con tomas" | También en notaps | Compara los dos estados a la misma velocidad (O1) | No consultado; ampliación |
| T6–T10, T9(b): velocidad | La principal: 400 kHz (taps), 1 MHz (notaps) | El plan no la fija | No consultado |
| T9(b): "la transferencia termina acotada … dentro de su presupuesto" | Presupuesto de 2 ms para el `Write` (T9: 5 ms) | La captura de T9(b) dispara en la bajada de SCL del `Write` y dura 8 ms a 1 MS/s: la recuperación tiene que caber | No consultado |
| T15: control "I2C4 con sus pines en AF termina bien" | Es T4 en I2C4 | T15 corre sin pines (F1_NOPIN) | No consultado |
| m18: "control vivo … con PB9 realmente en AF4" en un paso de CM7 | PB9 a AF4 por una escritura directa de CM7 (el fallo inyectado), solo con I2C1 con reloj y `PE = 0` leído, PB9 open-drain sin pull, con interrupciones enmascaradas y unos microsegundos | En un paso de CM7 ningún reclamo puede poner AF4 (S16): es la única forma de crear el estado. I2C1 a `PE = 0` no conduce nada | No consultado; lo pide la auditoría |
| F334: un `TIMINGR` | `FMP 0` → `0x20F10000` (SCLDEL 750 ns), `FMP 1` → `0x00A20000` | Ver F1 §5.5 | No consultado; herramienta del banco |

### 5.6 Respuesta a la auditoría de F1

| ID | Respuesta |
| --- | --- |
| M2 | Corregido en `daf1bbe` (encabezado, R3-6) y `a5e1135` (T9(b) en el selftest y el host), pendiente de verificación. El encabezado dice que `BUSY` solo refleja un START visto con `PE = 1` (50:2897-2899), que un bus retenido antes del `Init` no se ve, que antes del primer `Init` tras un arranque el llamador lee SCL y SDA con su `I2cBm_Recovery_t` (o llama a `RecoverBus`) y que `ERR_ARLO` en un bus de un solo maestro es síntoma de bus colgado; lo repiten `Init`, `CheckTransfer` y los códigos de las transferencias. T9(b): `ERR_ARLO` en las dos capturas (§5.3) |
| M3 | Corregido en `5e44044` (registro de f\_SCL en bruto en cada captura) y en este informe (§5.3), pendiente de verificación. La cifra medida es +0,8 a +1,1 % sobre lo pedido (el +1,3 % es el del reloj; el modelo queda un 0,3 % por debajo) |
| m17 | Corregido en `9b6db50`, pendiente de verificación: `s_rx_open` en `rx_take` con SBC, borrado en `TCR`, `ADDR` y al empezar la transferencia; `TCR` espera `RXNE` solo sin byte abierto. T8(b): `nk=1 rx=4` en las dos capturas, ningún `ev 0x20` |
| m18 | Corregido en `a5e1135`, pendiente de verificación: `t0b_cm7_live_s16()` en T0B_CM7 (§5.3, §5.5) |
| m19 | Corregido en `a5e1135`, pendiente de verificación: `HSION` solo si estaba a 0 (no se escribe: estaba a 1), restauraciones separadas con read-back. La frase de F1 §5.5 está corregida |
| m20 | Corregido en `a5e1135`, pendiente de verificación: `bench_safe()` limpia I2C1FMP e I2C4FMP (RMW de un bit cada uno), los pasos los apagan en todo camino que los encendió, y un veredicto por paso los juzga |
| m21 | Corregido en `a5e1135`, pendiente de verificación: CLKREF, el fin de la lectura por BDMA (sin `HAL_DMA_PollForTransfer`, por CYCCNT) y las esperas de CM4 (fin de la DMA de lectura, T12 a mano, B1) salen si CYCCNT se para |
| m22 | Corregido en `a5e1135`, pendiente de verificación (§5.3) |
| m23 | Corregido en `5e44044`, pendiente de verificación (§5.3) |
| m24 | Corregido en `a5e1135`, pendiente de verificación: `deinit_judged()` en cada `Deinit` (§5.3) |
| m25 | Corregido en `5e44044`, `3d0ad6d` y `9c07b93`, pendiente de verificación: cada periodo contra la ventana, filtro real de la línea SCOPE; las velocidades de muestreo, listadas como desviación (§5.5) |
| m26 | Corregido en `5e44044`, pendiente de verificación: listas de transferencias y capturas por sesión, comprobación final sin pendientes, mock en T0B(a) |
| m27 | Corregido en `5e44044`, pendiente de verificación: el host termina la sesión con una X o una captura > 400 kHz, o `FMP 1`, con tomas |
| m28 | Corregido en `daf1bbe`, pendiente de verificación: la fórmula de N con ceil en los dos sitios, en el encabezado (R3-1) |
| m29 | Corregido en `daf1bbe`, pendiente de verificación: `Init` escribe `TIMEOUTR` tres veces siempre (`TIMOUTEN = 0`, `TIMEOUTA`, `TIMOUTEN`), 50:2861 |
| m30 | Corregido en `daf1bbe`, pendiente de verificación: códigos de `Init`, `Deinit`, `Write/Read/WriteRead`, `StartIt`, `x->status`, `done_tx` tras ARLO/TIMEOUT, `DmaStart*`, `WaitEnd`, `DmaEnd`, `RecoverBus` (`ERR_TIMEOUT`) y `TakeErrors` en el encabezado |
| m31 | Corregido en `daf1bbe`, pendiente de verificación: read-back de `TXDMAEN`/`RXDMAEN` antes de `START`; si falla, el bit vuelve a 0, `TXDR` vaciado y no hay `START` |
| m32 | Corregido en `daf1bbe`, pendiente de verificación: "misma prioridad EV y ER" en el encabezado y en `XferIrq` (el selftest usa 6 en los dos) |
| O8 / R3-8 | Corregido en `daf1bbe`: las `I2C_BM_ERRF_*` documentadas como API |
| O9 | Corregido en `daf1bbe` (encabezado): una transferencia por interrupción solo se abandona con `Deinit`. Para el README |
| O10 / R3-9 | Corregido en `daf1bbe`: 50:2782, 50:2858, 50:2866, 50:2645, 50:2861 (comprobadas en el texto del RM) |
| O13 | Corregido en `5e44044`: `s15=ok` con límite de palabra (dos casos más en el mock); sin CLKREF, T5 y T10(b) fallan; el TIMINGR de T5 se recalcula con `calc_timing()` del host; las esperas de T0B con `stop_on_abort`. La independencia textual de T3 no cambia |
| O14 | Corregido en `9b6db50`: retardo de SDA de 62,5 ns (50:309-310) y el t\_r del banco en el comentario; `FMP`, `INIT` y `hold_start` comprueban y actúan enmascarados; `hold_release` enmascarado; `ln_pins` lee S15 enmascarado; `bench.h` dice v3; "1 MHz solo con `FMP 1`" en el comentario y en el README (con `FMP 0` cada SCL bajo dura al menos 812 ns, 50:347-349) |
| O15 | Corregido en `a5e1135`: T1 exige 62 y 8 casos exactos; comentarios de `CM7/CMakeLists.txt` y `pin_cfg`, `k_exp_dummy` quitado. Sin cambio, para el README: el reposo de 120 s de CM4, el resultado tardío de CM4 tras un timeout de 30 s, la guarda de 1 MHz que solo lee PF14 (hueco del plan) |
| O6, O7, O11, O12 | Sin cambio en el código; para el README: el START + STOP de `RecoverBus` (también en su comentario, `daf1bbe`); el PRIMASK de `SYSCFG_PMCR` frente al otro núcleo; el contador de glitches por muestras y el umbral de 0,5 V que no discrimina; tSU;DAT atribuido por el papel del bit |

### 5.7 Erratas del plan encontradas

| Sección del plan | Qué dice | Qué es correcto | Fuente |
| --- | --- | --- | --- |
| R3-7 | "el bus va un +1,3 % por encima de lo pedido" | El reloj va +1,3 %; el bus, en bruto, +0,8 a +1,1 % (el modelo de `CalcTiming` queda un 0,3 % por debajo de lo pedido): 100,9–101,1 kHz y 403,1–404,1 kHz | capturas de §5.3 |
| T9 (R3-6, control) | "la retención después del `Init` (T9, `ERR_BUS`)"; la fila de T9 admite `ERR_BUS` o `ERR_TIMEOUT` | Las dos capturas dan `ERR_BUS`; el selftest sigue aceptando los dos, como la fila | capturas |

### 5.8 Problemas abiertos y no ejercitado

**Lo que pasó en las sesiones de esta corrección:**

1. **`taps_5e44044`** (90/95, FW PASS): (a) m25 recién aplicado juzgó también los periodos bajos que estira la F334 y fallaron 4 grupos de T5; (b) la captura de T9(b), que dispara en la bajada de SCL del `Write` rechazado, se quedó sin la recuperación: la línea de veredicto del `Deinit` (UART bloqueante) la retrasó más allá de los 8 ms. `3d0ad6d` exime los bits 0 de byte tras la dirección y juzga el `Deinit` después de la recuperación.
2. **`taps_3d0ad6d`** (91/95): T9(b) ya pasa; quedaba un periodo largo por trama en un sitio que la regla no cubría. Una sesión de desarrollo con la posición impresa (`i2c_dev_f1_bench_taps_3d0ad6d.txt`, 95/95) mostró que es el ACK de la dirección (el `ADDR` de la F334, 5,4–8,4 µs). `9c07b93` lo añade.
3. **`notaps_5e44044`** (73/73) pasó; no es la de entrega porque el firmware cambió en `3d0ad6d`.

**Abierto:** lo de F1 §5.8 sigue (glitches del decodificador, ahora O11; la línea B1 cortada; T10(b) en notaps acotado solo por arriba).

**No ejercitado** (además de lo de F1 §5.8, que sigue vigente):

- T9(b) solo con el silicio de esta placa: `ERR_ARLO` es lo que da rev V; `ERR_BUS` y `ERR_TIMEOUT`, aceptados por R3-6, no han aparecido.
- m31: el camino `ERR_VERIFY` de `DmaStart*` (el bit DMA que no se relee) no se puede provocar.
- m29: un `Init` sin `Deinit` previo con `TIMOUTEN = 1` no está en ninguna prueba; el `Init` de T10(b) tras uno con timeout pasa por `Deinit`.
- La ruta de `bench_safe()` que limpia Fm+ (m20) solo corre al arrancar y en los abortos, y ninguna sesión abortó con el bit puesto.
- El aborto por S19 del host (m27) no se disparó (es su caso bueno); su lógica no tiene mock.

## F1, corrección 2

Responde a la auditoría de F1, parte 2 (`AUDIT.md`, `be202f4`): m33, m34, m35 y las observaciones O16–O20. Esta sección actualiza a la "F1, corrección (revisión 3)" en lo que cambia; lo demás sigue como allí.

### 5.1 Commits

Rango: `be202f4..` el commit que añade esta sección.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `6fa2439` | fix(i2c): software-reset in WaitEnd when CYCCNT is not running (m33, O17, O18) | m33, O17, O18 |
| `3d92f3e` | test(prj_01): answer the F1 audit, part 2 (m34, m35, O16, O18, O20) | m34, m35, O16, O18, O20 |
| `a03ee3f` | docs(i2c): add the F1 correction-2 sessions at 3d92f3e | — (notaps PASS; taps FAIL, §5.8) |
| `fefacf4` | test(prj_01): T5 judges the mode's minimums with 2 samples of tolerance | decisión del responsable (§5.5) |
| `e66f8ba` | docs(i2c): add the F1 correction-2 delivery captures at fefacf4 | — |
| `90aef10` | docs(i2c): add the F1 correction-2 build evidence at e66f8ba | — |
| (este) | docs(i2c): add the F1 correction-2 section of the implementation report | — |

### 5.2 Build y tamaños

En `e66f8ba` (el código de `fefacf4`), árbol limpio: `build_all -Clean` 8/8 (`docs/i2c_f1_build_all_e66f8ba.txt`); `selftest-i2c` desde un directorio vacío con 0 avisos (`..._selftest_build_e66f8ba.txt`); `selftest-i2c`, `debug` y `release` con `-Werror`, 0 avisos (`..._werror_e66f8ba.txt`); F334 `-Clean` y `-Clean -Werror`, `Warnings : 0` (`..._f334_build_e66f8ba.txt`); `.map` (`..._map_sizes_e66f8ba.txt`): `prj_01` igual a `2481396` en cada sección de la referencia, `i2c_bm.c.obj` 5174 B (CM7) y 5156 B (CM4) con `.data = .bss = 0`; `git diff cc8aa2c -- 02_firmware_projects` vacío.

`i2c_bm.c.obj` mide lo mismo que en `89847dc` aunque m33 añade una llamada: el compilador comparte la secuencia "reset y devolver" con los otros caminos de `WaitEnd`. En el desensamblado de `I2C_BM_WaitEnd` (`selftest-i2c`, CM7), la comprobación de `cyccnt_running()` salta a `mov r0, r6; bl soft_reset; movs r0, #6`.

### 5.3 Pruebas

Capturas de entrega, imágenes de `fefacf4`:

- **taps:** `docs/i2c_f1_bench_taps_fefacf4.txt`: 96/96 comprobaciones del host (22 `RECORD`), `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`;
- **notaps:** `docs/i2c_f1_bench_notaps_fefacf4.txt`: 74/74 (2 `RECORD`), `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`.

Rev `fefacf4` e id `470e954916ca1651` en host, CM7 y CM4; F334 Rev `fefacf4`, id `5e0c7c9096a308bc` (el del host); `tree clean`; checklist confirmado por Hector (notaps al empezar, "notaps (J2/J3 quitados)"; taps y notaps de nuevo con "listo" tras cada cambio de J2/J3). Una comprobación más que en la corrección en cada estado: la de m34. T0B(a) tiene dos mocks nuevos (m35 y los mínimos de T5).

| ID | Tier | Núcleo | Estado del banco | Resultado | Valor medido | Control negativo: valor | Oráculo | Oráculo F334/AD2 | Captura |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **T9(b)** (M2, R3-6, m34) | BM | CM7 + host | taps y notaps | PASS | La F334 retiene SDA **antes** del `Init`: `Init` OK, `ISR 0x00000001` (BUSY 0), `CheckTransfer` OK; `Write` → **`ERR_ARLO`** en 4050 (taps) y 1847 (notaps) ciclos, dentro de su presupuesto (2 ms, juzgado ≤ 2 ms + 1 ms); **tras el `Write`, PB8/PB9 = 1: SCL alto, SDA bajo (juzgado)**; por eso `RecoverBus` debe dar de 1 a 5 pulsos (con SDA alto, ninguno): **4**, PB8/PB9 = 3 después; AF6, `Init` y una escritura correcta. **Host:** pulsos + flancos de subida de SCL del `Write` = la cuenta `EDGES` de la F334 (5, `cause=EDGES`): con tomas, 1 subida del `Write` y 4 de `RecoverBus` contadas en la captura de T9(b) (separadas por el primer hueco de más de 500 µs), y las 4 iguales a `pulses`; sin tomas, los flancos del `Write` son 5 − 4 = 1, que debe ser ≥ 1 tras `ERR_ARLO` (el arbitraje se pierde con SCL alto). Osciloscopio (taps): 5 subidas antes de soltar SDA y el STOP | T9: la retención después del `Init` da `ERR_BUS` | `ISR`, `IDR`, `pulses` | `HOLD_END` (`edges=5`); AD2 (taps) | las dos |
| T5 | BM | CM4, CM7 | taps | PASS | 20 capturas exactas; f\_SCL en bruto 100,9–101,1 kHz y 403,1–404,1 kHz (+0,8 a +1,1 %), normalizada a ± 5 % del modelo. Cada tiempo contra el mínimo del modo − 2 muestras (§5.5): tHIGH a 400 kHz mínima 600–614 ns (Fm: 600), ninguna por debajo del mínimo en esta captura (0 `RECORD` de déficit). Exención de m25 solo donde el target estira (O16) | Intercambio de canales: distinto en las 20; en T0B(a), 599 ns pasa con nota y 519 ns falla | `t5_timing_problems()` | AD2 | taps |
| S19 del host (m27, m35) | — | host | taps y notaps | PASS | En T0B(a), por el código de la sesión: una línea X a 1 MHz (`s19_x_problem()`, el de `settle`), un punto SCOPE a 1 MHz (`Session.at_action`) y `FMP 1` (`Session.f334_cmd`, que no envía nada) con tomas abortan; lo mismo a 400 kHz o sin tomas, no | — (los casos que siguen son el control) | casos a mano | — | las dos |

El resto de filas da lo mismo que en la corrección (§F1 corrección, 5.3): T1–T4, T6–T8, T10, T12–T15, m18, m20, m22–m24, m26 (taps: 178 líneas X y 30 capturas; notaps: 154 líneas X).

### 5.5 Desviaciones del plan (cambios)

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| T5: tiempos "≥ mínimos del modo" | Cada tiempo ≥ el mínimo − 2 muestras (80 ns a 25 MS/s, 160 ns a 12,5 MS/s); un valor bajo el mínimo pero dentro de la tolerancia es una línea `RECORD` con el déficit, no un fallo | `taps_3d92f3e`: tHIGH mínima de 600 y 599 ns a 400 kHz frente a 600 ns. SCLH da 630 ns nominales, 622 ns con el reloj de la placa (+1,35 %); las medianas, 615–620 ns; la muestra es de 40 ns. Es la tolerancia de medida que el plan ya da a las ventanas | **Hector, 2026-10-08** ("Mínimos con tolerancia") |
| T9(b): presupuesto del `Write` | 2 ms de `max_cycles`; juzgado ≤ 2 ms + 1 ms (corrige la frase de §F1 corrección 5.5, O19) | La captura de T9(b) dura 8 ms y dispara en el `Write` | No consultado |
| T5: exención de m25 | Solo antes del ACK de la dirección y en el bit 0 de un byte que sigue a una dirección reconocida (O16) | Ningún target estira el bajo del maestro antes de STOP/Sr ni tras una dirección rechazada | No consultado; más estricto |

### 5.6 Respuesta a la auditoría de F1, parte 2

| ID | Respuesta |
| --- | --- |
| m33 | Corregido en `6fa2439`, pendiente de verificación: `WaitEnd` con CYCCNT parado al entrar hace el reset por software antes de `ERR_TIMEOUT`. Revisados los demás caminos de error de DMA e IT: `DmaStart*` y `StartIt` rechazan antes de `START` (m31); cada fin de `XferIrq` cierra la transferencia (`it_finish` o `timeout_end`); `ARLO` deja al periférico como esclavo con las líneas sueltas; tras `NACK`, `wait_stop` o el reset. Queda un caso por contrato: `WaitEnd` con `ERR_PARAM` no escribe nada (C6) aunque `DmaStart*` ya haya lanzado la transferencia; el encabezado dice ahora que se termina con `Deinit`. Firmas sin cambios |
| m34 | Corregido en `3d92f3e`, pendiente de verificación: el firmware juzga los niveles tras el `Write` (SCL alto; SDA bajo exige de 1 a 5 pulsos, SDA alto ninguno) y el host, pulsos + flancos del `Write` = `EDGES` de la F334 (con tomas, los flancos contados en la captura; sin tomas, deducidos y ≥ 1 tras `ERR_ARLO`). §5.3 dice qué se juzga |
| m35 | Corregido en `3d92f3e`, pendiente de verificación: `t0b_s19_selftest()`, un caso por aborto y sus controles, por el mismo código que la sesión (§5.3) |
| O16 | Corregido en `3d92f3e`: el decodificador da la trama de cada periodo bajo (`t_low_at`, tercer campo) y la exención se limita a las posiciones donde un target que reconoció su dirección puede estirar |
| O17 | Corregido en `6fa2439`: el encabezado dice que antes del primer `Init` SCL se lee con `scl_read`, porque `RecoverBus` devuelve OK con SDA alto sin leerla |
| O18 | Corregido en `6fa2439` (encabezado) y `3d92f3e` (selftest): 50:2897-2900. La cita de R3-6 en el plan (50:2897-2899) es del plan, que no toco |
| O19 | Corregido en este informe (§5.5): el límite es 2 ms + 1 ms; el código no cambia |
| O20 | Corregido en `3d92f3e`: la inyección de m18 se escribe dentro de `GPIO_BM_SectionEnter`/`Exit`, como `bench_safe()` |
| O21 | Sin cambio (lo pide la auditoría) |

### 5.8 Problemas abiertos y no ejercitado (cambios)

- **`taps_3d92f3e`** (94/96, FW PASS): dos capturas de T5 a 400 kHz con tHIGH mínima de 600 y 599 ns frente a 600; resuelto con la tolerancia de medida que decidió el responsable (§5.5). La sesión notaps de ese commit pasó (74/74). En la entrega (`fefacf4`), la mínima fue 600 ns: el margen de tHIGH a 400 kHz en esta placa es de unos 15–20 ns (2,5–3,5 %), sin contar el ruido de medida. Va al README junto al dato de f\_SCL de M3: un producto con este reloj debe pedir con margen.
- **m33:** el camino nuevo (CYCCNT parado al entrar en `WaitEnd`) no se puede provocar en el banco; está comprobado en el desensamblado (§5.2).
- **m34 sin tomas:** los flancos del `Write` se deducen de la cuenta de la F334, no se miden.
- Lo demás de §F1 corrección 5.8 y §F1 5.8 sigue vigente.

## F1, corrección 3

Responde a la auditoría de F1, parte 3 (`AUDIT.md`, `d88f579`): B1 y O22. Actualiza la "F1, corrección 2" en T10(b); lo demás sigue como allí.

### 5.1 Commits

Rango: `d88f579..` el commit que añade esta sección.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `26c2b4f` | test(prj_01): time T10(b) from the H755's own START (B1, O22) | B1, O22 |
| `5648533` | docs(i2c): add the F1 correction-3 delivery captures at 26c2b4f | — |
| `6360dd5` | docs(i2c): add the F1 correction-3 build evidence at 5648533 | — |
| (este) | docs(i2c): add the F1 correction-3 section of the implementation report | — |

El driver no cambia: `i2c_bm.c`/`.h` son los de `6fa2439`.

### 5.2 Build y tamaños

En `5648533` (código de `26c2b4f`), árbol limpio: `build_all -Clean` 8/8 (`docs/i2c_f1_build_all_5648533.txt`); `selftest-i2c` desde un directorio vacío, 0 avisos; `selftest-i2c`, `debug` y `release` con `-Werror`, 0 avisos; F334 `-Clean` y `-Clean -Werror`, `Warnings : 0`; `.map`: `prj_01` igual a `2481396`, `i2c_bm.c.obj` 5174 B (CM7) y 5156 B (CM4) con `.data = .bss = 0` (`docs/i2c_f1_*_5648533.txt`).

### 5.3 Pruebas

Capturas de entrega, imágenes de `26c2b4f` (Rev e id `5f42d988b8223ddc` en host, CM7 y CM4; F334 id `0ef47b9ceead50de`, el del host; `tree clean`; checklist confirmado por Hector el 2026-10-08: el banco estaba en taps; después "listo" al quitar J2/J3):

- **taps:** `docs/i2c_f1_bench_taps_26c2b4f.txt`: 96/96 (29 `RECORD`: 20 de f\_SCL en bruto, 5 de tHIGH a 400 kHz por debajo de 600 ns dentro de la tolerancia, 592–600 ns, 2 de T10(b), T0d y "No Digital Out"), `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`;
- **notaps:** `docs/i2c_f1_bench_notaps_26c2b4f.txt`: 74/74, `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`.

| ID | Tier | Núcleo | Estado del banco | Resultado | Valor medido | Control negativo: valor | Oráculo | Oráculo F334/AD2 | Captura |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **T10(b)** (B1, O22) | BM | CM4, CM7 | taps y notaps | PASS | SCL retenido 50 ms por la F334, `timeout_scl_low` 489 (TIMEOUTA 10 014 µs nominales). **El firmware** mide la bajada de SCL él mismo mientras espera el intercambio del `AT HOLD` y calcula n = ceil((START − bajada) / TIMEOUTA): CM4 empieza al acabar el intercambio (5926 µs taps, 6353 µs notaps: **n = 1**) y termina a 10 019 µs; CM7 empieza a propósito a 1,5 × TIMEOUTA (15 022 µs: **n = 2**) y termina a 20 031 µs; juzgado en [n × TIMEOUTA − 1 tic, n × TIMEOUTA + 1 ms] y `ERR_BUS`. **El host** (taps): n = ceil(SDAf / TIMEOUTA) desde la bajada de SCL, con SDAf el START del `Write` en el osciloscopio: CM4 SDAf 5,873 ms, n = 1, SDA libre a 9,911 ms (n × TIMEOUTA en tiempo del AD2: 9,908 ms); CM7 SDAf 14,869 ms, n = 2 (el pedido en la línea SCOPE, `nwant=2`), SDA libre a 19,821 ms (19,817 ms) y **el primer vencimiento no terminó nada** (SDA alta y sin flancos hasta el START); tolerancia ± 5 % de TIMEOUTA; n en una línea `RECORD` | Sin `timeout_scl_low`: `ERR_TIMEOUT` a los 30 ms. En T0B(a): un START a 15 ms soltado a 1 × TIMEOUTA, un START a 3 ms cuando se pedía n = 2 y una liberación 1 ms tarde, rechazados | CYCCNT desde la bajada de SCL | AD2 (taps) | las dos |

El resto de filas da lo mismo que en la corrección 2.

**Hecho de silicio (O22, para el README):** con SCL retenida abajo, TIMEOUT (`TIDLE = 0`) vuelve a dispararse cada TIMEOUTA mientras dure. Un vencimiento sin transferencia en curso no hace nada visible en el bus. Una transferencia que empieza después termina en el primer vencimiento posterior a su START, no TIMEOUTA después de él. RM0399 no lo dice (50:2275-2289). Prueba: `i2c_f1_bench_taps_26c2b4f.txt`, T10(b) de CM7 (n = 2), y la ejecución del auditor `i2c_f1_audit_bench_taps_cd8a0f4.txt` (START a 11,2 ms, liberación a 19,8 ms).

### 5.5 Desviaciones del plan (cambios)

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| T10(b): "`ERR_BUS` por `TIMEOUT` … SDA libre al vencer `TIMEOUTA`" (R3-5) | SDA libre en el primer múltiplo de TIMEOUTA después del START del maestro (n), contado desde la bajada de SCL; CM7 empieza a propósito tras el primer vencimiento (n = 2) | B1 y O22: TIMEOUT se repite cada TIMEOUTA con SCL abajo, y el momento del START depende de la latencia del intercambio por USB | Lo pide la auditoría (B1, O22) |

### 5.6 Respuesta a la auditoría de F1, parte 3

| ID | Respuesta |
| --- | --- |
| B1 | Corregido en `26c2b4f`, pendiente de verificación. El host juzga la liberación de SDA en n × TIMEOUTA desde la bajada de SCL, con n = ceil(SDAf / TIMEOUTA) y SDAf el START de la H755 en el osciloscopio, con la tolerancia de antes (5 %), y registra n. Además, el firmware ya no depende de la latencia: mide él mismo la bajada de SCL durante el intercambio (lo comprueba: "timed 1") y juzga el fin en n × TIMEOUTA desde ella, con n calculado de su propio START. Mock en T0B(a) con tres rechazos |
| O22 | En `26c2b4f`: CM7 empieza su `Write` a propósito 1,5 × TIMEOUTA después de la bajada de SCL (`nwant=2` en la línea SCOPE). El host exige n = 2 y que el primer vencimiento no haya terminado nada, y lo registra. Hecho de silicio para el README (§5.3). La memoria del proyecto ya lo recoge |

### 5.8 Problemas abiertos y no ejercitado (cambios)

- T10(b) de CM4 sigue dependiendo de la latencia para su n: ahora no importa, porque se juzga con el n que resulte. Ha sido n = 1 en todas las sesiones mías.
- El caso de un START justo en un múltiplo de TIMEOUTA (a unos µs) no está cubierto: n podría ser ambiguo. CM7 está a 1,5 × para evitarlo; en CM4 no ha ocurrido.
- Lo demás de las secciones anteriores sigue vigente.

## F2

Plan: revisión 3. Contrato de `i2c_bm`: `i2c_bm.h` de `a6de15b` (F1 pasa, `AUDIT.md` "F1, parte 4", `4fee0f9`).

| Fase | Estado | Último commit | Capturas |
| --- | --- | --- | --- |
| F2 | entregada para auditoría (el commit que añade esta sección) | `48ec427` (código); capturas en `f913626` y `ef2395f`; evidencia en `2aa948f` | `docs/i2c_f2_bench_taps_48ec427.txt`, `docs/i2c_f2_bench_notaps_48ec427.txt` |
| F3 | sin empezar | — | — |

Qué hay en F2:

- **`i2c_ll`:** `01_shared_drivers/05_i2c/i2c_ll.{h,c}` y el target `stm32h7_driver_i2c_ll`, que enlaza `stm32h7_driver_i2c_bm`.
- **Siete funciones añadidas a `i2c_bm.h`** (C2, solo añade): `I2C_BM_CheckWrite`, `_CheckRead`, `_CheckWriteRead`, `_CheckDmaStartWrite`, `_CheckDmaStartRead`, `_CheckWaitEnd` y `_CheckStartIt`. Las consulté antes de escribirlas (§5.5).
- **Selftest:** tres pasos nuevos, `F2_NOPIN`, `F2_CM4` y `F2_CM7` (banco v3), después de los de F1. Corren T1, T2, T4, T5 (taps), T6, T7(a)(b), T1 con `BUSY = 1` y T14 por `i2c_ll`. Las pruebas de F1 siguen en sus pasos con `i2c_bm`; no quité ninguna.
- **Host:** los mismos oráculos de F1 para las transferencias `<prueba>-LL`, y el prefijo de captura `i2c_f2_bench_`.

### 5.1 Commits

Rango de la entrega: `4fee0f9..` el commit que añade esta sección, en `driver/i2c`.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `52fa55c` | feat(i2c): add the per-call checks of i2c_bm for i2c_ll (C2 additions) | — (§5.5) |
| `56246ed` | feat(i2c): add i2c_ll, the LL tier of 05_i2c, and its target | — |
| `9667dcb` | test(prj_01): add the F2 steps of the 05_i2c selftest (i2c_ll, bench v3) | — |
| `acca289` | test(prj_01): add the F2 steps to i2c_hw_check.py (bench v3) | — |
| `48ec427` | test(prj_01): name the F2 captures i2c_f2_bench_*; T2-LL's first values | sesión de desarrollo en `acca289` (§5.8) |
| `f913626` | test(prj_01): F2 delivery capture, notaps, at 48ec427 | — |
| `ef2395f` | test(prj_01): F2 delivery capture, taps, at 48ec427 | — |
| `2aa948f` | docs(i2c): add the F2 build evidence at ef2395f | — |
| (este) | docs(i2c): add the F2 section of the implementation report | — |

Notas sobre el orden:

- **Las imágenes de las dos capturas** se compilaron y flashearon en `48ec427`, con el árbol limpio.
- **La captura de taps se tomó desde un checkout separado de `48ec427`** (`git checkout --detach`). Al commitear antes la de notaps (`f913626`), HEAD ya no era el Rev de las imágenes, y T0d exige que lo sea. `f913626` y `ef2395f` solo añaden las dos capturas: `git diff --stat 48ec427 ef2395f` da esos dos archivos de `docs/` y nada más.
- **La evidencia de build y de `.map` es de `ef2395f`**, cuyo código es el de `48ec427`.

### 5.2 Build y tamaños

Todo en `ef2395f`, con el árbol limpio:

- **`build_all -Clean`:** 8/8 (`docs/i2c_f2_build_all_ef2395f.txt`).
- **`selftest-i2c` desde un directorio vacío:** configure 0, build 0 y 0 líneas `warning:` o `error:` (`docs/i2c_f2_selftest_build_ef2395f.txt`).
- **`-Werror`:** `selftest-i2c`, `debug` y `release` con `CMAKE_C_FLAGS=-Werror` en directorios de scratch: configure 0, build 0 y 0 avisos en las tres; el `CMakeCache.txt` de cada una dice `-Werror` (`docs/i2c_f2_werror_ef2395f.txt`).
- **Firmware de la F334:** `build.ps1 -Clean` y `build.ps1 -Clean -Werror` dan `Warnings : 0` y `exit=0` (`docs/i2c_f2_f334_build_ef2395f.txt`).
- **`git diff cc8aa2c -- 02_firmware_projects`:** vacío.

Tamaños del `.map` enlazado (`docs/i2c_f2_map_sizes_ef2395f.txt`, mismo método y agrupación que F1):

| Imagen | .text | .data | .bss | Δ frente a 2481396 | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| prj_01 debug CM7 | 29304 | 132 | 55668 | 0 en cada sección de la referencia | `build/debug/cm7_firmware.map` |
| prj_01 debug CM4 | 3456 | 24 | 12560 | ídem | `build/debug/cm4_firmware.map` |
| prj_01 release CM7 | 28356 | 132 | 55672 | ídem | `build/release/cm7_firmware.map` |
| prj_01 release CM4 | 3112 | 24 | 12560 | ídem | `build/release/cm4_firmware.map` |
| F334 `i2c_target_f334.elf` | 11340 | 20 | 3392 | — (FLASH 11 360 B de 64 KB, RAM 3408 B de 12 KB) | `arm-none-eabi-size` |

Las secciones que el `.map` tiene y la referencia no miden 0 en las cuatro imágenes; el archivo las lista.

| Objeto | Núcleo | .text (+.rodata) | .data | .bss | Fuente (.map) |
| --- | --- | --- | --- | --- | --- |
| `i2c_bm.c.obj` | CM7 | 5316 | 0 | 0 | `build/selftest-i2c/cm7_firmware.map` |
| `i2c_ll.c.obj` | CM7 | 2674 | 0 | 0 | ídem |
| `i2c_bm.c.obj` | CM4 | 5312 | 0 | 0 | `build/selftest-i2c/cm4_firmware.map` |
| `i2c_ll.c.obj` | CM4 | 2650 | 0 | 0 | ídem |
| `i2c_hal.c.obj` | — | no existe en F2 | — | — | — |
| `i2c_selftest.c.obj` (selftest) | CM7 | 38366 | 8 | 919 | `build/selftest-i2c/cm7_firmware.map` |
| `i2c_selftest_common.c.obj` (selftest) | CM7 | 5313 | 0 | 0 | ídem |
| `dma_hal.c.obj` (BDMA de T7(b)) | CM7 | 872 | 0 | 96 | ídem |
| `dma_bm.c.obj` | CM7 | 352 | 0 | 0 | ídem |
| `i2c_selftest.c.obj` (selftest) | CM4 | 6025 | 4 | 65952 | `build/selftest-i2c/cm4_firmware.map` (65 536 son `s_buf`) |
| `i2c_selftest_common.c.obj` (selftest) | CM4 | 5289 | 0 | 0 | ídem |
| `dma_bm.c.obj` | CM4 | 1974 | 0 | 0 | ídem |

- `i2c_bm.c.obj` creció de 5174/5156 (F1, corrección 3) a 5316/5312 (CM7/CM4) por las siete funciones `Check*`.
- La imagen `selftest-i2c` mide 87072/140/56700 en CM7 y 26652/28/78576 en CM4 (agrupación de la tabla de arriba).

### 5.3 Pruebas

Las cifras salen de las dos capturas de entrega:

- **taps:** `docs/i2c_f2_bench_taps_48ec427.txt`: 130/130 comprobaciones del host (45 `RECORD` no contados: T0d, "No Digital Out", 2 de T10(b), 40 de f\_SCL en bruto y 1 de tHIGH dentro de la tolerancia), `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`, sin aborto;
- **notaps:** `docs/i2c_f2_bench_notaps_48ec427.txt`: 98/98 (2 `RECORD`), `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`, sin aborto.

Las dos tienen Rev `48ec427` e id `f5f24d8616e8feb4` en el host, CM7 y CM4, y la F334 con Rev `48ec427`, id `08d9792f8d2607f4` (el que calcula el host) y protocolo v3. Las dos dicen `tree clean`, sin depurador ni WaveForms (línea 11), y llevan el checklist confirmado por Hector el 2026-10-08. Notaps: el banco estaba en notaps con el checklist confirmado al empezar F2 (encargo). Taps: "J2 and J3 are plugged", la respuesta a mi petición de poner J2/J3.

Las filas de F0 y F1 pasan otra vez en las dos sesiones, con los mismos criterios. Aquí van las de F2. Velocidades como en F1: taps 100 y 400 kHz en T4/T5 y 400 kHz en T6/T7; notaps 400 kHz y 1 MHz en T4 y 1 MHz en T6/T7.

| ID | Tier | Núcleo | Estado del banco | Resultado | Valor medido | Control negativo: valor | Oráculo | Oráculo F334/AD2 | Captura |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| T1 | LL | CM7 (I2C2) y CM4 (I2C1) | taps y notaps | PASS | 63 casos por núcleo, 0 con otro estado: los 62 de F1 por `i2c_ll` (los mismos casos de `Common`, con la configuración traducida a constantes LL por `to_ll()`) y uno propio de LL, `analog_filter = 0x1234` → `ERR_PARAM`. Foto de la instancia, `RCC_C1/C2` `APB1LENR`/`APB4ENR`, `D2CCIP2R`, `D3CCIPR` y `SYSCFG_PMCR` sin cambios en los 2 grupos (`0x3`) | Un `I2C_LL_Init` válido cambia la foto (1) | casos escritos a mano (`ist_t1`) | — | las dos |
| T1 con `BUSY = 1` | LL | CM4 (I2C1) y CM7 (I2C4) | taps y notaps | PASS | La F334 retiene SDA (`ISR 0x00008001`): 8 transferencias por `i2c_ll` rechazadas con `ERR_BUS`; registros y RCC sin cambios | — (el propio caso) | `ISR` leído | `HOLD_END` | las dos |
| T2 | LL | CM7 (I2C2, I2C3, I2C4), CM4 (I2C1) | taps y notaps | PASS | 120 configuraciones por instancia (`TIMINGR` × filtro × DNF × timeout). Por caso: `I2C_BM_Init`, imagen, `Deinit`; `I2C_BM_Init` con `DNF + 1`, imagen, `Deinit`; `I2C_LL_Init`, imagen, `Deinit`. Imagen LL (`CR1`, `TIMINGR`, `TIMEOUTR`, `OAR1`, `OAR2`, `CR2`): 0 distintas de los campos del RM escritos a mano y 0 distintas de la de `i2c_bm`, bit a bit; 0 estados de `Init` distintos de los de BM; 0 `Deinit` fuera de la imagen de reset | La misma comparación de paridad contra la imagen de BM con `DNF + 1`: distinta en los 120 casos de cada instancia (todos deben serlo) | imagen de `i2c_bm` de la misma configuración y campos del RM a mano | — | las dos |
| T4 | LL | CM4 (I2C1) y CM7 (I2C4) | taps (100, 400 kHz) y notaps (400 kHz, 1 MHz) | PASS | Por velocidad y núcleo, por `i2c_ll`: 6 `Read`, 6 `WriteRead`, 2 sondeos y 6 `Write` de 1, 2, 255, 256, 1000 y 65 535 bytes: 0 con otro estado o un byte distinto. Host: 20 transferencias `T4-LL` por grupo emparejadas con su `RPT`, 0 con problema. `TIMINGR`: taps `0x30D06C81` y `0x20D0142E`; notaps `0x00D0479C` y `0x00801A33` | F334 con `CORRUPT 5`, lectura de 16 (`T4C-LL`): 1 byte distinto, en el índice 5, en cada velocidad y núcleo | datos regenerados por el host; CRC-32 | `RPT` de la F334 | las dos |
| T5 | LL | CM4 y CM7 | taps | PASS | 5 transferencias por velocidad y núcleo, decodificadas exactas (`S 85A A5A A4N P`, `S 84A 07A \| Sr 85A A2N P`, `S 84A P \| S 84A 00A P`, `S 84A 14A 5AA P`, `S 86N P`), 0 glitches. 100 kHz: tLOW mín. 5298–5306 ns, tHIGH 4262–4312 ns, tSU;DAT de la H755 ≥ 4809 ns, tHD;STA ≥ 4378 ns, tSU;STO ≥ 4379 ns, t\_r 248–251 ns, f\_SCL normalizada 99 656–99 770 Hz (modelo 99 700). 400 kHz: tLOW mín. 1573–1578 ns, tHIGH 598–634 ns, tSU;DAT de la H755 ≥ 1066 ns, tHD;STA ≥ 718 ns, tSU;STO ≥ 692 ns, t\_r 250–254 ns, f\_SCL normalizada 397 983–398 860 Hz (modelo 398 406). tHIGH de 598 ns (CM7 k=3, 400 kHz) dentro de la tolerancia de 2 muestras (`RECORD`, §5.8). El mínimo de tHIGH de BM en la misma sesión: §5.8 | Cada captura decodificada con SCL y SDA intercambiados: distinta en las 20; con umbral 0,5 V: igual en las 20 | `expected_frames()`, `t5_timing_problems()` | AD2 (12,5 MS/s a 100 kHz, 25 MS/s a 400 kHz) | taps |
| T6 | LL | CM4 y CM7 | taps (400 kHz) y notaps (1 MHz) | PASS | `Write`, `Read`, `WriteRead` de 1, 2, 256 y 1000 bytes por `I2C_LL_StartIt` + `I2C_LL_XferIrq` desde los dos vectores: 0 con otro estado o dato; IE en `CR1` al terminar `0x00`; 0 IRQ en los 10 ms siguientes | IE a 0 a mano tras `I2C_LL_StartIt`: sigue `PENDING`, la espera acaba por su presupuesto (`ERR_TIMEOUT`), 0 IRQ (`T6C-LL`) | datos del host + CRC | `RPT` (la F334 corta la transferencia abandonada) | las dos |
| T7(a) | LL | CM4 (I2C1, DMA2 con `dma_bm`) | taps (400 kHz) y notaps (1 MHz) | PASS | Lectura y escritura de 2, 255, 256 y 4096 bytes por `I2C_LL_DmaStart*` + `I2C_LL_WaitEnd` + `I2C_LL_DmaEnd`: 0 con otro estado o dato | `RXDMAEN` a 0 a mano, lectura de 64: `I2C_LL_WaitEnd` `ERR_TIMEOUT`, los 64 bytes conservan el centinela (`T7C-LL`) | ídem | `RPT` | las dos |
| T7(b) | LL | CM7 (I2C4, BDMA con `dma_hal`, `.d3_ram`) | ídem | PASS | Igual que T7(a) | Igual | ídem | `RPT` | las dos |
| T14 | LL | CM4 (I2C1), CM7 (I2C4) | taps y notaps | PASS | `I2C_BM_Deinit` tras cada prueba con `i2c_ll`: tras T4/T5 en cada velocidad, tras el control de T6 y al final de T1-BUSY/T6/T7: 8 juicios por sesión, 0 registros distintos de la imagen de reset en todos. En T2, tras cada `I2C_LL_Init`: 0 de 480 fuera del reset. Bus en reposo: S17 tras cada paso | El de F1 en la misma sesión (I2C3 inicializado y sin `Deinit`: distinto, `0x11`), que no depende del tier | tabla de reset del RM a mano. **Corregido (auditoría de F2, m38):** en `48ec427` los juicios de T14 tras cada prueba usaban una copia local de esa tabla en `inst_deinit`, no `ist_reset_diff()` como decía aquí (las dos tablas eran iguales); desde `4d6b092`, `inst_deinit` usa `ist_reset_diff_img()`, la tabla de `Common` | — | las dos |
| Fm+ con LL | BM (`SetFastModePlus`) | CM4 y CM7 | notaps (el juicio final, en las dos) | PASS | Notaps: bit de I2C1 e I2C4 puesto alrededor del tráfico LL a 1 MHz y quitado después (`SYSCFG_PMCR 0x03000001` / `0x03000008` → `0x03000000`), `FMP 1` en la F334. Al final de `F2_CM4` y `F2_CM7`, en las dos sesiones: `I2C1FMP\|I2C4FMP` 0x0 | — | `SYSCFG_PMCR` leído | — | las dos |
| F334 al final | — | host | taps y notaps | PASS | `scl=1 sda=1`, S15 ok, `uart_ovr 0`. `ev` 24 (taps) y 16 (notaps): sondeos y los cortes de los controles de T6 y T7 de BM y LL; `nk 4`; ningún `BERR`, `ARLO` ni `OVR` | — | — | `STATUS` | las dos |
| Veredicto | — | CM7 + host | taps y notaps | PASS | `bench completed without abort`; CM4 `timeout 0, aborts 0`; pila de `Status_Task` 694 (taps) y 706 (notaps) palabras libres de 1024; host 130/130 y 98/98 | — | — | — | las dos |

**El oráculo de las pruebas LL.** La configuración LL sale de `to_ll()` (`Common/Src/i2c_selftest_common.c`), que escribe a mano `LL_I2C_ANALOGFILTER_ENABLE`/`_DISABLE` (línea 285). No usa la traducción de `i2c_ll.c`, que es justo lo que prueban T1 y T2. Las transferencias LL van por los mismos `ist_write/read/…(ll, …)` de `Common/Inc/i2c_selftest.h`, con `ll = true`. El host las ve como `X <prueba>-LL` y las juzga con el oráculo de la prueba de F1 (`tier_base()`).

**O23 (latencia del `AT HOLD` en T10(b)).** El `Write` de CM4 empezó a 6117 µs (taps) y 6189 µs (notaps) después de la bajada de SCL; el osciloscopio da el START a 6,053 ms (taps). CM7 empezó a propósito a 15 022 µs (n = 2) en las dos, y la liberación fue a 19,791 ms (n × TIMEOUTA 19,778 ms). Lejos de 2 × TIMEOUTA (19,8 ms): no cambié el juicio de CM7.

### 5.4 Medidas (B1)

B1 no es una prueba de F2 en el plan (fila F2 de Fases: T1, T2, T4, T5, T6, T7 y T14) y no lo medí con `i2c_ll`. Las líneas B1 de las capturas son de `i2c_bm`, en sus pasos de F1:

| Medida | CM4 (200 MHz) | CM7 (400 MHz) | Fuente |
| --- | --- | --- | --- |
| `I2C_BM_Init` | 330 (taps) / 330 (notaps) ciclos | 674 / 618 | las dos capturas |
| `I2C_BM_Write` de 1 byte | 11 276 (400 kHz) / 5797 (1 MHz) | 22 518 (400 kHz) / 11 344 (1 MHz) | ídem |
| `I2C_BM_XferIrq` por llamada (escritura IT de 16) | 147–267 (las dos) | 125–332 (las dos) | ídem |
| `i2c_ll.c.obj` | 2650 / 0 / 0 | 2674 / 0 / 0 | §5.2 |
| `Status_Task` | — | 694 y 706 palabras libres | las dos capturas |

### 5.5 Desviaciones del plan

**Las firmas de `i2c_ll`** (`i2c_ll.h`), con su razón. Todas son reflejo de `i2c_bm`: mismos parámetros, mismos códigos (`I2cBm_Status_t`) y mismo significado documentado.

| Firma | Reflejo de | Razón |
| --- | --- | --- |
| `I2cLl_Config_t { I2C_TypeDef *i2c; uint32_t timing; uint32_t analog_filter; uint32_t digital_filter; uint32_t timeout_scl_low; }` | `I2cBm_Config_t { i2c, timingr, analog_filter (bool), dnf, timeout_scl_low }` | `analog_filter` toma `LL_I2C_ANALOGFILTER_ENABLE`/`_DISABLE`, la constante LL que el plan nombra. `timing` es el `Timing` de `LL_I2C_SetTiming`. `digital_filter` es el número 0..15 de `LL_I2C_SetDigitalFilter`, que no tiene constantes. `timeout_scl_low` es el de BM: LL no tiene constante para el número, y TIDLE queda fijo en `LL_I2C_SMBUS_TIMEOUTA_MODE_SCL_LOW`, como en BM |
| `I2cBm_Status_t I2C_LL_CheckConfig(const I2cLl_Config_t *cfg)` | `I2C_BM_CheckConfig` | Traduce y llama a `I2C_BM_CheckConfig`. Una constante analógica desconocida pasa a DNF 16, que BM rechaza con `ERR_PARAM` en su comprobación de campos (el orden de C6 es el de BM) |
| `I2cBm_Status_t I2C_LL_Init(const I2cLl_Config_t *cfg)` | `I2C_BM_Init` | Valida con `I2C_LL_CheckConfig` |
| `I2C_LL_Write(i2c, addr7, tx, n, max_cycles)`, `_Read(i2c, addr7, rx, n, max_cycles)`, `_WriteRead(i2c, addr7, tx, ntx, rx, nrx, max_cycles)` | `I2C_BM_Write/Read/WriteRead` | Validan con `I2C_BM_CheckWrite/Read/WriteRead` |
| `I2cBm_Status_t I2C_LL_StartIt(I2C_TypeDef *i2c, I2cBm_Xfer_t *x)` y `bool I2C_LL_XferIrq(I2C_TypeDef *i2c, I2cBm_Xfer_t *x)` | `I2C_BM_StartIt`, `I2C_BM_XferIrq` | El estado de la interrupción es el `I2cBm_Xfer_t` del llamador, que no lleva constantes LL (C7). `StartIt` valida con `I2C_BM_CheckStartIt` |
| `I2C_LL_DmaStartWrite(i2c, addr7, buf, n)`, `_DmaStartRead(i2c, addr7, n)`, `_WaitEnd(i2c, n, max_cycles)`, `_DmaEnd(i2c)` | `I2C_BM_DmaStart*`, `_WaitEnd`, `_DmaEnd` | `WaitEnd` lleva `n` como el de BM (desviación aprobada en F1). Validan con `I2C_BM_CheckDmaStartWrite/Read` y `I2C_BM_CheckWaitEnd`; `DmaEnd` con `I2C_BM_IsValidInstance` |
| (sin envoltorio LL) reloj, `Deinit`, `TakeErrors`, `GetIRQn`, `GetDmaRequest`, `GetCaps`, `GetKernelSource`, `CalcTiming`, `SetFastModePlus`, `RecoverBus`, `CheckTransfer` y los `Check*` | — | El plan dice que las de su lista "se usan de `i2c_bm`". Añadí a esa lista `GetCaps`, `GetKernelSource` y las comprobaciones, porque tampoco toman constantes LL. Lo dice `i2c_ll.h` |

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| Contrato de `i2c_ll`: "validadas con `I2C_BM_Check*`"; la API de `i2c_bm` congelada en `a6de15b` (C2: solo añade) | Siete funciones añadidas a `i2c_bm.h` (`52fa55c`): `I2C_BM_CheckWrite`, `_CheckRead`, `_CheckWriteRead`, `_CheckDmaStartWrite`, `_CheckDmaStartRead`, `_CheckWaitEnd` y `_CheckStartIt`. Son las comprobaciones que ya hacían esas llamadas de BM, que ahora las llaman (mismos códigos, mismo orden). `git diff a6de15b 48ec427 -- 01_shared_drivers/05_i2c/i2c_bm.h` no tiene ninguna línea quitada | `I2C_BM_CheckTransfer` no cubre los argumentos propios de cada llamada (buffer `NULL`, `max_cycles` 0, `n` < 2 en `DmaStartWrite`…). Sin estas, esas reglas tendrían una segunda copia en `i2c_ll` | Hector, 2026-10-08, antes de escribirlas: "Añadir Check* a i2c_bm (Recommended)" |
| Contrato: "constantes … donde aplican (`LL_I2C_ANALOGFILTER_*`, `LL_I2C_ADDRSLAVE_7BIT`, …)" | `LL_I2C_ADDRSLAVE_7BIT` se usa dentro (`LL_I2C_HandleTransfer` y el `CR2` de `StartIt`), no como parámetro: las transferencias toman `addr7`, como en BM | El contrato es solo de 7 bits; un parámetro de modo que solo admite un valor no sería un reflejo de BM | No consultado |
| "Escritura con inlines LL y `LL_I2C_WriteReg`" | `CR1` en `Init`, `OAR1`, `OAR2`, `CR2 = 0` e `ICR` con `LL_I2C_WriteReg`. `ICR` siempre con una sola `LL_I2C_WriteReg`, nunca con `LL_I2C_ClearFlag_*`. El `CR2` de `StartIt` con `LL_I2C_WriteReg` y después `LL_I2C_GenerateStartCondition` | Ninguna inline escribe `CR1`, `OAR1`/`OAR2` o `CR2` enteros. Las `LL_I2C_ClearFlag_*` (salvo `_TXE`) son `SET_BIT` sobre `ICR`, un RMW de un registro de solo escritura. `LL_I2C_HandleTransfer` con `LL_I2C_GENERATE_NOSTARTSTOP` deja `RD_WRN` fuera de su máscara (línea 2144): conservaría el sentido de la transferencia anterior | No consultado |
| Ídem, `TIMEOUTR` | `LL_I2C_WriteReg(TIMEOUTR, 0)`, `LL_I2C_SetSMBusTimeoutA(TIMEOUTA \| LL_I2C_SMBUS_TIMEOUTA_MODE_SCL_LOW)` y `LL_I2C_EnableSMBusTimeout(LL_I2C_SMBUS_TIMEOUTA)` (bajo PRIMASK), las tres escrituras de BM | TIMEOUTA solo se escribe con `TIMOUTEN = 0` (50:2861). `LL_I2C_SetSMBusTimeoutA` es un `WRITE_REG` del registro entero (1140); `LL_I2C_SetSMBusTimeoutAMode` también (1170) y borraría TIMEOUTA, y `LL_I2C_ConfigSMBusTimeout` es un `MODIFY_REG` | No consultado |
| T14 en F2: "`Deinit` tras cada prueba … Instancia sin `Deinit`: detectada" | El control es el de F1 (I2C3 sin `Deinit`, en `F1_NOPIN`, la misma sesión), no uno con LL | El control mide `ist_reset_diff()`, que no depende del tier; el `Deinit` es de BM en los dos. (En `48ec427` los juicios de T14 no usaban esa función sino una copia de su tabla: m38, corregido en `4d6b092`) | No consultado |
| T8–T10, T12, T13, T15 y B1 | Siguen con `i2c_bm`, en sus pasos de F1 | La fila F2 de Fases pide T1, T2, T4, T5, T6, T7 y T14 con LL | — |

### 5.6 Respuesta a la auditoría

La de F1 está en sus secciones (F1 pasa, `4fee0f9`). De F2 todavía no hay auditoría.

### 5.7 Erratas del plan encontradas

Ninguna en F2.

### 5.8 Problemas abiertos y no ejercitado

**Lo que pasó en desarrollo:**

1. **La primera sesión de F2** (notaps, `acca289`, árbol limpio: 98/98, `FW VERDICT=PASS`, `HOST VERDICT=PASS`) salió con el nombre `docs/i2c_f1_bench_notaps_acca289.txt`, porque el host aún escribía el prefijo de F1. Además, la línea de T2-LL imprimía sin inicializar los valores LL/BM "del primer caso distinto" cuando no había ninguno (p. ej. `LL 0x08007843 BM 0x0801163C` con 0 distintas). El veredicto no los usa. Corregido en `48ec427`, que commitea esa captura sin editar como desarrollo.

**Abierto:**

- **Los nombres del host en F2 conservan los de F1.** Las comprobaciones de las transferencias LL se llaman `F1 T4-LL …`, `F1 T5-LL scope …`. Las líneas `RECORD` de T5-LL dicen `T5 CM7 k=…`, sin `-LL`: los 20 f\_SCL en bruto de T5-LL y el tHIGH de 598 ns (`taps`, línea 2379) son de T5-LL. Se distinguen por su paso y su orden en la captura (justo después de su `AT SCOPE T5-LL`), no por el nombre. No lo corregí: cambiar el host habría pedido otro par de sesiones.
- **tHIGH a 400 kHz:** el mínimo de T5-LL fue 598 ns (CM7 k=3), dentro de la tolerancia de 2 muestras aprobada en F1; el de T5 con BM en la misma sesión, 604 ns. El `TIMINGR` es el mismo (`0x20D0142E`). Es el margen de ~3 % de esta placa (README, R3-7).
- **La pila de `Status_Task`** baja de 710/712 (F1) a 694/706 palabras libres de 1024.
- Lo abierto de F1 sigue vigente.

**No ejercitado en F2** (el README del cierre debe copiarlo entero):

- **`ERR_VERIFY` de `i2c_ll`:** el read-back de `Init` y el `PE = 0` que no baja no fallan nunca en el banco; sin prueba.
- **`I2C_LL_Init` con `PE = 1` y la misma configuración en vigor** (un reset por software, `i2c_bm.h`): no se probó con LL. Con `PE = 1` y otro `TIMINGR`, DNF, filtro o timeout, T1 lo rechaza con `ERR_BUSY` también por LL (grupo 2 de `ist_t1`, después de un `I2C_LL_Init` válido).
- **TIMEOUT, NACK por dato, stretching, `RecoverBus` y la errata 2.19.6 con `i2c_ll`:** T8, T9, T10 y T12 solo con `i2c_bm`. El camino de TIMEOUT de `I2C_LL_XferIrq` y de las esperas LL no se ejecutó; el NACK de dirección sí (sondeo de 0x43 en T4).
- **`BERR` y `ARLO` en `I2C_LL_XferIrq`:** como en F1, sin forma de provocarlos.
- **I2C2 e I2C3 con LL:** solo T1 (I2C2) y T2; sin transferencias.
- **T5 con LL a 1 MHz:** sin osciloscopio a 1 MHz (D3), como en F1.
- **B1 con LL:** no medido (§5.4).
- **`CYCCNT` parado durante una espera LL:** la salida existe en `clk_expired()`; la prueba no para `CYCCNT`.
- **Lo que sigue de F1:** T11 y multimaestro; esclavo, 10 bits, SMBus/PEC; silicio distinto de rev V.

### Constantes y funciones de `stm32h7xx_ll_i2c.h` que usa `i2c_ll`

Cabecera del Cube con el que compila `prj_01` (`CUBE_DIR` → `02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7`, `STM32H7xx_HAL_Driver` en `e3c518e`; SHA-256 `30c531ae…75d3d`, 2279 líneas). Es idéntica, byte a byte, a la copia de `00_docs_stm32h7`. Las líneas son las del `#define` o de la cabecera de la función. Uso: líneas de `i2c_ll.c` en código, sin comentarios.

| Constante o función | Línea | Qué hace en la cabecera | Uso en `i2c_ll.c` |
| --- | --- | --- | --- |
| `LL_I2C_CR1_TXIE` … `LL_I2C_CR1_ERRIE` (TX, RX, ADDR, NACK, STOP, TC, ERR) | 166–172 | bits de `CR1` | `IE_ALL` (25-26); los IE de `StartIt` (463, 471, 477) y del `NACKF` (539) |
| `LL_I2C_ANALOGFILTER_ENABLE` / `_DISABLE` | 192 / 193 | `0` / `I2C_CR1_ANFOFF` | `analog_known()`, `to_bm()` (55, 62) |
| `LL_I2C_ADDRSLAVE_7BIT` | 244 | `0` | `start()` (229) y el `CR2` de `StartIt` (469, 475, 481) |
| `LL_I2C_REQUEST_READ` | 254 | `I2C_CR2_RD_WRN` | sentido de la transferencia en `XferIrq` (514) |
| `LL_I2C_MODE_RELOAD` / `_AUTOEND` / `_SOFTEND` | 262 / 263 / 265 | `RELOAD` / `AUTOEND` / `0` | `chunk_mode()` (218, 220); el sondeo de `StartIt` (481) |
| `LL_I2C_GENERATE_START_READ` / `_START_WRITE` | 288 / 290 | `0x80000000 \| START \| RD_WRN` / `0x80000000 \| START` | `start()` (230) |
| `LL_I2C_SMBUS_TIMEOUTA_MODE_SCL_LOW` | 330 | `0` (TIDLE = 0) | `Init` (133) |
| `LL_I2C_SMBUS_TIMEOUTA` | 341 | `I2C_TIMEOUTR_TIMOUTEN` | `Init` (134, 151) |
| `LL_I2C_WriteReg` / `LL_I2C_ReadReg` | 371 / 379 | `WRITE_REG` / `READ_REG` | `CR1`, `TIMEOUTR = 0`, `OAR1`, `OAR2`, `CR2`, `ICR`; read-back y la foto de `ISR` de cada pasada (21 y 11 usos) |
| `LL_I2C_Enable` / `_Disable` / `_IsEnabled` | 429 / 443 / 454 | `SET_BIT`/`CLEAR_BIT`/`READ_BIT` de `CR1.PE` | `pe_on()` (107), `pe_off()` (88, 93), bajo PRIMASK |
| `LL_I2C_EnableDMAReq_TX` / `_DisableDMAReq_TX` / `_IsEnabledDMAReq_TX` | 549 / 560 / 571 | `TXDMAEN` | `DmaStartWrite` (609, 611, 614), `DmaEnd` (709, 713) |
| `LL_I2C_EnableDMAReq_RX` / `_DisableDMAReq_RX` / `_IsEnabledDMAReq_RX` | 582 / 593 / 604 | `RXDMAEN` | `DmaStartRead` (632, 634, 637), `DmaEnd` (710, 713) |
| `LL_I2C_SetTiming` | 922 | `WRITE_REG(TIMINGR)` | `Init` (141) |
| `LL_I2C_SetSMBusTimeoutA` | 1140 | `WRITE_REG(TIMEOUTR)` | `Init` (147) |
| `LL_I2C_EnableSMBusTimeout` | 1231 | `SET_BIT(TIMEOUTR)` | `Init` (151), bajo PRIMASK |
| `LL_I2C_IsEnabledIT_NACK` | 1408 | `READ_BIT(CR1.NACKIE)` | `XferIrq`: `NACKF` aún sin servir (532) |
| `LL_I2C_IsActiveFlag_RXNE` / `_NACK` / `_STOP` / `_BUSY` | 1578 / 1604 / 1617 / 1741 | `READ_BIT(ISR)` | 393, 582 / 586 / 282 / 168 |
| `LL_I2C_ClearFlag_TXE` | 1786 | `WRITE_REG(ISR, TXE)` (no es un RMW: `ISR` no es de solo escritura) | vaciar `TXDR` (268, 305, 462, 540, 606, 616) |
| `LL_I2C_GenerateStartCondition` / `_GenerateStopCondition` | 1990 / 2001 | `SET_BIT(CR2.START)` / `SET_BIT(CR2.STOP)` | `StartIt` (489) / `stop()` (257), bajo PRIMASK |
| `LL_I2C_GetTransferRequest` | 2066 | `READ_BIT(CR2.RD_WRN)` | `XferIrq` (514) |
| `LL_I2C_HandleTransfer` | 2133 | `MODIFY_REG(CR2, …)` | `start()` (229), bajo PRIMASK |
| `LL_I2C_ReceiveData8` / `LL_I2C_TransmitData8` | 2225 / 2237 | `RXDR` / `TXDR` | 354, 544 / 318, 364, 467, 554, 607 |

En el selftest se usan además `LL_I2C_ANALOGFILTER_ENABLE`/`_DISABLE` en `to_ll()` (`Common/Src/i2c_selftest_common.c:285`).

Sin usar a propósito:

- **`LL_I2C_ClearFlag_ADDR`, `_NACK`, `_STOP`, `_BERR`, `_ARLO`, `_OVR`** (1752–1819) y las demás de `ICR`: `SET_BIT` sobre `ICR`, de solo escritura (§5.5).
- **`LL_I2C_HandleTransfer` con `LL_I2C_GENERATE_NOSTARTSTOP`** (284): no actualiza `RD_WRN` (§5.5).
- **`LL_I2C_SetSMBusTimeoutAMode`** (1170, `WRITE_REG` de `TIMEOUTR` entero) y **`LL_I2C_ConfigSMBusTimeout`** (1123, `MODIFY_REG` con `TIMEOUTB`).
- **`LL_I2C_EnableAnalogFilter`/`_DisableAnalogFilter`/`_SetDigitalFilter`** (515 / 527 / 492) y los setters de campo de `CR2` (`LL_I2C_SetSlaveAddr` 2079, `_SetTransferSize` 1950, `_EnableReloadMode` 1914, `_EnableAutoEndMode` 1879, `_SetMasterAddressingMode` 790): escribirían `CR1` o `CR2` por partes, con estados intermedios y un RMW por campo.
- **Los `LL_I2C_EnableIT_*`/`_DisableIT_*`** (desde 1287): `ie_set()` cambia todos los IE en un solo RMW de `CR1` bajo PRIMASK.
- **`LL_I2C_Init`** (2251): solo existe con `USE_FULL_LL_DRIVER` (C8).

## Respuesta a la auditoría de F2, parte 1 (`12d2ce7`; plan revisión 4, `ceed2fc`)

Responde a B2, B3 y m36–m39. Esta sección sustituye a la de F2 en las pruebas de F0/F1 que cambian (T0C, T1 con `BUSY`, T9, T9(b), T10(b)) y en lo abierto; lo demás de F2 sigue como allí. `i2c_ll.c` y las funciones de `i2c_bm` no cambian.

### 5.1 Commits

Rango: `12d2ce7..` el commit que añade esta sección, en `driver/i2c`.

| Commit | Asunto | Hallazgo |
| --- | --- | --- |
| `4d6b092` | test(prj_01): order every F334 hold after its handshake; T9 by the release (bench v4) | B2, B3, m38, m39 |
| `0471570` | docs(i2c): complete the i2c_ll header and DmaStartWrite's codes (m36, m37) | m36, m37 |
| `3304bb9` | docs(i2c): add the F2 correction delivery captures at 0471570 | — |
| `60ac356` | docs(i2c): add the F2 correction build evidence at 3304bb9 | — |
| (este) | docs(i2c): answer the F2 audit, part 1, in the implementation report | m38 (el informe) |

- **Las imágenes de las dos capturas** son de `0471570`, compiladas desde un `--fresh` con el árbol limpio y flasheadas con `tools/flash.py --sn` (H755 `0016000C4142501220353451`, F334 `066BFF393731435243254117`; "Download verified successfully" en las dos).
- **Las dos sesiones corrieron en el mismo HEAD:** la captura de taps quedó sin commitear (`-uno`: no cuenta como cambio) mientras corría la de notaps. Las dos se commitearon juntas en `3304bb9`, que solo añade esos dos archivos.
- **La evidencia de build es de `3304bb9`**, cuyo código es el de `0471570`.
- **Banco v4:** cambia el protocolo de los puntos `AT HOLD` (la retención después del handshake) y del `SCOPE` (`trig=sdar`). `I2C_BENCH_VERSION` y el `BENCH_VERSION` del host pasan a 4, así que una imagen de v3 aborta en S7.

### 5.2 Build y tamaños

Todo en `3304bb9`, árbol limpio:

- `build_all -Clean` 8/8 (`docs/i2c_f2_build_all_3304bb9.txt`).
- `selftest-i2c` desde un directorio vacío: configure 0, build 0, 0 líneas `warning:` o `error:` (`docs/i2c_f2_selftest_build_3304bb9.txt`).
- `selftest-i2c`, `debug` y `release` con `CMAKE_C_FLAGS=-Werror` en directorios de scratch: 0 avisos en las tres (`docs/i2c_f2_werror_3304bb9.txt`).
- F334 `-Clean` y `-Clean -Werror`: `Warnings : 0`, `exit=0`; 11340/20/3392 (`docs/i2c_f2_f334_build_3304bb9.txt`).
- `.map` (`docs/i2c_f2_map_sizes_3304bb9.txt`): `prj_01` igual a `2481396` en cada sección de la referencia; `i2c_bm.c.obj` 5316 (CM7) y 5312 (CM4), `i2c_ll.c.obj` 2674 y 2650, todos con `.data = .bss = 0` (los mismos tamaños que en `ef2395f`). La imagen `selftest-i2c` mide 87128/140/56692 (CM7) y 26660/28/78576 (CM4).
- `git diff cc8aa2c -- 02_firmware_projects`: vacío.

### 5.3 Pruebas

Capturas de entrega, imágenes de `0471570`, Rev e id `0b1e773f8e5b83f9` en host, CM7 y CM4; F334 Rev `0471570`, id `469d9f476aa7c806` (el del host), protocolo v3; `tree clean`; sin depurador ni WaveForms:

- **taps:** `docs/i2c_f2_bench_taps_0471570.txt`: 130/130 (49 `RECORD`), `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`. Banco en taps con el checklist confirmado (encargo de la corrección).
- **notaps:** `docs/i2c_f2_bench_notaps_0471570.txt`: 98/98 (2 `RECORD`), `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`. "All set, J2 and J3 are removed", la respuesta del responsable a mi petición de quitarlos.

No hubo sesiones fallidas: no hay más capturas que commitear. Los fallos del auditor (B2, B3) no se reprodujeron en mis sesiones de F2, ni antes ni después de la corrección. La corrección quita la dependencia por construcción; las cifras de abajo muestran dónde estaba la latencia.

| ID | Tier | Núcleo | Estado del banco | Resultado | Valor medido | Control negativo: valor | Oráculo | Oráculo F334/AD2 | Captura |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| T0C (B2) | — | CM7 | taps y notaps | PASS | CM7 vigila antes de que el host ordene la retención y cierra 50 ms después de que la línea retenida suba. Las retenciones empezaron 63,4/61,8 ms (taps, SDA/SCL) y 67,8/64,0 ms (notaps) después de abrir la vigilancia: es la latencia del host, ahora solo registrada (`first_us`). Retención vista por CM7: 50 748/50 743 µs (taps), 50 769/50 738 µs (notaps); la otra línea, 0 bajadas; con taps, PF14/PF15 igual que PB8/PB9 | — (los límites 45–55 ms y "la otra línea nunca baja", como en F0) | CYCCNT | `HOLD_END` 50 003/50 001 µs; osciloscopio 50,08 ms (taps) | las dos |
| T9 (B3) | BM | CM7 | taps | PASS | Disparo en la liberación de SDA (`trig=sdar`, 3/4 del buffer antes, 2 MS/s): 5 pulsos de SCL, SDA libre tras el 5.º, STOP visto; primer flanco de SCL 3,023 ms y liberación 3,073 ms dentro de la captura | En T0B(a): sin STOP, SDA que sube y baja antes de la liberación, y 4 pulsos en T9: los tres rechazados | `scope_t9()` | AD2; `HOLD_END cause=EDGES` | taps |
| T9(b) (B3) | BM | CM7 | taps y notaps | PASS | Firmware: del inicio del `Write` al fin de `RecoverBus` 1083 µs (taps) y 1077 µs (notaps), CYCCNT, impreso después de la recuperación. Osciloscopio (taps): 1 subida del `Write` y 4 de `RecoverBus` (1 + 4 = los 5 de la F334), STOP visto; primer flanco de SCL 5,097 ms y liberación 6,145 ms dentro de la captura (1 MS/s, 6,14 ms antes del disparo), así que quedan ≥ 0,5 ms a cada lado | En T0B(a): T9(b) sin STOP rechazado | `scope_t9()`, `t9b_counts()` | AD2; `HOLD_END` | las dos |
| T9, control | BM | CM7 | taps y notaps | PASS | Sin `RecoverBus`, cronometrado desde la bajada que CM7 espera: SDA abajo 101 ms en las dos (antes 95–96 ms: le faltaba lo que tardaba el handshake) | — | CYCCNT | `HOLD_END` | las dos |
| T10(b) | BM | CM4, CM7 | taps y notaps | PASS | CM7 espera la bajada de SCL (≤ 2 s) y cronometra desde ella. CM4 empieza al verla: START 0,010 ms después de la bajada (osciloscopio, taps; firmware: 0 µs), **n = 1, ahora pedido** (`nwant=1`); fin a 10 019 µs. CM7 empieza a 1,5 × TIMEOUTA (15 022 µs): n = 2; SDA libre a 19,774 ms (n × TIMEOUTA 19,786 ms) | Sin `timeout_scl_low`: `ERR_TIMEOUT` 30 ms después de la bajada, en las dos | CYCCNT | AD2 (taps); `HOLD_END` | las dos |
| T1 con `BUSY = 1` | BM y LL | CM4, CM7 | taps y notaps | PASS | La retención empieza después del handshake; CM7 espera la bajada (≤ 2 s) y solo entonces llama o manda a CPU2: 8 rechazos `ERR_BUS` en cada núcleo y tier | — | `ISR` | `HOLD_END` | las dos |
| Resto de F0, F1 y F2 | BM y LL | — | taps y notaps | PASS | Como en la sección F2; pila de `Status_Task` 696 (taps) y 716 (notaps) palabras libres | — | — | — | las dos |

### 5.5 Desviaciones del plan (cambios)

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
| --- | --- | --- | --- |
| T10(b): CM4 empieza cuando acaba el intercambio del `AT HOLD` (n el que dé la latencia) | CM4 empieza en cuanto CM7 ve la bajada de SCL: n = 1, pedido y juzgado (`nwant=1`) | Con la retención ordenada después del handshake, CPU1 ya está esperando la bajada; el n de CM4 deja de depender del USB (B2, "cualquier otra ventana fija") | Lo pide la auditoría (B2) |
| Banco: `AT HOLD`, la retención antes del handshake | Después del handshake, en todos los puntos `AT HOLD` (banco v4) | B2 | Lo pide la auditoría (B2) |

### 5.6 Respuesta a la auditoría de F2, parte 1

| ID | Respuesta |
| --- | --- |
| B2 | Corregido en `4d6b092`, pendiente de verificación. **T0C:** `watch_bus()` se abre al bajar `MCU_ACK`, antes de que el host ordene la retención, espera la bajada de la línea retenida hasta 2 s (`HOLD_WAIT_MS`) y se cierra 50 ms después de que suba (`T0C_TAIL_MS`), o 500 ms después de la bajada si no sube (`T0C_LOW_MAX_MS`, S21). Ya no hay ventana fija de 600 ms. **Las demás ventanas abiertas en un handshake**, revisadas con el mismo criterio: en todos los `AT HOLD` el host ordenaba la retención antes de soltar a CM7, y la retención (50–300 ms) corría mientras el host terminaba el handshake. Ahora el host la ordena después (`at_action()`: acción posterior) y CM7 espera la bajada hasta 2 s y cronometra desde ella: T1 con `BUSY` (BM y LL), T9 y su control, T9(b), T10(b) y su control. La vigilancia de SCL durante el handshake de T10(b) (B1 de F1) ya no hace falta y se quitó. **Sin cambio**, porque no dependen de la latencia: las capturas de T5, T10(a) y T10(b), que disparan en el propio bus; CLKREF, que dispara en el pulso de `MCU_ACK`; los armados de la F334 (`NACK`, `CORRUPT`, `STRETCH`), que esperan a la transferencia sin plazo; y las esperas de 600 ms de los controles de T6/T7/T10(b), que cuentan desde una acción de CM7, no desde un handshake |
| B3 | Corregido en `4d6b092`, pendiente de verificación. La captura de la F334 del auditor (`HOLD_END … us=6678 edge1_us=5632`) muestra que el primer flanco de SCL llegó 5,6 ms después del inicio de la retención: la ventana de 8 ms contaba desde antes del `Write` y se gastaba en la latencia del host. Ahora T9 y T9(b) disparan en la liberación de SDA por la F334 (`trig=sdar`, `scope_arm_rise`, 3/4 del buffer antes), que ocurre dentro de `RecoverBus`. El `Write` rechazado y los pulsos van antes del disparo, y el STOP después. El firmware mide su propio tramo (inicio del `Write` a fin de `RecoverBus`, sin impresiones entre medias) y lo imprime después; el host exige que quepa antes del disparo con 0,5 ms de margen y que el primer flanco de SCL no esté a menos de 0,5 ms del inicio de la captura. Como la retención puede empezar ahora dentro de la captura, "SDA bajo al empezar" pasa a "SDA bajo desde antes del primer flanco de SCL hasta la liberación, sin flancos de SDA entre medias". Mock en T0B(a) (`t0b_t9_selftest`) con dos casos buenos y tres rechazos |
| m36 | Corregido en `0471570`, pendiente de verificación: `i2c_ll.h` dice que las `LL_I2C_ClearFlag_*` de `ICR` son `SET_BIT` y que `LL_I2C_ClearFlag_TXE`, un `WRITE_REG` de `ISR`, sí se usa |
| m37 | Corregido en `0471570`, pendiente de verificación. `i2c_ll.h` lista todas las escrituras con `LL_I2C_WriteReg`: `TIMEOUTR = 0`, el `CR2` de `StartIt` y de cada recarga, y los RMW de `ie_set()` y `reload()`. Dice que `XferIrq` y `DmaEnd` validan con `I2C_BM_IsValidInstance`. En `i2c_bm.h`, solo un comentario: el `ERR_PARAM` de `DmaStartWrite` con `buf` NULL o `n` < 2. `git diff 12d2ce7 0471570 -- 01_shared_drivers/05_i2c/i2c_bm.h` da 3 líneas de comentario quitadas y 4 añadidas, ninguna firma |
| m38 | Corregido en `4d6b092`, pendiente de verificación: `inst_deinit` juzga con `ist_reset_diff_img()`, nueva en `Common` (`ist_reset_diff()` la llama sobre los registros vivos), así que solo queda una tabla de reset. Corregidas en este commit las dos frases del informe de F2 (§5.3, T14; §5.5) que decían que T14 ya usaba `ist_reset_diff()` |
| m39 | Corregido en `4d6b092`, pendiente de verificación: la comprobación de tiempos y los `RECORD` de T5 llevan el nombre de la prueba. Taps: `F1 T5-LL CM4 100000 Hz scope: times` y sus `RECORD T5-LL …` (p. ej. `RECORD T5-LL CM7 k=1 f=400000: tHIGH min 599 ns`). Antes las cuatro de T5-LL salían con el nombre de las de T5 (dos comprobaciones con el mismo nombre); ahora cada una lleva el suyo y el total sigue en 130 |
| O24, O25 | Sin cambio; al README (el motor de transferencias de `i2c_ll` es una segunda copia del de BM; `LL_I2C_HandleTransfer` conserva `CR2.NACK` y `PECBYTE`) |
| O26–O29 | Sin cambio |
| O30 | Resuelto por R4-1 (plan revisión 4): B1 en ciclos solo en F1 |

### 5.8 Problemas abiertos y no ejercitado (cambios)

- **Cerrado por m39:** el abierto de F2 sobre los nombres de T5-LL en el host.
- **O23 ya no aplica:** el n de T10(b) no depende del intercambio del `AT HOLD` en ningún núcleo (CM4 n = 1, CM7 n = 2, los dos pedidos). Tampoco el caso de un START justo en un múltiplo de TIMEOUTA en CM4 (§F1 corrección 3): ahora empieza a unos µs de la bajada.
- **Lo que se registra y no se juzga:** la latencia del host hasta cada retención de T0C (`first_us`, 62–68 ms en estas sesiones), solo con el límite de 2 s.
- **T9 y T9(b) sin taps:** sin osciloscopio, como antes; T9(b) en notaps se juzga con `5 − pulsos`, y el tramo del firmware solo se registra (1077 µs).
- Lo demás de las secciones F1 y F2 sigue vigente.

## Decisiones abiertas usadas

| # | Opción usada | Consultada |
| --- | --- | --- |
| 1 | `I2cBm_Status_t` en los tres tiers (F1: `i2c_bm`) | Por defecto (encargo de F0) |
| 2 | Pull-ups de 1,0 kΩ al 3V3 de la H755 (el banco del responsable) | Por defecto |
| 3 | 1 MHz en una sesión sin tomas (`--bench-state notaps`; F1: T4, T6–T10, T12 a 1 MHz solo en notaps, con PF14 leído por el firmware) | Por defecto |
| 4 | CM4 I2C1; CM7 I2C2, I2C3, I2C4 (`bench_claim`/`bench_safe` de cada núcleo; F1: I2C1 desde CM4 e I2C4 desde CM7 en el bus) | Por defecto |
| 5 | Recuperación con funciones del llamador (`I2cBm_Recovery_t`; T9) | Por defecto |
| 6 | "No reproducido" para los controles de erratas (T12: no reproducido) | Por defecto |
| 7 | HAL I2C en CM7 solo con la opción (sin uso en F0) | Por defecto |
| 8 | Firmware de la F334 en `tools/i2c_target_f334/`, con el subconjunto del CMSIS de F3 y su licencia Apache-2.0 | Por defecto |
| 9 | Documentos de proceso borrados al cierre (sin uso en F0) | Por defecto |

## Tabla de capacidades

`k_i2c` en `i2c_bm.c` (C1, regla 2): la única fuente de hechos de silicio del driver. La comprueba en el banco la fila "Tabla de capacidades" de §F1 5.3.

| Instancia | Base | Dominio y bus | Habilitación (RCC) | Reloj del kernel | Bit de Fm+ | DMAMUX: RX / TX | IRQ EV / ER |
| --- | --- | --- | --- | --- | --- | --- | --- |
| I2C1 | `I2C1_BASE` | D2, APB1 | `RCC_APB1LENR.I2C1EN` bit 21 (09:7236) | `RCC_D2CCIP2R.I2C123SEL` [13:12] (09:4744-4749) | `SYSCFG_PMCR.I2C1FMP` bit 0 (13:122-137) | DMAMUX1 33 / 34 (tabla 126) | 31 / 32 |
| I2C2 | `I2C2_BASE` | D2, APB1 | bit 22 (09:7230) | ídem | bit 1 | DMAMUX1 35 / 36 (18:113-116) | 33 / 34 |
| I2C3 | `I2C3_BASE` | D2, APB1 | bit 23 (09:7224) | ídem | bit 2 | DMAMUX1 73 / 74 (18:110-111) | 72 / 73 |
| I2C4 | `I2C4_BASE` | D3, APB4 | `RCC_APB4ENR.I2C4EN` bit 7 (09:7683) | `RCC_D3CCIPR.I2C4SEL` [9:8] (09:4868-4873) | bit 3 | DMAMUX2 13 / 14 (tabla 130, 18:235-236) | 95 / 96 |

- **Banco de RCC:** cada núcleo escribe su propio banco (`RCC_C1`/`RCC_C2`, RM0399 9.7.1); un reloj sigue encendido mientras cualquiera de los dos bancos lo habilite (T13).
- **Bases e IRQn:** `stm32h755xx.h`. Los números de DMAMUX los contrasta la prueba con `stm32h7xx_hal_dma.h`.
- **Hechos de registro** (también en `i2c_bm.c`): bits reservados de `TIMINGR` 27:24 (50:2775); `NBYTES` de 8 bits, trozos de 255 (50:1081-1082); `TIMEOUTA` + 1 ≤ 4096 (50:2862); `PE = 0` al menos 3 ciclos de APB (50:441-445); `ISR` en reset `0x00000001` (50:2867).
