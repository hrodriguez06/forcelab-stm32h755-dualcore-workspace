# Registro de Auditoría: Driver HSEM (10_hsem)

Este documento registra los resultados de auditoría independientes para cada una de las fases (F0 a F5) del plan de implementación del driver `10_hsem`, según el protocolo definido en `IMPLEMENTATION_PLAN.md`.

---

## Fase F0: Base y Caracterización de Silicio

- **Fecha:** Sep 26, 2026
- **Rama:** `driver/hsem`
- **Plataforma:** NUCLEO-H755ZI-Q (STM32H755ZI Silicon Rev V, ID 0x450, ST-LINK/V3 en COM4)
- **Estado de Fase F0:** **APROBADA (GATE PASSED)**

### 1. Entregables y Acciones de la Fase F0

1. **Tabla de tamaños de referencia (8 builds con -Werror):**
   Ejecución limpia con `tools/build_all.ps1 -Clean`:
   * `prj_01_template_dualcore` [debug]: CM7 83,520 B, CM4 15,092 B
   * `prj_01_template_dualcore` [release]: CM7 82,472 B, CM4 14,844 B
   * `prj_02_eth_lwip_dualcore` [debug]: CM7 205,980 B, CM4 15,092 B
   * `prj_02_eth_lwip_dualcore` [release]: CM7 202,004 B, CM4 14,844 B
   * `prj_03_adc_dma_eth` [debug]: CM7 208,516 B, CM4 36,364 B
   * `prj_03_adc_dma_eth` [release]: CM7 204,476 B, CM4 34,208 B
   * `prj_04_adc_dma_bm_eth` [debug]: CM7 209,236 B, CM4 24,184 B
   * `prj_04_adc_dma_bm_eth` [release]: CM7 205,292 B, CM4 23,796 B
   Con `HSEM_DRIVER_SELFTEST=OFF` (por defecto), los binarios conservan **exactamente el mismo tamaño al byte**.

2. **Capturas de referencia de arranque y 20 resets en caliente:**
   * `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/warm_resets_reference.txt`: 20/20 resets con `boot=OK`, `CPU2 alive` en cada uno y relación de incrementos m4/m7 = 20/4 = 5.0 constante.
   * `03_firmware_templates/prj_01_template_dualcore/docs/warm_resets_reference.txt`: 20/20 resets con `boot=OK`, `CPU2 alive` y relación de incrementos m4/m7 = 15/4 = 3.75 (documentando la divergencia S5 antes de migración).

3. **Comprobación de DWT CYCCNT en ambos núcleos (sin depurador conectado):**
   * CM7: Inicialización con desbloqueo de `DWT_LAR` (`0xE0001FB0 = 0xC5ACCE55`) y `CoreDebug_DEMCR_TRCENA`. Delta medido: 4020 ciclos. `cm7_dwt_ok = 1`.
   * CM4: Inicialización con `CoreDebug_DEMCR_TRCENA` y `CYCCNTENA`. Delta medido: 5521 ciclos. `cm4_dwt_ok = 1`.

4. **Comprobación de lectura del banco RLR desde el depurador:**
   * Lectura SWD HOTPLUG de `0x58026480` (HSEM_RLR[0]): devolvió `0x80000300` (LOCK=1, COREID=3, PROCID=0).
   * La lectura por depuración toma el semáforo como bus master 0 (CPU1). Confirmado que los volcados de depuración deben restringirse a solo HSEM_R (`0x58026400-0x5802647C`).

5. **Esqueleto de Selftest:**
   * Creada opción `HSEM_DRIVER_SELFTEST` en `prj_04/CMakeLists.txt`.
   * Añadido preset `selftest-hsem` en `prj_04/CMakePresets.json`.
   * Archivos creados: `hsem_selftest_shared.h`, `CM4/App/Src/hsem_selftest.c`, `CM4/App/Inc/hsem_selftest.h`, `CM7/App/Src/hsem_selftest.c`, `CM7/App/Inc/hsem_selftest.h`.

6. **Control Negativo H2 (sin driver, registros a mano):**
   * Parte A (Contador sin lock): N = 100,000 por núcleo (esperado 200,000). Resultado final = 156,378. Pérdida de incrementos = 43,622 (> 0, **PASS**).
   * Parte B (Registro roto sin lock): N = 50,000 lecturas. Lecturas rotas observadas = 15,408 (> 0, **PASS**).
   * Evidencia registrada en `docs/hsem_selftest_capture.txt`.

7. **Benchmark B1 Parte 1:**
   * Tiempos crudos registrados en `docs/hsem_b1_baseline.txt`:
     - CM7 (400 MHz): FastTake = 16 cyc (40 ns), Take = 30 cyc (75 ns), Release = 3 cyc (7.5 ns).
     - CM4 (200 MHz): FastTake = 8 cyc (40 ns), Take = 24 cyc (120 ns), Release = 6 cyc (30 ns).
   * Retención en prj_04: HSEM 4 (ADC) ~1.35 µs; HSEM 3 (Mailbox con S11 en CM7) hasta ~6-12 ms.

### 2. Tabla de Cumplimiento de Criterios (Fase F0)

| # | Criterio | Comprobación | Estado |
|---|----------|--------------|--------|
| A1 | Reglas del catálogo | Selftest aislado, sin modificar drivers existentes | PASS |
| A2 | C1: Sin IDs fijos en driver | No hay driver aún; selftest usa IDs 24..31 | PASS |
| A4 | C6: Validación previa | Validada empíricamente en hardware | PASS |
| A5 | C7: Sin estado en BM/LL | Estructuras selftest en SRAM4 | PASS |
| A12 | C9: Matriz de compilación | 8/8 PASSED con `build_all.ps1 -Clean` | PASS |
| A14 | Pruebas de fase | Flasheo de `selftest-hsem` y captura en hardware | PASS |
| A15 | R1: Control negativo H2 | H2 pierde 43,622 incrementos y 15,408 lecturas rotas | PASS |
| A16 | R2: Trazabilidad | Hash git presente en salida de selftest | PASS |
| A17 | R4: No destructivo | Selftest en IDs 24..31; app continúa operativa | PASS |

Gate de salida de F0 completado y validado. Listo para iniciar Fase F1 (`hsem_bm` núcleo).

---

## Fase F1: Núcleo `hsem_bm` (Bare-Metal)

- **Fecha:** Sep 26, 2026
- **Rama:** `driver/hsem`
- **Rango de Commits:** `a8ec2a2..5f78246`
  - `a8ec2a2`: `feat(hsem): implement hsem_bm bare-metal driver core`
  - `86aa56c`: `test(prj_04): add F1 BM selftest suite (H0, H1, H1b, H3, H4, H7, H8, H9)`
  - `5f78246`: `test(prj_04): record F1 selftest hardware evidence`
- **Plataforma:** NUCLEO-H755ZI-Q (STM32H755ZI Silicon Rev V, ID 0x450, ST-LINK/V3 en COM4)
- **Estado de Fase F1:** **APROBADA (GATE PASSED)**

### 1. Entregables y Acciones de la Fase F1

1. **Implementación del driver `hsem_bm`:**
   - Creados `01_shared_drivers/10_hsem/hsem_bm.h` y `01_shared_drivers/10_hsem/hsem_bm.c`.
   - Registrado target `stm32h7_driver_hsem_bm` (INTERFACE) en `01_shared_drivers/CMakeLists.txt`.
   - API de funciones públicas de BM completada y congelada (C2).

2. **Verificación de C7 (.bss = 0, .data = 0 en BM):**
   `arm-none-eabi-size` sobre `hsem_bm.c.obj`:
   - CM7 (Cortex-M7): `text=1460, data=0, bss=0`
   - CM4 (Cortex-M4): `text=1416, data=0, bss=0`
   - Cero estado en RAM global / estática: 100% verificado.

3. **Verificación de C10 y C11 (objdump & grep):**
   - Accesos a registros HSEM son exclusivamente de palabra de 32 bits (`ldr.w`, `str.w`). Cero instrucciones `ldrb/strb/ldrh/strh` hacia el periférico HSEM (`0x58026400`).
   - Instrucciones `dmb sy` generadas tras `FastTake`/`Take` con éxito y antes del `str` de liberación en `Release` y `ReleaseAllOfCore`.
   - Lectura de `HSEM_RLR` restringida a `HSEM_BM_FastTake` y `HSEM_BM_TakeOrNotify`. `HSEM_BM_GetOwner` e `IsLocked` leen `HSEM_R`.

4. **Verificación de C12 y C8:**
   - PRIMASK es guardado con `__get_PRIMASK()` y restaurado con `__set_PRIMASK()`. Cero llamadas a `__enable_irq()`.
   - Cero dependencias hacia capas superiores: libre de FreeRTOS, `cmsis_os` y HAL en `hsem_bm.*`.

5. **Matriz de compilación CI (8/8 builds con -Werror):**
   Ejecución con `tools/build_all.ps1 -Clean`:
   - `prj_01_template_dualcore` [debug]: CM7 83,520 B (delta: 0 B), CM4 15,092 B (delta: 0 B)
   - `prj_01_template_dualcore` [release]: CM7 82,472 B (delta: 0 B), CM4 14,844 B (delta: 0 B)
   - `prj_02_eth_lwip_dualcore` [debug]: CM7 205,980 B (delta: 0 B), CM4 15,092 B (delta: 0 B)
   - `prj_02_eth_lwip_dualcore` [release]: CM7 202,004 B (delta: 0 B), CM4 14,844 B (delta: 0 B)
   - `prj_03_adc_dma_eth` [debug]: CM7 208,516 B (delta: 0 B), CM4 36,364 B (delta: 0 B)
   - `prj_03_adc_dma_eth` [release]: CM7 204,476 B (delta: 0 B), CM4 34,208 B (delta: 0 B)
   - `prj_04_adc_dma_bm_eth` [debug]: CM7 209,236 B (delta: 0 B), CM4 24,184 B (delta: 0 B)
   - `prj_04_adc_dma_bm_eth` [release]: CM7 205,292 B (delta: 0 B), CM4 23,796 B (delta: 0 B)
   Todas las 8 compilaciones pasaron limpiamente sin warnings. Cero sobrecoste de memoria en las configuraciones por defecto.

6. **Resultados de las pruebas en hardware (NUCLEO-H755ZI-Q, commit `86aa56c` limpio):**
   - **H0 (Caracterización y rechazo de parámetros C6):** PASS en ambos núcleos. COREID reporta 3 en CM7 y 1 en CM4. Rechazos de IDs inválidos (≥ 32), PROCIDs inválidos (0 en Take, 256), clave 0 y núcleos inválidos devuelven `HSEM_BM_ERR_PARAM`.
   - **H1 (Exclusión mutua en contención):** 1,000,000 de incrementos por núcleo (2,000,000 total). Total observado = 2,000,000. Pérdida = 0 (**PASS**, exacto, frente a 43,622 pérdidas observadas en el control negativo H2).
   - **H1b (Coherencia en registro multi-palabra):** 1,000,000 de lecturas de array de 8 palabras compartidas bajo lock. Lecturas rotas = 0 (**PASS**, frente a 15,408 en el control H2).
   - **H3 (Protección de liberación):** Liberación con PROCID erróneo o núcleo ajeno es ignorada en hardware y rechazada con `HSEM_BM_ERR_NOT_OWNER` (**PASS**).
   - **H4 (Timeout acotado en ciclos):** Espera con timeout acotado de 10 ms; tiempo medido con SysTick = 10 ms exactos, `relax` llamado 36,272 veces, estado devuelto = `HSEM_BM_ERR_TIMEOUT` (**PASS**).
   - **H7 (Detección de colisión en mismo núcleo - S1):** Detección atómica de reentrancia no permitida bajo PRIMASK; devuelve `HSEM_BM_ERR_BUSY_SELF` evitando corrupción de exclusión (**PASS**).
   - **H8 (Recuperación con clave):** Liberación masiva de semáforos de CM4 con `ReleaseAllOfCore` y clave de desbloqueo válida; el semáforo 28 de CM7 permanece intacto (**PASS**).
   - **H9 (Notificaciones por interrupción):** 1,000/1,000 interrupciones de notificación transferidas sin pérdida ni interrupciones espurias (**PASS**).
   - Continuidad de la aplicación: Tras la ejecución de los selftests (IDs 24..31), el enlace Ethernet (100 Mbps FD) y las tareas de la aplicación continúan respondiendo normalmente.

### 2. Tabla de Cumplimiento de Criterios (Fase F1)

| # | Criterio | Comprobación | Estado |
|---|----------|--------------|--------|
| A1 | Reglas del catálogo | API limpia, Doxygen completo, tipos bien definidos | PASS |
| A2 | C1: Sin IDs fijos en driver | `grep -nE "HSEM_ID_\|BOARD_\|shared_memory\|0xA5C3" 10_hsem/` = 0 | PASS |
| A3 | C2: API congelada tras F1 | Cabecera `hsem_bm.h` completa para todos los contratos de BM | PASS |
| A4 | C6: Validación previa | Todo parámetro verificado antes de tocar `HSEM->` | PASS |
| A5 | C7: Sin estado en BM/LL | `.bss = 0`, `.data = 0` en CM4 y CM7 | PASS |
| A6 | C8: Dependencias hacia abajo | `hsem_bm` solo depende de CMSIS (`stm32h7xx.h`) | PASS |
| A7 | C10: Barreras y accesos 32-bit | `dmb` tras take y antes de release; solo accesos palabra | PASS |
| A8 | C11: RLR solo donde toca | `RLR` únicamente en `FastTake` y `TakeOrNotify` | PASS |
| A9 | C12: PRIMASK guardado y restaurado | Cero `__enable_irq`; restaura PRIMASK previo | PASS |
| A10 | C13: Esperas acotadas | Timeouts en ciclos de DWT con hook `relax` | PASS |
| A11 | Símbolos del proyecto | No define `HSEMx_IRQHandler` ni `HAL_HSEM_FreeCallback` | PASS |
| A12 | C9: Matriz de compilación | 8/8 PASSED con `build_all.ps1 -Clean` (-Werror) | PASS |
| A13 | R3: Tamaños | Binarios con selftest apagado conservan tamaño de F0 | PASS |
| A14 | Pruebas de fase | H0, H1, H1b, H3, H4, H7, H8, H9 ejecutadas y capturadas | PASS |
| A15 | R1: Controles negativos | H2 pierde 43,622 incrementos; H1 pierde 0 exactos | PASS |
| A16 | R2: Trazabilidad | Hash git `86aa56c` (sin -dirty) en la captura de hardware | PASS |
| A17 | R4: No destructivo | Selftest en IDs 24..31; app (ETH/ADC) sigue operativa | PASS |
| A18 | Oráculo independiente | H1 usa contadores privados, H4 SysTick, H9 recuento | PASS |
| A19 | Matriz de cumplimiento | RM0399 / ES0445 / ARMv7-M cubierta para BM | PASS |
| A20 | Documentación | Registrada en `AUDIT.md` y captura en `docs/` | PASS |

Gate de salida de F1 completado y validado. Listo para iniciar Fase F2 (Arranque y Migración).

---

## Fase F2: Arranque y Migración de Proyectos

- **Fecha:** Sep 26, 2026
- **Rama:** `driver/hsem`
- **Rango de Commits:** `9616e98..6f1692f`
  - `9616e98`: `refactor(boot): migrate prj_04 and dualcore template to HSEM_BM boot handshake`
  - `3a43823`: `fix(boot): resolve compilation warnings and EXTI register in CM7 main`
  - `cb62eb6`: `fix(hsem): improve boot handshake STOP re-entry and H5 verification sequence`
  - `c557c6e`: `fix(hsem): correct boot sequence ordering so D2 domain remains in DSTOP during H5 verification`
  - `0748c71`: `fix(hsem): test H5 delay and premature events early while D2 domain is in DSTOP`
  - `eed2be7`: `fix(template): remove duplicate Shared_Init in CM7 app_main after HSEM boot release`
  - `2b792a8`: `test(prj_04): record F2 passing selftest capture`
  - `6f1692f`: `test(hsem): record H6 20 warm resets for prj_04 and dualcore template`
- **Plataforma:** NUCLEO-H755ZI-Q (STM32H755ZI Silicon Rev V, ID 0x450, ST-LINK/V3 en COM4)
- **Estado de Fase F2:** **APROBADA (GATE PASSED)**

### 1. Entregables y Acciones de la Fase F2

1. **Implementación de funciones de handshake de arranque:**
   - Implementadas `HSEM_BM_BootWaitForCpu1`, `HSEM_BM_BootWaitCpu2Parked` y `HSEM_BM_BootReleaseCpu2` en `01_shared_drivers/10_hsem/hsem_bm.c`.
   - CPU2 (`BootWaitForCpu1`): Configura `PWR_CPU2CR.PDDS_D2 = 0`, `SCB->SCR.SLEEPDEEP`, activa `SEVONPEND` y `HSEM C2IER[id]`, y ejecuta bucle `WFE`. Al despertar de forma prematura (e.g. H5b o H5c), limpia `C2ICR` antes de re-aparcar en STOP para permitir el sueño profundo. Al despertar legítimo, limpia interrupción, deshabilita `C2IER[id]` y restaura `SCB->SCR` a su valor previo (C12).
   - CPU1 (`BootWaitCpu2Parked` y `BootReleaseCpu2`): `BootWaitCpu2Parked` espera hasta que `RCC_CR.D2CKRDY == 0` con timeout en ciclos. `BootReleaseCpu2` ejecuta `HSEM_BM_FastTake` con verificación de retorno, emite `__DMB()`, libera el semáforo y espera hasta que `D2CKRDY == 1` o agote `max_cycles`.

2. **Migración de `prj_04_adc_dma_bm_eth` y plantilla `prj_01_template_dualcore`:**
   - `SHARED_LOCK`/`SHARED_UNLOCK` migrados a macros transparentes sobre `HSEM_BM_FastTake` y `HSEM_BM_Release`.
   - Incorporada `Shared_TryLockCycles(id, max_cycles)` y migrados `adc_acquisition.c` y `adc_server.c` con tracking de fallos en `g_shared.adc_lock_failures`.
   - S11 remediado en `Status_Task`: copia local del buzón y liberación inmediata de HSEM 3 antes de la invocación de `APP_LOG`.
   - S5 corregido en `prj_01_template_dualcore`: CM4 ya no diverge en el arranque; relación de heartbeats normalizada.
   - Eliminada doble llamada a `Shared_Init` en `app_main.c` de la plantilla.
   - Secuencia de arranque en `CM7/Core/Src/main.c`: verificación de H5 (500 ms de delay), H5b (liberación antes de magic) y H5c (evento espurio EXTI) ejecutadas antes de activar periféricos D2, garantizando que el dominio D2 permanezca en DSTOP durante las pruebas.
   - Verificación estricta de aislamiento: `git diff main` sobre `prj_02`, `prj_03` y `prj_eth_lwip_node` es exactamente 0.

3. **Matriz de compilación CI (8/8 builds con -Werror):**
   Ejecución limpia con `tools/build_all.ps1 -Clean`:
   - `prj_01_template_dualcore` [debug]: CM7 84,140 B (+620 B), CM4 14,916 B (-176 B)
   - `prj_01_template_dualcore` [release]: CM7 83,048 B (+576 B), CM4 14,700 B (-144 B)
   - `prj_02_eth_lwip_dualcore` [debug]: CM7 205,980 B (delta: 0 B), CM4 15,092 B (delta: 0 B)
   - `prj_02_eth_lwip_dualcore` [release]: CM7 202,004 B (delta: 0 B), CM4 14,844 B (delta: 0 B)
   - `prj_03_adc_dma_eth` [debug]: CM7 208,516 B (delta: 0 B), CM4 36,364 B (delta: 0 B)
   - `prj_03_adc_dma_eth` [release]: CM7 204,476 B (delta: 0 B), CM4 34,208 B (delta: 0 B)
   - `prj_04_adc_dma_bm_eth` [debug]: CM7 210,108 B (+872 B), CM4 24,552 B (+368 B)
   - `prj_04_adc_dma_bm_eth` [release]: CM7 206,176 B (+884 B), CM4 24,116 B (+320 B)
   Todas las 8 compilaciones pasaron exitosamente sin warnings. El crecimiento de CM4 release en `prj_04` es de solo +320 bytes (muy inferior al umbral de 1 KB).

4. **Resultados de las pruebas en hardware (NUCLEO-H755ZI-Q, commit `0748c71` limpio):**
   - **H5 (Boot Handshake):** CPU1 retarda 500 ms mientras muestrea `D2CKRDY == 0` continuamente; H5b (liberación sin magic) verifica que CPU2 re-aparca en STOP (`D2CKRDY == 0`); H5c (disparo de evento software EXTI línea 23 a CPU2) verifica que CPU2 ignora el evento y re-aparca; liberación formal con magic hace que `D2CKRDY` suba a 1 y CPU2 comience ejecución (**PASS**).
   - **H6 (20 Warm Resets consecutivos en prj_04):** 20/20 arranques exitosos con `CPU2 alive` en cada uno; relación de contadores `m7=4, m4=20` (ratio 5.0) idéntica a la referencia de F0 (**PASS**). Evidencia guardada en `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/warm_resets_f2.txt`.
   - **H6 (20 Warm Resets consecutivos en plantilla dualcore):** 20/20 arranques exitosos con `CPU2 alive` en cada uno; relación de contadores `m7=1, m4=1` (ratio 1.0 constante), confirmando la resolución definitiva de S5 (**PASS**). Evidencia guardada en `03_firmware_templates/prj_01_template_dualcore/docs/warm_resets_f2.txt`.
   - **Conjunto de pruebas BM (H0, H1, H1b, H3, H4, H7, H8, H9):** 100% PASS en la suite completa de `selftest-hsem`. 2,000,000 incrementos bajo lock con 0 pérdidas y 1,000,000 lecturas con 0 lecturas rotas.

### 2. Tabla de Cumplimiento de Criterios (Fase F2)

| # | Criterio | Comprobación | Estado |
|---|----------|--------------|--------|
| A1 | Reglas del catálogo | Contratos de handshake y boot documentados y limpios | PASS |
| A2 | C1: Sin IDs fijos en driver | `sem_id` proviene exclusivamente del llamador (`cfg->sem_id`) | PASS |
| A3 | C2: API congelada tras F1 | Cabecera `hsem_bm.h` sin cambios desde F1 (`git diff` vacío) | PASS |
| A4 | C6: Validación previa | Verificación de parámetros antes de acceder a hardware | PASS |
| A5 | C7: Sin estado en BM/LL | `.bss = 0`, `.data = 0` en CM4 y CM7 en release | PASS |
| A6 | C8: Dependencias hacia abajo | Cero dependencias de FreeRTOS o HAL en `hsem_bm.*` | PASS |
| A7 | C10: Barreras y accesos 32-bit | `dmb` verificado en objdump; accesos exclusivos de 32 bits a HSEM | PASS |
| A8 | C11: RLR solo donde toca | RLR leído únicamente en `FastTake` y `TakeOrNotify` | PASS |
| A9 | C12: PRIMASK guardado y restaurado | `SCB->SCR` y PRIMASK restaurados a su estado original | PASS |
| A10 | C13: Esperas acotadas | Timeouts en ciclos DWT (`max_cycles`) para CPU1 | PASS |
| A11 | Símbolos del proyecto | No define `HSEMx_IRQHandler` ni callbacks del proyecto | PASS |
| A12 | C9: Matriz de compilación | 8/8 PASSED con `build_all.ps1 -Clean` (-Werror) | PASS |
| A13 | R3: Tamaños | Deltas de tamaño documentados; diff nulo en prj_02/03 | PASS |
| A14 | Pruebas de fase | H5, H5b, H5c y H6 (20/20 resets en dos proyectos) PASS | PASS |
| A15 | R1: Controles negativos | H5b y H5c re-aparcan en STOP; H2 mantiene pérdida de incrementos | PASS |
| A16 | R2: Trazabilidad | Hash git sin -dirty en todas las capturas y suites | PASS |
| A17 | R4: No destructivo | Aplicaciones de red y adquisición operativas tras arranque | PASS |
| A18 | Oráculo independiente | `D2CKRDY` en H5; monitor serie externo para H6 | PASS |
| A19 | Matriz de cumplimiento | Handshake RM0399 §7/§9/§11/§21 y ARMv7-M cubiertos | PASS |
| A20 | Documentación | Registro en `AUDIT.md`, capturas en `docs/` | PASS |

Gate de salida de F2 completado y validado. Listo para iniciar Fase F3 (`hsem_ll`).

---

## Fase F3: Driver Low-Layer (`hsem_ll`) y Paridad BM ↔ LL

- **Fecha:** Sep 26, 2026
- **Rama:** `driver/hsem`
- **Rango de Commits:** `24ccb76..3091ee5`
  - `24ccb76`: `feat(hsem): implement hsem_ll low-layer driver and CMake target`
  - `a86756f`: `test(prj_04): add H10 BM-LL parity test to hsem selftest suite`
  - `617ec43`: `fix(hsem): partition test semaphores and reset ICR flags in H10 parity test`
  - `3091ee5`: `test(prj_04): record passing F3 capture for HSEM LL tier parity`
- **Plataforma:** NUCLEO-H755ZI-Q (STM32H755ZI Silicon Rev V, ID 0x450, ST-LINK/V3 en COM4)
- **Estado de Fase F3:** **APROBADA (GATE PASSED)**

### 1. Entregables y Acciones de la Fase F3

1. **Implementación del driver `hsem_ll` (`01_shared_drivers/10_hsem/`):**
   - `hsem_ll.h`: Cabecera autocontenida que compila de forma aislada sin requerir `USE_FULL_LL_DRIVER` ni incluir `stm32h7xx_hal.h`.
   - `hsem_ll.c`: Wrapper ligero sobre los helpers inline de ST LL (`LL_HSEM_1StepLock`, `LL_HSEM_2StepLock`, `LL_HSEM_ReleaseLock`, `LL_HSEM_EnableIT_C*`, `LL_HSEM_DisableIT_C*`, `LL_HSEM_ClearFlag_C*`, `LL_HSEM_GetStatus`).
   - Mismo retorno `HsemBm_Status_t` que el tier BM: `OK`, `BUSY`, `BUSY_SELF`, `TIMEOUT`, `ERR_PARAM`, `ERR_NOT_OWNER`, `ERR_TIMEBASE`.
   - Incorpora las protecciones críticas requeridas por RM0399 / ARMv7-M:
     - Detección de colisión en el mismo núcleo (S1) bajo PRIMASK previo a `LL_HSEM_1StepLock` y `LL_HSEM_2StepLock` devolviendo `BUSY_SELF`.
     - Barreras de memoria `__DMB()` tras locks exitosos y antes de release (C10).
     - Verificación previa de pertenencia en `HSEM_LL_Release` (C11) devolviendo `ERR_NOT_OWNER` si no pertenece al núcleo/proceso.
     - Esperas acotadas por ciclos DWT CYCCNT en `HSEM_LL_TakeTimeout` y `HSEM_LL_TakeTimeoutEx` (C13).
     - Limpieza de `CnICR` y manejo de PRIMASK en `NotifyEnable`, `NotifyDisable` y `NotifyTake`.
   - Huella en RAM (C7): Verificado con `arm-none-eabi-size` que `.bss = 0` y `.data = 0` tanto en CM7 (572 B text) como en CM4 (576 B text).
   - Accesos de 32 bits (C10): Verificado con `objdump` que no existen instrucciones `ldrb`, `ldrh`, `strb`, ni `strh`.

2. **Integración en CMake:**
   - Creado target `stm32h7_driver_hsem_ll` en `01_shared_drivers/CMakeLists.txt`.
   - Enlazado en `prj_04_adc_dma_bm_eth` en CM7 y CM4 bajo la opción `HSEM_DRIVER_SELFTEST`.

3. **Prueba de Paridad H10 (BM ↔ LL) en Hardware:**
   - Secuencia de 7 pasos ejecutada sucesivamente bajo BM y bajo LL:
     - Paso 0: `FastTake` de 1 paso.
     - Paso 1: `Take` de 2 pasos con proc_id 42.
     - Paso 2: `Release` con proc_id 42 (genera flag ISR en hardware).
     - Paso 3: `Release` con proc_id erróneo 99 (`ERR_NOT_OWNER`, sin escritura).
     - Paso 4: `Release` con proc_id correcto 0.
     - Paso 5: `NotifyEnable` (`CnIER` configurado, `CnICR` limpiado).
     - Paso 6: `NotifyDisable` (`CnIER` deshabilitado, `CnICR` limpiado).
   - Partición de semáforos de prueba: CM7 opera en semáforos 24, 25, 26; CM4 opera en semáforos 28, 29, 30.
   - Reseteo completo de periférico (`Hsem_ResetTestRegisters_M*`) antes de cada pasada: libera semáforos de prueba, deshabilita `CnIER = 0` y limpia flags `CnICR = 0xFFFFFFFFUL` bajo `__DSB(); __ISB();`.
   - Comparación estricta de fotos de registros (`R[24..31]`, `CnIER`, `CnISR`) y estados de retorno en cada paso: **PARIDAD EXACTA (PASS)** en CM7 y CM4.
   - Controles negativos (R1): Mutación deliberada de un bit en la foto de LL detectada como fallo de paridad en ambos núcleos.

4. **Captura en Hardware (commit `617ec43` sin -dirty):**
   - Registrada en `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/hsem_selftest_capture.txt`.
   - CM7: `[m7_hsem_test] H10 Parity (BM <-> LL): PASS`
   - CM4: `[m4_hsem_test] H7 same_core=PASS H9 notifications=1000 H10 parity=PASS`
   - Validación completa acumulada de F1, F2 y F3 (H0, H1 2M incs, H1b 1M lecturas, H3, H4, H5, H7, H8, H9, H10) en verde.

5. **Matriz de compilación CI (8/8 builds con -Werror):**
   - Ejecución limpia con `tools/build_all.ps1 -Clean`: las 8 configuraciones compilaron sin warnings ni errores.
   - `git diff main` sobre `prj_02`, `prj_03` y `prj_eth_lwip_node` es exactamente 0.

### 2. Tabla de Cumplimiento de Criterios (Fase F3)

| # | Criterio | Comprobación | Estado |
|---|----------|--------------|--------|
| A1 | Reglas del catálogo | Contratos LL consistentes con arquitectura del catálogo | PASS |
| A2 | C1: Sin IDs fijos en driver | Funciones reciben `id` y `proc_id` desde llamador | PASS |
| A3 | C2: API congelada | `hsem_bm.h` y `hsem_ll.h` estables y congeladas | PASS |
| A4 | C6: Validación previa | Verificación de límites antes de invocar registros LL | PASS |
| A5 | C7: Sin estado en BM/LL | `.bss = 0`, `.data = 0` en CM7 y CM4 verificado con `size` | PASS |
| A6 | C8: Dependencias hacia abajo | Autocontenido sin HAL en `hsem_ll.h` | PASS |
| A7 | C10: Barreras y accesos 32-bit | `dmb/dsb` presentes; 0 accesos de 8/16 bits en objdump | PASS |
| A8 | C11: RLR solo donde toca | RLR leído solo en `FastTake` | PASS |
| A9 | C12: PRIMASK guardado y restaurado | PRIMASK gestionado en S1, NotifyEnable/Disable/Take | PASS |
| A10 | C13: Esperas acotadas | `TakeTimeout` acotado por DWT CYCCNT | PASS |
| A11 | Símbolos del proyecto | No define `HSEMx_IRQHandler` ni callbacks de aplicación | PASS |
| A12 | C9: Matriz de compilación | 8/8 PASSED con `build_all.ps1 -Clean` (-Werror) | PASS |
| A13 | R3: Tamaños | Impacto nulo en builds de producción sin selftest | PASS |
| A14 | Pruebas de fase | H10 BM ↔ LL parity PASS en CM7 y CM4 | PASS |
| A15 | R1: Controles negativos | Mutación de foto detectada como fallo de paridad en CM7 y CM4 | PASS |
| A16 | R2: Trazabilidad | Captura registrada con hash limpio `617ec43` | PASS |
| A17 | R4: No destructivo | Selftest en IDs 24..30; ETH y adquisición funcionando | PASS |
| A18 | Oráculo independiente | Comparación bit a bit de snapshots de hardware | PASS |
| A19 | Matriz de cumplimiento | RM0399 §11, ES0445 y ARMv7-M cubiertos para LL | PASS |
| A20 | Documentación | Registrada en `AUDIT.md` y captura en `docs/` | PASS |

Gate de salida de F3 completado y validado. Listo para iniciar Fase F4 (`hsem_hal`).

