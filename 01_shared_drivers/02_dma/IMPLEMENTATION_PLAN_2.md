# Plan de implementación, iteración 2: pendientes de la auditoría de `02_dma`

| | |
| :--- | :--- |
| **Fecha / base** | 2026-09-25, `main` @ `ea9fecb` |
| **Alcance** | Los pendientes no bloqueantes de [AUDIT.md](AUDIT.md) §7: Y3, Y4, Y5, X6, X7, X8, X9, X10 y X11 |
| **Criterios** | Los mismos C1-C9 de [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) §0, en especial C2 (API inmutable) y C6 (validar antes de escribir) |
| **Rol** | Quien redactó el plan audita cada fase antes del merge |

Son nueve puntos, agrupados en cinco fases que van de menor a mayor riesgo. Ninguno cambia una firma pública. Solo X7 cambia qué configuraciones se aceptan, y lo hace en la dirección permisiva, ajustándose a RM0399.

---

## 0. Lecciones de la iteración 1 que pasan a ser reglas

| Regla | Origen |
| :--- | :--- |
| **R1. Toda prueba nueva lleva su control negativo.** Se reintroduce el defecto, se ejecuta y se registra el FAIL; después se restaura con `git checkout`. Una prueba sin FAIL demostrado no cuenta como evidencia. | Y1: un canario ciego pasaba siempre. El §8 de la auditoría demostró cuánto vale el control negativo. |
| **R2. La captura tiene que identificar la imagen.** El banner del selftest imprime el hash del commit (§4, G4.1). | §7: la captura posterior a Y1 no se podía distinguir de la anterior. |
| **R3. El tamaño se mide sobre el `.map` enlazado,** no sobre el `.text` del objeto. | §6: se reportaron 2 576 B de objeto frente a 1 584 B enlazados. |
| **R4. Nada destructivo si una prueba falla.** Un test que falla no puede corromper memoria ajena. | Y3. |

**Presupuesto de tamaño.** `dma_bm` enlazado en el CM4 release de `prj_04`: base de 652 B, límite de 652 + 1 024 = **1 676 B**, valor actual de **1 584 B**. Las funciones nuevas que `prj_04` no llama no cuentan, porque `--gc-sections` las elimina. X7 quita código de `InitStreamEx`, así que se espera que el tamaño **baje**.

---

## 1. Decisiones por punto

| ID | Problema (resumen) | Decisión | Fase |
| :--- | :--- | :--- | :---: |
| X9 | Comentarios desactualizados: `CMakeLists.txt:104,110` y `dma_bm.h:24` | Reescribirlos | G1 |
| X8 | `DMA_LL_Deinit` ignora que `Stop` falle; el chequeo de PFCTRL en `dma_ll.c:63-69` nunca se ejecuta | Propagar el fallo sin tocar registros y borrar el código muerto | G1 |
| X7a | Chequeo del límite de 1 KB que nunca se ejecuta: `dma_bm.c:315-329,547-554`, `dma_ll.c:320-327,410-412` | Borrarlo y dejar el razonamiento en un `_Static_assert` | G1 |
| X6 | `GetIRQn` devuelve `-1`, que es `SysTick_IRQn` | Mantener el valor (C2), darle nombre y añadir variantes `…Ex` que devuelven `bool` | G2 |
| X11 | `SetIdleMemory` comprueba el alcance con el NDTR restante; la carrera con CT no está documentada | `DMA_BM_SetIdleMemoryEx(s, addr, length)` y `DMA_LL_SetIdleMemory(h, addr)`, más documentación | G2 |
| X7b | El burst periférico se valida con la tabla del lado memoria y exige alinear a beats × tamaño aunque `PINC=0` | Aplicar la regla real de RM0399 §16.3.14 y relajar la alineación con `PINC=0` | G3 |
| Y3 | La prueba de X2 es destructiva si hay una regresión | Rediseñarla para que una regresión se vea como truncado, no como desbordamiento | G4 |
| Y4 | No hay prueba de LL con caché en el CM7 | T8b: `DMA_LL_MemCopy` en DMA2 sobre `.axi_ram` | G4 |
| Y5 | No hay prueba de `CheckErrata` → `HAL_BUSY` | T10b con un stream en `EN=1`; la propagación desde `Init` se declara inalcanzable | G4 |
| X10a | T1 solo prueba word, FIFO lleno e INCR4 | T1 recorre todas las combinaciones válidas | G4 |
| X10b | La backup SRAM está en la lista de permitidos del BDMA sin verificar | Decidirlo con RM0399 Fig. 1 y una prueba T11b en HW | G4 |

---

## 2. G1: comentarios y código muerto (sin cambio funcional)

**X9.** Estas son las líneas nuevas:

```cmake
    ${CMAKE_CURRENT_LIST_DIR}/02_dma      # DMA1/DMA2/BDMA tiers + dma_mem_bm.h (alignment, cache, reachability)
    ${CMAKE_CURRENT_LIST_DIR}/08_eth      # own ETH DMA; descriptors in SRAM3 - does not link 02_dma
```

```c
/* 32-byte alignment. Cache safety also needs the SIZE rounded up to 32:
   use DMA_MEM_BM_ALIGN + DMA_MEM_BM_SIZE() from dma_mem_bm.h. */
#define DMA_BM_ALIGN_32   __attribute__((aligned(32)))
```

**X8.**

- `DMA_LL_Deinit`: si `DMA_LL_Stop()` devuelve `false`, devuelve `false` **sin escribir ningún registro ni tocar el handle**, igual que `DMA_BM_DeinitStream`. Solo cambia el comportamiento en el camino de fallo, que antes mentía.
- Borrar el bloque `if (mode == PFCTRL) { if (mode == CIRCULAR) … }`. PFCTRL junto a M2M ya se rechaza en `dma_ll.c:55`, y PFCTRL junto a circular no se puede pedir, porque `mode` toma un único valor.

**X7a.** Los seis chequeos del límite de 1 KB nunca pueden fallar: la dirección ya está alineada a `beats × tamaño` ∈ {1, …, 64}, que divide a 1 024. Se borran y en su lugar se deja la prueba de ese razonamiento:

```c
/* A burst block never crosses a 1 KB boundary (RM0399 §16.3.13) because every
   burst address is aligned to beats x size, and the largest block divides 1 KB. */
_Static_assert((1024U % (16U * 4U)) == 0U, "largest burst block must divide 1 KB");
```

**Gate G1:** `build_all.ps1 -Clean` 8/8. `git diff` de los `.h`: solo cambian comentarios. `dma_bm` enlazado ≤ 1 584 B, y se reporta la cifra. Selftest T1-T11 PASS, porque ninguna prueba depende del código borrado. X8 no tiene prueba propia: la inyección de un fallo de `Stop` no es reproducible en HW, así que se cubre en la revisión y se dice explícitamente.

---

## 3. G2: API aditiva (X6, X11)

**X6.** El `-1` forma parte del contrato documentado y no se cambia (C2). Además, **ningún valor negativo es seguro** para quien no lo compruebe. CMSIS `NVIC_SetPriority` escribe en `SCB->SHPR[(IRQn & 0xF) - 4]`: `-1` reprograma el SysTick, y un centinela con los 4 bits bajos entre 0 y 3 escribiría fuera del array. La solución es poner nombre al valor y ofrecer una variante que no pueda malinterpretarse.

```c
#define DMA_BM_IRQN_INVALID   ((IRQn_Type)(-1))   /* == SysTick_IRQn: never pass it to NVIC_* */

bool DMA_BM_GetIRQnEx(const DMA_Stream_TypeDef *s, IRQn_Type *out);   /* false: invalid stream, *out untouched */
bool DMA_HAL_GetIRQnEx(const void *instance, IRQn_Type *out);          /* DMA1/DMA2 streams and BDMA channels */
```

- Los internos (`DMA_LL_EnableIrq`/`DisableIrq`, `DMA_HAL_Init`/`DeInit`) pasan a usar `…Ex`.
- El Doxygen de `DMA_BM_GetIRQn` y `DMA_HAL_GetIRQn` avisa del alias con SysTick y remite a `…Ex`.

**X11.** BM no tiene handle y ningún registro guarda la longitud programada: en DBM, NDTR es el restante del buffer activo. La longitud tiene que venir de quien llama.

```c
/* length = items per buffer (NDT units = PSIZE). Also returns false if TEIF is set
   after the write, i.e. CT flipped between the read and the write (RM0399 §16.3.11). */
bool DMA_BM_SetIdleMemoryEx(DMA_Stream_TypeDef *s, uint32_t addr, uint32_t length);

/* LL: uses the length recorded by DMA_LL_StartDoubleBuffer() (new field `dbm_length`). */
bool DMA_LL_SetIdleMemory(DmaLL_Handle_t *h, uint32_t addr);
```

- `DMA_BM_SetIdleMemory` sigue igual, pero su Doxygen explica ahora el límite del NDTR restante y remite a `…Ex`.
- En `DmaLL_Handle_t` se añade `uint32_t dbm_length` **al final**. El handle lo inicializa `DMA_LL_Init`, así que añadirlo no cambia nada para quien ya lo usa.
- `02_dma/README.md`, sección de doble buffer: escribir la memoria inactiva **desde el ISR de TC**, que es lo que recomienda RM0399 §16.3.11. Si TE se activa después de escribir, la escritura llegó tarde y el stream se ha deshabilitado.

**Gate G2:** C2, es decir, el diff de los `.h` solo añade. T5 cambia a `DMA_BM_SetIdleMemoryEx` en BM y a `DMA_LL_SetIdleMemory` en LL, y sigue en PASS. Caso nuevo en T2: `SetIdleMemoryEx` con una `length` que desborda la región alcanzable devuelve `false`. Su control negativo (R1) consiste en volver a usar el NDTR restante y comprobar que el caso pasa en falso.

---

## 4. G3: la regla real del burst periférico (X7b)

La tabla de `DMA_BM_CheckFifoBurst` (y `DMA_CheckFifoParam` de la HAL) es la del **lado memoria**, basada en MSIZE y MBURST. Para el lado periférico, RM0399 §16.3.14 (nota al final de la sección FIFO) da otra regla:

> *If (PBURST × PSIZE) = FIFO_SIZE (4 words), FIFO_Threshold = 3/4 is forbidden.*

Nueva función, que se usa para el lado periférico en `InitStreamEx` y `DMA_LL_Init`:

```c
/* RM0399 §16.3.14: peripheral-side burst vs FIFO (16 B). */
bool DMA_BM_CheckPeriphBurst(uint32_t psize, uint32_t threshold, uint32_t pburst);
/*   bytes = beats(pburst) << psize
     bytes >  16            -> false   (burst larger than the FIFO)
     bytes == 16 && TH 3/4  -> false   (the RM0399 rule)
     otherwise              -> true                                  */
```

- ⚠ Que un burst mayor que la FIFO no está permitido (`bytes > 16`) **es una deducción**: RM0399 no lo dice literalmente. Quien implemente tiene que buscarlo en RM0399 §16.3.14 y §16.3.18 (resumen de configuraciones). Si no aparece, se conserva el rechazo y se documenta como "más estricto que el RM, por seguridad".
- **Alineación con `PINC=0`:** con la dirección periférica fija, todos los beats caen en la misma dirección. Solo hace falta alinear a PSIZE (RM0399 §16.3.13), no a beats × tamaño, y el límite de 1 KB no se puede cruzar. Esto se aplica en `InitStreamEx`, `SetTransfer`, `DMA_LL_Start` y `DMA_LL_StartDoubleBuffer`. Afecta, por ejemplo, a USART TDR en el offset `0x28`, que antes se rechazaba con INCR16 byte.
- Es un cambio **permisivo**: se aceptan configuraciones que antes se rechazaban. Ninguna configuración que hoy se acepte pasa a rechazarse, salvo PSIZE × PBURST = 16 B con umbral 3/4, que RM0399 prohíbe. Esa excepción se escribe en el commit.

**Gate G3:** casos nuevos en T2:

- rechazo de word × INCR4 con umbral 3/4 en el lado periférico;
- aceptación de byte × INCR16 con `PINC=0` sobre `&USART2->TDR`, a nivel de Init, sin arrancar el stream;
- aceptación de half × INCR4 con umbral 1/2 en el lado periférico, que antes se rechazaba y RM0399 permite.

Control negativo (R1): con `CheckFifoBurst` otra vez en el lado periférico, el tercer caso tiene que fallar. Además, en HW (T1, §5), un P2M con PBURST y el umbral elegido completa sin FE ni TE.

---

## 5. G4: pruebas (Y3, Y4, Y5, X10) y trazabilidad

**G4.1 Trazabilidad (R2).** En `prj_04/CMakeLists.txt` se añade:

```cmake
execute_process(COMMAND git rev-parse --short HEAD ...) -> add_compile_definitions(FW_GIT_REV="...")
```

Se añade un sufijo `-dirty` si hay cambios locales. Cada selftest imprime `[m4_dma_test] rev=<hash>` y `[m7_dma_test] rev=<hash>` antes de los resultados.

**Y3.** El caso X2 pasa a `bytes = 65 552` en modo byte (65 536 + 16). Con la corrección, `HAL_ERROR`. Con una regresión, NDTR se trunca a 16, copia 16 B dentro del buffer de 256 B y devuelve `HAL_OK`, así que la prueba **falla sin corromper memoria** (R4). Reproduce además el síntoma original de X2, el truncado silencioso. El control negativo consiste en quitar el chequeo de `items > 65535` y comprobar FAIL con el canario del buffer intacto.

**Y4. T8b** (CM7, LL): `DMA_LL_MemCopy` en `DMA2_Stream3`, de un origen en `.axi_ram` a un destino en `.axi_ram`, con PSIZE word y MSIZE byte y 16 items. Usa el canario coherente de Y1: `PrepareRx` y `CompleteRx` sobre los 256 B. Hay que enlazar `stm32h7_driver_dma_ll` al target CM7 del preset `selftest`. El control negativo es volver a calcular `bytes` con MSIZE en `DMA_LL_MemCopy`: la parte invalidada queda corta y la comparación falla.

**Y5. T10b** (CM7): se configura `DMA2_Stream4` en P2M con request 1 (`dmamux1_req_gen0`, sin generador activo) y se fuerza `EN=1`, igual que en T2 #16 del CM4. `DMA_HAL_CheckErrata` tiene que devolver `HAL_BUSY` y `CR` no puede cambiar. La propagación desde `DMA_HAL_Init` **no se puede probar**: `HAL_DMA_Init` deshabilita el stream antes, así que `CheckErrata` nunca ve `EN=1` en ese camino. Se deja escrito en el README y en el commit. El control negativo es quitar la guarda de `EN` y comprobar FAIL.

**X10a. T1 exhaustivo** (CM4 BM y LL, más CM7 BM en DMA2): se recorren PSIZE = MSIZE ∈ {B, H, W} × umbral ∈ {1/4, 1/2, 3/4, lleno} × MBURST ∈ {single, 4, 8, 16}. Para cada combinación:

- si `DMA_BM_CheckFifoBurst` la acepta, M2M de 64 B con `memcmp` y canario;
- si la rechaza, `InitStreamEx` tiene que devolver `false` y `CR` seguir a 0.

Además, tres casos con PSIZE ≠ MSIZE: B → W, H → W y W → B. El resultado se imprime como máscara, por ejemplo `T1 combos ok=34/34 rej=14/14`. Los números exactos los fija la tabla. El control negativo es alterar una fila de `CheckFifoBurst` y comprobar que el contador cambia.

**X10b. Backup SRAM en el BDMA.** El criterio de decisión se fija antes de probar:

1. Consultar RM0399 §2.1, Figura 1 (matriz de buses): la ruta del BDMA hacia la backup SRAM tiene que aparecer en el PDF completo `docs/00_core_and_system/RM0399_…_full.pdf`. Hay que anotar la página.
2. **T11b** (CM7): `PWR->CR1 |= PWR_CR1_DBP` y `RCC_AHB4ENR.BKPRAMEN` en el banco del CM7, y después M2M por BDMA de SRAM4 a la backup SRAM y vuelta, 32 B. Al terminar se restaura DBP.
3. Si **1 y 2** confirman la ruta, la backup SRAM se queda en la lista de permitidos. Si cualquiera de los dos falla (TE, datos distintos o la ruta no aparece), se **quita** de `dma_mem_bm.h` y se añade un rechazo en T2. No hay término medio.

**Gate G4:** todas las pruebas nuevas PASS y cada control negativo registrado con su FAIL (R1). La captura incluye `rev=<hash>` (R2). La decisión de X10b queda escrita con la página del RM y la salida de T11b.

---

## 6. G5: evidencia y cierre

- Nueva captura `prj_04/docs/dma_selftest_capture.txt` con `rev=` y todas las líneas, y otra aparte con los controles negativos, `dma_negative_controls.txt`, con el hash de cada imagen mutada (`-dirty`).
- T12 en vivo, con la línea completa de `query_adc.py`.
- Tabla de tamaños desde el `.map` (R3): `dma_bm`, `dma_ll` y `dma_hal` enlazados en `prj_04` release, comparados con `ea9fecb`.
- `02_dma/README.md`: matriz de capacidades con la regla del burst periférico, `…Ex` de IRQn, `SetIdleMemoryEx` y el resultado de X10b.
- `AUDIT.md`: el auditor añade la §9 con la auditoría de esta iteración.

---

## 7. Definición de hecho

- [ ] C1-C9 del plan original y R1-R4 de este.
- [ ] El diff de los `.h` existentes solo añade o toca comentarios (C2).
- [ ] 8/8 builds con `-Werror` y el preset `selftest` compila sin warnings.
- [ ] `dma_bm` enlazado ≤ 1 676 B. Se espera que baje respecto a 1 584 B.
- [ ] Cada prueba nueva tiene su control negativo registrado.
- [ ] X10b resuelto sin ambigüedad: se queda con página del RM y T11b, o se va con su rechazo en T2.
- [ ] En cada commit, el porqué va en el cuerpo y los tamaños salen del `.map`.

---

## 8. Riesgos

| Riesgo | Mitigación |
| :--- | :--- |
| Que X7b acepte una configuración de burst periférico que el HW rechace (FE o TE) | Caso de HW en T1 con PBURST y el umbral elegido. Si falla, se vuelve a la regla estricta en ese caso y se documenta. |
| Que T10b deje un stream con `EN=1` que afecte a pruebas posteriores | T10b termina con `DMA_BM_Stop` + `DeinitStream` y comprueba `CR == 0`. |
| Que T11b corrompa la backup SRAM, que se conserva entre resets | Usar solo 32 B, guardarlos y restaurarlos. Hoy ningún proyecto usa la backup SRAM (búsqueda de `BKPSRAM` en `02_firmware_projects`). |
| Que el hash `-dirty` no se genere en builds fuera de git (CI, tarball) | Si no hay git, `FW_GIT_REV="unknown"` y un aviso de CMake, sin fallar la build. |

---

## 9. Orden de commits propuesto

1. `docs(dma): refresh stale 02_dma comments` (X9)
2. `fix(dma): propagate Stop failure from DMA_LL_Deinit and drop dead PFCTRL check` (X8)
3. `refactor(dma): replace unreachable 1 KB burst checks with a static assert` (X7a)
4. `feat(dma): add GetIRQnEx variants and name the invalid IRQn sentinel` (X6)
5. `feat(dma): add SetIdleMemoryEx with explicit length, and DMA_LL_SetIdleMemory` (X11)
6. `fix(dma): validate peripheral burst with the RM0399 peripheral-side rule` (X7b)
7. `build(prj_04): inject FW_GIT_REV into the selftest banner` (G4.1)
8. `test(prj_04): …` uno por punto: Y3, Y4, Y5, X10a, X10b
9. `docs(dma): …` README, capturas y controles negativos (G5)
