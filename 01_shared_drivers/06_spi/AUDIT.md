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

## F0, parte 2: ejecución del auditor en la placa (`153d5b3`)

Checklist físico confirmado por el responsable para esta ejecución (los cinco puntos y J1). `selftest-spi` recompilado en `153d5b3` (0 avisos), flasheado con `tools/flash.py --preset selftest-spi` con el script parado, arranque en frío desconectando y conectando el USB del ST-LINK, y una ejecución de `spi_hw_check.py --checklist-confirmed Hector`. Captura sin editar: `03_firmware_templates/prj_01_template_dualcore/docs/spi_f0_audit_bench_153d5b3.txt`.

| Resultado | Valor |
| --- | --- |
| Identidad | Rev `153d5b3` e id `9b2e44352527ef95` en CM7, CM4 y host; `tree clean` |
| Firmware | `FW VERDICT=PASS failures=0`; sin aborto; CM4 `timeout 0, aborts 0` |
| Host | **9/10**, `HOST VERDICT=FAIL`: falla `T0B(d) firmware timeout` |
| Frente a la entrega | Quitando marcas de tiempo, Rev e id, las 42 líneas de resultado (`-> PASS/FAIL`, veredictos, identidad) son idénticas a `spi_f0_bench_008db8b.txt` salvo T0B(d) y los dos veredictos del host |

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| B1 | Bloqueante (una prueba no se reproduce en mi ejecución) | **Carrera en el host de T0B(d).** `spi_hw_check.py` (l. 390-392) espera la línea `T0B timeout handled` y **después** lee MCU_ACK. Tras esa línea, el firmware empieza el paso siguiente: `step_begin("T0B_S13")` publica el paso y su `sync0()` sube MCU_ACK. En mi captura, `[spi] STEP T0B_S13` (+12,823 s) llega antes de la lectura del host (+12,823 s), que ve ACK alto. En la entrega la lectura cayó 2 ms antes de `STEP` (+12,710 s frente a +12,712 s): pasó por el orden de llegada, no por diseño. El firmware sí puso ACK bajo: su propia comprobación (`GPIOE->ODR` de PE7 = 0 tras `bench_safe()`, l. 1029) da PASS en las dos capturas. No es un defecto de seguridad | Que el host demuestre ACK bajo sin depender del orden: por ejemplo, sondear DIO13 desde el final del handshake hasta verlo bajo (ACK baja en `bench_safe()` y no sube hasta el `sync0()` del paso siguiente), con su límite de tiempo, y solo después esperar la línea. O que el firmware no empiece el paso siguiente hasta que el host lo pida. Corregir, nueva ejecución de entrega y repetiré la mía |

### Estado

**F0 no pasa** por B1. P1 y M1 siguen en la revisión 2 del plan (D9, aprobada por el responsable el 2026-10-03).

## F0, parte 3: verificación de B1 y M1 (`620a678`)

Entrega: `d9a9df7..620a678` (`d8f5e69` B1, `efc1be8` M1, evidencia `0e7a7c4`, informe `620a678`, sección "Respuesta a la auditoría 1").

### Revisión

| Hallazgo | Corrección | Verificación |
| --- | --- | --- |
| B1 | `d8f5e69`. Tras su espera de 10 s y `bench_safe()`, el firmware mantiene MCU_ACK bajo y no sigue hasta un pulso de HOST_REQ (`sync_ack_low()`, acotado por `SYNC_TIMEOUT_CYC`). El host espera ACK bajo con límite de 15 s (`wait_ack`, que lanza `BenchAbort` al vencer), mide el tiempo en alto (9..11 s), lee ACK bajo 20 veces en 100 ms, espera la línea y da el pulso, comprobando ACK bajo tras el flanco. Protocolo v2 en firmware y host | Leído. Ya no depende del orden de llegada de las líneas: mientras el host lee, el firmware está retenido en el mismo paso. **Cerrado** |
| M1 (R2-3) | `efc1be8`. `sst_spi_at_rest()` (común, pura) exige `SPE = 0`, `AFCNTR = 0`, `IOSWP = 0` y `MASTER = 1` o `CFG2 = 0`; S13 (SPI4) la usa también, que es más estricto. `bench_claim()` de CPU1 la aplica con `RCC_C1->APB4ENR.SPI6EN` y la de CPU2 con `RCC_C2->APB2ENR.SPI1EN` antes de cualquier AF en PA5/PA6/PB4/PB5. `bench_safe()` de cada núcleo suelta su SPI con `spi_release()` en el orden de la regla 9 (rev. 2) | Leído en los dos núcleos. T0b(f) con un caso por condición y sus dos controles. **Cerrado** |

### Build (mi ejecución en `620a678`)

`build_all -Clean` 8/8; `prj_01` debug CM7 29304/132/55668, CM4 3456/24/12560, release CM7 28356/132/55672, CM4 3112/24/12560: iguales a `2481396`. `selftest-spi` desde un directorio vacío: 0 avisos.

### Ejecución en la placa

Checklist físico confirmado por el responsable para esta ejecución. Imagen de `620a678` flasheada con el script parado, arranque en frío por USB del ST-LINK, una ejecución: `docs/spi_f0_audit_bench_620a678.txt`, sin editar.

| Resultado | Valor |
| --- | --- |
| Identidad | Rev `620a678` e id `c9289b372b468a8f` en CM7, CM4 y host; `tree clean`; banner v2 |
| Veredictos | 10/10 en el host, `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`, sin aborto |
| T0B(d) | ACK alto 9,877 s sin HOST_REQ, después bajo en 20/20 lecturas |
| R2-3 | CPU1 (SPI6, PA6 AF8) y CPU2 (SPI1, PA6 AF5): rechazados `SPE = 1` (releído 1), `AFCNTR = 1`, `IOSWP = 1`, esclavo con `CFG2 ≠ 0` y sin reloj; aceptados `0x60` (maestro en reposo y `CFG2 = 0`). Cada `bench_safe()` deja su SPI en `CR1 = 0`, `CFG2 = 0`, `SR = 0x00001002` |
| Frente a la entrega | Quitando marcas de tiempo, Rev, id y el tiempo de T0B(d), las 44 líneas de resultado son idénticas a `spi_f0_bench_efc1be8.txt` |

### Estado

**F0 pasa.** B1 y M1 cerrados; P1 resuelto en la revisión 2 del plan (R2-1, R2-2), que se aplica en F1. El implementador puede empezar F1 (`spi_bm`) con la revisión 2.
