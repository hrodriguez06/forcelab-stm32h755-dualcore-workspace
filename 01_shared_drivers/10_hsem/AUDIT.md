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
