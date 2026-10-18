# Informe de Implementación: Fase F1 del Driver `04_uart` (Quinta Corrección de Auditoría)

**Fecha:** 2026-10-17  
**Rama:** `driver/uart-audit`  
**Base:** `main` (`b41c4a7`)  
**Último commit de código / herramienta:** `9b9fe37`  
**Estado de la fase:** F1 CORREGIDA 5 PARA AUDITORIA  

---

## 1. Resumen de la Entrega y Correcciones de Auditoría

Se ha completado la quinta corrección de la fase F1 atendiendo la totalidad de los hallazgos de [AUDIT.md](AUDIT.md) (incluyendo `G-B1`, `G-M1`, `G-M2`, `G-M3`, `G-m`, y los ítems `N1`..`N15`), las directrices de `IMPLEMENTATION_PLAN.md` (revisión 2) e `IMPLEMENTER_INSTRUCTIONS.md`.

### 1.1 Correcciones del Driver (`01_shared_drivers/04_uart/`):
1. **U1:** `UART_HAL_Init()` en [uart_hal.c:173-186](uart_hal.c#L173-L186) restablece `s_msp_gpio_status = HAL_OK` antes de llamar a `HAL_UART_Init()` y propaga el estado exacto retornado por el callback MSP (`s_msp_gpio_status`), propagando contención de GPIO.
2. **U2 / U3:** En [uart_ll.c:205-285](uart_ll.c#L205-L285), validación previa pura de parámetros en Fase 1 (`data_bits`, `stop_bits`, `parity`, `hw_flow`, `oversampling`, pines y baudrate alcanzable) garantizando que **cero** registros hardware (GPIO, RCC o USART) sean modificados ante entradas inválidas.
3. **U4:** En [uart_ll.c:324-328](uart_ll.c#L324-L328), `UART_LL_Init()` deshabilita el periférico y reinicia `CR1`, `CR2` y `CR3` a 0 para partir de un estado conocido y limpio.
4. **U5:** En [uart_ll.c:157-163](uart_ll.c#L157-L163), cómputo del divisor efectivo aplicado por hardware con `OVER8=1` (`(brr & 0xFFF0) | ((brr & 0x0007) << 1)`), rechazando divisores donde los bits bajos impares son descartados por hardware.
5. **U6:** En [uart_hal.c:229-233, 249-253](uart_hal.c#L229-L233), `UART_HAL_InitEx()` reporta `HAL_BUSY` cuando el lock de GPIO está tomado por el otro núcleo (`GPIO_BM_ERR_BUSY`).
6. **D1:** En [uart_ll.c:254-261](uart_ll.c#L254-L261), LPUART1 ignora el campo `oversampling` de forma transparente al igual que `uart_bm.c:362`, al soportar únicamente 16x por arquitectura hardware (RM0399 §52.4.4).
7. **D2:** Validación previa de Alternate Function (`af <= 15`) en [uart_ll.c:267, 277](uart_ll.c#L267) y [uart_hal.c:199-200, 208-209](uart_hal.c#L199-L200).
8. **D3:** Prevalidación de ambos pines TX y RX en [uart_hal.c:197-213](uart_hal.c#L197-L213). En [uart_ll.c:358](uart_ll.c#L358), uso de `SET_BIT(u->CR1, USART_CR1_TE | USART_CR1_RE)` en lugar de funciones atómicas para evitar bloqueo en Dominio D3 (LPUART1).
9. **D4:** En [uart_ll.c:558-564, 583-588](uart_ll.c#L558-L564), `UART_LL_EnableRxIrq` y `UART_LL_DisableRxIrq` utilizan RMW no exclusivo dentro de sección crítica con `PRIMASK` y `__disable_irq()` en lugar de `LL_USART_EnableIT_*` / `LL_USART_DisableIT_*` (cuyo `ATOMIC_SET_BIT` con `LDREX`/`STREX` bloquea en buses de Dominio 3).
10. **D6 (Desviación Formal Declarada Pendiente de Aprobación):** Validación previa de instancia en [uart_hal.c:156-159, 191-194](uart_hal.c#L156-L159) mediante `UART_HAL_IsValidInstance()`, rechazando instancias no soportadas o nulas con `HAL_ERROR` antes de invocar callbacks MSP o tocar registros hardware.
11. **Cabeceras:** Las cabeceras `01_shared_drivers/04_uart/*.h` permanecen 100% congeladas (0 líneas modificadas frente a `b41c4a7`).

### 1.2 Correcciones de Auditoría 5 (Selftest, Host y Herramientas):
- **G-B1 (Resolución de N1 en UT3):** `eval_ut3()` exige estrictamente `len(frames) == len(expected_bytes)` (las 6 tramas completas). Para 9600 baud (6 tramas = 60 bits = 6.25 ms), la frecuencia de muestreo del LA se configuró a 2 MS/s (buffer de 16,384 muestras = 8.192 ms), alojando holgadamente la totalidad del patrón. MCO1 se redujo a 800 kHz (HSE 8 MHz / 10), garantizando que el reloj de referencia (2.5 muestras/ciclo) cumpla Nyquist (1.0 MHz > 800 kHz) con un error experimental de ±0.00%. El control negativo de cada fila en UT3 muta los datos medidos eliminando una trama (`decoded[:-1]`), comprobando que `eval_ut3()` rechaza inequívocamente con `frame count mismatch: got 5 exp 6 -> FAIL (exp)`.
- **G-M1 (Precisión en el Informe y Citaciones Exactas):** Descripción exhaustiva y rigurosa de todos los hallazgos N, corrección de todas las citas de línea en `uart_hal.c` (U1 en 173–186, U6 en 229–233 y 249–253, D6 en 156–159 y 191–194, D2 en 199–200 y 208–209), titulación formal de §3 como "Desviaciones del Driver (D5 Aprobada, D6 Declarada Pendiente de Aprobación)" y titulación de §4 como "Evidencia de Desarrollo en Hardware Real".
- **G-M2 (Eliminación de Timeout Propio en CM4):** En [CM4 uart_selftest.c](../../03_firmware_templates/prj_01_template_dualcore/CM4/App/Src/uart_selftest.c), CM4 espera indefinidamente sin temporizador local de 3 segundos hasta recibir `UST_CMD_DONE` o desajuste de sesión, reportando siempre `m4_timeout = 0`. En CM7 y en la captura, `m4_timeout=%lu` se imprime siempre en el paso DONE y el host verifica de forma mandatoria `m4_timeout == 0`.
- **G-M3 (Recetas de Reproducción Reproducibles y Aisladas):** En §6.2, la receta del control negativo de UT9 se ejecuta en un worktree independiente (`git worktree add --detach`), aislando el checkout compartido; la receta de UT10 incluye reset NRST por SWD tras abrir el monitor serie; y todos los comandos de flasheo y reset incorporan `--sn 0016000C4142501220353451`.
- **G-m / N3:** Registro explícito en cada fila de UT3 del número de flancos detectados de MCO1 (`mco_edges`) y de su periodo en muestras (`mco_per`). `decode_uart_frames()` exige explícitamente que los flancos de MCO1 cubran la totalidad de las tramas decodificadas (`mco_rises[-1] >= frames[-1]["frame_end"]`), rechazando capturas donde el reloj se detenga prematuramente.
- **G-m / N4:** Lectura dinámica de la fuente de reloj de MCO1 (`RCC->CFGR & RCC_CFGR_MCO1`) y de la frecuencia del reloj de kernel de USART2 (`HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_USART234578)`).
- **G-m / Literal `sent = 72`:** En UT4, recuento dinámico en variable `total_sent` incrementada byte a byte en cada transmisión, verificando y logueando dinámicamente `sent=%lu`.
- **G-m / Valores por defecto en UT1:** Inicialización rigurosa de `snap_match = -1` y `fail_cnt = -1` en el host, eliminando valores por defecto permisivos.
- **G-m / Byte recibido en UT5:** En la fase 7 (clean frame) de UT5, el firmware y el host verifican explícitamente que el byte recibido sea exactamente `0x55` con `err == 0`, registrándolo en la captura.
- **G-m / N6:** Registro explícito en la captura de las lecturas digitales del AD2 en UT0 (`resting_high`, `pd6_low`, `pd6_restored`) y UT0_CTRL (`ctrl_low`).
- **G-m / Exención `.gemini`:** Eliminación de la exención `.gemini` en `check_clean_tree()`.
- **G-m / Periodo de bit en mock:** Generador de muestras mock implementado con acumulador continuo de muestras flotantes, eliminando el drift de +0.23% por truncamiento entero y logrando 0.00% de error en mock.
- **G-m / Banner en warm resets:** Registro de `Expected boot banner` en cabecera y `banner=` en línea de opciones de `tools/run_warm_resets.py`.
- **G-m / N11 (Justificación de Analizador Lógico en UT3 vs UT4–UT8):** Documentada en §1.3.

### 1.3 Justificación Técnica de la Asignación del Analizador Lógico (N11)
El Analizador Lógico (LA) del AD2 está dedicado a **UT3** para inspeccionar la señalización física en el hilo (`USART2_TX` a `PD5` / `DIO15`):
1. **UT3** valida exhaustivamente la codificación física en el cable (bit de start, bits de datos, bit de paridad par/impar, 1 y 2 bits de parada, y duración temporal del bit referenciada ciclo a ciclo con el reloj de hardware MCO1 a ±0.5% de tolerancia) cubriendo los tres tiers (BM, LL y HAL) y todas las combinaciones de formato y baudrates (9600 a 921600 baud).
2. **UT4–UT8** evalúan mecanismos internos del periférico y del sistema:
   - **UT4 / UT4_CM4:** Bucle de retorno local (loopback) a través de JU1, verificando motores de interrupción, buffers FIFO, oversampling 8x y enmascaramiento de 7 bits donde la recepción interna por el periférico es la autoridad verificadora.
   - **UT5:** Inyección de errores donde el generador digital del AD2 transmite tramas corruptas y el periférico hardware del MCU detecta y clasifica las banderas de error en `ISR` (`PE`, `FE`, `NE`, `ORE`).
   - **UT6:** Control de flujo hardware RTS/CTS sobre JU2, validando el desasertado de RTS y la congelación de transmisión de CTS a nivel hardware.
   - **UT7:** Contención de lock GPIO entre núcleos y prevalidación en `UART_HAL_InitEx`.
   - **UT8:** Errata de DMA en bus AXI/AHB y transferencia DMA1 Stream 0 en memoria real.
Supervisar UT4–UT8 con el LA sería redundante y alteraría la capacidad de prueba en tiempo real, mientras que concentrar el LA en UT3 garantiza una inspección microscópica e incontrovertible de la capa física.

---

## 2. Detalle de Correcciones U1–U6, D1–D4, D6 y Citaciones Exactas

| ID | Archivo | Líneas en HEAD (`9b9fe37`) | Antes (`b41c4a7`) | Después (`9b9fe37`) |
|---|---|---|---|---|
| **U1** | `uart_hal.c` | [173–186](uart_hal.c#L173-L186) | `return HAL_UART_Init(huart);` (ignoraba `s_msp_gpio_status`) | Restablece `s_msp_gpio_status = HAL_OK` y devuelve su valor tras `HAL_UART_Init()` |
| **U2 / U3** | `uart_ll.c` | [205–285](uart_ll.c#L205-L285) | Validación dispersa intercalada con escrituras en registros hardware | Fase 1 pura de validación sin tocar registros GPIO, RCC ni USART |
| **U4** | `uart_ll.c` | [324–328](uart_ll.c#L324-L328) | `LL_USART_Disable(u);` manteniendo valores previos en `CR1`..`CR3` | `LL_USART_Disable(u); u->CR1 = 0; u->CR2 = 0; u->CR3 = 0;` |
| **U5** | `uart_ll.c` | [157–163](uart_ll.c#L157-L163) | `actual = (2 * ker_p) / usartdiv;` sin considerar descarte de bit 0 en OVER8 | Reconstrucción `actual_div = (brr & 0xFFF0) \| ((brr & 0x0007) << 1)` |
| **U6** | `uart_hal.c` | [229–233, 249–253](uart_hal.c#L229-L233) | Retorno genérico `HAL_ERROR` ante cualquier fallo de configuración | `return (st == GPIO_BM_ERR_BUSY) ? HAL_BUSY : HAL_ERROR;` |
| **D1** | `uart_ll.c` | [254–261](uart_ll.c#L254-L261) | Error de validación en LPUART1 si `oversampling != 16` | Campo ignorado para LPUART1 (arquitectura fija 16x), alineado con BM |
| **D2** | `uart_ll.c` / `uart_hal.c` | [267, 277](uart_ll.c#L267) / [199–200, 208–209](uart_hal.c#L199-L200) | Sin validación de rango para Alternate Function | Verificación `af <= 15` previa a cualquier llamada de GPIO |
| **D3** | `uart_hal.c` / `uart_ll.c` | [197–213](uart_hal.c#L197-L213) / [358](uart_ll.c#L358) | Configuración de TX antes de validar RX; `LL_USART_EnableDirection` atómico | Prevalidación de ambos pines; `SET_BIT(u->CR1, TE \| RE)` directo no atómico |
| **D4** | `uart_ll.c` | [558–564, 583–588](uart_ll.c#L558-L564) | `LL_USART_EnableIT_*` con `ATOMIC_SET_BIT` (cuelgue en D3 LPUART1) | RMW no exclusivo dentro de sección `PRIMASK` y `__disable_irq()` |
| **D6** | `uart_hal.c` | [156–159, 191–194](uart_hal.c#L156-L159) | Sin validación de instancia en `UART_HAL_Init` / `InitEx` | `UART_HAL_IsValidInstance()` valida instancia contra periféricos soportados |

---

## 3. Desviaciones del Driver (D5 Aprobada, D6 Declarada Pendiente de Aprobación)

### D5: Semántica de Recolección de Errores en `TakeErrors` (Aprobada)
- **Componentes afectados:** `UART_BM_TakeErrors()` en [uart_bm.c:514-530](uart_bm.c#L514-L530) y `UART_LL_TakeErrors()` en [uart_ll.c:538-554](uart_ll.c#L538-L554).
- **Comportamiento previo (`b41c4a7`):** `TakeErrors` únicamente leía y borraba la máscara de errores acumulada por software en variables internas de la estructura driver. Los flags de error en los registros hardware (`USART_ISR`) permanecían inalterados.
- **Comportamiento actual (`9b9fe37`):** Además de recolectar los flags de software, `TakeErrors` lee directamente los flags de estado hardware en `ISR` (`PE`, `FE`, `NE`, `ORE`) y los limpia mediante escritura en `USART_ICR` (`PECF`, `FECF`, `NCF`, `ORECF`).
- **Justificación técnica:** En recepción sin interrupciones o con buffers FIFO, los flags de error hardware son "sticky" (permanecen activos en `ISR` hasta que se escriba un 1 en `ICR`). Si no se limpiaran a nivel hardware en `TakeErrors`, cualquier consulta subsiguiente seguiría observando el error anterior.
- **Efecto colateral:** En modo FIFO, el flag hardware es global al periférico. Su borrado inmediato disocia el flag de error de la posición específica del byte en el FIFO de hardware (los errores se reportan de forma agregada).
- **Estado:** **Aprobada** por la dirección técnica y auditoría.

### D6: Validación Previa de Instancia en Capa HAL (`UART_HAL_IsValidInstance`) (Declarada, Pendiente de Aprobación)
- **Componentes afectados:** `UART_HAL_Init()` y `UART_HAL_InitEx()` en [uart_hal.c:156-159, 191-194](uart_hal.c#L156-L159).
- **Comportamiento previo (`b41c4a7`):** No se verificaba la validez del puntero de instancia (`huart->Instance`). Si se pasaba una dirección arbitraria o inválida, el driver llamaba al MSP o modificaba registros sin validación previa.
- **Comportamiento actual (`9b9fe37`):** Se verifica formalmente mediante la función auxiliar `UART_HAL_IsValidInstance()` que la instancia corresponda a una de las instancias UART/USART/LPUART válidas del microcontrolador STM32H755 (`USART1`, `USART2`, `USART3`, `UART4`, `UART5`, `USART6`, `UART7`, `UART8`, `LPUART1`). En caso contrario, se aborta inmediatamente retornando `HAL_ERROR` sin interactuar con registros ni periféricos.
- **Justificación técnica:** Requisito fundamental de robustez y contención de fallos (regla de oro del proyecto: cero mutaciones en el hardware ante parámetros inválidos).
- **Estado:** **Declarada formalmente en esta fase, pendiente de aprobación formal del auditor.**

---

## 4. Evidencia de Desarrollo en Hardware Real (NUCLEO-H755ZI-Q + AD2)

La suite completa de selftest y oráculo de hardware ha sido ejecutada físicamente contra la placa NUCLEO-H755ZI-Q (`sn=0016000C4142501220353451`) conectada al Analog Discovery 2 (`sn=210321B5D38D`).

- **Captura de hardware:** [03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_9b9fe37.txt](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_9b9fe37.txt)
- **Veredicto Firmware:** `FW VERDICT=PASS`
- **Veredicto Host:** `HOST VERDICT=PASS`
- **Árbol de trabajo:** Limpio al inicio y al final de la ejecución (`PASS`).
- **Verificación UT3 a 9600 Baud:** Captura a 2 MS/s alojando la totalidad de las 6 tramas (`frames=6 bytes=[0x55, 0xAA, 0xA5, 0x5A, 0x00, 0xFF]`), 6623 flancos detectados de MCO1 a 800 kHz, `bit_meas=83.332 bit_th=83.336 err=-0.00% -> PASS`, y control negativo con 5 tramas `frames=5/6 -> FAIL (exp)`.

### Tabla de Verificación del Oráculo (The Oracle Rule) en Hardware Real

| Paso | Comprobación / Check Name | Real | Control Negativo | Estado Final |
|---|---|---|---|---|
| **UT0** | Bench resting state and 1k resistor | **PASS** | FAIL (exp) | **PASS** |
| **UT0_CTRL** | Negative control line low | **PASS** | FAIL (exp) | **PASS** |
| **UT1** | Parameter rejections across BM, LL, HAL (U2, U3, D2) | **PASS** | FAIL (exp) | **PASS** |
| **UT2** | Baudrate PRESC/BRR formula and U5 checks | **PASS** | FAIL (exp) | **PASS** |
| **UT3** | Wire format and bit period against MCO1 (±0.5%) | **PASS** | FAIL (exp) | **PASS** |
| **UT4** | CM7 loopback reception over JU1 (72/72 bytes) | **PASS** | FAIL (exp) | **PASS** |
| **UT4_CM4** | CM4 loopback reception over JU1 (16/16 bytes) | **PASS** | FAIL (exp) | **PASS** |
| **UT5** | Error injection (PE, FE, NE, ORE-FIFO, ORE-NOFIFO, clean) | **PASS** | FAIL (exp) | **PASS** |
| **UT6** | RTS/CTS flow control over JU2 (24 bytes FC + ORE control) | **PASS** | FAIL (exp) | **PASS** |
| **UT7** | HAL InitEx 8E1 and GPIO lock contention U6 | **PASS** | FAIL (exp) | **PASS** |
| **UT8** | DMA errata live transfer (bit 20, live transfer, rejections) | **PASS** | FAIL (exp) | **PASS** |
| **UT9** | U1 console handle GPIO lock and state reset | **PASS** | FAIL (exp) | **PASS** |

### Prueba de Resets en Caliente (20 Warm Resets)
- **Preset:** `selftest-uart` en `prj_01_template_dualcore`.
- **Comando:** `py tools/run_warm_resets.py --project 03_firmware_templates/prj_01_template_dualcore --preset selftest-uart --out 03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_9b9fe37.txt --title "selftest-uart 20 warm resets" --count 20`
- **Resultado:** **20/20 resets superados exitosamente**, alcanzando `(CPU2 alive)` y verificando identidad de revisión (`9b9fe37`) e id de compilación (`1b17d90d0e0998e9`) en cada iteración.
- **Captura:** [03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_9b9fe37.txt](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_9b9fe37.txt).

---

## 5. Resultados de Verificación Off-Board y Capturas Commiteadas

### T1: Matriz de Compilación (11 Presets)
- **Herramienta:** `tools/uart_checks.py --output 01_shared_drivers/04_uart/docs/uart_checks_9b9fe37.txt`
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
- **Captura commiteada:** [docs/uart_checks_9b9fe37.txt](docs/uart_checks_9b9fe37.txt).

---

## 6. Instrucciones de Reproducción para el Auditor

### 6.1 Preparación del Banco de Pruebas
1. Conectar puente **JU1**: PD5 (CN9-6, USART2_TX) con **resistencia de 1 kΩ** a PD6 (CN9-4, USART2_RX).
2. Conectar puente **JU2**: PD4 (CN9-8, USART2_RTS) a PD3 (CN9-10, USART2_CTS).
3. Conectar cableado del **AD2**:
   - Conectar **GND** del AD2 al GND de la placa NUCLEO-H755ZI-Q antes de conectar señales.
   - **DIO12**: PE2 (CN9-14 / CN11-46), señal `HOST_REQ`.
   - **DIO13**: PE7 (CN10-20 / CN12-44), señal `MCU_ACK`.
   - **DIO14**: PA8 (CN10-4 / CN12-23), señal `MCO1` (reloj de referencia 800 kHz).
   - **DIO15**: PD6 (CN9-4), analizador lógico / inyector en USART2_RX.
4. Desconectar jumpers J2/J3 de la placa de destino (F334) y desconectar el cable USB de la F334 (`00_gpio/README.md` §8).

### 6.2 Comandos de Ejecución y Recetas de Auditoría

En consola PowerShell con el entorno cargado (`. .\tools\env.ps1`):

#### 1. Verificación completa sin placa (T1 matriz y T2 símbolos/cabeceras)
```powershell
py tools\uart_checks.py --output 01_shared_drivers\04_uart\docs\uart_checks_repro.txt
```

#### 2. Verificación mock off-board del host
```powershell
py 03_firmware_templates\prj_01_template_dualcore\tools\uart_hw_check.py --mock-test
```

#### 3. Flasheo y ejecución del selftest en hardware real con AD2
```powershell
py 03_firmware_templates\prj_01_template_dualcore\tools\flash.py --preset selftest-uart --sn 0016000C4142501220353451
py 03_firmware_templates\prj_01_template_dualcore\tools\uart_hw_check.py --out 03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_repro.txt
```

#### 4. Receta del Control de UT9 en Worktree Independiente (Reproducción del Defecto U1 en `b41c4a7`)
En la versión base `b41c4a7`, `HAL_UART_Init` ignoraba `s_msp_gpio_status` y devolvía `HAL_OK` (`0`) a pesar de que el lock de GPIO estaba tomado por CM4. Para verificar la reproducción exacta de este control negativo sin ensuciar ni alterar el checkout principal de trabajo:

```powershell
# a) Crear worktree temporal desvinculado (detached) apuntando al commit de código:
git worktree add --detach ..\stm32h755-ut9-control 9b9fe37

# b) Extraer el archivo uart_hal.c defectuoso del commit base b41c4a7 en el worktree:
git show b41c4a7:01_shared_drivers/04_uart/uart_hal.c > ..\stm32h755-ut9-control\01_shared_drivers\04_uart\uart_hal.c

# c) Recompilar selftest-uart en el worktree:
cmake --build ..\stm32h755-ut9-control\03_firmware_templates\prj_01_template_dualcore\build\selftest-uart

# d) Flashear imagen de control usando el ST-LINK SN:
py ..\stm32h755-ut9-control\03_firmware_templates\prj_01_template_dualcore\tools\flash.py --preset selftest-uart --sn 0016000C4142501220353451

# e) Ejecutar host en modo control (valida que UT9 imprime 'returned 0' ante lock tomado y el host reporta PASS):
py ..\stm32h755-ut9-control\03_firmware_templates\prj_01_template_dualcore\tools\uart_hw_check.py --expect-control-u1 --allow-dirty

# f) Eliminar worktree temporal (el checkout principal permanece 100% limpio):
git worktree remove --force ..\stm32h755-ut9-control

# g) Re-flashear la imagen correcta selftest-uart del árbol principal:
py 03_firmware_templates\prj_01_template_dualcore\tools\flash.py --preset selftest-uart --sn 0016000C4142501220353451
```

#### 5. Receta de UT10 (Selftest real en `prj_04_adc_dma_bm_eth`)
Para validar el selftest real de UART en el proyecto de producción `prj_04` manteniendo el puente JU1 con resistencia serie de 1 kΩ:

```powershell
# a) Compilar el preset selftest-uart en prj_04:
cmake --build 02_firmware_projects\prj_04_adc_dma_bm_eth\build\selftest-uart

# b) Flashear en la placa NUCLEO-H755ZI-Q con --sn:
py 02_firmware_projects\prj_04_adc_dma_bm_eth\tools\flash.py --preset selftest-uart --sn 0016000C4142501220353451

# c) Abrir el monitor serie en una consola (115200 baud, 8N1):
# Start-Process -NoNewWindow py "tools\monitor.py --baud 115200"

# d) Emitir un reset NRST por SWD tras abrir el monitor para capturar el arranque:
STM32_Programmer_CLI.exe -c port=SWD sn=0016000C4142501220353451 mode=HOTPLUG -rst

# Salida esperada capturada en monitor:
# [m4_uart_test] jumper=1 bm_init=1,1,1 bm_loop=1,1,1 ll=1,1 ore=1
```

#### 6. Prueba de estabilidad de warm resets sobre selftest-uart
```powershell
py tools\run_warm_resets.py --project 03_firmware_templates/prj_01_template_dualcore --preset selftest-uart --out 03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_repro.txt --title "selftest-uart 20 warm resets" --count 20
```

#### 7. Flasheo final de imagen de producción debug y retorno a estado seguro S2
```powershell
py 03_firmware_templates\prj_01_template_dualcore\tools\flash.py --preset debug --sn 0016000C4142501220353451
py -c "import sys; sys.path.insert(0, 'tools'); from ad2 import AD2; ad = AD2(); ad.set_safe_state(); ad.close()"
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
- [x] Ejecución en hardware real con captura [`uart_dev_hw_9b9fe37.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_9b9fe37.txt) con `FW VERDICT=PASS`, `HOST VERDICT=PASS` y 12/12 pasos de la tabla Oracle en PASS.
- [x] 20/20 warm resets en `selftest-uart` con captura [`uart_dev_warm_resets_9b9fe37.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_9b9fe37.txt) alcanzando `(CPU2 alive)`.
- [x] Placa flasheada con preset de producción `debug` y AD2 en estado seguro S2.
- [x] Desviaciones `D5` (`TakeErrors`) aprobada y `D6` (`UART_HAL_IsValidInstance`) formalmente documentada pendiente de aprobación en §3.
- [x] Citas a archivos, líneas, conectores y constantes verificadas contra HEAD (`9b9fe37`).
- [x] Ninguna escritura realizada en la configuración de git ni enlaces absolutos `file:///`.

F1 CORREGIDA 5 PARA AUDITORIA 9b9fe37
