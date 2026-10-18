# Informe de Implementación: Fase F1 del Driver `04_uart` (Cuarta Corrección de Auditoría)

**Fecha:** 2026-10-17  
**Rama:** `driver/uart-audit`  
**Base:** `main` (`b41c4a7`)  
**Último commit de código / herramienta:** `fdbf306`  
**Estado de la fase:** F1 CORREGIDA 4 PARA AUDITORIA  

---

## 1. Resumen de la Entrega y Correcciones de Auditoría

Se ha completado la cuarta corrección de la fase F1 atendiendo la totalidad de los hallazgos de [AUDIT.md](AUDIT.md) (incluyendo `F-M1`..`F-M3`, `F-m1`, `F-m2` y `N1`..`N15`), las directrices de `IMPLEMENTATION_PLAN.md` (revisión 2) e `IMPLEMENTER_INSTRUCTIONS.md`.

### 1.1 Correcciones del Driver (`01_shared_drivers/04_uart/`):
1. **U1:** `UART_HAL_Init()` en [uart_hal.c:162-174](uart_hal.c#L162-L174) restablece `s_msp_gpio_status = HAL_OK` antes de llamar a `HAL_UART_Init()` y propaga el estado exacto retornado por el callback MSP (`s_msp_gpio_status`), propagando contención de GPIO.
2. **U2 / U3:** En [uart_ll.c:205-285](uart_ll.c#L205-L285), validación previa pura de parámetros en Fase 1 (`data_bits`, `stop_bits`, `parity`, `hw_flow`, `oversampling`, pines y baudrate alcanzable) garantizando que **cero** registros hardware (GPIO, RCC o USART) sean modificados ante entradas inválidas.
3. **U4:** En [uart_ll.c:324-328](uart_ll.c#L324-L328), `UART_LL_Init()` deshabilita el periférico y reinicia `CR1`, `CR2` y `CR3` a 0 para partir de un estado conocido y limpio.
4. **U5:** En [uart_ll.c:157-163](uart_ll.c#L157-L163), cómputo del divisor efectivo aplicado por hardware con `OVER8=1` (`(brr & 0xFFF0) | ((brr & 0x0007) << 1)`), rechazando divisores donde los bits bajos impares son descartados por hardware.
5. **U6:** En [uart_hal.c:218-222, 238-241](uart_hal.c#L218-L222), `UART_HAL_InitEx()` reporta `HAL_BUSY` cuando el lock de GPIO está tomado por el otro núcleo (`GPIO_BM_ERR_BUSY`).
6. **D1:** En [uart_ll.c:254-261](uart_ll.c#L254-L261), LPUART1 ignora el campo `oversampling` de forma transparente al igual que `uart_bm.c:362`, al soportar únicamente 16x por arquitectura hardware (RM0399 §52.4.4).
7. **D2:** Validación previa de Alternate Function (`af <= 15`) en [uart_ll.c:267, 277](uart_ll.c#L267) y [uart_hal.c:198-204](uart_hal.c#L198-L204).
8. **D3:** Prevalidación de ambos pines TX y RX en [uart_hal.c:192-204](uart_hal.c#L192-L204). En [uart_ll.c:358](uart_ll.c#L358), uso de `SET_BIT(u->CR1, USART_CR1_TE | USART_CR1_RE)` en lugar de funciones atómicas para evitar bloqueo en Dominio D3 (LPUART1).
9. **D4:** En [uart_ll.c:558-564, 583-588](uart_ll.c#L558-L564), `UART_LL_EnableRxIrq` y `UART_LL_DisableRxIrq` utilizan RMW no exclusivo dentro de sección crítica con `PRIMASK` y `__disable_irq()` en lugar de `LL_USART_EnableIT_*` / `LL_USART_DisableIT_*` (cuyo `ATOMIC_SET_BIT` con `LDREX`/`STREX` bloquea en buses de Dominio 3).
10. **D6 (Nueva Desviación Formal):** Validación previa de instancia en [uart_hal.c:156-160, 206-210](uart_hal.c#L156-L160) mediante `UART_HAL_IsValidInstance()`, rechazando instancias no soportadas o nulas con `HAL_ERROR` antes de invocar callbacks MSP o tocar registros hardware.
11. **Cabeceras:** Las cabeceras `01_shared_drivers/04_uart/*.h` permanecen 100% congeladas (0 líneas modificadas frente a `b41c4a7`).

### 1.2 Correcciones de Auditoría 4 (Selftest, Host y Herramientas):
- **F-M1:** Registro detallado fila a fila de UT3 en la salida del host: cada una de las 15 filas imprime formato, baudrate, tasa de muestreo y tamaño del buffer del LA, recuento de tramas decodificadas y bytes leídos, ancho de bit medido referenciado a MCO1, ancho teórico (`PRESC`/`BRR`), error porcentual y veredicto PASS/FAIL, acompañado de su respectivo control negativo.
- **F-M2:** Modo control del host corregido (`--expect-control-u1`): el oráculo valida específicamente que el baseline defectuoso retorna `returned 0` (`HAL_OK`) ante contención de GPIO como condición esperada de fallo, logrando `HOST VERDICT=PASS` al evaluar el control.
- **F-M3:** Recetas completas de reproducción incorporadas en §6 para el control negativo de UT9 (reproducción del defecto U1 en `b41c4a7`) y para UT10 (selftest real en `prj_04_adc_dma_bm_eth`).
- **F-m1:** Bucle de vaciado de puerto serie en `tools/run_warm_resets.py` acotado con timeout de 1.0 s para evitar bloqueos ante imágenes que transmiten continuamente.
- **F-m2:** Sustitución de todos los enlaces absolutos `file:///c:/...` por enlaces relativos en el informe de implementación.
- **N1:** Dimensionamiento de frecuencia de muestreo del analizador lógico en UT3 según baudrate (10 MS/s para 9600 baud con buffer de 16k muestras para cumplir Nyquist frente a MCO1 de 4 MHz, 20 MS/s para 115200 baud, 100 MS/s para 921600 baud).
- **N2:** Verificación estricta de todos los bits de parada durante la decodificación de tramas UART (soporte para 1, 1.5 y 2 bits de parada).
- **N3:** Medición de frecuencia de MCO1 condicionada a un mínimo de 10 flancos detectados en el LA, evitando caídas silenciosas a valores teóricos.
- **N4:** Cómputo de frecuencia de MCO1 en firmware a partir de los registros de configuración del reloj (`RCC_CFGR`) y la frecuencia del oscilador HSE.
- **N5:** Verificación y registro explícito de valores numéricos concretos en todos los pasos (lecturas IDR en UT0, contadores de fallo en UT1, bytes enviados/recibidos en UT4 y UT4_CM4, estado de lock en UT7 y UT9, y flag DMA en UT8).
- **N6:** Evaluación trifásica en UT0 (`low=0`, `high=1`, `rest=1`) para verificar reposo y resistencia serie de 1 kΩ; logging de parámetros DOUT por fase en UT5.
- **N7:** Consolidación de UT4 en un único bucle continuo de 72 bytes verificando polled, IRQ, FIFO, oversampling 8x y máscara de 7 bits.
- **N8:** Documentación formal de las desviaciones aprobadas D5 (`TakeErrors`) y D6 (`UART_HAL_IsValidInstance`) en §3.
- **N9:** Verificación explícita de formatos 8E1 (9 bits en bus con paridad) y 7E1 (8 bits en bus con paridad) alineados con el registro `USART_CR1_M` de STM32.
- **N10:** Centrado de pulso de ruido (NE) en sub-muestra 9/16 en UT5 garantizando detección de margen por el periférico hardware.
- **N11:** Uso coherente de la instancia fija UART4 en las comprobaciones de UT2.
- **N12:** Comprobación y logging explícito del flag `m4_timeout` en CM7 al evaluar el estado compartido con CM4.
- **N13:** Eliminación de exenciones globales arbitrarias en `tools/uart_checks.py`, garantizando verificación rigurosa de árbol de trabajo limpio.
- **N14:** Parámetro `--banner` configurable en `tools/run_warm_resets.py` para validar cabecera de arranque en múltiples proyectos.
- **N15:** Filtrado de tramas medibles (mínimo 3 bits de intervalo) y soporte de múltiples bits de parada en generador de muestras mock.

---

## 2. Detalle de Correcciones U1–U6, D1–D4, D6 y Resolución de Defectos

| ID | Archivo | Líneas | Antes (`b41c4a7`) | Después (`fdbf306`) |
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
| **D6** | `uart_hal.c` | 156–160, 206–210 | Sin validación de instancia en `UART_HAL_Init` / `InitEx` | `UART_HAL_IsValidInstance()` valida instancia contra periféricos soportados |

---

## 3. Desviaciones Aprobadas (Deviations D5 y D6)

### D5: Semántica de Recolección de Errores en `TakeErrors`
- **Componentes afectados:** `UART_BM_TakeErrors()` en [uart_bm.c:514-530](uart_bm.c#L514-L530) y `UART_LL_TakeErrors()` en [uart_ll.c:538-554](uart_ll.c#L538-L554).
- **Comportamiento previo (`b41c4a7`):** `TakeErrors` únicamente leía y borraba la máscara de errores acumulada por software en variables internas de la estructura driver. Los flags de error en los registros hardware (`USART_ISR`) permanecían inalterados.
- **Comportamiento actual (`fdbf306`):** Además de recolectar los flags de software, `TakeErrors` lee directamente los flags de estado hardware en `ISR` (`PE`, `FE`, `NE`, `ORE`) y los limpia mediante escritura en `USART_ICR` (`PECF`, `FECF`, `NCF`, `ORECF`).
- **Justificación técnica:** En recepción sin interrupciones o con buffers FIFO, los flags de error hardware son "sticky" (permanecen activos en `ISR` hasta que se escriba un 1 en `ICR`). Si no se limpiaran a nivel hardware en `TakeErrors`, cualquier consulta subsiguiente seguiría observando el error anterior.
- **Efecto colateral:** En modo FIFO, el flag hardware es global al periférico. Su borrado inmediato disocia el flag de error de la posición específica del byte en el FIFO de hardware (los errores ya no quedan atados estrictamente al byte en la cabeza del FIFO sino que se reportan de forma agrupada).

### D6: Validación Previa de Instancia en Capa HAL (`UART_HAL_IsValidInstance`)
- **Componentes afectados:** `UART_HAL_Init()` y `UART_HAL_InitEx()` en [uart_hal.c:156-160, 206-210](uart_hal.c#L156-L160).
- **Comportamiento previo (`b41c4a7`):** No se verificaba la validez del puntero de instancia (`huart->Instance`). Si se pasaba una dirección arbitraria o inválida, el driver llamaba al MSP o modificaba registros sin validación previa.
- **Comportamiento actual (`fdbf306`):** Se verifica formalmente mediante la función auxiliar `UART_HAL_IsValidInstance()` que la instancia corresponda a una de las instancias UART/USART/LPUART válidas del microcontrolador STM32H755 (`USART1`, `USART2`, `USART3`, `UART4`, `UART5`, `USART6`, `UART7`, `UART8`, `LPUART1`). En caso contrario, se aborta inmediatamente retornando `HAL_ERROR` sin interactuar con registros ni periféricos.
- **Justificación técnica:** Requisito de robustez y contención de fallos (regla de oro del proyecto: cero mutaciones en el hardware ante parámetros inválidos).

---

## 4. Verificación en Hardware Real (NUCLEO-H755ZI-Q + AD2)

La suite completa de selftest y oráculo de hardware ha sido ejecutada físicamente contra la placa NUCLEO-H755ZI-Q (`sn=0016000C4142501220353451`) conectada al Analog Discovery 2 (`sn=210321B5D38D`).

- **Captura de hardware:** [03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_fdbf306.txt](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_fdbf306.txt)
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
| **UT4_CM4** | CM4 loopback reception over JU1 (16/16 bytes) | **PASS** | FAIL (exp) | **PASS** |
| **UT5** | Error injection (PE, FE, NE, ORE-FIFO, ORE-NOFIFO, clean) | **PASS** | FAIL (exp) | **PASS** |
| **UT6** | RTS/CTS flow control over JU2 (24 bytes FC + ORE control) | **PASS** | FAIL (exp) | **PASS** |
| **UT7** | HAL InitEx 8E1 and GPIO lock contention U6 | **PASS** | FAIL (exp) | **PASS** |
| **UT8** | DMA errata live transfer (bit 20, live transfer, rejections) | **PASS** | FAIL (exp) | **PASS** |
| **UT9** | U1 console handle GPIO lock and state reset | **PASS** | FAIL (exp) | **PASS** |

### Prueba de Resets en Caliente (20 Warm Resets)
- **Preset:** `selftest-uart` en `prj_01_template_dualcore`.
- **Comando:** `py tools/run_warm_resets.py --project 03_firmware_templates/prj_01_template_dualcore --preset selftest-uart --out 03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_fdbf306.txt --title "selftest-uart 20 warm resets" --count 20`
- **Resultado:** **20/20 resets superados exitosamente**, alcanzando `(CPU2 alive)` y verificando identidad de revisión (`fdbf306`) e id de compilación (`72be00215b3e165d`) en cada iteración.
- **Captura:** [03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_fdbf306.txt](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_fdbf306.txt).

---

## 5. Resultados de Verificación Off-Board y Capturas Commiteadas

### T1: Matriz de Compilación (11 Presets)
- **Herramienta:** `tools/uart_checks.py --output 01_shared_drivers/04_uart/docs/uart_checks_fdbf306.txt`
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
- **Captura commiteada:** [docs/uart_checks_fdbf306.txt](docs/uart_checks_fdbf306.txt).

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

### 6.2 Comandos de Ejecución y Recetas de Auditoría

En consola PowerShell con el entorno cargado (`. .\tools\env.ps1`):

#### 1. Verificación completa sin placa (T1 matriz y T2 símbolos/cabeceras)
```powershell
py tools\uart_checks.py --test all
```

#### 2. Verificación mock off-board del host
```powershell
py 03_firmware_templates\prj_01_template_dualcore\tools\uart_hw_check.py --mock-test
```

#### 3. Flasheo y ejecución del selftest en hardware real con AD2
```powershell
py 03_firmware_templates\prj_01_template_dualcore\tools\flash.py --preset selftest-uart
py 03_firmware_templates\prj_01_template_dualcore\tools\uart_hw_check.py
```

#### 4. Receta del Control de UT9 (Reproducción del Defecto U1 en `b41c4a7`)
En la versión base `b41c4a7`, `HAL_UART_Init` ignoraba `s_msp_gpio_status` y devolvía `HAL_OK` (`0`) a pesar de que el lock de GPIO estaba tomado por CM4. Para verificar la reproducción exacta de este control negativo:

```powershell
# a) Restaurar temporalmente el driver uart_hal.c del commit base b41c4a7:
git checkout b41c4a7 -- 01_shared_drivers/04_uart/uart_hal.c

# b) Recompilar selftest-uart en prj_01:
cmake --build 03_firmware_templates/prj_01_template_dualcore/build/selftest-uart

# c) Flashear imagen de control:
py 03_firmware_templates/prj_01_template_dualcore/tools/flash.py --preset selftest-uart

# d) Ejecutar host en modo control (valida que el firmware imprime 'returned 0' ante lock tomado):
py 03_firmware_templates/prj_01_template_dualcore/tools/uart_hw_check.py --expect-control-u1 --allow-dirty

# e) Restaurar uart_hal.c de HEAD y recompilar selftest-uart:
git checkout HEAD -- 01_shared_drivers/04_uart/uart_hal.c
cmake --build 03_firmware_templates/prj_01_template_dualcore/build/selftest-uart
```

#### 5. Receta de UT10 (Selftest real en `prj_04_adc_dma_bm_eth`)
Para validar el selftest real de UART en el proyecto de producción `prj_04` manteniendo el puente JU1 con resistencia serie de 1 kΩ:

```powershell
# a) Compilar el preset selftest-uart en prj_04:
cmake --build 02_firmware_projects/prj_04_adc_dma_bm_eth/build/selftest-uart

# b) Flashear en la placa NUCLEO-H755ZI-Q:
py 02_firmware_projects/prj_04_adc_dma_bm_eth/tools/flash.py --preset selftest-uart

# c) Monitorear la salida de selftest de CM4 por puerto serie (115200 baud, 8N1):
py tools/monitor.py --baud 115200

# Salida esperada en consola:
# [m4_uart_test] jumper=1 bm_init=1,1,1 bm_loop=1,1,1 ll=1,1 ore=1
```

#### 6. Prueba de estabilidad de warm resets sobre selftest-uart
```powershell
py tools\run_warm_resets.py --project 03_firmware_templates/prj_01_template_dualcore --preset selftest-uart --out 03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_fdbf306.txt --title "selftest-uart 20 warm resets" --count 20
```

#### 7. Flasheo final de imagen de producción debug y retorno a estado seguro S2
```powershell
py 03_firmware_templates/prj_01_template_dualcore/tools/flash.py --preset debug
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
- [x] Ejecución en hardware real con captura [`uart_dev_hw_fdbf306.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_hw_fdbf306.txt) con `FW VERDICT=PASS`, `HOST VERDICT=PASS` y 12/12 pasos de la tabla Oracle en PASS.
- [x] 20/20 warm resets en `selftest-uart` con captura [`uart_dev_warm_resets_fdbf306.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/uart_dev_warm_resets_fdbf306.txt) alcanzando `(CPU2 alive)`.
- [x] Placa flasheada con preset de producción `debug` y AD2 en estado seguro S2.
- [x] Desviaciones `D5` (`TakeErrors`) y `D6` (`UART_HAL_IsValidInstance`) formalmente documentadas en §3.
- [x] Citas a archivos, líneas, conectores y constantes verificadas contra HEAD (`fdbf306`).
- [x] Ninguna escritura realizada en la configuración de git ni enlaces absolutos `file:///`.

F1 CORREGIDA 4 PARA AUDITORIA fdbf306
