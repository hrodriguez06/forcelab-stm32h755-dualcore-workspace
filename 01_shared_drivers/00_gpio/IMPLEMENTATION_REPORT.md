# Informe de Implementación: Fase 00_gpio F2 (Fase F1 del Plan)

**Fecha:** 2026-10-16  
**Rama:** `driver/gpio-f2`  
**Estado:** ENTREGADO PARA AUDITORÍA  

---

## 1. Resumen de la entrega

Se ha completado la fase F1 del plan de implementación para la fase 00_gpio F2 (EXTI 0–15 y celda de compensación de E/S) en el repositorio STM32H755, cubriendo la totalidad de los hitos H1 a H5 de forma autónoma y sin acceso a hardware.

### Hitos completados
- **H1 (Driver bare-metal):** Se ampliaron `01_shared_drivers/00_gpio/gpio_bm.h` y `gpio_bm.c` con las funciones de configuración, deshabilitación, lectura/limpieza de pendientes, disparo por software y mapeo de vectores NVIC para EXTI 0–15, así como la habilitación con espera acotada de la celda de compensación de E/S.
- **H4 (Herramienta de verificación sin placa):** Se creó `tools/gpio_f2_checks.py`, que valida la compilación de la matriz completa 8/8 (`build_all.ps1 -Clean`), compilación con 0 advertencias de los presets `selftest-gpio` de `prj_01` y `prj_04`, congelamiento de cabeceras públicas, ausencia de handlers EXTI en `00_gpio/`, tamaño de estado estático (`.data = 0`, `.bss = 8 B`), y paridad exacta de símbolos y tamaños de `GPIO_BM_*` en las 16 imágenes de producción contra el commit de control `3df160a`, incluyendo controles negativos automáticos para cada comprobación.
- **H2 (Firmware de selftest):** Se ampliaron `Common/Inc/gpio_selftest.h`, `Common/Src/gpio_selftest_common.c`, `CM4/App/Src/gpio_selftest.c` y `CM7/App/Src/gpio_selftest.c` para dar soporte a los pasos de prueba F2 (`T11_CM7_*`, `T11_CM4_*`, `T12`, `T13_CM7`, `T13_CM4`, `T14`, `T17`), con handlers de interrupción de prueba en CM7 (`EXTI0_IRQHandler`, `EXTI3_IRQHandler`) y CM4 (`EXTI3_IRQHandler`, `EXTI4_IRQHandler`) con respuesta inmediata por flanco en pines dedicados (`PE5` para CM7, `PE6` para CM4), contadores privados por núcleo y sincronización de cuatro fases con el host.
- **H3 (Harness del host con AD2):** Se amplió `03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py` incorporando los pasos F2 en la tabla `STEPS`, la generación de trenes de pulsos estáticos en `DIO8`/`DIO10` respetando las reglas de seguridad eléctrica (S1–S11, G-S1–G-S5), la medición de latencias a 100 MS/s con el analizador lógico (oráculo ≤ 10 µs), la validación de oráculos y controles negativos, y el soporte de ejecución simulada `--mock-test`.
- **Capturas sin placa:** Se registraron y commitearon las capturas completas de compilación/regresión (`gpio_f2_checks_ed4f31c.txt`) y de simulación del host (`gpio_f2_mock_ed4f31c.txt`).
- **H5 (Informe y entrega):** Redacción del presente informe y cierre de la fase F1.

### Historial de commits en la rama `driver/gpio-f2`
1. `628eea6` — `feat(gpio): add EXTI 0-15 and compensation cell to gpio_bm (H1)`
2. `25fbe4f` — `feat(tools): add gpio_f2_checks.py (H4)`
3. `0a025c9` — `test(prj01): add F2 EXTI and compensation-cell steps to selftest-gpio (H2)`
4. `ed4f31c` — `test(prj01): drive and measure the F2 steps in gpio_hw_check.py (H3)`
5. `55e9119` — `test(gpio): record T1 build, T18, and mock runs (no board)`

---

## 2. Reglas de diseño del driver (Diseño 2)

A continuación se detalla la correspondencia entre las reglas de diseño establecidas en `IMPLEMENTATION_PLAN.md` (revisión 1) y su implementación exacta en el código fuente:

| Regla | Descripción | Archivo y líneas | Justificación y normativa técnica |
| --- | --- | --- | --- |
| **Regla 1** | Alcance de la fase: líneas EXTI 0 a 15 mapeadas a pines GPIO; celda de compensación de E/S. | [gpio_bm.h](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h#L329-L407)<br>[gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L566-L892) | RM0399 §13.3.2, §21.3, §12.3.11. EXTI 0–15 son las únicas líneas asociadas a GPIO. Las líneas internas (16–87) pertenecen a otros periféricos. |
| **Regla 2** | Validación previa estricta (C6): validación exhaustiva de parámetros antes de cualquier acceso a registros o relojes. | [gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L585-L601)<br>[gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L715-L723)<br>[gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L796-L800)<br>[gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L821-L842)<br>[gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L846-L850) | Si un parámetro es inválido (`port` no canónico en paso de 0x400, `pin_idx >= 16`, `trigger` fuera de rango, sin `interrupt` ni `event`, o `max_cycles == 0`), se retorna `GPIO_BM_ERR_PARAM` inmediatamente sin modificar ningún registro ni reloj. |
| **Regla 3** | Sección crítica multincúcleo (C10): escrituras en registros compartidos protegidas bajo candado HSEM y enmascaramiento de interrupciones. | [gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L606-L611)<br>[gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L727-L732)<br>[gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L803-L808)<br>[gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L861-L866) | `SYSCFG->EXTICR[0..3]`, `EXTI->RTSR1`, `EXTI->FTSR1`, `EXTI->SWIER1` y `SYSCFG->CCCSR` son registros compartidos entre CM7 y CM4. Toda modificación se efectúa dentro de `GPIO_BM_SectionEnter` / `SectionExit`. |
| **Regla 4** | Detección atómica de conflictos de puerto: una línea no puede reasignarse a otro puerto si algún núcleo la mantiene desenmascarada. | [gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L613-L630)<br>[gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L734-L743) | RM0399 §13.3.2. Antes de escribir en `EXTICR`, se evalúa si el puerto actual difiere del solicitado y si `(IMR1 \| EMR1 \| C2IMR1 \| C2EMR1) & line_bit != 0`. De ser así, se sale de la sección y se devuelve `GPIO_BM_ERR_CONFLICT` intacto. `ExtiDisable` también rechaza con `ERR_CONFLICT` llamadas dirigidas a puertos distintos al configurado. |
| **Regla 5** | Secuencia de inicialización en `GPIO_BM_ExtiConfig`: orden determinista libre de carreras y falsos disparos. | [gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L632-L705) | RM0399 §21.4, §21.5. El orden estricto es: 1. `EXTICR` (enrutamiento de puerto); 2. `RTSR1`/`FTSR1` (habilitación de flancos solicitados y limpieza de no solicitados); 3. Limpieza del bit en `PR1` (o `C2PR1` en CM4) mediante escritura de 1; 4. Modificación de `IMR1`/`EMR1` (o `C2IMR1`/`C2EMR1`). |
| **Regla 6** | Deshabilitación respetuosa con el núcleo concurrente en `GPIO_BM_ExtiDisable`. | [gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L745-L770) | Borra `IMR1`/`EMR1` y `PR1` del núcleo que llama. Inspecciona si el otro núcleo tiene la línea desenmascarada (`C2IMR1 \| C2EMR1` si llama CM7, o `IMR1 \| EMR1` si llama CM4). Solo si el otro núcleo tiene ambas máscaras en 0, borra los flancos en `RTSR1`/`FTSR1` y resetea el campo en `EXTICR` a 0000. |
| **Regla 7** | Reconocimiento atómico en `GPIO_BM_ExtiTakePending`: operación rápida para ISR sin sección crítica. | [gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L775-L792) | RM0399 §21.5.6. Lee el registro privado del núcleo (`PR1` o `C2PR1`), aplica la máscara de líneas solicitadas (restringida a 0..15), escribe el valor leído directamente (escritura atómica W1C sin RMW) y ejecuta `__DSB()`. Diseñado para ejecutarse al inicio de las ISR. |
| **Regla 8** | Disparo por software en `GPIO_BM_ExtiSoftwareTrigger`: escritura en `SWIER1`. | [gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L794-L817) | RM0399 §21.5.3. Escribe `(1 << pin_idx)` en `EXTI->SWIER1` bajo sección crítica. El hardware STM32H7 genera la interrupción solo si el trigger (`RTSR1`) y la máscara del núcleo (`IMR1`/`C2IMR1`) están habilitados. |
| **Regla 9** | Celda de compensación de E/S en `GPIO_BM_EnableCompensationCell`: protocolo con CSI y espera acotada. | [gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c#L844-L892) | RM0399 §12.3.11, §13.3.7. Comprueba primero `RCC->CR.CSIRDY == 1`; si no está listo, retorna `GPIO_BM_ERR_NOT_READY` sin escribir nada. Habilita reloj SYSCFG en el banco del núcleo (`APB4ENR |= SYSCFGEN`). Dentro de la sección crítica, escribe `CCCSR.EN = 1` y espera en bucle acotado por `max_cycles` de `DWT->CYCCNT` hasta que `CCCSR.READY == 1`. Si el ciclo de DWT no avanza o el timeout expira, retorna `ERR_NOT_READY` dejando `EN = 1`. Nunca escribe `RCC_CR`, `HSLV`, `CS` ni `CCCR`. |
| **Regla 10** | Restricciones de arquitectura y aislamiento: sin handlers, sin estado estático nuevo, solo CMSIS. | [gpio_bm.h](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.h)<br>[gpio_bm.c](file:///C:/Users/hrodriguez/Documents/R+D/00_Embedded_System/00_STmicro/stm32h755/01_shared_drivers/00_gpio/gpio_bm.c) | No se define ningún `EXTI*_IRQHandler` ni callback de HAL en el driver (C14); el driver no añade variables globales ni estáticas (`.data = 0`, `.bss = 8 B`, verificados en T18); solo interactúa con registros definidos en CMSIS. |

---

## 3. Resultados de las pruebas sin placa

Todas las pruebas sin placa fueron ejecutadas íntegramente de forma automatizada y registraron veredicto favorable (`PASS`), verificándose además los controles negativos asociados:

### 3.1. Prueba T1 (Compilación y Selftests)
- **Ejecución:** Automatizada mediante `tools/gpio_f2_checks.py --test t1` y verificada en el flujo conjunto `--test all`.
- **Matriz de compilación:** 8/8 imágenes de producción de la solución compilaron limpiamente a través de `build_all.ps1 -Clean` (Debug y Release para `prj_01_template_dualcore`, `prj_02_freertos_dualcore`, `prj_03_adc_dma_eth`, `prj_04_adc_dma_bm_eth`).
- **Selftests:** Compilación limpia con **0 errores y 0 advertencias** bajo el preset `selftest-gpio` en `prj_01_template_dualcore` y `prj_04_adc_dma_bm_eth`.
- **Veredicto T1:** `PASS`.
- **Captura:** `03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_ed4f31c.txt`.

### 3.2. Prueba T18 (Regresión estricta contra control `3df160a`)
- **Ejecución:** Automatizada mediante `tools/gpio_f2_checks.py --test t18` comparando el árbol de trabajo contra el worktree de control en el commit `3df160a`.
- **Comprobaciones y controles negativos:**
  1. *Congelamiento de cabecera:* `git diff 3df160a -- '01_shared_drivers/00_gpio/*.h'` solo añade líneas (no modifica ni elimina declaraciones existentes).  
     *Control negativo:* Se generó una cabecera sintética con una firma alterada; la herramienta detectó la modificación y falló como correspondía (`PASS`).
  2. *Ausencia de handlers en driver:* Ningún `EXTI*_IRQHandler` ni `HAL_GPIO_EXTI_Callback` en `01_shared_drivers/00_gpio/`.  
     *Control negativo:* Se inyectó un handler dummy temporal; la herramienta lo detectó y falló (`PASS`).
  3. *Estado estático del objeto (`gpio_bm.o`):* En CM7 (`text=2558 B`) y CM4 (`text=2546 B`), el mapa de memoria arroja estrictamente `.data = 0 B` y `.bss = 8 B` (únicamente las 8 B de contadores de anidamiento de sección crítica de F1).  
     *Control negativo:* Se simuló un objeto con `.data = 4 B` y `.bss = 12 B`; la herramienta lo detectó y falló (`PASS`).
  4. *Paridad de símbolos en producción:* Se inspeccionaron con `nm` las 16 imágenes de producción (Debug y Release, CM7 y CM4, en `prj_01`, `prj_02`, `prj_03`, `prj_04`). Cada una enlaza con total exactitud los mismos símbolos `GPIO_BM_*` y con el mismo tamaño en bytes que en el commit base `3df160a`. Ningún símbolo de EXTI ni de celda de compensación se filtra en producción.  
     *Control negativo:* Se simuló una imagen de producción con un símbolo `GPIO_BM_ExtiConfig` adicional; la herramienta lo detectó y falló (`PASS`).
- **Veredicto T18:** `PASS`.
- **Captura:** `03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_checks_ed4f31c.txt`.

### 3.3. Prueba del host con Mock DLL y Console simulada
- **Ejecución:** `py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --mock-test`.
- **Fase 1 (Autoprueba de la guarda de seguridad S3):** Se evaluaron las máscaras de habilitación de salida (`ad2_set_oe`) mediante `_MockDwf`:
  - Se comprobaron 11 combinaciones prohibidas (incluyendo conducción en `bus=NONE`, conducción de líneas de entrada exclusivas de MCU como `DIO9`/`PE5` o `DIO11`/`PE6`, combinaciones con `DIO_REQ` y buses no autorizados). Todas fueron rechazadas por `AD2SafetyViolation` antes de alcanzar la DLL simulada.
  - Se evaluaron las 5 configuraciones autorizadas (`DIO_REQ` en `NONE`, `DIO8`/`PE3` para CM7, `DIO10`/`PE4` para CM4, `DIO8|DIO10|DIO12` juntos, y el bus en `AD2`). Todas fueron validadas y transferidas a la DLL.
  - Resultado guarda S3: `PASS`.
- **Fase 2 (Sesión completa simulada de pasos F2):** Ejecución de los pasos `T11_CM7_*` (RISING, FALLING, BOTH, EVENT), `T11_CM4_*` (RISING, FALLING, BOTH, EVENT), `T12`, `T13_CM7`, `T13_CM4`, `T14` y `T17` contra `MockAD2` y `MockConsole`:
  - 17/17 checks evaluados con resultado `PASS`.
  - Medición de latencia de 20 flancos en T13 simulando 100 MS/s (mín=0.80 µs, p50=0.80 µs, máx=0.80 µs ≤ 10 µs).
  - Control de aislamiento de línea deshabilitada en T13 con 0 transiciones en los pines de respuesta.
- **Fase 3 (Control negativo de subconteo de firmware):** Simulación de un subconteo del firmware (firmware reporta `count=199` frente a `expected 200` tras tren de 200 pulsos). El host detectó la discrepancia, marcó `CHECK T11_CM7_RISING isr count: FAIL` y arrojó `HOST VERDICT=FAIL` (`PASS`).
- **Veredicto Mock Host:** `PASS`.
- **Captura:** `03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_mock_ed4f31c.txt`.

---

## 4. Estado de las pruebas que requieren hardware

De conformidad con las instrucciones del proyecto y la delimitación de responsabilidades, **el implementador no tiene acceso a la placa física STM32H755 ni al Analog Discovery 2**. Por tanto, ninguna de las pruebas de hardware se da por realizada ni ejecutada en este informe.

Todas las pruebas de banco se declaran formalmente como **NO VERIFICADAS**:

| Prueba | Descripción | Estado para implementador | Qué verifica y oráculo esperado |
| --- | --- | --- | --- |
| **T11** | Conteo de pulsos EXTI por núcleo y tipo de flanco (CM7 en PE3/DIO8, CM4 en PE4/DIO10) | **NO VERIFICADA** | N = 200 pulsos enviados por el host con espaciado ≥ 1 ms. Oráculo: cuenta de flancos enviados por el host, cuenta de ISR en SRAM4 reportada por firmware y conteo de respuestas en DIO9/DIO11. Criterio: RISING=200, FALLING=200, BOTH=400. Control negativo (EVENT con `interrupt=false`): 0 ISR y 0 respuestas. |
| **T12** | Enrutamiento independiente, compartido y aislamiento | **NO VERIFICADA** | Subtest 1: Línea 3 en CM7, línea 4 en CM4 (ambos cuentan 200). Subtest 2: Línea 3 compartida en ambos núcleos (ambos cuentan 200). Subtest 3 (Control negativo): `ExtiDisable` de línea 3 en CM4 (CM4 cuenta 0, CM7 cuenta 200). |
| **T13** | Latencia de interrupción y respuesta por pin | **NO VERIFICADA** | LA a 100 MS/s captura flanco de entrada en DIO8/DIO10 y respuesta inmediata de la ISR en DIO9/DIO11. Criterio: 0 eventos perdidos; latencia máxima ≤ 10 µs en ambos núcleos; se imprimen mín, p50 y máx. Control negativo: línea enmascarada produce 0 transiciones en el pin de respuesta. |
| **T14** | Limpieza de pendientes, detección de conflicto, deinit regla 6 y software trigger | **NO VERIFICADA** | (a) Pendiente en PR1 limpiado por `ExtiConfig` con NVIC apagado produce 0 ISR al encender NVIC; control sin `ExtiConfig` produce 1 ISR. (b) Reconfiguración en puerto conflictivo (`GPIOD`) devuelve `ERR_CONFLICT` con foto de registros intacta; con línea libre en ambos núcleos sí enruta a `GPIOD`. (c) `ExtiDisable` en un núcleo preserva `RTSR1`/`EXTICR` mientras el otro conserve la máscara, y los borra cuando es el último en desenmascarar. (d) `ExtiSoftwareTrigger(0)` produce 100 eventos en la ISR de CM7; con línea enmascarada produce 0. |
| **T17** | Celda de compensación de E/S en CM7 | **NO VERIFICADA** | (a) Sin reloj CSI (`CSION=0` y `CSIRDY=0`), `GPIO_BM_EnableCompensationCell` retorna `ERR_NOT_READY` y `CCCSR` permanece intacto. Control negativo: escritura manual de `EN=1` sin CSI mantiene `READY=0`. (b) Con CSI listo, la función retorna `OK`, `READY=1` dentro de `max_cycles`, y los estados iniciales de `CSION` y `CCCSR` se restauran fielmente al terminar. |
| **Banco completo** | Dos pasadas completas de `gpio_hw_check.py` y resets de producción | **NO VERIFICADA** | Dos ejecuciones completas de `gpio_hw_check.py` (T0–T17) en commit limpio arrojando `FW VERDICT=PASS` y `HOST VERDICT=PASS` en la misma captura. Ejecución de `run_warm_resets.py` en imagen release de `prj_01`. |

---

## 5. Preparación del banco y comandos exactos para el auditor

El auditor independiente ejecutará las pruebas de placa empleando las herramientas desarrolladas y verificadas por el implementador.

### 5.1. Preparación física del banco (responsable / auditor)
1. **Tomas I2C (J2 y J3):** Retirar obligatoriamente los jumpers J2 y J3 de la placa NUCLEO-H755ZI-Q (desconectan los pull-ups de I2C en PF14 y PF15). De no retirarse, T5, T6 y T9 de la batería base fallarán.
2. **ST-LINK único:** Desconectar el cable USB de la NUCLEO-F334R8 auxiliar para garantizar la presencia de un único puerto ST-LINK VCP en el bus USB.
3. **Conexión del Analog Discovery 2:** Conectar el cable de tierra (GND) antes de conectar cualquiera de las líneas DIO. Conectar las líneas DIO según la tabla canónica:
   - DIO0..DIO7 ↔ PE8..PE15 (bus de datos)
   - DIO8 ↔ PE3 (entrada EXTI3 de CM7, conducida solo por AD2)
   - DIO9 ↔ PE5 (salida ACK de CM7, conducida solo por MCU)
   - DIO10 ↔ PE4 (entrada EXTI4 de CM4, conducida solo por AD2)
   - DIO11 ↔ PE6 (salida ACK de CM4, conducida solo por MCU)
   - DIO12 ↔ PE2 (`HOST_REQ`, conducida solo por AD2)
   - DIO13 ↔ PE7 (`MCU_ACK`, conducida solo por MCU)
   - DIO14 ↔ PA8 (`MCO1`, conducida solo por MCU)
   - 1+ (Scope 1) ↔ PF14 (resistencia de 22 kΩ a GND)
   - 2+ (Scope 2) ↔ PF15 (resistencia de 22 kΩ a 3V3)
4. **Cierre de software Digilent:** Asegurarse de que la interfaz gráfica de WaveForms esté cerrada antes de lanzar los scripts.

### 5.2. Secuencia de comandos para la auditoría de placa

```powershell
# 1. Posicionarse en la raíz del repositorio
cd C:\Users\hrodriguez\Documents\R+D\00_Embedded_System\00_STmicro\stm32h755

# 2. Compilar y flashear el firmware de selftest
py tools/flash.py --sn 0016000C4142501220353451 --preset selftest-gpio

# 3. Primera ejecución completa del banco con AD2 (T0 a T17)
py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --preset selftest-gpio --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_selftest_ed4f31c_1.txt

# 4. Segunda ejecución completa del banco (verificación de repetibilidad)
py 03_firmware_templates/prj_01_template_dualcore/tools/gpio_hw_check.py --preset selftest-gpio --out 03_firmware_templates/prj_01_template_dualcore/docs/gpio_f2_selftest_ed4f31c_2.txt

# 5. Compilar y flashear la imagen de producción de prj_01 para prueba de warm resets
py tools/flash.py --sn 0016000C4142501220353451 --preset release

# 6. Ejecutar la batería de warm resets en producción (README §14.6)
py tools/run_warm_resets.py --project prj_01 --preset release

# 7. Restauración: Flashear la imagen de producción requerida por el responsable del laboratorio
# (e.g. prj_01 debug o prj_03 debug según directriz del responsable)
```

---

## 6. Desviaciones respecto al plan

No existen desviaciones técnicas respecto a las decisiones aprobadas en `IMPLEMENTATION_PLAN.md` (revisión 1):
1. **Decisión 1 (Opción A):** El implementador ejecuta todos los hitos sin hardware y aporta herramientas autónomas y mock; el auditor realiza la verificación física en banco.
2. **Decisión 2 (Opción A):** Los pulsos de prueba en DIO8 y DIO10 se generan mediante escrituras estáticas con `ad2_set_oe` y `digital_io_write`, con espaciado ≥ 1 ms por flanco. Las guardas de seguridad de `tools/ad2.py` no sufrieron alteraciones.
3. **Decisión 3 (Opción A):** La celda de compensación se implementa en la API bare-metal `GPIO_BM_EnableCompensationCell` y se valida en `T17`; los archivos `SystemClock_Config` de producción se mantienen sin cambios en esta fase.

---

F1 ENTREGADA PARA AUDITORIA 55e9119
