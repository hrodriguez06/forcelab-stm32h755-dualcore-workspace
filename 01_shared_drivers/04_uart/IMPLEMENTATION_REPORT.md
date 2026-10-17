# Informe de Implementación: Fase F1 del Driver `04_uart` (Segunda Corrección de Auditoría)

**Fecha:** 2026-10-17  
**Rama:** `driver/uart-audit`  
**Base:** `main` (`b41c4a7`)  
**Último commit de código / herramienta:** `474f514`  
**Estado de la fase:** F1 CORREGIDA 2 PARA AUDITORIA  

---

## 1. Resumen de la Entrega y Correcciones de Auditoría

Se ha completado la fase F1 atendiendo la totalidad de los hallazgos de [`AUDIT.md`](AUDIT.md), las directrices de [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 2) e [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md):

1. **Correcciones del Driver (`01_shared_drivers/04_uart/`):**
   - **U1:** `UART_HAL_Init()` en [`uart_hal.c:162-174`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_hal.c#L162-L174) restablece `s_msp_gpio_status = HAL_OK` antes de llamar a `HAL_UART_Init()` y devuelve el estado devuelto por el callback MSP (`s_msp_gpio_status`), propagando fallos de contención de GPIO.
   - **U2 / U3:** En [`uart_ll.c:205-285`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L205-L285), validación previa pura de parámetros en Fase 1 (`data_bits`, `stop_bits`, `parity`, `hw_flow`, `oversampling`, pines y baudrate alcanzable) garantizando que **cero** registros hardware (GPIO, RCC o USART) sean modificados ante entradas inválidas.
   - **U4:** En [`uart_ll.c:324-328`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L324-L328), `UART_LL_Init()` deshabilita el periférico y reinicia `CR1`, `CR2` y `CR3` a 0 para partir de un estado conocido y limpio.
   - **U5:** En [`uart_ll.c:157-163`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L157-L163), cómputo del divisor efectivo aplicado por hardware con `OVER8=1` (`(brr & 0xFFF0) | ((brr & 0x0007) << 1)`), rechazando divisores donde los bits bajos impares son descartados por hardware.
   - **U6:** En [`uart_hal.c:218-222, 238-241`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_hal.c#L218-L222), `UART_HAL_InitEx()` reporta `HAL_BUSY` cuando el lock de GPIO está tomado por el otro núcleo (`GPIO_BM_ERR_BUSY`).
   - **D1:** En [`uart_ll.c:254-261`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L254-L261), LPUART1 ignora el campo `oversampling` de forma transparente al igual que `uart_bm.c:362`, al soportar únicamente 16x por arquitectura hardware (RM0399 §52.4.4).
   - **D2:** Validación previa de Alternate Function (`af <= 15`) en [`uart_ll.c:267, 277`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L267) y [`uart_hal.c:198-204`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_hal.c#L198-L204).
   - **D3:** Prevalidación de ambos pines TX y RX en [`uart_hal.c:192-204`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_hal.c#L192-L204). En [`uart_ll.c:358`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L358), uso de `SET_BIT(u->CR1, USART_CR1_TE | USART_CR1_RE)` en lugar de funciones atómicas para evitar bloqueo en Dominio D3 (LPUART1).
   - **D4:** En [`uart_ll.c:558-564, 583-588`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L558-L564), `UART_LL_EnableRxIrq` y `UART_LL_DisableRxIrq` utilizan RMW no exclusivo protegido por `__disable_irq()` y `PRIMASK` en lugar de `LL_USART_EnableIT_*` / `LL_USART_DisableIT_*` (cuyo `ATOMIC_SET_BIT` con `LDREX`/`STREX` bloquea en buses de Dominio 3).
   - **Las cabeceras `01_shared_drivers/04_uart/*.h` permanecen 100% congeladas (0 líneas modificadas frente a `b41c4a7`).**

2. **Selftest de Firmware (`prj_01` `selftest-uart`):**
   - Pasos reorganizados en UT0..UT9 eliminando el paso redundante UT10.
   - Enlace y cita formal de la región compartida en D3 SRAM4 (`0x3800E000` dentro de `RAM_SHARED: ORIGIN = 0x38008000, LENGTH = 32K` según `stm32h755zi_cm7.ld:43` y `stm32h755zi_cm4.ld:34`).
   - Todos los accesos y configuraciones de pines pasan por `bench_claim()` conforme a las reglas de seguridad.
   - Verificación de instantáneas de los 11 registros (`CR1`..`CR3`, `BRR`, `PRESC`, `MODER`, `AFR`, `PUPDR`, `RCC_APB1LENR`) antes y después de cada rechazo en UT1.
   - Bounded spins con timeouts en todos los bucles de transmisión y recepción.
   - Restauración de MCO1 a estado de entrada pasiva al finalizar o abortar la sesión.

3. **Herramientas de Host y Simulación:**
   - [`tools/uart_checks.py`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/tools/uart_checks.py): verificación completa de T1 (11 configuraciones compiladas con `cmake --build --clean-first`, 0 avisos del compilador, control negativo con `-DCMAKE_C_FLAGS=-Wconversion` generando 224 avisos) y T2 (integridad de cabeceras, 16 ELFs de producción sin símbolos de selftest y control positivo verificando detección en `selftest-uart`).
   - [`03_firmware_templates/prj_01_template_dualcore/tools/uart_hw_check.py`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/tools/uart_hw_check.py): oráculo completo con suite de pruebas mock off-board (`--mock-test`), guardas SU1/S11, decodificador de tramas UART referenciado a MCO1 a 20 MS/s y evaluación independiente con oráculos negativos.

---

## 2. Detalle de Correcciones U1–U6 y D1–D4

| ID | Archivo | Líneas | Antes (`b41c4a7`) | Después (`474f514`) |
|---|---|---|---|---|
| **U1** | `uart_hal.c` | 162–174 | `return HAL_UART_Init(huart);` (ignoraba `s_msp_gpio_status`) | Restablece `s_msp_gpio_status = HAL_OK` y devuelve su valor tras `HAL_UART_Init()` |
| **U2 / U3** | `uart_ll.c` | 205–285 | Validación dispersa intercalada con escrituras en registros hardware | Fase 1 pura de validación sin tocar registros GPIO, RCC ni USART |
| **U4** | `uart_ll.c` | 324–328 | `LL_USART_Disable(u);` manteniendo valores previos en `CR1`..`CR3` | `LL_USART_Disable(u); u->CR1 = 0; u->CR2 = 0; u->CR3 = 0;` |
| **U5** | `uart_ll.c` | 157–163 | `actual = (2 * ker_p) / usartdiv;` sin considerar descarte de bit 0 en OVER8 | Reconstrucción `actual_div = (brr & 0xFFF0) \| ((brr & 0x0007) << 1)` |
| **U6** | `uart_hal.c` | 218–222 | Retorno genérico `HAL_ERROR` ante cualquier fallo de configuración | `return (st == GPIO_BM_ERR_BUSY) ? HAL_BUSY : HAL_ERROR;` |
| **D1** | `uart_ll.c` | 254–261 | Error de validación en LPUART1 si `oversampling != 16` | Campo ignorado para LPUART1 (arquitectura fija 16x), alineado con BM |
| **D2** | `uart_ll.c` / `uart_hal.c` | 267, 277 / 198–204 | Sin validación de rango para Alternate Function | Verificación `af <= 15` previa a cualquier llamada de GPIO |
| **D3** | `uart_hal.c` / `uart_ll.c` | 192–204 / 358 | Configuración de TX antes de validar RX; `LL_USART_EnableDirection` atómico | Prevalidación de ambos pines; `SET_BIT(u->CR1, TE \| RE)` directo no atómico |
| **D4** | `uart_ll.c` | 558–564, 583–588 | `LL_USART_EnableIT_*` con `ATOMIC_SET_BIT` (cuelgue en D3 LPUART1) | RMW no exclusivo protegido por `PRIMASK` y `__disable_irq()` |

---

## 3. Selftest de Firmware (UT0–UT9)

> **Nota:** Siguiendo la prohibición expresa de ejecución contra hardware real durante la fase F1 del implementador, los pasos UT0–UT9 en hardware real quedan catalogados como **NO VERIFICADOS** en este informe. La verificación del implementador se ha realizado de forma autónoma mediante la suite de simulación mock off-board (`uart_hw_check.py --mock-test`) y las herramientas de validación de compilación y símbolos (`tools/uart_checks.py`).

| Paso | Comprobación Firmware / Host | Oráculo / Comprobación Host | Control Negativo | Estado en Hardware Real |
|---|---|---|---|---|
| **UT0** | Reposo del banco con JU1 (1 kΩ): PD5 en entrada pull-up, PD6 alto | Host comprueba nivel alto inicial en DIO15, nivel bajo temporal al activar OE del AD2, y restauración | Entrada mutada forzando nivel bajo inicial | **NO VERIFICADO** (simulado en mock: PASS) |
| **UT0_CTRL** | Control negativo con MCU conduciendo PD5 a 0 mediante GPIO | Host comprueba que DIO15 lee nivel bajo | Entrada mutada forzando nivel alto | **NO VERIFICADO** (simulado en mock: PASS) |
| **UT1** | Rechazo de parámetros inválidos en BM, LL y HAL sin alterar registros | Instantánea de 11 registros antes y después de cada caso de rechazo | Caso válido de control que altera la instantánea | **NO VERIFICADO** (simulado en mock: PASS) |
| **UT2** | Cómputo de divisores `PRESC` y `BRR` en UART4, UART5, UART8, USART1, USART6, LPUART1 | Oráculo matemático Python con fórmula independiente de STM32 | Fila con BRR corrupto evaluada por la misma función | **NO VERIFICADO** (simulado en mock: PASS) |
| **UT3** | Formato en cable a 115200 baud medido contra MCO1 (PA8 / DIO14) | Captura de LA a 20 MS/s decodificando inicio, 8 bits de datos y parada; frecuencia MCO1 ±0.5% | Decodificación esperando paridad invertida o byte erróneo | **NO VERIFICADO** (simulado en mock: PASS) |
| **UT4** | Bucle JU1 en CM7 (polled, IRQ con ring buffer, FIFO, OVER8, máscara 7 bits) | Verificación de 64/64 bytes correctos en firmware y confirmación por host | Recuento mutado de bytes recibidos | **NO VERIFICADO** (simulado en mock: PASS) |
| **UT4_CM4** | Bucle JU1 en CM4 subordinado vía comando IPC `UST_CMD_UT4` | Confirmación de ejecución y recepción en CM4 | Veredicto mutado a fallo | **NO VERIFICADO** (simulado en mock: PASS) |
| **UT5** | Inyección de errores temporizados en DIO15 (PE, FE, BREAK, NE, ORE-FIFO, ORE-NOFIFO, trama limpia) | Verificación de recolección de errores en firmware y confirmación por host | Trama limpia que no debe generar flags de error | **NO VERIFICADO** (simulado en mock: PASS) |
| **UT6** | Control de flujo hardware RTS/CTS vía JU2 (PD4 -> PD3) con FIFO | 20 bytes transmitidos sin ORE con FC; secuencia idéntica sin FC produce ORE | Secuencia con FC donde se omite el ORE en control negativo | **NO VERIFICADO** (simulado en mock: PASS) |
| **UT7** | `UART_HAL_InitEx` 8E1, instantánea de pines PD5/PD6 y contención de lock GPIO (U6) | Host verifica que con lock tomado por CM4 retorna `HAL_BUSY`, y con lock libre `HAL_OK` | Retorno mutado de lock libre con lock tomado | **NO VERIFICADO** (simulado en mock: PASS) |
| **UT8** | Workaround errata DMA ES0445 en vivo con DMA1 Stream 0 y rechazos de configuración | Verificación de bit 20 en `DMA_SxCR`, transferencia correcta y rechazo de streams inválidos | Bit 20 mutado a 0 | **NO VERIFICADO** (simulado en mock: PASS) |
| **UT9** | Consola `UART_HAL_Init` reportando estado de lock GPIO (U1) | Retorno `HAL_BUSY` (2) con lock tomado, `HAL_OK` (0) con lock liberado | Retorno `HAL_OK` con lock tomado (defecto original de b41c4a7) | **NO VERIFICADO** (simulado en mock: PASS) |
| **DONE** | Fin de sesión y restauración de estado seguro | Host comprueba finalización ordenada de selftest y restauración de pines | — | **NO VERIFICADO** (simulado en mock: PASS) |

---

## 4. Resultados de Verificación Off-Board y Capturas Commiteadas

### T1: Matriz de Compilación (11 Presets)
- **Herramienta:** `tools/uart_checks.py --test all`
- **Resultados:** 11/11 presets compilados con `cmake --build --clean-first`, 0 errores, 0 avisos del compilador:
  - `prj_01_template_dualcore` [`debug`]: **PASS** (0 warnings, 274 objetos).
  - `prj_01_template_dualcore` [`release`]: **PASS** (0 warnings, 274 objetos).
  - `prj_02_eth_lwip_dualcore` [`debug`]: **PASS** (0 warnings, 318 objetos).
  - `prj_02_eth_lwip_dualcore` [`release`]: **PASS** (0 warnings, 318 objetos).
  - `prj_03_adc_dma_eth` [`debug`]: **PASS** (0 warnings, 326 objetos).
  - `prj_03_adc_dma_eth` [`release`]: **PASS** (0 warnings, 326 objetos).
  - `prj_04_adc_dma_bm_eth` [`debug`]: **PASS** (0 warnings, 324 objetos).
  - `prj_04_adc_dma_bm_eth` [`release`]: **PASS** (0 warnings, 324 objetos).
  - `prj_01_template_dualcore` [`selftest-uart`]: **PASS** (0 warnings, 282 objetos).
  - `prj_01_template_dualcore` [`selftest-gpio`]: **PASS** (0 warnings, 281 objetos).
  - `prj_04_adc_dma_bm_eth` [`selftest-uart`]: **PASS** (0 warnings, 327 objetos).
- **Control Negativo de T1:** Build fuera del árbol con `-DCMAKE_C_FLAGS=-Wconversion`: 224 avisos `[-Wconversion]` / `[-Wsign-conversion]` detectados correctamente (**PASS**).

### T2: Integridad de Cabeceras y Aislamiento de Símbolos
- **Comprobación de Cabeceras:** `git diff b41c4a7 -- 01_shared_drivers/04_uart/*.h` resulta en 0 líneas eliminadas (**PASS**, 100% congeladas).
- **Control Negativo de Cabeceras:** Modificación sintética evaluada con `git diff --no-index` detecta 1 línea eliminada (**PASS**).
- **Aislamiento de Símbolos:** Inspección de los 16 ELFs de producción (`release` y `debug`) con `nm` sin `-g`: 0 símbolos de selftest encontrados (**PASS**).
- **Control Positivo de Símbolos:** ELFs de `selftest-uart` evaluados por la misma función detectan 17 símbolos en CM7 y 4 en CM4 (**PASS**).
- **Captura commiteada:** [`01_shared_drivers/04_uart/docs/uart_checks_474f514.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/docs/uart_checks_474f514.txt).

### Prueba del Host con Mock (`uart_hw_check.py --mock-test`)
- **Fase 1 (Guardas SU1):** 4/4 comprobaciones de guardas estáticas y Digital Out en AD2 superadas (**PASS**).
- **Fase 2 (Sesión completa UT0..UT9):** 12/12 pasos superados en simulación con tabla Oracle completa (**PASS**).
- **Fase 3 (Control Negativo 1):** Detección de paridad mala en el decodificador de cable (**PASS**).
- **Fase 4 (Control Negativo 2):** Intento de activación de OE en DIO15 fuera de UT0 capturado por guarda (**PASS**).
- **Fase 5 (Control Negativo 3):** Detección de subconteo de tramas (3 vs 6) (**PASS**).
- **Veredicto global:** `OVERALL MOCK VERDICT: PASS`, `HOST VERDICT=PASS`.
- **Captura commiteada:** [`03_firmware_templates/prj_01_template_dualcore/docs/uart_mock_474f514.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/uart_mock_474f514.txt).

---

## 5. Instrucciones de Reproducción para el Auditor

### 5.1 Preparación del Banco de Pruebas
1. Conectar puente **JU1**: PD5 (CN9-6, USART2_TX) con **resistencia de 1 kΩ** a PD6 (CN9-4, USART2_RX).
   > *Importante:* Si existe un cable directo previo PD5–PD6 sin resistencia, sustituirlo por JU1 con 1 kΩ para proteger el pin durante las inyecciones de UT5.
2. Conectar puente **JU2**: PD4 (CN9-8, USART2_RTS) a PD3 (CN9-10, USART2_CTS).
3. Conectar cableado del **AD2**:
   - Conectar **GND** del AD2 al GND de la placa NUCLEO-H755ZI-Q antes de conectar señales.
   - **DIO12**: PE2 (CN9-14 / CN11-46), señal `HOST_REQ`.
   - **DIO13**: PE7 (CN10-20 / CN12-44), señal `MCU_ACK`.
   - **DIO14**: PA8 (CN10-4 / CN12-23), señal `MCO1` (reloj de referencia).
   - **DIO15**: PD6 (CN9-4), analizador lógico / inyector en USART2_RX.
4. Desconectar jumpers J2/J3 de la placa de destino (F334) y desconectar el cable USB de la F334 (`00_gpio/README.md` §8).

### 5.2 Comandos de Ejecución
En consola PowerShell con el entorno cargado (`. .\tools\env.ps1`):

```powershell
# 1. Verificación completa sin placa (T1 matriz y T2 símbolos/cabeceras)
py tools\uart_checks.py --test all

# 2. Verificación mock off-board del host
py 03_firmware_templates\prj_01_template_dualcore\tools\uart_hw_check.py --mock-test

# 3. Flasheo y ejecución del selftest en hardware real con AD2 (dos pasadas)
cd 03_firmware_templates\prj_01_template_dualcore
py tools\flash.py --preset selftest-uart
py tools\uart_hw_check.py
py tools\uart_hw_check.py

# 4. Verificación de UT9 sobre el control b41c4a7 (defecto esperado U1)
py tools\uart_hw_check.py --expect-control-u1

# 5. Prueba de estabilidad de warm resets sobre imagen de producción debug
py tools\flash.py --preset debug
cd ..\..
py tools\run_warm_resets.py --project 03_firmware_templates/prj_01_template_dualcore --preset debug --out 03_firmware_templates/prj_01_template_dualcore/docs/warm_resets_audit.txt --title "04_uart F1 debug warm resets"
```

---

## 6. Decisiones del Plan

Conforme a la aprobación de la dirección técnica:
- **Decisión 1 (Proyecto):** Opción A (`prj_01` con preset `selftest-uart` y protocolo de sincronización del AD2).
- **Decisión 2 (Banco):** Opción A (JU1 con resistencia serie de 1 kΩ, JU2 para control de flujo y DIO15 como monitor/inyector en PD6).
- **Decisión 3 (LPUART1):** Opción A (verificación en UT2 mediante cómputo de `PRESC`/`BRR` en instancia sin pines externos, preservando la configuración por defecto de puentes de soldadura SB11/SB96/SB15/SB94).
- **Decisión 4 (Documentos de proceso):** Opción A (conservación de documentos de proceso hasta el cierre definitivo).

---

## 7. Checklist Final de Entrega

- [x] `git status --porcelain -u --ignored`: vacío salvo `build/` tras commitear el informe y capturas.
- [x] `git diff b41c4a7 -- 01_shared_drivers/04_uart/*.h`: 100% congeladas (0 líneas modificadas).
- [x] T1 y T2 pasan con 0 avisos del compilador contados desde la salida de `cmake --build`.
- [x] Controles negativos de T1 y T2 pasan a través de la misma función evaluadora con entradas mutadas fuera del repo.
- [x] Suite mock off-board de `uart_hw_check.py` pasa con veredicto PASS y detecta los tres fallos sintéticos.
- [x] Citas a archivos, líneas, conectores y constantes verificadas contra HEAD (`474f514`).
- [x] Ninguna escritura realizada en la configuración de git.

F1 CORREGIDA 2 PARA AUDITORIA 474f514
F1 ENTREGADA PARA AUDITORIA 474f514
