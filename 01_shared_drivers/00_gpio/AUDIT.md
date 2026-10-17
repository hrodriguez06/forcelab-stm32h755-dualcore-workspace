# Auditoría del cierre de `00_gpio` (K1–K5)

## F1 (`2f929a4`, informe en `791674b`)

### K1: el firmware

**Correcto.**
- Las tres `CompensationCell_Init()` son idénticas, y los tres diffs de `main.c` también, salvo el contexto de un `#include` en `prj_04`.
- `SystemClock_Config()` solo pierde las tres líneas y gana el comentario de `CSIState`.
- La llamada está entre `SystemClock_Config()` y `PeripheralClock_Config()`, y los pasos 1–4 siguen el Diseño 1.
- `__HAL_RCC_SYSCFG_CLK_ENABLE()` desaparece sin efecto: el driver enciende `SYSCFGEN` (`gpio_bm.h:416`), y `eth_hal.c:274` también.

### La herramienta y el informe

| ID | Gravedad | Hallazgo | Qué se pide |
| --- | --- | --- | --- |
| B1 | Bloqueante | **El "0 avisos" de la matriz no demuestra nada.** `build_all.ps1` guarda la salida de `cmake --build` en `$buildOut` (l. 124) y solo la imprime si falla, así que `parse_compiler_warnings()` nunca ve un aviso de esos 8 builds. El control de T1 compila un `.c` suelto con `gcc`: prueba el parser, no la tubería de la matriz | T1 compila ella misma las 8 configuraciones (`cmake --preset`, `--build --clean-first`, como `compile_single_config()`) y cuenta los avisos de esa salida. El control negativo pasa por **esa misma tubería**: un preset configurado con `-B` en un directorio temporal fuera del repo, con una flag que hace aparecer avisos reales en el código del proyecto (por ejemplo `-DCMAKE_C_FLAGS=-Wconversion`), evaluado por `evaluate_build_output()`, con avisos ≥ 1. Quitar `pass_count` |
| B2 | Bloqueante | **La herramienta cambia la configuración del repo.** `ensure_control_worktree()` (l. 268) ejecuta `git -C <control> config submodule.….ignore all`. Los worktrees comparten `.git/config`, así que el repo principal quedó ignorando ese submódulo. El informe (§5.1) dice "en el worktree de control". Lo deshice con `git config --unset` | Ninguna escritura de configuración. El estado del control ya usa `--ignore-submodules` (l. 245); si hace falta algo más, `git -c …` por comando. Corregir §5.1 |
| B3 | Bloqueante | **T4 no compila.** Con `--test t4`, la herramienta toma el SHA-256 de los ELF que haya en disco, sean de cuando sean. El informe (§4.2, paso 1) dice que los compila | T4 compila en la ejecución las 4 imágenes (`--clean-first`), comprueba que son recientes y registra el SHA-256 de los ELF de CM7 y CM4 |
| B4 | Bloqueante | **El flasheo de T4 falla siempre.** `flash.py` no tiene `--project` (argumentos en `tools/flash.py:410-433`), así que argparse sale con error. El informe (§4.2, paso 2) da otra línea de comandos | Como en `eth_e3e6_checks.py:1532-1533`: el `tools/flash.py` del proyecto, con `cwd` en el proyecto, `--preset` y `--sn`. La salida de `flash.py` va completa a la captura. El éxito exige código 0 y `Download verified successfully` (`eth_e3e6_audit_t5t6_1150083.txt:103`) |
| B5 | Bloqueante | **La espera de T4 depende de la latencia.** `time.sleep(3.0)` y un único `net_check`, cuando el enlace sube a los 3,2–3,3 s del reset (T6 de `08_eth`) | Tras flashear o resetear, `net_check` se reintenta hasta que pasa o hasta un plazo (20 s). Cada intento, con su resultado, va a la captura |
| B6 | Bloqueante | **Los resets no son por NRST.** `-rst` es un reset por software, no el pin NRST que pide el plan. Además, no se comprueba el código de salida y la captura solo da el total | `-hardRst`, con su código de salida comprobado. Por cada reset, `CCCSR`, `RCC_CR` y el resultado de `net_check` en la captura |
| B7 | Mayor | **Los controles de T4 solo corren en simulación.** El plan pide que pasen por la salida real del CLI. Además, no hay captura de la simulación, aunque el informe (§4.1) la da por verificada | En la ejecución con placa, el texto real de la salida del CLI, con el valor leído cambiado en memoria (`READY = 0`, y aparte `EN = 0`), pasa por `parse_swd_hotplug_output()` y `evaluate_t4_registers()` y da FAIL. La simulación se commitea como `gpio_close_t4sim_<hash>.txt`, con "SIMULATED" en la cabecera |
| M1 | Mayor | **T3 no usa la API real.** El `.c` temporal declara prototipos inventados: `GPIO_BM_ExtiConfig(uint32_t line, uint32_t port, uint32_t trigger)` frente a la firma de `gpio_bm.h`. Además, define tipos GPIO falsos. Solo demuestra que dos objetos enlazan | El `.c` incluye `gpio_hal.h` y `gpio_bm.h` reales y se compila con las flags, includes y defines de `gpio_hal.c.obj` sacados de `build.ninja`. Llama a `GPIO_HAL_Init`, `GPIO_BM_ExtiConfig` y `GPIO_BM_EnableCompensationCell` con sus firmas reales. Stubs solo para los símbolos que no son de `00_gpio` |
| m1 | Menor | T2 recorre solo los objetos de HEAD: si un objeto existe solo en el control, no lo detecta | Comparar los dos conjuntos de objetos |
| m2 | Menor | La mutación de T2 usa un offset fijo (`0x4f0`) | Sacar el offset del byte de `.text.main` del `File off` de `objdump -h` |
| m3 | Menor | **El informe no coincide con el código ni con los registros:**<br>• §4.2 dice `-r32 … 1`, y el código pone `4`;<br>• §4.2 dice que `CSIRDY` es el bit 9, y es el bit 8 (el código usa `0x100`, que es correcto);<br>• §4.2 dice "NRST", y el código usa `-rst`;<br>• los enlaces son `file:///c:/…` | Corregir, con enlaces relativos al repo, y comprobar cada cita contra HEAD |

**Lo que sí está bien:**
- **El comparador de T2:** ALLOC, contenido, relocalizaciones y símbolos, con el control compilado en la ejecución.
- **`-ffile-prefix-map`:** aplicado solo al control, para que `__FILE__` coincida, y declarado en la captura.
- **La normalización de `FW_GIT_REV` y `FW_BUILD_ID`:** limitada a `main.c.obj` y `app_main.c.obj`.
- **Los controles de símbolos y de texto de T2:** pasan por la misma función.
- **El árbol limpio:** se comprueba con los ignorados, comparados con la instantánea inicial.

### Estado

**F1 no pasa.** K1 está bien; la herramienta necesita una ronda: B1–B7, M1 y m1–m3.

## F1, corrección 1 (`5826eb2`, informe en `ac59a73`)

### La herramienta

Cerrados B1–B7, M1 y m1–m3:
- **T1** compila ella misma las 8 configuraciones. El control usa la misma tubería con `-Wconversion` en un directorio temporal y da 224 avisos.
- **`git config`:** ninguna escritura.
- **T2** compara los dos conjuntos de objetos en ambos sentidos.
- **T3** compila con las cabeceras, defines e includes reales de `gpio_hal.c.obj`.
- **T4:**
  - compila sus imágenes en la ejecución;
  - flashea con el `flash.py` de cada proyecto y exige `Download verified successfully`;
  - reintenta `net_check` hasta 20 s;
  - hace los resets con `-hardRst`, cada uno en la captura;
  - pasa los controles por la salida real del CLI.

**Un fallo más, que corregí yo en `987f310`:** `run_net_check_with_retry()` usaba `192.168.1.50`, y la placa está en `192.168.1.150` (`net_config.h` de los tres proyectos). Todos los `net_check` de T4 habrían fallado.

### Ejecución del auditor: T1–T4 en la placa

Captura `docs/gpio_close_audit_t1t4_987f310.txt`: HEAD `987f310`, SN `0016000C4142501220353451`, **`OVERALL VERDICT: PASS`**, 55 comprobaciones PASS y 0 FAIL.
- **T1:** 8/8 y los tres presets de selftest, con 0 avisos; el control, con 224.
- **T2:**
  - los 6 CM7 sin `HAL_EnableCompensationCell` y con `GPIO_BM_EnableCompensationCell` sin definir en `main.c.obj`;
  - las tres funciones idénticas;
  - 2454 objetos idénticos al control `f066a8e`, compilado en la ejecución;
  - los tres controles, rechazados.
- **T3:** enlaza; sin `gpio_bm`, falla por `undefined reference`.
- **T4:**

| Imagen | `SYSCFG_CCCSR` | `RCC_CR` | `net_check` |
| --- | --- | --- | --- |
| `prj_02` `debug` | `0x00000101` (EN, READY) | `0x0307C1A5` (CSIRDY) | PASS |
| `prj_03` `debug` | `0x00000101` | `0x0F07C1A5` | PASS |
| `prj_04` `debug` | `0x00000101` | `0x0F07C1A5` | PASS; 5/5 resets por NRST, cada uno con `READY = 1` y `net_check` PASS |
| `prj_04` `release` | `0x00000101` | `0x0F07C1A5` | PASS |

- **Controles de T4:** la salida real del CLI con `READY = 0`, y aparte con `EN = 0`, da FAIL.
- **El primer `net_check` falla en cada arranque y el segundo pasa.** Se pierde un ping (ICMP 3/4) mientras sube el enlace, que es lo que justifica los reintentos.
- **Los bits 26–27 de `RCC_CR`** (`PLL2ON`, `PLL2RDY`) solo están en `prj_03` y `prj_04`, que usan PLL2 para el ADC.

### Estado

**F1 pasa.** Queda autorizado el cierre del implementador (K3):
- el README según el Diseño 3 del plan;
- en el README, solo las capturas que respaldan una afirmación actual: `gpio_close_t1t3_2f929a4.txt` es de la herramienta con B1–B7 y se borra (regla 7 de §14);
- §9 y §13.4 citan `gpio_close_audit_t1t4_987f310.txt` para T4.
