# Informe de implementación de `08_eth` E3–E7 (Fase F1 - Revisión 2)

Oct 16, 2026 · @Antigravity

Entrega corregida de la fase F1 del plan [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 2) en la rama `driver/eth-e3-e6`, resolviendo la totalidad de los hallazgos de la auditoría [`AUDIT.md`](AUDIT.md) (commit de auditoría `0441d75`).

---

## 1. Resumen de correcciones de auditoría (F1 Revisión 2)

### 1.1 Firmware
- **F-B1 (Bloqueante): Inanición y parada permanente de recepción.**
  - Si el pool se agotaba temporalmente (`*buff == NULL` en `HAL_ETH_RxAllocateCallback`), el descriptor DMA quedaba desarmado hasta la siguiente llamada a `HAL_ETH_ReadData`. Dado que el hilo de recepción esperaba el semáforo con `portMAX_DELAY` y `ETH_Driver_ReleaseRxBuffer` no despertaba a nadie, la recepción se bloqueaba de forma definitiva.
  - **Solución implementada:** Se incorporó la variable de estado `g_rx_rearm_pending` ([`eth_hal.c:147, 796-799`](eth_hal.c#L147)). Si la reserva falla, `g_rx_rearm_pending` se pone a 1. Al liberarse un búfer en `ETH_Driver_ReleaseRxBuffer` ([`eth_hal.c:751-755`](eth_hal.c#L751-L755)), si había rearme pendiente, se limpia la bandera y se retorna `1U`. En `ethernetif_rx_pbuf_free_custom` ([`ethernetif.c:78-84`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L78-L84)), si se recibe dicho aviso de rearme, se invoca `xSemaphoreGive(s_rx_semaphore)` despertando al hilo de entrada. Adicionalmente, en `ethernetif_input_thread` ([`ethernetif.c:296`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L296)) se sustituyó `portMAX_DELAY` por una espera con plazo periódico de 10 ms (`pdMS_TO_TICKS(10U)`) en modo cero-copia.
- **F-M1 (Mayor): Pérdida de búferes en ruta de error de `ethernetif_receive_one`.**
  - El bucle de limpieza tras fallo de reserva liberaba `segment->payload` antes de avanzar `segment = segment->next`. Dado que `ETH_Driver_ReleaseRxBuffer` pone `segment.next = NULL`, los segmentos subsecuentes se desvinculaban y se perdían del pool.
  - **Solución implementada:** Se almacena `next_seg = segment->next` antes de la llamada de liberación en ambos bucles de error ([`ethernetif.c:188-193, 209-214`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L188-L214)).
- **F-m1 (Menor): Comentario de cabecera desactualizado.**
  - Actualizado en [`ethernetif.c:19-22`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L19-L22) para documentar explícitamente que la recepción en `prj_02` opera en cero-copia hacia SRAM3 y que la transmisión mantiene la copia.

### 1.2 Arnés de verificación (`tools/eth_e3e6_checks.py`)
- **B1 (Bloqueante): Control T5 y flasheo independiente.** `prj_02` `debug` de HEAD y del control `69179d7` se compilan in-situ durante T5, se registra el SHA-256 de cada ELF y cada imagen se flashea utilizando el script `flash.py` de su propio árbol.
- **B2 (Bloqueante): Monitorización de contadores hardware en T5.** Se ejecuta `monitor.py` en segundo plano para cada imagen bajo prueba, analizando todas las líneas `[net]` para verificar `rx_nobuf=0`, `tx_nobuf=0` y `dma_err=0`. Control negativo sintético con contadores erróneos verificado.
- **B3 (Bloqueante): Códigos de salida de `net_check.py`.** Evaluados estrictamente e integrados en el veredicto global de T5.
- **M1 (Mayor): Criterio de throughput.** Mediana de 3 repeticiones en ambas imágenes: `mediana(HEAD) >= 0.98 * mediana(control)`.
- **M2 (Mayor): Robustez en carga T5.** Límite de plazo total (`total_deadline`), aborto tras 50 vencimientos consecutivos (`max_consecutive_timeouts`) y emparejamiento por número de secuencia de datagrama con vaciado de tardías.
- **M3 (Mayor): Carga en ventana de 32 datagramas en vuelo.** Incorporada en `run_udp_windowed_load`, capaz de agotar y poner a prueba la recuperación del pool de 12 búferes.
- **M4 (Mayor): Comparación binaria profunda en T2.** Inspección exhaustiva de `objdump -h` (46 secciones ALLOC con nombres, tamaños y banderas, incluyendo `.bss`, `.RxArraySection`, `.RxDescripSection`, `.TxDescripSection`, `.eth_ram`), `objdump -s` (volcado de contenidos no-debug), `objdump -r` (relocalizaciones fuera de depuración) y `nm -p` (símbolos sin depuración).
- **M5 (Mayor): Compilación in-situ y frescura.** T2 y T4 garantizan la compilación de HEAD y del control dentro de la ejecución, validando `st_mtime`.
- **M6 (Mayor): Árbol limpio con ignorados.** Conforme al Plan Revisión 2, solo los directorios `build/` pueden crearse o modificarse; cualquier archivo nuevo fuera de `build/` (incluso si está ignorado en `.gitignore`) causa fallo del chequeo.
- **M7 (Mayor): Validación de salida de `nm` en T3.** Se exige código de salida 0 y la presencia del símbolo fuerte `ETH_Driver_Init`.
- **M8 (Mayor): Cobertura de funciones de recepción en T4.** Análisis de desensamblado extendido a `ethernetif_receive_one`, `ethernetif_input_thread` (donde el compilador enlínea la recepción en optimización `-O2`/`-O3`) y `ethernetif_rx_pbuf_free_custom`, verificando la ausencia total de llamadas a `memcpy`, `__aeabi_memcpy*`, `pbuf_take` y `pbuf_take_at`.
- **m1–m3:** Flags del proyecto para control negativo T1, extracción dinámica de Región 6 MPU desde `mpu_config.c` (verificando inicio y fin de `g_rx_pool` en `debug` y `release`), y tiempo de asentamiento de 5.0 s en T6.

---

## 2. Cambios realizados con citas `archivo:línea` contra HEAD (`ebf2398`)

Todas las citas han sido comprobadas directamente contra el commit de código y herramienta `ebf2398`:

### 2.1 Driver Ethernet compartido (`01_shared_drivers/08_eth`)

1. [`01_shared_drivers/08_eth/eth_hal.h:48-50`](eth_hal.h#L48-L50):
   - Pool de recepción configurable desde el proyecto mediante guarda `#ifndef ETH_DRIVER_RX_BUFFER_COUNT`, manteniendo el valor por defecto en `8U`.
2. [`01_shared_drivers/08_eth/eth_hal.h:172-180`](eth_hal.h#L172-L180):
   - Declaración de la API de liberación individual de búferes DMA `ETH_Driver_ReleaseRxBuffer(void *buffer)` bajo la macro condicional `ETH_HAL_RX_ZERO_COPY`. Retorna no-cero si se requiere rearme del anillo tras inanición.
3. [`01_shared_drivers/08_eth/eth_hal.h:246-253`](eth_hal.h#L246-L253):
   - Documentación y definición del hook `ETH_HAL_CUSTOM_CALLBACKS`.
4. [`01_shared_drivers/08_eth/eth_hal.c:162-164`](eth_hal.c#L162-L164):
   - `_Static_assert` de tamaño de descriptores y búferes en D2 SRAM3 (32 KB máximo).
5. [`01_shared_drivers/08_eth/eth_hal.c:740-760`](eth_hal.c#L740-L760):
   - Implementación de `ETH_Driver_ReleaseRxBuffer(void *buffer)`. Identifica el descriptor mediante `eth_rx_index_of()`, libera el slot (`in_use = 0U`) bajo sección crítica `eth_lock()` / `eth_unlock()`, y comprueba/limpia `g_rx_rearm_pending` retornando `1U` si se requiere despertar al hilo de recepción.
6. [`01_shared_drivers/08_eth/eth_hal.c:775-878`](eth_hal.c#L775-L878):
   - Los seis callbacks de HAL ETH (`HAL_ETH_RxAllocateCallback`, `HAL_ETH_RxLinkCallback`, `HAL_ETH_TxFreeCallback`, `HAL_ETH_RxCpltCallback`, `HAL_ETH_TxCpltCallback`, `HAL_ETH_ErrorCallback`) están condicionados a `#if !defined(ETH_HAL_CUSTOM_CALLBACKS)`.
   - `HAL_ETH_RxAllocateCallback` ([`eth_hal.c:796-799`](eth_hal.c#L796-L799)) marca `g_rx_rearm_pending = 1U` si no hay búfer disponible en el pool.

### 2.2 Proyecto de referencia `prj_02_eth_lwip_dualcore`

1. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/CMakeLists.txt:91-92`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/CMakeLists.txt#L91-L92):
   - Definición de directivas de compilación para CM7:
     ```cmake
     ETH_HAL_RX_ZERO_COPY=1
     ETH_DRIVER_RX_BUFFER_COUNT=12U
     ```
2. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Inc/lwipopts.h:50`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Inc/lwipopts.h#L50):
   - Habilitación de compatibilidad con pbufs personalizados en lwIP:
     ```c
     #define LWIP_SUPPORT_CUSTOM_PBUF        1
     ```
3. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:19-22`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L19-L22):
   - Cabecera del archivo reflejando la arquitectura de cero-copia en recepción hacia SRAM3.
4. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:66-72`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L66-L72):
   - Declaración de `struct rx_custom_pbuf` y del pool estático lwIP `LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_DRIVER_RX_BUFFER_COUNT, sizeof(struct rx_custom_pbuf), "Zero-copy RX PBUF pool")`.
5. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:74-86`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L74-L86):
   - Implementación de `ethernetif_rx_pbuf_free_custom(struct pbuf *p)`: invoca `ETH_Driver_ReleaseRxBuffer(cp->dma_buffer)`, despierta `s_rx_semaphore` si el driver reporta rearme pendiente, y retorna el descriptor al pool con `LWIP_MEMPOOL_FREE(RX_POOL, cp)`.
6. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:159-242`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L159-L242):
   - `ethernetif_receive_one()` en cero-copia:
     - Entrega de cada segmento como `pbuf_custom` (`PBUF_REF`) referenciando directamente a `g_rx_pool[i]` en SRAM3.
     - Preservación de punteros `next` en bucles de liberación de error ([`ethernetif.c:188-193, 209-214`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L188-L214)).
     - Entrega de trama a la pila en [`ethernetif.c:237`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L237) (`netif->input(p_head, netif)`).
7. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:292-299`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L292-L299):
   - `ethernetif_input_thread()` con timeout de re-armado defensivo de 10 ms (`pdMS_TO_TICKS(10U)`) en modo cero-copia.
8. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:518`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L518):
   - Inicialización del pool de pbufs personalizados en `low_level_init()` mediante `LWIP_MEMPOOL_INIT(RX_POOL)`.

---

## 3. Contextos de liberación del búfer y concurrencia (Diseño 3)

La liberación de búferes DMA en recepción se produce mediante `ethernetif_rx_pbuf_free_custom()`, ejecutado cuando lwIP agota las referencias (`pbuf_free()`):

1. **Contexto de `tcpip_thread` (prioridad 7):**
   - Tráfico UDP/ICMP: tras responder al eco UDP o procesar la petición ICMP, lwIP libera el pbuf desde este hilo.
   - Tráfico TCP: al ser reconocidos o consumidos los datos.
2. **Contexto de `ethernetif_input_thread` (prioridad 6):**
   - Si `RX_POOL` se agota o falla `pbuf_alloced_custom()` ([`ethernetif.c:188, 209`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L188-L209)), los segmentos se liberan de inmediato con `ETH_Driver_ReleaseRxBuffer()`.
   - Si `netif->input()` rechaza la trama ([`ethernetif.c:237-240`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L237-L240)), `pbuf_free(p_head)` libera toda la cadena en el mismo hilo de entrada.
3. **Contexto de hilos de aplicación:**
   - Si una tarea de usuario recibe un pbuf (vía sockets BSD o netconn) y llama a `pbuf_free()`.
4. **Contexto de ISR:**
   - **Nunca.** `pbuf_free()` no es reentrante ni seguro desde ISR en lwIP. Las ISRs de Ethernet solo despiertan semáforos binarios de FreeRTOS.
5. **Seguridad de concurrencia:**
   - `ETH_Driver_ReleaseRxBuffer()` ([`eth_hal.c:747-756`](eth_hal.c#L747-L756)) protege la actualización de `in_use` y `g_rx_rearm_pending` con PRIMASK (`eth_lock()` / `eth_unlock()`), garantizando exclusión mutua contra `HAL_ETH_RxAllocateCallback()` y llamadas concurrentes entre hilos.
   - `LWIP_MEMPOOL_FREE(RX_POOL, cp)` ([`ethernetif.c:85`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L85)) está protegido internamente por `SYS_ARCH_PROTECT` de lwIP.

---

## 4. Presupuesto de memoria SRAM3 (.eth_ram) y aclaración de descriptores (m4)

La región SRAM3 (D2, `0x30040000`, 32 KB / 32,768 B) está mapeada en la región 6 de la MPU como Device/No-Cacheable.

> [!IMPORTANT]
> **Aclaración de descriptores DMA vs. pool de búferes:**
> El número de descriptores DMA está fijado por la configuración HAL en `stm32h7xx_hal_conf.h` (`ETH_RX_DESC_CNT = 4` y `ETH_TX_DESC_CNT = 4`), consumiendo exactamente 96 B por sección (4 descriptores × 24 B) tanto en el control como en HEAD.
> `ETH_DRIVER_RX_BUFFER_COUNT` es el tamaño del *pool de búferes de memoria*, no el número de descriptores DMA.

Desglose exacto extraído de los archivos `.map` (`build/debug/cm7_firmware.map` de `prj_02` HEAD y del control `69179d7`):

| Componente | Control `69179d7` (8 Rx / 4 Tx) | HEAD `ebf2398` en `prj_02` (12 Rx / 4 Tx) | Delta |
| :--- | :---: | :---: | :---: |
| **Descriptores Rx DMA (`.RxDescripSection`)** | 4 descriptores (0x60) = 96 B | 4 descriptores (0x60) = 96 B | 0 B |
| **Descriptores Tx DMA (`.TxDescripSection`)** | 4 descriptores (0x60) = 96 B | 4 descriptores (0x60) = 96 B | 0 B |
| **Búferes Rx DMA (`.RxArraySection`, `g_rx_pool`)** | 8 × 1536 B (0x3000) = 12,288 B | 12 × 1536 B (0x4800) = 18,432 B | +6,144 B |
| **Búferes Tx DMA (`.eth_ram`, `g_tx_pool`)** | 4 × 1536 B (0x1800) = 6,144 B | 4 × 1536 B (0x1800) = 6,144 B | 0 B |
| **Total sección `.eth_ram` en SRAM3** | **18,624 B** (0x48c0, 56.84%) | **24,768 B** (0x60c0, 75.59%) | **+6,144 B** |
| **Margen libre en SRAM3** | **14,144 B** (43.16%) | **8,000 B** (24.41%) | -6,144 B |

- **Pool `struct rx_custom_pbuf` (`memp_memory_RX_POOL_base`):** `12 × 24 B = 288 B`, ubicado fuera de SRAM3 en AXI SRAM D1 (`.bss` a `0x200001c0`).
- **Alineación:** `g_rx_pool` inicia en `0x300400c0` (múltiplo de 32 B) y termina en `0x300448c0`, dentro de la Región 6 MPU (`0x30040000..0x30048000`), comprobado en T4 para `debug` y `release`.

---

## 5. Resultados de las pruebas off-board (T1–T4) y capturas

Todas las pruebas se ejecutaron mediante [`tools/eth_e3e6_checks.py`](../../tools/eth_e3e6_checks.py) con árbol limpio comprobado antes y después de cada prueba.

> [!NOTE]
> **Identificador de las capturas:** Las capturas llevan el sufijo `_ebf2398` correspondiente al commit de código y herramienta `ebf2398`. Fueron generadas inmediatamente tras dicho commit y commiteadas en el repositorio (`b3de10b`).

| Prueba | Objetivo y Oráculo | Resultado | Archivo de captura |
| :---: | :--- | :---: | :--- |
| **T1** | Compilación directa `clean-first` de la matriz CI de 8 configuraciones (`prj_01` a `prj_04`, `debug` y `release`). Oráculo: código de salida 0, 0 advertencias, objetos generados > 0 y frescura de binarios. Control negativo: archivo C temporal con advertencia deliberada compilado con flags reales del proyecto es detectado y rechazado. | **PASS** (8/8 configs, 0 avisos) | [`docs/eth_e3e6_t1_ebf2398.txt`](docs/eth_e3e6_t1_ebf2398.txt) |
| **T2** | Invarianza binaria absoluta de `prj_03` y `prj_04`. Compilación in-situ del control `69179d7` con `-ffile-prefix-map`. Comparación profunda en 8 objetos (4 configs × 2 objetos) de `objdump -h` (46 secciones ALLOC incluyendo `.bss`, `.RxArraySection`, etc.), `objdump -s` (datos brutos no-debug), `objdump -r` (relocalizaciones no-debug) y `nm -p` (símbolos). Control negativo: objeto mutado detectado y rechazado. | **PASS** (8/8 objetos idénticos) | [`docs/eth_e3e6_t2_ebf2398.txt`](docs/eth_e3e6_t2_ebf2398.txt) |
| **T3** | Opt-out E3 (`ETH_HAL_CUSTOM_CALLBACKS`). Compilación con la macro: `nm` con código de salida 0 confirma que el driver define 0 de los 6 callbacks y contiene `ETH_Driver_Init`. Enlace parcial exitoso con callbacks de usuario. Control negativo: enlace sin la macro falla por conflicto de símbolos duplicados. | **PASS** (0/6 definidos, enlace ok) | [`docs/eth_e3e6_t3_ebf2398.txt`](docs/eth_e3e6_t3_ebf2398.txt) |
| **T4** | Presupuesto y cero-copia en `prj_02`. Mapa de memoria: `.eth_ram` = 24,768 B (≤ 32 KB). Límites dinámicos de Región 6 MPU desde `mpu_config.c`: `g_rx_pool` alineado a 32 B en `debug` y `release`. Desensamblado de ruta de recepción (`ethernetif_receive_one`, `ethernetif_input_thread`, `ethernetif_rx_pbuf_free_custom`) libre de `memcpy`, `__aeabi_memcpy*`, `pbuf_take` o `pbuf_take_at`. Controles negativos (.map de 33 KB y desensamblado con `__aeabi_memcpy` inyectado) detectados y rechazados. | **PASS** (24,768 B, 0 copias) | [`docs/eth_e3e6_t4_ebf2398.txt`](docs/eth_e3e6_t4_ebf2398.txt) |
| **T5 (local)** | Validación fuera de placa del motor de carga UDP de la herramienta contra servidor de eco local en `127.0.0.1`: prueba secuencial de 10,000 datagramas de 1472 B (46.66 Mbit/s, 0 pérdidas) y prueba en ventana de 32 datagramas en vuelo (52.98 Mbit/s, 0 pérdidas). Controles negativos (payload corrupto y contadores erróneos) rechazados. | **PASS** (0 pérdidas, payload exacto) | [`docs/eth_e3e6_t5_local_ebf2398.txt`](docs/eth_e3e6_t5_local_ebf2398.txt) |

---

## 6. Pruebas de placa del auditor (T5–T6) e instrucciones exactas

El implementador no tiene acceso a la placa física ni ejecutó comandos contra el hardware. La prueba en placa se reserva íntegramente al auditor, quien realizará una única sesión con la placa Nucleo-H755ZI-Q (`--sn 0016000C4142501220353451`, IP `192.168.1.150`).

### Comandos para la sesión de auditoría con la placa:

```powershell
# Ejecución combinada de T5 y T6 con registro en captura
py tools/eth_e3e6_checks.py --test t5,t6 --sn 0016000C4142501220353451 --target-ip 192.168.1.150 -o 01_shared_drivers/08_eth/docs/eth_e3e6_t5t6_<auditor_hash>.txt
```

O de forma independiente:
```powershell
# T5: Flasheo comparativo de control (69179d7) y HEAD, net_check, monitor [net], carga secuencial y 32-in-flight (mediana de 3 runs >= 0.98 * control)
py tools/eth_e3e6_checks.py --test t5 --sn 0016000C4142501220353451 --target-ip 192.168.1.150 -o 01_shared_drivers/08_eth/docs/eth_e3e6_t5_<auditor_hash>.txt

# T6: Resistencia a 10 reinicios por SWD (-rst) con 5.0 s de asentamiento de PHY y ping/burst
py tools/eth_e3e6_checks.py --test t6 --sn 0016000C4142501220353451 --target-ip 192.168.1.150 -o 01_shared_drivers/08_eth/docs/eth_e3e6_t6_<auditor_hash>.txt
```

---

## 7. Declaración explícita de lo no verificado

En estricto cumplimiento de las instrucciones de entrega:
- **T5 (Prueba de throughput y tráfico en placa física):** **NO VERIFICADA** por el implementador; reservada para el auditor.
- **T6 (Prueba de estrés de 10 resets SWD en placa física):** **NO VERIFICADA** por el implementador; reservada para el auditor.

---

F1 CORREGIDA PARA AUDITORIA ebf2398
