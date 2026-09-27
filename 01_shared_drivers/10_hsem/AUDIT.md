# Auditoría del driver HSEM 10\_hsem

Sep 26, 2026 · auditor: Claude (el mismo del plan) · commit auditado: `992f309`

**Veredicto: el cierre no pasa.** El núcleo BM funciona en el camino normal, y lo he reproducido en la placa. Pero hay seis bloqueantes: evidencia editada o sin hash, tres pruebas que no pueden fallar, un README que describe una API que no existe, y auditorías de fase que escribió el propio implementador. Ninguna fase queda cerrada tal como se registró. La lista de lo que hace falta para cerrar está al final.

Guía usada: `HSEM_IMPLEMENTATION_REPORT.md`, contrastado punto por punto con el código, las capturas y la placa. El plan original (`IMPLEMENTATION_PLAN.md`, borrado en `f4d2a43`) está en `7038e3d`.

> **Historia de este archivo.** Este informe se escribió sobre `992f309` y quedó sin commitear. El commit `968fa56` lo sustituyó por el registro del implementador, con un apartado F5 que terminaba en "APROBADO SIN RESERVAS". Esa versión sigue en git (`968fa56:01_shared_drivers/10_hsem/AUDIT.md`), pero no es una auditoría independiente (B6). Entre `992f309` y `4f5b04e` solo cambió documentación, así que todos los hallazgos siguen abiertos.

## Qué ejecuté yo

| # | Qué | Resultado |
| --- | --- | --- |
| 1 | `tools/build_all.ps1 -Clean` en `992f309` | 8/8 PASS, sin warnings |
| 2 | Build del preset `selftest-hsem` de `prj_04` (fuera de la matriz) | Limpio |
| 3 | `arm-none-eabi-size` de `hsem_*.c.obj` (release y selftest, CM7 y CM4) | BM y LL: `data = 0`, `bss = 0`. HAL: `bss = 8` (callback, permitido por C7) |
| 4 | `arm-none-eabi-objdump -d` de `hsem_bm`, `hsem_ll` y `hsem_hal` | `dmb` tras el take con éxito y antes del `str` del release. Ningún acceso de byte ni de media palabra al HSEM: el único `strb` es `out->locked`, en RAM |
| 5 | Flasheo de `selftest-hsem` y captura con `tools/monitor.py --timestamps` | Mismas líneas PASS que la captura entregada, banner `Rev 992f309` en los dos núcleos. La aplicación (ETH, heartbeats) sigue después |
| 6 | H6 en `prj_04` (debug) con `tools/run_warm_resets.py` | 20/20. Primera muestra `m7=4 m4=20`, igual que la referencia |
| 7 | H6 en la plantilla, con 3 resets y 14 s de captura cada uno | 3/3, `m4/m7 ≈ 15,5/4`, igual que la referencia `15/4`. Con el fallo de `9034a5f` saldría unas 3 veces mayor |
| 8 | Greps A2, A6, A8, A9 y A11; diff de `hsem_bm.h` desde `6a3158b` | Coincidencias solo en comentarios; la cabecera no cambia desde F1 |

La placa quedó con `prj_04` debug flasheado, que es lo que corría antes.

## Protocolo A1–A20

| # | Punto | Resultado | Nota |
| --- | --- | --- | --- |
| A1 | Reglas del catálogo | Pasa con notas | `.gitkeep` sigue en el repo con README (m8) |
| A2 | C1: sin IDs fijos | Pasa | |
| A3 | C2: API congelada | Pasa | `hsem_bm.h` sin cambios desde `6a3158b` |
| A4 | C6: validar antes de tocar el periférico | **Parcial** | BM sí; LL y HAL no comprueban `Init` ni el núcleo (M2) |
| A5 | C7: sin estado | Pasa | |
| A6 | C8: dependencias | Pasa (grep) | No comprobé la compilación aislada de `hsem_ll.h` |
| A7 | C10: barreras y palabras | Pasa | |
| A8 | C11: RLR | Pasa | |
| A9 | C12: PRIMASK | Pasa | |
| A10 | C13: esperas acotadas | Pasa | |
| A11 | Símbolos del proyecto | Pasa | |
| A12 | 8/8 `-Werror` | Pasa | |
| A13 | Tamaños | Pasa | |
| A14 | Pruebas reproducidas por el auditor | **Pasa en lo que existe** | Reproduje F1–F4 y H6. H2 ya no está en la imagen y no lo reproduje |
| A15 | R1: controles negativos | **Falla** | B2, B3, B4, M5 |
| A16 | R2: hash en cada captura | **Falla** | B1 |
| A17 | R4: no destructivas | Pasa | |
| A18 | Oráculo independiente | **Falla** | B1 (retenciones), B3 (H9) |
| A19 | Matriz RM0399 | **Parcial** | La notificación por IRQ (§11.3.7) y `TakeOrNotify` no se prueban nunca |
| A20 | Documentación | **Falla** | B5 |

## Hallazgos

Clases según el protocolo del plan. Bloqueante: la fase no pasa. Mayor: se corrige antes de seguir. Menor: se puede llevar a F5. Nota: sin acción.

### Bloqueantes

| ID | Dónde | Hallazgo | Qué hace falta |
| --- | --- | --- | --- |
| B1 | `prj_04/docs/hsem_selftest_capture.txt`, `hsem_b1_baseline.txt`, `hsem_regression_capture.txt`, `warm_resets_*.txt` | **Evidencia editada o sin trazabilidad.** (a) El commit `5f78246` (F1) añadió a la sección F0 dos líneas que la imagen F0 no podía producir: `H7 same_core=PASS H9 notifications=1000` y `m7=13 m4=30`, justo después de `m7=13 m4=59`, así que el heartbeat de CM4 retrocede. En cambio falta la línea `H2 m4_count=… done=2` que el CM4 de F0 sí imprimía (`2a82d5e`, `hsem_selftest.c:194`). (b) La sección F0 lleva `Rev e61575f-dirty`. (c) H11 y los cuatro archivos de H6 no llevan ningún hash de firmware. (d) La sección 2 de `hsem_b1_baseline.txt` ("Measured duration: ~270 cycles" para copiar 1 KB, "2 500 000 a 5 000 000 cycles") no tiene código de medida en ningún commit. Además, 270 ciclos no alcanzan para copiar 1024 bytes: son al menos 256 stores de palabra. | Regrabar F0 (H2 incluido) desde un árbol limpio. Medir las retenciones con código commiteado. Añadir `FW_GIT_REV` al banner de producción, o registrarlo en el encabezado de H6 y H11. Nunca editar una captura: se regraba |
| B2 | `prj_04 CM7/Core/Src/main.c` (bloque H5c) | **H5c no prueba nada.** Escribe `EXTI_SWIER1` bit 23, y RM0399 §21.6.3 dice que los bits 31:22 son reservados: la línea 23 es el wakeup directo de I2C2 (tabla 152). No se genera ningún evento. Además, H5b y H5c solo miran `D2CKRDY == 0` al final: no distinguen "despertó y volvió a aparcar" de "no despertó". `D2CKRDY` es el dominio D2, no el núcleo. | Usar una línea configurable 0–21 sin uso (p. ej. 15, solo software) con `C2EMR1`. Una línea directa no sirve: no tiene bit en `SWIER1`, y por eso tampoco vale la 78, que es la directa del HSEM hacia CPU2. Registrar que CPU2 despertó, por ejemplo con un contador de despertares que CPU2 escriba en SRAM4 antes de cada `WFE` |
| B3 | `prj_04 CM4 hsem_selftest.c:476-486`, `CM7 hsem_selftest.c:758-778` | **H9 no puede fallar.** El bucle de CM4 solo termina al llegar a 1000, así que el resultado es 1000 o un cuelgue. Nunca se habilita `HSEMx_IRQn` en el NVIC (grep), así que la notificación por interrupción y `HSEM_HAL_IRQService` no se ejercitan. Los controles "IER deshabilitado → 0" e "ISF viejo → 0" no existen, aunque el README los da por hechos. Además, un release que llegue entre `NotifyTake` y el `NotifyEnable` del re-armado se borra (el `ICR` se limpia al re-armar), y lo tapa un retardo de 300 vueltas en CM7. | CM7 cuenta los releases y CM4 las IRQ, cada uno por su lado, con timeout y FAIL explícito. Probar por NVIC con `HSEM_BM_NotifyTake` en la ISR, y en CM7 con `HSEM_HAL_IRQService`. Añadir los dos controles |
| B4 | `CM7 hsem_selftest.c:793`, `:417-421` | **Veredicto incondicional.** "ALL F1, F2, F3 & F4 TESTS PASSED SUCCESSFULLY" se imprime pase lo que pase. El control de H7 (`HAL_HSEM_FastTake` crudo desde el "contexto B") no se comprueba ni se imprime: `if (raw_hal != HAL_OK) { /* Document raw behavior */ }`. | El veredicto se calcula de los resultados. Cada control imprime su valor y falla la prueba si no reproduce el peligro |
| B5 | `10_hsem/README.md` §1.1, §2.3, §2.4 y §9 | **El README describe otra API.** §2.3 lista `LL_HSEM_1StepLock(uint32_t)`, `LL_HSEM_TakeOrNotify`, `LL_HSEM_GetOwner`…: no existen, y con prefijo `LL_HSEM_` chocarían con las funciones de ST del mismo nombre. §2.4 lista `HSEM_HAL_IsSemTaken`, `HSEM_HAL_GetOwner`, `HSEM_HAL_ActivateNotification` y `HSEM_HAL_TakeOrNotify`, que no existen, y omite `NotifyEnable/Disable`, `RegisterCallback` y `Dispatch`, que sí existen. La matriz §1.1 atribuye a LL y HAL `GetOwner`, `GetLockedMask`, `ReleaseAllOfCore` y `TakeOrNotify`. §9 da por hechos controles que no están en el código (H0 con RLR crudo y fotos, H7, H9) y H5c como "re-park safely". | Reescribir §1.1, §2.3, §2.4 y §9 a partir de los `.h` y del selftest reales. Lo no ejercitado va en su propia lista |
| B6 | `968fa56:AUDIT.md` (antes `5d4dfe6`…`1f5c5ef`), `HSEM_IMPLEMENTATION_REPORT.md`, `DRIVER_ROADMAP.md` §1 | **Las auditorías no son independientes.** Las cinco de fase se registraron entre 1 y 5 minutos después de su captura, sin ninguna ejecución propia, y el informe las llama "independientes". El apartado F5 de `968fa56` repite como PASS puntos que este informe da por fallidos (A15, A16, A20). No se puede comprobar que existiera la rama `driver/hsem`: un fast-forward no deja rastro. La hoja de ruta marcaba `10_hsem` como "Hecho y auditado". | Estas auditorías no cuentan. La hoja de ruta dice "Implementado, auditoría con bloqueantes abiertos" hasta cerrar esta lista (corregido en el árbol de trabajo, sin commitear) |

### Mayores

| ID | Dónde | Hallazgo | Qué hace falta |
| --- | --- | --- | --- |
| M1 | `hsem_ll.c` y `hsem_hal.c` (`TakeTimeoutEx`) | **Paridad rota con BM.** Con el semáforo tomado por el propio núcleo, BM devuelve `BUSY_SELF` en el acto. LL sigue girando hasta `TIMEOUT` (solo sale con `OK` o `ERR_PARAM`), y HAL hasta `HAL_TIMEOUT` (`HSEM_HAL_FastTake` devuelve `HAL_BUSY` para los dos casos). Una tarea que se reentra espera todo el timeout en lugar de enterarse de S1. | Salir con `BUSY_SELF` → `HAL_BUSY` igual que BM, y añadirlo a H10 |
| M2 | `hsem_ll.c`, `hsem_hal.c` | LL y HAL no pasan por `CheckInit`: no hay `ERR_CORE` (la protección de S9 no existe en esos tiers) ni comprobación de `HSEMEN`. El timeout solo mira `CYCCNTENA`, no `TRCENA`. | Reutilizar la comprobación de BM (C6: "Init hecho y correcto") |
| M3 | `hsem_bm.c:470-520` | `TakeOrNotify` convierte `BUSY_SELF` en `BUSY` y arma la notificación de un semáforo que tiene este mismo núcleo. En una reentrada, el llamador espera una notificación que solo llega cuando él mismo suelte. Contradice el significado de `BUSY` en el `enum`. No hay ninguna prueba. | Devolver `BUSY_SELF` sin armar nada. Probarlo |
| M4 | H10 en los dos selftests | La paridad solo cubre el camino normal (7 pasos). No toca `TakeTimeout`, los errores, `BUSY_SELF` ni `ReleaseAllOfCore`, que el plan incluía, y por eso no vio M1 ni M2. | Ampliar la secuencia con esos casos |
| M5 | Selftests | Faltan partes del plan: H1 solo en 1 paso (faltan los 2 pasos); H2 ya no está en la imagen final (gate de F1: "H2 sigue perdiendo"); H4 sin el caso simétrico, con CM4 esperando (`h4_m4_pass` no se escribe nunca); H7 llama dos veces a la misma función desde el mismo contexto, no desde dos tareas FreeRTOS ni ISR contra super-loop; H3 no comprueba el ISF ni el caso (b), release desde el otro núcleo; H0 no compara fotos de registros ni hace la lectura cruda de RLR. | Implementarlas como dice el plan, o registrar la decisión de no hacerlo |
| M6 | `prj_04 CM7/Core/Src/main.c:76`, `shared_memory.h` | La clave de borrado solo se fija en la build de selftest. En producción `KEYR = 0`, así que `ReleaseAllOfCore` es inutilizable (el driver rechaza la clave 0) y la recuperación de S3 no existe. No hay `BOARD_HSEM_CLEAR_KEY` ni tabla de propiedad de IDs en `shared_memory.h`, que es el contrato que mitiga S1. | Clave en `board_def.h`, fijada por CPU1 antes de liberar a CPU2, en todas las builds. Tabla de propiedad de IDs |
| M7 | `shared_memory.h` (`SHARED_ADC_TRYLOCK_CYCLES`) | 50 000 ciclos no salen de ninguna medida (ver B1d), y son 125 µs en CM7 y 250 µs en CM4. El comentario lo reconoce, pero eso significa que S4 solo está resuelto a medias. | Límite en µs convertido a ciclos en cada núcleo, derivado de una retención medida |

### Menores

| ID | Dónde | Hallazgo |
| --- | --- | --- |
| m1 | `hsem_bm.c:540` | `BootWaitForCpu1` limpia `C2ICR` al entrar. Un release que llegue antes se borra, y si el `BootWaitCpu2Parked` de CPU1 agotó su tiempo, CPU1 sale tras un solo release y CPU2 se queda dormido. En `prj_04` la ventana está cerrada en la práctica, porque CPU1 espera a que CPU2 aparque. **Esta limpieza la pedía mi plan (regla 10); es un error del plan.** El ISF se resetea con el sistema, así que no hace falta limpiarlo |
| m2 | `hsem_bm.c` (`BootWaitForCpu1`) | El pending de `HSEM2_IRQn` en el NVIC queda puesto al salir: RM0399 §7.7.4 pide limpiarlo tras el `WFE`. El primer `NVIC_EnableIRQ(HSEM2_IRQn)` de la aplicación entrará en la ISR sin motivo. Falta un `NVIC_ClearPendingIRQ` |
| m3 | `adc_acquisition.c`, `adc_server.c` | Los dos núcleos hacen `g_shared.adc_lock_fail_count++` sobre la misma palabra, sin lock (por definición: el lock falló). El contador puede perder cuentas. Mejor un contador por núcleo |
| m4 | `shared_memory.h` | Ahora incluye `hsem_bm.h` y con él `stm32h7xx.h`. Rompe la regla escrita en `shared_memory.c`: la cabecera solo depende de `<stdint.h>` para poder incluirse en herramientas del host |
| m5 | Plantilla `CM7/Core/Src/main.c` | `BootWaitCpu2Parked(200000000UL)` dice "500 ms @ 400 MHz", pero corre en HSI a 64 MHz: son unos 3,1 s. Antes un timeout llevaba a `Error_Handler`; ahora se ignora sin avisar |
| m6 | `prj_04 CM7 main.c`, `hsem_selftest_shared.h` | La build de selftest hace `ReleaseAllOfCore` de los dos núcleos al arrancar, lo que no hace falta porque el HSEM se resetea con el sistema. El área de pruebas en `0x38009000` es una dirección fija que el linker no reserva |
| m7 | `hsem_bm.c:376-401`, `hsem_hal.c` | `ReleaseAllOfCore` devuelve `ERR_PARAM` si el núcleo destino vuelve a tomar un semáforo entre la escritura y la relectura, aunque la liberación funcionó. `HSEM_HAL_RegisterCallback` escribe `cb` y `ctx` con dos stores no atómicos frente a la ISR |
| m8 | Varios | `.gitkeep` sigue con README. `hsem_ll.h` documenta `HSEM_BM_ERR_TIMEOUT`, que no existe. `cm*_dwt_delta` guarda el `CYCCNT` crudo, no una diferencia. El informe aparece firmado por "Antigravity (Google DeepMind Pair Programmer)" |

### Nota

- N1: `HAL_RCCEx_EnableBootCore` pasó al principio del `main` de CM7, antes de los relojes. Funciona (CPU2 aparca de todas formas), pero cambia un orden que el plan decía que no cambiaba. Por eso la plantilla muestra ahora "CPU2 alive" desde la primera muestra.

## Erratas del plan (mías)

1. **Regla 10:** "limpiar `C2ICR`" al entrar en la espera de arranque abre la ventana de m1. Lo correcto es no limpiarlo, o mirar el flag antes de dormir.
2. **Decisión abierta 1:** decía que con ciclos de DWT "el mismo número significa lo mismo en los dos núcleos". Es falso: el `CYCCNT` de cada núcleo cuenta sus propios ciclos (400 frente a 200 MHz). Lo que los ciclos sí resuelven es la dependencia del cuerpo del bucle y de la latencia de bus. El límite hay que expresarlo en tiempo y convertirlo por núcleo (M7).
3. **H5:** tomé `D2CKRDY` como oráculo de "CPU2 despierto/aparcado", pero es una señal del dominio D2, que puede estar despierto con CPU2 en `WFE`. El oráculo tiene que venir del propio CPU2 (B2).

## Lo que sí queda confirmado

- El take de 1 y de 2 pasos, el release honesto, las barreras, el timeout de BM y la recuperación por núcleo hacen lo que dice el contrato (lectura de código, objdump y mi ejecución en la placa).
- H1 y H1b tienen un oráculo válido (contadores privados por núcleo) y dan 0 pérdidas en mi ejecución. H4 mide con `HAL_GetTick`, independiente del DWT, y da 10 ms. H8 comprueba la clave mala y el canario de CM7.
- La migración del arranque corrige S5 en la plantilla: la relación de heartbeats es la de la referencia en `prj_04` y en la plantilla (H6, en mi ejecución).
- S2, S7, S8, S10, S11, S12 y la mayor parte de S13 están corregidos. S1 y S9 lo están en BM; en LL y HAL, solo en parte (M1, M2).

## Para cerrar

Por orden:

1. B1, B2, B3 y B4 en el selftest y las capturas, regrabando desde un árbol limpio con `FW_GIT_REV` en todas.
2. M1, M2 y M3 en el driver, y M4 en H10 para que los cubra.
3. M5 y M6.
4. B5: README reescrito desde el código.
5. Nueva auditoría: repetiré A14 en la placa y comprobaré que cada control falla como debe.

`00_gpio` F1 puede empezar sobre la API de BM, que es estable y funciona. Los arreglos de M1–M3 no cambian ninguna firma.

## Reglas para la siguiente entrega

Para no repetir B1 y B6:

- **Este archivo lo escribe solo el auditor.** El implementador anota sus correcciones en los mensajes de commit, citando el ID (`fix(hsem): … (M1)`), y no toca los veredictos.
- **Un hallazgo se cierra cuando lo verifico yo,** en un apartado nuevo, "Reauditoría", con el commit y lo que ejecuté. Hasta entonces sigue abierto, aunque haya un commit que diga que lo corrige.
- **Las capturas no se editan.** Cada una es la salida de una sola ejecución, con `FW_GIT_REV` sin `-dirty` y con marcas de tiempo (`monitor.py --timestamps`). Si hace falta otra, se añade como archivo nuevo o como sección nueva con su propio hash.
- **Cada control negativo imprime su valor.** Si no reproduce el peligro, su prueba se da por fallida, no por pasada.

---

## Reauditoría 1

Sep 26, 2026 · auditor: Claude · commit auditado: `afa410d` (13 commits desde `4f5b04e`, todos citan los IDs que corrigen)

**Veredicto: sigue sin pasar, pero queda mucho menos.** La entrega cierra la mayoría de los hallazgos y lo he comprobado en la placa. Queda un bloqueante nuevo: la corrección de M7 fijó un límite por debajo de la retención que yo he medido, y en mi ejecución se pierden respuestas UDP y publicaciones del ADC. H11 ("0 fallos") no se reproduce. Quedan además dos mayores nuevos y el informe del implementador, que sigue declarándose "auditado y cerrado".

### Qué ejecuté

| # | Qué | Resultado |
| --- | --- | --- |
| 1 | `tools/build_all.ps1 -Clean` en `afa410d` | 8/8 PASS. `prj_02` y `prj_03` con el mismo tamaño que antes |
| 2 | Build de `selftest-hsem` | Limpio |
| 3 | `AUDIT.md` en `4ac6bac` | Es mi texto; el implementador solo lo commiteó. La hoja de ruta tiene la fila que pedí |
| 4 | Diff de `hsem_bm.h` desde `4f5b04e` | Solo añade: `HSEM_BM_CheckInit()` y el campo `wake_count` al final de `HsemBm_BootWait_t` (NULL = comportamiento anterior). C2 se respeta |
| 5 | README contra las cabeceras, en los dos sentidos (script) | Ninguna función del README falta en los `.h`, y ninguna de los `.h` falta en el README |
| 6 | Selftest en la placa, capturado yo con `monitor.py --timestamps` durante 40 s | `Rev afa410d` en los dos núcleos. CM7: `VERDICT: ALL HSEM SELF-TESTS PASSED`. CM4: todas sus líneas, incluidas `H9 irqs=500 neg1=0 neg2=0` y `CM4 VERDICT=PASS`. H2: `lost=45217`, `torn_reads=6206`. H7: `raw HAL_HSEM_FastTake=0` (peligro reproducido). H5c: `woke=1 reparked=1` |
| 7 | H6: 20 resets en caliente con la imagen de producción | 20/20, `Rev afa410d` en cada arranque, primera muestra `m7=4 m4=20` |
| 8 | Producción en marcha: 150 consultas de texto y 150 `WAVE` al puerto 5001, y lectura por SWD de los contadores de retención y de fallo | Ver R-B1. 148/150 respuestas `WAVE` |

La placa quedó con `prj_04` debug, como estaba.

### Estado de cada hallazgo

| ID | Estado | Comprobación |
| --- | --- | --- |
| B1 | **Cerrado en parte** | (a)(b)(c) cerrados: captura nueva sin editar, con marcas de tiempo, y hash limpio en el selftest, H6 y H11. (d) pasa a R-B1: el código de medida existe, pero ningún valor se registra y el límite no sale de él. Nuevo residuo en R-M2: `capture_hsem_run.py` corta en el `VERDICT` de CM7, y la captura del repo pierde la línea H9 y el `CM4 VERDICT` de CM4 |
| B2 | Cerrado | Línea configurable 15 con `RTSR1`/`FTSR1`/`C2EMR1`/`SWIER1`, y contador de despertares escrito por CPU2. H5b y H5c exigen "despertó **y** volvió a aparcar". Residuo menor en R-m3 |
| B3 | Cerrado | IRQ por NVIC en los dos núcleos (`HSEM2_IRQn` con BM en CM4, `HSEM1_IRQn` con `HSEM_HAL_IRQService` en CM7). Cuentas independientes con timeout y FAIL. Los dos controles existen. Residuos en R-M1 y R-m4 |
| B4 | Cerrado en parte | El veredicto de CM7 se calcula y los controles se comprueban e imprimen. Pero ese veredicto "ALL" no incluye los resultados de CM4 (R-M2) |
| B5 | Cerrado | §1.1, §2.3 y §2.4 coinciden con el código. Residuos de documentación en R-m5 |
| B6 | **Abierto** | El implementador no tocó los veredictos de este archivo. Pero `HSEM_IMPLEMENTATION_REPORT.md` sigue diciendo que hubo "un riguroso protocolo de auditoría independiente", conserva la tabla A1–A20 toda en PASS y termina en "concluido, auditado, validado en hardware y cerrado al 100%". Contradice este archivo. Hay que corregirlo, o marcarlo como sustituido por `AUDIT.md` |
| M1 | Cerrado | LL sale en todo lo que no sea `BUSY`, y HAL comprueba `BUSY_SELF` antes y durante la espera. H10 lo cubre (paso 2) |
| M2 | Cerrado en LL/HAL | `HSEM_BM_CheckInit()` en todas las funciones de LL y HAL, y `TRCENA` + `CYCCNTENA`. Pero las funciones de notificación de BM siguen sin comprobarlo (R-m1) |
| M3 | Cerrado | `TakeOrNotify` devuelve `BUSY_SELF` sin armar nada. Sigue sin prueba (R-m5) |
| M4 | Cerrado | H10 con 13 pasos por tier: reentrada, `TakeTimeoutEx` sobre `BUSY_SELF`, errores de parámetro y notificación. No cubre `ReleaseAllOfCore` (queda en H8) |
| M5 | Cerrado en parte | H1 en 2 pasos, H2 en la imagen final y H4 simétrico, hechos y reproducidos. H7 en secuencia **se acepta**: para el hardware, una segunda lectura de `R`/`RLR` desde el mismo núcleo es idéntica venga del contexto que venga, y la detección del driver no depende del contexto. Siguen pendientes H3(b) con escritura cruda e ISF, y las fotos de H0 (R-m4) |
| M6 | Cerrado en parte | `BOARD_HSEM_CLEAR_KEY` en `board_def.h`, fijada por CPU1 en todas las builds, en `prj_04` y en la plantilla. La tabla de IDs existe pero no coincide con el selftest (R-m2) |
| M7 | **Abierto → R-B1** | Ver abajo |
| m1 | Cerrado | `BootWaitForCpu1` ya no limpia `C2ICR` al entrar |
| m2 | Cerrado | `NVIC_ClearPendingIRQ(HSEM2_IRQn)` después de bajar la línea |
| m3 | Cerrado | Un contador por núcleo |
| m4 | Cerrado | `shared_memory.h` solo incluye `<stdint.h>` y `<stdbool.h>`; `SHARED_LOCK` pasa a funciones en `shared_memory.c` |
| m5 | Cerrado | 32 000 000 ciclos a 64 MHz y `Error_Handler` en los dos proyectos. H6 20/20 |
| m6 | Cerrado | Producción ya no llama a `ReleaseAllOfCore` al arrancar |
| m7 | **Cerrado en parte** | El callback HAL se registra con PRIMASK. `ReleaseAllOfCore` **no cambió** (`hsem_bm.c` sin diff en esa función), aunque la entrega dice que sí |
| m8 | Cerrado | `.gitkeep` eliminado, documentación de `hsem_ll.h` corregida, delta real de DWT |

### Hallazgos nuevos

| ID | Clase | Dónde | Hallazgo | Qué hace falta |
| --- | --- | --- | --- | --- |
| R-B1 | **Bloqueante** | `shared_memory.h` (`SHARED_ADC_TRYLOCK_US = 25`) | **El límite es menor que la retención medida.** Leí por SWD los contadores que añadió la propia entrega, tras 30 s de producción con 300 consultas: CM4 retiene HSEM 4 hasta **7 525 ciclos a 200 MHz (37,6 µs)** en `adc_acquisition.c`, y CM7 hasta **17 069 ciclos a 400 MHz (42,7 µs)** en la respuesta `WAVE`. Con 25 µs, cada núcleo agota el tiempo mientras el otro copia: `m7_adc_lock_fail_count = 5` y `m4_adc_lock_fail_count = 1` (offsets `0x4E8` y `0x4EC` de `g_shared`), y 2 de 150 `WAVE` sin respuesta. Es exactamente S4, reintroducido: antes, con 50 000 ciclos, no pasaba. El comentario "derived from measured SRAM4 retention" no tiene medida detrás, y H11 ("0 fallos") no se reproduce. | Límite ≥ retención máxima medida con margen escrito (p. ej. 100 µs), convertido por núcleo. Registrar los máximos de retención en la captura de H11, junto con los contadores de fallo, con una carga `WAVE` definida |
| R-M1 | Mayor | `hsem_bm.c`, `hsem_hal.c`; ISR del selftest de CM4 | **El driver no puede re-armar una notificación sin perder releases.** `NotifyTake` es one-shot (quita el bit de `IER`), y la única manera de volver a armar es `NotifyEnable`, que primero limpia `ICR`. Un release que llegue entre las dos se borra. El selftest lo esquiva: la ISR de CM4 escribe `HSEM_COMMON->IER` a mano, y el callback HAL de CM7 usa `NotifyEnable` (con pérdida), tapado porque las dos pruebas van al paso: el emisor espera cada acuse. | Añadir un re-armado que solo ponga `IER` (p. ej. `HSEM_BM_NotifyRearm`, y lo mismo en LL y HAL), usarlo en las pruebas, y añadir una ráfaga de releases sin esperar al acuse |
| R-M2 | Mayor | `CM7 hsem_selftest.c:1164-1166`; `tools/capture_hsem_run.py` | **El veredicto global no es global.** `ALL HSEM SELF-TESTS PASSED` no incluye `h0/h3/h7/h10/h9_m4_pass`, y se imprime antes de que CM4 termine. La comparación "exactamente 500" solo decide el texto de la línea, no `h9_ok`: 501 IRQ no hacen fallar el veredicto de CM7. El script de captura corta en el primer `VERDICT:`, y por eso la captura del repo no tiene el resultado H9 ni el veredicto de CM4. | CM7 espera a CM4 e incluye sus flags. Cuenta exacta dentro de `h9_ok`. Capturar hasta `CM4 VERDICT=` |
| R-m1 | Menor | `hsem_bm.c` | `NotifyEnable/Disable/Take`, `IsLocked`, `GetOwner` y `GetLockedMask` de BM no llaman a `CheckInit`, y los de LL sí. Con `HSEMEN` apagado, `HSEM_BM_NotifyEnable` devuelve `true` y `HSEM_LL_NotifyEnable`, `false`: la paridad se rompe en el camino de error |
| R-m2 | Menor | `shared_memory.h` (tabla de IDs) | La tabla no coincide con el selftest. Dice 27 "cross" y 31 "notify"; el selftest usa 26 para H9, 27 para H4, 24 y 25 desde los dos núcleos en H1, 28 como `M4_LOCK1` y como canario de CM7, y CM4 toma 24–27 en H8. Tampoco dice qué contextos usan cada ID ni con qué procedimiento, que es lo que mitiga S1 |
| R-m3 | Menor | `prj_04 CM7 main.c` (H5c) | H5c pone `C2IMR1[15]` además de `C2EMR1[15]`. La IRQ `EXTI15_10` de CM4 queda pendiente en su NVIC, y nadie la limpia. En la build de selftest, el primer `NVIC_EnableIRQ(EXTI15_10_IRQn)` de CM4 entraría en la ISR sin motivo. Con el evento basta: `C2IMR1` sobra |
| R-m4 | Menor | Selftests | El control 2 de H9 no demuestra que existiera un ISF viejo: habría que registrar `C2ISR[26]` antes del `NotifyEnable`. H3(b) desde CM4 usa `HSEM_BM_Release`, que ni escribe (lo para la comprobación del driver), así que no prueba la protección del hardware frente al otro núcleo, y no se mira el ISF. H0 sigue sin fotos de registros antes y después de los rechazos |
| R-m5 | Menor | `README.md` | No hay lista de lo no ejercitado: `TakeOrNotify`, `ReleaseAllOfCore` con re-toma concurrente, BM sin `Init`. El log del selftest dice "14 steps" y el código tiene 13 (pasos 0 a 12), como dice §9. El pitfall 2 recomienda `ReleaseAllOfCore` tras un reset en caliente, pero el HSEM se resetea con el sistema. §9.1 dice que las retenciones se midieron, y no da ningún valor |
| R-N1 | Nota | `hsem_bm.h` | El campo nuevo de `HsemBm_BootWait_t` es compatible (al final y NULL por defecto), pero es un cambio de estructura después de congelar F1. Queda registrado |

### Para cerrar

1. **R-B1:** límite derivado de la medida, y H11 regrabado con carga `WAVE`, máximos de retención y contadores de fallo en la captura.
2. **R-M2:** veredicto global con CM4 y captura hasta `CM4 VERDICT=`.
3. **R-M1:** re-armado sin pérdida, y prueba en ráfaga.
4. **B6:** corregir o marcar como sustituido `HSEM_IMPLEMENTATION_REPORT.md`.
5. Los menores (R-m1 a R-m5 y la parte pendiente de m7) pueden ir en la misma entrega o quedar apuntados para después; ninguno bloquea.

Después repetiré el selftest, H6 y la carga de producción con lectura por SWD de los contadores.

---

## Reauditoría 2 — cierre

Sep 26, 2026 · auditor: Claude · commit auditado: `b6d0b26` (6 commits desde `afa410d`, todos con IDs)

**Veredicto: el cierre pasa.** No queda ningún bloqueante ni ningún mayor. Todo lo que dependía del hardware lo he reproducido yo en la placa. Quedan cinco menores y notas, que según el protocolo pueden quedar apuntados sin bloquear. `10_hsem` queda **hecho y auditado**.

### Qué ejecuté

| # | Qué | Resultado |
| --- | --- | --- |
| 1 | `tools/build_all.ps1 -Clean` | 8/8 PASS. `prj_02` y `prj_03` sin cambio de tamaño |
| 2 | Build de `selftest-hsem` | Limpio. El hash sale `b6d0b26-dirty`: la única diferencia con `HEAD` es este archivo, sin commitear (`git status`) |
| 3 | Selftest en la placa, capturado yo 40 s | Todo PASS en los dos núcleos, incluidos `CM4 sub-verdict: done=15 all_passed=1` antes del `VERDICT` global y `CM4 VERDICT=PASS`. Controles: H2 `lost=39907`, `torn_reads=19232`; H7 `raw HAL_HSEM_FastTake=0`; H9 control 2 con `stale flag was=1`; H5c `woke=1 reparked=1` |
| 4 | Producción: 150 consultas de texto a 50 Hz y **300 `WAVE` seguidos sin pausa** (más duro que los 10 Hz del H11 entregado), y lectura por SWD de `g_shared` + `0x4E0` | 300/300 respuestas de 1080 B. `m7_adc_lock_fail_count = 0`, `m4_adc_lock_fail_count = 0`. Picos: CM7 `0x428E` = 17 038 ciclos (42,6 µs), CM4 `0x1D56` = 7 510 ciclos (37,5 µs), frente a un límite de 100 µs |
| 5 | H6: 20 resets en caliente | 20/20, primera muestra `m7=4 m4=20` en todos |
| 6 | Diff del driver y de los selftests | Ver la tabla |

La placa quedó con `prj_04` debug.

### Estado de lo que seguía abierto

| ID | Estado | Comprobación |
| --- | --- | --- |
| R-B1 | Cerrado | Límite de 100 µs convertido por núcleo, sacado de retenciones medidas y ahora publicadas en `g_shared` (`0x4F4`, `0x4F8`). Mi carga sin pausa da 0 fallos |
| R-M1 | Cerrado (driver) | `NotifyRearm` en BM, LL y HAL solo pone `IER`, no toca `ICR`, y con PRIMASK. Las pruebas lo usan en lugar de escribir el registro a mano. Residuo de prueba en R2-m1 |
| R-M2 | Cerrado | CM7 espera `cm4_done == 15` e incluye `cm4_all_passed` y `h0/h3/h7/h10/h9_m4_pass` en el veredicto. Cuentas exactas (500 y 100) dentro de `h9_ok`. La captura llega hasta `CM4 VERDICT=` |
| B6 | Cerrado | `HSEM_IMPLEMENTATION_REPORT.md` empieza con un aviso de que lo sustituye `AUDIT.md` y ya no se declara cerrado |
| R-m1 | Cerrado | `CheckInit` en todas las funciones de BM |
| R-m2 | **Cerrado en parte** | Los IDs de selftest coinciden con el código. Los de aplicación no: R2-m2 |
| R-m3 | Cerrado | H5c solo usa `C2EMR1` |
| R-m4 | Cerrado | H9 registra y exige el ISF viejo. H3(b) hace la escritura cruda desde CM4 y comprueba que el ISF no cambia. H0 compara registros antes y después (R2-m3) |
| R-m5 | Cerrado | §9.2 con lo no ejercitado, 13 pasos en el log y en el README, pitfall 2 corregido, retenciones con valores |
| m7 | Cerrado | `ReleaseAllOfCore` solo mira los IDs que tenía el núcleo antes (`mask_after & mask_before`) |

### Menores que quedan apuntados

No bloquean el cierre. Se pueden corregir cuando se vuelva a tocar el driver o `prj_04`.

| ID | Dónde | Hallazgo |
| --- | --- | --- |
| R2-m1 | Selftests (H9) | La "ráfaga" separa cada release por unos 2000 NOP, así que la ISR termina antes del siguiente y ningún release cae entre `NotifyTake` y `NotifyRearm`. La prueba también pasaría con el re-armado antiguo, que perdía releases, y no demuestra R-M1. La corrección del driver es correcta por construcción. Una prueba determinista: la ISR de CM4, después de `NotifyTake`, espera a que CM7 haga otro release y solo entonces re-arma; con `NotifyRearm` tiene que llegar una IRQ más, y con `NotifyEnable` no (ese es el control) |
| R2-m2 | `shared_memory.h` (tabla de IDs) | Las filas de aplicación no coinciden con el código. HSEM 3 se toma con `SHARED_LOCK` (1 paso, sin límite) en los dos núcleos, y la tabla dice "CPU2 · `Shared_TryLockCycles`". Los IDs 1 y 2 no los usa nadie, y la tabla les asigna contexto |
| R2-m3 | Selftests (H0) | La foto de registros incluye `HSEM->CR`, que siempre se lee como 0 (RM0399 §11.4.7): esa parte de la comparación no mide nada. Serviría `R[0..31]` y `KEYR` |
| R2-m4 | `README.md` | La fila de §5 dice que `ReleaseAllOfCore` escribe `KEYR`; solo escribe `HSEM_CR` (`KEYR` lo escribe `SetClearKey`). §9.2 punto 3 justifica no probar BM con `HSEMEN = 0` "para evitar bus faults", pero `CheckInit` devuelve antes de tocar el periférico |
| R2-m5 | `docs/hsem_regression_capture.txt`, `HSEM_IMPLEMENTATION_REPORT.md` | La línea "Received: 300 / 300" del H11 es un resumen escrito, no la salida de `query_adc.py`. Lo reproduje, así que no invalida nada, pero la próxima vez tiene que ir la salida cruda. El enlace del aviso del informe (`file:///01_shared_drivers/…`) no funciona en el repo |

### Notas

- R-N1 sigue registrado: el campo `wake_count` se añadió a `HsemBm_BootWait_t` después de congelar F1. Es compatible.
- Se acepta H7 en secuencia (ver la reauditoría 1).
