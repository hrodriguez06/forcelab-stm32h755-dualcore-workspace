# Instrucciones para el implementador de `08_eth` (E1, E2, E5, E8)

Oct 11, 2026 · @Hector

Vas a implementar [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1) de esta carpeta: cerrar cuatro limitaciones del driver de Ethernet (E1, E2, E5, E8 de `08_eth/README.md` §10) sin cambiar su API, y validarlo en la placa a través de `prj_02_eth_lwip_dualcore`. Otra persona audita la fase y repite tus pruebas en la placa.

Si algo de aquí choca con el plan, manda el plan, salvo lo que este documento **prohíbe**, que sigue prohibido.

---

## 1. Tu papel

**Tú implementas y pruebas. No auditas.**

- No escribas "auditado", "aprobado", "cerrado" ni nada parecido sobre tu trabajo; lo más es **"entregado para auditoría"**.
- No crees, edites ni borres `08_eth/AUDIT.md`. Si no estás de acuerdo con un hallazgo, contéstalo en tu informe.
- No marques un hallazgo como cerrado: di "corregido en `<hash>`, pendiente de verificación".
- No empieces el cierre hasta que `AUDIT.md` diga que F1 pasa.

---

## 2. Qué puedes tocar y qué no

### 2.1 Lo único que puedes modificar

| Ruta | Qué |
| --- | --- |
| `01_shared_drivers/08_eth/eth_hal.c` | E1, E2, E5, E8 |
| `01_shared_drivers/08_eth/eth_hal.h`, `lan8742_phy.h` | Solo comentarios |
| `01_shared_drivers/CMakeLists.txt` | Solo `stm32h7_driver_eth` (el enlace a `stm32h7_driver_gpio_hal`) |
| `01_shared_drivers/08_eth/IMPLEMENTATION_REPORT.md` | Tu informe |
| `01_shared_drivers/08_eth/docs/**` | Las capturas de la fase |
| `tools/eth_build_checks.py` | Nuevo: las comprobaciones de compilación T2–T4 |
| `01_shared_drivers/08_eth/README.md` | Solo en el cierre, cuando el auditor lo pida |

Todo lo demás está fuera (A23: `git diff 7154af6 --stat`).

### 2.2 Prohibido

| Archivo o acción | Por qué |
| --- | --- |
| `02_firmware_projects/**` y `03_firmware_templates/**` (código, `board_def.h`, `stm32h7xx_hal_conf.h`, linker scripts, README) | El encargo es el driver; los proyectos son sus consumidores. El registro en `prj_02/README.md` §6 lo hace el responsable en el cierre (decisión 1) |
| Cambiar una firma de `eth_hal.h` o `lan8742_phy.h`, o un campo de `eth_driver_stats_t` | `08_eth/README.md` §11, regla 1 |
| Fuentes y README de otros drivers (`00_gpio`, `02_dma`, `04_uart`, `05_i2c`, `06_spi`, `10_hsem`…) | Drivers cerrados: los usas, no los cambias. Si encuentras un defecto en uno, apúntalo y avisa |
| `DRIVER_ROADMAP.md`, el plan, estas instrucciones, `AUDIT.md` | El plan es la norma; sus erratas van a tu informe §5.7 |
| Cambiar la configuración de red del PC | La configura el responsable (E-S4) |
| Capturas ya commiteadas | No se editan nunca |
| Merge, borrar la rama, reescribir historia, borrar documentos de proceso | Después del cierre, y no los haces tú |

---

## 3. Cómo trabajar

### 3.1 Rama y commits

- Rama **`driver/eth`** (desde `main` en `7154af6`). Un commit por preocupación: `fix(eth)`, `build(eth)`, `feat(tools)`, `docs(eth)`; asunto en inglés e imperativo; el porqué en el cuerpo; el ID de la limitación o del hallazgo entre paréntesis.
- Mensajes desde bash con `git commit -F - <<'EOF' … EOF` (PowerShell mete un BOM).

### 3.2 Decisiones abiertas

Opción por defecto de cada una (plan, decisiones 1–5). Si crees que otra es mejor, **para y pregunta** antes; en el informe di cuál usaste y, si alguien te aprobó otra, quién y cuándo.

### 3.3 Entorno

- `.\tools\env.ps1` antes de cualquier build (no hay `cmake` suelto). Matriz: `.\tools\build_all.ps1 -Clean`. `env.ps1` pisa variables de PowerShell como `$R` o `$root`: usa nombres propios.
- `py`, no `python`; solo biblioteca estándar.
- **Dos ST-LINK conectados** (la H755 y la F334 de `05_i2c`): `sn=` en cada llamada al programador (`tools/flash.py --sn`), `tools/monitor.py --sn`. La H755 es `0016000C4142501220353451`. O pide al responsable que desconecte el USB de la F334.
- **El halt por SWD no se mantiene entre llamadas** a `STM32_Programmer_CLI`: si T5 necesita los núcleos parados, halt, lecturas y `-run` en una sola llamada. Mejor: lee los registros sin parar (la configuración de los pines no cambia tras el arranque) y dilo.
- **Documentos:** `08_eth/README.md`, `00_gpio/README.md` §3.3 y §4, `gpio_hal.h`; la HAL de ETH y el `hal_conf` de cada proyecto en `02_firmware_projects/…/CM7/Core/Inc/`; RM0399 y ES0445 en `00_docs_stm32h7/docs/`.

### 3.4 Capturas

1. Una sola ejecución por captura, sin editar, con el comando, la fecha, el hash del árbol limpio y, en las de placa, el Rev y el id de la imagen que imprime `prj_02` (si no imprime id, el hash con que se compiló y se flasheó).
2. Nombre: `08_eth/docs/eth_f1_<prueba>_<hash>.txt` (por ejemplo `eth_f1_t6_netcheck_1a2b3c4.txt`); desarrollo: `eth_dev_…`. Se commitean todas, también las que fallen.
3. Cada número del informe sale de una captura o de un `.map` del repo.

### 3.5 Pruebas

- Cada prueba con su control negativo, que tiene que poder fallar de verdad: pasa el caso alterado por la misma comparación que el caso bueno y exige la diferencia.
- El oráculo nunca es el driver: el compilador, el desensamblado, los registros leídos por SWD frente a una tabla escrita a mano, `net_check.py`, los contadores.
- T2–T4 en `tools/eth_build_checks.py`, que toma las flags del `compile_commands.json` de `prj_02` (CM7), escribe las variantes de `board_def.h` y de `stm32h7xx_hal_conf.h` en un directorio de scratch (nunca en el proyecto) y compila solo `eth_hal.c`.
- T6 es la de `08_eth/README.md` §9.3: `net_check.py` y `monitor.py` al mismo tiempo, y solo cuenta si los dos extremos coinciden.

### 3.6 Seguridad en la placa

- **E-S1 antes del primer flasheo de `prj_02`:** lista cada pin que configuran sus dos núcleos y comprueba que ninguno es una línea del banco (AD2: PE2–PE15 y PA8; J1: PA5, PA6, PB4, PB5; bus I2C: PB8, PB9; tomas: PF14, PF15). Pon la lista en el informe. Si alguno coincide, para y pregunta.
- **E-S2:** dos ST-LINK, `sn=` siempre.
- **E-S3:** el AD2 en estado seguro: no corras ningún script del AD2.
- **E-S5:** flashear o resetear solo con `net_check.py` y `monitor.py` parados.
- Pide al responsable que confirme el cable Ethernet y la red antes de la primera sesión de T6.

---

## 4. Qué entregar al terminar F1

1. El rango de commits en `driver/eth`, con el árbol limpio.
2. `build_all -Clean` (8/8), los builds de `prj_02`, `prj_03`, `prj_04` con `-Werror` y 0 avisos, `prj_01` con los tamaños de `2481396`.
3. El `.text`/`.data`/`.bss` de `eth_hal.c.obj` en `prj_02/03/04`, antes (`7154af6`) y después.
4. Las capturas de T1–T6, commiteadas.
5. El informe `08_eth/IMPLEMENTATION_REPORT.md` con la sección F1.
6. Un aviso al auditor con el hash del último commit: "F1 entregada para auditoría".

## 5. El informe

Como el de `05_i2c` (recupéralo con `git show $(git log -1 --format=%h --diff-filter=D -- 01_shared_drivers/05_i2c/IMPLEMENTATION_REPORT.md)^:01_shared_drivers/05_i2c/IMPLEMENTATION_REPORT.md`): hechos y no valoraciones, cada número con su fuente, lo que falló tan visible como lo que salió bien. Secciones: 5.1 commits, 5.2 builds y tamaños, 5.3 pruebas (con oráculo y control de cada una), 5.4 la lista de pines de E-S1, 5.5 desviaciones (con quién se consultó y cuándo), 5.6 respuesta a la auditoría, 5.7 erratas del plan, 5.8 abierto y no ejercitado.

## 6. Cuándo parar y preguntar

- una prueba no se puede hacer como dice el plan;
- un pin de `prj_02` coincide con una línea del banco (E-S1);
- una corrección parece pedir una firma nueva o un cambio en `02_firmware_projects`;
- un consumidor deja de compilar con E1 o E2;
- `GPIO_HAL_Init` no puede configurar un pin como lo hacía `HAL_GPIO_Init`;
- quieres una opción distinta de la de por defecto.

Apunta la pregunta en §5.5 o §5.7 del informe.
