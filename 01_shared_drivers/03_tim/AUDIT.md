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

## F0, auditoría 4: revisión 3 aplicada, SMPS y la serie de 17 ejecuciones (2026-10-02, en `cf26e4e`)

Rango auditado: `92983ec..cf26e4e`. Código: `949c39c` (R3-3), `9986015`, `abea673` y `0f9534c` (R3-1) y `2481396` (SMPS en `prj_01`). El resto es evidencia: 17 capturas con la revisión 3, una abortada (`0f9534c`), builds y `.map`. El informe declara F0 **en curso, no entregada**: el implementador se detuvo como pide R3-1, porque el criterio falla con la misma imagen.

### Lo que ejecuté

| Qué | Resultado |
| --- | --- |
| `git show` de los cinco commits de código, diff completo | Ver la tabla |
| `git diff e7542dc cf26e4e --name-only` | Nada fuera de `01_shared_drivers/03_tim/`, `prj_01_template_dualcore/` y `tools/ad2.py`. `02_firmware_projects/` sin cambios (A23) |
| `prj_01` debug y release desde directorios vacíos, en `cf26e4e` | 0 avisos. CM7 debug 29304/132/55668 y release 28356/132/55672, frente a 29144 y 28228 de mi base `e7542dc`: **+160 y +128 B de `.text`**. CM4 igual: 3456/24/12560 y 3112/24/12560. Coincide con `docs/tim_f0_map_sizes_2481396.txt` |
| `HAL_PWREx_ConfigSupply` en la HAL del repositorio (`stm32h7xx_hal_pwr_ex.c`) | Si la configuración ya está bloqueada y coincide, devuelve `HAL_OK`; si no coincide, `HAL_ERROR` sin escribir nada; si no está bloqueada, la escribe y espera `ACTVOSRDY` con timeout. Un reset en caliente no llega a `Error_Handler` |
| Las 17 capturas con el criterio de R3-1: extraje con un script propio e, s\_ef, x, u, la cota, ē\_A y ē\_B | Ver "La serie" |
| `tim_f0_bench_2481396.txt`, `_cd0d613_9` y `_cd0d613_10` | Checkout del host, árbol limpio, `Rev` e id iguales en los tres lados, `FW VERDICT=PASS`. 15/16 (FAIL solo en TREF), 15/16 (ídem) y 16/16 |

### Veredicto de los cambios

| ID | Veredicto | Evidencia |
| --- | --- | --- |
| R3-3 | **Aplicado** | `TIM_BENCH_VERSION = 2`. La tabla de pasos es igual en `k_tst_steps` y en `STEPS`. `TREF_B` solo permite PA8 en AF0 (`af1_pa = 0`), y el AF1 sigue siendo exclusivo del control de `TREF_A`. `step_tref(name, control)` guarda y restaura `RCC_CFGR` con read-back en cada grupo |
| R3-1, capturas | **Aplicado** | 8 por grupo dentro de un handshake, ≥ 40 periodos y ≥ 100 ms entre comienzos, con una comprobación que falla si no se cumple (`TREF_x captures`). Se imprimen eᵢ, rᵢ, ē\_A, ē\_B, ē, s, r̄/√6 y s\_ef |
| R3-1, x | **Aplicado** | La consola recoge todas las líneas `TICK` con su hora de llegada. Se agrupan por ráfaga y el ajuste se hace sobre las M medias. u = se(b)/b² con M − 2 grados de libertad. Exige M ≥ 10 y el número de líneas esperado por ráfaga (16, 4, …, 16); si no, no hay x y TREF falla |
| R3-1, criterio | **Aplicado** | `n == 16`, \|ē\| ≤ 20 000 y \|ē − x\| ≤ 3·√(u² + s\_ef²/16) |
| SMPS, `2481396` | **Correcto, fuera del alcance y aceptado como decisión del responsable** | Está en un commit propio, comentado, con el retorno comprobado y el mismo valor que `prj_04` (`main.c:297`). El diagnóstico por SWD (`PWR_CR3 = 0x46`, `ACTVOSRDY = 0`, `PORRSTF`) cuadra con RM0399 §7.4.1. Es un defecto latente de la producción de `prj_01`, no del banco. **El gate de tamaños de F0 tiene que cambiar de base**: ver D6(b) |

### La serie: el criterio de R3-1 está mal calibrado

Sobre las 17 ejecuciones, con d = ē − x y z = d / (cota/3):

| Magnitud | Valor |
| --- | --- |
| Media de d, desviación típica (n − 1) | +36,1 y 184,5 ppm |
| σ que declara cada ejecución (cota/3), media | 107,5 ppm |
| rms de z, máximo de \|z\| | **2,00** y 5,32 (`2481396`) |
| Fallos | 2 de 17 (`2481396`, z = +5,32; `cd0d613_9`, z = +3,47) |

Confirmo los números del informe. Añado un hecho que el informe no separa. El rms de (ē\_A − ē\_B)/(s\_ef·√(2/8)) es **1,51**: dentro de una sesión, las medias de los dos grupos difieren 1,5 veces más de lo que predice la dispersión de sus capturas. Las 8 capturas de un grupo no son independientes, porque el reloj de la placa deriva en escalas de segundos, así que s/√16 infravalora la incertidumbre de ē. Ese factor explica parte del exceso. Si se aplica, el rms de z baja de 2,00 a 1,34; el resto lo atribuyo a x (latencia del VCP a lo largo de la sesión) o a que ē y x promedian el mismo reloj con distinta ponderación en el tiempo. Con estos datos no se puede separar más.

Ninguno de los dos fallos es del firmware: todo lo demás pasa en las 17 ejecuciones, y las dos referencias (el cristal del AD2 y el reloj del host) coinciden con la placa salvo por la deriva.

### Decisión D6 del responsable (bloquea F0)

**(a) Criterio de TREF.** Recomiendo añadir a la cota un término medido de la variación entre ejecuciones, σ\_ser, con el mismo principio de R2-10 (medido, no supuesto). La varianza que las 17 ejecuciones no explican es σ\_ser = √(media(d²) − media(u² + s\_ef²/16)) = **145 ppm**. El criterio queda:

\|ē\| ≤ 20 000 ppm y \|ē − x\| ≤ 3·√(u² + s\_ef²/16 + σ\_ser²), con σ\_ser = 145 ppm, fuente: las 17 capturas de la revisión 3 que se citan en esta sección.

Con ese criterio, el rms de z de las 17 queda en 1,05, el máximo de \|z\| en 2,55 y la peor ejecución (`2481396`) al 85 % de la cota. No falla ninguna. La cota queda entre unos 500 y 700 ppm. Perder sensibilidad no tiene coste: desde R3-2 ninguna medida de F1 en adelante se corrige con TREF, y un error grueso, como un divisor de MCO1 mal o una frecuencia de muestreo del LA mal, es ≥ 0,5 %, más de 7 veces la cota. σ\_ser es de este banco (esta placa y este ST-LINK): se vuelve a medir si cambia cualquiera de los dos.

**(b) Gate de producción.** La base pasa de `e7542dc` a `2481396`: CM7 debug 29304/132/55668 y release 28356/132/55672, CM4 debug 3456/24/12560 y release 3112/24/12560. Desde ahí, ningún commit del 03\_tim puede cambiarlos. El cambio de SMPS queda como desviación aprobada, con su propia fila en el informe de cierre y en el README.

### Para que F0 pase

1. D6 aprobada, y revisión 4 del plan.
2. El implementador cambia la cota (un commit, sin tocar nada más) y entrega una captura.
3. A18/A27: mi ejecución del banco sobre esa entrega, con la lista de comprobación física confirmada.

## F0, adenda a la auditoría 4 (2026-10-02)

- **D6 aprobada** por el responsable del catálogo. Está en la revisión 4 del plan: R4-1 (σ\_ser = 145 ppm en la cota de TREF) y R4-2 (base de la producción de `prj_01` en `2481396`; el cambio de SMPS queda como desviación aprobada).
- **Para que F0 pase:** el implementador aplica R4-1 en un commit y entrega una captura. Después viene mi ejecución del banco (A18/A27), con la lista de comprobación física confirmada.

## F0, auditoría 5: R4-1 y la serie de validación (2026-10-02, en `0adffa9`)

Rango auditado: `336180b..0adffa9`. Código: `7ac9e44` (R4-1), un solo commit. El resto es evidencia: la ejecución entregada (`tim_f0_bench_7ac9e44.txt`), una serie de 10 con la imagen de `2a78fe5` (pedida por el responsable, según el informe), builds y `.map`.

### Lo que ejecuté

| Qué | Resultado |
| --- | --- |
| `git show 7ac9e44` | Ver la tabla |
| `git diff 2481396 0adffa9`, sin `docs/` ni `tools/` de `prj_01` | Vacío: desde la base de R4-2 no cambió ningún archivo de firmware. Las imágenes de producción y de `selftest-tim` son las de `2481396` |
| `git diff e7542dc 0adffa9 -- 02_firmware_projects` | Vacío (A23) |
| `.\tools\build_all.ps1 -Clean` en `0adffa9` | 8/8 PASS. `prj_01`: CM7 debug 29304/132/55668, CM4 debug 3456/24/12560, CM7 release 28356/132/55672 y CM4 release 3112/24/12560, iguales a la base de R4-2 (A16) |
| Script propio sobre las 11 capturas: e, s\_ef, x, u, σ\_ser y la cota, recalculada; cabecera (`tree clean`, `Rev` e id iguales en CM7, CM4 y el host, 16/16, `FW`/`HOST VERDICT=PASS`) | Las 11 cotas coinciden con la mía al 0,1 ppm. Las 11 pasan, con las cabeceras completas |

### Veredicto

| ID | Veredicto | Evidencia |
| --- | --- | --- |
| R4-1 | **Aplicado** | `TREF_SIGMA_SER_PPM = 145.0`, con un comentario que cita su fuente (las 17 capturas, plan R4-1, auditoría 4). La cota es 3·√(u² + s\_ef²/n + σ\_ser²). Siguen `n == 16` y \|ē\| ≤ 20 000. La captura imprime σ\_ser y su fuente. No cambia nada más: el diff solo toca la constante, el docstring y la línea del criterio |
| Validación del criterio | **Pasa** | La serie de `2a78fe5` (10 ejecuciones) es independiente de la que fijó σ\_ser. Con R4-1: 10/10 pasan, rms de z = 0,89 y máximo \|z\| = 2,00 (`_6`, al 67 % de la cota). σ\_ser medido solo con esa serie da 106 ppm: la constante de 145 queda del lado conservador. Con el criterio de R3-1, `_6` habría fallado |

La serie de 10 contradice mi instrucción "una sola ejecución" del prompt de la revisión 4. El informe la presenta como una petición del responsable, separada de la ejecución entregada, y no cambia el código; la acepto como evidencia de validación, no como entrega.

### Estado de F0

La parte estática y de build pasa, y el criterio de TREF queda validado fuera de la muestra que lo fijó. **Solo falta A18/A27: mi ejecución del banco**, sobre `selftest-tim` compilado en la cabeza de la rama, con la lista de comprobación física confirmada por quien está en el banco. Si pasa, F0 se cierra y el implementador puede empezar F1 (`tim_bm`, con O1 y R3-2).

## F0, auditoría 6: ejecución del banco por el auditor (2026-10-02, en `cae5414`) — **F0 PASA**

Lista de comprobación física confirmada en el chat por el responsable del catálogo (Hector), en el banco: cableado y GND según `00_gpio` §8, sin resistencias en serie, nada en W1/W2/V+/V−, WaveForms cerrado, sin depurador y los dos USB conectados. Antes de ejecutar, `tasklist` no mostraba ningún proceso de WaveForms ni de depuración.

### Lo que ejecuté

| Qué | Resultado |
| --- | --- |
| `selftest-tim` desde un directorio vacío, en `cae5414` | 0 avisos |
| `py tools/flash.py --preset selftest-tim`, con el script del banco parado | "Download verified successfully" y reset |
| `py tools/tim_hw_check.py --checklist-confirmed Hector --out <scratchpad>`, **una sola ejecución** | **16/16, `FW VERDICT=PASS` y `HOST VERDICT=PASS`**. Captura sin editar: `03_firmware_templates/prj_01_template_dualcore/docs/tim_f0_audit_bench_cae5414.txt` (248 líneas), copiada del scratchpad después de ejecutar para que el árbol estuviera limpio durante la ejecución |

### Contenido de la captura

- **Trazabilidad (A21):** checkout del host `cae5414`, `tree clean`, `Rev` `cae5414` e id `d5f8bbb3e68470f8` iguales en CM7, CM4 y el host, protocolo v2 y REV\_ID `0x2003`.
- **AD2 (A27):** se abre en S2 (`OE=0x0000 DIO=0x0000 Digital Out enable=0x0000 Zet=0xFFFF`) y se cierra en S2 (`OE=0x0000 enable=0x0000 Zet=0xFFFF`).
- **T0a:** 13 líneas en pull-up `0x4FFF`/`0x1FFF` y en pull-down `0x0000`, en el AD2 y en el MCU. Walking-1 sin diferencias. El control marca `0x0200` en los dos lados.
- **TREF\_A:** 8 capturas de 40–41 periodos, separadas ≥ 100,4 ms, con eᵢ de +13 096 a +14 199 ppm. El control da 0 flancos con PA8 en AF1 y TIM1 sin reloj (C1 = C2 = 0). `RCC_CFGR` se restaura (`0x0000001B`).
- **T0b(a)/(e):** mocks PASS.
- **T0b(b):** CM7 da 20 rechazos y el control aceptado, con los registros sin tocar. CM4 da `accepted 0x0001` en T0B, `0x0000` entre pasos y `regs changed 0x000` las dos veces (m4 verificado en placa). `bench_safe` de CM4 devuelve PE5 a entrada.
- **T0b(c):** `0x5A` detectado dos veces. **T0b(d):** timeout con `bench_safe`.
- **T0b(f):** Digital Out a 1,0000 MHz solo en DIO1, con OE durante la ejecución `0x1000` (S11 d). Con Zet, el bus sigue los dos pulls. El control con Low da `0xFD` en el AD2 y en el MCU (m1 verificado en placa). S2 deja `enable 0x0000` y `Zet 0xFFFF`.
- **T0b(g):** 13 rechazos y el control aceptado. S5 con PE9/PE11 en AF1 pasa. TIM1 vuelve a los valores de reset.
- **TREF\_B:** 8 capturas, `RCC_CFGR` restaurado.
- **CM4:** `timeout 0, aborts 0`.
- **TREF (R4-1):** ē = +13 106,7 ppm (ē\_A +13 577,6, ē\_B +12 635,8), s\_ef = 653,9; x = +13 071,8 ± 18,2 (11 ráfagas, residuos 0,358 ms). \|ē − x\| = 34,9 ≤ 657,8 → PASS.

### Tabla A de F0, cerrada

| # | Resultado |
| --- | --- |
| A1, A16, A17, A19–A23, A26, A28 | Pasan (auditorías 1–5, con la base de producción de R4-2) |
| A18 | **Pasa**: T0b en verde en la ejecución del auditor |
| A27 | **Pasa**: guardas S2/S3/S11/S12 en placa y en los mocks |

Hallazgos de F0: m1–m4 cerrados y verificados en placa (m1, m4). O1 (control de sensibilidad de las fotos de registros) pasa a F1.

**Veredicto: F0 pasa.** El implementador puede empezar F1 (`tim_bm`) sobre `driver/tim`.

### Notas que siguen abiertas, fuera del gate

- **N1 (del responsable):** el reloj de la placa va un +1,3 % alto y deriva. En esta misma ejecución, ē\_A − ē\_B = +942 ppm en unos 12 s. Hay que medirlo con un instrumento independiente y revisar el firmware del ST-LINK. En `03_tim`, R3-2 lo neutraliza para las medidas del AD2, pero no el exceso sobre VOS1.
- **σ\_ser (R4-1)** es de este banco: si cambia la placa o el ST-LINK, se vuelve a medir.

## F1, parte 1: revisión estática, de build y de la captura (2026-10-02, en `43d083a`)

Rango auditado: `708c212..43d083a`. Commit entregado: `f86706c`, con la evidencia en `9e7388c` y el informe en `43d083a`. La parte de código son `tim_bm.h` (419 líneas), `tim_bm.c` (1473), el target CMake, `ad2.py` (patrón custom), los dos `tim_selftest.c`, `tim_selftest_common.c`, `tim_selftest.h` y `tim_hw_check.py`.

**Estado: F1 no pasa todavía.** No hay ningún hallazgo bloqueante en esta parte. Falta A18 (mi ejecución del banco), y la decisión D7 sobre el contenido de la revisión 5 del plan, antes de congelar la API.

### Lo que ejecuté

| Qué | Resultado |
| --- | --- |
| Lectura completa de `tim_bm.h` y `tim_bm.c` contra el contrato del plan (C1–C15, reglas 1–12) | Ver "Driver" |
| Verificación independiente de `k_tim[]` contra RM0399 (`timers_pwm/40–44_*.txt`, `dma_mdma/18_*.txt` tabla 126, `rcc/09_*.txt`) y `stm32h755xx.h`, campo a campo en las 14 instancias: ancho, canales, ITR, MMS, RCR/BDTR/SMCR/DCR/AF1/AF2/CCR5-6, valores de reset, TISEL, DMAMUX, vectores y bit de RCC | **0 discrepancias.** Las citas de línea de cada fila apuntan a la sección correcta. TS: ITR0–3 = `00000`–`00011` (40.4.3, 42.4.3, 43.5.3) e ITR4–8 = `01000`–`01100` (41.4.3); `itr_mask` impide generar ITR4–8 donde son reservados |
| Las dos erratas del informe §5.7 | **Confirmadas.** (a) TIM1 `TI1SEL = 0001` es COMP1 (40.4.30, l. 4793) y TIM8 `0001` es COMP2 (40.4.31, l. 4826). (b) DIR es de solo lectura en center-aligned (40.4.1, l. 2933–2937), y 40.3.2 (l. 1619) añade que DIR y CMS no se cambian a la vez. Las dos escrituras de `InitTimeBase` y la RMW de `Deinit` cumplen |
| `tim_bm.c` aislado, con `arm-none-eabi-gcc` y los flags de CPU/defines de `compile_commands.json`, CMSIS como `-isystem`, `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wcast-qual -Werror`, CM7 y CM4 | 0 avisos en los dos. Con los vendor headers como `-I`, `-Wconversion` salta en `cachel1_armv7.h` (CMSIS, no el driver): el informe debería decir que usó `-isystem` |
| Lo mismo **sin `-DUSE_HAL_DRIVER`** | Compila, y `-M` no muestra ningún header `stm32h7xx_hal*`/`_ll*`. Con la opción del proyecto, `stm32h7xx.h` incluye la HAL de forma transitiva, pero el driver no la usa (C8) |
| `.\tools\build_all.ps1 -Clean` en `43d083a` | 8/8. `prj_01`: CM7 29304/132/55668 y 28356/132/55672, CM4 3456/24/12560 y 3112/24/12560, iguales a la base de R4-2 (A16) |
| `selftest-tim` desde un directorio vacío | 0 avisos. CM7 85224/140/65828, CM4 16244/24/13816 |
| `git diff e7542dc 43d083a --name-only` | Nada fuera de lo permitido; `02_firmware_projects` sin cambios (A23). Desde `2481396`, en `prj_01` solo cambian los CMakeLists (dentro de `if(TIM_DRIVER_SELFTEST)`), el selftest, el host y `docs/` |
| T3 rehecho con mi propia implementación de las "Reglas de cálculo" del plan (Python, enteros con truncado de C) sobre las líneas `T3 CT/DC/KC` de la captura | **234 + 70 + 34 vectores, 0 diferencias** (estado, psc, arr, ticks, error\_ppm; ccr; tabla 59) |
| `git grep TIM6` en el selftest | Solo comentarios y las listas de solo lectura (`k_all_tims`: `GetCaps` y `GetKernelClockHz`; tablas de `GetIRQn`/`GetDmaRequest`). `k_tims`, la lista que se escribe, no tiene TIM6 (A22) |
| `docs/tim_f1_bench_f86706c.txt` (1149 líneas) | Checkout `f86706c`, `tree clean`, `Rev` e id `e3df266a1b52e8fc` iguales en CM7, CM4 y el host, protocolo v3, lista confirmada. AD2 abierto y cerrado en S2. **109/109, 0 líneas FAIL, `FW VERDICT=PASS` y `HOST VERDICT=PASS`.** TREF (R4-1): 303,4 ≤ 590,1 |

### Driver

Cumple el contrato. Lo que comprobé, punto por punto:

- **Validación (C6):** el orden es NULL, instancia, canal o evento, capacidad, rango y estado, sin acceso al periférico antes de terminar. Todo dato de silicio sale de `k_tim[]` (C1).
- **C12:** `ERR_BUSY` con CCxE = 1 (ConfigOutput/ConfigInput), CEN = 1 (InitTimeBase) y SMS ≠ 0 (ConfigSlave y ConfigPwmInput). Read-back en cada escritura de configuración. `Start` no lo hace, de forma justificada (OPM).
- **C10:** las RMW de CR1, CR2, SMCR, CCMRx, CCER, BDTR, DIER, TISEL y RCC van bajo PRIMASK guardado y restaurado. SR se limpia con una sola escritura (`SR = ~taken`, como `__HAL_TIM_CLEAR_FLAG`). En RCC solo se escribe el banco propio; `D2CFGR` y `CFGR` solo se leen.
- **C11:** InitTimeBase sigue CR1 → PSC → ARR → RCR → UG con URS = 1 → CR1 → SR = 0. Deinit apaga primero las salidas (MOE, CCER), después el contador y DIER, y por último la imagen de reset con read-back; no genera UG, por TRGO.
- **C13:** `WaitFlags` está acotada por CYCCNT y detecta un DWT parado.
- **C15:** las funciones puras se verifican arriba.
- **TS y SMS:** codificación con SMS[3] y TS[4:3], y secuencia SMS = 0 → TS → SMS.
- **ES0445 2.16.1:** se rechaza por las tres vías (ConfigMaster, ConfigSlave e InitTimeBase con OPM).

No encontré defectos. Una nota menor: el `@brief` de `InitTimeBase` dice "SR.UIF = 0", pero `SR = 0` limpia todos los flags de la instancia. Conviene decirlo en el README de cierre.

### Pruebas y banco

- **`ad2.py` (`dout_custom`):** solo configura el canal (tipo, divisor y datos, con read-back), rechaza todo canal fuera de DIO1/DIO3 y no habilita nada. La única vía de arranque sigue siendo `ad2_set_dout` (S11), y S2 resetea Digital Out. Pasa A27 en lo estático.
- **T8, desviación 1:** d = media(diferencias directas) − media(periodos en PWM input), sobre una generación de 1 MHz. Está acotado a 0 ≤ d ≤ 4 ticks por una comprobación del host; en la entrega, d = 1,94. La captura directa queda anclada a MCO1 por otras pruebas independientes (ICPSC = 8, filtro y DMA de T13(b)), así que la corrección solo es autorreferente en el punto de 1 MHz, y d es una magnitud física que RM0399 40.3.26 describe sin valor. Sin d, el punto de 1 MHz fallaría por unos 2 ticks (200/201 frente a 202,65). La acepto. El informe dice que el responsable la aprobó; **falta recogerla en el plan (D7)**.
- **R3-2:** T6 y T8 juzgan con MCO1 en DIO14 en la misma adquisición. En T8, MCO1 se mide antes y después de las capturas (desviación 2), lo que acota la deriva del reloj. Tabla de pasos v3, con `RCC_CFGR` guardado y restaurado en `T6_TIM1`, `T6_TIM15`, `T8` y `T9`.
- **O1:** controles de sensibilidad en T0b(b), con `0x00021` en CM7 y `0x440` en CM4, y en T1, con `0x0002040` y `RCC 0x01`, cada uno con la máscara impresa. **O1 cerrado.**
- **T15:** 0 pérdidas con el driver y 8893 en el control RMW sin PRIMASK.
- **Desviaciones 2–7 y 9–12 del informe:** procedimiento de medida o detalle que el plan no fija. Las acepto como están documentadas.
- **Capturas de desarrollo:** siete `tim_dev_f1_*` (3,3 MB), con el nombre de §3.4 y fuera de la evidencia. Aceptadas, aunque pesan; al cierre conviene borrarlas, como los documentos de proceso (decisión abierta 6).

### Decisión D7 del responsable, antes de congelar la API

La revisión 5 del plan tiene que recoger:

1. **R5-1 (T8):** la corrección d de la desviación 1, con la cota 0 ≤ d ≤ 4 ticks, medida en cada sesión, solo para los puntos en PWM input.
2. **R5-2 (API):** `TIM_BM_CH(n)` entra en la API congelada de `tim_bm` (desviación 8). Lo recomiendo: el contrato usa `ch_mask` sin fijar la codificación, y la macro la fija.
3. **R5-3 (erratas):** regla 2, TISEL de TIM1/TIM8 (`0001` = COMP1/COMP2), y reglas 3 y 11, DIR/CMS (una escritura que borra CMS antes de la que fija o borra DIR).
4. **R5-4 (pasos v3):** los pasos `T6_TIM1`, `T6_TIM15`, `T8` y `T9`, y las desviaciones de procedimiento 2–7 de T5, T7, T8, T9 y T10.

### Para que F1 pase

1. D7 aprobada y revisión 5 del plan, que no requiere código.
2. **A18/A27:** mi ejecución del banco sobre `selftest-tim` compilado en la cabeza de la rama, con la lista de comprobación física confirmada.

## F1, adenda a la parte 1 (2026-10-02)

- **D7 aprobada** por el responsable del catálogo. Está en la revisión 5 del plan: R5-1 (corrección d de T8, 0 ≤ d ≤ 4 ticks), R5-2 (`TIM_BM_CH(n)` en la API), R5-3 (erratas de TISEL de TIM1/TIM8 y de DIR/CMS) y R5-4 (pasos v3 y desviaciones de procedimiento 2–7).
- **Para que F1 pase:** solo queda A18/A27, mi ejecución del banco.
