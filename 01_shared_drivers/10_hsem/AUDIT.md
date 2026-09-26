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
