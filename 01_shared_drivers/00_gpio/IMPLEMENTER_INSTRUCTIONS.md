# Instrucciones para el implementador de `00_gpio` F1b (G1, G13 en `prj_04`)

Oct 12, 2026 · @Hector

Vas a implementar [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 3) de esta carpeta: instalar el lock de GPIO entre núcleos en `prj_04_adc_dma_bm_eth` y pasar por el driver a todos los escritores de configuración de GPIO que quedan fuera (H1–H5). Ninguna firma cambia. Otra persona audita la fase y repite tus pruebas en la placa.

Si algo de aquí choca con el plan, manda el plan, salvo lo que este documento **prohíbe**, que sigue prohibido.

---

## 1. Tu papel

**Tú implementas y pruebas. No auditas.**

- No escribas "auditado", "aprobado", "cerrado" ni nada parecido sobre tu trabajo; lo más es **"entregado para auditoría"**.
- No crees, edites ni borres `00_gpio/AUDIT.md`. Si no estás de acuerdo con un hallazgo, contéstalo en tu informe.
- No marques un hallazgo como cerrado: di "corregido en `<hash>`, pendiente de verificación".
- No empieces el cierre hasta que `AUDIT.md` diga que F1 pasa.

---

## 2. Qué puedes tocar y qué no

### 2.1 Lo único que puedes modificar

| Ruta | Qué |
| --- | --- |
| `02_firmware_projects/prj_04_adc_dma_bm_eth/Common/Inc/shared_memory.h`, `Common/Src/shared_memory.c` | H1 |
| `prj_04/CM7/Core/Src/main.c`, `prj_04/CM4/Core/Src/main.c` | H1 (instalación), H4 (sonda de `selftest-uart`) |
| `prj_04/CM4/App/Src/adc_acquisition.c` | H2 |
| `prj_04/CM4/App/Src/dma_selftest.c` | H4 |
| `prj_04/CM7/App/Src/gpio_selftest.c`, `prj_04/CM4/App/Src/gpio_selftest.c` | H5, solo las líneas que dejan `NULL` |
| `01_shared_drivers/04_uart/uart_hal.c` y, en `01_shared_drivers/CMakeLists.txt`, solo `stm32h7_driver_uart` | H3, si la decisión 1 es A |
| `tools/gpio_f1b_checks.py` | Nuevo: T2, T3, T4 y T6 |
| `prj_04/docs/gpio_f1b_*` | Las capturas de la fase |
| `01_shared_drivers/00_gpio/IMPLEMENTATION_REPORT.md` | Tu informe |
| `00_gpio/README.md`, `04_uart/README.md`, `prj_04/README.md` | Solo en el cierre, cuando el auditor lo pida |

Todo lo demás está fuera (A23: `git diff 07db657 --stat`).

### 2.2 Prohibido

| Archivo o acción | Por qué |
| --- | --- |
| Fuentes de `00_gpio` (`gpio_bm.[ch]`, `gpio_ll.[ch]`, `gpio_hal.[ch]`) | El driver ya hace lo que hace falta. Si crees que necesita un cambio (p. ej. un getter del hook), para y pregunta |
| Cualquier firma de `gpio_*.h` o `uart_hal.h` | `01_shared_drivers/README.md` regla 2 |
| Otros proyectos (`prj_01`, `prj_02`, `prj_03`, `prj_eth_lwip_node`) y sus `board_def.h` | Fuera de alcance; G1 en `prj_03` es la decisión 5 |
| Otros drivers (`01_led`, `02_dma`, `07_adc`, `08_eth`, `10_hsem`…) | Cerrados: los usas, no los cambias. Si encuentras un defecto, apúntalo y avisa |
| La lógica de los selftests (umbrales, retardos, vectores) | H4 y H5 solo cambian cómo se configuran los pines |
| Campos existentes de `SharedMemory_t` (orden, tipo, tamaño) | Otros los leen por desplazamiento: solo añades al final |
| `DRIVER_ROADMAP.md`, el plan, estas instrucciones, `AUDIT.md` | El plan es la norma; sus erratas van a tu informe §5.7 |
| Flashear un preset de selftest de `prj_04` | G-S1: `selftest-gpio` mueve líneas del banco |
| Cambiar la configuración de red del PC | La configura el responsable (G-S4) |
| Capturas ya commiteadas | No se editan nunca |
| Merge, borrar la rama, reescribir historia, borrar documentos de proceso | Después del cierre, y no los haces tú |

---

## 3. Cómo trabajar

### 3.1 Rama y commits

- Rama **`driver/gpio-f1b`** (desde `main` en `07db657`). Un commit por preocupación: `fix(prj04)`, `fix(uart)`, `build(uart)`, `feat(tools)`, `docs(gpio)`; asunto en inglés e imperativo; el porqué en el cuerpo; el ID (H1…H5, G1, G13) entre paréntesis.
- Mensajes desde bash con `git commit -F - <<'EOF' … EOF` (PowerShell mete un BOM).

### 3.2 Decisiones abiertas

Opción por defecto de cada una (plan, decisiones 1–6). Si crees que otra es mejor, **para y pregunta** antes; en el informe di cuál usaste y, si alguien te aprobó otra, quién y cuándo.

### 3.3 Entorno

- `.\tools\env.ps1` antes de cualquier build (no hay `cmake` suelto). Matriz: `.\tools\build_all.ps1 -Clean`; solo cubre debug y release de cuatro proyectos: los otros presets de `prj_04` se compilan aparte (`cmake --preset <p>` y `cmake --build --preset <p>` en `prj_04`). `env.ps1` pisa variables de PowerShell como `$R` o `$root`: usa nombres propios.
- `py`, no `python`; solo biblioteca estándar.
- **Dos ST-LINK conectados:** `sn=0016000C4142501220353451` (la H755) en cada llamada a `STM32_Programmer_CLI` y a `tools/flash.py --sn`, y `tools/monitor.py --sn`.
- **El halt por SWD no se mantiene entre llamadas** al programador. T3 lee sin parar los núcleos (`mode=HOTPLUG`) y no usa el depurador.
- **Antes de escribir T3, comprueba que el probe lee la DTCM** (`0x2000_0000…`) con `mode=HOTPLUG`: si `s_shared_lock` de CM7 queda allí y no se puede leer, para y pregunta.
- **Documentos:**
  - `00_gpio/README.md` §3, §4, §10, §12.2 y §13.1;
  - `gpio_bm.h` y `gpio_hal.h`;
  - el hook de referencia en `03_firmware_templates/prj_01_template_dualcore/Common/` y sus dos `main.c`;
  - `04_uart/README.md` §3;
  - `10_hsem/README.md` §3.5;
  - RM0399 en `00_docs_stm32h7/docs/`.

### 3.4 Capturas

1. Una sola ejecución por captura, sin editar, con el comando, la fecha, el hash del árbol limpio y, en las de placa, el `Rev` que imprime `prj_04` (y el build id de `prj_01` en T7). La salida de cada herramienta tal cual; los juicios, en el informe.
2. Nombre: `prj_04/docs/gpio_f1b_<prueba>_<hash>.txt` (por ejemplo `gpio_f1b_t3_swd_1a2b3c4.txt`); desarrollo: `gpio_f1b_dev_…`. Se commitean todas, también las que fallen.
3. Cada número del informe sale de una captura o de un `.map` del repo.

### 3.5 Pruebas

- Cada prueba con su control negativo, que tiene que poder fallar de verdad: pasa el caso alterado por la misma comparación que el caso bueno y exige la diferencia.
- El oráculo nunca es el código que se prueba: el enlazador, el preprocesador, los registros por SWD frente a una tabla escrita a mano y a las direcciones de `nm`, `net_check.py`, `query_adc.py`, los contadores.
- `tools/gpio_f1b_checks.py`:
  - T2 lee las flags de cada `compile_commands.json` y preprocesa en un directorio de scratch, nunca en el proyecto;
  - la lista de excepciones de T2b es la del plan, escrita en la herramienta y en su salida;
  - el control de T2 y T3 es una compilación de `07db657` en un worktree o directorio aparte, nunca en tu árbol.
- T5 es `monitor.py`, `net_check.py` y `query_adc.py --json` al mismo tiempo, y solo cuenta si los tres extremos coinciden.
- T6a: los 20 resets por SWD los hace la herramienta, con los lectores de consola y red parados (G-S5). T6b (arranque en frío) queda pendiente mientras el responsable esté en remoto: no la pidas, apúntala en tu informe §5.8.

### 3.6 Seguridad en la placa

- **G-S1 antes del primer flasheo de `prj_04`:** lista cada pin que configuran sus dos núcleos en debug y release y comprueba que ninguno es una línea del banco: AD2 en PE2–PE15 y PA8; J1 en PA5, PA6, PB4, PB5; bus I2C en PB8, PB9; tomas en PF14, PF15. Pon la lista en el informe. Si alguno coincide, para y pregunta.
- **G-S3:** el AD2 en estado seguro: no corras ningún script del AD2.
- **G-S5:** flashear o resetear solo con `monitor.py`, `net_check.py` y `query_adc.py` parados.
- **El responsable está en remoto (G-S7):** nadie puede apagar la placa, pulsar RESET, reconectar un USB ni cambiar un cable. No pidas confirmaciones físicas. Antes de la primera sesión, comprueba la red con `net_check.py` sobre la imagen que haya en la placa (`prj_eth_lwip_node` en `07db657`, en `192.168.1.150`); si no contesta, para y avisa.
- **Nada que solo se recupere a mano:** no toques la configuración de alimentación ni `SystemClock_Config`, ni los option bytes (`flash.py --ob`), ni el firmware del ST-LINK, y no borres la flash entera. Para resetear, `STM32_Programmer_CLI … -rst` o `-hardRst` (NRST del ST-LINK); para conectar a una placa que no responde, `mode=UR`. Si ni así responde por SWD, para y avisa: no insistas.

---

## 4. Qué entregar al terminar F1

1. El rango de commits en `driver/gpio-f1b`, con el árbol limpio.
2. T1: `build_all -Clean` (8/8), los siete presets de `prj_04`, `-Werror` con 0 avisos donde dice el plan, y los `.text` de `uart_hal.c.obj` y `adc_acquisition.c.obj` antes (`07db657`) y después.
3. Las capturas de T2–T7, commiteadas.
4. El informe `00_gpio/IMPLEMENTATION_REPORT.md` con la sección F1.
5. Un aviso al auditor con el hash del último commit: "F1 entregada para auditoría".

## 5. El informe

Como el de `08_eth` (recupéralo con `git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/08_eth/IMPLEMENTATION_REPORT.md)^:01_shared_drivers/08_eth/IMPLEMENTATION_REPORT.md`): hechos y no valoraciones, cada número con su fuente, lo que falló tan visible como lo que salió bien.

Secciones:
- 5.1 commits;
- 5.2 builds y tamaños;
- 5.3 pruebas, con el oráculo y el control de cada una;
- 5.4 la lista de escritores de GPIO de `prj_04` antes y después (núcleo, archivo, puerto, antes o después de liberar a CPU2) y la de pines de G-S1;
- 5.5 desviaciones, con quién se consultó y cuándo;
- 5.6 respuesta a la auditoría;
- 5.7 erratas del plan;
- 5.8 lo abierto y lo no ejercitado.

## 6. Cuándo parar y preguntar

- una prueba no se puede hacer como dice el plan;
- un pin de `prj_04` coincide con una línea del banco (G-S1);
- una corrección parece pedir una firma nueva o un cambio en las fuentes de `00_gpio`;
- `s_shared_lock` de un núcleo no se puede leer por SWD;
- T4 da menos de 10× con 60 µs;
- la placa no responde por SWD ni bajo reset, o la red deja de contestar;
- encuentras otro escritor de configuración de GPIO que el plan no lista;
- quieres una opción distinta de la de por defecto.

Apunta la pregunta en §5.5 o §5.7 del informe.
