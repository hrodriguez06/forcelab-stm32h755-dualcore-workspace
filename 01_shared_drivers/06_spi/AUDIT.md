# Auditoría de 06\_spi

Auditor: Claude (Claude Code), para Hector Rodriguez · Rama: `driver/spi` · Plan: `IMPLEMENTATION_PLAN.md` revisión 1

Este archivo lo escribe solo el auditor. Un hallazgo se cierra cuando el auditor lo verifica.

## F0, parte 1: revisión del código, build y evidencia (`54e2b5c`)

Entrega: `c9cdf78..54e2b5c`. Diez commits, código en `008db8b`, evidencia en `f1e6033`, informe en `54e2b5c`.

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| A23 alcance: `git diff 8901b77..54e2b5c --name-only` | Solo `01_shared_drivers/06_spi/` y `03_firmware_templates/prj_01_template_dualcore/`. `tools/`, `02_firmware_projects/` y los demás drivers, sin cambios |
| Producción de `prj_01` | `main.c` (CM7, CM4), `app_main.c`, `CMakeLists.txt` y `CMakePresets.json` solo ganan bloques `#if defined(SPI_DRIVER_SELFTEST)`, la opción, su `FATAL_ERROR` frente a `GPIO_`/`TIM_DRIVER_SELFTEST` y el preset |
| A16 `.\tools\build_all.ps1 -Clean` en `54e2b5c` (mi ejecución) | 8/8. `prj_01` debug CM7 29304/132/55668, CM4 3456/24/12560; release CM7 28356/132/55672, CM4 3112/24/12560: iguales a `2481396` y a `docs/spi_f0_map_sizes_008db8b.txt` |
| C9 `selftest-spi` desde un directorio vacío (mi ejecución) | Configure 0, 0 líneas `warning:`/`error:`; CM4 6156/24/12560, igual que el informe §5.2 |
| C1–C15 | No aplican al driver en F0: no hay `spi_*.[ch]` ni targets `_spi_*` (correcto según la tabla de fases) |

### A27: seguridad eléctrica (lectura)

| Punto | Dónde | Resultado |
| --- | --- | --- |
| Tabla única de permisos | `Common/Src/spi_selftest_common.c` | `sst_line_rule()` deniega por defecto. PE0–PE7: nunca AF ni salida. PE8–PE15: solo CPU1 y solo en pasos "bus = MCU"; AF únicamente AF5 en PE11–PE14 y con el bit del paso. J1: un núcleo por paso, AF5 (CPU2) u AF8 (CPU1), PB5 salida solo en T0C, PA5/PA6/PB4 nunca salida. PA4, PA7, PG13, PC13, SWD y PF14/PF15: rechazados. Coincide con S4, S13 y S14 |
| Rutas de escritura a pines, CM7 | `CM7/App/Src/spi_selftest.c` | Toda configuración pasa por `pin_cfg()` (l. 313), que llama a `bench_claim()` antes de una salida o un AF. Las únicas escrituras directas son: `bench_safe()` (l. 192-255, pines a entrada), `ack()` (MCU_ACK) y el `BSRR` de PB5 en T0C (l. 859), con PB5 ya reclamado como salida |
| S13, lectura de SPI4 | `spi4_s13_ok()` (l. 275-288) | Reloj en el banco de CPU1, `SPE = 0`, `AFCNTR = 0` y `MASTER = 1` o `CFG2 = 0`; solo lee. `bench_claim()` la exige para cualquier AF en PE11–PE14 (l. 304-307) |
| Rutas de escritura a pines, CM4 | `CM4/App/Src/spi_selftest.c` | Solo J1 (GPIOA/GPIOB) en `bench_safe()` (l. 47-56) y en la ruta reclamada. GPIOE solo aparece en casos que deben rechazarse (l. 145-148) y en la foto de registros (l. 162): no lo escribe |
| Host | `prj_01/tools/spi_hw_check.py` | Ninguna llamada a `ad2_set_dout` ni a `FDwfDigitalOut*` de configuración. Todo cambio de OE pasa por `spi_set_oe()` (solo HOST_REQ) y después por `ad2_set_oe()` (S3). `tools/ad2.py`, sin cambios |
| Casos con SPI4 activo | `step_t0b_s13()` | El caso `SPE = 1` corre con PE11–PE14 en entrada (el reclamo se rechaza y no se escribe). El control `AFCNTR = 1` solo deja conducir SCK y MOSI, con el AD2 en entrada: sin contienda, como pedía el plan |

### Evidencia de la entrega

`docs/spi_f0_bench_008db8b.txt`: una ejecución, marcas de tiempo de `[host] argv:` a `HOST VERDICT=PASS`, checklist confirmado con la línea de J1, `tree clean`, Rev `008db8b` e id `a343a195b09c7a13` iguales en CM7, CM4 y host. T0a (13 líneas, walking-1, control DIO9 `0x0200`), T0c (control primero, 16 niveles de PB5 en PA6, PB4/PA5 en su pull), T0b(a)–(f) y "No Digital Out" en las 8 fronteras de paso: todo en verde, 10/10 comprobaciones del host, `FW VERDICT=PASS failures=0`. La primera ejecución (`docs/spi_f0_bench_65e19f7.txt`, `FW VERDICT=FAIL`) está commiteada y explicada en el informe §5.8, sin editar.

Las desviaciones del informe §5.5 son todas más estrictas que el plan o lo amplían sin rebajar nada; las acepto. Destaco la de T0c: el control va primero, y PB5 solo pasa a salida si la red de J1 sigue al pull de PA6.

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| P1 | Errata del plan (la planteó el implementador, informe §5.7) | **Confirmada.** RM0399 53:1464-1476 no limita MODF a `SPE = 1`: un maestro con `SSM = 1` y `SSI = SSIOP` lo levanta, y con MODF puesto `SPE` no se puede activar (53:3280, 53:3657-3661). La regla 3 escribe `CFG2` (`MASTER = 1`, `SSM = 1`) con `CR1.SSI` todavía en reset (0, activo con `SSIOP = 0`), y la regla 9 vuelve `CR1` a 0 con `CFG2` aún de maestro. Además, con `keep_pins` vigente, la secuencia de C11 (`CFG2` con `AFCNTR = 0` y después `AFCNTR = 1`) soltaría los pines un instante en un re-`Init` | Revisión 2 del plan (D9) |
| M1 | Mayor (para F1, no afecta a F0) | S14 solo juzga pines. Nadie comprueba el estado de SPI1/SPI6 cuando sus pines de J1 pasan a AF. Un SPI en esclavo (`MASTER = 0`, que es lo que deja un MODF) o con `IOSWP = 1` conduciría PA6. Con un solo SPI en J1 no hay contienda, pero S14 dice "PA6 nunca salida". En F0 solo PA5 entra en AF, con SPI1 en reset | Revisión 2 del plan (D9): `bench_claim()` de J1 en AF lee la instancia dueña como S13 lee SPI4, y T11 (MODF) pasa a SPI2, sin pines. Antes de la primera prueba de F1 que ponga J1 en AF |

### Estado

La parte 1 pasa. **F0 queda pendiente de mi ejecución en la placa (parte 2)**, que necesita la confirmación del checklist físico por el responsable.

**D9 (para el responsable):** revisión 2 del plan con: (a) regla 3: `CR1.SSI` inactivo antes de `CFG2`; si el `CFG2` actual es de maestro con `SSM = 1` y otro `SSIOP`, primero `CFG2 = 0`; con `AFCNTR = 1` vigente, mismo `CPOL` y `SSIOP` o `ERR_BUSY`, y `CFG2` en una sola escritura con `AFCNTR = 1`; read-back de `SR.MODF = 0`. (b) Regla 9: `IER = 0` → `SPE = 0` conservando `SSI` → `CFG2 = 0` → `CR1 = 0` → resto → `IFCR`. (c) S14 con la lectura de la instancia dueña (M1). (d) T11 en SPI2.
