# Plan de implementación de `08_eth` E3–E7: cero-copia de recepción y callbacks desactivables

Oct 16, 2026 · @Hector · revisión 3

Resuelve los cuatro pendientes de `08_eth/README.md` §10.

**Las pruebas en la placa se hacen en un solo proyecto, `prj_02_eth_lwip_dualcore`**, por decisión del responsable para ahorrar recursos. Por eso **nada de lo que cambie puede alterar `prj_03` ni `prj_04`**: el driver solo cambia detrás de macros que esos proyectos no definen, y se demuestra sin placa que sus objetos compilados son idénticos a los del control.

**Reparto del trabajo:**
- **Antigravity** hace la carga pesada:
  - el código;
  - la herramienta, que incluye la prueba de throughput en la placa;
  - todas las pruebas sin placa y el informe.
- **El auditor** revisa el diff, ejecuta la herramienta sin placa una vez y hace **una sola sesión de placa** en `prj_02`, con la imagen nueva y la de control en la misma sesión.

Una fase (F1) y un cierre. Base y control: `main` en `69179d7`. Reglas para el implementador en [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md).

**Revisión 3 (2026-10-16), decisión del responsable tras la sesión de placa: E6 no se adopta.**
- **Medidas** (`docs/eth_e3e6_audit_t5t6_1150083.txt`):
  - con la carga una a una, el cero-copia de recepción rinde igual que la copia (6,19 frente a 6,28 Mbit/s);
  - con 32 en vuelo pierde 24 de 5000 tramas (`rx_nobuf` = 26) y rinde 32,8 frente a 54,1 Mbit/s;
  - la copia almacena unas 24 tramas (8 búferes DMA y 16 pbufs), y el cero-copia solo los 12 búferes DMA que caben en SRAM3;
  - no hay una medida de CPU que muestre ganancia.
- **Qué cambia:**
  - se quita el código de cero-copia: `ETH_Driver_ReleaseRxBuffer`, `g_rx_rearm_pending`, la rama de `ethernetif.c`, `LWIP_SUPPORT_CUSTOM_PBUF` y las dos definiciones de `prj_02`;
  - se conservan E3 (`ETH_HAL_CUSTOM_CALLBACKS`) y el `#ifndef` de `ETH_DRIVER_RX_BUFFER_COUNT`;
  - `prj_02` vuelve a ser idéntico al control, y T2 lo comprueba también para `prj_02`;
  - T4 y T5 dejan de ser criterios de cierre: quedan como medidas en el README;
  - no hace falta otra sesión de placa.
- **El README** documenta E6 como "medido, no adoptado", con las cifras y la causa, y deja el camino para quien lo retome (un pool más grande, o copia de respaldo cuando quedan pocos búferes).

**Revisión 2 (2026-10-16), tras la auditoría de F1:**
- **T5:** la carga "una a una" no puede vaciar el pool. Además de esa carga, hay una con 32 datagramas en vuelo. El criterio de throughput es la mediana de 3 repeticiones de cada imagen: la nueva ≥ 0,98 × el control, para absorber el ruido del PC.
- **Árbol limpio:** la herramienta puede escribir en `build/` de los proyectos (es su salida de compilación, ignorada); fuera de `build/` no puede crear nada, ni ignorado.

**Decisiones del responsable (2026-10-16): aprobadas las cuatro, con la opción A en 1–3.**

**Decisiones del plan** (ver [Decisiones abiertas](#decisiones-abiertas)):
1. Alcance: E6 (cero-copia de recepción) y E3, solo código; E4 y E7, solo documentación.
2. E6 solo en recepción; la transmisión sigue copiando.
3. Proyecto de placa: `prj_02`.
4. Documentos de proceso: se borran al cierre.

**Reglas de verificación (las de `00_gpio` F2 y `01_led`, desde el primer día):**
- cada comprobación puede fallar de verdad;
- cada control negativo pasa por la misma función que la comprobación real, sobre una entrada mutada en un directorio temporal fuera del repo;
- el control se compila dentro de la propia ejecución;
- la herramienta no escribe en el árbol (tampoco archivos ignorados);
- árbol limpio al empezar y al terminar;
- capturas UTF-8 de una sola ejecución, con argv y HEAD;
- cada cita `archivo:línea` del informe se comprueba contra HEAD antes de entregar;
- todo lo que se quiera copiar en el README, en inglés.

---

## Encargo

| ID | Hoy (`69179d7`) | Qué cambia |
| --- | --- | --- |
| **H1 (E6)** | **Recepción de `prj_02`:** `ethernetif_receive_one()` (`prj_02/CM7/App/Src/ethernetif.c:133-186`) lee la trama del driver (`ETH_Driver_ReadPacket`), la copia a un pbuf de `PBUF_POOL` (`pbuf_take_at`) y devuelve el búfer al driver (`ETH_Driver_ReleasePacket`) | **Cero-copia de recepción en `prj_02`.** La trama se entrega a lwIP como `pbuf_custom` (`LWIP_SUPPORT_CUSTOM_PBUF = 1`) que apunta al búfer DMA del driver. La función `custom_free_function` devuelve el búfer al driver cuando lwIP lo libera. Los detalles van en el Diseño. `prj_03` y `prj_04` no cambian |
| **H2 (E6)** | **El driver:**<br>• el pool de recepción es fijo (`ETH_DRIVER_RX_BUFFER_COUNT 8U`, `eth_hal.h:48`);<br>• `ETH_Driver_ReleasePacket` se llama hoy solo desde el hilo de entrada | **Lo mínimo para H1, sin efecto si no se activa:**<br>• `ETH_DRIVER_RX_BUFFER_COUNT` se puede fijar desde el proyecto (`#ifndef`), con el `_Static_assert` de 32 KB de SRAM3 (`eth_hal.c:162-164`) intacto;<br>• si `ETH_Driver_ReleasePacket` no es segura desde otro hilo de lwIP, una versión segura detrás de `ETH_HAL_RX_ZERO_COPY`, que solo define `prj_02` |
| **H3 (E3)** | Los callbacks de HAL ETH (`HAL_ETH_RxAllocateCallback`, `RxLinkCallback`, `TxFreeCallback`, `RxCpltCallback`, `TxCpltCallback`, `ErrorCallback`; `eth_hal.c:745-835`) son siempre símbolos fuertes; solo la MSP tiene `ETH_HAL_CUSTOM_MSP` (`eth_hal.c:329`) | `ETH_HAL_CUSTOM_CALLBACKS`: si el proyecto lo define, el driver no define esos callbacks y el proyecto los aporta. Nadie lo define hoy |
| **H4 (E4, E7)** | — | Solo documentación, en el cierre:<br>• **E4:** una sola instancia, que es lo que tiene el silicio; queda \"por diseño\";<br>• **E7:** `prj_eth_lwip_node` conserva su copia privada (decisión 1) |
| **H5** | — | `tools/eth_e3e6_checks.py` (nuevo): T1–T4 sin placa y T5–T6 con la placa |
| **H6** | — | `IMPLEMENTATION_REPORT.md` |

**Fuera:**
- el cero-copia de transmisión (decisión 2);
- `prj_03`, `prj_04` y `prj_eth_lwip_node` (no cambian);
- el mapa de MPU y los scripts de enlazado, salvo que el presupuesto de SRAM3 lo exija; en ese caso se para y se pregunta.

---

## Diseño de H1 (lo que audito)

1. **Configuración:** `LWIP_SUPPORT_CUSTOM_PBUF 1` en `prj_02/CM7/App/Inc/lwipopts.h`.
2. **Recepción:**
   - cada trama, de uno o más segmentos, se entrega como un `pbuf_custom`, o como una cadena si tiene varios segmentos, cuyo `payload` es el búfer DMA (`.RxArraySection`, SRAM3, no cacheable por la región 6 de la MPU);
   - no hay `memcpy` en el camino de recepción.
3. **Liberación:** `custom_free_function` llama a la liberación del driver. Tiene que ser segura desde el hilo donde lwIP libere el pbuf (`tcpip_thread` o el de la aplicación), nunca desde una ISR. El informe dice, con `archivo:línea`, desde qué contextos se llama y por qué es segura.
4. **Inanición:**
   - lwIP retiene búferes del pool mientras procesa;
   - el pool de recepción de `prj_02` sube lo necesario para que el anillo no se vacíe (por ejemplo, de 8 a 12) dentro de los 32 KB de SRAM3;
   - si no hay búfer libre, la trama se descarta y se cuenta (`rx_nobuf`), sin bloquearse.
5. **Las estructuras `pbuf_custom`** van en un pool estático propio (`LWIP_MEMPOOL_DECLARE` o un array). No van en la memoria DMA: no la necesitan.
6. **Sin cambios de comportamiento visibles:** mismos contadores `[net]`, mismo `net_check.py`, mismas trazas de arranque.

---

## Pruebas

| ID | Qué | Oráculo | Control negativo | Criterio |
| --- | --- | --- | --- | --- |
| **T1** | Los 8 configs compilados directamente (`cmake --preset X --fresh`, `cmake --build --preset X --clean-first`), con todos los avisos de su salida real contados | Código de salida, objetos > 0, líneas `warning:` | Un `.c` temporal con un aviso deliberado, compilado con las flags de un proyecto por el mismo camino, falla | 8/8 con 0 avisos |
| **T2** | **`prj_03` y `prj_04` sin cambios:** para los 4 builds (`debug` y `release`), las secciones de código y datos (`.text*`, `.rodata*`, `.data*`, `.bss*`) de `eth_hal.c.obj` y de `ethernetif.c.obj` de CM7, iguales byte a byte a las del control `69179d7`, compilado en la ejecución (`objdump -s` sin depuración) | Igualdad de secciones | La misma comparación con un byte cambiado en una copia temporal falla | Iguales en los 4 × 2 objetos |
| **T3** | **E3 en compilación:** `eth_hal.c` con `-DETH_HAL_CUSTOM_CALLBACKS` y un `.c` temporal que define los seis callbacks; con `nm`, el objeto del driver no define ninguno | `nm` y el enlace del par | Sin la macro, el mismo par da error de símbolo duplicado | Enlaza con la macro, falla sin ella |
| **T4** | **Memoria de `prj_02`:**<br>• desde el `.map`: el uso de SRAM3 (`.eth_ram`, `.RxArraySection`, descriptores) ≤ 32 KB;<br>• los búferes de recepción alineados a 32 B y dentro de la región 6 de la MPU;<br>• ningún `pbuf_take`/`memcpy` en `ethernetif_receive_one` (objdump de la función) | `.map`, `nm` y desensamblado | Un `.map` con la sección de 33 KB, y un desensamblado con una llamada a `memcpy`, fallan por las mismas funciones | Todo dentro; sin copia en recepción |
| **T5** (auditor, placa) | **`prj_02`, imagen de control (`69179d7`) y nueva, en la misma sesión:**<br>• `net_check.py`: ping 4/4, barrido UDP 1–1472 B y ráfaga 100/100;<br>• una prueba de carga en la herramienta: 10 000 datagramas UDP de 1472 B al eco, con todas las respuestas comprobadas byte a byte;<br>• `monitor.py` a la vez, con las líneas `[net]` | Respuestas y payload; contadores `rx_nobuf`, `tx_nobuf`, `dma_err` de `[net]`; throughput en Mbit/s medido por el host | La misma comprobación con un payload alterado en una copia falla | En las dos imágenes: 0 pérdidas, payload exacto y contadores a 0. En la nueva, throughput ≥ el del control. Las cifras, en la captura |
| **T6** (auditor, placa) | 10 resets por SWD (`-rst`) de la imagen nueva; tras cada uno, ping 4/4 y 100 datagramas de eco | Respuestas | — | 10/10 |

### Normas para las capturas
`01_shared_drivers/08_eth/docs/eth_e3e6_<prueba>_<hash>.txt`, de una ejecución, con argv, HEAD y el estado del árbol.

---

## Banco y seguridad

- Solo `prj_02` (`debug`, el de HEAD y el de control), con `sn=0016000C4142501220353451`.
- La red del banco es la de siempre; la configura el responsable.
- No se usa el AD2.
- No se escriben registros por SWD (solo `-rst` en T6), y no se tocan alimentación, reloj ni option bytes.
- Al terminar, la placa queda con `prj_02` `debug` de HEAD.

---

## Fases y entregables

| Fase | Entregable | Criterio de pase |
| --- | --- | --- |
| **F1** | H1–H3, H5 y H6, más las capturas de T1–T4 | Una revisión del auditor (código y herramienta), T1–T4 ejecutadas por el auditor una vez, y T5–T6 en una sesión de placa |
| **Cierre** | **Implementador:** `08_eth/README.md` (§2, §4, §9 con T1–T6 y las capturas, §9.5, §10 con E3/E6 resueltos y E4/E7 \"por diseño\", §12 historia) y `prj_02/README.md` y `ETHERNET_BRINGUP.md` §9 (el cero-copia hecho en recepción).<br>**Responsable:** la hoja de ruta, el borrado de los documentos de proceso y el merge | Revisión del auditor |

---

## Riesgos

| Riesgo | Mitigación |
| --- | --- |
| Inanición del anillo de recepción por pbufs retenidos | Pool más grande en `prj_02` (H1.4); la carga de T5 lo pone a prueba y `rx_nobuf` lo cuenta |
| Liberación desde un hilo inseguro | H1.3: contextos documentados; la carga de T5 libera desde `tcpip_thread` |
| Que `prj_03`/`prj_04` cambien sin probarse en placa | T2 exige objetos idénticos al control |
| SRAM3 se llena | El `_Static_assert` existente y T4. Si no cabe, se para y se pregunta |

---

## Decisiones abiertas

1. **Alcance:**
   - **A (por defecto):** E6 en recepción y E3 en el código; E4 (una instancia, por diseño) y E7 (la copia de `prj_eth_lwip_node` se conserva) solo en la documentación.
   - **B:** solo E3 y la documentación, sin cero-copia. Más barato, pero no aporta rendimiento.
2. **Alcance de E6:**
   - **A (por defecto):** solo recepción. La transmisión sigue copiando, porque cero-copia ahí exige que el heap de lwIP sea alcanzable por el DMA y no cacheable, y eso cambia la MPU.
   - **B:** recepción y transmisión.
3. **Proyecto de placa:**
   - **A (por defecto):** `prj_02`, el proyecto de referencia de Ethernet, sin ADC.
   - **B:** `prj_04`.
4. **Documentos de proceso:** se borran al cierre.
