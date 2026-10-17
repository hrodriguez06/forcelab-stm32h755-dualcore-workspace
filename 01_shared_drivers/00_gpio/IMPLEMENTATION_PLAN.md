# Plan de implementación de `00_gpio`: cierre del driver (K1–K3)

Oct 16, 2026 · @Hector · revisión 1

Cierra `00_gpio`. El código del driver **no cambia**: cambian los proyectos que lo llaman y la documentación.
- **K1:** la celda de compensación de `prj_02`, `prj_03` y `prj_04` pasa por `GPIO_BM_EnableCompensationCell()`.
- **K2:** EXTI y celda de compensación quedan como API de `gpio_bm` por diseño, válida desde cualquier tier.
- **K3:** el README dice el estado real del driver.

Una sola fase de implementación (F1 de este plan) y un cierre. La parte de placa (T4) la ejecuta el auditor. Las reglas para el implementador están en [`IMPLEMENTER_INSTRUCTIONS.md`](IMPLEMENTER_INSTRUCTIONS.md).

Base y control: `main` en `f066a8e`.

**El desarrollador es Antigravity.** No tiene acceso a la placa: T4 la ejecuta el auditor con la herramienta del implementador. El implementador entrega el código, la herramienta y T1–T3 sin placa.

**Banco:** el de las sesiones de `08_eth`, sin nada más:
- la Nucleo por el USB del ST-LINK (SN `0016000C4142501220353451`);
- el cable Ethernet a la red del host (`192.168.1.150`).

No hace falta el AD2 ni recablear: si sigue conectado, se deja sin abrir.

**Decisiones del responsable (2026-10-16): aprobadas las cuatro, con la opción A en 1–3** (ver [Decisiones abiertas](#decisiones-abiertas)).

**Reglas de verificación (heredadas de `08_eth` E3–E7 y de `00_gpio` F2):**
- Cada comprobación puede fallar de verdad y cuenta en el veredicto.
- Cada control negativo pasa por **la misma función** que la comprobación real, con una entrada mutada:
  - en una copia temporal fuera del repo;
  - o en el control compilado **en la misma ejecución**.

  Una constante o un atajo que no llama a esa función no es un control.
- Árbol limpio al empezar y al terminar, incluidos los archivos ignorados; solo se escribe en `build/`.
- Capturas UTF-8 de una sola ejecución, con `argv` y `HEAD`, sin edición manual.
- Cada cita (`archivo:línea`, hash, cifra) se comprueba contra `HEAD` antes de entregar.
- El texto que va a un README se escribe en inglés.

---

## Encargo

| ID | Hoy (`f066a8e`) | Qué cambia |
| --- | --- | --- |
| **K1** | `prj_02` (`CM7/Core/Src/main.c:224-229`), `prj_03` (`:236-241`) y `prj_04` (`:345-350`), al final de `SystemClock_Config()`:<br>• `__HAL_RCC_CSI_ENABLE()`;<br>• `__HAL_RCC_SYSCFG_CLK_ENABLE()`;<br>• `HAL_EnableCompensationCell()`.<br>Esa función solo pone `CCCSR.EN`, fuera de la sección y sin esperar `CSIRDY` ni comprobar `READY` | Una función local `CompensationCell_Init()` por proyecto (Diseño 1), llamada desde `main()` justo después de `SystemClock_Config()` |
| **K2** | `gpio_hal` rechaza los modos IT/EVT; `gpio_ll` no tiene EXTI. G6 figura "Open (F2)" (README §12.2) | **Sin código.** EXTI y celda son API de `gpio_bm`. `stm32h7_driver_gpio_ll` y `_hal` ya enlazan `_bm` (catálogo C4), así que un proyecto de cualquier tier las llama directamente. T3 lo demuestra y el README lo documenta (Diseño 2) |
| **K3** | El README contradice el código (lista de Diseño 3) | El README y el estado de los hallazgos, corregidos en el cierre |
| **K4** | — | `tools/gpio_close_checks.py` (nuevo): T1–T3 sin placa y T4 para el auditor |
| **K5** | — | `IMPLEMENTATION_REPORT.md` |

**Fuera de este plan:**
- `01_shared_drivers/00_gpio/*.c` y `*.h`: no cambian ni una línea.
- `prj_eth_lwip_node`: no enlaza `00_gpio` (tiene drivers privados en `Custom_Drivers/`, sin GPIO). Sigue con `HAL_EnableCompensationCell()`.
- `prj_01_template_dualcore`: su producción no enciende la celda (no tiene interfaces rápidas); solo la usa T17 de `selftest-gpio`.
- `HAL_RCC_OscConfig`, `HAL_RCC_ClockConfig`, PLL, `HAL_PWREx_ConfigSupply`, option bytes: no cambian.
- Lo no ejercitado de README §9.5 (puertos B, C, D, G, H, EXTI en otros puertos, CM4…): sigue en la lista.

---

## Fuentes normativas

- **`gpio_bm.h:409-429`:** el contrato de `GPIO_BM_EnableCompensationCell()`:
  - exige `RCC_CR.CSIRDY = 1` y `DWT->CYCCNT` contando, y si no, devuelve `ERR_NOT_READY` sin escribir;
  - pone `EN` en la sección y espera `READY` fuera, hasta `max_cycles`;
  - nunca escribe `RCC_CR`.
- **README §10:** `READY` tarda unos 76 400 ciclos tras encender CSI (76 493 y 76 385 en las corridas de cierre de F2).
- **`hsem_bm.c:95-101` (`HSEM_BM_TimebaseStart`):** la secuencia que arranca `DWT->CYCCNT` en CM7: `DEMCR.TRCENA`, el desbloqueo de `DWT->LAR` y `CYCCNTENA`.
- **Placa a `f066a8e`** (lectura SWD del auditor, 2026-10-16, imagen `prj_02` debug): `SYSCFG_CCCSR = 0x00000101` (`EN = 1`, `READY = 1`) y `RCC_CR = 0x0307C1A5` (`CSION = 1`, `CSIRDY = 1`). Hoy la celda funciona: K1 no corrige un fallo, hace que el driver lo compruebe.

---

## Diseño

### 1. K1: `CompensationCell_Init()` en cada proyecto

En `CM7/Core/Src/main.c` de `prj_02`, `prj_03` y `prj_04`:

1. **Se quitan** las tres líneas del final de `SystemClock_Config()` (`__HAL_RCC_CSI_ENABLE`, `__HAL_RCC_SYSCFG_CLK_ENABLE`, `HAL_EnableCompensationCell`). El comentario que explica por qué se usa la celda pasa a la función nueva. El resto de `SystemClock_Config()` no cambia, incluido `osc.CSIState = RCC_CSI_OFF`. Ese campo no tiene efecto, porque `OscillatorType` solo incluye HSE; se le añade un comentario de una línea que lo diga.
2. **`static void CompensationCell_Init(void)`**, llamada en `main()` entre `SystemClock_Config()` y `PeripheralClock_Config()` (las tres hoy: `prj_02` l. 86-87, `prj_03` l. 87-88, `prj_04` l. 190-191):
   1. arranca `DWT->CYCCNT` con la misma secuencia que `HSEM_BM_TimebaseStart()`, en código local. `prj_02` no enlaza `10_hsem`, y las tres copias son idénticas. Es idempotente con el `HSEM_BM_TimebaseStart()` posterior de `prj_03` y `prj_04`;
   2. `__HAL_RCC_CSI_ENABLE()`;
   3. espera `CSIRDY` hasta `COMP_CELL_CSI_MAX_CYCLES` = 800 000 ciclos de `DWT->CYCCNT` (2 ms a 400 MHz). Si no llega, llama a `Error_Handler()`;
   4. `GPIO_BM_EnableCompensationCell(COMP_CELL_MAX_CYCLES)`, con `COMP_CELL_MAX_CYCLES` = 2 000 000 (5 ms a 400 MHz, 26 veces los 76 493 medidos). Si no devuelve `GPIO_BM_OK`, llama a `Error_Handler()` (Decisión 2).
3. **Las dos constantes** son `#define` locales de `main.c`, con su cálculo en el comentario.
4. **Sin lock:** en ese punto el hook entre núcleos aún no está instalado (`Shared_GpioLockInstall()` va después) y CPU2 espera su liberación. La sección del driver es solo PRIMASK, como en cualquier llamada anterior a la instalación del hook (README §3.2). Se dice en un comentario.

**Por qué en `main()` y no en `SystemClock_Config()`:** la función cambia el orden de arranque y puede llamar a `Error_Handler()`. Fuera de `SystemClock_Config()` se ve en el `main()`, junto a las demás etapas.

### 2. K2: EXTI y celda, solo en `gpio_bm`, por diseño

- **No se añaden** funciones EXTI a `gpio_ll` ni a `gpio_hal`. Serían envoltorios de `GPIO_BM_Exti*` sin nada propio, y duplicarían pruebas de banco (T11–T14 por tier) sin un consumidor.
- `gpio_hal` **sigue rechazando** los modos `GPIO_MODE_IT_*` y `GPIO_MODE_EVT_*`, porque `HAL_GPIO_Init` reescribe `EXTICR` y `HAL_GPIO_DeInit` borra `RTSR1`/`FTSR1` compartidos (G6).
- **Un proyecto de cualquier tier** configura el pin con su tier y la línea con `GPIO_BM_ExtiConfig()`. El README añade un ejemplo F con `gpio_hal` + `gpio_bm`.
- **G6** pasa a "Closed by design (K2)".

### 3. K3: lo que el README tiene que corregir (en el cierre)

| Sitio (a `f066a8e`) | Hoy dice | Pasa a decir |
| --- | --- | --- |
| Estado, l. 28-29 | "EXTI and the compensation cell (F2) are not implemented" | Implementados en `gpio_bm` (F2); el driver, cerrado |
| §1.1, l. 111 | `EXTI, compensation cell` = no / no / no | BM: yes; LL y HAL: "via BM" (K2) |
| §9.5, primer punto | `prj_04` sin hook y G1/G13 abiertos allí | Se quita: F1b lo cerró (§13.1) |
| §9.5, último punto | Producción con `HAL_EnableCompensationCell` | `prj_02`–`prj_04` por el driver (K1); celda en CM4, sin ejercitar |
| §11 | El párrafo final y las filas de `prj_02`–`prj_04` | `GPIO_BM_EnableCompensationCell` en las tres; `prj_02` como consumidor |
| §12.2 | G6 "Open (F2)", G8 "except the compensation cell", G12 "EXTI pending" | G6 por diseño (K2), G8 cerrado (F2 + K1), G12 cerrado (F2) |
| §13 | "Open work" | "Phase records", con un §13.4 para este cierre |
| §15 | — | La fila del cierre |

Ningún otro README ni `docs/` de proyecto cita hoy la celda (comprobado a `f066a8e`).

---

## Archivos

| Archivo | Qué |
| --- | --- |
| `02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/Core/Src/main.c` | K1 |
| `02_firmware_projects/prj_03_adc_dma_eth/CM7/Core/Src/main.c` | K1 |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/CM7/Core/Src/main.c` | K1 |
| `tools/gpio_close_checks.py` (nuevo) | K4 |
| `01_shared_drivers/00_gpio/docs/gpio_close_*` | Capturas sin placa del implementador |
| `01_shared_drivers/00_gpio/IMPLEMENTATION_REPORT.md` | K5 |
| `01_shared_drivers/00_gpio/README.md` | K3, solo en el cierre |

---

## Pruebas

| ID | Qué | Criterio | Control negativo (misma función) |
| --- | --- | --- | --- |
| **T1** | Matriz: `build_all.ps1 -Clean` (8/8) y los presets `selftest-gpio` de `prj_01` y `prj_04` y `selftest` de `prj_03`, con el recuento real de avisos | 0 avisos, 0 errores, cada imagen recién escrita | Un preset compilado en la ejecución con un aviso inyectado en una copia temporal de las flags da `warnings ≥ 1` |
| **T2** | **Símbolos y objetos.** Para los CM7 de `prj_02`, `prj_03` y `prj_04` (`debug` y `release`):<br>• `main.c.obj` tiene `GPIO_BM_EnableCompensationCell` sin definir y no tiene `HAL_EnableCompensationCell`;<br>• el ELF enlazado no contiene `HAL_EnableCompensationCell`.<br>**El resto, idéntico al control** `f066a8e` compilado en la misma ejecución, comparando secciones ALLOC, contenido, relocalizaciones y símbolos (el comparador de T2 de `eth_e3e6_checks.py`):<br>• los demás objetos de los CM7 de los tres proyectos (todos salvo `main.c.obj`);<br>• todos los objetos de los CM4;<br>• los de `prj_01`.<br>**Texto:** las tres `CompensationCell_Init()` son idénticas | Todo PASS | El control `f066a8e` pasado por la misma función de símbolos da FAIL; un objeto del control con una sección mutada en una copia temporal da FAIL en el comparador; una copia temporal de un `main.c` con una línea cambiada en la función da FAIL en la comparación de texto |
| **T3** | **K2, enlace:** un `.c` temporal, fuera del repo, que llama a `GPIO_HAL_Init`, `GPIO_BM_ExtiConfig` y `GPIO_BM_EnableCompensationCell` se enlaza (`-r` o enlace completo con un `main` mínimo) contra exactamente las bibliotecas que `build.ninja` da a `stm32h7_driver_gpio_hal` | Enlaza sin símbolos sin resolver | El mismo enlace sin la biblioteca `_bm` falla con `undefined reference` a `GPIO_BM_*` |
| **T4** | **Placa (auditor).** Para `prj_02` `debug`, `prj_03` `debug` y `prj_04` `debug` y `release`, cada imagen compilada en la ejecución:<br>• flasheo con `sn=` y su verificación;<br>• tras el arranque, una sola llamada SWD `HOTPLUG` que lee `SYSCFG_CCCSR` (`0x58000420`) y `RCC_CR` (`0x58024400`);<br>• `net_check.py`.<br>En `prj_04` `debug`, además, 5 resets por NRST, con la misma lectura y un ping tras cada uno (Decisión 3) | `CCCSR.EN = 1`, `READY = 1`, `CSIRDY = 1`, `net_check` PASS, 5/5 | La función que evalúa la lectura, sobre la salida real del CLI con `READY` puesto a 0 en una copia en memoria, da FAIL; ídem con `EN = 0` |

**Lo que T4 no distingue:** el control también da `READY = 1` (la celda ya funcionaba). La prueba de que corre K1 es la identidad de la imagen (SHA-256 del ELF compilado en la ejecución y el "verify" del flasheo) más T2.

### Normas para las capturas

- **Sin placa:** `01_shared_drivers/00_gpio/docs/gpio_close_t1t3_<hash>.txt`, del implementador.
- **Con placa:** `gpio_close_audit_t4_<hash>.txt`, del auditor.
- La cabecera de cada captura lleva `argv`, `HEAD`, el árbol limpio y la ruta del worktree de control.

---

## Banco de pruebas y seguridad

- **Sin cableado nuevo.** T4 usa el ST-LINK y la red; el AD2 no se abre.
- **Las imágenes son del repo**, así que no hace falta copia de seguridad del flash.
- **Toda la recuperación es por SWD:** si una imagen se queda en `Error_Handler()` al arrancar, se recupera con *connect under reset* y se vuelve a flashear. No hace falta tocar el banco.
- **Una llamada al CLI por lectura**, con `sn=` (dos ST-LINK en el host).

---

## Fases y entregables

1. **F1 (implementador):** K1, K4, T1–T3 sin placa con capturas commiteadas, y K5. Termina con `F1 ENTREGADA PARA AUDITORIA <hash>`.
2. **Auditoría de F1:** diff, T1–T3 en ejecución propia y T4 en la placa.
3. **Cierre (implementador, cuando `AUDIT.md` lo autorice):** K3. Termina con `CIERRE ENTREGADO PARA AUDITORIA <hash>`.
4. **Cierre del responsable:** la fila de `00_gpio` en `DRIVER_ROADMAP.md` ("Hecho y cerrado"), borrar los documentos de proceso, quitar el worktree de control y el merge.

---

## Riesgos y mitigaciones

| Riesgo | Mitigación |
| --- | --- |
| `CYCCNT` parado en `prj_02` → `ERR_NOT_READY` → `Error_Handler()` en cada arranque | El paso 1 de `CompensationCell_Init()` lo arranca; T4 en `prj_02` lo comprueba |
| `CSIRDY` aún a 0 al llamar al driver | El paso 3 lo espera, acotado |
| Un cambio colado en `SystemClock_Config()` (PLL, buses) | T2: fuera de `main.c.obj` de CM7, todo idéntico al control; el diff de `main.c` lo reviso línea a línea |
| `prj_04` reinicia `DWT->CYCCNT` tras `SystemClock_Config()` (l. 194) | Pasa después de `CompensationCell_Init()`: no afecta a la espera |
| La imagen se queda en `Error_Handler()` | Recuperación por SWD (sección anterior) |

---

## Decisiones abiertas

1. **K2: EXTI y celda en LL/HAL.**
   - **A (recomendada):** por diseño, solo en `gpio_bm`, válidas desde cualquier tier. Sin código; T3 lo demuestra.
   - **B:** envoltorios en `gpio_ll` y `gpio_hal`, con sus pruebas T11–T14 por tier en el banco del AD2 (PE3/PE4). Requiere sesión de banco con el AD2 y amplía la API congelada.
2. **K1: qué hacer si la celda no llega a `READY` en el arranque.**
   - **A (recomendada):** `Error_Handler()`, como cualquier otro fallo de `SystemClock_Config()`.
   - **B:** seguir arrancando y dejar el estado en una variable para leerla por SWD. Oculta el fallo en la consola.
3. **T4: alcance en la placa.**
   - **A (recomendada):** `prj_02`, `prj_03` y `prj_04` en `debug`, `prj_04` también en `release`, y 5 resets en `prj_04` `debug`.
   - **B:** solo `prj_04` (`debug` y `release`); `prj_02` y `prj_03` quedarían solo con T2.
4. **Documentos de proceso:** se borran al cierre, como en las fases anteriores.
