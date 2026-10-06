# Instrucciones para el implementador de 05\_i2c

Oct 6, 2026 · @Hector

Vas a implementar [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1) de esta carpeta: el driver I2C maestro en tres tiers, `i2c_bm` (F1), `i2c_ll` (F2) e `i2c_hal` (F3), con una fase F0 que prepara el banco en `prj_01_template_dualcore`: el AD2 de siempre, el bus I2C en PB8/PB9 y una **NUCLEO-F334R8** como target, con un firmware de banco que también escribes tú. Otra persona audita cada fase y repite tus pruebas en la placa.

Si algo de aquí choca con el plan, manda el plan, salvo lo que este documento **prohíbe**, que sigue prohibido.

**El encargo, en tres frases:** driver de catálogo con BM, LL y HAL; se valida solo en `prj_01_template_dualcore` (más el firmware de banco de la F334, que es un instrumento, no un proyecto validado: plan D2); al final, un `README.md` con el formato de los drivers terminados.

---

## 1. Tu papel

**Tú implementas y pruebas. No auditas.**

- No escribas "auditado", "aprobado", "cerrado" ni nada parecido sobre tu trabajo; lo más es **"entregado para auditoría"**.
- No crees, edites ni borres `05_i2c/AUDIT.md`. Si no estás de acuerdo con un hallazgo, contéstalo en tu informe.
- No marques un hallazgo como cerrado: di "corregido en `<hash>`, pendiente de verificación".
- No empieces la fase siguiente hasta que `AUDIT.md` diga que la actual pasa.

En `06_spi`, la ejecución propia del auditor encontró una carrera del host (B1) que la entrega no mostraba, y la auditoría del README encontró cuatro afirmaciones falsas. El reparto de papeles existe por eso.

---

## 2. Qué puedes tocar y qué no

### 2.1 Lo único que puedes modificar

| Ruta | Qué |
| --- | --- |
| `01_shared_drivers/05_i2c/i2c_{bm,ll,hal}.{h,c}` | El driver (el `.gitkeep` se borra con el primer fuente) |
| `01_shared_drivers/05_i2c/IMPLEMENTATION_REPORT.md` | Tu informe |
| `01_shared_drivers/05_i2c/README.md` | Solo en el cierre, cuando el auditor lo pida |
| `01_shared_drivers/CMakeLists.txt` | Solo los tres targets `_i2c_*` |
| `01_shared_drivers/README.md` | Solo la línea de `05_i2c`, en el cierre |
| `03_firmware_templates/prj_01_template_dualcore/**` | Lo que el plan lista en "Archivos"; en `stm32h7xx_hal_conf.h` de CM7 y en `app_main.c`, solo bajo `#if defined(I2C_DRIVER_SELFTEST)` |
| `tools/i2c_target_f334/**` | El firmware de banco de la F334 y su subconjunto del CMSIS de F3 (con su licencia) |
| `tools/flash.py`, `tools/monitor.py`, `tools/ad2.py` | **Solo añadir** (`--sn`, puerto por SN, disparo por flanco de bajada, decodificador I2C analógico). Sin cambiar el comportamiento de quien ya los usa |

Todo lo demás está fuera (A23: `git diff cc8aa2c --stat`).

### 2.2 Prohibido

| Archivo o acción | Por qué |
| --- | --- |
| `02_firmware_projects/**` | Encargo |
| `DRIVER_ROADMAP.md`, `02_dma/README.md`, `00_gpio/README.md`, `06_spi/README.md` | Los actualiza el responsable en el cierre |
| `05_i2c/AUDIT.md`, el plan, estas instrucciones | El plan es la norma; sus erratas van a tu informe §5.7 |
| Fuentes y README de `00_gpio`, `02_dma`, `03_tim`, `04_uart`, `06_spi`, `10_hsem` | Drivers cerrados: los usas, no los cambias. Si encuentras un defecto en uno, apúntalo y avisa (como N3 en `06_spi`) |
| Producción y evidencia de otros drivers en `prj_01` (`stm32h7xx_it.c`, `mpu_config.c`, linker scripts, `board_def.h`, `shared_memory.[ch]`, `*_selftest*` y `*_hw_check.py` de otros drivers) | Producción o evidencia ajena |
| Escribir `RCC_D2CCIP2R`/`RCC_D3CCIPR` **fuera de T12**, o `RCC_APBxRSTR` | El multiplexor de kernel es del proyecto; T12 lo cambia y lo restaura con read-back |
| Cambiar una firma publicada tras pasar su fase; rebajar un criterio o una prueba | Plan C2 |
| Capturas ya commiteadas | No se editan nunca |
| Merge, borrar la rama, reescribir historia, borrar documentos de proceso | Después del cierre, y no los haces tú |

---

## 3. Cómo trabajar

### 3.1 Rama y commits

- Rama **`driver/i2c`** (desde `main` en `cc8aa2c`). Un commit por preocupación: `build(i2c)`, `feat(i2c)`, `fix(i2c)`, `test(prj_01)`, `feat(tools)`, `fix(tools)`, `docs(i2c)`; asunto en inglés e imperativo; el porqué en el cuerpo; el ID del hallazgo entre paréntesis.
- Mensajes desde bash con `git commit -F - <<'EOF' … EOF` (PowerShell mete un BOM).

### 3.2 Decisiones abiertas

Opción por defecto de cada una (plan, decisiones 1–9). Si crees que otra es mejor, **para y pregunta** antes; en el informe di cuál usaste y, si alguien te aprobó otra, quién y cuándo.

### 3.3 Entorno

- `.\tools\env.ps1` antes de cualquier build (no hay `cmake` suelto). Matriz: `.\tools\build_all.ps1 -Clean`. **Cuidado:** `env.ps1` pisa variables de PowerShell como `$R` o `$root`; usa nombres propios en tus scripts.
- `py`, no `python`; solo biblioteca estándar.
- **Dos ST-LINK a la vez.** Hasta que `flash.py` y el reset del host tengan `--sn` (F0), trabaja con **una sola placa conectada por USB**. Después, toda llamada al programador lleva `sn=` (S18). Obtén los SN con `STM32_Programmer_CLI -l st-link-only`. Si el programador pide actualizar el firmware del ST-LINK de la F334, **para y pregunta**: no lo actualices sin permiso.
- **F334:** antes del primer flasheo, guarda su flash original (`STM32_Programmer_CLI -c port=SWD sn=<SN> -r … `) en un archivo fuera del repositorio y apunta dónde en el informe.
- **CMSIS de F3:** `STMicroelectronics/cmsis_device_f3` v2.3.9 (descarga aprobada por el responsable). Copia solo lo necesario a `tools/i2c_target_f334/cmsis/` (cabeceras del F334, `system_stm32f3xx.h`, el startup de GCC y la licencia); `core_cm4.h` sale del CMSIS del Cube H7.
- **AD2 (SN `210321B5D38D`):** GUI de WaveForms cerrada. En este banco el AD2 solo usa `HOST_REQ` y sus dos canales de osciloscopio.
- **Documentos:** RM0399 cap. 50 en `00_docs_stm32h7/docs/peripherals/i2c/` (cítalo `50:<línea>`); ES0445 §2.19 y DS12923 en `00_core_and_system/`; HAL y LL de I2C en `00_docs_stm32h7/docs/01_drivers_and_hal/STM32CubeH7/`.
- Capturas en frío y sin depurador; no leas el banco `RLR` del HSEM con el depurador. Placa muda tras un arranque en frío: mira `PWR_CR3`.

### 3.4 Capturas

1. Una sola ejecución de `prj_01/tools/i2c_hw_check.py`, con marcas de tiempo, de `[host] argv:` a `HOST VERDICT=`.
2. Identidad completa (S18, A21): `Rev` e id de build iguales en CM7, CM4 y host, sin `-dirty`; banner de la F334 con su Rev e id; los SN de los dos ST-LINK.
3. Checklist confirmado, con las líneas del bus I2C, del estado del banco (`--bench-state taps|notaps`) y de la F334 encendida.
4. No se edita. Una repetición va en un archivo nuevo.
5. Cada número del informe sale de una captura o de un `.map` del repo.
6. Nombre: `docs/i2c_<fase>_<prueba>_<estado>_<hash>.txt` (por ejemplo `i2c_f1_bench_taps_1a2b3c4.txt`); desarrollo: `i2c_dev_…`. **Cada fase entrega dos sesiones**: una "taps" y una "notaps" (plan D3).

### 3.5 Pruebas

- Cada prueba imprime su control negativo; si el control no reproduce el fallo, la prueba falla (salvo la decisión 6, que se imprime como "no reproducido").
- El oráculo nunca es el driver: la F334, el osciloscopio decodificado por el host, `CYCCNT`, registros leídos directamente, tablas del RM a mano.
- **El host no depende del orden de llegada de las líneas** (lección B1): cuando un paso necesita que el host lea un estado, el firmware lo mantiene hasta un pulso de `HOST_REQ` (`sync_ack_low`), desde el protocolo v1.
- **Toda espera tiene salida** si `CYCCNT` o `HAL_GetTick()` dejan de avanzar (lecciones m1, m4), también en el selftest y en el firmware de la F334.
- **Un hecho de silicio que vaya al contrato o al README necesita su prueba en una captura de entrega** (lección E2/T15), nunca solo una de desarrollo.
- No destructivas: nunca TIM6, SysTick, `DBGMCU`, `RCC_APBxRSTR`; `RCC_D2CCIP2R` solo en T12, guardado y restaurado con read-back; `SYSCFG_PMCR` solo con RMW de un bit y restaurado; nunca PE0, PE1, PC13, PA4, PA7, PA13, PA14; cada prueba termina con `Deinit` (T14) y el bus en reposo (S17).
- Propiedad: CM4 tiene I2C1; CM7, I2C2–I2C4. Los pines del bus los toma en cada paso un solo núcleo (S16).
- Buffers de BDMA solo desde CM7 en `.d3_ram`; DMA1/2 en `.dma_buf` (CM7) o en la SRAM2 de CM4; alineados a 32 bytes.
- El veredicto lo calcula CM7 cuando CM4 ha terminado; el host añade las comprobaciones de la F334 y del osciloscopio.

### 3.6 Seguridad eléctrica

Entre la H755, la F334 y el AD2 no hay resistencias en serie. Valen S1–S14 tal como están; este driver añade S15–S21 (plan, "Reglas nuevas"):

- **S15, solo open-drain:** ningún pin del bus en push-pull ni con pull interno, en ninguna de las dos placas. PF14/PF15 de la H755, solo entrada sin pull. Escríbelo primero en `bench_claim()` de los dos núcleos y en el firmware de la F334, antes que ninguna prueba.
- **S16:** una sola instancia de la H755 en AF sobre PB8/PB9 a la vez, de un solo núcleo, con la instancia dueña leída antes (reloj, `PE = 0`).
- **S17:** niveles del bus comprobados antes de conducir y tras cada paso.
- **S18:** cada llamada al programador con `sn=`; puertos por SN; los otros bancos con el USB de la F334 desconectado.
- **S19:** el estado del banco declarado y comprobado; 1 MHz solo sin tomas.
- **S20:** las dos placas por USB del mismo PC, ningún riel entre placas, GND primero; flashear o resetear solo con el host parado.
- **S21:** la F334 retiene una línea solo en open-drain, en un paso anunciado y con su propio límite de tiempo.
- **No cambies el cableado** ni pongas o quites J2/J3 tú: lo hace el responsable entre sesiones.
- **Si algo sale raro** (niveles fuera de S17, un `ABORT`, una placa que no responde), para, deja todo en estado seguro, desconecta el bus y apúntalo antes de reintentar.

A27 lo revisa todo; un incumplimiento es bloqueante aunque las pruebas pasen.

---

## 4. Qué entregar al terminar cada fase

1. El rango de commits en `driver/i2c`, con el árbol limpio.
2. `build_all -Clean` (8/8), `selftest-i2c` (0 avisos), compilación aparte con `CMAKE_C_FLAGS=-Werror`, y el build del firmware de la F334 (0 avisos).
3. Tamaños del `.map` (sin "Discarded input sections"): cada `i2c_*.c.obj` en CM4 y CM7 y la producción de `prj_01` frente a `2481396`.
4. Las dos capturas de la fase (taps y notaps), commiteadas.
5. El informe con la sección de la fase.
6. Un aviso al auditor con el hash del último commit.

---

## 5. El informe: `05_i2c/IMPLEMENTATION_REPORT.md`

La plantilla y las reglas de redacción son las de `06_spi` (recupérala con `git show 18b4abd^:01_shared_drivers/06_spi/IMPLEMENTER_INSTRUCTIONS.md`, §5): hechos y no valoraciones, cada número con su fuente, sin palabras de auditoría, lo que falló tan visible como lo que salió bien, y cada identificador comprobado con `git grep`. Añade a la plantilla:

- en §5.2, el tamaño del firmware de la F334;
- en §5.3, una columna "Estado del banco" (taps/notaps) y otra "Oráculo F334/AD2";
- una sección "Constantes y funciones de `stm32h7xx_ll_i2c.h` que usa `i2c_ll`" (F2) y "Líneas de `stm32h7xx_hal_i2c.c` en las que se apoya `i2c_hal`" (F3), como en `06_spi`;
- en §5.8, **todo** lo no ejercitado de la fase: el README del cierre lo copiará entero (lección M6).

---

## 6. Lo que te toca por fase

- **F0:**
  - Confirma con el responsable que el bus está colocado y las comprobaciones de la F334 y la H755 hechas (plan, "El bus I2C").
  - Primero, `--sn` en `flash.py` y la selección de puerto por SN (S18); después, el respaldo de la flash de la F334 y su firmware de banco con S15 y S21; después, las guardas de la H755 y `i2c_hw_check.py`.
  - Sin código del driver ni targets `_i2c_*`.
- **F1:**
  - Tabla de capacidades con la línea del RM en cada fila; validación C6 y `I2C_BM_Check*`; `CalcTiming` con T3 desde el principio, porque es donde es más fácil desviarse del plan.
  - El primer byte de una escritura se precarga antes de `START` (2.19.6), también en DMA.
  - `BERR` en maestro se borra y no aborta (2.19.2).
  - `ICR` se escribe una vez con los bits a borrar; nunca `ICR |=`.
  - `RecoverBus` no conoce pines: recibe funciones del llamador.
- **F2:** `i2c_bm.h` solo añade; LL valida con `I2C_BM_Check*`; paridad bit a bit.
- **F3:** `HAL_I2C_MODULE_ENABLED` y `USE_HAL_I2C_REGISTER_CALLBACKS 0U` solo bajo la opción, en CM7; `I2C_HAL_Init` llama a `I2C_BM_Deinit` antes de `HAL_I2C_Init`; sin callbacks ni MSP; `I2C_HAL_Wait` con salida si el tick se para; cada diferencia con BM, con su línea de `stm32h7xx_hal_i2c.c`; `HAL_GetTick()` avanza en la captura.

### 6.1 Cierre: el README

Solo cuando el auditor lo pida. En inglés, ≤ 600 líneas, secciones del plan. Además de lo del plan:

- atribuye cada evidencia a la función que la produjo (lección M3);
- cita los umbrales del RM exactos (M4);
- di qué referencia usa cada prueba (M5);
- copia **todo** lo no ejercitado de tu informe (M6);
- di qué controles no se reproducen en todas las ejecuciones (m5);
- di en qué núcleos se compila cada tier (m6);
- los ejemplos compilan con `-Wall -Wextra -Wpedantic -Wconversion -Werror` y los pines que usan están en el banco, o se dice que no;
- los documentos de proceso, con la forma de recuperación `git show $(git log -1 --format=%h --diff-filter=D -- <ruta>)^:<ruta>`.

---

## 7. Cuándo parar y preguntar

- una prueba no se puede hacer como dice el plan;
- algo parece exigir push-pull en el bus, dos conductores en una línea, o tocar J2/J3 o el cableado;
- el programador pide actualizar el ST-LINK de la F334, o aparece un tercer ST-LINK;
- necesitas una función pública fuera del contrato, o cambiar una firma, una regla de cálculo, un criterio o un gate;
- necesitas tocar algo fuera de §2.1;
- un dato del RM, del DS o de los documentos de la F334 contradice el plan;
- quieres una opción distinta de la de por defecto.

Apunta la pregunta en §5.5 o §5.7 del informe.
