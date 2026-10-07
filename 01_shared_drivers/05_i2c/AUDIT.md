# Auditoría de 05\_i2c

Auditor: Claude (Claude Code), para Hector Rodriguez · Rama: `driver/i2c` · Plan: `IMPLEMENTATION_PLAN.md` revisión 1

Este archivo lo escribe solo el auditor. Un hallazgo se cierra cuando el auditor lo verifica.

## F0, parte 1: revisión del código, build y evidencia (`d1fb188`)

Entrega: `4891cbf..d1fb188`. Trece commits: código en `bdaf752`, capturas de desarrollo en `f7323f2`, evidencia de entrega en `6322f49`, informe en `d1fb188`.

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| A23 alcance: `git diff 4891cbf..d1fb188 --name-only` | Solo `01_shared_drivers/05_i2c/IMPLEMENTATION_REPORT.md`, `03_firmware_templates/prj_01_template_dualcore/`, `tools/i2c_target_f334/` y adiciones en `tools/flash.py`, `tools/monitor.py`, `tools/ad2.py`. `git diff cc8aa2c -- 02_firmware_projects`: vacío |
| Producción de `prj_01` | `main.c` (CM7, CM4), `app_main.c`, los tres `CMakeLists.txt` y `CMakePresets.json` solo ganan bloques `#if defined(I2C_DRIVER_SELFTEST)`, la opción (OFF), su `FATAL_ERROR` frente a `GPIO_`/`TIM_`/`SPI_DRIVER_SELFTEST` y el preset |
| A16 `.\tools\build_all.ps1 -Clean` en `d1fb188` (mi ejecución) | 8/8. `prj_01` debug CM7 29304/132/55668, CM4 3456/24/12560; release CM7 28356/132/55672, CM4 3112/24/12560: iguales a `2481396` (`docs/tim_f0_map_sizes_2481396.txt`) y al informe §5.2 |
| C9 `selftest-i2c` con `CMAKE_C_FLAGS=-Werror`, directorio vacío (mi ejecución) | Configure y build 0, 0 líneas `warning:`/`error:`; CM7 41972/136/56088, CM4 7056/24/12568, iguales al informe |
| Firmware de la F334, `build.ps1 -Clean -Werror` (mi ejecución) | `Warnings : 0`, exit 0; 10304/20/3392, igual al informe |
| C1–C15 | No aplican en F0: no hay `i2c_*.[ch]` ni targets `_i2c_*` (correcto según la tabla de fases) |

### A27: seguridad eléctrica (lectura)

| Punto | Dónde | Resultado |
| --- | --- | --- |
| S15 en la F334 | `tools/i2c_target_f334/src/target.c:40-62` | Toda escritura a PB8/PB9 pasa por `pin_input`, `pin_af_od` y `pin_out_od_released`. `OTYPER = 1` y `PUPDR = 0` antes de `MODER` = AF; para salida, `ODR = 1` (BSRR) antes de `MODER = 01`. `target_pins_safe()` es lo primero de `main` y lo llaman los handlers de fallo. `target_poll()` revisa S15 en cada vuelta y aborta a entrada. El banner y `LVL` leen los registros reales |
| S21 en la F334 | `target.c:620-675`, `main.c:116-123` | `HOLD` solo por comando, 1..500 ms; liberación por TIM2 CC1 (1 MHz), por `EDGES`, por el aborto S15, por los handlers de fallo y por el IWDG. La liberación pone `ODR = 1` antes de pasar a entrada. Ver m1–m3 |
| S15/S16 en la H755 | `Common/Src/i2c_selftest_common.c:47-94`, `CM7/App/Src/i2c_selftest.c`, `CM4/App/Src/i2c_selftest.c` | Pull rechazado primero; push-pull rechazado en todo modo que no sea entrada; AF solo para el núcleo del paso (AF4 en CM4, AF6 en CM7); PF14/PF15 solo entrada sin pull; el resto de pines, solo entrada. Antes de un AF, cada núcleo lee su instancia: reloj en su banco RCC y `PE = 0` (lección R2-3). Escrituras con `GPIO_BM_ConfigAlternateEx` (`OTYPER`, `PUPDR`, `AFR` y `MODER` al final, bajo PRIMASK y el HSEM) |
| Prohibidos | grep en los dos núcleos | Ni TIM6, ni SysTick, ni `RCC_*RSTR`, `D2CCIP2R` o `D3CCIPR`; DBGMCU solo leído; PE0/PE1, PC13 y PA13/PA14 intactos |
| Esperas | `wait_until()`, `watch_bus()` (CM7 l. 92-127, 793-860; CM4 l. 40-75) | Salida por CYCCNT parado y por tiempo; handshake con `sync_ack_low` (B1) |
| Memoria compartida | `0x3800D000` | No se solapa con `.shared_ram` ni con los bloques de `00_gpio`, `03_tim` y `06_spi` (`0x3800A000`–`0x3800C000`); dentro de `RAM_SHARED`; `_Static_assert` presente; región MPU no cacheable |
| Host S2/S3 | `i2c_hw_check.py` | OE solo por `i2c_set_oe` (HOST_REQ) → `ad2_set_oe`; Digital Out nunca activado y releído en cada frontera de paso; `DwfParamOnClose = 1` |
| Host S18 | `i2c_hw_check.py:259, 1037`; `tools/flash.py:306-310` | Exactamente dos ST-LINK con su SN y su placa; puertos por SN; el reset lleva `sn=`; el host nunca flashea ni resetea la F334. `flash.py --sn` añade `sn=` a toda conexión; sin la opción, el comando es idéntico al de antes |
| Host S17/S19 | `i2c_hw_check.py:107-109, 689-694` | Umbrales del plan; niveles antes de cada paso y tras cada retención; primera adquisición descartada e impresa |

### Evidencia de la entrega

- `docs/i2c_f0_bench_taps_f7323f2_5.txt`: de `argv` a `HOST VERDICT=PASS`, checklist con fecha, `tree clean`, Rev `f7323f2` e ids iguales en CM7, CM4, host y F334; 28/28, `FW VERDICT=PASS failures=0`. Niveles S17 3,156–3,159 V / 3,299–3,304 V; retenciones de 50 ms vistas por la F334, CM7 (PB8/PB9 y PF14/PF15), y el osciloscopio, con la otra línea libre.
- `docs/i2c_f0_bench_notaps_f7323f2_2.txt`: 26/26, PASS; 1+ 0,056–0,059 V (S19), PF14 = 0.
- Las cuatro sesiones abortadas (`notaps_f7323f2`, `taps_f7323f2_2/_3/_4`) y `taps_f7323f2` (PASS con las sondas en el lado del bus) están commiteadas sin editar y explicadas en §5.3/§5.8. En las abortadas nada movió el bus.

Las desviaciones del informe §5.5 amplían el plan sin rebajar nada; las acepto. Sobre la de S17 en notaps, ver m13.

**El informe §5.7 tiene razón sobre mi verificación del banco del 2026-10-06.** Con J2/J3 puestos, mis lecturas (1+ ≈ 3,15 V, 2+ ≈ 3,30 V) son las mismas con las sondas en el lado de PF14/PF15 o en el del bus. La verificación debió incluir quitar J2/J3 y ver 1+ < 0,1 V (S19), que es lo que destapó `notaps_f7323f2`. Lo tomo como lección para las verificaciones de banco: cada sitio de una sonda se comprueba en un estado que lo distinga.

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| M1 | Mayor (para F1; F0 no lo usa) | **NACK en el byte k y stretching por byte en la F334.** `target.c:439-482`: la decisión de ACK/NACK de un byte se toma en `TCR` con `s_rx_nack_this`, que fija `RXNE`. El ISR atiende `RXNE` antes que `TCR` en la misma pasada, pero si una lectura de `ISR` ve `TCR` sin `RXNE` todavía, el NACK se pierde o cae un byte tarde. Tampoco está comprobado SBC + `RELOAD` en transmisión de esclavo (`STRETCH BYTE` en lecturas, `target.c:416`). T8(b) y T10(a) se apoyan en los dos | Antes de usarlos en F1: en `TCR` de recepción, si `RXNE` está pendiente, consumirlo primero (o decidir con `RXNE` y `TCR` juntos); y que T8(b) y T10(a) comprueben `nk`, `rx` y el osciloscopio, no solo el estado del driver |
| m1 | Menor | El IWDG de la F334 (`/32`, `RLR = 499`, `main.c:119`) vence a 533 ms con el LSI mínimo (30 kHz). Es el único límite cuando TIM2 no puede correr (bloqueo, una IRQ en `Default_Handler`), así que esa ruta pasa de los 500 ms de S21. El comentario de `main.c:15-16` ya lo dice | `RLR = 449` (288–480 ms) o ajustar S21 en el README de la F334 |
| m2 | Menor | `Default_Handler` (startup, l. 117) es un bucle vacío: SVC, PendSV, SysTick, DebugMon e IRQ no usadas no sueltan los pines; solo el IWDG acaba una retención desde ahí. Hoy no hay ninguna fuente así activa | Que `Default_Handler` llame a `fault_release()` |
| m3 | Menor | EXTI9_5 y TIM2 tienen la misma prioridad (`main.c:73-75`) y gana EXTI (IRQ 23 frente a 28): un SCL rápido y continuo durante una retención de SDA sin `EDGES` retrasa la liberación por tiempo (el IWDG la acota) | TIM2 con más prioridad que EXTI |
| m4 | Menor | Si `STOPF` de una transferencia y `ADDR` de la siguiente están pendientes en la misma pasada del ISR, se atiende `ADDR` primero (`target.c:398` antes de `:520`): cuenta un Sr falso y luego cierra la transferencia. Necesita más de unos 10 µs de latencia | Atender `STOPF` antes que `ADDR` |
| m5 | Menor | `CORRUPT k` cuenta los bytes escritos en `TXDR`, también los devueltos, en todos los segmentos (`target.c:493, 500`): en R-Sr-R el índice se desplaza uno respecto al "byte k devuelto" del plan | Contar solo bytes enviados, por transferencia, antes de T4 |
| m6 | Menor | Parser de la F334: tokens sobrantes ignorados (`target.c:1031`); `HOLD … EDGES 0` aceptado aunque el uso dice 1..255 (`:628`) | Rechazarlos |
| m7 | Menor | El comentario de `TIMINGR` (`target.c:135-141`) da por cumplido tVD;DAT de Fm+ con t\_r = 120 ns; con `SDADEL = 2` solo vale hasta unos 90 ns (vale sin tomas). Si el PLL no engancha, la F334 sigue a 8 MHz con el mismo `TIMINGR`, y el host no rechaza `sysclk ≠ 64000000` | Corregir el comentario; el host aborta si `sysclk ≠ 64000000` |
| m8 | Menor (evidencia) | La línea "T0B CM7 claim of PB8/PB9 AF6 while CM4 holds them in AF4: refused" (`CM7 i2c_selftest.c:1039-1041`) se rechaza por la regla de dueño del paso (`i2c_selftest_common.c:67-70`), no por el estado real de los pines: pasaría igual si CM4 no hubiera escrito AF4. El rechazo de `ist_s16_pins_ok()` solo está probado sobre imágenes escritas a mano (`:107-131`). Lo mismo el comentario del caso "GPIO OD" (`:621`) | Rotular la línea por lo que prueba y añadir, en un paso de CM7, un control vivo con PB9 realmente en AF4 (I2C1 en `PE = 0`) que la comprobación de pines rechace |
| m9 | Menor (heredado de `06_spi`) | `bench_abort()` (`CM7 i2c_selftest.c:315-324`) no borra el paso publicado: tras un aborto en T0B, CM4 seguiría aceptando reclamos AF4 hasta DONE. Hoy no se explota (CM4 solo atiende la última orden y DONE hace `bench_safe()`) | `publish_step(IST_STEP_NONE)` en `bench_abort()` |
| m10 | Menor | `k_t0b_refuse` tiene 32 casos y llena el `uint32_t` del resultado (`CM7 :590-600, 618-635`): un caso 33 haría `1UL << 32` | `_Static_assert(NCASES(…) <= 32)` en cada tabla |
| m11 | Menor (heredado de `06_spi`) | `main()` solo captura `OSError` y `BenchAbort` (`i2c_hw_check.py:973`). `AD2Error` (al abrir el AD2 o en la adquisición descartada, `:1025-1030`), `subprocess.TimeoutExpired` (`:259`) y Ctrl+C terminan con un traceback, **sin `HOST VERDICT` y sin captura** (se escribe en `:1001`). El AD2 sí queda en S2 | Capturar también esas excepciones y escribir la captura en un `finally` |
| m12 | Menor (evidencia) | Dos comprobaciones contadas no pueden fallar: "T0d identity" se registra con `True` literal (`:1062`, si falla se aborta antes) y "No Digital Out" (`:886-889`, `step()` ya aborta). Las mediciones reales son 26/28 (taps) y 24/26 (notaps) | Rotularlas como "registrada" o no contarlas |
| m13 | Menor | En notaps el host exige 2+ ≥ 3,20 V (`:693`), que el plan no pide: sin J3, 2+ mide PF15 (22 kΩ a 3V3 frente a 1 MΩ, 3,229 V calculados) con 35 mV de margen. Un fallo ahí se informa como "S19 … 1+ reads …" (`:694, 708`) | En notaps, 2+ dentro del divisor esperado (por ejemplo 3,10–3,30 V) y el mensaje con la línea correcta |
| m14 | Menor | `s15_report_ok` (`:188`) se queda con la última coincidencia de cada pin: un informe con PB8 repetido (uno malo, uno bueno) pasa; la regex no tiene límite de palabra | Exigir exactamente una aparición de cada pin |
| m15 | Menor | `--checklist-date` no es obligatoria ni se valida (`:391, 903`): `--checklist-confirmed` sola imprime "on None" | Obligatoria con `--checklist-confirmed`, formato ISO |
| m16 | Menor | `H755Console.expect` no corta con un `ABORT` del firmware, como sí hace `spi_hw_check.py` tras B1 (l. 347-355): un aborto en `step()` cuesta hasta 120 s y se informa como timeout | `stop_on_abort` como en `06_spi` |

**Observaciones** (sin acción en F0):

- **O1. Glitch en el PB8 de la F334 al bajar ella SDA, solo con tomas** (`blank=1` en taps, `blank=0` en notaps; informe §5.8). Encaja con un rebote de masa en la F334 al descargar los 150 pF de la toma de SDA por W3. En F1 la F334 baja SDA (ACK, datos de lectura) con SCL bajo: si ese glitch supera el filtro analógico de 50 ns de su I2C, podría verlo como un flanco de SCL. T4 con CRC en el estado "taps" lo detectaría; que el informe de F1 dé `blank` y cualquier discrepancia de T4/T5 separada por estado del banco.
- **O2.** El mock de S21 (`t0b_s21_selftest`, `:241-254`) prueba `f334_hold_guard` y no `F334.hold()`; por lectura, `hold()` llama al guarda antes de escribir (`:627-630`).
- **O3.** `monitor._usb_serial_instances` (`tools/monitor.py:266-288`) indexa por nombre de puerto: una entrada no presente con el mismo nombre podría tapar la presente. Falla en seguro (el host aborta sin puerto).
- **O4.** El hilo lector del host aún puede morir si falla el `_sink` de respaldo (`:489`); acabaría en un `BenchAbort` por tiempo, nunca en un PASS falso.

### Estado

La parte 1 pasa: ningún hallazgo bloqueante. M1 debe resolverse antes de la primera prueba de F1 que use NACK en el byte k o stretching por byte; m1–m16 pueden corregirse al empezar F1, con su línea en la "Respuesta a la auditoría". **F0 queda pendiente de mi ejecución en la placa (parte 2)**: taps y notaps con imágenes de `d1fb188`, tras la confirmación del checklist por el responsable.

## F0, parte 2: ejecución del auditor en la placa (`85f9d29`)

Checklist físico confirmado por el responsable el 2026-10-07 (banco en taps; después, J2/J3 quitados para notaps). `selftest-i2c` y el firmware de la F334 recompilados en `85f9d29` (`--fresh`, 0 avisos), flasheados con `tools/flash.py --sn` (H755 `0016000C4142501220353451`, Device ID 0x450; F334 `066BFF393731435243254117`, Device ID 0x438; las dos "Download verified successfully") con el script parado. Una ejecución de `i2c_hw_check.py` por estado. Capturas sin editar:

- `03_firmware_templates/prj_01_template_dualcore/docs/i2c_f0_audit_bench_taps_85f9d29.txt`
- `03_firmware_templates/prj_01_template_dualcore/docs/i2c_f0_audit_bench_notaps_85f9d29.txt`

| Resultado | taps | notaps |
| --- | --- | --- |
| Identidad | Rev `85f9d29`; id `76e34b076c5812d8` en CM7, CM4 y host; F334 id `8862f04d55606362` (el del host); `tree clean`; los dos ST-LINK por SN (COM4, COM7) | Igual |
| Firmware | `FW VERDICT=PASS failures=0`; sin aborto | Igual |
| Host | **28/28**, `HOST VERDICT=PASS` | **26/26**, `HOST VERDICT=PASS` |
| Frente a la entrega | Quitando marcas de tiempo, Rev, ids y valores medidos, las líneas de resultado (`-> PASS/FAIL`, veredictos, recuentos) son idénticas a `i2c_f0_bench_taps_f7323f2_5.txt` | Idénticas a `i2c_f0_bench_notaps_f7323f2_2.txt` |
| Niveles en reposo | 1+ 3,102 V, 2+ 3,245 V | 1+ 0,021 V (S19), 2+ 3,237 V; PF14 = 0, PF15 = 1 |
| Retenciones de 50 ms | SDA: F334 50001 µs, `blank=1`; CM7 PB9 50641 µs, PB8 0 caídas; osciloscopio 50,10 ms, mínimo 0,017 V. SCL: F334 50001 µs; CM7 PB8 50632 µs, PB9 0; osciloscopio 50,08 ms | SDA y SCL vistas por la F334 y CM7, PF14/PF15 sin caídas, `blank=0` |
| T0B(d) | MCU_ACK alto 9,890 s, luego bajo 20/20 | Igual en forma |

`blank=1` con tomas y `blank=0` sin ellas se reproduce (O1).

### Observación nueva

- **O5. Los umbrales absolutos de S17 tienen poco margen.** En mi sesión taps los dos niveles salieron un 1,7 % más bajos que en la entrega (1+ 3,102 V frente a 3,157 V; 2+ 3,245 V frente a 3,301 V), con la misma razón SCL/SDA (0,956, el divisor 1 kΩ / 22 kΩ). Minutos después, en notaps, 2+ leyó 3,237 V, como en la entrega (3,235 V). No sé si fue el riel de 3V3 o el AD2 recién conectado. Pasó con 42 mV de margen sobre el mínimo de SCL (3,06 V). Antes de la sesión había previsto que notaps abortaría por m13 suponiendo el riel bajo; no ocurrió, lo que apunta a que el desplazamiento fue de esa sesión y no del riel. **Propuesta para el responsable (D9, revisión 2 del plan):** S17 "con tomas" como razón: SCL/SDA = 0,956 ± 0,015 (detecta el 22 kΩ a GND en el nodo de SCL y un pull-up distinto de 1 kΩ) y SDA ≥ 3,0 V; en notaps, 2+ dentro de 0,97–0,99 del nivel de SDA de taps o simplemente ≥ 3,0 V (m13). La regla de seguridad no cambia: un bus en reposo, alto y con los dos nodos separados.

### Estado

**F0 pasa.** El implementador puede empezar F1. Antes de la primera prueba de F1 que use NACK en el byte k o stretching por byte, M1 debe estar corregido; m1–m16 se contestan en la "Respuesta a la auditoría" de F1, con su commit. O5/D9 queda a decisión del responsable. El banco quedó en notaps (J2/J3 quitados).
