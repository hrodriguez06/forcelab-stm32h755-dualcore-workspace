# Auditoría de `00_gpio` F2

Auditor: Claude (Claude Code), para Hector Rodriguez · Rama: `driver/gpio-f2` · Plan: revisión 2

## F1 (`cbd68a9`, código en `628eea6`…`ed4f31c`)

Entrega:
- `628eea6` driver (H1);
- `25fbe4f` herramienta sin placa (H4);
- `0a025c9` selftest (H2);
- `ed4f31c` host (H3);
- `55e9119` capturas;
- `cbd68a9` informe.

La marca del informe dice `55e9119`, que solo añade capturas; el último commit de código o herramienta es `ed4f31c`.

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| Revisión del código | Tres revisiones línea a línea en paralelo (driver; selftest, host y seguridad; herramienta sin placa). Lo de abajo lo comprobé yo en el código: la espera de `READY` dentro de la sección (`gpio_bm.c:861-890`), los controles negativos constantes de `gpio_f2_checks.py` (l. 218-219, 295-298) y las cadenas de T17 (`gpio_selftest.c:1910` frente a `gpio_hw_check.py:948`) |
| Compilación | `build_all.ps1 -Clean`: 8/8. `selftest-gpio` de `prj_01` y de `prj_04` con `--fresh` (327 pasos): 0 avisos |
| Producción sin cambios (T18, mía) | Las 16 imágenes de producción (`prj_01`–`prj_04`, `debug` y `release`, CM7 y CM4) enlazan los mismos símbolos `GPIO_BM_*`, con el mismo tamaño, que el control `3df160a` (`nm -S`). Ninguna tiene un `EXTI*_IRQHandler`. Control positivo: la imagen `selftest-gpio` tiene 12 símbolos de esos |
| `tools/ad2.py` | Sin cambios desde `3df160a` |
| Seguridad eléctrica | Revisada antes de tocar la placa:<br>• `claim_one()` sigue rechazando PE0–PE4 y PE7, y los pasos nuevos solo añaden PE5 y PE6;<br>• el host solo conduce DIO8, DIO10 y DIO12, de forma estática;<br>• ningún camino tiene dos conductores en una línea;<br>• toda excepción termina en S2.<br>Apta para el banco, salvo M3 (abajo), que rompe S4 pero no puede causar contención |
| Corrida de diagnóstico en la placa | `gpio_f2_audit_diag_cbd68a9.txt` (`prj_01/docs/`): imagen `cbd68a9`, `Rev` y build id de los dos núcleos iguales al checkout, árbol limpio. No es una de las dos corridas de cierre. Resultados abajo |

### Resultados de la corrida de diagnóstico

| Prueba | Resultado en la placa |
| --- | --- |
| F1 (T0–T10, T15, T16) | Todo PASS salvo la compuerta de T9 en PF14: 1,37 ns frente a 1,37 ns. Es el margen escaso conocido (README §10); en la línea base `963fab1` pasó con 1,80 frente a 1,50 |
| T11 | Las cuentas de la ISR, exactas: 200/200/400 en CM7 y en CM4; solo evento: 0. **El LA no cuenta ni los flancos ni las respuestas** (B3) |
| T12 | Independientes 200/200; compartida 200/200; CM4 deshabilitado: CM7 200, CM4 0 |
| T13 | 20 flancos por núcleo: CM7 mín 0,10, p50 0,10, máx 0,12 µs; CM4 0,15, 0,16 y 0,24 µs. Control enmascarado: 0 respuestas |
| T14 | (a) 0 ISR, control 1; (b) `ERR_CONFLICT` (6) con la foto igual, control OK; (c) la regla 6 se cumple; (d) 100 ISR, control 0 |
| T17 | (a) `ERR_NOT_READY` (7) sin CSI, con la foto igual, y `READY` a 0 en el control. **(b) Con CSI, la llamada devuelve `ERR_NOT_READY` tras 50 946 ciclos con `CCCSR = 0x00000001`** (`EN = 1`, `READY = 0`). Al restaurar, `CCCSR = 0x00000101`: `READY` llega, pero después de los 50 000 ciclos (125 µs) que usa la prueba |
| Final | El host esperaba otra línea de T17 y abortó por tiempo (B1); el firmware hizo DONE por tiempo de espera y pasó a S2. `FW VERDICT=FAIL failures=3` (T17, el aborto y el resumen del banco); `HOST VERDICT=FAIL`, 53/54 |

### Hallazgos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| B1 | Bloqueante | La línea de T17 no coincide:<br>• el firmware imprime \"T17 compensation cell without CSI (refused) and with CSI (ready)\" (`gpio_selftest.c:1910`);<br>• el host espera \"…ready with CSI and refused without\" (`gpio_hw_check.py:948`);<br>• el mock (l. 1107) copia la cadena del host y lo oculta.<br>La corrida completa no puede pasar | Una sola cadena. El mock construye sus líneas a partir de las del firmware, o una comprobación compara cada expresión del host con las fuentes del firmware |
| B2 | Bloqueante | **T17(b) falla en el silicio y la espera está en la sección:**<br>• `EnableCompensationCell` espera `READY` con la sección tomada (PRIMASK y HSEM 5) durante `max_cycles` (`gpio_bm.c:861-890`);<br>• con 50 000 ciclos son 125 µs, por encima de los 60 µs del hook;<br>• en la placa no llegó a `READY` en ese tiempo.<br>La regla 9 del plan era ambigua: errata mía, revisión 2 | `EN` en la sección, `SectionExit` y la espera fuera (`READY` es solo lectura). T17 mide e imprime los ciclos hasta `READY` con un límite holgado (por ejemplo 4 000 000 ciclos, 10 ms) y comprueba el bloqueo: la sección dura menos de 60 µs |
| B3 | Bloqueante | **T11 y T12 no tienen los oráculos del LA del plan:**<br>• solo se lee la cuenta del firmware, y \"host delivered 200 pulses\" no se mide (`gpio_hw_check.py:764-805`);<br>• T13 usa 20 flancos propios (l. 809, 874), no los de T11.<br>Un buffer de 4 k muestras no cubre 200 pulsos espaciados con una respuesta de ~100 ns | Una captura disparada a 100 MS/s por flanco:<br>• se cuentan el flanco enviado y su respuesta en DIO9/DIO11;<br>• da también las latencias de T13 sobre los 200 flancos de subida de T11;<br>• FALLING y BOTH necesitan disparo en flanco de bajada.<br>Se permite un ayudante de DigitalIn en el host, solo de escucha; `ad2.py` y S11 no cambian |
| B4 | Bloqueante | **`gpio_f2_checks.py`: ninguna comprobación de T18 puede fallar de verdad:**<br>• los controles negativos son constantes o literales que no pasan por la comprobación real (l. 142-151, 186-188, 218-219, 295-298);<br>• \"0 avisos\" sale de una compilación incremental que no hizo nada (l. 100; `prj_01` no tiene `-Werror`);<br>• `build_all.ps1` corre sin `-Clean` (l. 88) | Funciones puras (diff de cabeceras, comparación de símbolos y tamaños, compuerta de tamaños) llamadas con la entrada real y con una entrada mutada temporal. `--fresh` y `--clean-first`, con un número de objetos compilados mayor que 0 en la captura. `-Clean` |
| M1 | Mayor | El driver no lee de vuelta lo que escribe (C12): `ExtiConfig` y `ExtiDisable` devuelven OK sin comprobar `EXTICR`, `RTSR1`/`FTSR1` ni `IMR`/`EMR` (c:707-708, 771-772), y `enable_syscfg_clock` no comprueba el bit (c:573-578). Las funciones de F1 sí lo hacen (`ERR_VERIFY`) | Lectura de vuelta en la sección y `GPIO_BM_ERR_VERIFY` si no coincide |
| M2 | Mayor | `gpio_f2_checks.py`:<br>• un árbol sucio no hace fallar la ejecución (l. 321-330);<br>• los tamaños salen de `size` sobre los `.obj` del selftest, no del `.map`, y pasan sin comprobar nada si la salida es corta (l. 198-215);<br>• las ISR del selftest no se buscan en producción, y un `nm` que falla da `{} == {}` (l. 231-238);<br>• la cabecera: solo `gpio_bm.h`, ignorando comentarios (l. 127, 136);<br>• el control solo comprueba su revisión, no que esté limpio, y el submódulo de Cube es una unión al árbol principal sin decirlo;<br>• la captura no lleva argv, HEAD ni la salida de compilación | Corregir cada punto; una captura nueva con argv, HEAD, árbol y salida de compilación |
| M3 | Mayor | CM4 pone PE6 como salida sin `bench_claim()` (`CM4/App/Src/gpio_selftest.c:150`), y tras un aborto CM7 sigue mandando `F2_CONFIG` (T12, T14(c)), así que PE6 vuelve a ser salida después de `bench_safe()`. El comentario de CM7:204 (\"CPU2 never configures GPIOA, GPIOE\") es falso. No hay contención (el AD2 nunca conduce DIO11), pero rompe S2/S4 | CM7 comprueba `bench_claim(GPIOE, PE6)` y `!s_bench_abort` antes de cada `F2_CONFIG`; CM4 lo rechaza sin el permiso de CM7; el comentario, corregido |
| M4 | Mayor | **Controles negativos que no aíslan lo que prueban:**<br>• T11 \"solo evento\" no enciende el NVIC (CM7:1596-1601, CM4:152), así que da 0 aunque el driver ponga `IMR` por error;<br>• T13: en CM7 el NVIC se apaga antes de `ExtiDisable` (CM7:1721-1722), y en CM4 PE6 vuelve a entrada antes del control (CM4:174) | NVIC encendido y pin de respuesta en salida en los controles; solo se enmascara la línea EXTI |
| M5 | Mayor | **T14 tiene menos oráculos que el plan:**<br>• (a) no lee `PR1` directamente (1 tras el flanco, 0 tras `ExtiConfig`);<br>• (c) solo mira `EXTICR` y `RTSR1`, ni `FTSR1` ni las máscaras de los dos núcleos;<br>• (d) no comprueba que `ExtiTakePending` devuelva el bit 0 (`(void)pend`, CM7:131, 140);<br>• (b) no vuelve a enrutar la línea a PE.<br>Además, el host solo ve el veredicto global de T14 | Añadirlos, con un `trigger` FALLING o BOTH en (c), y cada subprueba como `[host] CHECK` |
| M6 | Mayor | El informe afirma cosas sin respaldo:<br>• \"-Clean\" (l. 15, 55);<br>• capturas separadas de `t1` y `t18` que no existen (l. 54, 61);<br>• \"0 errores y 0 advertencias\" (l. 56);<br>• \"la herramienta detectó\" con controles constantes (l. 64-70);<br>• \"el mapa de memoria\" cuando es `size` sobre `.obj` (l. 67);<br>• \"ningún símbolo se filtra en producción\", sin comprobar (l. 69);<br>• \"HOST VERDICT=FAIL\" en el mock, que dice PASS (l. 84);<br>• \"no existen desviaciones\" (l. 156) | Corregir; cada afirmación, de una captura. Puede citar mis resultados de esta sección como medidos por el auditor |
| m1 | Menor | `SYSCFGEN` se escribe antes de la sección (c:603, 725, 858), así que `ERR_BUSY` y `ERR_CONFLICT` no son \"sin escribir nada\". Es inevitable | Decirlo en la cabecera |
| m2 | Menor | Citas de RM0399 mal: `RTSR1`/`FTSR1` son §21.6.1/§21.6.2; `SWIER1` §21.5.5 y §21.6.3; `PR1` §21.6.21; `IMR`/`EMR` §21.6.19/§21.6.20 (h:316, 370; c:644, 663, 670, 778, 810) | Corregir |
| m3 | Menor | El comentario de `ExtiSoftwareTrigger` dice que hace falta `RTSR1`; vale `RTSR1` o `FTSR1` (errata mía en la regla 8, revisión 2) | Corregir |
| m4 | Menor | Un núcleo puede cambiar el `trigger` de una línea que el otro tiene desenmascarada (c:622-661); el plan lo permite | Documentarlo en la cabecera |
| m5 | Menor | Sin `#error` si `DUAL_CORE` está definido y no lo está `CORE_CM4` ni `CORE_CM7`; un segundo `ExtiDisable` de una línea ya liberada devuelve `ERR_CONFLICT` | `#error`; documentar lo segundo |
| m6 | Menor | **Selftest y host:**<br>• la foto de T1 no incluye `PR1`/`C2PR1`, `SWIER1` ni `APB4ENR`, y `gst_photo_take` pone `SYSCFGEN` él mismo (common.c:78-84);<br>• PE3/PE4 sin pull mientras la línea está armada;<br>• T17 no dice si las esperas de `CSIRDY` se agotaron;<br>• el control de T13 del mock lo pone el propio código de producción (`hasattr`, l. 845, 862);<br>• sin margen frente al `SYNC_TIMEOUT` de 10 s del firmware | Corregir: pull-down en PE3/PE4 durante los pasos de F2, el tiempo de cada `pre()` en la captura y un mock independiente |
| m7 | Menor | **Herramienta e informe:**<br>• la marca (`55e9119`);<br>• el nombre `prj_02_freertos_dualcore` (es `prj_02_eth_lwip_dualcore`);<br>• la fecha del informe;<br>• el PATH de la herramienta, que no usa `env.ps1`;<br>• la captura, que se escribe solo al final | Corregir |

**Observación del banco, no de F2:** la compuerta de T9 en PF14 pasó en la línea base (`963fab1`) y falló aquí por 0,00 ns. Es el margen escaso que documenta README §10. Puede bloquear las dos corridas completas del cierre; lo trataré con el responsable antes de ellas.

### Estado

**F1 no pasa.**
- **Lo que funciona en la placa:** el driver hace lo que debe en el silicio para T11–T14 y T17(a), con latencias de 0,10–0,24 µs. La producción no cambia.
- **Por qué no pasa:**
  - B1 impide cualquier corrida completa;
  - B2 es un fallo real en el silicio y del diseño (errata mía en la regla 9);
  - B3 y B4 dejan pruebas sin oráculos independientes y controles que no pueden fallar;
  - quedan M1–M6.
- **Siguiente ronda:** corregir B1–B4, M1–M6 y los menores, y repetir T1 de compilación, T18 y el mock sin placa. Después repetiré la corrida en la placa.
