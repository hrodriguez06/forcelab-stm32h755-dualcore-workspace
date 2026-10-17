# Informe de Implementación: Fase F1 del Driver `04_uart`

**Fecha:** 2026-10-17  
**Rama:** `driver/uart-audit`  
**Base:** `main` (`b41c4a7`)  
**Último commit de código / herramienta:** `76eb6d7`  
**Estado de la fase:** Entregado para auditoría  

---

## 1. Resumen de la Entrega

Se ha completado la fase F1 según lo especificado en [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1) e [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md):
- Correcciones U1 a U6 del driver (`uart_bm.c`, `uart_ll.c`, `uart_hal.c`). Las cabeceras `04_uart/*.h` únicamente recibieron cambios aditivos sin alterar firmas existentes.
- Preset `selftest-uart` en `prj_01_template_dualcore` (CM7, CM4, Common, CMake), implementando los pasos UT0–UT10 bajo el marco de banco del AD2 con guardas eléctricas (S2, S4, S7, SU1–SU4).
- Host `03_firmware_templates/prj_01_template_dualcore/tools/uart_hw_check.py` con oráculos, guardas SU1–SU4, decodificador lógico bit a bit y suite de pruebas mock (`--mock-test`).
- Extensión aditiva de `tools/ad2.py` con la guarda SU1 (`AD2_DIO_UART_RX_INJECT`, `ad2_set_uart_oe`, `AD2.set_uart_oe`), restringiendo DIO15 como salida exclusivamente a UT5 y preservando intactas las guardas S3, S4, S5 y S11.
- Herramienta de verificación sin placa `tools/uart_checks.py` ejecutando T1 (11 configuraciones y selftests con recuento estricto de avisos del compilador y control negativo con `-Wconversion`) y T2 (verificación de diff de cabeceras y ausencia de símbolos de selftest en ELFs de producción, ambos con controles negativos).
- Capturas sin placa generadas y commiteadas en `docs/`.

### Commits Realizados en la Fase F1
1. `006c62b`: `fix(uart): return the console MSP pin status from UART_HAL_Init (U1)`
2. `5322ece`: `fix(uart): validate every LL constant before touching hardware (U2, U3)`
3. `c88bc5e`: `fix(uart): reset CR1-CR3 on LL init (U4)`
4. `e0a530a`: `fix(uart): check the OVER8 divider the hardware applies in LL (U5)`
5. `9b9c1a0`: `fix(uart): report a busy GPIO lock from UART_HAL_InitEx (U6)`
6. `593d720`: `test(prj01): add the selftest-uart preset with UT0-UT10`
7. `ae3e620`: `test(prj01): add uart_hw_check.py and its mock`
8. `76eb6d7`: `feat(tools): add uart_checks.py`

---

## 2. Detalle de Correcciones U1–U6

### U1: Retorno del estado del MSP en `UART_HAL_Init()`
- **Archivo y líneas:** [`01_shared_drivers/04_uart/uart_hal.c:162-176`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_hal.c#L162-L176) (asimismo `uart_hal.c:101-112`)
- **Antes (`b41c4a7`):**
  ```c
  HAL_UART_MspInit(&g_console);
  return HAL_UART_Init(huart);
  ```
  `HAL_UART_Init` llamaba a `MspInit` internamente; si la reserva de pines GPIO fallaba (por ejemplo por lock cross-core ocupado), el error se descartaba y `UART_HAL_Init` devolvía `HAL_OK`.
- **Después (`006c62b`):**
  Se introduce `s_msp_gpio_status` estático a nivel de archivo reiniciado antes de invocar `HAL_UART_Init(huart)`. En `UART_HAL_MspInit_User()`:
  ```c
  HAL_StatusTypeDef st_tx = GPIO_HAL_Init(BOARD_CONSOLE_TX_PORT, &gpio);
  if ((st_tx != HAL_OK) && (s_msp_gpio_status == HAL_OK)) {
      s_msp_gpio_status = st_tx;
  }
  HAL_StatusTypeDef st_rx = GPIO_HAL_Init(BOARD_CONSOLE_RX_PORT, &gpio);
  if ((st_rx != HAL_OK) && (s_msp_gpio_status == HAL_OK)) {
      s_msp_gpio_status = st_rx;
  }
  ```
  Y en `UART_HAL_Init()`:
  ```c
  s_msp_gpio_status = HAL_OK;
  HAL_StatusTypeDef status = HAL_UART_Init(huart);
  if (status != HAL_OK) {
      return status;
  }
  if (s_msp_gpio_status != HAL_OK) {
      return s_msp_gpio_status;
  }
  return HAL_OK;
  ```
- **Justificación:** Previene declarar inicializada la consola cuando los pines no pudieron configurarse en la capa GPIO compartida.

---

### U2 y U3: Validación previa exhaustiva en `UART_LL_Init()` sin tocar hardware
- **Archivo y líneas:** [`01_shared_drivers/04_uart/uart_ll.c:192-280`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L192-L280)
- **Antes (`b41c4a7`):**
  `UART_LL_Init()` configuraba pines GPIO y habilitaba el reloj en RCC antes de validar si el baudrate era alcanzable o si los parámetros eran válidos. Además, constantes inválidas (o constantes de `uart_bm` pasadas por confusión) pasaban directamente a los registros sin validación de conjunto cerrado.
- **Después (`5322ece`):**
  Se estructura `UART_LL_Init()` en Fase 1 (Validación y cálculo puro) y Fase 2 (Aplicación en hardware).
  En Fase 1:
  - `data_bits` validado estrictamente contra `LL_USART_DATAWIDTH_7B`, `8B`, `9B`.
  - `stop_bits` validado contra `LL_USART_STOPBITS_0_5`, `1`, `1_5`, `2`. Rechazo de 0.5 y 1.5 en LPUART (RM0399 §52.4.4).
  - `parity` validado contra `LL_USART_PARITY_NONE`, `EVEN`, `ODD`.
  - `hw_flow` validado contra `LL_USART_HWCONTROL_NONE`, `RTS`, `CTS`, `RTS_CTS`.
  - `oversampling` validado contra `LL_USART_OVERSAMPLING_16`, `8`. Rechazo de OVER8 en LPUART (RM0399 §52.4.4) y de 7B con OVER8 (RM0399 §51.5.4).
  - Validación de punteros y límites de pines (`pin_idx <= 15`, puertos válidos) sin escribir en hardware.
  - Cálculo de divisor de baudrate y verificación de rango (±2% y divisores entre 16 y 0xFFFF para USART, o 0x300 y 0xFFFFF para LPUART).
- **Justificación:** Principio de estado transaccional: una llamada rechazada por parámetros inválidos no debe alterar `CR1–CR3`, `BRR`, `PRESC`, registros GPIO ni RCC.

---

### U4: Reinicio de `CR1–CR3` a estado conocido en `UART_LL_Init()`
- **Archivo y líneas:** [`01_shared_drivers/04_uart/uart_ll.c:323-330`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L323-L330)
- **Antes (`b41c4a7`):**
  `LL_USART_Init()` solo aplicaba una máscara de configuración sobre `CR1–CR3`, dejando bits residuales de ejecuciones previas (como habilitaciones de interrupciones o DMA).
- **Después (`c88bc5e`):**
  ```c
  LL_USART_Disable(u);
  u->CR1 = 0U;
  u->CR2 = 0U;
  u->CR3 = 0U;
  ```
- **Justificación:** RM0399 §51.5.2 / §52.4.2 especifica que la reconfiguración debe realizarse con `UE = 0`. Reiniciar explícitamente `CR1–CR3` garantiza un estado determinista libre de flags residuales.

---

### U5: Cálculo del error de baudrate sobre el divisor real del hardware en OVER8 (LL)
- **Archivo y líneas:** [`01_shared_drivers/04_uart/uart_ll.c:154-164`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_ll.c#L154-L164)
- **Antes (`b41c4a7`):**
  En OVER8, el cálculo del error relativo de baudrate evaluaba `actual = (2U * ker_p) / usartdiv`, usando `usartdiv` sin tener en cuenta que el hardware ignora `BRR[3]` y concatena `BRR[2:0]` desplazado (`(BRR[15:4] << 4) | (BRR[2:0] << 1)`), produciendo desviaciones en divisores impares pequeños.
- **Después (`e0a530a`):**
  ```c
  uint32_t brr = (usartdiv & 0xFFF0UL) | ((usartdiv & 0x000FUL) >> 1);
  uint32_t actual_div = (brr & 0xFFF0UL) | ((brr & 0x0007UL) << 1);
  if (actual_div == 0U) {
      continue;
  }
  actual = (2U * ker_p) / actual_div;
  ```
- **Justificación:** Idéntico tratamiento al implementado en `uart_bm.c:127-133` conforme a RM0399 §51.5.5.

---

### U6: Retorno de `HAL_BUSY` ante colisión de lock GPIO en `UART_HAL_InitEx()`
- **Archivo y líneas:** [`01_shared_drivers/04_uart/uart_hal.c:205-236`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/uart_hal.c#L205-L236)
- **Antes (`b41c4a7`):**
  `UART_HAL_InitEx()` no comprobaba el retorno de la inicialización de pines, o bien devolvía `HAL_ERROR` genérico, sin distinguir contención del semáforo hardware de configuración de pines.
- **Después (`9b9c1a0`):**
  La configuración de pines valida primero punteros y puertos sin tocar hardware, y luego invoca `GPIO_BM_ConfigPin()`. Si el semáforo cross-core está tomado:
  ```c
  GpioBm_Status_t st = GPIO_BM_ConfigPin(&pin_cfg);
  if (st != GPIO_BM_OK) {
      return (st == GPIO_BM_ERR_BUSY) ? HAL_BUSY : HAL_ERROR;
  }
  ```
  El reloj del periférico solo se enciende si la configuración de los pines fue exitosa.
- **Justificación:** Permite que las llamadas de inicialización reconozcan que el bloqueo es temporal por contención cross-core y puedan reintentar de forma segura.

---

## 3. Selftest en Banco: Pasos UT0 a UT10

> [!IMPORTANT]
> **Estado de verificación en placa física:** Todos los pasos UT0 a UT10 sobre hardware real se declaran formalmente como **NO VERIFICADOS** en esta fase F1, quedando pendientes de ejecución por el auditor sobre la placa NUCLEO-H755ZI-Q y el Analog Discovery 2.
> La lógica de firmware y la suite de oráculos del host han sido verificadas exhaustivamente sin placa mediante el mock del host (`--mock-test`).

| Paso | Qué prueba el firmware | Qué imprime la consola | Qué comprueba el host (AD2) |
|---|---|---|---|
| **UT0** | Reposo del banco: pines PD3–PD6 en entrada sin pull; PE2 entrada pull-down, PE7 salida baja. | `[uart] UT0 bench resting state (PD6 high) -> PASS` | AD2 DIO15 lee reposo alto (1) en PD6 (a través de JU1 desde PD5). |
| **UT0_CTRL** | Control negativo de reposo: CM7 conduce PD5 a nivel bajo mediante GPIO. | `[uart] UT0_CTRL negative control (PD5 low) -> PASS` | AD2 DIO15 lee nivel bajo (0) en PD6. Al terminar, PD5 vuelve a entrada. |
| **UT1** | Rechazo de parámetros en BM, LL y HAL. Instantánea de registros antes y después (`CR1–CR3`, `BRR`, `PRESC`, `MODER`, `AFR[0]`, `RCC_APB1LENR`). Control positivo mutado. | `[uart] UT1 parameter rejection across BM, LL, HAL (U2, U3) -> PASS` | Comprueba reporte exacto `PASS` del firmware tras comprobar que los registros se mantuvieron inalterados (U3). |
| **UT2** | Fórmulas `PRESC`/`BRR` en instancias sin pines externos (UART4, UART5, UART8, USART1, USART6, LPUART1) bajo combinaciones de `(f_ker, baud, oversampling)`. | `[uart] UT2 baudrate divider and prescaler calculations (U5, LPUART1) -> PASS` | Oráculo independiente en Python calcula divisor y verifica tolerancia ±2% sobre `BRR` reportado. |
| **UT3** | Codificación en el cable en USART2 con BM: 8N1, 8E1, 8O1, 7E1, 7N1, 9N1, 8N2 a 9600, 115200 y 921600 (OVER8). | `[uart] UT3 format=... baud=... over8=... BRR=...`<br>`[uart] UT3 wire format encoding across BM, LL, HAL -> PASS` | Analizador lógico en DIO15 decodifica bit a bit inicio, datos LSB, paridad y parada. Mide tiempo contra MCO1 (DIO14). |
| **UT4** | Bucle de retorno JU1 (PD5 -> 1 kΩ -> PD6) en CM7 (BM y LL con IRQ a ring buffer) y en CM4 (LL con IRQ a ring buffer vía `UST_CMD_UT4`). | `[uart] UT4 loopback reception (JU1) on CM7 and CM4 -> PASS` | Comprueba envío y recepción byte a byte idénticos en ambos núcleos y conteo de tramas por el LA. |
| **UT4_CM4** | Subpaso de ejecución de UT4 en CM4. | `[uart] UT4 loopback reception (JU1) on CM7 and CM4 -> PASS` | Verifica respuesta `UST_AREA->m4_ut4_ok == 1`. |
| **UT5** | Inyección de errores por el AD2 en PD6 vía DIO15 (SU1–SU3): paridad errónea (PE), bit de parada en 0/break (FE), ruido centrado en bit (NE), sobreescritura sin lectura (ORE). | `[uart] UT5 error injection detection (PE, FE, NE, ORE) -> PASS` | AD2 inyecta tramas anómalas con OE activo solo durante la inyección. Comprueba que el firmware registra y consume cada flag una sola vez mediante `TakeErrors()`. |
| **UT6** | Control de flujo hardware RTS/CTS vía JU2 (PD4 RTS -> PD3 CTS) con FIFO. Envío de 16 bytes sin lectura; TX se suspende sin generar ORE. Al drenar RX, TX se reanuda. | `[uart] UT6 RTS/CTS hardware flow control via JU2 -> PASS` | Comprueba que no se genera ORE con control de flujo activo y que el LA cuenta las tramas transmitidas en dos bloques. |
| **UT7** | Inicialización con `UART_HAL_InitEx` en USART2 (CM7). Contención de lock GPIO: CM4 toma `BOARD_GPIO_HSEM_ID` (`UST_CMD_LOCK_ACQUIRE`), llamada devuelve `HAL_BUSY` (U6). CM4 libera, llamada devuelve `HAL_OK`. Transmisión 8E1. | `[uart] UT7 UART_HAL_InitEx and GPIO lock contention U6 -> PASS` | Verifica instantánea de pines PD5/PD6 (AF7, pull-up, velocidad), retorno `HAL_BUSY` bajo contención y `HAL_OK` tras liberación. |
| **UT8** | Workaround de errata DMA (`UART_HAL_CheckDmaErrata`) en vivo con DMA1 Stream 0. Verifica bit 20 a 1 y leído; rechazos ante puntero nulo, canal BDMA, petición no-UART y stream activo (`HAL_BUSY`). | `[uart] UT8 UART_HAL_CheckDmaErrata in-vivo with DMA1 (bit 20, busy, error) -> PASS` | Comprueba bit 20 en `DMA_SxCR` y rechazos conformes a ES0445. |
| **UT9** | Consola `UART_HAL_Init()` (U1). CM4 adquiere el lock GPIO: llamada devuelve `HAL_BUSY`. CM4 libera: llamada devuelve `HAL_OK`. | `[uart] UT9 UART_HAL_Init console MSP GPIO lock status U1 -> PASS` | Comprueba valor devuelto exacto. Bajo `--expect-control-u1` en control `b41c4a7`, detecta el fallo de U1 (retorno erróneo `HAL_OK`) y lo marca como esperado. |
| **UT10** | Regresión de `prj_04`: CM4 ejecuta el selftest de `prj_04` (`run_cm4_ut10()`) a través del bucle JU1 con la resistencia de 1 kΩ. | `[uart] [m4_uart_test] bm_loop=1,1,1 ll=1,1 ore=1`<br>`[uart] UT10 prj_04 regression loopback with 1k resistor -> PASS` | Comprueba que la resistencia de 1 kΩ de JU1 no degrada las señales de las pruebas de regresión previas. |
| **DONE** | Fin de sesión: CM7 envía `UST_CMD_DONE` a CM4, restaura banco seguro y reporta veredicto global. | `[uart] === 04_uart selftest finished: 0 failed ===`<br>`[uart] FW VERDICT=PASS` | Host valida `FW VERDICT=PASS` y emite `HOST VERDICT=PASS`. |

---

## 4. Resultados de Verificación sin Placa

### T1: Matriz de Compilación (8 Configuraciones + 3 Selftests)
- **Herramienta:** `tools/uart_checks.py --test t1` (integrada en `--test all`).
- **Criterio:** Compilación limpia con `cmake --build --clean-first`, 0 avisos del compilador, 0 errores, binarios generados en la ejecución actual.
- **Resultado:** **PASS** (11/11 compilaciones exitosas, 0 avisos):
  - `prj_01_template_dualcore` [`debug`]: exit=0, warnings=0
  - `prj_01_template_dualcore` [`release`]: exit=0, warnings=0
  - `prj_02_eth_lwip_dualcore` [`debug`]: exit=0, warnings=0
  - `prj_02_eth_lwip_dualcore` [`release`]: exit=0, warnings=0
  - `prj_03_adc_dma_eth` [`debug`]: exit=0, warnings=0
  - `prj_03_adc_dma_eth` [`release`]: exit=0, warnings=0
  - `prj_04_adc_dma_bm_eth` [`debug`]: exit=0, warnings=0
  - `prj_04_adc_dma_bm_eth` [`release`]: exit=0, warnings=0
  - `prj_01_template_dualcore` [`selftest-uart`]: exit=0, warnings=0
  - `prj_01_template_dualcore` [`selftest-gpio`]: exit=0, warnings=0
  - `prj_04_adc_dma_bm_eth` [`selftest-uart`]: exit=0, warnings=0
- **Control Negativo de T1:**
  Compilación en directorio temporal fuera del repositorio con `-DCMAKE_C_FLAGS=-Wconversion` evaluada mediante la **misma función** `evaluate_build()`:
  - Resultado: `warnings=224`, evaluación retornó `False` como se esperaba (**PASS**).
- **Captura commiteada:** [`01_shared_drivers/04_uart/docs/uart_checks_76eb6d7.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/docs/uart_checks_76eb6d7.txt).

---

### T2: Integridad de Cabeceras y Aislamiento de Símbolos
- **Herramienta:** `tools/uart_checks.py --test t2` (integrada en `--test all`).
- **Comprobación 1 (Cabeceras):** `git diff -U0 b41c4a7 -- 01_shared_drivers/04_uart/*.h` únicamente agrega líneas (`deleted_lines=0`).
  - Resultado: **PASS**.
  - Control negativo: Diff sintético con una eliminación evaluado por la misma función `evaluate_header_diff()` fue rechazado con 1 línea eliminada (**PASS**).
- **Comprobación 2 (Símbolos):** Extracción de símbolos con `arm-none-eabi-nm -g` en los 8 ELFs de producción (`release`).
  - Símbolos de selftest (`UT*`, `uart_selftest*`, `ust_*`): 0 encontrados en los 8 ELFs (**PASS**).
  - Control negativo: Lista mutada con símbolo `UART_Selftest_CM7_Run` evaluada por la misma función `evaluate_symbols()` fue rechazada (**PASS**).
- **Captura commiteada:** [`01_shared_drivers/04_uart/docs/uart_checks_76eb6d7.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/04_uart/docs/uart_checks_76eb6d7.txt).

---

### Prueba del Host con Mock (`uart_hw_check.py --mock-test`)
- **Comprobación:** Ejecución completa sin hardware simulando DWF y firmware console.
- **Fase 1 (Guardas SU1):**
  - Salida permitida para `HOST_REQ` (DIO12): OK.
  - Rechazo de DIO15 fuera de UT5: OK (`AD2SafetyViolation` capturada).
  - DIO15 permitido en UT5: OK.
  - Rechazo de otras líneas prohibidas en UT5: OK (`AD2SafetyViolation` capturada).
- **Fase 2 (Sesión completa UT0–UT10):** Todos los 14 pasos completados con handshake y oráculos conformes (**PASS**).
- **Fase 3 (Control Negativo 1 - Paridad Errónea):** Trama con paridad incorrecta inyectada en la muestra del LA: el decodificador la marca con error (**PASS**).
- **Fase 4 (Control Negativo 2 - DIO15 fuera de UT5):** Intento de activar OE de DIO15 en paso UT3 genera `AD2SafetyViolation` (**PASS**).
- **Fase 5 (Control Negativo 3 - Conteo Insuficiente de Tramas):** 5 tramas decodificadas frente a 6 esperadas detectado como fallo (**PASS**).
- **Veredicto:** `OVERALL MOCK VERDICT: PASS`, `HOST VERDICT=PASS`.
- **Captura commiteada:** [`03_firmware_templates/prj_01_template_dualcore/docs/uart_mock_593d720.txt`](file:///c:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/03_firmware_templates/prj_01_template_dualcore/docs/uart_mock_593d720.txt).

---

## 5. Instrucciones y Comandos para el Auditor

Para la ejecución de la auditoría sobre la placa física y el Analog Discovery 2:

### 1. Preparación del Banco (Responsable / Auditor)
1. **Conexión a tierra:** Conectar primero el cable GND del AD2 al pin GND del conector CN9.
2. **Conexiones de USART2 en CN9 (UM2408 `USART_B`):**
   - **JU1:** Conectar PD5 (CN9-6, `USART2_TX`) con una **resistencia serie de 1 kΩ** hacia PD6 (CN9-4, `USART2_RX`). *Retirar el cable directo previo de `prj_04`/`02_dma` si aún estaba instalado.*
   - **JU2:** Conectar PD4 (CN9-8, `USART2_RTS`) directamente hacia PD3 (CN9-10, `USART2_CTS`).
   - **DIO15:** Conectar la sonda digital DIO15 del AD2 al nodo PD6 (CN9-4).
3. **Señales de sincronismo y reloj:**
   - **DIO12:** PE2 (CN11-1, `HOST_REQ`).
   - **DIO13:** PE7 (CN11-13, `MCU_ACK`).
   - **DIO14:** PA8 (CN12-23, `MCO1` reloj de referencia).
4. **Aislamiento:** Asegurar que los jumpers J2/J3 de la placa de pruebas GPIO estén retirados y el cable USB de la NUCLEO-F334R8 desconectado.

### 2. Comandos de Verificación en Banco
En consola PowerShell con herramientas cargadas (`. .\tools\env.ps1`):

```powershell
# 1. Compilar y flashear el selftest de UART en prj_01
cd 03_firmware_templates\prj_01_template_dualcore
cmake --preset selftest-uart
cmake --build --preset selftest-uart --clean-first
py tools\flash.py --preset selftest-uart

# 2. Primera ejecucion en banco
py tools\uart_hw_check.py

# 3. Segunda ejecucion en banco (reproducibilidad)
py tools\uart_hw_check.py

# 4. Prueba UT9 sobre el commit de control b41c4a7 (demostracion de la correccion U1)
# Compilar control b41c4a7 con selftest-uart y ejecutar con flag de control esperado:
py tools\uart_hw_check.py --expect-control-u1

# 5. Prueba de estabilidad ante warm resets (20/20)
py tools\run_warm_resets.py --preset selftest-uart --count 20

# 6. Regresion de prj_04 con el bucle JU1 de 1 kOhm
cd ..\..\02_firmware_projects\prj_04_adc_dma_bm_eth
cmake --preset selftest-uart
cmake --build --preset selftest-uart --clean-first
py tools\flash.py --preset selftest-uart
py tools\monitor.py --timeout 15
```

---

## 6. Desviaciones Respecto al Plan

No existen desviaciones técnicas respecto a las especificaciones aprobadas del plan de implementación (`IMPLEMENTATION_PLAN.md` rev. 1):
- Se implementó la Decisión 1 con la Opción A (`prj_01` con preset `selftest-uart`).
- Se implementó la Decisión 2 con la Opción A (banco con JU1 de 1 kΩ, JU2 y DIO15 oráculo / inyección).
- Se implementó la Decisión 3 con la Opción A (LPUART1 verificado en UT2 a nivel de cálculo de `PRESC`/`BRR` en instancia sin pines externos).
- Se implementó la Decisión 4 (los documentos de proceso se conservan hasta el cierre final).

---

## 7. Checklist Final de Entrega

- [x] `git status --porcelain -u`: árbol limpio tras commitear el informe.
- [x] `git diff b41c4a7 --stat`: modificado únicamente en las rutas autorizadas de §2.1.
- [x] `git diff b41c4a7 -- '01_shared_drivers/04_uart/*.h'`: únicamente cambios aditivos.
- [x] T1 y T2 pasan con 0 avisos del compilador contados desde la salida de `cmake --build`.
- [x] En `tools/ad2.py`, únicamente agregada la máscara y funciones de SU1 sin alterar S3, S4, S5 ni S11.
- [x] Todos los controles negativos pasan por la misma función y fallan como corresponde.
- [x] Todas las citas a archivos, líneas y hashes comprobadas contra HEAD.

F1 ENTREGADA PARA AUDITORIA 76eb6d7
