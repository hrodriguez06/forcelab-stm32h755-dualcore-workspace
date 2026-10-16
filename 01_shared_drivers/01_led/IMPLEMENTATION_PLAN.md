# Plan de implementación de `01_led` L1/L2: macros de pines obligatorias

Oct 16, 2026 · @Hector · revisión 1

Cierra L1 y L2 de `01_led/README.md` §5:
- **L1:** `prj_02`, `prj_03` y `prj_04` definen `BOARD_LED_<x>_PIN` (máscaras `GPIO_PIN_x`), pero el driver lee `BOARD_LED_<x>_BIT` (número de bit) y, al no encontrarlo, usa en silencio sus pines por defecto.
- **L2:** esos pines por defecto (`#ifndef … #define`) van contra la regla 1 de `01_shared_drivers/README.md` ("never hardcode GPIO pins").

El arreglo es el mismo que E1 de `08_eth`: un `#error` por macro que falte. Los tres `board_def.h` pasan a `_BIT`, para que ningún proyecto deje de compilar. Además se comprueba en compilación que cada `_BIT` vale 0–15, lo que cierra L5 en el camino de `board_def.h`.

**La API no cambia:** `led_bm.h` queda igual. Los pines no cambian (PB0, PE1, PB14), así que el comportamiento de los cuatro proyectos tampoco debe cambiar, y eso se comprueba.

Una fase de implementación (F1) y un cierre. Base y control: `main` en `9dc6896`. Reglas para el implementador en [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md).

**El desarrollador es Antigravity.** No tiene acceso a la placa: la prueba en la placa (T4) la ejecuta el auditor con su herramienta.

**Decisiones del plan** (pendientes del responsable; ver [Decisiones abiertas](#decisiones-abiertas)):
1. Alcance: L1, L2 y la comprobación de rango de `_BIT` (L5 en `board_def.h`). L3, L4 y la comprobación de `pin_num` en `LED_Init_Config()` quedan fuera.
2. Prueba en la placa: lectura por SWD, sin AD2.
3. Documentos de proceso: se borran al cierre.

**Reglas de verificación (las de `00_gpio` F2):**
- Cada comprobación puede fallar de verdad.
- Cada control negativo pasa por la misma función que la comprobación real, sobre una copia temporal fuera del árbol.
- El control se compila dentro de la propia ejecución.
- Árbol limpio; capturas UTF-8 de una sola ejecución.
- Cada afirmación del informe, de una captura o de `archivo:línea` comprobado contra HEAD.

---

## Encargo

| ID | Hoy (`9dc6896`) | Qué cambia |
| --- | --- | --- |
| **H1** | `led_bm.c:16-35`: un `#ifndef BOARD_LED_<x>_PORT/_BIT` con `#define` por defecto para cada uno de los seis | Cada `#ifndef … #define` pasa a `#ifndef … #error "BOARD_LED_<x>_<y> must be defined in board_def.h (Rule 1: no hardcoded hardware in shared drivers)"`, como `08_eth/eth_hal.c:34-56`. Añade `#if (BOARD_LED_<x>_BIT) > 15U` → `#error`, para los tres. `#include "board_def.h"` deja de ser condicional (`__has_include`) |
| **H2** | `prj_02`, `prj_03` y `prj_04`: `Common/Inc/board_def.h:70-75` con `BOARD_LED_<x>_PIN GPIO_PIN_n` | `BOARD_LED_<x>_BIT` con los mismos números (0, 1, 14), y el comentario de `prj_01` (`board_def.h:50-53`) que explica que son números de bit, no máscaras. `_PIN` se quita, porque nadie más lo usa |
| **H3** | — | `tools/led_l1l2_checks.py` (nuevo): T1–T3 sin placa y T4 con la placa, para el auditor |
| **H4** | — | `IMPLEMENTATION_REPORT.md` |

**Fuera:**
- `prj_eth_lwip_node`, que tiene su propia copia en `Custom_Drivers/01_led/`;
- L3 (`MODER` fuera de `gpio_bm`) y L4 (`LED_Toggle` no atómico);
- la comprobación de `pin_num` en `LED_Init_Config()` (L5 para tablas pasadas a mano);
- `led_bm.h`.

---

## Lo que existe hoy

| Elemento | Estado |
| --- | --- |
| Dónde se compila | Solo en CM7, en los cuatro proyectos (`compile_commands.json` de `debug`). `prj_01` y `prj_04` lo enlazan con `stm32h7_driver_led`; `prj_02` y `prj_03`, con la biblioteca general `stm32h7_shared_drivers` (`01_shared_drivers/CMakeLists.txt:180-188`) |
| Llamadas | `LED_Init()` en el `main.c` de CM7 de los cuatro (antes de liberar a CPU2); `LED_PanicRed()` en los handlers de fallo; `LED_Set`/`LED_Toggle`/`LED_BootStage` en la aplicación |
| `_PIN` | Solo se define en los tres `board_def.h`; ningún archivo lo lee |
| Tabla en RAM | `g_led_config` (`led_bm.c:37-43`), en `.data`, con los puertos y bits que se compilan |

---

## Pruebas

| ID | Qué | Oráculo | Control negativo | Criterio |
| --- | --- | --- | --- | --- |
| **T1** | `build_all.ps1 -Clean` 8/8; además, los presets de selftest que enlazan `led_bm` en `prj_01` (`selftest-gpio`, `selftest-tim`, `selftest-spi`, `selftest-i2c`) y en `prj_04` (`selftest`, `selftest-adc`, `selftest-uart`, `selftest-hsem`, `selftest-gpio`), con `--fresh` y `--clean-first` | Código de salida y todas las líneas `warning:` | Una línea de aviso sintética pasa por el mismo contador y lo hace fallar | 0 errores y 0 avisos en todo |
| **T2** | El `#error` y el rango, compilando `led_bm.c` con `-fsyntax-only` y los flags de `compile_commands.json` de cada proyecto, contra un `board_def.h` que es una **copia temporal** fuera del árbol | Código de salida del compilador y texto del `#error` | Cuatro mutaciones del `board_def.h` real de cada proyecto, cada una debe dar el `#error` esperado:<br>• sin `BOARD_LED_RED_BIT`;<br>• sin `BOARD_LED_GREEN_PORT`;<br>• `_PIN` en lugar de `_BIT` (el estado de hoy de `prj_02`–`prj_04`);<br>• `BOARD_LED_YELLOW_BIT 16U`.<br>El `board_def.h` sin mutar compila | Las 4 × 4 mutaciones fallan con su mensaje; los 4 originales compilan |
| **T3** | Mismo comportamiento: el contenido inicial de `g_led_config` (sección `.data` del objeto `led_bm.c.obj` de CM7, leído con `objdump -s`) de los 8 builds de producción, contra el control `9dc6896` compilado en la ejecución | Bytes de la tabla: puertos `0x58020400` (GPIOB), `0x58021000` (GPIOE), y bits 0, 1, 14 | La misma comparación, con una copia de la salida de `objdump` del control con un byte cambiado, debe fallar | Iguales en los 8, y además iguales a la tabla escrita a mano desde UM2408 (LD1 PB0, LD2 PE1, LD3 PB14) |
| **T4** (auditor, placa) | Por proyecto, `debug` flasheado y verificado; tras el arranque, lectura por SWD en `mode=HOTPLUG`, sin detener: `g_led_config` (dirección de `nm`) y `MODER` de GPIOB y GPIOE | Tabla en RAM igual a la de T3; los bits de `MODER` de PB0, PB14 y PE1 en `01` (salida) | La misma comparación contra una tabla alterada (un bit cambiado) falla | Los cuatro proyectos PASS. El responsable mira además los LED, sin que cuente |

---

## Banco y seguridad

- **Sin AD2.** En T4 solo se flashean imágenes `debug` del repo y se hacen lecturas SWD (`mode=HOTPLUG`), con `sn=0016000C4142501220353451`.
- No se escribe ningún registro por SWD, no se tocan alimentación, reloj ni option bytes.
- Al terminar, la placa queda con la imagen que diga el responsable.

---

## Fases y entregables

| Fase | Entregable | Criterio de pase |
| --- | --- | --- |
| **F1** | H1–H4; capturas de T1–T3 en `01_shared_drivers/01_led/docs/` (carpeta nueva); el comando de T4 para el auditor | Auditoría del código y de la herramienta, T1–T3 del auditor y T4 en la placa |
| **Cierre** | **Implementador:** `01_led/README.md` (§3 contrato sin valores por defecto, §4, §5 sin L1/L2 y con L5 parcial, §6 historia; validación con sus capturas).<br>**Responsable:** la hoja de ruta, el borrado de los documentos de proceso y el merge | Auditoría del cierre |

---

## Riesgos

| Riesgo | Mitigación |
| --- | --- |
| Algún proyecto o preset no incluye `board_def.h` y deja de compilar | T1 cubre la matriz y todos los presets que enlazan `led_bm` |
| Un número de bit mal copiado | T3 compara los bytes con el control y con la tabla de UM2408 |
| `prj_eth_lwip_node` | No enlaza este driver (tiene su copia); T1 no lo compila |

---

## Decisiones abiertas

1. **Alcance:**
   - **A (por defecto):** L1, L2 y el rango de `_BIT` en compilación.
   - **B:** además, `LED_Init_Config()` ignora las entradas con `pin_num > 15`, lo que cierra L5 también para tablas pasadas a mano. La función devuelve `void`, así que el error sería silencioso.
2. **Prueba en la placa:**
   - **A (por defecto):** lectura SWD de la tabla y de `MODER`, objetiva y sin AD2.
   - **B:** además, capturar los LED con el AD2. Requiere cablear LD1–LD3, que no están en el banco.
3. **Documentos de proceso:** se borran al cierre.
