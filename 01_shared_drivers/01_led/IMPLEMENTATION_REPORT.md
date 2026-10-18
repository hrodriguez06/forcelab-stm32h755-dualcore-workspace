# Informe de Implementación: Cierre de Limitaciones L3, L4 y L5 (Driver 01_led)

**Fecha:** 2026-10-17  
**Rama de trabajo:** `driver/led-close`  
**Estado:** Entregado para auditoría  
**Commit base:** `0927c75` (main)  
**Último commit:** `c080fcb`  

---

## 1. Resumen Ejecutivo

En esta fase se implementaron y verificaron íntegramente las limitaciones L3, L4 y L5 del driver compartido `01_led` (`led_bm.c` y `led_bm.h`) sobre la arquitectura STM32H755ZI, de acuerdo con las especificaciones de [`01_shared_drivers/01_led/IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) y [`01_shared_drivers/01_led/IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md).

### Secuencia de commits entregados:
1. `654cf75`: `fix(led): configure LEDs through GPIO_BM_ConfigOutput (L3, L5)`
2. `bba74a6`: `feat(led): add LED_Init_ConfigEx returning the configured mask (L5)`
3. `3e03485`: `fix(led): make LED_Toggle atomic on the calling core (L4)`
4. `d3a03b0`: `test(prj01): add the selftest-led preset with LT1-LT5`
5. `db39a0f`: `test(prj01): add led_hw_check.py and its mock`
6. `c080fcb`: `test(tools): add selftest-led and the new T4 registers to led_l1l2_checks`

Todas las pruebas fuera de placa (T1, T2, T3 y suite mock) pasaron satisfactoriamente (8/8 builds de matriz, 10 presets de selftest, 20/20 casos T2, y T3 idéntico al oráculo UM2408 y al control `9dc6896`). Asimismo, se ejecutó en hardware real (NUCLEO-H755ZI-Q, ST-LINK SN `0016000C4142501220353451`) la suite de comprobación `led_hw_check.py` en dos ejecuciones consecutivas independientes (`run1` y `run2`) y la suite T4 completa, resultando todas en `PASS`.

---

## 2. Detalle de Cambios en el Driver: L3, L4 y L5

### 2.1 L3: Configuración atómica vía `GPIO_BM_ConfigOutput`
- **Archivo:** [`01_shared_drivers/01_led/led_bm.c:72-76`](led_bm.c#L72-L76)
- **Antes (commit `0927c75`, líneas 65–85):**
  `LED_Init()` configuraba manualmente los registros `MODER`, `OTYPER`, `OSPEEDR`, `PUPDR` de los puertos GPIO mediante operaciones RMW (`|=` y `&=`) directas, sin entrar en la sección atómica de `00_gpio`, sin tomar el lock de hardware HSEM entre núcleos (`Shared_GpioLockInstall()`), y sin verificación de lectura posterior (*read-back verification*).
- **Después (`HEAD`, líneas 68–76 y 94–104):**
  `LED_Init()` delega en `LED_Init_Config()` pasándole la configuración por defecto, y ésta llama a `LED_Init_ConfigEx()`. Para cada pin se invoca `GPIO_BM_ConfigOutput(port, bit, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 0U)`. La configuración valida puerto y pin antes de tocar hardware, escribe `BSRR` antes de `MODER` para evitar transiciones espurias, se ejecuta bajo la sección crítica con el lock de hardware cross-core (`00_gpio/README.md` §3.3), y realiza verificación de lectura.

### 2.2 L4: `LED_Toggle` atómico en el núcleo que llama
- **Archivo:** [`01_shared_drivers/01_led/led_bm.c:149-162`](led_bm.c#L149-L162)
- **Antes (commit `0927c75`, líneas 110–133):**
  ```c
  if (port->ODR & (1UL << bit)) {
      port->BSRR = (1UL << (bit + 16U));
  } else {
      port->BSRR = (1UL << bit);
  }
  ```
  La lectura de `ODR` y la posterior escritura en `BSRR` no estaban protegidas frente a interrupciones. Una ISR ejecutada entre la lectura y la escritura provocaba la pérdida de la transición (*lost toggle*).
- **Después (`HEAD`, líneas 146–162):**
  ```c
  uint32_t primask = __get_PRIMASK();
  __disable_irq();

  if (port->ODR & (1UL << bit)) {
      port->BSRR = (1UL << (bit + 16U));
  } else {
      port->BSRR = (1UL << bit);
  }

  __set_PRIMASK(primask);
  ```
  Se resguarda `PRIMASK`, se deshabilitan las interrupciones en el núcleo local durante la lectura de `ODR` y la emisión del bit en `BSRR`, y se restaura el nivel previo de `PRIMASK`. Es estrictamente atómico frente a cualquier interrupción en el núcleo invocador.

### 2.3 L5: `LED_Init_ConfigEx`, máscara devuelta y anulación de entradas inválidas
- **Cabecera:** [`01_shared_drivers/01_led/led_bm.h:73-80`](led_bm.h#L73-L80)
  Se añadió la declaración:
  ```c
  uint32_t LED_Init_ConfigEx(const led_driver_config_t *config);
  ```
  La adición es estrictamente aditiva, respetando el congelamiento de firmas existentes (`01_shared_drivers/README.md`, regla 2).
- **Implementación:** [`01_shared_drivers/01_led/led_bm.c:53-87`](led_bm.c#L53-L87)
  - Si `config == NULL`, retorna 0 inmediatamente sin modificar la configuración interna.
  - Para cada LED `i` (0 a `LED_COUNT-1`): si `GPIO_BM_ConfigOutput` retorna `GPIO_BM_OK`, se copia en la estructura interna `g_led_config` y se acumula el bit `(1UL << i)` en la máscara.
  - Si `GPIO_BM_ConfigOutput` falla (puerto nulo o inválido, pin > 15, pin de depuración SWD como PA13, pin bloqueado por `LCKR`, fallo del lock HSEM o discordancia de read-back), la entrada correspondiente en `g_led_config.leds[i].port` se anula a `NULL`.
  - Como consecuencia de la anulación a `NULL`, las funciones operativas (`LED_Set`, `LED_Toggle`, `LED_BootStage` y `LED_PanicRed`) verifican `port == NULL` y descartan la operación de forma segura.

---

## 3. Descripción y Validación de los Pasos del Selftest (LT1–LT5)

El selftest corre en CM7 tras el `LED_Init()` del arranque, no utiliza el AD2 y restringe cualquier alteración física exclusivamente a los pines PB0, PE1 y PB14.

| Paso | Operación Real | Datos Impresos | Oráculo del Host | Control Negativo en la misma Ejecución |
| --- | --- | --- | --- | --- |
| **LT1** (L3) | Pasa PB0, PE1 y PB14 a analógico con `GPIO_BM_DeinitPin`. Instala un hook de lock cross-core que cuenta llamadas y retorna `true`. Llama a `LED_Init_ConfigEx` con `board_def.h`. Lee configuración de registros y `ODR`. | `LT1: mask=0x07 lock=3 unlock=3`<br>`LT1_PINS: G(...) Y(...) R(...)`<br>`LT1_CTRL: mask=0x00 regs_equal=1 odr_g=0 odr_y=0 odr_r=0` | Máscara `0x07`; `lock == unlock >= 3`; los 3 pines en salida (mode=1), push-pull (otype=0), velocidad baja (speed=0), sin pull (pull=0), `ODR=0`. | Pines de nuevo en analógico y hook retornando `false`: máscara 0, registros de GPIOA/B/E idénticos a la instantánea previa (`regs_equal=1`) y `LED_Set(..., 1)` no altera `ODR`. |
| **LT2** (L5) | Llama a `LED_Init_ConfigEx` con tabla deliberadamente inválida: verde con `pin_num=16`, amarillo con dirección de puerto fuera de rango, rojo en PA13 (SWD). Toma instantánea de 15 palabras (MODER, OTYPER, OSPEEDR, PUPDR, ODR de GPIOA, GPIOB, GPIOE) antes, post-init y post-ops (`LED_Set`, `LED_Toggle`, `LED_BootStage`). | `LT2: mask=0x00 init_equal=1 ops_equal=1`<br>`LT2_CTRL: mask=0x01 pb0_mode_before=3 pb0_mode_after=1 gpiob_changed=1` | Máscara `0x00`; las 15 palabras de registros se mantienen 100% inalteradas antes, tras init y tras operaciones (`init_equal=1`, `ops_equal=1`). | Tabla semi-inválida donde únicamente el verde (PB0) es válido: máscara `0x01`, PB0 pasa de analógico (3) a salida (1) y `gpiob_changed=1`. |
| **LT3** (L4) | Inicia TIM17 periódicamente a 200 kHz (~5 µs por interrupción). La ISR alterna `LED_GREEN` con `LED_Toggle`. El hilo principal ejecuta N=200,000 llamadas a `LED_Toggle(LED_GREEN)`, muestreando con `PRIMASK` el ODR y la cuenta de la ISR antes y después de cada llamada. | `LT3: N=200000 freq_hz=200000 window_isrs=6986 lost=0`<br>`LT3_CTRL: N=200000 freq_hz=200000 window_isrs=7225 lost=2155` | `lost == 0`, con `window_isrs > 0` constatando que la ISR se solapó miles de veces durante las llamadas. | Misma prueba ejecutando una copia local de la versión antigua de `LED_Toggle` de commit `0927c75` (sin `PRIMASK`): pierde 2,155 transiciones (`lost > 0`). Si el control diese 0, el paso falla. |
| **LT4** (G14) | Pone PB14 (rojo) en analógico. Ejecuta `LED_PanicRed()`. | `LT4: red_mode=1 red_odr=1`<br>`LT4_CTRL: green_unchanged=1 green_mode=1 green_odr=0` | PB14 queda en modo salida (mode=1) con `ODR=1`. | PB0 (verde), el cual `LED_PanicRed` no debe tocar, permanece inalterado (`green_unchanged=1`, mode=1, ODR=0). |
| **LT5** | Restaura el estado final llamando a `LED_Init()`, detiene TIM17, deshabilita `UIE`, limpia pendientes en NVIC y apaga su reloj APB2. | `LT5: tim17_stopped=1`<br>`LT5_PINS: G(...) Y(...) R(...)` | TIM17 completamente detenido (`tim17_stopped=1`); los tres pines restaurados a salida push-pull baja velocidad sin pull con `ODR=0`. | — |

---

## 4. Resultados de Pruebas sin Placa (T1, T2, T3 y Mock)

### 4.1 T1: Matriz de compilación y presets selftest
- **8 Configuraciones de matriz:** `prj_01` (debug/release), `prj_02` (debug/release), `prj_03` (debug/release), `prj_04` (debug/release).  
  Resultado: 8/8 `PASS` (0 avisos del compilador, objetos `led_bm.c.obj` compilados y verificados).
- **10 Presets de selftest con `led_bm`:**  
  `prj_01` (`selftest-gpio`, `selftest-tim`, `selftest-spi`, `selftest-i2c`, `selftest-led`).  
  `prj_04` (`selftest`, `selftest-adc`, `selftest-uart`, `selftest-hsem`, `selftest-gpio`).  
  Resultado: 10/10 `PASS` (0 avisos del compilador).
- **Control negativo de avisos (MA2):** Compilación sintética de advertencia capturada por [`evaluate_build_output`](../../tools/led_l1l2_checks.py#L293-L315) (`PASS`).
- **Veredicto T1:** `PASS`.

### 4.2 T2: Pruebas de preprocesador y `#error`
- 4 Controles positivos (1 por proyecto) con 0 avisos y código de salida 0.
- 16 Mutaciones negativas (4 por proyecto: sin `RED_BIT`, sin `GREEN_PORT`, sufijo `_PIN` en vez de `_BIT`, y `YELLOW_BIT` fuera de rango 0..15).
- **Veredicto T2:** `PASS` (20/20 casos comprobados).

### 4.3 T3: Comparación binaria de `g_led_config`
- Extracción de los 24 bytes de `g_led_config` en los 8 builds actuales y coincidencia exacta con el oráculo UM2408 (`words=['0x58020400', '0x0', '0x58021000', '0x1', '0x58020400', '0xe']`).
- Coincidencia al 100% con los 8 builds del árbol de control `9dc6896` compilados in situ.
- Control negativo de byte alterado rechazado con éxito (`PASS`).
- **Veredicto T3:** `PASS`.

### 4.4 Host Mock (`led_hw_check.py --mock`)
Ejecutado mediante `py 03_firmware_templates/prj_01_template_dualcore/tools/led_hw_check.py --mock`:
- Verificación nominal simulada completa (`PASS`).
- **Fallo simulado 1 obligatorio:** `lost > 0` en LT3 detectado y rechazado por `eval_lt3` (`CHECK mock_failure_lt3_real_lost_gt_0_detected: PASS`).
- **Fallo simulado 2 obligatorio:** `lost == 0` en control LT3 detectado y rechazado por `eval_lt3` (`CHECK mock_failure_lt3_ctrl_lost_eq_0_detected: PASS`).
- **Fallo simulado 3 obligatorio:** Pin en LT1 con pull activado detectado y rechazado por `eval_lt1` (`CHECK mock_failure_lt1_pin_with_pull_detected: PASS`).
- **Veredicto Mock:** `HOST VERDICT=PASS`.

---

## 5. Resultados en la Placa (Hardware Real)

Pruebas ejecutadas sobre NUCLEO-H755ZI-Q con ST-LINK serial `0016000C4142501220353451`:

### 5.1 Ejecuciones del Host `led_hw_check.py` (Dos Corridas Consecutivas)
Ambas capturas completas están archivadas en:
- **Run 1:** [`03_firmware_templates/prj_01_template_dualcore/docs/led_dev_hw_c080fcb_run1.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/led_dev_hw_c080fcb_run1.txt) (y copia en [`docs/led_dev_hw_c080fcb_run1.txt`](docs/led_dev_hw_c080fcb_run1.txt))
  - `FW VERDICT=PASS`
  - `OVERALL HARDWARE VERDICT: PASS`
  - `HOST VERDICT=PASS`
  - Procedencia: `fw_rev=c080fcb`, `host_head=c080fcb`, `build_id=024bea52952d4b14`, árbol limpio.
  - Mediciones LT3: Real `lost=0` con 6,986 ISRs solapadas; Control `lost=2155` con 7,225 ISRs solapadas.
- **Run 2:** [`03_firmware_templates/prj_01_template_dualcore/docs/led_dev_hw_c080fcb_run2.txt`](../../03_firmware_templates/prj_01_template_dualcore/docs/led_dev_hw_c080fcb_run2.txt) (y copia en [`docs/led_dev_hw_c080fcb_run2.txt`](docs/led_dev_hw_c080fcb_run2.txt))
  - `FW VERDICT=PASS`
  - `OVERALL HARDWARE VERDICT: PASS`
  - `HOST VERDICT=PASS`
  - Procedencia: `fw_rev=c080fcb`, `host_head=c080fcb`, `build_id=024bea52952d4b14`, árbol limpio.
  - Mediciones LT3: Real `lost=0` con 6,986 ISRs solapadas; Control `lost=2155` con 7,225 ISRs solapadas.

### 5.2 Ejecución de T4 con Lectura SWD de Registros Extendidos
Captura archivada en [`01_shared_drivers/01_led/docs/led_dev_t4_c080fcb.txt`](docs/led_dev_t4_c080fcb.txt):
- Flasheo de imágenes `debug` con verificación de descarga en `prj_01`, `prj_02`, `prj_03`, `prj_04` (`PASS`).
- Lectura de 6 palabras de `g_led_config` por SWD en `HOTPLUG`: Coincidencia exacta con la tabla UM2408 en los 4 proyectos (`words_match=True`).
- Lectura por SWD de los registros `MODER`, `OTYPER`, `OSPEEDR`, `PUPDR` y `ODR` de GPIOB (`0x58020400`) y GPIOE (`0x58021000`):
  - **PB0 (Verde):** `mode=1` (salida), `otype=0` (push-pull), `speed=0` (baja velocidad), `pull=0` (sin pull), `odr=0`.
  - **PB14 (Rojo):** `mode=1` (salida), `otype=0` (push-pull), `speed=0` (baja velocidad), `pull=0` (sin pull), `odr` válido.
  - **PE1 (Amarillo):** `mode=1` (salida), `otype=0` (push-pull), `speed=0` (baja velocidad), `pull=0` (sin pull), `odr` válido.
- Controles negativos de T4:
  1. Tabla de RAM mutada rechazada (`PASS`).
  2. Registro MODER mutado (PB0 en entrada) rechazado (`PASS`).
  3. Registro PUPDR mutado (PB0 con pull-up) rechazado (`PASS`).
- **Veredicto T4:** `T4 VERDICT: PASS`, `OVERALL VERDICT: PASS`.

### 5.3 Elementos NO VERIFICADOS por el Implementador
- **Resets en caliente de `prj_02`, `prj_03` y `prj_04`:** Reservados a la auditoría independiente.
- **L6 (comportamiento bajo carga pesada con tareas FreeRTOS preemptivas):** Fuera del alcance funcional de esta fase (se mantiene como observación de integración para cada proyecto).

---

## 6. Comandos de Reproducción para el Auditor

Para reproducir la verificación completa de forma independiente:

```powershell
# 1. Configurar entorno de compilación
. .\tools\env.ps1 -Quiet

# 2. Compilar y flashear el selftest en hardware
cmake --build --preset selftest-led
py tools/flash.py --preset selftest-led --sn 0016000C4142501220353451

# 3. Ejecutar el oráculo host sobre la placa
py 03_firmware_templates/prj_01_template_dualcore/tools/led_hw_check.py --sn 0016000C4142501220353451

# 4. Ejecutar la suite T4 en hardware (flasheo de debug y lectura de registros SWD)
py tools/led_l1l2_checks.py --test t4 --sn 0016000C4142501220353451

# 5. Ejecutar la suite off-board (T1-T3)
py tools/led_l1l2_checks.py --test t1,t2,t3

# 6. Dejar la placa con la imagen de plantilla de producción debug
py tools/flash.py --preset debug --sn 0016000C4142501220353451
```

---

## 7. Desviaciones Respecto al Plan

- **Desmascaramiento de `BASEPRI` antes de LT3 en CM7:**  
  *Contexto:* Al invocar `App_Log_Init()` durante el arranque inicial antes de `vTaskStartScheduler()`, la creación del mutex `xSemaphoreCreateMutex()` activa secciones críticas internas de FreeRTOS que fijan `BASEPRI = 0x50` (`configMAX_SYSCALL_INTERRUPT_PRIORITY`). Al no haber arrancado todavía el kernel, `BASEPRI` permanecía en `0x50`, lo cual enmascaraba en el núcleo Cortex-M7 cualquier interrupción configurada con prioridad numérica 6 (como la de TIM17).  
  *Solución:* Se agregó un `__set_BASEPRI(0)` explícito previo al armado del temporizador en `led_selftest.c`, garantizando la ejecución incondicional y periódica de la ISR de TIM17 durante la ventana de evaluación de concurrencia de LT3.

---

CIERRE L3-L5 ENTREGADO PARA AUDITORIA c080fcb
