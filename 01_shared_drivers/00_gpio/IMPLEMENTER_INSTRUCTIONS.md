# Instrucciones para el implementador de 00\_gpio F1–F2

Sep 26, 2026 · @Hector

Vas a implementar el plan [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 2) de esta carpeta: el núcleo BM de `gpio_bm` (F0, F1a, F1b) y EXTI + SYSCFG (F2). Otra persona audita cada fase de forma independiente y repite tus pruebas en la placa.

Este documento dice qué te toca, qué no te toca y qué tienes que entregar.

> **F0 está en corrección.** La auditoría de F0 (`AUDIT.md`) no pasó. Qué corregir y en qué orden está en [`F0_CORRECTION_INSTRUCTIONS.md`](F0_CORRECTION_INSTRUCTIONS.md). No empieces F1a hasta que la reauditoría de F0 pase. Si algo de aquí choca con el plan, manda el plan. Una excepción: lo que este documento te **prohíbe** sigue prohibido aunque el plan lo mencione.

---

## 1. Tu papel, y lo que no es tu papel

**Tú implementas y pruebas. No auditas.**

- **No hagas auditorías, ni de tu trabajo ni del de nadie.** No escribas "auditoría", "auditado", "aprobado", "cerrado", "validado por auditoría" ni nada parecido para describir una fase o un hallazgo. Lo más que puede decir tu trabajo de sí mismo es **"entregado para auditoría"**.
- **No crees, edites ni borres `00_gpio/AUDIT.md`.** Lo escribe solo el auditor. Si no estás de acuerdo con un hallazgo, contéstalo en tu informe (§5.6), no en ese archivo.
- **No marques un hallazgo como cerrado.** Un hallazgo (`G1`, `B1`, `M1`, `m1`…) se cierra cuando el auditor lo verifica. Tu commit dice que lo *corrige*, no que lo cierra.
- **No rellenes la tabla A1–A27** del protocolo de auditoría del plan. Es la lista de lo que el auditor comprobará, y te sirve para saber qué va a mirar. No es un formulario tuyo.
- **No empieces la fase siguiente** hasta que el auditor haya dejado en `AUDIT.md` que la fase actual pasa. Mientras esperas, corrige lo que te pida.

Este reparto no es una formalidad. En `10_hsem`, las auditorías de fase las escribió el implementador y el informe se declaraba "auditado y cerrado al 100 %". La auditoría independiente las anuló todas: el hallazgo B6 (`git show 6f7af43:01_shared_drivers/10_hsem/AUDIT.md`) cuenta por qué.

---

## 2. Archivos que no puedes modificar

| Archivo | Por qué |
| --- | --- |
| `01_shared_drivers/DRIVER_ROADMAP.md` | **No lo toques en ninguna fase, tampoco en el cierre.** El estado de `00_gpio` en la hoja de ruta lo actualiza el responsable del catálogo después de la auditoría. La fila "Cierre" del plan que lo menciona no es tarea tuya. |
| `00_gpio/AUDIT.md` | Solo el auditor (§1). |
| `00_gpio/IMPLEMENTATION_PLAN.md` | Es la norma que se te audita. Si encuentras un error en el plan, apúntalo en tu informe (§5.7) y avisa. No lo corrijas tú. |
| `00_gpio/IMPLEMENTER_INSTRUCTIONS.md` | Este archivo. |
| `10_hsem/hsem_*.c`, `10_hsem/hsem_*.h` | Driver cerrado y auditado. Lo usas; no lo cambias. Si necesitas algo de él, para y pregunta. |
| `02_firmware_projects/prj_02_*`, `prj_03_*`, `prj_eth_lwip_node` | El gate de F1b exige `git diff` vacío en ellos. |
| Capturas ya commiteadas (`prj_04/docs/*.txt`) | Una captura no se edita nunca. Una ejecución nueva va en un archivo nuevo. |

Tampoco puedes:

- **Cambiar las firmas ni las constantes existentes** de `gpio_bm.h` (`GPIO_BM_EnablePortClock`, `GPIO_BM_ConfigAlternate`, `GPIO_BM_ConfigAnalog`, `GPIO_BM_PULL_*` y `GPIO_BM_SPEED_*`). Desde que el auditor pase F1a, tampoco las de la API nueva.
- **Rebajar un criterio, un gate o una prueba** del plan. Si una prueba no se puede hacer como está escrita, para, explícalo en el informe y espera respuesta.
- **Hacer merge a `main`, borrar la rama, reescribir historia ya entregada** (`rebase`, `commit --amend`, `push --force`) ni borrar el plan, `AUDIT.md` o tu informe. El merge y el borrado de los documentos de proceso vienen después del cierre de la auditoría, y no los haces tú.

---

## 3. Cómo trabajar

### 3.1 Rama y commits

- Todo va en la rama **`driver/gpio`**, creada desde `main` en `fb38ebe` o posterior.
- **Un commit por preocupación,** con el estilo del repo:
  - prefijos `build(gpio)`, `feat(gpio)`, `fix(gpio)`, `fix(eth)`, `fix(uart)`, `refactor(prj_04)`, `test(prj_04)` y `docs(gpio)`;
  - asunto en inglés e imperativo;
  - el porqué en el cuerpo;
  - el ID del hallazgo entre paréntesis: `fix(gpio): reject reserved PUPDR value 11 (G3)`.
- Los cambios en `08_eth` y `04_uart` van en **commits propios y mínimos** (F1b, decisión abierta 3 del plan). `08_eth` tiene trabajo en curso (E1/E2): no mezcles nada más en esos commits.
- **Escribe los mensajes de commit desde bash** con `git commit -F - <<'EOF' … EOF`. Desde PowerShell, `$msg | git commit -F -` mete un BOM al principio del asunto.
- Cada entrega es un rango de commits con el árbol limpio. Si el repositorio tiene remoto, haz push de `driver/gpio`. Si no, deja la rama en el repositorio de trabajo: tiene que existir como rama para que su historia sea visible, no como un fast-forward sobre `main`.

### 3.2 Decisiones abiertas del plan

Usa la opción por defecto de cada una (plan, "Decisiones abiertas" 1–6). Si crees que otra opción es mejor, **para y pregunta** antes de implementarla. En el informe, di cuál usaste.

### 3.3 Entorno

- **Antes de cualquier build,** ejecuta `.\tools\env.ps1` en PowerShell: cmake, ninja y gcc están dentro de los directorios de STM32CubeIDE. Matriz: `.\tools\build_all.ps1 -Clean`.
- **Python:** usa `py`, no `python`. Solo biblioteca estándar; el AD2 se maneja por ctypes sobre `dwf.dll`, sin pip.
- **Analog Discovery 2 (SN 210321B5D38D):** cierra la GUI de WaveForms antes de ejecutar los scripts, porque el SDK solo abre un dispositivo libre. El cableado es el de la sección "Banco de pruebas" del plan y **va directo, sin resistencias en serie**. Lee §3.6 antes de escribir una sola línea del script o del selftest.
- **Depurador:** no leas con el depurador el banco `RLR` del HSEM (`0x58026480`–`0x580264FC`), porque leerlo toma los semáforos (`10_hsem/README.md` §12). Las capturas de prueba se hacen **en frío y sin depurador conectado**.
- **Placa colgada después de un cambio:** conecta con "connect under reset" y lee `MPU_CTRL` y `SCB_CFSR` antes de nada.

### 3.4 Capturas (evidencia)

Si una captura no cumple **todo** esto, no vale como evidencia:

1. Es la salida de **una sola ejecución** de `prj_04/tools/gpio_hw_check.py`, con marcas de tiempo, hasta `HOST VERDICT=`.
2. Los dos núcleos imprimen `Rev <hash>`, ese hash es el del commit entregado y **no lleva `-dirty`**. Commitea antes de compilar la imagen que vas a capturar.
3. **No se edita.** Ni recortar, ni resumir, ni corregir a mano. Si hay que repetir, la nueva ejecución va en un archivo nuevo, con su hash.
4. Los números que cites en el informe salen de una captura o de un `.map` que esté en el repo. Una línea escrita a mano como "300/300 OK" no es evidencia: tiene que ser la salida cruda de `query_adc.py`.
5. Nombres: `prj_04_adc_dma_bm_eth/docs/gpio_<fase>_<prueba>_<hash>.txt` (por ejemplo `gpio_f1a_selftest_1a2b3c4.txt`).

### 3.5 Pruebas

- **Cada prueba imprime el valor de su control negativo.** Si el control no reproduce el peligro, la prueba **falla**, aunque la parte positiva salga bien. El caso típico es T15: si el bucle sin lock no pierde, hay que ensanchar la ventana. Nunca se da por buena.
- **El oráculo no puede ser el driver.** Usa lo que dice el plan en cada fila: el AD2, la imagen de `HAL_GPIO_Init`, contadores privados o fotos de registros.
- **Las pruebas no son destructivas (R4):**
  - solo los pines del "Banco de pruebas" del plan (GPIOE salvo PE1, PF6–PF11, PF14, PF15 y PA8) y las líneas EXTI 0, 3 y 4;
  - **nunca PE1**, que es el LED LD2; **nunca la línea 15**, que la usa H5c de HSEM; **nunca PC13**;
  - guarda y restaura `RCC_CFGR` (T10), y `CSION` y `CCCSR` (T17);
  - `LCKR` solo en PF7–PF9, y como última prueba de la imagen.
- **No cambies el cableado del plan sin preguntar.** Está sacado de UM2408 Rev 6, tablas 17 y 19. En esta placa, PF0–PF5, PF12 y PF13 no llegan a ningún conector.
- **El veredicto se calcula,** nunca se imprime sin más. CM7 lo imprime cuando CM4 ha terminado, e incluye los resultados de CM4.

### 3.6 Seguridad eléctrica: sin resistencias en serie

Entre el AD2 y la Nucleo no hay ninguna resistencia. Si tu código hace que el MCU y el AD2 conduzcan la misma línea a la vez, o que el AD2 conduzca con la placa sin alimentar, **se puede dañar el hardware**, y no siempre de forma visible. Por eso las reglas S1–S10 del plan ("Seguridad eléctrica (obligatoria)") no son recomendaciones:

- **Impleméntalas antes que ninguna prueba,** en F0: `ad2_set_oe()` como único camino a `FDwfDigitalIOOutputEnableSet`, `bench_claim_output()` como único camino a `MODER`/`OTYPER`/`AFR` de un pin cableado, `bench_safe()` en el firmware, el estado seguro S2 en `try/finally`, `DwfParamOnClose = 1`, `gpio_bench_protocol.h` con la tabla de pasos y la comprobación del banner (S8).
- **T0b va antes que todo lo demás.** Hasta que el auditor la vea en verde en su propia ejecución, no ejecutes ninguna prueba que mueva el bus PE8–PE15. T0a (líneas sueltas y cableado) se repite al empezar **cada** sesión.
- **Nunca pruebes una guarda provocando una contienda de verdad.** El control de T0b(c) está diseñado para que el MCU conduzca el bus mientras el AD2 está en **entrada**: sin riesgo. Si se te ocurre otra forma de probar una guarda, pregunta antes.
- **No cambies la dirección de ninguna línea "solo MCU" / "solo AD2"** de la tabla de cableado, ni añadas líneas, ni conectes W1, W2, V+ o V−. Las resistencias de 22 kΩ de PF14/PF15 van **a un riel** (GND y 3V3), nunca en serie con el pin; no las muevas ni añadas componentes.
- **No flashees ni resetees con el script corriendo** (S10). Primero paras el script (AD2 en S2) y luego flasheas.
- **Si algo sale raro** (una lectura inesperada en la prueba de sueltas, un `ABORT`, un pin que se calienta, el LED de la Nucleo que se apaga), **para, deja el AD2 en S2, desconecta las DIO** y apúntalo en el informe (§5.8) antes de volver a intentarlo.
- En la captura, la lista de comprobación del "Procedimiento operativo" del plan va al principio, marcada.

El auditor revisa todo esto en el punto A27. Un incumplimiento es **bloqueante** aunque todas las pruebas pasen, y el auditor no conecta el AD2 a tu imagen hasta que esté corregido.

---

## 4. Qué entregar al terminar cada fase

1. El rango de commits en `driver/gpio`, con el árbol limpio.
2. La salida completa de `.\tools\build_all.ps1 -Clean`: 8/8 con `-Werror`.
3. Los tamaños **del `.map` enlazado**, sin contar "Discarded input sections": `.text`, `.data` y `.bss` de `gpio_bm.o` y el total de CM4 y CM7 release, frente a F0.
4. Las capturas de la fase (§3.4), commiteadas.
5. Los fragmentos de objdump y `.map` que pide la fase (por ejemplo, el `str` único de `GPIO_BM_Set` y el `.bss` de `gpio_bm.o`).
6. **Tu informe actualizado** (§5), con la sección de la fase.
7. Un aviso al auditor con el hash del último commit. Y a esperar.

Cuando el auditor deje hallazgos:

- Corrígelos en commits nuevos que citen el ID (`fix(gpio): … (M1)`).
- Añade al informe una sección "Respuesta a la auditoría n" que diga qué commit corrige cada hallazgo y qué ejecutaste para comprobarlo.
- **No digas que un hallazgo está cerrado:** di que está "corregido en `<hash>`, pendiente de verificación".

---

## 5. El informe: `00_gpio/IMPLEMENTATION_REPORT.md`

Es obligatorio. Lo creas en F0 y le añades una sección por fase y por cada respuesta a una auditoría. **Las secciones ya entregadas no se reescriben.** Si algo que dijiste era falso, lo corriges en una sección nueva que diga qué estaba mal.

Reglas de redacción:

- **Hechos, no valoraciones.** "T15: 0 corrupciones en 1 000 000 llamadas; control sin lock: 41 233" sí. "El driver es robusto y está validado" no.
- **Cada número lleva su fuente:** el archivo de captura con su hash, o el `.map`.
- **Nada de palabras de auditoría** (§1). El estado de una fase en tu informe solo puede ser "en curso" o "entregada para auditoría (`<hash>`)".
- **Lo que falló y lo que no se pudo ejercitar va tan visible como lo que salió bien.**

### Plantilla

```markdown
# Informe de implementación 00_gpio F1–F2

Implementador: <nombre> · Rama: driver/gpio · Plan: IMPLEMENTATION_PLAN.md revisión 2

## Resumen
<5-10 líneas: qué hay implementado, en qué fase estás, qué falta y qué está bloqueado.
 Sin valoraciones.>

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F0  | entregada para auditoría | `abc1234` | `docs/gpio_f0_…_abc1234.txt` |
| F1a | en curso | — | — |

## F0 (y una sección igual para F1a, F1b y F2)

### 5.1 Commits
| Commit | Asunto | Hallazgo |

### 5.2 Build y tamaños
- build_all -Clean: 8/8 (salida en <archivo>)
| Imagen | .text | .data | .bss | Δ frente a F0 | Fuente (.map) |
- gpio_bm.o: .text / .data / .bss

### 5.3 Pruebas
| ID | Resultado (PASS/FAIL) | Valor medido | Control negativo: valor | Oráculo | Captura |
(Todas las pruebas de la fase, también las que fallaron.)

### 5.4 Medidas (B1 en F1a)
Ciclos de ConfigPin con y sin hook; tiempo máximo con PRIMASK y con el lock por
núcleo; límite SHARED_GPIO_LOCK_US elegido y margen, con la cuenta.

### 5.5 Desviaciones del plan
| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién/cuándo? |
(Si no hay ninguna, escribe "Ninguna".)

### 5.6 Respuesta a la auditoría n (solo si la hay)
| Hallazgo | Commit que lo corrige | Qué ejecuté para comprobarlo | Estado: "corregido, pendiente de verificación" |
Desacuerdos, con argumentos y fuente (RM0399, ES0445, código).

### 5.7 Erratas del plan encontradas
| Sección del plan | Qué dice | Qué es correcto | Fuente |

### 5.8 Problemas abiertos y no ejercitado
- Lo que no funciona o no está terminado.
- Lo que no se pudo probar en la placa y por qué.

## Decisiones abiertas usadas
| # | Opción usada | Consultada |

## Mapa de escritores (F1b)
El mapa del plan con una columna nueva: commit que migra cada escritor, o
"excepción documentada" (G14).
```

---

## 6. Resumen por fase de lo que te toca

La lista completa de entregables y gates está en la tabla "Fases" del plan. Aquí van los puntos donde es fácil equivocarse.

- **F0:** lo primero son las guardas de seguridad (§3.6) y T0b; sin eso, nada que use el AD2 más allá de T0a. Aún no tocas `gpio_bm.c`. El control de T15 tiene que salir con **> 0 corrupciones** usando RMW a mano, y la referencia de T18 se captura **antes** de cualquier cambio. Estos son los binarios que se comparan después.
- **F1a:**
  - En `gpio_bm.h` solo se añade. Compruébalo tú con `git diff ca569b6 -- 01_shared_drivers/00_gpio/gpio_bm.h` antes de entregar.
  - B1 se mide aquí, y de él sale el límite del lock de F1b.
- **F1b:**
  - El hook vive en el proyecto (`prj_04/Common/Src/shared_memory.c` y la plantilla), no en `gpio_bm`.
  - Cada `HAL_GPIO_Init` se envuelve pin a pin, no el bucle entero.
  - El mapa de escritores del informe tiene que cuadrar con un `git grep` de `HAL_GPIO_Init` y `->MODER|->PUPDR|->OTYPER|->OSPEEDR|->AFR`.
- **F2:**
  - La API EXTI solo toca las máscaras **del núcleo que llama**.
  - El driver no define ningún `EXTIx_IRQHandler`; los de la prueba viven en `gpio_selftest.c`.
- **Después de F2:** si el auditor da la fase por buena, **te pedirá** el README de cierre y la línea del README del catálogo. No lo adelantes, y en ningún caso toques `DRIVER_ROADMAP.md`.

---

## 7. Cuándo parar y preguntar

Para y pregunta, sin seguir por tu cuenta, si:

- una prueba no se puede hacer como dice el plan (por hardware, porque el control no reproduce el peligro o por R4);
- una prueba parece necesitar que el MCU y el AD2 conduzcan la misma línea, o tocar una línea con una dirección distinta de la de la tabla (S1);
- necesitas cambiar una firma, un criterio C, un gate o algo de `10_hsem`;
- el cambio en `08_eth` choca con el trabajo de E1/E2;
- quieres elegir una opción distinta de la de por defecto en una decisión abierta;
- encuentras un error en el plan que cambia lo que hay que construir.

Apunta la pregunta en §5.5 o §5.7 del informe para que quede registrada.
