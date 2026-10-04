# Instrucciones para el implementador de 06\_spi

Oct 3, 2026 · @Hector

Vas a implementar [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 2) de esta carpeta. El trabajo es el driver SPI en tres tiers, `spi_bm` (F1), `spi_ll` (F2) y `spi_hal` (F3), con una fase F0 que prepara el banco en `prj_01_template_dualcore`: el AD2 de siempre más un puente nuevo, J1. Otra persona audita cada fase de forma independiente y repite tus pruebas en la placa.

Este documento dice qué te toca, qué no te toca y qué tienes que entregar. Si algo de aquí choca con el plan, manda el plan, con una excepción: lo que este documento te **prohíbe** sigue prohibido aunque el plan lo mencione.

**El encargo, en tres frases** (texto completo en el plan, "Encargo"):

1. Driver de catálogo con BM, LL y HAL.
2. Se valida **solo en `prj_01_template_dualcore`**, con el cableado del AD2 que ya existe y el puente J1. No validas ni corriges ningún otro proyecto.
3. Al final, un `README.md` limpio y conciso, con el formato de los drivers terminados.

---

## 1. Tu papel, y lo que no es tu papel

**Tú implementas y pruebas. No auditas.**

- **No hagas auditorías, ni de tu trabajo ni del de nadie.** No escribas "auditoría", "auditado", "aprobado", "cerrado", "validado por auditoría" ni nada parecido para describir una fase o un hallazgo. Lo más que puede decir tu trabajo de sí mismo es **"entregado para auditoría"**.
- **No crees, edites ni borres `06_spi/AUDIT.md`.** Lo escribe solo el auditor. Si no estás de acuerdo con un hallazgo, contéstalo en tu informe (§5.6).
- **No marques un hallazgo como cerrado.** Se cierra cuando el auditor lo verifica. Tu commit dice que lo *corrige*.
- **No rellenes la tabla A1–A28** del plan. Es lo que va a mirar el auditor.
- **No empieces la fase siguiente** hasta que el auditor deje en `AUDIT.md` que la actual pasa.

En `10_hsem`, las auditorías de fase las escribió el implementador, y la auditoría independiente las anuló todas (`git show 6f7af43:01_shared_drivers/10_hsem/AUDIT.md`, hallazgo B6). En `03_tim`, la ejecución propia del auditor encontró dos fallos que las entregas no mostraban (`03_tim/README.md` §13).

---

## 2. Qué puedes tocar y qué no

### 2.1 Lo único que puedes modificar

| Ruta | Qué |
| --- | --- |
| `01_shared_drivers/06_spi/spi_{bm,ll,hal}.{h,c}` | El driver. El `.gitkeep` lo borras en el commit que añade el primer fuente |
| `01_shared_drivers/06_spi/IMPLEMENTATION_REPORT.md` | Tu informe (§5) |
| `01_shared_drivers/06_spi/README.md` | Solo en el cierre, cuando el auditor lo pida (§6.1) |
| `01_shared_drivers/CMakeLists.txt` | Solo los tres targets `_spi_*` del plan |
| `01_shared_drivers/README.md` | Solo la línea de `06_spi` del árbol, en el cierre |
| `03_firmware_templates/prj_01_template_dualcore/**` | Lo que el plan lista en "Archivos, dependencias y targets", nada más. En `CM7/Core/Inc/stm32h7xx_hal_conf.h`, solo `HAL_SPI_MODULE_ENABLED` y su `#include`, dentro de `#if defined(SPI_DRIVER_SELFTEST)`, en F3 |
| `tools/ad2.py` | Solo añadir (por ejemplo, el decodificador de SPI sobre la captura LA) |

Todo lo demás está fuera. El auditor lo comprueba con `git diff 8901b77 --stat` (A23).

### 2.2 Prohibido expresamente

| Archivo o acción | Por qué |
| --- | --- |
| `02_firmware_projects/**` | Encargo. `git diff 8901b77 -- 02_firmware_projects` tiene que salir vacío |
| `01_shared_drivers/DRIVER_ROADMAP.md`, `02_dma/README.md`, `00_gpio/README.md` | Los actualiza el responsable del catálogo al cierre (la línea de BDMA de `02_dma` §9 y la mención de J1 en `00_gpio` §8 incluidas) |
| `06_spi/AUDIT.md`, `06_spi/IMPLEMENTATION_PLAN.md`, `06_spi/IMPLEMENTER_INSTRUCTIONS.md` | El plan es la norma que se te audita. Si tiene un error, lo apuntas en el informe (§5.7) y avisas |
| `00_gpio/*`, `02_dma/*`, `03_tim/*`, `04_uart/*`, `10_hsem/*` (fuentes y README) | Drivers cerrados. Los usas; no los cambias. Si te falta algo (por ejemplo, BDMA en `dma_bm`), para y pregunta |
| Producción y evidencia de otros drivers en `prj_01`: `stm32h7xx_hal_timebase_tim.c`, `stm32h7xx_it.c`, `mpu_config.c`, los linker scripts, `board_def.h`, `shared_memory.[ch]`, `gpio_selftest*`, `tim_selftest*`, `gpio_hw_check.py`, `tim_hw_check.py` | En `main.c` y `app_main.c` solo añades llamadas dentro de `#if defined(SPI_DRIVER_SELFTEST)`. El hallazgo SP1 del plan (comentarios que citan el plan borrado de `03_tim`) no es tuyo |
| `RCC_D2CCIP1R`, `RCC_D3CCIPR` (escritura) | El multiplexor de kernel es del proyecto. Las pruebas corren con el valor de reset |
| Capturas ya commiteadas (`docs/*.txt`) | Una captura no se edita nunca |
| Cambiar una firma o constante publicada, después de que el auditor pase la fase que la congela | Plan C2 |
| Rebajar un criterio, un gate o una prueba | Si una prueba no se puede hacer como está escrita, para, explícalo en el informe y espera |
| Merge a `main`, borrar la rama, reescribir historia entregada (`rebase`, `commit --amend`, `push --force`), borrar el plan, `AUDIT.md` o tu informe | Vienen después del cierre, y no los haces tú |

---

## 3. Cómo trabajar

### 3.1 Rama y commits

- Todo va en **`driver/spi`**, creada desde `main` en `8901b77`. Trabaja sobre el último commit de la rama que toca este archivo o el plan, o sobre uno posterior.
- **Un commit por preocupación,** con el estilo del repo:
  - prefijos `build(spi)`, `feat(spi)`, `fix(spi)`, `test(prj_01)`, `feat(tools)`, `fix(tools)` y `docs(spi)`;
  - asunto en inglés e imperativo;
  - el porqué en el cuerpo;
  - el ID del hallazgo entre paréntesis: `fix(spi): clear IER before SPE on abort (M2)`.
- **Mensajes de commit desde bash** con `git commit -F - <<'EOF' … EOF`. Desde PowerShell, `$msg | git commit -F -` mete un BOM al principio del asunto.
- Cada entrega es un rango de commits con el árbol limpio. El repositorio no tiene remoto: deja la rama en el repositorio de trabajo.

### 3.2 Decisiones abiertas

Usa la opción por defecto de cada una (plan, "Decisiones abiertas" 1–8). Si crees que otra es mejor, **para y pregunta** antes de implementarla. En el informe di cuál usaste.

### 3.3 Entorno

- **Antes de cualquier build,** `.\tools\env.ps1` en PowerShell: cmake, ninja y gcc están dentro de los directorios de STM32CubeIDE, y un `cmake` suelto falla. Matriz: `.\tools\build_all.ps1 -Clean`.
- **Python:** `py`, no `python` (que es un stub roto de la Store). Solo biblioteca estándar; el AD2 va por ctypes sobre `dwf.dll`.
- **Analog Discovery 2 (SN `210321B5D38D`):** cierra la GUI de WaveForms antes de ejecutar los scripts. El cableado es el de `00_gpio/README.md` §8, **directo, sin resistencias en serie**. Lee §3.6 antes de escribir una línea del script o del selftest.
- **Puente J1** (plan, "Banco de pruebas"): lo coloca el responsable del catálogo. Tú no lo mueves, no lo quitas y no añades otro.
- **Manual del SDK:** `C:\Program Files\Digilent\WaveFormsSDK\WaveForms SDK Reference Manual.pdf`; las constantes, en `inc\dwf.h`. Para leer PDFs, `pdftotext -layout` de Git (`/mingw64/bin/pdftotext.exe`).
- **RM0399, ES0445, DS12923, HAL:** el capítulo SPI en `.txt` está en `00_docs_stm32h7/docs/peripherals/spi/53_serial-peripheral-interface-spi.txt`; RCC en `rcc/`, DMAMUX y BDMA en `dma_mdma/`, ES0445 y DS12923 en `00_core_and_system/`. La HAL y el LL de SPI, en `00_docs_stm32h7/docs/01_drivers_and_hal/STM32CubeH7/Drivers/STM32H7xx_HAL_Driver/` (la misma V1.11.6 que compila `prj_01`). Cita el RM como `53:<línea>`.
- **Depurador:** no leas con el depurador el banco `RLR` del HSEM (`0x58026480`–`0x580264FC`). Las capturas se hacen **en frío y sin depurador conectado**.
- **Placa colgada después de un cambio:** conecta con "connect under reset" y lee `MPU_CTRL` y `SCB_CFSR` antes de nada. Sin consola después de un arranque en frío: mira `PWR_CR3`.

### 3.4 Capturas (evidencia)

Si una captura no cumple **todo** esto, no vale como evidencia:

1. Es la salida de **una sola ejecución** de `prj_01/tools/spi_hw_check.py`, con marcas de tiempo, desde `[host] argv:` hasta `HOST VERDICT=`.
2. Los dos núcleos imprimen el mismo `Rev <hash>` y el mismo id de build. El hash es el del commit entregado, sin `-dirty`, y el id es el que calcula el host. Commitea antes de compilar la imagen que vas a capturar.
3. Lleva T0a y T0c al principio y la lista de comprobación del procedimiento de `00_gpio/README.md` §8 marcada, con una línea más: "J1 colocado (CN7-14 ↔ CN7-12)".
4. **No se edita.** Si hay que repetir, la nueva ejecución va en un archivo nuevo.
5. Los números que cites en el informe salen de una captura o de un `.map` que estén en el repo.
6. Nombre: `03_firmware_templates/prj_01_template_dualcore/docs/spi_<fase>_<prueba>_<hash>.txt`. Las ejecuciones de desarrollo con `--allow-dirty` llevan `spi_dev_…` y no cuentan como entrega.

### 3.5 Pruebas

- **Cada prueba imprime el valor de su control negativo.** Si el control no reproduce el fallo, la prueba **falla**, aunque la parte positiva salga bien. La única excepción es la de la decisión abierta 6 (controles de erratas de T12), y se imprime como "no reproducido".
- **El oráculo no puede ser el driver.** Usa el de la fila del plan: el cable de J1, el AD2, `CYCCNT`, `IDR`, registros leídos directamente, la tabla del RM escrita a mano o el cálculo del host.
- **El patrón de datos es el del plan** (xorshift32 con semilla impresa, resumen y cobertura de bits). Un patrón que no cubre todos los bits haría pasar el control de pull: el host lo rechaza.
- **El lazo J1 es ciego al orden de bits y a `CPOL`/`CPHA`:** no lo uses como prueba de esos campos. Eso es T5.
- **Las pruebas no son destructivas (R4):**
  - nunca TIM6, SysTick, `DBGMCU`, `RCC_D2CFGR`, `RCC_D2CCIP1R`, `RCC_D3CCIPR` ni ningún `RCC_APBxRSTR`;
  - `RCC_CFGR` (`MCO1SEL`/`MCO1PRE`) solo en los pasos de T5, guardado antes y restaurado con read-back;
  - nunca PE0, PE1, PC13, PA4, PA7, PA13 ni PA14;
  - cada prueba termina con `Deinit` de su instancia, y T14 lo comprueba.
- **Propiedad de las instancias:** CM7 tiene SPI2–SPI6 y CM4 tiene SPI1. Una instancia la escribe solo su núcleo. Los pines de J1 los toma en cada paso el núcleo de ese paso.
- **Buffers:** BDMA solo desde CM7 y en `.d3_ram` (SP3 del plan). DMA1/2 en `.dma_buf` (CM7) o en la SRAM2 de CM4. Todo buffer de DMA alineado a 32 bytes y de tamaño múltiplo de 32.
- **El veredicto se calcula, nunca se imprime sin más.** CM7 lo imprime cuando CM4 ha terminado, e incluye los resultados de CM4.

### 3.6 Seguridad eléctrica: sin resistencias en serie

Entre el AD2 y la Nucleo no hay ninguna resistencia, y J1 une dos pines del MCU con un cable. Si tu código hace que dos conductores manejen la misma línea a la vez, **se puede dañar el hardware**, y no siempre de forma visible (DS12923 tabla 11: 20 mA por pin).

Valen S1–S12 tal como están. Este driver añade dos reglas (plan, "Reglas nuevas S13 y S14"):

- **S13: SPI4 en el bus del AD2.**
  - PE11, PE12 y PE14 van a AF5 solo en un paso "bus = MCU", y PE13 va a AF5 como entrada en ese mismo paso.
  - `bench_claim()` comprueba antes de aceptar que SPI4 tiene `SPE = 0` y `AFCNTR = 0`. Con eso el SPI no conduce los pines hasta que empieza la transferencia, y la prueba S5 se hace **con los pines ya en AF5**.
  - Un paso que use `keep_pins = true` (`AFCNTR = 1`) termina con `Deinit` antes de devolver el bus. Con `AFCNTR = 1` el SPI conduce SCK y MOSI aunque `SPE = 0`.
  - PE8–PE10 y PE15 nunca van a AF; PE0–PE7 nunca van a AF ni a salida.
- **S14: el puente J1.**
  - **PA6 y PB4 nunca son salida GPIO.** Solo entrada o AF de MISO.
  - PB5 es salida GPIO solo en T0c; en los demás pasos, AF de MOSI o entrada.
  - Los pines de J1 los toma un solo núcleo por paso, por el lock GPIO entre núcleos. `bench_claim()` existe en los dos núcleos y `bench_safe()` deja PA5, PA6, PB4 y PB5 en entrada sin pull.
- **Sin Digital Out.** El banco SPI no lo usa (D6): `spi_hw_check.py` no llama a `ad2_set_dout` y lee en cada paso que el enable de Digital Out es 0.
- **T0b y T0c van antes que todo lo demás.** Hasta que el auditor los vea en verde en su propia ejecución, no ejecutes nada que mueva el bus o los pines de J1. T0a y T0c se repiten al empezar **cada** sesión.
- **Nunca pruebes una guarda provocando una contienda de verdad.** El control de T0b(e) está diseñado sin contienda (el AD2 en entrada). Si se te ocurre otra forma de probar una guarda, pregunta antes.
- **No cambies el cableado,** ni la dirección de ninguna línea, ni conectes W1, W2, V+ o V−, ni muevas J1.
- **No flashees ni resetees con el script corriendo** (S10).
- **Si algo sale raro** (lectura inesperada en T0a o T0c, un `ABORT`, un pin que se calienta, el LED de la Nucleo que se apaga), **para, deja el AD2 en S2, desconecta las DIO** y apúntalo en el informe (§5.8) antes de volver a intentarlo.

El auditor revisa todo esto en A27. Un incumplimiento es **bloqueante** aunque todas las pruebas pasen, y el auditor no conecta el AD2 a tu imagen hasta que esté corregido.

---

## 4. Qué entregar al terminar cada fase

1. El rango de commits en `driver/spi`, con el árbol limpio.
2. La salida completa de `.\tools\build_all.ps1 -Clean` (8/8), la de `cmake --build --preset selftest-spi` (0 avisos) y la de una compilación aparte con `CMAKE_C_FLAGS=-Werror`.
3. Los tamaños **del `.map` enlazado**, sin "Discarded input sections": `.text`, `.data` y `.bss` de cada `spi_*.c.obj` en CM4 y CM7, y los totales de `prj_01` debug/release frente a `2481396` (tienen que ser iguales: con la opción apagada nada enlaza el driver).
4. Las capturas de la fase (§3.4), commiteadas.
5. **Tu informe actualizado** (§5), con la sección de la fase.
6. Un aviso al auditor con el hash del último commit. Y a esperar.

Cuando el auditor deje hallazgos:

- Corrígelos en commits nuevos que citen el ID.
- Añade al informe una sección "Respuesta a la auditoría n" que diga qué commit corrige cada hallazgo y qué ejecutaste para comprobarlo.
- **No digas que un hallazgo está cerrado:** di que está "corregido en `<hash>`, pendiente de verificación".

---

## 5. El informe: `06_spi/IMPLEMENTATION_REPORT.md`

Es obligatorio. Lo creas en F0 y le añades una sección por fase y por cada respuesta a una auditoría. **Las secciones ya entregadas no se reescriben.** Si algo que dijiste era falso, lo corriges en una sección nueva que diga qué estaba mal.

Reglas de redacción:

- **Hechos, no valoraciones.** "T4 SPI6: 0 discrepancias en 896 transferencias; control PB4 pull-up: 4093 discrepancias, esperadas 4093" sí. "El lazo es robusto" no.
- **Cada número lleva su fuente:** la captura con su hash, o el `.map`.
- **Nada de palabras de auditoría** (§1). El estado de una fase solo puede ser "en curso" o "entregada para auditoría (`<hash>`)".
- **Lo que falló y lo que no se pudo ejercitar va tan visible como lo que salió bien.**
- **Cada identificador que cites** (función, macro, registro, archivo) se comprueba con `git grep` antes de entregar.

### Plantilla

```markdown
# Informe de implementación 06_spi

Implementador: <nombre> · Rama: driver/spi · Plan: IMPLEMENTATION_PLAN.md revisión 2

## Resumen
<5-10 líneas: qué hay implementado, en qué fase estás, qué falta y qué está bloqueado.>

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F0 | entregada para auditoría | `abc1234` | `docs/spi_f0_bench_abc1234.txt` |
| F1 | en curso | — | — |

## F0 (y una sección igual para F1, F2 y F3)

### 5.1 Commits
| Commit | Asunto | Hallazgo |

### 5.2 Build y tamaños
| Imagen | .text | .data | .bss | Δ frente a 2481396 | Fuente (.map) |
| Objeto | Núcleo | .text | .data | .bss | Fuente (.map) |

### 5.3 Pruebas
| ID | Tier | Núcleo | Resultado (PASS/FAIL) | Valor medido | Control negativo: valor | Oráculo | Captura |

### 5.4 Medidas (B1)
Ciclos por función y núcleo; frecuencia más alta que pasó en T8, por instancia.

### 5.5 Desviaciones del plan
| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |

### 5.6 Respuesta a la auditoría n (solo si la hay)
| Hallazgo | Commit que lo corrige | Qué ejecuté para comprobarlo | Estado: "corregido, pendiente de verificación" |

### 5.7 Erratas del plan encontradas
| Sección del plan | Qué dice | Qué es correcto | Fuente (53:<línea>, ES0445 §, página del SDK) |

### 5.8 Problemas abiertos y no ejercitado

## Decisiones abiertas usadas
| # | Opción usada | Consultada |

## Tabla de capacidades
La tabla de la regla 2 del plan, con una columna más: la línea del .txt del RM de la que sale
cada valor. Marca toda celda que difiera del plan.
```

---

## 6. Resumen por fase de lo que te toca

La lista completa de entregables y gates está en la tabla "Fases" del plan. Aquí van los puntos donde es fácil equivocarse.

- **F0:**
  - J1 está colocado y verificado desde el 2026-10-03 (cabecera del plan). Si al empezar una sesión T0c falla, para y avisa: no lo recoloques tú.
  - Lo primero del código son las guardas (§3.6): S13 y S14 en `bench_claim()`/`bench_safe()` de los dos núcleos, la tabla de pasos con versión y `spi_hw_check.py` con S2–S8 sin Digital Out.
  - No hay código del driver ni targets `_spi_*` en F0: cada target entra en su fase.
  - Con la opción apagada, la producción de `prj_01` tiene los tamaños de `2481396`: compruébalo antes de entregar.
- **F1:**
  - Empieza por la tabla de capacidades, con la línea del RM en cada fila.
  - Después, la validación (C6), las funciones `SPI_BM_Check*` y las funciones puras con T3.
  - El fin de transferencia de la regla 4 es donde viven tres erratas (2.22.1, 2.22.3 y 2.22.5): `IER = 0` **antes** de `SPE = 0`, guarda entre `EOT` y `SPE = 0`, y `SPE = 0` tras cada `EOT`. Un `CSTART` sobre un SPI que sigue habilitado tras un `EOT` es bloqueante.
  - `DSIZE = 4` empaqueta dos tramas por byte (53:772-776). Cita el RM para el caso de un número impar de tramas y pruébalo en T4.
  - `IFCR` se escribe una vez con los bits a borrar. Un `IFCR |=` es un RMW de un registro de solo escritura y se marca en A9.
- **F2:**
  - En `spi_bm.h` solo se añade. Compruébalo con `git diff <cierre F1> -- 01_shared_drivers/06_spi/spi_bm.h`.
  - LL valida llamando a `SPI_BM_Check*`: si duplicas la tabla o una validación, es un hallazgo.
  - La paridad de T2 es bit a bit.
- **F3:**
  - `HAL_SPI_MODULE_ENABLED` y su `#include` solo bajo `SPI_DRIVER_SELFTEST`, en CM7. La producción no cambia de tamaño.
  - `spi_hal` no define callbacks ni MSP (C14). Para IT y DMA, el `SPIx_IRQHandler` y los de DMA viven en `spi_selftest.c`.
  - Cada diferencia de imagen HAL frente a BM va al informe con su línea de `stm32h7xx_hal_spi.c`.
  - La captura tiene que mostrar que `HAL_GetTick()` sigue avanzando durante y después de las pruebas HAL.

### 6.1 Cierre: el README

Solo cuando el auditor dé F3 por buena y **te lo pida**.

- En inglés, en `01_shared_drivers/06_spi/README.md`, con las secciones de "README de cierre" del plan y en ese orden.
- **Como mucho 600 líneas.** El formato de referencia es `03_tim/README.md`.
- **Tiene que llevar:** el diagrama ASCII del banco con J1 y las líneas de SPI4; la tabla de líneas; la guía de configuración paso a paso; la tabla de capacidades; los comandos para reproducir la validación; los ejemplos que compilan con los flags del plan.
- **Cada afirmación sale del código o de una captura,** y cada número lleva su captura con hash. Lo no ejercitado va en su sección, sin suavizarlo.
- En el mismo commit (`docs(spi): …`), la línea de `06_spi` del árbol de `01_shared_drivers/README.md`, con el estilo de la de `03_tim`.
- `DRIVER_ROADMAP.md`, `02_dma/README.md` y `00_gpio/README.md` no se tocan.

---

## 7. Cuándo parar y preguntar

Para y pregunta, sin seguir por tu cuenta, si:

- una prueba no se puede hacer como dice el plan (por hardware, porque el control no reproduce el fallo, o por R4);
- una prueba parece necesitar que dos conductores manejen la misma línea, que una línea cambie de dirección respecto a la tabla, que se use Digital Out o que J1 se mueva (S1, S13, S14);
- T0c falla (J1 ausente, flojo o en otro sitio);
- necesitas una función pública que no está en el contrato del plan, o cambiar una firma, una regla de cálculo, un criterio C o un gate;
- necesitas tocar algo fuera de §2.1, en especial `02_firmware_projects`, otro driver o la producción de `prj_01`;
- un dato del RM contradice el plan de una forma que cambia lo que hay que construir;
- quieres elegir una opción distinta de la de por defecto en una decisión abierta.

Apunta la pregunta en §5.5 o §5.7 del informe para que quede registrada.
