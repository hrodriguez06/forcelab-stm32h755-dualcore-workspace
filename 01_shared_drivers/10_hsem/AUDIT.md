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
