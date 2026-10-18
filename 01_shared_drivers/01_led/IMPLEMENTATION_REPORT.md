# Informe de Implementación: Cierre de Limitaciones L3, L4 y L5 (Driver 01_led) - Ronda 2

**Fecha:** 2026-10-18  
**Rama de trabajo:** `driver/led-close`  
**Estado:** Ronda 2 entregada para auditoría  
**Commit base:** `0927c75` (main)  
**Commit Ronda 1:** `53f03cc` (auditado en `2fd007a` con hallazgos A1–A12)  
**Commit de código/pruebas Ronda 2:** `65e5244`  
*(Nota: El commit de entrega documental posterior incorpora este informe y las 5 capturas en `01_shared_drivers/01_led/docs/` sin tocar el código ejecutable).*

---

## 1. Resumen Ejecutivo

En esta segunda ronda de entrega se corrigieron de forma integral todos los hallazgos señalados en la auditoría (`01_shared_drivers/01_led/AUDIT.md`, commit `2fd007a`, puntos A1 a A12), manteniendo intacto el código de producción del driver en `led_bm.c` (aprobado en la Ronda 1) y actualizando únicamente las cabeceras documentales en `led_bm.h` para reflejar con precisión el paso por la sección atómica de `00_gpio`, la configuración completa de registros vía `GPIO_BM_ConfigOutput`, el soporte de exclusión mutua cross-core HSEM y la anulación a `NULL` de canales en fallo.

### Secuencia completa de commits:
- **Ronda 1:**
  1. `654cf75`: `fix(led): configure LEDs through GPIO_BM_ConfigOutput (L3, L5)`
  2. `bba74a6`: `feat(led): add LED_Init_ConfigEx returning the configured mask (L5)`
  3. `3e03485`: `fix(led): make LED_Toggle atomic on the calling core (L4)`
  4. `d3a03b0`: `test(prj01): add the selftest-led preset with LT1-LT5`
  5. `db39a0f`: `test(prj01): add led_hw_check.py and its mock`
  6. `c080fcb`: `test(tools): add selftest-led and the new T4 registers to led_l1l2_checks`
  7. `53f03cc`: `docs(led): add implementation report and captures for L3-L5 closure audit`
- **Ronda 2:**
  8. `65e5244`: `fix(led): address audit findings A1-A6 and A11-A12 in selftest and tools`

Todas las pruebas fuera de placa (T1, T2, T3 y suite mock) pasaron satisfactoriamente (8/8 builds de matriz, 10 presets de selftest, 20/20 casos T2, T3 idéntico al oráculo UM2408 y al control `9dc6896`, y 6/6 comprobaciones del mock sin `--allow-dirty`). Asimismo, se ejecutó en hardware real (NUCLEO-H755ZI-Q, ST-LINK SN `0016000C4142501220353451`, VCP COM4) la suite `led_hw_check.py` en dos corridas consecutivas (`run1` y `run2`) y la suite T4 completa, resultando todas en `PASS` en árbol de trabajo estrictamente limpio. La placa queda flasheada con la imagen de desarrollo de referencia `prj_01` `debug`.

---

## 2. Detalle de Cambios en el Driver: L3, L4 y L5

### 2.1 L3: Configuración atómica vía `GPIO_BM_ConfigOutput`
- **Archivo:** [`01_shared_drivers/01_led/led_bm.c:72-76`](led_bm.c#L72-L76)
- **Antes (commit `0927c75`, líneas 64–88):**
  ```c
  void LED_Init_Config(const led_driver_config_t *config)
  {
      if (config == NULL)
      {
          return;
      }

      g_led_config = *config;

      for (uint32_t i = 0; i < (uint32_t)LED_COUNT; i++)
      {
          GPIO_TypeDef *port = g_led_config.leds[i].port;
          uint32_t bit = g_led_config.leds[i].pin_num;

          if (port != NULL)
          {
              (void)GPIO_BM_EnablePortClock(port);

              /* MODER: 0b01 = general purpose output. */
              MODIFY_REG(port->MODER, (3UL << (bit * 2U)), (1UL << (bit * 2U)));

              /* Turn off initially: active high, BSRR high half resets the pin */
              port->BSRR = (1UL << (bit + 16U));
          }
      }
  }
  ```
  En `0927c75`, `LED_Init_Config` únicamente configuraba `MODER` mediante la macro `MODIFY_REG` y escribía en `BSRR`. Nunca configuraba `OTYPER`, `OSPEEDR` ni `PUPDR` (asumiendo implícitamente valores de reset o configuración previa). No entraba en la sección atómica de `00_gpio`, no tomaba el lock hardware entre núcleos HSEM (`Shared_GpioLockInstall()`), y carecía totalmente de verificación de lectura (*read-back*).
- **Después (`65e5244`, líneas 68–76 y 94–104):**
  `LED_Init()` delega en `LED_Init_Config()` llamando a `LED_Init_ConfigEx(&g_default_led_config)`. Para cada LED, se invoca `GPIO_BM_ConfigOutput(port, bit, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 0U)`. Esta función valida el puerto (rango GPIOA–GPIOK según RM0399 §2.2 Tabla 8) y pin (0..15, rechazando pines SWD como PA13), programa `BSRR` previo a `MODER` para evitar glitches espurios, programa los 5 registros (`MODER`, `OTYPER`, `OSPEEDR`, `PUPDR`, `BSRR`), opera bajo sección crítica con PRIMASK y lock cross-core HSEM, realizando read-back completo.

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
  La lectura de `ODR` y la posterior escritura en `BSRR` estaban desprotegidas contra interrupciones. Una ISR ejecutada entre la lectura y la escritura provocaba la pérdida de la transición (*lost toggle*).
- **Después (`65e5244`, líneas 146–162):**
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
  ```c
  uint32_t LED_Init_ConfigEx(const led_driver_config_t *config);
  ```
- **Implementación:** [`01_shared_drivers/01_led/led_bm.c:53-87`](led_bm.c#L53-L87)
  - Si `config == NULL`, retorna 0 inmediatamente sin modificar el estado interno.
  - Para cada LED `i` (0 a `LED_COUNT-1`): si `GPIO_BM_ConfigOutput` retorna `GPIO_BM_OK`, se copia en la estructura interna `g_led_config` y se acumula el bit `(1UL << i)` en la máscara.
  - Si `GPIO_BM_ConfigOutput` falla (puerto nulo o inválido, pin > 15, pin de depuración SWD PA13, pin bloqueado, o fallo de lock/read-back), la entrada correspondiente en `g_led_config.leds[i].port` se anula a `NULL`.
  - Como consecuencia de la anulación a `NULL`, las funciones operativas (`LED_Set`, `LED_Toggle`, `LED_BootStage`, `LED_PanicRed`) descartan la operación de forma segura.

---

## 3. Descripción y Validación de los Pasos del Selftest (LT1–LT5)

El selftest corre en CM7 tras el `LED_Init()` del arranque. No utiliza el AD2. La consola de depuración usa USART3 en PD8/PD9 (UM2408 §6.7), mientras que las alteraciones físicas de pines en el test se restringen a PB0 (verde), PE1 (amarillo) y PB14 (rojo). En la Ronda 2, el firmware transmite telemetría cruda en hexadecimal y delega los cómputos y oráculos de decisión exclusivamente al host.

| Paso | Operación Real en Firmware | Datos Crudos Impresos en Consola | Oráculo del Host | Control Negativo en la misma Ejecución |
| --- | --- | --- | --- | --- |
| **LT1** (L3) | Comprueba `DeinitPin` y `ReadConfig` de PB0, PE1 y PB14 (modo analógico previo = 3). Instala hook de lock cross-core que cuenta llamadas y retorna `true`, llamando a `LED_Init_ConfigEx`. Restaura inmediatamente `Shared_GpioLockInstall()`. | `LT1_PRE: deinit_rc=0,0,0 read_rc=0,0,0 modes=3,3,3`<br>`LT1: mask=0x07 lock=3 unlock=3`<br>`LT1_PINS: G(...) Y(...) R(...)` (mode, otype, speed, pull, odr crudos) | Máscara `0x07`; `lock == unlock >= 3`; pines en salida (mode=1), push-pull (otype=0), baja velocidad (speed=0), sin pull (pull=0), `ODR=0`. | Pines en analógico y hook retornando `false`: máscara `0x00`. Imprime instantáneas hex de 15 palabras antes (`LT1_CTRL_PRE_A/B/E`) y después (`LT1_CTRL_POST_A/B/E`). El host evalúa `eval_snapshot_equal` (PASS) y comprueba que `ODR` no se altera. |
| **LT2** (L5) | Configuración deliberadamente inválida: verde pin 16, amarillo puerto inválido `0x58024000` (RM0399 §2.2 Tabla 8), rojo PA13 (SWD). Toma instantánea de 15 palabras antes, post-init y post-ops (`LED_Set`, `LED_Toggle`, `LED_BootStage`). | `LT2: mask=0x00`<br>`LT2_PRE_A/B/E: [15 palabras hex]`<br>`LT2_POST_INIT_A/B/E: [15 palabras hex]`<br>`LT2_POST_OPS_A/B/E: [15 palabras hex]` | Máscara `0x00`; el host aplica `eval_snapshot_equal` entre PRE y POST_INIT, y entre POST_INIT y POST_OPS, verificando que los 15 registros quedan 100% inalterados. | Tabla semi-inválida donde únicamente PB0 es válido: máscara `0x01`. Imprime `LT2_CTRL_PRE_A/B/E` y `LT2_CTRL_POST_A/B/E`. El host aplica `eval_snapshot_equal` y comprueba que la igualdad falla (se detecta la alteración del pin válido). |
| **LT3** (L4) | Salva `BASEPRI`, bit de reloj APB2 de TIM17 y prioridad NVIC. Inicia TIM17 a 200 kHz (~5 µs por ISR). La ISR alterna verde. Hilo principal ejecuta N=200,000 llamadas a `LED_Toggle`. Mide ciclos DWT (`DWT->CYCCNT`) y cuentas de ISR. Detiene TIM17 al terminar LT3 y restaura `BASEPRI`. | `LT3: N=200000 dwt_cycles=... dwt_isrs=1000 cpu_hz=400000000 window_isrs=... lost=0`<br>`LT3_CTRL: N=200000 window_isrs=... lost=...` | El host calcula `freq = (dwt_isrs * cpu_hz) // dwt_cycles` y verifica `freq >= 100000`, `window_isrs > 0` y `lost == 0`. | Misma prueba ejecutando la copia local `old_LED_Toggle` de commit `0927c75` (sin `PRIMASK`): pierde miles de transiciones (`lost > 0`). El host comprueba que el control pierde transiciones (`ctrl_lost > 0`). |
| **LT4** (G14) | Pone PB14 (rojo) en analógico. Lee campos crudos de PB0 (verde). Llama a `LED_PanicRed()`. Lee campos crudos de PB0 y PB14. | `LT4: red_mode=1 red_odr=1`<br>`LT4_GREEN_PRE: mode=1 otype=0 speed=0 pull=0 odr=0`<br>`LT4_GREEN_POST: mode=1 otype=0 speed=0 pull=0 odr=0` | PB14 en salida (mode=1) con `ODR=1`. El host aplica `eval_pin_equal` sobre verde PRE y POST certificando que no fue modificado. | — |
| **LT5** | Reconstruye la configuración de `board_def.h` llamando a `LED_Init()` y restaura `Shared_GpioLockInstall()`. Lee registros crudos de TIM17 (`CR1`, `DIER`, `NVIC_PENDING`, `APB2ENR`) y pines. | `LT5_TIM17: cr1=0x0000 dier=0x0000 nvic_pending=0 apb2enr=0x00000000`<br>`LT5_PINS: G(...) Y(...) R(...)` | El host aplica `eval_tim17_stopped` verificando `CR1==0`, `DIER==0`, `NVIC_PENDING==0` y `APB2ENR` apagado; verifica pines restaurados en salida push-pull baja velocidad sin pull con `ODR=0`. | — |

---

## 4. Resultados de Pruebas sin Placa (T1, T2, T3 y Mock)

### 4.1 T1: Matriz de compilación y presets selftest
- **8 Configuraciones de matriz:** `prj_01` (debug/release), `prj_02` (debug/release), `prj_03` (debug/release), `prj_04` (debug/release).  
  Resultado: 8/8 `PASS` (0 avisos del compilador, objetos `led_bm.c.obj` verificados).
- **10 Presets de selftest con `led_bm`:**  
  `prj_01` (`selftest-gpio`, `selftest-tim`, `selftest-spi`, `selftest-i2c`, `selftest-led`).  
  `prj_04` (`selftest`, `selftest-adc`, `selftest-uart`, `selftest-hsem`, `selftest-gpio`).  
  Resultado: 10/10 `PASS` (0 avisos del compilador).
- **Control negativo de avisos:** Compilación sintética de advertencia capturada con éxito (`PASS`).
- **Captura:** [`01_shared_drivers/01_led/docs/led_dev_t1t3_65e5244.txt`](docs/led_dev_t1t3_65e5244.txt)
- **Veredicto T1:** `PASS`.

### 4.2 T2: Pruebas de preprocesador y `#error`
- 4 Controles positivos (1 por proyecto) con 0 avisos y código de salida 0.
- 16 Mutaciones negativas (4 por proyecto: sin `RED_BIT`, sin `GREEN_PORT`, sufijo `_PIN` en vez de `_BIT`, y `YELLOW_BIT` fuera de rango 0..15).
- **Captura:** Incluida en [`01_shared_drivers/01_led/docs/led_dev_t1t3_65e5244.txt`](docs/led_dev_t1t3_65e5244.txt).
- **Veredicto T2:** `PASS` (20/20 casos comprobados).

### 4.3 T3: Comparación binaria de `g_led_config`
- Extracción de las 6 palabras de `g_led_config` en los 8 builds actuales y coincidencia exacta con el oráculo UM2408 (`words=['0x58020400', '0x0', '0x58021000', '0x1', '0x58020400', '0xe']`).
- Coincidencia al 100% con los 8 builds del árbol de control `9dc6896` compilados in situ.
- Control negativo de byte mutado en objdump rechazado con éxito (`PASS`).
- **Captura:** Incluida en [`01_shared_drivers/01_led/docs/led_dev_t1t3_65e5244.txt`](docs/led_dev_t1t3_65e5244.txt).
- **Veredicto T3:** `PASS`.

### 4.4 Host Mock (`led_hw_check.py --mock`)
Ejecutado sin `--allow-dirty`, generando líneas de consola crudas y alimentándolas al parser real:
- Caso nominal simulado: `PASS`.
- Fallo simulado 1 (LT3 `lost > 0` en firmware): detectado y rechazado por el oráculo (`PASS`).
- Fallo simulado 2 (LT3 control `lost == 0` en firmware): detectado y rechazado por el oráculo (`PASS`).
- Fallo simulado 3 (LT1 pin con pull activado): detectado y rechazado por el oráculo (`PASS`).
- Caso línea faltante (omisión de telemetría LT): detectado y rechazado (`PASS`).
- Caso instantánea alterada en LT2: detectado y rechazado por `eval_snapshot_equal` (`PASS`).
- **Captura:** [`01_shared_drivers/01_led/docs/led_dev_mock_65e5244.txt`](docs/led_dev_mock_65e5244.txt).
- **Veredicto Mock:** `HOST VERDICT=PASS` (6/6 comprobaciones aprobadas).

---

## 5. Resultados en la Placa (Hardware Real)

Pruebas ejecutadas sobre NUCLEO-H755ZI-Q con ST-LINK serial `0016000C4142501220353451` en árbol estrictamente limpio (sin `--allow-dirty`):

### 5.1 Ejecuciones del Host `led_hw_check.py` (Dos Corridas Consecutivas en Árbol Limpio)
Archivadas exclusivamente en:
- **Run 1:** [`01_shared_drivers/01_led/docs/led_dev_hw_65e5244_run1.txt`](docs/led_dev_hw_65e5244_run1.txt)
  - `FW VERDICT=PASS`, `OVERALL HARDWARE VERDICT: PASS`, `HOST VERDICT=PASS`
  - Procedencia: `fw_rev=65e5244`, `host_head=65e5244`, `fw_id=38ca718e36826aef`, `host_id=38ca718e36826aef`, árbol limpio (`tree_clean: PASS`).
  - Mediciones LT3: `N=200000`, `dwt_cycles=2000129`, `dwt_isrs=1000`, `cpu_hz=400000000`, frecuencia calculada por el host = `199,987 Hz`. Real `lost=0` con 6,992 ISRs solapadas; Control `lost=2081` con 6,974 ISRs solapadas.
  - Oráculos universales: `eval_snapshot_equal` aprobó LT1_CTRL (PRE vs POST) y LT2 (PRE vs POST_INIT vs POST_OPS); rechazó LT2_CTRL. `eval_pin_equal` aprobó LT4 verde inalterado. `eval_tim17_stopped` aprobó parada total de TIM17. Mutaciones negativas en el host rechazadas (10/10 `PASS`).
- **Run 2:** [`01_shared_drivers/01_led/docs/led_dev_hw_65e5244_run2.txt`](docs/led_dev_hw_65e5244_run2.txt)
  - `FW VERDICT=PASS`, `OVERALL HARDWARE VERDICT: PASS`, `HOST VERDICT=PASS`
  - Procedencia: `fw_rev=65e5244`, `host_head=65e5244`, `fw_id=38ca718e36826aef`, `host_id=38ca718e36826aef`, árbol limpio (`tree_clean: PASS`).
  - Determinismo idéntico: Frecuencia calculada `199,987 Hz`, `window_isrs=6992`, `lost=0`, control `lost=2081`.

### 5.2 Ejecución de T4 con Lectura SWD en Árbol Limpio
Captura archivada en [`01_shared_drivers/01_led/docs/led_dev_t4_65e5244.txt`](docs/led_dev_t4_65e5244.txt):
- Compilación y flasheo de imágenes `debug` con verificación de descarga en `prj_01`, `prj_02`, `prj_03`, `prj_04` (`PASS`).
- Lectura de 6 palabras de `g_led_config` por SWD en `HOTPLUG`: Coincidencia exacta con UM2408 en los 4 proyectos (`words_match=True`).
- Lectura por SWD de los registros `MODER`, `OTYPER`, `OSPEEDR`, `PUPDR` y `ODR` de GPIOB (`0x58020400`) y GPIOE (`0x58021000`):
  - **PB0 (Verde):** `mode=1` (salida), `otype=0` (push-pull), `speed=0` (baja velocidad), `pull=0` (sin pull), `odr=0`.
  - **PB14 (Rojo):** `mode=1` (salida), `otype=0` (push-pull), `speed=0` (baja velocidad), `pull=0` (sin pull), `odr=1`.
  - **PE1 (Amarillo):** `mode=1` (salida), `otype=0` (push-pull), `speed=0` (baja velocidad), `pull=0` (sin pull), `odr=1`.
- Controles negativos de T4 mutando telemetría real medida:
  1. Telemetría de RAM mutada rechazada (`PASS`).
  2. Telemetría del registro `MODER` mutada (PB0 puesto en entrada) rechazada (`PASS`).
  3. Telemetría del registro `PUPDR` mutada (PB0 con pull-up activado) rechazada (`PASS`).
- **Veredicto T4:** `T4 VERDICT: PASS`, `OVERALL VERDICT: PASS`.

### 5.3 Elementos Exclusivamente Reservados / NO VERIFICADOS por el Implementador
- **Resets en caliente de `prj_02`, `prj_03` y `prj_04`:** `NO VERIFICADO por el implementador` (reservado por protocolo a la auditoría independiente con herramientas específicas).
- **L6 (comportamiento bajo carga pesada con tareas FreeRTOS preemptivas):** `NO VERIFICADO por el implementador` (fuera del alcance funcional de esta fase).

---

## 6. Comandos de Reproducción para el Auditor

Para reproducir la verificación completa de forma independiente en un entorno PowerShell:

```powershell
# 1. Posicionarse en el directorio del proyecto
cd 03_firmware_templates\prj_01_template_dualcore

# 2. Configurar el entorno de compilación
. ..\..\tools\env.ps1 -Quiet

# 3. Configurar los presets de CMake
cmake --preset selftest-led
cmake --preset debug

# 4. Compilar y flashear el selftest en hardware
cmake --build --preset selftest-led
py tools/flash.py --preset selftest-led --sn 0016000C4142501220353451

# 5. Ejecutar la primera corrida de comprobación en placa (run1)
py tools/led_hw_check.py --sn 0016000C4142501220353451 --out ../../01_shared_drivers/01_led/docs/led_audit_hw_run1.txt

# 6. Ejecutar la segunda corrida de comprobación en placa (run2)
py tools/led_hw_check.py --sn 0016000C4142501220353451 --out ../../01_shared_drivers/01_led/docs/led_audit_hw_run2.txt

# 7. Ejecutar la suite host mock
py tools/led_hw_check.py --mock

# 8. Regresar a la raíz y ejecutar la suite T4 en hardware
cd ..\..
py tools/led_l1l2_checks.py --test t4 --sn 0016000C4142501220353451 -o 01_shared_drivers/01_led/docs/led_audit_t4.txt

# 9. Ejecutar las pruebas off-board (T1-T3)
py tools/led_l1l2_checks.py --test t1,t2,t3 -o 01_shared_drivers/01_led/docs/led_audit_t1t3.txt

# 10. Restaurar la placa con la imagen de desarrollo de referencia (prj_01 debug)
cd 03_firmware_templates\prj_01_template_dualcore
py tools/flash.py --preset debug --sn 0016000C4142501220353451
cd ..\..
```

---

## 7. Desviaciones y Adaptaciones Respecto al Plan

1. **Adaptaciones de evidencia A1–A4 (Auditoría Ronda 2):**
   - El firmware imprime en crudo las 15 palabras hex de cada instantánea (`LT1_CTRL_PRE/POST`, `LT2_PRE/POST_INIT/POST_OPS`, `LT2_CTRL_PRE/POST`), los campos de registro de pines en `LT1_PINS`, `LT4_GREEN_PRE/POST` y `LT5_PINS`, y los registros de TIM17 en `LT5_TIM17`.
   - Se crearon oráculos universales en el host (`eval_snapshot_equal`, `eval_pin_equal`, `eval_tim17_stopped`) aplicados de forma simétrica a los pasos reales y a los controles negativos.
   - La frecuencia de la ISR de TIM17 se calcula en el host dividiendo la cuenta de ISRs entre los ciclos hardware medidos con `DWT->CYCCNT`.
   - El hook cross-core de hardware (`Shared_GpioLockInstall()`) se preserva y restaura inmediatamente en LT1 y LT5 para no dejar desprotegido al núcleo CM4.
2. **Reconstrucción de tabla interna en `LED_Init()`:**
   - En lugar de limitarse a reaplicar los punteros presentes en `g_led_config` (que pudieron haber sido anulados a `NULL` durante pruebas de error o fallos previos), `LED_Init()` re-inicializa llamando a `LED_Init_Config(&g_default_led_config)`. Esto asegura que una invocación a `LED_Init()` re-arme los pines válidos de `board_def.h`, necesario para LT5 y coherente con el README §2.
3. **Copia local de `old_LED_Toggle` en `led_selftest.c`:**
   - Para la prueba de concurrencia de LT3, se incorporó una función estática local `old_LED_Toggle()` que reproduce la implementación previa de commit `0927c75` (sin `PRIMASK`), vinculada a la configuración de selftest. Esto permite cuantificar objetivamente la pérdida de alternancias bajo interrupciones a 200 kHz frente a la versión atómica.
4. **Resguardo y restauración de `BASEPRI`, reloj APB2 de TIM17 y prioridad NVIC:**
   - En LT3 se resguarda el valor inicial de `BASEPRI` antes de forzarlo a 0 para permitir las interrupciones del timer, y se restaura al salir. Asimismo, se conserva el estado del reloj en `RCC->APB2ENR` y la configuración de prioridad en el NVIC para no alterar el entorno del sistema tras el selftest.
5. **Ubicación de parada de TIM17:**
   - La detención activa del temporizador TIM17 (`CR1 = 0`, `DIER = 0`) se efectúa al concluir la ventana de medición de LT3, mientras que LT5 comprueba y certifica de forma pasiva que dicho periférico permanece 100% detenido e inactivo antes del retorno al hilo principal.
6. **Dirección base de prueba GPIOK:**
   - Para la prueba de puerto fuera de rango en LT2 se utiliza `0x58024000`, la cual corresponde al inicio del espacio de memoria reservado inmediatamente posterior a GPIOK (`0x58022800`), según lo especificado en la Tabla 8 del RM0399 §2.2.

---

RONDA 2 ENTREGADA PARA AUDITORIA 65e5244
