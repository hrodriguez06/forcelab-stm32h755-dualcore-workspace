# Instrucciones para corregir la reauditoría 3 de 00\_gpio

Sep 30, 2026 · auditor · referencia: `README.md` §12.5 (HEAD `5a5deeb`, revisión validada `1ff10c8`)

La reauditoría 3 verificó la corrección de R2-a, R2-b y R3-a (`88b0cba` … `5a5deeb`).

**Veredicto:** los tres cerrados.

**Lo comprobado, sin discrepancias:**

- **R2-a:** comparación binaria idéntica (mismo sha256 en CM7 y CM4), y ninguna afirmación de replay exacto en el repositorio.
- **R2-b:** la cita del propietario en `1ff10c8`, y 12.10× en las dos corridas.
- **R3-a:**
  - host = CM7 = CM4 en las dos corridas;
  - control 1 abortado antes de S8 con `OE=0x0000`;
  - control 2 con `663ae65-dirty` y un id nuevo;
  - **los tres ids recalculados a mano coinciden con los impresos.**
- **Cabeceras:** los `.h` contra `7b01a10`.
- **README:** cada cifra de la cabecera, §3.1, §9.1–§9.4, §10, §12.4 y §15 contra su captura, salvo las frases de R3-b y R3-c.
- **Desviaciones:** las 1–8 de la entrega aceptadas; la 5 deja el hueco de R3-b.

Quedan dos hallazgos nuevos, R3-b y R3-c. Los dos son de trazabilidad: en esta ronda `01_shared_drivers` no cambió ni una línea de código.

Siguen vigentes las reglas de README §14:

- las firmas y los campos existentes quedan congelados, y solo se añade al final;
- cada test lleva un control negativo que imprime su valor;
- las capturas salen de una sola corrida y no se editan;
- antes de mergear: `build_all.ps1 -Clean` 8/8, dos corridas en banco y los warm resets.

No toques este archivo: se borra cuando lo esencial esté en el README (§3, paso 6).

---

## 0. Antes de empezar

1. **Trabaja sobre `driver/gpio` desde el commit que registra esta reauditoría, o posterior.** No reescribas la historia: nada de `rebase`, `--amend` ni `push --force`. No hagas push.
2. **Un hallazgo por commit, citando el ID**, por ejemplo `fix(prj_01): bind the build id to the commit (R3-b)`. Escribe los mensajes desde bash con heredoc: desde PowerShell, el pipe añade un BOM.
3. **`tools/run_warm_resets.py` es compartido** (`10_hsem` lo usa). Todo cambio en él tiene que dejar su uso actual intacto para quien no pase las opciones nuevas.

---

## 1. Orden de trabajo

| Paso | Hallazgo | Por qué en este orden |
| --- | --- | --- |
| 1 | R3-b | Cambia el id que R3-c imprime en producción |
| 2 | R3-c | Cambia las imágenes de producción y la herramienta de warm resets |
| 3 | — | Banco, regresión, README y borrado de este archivo (§3) |

---

## 2. Qué corregir, y cómo tiene que quedar

### 2.1 R3-b: el host no ata la imagen a su commit

**Hecho.** En un árbol limpio, `FW_BUILD_ID` depende solo de `CMAKE_C_FLAGS` y del preset: es `3c889d2e7e66805d` en `663ae65` y en `1ff10c8`, como documenta README §9.4.

`gpio_hw_check.py` (línea 709) exige que las `Rev` de CM7 y CM4 sean iguales y sin `-dirty`, pero **no** las compara con su propio `HEAD`. Por eso, una imagen compilada limpia en un commit X pasa el control cuando el script corre desde un checkout limpio en otro commit Y.

Eso contradice lo que afirma el README:

- §9.1, fila "Build id": "The image is the one this checkout and `build/selftest-gpio` build";
- §9.4: "The host refuses an image that this checkout and `build/<preset>` would not build".

**Cómo tiene que quedar (opción recomendada: el id cubre el commit):**

- **`cmake/fw_build_id.cmake`:** el payload empieza por `HEAD=<git rev-parse HEAD>` (el hash completo de 40 hex), sin salto de línea. Le siguen, como ahora, los bytes de `git diff --no-color HEAD`, `CMAKE_C_FLAGS=<flags>` y ` PRESET=<preset>`. Si `rev-parse` falla, `unknown`, igual que ahora.
- **`tools/gpio_hw_check.py`, `host_build_id`:** el mismo payload, en el mismo orden y con los mismos bytes.
- **Además, en el host:** antes de S8, comprueba que la `Rev` de CM7 sin `-dirty` sea igual a `git rev-parse --short HEAD`. Si no lo es, `BenchAbort` antes de habilitar ninguna salida. `--allow-dirty` **no** lo dispensa: una corrida `-dirty` es una edición sobre el mismo commit, no una imagen de otro.
- **Documenta el payload nuevo** en la cabecera de `fw_build_id.cmake` y en README §9.4, y quita la frase "it is the same for every commit".

Si se prefiere no tocar el payload, basta con la comprobación de `Rev` frente a `HEAD` en el host. En ese caso §9.4 tiene que explicar que la identidad es el par (Rev, id) y que el host exige los dos. La opción recomendada es mejor porque una imagen se identifica por un solo valor.

**Prueba:**

- **Recálculo:** en una corrida limpia, host = CM7 = CM4, y el id ya no es `3c889d2e7e66805d`. Anota en la captura, o en un fichero aparte, el recálculo manual del id a partir de `HEAD`, el diff vacío, los flags y el preset.
- **Control negativo 3:**
  - compila y flashea `selftest-gpio` en un worktree limpio en el commit anterior (`HEAD~1`);
  - corre `gpio_hw_check.py` desde el checkout en `HEAD`;
  - tiene que abortar antes de S8, sin ninguna línea S8 y con el AD2 en `OE=0x0000` de principio a fin;
  - captura: `docs/gpio_dev_build_id_old_commit_<rev>.txt`.
- **Producción sin cambios por R3-b:** `fw_build_id.h` solo lo incluyen los ficheros del selftest. Demuéstralo con una comparación binaria de los `.bin` de `debug` y `release` antes y después de R3-b (captura `docs/gpio_r3b_bin_cmp_<rev>.txt`). R3-c los cambiará después, y eso es lo que se valida en su paso.

### 2.2 R3-c: las capturas de producción no identifican la imagen

**Hechos:**

- La cabecera del README afirma "every capture carries a build id". Pero las imágenes `debug` y `release` no imprimen ni `FW_GIT_REV` ni `FW_BUILD_ID`: `fw_build_id.h` solo lo incluye el selftest.
- `tools/run_warm_resets.py` escribe `Firmware Git Revision: <HEAD del host>`, sin mirar `-dirty` y sin leer nada de la imagen. El título es texto libre.
- `gpio_warm_resets_{debug,release}_fa44386.txt` y `_1ff10c8.txt` se titulan "00_gpio after re-audit 1, R1-R4". La de `1ff10c8` corresponde a la reauditoría 2.
- `gpio_swd_counters_*.txt` y los logs de compilación tampoco llevan identidad de imagen.

**Cómo tiene que quedar:**

- **Firmware, CPU1** (`CM7/App/Src/app_main.c`): una línea justo después del banner, `[sys] build rev=<FW_GIT_REV> id=<FW_BUILD_ID>`. Incluye `fw_build_id.h`, que `CMakeLists.txt` ya pone en el include path de las dos imágenes.
- **Firmware, CPU2:** publica su `FW_GIT_REV` y su `FW_BUILD_ID` en campos nuevos **al final** de `SharedMemory_t` (`Common/Inc/shared_memory.h`).
  - Los escribe solo CPU2, como sus contadores.
  - Van antes de que CPU2 se declare vivo.
  - CPU1 los imprime como `[sys] cpu2 build rev=<...> id=<...>` cuando lo ve vivo.
  - No muevas ni cambies el tamaño de ningún campo existente. Si el `static_assert` o el tamaño documentado de `g_shared` cambian, actualiza el comentario y README §9.3.
- **`tools/run_warm_resets.py`**, con opciones nuevas y opcionales (sin ellas se comporta como hoy):
  - `--project <dir>` y `--preset <name>`: calcula el id como `gpio_hw_check.py`, reutilizando su función o copiando la definición con una referencia cruzada en los dos ficheros. En cada reset exige que CPU1 y CPU2 impriman la misma `rev` y el mismo `id` que el host. Si no, la corrida falla con el motivo.
  - Registra el estado del árbol como R3: `tree clean`, o el diff con su sha256.
  - El título se compone con la revisión del host y el id calculado; el texto libre de `--title` queda como subtítulo.
- **`gpio_swd_counters_*`:** la cabecera tiene que decir qué imagen estaba flasheada. Basta con leer el id del banner de una de las corridas de warm resets de la misma tanda y citarlo, o con leer la cadena del id de la flash mediante el CLI.
- **README:**
  - cabecera: la frase "every capture carries a build id" pasa a decir qué capturas lo llevan;
  - §9.3: anota que los títulos de `gpio_warm_resets_*_fa44386.txt` y `_1ff10c8.txt` son erróneos y que su "Firmware Git Revision" es el `HEAD` del host, no la imagen. Las capturas no se editan;
  - §9.2: los tamaños de producción nuevos.

**Prueba:**

- warm resets de `debug` y `release` (20/20 cada uno) con `--project` y `--preset`: cada reset muestra la `rev` y el `id` de los dos núcleos iguales a los del host;
- `m7/m4_gpio_lock_fail_count` = 0, con la cabecera identificando la imagen;
- **control negativo 4:** flashea `release` y corre `run_warm_resets.py --preset debug`. Tiene que fallar al primer reset por id distinto (captura `docs/gpio_dev_warm_resets_id_mismatch_<rev>.txt`);
- `build_all.ps1 -Clean` 8/8, con los tamaños nuevos de `prj_01` en la captura. Los demás proyectos no cambian;
- los demás usuarios de `run_warm_resets.py`: una ejecución sin las opciones nuevas en otro proyecto o preset (por ejemplo, el de `10_hsem`) sigue funcionando. Basta con una captura corta.

---

## 3. Entrega

1. `.\tools\build_all.ps1 -Clean`: 8/8. `selftest-gpio` de `prj_01` y `prj_04` compila. Los tres `.c` del driver, con `-Wextra -Wconversion -Wsign-conversion -Wshadow -Wcast-qual`, dan 0 warnings en CM7 y CM4.
2. La comparación binaria de R3-b (§2.1).
3. Dos corridas completas de `gpio_hw_check.py` en la misma revisión limpia, con semillas distintas: `FW VERDICT=PASS` y `HOST VERDICT=PASS` en ambas, y host = CM7 = CM4 con el id nuevo.
4. Los controles negativos 3 (commit antiguo) y 4 (id de producción distinto).
5. Warm resets de `debug` y `release` (20/20 cada uno) con la identidad de la imagen en cada reset, contadores de fallos del lock en 0, y la comprobación de compatibilidad de `run_warm_resets.py`.
6. README:
   - §12.5: cada hallazgo pasa a su estado final con su prueba.
   - Cabecera, §9.1, §9.2, §9.3, §9.4 y §15: actualizados.
   - Cuando todo lo necesario para un usuario esté en el README, borra este archivo en un commit propio, como con los anteriores.

**En la siguiente reauditoría comprobaré:**

- el payload y el recálculo del id nuevo;
- el control 3 abortado antes de S8, y la comprobación de `Rev` frente a `HEAD`;
- que R3-b no cambió las imágenes de producción, y que R3-c sí, solo con las líneas y campos pedidos;
- que cada reset de las capturas de producción identifica los dos núcleos, y el control 4;
- que `run_warm_resets.py` sin opciones nuevas se comporta como antes;
- con el diff contra `5a5deeb`, que los `.h` solo añaden líneas o cambian comentarios;
- y otra vez cada cifra del README contra su captura.
