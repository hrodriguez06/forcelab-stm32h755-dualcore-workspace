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

## F1, corrección (`6b7f10a`, código en `31c5187`…`bc7e64d`)

Entrega: `3497df9..6b7f10a`:
- `31c5187` driver;
- `7fb71ca` selftest;
- `493a152` host;
- `bc7e64d` herramienta sin placa;
- `25add1f` capturas;
- `51bf03e` y `6b7f10a` informe.

La marca (`bc7e64d`) es el último commit de código o herramienta.

### Qué ejecuté

| Comprobación | Resultado |
| --- | --- |
| Revisión del código | Dos revisiones línea a línea en paralelo (firmware, host y seguridad; herramienta sin placa). Lo de abajo lo comprobé yo: los controles negativos de `gpio_f2_checks.py` (l. 344-348, 420-428) y su contador de avisos (l. 112-117) |
| Compilación | `build_all.ps1 -Clean`: 8/8. `selftest-gpio` de `prj_01` y `prj_04` con `--fresh` y `--clean-first`: 0 avisos |
| Producción (T18, mía) | Las 16 imágenes de producción siguen con los mismos símbolos `GPIO_BM_*` y tamaños que el control `3df160a`, y sin handlers EXTI |
| Seguridad eléctrica | Revisada otra vez antes de la placa:<br>• `claim_one()` rechaza PE0–PE4 y PE7;<br>• PE5 y PE6 solo en sus pasos;<br>• el host solo conduce DIO8, DIO10 y DIO12;<br>• el ayudante nuevo de DigitalIn solo escucha (llama únicamente a funciones `FDwfDigitalIn*`);<br>• `ad2.py` sin cambios.<br>Apta |
| Corrida en la placa | `gpio_f2_audit_bench_6b7f10a.txt` (`prj_01/docs/`): imagen `6b7f10a`, `Rev` y build id de los dos núcleos iguales al checkout, árbol limpio. **63/63 comprobaciones del host, `FW VERDICT=PASS failures=0`, `HOST VERDICT=PASS`** |

### Resultados en la placa (`6b7f10a`)

| Prueba | Resultado |
| --- | --- |
| F1 (T0–T10, T15, T16) | Todo PASS. T9 en PF14: 1,23 ns frente a 1,21 ns, al límite (abajo) |
| T11 | En CM7 y en CM4, por la cuenta de la ISR, la del LA de flancos enviados y la del LA de respuestas: RISING 200/200/200, FALLING 200/200/200, BOTH 400/400/400. Solo evento, con el NVIC encendido: 0 ISR y 0 respuestas, con 200 flancos enviados |
| T12 | Independientes 200 y 200 (LA PE3→DIO9 200, PE4→DIO11 200); compartida 200 y 200; CM4 deshabilitado: CM7 200, CM4 0 (LA DIO11 0) |
| T13 | Sobre los 200 flancos de subida de T11: CM7 mín 0,10, p50 0,10, máx 0,22 µs; CM4 0,15, 0,16 y 0,24 µs. Controles con solo la línea enmascarada: 0 respuestas en 20 pulsos |
| T14 | (a) `PR1` 0x08 antes y 0 después, 0 ISR; control: `PR1` 0x08 y 1 ISR.<br>(b) `ERR_CONFLICT` con la foto igual; control OK; vuelta a PE OK.<br>(c) BOTH: compartida conserva `EXTICR`, `RTSR1` y `FTSR1`; la última los borra.<br>(d) 100 ISR con `ExtiTakePending` devolviendo el bit 0 en las 100; control enmascarado: 0 |
| T17 | (a) `CSIRDY` llegó a 0; `ERR_NOT_READY` con la foto igual; control `READY = 0`.<br>(b) OK, `READY` tras **76 641 ciclos** (~192 µs, más que los 50 000 de la primera entrega), con la sección retenida **140 ciclos** (0,35 µs).<br>Restaurado `CSION = 1`, `CCCSR = 0x101` |
| Tiempos del host | El `pre()` más lento, 1,87 s, frente al `SYNC_TIMEOUT` de 10 s del firmware |

### Estado de los hallazgos de la primera auditoría

| ID | Estado |
| --- | --- |
| B1 | **Parcial.** Las 18 expresiones del host para F2 casan con las cadenas del firmware y la corrida completa pasa. Falta el mecanismo pedido: el mock sigue escrito a mano y ya está desfasado (`gpio_hw_check.py:1424` frente a `gpio_selftest.c:2003`), y nada compara las expresiones con las fuentes (N1) |
| B2 | Cerrado: `EN` en la sección, la espera fuera (`gpio_bm.c:954-983`); en la placa, 76 641 ciclos hasta `READY` y 140 de sección |
| B3 | Cerrado: una captura disparada por flanco a 100 MS/s; T13 sobre los 200 flancos de T11; disparo de bajada con un ayudante que solo escucha |
| B4 | **Parcial** (R1–R3 abajo) |
| M1 | Cerrado: lectura de vuelta con `ERR_VERIFY` en `ExtiConfig`, `ExtiDisable`, `enable_syscfg_clock` y `CCCSR.EN` |
| M2 | **Parcial:**<br>• cerrados los tamaños desde el `.map` con los dos núcleos, y `nm` en las 16 imágenes con las ISR;<br>• la cabecera cubre ahora todos los `*.h`;<br>• quedan R3 y R5 |
| M3 | Casi cerrado: queda N2 |
| M4 | Cerrado |
| M5 | Cerrado |
| M6 | **No cerrado** (R4) |
| m1–m6 | Cerrados, con detalles en N3–N8 |
| m7 | Parcial: la herramienta sigue sin usar `env.ps1` (l. 29-59) |

### Hallazgos nuevos

| ID | Clase | Hallazgo | Acción |
| --- | --- | --- | --- |
| R1 | Bloqueante | **Controles negativos que no pasan por el analizador real:**<br>• el de tamaños llama a `verify_driver_sizes(..., override_data=4, override_bss=12)`, que se salta `parse_map_sizes` (`gpio_f2_checks.py:346`);<br>• el de símbolos pasa dos diccionarios escritos a mano a `compare_symbols` (l. 423-426), así que `get_gpio_bm_symbols` nunca corre;<br>• el de handlers en producción añade el nombre después de analizar `nm` (l. 235-238, 283), y el de cabecera inyecta por `extra_content` (l. 250).<br>Si `nm` diera tres campos en lugar de cuatro (l. 372), todos los tamaños valdrían 0, las 16 imágenes pasarían y los controles también | Analizadores que reciben texto. Se les pasa una copia temporal del `.map` real con una línea `.data` de `gpio_bm.c.obj` inyectada, y la salida real de `nm -S` con una línea `GPIO_BM_ExtiConfig` de más. El control tiene que FALLAR por la misma función. El handler, inyectado en una copia temporal de un `.c` del driver |
| R2 | Mayor | **Compilación de los `selftest-gpio`:**<br>• el contador de avisos solo cuenta líneas que contienen `cm4_firmware`, `cm7_firmware` o `gpio` (l. 114), así que un aviso en `main.c` no cuenta, y `prj_01` no tiene `-Werror`;<br>• no hay `--fresh`;<br>• \"objetos compilados\" es el total `[n/N]` de ninja (l. 115-116) | Contar todas las líneas `warning:`, con un control que pase una línea de aviso por la misma función. `--fresh` y luego `--clean-first`. Contar las líneas \"Building C object\" |
| R3 | Mayor | Si `git status` falla, la salida vacía cuenta como árbol limpio (l. 458-466), y lo ignorado es cualquier línea que contenga el nombre de la captura, no solo `??` | Código de salida distinto de 0 es FAIL; solo se ignora `?? <ruta exacta>` |
| R4 | Mayor | **El informe afirma cosas sin respaldo:**<br>• \"totalidad corregida\" (l. 12);<br>• \"todas las afirmaciones provienen de las capturas\" (l. 14, 45), cuando M1, M3–M5 y m1–m6 van sin `archivo:línea`;<br>• \"retención < 60 µs\" sin captura (l. 37); la tiene ahora mi corrida: 140 ciclos;<br>• \"entradas mutadas\" y \"objetos > 0\" (l. 39);<br>• \"salida de compilación\" (l. 41) y el PATH \"fijo\" (l. 52);<br>• valores del mock como \"medidos en LA\" (l. 85-104) y \"230–496 ms\" (l. 111);<br>• las pruebas de placa como \"corregidas\" en lugar de NO VERIFICADAS (l. 120).<br>**Los comandos del banco no funcionan:** `py tools/flash.py` desde la raíz (l. 130) y `run_warm_resets.py` sin `--out` ni `--title` (l. 140).<br>**Faltan secciones de §5:** las reglas del driver con `archivo:línea` y las desviaciones | Reescribir esas filas desde el código y las capturas, citando mi corrida como medida por el auditor. Comandos que funcionen: `03_firmware_templates/prj_01_template_dualcore/tools/flash.py` y los argumentos reales de `run_warm_resets.py` |
| R5 | Menor | **Herramienta sin placa:**<br>• el control de cabecera compara con una copia mutada, no con `3df160a`, y borra una declaración en vez de cambiar una firma;<br>• una línea `/* … */ int x;` borrada cuenta como comentario;<br>• no se comprueba el commit del submódulo de Cube ni que las ELF del control salgan de `3df160a`;<br>• `main()` sin `try/finally`;<br>• `--allow-dirty` puede acabar en PASS | Corregir |
| N1 | Mayor | El mecanismo de B1 falta (arriba) | Una comprobación en `gpio_f2_checks.py` que compile cada expresión `expect()` del host y la case con las cadenas de formato del firmware; o el mock construido desde ellas |
| N2 | Menor | T12 manda un segundo `F2_CONFIG` autorizado sin volver a mirar el aborto (`CM7 gpio_selftest.c:1706`) | `if (s_bench_abort) goto cleanup;` antes de cada `cm4_send(F2_CONFIG…)` |
| N3 | Menor | `sec_ok` de T17 pasa si el shim no corrió (retención 0) (CM7:2000) | Exigir que el contador del shim suba al menos 1 |
| N4 | Menor | Los bucles por flanco no miran `fw_abort` ni los códigos de retorno de DWF: con el disparo muerto, DIO8/DIO10 siguen conmutando hasta ~80 s después del aborto del firmware. Es seguro, pero inútil | Salir con `fw_abort` o tras N lecturas vencidas; comprobar los retornos de `FDwf*` |
| N5 | Menor | La ventana tras el disparo es de 4,62 µs (462 muestras), menos que el criterio de 10 µs de T13: una respuesta a 5–10 µs contaría como perdida | Al menos 1100 muestras tras el disparo |
| N6 | Menor | `f2_pins_pd` hace read-modify-write de `GPIOE->PUPDR` sin la sección (CM7:1589-1600) | `pins_input_raw(GPIOE, PE3\|PE4, …)` |
| N7 | Menor | La salida por `CYCCNT` parado en `EnableCompensationCell` solo mira si es igual a `start`: si avanza y después se para, el bucle no tiene límite (`gpio_bm.c:978-979`) | Contar las lecturas sin cambio respecto a la anterior |
| N8 | Menor | `gpio_hw_check.py` escribe la captura solo al final (l. 1617-1620) | Escribir cada línea al momento |
| N9 | Menor | Parámetro sin uso `expect_resp` (l. 879) | Quitarlo |

**El banco (para el responsable):** la compuerta de T9 en PF14 pasó por 0,02 ns (1,23 frente a 1,21), después de 1,80/1,50 en la línea base y un fallo de 1,37/1,37 en el diagnóstico. Es el margen escaso de README §10. Las dos corridas completas del cierre pueden fallar por ella; lo decido con el responsable antes de hacerlas.

### Estado

**F1 no pasa todavía, pero el firmware está verificado en la placa:** 63/63, con las tres cuentas de T11/T12, T13 sobre 200 flancos, T14 con `PR1` leído y T17 con `READY` y la sección medidos.
- **Por qué no pasa:** queda la herramienta sin placa (R1–R3), el informe (R4) y el mecanismo de B1 (N1).
- **Siguiente ronda:** R1–R5 y N1–N9, y repetir T1 de compilación, T18 y el mock sin placa. Después, las dos corridas completas del cierre y los resets de producción.
