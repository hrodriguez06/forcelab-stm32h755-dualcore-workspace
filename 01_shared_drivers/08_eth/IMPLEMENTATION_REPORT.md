# Informe de implementación de `08_eth` E3–E7 (Fase F1 - Corrección 2)

Oct 16, 2026 · @Antigravity

Segunda entrega corregida de la fase F1 del plan [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 2) en la rama `driver/eth-e3-e6`, resolviendo la totalidad de los hallazgos de la segunda auditoría [`AUDIT.md`](AUDIT.md) (commit `85aa255`).

---

## 1. Estado de firmware y pruebas T1–T4 (Auditados y cerrados)

En la segunda auditoría ([`AUDIT.md`](AUDIT.md)), el auditor revisó y cerró satisfactoriamente los hallazgos de firmware y las pruebas automáticas sin placa T1–T4 (captura del auditor `eth_e3e6_audit_t1t4_1493792.txt`, veredicto `OVERALL VERDICT: PASS`):

- **F-B1 (Cerrado):** `HAL_ETH_RxAllocateCallback` marca `g_rx_rearm_pending` al agotar el pool; `ETH_Driver_ReleaseRxBuffer` devuelve si había rearme pendiente; `ethernetif_rx_pbuf_free_custom` hace `xSemaphoreGive` desde el hilo de lwIP; el hilo de entrada tiene un plazo de espera de 10 ms en modo cero-copia.
- **F-M1 (Cerrado):** Se preserva `next_seg` antes de liberar segmentos en los bucles de error de `ethernetif_receive_one`.
- **F-m1 (Cerrado):** Comentario de cabecera de `ethernetif.c` actualizado a la arquitectura cero-copia en recepción.
- **T1–T4 (Cerrados):**
  - T1: Matriz 8/8 compilada `clean-first` con 0 advertencias; control negativo con flags del proyecto rechazado.
  - T2: 8 objetos de `prj_03`/`prj_04` idénticos al control `69179d7` compilado en la ejecución, en secciones ALLOC (con NOBITS), contenidos, relocalizaciones y símbolos.
  - T3: 0 de 6 callbacks definidos en `eth_hal.c` con `ETH_HAL_CUSTOM_CALLBACKS`; sin la macro, detección de definición múltiple.
  - T4: SRAM3 en 24,768 / 32,768 B; `g_rx_pool` en `0x300400c0..0x300448c0` (alineado a 32 B y en Región 6 MPU para `debug` y `release`); 0 copias en la ruta de recepción (`ethernetif_receive_one`, `ethernetif_input_thread`, `ethernetif_rx_pbuf_free_custom`); controles negativos rechazados.

---

## 2. Resoluciones aplicadas a la herramienta (`tools/eth_e3e6_checks.py`) en commit `15d1d3a`

Conforme a la tabla de estado de T5/T6 de la auditoría 2 ([`AUDIT.md:59-70`](AUDIT.md#L59-L70)), se corrigieron todos los aspectos pendientes del arnés de verificación:

### 2.1 Hallazgo B1 (Bloqueante): Worktree de control, SHA-256 de CM4 y volcado de flasheo
- En `run_t5` ([`tools/eth_e3e6_checks.py:1371-1374`](../../tools/eth_e3e6_checks.py#L1371-L1374)), se invoca `ensure_control_worktree(control_dir, logger)` al iniciar la prueba en placa.
- Se compilan `prj_02` `debug` de control y de HEAD, y se calculan y registran en la captura los SHA-256 de **ambos núcleos** (CM7 y CM4) para ambas imágenes ([`tools/eth_e3e6_checks.py:1385-1396`](../../tools/eth_e3e6_checks.py#L1385-L1396)).
- Se comprueba explícitamente que la imagen de control y la de HEAD sean distintas: `diff_sha_ok = (ctrl_cm7_sha != head_cm7_sha)` ([`tools/eth_e3e6_checks.py:1398-1401`](../../tools/eth_e3e6_checks.py#L1398-L1401)).
- Se registra la salida íntegra de `flash.py` (stdout y stderr) en la captura con las etiquetas `[flash_ctrl]` y `[flash_head]` ([`tools/eth_e3e6_checks.py:1414-1418, 1471-1475`](../../tools/eth_e3e6_checks.py#L1414-L1418)).

### 2.2 Hallazgo B2 (Bloqueante): Robustez del monitor y análisis de contadores
- `evaluate_monitor_net_lines(log_text)` ([`tools/eth_e3e6_checks.py:1016-1058`](../../tools/eth_e3e6_checks.py#L1016-L1058)) exige al menos 1 muestra válida de `[net]` (`min_samples >= 1`), retornando `FAIL` si `net_lines_count == 0` (como en el caso de log vacío `""`).
- Se incorporó un tercer control negativo específico en T5 ([`tools/eth_e3e6_checks.py:1339-1344`](../../tools/eth_e3e6_checks.py#L1339-L1344)) comprobando que `evaluate_monitor_net_lines("")` es rechazado (`PASS`).
- El proceso `monitor.py` se ejecuta con la variable de entorno `PYTHONIOENCODING=utf-8:backslashreplace` ([`tools/eth_e3e6_checks.py:1406-1407`](../../tools/eth_e3e6_checks.py#L1406-L1407)) y la bandera `--timestamps` ([`tools/eth_e3e6_checks.py:1422, 1479`](../../tools/eth_e3e6_checks.py#L1422)), evitando caídas por caracteres no imprimibles post-reset.
- Tras la carga de tráfico, se añade una espera obligatoria de 6.0 s ([`tools/eth_e3e6_checks.py:1447-1448, 1504-1505`](../../tools/eth_e3e6_checks.py#L1447-L1448)) para garantizar que el log recoja una muestra `[net]` posterior a la finalización del tráfico (periodo nominal del firmware: 5.0 s).
- Se verifica `mon_proc.poll() is None` tanto al inicio como tras el tráfico ([`tools/eth_e3e6_checks.py:1426, 1450, 1483, 1507`](../../tools/eth_e3e6_checks.py#L1426)).
- El log completo del monitor y su hash SHA-256 se vuelcan en la captura bajo los bloques `[monitor_ctrl]` y `[monitor_head]` ([`tools/eth_e3e6_checks.py:1460-1466, 1517-1523`](../../tools/eth_e3e6_checks.py#L1460-L1466)).
- Todo el ciclo de vida del monitor está protegido por bloques `try ... finally:` ([`tools/eth_e3e6_checks.py:1424-1458, 1481-1515`](../../tools/eth_e3e6_checks.py#L1424-L1458)), garantizando la terminación del proceso incluso ante excepciones.

### 2.3 Hallazgos M2 y M3 (Mayor): Robustez en carga de tráfico y pruebas de eco local
- **Manejo seguro de prefijo:** En `run_udp_windowed_load` ([`tools/eth_e3e6_checks.py:1227-1241`](../../tools/eth_e3e6_checks.py#L1227-L1241)), la conversión del número de secuencia está encapsulada en `try: int(...) except (ValueError, UnicodeDecodeError): corrupt_count += 1`, evitando que prefijos corruptos hagan caer la herramienta.
- **Caducidad de datagramas en vuelo:** Se calcula el tiempo de envío individual por paquete (`inflight[seq] = (p, time.perf_counter())`). En cada iteración, los paquetes con antigüedad superior a `packet_timeout` se eliminan de `inflight` e incrementan `lost_count` ([`tools/eth_e3e6_checks.py:1198-1204`](../../tools/eth_e3e6_checks.py#L1198-L1204)), impidiendo que la ventana se bloquee o encoja ante pérdidas.
- **Búfer de recepción socket:** Configuración explícita de `SO_RCVBUF` a 1 MB (`1024 * 1024`) en `run_udp_sequential_load` ([`tools/eth_e3e6_checks.py:1072-1075`](../../tools/eth_e3e6_checks.py#L1072-L1075)) y `run_udp_windowed_load` ([`tools/eth_e3e6_checks.py:1173-1176`](../../tools/eth_e3e6_checks.py#L1173-L1176)).
- **Emparejamiento por secuencia en carga una a una:** `run_udp_sequential_load` ([`tools/eth_e3e6_checks.py:1105-1139`](../../tools/eth_e3e6_checks.py#L1105-L1139)) implementa un bucle de recepción con descarte de respuestas tardías (`recv_seq < seq`), emparejando exactamente `recv_seq == seq`.
- **Plazo total de seguridad:** Plazo global de 600 s en T5 (`t5_deadline = t5_start + 600.0`, [`tools/eth_e3e6_checks.py:1369-1370`](../../tools/eth_e3e6_checks.py#L1369-L1370)), y plazos de 120 s por cada ráfaga de carga.
- **Pruebas contra eco UDP local (`LocalUdpEchoServer`):**
  - Sub-prueba A: Carga limpia (10,000 secuenciales @ 1472 B a 40.76 Mbit/s, y 5,000 en ventana de 32 a 38.57 Mbit/s, 0 corruptos, 0 pérdidas).
  - Sub-prueba B: Inyección de prefijo corrupto cada 25 paquetes (`corrupt_prefix_interval=25`). Gestionada con seguridad sin excepción, registrando `corrupt=19` (`PASS`).
  - Sub-prueba C: Inyección de pérdidas cada 20 paquetes (`drop_interval=20`). Los datagramas en vuelo caducan ordenadamente registrando `lost=25` sin estancamiento (`PASS`).

### 2.4 Hallazgo T6: Confirmación de imagen previa y reintento de enlace
- Al iniciar T6 ([`tools/eth_e3e6_checks.py:1515-1530`](../../tools/eth_e3e6_checks.py#L1515-L1530)), se compila/confirma la imagen HEAD `debug`, se registran los SHA-256 de CM7 y CM4, se flashea con `flash.py` de HEAD y se vuelca la salida completa bajo `[flash_t6]`.
- Tras cada reset por SWD ([`tools/eth_e3e6_checks.py:1544-1558`](../../tools/eth_e3e6_checks.py#L1544-L1558)), se ejecuta un sondeo de pings en bucle durante un máximo de 15.0 s para esperar el re-enlace del PHY y el arranque de lwIP antes de disparar la ráfaga de `net_check --ping 4 --burst 100`.

### 2.5 Seguridad y protección con `--local-echo`
- En la CLI ([`tools/eth_e3e6_checks.py:1594-1595`](../../tools/eth_e3e6_checks.py#L1594-L1595)), si se especifica `--local-echo` y se solicita `--test t6`, el analizador aborta de inmediato con error:
  `--local-echo cannot be combined with T6 (T6 requires physical hardware reset)`.
- Si se especifica `--test all` con `--local-echo`, T6 queda automáticamente excluida ([`tools/eth_e3e6_checks.py:1598`](../../tools/eth_e3e6_checks.py#L1598)).

---

## 3. Citas `archivo:línea` contra HEAD actual (`15d1d3a`)

Todas las citas han sido contrastadas contra el árbol de trabajo actual:

### 3.1 Driver Ethernet compartido (`01_shared_drivers/08_eth`)
1. [`01_shared_drivers/08_eth/eth_hal.h:48-50`](eth_hal.h#L48-L50): Macro `ETH_DRIVER_RX_BUFFER_COUNT` configurable, default 8U.
2. [`01_shared_drivers/08_eth/eth_hal.h:172-180`](eth_hal.h#L172-L180): Prototipo de `ETH_Driver_ReleaseRxBuffer(void *buffer)` bajo guarda `ETH_HAL_RX_ZERO_COPY`.
3. [`01_shared_drivers/08_eth/eth_hal.h:246-253`](eth_hal.h#L246-L253): Hook de compilación `ETH_HAL_CUSTOM_CALLBACKS`.
4. [`01_shared_drivers/08_eth/eth_hal.c:162-164`](eth_hal.c#L162-L164): `_Static_assert` de tamaño de descriptores y búferes en D2 SRAM3 (≤ 32 KB).
5. [`01_shared_drivers/08_eth/eth_hal.c:188-190`](eth_hal.c#L188-L190): Definición de variable volátil `g_rx_rearm_pending` bajo `ETH_HAL_RX_ZERO_COPY`.
6. [`01_shared_drivers/08_eth/eth_hal.c:740-760`](eth_hal.c#L740-L760): Implementación de `ETH_Driver_ReleaseRxBuffer()` con sección crítica PRIMASK y retorno de aviso de rearme.
7. [`01_shared_drivers/08_eth/eth_hal.c:775-878`](eth_hal.c#L775-L878): Los seis callbacks condicionados a `#if !defined(ETH_HAL_CUSTOM_CALLBACKS)`.
8. [`01_shared_drivers/08_eth/eth_hal.c:796-799`](eth_hal.c#L796-L799): `HAL_ETH_RxAllocateCallback` marca `g_rx_rearm_pending = 1U` ante fallo de reserva.

### 3.2 Proyecto de referencia `prj_02_eth_lwip_dualcore`
1. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/CMakeLists.txt:91-92`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/CMakeLists.txt#L91-L92): Definición de `ETH_HAL_RX_ZERO_COPY=1` y `ETH_DRIVER_RX_BUFFER_COUNT=12U`.
2. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Inc/lwipopts.h:50`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Inc/lwipopts.h#L50): `LWIP_SUPPORT_CUSTOM_PBUF 1`.
3. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:19-22`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L19-L22): Documentación de cabecera: cero-copia en recepción hacia SRAM3.
4. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:66-72`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L66-L72): Declaración de `struct rx_custom_pbuf` y pool `RX_POOL`.
5. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:74-86`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L74-L86): `ethernetif_rx_pbuf_free_custom()` con liberación de búfer DMA y despertar condicional de `s_rx_semaphore`.
6. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:188-193, 209-214`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L188-L214): Preservación de `next_seg` antes de liberación en rutas de error de recepción.
7. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:237`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L237): Entrega de trama a la pila con `netif->input(p_head, netif)`.
8. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:292-299`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L292-L299): Hilo de entrada con espera periódica de 10 ms en modo cero-copia.
9. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:518`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L518): Inicialización de pool en `low_level_init()` con `LWIP_MEMPOOL_INIT(RX_POOL)`.

### 3.3 Arnés de verificación (`tools/eth_e3e6_checks.py`)
1. [`tools/eth_e3e6_checks.py:1016-1058`](../../tools/eth_e3e6_checks.py#L1016-L1058): Evaluador puro de líneas `[net]` de `monitor.py` (rechazo de logs vacíos y contadores no-cero).
2. [`tools/eth_e3e6_checks.py:1060-1158`](../../tools/eth_e3e6_checks.py#L1060-L1158): Carga secuencial con emparejamiento por secuencia y descarte de tardías.
3. [`tools/eth_e3e6_checks.py:1161-1255`](../../tools/eth_e3e6_checks.py#L1161-L1255): Carga en ventana de 32 datagramas en vuelo con caducidad por paquete y análisis protegido de prefijo.
4. [`tools/eth_e3e6_checks.py:1258-1306`](../../tools/eth_e3e6_checks.py#L1258-L1306): `LocalUdpEchoServer` con inyección opcional de prefijos corruptos y descarte de datagramas.
5. [`tools/eth_e3e6_checks.py:1309-1506`](../../tools/eth_e3e6_checks.py#L1309-L1506): Implementación de T5: controles negativos, sub-pruebas locales, worktree de control, SHA-256 dual CM7/CM4, volcado de flash, monitorización bajo `try/finally` y comparación de rendimiento (mediana ≥ 0.98 × control).
6. [`tools/eth_e3e6_checks.py:1508-1549`](../../tools/eth_e3e6_checks.py#L1508-L1549): Implementación de T6: confirmación y flasheo previo de imagen HEAD `debug` con SHA-256, sondeo de enlace hasta 15 s tras reset, y verificación combinada ping/burst.
7. [`tools/eth_e3e6_checks.py:1594-1598`](../../tools/eth_e3e6_checks.py#L1594-L1598): Protección CLI de `--local-echo` (prohibición de T6 y exclusión de T6 en `all`).

---

## 4. Presupuesto de memoria SRAM3 (.eth_ram)

La región SRAM3 (D2, `0x30040000`, 32 KB / 32,768 B) está configurada en la Región 6 de la MPU como Device/No-Cacheable:

| Componente | Control `69179d7` (8 Rx / 4 Tx) | HEAD `15d1d3a` en `prj_02` (12 Rx / 4 Tx) | Delta |
| :--- | :---: | :---: | :---: |
| **Descriptores Rx DMA (`.RxDescripSection`)** | 4 descriptores (0x60) = 96 B | 4 descriptores (0x60) = 96 B | 0 B |
| **Descriptores Tx DMA (`.TxDescripSection`)** | 4 descriptores (0x60) = 96 B | 4 descriptores (0x60) = 96 B | 0 B |
| **Búferes Rx DMA (`.RxArraySection`, `g_rx_pool`)** | 8 × 1536 B (0x3000) = 12,288 B | 12 × 1536 B (0x4800) = 18,432 B | +6,144 B |
| **Búferes Tx DMA (`.eth_ram`, `g_tx_pool`)** | 4 × 1536 B (0x1800) = 6,144 B | 4 × 1536 B (0x1800) = 6,144 B | 0 B |
| **Total sección `.eth_ram` en SRAM3** | **18,624 B** (0x48c0, 56.84%) | **24,768 B** (0x60c0, 75.59%) | **+6,144 B** |
| **Margen libre en SRAM3** | **14,144 B** (43.16%) | **8,000 B** (24.41%) | -6,144 B |

- **Pool de descriptores `struct rx_custom_pbuf` (`memp_memory_RX_POOL_base`):** `12 × 24 B = 288 B`, asignado fuera de SRAM3 en memoria AXI SRAM D1 (`.bss` a `0x200001c0`).
- **Alineación:** `g_rx_pool` comienza en `0x300400c0` (múltiplo exacto de 32 B) y finaliza en `0x300448c0`, completamente contenido en la Región 6 MPU (`0x30040000..0x30048000`), comprobado en T4 tanto en `debug` como en `release`.

---

## 5. Pruebas off-board (T1–T4) y prueba local T5 con capturas

Ejecutadas íntegramente mediante `tools/eth_e3e6_checks.py` con comprobación de árbol limpio estricta al inicio y al final:

| Prueba | Oráculo y metodología | Resultado | Archivo de captura |
| :---: | :--- | :---: | :--- |
| **T1** | Compilación `clean-first` de la matriz CI de 8 configuraciones (`prj_01` a `prj_04`, `debug` y `release`). Código de salida 0, 0 advertencias, objetos generados > 0 y frescura de binarios. Control negativo con flags de proyecto rechazado. | **PASS** (8/8 configs, 0 avisos) | [`docs/eth_e3e6_t1_ebf2398.txt`](docs/eth_e3e6_t1_ebf2398.txt) *(auditado en `1493792`)* |
| **T2** | Invarianza binaria absoluta de `prj_03` y `prj_04`. Compilación in-situ del control `69179d7` con `-ffile-prefix-map`. Comparación profunda en 8 objetos de `objdump -h` (46 secciones ALLOC con NOBITS), `objdump -s` (datos brutos no-debug), `objdump -r` (relocalizaciones no-debug) y `nm -p` (símbolos). Control negativo rechazado. | **PASS** (8/8 objetos idénticos) | [`docs/eth_e3e6_t2_ebf2398.txt`](docs/eth_e3e6_t2_ebf2398.txt) *(auditado en `1493792`)* |
| **T3** | Opt-out E3 (`ETH_HAL_CUSTOM_CALLBACKS`). `nm` con rc=0 confirma que el driver define 0 de 6 callbacks y contiene `ETH_Driver_Init`. Enlace parcial exitoso con callbacks de usuario. Control negativo falla con conflicto de símbolos. | **PASS** (0/6 callbacks, enlace ok) | [`docs/eth_e3e6_t3_ebf2398.txt`](docs/eth_e3e6_t3_ebf2398.txt) *(auditado en `1493792`)* |
| **T4** | Presupuesto y cero-copia en `prj_02`. Mapa de memoria: `.eth_ram` = 24,768 B (≤ 32 KB). Límites de Región 6 MPU desde `mpu_config.c`: `g_rx_pool` alineado a 32 B en `debug` y `release`. Desensamblado de ruta de recepción libre de llamadas a `memcpy`, `__aeabi_memcpy*`, `pbuf_take` o `pbuf_take_at`. Controles negativos rechazados. | **PASS** (24,768 B, 0 copias) | [`docs/eth_e3e6_t4_ebf2398.txt`](docs/eth_e3e6_t4_ebf2398.txt) *(auditado en `1493792`)* |
| **T5 (local)** | Validación fuera de placa del motor de carga UDP contra servidor de eco local en `127.0.0.1`: prueba secuencial limpia de 10,000 datagramas de 1472 B (40.76 Mbit/s, 0 pérdidas); prueba en ventana limpia de 32 datagramas en vuelo (38.57 Mbit/s, 0 pérdidas); prueba con inyección de prefijo corrupto (manejada sin `ValueError`, `corrupt=19`, `PASS`); prueba con inyección de pérdidas de datagramas (caducidad de paquetes en vuelo correcta sin estancamiento, `lost=25`, `PASS`). Controles negativos (payload mutado, contadores sintéticos de monitor no-cero y log de monitor vacío) detectados y rechazados. | **PASS** (100% oráculos cumplidos) | [`docs/eth_e3e6_t5_local_15d1d3a.txt`](docs/eth_e3e6_t5_local_15d1d3a.txt) *(generada en `15d1d3a`)* |

---

## 6. Pruebas de placa del auditor (T5–T6) e instrucciones exactas

El implementador no tiene acceso a la placa física ni ejecutó comandos contra el hardware. La prueba en placa se reserva íntegramente al auditor para su sesión con la placa Nucleo-H755ZI-Q (`--sn 0016000C4142501220353451`, IP `192.168.1.150`).

### Comandos para la sesión de auditoría con la placa:

```powershell
# Ejecución combinada de T5 y T6 con registro en captura
py tools/eth_e3e6_checks.py --test t5,t6 --sn 0016000C4142501220353451 --target-ip 192.168.1.150 -o 01_shared_drivers/08_eth/docs/eth_e3e6_t5t6_<auditor_hash>.txt
```

O de forma independiente:
```powershell
# T5: Flasheo comparativo de control (69179d7) y HEAD con registro de SHA-256 de CM7/CM4, net_check, monitor.py [net] bajo try/finally, carga secuencial y 32-in-flight (mediana de 3 runs >= 0.98 * control)
py tools/eth_e3e6_checks.py --test t5 --sn 0016000C4142501220353451 --target-ip 192.168.1.150 -o 01_shared_drivers/08_eth/docs/eth_e3e6_t5_<auditor_hash>.txt

# T6: Confirmación de imagen HEAD previa, 10 reinicios SWD (-rst) con sondeo de re-enlace del PHY hasta 15 s y net_check (ping/burst)
py tools/eth_e3e6_checks.py --test t6 --sn 0016000C4142501220353451 --target-ip 192.168.1.150 -o 01_shared_drivers/08_eth/docs/eth_e3e6_t6_<auditor_hash>.txt
```

---

## 7. Declaración explícita de lo no verificado

En estricto cumplimiento de las instrucciones de entrega:
- **T5 (Prueba de throughput y tráfico en placa física):** **NO VERIFICADA** por el implementador; reservada para el auditor.
- **T6 (Prueba de estrés de 10 resets SWD en placa física):** **NO VERIFICADA** por el implementador; reservada para el auditor.

---

F1 CORREGIDA 2 PARA AUDITORIA 15d1d3a
