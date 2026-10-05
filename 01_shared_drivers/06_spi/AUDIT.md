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

## F1: `spi_bm` (`b4fa22d`)

Entrega: `6a55bb0..b4fa22d` (código en `5c117ce`, evidencia `81ac85c` en `4142921`, informe `b4fa22d`).

### Revisión del driver (`spi_bm.h`, `spi_bm.c`)

| Punto | Resultado |
| --- | --- |
| A1–A5, C1–C4 | Sufijo, includes sin prefijo, `stm32h7_driver_spi_bm` sin target sin sufijo, paraguas igual. `grep` de A2: 0 literales de proyecto. `spi_bm` incluye solo `stm32h7xx.h`, `<stdint.h>` y `<stdbool.h>` (define `NULL` él mismo) |
| A15 tabla de capacidades | `k_spi[]` (l. 51-59) igual a la regla 2 del plan, fila por fila, con sus citas del RM. Contrastada además en T2 `GetCaps` y T7(d) |
| A6 C6 | `chk_config()`: `NULL` → instancia → capacidad (`data_bits` > máximo de la instancia: `ERR_UNSUPPORTED`) → rango → estado. `chk_transfer()` igual. Ningún camino escribe antes de validar |
| C7 | `.data = .bss = 0` en `spi_bm.c.obj` (mi `.map`: CM7 `.text` 3324 + `.rodata` 96, CM4 3266 + 96, igual que el informe). El estado de IT vive en `SpiBm_Xfer_t` |
| A9 C10 | RMW de `CR1`, `CFG1`, `IER` y de los enable de RCC bajo PRIMASK guardado y restaurado (`irq_save`/`irq_restore`); `IFCR` siempre en una escritura; `RCC_D2CCIP1R`/`D3CCIPR` solo leídos; ningún `RSTR` |
| R2-1 (`Init`, l. 382-440) | `IER = 0` → `IFCR` → `CFG2 = 0` solo si el vigente es maestro con `SSM = 1`, otro `SSIOP` y `AFCNTR = 0` → `CR1 = SSI` inactivo → `CFG1` (`CRCSIZE = 00111`) → `CFG2` en una o dos escrituras según `AFCNTR` vigente (C11) → `SR.MODF` → read-back de `CFG1`, `CFG2`, `CR1`. Correcto |
| R2-2 (`Deinit`, l. 442-465) | `IER = 0`, `SPE = 0` conservando `SSI` (bajo PRIMASK), `CFG2 = 0`, `CR1 = 0`, `CR2`, `CFG1`, `IFCR`, read-back con `MODF`, `CRCPOLY`, `UDRDR` e `I2SCFGR`. Correcto |
| Regla 4 y erratas (`finish()`, l. 667-684) | Drenaje de la RxFIFO, guarda contada desde `EOT` (2.22.5), `IER = 0` antes de `SPE = 0` (2.22.3), `SPE = 0` tras cada `EOT` (2.22.1), `IFCR`. En full duplex, `service_tx()` no pone en vuelo más tramas de las que caben en la RxFIFO, así que no hay `OVR` por servicio tardío |
| Regla 6 | Un acceso por trama del ancho del elemento; sin empaquetado (E1 del informe, aprobado por el responsable). Umbral con 1/2/3/4 B por trama (53:927-929) |
| Regla 8 (DMA) | `DmaPrepare` (`TSIZE`, `RXDMAEN`), `DmaStart` (`TXDMAEN`, `SPE`, `CSTART`), `WaitEnd`, `DmaEnd` (guarda, `IER = 0`, `SPE = 0`, bits de DMA, `IFCR`): el orden de 53:1262-1274 |
| C13 | `Transfer` y `WaitEnd`: límite `max_cycles` y detección de `CYCCNT` parado (1000 lecturas iguales). Ver m1 |
| C14 | Sin handlers, callbacks ni `NVIC_*` |

### Seguridad del banco (A27)

Pasos nuevos `F1_NOPIN`, `J1_SPI6` (CPU1, AF8), `J1_SPI1` (CPU2, AF5) y `SPI4_AD2` (bus = MCU, PE11–PE14 en AF5, PA8 en AF0). Toda AF pasa por `bench_claim()` con S13/R2-3. El control D2 suelta un MISO antes de poner el otro en AF (`spi6_pins()`, l. 2342-2359). Buffers de BDMA solo en CPU1 (`.d3_ram`). `spi_hw_check.py`: ninguna llamada que configure Digital Out; enable releído 0 en las 12 fronteras. `tools/ad2.py`: solo añade (0 líneas quitadas).

### Build y ejecución (mías)

`build_all -Clean` 8/8 en `b4fa22d`, `prj_01` igual a `2481396`; `selftest-spi` desde vacío con 0 avisos. Checklist físico confirmado por el responsable; flasheo con el script parado, arranque en frío, una ejecución sin editar: `docs/spi_f1_audit_bench_b4fa22d.txt`.

| Resultado | Valor |
| --- | --- |
| Veredictos | 29/29 en el host, `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`, sin aborto; Rev `b4fa22d` en CM7, CM4 y host, `tree clean` |
| Frente a la entrega | 125 líneas de resultado; solo cambian valores medidos, todos dentro de su cota: T5(d) `mssi=15` 237 frente a 236,4 (entrega 236 frente a 237,9), T5(g) entre −165 y +218 ppm, T9 y su control con `MBR + 1` en torno a −500 000 ppm, controles de T8 1019/1018 discrepancias, T4 la más larga 2 098 952 ciclos |
| Erratas | T12(a) control: 20 bloqueos en 40 (2.22.1 reproducida otra vez); T12(b) sin guarda: último pulso de 1 muestra frente a 126,3 (2.22.5 reproducida); con guarda, 126 |

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| M2 | Mayor | **E2 sin evidencia de entrega.** `spi_bm.h` (l. 47-53) publica como contrato que un maestro sin SCK en AF no termina, y la única fuente es una captura de desarrollo con árbol sucio (`spi_dev_f1_bench_25426f8-dirty.txt`), que no vale como evidencia (instrucciones §3.4). Tampoco hay en la entrega ningún `ERR_TIMEOUT` provocado, así que el camino de aborto de C13 no está ejercitado | Una prueba en el selftest: SPI6 por J1, 1 trama con PA5 (SCK) en entrada → `ERR_TIMEOUT` dentro de `max_cycles`, registros tras el aborto (`SPE = 0`, `IER = 0`) e imagen de `Deinit`; control con PA5 en AF8 → `OK`. En la entrega de F2 |
| m1 | Menor | `wait_from()` (l. 656-661), la guarda de `finish()` y `DmaEnd()`, no tiene salida si `CYCCNT` se para después de la comprobación inicial (C13: "ningún bucle sin salida") | Añadir el mismo contador de lecturas iguales que usa el bucle principal. Antes del cierre; es un cambio interno, no de API |
| m2 | Menor | El control de T3 solo prueba que el oráculo distingue en `CalcPrescaler` (`MBR + 1`); `CalcGuardCycles` no tiene esperado alterado (R1) | Un esperado alterado también para `CalcGuardCycles`. En la entrega de F2 |
| N1 | Nota | T2 no compara con la imagen de la HAL cruda en CM7 (plan): necesita `HAL_SPI_MODULE_ENABLED`, que la decisión 7 deja para F3 | Pasa a F3 (revisión 3 del plan) |

Las desviaciones del informe §5.5 y las erratas E1–E4 de §5.7 son correctas. Las tres que el informe dice aprobadas por el responsable el 2026-10-03 (`guard_cycles` en cada llamada que la aplica; T9, T11(b) y B1 en instancias con pines; regla 6 según el RM) cambian el contrato o la tabla de pruebas del plan: van a la revisión 3 (D10).

### Estado

**F1 pasa.** La API de `spi_bm` queda **congelada** con `spi_bm.h` tal como está en `b4fa22d` (C2: desde aquí solo se añade). M2 y m2 se entregan con F2; m1, antes del cierre.

**D10 (para el responsable):** revisión 3 del plan, que recoge lo ya aprobado y lo verificado en F1: (a) `guard_cycles` como parámetro de `Transfer`, `DmaEnd` y `SpiBm_Xfer_t`, fuera de `SpiBm_Config_t`; (b) regla 6 según el RM (sin empaquetado; 1/2/3/4 B por trama en el umbral); (c) E2 en el contrato y en "No ejercitado": T9 en SPI6, SPI4 y SPI1; T11(a) en SPI2 sin transferir y T11(b) en SPI6; B1 en SPI6/SPI1; (d) T2 contra la HAL cruda en F3; (e) T5: flancos de muestreo e intervalos de SS, y periodo medido en la misma captura; (f) citas E3, E4; (g) M2 como prueba nueva (T15).

## F2: `spi_ll` (`99fe89b`)

Entrega: `b9de729..99fe89b` (m1 `544fb99`, `SPI_BM_CheckDma*` `14ea027`, `spi_ll` `e731a9f`, selftest `d87a576`/`73c1b76`, evidencia `0f05976`, informe `99fe89b`).

### Revisión

| Punto | Resultado |
| --- | --- |
| A3 C2 (`spi_bm.h`) | `git diff b4fa22d..99fe89b -- spi_bm.h`: solo añade `SPI_BM_CheckDmaPrepare` y `SPI_BM_CheckDmaStart`. El responsable confirmó el 2026-10-04 que las aprobó (el implementador las añadió antes de preguntar y lo declara en su informe, §5.5) |
| m1 | `wait_from()` cuenta lecturas iguales de `CYCCNT` (`STALL_READS`, una sola constante) y, si se rinde, `finish()` y `DmaEnd()` deshabilitan igual y devuelven `ERR_TIMEOUT`. Leído en `spi_bm.c` y en la copia de `spi_ll.c`. No ejercitado en la placa (necesitaría parar `CYCCNT` a mitad de una guarda); lo acepto por lectura. **Cerrado** |
| Contrato de `spi_ll` | Cada constante LL se traduce a su valor BM (`to_bm()`); una desconocida da un valor que `spi_bm` rechaza en su sitio de C6. Valida solo con `SPI_BM_CheckConfig`, `SPI_BM_CheckTransfer` y `SPI_BM_CheckDma*`: ni la tabla de capacidades ni ninguna validación se duplican (`GetCaps` para el tamaño de FIFO) |
| Escrituras (C8, C10) | Solo inlines de `stm32h7xx_ll_spi.h` y `LL_SPI_WriteReg`/`ReadReg`; sin `USE_FULL_LL_DRIVER` ni `LL_SPI_Init`. Los RMW de LL (`Enable`/`Disable`, `StartMasterTransfer`, `Enable`/`DisableDMAReq_*`) bajo PRIMASK; `IFCR` en una escritura, sin `LL_SPI_ClearFlag_*` (son `SET_BIT`, el informe lo explica). Mismo orden que BM en `Init` (R2-1), fin de transferencia (las cuatro erratas), aborto y DMA |
| C7 | `.data = .bss = 0` en `spi_bm.c.obj` y `spi_ll.c.obj` (informe §5.2) |
| Selftest | La configuración LL de cada prueba sale de `sst_ll_of()` con constantes escritas a mano, no de la traducción de `spi_ll.c`: el oráculo es independiente. Sin cambios en las guardas S13/S14 ni en Digital Out (enable 0 en las 16 fronteras) |
| Alcance | `git diff b9de729..99fe89b --name-only`: nada fuera de las rutas permitidas |

### Build y ejecución (mías)

`build_all -Clean` 8/8 en `99fe89b`, `prj_01` igual a `2481396`; `selftest-spi` desde vacío con 0 avisos; imágenes 91816/140/253556 (CM7) y 25788/28/45400 (CM4), como el informe. Checklist físico confirmado por el responsable; flasheo con el script parado, arranque en frío, una ejecución sin editar: `docs/spi_f2_audit_bench_99fe89b.txt`.

| Resultado | Valor |
| --- | --- |
| Veredictos | 38/38 en el host, `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`; Rev `99fe89b` en CM7, CM4 y host |
| T2 LL | SPI1..6: la imagen de `spi_ll` difiere de la de `spi_bm` en 0 y de la tabla del RM en 0; control (`MASTER` invertido) detectado |
| T15 (M2) | BM: `ERR_TIMEOUT` tras 401 009 ciclos; LL: tras 400 986 (`max_cycles` 400 000, cota +10 %). Controles con PA5 en AF8: `OK` y `0xA5` recibido por J1. `Deinit` a la imagen de reset |
| T3 (m2, R3-6) | El esperado alterado de `CalcGuardCycles` marca 83 de 83 |
| Frente a la entrega | 167 líneas de resultado; solo cambian medidas del AD2 (T5(a) control, T5(d), T5(g), T12(b)), todas dentro de su cota. T5(d): el punto más justo, `mssi=4` 62 frente a 63,1 (± 2) |

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| M2 | — | T15 lo demuestra en una captura de entrega, con BM y LL, y ejercita el aborto de C13 | **Cerrado** |
| m2 | — | Control de `CalcGuardCycles` en T3 | **Cerrado** |
| m3 | Menor (para F3) | La pila de `Status_Task` baja con cada fase: 272, 182 y 128 palabras libres de 512 (F0, F1, F2). F3 añade la HAL encima | D11 |
| N2 | Nota | `LL_SPI_SetTransferSize` es un RMW de `CR2` sin PRIMASK, mientras `spi_bm` escribe `CR2` entero. Con `SPE = 0`, `TSER = 0` y un solo dueño por instancia no tiene efecto; T2 da la misma imagen | Solo registro |

### Estado

**F2 pasa.** La API de `spi_ll` queda **congelada** con `spi_ll.h` de `99fe89b`, y `spi_bm.h` sigue congelada con las dos funciones añadidas.

**D11 (para el responsable):** autorizar que `CM7/App/Src/app_main.c` dé a `Status_Task` más pila **solo con `SPI_DRIVER_SELFTEST`**, como ya hace con `GPIO_DRIVER_SELFTEST` (2048 palabras, `app_main.c:52-56`). La producción no cambia (A16 lo comprueba). Requisito para F3: la captura de entrega con al menos 128 palabras libres.

**D11 aprobada** por el responsable el 2026-10-04: con `SPI_DRIVER_SELFTEST`, `app_main.c` puede dar a `Status_Task` más pila (el mismo mecanismo de `GPIO_DRIVER_SELFTEST`, `app_main.c:52-56`). Requisito de F3: producción sin cambios (A16) y la captura de entrega con al menos 128 palabras libres.

## F3: `spi_hal` (`b267f4c`)

Entrega: `5d6f21a..b267f4c` (`spi_hal` `4e147b5`, build y D11 `dbab128`, corrección `3c23568`, selftest `b9b6237`, capturas de desarrollo `efb4a3f`, evidencia `5f9f1f3`, citas `4e96c69`, informe `b267f4c`).

### Revisión

| Punto | Resultado |
| --- | --- |
| Alcance y C2 | Nada fuera de las rutas permitidas. `spi_bm.{h,c}` y `spi_ll.{h,c}` sin cambios desde `99fe89b` |
| Producción de `prj_01` | `stm32h7xx_hal_conf.h` de CM7 gana `HAL_SPI_MODULE_ENABLED`, su `#include` y `USE_HAL_SPI_REGISTER_CALLBACKS 0U`, los tres bajo `SPI_DRIVER_SELFTEST`. `app_main.c`: 1024 palabras para `Status_Task` solo con la opción (D11). Mi `build_all -Clean`: producción igual a `2481396` |
| C8 | `spi_hal.h`/`.c` no declaran ni compilan nada sin `HAL_SPI_MODULE_ENABLED`; `#error` con `USE_SPI_CRC != 0` (regla 10). `_Static_assert` de que las constantes de la HAL son los campos del registro |
| C6 | `h`, instancia y `h->Instance == cfg->spi`, después `SPI_BM_CheckConfig`/`CheckTransfer`, después el estado del handle; en DMA, además, los handles enlazados y su tamaño de memoria antes de llamar a la HAL (la HAL lo rechazaría dejando el handle ocupado) |
| R2-1 en `SPI_HAL_Init` | `SPI_BM_Deinit` primero y el handle a RESET: desde la imagen de reset, la HAL pone `SSI` (l. 375-381) antes de `CFG2` (l. 398) y conserva `CRCSIZE = 00111` (l. 367). Decisión del responsable ("SPI_BM_Deinit first", confirmada el 2026-10-04) |
| C12 | Read-back tras cada llamada: `CFG1`, `CFG2`, `CR1`, `SR.MODF` y estado del handle en `Init`; `SPE`, `IER` (salvo `TSERFIE`, que la HAL deja: l. 1743/1837/1964 frente a la máscara de l. 3948), DMA, `TSIZE` y `COMM` en las transferencias |
| C14 | Sin callbacks ni MSP; `SPI_HAL_Deinit` no llama a `HAL_SPI_DeInit`. El selftest no define `HAL_SPI_MspInit` (se usa la débil, vacía) |
| Limitaciones de la HAL | Documentadas en `spi_hal.h`: 2.22.5 sin guarda (decisión 5); 2.22.3 en el camino IT con todas las tramas en la FIFO, una IRQ tras el cierre (decisión del responsable "Keep HAL, document", confirmada el 2026-10-04; T6 la cuenta y la espera); `keep_pins` suelta los pines unos ciclos en el `Init`; `TSERFIE` queda a 1; `COMM` pasa a solo TX con `rx == NULL` |

### Build y ejecución (mías)

`build_all -Clean` 8/8 en `b267f4c`; `selftest-spi` desde vacío con 0 avisos. Checklist físico confirmado por el responsable; flasheo con el script parado, arranque en frío, una ejecución sin editar: `docs/spi_f3_audit_bench_b267f4c.txt`.

| Resultado | Valor |
| --- | --- |
| Veredictos | 45/45 en el host, `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`; Rev `b267f4c` en CM7, CM4 y host |
| T2 HAL | SPI2..6: imagen de `SPI_HAL_Init` igual a la de `HAL_SPI_Init` crudo en 0 diferencias; frente a `spi_bm`, 0 fuera de la diferencia por diseño (`CR1.SSI` con SS por hardware activo bajo) |
| T6 HAL | IRQ tras el cierre de la HAL = esperado (1 con 1 trama, 0 con FIFO + 1 y 1024), en 8 y 16 bits; BM y LL, 0 |
| Tick y pila | 12 líneas `TICK` estrictamente crecientes, la última después de `DONE`; `Status_Task` con 640 palabras libres de 1024 (D11 pide ≥ 128) |
| Frente a la entrega | 196 líneas de resultado; solo cambian medidas del AD2, todas dentro de su cota |

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| m3 | — | Pila de `Status_Task` (D11): 640 palabras libres | **Cerrado** |
| m4 | Menor | `SPI_HAL_Wait` (`spi_hal.c:305-312`) no tiene salida si `HAL_GetTick()` no avanza: con PRIMASK puesto o desde una ISR de prioridad 0 (la del tick por TIM6 en `prj_01`, `TICK_INT_PRIORITY 0`). C13 pide que ningún bucle se quede sin salida | Salida por tick parado (por ejemplo, contar con `CYCCNT` o un número de vueltas sin cambio de tick → `ERR_TIMEOUT`), y la precondición en `spi_hal.h`. Antes del cierre; cambio interno |
| N3 | Nota (fuera de `06_spi`) | **Defecto en `02_dma`:** `DMA_HAL_Init` (`dma_hal.c:183-184`) compara en modo directo `periph_align` (`DMA_PDATAALIGN_*`, bits `PSIZE` 12:11) con `mem_align` (`DMA_MDATAALIGN_*`, bits `MSIZE` 14:13), así que rechaza el modo directo con media palabra o palabra aunque `PSIZE = MSIZE`. T7(c) lo evita con modo FIFO | Tarea aparte para `02_dma` (driver cerrado); no bloquea `06_spi` |

### Estado

**F3 pasa.** La API de `spi_hal` queda **congelada** con `spi_hal.h` de `b267f4c`. Las tres APIs están congeladas.

**Cierre:** pido al implementador el README (plan, "README de cierre") y la línea de `06_spi` en `01_shared_drivers/README.md`, con m4 corregido en un commit propio. El README tiene que llevar: E2/R3-3 (SCK en AF) con T15 como evidencia; las limitaciones de `spi_hal`; SP3 (`.d3_ram` en los dos linker scripts); SP4 y N1 (reloj de la placa); N3 como nota de consumo de `02_dma`; y lo no ejercitado del plan y de los informes.

## Cierre, parte 1: m4 y README (`575ace9`)

Entrega: `6675064..575ace9` (m4 `1c8ab15`, su prueba `8b5d165`, evidencia `cc951c4`, README y línea del catálogo `575ace9`).

### m4

`SPI_HAL_Wait` (`spi_hal.c:302-328`) sale con `ERR_TIMEOUT` tras 2 × `HAL_GetTickFreq()` × (`SystemCoreClock` / 1000) lecturas con el mismo tick (al menos dos periodos de tick), además de `timeout_ms`. `spi_hal.h` solo cambia el comentario de `SPI_HAL_Wait` con la precondición (C2). El selftest lo prueba con PRIMASK puesto (tick parado) y con el control con el tick en marcha (`spi_f3_bench_8b5d165.txt`: `ERR_TIMEOUT` tras 14 400 178 ciclos con el tick sin cambiar; control, `ERR_TIMEOUT` tras ≥ 10 ms). Alcance: nada fuera de `06_spi`, `prj_01` y la línea de `01_shared_drivers/README.md`. **Cerrado.**

### README

600 líneas (el máximo), en inglés, con las secciones del plan en orden. Comprobado por mí y por una verificación sistemática de cada cifra y cita:

- **Cifras:** unas 115, contra la captura que cita cada una: 0 discrepancias. Las del `.map` (`spi_f3_map_sizes_8b5d165.txt`) y los recuentos de las 18 capturas de §9.1, también.
- **API:** 24 declaraciones BM, 9 funciones LL, 6 HAL, 11 estados, los campos de `SpiBm_Config_t`, `SpiBm_Xfer_t` y `SpiLl_Config_t` en orden: iguales a las cabeceras.
- **Tabla de capacidades** igual a `k_spi[]`; las seis citas `53:<línea>`, las del HAL (l. 375-381, 398, 474, 3121, 3124), los linker scripts y `dma_hal.c:183-186`, correctas. Los 25 hashes de la historia existen y su asunto coincide.
- **Ejemplos:** los tres compilan, en mi ejecución, con `-Wall -Wextra -Wpedantic -Wconversion -Werror` (A para CM4; B y C para CM7 con `SPI_DRIVER_SELFTEST`), cabeceras de CMSIS y HAL como `-isystem`.
- **Sin sobreafirmaciones** sobre velocidades por encima de 25 MHz, SPI2/3/5 o la HAL en CM4.

### Hallazgos del README

| ID | Clase | Línea | Hallazgo | Corrección |
| --- | --- | --- | --- | --- |
| M3 | Mayor | 552-553 (§10.2) | "`Transfer` and `WaitEnd` then return `ERR_TIMEOUT` … and leave the SPI disabled": `WaitEnd` no deshabilita (`spi_bm.c:976-978`, `spi_ll.c:614`, `spi_bm.h:192`); T15 solo ejercita `Transfer` | `Transfer` deja `IER = 0`, `SPE = 0`; `WaitEnd` devuelve `ERR_TIMEOUT` con el SPI habilitado: parar los streams y después `DmaEnd` |
| M4 | Mayor | 164 (§3) | `SPI_BM_NSS_HW_PULSE` "for `midi` ≥ 1": RM0399 53:3470 ("when MIDI[3:0]>1") y 53:615-616 piden `MIDI > 1`; T5(c) usó 2 y el driver no lo comprueba | "`midi` ≥ 2 (RM0399 53:3470); the driver does not check it" |
| M5 | Mayor | 251 (§6.3), 416 y 431 (§8) | MCO1 se usa solo en T5(g) (`spi_hw_check.py:804`), y solo en el paso `SPI4_AD2` (F1); T5(d) y T12(b) miden el periodo de SCK dentro de la misma captura (R3-5). El README dice "every T5 period", "T5 measures SCK against MCO1" y "`RCC_CFGR` only in the SPI4 steps" | "T5(g) only (step `SPI4_AD2`)"; "T5(g) is a ratio to MCO1; T5(d) and T12(b) count in SCK periods measured in the same capture" |
| M6 | Mayor | 515-527 (§9.5) | Omisiones frente al informe (§5.8 de F1–F3): `TransferIrq` de **BM** con `MODF`/`OVR` (solo dice LL); `keep_pins` solo en SPI4 con `CPOL = 0`; 65 535 tramas solo en SPI6, 8 bits, por sondeo (la fila T4 de l. 473 sugiere también SPI1); el camino de C13 con `CYCCNT` sin contar o parado en `Transfer`/`WaitEnd`/`TransferStartIt` (solo dice "inside a guard"); el `ERR_TIMEOUT` de `SPI_HAL_Transfer` (el de la HAL) | Añadirlos a "Not exercised" |
| m5 | Menor | 244, 481 | Control de T12(b): "156 samples" es correcto en `8b5d165`, pero 156 > 126,4 no es truncado, y varía entre ejecuciones: 1–2 muestras (reproducido) en `c41040a`, `4142921`, `b4fa22d`, `73c1b76`, `b267f4c`; 146–156 en `99fe89b`, `efb4a3f`, `8b5d165`. La decisión 6 pide decir si se reproduce | "Not reproduced in this run; reproduced (1–2 samples) in 5 of the 8 clean runs" |
| m6 | Menor | 35 | "builds empty in `prj_01`": en CM4 `spi_hal.c` ni se compila (`CM4/CMakeLists.txt` no enlaza `_spi_hal`) | "not built on CM4" |
| m7 | Menor | 51 | LL "use `SPI_BM_*`" para `Check*`, pero existe `SPI_LL_CheckConfig` (l. 120 lo cita) | Matizar |
| m8 | Menor | 136 | `SPI_HAL_Init` no comprueba el estado del handle: lo fuerza a RESET tras `SPI_BM_Deinit` (`spi_hal.c:113-118`); solo las transferencias exigen `READY` | Precisar |
| m9 | Menor | 174 | `WaitEnd` termina en `EOT` (con `ERR_OVERRUN` si hay `OVR`), en `MODF` o por tiempo; `OVR` solo no la termina | Precisar |
| m10 | Menor | 441 | "Every test prints its negative control", pero T10 y la fila de tick/pila no tienen | "every test that has one" o el control de T10 (T9) |
| m11 | Menor | 406, 417 | J1 también lo usan T9, T11(b), T12(a) y B1 | Completar |
| m12 | Menor | 258, 279-292 | "Pins are those of the bench", pero el CS del ejemplo A es PD14, que no es una línea del banco | Decir que PD14 es un GPIO libre del proyecto, o usar otro y decirlo |
| m13 | Menor | 587-600 (§13) | Los documentos de proceso se citan por hashes intermedios (`AUDIT.md` en `6675064`, el informe en `b267f4c`), que no serán sus versiones finales | La forma de recuperación de `03_tim`: `git show $(git log -1 --format=%h --diff-filter=D -- <ruta>)^:<ruta>` |

Notas (opcionales): `spi_f1_bench_c41040a.txt` figura como entrega sin decir que la sustituyó `4142921` (l. 453); `spi_dev_f3_bench_dbab128-dirty_3` (45/45) está bajo "Failed runs kept" (l. 461); "B1" nombra a la vez el hallazgo de F0 y la medida del plan (l. 459, 502); `git diff main -- …` solo sirve después del merge (l. 578); el bucle se rinde con más de 1000 lecturas iguales, no con 1000 (l. 211).

### Estado

**El cierre no pasa todavía:** M3–M6 son afirmaciones falsas u omisiones del README. Con M3–M6 y m5–m13 corregidos (sin pasar de 600 líneas), y la respuesta en el informe, haré la parte 2. No hace falta otra ejecución en la placa: m4 ya está en `spi_f3_bench_8b5d165.txt`.

## Cierre, parte 2: verificación del README (`3dfb2cf`)

Entrega: `6c66ac8..3dfb2cf` (M3–M6 `0f10fd6`, m5–m13 y notas `fc5ec16`, respuesta en el informe `3dfb2cf`). README: 600 líneas.

| Hallazgo | Corrección en el README | Verificación |
| --- | --- | --- |
| M3 | §10.2 y §3: `Transfer` deja `IER = 0`, `SPE = 0`; `WaitEnd` devuelve `ERR_TIMEOUT` con el SPI habilitado (parar streams, después `DmaEnd`); la evidencia de T15 es solo de `Transfer` | Igual a `spi_bm.c`/`spi_ll.c`. **Cerrado** |
| M4 | §3: `NSS_HW_PULSE` solo con `midi` ≥ 2 (53:3470, 53:615-616), sin comprobación del driver | **Cerrado** |
| M5 | §6.3, diagrama y tabla de §8, línea de `RCC_CFGR`: MCO1 solo para T5(g) en `SPI4_AD2`; T5(d) y T12(b) en periodos medidos en la captura (R3-5) | **Cerrado** |
| M6 | §9.5: `TransferIrq` de BM y LL con `MODF`/`OVR`; C13 con `CYCCNT` sin contar o parado; `keep_pins` solo con `CPOL = 0`; 65 535 tramas solo en SPI6 (también en la fila T4); `ERR_TIMEOUT` de `SPI_HAL_Transfer` | Coincide con el informe (§5.8 de F1–F3). **Cerrado** |
| m5–m13 | Control de T12(b) (156, no reproducido aquí; 1–2 muestras en 5 de 8 ejecuciones limpias); "not built on CM4"; `SPI_LL_CheckConfig`; `SPI_HAL_Init` sin comprobación de estado; fin de `WaitEnd`; controles negativos "that has one"; J1 también en T9, T11(b), T12(a), B1; PD14 declarado fuera del banco; forma de recuperación de los documentos de proceso | **Cerrados** |
| Notas | `c41040a` sustituida por `4142921`; `dbab128-dirty_3` aparte; "B1 of the plan" frente al hallazgo B1 de F0; `git diff main` "after the merge"; "more than 1000" | Atendidas |

El código de los tres ejemplos no cambia en esta entrega: sigue valiendo mi compilación con `-Werror` de la parte 1.

### Estado

**El cierre pasa.** `06_spi` queda auditado: F0–F3 y README. Todos los hallazgos de la rama están cerrados, salvo N3, que es de `02_dma` y está propuesto como tarea aparte.

Pasos del responsable del catálogo, no del implementador:

1. `DRIVER_ROADMAP.md`: la fila de `06_spi` y su sección.
2. `02_dma/README.md` §9: la línea "BDMA with a real D3 peripheral" queda ejercitada con SPI6 (T7(b), BM, LL y HAL). Más la nota N3.
3. `00_gpio/README.md` §8: la mención de J1.
4. Borrado de los documentos de proceso, merge a `main` y borrado de la rama.
