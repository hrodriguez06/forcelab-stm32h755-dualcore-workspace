# Plan de implementación de `00_gpio` F2: EXTI 0–15 y celda de compensación

Oct 15, 2026 · @Hector · revisión 1

Añade a `gpio_bm` las líneas EXTI 0–15 del núcleo que llama y la celda de compensación de I/O, con el contrato que fijó el plan original de GPIO y que guarda `00_gpio/README.md` §13.3. Se valida en `prj_01_template_dualcore`, preset `selftest-gpio`, con el banco del AD2 de F1 sin recablear.

Es una **ampliación** del driver. Las firmas existentes de `gpio_bm.h` no cambian; solo se añaden (catálogo C2). Los tiers LL y HAL no cambian: `gpio_hal` sigue rechazando los modos IT/EVT (G6 en HAL queda aplazado).

Una sola fase de implementación (F1 de este plan) y un cierre. La fase pasa una auditoría independiente con ejecución propia del auditor en la placa. Las reglas para el implementador están en [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md).

Base y control: `main` en `3df160a` (cierre de `03_tim` F4). El código de `00_gpio` no cambia desde F1c.

**El desarrollador es Antigravity** (autorizado por el responsable el 2026-10-15). No tiene acceso a la placa ni al AD2: todo lo que los necesita (T11–T14, T17 y las dos ejecuciones completas del banco) lo ejecuta el auditor con las herramientas del implementador. El implementador entrega el firmware, las herramientas y las pruebas sin placa (T1 de compilación, T18).

**Decisiones del responsable (2026-10-15): aprobadas las cuatro, con la opción A en 1–3.** Banco verificado antes de la fase: `gpio_f2_bench_baseline_4f94c0f.txt` en `prj_01/docs/` (38/38, `FW VERDICT=PASS`).

**Decisiones del plan** (ver [Decisiones abiertas](#decisiones-abiertas)):
1. Implementador Antigravity; banco y AD2, solo el auditor.
2. Pulsos en DIO8/DIO10 con escrituras estáticas del AD2, sin tocar la guarda S11 del generador de patrones.
3. Celda de compensación: API y T17; los proyectos siguen con `HAL_EnableCompensationCell()`.
4. Documentos de proceso: se borran al cierre.

**Reglas de verificación (heredadas de `00_gpio` F1b/F1c y `03_tim` F4):**
- Cada comprobación puede fallar de verdad y cuenta en el veredicto.
- Cada prueba lleva un control negativo real y un oráculo que no es el driver.
- Cada captura de placa prueba qué imagen corre: `Rev` y build id de los dos núcleos, como ya exige `gpio_hw_check.py` (R3-a, R3-b, R4-a).
- Árbol limpio; un aviso, un archivo o un preset que falta es FAIL, nunca "sin hallazgos".
- Capturas UTF-8 de una sola ejecución, sin edición manual.
- Seguridad eléctrica S1–S11 del banco (`00_gpio/README.md` §8), sin relajar ninguna.

---

## Encargo

| ID | Hoy (`3df160a`) | Qué cambia |
| --- | --- | --- |
| **H1** | `gpio_bm` no tiene EXTI ni SYSCFG. `GPIO_BM_ERR_CONFLICT` y `GPIO_BM_ERR_NOT_READY` existen en `gpio_bm.h:120-121`, reservados y nunca devueltos (README §2.1) | Las seis funciones y las tres macros del contrato (Diseño 1), en `gpio_bm.c`, solo CMSIS |
| **H2** | `prj_01` `selftest-gpio`: T0–T10, T15, T16, TB, B1. PE3/PE4 cableados y "sin usar"; PE6 sin uso; ningún handler EXTI en `prj_01` | Pruebas T11–T14 y T17 en CM7 y CM4 (Diseño 3), con sus ISR en `gpio_selftest.c` bajo la opción, sus pasos en `k_steps[]` y sus comandos de CM4 en `GstCmd_t` |
| **H3** | `prj_01/tools/gpio_hw_check.py` no conduce DIO8/DIO10 | Los pasos de T11–T14 en el host: pulsos estáticos en DIO8/DIO10, captura con el LA de DIO8–DIO11, cuentas y latencias, con el mismo `STEPS`/`Session.step()` |
| **H4** | — | `tools/gpio_f2_checks.py` (nuevo): T1 de compilación y T18 (regresión sin placa), con controles negativos |
| **H5** | — | `IMPLEMENTATION_REPORT.md` |

**Fuera de este plan:**
- **H5c de `10_hsem` en `prj_04`** (`CM7/Core/Src/main.c:114-137`): línea 15 a mano, con un evento hacia el otro núcleo. La API solo configura las máscaras del núcleo que llama; H5c sigue a mano. Ninguna prueba usa la línea 15.
- Eventos hacia el otro núcleo, líneas 16 en adelante, `HSLV`, `CCCR`/`CS` (código de compensación a mano).
- `gpio_ll`, `gpio_hal` (G6 en HAL), los proyectos `prj_02`–`prj_04` y `prj_eth_lwip_node`.
- La migración de `HAL_EnableCompensationCell()` en los `SystemClock_Config` (Decisión 3).

---

## Fuentes normativas

- `01_shared_drivers/00_gpio/README.md`:
  - §2 (estado y API), §3 (contrato del proyecto y hook);
  - §8 (banco, cableado y S1–S11);
  - §9 (validación y reproducción);
  - §10 (limitaciones: EXTI y la celda de compensación, faltan);
  - §13.3 (contrato de F2);
  - §14 (criterios C1–C14 y lista antes del merge).
- RM0399:
  - cap. 21 (EXTI): líneas 0–15 configurables (tabla 152); `RTSR1` 0x00, `FTSR1` 0x04, `SWIER1` 0x08 compartidos; `C1IMR1`/`C1EMR1`/`C1PR1` 0x80–0x88 y `C2IMR1`/`C2EMR1`/`C2PR1` 0xC0–0xC8 por núcleo; `CnPR1` solo se pone con la interrupción de ese núcleo desenmascarada, y se borra escribiendo 1;
  - cap. 13 (SYSCFG): `EXTICR1`–`EXTICR4` (0x08–0x14, cuatro campos de 4 bits, 0000 = PA … 1010 = PK); `CCCSR` 0x20 (`EN` bit 0, `CS` bit 1, `READY` bit 8, que no sube sin CSI; `HSLV` bit 16, destructivo por encima de 2,7 V);
  - §12.3.11 (celda de compensación) y §9.7.1 (`APB4ENR`, con `SYSCFGEN`, tiene una copia por núcleo).
- CMSIS `stm32h755xx.h`: `EXTI->IMR1/EMR1/PR1` (CPU1) y `EXTI->C2IMR1/C2EMR1/C2PR1` (CPU2); vistas `EXTI_D1`/`EXTI_D2`.
- `00_docs_stm32h7/docs/peripherals/interrupts_exti/README.md` §7 (limpiar pending antes de habilitar; `__DSB()` al final de la ISR del M7).
- ES0445: ninguna errata de EXTI 0–15, SYSCFG ni la celda de compensación (`errata_summary.md`).

---

## Lo que existe hoy (`3df160a`)

| Elemento | Estado |
| --- | --- |
| Sección de `gpio_bm` | `GPIO_BM_SectionEnter/Exit` (`gpio_bm.c:219-297`): PRIMASK, y el hook entre núcleos (HSEM 5, 60 µs) solo en la profundidad 1; `ERR_BUSY` si el hook falla |
| Núcleo que llama | El driver no mira `CORE_CM4` hoy; solo `DUAL_CORE` en `IsPortClocked`. Los `CMakeLists.txt` de `prj_01` definen `CORE_CM7`/`CORE_CM4` |
| RAM del driver (C7) | `.data = 0`, `.bss = 8 B` (`s_shared_lock`, `s_section_depth`) |
| Selftest | Bloque SRAM4 `0x3800A000` (`GstArea_t`, 4 KB como máximo; los campos nuevos van al final). Comandos de CM4 en `GstCmd_t`. Pasos con el host en `k_steps[]` (CM7) y `STEPS` (host), con el handshake de 4 fases por PE2/PE7 |
| Pines del banco para F2 | PE3 ← DIO8 y PE4 ← DIO10 (solo AD2); PE5 → DIO9 (solo MCU; hoy T8); PE6 → DIO11 (solo MCU; sin uso). `bench_claim()` rechaza siempre PE0–PE4 y PE7 |
| AD2 | `ad2_set_oe()` ya permite DIO8, DIO10 y DIO12 como salidas estáticas. El generador de patrones (`ad2_set_dout`, guarda S11) solo puede usar DIO1/DIO3 |
| Celda de compensación | `prj_01`–`prj_04` y `prj_eth_lwip_node` la encienden en `SystemClock_Config` con `__HAL_RCC_CSI_ENABLE()`, `__HAL_RCC_SYSCFG_CLK_ENABLE()` y `HAL_EnableCompensationCell()`, que pone `EN` sin esperar `READY`. En `prj_01`, CSI no tiene otro uso |
| EXTI en proyectos | Solo H5c de `prj_04` (línea 15). Nadie escribe `EXTICR` ni llama a `HAL_GPIO_EXTI*` |

---

## Diseño

### 1. API (añadida a `gpio_bm.h`)

```c
#define GPIO_BM_EXTI_RISING   1U
#define GPIO_BM_EXTI_FALLING  2U
#define GPIO_BM_EXTI_BOTH     3U

GpioBm_Status_t GPIO_BM_ExtiConfig(GPIO_TypeDef *port, uint32_t pin_idx, uint32_t trigger,
                                   bool interrupt, bool event);        /* calling core's masks only */
GpioBm_Status_t GPIO_BM_ExtiDisable(GPIO_TypeDef *port, uint32_t pin_idx);
uint32_t        GPIO_BM_ExtiTakePending(uint32_t line_mask);          /* this core's PR1, then DSB */
GpioBm_Status_t GPIO_BM_ExtiSoftwareTrigger(uint32_t pin_idx);
IRQn_Type       GPIO_BM_ExtiGetIRQn(uint32_t pin_idx);
GpioBm_Status_t GPIO_BM_EnableCompensationCell(uint32_t max_cycles);  /* needs CSI; never writes RCC_CR or HSLV */
```

Las firmas son las de README §13.3, sin cambios.

### 2. Reglas de implementación (lo que audito línea a línea)

1. **Núcleo que llama.**
   - Con `CORE_CM4` se usan `C2IMR1`/`C2EMR1`/`C2PR1`; si no, `IMR1`/`EMR1`/`PR1` (CPU1). Es el mismo criterio que la HAL (`stm32h7xx_hal_gpio.c:172-175`).
   - `SYSCFGEN` se habilita en el banco de `RCC` del núcleo que llama y se lee de vuelta, como `EnablePortClock`.
2. **Validación antes de escribir (C6):**
   - `ERR_PARAM` si el puerto no es válido (`GPIO_BM_IsValidPort`), si `pin_idx > 15`, si `trigger` no es 1–3, o si `interrupt` y `event` son los dos `false`;
   - `GPIO_BM_ExtiSoftwareTrigger(pin_idx > 15)` y `GPIO_BM_EnableCompensationCell(0)` también devuelven `ERR_PARAM`;
   - `GPIO_BM_ExtiGetIRQn(pin_idx > 15)` devuelve `GPIO_BM_EXTI_IRQN_INVALID`, una macro nueva igual a `(IRQn_Type)(-128)`, que no es ningún vector de CMSIS;
   - `GPIO_BM_ExtiTakePending` solo mira los bits 0–15 de `line_mask`.
3. **Lo compartido, en la sección (C10).** `RTSR1`, `FTSR1`, `SWIER1`, `SYSCFG_EXTICR` y `SYSCFG_CCCSR` solo se escriben entre `SectionEnter` y `SectionExit`. Las comprobaciones de conflicto y de las máscaras del otro núcleo se hacen dentro de la misma sección, antes de escribir.
4. **Conflicto.** `ExtiConfig` devuelve `ERR_CONFLICT`, sin escribir nada, si `EXTICR` enruta la línea a otro puerto y alguna de `IMR1`, `EMR1`, `C2IMR1` o `C2EMR1` tiene el bit de la línea. `ExtiDisable` devuelve `ERR_CONFLICT`, sin escribir, si la línea está enrutada a otro puerto.
5. **Orden de `ExtiConfig`:**
   1. `EXTICR`;
   2. `RTSR1`/`FTSR1`, según `trigger`;
   3. se borra el `PR1` de este núcleo;
   4. se ponen `IMR1`/`EMR1` de este núcleo, según `interrupt`/`event`; el bit que no se pide se borra.

   La función no toca el modo del pin: el llamador lo configura antes como entrada (`GPIO_BM_ConfigInput`). Tampoco toca el NVIC.
6. **`ExtiDisable` respeta al otro núcleo.**
   - Borra `IMR1`/`EMR1` de este núcleo y su `PR1`.
   - `RTSR1`, `FTSR1` y el campo de `EXTICR` (a 0000) solo los borra si las dos máscaras del otro núcleo están a 0 para esa línea.
7. **`ExtiTakePending`:**
   1. lee el `PR1` de este núcleo y lo enmascara con `line_mask & 0xFFFF`;
   2. escribe solo esos bits (w1c, sin read-modify-write);
   3. hace `__DSB()` y devuelve los bits.

   Se puede llamar desde una ISR: no entra en la sección.
8. **`ExtiSoftwareTrigger`** escribe `1 << pin_idx` en `SWIER1` dentro de la sección. Solo dispara si `RTSR1` y la máscara del núcleo lo permiten (RM0399 cap. 21), y la función no lo comprueba: lo dice el comentario.
9. **`EnableCompensationCell(max_cycles)`:**
   - Si `RCC_CR.CSIRDY = 0`, devuelve `ERR_NOT_READY` sin escribir.
   - Si no, pone `CCCSR.EN` en la sección y espera `READY` durante `max_cycles` ciclos de `DWT->CYCCNT`.
   - Si `CYCCNT` no avanza o se agota el tiempo, devuelve `ERR_NOT_READY` y deja `EN = 1`; el comentario lo dice.
   - Nunca escribe `RCC_CR`, `HSLV`, `CS` ni `CCCR`.
10. **Sin handlers (C14)** y **sin estado nuevo en RAM (C7)**: `.data = 0` y `.bss` igual (8 B).

### 3. Pruebas en `prj_01` `selftest-gpio` (H2, H3)

- **ISR de prueba** en los `gpio_selftest.c`, bajo `GPIO_DRIVER_SELFTEST`:
  - CM7: `EXTI3_IRQHandler` y `EXTI0_IRQHandler`;
  - CM4: `EXTI4_IRQHandler` y `EXTI3_IRQHandler` (para T12).

  Cada una:
  1. pone a 1 su pin de respuesta con `BSRR`, como primera instrucción (CM7 → PE5/DIO9; CM4 → PE6/DIO11);
  2. llama a `GPIO_BM_ExtiTakePending`;
  3. cuenta en un contador privado del núcleo;
  4. baja el pin de respuesta.

  `stm32h7xx_it.c` no define ninguna EXTI.
- **Prioridad NVIC:**
  - en CM7, numéricamente por debajo de `configMAX_SYSCALL_INTERRUPT_PRIORITY`, para que las secciones de FreeRTOS no la retrasen; la ISR no llama a FreeRTOS;
  - en CM4, la que diga el informe.
- **`bench_claim()`:** PE5 y PE6 como salida solo en los pasos de F2. PE3 y PE4 siguen rechazados siempre: los conduce solo el AD2.
- **Pulsos del host (Decisión 2):**
  - `ad2_set_oe(DIO8|DIO10, …)` y escrituras estáticas con `digital_io_write`, cada flanco separado al menos 1 ms;
  - DIO8 y DIO10 parten de 0 y vuelven a 0, y OE vuelve a 0 al final de cada paso (S2);
  - el LA captura DIO8–DIO11 y da el número de flancos enviados y el de respuestas, con un oráculo independiente del firmware.
- **Orden:** T11–T14 y T17 van después de T10 y antes de T16, que bloquea pines hasta el reset.

---

## Archivos

| Ruta | Cambio |
| --- | --- |
| `01_shared_drivers/00_gpio/gpio_bm.h`, `gpio_bm.c` | H1 |
| `03_firmware_templates/prj_01_template_dualcore/CM7/App/Src/gpio_selftest.c`, `CM4/App/Src/gpio_selftest.c`, `Common/Inc/gpio_selftest.h` (campos nuevos al final de `GstArea_t`, comandos nuevos en `GstCmd_t`), `Common/Src/gpio_selftest_common.c` si hace falta | H2 |
| `03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py` | H3 |
| `tools/gpio_f2_checks.py` (nuevo) | H4 |
| `prj_01/docs/gpio_f2_*` | Capturas sin placa (implementador) y de placa (auditor) |
| `01_shared_drivers/00_gpio/IMPLEMENTATION_REPORT.md` | H5 |
| `01_shared_drivers/00_gpio/README.md` | Solo en el cierre |

`tools/ad2.py` **no cambia** (Decisión 2).

---

## Pruebas

| ID | Qué | Oráculo | Control negativo | Criterio |
| --- | --- | --- | --- | --- |
| **T1** (ampliada) | Rechazos de la API nueva, en CM7 y CM4:<br>• `ExtiConfig` con `pin_idx` 16, `trigger` 0 y 4, sin `interrupt` ni `event`, y un puerto inválido;<br>• `ExtiSoftwareTrigger(16)`;<br>• `EnableCompensationCell(0)`;<br>• `ExtiGetIRQn(0..16)` | `ERR_PARAM` y una foto de `EXTICR1–4`, `RTSR1`, `FTSR1`, `IMR1`/`EMR1`/`C2IMR1`/`C2EMR1` y `CCCSR` sin cambios; `ExtiGetIRQn` contra una tabla escrita a mano de los `EXTIx_IRQn` de CMSIS (0–4, 9_5, 15_10) e `INVALID` para 16 | Una llamada válida sí cambia la foto | Todos los rechazos y la tabla exactos |
| **T11** | Por núcleo (CM7 en PE3/línea 3, CM4 en PE4/línea 4) y por `trigger` (RISING, FALLING, BOTH): el host da N = 200 pulsos | Cuenta del LA de los flancos enviados (independiente), cuenta de la ISR (SRAM4) y respuestas en DIO9/DIO11 contadas por el LA | La misma secuencia con la línea configurada solo como evento (`interrupt = false`): 0 ISR y 0 respuestas | RISING = FALLING = 200 y BOTH = 400, en las tres cuentas |
| **T12** | Línea 3 solo en CM7, línea 4 solo en CM4, y después la línea 3 en los dos núcleos | Cuentas de cada núcleo y respuestas en DIO9/DIO11 | `ExtiDisable` de la línea 3 en CM4: CM4 cuenta 0 y CM7 sigue contando 200 | Cada núcleo cuenta solo sus líneas; la compartida, 200 y 200 |
| **T13** | Latencia del flanco de DIO8 a DIO9 (CM7) y de DIO10 a DIO11 (CM4), sobre los flancos de subida de T11 | El LA a 100 MS/s | Línea enmascarada: ninguna respuesta | Ningún evento perdido; mín, p50 y máx impresos; máx ≤ 10 µs en los dos núcleos |
| **T14** | (a) Pending: línea 3 desenmascarada con el NVIC apagado; un flanco del host deja `PR1` a 1; `ExtiConfig` otra vez lo deja a 0; la prueba borra el pending del NVIC (`NVIC_ClearPendingIRQ`), que el driver no toca, y al encender el NVIC hay 0 ISR.<br>(b) Conflicto: con la línea 3 en PE y desenmascarada, `ExtiConfig(GPIOD, 3, …)` devuelve `ERR_CONFLICT` con la foto sin cambios.<br>(c) Deinit: `ExtiDisable` en un núcleo deja `RTSR1`, `FTSR1` y `EXTICR` mientras el otro tiene la máscara, y los borra cuando es el último.<br>(d) `ExtiSoftwareTrigger(0)` con la línea 0 en PE (PE0 no se toca) en CM7: 100 disparos | Lectura directa de `PR1`, `EXTICR`, `RTSR1`, `FTSR1` y las máscaras; contadores de la ISR | (a) Desenmascarar a mano sin borrar `PR1`, con el mismo `NVIC_ClearPendingIRQ`: `PR1` vuelve a pedir la interrupción y hay 1 ISR. (b) Con la línea 3 libre en los dos núcleos, `GPIOD` sí se enruta (y se vuelve a PE). (d) Con la línea 0 enmascarada: 0 ISR | (a) 0 ISR; (b) `ERR_CONFLICT` y foto igual; (c) según la regla 6; (d) 100 ISR y `ExtiTakePending` devuelve el bit 0 |
| **T17** | Celda de compensación en CM7. Se guardan `RCC_CR.CSION` y `CCCSR` y se restauran al final.<br>(a) Sin CSI (`CSION = 0`, esperando `CSIRDY = 0` con límite) y `EN = 0` puesto por la prueba: la llamada devuelve `ERR_NOT_READY` y `CCCSR` no cambia.<br>(b) Con CSI: OK, y ciclos hasta `READY` impresos | `CCCSR.READY` y `RCC_CR.CSIRDY` leídos aparte | Sin CSI, `READY` sigue a 0 tras escribir `EN = 1` a mano (RM0399 §13.3.7) | (a) `ERR_NOT_READY` sin escribir; (b) OK con `READY = 1` dentro de `max_cycles`; estado restaurado |
| **T18** (sin placa, H4) | Regresión:<br>• `build_all.ps1 -Clean` 8/8;<br>• `selftest-gpio` de `prj_01` y de `prj_04` compilan con 0 avisos;<br>• `git diff 3df160a -- '01_shared_drivers/00_gpio/*.h'` solo añade;<br>• ningún `EXTI*_IRQHandler` ni `HAL_GPIO_EXTI_Callback` en `00_gpio/`;<br>• `.data = 0` y `.bss = 8 B` de `gpio_bm.o` en el `.map`;<br>• las imágenes de producción de `prj_01`–`prj_04` enlazan los mismos símbolos `GPIO_BM_*` y con el mismo tamaño que en `3df160a` | `.map` y `nm` de cada imagen, contra el control `3df160a` compilado en un worktree | El control con una cabecera alterada (una firma cambiada) y un `.map` con un símbolo `GPIO_BM_Exti*` de más fallan las mismas comprobaciones | Todo PASS; el control falla |
| **Banco completo** | Dos ejecuciones completas de `gpio_hw_check.py` (T0–T17, F1 incluida) con imágenes de un commit limpio, y `tools/run_warm_resets.py` de producción (README §14.6) | `FW VERDICT=PASS` y `HOST VERDICT=PASS` en la misma captura | Los de cada prueba | Las dos PASS; resets PASS |

### Normas para las capturas
- **Firmware de placa:** las del banco de F1. `gpio_hw_check.py` exige el banner, `Rev` y build id limpios, y escribe `docs/gpio_selftest_<rev>[_n].txt`. Las de F2 se renombran `gpio_f2_selftest_<rev>[_n].txt`.
- **Sin placa:** `prj_01/docs/gpio_f2_<prueba>_<hash>.txt`, de una ejecución, con los comandos dentro.

---

## Banco de pruebas y seguridad

El banco es el de `00_gpio/README.md` §8, sin recablear, con S1–S11 sin cambios. **Antes de cada sesión de placa, el responsable:**
- quita J2 y J3 (las tomas de I2C en PF14/PF15), porque con ellas T5, T6 y T9 de F1 fallan;
- desconecta el USB de la NUCLEO-F334R8, para que solo quede un ST-LINK;
- deja el AD2 por USB, con GND puesto antes que las DIO;
- cierra la GUI de WaveForms.

| # | Regla |
| --- | --- |
| G-S1 | Solo se flashea `prj_01` `selftest-gpio` (y producción de `prj_01` para los resets). El AD2 solo conduce DIO8, DIO10 y DIO12 de forma estática y el bus en sus pasos; PE5 y PE6 solo los conduce el MCU en los pasos de F2 |
| G-S2 | `sn=0016000C4142501220353451` en toda llamada a `STM32_Programmer_CLI`, `flash.py` y `monitor.py` |
| G-S3 | S9 y S10: nunca se desconecta ni se resetea la Nucleo con el script corriendo; el flasheo, con el AD2 en S2 |
| G-S4 | T17 toca `RCC_CR.CSION` solo dentro de la prueba y lo restaura; nada más del árbol de reloj, nada de alimentación ni option bytes |
| G-S5 | Al terminar, la placa queda con la imagen que diga el responsable; el informe dice cuál |

---

## Fases y entregables

| Fase | Entregable | Criterio de pase |
| --- | --- | --- |
| **F1** | H1–H5. Capturas sin placa de T1 (compilación) y T18. Los comandos exactos del banco para el auditor | Auditoría de código y herramientas, T18 propia del auditor, y las dos ejecuciones completas del banco y los resets del auditor |
| **Cierre** | **Implementador:** `00_gpio/README.md` (§2, §9 con las pruebas nuevas y sus capturas, §9.5, §10 sin la falta de EXTI, §11, §13.3 hecho, §15).<br>**Responsable:** `DRIVER_ROADMAP.md`, el borrado de los documentos de proceso y el merge | Auditoría de cierre conforme |

---

## Riesgos y mitigaciones

| Riesgo | Mitigación |
| --- | --- |
| Contención en PE3/PE4 o PE5/PE6 | S1–S4 sin cambios: PE3/PE4 siguen rechazados en `bench_claim()`; DIO9/DIO11 siguen prohibidos como salida en `ad2.py`; la autoprueba T0B de las guardas sigue en cada ejecución |
| T17 deja la celda o CSI distintos | Se guardan y restauran `CSION` y `CCCSR`; la ejecución imprime los dos al final; en `prj_01`, CSI solo alimenta la celda |
| Una ISR de prueba se queda habilitada y llega a T16 o a producción | Cada prueba hace `ExtiDisable` y apaga el NVIC al terminar; las ISR solo existen con `GPIO_DRIVER_SELFTEST` (T18 lo comprueba con `nm` en producción) |
| Latencia de T13 alta por las secciones de FreeRTOS en CM7 | Prioridad de la EXTI por encima de `configMAX_SYSCALL_INTERRUPT_PRIORITY` |
| El implementador no puede probar el banco | La lógica del host se prueba sin placa con el `_MockDwf` de `gpio_hw_check.py`; el primer contacto con la placa es del auditor, que puede pedir rondas |
| Línea 15 de H5c | Ninguna prueba la usa; T18 lo comprueba en las fuentes |

---

## Decisiones abiertas

1. **Implementador y banco:**
   - **Opción A (por defecto):** Antigravity implementa; el auditor ejecuta todo lo de la placa con las herramientas del implementador, como en `03_tim` F4.
   - **Opción B:** otro implementador con acceso a la placa.
2. **Cómo dar los pulsos en DIO8/DIO10:**
   - **Opción A (por defecto):** escrituras estáticas, como ya permite `ad2_set_oe`. T11 da 200 pulsos espaciados (unos ms por flanco); la cuenta del LA es el oráculo. `ad2.py` y la guarda S11 no cambian.
   - **Opción B:** ampliar la guarda S11 para que el generador de patrones conduzca DIO8/DIO10 y dar trenes de 10 kHz, como pedía el plan original. Más cerca de un uso real, pero toca la guarda de seguridad y su autoprueba.
3. **Celda de compensación:**
   - **Opción A (por defecto):** solo la API y T17; los cinco `SystemClock_Config` siguen con `HAL_EnableCompensationCell()`.
   - **Opción B:** además, `prj_01` pasa a `GPIO_BM_EnableCompensationCell()`, que espera `READY` de verdad. Toca `SystemClock_Config`.
4. **Documentos de proceso:** se borran al cierre, como en F1b, F1c y `03_tim` F4.
