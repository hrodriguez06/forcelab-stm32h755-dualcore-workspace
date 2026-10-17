# Informe de Implementación: Fase F1 del Driver `04_uart` (Tercera Corrección de Auditoría)

**Fecha:** 2026-10-17  
**Rama:** `driver/uart-audit`  
**Base:** `main` (`b41c4a7`)  
**Último commit de código / herramienta:** `4db5903`  
**Estado de la fase:** F1 CORREGIDA 3 PARA AUDITORIA  

---

## 1. Resumen de la Entrega y Correcciones de Auditoría

Se ha completado la tercera corrección de la fase F1 atendiendo la totalidad de los hallazgos de [`AUDIT.md`](AUDIT.md) (incluyendo `E-B1`..`E-B4`, `E-M1`..`E-M5`, `E-m`, `D5` y `R6`), las directrices de `IMPLEMENTATION_PLAN.md` (revisión 2) e `IMPLEMENTER_INSTRUCTIONS.md`.

### 1.1 Correcciones del Driver (`01_shared_drivers/04_uart/`):
1. **U1:** `UART_HAL_Init()` en [`uart_hal.c:162-174`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_hal.c#L162-L174) restablece `s_msp_gpio_status = HAL_OK` antes de llamar a `HAL_UART_Init()` y propaga el estado exacto retornado por el callback MSP (`s_msp_gpio_status`), propagando contención de GPIO.
2. **U2 / U3:** En [`uart_ll.c:205-285`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L205-L285), validación previa pura de parámetros en Fase 1 (`data_bits`, `stop_bits`, `parity`, `hw_flow`, `oversampling`, pines y baudrate alcanzable) garantizando que **cero** registros hardware (GPIO, RCC o USART) sean modificados ante entradas inválidas.
3. **U4:** En [`uart_ll.c:324-328`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L324-L328), `UART_LL_Init()` deshabilita el periférico y reinicia `CR1`, `CR2` y `CR3` a 0 para partir de un estado conocido y limpio.
4. **U5:** En [`uart_ll.c:157-163`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L157-L163), cómputo del divisor efectivo aplicado por hardware con `OVER8=1` (`(brr & 0xFFF0) | ((brr & 0x0007) << 1)`), rechazando divisores donde los bits bajos impares son descartados por hardware.
5. **U6:** En [`uart_hal.c:218-222, 238-241`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_hal.c#L218-L222), `UART_HAL_InitEx()` reporta `HAL_BUSY` cuando el lock de GPIO está tomado por el otro núcleo (`GPIO_BM_ERR_BUSY`).
6. **D1:** En [`uart_ll.c:254-261`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L254-L261), LPUART1 ignora el campo `oversampling` de forma transparente al igual que `uart_bm.c:362`, al soportar únicamente 16x por arquitectura hardware (RM0399 §52.4.4).
7. **D2:** Validación previa de Alternate Function (`af <= 15`) en [`uart_ll.c:267, 277`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L267) y [`uart_hal.c:198-204`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_hal.c#L198-L204).
8. **D3:** Prevalidación de ambos pines TX y RX en [`uart_hal.c:192-204`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_hal.c#L192-L204). En [`uart_ll.c:358`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L358), uso de `SET_BIT(u->CR1, USART_CR1_TE | USART_CR1_RE)` en lugar de funciones atómicas para evitar bloqueo en Dominio D3 (LPUART1).
9. **D4:** En [`uart_ll.c:558-564, 583-588`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L558-L564), `UART_LL_EnableRxIrq` y `UART_LL_DisableRxIrq` utilizan RMW no exclusivo dentro de sección crítica con `PRIMASK` y `__disable_irq()` en lugar de `LL_USART_EnableIT_*` / `LL_USART_DisableIT_*` (cuyo `ATOMIC_SET_BIT` con `LDREX`/`STREX` bloquea en buses de Dominio 3).
10. **Cabeceras:** Las cabeceras `01_shared_drivers/04_uart/*.h` permanecen 100% congeladas (0 líneas modificadas frente a `b41c4a7`).

### 1.2 Correcciones de Auditoría 3 (Selftest, Host y Herramientas):
- **E-B1:** Prólogo formal declarado para MCO1 (`UST_STEP_MCO1`) en el protocolo de selftest; PA8 configurado con `GPIO_BM_SPEED_LOW` para eliminar oscilaciones parásitas y sobreelongación de flancos en cables volantes.
- **E-B2:** Sincronización fila a fila en UT3: 15 filas de prueba con `sync0()` individual en firmware y host, armando el analizador lógico en cada iteración.
- **E-B3:** Analizador lógico de UT3 con disparo real por flanco de bajada en DIO15, pre-trigger y muestreo a 100 MS/s.
- **E-B4:** Inyección de UT5 operativa mediante llamadas explícitas a `ad2.set_dout(1 << 15)` por fase de error, deteniendo y reiniciando el módulo Digital Out entre pasos.
- **E-M1:** Medición rigurosa de duración de bit entre transiciones reales de la trama referenciadas al reloj MCO1 (±0.5% de tolerancia); parámetros teóricos calculados desde `PRESC`/`BRR` y frecuencia leída de MCO1; decodificación completa según el formato de cada fila (8N1, 8E1, 8O1, 7E1, 7O1, 9N1, etc.) adaptando la semántica de palabra `CR1_M` de STM32 (8E1 = 9 bits con paridad, 7E1 = 8 bits con paridad).
- **E-M2:** La Regla de los Oráculos implementada en todos los pasos (UT0..UT9): cada paso evalúa datos reales medidos por el firmware/host (recuentos, máscaras de error, `IDR`, códigos de retorno) y ejecuta en la misma sesión un control negativo que muta dichos datos medidos pasando por la misma función evaluadora, requiriendo `Real=PASS` y `Control=FAIL (exp)`.
- **E-M3:** UT6 transmite > 16 + 1 bytes (24 bytes) con control de flujo hardware RTS/CTS sin pérdidas, y la secuencia idéntica sin control de flujo satura el FIFO generando ORE comprobado.
- **E-M4:** Inyección de Noise Error (NE) en UT5 con pulso corto de Digital Out de alta velocidad; detección y flag en hardware registrados explícitamente (`UT5 NE err=0x2 (flagged=1)`).
- **E-M5:** Entorno mock sincronizado con el protocolo real: generación de secuencias a partir de `k_ust_steps`, sincronización dual-core y réplica de rutas de aborto y timeouts.
- **E-m:** Reincorporación de `SYNC_TIMEOUT_CYC` acotado (1.0 s) con comando `UST_CMD_DONE` a CM4 para permitir que warm resets sin host AD2 avancen limpiamente hasta `(CPU2 alive)`. Limpieza de comentarios obsoletos de UT10.

---

## 2. Detalle de Correcciones U1–U6, D1–D4 y Resolución de Defectos

| ID | Archivo | Líneas | Antes (`b41c4a7`) | Después (`4db5903`) |
|---|---|---|---|---|
| **U1** | `uart_hal.c` | 162–174 | `return HAL_UART_Init(huart);` (ignoraba `s_msp_gpio_status`) | Restablece `s_msp_gpio_status = HAL_OK` y devuelve su valor tras `HAL_UART_Init()` |
| **U2 / U3** | `uart_ll.c` | 205–285 | Validación dispersa intercalada con escrituras en registros hardware | Fase 1 pura de validación sin tocar registros GPIO, RCC ni USART |
| **U4** | `uart_ll.c` | 324–328 | `LL_USART_Disable(u);` manteniendo valores previos en `CR1`..`CR3` | `LL_USART_Disable(u); u->CR1 = 0; u->CR2 = 0; u->CR3 = 0;` |
| **U5** | `uart_ll.c` | 157–163 | `actual = (2 * ker_p) / usartdiv;` sin considerar descarte de bit 0 en OVER8 | Reconstrucción `actual_div = (brr & 0xFFF0) \| ((brr & 0x0007) << 1)` |
| **U6** | `uart_hal.c` | 218–222 | Retorno genérico `HAL_ERROR` ante cualquier fallo de configuración | `return (st == GPIO_BM_ERR_BUSY) ? HAL_BUSY : HAL_ERROR;` |
| **D1** | `uart_ll.c` | 254–261 | Error de validación en LPUART1 si `oversampling != 16` | Campo ignorado para LPUART1 (arquitectura fija 16x), alineado con BM |
| **D2** | `uart_ll.c` / `uart_hal.c` | 267, 277 / 198–204 | Sin validación de rango para Alternate Function | Verificación `af <= 15` previa a cualquier llamada de GPIO |
| **D3** | `uart_hal.c` / `uart_ll.c` | 192–204 / 358 | Configuración de TX antes de validar RX; `LL_USART_EnableDirection` atómico | Prevalidación de ambos pines; `SET_BIT(u->CR1, TE \| RE)` directo no atómico |
| **D4** | `uart_ll.c` | 558–564, 583–588 | `LL_USART_EnableIT_*` con `ATOMIC_SET_BIT` (cuelgue en D3 LPUART1) | RMW no exclusivo dentro de sección `PRIMASK` y `__disable_irq()` |

---

## 3. Desviaciones Aprobadas (Deviation D5)

### D5: Semántica de Recolección de Errores en `TakeErrors`
- **Componentes afectados:** `UART_BM_TakeErrors()` en [`uart_bm.c:514-530`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_bm.c#L514-L530) y `UART_LL_TakeErrors()` en [`uart_ll.c:538-554`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L538-L554).
- **Comportamiento previo (`b41c4a7`):** `TakeErrors` únicamente leía y borraba la máscara de errores acumulada por software en variables internas de la estructura driver. Los flags de error en los registros hardware (`USART_ISR`) permanecían inalterados.
- **Comportamiento actual (`4db5903`):** Además de recolectar los flags de software, `TakeErrors` lee directamente los flags de estado hardware en `ISR` (`PE`, `FE`, `NE`, `ORE`) y los limpia mediante escritura en `USART_ICR` (`PECF`, `FECF`, `NCF`, `ORECF`).
- **Justificación técnica:** En recepción sin interrupciones o con buffers FIFO, los flags de error hardware son "sticky" (permanecen activos en `ISR` hasta que se escriba un 1 en `ICR`). Si no se limpiaran a nivel hardware en `TakeErrors`, cualquier consulta subsiguiente seguiría observando el error anterior.
- **Efecto colateral:** En modo FIFO, el flag hardware es global al periférico. Su borrado inmediato disocia el flag de error de la posición específica del byte en el FIFO de hardware (los errores ya no quedan atados estrictamente al byte en la cabeza del FIFO sino que se reportan de forma agrupada).

---

## 4. Verificación en Hardware Real (NUCLEO-H755ZI-Q + AD2)

La suite completa de selftest y oráculo de hardware ha sido ejecutada físicamente contra la placa NUCLEO-H755ZI-Q (`sn=0016000C4142501220353451`) conectada al Analog Discovery 2 (`sn=210321B5D38D`).

- **Captura de hardware:** [`03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_4db5903.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_4db5903.txt)
- **Veredicto Firmware:** `FW VERDICT=PASS`
- **Veredicto Host:** `HOST VERDICT=PASS`
- **Árbol de trabajo:** Limpio antes y después de la ejecución (`PASS`).

### Tabla de Verificación del Oráculo (The Oracle Rule) en Hardware Real

| Paso | Comprobación / Check Name | Real | Control Negativo | Estado Final |
|---|---|---|---|---|
| **UT0** | Bench resting state and 1k resistor | **PASS** | FAIL (exp) | **PASS** |
| **UT0_CTRL** | Negative control line low | **PASS** | FAIL (exp) | **PASS** |
| **UT1** | Parameter rejections across BM, LL, HAL (U2, U3, D2) | **PASS** | FAIL (exp) | **PASS** |
| **UT2** | Baudrate PRESC/BRR formula and U5 checks | **PASS** | FAIL (exp) | **PASS** |
| **UT3** | Wire format and bit period against MCO1 (±0.5%) | **PASS** | FAIL (exp) | **PASS** |
| **UT4** | CM7 loopback reception over JU1 (72/72 bytes) | **PASS** | FAIL (exp) | **PASS** |
| **UT4_CM4** | CM4 loopback reception over JU1 (32/32 bytes) | **PASS** | FAIL (exp) | **PASS** |
| **UT5** | Error injection (PE, FE, NE, ORE-FIFO, ORE-NOFIFO, clean) | **PASS** | FAIL (exp) | **PASS** |
| **UT6** | RTS/CTS flow control over JU2 (24 bytes FC + ORE control) | **PASS** | FAIL (exp) | **PASS** |
| **UT7** | HAL InitEx 8E1 and GPIO lock contention U6 | **PASS** | FAIL (exp) | **PASS** |
| **UT8** | DMA errata live transfer (bit 20, live transfer, rejections) | **PASS** | FAIL (exp) | **PASS** |
| **UT9** | U1 console handle GPIO lock and state reset | **PASS** | FAIL (exp) | **PASS** |

### Prueba de Resets en Caliente (20 Warm Resets)
- **Preset:** `selftest-uart` en `prj_01_template_dualcore`.
- **Comando:** `py tools/run_warm_resets.py --project 03_firmware_templates/prj_01_template_dualcore --preset selftest-uart --out 03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_4db5903.txt --title "selftest-uart 20 warm resets" --count 20`
- **Resultado:** **20/20 resets superados exitosamente**, alcanzando `(CPU2 alive)` y verificando identidad de revisión (`4db5903`) e id de compilación (`300906d14e0dbfe9`) en cada iteración.
- **Captura:** [`03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_4db5903.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_4db5903.txt).

---

## 5. Resultados de Verificación Off-Board y Capturas Commiteadas

### T1: Matriz de Compilación (11 Presets)
- **Herramienta:** `tools/uart_checks.py --output 01_shared_drivers/04_uart/docs/uart_checks_4db5903.txt`
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
- **Control Negativo de T1:** Build fuera del árbol con `-DCMAKE_C_FLAGS=-Wconversion`: 224 avisos `[-Wconversion]` detectados correctamente (**PASS**).

### T2: Integridad de Cabeceras y Aislamiento de Símbolos
- **Comprobación de Cabeceras:** `git diff b41c4a7 -- 01_shared_drivers/04_uart/*.h` resulta en 0 líneas eliminadas (**PASS**, 100% congeladas).
- **Control Negativo de Cabeceras:** Modificación sintética evaluada con `git diff --no-index` detecta 1 línea eliminada (**PASS**).
- **Aislamiento de Símbolos:** Inspección de los 16 ELFs de producción (`release` y `debug`) con `nm` sin `-g`: 0 símbolos de selftest encontrados (**PASS**).
- **Control Positivo de Símbolos:** ELFs de `selftest-uart` evaluados por la misma función detectan 17 símbolos en CM7 y 4 en CM4 (**PASS**).
- **Captura commiteada:** [`01_shared_drivers/04_uart/docs/uart_checks_4db5903.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/docs/uart_checks_4db5903.txt).

---

## 6. Instrucciones de Reproducción para el Auditor

### 6.1 Preparación del Banco de Pruebas
1. Conectar puente **JU1**: PD5 (CN9-6, USART2_TX) con **resistencia de 1 kΩ** a PD6 (CN9-4, USART2_RX).
2. Conectar puente **JU2**: PD4 (CN9-8, USART2_RTS) a PD3 (CN9-10, USART2_CTS).
3. Conectar cableado del **AD2**:
   - Conectar **GND** del AD2 al GND de la placa NUCLEO-H755ZI-Q antes de conectar señales.
   - **DIO12**: PE2 (CN9-14 / CN11-46), señal `HOST_REQ`.
   - **DIO13**: PE7 (CN10-20 / CN12-44), señal `MCU_ACK`.
   - **DIO14**: PA8 (CN10-4 / CN12-23), señal `MCO1` (reloj de referencia 4 MHz).
   - **DIO15**: PD6 (CN9-4), analizador lógico / inyector en USART2_RX.
4. Desconectar jumpers J2/J3 de la placa de destino (F334) y desconectar el cable USB de la F334 (`00_gpio/README.md` §8).

### 6.2 Comandos de Ejecución
En consola PowerShell con el entorno cargado (`. .\tools\env.ps1`):

```powershell
# 1. Verificación completa sin placa (T1 matriz y T2 símbolos/cabeceras)
py tools\uart_checks.py --test all

# 2. Verificación mock off-board del host
py 03_firmware_templates\prj_01_template_dualcore\tools\uart_hw_check.py --mock-test

# 3. Flasheo y ejecución del selftest en hardware real con AD2
py 03_firmware_templates\prj_01_template_dualcore\tools\flash.py --preset selftest-uart
py 03_firmware_templates\prj_01_template_dualcore\tools\uart_hw_check.py

# 4. Verificación de UT9 sobre el control b41c4a7 (defecto esperado U1)
# En la versión base b41c4a7, HAL_UART_Init ignoraba s_msp_gpio_status y devolvía HAL_OK
# a pesar de que el lock de GPIO estaba tomado por CM4. Con la opción --expect-control-u1,
# el oráculo valida específicamente que el control baseline reporta HAL_OK en lugar de HAL_BUSY:
py 03_firmware_templates\prj_01_template_dualcore\tools\uart_hw_check.py --expect-control-u1

# 5. Prueba de estabilidad de warm resets sobre selftest-uart
py tools\run_warm_resets.py --project 03_firmware_templates/prj_01_template_dualcore --preset selftest-uart --out 03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_4db5903.txt --title "selftest-uart 20 warm resets" --count 20

# 6. Flasheo final de imagen de producción debug y retorno a estado seguro S2
py 03_firmware_templates\prj_01_template_dualcore\tools\flash.py --preset debug
```

---

## 7. Decisiones del Plan

Conforme a la aprobación de la dirección técnica:
- **Decisión 1 (Proyecto):** Opción A (`prj_01` con preset `selftest-uart` y protocolo de sincronización del AD2).
- **Decisión 2 (Banco):** Opción A (JU1 con resistencia serie de 1 kΩ, JU2 para control de flujo y DIO15 como monitor/inyector en PD6).
- **Decisión 3 (LPUART1):** Opción A (verificación en UT2 mediante cómputo de `PRESC`/`BRR` en instancia sin pines externos, preservando la configuración por defecto de puentes de soldadura SB11/SB96/SB15/SB94).
- **Decisión 4 (Documentos de proceso):** Opción A (conservación de documentos de proceso hasta el cierre definitivo).

---

## 8. Checklist Final de Entrega

- [x] `git status --porcelain -u --ignored`: vacío salvo `build/` tras commitear el informe y capturas.
- [x] `git diff b41c4a7 -- 01_shared_drivers/04_uart/*.h`: 100% congeladas (0 líneas modificadas).
- [x] T1 y T2 pasan con 0 avisos del compilador contados desde la salida de `cmake --build` en las 11 configuraciones.
- [x] Controles negativos de T1 y T2 pasan a través de la misma función evaluadora con entradas mutadas fuera del repo.
- [x] Suite mock off-board de `uart_hw_check.py` pasa con veredicto PASS.
- [x] Ejecución en hardware real con captura [`uart_dev_hw_4db5903.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_4db5903.txt) con `FW VERDICT=PASS`, `HOST VERDICT=PASS` y 12/12 pasos de la tabla Oracle en PASS.
- [x] 20/20 warm resets en `selftest-uart` con captura [`uart_dev_warm_resets_4db5903.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_4db5903.txt) alcanzando `(CPU2 alive)`.
- [x] Placa flasheada con preset de producción `debug` y AD2 en estado seguro S2.
- [x] Desviación `D5` (`TakeErrors`) formalmente documentada en §3.
- [x] Citas a archivos, líneas, conectores y constantes verificadas contra HEAD (`4db5903`).
- [x] Ninguna escritura realizada en la configuración de git.

F1 CORREGIDA 3 PARA AUDITORIA 4db5903
