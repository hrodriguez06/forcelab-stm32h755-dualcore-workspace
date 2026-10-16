# Informe de implementación de `08_eth` E3–E7 (Fase F1)

Oct 16, 2026 · @Antigravity

Entrega de la fase F1 del plan [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1) en la rama `driver/eth-e3-e6`.

---

## 1. Resumen del alcance y decisiones

En conformidad con las cuatro decisiones aprobadas por el responsable el 2026-10-16 (1A, 2A, 3A y 4):
- **E6 (H1 y H2):** Cero-copia implementado exclusivamente en el camino de recepción en `prj_02_eth_lwip_dualcore` mediante `pbuf_custom` (`LWIP_SUPPORT_CUSTOM_PBUF = 1`). El camino de transmisión permanece inalterado (con copia), preservando la configuración de la MPU y el aislamiento de SRAM3.
- **E3 (H3):** Macro de opt-out `ETH_HAL_CUSTOM_CALLBACKS` incorporada en `eth_hal.h` y `eth_hal.c`, permitiendo a proyectos consumidores sobrescribir los 6 callbacks HAL de Ethernet sin conflictos de símbolos duplicados.
- **E4 y E7 (H4):** Documentados como "por diseño" (instancia única por arquitectura de silicio STM32H7; copia privada de `prj_eth_lwip_node` preservada).
- **H5:** Herramienta de verificación [`tools/eth_e3e6_checks.py`](../../tools/eth_e3e6_checks.py) implementando las pruebas off-board T1–T4 con controles negativos mutados y el arnés de prueba de carga UDP / SWD resets para la sesión de auditoría (T5–T6). Probado sin placa contra un servidor de eco UDP local en `127.0.0.1`.
- **Integridad de proyectos no modificados:** `prj_03_adc_dma_eth` y `prj_04_adc_dma_bm_eth` no sufrieron cambios de código, y la prueba T2 demuestra que sus objetos compilados son idénticos byte a byte a los del commit de control `69179d7`.

---

## 2. Cambios realizados con citas `archivo:línea` contra HEAD

Todos los archivos y rangos de líneas han sido comprobados contra HEAD:

### 2.1 Driver Ethernet compartido (`01_shared_drivers/08_eth`)

1. [`01_shared_drivers/08_eth/eth_hal.h:48-50`](eth_hal.h#L48-L50):
   - Pool de recepción configurable desde el proyecto mediante guarda `#ifndef ETH_DRIVER_RX_BUFFER_COUNT`, manteniendo el valor por defecto en `8U`.
2. [`01_shared_drivers/08_eth/eth_hal.h:172-178`](eth_hal.h#L172-L178):
   - Declaración de la API de liberación individual de búferes DMA `ETH_Driver_ReleaseRxBuffer(void *buffer)` bajo la macro condicional `ETH_HAL_RX_ZERO_COPY`.
3. [`01_shared_drivers/08_eth/eth_hal.h:243-251`](eth_hal.h#L243-L251):
   - Documentación y definición del hook `ETH_HAL_CUSTOM_CALLBACKS`.
4. [`01_shared_drivers/08_eth/eth_hal.c:736-750`](eth_hal.c#L736-L750):
   - Implementación de `ETH_Driver_ReleaseRxBuffer(void *buffer)`. Identifica el descriptor mediante `eth_rx_index_of()` y libera el slot limpiando `next`, `length` e `in_use` bajo sección crítica `eth_lock()` / `eth_unlock()`.
5. [`01_shared_drivers/08_eth/eth_hal.c:764-861`](eth_hal.c#L764-L861):
   - Los seis callbacks de HAL ETH (`HAL_ETH_RxAllocateCallback`, `HAL_ETH_RxLinkCallback`, `HAL_ETH_TxFreeCallback`, `HAL_ETH_RxCpltCallback`, `HAL_ETH_TxCpltCallback`, `HAL_ETH_ErrorCallback`) están condicionados a `#if !defined(ETH_HAL_CUSTOM_CALLBACKS)`.

### 2.2 Proyecto de referencia `prj_02_eth_lwip_dualcore`

1. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/CMakeLists.txt:91-92`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/CMakeLists.txt#L91-L92):
   - Definición de directivas de compilación para CM7:
     ```cmake
     ETH_HAL_RX_ZERO_COPY=1
     ETH_DRIVER_RX_BUFFER_COUNT=12U
     ```
2. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Inc/lwipopts.h:49-51`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Inc/lwipopts.h#L49-L51):
   - Habilitación de compatibilidad con pbufs personalizados en lwIP:
     ```c
     #define LWIP_SUPPORT_CUSTOM_PBUF        1
     ```
3. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:59-75`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L59-L75):
   - Declaración de `struct rx_custom_pbuf` y del pool estático lwIP `LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_DRIVER_RX_BUFFER_COUNT, sizeof(struct rx_custom_pbuf), "Zero-copy RX PBUF pool")`.
   - Implementación de `ethernetif_rx_pbuf_free_custom(struct pbuf *p)` que invoca `ETH_Driver_ReleaseRxBuffer(cp->dma_buffer)` y retorna el envoltorio con `LWIP_MEMPOOL_FREE(RX_POOL, cp)`.
4. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:156-234`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L156-L234):
   - Refactorización de `ethernetif_receive_one()` a cero-copia. Cada segmento recibido del DMA se asocia a un `pbuf_custom` mediante `pbuf_alloced_custom()` con payload apuntando directamente a la SRAM3 (`g_rx_pool[i]`). Se eliminaron completamente las llamadas a `pbuf_take_at()`, `pbuf_take()` o `memcpy()`. Manejo robusto de inanición/errores de asignación con liberación inmediata y contadores lwIP `link.memerr` / `link.drop`.
5. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:499-501`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L499-L501):
   - Inicialización del pool de pbufs personalizados en `low_level_init()` mediante `LWIP_MEMPOOL_INIT(RX_POOL)`.

---

## 3. Contextos de liberación del búfer (Diseño 3) y concurrencia

La liberación de búferes DMA en el esquema cero-copia se produce exclusivamente a través de `ethernetif_rx_pbuf_free_custom()`, invocado por `pbuf_free()` cuando el contador de referencias del `pbuf` llega a cero.

### 3.1 Contextos de ejecución

1. **Contexto del hilo de entrada (`ethernetif_input_thread`, prioridad 6):**
   - **Caso de fallo de asignación:** Si el pool `RX_POOL` se agota o `pbuf_alloced_custom()` falla al armar una trama multisegmento ([`ethernetif.c:184, 204`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L184-L204)), los búferes leídos del DMA se liberan inmediatamente mediante `ETH_Driver_ReleaseRxBuffer()` antes de descartar la trama.
   - **Rechazo por la pila:** Si `netif->input()` devuelve un código de error ([`ethernetif.c:231`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L231)), `pbuf_free(p_head)` libera toda la cadena en el mismo hilo de entrada.
2. **Contexto del hilo de la pila (`tcpip_thread`, prioridad 7):**
   - **Tráfico UDP / ICMP (como la prueba de eco):** Una vez que lwIP procesa el paquete UDP y transmite la respuesta de eco, la pila invoca `pbuf_free()` desde `tcpip_thread`.
   - **Tráfico TCP:** Al completarse el procesamiento de segmentos TCP o al ser consumidos por sockets, `pbuf_free()` se ejecuta en el contexto de `tcpip_thread`.
3. **Contexto de hilos de aplicación:**
   - Si una tarea de usuario recibe un pbuf (vía APIs de sockets o netconn) y posterga su liberación, la llamada a `pbuf_free()` ocurre en dicho hilo de aplicación.
4. **Contexto de ISR:**
   - **Nunca.** Por diseño y especificación estricta de lwIP, `pbuf_free()` no es seguro desde una rutina de servicio de interrupción (ISR) y el sistema no lo invoca en interrupciones Ethernet. La ISR de Ethernet solo notifica semáforos binarios a FreeRTOS (`on_rx_complete`).

### 3.2 Seguridad y sincronización

- En `ETH_Driver_ReleaseRxBuffer(void *buffer)` ([`eth_hal.c:737-749`](eth_hal.c#L737-L749)):
  - La resolución del índice del búfer (`eth_rx_index_of`) es aritmética pura y de solo lectura.
  - La actualización del estado del slot (`g_rx_slots[index].in_use = 0U`) se encuentra protegida por `eth_lock()` (`__get_PRIMASK()` / `__disable_irq()`) y `eth_unlock(primask)`.
  - Esta sección crítica garantiza exclusión mutua contra `HAL_ETH_RxAllocateCallback()` (que corre cuando la HAL recarga descriptores) y contra llamadas concurrentes entre hilos.
- En `ethernetif_rx_pbuf_free_custom()` ([`ethernetif.c:68-74`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L68-L74)):
  - `LWIP_MEMPOOL_FREE(RX_POOL, cp)` utiliza la protección nativa de mempools de lwIP (`SYS_ARCH_PROTECT`).

---

## 4. Presupuesto de memoria SRAM3 (.eth_ram)

La región SRAM3 (D2, `0x30040000`, 32 KB / 32,768 B) está configurada en la región 6 de la MPU como Device/No-Cacheable.

| Componente | Control `69179d7` (8 Rx / 4 Tx) | HEAD `a249894` en `prj_02` (12 Rx / 4 Tx) | Delta |
| :--- | :---: | :---: | :---: |
| **Descriptores Rx DMA** | 8 × 16 B = 128 B | 12 × 16 B = 192 B | +64 B |
| **Descriptores Tx DMA** | 4 × 32 B = 128 B | 4 × 32 B = 128 B | 0 B |
| **Búferes Rx DMA (`g_rx_pool`)** | 8 × 1536 B = 12,288 B | 12 × 1536 B = 18,432 B | +6,144 B |
| **Búferes Tx DMA (`g_tx_pool`)** | 4 × 1536 B = 6,144 B | 4 × 1536 B = 6,144 B | 0 B |
| **Total sección `.eth_ram`** | **18,624 B** (0x48c0, 56.84%) | **24,768 B** (0x60c0, 75.59%) | **+6,144 B** |
| **Margen libre en SRAM3** | **14,144 B** (43.16%) | **8,000 B** (24.41%) | -6,144 B |

### Memoria auxiliar fuera de SRAM3:
- El pool de estructuras envoltorio `struct rx_custom_pbuf` (`memp_memory_RX_POOL_base`) consume `12 × 24 B = 288 B` y reside en AXI SRAM D1 (`.bss` a `0x200001c0`), sin ocupar espacio en SRAM3 ni requerir coherencia DMA.
- La alineación de `g_rx_pool` a 32 bytes (`0x300400c0`) y su contención íntegra en el rango `0x30040000..0x30048000` se verifican en la prueba T4.

---

## 5. Resultados de las pruebas off-board (T1–T4)

Todas las pruebas se ejecutaron mediante la herramienta `tools/eth_e3e6_checks.py`, con árbol limpio al inicio y al final, y con controles negativos rigurosos que pasan por las mismas funciones de evaluación con entradas temporales mutadas fuera del repositorio.

| Prueba | Objetivo | Resultado | Captura commiteada |
| :---: | :--- | :---: | :--- |
| **T1** | Compilación directa `clean-first` de la matriz CI de 8 configuraciones (`prj_01` a `prj_04`, `debug` y `release`). Verificación de 0 advertencias, objetos generados > 0 y frescura de imagen. Control negativo con advertencia deliberada en archivo C temporal rechazado. | **PASS** (8/8 configs, 0 avisos) | [`01_shared_drivers/08_eth/docs/eth_e3e6_t1_a249894.txt`](docs/eth_e3e6_t1_a249894.txt) |
| **T2** | Invarianza binaria de `prj_03` y `prj_04`. Compilación in-situ del control `69179d7` con mapeo de prefijos canónico `-ffile-prefix-map`. Comparación byte a byte de `.text*`, `.rodata*`, `.data*`, `.bss*` para `eth_hal.c.obj` y `ethernetif.c.obj` (8 objetos). Control negativo con byte mutado detectado y rechazado. | **PASS** (8/8 objetos idénticos) | [`01_shared_drivers/08_eth/docs/eth_e3e6_t2_a249894.txt`](docs/eth_e3e6_t2_a249894.txt) |
| **T3** | Opt-out E3 (`ETH_HAL_CUSTOM_CALLBACKS`). Compilación con la macro y comprobación de que el objeto del driver define 0 de los 6 callbacks con `nm`. Enlace parcial exitoso con callbacks de usuario. Control negativo: enlace sin la macro falla por duplicación de símbolos. | **PASS** (0/6 definidos, enlace ok) | [`01_shared_drivers/08_eth/docs/eth_e3e6_t3_a249894.txt`](docs/eth_e3e6_t3_a249894.txt) |
| **T4** | Presupuesto y cero-copia en `prj_02`. Mapa de memoria: `.eth_ram` = 24,768 B (≤ 32 KB). Alineación a 32 B de `g_rx_pool` (`0x300400c0`) en Región 6 MPU. Desensamblado de `ethernetif_receive_one` libre de `memcpy`, `pbuf_take` o `pbuf_take_at`. Controles negativos (.map mutado a 33 KB y desensamblado con `memcpy` inyectado) rechazados. | **PASS** (24,768 B, 0 llamadas copia) | [`01_shared_drivers/08_eth/docs/eth_e3e6_t4_a249894.txt`](docs/eth_e3e6_t4_a249894.txt) |

---

## 6. Pruebas de placa del auditor (T5–T6) y comandos exactos

El implementador no tiene acceso a hardware ni ejecutó herramientas de programación o monitorización contra la placa física.
El motor de prueba de carga UDP de alta tasa y la verificación byte a byte de payload fueron validados en el host mediante un servidor de eco UDP local en `127.0.0.1` (`--local-echo`), procesando 10,000 datagramas de 1472 B con 0 pérdidas y 42.68 Mbit/s.

### Comandos para la sesión de auditoría con la placa STM32H755:

El auditor ejecutará una única sesión en `prj_02` contra la placa física (`--sn 0016000C4142501220353451`, IP `192.168.1.150`):

```powershell
# Ejecución combinada de T5 y T6 con registro en captura
py tools/eth_e3e6_checks.py --test t5,t6 --sn 0016000C4142501220353451 --target-ip 192.168.1.150 -o 01_shared_drivers/08_eth/docs/eth_e3e6_t5t6_<auditor_hash>.txt
```

O de forma individual:
```powershell
# T5: Flasheo comparativo (control 69179d7 y HEAD), net_check.py, carga 10 000 datagramas de 1472 B y throughput
py tools/eth_e3e6_checks.py --test t5 --sn 0016000C4142501220353451 --target-ip 192.168.1.150 -o 01_shared_drivers/08_eth/docs/eth_e3e6_t5_<auditor_hash>.txt

# T6: Resistencia a 10 reinicios por SWD (-rst) y reconexión inmediata
py tools/eth_e3e6_checks.py --test t6 --sn 0016000C4142501220353451 --target-ip 192.168.1.150 -o 01_shared_drivers/08_eth/docs/eth_e3e6_t6_<auditor_hash>.txt
```

---

## 7. Declaración de lo no verificado

En estricto cumplimiento de las instrucciones de entrega:
- **T5 (Prueba de enlace, tráfico y throughput en placa física):** **NO VERIFICADA** por el implementador; reservada para el auditor.
- **T6 (Prueba de estrés de 10 resets SWD en placa física):** **NO VERIFICADA** por el implementador; reservada para el auditor.

---

F1 ENTREGADA PARA AUDITORIA a249894
