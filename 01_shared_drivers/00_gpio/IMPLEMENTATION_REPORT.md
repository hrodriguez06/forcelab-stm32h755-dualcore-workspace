# Informe de implementación de `00_gpio` F1c (G1 en `prj_03`)

Oct 13, 2026 · Rama `driver/gpio-f1c` · Estado: **Cierre de F1c entregado para auditoría**

Este informe documenta la implementación, verificación y resolución de hallazgos de auditoría de la fase F1c del driver `00_gpio`, correspondiente al cierre del hallazgo inter-núcleo G1 en `02_firmware_projects/prj_03_adc_dma_eth`. Se ha instalado el hook de bloqueo compartido entre núcleos basado en HSEM 5 (H1) en ambos núcleos de `prj_03`, se ha enrutado la configuración del pin analógico del ADC de CM4 a través de `gpio_bm` (H2), y se ha extendido y corregido `tools/gpio_f1b_checks.py` con `--project {prj_04, prj_03}` respondiendo a todos los hallazgos M1–M3 y m1–m6 de la auditoría independiente.

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
| `f733b98` | `feat(tools): add --project support and fix control path resolution` | Extensión inicial de `tools/gpio_f1b_checks.py` con `--project {prj_04, prj_03}`. |
| `7ba2cf0` | `docs(gpio): record implementation report and test captures for F1c` | Primera entrega de informe y capturas de prueba para auditoría independiente. |
| `e9e98c4` | `fix(tools): address audit findings M1, m1, m4, m5 in gpio_f1b_checks.py` | Resolución de hallazgos de auditoría: M1 (`--project` obligatorio sin default, validación estricta de commit de control contra el proyecto); m1 (nota verdadera de telemetría por proyecto en T5); m4 (impresión del origen exacto de offsets en T4: `nm` + DWARF); m5 (tabla de configuración por proyecto para pines de T3 y query de T5, eliminación de caso repetido en control, contabilización de status en T2a, docstring actualizado). |

---

## 5.2 Compilaciones y tamaños (T1)

### 5.2.1 Matriz CI (`tools/build_all.ps1 -Clean`)

Compilación limpia de los 4 proyectos dual-core en `debug` y `release` (8 configuraciones):

| Proyecto | Preset | Núcleo | Estado | Text (B) | Data (B) | Bss (B) | Total (B) | Tiempo |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `prj_01_template_dualcore` | debug | CM7 | PASS | 30632 | 132 | 55676 | 86440 | 23.6s |
| `prj_01_template_dualcore` | debug | CM4 | PASS | 3456 | 24 | 12560 | 16040 | 23.6s |
| `prj_01_template_dualcore` | release | CM7 | PASS | 29620 | 132 | 55680 | 85432 | 17.1s |
| `prj_01_template_dualcore` | release | CM4 | PASS | 3112 | 24 | 12560 | 15696 | 17.1s |
| `prj_02_eth_lwip_dualcore` | debug | CM7 | PASS | 87312 | 152 | 120096 | 207560 | 28.9s |
| `prj_02_eth_lwip_dualcore` | debug | CM4 | PASS | 2580 | 24 | 12488 | 15092 | 28.9s |
| `prj_02_eth_lwip_dualcore` | release | CM7 | PASS | 83224 | 152 | 120096 | 203472 | 22.7s |
| `prj_02_eth_lwip_dualcore` | release | CM4 | PASS | 2332 | 24 | 12488 | 14844 | 22.7s |
| `prj_03_adc_dma_eth` | debug | CM7 | PASS | 89684 | 152 | 121224 | 211060 | 29.9s |
| `prj_03_adc_dma_eth` | debug | CM4 | PASS | 21332 | 24 | 16192 | 37548 | 29.9s |
| `prj_03_adc_dma_eth` | release | CM7 | PASS | 85480 | 152 | 121224 | 206856 | 23.5s |
| `prj_03_adc_dma_eth` | release | CM4 | PASS | 18932 | 24 | 16192 | 35148 | 23.5s |
| `prj_04_adc_dma_bm_eth` | debug | CM7 | PASS | 90520 | 156 | 121268 | 211944 | 29.2s |
| `prj_04_adc_dma_bm_eth` | debug | CM4 | PASS | 9844 | 28 | 15920 | 25792 | 29.2s |
| `prj_04_adc_dma_bm_eth` | release | CM7 | PASS | 86532 | 156 | 121264 | 207952 | 22.6s |
| `prj_04_adc_dma_bm_eth` | release | CM4 | PASS | 9296 | 28 | 15920 | 25244 | 22.6s |

Total: **8 de 8 configuraciones compilaron limpiamente (0 errores, 0 avisos con `-Werror`)**.  
Presets adicionales verificados en compilación limpia en T1: `prj_03` preset `selftest` (PASS, 326 unidades, 0 avisos) y `prj_04` preset `debug` (PASS, 324 unidades, 0 avisos). Se verificó la presencia explícita de `-Werror` en las banderas de compilación de CM7 y CM4 en debug y release.  
Fuente única de datos: `02_firmware_projects/prj_03_adc_dma_eth/docs/gpio_f1c_t1_e9e98c4.txt`.

### 5.2.2 Comparación de tamaños de objetos (.text) antes y después
Comparación directa mediante `arm-none-eabi-size` entre el baseline `e6cb75e` y el commit `e9e98c4` (registrado en `docs/gpio_f1c_t1_e9e98c4.txt`):

| Objeto | Configuración | Baseline `e6cb75e` (text / data / bss) | F1c `e9e98c4` (text / data / bss) | Delta text | Causa del delta |
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
| **T1** | Compilaciones limpias: `build_all.ps1 -Clean` (8/8), `prj_03` preset `selftest`, `prj_04` preset `debug`, verificación de `-Werror` | Exit code 0 y 0 avisos del compilador (`-Werror` activo en CM7 y CM4) | — | 8/8 matrix PASS, `selftest` PASS (326 unidades), `prj_04` debug PASS (324 unidades) con 0 avisos. Comandos y banderas documentados. | `gpio_f1c_t1_e9e98c4.txt` |
| **T2a** | Enlace de símbolos HAL GPIO en producción en `prj_03` | `nm -u` de cada objeto enlazado en las 4 ELFs de `prj_03`: `HAL_GPIO_Init` solo lo referencia `gpio_hal.c.obj` (y `stm32h7xx_hal_rcc.c.obj`); `HAL_GPIO_DeInit` no está definido en ninguna ELF; `adc_acquisition.c.obj` libre de llamadas HAL. | Las 4 ELFs de `prj_03` compiladas en `e6cb75e`: `adc_acquisition.c.obj` referencia `HAL_GPIO_Init` en las dos ELFs de CM4 (`debug` y `release`), y el oráculo falla. | PASS (4/4 ELFs actuales conformes; control negativo falla en 2/2 CM4). | `gpio_f1c_t2_e9e98c4.txt` |
| **T2b** | Ausencia de escrituras directas a registros GPIO fuera del driver en `prj_03` | Preprocesado (`gcc -E`) de todas las unidades de `compile_commands.json` en los 3 presets de `prj_03` (`debug`, `release`, `selftest`): 0 escrituras fuera de excepciones. Autoprueba CMSIS verifica 12 formas de escritura detectadas y 9 lecturas rechazadas. | Autoprueba CMSIS sirve como control negativo; control en `e6cb75e` confirmado. | PASS (Autoprueba 12/12 escrituras detectadas, 0/9 lecturas falsas; 970 unidades actuales limpias: debug 323, release 323, selftest 324; 0 unidades fallidas). | `gpio_f1c_t2_e9e98c4.txt` |
| **T3** | Inspección SWD en vivo del hook, HSEM 5, contadores y registros de pines | Punteros `s_shared_lock` coinciden con `g_shared_gpio_lock` (CM7: `0x8015B80`, CM4: `0x81052F0`); `HSEM_R5 = 0x0` (`0x58026414`, libre, sin efectos colaterales); fallos = 0; PB1 analógico; PB13 AF11 PP muy alta; PD8/PD9 AF7 PP pullup; PB15 y PD0 en reset. | Imagen de `e6cb75e` flasheada en placa: ambos `s_shared_lock` leen `0x00000000` (NULL), T3 confirma ausencia de hook en control. Imagen de producción restablecida al finalizar. | PASS (Flasheo verificado al inicio; todos los valores exactos con comprobación `pd8_ok`; control negativo confirma NULL; imagen restaurada; verificado por auditor). | `gpio_f1c_t3_f733b98.txt`, auditor: `gpio_f1c_audit_t3_7ba2cf0.txt` |
| **T4** | Margen de retención del lock (Decisión 2) | Medición de `m7_/m4_gpio_lock_hold_cycles_max` por SWD tras el arranque: ambos > 0 y margen `>= 10.0x`. Límites derivados de `shared_memory.h` (24000/12000). Offsets derivados de DWARF y base de `g_shared` desde `nm` (`0x380084f0`, `0x380084f4`). | Mismo cálculo con límite 10x inferior (2400 / 1200 ciclos) da margen `< 10x` y falla. | PASS (CM7: 1274 ciclos / 18.84x; CM4: 363 ciclos / 33.06x; control negativo 1.88x / 3.31x, ambos < 10x). | `gpio_f1c_t4_f733b98.txt` |
| **T5** | Regresión de producción (debug y release) | Flasheo con descarga verificada; ICMP 4/4; UDP sweep 1..1472 B idénticos; UDP burst 100/100; todas las líneas `[net]` con `rx_nobuf=0`, `tx_nobuf=0`, `dma_err=0`; telemetría ADC continua con `Samples` creciente y `OVR == 0`. | — | PASS en debug y release (verificado independientemente por auditor con red y query). | `gpio_f1c_t5_debug_f733b98.txt`, `gpio_f1c_t5_release_f733b98.txt`, auditor: `gpio_f1c_audit_t5_netcheck_7ba2cf0.txt` |
| **T6a** | Arranques repetidos por SWD (20 resets) | 20 resets secuenciales con `-rst`, espera de 10s tras cada uno, ejecución de T3 completo tras cada reset. Flasheo inicial verificado. | — | PASS (20 de 20 resets con 10/10 comprobaciones T3 válidas tras cada uno; verificado independientemente por auditor). | `gpio_f1c_t6a_f733b98.txt`, auditor: `gpio_f1c_audit_t6a_7ba2cf0.txt` |
| **T6b** | Arranque en frío (corte de alimentación física) | Repetición de T3 y T5 tras arranque en frío con verificación de bits `RCC_RSR`. | — | PENDIENTE por banco en remoto (G-S7). No bloqueó la entrega de F1; lo ejecutará el responsable en el banco para el cierre definitivo. | Ver §5.8 |
| **T7** | Compatibilidad con `prj_04`: `--project prj_04 --test t2` | `run_t2a` y `run_t2b` sobre `prj_04`: 1951 unidades escaneadas en 6 presets con 0 escrituras ilegales; control en `07db657` detecta las 20 escrituras por `(preset, ruta_relativa)` con rutas y líneas exactas. | Las 5 ELFs CM7 de `07db657` fallan el oráculo T2a. | PASS (1951 unidades limpias, 20 hallazgos en control, autoprueba 12/12 y 0/9; verificado por auditor). | `gpio_f1c_t7_e9e98c4.txt` (en `prj_04/docs/`) |

---

## 5.4 Escritores de GPIO de `prj_03` y comprobación de pines (G-S1)

### 5.4.1 Escritores antes (`e6cb75e`) y después (`e9e98c4`)

| Núcleo | Archivo | Puerto / Pines | Momento | Estado en `e6cb75e` (antes) | Estado en `e9e98c4` (después) |
| --- | --- | --- | --- | --- | --- |
| **CM7** | `prj_03/CM7/Core/Src/main.c` | PB0, PE1, PB14 (LEDs de usuario) | Antes de liberar CPU2 (`main.c:46`) | `LED_Init()` mono-núcleo (sin lock) | `LED_Init()` mono-núcleo (sin lock; seguro, CPU2 dormido) |
| **CM7** | `prj_03/CM7/Core/Src/main.c` | PB14 (LED rojo de pánico) | Excepción de pánico / HardFault (`main.c:134`) | `LED_PanicRed` escritura directa `MODER` sin lock | `LED_PanicRed` escritura directa `MODER` sin lock (excepción terminal aceptada G14) |
| **CM7** | `01_shared_drivers/08_eth/eth_hal.c` | PA1, PA2, PA7, PB13, PC1, PC4, PC5, PG11, PG13 (RMII) | **Después de liberar CPU2** (`app_main()` en `main.c:123`, tras liberación en l. 110-111) | `GPIO_HAL_Init()` con hook `NULL` (sin lock activo; **riesgo de carrera G1 en `GPIOB` con CM4**) | `GPIO_HAL_Init()` protegido por `s_shared_lock` (HSEM 5) |
| **CM7** | `01_shared_drivers/04_uart/uart_hal.c` | PD8, PD9 (USART3 TX/RX) | **Después de liberar CPU2** (`app_main()` en `main.c:123`, tras liberación en l. 110-111) | `GPIO_HAL_Init()` con hook `NULL` en `e6cb75e` (migrado en F1b) | `GPIO_HAL_Init()` protegido por `s_shared_lock` (HSEM 5) |
| **CM4** | `prj_03/CM4/App/Src/adc_acquisition.c` | PB1 (ADC_INP5 analógico) | **Después de liberar CPU2** (`ADC_Acquisition_Init` en super-loop) | **`HAL_GPIO_Init(GPIOB, &s)` SIN PROTECCIÓN (G1)** | **`GPIO_BM_ConfigPin()` protegido por `s_shared_lock` (HSEM 5) (G1 RESUELTO)** |
| **CM4** | `prj_03/CM4/App/Src/adc_selftest_hw.c` | PB1, PA3 (canales analógicos ADC) | Solo en preset `selftest` (tras despertar CPU2) | Drivers `07_adc` / `adc_hal` | Drivers `07_adc` / `adc_hal` (`ADC_SelfTestHw_Run`, preset `selftest` solo compilado) |

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

- **M1 (Corregida en `e9e98c4`):** La entrega inicial de la herramienta (`f733b98`) asignaba `prj_03` por defecto a `--project` y no bloqueaba la ejecución si el directorio de control correspondía a un commit distinto del esperado para el proyecto. En `e9e98c4`, `--project` es estrictamente obligatorio (`required=True`), y la herramienta valida que el commit del control coincida con el esperado (`07db657` para `prj_04`, `e6cb75e` para `prj_03`), abortando con FAIL y código 1 en caso de discrepancia.
- **Entorno de control:** El worktree de control `../stm32h755_control_e6cb75e` se configuró en el commit base limpio `e6cb75e`. Para optimizar almacenamiento y evitar descargas redundantes, el submódulo `00_Cube/STM32CubeH7` se enlazó mediante junction al árbol principal.

---

## 5.6 Respuesta a la auditoría (AUDIT.md `7ba2cf0`)

En la auditoría de `7ba2cf0`, el auditor dictaminó **F1 pasa, con condiciones**, verificando que el código de producción es correcto y completo, y que cada comprobación en placa y enlace estaba respaldada por ejecuciones independientes. Se han resuelto todos los hallazgos M1–M3 y m1–m6 para el cierre formal:

- **M1 (Mayor):** `--project` es ahora obligatorio en `gpio_f1b_checks.py`. Se valida que el commit de control coincida con el esperado para el proyecto (`07db657` para `prj_04`, `e6cb75e` para `prj_03`); si no coincide, aborta inmediatamente como FAIL con código 1.
- **M2 (Mayor):** Se eliminó la afirmación de `net_check` tras T6a en §5.8.3, ya que la captura propia de T6a no incluía `net_check`. Se cita la captura independiente del auditor `gpio_f1c_audit_t5_netcheck_7ba2cf0.txt` que verificó la red inmediatamente tras los 20 resets.
- **M3 (Mayor):** Se corrigió la tabla §5.4.1 reflejando la secuencia temporal exacta del código fuente: las MSP de USART3 y Ethernet corren en `app_main()` en `main.c:123`, **después** de liberar CPU2 en las líneas 110-111 (razón fundamental de la carrera inter-núcleo G1); `LED_Init()` corre mono-núcleo en `main.c:46` antes de liberar CPU2; se añadieron `LED_PanicRed` (G14, handler de pánico) y el escritor de `selftest` (`ADC_SelfTestHw_Run`).
- **m1 (Menor):** Se actualizó T5 para imprimir una nota contextualizada y verdadera por proyecto: en `prj_03`, `OVR` reporta `half_overrun_count` y `[net]` no contiene `acl_drop`; en `prj_04` se mantiene la descripción correspondiente a su telemetría.
- **m2 (Menor):** Todas las capturas se generan de forma íntegra en una sola ejecución continua sin concatenación manual.
- **m3 (Menor):** T1 ejecuta una compilación limpia (`build_all.ps1 -Clean`, y `--fresh` para `prj_03 selftest` y `prj_04 debug`) imprimiendo los comandos exactos y verificando que `-Werror` se aplica estrictamente en CM7 y CM4 con 0 avisos del compilador.
- **m4 (Menor):** T4 imprime explícitamente el origen de cada dirección: base de `g_shared` obtenida de `nm` + desplazamientos relativos extraídos de DWARF.
- **m5 (Menor):** En la herramienta, la tabla `PROJECTS_CONFIG` define `t3_pins` y el formato de query por proyecto; se eliminó el caso redundante `autoprueba_gate` del control de `prj_03`; los casos de status de CM7 en T2a se contabilizan en el veredicto; y el docstring del módulo se actualizó para describir F1c y `--project`.
- **m6 (Menor):** En el informe, se corrigió la indicación sobre la comparación de rutas en T2b: la herramienta utiliza expresiones regulares sobre la ruta relativa normalizada (no `Path.relative_to`); en la tabla de T3 (§5.3), se corrigieron los valores leídos de `s_shared_lock` a los punteros de función reales `0x8015B80` (CM7) y `0x81052F0` (CM4).

---

## 5.7 Erratas del plan

- **Ninguna errata encontrada:** El plan rev. 1 se ajustó con total precisión a la arquitectura de `prj_03` y a las herramientas de prueba.

---

## 5.8 Lo abierto y lo no ejercitado

1. **T6b (arranque en frío):** Pendiente de ejecución en el banco físico por el responsable (G-S7). Conforme al plan (§112 y §140), no bloqueó la entrega de F1, pero sí es requisito indispensable para el cierre formal de la fase. Lo ejecutará el responsable con acceso físico a la placa.
2. **Preset `selftest` de `prj_03`:** Compilado con 0 errores y 0 avisos en T1 y escaneado con 324 unidades limpias en T2b. No se flashea en placa física por regla de seguridad G-S1 (`ADC_SelfTestHw` manipula pines y canales de hardware de los ADCs).
3. **Imagen que quedó en la placa (G-S6):** La placa física quedó flasheada y en ejecución con la imagen de producción **`prj_03_adc_dma_eth` preset `debug` (commit `7ba2cf0`)**, verificada independientemente por el auditor mediante SWD (`docs/gpio_f1c_audit_swd_7ba2cf0.txt`) y conectividad de red tras los 20 resets (`docs/gpio_f1c_audit_t5_netcheck_7ba2cf0.txt`).
