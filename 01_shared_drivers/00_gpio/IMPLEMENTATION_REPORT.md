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
