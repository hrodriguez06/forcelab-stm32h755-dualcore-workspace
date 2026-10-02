# Instrucciones para el implementador de 03\_tim

Oct 1, 2026 · @Hector

Vas a implementar [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 6) de esta carpeta. El trabajo es el driver de timers en tres tiers, `tim_bm` (F1), `tim_ll` (F2) y `tim_hal` (F3), con una fase F0 que prepara el banco del AD2 en `prj_01_template_dualcore`. Otra persona audita cada fase de forma independiente y repite tus pruebas en la placa.

Este documento dice qué te toca, qué no te toca y qué tienes que entregar. Si algo de aquí choca con el plan, manda el plan, con una excepción: lo que este documento te **prohíbe** sigue prohibido aunque el plan lo mencione.

**El encargo, en tres frases** (texto completo en el plan, "Encargo"):

1. Driver de catálogo con BM, LL y HAL.
2. Se valida **solo en `prj_01_template_dualcore`**, con el cableado del AD2 que ya existe. No validas ni corriges ningún otro proyecto.
3. Al final, un `README.md` limpio y conciso, con el formato de los drivers terminados.

---

## 1. Tu papel, y lo que no es tu papel

**Tú implementas y pruebas. No auditas.**

- **No hagas auditorías, ni de tu trabajo ni del de nadie.** No escribas "auditoría", "auditado", "aprobado", "cerrado", "validado por auditoría" ni nada parecido para describir una fase o un hallazgo. Lo más que puede decir tu trabajo de sí mismo es **"entregado para auditoría"**.
- **No crees, edites ni borres `03_tim/AUDIT.md`.** Lo escribe solo el auditor. Si no estás de acuerdo con un hallazgo, contéstalo en tu informe (§5.6).
- **No marques un hallazgo como cerrado.** Se cierra cuando el auditor lo verifica. Tu commit dice que lo *corrige*.
- **No rellenes la tabla A1–A28** del plan. Es lo que va a mirar el auditor, no un formulario tuyo.
- **No empieces la fase siguiente** hasta que el auditor deje en `AUDIT.md` que la actual pasa. Mientras esperas, corrige lo que te pida.

Este reparto no es una formalidad. En `10_hsem`, las auditorías de fase las escribió el implementador y el informe se declaraba "auditado y cerrado al 100 %". La auditoría independiente las anuló todas (`git show 6f7af43:01_shared_drivers/10_hsem/AUDIT.md`, hallazgo B6).

---

## 2. Qué puedes tocar y qué no

### 2.1 Lo único que puedes modificar

| Ruta | Qué |
| --- | --- |
| `01_shared_drivers/03_tim/tim_{bm,ll,hal}.{h,c}` | El driver |
| `01_shared_drivers/03_tim/IMPLEMENTATION_REPORT.md` | Tu informe (§5) |
| `01_shared_drivers/03_tim/README.md` | Solo en el cierre, cuando el auditor lo pida (§6) |
| `01_shared_drivers/CMakeLists.txt` | Solo los tres targets `_tim_*` del plan |
| `01_shared_drivers/README.md` | Solo la línea de `03_tim` del árbol, en el cierre |
| `03_firmware_templates/prj_01_template_dualcore/**` | Lo que el plan lista en "Archivos, dependencias y targets", nada más |
| `tools/ad2.py` | Solo añadir (Digital Out, S11) y ampliar S2 |

Todo lo demás está fuera. El auditor lo comprueba con `git diff e7542dc --stat` (A23).

### 2.2 Prohibido expresamente

| Archivo o acción | Por qué |
| --- | --- |
| `02_firmware_projects/**` (`prj_02`, `prj_03`, `prj_04`, `prj_eth_lwip_node`) | Encargo. TIM3/TIM7 de `prj_04` se quedan como están (plan, D1). `git diff e7542dc -- 02_firmware_projects` tiene que salir vacío |
| `01_shared_drivers/DRIVER_ROADMAP.md` | Lo actualiza el responsable del catálogo después de la auditoría, también en el cierre |
| `03_tim/AUDIT.md`, `03_tim/IMPLEMENTATION_PLAN.md`, `03_tim/IMPLEMENTER_INSTRUCTIONS.md` | El plan es la norma que se te audita. Si tiene un error, lo apuntas en el informe (§5.7) y avisas; no lo corriges tú |
| `00_gpio/*`, `02_dma/*`, `10_hsem/*`, `04_uart/*` (fuentes y README) | Drivers cerrados. Los usas; no los cambias. Si te falta algo, para y pregunta |
| `prj_01` de producción: `stm32h7xx_hal_timebase_tim.c`, `stm32h7xx_it.c`, `mpu_config.c`, los linker scripts, `board_def.h`, `shared_memory.[ch]` y el código de `00_gpio` (`gpio_selftest*`, `gpio_hw_check.py`) | Producción o evidencia de otro driver. En `main.c` y `app_main.c` solo añades llamadas dentro de `#if defined(TIM_DRIVER_SELFTEST)` |
| Capturas ya commiteadas (`docs/*.txt`) | Una captura no se edita nunca. Una ejecución nueva va en un archivo nuevo |
| Cambiar una firma o constante publicada, después de que el auditor pase la fase que la congela | Plan C2 |
| Rebajar un criterio, un gate o una prueba | Si una prueba no se puede hacer como está escrita, para, explícalo en el informe y espera |
| Merge a `main`, borrar la rama, reescribir historia entregada (`rebase`, `commit --amend`, `push --force`), borrar el plan, `AUDIT.md` o tu informe | Vienen después del cierre de la auditoría, y no los haces tú |

---

## 3. Cómo trabajar

### 3.1 Rama y commits

- Todo va en **`driver/tim`**, que ya existe, creada desde `main` en `e7542dc`. Trabaja sobre el último commit de la rama que toca este archivo o el plan, o sobre uno posterior.
- **Un commit por preocupación,** con el estilo del repo:
  - prefijos `build(tim)`, `feat(tim)`, `fix(tim)`, `test(prj_01)`, `feat(tools)`, `fix(tools)` y `docs(tim)`;
  - asunto en inglés e imperativo;
  - el porqué en el cuerpo;
  - el ID del hallazgo entre paréntesis: `fix(tim): refuse CCxS writes while CCxE is set (M2)`.
- **Mensajes de commit desde bash** con `git commit -F - <<'EOF' … EOF`. Desde PowerShell, `$msg | git commit -F -` mete un BOM al principio del asunto.
- Cada entrega es un rango de commits con el árbol limpio. El repositorio no tiene remoto: deja la rama en el repositorio de trabajo, como rama.

### 3.2 Decisiones abiertas

Usa la opción por defecto de cada una (plan, "Decisiones abiertas" 1–6). Si crees que otra es mejor, **para y pregunta** antes de implementarla. En el informe di cuál usaste.

### 3.3 Entorno

- **Antes de cualquier build,** `.\tools\env.ps1` en PowerShell: cmake, ninja y gcc están dentro de los directorios de STM32CubeIDE, y un `cmake` suelto falla. Matriz: `.\tools\build_all.ps1 -Clean`.
- **Python:** `py`, no `python` (que es un stub roto de la Store). Solo biblioteca estándar; el AD2 va por ctypes sobre `dwf.dll`, sin pip.
- **Analog Discovery 2 (SN `210321B5D38D`):** cierra la GUI de WaveForms antes de ejecutar los scripts. El cableado es el de `00_gpio/README.md` §8, **directo, sin resistencias en serie**. Lee §3.6 antes de escribir una línea del script o del selftest.
- **Manual del SDK:** `C:\Program Files\Digilent\WaveFormsSDK\WaveForms SDK Reference Manual.pdf` (revisado el 4 de marzo de 2026); las constantes, en `inc\dwf.h`. Para leer PDFs, `pdftotext -layout` de Git (`/mingw64/bin/pdftotext.exe`). Cita la sección del manual en cada función nueva de `ad2.py`.
- **RM0399, ES0445, HAL:** los capítulos en `.txt` están en `00_docs_stm32h7/docs/peripherals/timers_pwm/` (40–45), `rcc/`, `dma_mdma/` y `gpio/`. La HAL y el LL de TIM, en el Cube de `02_firmware_projects/prj_eth_lwip_node/external/STM32CubeH7/Drivers/STM32H7xx_HAL_Driver/`.
- **Depurador:** no leas con el depurador el banco `RLR` del HSEM (`0x58026480`–`0x580264FC`), porque leerlo toma los semáforos. Las capturas se hacen **en frío y sin depurador conectado**.
- **Placa colgada después de un cambio:** conecta con "connect under reset" y lee `MPU_CTRL` y `SCB_CFSR` antes de nada.

### 3.4 Capturas (evidencia)

Si una captura no cumple **todo** esto, no vale como evidencia:

1. Es la salida de **una sola ejecución** de `prj_01/tools/tim_hw_check.py`, con marcas de tiempo, desde `[host] argv:` hasta `HOST VERDICT=`.
2. Los dos núcleos imprimen el mismo `Rev <hash>` y el mismo id de build. El hash es el del commit entregado, sin `-dirty`, y el id es el que calcula el host (mismo método que `00_gpio/README.md` §9.4). Commitea antes de compilar la imagen que vas a capturar.
3. Lleva TREF\_A al principio y TREF\_B al final (R3-1) y la lista de comprobación del procedimiento de `00_gpio/README.md` §8 marcada.
4. **No se edita:** ni recortar, ni resumir, ni corregir a mano. Si hay que repetir, la nueva ejecución va en un archivo nuevo, con su hash.
5. Los números que cites en el informe salen de una captura o de un `.map` que estén en el repo.
6. Nombre: `03_firmware_templates/prj_01_template_dualcore/docs/tim_<fase>_<prueba>_<hash>.txt` (por ejemplo `tim_f1_selftest_1a2b3c4.txt`). Las ejecuciones de desarrollo con `--allow-dirty` llevan `tim_dev_…` y no cuentan como entrega.

### 3.5 Pruebas

- **Cada prueba imprime el valor de su control negativo.** Si el control no reproduce el fallo, la prueba **falla**, aunque la parte positiva salga bien.
- **El oráculo no puede ser el driver.** Usa el de la fila del plan: AD2, `CYCCNT`, `IDR`, registros leídos directamente, la tabla del RM escrita a mano o el cálculo del host. Una prueba que lee un registro con una función del propio driver para comprobar lo que el driver escribió no cuenta.
- **Las pruebas no son destructivas (R4):**
  - **nunca TIM6**: es el tick de la HAL en CM7. No se lo pases a ninguna función que escriba, ni para probar un rechazo (usa TIM7);
  - nunca SysTick, LPTIM, HRTIM, `DBGMCU`, `RCC_D2CFGR`, `RCC_CFGR.TIMPRE` ni ningún `RCC_APBxRSTR`;
  - `RCC_CFGR` (`MCO1SEL`/`MCO1PRE`) solo en TREF\_A, TREF\_B, T9 y los pasos de T6/T8 (R3-2): lo guardas antes y lo restauras con read-back;
  - nunca PE0, PE1 (LD2) ni PC13;
  - cada prueba termina con `Deinit` de su timer, y T14 lo comprueba.
- **Propiedad de las instancias:** la de la tabla del plan ("Selftest"). Un timer lo escribe solo su núcleo dueño.
- **El veredicto se calcula, nunca se imprime sin más.** CM7 lo imprime cuando CM4 ha terminado, e incluye los resultados de CM4.

### 3.6 Seguridad eléctrica: sin resistencias en serie

Entre el AD2 y la Nucleo no hay ninguna resistencia. Si tu código hace que el MCU y el AD2 conduzcan la misma línea a la vez, o que el AD2 conduzca con la placa sin alimentar, **se puede dañar el hardware**, y no siempre de forma visible (DS12923 tabla 11: 20 mA por pin, +0 mA de inyección en pines FT).

Valen S1–S10 de `00_gpio/README.md` §8, tal como están. Este driver añade dos reglas (plan, "Reglas nuevas S11 y S12"), y es fácil equivocarse en ellas:

- **S11: Digital Out conduce aunque `OE = 0`.** Manual del SDK, §8: Digital-Out se aplica a una línea justo cuando Digital-IO tiene enable y output a 0, que es el S2 actual. Por eso:
  - La única llamada a `FDwfDigitalOutEnableSet` está dentro de `ad2_set_dout(mask, step_bus_owner)`. Esa función acepta solo DIO1 y DIO3, y solo en un paso "bus = AD2".
  - S2 hace `FDwfDigitalOutReset`, `Configure(0)`, `EnableSet(0)` e `IdleSet(DwfDigitalOutIdleZet)` en los 16 canales, con read-back.
  - Todo canal que habilitas lleva `Idle = Zet` y push-pull **antes** de `Configure(1)`. `Idle = Low` solo se acepta en el control de T0b(f), ligado a su paso (plan R2-4).
  - Mientras corre Digital Out, el `OE` de Digital-IO lee 0 en los bits de los canales de Digital Out. HOST\_REQ puede seguir habilitado (plan R2-3).
  - Impleméntalo **en F0, antes que ninguna prueba**, y comprueba que `gpio_hw_check.py` sigue pasando su T0b(a).
- **S12: un pin de captura en AF mientras el AD2 conduce.** Solo PE9 y PE11, en AF1, con TIM1 en entrada. `bench_claim()` lee TIM1 antes de aceptar: `CCxS ≠ 00`, `CCxNE = 0`, `CC3E = CC4E = 0` y `MOE = OSSI = OSSR = 0`. La prueba de pulls S5 se hace con los pines **ya en AF**.
- **Líneas que nunca van en AF:** PE0–PE4, PE7, PE8, PE10, PE12 y PE15. PE4 en AF4 es TIM15\_CH1N: el MCU conduciría una línea "solo AD2". `bench_claim()` existe en los dos núcleos, porque CM4 configura PE5/PE6 para TIM15.
- **T0b va antes que todo lo demás.** Hasta que el auditor la vea en verde en su propia ejecución, no ejecutes nada que mueva el bus o use Digital Out. T0a (sueltas y cableado) se repite al empezar **cada** sesión.
- **Nunca pruebes una guarda provocando una contienda de verdad.** Los controles de T0b(c) y T0b(f) están diseñados para que no haya contienda: el MCU en entrada con pull, o el AD2 en entrada. Si se te ocurre otra forma de probar una guarda, pregunta antes.
- **No cambies el cableado,** ni la dirección de ninguna línea "solo MCU" / "solo AD2", ni conectes W1, W2, V+ o V−. Las resistencias de 22 kΩ de PF14/PF15 se quedan como están.
- **No flashees ni resetees con el script corriendo** (S10).
- **Si algo sale raro** (una lectura inesperada en la prueba de sueltas, un `ABORT`, un pin que se calienta, el LED de la Nucleo que se apaga), **para, deja el AD2 en S2, desconecta las DIO** y apúntalo en el informe (§5.8) antes de volver a intentarlo.

El auditor revisa todo esto en A27. Un incumplimiento es **bloqueante** aunque todas las pruebas pasen, y el auditor no conecta el AD2 a tu imagen hasta que esté corregido.

---

## 4. Qué entregar al terminar cada fase

1. El rango de commits en `driver/tim`, con el árbol limpio.
2. La salida completa de `.\tools\build_all.ps1 -Clean` (8/8), la de `cmake --build --preset selftest-tim` (0 avisos) y la de una compilación aparte con `CMAKE_C_FLAGS=-Werror` (plan C9, R2-5).
3. Los tamaños **del `.map` enlazado**, sin contar "Discarded input sections": `.text`, `.data` y `.bss` de cada `tim_*.c.obj` en CM4 y CM7, y los totales de `prj_01` debug/release frente a `2481396` (plan R4-2; tienen que ser iguales: con la opción apagada nada enlaza el driver).
4. Las capturas de la fase (§3.4), commiteadas.
5. **Tu informe actualizado** (§5), con la sección de la fase.
6. Un aviso al auditor con el hash del último commit. Y a esperar.

Cuando el auditor deje hallazgos:

- Corrígelos en commits nuevos que citen el ID.
- Añade al informe una sección "Respuesta a la auditoría n" que diga qué commit corrige cada hallazgo y qué ejecutaste para comprobarlo.
- **No digas que un hallazgo está cerrado:** di que está "corregido en `<hash>`, pendiente de verificación".

---

## 5. El informe: `03_tim/IMPLEMENTATION_REPORT.md`

Es obligatorio. Lo creas en F0 y le añades una sección por fase y por cada respuesta a una auditoría. **Las secciones ya entregadas no se reescriben.** Si algo que dijiste era falso, lo corriges en una sección nueva que diga qué estaba mal.

Reglas de redacción:

- **Hechos, no valoraciones.** "T5 TIM2: −3 ppm en 100 ms; control PSC+1: −90 909 ppm" sí. "La base de tiempo es exacta y robusta" no.
- **Cada número lleva su fuente:** la captura con su hash, o el `.map`.
- **Nada de palabras de auditoría** (§1). El estado de una fase solo puede ser "en curso" o "entregada para auditoría (`<hash>`)".
- **Lo que falló y lo que no se pudo ejercitar va tan visible como lo que salió bien.**
- **Cada identificador que cites** (función, macro, registro, archivo) se comprueba con `git grep` antes de entregar.

### Plantilla

```markdown
# Informe de implementación 03_tim

Implementador: <nombre> · Rama: driver/tim · Plan: IMPLEMENTATION_PLAN.md revisión 6

## Resumen
<5-10 líneas: qué hay implementado, en qué fase estás, qué falta y qué está bloqueado.
 Sin valoraciones.>

| Fase | Estado | Último commit | Captura |
| --- | --- | --- | --- |
| F0 | entregada para auditoría | `abc1234` | `docs/tim_f0_bench_abc1234.txt` |
| F1 | en curso | — | — |

## F0 (y una sección igual para F1, F2 y F3)

### 5.1 Commits
| Commit | Asunto | Hallazgo |

### 5.2 Build y tamaños
- build_all -Clean: 8/8 (salida en <archivo>); selftest-tim: <salida>
| Imagen | .text | .data | .bss | Δ frente a 2481396 (R4-2) | Fuente (.map) |
| Objeto | Núcleo | .text | .data | .bss | Fuente (.map) |

### 5.3 Pruebas
| ID | Tier | Núcleo | Resultado (PASS/FAIL) | Valor medido | Control negativo: valor | Oráculo | Captura |
(Todas las pruebas de la fase, también las que fallaron.)

### 5.4 Medidas (B1)
Ciclos por función y núcleo; peor caso de CalcTimeBase; resolución de TREF en ppm.

### 5.5 Desviaciones del plan
| Qué dice el plan | Qué hice | Por qué | ¿Consultado con quién y cuándo? |
(Si no hay ninguna, escribe "Ninguna".)

### 5.6 Respuesta a la auditoría n (solo si la hay)
| Hallazgo | Commit que lo corrige | Qué ejecuté para comprobarlo | Estado: "corregido, pendiente de verificación" |
Desacuerdos, con argumentos y fuente (RM0399, ES0445, SDK, código).

### 5.7 Erratas del plan encontradas
| Sección del plan | Qué dice | Qué es correcto | Fuente (sección y línea del .txt del RM, o página del SDK) |

### 5.8 Problemas abiertos y no ejercitado
- Lo que no funciona o no está terminado.
- Lo que no se pudo probar en la placa y por qué.

## Decisiones abiertas usadas
| # | Opción usada | Consultada |

## Tabla de capacidades
La tabla de la regla 2 del plan, con una columna más: la sección y la línea del .txt del RM de
la que sale cada valor. Marca toda celda que difiera del plan.
```

---

## 6. Resumen por fase de lo que te toca

La lista completa de entregables y gates está en la tabla "Fases" del plan. Aquí van los puntos donde es fácil equivocarse.

- **F0:**
  - Lo primero son las guardas de seguridad (§3.6): S11 en `ad2.py`, S2 ampliado, `bench_claim()`/`bench_safe()` en los dos núcleos y la tabla de pasos con versión. Sin eso, nada que use el AD2 más allá de T0a.
  - No hay código del driver ni targets `_tim_*` en F0: cada target entra en su fase (F1, F2 o F3), con su fuente.
  - Con la opción apagada, las imágenes de producción de `prj_01` tienen que quedar con los mismos tamaños que en `2481396` (plan R4-2): compruébalo antes de entregar.
- **F1:**
  - Empieza por la tabla de capacidades, con la cita del RM en cada fila. Si un valor del RM difiere del plan, manda el RM: apúntalo en §5.7.
  - Después, la validación (C6) y las funciones puras con T3. Así el host te dice pronto si tu redondeo es el del plan.
  - `TakeFlags` escribe `SR` una sola vez (`SR = ~taken`). Un `SR &= ~x` es un RMW y es bloqueante (A9).
  - T15 tiene que mostrar pérdidas > 0 en el control. Si sale 0, ensancha la ventana del RMW. Nunca se da por bueno.
  - Antes de entregar, `git grep -n "TIM6" 03_firmware_templates/prj_01_template_dualcore/C*/App/Src/tim_selftest.c` tiene que dar 0 líneas que escriban en TIM6.
- **F2:**
  - En `tim_bm.h` solo se añade. Compruébalo con `git diff <cierre F1> -- 01_shared_drivers/03_tim/tim_bm.h`.
  - LL valida llamando a BM: si duplicas la tabla de capacidades o una validación, es un hallazgo.
  - La paridad de T2 es bit a bit.
- **F3:**
  - `tim_hal` no define callbacks ni MSP (C14). El proyecto ya define `HAL_TIM_PeriodElapsedCallback` para TIM6, y otra definición rompe el link o el tick.
  - Cada diferencia de imagen HAL frente a BM va al informe con su línea de `stm32h7xx_hal_tim.c`.
  - La captura tiene que mostrar que `HAL_GetTick()` sigue avanzando durante y después de las pruebas HAL.

### 6.1 Cierre: el README

Solo cuando el auditor dé F3 por buena y **te lo pida**. No lo adelantes.

- En inglés, en `01_shared_drivers/03_tim/README.md`, con las secciones de "README de cierre" del plan y en ese orden.
- **Como mucho 600 líneas.** El formato de referencia es `00_gpio/README.md`: tablas, una línea por fila, ejemplos cortos que compilan. La concisión de referencia es `02_dma/README.md`.
- **Tiene que llevar:**
  - el diagrama ASCII del banco con solo las líneas que usan las pruebas de TIM;
  - la tabla de líneas, con quién conduce y qué prueba la usa;
  - la guía de configuración paso a paso (reloj → base de tiempo → canal → pin → salida → arranque);
  - la tabla de capacidades por instancia;
  - los comandos para reproducir la validación.
- **Cada afirmación del README sale del código o de una captura.** Cada número lleva su captura con hash. Lo no ejercitado del plan va en su sección, sin suavizarlo.
- La historia lista los hashes de los documentos de proceso. Los borra el responsable del catálogo después: tú no los borras.
- En el mismo commit (`docs(tim): …`), la línea de `03_tim` del árbol de `01_shared_drivers/README.md`, con el mismo estilo que la de `00_gpio`.
- `DRIVER_ROADMAP.md` no se toca.

---

## 7. Cuándo parar y preguntar

Para y pregunta, sin seguir por tu cuenta, si:

- una prueba no se puede hacer como dice el plan (por hardware, porque el control no reproduce el fallo, o por R4);
- una prueba parece necesitar que el MCU y el AD2 conduzcan la misma línea, que Digital Out se use fuera de DIO1/DIO3, o que una línea cambie de dirección respecto a la tabla (S1, S11, S12);
- necesitas una función pública que no está en el contrato del plan, o cambiar una firma, una regla de cálculo, un criterio C o un gate;
- necesitas tocar algo fuera de §2.1, en especial cualquier archivo de `02_firmware_projects`, de otro driver o de la producción de `prj_01`;
- un dato del RM contradice el plan de una forma que cambia lo que hay que construir;
- quieres elegir una opción distinta de la de por defecto en una decisión abierta.

Apunta la pregunta en §5.5 o §5.7 del informe para que quede registrada.
