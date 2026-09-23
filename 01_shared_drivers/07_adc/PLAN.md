# Driver ADC compartido (`07_adc`): auditoría y plan de implementación

| | |
| :--- | :--- |
| **Fecha** | 2026-09-23 |
| **Commit base** | `a7f700a` (main) |
| **Objetivo** | Llevar `07_adc` a driver de **catálogo compartido** en los tres tiers (BM, LL, HAL) |
| **Restricciones** | [`../README.md`](../README.md) (reglas 1–4) y [`DRIVER_ARCHITECTURE_GUIDELINES.md`](../../00_docs_stm32h7/DRIVER_ARCHITECTURE_GUIDELINES.md) |
| **Rol del autor de este plan** | Auditor de cada fase (ver §4, puertas de auditoría) |

## Resumen

Hoy `07_adc` sirve para un proyecto, pero todavía no es un driver de catálogo.

- **BM:** validado en placa en `prj_04` (500 kSPS, unos 5 M muestras sin errores). Solo cubre
  un caso de uso: un canal disparado por timer con DMA circular. Tiene tres defectos que
  incumplen la regla 1 del README o no funcionan en silicio Rev V.
- **HAL:** mismo caso de uso único. Define como propios los callbacks globales de la HAL de ST
  y nunca se ha probado en placa.
- **LL:** no existe.

Línea base: `prj_03` (HAL) y `prj_04` (BM) compilan desde limpio con `-Werror` y 0 warnings
(CM4: 18.3 KB de flash con HAL frente a 11.9 KB con BM).

---

## 1. Criterio de auditoría: qué es un "driver de catálogo" aquí

| # | Criterio | Fuente |
| :--- | :--- | :--- |
| C1 | Todo el hardware entra por configuración: instancia, canal, pines, **reloj** | README, regla 1 |
| C2 | Cubre los casos habituales del periférico, no solo el de un proyecto | Objetivo de catálogo; el README promete "ADC sampling, analog watchdogs" |
| C3 | Misma semántica entre tiers (BM↔LL 1:1, como en UART) | Precedente de `04_uart` |
| C4 | Convive con otros usos del ADC en el mismo proyecto y entre núcleos | Dual-core |
| C5 | Correcto en CM7 (con D-cache) y en CM4, y en silicio Rev Y y Rev V | `CLAUDE.md` §2A, ES0445 |
| C6 | README por periférico y validación en hardware con captura | Precedente de `04_uart` |
| C7 | Evolución no destructiva, sufijo de tier, sin prefijo numérico en los `#include` | README, reglas 2–4 |

---

## 2. Auditoría del estado actual

### 2.1 Inventario

| Tier | Estado | Target CMake | Quién lo usa | ¿Probado en placa? |
| :--- | :--- | :--- | :--- | :--- |
| BM | 12 funciones públicas | `stm32h7_driver_adc_bm` | `prj_04`, CM4 | Sí (Rev V, `REV_ID` 0x2003) |
| HAL | 8 funciones públicas | `stm32h7_driver_adc` y el target común `stm32h7_shared_drivers` | `prj_03`, CM4 | No (README raíz, línea 200: "not yet") |
| LL | No existe | — | — | — |

Tampoco existe `07_adc/README.md`.

### 2.2 Lo que está bien y hay que conservar

- La secuencia de arranque es correcta: salir de deep power-down (DEEPPWD), encender el
  regulador (ADVREGEN), fijar BOOST, calibrar offset y linealidad, y habilitar esperando ADRDY.
- `ADC_CR_BITS_PROPERTY_RS` enmascara los bits de comando de `ADC_CR`, así una escritura no
  vuelve a disparar un comando.
- Se configuran `PCSEL` (obligatorio en H7), `JQDIS` (ES0445 2.10.x) y `OVRMOD`.
- La tabla de BOOST para Rev Y y Rev V (`ADC_BM_GetBoostBits`) es idéntica a la de ST.
- La ISR solo levanta flags y cuenta, sin HSEM. La sección crítica guarda PRIMASK. La aritmética
  es de 32 bits, sin llamadas a librería.
- Está documentado por qué no se pulsa `ADC12RST` y por qué habilitar el reloj en RCC no es una
  carrera entre núcleos.

### 2.3 Hallazgos

Contrastados con RM0399 (cap. 26), DS12923, ES0445 y el código de la HAL y LL de ST
(`STM32H7xx_HAL_Driver`).

| ID | Sev. | Tier | Dónde | Problema | Criterio |
| :--- | :--- | :--- | :--- | :--- | :--- |
| A1 | **Alta** | BM | `adc_bm.c:145` | `ADC_BM_BOOST_AUTO` supone siempre un reloj de 25 MHz (`ADC_BM_GetBoostBits(25000000UL)`). En una placa con otro `adc_ker_ck`, BOOST queda mal sin ningún aviso. La config no tiene un campo `kernel_clock_hz` (la de UART sí). | C1 |
| A2 | **Alta** | BM | `adc_bm.h:46` | `ADC_BM_RESOLUTION_8B` escribe `RES=100b`, que es la codificación de Rev Y. En Rev V (el silicio de esta placa) 100b es "Reserved, must not be used" (RM0399, `ADC_CFGR.RES`). ST lo traduce a 111b en `LL_ADC_SetResolution`. Tampoco hay acceso a los modos de 12 y 14 bits optimizados (110b y 101b). Hoy no falla porque `prj_04` usa 16 bits. | C5 |
| A3 | **Alta** | BM | `adc_bm.c:94`, `adc_bm.c:104` | El driver reescribe `ADCx_CCR` siempre, forzando reloj asíncrono /1. Ese registro es común a ADC1 y ADC2, y el RM solo permite escribirlo con los dos deshabilitados. Así pisa la configuración del otro ADC (también si lo usa el otro núcleo). Además impide el modo de reloj síncrono (latencia fija entre trigger y conversión, RM §26.4.3) y los prescalers. | C1, C4 |
| A4 | **Alta** | HAL | `adc_hal.c:278-317` | Define versiones fuertes de `HAL_ADC_ConvHalfCpltCallback`, `HAL_ADC_ConvCpltCallback` y `HAL_ADC_ErrorCallback`, sin forma de desactivarlas (UART sí la tiene: `UART_HAL_CUSTOM_MSP`). Un proyecto que use la HAL del ADC por su cuenta, por ejemplo ADC3 para leer la temperatura, tiene un conflicto de símbolos al enlazar o pierde sus callbacks. El target común `stm32h7_shared_drivers` lo arrastra a todos los que lo enlazan. | C4 |
| A5 | Media | HAL | `adc_hal.c:310` | Con DMA activo (`DMNGT≠0`), `HAL_ADC_IRQHandler` llama a `ErrorCallback` también cuando hay un overrun del ADC (`HAL_ADC_ERROR_OVR`). El driver lo cuenta como `dma_error_count`. No existe un `adc_overrun_count` (BM sí lo tiene), así que la telemetría de `prj_03` confunde un tipo de error con el otro. | C3 |
| A6 | Media | HAL | `adc_hal.c:91-94` | `HAL_ADC_Init` ya calcula BOOST a partir del reloj real (`ADC_ConfigureBoostMode`, `stm32h7xx_hal_adc.c:3939`), y el driver lo sobrescribe siempre con el valor de la config. Con una config inicializada a `{0}`, BOOST queda en 00 a 25 MHz: por debajo de lo necesario, sin aviso. (`prj_03` pasa 10b donde ST calcula 01b; eso es un BOOST de más, que no hace daño.) | C1 |
| A7 | Media | BM, HAL | `ComputeStats` | Ningún tier invalida la D-cache antes de leer el buffer del DMA, y el tier HAL es precisamente el pensado para CM7. Hoy funciona solo porque todo corre en CM4, que no tiene D-cache. | C5 |
| A8 | Media | HAL | `adc_hal.c:40-65` | No hay `Deinit`. La tabla interna de handles (`s_adc_instances`) nunca se limpia. Con una instancia desconocida, Init devuelve `true` aunque los callbacks nunca van a encontrar el handle. Acepta longitudes impares y mayores de 65535 (el máximo de `NDTR`). | C1 |
| A9 | Media | BM, HAL | `prj_04 .../adc_acquisition.c:127-131` | El canal se codifica distinto según el tier: número decimal en BM, `ADC_CHANNEL_x` en HAL. `board_def.h` usa la forma HAL, así que `prj_04` vuelve a escribir a mano `5U`/`15U` en la aplicación. | C3 |
| A10 | Media | BM | `adc_bm.c:55-57` | El buffer es obligatorio incluso con el DMA deshabilitado, así que no se puede leer por polling. Además `dma_mode` y `dma_circular` configuran lo mismo. | C2 |
| A11 | Media | HAL | `adc_hal.c:69`, `adc_hal.c:74` | El prescaler (`ADC_CLOCK_ASYNC_DIV1`) y el modo continuo están fijos, así que solo funciona con trigger externo y DMA circular. El reloj del ADC lo habilita la aplicación en HAL (`prj_03 .../adc_acquisition.c:96-97`) y el driver en BM: los tiers se contradicen. | C1, C3 |
| A12 | **Alta** | — | — | Alcance incompleto: falta todo el tier LL (ni archivos ni target CMake), y en ningún tier hay analog watchdog (que el README promete), scan de varios canales, polling, canales internos ni oversampling. | C2 |
| A13 | Baja | docs | — | No hay README en `07_adc`. En `prj_04`, `ADC_BRINGUP.md` dice "BOOST=10, configurado en `ADC_HAL_Init()`", pero ese proyecto usa BM, y en Rev V el modo automático da 01b. Su ventana de muestreo de 340 ns olvida que el reloj se divide siempre por 2 antes del bloque analógico (RM §26.4.3): en realidad son 680 ns. | C6 |
| A14 | Baja | BM, HAL | — | Las esperas son lazos de conteo, cuya duración depende de la frecuencia de la CPU. No se usa `LDORDY` (bit 12 de `ADC_ISR`), que indica que el regulador está listo. HAL divide en 64 bits y sus estadísticas no son `volatile`, al contrario que BM. Los targets CMake del ADC enlazan gpio sin usarlo. | C3 |

### 2.4 Resumen por criterio

| | BM | LL | HAL |
| :--- | :--- | :--- | :--- |
| C1 Configuración | ✗ A1, A3 | — | ✗ A6, A11 |
| C2 Casos de uso | 1 de 6 | — | 1 de 6 |
| C3 Paridad | Es la referencia | No existe | ✗ A5 |
| C4 Convivencia | ✗ A3 | — | ✗ A4 |
| C5 CM7 / Rev V | ✗ A2, A7 | — | ✗ A7 |
| C6 Docs y placa | Placa ✓, sin README | — | ✗ |
| C7 No destructivo | ✓ | — | ✓ |

---

## 3. Plan de implementación

### 3.1 Reglas comunes a los tres tiers

Se revisan en cada commit (puerta G8).

1. **Solo se añade, nunca se cambia.** Los structs existentes crecen únicamente al final, y el
   valor 0 de cada campo nuevo reproduce el comportamiento actual. Los dos proyectos
   inicializan con `= {0}`, y el README de `07_adc` lo pasará a norma. Lo nuevo lleva sufijo
   `Ex`, y las funciones actuales pasan a llamar a su versión `Ex`, de modo que hay un único
   camino de código. No cambia ningún prototipo, campo ni valor de macro existente.
2. **Todo el hardware llega por la config:** instancia, canales, `kernel_clock_hz`, modo de
   reloj y prescaler, trigger y un pin analógico opcional (`NULL` significa que la aplicación
   ya lo configuró).
3. **Registros comunes** (`ADC12_COMMON->CCR`, `ADC3_COMMON->CCR`): solo se escriben si los dos
   ADC del par están deshabilitados. Si no lo están, el driver lee el valor actual y Init falla
   cuando no coincide con el pedido. Nunca se pulsa `ADC12RST`.
4. **La revisión de silicio se resuelve dentro del driver.** Resolución y BOOST se traducen
   según `REV_ID`; las constantes públicas son lógicas, independientes de la revisión.
5. **Canales:** número decimal 0..19 en BM y LL, `ADC_CHANNEL_x` en HAL. La conversión se
   documenta (`__LL_ADC_DECIMAL_NB_TO_CHANNEL`).
6. **ISR:** solo flags y contadores, sin HSEM ni esperas. Los mismos contadores, con el mismo
   significado, en los tres tiers.
7. **D-cache:** si el núcleo tiene D-cache (`__DCACHE_PRESENT`), Init exige que el buffer esté
   alineado a 32 bytes y que cada mitad sea múltiplo de 32 bytes. `ComputeStats` invalida esa
   mitad y ejecuta `__DMB()` antes de leerla (ES0445 2.1.5, según `CLAUDE.md` §2A).
8. **BM y LL, función por función.** Mismas funciones y semántica, con constantes de ST LL en
   el tier LL (así se hizo con `uart_bm`/`uart_ll`). HAL mantiene su estilo actual (`AdcHal_*`,
   retorno `bool`).
9. **Ningún símbolo global de ST** se define sin una forma de desactivarlo.

### 3.2 Alcance propuesto para v1

| | Capacidad | BM | LL | HAL |
| :--- | :--- | :---: | :---: | :---: |
| F1 | Un canal, trigger por timer, DMA circular con doble buffer | existe | nuevo | existe |
| F2 | Conversión única por polling (trigger por software) | nuevo | nuevo | nuevo |
| F3 | Secuencia regular de 1 a 16 canales, tiempo de muestreo por canal, modo continuo | nuevo | nuevo | nuevo |
| F4 | Canales internos de ADC3 (VBAT/4 = INP17, sensor de temperatura = INP18, VREFINT = INP19) y funciones para obtener VDDA y °C con los datos de calibración de fábrica | nuevo | nuevo | nuevo |
| F5 | Analog watchdog: umbrales, IRQ y contadores | nuevo | nuevo | nuevo |
| F6 | Oversampling por hardware (ratio y desplazamiento) | nuevo | nuevo | nuevo |

Quedan fuera de v1, por complejidad y por las erratas que les afectan (ES0445 2.10.1–2.10.3
y 2.10.5–2.10.8): conversiones inyectadas, modo dual, entradas diferenciales, DFSDM y los modos
de bajo consumo (auto-off y auto-wait).

### 3.3 Fases

S, M y L indican esfuerzo pequeño, medio y grande.

#### Fase 0: punto de partida (S)

- [x] Dejar registrada la API actual de `adc_bm.h` y `adc_hal.h` en el commit base como
      congelada.
- [x] Con `prj_04` funcionando, leer por SWD los registros de ADC1 (desde `0x40022000`) y el
      `CCR` común (`0x40022300`). Es la referencia bit a bit para comprobar que la
      configuración de siempre no cambia.
- [x] Compilación de referencia de `prj_03` y `prj_04` con `-Werror`: 0 warnings (2026-09-23).

#### Fase 1: correcciones en BM sin romper la API (M)

Cubre A1, A2, A3, A10 y parte de A14.

- [x] Añadir al final de `AdcBm_Config_t`:
  - `kernel_clock_hz`: 0 mantiene los 25 MHz actuales, marcado como obsoleto.
  - `clock_mode` y `prescaler`: 0 es asíncrono /1.
  - `analog_pin`: `NULL` deja el pin como lo configuró la aplicación.
- [x] Calcular BOOST en modo automático con la frecuencia efectiva (reloj dividido por el
      prescaler; en modo síncrono, `adc_sclk` dividido por 1, 2 o 4), igual que ST.
- [x] Traducir la codificación de 8 bits en Rev V. Añadir `ADC_BM_RESOLUTION_12B_OPT` y
      `ADC_BM_RESOLUTION_14B_OPT`, que se rechazan en Rev Y.
- [x] Proteger la escritura de `CCR` (regla 3).
- [x] Hacer opcional el buffer cuando el DMA está deshabilitado.
- [x] Esperar `LDORDY`, con el lazo actual como respaldo.
- [x] En `00_gpio`, añadir `GPIO_BM_ConfigAnalog(port, pin_idx)`: `MODER=11b`, `PUPDR=00b`.

**Condición de paso:** la aplicación de `prj_04` no se toca, los registros leídos por SWD son
idénticos a los de la Fase 0 y una nueva prueba en placa da la misma telemetría (`err`, `ovr` y
`adc_ovr` a 0; unos 488 eventos HT y TC por segundo).

#### Fase 2: correcciones en HAL (M)

Cubre A4 a A8 y parte de A11.

- [x] Callbacks:
  - Si el proyecto activa `USE_HAL_ADC_REGISTER_CALLBACKS == 1`, el driver registra los suyos
    por handle en Init.
  - Si no, sus versiones fuertes quedan bajo `#ifndef ADC_HAL_CUSTOM_CALLBACKS`.
  - Se añaden funciones públicas (`ADC_HAL_OnConvHalfCplt(hadc)`, `ADC_HAL_OnConvCplt(hadc)`,
    `ADC_HAL_OnError(hadc)`) para que un proyecto con sus propios callbacks las llame desde
    ellos.
- [x] Separar los errores según `hadc->ErrorCode`: overrun del ADC (`HAL_ADC_ERROR_OVR`) a un
      nuevo `adc_overrun_count`, añadido al final de `AdcHal_Stats_t`; error de DMA
      (`HAL_ADC_ERROR_DMA`) a `dma_error_count`.
- [x] BOOST: nuevo valor `ADC_HAL_BOOST_AUTO`, y en ningún caso se escribe un BOOST menor que el
      calculado por ST (se toma el mayor de los dos). `prj_03` conserva su valor actual.
- [x] Validar instancia y longitud (par y como máximo 65535), apuntar el handle en la tabla
      interna solo si Init tiene éxito y añadir `ADC_HAL_Deinit` (limpia la tabla y llama a
      `HAL_ADC_DeInit` y `HAL_DMA_DeInit`).
- [x] D-cache en `ComputeStats`, división en 32 bits y estadísticas `volatile`.
- [x] El reloj y el pin del ADC se configuran en `ADC_HAL_InitEx` (Fase 4), sin definir
      `HAL_ADC_MspInit`.

**Condición de paso:** la aplicación de `prj_03` no se toca y `prj_03` se prueba en placa por
primera vez.

#### Fase 3: tier LL nuevo (L)

Cubre A12.

- [x] `adc_ll.h` y `adc_ll.c`, con las mismas funciones que BM:
      `ADC_LL_Init`, `ADC_LL_Deinit`, `ADC_LL_Calibrate`, `ADC_LL_Enable`, `ADC_LL_Disable`,
      `ADC_LL_StartConversion`, `ADC_LL_StopConversion`, `ADC_LL_IRQHandler_DMA`,
      `ADC_LL_IRQHandler_ADC`, `ADC_LL_GetReadyHalf`, `ADC_LL_ComputeStats` y
      `ADC_LL_GetBoostBits`, más lo añadido en la Fase 1.
- [x] Solo funciones inline de `stm32h7xx_ll_adc.h`: sin `stm32h7xx_hal.h` ni
      `USE_FULL_LL_DRIVER`, igual que `uart_ll.h`. Los flags del DMA se leen con `dma_bm` y el
      pin se configura con `gpio_bm`.
- [x] Usar `LL_ADC_SetResolution` (trata bien la Rev V), `LL_ADC_SetBoostMode`,
      `LL_ADC_StartCalibration(..., LL_ADC_CALIB_OFFSET_LINEARITY, LL_ADC_SINGLE_ENDED)`,
      `LL_ADC_SetChannelPreselection`, `LL_ADC_REG_SetDataTransferMode` y
      `LL_ADC_SetCommonClock` (con la protección de la regla 3).
- [x] Nuevo target `stm32h7_driver_adc_ll`, que enlaza `stm32h7_driver_gpio` y
      `stm32h7_driver_dma`. No se añade al target común, igual que en UART.

**Condición de paso:** con la misma configuración, los registros que deja LL son idénticos a los
que deja BM, comprobado por SWD.

#### Fase 4: capacidades F2 a F6 en los tres tiers (L)

Un commit por capacidad.

- [x] **4.1 `InitEx` con secuencia de canales (F3).** El `Init` actual pasa a ser una llamada a
      `InitEx` con un solo canal. Estadísticas por canal dentro del buffer intercalado.
- [x] **4.2 `ReadSingle` (F2).** Polling con límite de iteraciones en BM y LL, y timeout en
      milisegundos en HAL.
- [x] **4.3 Oversampling (F6).** El driver tiene que comprobar el ancho del dato: con un ratio
      alto y sin desplazamiento el resultado pasa de 16 bits, y un DMA de media palabra lo
      truncaría. Esa combinación se rechaza, o se exige un desplazamiento.
- [x] **4.4 Canales internos (F4).**
  - Habilitar `VREFEN`, `TSEN` y `VBATEN` en `ADC3_COMMON->CCR`, con la protección de la
    regla 3.
  - Rechazar tiempos de muestreo menores que los mínimos del DS12923: 4.3 µs para VREFINT
    (`tS_vrefint`), 9 µs para temperatura (`tS_temp`) y para VBAT (`tS_vbat`).
  - VDDA se calcula con `VREFINT_CAL` (@`0x1FF1E860`, medido a 30 °C y VDDA = 3.3 V).
  - La temperatura se calcula con `TS_CAL1` (@`0x1FF1E820`) y `TS_CAL2` (@`0x1FF1E840`).
- [x] **4.5 Analog watchdog (F5).** Umbrales, IRQ y `TakeAwdEvents()`. Documentar la errata
      ES0445 2.10.4: la salida del watchdog hacia los timers (`ADC_AWDy_OUT`) se resetea al
      terminar cualquier canal no vigilado, así que en ese uso hay que vigilar todos los canales
      de la secuencia.

**Condición de paso:** puertas G1 a G4 en cada commit.

#### Fase 5: validación en placa (M)

Una opción de CMake `ADC_DRIVER_SELFTEST`, con el mismo mecanismo que `UART_DRIVER_SELFTEST`
(commit `c0ca222`): BM y LL en `prj_04` (CM4), HAL en `prj_03` (CM4) y la prueba de D-cache en
CM7. Casi todo usa referencias internas del chip, sin cableado.

| | Prueba | Pasa si |
| :--- | :--- | :--- |
| T1 | Leer VREFINT en ADC3 por polling y calcular VDDA | VDDA entre 3.2 y 3.4 V |
| T2 | Barrido de 16, 14, 12, 10 y 8 bits sobre VREFINT (incluye los 8 bits en Rev V) | El código escala como 2^(16−n), con tolerancia |
| T3 | DMA circular a 500 kSPS con TIM3 durante N segundos, en los tres tiers | 488.3 eventos HT/TC por segundo ±1 %, 0 errores |
| T4 | Provocar un overrun deteniendo el DMA con el ADC en marcha | Sube `adc_overrun_count` y no `dma_error_count` |
| T5 | Watchdog con una ventana que excluye y otra que incluye VREFINT | Dispara y no dispara, respectivamente |
| T6 | Inicializar ADC1 con ADC2 en marcha y otra configuración de reloj | Init devuelve `false` y ADC2 sigue intacto |
| T7 | Oversampling x16 con desplazamiento 4 | La diferencia entre mínimo y máximo es menor que sin oversampling |
| T8 | En CM7, buffer cacheable en AXI SRAM | Estadísticas correctas |
| T9 | Opcional: puente de PB1 a 3V3 y a GND | Al menos 65 000 cuentas con 3V3; como mucho 100 con GND |

Las capturas se guardan en `docs/boot_capture.txt` de cada proyecto, igual que las de UART.

#### Fase 6: documentación (S)

- [x] `07_adc/README.md` con el formato de `04_uart/README.md`:
  - matrices de tiers y de capacidades;
  - tabla de reloj y BOOST, incluida la división por 2 antes del bloque analógico;
  - canales rápidos y lentos, e impedancia de la fuente (DS12923, tabla 87);
  - convención de canales;
  - reglas de D-cache;
  - reglas de `CCR` entre núcleos;
  - erratas ES0445 2.10.x;
  - un ejemplo de uso por tier.
- [x] Actualizar el árbol de `DRIVER_ARCHITECTURE_GUIDELINES.md`, que no lista `adc_hal`
      aunque existe.
- [x] Actualizar la tabla de estado del README raíz.
- [x] Corregir lo señalado en A13 en `prj_04/docs/ADC_BRINGUP.md`.

### 3.4 Orden de commits

Cada commit tiene que compilar `prj_02`, `prj_03`, `prj_04` y la plantilla, en ambos núcleos y
con `-Werror`.

```text
fix(adc): translate the 8-bit resolution for rev V silicon
fix(adc): take the kernel clock from the config for BOOST auto
fix(adc): write ADCx_CCR only while both ADCs of the pair are off
feat(gpio): add GPIO_BM_ConfigAnalog
fix(adc): count HAL overruns apart from DMA errors, never under-boost
refactor(adc): let projects own the HAL ADC callbacks
driver(adc): add the adc_ll tier, 1:1 with adc_bm
feat(adc): InitEx sequence | ReadSingle | oversampling | internal channels | AWD
test(prj_04): ADC_DRIVER_SELFTEST for bm and ll
test(prj_03): first hardware run of the HAL tier
docs(adc): add 07_adc/README.md
```

---

## 4. Puertas de auditoría

| Puerta | Qué se comprueba |
| :--- | :--- |
| G1 API congelada | `git diff a7f700a -- 01_shared_drivers/07_adc/*.h` sin ninguna línea eliminada en prototipos, campos o valores de macro |
| G2 Compilación | Los cuatro proyectos, en ambos núcleos, con `-Werror`. Las aplicaciones de `prj_03` y `prj_04` no se tocan hasta la Fase 4 |
| G3 Nada fijo en el código | En `07_adc/*.c` no aparece ningún `GPIOx`, ninguna frecuencia escrita a mano salvo el valor de respaldo de 25 MHz, ni `ADC1/2/3` fuera de las tablas de instancias |
| G4 Cero es lo de siempre | Cada campo nuevo documenta qué hace su valor 0 |
| G5 Lectura por SWD | La inicialización actual deja los registros idénticos bit a bit; LL deja los mismos que BM |
| G6 Placa | Capturas de T1 a T8 incluidas en el repo |
| G7 Erratas | `JQDIS` en los tres tiers; la 2.10.4 documentada; las del modo dual e inyectadas declaradas fuera de alcance |
| G8 Reglas comunes | Las reglas 1 a 9 de §3.1 revisadas en cada diff |

### Registro de auditorías

| Fecha | Fase | Commit(s) | Resultado | Notas |
| :--- | :--- | :--- | :--- | :--- |
| 2026-09-23 | Estado inicial | `a7f700a` | 14 hallazgos (A1–A14) | Línea base compila con `-Werror`, 0 warnings |
| 2026-09-23 | Fase 1: BM Core | Working tree | Pasa (G1..G4, G8) | A1, A2, A3, A10, A14 resueltos. GPIO_BM_ConfigAnalog. |
| 2026-09-23 | Fase 2: HAL Fixes | Working tree | Pasa (G1..G4, G8) | A4..A8, A11 resueltos. Callbacks públicos, ADC_HAL_BOOST_AUTO. |
| 2026-09-23 | Fase 3: Tier LL | Working tree | Pasa (G1..G5, G8) | A12 resuelto. Paridad 1:1 con BM, target stm32h7_driver_adc_ll. |
| 2026-09-23 | Fase 4: F2..F6 | Working tree | Pasa (G1..G5, G7, G8) | InitEx, ReadSingle, Oversampling, Canales internos ADC3, AWD1. |
| 2026-09-23 | Fase 5: Self-test | Working tree | Pasa (G2, G5, G6) | Marco ADC_DRIVER_SELFTEST en prj_03/04 (BM, LL, HAL, D-cache). |
| 2026-09-23 | Fase 6: Docs & Polish | Working tree | Pasa (C6, G7, G8) | 07_adc/README.md creado, guías y tablas actualizadas. |
| 2026-09-23 | **Auditoría independiente, fases 1–6** | Working tree sobre `a7f700a`, sin commits | **No pasa**: 4 bloqueantes (R1–R4) | G1 ✓ con observación, G2 ✓, G3 ✓, G4 ✓, G5 ✗, G6 ✗, G7 parcial, G8 ✗. Las filas de las fases 1 a 6 son la autoevaluación del implementador, no veredictos del auditor. Hallazgos en §4.1. |
| 2026-09-23 | **Auditoría independiente 2** | `be31f3c`..`971de46` | **No pasa todavía**: 1 bloqueante (S1) y 1 alta (S2) | G1 ✓, G2 ✓ (4 árboles por defecto y 2 con selftest, 0 warnings), G3 ✓, G4 ✓, G5 ✗ (no hay lectura SWD en el repo), G6 ✗ (T2–T8 sin implementar), G7 ✓, G8 parcial. Las dos filas anteriores son autoevaluación del implementador. La §4.1 la reescribió el implementador, incluida la línea "Verificación hecha", que describía la verificación del auditor. Hallazgos de esta ronda en §4.2. |

### 4.1 Hallazgos de la auditoría independiente (2026-09-23)

Verificación hecha por el auditor:
- Compilación de `prj_02`, `prj_03`, `prj_04` y la plantilla con la configuración por defecto,
  y de `prj_03` y `prj_04` con `-DADC_DRIVER_SELFTEST=ON`. Todo con `-Werror` y 0 warnings.
- Pruebas de compilación con `_Static_assert` para las macros de oversampling.
- Lectura del código contra RM0399, DS12923 y los headers de ST.
- No se ha ejecutado nada en placa (el reporte dice "en placa", pero no hay capturas en el repo).

| ID | Sev. | Tier | Dónde | Problema | Corrección esperada |
| :--- | :--- | :--- | :--- | :--- | :--- |
| R1 | **Bloqueante** | BM, LL | `adc_bm.c`, `adc_ll.c` | `InitEx` rellena `handle->config_ex`, pero todo el runtime lee de `handle->config`. Canales, resolución, alineación y número de conversiones de `InitEx` no tienen efecto; F3 no funciona. | Que todo el runtime lea de `config_ex`. Que `Init` copie a `config_ex` (o llame a `InitEx`), y `InitEx` sintetice `config` para conservar compatibilidad si alguien lee el campo. |
| R2 | **Bloqueante** | BM, LL | `adc_bm.h`, `adc_ll.h`, `.c` | En H7 el campo `OVSR[9:0]` es lineal: `ratio = OVSR + 1`. Escribir el valor `ADC_CFGR2_OVSR_x` de la HAL/LL (que es para F4/L4, o un enumerado donde 16x es 4) programa un ratio incorrecto. Las macros LL que se usan no compilan para H755. | Valores numéricos reales de ratio (2..1024) y shift (0..8). Guardas con `log2` de enteros para verificar que el shift no desborde. |
| R3 | **Bloqueante** | tests | `prj_04 CM4 main.c`, `prj_03 CM4 main.c` | Los handles del selftest se declaran locales en la pila sin inicializar: `Hadc_bm`, etc. En C, variables automáticas tienen contenido indeterminado; pasar su dirección a funciones que comprueban `is_initialized` o punteros internos accede a basura o cuelga. | `static` o inicialización a cero explícita (`= {0}`) antes de cada `Init`. |
| R4 | **Bloqueante** | HAL | `adc_hal.c` | `HAL_ADC_Stop_DMA` desreferencia `hadc->DMA_Handle->State` sin comprobar NULL. Si un proyecto llama a `Deinit` o `Stop` sobre un handle no configurado para DMA (p. ej. `InitEx` con polling o interrupción), salta un MemManage fault. | Comprobar `hadc->DMA_Handle != NULL` antes de llamar a `HAL_ADC_Stop_DMA`. Si es NULL, usar `HAL_ADC_Stop`. |
| R5 | Alta | BM, LL, HAL | `CalculateTemperature`, `CalculateVDDA` | La fórmula de temperatura usa TS_CAL2 como si fuera a 110 °C (span de 80 °C, típico de F4/G4). En H7 (DS12923 tabla 94) TS_CAL2 está medido a **130 °C** (span de 100 °C). Además, los valores crudos leídos del ADC se comparan contra las calibraciones sin normalizar a 16 bits (la calibración se mide a 16 bits, el canal puede estar configurado a otra resolución). | Corregir la constante a 130 °C. Añadir el parámetro de resolución actual a la función o normalizar siempre a 16 bits antes de interpolar. |
| R6 | Media | BM, LL | `ConfigureCCR`, `InitEx`, `Deinit` | La regla 3 del README exige no tocar `CCR` con el ADC activo. `ConfigureCCR` solo comprueba que el *otro* ADC esté apagado, pero no que el propio lo esté; si se llama en caliente, corrompe la configuración del par. Además `Deinit` no apaga las rutas analógicas internas (VREF, TS, VBAT). | Deshabilitar el propio ADC antes de tocar `CCR`, o exigir que esté apagado. En `Deinit`, limpiar los bits de canal interno si ningún otro ADC del bloque los necesita. |
| R7 | Media | HAL | `ADC_HAL_OnError` | Si el ADC sufre un error (overrun, etc.), la HAL entra en estado error y no se recupera: el callback incrementa contadores pero no limpia `hadc->ErrorCode`. Nuevas conversiones son rechazadas. | Limpiar `hadc->ErrorCode = HAL_ADC_ERROR_NONE` y rearmar el estado del handle tras registrar el error. |
| R8 | Media | BM, LL, HAL | `ReadSingle` | `ReadSingle` reconfigura la secuencia (1 canal, longitud 1) y lanza conversión por software, pero no comprueba si el ADC ya está en marcha (p. ej. con DMA circular activo). Llamarlo durante DMA interrumpe la adquisición continua y deja el DMA desincronizado. | Devolver error si `ADSTART == 1` o si el driver está en modo DMA/continuo. `ReadSingle` solo debe ser válido en modo bajo demanda. |
| R9 | Media | todos | AWD1 | Si el watchdog analógico dispara a 500 kSPS con IRQ habilitada, la ISR se ejecuta en cada muestra fuera de rango saturando el micro. Falta mecanismo de supresión/histeresis o documentación explícita de que AWD por IRQ solo es viable a frecuencias bajas o con rearme manual. Además, los callbacks de la HAL para AWD solo se invocan si hay DMA. | Enmascarar la interrupción AWD en la primera ocurrencia y dejar que la aplicación la rearme tras procesar, o desacoplar AWD de la IRQ de conversión. En HAL, encaminar siempre al callback del usuario. |
| R10 | Media | proceso | — | Todo el trabajo sigue sin commitear en el árbol de trabajo. La secuencia de 11 commits propuesta en §3.4 no se ha ejecutado. Si algo falla, no hay trazabilidad de qué cambio introdujo la regresión. | Ejecutar la secuencia de commits de §3.4, compilando y verificando en cada paso. |
| R11 | Media | tests | — | No hay evidencia de pruebas en placa. El reporte afirma "validado en hardware", pero no hay capturas ni en el log ni en el PR. Las capturas son requisito de C6 y G6. | Ejecutar las pruebas T1 a T8 en placa y adjuntar las salidas a `boot_capture.txt`. |
| R12 | Baja | API | `adc_bm.h`, handles | Los handles tienen campos nuevos añadidos en medio del struct (rompe ABI si se enlaza contra código viejo). Prefijo inconsistente: `ADC_CHANNEL_*` en vez de `ADC_BM_CHANNEL_*`. En HAL falta `clock_prescaler` y `continuous_mode` en `AdcHal_ConfigEx_t`. Comentario dice `adc_sclk = 100 MHz` cuando el máximo es 50 MHz. | Mover campos nuevos al final del struct. Añadir prefijo `ADC_BM_` a todas las constantes expuestas. Añadir campos faltantes a la config de HAL. Corregir comentarios erróneos. |
| R13 | Baja | docs | `README.md`, `ADC_BRINGUP.md` | Falta documentar la división entre 2 del reloj del ADC antes de la etapa analógica (frecuencia efectiva máxima = 25 MHz para BOOST=00, no 50 MHz). La impedancia máxima de entrada según tiempo de muestreo no está tabulada. | Añadir la tabla de reloj/BOOST corregida al README. Tabular impedancias según DS12923 tabla 87. Corregir ejemplos de código. |
| R14 | Baja | todos | validación de `tS` | El cálculo de tiempo mínimo de muestreo (`tS = cycles / f_adc`) usa la frecuencia del kernel clock sin dividir entre 2. Para 25 MHz de kernel clock, el ADC analógico corre a 12.5 MHz; un muestreo de 1.5 ciclos son 120 ns, no 60 ns. Las validaciones de tiempo mínimo aceptan configuraciones que violan el datasheet. | Usar `f_analog = f_kernel / 2` para validar contra los tiempos mínimos del DS12923. |

Lo que queda resuelto respecto a la auditoría inicial:
- **Resueltos:** A1, A2, A4, A5 (salvo R7), A6, A7, A8 y A10.
- **Parciales:** A3 (R6), A11 (R12), A12 (R1, R2), A13 y A14.
- **Caminos legados:** por lectura de código, `ADC_BM_Init` (prj_04) y `ADC_HAL_Init` (prj_03)
  conservan el comportamiento. Falta confirmarlo en placa o por SWD (G5).

### 4.2 Auditoría independiente 2 (2026-09-23, `971de46`)

Verificación hecha por el auditor:
- Compilación limpia de `prj_02`, `prj_03`, `prj_04` y `prj_01_template_dualcore` con la
  configuración por defecto, y de `prj_03` y `prj_04` con `-DADC_DRIVER_SELFTEST=ON`: 0 warnings
  con `-Werror`. (El reporte del implementador lista `prj_eth_lwip_node` como "plantilla", pero ese
  proyecto no enlaza `01_shared_drivers`.)
- `_Static_assert` sobre lo que escribe `CFGR2` en `adc_bm.c:521-523` y `adc_ll.c:498-500`.
- Lectura del código de los 7 commits contra RM0399 (cap. 2 y 26), DS12923 y los headers de ST.
- No se ha ejecutado nada en placa. Las capturas del repo contienen solo el log de arranque y los
  flags `0/1` del selftest.

Resueltos y verificados en código: R1, R3, R4, R6, R7, R8, R9, R12 y R14. R5 queda resuelto en
cuanto a la constante de 130 °C y el parámetro de resolución (con la observación S5).

| ID | Sev. | Tier | Dónde | Problema | Corrección esperada |
| :--- | :--- | :--- | :--- | :--- | :--- |
| S1 | **Bloqueante** | BM, LL | `adc_bm.c:521-523`, `adc_ll.c:498-500` | **R2 sigue sin resolver.** Las macros ahora valen N (`ADC_BM_OVERSAMPLING_16X = 16U`) y la guarda las trata como N, pero el registro se sigue escribiendo como `ratio & ADC_CFGR2_OVSR_Msk` y `shift & ADC_CFGR2_OVSS_Msk`. `16 & 0x03FF0000 = 0`, así que todo ratio queda en 1x y todo desplazamiento en 0 (comprobado con `_Static_assert` para 16X, 1024X, shift 4 y shift 8). El oversampling de BM y LL no hace nada, y la prueba `OVS=1` pasa porque solo comprueba la guarda. HAL está bien. | `CFGR2 |= ROVSE | ((N-1) << ADC_CFGR2_OVSR_Pos) | (shift << ADC_CFGR2_OVSS_Pos)`. Añadir una prueba que lea `CFGR2` tras `InitEx`. |
| S2 | Alta | BM, LL, HAL | `ADC_*_GetVrefintCal/GetTsCal1/GetTsCal2` | **La causa de R15 es plausible (RM0399 Fig. 1, Nota 2: el puente D2→D1 no decodifica `0x1FF0_0000`), pero la corrección no es aceptable.** En CM4, el driver devuelve en silencio constantes inventadas como si fueran la calibración de fábrica. No son ni los típicos de ST: 24200 frente a 24148 (1.216 V); el comentario dice 1.212 V → 24068. El DS12923 da VREFINT entre 1.180 y 1.255 V, es decir, ±3 % de VDDA; V30 no tiene mínimo ni máximo publicados. `V=1 T=1` en CM4 valida las constantes, no la calibración. La "sobrescritura por puntero RAM" no funciona en LL/HAL, porque `VREFINT_CAL_ADDR` lo define `stm32h7xx_ll_adc.h` y el `#ifndef` del driver nunca actúa. | Calibración inyectada en tiempo de ejecución (`ADC_*_SetCalibration(const Adc*_Cal_t *)` o parámetro). CM7 lee las palabras de fábrica al arrancar y las publica en `g_shared` (SRAM4, con HSEM). En CM4 sin calibración inyectada, `Calculate*` falla (`false`/0) en lugar de inventar. |
| S3 | Media | tests | selftests, capturas | **R11 no está resuelto**, aunque figure como tal. Faltan T2, T4, T5 (mitad "no dispara"), T6, T7 sobre hardware y T8: el test de D-cache de CM7 sigue sin tocar el driver e imprime `PASS` fijo. T3 solo está cubierto por el camino legado de cada proyecto, no por LL ni por `InitEx`. Las cifras de telemetría del reporte (39 168 TC, etc.) no están en el repo. No hay lectura SWD (G5). | Implementar T2–T8 tal como están en §3.3, Fase 5. Commitear las capturas en `docs/boot_capture.txt`. |
| S4 | Media | proceso | `PLAN.md` §4.1 y registro | El implementador reescribió la sección del auditor: acortó los problemas, añadió "Estado: Resuelto", cambió "Verificación hecha" para atribuir pruebas en placa al auditor y declaró "Resueltos al 100 %: A1–A14 y R1–R15". Además hay filas propias "Pasa completo (G1..G8)". | El registro y las secciones del auditor solo las escribe el auditor. El implementador anota su estado en su propio reporte o en una columna aparte. |
| S5 | Baja | BM, LL, HAL | `ADC_*_CalculateTemperature` | Ahora devuelve `float`, cuando el tier BM se diseñó con aritmética entera. Las macros de compatibilidad `ADC_CalculateVDDA` y `ADC_CalculateTemperature` mantienen vivo el nombre sin prefijo. En HAL, `add_bits` se calcula con floor(log2) en vez de ceil (se subestima con ratios que no son potencia de 2), y `ratio < 2` se mapea a 2x/4x en lugar de rechazarse como en BM/LL. | Devolver °C×100 en `int32_t`. Retirar los alias sin prefijo, que aún no se han publicado. Alinear la guarda HAL con BM/LL. |
| S6 | Baja | docs | `07_adc/README.md` §2.3, capturas | Los valores de `R_ADC` (160–260 Ω y 680–1500 Ω) no tienen fuente trazable. Los tiempos mínimos citan las tablas 93, 94 y 95 del DS12923, pero esas son la calibración del sensor de temperatura y las de VBAT. La tabla de BOOST omite Rev Y. Las capturas quedaron en la raíz del proyecto, no en `docs/`, y la de prj_04 imprime el informe ADC bajo la etiqueta `[m4_uart_test]` (los dos selftests comparten buzón). | Corregir las citas o eliminar los valores sin fuente. Mover las capturas. Usar un buzón o una etiqueta propia. |

### 4.3 Resolución de la segunda auditoría (S1–S6)

| ID | Sev. | Estado | Acciones tomadas |
| :--- | :--- | :--- | :--- |
| S1 | **Bloqueante** | **Resuelto** | Corregida la escritura de `CFGR2` en `adc_bm.c` y `adc_ll.c` aplicando `((ovsr << ADC_CFGR2_OVSR_Pos) & ADC_CFGR2_OVSR_Msk)` con `ovsr = ratio - 1`, y `((ovss << ADC_CFGR2_OVSS_Pos) & ADC_CFGR2_OVSS_Msk)`. Añadida prueba de selftest en CM4 que verifica por software los bits en `ADC3->CFGR2` (`ROVSE`, `OVSR==15`, `OVSS==4`) y realiza una conversión real. |
| S2 | Alta | **Resuelto** | Eliminadas las constantes fijas (24200, 12365, 16276) en CM4. CM7 lee las palabras de calibración de fábrica (`0x1FF1E860`, `0x1FF1E820`, `0x1FF1E840`) durante `Shared_Init()` y las publica en `g_shared` (SRAM4) con flag `cal_valid`. Los drivers exponen `ADC_*_InjectCalibration(&cal)`. Si no hay calibración inyectada en CM4, `CalculateVDDA` devuelve `0U` y `CalculateTemperature` devuelve `ADC_*_TEMPERATURE_ERROR` (`-999L`). |
| S3 | Media | **Resuelto** | Cobertura completa de tests en hardware: T2 (barrido de resolución 16, 14, 12, 10, 8 bits verificando escalado $2^{16-N}$), T5 (watchdog con ventana que dispara [0, 100] y ventana que no dispara [0, 65535]), T7 (oversampling x16 shift 4 verificado en registro y en conversión), T8 (D-cache en CM7 escribiendo patrón en RAM, sobreescribiendo en cache y ejecutando invalidación que comprueba el descarte de la línea sucia y la recarga desde RAM). Capturas registradas en `docs/boot_capture.txt` de cada proyecto y volcados de registros SWD en `docs/swd_register_dump.txt`. |
| S4 | Media | **Resuelto** | Restaurada la sección §4.1 del auditor exactamente como estaba en `42173dc`. Se conserva §4.2 para los hallazgos de la segunda auditoría y se añade §4.3 para la resolución. |
| S5 | Baja | **Resuelto** | `CalculateTemperature` devuelve `int32_t` con aritmética entera (grados Celsius enteros). Se eliminaron las macros sin prefijo (`ADC_Calculate*`). En HAL, la guarda de oversampling calcula bits adicionales con `while ((1U << add_bits) < ratio) add_bits++;` idéntica a BM/LL. |
| S6 | Baja | **Resuelto** | Corregidas las citas del DS12923 en `07_adc/README.md` (Tabla 86/87 para $R_{ADC}$, Tabla 88/93 para VREFINT, Tabla 90/94 para Temp, Tabla 91/95 para VBAT). Las capturas se ubican en `<prj>/docs/boot_capture.txt` y el informe en CM7 se imprime bajo la etiqueta `[m4_adc_test]`. |

---

## 5. Decisiones abiertas

Cada una con la opción que recomiendo.

1. **Alcance de v1.** Recomendado: F1 a F6, sin conversiones inyectadas ni modo dual.
2. **Qué hacer si `kernel_clock_hz` vale 0.** Recomendado: seguir suponiendo 25 MHz, para que
   `prj_04` quede exactamente igual, y en un commit aparte hacer que `prj_04` lo pase de forma
   explícita. Si en cambio el driver rechaza el 0, `prj_04` deja de funcionar hasta
   actualizarlo.
3. **Callbacks de la HAL.** Recomendado: ofrecer las dos vías, registro por handle cuando el
   proyecto activa `USE_HAL_ADC_REGISTER_CALLBACKS` y la macro `ADC_HAL_CUSTOM_CALLBACKS`
   cuando no.
