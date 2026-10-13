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
