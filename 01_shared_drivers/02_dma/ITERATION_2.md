# Informe de Cierre: Iteración 2 del Driver DMA (`02_dma`)

- **Repositorio:** `stm32h755`
- **Rama:** `driver/dma-iteration-2` (base `b067fec`)
- **Fecha:** 25 de septiembre de 2026
- **Estado:** COMPLETADO — Todos los gates superados, verificado en hardware (salvo dos condiciones de fallo de hardware/HAL no inducibles en ejecución normal, detalladas en §7.4).

---

## 1. Resumen Ejecutivo

La **Iteración 2** del driver compartido DMA para STM32H755ZI (`01_shared_drivers/02_dma`) implementó y verificó las acciones correctivas y ampliaciones técnicas derivadas de la auditoría (`AUDIT.md` §3, §6, §7 y §9).

Se completaron todas las fases planificadas en [`IMPLEMENTATION_PLAN_2.md`](./IMPLEMENTATION_PLAN_2.md) y la subsanación del hallazgo bloqueante Z1:
- **G1 (Higiene y mantenimiento):** Actualización de comentarios obsoletos (X9), eliminación de comprobación muerta PFCTRL+circular y propagación de errores en `DMA_LL_Deinit` (X8), aserción estática de límites de ráfaga (X7a).
- **G2 (Robustez y tipado de APIs):** Resolución segura de interrupciones `GetIRQnEx` con centinela `DMA_BM_IRQN_INVALID` (X6), actualización de memoria inactiva en DBM con longitud explícita `SetIdleMemoryEx` y `DMA_LL_SetIdleMemory` (X11).
- **G3 (Alineación con RM0399 y corrección funcional):** Validación de ráfagas periféricas vía `DMA_BM_CheckPeriphBurst` según RM0399 §16.3.14 y relajación de alineación para `PINC=0` (X7b).
- **G4 (Verificación empírica en hardware y trazabilidad):** Trazabilidad de revisión `FW_GIT_REV` en banners de selftest (G4.1), test de desbordamiento no destructivo en regresión (Y3), test de coherencia D-Cache para `DMA_LL_MemCopy` en CM7 (Y4), test de stream ocupado en `CheckErrata` (Y5), matriz combinatoria exhaustiva de ráfagas FIFO en CM4 con oráculo independiente (X10a, Z1), y confirmación de acceso de BDMA a Backup SRAM en hardware (X10b).
- **G5 (Evidencia, documentación y cierre):** Captura limpia de hardware con hash exacto `rev=df46b94`, registro exhaustivo de 7 controles negativos mutados en silicio (R1), verificación de presupuesto de memoria flash en CM4 release (1.596 B $\le$ 1.676 B total `.text + .rodata`, 1.588 B `.text`), actualización de [`README.md`](./README.md) y validación en vivo de telemetría de producción a 500 kSPS sin errores.

---

## 2. Matriz de Puntos Auditados y Acciones Realizadas

| Punto | Categoría / Defecto | Acción Correctiva Implementada | Estado | Commit |
| :--- | :--- | :--- | :---: | :--- |
| **X9** | Comentarios obsoletos | Actualizados comentarios en `dma_bm.h`, `dma_bm.c` y `dma_ll.c` reflejando líneas de D-Cache de 32 B, bits TRBUFF y regla de unidades PSIZE de NDTR (RM0399 §16.3.12). | **CERRADO** | `4922386` |
| **X8** | Dead code PFCTRL y retorno en `DMA_LL_Deinit` | Eliminado check redundante `mode == CIRCULAR` dentro del bloque `mode == PFCTRL` en `dma_ll.c` (modos escalares mutuamente excluyentes; circular + PFCTRL no soportado en HW). Propagado fallo de parada de stream en `DMA_LL_Deinit()`. | **CERRADO** | `9dd4baa` |
| **X7a** | Comprobación ráfagas > 1 KB en runtime | Reemplazadas comprobaciones inalcanzables en runtime por un `_Static_assert` en `dma_bm.c`: `DMA_BM_MAX_BURST_BYTES <= 1024U`. | **CERRADO** | `4ab5c42` |
| **X6** | Sentinel `GetIRQn` colisionaba con `SysTick_IRQn` | Añadidas variantes seguras `DMA_BM_GetIRQnEx` y `DMA_HAL_GetIRQnEx` con retorno `bool` y puntero de salida `out`. Definido macro `DMA_BM_IRQN_INVALID` con advertencia `@warning`. | **CERRADO** | `d45877e` |
| **X11** | `SetIdleMemory` span basado en NDTR decreciente | Creado `DMA_BM_SetIdleMemoryEx()` con parámetro de longitud explícita y `DMA_LL_SetIdleMemory()` registrando `dbm_length` al arrancar. Comprobación de TEIF contra carreras de hardware al conmutar CT. | **CERRADO** | `52a73e0` |
| **X7b** | Regla de ráfaga periférica aplicada incorrectamente | Implementado `DMA_BM_CheckPeriphBurst()` aplicando RM0399 §16.3.14 ($PBURST \times PSIZE \le 16\text{ B}$, prohibido 3/4 si tamaño = 16 B). Relajada alineación para `PINC=0` a PSIZE. | **CERRADO** | `efce94e` |
| **G4.1** | Trazabilidad en selftest de hardware (R2) | Inyección automática de `FW_GIT_REV` en CMake (`CMakeLists.txt`) mostrada en el banner de arranque de ambos núcleos. | **CERRADO** | `6252ca4` |
| **Y3** | Test X2 destructivo en regresión | Modificado T9 caso 1c en CM7 con `bytes = 65568U` (múltiplo de 32 B). En regresión, el truncamiento de NDTR copia solo 32 B, dejando el canary en offset 64 intacto sin corromper memoria adyacente (R4). | **CERRADO** | `53bd261` |
| **Y4** | Coherencia D-Cache para `DMA_LL_MemCopy` en CM7 | Incorporado test T8b en CM7 ejecutando `DMA_LL_MemCopy` sobre `DMA2_Stream3` con PSIZE=WORD y MSIZE=BYTE (16 items = 64 bytes) verificando limpieza y lectura coherente en RAM física. | **CERRADO** | `0ca9844` |
| **Y5** | Test de stream ocupado en `CheckErrata` | Añadido caso T10b en CM7 forzando `EN=1` y verificando que `DMA_HAL_CheckErrata()` retorna `HAL_BUSY` sin modificar `CR`. | **CERRADO** | `64c4736` |
| **X10a** | Matriz combinatoria de bursts FIFO | Implementada matriz de 48 combinaciones PSIZE=MSIZE + 3 heterogéneas en CM4 T1. | **CERRADO** | `9018c76` |
| **X10b** | Acceso de BDMA a Backup SRAM en hardware | Caso T11b en CM7 verificó transferencias M2M bidireccionales vía BDMA entre SRAM4 (`0x38000000`) y Backup SRAM (`0x38800000`). Confirmada la conexión en la matriz AHB D3 (RM0399). | **CERRADO** | `4418037` |
| **Z1** | Oráculo independiente para matriz de bursts FIFO | Sustituido el oráculo tautológico por tabla `s_rm0399_burst_valid[3][4][4]` (RM0399 Tabla 116 y §16.3.14). Bucle sin aborto temprano; reporte completo `valid=23 ok=23 invalid=25 rej=25 mismatch=0`. | **CERRADO** | `df46b94` |
| **G5** | Documentación, capturas y verificación final | Generados `dma_selftest_capture.txt`, `dma_negative_controls.txt`, actualizado `README.md`, verificado tamaño release en CM4 (1.596 B total, 1.588 B `.text`) y comprobada telemetría de producción a 500 kSPS. | **CERRADO** | (HEAD) |

---

## 3. Registro Histórico de Commits

La rama `driver/dma-iteration-2` contiene los siguientes commits atómicos, convencionales y trazables sobre `main` (`b067fec`):

```
df46b94 test(prj_04): add independent oracle for FIFO burst matrix in T1
1bfc763 docs(dma): record the iteration-2 audit, blocked on Z1
c11e30c docs(dma): add iteration 2 closure report
b829467 docs(dma): update README for iteration 2 features and findings
4418037 test(dma): verify BDMA access to Backup SRAM on hardware
9018c76 test(dma): add exhaustive combinatorial matrix for FIFO bursts in T1
64c4736 test(dma): add CheckErrata busy test on CM7
0ca9844 test(dma): add CM7 cache-coherency test for DMA_LL_MemCopy
53bd261 test(dma): make the X2 test non-destructive on regression
6252ca4 build(prj_04): inject FW_GIT_REV into the selftest banner
efce94e fix(dma): validate peripheral burst with the RM0399 peripheral-side rule
52a73e0 feat(dma): add SetIdleMemoryEx with explicit length, and DMA_LL_SetIdleMemory
d45877e feat(dma): add GetIRQnEx variants and name the invalid IRQn sentinel
4ab5c42 refactor(dma): replace unreachable 1 KB burst checks with a static assert
9dd4baa fix(dma): propagate Stop failure from DMA_LL_Deinit and drop dead PFCTRL check
4922386 docs(dma): refresh stale 02_dma comments
```

---

## 4. Controles Negativos en Hardware (Regla R1)

Siguiendo estrictamente la regla R1, cada test añadido o modificado fue sometido a un control negativo en la placa física: el código del driver o test fue mutado para reintroducir el defecto, se compiló y flasheó el firmware con hash `-dirty`, y se capturó la línea exacta de fallo en UART antes de revertir la mutación con `git checkout` y verificar el pase limpio.

El registro completo con los bloques de terminal se encuentra en:
`02_firmware_projects/prj_04_adc_dma_bm_eth/docs/dma_negative_controls.txt`.

### Resumen de Controles Negativos
1. **X11 (`SetIdleMemoryEx`):** Mutación usando NDTR decreciente para validar alcance $\to$ Fallo capturado: `T2_REJ=FAIL` (`rev=d45877e-dirty`).
2. **X7b (`CheckPeriphBurst`):** Mutación usando `CheckFifoBurst` en el lado periférico $\to$ Fallo capturado: `T2_REJ=FAIL` (`rev=52a73e0-dirty`).
3. **Y3 (Test X2 no destructivo):** Mutación eliminando comprobación `items > 65535` en `DMA_HAL_MemCopy` $\to$ Fallo capturado: `T9_HAL_MEMCOPY_IT: FAIL` (`rev=6252ca4-dirty`), truncamiento a 32 B con canary en offset 64 intacto.
4. **Y4 (D-Cache en `DMA_LL_MemCopy`):** Mutación calculando bytes desde MSIZE en vez de PSIZE $\to$ Fallo capturado: `T8_DCACHE_COHERENCY: FAIL` (`rev=53bd261-dirty`).
5. **Y5 (`CheckErrata` con stream activo):** Mutación omitiendo comprobación de `EN` $\to$ Fallo capturado: `T10_USART3_DMA: FAIL` (`rev=53bd261-dirty`).
6. **X10a / Z1 (Matriz combinatoria de bursts con oráculo independiente):** Mutación en `DMA_BM_CheckFifoBurst` bajo BYTE + TH_1_2 haciendo que INCR8 retorne `false`. Oráculo detecta `mismatch=1`, bucle evalúa las 48 combinaciones completas $\to$ Fallo capturado: `T1 combos valid=23 ok=22 invalid=25 rej=25 mismatch=1`, `T1_M2M=FAIL`, `OVERALL=FAIL (mask=0xFC/0xFE)` (`rev=df46b94-dirty`).
7. **X10b (Acceso BDMA a Backup SRAM):** Mutación inhabilitando `0x38800000` en `dma_mem_bm.h` $\to$ Fallo capturado: `T11_BDMA_SRAM4: FAIL` (`rev=53bd261-dirty`).

#### Nota de trazabilidad sobre la regla R2
Los controles negativos Y4, Y5 y X10b fueron ejecutados y verificados en hardware sobre la revisión `53bd261-dirty` antes de realizar los commits de cada caso específico en el repositorio, debido a que se agruparon durante la sesión de trabajo interactiva. En contraste, el control negativo de Z1 fue ejecutado con trazabilidad estricta R2 sobre `df46b94-dirty` inmediatamente tras commitear Z1.

---

## 5. Captura de Selftest en Hardware y Trazabilidad

- **Archivo de captura:** `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/dma_selftest_capture.txt`
- **Revisión en silicio:** `rev=df46b94` (limpio, sin `-dirty`)

```
=== STM32H755ZI dual-core Ethernet / lwIP - CPU1 (Cortex-M7) ===
[sys] sysclk=400000000 Hz hclk=200000000 Hz
[sys] silicon revision id=0x2003
[m7_dma_test] rev=df46b94
[m7_dma_test] T8_DCACHE_COHERENCY: PASS
[m7_dma_test] T9_HAL_MEMCOPY_IT: PASS
[m7_dma_test] T10_USART3_DMA: PASS (TRBUFF verified)
[m7_dma_test] T10_USART3_DMA: PASS
[m7_dma_test] T11_BDMA_SRAM4: PASS
[m7_dma_test] OVERALL: PASS (T8-T11)
...
[m4_dma_test] rev=df46b94
[m4_dma_test] T1_M2M=PASS T2_REJ=PASS T3_UART=PASS
[m4_dma_test] T1 combos valid=23 ok=23 invalid=25 rej=25 mismatch=0
[m4_dma_test] T4_TIM7=PASS (tc=9,ht=10) T5_DBM=PASS
[m4_dma_test] T6_REARM=PASS T7_ITCM=PASS (te=1)
[m4_dma_test] OVERALL=PASS (mask=0xFE/0xFE)
```

---

## 6. Presupuesto de Memoria Flash (Regla R3)

Medición en `build/release/cm4_firmware.map` (excluyendo secciones descartadas por `--gc-sections` y secciones de depuración):

| Sección enlazada en FLASH | Dirección | Tamaño |
| :--- | :---: | :---: |
| `.text.DMA_MEM_BM_IsReachable.part.0` | `0x081016f8` | 40 B |
| `.text.DMA_BM_IsUartRequest` | `0x08101720` | 40 B |
| `.text.DMA_BM_CheckFifoBurst` | `0x08101748` | 76 B |
| `.text.DMA_BM_StopEx` | `0x08101794` | 52 B |
| `.text.DMA_BM_GetFlags` | `0x081017c8` | 160 B |
| `.text.DMA_BM_ClearFlags` | `0x08101868` | 172 B |
| `.text.DMA_BM_InitStreamEx` | `0x08101914` | 952 B |
| `.text.DMA_BM_InitStream` | `0x08101ccc` | 72 B |
| `.text.DMA_BM_Start` | `0x08101d14` | 24 B |
| `.rodata.s_burst_beats` | `0x08101ecc` | 4 B |
| `.rodata.s_stream_bit_shifts` | `0x08101ed0` | 4 B |
| **Total enlazado `dma_bm.c.obj` (`.text + .rodata`)** | | **1 596 B** |
| **Total código ejecutable (`.text` únicamente)** | | **1 588 B** |
| **Límite presupuestario (R3)** | | **$\le 1 676\text{ B}$** |
| **Margen disponible** | | **80 B libres** |

El total enlazado de `dma_bm.c.obj` es de 1 596 B (`.text + .rodata`). El código ejecutable (`.text`) mide estrictamente 1 588 B, complementado por 8 B de tablas constantes en `.rodata` (`s_burst_beats` de 4 B y `s_stream_bit_shifts` de 4 B).

---

## 7. Hallazgos en Silicio STM32H755ZI Rev V (Device 0x450, Rev ID 0x2003)

1. **Workaround TRBUFF (ES0445 2.20.3 / 2.3.12):**
   - Lectura directa de registro en hardware confirma que bit 20 (`TRBUFF`) es activado en `DMA_SxCR` tras `HAL_DMA_Init` para periféricos UART/HASH en revisión $\ge$ 0x2000.
   - `DMA_HAL_CheckErrata()` protege la configuración retornando `HAL_BUSY` si `CR.EN == 1`.
   - `HAL_DMA_Init()` deshabilita explícitamente el stream antes de llamar a la verificación, garantizando que el stream siempre esté inactivo al iniciar.

2. **Acceso de BDMA a Backup SRAM (RM0399 p. 109, 110, 118):**
   - Se validaron transferencias M2M bidireccionales entre SRAM4 (`0x38000000`) y Backup SRAM (`0x38800000`) sin fallos de bus.
   - Requiere habilitar `PWR->CR1 |= PWR_CR1_DBP` y `RCC->AHB4ENR |= RCC_AHB4ENR_BKPRAMEN`.
   - Se confirmó que el silicio cumple con la matriz de bus AHB D3 documentada en RM0399.

3. **Comportamiento de Ráfagas Periféricas en FIFO (RM0399 §16.3.14):**
   - Las ráfagas del puerto periférico en modo FIFO están restringidas por el límite de 16 B de la FIFO y no por las reglas de umbral de memoria.
   - Cuando el incremento periférico está desactivado (`PINC=0`), la alineación de la dirección periférica solo requiere ser múltiplo de `PSIZE`, permitiendo apuntar a registros periféricos de ancho fijo independientemente del número de beats de la ráfaga.

4. **Casos no ejercitables en hardware:**
   - **Fallo de parada en `DMA_LL_Deinit` (X8):** La propagación del fallo requiere que `DMA_LL_Stop()` no logre apagar el stream (`EN=1`) tras expirar el timeout, lo cual requeriría silicio físicamente dañado o un bloqueo catastrófico de bus.
   - **Propagación de `CheckErrata` retornando fallo desde `DMA_HAL_Init` (Y5):** `HAL_DMA_Init()` de STM32Cube HAL desactiva explícitamente el bit `EN` antes de invocar la comprobación de erratas, haciendo que el camino donde `DMA_HAL_CheckErrata()` retorna `HAL_BUSY` en inicialización normal sea inalcanzable en hardware no alterado.

---

## 8. Verificación Final en Producción (Preset `release`)

La placa queda programada con el firmware limpio del preset `release`:
- **CM4 release (`0x08100000`):** Adquisición ADC a 500 kSPS vía DMA en doble buffer.
- **CM7 release (`0x08000000`):** Servidor UDP de telemetría y stack TCP/IP lwIP en FreeRTOS.

### Telemetría en Vivo (`tools/query_adc.py`):
```
[*] Querying ADC telemetry from 192.168.1.150:5001 (Mode: TEXT)...
[0001] ADC1_CH5: raw=1255, avg=1126, rate=500000, samples=142479872, dma_tc=139140, dma_ht=139141, err=0, ovr=0, adc_ovr=0
[0005] ADC1_CH5: raw=1192, avg=1130, rate=500000, samples=143497728, dma_tc=140134, dma_ht=140135, err=0, ovr=0, adc_ovr=0
[0010] ADC1_CH5: raw=1079, avg=1125, rate=500000, samples=144771584, dma_tc=141378, dma_ht=141379, err=0, ovr=0, adc_ovr=0
```
- **Tasa sostenida:** 500.000 muestras por segundo (500 kSPS).
- **Muestras totales acumuladas:** > 144,7 millones sin errores (`err=0`, `ovr=0`, `adc_ovr=0`).
- **Estado final:** Completamente operativo y listo para la re-auditoría.
