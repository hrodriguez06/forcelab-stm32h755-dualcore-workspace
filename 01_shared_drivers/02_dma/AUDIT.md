# Auditoría de `02_dma`: fases F0-F6

| | |
| :--- | :--- |
| **Fecha** | 2026-09-24 |
| **Rama / commit auditado** | `driver/dma-tiers-and-gpio-plan` @ `40105eb` |
| **Base del plan** | `a7f700a` ([IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md)) |
| **Veredicto** | 1.ª auditoría (`40105eb`): no se aprueba el merge, hay que corregir X1-X5. **Reauditoría (`b224b24`): X1-X5 cerrados; falta Y1 (§6).** |

La auditoría se hizo sobre el código y sobre la placa, no sobre el informe de ejecución.
Todo lo que figura como "verificado" aquí se comprobó directamente.

---

## 1. Verificado y conforme

| Punto | Evidencia |
| :--- | :--- |
| C2: la API es inmutable | `git diff a7f700a..HEAD -- dma_bm.h` no borra ninguna línea: todo son adiciones. |
| A12: targets CMake | Existe `stm32h7_driver_dma_bm`. El alias `stm32h7_driver_dma` no tiene fuentes y enlaza `_dma_bm`. `_dma_ll` y `_dma_hal` enlazan `_dma_bm`. El paraguas no cambia. |
| Matriz de builds (C9) | Se relanzó `tools/build_all.ps1 -Fresh`: las 8 configuraciones pasan con `-Werror`. |
| A1: flags antes de EN | `DMA_BM_Start()` y `DMA_BM_StartEx()` limpian `DMA_BM_FLAG_ALL` antes de EN. |
| A2: re-armado | `DMA_BM_SetTransfer()` existe, exige `EN=0` y valida. |
| A3: TRBUFF automático | Se activa por request ID con la condición de revisión de la HAL, en BM (`dma_bm.c:445-454`) y en LL (`dma_ll.c:202-207`). |
| A4 (parcial), A5, A6, A8, A10 | Validaciones presentes en `DMA_BM_InitStreamEx`. La tabla 115 **falta** (X3). |
| A7: `fail_if_busy` | Presente en BM y LL. T2 #16 lo ejercita con el stream en `EN=1`. |
| C6: validar antes de escribir | `InitStreamEx` y `DMA_LL_Init` no tocan ningún registro del stream hasta pasar todas las validaciones. T2 comprueba que `CR == 0` tras cada rechazo. |
| T12: regresión | **Comprobado en vivo** con `tools/query_adc.py` contra la placa: unos 503 kSPS de media (entre 487 y 511 por muestra), `OVR` = 0 sobre 126 M de muestras y el heartbeat del M4 avanzando. |

---

## 2. Hallazgos que bloquean el merge

| ID | Sev. | Dónde | Hallazgo | Corrección esperada |
| :--- | :---: | :--- | :--- | :--- |
| **X1** | Alta | `dma_hal.c:266-290`; `dma_ll.c:553-554`; `dma_bm.c:322,328,335,534,540,796`; `dma_ll.c:320,326,396-397` | **Unidades de NDTR equivocadas.** NDTR cuenta en unidades de PSIZE (RM0399 §16.3.12: *"The data width of the number of data items to transfer … is equal to the width of the peripheral bus"*). `DMA_HAL_MemCopy` divide los bytes entre el tamaño de memoria. Con `PeriphDataAlignment = WORD` y `MemDataAlignment = BYTE`, transfiere 4 veces los bytes pedidos y **se sale del buffer `dst`**. En `DMA_LL_MemCopy`, el rango de clean/invalidate se calcula con MSIZE: si PSIZE > MSIZE deja líneas sin invalidar en el CM7. Las comprobaciones de alcance de BM y LL usan `length × mem_elem` y se quedan cortas cuando PSIZE > MSIZE. Ninguna prueba lo detecta porque todas usan tamaños iguales. | Bytes en memoria = NDT × PSIZE. En HAL, `items = bytes / psize_bytes` y exigir `bytes % psize_bytes == 0` y `bytes % msize_bytes == 0`. Añadir un caso M2M con WORD → BYTE en T1 (LL) y en T9 (HAL). |
| **X2** | Media | `dma_hal.c:281-290` | `DMA_HAL_MemCopy` no limita `items` a 65 535. `HAL_DMA_Start` solo lo comprueba con `USE_FULL_ASSERT`, así que la transferencia se trunca y la función devuelve `HAL_OK`. Es el defecto A4 otra vez, ahora en el tier HAL. | Devolver `HAL_ERROR` si `items == 0` o `items > 65535`. Añadir el caso a T2 o T9. |
| **X3** | Media | `dma_bm.c:174-345`, `dma_ll.c:15-158`, `dma_bm.h:249` | **La tabla 115 no está implementada** (A4): si PSIZE < MSIZE, NDT tiene que ser múltiplo de MSIZE/PSIZE. Además, el comentario de `DMA_BM_CheckFifoBurst` la cita como "Table 115", pero esa función implementa la tabla de umbral de FIFO frente a burst. | Rechazar en `InitStreamEx`, `SetTransfer`, `DMA_LL_Start` y `DMA_LL_StartDoubleBuffer`. Corregir el comentario. Añadir el caso a T2. |
| **X4** | Media | CM7 `dma_selftest.c` T10 (líneas 270-280); `dma_hal.c:89-117`, `:201` | **T10 no puede fallar en lo que dice comprobar.** `DMA_HAL_Init` ya llama a `DMA_HAL_CheckErrata`, que pone el bit 20 si falta, y el test la vuelve a llamar. Así no se demuestra que `HAL_DMA_Init` aplicara TRBUFF, que era el gate de F3. Además, `CheckErrata` escribe `CR` sin comprobar `EN=0`, a diferencia de `UART_HAL_CheckDmaErrata`, que devuelve `HAL_BUSY`. Y `DMA_HAL_Init` ignora su resultado. | T10 lee `CR` justo después de `HAL_DMA_Init()` y antes de cualquier `CheckErrata`. `CheckErrata` devuelve `HAL_BUSY` si `EN=1`. `DMA_HAL_Init` propaga el error. |
| **X5** | Media | `00_docs_stm32h7/docs/00_core_and_system/errata_summary.md` | **La errata está mal documentada.** (a) *"REV_ID >= 0x2000 (revisions V and Y)"*: la revisión Y es `0x1003`, por debajo del umbral. (b) La 2.3.12 aparece como un bloqueo del stream, cuando ES0445 describe corrupción de datos de entrada del HASH. (c) *"DMA1 and DMA2 must not service HASH_IN concurrently"* no es la restricción real. ES0445 dice que, mientras el HASH transfiere por DMA1, no puede haber ningún canal activo en DMA2, o al revés; además hay que poner el bit 20. | Reescribir según el texto de ES0445 §2.3.12 y §2.20.3 (líneas 889-902 y 2069-2078 del `.txt`). |

---

## 3. Hallazgos que no bloquean

| ID | Sev. | Dónde | Hallazgo |
| :--- | :---: | :--- | :--- |
| X6 | Baja | `dma_bm.c:94`, `dma_hal.c:58,77`, `dma_bm.h:233` | `GetIRQn` usa `(IRQn_Type)(-1)` para indicar un stream inválido, pero ese valor es `SysTick_IRQn`. Un llamador externo que lo pase a `NVIC_SetPriority` reprograma el SysTick sin aviso. Los llamadores internos sí comprueban `< 0`. Conviene exportar un centinela con nombre y documentar la trampa. |
| X7 | Baja | `dma_bm.c:304-319,525-532`; `dma_ll.c:309-317,389-394` | Las comprobaciones del límite de 1 KB nunca pueden fallar: la dirección ya está alineada a beats × tamaño, que divide 1024. T2 no tiene ese caso. El burst del lado periférico se valida con la tabla del lado memoria (`DMA_CheckFifoParam` solo cubre MBURST), lo que es más estricto que RM0399 y puede rechazar configuraciones legales. |
| X8 | Baja | `dma_ll.c:240`, `:63-69` | `DMA_LL_Deinit` ignora que `Stop` falle, escribe registros aunque `EN` siga a 1 y devuelve `true`. El chequeo de PFCTRL con circular nunca se ejecuta, porque `mode` no puede valer las dos cosas a la vez. |
| X9 | Doc | `CMakeLists.txt:104,110`; `dma_bm.h:24` | A11 no está cerrado del todo: siguen los comentarios *"needed by 04..09"*, *"08_eth depends on 02_dma"* y *"cache safety"*. |
| X10 | Pruebas | CM4 `dma_selftest.c` T1; `dma_mem_bm.h:81-84` | T1 solo prueba word con FIFO lleno e INCR4. El plan pedía 8, 16 y 32 bits × 4 umbrales × INCR4/8/16. La backup SRAM (`0x3880_0000`) entró en la lista de permitidos del BDMA sin verificarla: RM0399 cap. 17 solo dice *"D3 domain SRAM and AHB/APB peripherals"*. Tampoco hay prueba que la cubra. |
| X11 | Baja | `dma_bm.c:770-799` | `SetIdleMemory` comprueba el alcance con el NDTR restante del buffer activo, no con la longitud total. Además, cambiar CT entre la lectura y la escritura es una carrera inherente: RM0399 recomienda escribir justo después de TCIF. Hay que documentarlo en el README. |

---

## 4. Afirmaciones del informe de ejecución que no se sostienen

| Afirmación | Realidad medida |
| :--- | :--- |
| *"Crecimiento de únicamente +104 bytes en flash (.text)"* | Se comparó el `.map` del CM4 release de `prj_04` con una build de `a7f700a`. El `.text` enlazado de `dma_bm.c.obj` pasa de **652 a 1 528 B (+876 B)**, dentro del presupuesto de 1 KB. La imagen CM4 completa pasa de 5 144 a 7 832 B (+2 688 B), sobre todo por el trabajo de `07_adc` de otra sesión (`adc_bm.c.obj`: de 1 236 a 2 732 B). |
| *"T12 … probado en la placa"* | `docs/dma_selftest_capture.txt` no incluye ninguna evidencia de T12. Se cubrió con la consulta en vivo de la §1. |
| *"Latencia de latch en el bus AHB para DMA_SxCR_EN"* | No está respaldado. En un Cortex-M, una lectura al mismo periférico no adelanta a la escritura anterior. El bucle acotado de `DMA_BM_StartEx` es inofensivo, pero no debe citarse como hallazgo de hardware. |
| *"T10 … TRBUFF verified"* | Es circular: ver X4. |

---

## 5. Condiciones para la reauditoría

1. X1-X5 corregidos, cada uno en su commit y con el porqué en el cuerpo.
2. Nuevos casos de prueba: M2M con PSIZE ≠ MSIZE en LL y HAL (X1); `items > 65535` en HAL (X2); tabla 115 en T2 (X3); T10 leyendo `CR` antes de cualquier `CheckErrata` (X4).
3. Nueva captura en `prj_04/docs/dma_selftest_capture.txt` que incluya una línea de T12 (kSPS, `err`, `ovr`, `adc_ovr`).
4. Tamaños reales en el mensaje de commit, obtenidos de `build_all.ps1` o del `.map`, no estimados.

---

## 6. Reauditoría (2026-09-24, commit `b224b24`)

**Veredicto: X1 a X5 están cerrados en el código. Antes del merge falta Y1**, que es corregir una prueba y volver a ejecutarla. No hace falta tocar ningún driver.

### 6.1 Verificado

| ID | Resultado | Evidencia |
| :--- | :--- | :--- |
| X1 | Cerrado | `DMA_HAL_MemCopy` usa `items = bytes / psize_bytes` y exige divisibilidad por PSIZE y MSIZE. `DMA_LL_MemCopy` calcula el rango de caché con PSIZE. El alcance usa `NDT × PSIZE` en `InitStreamEx`, `SetTransfer`, `SetIdleMemory`, `DMA_LL_Start` y `DMA_LL_StartDoubleBuffer`. El canario del CM4 (T1, WORD → BYTE, LL) sí discrimina. |
| X2 | Cerrado | `items == 0` o `items > 65535` devuelve `HAL_ERROR` antes de cualquier mantenimiento de caché o arranque. La prueba (65 536 items) solo puede fallar por esa comprobación. |
| X3 | Cerrado | Tabla 115 en `InitStreamEx`, `SetTransfer`, `DMA_LL_Start` y `DMA_LL_StartDoubleBuffer`. En HAL queda cubierta por `bytes % msize_bytes`. Comentario de `dma_bm.h:249` corregido. Casos T2 #17 y LL7. |
| X4 | Cerrado | T10 llama a `HAL_DMA_Init()` directamente y lee `CR` antes de cualquier `CheckErrata`, así que ahora sí puede fallar. `CheckErrata` devuelve `HAL_BUSY` con `EN=1`, e `Init` propaga el error. |
| X5 | Cerrado | El umbral de revisión es correcto (Y = `0x1003` queda por debajo). La 2.3.12 aparece como corrupción de datos, con la restricción real y la alternativa por IT o polling. |
| C2 | Conforme | Entre `40105eb` y `b224b24`, el único cambio en `dma_bm.h` es el comentario Doxygen. |
| Builds | Conforme | `build_all.ps1 -Clean`: 8/8 en verde con `-Werror`. El preset `selftest` de `prj_04` compila y enlaza los tests de CM4 y CM7. |
| Tamaño | Conforme, sin margen | `.text` **enlazado** de `dma_bm.c.obj` en el CM4 release: 652 → **1 584 B (+932 B)**, dentro del límite de 1 KB por muy poco. Los 2 576 B del informe son el `.text` del objeto antes del `--gc-sections`, no lo que se enlaza. |
| T12 | Conforme (en vivo) | `query_adc.py --rate 2`: 1 526 272 muestras en 3 s (unos 509 kSPS); `err`, `ovr` y `adc_ovr` a 0 sobre unos 499 M de muestras; `m4_hb` avanzando. |

### 6.2 Pendiente antes del merge

| ID | Sev. | Dónde | Hallazgo | Corrección |
| :--- | :---: | :--- | :--- | :--- |
| **Y1** | Media | CM7 `dma_selftest.c`, T9 caso 1b | **El canario del CM7 no puede detectar el desbordamiento.** `s_rx_buf` está en `.axi_ram`, que es write-back cacheable (MPU R4, `mpu_config.c:112-116`). La CPU escribe el canario en `s_rx_buf[16]` y esa línea queda sucia en la D-Cache. `PrepareRx` y `CompleteRx` solo cubren los primeros 64 B, así que la lectura final del canario sale de la caché: pasaría aunque el DMA hubiera escrito encima. Por eso no hay evidencia en hardware de que X1 esté corregido en la HAL, aunque la revisión del código dice que sí. | Tras escribir el canario, `SCB_CleanInvalidateDCache_by_Addr` sobre el buffer entero (256 B) + `__DMB()`, e invalidar la línea del canario antes de leerla. Alternativa: poner el buffer del caso 1b en `.dma_buf`, que no es cacheable. Volver a ejecutar T9 y registrar la captura. |

### 6.3 No bloqueante

| ID | Sev. | Hallazgo |
| :--- | :---: | :--- |
| Y2 | Doc | `errata_summary.md` tiene ahora **dos** secciones 2.3.12: la original en §3 y otra añadida en §4. Hay que dejar una sola. El umbral `REV_ID ≥ 0x2000` para HASH es criterio de la HAL de ST, no de ES0445; conviene decirlo así. |
| Y3 | Pruebas | El caso X2 (65 536 B sobre un buffer de 256 B) es destructivo si la corrección se pierde en una regresión: la prueba corrompería memoria antes de fallar. Mejor usar un `dst` falso fuera de rango que se rechace, o comprobar `items` con una función pura. |
| Y4 | Pruebas | La parte de X1 que afecta al rango de caché en `DMA_LL_MemCopy` solo se ejercita en el CM4, que no tiene caché. No hay prueba de LL en el CM7. |
| Y5 | Pruebas | El camino `HAL_BUSY` de `DMA_HAL_CheckErrata` no tiene prueba. Por otro lado, T10 ya no pasa por `DMA_HAL_Init`, así que la propagación del error de `CheckErrata` desde `Init` tampoco se ejercita. |
| X6-X11 | — | Siguen abiertos tal como se describen en la §3. Nada de lo que cambió los resuelve. |
