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
