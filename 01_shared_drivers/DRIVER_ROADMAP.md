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
| `00_gpio` | Parcial | Solo `gpio_bm`: reloj de puerto, AF push-pull, analógico. Plan F0–F6 escrito en `00_gpio/IMPLEMENTATION_PLAN.md`, sin ejecutar. |
| `01_led` | Hecho | `led_bm`. Sin README. |
| `02_dma` | Hecho y auditado | BM, LL, HAL. BDMA solo validado en M2M. |
| `03_tim` | **Vacío** | |
| `04_uart` | Hecho | BM, LL, HAL. |
| `05_i2c` | **Vacío** | |
| `06_spi` | **Vacío** | |
| `07_adc` | Hecho y auditado | BM, LL, HAL. |
| `08_eth` | En trabajo | HAL. E1 y E2 abiertos (ver `08_eth/README.md` §10). |
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
03_tim ──► 07_adc (disparo TRGO, hoy a mano)
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

Tareas que se intercalan sin bloquear a nadie: README de `01_led`; E1 y E2 de
`08_eth` cuando termine el trabajo en curso; `00_gpio` F3–F4 cuando aparezca un
consumidor de `gpio_ll` / `gpio_hal`.

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

El plan ya existe (`00_gpio/IMPLEMENTATION_PLAN.md`); aquí solo el porqué del
orden.

**Por qué segundo**

- **G1 es un fallo real hoy.** En `prj_04`, CM7 configura PB13 (TXD1 de
  Ethernet, `HAL_GPIO_Init`) en `net_task` después de liberar a CPU2, y CM4
  configura PB1 (ADC, `GPIO_BM_ConfigAnalog`) al despertar. Las dos escrituras
  son read-modify-write sin protección sobre el mismo `GPIOB_MODER`. Si se
  intercalan, PB13 puede volver a analógico: el enlace sigue arriba y la
  transmisión muere sin que nada lo detecte. F1 lo cierra con sección crítica
  y el hook HSEM del paso 1.
- **Lo que necesitan los siguientes:**
  - I2C necesita AF **open-drain**; hoy `GPIO_BM_ConfigAlternate()` solo hace
    push-pull.
  - SPI necesita un pin de **salida** para CS.
  - Los dispositivos SPI/I2C suelen tener una línea de interrupción: **EXTI** (F2).

**Alcance:** F1 (G1–G5, G8–G10 del plan) y F2 (EXTI 0–15 y SYSCFG). F3–F4
(`gpio_ll`, `gpio_hal`) se aplazan hasta que haya consumidor. F5 solo si
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
