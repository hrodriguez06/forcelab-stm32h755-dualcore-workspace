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

## F1, corrección (`1493792`, código y herramienta en `ebf2398`)

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| Firmware, revisado por mí | **F-B1 cerrado:**<br>• `HAL_ETH_RxAllocateCallback` marca `g_rx_rearm_pending` cuando no hay búfer;<br>• `ETH_Driver_ReleaseRxBuffer` devuelve si había un rearme pendiente;<br>• `custom_free_function` hace entonces `xSemaphoreGive`, en el contexto de hilo de lwIP;<br>• el hilo de entrada espera además con un plazo de 10 ms en modo cero-copia;<br>• `ETH_Driver_ReadPacket` llama siempre a `HAL_ETH_ReadData`, que rearma.<br>**F-M1 cerrado:** se guarda `next` antes de liberar. **F-m1 cerrado.**<br>Todo queda detrás de `ETH_HAL_RX_ZERO_COPY` |
| T1–T4, ejecutadas por mí | `eth_e3e6_audit_t1t4_1493792.txt`: HEAD `1493792`, **`OVERALL VERDICT: PASS`**.<br>• T1: 8/8 con 0 avisos reales; el control con flags del proyecto, rechazado.<br>• T2: los 8 objetos de `prj_03`/`prj_04` idénticos al control, compilado en la ejecución, en secciones ALLOC (con NOBITS), contenidos, relocalizaciones y símbolos.<br>• T3: 0 de 6 callbacks con la macro; sin ella, definición múltiple.<br>• T4: SRAM3 24 768 / 32 768 B; `g_rx_pool` en `0x300400c0..0x300448c0`, alineado y en la región 6, en `debug` y `release`; sin copia en la recepción; controles rechazados |
| T5/T6 (herramienta), revisión de un subagente | **Segura para la placa, pero todavía no da evidencia válida:** detalle abajo |

### Estado de T5/T6

| ID | Estado |
| --- | --- |
| B1 | **Casi cerrado:**<br>• control y HEAD se compilan en la ejecución y se flashean con el `flash.py` de cada árbol (los ELF correctos, comprobado), con el SHA-256 de CM7;<br>• **falta:** comprobar el worktree del control en T5, el SHA-256 de CM4, la salida de `flash.py` en la captura y que las dos imágenes sean distintas |
| B2 | **Bloqueante:**<br>• `evaluate_monitor_net_lines("")` da PASS, y no se exige ninguna línea `[net]`;<br>• el monitor no usa `PYTHONIOENCODING=utf-8:backslashreplace` (un U+FFFD de un reset lo mata);<br>• se para justo después de la carga, y `[net]` sale cada 5 s, así que puede no ver la muestra posterior;<br>• no se mira `poll()`, y el log no llega a la captura |
| B3 | Cerrado |
| M1 | Cerrado (mediana de 3 y 0,98) |
| M2/M3 | **Parcial:**<br>• un prefijo corrupto en la carga en ventana hace caer la herramienta (`ValueError`, l. 1151) y deja el monitor abierto;<br>• un datagrama perdido no caduca, así que la ventana encoge;<br>• el `SO_RCVBUF` del PC (64 KB) va justo para 32 × 1472 B;<br>• la carga una a una no empareja por secuencia;<br>• no hay plazo total ni `try/finally` para los monitores |
| T6 | **Parcial:** no confirma qué imagen hay en la placa ni reintenta hasta tener enlace |
| Seguridad | Correcta:<br>• `flash.py` con `--sn`, sin borrado;<br>• `-rst` con `sn=` en HOTPLUG;<br>• todo unicast a `192.168.1.150:7`;<br>• la ventana limita el ritmo.<br>Pero `--local-echo` no impide que T6 resetee la placa |

### Estado

**F1 no pasa todavía, por la parte de placa de la herramienta.** El firmware y T1–T4 están bien. Siguiente ronda, solo la herramienta de T5/T6 (lo de arriba). Después, la sesión de placa.

## F1, corrección 2 (`1150083`, herramienta en `15d1d3a`) y sesión de placa

### La herramienta (revisada por mí)

B2 está cerrado:
- `evaluate_monitor_net_lines` exige al menos 2 líneas `[net]`;
- el monitor corre con `PYTHONIOENCODING=utf-8:backslashreplace` y `--timestamps`;
- espera 6 s tras cada carga y comprueba `poll()`;
- guarda el log y su SHA-256 en la captura.

**B1 está cerrado:** el control y HEAD se compilan en la ejecución y se comparan sus SHA-256, que deben ser distintos.

**La carga en ventana:** en la prueba local (`eth_e3e6_t5_local_15d1d3a.txt`) aguanta un eco que corrompe (19 corruptas) y uno que pierde (25 caducadas). `--local-echo` impide T6.

### Sesión de placa (`eth_e3e6_audit_t5t6_1150083.txt`, HEAD `1150083`, `prj_02` `debug`)

Imágenes compiladas en la ejecución:
- **control:** CM7 `ae8372ad…`;
- **HEAD:** CM7 `2b8f0cde…`.

| Prueba | Control (copia, 8 búferes) | HEAD (cero-copia, 12 búferes) |
| --- | --- | --- |
| `net_check` | PASS | PASS |
| Carga una a una, 3 × 10 000 × 1472 B | 30 000/30 000, mediana 6,28 Mbit/s | 30 000/30 000, mediana 6,19 Mbit/s (0,986 × el control: pasa el 0,98) |
| Carga con 32 en vuelo, 5000 × 1472 B | **5000/5000, 54,06 Mbit/s, 0 perdidos** | **4976/5000, 32,84 Mbit/s, 24 perdidos** |
| `[net]` del monitor | `rx_nobuf=0` en todas las muestras | **`rx_nobuf=26`** en las dos últimas muestras |
| T6, 10 resets | — | 10/10 (enlace a los 3,2–3,3 s, ping y eco) |

`OVERALL VERDICT: FAIL`, por el monitor de HEAD.

**Qué significa:**
- **El cero-copia funciona y no se atasca** (F-B1 resuelto: la carga termina y T6 pasa), y a una trama cada vez rinde igual que la copia.
- **Bajo ráfaga empeora la recepción:** pierde tramas (`rx_nobuf` = 26) y rinde un 39 % menos.
- **La causa probable es la capacidad de búfer:**
  - con la copia, los 8 búferes DMA se devuelven enseguida y las tramas esperan en los 16 pbufs de `PBUF_POOL`, unas 24 tramas en total;
  - con el cero-copia, solo caben los 12 búferes DMA que lwIP retiene hasta procesar el eco.
- **No hay una medida de CPU** que muestre la ganancia que justificaba E6.

### Estado

**F1 no pasa.** No es un fallo de la herramienta: es un resultado del producto. Tal como está, E6 es una regresión de `prj_02` bajo ráfaga. La decisión es del responsable.
