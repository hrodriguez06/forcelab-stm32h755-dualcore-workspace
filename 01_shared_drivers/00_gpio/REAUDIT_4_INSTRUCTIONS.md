# Instrucciones para corregir la reauditoría 4 de 00\_gpio

Sep 30, 2026 · auditor · referencia: `README.md` §12.6 (HEAD `7fc7ad2`, revisión validada `7456494`)

La reauditoría 4 verificó la corrección de R3-b y R3-c (`f005eda` … `7fc7ad2`).

**Veredicto:** los dos cerrados.

**Lo comprobado, sin discrepancias:**

- **Build ids:** los tres (`991971a381ccb862`, `b761febaca21c7b7`, `8c71cc7386c89b16`) **recalculados a mano** desde `git rev-parse 7456494`, iguales a la captura y a lo impreso.
- **Control 3:** abortado antes de S8 con `OE=0x0000`, y la comprobación `Rev == HEAD` no depende de `--allow-dirty` en el código.
- **Producción:** los `.bin` son idénticos tras R3-b, y R3-c añade solo lo pedido.
- **Warm resets:** 20 de 20 resets con identidad PASS en `debug` y `release`; control 4 con salida 1; compatibilidad sin opciones con el formato antiguo.
- **Cabeceras:** los `.h` contra `5a5deeb`.
- **README:** cada cifra de la cabecera, §9.1–§9.4, §10, §12.5 y §15 contra su captura.

Quedan dos hallazgos de gravedad baja, R4-a y R4-b.

- Los dos son del host.
- Solo afectan a las corridas de desarrollo (`-dirty`, `--allow-dirty`), no a la validación limpia.
- **No bloquean el merge.**
- Si el propietario decide aplazarlos, basta con aplicar la alternativa de §2.3. Pregúntale antes de empezar y cita su respuesta en el primer commit.

Siguen vigentes las reglas de README §14:

- cada test lleva un control negativo que imprime su valor;
- las capturas salen de una sola corrida y no se editan;
- antes de mergear, las comprobaciones de su punto 6.

No toques este archivo: se borra cuando lo esencial esté en el README (§3, paso 5).

---

## 0. Antes de empezar

1. **Trabaja sobre `driver/gpio` desde el commit que registra esta reauditoría, o posterior.** No reescribas la historia: nada de `rebase`, `--amend` ni `push --force`. No hagas push.
2. **Un hallazgo por commit, citando el ID**, por ejemplo `fix(prj_01): bind both cores to the commit under --allow-dirty (R4-a)`. Escribe los mensajes desde bash con heredoc: desde PowerShell, el pipe añade un BOM.
3. **No toques el firmware.** Los dos hallazgos se corrigen en `tools/gpio_hw_check.py` y `tools/run_warm_resets.py`. Si crees que hace falta cambiar firmware, para y pregunta.

---

## 1. Orden de trabajo

| Paso | Hallazgo | Por qué en este orden |
| --- | --- | --- |
| 1 | R4-b | La línea `argv` tiene que estar ya en la captura del control de R4-a |
| 2 | R4-a | Cambia qué dispensa `--allow-dirty` |
| 3 | — | Banco, README y borrado de este archivo (§3) |

---

## 2. Qué corregir, y cómo tiene que quedar

### 2.1 R4-b: las herramientas del host no registran sus argumentos

**Hecho.** `gpio_dev_build_id_old_commit_7456494_allow_dirty.txt` no se distingue de la corrida sin el flag. El abort de R3-b llega antes de la única línea que depende de `--allow-dirty` ("build ids differ; --allow-dirty given"), y ninguna herramienta imprime su línea de comandos. Por eso "con y sin `--allow-dirty`" (§9.1, §9.3) se sostiene por el código, no por la captura (C9).

**Cómo tiene que quedar:**

- **`gpio_hw_check.py`:** la primera línea de toda captura, antes de `host checkout`, es `[host] argv: <argumentos>`. Esos argumentos son `sys.argv[1:]` con comillas de shell (`shlex.join`), sin rutas absolutas del usuario si no son necesarias. Justo después, una línea `[host] options: allow_dirty=<bool> preset=<...> no_reset=<bool>` con los valores ya resueltos por `argparse`, incluidos los valores por defecto.
- **`run_warm_resets.py`:** lo mismo en su cabecera, en los dos modos (con y sin `--project`/`--preset`). En el modo sin opciones, la línea `argv` es la única diferencia admitida con el formato antiguo. Documéntalo en el docstring y en README §9.4.
- **README §9.3:** anota que las capturas anteriores a R4-b no registran sus argumentos, y que en las dos del control 3 la diferencia de flag se sostiene por el código.

**Prueba:** las capturas de §2.2 y de §3 muestran las dos líneas. En la del control de R4-a, `allow_dirty=True`.

### 2.2 R4-a: con `--allow-dirty`, el CM4 no queda atado a nada

**Hecho** (`tools/gpio_hw_check.py`, fase S8):

- `clean` incluye `revs["cm7"] == revs["cm4"]`, y `--allow-dirty` dispensa `clean` entero.
- La diferencia de build id solo se anota con `--allow-dirty`: "build ids differ; --allow-dirty given, going on".
- La comprobación de R3-b calcula `base` solo a partir de la `Rev` del CM7.

Consecuencia: en una corrida `-dirty`, y todas las de mutación lo son, un CM4 flasheado desde otro commit u otro build pasaría sin que la captura lo dijera. Una mutación que afecte a los dos núcleos se daría por probada en el CM4 sin que el CM4 la llevara.

**Cómo tiene que quedar.** `--allow-dirty` dispensa solo dos cosas: que la `Rev` lleve `-dirty`, y que el id de la imagen difiera del id que calcula el host. Todo lo demás se exige siempre, antes de S8 y sin habilitar ninguna salida:

1. `Rev(cm7) == Rev(cm4)`, `-dirty` incluido: las dos imágenes salen del mismo árbol.
2. `id(cm7) == id(cm4)`: salen del mismo directorio de compilación, con un único `generated/fw_build_id.h`. Ninguno puede ser `unknown`.
3. `base` (la `Rev` sin `-dirty`) igual a `git rev-parse --short HEAD`, calculada para los dos núcleos. Con el punto 1 basta con una, pero el mensaje de abort tiene que nombrar el núcleo que falla.

Si falla cualquiera, `BenchAbort` con un mensaje que nombre `cm7` y `cm4` y sus valores. Sin `--allow-dirty` se mantiene todo lo de ahora (id del host igual al de la imagen y `clean`).

- **`run_warm_resets.py --project --preset` ya exige** que los dos núcleos tengan la `rev` y el id del host en cada reset, y no tiene `--allow-dirty`: no cambia, salvo por R4-b.
- **Documenta** en el docstring de `gpio_hw_check.py` y en README §9.4 exactamente qué dispensa `--allow-dirty` y qué no.

**Prueba: control 5**, en un worktree limpio en `HEAD`, con el AD2 conectado pero sin habilitar ninguna salida:

1. Compila `selftest-gpio` en `HEAD`, y en un segundo worktree limpio en `HEAD~1`.
2. Flashea las dos imágenes de `HEAD` (`py tools/flash.py --preset selftest-gpio`) y, después, solo el CM4 de `HEAD~1`: `py tools/flash.py --core cm4 --elf <worktree HEAD~1>/03_firmware_templates/prj_01_template_dualcore/build/selftest-gpio/CM4/cm4_firmware.elf`.
3. Corre `py tools/gpio_hw_check.py --allow-dirty`. Tiene que abortar antes de S8, sin ninguna línea S8 y con `OE=0x0000` de principio a fin. El mensaje debe nombrar la diferencia entre `cm7` y `cm4`, y la línea `options` debe decir `allow_dirty=True`.
4. Captura: `docs/gpio_dev_cm4_other_build_allow_dirty_<rev>.txt`.
5. Vuelve a flashear las dos imágenes de `HEAD` antes de las corridas de §3.

**Control positivo, en la misma tanda:** una corrida `-dirty` real (por ejemplo, una edición de un comentario del selftest, compilada y flasheada en los dos núcleos) con `--allow-dirty`. Tiene que pasar S8, con los dos núcleos en `<rev>-dirty` y el mismo id, y la captura lleva su parche (R3). Basta con que llegue a S8 y termine: su veredicto final no importa.

### 2.3 Alternativa si el propietario aplaza R4-a y R4-b

Sin cambiar código:

- README §10 dice exactamente qué dispensa hoy `--allow-dirty`: la `Rev` del CM4, la igualdad entre núcleos y la comparación de ids.
- README §9.3 marca las capturas `-dirty` y las del control 3 como "sin registro de argumentos".
- §12.6 recoge los dos hallazgos como aceptados, con la cita del propietario.

No hace falta banco.

---

## 3. Entrega

1. **Firmware sin cambios:** el `git diff` desde el commit de registro no toca `CM7/`, `CM4/`, `Common/`, `cmake/` ni `01_shared_drivers/*.c|h`. Con eso no hace falta repetir `build_all` ni los warm resets.
2. **Una corrida completa y limpia** de `gpio_hw_check.py` en la revisión final: `FW VERDICT=PASS` y `HOST VERDICT=PASS`, con las líneas `argv` y `options`, y host = CM7 = CM4.

   Aunque README §14 pide dos corridas antes de mergear, aquí basta una: el firmware es el de `7456494`, ya validado dos veces. Dilo en §9.1.
3. **El control 5 y el control positivo** de §2.2.
4. **Una ejecución corta de `run_warm_resets.py` sin opciones nuevas**, para demostrar que la única diferencia con el formato antiguo es la línea `argv`. Con 1 o 2 resets basta: pon esa cantidad en la captura (el número de resets no es hoy un argumento; si lo añades, que sea opcional y que por defecto siga siendo 20).
5. **README:**
   - §12.6: cada hallazgo pasa a su estado final con su prueba.
   - Cabecera, §9.1, §9.3, §9.4 y §15: actualizados.
   - Cuando todo lo necesario para un usuario esté en el README, borra este archivo en un commit propio, como con los anteriores.

**En la siguiente reauditoría comprobaré:**

- que el firmware no cambió;
- las líneas `argv` y `options` en cada captura nueva;
- el control 5 abortado antes de S8, con `allow_dirty=True`, y el control positivo pasando S8;
- qué dispensa `--allow-dirty`, en el código y en el README;
- el formato sin opciones de `run_warm_resets.py`;
- y otra vez cada cifra del README contra su captura.
