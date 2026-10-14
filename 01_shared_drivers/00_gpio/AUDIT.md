# Auditoría de `00_gpio` F1c (G1 en `prj_03`)

Auditor: Claude (Claude Code), para Hector Rodriguez · Rama: `driver/gpio-f1c` · Plan: `IMPLEMENTATION_PLAN.md` revisión 1

Este archivo lo escribe solo el auditor. Un hallazgo se cierra cuando el auditor lo verifica.

## F1 (`7ba2cf0`)

Entrega: `0244b31..7ba2cf0`. H1 en `90a866f`, H2 en `ee5e3b3`, la herramienta en `f733b98`, el informe y las capturas en `7ba2cf0`.

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| Alcance | Solo `prj_03` (`Common/shared_memory.[ch]`, los dos `main.c`, los dos `CMakeLists.txt`, `adc_acquisition.c`), `tools/gpio_f1b_checks.py`, capturas e informe. Nada en `01_shared_drivers` ni en el arranque de CPU2 |
| Revisión del firmware | Cumple el plan:<br>• el hook es el de `prj_04`, con los campos al final de `SharedMemory_t` (`aligned(32)`);<br>• `HSEM_BM_Init` y `HSEM_BM_TimebaseStart` se llaman con el retorno comprobado: en CM7 tras `Shared_Init` y antes de liberar a CPU2, y en CM4 tras `HAL_Init` y antes de los selftests y del ADC;<br>• `stm32h7_driver_hsem_bm` está enlazado en los dos núcleos;<br>• PB1 se configura por `GPIO_BM_ConfigPin`, y su fallo propaga a `ADC_Acquisition_Init`;<br>• el arranque por `HAL_HSEM`, sin tocar.<br>El único cambio colateral: el BOM de `adc_acquisition.c` desaparece (inocuo) |
| `build_all -Clean` en `7ba2cf0`, árbol limpio (mi ejecución) | 8/8. `prj_03` `selftest`: exit 0, 0 avisos. `prj_03` debug y release con `-Werror`: 0 avisos |
| T2 (`--project prj_03 --test t2`, `gpio_f1c_audit_t2_7ba2cf0.txt`) | **T2a PASS:** las cuatro ELF; el control falla en las dos CM4 de `e6cb75e`. **T2b PASS:** autoprueba 12/12 y 0/9; 323/323/324 unidades, 0 fallidas, 0 escrituras |
| T7 (`--project prj_04 --test t2 --control-dir ../stm32h755_control_07db657`, `prj_04/docs/gpio_f1c_audit_t7_7ba2cf0.txt`) | T2a PASS (control 5/5 CM7); T2b: autoprueba 12/12 y 0/9; 1951 unidades, 0 fallidas, 0 escrituras; control con 1951 unidades y 20 hallazgos. Los mismos números que F1b (`gpio_f1b_t2_7cc456d.txt`), con las rutas y líneas corregidas. **PASS** |
| SWD independiente, con mi propio script (`gpio_f1c_audit_swd_7ba2cf0.txt`) | `prj_03` debug compilado en `7ba2cf0` y flasheado ("Download verified successfully"). Desplazamientos del bloque compartido sacados del DWARF de la ELF (comprobado antes contra los conocidos de `prj_04`). Resultados:<br>• hooks de CM7 y CM4 instalados;<br>• `HSEM_R5` = 0;<br>• contadores de fallo a 0;<br>• retención: CM7 1274 ciclos (3,19 µs, 18,8×), CM4 363 (1,81 µs, 33,1×);<br>• PB1, PB13, PD8 y PD9 exactos, y los controles PB15 y PD0 en reset.<br>**PASS** |
| T6a independiente (`gpio_f1c_audit_t6a_7ba2cf0.txt`) | 20 × (`-rst`, 10 s, mi lectura completa): **20/20 PASS** |
| T5 independiente (`gpio_f1c_audit_t5_netcheck_7ba2cf0.txt`, `…_t5_query_…`) | Tras los 20 resets: ICMP 4/4, UDP de 1 a 1472 B, ráfaga 100/100, `[net]` a 0; las muestras crecen y `OVR: 0`. **PASS** |
| T3 de la herramienta, con su control (`gpio_f1c_audit_t3_control_7ba2cf0.txt`, `gpio_f1c_audit_t3_7ba2cf0.txt`) | **Control:** flasheó `e6cb75e` y leyó `s_shared_lock` = 0 en `0x200128F4`/`0x30020A54`, las direcciones de `nm` de las ELF de control. **Producción:** reflasheada, los diez casos PASS. La placa queda con `prj_03` debug de `7ba2cf0` |
| Revisión de la herramienta y de las capturas | Un subagente la revisó línea a línea; los hallazgos de abajo los comprobé yo |

**El código cumple el plan y la herramienta comprueba lo que dice.** Para `prj_04`, ninguna comprobación se debilita, y los tres menores de F1b están corregidos: `g_shared` sale de `nm` sin respaldo, el control se compara por ruta relativa, y las rutas y líneas del control son exactas (comprobadas contra las fuentes de `07db657`). Mis ejecuciones coinciden con las del implementador. Lo que falla está en una desviación no declarada de la herramienta y en el informe.

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| M1 | Mayor (desviación no declarada) | **`--project` tiene `prj_03` por defecto** (`gpio_f1b_checks.py:1611-1612`). El plan decía `prj_04` "que tiene que seguir funcionando igual" (Diseño 4). El informe §5.5 dice "Sin desviaciones". Las líneas de comando de las capturas de F1b (p. ej. `--test t2 --control-dir ../stm32h755_control_07db657`) corren ahora sobre `prj_03` sin aviso, y la herramienta no comprueba que el directorio de control sea del proyecto elegido | `--project` obligatorio, sin valor por defecto. Comprobar que el commit del control coincide con el del proyecto elegido (hoy se imprime y no se compara); FAIL si no |
| M2 | Mayor (afirmación sin captura) | El informe §5.8.3 dice que se verificó `net_check` 3/3 "tras los 20 resets de T6a". Ninguna captura lo contiene; la de T6a no tiene `net_check`. Es la regla que las instrucciones ponen en §1 | Quitarlo o citar una captura. (Mi T5 tras mis 20 resets sí lo muestra: `gpio_f1c_audit_t5_netcheck_7ba2cf0.txt`) |
| M3 | Mayor (el mismo error que m1 de F1b) | La tabla de escritores §5.4.1:<br>• dice que las MSP de la consola y de Ethernet corren "Antes de liberar CPU2": corren después (`app_main()` en `main.c:123`, después de la liberación en l. 110-111). Es la razón de G1;<br>• llama `BSP_LED_Init()` a `LED_Init()` (`main.c:46`);<br>• omite `LED_PanicRed` (G14) y el escritor del preset `selftest` (`ADC_SelfTestHw_Run`, por los drivers de `07_adc`) | Corregir la tabla |
| m1 | Menor | La salida de T5 dice que `query_adc.py` "does not output … half_overrun_count" (l. 1376). Es falso en `prj_03`: su `OVR` es `half_overrun_count` (`adc_server.c:138`). Tampoco dice que `[net]` de `prj_03` no tiene `acl_drop`. Las dos cosas las pedían las instrucciones §3.5 | Una nota por proyecto, verdadera |
| m2 | Menor | La captura de T3 son tres ejecuciones pegadas con cabeceras "=== PART n ===" escritas a mano. Las instrucciones §3.4.1 lo prohíben | Una captura por ejecución |
| m3 | Menor | T1:<br>• no imprime los comandos de los pasos 2 y 3 ni muestra `-Werror`;<br>• `selftest` y `prj_04` dicen "ninja: no work to do": sus objetos eran anteriores a `f733b98`, así que T1 no compiló esos presets en esa ejecución | Compilación limpia (`--fresh` o borrar el build) de cada preset, con los comandos |
| m4 | Menor | T4 imprime las direcciones de la retención sin su origen | Origen: `nm` de `g_shared` + DWARF |
| m5 | Menor | Herramienta:<br>• la tabla por proyecto no lleva los pines de T3 ni el formato de `query_adc` (están escritos a mano, iguales para los dos; correctos hoy);<br>• el caso "autoprueba_gate" del control de T2b en `prj_03` repite `st_pass`;<br>• los casos "status" del control de T2a no cuentan en el veredicto;<br>• el docstring del módulo sigue describiendo F1b | Con M1, o en la siguiente vez que se toque |
| m6 | Menor | Informe:<br>• dice que las rutas usan `Path.relative_to` (la herramienta usa una expresión regular);<br>• la fila T3 da las direcciones de `s_shared_lock` como valores leídos (son `0x8015B80` y `0x81052F0`) | Corregir |
| — | Información | `query_adc.py` imprime `CH351272992`: es `ADC_CHANNEL_5` codificado por la HAL (`0x14F00020`), anterior a F1c (`a2ef0fc`). No es de esta fase | Nada |

### Estado

**F1 pasa, con condiciones.**
- **Por qué pasa:**
  - el código de producción es correcto y está completo;
  - cada comprobación de placa y de enlace está respaldada por capturas válidas y por mis ejecuciones independientes (SWD, 20 resets, T5, T2 y T3 con su control);
  - ninguna comprobación de la herramienta es vacía.
- **Qué queda para el cierre:** M1–M3 y m1–m6 se corrigen con la entrega del cierre (un commit de la herramienta y el informe), y se repiten T2 de `prj_03` y T7, sin placa.
- **El cierre necesita además T6b** (arranque en frío), con el responsable en el banco.

## Correcciones de F1 y cierre, parte 1 (`7b56d99`)

Entrega: `a20e27c..7b56d99`. Herramienta en `e9e98c4`; informe, capturas de `e9e98c4` (T1, T2, T7) y README en `7b56d99`. El firmware no cambia. El plan y `AUDIT.md` no se tocaron (comprobado en los dos commits).

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| `py tools/gpio_f1b_checks.py --test t2` (sin `--project`) | Sale con código 2: "the following arguments are required: --project". M1 |
| `--project prj_03 --test t2` en `7b56d99`, árbol limpio (`gpio_f1c_audit2_t2_7b56d99.txt`) | `T2a_control_e6cb75e_commit_matches` PASS; T2a PASS (control 2/2 CM4); T2b: autoprueba 12/12 y 0/9; 323/323/324 unidades, 0 fallidas, 0 escrituras. **PASS** |
| `--project prj_04 --test t2 --control-dir ../stm32h755_control_07db657` (T7, `prj_04/docs/gpio_f1c_audit2_t7_7b56d99.txt`) | Comprobación del commit del control PASS en T2a y T2b; T2a 7/7 con control 5/5 CM7; T2b: autoprueba 12/12 y 0/9; 1951 unidades, 0 escrituras; control con 1951 unidades y 20 hallazgos. **PASS** |
| `--project prj_03 --test t2 --control-dir ../stm32h755_control_07db657`: un control que no es del proyecto (`gpio_f1c_audit2_wrongctrl_7b56d99.txt`) | `T2a_control_e6cb75e_commit_matches` **FAIL** ("Expected e6cb75e … found 07db657"), veredicto final FAIL, código de salida 1: la comprobación de M1 falla de verdad |
| Revisión de `e9e98c4` y de las capturas | Un subagente la revisó línea a línea; lo de abajo lo comprobé yo |

### Hallazgos de F1

| ID | Estado |
| --- | --- |
| M1 | **Cerrado.**<br>• `--project` obligatorio (l. 1631).<br>• El commit del control se compara con el del proyecto en T2a, T2b, el control de T3 y T7; si no coincide, FAIL.<br>• Si falta el directorio de control, FAIL.<br>• No queda ningún camino que use el árbol actual en lugar del control.<br>Queda un residuo menor: `run_t7` elige el directorio de `07db657` por subcadena y con una ruta escrita a mano (l. 1531). Es seguro, porque la comprobación del commit sigue corriendo |
| M2 | **Cerrado.** §5.8 cita mi `gpio_f1c_audit_t5_netcheck_7ba2cf0.txt` |
| M3 | **Cerrado, salvo dos detalles:**<br>• `LED_PanicRed` no está en `main.c:134`, sino en `Error_Handler` (`main.c:282`) y en los manejadores de fallo de `stm32h7xx_it.c`;<br>• el escritor del preset `selftest` solo toca PB1, no PA3 |
| m1 | **Cerrado.** Las notas por proyecto de T5 son verdaderas: en los dos proyectos `OVR` es `half_overrun_count`, y `prj_03` no tiene `acl_drop`. No hay captura, porque T5 no se repitió |
| m2 | **No cerrado, y el informe dice lo contrario.** §5.6 afirma que "todas las capturas se generan … sin concatenación manual". `gpio_f1c_t3_f733b98.txt` sigue siendo tres ejecuciones pegadas (`=== PART 1/2/3 ===`, l. 1, 144, 267), y el informe y los dos README la citan como la T3 oficial |
| m3 | **Cerrado.** T1 de `e9e98c4` compila de verdad cada preset (`-Clean` y `--fresh`, sin "no work to do"), en UTF-8, con `-Werror` en `prj_03` debug y release. Le falta la línea de comando del arnés (no está en el repo) |
| m4, m5 | **Cerrados.** T4 imprime el origen de cada dirección. Los pines de T3 y el comando de `query_adc` van en la tabla por proyecto, sin cambiar los valores. Se quitó el caso repetido del control de T2b. Los casos "status" del control de T2a cuentan en el veredicto |
| m6 | **Valores bien, redacción mal:** llama "punteros de función" a direcciones de un descriptor `GpioBm_SharedLock_t` |

Sin regresiones de `f733b98` a `e9e98c4`: ninguna comprobación se debilita, ninguna excepción se traga y no hay nuevos respaldos escritos a mano.

### Hallazgos del cierre

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| K1 | Mayor | **`e9e98c4` reescribe T3 (los pines en tabla, el bucle), T4 (la impresión) y T5 (nota y comando), y nada de eso se ha ejecutado en la placa.** Las T3, T4 y T5 citadas son de `f733b98`, con el código anterior | Repetir, con la herramienta de este commit y desde el árbol limpio, T3, T3 `--control`, T4 y T5 debug de `prj_03`, **una captura por ejecución**. Eso cierra también m2. T6a no hace falta |
| K2 | Mayor (referencias falsas en los README) | **`00_gpio` §13.2 y `prj_03` §7:**<br>• citan `…_t3_control_…` del implementador, que no existe (su control está dentro de `gpio_f1c_t3_f733b98.txt`; la única `t3_control` es mía);<br>• los dos dicen que T2b tiene un "negative control in `e6cb75e` confirmed": en `prj_03` T2b no tiene control de árbol (la captura lo dice; el control es la autoprueba y `e6cb75e` lo cubre T2a);<br>• `prj_03` §7 sitúa la instalación en `CM7/Core/Src/main.c:103-107` y `CM4/Core/Src/main.c:48-52` (son l. 97-105 y 70-78) y cita `HAL_HSEM_Release(HSEM_ID_CPU2_BOOT)`, un símbolo que no existe (es `HSEM_ID_BOOT_SYNC`, l. 111) | Corregir. Con K1, citar las capturas nuevas |
| k1 | Menor | `00_gpio` §12.2 y §13.2 citan las capturas de `prj_03` como `docs/…` sin decir de qué proyecto (las de `prj_04` usan la misma forma) | Ruta desde la raíz del repo para las de F1c |
| k2 | Menor | Informe:<br>• m2 y m6 (arriba);<br>• §5.2.2 dice `-Os` (es `-O2`) y nombra `s_hook_installed` (es `s_gpio_lock_start_cyc`);<br>• la fila T4 llama "g_shared base" a las direcciones de los campos;<br>• la fila T3 menciona `pd8_ok`, que ya no existe;<br>• cita mal el asunto de `e9e98c4`;<br>• dice que M1 "aborta inmediatamente" (la prueba afectada falla y la ejecución sigue) | Corregir |

### Estado

**Las correcciones de F1 pasan**, salvo m2, que se cierra con K1.

**El cierre no pasa:**
- K1: las pruebas de placa no se han repetido con la herramienta reescrita;
- K2: los README contienen referencias falsas;
- T6b sigue pendiente del responsable en el banco.

La placa tiene `prj_03` debug de `7ba2cf0`, de mi T3. El firmware es el mismo que en `7b56d99`.

## Cierre, parte 2 (`02b1a7f`)

Entrega: `c9a3d23..02b1a7f`. Capturas de placa nuevas de `c9a3d23` (T3, el control de T3, T4, T5 debug), los dos README y el informe. Ni firmware ni herramienta.

| Hallazgo | Verificación |
| --- | --- |
| K1 (y m2) | Cuatro capturas, cada una de una sola ejecución, con el árbol limpio en `c9a3d23` y su "Download verified successfully":<br>• `gpio_f1c_t3_c9a3d23.txt`: diez casos PASS con la tabla de pines por proyecto;<br>• `gpio_f1c_t3_control_c9a3d23.txt` (`--control`): flasheó las ELF de `e6cb75e`, leyó `s_shared_lock` en `0x200128F4`/`0x30020A54` (`nm` de esas ELF), NULL en los dos;<br>• `gpio_f1c_t4_c9a3d23.txt`: 1274/363 ciclos, 18,84×/33,06×, con cada dirección y su origen (`g_shared` de `nm` + desplazamiento de DWARF);<br>• `gpio_f1c_t5_debug_c9a3d23.txt`: red, ADC (las muestras crecen, `OVR` [0,0,0]) y `[net]` a 0.<br>El código reescrito de T3, T4 y T5 ya corrió en la placa. **Cerrado** |
| K2 | `00_gpio` §12.2/§13.2 y `prj_03` §7:<br>• citan las capturas nuevas y las mías por su nombre; ninguna referencia a `…_t3_control_…` inexistente ni a la T3 pegada;<br>• T2b de `prj_03` explicado bien (la autoprueba es el control, y `e6cb75e` lo cubre T2a);<br>• la liberación con `HAL_HSEM_Release(HSEM_ID_BOOT_SYNC, 0)` y sin números de línea.<br>**Cerrado** |
| k1 | Las capturas de F1c en `00_gpio`, con ruta desde la raíz del repo. **Cerrado** |
| k2 | El informe, corregido (m2, m6, §5.2.2, las filas T3/T4, el asunto de `e9e98c4`, M1, M3). **Cerrado** |

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| k3 | Menor (cosmético) | La frase de T2b quedó en castellano dentro de dos README en inglés (`00_gpio/README.md` l. 1134, `prj_03/README.md` l. 138) | La traduce el responsable en el commit de cierre, si lo aprueba |

**El cierre pasa en lo documental. Falta T6b** (arranque en frío), con el responsable en el banco. Después: k3, `DRIVER_ROADMAP.md`, borrado de los documentos de proceso y de los dos worktrees de control, merge.

## T6b, arranque en frío (2026-10-13, en `009e997`)

Las ejecuté yo, sin flashear ni resetear desde el encendido salvo donde se dice. La imagen es `prj_03` debug, la de `gpio_f1c_t5_debug_c9a3d23.txt` (el código de producción no cambió).

**Primer arranque en frío: anulado.** El responsable reconectó la placa:
- `gpio_f1c_t6b_rsr_009e997.txt`: `RCC_RSR = 0x00FE0000`;
- `…_swd_…`: PASS;
- `…_t3_…`: PASS.

T5, en cambio, salió errático (`…_t5_netcheck_009e997.txt`: UDP de 512 B o más sin respuesta, ráfaga 95/100 en 12 s; `…_2.txt`: ICMP 1/4, tiempos de 6 a 105 ms; el `ping` del sistema, 30 ms). Para separar la placa del entorno resetear por SWD (`-rst`): con la imagen en caliente siguió igual (`…_3_after_warm_reset.txt`: ICMP 3/4, 40 ms). El responsable informó de un problema de conexión en su lado. Una vez arreglado, la misma imagen pasó (`…_4_network_recovered.txt`: 3/3, 1–2 ms). Los fallos eran de la red del host, no de la placa: los contadores `rx_nobuf`, `tx_nobuf` y `dma_err` estuvieron a 0 en todas, y el ADC siguió bien (`…_t5_query_009e997.txt`). Como el reset en caliente anuló la condición de arranque en frío, se repitió todo.

**Segundo arranque en frío: el que cuenta.** El responsable desconectó y reconectó la placa otra vez.

| Captura | Resultado |
| --- | --- |
| `gpio_f1c_t6b2_rsr_009e997.txt` | `RCC_RSR = 0x00FE0000` (reset de encendido, sin el bit de reset por software), `PWR_CR3 = 0x00010044`, `RCC_CR = 0x0F07C1A5` |
| `gpio_f1c_t6b2_swd_009e997.txt` (mi script) | Hooks, `HSEM_R5` = 0, contadores a 0, retención 1274/363 ciclos (18,8×/33,1×), pines y controles exactos. **PASS** |
| `gpio_f1c_t6b2_t3_009e997.txt` (`gpio_f1b_checks.py --project prj_03 --test t3`, sin `--flash`) | **PASS** |
| `gpio_f1c_t6b2_t5_netcheck_009e997.txt`, `…_t5_query_…` | ICMP 4/4, UDP de 1 a 1472 B (1,2–2,2 ms), ráfaga 100/100, `[net]` a 0; las muestras del ADC crecen, `OVR: 0`. **PASS** |

**T6b pasa. El cierre de F1c pasa.** Pasos del responsable (aprobados): k3, el estado de T6b en los README (de "pending" a hecho, con estas capturas), `DRIVER_ROADMAP.md`, el borrado de los documentos de proceso y de los dos worktrees de control, y el merge.
