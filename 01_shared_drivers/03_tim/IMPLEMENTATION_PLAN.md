# Plan del driver TIM 03\_tim

Oct 2, 2026 · @Hector · revisión 4 (sustituye a la revisión 3, `92983ec`)

Este plan cubre el paso 3 de `DRIVER_ROADMAP.md`: el driver de timers `03_tim` en sus tres tiers, `tim_bm`, `tim_ll` y `tim_hal`. Tiene una fase F0 de preparación del banco, tres fases de implementación (F1 BM, F2 LL, F3 HAL) y una de cierre.

Cada fase pasa una auditoría independiente antes de empezar la siguiente, con las reglas de [Protocolo de auditoría](#protocolo-de-auditoría). Las reglas del implementador están en [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md).

Base: `e7542dc` de `main` (merge de `00_gpio` F1), árbol limpio, rama `driver/tim`.

## Cambios respecto a la revisión 3

Salen de la auditoría 4 de F0 (`AUDIT.md`, `f9bdb78`, decisión D6), que el responsable del catálogo aprobó el 2026-10-02. En 17 ejecuciones con la revisión 3, d = ē − x tiene una desviación típica de 184,5 ppm, frente a los unos 108 ppm que declara cada ejecución. El rms de z es 2,00 y fallan 2 de 17, en los dos casos solo por TREF. Las 8 capturas de un grupo no son independientes: el reloj de la placa deriva en escalas de segundos.

| # | Cambio | Por qué |
| --- | --- | --- |
| R4-1 | **Criterio de TREF** (sustituye al criterio de R3-1; las capturas y el ajuste de x de R3-1 no cambian): \|ē\| ≤ 20 000 ppm y \|ē − x\| ≤ 3·√(u² + s\_ef²/16 + σ\_ser²), con **σ\_ser = 145 ppm**. σ\_ser es la variación entre ejecuciones que u y s\_ef no explican: √(media(d²) − media(u² + s\_ef²/16)) sobre las 17 capturas de la revisión 3 (`AUDIT.md`, auditoría 4). La captura imprime σ\_ser y su fuente. σ\_ser es de este banco: si cambia la placa o el ST-LINK, se vuelve a medir con una serie de ≥ 10 ejecuciones | Con σ\_ser, el rms de z de la serie es 1,05, el máximo de \|z\| es 2,55 y no falla ninguna. Perder sensibilidad no tiene coste: desde R3-2, nada se corrige con TREF, y un error grueso (≥ 0,5 %) sigue siendo más de 7 veces la cota |
| R4-2 | **Base de la producción de `prj_01`: `2481396`**, en lugar de `e7542dc`, para C9, A16, el gate de F0 y el informe. `.text`/`.data`/`.bss`: CM7 debug 29304/132/55668, CM7 release 28356/132/55672, CM4 debug 3456/24/12560 y CM4 release 3112/24/12560. Desde `2481396`, ningún commit de `03_tim` puede cambiarlos. El alcance (A23) se sigue midiendo desde `e7542dc` | `2481396` (`HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY)` en `SystemClock_Config`) corrige un defecto latente de `prj_01`, que no arrancaba después de un arranque en frío. Es una desviación aprobada por el responsable el 2026-10-02: va con su propia fila en el informe de cierre y en el README |

## Cambios respecto a la revisión 2

Salen de la auditoría 3 de F0 (`AUDIT.md`, `7ba1539`, decisión D5) y de la sección "Problema abierto" del informe. El responsable del catálogo aprobó D5 el 2026-10-01.

El problema es que el reloj de la placa varía cientos de ppm. En tres de las seis capturas de F0, una ventana de 40 µs de MCO1 se separa de la media de 12 s más de lo que explica la cuantización: hasta −2,44 muestras, unos 620 ppm. Además, x cambia de una ejecución a otra (+12 847 → +13 571 ppm). Un único TREF no sirve como referencia, y con u medido, la cota de R2-2 se queda en ≈ 3r.

| # | Cambio | Por qué |
| --- | --- | --- |
| R3-1 | **TREF en dos grupos y nuevo criterio** (sustituye a R2-2; el criterio lo sustituye R4-1). **TREF\_A** va después de T0a, con el control de R2-1. **TREF\_B** es el último paso antes de `DONE`, sin control. Cada grupo hace 8 capturas del LA, cada una de ≥ 40 periodos de MCO1, separadas ≥ 100 ms. Se registra cada eᵢ con su rᵢ. Con las 16 capturas se calculan ē (media) y s (desviación típica, n − 1), y se toma s\_ef = max(s, r/√6); r/√6 es la desviación típica de la cuantización de los dos flancos extremos. **x:** CPU1 imprime una ráfaga de 4 líneas `TICK` (cada 50 ms) al empezar cada paso, antes de la línea `STEP`, y de 16 en el banner y en el veredicto. El host ajusta por mínimos cuadrados las medias de cada ráfaga (k̄ⱼ, t̄ⱼ), con M ≥ 10 ráfagas, y u es el error estándar de la pendiente con M − 2 grados de libertad. **Criterio:** \|ē\| ≤ 20 000 ppm y \|ē − x\| ≤ 3·√(u² + s\_ef²/16). Se registran también ē\_A, ē\_B, cada pendiente por ráfaga y u | Con 8 + 8 capturas, e cubre el principio y el final del mismo intervalo que mide x, y su dispersión es la medida, no r. Ajustar sobre las medias de ráfaga hace que u incluya cualquier diferencia de latencia entre ráfagas (O2) |
| R3-2 | **Desde F1, las frecuencias que mide el AD2 se refieren a MCO1 en la misma adquisición** (sustituye a "corregido con TREF" de R2-9). En los pasos de T6 y T8, PA8 está en AF0 con MCO1 = HSE/8, y el LA captura DIO14 junto con los canales medidos. En `prj_01`, f\_timclk = 200·f\_MCO1 exactamente: los timers van a HSE·25 (PLL1 entero, `PLLM = 4`, `PLLN = 400`, `PLLP = 2`, `PLLFRACN = 0`, `CM7/Core/Src/main.c:200-203`, HPRE /2) y MCO1 a HSE/8. Por tanto: T6 compara f\_salida/f\_MCO1 con 200/((PSC+1)(ARR+1)), y T8 espera un periodo de 200·f\_MCO1,medida/f\_DigitalOut ticks. TREF\_A/TREF\_B siguen en cada captura como validación del banco, no como corrección | El reloj de la placa y el del AD2 se cancelan en el cociente, porque MCO1 y los timers salen de la misma fuente. Un único TREF al principio arrastra hasta unos 600 ppm |
| R3-3 | **Protocolo del banco v2:** `TIM_BENCH_VERSION = 2`. Tabla de pasos: `T0A`, `TREF_A`, `T0B`, `T0B_S5NEG`, `T0B_TIMEOUT`, `T0B_DOUT`, `T0B_S12`, `TREF_B`, `DONE`, igual en el firmware y en la tabla `STEPS` del host (S7/S8) | Cambian los pasos y las líneas de consola |

Si la ejecución falla con el criterio de R3-1, el implementador se detiene e informa con la captura. No cambia el criterio por su cuenta.

## Cambios respecto a la revisión 1

Salen de la entrega de F0 (`IMPLEMENTATION_REPORT.md`, §5.5 y §5.7) y de la parte 1 de la auditoría de F0 (`AUDIT.md`, `83271b1`). Las decisiones R2-1 a R2-3 son del responsable del catálogo, del 2026-10-01, y se confirmaron después de la auditoría (N3).

| # | Cambio | Por qué |
| --- | --- | --- |
| R2-1 | **Control de TREF:** PA8 en AF1 (TIM1\_CH1) con TIM1 sin reloj en los dos bancos y MCO1 todavía en HSE/8. Ya no es `MCO1PRE = 0000` | E1: en esta placa (rev V), `MCO1PRE = 0000` deja salir HSE sin dividir (332 flancos en 40,96 µs, `tim_f0_bench_d01061e.txt`), aunque RM0399 lo llama "prescaler disabled". Es el control de T10 de `00_gpio` |
| R2-2 | **(Sustituido por R3-1.) Criterio de TREF:** \|e\| ≤ 20 000 ppm y \|e − x\| ≤ 3·√(u² + r²), donde x es la verificación cruzada (`HAL_GetTick` frente al reloj del host), u su incertidumbre y r la resolución de TREF de esa captura. Ya no es \|e\| ≤ 500 ppm | E2: el HSE de la placa mide +1,29 % con dos referencias independientes. La cota absoluta solo detecta un reloj absurdo; lo que valida TREF es que dos referencias independientes coincidan. Se añade r porque e salta en pasos de una muestra del LA (unos 253 ppm) |
| R2-3 | **S11 (d):** mientras corre Digital Out, el `OE` de Digital-IO es 0 en los bits **de los canales de Digital Out** (read-back), no en todos. HOST\_REQ puede seguir habilitado | T8 necesita el handshake mientras el AD2 genera. El SDK, §8, aplica Digital Out por línea, donde Enable y Output de Digital-IO son 0. S3 mantiene HOST\_REQ fuera del bus |
| R2-4 | **S11 (c):** `Idle = Low` solo en el control de T0b(f), ligado a su paso. En cualquier otro caso, `Idle = Zet` | m1 de la auditoría de F0 |
| R2-5 | **`-Werror`:** `prj_01` no lo lleva en sus flags, y añadirlo cambiaría su producción. C9 pide 8/8 en `build_all` y 0 avisos en las imágenes de `prj_01`, con una compilación aparte con `CMAKE_C_FLAGS=-Werror` como evidencia | E3 |
| R2-6 | **T0a:** las 13 líneas que no son del handshake, sueltas y en walking-1, con un control negativo (DIO9 con pull-down donde el host espera pull-up). HOST\_REQ y MCU\_ACK quedan cubiertos por cada handshake | E4: el T0a de `00_gpio` no tenía walking-1 ni control, y las instrucciones §3.5 exigen el control |
| R2-7 | **Orden de la sesión:** T0a → TREF → T0b → pruebas. TREF no mueve el bus ni usa Digital Out. T0b sigue yendo antes que cualquier prueba que sí lo haga | Informe §5.5 |
| R2-8 | **Tabla de líneas:** PE8, PE10, PE12 y PE15 nunca van en AF. Solo son salida GPIO en T0b(c), como en `00_gpio` | Informe §5.5 |
| R2-9 | **Reloj real:** los relojes nominales (8/400/200 MHz) no son los reales; se mide unos +1,29 % (unos 202,6 MHz en los timers). Las medidas contra `CYCCNT` y las capturas internas no cambian, porque usan el mismo reloj. Lo que mide el AD2 se corrige con TREF (en la revisión 3, lo sustituye R3-2) | E2. La nota N1 de la auditoría (SYSCLK por encima del límite de VOS1) es del responsable del catálogo y no cambia este plan |
| R2-10 | **u de la verificación cruzada:** sale de una medida, no de un supuesto | m2 de la auditoría de F0 |

## Encargo

Texto del responsable del catálogo, 2026-10-01:

> Desarrolla el driver TIM bajo el estándar de driver de catálogo, asegurando que incluya implementaciones: BM, HAL y LL.
>
> Como restricción de alcance, no intentes validar ni corregir el resto de los proyectos del repositorio; enfoca la validación exclusivamente en prj_01_template_dualcore, aprovechando las conexiones ya establecidas con el AD2 (Analog Discovery 2).
>
> Al finalizar, genera un README.md limpio y conciso con la información técnica trascendental (diagrama/pines con AD2, guía de uso y configuración), manteniendo el mismo formato y calidad que los drivers ya terminados en el proyecto.

## Fuentes normativas

Por orden de precedencia:

1. Este encargo.
2. `01_shared_drivers/README.md`, reglas 1–4.
3. `DRIVER_ROADMAP.md` §5.3 (alcance de la primera iteración) y §6 (ciclo de trabajo, reglas R1–R4), con las desviaciones de la tabla siguiente.
4. `00_docs_stm32h7/DRIVER_ARCHITECTURE_GUIDELINES.md` y `00_docs_stm32h7/CLAUDE.md`.
5. `00_gpio/README.md` §8: el banco del AD2 y las reglas S1–S10, que este plan hereda sin cambios.
6. Silicio y herramientas:
   - RM0399 rev 4: cap. 9 (RCC: tabla 59, `RCC_CFGR.TIMPRE`, §9.5.10), §12.3.12 (GPIO en AF), cap. 18 (DMAMUX, tabla 126) y cap. 40–44 (timers).
   - ES0445 rev 6, §2.16.
   - WaveForms SDK Reference Manual (revisado el 4 de marzo de 2026): §8 Digital I/O y §10 Digital Out.
   - HAL del Cube `8ec3113` (STM32H7xx HAL V1.11.6).

## Desviaciones respecto a la hoja de ruta

| # | Hoja de ruta §5.3 | Este plan | Por qué |
| --- | --- | --- | --- |
| D1 | Criterio de salida: "`prj_04` usa el driver para TIM3; T12 igual que hoy" | `prj_04` no se toca. TIM3 y TIM7 siguen a mano en `prj_04` (`CM4/App/Src/adc_acquisition.c:66-81`, `adc_selftest_hw.c:134-154`, `dma_selftest.c:748,770`). Quedan como consumidores esperando en el README | Encargo: solo `prj_01` |
| D2 | Prueba "TRGO disparando el ADC con la tasa de `prj_04`" y su control "MMS mal programado (el ADC no convierte)" | TRGO se valida con un timer esclavo por ITR, y el oráculo es el `CNT` del esclavo (T10). TRGO → ADC queda como "no ejercitado" | `prj_01` no usa ADC |
| D3 | "Captura de entrada TIM2–5, 12–17" | Hay captura desde pin solo en TIM1 (PE9/PE11, desde el AD2) y en TIM17 por `TISEL` (MCO1). TIM2–5, TIM8, TIM12 y TIM15 capturan TRC, un disparo interno (T10d). La captura de TIM13, TIM14 y TIM16 queda como "no ejercitada" | El banco no tiene más pines de captura cableados (ver [Banco de pruebas](#banco-de-pruebas)) |
| D4 | — | Nuevas reglas de seguridad S11 y S12 para el generador de patrones del AD2 y los pines de captura | El SDK, §8: Digital Out conduce la línea cuando Digital-IO tiene enable y output a 0. El estado seguro S2 actual no lo para |

## Criterios de catálogo

Estos 15 criterios se auditan en cada fase. La columna Origen dice de qué fuente sale cada uno.

| # | Criterio | Origen |
| --- | --- | --- |
| C1 | **Sin pines ni instancias del proyecto.** El driver no configura GPIO, no incluye `board_def.h`, `shared_memory.h`, `gpio_*.h`, `dma_*.h` ni `hsem_*.h`, y no contiene frecuencias de reloj, prioridades de NVIC ni IDs de HSEM. En él solo viven hechos del silicio, en una tabla de capacidades por instancia: dirección (macros `TIMx` de CMSIS), bus y bit de enable en RCC, ancho del contador, canales, ITR válidos, valores de `TISEL`, IRQn por evento y peticiones de DMAMUX1 | README regla 1 |
| C2 | **API congelada al pasar su auditoría.** La API de `tim_bm` se congela al pasar F1, la de `tim_ll` al pasar F2 y la de `tim_hal` al pasar F3. Después, solo se añade | README regla 2 |
| C3 | Cada archivo lleva su sufijo de tier, los nombres son únicos y ningún `#include` lleva el prefijo numérico (`#include "tim_bm.h"`) | README reglas 3 y 4 |
| C4 | **Targets.** `stm32h7_driver_tim_bm` compila `tim_bm.c`. `stm32h7_driver_tim_ll` compila `tim_ll.c` y `stm32h7_driver_tim_hal` compila `tim_hal.c`, y los dos enlazan `_tim_bm`. No hay target sin sufijo, y el paraguas `stm32h7_shared_drivers` no los enlaza; ya incluye el directorio `03_tim` | Precedente de `10_hsem` (C4 de su README) |
| C5 | **Erratas.** Si una errata se puede aplicar en el driver, la aplica el driver. Si no, va al README con su ID (ver [Matriz RM0399 / ES0445](#matriz-rm0399--es0445)) | CLAUDE.md §F |
| C6 | **Validar antes de escribir.** Todo parámetro inválido, reservado o no soportado por la instancia devuelve error **antes del primer acceso**, también a RCC. El orden de los rechazos es fijo: `NULL`, instancia, canal, capacidad de la instancia, rango de campos y estado (`ERR_BUSY`) | C6 de `00_gpio` y `10_hsem` |
| C7 | **Sin estado en RAM.** `.data = 0` y `.bss = 0` en `tim_bm.c.obj`, `tim_ll.c.obj` y `tim_hal.c.obj`, medido en el `.map`. El `TIM_HandleTypeDef` lo pone el llamador | C7 de `10_hsem` |
| C8 | **Dependencias hacia abajo.** `tim_bm` usa solo `stm32h7xx.h`, `<stdint.h>` y `<stdbool.h>`. `tim_ll` añade solo funciones `static inline` de `stm32h7xx_ll_tim.h`: sin `USE_FULL_LL_DRIVER` y sin `LL_TIM_Init`. `tim_hal` añade `stm32h7xx_hal.h` y no compila nada sin `HAL_TIM_MODULE_ENABLED`. Ningún tier usa FreeRTOS | `00_gpio/README.md` §1 |
| C9 | **Evidencia.** `tools/build_all.ps1 -Clean` da 8/8. `prj_01` no lleva `-Werror` en sus flags (R2-5): sus imágenes, también `selftest-tim`, compilan con 0 avisos, y una compilación aparte con `CMAKE_C_FLAGS=-Werror` sale con 0. Cada captura lleva `Rev` e id de build limpios, y los tamaños salen del `.map` enlazado | Hoja de ruta §6.3, R2 y R3 |
| C10 | **Propiedad y concurrencia.** Cada instancia tiene un único núcleo dueño, y el driver no toma locks entre núcleos. En RCC solo escribe el banco del núcleo que llama (`RCC->APB1LENR` para TIM2–7 y 12–14, `RCC->APB2ENR` para TIM1, 8 y 15–17; RM0399 §9.5.10). Nunca escribe `RCC_APBxRSTR`, que es compartido, ni `RCC_CFGR` o `RCC_D2CFGR`, que solo lee. Dentro de un núcleo: `SR` se borra con una sola escritura (`SR = ~mask`, bits rc_w0), nunca con RMW. Los RMW de `DIER`, `CCER` y `BDTR` van con PRIMASK guardado y restaurado con `__set_PRIMASK(saved)`, sin `__enable_irq()` | RM0399 §9.5.10; T15 de `00_gpio` |
| C11 | **Sin glitch ni eventos espurios.** La configuración se hace con `CEN = 0`. El `UG` que carga PSC se hace con `URS = 1` temporal, como la HAL (`TIM_Base_SetConfig`), o borrando `UIF` justo después, así que tras la init `SR.UIF = 0`. Un canal se configura entero (modo, `CCR`, polaridad) antes de `CCxE`. `MOE` va el último. La deinit libera primero las salidas (`MOE = 0`, `CCER = 0`) | RM0399 §40.3.1; HAL V1.11.6 |
| C12 | **Honestidad.** Después de escribir hay read-back de cada registro de configuración: si no coincide, `ERR_VERIFY`. Una escritura que el hardware ignoraría se rechaza con `ERR_BUSY` sin escribir: `CCxS` con `CCxE = 1`, `TS` con `SMS ≠ 0` o `PSC`/modo con `CEN = 1`. La frecuencia que devuelve el cálculo es la real, no la pedida | C11 de `10_hsem`; RM0399 §40.4.3 (nota de `TS`) |
| C13 | **Esperas acotadas.** La única espera del driver (`TIM_BM_WaitFlags`) tiene un límite en ciclos DWT que pasa el llamador. Si `CYCCNT` no avanza, devuelve `ERR_TIMEOUT`. No hay ningún bucle sin salida | C13 de `00_gpio` |
| C14 | **El driver no define símbolos del proyecto:** ni `TIMx_IRQHandler`, ni `HAL_TIM_*Callback`, ni `HAL_TIM_*MspInit/MspDeInit`, ni `HAL_InitTick`/`HAL_SuspendTick`/`HAL_ResumeTick`. Tampoco llama a `NVIC_*`: `GetIRQn` solo devuelve el número | C14 de `00_gpio` |
| C15 | **Aritmética exacta y reproducible.** `TIM_BM_CalcTimeBase`, `TIM_BM_DutyToCompare` y `TIM_BM_KernelClockFromFields` son funciones puras: enteros de 64 bits, sin coma flotante, con las reglas de redondeo de [Reglas de cálculo](#reglas-de-cálculo). Un host independiente las reproduce bit a bit (T3) | Contrato de la hoja de ruta §5.3: "devuelve la frecuencia real" |

## Lo que existe hoy (`e7542dc`)

| Elemento | Estado |
| --- | --- |
| `01_shared_drivers/03_tim/` | Solo `.gitkeep`. Sin target en `CMakeLists.txt`; el paraguas ya incluye el directorio |
| `prj_01` CM7, `Core/Src/stm32h7xx_hal_timebase_tim.c` | **TIM6 es el tick de la HAL** (1 kHz, prioridad 0, `HAL_TIM_PeriodElapsedCallback` definida ahí). Calcula el reloj del timer como `PCLK1 × 2` si `APB1 ≠ /1`, que es la rama `TIMPRE = 0` de la tabla 59 |
| `prj_01` CM7, `App/Src/gpio_selftest.c:596-661` | TIM17 a mano, solo en la imagen `selftest-gpio` (ISR de T15 `f/g`), con `ARR = 9999` sobre 200 MHz escrito en el comentario |
| `prj_01` CM4 | Ningún timer. El tick de la HAL es SysTick, y `HAL_TIM_MODULE_ENABLED` está comentado en `CM4/Core/Inc/stm32h7xx_hal_conf.h:31` |
| Reloj de los timers en `prj_01` | HCLK1 = 200 MHz, `D2PPRE1 = D2PPRE2 = /2` y `TIMPRE = 0`. Por la tabla 59, `rcc_timx_ker_ck = rcc_timy_ker_ck = rcc_hclk1 = 200 MHz` en APB1 y APB2. Son valores nominales: el HSE real (MCO del ST-LINK) mide unos +1,29 %, así que los timers van a unos 202,6 MHz (R2-9), y ese valor varía cientos de ppm (R3-2) |

### Hallazgos sobre el código actual

| ID | Sev. | Dónde | Hallazgo | Acción |
| --- | --- | --- | --- | --- |
| TM1 | Baja | `prj_01 stm32h7xx_hal_timebase_tim.c:33-36` | Supone `TIMPRE = 0`. Con `TIMPRE = 1` y `D2PPRE1 = /4`, la tabla 59 da HCLK1 = 4 × PCLK1, y el tick saldría al doble. Con el árbol de `prj_01` el valor es correcto | Fuera de alcance (decisión abierta 4). Va al README del driver como ejemplo de lo que `TIM_BM_GetKernelClockHz` evita |
| TM2 | Baja | `prj_04` (D1) | TIM3/TIM7 programados a mano, con el reloj fijo en constantes | Fuera de alcance (D1). Consumidor esperando |

## Alcance

| Qué | Estado | Nota |
| --- | --- | --- |
| Base de tiempo: PSC/ARR, up, down y center 1–3 (donde existan), RCR, preload de ARR, one-pulse, URS | Dentro | |
| Cálculo de PSC/ARR con frecuencia real; duty → `CCR`; reloj de kernel por tabla 59 | Dentro | C15 |
| Interrupciones y flags: update, CC1–CC4, trigger | Dentro | Break y COM, fuera |
| Peticiones DMA: update y CC1–CC4 (`UDE`, `CCxDE`); tabla de DMAMUX1 | Dentro | El stream lo configura el llamador con `02_dma` |
| Output compare: frozen, active, inactive, toggle, force active/inactive, PWM1, PWM2; polaridad; preload de `CCR`; `MOE` | Dentro | Canales 1–4 |
| Captura de entrada: directa, indirecta, TRC; flanco; prescaler 1/2/4/8; filtro 0–15; `TISEL`; modo PWM input | Dentro | |
| Maestro/esclavo: `MMS` (TRGO) y `MSM`; `SMS` reset, gated, trigger, external clock 1 y reset + trigger combinado; `TS` = ITR0–ITR8, TI1F\_ED, TI1FP1 y TI2FP2 | Dentro | ETR, fuera |
| Deinit a valores de reset sin RCC reset | Dentro | C10 |
| Salidas complementarias, dead-time y break (TIM1/8/15/16/17) | Fuera | Hoja de ruta §5.3 |
| Encoder, Hall, XOR, PWM combinado o asimétrico, one-pulse redisparable, `OCREF_CLR`, ETR | Fuera | Hoja de ruta §5.3; errata 2.16.3/2.16.4 |
| Canales 5–6 y TRGO2 (`MMS2`) de TIM1/TIM8 | Fuera | Solo tienen sentido con ADC, que no está en `prj_01` (D2) |
| DMA burst (`DCR`/`DMAR`) | Fuera | Sin consumidor |
| LPTIM1–5 y HRTIM | Fuera | Hoja de ruta §5.3: carpeta aparte si aparece consumidor |
| TIM6 | Soportado por el driver, **nunca tocado por las pruebas** | Es el tick de la HAL de CM7 en `prj_01` |
| Migrar TIM6 de `prj_01` o TIM3/TIM7 de `prj_04` | Fuera | Encargo; decisión abierta 4 |

Una función que el alcance deja fuera no existe en la API. No se deja un parámetro "reservado para después".

## Archivos, dependencias y targets

```text
01_shared_drivers/03_tim/
├── IMPLEMENTATION_PLAN.md       # este archivo; se borra al cierre (queda en git)
├── IMPLEMENTER_INSTRUCTIONS.md  # reglas del implementador; se borra al cierre
├── IMPLEMENTATION_REPORT.md     # lo escribe SOLO el implementador
├── AUDIT.md                     # lo escribe SOLO el auditor
├── README.md                    # al cierre (ver "README de cierre")
├── tim_bm.h / tim_bm.c          # F1
├── tim_ll.h / tim_ll.c          # F2
└── tim_hal.h / tim_hal.c        # F3

03_firmware_templates/prj_01_template_dualcore/
├── CMakeLists.txt               # option(TIM_DRIVER_SELFTEST ...), incompatible con GPIO_DRIVER_SELFTEST
├── CMakePresets.json            # preset selftest-tim
├── CM7/CMakeLists.txt, CM4/CMakeLists.txt   # enlazan _tim_* y _dma_bm SOLO con la opción
├── CM7/App/Src/tim_selftest.c   # secuenciador, pruebas CM7, ISR de prueba
├── CM4/App/Src/tim_selftest.c   # pruebas CM4 (TIM15), servidor de comandos
├── Common/Inc/tim_selftest.h    # área SRAM4, comandos, resultados, tabla de pasos (versión)
├── Common/Src/tim_selftest_common.c
├── CM7/Core/Src/main.c, CM7/App/Src/app_main.c, CM4/Core/Src/main.c   # solo llamadas bajo #if
├── tools/tim_hw_check.py        # host del banco TIM
└── docs/tim_<fase>_<prueba>_<hash>.txt      # capturas

tools/ad2.py                     # se amplía: Digital Out y su guarda (S11); solo se añade
```

```cmake
add_library(stm32h7_driver_tim_bm INTERFACE)
target_include_directories(stm32h7_driver_tim_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/03_tim)
target_sources(stm32h7_driver_tim_bm INTERFACE ${CMAKE_CURRENT_LIST_DIR}/03_tim/tim_bm.c)

add_library(stm32h7_driver_tim_ll INTERFACE)
target_include_directories(stm32h7_driver_tim_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/03_tim)
target_sources(stm32h7_driver_tim_ll INTERFACE ${CMAKE_CURRENT_LIST_DIR}/03_tim/tim_ll.c)
target_link_libraries(stm32h7_driver_tim_ll INTERFACE stm32h7_driver_tim_bm)

add_library(stm32h7_driver_tim_hal INTERFACE)
target_include_directories(stm32h7_driver_tim_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/03_tim)
target_sources(stm32h7_driver_tim_hal INTERFACE ${CMAKE_CURRENT_LIST_DIR}/03_tim/tim_hal.c)
target_link_libraries(stm32h7_driver_tim_hal INTERFACE stm32h7_driver_tim_bm)
```

Los pines los configura el llamador con `gpio_bm` antes de habilitar la salida. Los streams de DMA, con `02_dma`. `tim_bm` no enlaza ninguno de los dos (C8).

## Contrato de API: tim\_bm (F1)

Todo lo que sigue es el contrato mínimo. El implementador puede añadir funciones `static` internas, pero cualquier función pública que no esté aquí necesita aprobación (instrucciones §7).

```c
/* ---- Status ------------------------------------------------------------------- */
typedef enum
{
    TIM_BM_OK = 0,
    TIM_BM_ERR_PARAM,        /*!< refused before any peripheral access (C6)               */
    TIM_BM_ERR_UNSUPPORTED,  /*!< the instance lacks this feature (no channel 3 on TIM12)  */
    TIM_BM_ERR_RANGE,        /*!< the request is not reachable with this PSC/ARR width     */
    TIM_BM_ERR_BUSY,         /*!< the hardware would ignore the write in this state (C12)  */
    TIM_BM_ERR_VERIFY,       /*!< read-back differs from what was written (C12)            */
    TIM_BM_ERR_TIMEOUT,      /*!< bounded wait expired, or CYCCNT not running (C13)        */
    TIM_BM_ERR_HAL           /*!< tim_hal only: a HAL call returned an error               */
} TimBm_Status_t;

/* ---- Capabilities (silicon facts, RM0399 ch. 40-44) --------------------------- */
typedef struct
{
    uint8_t  counter_bits;     /*!< 16 or 32 (TIM2, TIM5)                                   */
    uint8_t  channels;         /*!< 0 (TIM6/7), 1 (TIM13/14/16/17), 2 (TIM12/15), 4          */
    uint16_t itr_mask;         /*!< bit n = ITRn (0..8) is wired (tables 353, 358, 361, 366) */
    uint8_t  mms_max;          /*!< highest valid MMS: 2 (TIM6/7), 5 (TIM12/15), 7 (others) */
    bool     up_down_center;   /*!< TIM1-5, TIM8                                            */
    bool     repetition;       /*!< TIM1/8 (16-bit RCR), TIM15/16/17 (8-bit RCR)            */
    bool     bdtr;             /*!< MOE gate: TIM1, TIM8, TIM15, TIM16, TIM17               */
    bool     slave_mode;       /*!< SMCR: TIM1-5, TIM8, TIM12, TIM15                        */
    bool     master_mode;      /*!< MMS/TRGO: TIM1-8, TIM12, TIM15 (not 13/14/16/17)        */
    uint16_t tisel_mask[4];    /*!< bit v = TIxSEL value v is valid, per TI1..TI4           */
} TimBm_Caps_t;

bool           TIM_BM_IsValidInstance(const TIM_TypeDef *tim);
TimBm_Status_t TIM_BM_GetCaps(const TIM_TypeDef *tim, TimBm_Caps_t *out);

/* ---- Clock -------------------------------------------------------------------- */
TimBm_Status_t TIM_BM_EnableClock(TIM_TypeDef *tim);    /* this core's RCC bank, read back  */
TimBm_Status_t TIM_BM_DisableClock(TIM_TypeDef *tim);   /* this core's bank only (§9.5.10)  */
TimBm_Status_t TIM_BM_KernelClockFromFields(uint32_t hclk1_hz, uint32_t d2ppre, uint32_t timpre,
                                            uint32_t *out_hz);                 /* pure, table 59 */
TimBm_Status_t TIM_BM_GetKernelClockHz(const TIM_TypeDef *tim, uint32_t hclk1_hz,
                                       uint32_t *out_hz);   /* reads D2PPRE1/2 and TIMPRE only */

/* ---- Pure arithmetic (C15) ------------------------------------------------------ */
typedef struct
{
    uint32_t psc;
    uint32_t arr;
    uint64_t ticks;            /*!< (psc + 1) * (arr + 1)                                    */
    int32_t  error_ppm;        /*!< (clk / ticks - target) / target * 1e6, toward zero      */
} TimBm_TimeBase_t;

TimBm_Status_t TIM_BM_CalcTimeBase(uint32_t timer_clock_hz, uint32_t target_hz,
                                   uint32_t arr_max, TimBm_TimeBase_t *out);
TimBm_Status_t TIM_BM_DutyToCompare(uint32_t arr, uint32_t counter_bits,
                                    uint32_t duty_x100,            /* 0..10000 = 0.00..100.00 % */
                                    uint32_t *ccr);

/* ---- Time base --------------------------------------------------------------- */
#define TIM_BM_COUNT_UP       0U
#define TIM_BM_COUNT_DOWN     1U
#define TIM_BM_COUNT_CENTER1  2U
#define TIM_BM_COUNT_CENTER2  3U
#define TIM_BM_COUNT_CENTER3  4U

typedef struct
{
    TIM_TypeDef *tim;
    uint32_t     psc;          /*!< 0..65535                                                */
    uint32_t     arr;          /*!< 1..0xFFFF, or 1..0xFFFFFFFF on 32-bit counters           */
    uint32_t     count_mode;   /*!< TIM_BM_COUNT_*; non-UP needs caps.up_down_center         */
    uint32_t     repetition;   /*!< 0 if !caps.repetition; <= 0xFF on TIM15/16/17            */
    bool         arr_preload;  /*!< ARPE                                                     */
    bool         one_pulse;    /*!< OPM                                                      */
    bool         urs_overflow_only;  /*!< URS: UG and slave resets do not raise UIF          */
} TimBm_TimeBaseConfig_t;

TimBm_Status_t TIM_BM_InitTimeBase(const TimBm_TimeBaseConfig_t *cfg);   /* CEN = 0 required */
TimBm_Status_t TIM_BM_SetPeriod(TIM_TypeDef *tim, uint32_t arr);         /* live, preloaded  */
TimBm_Status_t TIM_BM_Start(TIM_TypeDef *tim);
TimBm_Status_t TIM_BM_Stop(TIM_TypeDef *tim);
TimBm_Status_t TIM_BM_Deinit(TIM_TypeDef *tim);   /* reset values, outputs first; no RCC write */
static inline uint32_t TIM_BM_GetCounter(const TIM_TypeDef *tim);

/* ---- Events, flags, interrupts, DMA requests ---------------------------------- */
#define TIM_BM_EV_UPDATE   (1UL << 0)   /* bit positions follow DIER/SR (RM0399 §40.4.4) */
#define TIM_BM_EV_CC1      (1UL << 1)
#define TIM_BM_EV_CC2      (1UL << 2)
#define TIM_BM_EV_CC3      (1UL << 3)
#define TIM_BM_EV_CC4      (1UL << 4)
#define TIM_BM_EV_TRIGGER  (1UL << 6)

TimBm_Status_t TIM_BM_EnableIrq(TIM_TypeDef *tim, uint32_t ev_mask);    /* C10: under PRIMASK */
TimBm_Status_t TIM_BM_DisableIrq(TIM_TypeDef *tim, uint32_t ev_mask);
TimBm_Status_t TIM_BM_EnableDmaRequest(TIM_TypeDef *tim, uint32_t ev_mask);   /* UPDATE, CC1-4 */
TimBm_Status_t TIM_BM_DisableDmaRequest(TIM_TypeDef *tim, uint32_t ev_mask);
uint32_t       TIM_BM_TakeFlags(TIM_TypeDef *tim, uint32_t ev_mask);   /* SR = ~taken, then DSB */
uint32_t       TIM_BM_TakeOvercapture(TIM_TypeDef *tim, uint32_t ch_mask);   /* CCxOF          */
TimBm_Status_t TIM_BM_WaitFlags(TIM_TypeDef *tim, uint32_t ev_mask, uint32_t max_cycles,
                                uint32_t *taken);                           /* C13          */
TimBm_Status_t TIM_BM_GenerateEvent(TIM_TypeDef *tim, uint32_t ev_mask);   /* EGR           */
TimBm_Status_t TIM_BM_GetIRQn(const TIM_TypeDef *tim, uint32_t ev, IRQn_Type *out);
TimBm_Status_t TIM_BM_GetDmaRequest(const TIM_TypeDef *tim, uint32_t ev, uint32_t *request_id);

/* ---- Output compare / PWM (channels 1..4) ------------------------------------- */
#define TIM_BM_OC_FROZEN          0U   /* OCxM values, RM0399 §40.4.7 */
#define TIM_BM_OC_ACTIVE          1U
#define TIM_BM_OC_INACTIVE        2U
#define TIM_BM_OC_TOGGLE          3U
#define TIM_BM_OC_FORCE_INACTIVE  4U
#define TIM_BM_OC_FORCE_ACTIVE    5U
#define TIM_BM_OC_PWM1            6U
#define TIM_BM_OC_PWM2            7U

typedef struct
{
    TIM_TypeDef *tim;
    uint32_t     channel;      /*!< 1..caps.channels                                        */
    uint32_t     mode;         /*!< TIM_BM_OC_*                                             */
    uint32_t     compare;      /*!< CCR, <= counter max                                     */
    bool         active_low;   /*!< CCxP                                                    */
    bool         preload;      /*!< OCxPE                                                   */
} TimBm_OcConfig_t;

TimBm_Status_t TIM_BM_ConfigOutput(const TimBm_OcConfig_t *cfg);   /* CCxE must be 0 (C12)  */
TimBm_Status_t TIM_BM_SetCompare(TIM_TypeDef *tim, uint32_t channel, uint32_t ccr);
static inline void TIM_BM_WriteCcr(TIM_TypeDef *tim, uint32_t channel, uint32_t ccr); /* unchecked */
TimBm_Status_t TIM_BM_OutputEnable(TIM_TypeDef *tim, uint32_t ch_mask);   /* CCxE, then MOE    */
TimBm_Status_t TIM_BM_OutputDisable(TIM_TypeDef *tim, uint32_t ch_mask);  /* MOE = 0 when none */

/* ---- Input capture ------------------------------------------------------------ */
#define TIM_BM_IC_DIRECT    1U   /* CCxS = 01 */
#define TIM_BM_IC_INDIRECT  2U   /* CCxS = 10 */
#define TIM_BM_IC_TRC       3U   /* CCxS = 11, needs caps.slave_mode */
#define TIM_BM_EDGE_RISING  0U
#define TIM_BM_EDGE_FALLING 1U
#define TIM_BM_EDGE_BOTH    3U

typedef struct
{
    TIM_TypeDef *tim;
    uint32_t     channel;      /*!< 1..caps.channels                                        */
    uint32_t     selection;    /*!< TIM_BM_IC_*                                             */
    uint32_t     edge;         /*!< TIM_BM_EDGE_*                                           */
    uint32_t     prescaler;    /*!< 1, 2, 4 or 8 events per capture                         */
    uint32_t     filter;       /*!< ICxF, 0..15                                             */
    uint32_t     tisel;        /*!< 0 = the CHx pin; else a value in caps.tisel_mask         */
} TimBm_IcConfig_t;

TimBm_Status_t TIM_BM_ConfigInput(const TimBm_IcConfig_t *cfg);    /* CCxE must be 0 (C12)  */
TimBm_Status_t TIM_BM_ConfigPwmInput(TIM_TypeDef *tim, uint32_t ti /* 1 or 2 */, uint32_t edge,
                                     uint32_t prescaler, uint32_t filter, uint32_t tisel);
TimBm_Status_t TIM_BM_CaptureEnable(TIM_TypeDef *tim, uint32_t ch_mask);
TimBm_Status_t TIM_BM_CaptureDisable(TIM_TypeDef *tim, uint32_t ch_mask);
static inline uint32_t TIM_BM_ReadCapture(const TIM_TypeDef *tim, uint32_t channel);   /* unchecked */

/* ---- Master / slave ----------------------------------------------------------- */
#define TIM_BM_MMS_RESET    0U   /* MMS values, RM0399 §40.4.2 */
#define TIM_BM_MMS_ENABLE   1U
#define TIM_BM_MMS_UPDATE   2U
#define TIM_BM_MMS_CMP_PULSE 3U
#define TIM_BM_MMS_OC1REF   4U
#define TIM_BM_MMS_OC2REF   5U
#define TIM_BM_MMS_OC3REF   6U
#define TIM_BM_MMS_OC4REF   7U

#define TIM_BM_SMS_DISABLED       0U   /* SMS values, RM0399 §40.4.3 */
#define TIM_BM_SMS_RESET          4U
#define TIM_BM_SMS_GATED          5U
#define TIM_BM_SMS_TRIGGER        6U
#define TIM_BM_SMS_EXT_CLOCK1     7U
#define TIM_BM_SMS_RESET_TRIGGER  8U

/* Trigger sources: driver values, NOT the TS[4:0] encoding (see the note below). */
#define TIM_BM_TRG_ITR(n)   (n)      /* n = 0..8, only where caps.itr_mask has bit n */
#define TIM_BM_TRG_TI1F_ED  16U
#define TIM_BM_TRG_TI1FP1   17U
#define TIM_BM_TRG_TI2FP2   18U

TimBm_Status_t TIM_BM_ConfigMaster(TIM_TypeDef *tim, uint32_t mms, bool msm);   /* mms <= caps.mms_max */
TimBm_Status_t TIM_BM_ConfigSlave(TIM_TypeDef *tim, uint32_t sms, uint32_t trigger);
```

> **Codificación de `TS`.** El driver traduce `TIM_BM_TRG_*` a `TS[4:0]` (bits 21, 20, 6, 5 y 4 de `SMCR`). `TS` mezcla ITR0–ITR3 (`00000`–`00011`), TI1F\_ED (`00100`), TI1FP1 (`00101`), TI2FP2 (`00110`), ETRF (`00111`) e ITR4–ITR8 (`01000`–`01100`, RM0399 §41.4.3). Por eso las constantes de la API no son la codificación. T2 comprueba la traducción contra esta lista, escrita a mano en la prueba. ETRF no tiene constante y se rechaza.

### Reglas de implementación (lo que audito línea a línea)

1. **Validación completa antes del primer acceso (C6).** Un rechazo no toca ni el reloj de RCC. Las comprobaciones de estado (`CEN`, `CCxE`, `SMS`) leen registros, pero no escriben nada.
2. **Tabla de capacidades.** Es una sola tabla `static const` indexada por instancia, con la sección del RM en un comentario por fila. Todas las validaciones por instancia salen de ella. Valores mínimos:

   | Instancia | Bits | Canales | Up/down/center | RCR | BDTR | Slave | `MMS` válidos | ITR válidos | `TISEL` ≠ 0 válidos |
   | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
   | TIM1, TIM8 | 16 | 4 | sí | 16 bits | sí | sí | 0–7 | 0–3 | ninguno (§40.4.30–31) |
   | TIM2 | 32 | 4 | sí | — | — | sí | 0–7 | 0–6 | TI4: 1–3 (COMP) |
   | TIM3 | 16 | 4 | sí | — | — | sí | 0–7 | 0–4 | TI1: 1–3 (COMP) |
   | TIM4 | 16 | 4 | sí | — | — | sí | 0–7 | 0–3 | ninguno |
   | TIM5 | 32 | 4 | sí | — | — | sí | 0–7 | 0–3, 6–8 | TI1: 1–2 (fdcan1) |
   | TIM6, TIM7 | 16 | 0 | solo up | — | — | — | 0–2 | — | — |
   | TIM12 | 16 | 2 | solo up | — | — | sí | 0–5 | 0–3 | TI1: 1 (spdifrx) |
   | TIM13, TIM14 | 16 | 1 | solo up | — | — | — | sin TRGO (no hay `CR2`) | — | ninguno |
   | TIM15 | 16 | 2 | solo up | 8 bits | sí | sí | 0–5 | 0–3 | TI1: 1–6; TI2: 1–3 |
   | TIM16 | 16 | 1 | solo up | 8 bits | sí | — | sin TRGO (`CR2` sin `MMS`) | — | TI1: 1–3 (LSI, LSE, WKUP\_IT) |
   | TIM17 | 16 | 1 | solo up | 8 bits | sí | — | sin TRGO (`CR2` sin `MMS`) | — | TI1: 2 (HSE\_1MHz), 3 (MCO1) |

   Fuentes: tablas 353, 358, 361 y 366 (ITR); `TIMx_CR2.MMS` de cada capítulo (§40.4.2, §41.4.2, §42.4.2, §43.5.2, §44.4.2); §40.4.30–31, §41.4.26–29, §42.4.15, §42.5.12–13, §43.5.20, §43.6.18 y §43.6.20 (`TISEL`). TIM17 `TI1SEL = 0001` no aparece en RM0399 rev 4, así que se trata como reservado. Si el implementador encuentra en el RM un dato distinto de esta tabla, manda el RM: lo apunta como errata del plan (instrucciones §5.7) y lo cita.
3. **`InitTimeBase` (C11, C12).** Exige `CEN = 0`. Si no, devuelve `ERR_BUSY`. Orden: `CR1` (modo, ARPE, OPM, URS) con `CEN = 0` → `PSC` → `ARR` → `RCR` → `UG` con `URS = 1` temporal → `CR1` final → `SR = 0` → read-back de `CR1`, `PSC`, `ARR` y `RCR`. Después, `SR.UIF = 0` (T12 lo comprueba).
4. **`ARR = 0` se rechaza.** Con `ARR = 0` el contador no cuenta (RM0399 §40.4.14).
5. **Salidas (C11).** `ConfigOutput` exige `CCxE = 0`. `OutputEnable` pone `CCxE` y, en las instancias con BDTR, `MOE` el último. `OutputDisable` borra `CCxE` y, si no queda ningún canal habilitado, `MOE`. `OSSI`, `OSSR` y `CCxNE` no se tocan nunca: quedan a 0, así que con `CCxE = 0` el pin no lo conduce el timer (tablas 354 y 359: Hi-Z).
6. **PWM 0 % y 100 %.** En PWM1 hacia arriba, `CCR = 0` da 0 % y `CCR > ARR` da 100 % (RM0399 §40.3.11). `DutyToCompare(10000)` devuelve `ARR + 1`. Por eso exige `ARR + 1 ≤` máximo del contador y, si no, `ERR_RANGE`.
7. **Entradas.** `ConfigInput` exige `CCxE = 0` (C12). `TISEL` solo acepta los valores de la tabla. `ConfigPwmInput` exige `caps.slave_mode` y dos canales: CC1 y CC2 sobre el mismo TI con flancos opuestos, y `SMS = reset` sobre TI1FP1 o TI2FP2 (RM0399 §40.3.8).
8. **Esclavo.** `ConfigSlave` sigue la nota de `TS` (RM0399 §40.4.3): `SMS = 0` → `TS` → `SMS`. Rechaza un ITR sin bit en la tabla, ETRF y los modos de encoder.
9. **Errata 2.16.1 (C5).** `ConfigMaster(msm = true)` sobre un timer con `OPM = 1` y `SMS = reset + trigger`, o `ConfigSlave(RESET_TRIGGER)` con `MSM = 1` y `OPM = 1`, devuelve `ERR_UNSUPPORTED` sin escribir.
10. **Flags.** `TakeFlags` lee `SR`, calcula `taken = SR & mask`, escribe `SR = ~taken` (una sola escritura), hace `__DSB()` y devuelve `taken`. El `DSB` es para que la línea de IRQ baje antes de salir de la ISR del M7, como `GPIO_BM_ExtiTakePending` en el plan de GPIO.
11. **Deinit (C10, C11).** Exige un timer válido. Orden: `BDTR.MOE = 0` → `CCER = 0` → `CR1.CEN = 0` → `DIER = 0` → los demás registros a su valor de reset (tabla del RM por instancia, también `AF1`/`AF2`/`TISEL`) → `SR = 0` → read-back de todos. No escribe en RCC.
12. **Sin literales de proyecto (C1).** `grep -nE "BOARD_|GPIO[A-K]|HSEM|DMA[12]_Stream|NVIC_|200000000|400000000" tim_*.[ch]` = 0.

### Reglas de cálculo

Son las reglas que T3 reproduce en el host. Si el implementador cambia una, cambia la API: hay que preguntar antes.

- **`KernelClockFromFields`** implementa las 16 filas de la tabla 59: `d2ppre` es el campo de 3 bits (`0xx`, `100`–`111`) y `timpre` es 0 o 1. Un `d2ppre > 7` o un `timpre > 1` da `ERR_PARAM`. `GetKernelClockHz` lee `D2PPRE1` para las instancias de APB1 y `D2PPRE2` para las de APB2, más `TIMPRE`, y llama a la función pura.
- **`CalcTimeBase`.** Rechaza con `ERR_PARAM` si `target_hz = 0`, si `target_hz > timer_clock_hz / 2` o si `arr_max = 0`. Después:
  1. `psc = ceil(clk / (target · (arr_max + 1))) − 1`, y como mínimo 0. Si `psc > 65535` → `ERR_RANGE`.
  2. `d = (psc + 1) · target` (64 bits) y `n = (clk + d/2) / d` (redondeo hacia arriba en el empate). Si `n > arr_max + 1`, entonces `n = arr_max + 1`. Si `n < 2` → `ERR_RANGE`.
  3. `arr = n − 1`, `ticks = (psc + 1) · n` y `error_ppm = (clk · 10⁶ / ticks − target · 10⁶) / target`, todo en 64 bits con signo y truncado hacia cero.

  Es decir, el prescaler más pequeño que cabe, que da la máxima resolución. No busca el error mínimo global: esa búsqueda se registra en T3 solo como información (decisión abierta 3).
- **`DutyToCompare`.** `ccr = ((arr + 1) · duty_x100 + 5000) / 10000`. Un `duty_x100 > 10000` → `ERR_PARAM`. Si `duty_x100 = 10000`, exige `arr + 1 ≤ 2^counter_bits − 1`; si no, `ERR_RANGE`.

## Contrato de API: tim\_ll (F2) y tim\_hal (F3)

**`tim_ll`.** Mismas operaciones que `tim_bm` (`TIM_LL_*`), con constantes LL (`LL_TIM_COUNTERMODE_*`, `LL_TIM_OCMODE_*`, `LL_TIM_CHANNEL_CH1..4`, `LL_TIM_IC_POLARITY_*`, `LL_TIM_TS_*`). Reglas:

- Traduce las constantes LL a valores BM y valida **llamando a `tim_bm`**: una sola copia de la validación y de la tabla de capacidades. Una constante LL desconocida → `TIM_BM_ERR_PARAM`.
- Escribe con funciones `static inline` de `stm32h7xx_ll_tim.h` y respeta las mismas reglas 3–11 y el mismo orden.
- Devuelve `TimBm_Status_t`.
- **Paridad:** para la misma configuración, la imagen de registros es idéntica bit a bit a la de BM (T2).

**`tim_hal`.** Envoltorio fino de la HAL para código que ya usa `TIM_HandleTypeDef`. Funciones mínimas:

```c
TimBm_Status_t TIM_HAL_InitTimeBase(TIM_HandleTypeDef *h, const TimBm_TimeBaseConfig_t *cfg);
TimBm_Status_t TIM_HAL_ConfigOutput(TIM_HandleTypeDef *h, const TimBm_OcConfig_t *cfg);
TimBm_Status_t TIM_HAL_StartPwm(TIM_HandleTypeDef *h, uint32_t channel);
TimBm_Status_t TIM_HAL_StopPwm(TIM_HandleTypeDef *h, uint32_t channel);
TimBm_Status_t TIM_HAL_ConfigInput(TIM_HandleTypeDef *h, const TimBm_IcConfig_t *cfg);
TimBm_Status_t TIM_HAL_StartCapture(TIM_HandleTypeDef *h, uint32_t channel);
TimBm_Status_t TIM_HAL_ConfigMaster(TIM_HandleTypeDef *h, uint32_t mms, bool msm);
TimBm_Status_t TIM_HAL_ConfigSlave(TIM_HandleTypeDef *h, uint32_t sms, uint32_t ts);
TimBm_Status_t TIM_HAL_Deinit(TIM_HandleTypeDef *h);
```

Reglas:

- Valida con `tim_bm` **antes** de llamar a la HAL (C6). `h == NULL`, `h->Instance` que no es un timer, o `h->Instance ≠ cfg->tim` → `ERR_PARAM`.
- Después de cada llamada a la HAL hace read-back de los registros que esa llamada escribe (C12). Si la HAL devuelve un error → `ERR_HAL`. Si devuelve `HAL_OK` pero el read-back no coincide → `ERR_VERIFY`.
- No define callbacks ni MSP (C14). La HAL llama a `HAL_TIM_*_MspInit`, que es del proyecto.
- `TIM_HAL_Deinit` no llama a `HAL_TIM_Base_DeInit` (la HAL llamaría a la MSP del proyecto); llama a `TIM_BM_Deinit` y pone `h->State = HAL_TIM_STATE_RESET`.
- **Paridad:** para la misma configuración, la imagen que deja `tim_hal` es la de BM, salvo los bits que la HAL escribe distinto por diseño. Cada uno de esos bits se lista en el README con su línea de `stm32h7xx_hal_tim.c` (T2).

## Matriz RM0399 / ES0445

| Regla | Mecanismo | Prueba |
| --- | --- | --- |
| Tabla 59: el reloj del timer depende de `D2PPREx` y `TIMPRE` | `KernelClockFromFields`, `GetKernelClockHz`; el llamador pasa `timer_clock_hz` | T3, T5 |
| §9.5.10: `PERxEN` por núcleo; el periférico tiene reloj si lo habilita CPU1 **o** CPU2 | `Enable/DisableClock` solo en el banco del que llama; README | T16 |
| `RCC_APBxRSTR` es compartido (offset < `0xD0`) | Nunca se escribe; la deinit usa los valores de reset | Revisión (A9) |
| §40.4.14: `ARR = 0` bloquea el contador | `ERR_PARAM` | T1 |
| §40.3.1: `PSC` es preload; `UG` lo carga y levanta `UIF` | `UG` con `URS = 1` temporal; `SR = 0` | T12(b) |
| §40.4.3: `TS` solo se cambia con `SMS = 000` | Secuencia de la regla 8 | T2, T10 |
| §40.4.7: `CCxS` solo se escribe con el canal apagado (`CCxE = 0`) | `ERR_BUSY` | T1 |
| Tablas 354 y 359: con `CCxE = 0` (y `MOE = 0`, `OSSI = 0`) la salida está en Hi-Z | Reglas 5 y 11; S12 | T6 (control), T0b(g) |
| §40.3.11: 0 % con `CCR = 0`, 100 % con `CCR > ARR` | `DutyToCompare` | T3, T6 |
| §40.3.8 / §43.4.7: modo PWM input | `ConfigPwmInput` | T8 |
| Tablas 353, 358, 361 y 366: ITR por instancia | Tabla de capacidades | T1, T10 |
| `TISEL` por instancia | Tabla de capacidades | T1, T9 |
| Tabla 126: peticiones de DMAMUX1 por timer | `GetDmaRequest` | T13 |
| §12.3.12: en AF, los pulls de `PUPDR` siguen activos y `IDR` se muestrea | Base de S12 y de T7 | T0b(g), T7 |
| ES0445 2.16.1: one-pulse no ve el disparo con `MSM = 1` en reset + trigger | Combinación rechazada (regla 9); README recomienda `MSM = 0` | T1 |
| ES0445 2.16.2: compare consecutivo perdido (toggle, centro) | README; sin workaround | Revisión |
| ES0445 2.16.3 / 2.16.4: `ocref_clr` | `OCREF_CLR` fuera de la API; README | Revisión |

## Banco de pruebas

Es el banco de `00_gpio/README.md` §8, **sin cambiar ni un cable**: placa NUCLEO-H755ZI-Q (MB1363, Rev V) y AD2 SN `210321B5D38D`, con la GUI de WaveForms cerrada. Las reglas S1–S10 siguen vigentes tal como están allí. Este plan añade S11 y S12, y decide qué hace cada línea en las pruebas de TIM.

### Uso de las líneas en las pruebas de TIM

Funciones alternativas de STM32H755_Alternate_Functions.md (DS12923 tabla 9).

| AD2 | MCU | Función TIM disponible | Uso en las pruebas de TIM | Quién conduce |
| --- | --- | --- | --- | --- |
| DIO1 | PE9 | TIM1\_CH1 (AF1) | Salida PWM/OPM (T6, T11, T13a) en un paso "bus = MCU". Entrada de captura (T8, T13b) en un paso "bus = AD2" | MCU **o** AD2, según el paso (S5, S11, S12) |
| DIO3 | PE11 | TIM1\_CH2 (AF1) | Igual que PE9 | Igual |
| DIO5 | PE13 | TIM1\_CH3 (AF1) | Salida PWM (T6) en "bus = MCU" | Bus |
| DIO6 | PE14 | TIM1\_CH4 (AF1) | Salida PWM (T6) en "bus = MCU" | Bus |
| DIO0, 2, 4, 7 | PE8, PE10, PE12, PE15 | TIM1\_CH1N/CH2N/CH3N, TIM1\_BKIN | **Nunca en AF.** En entrada, con los pulls del procedimiento S5. Salida GPIO solo en T0b(c), como en `00_gpio` (R2-8) | Bus |
| DIO9 | PE5 | TIM15\_CH1 (AF4) | Salida PWM de CM4 (T6) | Solo MCU |
| DIO11 | PE6 | TIM15\_CH2 (AF4) | Salida PWM de CM4 (T6) | Solo MCU |
| DIO14 | PA8 | MCO1 (AF0), TIM1\_CH1 (AF1) | MCO1 = HSE/8 (TREF\_A, TREF\_B, T9; referencia de T6 y T8, R3-2) | Solo MCU |
| DIO8 | PE3 | TIM15\_BKIN (AF4) | **Nunca en AF.** Entrada | Solo AD2 (no la conduce en este banco) |
| DIO10 | PE4 | TIM15\_CH1N (AF4) | **Nunca en AF:** en AF4 el MCU la conduciría contra una línea "solo AD2". Entrada | Solo AD2 (no la conduce en este banco) |
| DIO12 | PE2 | — | `HOST_REQ` | Solo AD2 |
| DIO13 | PE7 | TIM1\_ETR (AF1) | `MCU_ACK`; nunca en AF | Solo MCU |
| Scope 1+/2+ | PF14/PF15 | — | No se usan; en entrada (S2) | — |

Sin cablear (nadie los conduce desde fuera; en las pruebas llevan pull-down para que el control con el canal apagado sea determinista):

| Pin | Función | Uso |
| --- | --- | --- |
| PF6 | TIM16\_CH1 (AF1) | T7 |
| PF7 | TIM17\_CH1 (AF1) | T7; control de T9 (`TI1SEL = 0`) |
| PF8 | TIM13\_CH1 (AF9) | T7 |
| PF9 | TIM14\_CH1 (AF9) | T7 |

**Nunca se tocan:** PE0, PE1 (LD2), PC13, TIM6 (tick de la HAL de CM7), SysTick, LPTIM, HRTIM, `RCC_D2CFGR`, `TIMPRE`, `RCC_APBxRSTR` y `DBGMCU`. `RCC_CFGR` solo se escribe en TREF\_A, TREF\_B, T9 y los pasos de T6 y T8 (campos `MCO1SEL`/`MCO1PRE`, R3-2), se guarda antes y se restaura con read-back después. RM0399 avisa de que cambiar MCO1 en marcha puede dar glitches en MCO1: solo afectan a PA8 (una entrada del AD2) y a la entrada de TIM17, así que el primer milisegundo después del cambio se descarta.

### Reglas nuevas S11 y S12

| # | Regla | Cómo se aplica | Quién lo comprueba |
| --- | --- | --- | --- |
| S11 | **Digital Out es otro conductor.** El SDK, §8: "Digital-IO has priority over Digital-Out. The Digital-Out is only applied for a signal when the Digital-IO Enable and Output are zero". Con `OE = 0` (el S2 actual), un canal de Digital Out habilitado **sí conduce**. Por eso: (a) toda llamada a `FDwfDigitalOutEnableSet` pasa por una única función, `ad2_set_dout(mask, step_bus_owner)`, que lanza una excepción, después de ir a S2, si la máscara tiene algún bit fuera de {DIO1, DIO3} o si el paso no es "bus = AD2". (b) S2 amplía: `FDwfDigitalOutReset`, `FDwfDigitalOutConfigure(0)` y, para los 16 canales, `EnableSet(0)` e `IdleSet(DwfDigitalOutIdleZet)`, con read-back de `EnableGet` = 0 y de `IdleGet` = Zet. (c) Todo canal habilitado lleva `IdleSet(Zet)` y salida push-pull **antes** de `Configure(1)`. `Idle = Low` solo se acepta en el control de T0b(f), ligado a su paso (R2-4). (d) Mientras corre Digital Out, el `OE` de Digital-IO es 0 en los bits de los canales de Digital Out, con read-back. HOST\_REQ puede seguir habilitado (R2-3) | `tools/ad2.py`; ningún otro camino a `FDwfDigitalOut*Set` que habilite o configure | grep (A27) + T0b(e)(f) |
| S12 | **Pin de captura en AF mientras el AD2 conduce.** En un paso "bus = AD2", `bench_claim()` solo acepta AF en PE9 y PE11, en AF1 y como entrada de TIM1. Antes de aceptarlo, hace read-back de TIM1: `CC1S`/`CC2S` ≠ 00 (entrada), `CCER.CCxNE = 0` en todos los canales, `CCER.CC3E = CC4E = 0`, y `BDTR.MOE = OSSI = OSSR = 0`. Si algo no cuadra, rechaza sin escribir. La prueba eléctrica S5 se hace **con los pines ya en esa configuración AF**: los pulls de `PUPDR` siguen activos en AF (RM0399 §12.3.12), así que si el MCU condujera, la línea no seguiría a los dos pulls | `CM7/App/Src/tim_selftest.c`; ningún otro camino a `MODER`/`AFR` de un pin cableado | grep (A27) + T0b(g) |

Las rutas de firmware de S4 se amplían igual: `bench_claim()` existe en los dos núcleos, porque CM4 configura PE5/PE6 (TIM15). Rechaza siempre AF en PE0–PE4, PE7, PE8, PE10, PE12 y PE15. `bench_safe()` (S2 del MCU) pone en entrada **todos** los pines cableados, también PE5/PE6, que configura CM4. Las escrituras GPIO de los dos núcleos pasan por el lock entre núcleos de `prj_01` (`Shared_GpioLockInstall`).

### Herramientas

| Archivo | Contenido |
| --- | --- |
| `tools/ad2.py` | Se añaden Digital Out (pulso por canal con divisor y contadores, `FDwfDigitalOutInternalClockInfo`), la guarda S11 y la ampliación de S2. Se añade una medida de frecuencia y de tiempo en alto sobre la captura LA que ya existe. Las funciones existentes no cambian de firma ni de comportamiento, salvo el S2 ampliado. `gpio_hw_check.py` tiene que seguir pasando su T0b(a) con el mock |
| `prj_01/tools/tim_hw_check.py` | Igual que `gpio_hw_check.py`: S2 en `try/finally`, banner `TIM_BENCH v1 idle` (S8), tabla de pasos con versión (S7), comprobación de build id (R3-a/R3-b de `00_gpio`), `[host] argv:`/`[host] options:`, una captura por ejecución con marcas de tiempo hasta `HOST VERDICT=` |

### Selftest

- Preset `selftest-tim` (`TIM_DRIVER_SELFTEST=ON`, Debug). La opción y `GPIO_DRIVER_SELFTEST` son incompatibles: `FATAL_ERROR` en CMake si están las dos.
- Con la opción apagada, las imágenes `debug`/`release` de `prj_01` tienen el mismo `.text`, `.data` y `.bss` que en `2481396` (A16, R4-2).
- **Área SRAM4:** `0x3800B000`, 4 KB, dentro de `RAM_SHARED` en los dos linker scripts y de la región MPU 7 (no cacheable en CM7). No se solapa con `g_shared` (`0x38008000`) ni con el área de `00_gpio` (`0x3800A000`). CPU1 la pone a cero y abre una sesión nueva antes de `HSEM_BM_BootReleaseCpu2`. CPU2 solo actúa sobre comandos de esa sesión.
- **Propiedad de las instancias** (C10): CM7 tiene TIM1, TIM2, TIM3, TIM4, TIM5, TIM7, TIM8, TIM12, TIM13, TIM14, TIM16 y TIM17. CM4 tiene TIM15. En T10d, TIM15 (CM4) captura el TRGO de TIM1 (CM7): la señal cruza por hardware, y cada núcleo solo escribe su timer. En T16, CM4 llama a `Enable/DisableClock(TIM2)`: solo escribe su banco de RCC.
- CM7 corre en `Status_Task`, como el selftest de GPIO. CM4 corre después de `Shared_GpioLockInstall()` y `CPU2_PublishBuild()`. Las ISR de prueba viven en `tim_selftest.c` bajo la opción, no llaman a FreeRTOS y tienen prioridad 6. `stm32h7xx_it.c` no gana ningún handler.
- Cada núcleo imprime primero `Rev <FW_GIT_REV>` e id de build. **El veredicto global lo imprime CM7** cuando CM4 ha terminado, e incluye los resultados de CM4. Las pruebas con AD2 pasan solo si también pasan sus líneas `[ad2]` en la misma captura.
- Cada prueba deja su timer con `Deinit` y comprueba la imagen de reset (T14). La aplicación sigue igual después del selftest.

## Fases

Todo va en la rama `driver/tim`. Cada fase termina en commits del estilo del repo: `build(tim)`, `feat(tim)`, `fix(tim)`, `test(prj_01)`, `feat(tools)`, `docs(tim)`. Una preocupación por commit, el porqué en el cuerpo y el ID del hallazgo entre paréntesis.

| Fase | Entregable | Gate (lo comprueba el auditor) |
| --- | --- | --- |
| **F0: banco** | Sin código del driver ni targets: `_tim_bm` entra en F1, `_tim_ll` en F2 y `_tim_hal` en F3, cada uno con su fuente. Opción, preset y esqueleto del selftest (Rev, id, veredicto, área SRAM4, tabla de pasos). `ad2.py` con Digital Out, S11 y S2 ampliado; `tim_hw_check.py` con S2–S8 y S11–S12 **desde el primer commit**. **T0a, T0b(a)–(g), TREF\_A y TREF\_B** (protocolo v2, R3-3) | 8/8 y C9 (R2-5). Producción de `prj_01` con los mismos tamaños que `2481396` (R4-2). **T0b en verde en la ejecución del auditor antes de cualquier prueba que mueva el bus o use Digital Out.** TREF dentro del criterio de R4-1. `git diff e7542dc -- 02_firmware_projects` vacío |
| **F1: `tim_bm`** | Contrato completo de `tim_bm`. Pruebas BM en los dos núcleos: T1–T16 y B1 | Protocolo A1–A28 en verde. **API de `tim_bm` congelada** |
| **F2: `tim_ll`** | Contrato de `tim_ll`. T1, T2, T5, T6, T8, T10a/b, T12 y T14 con LL en los dos núcleos | A1–A28. Paridad LL = BM bit a bit (T2). `git diff <cierre F1> -- 03_tim/tim_bm.h`: solo añade. **API de `tim_ll` congelada** |
| **F3: `tim_hal`** | Contrato de `tim_hal`. T1, T2, T5, T6, T8, T9 y T14 con HAL en CM7 | A1–A28. Paridad HAL contra BM con las diferencias por diseño listadas. TIM6 sigue dando el tick (`HAL_GetTick` avanza 1 ms por ms en la captura). **API de `tim_hal` congelada** |
| **Cierre** | A petición del auditor, después de que pase F3. **Implementador:** `03_tim/README.md` (ver [README de cierre](#readme-de-cierre)) y la línea de `03_tim` en `01_shared_drivers/README.md`. **Responsable del catálogo, no el implementador:** `DRIVER_ROADMAP.md`, borrado de los documentos de proceso, merge y borrado de la rama | Nada documentado sin código ni prueba detrás. Auditoría de cierre en `AUDIT.md` |

## Plan de pruebas

Cada prueba tiene un **control negativo** (R1) y un **oráculo que no es el driver**. Si el control no falla como se espera, la prueba no cuenta y hay que rediseñarla. "Tier" dice con qué tiers se ejecuta en F1 (BM), F2 (LL) y F3 (HAL).

**Tolerancias comunes.** Reloj de los timers: 200 MHz nominales (unos 202,6 MHz reales, R2-9), y un tick son unos 5 ns. Los criterios se expresan en ticks del timer o en cocientes con MCO1 medido en la misma adquisición (R3-2); ninguno depende del valor nominal ni de un TREF tomado en otro momento. El LA del AD2 muestrea a 100 MS/s (10 ns), y el tamaño de buffer que usa cada captura se lee de `FDwfDigitalInBufferSizeInfo` y se registra. **Frecuencia en el AD2:** ±0,1 % sobre el cociente con MCO1 de la misma adquisición (R3-2). En todo punto que mide el AD2, `PSC + 1 ≤ 200` y `ARR + 1 ≤ 200`, así que un error de una cuenta en cualquiera de los dos (≥ 0,5 %) supera la tolerancia al menos 5 veces. **Tiempo en alto en el AD2:** ±15 ns sobre la media de todos los periodos completos de la ventana (1 muestra del LA + 1 tick). Ningún punto pide medir un pulso de menos de 50 ns.

| ID | Fase / tier | Qué | Oráculo | Control negativo | Criterio |
| --- | --- | --- | --- | --- | --- |
| T0a | F0 y al empezar cada sesión | Las 13 líneas que no son del handshake: sueltas (el MCU pone pull-up y luego pull-down) y walking-1 (una línea con pull-up y las demás con pull-down), siguiendo S1–S6 sin que ningún lado conduzca. HOST\_REQ y MCU\_ACK quedan cubiertos por cada handshake (R2-6) | DIO del AD2 y `IDR` del MCU, cada uno comparando la palabra entera | El firmware pone DIO9 (PE5) con pull-down donde el host espera pull-up: los dos lados tienen que marcar exactamente ese bit | Las 13 líneas siguen cada patrón en los dos lados; el control marca solo DIO9 |
| T0b | F0 | (a)–(d) como en `00_gpio`. **(e)** `ad2_set_dout` con un bit fuera de {DIO1, DIO3}, o en un paso que no es "bus = AD2" → excepción y AD2 en S2 **antes** de llamar a la DLL (mock). **(f)** Liberación real de Digital Out: DIO1 con Digital Out a 1 MHz; `Configure(0)` con `Idle = Zet`; el MCU, con PE9 en entrada, pone pull-up y luego pull-down, y el host tiene que leer 1 y 0. **(g)** `bench_claim(PE9, AF1)` en un paso "bus = AD2" con TIM1 `CC1S = 00` (salida), o con `MOE = 1`, o con `OSSI = 1`: rechazo sin escribir. Con la configuración S12 correcta: aceptado, y la prueba S5 en AF pasa | (a)(e) mock de la DLL; (f) DIO del AD2; (g) fotos de `MODER`/`AFR` y de los registros de TIM1 | (f) **`Idle = Low`** con el MCU en entrada con pull-up: la línea no sigue al pull, y la función de comprobación tiene que abortar. No hay contienda: el MCU no conduce. Tiene que ser la misma función que protege toda captura real (S5). (g) la configuración correcta sí se acepta, así que la guarda distingue | (a)(b)(e)(g) rechazo sin escritura; (c) `ABORT … contention check`; (d) pines en entrada; (f) aborta con `Idle = Low` y pasa con `Idle = Zet` |
| TREF\_A, TREF\_B | F0 y cada captura: TREF\_A después de T0a (R2-7), TREF\_B como último paso antes de `DONE` (R3-1) | MCO1 = HSE/8 en PA8 (AF0); el AD2 mide su frecuencia en 8 capturas por grupo (≥ 40 periodos cada una, separadas ≥ 100 ms) → eᵢ y rᵢ; ē y s de las 16. Verificación cruzada x: ajuste de las medias de las ráfagas `TICK` (una por paso, más el banner y el veredicto) frente al reloj del host; u es el error estándar de la pendiente (R2-10, R3-1) | DIO14 del AD2; reloj del host | Solo en TREF\_A: PA8 en AF1 (TIM1\_CH1) con TIM1 sin reloj en los dos bancos y MCO1 todavía en HSE/8, sin flancos (R2-1) | \|ē\| ≤ 20 000 ppm y \|ē − x\| ≤ 3·√(u² + s\_ef²/16 + σ\_ser²), con s\_ef = max(s, r/√6) (R3-1) y σ\_ser = 145 ppm (R4-1). Se registra también σ\_ser. Se registran cada eᵢ, ē\_A, ē\_B, ē, s, x, u y la pendiente de cada ráfaga. `RCC_CFGR` restaurado con read-back en cada grupo |
| T1 | F1 BM, F2 LL, F3 HAL | Rechazos: `NULL`; puntero que no es un timer (`(TIM_TypeDef *)USART3`, `TIM1 + 4`); canal 0, 5 y 3 en TIM12; PWM en TIM7; `ARR = 0`; `ARR > 0xFFFF` en un timer de 16 bits; `PSC > 65535`; `repetition > 0xFF` en TIM15; `count_mode` center en TIM12; ITR5 en TIM4; ETRF; `TISEL` reservado (TIM17 = 1); `MSM` + reset/trigger + OPM (2.16.1); `ConfigOutput`/`ConfigInput` con `CCxE = 1` y `InitTimeBase` con `CEN = 1` (`ERR_BUSY`); `duty > 10000`; 100 % con `ARR = 0xFFFF` en 16 bits (`ERR_RANGE`) | Foto de todos los registros de la instancia y de `RCC_C1`/`RCC_C2` `APB1LENR`/`APB1HENR`/`APB2ENR`, antes y después | Una llamada válida sí cambia la foto | Estado exacto por caso y fotos idénticas en los rechazos |
| T2 | F1 BM, F2 LL, F3 HAL | Imagen de registros: matriz por instancia (todas salvo TIM6) de base de tiempo, OC (los 8 modos × 2 polaridades × preload), IC (3 selecciones × 3 flancos × 4 prescalers × filtros 0/7/15 × `TISEL` válidos), maestro y esclavo (todos los ITR válidos) | Tabla de campos del RM escrita a mano en la prueba (los dos núcleos). En CM7, además, la imagen que deja la HAL cruda (`HAL_TIM_*` sin el driver). LL y HAL: también contra la imagen de BM | Una imagen esperada alterada a propósito se detecta | Bits idénticos. Las diferencias de la HAL por diseño, listadas una por una |
| T3 | F1 | Funciones puras: ≥ 200 vectores de `CalcTimeBase` (los límites: `target = clk/2`, el que da `psc = 65535`, uno justo por debajo → `ERR_RANGE`, divisores exactos, primos, `arr_max` de 16 y 32 bits), ≥ 50 de `DutyToCompare` y las 16 filas de la tabla 59 | El host reimplementa las [reglas de cálculo](#reglas-de-cálculo) en Python con `fractions` y compara psc, arr, ticks, `error_ppm` y estado. Registra aparte la búsqueda de error mínimo global, solo como información | El host altera a propósito el valor esperado de un vector: tiene que marcar fallo | Coincidencia exacta en todos los vectores |
| T5 | F1 BM (los dos núcleos), F2 LL, F3 HAL | Base de tiempo frente a DWT en cada instancia salvo TIM6: (a) periodos de ≤ 1000 ticks, ventana ≥ 100 ms; (b) RCR en TIM1/8/15/16/17: un update cada RCR+1 periodos; (c) 32 bits en TIM2/TIM5: `ARR + 1 = 0x20000` (si el contador fuera de 16 bits, el periodo sería la mitad); (d) down y center en TIM1 y TIM2: center da un periodo de 2·ARR ticks; (e) `GetKernelClockHz` = 200 000 000 en todas las instancias | `CYCCNT` del núcleo: 2 ciclos por tick en CM7 (400 MHz) y 1 en CM4 (200 MHz), con el mismo PLL. Duración del primer al último update en la ventana | La misma medida con `PSC + 1`: fuera de tolerancia | \|error\| ≤ 100 ppm frente a `ticks` (un error de una cuenta en (a) es ≥ 1000 ppm). (c) dentro de tolerancia con el periodo de 32 bits |
| T6 | F1 BM, F2 LL, F3 HAL (TIM1); F1 BM y F2 LL en CM4 (TIM15) | PWM en el AD2: TIM1 CH1–CH4 en PE9/PE11/PE13/PE14 (paso "bus = MCU") y TIM15 CH1–CH2 en PE5/PE6. Puntos: 1 MHz (`PSC = 0`, `ARR = 199`) al 50 %; 100 kHz (`PSC = 9`, `ARR = 199`) con `CCR` = 1 (50 ns), 0, 25, 50 y 100 %; `active_low`; PWM2; center 1 en TIM1 (periodo de 2·ARR ticks) | LA del AD2 a 100 MS/s, con DIO14 (MCO1 = HSE/8, PA8 en AF0) en la misma adquisición: frecuencia como cociente con MCO1 (R3-2) | TIM1 con `MOE = 0` (`OSSI = 0`) y TIM15 con `CCxE = 0`: el pin no lo conduce el timer y sigue al pull-down de `PUPDR` → nivel constante. Ninguna transición en los canales que no se configuraron | Frecuencia y tiempo en alto dentro de las tolerancias comunes; 0 %: sin flanco de subida, nivel bajo; 100 %: sin flanco de bajada, nivel alto; solo el canal configurado conmuta |
| T7 | F1 BM | PWM en pines sin cablear: TIM13 (PF8), TIM14 (PF9), TIM16 (PF6) y TIM17 (PF7) a 1 kHz al 25 % | `IDR` muestreado en bucle con PRIMASK y marcas de `CYCCNT`, ≥ 10 periodos | `CCxE = 0` con pull-down: `IDR` constante a 0 | Periodo ±0,5 % y duty ±0,5 puntos |
| T8 | F1 BM, F2 LL, F3 HAL | Captura desde el AD2 (Digital Out en DIO1/DIO3, paso "bus = AD2", S5 + S11 + S12): modo PWM input en TI1 (PE9) y TI2 (PE11), a 1 kHz, 10 kHz, 100 kHz y 1 MHz, con duty de 25, 50 y 75 %; prescaler 8 a 1 MHz; filtro: Digital Out en modo Custom a 100 MHz con un glitch de una muestra (10 ns) por periodo, en el tamaño de buffer que devuelve `FDwfDigitalOutDataInfo` (sin filtro se captura; con `ICxF = 0011`, fCK\_INT y N = 8, es decir 40 ns, no); overcapture | El divisor y los contadores programados en Digital Out (reloj de `FDwfDigitalOutInternalClockInfo`), con MCO1 en DIO14 capturado por el LA durante la misma generación (R3-2) | Polaridad invertida: duty complementario. `CC1E = 0`: `CC1IF` nunca se levanta. Sin filtro: el glitch sí se captura | Periodo en ticks = 200·f\_MCO1,medida / f ± (1 tick + 0,1 %) (R3-2); tiempo en alto ± 2 ticks; `CC1OF = 1` cuando no se lee `CCR1` dos eventos seguidos |
| T9 | F1 BM, F2 LL, F3 HAL | `TISEL`: TIM17 `TI1SEL = 0011` (MCO1) con MCO1 = HSE/8 y prescaler de entrada 1 → periodo de 200 ticks | El AD2 mide MCO1 en PA8 a la vez | `TI1SEL = 0000` (PF7 sin cablear, pull-down): ninguna captura | Periodo de 200 ± 1 ticks, con la frecuencia del AD2 dentro de la tolerancia común. `RCC_CFGR` restaurado |
| T10 | F1 BM; (a) y (b) también F2 LL | Maestro/esclavo interno. (a) External clock 1: TIM3 (`MMS = update`) → TIM2 (ITR2), 1000 updates. (b) Gated: TIM2 (`MMS = OC1REF`, PWM) → TIM4 (ITR1, reloj interno): tras N periodos, `CNT = N·CCR`. (c) Trigger: TIM1 TRGO → TIM8 (ITR0), TIM8 arranca solo. (d) Captura de TRC: TIM2, TIM3, TIM4, TIM5, TIM8, TIM12 y TIM15 (CM4) capturan el TRGO de un maestro con ITR válido (TIM1 para todos salvo TIM12, que usa TIM4 por ITR0); la diferencia entre capturas = periodo del maestro. (e) OC1REF de TIM13 y TIM14 → TIM12 (ITR2/ITR3) en gated: `CNT = N·CCR` | `CNT` y `CCRx` del esclavo, leídos directamente. El periodo del maestro ya está validado por T5 | `TS` de un ITR cuyo maestro está parado: cuenta 0, no arranca, ninguna captura | (a) cuenta = 1000 exactos; (b)(e) \|`CNT − N·CCR`\| ≤ N; (c) `CEN` de TIM8 = 0 antes y 1 después del primer update de TIM1; (d) diferencia = periodo ± 1 tick |
| T11 | F1 BM | One-pulse en TIM1 CH1 (PE9, "bus = MCU"): PWM2, retardo D y anchura W; 10 disparos por software | LA del AD2 | `OPM = 0`: pulsos periódicos (> 1 en la ventana) | Exactamente 1 pulso por disparo; D y W ± (1 tick + 1 muestra) |
| T12 | F1 BM (los dos núcleos); F2 LL | Interrupciones: (a) update a 10 kHz durante 100 ms en cada vector: `TIM1_UP`, `TIM1_CC` (CC1), `TIM2`–`TIM5`, `TIM7`, `TIM8_UP_TIM13` (TIM8 y TIM13, distinguidos con `TakeFlags`), `TIM8_BRK_TIM12` (TIM12), `TIM8_TRG_COM_TIM14` (TIM14), `TIM8_CC`, `TIM15` (CM4), `TIM16` y `TIM17`. (b) Sin IRQ espuria: `EnableIrq(UPDATE)` justo después de `InitTimeBase` y sin `Start` → 0 IRQ en 10 ms. (c) `GetIRQn` contra una tabla escrita a mano desde `stm32h755xx.h` | Contadores de la ISR de prueba y `CYCCNT` | `DIER.UIE = 0`: 0 IRQ. (b) sin `URS` temporal y sin borrar `UIF` (inicialización a mano): 1 IRQ espuria | (a) 1000 ± 1 por vector, sin cruces entre TIM8 y TIM12/13/14; (b) 0; (c) igual a la tabla |
| T13 | F1 BM | DMA, con `dma_bm` y el buffer en `.dma_buf`. (a) TIM1 UP (petición 15) → DMA1 escribe en `TIM1->CCR1` una secuencia de 16 valores; PWM en PE9 ("bus = MCU"). (b) TIM1 CC1 (petición 11) → 64 capturas a memoria de una señal del AD2 en PE9 ("bus = AD2"). (c) `GetDmaRequest` de todas las instancias y eventos contra la tabla 126 | (a) LA del AD2: 16 periodos con los tiempos en alto de la secuencia. (b) Las diferencias = periodo del Digital Out. (c) Los `DMA_REQUEST_TIM*` de `stm32h7xx_hal_dma.h` | (a) `UDE = 0`: todos los periodos iguales. (b) `CC1DE = 0`: el buffer conserva su centinela | (a) secuencia exacta (± 15 ns por periodo); (b) 64/64 diferencias = periodo ± 1 tick; (c) igual a la tabla |
| T14 | F1 BM, F2 LL, F3 HAL | Deinit: después de cada prueba, todos los registros con su valor de reset y, en los pines cableados, la prueba S5 de pulls pasa | Tabla de valores de reset del RM escrita a mano | Un timer que no se desinicializa a propósito se detecta | Imagen de reset exacta; S5 en verde |
| T15 | F1 BM | Concurrencia en un núcleo: el hilo hace `Enable/DisableIrq(TIM2, CC2)` 10⁶ veces mientras una ISR de TIM7 a 50 kHz hace `Enable/DisableIrq(TIM2, CC3)` | Contadores privados y una foto final de `DIER` | El mismo bucle con RMW a mano sin PRIMASK: actualizaciones perdidas > 0 | 0 pérdidas, con solapamiento demostrado (la ISR se ejecutó dentro de la ventana de RMW del hilo al menos una vez en el control) |
| T16 | F1 BM | Bancos de RCC: CM7 habilita TIM2 y lo deja contando; CM4 llama a `DisableClock(TIM2)` → TIM2 sigue contando. CM4 llama a `EnableClock(TIM2)` y luego a `DisableClock(TIM2)` → TIM2 sigue contando. CM7 llama a `DisableClock(TIM2)` → TIM2 se para | `CNT` de TIM2 leído por CM7, y `RCC_C1->APB1LENR`/`RCC_C2->APB1LENR` | El paso final (CM7 deshabilita) sí lo para: el oráculo distingue | Cada paso como se describe; los registros de banco cambian solo en el banco del que llama |
| B1 | F1, F2, F3 | Medida: `.text`, `.data` y `.bss` de cada `tim_*.c.obj` en CM4 y CM7 (`.map`); ciclos de `TakeFlags`, `OutputEnable`, `ConfigOutput` e `InitTimeBase`; peor caso de `CalcTimeBase` en el barrido de T3 | `.map` y DWT | — | Solo registro. `.data = .bss = 0` (C7) |

### No ejercitado por diseño (va al README)

- TRGO → ADC/DAC (D2). TRGO2, canales 5–6.
- Captura desde pin en TIM2–5, TIM8, TIM12, TIM15, TIM16, TIM13 y TIM14 (D3). TIM2–5, 8, 12 y 15 capturan TRC en T10d. TIM13, TIM14 y TIM16 no capturan nada.
- Salida PWM en pin de TIM2–5, TIM8 y TIM12: sin pin en el banco. Su `OCxREF` sí se valida por TRGO + gated (T10b), salvo TIM12, cuyo TRGO no llega a ningún ITR.
- `tim_hal` en CM4 (HAL TIM deshabilitado en `CM4/Core/Inc/stm32h7xx_hal_conf.h`).
- Revisiones de silicio distintas de V. Otros árboles de reloj distintos del de `prj_01`, salvo por la función pura (T3).

## Protocolo de auditoría

### Qué entrega el implementador al cerrar una fase

1. El rango de commits en `driver/tim`, con el árbol limpio. La rama existe como rama, no como un fast-forward sobre `main`.
2. La salida de `tools/build_all.ps1 -Clean` (8/8), la de la compilación de `selftest-tim` (0 avisos) y la de la compilación aparte con `CMAKE_C_FLAGS=-Werror` (C9, R2-5).
3. Tamaños del `.map` enlazado (B1), sin contar "Discarded input sections", y la comparación de la producción de `prj_01` con `2481396` (R4-2).
4. La captura de `tim_hw_check.py`: una ejecución, sin editar, con marcas de tiempo, `Rev` igual al commit y sin `-dirty` en los dos núcleos, id de build igual al del host, TREF\_A al principio, TREF\_B al final y `HOST VERDICT=` después (R3-1).
5. La lista de controles negativos de la fase con el valor que dio cada uno.
6. `IMPLEMENTATION_REPORT.md` con la sección de la fase, según la plantilla de las instrucciones (§5).

### Qué compruebo y cómo

| # | Punto | Cómo | Pasa si |
| --- | --- | --- | --- |
| A1 | Reglas 1–4 del README y checklist §6 de las guías | Lectura del diff | Sin excepciones |
| A2 | C1: sin literales de proyecto | `grep -nE "BOARD_\|GPIO[A-K]\|HSEM\|DMA[12]_Stream\|NVIC_\|board_def\|shared_memory\|gpio_\|dma_" 03_tim/tim_*.[ch]` | 0, salvo `IRQn_Type` y los nombres de la tabla de capacidades |
| A3 | C2: API congelada | `git diff <cierre de la fase anterior> -- 03_tim/tim_*.h` | Solo añade o comenta |
| A4 | C3: nombres e includes | `git grep -n "03_tim/"` en código C | 0 |
| A5 | C4: targets | `CMakeLists.txt` | Los tres targets, LL/HAL enlazan BM, sin target sin sufijo, paraguas igual |
| A6 | C6: validación antes del acceso | Lectura de cada función pública + T1 con fotos | Ningún camino toca RCC o TIM antes de validar |
| A7 | C7: estado | `.map` de CM4 y CM7 | `.data = .bss = 0` en los tres `.obj` |
| A8 | C8: dependencias | `grep -n "#include" 03_tim/tim_*.[ch]`; un `.c` que solo incluye `tim_bm.h` compilado sin HAL | Solo lo permitido; compila |
| A9 | C10: RCC y concurrencia | `grep -nE "RSTR\|RCC->CFGR\s*=\|D2CFGR\s*=\|__enable_irq\|->SR\s*&=\|->SR\s*\|=" 03_tim/` y lectura de los RMW de `DIER`/`CCER`/`BDTR` | 0 coincidencias; cada RMW con PRIMASK guardado y restaurado |
| A10 | C11: orden y `UIF` | Lectura + T12(b) | Orden de las reglas 3, 5 y 11 |
| A11 | C12: honestidad | Lectura + T1 (`ERR_BUSY`) | Read-back en cada escritura de configuración |
| A12 | C13: esperas | Lectura de cada bucle | Todos acotados por el llamador |
| A13 | C14: símbolos | `grep -nE "IRQHandler\|HAL_TIM_[A-Za-z]*Callback\|MspInit\|MspDeInit\|HAL_InitTick\|NVIC_" 03_tim/` | 0 |
| A14 | C15: cálculo | T3 ejecutado por mí | Coincidencia exacta |
| A15 | Tabla de capacidades | Comparo cada fila con el RM | Sin diferencias, o diferencias justificadas en el informe |
| A16 | C9: build y producción intacta | `build_all -Clean` y comparación de tamaños de `prj_01` debug/release con `2481396` (R4-2) | 8/8; tamaños iguales |
| A17 | R3: tamaños | Del `.map` | Coinciden con la entrega |
| A18 | Pruebas de la fase | Flasheo `selftest-tim` y ejecuto **yo** `tim_hw_check.py`, en frío y sin depurador | Mis resultados coinciden con los de la entrega, con el mismo `Rev` |
| A19 | R1: controles | Cada control imprime su valor y falla como se espera | Un control que no falla invalida su prueba |
| A20 | Oráculo independiente | Lectura de las pruebas y del script | Ninguna prueba compara el driver consigo mismo |
| A21 | R2: trazabilidad | Cada captura | `Rev` e id limpios, marcas de tiempo, TREF\_A y TREF\_B (R3-1) |
| A22 | R4: no destructivas | Lectura + ejecución | TIM6, SysTick, PE0/PE1/PC13, `D2CFGR`, `TIMPRE`, `RSTR` y `DBGMCU` intactos; `RCC_CFGR` restaurado; `HAL_GetTick` sigue avanzando; la aplicación sigue igual |
| A23 | Alcance (encargo) | `git diff e7542dc --stat` | Nada fuera de `01_shared_drivers/03_tim`, `01_shared_drivers/CMakeLists.txt`, `03_firmware_templates/prj_01_template_dualcore` y `tools/ad2.py`. Al cierre, además, la línea de `01_shared_drivers/README.md` |
| A24 | Paridad (F2, F3) | T2 | LL = BM; HAL = BM salvo diferencias listadas |
| A25 | Erratas | Lectura + T1 | 2.16.1 rechazada; 2.16.2–2.16.4 en el README |
| A26 | Documentación | README y comentarios contra el código | Nada prometido que no exista; lo no ejercitado, escrito |
| **A27** | **Seguridad eléctrica S1–S12** (en todas las fases y **antes** de conectar el AD2 a la imagen) | `grep -n "FDwfDigitalIOOutputEnableSet\|FDwfDigitalOutEnableSet\|FDwfDigitalOutIdleSet\|FDwfDigitalOutConfigure" tools/ 03_firmware_templates/prj_01_template_dualcore/tools/`: cada uno solo dentro de su guarda o de S2. `grep -nE "MODER\|OTYPER\|AFR" C*/App/Src/tim_selftest.c`: todo pasa por `bench_claim`. Las máscaras coinciden con la tabla de líneas. `DwfParamOnClose = 1`. T0b(a)–(g) en verde en **mi** ejecución | Todo se cumple. **Cualquier incumplimiento es bloqueante,** aunque todas las demás pruebas pasen |
| A28 | Informe | `IMPLEMENTATION_REPORT.md` contra el código y las capturas | Cada número con su fuente; sin lenguaje de veredicto |

### Clasificación de lo que encuentre

| Clase | Significado | Efecto |
| --- | --- | --- |
| Bloqueante (B) | Incumple un criterio C, una regla R o S, o un gate; una prueba no se reproduce en mi ejecución; evidencia editada o sin hash | La fase no pasa |
| Mayor (M) | Defecto real que no invalida la fase | Se corrige antes de la fase siguiente |
| Menor (m) | Robustez, estilo o documentación | Puede ir al cierre, apuntado en `AUDIT.md` |
| Nota | Observación sin acción | Solo registro |

Reglas del registro: `AUDIT.md` lo escribe solo el auditor, con un apartado por fase. Un hallazgo se cierra cuando lo verifica el auditor. Las capturas no se editan. Un resumen escrito a mano no es evidencia.

## README de cierre

Lo escribe el implementador **solo cuando el auditor lo pida**, después de que pase F3. En inglés, como los demás README del catálogo, con la estructura de `00_gpio/README.md` y la concisión de `02_dma/README.md`: **como mucho 600 líneas**. Secciones:

1. Cabecera: qué cubre, estado (revisión validada, placa, fecha, `Rev` de las capturas) y enlace a la validación.
2. **Tiers:** tabla de tiers, targets, dependencias y uso típico; tabla tier × núcleo; matriz de funciones BM/LL/HAL.
3. **API summary:** estados y funciones por tier, con una línea cada una.
4. **Configuration guide:** cómo obtener `timer_clock_hz` (tabla 59, `GetKernelClockHz`); cómo elegir PSC/ARR con `CalcTimeBase`; orden de arranque de una salida PWM (reloj → base de tiempo → canal → pin con `gpio_bm` → `OutputEnable` → `Start`); captura; TRGO/esclavo con la tabla de ITR; DMA con `GetDmaRequest` y `02_dma`; IRQ con `GetIRQn` y `TakeFlags`.
5. **Project contract:** propiedad de cada instancia por núcleo, bancos de RCC (§9.5.10), TIM6 como tick de la HAL en `prj_01`, sin callbacks ni MSP en el driver.
6. **Rules the driver enforces:** C6, C10–C13 en forma de lista.
7. **Silicon facts and errata:** tabla de capacidades, 2.16.1–2.16.4 y TM1.
8. **Examples:** PWM BM en CM4; captura PWM input LL; base de tiempo HAL en CM7; maestro/esclavo.
9. **Test bench (Analog Discovery 2):** diagrama ASCII y tabla de líneas usadas por las pruebas de TIM (sección [Banco de pruebas](#banco-de-pruebas)), S1–S12 resumidas, procedimiento.
10. **Validation:** tabla de pruebas con resultado y captura de origen; cómo reproducirlo (comandos); no ejercitado.
11. **Known limitations**, **Consumers** (`prj_01` selftest; `prj_04` TIM3/TIM7 esperando, D1), **Changing this driver**, **History** (hashes de los documentos de proceso borrados).

## Riesgos

| Riesgo | Mitigación |
| --- | --- |
| **Digital Out conduce con `OE = 0`** (SDK §8) y el S2 actual no lo para | S11: guarda única, S2 ampliado con read-back, `Idle = Zet`, T0b(e)(f) antes de cualquier uso |
| **Contienda en PE9/PE11** si TIM1 condujera mientras el AD2 genera | S12: read-back de TIM1 + prueba eléctrica S5 con los pines ya en AF; `CCxNE`/`MOE`/`OSSI` a 0 |
| PE4 en AF4 (TIM15\_CH1N) conduciría una línea "solo AD2" | `bench_claim` lo rechaza siempre; T0b(b) |
| Romper el tick de la HAL (TIM6) | TIM6 nunca se pasa a una función que escribe; A22 comprueba que `HAL_GetTick` avanza |
| Que la HAL de TIM6 y la de `tim_hal` choquen por `HAL_TIM_PeriodElapsedCallback` | `tim_hal` no define callbacks (C14); la prueba HAL usa la ISR del selftest para contar |
| Jitter de FreeRTOS y de TIM6 (prioridad 0) en las medidas por DWT | Medida del primer al último evento en ventanas ≥ 100 ms; tolerancia de 100 ppm frente a un error de una cuenta ≥ 1000 ppm |
| Precisión del reloj del AD2 frente al de la placa (en esta placa, +1,29 %: R2-9) | Medidas del AD2 como cociente con MCO1 en la misma adquisición (R3-2); TREF\_A/TREF\_B en cada captura, validados contra una segunda referencia (R3-1); tolerancia de 0,1 % y puntos con ≤ 200 ticks |
| Cambiar MCO1 en marcha (aviso del RM) | Solo en TREF\_A, TREF\_B, T9 y al empezar los pasos de T6/T8; se descarta el primer ms; `RCC_CFGR` guardado y restaurado |
| Tocar `prj_04` u otros proyectos | A23 con `git diff --stat`; el encargo lo prohíbe |
| Que el plan tenga un dato del RM equivocado | Manda el RM; el implementador lo apunta como errata del plan con la cita (instrucciones §5.7) |

## Decisiones abiertas

Cada una trae la opción por defecto que asume este plan.

1. **Retorno:** `TimBm_Status_t` en los tres tiers (recomendado: distingue `BUSY` de `PARAM`, lección G15 de `00_gpio`). Alternativa: `bool`.
2. **Alcance de la primera iteración:** el de la tabla [Alcance](#alcance) (recomendado: el de la hoja de ruta §5.3, menos lo que no se puede validar en `prj_01`). Alternativa: añadir salidas complementarias y dead-time en TIM1, que el banco sí podría medir en PE8/PE10/PE12.
3. **Regla de `CalcTimeBase`:** el PSC más pequeño que cabe (recomendado: máxima resolución y predecible). Alternativa: error mínimo global, más lento y con empates más difíciles de especificar.
4. **TIM6 de `prj_01` (TM1):** no se migra al driver (recomendado: es producción y el encargo pide validar el driver, no cambiar la plantilla). Alternativa: migrar `HAL_InitTick` a `TIM_BM_GetKernelClockHz` en un commit propio.
5. **`tim_hal` en CM4:** no se ejercita (recomendado). Alternativa: habilitar `HAL_TIM_MODULE_ENABLED` en CM4 solo con la opción del selftest.
6. **Documentos de proceso al cierre:** se borran el plan, las instrucciones, el informe y `AUDIT.md`, como en `00_gpio` y `10_hsem` (hoja de ruta §6.6). Quedan recuperables con `git show`.
