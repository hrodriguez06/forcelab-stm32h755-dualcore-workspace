# Informe de Implementación: Fase F1 del Driver `04_uart` (Sexta Corrección de Auditoría)

**Fecha:** 2026-10-17  
**Rama:** `driver/uart-audit`  
**Base:** `main` (`b41c4a7`)  
**Último commit de código / herramienta:** `ab390d3`  
**Estado de la fase:** F1 CORREGIDA 6 PARA AUDITORIA  

---

## 1. Resumen de la Entrega y Correcciones de Auditoría

Se ha completado la sexta corrección de la fase F1 atendiendo la totalidad de los hallazgos de [AUDIT.md](AUDIT.md) (§ "F1, corrección 5", incluyendo `H-M1`, `H-M2`, `H-m1`..`H-m7`), las directrices de `IMPLEMENTATION_PLAN.md` (revisión 2) e `IMPLEMENTER_INSTRUCTIONS.md`.

### 1.1 Correcciones del Driver (`01_shared_drivers/04_uart/`):
1. **U1:** `UART_HAL_Init()` en [uart_hal.c:173-186](uart_hal.c#L173-L186) restablece `s_msp_gpio_status = HAL_OK` antes de llamar a `HAL_UART_Init()` y propaga el estado exacto retornado por el callback MSP (`s_msp_gpio_status`), propagando contención de GPIO.
2. **U2 / U3:** En [uart_ll.c:205-285](uart_ll.c#L205-L285), validación previa pura de parámetros en Fase 1 (`data_bits`, `stop_bits`, `parity`, `hw_flow`, `oversampling`, pines y baudrate alcanzable) garantizando que cero registros hardware (GPIO, RCC o USART) sean modificados ante entradas inválidas.
3. **U4:** En [uart_ll.c:324-328](uart_ll.c#L324-L328), `UART_LL_Init()` deshabilita el periférico y reinicia `CR1`, `CR2` y `CR3` a 0 para partir de un estado conocido y limpio.
4. **U5:** En [uart_ll.c:157-163](uart_ll.c#L157-L163), cómputo del divisor efectivo aplicado por hardware con `OVER8=1` (`(brr & 0xFFF0) | ((brr & 0x0007) << 1)`), rechazando divisores donde los bits bajos impares son descartados por hardware.
5. **U6:** En [uart_hal.c:229-233, 249-253](uart_hal.c#L229-L233), `UART_HAL_InitEx()` reporta `HAL_BUSY` cuando el lock de GPIO está tomado por el otro núcleo (`GPIO_BM_ERR_BUSY`).
6. **D1:** En [uart_ll.c:254-261](uart_ll.c#L254-L261), LPUART1 ignora el campo `oversampling` de forma transparente al igual que `uart_bm.c:362`, al soportar únicamente 16x por arquitectura hardware (RM0399 §52.4.4).
7. **D2:** Validación previa de Alternate Function (`af <= 15`) en [uart_ll.c:267, 277](uart_ll.c#L267) y [uart_hal.c:199-200, 208-209](uart_hal.c#L199-L200).
8. **D3:** Prevalidación de ambos pines TX y RX en [uart_hal.c:197-213](uart_hal.c#L197-L213). En [uart_ll.c:358](uart_ll.c#L358), uso de `SET_BIT(u->CR1, USART_CR1_TE | USART_CR1_RE)` en lugar de funciones atómicas para evitar bloqueo en Dominio D3 (LPUART1).
9. **D4:** En [uart_ll.c:558-564, 583-588](uart_ll.c#L558-L564), `UART_LL_EnableRxIrq` y `UART_LL_DisableRxIrq` utilizan RMW no exclusivo dentro de sección crítica con `PRIMASK` y `__disable_irq()` en lugar de `LL_USART_EnableIT_*` / `LL_USART_DisableIT_*` (cuyo `ATOMIC_SET_BIT` con `LDREX`/`STREX` bloquea en buses de Dominio 3).
10. **D6 (Desviación Formal Declarada Pendiente de Aprobación):** Validación previa de instancia en [uart_hal.c:156-159, 191-194](uart_hal.c#L156-L159) mediante `UART_HAL_IsValidInstance()`, rechazando instancias no soportadas o nulas con `HAL_ERROR` antes de invocar callbacks MSP o tocar registros hardware.
11. **Cabeceras:** Las cabeceras `01_shared_drivers/04_uart/*.h` permanecen 100% congeladas (0 líneas modificadas frente a `b41c4a7`).

### 1.2 Correcciones de Auditoría 6 (Selftest, Host y Herramientas):
- **H-M1 (Controles Negativos de UT3 por Fila):** En [uart_hw_check.py](../../03_firmware_templates/prj_01_template_dualcore/tools/uart_hw_check.py), para cada una de las 15 filas de UT3 se ejecutan y registran 5 mutaciones independientes sobre los datos medidos evaluadas con `eval_ut3()`:
  1. `drop_frame`: omisión de una trama (`decoded[:-1]`), evaluando rechazo por recuento de tramas (`frame count mismatch`).
  2. `timing`: incremento del ancho de bit medido en `2 * tol` (`mco_per_bit * (1.0 + 2 * tolerance)`), evaluando rechazo por error temporal de MCO1 (`mco error`).
  3. `bit_flip`: inversión de un bit de datos (`byte ^ 1`), evaluando rechazo por discordancia de byte (`byte mismatch`).
  4. `parity`: inversión de paridad (`parity_ok = False` en las filas configuradas con paridad), evaluando rechazo por paridad (`parity error`).
  5. `stop_bit`: corrupción del bit de parada (`stop_ok = False`), evaluando rechazo por bit de parada (`stop bit error`).
  Cada una de estas 5 mutaciones se evalúa estrictamente a través de `eval_ut3()` y se registra en la captura como `FAIL (exp)`.
- **H-M2 (Corrección de Carrera en Handshake de DONE y Protocolo Limpio):**
  - En CM4 [uart_selftest.c](../../03_firmware_templates/prj_01_template_dualcore/CM4/App/Src/uart_selftest.c), se eliminó la comprobación `cmd == UST_CMD_DONE` del bucle `while (cmd_seq == last_seq)`, asegurando que CM4 espere a que CM7 incremente la secuencia y procese el comando en el switch, actualizando `UST_AREA->ack_seq = last_seq` con barrera de memoria `__DMB()` antes de finalizar la tarea.
  - En CM7 [uart_selftest.c](../../03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/uart_selftest.c), se comprueba el retorno booleano de `cm4_send_cmd(UST_CMD_DONE, CM4_TIMEOUT_MS)`. Si CM4 no confirma el comando, se incrementa `s_fail`.
  - Se registra `m4_done_ack=%u` (1 en caso de confirmación exitosa, 0 en caso de fallo). El host comprueba mandatoriamente `m4_done_ack == 1`.
  - Se eliminó el campo ambiguo `m4_timeout` tanto del firmware como del host.
- **H-m1 (Lectura Dinámica del Reloj de Kernel desde Registros Hardware):** En [CM7 uart_selftest.c](../../03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/uart_selftest.c), `mco1_enable()` lee el registro `RCC->D2CCIP2R` (`RCC_D2CCIP2R_USART28SEL`). Si la fuente seleccionada es `RCC_USART234578CLKSOURCE_D2PCLK1` (0), se obtiene la frecuencia mediante `HAL_RCC_GetPCLK1Freq()`; si la fuente configurada no es D2PCLK1, se aborta la sesión invocando `bench_abort("USART2 kernel clock not D2PCLK1")`.
- **H-m2 (Verificación de Continuidad Temporal de MCO1 en `decode_uart_frames`):** En [uart_hw_check.py](../../03_firmware_templates/prj_01_template_dualcore/tools/uart_hw_check.py), se valida: (1) que el primer flanco de MCO1 ocurra antes o durante el inicio de la primera trama (`mco_rises[0] <= frames[0]["start_edge"]`), (2) que el espaciado entre flancos consecutivos no exceda `1.5 * mco_period_samples` dentro del rango temporal de las tramas, y (3) que los flancos de MCO1 cubran hasta el final de las tramas (`mco_rises[-1] >= frames[-1]["frame_end"]`).
- **H-m3 (Recuento Efectivo de Bytes Transmitidos en UT4):** En [CM7 uart_selftest.c](../../03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/uart_selftest.c), `total_sent` se incrementa únicamente cuando la llamada a `UART_BM_WriteByte()`, `UART_LL_WriteByte()` o `UART_LL_Write()` retorna éxito, reportando exactamente los bytes transmitidos al periférico.
- **H-m4 (Mínimo de Tramas Medibles en `eval_ut3`):** En [uart_hw_check.py](../../03_firmware_templates/prj_01_template_dualcore/tools/uart_hw_check.py), `eval_ut3()` comprueba que al menos 4 de las 6 tramas decodificadas sean medibles (`sum(1 for f in frames if f.get("measurable", False)) >= 4`).
- **H-m5 (Citas Exactas de D5 y Titulación Pendiente de Aprobación):** Se corrigieron las citas de D5 a [uart_bm.c:575-609](uart_bm.c#L575-L609) y [uart_ll.c:514-536](uart_ll.c#L514-L536). Se titularon formalmente D5 y D6 como "declaradas, pendientes de aprobación", eliminando afirmaciones preliminares de aprobación o términos hiperbólicos.
- **H-m6 (Recetas de Reproducción Ejecutables y Aisladas):** En §6.2, la receta de UT9 se ejecuta en un worktree independiente extrayendo el archivo base mediante `git -C <wt> checkout b41c4a7 -- 01_shared_drivers/04_uart/uart_hal.c`, configurando con `cmake --preset selftest-uart -DCUBE_DIR=...` y guardando la captura con `--out` fuera del worktree. La receta de UT10 utiliza `monitor.py --port COM4 --sn ... --timeout 10` y reset por hardware con `-hardRst`. En [tools/run_warm_resets.py](../../tools/run_warm_resets.py), `do_reset()` incluye `sn=0016000C4142501220353451`.
- **H-m7 (Detalles Menores y Mapeo MCO1):** Comentario en línea 2195 de CM7 actualizado a 800 kHz (HSE/10, ratio 125:1). Valor por defecto `f_mco = 800000.0` en `decode_uart_frames()`. Mapeo exhaustivo de `mco1_sel` en `mco1_enable()` para todos los casos (0=HSI, 1=LSE, 2=HSE, 3=PLL1Q vía `HAL_RCCEx_GetPLL1ClockFreq`, 4=HSI48).

### 1.3 Asignación del Analizador Lógico (N11)
El Analizador Lógico (LA) del AD2 está dedicado a **UT3** para inspeccionar la señalización física en el hilo (`USART2_TX` a `PD5` / `DIO15`):
1. **UT3** valida la codificación física en el cable (bit de start, bits de datos, bit de paridad par/impar, 1 y 2 bits de parada, y duración temporal del bit referenciada con el reloj de hardware MCO1 a ±0.5% de tolerancia) cubriendo los tres tiers (BM, LL y HAL) y combinaciones de formato y baudrates (9600 a 921600 baud).
2. **UT4–UT8** evalúan mecanismos internos del periférico y del sistema:
   - **UT4 / UT4_CM4:** Bucle de retorno local (loopback) a través de JU1, verificando motores de interrupción, buffers FIFO, oversampling 8x y enmascaramiento de 7 bits donde la recepción interna por el periférico es la autoridad verificadora.
   - **UT5:** Inyección de errores donde el generador digital del AD2 transmite tramas corruptas y el periférico hardware del MCU detecta y clasifica las banderas de error en `ISR` (`PE`, `FE`, `NE`, `ORE`).
   - **UT6:** Control de flujo hardware RTS/CTS sobre JU2, validando el desasertado de RTS y la congelación de transmisión de CTS a nivel hardware.
   - **UT7:** Contención de lock GPIO entre núcleos y prevalidación en `UART_HAL_InitEx`.
   - **UT8:** Errata de DMA en bus AXI/AHB y transferencia DMA1 Stream 0 en memoria real.

---

## 2. Detalle de Correcciones U1–U6, D1–D4, D6 y Citaciones Exactas

| ID | Archivo | Líneas en HEAD (`ab390d3`) | Antes (`b41c4a7`) | Después (`ab390d3`) |
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

## 3. Desviaciones del Driver (D5 y D6 Declaradas, Pendientes de Aprobación)

### D5: Semántica de Recolección de Errores en `TakeErrors` (Declarada, Pendiente de Aprobación)
- **Componentes afectados:** `UART_BM_TakeErrors()` en [uart_bm.c:575-609](uart_bm.c#L575-L609) y `UART_LL_TakeErrors()` en [uart_ll.c:514-536](uart_ll.c#L514-L536).
- **Comportamiento previo (`b41c4a7`):** `TakeErrors` únicamente leía y borraba la máscara de errores acumulada por software en variables internas de la estructura driver. Los flags de error en los registros hardware (`USART_ISR`) permanecían inalterados.
- **Comportamiento actual (`ab390d3`):** Además de recolectar los flags de software, `TakeErrors` lee directamente los flags de estado hardware en `ISR` (`PE`, `FE`, `NE`, `ORE`) y los limpia mediante escritura en `USART_ICR` (`PECF`, `FECF`, `NCF`, `ORECF`).
- **Justificación técnica:** En recepción sin interrupciones o con buffers FIFO, los flags de error hardware son persistentes en `ISR` hasta que se escriba en `ICR`. Si no se limpiaran a nivel hardware en `TakeErrors`, cualquier consulta subsiguiente continuaría reportando el error previo.
- **Efecto colateral:** En modo FIFO, el flag hardware es global al periférico. Su borrado inmediato disocia el flag de error de la posición específica del byte en el FIFO de hardware (los errores se reportan de forma agregada).
- **Estado:** **Declarada formalmente en esta fase, pendiente de aprobación formal del auditor.**

### D6: Validación Previa de Instancia en Capa HAL (`UART_HAL_IsValidInstance`) (Declarada, Pendiente de Aprobación)
- **Componentes afectados:** `UART_HAL_Init()` y `UART_HAL_InitEx()` en [uart_hal.c:156-159, 191-194](uart_hal.c#L156-L159).
- **Comportamiento previo (`b41c4a7`):** No se verificaba la validez del puntero de instancia (`huart->Instance`). Si se pasaba una dirección arbitraria o inválida, el driver llamaba al MSP o modificaba registros sin validación previa.
- **Comportamiento actual (`ab390d3`):** Se verifica formalmente mediante la función auxiliar `UART_HAL_IsValidInstance()` que la instancia corresponda a una de las instancias UART/USART/LPUART válidas del microcontrolador STM32H755 (`USART1`, `USART2`, `USART3`, `UART4`, `UART5`, `USART6`, `UART7`, `UART8`, `LPUART1`). En caso contrario, se aborta inmediatamente retornando `HAL_ERROR` sin interactuar con registros ni periféricos.
- **Justificación técnica:** Requisito de robustez y contención de fallos para garantizar cero mutaciones en el hardware ante parámetros inválidos.
- **Estado:** **Declarada formalmente en esta fase, pendiente de aprobación formal del auditor.**

---

## 4. Evidencia de Desarrollo en Hardware Real (NUCLEO-H755ZI-Q + AD2)

La suite completa de selftest y oráculo de hardware ha sido ejecutada físicamente contra la placa NUCLEO-H755ZI-Q (`sn=0016000C4142501220353451`) conectada al Analog Discovery 2 (`sn=210321B5D38D`) dos veces consecutivas:

- **Captura principal de hardware:** [03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_ab390d3.txt](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_ab390d3.txt)
- **Captura Ejecución 1:** [03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_ab390d3_run1.txt](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_ab390d3_run1.txt)
- **Captura Ejecución 2:** [03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_ab390d3_run2.txt](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_ab390d3_run2.txt)
- **Veredicto Firmware:** `FW VERDICT=PASS` (en ambas ejecuciones)
- **Veredicto Host:** `HOST VERDICT=PASS` (en ambas ejecuciones)
- **Handshake DONE:** `m4_done_ack=1` en ambas ejecuciones, verificado por el host.
- **Árbol de trabajo:** Limpio al inicio y al final de cada ejecución (`PASS`).
- **Verificación UT3 con 5 Controles por Fila:** Las 15 filas de UT3 decodifican las 6 tramas completas y registran los 5 controles negativos evaluados vía `eval_ut3()`: `drop_frame=FAIL (exp)`, `timing(+1.0%)=FAIL (exp)`, `bit_flip=FAIL (exp)`, `parity=FAIL (exp)` (o `N/A`), `stop_bit=FAIL (exp)`.

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
- **Comando:** `py tools/run_warm_resets.py --project 03_firmware_templates/prj_01_template_dualcore --preset selftest-uart --out 03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_ab390d3.txt --title "selftest-uart 20 warm resets" --count 20 --sn 0016000C4142501220353451`
- **Resultado:** **20/20 resets superados exitosamente**, alcanzando `(CPU2 alive)`, reportando `Reset method: STM32_Programmer_CLI -c port=SWD sn=0016000C4142501220353451 mode=HOTPLUG -rst`, y verificando identidad de revisión (`ab390d3`) e id de compilación (`c1d8890dd8cd3f7d`) en cada iteración.
- **Captura:** [03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_ab390d3.txt](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_ab390d3.txt).

---

## 5. Resultados de Verificación Off-Board y Capturas Commiteadas

### T1: Matriz de Compilación (11 Presets)
- **Herramienta:** `tools/uart_checks.py --output 01_shared_drivers/04_uart/docs/uart_checks_ab390d3.txt`
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
- **Captura commiteada:** [docs/uart_checks_ab390d3.txt](docs/uart_checks_ab390d3.txt).

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

#### 3. Flasheo y ejecución del selftest en hardware real con AD2 (dos ejecuciones)
```powershell
py 03_firmware_templates\prj_01_template_dualcore\tools\flash.py --preset selftest-uart --sn 0016000C4142501220353451
py 03_firmware_templates\prj_01_template_dualcore\tools\uart_hw_check.py --out 03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_repro_run1.txt
py 03_firmware_templates\prj_01_template_dualcore\tools\uart_hw_check.py --out 03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_repro_run2.txt
```

#### 4. Receta del Control de UT9 en Worktree Independiente (Reproducción del Defecto U1 en `b41c4a7`)
En la versión base `b41c4a7`, `HAL_UART_Init` ignoraba `s_msp_gpio_status` y devolvía `HAL_OK` (`0`) a pesar de que el lock de GPIO estaba tomado por CM4. Para verificar la reproducción exacta de este control negativo sin alterar el checkout principal de trabajo:

```powershell
# a) Crear worktree temporal desvinculado (detached) apuntando al commit de entrega:
git worktree add --detach ..\stm32h755-ut9-control ab390d3

# b) Extraer el archivo uart_hal.c defectuoso del commit base b41c4a7 dentro del worktree:
git -C ..\stm32h755-ut9-control checkout b41c4a7 -- 01_shared_drivers/04_uart/uart_hal.c

# c) Configurar y compilar selftest-uart en el worktree pasando CUBE_DIR si aplica:
cmake -S ..\stm32h755-ut9-control\03_firmware_templates\prj_01_template_dualcore -B ..\stm32h755-ut9-control\03_firmware_templates\prj_01_template_dualcore\build\selftest-uart --preset selftest-uart -DCUBE_DIR=c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/STM32CubeH7
cmake --build ..\stm32h755-ut9-control\03_firmware_templates\prj_01_template_dualcore\build\selftest-uart

# d) Flashear imagen de control usando el ST-LINK SN:
py ..\stm32h755-ut9-control\03_firmware_templates\prj_01_template_dualcore\tools\flash.py --preset selftest-uart --sn 0016000C4142501220353451

# e) Ejecutar host en modo control guardando salida fuera del worktree:
py ..\stm32h755-ut9-control\03_firmware_templates\prj_01_template_dualcore\tools\uart_hw_check.py --expect-control-u1 --allow-dirty --out 03_firmware_templates/prj_01_template_dualcore/docs/uart_ut9_control_repro.txt

# f) Eliminar worktree temporal:
git worktree remove --force ..\stm32h755-ut9-control

# g) Re-flashear la imagen selftest-uart correcta del árbol principal:
py 03_firmware_templates\prj_01_template_dualcore\tools\flash.py --preset selftest-uart --sn 0016000C4142501220353451
```

#### 5. Receta de UT10 (Selftest real en `prj_04_adc_dma_bm_eth`)
Para validar el selftest real de UART en el proyecto de producción `prj_04` manteniendo el puente JU1 con resistencia serie de 1 kΩ:

```powershell
# a) Compilar el preset selftest-uart en prj_04:
cmake --build 02_firmware_projects\prj_04_adc_dma_bm_eth\build\selftest-uart

# b) Flashear en la placa NUCLEO-H755ZI-Q con --sn:
py 02_firmware_projects\prj_04_adc_dma_bm_eth\tools\flash.py --preset selftest-uart --sn 0016000C4142501220353451

# c) Abrir el monitor serie con timeout y ST-LINK SN en segundo plano:
Start-Process -NoNewWindow py "tools\monitor.py --port COM4 --sn 0016000C4142501220353451 --timeout 10"

# d) Emitir un reset NRST por hardware por SWD tras abrir el monitor para capturar el arranque:
STM32_Programmer_CLI.exe -c port=SWD sn=0016000C4142501220353451 mode=HOTPLUG -hardRst

# Salida capturada en monitor:
# [m4_uart_test] jumper=1 bm_init=1,1,1 bm_loop=1,1,1 ll=1,1 ore=1
```

#### 6. Prueba de estabilidad de warm resets sobre selftest-uart
```powershell
py tools\run_warm_resets.py --project 03_firmware_templates/prj_01_template_dualcore --preset selftest-uart --out 03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_repro.txt --title "selftest-uart 20 warm resets" --count 20 --sn 0016000C4142501220353451
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
- [x] Ejecución en hardware real con capturas [`uart_dev_hw_ab390d3_run1.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_ab390d3_run1.txt) y [`uart_dev_hw_ab390d3_run2.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_ab390d3_run2.txt) con `FW VERDICT=PASS`, `HOST VERDICT=PASS` y 12/12 pasos de la tabla Oracle en PASS.
- [x] Handshake de DONE con confirmación explícita `m4_done_ack=1` en CM4, CM7 y host.
- [x] 20/20 warm resets en `selftest-uart` con captura [`uart_dev_warm_resets_ab390d3.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_ab390d3.txt) alcanzando `(CPU2 alive)`.
- [x] Placa flasheada con preset de producción `debug` y AD2 en estado seguro S2.
- [x] Desviaciones `D5` (`TakeErrors`, [uart_bm.c:575-609](uart_bm.c#L575-L609) y [uart_ll.c:514-536](uart_ll.c#L514-L536)) y `D6` (`UART_HAL_IsValidInstance`, [uart_hal.c:156-159, 191-194](uart_hal.c#L156-L159)) formalmente declaradas pendientes de aprobación en §3.
- [x] Citas a archivos, líneas, conectores y constantes verificadas contra HEAD (`ab390d3`).
- [x] Ninguna escritura realizada en la configuración de git ni enlaces absolutos `file:///`.

F1 CORREGIDA 6 PARA AUDITORIA ab390d3
