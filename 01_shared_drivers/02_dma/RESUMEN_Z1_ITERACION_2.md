# Resumen Ejecutivo: Corrección de Z1 e Iteración 2 del Driver DMA (`02_dma`)

- **Repositorio:** `stm32h755`
- **Rama:** `driver/dma-iteration-2` (base `b067fec`)
- **Microcontrolador:** STM32H755ZI (Nucleo-H755ZI-Q, Device ID 0x450, Rev V, Rev ID 0x2003)
- **Fecha:** 25 de septiembre de 2026
- **Estado:** implementación completa y aprobada por la auditoría (`AUDIT.md` §10). Verificado en silicio salvo dos caminos que no se pueden inducir en HW y se cubren por revisión: el fallo de `Stop` en `DMA_LL_Deinit` (X8) y la propagación de `CheckErrata` desde `DMA_HAL_Init` (Y5). Ver `ITERATION_2.md` §7.4.

---

## 1. Alcance de la Iteración 2 y Corrección de Z1

La Iteración 2 consolidó las correcciones de auditoría (**X6–X11**, **Y3–Y5**) y subsanó el hallazgo bloqueante **Z1** (oráculo tautológico en la matriz de ráfagas FIFO):

| Punto | Tipo | Resumen del Cambio | Estado | Commit |
| :--- | :--- | :--- | :---: | :--- |
| **Z1** | Bloqueante | Oráculo independiente RM0399 Tabla 116 / §16.3.14 (48 combinaciones, 23 válidas, 25 inválidas). Bucle sin corte temprano, reporte exhaustivo `valid=23 ok=23 invalid=25 rej=25 mismatch=0`. | **CERRADO** | `df46b94` |
| **X8** | Higiene | Eliminado check redundante `mode == CIRCULAR` en bloque `PFCTRL` de `dma_ll.c` (modos escalares excluyentes). Propagado fallo de parada en `DMA_LL_Deinit()`. | **CERRADO** | `9dd4baa` |
| **X7a** | Higiene | Reemplazada validación en runtime por `_Static_assert((1024U % (16U * 4U)) == 0U)`: el bloque de burst más grande divide a 1 KB. | **CERRADO** | `4ab5c42` |
| **X6** | Robustez | Creados `DMA_BM_GetIRQnEx` y `DMA_HAL_GetIRQnEx` con centinela `DMA_BM_IRQN_INVALID` (evita colisión con `SysTick_IRQn`). | **CERRADO** | `d45877e` |
| **X11** | Robustez | `DMA_BM_SetIdleMemoryEx` con longitud explícita y `DMA_LL_SetIdleMemory`. Chequeo de TEIF contra colisiones de conmutación CT. | **CERRADO** | `52a73e0` |
| **X7b** | Alineación RM0399 | `DMA_BM_CheckPeriphBurst` según RM0399 §16.3.14. Alineación relajada para `PINC=0` a `PSIZE`. | **CERRADO** | `efce94e` |
| **Y3** | No destructivo | T9 caso 1c en CM7 con `bytes = 65568U`: en regresión copia solo 32 B, dejando el canary intacto (R4). | **CERRADO** | `53bd261` |
| **Y4** | D-Cache | Test T8b en CM7 verificando coherencia de D-Cache en RAM física para `DMA_LL_MemCopy`. | **CERRADO** | `0ca9844` |
| **Y5** | Errata | Test T10b en CM7 verificando que `DMA_HAL_CheckErrata` retorna `HAL_BUSY` si `CR.EN == 1`. | **CERRADO** | `64c4736` |
| **X10b** | Silicio BDMA | Test T11b en CM7 confirmando acceso de BDMA a Backup SRAM (`0x38800000`) en matriz AHB D3. | **CERRADO** | `4418037` |
| **Z2–Z5** | Documentación | Actualización de `ITERATION_2.md`, `dma_negative_controls.txt` y `dma_selftest_capture.txt`. | **CERRADO** | `3e1badd` |

---

## 2. Detalle de la Corrección de Z1

### Defecto Previo
- La prueba X10a utilizaba el propio código bajo prueba (`DMA_BM_CheckFifoBurst` y `DMA_BM_CheckPeriphBurst`) para definir qué combinaciones debían ser válidas, convirtiendo la prueba en una tautología.
- El bucle abortaba al primer fallo, enmascarando el comportamiento del resto de combinaciones.
- Los denominadores en el mensaje de log estaban hardcodeados (`ok=26/26 rej=25/25`).

### Solución Implementada
1. **Tabla de verdad externa (`s_rm0399_burst_valid[3][4][4]`):**
   - Extraída estrictamente de RM0399 Tabla 116 y §16.3.14:
     - `BYTE`: 11 válidas, 5 inválidas.
     - `HALFWORD`: 7 válidas, 9 inválidas.
     - `WORD`: 5 válidas, 11 inválidas.
     - Total: **23 válidas, 25 inválidas (48 homogéneas)**.
2. **Evaluación de discordancias (`mismatch`):**
   - Compara `oracle_valid` contra el veredicto del driver.
   - Si difieren, incrementa `s_t1_combos_mismatch` sin detener el bucle.
3. **Casos heterogéneos independientes:**
   - 3 combinaciones cruzadas (`BYTE->WORD`, `HALFWORD->WORD`, `WORD->BYTE`) evaluadas por separado (`diff_sizes_ok == 3`).
4. **Condición de pase estricta en T1:**
   `mismatch == 0 && valid == 23 && ok == 23 && invalid == 25 && rej == 25 && diff_sizes_ok == 3`.

---

## 3. Evidencia Empírica en Hardware Real

### 3.1 Control Negativo de Z1 (Regla R1)
- **Mutación:** En `DMA_BM_CheckFifoBurst`, se alteró `BYTE + TH_1_2` para aceptar únicamente `INCR4` (rechazando erróneamente `INCR8`).
- **Resultado en UART sobre silicio (`rev=df46b94-dirty`):**
  ```
  [  11.008] [m4_dma_test] rev=df46b94-dirty
  [  12.982] [m4_dma_test] T1_M2M=FAIL T2_REJ=PASS T3_UART=PASS
  [  14.957] [m4_dma_test] T1 combos valid=23 ok=22 invalid=25 rej=25 mismatch=1
  [  16.928] [m4_dma_test] T4_TIM7=PASS (tc=9,ht=10) T5_DBM=PASS
  [  18.901] [m4_dma_test] T6_REARM=PASS T7_ITCM=PASS (te=1)
  [  20.873] [m4_dma_test] OVERALL=FAIL (mask=0xFC/0xFE)
  ```
  El oráculo detectó exactamente 1 discordancia, la prueba falló (`mask=0xFC`), y solo se vio afectada la celda mutada.

### 3.2 Selftest Limpio en Hardware (`rev=df46b94`)
- **Captura UART completa:**
  ```
  === STM32H755ZI dual-core Ethernet / lwIP - CPU1 (Cortex-M7) ===
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

## 4. Presupuesto de Memoria Flash (Regla R3)

Medición en `build/release/cm4_firmware.map`:
- **Código ejecutable (`.text` únicamente):** **1 588 B**
- **Constantes en `.rodata` (`s_burst_beats` y `s_stream_bit_shifts`):** **8 B**
- **Total enlazado `dma_bm.c.obj` (`.text + .rodata`):** **1 596 B**
- **Límite presupuestario (R3):** $\le 1 676\text{ B}$
- **Margen disponible:** **80 B libres**

---

## 5. Verificación de Integridad y Producción

1. **Matriz de compilación CI (`tools/build_all.ps1 -Clean`):**
   - **8/8 configuraciones PASARON sin advertencias con `-Werror`** en debug y release (`prj_01`, `prj_02`, `prj_03`, `prj_04`).
2. **Telemetría en Vivo (`tools/query_adc.py`):**
   - Placa programada con el preset `release`.
   - Adquisición activa a **500 kSPS** sostenidos sin errores (`err=0`, `ovr=0`, `adc_ovr=0`).
