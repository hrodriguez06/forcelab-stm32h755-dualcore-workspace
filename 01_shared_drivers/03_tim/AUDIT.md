# Auditoría de 03\_tim

Lo escribe solo el auditor (plan, "Protocolo de auditoría"). El implementador contesta en `IMPLEMENTATION_REPORT.md`.

## F0, parte 1: revisión estática y de build (2026-10-01, en `b744228`)

Rango auditado: `21c0683..b744228` en `driver/tim`. El código es de `d69508e`; los commits posteriores solo añaden la captura, la compilación de `selftest-tim` y el informe (`git diff --stat d69508e b744228`: 3 archivos de documentación).

**Estado: F0 no pasa todavía.** No hay ningún hallazgo bloqueante en esta parte. Falta A18: el gate de F0 exige T0b en verde en la ejecución del auditor, con el AD2, antes de cualquier prueba que mueva el bus o use Digital Out. Ver "Pendiente".

### Lo que ejecuté

| Qué | Resultado |
| --- | --- |
| `.\tools\build_all.ps1 -Clean` | 8/8 PASS. Tamaños de `prj_01` debug/release iguales a los de la entrega |
| `e7542dc` en un worktree propio (`build/wtb`, ruta corta: con la ruta del scratchpad, el `try_compile` de CMake falla en Windows), `prj_01` debug y release con `-DCUBE_DIR` del checkout principal | `arm-none-eabi-size` igual en los cuatro ELF: CM7 debug 29144/132/55668, CM4 debug 3456/24/12560, CM7 release 28228/132/55672 y CM4 release 3112/24/12560 (text/data/bss). La producción no cambia (A16) |
| `selftest-tim` desde un directorio vacío, en `b744228` | Configura y compila con 0 líneas `warning:` |
| `git diff e7542dc --stat`, y filtrado fuera de lo permitido | Nada fuera de `01_shared_drivers/03_tim/`, `03_firmware_templates/prj_01_template_dualcore/` y `tools/ad2.py`. `02_firmware_projects/` sin cambios (A23) |
| Lectura de `tools/ad2.py` (diff completo), `tim_hw_check.py` (Session, S5, T0b, TREF), `tim_selftest_common.c`, `bench_safe`/`bench_claim`/`tim1_s12_ok` de CM7, CM4 completo, y la integración CMake/preset/`main.c`/`app_main.c` | Ver la tabla A |
| `docs/tim_f0_bench_d69508e.txt` | `Rev` d69508e e id `463776aebcff76c0` iguales en CM7, CM4 y el host, árbol limpio, TREF al principio, 14/14, `FW VERDICT=PASS`, `HOST VERDICT=PASS` |
| DS12923, tabla 13 | VOS1: fCPU1 ≤ 400 MHz y fCPU2 ≤ 200 MHz (para la nota N1) |

### Tabla A (puntos que aplican a F0)

F0 no tiene código del driver, así que A2–A15, A24 y A25 no aplican.

| # | Resultado | Evidencia |
| --- | --- | --- |
| A1 | Pasa | Reglas 1–4: sin código de catálogo nuevo; los archivos del selftest siguen el patrón de `00_gpio` |
| A16 | Pasa | 8/8 en mi ejecución; producción de `prj_01` idéntica a `e7542dc` en mi propia base |
| A17 | Pasa | Mis tamaños coinciden con `docs/tim_f0_map_sizes_456a7a7.txt` y con el informe §5.2 |
| A18 | **Pendiente** | No he ejecutado el banco (ver "Pendiente") |
| A19 | Pasa (sobre la captura de la entrega) | Cada control imprime su valor y falla como debe: T0a `0x0200`; TREF 0 flancos con PA8 en AF1; T0b(c) `False` ×2 con `0x5A`; T0b(f) `Idle = Low` → `0xFD`; T0b(g) la configuración correcta se acepta y 13 incorrectas no |
| A20 | Pasa | Los oráculos son el AD2, `IDR`, lecturas directas de registros, el mock de la DLL y `HAL_GetTick` frente al reloj del host. Ninguno es código bajo prueba |
| A21 | Pasa | Captura de `d69508e` con `Rev`/id limpios y coincidentes, marcas de tiempo, `[host] argv`/`options` y TREF |
| A22 | Pasa (estático) | TIM6 no aparece en ninguna escritura (el único "TIM6" es un comentario, `CM7/App/Src/tim_selftest.c:970`); `RCC_CFGR` guardado y restaurado con read-back (`0x0000001B` antes y después); `DBGMCU` solo se lee (`IDCODE`); TIM1 vuelve a reset y su reloj al estado previo en T0b(g); `vTaskSuspendAll` es compatible con `App_Log` (solo toma el mutex con el planificador en `RUNNING`) |
| A23 | Pasa | Ver "Lo que ejecuté" |
| A26 | Pasa, con m3 | Comentarios fieles al código salvo m3 |
| A27 | Pasa (estático); falta mi ejecución | `FDwfDigitalIOOutputEnableSet`: una llamada, dentro de `ad2_set_oe`. `FDwfDigitalOutEnableSet`/`IdleSet`/`OutputSet`/`Configure`: solo en `ad2_set_dout` (guarda S11) y en `ad2_dout_safe` (S2); el resto son el mock de `tim_hw_check.py`. La guarda valida máscara, paso, idle y OE **antes** de cualquier llamada que habilite. S2 incluye Digital Out con read-back en `open`, `set_safe_state`, cada frontera de paso y cada rechazo. `DwfParamOnClose = 4 → 1` (`dwf.h:328`). Escrituras de pines: toda salida o AF pasa por `pin_cfg` → `bench_claim` → `tst_line_rule`, cuyas máscaras coinciden con la tabla de líneas del plan (PE0–PE4, PE7, PE8, PE10, PE12 y PE15 nunca en AF; PE5/PE6 solo AF4 y solo CM4; PA8 solo CM7; S12 con read-back de TIM1). Las escrituras crudas son solo de `bench_safe` (a entrada), dentro de `GPIO_BM_SectionEnter` |
| A28 | Pasa | Cada número del informe tiene su captura o su `.map`; no hay lenguaje de veredicto (grep) |

### Hallazgos

| ID | Clase | Dónde | Hallazgo | Acción |
| --- | --- | --- | --- | --- |
| m1 | Menor | `tools/ad2.py`, `ad2_set_dout` | `idle = Low` se acepta en cualquier paso "bus = AD2" sobre DIO1/DIO3, no solo en el control de T0b(f). Hoy no hay riesgo eléctrico: en esos pasos el MCU no conduce PE9/PE11, y S12 lo garantiza. Pero la guarda permite más de lo que el plan necesita | Ligar `idle = Low` al paso `T0B_DOUT` (por ejemplo, un argumento de paso que la guarda compare), o justificarlo en el informe. Puede ir al cierre de F0 o con F1 |
| m2 | Menor | `tim_hw_check.py`, `TREF_XCHK_JITTER_S = 2e-3` | La incertidumbre de la verificación cruzada de TREF (u ≈ 191 ppm) sale de un jitter de llegada de 2 ms que es un supuesto, no una medida (el propio informe §5.8 lo dice). La cota de TREF depende de él | Medirlo (por ejemplo, la dispersión entre la marca del host y `HAL_GetTick` en las líneas `TICK` de varias capturas) o dejarlo escrito como supuesto en el README. Antes del cierre |
| m3 | Menor | `CM4/App/Src/tim_selftest.c`, `k_claims[8]` ("PE0 LD2"); `tim_selftest_common.c`, `rule_gpioe` (`pin <= 1U` comentado como LD2) | LD2 es PE1. PE0 está libre y no se toca, que es lo correcto, pero el comentario lo nombra mal | Corregir los comentarios |
| m4 | Menor | `CM4/App/Src/tim_selftest.c`, `regs_sum` | El oráculo "registros sin cambiar" de T0b(b) en CM4 es un hash FNV de 10 registros: no incluye `OSPEEDR` y, en teoría, puede colisionar. CM7 compara una foto completa | Comparar una foto completa, como en CM7, con `OSPEEDR` incluido |

### Erratas del plan (las acepto; las corrige el responsable del catálogo en la revisión 2)

| Errata del informe | Veredicto |
| --- | --- |
| E1: `MCO1PRE = 0000` no apaga MCO1 | Aceptada. El RM dice "prescaler disabled", y en esta placa (rev V) sale HSE sin dividir: 332 flancos en 40,96 µs (`tim_f0_bench_d01061e.txt`). El control nuevo, PA8 en AF1 con TIM1 sin reloj, es el de T10 de `00_gpio` |
| E2: el HSE no es 8 MHz | Aceptada, con la nota N1 |
| E3: `prj_01` no compila con `-Werror` | Aceptada (`prj_01/cmake/common_flags.cmake` no lo lleva; `prj_03`/`prj_04` sí). La compilación con `-Werror` en scratch es una evidencia complementaria válida; el plan pasará a pedir "0 avisos" para `prj_01` |
| E4: el T0a de `00_gpio` no tiene walking-1 ni control | Aceptada. Lo que añadió el implementador cumple las instrucciones §3.5 |
| §5.5, T0b(c) con PE8/PE10/PE12/PE15 como salidas GPIO | Correcto: es el T0b(c) de `00_gpio` (paso "bus = MCU", nunca AF). La tabla de líneas del plan dirá "nunca en AF; salida GPIO solo en T0b(c)" |

### Notas

- **N1 (fuera del alcance de `03_tim`, para el responsable del catálogo).** Dos referencias independientes miden el reloj de la placa +1,29 % alto: el cristal del AD2 (TREF, +12 915 ppm) y el reloj del host (`HAL_GetTick`, +12 847 ppm), y `00_gpio` T10 ya lo había visto. Con `HSE_VALUE = 8 MHz` y PLL ×50, eso da SYSCLK ≈ 405 MHz y CM4 ≈ 203 MHz en VOS1. DS12923 tabla 13 limita VOS1 a 400 MHz (CM7) y 200 MHz (CM4): la plantilla funciona un 1,3 % por encima del máximo garantizado. El HSE viene del MCO del ST-LINK (UM2408: 8 MHz por defecto, puede cambiar con una actualización de firmware del ST-LINK, RN0093). Recomiendo confirmarlo con un instrumento independiente y revisar el firmware del ST-LINK antes de otra cosa. No bloquea F0: TREF existe precisamente para corregir las medidas del AD2.
- **N2.** La lectura `DIO = 0x2001` al abrir el AD2 (informe §5.8) es coherente con el firmware: `step_begin("T0A")` deja `MCU_ACK` alto en el primer handshake con el bus en `bench_safe()` (entradas sin pull), y PE8 flota. El AD2 solo leía (OE = 0, Digital Out deshabilitado): no hay contienda.
- **N3.** Los cambios de criterio que el informe atribuye al responsable (control de TREF, cota de TREF, S11(d) solo en los canales de Digital Out) están documentados con fecha y cita en el informe y en los commits. **Necesitan la confirmación del responsable** y la revisión 2 del plan.

### Pendiente para que F0 pase

1. **A18 y A27, ejecución del auditor:** flasheo `selftest-tim` de la cabeza de la rama y ejecuto `tim_hw_check.py` en frío y sin depurador. T0a, T0b(a)–(g) y TREF tienen que dar el mismo resultado que en la entrega. Requiere el banco conectado y la lista de comprobación física confirmada por quien está en el banco.
2. N3 confirmada por el responsable.

m1–m4 no bloquean el paso de F0, pero se corrigen antes de entregar F1.

## F0, adenda a la parte 1 (2026-10-01)

- **N3 confirmada.** El responsable del catálogo confirmó las tres decisiones: el control de TREF con PA8 en AF1, la cota de TREF 3·√(u² + r²) y S11(d) solo en los canales de Digital Out. Están en la revisión 2 del plan como R2-1, R2-2 y R2-3, junto con E1–E4 (R2-5 a R2-9), m1 (R2-4) y m2 (R2-10).
- **Efecto sobre la entrega.** El código de `d69508e` ya cumple R2-1, R2-2, R2-3, R2-5, R2-6, R2-7 y R2-8. Faltan R2-4 (m1) y R2-10 (m2), y quedan abiertos m3 y m4. Ninguno bloquea F0; los cuatro se corrigen antes de entregar F1.
- **Para que F0 pase** solo queda A18/A27: la ejecución del banco por el auditor.

## F0, auditoría 2: m3 y m4 (2026-10-01, en `96d3d8d`)

Commits auditados: `34de37b` (m3) y `e9a09c9` (m4). Respuesta del implementador: `IMPLEMENTATION_REPORT.md`, filas m3 y m4. m1 y m2 quedan para otra revisión.

### Lo que ejecuté

| Qué | Resultado |
| --- | --- |
| `git show 34de37b` y `git show e9a09c9`, diff completo | Ver la tabla |
| `grep` de `LD2`, `PE0` y `PE1` en los cinco archivos del selftest y en `tim_hw_check.py` | Solo quedan comentarios correctos: `PE1 LD2` y `PE0 (R4)` |
| `Common/Inc/board_def.h:56` | `BOARD_LED_YELLOW_PORT GPIOE /* LD2 - PE1 */` |
| `selftest-tim` desde un directorio vacío, en `96d3d8d` | Configura y compila con 0 líneas `warning:` |
| `CM4/CMakeLists.txt:112-116` y `CM7/CMakeLists.txt:120-124` | Los dos `tim_selftest.c` solo se compilan con `TIM_DRIVER_SELFTEST`: la producción no cambia |
| `docs/tim_f0_bench_8c4ab93.txt`, líneas 94–97 | `regs changed 0x000 -> PASS` en los dos casos de CM4 y `regs untouched 1` en CM7 |

### Veredicto

| ID | Veredicto | Evidencia |
| --- | --- | --- |
| m3 | **Cerrado** | `34de37b` solo cambia comentarios. Corrige los dos que señalé (`k_claims[8]` de CM4 y `rule_gpioe`) y otros dos iguales en CM7 (comentario de `bench_safe` y la primera fila de `k_t0b_refuse`). "PE0 (R4)" remite bien al plan: PE0 está en la lista "Nunca se tocan" (línea 485), que A22 asocia a R4 |
| m4 | **Cerrado** | El `port_snap()` de CM4 lee en crudo 12 registros: `MODER`, `OTYPER`, `OSPEEDR`, `PUPDR`, `AFR[0]` y `AFR[1]` de GPIOA y de GPIOE, a través de `__IO` (lecturas volátiles). `port_snap_diff()` compara registro a registro, y `regs_changed` pasa a ser una máscara de 12 bits, impresa con `0x%03lX`. El 0 esperado es correcto también para el caso aceptado (case 0): `bench_claim` solo evalúa y no escribe. CM7 añade `OSPEEDR` a su foto (`r[18]`: 3 puertos × 6 registros) y conserva el `memcmp`. El comentario de `tim_selftest.h` coincide con el código. Mientras CM4 ejecuta los claims, CPU1 espera en `cm4_send`, así que ninguna escritura concurrente puede dar un falso "sin cambios"; una escritura de verdad solo podría dar un falso fallo |

### Observación (no bloquea)

- **O1.** Ninguna de las dos fotos tiene un control de sensibilidad, es decir, un caso que demuestre que el oráculo detecta un cambio. Las dos solo se ejercitan con resultado esperado "sin cambios". Era igual con el hash, así que no es una regresión de m4. Propuesta para F1: en `run_safe()` de CM4, tomar una foto antes y otra después de `pin_cfg(PE5, AF4)` y exigir que la máscara marque exactamente los registros de GPIOE que cambian (`MODER` y `AFR[0]`).

### Pendiente

La captura con `0x000` es de la entrega; en placa lo verifico en mi ejecución del banco (A18/A27). Siguen abiertos m1 y m2.

## F0, auditoría 3: m1 y m2 (2026-10-01, en `157fa16`)

Commits auditados: `6a6b5f1` (m1) y `8c4ab93` (m2). Respuesta del implementador: `IMPLEMENTATION_REPORT.md`, filas m1 y m2 y la sección "Problema abierto: TREF varía más que su resolución r".

### Lo que ejecuté

| Qué | Resultado |
| --- | --- |
| `git show 6a6b5f1` y `git show 8c4ab93`, diff completo | Ver la tabla |
| `t0b_dout_guard_selftest()` importado de `tim_hw_check.py` y ejecutado en mi máquina, sobre el mock | PASS: 23 líneas, 16 rechazos ("refused, S2 reached") y 7 controles ("passed to the DLL") |
| Ajuste por mínimos cuadrados rehecho por mi cuenta con los 32 puntos impresos en `docs/tim_f0_bench_8c4ab93.txt` (líneas 158–159) | x = +13 571,8 ppm, u = 2,18 ppm y dispersión de 0,068 ms. La captura dice +13 571,2, 2,2 y 0,067: la diferencia se explica porque la captura imprime las marcas redondeadas a 0,1 ms. Con un segundo estimador (diferencia de las medias de las dos ráfagas) sale +13 572,2 |
| Pendiente de cada ráfaga por separado | banner +13 467 ppm, veredicto +13 256 ppm (ver O2) |
| `e` a partir del tramo impreso (3944 muestras para 40 periodos) | 4000/3944 − 1 = +14 198,8 ppm, igual que en la captura. Según x, el tramo esperado es 4000/(1 + x) = 3946,44 muestras |
| `time.get_clock_info('monotonic')` con el `py` del banco (3.14.6) | `QueryPerformanceCounter()`, resolución de 0,1 µs. Las marcas del host no están cuantizadas a 15,6 ms |
| `grep` de `IWDG`/`WWDG` en `prj_01` | Sin watchdog: la espera activa de `tick_burst()` (unos 0,8 s, dos veces) no tiene efecto fuera del selftest |

### Veredicto

| ID | Veredicto | Evidencia |
| --- | --- | --- |
| m1 | **Cerrado** | La guarda rechaza `Idle = Low` si `step_name != "T0B_DOUT"`. Esa comprobación va después de la de idle y antes de la lectura del OE y de cualquier llamada que habilite; el rechazo pasa por S2, igual que los demás. `step_name` solo lo asigna `Session.step()`, después de validar con S7 el nombre que anuncia el firmware contra la tabla `STEPS` del host. El valor por defecto `None` hace que cualquier llamada que no nombre el paso tenga Low rechazado. Es el único llamador fuera del mock (`grep`). El mock cubre Low en otro paso, Low sin paso y, como control, Zet en `T0B_S12`. Captura `8c4ab93`, líneas 19–41 |
| m2 | **Cerrado** | `TREF_XCHK_JITTER_S` desaparece. La cuenta es correcta: con t = a + b·k, la tabla da x = 1/b − 1 y u = se(b)/b². La dispersión se calcula con n − 2 grados de libertad. La captura imprime los 32 puntos, la dispersión de cada ráfaga y la global, x y u. El firmware imprime el valor que devuelve `HAL_GetTick()` en el momento, no el objetivo, así que un retraso del `LOG` no sesga k. La cota R2-2 no cambia |

### Observación (no bloquea)

- **O2.** u es solo el error estadístico de la pendiente. Supone que el retardo de llegada es ruido independiente de una línea a otra. Una diferencia de latencia constante entre las dos ráfagas no aparece en los residuos: 1 ms de diferencia equivale a unos 80 ppm en 12 s. Además, las pendientes de cada ráfaga por separado (+13 467 y +13 256) se separan de x en unas 1 y 3 desviaciones típicas de su propia pendiente, que es de unos 100 ppm. Hoy no tiene consecuencias, porque la cota queda dominada por r. Si el plan cambia la cota (ver D5), u tiene que incluir este término o justificar que se puede despreciar.

### Problema abierto de TREF: decisión D5 del responsable (bloquea F0)

Confirmo la cuenta del implementador. En `8c4ab93`, el tramo de 40 periodos mide 3944 muestras frente a las 3946,44 que predice x: −2,44 muestras. La cuantización de los dos flancos extremos solo puede explicar menos de ±1 muestra. En tres de las seis capturas la diferencia es mayor (+1,89, −2,23 y −2,44). Además, x pasa de +12 847 (`d69508e`) a +13 571 ppm (`8c4ab93`) entre una ejecución y otra.

Mi lectura es que el reloj de la placa tiene variación propia de cientos de ppm a corto plazo (40 µs frente a 12 s) y de una ejecución a otra. Con el u medido, la cota se reduce a ≈ 3r ≈ 760 ppm, y la entrega pasó con 627,5. Mi ejecución del banco (A18) puede fallar por el reloj y no por el firmware. **Hay que decidir el criterio antes de que yo ejecute el banco.**

Recomendación:

1. **TREF en F0: la opción (a) del implementador, con las capturas repartidas en el tiempo.** Se toman N capturas de 40 periodos (por ejemplo, 8 junto a la ráfaga `TICK` del banner y 8 junto a la del veredicto). Así e y x cubren el mismo intervalo. ē es la media y s su dispersión medida, que ya incluye r. Criterio: |ē| ≤ 20 000 ppm y |ē − x| ≤ 3·√(u² + s²/N + u_lat²), donde u_lat es el término de O2. Cambia la tabla de pasos (TREF al final), así que `TIM_BENCH_VERSION` pasa a 2.
2. **De F1 en adelante: medidas de frecuencia relativas a MCO1 en la misma captura.** MCO1 (PA8, DIO14) es HSE/8 y sale de la misma fuente que los temporizadores. Si cada medida del LA sobre un canal de TIM se divide por la frecuencia de DIO14 medida en la misma adquisición, la variación del reloj se cancela. Esto sustituye "corregido con TREF" de R2-9: un único TREF al principio arrastra hasta unos 600 ppm de error. Va en la revisión 3 del plan.

### N1, actualizada

Además del +1,3 % de desviación media, el reloj de la placa varía de una ejecución a otra unos ±400 ppm, y en una ventana de 40 µs se aleja hasta unos 600 ppm de su media de 12 s. Eso es compatible con un HSE que no sale de un cristal, sino de un oscilador RC o de una cadena de PLL sin referencia de cristal en el ST-LINK. Es una hipótesis que no he verificado. Refuerza la recomendación de medir el MCO del ST-LINK con un instrumento independiente. Si se confirma, cambiar a otra fuente de HSE es una decisión de la placa y del catálogo, fuera de `03_tim`.

### Pendiente para que F0 pase

1. Decisión D5 del responsable y revisión 3 del plan.
2. El implementador aplica D5 y entrega una captura nueva.
3. A18/A27: mi ejecución del banco sobre esa entrega, con la lista de comprobación física confirmada.

## F0, adenda a la auditoría 3 (2026-10-01)

- **D5 aprobada.** El responsable del catálogo aprobó la recomendación. Está en la revisión 3 del plan: R3-1 (TREF\_A + TREF\_B, 8 + 8 capturas, x ajustada sobre las medias de ráfaga, criterio \|ē − x\| ≤ 3·√(u² + s\_ef²/16)), R3-2 (desde F1, las medidas del AD2 como cociente con MCO1 en la misma adquisición) y R3-3 (protocolo del banco v2). O2 queda cubierta por R3-1.
- **Para que F0 pase:** el implementador aplica R3-1 y R3-3 y entrega una captura nueva. Después viene mi ejecución del banco (A18/A27), con la lista de comprobación física confirmada.
