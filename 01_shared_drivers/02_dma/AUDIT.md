# Auditoría de `02_dma`: fases F0-F6

| | |
| :--- | :--- |
| **Fecha** | 2026-09-24 |
| **Rama / commit auditado** | `driver/dma-tiers-and-gpio-plan` @ `40105eb` |
| **Base del plan** | `a7f700a` ([IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md)) |
| **Veredicto** | 1.ª auditoría (`40105eb`): no se aprueba el merge, hay que corregir X1-X5. Reauditoría (`b224b24`): X1-X5 cerrados; falta Y1 (§6). **Cierre (`f3ab055`): Y1 cerrado; se aprueba el merge (§7), confirmado en hardware por el auditor (§8).** Iteración 2 (`c11e30c`): no se aprueba el merge; falta Z1 (§9). **Cierre iteración 2 (`6d13177`): Z1 cerrado; se aprueba el merge (§10).** |

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

---

## 7. Cierre (2026-09-25, commit `f3ab055`)

**Veredicto: se aprueba el merge de `driver/dma-tiers-and-gpio-plan`.** No queda ningún hallazgo bloqueante. Hacer el merge a `main` le corresponde a quien sea dueño de la rama.

| ID | Resultado | Evidencia |
| :--- | :--- | :--- |
| Y1 | Cerrado | `26864ae`: después de escribir el canario se llama a `DMA_MEM_BM_PrepareRx(s_rx_buf, 256)`, que hace clean + invalidate y así lleva el canario a la RAM. Después del DMA, `DMA_MEM_BM_CompleteRx(s_rx_buf, 256)` fuerza que la CPU relea desde la RAM. El buffer está alineado a 32 y su tamaño es múltiplo de 32, así que ninguna llamada devuelve `false` sin avisar. La prueba ahora discrimina: con el defecto X1, el DMA habría escrito 256 B y pisado `s_rx_buf[16]`. |
| Y2 | Cerrado | `d51e02c`: `errata_summary.md` tiene una sola sección 2.3.12 (línea 173). |
| Build | Conforme | El preset `selftest` de `prj_04` compila sin warnings. La ELF de CM7 (00:57:48) se compiló después de editar la prueba (00:57:08) y antes del commit (01:01:37). |

**Límite de la evidencia en hardware.** En `dma_selftest_capture.txt`, lo que cambia respecto a la captura anterior es la fecha, el título y un contador de red. T9 solo imprime `PASS`, así que el log no permite distinguir una ejecución posterior a Y1 de una anterior. Se acepta porque la corrección se verifica por revisión y el orden de las marcas de tiempo es coherente. A partir de ahora, conviene que el selftest imprima el hash de commit de la imagen (por ejemplo, `git describe` inyectado por CMake) para que cada captura sea trazable.

**Pendientes no bloqueantes** (para una siguiente iteración de `02_dma`):

- Y3: la prueba de X2 es destructiva si hay una regresión.
- Y4: no hay prueba de LL con caché en el CM7.
- Y5: no hay prueba de `CheckErrata` → `HAL_BUSY`, ni de la propagación desde `DMA_HAL_Init`.
- X6: `GetIRQn` devuelve `-1`, que es el valor de `SysTick_IRQn`.
- X7: la comprobación del límite de 1 KB es código que nunca se ejecuta, y el burst periférico se valida con la tabla del lado memoria.
- X8: `DMA_LL_Deinit` no propaga el fallo de `Stop`.
- X9: los comentarios de `CMakeLists.txt:104,110` y `dma_bm.h:24` siguen desactualizados.
- X10: la cobertura de T1 y la inclusión de la backup SRAM en el BDMA siguen sin verificar.
- X11: `SetIdleMemory` comprueba el alcance con el NDTR restante.
- Tamaño: `dma_bm` usa +932 B de los 1 024 del presupuesto del CM4. Cualquier ampliación futura del tier BM tiene que venir acompañada de una medición del `.map`.

---

## 8. Verificación independiente en hardware (2026-09-25)

El auditor compiló, flasheó y capturó todo esto por su cuenta, sin usar ningún artefacto del implementador.

**Imagen.** Commit `f3ab055`, con build limpia del preset `selftest` (se borró `build/selftest` antes de compilar). Tamaños: CM7 101 424 B, CM4 16 704 B. Sin warnings con `-Werror`. Se flasheó con `tools/flash.py --preset selftest --core both`, con verificación de lectura. Captura con `tools/monitor.py --timestamps` por COM4. Placa: NUCLEO-H755ZI-Q, `REV_ID=0x2003`, con el puente PD5-PD6 puesto.

```text
[   7.107] [m7_dma_test] T8_DCACHE_COHERENCY: PASS
[   7.110] [m7_dma_test] T9_HAL_MEMCOPY_IT: PASS
[   7.115] [m7_dma_test] T10_USART3_DMA: PASS (TRBUFF verified)
[   7.118] [m7_dma_test] T10_USART3_DMA: PASS
[   7.121] [m7_dma_test] T11_BDMA_SRAM4: PASS
[   7.124] [m7_dma_test] OVERALL: PASS (T8-T11)
[  13.027] [m4_dma_test] T1_M2M=PASS T2_REJ=PASS T3_UART=PASS
[  15.000] [m4_dma_test] T4_TIM7=PASS (tc=10,ht=10) T5_DBM=PASS
[  16.971] [m4_dma_test] T6_REARM=PASS T7_ITCM=PASS (te=1)
[  18.943] [m4_dma_test] OVERALL=PASS (mask=0xFE/0xFE)
```

**Control negativo de X1 y Y1.** En `dma_hal.c:303` se volvió a poner el cálculo defectuoso (`items = bytes / msize_bytes`), se recompiló y se flasheó solo el CM7. Resultado: T9 falla y T8, T10 y T11 siguen pasando. Esto demuestra que T9 detecta el defecto X1, y por tanto que la corrección de Y1 hace que el canario funcione. Después, el archivo se restauró con `git checkout` y se borró `build/selftest` para que no quedara ninguna imagen con el defecto.

```text
[  72.635] [m7_dma_test] T8_DCACHE_COHERENCY: PASS
[  72.639] [m7_dma_test] T9_HAL_MEMCOPY_IT: FAIL
[  72.643] [m7_dma_test] T10_USART3_DMA: PASS (TRBUFF verified)
[  72.649] [m7_dma_test] T11_BDMA_SRAM4: PASS
[  72.653] [m7_dma_test] OVERALL: FAIL (T8-T11)
```

**Restauración.** Build limpia del release a partir de `f3ab055` (CM7 84 060 B, CM4 7 912 B), flasheada en los dos núcleos con verificación. T12 medido en vivo con `query_adc.py --rate 2`: 1 271 808 muestras en 2,5 s (unos 509 kSPS); `err`, `ovr` y `adc_ovr` a 0; `m4_hb` avanzando. La placa queda con el firmware de producción.

**Veredicto final:** se confirma la aprobación de la §7. La evidencia del implementador (§6 y §7) coincide con lo observado por el auditor.

---

## 9. Auditoría de la iteración 2 (2026-09-25, rama `driver/dma-iteration-2` @ `c11e30c`)

**Veredicto: no se aprueba el merge todavía.** El código de los drivers es correcto y ninguna firma pública cambió. Lo que bloquea es una prueba de X10a que no verifica lo que dice verificar, y la regla R1 (control negativo) queda sin cumplir para ese punto. Base: [IMPLEMENTATION_PLAN_2.md](IMPLEMENTATION_PLAN_2.md). Informe del implementador: [ITERATION_2.md](ITERATION_2.md).

### 9.1 Verificado por el auditor

| Punto | Resultado | Evidencia |
| :--- | :--- | :--- |
| C2 | Conforme | `git diff main..HEAD -- '02_dma/*.h'` solo borra tres líneas, y las tres son comentarios. |
| Builds | Conforme | `build_all.ps1 -Clean` pasa las 8 con `-Werror`. Build limpia del preset `selftest` sin warnings: CM7 104 164 B, CM4 17 996 B. |
| Captura | Conforme | Entre `4418037` (el `rev=` de la captura) y `HEAD` solo cambian documentos. |
| X6 | Cerrado | `DMA_BM_IRQN_INVALID`, `DMA_BM_GetIRQnEx` y `DMA_HAL_GetIRQnEx`. Los usos internos ya pasan por `…Ex`. |
| X7a | Cerrado | Los seis chequeos inalcanzables se borraron. El `_Static_assert((1024U % (16U * 4U)) == 0U)` es correcto. |
| X7b | Cerrado | `DMA_BM_CheckPeriphBurst` se usa en el lado periférico de BM y LL. Con `PINC=0` solo se exige alinear a PSIZE. La regla `bytes > 16` es **literal** en RM0399: *"the burst size multiplied by the data size must not exceed the FIFO size"* (`16_…dma.txt:743`). |
| X8 | Cerrado (por revisión) | `DMA_LL_Deinit` devuelve `false` sin tocar nada si `Stop` falla. El chequeo muerto de PFCTRL + circular se borró. No tiene prueba en HW, como preveía el plan. |
| X9 | Cerrado | Ver `CMakeLists.txt:104,110` y `dma_bm.h:24-26`. |
| X11 | Cerrado | `SetIdleMemoryEx` recibe la longitud explícita y comprueba TE después de escribir. `DMA_LL_SetIdleMemory` usa `dbm_length`, que se añadió al final del handle. |
| X10b | Cerrado: la backup SRAM se queda | RM0399 §2, literal: *"It can access the internal SRAM4, backup RAM, and AHB4 and APB4 peripherals"* (`rm0399_system/02_memory-and-bus-architecture.txt:328-329`). T11b discrimina: en 3a el patrón solo puede haber llegado a la backup SRAM por el BDMA, porque la CPU solo escribió ceros; en 3b, lo mismo en sentido contrario. La coherencia de caché la resuelven los helpers de `MemCopy`. |
| Y3, Y4, Y5 | Cerrados | Cada uno tiene su control negativo con FAIL en la línea correcta. En Y3, el truncado a 32 B no puede desbordar (R4). |
| Tamaño | Conforme | `dma_bm.c.obj` enlazado en el CM4 release: **1 596 B** de `.text + .rodata` (1 588 B solo de `.text`), dentro del límite de 1 676 B. El plan esperaba que bajara; en `.text` queda prácticamente igual que antes (1 584 B). |
| T12 | Conforme (en vivo) | `query_adc.py --rate 2`: 1 271 808 muestras en 2,5 s (unos 509 kSPS); `err`, `ovr` y `adc_ovr` a 0. |

### 9.2 Bloqueante

| ID | Sev. | Dónde | Hallazgo | Corrección |
| :--- | :---: | :--- | :--- | :--- |
| **Z1** | Media | CM4 `dma_selftest.c:381-431`, `:1424-1428`; `dma_negative_controls.txt` §6 | **La matriz de X10a es tautológica.** `expect_valid` sale de `DMA_BM_CheckFifoBurst` y `DMA_BM_CheckPeriphBurst`, que son justo lo que se quiere probar. La rama de rechazo solo comprueba que `InitStreamEx`, que llama a esas mismas funciones, también rechaza. Una celda equivocada en la tabla nunca produciría un FAIL. **El control negativo registrado no demuestra nada:** la mutación de Word, umbral lleno e INCR4 rompe la primera parte de T1 (streams 1-7, `dma_selftest.c` al principio de `Test_T1`). T1 aborta antes de la matriz, y por eso los contadores salen `0/0` en lugar de `25/26`. Además, los denominadores `/26` y `/25` están escritos a mano. Las 26 combinaciones válidas **sí** se ejecutan en HW (con `memcmp` y canario), y esa parte de la cobertura es real. | Poner un oráculo independiente en la prueba: una tabla fija de 48 entradas transcrita de RM0399 §16.3.14 (tabla de umbral de FIFO) y de la nota del lado periférico, **no** derivada del driver. Contar las discrepancias con el driver e imprimir `mismatch=N` junto a totales calculados. Repetir el control negativo mutando una celda que no use el resto de T1 (por ejemplo, Byte, 1/2, INCR8) y registrar `mismatch=1` con su hash. |

### 9.3 No bloqueante (corregir en el informe o en el proceso)

| ID | Hallazgo |
| :--- | :--- |
| Z2 | Hay tres descripciones de `ITERATION_2.md` que no coinciden con el código. X8 habla de una "rama `cfg->periph_flow_ctrl` en P2M", cuando lo que se borró fue PFCTRL + circular. X7a cita `DMA_BM_MAX_BURST_BYTES <= 1024U`, pero el código dice `1024 % 64 == 0`. En el control de Y3 pone "NDTR truncado a 16", pero la prueba usa 65 568 B, que se trunca a 32. |
| Z3 | El informe no incluye la sección de "lo que no se pudo ejercitar", que el encargo pedía explícitamente. Ahí van X8 y la propagación de `CheckErrata` desde `DMA_HAL_Init` (Y5), que solo se cubren por revisión. |
| Z4 | R2 queda cumplida solo en parte. Los controles de Y4, Y5, X10a y X10b llevan `rev=53bd261-dirty`, pero sus tests se commitearon más tarde (`0ca9844`, `64c4736`, `9018c76` y `4418037`). Ese hash no identifica el código que corrió. Cada control tiene que ejecutarse sobre el commit que añade su test, más la mutación. |
| Z5 | El informe dice "exactamente 13 commits", pero la rama tiene 14. Además, "100 % verificado en hardware" no se sostiene mientras Z1 siga abierto. |

---

## 10. Cierre de la iteración 2 (2026-09-25, `driver/dma-iteration-2` @ `6d13177`)

**Veredicto: Z1 está cerrado y se aprueba el merge de `driver/dma-iteration-2`.** Lo que queda abierto son erratas en la documentación y ninguna bloquea. El merge a `main` lo decide quien sea dueño de la rama.

### 10.1 Verificado por el auditor

| Punto | Resultado | Evidencia |
| :--- | :--- | :--- |
| Z1: oráculo | Cerrado | `df46b94`: `s_rm0399_burst_valid[3][4][4]` es una tabla fija de 48 entradas. Comparada celda a celda con RM0399 Tabla 116 (`16_…dma.txt:722-740`) y con la regla del lado periférico (líneas 743 y 754-758), coincide en todas. No llama al driver. Da 23 válidas y 25 inválidas, igual que el recuento independiente del auditor. |
| Z1: bucle y salida | Cerrado | Ya no aborta a mitad: las discrepancias se cuentan en `s_t1_combos_mismatch`. `valid`, `ok`, `invalid`, `rej` y `mismatch` se calculan; ya no hay denominadores escritos a mano. PASS solo si `mismatch == 0` y todos los recuentos cuadran. |
| Z1: control negativo (R1 y R2) | Cerrado | Mutación: Byte + 1/2 + INCR8 rechazado. Resultado: `valid=23 ok=22 invalid=25 rej=25 mismatch=1` sobre `rev=df46b94-dirty`, que es el commit que añade la prueba más la mutación. La celda mutada no la usa el resto de T1, así que el FAIL viene de la matriz. |
| Captura limpia | Conforme | `rev=df46b94` con `mismatch=0`. Entre `1bfc763` y `6d13177` no cambia ninguna línea de los drivers (`01_shared_drivers/02_dma/*.c` y `*.h`); solo cambian el test y los documentos. |
| Build | Conforme | Build limpia del preset `selftest` de `prj_04` sin warnings (CM7 104 164 B, CM4 18 332 B). La matriz de 8 builds verificada en la §9 sigue valiendo, porque el código de los drivers no ha cambiado. |
| Z3 y Z4 | Cerrados | `ITERATION_2.md` §7.4 lista lo no ejercitable en HW (X8 y la propagación de Y5). En la línea 86 se aclara que los controles antiguos se ejecutaron sobre `53bd261-dirty`. |
| Tamaño | Conforme | 1 596 B (`.text + .rodata`) y 1 588 B (`.text`), ahora desglosado en el informe. |
| T12 | Conforme (en vivo) | `query_adc.py --rate 2`: 1 276 928 muestras en 2,5 s (unos 511 kSPS); `err`, `ovr` y `adc_ovr` a 0. |

### 10.2 Erratas que quedan (no bloquean)

| ID | Hallazgo |
| :--- | :--- |
| Z2′ | X7a sigue descrito como `_Static_assert(DMA_BM_MAX_BURST_BYTES <= 1024U)`, en `ITERATION_2.md:29` y en `RESUMEN_Z1_ITERACION_2.md:19`. El código real es `(1024U % (16U * 4U)) == 0U`. |
| Z6 | `ITERATION_2.md:146` dice que `HAL_DMA_Init` activa TRBUFF "para periféricos UART/HASH". La HAL de ST solo lo hace para requests UART (`IS_DMA_UART_USART_REQUEST` excluye la 78). En HASH lo pone `DMA_HAL_CheckErrata`. Además, T10 solo ejercita USART3. |
| Z7 | `RESUMEN_Z1_ITERACION_2.md:7` se declara "COMPLETADO Y CERRADO — 100 %". El cierre lo decide esta auditoría, y el propio informe reconoce dos caminos no ejercitables en HW. |
| Z8 | Límite de cobertura, no un defecto. Con tamaños iguales en los dos lados, la tabla del lado memoria ya rechaza todo lo que prohíbe la regla del periférico, así que la matriz no discrimina mutaciones de `DMA_BM_CheckPeriphBurst`. Esa función queda cubierta por los casos de T2 de X7b, que tienen su control negativo en `52a73e0-dirty`. |

Z2′, Z6 y Z7 se pueden corregir en cualquier commit de documentación posterior. Z8 no requiere acción.
