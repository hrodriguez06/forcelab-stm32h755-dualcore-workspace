# Instrucciones para el implementador de `08_eth` E3–E7

Oct 16, 2026 · @Hector

Vas a implementar el plan [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) (revisión 1) de esta carpeta:
- el cero-copia de recepción en `prj_02` (E6);
- `ETH_HAL_CUSTOM_CALLBACKS` (E3);
- la herramienta `tools/eth_e3e6_checks.py`.

**Llevas la carga de trabajo:**
- todas las pruebas sin placa;
- la herramienta completa, incluida la parte de placa (T5–T6), probada sin placa hasta donde se pueda;
- un informe que el auditor pueda comprobar rápido.

Si algo de este documento choca con el plan, manda el plan, salvo las prohibiciones de 2.2.

---

## 1. Tu papel

- Implementas y pruebas sin placa. El estado de tu entrega es "entregado para auditoría".
- No tocas `AUDIT.md`.
- No afirmas nada que no demuestre una captura commiteada o un `archivo:línea` que hayas comprobado contra HEAD.
- T5 y T6 quedan NO VERIFICADAS hasta que las ejecute el auditor.

---

## 2. Qué puedes tocar

### 2.1 Solo esto

| Ruta | Propósito |
| --- | --- |
| `02_firmware_projects/prj_02_eth_lwip_dualcore/CM7/App/Src/ethernetif.c`, `CM7/App/Inc/lwipopts.h`, y el `CMakeLists.txt` o `board_def.h` de `prj_02` solo para definir `ETH_HAL_RX_ZERO_COPY` y el tamaño del pool | H1 |
| `01_shared_drivers/08_eth/eth_hal.c`, `eth_hal.h` | H2 y H3: solo añadir, y solo detrás de macros que `prj_03`/`prj_04` no definen |
| `tools/eth_e3e6_checks.py` (nuevo) | H5 |
| `01_shared_drivers/08_eth/docs/eth_e3e6_*` | Tus capturas |
| `01_shared_drivers/08_eth/IMPLEMENTATION_REPORT.md` | H6 |
| `08_eth/README.md`, `prj_02/README.md`, `prj_02/docs/ETHERNET_BRINGUP.md` | Solo en el cierre, cuando el auditor lo autorice |

### 2.2 Prohibido

| Qué | Por qué |
| --- | --- |
| Cambiar cualquier cosa de `prj_03`, `prj_04` o `prj_eth_lwip_node`, o que cambie sus objetos compilados | Solo se prueba en la placa `prj_02` (T2 lo comprueba) |
| Cambiar o borrar declaraciones existentes de `eth_hal.h` o `lan8742_phy.h` | API congelada |
| La MPU, los scripts de enlazado, `SystemClock_Config`, la alimentación, los option bytes | Fuera de alcance; si SRAM3 no alcanza, para y pregunta |
| Programar, resetear o leer la placa | T5–T6 las ejecuta el auditor con tu herramienta |
| Escribir en el árbol durante una prueba, también archivos ignorados (`.su`, `.o`, logs) | Todo va a un directorio temporal fuera del repo, y el `cwd` de los compiladores también |
| Editar capturas commiteadas, hacer merge, borrar ramas o documentos de proceso | Integridad y papel del responsable |

---

## 3. Cómo trabajar

- **Rama:** `driver/eth-e3-e6`, desde `main` en `69179d7` (control). La herramienta crea el worktree del control y lo compila dentro de su propia ejecución.
- **Commits:** uno por objetivo, en inglés e imperativo, desde bash con heredoc.
- **Entorno:** `.\tools\env.ps1`; `py`, nunca `python`; solo la biblioteca estándar.
- **Herramienta** (`--test {t1..t6,all}`, `--output`):
  - comprueba el árbol limpio, sin archivos nuevos ignorados, al empezar y al terminar;
  - comprueba que el control esté en `69179d7` y limpio;
  - escribe la captura línea a línea;
  - tiene límite de tiempo en cada subproceso;
  - **un error de herramienta, un archivo que falta o una salida vacía es FAIL;**
  - **cada control negativo pasa por la misma función que la comprobación real, con una entrada mutada temporal.**
- **T5–T6 en la herramienta:**
  - flashea con el `tools/flash.py` de `prj_02` (HEAD y control) y `--sn 0016000C4142501220353451`;
  - usa `net_check.py` y `monitor.py` como en `00_gpio`/`03_tim` (el monitor antes del flasheo solo si la prueba lo necesita);
  - la carga UDP la mide el host: datagramas enviados, recibidos y comparados byte a byte, y Mbit/s;
  - T6 usa `STM32_Programmer_CLI -c port=SWD sn=… mode=HOTPLUG -rst`;
  - nada más contra la placa.
- **Decisiones:** las que apruebe el responsable. Por defecto, 1A, 2A, 3A y 4.

---

## 4. Orden recomendado

1. H3 y su prueba (T3).
2. H1 y H2 en `prj_02`, y la compilación de la matriz.
3. H5: T1–T4 con sus controles negativos fallando como deben, y la parte de placa probada contra un servidor UDP de eco local en el PC, que solo usa `127.0.0.1` y no toca la placa.
4. Las capturas de T1–T4.
5. **H6, el informe:**
   - qué cambió, con `archivo:línea`;
   - los contextos desde los que se libera un búfer (Diseño 3);
   - el presupuesto de SRAM3 antes y después;
   - los resultados de T1–T4 con sus capturas;
   - el comando exacto de T5–T6 para el auditor;
   - lo no verificado.
6. **Entrega.** Árbol limpio. La última línea del informe es exactamente `F1 ENTREGADA PARA AUDITORIA <hash del último commit de código o herramienta>`.
