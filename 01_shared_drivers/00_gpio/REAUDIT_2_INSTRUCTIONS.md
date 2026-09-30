# Instrucciones para corregir la reauditoría 2 de 00\_gpio

Sep 30, 2026 · auditor · referencia: `README.md` §12.4 (HEAD `7b01a10`, revisión validada `fa44386`)

La reauditoría 2 verificó la corrección de R1–R4 (`31406fa` … `7b01a10`).

**Veredicto:** R1, R3 y R4 cerrados. R2 cerrado, con el criterio de replay aceptado. Motivo: el objetivo de R2 (una muestra de entrelazado distinta por corrida) se cumple, y el replay era solo una ayuda de depuración.

**Lo comprobado, sin discrepancias:**

- **Congelamiento de API:** contra `1a635d8`, los `.h` solo pierden líneas de comentario, y `GPIO_BM_ERR_NO_CLOCK` es el último estado.
- **C8:** se mantiene.
- **Tests nuevos:** el caso C y su control, las verdicts de semilla y de plazo, y las mutaciones M-R1 y M-R2.
- **R3:** los sha256 de las siete capturas `-dirty` de `fa44386`, recalculados sobre su contenido.
- **README:** cada cifra de §2, §6, §9.1–§9.4, §10, §12.3 y §15 contra su captura.
- **Alcance:** el código de `1ba0f65` es idéntico al de `fa44386`. No hay rewrites ni push.

Quedan tres hallazgos nuevos, R2-a, R2-b y R3-a. Ninguno es un defecto del driver.

Siguen vigentes las reglas de README §14:

- las firmas y los campos existentes quedan congelados, y solo se añade al final;
- `gpio_bm` sigue siendo solo CMSIS;
- cada test lleva un control negativo que imprime su valor;
- las capturas salen de una sola corrida y no se editan;
- antes de mergear: `build_all.ps1 -Clean` 8/8, dos corridas en banco y los warm resets.

No toques este archivo: se borra cuando lo esencial esté en el README (§3, paso 5).

---

## 0. Antes de empezar

1. **Trabaja sobre `driver/gpio` desde `7b01a10` o posterior.** No reescribas la historia: nada de `rebase`, `--amend` ni `push --force`. No hagas push.
2. **Un hallazgo por commit, citando el ID**, por ejemplo `fix(prj_01): stamp a build id into the image and the banner (R3-a)`. Escribe los mensajes desde bash con heredoc: desde PowerShell, el pipe añade un BOM.
3. **R2-b empieza con una decisión del propietario** (§2.2). Pregúntale antes de tocar el límite y cita su respuesta en el mensaje del commit.

---

## 1. Orden de trabajo

| Paso | Hallazgo | Por qué en este orden |
| --- | --- | --- |
| 1 | R2-a | Solo comentarios; se demuestra con una comparación binaria, sin banco |
| 2 | R3-a | Cambia el banner y el host; las corridas de R2-b tienen que llevarlo ya |
| 3 | R2-b | Cambia el hook de producción: banco completo |
| 4 | — | Banco, README y borrado de este archivo (§3) |

---

## 2. Qué corregir, y cómo tiene que quedar

### 2.1 R2-a: dos comentarios afirman un replay exacto

`CM7/App/Src/gpio_selftest.c` afirma lo que `gpio_dev_replay_seed_d9d40bdd_fa44386.txt` refuta (`c_raw_both` 59 668 frente a 59 141):

- **línea 670**, sobre `GST_RACE_SEED`: "the replay image has the timing of the image it replays";
- **línea 870**, sobre el plazo de arranque: "so the seed alone decides the interleaving and a replay replays".

README §10 lo dice bien.

**Cómo tiene que quedar:** los dos comentarios dicen lo que se midió:

- la imagen del replay difiere solo en los 4 bytes de la semilla (`gpio_seed_replay_bin_cmp_fa44386.txt`);
- el replay reproduce la secuencia de retardos, no el entrelazado, y la fuente de la diferencia que queda no está aislada;
- el plazo fijo quita la variación del arranque del HSI48/RNG (`gpio_dev_deadline_*_7cedf36-dirty*`).

Ningún otro comentario ni texto del repositorio puede afirmar que el replay es exacto: búscalo con `grep`.

**Prueba, sin banco.** Compila `selftest-gpio` en un worktree limpio en el commit anterior y en el de R2-a. Compara con `cmp` los `.bin` de CM7 y de CM4.

- Si son idénticos, no hace falta banco. Guarda la salida como `docs/gpio_r2a_bin_cmp_<rev>.txt`.
- Aviso: `FW_GIT_REV` va compilado en la imagen, así que las dos imágenes difieren como mínimo en la cadena de la revisión. Compila las dos con la misma `FW_GIT_REV`, pasándola en la línea de comandos solo para esta comparación. El único otro cambio admitido es el de esa cadena, y la captura debe decir cuál es.
- Si difiere algo más, el commit tocó código: rehazlo.

### 2.2 R2-b: B1 está en su límite y depende del layout

**Hecho medido.** La puerta es `hold × 10 ≤ SHARED_GPIO_LOCK_US`. Con 992 ciclos en el CM4 (4 960 ns frente a 5 000 ns), el margen es de 80 ciclos.

README §10 dice que la causa de la subida (841 → 992) no se aisló y menciona el jitter. **Las capturas apuntan al layout del enlazado, no al jitter.** La subida también aparece en B1, que se mide antes de T15, sin contención ni jitter (`gpio_selftest_0cd803f.txt` frente a `gpio_selftest_fa44386.txt`):

| B1, máximo | `0cd803f` | `fa44386` |
| --- | --- | --- |
| CM7 HAL, sin hook | 1264 | 1347 |
| CM7 HAL, con hook | 1559 | 1739 |
| CM4 HAL, sin hook | 1364 | 1468 |
| CM4 HAL, con hook | 1875 | 1983 |

`gpio_hal.c` no cambió. `gpio_bm` creció 62 B (1656 → 1718) y desplazó lo que se enlaza detrás. Por eso cualquier cambio de código, aunque no toque el driver, puede tumbar B1.

**Primero, confirma la causa.** Haz una corrida de desarrollo en `fa44386-dirty`, con R3 registrando el parche. Añade un relleno de 62 B (por ejemplo, una función `static` que no se use, marcada `__attribute__((used))`) al final de `gpio_bm.c`, o quita el mismo espacio antes de `gpio_hal.c`.

- Si el máximo de B1 de HAL se mueve de forma apreciable con el relleno, la causa queda confirmada: layout.
- Guarda la captura como `docs/gpio_dev_layout_pad_<rev>-dirty.txt`.
- Si no se mueve, dilo en el README y deja la causa como "no aislada".

**Después, la decisión del propietario** (pregúntale y cita su respuesta):

- **Opción A (recomendada):** `SHARED_GPIO_LOCK_US` = 60 µs en `Common/Inc/shared_memory.h`. Da 12,1× con 992 ciclos y un margen de unos 200 ciclos sobre la puerta.
  - Coste: la cota de interrupciones enmascaradas (A2) sube 10 µs. Actualízala en README §3.1 y §10.
  - Actualiza el comentario del define con los valores medidos.
  - `SHARED_GPIO_LOCK_CYCLES_CM7` y `_CM4` se recalculan solos.
- **Opción B:** mantener 50 µs. README §10 pasa a decir que la puerta de B1 depende del layout, que el margen actual es de 80 ciclos, y qué hacer cuando falle: subir el límite, nunca bajar la puerta.

Ninguna opción cambia la puerta de 10×.

**Prueba (opción A):**

- dos corridas completas: B1 PASS, con el margen impreso;
- warm resets de `debug` y `release` (20/20);
- `m7/m4_gpio_lock_fail_count` = 0.

El hook de producción cambia, así que la regresión de producción es obligatoria.

### 2.3 R3-a: la captura no identifica la imagen que se ejecutó

`[host] tree clean` y el diff de R3 describen el checkout **donde corre el script**, no el árbol ni los flags con que **se compiló** la imagen. Esto tiene dos consecuencias:

- `gpio_dev_replay_seed_d9d40bdd_fa44386.txt` dice `Rev fa44386` y `tree clean` para una imagen compilada con `-DGST_RACE_SEED`. Solo lo delata la línea de la semilla.
- `FW_GIT_REV` se calcula con `execute_process` al **configurar** (`CMakeLists.txt`, líneas 33–54). Una edición hecha después de `cmake --preset`, sin reconfigurar, compila con la revisión de antes y sin `-dirty`.

**Cómo tiene que quedar:**

- **Build id calculado al compilar, no al configurar.** Un paso de compilación que se ejecute siempre (por ejemplo, `add_custom_target(... ALL)` que llame a `cmake -P fw_build_id.cmake`) escribe un `fw_build_id.h` en el directorio de compilación. El fichero solo se reescribe si el contenido cambia, para no forzar recompilaciones.
- **Contenido del header:**
  - `FW_GIT_REV`, con la misma regla de `-dirty` que ahora;
  - `FW_BUILD_ID`: los 16 primeros hex del sha256 de la concatenación de `git diff --no-color HEAD` (ficheros versionados), `CMAKE_C_FLAGS` y el nombre del preset.
  - Si `git` falla, la cadena `unknown`: nunca un id inventado.
- **Firmware:**
  - el CM7 imprime `Build id <FW_BUILD_ID>` junto a `CM7 Rev`;
  - el CM4 lo publica en un campo nuevo **al final** de `GstArea_t`, igual que `m4_rev`;
  - el CM7 imprime `CM4 build id ...`.
- **Host (`gpio_hw_check.py`):**
  - calcula el mismo id sobre su checkout, con los flags del preset leídos de `build/<preset>/CMakeCache.txt`;
  - imprime `[host] build id host=<x> cm7=<y> cm4=<z>`;
  - si no coinciden, `BenchAbort` antes de habilitar ninguna salida (la misma fase que S8), salvo con `--allow-dirty`, que lo anota y sigue.
- **Documenta** en README §9.3 que las capturas anteriores a R3-a identifican la imagen solo por `Rev`, y marca como tal la del replay de `fa44386`.

**Prueba:**

- una corrida limpia muestra los tres ids iguales;
- control negativo, **sin AD2 conectado** o antes de S8: la imagen compilada con `-DGST_RACE_SEED=...` y el checkout limpio dan ids distintos, y el host aborta antes de S8 (captura `docs/gpio_dev_build_id_mismatch_<rev>.txt`);
- control negativo de configurar y luego editar: tras `cmake --preset`, edita un comentario, compila sin reconfigurar y comprueba que la imagen dice `-dirty` y otro id.

### 2.4 Opcional: centinelas del caso C

En `run_t16_a1`, `cb_ref = cb` y `cl_ref = cl` copian por asignación, y el `memcmp` también compara el padding. Solo puede dar un falso FAIL, nunca un falso PASS. Si tocas ese fichero por otro motivo, usa `memcpy(&cb_ref, &cb, sizeof cb)`. No merece un commit propio.

---

## 3. Entrega

1. `.\tools\build_all.ps1 -Clean`: 8/8. `selftest-gpio` de `prj_01` y `prj_04` compila. Los tres `.c` del driver, con `-Wextra -Wconversion -Wsign-conversion -Wshadow -Wcast-qual`, dan 0 warnings en CM7 y CM4.
2. La comparación binaria de R2-a (§2.1).
3. Dos corridas completas de `gpio_hw_check.py` en la misma revisión limpia, con semillas distintas y los tres build ids iguales: `FW VERDICT=PASS` y `HOST VERDICT=PASS` en ambas.
4. Warm resets de `debug` y `release` (20/20 cada uno) y contadores de fallos del lock en 0.
5. Las capturas de R2-b (relleno de layout) y de R3-a (los dos controles negativos), con su parche en la captura (R3).
6. README:
   - §12.4: cada hallazgo pasa a su estado final con su prueba.
   - §3.1, §9.1, §9.2, §9.3, §10 y §15: actualizados.
   - Cabecera: la línea de estado cita la nueva revisión.
   - Cuando todo lo necesario para un usuario esté en el README, borra este archivo en un commit propio, como con `REAUDIT_1_INSTRUCTIONS.md`.

**En la siguiente reauditoría comprobaré:**

- que R2-a no cambió el binario salvo la cadena de revisión;
- la decisión de R2-b citada en su commit, y el margen nuevo en las dos corridas;
- que el build id coincide entre host, CM7 y CM4 en las corridas limpias, y que los dos controles negativos lo detectan;
- con el diff contra `7b01a10`, que los `.h` solo añaden líneas o cambian comentarios;
- y otra vez cada cifra del README contra su captura.
