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

## F1, parte 1: revisión del código, build y evidencia (`2802412`)

Entrega: `4abd264..2802412`. Código en `7a22a3f` (y un comentario en `d326575`), imágenes de las capturas de entrega en `e27c673`, evidencia de build en `3a26563`, informe en `2802412`.

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| A23 alcance: `git diff 4abd264..2802412 --name-only` | Solo `05_i2c/` (`i2c_bm.[ch]`, informe; borra `.gitkeep`), `01_shared_drivers/CMakeLists.txt`, `prj_01`, `tools/i2c_target_f334/` y adiciones en `tools/ad2.py`. `AUDIT.md`, el plan y las instrucciones, sin tocar |
| A16 `build_all -Clean` en `2802412` (mi ejecución) | 8/8; `prj_01` debug CM7 29304/132/55668, CM4 3456/24/12560; release CM7 28356/132/55672, CM4 3112/24/12560: iguales a `2481396` |
| C9 `selftest-i2c` con `-Werror`, directorio vacío (mi ejecución) | 0 avisos; CM7 78488/140/130452, CM4 22076/28/78576 (`arm-none-eabi-size`), como el informe |
| C7 / B1 en el `.map` | `i2c_bm.c.obj`: `.data` 0 y `.bss` 0 en CM7 y CM4 |
| F334 `build.ps1 -Clean -Werror` (mi ejecución) | 0 avisos; 11288/20/3392, como el informe |
| Capturas de entrega | `i2c_f1_bench_taps_e27c673.txt` 90/90 (2 `RECORD` sin contar), `i2c_f1_bench_notaps_e27c673.txt` 69/69; `tree clean`, Rev e ids iguales en CM7, CM4, host y F334; checklist con fecha; CLKREF +1,35 % / +1,31 %. Las cuatro de desarrollo, commiteadas sin editar |

Revisé el código con cuatro lecturas independientes (driver; F334; selftest de los dos núcleos; host y decodificador del AD2) y comprobé yo mismo, en el código o en el RM, cada hallazgo mayor y los menores que cito con línea.

### Verificación de los hallazgos de F0

| Hallazgo | Verificación |
| --- | --- |
| M1 | `TCR` de recepción consume su propio byte primero (`target.c:490-493`), con espera acotada y `ev 0x20` si no llega; orden del ISR correcto para W, R, W-Sr-R, R-Sr-R, sondeo, stretching y corrupción. T8(b) con tres vistas y T10(a) con el stretching por byte en lectura visto en el osciloscopio. **Cerrado**, con m17 |
| m1–m7 | IWDG 288–480 ms; `vectors.c` cubre los 49 vectores no usados (con los 10 definidos, los 59 del startup); prioridades TIM2 0, I2C1 1, EXTI 2, USART2 3; `STOPF` antes que `ADDR`; `CORRUPT` por bytes enviados y por transferencia; parser estricto; el host aborta con `sysclk ≠ 64000000`. **Cerrados** (O14 recoge detalles del comentario de `TIMINGR`) |
| m8 | La línea se rotula bien y hay un control con los registros vivos, pero en un paso de CM4 y sobre la función pura: el camino `bench_claim()` → `ist_s16_pins_ok()` sigue sin rechazar nunca por un pin realmente en otro AF. **Abierto** como m18 |
| m9, m10 | `bench_abort()` publica `IST_STEP_NONE`; `_Static_assert(≤ 32)` en las cuatro tablas. **Cerrados** |
| m11–m16 | Captura y veredicto en un `finally`; T0d y "No Digital Out" como `RECORD`; S17 según R2-1 (razón 0,941–0,971 inclusive, 2+ ≥ 3,0 V; notaps 1+ < 0,10 V) con la línea nombrada; una aparición por pin con límite de palabra; `--checklist-date` obligatoria e ISO; `stop_on_abort` en `step()`. **Cerrados** (O13: `s15=ok` sin límite de palabra; las esperas de T0B sin `stop_on_abort`) |
| O1 | `blank` = 1 en las 5 retenciones de SDA con tomas y 0 sin ellas; 0 discrepancias de T4 en los dos estados y ningún `BERR`, `ARLO` ni `OVR`: el glitch no aparece en las transferencias. **Cerrada** |

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| M2 | Mayor (contrato) | **"Bus ocupado → `ERR_BUS`" solo funciona si el periférico vio un START con `PE = 1`.** `BUSY` se pone al detectar un START y se borra con `PE = 0` (50:2897-2899). Si un target retiene SDA desde antes del `Init` (el caso de arranque para el que existe `RecoverBus`), `Init` y `CheckTransfer` ven `BUSY = 0` y devuelven OK (`i2c_bm.c:465, 546`), y `Write` lanza el START: lo esperable es `ERR_ARLO` (el primer bit de la dirección vuelve a 0) o un `ERR_TIMEOUT`, nunca `ERR_BUS`. Un llamador que recupera solo ante `ERR_BUS` no recupera nunca. Lo prometen el encabezado (`i2c_bm.h:86, 151-153`) y el plan (regla 3). T1 y T9 solo retienen SDA con `PE = 1` ya puesto, así que no lo ven | D10 (abajo) y una prueba nueva |
| M3 | Mayor (evidencia) | **El bus va por encima de la frecuencia pedida y el informe no lo dice.** T5 juzga "f\_SCL ≤ la pedida" sobre la frecuencia normalizada por CLKREF (`i2c_hw_check.py:1158-1163`). En bruto: 101 015–101 119 Hz con 100 kHz pedidos y 403 317–404 442 Hz con 400 kHz (`i2c_f1_bench_taps_e27c673.txt`), por encima de lo pedido y del máximo de Sm y de Fm. El informe (§5.3, T5) da solo las cifras normalizadas y §5.5 no lo lista. No es un defecto del driver (su aritmética es correcta para el reloj nominal; el HSE de la placa va +1,3 %), pero es un hecho del bus en esta placa | D11 (abajo) y el dato en bruto en §5.3 y en el README |
| m17 | Menor | El arreglo de M1 en la F334 pierde el NACK armado si `RXNE` se atendió en una pasada anterior del ISR y `TCR` llega después con `RXNE = 0` (`target.c:480-511`): `rx_wait_take` espera 200 µs, ACK y `ev 0x20`. No es silencioso (ninguna captura tiene un `0x20`) | Una marca de "byte abierto" puesta en `rx_take` con SBC y borrada en `TCR`; esperar solo si no hay byte abierto |
| m18 | Menor (evidencia) | m8 a medias: falta un control vivo en un paso de CM7, por `bench_claim()`, con PB9 realmente en AF4 (`CM7 i2c_selftest.c:1053-1070, 367-370`) | El control pedido en F0 |
| m19 | Menor (informe) | T12 hace RMW de `RCC_CR` con `HSION` aunque ya esté a 1 (`CM7 :2802`, `rcc_cr_set`), contra lo que dice §5.5 ("no se escribe"). La restauración (`:2835-2836`) encadena con `&&`: si falla la de `I2C123SEL`, no apaga `HSION` (solo importa si estaba a 0) | Escribir solo si estaba a 0, restaurar cada cosa por separado y corregir §5.5 |
| m20 | Menor | El bit de Fm+ (`SYSCFG_PMCR`) puede quedar puesto tras un aborto (`CM7 :2250-2282, 2996, 3039, 3073`); `bench_safe()` no lo limpia y el valor "off" solo se imprime | Restaurar en toda salida y juzgarlo |
| m21 | Menor (C13 en el selftest) | Esperas sin salida por CYCCNT parado: CLKREF con interrupciones enmascaradas (`CM7 :2036`), `HAL_DMA_PollForTransfer` con el tick de la HAL (`:1590`, contra el encabezado `:38-39`), `CM4 :774, 891, 1018` | Salida por CYCCNT parado en todas |
| m22 | Menor (evidencia) | El control de T2 no puede fallar: solo corre si `g == e` y `e2` difiere de `e` por construcción (`i2c_selftest_common.c:508-513`) | Pasar una tabla esperada alterada por el mismo bucle y exigir `bad > 0` |
| m23 | Menor (evidencia) | El control de T3 "SCLL + 1" tampoco puede fallar: `altered = t + 1; good = altered != t` (`i2c_hw_check.py:825-829`) | Pasar el esperado alterado por la comparación real de T3 |
| m24 | Menor | T14 solo se juzga tras T4/T5 (`CM7 :2290-2292`); los demás `Deinit` descartan su resultado; §5.5 no lo lista | Juzgar el `Deinit` de cada prueba, como dice el plan |
| m25 | Menor | T5: la cota superior de tLOW/tHIGH se aplica solo a la mediana (`:1148-1157`); la ventana es [contador − tol, contador + sync + tol] (unos 1,19–2,03 µs a 400 kHz) con el filtro y DNF fijos en el cálculo; osciloscopio a 12,5/25 MS/s en vez de 20/50 MS/s (`CM7 :2185`). Nada de esto está en §5.5 | Cada periodo contra la ventana del plan (± 2 muestras + 2 ciclos de kernel + N1) con el filtro real; las velocidades del plan o la desviación listada |
| m26 | Menor | El host no tiene la lista de transferencias y capturas esperadas (`:1459-1516`) ni comprueba al final que no queden `scope_pending`/`hold_pending` (`:1810-1834`): un firmware que se saltara un tamaño de T4 o una captura de T5 daría PASS | Lista esperada por paso y comprobación final |
| m27 | Menor | "1 MHz solo sin tomas" lo impone solo el firmware: el host no rechaza una línea X con `f=1000000` ni `FMP 1` en una sesión taps (`:1676, 1822-1825`) | Que el host lo rechace también (S19) |
| m28 | Menor (contrato) | El redondeo aprobado de N ("ceil en los dos") no está en el encabezado: `i2c_bm.h:46-47, 109, 123-127` remiten a las reglas del plan, que tienen la división hacia abajo | Encabezado y plan (revisión 3) |
| m29 | Menor | Con el timeout apagado, `Init` escribe `TIMEOUTR = 0` una sola vez (`i2c_bm.c:446-451`); si `TIMOUTEN` estaba a 1 (sin `Deinit` previo), `TIMEOUTA` puede sobrevivir (50:2861) e `Init` sale con `ERR_VERIFY` y `PE = 1` | Doble escritura siempre, como `Deinit` (`:478-479`) |
| m30 | Menor (contrato) | Códigos de retorno sin documentar: `RecoverBus` `ERR_TIMEOUT`; `Write`/`Read`/`WriteRead` `ERR_ARLO`, `ERR_BUS` por TIMEOUT (`:693`) o por cuenta incompleta en `STOPF` (`:755`), `ERR_TIMEOUT` sin CYCCNT (`:654`); `Init` `ERR_VERIFY`; los `x->status` finales de `XferIrq`. `done_tx`, "bytes reconocidos" (`h:183`), cuenta bytes escritos en `TXDR` tras ARLO o TIMEOUT | Completar el encabezado antes de congelar la API |
| m31 | Menor | `DmaStartWrite`/`DmaStartRead` releen `TXDMAEN`/`RXDMAEN` después de poner `START` (`:1012-1013, 1033-1034`): un `ERR_VERIFY` deja la transferencia en marcha sin forma documentada de pararla | Releer antes de escribir `CR2` con `START` |
| m32 | Menor (contrato) | `XferIrq` corre desde los vectores EV y ER (`h:191`) sin exigir la misma prioridad: si ER interrumpe a EV a mitad, los dos tocan `x` y `CR1`/`CR2` | Documentar "misma prioridad, sin anidar" (el selftest usa 6 en los dos) |

**Observaciones** (sin acción obligatoria en F1; van al README donde aplique):

- **O6.** `RecoverBus` termina con START + STOP (`i2c_bm.c:1206-1209`), no con un STOP solo; UM10204 lo llama formato ilegal, aunque la mayoría de los dispositivos lo aceptan (funcionó en T9). Un target que estire SCL tras soltarla da `ERR_BUS` (`:1197-1199`).
- **O7.** El PRIMASK de `SYSCFG_PMCR` no protege frente al otro núcleo (por ejemplo `EPIS` de `08_eth` en CM7): aceptado por C10, va al README.
- **O8.** Las macros públicas `I2C_BM_ERRF_*` (`h:236-239`) no están en el contrato del plan; quedan congeladas con la API (revisión 3).
- **O9.** No hay función para abandonar una transferencia por interrupción: `Init` da `ERR_BUSY` (`running()`, `:369`) y solo `Deinit` la recupera. Al encabezado y al README.
- **O10.** Citas del RM desplazadas: `c:62` 50:2775 → 50:2782; `c:74` 50:2862 → 50:2858; `c:69` 50:2867 → 50:2866; `c:425` 50:2652 → 50:2645; `c:446`, `h:149` (y el plan) 50:2845 → 50:2861.
- **O11.** El contador de glitches del decodificador usa 3 muestras, no un tiempo (`ad2.py:420-425`): a 4 MS/s son 750 ns, más que el tHIGH de 400 kHz, y cada pulso alto cuenta (45 = 5 bytes × 9 relojes). El bus no tiene glitches. El umbral de 0,5 V de T5 no es un control (decodifica igual con flancos limpios); el control que discrimina es el intercambio de canales, que sí se juzga.
- **O12.** tSU;DAT se atribuye por el papel del bit, no por quién movió la línea (`ad2.py:479, 492`): en un traspaso, el último flanco de SDA puede ser del otro lado.
- **O13.** Host: `s15=ok` sin límite de palabra (`:233`); sin CLKREF, T5 y T10(b) usan razón 1,0 en silencio (`:1744`); el `TIMINGR` de T5 sale de la línea del firmware y no se recalcula con `calc_timing()` del host; las esperas de T0B (`:1392, 1399`) sin `stop_on_abort`. T3 no es textualmente independiente del driver (mismos nombres y forma), pero una reimplementación exacta desde el texto del plan da 0 diferencias en 115 200 vectores.
- **O14.** F334: el comentario de `TIMINGR` (`target.c:154-158`) da 94 ns de retardo de SDA donde el RM da 62,5 ns (50:309) y llama "Sm" al t\_r del banco; ventanas de comprobar-y-actuar sobre `s_active` en `FMP`, `hold_start` e `INIT` (`:705-709, 1205-1214, 1278-1293`); TIM2 (prioridad 0) puede interrumpir a EXTI dentro de `hold_release` y sobrescribir `cause` (`:637-654`); `ln_pins` llama a `target_s15_ok()` sin enmascarar (`:130`); `bench.h:31` dice "v2" con el valor 3. Con `FMP 0`, la F334 alarga cada SCL bajo unos 0,8–1,2 µs (50:345-348): nunca debe llevar tráfico de 1 MHz.
- **O15.** Selftest: T1 comprueba `cases ≤ IST_T1_MAX` y no `== 62` (`CM7 :1806, 1818`; `t1_busy` no exige 8); el reposo de 120 s de CM4 ejecutaría `bench_safe()` sobre GPIOB a mitad de un paso de CM7 (margen actual unos 2,4×); un resultado tardío de CM4 tras un timeout de 30 s puede solaparse con la orden siguiente; la guarda de 1 MHz solo lee PF14 (J3 olvidado en notaps no se detecta: hueco del plan); comentarios obsoletos (`CM7/CMakeLists.txt` "for T13", `CM7 :397` "F0 selftest", `k_exp_dummy` `:1816`).

### Decisiones (para el responsable)

1. **Aprobaciones del 2026-10-07 que cita el informe (§F1 5.5):** N con "ceil en los dos"; `WaitEnd(i2c, n, max_cycles)`; 2.19.1 solo en `CalcTiming`; `HSION` solo en T12; T10(b) sin STOP, con reset por software ("Accept and deliver"). Si el responsable las confirma, van a la revisión 3 del plan (con O8 y m28).
2. **D10 (M2).** Recomiendo: el contrato dice que `Init` y las transferencias solo ven un bus ocupado si el periférico vio el START con `PE = 1`; antes del primer `Init` tras un arranque, el llamador comprueba SCL y SDA con las funciones de `I2cBm_Recovery_t` (o llama a `RecoverBus`, que ya lee SDA) y, en un bus de un solo maestro, `ERR_ARLO` se documenta como síntoma de bus colgado. Prueba nueva en F1: la F334 retiene SDA **antes** del `Init` → `Init` OK y `Write` termina acotada con el código que dé el silicio (registrado); después `RecoverBus` y una transferencia correcta. Alternativa: que `Init` reciba las funciones de lectura y devuelva `ERR_BUS` él mismo (cambia la firma).
3. **D11 (M3).** Recomiendo: T5 sigue juzgando "≤ la pedida" sobre la frecuencia normalizada (la aritmética del driver), pero imprime el valor en bruto como registro, y el informe y el README dicen que en esta placa el bus va +1,3 % por encima de lo pedido (101 kHz con 100 kHz pedidos), con la recomendación de pedir con margen en un producto. Alternativa: juzgar en bruto, lo que en esta placa haría fallar T5 siempre.

### Estado

La parte 1 no tiene bloqueantes, pero **F1 no puede pasar todavía**: M2 y M3 necesitan las decisiones D10 y D11 y su corrección antes de congelar la API de `i2c_bm`; m17–m32 se contestan en la "Respuesta a la auditoría de F1". Mi ejecución en la placa (parte 2, taps y notaps) queda para después de las correcciones.

## F1, parte 2: verificación de la corrección (`d570281`)

Entrega: `063c299..d570281` (código en `daf1bbe`, `9b6db50`, `a5e1135`, `5e44044`, `3d0ad6d`, `9c07b93`; imágenes de entrega en `9c07b93`; evidencia de build en `89847dc`; informe en `d570281`, sección "F1, corrección (revisión 3)").

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| A23 alcance | Solo `i2c_bm.[ch]`, el informe, `prj_01`, `tools/i2c_target_f334/` y adiciones en `tools/ad2.py`. `AUDIT.md`, el plan y las instrucciones, sin tocar |
| `build_all -Clean` en `d570281` (mi ejecución) | 8/8; `prj_01` igual a `2481396` en las cuatro imágenes |
| `selftest-i2c` con `-Werror` (mi ejecución) | 0 avisos; CM7 80644/140/130452, CM4 22284/28/78576; `i2c_bm.c.obj` con `.data = .bss = 0` en los dos núcleos |
| F334 `build.ps1 -Clean -Werror` (mi ejecución) | 0 avisos; 11340/20/3392, como el informe |
| Capturas de entrega | `i2c_f1_bench_taps_9c07b93.txt` 95/95 (22 `RECORD`: 20 de f\_SCL en bruto, T0d y "No Digital Out") y `i2c_f1_bench_notaps_9c07b93.txt` 73/73; `tree clean`; T9(b): `Init` OK con `BUSY = 0`, `Write` → `ERR_ARLO` en 4052 ciclos, `RecoverBus` 4 pulsos, escritura correcta después. Las fallidas (`taps_5e44044`, `taps_3d0ad6d`) y la de desarrollo, commiteadas sin editar y explicadas en §5.8 |

Revisé el diff con dos lecturas independientes (driver y F334; selftest y host, con las funciones puras del host ejecutadas sobre datos sintéticos) y comprobé yo mismo en el código el hallazgo nuevo del driver.

### Verificación de los hallazgos de F1

| Hallazgo | Verificación |
| --- | --- |
| M2 / R3-6 | El encabezado lo dice en general (`i2c_bm.h:52-63`) y en `Init`, `CheckTransfer` y los códigos de las transferencias. T9(b): la retención empieza antes del `Init` (`CM7 :3149-3155`), `BUSY = 0` se juzga y el código se juzga en {`ERR_ARLO`, `ERR_BUS`, `ERR_TIMEOUT`}. **Cerrado** (m34 sobre los pulsos) |
| M3 / R3-7 | Una línea `RECORD` con f\_SCL en bruto por captura de T5 (`i2c_hw_check.py:1755`); §5.3 da las dos cifras, +0,8 a +1,1 % sobre lo pedido. El informe corrige con razón la cifra de R3-7 (+1,3 % es el reloj, no el bus). **Cerrado** |
| m17 | `s_rx_open` en `rx_take` con SBC, borrado en `TCR`, `ADDR` y al empezar; los dos órdenes de `RXNE`/`TCR` deciden bien el NACK y no hay carrera nueva (con SBC la F334 retiene SCL hasta reescribir `NBYTES`). **Cerrado** |
| m18 | En T0B_CM7 (dueño CM7) el rechazo sale de `ist_s16_pins_ok()` sobre `MODER`/`AFR` vivos, y el mismo reclamo se acepta con PB9 de vuelta en entrada. La inyección es segura (I2C1 con reloj y `PE = 0`, open-drain sin pull, PRIMASK, restauración comprobada). **Cerrado** |
| m19, m20, m22, m23, m26 | `HSION` solo si estaba a 0 y restauraciones separadas con read-back; Fm+ limpiado en `bench_safe()` y juzgado en los tres pasos del bus; el control de T2 exige `control == cases`; el de T3 pasa por `t3_judge` real (`bad == ok_n`); listas esperadas construidas de constantes del plan y comprobación final de pendientes. **Cerrados** |
| m21 | Salida por CYCCNT parado en CLKREF, en el fin de la BDMA y en CM4. Queda el bucle interno de `HAL_DMA_Abort` con el tick (riesgo despreciable: `EN` baja enseguida). **Cerrado** |
| m24 | `deinit_judged()` en cada prueba; quedan sin juzgar los `Deinit` de medida o desmontaje (T13, B1, DONE de CM4), que no son pruebas. **Cerrado** |
| m25 | Cada tLOW contra la cota inferior y cada tHIGH y cada tLOW no exento contra las dos; la exención es por posición (ACK de la dirección, bit 0 de cada byte de datos, el bajo antes de STOP/Sr): en una trama 1+1 quedan 32 de 38 periodos juzgados enteros, así que un error sistemático de SCLL no se esconde. Las velocidades de muestreo, listadas en §5.5. **Cerrado** (O16) |
| m27 | Tres puntos de aborto en el host (X, SCOPE > 400 kHz, `FMP 1`). **Cerrado en la lógica**; sin mock (m35) |
| m28–m32, O8–O10, R3-1/2/3/5/8/9 | N con techo en el encabezado y en el código; `TIMEOUTR` escrito tres veces con `PE = 0` (50:2861) y el read-back intacto; todos los códigos de retorno documentados (cada `return` comprobado); read-back de `TXDMAEN`/`RXDMAEN` antes de `START` con la precarga de 2.19.6 intacta; "misma prioridad EV/ER"; macros `ERRF` como API; abandono solo con `Deinit`; citas corregidas y comprobadas en el RM. **Cerrados** |
| O13, O14, O15 | `s15=ok` con límite de palabra; sin CLKREF, T5 y T10(b) fallan; `TIMINGR` de T5 recalculado en el host; F334 con comprobar-y-actuar enmascarado y comentarios corregidos; T1 con 62 y 8 casos exactos. **Cerradas** (lo diferido, para el README) |

### Hallazgos nuevos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| m33 | Menor (driver) | `WaitEnd` con CYCCNT parado al entrar devuelve `ERR_TIMEOUT` **sin** reset por software (`i2c_bm.c:1059-1061`), contra su encabezado ("a stopped CYCCNT … the instance is software-reset", `i2c_bm.h:271-274`). Ahí `DmaStart*` ya lanzó el START: la transferencia DMA sigue viva, y `DmaEnd` limpia `CR2` y los bits DMA sin `PE = 0`, así que el maestro puede quedarse con el bus. (En `Write`/`Read` no hay nada en marcha y no importa) | `soft_reset` en ese camino, como dice el encabezado |
| m34 | Menor (evidencia) | T9(b) juzga solo `pulses ≤ 5` (`CM7 :3192`), que acepta de 0 a 5; el "4" del informe no se juzga ni se deriva, y el nivel de SDA tras el `Write` (`lv0`) se imprime sin juzgar. Un `RecoverBus` que diera pulsos de más pasaría T9(b) (lo atraparía solo T9) | Juzgar `pulses ≥ 1` si SDA seguía abajo tras el `Write`, y pulsos + flancos del `Write` = 5 contra la cuenta de la F334; o redactar el informe como "≤ 5" |
| m35 | Menor (evidencia) | Los abortos de m27 no tienen mock (§5.8 lo reconoce) | Un caso por punto en T0B(a) |

**Observaciones:**

- **O16.** La exención de m25 es más amplia de lo necesario: incluye el bajo propio del maestro antes de STOP/Sr y el ACK de una dirección rechazada (NACK), donde ningún target estira. Basta con las posiciones con target y ACK.
- **O17.** `RecoverBus` como "la comprobación antes del primer `Init`" (`i2c_bm.h:302`): devuelve OK con SDA alto sin leer SCL, así que una SCL retenida sola no se ve. El encabezado debe decir que SCL se lee con `scl_read`.
- **O18.** "Se borra con `PE = 0`" está en 50:2900, no en 50:2897-2899 (encabezado y R3-6).
- **O19.** El límite efectivo de T9(b) es 2 ms + 1 ms (`CM7 :3190`), no los 2 ms de §5.5.
- **O20.** La inyección de m18 hace el RMW de GPIOB sin `GPIO_BM_SectionEnter` (a diferencia de `bench_safe()`); una escritura simultánea de CM4 podría cruzarse, sin efecto eléctrico.
- **O21.** `hold_start` de la F334 usa `__disable_irq`/`__enable_irq` en vez de guardar PRIMASK; correcto porque solo se llama desde el bucle principal.

### Estado

Todos los hallazgos mayores de F1 (M2, M3) y los menores m17–m32 están cerrados. Quedan tres menores nuevos: m33 (un camino del driver que deja una transferencia DMA viva) y dos de evidencia (m34, m35). **F1 pasa cuando:** (1) m33 esté corregido (cambia el código del driver, no la firma), con m34 y m35 y las observaciones baratas (O17, O18, O19) en la misma entrega; (2) yo haya hecho mi ejecución en la placa, taps y notaps, sobre ese código. Al pasar, la API de `i2c_bm` queda congelada.

## F1, parte 3: corrección 2 y ejecución del auditor en la placa (`cd8a0f4`)

Entrega: `be202f4..cd8a0f4` (código en `6fa2439`, `3d92f3e`, `fefacf4`; capturas de entrega en `e66f8ba`, imágenes de `fefacf4`; evidencia de build en `90aef10`; informe en `cd8a0f4`).

### Revisión

| Hallazgo | Verificación |
| --- | --- |
| m33 | `WaitEnd` hace `soft_reset` con CYCCNT parado al entrar (`i2c_bm.c:1061`); el encabezado dice además que un `ERR_PARAM` no escribe nada y que una transferencia empezada se termina con `Deinit`. **Cerrado** |
| m34 | El firmware juzga los pulsos según el nivel de SDA tras el `Write` (SDA bajo: 1..5; alto: 0) y que SCL quedó alta; el host juzga pulsos + flancos de SCL del `Write` = `EDGES` de la F334 (con tomas, medidos en el osciloscopio: 1 + 4). **Cerrado** |
| m35 | Mocks en T0B(a) de los tres abortos S19 del host (X, SCOPE, `FMP 1`), por el código de la sesión. **Cerrado** |
| O16–O20 | Exención de T5 solo donde un target que reconoció su dirección puede estirar; encabezado de `RecoverBus` y del chequeo previo al `Init` con `scl_read`; cita 50:2897-2900; T9(b) acotado a 2 ms + 1 ms; `GPIO_BM_SectionEnter` en la inyección de m18. **Cerradas** |
| `fefacf4` (T5) | Los mínimos del modo se juzgan con 2 muestras de tolerancia (80 ns a 25 MS/s); un valor por debajo del mínimo pero dentro de ella es un `RECORD`. **Confirmado por el responsable el 2026-10-08.** La causa: tHIGH mínima de 599–600 ns a 400 kHz frente a 600 ns (SCLH 630 ns nominales, 622 ns con el reloj de la placa): el margen de tHIGH a 400 kHz en esta placa es de un 3 % |

Builds (mi ejecución, `cd8a0f4`): `build_all -Clean` 8/8, `prj_01` igual a `2481396`; `selftest-i2c` con `-Werror` 0 avisos (CM7 80712/140/130452, CM4 22284/28/78576); F334 con `-Werror` 0 avisos (11340/20/3392). Capturas de entrega `i2c_f1_bench_taps_fefacf4.txt` 96/96 y `..._notaps_fefacf4.txt` 74/74; las de `3d92f3e` (taps 94/96, notaps 74/74), commiteadas y explicadas.

### Mi ejecución

Checklist confirmado por el responsable el 2026-10-08 (banco en notaps; después J2/J3 puestos para taps). Imágenes de `cd8a0f4` en las dos placas (`tools/flash.py --sn`, "Download verified successfully" en la H755, 0x450, y en la F334, 0x438). Capturas sin editar:

- `03_firmware_templates/prj_01_template_dualcore/docs/i2c_f1_audit_bench_notaps_cd8a0f4.txt`: **74/74, `HOST VERDICT=PASS`**, `FW VERDICT=PASS`. Las líneas de resultado, iguales a las de `notaps_fefacf4` (solo cambia el orden de dos líneas de log). T9(b): `ERR_ARLO` en 1819 ciclos, `RecoverBus` 4 pulsos, `EDGES` 5 en la F334.
- `03_firmware_templates/prj_01_template_dualcore/docs/i2c_f1_audit_bench_taps_cd8a0f4.txt`: **95/96, `HOST VERDICT=FAIL`**, `FW VERDICT=PASS`. Falla "F1 T10(b) scope: SCL held, TIMEOUT, bus free after" en CM7 (I2C4); el de CM4 pasa.

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| B1 | Bloqueante (una prueba no se reproduce en mi ejecución) | **T10(b) del host depende de cuándo empieza el `Write`.** El host exige que SDA se suelte a TIMEOUTA contado desde la bajada de SCL de la F334. Eso solo vale si el `Write` de la H755 empieza antes de que venza el primer TIMEOUTA. En mi captura (CM7) la F334 bajó SCL en 0 ms, el `Write` empezó (SDA baja) a **11,234 ms**, después del primer vencimiento (9,901 ms), y SDA se soltó a **19,793 ms ≈ 2 × TIMEOUTA**. En la entrega el `Write` empezó a 4,5–5,1 ms y SDA se soltó a 9,885 ms. El driver hizo lo correcto: el propio firmware lo juzga (8666 µs desde que vio SCL baja, ≤ 11 014) y pasa. Lo que cambia es la latencia del intercambio del `AT HOLD` por USB: la entrega pasó por el orden de llegada, no por diseño (como B1 de `06_spi`) | Que el criterio no dependa de esa latencia: o el firmware garantiza que el `Write` empieza antes de que venza el primer TIMEOUTA (y el host lo comprueba con el flanco de SDA), o el host juzga la liberación en el primer múltiplo de TIMEOUTA posterior al START de la H755 (`n = ceil(SDAf / TIMEOUTA)`, registrado) con su tolerancia. Nueva ejecución de entrega en taps, y repetiré la mía |
| O22 | Hecho de silicio | **Con SCL retenida abajo, TIMEOUT vuelve a dispararse cada TIMEOUTA.** El primer vencimiento (9,9 ms, sin transferencia en curso) no terminó nada; el siguiente, a 2 × TIMEOUTA, terminó el `Write`. RM0399 no lo dice (50:2275-2289). Va al README con su prueba en una captura de entrega (lección E2): la corrección de B1 debe dejarlo visible (por ejemplo, registrando `n`) | README |

### Estado

**F1 no pasa** por B1. Lo demás está cerrado: M2, M3, m17–m35 y las observaciones aplicables. Cuando B1 esté corregido y haya una nueva sesión de entrega en taps, repetiré mi sesión taps (la notaps ya pasó sobre `cd8a0f4`; si la corrección toca el firmware o el driver, repetiré las dos).

## F1, parte 4: corrección de B1 y ejecución del auditor en la placa (`a6de15b`)

Entrega: `d88f579..a6de15b` (código en `26c2b4f`, solo el selftest de CM7 y el host; capturas de entrega en `5648533`; evidencia de build en `6360dd5`; informe en `a6de15b`). `i2c_bm.[ch]` y el firmware de la F334, sin cambios.

### Revisión

| Hallazgo | Verificación |
| --- | --- |
| B1 | CM7 cronometra la bajada de SCL durante el propio intercambio del `AT HOLD` (`scl_watch_*`, sondeado en el bucle del handshake) y juzga el fin en `n × TIMEOUTA` contado desde ella, con `n` el primer múltiplo posterior a su propio START. El host juzga en el osciloscopio la liberación de SDA en `n × TIMEOUTA` con `n = ceil(SDAf / TIMEOUTA)` (± 5 %), y con `n ≥ 2` exige que no haya ningún flanco de SDA entre la bajada de SCL y el START (el primer vencimiento no terminó nada). Mock con cinco casos en T0B(a). El criterio ya no depende de la latencia del intercambio. **Cerrado** |
| O22 | Demostrado en una captura de entrega a propósito: CM7 empieza su `Write` a 1,5 × TIMEOUTA (`nwant = 2`). `taps_26c2b4f`: CM4 `n = 1` (START a 5,873 ms, liberación a 9,911 ms), CM7 `n = 2` (START a 14,869 ms, liberación a 19,821 ms, sin flancos antes del START). **Cerrada**; va al README como hecho de silicio con esta captura |

Builds (mi ejecución, `a6de15b`): `build_all -Clean` 8/8, `prj_01` igual a `2481396`; `selftest-i2c` con `-Werror` 0 avisos (CM7 81236/140/130460, CM4 22284/28/78576); F334 con `-Werror` 0 avisos (11340/20/3392). Capturas de entrega `i2c_f1_bench_taps_26c2b4f.txt` 96/96 y `..._notaps_26c2b4f.txt` 74/74.

### Mi ejecución

Checklist del responsable vigente (2026-10-08): banco en taps (J2/J3 puestos), después J2/J3 quitados para notaps. Imágenes de `a6de15b` en las dos placas (`tools/flash.py --sn`; H755 0x450 y F334 0x438, "Download verified successfully"). Capturas sin editar:

- `03_firmware_templates/prj_01_template_dualcore/docs/i2c_f1_audit_bench_taps_a6de15b.txt`: **96/96, `HOST VERDICT=PASS`**, `FW VERDICT=PASS`. T10(b): CM4 `n = 1` (START 5,651 ms, liberación 9,892 ms), CM7 `n = 2` (START 14,829 ms, liberación 19,770 ms, el primer vencimiento sin efecto). Un tHIGH mínimo de 599 ns a 400 kHz, registrado dentro de la tolerancia de 2 muestras (en la entrega, cinco de 592–600 ns).
- `03_firmware_templates/prj_01_template_dualcore/docs/i2c_f1_audit_bench_notaps_a6de15b.txt`: **74/74, `HOST VERDICT=PASS`**, `FW VERDICT=PASS`. T10(b) CM7: START a 15 022 µs, `n = 2`, fin a 20 030 µs.

En las dos, las líneas de resultado (`-> PASS/FAIL`, veredictos, recuentos), ordenadas y con los números normalizados, son idénticas a las de la entrega.

**Observación O23.** `nwant = 2` en CM7 falla si el intercambio del `AT HOLD` tardara más de 2 × TIMEOUTA (19,8 ms): el `Write` empezaría tarde y daría `n = 3`. El máximo visto es 11,2 ms (`i2c_f1_audit_bench_taps_cd8a0f4.txt`). Si llegara a pasar, la salida es juzgar `n ≥ 2` en CM7.

### Estado

**F1 pasa.** Están cerrados M2, M3, m17–m35, B1 y las observaciones aplicables; O6, O7, O9, O11, O12, O15 (lo diferido), O22 y O23, y el margen de tHIGH a 400 kHz en esta placa (~3 %) y el bus +0,8 a +1,1 % por encima de lo pedido (R3-7), van al README. **La API de `i2c_bm` queda congelada en `a6de15b`**: el contrato es `01_shared_drivers/05_i2c/i2c_bm.h` de ese commit (plan revisión 3, C2). El implementador puede empezar F2 (`i2c_ll`). El banco quedó en notaps (J2/J3 quitados).
