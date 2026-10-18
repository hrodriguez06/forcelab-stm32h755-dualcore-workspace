# Hoja de ruta de drivers pendientes (`01_shared_drivers`)

Orden recomendado para desarrollar los drivers que faltan en el catálogo, con
el porqué de cada posición. Complementa a `README.md` (reglas del catálogo) y
no lo sustituye.

> **La numeración de las carpetas no es el orden de desarrollo.** El prefijo
> `NN_` es la posición en la secuencia de arranque de una placa (pines, LED,
> DMA, ..., semáforo entre núcleos al final). El orden de desarrollo lo
> deciden las dependencias entre drivers, los consumidores que ya esperan y el
> riesgo que hoy está abierto. Por eso `10_hsem` va primero aquí aunque
> arranque el último.

---

## 1. Estado actual

| Carpeta | Estado | Notas |
| :--- | :--- | :--- |
| `00_gpio` | Hecho y cerrado (F1, F1b, F1c, F2 y cierre K1–K3) | BM, LL, HAL. F1: validado en `prj_01` con el AD2 (`00_gpio/README.md` §9), auditoría cerrada con la re-auditoría 5 en `75d106a`, sin auditoría independiente en la placa. F1b: el lock entre núcleos instalado en `prj_04` y todos sus escritores de configuración de GPIO por el driver (también la MSP de la consola de `04_uart`); G1 y G13 cerrados allí, con auditoría independiente en la placa y arranque en frío (`00_gpio/README.md` §13.1). F1c: lo mismo en `prj_03` (el hook, con `10_hsem` BM solo para él, y PB1 por `gpio_bm`); G1 cerrado en todos los proyectos, con auditoría independiente en la placa y arranque en frío (§13.2). F2: EXTI 0–15 del núcleo que llama (conflicto entre puertos, pending antes de desenmascarar, deinit que respeta al otro núcleo) y la celda de compensación (`READY` esperado fuera de la sección), validados en `prj_01` con el AD2 y dos corridas completas del auditor en `85b4325`: cuentas por la ISR y por el LA, latencia de 0,10–0,25 µs, `READY` tras ~76 400 ciclos (§9, §13.3). La compuerta de T9 en PF14 no se resuelve en el banco recableado y no contó para F2 (§10). Las capas LL y HAL (F3–F4 del plan) se hicieron y validaron con F1, y `gpio_hal` tiene consumidores (la MSP de la consola de `04_uart` y los pines RMII de `08_eth`). Cierre (`00_gpio/README.md` §13.4): `prj_02`, `prj_03` y `prj_04` encienden la celda de compensación con `GPIO_BM_EnableCompensationCell()` y comprueban `READY` (K1); EXTI y la celda quedan en `gpio_bm` por diseño, válidas desde cualquier tier (K2, G6 cerrado por diseño); el README dice el estado real (K3). Auditoría independiente con ejecución propia del auditor en la placa: `READY = 1` y `net_check` en los tres, 5/5 resets por NRST en `prj_04`, y el resto de objetos idéntico a `f066a8e`. Sin ejercitar: la celda en CM4, EXTI fuera de PE y las líneas 16–21 (§9.5). |
| `01_led` | Hecho y auditado (L1/L2 y L3–L5) | `led_bm`. L1/L2 cerrados: las seis macros `BOARD_LED_<x>_PORT/_BIT` son obligatorias (un `#error` por macro, como E1 de `08_eth`) y cada bit se comprueba en 0–15 al compilar; los cuatro `board_def.h` usan `_BIT`. L3–L5 cerrados (2026-10-18): `LED_Init_Config` configura los pines con `GPIO_BM_ConfigOutput` (sección y lock entre núcleos de `gpio_bm`, read-back); `LED_Toggle` es atómico en el núcleo que llama (`PRIMASK`); un LED con puerto o pin inválido, o que `gpio_bm` rechaza, queda deshabilitado, y la nueva `LED_Init_ConfigEx` devuelve la máscara. Validado con `selftest-led` en `prj_01` (LT1–LT5, el host juzga los registros medidos, con controles en la misma ejecución: la `LED_Toggle` anterior pierde ~2000 cambios con una ISR a 200 kHz, la nueva ninguno) y con T1–T4 en los cuatro proyectos; auditoría independiente con ejecución propia del auditor en la placa (`01_led/README.md` §6). Queda: `LED_PanicRed` fuera de `gpio_bm` (excepción G14), `LED_Toggle` no atómico entre núcleos, y un HSEM ocupado al iniciar deshabilita el LED (§5). |
| `02_dma` | Hecho y auditado | BM, LL, HAL. BDMA validado en M2M y con dos periféricos de D3: SPI6 (`06_spi` T7(b)) e I2C4 (`05_i2c` T7(b), con los tres tiers de I2C). N3 (`DMA_HAL_Init` rechazaba el modo directo con media palabra o palabra) corregido en `57b1bf7`, con la prueba T13 en `dd65b06` y dos controles negativos; ejecución independiente del auditor en `04fb865` (T13 PASS `ok=3 rej=6 mismatch=0`, NC1 FAIL `ok=1 rej=6 mismatch=2`; `02_dma/README.md` §9). |
| `03_tim` | Hecho (F0–F4) y auditado | BM, LL, HAL (TIM1–8, TIM12–17). Validado en `prj_01` con el AD2 (`03_tim/README.md` §9). Cada fase pasó una auditoría independiente con ejecución propia del auditor en la placa (F3 en `70b2062`: 259/259; cierre de F0–F3 auditado en `1cf8ad4`). F4: TIM3 (disparo del ADC por TRGO) y TIM7 (selftest de DMA) de `prj_03` y `prj_04` ya van por el driver: `tim_bm` en `prj_04`; `tim_hal` y `tim_bm` en `prj_03`, el primer `tim_hal` en CM4. El auditor lo ejecutó en la placa contra el control `04fb865` en la misma sesión: registros de TIM3 idénticos, tasa del ADC equivalente (Δ 0,0022 % y 0,049 %), 20/20 resets por SWD, los tres selftests y la red sin errores (§9.5, §11). Arranque en frío (T6b) de los dos proyectos tras apagar la placa: PASS. Pendiente: TRGO → DAC y TRGO2; LPTIM y HRTIM fuera (§9.5). Abierto fuera del driver: el reloj de la placa va un +1,3 % alto (HSE del MCO del ST-LINK), por encima de VOS1 (§10.5). |
| `04_uart` | Hecho y auditado (F1 y cierre) | BM, LL, HAL. F1: U1–U6 corregidos (la consola devuelve el estado de sus pines; LL valida antes de tocar el hardware, deja `CR1`–`CR3` limpios y comprueba el divisor que aplica OVER8; `InitEx` devuelve `HAL_BUSY`), D1–D3, D4 (sin `ATOMIC_*` en LPUART1, que colgaba el núcleo) y D5/D6 aprobadas por el responsable (`TakeErrors` recoge los flags del hardware; `UART_HAL_Init`/`InitEx` rechazan instancias que no son UART). Validado en `prj_01` (`selftest-uart`, UT0–UT9) con JU1 (PD5 → 1 kΩ → PD6), JU2 (PD4 → PD3) y el AD2 en PD6 (DIO15) y MCO1 (DIO14): 15 filas de formato, velocidad y tier medidas contra MCO1 a ±0,5 %, errores PE/FE/NE/ORE inyectados, RTS/CTS, HSEM entre núcleos, DMA (ES0445 2.20.3), cada paso con su control en la misma ejecución; más UT10 en `prj_04`. Auditoría independiente con ejecución propia del auditor en la placa sobre la imagen final `f584633`: 3/3 sesiones, 20/20 warm resets, el control de UT9 reproduce U1 (`04_uart/README.md` §9). Sin ejercitar: LPUART1 en el cable (solo `PRESC`/`BRR`), 9 bits de datos por la API de bytes, modos síncrono/IrDA/LIN/RS-485 (§10). |
| `05_i2c` | Hecho (F0–F3) y auditado | BM, LL, HAL (I2C1–I2C4, maestro, 7 bits, hasta 1 MHz; sondeo, IRQ y DMA/BDMA; `CalcTiming`, timeout de SCL bajo, recuperación del bus). Validado en `prj_01` contra una NUCLEO-F334R8 como target (`tools/i2c_target_f334/`) y el AD2 en dos tomas desmontables (`05_i2c/README.md` §8–§9). Cada fase pasó una auditoría independiente con dos ejecuciones propias del auditor en la placa (con y sin tomas; la última, F3 en `01e462f`: 149/149 y 112/112); cierre auditado. I2C4 por BDMA en verde. Hechos de silicio nuevos: TIMEOUT se repite cada TIMEOUTA con SCL retenida, el TIMEOUT de maestro no manda STOP, `BUSY` solo tras un START visto con `PE = 1` (§6.3). Pendiente: esclavo, 10 bits y SMBus/PEC (segunda iteración); I2C2/I2C3 sin pines en el banco; sin consumidores fuera del selftest. Abierto fuera del driver: el reloj de la placa (+1,3 %: el bus va un +0,8 a +1,1 % por encima de lo pedido, §6.4). |
| `06_spi` | Hecho (F0–F3) y auditado | BM, LL, HAL (SPI1–SPI6, maestro). Validado en `prj_01` con el AD2 y el puente J1 (`06_spi/README.md` §9). Cada fase pasó una auditoría independiente con ejecución propia del auditor en la placa (la última, F3 en `b267f4c`: 45/45); cierre auditado en `e128eed`. Primer consumidor real de BDMA (SPI6). Pendiente: esclavo, CRC, TI e I2S (segunda iteración); SPI2/3/5 sin transferencias en el banco, porque un maestro sin SCK en AF no termina (`06_spi/README.md` §10.2); sin consumidores fuera del selftest. Abierto fuera del driver: el reloj de la placa (+1,3 %, §6.3). N3 de `02_dma` se corrigió en `57b1bf7`; T7(c) de este driver sigue en modo FIFO (§10.3). |
| `07_adc` | Hecho y auditado | BM, LL, HAL. |
| `08_eth` | Hecho y auditado | HAL (ETH MAC, RMII, PHY LAN8742A). E1, E2, E5 y E8 cerrados: `board_def.h` obligatorio con un `#error` por macro, `#error` si `USE_HAL_ETH_REGISTER_CALLBACKS`, contadores atómicos, pines RMII por `gpio_hal` en la inicialización y en la desinicialización (`08_eth/README.md` §10.1). Validado en `prj_02` con `net_check.py` y `monitor.py` a la vez; F1 y el cierre pasaron una auditoría independiente con ejecución propia del auditor en la placa (la última, sobre la imagen final `2165309`). E3–E7 cerrados (§10): E3 resuelto con `ETH_HAL_CUSTOM_CALLBACKS` (comprobado sin placa, sin consumidor); E4 por diseño (un solo MAC); E6 medido en la placa y no adoptado (el cero-copia pierde 24 de 5000 datagramas con 32 en vuelo y rinde 32,8 frente a 54,1 Mbit/s); E7 conservado. `prj_02`, `prj_03` y `prj_04` compilan objetos idénticos al control `69179d7`, comprobado por el auditor en su propia ejecución. Pendiente: lo no ejercitado (§9.6: 10 Mbps, desenchufar bajo tráfico, `HAL_ETH_DeInit`…). La configuración de alimentación que faltaba en `prj_02`, `prj_03` y `prj_eth_lwip_node` (no arrancaban tras un POR) se corrigió en `5dd43a2`, comprobado en los tres tras un apagado real (`prj_02/README.md` §6). |
| `09_fdcan` | **Vacío** | |
| `10_hsem` | Hecho y auditado | BM, LL, HAL + utilidad de arranque. Auditoría cerrada en `b6d0b26`; lo pendiente, en `10_hsem/README.md` §11.5 y §12. |

---

## 2. Criterios para ordenar

En este orden de peso:

1. **Dependencias.** Un driver no empieza hasta que existe lo que necesita
   (open-drain para I2C, HSEM para el lock de GPIO).
2. **Riesgo abierto hoy.** Un fallo real en un proyecto que ya funciona pesa más
   que una función nueva.
3. **Consumidores esperando.** Código escrito a mano en los proyectos que el
   driver sustituiría: cuanto más duplicado, antes.
4. **Coste de validación.** Lo que se prueba en la Nucleo sin hardware externo
   va antes que lo que necesita un dispositivo o un transceptor.
5. **Riesgo técnico.** El driver más complejo va cuando el patrón
   periférico + DMA + selftest ya está probado en otros.

---

## 3. Grafo de dependencias

```text
10_hsem ──► 00_gpio F1 (lock entre núcleos, G1) ──► 00_gpio F2 (EXTI)
                    │                                     │
                    ├──► 06_spi (salida para CS) ◄────────┤ (IRQ de dispositivo)
                    └──► 05_i2c (AF open-drain) ◄─────────┘
03_tim ──► 07_adc (disparo TRGO, por el driver desde F4)
      └──► 06_spi / 05_i2c (timeouts y marcas de tiempo)
02_dma ──► 06_spi, 05_i2c (SPI1-5/I2C1-3 por DMA1/2; SPI6/I2C4 por BDMA)
09_fdcan ── sin dependencias nuevas; RAM de mensajes compartida (usa 10_hsem si
            los dos núcleos la tocan)
```

---

## 4. Orden recomendado

| # | Driver | Motivo principal |
| :-: | :--- | :--- |
| 1 | `10_hsem` | Pequeño, duplicado en todos los proyectos, y lo necesita el paso 2 |
| 2 | `00_gpio` F1–F2 | Cierra G1, un fallo real hoy en `prj_04`, y da open-drain, salida y EXTI |
| 3 | `03_tim` | TIM3/TIM7 ya se programan a mano en los proyectos |
| 4 | `06_spi` | Se valida con un puente; primer consumidor real de BDMA |
| 5 | `05_i2c` | Depende de open-drain y necesita un dispositivo externo |
| 6 | `09_fdcan` | El más complejo, sin consumidor y sin transceptor en la Nucleo |

`00_gpio` está cerrado: un proyecto que necesite EXTI la configura con `GPIO_BM_ExtiConfig()` desde
cualquier tier (`00_gpio/README.md` §7, ejemplo F).

---

## 5. Detalle por driver

### 5.1 `10_hsem` — semáforo hardware entre núcleos

**Por qué primero**

- **Duplicado en todos los proyectos.** El arranque sincronizado con HSEM 0
  (CPU2 en STOP hasta que CPU1 libera el semáforo) está copiado en el `main.c`
  de CM4 y CM7 de la plantilla, `prj_02`, `prj_03`, `prj_04` y
  `prj_eth_lwip_node`. Cada copia repite la parte delicada: despertar de STOP
  por *evento* con `SEVONPEND`, no por interrupción, documentada en
  `prj_04 CM4/Core/Src/main.c`.
- **`shared_memory.h` ya reparte IDs** (0 arranque, 1 estado, 2 y 3 buzones,
  4 datos del ADC en `prj_04`) y define `SHARED_LOCK()` como un bucle sin
  límite sobre `HAL_HSEM_FastTake()`. Solo `prj_04` tiene un take acotado
  (`Shared_TryLock`), y lo acota en vueltas, no en tiempo. No hay diagnóstico de
  quién retiene el semáforo, ni versión sin HAL para CM4.
- **Desbloquea el paso 2** y cualquier registro compartido entre núcleos que hoy
  se evita: el hook `GPIO_BM_SetSharedLock()` del plan de GPIO, la selección de
  reloj de kernel en RCC (fuera del banco por núcleo, por eso `04_uart` no la
  toca), `SYSCFG_PMCR` y `SYSCFG_EXTICR`.
- **Validación barata:** solo hacen falta los dos núcleos.

**Alcance**

| Tier | Contenido |
| :--- | :--- |
| BM | Take en 1 paso (lectura de `RLR`) y en 2 pasos (escritura con `PROCID` + verificación), release, estado (`IsLocked`, núcleo y proceso dueño), notificación por interrupción/evento por núcleo, borrado de flags con `C1ICR`/`C2ICR`. Take con timeout acotado en ciclos. |
| LL | Mismas funciones sobre `stm32h7xx_ll_hsem.h`. |
| HAL | Envoltorio fino de `HAL_HSEM_*` con timeout y la misma semántica de retorno. |
| Utilidad | Secuencia de arranque (CPU1 libera, CPU2 espera en STOP) como función del driver, parametrizada por ID y sin asumir FreeRTOS. |

Los IDs son del proyecto (`shared_memory.h`), nunca del driver (regla 1).

**Riesgos y reglas de RM0399 a cubrir**

- El release solo lo acepta el núcleo y el `PROCID` dueño; un release con
  `PROCID` equivocado se ignora en silencio.
- 1 paso usa `PROCID = 0`; mezclar take de 1 paso con release de otro
  `PROCID` deja el semáforo tomado.
- La interrupción de liberación llega a los dos núcleos solo si cada uno la
  habilita en su propio `CxIER`.
- `SEVONPEND` es lo que convierte la notificación pendiente en un evento que
  saca a CPU2 de `WFE`.

**Pruebas (en la placa, sin hardware externo)**

- H1: exclusión mutua. Los dos núcleos incrementan un contador compartido en
  SRAM4 N veces bajo lock; el total es exactamente 2N.
- H2 (control negativo): el mismo test sin lock pierde incrementos. Si no los
  pierde, H1 no demuestra nada.
- H3: release con `PROCID` equivocado no libera.
- H4: timeout: un núcleo retiene, el otro vuelve con error dentro del límite.
- H5: notificación: CPU2 despierta de STOP por la liberación, y no antes.
- H6: arranque migrado en `prj_04` con el mismo log de hoy.

**Criterio de salida:** H1–H6 en verde; `prj_04` y la plantilla usan el driver
para el arranque; `tools/build_all.ps1 -Clean` 8/8.

---

### 5.2 `00_gpio` F1–F2 — núcleo BM completo y EXTI

El plan se retiró al cerrar F1 (`00_gpio/README.md` §15 dice cómo recuperarlo); aquí solo el porqué del
orden.

**Por qué segundo**

- **G1 es un fallo real hoy.** En `prj_04`, CM7 configura PB13 (TXD1 de
  Ethernet, `HAL_GPIO_Init`) en `net_task` después de liberar a CPU2, y CM4
  configura PB1 (ADC) al despertar, escribiendo `MODER` y `PUPDR` a mano en
  `GPIO_Analog_Init()` (`CM4/App/Src/adc_acquisition.c`). Ninguna de las dos
  escrituras pasa por `gpio_bm`: las dos son read-modify-write sin protección
  sobre el mismo `GPIOB_MODER`. Si se intercalan, PB13 puede volver a
  analógico: el enlace sigue arriba y la transmisión muere sin que nada lo
  detecte. F1 lo cierra con sección crítica y el hook HSEM del paso 1, y
  haciendo que **todos** los escritores de un puerto compartido tomen ese lock
  (hallazgo G13 del plan).
- **Lo que necesitan los siguientes:**
  - I2C necesita AF **open-drain**; hoy `GPIO_BM_ConfigAlternate()` solo hace
    push-pull.
  - SPI necesita un pin de **salida** para CS.
  - Los dispositivos SPI/I2C suelen tener una línea de interrupción: **EXTI** (F2).

**Alcance:** F1 (G1–G5, G8–G10 del plan) y F2 (EXTI 0–15 y SYSCFG). F3–F4
(`gpio_ll`, `gpio_hal`) se aplazaron hasta que hubiera consumidor, pero se hicieron y validaron con F1. F5 solo si
aparece un canal ADC en los pads `_C`.

**Criterio de salida:** el del plan de GPIO para F1 y F2, incluida T15 (la
carrera entre núcleos con y sin lock) y no regresión de `04_uart` y `07_adc`.

---

### 5.3 `03_tim` — timers

**Por qué tercero**

- **Consumidores esperando:**
  - `prj_03` y `prj_04` programan TIM3 a mano en
    `CM4/App/Src/adc_acquisition.c` (TRGO a 500 kHz para el ADC). Además, el
    reloj del timer va fijo en el comentario (`APB1 timer clock = 200 MHz`).
  - El selftest de DMA (`prj_04 CM4/App/Src/dma_selftest.c`) usa TIM7 como
    fuente de peticiones.
  - `07_adc/README.md` deja el disparo explícitamente "a la aplicación".
- **Lo aprovechan los siguientes:** timeouts y marcas de tiempo precisos para
  SPI e I2C, y un generador de señal para probarlos.
- **Se valida en la placa:** salida PWM a entrada de captura con un puente, o
  el Analog Discovery 2.

**Alcance (primera iteración)**

| Función | Timers |
| :--- | :--- |
| Base de tiempo, interrupción de update, one-pulse | Todos, incluidos TIM6/TIM7 |
| TRGO (MMS) como disparo de ADC/DAC/DMA | TIM1–8, TIM15 |
| Petición DMA de update / CC | Los que la tienen |
| PWM por canal, polaridad, preload | TIM1–5, 8, 12–17 |
| Captura de entrada (frecuencia/periodo) | TIM2–5, 12–17 |

Fuera de la primera iteración: encoder, salidas complementarias con tiempo
muerto y break (TIM1/TIM8), HRTIM y LPTIM. LPTIM tiene sus propias erratas
(2.17.x) y merece carpeta aparte si aparece un consumidor de bajo consumo.

**Contrato:** el llamador pasa `timer_clock_hz`, igual que `kernel_clock_hz` en
`04_uart`. El reloj de un timer depende del prescaler APB y de
`RCC_CFGR.TIMPRE` (x1, x2 o x4 según RM0399), así que el driver no lo adivina.
El driver calcula PSC/ARR a partir de una frecuencia y devuelve la frecuencia
**real** obtenida.

**Erratas a revisar:** ES0445 2.16.1–2.16.4 (one-pulse con reset+trigger,
compare consecutivo, OC clear con reset externo, PWM con `ocref_clr`).

**Pruebas:** frecuencia de update medida contra el SysTick/DWT; PWM medido por
captura en otro timer (oráculo independiente); TRGO disparando el ADC con la
tasa de `prj_04`; control negativo con MMS mal programado (el ADC no convierte).
Migración de TIM3 en `prj_04` sin cambiar T12 (~500 kSPS, `err/ovr = 0`).

**Criterio de salida:** BM, LL y HAL con paridad; `prj_04` usa el driver para
TIM3; T12 igual que hoy.

---

### 5.4 `06_spi` — SPI1–SPI6

**Por qué antes que I2C**

- **Validación sin dispositivo:** un puente MOSI–MISO en el conector basta para
  probar el modo maestro full-duplex a varias velocidades.
- **El mejor consumidor de `02_dma`:** SPI1–5 por DMA1/DMA2 y **SPI6 por BDMA**.
  `02_dma/README.md` §9 deja como no ejercitado "BDMA con un periférico real de
  D3"; SPI6 lo cierra.
- **Protocolo más simple que I2C:** sin arbitraje, sin ACK, sin recuperación de
  bus. Establece el patrón periférico + DMA + selftest que I2C reutiliza.

**Alcance (primera iteración)**

- Maestro, full-duplex y solo transmisión, modos 0–3, 4–32 bits por trama,
  MSB/LSB primero.
- `TSIZE` y `EOT` del SPI de H7 (distinto al de F4), umbrales de FIFO, CS por
  software (GPIO del paso 2) y por hardware.
- Transferencias por sondeo, por interrupción y por DMA.
- Esclavo y CRC se dejan para una segunda iteración.

**Erratas:**

- 2.22.1: el maestro se bloquea con SYSCLK mucho más rápido que SCK.
- 2.22.3: `TXP` con el SPI deshabilitado.
- 2.22.2, 2.22.4 y 2.22.5: CRC y `EOT` (entran en la iteración de CRC).

**Pruebas:**

- Puente MOSI–MISO con patrón pseudoaleatorio en todos los modos y tamaños.
- Control negativo sin puente: los datos no coinciden. Si coinciden, la prueba
  no mide nada.
- DMA en DMA1/2 y en BDMA (SPI6).
- Barrido de velocidad hasta el límite del pin; captura de SCK/CS con el AD2.

**Criterio de salida:** BM, LL y HAL; SPI6 + BDMA en verde (se actualiza
la lista de no ejercitado de `02_dma/README.md` §9); erratas 2.22.1/2.22.3 aplicadas y documentadas.

---

### 5.5 `05_i2c` — I2C1–I2C4

**Por qué quinto**

- **Depende de open-drain** (paso 2) y de calcular `TIMINGR` a partir del reloj
  de kernel, que también pasa el llamador.
- **Tiene más formas de fallar:** bus colgado (SDA bajo, recuperación con 9
  pulsos de SCL por GPIO), arbitraje, NACK, timeouts, stretching.
- **Necesita un dispositivo externo:** el AD2 como esclavo I2C, o un sensor
  barato. La Nucleo no trae ninguno en el bus.
- **I2C4 está en D3 y va por BDMA,** así que conviene que SPI6 haya validado
  ese camino primero.

**Alcance (primera iteración):** maestro, 7 bits, 100 kHz / 400 kHz / 1 MHz,
escritura, lectura y escritura+lectura con restart (registro de dispositivo),
por sondeo, IRQ y DMA; recuperación de bus. Esclavo y SMBus en una segunda
iteración.

**Erratas:**

- 2.19.2: error de bus espurio en modo controlador.
- 2.19.4: `START` se borra al poner `ADDRCF`, no al coincidir la dirección.
- 2.19.6: transmisión detenida tras el primer byte.
- 2.19.1: muestreo con `tSU;DAT` menor que un ciclo de kernel. Restringe los
  `TIMINGR` válidos.
- 2.19.7: SDA bajo tras timeout SMBus en modo target (segunda iteración).

**Pruebas:** AD2 como esclavo con registros conocidos; lectura/escritura a las
tres velocidades con tiempos medidos por el AD2; NACK a dirección inexistente;
bus colgado provocado (el AD2 retiene SDA) y recuperado; control negativo sin
pull-up (el driver devuelve error, no se cuelga).

**Criterio de salida:** BM, LL y HAL; recuperación de bus demostrada; I2C4 +
BDMA en verde.

---

### 5.6 `09_fdcan` — FDCAN1/FDCAN2

**Por qué último**

- **Es el más complejo:**
  - temporización nominal y de datos (CAN FD);
  - reloj de kernel propio;
  - filtros;
  - RAM de mensajes de 10 KB en `0x4000AC00`, compartida por FDCAN1 y FDCAN2.
  El reparto de esa RAM es un contrato entre instancias y, si los dos núcleos
  usan una cada uno, también entre núcleos (paso 1).
- **Sin consumidor hoy:** ningún proyecto usa CAN.
- **Sin transceptor en la Nucleo:** la validación se limita al loopback interno
  (y al externo con TX/RX puenteados) mientras no haya hardware CAN.

**Alcance:** HAL primero (encaja en "stacks complejos" de la matriz del
`README.md` raíz), con un reparto explícito de la RAM de mensajes como
parámetro. BM/LL solo si aparece un consumidor en CM4 sin HAL.

**Erratas:**

- 2.23.1: desincronización con filtrado de flancos (`EFBI`). Se deshabilita.
- 2.23.2: mensajes de la FIFO Tx invertidos cuando se mezcla con buffers
  dedicados de mayor prioridad.
- 2.23.3: fallo de transmisión en modo DAR por arbitraje perdido.

**Pruebas:** loopback interno en clásico y FD a varias tasas; FDCAN1 y FDCAN2
simultáneos con particiones de RAM que no se pisan (y un control negativo que
sí se pisa); filtros aceptando y rechazando IDs conocidos.

**Criterio de salida:** HAL con loopback interno en verde en las dos
instancias; lo no validado sin transceptor, escrito.

---

## 6. Ciclo de trabajo por driver

El mismo que se siguió con `02_dma` y `07_adc`:

1. **Auditoría de lo existente** (código a mano en los proyectos que el driver
   va a sustituir) y **plan** con contratos de API, matriz RM0399/ES0445,
   pruebas y fases.
2. **Rama** `driver/<nombre>`.
3. **Implementación** por fases, cada una con `tools/build_all.ps1 -Clean` 8/8
   con `-Werror`.
4. **Auditoría independiente** con las reglas de siempre:
   - R1: cada prueba tiene su control negativo;
   - R2: el hash de la imagen (`FW_GIT_REV`) aparece en cada captura;
   - R3: los tamaños salen del `.map` enlazado, sin "Discarded input sections";
   - R4: las pruebas no son destructivas.
   Además, el oráculo es independiente: una prueba que compara el driver
   consigo mismo no cuenta.
5. **Verificación en la placa** por quien audita, no solo por quien implementa.
6. **README** del driver con lo validado, lo no ejercitado y las limitaciones.
   Los documentos de proceso se borran al cerrar y quedan en el historial.
7. **Merge** y borrado de la rama.

Reglas del catálogo que aplican a todos (`README.md`): sin pines ni instancias
fijos; firmas congeladas una vez publicadas; sufijo de tier; sin prefijo
numérico en los `#include`; macro de `board_def.h` ausente = `#error`.

---

## 7. Fuera de esta hoja de ruta

Sin consumidor ni carpeta hoy: LPTIM, HRTIM, DAC, QUADSPI, FMC, SDMMC, USB, SAI,
RTC, watchdogs y MDMA. Entran cuando un proyecto los pida, en el slot de
arranque que les corresponda (renumerando con `git mv`, como indica el
`README.md`).

---

## 8. Decisiones abiertas

- **¿Hay hardware para I2C y CAN?** Si se dispone de un sensor I2C o de un
  transceptor CAN, las pruebas de 5.5 y 5.6 cambian de alcance. Sin ellos, el
  AD2 y el loopback interno son el techo.
- **¿`10_hsem` asume FreeRTOS?** La propuesta es que no: el driver ofrece take
  con timeout en ciclos y el proyecto decide si ceder la CPU mientras espera.
- **¿Migrar todos los proyectos o solo `prj_04` y la plantilla?** La propuesta
  es migrar esos dos en cada driver y dejar `prj_02`/`prj_03` para cuando se
  toquen por otro motivo. `prj_eth_lwip_node` no se migra (sus copias privadas
  son una decisión registrada).
