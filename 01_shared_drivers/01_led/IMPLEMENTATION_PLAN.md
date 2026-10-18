# Plan de cierre de `01_led`: L3, L4 y L5

Oct 17, 2026 · @Hector (responsable) · revisión 1

Base: `main` en `0927c75`. Rama: `driver/led-close`.

---

## 1. Qué queda abierto (README §5)

| # | Limitación | Dónde |
| --- | --- | --- |
| L3 | `LED_Init_Config()` y `LED_PanicRed()` escriben `MODER` con un RMW propio, fuera de la sección de `gpio_bm` y de su lock entre núcleos | `led_bm.c:72`, `led_bm.c:152` |
| L4 | `LED_Toggle()` lee `ODR` y después escribe `BSRR`: dos contextos que alternan el mismo LED pueden leer el mismo estado. **Pasa hoy:** en `prj_02`, `prj_03` y `prj_04`, `app_main` y `net_task` alternan `LED_GREEN` desde dos tareas de CPU1 | `led_bm.c:110-133` |
| L5 | Un `pin_num` > 15 o un puerto inválido en una tabla pasada a `LED_Init_Config()` se usa tal cual: el desplazamiento sale de los 32 bits de `MODER` (comportamiento indefinido en C) | `led_bm.c:53-78` |

## 2. Decisiones (aprobadas por el responsable el 2026-10-17)

1. **L3:** `LED_Init_Config()` configura cada LED con `GPIO_BM_ConfigOutput(port, bit, GPIO_BM_OTYPE_PP, GPIO_BM_SPEED_LOW, GPIO_BM_PULL_NONE, 0)`:
   - valida el puerto y el pin antes de tocar nada;
   - escribe `BSRR` antes de `MODER`, dentro de la sección y del lock entre núcleos;
   - hace read-back.

   `OTYPER`, `OSPEEDR` y `PUPDR` pasan a escribirse con los mismos valores que tienen tras el reset (push-pull, velocidad baja, sin pull): el README §2 cambia de "no se escriben" a "se escriben con sus valores de reset".

   **`LED_PanicRed()` no cambia:** sigue escribiendo `MODER` directamente. Es la excepción aceptada G14 de `00_gpio` (el camino de fallo es terminal y no puede esperar un semáforo).
2. **L4:** `LED_Toggle()` lee `ODR` y escribe `BSRR` con las interrupciones enmascaradas, guardando y restaurando `PRIMASK` (nunca habilita lo que estaba enmascarado, así que sigue valiendo dentro de un handler).
   - Es atómico en el núcleo que llama: tareas e ISR del mismo núcleo.
   - **Entre núcleos sigue sin serlo.** Hoy ningún proyecto alterna un LED desde CM4. Queda documentado.
3. **L5:**
   - un LED cuyo `GPIO_BM_ConfigOutput` no devuelve `GPIO_BM_OK` (puerto o pin inválido, pin de SWD, pin bloqueado por `LCKR`, lock ocupado o read-back distinto) queda **deshabilitado**: su entrada en la copia interna pasa a `port = NULL`, así que `LED_Set`, `LED_Toggle`, `LED_BootStage` y `LED_PanicRed` no hacen nada con él;
   - **nueva** `uint32_t LED_Init_ConfigEx(const led_driver_config_t *cfg)`: hace lo mismo que `LED_Init_Config` y devuelve la máscara de los LED configurados (bit `i` = LED `i`). `cfg == NULL` devuelve 0 y no toca nada. `LED_Init_Config()` llama a `LED_Init_ConfigEx()` y descarta el resultado;
   - **las firmas existentes no cambian** (`01_shared_drivers/README.md`, regla 2).
4. **Placa:** el implementador puede usarla en esta fase (sección 2.2 de las instrucciones), como en `04_uart`.

## 3. Trabajo

| ID | Qué | Dónde |
| --- | --- | --- |
| H1 | L3, L4 y L5 en el driver (sección 2) | `led_bm.c`, `led_bm.h` (solo añadir `LED_Init_ConfigEx`) |
| H2 | Preset `selftest-led` de `prj_01`, solo CM7, con los pasos LT1–LT5 (sección 4) | `prj_01`: `CMakePresets.json`, opción `LED_DRIVER_SELFTEST`, `CM7/App/Src/led_selftest.c` + su cabecera, enganche en `CM7/Core/Src/main.c` bajo la opción |
| H3 | Host `prj_01/tools/led_hw_check.py` (sección 5) | Nuevo |
| H4 | `tools/led_l1l2_checks.py`: `selftest-led` en la lista de T1, y T4 lee también `OTYPER`, `OSPEEDR`, `PUPDR` y `ODR` de PB0, PE1 y PB14 | Cambios aditivos |
| H5 | Informe | `01_shared_drivers/01_led/IMPLEMENTATION_REPORT.md` |
| Cierre | README de `01_led` (§2, §3.1, §5, §6, §7) y la fila de la hoja de ruta la hace el responsable | Tras la auditoría |

## 4. El selftest (`selftest-led`, CM7)

Corre después del `LED_Init()` normal del arranque, imprime por la consola y termina con `FW VERDICT=PASS|FAIL`. **No usa el AD2 ni toca pines fuera de PB0, PE1 y PB14** (sí lee registros de GPIOA, GPIOB y GPIOE). Cada paso deja los tres LED como los deja `LED_Init()`.

**Cada paso imprime los datos medidos en crudo** (valores de registro en hex, cuentas). El firmware puede dar su propio PASS/FAIL, pero el veredicto que cuenta es el del host, calculado desde esos datos (sección 5).

| Paso | Qué prueba | Oráculo (host, sobre lo impreso) | Control en la misma ejecución |
| --- | --- | --- | --- |
| **LT1** (L3) | Los tres pines se pasan a analógico con `GPIO_BM_DeinitPin`. Se instala un hook de `GPIO_BM_SetSharedLock` que cuenta `lock()`/`unlock()` y devuelve `true`. `LED_Init_ConfigEx` con la tabla de `board_def.h`. Se imprimen la máscara, las cuentas y `GPIO_BM_ReadConfig` + `ODR` de cada pin | Máscara `0x7`; `lock` = `unlock` ≥ 3; cada pin en salida, push-pull, velocidad baja, sin pull, `ODR` = 0 | Los pines otra vez en analógico y un hook cuyo `lock()` devuelve `false`: máscara 0, los tres pines siguen en analógico (registros iguales a la instantánea previa) y `LED_Set(…, 1)` no cambia `ODR` |
| **LT2** (L5) | `LED_Init_ConfigEx` con una tabla mala: verde con `pin_num = 16`, amarillo con un puerto que no es GPIOA–GPIOK, rojo en PA13 (SWD). Instantánea de `MODER`, `OTYPER`, `OSPEEDR`, `PUPDR` y `ODR` de GPIOA, GPIOB y GPIOE antes y después, y otra vez tras `LED_Set`/`LED_Toggle`/`LED_BootStage(7)` | Máscara 0 y las 15 palabras iguales antes y después | En el mismo paso, una tabla con solo el verde válido (PB0): máscara `0x1` y la instantánea de GPIOB cambia en el campo de PB0. Así el oráculo de instantáneas puede fallar |
| **LT3** (L4) | Una ISR periódica de CM7 (TIM17, como en `gpio_selftest.c`, ≥ 100 kHz) alterna `LED_GREEN` con `LED_Toggle` y cuenta. El bucle principal hace N llamadas a `LED_Toggle(LED_GREEN)`; antes y después de cada una toma, con `PRIMASK`, la instantánea (bit de `ODR`, cuenta de la ISR). Una llamada pierde un cambio si `odr_después ≠ odr_antes ^ 1 ^ ((isr_después − isr_antes) & 1)` | `lost == 0`, con N y las interrupciones de la ventana impresos | **La misma prueba con una copia local de la `LED_Toggle` anterior** (sin `PRIMASK`, idéntica a `led_bm.c:110-133` de `0927c75`): `lost > 0`. N y la frecuencia se eligen para que el control pierda cientos o más; si en algún run el control da 0, el paso falla |
| **LT4** (G14) | Rojo en analógico, `LED_PanicRed()` | Rojo en salida y `ODR` = 1 | El verde (que `LED_PanicRed` no toca) sigue igual |
| **LT5** | Final: `LED_Init()`, el hook original (ninguno en `prj_01`) y TIM17 parado y sin pending | Los tres pines como en LT1 y `ODR` = 0 | — |

Además, la línea `[sys] build rev=… id=…` de siempre, para la procedencia.

## 5. El host (`prj_01/tools/led_hw_check.py`)

- **Argumentos:** `--sn` (por defecto el de esta placa, con el origen citado), `--port` (si falta, de `monitor.port_for_sn`), `--out`, `--allow-dirty` (no cuenta como PASS de procedencia).
- **Orden:** abre el puerto serie **antes** del reset NRST por SWD con `sn=`, y lee hasta `FW VERDICT=` o un plazo.
- **Procedencia:** árbol limpio al empezar, `HEAD` = `rev` de la imagen y el build id, como `uart_hw_check.py`.
- **Una función `eval_ltN(datos)` por paso.** El control de cada paso y una mutación de los datos medidos (por ejemplo, un `ODR` cambiado, la máscara cambiada, `lost` = 1) pasan por **esa misma función** y tienen que dar FAIL. Una línea `CHECK …: PASS|FAIL (<detalle>)` por comprobación, y `HOST VERDICT=` al final.
- **Mock:** el host se prueba sin placa con un mock de la consola que incluye al menos tres fallos que tiene que detectar: `lost > 0` en LT3, el control de LT3 con `lost = 0`, y un pin de LT1 con pull.

## 6. Pruebas sin placa

- `led_l1l2_checks.py --test t1,t2,t3`: 8/8, 10 selftests (con `selftest-led`), T2 20/20 y T3 con la tabla igual al oráculo y al control `9dc6896`. La tabla `g_led_config` no cambia.
- Avisos contados desde la salida de `cmake --build`, nunca desde `build_all.ps1`.

## 7. En la placa

- **Implementador** (evidencia de desarrollo, prefijo `led_dev_`): `led_hw_check.py` dos veces en el commit entregado, y `led_l1l2_checks.py --test t4`.
- **Auditor:** lo mismo de forma independiente, más warm resets de los `debug` de `prj_02`, `prj_03` y `prj_04` (el arranque cambia: `LED_Init` pasa por la sección de `gpio_bm`, y `prj_03` instala el hook de HSEM).

## 8. Fuera de alcance

- La copia privada de `prj_eth_lwip_node` (`Custom_Drivers/01_led/`).
- Hacer `LED_Toggle` atómico entre núcleos (necesitaría HSEM).
- L6 (parpadeo con FreeRTOS cargado): sigue siendo observación en cada proyecto.
