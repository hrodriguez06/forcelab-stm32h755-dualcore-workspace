# Instrucciones para el implementador de `00_gpio` F1c (G1 en `prj_03`)

Oct 13, 2026 · @Hector

Vas a implementar [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1) de esta carpeta: instalar el lock de GPIO entre núcleos en `prj_03_adc_dma_eth` (H1) y configurar el pin del ADC de CM4 por el driver (H2), y extender `tools/gpio_f1b_checks.py` con `--project`. Es lo mismo que F1b hizo en `prj_04`; el plan lista las diferencias. Ninguna firma cambia. Otra persona audita la fase y repite tus pruebas en la placa.

Si algo de aquí choca con el plan, manda el plan, salvo lo que este documento **prohíbe**, que sigue prohibido.

---

## 1. Tu papel

**Tú implementas y pruebas. No auditas.**

- No escribas "auditado", "aprobado", "cerrado" ni nada parecido sobre tu trabajo; lo más es **"entregado para auditoría"**.
- No crees, edites ni borres `00_gpio/AUDIT.md`. Si no estás de acuerdo con un hallazgo, contéstalo en tu informe.
- No marques un hallazgo como cerrado: di "corregido en `<hash>`, pendiente de verificación".
- No empieces el cierre hasta que `AUDIT.md` diga que F1 pasa.
- **No afirmes en el informe nada que una captura no muestre.** En F1b, el informe dio por cubiertas comprobaciones que la herramienta no hacía, y eso costó dos rondas.

---

## 2. Qué puedes tocar y qué no

### 2.1 Lo único que puedes modificar

| Ruta | Qué |
| --- | --- |
| `02_firmware_projects/prj_03_adc_dma_eth/Common/Inc/shared_memory.h`, `Common/Src/shared_memory.c` | H1 |
| `prj_03/CM7/Core/Src/main.c`, `prj_03/CM4/Core/Src/main.c` | H1 (`HSEM_BM_Init`, `HSEM_BM_TimebaseStart`, `Shared_GpioLockInstall`) |
| `prj_03/CM7/CMakeLists.txt`, `prj_03/CM4/CMakeLists.txt` | H1: solo enlazar `stm32h7_driver_hsem_bm` |
| `prj_03/CM4/App/Src/adc_acquisition.c` | H2 |
| `tools/gpio_f1b_checks.py` | `--project` y los menores de F1b (plan, Diseño 4) |
| `prj_03/docs/gpio_f1c_*`, `prj_04/docs/gpio_f1c_t7_*` | Las capturas de la fase |
| `01_shared_drivers/00_gpio/IMPLEMENTATION_REPORT.md` | Tu informe |
| `00_gpio/README.md`, `prj_03/README.md` | Solo en el cierre, cuando el auditor lo pida |

Todo lo demás está fuera (`git diff e6cb75e --stat`).

### 2.2 Prohibido

| Archivo o acción | Por qué |
| --- | --- |
| Todo `01_shared_drivers/**` (fuentes, headers, `CMakeLists.txt`) | El driver y los demás ya hacen lo que hace falta. Si crees que necesitan un cambio, para y pregunta |
| El arranque de CPU2 de `prj_03` (`HAL_HSEM_FastTake`/`Release`, `CPU2_WaitForCPU1`, la espera de `D2CKRDY`) | Decisión 1: no cambia |
| `SystemClock_Config`, la configuración de alimentación, los option bytes | Nada que solo se recupere a mano (G-S7) |
| Otros proyectos (`prj_01`, `prj_02`, `prj_04`, `prj_eth_lwip_node`) | Fuera de alcance. `prj_04` solo se usa para T7 (sin flashear) |
| Campos existentes de `SharedMemory_t` (orden, tipo, tamaño) | Otros los leen por desplazamiento: solo añades al final |
| Cambiar el comportamiento de la herramienta para `prj_04` salvo los menores del plan | T7 lo comprueba |
| `DRIVER_ROADMAP.md`, el plan, estas instrucciones, `AUDIT.md` | El plan es la norma; sus erratas van a tu informe §5.7 |
| Flashear el preset `selftest` de `prj_03` | G-S1 |
| Cambiar la configuración de red del PC | La configura el responsable (G-S4) |
| Capturas ya commiteadas | No se editan nunca |
| Merge, borrar la rama, reescribir historia, borrar documentos de proceso | Después del cierre, y no los haces tú |

---

## 3. Cómo trabajar

### 3.1 Rama y commits

- Rama **`driver/gpio-f1c`** (desde `main` en `e6cb75e`; el plan está en ella). Un commit por preocupación: `fix(prj03)`, `build(prj03)`, `feat(tools)`, `docs(gpio)`; asunto en inglés e imperativo; el porqué en el cuerpo; el ID (H1, H2, G1) entre paréntesis.
- Mensajes desde bash con `git commit -F - <<'EOF' … EOF` (PowerShell mete un BOM).

### 3.2 Decisiones abiertas

Opción por defecto de cada una (plan, decisiones 1–4), salvo que el responsable haya aprobado otra; lo dice el plan. Si crees que otra es mejor, **para y pregunta** antes.

### 3.3 Entorno

- `.\tools\env.ps1` antes de cualquier build. Matriz: `.\tools\build_all.ps1 -Clean`; el preset `selftest` de `prj_03` se compila aparte. `env.ps1` pisa variables de PowerShell como `$R` o `$root`: usa nombres propios.
- **Salidas en UTF-8.** En PowerShell, `>` escribe UTF-16, así que usa `| Out-File -Encoding utf8` o ejecuta desde bash. En F1b, la captura de T1 salió con dos codificaciones mezcladas.
- `py`, no `python`; solo biblioteca estándar.
- **Dos ST-LINK conectados:** `sn=0016000C4142501220353451` (la H755) en cada llamada a `STM32_Programmer_CLI`, a `tools/flash.py --sn` y a `monitor.py --sn`.
- **SWD:** T3 lee sin parar los núcleos (`mode=HOTPLUG`) y no usa el depurador. **Nunca leas `HSEM_RLR`:** leerlo toma el semáforo.
- **El control** (`e6cb75e`) se compila en un worktree aparte (`git worktree add ../stm32h755_control_e6cb75e e6cb75e`), nunca en tu árbol. Si para el submódulo de Cube enlazas el del repo, dilo en el informe.
- **Documentos:**
  - `00_gpio/README.md` §3, §10, §12.2 y §13.1;
  - el hook de `prj_04` (`Common/`, sus dos `main.c`) y el commit `53f0f88` (su H2);
  - `10_hsem/README.md` §3;
  - RM0399 en `00_docs_stm32h7/docs/`.

### 3.4 Capturas

1. **Contenido:** una sola ejecución por captura, sin editar y sin montar a mano, con la línea de comando, la fecha y el hash del árbol limpio. La salida de cada herramienta tal cual; los juicios, en el informe.
2. **Identidad de la imagen:** cada sesión de placa empieza flasheando la ELF compilada en ese commit, con su "Download verified successfully" en la captura. Ni recompilación ni otro flasheo en medio.
3. **Lo leído:** toda dirección que se lee se imprime junto a su origen (`nm` de qué ELF, DWARF, header).
4. **Nombre:** `prj_03/docs/gpio_f1c_<prueba>_<hash>.txt`; T7, en `prj_04/docs/gpio_f1c_t7_<hash>.txt`. Se commitean todas, también las que fallen.
5. **Números:** cada número del informe sale de una captura o de un `.map` del repo.

### 3.5 Pruebas

- **Controles:** cada prueba tiene un control negativo que tiene que poder fallar de verdad, pasado por la misma comparación que el caso bueno.
- **Comprobaciones:** cada una que la herramienta imprime cuenta en su veredicto. Nada de "PASS" fijo ni de condiciones que se cumplen siempre.
- **Fallos de herramienta:** un fallo de `nm`, de `gcc -E` o de SWD, un archivo, un símbolo o un preset que falta es FAIL. Y lo que no se pueda derivar no se rellena con un valor escrito a mano.
- **T5:** secuencial, como F1b. En `prj_03`, `[net]` no tiene `acl_drop` y `OVR` es `half_overrun_count`. Dilo en la salida.
- **T6a:** los 20 resets por SWD los hace la herramienta, con los lectores parados (G-S5).
- **T6b:** necesita al responsable en el banco; pídeselo cuando todo lo demás esté hecho. Si está en remoto, queda en tu informe §5.8.

### 3.6 Seguridad en la placa

- **G-S1 antes del primer flasheo de `prj_03`:** lista cada pin que configuran sus dos núcleos en debug y release y comprueba que ninguno es una línea del banco: AD2 en PE2–PE15 y PA8; J1 en PA5, PA6, PB4, PB5; bus I2C en PB8, PB9; tomas en PF14, PF15. Pon la lista en el informe. Si alguno coincide, para y pregunta.
- **G-S3:** el AD2 en estado seguro: no corras ningún script del AD2.
- **G-S5:** flashear o resetear solo con `monitor.py`, `net_check.py` y `query_adc.py` parados.
- **G-S7, si el responsable está en remoto:** nada que solo se recupere a mano. Para resetear, `-rst` o `-hardRst`; para conectar a una placa que no responde, `mode=UR`. Si ni así responde por SWD, para y avisa.
- **Antes de la primera sesión**, comprueba la red con `net_check.py` sobre la imagen que haya en la placa (`prj_04` debug, `192.168.1.150`); si no contesta, para y avisa.

---

## 4. Qué entregar al terminar F1

1. El rango de commits en `driver/gpio-f1c`, con el árbol limpio.
2. Las capturas de T1–T5, T6a y T7, commiteadas, y los `.text` de `adc_acquisition.c.obj` y `shared_memory.c.obj` de `prj_03` antes (`e6cb75e`) y después.
3. El informe `00_gpio/IMPLEMENTATION_REPORT.md` con la sección F1.
4. Un aviso al auditor con el hash del último commit: "F1 entregada para auditoría".

## 5. El informe

Como el de F1b (recupéralo con `git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/00_gpio/IMPLEMENTATION_REPORT.md)^:01_shared_drivers/00_gpio/IMPLEMENTATION_REPORT.md`): hechos y no valoraciones, cada número con su fuente, lo que falló tan visible como lo que salió bien.

Secciones:
- 5.1 commits;
- 5.2 builds y tamaños;
- 5.3 pruebas, con el oráculo y el control de cada una;
- 5.4 la lista de escritores de GPIO de `prj_03` antes y después (núcleo, archivo, puerto, antes o después de liberar a CPU2) y la de pines de G-S1;
- 5.5 desviaciones, con quién se consultó y cuándo;
- 5.6 respuesta a la auditoría;
- 5.7 erratas del plan;
- 5.8 lo abierto y lo no ejercitado.

## 6. Cuándo parar y preguntar

- una prueba no se puede hacer como dice el plan;
- un pin de `prj_03` coincide con una línea del banco (G-S1);
- una corrección parece pedir un cambio en `01_shared_drivers` o en el arranque de CPU2;
- `HSEM_BM_Init` o `HSEM_BM_TimebaseStart` fallan en algún núcleo;
- `s_shared_lock` de un núcleo no se puede leer por SWD;
- T4 da menos de 10× con 60 µs;
- la placa no responde por SWD ni bajo reset, o la red deja de contestar;
- encuentras otro escritor de configuración de GPIO que el plan no lista;
- quieres una opción distinta de la de por defecto.

Apunta la pregunta en §5.5 o §5.7 del informe.
