# Auditoría de `00_gpio` F1b (G1, G13 en `prj_04`)

Auditor: Claude (Claude Code), para Hector Rodriguez · Rama: `driver/gpio-f1b` · Plan: `IMPLEMENTATION_PLAN.md` revisión 2 (revisión 3 con esta auditoría)

Este archivo lo escribe solo el auditor. Un hallazgo se cierra cuando el auditor lo verifica.

## F1 (`63a746c`)

Entrega: `50f16d8..63a746c`. Código en `9051a6a`, `53f0f88`, `c8001bf`, `ee2ba23`, `61c6e28`, `af4162e`; herramienta en `98af5a8`, `0f2f7b1`, `7ad0bbd`; capturas en `059a5bb`; informe en `63a746c`. La placa, en remoto (G-S7).

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| Alcance | Solo lo permitido: `prj_04` (`Common/shared_memory.[ch]`, los dos `main.c`, `adc_acquisition.c`, `dma_selftest.c`, los dos `gpio_selftest.c`), `04_uart/uart_hal.c`, el target `stm32h7_driver_uart`, `tools/gpio_f1b_checks.py`, capturas e informe |
| `build_all -Clean` en `63a746c`, árbol limpio (mi ejecución) | 8/8; tamaños iguales a los del informe §5.2.1 |
| `-Werror` (mi ejecución) | `prj_04` debug y release, `prj_01`, `prj_02` y `prj_03` debug: exit 0 y 0 avisos en los cinco |
| Lectura por SWD independiente, con mi propio script (`prj_04/docs/gpio_f1b_audit_swd_63a746c.txt`) | `prj_04` debug compilado en `63a746c` y flasheado ("Download verified successfully"). Resultados:<br>• `s_shared_lock` de CM7 y CM4 iguales a `g_shared_gpio_lock` de cada ELF (direcciones de `nm`);<br>• `HSEM_R5` (`0x58026414`) = 0;<br>• `m7_/m4_gpio_lock_fail_count` = 0;<br>• retención máxima: CM7 1228 ciclos (3,07 µs, 19,5×), CM4 356 ciclos (1,78 µs, 33,7×);<br>• PB1, PB13, PD8 y PD9 exactos frente a una tabla escrita a mano, y los controles PB15 y PD0 en reset.<br>**PASS** |
| T5 (mi ejecución, `gpio_f1b_audit_t5_netcheck_63a746c.txt`, `gpio_f1b_audit_t5_query_63a746c.txt`) | ICMP 4/4, UDP de 1 a 1472 B idénticos, ráfaga 100/100; `rx_nobuf`, `tx_nobuf`, `dma_err` y `acl_drop` a 0. `query_adc.py`: las muestras crecen, `OVR: 0`, 505 kHz medidos (el reloj de la placa va +1,1 %). **PASS**. La consulta del ADC se hizo justo después y no a la vez; su captura es la salida tal cual de `py tools/query_adc.py --json --count 3 --rate 1` desde `prj_04`, a las 12:52, con la misma imagen |
| Revisión de la herramienta | Un subagente la revisó línea a línea; los hallazgos de abajo que vienen de esa revisión los comprobé yo: rutas de T2a, `--test all`, direcciones del control de T3, la base del HSEM |

**El código de producción hace lo que pide el plan:** el hook en los dos núcleos y en el orden correcto, PB1 por `gpio_bm` con su error propagado, la MSP de la consola por `gpio_hal` y los selftests de CM4 por `gpio_bm`. Lo confirma mi evidencia independiente. Lo que no pasa es la **evidencia de la entrega** (la herramienta y las capturas) y un selftest roto por el diseño del plan.

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| B1 | Bloqueante | **La comprobación de HSEM 5 de T3 (y de T6a) lee otro periférico y pasa siempre.** `gpio_f1b_checks.py:61-62` usa `HSEM_BASE = 0x58001400` (es `SPI6_BASE`) + `0x114`: un offset reservado de SPI6 que lee 0. `stm32h755xx.h:2329`: `HSEM_BASE = D3_AHB1PERIPH_BASE + 0x6400` = `0x58026400`. **El plan también estaba mal (mío):** pedía `HSEM_RLR5`, y leer `RLR` *toma* el semáforo (RM0399 §11.4.2, `10_hsem/README.md`); con la dirección buena, esa lectura por el depurador habría bloqueado el hook de los dos núcleos | Leer `HSEM_R5` (`0x58026414`, sin efectos) con la base tomada del CMSIS; plan revisión 3 |
| B2 | Bloqueante | **T2a no mira `prj_02` ni `prj_03`:** sus rutas no existen (`prj_02_freertos_lwip`, `prj_03_adc_dma_freertos`, l. 258-259) y una ELF que falta se salta sin aviso (l. 239, 262) | Rutas reales; una ELF que falta es FAIL |
| B3 | Bloqueante | **El criterio de T2a cambió y la herramienta no aplica el nuevo.** La errata del informe §5.7.1 es correcta y es mía: `gpio_hal.c` llama a `HAL_GPIO_Init` dentro de la sección, así que toda CM7 que enlace `gpio_hal` lo define. Pero el criterio 4 del informe ("`gpio_hal.c.obj` es el único objeto que referencia `HAL_GPIO_Init`") no está implementado, y §5.5 dice que no hubo desviaciones | Criterio de la revisión 3: en cada ELF, `HAL_GPIO_Init` solo lo referencia `gpio_hal.c.obj` (`nm -u` de cada objeto enlazado) y `HAL_GPIO_DeInit` no está; §5.5 lo dice como desviación |
| B4 | Bloqueante | **El control de T2a no usa ELF:** solo hace `nm` del primer `uart_hal.c.obj` que encuentra (l. 276-290), y si falta se omite y T2a pasa. El directorio de control no tiene `prj_04` release, `prj_02` ni `prj_03` | Las cinco ELF de control compiladas en `07db657`, el mismo criterio aplicado a ellas y exigido que falle; si falta una, FAIL |
| B5 | Bloqueante | **La captura del control de T3 no vale como evidencia.** Problemas:<br>• dice "tree dirty" y es anterior a `7ad0bbd`, que cambia ese modo;<br>• la parte 1 leyó las direcciones del árbol actual en la imagen de `07db657`: allí `0x20012908` es `g_console` y `0x3002092c` es `__bss_end__` (los `s_shared_lock` de `07db657` están en `0x20012904` y `0x30020928`);<br>• `g_console` leyó 0 con PD8/PD9 en AF7, así que no es seguro que la imagen fuera la de control;<br>• es una captura montada a mano (cabecera propia, "Part 1/Part 2"), no una ejecución sin editar | Repetir el control con la herramienta commiteada, el árbol limpio, las direcciones impresas y la identidad de la imagen comprobada (B6) |
| B6 | Bloqueante | **No hay prueba de qué imagen había en la placa en T3, T4 ni T6a**, y la ELF de `prj_04` se recompiló entre T4 (12:10) y T5 (12:14). El plan pide "la imagen del mismo commit" | Cada sesión de placa empieza con el flasheo de la ELF del commit y su "Download verified" en la captura, o compara un símbolo leído por SWD con la ELF |
| B7 | Bloqueante | **T2b cuenta como "sin hallazgos" lo que no pudo preprocesar:** `return []` si `gcc -E` falla (l. 340-341), y se salta en silencio una fuente, un comando o un `compile_commands.json` que faltan (l. 306-321, 378-384). Ya pasó: en `98af5a8` fallaban todas las unidades y el árbol "pasaba". Tampoco imprime cuántas unidades escaneó | Un fallo de preprocesado o un preset que falta es FAIL; imprimir unidades escaneadas y fallidas por preset |
| M1 | Mayor | **La expresión de T2b no ve formas comunes de escritura:**<br>• `((GPIOD)->MODER) = …`;<br>• las expansiones de `WRITE_REG`/`MODIFY_REG`/`SET_BIT`/`CLEAR_BIT` (`((x->PUPDR) |= …)`);<br>• `AFR[position >> 3U]`;<br>• `<<=`;<br>• las sentencias partidas en líneas.<br>El control de `07db657` solo contiene la forma directa, así que no lo puede detectar. Además, el control acepta cualquier `main.c` (también el de CM7) y no comprueba el preset | Expresión sobre sentencias unidas que cubra esas formas; casos de macro en el control; control por (preset, ruta relativa) con los hallazgos impresos; excepciones por ruta relativa, no por nombre |
| M2 | Mayor (error del plan, mío) | **H1 y H5 rompen T15 de `selftest-gpio`.** Las fases (d) ("both, gpio_bm **without hook**", espera corrupciones > 0: `CM7/App/Src/gpio_selftest.c:724-728`, `CM4/App/Src/gpio_selftest.c:847-852`) y (g) ("gpio_bm, **no hook**", CM7 l. 763) contaban con que no hubiera hook. Ahora el hook de producción está instalado desde el arranque en los dos núcleos (H1), y tras (e) se reinstala en lugar de `NULL` (H5). La fase (d) correría protegida y su control fallaría. No se ve porque `selftest-gpio` solo se compila (G-S1) | `GPIO_BM_SetSharedLock(NULL)` explícito antes de (d) en los dos núcleos y antes de (g) en CM7; `Shared_GpioLockInstall()` al terminar el selftest. Verificado por revisión; la ejecución queda en "no ejercitado" |
| M3 | Mayor | **Capturas de árbol sucio e imágenes `-dirty`:** T3, T4 y T6a dicen "tree dirty"; los banners de T5 release y T7 dicen `Rev 0f2f7b1-dirty`, aunque el veredicto imprime "Banner Rev 0f2f7b1: PASS" y el informe dice `0f2f7b1`. La herramienta no dice qué estaba sucio | Repetir desde el árbol limpio; con el árbol sucio, imprimir `git status --porcelain` |
| M4 | Mayor | **T5 y T7 salen de un arnés que no está en el repo,** y la captura de T5 dice "Starting monitor.py… Resetting target via SWD": un reset con `monitor.py` corriendo, contra G-S5. El informe no lo menciona | Commitear el arnés; resetear solo con las herramientas del host paradas (el banner se lee arrancando `monitor.py` después del flasheo y antes de que el banner salga, o se omite y la identidad la da B6) |
| M5 | Mayor | `--test all` no ejecuta T2b (l. 776-787) | Que `all` expanda como `t2` |
| M6 | Mayor | El informe §5.8.3 dice que la placa quedó con `prj_04` debug `0f2f7b1`, pero la última captura (T7, 12:26) flasheó `prj_01` y ninguna posterior lo muestra | Evidencia o corrección. (Ahora la placa tiene `prj_04` debug de `63a746c`, flasheado por mí) |
| m1 | Menor | Informe §5.4.1, la tabla que pide el plan:<br>• `LED_BM_Init` no usa `GPIO_BM_ConfigOutput`: `led_bm.c:66` escribe `MODER` con `MODIFY_REG`, antes de liberar a CPU2, y `LED_PanicRed` (l. 146) es G14;<br>• la MSP de la consola corre **después** de liberar a CPU2 (`main.c:226` libera y `app_main` en l. 241 llama a `App_Log_Init`), no antes. Es la razón de H3 en `prj_04` | Corregir la tabla |
| m2 | Menor | T1 sin captura: el informe cita "task-1297", que no está en el repo. Cada número tiene que salir de una captura o de un `.map`. Además, §5.2.1 y §5.2.2 dan dos `.text` distintos para `prj_04` release CM7 (86524 y 86532) | Captura de T1 y una sola cifra con su fuente |
| m3 | Menor | `App_Log_Init` de `prj_04` descarta el resultado de `UART_Console_Init` (`(void)`): si `GPIO_HAL_Init` falla en la MSP, nada lo muestra. La mitigación del plan ("el LED de fallo lo muestra") era falsa para `prj_04` (errata mía) | Nada en F1b: `App_Log_Init` está fuera de alcance; se apunta para el README de `prj_04` en el cierre |
| m4 | Menor | Herramienta:<br>• las direcciones del bloque compartido (`0x4FC`…`0x508`, correctas) y los límites de T4 (24000/12000, correctos) están escritos a mano;<br>• T2b preprocesa con el primer `gcc` que encuentra en lugar del del comando, y parte los argumentos con `split()`;<br>• T6a no dice qué caso falló, y una excepción de SWD aborta todo;<br>• T3 mira solo modo y pull en PB1/PB15/PD0;<br>• el control se marca "dirty" cuando lo que falla es `git status` (el submódulo de `prj_eth_lwip_node`) | Derivar del ELF (`nm g_shared`) y del header; el compilador del comando y `shlex`; `--ignore-submodules=all` |
| m5 | Menor | El control de T4 es la misma aritmética con otro límite; cumple la letra del plan, pero es débil | Aceptado |

**Observación:** la base mal puesta del HSEM (B1) evitó un daño real. Con la dirección buena, la lectura de `RLR5` que pedía el plan habría tomado el semáforo desde el depurador, y cada configuración de pines posterior habría devuelto `ERR_BUSY` hasta un reset.

### Estado

**F1 no pasa.** El código de producción es correcto y mi ejecución lo muestra. Pero la evidencia no sostiene sus PASS (B1–B7, M1, M3–M6), y `selftest-gpio` queda roto (M2).

**Con la revisión 3 del plan:**
- corregir la herramienta (B1, B2, B4, B5, B7, M1, M5, m4);
- implementar el criterio nuevo de T2a (B3);
- arreglar T15 en `selftest-gpio` (M2);
- commitear el arnés de T5/T7 (M4);
- repetir T2–T5, T6a y T7 desde el árbol limpio, con la identidad de cada imagen en la captura (B6, M3);
- corregir el informe (m1, m2, M6, §5.5).

Las capturas de `0f2f7b1` se quedan (no se editan); el informe dice cuáles sustituyen las nuevas. T6b sigue pendiente del responsable en el banco.

## F1, corrección (`32f343a`)

Entrega: `198be5b..32f343a`. Selftest en `5a8f13a`; herramienta en `79ef531`, `03d4e54`, `9125d64`, `8444cb3`, `8ee9a13`; capturas de `8ee9a13` en `6f49e8e`; informe en `32f343a`. El código de producción no cambia desde `63a746c`.

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| `build_all -Clean` en `32f343a`, árbol limpio (mi ejecución) | 8/8, tamaños iguales a los de T1; `selftest-gpio` de `prj_04`: exit 0, 0 avisos |
| SWD independiente (`gpio_f1b_audit2_swd_32f343a.txt`) | `prj_04` debug compilado en `32f343a` y flasheado ("Download verified successfully"). Los dos hooks instalados, `HSEM_R5` = 0, contadores a 0, retención 1228/356 ciclos (19,5×/33,7×), pines y controles exactos. **PASS** |
| T6a independiente (`gpio_f1b_audit2_t6a_32f343a.txt`) | 20 × (`-rst` por SWD, 10 s, mi lectura completa): **20/20 PASS**. La retención cambia entre 1226 y 1228 ciclos de un arranque a otro: `Shared_Init` la pone a cero en cada arranque, así que hubo reinicio |
| T5 independiente (`gpio_f1b_audit2_t5_netcheck_32f343a.txt`, `…_query_32f343a.txt`) | Misma imagen, tras los 20 resets: ICMP 4/4, UDP de 1 a 1472 B idénticos, ráfaga 100/100, contadores de red a 0; `query_adc.py`: las muestras crecen, `OVR: 0`. **PASS** |
| `grep` de escrituras con macro (`MODIFY_REG`/`WRITE_REG`/`SET_BIT`/`CLEAR_BIT` sobre `MODER`/`OTYPER`/`OSPEEDR`/`PUPDR`/`AFR`) en `01_shared_drivers` y en las fuentes de `prj_04` | Solo `led_bm.c:66` y `:146`, que están en la lista de excepciones (`LED_Init` antes de liberar a CPU2; G14). Es la comprobación que T2b todavía no hace (M1, abajo) |
| `py tools/gpio_f1b_checks.py --test t2 --control-dir ../stm32h755_control_07db657` en `32f343a`, árbol limpio (mi ejecución, `gpio_f1b_audit2_t2_32f343a.txt`) | Reproduce la captura de `8ee9a13`: T2a PASS con su control fallando en las cinco CM7; T2b 1951 unidades, 0 fallidas, 0 escrituras; control con 16 hallazgos. Con los límites de M1 y B7 de abajo |
| Revisión de la herramienta y de las capturas | Un subagente la revisó de nuevo; los hallazgos de abajo los comprobé yo en el código (`gpio_f1b_checks.py:860`, `:1076`, `:1090-1096`, `:1220`, `:466-472`) |

### Los hallazgos de la primera entrega

| ID | Estado |
| --- | --- |
| M2 | **Cerrado.** `GPIO_BM_SetSharedLock(NULL)` antes de T15 (d) en los dos núcleos y antes de (g) en CM7. `b1_measure` vuelve a dejar `NULL` como antes. `Shared_GpioLockInstall()` al final del selftest y en todos los caminos de fallo: `goto test_failed` en CM7 y `cm4_fail` en CM4, y no hay otro `return`. Verificado por revisión y build; sin ejecutar (G-S1) |
| B1 | **Cerrado.** `HSEM_R5` en `0x58026414` (`gpio_f1b_checks.py:73-74`); ninguna lectura cae en `0x58026480…0x580264FC` (`RLR`) |
| B2, B3, B4 | **Cerrados.** Siete ELF con sus rutas reales; los objetos enlazados salen de `build.ninja` (no del `.map`, como dice el informe); FAIL si falta una ELF o un objeto. El control falla en las cinco CM7 de `07db657`. **Desviación no declarada:** los objetos `stm32h7xx_hal_*` se filtran porque `stm32h7xx_hal_rcc.c.obj` referencia `HAL_GPIO_Init` (por `HAL_RCC_MCOConfig`, que no está enlazada en ninguna de las siete ELF; en las cinco CM7 el único llamador es `apply` de `gpio_hal.c`, comprobado con `objdump`). Aceptable, pero tiene que estar en el informe |
| B5, B6 | **Cerrados en las capturas.** Cada sesión de placa flashea su ELF con "Download verified successfully". El control de T3 flasheó las ELF de `07db657` y leyó 0 en las direcciones de `nm` de esas ELF (`0x20012904`, `0x30020928`). La herramienta tiene un hueco nuevo: N4 |
| B7 | **Parcial.** En el árbol actual, un fallo de preprocesado o un preset que falta es FAIL y se imprimen las cuentas (1951 unidades, 0 fallidas). En el control, `ctrl_ok` se calcula y no se usa (l. 634-649) y sus cuentas no se imprimen: en `07db657` faltan `selftest-adc` y `selftest-hsem` y el control da PASS igual |
| M1 | **Parcial; lo que dice el informe es falso.** Ahora detecta `->MODER =`, `AFR[expr] |=`, `<<=` y sentencias en varias líneas. **No detecta** `((X)->MODER) =` con la base expandida del CMSIS ni las expansiones de `WRITE_REG`/`MODIFY_REG`/`SET_BIT`/`CLEAR_BIT`, que dan `(((x->MODER)) = …)`. `PAREN_WRITE_RE` (l. 470-472) solo acepta un identificador o un nivel de paréntesis como base y un solo `)` antes del `=`. Además, `== ` cuenta como escritura (falso positivo, seguro). El control sigue comparando por nombre y "cualquier preset" (`any(name == "main.c")`, l. 636-639), sin casos de macro y sin imprimir los hallazgos. La lista de excepciones añade `gpio_bm.h`, `gpio_ll.h`, `led_bm.h` y `stm32h7xx_hal_gpio.*`, que el plan no incluye. Hoy no se escapa nada (mi `grep`, arriba), pero el "0 hallazgos" de T2b no lo demuestra |
| M3 | **Cerrado.** Capturas de `8ee9a13` con el árbol limpio; con el árbol sucio imprime `--porcelain`. El `-uno` es aceptable: el único `file(GLOB)` de los proyectos recorre el submódulo de Cube |
| M4 | **Parcial.** El arnés está commiteado y nunca resetea con una herramienta corriendo. T5 es secuencial (monitor, `net_check`, `query_adc`, monitor) y no "a la vez": es una desviación aceptable, pero sin declarar. Sus comprobaciones son vacías (N2, N3) |
| M5 | **Cerrado** |
| M6 | **No cerrado.** El informe §5.8 dice que se reflasheó `prj_04` y que `net_check` dio 3/3 tras T7, pero no hay captura de eso. (La placa tiene ahora `prj_04` debug de `32f343a`, flasheado por mí) |
| m1 | **Cerrado** |
| m2 | **Parcial.** `gpio_f1b_t1_8ee9a13.txt` son dos escritores en un archivo: una cabecera UTF-8 con BOM y una tabla en UTF-16LE (git lo trata como binario). No tiene la línea de comando ni las compilaciones con `-Werror` que pide T1. Las cifras coinciden con las mías |
| m3, m5 | Sin cambios, como se acordó |
| m4 | **Casi cerrado.** Desplazamientos de DWARF y límites del header, con un respaldo silencioso a los valores escritos a mano. En el modo control, la captura dice "from DWARF" para un campo que la ELF de `07db657` no tiene |

### Hallazgos nuevos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| N1 | Mayor | **T3 no puede fallar por PD8:** `gpio_f1b_checks.py:860` comprueba `pb1_ok` en lugar de `pd8_ok`. Un PD8 equivocado imprime FAIL pero el veredicto de T3 y el de cada reset de T6a quedan en PASS. Las capturas no cambian: PD8 es exacto en T3 y en los 20 resets de T6a | `pd8_ok` |
| N2 | Mayor | **El control del ADC de T5 es vacío:** `query_ok` (l. 1076) acepta `"0" in stdout`, que siempre se cumple. No comprueba que `Samples` crezca, y `dma_error_count`/`half_overrun_count` ni se miran (lo debilitó `8444cb3`) | Comparar dos lecturas de `Samples` y exigir `OVR: 0`; si `query_adc.py` no da los otros contadores, decirlo |
| N3 | Mayor | **Otras dos comprobaciones siempre dan PASS:**<br>• contadores de T5 (l. 1090-1096): pasa si no aparece `dma_err`; no mira `rx_nobuf`, `tx_nobuf` ni `acl_drop`; no afecta a `results["pass"]`;<br>• banner de T7 (l. 1220): `banner_found or has_download_ok`, verdadero siempre que el flasheo vaya bien (lo debilitó `8ee9a13`). El build id que pide el plan no se comprueba.<br>A mano, los valores de las capturas son correctos | Parsear las líneas `[net]` y exigir los cuatro a 0 en todas, contando en el veredicto. T7: comprobar la identidad sin el banner (B6 ya lo cubre: decirlo) o leer el build id; no fingir que se leyó |
| N4 | Mayor | **El control de T3 puede pasar sin flasheo:** el modo `--control` no exige `--flash` (l. 675, 736-745). En la imagen actual, `0x20012904` y `0x30020928` son `s_section_depth` (0 en reposo), así que `--test t3 --control` sin `--flash` pasa sobre la imagen de producción | `--control` implica `--flash` (o lo exige) |
| N5 | Menor | T3 lee justo después del flasheo, sin espera (T4 y T6a sí esperan). Solo puede dar un FAIL falso | Una espera como en T4 |
| N6 | Menor | T2b sin directorio de control no añade el caso de control y pasa (l. 632) | Sin control, FAIL |
| N7 | Menor | El informe: §5.5 no declara el filtro de la HAL de ST ni T5 secuencial; dice que los objetos salen "del .map"; §5.6 M1 afirma una cobertura que no existe | Corregir |

(El subagente dio además las líneas `[ipc]` duplicadas en `gpio_f1b_t5_release_8ee9a13.txt`: no lo están. Hay cinco, de dos lecturas distintas de `monitor.py`. Descartado.)

### Estado

**F1 no pasa todavía, pero queda poco.** El código de producción y `selftest-gpio` están bien. Mi evidencia independiente lo muestra en tres frentes: SWD, 20 resets y T5. Lo que falta es que la herramienta pueda fallar donde dice que comprueba: M1, N1–N4 y B7 en el control. También falta que el informe diga la verdad: M1, N7, M6 y m2. La herramienta es parte de la entrega y se reutiliza en F1c.

**Qué repetir tras la corrección:**
- T2 (sin placa), porque cambia T2b;
- T3 y su control, T5 debug y release, y T7, porque cambian sus comprobaciones;
- T1, con una sola salida legible que incluya las compilaciones con `-Werror`.

La T6a de `8ee9a13` se queda: N1 no cambia sus valores, y PD8 es exacto en las 20. También mi T6a de `32f343a`. T6b sigue pendiente del responsable en el banco.
