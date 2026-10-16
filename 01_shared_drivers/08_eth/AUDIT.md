# Auditoría de `08_eth` E3–E7

Auditor: Claude (Claude Code), para Hector Rodriguez · Rama: `driver/eth-e3-e6` · Plan: revisión 2

## F1 (`d428b77`, código en `fa19331`, `9da7c4f`; herramienta en `a249894`)

Revisé yo el firmware. La herramienta la revisó un subagente; los hallazgos de firmware los comprobé yo en el código. **No hice todavía la sesión de placa:** la haré una sola vez, con el código final.

### Firmware

**Correcto:**
- **E3:** los seis callbacks quedan bajo `#if !defined(ETH_HAL_CUSTOM_CALLBACKS)` (`eth_hal.c:764`, `:861`).
- **`ETH_DRIVER_RX_BUFFER_COUNT`:** se puede fijar desde el proyecto (`eth_hal.h:48-50`), y el `_Static_assert` de SRAM3 sigue.
- **`ETH_Driver_ReleaseRxBuffer`:** está detrás de `ETH_HAL_RX_ZERO_COPY` y protegida con PRIMASK, así que es segura desde cualquier hilo (`eth_hal.c:736-750`).
- **`prj_02`:** define `ETH_HAL_RX_ZERO_COPY=1` y 12 búferes (`CM7/CMakeLists.txt:91-92`).
- **El camino normal:** cada segmento se entrega como un `pbuf_custom` (`PBUF_REF`) sobre el búfer DMA, y lwIP lo libera con `custom_free_function`.
- **Sin la macro,** el código de `prj_03`/`prj_04` es el de antes. El subagente comprobó a mano que sus secciones ALLOC y sus relocalizaciones son iguales a las del control; T2 debe probarlo (M4).

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| F-B1 | Bloqueante (firmware) | **La recepción se puede parar para siempre:**<br>• si `HAL_ETH_RxAllocateCallback` no encuentra búfer libre, el descriptor queda sin armar "hasta el siguiente `HAL_ETH_ReadData`" (`eth_hal.c:786-792`);<br>• el hilo de entrada espera el semáforo con `portMAX_DELAY` (`ethernetif.c:284`);<br>• `ETH_Driver_ReleaseRxBuffer` no despierta a nadie.<br>Si lwIP retiene los búferes suficientes, los descriptores no se vuelven a armar y no llega ninguna interrupción más. Con la copia no pasaba | Al liberar un búfer, si hubo un fallo de reserva pendiente, despertar al hilo de entrada para que rearme (por ejemplo, una función del driver detrás de la macro que diga si hay rearme pendiente, y un `xSemaphoreGive` desde `custom_free_function`, nunca desde una ISR). O que el hilo espere con un plazo corto en modo cero-copia. Probarlo con la carga en ventana de T5 |
| F-M1 | Mayor (firmware) | **La limpieza de la ruta de error de `ethernetif_receive_one` pierde búferes.** El bucle `while (segment != NULL) { ETH_Driver_ReleaseRxBuffer(segment->payload); segment = segment->next; }` lee `next` después de liberar, y la liberación lo pone a `NULL` (`eth_hal.c:743`). Así que el resto de la cadena nunca vuelve al pool. Hoy no se da, porque una trama de 1536 B cabe en un segmento | Guardar `next` antes de liberar, en los dos bucles |
| F-m1 | Menor | `ethernetif.c:19` sigue diciendo "Frames are copied" | Corregirlo |
| B1 | Bloqueante (herramienta) | **El "control" de T5 es la imagen de HEAD:** se usa el `flash.py` de `prj_02` del repo, que fija la raíz desde su propio `__file__` (`prj_02/tools/flash.py:18-32`), así que el `cwd` del control no cuenta. Además, `prj_02` de control no se compila nunca, y el de HEAD no se recompila en T5 | Compilar `prj_02` `debug` de HEAD y del control en la ejecución, flashear cada uno con su propio `flash.py` (o con `--elf`), y registrar el SHA-256 de cada ELF |
| B2 | Bloqueante | `monitor.py` no se ejecuta, así que `rx_nobuf`, `tx_nobuf` y `dma_err` no se comprueban | Monitor en segundo plano por imagen, con las líneas `[net]` analizadas y los tres contadores a 0, y un control negativo por el mismo analizador |
| B3 | Bloqueante | Los códigos de salida de `net_check` se registran, pero no cuentan (l. 913-917, 934-937) | Que cuenten en el veredicto |
| M1 | Mayor | El criterio de T5 es `≥ 0,95 × control` (l. 943) | Plan revisión 2: la mediana de 3 repeticiones, la nueva ≥ 0,98 × control |
| M2 | Mayor | **T5:**<br>• no tiene plazo total (10 000 × 2 s si la placa no responde);<br>• no aborta tras N vencimientos seguidos;<br>• no empareja las respuestas por secuencia: una respuesta tardía hace "corruptas" todas las siguientes | Plazo total, aborto tras N vencimientos, y emparejamiento por número de secuencia vaciando las tardías |
| M3 | Mayor | T5 tiene un solo datagrama en vuelo, así que no puede vaciar el pool | Plan revisión 2: además, una carga con 32 en vuelo |
| M4 | Mayor | **T2 compara menos de lo que dice:**<br>• `objdump -s` no vuelca `.bss`;<br>• no compara relocalizaciones, símbolos, tamaños ni las secciones propias (`.RxArraySection`, `.RxDescripSection`, `.TxDescripSection`, `.eth_ram`);<br>• en objetos sintéticos, llamadas a funciones distintas o un `.bss` de otro tamaño pasan | Comparar también `objdump -h` (secciones ALLOC con tamaño, incluidas NOBITS), `objdump -r` (relocalizaciones fuera de depuración) y `nm` sin símbolos de depuración |
| M5 | Mayor | T2 y T4 no compilan HEAD ni comprueban que los objetos sean de esta ejecución | Compilar HEAD en la ejecución o rechazar objetos anteriores a su inicio |
| M6 | Mayor | El árbol limpio no mira los ignorados (l. 196) | Plan revisión 2: solo `build/` está permitido; cualquier otro archivo nuevo, ignorado o no, es FAIL |
| M7 | Mayor | En T3 se ignora el código de salida de `nm` (l. 599); un `nm` que falla da 0 de 6 definidos y PASS | rc == 0 y un símbolo conocido presente (`ETH_Driver_Init`) |
| M8 | Mayor | **T4 no ve todas las copias:** un `memcpy` en línea, un bucle o una copia en una función auxiliar estática pasan | Revisar también las funciones estáticas llamadas desde `ethernetif_receive_one` y buscar `__aeabi_memcpy*` |
| m1 | Menor | El control negativo de T1 no usa las flags del proyecto, y el recuento de objetos es inventado (l. 377-388) | Compilar el `.c` temporal con las flags de un proyecto |
| m2 | Menor | **T4:** la región 6 de la MPU está escrita a mano (l. 729); solo se mira el principio de `g_rx_pool`, no el final; y solo en `debug` | Sacarla de `mpu_config.c`, comprobar el principio y el final, y también `release` |
| m3 | Menor | **T6:**<br>• espera 3,5 s, que puede no bastar para el PHY;<br>• no confirma qué imagen hay en la placa;<br>• una `RuntimeError` de `objdump` en T2 sale sin veredicto | Corregir |
| m4 | Menor | **El informe:**<br>• la tabla de SRAM3 (l. 94-99) cuenta un descriptor por búfer (son `ETH_RX_DESC_CNT` = 4), y la cifra del control no tiene captura;<br>• la l. 114 dice que se compara `.bss`;<br>• la l. 123 da un resultado de eco local sin captura;<br>• dos citas mal (`eth_hal.h:243-251` es un comentario; `netif->input` está en l. 229);<br>• las capturas se llaman `_a249894`, pero se hicieron en commits posteriores solo de documentos (decirlo) | Corregir |

### Estado

**F1 no pasa.**
- **El firmware tiene un bloqueante real** (F-B1) y un mayor (F-M1).
- **La herramienta tiene tres bloqueantes** que darían un T5 falso (B1–B3).
- **Siguiente ronda:** todo lo anterior. Después, una ejecución mía de T1–T4 y una sola sesión de placa en `prj_02`.
