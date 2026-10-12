# Informe de implementación 08_eth (E1, E2, E5, E8)

Implementador: Claude (Claude Code), para Hector Rodriguez · Rama: driver/eth · Plan: IMPLEMENTATION_PLAN.md revisión 1

## Resumen

**F1 está entregada para auditoría**: cierra las limitaciones E1, E2, E5 y E8 en `08_eth/eth_hal.c`, `eth_hal.h` y `CMakeLists.txt` sin alterar la API congelada del driver ni la definición de `eth_driver_stats_t`.

Las pruebas T1–T6 pasan al 100%:
- **T1:** Matriz `build_all.ps1 -Clean` 8/8 exitosa (`docs/eth_f1_t1_build_all_709b869.txt`); compilaciones con `-Werror` de `prj_02`, `prj_03` y `prj_04` (debug y release, 6/6) con 0 avisos (`docs/eth_f1_t1_werror_709b869.txt`); tamaños de `prj_01` idénticos byte a byte a `2481396`.
- **T2:** Las 19 variantes de `board_def.h` incompletas o ausentes fallan con `#error` citando explícitamente la macro faltante y la Regla 1; los dos controles negativos (archivo completo y compilación sin `HAL_ETH_MODULE_ENABLED`) compilan con 0 errores (`docs/eth_f1_t2_t4_checks_709b869.txt`).
- **T3:** Configuración `USE_HAL_ETH_REGISTER_CALLBACKS 1U` falla con `#error`; los dos controles negativos (`0U` y sin definir) compilan con 0 errores (`docs/eth_f1_t2_t4_checks_709b869.txt`).
- **T4:** Desensamblado de `eth_hal.c.obj` confirma 5 bucles atómicos de lectura-modificación-escritura (`ldrex`/`strex`), y la tabla de símbolos importados muestra `GPIO_HAL_Init` presente y `HAL_GPIO_Init` ausente; contrastado con el objeto base de `7154af6` (`docs/eth_f1_t2_t4_checks_709b869.txt`).
- **T5:** Volcado SWD en vivo (`mode=HOTPLUG`) de `MODER`, `OTYPER`, `OSPEEDR`, `PUPDR`, `AFRL`/`AFRH` de los puertos GPIOA, GPIOB, GPIOC y GPIOG con `prj_02` corriendo en la placa: los 9 pines RMII están configurados exactamente en AF11, Push-Pull, Very High Speed, No Pull; los 4 pines de control no utilizados en dichos puertos permanecen en sus valores de reset (`docs/eth_f1_t5_swd_gpio_709b869.txt`).
- **T6:** Camino de datos validado concurrentemente con `net_check.py` y `monitor.py` en hardware: ICMP 4/4 (0% pérdida); barrido de tamaños UDP (1, 25, 512, 1024 y 1472 B) con eco byte a byte exacto; ráfaga de 100/100 datagramas consecutivos sin pérdidas; contadores del firmware en `rx_nobuf=0`, `tx_nobuf=0`, `dma_err=0` de inicio a fin (`docs/eth_f1_t6_netcheck_709b869.txt`).

| Fase | Estado | Último commit | Evidencia / Capturas |
| --- | --- | --- | --- |
| F1 | Entregada para auditoría | `709b869` (código y herramientas) | `docs/eth_f1_t1_build_all_709b869.txt`<br>`docs/eth_f1_t1_werror_709b869.txt`<br>`docs/eth_f1_t2_t4_checks_709b869.txt`<br>`docs/eth_f1_t5_swd_gpio_709b869.txt`<br>`docs/eth_f1_t6_netcheck_709b869.txt` |
| Cierre | Sin empezar (espera a que `AUDIT.md` apruebe F1) | — | — |

---

## F1

### 5.1 Commits

Rango de la entrega: `92e2c77..` el commit que añade este informe, en la rama `driver/eth`.

| Commit | Asunto | Limitación / Preocupación |
| --- | --- | --- |
| `d4feb24` | `fix(eth): address limitations E1, E2, E5, and E8 in eth_hal` | E1, E2, E5, E8 |
| `709b869` | `feat(tools): add eth_build_checks.py for T2-T4 build and object validation` | T2, T3, T4 |

---

### 5.2 Builds y tamaños

Todos los números provienen de capturas en `01_shared_drivers/08_eth/docs/` y de los ejecutables generados a partir de `709b869` con el árbol limpio.

#### 5.2.1 Matriz CI (`build_all.ps1 -Clean`)
Captura: `docs/eth_f1_t1_build_all_709b869.txt`. Los 8 builds completaron con estado `PASS`.

| Proyecto | Preset | Núcleo | Estado | Text (B) | Data (B) | Bss (B) | Total (B) | Tiempo |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| `prj_01_template_dualcore` | debug | CM7 | PASS | 29304 | 132 | 55668 | 85104 | 26.4s |
| `prj_01_template_dualcore` | debug | CM4 | PASS | 3456 | 24 | 12560 | 16040 | 26.4s |
| `prj_01_template_dualcore` | release | CM7 | PASS | 28356 | 132 | 55672 | 84160 | 17.7s |
| `prj_01_template_dualcore` | release | CM4 | PASS | 3112 | 24 | 12560 | 15696 | 17.7s |
| `prj_02_eth_lwip_dualcore` | debug | CM7 | PASS | 87136 | 152 | 120088 | 207376 | 30.8s |
| `prj_02_eth_lwip_dualcore` | debug | CM4 | PASS | 2580 | 24 | 12488 | 15092 | 30.8s |
| `prj_02_eth_lwip_dualcore` | release | CM7 | PASS | 83072 | 152 | 120096 | 203320 | 23.5s |
| `prj_02_eth_lwip_dualcore` | release | CM4 | PASS | 2332 | 24 | 12488 | 14844 | 23.5s |
| `prj_03_adc_dma_eth` | debug | CM7 | PASS | 88552 | 152 | 121216 | 209920 | 32.2s |
| `prj_03_adc_dma_eth` | debug | CM4 | PASS | 20944 | 24 | 16184 | 37152 | 32.2s |
| `prj_03_adc_dma_eth` | release | CM7 | PASS | 84424 | 152 | 121216 | 205792 | 24.7s |
| `prj_03_adc_dma_eth` | release | CM4 | PASS | 18556 | 24 | 16184 | 34764 | 24.7s |
| `prj_04_adc_dma_bm_eth` | debug | CM7 | PASS | 90336 | 156 | 121228 | 211720 | 32.1s |
| `prj_04_adc_dma_bm_eth` | debug | CM4 | PASS | 9688 | 28 | 15888 | 25604 | 32.1s |
| `prj_04_adc_dma_bm_eth` | release | CM7 | PASS | 86336 | 156 | 121232 | 207724 | 23.7s |
| `prj_04_adc_dma_bm_eth` | release | CM4 | PASS | 9032 | 28 | 15888 | 24948 | 23.7s |

**Comprobación de `prj_01` frente a `2481396`:**
Los cuatro binarios de `prj_01` (CM7 debug, CM4 debug, CM7 release, CM4 release) coinciden byte a byte exactamente en `text`, `data` y `bss` con los valores de referencia del commit `2481396`. La inclusión incondicional de `board_def.h` dentro de `HAL_ETH_MODULE_ENABLED` preserva íntegra la compilación en blanco de `eth_hal.c` cuando Ethernet está desactivado.

#### 5.2.2 Compilación con `-Werror`
Captura: `docs/eth_f1_t1_werror_709b869.txt`.
Se compilaron las 6 configuraciones consumidoras con `-DCMAKE_C_FLAGS=-Werror`:
- `prj_02_eth_lwip_dualcore` (debug y release): exit 0, warnings=0.
- `prj_03_adc_dma_eth` (debug y release): exit 0, warnings=0.
- `prj_04_adc_dma_bm_eth` (debug y release): exit 0, warnings=0.

#### 5.2.3 Tamaño del objeto `eth_hal.c.obj` antes y después
Medido con `arm-none-eabi-size`:

| Proyecto | Configuración | `text` antes (`7154af6`) | `text` después (`709b869`) | $\Delta$ text | `data` | `bss` antes | `bss` después | $\Delta$ bss |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `prj_02` | debug | 1888 B | 1936 B | +48 B | 18624 B | 366 B | 367 B | +1 B |
| `prj_02` | release | 1792 B | 1848 B | +56 B | 18624 B | 366 B | 367 B | +1 B |
| `prj_03` | debug | 1888 B | 1936 B | +48 B | 18624 B | 366 B | 367 B | +1 B |
| `prj_03` | release | 1792 B | 1848 B | +56 B | 18624 B | 366 B | 367 B | +1 B |
| `prj_04` | debug | 1888 B | 1936 B | +48 B | 18624 B | 366 B | 367 B | +1 B |
| `prj_04` | release | 1792 B | 1848 B | +56 B | 18624 B | 366 B | 367 B | +1 B |

*Nota explicativa del crecimiento:*
- `.bss`: +1 B debido a la variable estática `s_msp_gpio_status` (`HAL_StatusTypeDef`, `uint8_t`), utilizada para almacenar el primer estado de error de inicialización de pines GPIO en MSP (decisión 4).
- `.text`: +48 B en debug / +56 B en release debido a los 5 bucles atómicos `ldrex`/`strex` para `g_stats` (E5) y a la verificación del estado devuelto por `GPIO_HAL_Init` (E8).
- `.data`: 18624 B sin variación (tablas estáticas de descriptores y constantes).

---

### 5.3 Pruebas

| ID | Qué prueba | Oráculo | Control negativo | Captura | Veredicto |
| --- | --- | --- | --- | --- | --- |
| **T1** | Compilaciones: matriz `build_all.ps1 -Clean` 8/8; `-Werror` en 6 configuraciones; `prj_01` inalterado frente a `2481396`. | Códigos de salida del compilador (exit 0, 0 avisos); reporte de tamaños de `arm-none-eabi-size`. | Objeto base de `7154af6` para contraste de tamaños. | `docs/eth_f1_t1_build_all_709b869.txt`<br>`docs/eth_f1_t1_werror_709b869.txt` | **PASS** |
| **T2** | E1: Inclusión obligatoria de `board_def.h` y 18 `#error` para cada macro `BOARD_ETH_<SIG>_PORT` y `_PIN`. | Salida de compilación con las flags de `prj_02` CM7: cada variante falla con el `#error` citando el nombre exacto de la macro y la Regla 1. | Control 1: `board_def.h` completo compila con 0 errores.<br>Control 2: sin `HAL_ETH_MODULE_ENABLED` compila a objeto vacío con 0 errores. | `docs/eth_f1_t2_t4_checks_709b869.txt` | **PASS** (19/19 variantes fallan; 2 controles pasan) |
| **T3** | E2: Verificación de `USE_HAL_ETH_REGISTER_CALLBACKS != 0`. | Compilación con `USE_HAL_ETH_REGISTER_CALLBACKS 1U` falla con `#error "USE_HAL_ETH_REGISTER_CALLBACKS must be 0 ..."`. | Control 1: con `0U` compila con 0 errores.<br>Control 2: sin definir compila con 0 errores. | `docs/eth_f1_t2_t4_checks_709b869.txt` | **PASS** (variante falla; 2 controles pasan) |
| **T4** | E5 y E8: Desensamblado e importación de símbolos de `eth_hal.c.obj`. | `arm-none-eabi-objdump -d`: exactamente 5 bucles `ldrex`/`strex` para contadores de `g_stats`. `arm-none-eabi-nm -u`: importa `GPIO_HAL_Init` y no contiene `HAL_GPIO_Init`. | Control: objeto de `7154af6` contiene 0 instrucciones `ldrex`/`strex` (operaciones no atómicas `ldr`/`add`/`str`), e importa `HAL_GPIO_Init` sin referencia a `GPIO_HAL_Init`. | `docs/eth_f1_t2_t4_checks_709b869.txt` | **PASS** (objeto actual y control contrastados) |
| **T5** | E8 en hardware: Configuración de registros de puertos GPIOA, GPIOB, GPIOC y GPIOG. | Lectura directa por SWD en caliente (`mode=HOTPLUG`) de `MODER`, `OTYPER`, `OSPEEDR`, `PUPDR` y `AFRL`/`AFRH`. Los 9 pines RMII configuran: AF (10), PP (0), Very High Speed (11), No Pull (00), AF11 (`0xB`). | Pines no utilizados en los mismos puertos (PA0, PB12, PC0, PG0) permanecen inalterados en sus estados de reset del silicio. | `docs/eth_f1_t5_swd_gpio_709b869.txt` | **PASS** (9/9 pines RMII exactos; controles inalterados) |
| **T6** | Camino de datos Ethernet (README §9.3): interacción host-placa bajo tráfico de red real. | `tools/net_check.py`: ICMP 4/4 respuestas; barrido de tamaños UDP (1..1472 B) idénticos byte a byte; ráfaga de 100/100 datagramas sin pérdida.<br>`tools/monitor.py`: contadores `rx_nobuf=0`, `tx_nobuf=0`, `dma_err=0`. | Ambos extremos contrastados concurrentemente durante la misma sesión de tráfico. | `docs/eth_f1_t6_netcheck_709b869.txt` | **PASS** (3/3 checks host; contadores target en 0) |

---

### 5.4 Lista de pines de E-S1

Antes del primer flasheo de `prj_02_eth_lwip_dualcore`, se analizó la totalidad de los pines configurados por ambos núcleos (`CM7` y `CM4`) a partir del código fuente y `board_def.h`.

#### 5.4.1 Pines configurados por `prj_02`
- **Cortex-M7 (14 pines en total):**
  1. `PB0`: LED1 (Green) - Output Push-Pull, Low speed, No pull.
  2. `PE1`: LED2 (Yellow) - Output Push-Pull, Low speed, No pull.
  3. `PB14`: LED3 (Red) - Output Push-Pull, Low speed, No pull.
  4. `PD8`: Console USART3 TX - AF7 Push-Pull, Very High Speed, No pull.
  5. `PD9`: Console USART3 RX - AF7 Push-Pull, Very High Speed, No pull.
  6. `PA1`: `ETH_RMII_REF_CLK` - AF11 Push-Pull, Very High Speed, No pull.
  7. `PA2`: `ETH_MDIO` - AF11 Push-Pull, Very High Speed, No pull.
  8. `PA7`: `ETH_RMII_CRS_DV` - AF11 Push-Pull, Very High Speed, No pull.
  9. `PB13`: `ETH_RMII_TXD1` - AF11 Push-Pull, Very High Speed, No pull.
  10. `PC1`: `ETH_MDC` - AF11 Push-Pull, Very High Speed, No pull.
  11. `PC4`: `ETH_RMII_RXD0` - AF11 Push-Pull, Very High Speed, No pull.
  12. `PC5`: `ETH_RMII_RXD1` - AF11 Push-Pull, Very High Speed, No pull.
  13. `PG11`: `ETH_RMII_TX_EN` - AF11 Push-Pull, Very High Speed, No pull.
  14. `PG13`: `ETH_RMII_TXD0` - AF11 Push-Pull, Very High Speed, No pull.
- **Cortex-M4 (0 pines):**
  El núcleo CM4 sincroniza con CM7 por HSEM y ejecuta su tarea periódica sin configurar ningún periférico GPIO.

#### 5.4.2 Cotejo contra las líneas protegidas del banco
- **Líneas del Analog Discovery 2 (AD2):** `PE2`–`PE15` y `PA8`.
  - En puerto E: `prj_02` solo configura `PE1` (`PE1 != PE2..PE15`).
  - En puerto A: `prj_02` configura `PA1`, `PA2`, `PA7` (`PA1, PA2, PA7 != PA8`).
- **Jumper J1:** `PA5`, `PA6`, `PB4`, `PB5`.
  - En puerto A: `PA1`, `PA2`, `PA7` no coinciden con `PA5` ni `PA6`.
  - En puerto B: `PB0`, `PB13`, `PB14` no coinciden con `PB4` ni `PB5`.
- **Bus I2C:** `PB8`, `PB9`.
  - En puerto B: `PB0`, `PB13`, `PB14` no coinciden con `PB8` ni `PB9`.
- **Tomas de osciloscopio / taps:** `PF14`, `PF15`.
  - `prj_02` no utiliza ningún pin del puerto F.

**Conclusión E-S1:** Ninguno de los 14 pines coincide con ninguna línea del banco de pruebas. Se procedió al flasheo con total seguridad.

---

### 5.5 Desviaciones del plan

1. **Decisiones abiertas 1–5:** Se aplicaron rigurosamente las opciones por defecto establecidas en el plan de implementación:
   - Decisión 1: No se modificó `02_firmware_projects/prj_02_eth_lwip_dualcore/README.md` §6 en esta fase; queda reservado para el cierre si el responsable lo aprueba.
   - Decisión 2: No se portaron las limitaciones a `prj_eth_lwip_node` (E7 permanece como limitación explícita de proyecto).
   - Decisión 3: Los 5 contadores de `g_stats` se implementaron mediante `__atomic_fetch_add(&g_stats.<campo>, 1U, __ATOMIC_RELAXED)`.
   - Decisión 4: La MSP captura el primer fallo de `GPIO_HAL_Init` en la variable estática `s_msp_gpio_status`, y `ETH_Driver_Init()` retorna `ETH_DRIVER_ERROR` si dicho estado es distinto de `HAL_OK`. No se alteró ninguna firma.
   - Decisión 5: Los documentos de proceso se conservan hasta el cierre definitivo.
2. **Procedimiento de lectura SWD en T5:** De acuerdo con la directriz de las instrucciones §3.3 ("El halt por SWD no se mantiene entre llamadas... Mejor: lee los registros sin parar y dilo"), la inspección de registros GPIO en T5 se ejecutó con `STM32_Programmer_CLI -c port=SWD mode=HOTPLUG` sin detener el microcontrolador. Esto garantiza que los relojes de los periféricos en el dominio D3 permanezcan activos y que la aplicación continúe su ejecución normal durante la inspección.
3. **Confirmación de red previa a T6:** En cumplimiento de las instrucciones §3.6 y la regla E-S4, se solicitó al responsable (@Hector) la confirmación interactiva de la conexión del cable Ethernet y la configuración de la subred `192.168.1.0/24` antes de ejecutar la primera sesión de tráfico de red en T6.

---

### 5.6 Respuesta a la auditoría

Esta es la primera entrega formal de la fase F1; no existen hallazgos de auditoría previos que responder.

---

### 5.7 Erratas del plan encontradas

Ninguna errata identificada en `IMPLEMENTATION_PLAN.md` (revisión 1). Las especificaciones de diseño, nombres de archivos y criterios de oráculo y control resultaron exactos y reproducibles.

---

### 5.8 Problemas abiertos y no ejercitado

1. **Lo no ejercitado en hardware:** Coincide con lo declarado en `08_eth/README.md` §9.4 y fuera de alcance del encargo actual:
   - Modo forzado a 10 Mbps y erratum 2.25.10 (FEP).
   - Lectura por software de registros de erratas en caliente (`DMACRXIWTR`, `SYSCFG_PMCR.EPIS = 4`).
   - Desconexión y reconexión física de cable bajo flujo continuo de paquetes.
   - Tráfico de carga sostenida extrema (flood ping durante minutos) y asignación por DHCP.
   - Checksum acelerado por hardware (`ETH_USE_HARDWARE_CHECKSUM = 1`).
   - Hook de MSP personalizada (`ETH_HAL_CUSTOM_MSP` / `ETH_HAL_MspInit_User` sobreescrita).
2. **Alcance de la protección entre núcleos de E8:** El reemplazo de `HAL_GPIO_Init` por `GPIO_HAL_Init` elimina por completo el acceso sin sección crítica en `08_eth` hacia los registros de configuración de los puertos GPIO compartidos (G13). No obstante, tal como especifica el plan (§4, punto 4), la protección real multihilo/multinúcleo requiere que ambos núcleos tengan instalado el lock compartido (`GPIO_BM_SetSharedLock`) y que el otro núcleo también realice sus accesos a través de `gpio_bm`. La instalación de dicho lock a nivel de proyecto está fuera del alcance de este driver compartido.
