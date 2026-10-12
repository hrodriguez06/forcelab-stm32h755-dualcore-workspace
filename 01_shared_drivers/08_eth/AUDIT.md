# Auditoría de `08_eth` (E1, E2, E5, E8)

Auditor: Claude (Claude Code), para Hector Rodriguez · Rama: `driver/eth` · Plan: `IMPLEMENTATION_PLAN.md` revisión 1

Este archivo lo escribe solo el auditor. Un hallazgo se cierra cuando el auditor lo verifica.

## F1: revisión, build y ejecución del auditor en la placa (`bb70e56`)

Entrega: `92e2c77..bb70e56` (código en `d4feb24`, herramienta en `709b869`, capturas e informe en `bb70e56`). El responsable aceptó las cinco decisiones del plan con su opción por defecto el 2026-10-11.

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| Alcance | Solo `eth_hal.[ch]`, el target `stm32h7_driver_eth` de `01_shared_drivers/CMakeLists.txt`, `tools/eth_build_checks.py`, `08_eth/docs/` y el informe. Nada en `02_firmware_projects` ni en `03_firmware_templates`; el plan y las instrucciones, sin tocar |
| API | Ninguna firma de `eth_hal.h` ni de `lan8742_phy.h` cambia; `eth_driver_stats_t`, igual. `eth_hal.h` solo gana comentarios |
| `build_all -Clean` en `bb70e56` (mi ejecución) | 8/8; `prj_01` igual a `2481396` (`eth_hal.c` compila a nada sin `HAL_ETH_MODULE_ENABLED`) |
| `prj_02`, `prj_03`, `prj_04` debug con `CMAKE_C_FLAGS=-Werror` (mi ejecución) | Exit 0 y 0 avisos en los tres |
| `py tools/eth_build_checks.py` (mi ejecución) | T2, T3 y T4 PASS |
| E-S1 | En `prj_02` ningún núcleo configura pines fuera de los drivers (ni `HAL_GPIO_Init` ni `GPIO_*` en su `CM4`, `CM7` o `Common`); los 14 pines del informe (§5.4: LED PB0, PE1, PB14; USART3 PD8/PD9; los nueve RMII) no coinciden con ninguna línea del banco |

### Revisión del código

- **E1:** `#include "board_def.h"` sin `__has_include` y un `#error` por cada una de las 18 macros, con el nombre y la regla 1, todo dentro de `#if defined(HAL_ETH_MODULE_ENABLED)`; los pines por defecto, borrados. La tabla `s_rmii_pins` no cambia. Los tres consumidores definen las 18.
- **E2:** `#error` si `USE_HAL_ETH_REGISTER_CALLBACKS` está definida y es distinta de 0; la macro llega por `eth_hal.h` → `stm32h7xx_hal.h` → `stm32h7xx_hal_conf.h`, antes de la comprobación.
- **E5:** los cinco incrementos con `__atomic_fetch_add(…, 1U, __ATOMIC_RELAXED)`; T4 ve cinco bucles `LDREX`/`STREX` y ninguno en `7154af6`.
- **E8:** los nueve pines con `GPIO_HAL_Init`, que habilita el reloj del puerto él mismo (`gpio_hal.c`: "Clock it before reading LCKR"), por eso desaparece `GPIO_BM_EnablePortClock`; el primer estado distinto de `HAL_OK` se guarda y `ETH_Driver_Init` devuelve `ETH_DRIVER_ERROR` (decisión 4). `stm32h7_driver_eth` enlaza `stm32h7_driver_gpio_hal`. `nm -u` del objeto: `GPIO_HAL_Init` sí, `HAL_GPIO_Init` no.
- **Herramienta:** T2 exige `exit ≠ 0`, `#error` en la salida y el nombre de la macro (con una sola macro quitada por variante, solo su `#error` puede saltar); T3 lo mismo con `USE_HAL_ETH_REGISTER_CALLBACKS`; los controles compilan. Las variantes se escriben en `build/scratch_eth_checks`, nunca en el proyecto.

### Mi ejecución en la placa

`prj_02` debug compilado en `bb70e56` (código igual al de `709b869`) y flasheado con `tools/flash.py --preset debug --sn 0016000C4142501220353451` (Device ID 0x450, "Download verified successfully"); cable y red confirmados por el responsable. Capturas sin editar:

- `08_eth/docs/eth_f1_audit_t5_swd_gpio_bb70e56.txt`: lectura por SWD sin parar (`mode=HOTPLUG`) de GPIOA/B/C/G. Los nueve pines RMII en MODER 2, OTYPER 0, OSPEEDR 3, PUPDR 0, AF 11 frente a la tabla escrita a mano; el control (PA3, PB10, PC0, PG0 por la misma comparación) los distingue. **PASS.**
- `08_eth/docs/eth_f1_audit_t6_netcheck_bb70e56.txt`: `net_check.py` con `monitor.py --sn` corriendo desde 6 s antes. ICMP 4/4; UDP de 1, 25, 512, 1024 y 1472 B idénticos; ráfaga 100/100; enlace 100 Mbps full duplex; `rx_nobuf`, `tx_nobuf` y `dma_err` a 0 antes y después (rx 5 → 117, tx 1 → 112). **PASS.**

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| m1 | Menor (evidencia) | La captura de T2/T3 (`eth_f1_t2_t4_checks_709b869.txt`) imprime como detalle la última línea de contexto del compilador (p. ej. "Missing BOARD_ETH_MDIO_PORT (…BOARD_ETH_MDC_PORT)"), no el texto del `#error` que la herramienta comprueba. El criterio es correcto, pero la evidencia no lo muestra | Que la herramienta imprima la línea del `#error` (en el cierre o cuando se toque) |
| m2 | Menor (evidencia) | La captura de T6 de la entrega junta la salida de dos herramientas y termina con un resumen escrito a mano ("Oracle Criteria … Verdict: PASS"), y la de T5 no dice qué pines de control usó hasta el final. No es una ejecución "sin editar" en sentido estricto | En adelante, la salida de cada herramienta tal cual; los juicios, en el informe |
| m3 | Menor (doc) | Si un pin falla, `ETH_Driver_Init` devuelve `ETH_DRIVER_ERROR` después de que `HAL_ETH_Init` dejó el MAC inicializado y la MSP habilitó su interrupción; el encabezado no lo dice ni qué hacer (`HAL_ETH_DeInit`) | Una frase en `eth_hal.h` o en el README del cierre |

**Observaciones:** O1, `prj_02` no imprime un id de compilación, así que la identidad de la imagen de las capturas de placa descansa en el commit con que se compiló y flasheó; mi ejecución es la evidencia independiente. O2, E8 solo protege entre núcleos si el proyecto instala el lock compartido en los dos y el otro núcleo configura sus pines por `gpio_bm`; ningún proyecto de producción lo hace (plan, fuera de alcance; va al README).

### Estado

**F1 pasa.** Sin hallazgos bloqueantes ni mayores; m1–m3 se contestan en el cierre. El implementador puede empezar el cierre: `08_eth/README.md` (§3, §8, §9 con el registro nuevo, §10 sin E1, E2, E5 y E8 como limitaciones del driver, §12) y la respuesta a m1–m3. El registro de `prj_02/README.md` §6 lo hace el responsable en el cierre (decisión 1, aceptada el 2026-10-11). La H755 quedó con la imagen de `prj_02`.
