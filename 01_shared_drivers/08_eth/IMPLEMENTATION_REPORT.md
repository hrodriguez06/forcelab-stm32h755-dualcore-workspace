# Informe de implementación y cierre de `08_eth` E3–E7 (Fase F1 - Cierre)

Oct 16, 2026 · @Antigravity

Entrega de cierre definitivo de la fase F1 del plan [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 3) en la rama `driver/eth-e3-e6`.

---

## 1. Resumen ejecutivo del cierre (Plan Revisión 3)

Tras la sesión en placa física de la auditoría 2 sobre el prototipo cero-copia (commit `1150083`, captura del auditor [`docs/eth_e3e6_audit_t5t6_1150083.txt`](docs/eth_e3e6_audit_t5t6_1150083.txt)), el responsable del producto determinó la **no adopción de cero-copia (E6)**:

1. **Evidencia empírica en hardware:**
   - **Carga secuencial 1-en-vuelo** (3 × 10.000 datagramas @ 1472 B UDP): Cero-copia rindió idéntico a copia (mediana 6,19 Mbit/s vs 6,28 Mbit/s, ratio 0,9859× control, 0 pérdidas).
   - **Carga en ráfaga de 32 datagramas en vuelo** (5.000 datagramas @ 1472 B UDP): El control en copia absorbió la totalidad sin pérdidas (5.000/5.000 tramas, 54,06 Mbit/s, `rx_nobuf=0`). El cero-copia sufrió una regresión de throughput del 39% (32,84 Mbit/s), perdiendo 24 de 5.000 tramas (`rx_nobuf=26`).
   - **Causa raíz:** En copia, lwIP desacopla el anillo DMA de la pila almacenando paquetes en 16 pbufs de `PBUF_POOL` y devolviendo de inmediato los 8 búferes DMA (capacidad efectiva de ~24 tramas). En cero-copia, lwIP retiene los búferes DMA durante todo el procesamiento; dado que D2 SRAM3 (32 KB) solo puede albergar 12 búferes DMA (18.432 B) junto con descriptores y Tx, una ráfaga de 32 paquetes agota los descriptores y causa descartes. No se evidenció ahorro de CPU justificable.
2. **Retirada de cero-copia de firmware y proyecto:**
   - Se eliminaron `ETH_Driver_ReleaseRxBuffer` y `g_rx_rearm_pending` de `eth_hal.h` y `eth_hal.c`.
   - Se revirtió `prj_02_eth_lwip_dualcore` (`ethernetif.c`, `lwipopts.h`, `CMakeLists.txt`) al estado idéntico del control `69179d7`.
3. **Mejoras conservadas:**
   - **E3 resuelta:** Desacoplamiento de callbacks mediante `#if !defined(ETH_HAL_CUSTOM_CALLBACKS)`.
   - **Flexibilidad de pool:** Macro de tamaño de pool Rx `#ifndef ETH_DRIVER_RX_BUFFER_COUNT` configurable por proyecto con guarda `_Static_assert` de 32 KB intacta.
4. **Invarianza binaria absoluta extendida (T2):**
   - El arnés de verificación (`tools/eth_e3e6_checks.py`) extendió la comprobación T2 a `prj_02` (`debug` y `release`), demostrando que la totalidad de los 12 objetos compilados (`eth_hal.c.obj` y `ethernetif.c.obj` en `prj_02`, `prj_03` y `prj_04`) son idénticos byte a byte al control `69179d7` compilado en la ejecución.
5. **Criterios de cierre y documentación:**
   - T4 (desensamblado cero-copia) y T5/T6 (pruebas de placa física) se retiraron de los criterios de cierre automático, quedando documentadas como referencias históricas y guías de arquitectura.
   - La suite por defecto de la herramienta ejecuta sin placa T1, T2 y T3.
   - Se actualizaron en inglés [`08_eth/README.md`](README.md) y [`prj_02/docs/ETHERNET_BRINGUP.md`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/docs/ETHERNET_BRINGUP.md).

---

## 2. Pruebas sin placa de cierre (T1–T3)

Ejecutadas íntegramente mediante `tools/eth_e3e6_checks.py` en commit `20eb735`, registradas en [`docs/eth_e3e6_t1t3_20eb735.txt`](docs/eth_e3e6_t1t3_20eb735.txt):

| Prueba | Metodología y Oráculo | Resultado | Veredicto |
| :---: | :--- | :---: | :---: |
| **T1** | Compilación `clean-first` de la matriz CI de 8 configuraciones (`prj_01` a `prj_04`, `debug` y `release`). Código de salida 0, 0 advertencias, objetos generados > 0 y frescura de binarios. Control negativo con flags de proyecto rechazado. | 8/8 configs clean-first compiladas con 0 warnings. Control negativo rechazado (`PASS`). | **PASS** |
| **T2** | Invarianza binaria absoluta de `prj_02`, `prj_03` y `prj_04`. Compilación in-situ del control `69179d7` con `-ffile-prefix-map`. Comparación profunda en los **12 objetos** de secciones ALLOC (con NOBITS), datos brutos no-debug (`objdump -s`), relocalizaciones no-debug (`objdump -r`) y símbolos (`nm -p`). Control negativo con sección mutada rechazado. | 12/12 objetos idénticos byte a byte al control `69179d7`. Control negativo rechazado (`PASS`). | **PASS** |
| **T3** | Opt-out E3 (`ETH_HAL_CUSTOM_CALLBACKS`). `nm` confirma que el driver define 0 de 6 callbacks y contiene `ETH_Driver_Init`. Enlace parcial exitoso con callbacks externos de usuario. Control negativo falla con conflicto de símbolos duplicados. | 0/6 callbacks definidos, enlace ok con callbacks de usuario. Control negativo rechazado (`PASS`). | **PASS** |
| **Árbol limpio** | Verificación estricta del árbol de trabajo al inicio y al final de la ejecución; sin creación de archivos en el repositorio. | `working tree clean (start): PASS`<br>`working tree clean (end): PASS` | **PASS** |

---

## 3. Presupuesto de memoria SRAM3 (.eth_ram)

Con la retirada del prototipo cero-copia y la reversión de `prj_02` al control, el uso de D2 SRAM3 (`0x30040000`, 32 KB / 32.768 B) queda restablecido a su valor nominal de producción:

| Componente | Configuración de Cierre (8 Rx / 4 Tx) | Ocupación | Porcentaje |
| :--- | :---: | :---: | :---: |
| **Descriptores Rx DMA (`.RxDescripSection`)** | 4 descriptores × 24 B | 96 B | 0,29 % |
| **Descriptores Tx DMA (`.TxDescripSection`)** | 4 descriptores × 24 B | 96 B | 0,29 % |
| **Búferes Rx DMA (`.RxArraySection`, `g_rx_pool`)** | 8 × 1536 B | 12.288 B | 37,50 % |
| **Búferes Tx DMA (`.eth_ram`, `g_tx_pool`)** | 4 × 1536 B | 6.144 B | 18,75 % |
| **Total sección `.eth_ram` en SRAM3** | **12 descriptores + 12 búferes** | **18.624 B** | **56,84 %** |
| **Margen libre en SRAM3** | — | **14.144 B** | **43,16 %** |

---

## 4. Actualizaciones de documentación aplicadas (en inglés)

En cumplimiento de las instrucciones de cierre, se actualizaron y commitearon en `dcf27e5` los siguientes documentos:

1. **[`01_shared_drivers/08_eth/README.md`](README.md):**
   - **Sección 2.3:** Añadida tabla de macros de configuración en tiempo de compilación documentando `ETH_DRIVER_RX_BUFFER_COUNT`, `ETH_HAL_CUSTOM_CALLBACKS`, `ETH_HAL_CUSTOM_MSP`, `ETH_DRIVER_IRQ_PRIORITY` y `ETH_USE_HARDWARE_CHECKSUM`.
   - **Sección 9.3:** Incorporada subsección de validación de E3–E7:
     * 9.3.1: Verificación off-board T1–T3 (commit `20eb735`, captura `docs/eth_e3e6_t1t3_20eb735.txt`).
     * 9.3.2: Sesión de benchmark en hardware del auditor (commit `1150083`, captura `docs/eth_e3e6_audit_t5t6_1150083.txt`) con cifras exactas de carga secuencial y en ráfaga, causa raíz del descarte de búferes y decisión de producto.
   - **Sección 9.6:** Añadido `ETH_HAL_CUSTOM_CALLBACKS` a la lista de elementos no ejercitados en hardware (comprobado off-board mediante T3).
   - **Sección 10:** Tabla actualizada marcando E4 como "By design" (única instancia impuesta por el silicio), E7 como "Deliberately kept" (copia privada de `prj_eth_lwip_node`), y E6 como "Measured and not adopted" con resumen de motivos y referencias a §9.3.2 y `ETHERNET_BRINGUP.md` §9.
   - **Sección 10.1:** E3 documentada como cerrada en `20eb735` mediante `ETH_HAL_CUSTOM_CALLBACKS`.
   - **Sección 12:** Historial ampliado con los hitos de commits `15d1d3a`, `805ed29`/`afe605c` y `20eb735`/`cb09823`. Actualizadas las instrucciones `git show` para consulta de los documentos de proceso de la revisión 3.
2. **[`02_firmware_projects/prj_02_eth_lwip_dualcore/docs/ETHERNET_BRINGUP.md`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/docs/ETHERNET_BRINGUP.md):**
   - **Sección 9:** Reescrita para documentar la evaluación y resultados de benchmark de cero-copia en hardware (octubre 2026), explicando por qué se retuvo el modo copia (desacoplamiento de turnovers y profundidad de cola en lwIP) y estableciendo directrices técnicas para futuros intentos (reubicación de Tx a SRAM1 para habilitar 16–20 búferes Rx en SRAM3, o esquema híbrido con copia de respaldo).

---

## 5. Citas `archivo:línea` contra HEAD (`dcf27e5`)

Todas las citas han sido contrastadas de manera estricta contra el árbol de trabajo actual:

### 5.1 Driver Ethernet compartido (`01_shared_drivers/08_eth`)
1. [`01_shared_drivers/08_eth/eth_hal.h:48-50`](eth_hal.h#L48-L50): Macro `ETH_DRIVER_RX_BUFFER_COUNT` configurable, default 8U con guarda `#ifndef`.
2. [`01_shared_drivers/08_eth/eth_hal.h:236-245`](eth_hal.h#L236-L245): Documentación del hook de compilación `ETH_HAL_CUSTOM_CALLBACKS`.
3. [`01_shared_drivers/08_eth/eth_hal.c:162-164`](eth_hal.c#L162-L164): `_Static_assert` de tamaño de descriptores y búferes en D2 SRAM3 (≤ 32 KB).
4. [`01_shared_drivers/08_eth/eth_hal.c:167-168`](eth_hal.c#L167-L168): `_Static_assert` de margen del pool Rx (`ETH_DRIVER_RX_BUFFER_COUNT > ETH_RX_DESC_CNT`).
5. [`01_shared_drivers/08_eth/eth_hal.c:736-833`](eth_hal.c#L736-L833): Los seis callbacks de HAL ETH condicionados a `#if !defined(ETH_HAL_CUSTOM_CALLBACKS)`.

### 5.2 Proyecto de referencia `prj_02_eth_lwip_dualcore`
1. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/CMakeLists.txt:87-92`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/CMakeLists.txt#L87-L92): Definiciones de compilación de CM7 idénticas al control `69179d7`.
2. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Inc/lwipopts.h:46-48`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Inc/lwipopts.h#L46-L48): Configuración estándar de memoria de lwIP (`PBUF_POOL_SIZE 16`, sin `LWIP_SUPPORT_CUSTOM_PBUF`).
3. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:18-23`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L18-L23): Documentación de cabecera: arquitectura de copia entre pbufs en DTCM y búferes en SRAM3.
4. [`02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c:133-185`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c#L133-L185): Implementación de `ethernetif_receive_one()` con copia mediante `pbuf_take_at` y liberación inmediata del búfer DMA vía `ETH_Driver_ReleasePacket`.

### 5.3 Arnés de verificación (`tools/eth_e3e6_checks.py`)
1. [`tools/eth_e3e6_checks.py:73-80`](../../tools/eth_e3e6_checks.py#L73-L80): `T2_CONFIGS` con los 6 entornos evaluados (`prj_02`, `prj_03` y `prj_04` en `debug` y `release`).
2. [`tools/eth_e3e6_checks.py:577-704`](../../tools/eth_e3e6_checks.py#L577-L704): Implementación de `run_t2` con comparación profunda de los 12 objetos (`eth_hal.c.obj` y `ethernetif.c.obj`) y control negativo.
3. [`tools/eth_e3e6_checks.py:721-835`](../../tools/eth_e3e6_checks.py#L721-L835): Implementación de `run_t3` verificando el opt-out E3 (`ETH_HAL_CUSTOM_CALLBACKS`) con enlace de callbacks externos y rechazo del control negativo sin la macro.
4. [`tools/eth_e3e6_checks.py:1732-1735`](../../tools/eth_e3e6_checks.py#L1732-L1735): Selección de pruebas por defecto para `--test all` ajustada a `["t1", "t2", "t3"]`.

### 5.4 Documentación técnica de referencia
1. [`01_shared_drivers/08_eth/README.md:86-95`](README.md#L86-L95): Sección 2.3: tabla de macros de compilación.
2. [`01_shared_drivers/08_eth/README.md:368-401`](README.md#L368-L401): Sección 9.3: validación de E3–E7 (pruebas T1–T3 off-board y medidas en hardware).
3. [`01_shared_drivers/08_eth/README.md:460-466`](README.md#L460-L466): Sección 10: tabla de limitaciones conocidas (E4 por diseño, E6 medida y no adoptada, E7 privada conservada).
4. [`01_shared_drivers/08_eth/README.md:468-474`](README.md#L468-L474): Sección 10.1: limitación E3 resuelta mediante opt-out.
5. [`01_shared_drivers/08_eth/README.md:520-536`](README.md#L520-L536): Sección 12: hitos de commits y comandos `git show` de documentos de proceso.
6. [`02_firmware_projects/prj_02_eth_lwip_dualcore/docs/ETHERNET_BRINGUP.md:411-441`](../../02_firmware_projects/prj_02_eth_lwip_dualcore/docs/ETHERNET_BRINGUP.md#L411-L441): Sección 9: evaluación en placa de cero-copia, causas de no adopción y directrices de diseño.

---

CIERRE ENTREGADO PARA AUDITORIA dcf27e5
