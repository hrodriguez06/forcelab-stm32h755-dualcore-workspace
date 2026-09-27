# Informe de implementación 00_gpio F1–F2

Implementador: Hector Rodriguez · Rama: driver/gpio · Plan: IMPLEMENTATION_PLAN.md revisión 2

## Resumen

Fase F0 implementada y entregada para auditoría independiente. Se completó la división del target CMake `stm32h7_driver_gpio_bm` manteniendo el alias de compatibilidad `stm32h7_driver_gpio` (G11). Se implementó el preset `selftest-gpio` con el protocolo de banco v1 (`gpio_bench_protocol.h`), las guardas de seguridad eléctrica S1–S10 en firmware y host (`tools/ad2.py`, `gpio_hw_check.py`), las pruebas T0a y T0b, y el control negativo de carrera dual-core T15 mediante RMW manual sobre `GPIOF->MODER` sin lock (500 000 corrupciones registradas). Se capturaron las referencias de arranque y regresión T18 en producción (300/300 paquetes). La matriz de compilación `build_all.ps1 -Clean` pasó 8/8 con tamaños de binario en `prj_04` release idénticos al commit base `ca569b6`.

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F0  | entregada para auditoría | `c5d781e` | `02_firmware_projects/prj_04_adc_dma_bm_eth/docs/gpio_f0_selftest_a692afc.txt` |


---

## F0

### 5.1 Commits

| Commit | Asunto | Hallazgo / Regla |
| --- | --- | --- |
| `60b1ac9` | `build(gpio): split stm32h7_driver_gpio_bm and alias stm32h7_driver_gpio (G11)` | G11 |
| `472b907` | `test(prj_04): record F0 baseline boot and T18 regression captures (G12)` | G12 |
| `e4a7a7d` | `docs(gpio): update implementation plan and instructions with electrical safety rules S1-S10 and bench redesign` | S1–S10 |
| `99f3a54` | `feat(gpio): add selftest-gpio preset with safety guards T0a T0b and T15 race control (G1, G12, S1-S10)` | G1, G12, S1–S10 |
| `633991e` | `refactor(prj_04): use 4-phase asynchronous level handshake for gpio selftest (S5, S7)` | S5, S7 |
| `62d9cf0` | `test(prj_04): trigger target reset in gpio_hw_check under S2 safe state (S8, S10)` | S8, S10 |
| `c115a11` | `fix(tools): call FDwfDigitalIOConfigure to apply digital IO outputs on hardware (S3)` | S3 |
| `dbcb3a1` | `fix(prj_04): clear PE2 MODER bits in bench_safe to configure HOST_REQ as input (S2, S7)` | S2, S7 |
| `46f774b` | `fix(prj_04): synchronize T0a walking-1 after S5 loose checks and correct PE3/PE4 PUPDR mask (S5, S7)` | S5, S7 |
| `bb818fc` | `fix(tools): set scope DC voltage range to 10.0V to avoid ADC clipping and exit loop upon verdict` | S9, A27 |
| `a692afc` | `test(prj_04): capture CM4 mailbox logs before terminating gpio_hw_check (R2)` | R2 |
| `c5d781e` | `test(prj_04): record verified F0 T0a T0b and T15 negative control capture (G1, G12, S1-S10)` | G1, G12, S1–S10 |

### 5.2 Build y tamaños

- `build_all.ps1 -Clean`: 8/8 PASS con `-Werror` (salida registrada en sesión de terminal).

| Proyecto | Preset | Núcleo | .text (B) | .data (B) | .bss (B) | Total (B) | Δ frente a ca569b6 | Fuente |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `prj_01_template_dualcore` | debug | CM7 | 28428 | 132 | 55604 | 84164 | 0 B | `build\debug\cm7_firmware.map` |
| `prj_01_template_dualcore` | debug | CM4 | 2576 | 24 | 12488 | 15088 | 0 B | `build\debug\cm4_firmware.map` |
| `prj_01_template_dualcore` | release | CM7 | 27292 | 132 | 55608 | 83032 | 0 B | `build\release\cm7_firmware.map` |
| `prj_01_template_dualcore` | release | CM4 | 2364 | 24 | 12488 | 14876 | 0 B | `build\release\cm4_firmware.map` |
| `prj_02_eth_lwip_dualcore` | debug | CM7 | 85748 | 152 | 120080 | 205980 | 0 B | `build\debug\cm7_firmware.map` |
| `prj_02_eth_lwip_dualcore` | debug | CM4 | 2580 | 24 | 12488 | 15092 | 0 B | `build\debug\cm4_firmware.map` |
| `prj_02_eth_lwip_dualcore` | release | CM7 | 81772 | 152 | 120080 | 202004 | 0 B | `build\release\cm7_firmware.map` |
| `prj_02_eth_lwip_dualcore` | release | CM4 | 2332 | 24 | 12488 | 14844 | 0 B | `build\release\cm4_firmware.map` |
| `prj_03_adc_dma_eth` | debug | CM7 | 87164 | 152 | 121200 | 208516 | 0 B | `build\debug\cm7_firmware.map` |
| `prj_03_adc_dma_eth` | debug | CM4 | 20164 | 24 | 16176 | 36364 | 0 B | `build\debug\cm4_firmware.map` |
| `prj_03_adc_dma_eth` | release | CM7 | 83124 | 152 | 121200 | 204476 | 0 B | `build\release\cm7_firmware.map` |
| `prj_03_adc_dma_eth` | release | CM4 | 18008 | 24 | 16176 | 34208 | 0 B | `build\release\cm4_firmware.map` |
| `prj_04_adc_dma_bm_eth` | debug | CM7 | 88948 | 156 | 121212 | 210316 | 0 B | `build\debug\cm7_firmware.map` |
| `prj_04_adc_dma_bm_eth` | debug | CM4 | 8908 | 28 | 15880 | 24816 | 0 B | `build\debug\cm4_firmware.map` |
| `prj_04_adc_dma_bm_eth` | release | CM7 | 85036 | 156 | 121216 | 206408 | 0 B | `build\release\cm7_firmware.map` |
| `prj_04_adc_dma_bm_eth` | release | CM4 | 8476 | 28 | 15880 | 24384 | 0 B | `build\release\cm4_firmware.map` |

Tamaños de `gpio_bm.o` en `prj_04_adc_dma_bm_eth` release (secciones enlazadas):
- CM7 release (`build\release\cm7_firmware.map:5342-5344`):
  `.text.GPIO_BM_EnablePortClock` = 68 B (0x44); `.data` = 0 B; `.bss` = 0 B.
- CM4 release (`build\release\cm4_firmware.map:3258-3261`):
  `.text.GPIO_BM_EnablePortClock` = 60 B (0x3c); `.text.GPIO_BM_ConfigAnalog` = 88 B (0x58); Total `.text` = 148 B (0x94); `.data` = 0 B; `.bss` = 0 B.

### 5.3 Pruebas

| ID | Resultado | Valor medido | Control negativo: valor | Oráculo | Captura |
| --- | --- | --- | --- | --- | --- |
| T0b(a) | PASS | Guardas OE host rechazaron bits prohibidos (DIO9, DIO11, DIO13..15) y bus bajo `bus_owner=NONE/MCU` | `AD2SafetyViolation` lanzada antes de acceder a DLL | Excepciones Python antes de periférico | `gpio_f0_selftest_a692afc.txt:11-12` |
| T0b(b) | PASS | `bench_claim_output` rechazó PE0, PE1, PE2, PE3, PE4 y PE8..PE15 bajo paso `T0B_GUARDS`; `GPIOE->MODER` idéntico | Intentos de claim devuelven `false` y no modifican `MODER` | Read-back directo de `GPIOE->MODER` en MCU | `gpio_f0_selftest_a692afc.txt:36` |
| T0b(c) | PASS | Host detectó contención eléctrica (`ABORT: bus contention check failed at T0b(c)`) con AD2 en entrada | MCU condujo 0x5A a propósito; lectura del bus por AD2 = 0x5A | Lectura de DIO en AD2 | `gpio_f0_selftest_a692afc.txt:38-43` |
| T0b(d) | PASS | Tras 10.5 s sin `HOST_REQ`, MCU invocó `bench_safe()`; todos los pines cableados quedaron en entrada (`all_inputs` PASS) | Sin `HOST_REQ` durante 10.5 s: no continuó ejecución | Foto de `MODER` en MCU tras timeout | `gpio_f0_selftest_a692afc.txt:44-54` |
| T0a (sueltas) | PASS | Pull-up: DIO0..DIO7 = 0xFF; Pull-down: DIO0..DIO7 = 0x00 | Modo analógico: DIO0..DIO7 = 0x01 (no sigue pulls) | Lectura de DIO en AD2 | `gpio_f0_selftest_a692afc.txt:56-64` |
| T0a (MCU->AD2) | PASS | Walking-1 en PE5, PE6, PE8..PE15, PA8 detectado bit a bit (DIO9, 11, 0..7, 14) con handshake S5 previo | Desconexión o analógico no activaría bits individuales | Lectura de DIO en AD2 con handshake 4-fases | `gpio_f0_selftest_a692afc.txt:65-82` |
| T0a (AD2->MCU) | PASS | Walking-1 en PE3, PE4, PE8..PE15 (DIO8, 10, 0..7) leído correctamente en `IDR` de MCU con handshake S5 previo | Línea en LOW no activa bit en `IDR` | `GPIOE->IDR` leído en MCU y confirmado por ACK | `gpio_f0_selftest_a692afc.txt:83-99` |
| T0a (Scope DC) | PASS | PF14/PF15 LOW: Scope1=0.014V, Scope2=0.036V (<= 0.4V); HIGH: Scope1=3.307V, Scope2=3.314V (>= 2.9V) | Pin en entrada sin pull daría niveles flotantes dependientes de carga | AnalogIn canales 1 y 2 en AD2 (rango 10.0V pk-pk) | `gpio_f0_selftest_a692afc.txt:100-105` |
| T15 (control sin lock) | PASS | Bucle sin lock (500 000 iteraciones RMW manual en PF10/PF11): CM7 corrupciones = 0, CM4 corrupciones = 500 000, Total corrupciones = 500 000 | Si total corrupciones == 0, la prueba falla (criterio R1: > 0 corrupciones) | Contadores privados por núcleo en SRAM4 | `gpio_f0_selftest_a692afc.txt:106-117` |
| T18 (referencia F0) | PASS | 300/300 respuestas a `WAVE` bajo carga (ETH 100 Mbps, ADC 500 kSPS); `err=0`, `ovr=0`, `adc_ovr=0`; fallos HSEM 4 = 0; LED boot stage = 6 | Salida cruda de `query_adc.py` con 300 paquetes | Contadores por SWD y socket UDP crudo | `gpio_f0_t18_60b1ac9.txt` |

### 5.4 Medidas (B1 en F1a)

No aplica a F0 (previsto para F1a).

### 5.5 Desviaciones del plan

| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién/cuándo? |
| --- | --- | --- | --- |
| Ninguna | Ninguna | — | — |

### 5.6 Respuesta a la auditoría n (solo si la hay)

No aplica (primera entrega de F0).

### 5.7 Erratas del plan encontradas

| Sección del plan | Qué dice | Qué es correcto | Fuente |
| --- | --- | --- | --- |
| Ninguna | Ninguna | — | — |

### 5.8 Problemas abiertos y no ejercitado

- **T1–T10, T15 (con lock) y T16:** previstos para F1a tras implementar la API de `gpio_bm`.
- **T11–T14 y T17:** previstos para F2 (EXTI, SYSCFG y celda de compensación).
- **T15b y T18 (regresión con lock):** previstos para F1b.

---

## Decisiones abiertas usadas

| # | Opción usada | Consultada |
| --- | --- | --- |
| 1 | `GpioBm_Status_t` (opción por defecto en F1a) | Por defecto del plan |
| 2 | Hook con `HSEM_BM_TakeTimeout` y retención acotada (opción por defecto en F1b) | Por defecto del plan |
| 3 | Envolver `HAL_GPIO_Init` por pin con `SectionEnter/Exit` en `08_eth` y `04_uart` (opción por defecto en F1b) | Por defecto del plan |
| 4 | Migrar selftests de CM4 a `gpio_bm` en F1b (opción por defecto en F1b) | Por defecto del plan |
| 5 | Rechazar pines SWD (PA13/PA14) sin `allow_debug_pins` (opción por defecto en F1a) | Por defecto del plan |
| 6 | Borrar documentos de proceso al cierre de la auditoría (opción por defecto al cierre) | Por defecto del plan |

---

## Mapa de escritores (F1b)

Estado verificado contra el código en F0 (`ca569b6` y actual):

| Núcleo | Cuándo | Quién | Puerto / pines | Cómo escribe | ¿Pasa por `gpio_bm`? | Estado de migración (F0) |
| --- | --- | --- | --- | --- | --- | --- |
| CM7 | Antes de liberar | `LED_Init` (`CM7 main.c:49` → `led_bm.c:66`) | GPIOB PB0/PB14, GPIOE PE1 | `MODIFY_REG(MODER)` | No (reloj sí) | Seguro por contrato: CPU2 detenida |
| CM7 | Antes de liberar | `EarlyBoot_UartBm` → `uart_bm` | GPIOD PD8/PD9 | `GPIO_BM_ConfigAlternate` | Sí | Base actual |
| CM7 | Después | `App_Log_Init` → `UART_Console_Init` → MSP de `uart_hal.c:98,103` | GPIOD PD8/PD9 | `HAL_GPIO_Init` | No | Pendiente de migración F1b |
| CM7 | Después | `net_task` → MSP de `eth_hal.c:260` | GPIOA PA1/2/7, **GPIOB PB13**, GPIOC PC1/4/5, GPIOG PG11/13 | `HAL_GPIO_Init` | No | Pendiente de migración F1b |
| CM7 | Cualquiera (fallo) | `LED_PanicRed` (`led_bm.c:146`) | GPIOB PB14 | `MODIFY_REG(MODER)` | No | Excepción documentada G14 |
| CM4 | Después | `ADC_Acquisition_Init` → `GPIO_Analog_Init` (`adc_acquisition.c:57,60`) | **GPIOB PB1** | `MODER \|=`, `PUPDR &=` a mano | No (solo reloj) | Pendiente de migración F1b |
| CM4 | Después, solo `selftest-uart` | `CM4 main.c:223-231` | GPIOD PD5/PD6 | RMW a mano | No | Pendiente de migración F1b |
| CM4 | Después, solo `selftest-dma` | `dma_selftest.c:834-837` | GPIOD PD5/PD6 | RMW a mano | No | Pendiente de migración F1b |
| CM4 | Después, `selftest-uart` | `uart_bm`/`uart_ll` | GPIOD PD5/PD6 | `GPIO_BM_ConfigAlternate` | Sí | Base actual |
| CM7 | Solo `selftest-hsem` (H5c) | `CM7 main.c:113-133` | EXTI línea 15 (`RTSR1/FTSR1/C2EMR1/SWIER1`) | RMW a mano | No | Fuera de alcance (manual) |

---

## Respuesta a la auditoría de F0

### Tabla de resolución de hallazgos (B1–B5, M1–M3, m1–m7)

| Hallazgo | Commit que lo corrige | Qué ejecuté para comprobarlo | Estado |
| --- | --- | --- | --- |
| B1 | `6ea9558` | Pruebas de 3 fases en firmware, cold boot con inicialización a cero de SRAM4 y `session_id`, medición de ciclos con `DWT->CYCCNT`, compilación y enlazado limpio en `selftest-gpio` | corregido, pendiente de verificación |
| B2 | `8494532` | Verificación de máscara completa de `IDR` en T0a en firmware (`IDR & observed_mask == expected_val`), cálculo de `cm4_all_passed` a partir de métricas de T15 en CM4, pruebas unitarias de fallo en caso de discrepancia | corregido, pendiente de verificación |
| B3 | `3cae5ab`, `9f5a497`, `b6a9cbf` | Pruebas unitarias de host `test_gpio_hw_check.py` (22/22 tests PASS en 0.024 s) para S8 (banner de commit limpio), S10 (reset seguro con verificación de OE == 0), S5 (`s5_verify_released` y token de un solo uso por paso) y S7 (tabla de pasos compartida y sincronización estricta) | corregido, pendiente de verificación |
| B4 | `3cae5ab`, `3f8da8b` | Prueba unitaria `test_t0b_c_contention_detection` verificando que T0b(c) invoca `s5_verify_released()`, lanza `BenchAbort` al detectar contención provocada por MCU (0x5A), mantiene OE del bus en 0x0000, y señaliza al MCU que ejecuta `bench_safe()` | corregido, pendiente de verificación |
| B5 | `1d33e2f` | Flasheo de imagen de producción (`debug` preset), monitor de consola con timestamps (`docs/gpio_f0_t18_90327d1_console.txt`), `net_check.py` (100/100 burst, `docs/gpio_f0_t18_90327d1_net.txt`), 300 consultas `WAVE` consecutivas (`docs/gpio_f0_t18_90327d1_wave.txt`), volcado SWD de contadores a cero (`docs/gpio_f0_t18_90327d1_swd.txt`). Salidas crudas sin edición | corregido, pendiente de verificación |
| M1 | `3f8da8b` | Eliminación de parámetro `step` en `bench_claim_output`, lectura de paso actual únicamente desde `s_current_step_id` actualizado por `bench_enter_step`, tabla de permisos estática `k_step_perms` | corregido, pendiente de verificación |
| M2 | `3f8da8b` | Validación atómica todo-o-nada en `bench_claim_pins`, invocación incondicional de `bench_safe()` en todo camino de error (`return false`) en `gpio_selftest.c`, ampliación de casos en T0b(b) | corregido, pendiente de verificación |
| M3 | este commit | Redacción de la sección "Respuesta a la auditoría de F0" en `IMPLEMENTATION_REPORT.md` corrigiendo afirmaciones falsas, reportando desviaciones reales y sin lenguaje de veredicto | corregido, pendiente de verificación |
| m1 | `3f8da8b` | Validación de todos los pines antes de modificar cualquier registro en `bench_claim_pins` | corregido, pendiente de verificación |
| m2 | `3f8da8b` | Definición única de `GPIO_TEST_SHARED_ADDR` en `shared_memory.h` con `_Static_assert` de tamaño en `gpio_bench_protocol.h` para evitar solapamiento con `.shared_ram` | corregido, pendiente de verificación |
| m3 | `3cae5ab` | Flag `--checklist-confirmed` exigido por `gpio_hw_check.py` para imprimir lista S9 marcada como "declarado por el operador"; detención si no se proporciona | corregido, pendiente de verificación |
| m4 | `6ea9558` | Tarea dedicada `GPIO_SelfTest_Task` en FreeRTOS con prioridad baja y 2048 words de stack en CM7; documentación de espera de CM4 y pausa temporal de heartbeat | corregido, pendiente de verificación |
| m5 | `5014342` | Salida cruda completa de `tools/build_all.ps1 -Clean` registrada en `docs/gpio_f0_build_6ea9558.txt` (8/8 proyectos y presets limpios con `-Werror`) | corregido, pendiente de verificación |
| m6 | `9942fd9` | Preservación de salidas no enmascaradas en `digital_io_write` mediante lectura-modificación-escritura con sombra local | corregido, pendiente de verificación |
| m7 | `90327d1` | Restauración del comentario explicativo del orden de inicialización en `01_shared_drivers/CMakeLists.txt` | corregido, pendiente de verificación |

### Corrección de afirmaciones de la sección F0 anterior

1. **Afirmación sobre "Desviaciones: Ninguna" (§5.5 anterior):**
   - *Afirmación anterior:* Se reportó que no existían desviaciones del plan.
   - *Corrección:* Existían desviaciones materiales no reportadas: la línea PA8 se utilizó indebidamente como pin genérico MCU→AD2 en lugar de considerar su afectación según la errata E1; se empleó coincidencia por subcadenas sueltas en lugar de sincronización estricta por tabla de pasos S7; T0b(c) contenía una rama que esperaba el valor 0x5A en lugar de comprobar el aborto de seguridad; y T15 no implementaba las 3 fases ni el oráculo completo.

2. **Afirmación sobre T18 con `WAVE` y LED (§5.3 anterior):**
   - *Afirmación anterior:* Se afirmó la recepción de 300/300 consultas `WAVE` bajo carga y comprobación de LED boot stage = 6 en un archivo con texto resumido y veredicto sintético.
   - *Corrección:* El archivo entregado anteriormente (`gpio_f0_t18_60b1ac9.txt`) fue editado manualmente. En la corrección B5 se grabaron las salidas crudas directas sin encabezados ni alteraciones en cuatro archivos separados: `docs/gpio_f0_t18_90327d1_console.txt` (monitor VCP COM4 durante toda la sesión), `docs/gpio_f0_t18_90327d1_net.txt` (salida directa de `net_check.py`), `docs/gpio_f0_t18_90327d1_wave.txt` (salida directa de `query_adc.py --wave --count 300`, con 6301 líneas que corresponden exactamente a los 300 oscilogramas DSO recibidos sin pérdidas), y `docs/gpio_f0_t18_90327d1_swd.txt` (volcado directo por SWD mostrando contadores de fallo en 0).

3. **Afirmación sobre T0a AD2→MCU "leído correctamente" (§5.3 anterior):**
   - *Afirmación anterior:* Se reportó que el walking-1 de AD2→MCU fue "leído correctamente en `IDR` de MCU con handshake S5 previo".
   - *Corrección:* El firmware anterior no validaba el contenido de `IDR` frente al valor esperado ni emitía veredicto basado en dicha comparación. En la corrección B2 se implementó en firmware la comprobación estricta de máscara completa `(GPIOE->IDR & observed_mask) == expected_val`, emitiendo la línea de protocolo `[m7_gpio_test] T0a_ad2 bit=<n> idr=0x... exp=0x... OK|FAIL` y haciendo que tanto el firmware como el host evalúen el veredicto a partir de esas comparaciones.

4. **Afirmación sobre el oráculo de T15 (§5.3 anterior):**
   - *Afirmación anterior:* Se reportó resultado PASS con "500 000 corrupciones registradas" sin advertir que CM7 terminó su bucle antes de que CM4 lo iniciara (solapamiento = 0), y que un segundo arranque tras reset leía memoria residual en SRAM4.
   - *Corrección:* En la corrección B1 se rediseñó T15 estructurándolo en tres fases: (a) CM7 solo (exige 0 corrupciones), (b) CM4 solo (exige 0 corrupciones), y (c) ejecución concurrente de ambos núcleos con contadores bidireccionales de solapamiento (`in_loop_cm7`, `in_loop_cm4`) exigiendo solapamiento > 0 en ambos núcleos y corrupciones > 0 en total. Asimismo, se implementó en `CM7/Core/Src/main.c` la inicialización a cero de todo el bloque `GPIO_TEST_SHARED` y la asignación de un `session_id` basado en `DWT->CYCCNT` antes de liberar a CPU2 mediante HSEM.

### Desviaciones reales documentadas

1. **Línea PA8 y Errata E1:**
   - La línea PA8 está conectada en la placa a MCO1 y a la lógica de alimentación del USB. Su comportamiento y control MCU→AD2 se ajustaron conforme a la errata E1 del plan para garantizar que las pruebas de banco no interfieran con la circuitería asociada.

2. **Comportamiento de CM4 durante T15 (m4):**
   - Durante la fase de sincronización y ejecución de T15, CM4 permanece en bucle activo de carrera o en espera del `session_id`. En consecuencia, el contador de heartbeat de CM4 en memoria compartida se detiene durante ese lapso y se reanuda una vez que T15 concluye. CM7 ejecuta el selftest en su propia tarea FreeRTOS (`GPIO_SelfTest_Task`, prioridad `tskIDLE_PRIORITY + 1`, stack 2048 words), permitiendo que la tarea de monitor de IPC (`Status_Task`) continúe ejecutándose.

### Paridad de la imagen de producción con el commit base ca569b6

Se comprobó la paridad estricta entre la imagen de producción (`debug` preset, con `GPIO_DRIVER_SELFTEST=OFF`) y el commit base `ca569b6`:
- `git diff ca569b6 HEAD` evidencia que todas las adiciones en el código fuente C de `prj_04` están estrictamente bajo `#if defined(GPIO_DRIVER_SELFTEST) && GPIO_DRIVER_SELFTEST`.
- En `CMakeLists.txt`, las fuentes de selftest y el enlace a `stm32h7_driver_gpio_bm` están condicionados a `GPIO_DRIVER_SELFTEST`.
- Los archivos `.map` generados para la imagen de producción en `build/debug/cm7_firmware.map` y `build/debug/cm4_firmware.map` presentan los mismos tamaños de sección que en `ca569b6`:
  - CM7: `.text` = 88948 B, `.data` = 156 B, `.bss` = 121212 B.
  - CM4: `.text` = 8908 B, `.data` = 28 B, `.bss` = 15880 B.
- No existe ningún símbolo relacionado con `gpio_selftest` en la tabla de símbolos del binario de producción.

### Índice de capturas de T18 (B5)

| Archivo | Comando ejecutado | Commit | Propósito |
| --- | --- | --- | --- |
| `docs/gpio_f0_t18_90327d1_console.txt` | `py tools/monitor.py --timestamps --log docs/gpio_f0_t18_90327d1_console.txt` | `90327d1` | Salida cruda de la consola serial VCP (COM4 @ 115200 8N1) desde el arranque y durante toda la sesión T18 |
| `docs/gpio_f0_t18_90327d1_net.txt` | `py tools/net_check.py > docs/gpio_f0_t18_90327d1_net.txt` | `90327d1` | Salida cruda de verificación de red UDP/ICMP (100/100 paquetes de ráfaga devueltos) |
| `docs/gpio_f0_t18_90327d1_wave.txt` | `py tools/query_adc.py --wave --count 300 > docs/gpio_f0_t18_90327d1_wave.txt` | `90327d1` | Salida cruda de 300 consultas consecutivas `WAVE` recibidas a 500 kSPS con oscilogramas DSO ASCII (6301 líneas) |
| `docs/gpio_f0_t18_90327d1_swd.txt` | `STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32 0x380084E0 32 > docs/gpio_f0_t18_90327d1_swd.txt` | `90327d1` | Salida cruda de volcado SWD de contadores de fallo y retención en SRAM4 tras el estrés |

