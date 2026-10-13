# Informe de implementación de `00_gpio` F1c (G1 en `prj_03`)

Oct 13, 2026 · Rama `driver/gpio-f1c` · Estado: **F1 entregada para auditoría**

Este informe documenta la implementación y verificación de la fase F1c del driver `00_gpio`, correspondiente al cierre del hallazgo inter-núcleo G1 en `02_firmware_projects/prj_03_adc_dma_eth`. Se ha instalado el hook de bloqueo compartido entre núcleos basado en HSEM 5 (H1) en ambos núcleos de `prj_03`, se ha enrutado la configuración del pin analógico del ADC de CM4 a través de `gpio_bm` (H2), y se ha extendido `tools/gpio_f1b_checks.py` con `--project {prj_04, prj_03}` resolviendo los tres puntos menores pendientes de F1b.

---

## 5.1 Commits

Rama de trabajo: `driver/gpio-f1c`  
Punto de partida: `e6cb75e` (merge de F1b en `main`)

| Commit | Asunto | Descripción / Justificación técnica |
| --- | --- | --- |
| `fe4d406` | `docs(gpio): plan F1c, the cross-core pin lock in prj_03 (G1)` | Planificación de F1c (revisión 1) e instrucciones para el implementador. |
| `0244b31` | `docs(gpio): record the owner's approval of the F1c decisions` | Aprobación de las cuatro decisiones de F1c por el responsable con opciones por defecto. |
| `90a866f` | `fix(prj03): install cross-core GPIO lock hook (H1, G1)` | **H1:** Instalación del hook de GPIO en `prj_03`: `BOARD_GPIO_HSEM_ID 5U`, 60 µs timeout, límites de 24.000 / 12.000 ciclos en `shared_memory.h`; funciones estáticas `shared_gpio_lock`/`unlock` y export de `Shared_GpioLockInstall` en `shared_memory.c`; `HSEM_BM_Init`, `HSEM_BM_TimebaseStart` e instalación en `main.c` de CM7 (tras `Shared_Init` antes de liberar CPU2) y CM4 (tras arranque y `HAL_Init` antes de configurar pines); enlace de `stm32h7_driver_hsem_bm` en CM7 y CM4. Campos al final de `SharedMemory_t`. |
| `ee5e3b3` | `fix(prj03): route CM4 ADC pin configuration through gpio_bm (H2, G1)` | **H2:** Reemplazo de llamada directa a `HAL_GPIO_Init` por `GPIO_BM_ConfigPin` para `BOARD_ADC_PORT` (GPIOB) y pin PB1 en modo analógico sin pull; retorno booleano y fallo de `ADC_Acquisition_Init()` ante error. |
| `f733b98` | `feat(tools): add --project support and fix control path resolution` | Extensión de `tools/gpio_f1b_checks.py` con `--project {prj_04, prj_03}`. Corrección de los tres menores de F1b: `g_shared` obtenido de `nm` sin fallback hardcodeado; control de T2b comparado por ruta relativa completa (`Path.relative_to`); líneas y rutas de control calculadas relativas a la raíz. |

---

## 5.2 Compilaciones y tamaños (T1)

### 5.2.1 Matriz CI (`tools/build_all.ps1 -Clean`)

Compilación limpia de los 4 proyectos dual-core en `debug` y `release` (8 configuraciones):

| Proyecto | Preset | Núcleo | Estado | Text (B) | Data (B) | Bss (B) | Total (B) | Tiempo |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `prj_01_template_dualcore` | debug | CM7 | PASS | 30632 | 132 | 55676 | 86440 | 25.4s |
| `prj_01_template_dualcore` | debug | CM4 | PASS | 3456 | 24 | 12560 | 16040 | 25.4s |
| `prj_01_template_dualcore` | release | CM7 | PASS | 29620 | 132 | 55680 | 85432 | 18.0s |
| `prj_01_template_dualcore` | release | CM4 | PASS | 3112 | 24 | 12560 | 15696 | 18.0s |
| `prj_02_eth_lwip_dualcore` | debug | CM7 | PASS | 87312 | 152 | 120096 | 207560 | 31.4s |
| `prj_02_eth_lwip_dualcore` | debug | CM4 | PASS | 2580 | 24 | 12488 | 15092 | 31.4s |
| `prj_02_eth_lwip_dualcore` | release | CM7 | PASS | 83224 | 152 | 120096 | 203472 | 23.3s |
| `prj_02_eth_lwip_dualcore` | release | CM4 | PASS | 2332 | 24 | 12488 | 14844 | 23.3s |
| `prj_03_adc_dma_eth` | debug | CM7 | PASS | 89684 | 152 | 121224 | 211060 | 30.7s |
| `prj_03_adc_dma_eth` | debug | CM4 | PASS | 21332 | 24 | 16192 | 37548 | 30.7s |
| `prj_03_adc_dma_eth` | release | CM7 | PASS | 85480 | 152 | 121224 | 206856 | 24.0s |
| `prj_03_adc_dma_eth` | release | CM4 | PASS | 18932 | 24 | 16192 | 35148 | 24.0s |
| `prj_04_adc_dma_bm_eth` | debug | CM7 | PASS | 90512 | 156 | 121268 | 211936 | 29.8s |
| `prj_04_adc_dma_bm_eth` | debug | CM4 | PASS | 9844 | 28 | 15920 | 25792 | 29.8s |
| `prj_04_adc_dma_bm_eth` | release | CM7 | PASS | 86524 | 156 | 121264 | 207944 | 25.3s |
| `prj_04_adc_dma_bm_eth` | release | CM4 | PASS | 9296 | 28 | 15920 | 25244 | 25.3s |

Total: **8 de 8 configuraciones compilaron limpiamente (0 errores, 0 avisos con `-Werror`)**.  
Presets adicionales verificados en T1: `prj_03` preset `selftest` (PASS, 0 avisos) y `prj_04` preset `debug` (PASS, 0 avisos).  
Fuente única de datos: `02_firmware_projects/prj_03_adc_dma_eth/docs/gpio_f1c_t1_f733b98.txt`.

### 5.2.2 Comparación de tamaños de objetos (.text) antes y después
Comparación directa mediante `arm-none-eabi-size` entre el baseline `e6cb75e` y el commit `f733b98` (registrado en `docs/gpio_f1c_t1_f733b98.txt`):

| Objeto | Configuración | Baseline `e6cb75e` (text / data / bss) | F1c `f733b98` (text / data / bss) | Delta text | Causa del delta |
| --- | --- | --- | --- | --- | --- |
| `adc_acquisition.c.obj` (CM4) | `debug` | 704 / 0 / 2536 | 688 / 0 / 2536 | -16 B | Reemplazo de configuración con `HAL_GPIO_Init` por llamada a `GPIO_BM_ConfigPin`. |
| `adc_acquisition.c.obj` (CM4) | `release` | 636 / 0 / 2536 | 624 / 0 / 2536 | -12 B | Misma causa bajo optimización `-Os`. |
| `shared_memory.c.obj` (CM7) | `debug` | 64 / 1280 / 0 | 200 / 1280 / 4 | +136 B | Implementación del hook `shared_gpio_lock`, `shared_gpio_unlock`, `Shared_GpioLockInstall` y variable estática `s_hook_installed`. |
| `shared_memory.c.obj` (CM4) | `debug` | 64 / 1280 / 0 | 200 / 1280 / 4 | +136 B | Misma implementación compilada para Cortex-M4. |

---

## 5.3 Pruebas (T1–T7)

Todas las capturas corresponden a ejecuciones limpias e íntegras en `02_firmware_projects/prj_03_adc_dma_eth/docs/` (y `prj_04` para T7):

| ID | Qué | Oráculo | Control negativo | Criterio y resultado | Captura oficial |
| --- | --- | --- | --- | --- | --- |
| **T1** | Compilaciones: `build_all.ps1 -Clean` (8/8), `prj_03` preset `selftest`, `prj_04` preset `debug` | Exit code 0 y 0 avisos del compilador (`-Werror`) | — | 8/8 matrix PASS, `selftest` PASS, `prj_04` debug PASS con 0 avisos. Deltas de objetos documentados. | `gpio_f1c_t1_f733b98.txt` |
| **T2a** | Enlace de símbolos HAL GPIO en producción en `prj_03` | `nm -u` de cada objeto enlazado en las 4 ELFs de `prj_03`: `HAL_GPIO_Init` solo lo referencia `gpio_hal.c.obj` (y `stm32h7xx_hal_rcc.c.obj`); `HAL_GPIO_DeInit` no está definido en ninguna ELF; `adc_acquisition.c.obj` libre de llamadas HAL. | Las 4 ELFs de `prj_03` compiladas en `e6cb75e`: `adc_acquisition.c.obj` referencia `HAL_GPIO_Init` en las dos ELFs de CM4 (`debug` y `release`), y el oráculo falla. | PASS (4/4 ELFs actuales conformes; control negativo falla en 2/2 CM4). | `gpio_f1c_t2_f733b98.txt` |
| **T2b** | Ausencia de escrituras directas a registros GPIO fuera del driver en `prj_03` | Preprocesado (`gcc -E`) de todas las unidades de `compile_commands.json` en los 3 presets de `prj_03` (`debug`, `release`, `selftest`): 0 escrituras fuera de excepciones. Autoprueba CMSIS verifica 12 formas de escritura detectadas y 9 lecturas rechazadas. | Autoprueba CMSIS sirve como control negativo (el árbol en `e6cb75e` usaba llamadas HAL en lugar de registros directos, cubierto por T2a). | PASS (Autoprueba 12/12 escrituras detectadas, 0/9 lecturas falsas; 970 unidades actuales limpias: debug 323, release 323, selftest 324; 0 unidades fallidas). | `gpio_f1c_t2_f733b98.txt` |
| **T3** | Inspección SWD en vivo del hook, HSEM 5, contadores y registros de pines | Punteros `s_shared_lock` coinciden con `g_shared_gpio_lock` (CM7: `0x200128f8`, CM4: `0x30020a58`); `HSEM_R5 = 0x0` (`0x58026414`, libre, sin efectos); fallos = 0; PB1 analógico; PB13 AF11 PP muy alta; PD8/PD9 AF7 PP pullup; PB15 y PD0 en reset. | Imagen de `e6cb75e` flasheada en placa: ambos `s_shared_lock` leen `0x00000000` (NULL), T3 confirma ausencia de hook en control. Imagen de producción restablecida al finalizar. | PASS (Flasheo verificado al inicio; todos los valores exactos con comprobación `pd8_ok`; control negativo confirma NULL; imagen restaurada). | `gpio_f1c_t3_f733b98.txt` |
| **T4** | Margen de retención del lock (Decisión 2) | Medición de `m7_/m4_gpio_lock_hold_cycles_max` por SWD tras el arranque: ambos > 0 y margen `>= 10.0x`. Límites derivados de `shared_memory.h` (24000/12000). Offsets derivados de DWARF sin respaldos silenciosos (`0x380084f0`, `0x380084f4`). | Mismo cálculo con límite 10x inferior (2400 / 1200 ciclos) da margen `< 10x` y falla. | PASS (CM7: 1274 ciclos / 18.84x; CM4: 363 ciclos / 33.06x; control negativo 1.88x / 3.31x, ambos < 10x). | `gpio_f1c_t4_f733b98.txt` |
| **T5** | Regresión de producción (debug y release) | Flasheo con descarga verificada; ICMP 4/4; UDP sweep 1..1472 B idénticos; UDP burst 100/100; todas las líneas `[net]` con `rx_nobuf=0`, `tx_nobuf=0`, `dma_err=0`; telemetría ADC continua con `Samples` creciente y `OVR == 0`. | — | PASS en debug y release. | `gpio_f1c_t5_debug_f733b98.txt`, `gpio_f1c_t5_release_f733b98.txt` |
| **T6a** | Arranques repetidos por SWD (20 resets) | 20 resets secuenciales con `-rst`, espera de 10s tras cada uno, ejecución de T3 completo tras cada reset. Flasheo inicial verificado. | — | PASS (20 de 20 resets con 10/10 comprobaciones T3 válidas tras cada uno). | `gpio_f1c_t6a_f733b98.txt` |
| **T6b** | Arranque en frío (corte de alimentación física) | Repetición de T3 y T5 tras arranque en frío. | — | PENDIENTE por banco en remoto (G-S7). No bloquea entrega de F1; sí el cierre. Lo ejecutará el auditor/responsable con acceso físico. | Ver §5.8 |
| **T7** | Compatibilidad con `prj_04`: `--project prj_04 --test t2` | `run_t2a` y `run_t2b` sobre `prj_04`: 1951 unidades escaneadas en 6 presets con 0 escrituras ilegales; control en `07db657` detecta las 20 escrituras por `(preset, ruta_relativa)` con rutas relativas a la raíz y líneas exactas. | Las 5 ELFs CM7 de `07db657` fallan el oráculo T2a. | PASS (1951 unidades limpias, 20 hallazgos en control, autoprueba 12/12 y 0/9). | `gpio_f1c_t7_f733b98.txt` (en `prj_04/docs/`) |

---

## 5.4 Escritores de GPIO de `prj_03` y comprobación de pines (G-S1)

### 5.4.1 Escritores antes (`e6cb75e`) y después (`f733b98`)

| Núcleo | Archivo | Puerto / Pines | Momento | Estado en `e6cb75e` (antes) | Estado en `f733b98` (después) |
| --- | --- | --- | --- | --- | --- |
| **CM7** | `prj_03/CM7/Core/Src/main.c` | PB0, PE1, PB14 (LEDs) | Antes de liberar CPU2 | `BSP_LED_Init()` mono-núcleo (sin lock) | `BSP_LED_Init()` mono-núcleo (sin lock; seguro, CPU2 dormido) |
| **CM7** | `01_shared_drivers/08_eth/eth_hal.c` | PA1, PA2, PA7, PB13, PC1, PC4, PC5, PG11, PG13 (RMII) | Antes de liberar CPU2 | `GPIO_HAL_Init()` con hook `NULL` (sin lock activo) | `GPIO_HAL_Init()` protegido por `s_shared_lock` (HSEM 5) |
| **CM7** | `01_shared_drivers/04_uart/uart_hal.c` | PD8, PD9 (USART3 TX/RX) | Antes de liberar CPU2 | `GPIO_HAL_Init()` protegido por `s_shared_lock` (migrado en F1b) | `GPIO_HAL_Init()` protegido por `s_shared_lock` (HSEM 5) |
| **CM4** | `prj_03/CM4/App/Src/adc_acquisition.c` | PB1 (ADC_INP5 analógico) | **Después de liberar CPU2** | **`HAL_GPIO_Init(GPIOB, &s)` SIN PROTECCIÓN (G1)** | **`GPIO_BM_ConfigPin()` protegido por `s_shared_lock` (HSEM 5) (G1 RESUELTO)** |

### 5.4.2 Comprobación de seguridad de pines del banco (G-S1)
Comprobación exhaustiva de los pines utilizados en producción por `prj_03` contra las líneas reservadas del banco físico de pruebas:

1. **Pines de producción de `prj_03`:**
   - Ethernet RMII: PA1, PA2, PA7, PB13, PC1, PC4, PC5, PG11, PG13.
   - LEDs de usuario: PB0 (verde), PE1 (amarillo), PB14 (rojo).
   - Consola serie USART3: PD8 (TX), PD9 (RX).
   - ADC adquisición: PB1 (`BOARD_ADC_USE_FAST_CHANNEL_PB1 = 1`).
2. **Líneas reservadas del banco:**
   - Analizador Digital Discovery (AD2): PE2–PE15 y PA8. (PE1 es LED, no colisiona con PE2–PE15 ni PA8).
   - Conector J1 (SPI / hardware auxiliar): PA5, PA6, PB4, PB5. (No utilizados por `prj_03`).
   - Bus I2C de telemetría y sensores: PB8, PB9. (No utilizados por `prj_03`).
   - Shunt de corriente / tomas auxiliares: PF14, PF15. (No utilizados por `prj_03`).
3. **Conclusión G-S1:** Ningún pin de producción de `prj_03` colisiona con líneas reservadas del banco físico.

---

## 5.5 Desviaciones

- **Sin desviaciones:** La implementación se ajustó estrictamente al diseño de [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1) y a las 4 decisiones aprobadas por el responsable.
- **Entorno de control:** El worktree de control `../stm32h755_control_e6cb75e` se configuró en el commit base limpio `e6cb75e`. Para optimizar almacenamiento y evitar descargas redundantes, el submódulo `00_Cube/STM32CubeH7` se enlazó mediante junction al árbol principal.

---

## 5.6 Respuesta a la auditoría

- **Primera entrega de F1c:** Esta es la entrega inicial para auditoría independiente.
- **Incorporación de lecciones de F1b:**
  - **B1:** Lectura exclusiva de `HSEM_R5` (`0x58026414`), nunca `HSEM_RLR5` (que bloquearía el semáforo).
  - **B2–B4:** Criterio riguroso de objetos enlazados en T2a con control negativo que exige fallo en las ELFs de CM4 de `e6cb75e`.
  - **B7 / M1:** Preprocesado de código en T2b verificando 0 escrituras directas y autoprueba de macros CMSIS (12 escrituras detectadas, 9 lecturas rechazadas).
  - **N1:** Verificación completa de los 5 campos en los registros SWD en T3, asegurando la comprobación `pd8_ok`.
  - **N2:** Exigencia de crecimiento estricto de `Samples` y `OVR == 0` en `query_adc.py`.
  - **N3:** Verificación de `rx_nobuf=0`, `tx_nobuf=0` y `dma_err=0` en todas las líneas `[net]`.
  - **N4:** La opción `--control` implica `--flash`.
  - **N5:** Espera de 3.0s tras flasheo para estabilización de hardware previa a la inspección SWD.
  - **N6:** Directorio de control obligatorio para el escaneo del árbol en T2b (`prj_04`).
  - **m4:** Offsets DWARF y límites de tiempo derivados dinámicamente sin fallbacks silenciosos.
- **Resolución de los tres menores pendientes de F1b:**
  1. `g_shared` se obtiene de `nm` sin fallback hardcodeado.
  2. El control de T2b compara por ruta relativa completa (`CM4/Core/Src/main.c`, etc.).
  3. Las rutas y líneas de control se calculan exactamente relativas a la raíz del árbol.

---

## 5.7 Erratas del plan

- **Ninguna errata encontrada:** El plan rev. 1 se ajustó con total precisión a la arquitectura de `prj_03` y a las herramientas de prueba.

---

## 5.8 Lo abierto y lo no ejercitado

1. **T6b (arranque en frío):** Pendiente de ejecución en el banco físico por el responsable (G-S7). Conforme al plan (§112 y §140), no bloquea la entrega de F1 para auditoría, pero sí es requisito indispensable para el cierre formal de la fase.
2. **Preset `selftest` de `prj_03`:** Compilado con 0 errores y 0 avisos en T1 y escaneado con 324 unidades limpias en T2b. No se flashea en placa física por regla de seguridad G-S1 (`ADC_SelfTestHw` manipula pines y canales de hardware de los ADCs).
3. **Imagen que quedó en la placa (G-S6):** La placa física quedó flasheada y en ejecución con la imagen de producción **`prj_03_adc_dma_eth` preset `debug` (commit `f733b98`)**. Se verificó su correcto funcionamiento tras los 20 resets de T6a mediante `net_check.py --host 192.168.1.150` (PASS 3/3: ICMP 4/4, UDP 1..1472 B, burst 100/100).
