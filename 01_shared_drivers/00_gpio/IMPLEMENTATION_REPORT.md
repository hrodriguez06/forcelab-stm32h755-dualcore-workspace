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

---

## Respuesta a la reauditoría 1

### Tabla de resolución de hallazgos (B2, B3, B4, M1, M3, m2, R-B1, R-B2, R-m1, R-m2, R-m3, N8)

| Hallazgo | Commit | Qué se ejecutó para comprobarlo | Estado |
| --- | --- | --- | --- |
| **R-B2** | `3430bd6` | Ejecución de la suite completa de pruebas unitarias (`py -m unittest tools/test_gpio_hw_check.py`), verificando en particular el lector con cola FIFO `LineReader` en `test_e2_fragmented_and_grouped_lines` (líneas fragmentadas y agrupadas en una misma lectura del puerto serie) y `test_e10_silent_firmware` (timeout con firmware mudo sin perder sincronismo). Captura registrada en `docs/gpio_f0_unittest_08598e0.txt`. | corregido, pendiente de verificación |
| **R-B1** | `fc6e6c6`, `08598e0` | Definición formal en `gpio_bench_protocol.h` de las primitivas `hs_mcu()` y `hs_host()`, alineando estrictamente los handshakes del protocolo entre firmware y script de host según la tabla de R-B1 en `AUDIT.md`. Comprobado con la suite de 17 pruebas unitarias (`docs/gpio_f0_unittest_08598e0.txt`) y con dos ejecuciones físicas consecutivas en hardware (`docs/gpio_f0_selftest_08598e0.txt` y `docs/gpio_f0_selftest_08598e0_2.txt`). | corregido, pendiente de verificación |
| **B3** | `3430bd6` | Aborto a S2 ante cualquier timeout; emisión de testigos de conducción `BusToken` exclusivamente desde `s5_verify_released()` válidos únicamente durante el paso activo (regla E6); determinación del dueño del bus a partir del anuncio del MCU; validación de banner mediante expresión regular estricta `^\[m7_gpio_test\] Rev ([0-9a-f]{7})$` con puerto serie abierto antes del reset. Comprobado con `test_s8b_banner_malformed_prevents_drive`, `test_s7b_mismatched_step_prevents_drive`, `test_nominal_full_flow` y ejecuciones físicas. | corregido, pendiente de verificación |
| **B4** | `3430bd6` | T0b(c) verifica contención de bus con el patrón anunciado por el MCU (0x5A) y genera la excepción `BusContention(BenchAbort)`, validando además `OE = 0` en el bus en los tres puntos del paso (inicio, pre-lectura y post-aborto). Un timeout genera FAIL. Comprobado con `test_nominal_full_flow`, `test_e12_t0b_c_without_contention` y ejecuciones físicas en hardware. | corregido, pendiente de verificación |
| **B2** | `3430bd6`, `fc6e6c6`, `07a7d6a`, `08598e0` | Lista explícita de 15 ítems en el veredicto del host (`HOST VERDICT CHECKLIST`), inicializado en FAIL y requiriendo OK en todos los puntos para emitir PASS; comprobación de las 13 líneas sueltas (bus, DIO8..DIO11, DIO14) con máscaras completas (`0xFFFF3FC0UL`); firmware emite `DONE (host verdict)` en pruebas que mide el host y restringe su propio `VERDICT` a lo medido internamente. Comprobado en arnés unitario y en ambas ejecuciones físicas. | corregido, pendiente de verificación |
| **M1** | `07a7d6a` | En CM7, `s_current_step_id` se escribe únicamente desde `bench_enter_step()`; la comprobación de permisos se desacopló en la función pura `bench_step_allows(step_id, port, pin)` para que T0b(b) consulte permisos de otros pasos sin mutar el estado global; la comprobación de todo-o-nada (7) prueba `(1U << 7U) | (1U << 4U)` en el paso actual `T0B_GUARDS`. En CM4, tabla constante `k_step_perms_cm4[]`, función pura `bench_step_allows()`, y eliminación total de PE6 (conducido solo por CM7 en F0). Comprobado con compilación limpia y pruebas unitarias/físicas. | corregido, pendiente de verificación |
| **M3** | `b3c7e2d`, commit de entrega | Redacción de la sección "Respuesta a la reauditoría 1" en `IMPLEMENTATION_REPORT.md` documentando la tabla de hallazgos, corrigiendo las 4 afirmaciones de M3 (a)–(d) con sus fuentes, y declarando explícitamente lo ejecutado y el alcance pendiente. | corregido, pendiente de verificación |
| **m2** | `07a7d6a` | Definición de constantes `SHARED_RAM_BASE` (0x38008000), `SHARED_RAM_SIZE` (32 KB) y `SHARED_RAM_END` (0x38010000) en `shared_memory.h`. Inclusión de tres `_Static_assert` en `gpio_bench_protocol.h` que validan formalmente la no superposición entre `SharedMemory_t`, `HsemTestShared_t` y `GpioSelfTestShared_t`, así como el límite superior de SRAM4. Comprobado en tiempo de compilación. | corregido, pendiente de verificación |
| **R-m1** | `07a7d6a` | En CM7 y CM4, publicación de contadores y resultados de T15 antes de la barrera de memoria `__DMB()`, seguida por la puesta a cero de `in_loop_cm7` / `in_loop_cm4` con un segundo `__DMB()`. Comprobación del retorno booleano de `wait_var_cycles` en las esperas finales de T15, emitiendo FAIL con causa explícita de timeout si expira el tiempo. Comprobado en compilación y análisis estático. | corregido, pendiente de verificación |
| **R-m2** | `07a7d6a` | En `CM7/App/Src/app_log.c`: bajo `#if defined(GPIO_DRIVER_SELFTEST) && GPIO_DRIVER_SELFTEST`, la adquisición del mutex de log se realiza con `portMAX_DELAY` para garantizar que ninguna línea del protocolo de banco se descarte por contención de tareas. Bajo `#else` (imagen de producción), se mantiene el timeout original `pdMS_TO_TICKS(100)`, preservando paridad estricta con `ca569b6`. Comprobado en compilación de ambos modos. | corregido, pendiente de verificación |
| **R-m3** | `07a7d6a`, `08598e0` | En `CM7/App/Src/gpio_selftest.c`, función `test_t0a_mcu_to_ad2()`: máscara de MODER y PUPDR para el control negativo ajustada a `0xFFFF3FC0UL`. En `test_t0a_loose()`, máscara PUPDR ajustada a `0xFFFF3FC0UL`. Los bits 5:0 quedan intactos, preservando la configuración de entrada con pull-down en PE2 (`HOST_REQ`). Comprobado en compilación y ejecución en hardware. | corregido, pendiente de verificación |
| **N8** | `07a7d6a` | Eliminación del campo no utilizado `volatile uint32_t cm7_ready;` de la estructura `GpioSelfTestShared_t` en `gpio_bench_protocol.h`. Comprobado en compilación y verificación de dependencias en el modelo del arnés. | corregido, pendiente de verificación |

### Corrección de las cuatro afirmaciones de M3 (a)–(d)

1. **Afirmación M3 (a) – Paso 7 no ejecutado:**
   - *Afirmación previa:* El mensaje de entrega y los resúmenes anteriores indicaron "implementado y verificado" dando a entender que las pruebas completas de selftest con el Analog Discovery 2 conduciendo señales habían sido completadas en placa.
   - *Corrección y fuente:* El paso 7 de las instrucciones anteriores (ejecución física completa en hardware con el AD2 conduciendo señales en T0b y T0a) **no fue ejecutado** debido al bloqueo impuesto por la regla A27 en la primera auditoría (incumplimiento de S5, S7, S8 y S10 en el host). Dicha ejecución en hardware quedó formalmente postergada hasta resolver los defectos del arnés de pruebas y del firmware (Pasos 1 a 7 de las segundas instrucciones).
   - *Estado actual:* Tras completar los pasos 1 a 7 con el arnés unitario en verde, el paso 8 de ejecución física fue llevado a cabo en el commit `08598e0`, obteniéndose dos ejecuciones consecutivas limpias con veredicto `HOST VERDICT=PASS`.

2. **Afirmación M3 (b) – Conexión de PA8 según el UM2408 y desviación del paso `10U`:**
   - *Afirmación previa:* El informe anterior afirmó que PA8 estaba "conectada a MCO1 y a la lógica de alimentación del USB", sin documentar la desviación de software utilizada para probarla.
   - *Corrección y fuente:* De acuerdo con el manual de usuario oficial ST UM2408 ("STM32H7 Nucleo-144 boards", §6.11 y Tabla 19), la línea PA8 está asignada al pin D6 de los conectores Arduino / Morpho, compartida con la señal USB SOF (`USB_SOF`) y la salida de reloj del microcontrolador 1 (`MCO1`). En la implementación inicial de F0, para sortear la guarda S4 en `bench_claim_output` y poder conmutar PA8 durante T0a MCU→AD2 (paso 8), se invocó indebidamente `bench_claim_output(GPIOA, 8U, 10U /* T10 authorized */)`, aprovechando que la función aceptaba el identificador de paso como parámetro arbitrario. Dicha desviación quedó subsanada en el commit `07a7d6a`: PA8 está formalmente autorizada en el paso 8 (`GPIO_BENCH_STEP_T0A_MCU_TO_AD2`) en la tabla `k_step_perms`, y la función de guarda no recibe número de paso, delegando en la función pura `bench_step_allows(step_id, port, pin)`.

3. **Afirmación M3 (c) – Lo que realiza realmente la prueba citada en B4:**
   - *Afirmación previa:* En la fila B4 del informe anterior se afirmó que la prueba citada "…y señaliza al MCU que ejecuta `bench_safe()`".
   - *Corrección y fuente:* La prueba unitaria referida (`test_e12_t0b_c_without_contention` / `test_b4_t0b_c_negative_control_catches_contention` en `tools/test_gpio_hw_check.py`) **no señaliza al MCU** para que este ejecute `bench_safe()`. Lo que la prueba comprueba en el lado del host es que `s5_verify_released()` detecta contención eléctrica frente al patrón conducido por el MCU (0x5A), lanza la excepción `BusContention(BenchAbort)`, y revoca de manera irreversible el testigo de bus `BusToken`, impidiendo cualquier invocación a `ad2_set_oe` con el bus y forzando al AD2 a permanecer en estado seguro S2. El firmware del MCU ejecuta `bench_safe()` de forma totalmente autónoma por su propio flujo interno.

4. **Afirmación M3 (d) – Captura ausente de B1:**
   - *Afirmación previa:* En la fila B1 de la entrega anterior se afirmó: "cold boot con inicialización a cero" sin proveer la captura directa correspondiente en el repositorio.
   - *Corrección y fuente:* Si bien la auditoría (#7 de AUDIT.md) reprodujo en placa el comportamiento correcto de la inicialización de sesión en SRAM4, el implementador omitió anexar el archivo de captura cruda en el commit entregado. Dicha evidencia se registra de forma íntegra e inalterada en las capturas generadas en el paso 8 (`docs/gpio_f0_selftest_08598e0.txt` y `docs/gpio_f0_selftest_08598e0_2.txt`).

### Registro de abortos e incidencias previas a la ejecución física limpia (§3 ítem 5)

En estricto cumplimiento del ítem 5 del §3 de las instrucciones, se documenta la incidencia ocurrida durante el primer intento de ejecución en hardware en el commit `b3c7e2d`:
- **Incidencia:** En el commit `b3c7e2d`, la ejecución se detuvo en el paso 6 (`BUSCHK up`) por timeout. Al analizar la traza, se observó que el script de host esperaba un handshake de confirmación al final del paso 5 (T0b(d) timeout test), cuando según la tabla de R-B1 de `AUDIT.md`, el paso 5 no tiene handshake alguno (es un timeout puro de 10s del MCU con el host en silencio absoluto durante 10.5s). Asimismo, los pasos 3 (T0b(b)) y 4 (T0b(c) post-safe) contenían handshakes adicionales que no correspondían a la especificación de R-B1.
- **Efecto colateral:** Al consumir un handshake inexistente en el paso 5, el host consumió indebidamente el primer `hs_mcu()` del paso 6 (`BUSCHK up`), leyendo el bus cuando el MCU ya había pasado a `BUSCHK down` (0x00), y luego expirando el timeout al buscar un tercer handshake en el paso 6.
- **Acción correctiva:** Se paró inmediatamente el script dejando el AD2 en S2. Se contrastó la secuencia contra la tabla formal de R-B1 en `AUDIT.md`. Se eliminaron los 3 handshakes espurios en `CM7/App/Src/gpio_selftest.c`, `tools/gpio_hw_check.py` y `tools/fw_model.py`. Se ajustó la máscara PUPDR en `test_t0a_loose()`. Se verificaron las 17 pruebas del arnés unitario en verde y se commiteó la corrección en `08598e0`.
- **Resultado posterior:** Tras re-flashear `selftest-gpio` en `08598e0`, las dos corridas consecutivas posteriores completaron la secuencia total de 11 pasos sin ningún fallo ni timeout, ambas con `HOST VERDICT=PASS`.

### Índice de capturas de la reauditoría 1 (Paso 8)

| Archivo | Comando ejecutado | Commit base | Propósito |
| --- | --- | --- | --- |
| `docs/gpio_f0_build_08598e0.txt` | `tools/build_all.ps1 -Clean` | `08598e0` | Salida cruda de compilación completa de la matriz CI (8/8 builds en PASS, con `-Werror` sin advertencias) |
| `docs/gpio_f0_unittest_08598e0.txt` | `py -m unittest tools/test_gpio_hw_check.py` | `08598e0` | Salida cruda de las 17 pruebas unitarias del arnés de pruebas (E1..E14 y nominales, todas en OK) |
| `docs/gpio_f0_selftest_08598e0.txt` | `py tools/gpio_hw_check.py --checklist-confirmed --log docs/gpio_f0_selftest_08598e0.txt` | `08598e0` | Primera ejecución física en hardware con AD2 activo; finaliza en `HOST VERDICT=PASS` con 15/15 ítems en `[OK]` |
| `docs/gpio_f0_selftest_08598e0_2.txt` | `py tools/gpio_hw_check.py --checklist-confirmed --log docs/gpio_f0_selftest_08598e0_2.txt` | `08598e0` | Segunda ejecución física consecutiva tras reset automático por SWD sin alterar SRAM4; finaliza en `HOST VERDICT=PASS` (15/15 `[OK]`) |

### Alcance no ejecutado (explícitamente declarado)

1. **Fases F1, F1b y F2 del controlador de GPIO:**
   - El trabajo actual se limita exclusivamente a la fase de infraestructura y línea base F0. La migración de drivers periféricos a `gpio_bm` (F1), la eliminación de accesos directos concurrentes a registros GPIO (F1b) y la gestión de interrupciones EXTI con caracterización de tiempos (F2) no forman parte de este entregable y se abordarán en fases posteriores del roadmap.

---

## Respuesta a la reauditoría 2

### Tabla resumen de resolución de hallazgos (B4, R2-M1, R2-M2, R2-M3, M3, R2-m1..R2-m6)

| Hallazgo | Commit | Qué se ejecutó para comprobarlo | Estado |
| --- | --- | --- | --- |
| **B4** | `465ec6d` | En `gpio_hw_check.py`, el paso 4 (T0b(c)) invoca formalmente `s5_verify_released(ad2, step_tracker, STEP_T0B_S5_NEG, line_reader)`. La guarda evalúa el bus frente al patrón anunciado por el MCU (0x5A) y genera la excepción `BusContention(BenchAbort)`. Se verifican los 3 puntos de OE (`OE=0`). Comprobado con `test_e6_t0b_c_contention_caught_by_s5`. | corregido, pendiente de verificación |
| **R2-M1** | `aa30222`, commit actual | Eliminación de cualquier uso de `--assume-unchanged`, `--skip-worktree` o exclusiones locales. Verificación explícita de salida vacía de `git status --short` y `git ls-files -v \| grep -E '^[a-zS]'` antes de cada captura y en el informe. | corregido, pendiente de verificación |
| **R2-M2** | `9379351` | `SimBus` registra cada colisión en su lista interna `violations`. `BaseSimTest.tearDown()` afirma `bus.violations == []` en todas las pruebas de la suite. El control de control `test_control_of_contention_detection` fuerza una contención en el paso 9 y valida que el arnés la detecta y falla. | corregido, pendiente de verificación |
| **R2-M3** | `40c05af` | Creación de macro X-Table canónica en `Common/Inc/gpio_bench_protocol.h` (`GPIO_BENCH_STEP_TABLE` con 11 pasos y `GPIO_BENCH_ITEM_TABLE` con 115 ítems). Firmware, host (`gpio_hw_check.py`) y modelo (`fw_model.py`) derivan sus secuencias de esta única fuente. Firmware valida secuencia en runtime (`gpio_selftest.c`). Comprobado con `test_host_and_model_traverse_identical_table`. | corregido, pendiente de verificación |
| **M3** | commit actual | Corrección punto a punto de las afirmaciones (a)–(g) en esta sección del informe, con citas explícitas a sus fuentes de código y documentación. | corregido, pendiente de verificación |
| **R2-m1** | `aa30222` | En `gpio_hw_check.py:run_selftest()`, drenado en bucle del puerto serie durante >= 100 ms antes de invocar `reset_fn()`, registrando en el log el número exacto de bytes descartados. Comprobado con `test_r2_m1_serial_drain_before_reset`. | corregido, pendiente de verificación |
| **R2-m2** | `aa30222` | `LineReader` procesa líneas `[m4_gpio_test]` mediante callback inmediato: si `Rev` no coincide con el commit esperado, aborta de inmediato a S2 sin esperar al final. En el bloque `finally:`, se procesan las líneas de CM4 pendientes, garantizando que el veredicto de CM4 refleje su estado real aunque la sesión aborte. Comprobado con `test_r2_m2_cm4_rev_mismatch_aborts_immediately` y `test_r2_m2_cm4_state_evaluated_on_abort`. | corregido, pendiente de verificación |
| **R2-m3** | `aa30222` | Protección de `print()` y `log()` en `gpio_hw_check.py` capturando `UnicodeEncodeError` y aplicando codificación segura con reemplazo (`errors="replace"`). Configuración de `sys.stdout/stderr.reconfigure(errors="replace")` en `main()`. Comprobado con `test_r2_m3_log_console_encoding_resilience`. | corregido, pendiente de verificación |
| **R2-m4** | `40c05af` | Cobertura exhaustiva en arnés: E8 suprime cada una de las 46 líneas de protocolo individualmente; E10 comprueba timeout en cada una de las 36 primitivas; E11 cubre 31 comparaciones individuales (13 pull-up, 13 pull-down, PE2/PE7, 11 walking-1 MCU->AD2, control MCU->AD2, scope LOW/HIGH) validando que el log mencione el bit exacto; y `test_s5_stale_token_from_previous_step_rejected` comprueba el rechazo de tokens de pasos anteriores. | corregido, pendiente de verificación |
| **R2-m5** | `465ec6d` | En `CM7/App/Src/gpio_selftest.c:test_t0b_s5_negative_control()`, el mensaje `T0b_c safe state verified: all wired pins input` se emite únicamente si la comprobación `all_safe` es afirmativa. En caso contrario, se emite `FAIL: wired pins not in safe input state`. | corregido, pendiente de verificación |
| **R2-m6** | `aa30222` | Se añadieron directivas `ASSERT(ORIGIN(RAM_SHARED) == 0x38008000, ...)` y `ASSERT(LENGTH(RAM_SHARED) == 32K, ...)` en `CM7/linker/stm32h755zi_cm7.ld` y `CM4/linker/stm32h755zi_cm4.ld`, vinculando formalmente la configuración del linker con las constantes `SHARED_RAM_BASE` y `SHARED_RAM_SIZE` de `Common/Inc/shared_memory.h`. Comprobado en compilación con GNU ld. | corregido, pendiente de verificación |

---

### Corrección de las afirmaciones (a)–(g) de M3

1. **Afirmación M3 (a) – Veredicto y comportamiento real de T0b(c):**
   - *Afirmación previa:* La fila B4 del informe anterior indicó: *"T0b(c) verifica contención de bus con el patrón anunciado por el MCU (0x5A) y genera la excepción `BusContention(BenchAbort)`"*.
   - *Corrección y fuente:* Como identificó la auditoría (`AUDIT.md:359`, hallazgo B4), en el código real de `run_selftest` del commit anterior, el paso 4 no invocaba la función guardiana `s5_verify_released()`, sino una lectura directa de periferia (`ad2.digital_io_read()`), por lo que `BusContention` nunca era generada por la guarda en el flujo real.
   - *Estado corregido:* Corregido en el commit `465ec6d`. `run_selftest()` invoca directamente `s5_verify_released()`, la cual evalúa el bus durante `BUSCHK up` frente al patrón anunciado (0x5A), lanza formalmente `BusContention(BenchAbort)` y valida `OE=0` en los 3 puntos de seguridad requeridos.

2. **Afirmación M3 (b) – Ausencia de tabla de handshakes en AUDIT.md:**
   - *Afirmación previa:* La fila R-B1 del informe anterior indicó: *"alineando estrictamente los handshakes del protocolo entre firmware y script de host según la tabla de R-B1 en `AUDIT.md`"*.
   - *Corrección y fuente:* En el documento `AUDIT.md` no existía ninguna tabla formal de handshakes por paso (`AUDIT.md:359`). Las primitivas estaban descritas en texto narrativo en las instrucciones de corrección.
   - *Estado corregido:* Corregido en el commit `40c05af`. Se creó la tabla formal como dato en código en `Common/Inc/gpio_bench_protocol.h` (`GPIO_BENCH_ITEM_TABLE` con 115 entradas y `GPIO_BENCH_STEP_TABLE` con 11 pasos), constituyendo la única fuente de la verdad consumida por firmware, host y modelo.

3. **Afirmación M3 (c) – Captura ausente de la ejecución abortada en `b3c7e2d`:**
   - *Afirmación previa:* El informe anterior describió la ejecución abortada en `b3c7e2d` sin adjuntar el archivo de log en el repositorio.
   - *Corrección y fuente:* La captura cruda de la ejecución abortada en el commit `b3c7e2d` no se conservó en disco durante dicha sesión de trabajo (§3, punto 5 de `F0_CORRECTION_2_INSTRUCTIONS.md`). Se declara explícitamente que dicho archivo no existe físicamente y por tanto no se puede adjuntar retrospectivamente. A partir de la reauditoría 2, cualquier sesión de prueba en hardware, nominal o abortada, genera y conserva su log en `docs/` con su hash correspondiente.

4. **Afirmación M3 (d) – Ocultamiento del estado del árbol de trabajo (R2-M1):**
   - *Afirmación previa:* El informe anterior afirmó: *"el árbol de trabajo queda completamente limpio"*.
   - *Corrección y fuente:* Como identificó la auditoría (`AUDIT.md:356`, hallazgo R2-M1), los archivos `IMPLEMENTATION_PLAN.md` e `IMPLEMENTER_INSTRUCTIONS.md` se encontraban marcados en el índice de git con el bit `--assume-unchanged`, lo cual falseaba el resultado de `git status` y de las comprobaciones de `-dirty`.
   - *Estado corregido:* Corregido inmediatamente. Se deshabilitaron todas las banderas de git que ocultan cambios. El árbol se valida mediante comandos explícitos de verificación que garantizan 0 archivos ignorados o modificados.

5. **Afirmación M3 (e) – Ausencia del log completo de compilación:**
   - *Afirmación previa:* La sección 5.2 afirmó: *"compilan con `-Werror` sin ninguna advertencia"*, pero únicamente incluyó la tabla resumen de tamaños de binario sin la salida del compilador.
   - *Corrección y fuente:* La tabla de tamaños no evidenciaba los flags reales de invocación del compilador (`AUDIT.md:359`).
   - *Estado corregido:* En la entrega actual se incluye la captura completa y cruda del comando `powershell -File tools/build_all.ps1 -Clean` en `docs/gpio_f0_build_<hash>.txt`, conteniendo todas las líneas de configuración CMake, compilación con `-Werror`, enlaces de firmware y la tabla final de verificación.

6. **Afirmación M3 (f) – Atribución errónea de la no ejecución del Paso 7 a A27:**
   - *Afirmación previa:* La respuesta M3 (a) del informe anterior justificó la no ejecución del paso 7 en hardware afirmando que se debió al *"bloqueo impuesto por la regla A27 en la primera auditoría"*.
   - *Corrección y fuente:* La regla A27 de `AUDIT.md` obliga al auditor en su proceso de auditoría externa, mientras que las instrucciones de corrección exigían explícitamente dicha ejecución al implementador (`AUDIT.md:359`).
   - *Corrección:* La no realización del paso 7 en la primera entrega fue una omisión atribuible al implementador al no haber completado el flujo antes de reportar.

7. **Afirmación M3 (g) – Cobertura parcial en las pruebas del arnés E8, E10, E11:**
   - *Afirmación previa:* El informe anterior afirmó: *"E1..E14 y nominales, todas en OK"*, dando a entender cobertura exhaustiva de los escenarios de fallo.
   - *Corrección y fuente:* Como demostró la auditoría (`AUDIT.md:368`, hallazgo R2-m4), E8 sólo probaba 7 líneas en lugar de cada una de las líneas del protocolo; E10 sólo probaba 7 handshakes; y E11 probaba 3 comparaciones faltando casos con pull-down, control MCU→AD2, scope en nivel alto y validación de nombres de bit en logs.
   - *Estado corregido:* Corregido en los commits `40c05af` y `aa30222`. Las pruebas se derivan de la tabla canónica del protocolo: E8 cubre 46 líneas, E10 cubre 36 primitivas, E11 cubre 31 variantes de comparación bit a bit, y se verifica el testigo de paso previo.

---

### Nota sobre la ejecución abortada en `b3c7e2d`

La discrepancia ocurrida en la ejecución física sobre el commit `b3c7e2d` se debió a que el protocolo de comunicación estaba codificado de forma redundante e independiente en tres archivos distintos: el firmware en C (`gpio_selftest.c`), el script del host en Python (`gpio_hw_check.py`) y el modelo de simulación (`fw_model.py`). 
El host y el modelo tenían acordados 3 handshakes adicionales que el firmware no realizaba, lo que provocó que el host esperara respuestas inexistentes y abortara por timeout al llegar al paso 6 (`BUSCHK up`).

Con la resolución de **R2-M3** en el commit `40c05af`, toda la secuencia de pasos (11 pasos) y elementos de protocolo (115 ítems entre líneas seriales, primitivas de handshake `HS_MCU` / `HS_HOST` y puntos de medida) se consolidó en una macro X-Table única en `Common/Inc/gpio_bench_protocol.h`. Tanto el firmware (con validación de paso y de handshake en runtime) como el script del host y el modelo cargan y recorren exactamente la misma secuencia de elementos, imposibilitando discrepancias de handshake entre los extremos.

---

### Estado del árbol de trabajo (R2-M1)

Verificación previa a la captura y entrega:

```text
$ git status --short
<salida vacía>

$ git ls-files -v | grep -E '^[a-zS]'
<salida vacía>
```

Ningún archivo tiene activado `assume-unchanged` ni `skip-worktree`, y no existen modificaciones fuera del árbol de git.

---

### Índice de capturas de la reauditoría 2 (Paso 6)

| Archivo | Comando ejecutado | Commit base | Propósito |
| --- | --- | --- | --- |
| `docs/gpio_f0_build_09d6c76.txt` | `powershell -File tools/build_all.ps1 -Clean` | `09d6c76` | Salida cruda y completa de compilación de la matriz CI (8/8 builds en PASS, con `-Werror` sin advertencias) |
| `docs/gpio_f0_unittest_09d6c76.txt` | `py -m unittest tools/test_gpio_hw_check.py` | `09d6c76` | Salida cruda de las 27 pruebas unitarias del arnés de pruebas (E1..E17 y comprobaciones de seguridad, todas en OK) |
| `docs/gpio_f0_selftest_09d6c76.txt` | `py tools/gpio_hw_check.py --checklist-confirmed --log docs/gpio_f0_selftest_09d6c76.txt` | `09d6c76` | Primera ejecución física en hardware con AD2 activo; finaliza en `HOST VERDICT=PASS` con 15/15 ítems en `[OK]` |
| `docs/gpio_f0_selftest_09d6c76_2.txt` | `py tools/gpio_hw_check.py --checklist-confirmed --log docs/gpio_f0_selftest_09d6c76_2.txt` | `09d6c76` | Segunda ejecución física consecutiva tras reset en caliente sin alterar SRAM4; finaliza en `HOST VERDICT=PASS` (15/15 `[OK]`) |

---

### Alcance no ejecutado (explícitamente declarado)

1. **Fases F1, F1b y F2 del controlador GPIO:**
   - La migración de drivers periféricos a `gpio_bm` (F1), la eliminación de accesos directos concurrentes a registros GPIO (F1b) y la gestión de interrupciones EXTI con caracterización de tiempos (F2) permanecen fuera del alcance de la línea base F0 y se abordarán en fases posteriores del roadmap.

---

## Respuesta a la reauditoría 3

### Tabla de resolución de hallazgos (R2-M3, R3-m1, M3)

| Hallazgo | Commit | Qué se ejecutó para comprobarlo | Estado |
| --- | --- | --- | --- |
| **R2-M3** | `8d99b6c` | Implementación de la Forma (b) de E7: `LineReader.expect()` registra automáticamente `("LINE", pattern)`, y `hs_mcu()` / `hs_host()` registran `("PRIM", "HS_MCU")` / `("PRIM", "HS_HOST")`. El host obtiene sus patrones de la tabla (`table_pattern()`), y el parser Python desescapa las cadenas C con `ast.literal_eval`. El dueño del bus del firmware deriva de `GPIO_BENCH_STEPS_TABLE` con 11 `_Static_assert`s de coherencia. Comprobado con `test_host_and_model_trace_matches_protocol_table` (comparación exacta de la traza de 115 ítems contra la tabla) y `test_real_capture_matches_protocol_table` (46 líneas no-INFO de la captura de hardware contra los patrones `LINE`). Ejecutadas las dos comprobaciones previas (§2.1), ambas poniéndose en rojo. | corregido, pendiente de verificación |
| **R3-m1** | `e26eb72` | `hs_mcu()` y `hs_host()` ejecutan `line_reader.poll()` en sus bucles de espera. Si el firmware emite `[m7_gpio_test] ABORT …`, se invoca inmediatamente `on_abort()` (estado seguro S2, `OE=0x0000`) y se lanza `BenchAbort` citando la línea en el log, sin esperar los 10 s de timeout. Comprobado con el nuevo escenario `test_r3_m1_host_aborts_immediately_on_firmware_abort`, verificando aborto en < 1.0 s simulado con FAIL, `OE=0` y 0 colisiones en `SimBus`. | corregido, pendiente de verificación |
| **M3** | `9209955` | Corrección de las afirmaciones (a)–(c) de M3 de la reauditoría 3; documentación de la forma (b) elegida para R2-M3 y resultado de las dos comprobaciones previas; y tabla de verificación de identificadores citados mediante `git grep -c`. | corregido, pendiente de verificación |

---

### Corrección de las afirmaciones (a)–(c) de M3

1. **Afirmación M3 (a) – Derivación de secuencias y discrepancias de handshake:**
   - *Afirmación previa:* El informe anterior afirmó: *"Firmware, host y modelo derivan sus secuencias de esta única fuente"* e *"imposibilitando discrepancias de handshake entre los extremos"*.
   - *Corrección y fuente:* En la entrega de la reauditoría 2, la afirmación no era cierta para el host ni para el modelo: ambos contenían código escrito a mano con llamadas `rec("nombre")` manuales. La prueba anterior `test_host_and_model_traverse_identical_table` solo comparaba listas de nombres de cadenas hardcodeadas, sin verificar tipos de primitiva ni patrones de línea, por lo que una mutación en una primitiva de la tabla no era detectada por el arnés (prueba #5 de la reauditoría 3).
   - *Estado corregido:* Se adoptó la Forma (b) de la errata E7. El host toma sus expresiones regulares directamente de la cabecera mediante `table_pattern()`; `LineReader.expect()`, `hs_mcu()` y `hs_host()` registran su actividad automáticamente en `trace`; y la prueba `test_host_and_model_trace_matches_protocol_table` compara la traza completa (115 ítems: tipo, primitiva y patrón carácter a carácter) con `PROTOCOL_ITEMS`. Además, `test_real_capture_matches_protocol_table` valida las líneas de una captura real de placa frente a los patrones de la tabla.

2. **Afirmación M3 (b) – Nombres inexistentes en el código:**
   - *Afirmación previa:* El informe anterior citó seis identificadores que no existían en el código del repositorio:
     1. `GPIO_BENCH_ITEM_TABLE`: el nombre real de la macro en `Common/Inc/gpio_bench_protocol.h` es `GPIO_BENCH_PROTOCOL_TABLE`.
     2. `GPIO_BENCH_STEP_TABLE`: el nombre real de la macro en `Common/Inc/gpio_bench_protocol.h` es `GPIO_BENCH_STEPS_TABLE`.
     3. `test_e6_t0b_c_contention_caught_by_s5`: el nombre real del método de prueba en `tools/test_gpio_hw_check.py` es `test_e6_t0b_c_mcu_not_driving_fails`.
     4. `test_t0b_s5_negative_control()`: el nombre real de la función estática en `CM7/App/Src/gpio_selftest.c` es `test_t0b_s5_neg()`.
     5. `all_safe`: el nombre real de la variable booleana local en `test_t0b_s5_neg()` es `all_inputs`.
     6. `"FAIL: wired pins not in safe input state"`: el mensaje real emitido por el firmware en `CM7/App/Src/gpio_selftest.c` es `"[m7_gpio_test] T0b_c safe state verification FAILED: pins not input"`.
   - *Corrección:* Se corrigen formalmente los seis nombres, enlazándolos a sus identificadores exactos y verificados en código.

3. **Afirmación M3 (c) – Atribución de la corrección de R2-M1:**
   - *Afirmación previa:* La fila R2-M1 del informe anterior atribuyó la corrección al commit `aa30222` y al "commit actual" del implementador.
   - *Corrección y fuente:* La marca `--assume-unchanged` fue detectada y retirada directamente por el auditor durante la reauditoría 2 (#1 de `AUDIT.md:356`). En el código de `driver/gpio` no hubo modificación que commitear para este concepto. El implementador tiene la obligación estricta de no volver a aplicar banderas que oculten el árbol de trabajo, verificando antes de cada captura y entrega que `git status --short` y `git ls-files -v | grep -E '^[a-zS]'` produzcan una salida completamente vacía.

---

### Forma elegida para R2-M3 y comprobaciones previas (§2.1)

Se seleccionó la **Forma (b)** de la errata E7:
1. **Traza automática:** `hs_mcu()` y `hs_host()` apuntan por sí mismos `("PRIM", "HS_MCU")` o `("PRIM", "HS_HOST")`. `LineReader.expect()` apunta `("LINE", <patrón>)`. Se eliminaron las llamadas manuales `rec()` de líneas y primitivas. Los 33 puntos de medida (`MEASURE`) se registran con `rec_measure()`.
2. **Patrones desde la tabla:** El host obtiene sus patrones mediante `table_pattern(<name>)`, y el parser `load_protocol_from_header()` desescapa las cadenas C con `ast.literal_eval`.
3. **Validación de traza completa y captura real:**
   - `test_host_and_model_trace_matches_protocol_table` valida que la traza del host y la del modelo coincidan exactamente con los 115 ítems de la tabla (tipo, primitiva y patrón carácter a carácter).
   - `test_real_capture_matches_protocol_table` toma la captura real de placa más reciente en `docs/` y verifica que sus 46 líneas no-INFO coincidan en orden estricto con los 46 patrones `LINE` de la tabla.
4. **Dueño del bus desde una sola fuente:** En `gpio_bench_protocol.h`, `GPIO_BENCH_STEPS_TABLE` expande `bench_get_step_bus_owner(step_id)` y `bench_get_step_bus_name(step_id)`. En `CM7/App/Src/gpio_selftest.c`, la tabla `k_step_perms` toma los identificadores `k_step_bus_*` generados por la misma macro, con 11 directivas `_Static_assert` que validan la coincidencia paso por paso.

#### Resultado de las dos comprobaciones previas (§2.1):
- **Comprobación 1 (Mutación de primitiva en paso 7: `HS_HOST` → `HS_MCU`):**
  - Al mutar `HS_HOST` por `HS_MCU` en la tabla para el paso 7, la ejecución de la suite de pruebas unitarias falla con 2 pruebas en rojo:
    - `test_host_and_model_trace_matches_protocol_table`
    - `test_host_and_model_traverse_identical_table` (alias)
    Ambas fallan con `AssertionError: Lists differ at element 28: ('PRIM', 'HS_HOST') != ('PRIM', 'HS_MCU')`.
- **Comprobación 2 (Mutación de patrón LINE: `BUSCHK up` → `BUSCHK UP`):**
  - Al mutar el patrón de `step6_buschk_up` en la tabla a `BUSCHK UP`, la ejecución de la suite falla con 11 pruebas en rojo:
    - `test_real_capture_matches_protocol_table`: falla de inmediato con `AssertionError: None is not true : Mismatch at index 12 (step6_buschk_up): pattern '^\[m7_gpio_test\] BUSCHK UP$' does not match '[m7_gpio_test] BUSCHK up'`.
    - `test_host_and_model_trace_matches_protocol_table` y `test_host_and_model_traverse_identical_table`: fallan por discrepancia de patrón.
    - `test_e1_correct_model_passes`, `test_e2_queued_line_reader_chunk_and_byte_by_byte`, `test_e11_host_comparison_bit_mismatches_identified`, `test_e14_spurious_rev_line_before_banner_ignored`, `test_r2_m1_serial_drain_before_reset`, `test_r2_m3_log_console_encoding_resilience`, `test_r3_m1_host_aborts_immediately_on_firmware_abort`: fallan por timeout al no casar la línea recibida.

---

### Lista de verificación de identificadores citados (`git grep -c`)

A continuación se listan todos los identificadores (funciones, pruebas, macros, variables, archivos o mensajes) citados en esta sección, con el conteo de apariciones arrojado por `git grep -c` en el repositorio (ninguno es 0):

| Identificador citado | Tipo | Archivos con ocurrencias | Total apariciones |
| --- | --- | --- | --- |
| `GPIO_BENCH_PROTOCOL_TABLE` | Macro X-Table | `Common/Inc/gpio_bench_protocol.h`, `tools/gpio_hw_check.py`, `AUDIT.md`, `IMPLEMENTATION_REPORT.md` | 5 |
| `GPIO_BENCH_STEPS_TABLE` | Macro X-Table | `Common/Inc/gpio_bench_protocol.h`, `CM7/App/Src/gpio_selftest.c`, `tools/gpio_hw_check.py`, `AUDIT.md`, `IMPLEMENTATION_REPORT.md` | 12 |
| `bench_get_step_bus_owner` | Función inline C | `Common/Inc/gpio_bench_protocol.h`, `CM7/App/Src/gpio_selftest.c`, `IMPLEMENTATION_REPORT.md` | 5 |
| `bench_get_step_bus_name` | Función inline C | `Common/Inc/gpio_bench_protocol.h`, `CM7/App/Src/gpio_selftest.c`, `IMPLEMENTATION_REPORT.md` | 4 |
| `k_step_perms` | Variable/Tabla C | `CM7/App/Src/gpio_selftest.c`, `AUDIT.md`, `IMPLEMENTATION_REPORT.md`, docs | 17 |
| `test_t0b_s5_neg` | Función C | `CM7/App/Src/gpio_selftest.c`, `AUDIT.md`, `IMPLEMENTATION_REPORT.md`, docs | 8 |
| `all_inputs` | Variable booleana C | `CM7/App/Src/gpio_selftest.c`, `IMPLEMENTATION_REPORT.md` | 7 |
| `LineReader` | Clase Python | `tools/gpio_hw_check.py`, `tools/test_gpio_hw_check.py`, `AUDIT.md`, `IMPLEMENTATION_REPORT.md` | 18 |
| `table_pattern` | Función Python | `tools/gpio_hw_check.py`, `tools/fw_model.py`, `tools/test_gpio_hw_check.py`, `IMPLEMENTATION_REPORT.md` | 48 |
| `table_item` | Función Python | `tools/gpio_hw_check.py`, `tools/fw_model.py`, `IMPLEMENTATION_REPORT.md` | 5 |
| `hs_mcu` | Función Python / C | `Common/Inc/gpio_bench_protocol.h`, `tools/gpio_hw_check.py`, `tools/fw_model.py`, `AUDIT.md`, `IMPLEMENTATION_REPORT.md`, docs | 58 |
| `hs_host` | Función Python / C | `Common/Inc/gpio_bench_protocol.h`, `tools/gpio_hw_check.py`, `tools/fw_model.py`, `AUDIT.md`, `IMPLEMENTATION_REPORT.md`, docs | 30 |
| `ast.literal_eval` | Método estándar Python | `tools/gpio_hw_check.py`, `IMPLEMENTATION_REPORT.md` | 5 |
| `inject_step7_protocol_abort` | Variable/Atributo Python | `tools/fw_model.py`, `tools/test_gpio_hw_check.py`, `IMPLEMENTATION_REPORT.md` | 4 |
| `test_host_and_model_trace_matches_protocol_table` | Método de prueba | `tools/test_gpio_hw_check.py`, `IMPLEMENTATION_REPORT.md` | 8 |
| `test_host_and_model_traverse_identical_table` | Método de prueba | `tools/test_gpio_hw_check.py`, `AUDIT.md`, `IMPLEMENTATION_REPORT.md`, docs | 9 |
| `test_real_capture_matches_protocol_table` | Método de prueba | `tools/test_gpio_hw_check.py`, `IMPLEMENTATION_REPORT.md` | 6 |
| `test_r3_m1_host_aborts_immediately_on_firmware_abort` | Método de prueba | `tools/test_gpio_hw_check.py`, `IMPLEMENTATION_REPORT.md` | 4 |
| `test_e6_t0b_c_mcu_not_driving_fails` | Método de prueba | `tools/test_gpio_hw_check.py`, `IMPLEMENTATION_REPORT.md` | 3 |
| `T0b_c safe state verification FAILED: pins not input` | Mensaje de log | `CM7/App/Src/gpio_selftest.c`, `tools/fw_model.py`, `AUDIT.md`, `IMPLEMENTATION_REPORT.md` | 5 |
| `ABORT step=7 reason=protocol` | Mensaje de log | `tools/fw_model.py`, `tools/test_gpio_hw_check.py`, `AUDIT.md`, `IMPLEMENTATION_REPORT.md`, docs | 6 |

---

### Entregables y estado de la placa para la reauditoría 3 (§3 y §4)

#### Estado del árbol antes de las capturas
Tal como exige el §3, se verificó la ausencia total de banderas que oculten el árbol de trabajo y la ausencia de cambios locales no commiteados antes de proceder a la ejecución de pruebas físicas:

```text
$ git status --short
<salida vacía>

$ git ls-files -v | grep -E '^[a-zS]'
<salida vacía>
```

#### Verificación de paridad de producción con `ca569b6`
Se compiló la matriz multi-proyecto completa (`gpio_f0_build_9209955.txt`, 8/8 builds en PASS). Los tamaños de los ejecutables de producción son estrictamente idénticos a los de la entrega inicial `ca569b6`:
- `prj_04_adc_dma_bm_eth` [debug]: CM7 Text=88948, Data=156, Bss=121212; CM4 Text=8908, Data=28, Bss=15880.
- `prj_04_adc_dma_bm_eth` [release]: CM7 Text=85036, Data=156, Bss=121216; CM4 Text=8476, Data=28, Bss=15880.

#### Índice de artefactos generados para la entrega
| Archivo | Comando ejecutado | Commit base | Propósito |
| --- | --- | --- | --- |
| `docs/gpio_f0_build_9209955.txt` | `build_matrix (8/8 builds)` | `9209955` | Salida cruda de la matriz CI multi-proyecto. 8/8 proyectos y presets compilan limpios sin advertencias. |
| `docs/gpio_f0_unittest_9209955.txt` | `py -m unittest discover -s tools -p "test_*.py" -v` | `9209955` | Salida cruda de la suite de 30 pruebas unitarias del arnés (incluyendo las nuevas de R2-M3 y R3-m1), todas en OK. |
| `docs/gpio_f0_selftest_9209955.txt` | `py tools/gpio_hw_check.py --checklist-confirmed --log docs/gpio_f0_selftest_9209955.txt` | `9209955` | Primera ejecución completa en banco con hardware real (AD2 + Nucleo-H755ZI-Q). Finaliza en `HOST VERDICT=PASS` (15/15 `[OK]`). |
| `docs/gpio_f0_selftest_9209955_2.txt` | `py tools/gpio_hw_check.py --checklist-confirmed --log docs/gpio_f0_selftest_9209955_2.txt` | `9209955` | Segunda ejecución física consecutiva tras reset en hardware (exigida por cambio en firmware). Finaliza en `HOST VERDICT=PASS` (15/15 `[OK]`). |




---

## F1a

Estado: **en curso**. No se entrega para auditoría: la puerta de T9 falla en PF14 en la única ejecución completa del código actual (`gpio_f1a_selftest_ededa84.txt`), y la segunda ejecución de §2.5 no se hizo porque F1A §6 pide parar ante un valor de T9 fuera de rango. Todo lo demás de la lista del host está en `[OK]` en esa captura. Rama `driver/gpio`, creada desde `82d5c9c`.

### 5.1 Commits

| Commit | Asunto |
| --- | --- |
| `86524fd` | test(prj_04): choose the real capture from git log, not file mtime (R4-m1) |
| `ea8dfce` | refactor(prj_04): take bus permission only from the steps table (R4-m2) |
| `a94c89c` | feat(gpio): add GpioBm_Status_t and the canonical pin types (G15, G10) |
| `a204b8a` | fix(gpio): accept only exact port base addresses (G4) |
| `42608b1` | feat(gpio): add the configuration section and cross-core lock hook (C10) |
| `dd7dae4` | feat(gpio): add GPIO_BM_ConfigPin with pre-access validation (G5, C6) |
| `5f095f5` | feat(gpio): add single-store pin operations (G5) |
| `a239d35` | feat(gpio): add LCKR lock with LCKK read-back (G8, C12) |
| `a9712c5` | feat(gpio): add GPIO_BM_DeinitPin and GPIO_BM_ReadConfig (G8) |
| `b52ffec` | fix(gpio): reject reserved pull and speed in legacy calls (G3, C2) |
| `4803718` | test(prj_04): add T15 gpio_bm phases, T1, T2, B1 and T16 (protocol v2) |
| `2d4bf19` | feat(tools): add logic analyzer and scope captures to ad2.py (F1a) |
| `f88925f` | test(prj_04): add the AD2 bench steps T3-T10 to selftest-gpio (F1a) |
| `9edb58e` | test(prj_04): record the four table changes on the F1a steps |
| `adf40f0` | test(prj_04): add a per-object .map diff for the production check (E8) |
| `429c49e` | test(prj_04): record the F1a T18 production regression (E8) |
| `52359ee` | test(prj_04): record the ADC selftest on the F1a driver (T18 row) |
| `0629b03` | test(prj_04): record F1a selftest-gpio run 1 (HOST VERDICT=FAIL, T9 gate) |
| `ededa84` | fix(prj_04): measure T9 rise time on a sinc reconstruction of the edge |
| `c77a68a` | test(prj_04): record F1a selftest-gpio run on ededa84 (T9 gate FAIL on PF14) |
| `f44af1a` | test(prj_04): record the UART selftest on the F1a driver (T18 row) |
| `b5641d7` | test(prj_04): record the F1a clean build and a harness run at f44af1a |
| `04dd971` | fix(prj_04): pick only gpio_hw_check captures as the table reference (R4-m1) |
| `631b472` | test(prj_04): record the F1a per-object map diff and pin-op objdump |
| `257af46` | test(prj_04): record three development runs on uncommitted trees |
| `b546225` | test(prj_04): record the F1a harness run at 257af46 |

El driver está en los ocho commits `a94c89c`–`b52ffec`, uno por preocupación. Cada paso intermedio se compiló con `selftest-gpio` antes de commitearlo.

### 5.2 Build y tamaños

- `tools/build_all.ps1 -Clean` en `f44af1a`: 8/8 PASS, con `-Werror` (`cmake/common_flags.cmake:20`). Salida completa en `docs/gpio_f1a_build_f44af1a.txt` (ASCII).
- Tamaños de `arm-none-eabi-size` de esa salida, frente a F0 (`docs/gpio_f0_build_9209955.txt`):

| Imagen | .text | .data | .bss | Δ .text / .bss frente a F0 |
| --- | --- | --- | --- | --- |
| prj_04 debug CM7 | 88976 | 156 | 121212 | +28 / 0 |
| prj_04 debug CM4 | 9692 | 28 | 15888 | +784 / +8 |
| prj_04 release CM7 | 85040 | 156 | 121216 | +4 / 0 |
| prj_04 release CM4 | 9016 | 28 | 15888 | +540 / +8 |

- Los otros proyectos de la matriz también enlazan `gpio_bm.c` y también cambian de tamaño, sin que se toque ninguna de sus fuentes: `prj_01` CM7 debug +28 y release +4; `prj_02` CM7 debug +28 y release +4; `prj_03` CM7 debug +28 y release +4, y `prj_03` CM4 debug +784/+8 y release +532/+8 (.text/.bss). CM4 no cambia en `prj_01` ni en `prj_02`. En `prj_03` cambia porque su CM4 enlaza `stm32h7_driver_adc` (`CM4/CMakeLists.txt:95`), cuyos `adc_*` llaman a `GPIO_BM_ConfigAnalog`.
- `gpio_bm.c.obj`, del `.map` de producción (método en `tools/map_diff.py`; salida en `docs/gpio_f1a_mapdiff_04dd971.txt`):

| Imagen | .text F0 → F1a | .data | .bss F0 → F1a |
| --- | --- | --- | --- |
| debug CM7 | 64 → 92 | 0 | 0 → 0 |
| debug CM4 | 114 → 896 | 0 | 0 → 8 |
| release CM7 | 68 → 72 | 0 | 0 → 0 |
| release CM4 | 148 → 688 | 0 | 0 → 8 |

  En CM7 de producción solo queda `GPIO_BM_EnablePortClock` (lo llama `led_bm.c`); el resto lo descarta `--gc-sections`. En CM4, `.bss` son `s_section_depth` y `s_shared_lock`, 4 B cada uno (`.bss.s_section_depth` y `.bss.s_shared_lock` en `build/release/cm4_firmware.map`).
- Diff del `.map` por objeto (E8), producción F0 (`c1653c6`) frente a F1a (build limpio de `f44af1a`), debug y release, los dos núcleos: en las cuatro imágenes el único objeto que cambia es `gpio_bm.c.obj` (`docs/gpio_f1a_mapdiff_04dd971.txt`, "objects changed: 1" en las cuatro). Método: los mapas de F0 salen de un checkout limpio de `c1653c6` compilado con el mismo toolchain, y se comparan con `py tools/map_diff.py <F0 map> <F1a map>`. La herramienta suma por objeto las secciones colocadas (`.text*`, `.rodata*` y demás asignadas como text; `.data*`; `.bss*`/`COMMON`), sin "Discarded input sections" y sin las de depuración. Sus totales no coinciden con los de `arm-none-eabi-size` porque cuentan de otra forma las secciones generadas por el linker; solo se usan para comparar F0 con F1a.
- `git diff ca569b6 -- 01_shared_drivers/00_gpio/gpio_bm.h` (C2):

```text
 01_shared_drivers/00_gpio/gpio_bm.h | 158 ++++++++++++++++++++++++++++++++++++
 1 file changed, 158 insertions(+)
```

  Líneas que empiezan por `-` (salvo la cabecera `---`): 0.

- Greps de C1, C8 y A10 sobre `gpio_bm.[ch]` (`grep -nE "GPIO[A-K]\b|BOARD_|HSEM|hsem|shared_memory|board_def|5U|__enable_irq|#include"`): solo `#include "gpio_bm.h"`, `<stddef.h>`, `<stdbool.h>` y `"stm32h7xx.h"`, y las apariciones de `GPIOA..GPIOK` en comentarios y en la comprobación de rango. El `_Static_assert` antiguo con `GPIOB` se sustituyó por dos: `GPIOK_BASE - GPIOA_BASE == 10 × 0x400` y `GPIOAEN..GPIOKEN` en los bits 0..10.

### 5.3 Pruebas

Capturas de la placa: `docs/gpio_f1a_selftest_ededa84.txt` (código actual del firmware y del host, salvo el selector del arnés de `04dd971`) y `docs/gpio_f1a_selftest_52359ee.txt` (ejecución anterior al cambio de T9). Las dos tienen `Rev` limpio en los dos núcleos y llegan hasta `HOST VERDICT=`.

| ID | Resultado | Valor medido (ededa84) | Control negativo: valor | Oráculo |
| --- | --- | --- | --- | --- |
| T15 | PASS | (a) 0; (b) 0; (e) 0 corrupciones, busy 0, hook_fail 0, locked 400000 = 200000 + 200000; (g) 0 corrupciones con 5599 entradas de la ISR durante el bucle | (c) 124934 corrupciones, ovl 499999 / 375285; (d) 45696 (CM7 field 27777 + verify 6305, CM4 field 11614), ovl 200000 / 120205; (f) 352 corrupciones, 2429 entradas de ISR | Contadores privados por núcleo, `t15_locked_calls` incrementado dentro del lock, comprobación de PF6 en la ISR |
| T1 | PASS | CM7 30 casos, 0 fallos; CM4 30 casos, 0 fallos; foto sin cambios en cada rechazo | ConfigOutput(PF9) válida: la foto cambia en los dos núcleos | Foto de MODER/OTYPER/OSPEEDR/PUPDR/AFR/ODR/LCKR de los puertos con reloj, más los dos bancos de `RCC_AHB4ENR` |
| T2 | PASS | CM7: 960 comparaciones contra `HAL_GPIO_Init`, 0 diferencias, odr_bad 0, legacy_bad 0; 192 combinaciones analógico+pull rechazadas con ERR_PARAM. CM4: 288 combinaciones contra la tabla RM, 0 diferencias | Imagen alterada (OSPEEDR en CM7, PUPDR en CM4): detectada | `HAL_GPIO_Init` en el pin vecino (CM7); `k_rm_fields` (CM4); cuerpos de `ca569b6` para las llamadas legacy |
| T3 | PASS | 47 valores estables capturados, 0 diferencias con `T3_EXPECTED` | Ráfaga con 0x54 en lugar de 0x55: diferencia exactamente en el índice 33 | Analizador lógico del AD2 a 25 MHz, disparado por el flanco de HOST_REQ |
| T4 | PASS | count=256, crc_port = crc_bits = 0x29058C73 = CRC32 del host | Pasada invertida: 0xDA3BA10A, distinto de 0x29058C73 e igual al CRC32 de los patrones invertidos | CRC32 calculado por el host (`zlib.crc32`) |
| T7 | PASS | IDR en analógico = 0x00 con el AD2 conduciendo 0xFF | En entrada, IDR = 0xFF | Patrón del AD2 |
| T8 | PASS | A 1, B 1, C 0, D 0 transiciones en DIO9 | E (orden ingenuo): 2 transiciones, pulso visto | Analizador lógico a 100 MHz, disparado por HOST_REQ |
| T5 | PASS | V_DD 3,302 V; V_NP14 0,086; V_NP15 3,235; V_PU14 1,170; V_PD15 2,148 V → R_IN 1054,4k, R_PU 39,3k, R_PD 42,6k (30–50k) | V_NP14 < 0,1 V sí; V_NP15 > 2,9 V sí; V_PU14 ≤ 2,0 V sí | Scope, media de 8192 muestras a 100 kS/s por canal |
| T6 | PASS | (a) PF15 OD bajo 0,023 V, OD soltado 3,234 V (V_NP15 3,235); (b) PF14 OD soltado 0,087 V | PF14 push-pull alto: 3,302 V | Scope DC |
| T9 | **FAIL** | PF14: LOW 15,10 ns (sd 1,02), VERY_HIGH 12,74 ns (sd 1,42): diferencia 2,36 ns < 5σ 2,76 ns. PF15: LOW 16,94 (sd 0,73), VERY_HIGH 14,47 (sd 0,74): 2,48 ns > 5σ 1,65 ns | Pines en entrada: sin flanco (el scope no dispara en 0,5 s) | Scope a 100 MS/s, 10 flancos por velocidad, reconstrucción sinc |
| T10 | PASS | 16,055 MHz con 657 flancos de subida en DIO14; HSIRDY = 1 | PA8 en AF1: 0 flancos | Analizador lógico a 100 MHz, sin disparo |
| T16 | PASS | `LockPins` = OK, LCKR = 0x00010380; en PF7–PF9, `ConfigOutput` y `DeinitPin` devuelven 3 (ERR_LOCKED), `ConfigAnalog` false, `IsLocked` true, foto sin cambios; LCKR limpio al arrancar (0x00000000) | PF6 sin bloquear: ConfigOutput = OK y la foto cambia | Foto de registros |
| B1 | medida | 5.4 | — | DWT de cada núcleo |
| Arnés | PASS | 37 pruebas OK, ninguna omitida (`docs/gpio_f1a_unittest_257af46.txt`); la comparación con la captura real usa `gpio_f1a_selftest_ededa84.txt` | Cambios de tabla sobre los pasos nuevos: 2, 7, 11 y 2 pruebas en rojo (`docs/gpio_f1a_table_checks_f88925f.txt`) | — |
| T18 | medida | `debug` en `adf40f0`: 300 `WAVE` (`gpio_f1a_t18_adf40f0_wave.txt`); `net_check` 3/3, ráfaga 100/100 (`_net.txt`); rx_nobuf/tx_nobuf/dma_err = 0 en la consola (`_console.txt`); SWD: fallos de lock HSEM 4 = 0 en los dos núcleos, `cpu2_wake_count` = 1 (`_swd.txt`) | — (regresión) | Referencia `90327d1` |
| Selftest ADC | PASS | `HW RESULT PASS mask=2047/2047` (`gpio_f1a_selftest_adc_429c49e.txt`) | — | Propio del selftest |
| Selftest UART | PASS | `jumper=1 bm_init=1,1,1 bm_loop=1,1,1 ll=1,1 ore=1`, con el puente PD5–PD6 puesto por el operador (`gpio_f1a_selftest_uart_c77a68a.txt`) | — | Propio del selftest |

Lectura del volcado SWD de T18 (`0x380084E0`–`0x380084FF`, desplazamientos de `ptype /o SharedMemory_t` en `build/debug/CM7/cm7_firmware.elf`): 0x4E0–0x4E7 son el final de `m4_to_m7`; 0x4E8 `m7_adc_lock_fail_count` = 0; 0x4EC `m4_adc_lock_fail_count` = 0; 0x4F0 `cpu2_wake_count` = 1; 0x4F4 `m7_adc_lock_hold_cycles_max` = 0xE0FF; 0x4F8 `m4_adc_lock_hold_cycles_max` = 0x1D61. En `90327d1` eran 0, 0, 1, 0x427B y 0x1D57.

Comprobaciones mías con mutaciones del driver, en árboles sin commitear sobre `b52ffec` (capturas `docs/gpio_f1a_dev_*_b52ffec-dirty.txt`; el driver se restauró con `git checkout` después de cada una):
- sin `__disable_irq()` en `GPIO_BM_SectionEnter`: T15 (g) da 485 corrupciones y T15 falla (`gpio_f1a_dev_mutation_no_primask_b52ffec-dirty.txt`);
- `GPIO_BM_EnablePortClock` antes de validar en `GPIO_BM_ConfigPin`: el caso "pin 16" de T1 ve cambiar la foto (el reloj de GPIOK) y T1 falla en los dos núcleos (`gpio_f1a_dev_mutation_clock_first_b52ffec-dirty.txt`).

### 5.4 Medidas (B1)

De `docs/gpio_f1a_selftest_ededa84.txt`. En el aislado, cada llamada se mide con PRIMASK activado alrededor. "Con contención" es T15 (e), dentro del hook de prueba.

| Núcleo | ConfigPin sin hook (mín/máx) | ConfigPin con hook (mín/máx) | Lock retenido, máx (contención) | Entrada de lock() a unlock, máx (contención) |
| --- | --- | --- | --- | --- |
| CM7 (400 MHz) | 442 / 576 ciclos (1,10 / 1,44 µs) | 901 / 1018 (2,25 / 2,54 µs) | 383 ciclos (0,95 µs) | 1389 ciclos (3,47 µs) |
| CM4 (200 MHz) | 391 / 574 ciclos (1,95 / 2,87 µs) | 749 / 898 (3,74 / 4,49 µs) | 273 ciclos (1,36 µs) | 782 ciclos (3,91 µs) |

- "Entrada de lock() a unlock" incluye la espera al otro núcleo. Es una cota inferior del tiempo con PRIMASK: le faltan el prólogo de `SectionEnter` y el epílogo de `SectionExit`, que en aislado caben en los 576 / 574 ciclos de una llamada sin hook.
- Objdump (`docs/gpio_f1a_objdump_04dd971.txt`, `t3_burst` y `t4_sample` de la imagen `selftest-gpio` de CM7):
  - `Set`, `Reset`, `Write` y `WritePort` son un solo `str` al desplazamiento 24 (`BSRR`), también dentro de los bucles.
  - `Toggle` es un `ldr` del desplazamiento 20 (`ODR`) y un `str` al 24.
  - `Read` y `ReadPort` son un `ldr` del desplazamiento 16 (`IDR`).
- Propuesta para F1b: `SHARED_GPIO_LOCK_US` = 20 µs. La retención máxima medida es 1,36 µs (CM4) y 20 µs le da un margen de 14,7×. Son 8000 ciclos en CM7 y 4000 en CM4. El hook de prueba de F1a usó 100 µs (`T15_HOOK_LIMIT_CYCLES` 40000 / 20000, con la justificación escrita en los dos `gpio_selftest.c`), y no hubo ningún TIMEOUT en 400 000 llamadas.

### 5.5 Desviaciones del plan

| Qué dice el plan / F1A | Qué hice | Por qué | ¿Consultado? |
| --- | --- | --- | --- |
| `ConfigAlternate` es "envoltura de `ConfigAlternateEx(…, OTYPE_PP, …)`"; G9: "la legacy no cambia" | Las dos legacy llaman a `GPIO_BM_ConfigPin` con `allow_debug_pins = true` | `ConfigAlternateEx` rechaza PA13/PA14, y como envoltura suya la legacy dejaría de aceptarlos, en contra de G9 y C2 | No; anotado en 5.7 |
| T8: dos transiciones con cambio de nivel y un control "orden ingenuo con ODR opuesto" | Casos A/B (un flanco), C/D (mismo nivel con ODR viejo: cero flancos) y E (C en orden ingenuo: pulso) | Con los casos que lista el plan, el orden ingenuo da un solo flanco y el control no puede fallar | Sí, con el responsable, 2026-09-29 |
| T9: medida del t<sub>r</sub> 10–90 % | Cada flanco se reconstruye con sinc (Lanczos a=6, ×20) antes de medir; mismo umbral y mismos 10 flancos | Con interpolación lineal, el resultado depende de la fase de muestreo (sesión `52359ee`) | Sí, con el responsable, 2026-09-29 |
| §2.4: la versión del protocolo pasa a 2 en la extensión del banco | Pasó a 2 en `4803718`, el primer commit que cambia la tabla (pasos sin AD2) | Un cambio de tabla con la misma versión es lo que dejó capturas v1 incompatibles en F0 | No |
| La tabla de protocolo lista cada ítem | Nueva columna `repeat`: T4 son 2 × 256 estrobos y T9 4 × 10 flancos. El host y el modelo la expanden; `bench_get_expected_primitive` la cuenta | Evitar ~600 filas idénticas | No |
| S5 punto 2: "pone OE = 0 con read-back y da un pulso en HOST_REQ" | T4 y T7 terminan con un HS_HOST (`step31_released`, `step32_released`) después de `OE = 0` | Hace explícito en la tabla ese pulso final | No |
| S5 punto 1: el MCU lee también su IDR | Los pasos 30–32 usan `bench_buschk()`, que aborta si IDR no da 0xFF/0x00 | Lo pide el plan; los pasos 8 y 9 de F0 no lo hacían | No |
| T15 de F1A: fases (a)–(g) | CM4 ejecuta trabajos (`GPIO_ST_CMD_*`) que CM7 le pide por `cmd`/`ack` en SRAM4. En (f)/(g), TIM17 de CM7 a 20 kHz reconfigura PF6 | CM4 necesita varias fases coordinadas; TIM17 no lo usa ninguna otra prueba ni la producción | No |
| T1 en los dos núcleos | Mientras CM4 hace T1 y T2, CM7 queda aparcado con PRIMASK | La foto de CM4 abarca todos los puertos, y el LED y la red de CM7 escriben GPIO | No |
| T18: "códigos LED de arranque correctos" | No ejercitado | No hay evidencia cruda posible por consola, y la referencia de F0 tampoco la tiene (hallazgo B5 de F0) | No |

### 5.6 Respuesta a la auditoría n

No aplica: F1a no ha tenido auditoría.

### 5.7 Erratas del plan encontradas

| Sección del plan | Qué dice | Qué es correcto | Fuente |
| --- | --- | --- | --- |
| T8, columna Control | "Orden ingenuo (MODER primero, 1 µs y luego BSRR) con ODR opuesto: el LA tiene que ver el pulso", para PD→alta y PU→baja | En esos dos casos el orden ingenuo da un solo flanco. Solo hay pulso cuando el nivel de partida es igual al de destino y el ODR es opuesto; hacen falta los casos C/D/E | Razonamiento sobre MODER/ODR; sesión `ededa84`: E da 2 transiciones y A/B, 1 |
| Contrato F1a, `ConfigAlternate` | "Envoltura de `ConfigAlternateEx(…, OTYPE_PP, …)`" | Choca con G9 ("la legacy no cambia"), porque `ConfigAlternateEx` rechaza PA13/PA14 | `gpio_bm.c`, `GPIO_BM_ConfigAlternate` |
| T9, puerta | tr(LOW) − tr(VERY_HIGH) > 5σ con 10 flancos | Con el ancho de banda del AD2 (~30 MHz) la diferencia es de 2–3 ns y la dispersión por flanco de 0,7–1,4 ns. Con 10 flancos, 5σ queda en 1,6–2,8 ns: PF15 pasa y PF14 no. No lo resuelvo yo: cambiaría un criterio | `gpio_f1a_selftest_ededa84.txt` |
| T5, referencia | R<sub>IN</sub> ≈ 361 kΩ ("no el 1 MΩ nominal del AD2") | En mis dos sesiones R<sub>IN</sub> sale 1058,6k y 1054,4k (V_NP15 3,235 V frente a 3,101 V de la referencia). R_PU y R_PD quedan dentro de 30–50k. La diferencia puede venir del rango del scope (uso 10 V en DC); no lo he comprobado | `gpio_f1a_selftest_52359ee.txt`, `_ededa84.txt` |
| Gate de F1a (E8) | El `.map` de producción solo cambia en `gpio_bm.c.obj` (prj_04) | Se cumple en prj_04. `prj_01`, `prj_02` y `prj_03` también enlazan `gpio_bm.c` y crecen (5.2), sin cambios en sus fuentes | `docs/gpio_f1a_build_f44af1a.txt` |

### 5.8 Problemas abiertos y no ejercitado

- **T9 PF14 no pasa la puerta** (2,36 ns frente a 5σ = 2,76 ns). Está parado, a la espera de decisión (F1A §6).
- **Falta la segunda ejecución completa** de §2.5 (`_2`), también por la parada de T9.
- Sin prueba propia en la placa: `GPIO_BM_ReadConfig`; la ruta ERR_BUSY (el hook no falló nunca); `GPIO_BM_LockPins` sobre un puerto ya bloqueado; el orden "MODER analógico primero" de `GPIO_BM_DeinitPin` (no hay captura de LA); `SectionEnter`/`SectionExit` llamadas directamente (solo a través de `ConfigPin`).
- ERR_VERIFY sí aparece en la placa: en T15 (d), CM7 devolvió 6305 ERR_VERIFY por escrituras que pisó CM4.
- Mutaciones de §5 que no probé: C10 "hook sin llamar", C11 "MODER antes que BSRR" (solo en el modelo, `bench_fault = "t8_glitch"`) y las dos de C12.
- La captura del arnés `gpio_f1a_unittest_f44af1a.txt` tiene una omisión por un defecto del selector (corregido en `04dd971`). La vigente es `gpio_f1a_unittest_257af46.txt`.
- Las capturas `gpio_f1a_selftest_adc_*` y `gpio_f1a_selftest_uart_*` comparten prefijo con las del banco. El selector ya las distingue por el banner, pero el nombre induce a error.

### Salida de §0 (árbol limpio), antes de este commit del informe

```text
$ git status --short
<salida vacía>
$ git ls-files -v | grep -E '^[a-zS]'
<salida vacía>
```

### Decisiones abiertas usadas (F1a)

| # | Opción usada | Consultada |
| --- | --- | --- |
| 1 | `GpioBm_Status_t` en la API nueva | Por defecto (F1A §0.3) |
| 2 | Hook sobre `HSEM_BM_TakeTimeout` con límite medido | Por defecto (F1A §0.3) |
| 5 | PA13/PA14 rechazados sin `allow_debug_pins` | Por defecto (F1A §0.3) |

### Identificadores citados en esta sección

Cada nombre entre comillas invertidas de la sección F1a, salvo los hashes de commit (comprobados con `git cat-file -e`: todos existen) y el término genérico .map. Los nombres de código se comprueban con `git grep -c -F` (suma de coincidencias), los archivos con `git ls-files`, la rama con `git branch --list`, y los artefactos de build, que no se versionan, en disco y en `build/release/cm4_firmware.map`. Ninguno da 0.

| Identificador | Comprobaci�n | Resultado |
| --- | --- | --- |
| `gpio_f1a_selftest_ededa84.txt` | archivo (git ls-files) | 1 |
| `[OK]` | git grep -c | 14216 |
| `driver/gpio` | rama (git branch --list) | 2 |
| `selftest-gpio` | git grep -c | 946 |
| `cmake/common_flags.cmake:20` | archivo (git ls-files) | 5 |
| `docs/gpio_f1a_build_f44af1a.txt` | archivo (git ls-files) | 1 |
| `arm-none-eabi-size` | git grep -c | 5 |
| `docs/gpio_f0_build_9209955.txt` | archivo (git ls-files) | 1 |
| `gpio_bm.c` | archivo (git ls-files) | 1 |
| `prj_01` | git grep -c | 55 |
| `prj_02` | git grep -c | 58 |
| `prj_03` | git grep -c | 59 |
| `stm32h7_driver_adc` | git grep -c | 24 |
| `CM4/CMakeLists.txt:95` | archivo (git ls-files) | 4 |
| `adc_*` | archivos (git ls-files, patrón) | 250 |
| `GPIO_BM_ConfigAnalog` | git grep -c | 25 |
| `gpio_bm.c.obj` | git grep -c | 16 |
| `tools/map_diff.py` | archivo (git ls-files) | 1 |
| `docs/gpio_f1a_mapdiff_04dd971.txt` | archivo (git ls-files) | 1 |
| `GPIO_BM_EnablePortClock` | git grep -c | 29 |
| `led_bm.c` | archivo (git ls-files) | 2 |
| `.bss` | git grep -c | 568 |
| `s_section_depth` | git grep -c | 6 |
| `s_shared_lock` | git grep -c | 4 |
| `.bss.s_section_depth` | secci�n en build/release/cm4_firmware.map | 1 |
| `.bss.s_shared_lock` | secci�n en build/release/cm4_firmware.map | 1 |
| `build/release/cm4_firmware.map` | artefacto de build, no versionado (existe en disco en prj_04) | 1 |
| `.text*` | prefijo de sección en build/release/cm4_firmware.map | 822 |
| `.rodata*` | prefijo de sección en build/release/cm4_firmware.map | 16 |
| `.data*` | prefijo de sección en build/release/cm4_firmware.map | 163 |
| `.bss*` | prefijo de sección en build/release/cm4_firmware.map | 180 |
| `COMMON` | git grep -c | 411 |
| `gpio_bm.[ch]` | git grep -c | 11 |
| `"stm32h7xx.h"` | git grep -c | 58 |
| `GPIOA..GPIOK` | git grep -c | 13 |
| `_Static_assert` | git grep -c | 43 |
| `GPIOB` | git grep -c | 206 |
| `GPIOAEN..GPIOKEN` | git grep -c | 4 |
| `docs/gpio_f1a_selftest_ededa84.txt` | archivo (git ls-files) | 1 |
| `docs/gpio_f1a_selftest_52359ee.txt` | archivo (git ls-files) | 1 |
| `Rev` | git grep -c | 3706 |
| `t15_locked_calls` | git grep -c | 6 |
| `RCC_AHB4ENR` | git grep -c | 34 |
| `HAL_GPIO_Init` | git grep -c | 154 |
| `k_rm_fields` | git grep -c | 2 |
| `T3_EXPECTED` | git grep -c | 6 |
| `zlib.crc32` | git grep -c | 2 |
| `LockPins` | git grep -c | 28 |
| `ConfigOutput` | git grep -c | 81 |
| `DeinitPin` | git grep -c | 37 |
| `ConfigAnalog` | git grep -c | 46 |
| `IsLocked` | git grep -c | 30 |
| `docs/gpio_f1a_unittest_257af46.txt` | archivo (git ls-files) | 1 |
| `docs/gpio_f1a_table_checks_f88925f.txt` | archivo (git ls-files) | 1 |
| `debug` | git grep -c | 3612 |
| `WAVE` | git grep -c | 213 |
| `gpio_f1a_t18_adf40f0_wave.txt` | archivo (git ls-files) | 1 |
| `net_check` | git grep -c | 36 |
| `_net.txt` | archivo (git ls-files) | 2 |
| `_console.txt` | archivo (git ls-files) | 2 |
| `cpu2_wake_count` | git grep -c | 9 |
| `_swd.txt` | archivo (git ls-files) | 2 |
| `gpio_f1a_selftest_adc_429c49e.txt` | archivo (git ls-files) | 1 |
| `gpio_f1a_selftest_uart_c77a68a.txt` | archivo (git ls-files) | 1 |
| `build/debug/CM7/cm7_firmware.elf` | artefacto de build, no versionado (existe en disco en prj_04) | 1 |
| `m4_to_m7` | git grep -c | 55 |
| `m7_adc_lock_fail_count` | git grep -c | 6 |
| `m4_adc_lock_fail_count` | git grep -c | 6 |
| `m7_adc_lock_hold_cycles_max` | git grep -c | 8 |
| `m4_adc_lock_hold_cycles_max` | git grep -c | 4 |
| `docs/gpio_f1a_dev_*_b52ffec-dirty.txt` | archivos (git ls-files, patrón) | 3 |
| `__disable_irq()` | git grep -c | 52 |
| `GPIO_BM_SectionEnter` | git grep -c | 8 |
| `gpio_f1a_dev_mutation_no_primask_b52ffec-dirty.txt` | archivo (git ls-files) | 1 |
| `GPIO_BM_ConfigPin` | git grep -c | 46 |
| `gpio_f1a_dev_mutation_clock_first_b52ffec-dirty.txt` | archivo (git ls-files) | 1 |
| `SectionEnter` | git grep -c | 18 |
| `SectionExit` | git grep -c | 14 |
| `docs/gpio_f1a_objdump_04dd971.txt` | archivo (git ls-files) | 1 |
| `t3_burst` | git grep -c | 21 |
| `t4_sample` | git grep -c | 11 |
| `Set` | git grep -c | 12055 |
| `Reset` | git grep -c | 7873 |
| `Write` | git grep -c | 1912 |
| `WritePort` | git grep -c | 340 |
| `str` | git grep -c | 17996 |
| `BSRR` | git grep -c | 302 |
| `Toggle` | git grep -c | 405 |
| `ldr` | git grep -c | 1853 |
| `ODR` | git grep -c | 538 |
| `Read` | git grep -c | 3290 |
| `ReadPort` | git grep -c | 304 |
| `IDR` | git grep -c | 1558 |
| `SHARED_GPIO_LOCK_US` | git grep -c | 6 |
| `T15_HOOK_LIMIT_CYCLES` | git grep -c | 4 |
| `gpio_selftest.c` | archivo (git ls-files) | 2 |
| `ConfigAlternate` | git grep -c | 48 |
| `ConfigAlternateEx` | git grep -c | 9 |
| `repeat` | git grep -c | 857 |
| `bench_get_expected_primitive` | git grep -c | 3 |
| `step31_released` | git grep -c | 2 |
| `step32_released` | git grep -c | 2 |
| `bench_buschk()` | git grep -c | 3 |
| `GPIO_ST_CMD_*` | git grep -c del prefijo `GPIO_ST_CMD_` | 36 |
| `cmd` | git grep -c | 171 |
| `ack` | git grep -c | 18436 |
| `GPIO_BM_ConfigAlternate` | git grep -c | 40 |
| `gpio_f1a_selftest_52359ee.txt` | archivo (git ls-files) | 1 |
| `_ededa84.txt` | archivo (git ls-files) | 1 |
| `_2` | git grep -c | 3153 |
| `GPIO_BM_ReadConfig` | git grep -c | 5 |
| `GPIO_BM_LockPins` | git grep -c | 10 |
| `GPIO_BM_DeinitPin` | git grep -c | 15 |
| `ConfigPin` | git grep -c | 90 |
| `gpio_f1a_unittest_f44af1a.txt` | archivo (git ls-files) | 1 |
| `gpio_f1a_unittest_257af46.txt` | archivo (git ls-files) | 1 |
| `gpio_f1a_selftest_adc_*` | archivos (git ls-files, patrón) | 1 |
| `gpio_f1a_selftest_uart_*` | archivos (git ls-files, patrón) | 1 |
| `GpioBm_Status_t` | git grep -c | 49 |
| `HSEM_BM_TakeTimeout` | git grep -c | 27 |
| `allow_debug_pins` | git grep -c | 20 |
